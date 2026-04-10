
#define _GNU_SOURCE
#include "monitor_ioctl.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ─────────────────────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────────────────────── */
#define STACK_SIZE          (1024 * 1024)
#define BUFFER_SIZE         16
#define MAX_LOG_LINE        512
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define CHILD_COMMAND_LEN   256
#define CONTROL_MESSAGE_LEN 1024
#define LOG_DIR             "logs"
#define CGROUP_BASE         "/sys/fs/cgroup"

typedef enum { CMD_SUPERVISOR, CMD_START, CMD_STOP, CMD_PS, CMD_LOGS } command_kind_t;
typedef enum { CONTAINER_RUNNING, CONTAINER_STOPPED, CONTAINER_KILLED, CONTAINER_EXITED } container_state_t;

/* ─────────────────────────────────────────────────────────────
 * Structs
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char msg[MAX_LOG_LINE];
} log_entry_t;

typedef struct {
    log_entry_t pool[BUFFER_SIZE];
    int head, tail, count, shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_full, not_empty;
} bounded_buffer_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    container_state_t state;
    int exit_code;
    int stop_requested;
    pthread_t producer_tid;
    int producer_done;
    struct container_record *next;
} container_record_t;

typedef struct {
    char id[CONTAINER_ID_LEN], rootfs[PATH_MAX], command[CHILD_COMMAND_LEN];
    int pipe_write_fd;
} child_config_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    int pipe_read_fd;
} producer_config_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN], rootfs[PATH_MAX], command[CHILD_COMMAND_LEN];
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    int server_fd;
    volatile int should_stop;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    bounded_buffer_t log_buffer;
    pthread_t consumer_tid;
} supervisor_ctx_t;

void check_container_health(supervisor_ctx_t *ctx) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) return;

    pthread_mutex_lock(&ctx->metadata_lock);
    for (container_record_t *r = ctx->containers; r; r = r->next) {
        if (r->state == CONTAINER_RUNNING) {
            struct health_response h_req = { .pid = r->host_pid };
            
            if (ioctl(fd, MONITOR_GET_HEALTH, &h_req) == 0) {
                if (h_req.health == HEALTH_WARN) {
                    // ACTION: Log a warning to the container's log file
                    char warn_msg[MAX_LOG_LINE];
                    snprintf(warn_msg, MAX_LOG_LINE, "[POLICY ADVISORY] Container %s is exceeding soft memory limits!\n", r->id);
                    bounded_buffer_push(&ctx->log_buffer, r->id, warn_msg);
                    
                    // Optional: You could also lower the process priority (niceness)
                    setpriority(PRIO_PROCESS, r->host_pid, 10);
                }
            }
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    close(fd);
}

static supervisor_ctx_t *g_ctx = NULL;

/* ─────────────────────────────────────────────────────────────
 * TASK 4: CGROUP HELPERS
 * ───────────────────────────────────────────────────────────── */
static void write_cgroup(const char *subsystem, const char *id, const char *file, const char *val) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/mini_runtime/%s/%s", CGROUP_BASE, subsystem, id, file);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, val, strlen(val)); close(fd); }
}

static void setup_cgroups(const char *id, pid_t pid) {
    char path[PATH_MAX], pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d", pid);

    // 1. Create the base runtime directory
    mkdir("/sys/fs/cgroup/mini_runtime", 0755);
    
    // 2. Create the container-specific directory
    snprintf(path, sizeof(path), "/sys/fs/cgroup/mini_runtime/%s", id);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        perror("mkdir cgroup");
        return;
    }

    // 3. Set Limits (v2 style)
    char file_path[PATH_MAX];
    
    // Memory Max: 64MB
    snprintf(file_path, sizeof(file_path), "%s/memory.max", path);
    int fd = open(file_path, O_WRONLY);
    if (fd >= 0) { write(fd, "64M", 3); close(fd); }

    // CPU Max: 25% (expressed as "quota period")
    snprintf(file_path, sizeof(file_path), "%s/cpu.max", path);
    fd = open(file_path, O_WRONLY);
    if (fd >= 0) { write(fd, "25000 100000", 12); close(fd); }

    // 4. Attach PID to the cgroup
    snprintf(file_path, sizeof(file_path), "%s/cgroup.procs", path);
    fd = open(file_path, O_WRONLY);
    if (fd >= 0) { write(fd, pid_str, strlen(pid_str)); close(fd); }
}

static void cleanup_cgroups(const char *id) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/memory/mini_runtime/%s", CGROUP_BASE, id);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/cpu/mini_runtime/%s", CGROUP_BASE, id);
    rmdir(path);
}

