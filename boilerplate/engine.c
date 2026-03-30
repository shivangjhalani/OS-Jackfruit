/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Task 2 implemented:
 *   - UNIX domain socket control plane (Path B)
 *   - Supervisor accept loop with request dispatch
 *   - SIGCHLD / SIGINT / SIGTERM handling
 *   - All CLI commands: start, run, ps, logs, stop
 *
 * Tasks 1 and 3 stubs preserved as-is from boilerplate.
 */

#define _GNU_SOURCE
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

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  256
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT   (64UL << 20)   /* 64 MiB */

/* ------------------------------------------------------------------ */
/* Enumerations                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ------------------------------------------------------------------ */
/* Data structures                                                      */
/* ------------------------------------------------------------------ */

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;          /* set before sending SIGTERM/SIGKILL */
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    pid_t client_pid;            /* used by CMD_RUN to identify waiter */
} control_request_t;

typedef struct {
    int  status;                 /* 0 = ok, non-zero = error            */
    int  container_exit_code;   /* used by CMD_RUN response             */
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

typedef struct {
    int             server_fd;
    int             monitor_fd;
    volatile int    should_stop;
    pthread_t       logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ------------------------------------------------------------------ */
/* Global supervisor context pointer (needed by signal handlers)        */
/* ------------------------------------------------------------------ */

static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Usage                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/* Argument parsing helpers                                             */
/* ------------------------------------------------------------------ */

static int parse_mib_flag(const char *flag,
                           const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                 int argc, char *argv[],
                                 int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long  nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1],
                                &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1],
                                &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr,
                "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* State helpers                                                        */
/* ------------------------------------------------------------------ */

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded buffer (Task 3 stubs — not implemented yet)                  */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/* TODO Task 3 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    (void)buffer; (void)item;
    return -1;
}

/* TODO Task 3 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    (void)buffer; (void)item;
    return -1;
}

/* TODO Task 3 */
void *logging_thread(void *arg)
{
    (void)arg;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container clone / child (Task 1 stub)                                */
/* ------------------------------------------------------------------ */

/* TODO Task 1 */
int child_fn(void *arg)
{
    (void)arg;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Monitor ioctl helpers                                                */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                           const char *container_id,
                           pid_t host_pid,
                           unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                             const char *container_id,
                             pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ================================================================== */
/*                                                                      */
/*   TASK 2 — Supervisor CLI and Signal Handling                        */
/*                                                                      */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Signal handling                                                      */
/*                                                                      */
/* SIGCHLD  — reap exited children, update metadata                    */
/* SIGINT / SIGTERM — set should_stop so the event loop exits cleanly  */
/* ------------------------------------------------------------------ */

/*
 * SIGCHLD handler.
 *
 * We use waitpid(-1, WNOHANG) in a loop to reap every child that has
 * exited since the last delivery.  The handler only touches the global
 * context pointer; all metadata updates happen in the supervisor loop
 * after the signal is noticed via the self-pipe trick.
 *
 * We write one byte to a self-pipe so the blocking accept() / select()
 * in the event loop can wake up and call the reaper without a race.
 */
static int g_sigchld_pipe[2] = {-1, -1};

static void sigchld_handler(int sig)
{
    (void)sig;
    /* Best-effort write; ignore EAGAIN — the pipe already has data */
    char byte = 'c';
    (void)write(g_sigchld_pipe[1], &byte, 1);
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
    /* Wake the accept loop by closing / interrupting */
    char byte = 'q';
    (void)write(g_sigchld_pipe[1], &byte, 1);
}

/*
 * Reap all exited children and update their metadata records.
 * Called from the supervisor event loop whenever the self-pipe is readable.
 */
static void reap_children(supervisor_ctx_t *ctx)
{
    int   wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        container_record_t *rec;

        pthread_mutex_lock(&ctx->metadata_lock);
        for (rec = ctx->containers; rec; rec = rec->next) {
            if (rec->host_pid != pid)
                continue;

            if (WIFEXITED(wstatus)) {
                rec->exit_code = WEXITSTATUS(wstatus);
                rec->exit_signal = 0;
                rec->state = CONTAINER_EXITED;
            } else if (WIFSIGNALED(wstatus)) {
                rec->exit_signal = WTERMSIG(wstatus);
                rec->exit_code   = 128 + rec->exit_signal;
                /*
                 * Distinguish graceful stop from hard-limit kill:
                 *   stop_requested set  → CONTAINER_STOPPED
                 *   SIGKILL, no request → CONTAINER_KILLED (hard limit)
                 */
                if (rec->stop_requested)
                    rec->state = CONTAINER_STOPPED;
                else
                    rec->state = CONTAINER_KILLED;
            }

            /* Unregister from kernel monitor if available */
            if (ctx->monitor_fd >= 0)
                unregister_from_monitor(ctx->monitor_fd,
                                        rec->id, rec->host_pid);
            break;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Supervisor command dispatch                                           */
/* ------------------------------------------------------------------ */

/*
 * Handle CMD_PS: build a human-readable table of all containers and
 * send it back in the response message.
 */
static void handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    container_record_t *rec;
    char line[256];
    char body[CONTROL_MESSAGE_LEN];
    int  off = 0;

    body[0] = '\0';

    pthread_mutex_lock(&ctx->metadata_lock);

    if (!ctx->containers) {
        snprintf(body, sizeof(body), "No containers.\n");
    } else {
        /* Header */
        off += snprintf(body + off, sizeof(body) - off,
                        "%-16s %-8s %-10s %s\n",
                        "ID", "PID", "STATE", "LOG");

        for (rec = ctx->containers; rec && off < (int)sizeof(body) - 1;
             rec = rec->next) {
            int n = snprintf(line, sizeof(line),
                             "%-16s %-8d %-10s %s\n",
                             rec->id,
                             (int)rec->host_pid,
                             state_to_string(rec->state),
                             rec->log_path);
            if (off + n >= (int)sizeof(body) - 1)
                break;
            memcpy(body + off, line, n);
            off += n;
        }
        body[off] = '\0';
    }

    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    strncpy(resp->message, body, sizeof(resp->message) - 1);
}

/*
 * Handle CMD_LOGS: read the container's log file and stream it back.
 * For simplicity the entire log is placed in the response message.
 * (Large logs will be truncated to CONTROL_MESSAGE_LEN bytes — a full
 * streaming implementation is a Task 3 extension.)
 */
static void handle_logs(supervisor_ctx_t *ctx,
                         const control_request_t *req,
                         control_response_t *resp)
{
    container_record_t *rec;
    char log_path[PATH_MAX];
    FILE *f;

    log_path[0] = '\0';

    pthread_mutex_lock(&ctx->metadata_lock);
    for (rec = ctx->containers; rec; rec = rec->next) {
        if (strncmp(rec->id, req->container_id,
                    CONTAINER_ID_LEN) == 0) {
            strncpy(log_path, rec->log_path, PATH_MAX - 1);
            break;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (log_path[0] == '\0') {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "No container with id '%s'.\n", req->container_id);
        return;
    }

    f = fopen(log_path, "r");
    if (!f) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Cannot open log '%s': %s\n",
                 log_path, strerror(errno));
        return;
    }

    size_t n = fread(resp->message, 1,
                     sizeof(resp->message) - 1, f);
    resp->message[n] = '\0';
    fclose(f);
    resp->status = 0;
}

/*
 * Handle CMD_STOP: send SIGTERM to the container's host PID.
 * Sets stop_requested so the SIGCHLD reaper classifies it correctly.
 */
static void handle_stop(supervisor_ctx_t *ctx,
                          const control_request_t *req,
                          control_response_t *resp)
{
    container_record_t *rec;
    int found = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    for (rec = ctx->containers; rec; rec = rec->next) {
        if (strncmp(rec->id, req->container_id,
                    CONTAINER_ID_LEN) != 0)
            continue;

        if (rec->state != CONTAINER_RUNNING &&
            rec->state != CONTAINER_STARTING) {
            resp->status = 1;
            snprintf(resp->message, sizeof(resp->message),
                     "Container '%s' is not running (state: %s).\n",
                     rec->id, state_to_string(rec->state));
            found = 1;
            break;
        }

        rec->stop_requested = 1;
        if (kill(rec->host_pid, SIGTERM) < 0) {
            resp->status = 1;
            snprintf(resp->message, sizeof(resp->message),
                     "kill(%d, SIGTERM) failed: %s\n",
                     (int)rec->host_pid, strerror(errno));
        } else {
            resp->status = 0;
            snprintf(resp->message, sizeof(resp->message),
                     "SIGTERM sent to container '%s' (pid %d).\n",
                     rec->id, (int)rec->host_pid);
        }
        found = 1;
        break;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!found) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "No container with id '%s'.\n", req->container_id);
    }
}

/*
 * Handle CMD_START / CMD_RUN.
 *
 * Task 1 implements the actual clone() + namespace setup.
 * Here we allocate the metadata record and prepare the log file so
 * that the control-plane bookkeeping is correct even before Task 1
 * is filled in.
 *
 * Returns the new container record (with host_pid still 0 until
 * Task 1 sets it), or NULL on allocation failure.
 */
static container_record_t *handle_start(supervisor_ctx_t *ctx,
                                          const control_request_t *req,
                                          control_response_t *resp)
{
    container_record_t *rec;
    char log_path[PATH_MAX];

    /* Reject duplicate IDs */
    pthread_mutex_lock(&ctx->metadata_lock);
    for (rec = ctx->containers; rec; rec = rec->next) {
        if (strncmp(rec->id, req->container_id,
                    CONTAINER_ID_LEN) == 0 &&
            (rec->state == CONTAINER_RUNNING ||
             rec->state == CONTAINER_STARTING)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp->status = 1;
            snprintf(resp->message, sizeof(resp->message),
                     "Container '%s' is already running.\n",
                     req->container_id);
            return NULL;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);
    snprintf(log_path, sizeof(log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Out of memory allocating container record.\n");
        return NULL;
    }

    strncpy(rec->id,       req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(rec->log_path, log_path,          PATH_MAX - 1);
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_STARTING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;

    /*
     * TODO Task 1: call clone() here, set rec->host_pid, transition
     * state to CONTAINER_RUNNING, register with kernel monitor.
     */
    rec->host_pid = 0; /* placeholder until Task 1 */

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next        = ctx->containers;
    ctx->containers  = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "Container '%s' queued (Task 1 not yet implemented).\n",
             rec->id);
    return rec;
}

/*
 * Dispatch a single control request received from the client and
 * write the response back on the connected socket fd.
 */
static void dispatch_request(supervisor_ctx_t *ctx,
                              int client_fd,
                              const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    switch (req->kind) {
    case CMD_START:
        handle_start(ctx, req, &resp);
        break;

    case CMD_RUN: {
        /*
         * run = start + wait.
         * We start the container then block until its state is
         * no longer STARTING or RUNNING.  The SIGCHLD handler
         * will wake us via the self-pipe; we poll here.
         *
         * A production implementation would use a condition
         * variable on the record.  This polling loop keeps the
         * code self-contained for Task 2.
         */
        container_record_t *rec = handle_start(ctx, req, &resp);
        if (rec && resp.status == 0) {
            /* Wait for the container to finish */
            while (1) {
                container_state_t st;
                pthread_mutex_lock(&ctx->metadata_lock);
                st = rec->state;
                pthread_mutex_unlock(&ctx->metadata_lock);

                if (st != CONTAINER_STARTING &&
                    st != CONTAINER_RUNNING)
                    break;

                usleep(100 * 1000); /* 100 ms poll */
            }

            pthread_mutex_lock(&ctx->metadata_lock);
            resp.container_exit_code = rec->exit_code;
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' finished. Exit code: %d.\n",
                     rec->id, rec->exit_code);
            pthread_mutex_unlock(&ctx->metadata_lock);
        }
        break;
    }

    case CMD_PS:
        handle_ps(ctx, &resp);
        break;

    case CMD_LOGS:
        handle_logs(ctx, req, &resp);
        break;

    case CMD_STOP:
        handle_stop(ctx, req, &resp);
        break;

    default:
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message),
                 "Unknown command kind %d.\n", req->kind);
        break;
    }

    /* Send response — best effort */
    (void)write(client_fd, &resp, sizeof(resp));
}

