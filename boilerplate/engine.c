/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Implements:
 *  - Supervisor daemon (engine supervisor <base-rootfs>) listening on CONTROL_PATH
 *  - CLI client commands: start, run, ps, logs, stop
 *  - Minimal container isolation using PID/UTS/mount namespaces + chroot + /proc
 *  - Per-container log files under LOG_DIR
 *  - Kernel monitor registration via /dev/container_monitor and ioctl(MONITOR_REGISTER/UNREGISTER)
 *
 * NOTE:
 *  - Logging pipeline is implemented as direct pipe->file relay per container (simple, robust).
 *    You can refactor into bounded-buffer producer/consumer threads for Task 3 later.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
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
#include <stdint.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
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
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    int log_pipe_fd;          // supervisor reads from this
    pthread_t log_thread;     // relays pipe to file
    int log_thread_started;
    struct container_record *next;
} container_record_t;

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
    int status; // 0 ok, nonzero error
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd; // child writes stdout/stderr here
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
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
    for (int i = start_index; i < argc; i += 2) {
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
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                return "unknown";
    }
}

static int mkdir_p(const char *path, mode_t mode)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        errno = ENOTDIR;
        return -1;
    }
    return mkdir(path, mode);
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n == 0) return -1; // EOF
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

static int send_msg(int fd, const void *buf, size_t len)
{
    uint32_t n = (uint32_t)len;
    if (write_all(fd, &n, sizeof(n)) != 0) return -1;
    if (len > 0 && write_all(fd, buf, len) != 0) return -1;
    return 0;
}

static int recv_msg(int fd, void *buf, size_t cap, size_t *out_len)
{
    uint32_t n = 0;
    if (read_all(fd, &n, sizeof(n)) != 0) return -1;
    if (n > cap) {
        // drain
        char tmp[256];
        uint32_t left = n;
        while (left > 0) {
            size_t chunk = left > sizeof(tmp) ? sizeof(tmp) : left;
            if (read_all(fd, tmp, chunk) != 0) return -1;
            left -= (uint32_t)chunk;
        }
        errno = EMSGSIZE;
        return -1;
    }
    if (n > 0 && read_all(fd, buf, n) != 0) return -1;
    *out_len = n;
    return 0;
}

static int register_with_monitor(int monitor_fd,
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
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}

static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id)
{
    for (container_record_t *c = ctx->containers; c; c = c->next) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0) return c;
    }
    return NULL;
}

static int rootfs_in_use_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    for (container_record_t *c = ctx->containers; c; c = c->next) {
        if ((c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) &&
            strncmp(c->rootfs, rootfs, sizeof(c->rootfs)) == 0) {
            return 1;
        }
    }
    return 0;
}

static void *log_relay_thread(void *arg)
{
    container_record_t *c = (container_record_t *)arg;

    int fd = open(c->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return NULL;

    char buf[4096];
    for (;;) {
        ssize_t n = read(c->log_pipe_fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        (void)write_all(fd, buf, (size_t)n);
    }

    close(fd);
    close(c->log_pipe_fd);
    c->log_pipe_fd = -1;
    return NULL;
}

static int setup_container_fs(const char *newroot)
{
    if (chdir(newroot) != 0) return -1;
    if (chroot(".") != 0) return -1;
    if (chdir("/") != 0) return -1;

    // Ensure /proc exists inside rootfs
    (void)mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        return -1;
    }
    return 0;
}

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    // Redirect stdout/stderr
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) _exit(111);
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) _exit(111);
    if (cfg->log_write_fd > STDERR_FILENO) close(cfg->log_write_fd);

    // UTS hostname (best-effort)
    (void)sethostname(cfg->id, strnlen(cfg->id, CONTAINER_ID_LEN));

    // nice value (best-effort; requires privilege)
    if (cfg->nice_value != 0) {
        errno = 0;
        if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) != 0) {
            // log to stderr (already redirected)
            fprintf(stderr, "warn: setpriority(%d) failed: %s\n", cfg->nice_value, strerror(errno));
        }
    }

    if (setup_container_fs(cfg->rootfs) != 0) {
        fprintf(stderr, "fatal: setup_container_fs failed for %s: %s\n", cfg->rootfs, strerror(errno));
        _exit(112);
    }

    // Execute command via /bin/sh -c "<command>"
    char *const sh_argv[] = { (char*)"/bin/sh", (char*)"-c", cfg->command, NULL };
    execv("/bin/sh", sh_argv);

    fprintf(stderr, "fatal: exec failed: %s\n", strerror(errno));
    _exit(127);
}

