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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/resource.h>
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
#define CONTROL_MESSAGE_LEN 256
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
    CONTAINER_HARD_LIMIT_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    void *stack_ptr;
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
    int exit_code;
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
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    pthread_cond_t state_change;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t *ctx;
    int client_fd;
    control_request_t req;
} client_request_ctx_t;

typedef struct log_reader_ctx {
    int fd;
    bounded_buffer_t *buffer;
    char container_id[CONTAINER_ID_LEN];
} log_reader_ctx_t;

static int handle_control_request(supervisor_ctx_t *ctx,
                                  int client_fd,
                                  const control_request_t *req);
static const char *state_to_string(container_state_t state);
static int child_fn(void *arg);
static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes);
static int unregister_from_monitor(int monitor_fd,
                                   const char *container_id,
                                   pid_t host_pid);
static void *log_reader_thread(void *arg);

static volatile sig_atomic_t supervisor_terminate = 0;
static volatile sig_atomic_t child_reaped = 0;

static int send_all(int fd, const void *buf, size_t size)
{
    const char *ptr = buf;
    size_t left = size;
    while (left > 0) {
        ssize_t written = write(fd, ptr, left);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        ptr += written;
        left -= written;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t size)
{
    char *ptr = buf;
    size_t left = size;
    while (left > 0) {
        ssize_t n = read(fd, ptr, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return 0;
        ptr += n;
        left -= n;
    }
    return 1;
}

static container_record_t *find_container_by_id(supervisor_ctx_t *ctx,
                                                const char *id)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (strcmp(cur->id, id) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static container_record_t *find_container_by_pid(supervisor_ctx_t *ctx,
                                                 pid_t pid)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (cur->host_pid == pid)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static void add_container_record(supervisor_ctx_t *ctx,
                                 container_record_t *record)
{
    record->next = ctx->containers;
    ctx->containers = record;
}

static int create_logs_dir(void)
{
    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void stop_signal_handler(int signo)
{
    (void)signo;
    supervisor_terminate = 1;
}

static void child_signal_handler(int signo)
{
    (void)signo;
    child_reaped = 1;
}

static void update_child_exit(supervisor_ctx_t *ctx, pid_t pid, int status)
{
    container_record_t *rec;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = find_container_by_pid(ctx, pid);
    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        return;
    }

    rec->exit_signal = 0;
    if (WIFEXITED(status)) {
        rec->state = CONTAINER_EXITED;
        rec->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        rec->exit_signal = WTERMSIG(status);
        if (rec->stop_requested) {
            rec->state = CONTAINER_STOPPED;
            rec->exit_code = 128 + rec->exit_signal;
        } else if (rec->exit_signal == SIGKILL) {
            rec->state = CONTAINER_HARD_LIMIT_KILLED;
            rec->exit_code = 128 + rec->exit_signal;
        } else {
            rec->state = CONTAINER_KILLED;
            rec->exit_code = 128 + rec->exit_signal;
        }
    } else {
        rec->state = CONTAINER_STOPPED;
        rec->exit_code = -1;
    }

    if (rec->stack_ptr) {
        free(rec->stack_ptr);
        rec->stack_ptr = NULL;
    }

    pthread_cond_broadcast(&ctx->state_change);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0)
        unregister_from_monitor(ctx->monitor_fd, rec->id, pid);
}

static int reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        update_child_exit(ctx, pid, status);
    }

    if (pid < 0 && errno != ECHILD)
        return -1;

    child_reaped = 0;
    return 0;
}

static int send_response(int client_fd,
                         int status,
                         int exit_code,
                         const char *message,
                         const char *payload,
                         size_t payload_len)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    resp.exit_code = exit_code;
    if (message)
        strncpy(resp.message, message, sizeof(resp.message) - 1);

    if (send_all(client_fd, &resp, sizeof(resp)) != 0)
        return -1;

    if (payload && payload_len > 0)
        return send_all(client_fd, payload, payload_len);

    return 0;
}

static int send_string_response(int client_fd,
                                int status,
                                int exit_code,
                                const char *message)
{
    return send_response(client_fd, status, exit_code, message, NULL, 0);
}

