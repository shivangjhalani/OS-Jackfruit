/*
 * engine.c — OS-Jackfruit User-Space Runtime
 * Tasks 1, 2, 3 — Containers + CLI/IPC + Bounded-Buffer Logging
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <sched.h>
#include "monitor_ioctl.h"

/* ── Constants ── */
#define MAX_CONTAINERS    16
#define CONTAINER_ID_LEN  64
#define LOG_PATH_LEN      256
#define SOCKET_PATH       "/tmp/jackfruit.sock"
#define LOG_DIR           "./logs"
#define MONITOR_DEV       "/dev/container_monitor"

/* Bounded buffer */
#define LOG_BUF_SLOTS     64
#define LOG_ENTRY_SIZE    512

/* IPC message sizes */
#define MSG_PAYLOAD_SIZE  512
#define MSG_RESPONSE_SIZE 2048

/* ── Container state ── */
typedef enum {
    STATE_STARTING = 0,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED
} ContainerState;

static const char *STATE_NAMES[] = {
    "starting", "running", "stopped", "killed"
};

/* ── Per-container metadata ── */
typedef struct {
    char            id[CONTAINER_ID_LEN];
    pid_t           host_pid;
    struct timespec start_time;
    ContainerState  state;
    int             soft_limit_mb;
    int             hard_limit_mb;
    char            log_path[LOG_PATH_LEN];
    int             exit_status;
    int             exit_signal;
    int             pipe_read_fd;
    int             active;
} Container;

/* ── IPC protocol ── */
#define CMD_START   1
#define CMD_RUN     2
#define CMD_PS      3
#define CMD_LOGS    4
#define CMD_STOP    5

typedef struct {
    int  command;
    char arg1[CONTAINER_ID_LEN];
    char arg2[LOG_PATH_LEN];
    char arg3[MSG_PAYLOAD_SIZE];
} IpcRequest;

typedef struct {
    int  status;
    char message[MSG_RESPONSE_SIZE];
} IpcResponse;

/* ── Bounded buffer (Task 3) ── */
typedef struct {
    char data[LOG_ENTRY_SIZE];
    char log_path[LOG_PATH_LEN];
    int  len;
} LogEntry;

typedef struct {
    LogEntry          slots[LOG_BUF_SLOTS];
    int               head;
    int               tail;
    int               count;
    pthread_mutex_t   mutex;
    pthread_cond_t    not_full;
    pthread_cond_t    not_empty;
    int               shutdown;
} BoundedBuffer;

/* ── Globals ── */
static Container    containers[MAX_CONTAINERS];
static int          container_count = 0;
static pthread_mutex_t containers_mutex = PTHREAD_MUTEX_INITIALIZER;

static BoundedBuffer log_buf;
static pthread_t     consumer_tid;
static int           monitor_fd = -1;   /* /dev/container_monitor */

static volatile sig_atomic_t supervisor_running = 1;

/* ══════════════════════════════════════════════
 * BOUNDED BUFFER (Task 3)
 * ══════════════════════════════════════════════ */

static void bb_init(BoundedBuffer *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex,     NULL);
    pthread_cond_init(&b->not_full,   NULL);
    pthread_cond_init(&b->not_empty,  NULL);
    b->shutdown = 0;
}

/* Producer: called from pipe-reader threads */
static void bb_push(BoundedBuffer *b, const char *data, int len,
                    const char *log_path)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUF_SLOTS && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (!b->shutdown) {
        LogEntry *e = &b->slots[b->tail];
        e->len = len < LOG_ENTRY_SIZE - 1 ? len : LOG_ENTRY_SIZE - 1;
        memcpy(e->data, data, e->len);
        e->data[e->len] = '\0';
        strncpy(e->log_path, log_path, LOG_PATH_LEN - 1);
        b->tail  = (b->tail + 1) % LOG_BUF_SLOTS;
        b->count++;
        pthread_cond_signal(&b->not_empty);
    }
    pthread_mutex_unlock(&b->mutex);
}

/* Consumer thread: write entries to log files */
static void *log_consumer(void *arg) {
    (void)arg;
    BoundedBuffer *b = &log_buf;

    while (1) {
        pthread_mutex_lock(&b->mutex);
        while (b->count == 0 && !b->shutdown)
            pthread_cond_wait(&b->not_empty, &b->mutex);

        if (b->count == 0 && b->shutdown) {
            pthread_mutex_unlock(&b->mutex);
            break;
        }

        LogEntry e = b->slots[b->head];
        b->head  = (b->head + 1) % LOG_BUF_SLOTS;
        b->count--;
        pthread_cond_signal(&b->not_full);
        pthread_mutex_unlock(&b->mutex);

        /* Write to log file */
        FILE *f = fopen(e.log_path, "a");
        if (f) {
            fwrite(e.data, 1, e.len, f);
            fclose(f);
        }
    }
    return NULL;
}

