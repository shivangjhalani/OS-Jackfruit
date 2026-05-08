/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
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
#include <poll.h>
#include <sys/resource.h>
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

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 8192
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

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

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs_path[PATH_MAX];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int finished;
    int stop_requested;
    int producer_started;
    pthread_t producer_thread;
    void *child_stack;
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
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile sig_atomic_t should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    pthread_cond_t state_cv;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    bounded_buffer_t *buffer;
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
} producer_args_t;

typedef struct {
    supervisor_ctx_t *ctx;
    int client_fd;
} client_handler_args_t;

static volatile sig_atomic_t g_reap_requested = 0;
static volatile sig_atomic_t g_stop_requested = 0;

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes);
int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid);

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

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
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

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

static ssize_t read_full(int fd, void *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = read(fd, (char *)buf + off, len - off);
        if (n == 0)
            return (ssize_t)off;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }

    return (ssize_t)off;
}

static ssize_t write_full(int fd, const void *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, (const char *)buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }

    return (ssize_t)off;
}

static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur = ctx->containers;

    while (cur) {
        if (strncmp(cur->id, id, CONTAINER_ID_LEN) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static int running_rootfs_conflict_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *cur = ctx->containers;

    while (cur) {
        if (!cur->finished && strncmp(cur->rootfs_path, rootfs, sizeof(cur->rootfs_path)) == 0)
            return 1;
        cur = cur->next;
    }
    return 0;
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (!buffer->shutting_down && buffer->count == LOG_BUFFER_CAPACITY)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST)
        perror("mkdir logs");

    for (;;) {
        FILE *fp;
        char path[PATH_MAX];
        int rc = bounded_buffer_pop(&ctx->log_buffer, &item);

        if (rc == 1)
            break;
        if (rc != 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fp = fopen(path, "ab");
        if (!fp)
            continue;

        if (item.length > 0)
            fwrite(item.data, 1, item.length, fp);
        fclose(fp);
    }

    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (sethostname(cfg->id, strnlen(cfg->id, sizeof(cfg->id))) < 0) {
        perror("sethostname");
        return 1;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount private");
        return 1;
    }

    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        return 1;
    }
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
        perror("setpriority");

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0)
        return 1;
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0)
        return 1;

    close(cfg->log_write_fd);

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("exec /bin/sh");
    return 1;
}

static void signal_handler(int sig)
{
    if (sig == SIGCHLD)
        g_reap_requested = 1;
    else if (sig == SIGINT || sig == SIGTERM)
        g_stop_requested = 1;
}

static void *producer_thread(void *arg)
{
    producer_args_t *pargs = (producer_args_t *)arg;
    log_item_t item;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, pargs->container_id, sizeof(item.container_id) - 1);

    for (;;) {
        ssize_t n = read(pargs->read_fd, item.data, sizeof(item.data));
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        item.length = (size_t)n;
        if (bounded_buffer_push(pargs->buffer, &item) != 0)
            break;
    }

    close(pargs->read_fd);
    free(pargs);
    return NULL;
}