/*
 * Logging & Buffer Logic
 *  */
static int bounded_buffer_push(bounded_buffer_t *buf, const char *id, const char *msg) {
    pthread_mutex_lock(&buf->lock);
    while (buf->count == BUFFER_SIZE && !buf->shutdown) pthread_cond_wait(&buf->not_full, &buf->lock);
    if (buf->shutdown) { pthread_mutex_unlock(&buf->lock); return -1; }
    strncpy(buf->pool[buf->head].id, id, CONTAINER_ID_LEN);
    strncpy(buf->pool[buf->head].msg, msg, MAX_LOG_LINE);
    buf->head = (buf->head + 1) % BUFFER_SIZE;
    buf->count++;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->lock);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buf, log_entry_t *out) {
    pthread_mutex_lock(&buf->lock);
    while (buf->count == 0 && !buf->shutdown) pthread_cond_wait(&buf->not_empty, &buf->lock);
    if (buf->count == 0 && buf->shutdown) { pthread_mutex_unlock(&buf->lock); return -1; }
    *out = buf->pool[buf->tail];
    buf->tail = (buf->tail + 1) % BUFFER_SIZE;
    buf->count--;
    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->lock);
    return 0;
}

void *logger_consumer(void *arg) {
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_entry_t entry;
    mkdir(LOG_DIR, 0755);
    while (bounded_buffer_pop(&ctx->log_buffer, &entry) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, entry.id);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { write(fd, entry.msg, strlen(entry.msg)); close(fd); }
    }
    return NULL;
}

void *container_reader_producer(void *arg) {
    producer_config_t *cfg = (producer_config_t *)arg;
    FILE *stream = fdopen(cfg->pipe_read_fd, "r");
    char line[MAX_LOG_LINE];
    while (stream && fgets(line, MAX_LOG_LINE, stream)) {
        if (bounded_buffer_push(&g_ctx->log_buffer, cfg->id, line) < 0) break;
    }
    if (stream) fclose(stream);
    pthread_mutex_lock(&g_ctx->metadata_lock);
    for (container_record_t *r = g_ctx->containers; r; r = r->next)
        if (strcmp(r->id, cfg->id) == 0) r->producer_done = 1;
    pthread_mutex_unlock(&g_ctx->metadata_lock);
    free(cfg);
    return NULL;
}
/* In your supervisor logic */
void enforce_policy(pid_t pid, const char* id) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) return;

    struct health_response res = { .pid = pid };
    if (ioctl(fd, MONITOR_GET_HEALTH, &res) == 0) {
        if (res.health == HEALTH_WARN) {
            printf("[POLICY] Container %s is throttled (Soft Limit Hit)\n", id);
            // Action: Increase 'niceness' to deprioritize CPU
            setpriority(PRIO_PROCESS, pid, 15); 
        }
    }
    close(fd);
}

/* 
 * Container Lifecycle
 * */
int child_fn(void *arg) {
    child_config_t *cfg = (child_config_t *)arg;
    dup2(cfg->pipe_write_fd, STDOUT_FILENO);
    dup2(cfg->pipe_write_fd, STDERR_FILENO);
    close(cfg->pipe_write_fd);
    chroot(cfg->rootfs); chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);
    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
    return 1;
}

static void sigchld_handler(int sig) {
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        for (container_record_t *r = g_ctx->containers; r; r = r->next) {
            if (r->host_pid == pid) {
                r->state = WIFEXITED(status) ? CONTAINER_EXITED : (r->stop_requested ? CONTAINER_STOPPED : CONTAINER_KILLED);
                r->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
                cleanup_cgroups(r->id);
                break;
            }
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}
void register_with_kernel(pid_t pid, const char* container_id) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("Failed to open /dev/container_monitor");
        return;
    }

    struct monitor_request req;
    req.pid = pid;
    strncpy(req.container_id, container_id, MONITOR_NAME_LEN);
    
    // Limits: 10MB soft (easy to hit), 64MB hard
    req.soft_limit_bytes = 10 * 1024 * 1024; 
    req.hard_limit_bytes = 64 * 1024 * 1024;

    if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
        perror("IOCTL Register Failed");
    } else {
        printf("Successfully registered %s (PID %d) with Kernel Monitor\n", container_id, pid);
    }

    close(fd);
}