static void on_sigint_term(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

static void on_sigchld(int sig)
{
    (void)sig;
    // reaped in main loop via waitpid(WNOHANG)
}

static void reap_children(supervisor_ctx_t *ctx)
{
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;

        pthread_mutex_lock(&ctx->metadata_lock);
        for (container_record_t *c = ctx->containers; c; c = c->next) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->state = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                    c->exit_signal = 0;
                } else if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    c->exit_signal = sig;
                    c->exit_code = 0;
                    if (c->stop_requested) c->state = CONTAINER_STOPPED;
                    else if (sig == SIGKILL) c->state = CONTAINER_KILLED;
                    else c->state = CONTAINER_EXITED;
                } else {
                    c->state = CONTAINER_EXITED;
                }

                if (ctx->monitor_fd >= 0) {
                    (void)unregister_from_monitor(ctx->monitor_fd, c->id, c->host_pid);
                }
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static void format_time(time_t t, char *out, size_t out_sz)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, out_sz, "%Y-%m-%d %H:%M:%S", &tm);
}

static void respond_error(control_response_t *resp, const char *fmt, ...)
{
    resp->status = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(resp->message, sizeof(resp->message), fmt, ap);
    va_end(ap);
}

static void respond_ok(control_response_t *resp, const char *fmt, ...)
{
    resp->status = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(resp->message, sizeof(resp->message), fmt, ap);
    va_end(ap);
}

static int do_start_or_run(supervisor_ctx_t *ctx, const control_request_t *req, int wait_for_exit, control_response_t *resp)
{
    if (strlen(req->container_id) == 0) {
        respond_error(resp, "invalid id");
        return 0;
    }

    // Ensure log dir exists
    if (mkdir_p(LOG_DIR, 0755) != 0) {
        respond_error(resp, "failed to create log dir '%s': %s", LOG_DIR, strerror(errno));
        return 0;
    }

    pthread_mutex_lock(&ctx->metadata_lock);

    if (find_container_locked(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        respond_error(resp, "container id already exists: %s", req->container_id);
        return 0;
    }
    if (rootfs_in_use_locked(ctx, req->rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        respond_error(resp, "rootfs already in use by a running container: %s", req->rootfs);
        return 0;
    }

    container_record_t *c = (container_record_t *)calloc(1, sizeof(*c));
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        respond_error(resp, "oom");
        return 0;
    }

    strncpy(c->id, req->container_id, sizeof(c->id) - 1);
    strncpy(c->rootfs, req->rootfs, sizeof(c->rootfs) - 1);
    strncpy(c->command, req->command, sizeof(c->command) - 1);
    c->soft_limit_bytes = req->soft_limit_bytes;
    c->hard_limit_bytes = req->hard_limit_bytes;
    c->nice_value = req->nice_value;
    c->started_at = time(NULL);
    c->state = CONTAINER_STARTING;
    c->log_pipe_fd = -1;

    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, c->id);

    // Create pipe for logs
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        free(c);
        respond_error(resp, "pipe2 failed: %s", strerror(errno));
        return 0;
    }

    // Create child config
    child_config_t *cfg = (child_config_t *)calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pfd[0]); close(pfd[1]);
        pthread_mutex_unlock(&ctx->metadata_lock);
        free(c);
        respond_error(resp, "oom");
        return 0;
    }
    strncpy(cfg->id, c->id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, c->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, c->command, sizeof(cfg->command) - 1);
    cfg->nice_value = c->nice_value;
    cfg->log_write_fd = pfd[1];

    // Allocate stack for clone
    void *stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pfd[0]); close(pfd[1]);
        free(cfg);
        pthread_mutex_unlock(&ctx->metadata_lock);
        free(c);
        respond_error(resp, "oom stack");
        return 0;
    }

    int flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(child_fn, (char *)stack + STACK_SIZE, flags, cfg);
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        free(stack);
        free(cfg);
        pthread_mutex_unlock(&ctx->metadata_lock);
        free(c);
        respond_error(resp, "clone failed: %s", strerror(errno));
        return 0;
    }

    // Parent owns read end
    close(pfd[1]);
    c->host_pid = pid;
    c->state = CONTAINER_RUNNING;
    c->log_pipe_fd = pfd[0];

    // insert into list
    c->next = ctx->containers;
    ctx->containers = c;

    // Start log relay thread
    if (pthread_create(&c->log_thread, NULL, log_relay_thread, c) == 0) {
        c->log_thread_started = 1;
    }

    // Register with kernel monitor (best-effort)
    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd, c->id, c->host_pid, c->soft_limit_bytes, c->hard_limit_bytes) != 0) {
            // don't fail container start; just report warning
            fprintf(stderr, "warn: monitor register failed for %s pid %d: %s\n",
                    c->id, c->host_pid, strerror(errno));
        }
    }

    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!wait_for_exit) {
        respond_ok(resp, "started %s pid=%d log=%s", c->id, (int)c->host_pid, c->log_path);
        return 0;
    }

    // Wait for completion; reap loop updates metadata via SIGCHLD handler + waitpid in reap_children
    // Here we block on waitpid for this pid directly for accurate exit code
    int st = 0;
    pid_t w = waitpid(pid, &st, 0);
    if (w < 0) {
        respond_error(resp, "waitpid failed: %s", strerror(errno));
        return 0;
    }

    // Update record
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *cc = find_container_locked(ctx, req->container_id);
    if (cc) {
        if (WIFEXITED(st)) {
            cc->state = CONTAINER_EXITED;
            cc->exit_code = WEXITSTATUS(st);
            cc->exit_signal = 0;
        } else if (WIFSIGNALED(st)) {
            cc->exit_signal = WTERMSIG(st);
            if (cc->stop_requested) cc->state = CONTAINER_STOPPED;
            else if (cc->exit_signal == SIGKILL) cc->state = CONTAINER_KILLED;
            else cc->state = CONTAINER_EXITED;
        } else {
            cc->state = CONTAINER_EXITED;
        }

        if (ctx->monitor_fd >= 0) (void)unregister_from_monitor(ctx->monitor_fd, cc->id, cc->host_pid);
        if (cc->log_thread_started) (void)pthread_join(cc->log_thread, NULL);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    int exit_status = 0;
    if (WIFEXITED(st)) exit_status = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) exit_status = 128 + WTERMSIG(st);

    respond_ok(resp, "run complete %s status=%d", req->container_id, exit_status);
    resp->status = exit_status; // as required: return container status
    return 0;
}

