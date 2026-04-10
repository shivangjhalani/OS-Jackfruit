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
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STACK_SIZE          (1024 * 1024)
#define BUFFER_SIZE         16
#define MAX_LOG_LINE        512
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define CONTROL_MESSAGE_LEN 16384
#define LOG_DIR             "logs"
#define CHILD_COMMAND_LEN   256

typedef enum { CMD_UNKNOWN = 0, CMD_SUPERVISOR, CMD_START, CMD_RUN, CMD_STOP, CMD_PS, CMD_LOGS } command_kind_t;
typedef enum { CONTAINER_STARTING = 0, CONTAINER_RUNNING, CONTAINER_STOPPED, CONTAINER_HARD_LIMIT_KILLED, CONTAINER_EXITED } container_state_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char msg[MAX_LOG_LINE];
} log_entry_t;

typedef struct {
    log_entry_t pool[BUFFER_SIZE];
    int head;
    int tail;
    int count;
    int shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} bounded_buffer_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t start_time;
    container_state_t state;
    int exit_code;
    int term_signal;
    int stop_requested;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice;
    pthread_t producer_tid;
    int producer_done;
    struct container_record *next;
} container_record_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int pipe_write_fd;
} child_config_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice;
} control_request_t;

typedef struct {
    int status;
    int exit_code;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    int server_fd;
    volatile sig_atomic_t should_stop;
    pthread_mutex_t metadata_lock;
    pthread_cond_t metadata_cond;
    container_record_t *containers;
    bounded_buffer_t log_buffer;
    pthread_t consumer_tid;
    int signal_pipe[2];
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;
static volatile sig_atomic_t client_interrupt_requested = 0;
static char client_run_id[CONTAINER_ID_LEN] = "";

static const char *state_name(container_state_t state) {
    switch (state) {
        case CONTAINER_STARTING: return "starting";
        case CONTAINER_RUNNING: return "running";
        case CONTAINER_STOPPED: return "stopped";
        case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
        case CONTAINER_EXITED: return "exited";
        default: return "unknown";
    }
}

static int bounded_buffer_push(bounded_buffer_t *buf, const char *id, const char *msg) {
    pthread_mutex_lock(&buf->lock);
    while (buf->count == BUFFER_SIZE && !buf->shutdown) {
        pthread_cond_wait(&buf->not_full, &buf->lock);
    }
    if (buf->shutdown) {
        pthread_mutex_unlock(&buf->lock);
        return -1;
    }
    strncpy(buf->pool[buf->head].id, id, CONTAINER_ID_LEN - 1);
    buf->pool[buf->head].id[CONTAINER_ID_LEN - 1] = '\0';
    strncpy(buf->pool[buf->head].msg, msg, MAX_LOG_LINE - 1);
    buf->pool[buf->head].msg[MAX_LOG_LINE - 1] = '\0';
    buf->head = (buf->head + 1) % BUFFER_SIZE;
    buf->count++;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->lock);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buf, log_entry_t *out) {
    pthread_mutex_lock(&buf->lock);
    while (buf->count == 0 && !buf->shutdown) {
        pthread_cond_wait(&buf->not_empty, &buf->lock);
    }
    if (buf->count == 0 && buf->shutdown) {
        pthread_mutex_unlock(&buf->lock);
        return -1;
    }
    *out = buf->pool[buf->tail];
    buf->tail = (buf->tail + 1) % BUFFER_SIZE;
    buf->count--;
    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->lock);
    return 0;
}

static void shutdown_logging(bounded_buffer_t *buf) {
    pthread_mutex_lock(&buf->lock);
    buf->shutdown = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->lock);
}

static void *logger_consumer(void *arg) {
    supervisor_ctx_t *ctx = arg;
    log_entry_t entry;
    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(&ctx->log_buffer, &entry) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, entry.id);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, entry.msg, strlen(entry.msg));
            close(fd);
        }
    }
    return NULL;
}