/* Per-container pipe reader thread */
typedef struct { int fd; char log_path[LOG_PATH_LEN]; } PipeReaderArg;

static void *pipe_reader(void *arg) {
    PipeReaderArg *pra = (PipeReaderArg *)arg;
    char buf[LOG_ENTRY_SIZE];
    ssize_t n;

    while ((n = read(pra->fd, buf, sizeof(buf) - 1)) > 0)
        bb_push(&log_buf, buf, (int)n, pra->log_path);

    close(pra->fd);
    free(pra);
    return NULL;
}

/* ══════════════════════════════════════════════
 * CONTAINER TABLE HELPERS
 * ══════════════════════════════════════════════ */

static int find_free_slot(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!containers[i].active) return i;
    return -1;
}

static int find_running_container(const char *id) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].active &&
            containers[i].state == STATE_RUNNING &&
            strcmp(containers[i].id, id) == 0)
            return i;
    return -1;
}

static int find_container_by_id(const char *id) {
    /* Prefer running, fall back to any active */
    int fallback = -1;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].active) continue;
        if (strcmp(containers[i].id, id) != 0) continue;
        if (containers[i].state == STATE_RUNNING) return i;
        fallback = i;
    }
    return fallback;
}

/* ══════════════════════════════════════════════
 * SIGNAL HANDLERS
 * ══════════════════════════════════════════════ */

static void sigchld_handler(int sig) {
    (void)sig;
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].active) continue;
            if (containers[i].host_pid != pid) continue;
            if (WIFEXITED(status)) {
                containers[i].state       = STATE_STOPPED;
                containers[i].exit_status = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                containers[i].state       = STATE_KILLED;
                containers[i].exit_signal = WTERMSIG(status);
            }
            containers[i].active = 0;  // ← FREE the parking spot
            container_count--;         // ← keep the count accurate
            break;
        }
        pthread_mutex_unlock(&containers_mutex);
    }
}

static void shutdown_handler(int sig) {
    (void)sig;
    printf("\n[engine] shutdown signal — cleaning up...\n");
    supervisor_running = 0;
}

/* ══════════════════════════════════════════════
 * CONTAINER LAUNCH (Task 1)
 * ══════════════════════════════════════════════ */

int container_start(const char *id,
                    const char *rootfs,
                    char *const argv[])
{
    pthread_mutex_lock(&containers_mutex);

    int slot = find_free_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&containers_mutex);
        fprintf(stderr, "[engine] ERROR: container table full\n");
        return -1;
    }

    /* Block duplicate running containers */
    if (find_running_container(id) >= 0) {
        pthread_mutex_unlock(&containers_mutex);
        fprintf(stderr, "[engine] ERROR: container '%s' already running\n", id);
        return -1;
    }

    mkdir(LOG_DIR, 0755);

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        pthread_mutex_unlock(&containers_mutex);
        perror("[engine] pipe"); return -1;
    }

    Container *c = &containers[slot];
    memset(c, 0, sizeof(Container));
    strncpy(c->id, id, CONTAINER_ID_LEN - 1);
    clock_gettime(CLOCK_REALTIME, &c->start_time);
    c->state         = STATE_STARTING;
    c->soft_limit_mb = 50;
    c->hard_limit_mb = 100;
    c->pipe_read_fd  = pipefd[0];
    c->active        = 1;
    snprintf(c->log_path, LOG_PATH_LEN, "%s/%s.log", LOG_DIR, id);
    container_count++;

    pthread_mutex_unlock(&containers_mutex);

    pid_t pid = fork();
    if (pid < 0) {
        perror("[engine] fork");
        c->active = 0; container_count--;
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    /* ── CHILD: becomes the container ── */
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        if (unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS) < 0) {
            perror("[container] unshare"); exit(EXIT_FAILURE);
        }

        sethostname(id, strlen(id));

        char proc_path[512];
        snprintf(proc_path, sizeof(proc_path), "%s/proc", rootfs);
        mkdir(proc_path, 0555);
        mount("proc", proc_path, "proc",
              MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL);

        if (chroot(rootfs) < 0) {
            perror("[container] chroot"); exit(EXIT_FAILURE);
        }
        chdir("/");

        char *sh_argv[] = { "/bin/sh", "-c", argv[0], NULL };
        execv("/bin/sh", (char *const *)sh_argv);
        perror("[container] execv");
        exit(EXIT_FAILURE);
    }

    /* ── PARENT: save metadata ── */
    close(pipefd[1]);

    pthread_mutex_lock(&containers_mutex);
    c->host_pid = pid;
    c->state    = STATE_RUNNING;
    pthread_mutex_unlock(&containers_mutex);

    /* Start pipe reader thread (producer) */
    PipeReaderArg *pra = malloc(sizeof(*pra));
    pra->fd = pipefd[0];
    strncpy(pra->log_path, c->log_path, LOG_PATH_LEN - 1);
    pthread_t reader_tid;
    pthread_create(&reader_tid, NULL, pipe_reader, pra);
    pthread_detach(reader_tid);

    /* Register with kernel monitor */
    if (monitor_fd >= 0) {
        MonitorEntry me;
        memset(&me, 0, sizeof(me));
        me.pid           = pid;
        me.soft_limit_mb = c->soft_limit_mb;
        me.hard_limit_mb = c->hard_limit_mb;
        strncpy(me.name, id, 63);
        if (ioctl(monitor_fd, IOCTL_REGISTER_PID, &me) < 0)
            perror("[engine] ioctl REGISTER_PID");
    }

    printf("[engine] container '%s' started with PID %d\n", id, pid);
    return pid;
}

