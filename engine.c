/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 * - command-line shape is defined
 * - key runtime data structures are defined
 * - bounded-buffer skeleton is defined
 * - supervisor / client split is outlined
 *
 * Students are expected to design:
 * - the control-plane IPC implementation
 * - container lifecycle and metadata synchronization
 * - clone + namespace setup for each container
 * - producer/consumer behavior for log buffering
 * - signal handling and graceful shutdown
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

/* Global flag for graceful shutdown */
volatile sig_atomic_t keep_running = 1;
/*When you press Ctrl+C, it sets the keep_running to 0 and exits the execution of the supervisor*/
static void handle_signal(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) { //SIGINT = Interrupt, SIGTERM = Termination
        keep_running = 0;
    }
}

typedef enum {
    CMD_SUPERVISOR = 0, //process should boot up as the long-running daemon itself.
    CMD_START, //Triggers the spawn container logic
    CMD_RUN, //Tells the client to wait in the foreground until the container exits
    CMD_PS, //Read LL and send back the metadate to all the containers
    CMD_LOGS, //Request the supervisor to read specific log files and stream it
    CMD_STOP //Commands the supervisor to find a specific PID and send it a SIGKILL
} command_kind_t;

/*Helps track the lifecycle of the containers, used in PS command, logging and cleanup*/
typedef enum {
    CONTAINER_STARTING = 0, //Container is being created
    CONTAINER_RUNNING, //Container is actively running
    CONTAINER_STOPPED, // Gracefully stopped by user
    CONTAINER_KILLED, // Forcefully killed (SIGKILL or memory limit)
    CONTAINER_EXITED // Finished normally
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN]; //Name of the container (alpha, beta), used to identify containers
    pid_t host_pid; //actual Linux PID of the container process
    time_t started_at; //When container was launched , useful for logs and scheduling experiments
    container_state_t state; //Tracks current lifecycle stage
    unsigned long soft_limit_bytes; //Soft limit → warning only
    unsigned long hard_limit_bytes;// Hard limit → kill process
    int exit_code; //If container exits normally: give exit code
    int exit_signal; //If killed: give exit signal
    char log_path[PATH_MAX]; //Where logs are stored
    struct container_record *next; //Points to next container in list
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN]; //Which container generated this log
    size_t length; //Size of actual data inside data[]
    char data[LOG_CHUNK_SIZE]; //Actual log content
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY]; //Array storing log messages
    size_t head; //Points to next item to remove
    size_t tail; // Points to next free slot to insert
    size_t count; //Number of items currently in buffer
    int shutting_down; 
    pthread_mutex_t mutex; ////only one thread accesses buffer at a time, mutex
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

/* It’s a structure used to send commands from the CLI to the supervisor.*/
typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;
/*reply from the supervisor back to the CLI.*/
typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;
/*holds all the info needed to start a container process.*/
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;
/*main brain/state of the supervisor.*/
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
 * - block or fail according to your chosen policy when the buffer is full
 * - wake consumers correctly
 * - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    // Wait if the buffer is full, unless we are shutting down
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    // Abort if a shutdown was triggered while we were waiting
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Insert the item
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    // Wake up any waiting consumers
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 * - wait correctly while the buffer is empty
 * - return a useful status when shutdown is in progress
 * - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    // Wait if the buffer is empty, unless we are shutting down
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    // If we are shutting down AND the buffer is empty, time to exit
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Remove the item
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    // Wake up any waiting producers
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 * - remove log chunks from the bounded buffer
 * - route each chunk to the correct per-container log file
 * - exit cleanly when shutdown begins and pending work is drained
 */
static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;

    while (!ctx->should_stop) {
        log_item_t item;
        memset(&item, 0, sizeof(item));

        int rc = bounded_buffer_pop(&ctx->log_buffer, &item);

        if (rc == 0 && item.container_id[0] != '\0') {
            printf("[LOG] %s\n", item.container_id);
            fflush(stdout);
        }
    }

    return NULL;
}

static void log_event(supervisor_ctx_t *ctx,
                      const char *id,
                      const char *event)
{
    log_item_t item;
    memset(&item, 0, sizeof(item));

    snprintf(item.container_id,
             sizeof(item.container_id),
             "%s:%s",
             id,
             event);
    printf("DEBUG: pushing log %s:%s\n", id, event);

    bounded_buffer_push(&ctx->log_buffer, &item);
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 * - isolated PID / UTS / mount context
 * - chroot or pivot_root into rootfs
 * - working /proc inside container
 * - stdout / stderr redirected to the supervisor logging path
 * - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Apply nice priority before locking down the container */
    if (cfg->nice_value != 0) {
        nice(cfg->nice_value);
    }

    chdir(cfg->rootfs);
    chroot(cfg->rootfs);

    mount("proc", "/proc", "proc", 0, NULL);

    /* Disconnect standard input so the container can't steal the terminal */
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        close(null_fd);
    }

    /* Execute the command passed from the config */
    execl("/bin/sh", "sh", "-c", cfg->command, NULL);

    perror("execl");
    return 1;
}