static void *container_reader_producer(void *arg) {
    child_config_t *cfg = arg;
    FILE *stream = fdopen(cfg->pipe_write_fd, "r");
    char line[MAX_LOG_LINE];

    while (stream && fgets(line, sizeof(line), stream)) {
        if (bounded_buffer_push(&g_ctx->log_buffer, cfg->id, line) < 0) {
            break;
        }
    }
    if (stream) {
        fclose(stream);
    }

    pthread_mutex_lock(&g_ctx->metadata_lock);
    for (container_record_t *r = g_ctx->containers; r; r = r->next) {
        if (strcmp(r->id, cfg->id) == 0) {
            r->producer_done = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_ctx->metadata_lock);
    free(cfg);
    return NULL;
}

static void write_cgroup(const char *subsystem, const char *id, const char *file, const char *val) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/mini_runtime/%s/%s", subsystem, id, file);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, val, strlen(val));
        close(fd);
    }
}

static void setup_cgroups(const char *id, pid_t pid) {
    char path[PATH_MAX];
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", pid);

    mkdir("/sys/fs/cgroup/mini_runtime", 0755);
    snprintf(path, sizeof(path), "/sys/fs/cgroup/mini_runtime/%s", id);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        return;
    }

    write_cgroup("memory", id, "memory.max", "64M");
    write_cgroup("cpu", id, "cpu.max", "25000 100000");
    write_cgroup("", id, "cgroup.procs", pid_str);
}

static void cleanup_cgroups(const char *id) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/fs/cgroup/memory/mini_runtime/%s", id);
    rmdir(path);
    snprintf(path, sizeof(path), "/sys/fs/cgroup/cpu/mini_runtime/%s", id);
    rmdir(path);
}

static void register_with_kernel(pid_t pid, const control_request_t *req) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        return;
    }

    struct monitor_request mreq;
    mreq.pid = pid;
    mreq.soft_limit_bytes = req->soft_limit_bytes;
    mreq.hard_limit_bytes = req->hard_limit_bytes;
    strncpy(mreq.container_id, req->container_id, MONITOR_NAME_LEN - 1);
    mreq.container_id[MONITOR_NAME_LEN - 1] = '\0';

    if (ioctl(fd, MONITOR_REGISTER, &mreq) < 0) {
        perror("monitor register");
    }
    close(fd);
}

static int launch_container(supervisor_ctx_t *ctx, const control_request_t *req) {
    int p[2];
    if (pipe(p) < 0) {
        return -1;
    }

    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    cfg->id[CONTAINER_ID_LEN - 1] = '\0';
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    cfg->rootfs[PATH_MAX - 1] = '\0';
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->command[CHILD_COMMAND_LEN - 1] = '\0';
    cfg->pipe_write_fd = p[1];

    void *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(p[0]);
        close(p[1]);
        return -1;
    }

    pid_t pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, cfg);
    if (pid < 0) {
        free(cfg);
        free(stack);
        close(p[0]);
        close(p[1]);
        return -1;
    }

    close(p[1]);

    if (req->nice != 0) {
        setpriority(PRIO_PROCESS, pid, req->nice);
    }

    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        close(p[0]);
        return -1;
    }

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->id[CONTAINER_ID_LEN - 1] = '\0';
    rec->host_pid = pid;
    rec->start_time = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->exit_code = -1;
    rec->term_signal = 0;
    rec->stop_requested = 0;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->nice = req->nice;
    rec->producer_done = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    child_config_t *producer_cfg = malloc(sizeof(*producer_cfg));
    if (!producer_cfg) {
        close(p[0]);
        return -1;
    }
    strncpy(producer_cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    producer_cfg->id[CONTAINER_ID_LEN - 1] = '\0';
    producer_cfg->pipe_write_fd = p[0];

    pthread_create(&rec->producer_tid, NULL, container_reader_producer, producer_cfg);

    setup_cgroups(req->container_id, pid);
    register_with_kernel(pid, req);

    return 0;
}