static int send_file_response(int client_fd,
                              const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    if (send_string_response(client_fd, 0, 0, NULL) != 0) {
        close(fd);
        return -1;
    }

    char buffer[LOG_CHUNK_SIZE];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        if (send_all(client_fd, buffer, (size_t)n) != 0) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

static int send_list_response(int client_fd, const char *list_text)
{
    return send_response(client_fd, 0, 0, NULL, list_text, strlen(list_text));
}

static int send_control_response(int client_fd, int status, const char *message)
{
    return send_string_response(client_fd, status, 0, message);
}

static void *client_request_thread(void *arg)
{
    client_request_ctx_t *client_ctx = arg;
    handle_control_request(client_ctx->ctx, client_ctx->client_fd, &client_ctx->req);
    close(client_ctx->client_fd);
    free(client_ctx);
    return NULL;
}

static int build_ps_output(supervisor_ctx_t *ctx, char *buffer, size_t buffer_len)
{
    size_t used = 0;
    used += snprintf(buffer + used, buffer_len - used,
                     "%-16s %-10s %-8s %-8s %-8s\n",
                     "CONTAINER", "STATE", "PID", "SOFT(MiB)", "HARD(MiB)");

    container_record_t *cur = ctx->containers;
    while (cur && used + 128 < buffer_len) {
        used += snprintf(buffer + used, buffer_len - used,
                         "%-16s %-10s %-8d %-8lu %-8lu\n",
                         cur->id,
                         state_to_string(cur->state),
                         (int)cur->host_pid,
                         cur->soft_limit_bytes >> 20,
                         cur->hard_limit_bytes >> 20);
        cur = cur->next;
    }
    return 0;
}

static int handle_control_request(supervisor_ctx_t *ctx,
                                  int client_fd,
                                  const control_request_t *req)
{
    if (req->kind == CMD_START || req->kind == CMD_RUN) {
        pthread_mutex_lock(&ctx->metadata_lock);
        if (find_container_by_id(ctx, req->container_id) != NULL) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            return send_control_response(client_fd, 1, "container already exists");
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (create_logs_dir() != 0)
            return send_control_response(client_fd, 1, "failed to create logs directory");

        int pipe_fds[2];
        if (pipe(pipe_fds) < 0)
            return send_control_response(client_fd, 1, "failed to open log pipe");

        void *child_stack = malloc(STACK_SIZE);
        if (!child_stack) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return send_control_response(client_fd, 1, "failed to allocate child stack");
        }

        child_config_t *config = calloc(1, sizeof(*config));
        if (!config) {
            free(child_stack);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return send_control_response(client_fd, 1, "failed to allocate child config");
        }

        strncpy(config->id, req->container_id, sizeof(config->id) - 1);
        strncpy(config->rootfs, req->rootfs, sizeof(config->rootfs) - 1);
        strncpy(config->command, req->command, sizeof(config->command) - 1);
        config->nice_value = req->nice_value;
        config->log_write_fd = pipe_fds[1];

        pid_t child_pid = clone(child_fn,
                                child_stack + STACK_SIZE,
                                CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD,
                                config);
        if (child_pid < 0) {
            free(child_stack);
            free(config);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return send_control_response(client_fd, 1, "failed to clone child");
        }

        close(pipe_fds[1]);

        container_record_t *record = calloc(1, sizeof(*record));
        if (!record) {
            kill(child_pid, SIGKILL);
            close(pipe_fds[0]);
            free(child_stack);
            free(config);
            return send_control_response(client_fd, 1, "failed to allocate container record");
        }

        strncpy(record->id, req->container_id, sizeof(record->id) - 1);
        record->host_pid = child_pid;
        record->started_at = time(NULL);
        record->state = CONTAINER_RUNNING;
        record->soft_limit_bytes = req->soft_limit_bytes;
        record->hard_limit_bytes = req->hard_limit_bytes;
        record->exit_code = -1;
        record->exit_signal = 0;
        record->stop_requested = 0;
        record->stack_ptr = child_stack;
        snprintf(record->log_path, sizeof(record->log_path), LOG_DIR "/%s.log", record->id);

        pthread_mutex_lock(&ctx->metadata_lock);
        add_container_record(ctx, record);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (ctx->monitor_fd >= 0)
            register_with_monitor(ctx->monitor_fd,
                                  record->id,
                                  record->host_pid,
                                  record->soft_limit_bytes,
                                  record->hard_limit_bytes);

        struct log_reader_ctx *reader = calloc(1, sizeof(*reader));
        if (reader) {
            pthread_t thread;
            reader->fd = pipe_fds[0];
            reader->buffer = &ctx->log_buffer;
            strncpy(reader->container_id, record->id, sizeof(reader->container_id) - 1);
            if (pthread_create(&thread, NULL, log_reader_thread, reader) != 0) {
                close(reader->fd);
                free(reader);
            } else {
                pthread_detach(thread);
            }
        } else {
            close(pipe_fds[0]);
        }

        if (req->kind == CMD_RUN) {
            pthread_mutex_lock(&ctx->metadata_lock);
            while (record->state == CONTAINER_RUNNING && !supervisor_terminate)
                pthread_cond_wait(&ctx->state_change, &ctx->metadata_lock);
            int exit_code = record->exit_code;
            pthread_mutex_unlock(&ctx->metadata_lock);
            return send_string_response(client_fd, 0, exit_code, "container exited");
        }

        return send_control_response(client_fd, 0, "container started");
    }

    if (req->kind == CMD_PS) {
        char output[8192];
        pthread_mutex_lock(&ctx->metadata_lock);
        build_ps_output(ctx, output, sizeof(output));
        pthread_mutex_unlock(&ctx->metadata_lock);
        return send_list_response(client_fd, output);
    }

    if (req->kind == CMD_LOGS) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *record = find_container_by_id(ctx, req->container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (!record)
            return send_control_response(client_fd, 1, "container not found");
        if (send_file_response(client_fd, record->log_path) != 0)
            return send_control_response(client_fd, 1, "failed to send logs");
        return 0;
    }

    if (req->kind == CMD_STOP) {
    container_record_t *record;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_by_id(ctx, req->container_id);
    if (record)
        record->stop_requested = 1;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!record)
        return send_control_response(client_fd, 1, "container not found");

    if (kill(record->host_pid, SIGTERM) < 0) {
        perror("SIGTERM failed");
        return send_control_response(client_fd, 1, "failed to send SIGTERM");
    }

    sleep(1);

    if (kill(record->host_pid, 0) == 0) {
        if (kill(record->host_pid, SIGKILL) < 0) {
            perror("SIGKILL failed");
            return send_control_response(client_fd, 1, "failed to force kill container");
        }
    }

    return send_control_response(client_fd, 0, "stop requested");
}
}
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
    case CONTAINER_HARD_LIMIT_KILLED:
        return "hard_limit_killed";
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