/* ══════════════════════════════════════════════
 * SUPERVISOR SOCKET
 * ══════════════════════════════════════════════ */

static int supervisor_socket_init(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("[engine] socket"); return -1; }

    unlink(SOCKET_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[engine] bind"); close(fd); return -1;
    }
    if (listen(fd, 8) < 0) {
        perror("[engine] listen"); close(fd); return -1;
    }

    printf("[engine] control socket ready at %s\n", SOCKET_PATH);
    return fd;
}

/* ── ps table ── */
static void format_ps(char *buf, size_t sz) {
    int w = snprintf(buf, sz,
        "%-16s %-8s %-10s %-10s %-10s %s\n"
        "%-16s %-8s %-10s %-10s %-10s %s\n",
        "NAME","PID","STATE","SOFT(MB)","HARD(MB)","LOG",
        "────────────────","────────","──────────",
        "──────────","──────────","───────────────");

    pthread_mutex_lock(&containers_mutex);
    int found = 0;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].active) continue;
        found = 1;
        w += snprintf(buf + w, sz - w,
            "%-16s %-8d %-10s %-10d %-10d %s\n",
            containers[i].id,
            containers[i].host_pid,
            STATE_NAMES[containers[i].state],
            containers[i].soft_limit_mb,
            containers[i].hard_limit_mb,
            containers[i].log_path);
    }
    if (!found)
        snprintf(buf + w, sz - w, "(no containers)\n");
    pthread_mutex_unlock(&containers_mutex);
}

/* ── handle one CLI connection ── */
static void handle_client(int cfd) {
    IpcRequest  req;
    IpcResponse resp;
    memset(&resp, 0, sizeof(resp));

    if (recv(cfd, &req, sizeof(req), MSG_WAITALL) != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "ERROR: bad request");
        send(cfd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.command) {

    case CMD_START: {
        char *cmd[] = { req.arg3, NULL };
        int pid = container_start(req.arg1, req.arg2, cmd);
        if (pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: failed to start '%s'", req.arg1);
        } else {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "OK: container '%s' started, PID %d", req.arg1, pid);
        }
        break;
    }

    case CMD_RUN: {
    char *cmd[] = { req.arg3, NULL };
    int pid = container_start(req.arg1, req.arg2, cmd);
    if (pid < 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "ERROR: failed to run '%s'", req.arg1);
    } else {
        int st;
        pid_t r;
        do {
            r = waitpid(pid, &st, WNOHANG); // check without blocking
            if (r == 0) usleep(200000);     // not done yet → sleep 200ms
        } while (r == 0);                   // keep looping until done
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "OK: container '%s' finished", req.arg1);
    }
    break;
}

    case CMD_PS:
        resp.status = 0;
        format_ps(resp.message, sizeof(resp.message));
        break;

    case CMD_LOGS: {
        pthread_mutex_lock(&containers_mutex);
        int idx = find_container_by_id(req.arg1);
        char lp[LOG_PATH_LEN] = {0};
        if (idx >= 0) strncpy(lp, containers[idx].log_path, LOG_PATH_LEN-1);
        pthread_mutex_unlock(&containers_mutex);

        if (idx < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: container '%s' not found", req.arg1);
            break;
        }
        FILE *f = fopen(lp, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: log empty or not found: %s", lp);
            break;
        }
        resp.status = 0;
        size_t n = fread(resp.message, 1, sizeof(resp.message)-1, f);
        resp.message[n] = '\0';
        fclose(f);
        break;
    }

    case CMD_STOP: {
        /* ── FIXED: search specifically for RUNNING entry ── */
        pid_t target_pid = -1;
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].active) continue;
            if (strcmp(containers[i].id, req.arg1) != 0) continue;
            if (containers[i].state != STATE_RUNNING) continue;
            target_pid = containers[i].host_pid;
            break;
        }
        pthread_mutex_unlock(&containers_mutex);

        if (target_pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: container '%s' not found or not running",
                     req.arg1);
            break;
        }

        /* Graceful: SIGTERM → wait 2s → SIGKILL */
        kill(target_pid, SIGTERM);
        sleep(2);

        pthread_mutex_lock(&containers_mutex);
        int idx2 = find_running_container(req.arg1);
        if (idx2 >= 0) kill(target_pid, SIGKILL);
        pthread_mutex_unlock(&containers_mutex);

        /* Unregister from kernel monitor */
        if (monitor_fd >= 0)
            ioctl(monitor_fd, IOCTL_UNREGISTER_PID, &target_pid);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "OK: container '%s' stopped", req.arg1);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "ERROR: unknown command");
        break;
    }

    send(cfd, &resp, sizeof(resp), 0);
}