/* ------------------------------------------------------------------ */
/* Supervisor event loop                                                 */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t    ctx;
    struct sockaddr_un  addr;
    struct sigaction    sa;
    fd_set              readfds;
    int                 rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx          = &ctx;

    /* -- Metadata lock -- */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    /* -- Bounded buffer (Task 3) -- */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* -- Self-pipe for signal notification -- */
    if (pipe(g_sigchld_pipe) < 0) {
        perror("pipe");
        goto cleanup;
    }
    fcntl(g_sigchld_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_sigchld_pipe[1], F_SETFL, O_NONBLOCK);

    /* -- Signal handlers -- */
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_flags   = SA_RESTART;
    sa.sa_handler = sigterm_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* -- Open kernel monitor device (optional — may not be loaded) -- */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] /dev/container_monitor not available "
                "(kernel module not loaded?)\n");

    /* -- Create UNIX domain socket -- */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    unlink(CONTROL_PATH); /* remove stale socket if any */

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen");
        goto cleanup;
    }

    fprintf(stderr,
            "[supervisor] Listening on %s  (rootfs base: %s)\n",
            CONTROL_PATH, rootfs);

    /* -- TODO Task 3: start logger thread --
     *    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
     */

    /* ---- Event loop ---- */
    while (!ctx.should_stop) {
        int    nfds;
        int    client_fd;
        struct timeval tv = {1, 0}; /* 1-second timeout so we check
                                       should_stop periodically */

        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd,       &readfds);
        FD_SET(g_sigchld_pipe[0],   &readfds);

        nfds = (ctx.server_fd > g_sigchld_pipe[0]
                ? ctx.server_fd
                : g_sigchld_pipe[0]) + 1;

        rc = select(nfds, &readfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        /* Drain self-pipe and reap children */
        if (FD_ISSET(g_sigchld_pipe[0], &readfds)) {
            char buf[64];
            while (read(g_sigchld_pipe[0], buf, sizeof(buf)) > 0)
                ; /* drain */
            reap_children(&ctx);

            /* 'q' byte means we should stop */
            if (ctx.should_stop)
                break;
        }

        /* Accept and handle one client connection */
        if (FD_ISSET(ctx.server_fd, &readfds)) {
            client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                perror("accept");
                break;
            }

            /* Read the fixed-size request struct */
            control_request_t req;
            ssize_t n = read(client_fd, &req, sizeof(req));
            if (n == (ssize_t)sizeof(req)) {
                dispatch_request(&ctx, client_fd, &req);
            } else {
                control_response_t err_resp;
                memset(&err_resp, 0, sizeof(err_resp));
                err_resp.status = 1;
                snprintf(err_resp.message, sizeof(err_resp.message),
                         "Malformed request (got %zd bytes).\n", n);
                (void)write(client_fd, &err_resp, sizeof(err_resp));
            }
            close(client_fd);
        }
    }

    fprintf(stderr, "[supervisor] Shutting down.\n");

    /* Send SIGTERM to all still-running containers */
    {
        container_record_t *rec;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (rec = ctx.containers; rec; rec = rec->next) {
            if (rec->state == CONTAINER_RUNNING ||
                rec->state == CONTAINER_STARTING) {
                rec->stop_requested = 1;
                kill(rec->host_pid, SIGTERM);
            }
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    /* Give children a moment then reap */
    usleep(500 * 1000);
    reap_children(&ctx);

cleanup:
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    /* TODO Task 3: pthread_join(ctx.logger_thread, NULL); */
    bounded_buffer_destroy(&ctx.log_buffer);

    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    if (g_sigchld_pipe[0] >= 0) close(g_sigchld_pipe[0]);
    if (g_sigchld_pipe[1] >= 0) close(g_sigchld_pipe[1]);

    /* Free container metadata list */
    {
        container_record_t *rec = ctx.containers;
        while (rec) {
            container_record_t *next = rec->next;
            free(rec);
            rec = next;
        }
    }

    pthread_mutex_destroy(&ctx.metadata_lock);
    g_ctx = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI client — send_control_request                                    */
/*                                                                      */
/* Opens the UNIX domain socket, writes the request struct, reads the  */
/* response struct, prints it, and returns the response status.        */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int                sock_fd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t            n;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s: %s\n"
                "Is the supervisor running?\n",
                CONTROL_PATH, strerror(errno));
        close(sock_fd);
        return 1;
    }

    /* Send request */
    n = write(sock_fd, req, sizeof(*req));
    if (n != (ssize_t)sizeof(*req)) {
        fprintf(stderr, "Short write sending request.\n");
        close(sock_fd);
        return 1;
    }

    /* Read response */
    n = read(sock_fd, &resp, sizeof(resp));
    close(sock_fd);

    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Short read receiving response.\n");
        return 1;
    }

    /* Print the supervisor's message */
    if (resp.message[0])
        printf("%s", resp.message);

    return resp.status;
}