/*
 * Producer-side insertion into the bounded buffer.
 * This implementation blocks when the buffer is full and wakes consumers
 * correctly, while stopping cleanly during shutdown.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    int rc = 0;

    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        rc = -1;
    } else {
        buffer->items[buffer->tail] = *item;
        buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
        buffer->count++;
        pthread_cond_signal(&buffer->not_empty);
        rc = 0;
    }
    pthread_mutex_unlock(&buffer->mutex);
    return rc;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    int rc = 0;

    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        rc = 1;
    } else {
        *item = buffer->items[buffer->head];
        buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
        buffer->count--;
        pthread_cond_signal(&buffer->not_full);
        rc = 0;
    }
    pthread_mutex_unlock(&buffer->mutex);
    return rc;
}

static void write_all_to_fd(int fd, const char *data, size_t len)
{
    const char *ptr = data;
    size_t left = len;
    while (left > 0) {
        ssize_t written = write(fd, ptr, left);
        if (written <= 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        ptr += written;
        left -= written;
    }
}

static void *log_reader_thread(void *arg)
{
    log_reader_ctx_t *reader = arg;
    ssize_t n;
    log_item_t item;
    char chunk[LOG_CHUNK_SIZE];

    while ((n = read(reader->fd, chunk, sizeof(chunk))) > 0) {
        item.length = (size_t)n;
        memcpy(item.container_id, reader->container_id, sizeof(item.container_id));
        memcpy(item.data, chunk, item.length);
        if (bounded_buffer_push(reader->buffer, &item) != 0)
            break;
    }

    close(reader->fd);
    free(reader);
    return NULL;
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (1) {
        int rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc == 1)
            break;
        if (rc != 0)
            continue;

        char path[PATH_MAX];
        int fd;

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *record = find_container_by_id(ctx, item.container_id);
        if (record)
            strncpy(path, record->log_path, sizeof(path) - 1);
        else
            snprintf(path, sizeof(path), LOG_DIR "/%s.log", item.container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);

        fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0)
            continue;

        write_all_to_fd(fd, item.data, item.length);
        close(fd);
    }

    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *config = arg;
    char *const child_argv[] = {"sh", "-c", config->command, NULL};

    if (sethostname(config->id, strnlen(config->id, sizeof(config->id))) < 0)
        perror("sethostname");

    if (chroot(config->rootfs) < 0) {
    perror("chroot");
    _exit(1);
}

if (chdir("/") < 0) {
    perror("chdir");
    _exit(1);
}

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        _exit(1);
    }

    if (mount("proc", "/proc", "proc", MS_NOEXEC | MS_NOSUID | MS_NODEV, "") < 0) {
        perror("mount /proc");
        _exit(1);
    }

    if (dup2(config->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(config->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        _exit(1);
    }

    close(config->log_write_fd);

    if (setpriority(PRIO_PROCESS, 0, config->nice_value) < 0)
        perror("setpriority");

    execvp("/bin/sh", child_argv);
    perror("execvp");
    _exit(127);
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
 * Supervisor process implementation.
 * The supervisor creates the control socket, initializes metadata,
 * starts logging and client worker threads, reaps child containers,
 * and responds to control requests.
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    struct sigaction act;
    struct pollfd pfd;
    int rc;
    int logger_started = 0;

    (void)rootfs;
    supervisor_terminate = 0;
    child_reaped = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = pthread_cond_init(&ctx.state_change, NULL);
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
        pthread_cond_destroy(&ctx.state_change);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR | O_CLOEXEC);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
        goto cleanup;
    }

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        goto cleanup;
    }

    memset(&act, 0, sizeof(act));
    act.sa_handler = child_signal_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &act, NULL);

    act.sa_handler = stop_signal_handler;
    act.sa_flags = 0;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create");
        goto cleanup;
    }
    logger_started = 1;

    while (!supervisor_terminate) {
        pfd.fd = ctx.server_fd;
        pfd.events = POLLIN;

        rc = poll(&pfd, 1, 500);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (rc > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR)
                    continue;
                perror("accept");
                break;
            }

            control_request_t req;
            if (recv_all(client_fd, &req, sizeof(req)) != 1) {
                close(client_fd);
                continue;
            }

            client_request_ctx_t *client_ctx = malloc(sizeof(*client_ctx));
            if (!client_ctx) {
                close(client_fd);
                continue;
            }

            client_ctx->ctx = &ctx;
            client_ctx->client_fd = client_fd;
            client_ctx->req = req;

            pthread_t thread;
            if (pthread_create(&thread, NULL, client_request_thread, client_ctx) != 0) {
                close(client_fd);
                free(client_ctx);
                continue;
            }
            pthread_detach(thread);
        }

        if (child_reaped)
            reap_children(&ctx);
    }

    if (logger_started)
        bounded_buffer_begin_shutdown(&ctx.log_buffer);

    if (logger_started)
        pthread_join(ctx.logger_thread, NULL);

cleanup:
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    unlink(CONTROL_PATH);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_cond_destroy(&ctx.state_change);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * Client-side control path implementation over a UNIX domain socket.
 * `run` clients also forward interrupt/termination intent via a stop request.
 */
