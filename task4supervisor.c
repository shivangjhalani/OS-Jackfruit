#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string.h>
#include <stdbool.h>

#define MONITOR_MAGIC 'k'
#define REGISTER_CONTAINER _IOW(MONITOR_MAGIC, 1, struct reg_info)

struct reg_info {
    pid_t pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <soft_kb> <hard_kb>\n", argv[0]);
        return 1;
    }

    bool stop_requested = false;

    pid_t pid = fork();
    if (pid == 0) {
        // Simple memory eater to test
        char *mem = malloc(1024 * 1024 * 200);  // 200 MB
        memset(mem, 1, 1024 * 1024 * 200);
        while (1) sleep(1);
    } else {
        int fd = open("/dev/container_monitor", O_RDWR);
        struct reg_info info = {pid, atol(argv[1]), atol(argv[2])};
        ioctl(fd, REGISTER_CONTAINER, &info);
        close(fd);

        printf("Supervisor: Monitoring PID %d...\n", pid);

        // Wait for container exit
        int status;
        waitpid(pid, &status, 0);

        const char *reason = "unknown";
        if (WIFEXITED(status)) {
            reason = "normal_exit";
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            if (stop_requested) {
                reason = "stopped";
            } else if (sig == SIGKILL) {
                // Hard limit check via attribution rule
                reason = "hard_limit_killed";
            } else {
                reason = "signal_terminated";
            }
        }
        printf("Container Result: %s\n", reason);
    }
    return 0;
}