static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           container_record_t **out_rec,
                           char *msg,
                           size_t msg_len)
{
    int pipefd[2] = { -1, -1 };
    void *stack = NULL;
    child_config_t *cfg = NULL;
    container_record_t *rec = NULL;
    producer_args_t *pargs = NULL;
    pid_t child_pid;

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_locked(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(msg, msg_len, "container id already exists: %s", req->container_id);
        return -1;
    }
    if (running_rootfs_conflict_locked(ctx, req->rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(msg, msg_len, "rootfs already used by a running container: %s", req->rootfs);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pipe(pipefd) < 0) {
        snprintf(msg, msg_len, "pipe failed: %s", strerror(errno));
        return -1;
    }

    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        snprintf(msg, msg_len, "alloc child config failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    stack = malloc(STACK_SIZE);
    if (!stack) {
        snprintf(msg, msg_len, "alloc child stack failed");
        close(pipefd[0]);
        close(pipefd[1]);
        free(cfg);
        return -1;
    }

    child_pid = clone(child_fn,
                      (char *)stack + STACK_SIZE,
                      CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                      cfg);
    if (child_pid < 0) {
        snprintf(msg, msg_len, "clone failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        free(cfg);
        free(stack);
        return -1;
    }

    close(pipefd[1]);
    free(cfg);

    pargs = calloc(1, sizeof(*pargs));
    rec = calloc(1, sizeof(*rec));
    if (!pargs || !rec) {
        if (pargs)
            free(pargs);
        if (rec)
            free(rec);
        kill(child_pid, SIGKILL);
        close(pipefd[0]);
        free(stack);
        snprintf(msg, msg_len, "alloc container metadata failed");
        return -1;
    }

    pargs->buffer = &ctx->log_buffer;
    pargs->read_fd = pipefd[0];
    strncpy(pargs->container_id, req->container_id, sizeof(pargs->container_id) - 1);

    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    strncpy(rec->rootfs_path, req->rootfs, sizeof(rec->rootfs_path) - 1);
    rec->host_pid = child_pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->exit_code = -1;
    rec->exit_signal = 0;
    rec->finished = 0;
    rec->stop_requested = 0;
    rec->child_stack = stack;
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, rec->id);

    if (pthread_create(&rec->producer_thread, NULL, producer_thread, pargs) != 0) {
        free(pargs);
        free(rec);
        kill(child_pid, SIGKILL);
        close(pipefd[0]);
        free(stack);
        snprintf(msg, msg_len, "failed to start producer thread");
        return -1;
    }
    rec->producer_started = 1;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd,
                                  rec->id,
                                  rec->host_pid,
                                  rec->soft_limit_bytes,
                                  rec->hard_limit_bytes) < 0) {
            fprintf(stderr,
                    "monitor register failed for %s (pid=%d): %s\n",
                    rec->id,
                    rec->host_pid,
                    strerror(errno));
        }
    }

    snprintf(msg, msg_len, "started container=%s pid=%d", rec->id, rec->host_pid);
    if (out_rec)
        *out_rec = rec;
    return 0;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    for (;;) {
        container_record_t *rec = NULL;

        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;

        pthread_mutex_lock(&ctx->metadata_lock);
        for (rec = ctx->containers; rec; rec = rec->next) {
            if (rec->host_pid == pid)
                break;
        }
        if (rec && !rec->finished) {
            rec->finished = 1;
            if (WIFEXITED(status)) {
                rec->exit_code = WEXITSTATUS(status);
                rec->state = CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                rec->exit_signal = WTERMSIG(status);
                if (rec->stop_requested)
                    rec->state = CONTAINER_STOPPED;
                else if (rec->exit_signal == SIGKILL)
                    rec->state = CONTAINER_KILLED;
                else
                    rec->state = CONTAINER_STOPPED;
            } else {
                rec->state = CONTAINER_STOPPED;
            }
            pthread_cond_broadcast(&ctx->state_cv);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (rec) {
            if (ctx->monitor_fd >= 0)
                (void)unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
            if (rec->producer_started) {
                pthread_join(rec->producer_thread, NULL);
                rec->producer_started = 0;
            }
            if (rec->child_stack) {
                free(rec->child_stack);
                rec->child_stack = NULL;
            }
        }
    }
}

static void build_ps_output(supervisor_ctx_t *ctx, char *out, size_t out_len)
{
    container_record_t *cur;
    size_t used = 0;
    int n;

    n = snprintf(out, out_len, "id pid state soft_mib hard_mib exit signal\n");
    if (n < 0)
        return;
    used = (size_t)n < out_len ? (size_t)n : out_len;

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur && used < out_len) {
        n = snprintf(out + used,
                     out_len - used,
                     "%s %d %s %lu %lu %d %d\n",
                     cur->id,
                     cur->host_pid,
                     state_to_string(cur->state),
                     cur->soft_limit_bytes >> 20,
                     cur->hard_limit_bytes >> 20,
                     cur->exit_code,
                     cur->exit_signal);
        if (n < 0)
            break;
        if ((size_t)n >= out_len - used) {
            used = out_len - 1;
            break;
        }
        used += (size_t)n;
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void handle_request(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           control_response_t *resp)
{
    container_record_t *rec;

    memset(resp, 0, sizeof(*resp));
    resp->status = 1;

    switch (req->kind) {
    case CMD_START:
        if (start_container(ctx, req, NULL, resp->message, sizeof(resp->message)) == 0)
            resp->status = 0;
        break;
    case CMD_RUN: {
        if (start_container(ctx, req, &rec, resp->message, sizeof(resp->message)) != 0)
            break;

        pthread_mutex_lock(&ctx->metadata_lock);
        while (!rec->finished)
            pthread_cond_wait(&ctx->state_cv, &ctx->metadata_lock);

        if (rec->exit_signal != 0)
            resp->status = 128 + rec->exit_signal;
        else
            resp->status = rec->exit_code;

        snprintf(resp->message,
                 sizeof(resp->message),
                 "container=%s finished state=%s status=%d",
                 rec->id,
                 state_to_string(rec->state),
                 resp->status);
        pthread_mutex_unlock(&ctx->metadata_lock);
        break;
    }
    case CMD_PS:
        resp->status = 0;
        build_ps_output(ctx, resp->message, sizeof(resp->message));
        break;
    case CMD_LOGS: {
        char path[PATH_MAX];
        int fd;
        ssize_t n;

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            snprintf(resp->message, sizeof(resp->message), "logs not found for %s", req->container_id);
            break;
        }

        n = read(fd, resp->message, sizeof(resp->message) - 1);
        if (n < 0) {
            close(fd);
            snprintf(resp->message, sizeof(resp->message), "failed to read logs: %s", strerror(errno));
            break;
        }
        resp->message[n] = '\0';
        close(fd);
        resp->status = 0;
        break;
    }
    case CMD_STOP:
        pthread_mutex_lock(&ctx->metadata_lock);
        rec = find_container_locked(ctx, req->container_id);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            snprintf(resp->message, sizeof(resp->message), "container not found: %s", req->container_id);
            break;
        }
        if (rec->finished) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            snprintf(resp->message, sizeof(resp->message), "container already finished: %s", req->container_id);
            resp->status = 0;
            break;
        }
        rec->stop_requested = 1;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (kill(rec->host_pid, SIGTERM) < 0) {
            snprintf(resp->message, sizeof(resp->message), "failed to stop %s: %s", req->container_id, strerror(errno));
            break;
        }

        resp->status = 0;
        snprintf(resp->message, sizeof(resp->message), "stop requested for %s", req->container_id);
        break;
    default:
        snprintf(resp->message, sizeof(resp->message), "unsupported command");
        break;
    }
}

