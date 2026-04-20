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
    int stop_requested;
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
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

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

    /* wait while full */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    /* if shutting down, stop */
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* insert item */
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    /* signal consumers */
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

    /* wait while empty */
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    /* if shutdown and empty → exit */
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* remove item */
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    /* signal producers */
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

    while (1) {
        log_item_t item;

        int ret = bounded_buffer_pop(&ctx->log_buffer, &item);

        /* exit condition */
        if (ret < 0) {
            break;
        }

        /* build file path */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        /* open file */
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("open log file");
            continue;
        }

        /* write full chunk (IMPORTANT: loop for safety) */
        size_t total = 0;
        while (total < item.length) {
            ssize_t written = write(fd, item.data + total, item.length - total);
            if (written < 0) {
                perror("write");
                break;
            }
            total += written;
        }

        close(fd);
    }

    return NULL;
}

static void *log_reader(void *arg)
{
    void **args = (void **)arg;

    int read_fd = *(int *)args[0];
    char *container_id = (char *)args[1];
    bounded_buffer_t *buffer = (bounded_buffer_t *)args[2];

    free(args[0]);
    free(args);

    char buf[LOG_CHUNK_SIZE];

    while (1) {
        ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        size_t offset = 0;

        while (offset < (size_t)n) {
            size_t chunk = n - offset;
            if (chunk > LOG_CHUNK_SIZE)
                chunk = LOG_CHUNK_SIZE;

            log_item_t item;
            memset(&item, 0, sizeof(item));

            strncpy(item.container_id, container_id,
                    sizeof(item.container_id) - 1);

            memcpy(item.data, buf + offset, chunk);
            item.length = chunk;

            bounded_buffer_push(buffer, &item);

            offset += chunk;
        }
    }

    close(read_fd);
    free(container_id);
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

    /* 1. set hostname */
    sethostname(config->id, strlen(config->id));

    /* 2. change root filesystem */
    if (chroot(config->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    /* 3. mount /proc */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
        return 1;
    }

    /* 4. set nice value */
    if (config->nice_value != 0) {
        nice(config->nice_value);
    }

    /* 5. execute command */
    dup2(config->log_write_fd, STDOUT_FILENO);
    dup2(config->log_write_fd, STDERR_FILENO);
    close(config->log_write_fd);

    /* execute command directly */
    execlp("sh", "sh", "-c", config->command, NULL);

    perror("execvp");
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

static volatile sig_atomic_t stop_flag = 0;

static void handle_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    ctx.should_stop = 0;
    ctx.containers = NULL;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* ================= PHASE 1 IMPLEMENTATION ================= */

    /* 1) open /dev/container_monitor */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
        goto cleanup;
    }

    /* 2) create control socket */
    struct sockaddr_un addr;

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int flags = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }
    chmod(CONTROL_PATH, 0666);

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        goto cleanup;
    }

    /* 3) install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    /* ignore SIGPIPE (important for sockets) */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    /* allow normal child handling */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL);

    /* handle Ctrl+C / termination properly */
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 4) spawn logging thread (will implement later) */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        goto cleanup;
    }

    /* 5) supervisor event loop */
    while (!ctx.should_stop) {
        if (stop_flag) {
            ctx.should_stop = 1;
            break;
        }
        /* 🔥 ALWAYS reap children FIRST */
        int status;
        pid_t pid;

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        
            pthread_mutex_lock(&ctx.metadata_lock);
        
            container_record_t *cur = ctx.containers;
        
            while (cur) {
                if (cur->host_pid == pid) {
                
                    if (WIFEXITED(status)) {
                        cur->state = CONTAINER_EXITED;
                        cur->exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        cur->exit_signal = WTERMSIG(status);

                        if (cur->stop_requested) {
                            cur->state = CONTAINER_STOPPED;
                        } else {
                            cur->state = CONTAINER_KILLED;
                        }
                    }
                
                    if (unregister_from_monitor(ctx.monitor_fd,
                                                cur->id,
                                                cur->host_pid) < 0) {
                        if (errno != ENOENT) {
                            perror("unregister_from_monitor");
                        }
                    }
                
                    break;
                }
                cur = cur->next;
            }
        
            pthread_mutex_unlock(&ctx.metadata_lock);
        }
        int client_fd = accept(ctx.server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            if (errno == EINTR)
                continue;
        
            perror("accept");
            break;
        }

        /* For now: just close (no command handling yet) */
        control_request_t req;
        control_response_t resp;

        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));

        ssize_t n = read(client_fd, &req, sizeof(req));

        if (n <= 0) {
            close(client_fd);
            continue;
        }

        /* basic handling */
        switch (req.kind) {

            case CMD_START: {

                /* 1. create pipe for logging */
                int pipefd[2];
                if (pipe(pipefd) < 0) {
                    perror("pipe");
                    snprintf(resp.message, sizeof(resp.message), "pipe failed");
                    resp.status = 1;
                    break;
                }

                /* 2. allocate stack */
                void *stack = malloc(STACK_SIZE);
                if (!stack) {
                    snprintf(resp.message, sizeof(resp.message), "malloc failed");
                    resp.status = 1;
                    close(pipefd[0]);
                    close(pipefd[1]);
                    break;
                }

                void *stack_top = (char *)stack + STACK_SIZE;

                /* 3. prepare child config */
                child_config_t config;
                memset(&config, 0, sizeof(config));

                strncpy(config.id, req.container_id, sizeof(config.id) - 1);
                strncpy(config.rootfs, req.rootfs, sizeof(config.rootfs) - 1);
                strncpy(config.command, req.command, sizeof(config.command) - 1);
                config.nice_value = req.nice_value;

                /* pass pipe write end */
                config.log_write_fd = pipefd[1];

                /* 4. clone container */
                int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

                pid_t pid = clone(child_fn, stack_top, flags, &config);

                if (pid < 0) {
                    perror("clone");
                    snprintf(resp.message, sizeof(resp.message), "clone failed");
                    resp.status = 1;

                    free(stack);
                    close(pipefd[0]);
                    close(pipefd[1]);
                    break;
                }

                /* parent does not write */
                close(pipefd[1]);

                /* 5. fork log reader process */
                /* create log reader thread */
                pthread_t log_thread;

                /* prepare arguments */
                void **args = malloc(3 * sizeof(void *));
                if (!args) {
                    snprintf(resp.message, sizeof(resp.message), "alloc failed");
                    resp.status = 1;
                    break;
                }

                int *fd_ptr = malloc(sizeof(int));
                if (!fd_ptr) {
                    free(args);
                    snprintf(resp.message, sizeof(resp.message), "alloc failed");
                    resp.status = 1;
                    break;
                }

                *fd_ptr = pipefd[0];

                args[0] = fd_ptr;
                char *id_copy = strdup(req.container_id);
                if (!id_copy) {
                    free(fd_ptr);
                    free(args);
                    snprintf(resp.message, sizeof(resp.message), "alloc failed");
                    resp.status = 1;
                    break;
                }

                args[1] = id_copy;
                args[2] = &ctx.log_buffer;

                if (pthread_create(&log_thread, NULL, log_reader, args) != 0) {
                    perror("pthread_create");
                    free(fd_ptr);
                    free(args);
                    close(pipefd[0]);
                    break;
                }

                pthread_detach(log_thread);

                /* 6. create metadata */
                container_record_t *rec = malloc(sizeof(container_record_t));
                if (!rec) {
                    snprintf(resp.message, sizeof(resp.message), "metadata alloc failed");
                    resp.status = 1;
                    break;
                }

                memset(rec, 0, sizeof(*rec));

                strncpy(rec->id, req.container_id, sizeof(rec->id) - 1);
                rec->host_pid = pid;
                rec->started_at = time(NULL);
                rec->state = CONTAINER_RUNNING;

                /* set limits FIRST */
                rec->soft_limit_bytes = req.soft_limit_bytes;
                rec->hard_limit_bytes = req.hard_limit_bytes;

                /* fallback if zero */
                if (rec->soft_limit_bytes == 0)
                    rec->soft_limit_bytes = DEFAULT_SOFT_LIMIT;

                if (rec->hard_limit_bytes == 0)
                    rec->hard_limit_bytes = DEFAULT_HARD_LIMIT;

                /* NOW register */
                if (register_with_monitor(ctx.monitor_fd,
                                          rec->id,
                                          rec->host_pid,
                                          rec->soft_limit_bytes,
                                          rec->hard_limit_bytes) < 0) {
                    perror("register_with_monitor");
                }
                rec->started_at = time(NULL);
                rec->state = CONTAINER_RUNNING;

                snprintf(rec->log_path, sizeof(rec->log_path),
                         "%s/%s.log", LOG_DIR, rec->id);

                int log_fd = open(rec->log_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (log_fd < 0) {
                    perror("open log file");
                } else {
                    close(log_fd);
                }

                /* insert into list */
                pthread_mutex_lock(&ctx.metadata_lock);
                rec->next = ctx.containers;
                ctx.containers = rec;
                pthread_mutex_unlock(&ctx.metadata_lock);

                /* response */
                snprintf(resp.message, sizeof(resp.message),
                         "Container %s started (PID %d)", rec->id, pid);
                resp.status = 0;
                break;
            }

            case CMD_PS: {
                pthread_mutex_lock(&ctx.metadata_lock);

                char buffer[CONTROL_MESSAGE_LEN] = {0};
                size_t offset = 0;

                container_record_t *cur = ctx.containers;

                while (cur) {
                    const char *state_str = "unknown";
                
                    switch (cur->state) {
                        case CONTAINER_STARTING: state_str = "starting"; break;
                        case CONTAINER_RUNNING:  state_str = "running";  break;
                        case CONTAINER_STOPPED:  state_str = "stopped";  break;
                        case CONTAINER_KILLED:   state_str = "killed";   break;
                        case CONTAINER_EXITED:   state_str = "exited";   break;
                    }
                
                    int written = snprintf(buffer + offset,
                                           sizeof(buffer) - offset,
                                           "%s (PID %d) [%s]\n",
                                           cur->id,
                                           cur->host_pid,
                                           state_str);
                    
                    if (written < 0 || offset + written >= sizeof(buffer)) {
                        break;  // prevent overflow
                    }
                
                    offset += written;
                    cur = cur->next;
                }
            
                if (offset == 0) {
                    snprintf(buffer, sizeof(buffer), "No containers\n");
                }
            
                strncpy(resp.message, buffer, sizeof(resp.message) - 1);
                resp.status = 0;
            
                pthread_mutex_unlock(&ctx.metadata_lock);
                break;
            }

            case CMD_STOP: {
                pthread_mutex_lock(&ctx.metadata_lock);

                container_record_t *cur = ctx.containers;

                while (cur) {
                    if (strcmp(cur->id, req.container_id) == 0) {
                    
                        /* mark that this was a user-requested stop */
                        cur->stop_requested = 1;
                    
                        /* kill container */
                        kill(cur->host_pid, SIGKILL);
                    
                        snprintf(resp.message, sizeof(resp.message),
                                 "Container %s stopped", cur->id);
                        resp.status = 0;
                    
                        pthread_mutex_unlock(&ctx.metadata_lock);
                        break;
                    }
                    cur = cur->next;
                }
            
                if (!cur) {
                    snprintf(resp.message, sizeof(resp.message),
                             "Container not found");
                    resp.status = 1;
                    pthread_mutex_unlock(&ctx.metadata_lock);
                }
            
                break;
            }

            case CMD_RUN: {

                /* 1. create pipe for logging */
                int pipefd[2];
                if (pipe(pipefd) < 0) {
                    perror("pipe");
                    snprintf(resp.message, sizeof(resp.message), "pipe failed");
                    resp.status = 1;
                    break;
                }
            
                /* 2. allocate stack */
                void *stack = malloc(STACK_SIZE);
                if (!stack) {
                    snprintf(resp.message, sizeof(resp.message), "malloc failed");
                    resp.status = 1;
                    close(pipefd[0]);
                    close(pipefd[1]);
                    break;
                }
            
                void *stack_top = (char *)stack + STACK_SIZE;
            
                /* 3. prepare child config */
                child_config_t config;
                memset(&config, 0, sizeof(config));
            
                strncpy(config.id, req.container_id, sizeof(config.id) - 1);
                strncpy(config.rootfs, req.rootfs, sizeof(config.rootfs) - 1);
                strncpy(config.command, req.command, sizeof(config.command) - 1);
                config.nice_value = req.nice_value;
                config.log_write_fd = pipefd[1];
            
                int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
            
                pid_t pid = clone(child_fn, stack_top, flags, &config);
            
                if (pid < 0) {
                    perror("clone");
                    snprintf(resp.message, sizeof(resp.message), "clone failed");
                    resp.status = 1;
                    free(stack);
                    close(pipefd[0]);
                    close(pipefd[1]);
                    break;
                }
            
                close(pipefd[1]);
            
                /* logging thread (same as start) */
                pthread_t log_thread;
            
                void **args = malloc(3 * sizeof(void *));
                int *fd_ptr = malloc(sizeof(int));
                char *id_copy = strdup(req.container_id);
            
                *fd_ptr = pipefd[0];
                args[0] = fd_ptr;
                args[1] = id_copy;
                args[2] = &ctx.log_buffer;
            
                pthread_create(&log_thread, NULL, log_reader, args);
                pthread_detach(log_thread);
            
                /* metadata (optional but good) */
                container_record_t *rec = malloc(sizeof(container_record_t));
                memset(rec, 0, sizeof(*rec));
            
                strncpy(rec->id, req.container_id, sizeof(rec->id) - 1);
                rec->host_pid = pid;
                rec->started_at = time(NULL);
                rec->state = CONTAINER_RUNNING;
            
                /* register with kernel */
                if (register_with_monitor(ctx.monitor_fd,
                                          rec->id,
                                          rec->host_pid,
                                          req.soft_limit_bytes,
                                          req.hard_limit_bytes) < 0) {
                    perror("register_with_monitor");
                }
            
                /* 🔥 BLOCK HERE */
                int status;
                waitpid(pid, &status, 0);
            
                /* update state */
                if (WIFEXITED(status)) {
                    rec->state = CONTAINER_EXITED;
                    rec->exit_code = WEXITSTATUS(status);
                    snprintf(resp.message, sizeof(resp.message),
                             "Exited with code %d", rec->exit_code);
                } else if (WIFSIGNALED(status)) {
                    rec->exit_signal = WTERMSIG(status);

                    if (rec->stop_requested) {
                        rec->state = CONTAINER_STOPPED;
                    } else {
                        rec->state = CONTAINER_KILLED;
                    }
                }
            
                /* unregister */
                if (unregister_from_monitor(ctx.monitor_fd,
                                            rec->id,
                                            rec->host_pid) < 0) {
                    if (errno != ENOENT)
                        perror("unregister_from_monitor");
                }
            
                resp.status = 0;
                free(rec);
                break;
            }

            case CMD_LOGS: {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);

                int fd = open(path, O_RDONLY);
                if (fd < 0) {
                    snprintf(resp.message, sizeof(resp.message),
                             "Log file not found for container %s",
                             req.container_id);
                    resp.status = 1;
                    write(client_fd, &resp, sizeof(resp));
                    break;
                }
            
                /* send success response first */
                snprintf(resp.message, sizeof(resp.message), "OK");
                resp.status = 0;
                write(client_fd, &resp, sizeof(resp));
            
                /* now stream file contents */
                char buffer[4096];
                ssize_t n;
            
                while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
                    ssize_t total = 0;
                    while (total < n) {
                        ssize_t written = write(client_fd, buffer + total, n - total);
                        if (written < 0) {
                            perror("write logs");
                            break;
                        }
                        total += written;
                    }
                }
            
                close(fd);
                close(client_fd);  // IMPORTANT: close after streaming
                continue;          // skip normal response handling
            }

            default:
                snprintf(resp.message, sizeof(resp.message),
                        "Unknown command");
                resp.status = 1;
                break;
        }

        /* send response */
        write(client_fd, &resp, sizeof(resp));

        close(client_fd);
        
    }
    /* 🔥 kill all running containers on shutdown */
    pthread_mutex_lock(&ctx.metadata_lock);

    container_record_t *cur = ctx.containers;

    while (cur) {
        if (cur->state == CONTAINER_RUNNING) {
            kill(cur->host_pid, SIGKILL);
        }
        cur = cur->next;
    }

    pthread_mutex_unlock(&ctx.metadata_lock);

cleanup:
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    return 1;
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

    /* send request */
    ssize_t n = write(fd, req, sizeof(*req));
    if (n != sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    /* receive response */
    n = read(fd, &resp, sizeof(resp));
    if (n > 0) {
        printf("%s\n", resp.message);   // 🔥 ALWAYS PRINT
        if (resp.status != 0) {
            close(fd);
            return 1;
        }
    }
    
    /* now read full log stream */
    /* only stream logs for logs command */
    if (req->kind == CMD_LOGS) {
        char buffer[4096];
        while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
            write(STDOUT_FILENO, buffer, n);
        }
    }

    close(fd);
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