static volatile sig_atomic_t run_stop_signal = 0;
static char run_stop_container[CONTAINER_ID_LEN];

static void run_signal_forward_handler(int signo)
{
    (void)signo;
    run_stop_signal = 1;
}

static int send_control_request(const control_request_t *req);

static int send_stop_request(const char *container_id)
{
    control_request_t stop_req;
    memset(&stop_req, 0, sizeof(stop_req));
    stop_req.kind = CMD_STOP;
    strncpy(stop_req.container_id, container_id, sizeof(stop_req.container_id) - 1);
    return send_control_request(&stop_req);
}

static int send_control_request(const control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    if (send_all(sock, req, sizeof(*req)) != 0) {
        perror("send");
        close(sock);
        return 1;
    }

    control_response_t resp;
    while (1) {
        int result = recv_all(sock, &resp, sizeof(resp));
        if (result == 1)
            break;

        if (result == 0) {
            fprintf(stderr, "connection closed unexpectedly\n");
            close(sock);
            return 1;
        }

        if (errno == EINTR && run_stop_signal && req->kind == CMD_RUN) {
            run_stop_signal = 0;
            send_stop_request(run_stop_container);
            continue;
        }

        perror("recv");
        close(sock);
        return 1;
    }

    if (resp.status != 0) {
        fprintf(stderr, "%s\n", resp.message[0] ? resp.message : "request failed");
        close(sock);
        return resp.status;
    }

    if (resp.message[0])
        printf("%s\n", resp.message);

    if (req->kind == CMD_PS || req->kind == CMD_LOGS) {
        char buffer[LOG_CHUNK_SIZE];
        ssize_t n;
        while ((n = read(sock, buffer, sizeof(buffer))) > 0)
            write_all_to_fd(STDOUT_FILENO, buffer, (size_t)n);
        if (n < 0 && errno != EINTR)
            perror("read");
    }

    close(sock);
    if (req->kind == CMD_RUN)
        return resp.exit_code;
    return 0;
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
    req.nice_value = 0;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    struct sigaction old_int, old_term, act;
    int result;

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
    req.nice_value = 0;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    run_stop_signal = 0;
    strncpy(run_stop_container, req.container_id, sizeof(run_stop_container) - 1);
    run_stop_container[CONTAINER_ID_LEN - 1] = '\0';

    memset(&act, 0, sizeof(act));
    act.sa_handler = run_signal_forward_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGINT, &act, &old_int);
    sigaction(SIGTERM, &act, &old_term);

    result = send_control_request(&req);

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    run_stop_signal = 0;

    return result;
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