static int launch_container(supervisor_ctx_t *ctx, const control_request_t *req) {
    int p[2]; pipe(p);
    child_config_t c_cfg;
    strncpy(c_cfg.id, req->container_id, CONTAINER_ID_LEN);
    strncpy(c_cfg.rootfs, req->rootfs, PATH_MAX);
    strncpy(c_cfg.command, req->command, CHILD_COMMAND_LEN);
    c_cfg.pipe_write_fd = p[1];

    void *stack = malloc(STACK_SIZE);
    pid_t pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, &c_cfg);
    close(p[1]);

    setup_cgroups(req->container_id, pid);

    container_record_t *rec = calloc(1, sizeof(*rec));
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN);
    rec->host_pid = pid; rec->state = CONTAINER_RUNNING;
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers; ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    producer_config_t *p_cfg = malloc(sizeof(*p_cfg));
    strncpy(p_cfg->id, req->container_id, CONTAINER_ID_LEN);
    p_cfg->pipe_read_fd = p[0];
    pthread_create(&rec->producer_tid, NULL, container_reader_producer, p_cfg);
    if (pid > 0) {
    setup_cgroups(req->container_id, pid); // Task 4
    register_with_kernel(pid, req->container_id); // Task 5 Integration
}
    return 0;
}

/* 
 * Server & Client Handlers
 * */
static void handle_request(supervisor_ctx_t *ctx, int fd) {
    control_request_t req; control_response_t res = {0};
    if (recv(fd, &req, sizeof(req), MSG_WAITALL) <= 0) { close(fd); return; }

    if (req.kind == CMD_START) {
        launch_container(ctx, &req);
        snprintf(res.message, CONTROL_MESSAGE_LEN, "started '%s'", req.container_id);
    } else if (req.kind == CMD_PS) {
        pthread_mutex_lock(&ctx->metadata_lock);
        int off = snprintf(res.message, CONTROL_MESSAGE_LEN, "%-16s %-8s %-10s\n", "ID", "PID", "STATE");
        for (container_record_t *r = ctx->containers; r; r = r->next) {
            const char *s = (r->state == CONTAINER_RUNNING) ? "running" : (r->state == CONTAINER_EXITED ? "exited" : "killed");
            off += snprintf(res.message + off, CONTROL_MESSAGE_LEN - off, "%-16s %-8d %-10s\n", r->id, r->host_pid, s);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    } else if (req.kind == CMD_STOP) {
        pthread_mutex_lock(&ctx->metadata_lock);
        for (container_record_t *r = ctx->containers; r; r = r->next) {
            if (strcmp(r->id, req.container_id) == 0 && r->state == CONTAINER_RUNNING) {
                r->stop_requested = 1; kill(r->host_pid, SIGTERM);
                snprintf(res.message, CONTROL_MESSAGE_LEN, "stopped '%s'", r->id);
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
    send(fd, &res, sizeof(res), 0); close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "supervisor") == 0) {
        supervisor_ctx_t ctx = {0}; g_ctx = &ctx;
        pthread_mutex_init(&ctx.metadata_lock, NULL);
        pthread_mutex_init(&ctx.log_buffer.lock, NULL);
        pthread_cond_init(&ctx.log_buffer.not_full, NULL);
        pthread_cond_init(&ctx.log_buffer.not_empty, NULL);

        struct sigaction sa = {.sa_handler = sigchld_handler, .sa_flags = SA_RESTART};
        sigaction(SIGCHLD, &sa, NULL);

        pthread_create(&ctx.consumer_tid, NULL, logger_consumer, &ctx);
        unlink(CONTROL_PATH);
        ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
        bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
        listen(ctx.server_fd, 8);

        while (!ctx.should_stop) {
            int c_fd = accept(ctx.server_fd, NULL, NULL);
            if (c_fd >= 0) handle_request(&ctx, c_fd);
        }
        return 0;
    }

    control_request_t req = {0};
    if (strcmp(argv[1], "start") == 0 && argc >= 5) {
        req.kind = CMD_START;
        strncpy(req.container_id, argv[2], CONTAINER_ID_LEN);
        strncpy(req.rootfs, argv[3], PATH_MAX);
        strncpy(req.command, argv[4], CHILD_COMMAND_LEN);
    } else if (strcmp(argv[1], "ps") == 0) {
        req.kind = CMD_PS;
    } else if (strcmp(argv[1], "stop") == 0 && argc >= 3) {
        req.kind = CMD_STOP;
        strncpy(req.container_id, argv[2], CONTAINER_ID_LEN);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        send(fd, &req, sizeof(req), 0);
        control_response_t res; recv(fd, &res, sizeof(res), MSG_WAITALL);
        printf("%s\n", res.message);
    }
    close(fd);
    return 0;
}
