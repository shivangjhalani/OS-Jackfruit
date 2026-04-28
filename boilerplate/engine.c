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
#include <sys/ioctl.h>
#include <sys/mman.h>
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
    int should_stop;
    pthread_t logger_thread;
    pthread_t reaper_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    pthread_cond_t state_changed;
    container_record_t *containers;
} supervisor_ctx_t;

static volatile sig_atomic_t g_should_stop = 0;
static int g_sigchld_pipe[2];

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid);
int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid,
                          unsigned long soft_limit_bytes, unsigned long hard_limit_bytes);

static void handle_shutdown(int sig)
{
    (void)sig;
    g_should_stop = 1;
    char c = 'S';
    if (write(g_sigchld_pipe[1], &c, 1) < 0) {}
}

static void handle_sigchld(int sig)
{
    (void)sig;
    char c = 'C';
    if (write(g_sigchld_pipe[1], &c, 1) < 0) {}
}

void *reaper_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    char c;
    while (!g_should_stop) {
        if (read(g_sigchld_pipe[0], &c, 1) > 0) {
            int status;
            pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *curr = ctx->containers;
                while (curr) {
                    if (curr->host_pid == pid) {
                        curr->state = CONTAINER_EXITED;
                        curr->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                        curr->exit_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
                        if (ctx->monitor_fd >= 0) {
                            unregister_from_monitor(ctx->monitor_fd, curr->id, curr->host_pid);
                        }
                        break;
                    }
                    curr = curr->next;
                }
                pthread_cond_broadcast(&ctx->state_changed);
                pthread_mutex_unlock(&ctx->metadata_lock);
            }
        }
    }
    return NULL;
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

    // Wait while full. Using a while loop handles "spurious wakeups"
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    // If we woke up because of shutdown, bail out
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Insert the item
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    // Wake up any consumers waiting for data
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

    // Wait while empty
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    // If shutting down and the buffer is completely drained, bail out
    if (buffer->shutting_down && buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Remove the item
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    // Wake up any producers waiting for space
    pthread_cond_signal(&buffer->not_full);

    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_ctx_t;

// PRODUCER: Reads from the container's pipe
void *producer_thread(void *arg)
{
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    log_item_t item;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, ctx->container_id, CONTAINER_ID_LEN - 1);

    while (1) {
        ssize_t n = read(ctx->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0)
            break; // Pipe closed (container exited) or error

        item.length = n;
        // Push blocks if full. Returns -1 if shutdown begins.
        if (bounded_buffer_push(ctx->buffer, &item) < 0) {
            break;
        }
    }

    close(ctx->read_fd);
    free(ctx);
    return NULL;
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

    // Ensure the logs directory exists
    mkdir(LOG_DIR, 0755);

    // Pop blocks if empty. Returns -1 if shutting down AND buffer is empty.
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        // O_APPEND ensures we don't overwrite previous chunks
        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
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
    child_config_t *config = (child_config_t *)arg;

    // 1. Isolate the hostname
    if (sethostname(config->id, strlen(config->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    // 2. Isolate the filesystem to the container's rootfs
    if (chroot(config->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    // 3. Mount /proc so tools like 'ps' work inside the container
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    // Apply priority
    if (nice(config->nice_value) == -1 && errno != 0) {
        perror("nice");
    }

    // 4. Redirect stdout (1) and stderr (2) to the write-end of the pipe
    dup2(config->log_write_fd, STDOUT_FILENO);
    dup2(config->log_write_fd, STDERR_FILENO);
    close(config->log_write_fd);

    // 5. Execute the command using the shell
    execl("/bin/sh", "sh", "-c", config->command, NULL);

    // If we reach here, execl failed
    perror("execl");
    return 1;
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
typedef struct {
    int client_fd;
    supervisor_ctx_t *ctx;
} client_ctx_t;

void *client_thread(void *arg);

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    if (pipe(g_sigchld_pipe) < 0) {
        perror("pipe");
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
    }

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = pthread_cond_init(&ctx.state_changed, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_cond_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    // Install signal handlers
    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_shutdown;
    sa_term.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    struct sockaddr_un addr;

    // Clean up any stale socket file
    unlink(CONTROL_PATH);

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(ctx.server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    printf("Supervisor listening on %s\n", CONTROL_PATH);

    // Spawn the logger thread
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logging_thread");
        return 1;
    }

    if (pthread_create(&ctx.reaper_thread, NULL, reaper_thread, &ctx) != 0) {
        perror("pthread_create reaper_thread");
        return 1;
    }

    while (!g_should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        client_ctx_t *cctx = malloc(sizeof(client_ctx_t));
        if (cctx) {
            cctx->client_fd = client_fd;
            cctx->ctx = &ctx;
            pthread_t pt;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_create(&pt, &attr, client_thread, cctx);
            pthread_attr_destroy(&attr);
        } else {
            close(client_fd);
        }
    }

    printf("Supervisor shutting down...\n");
    bounded_buffer_begin_shutdown(&ctx.log_buffer);

    // Wake up reaper thread
    char c = 'S';
    if (write(g_sigchld_pipe[1], &c, 1) < 0) {}

    pthread_join(ctx.logger_thread, NULL);
    pthread_join(ctx.reaper_thread, NULL);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_cond_destroy(&ctx.state_changed);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.monitor_fd >= 0) {
        close(ctx.monitor_fd);
    }
    close(g_sigchld_pipe[0]);
    close(g_sigchld_pipe[1]);
    unlink(CONTROL_PATH);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
void *client_thread(void *arg)
{
    client_ctx_t *cctx = (client_ctx_t *)arg;
    int client_fd = cctx->client_fd;
    supervisor_ctx_t *ctx = cctx->ctx;
    free(cctx);

    control_request_t req;
    control_response_t res;
    memset(&res, 0, sizeof(res));

    if (read(client_fd, &req, sizeof(req)) > 0) {
        if (req.kind == CMD_START || req.kind == CMD_RUN) {
            // 1. Create a pipe for the container's output
            int pipefd[2];
            if (pipe(pipefd) < 0) {
                perror("pipe");
                res.status = 1;
                snprintf(res.message, sizeof(res.message), "Failed to create pipe.");
                goto send_resp;
            }

            // 2. Allocate the stack for the child using mmap
            void *child_stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
            if (child_stack == MAP_FAILED) {
                perror("mmap");
                res.status = 1;
                snprintf(res.message, sizeof(res.message), "Failed to allocate stack.");
                close(pipefd[0]);
                close(pipefd[1]);
                goto send_resp;
            }

            void *stack_top = (char *)child_stack + STACK_SIZE;

            // 3. Prepare the config for the child process
            child_config_t *config = malloc(sizeof(child_config_t));
            if (!config) {
                res.status = 1;
                snprintf(res.message, sizeof(res.message), "Allocation failed.");
                munmap(child_stack, STACK_SIZE);
                close(pipefd[0]);
                close(pipefd[1]);
                goto send_resp;
            }

            strncpy(config->id, req.container_id, CONTAINER_ID_LEN - 1);
            strncpy(config->rootfs, req.rootfs, PATH_MAX - 1);
            strncpy(config->command, req.command, CHILD_COMMAND_LEN - 1);
            config->nice_value = req.nice_value;
            config->log_write_fd = pipefd[1];

            // 4. Call clone with namespace isolation flags
            int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
            pid_t child_pid = clone(child_fn, stack_top, clone_flags, config);

            if (child_pid < 0) {
                perror("clone");
                res.status = 1;
                snprintf(res.message, sizeof(res.message), "Clone failed.");
                free(config);
                munmap(child_stack, STACK_SIZE);
                close(pipefd[0]);
                close(pipefd[1]);
            } else {
                // Parent closes the write end; keep the read end for logging
                close(pipefd[1]);

                // Spawn a producer thread to read from this container's pipe
                producer_ctx_t *pctx = malloc(sizeof(producer_ctx_t));
                if (pctx) {
                    pctx->read_fd = pipefd[0];
                    strncpy(pctx->container_id, req.container_id, CONTAINER_ID_LEN - 1);
                    pctx->buffer = &ctx->log_buffer;

                    pthread_t pt;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                    pthread_create(&pt, &attr, producer_thread, pctx);
                    pthread_attr_destroy(&attr);
                } else {
                    close(pipefd[0]);
                }

                if (ctx->monitor_fd >= 0) {
                    if (register_with_monitor(ctx->monitor_fd, req.container_id, child_pid, req.soft_limit_bytes, req.hard_limit_bytes) < 0) {
                        perror("register_with_monitor");
                    }
                }

                // 5. Add to our linked list safely
                container_record_t *rec = malloc(sizeof(container_record_t));
                if (rec) {
                    memset(rec, 0, sizeof(*rec));
                    strncpy(rec->id, req.container_id, CONTAINER_ID_LEN - 1);
                    rec->host_pid = child_pid;
                    rec->started_at = time(NULL);
                    rec->state = CONTAINER_RUNNING;
                    rec->soft_limit_bytes = req.soft_limit_bytes;
                    rec->hard_limit_bytes = req.hard_limit_bytes;
                    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req.container_id);

                    pthread_mutex_lock(&ctx->metadata_lock);
                    rec->next = ctx->containers;
                    ctx->containers = rec;
                    pthread_mutex_unlock(&ctx->metadata_lock);
                }

                if (req.kind == CMD_RUN) {
                    pthread_mutex_lock(&ctx->metadata_lock);
                    while (rec && (rec->state == CONTAINER_STARTING || rec->state == CONTAINER_RUNNING)) {
                        pthread_cond_wait(&ctx->state_changed, &ctx->metadata_lock);
                    }
                    res.status = rec ? rec->exit_code : 1;
                    pthread_mutex_unlock(&ctx->metadata_lock);
                    snprintf(res.message, sizeof(res.message), "Container %s finished with status %d.", req.container_id, res.status);
                } else {
                    res.status = 0;
                    snprintf(res.message, sizeof(res.message), "Container %s started with PID %d.", req.container_id, child_pid);
                }
            }
        } else if (req.kind == CMD_PS) {
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *curr = ctx->containers;
            int offset = snprintf(res.message, sizeof(res.message), "%-15s %-10s %-15s\n", "ID", "PID", "STATE");

            while (curr && offset < (int)sizeof(res.message) - 1) {
                offset += snprintf(res.message + offset, sizeof(res.message) - offset,
                                   "%-15s %-10d %-15s\n",
                                   curr->id, curr->host_pid, state_to_string(curr->state));
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);
            res.status = 0;

        } else if (req.kind == CMD_STOP) {
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *curr = ctx->containers;
            int found = 0;

            while (curr) {
                if (strcmp(curr->id, req.container_id) == 0) {
                    found = 1;
                    if (curr->state == CONTAINER_RUNNING) {
                        if (kill(curr->host_pid, SIGTERM) == 0) {
                            curr->state = CONTAINER_STOPPED;
                            snprintf(res.message, sizeof(res.message), "Sent SIGTERM to container %s (PID %d).", curr->id, curr->host_pid);
                            res.status = 0;
                        } else {
                            perror("kill");
                            snprintf(res.message, sizeof(res.message), "Failed to stop container.");
                            res.status = 1;
                        }
                    } else {
                        snprintf(res.message, sizeof(res.message), "Container is not running.");
                        res.status = 1;
                    }
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            if (!found) {
                snprintf(res.message, sizeof(res.message), "Container %s not found.", req.container_id);
                res.status = 1;
            }

        } else if (req.kind == CMD_LOGS) {
            snprintf(res.message, sizeof(res.message), "Logs for %s are located at: %s/%s.log", req.container_id, LOG_DIR, req.container_id);
            res.status = 0;
        } else {
            snprintf(res.message, sizeof(res.message), "Unknown command.");
            res.status = 1;
        }
    }

send_resp:
    write(client_fd, &res, sizeof(res));
    close(client_fd);
    return NULL;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t res;

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

    if (write(fd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    if (read(fd, &res, sizeof(res)) > 0) {
        printf("%s\n", res.message);
    } else {
        res.status = 1;
    }

    close(fd);
    return res.status;
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

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
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