/* ══════════════════════════════════════════════
 * SUPERVISOR MAIN LOOP
 * ══════════════════════════════════════════════ */

static void supervisor_run(const char *rootfs) {
    (void)rootfs;

    /* Signals */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = shutdown_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* Bounded buffer + consumer */
    bb_init(&log_buf);
    pthread_create(&consumer_tid, NULL, log_consumer, NULL);

    /* Open kernel monitor device */
    monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "[engine] WARNING: cannot open %s "
                "(is monitor.ko loaded?)\n", MONITOR_DEV);

    /* Control socket */
    int sfd = supervisor_socket_init();
    if (sfd < 0) exit(EXIT_FAILURE);

    printf("[engine] supervisor ready (PID %d)\n", getpid());

    while (supervisor_running) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(sfd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(sfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue;

        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; continue; }
        handle_client(cfd);
        close(cfd);
    }

    /* ── Orderly shutdown ── */
    printf("[engine] stopping all containers...\n");
    pthread_mutex_lock(&containers_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].active && containers[i].state == STATE_RUNNING) {
            printf("[engine] killing '%s' (PID %d)\n",
                   containers[i].id, containers[i].host_pid);
            kill(containers[i].host_pid, SIGKILL);
        }
    }
    pthread_mutex_unlock(&containers_mutex);
    sleep(1);

    /* Shutdown logger */
    pthread_mutex_lock(&log_buf.mutex);
    log_buf.shutdown = 1;
    pthread_cond_signal(&log_buf.not_empty);
    pthread_mutex_unlock(&log_buf.mutex);
    pthread_join(consumer_tid, NULL);

    if (monitor_fd >= 0) close(monitor_fd);
    close(sfd);
    unlink(SOCKET_PATH);
    printf("[engine] supervisor exited cleanly\n");
}

/* ══════════════════════════════════════════════
 * CLI CLIENT
 * ══════════════════════════════════════════════ */

static int cli_send(IpcRequest *req) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("[cli] socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
            "[cli] ERROR: cannot connect to supervisor\n"
            "      Run: sudo ./engine supervisor ./rootfs\n");
        close(fd); return -1;
    }

    send(fd, req, sizeof(*req), 0);

    IpcResponse resp;
    recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    printf("%s\n", resp.message);
    close(fd);
    return resp.status;
}

/* ══════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  sudo ./engine supervisor <rootfs>\n"
            "  sudo ./engine start <name> <rootfs> <cmd>\n"
            "  sudo ./engine run   <name> <rootfs> <cmd>\n"
            "  sudo ./engine ps\n"
            "  sudo ./engine logs  <name>\n"
            "  sudo ./engine stop  <name>\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "Need rootfs path\n"); return 1; }
        supervisor_run(argv[2]);
        return 0;
    }

    IpcRequest req;
    memset(&req, 0, sizeof(req));

    if      (strcmp(argv[1], "start") == 0) req.command = CMD_START;
    else if (strcmp(argv[1], "run")   == 0) req.command = CMD_RUN;
    else if (strcmp(argv[1], "ps")    == 0) req.command = CMD_PS;
    else if (strcmp(argv[1], "logs")  == 0) req.command = CMD_LOGS;
    else if (strcmp(argv[1], "stop")  == 0) req.command = CMD_STOP;
    else { fprintf(stderr, "Unknown command: %s\n", argv[1]); return 1; }

    if (argc > 2) strncpy(req.arg1, argv[2], CONTAINER_ID_LEN - 1);
    if (argc > 3) strncpy(req.arg2, argv[3], LOG_PATH_LEN     - 1);
    if (argc > 4) strncpy(req.arg3, argv[4], MSG_PAYLOAD_SIZE - 1);

    return cli_send(&req) == 0 ? 0 : 1;
}
