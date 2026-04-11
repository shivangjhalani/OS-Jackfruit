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
typedef struct {
    int fd;
    char log_path[PATH_MAX];
} reader_args_t;

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
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    char rootfs[PATH_MAX];
    int pipefd[2]; 
} container_args_t;

static supervisor_ctx_t *global_ctx = NULL;

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

    // Wait while buffer is full
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    // If shutdown, stop
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Insert item
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    // Signal consumer
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

    // Wait while buffer is empty
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    // If shutdown and nothing left
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Remove item
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    // Signal producer
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

    while (1) {
        // Get log from buffer
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0) {
            break;
        }

        // Build log file path
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "logs/%s.log", item.container_id);

        // Open log file
        FILE *f = fopen(path, "a");
        if (!f) {
            perror("fopen");
            continue;
        }

        // Write data
        fwrite(item.data, 1, item.length, f);
        fclose(f);
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

static void handle_sigchld(int sig) {
    (void)sig;

    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("Container with PID %d exited\n", pid);

        pthread_mutex_lock(&global_ctx->metadata_lock);

        container_record_t *curr = global_ctx->containers;
        while (curr) {
            if (curr->host_pid == pid) {
                curr->state = CONTAINER_EXITED;

                if (WIFEXITED(status)) {
                    curr->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    curr->exit_signal = WTERMSIG(status);
                    curr->state = CONTAINER_KILLED;
                }
                break;
            }
            curr = curr->next;
        }

        pthread_mutex_unlock(&global_ctx->metadata_lock);
    }
}

void handle_shutdown(int sig)
{
    (void)sig;
    printf("\nShutting down supervisor...\n");

    if (!global_ctx)
        exit(0);

    pthread_mutex_lock(&global_ctx->metadata_lock);

    container_record_t *curr = global_ctx->containers;

    while (curr) {
        printf("Stopping container %s (PID %d)\n",
               curr->id, curr->host_pid);

        kill(curr->host_pid, SIGTERM);
        curr = curr->next;
    }

    pthread_mutex_unlock(&global_ctx->metadata_lock);

    exit(0);
}

static int child_fn(void *arg) {
    
    container_args_t *args = (container_args_t *)arg;
    
    dup2(args->pipefd[1], STDOUT_FILENO);
    dup2(args->pipefd[1], STDERR_FILENO);
        
    close(args->pipefd[0]);
    close(args->pipefd[1]);
    printf("Inside container (namespaces)\n");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
    	perror("mount private failed");
    }

//    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) != 0) {
//        perror("bind mount failed");
//    }

   if (chroot(args->rootfs) != 0) {
     perror("chroot failed");
        return 1;
    }

    chdir("/");

    mkdir("/proc", 0555);

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc failed");
    }
    //fsync(STDOUT_FILENO);
    execl("/memory_hog", "/memory_hog", NULL);

    perror("execl failed");
    exit(1);
  //  free(args);
   // return 1;
}

void *pipe_reader_thread(void *arg)
{
    reader_args_t *rargs = (reader_args_t *)arg;
    int fd = rargs->fd;
//    free(arg);

    mkdir("logs",0755);
    FILE *f = fopen(rargs->log_path,"a");
    if (!f) {
        perror("fopen failed");
        close(fd);
        return NULL;
    }
    char buffer[1024];

    while (1) {
        ssize_t n = read(fd, buffer, sizeof(buffer));

    if (n <= 0) {
        break;
    }

    fwrite(buffer, 1, n, f);
        fflush(f); 
    }

    fclose(f);
    close(fd);
    free(rargs);
    printf("Finished writing logs\n");
    return NULL;
}