static int send_control_response(int fd, const control_response_t *res) {
    ssize_t total = 0;
    const char *buffer = (const char *)res;
    size_t remaining = sizeof(*res);
    while (remaining > 0) {
        ssize_t sent = send(fd, buffer + total, remaining, 0);
        if (sent <= 0) return -1;
        total += sent;
        remaining -= sent;
    }
    return 0;
}

static int recv_control_request(int fd, control_request_t *req) {
    ssize_t total = 0;
    char *buffer = (char *)req;
    size_t remaining = sizeof(*req);
    while (remaining > 0) {
        ssize_t got = recv(fd, buffer + total, remaining, MSG_WAITALL);
        if (got <= 0) return -1;
        total += got;
        remaining -= got;
    }
    return 0;
}

static void stop_container_by_id(supervisor_ctx_t *ctx, const char *id, control_response_t *res) {
    pthread_mutex_lock(&ctx->metadata_lock);
    for (container_record_t *r = ctx->containers; r; r = r->next) {
        if (strcmp(r->id, id) == 0) {
            if (r->state == CONTAINER_RUNNING) {
                r->stop_requested = 1;
                kill(r->host_pid, SIGTERM);
                snprintf(res->message, sizeof(res->message), "stopped '%s'", id);
                res->status = 0;
            } else {
                snprintf(res->message, sizeof(res->message), "container '%s' is not running", id);
                res->status = 1;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);
            return;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    snprintf(res->message, sizeof(res->message), "no such container '%s'", id);
    res->status = 1;
}

static void request_stop_all(supervisor_ctx_t *ctx) {
    pthread_mutex_lock(&ctx->metadata_lock);
    for (container_record_t *r = ctx->containers; r; r = r->next) {
        if (r->state == CONTAINER_RUNNING) {
            r->stop_requested = 1;
            kill(r->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void reap_children(supervisor_ctx_t *ctx) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        for (container_record_t *r = ctx->containers; r; r = r->next) {
            if (r->host_pid == pid) {
                if (WIFEXITED(status)) {
                    r->exit_code = WEXITSTATUS(status);
                    r->state = r->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    r->term_signal = WTERMSIG(status);
                    r->exit_code = 128 + r->term_signal;
                    if (r->stop_requested) {
                        r->state = CONTAINER_STOPPED;
                    } else if (r->term_signal == SIGKILL) {
                        r->state = CONTAINER_HARD_LIMIT_KILLED;
                    } else {
                        r->state = CONTAINER_STOPPED;
                    }
                }
                cleanup_cgroups(r->id);
                break;
            }
        }
        pthread_cond_broadcast(&ctx->metadata_cond);
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static void handle_signal_event(supervisor_ctx_t *ctx, int sig) {
    if (sig == SIGCHLD) {
        reap_children(ctx);
        return;
    }
    if (sig == SIGINT || sig == SIGTERM) {
        ctx->should_stop = 1;
        request_stop_all(ctx);
    }
}

static void signal_handler(int sig) {
    int saved_errno = errno;
    if (write(g_ctx->signal_pipe[1], &sig, sizeof(sig)) < 0) {
    }
    errno = saved_errno;
}

static int parse_mib(const char *arg, unsigned long *out) {
    char *end;
    long value = strtol(arg, &end, 10);
    if (end == arg || value < 0) return -1;
    *out = (unsigned long)value * 1024UL * 1024UL;
    return 0;
}

static int build_command_text(int argc, char *argv[], int start_index, char *buffer, size_t size, int *next_arg) {
    size_t len = 0;
    int i = start_index;
    while (i < argc && strncmp(argv[i], "--", 2) != 0) {
        int written = snprintf(buffer + len, size - len, "%s%s", len ? " " : "", argv[i]);
        if (written < 0 || (size_t)written >= size - len) return -1;
        len += written;
        i++;
    }
    if (next_arg) *next_arg = i;
    return 0;
}

static int parse_client_request(int argc, char *argv[], control_request_t *req) {
    memset(req, 0, sizeof(*req));
    req->soft_limit_bytes = 40UL * 1024UL * 1024UL;
    req->hard_limit_bytes = 64UL * 1024UL * 1024UL;
    req->nice = 0;

    if (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "run") == 0) {
        if (argc < 5) return -1;
        req->kind = strcmp(argv[1], "start") == 0 ? CMD_START : CMD_RUN;
        strncpy(req->container_id, argv[2], CONTAINER_ID_LEN - 1);
        req->container_id[CONTAINER_ID_LEN - 1] = '\0';
        strncpy(req->rootfs, argv[3], PATH_MAX - 1);
        req->rootfs[PATH_MAX - 1] = '\0';

        int next_arg = 0;
        if (build_command_text(argc, argv, 4, req->command, sizeof(req->command), &next_arg) < 0) {
            return -1;
        }

        int i = next_arg;
        while (i < argc) {
            if (strcmp(argv[i], "--soft-mib") == 0 && i + 1 < argc) {
                if (parse_mib(argv[i + 1], &req->soft_limit_bytes) < 0) return -1;
                i += 2;
            } else if (strcmp(argv[i], "--hard-mib") == 0 && i + 1 < argc) {
                if (parse_mib(argv[i + 1], &req->hard_limit_bytes) < 0) return -1;
                i += 2;
            } else if (strcmp(argv[i], "--nice") == 0 && i + 1 < argc) {
                req->nice = atoi(argv[i + 1]);
                i += 2;
            } else {
                return -1;
            }
        }
        return 0;
    }

    if (strcmp(argv[1], "ps") == 0) {
        req->kind = CMD_PS;
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0 && argc == 3) {
        req->kind = CMD_STOP;
        strncpy(req->container_id, argv[2], CONTAINER_ID_LEN - 1);
        req->container_id[CONTAINER_ID_LEN - 1] = '\0';
        return 0;
    }
    if (strcmp(argv[1], "logs") == 0 && argc == 3) {
        req->kind = CMD_LOGS;
        strncpy(req->container_id, argv[2], CONTAINER_ID_LEN - 1);
        req->container_id[CONTAINER_ID_LEN - 1] = '\0';
        return 0;
    }
    return -1;
}

static int send_control_request(const control_request_t *req, control_response_t *res) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (send(fd, req, sizeof(*req), 0) != sizeof(*req)) {
        close(fd);
        return -1;
    }

    ssize_t total = 0;
    char *buffer = (char *)res;
    size_t remaining = sizeof(*res);
    while (remaining > 0) {
        ssize_t got = recv(fd, buffer + total, remaining, 0);
        if (got <= 0) {
            close(fd);
            return -1;
        }
        total += got;
        remaining -= got;
    }

    close(fd);
    return 0;
}

static int client_send_stop_request(const char *container_id) {
    control_request_t req = {0};
    control_response_t res = {0};
    req.kind = CMD_STOP;
    strncpy(req.container_id, container_id, CONTAINER_ID_LEN - 1);
    req.container_id[CONTAINER_ID_LEN - 1] = '\0';
    if (send_control_request(&req, &res) == 0) {
        fprintf(stderr, "[engine] stop request sent: %s\n", res.message);
        return 0;
    }
    return -1;
}

static void interrupt_handler(int sig) {
    (void)sig;
    client_interrupt_requested = 1;
}

static int container_exists(supervisor_ctx_t *ctx, const char *id) {
    for (container_record_t *r = ctx->containers; r; r = r->next) {
        if (strcmp(r->id, id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int handle_request(supervisor_ctx_t *ctx, int fd) {
    control_request_t req;
    control_response_t res = {0};

    if (recv_control_request(fd, &req) < 0) {
        close(fd);
        return -1;
    }

    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        pthread_mutex_lock(&ctx->metadata_lock);
        int exists = container_exists(ctx, req.container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (exists) {
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "container '%s' already exists", req.container_id);
            send_control_response(fd, &res);
            close(fd);
            return -1;
        }

        if (launch_container(ctx, &req) < 0) {
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "failed to launch container '%s'", req.container_id);
            send_control_response(fd, &res);
            close(fd);
            return -1;
        }

        if (req.kind == CMD_START) {
            res.status = 0;
            snprintf(res.message, sizeof(res.message), "started '%s'", req.container_id);
            send_control_response(fd, &res);
            close(fd);
            return 0;
        }

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *record = NULL;
        for (container_record_t *r = ctx->containers; r; r = r->next) {
            if (strcmp(r->id, req.container_id) == 0) {
                record = r;
                break;
            }
        }
        while (record && record->state == CONTAINER_RUNNING) {
            pthread_cond_wait(&ctx->metadata_cond, &ctx->metadata_lock);
        }
        if (record) {
            res.status = 0;
            res.exit_code = record->exit_code;
            snprintf(res.message, sizeof(res.message), "container '%s' finished with %s (%d)", record->id, state_name(record->state), record->exit_code);
        } else {
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "container '%s' disappeared", req.container_id);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        send_control_response(fd, &res);
        close(fd);
        return 0;
    }

    if (req.kind == CMD_PS) {
        pthread_mutex_lock(&ctx->metadata_lock);
        int off = snprintf(res.message, sizeof(res.message), "%-16s %-8s %-18s %-10s %-10s\n", "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)");
        for (container_record_t *r = ctx->containers; r; r = r->next) {
            off += snprintf(res.message + off, sizeof(res.message) - off,
                            "%-16s %-8d %-18s %-10lu %-10lu\n",
                            r->id,
                            r->host_pid,
                            state_name(r->state),
                            r->soft_limit_bytes / (1024UL * 1024UL),
                            r->hard_limit_bytes / (1024UL * 1024UL));
            if (off >= (int)sizeof(res.message) - 1) break;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        res.status = 0;
        send_control_response(fd, &res);
        close(fd);
        return 0;
    }

    if (req.kind == CMD_LOGS) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
        int lf = open(path, O_RDONLY);
        if (lf < 0) {
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "failed to open logs for '%s'", req.container_id);
        } else {
            ssize_t read_bytes = read(lf, res.message, sizeof(res.message) - 1);
            if (read_bytes < 0) {
                res.status = 1;
                snprintf(res.message, sizeof(res.message), "failed to read logs for '%s'", req.container_id);
            } else {
                res.status = 0;
                res.message[read_bytes] = '\0';
            }
            close(lf);
        }
        send_control_response(fd, &res);
        close(fd);
        return 0;
    }

    if (req.kind == CMD_STOP) {
        stop_container_by_id(ctx, req.container_id, &res);
        send_control_response(fd, &res);
        close(fd);
        return 0;
    }

    res.status = 1;
    snprintf(res.message, sizeof(res.message), "unsupported command");
    send_control_response(fd, &res);
    close(fd);
    return -1;
}

static void wait_for_all_containers(supervisor_ctx_t *ctx) {
    pthread_mutex_lock(&ctx->metadata_lock);
    while (1) {
        int running = 0;
        for (container_record_t *r = ctx->containers; r; r = r->next) {
            if (r->state == CONTAINER_RUNNING) {
                running = 1;
                break;
            }
        }
        if (!running) break;
        pthread_cond_wait(&ctx->metadata_cond, &ctx->metadata_lock);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void join_producers(supervisor_ctx_t *ctx) {
    pthread_mutex_lock(&ctx->metadata_lock);
    for (container_record_t *r = ctx->containers; r; r = r->next) {
        if (r->producer_tid) {
            pthread_join(r->producer_tid, NULL);
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s supervisor <base-rootfs>\n", prog);
    fprintf(stderr, "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", prog);
    fprintf(stderr, "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", prog);
    fprintf(stderr, "  %s ps\n", prog);
    fprintf(stderr, "  %s logs <id>\n", prog);
    fprintf(stderr, "  %s stop <id>\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        supervisor_ctx_t ctx = {0};
        g_ctx = &ctx;
        pthread_mutex_init(&ctx.metadata_lock, NULL);
        pthread_cond_init(&ctx.metadata_cond, NULL);
        pthread_mutex_init(&ctx.log_buffer.lock, NULL);
        pthread_cond_init(&ctx.log_buffer.not_full, NULL);
        pthread_cond_init(&ctx.log_buffer.not_empty, NULL);

        if (pipe(ctx.signal_pipe) < 0) {
            perror("pipe");
            return 1;
        }
        fcntl(ctx.signal_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(ctx.signal_pipe[1], F_SETFL, O_NONBLOCK);

        struct sigaction sa = {0};
        sa.sa_handler = signal_handler;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGCHLD, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);

        pthread_create(&ctx.consumer_tid, NULL, logger_consumer, &ctx);

        unlink(CONTROL_PATH);
        ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (ctx.server_fd < 0) {
            perror("socket");
            return 1;
        }

        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
        if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind");
            return 1;
        }
        if (listen(ctx.server_fd, 8) < 0) {
            perror("listen");
            return 1;
        }

        struct pollfd fds[2];
        fds[0].fd = ctx.server_fd;
        fds[0].events = POLLIN;
        fds[1].fd = ctx.signal_pipe[0];
        fds[1].events = POLLIN;

        while (!ctx.should_stop) {
            int rc = poll(fds, 2, -1);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (fds[1].revents & POLLIN) {
                int sig;
                while (read(ctx.signal_pipe[0], &sig, sizeof(sig)) == sizeof(sig)) {
                    handle_signal_event(&ctx, sig);
                }
            }
            if (ctx.should_stop) break;
            if (fds[0].revents & POLLIN) {
                int client_fd = accept(ctx.server_fd, NULL, NULL);
                if (client_fd >= 0) {
                    handle_request(&ctx, client_fd);
                }
            }
        }

        request_stop_all(&ctx);
        wait_for_all_containers(&ctx);
        join_producers(&ctx);
        shutdown_logging(&ctx.log_buffer);
        pthread_join(ctx.consumer_tid, NULL);
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        return 0;
    }

    control_request_t req;
    if (parse_client_request(argc, argv, &req) < 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (req.kind == CMD_RUN) {
        strncpy(client_run_id, req.container_id, CONTAINER_ID_LEN - 1);
        client_run_id[CONTAINER_ID_LEN - 1] = '\0';
        struct sigaction sa = {0};
        sa.sa_handler = interrupt_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    control_response_t res = {0};
    if (req.kind == CMD_RUN) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            return 1;
        }
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(fd);
            return 1;
        }
        if (send(fd, &req, sizeof(req), 0) != sizeof(req)) {
            perror("send");
            close(fd);
            return 1;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        while (1) {
            if (client_interrupt_requested) {
                client_send_stop_request(client_run_id);
                client_interrupt_requested = 0;
            }
            int rc = poll(&pfd, 1, 500);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (rc == 0) continue;
            if (pfd.revents & POLLIN) {
                ssize_t total = 0;
                char *buffer = (char *)&res;
                size_t remaining = sizeof(res);
                while (remaining > 0) {
                    ssize_t got = recv(fd, buffer + total, remaining, 0);
                    if (got <= 0) {
                        perror("recv");
                        close(fd);
                        return 1;
                    }
                    total += got;
                    remaining -= got;
                }
                break;
            }
        }
        close(fd);
        if (res.status != 0) {
            fprintf(stderr, "%s\n", res.message);
            return 1;
        }
        printf("%s\n", res.message);
        return res.exit_code;
    }

    if (send_control_request(&req, &res) < 0) {
        fprintf(stderr, "failed to communicate with supervisor\n");
        return 1;
    }
    if (res.status != 0) {
        fprintf(stderr, "%s\n", res.message);
        return 1;
    }
    printf("%s\n", res.message);
    return 0;
}