static pid_t spawn_container(const control_request_t *req)
{
    char *stack = malloc(STACK_SIZE);
    child_config_t *cfg = malloc(sizeof(child_config_t));
    
    strcpy(cfg->rootfs, req->rootfs);
    strcpy(cfg->command, req->command);
    cfg->nice_value = req->nice_value;

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, cfg);
    
    if (pid < 0) {
        perror("clone");
    }
    return pid;
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

static void add_container(supervisor_ctx_t *ctx, const char *id, pid_t pid)
{
    container_record_t *rec = malloc(sizeof(container_record_t));
    if (!rec)
        return;

    memset(rec, 0, sizeof(*rec));

    strncpy(rec->id, id, CONTAINER_ID_LEN - 1);
    rec->host_pid = pid;
    rec->state = CONTAINER_RUNNING;
    rec->started_at = time(NULL);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 * 1) open /dev/container_monitor
 * 2) create the control socket / FIFO / shared-memory channel
 * 3) install SIGCHLD / SIGINT / SIGTERM handling
 * 4) spawn the logger thread
 * 5) enter the supervisor event loop
 * - accept control requests and update container state
 * - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        perror("bounded_buffer_init");
        return 1;
    }

    printf("Supervisor started for rootfs: %s\n", rootfs);

    /* create control socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    ctx.should_stop = 0;
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        perror("pthread_create");
        return 1;
    }

    /* Register signal handlers for graceful shutdown */
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; 
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Open the kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("Failed to open /dev/container_monitor");
    }

    /* serve CLI requests until SIGINT or SIGTERM is caught */
    while (keep_running) {
        
        /* 1. Check for dead containers (Zombie Reaper) */
        pthread_mutex_lock(&ctx.metadata_lock);
        container_record_t *c = ctx.containers;
        while (c) {
            if (c->state == CONTAINER_RUNNING) {
                int wstatus;
                if (waitpid(c->host_pid, &wstatus, WNOHANG) > 0) {
                    if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGKILL) {
                        c->state = CONTAINER_KILLED;
                        log_event(&ctx, c->id, "hard_limit_killed");
                    } else {
                        c->state = CONTAINER_EXITED;
                        log_event(&ctx, c->id, "exited");
                    }
                }
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);

        /* 2. Wait for CLI commands (SOCK_CLOEXEC prevents fd leaks to children) */
        int client_fd = accept4(ctx.server_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) break;
            continue;
        }

        control_request_t req;
        ssize_t n = recv(client_fd, &req, sizeof(req), 0);
        
        if (n <= 0) {
            close(client_fd);
            continue;
        }

        /* 3. Handle specific commands */
        if (req.kind == CMD_START) {
            pid_t new_pid = spawn_container(&req);
            if (new_pid > 0) {
                add_container(&ctx, req.container_id, new_pid);
                log_event(&ctx, req.container_id, "started");
                
                if (ctx.monitor_fd >= 0) {
                    register_with_monitor(ctx.monitor_fd, req.container_id, new_pid, req.soft_limit_bytes, req.hard_limit_bytes);
                }
            }
        } 
        else if (req.kind == CMD_STOP) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *cur = ctx.containers;
            while (cur) {
                if (strcmp(cur->id, req.container_id) == 0 && cur->state == CONTAINER_RUNNING) {
                    kill(cur->host_pid, SIGKILL);
                    cur->state = CONTAINER_KILLED;
                    log_event(&ctx, cur->id, "killed");
                    break;
                }
                cur = cur->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        /* 4. Always send container list back to client */
        pthread_mutex_lock(&ctx.metadata_lock);
        container_record_t *cur = ctx.containers;
        while (cur) {
            dprintf(client_fd, "ID=%s PID=%d STATE=%s\n", cur->id, cur->host_pid, state_to_string(cur->state));
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
        close(client_fd);
    }

    /* =========================================
     * TEARDOWN AND CLEANUP PHASE
     * ========================================= */
    printf("\nShutting down supervisor...\n");

    ctx.should_stop = 1;
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *cur = ctx.containers;
    while (cur) {
        if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
            printf("Terminating lingering container: %s (PID: %d)\n", cur->id, cur->host_pid);
            kill(cur->host_pid, SIGKILL);
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    printf("Cleanup complete. Exiting.\n");
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
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    /* Send the full struct over IPC so the supervisor gets all arguments */
    write(fd, req, sizeof(control_request_t));

    char buffer[512];
    ssize_t n;

    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        write(STDOUT_FILENO, buffer, n);
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

static int cmd_stop(const char *id)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));

    req.kind = CMD_STOP;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);

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

    if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
            return 1;
        }
        return cmd_stop(argv[2]);
    }

    usage(argv[0]);
    return 1;
}