static int do_stop(supervisor_ctx_t *ctx, const control_request_t *req, control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container_locked(ctx, req->container_id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        respond_error(resp, "no such container: %s", req->container_id);
        return 0;
    }
    if (c->state != CONTAINER_RUNNING && c->state != CONTAINER_STARTING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        respond_error(resp, "container not running: %s (state=%s)", c->id, state_to_string(c->state));
        return 0;
    }

    c->stop_requested = 1;
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&ctx->metadata_lock);

    // Try graceful then force
    if (kill(pid, SIGTERM) != 0) {
        respond_error(resp, "kill(SIGTERM) failed: %s", strerror(errno));
        return 0;
    }

    // wait a bit
    for (int i = 0; i < 20; i++) {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            // reaped here; metadata will be updated by reap_children or we do it now
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *cc = find_container_locked(ctx, req->container_id);
            if (cc) {
                if (WIFEXITED(st)) {
                    cc->state = CONTAINER_STOPPED;
                    cc->exit_code = WEXITSTATUS(st);
                } else if (WIFSIGNALED(st)) {
                    cc->state = CONTAINER_STOPPED;
                    cc->exit_signal = WTERMSIG(st);
                } else {
                    cc->state = CONTAINER_STOPPED;
                }
                if (ctx->monitor_fd >= 0) (void)unregister_from_monitor(ctx->monitor_fd, cc->id, cc->host_pid);
                if (cc->log_thread_started) (void)pthread_join(cc->log_thread, NULL);
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            respond_ok(resp, "stopped %s", req->container_id);
            return 0;
        }
        usleep(100 * 1000);
    }

    // Force kill
    (void)kill(pid, SIGKILL);
    respond_ok(resp, "stop requested (SIGKILL sent) %s", req->container_id);
    return 0;
}

static int do_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    // Build a simple multi-line report into resp->message (bounded),
    // for real usage you'd stream; for this assignment it's ok to truncate.
    char out[CONTROL_MESSAGE_LEN];
    size_t used = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    for (container_record_t *c = ctx->containers; c; c = c->next) {
        char tbuf[64];
        format_time(c->started_at, tbuf, sizeof(tbuf));
        int n = snprintf(out + used, sizeof(out) - used,
                         "%s pid=%d state=%s soft=%lu hard=%lu nice=%d started=%s log=%s exit=%d sig=%d\n",
                         c->id, (int)c->host_pid, state_to_string(c->state),
                         c->soft_limit_bytes, c->hard_limit_bytes, c->nice_value,
                         tbuf, c->log_path, c->exit_code, c->exit_signal);
        if (n < 0 || (size_t)n >= sizeof(out) - used) {
            used = sizeof(out) - 1;
            break;
        }
        used += (size_t)n;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    respond_ok(resp, "%s", out[0] ? out : "(no containers)\n");
    return 0;
}