static void *client_handler_thread(void *arg)
{
    client_handler_args_t *args = (client_handler_args_t *)arg;
    control_request_t req;
    control_response_t resp;

    if (read_full(args->client_fd, &req, sizeof(req)) == (ssize_t)sizeof(req)) {
        handle_request(args->ctx, &req, &resp);
        (void)write_full(args->client_fd, &resp, sizeof(resp));
    }

    close(args->client_fd);
    free(args);
    return NULL;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int exit_status = 1;
    int rc;
    struct sigaction sa;
    struct sockaddr_un addr;
    struct pollfd pfd;

    struct stat st;

    if (stat(rootfs, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Invalid base-rootfs: %s\n", rootfs);
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = pthread_cond_init(&ctx.state_cv, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_cond_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_cond_destroy(&ctx.state_cv);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "warning: monitor device unavailable: %s\n", strerror(errno));

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

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

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) < 0 ||
        sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        goto cleanup;
    }

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST)
        fprintf(stderr, "warning: cannot create logs dir: %s\n", strerror(errno));

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger");
        goto cleanup;
    }

    fprintf(stdout, "Supervisor listening on %s (base-rootfs=%s)\n", CONTROL_PATH, rootfs);

    pfd.fd = ctx.server_fd;
    pfd.events = POLLIN;

    while (!ctx.should_stop) {
        int prc;

        if (g_stop_requested)
            ctx.should_stop = 1;

        if (g_reap_requested) {
            reap_children(&ctx);
            g_reap_requested = 0;
        } else {
            reap_children(&ctx);
        }

        prc = poll(&pfd, 1, 500);
        if (prc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (prc > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                client_handler_args_t *args = calloc(1, sizeof(*args));
                pthread_t th;

                if (!args) {
                    close(client_fd);
                    continue;
                }

                args->ctx = &ctx;
                args->client_fd = client_fd;
                if (pthread_create(&th, NULL, client_handler_thread, args) == 0) {
                    pthread_detach(th);
                } else {
                    close(client_fd);
                    free(args);
                }
            }
        }
    }

    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *cur = ctx.containers;
        while (cur) {
            if (!cur->finished) {
                cur->stop_requested = 1;
                (void)kill(cur->host_pid, SIGTERM);
            }
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    for (;;) {
        int pending = 0;
        container_record_t *cur;

        reap_children(&ctx);

        pthread_mutex_lock(&ctx.metadata_lock);
        cur = ctx.containers;
        while (cur) {
            if (!cur->finished) {
                pending = 1;
                break;
            }
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);

        if (!pending)
            break;
    }

    exit_status = 0;

cleanup:
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    if (ctx.logger_thread)
        pthread_join(ctx.logger_thread, NULL);

    pthread_mutex_lock(&ctx.metadata_lock);
    while (ctx.containers) {
        container_record_t *next = ctx.containers->next;
        if (ctx.containers->producer_started)
            pthread_join(ctx.containers->producer_thread, NULL);
        if (ctx.containers->child_stack)
            free(ctx.containers->child_stack);
        free(ctx.containers);
        ctx.containers = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_cond_destroy(&ctx.state_cv);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return exit_status;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (write_full(fd, req, sizeof(*req)) < 0) {
        perror("write request");
        close(fd);
        return 1;
    }

    if (read_full(fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Failed to read supervisor response\n");
        close(fd);
        return 1;
    }

    if (resp.message[0] != '\0')
        printf("%s\n", resp.message);

    close(fd);
    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

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
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

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
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