static pid_t start_container(supervisor_ctx_t *ctx,
                             const char *id,
                             const char *rootfs,
		 size_t soft_limit_bytes, size_t  hard_limit_bytes)
{
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        return -1;
    }

    char *stack_top = stack + STACK_SIZE;

    int flags =  CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    container_args_t *args = malloc(sizeof(container_args_t));
    if (!args) {
        perror("malloc");
        return -1;
    }

    strncpy(args->rootfs, rootfs, PATH_MAX - 1);
    args->rootfs[PATH_MAX - 1] = '\0';

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return -1;
    }

    args->pipefd[0] = pipefd[0];
    args->pipefd[1] = pipefd[1];

    pid_t pid = clone(child_fn, stack_top, flags, (void *)args);

    if (pid == -1) {
        perror("clone");
        return -1;
    }

    // Parent process
    close(pipefd[1]);
    //int actual_pid;
    //read(pipefd[0], &actual_pid, sizeof(actual_pid));
    pthread_t reader_thread;
    reader_args_t *rargs = malloc(sizeof(reader_args_t));
    rargs->fd = pipefd[0];

    mkdir("logs", 0755);
    snprintf(rargs->log_path, PATH_MAX, "logs/%s.log", id);

    pthread_create(&reader_thread, NULL, pipe_reader_thread, rargs);

    printf("Started container %s with PID %d\n", id, pid);

    // Create container record
    // Register with kernel monitor
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("open monitor device");
    } else {
        struct monitor_request req;

        req.pid = pid;
        req.soft_limit_bytes = soft_limit_bytes;
        req.hard_limit_bytes = hard_limit_bytes;

        strncpy(req.container_id, id, MONITOR_NAME_LEN - 1);
        req.container_id[MONITOR_NAME_LEN - 1] = '\0';

        if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
            perror("ioctl REGISTER");
        }

        close(fd);
    }
     // Metadata
    container_record_t *rec = malloc(sizeof(container_record_t));
    if (!rec) {
        perror("malloc");
        return -1;
    }

    memset(rec, 0, sizeof(*rec));

    strncpy(rec->id, id, sizeof(rec->id) - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = soft_limit_bytes;
    rec->hard_limit_bytes = hard_limit_bytes;

    snprintf(rec->log_path, sizeof(rec->log_path), "logs/%s.log", rec->id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    return pid;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    global_ctx = &ctx;
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    signal(SIGCHLD, handle_sigchld);
    signal(SIGTERM, handle_sigchld);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /*
     * TODO:
     *   1) open /dev/container_monitor
     *   2) create the control socket / FIFO / shared-memory channel
     *   3) install SIGCHLD / SIGINT / SIGTERM handling
     *   4) spawn the logger thread
     *   5) enter the supervisor event loop
     */
    printf("Supervisor started with base rootfs: %s\n", rootfs);

    #define FIFO_PATH "/tmp/container_fifo"
    mkfifo(FIFO_PATH, 0666);

    while (1) {
        int fd = open(FIFO_PATH, O_RDONLY);
    	if (fd >= 0) {
        	control_request_t req;
        	int n = read(fd, &req, sizeof(req));
        	if (n == sizeof(req)) {

            	    if (req.kind == CMD_START) {
                    	  printf("Starting container %s\n", req.container_id);
                    	  start_container(&ctx, req.container_id, req.rootfs,
				 req.soft_limit_bytes, req.hard_limit_bytes);
                    }

            	    else if (req.kind == CMD_PS) {
               		 pthread_mutex_lock(&ctx.metadata_lock);
                	 container_record_t *curr = ctx.containers;
                	 printf("ID\tPID\tSTATE\n");

                	 while (curr) {
			      const char *state_str =  (curr->state == CONTAINER_RUNNING) ? "RUNNING" : "STOPPED";
                    	      printf("%s\t%d\t%s\n", curr->id, curr->host_pid, state_str);
                    	      curr = curr->next;
                	 }

                	 pthread_mutex_unlock(&ctx.metadata_lock);
            	    }
		    
		    else if (req.kind == CMD_STOP) {
    		         pthread_mutex_lock(&ctx.metadata_lock);

    		         container_record_t *curr = ctx.containers;

    		         while (curr) {
        		     if (strcmp(curr->id, req.container_id) == 0) {
            		     printf("Stopping container %s\n", curr->id);
            		     kill(curr->host_pid, SIGTERM);
			     sleep(1);
            		     kill(curr->host_pid, SIGKILL);
            		     curr->state = CONTAINER_STOPPED;
            		     break;
        	             }
        	             curr = curr->next;
    		         }
    		         pthread_mutex_unlock(&ctx.metadata_lock);
		    }
		
		    else if (req.kind == CMD_RUN) {

    			printf("Running container %s in foreground\n", req.container_id);
    			pid_t pid = start_container(&ctx, req.container_id,
 req.rootfs, req.soft_limit_bytes, req.hard_limit_bytes);

    			if (pid > 0) {
                       	     int status;
        	       	     waitpid(pid, &status, 0);
        		     printf("Container %s finished\n", req.container_id);
    		        }
                    }

		    else if (req.kind == CMD_LOGS) {

    			pthread_mutex_lock(&ctx.metadata_lock);

    			container_record_t *curr = ctx.containers;

    			while (curr) {
        			if (strcmp(curr->id, req.container_id) == 0) {

            			printf("Logs for %s: %s\n", curr->id, curr->log_path);
            			break;
        			}
       		         	curr = curr->next;
    			}

    			pthread_mutex_unlock(&ctx.metadata_lock);
		    }

        	}
        	close(fd);
    	}
  }
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
    int fd = open("/tmp/container_fifo", O_WRONLY);
    if (fd < 0) {
        perror("open fifo");
        return 1;
    }

    ssize_t n = write(fd, req, sizeof(*req));
    if (n != sizeof(*req)) {
        perror("write failed");
        close(fd);
        return 1;
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

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
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

    if (strcmp(argv[1], "start") == 0) {
        return cmd_start(argc, argv);
    }

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