static int do_logs(supervisor_ctx_t *ctx, const control_request_t *req, control_response_t *resp)
{
    (void)ctx;
    // Client can read the file directly, but spec wants a command; easiest is to return path
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);

    struct stat st;
    if (stat(path, &st) != 0) {
        respond_error(resp, "no log for %s (expected %s): %s", req->container_id, path, strerror(errno));
        return 0;
    }
    respond_ok(resp, "log_path=%s", path);
    return 0;
}

static void handle_request(supervisor_ctx_t *ctx, int cfd, const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    switch (req->kind) {
    case CMD_START:
        do_start_or_run(ctx, req, 0, &resp);
        break;
    case CMD_RUN:
        do_start_or_run(ctx, req, 1, &resp);
        break;
    case CMD_PS:
        do_ps(ctx, &resp);
        break;
    case CMD_LOGS:
        do_logs(ctx, req, &resp);
        break;
    case CMD_STOP:
        do_stop(ctx, req, &resp);
        break;
    default:
        respond_error(&resp, "unknown command");
        break;
    }

    (void)send_msg(cfd, &resp, sizeof(resp));
}

static int setup_control_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    (void)unlink(CONTROL_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 32) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int run_supervisor(const char *rootfs)
{
    (void)rootfs; // base-rootfs is a template, not used directly here

    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        perror("pthread_mutex_init");
        return 1;
    }

    // Open kernel monitor (best-effort)
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR | O_CLOEXEC);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr, "warn: failed to open /dev/container_monitor: %s\n", strerror(errno));
    }

    ctx.server_fd = setup_control_socket();
    if (ctx.server_fd < 0) {
        perror("setup_control_socket");
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    // Signals
    g_ctx = &ctx;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint_term;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    fprintf(stderr, "supervisor: listening on %s\n", CONTROL_PATH);

    while (!ctx.should_stop) {
        reap_children(&ctx);

        int cfd = accept(ctx.server_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        control_request_t req;
        size_t n = 0;
        if (recv_msg(cfd, &req, sizeof(req), &n) != 0 || n != sizeof(req)) {
            close(cfd);
            continue;
        }

        handle_request(&ctx, cfd, &req);
        close(cfd);
    }

    fprintf(stderr, "supervisor: shutting down...\n");

    // Stop all running containers
    pthread_mutex_lock(&ctx.metadata_lock);
    for (container_record_t *c = ctx.containers; c; c = c->next) {
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) {
            c->stop_requested = 1;
            (void)kill(c->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    // Reap remaining
    for (int i = 0; i < 50; i++) {
        reap_children(&ctx);
        usleep(100 * 1000);
    }

    // Join log threads
    pthread_mutex_lock(&ctx.metadata_lock);
    for (container_record_t *c = ctx.containers; c; c = c->next) {
        if (c->log_thread_started) (void)pthread_join(c->log_thread, NULL);
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    close(ctx.server_fd);
    (void)unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    pthread_mutex_destroy(&ctx.metadata_lock);

    return 0;
}

static int send_control_request(const control_request_t *req, control_response_t *out)
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

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "error: supervisor not running (connect %s failed): %s\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    if (send_msg(fd, req, sizeof(*req)) != 0) {
        perror("send");
        close(fd);
        return 1;
    }

    size_t n = 0;
    if (recv_msg(fd, out, sizeof(*out), &n) != 0 || n != sizeof(*out)) {
        perror("recv");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;

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

    if (send_control_request(&req, &resp) != 0) return 1;
    printf("%s\n", resp.message);
    return resp.status;
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;

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

    if (send_control_request(&req, &resp) != 0) return 1;
    printf("%s\n", resp.message);
    return resp.status;
}

static int cmd_ps(void)
{
    control_request_t req;
    control_response_t resp;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    if (send_control_request(&req, &resp) != 0) return 1;
    printf("%s", resp.message);
    return resp.status;
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    if (send_control_request(&req, &resp) != 0) return 1;
    printf("%s\n", resp.message);

    // convenience: print file contents too
    if (resp.status == 0 && strncmp(resp.message, "log_path=", 9) == 0) {
        const char *path = resp.message + 9;
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                (void)write_all(STDOUT_FILENO, buf, (size_t)n);
            }
            close(fd);
        }
    }

    return resp.status;
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    if (send_control_request(&req, &resp) != 0) return 1;
    printf("%s\n", resp.message);
    return resp.status;
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