/* ------------------------------------------------------------------ */
/* CMD_RUN client-side SIGINT/SIGTERM forwarding                        */
/*                                                                      */
/* When the run client receives SIGINT or SIGTERM it should forward a  */
/* stop to the supervisor and then continue waiting for the final       */
/* status.  We store the container id for the signal handler to use.   */
/* ------------------------------------------------------------------ */

static char g_run_container_id[CONTAINER_ID_LEN];

static void run_sigforward_handler(int sig)
{
    (void)sig;
    if (g_run_container_id[0]) {
        control_request_t stop_req;
        memset(&stop_req, 0, sizeof(stop_req));
        stop_req.kind = CMD_STOP;
        strncpy(stop_req.container_id, g_run_container_id,
                CONTAINER_ID_LEN - 1);
        /* fire-and-forget; ignore errors inside signal handler */
        send_control_request(&stop_req);
    }
}

/* ------------------------------------------------------------------ */
/* Individual command entry points                                       */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind              = CMD_START;
    req.soft_limit_bytes  = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes  = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    struct sigaction  sa;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind              = CMD_RUN;
    req.soft_limit_bytes  = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes  = DEFAULT_HARD_LIMIT;
    req.client_pid        = getpid();
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    /* Install forwarding handlers so Ctrl-C stops the container */
    strncpy(g_run_container_id, req.container_id, CONTAINER_ID_LEN - 1);
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;
    sa.sa_handler = run_sigforward_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                    "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
