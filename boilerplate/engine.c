#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define STATE_FILE "/tmp/container_state.txt"
#define LOG_FILE "/tmp/container_log.txt"

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
} child_config_t;

/* ======================================================
 * Logging
 * ====================================================== */
void log_event(const char *msg)
{
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

/* ======================================================
 * Container logic
 * ====================================================== */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("mount failed");
        return 1;
    }

    if (chroot(cfg->rootfs) != 0) {
        perror("chroot failed");
        return 1;
    }

    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    /* low CPU loop */
    char *args[] = {"/bin/sh", "-c", "while true; do sleep 10; done", NULL};
    execvp(args[0], args);

    perror("exec failed");
    return 1;
}

/* ======================================================
 * RUN
 * ====================================================== */
int cmd_run(int argc, char *argv[])
{
    if (argc < 4) {
        printf("Usage: %s run <id> <rootfs>\n", argv[0]);
        return 1;
    }

    child_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    strcpy(cfg.id, argv[2]);
    strncpy(cfg.rootfs, argv[3], PATH_MAX - 1);

    static char stack[STACK_SIZE];

    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                      &cfg);

    if (pid < 0) {
        perror("clone failed");
        return 1;
    }

    waitpid(pid, NULL, 0);
    return 0;
}

/* ======================================================
 * START
 * ====================================================== */
int cmd_start(int argc, char *argv[])
{
    if (argc < 4) {
        printf("Usage: %s start <id> <rootfs>\n", argv[0]);
        return 1;
    }

    char *id = argv[2];
    char *rootfs = argv[3];

    printf("[engine] Starting container: %s\n", id);

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork failed");
        return 1;
    }

    if (pid > 0) {
        printf("[engine] Started (background)\n");

        /* store state */
        FILE *f = fopen(STATE_FILE, "a");
        if (f) {
            fprintf(f, "%s %d running\n", id, pid);
            fclose(f);
        }

        /* log */
        char logbuf[100];
        snprintf(logbuf, sizeof(logbuf), "START %s", id);
        log_event(logbuf);

        return 0;
    }

    setsid();

    child_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.id, id, CONTAINER_ID_LEN - 1);
    strncpy(cfg.rootfs, rootfs, PATH_MAX - 1);

    static char stack[STACK_SIZE];

    pid_t cpid = clone(child_fn,
                       stack + STACK_SIZE,
                       CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                       &cfg);

    if (cpid < 0) {
        perror("clone failed");
        exit(1);
    }

    signal(SIGCHLD, SIG_IGN);
    pause();
}

/* ======================================================
 * PS
 * ====================================================== */
int cmd_ps()
{
    FILE *f = fopen(STATE_FILE, "r");

    if (!f) {
        printf("No containers found\n");
        return 0;
    }

    char id[32], status[16];
    int pid;

    printf("ID\tSTATUS\tPID\n");

    while (fscanf(f, "%s %d %s", id, &pid, status) != EOF) {
        printf("%s\t%s\t%d\n", id, status, pid);
    }

    fclose(f);
    return 0;
}

/* ======================================================
 * STOP
 * ====================================================== */
int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        printf("Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    char *target = argv[2];

    FILE *f = fopen(STATE_FILE, "r");
    if (!f) {
        printf("No containers found\n");
        return 0;
    }

    FILE *temp = fopen("/tmp/container_temp.txt", "w");
    if (!temp) {
        perror("temp file failed");
        fclose(f);
        return 1;
    }

    char id[32], status[16];
    int pid;
    int found = 0;

    while (fscanf(f, "%s %d %s", id, &pid, status) != EOF) {

        if (strcmp(id, target) == 0 && strcmp(status, "running") == 0) {

            if (kill(pid, SIGKILL) == 0) {
                printf("[engine] Stopped container %s (PID %d)\n", id, pid);

                /* log event */
                char logbuf[100];
                snprintf(logbuf, sizeof(logbuf), "STOP %s", id);
                log_event(logbuf);

            } else {
                perror("kill failed");
            }

            found = 1;

        } else {
            /* keep other containers */
            fprintf(temp, "%s %d %s\n", id, pid, status);
        }
    }

    fclose(f);
    fclose(temp);

    /* replace old state file */
    if (rename("/tmp/container_temp.txt", STATE_FILE) != 0) {
        perror("rename failed");
    }

    if (!found) {
        printf("Container not found or already stopped\n");
    }

    return 0;
}
/* ======================================================
 * MAIN
 * ====================================================== */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s run <id> <rootfs>\n", argv[0]);
        printf("  %s start <id> <rootfs>\n", argv[0]);
        printf("  %s ps\n", argv[0]);
        printf("  %s stop <id>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    printf("Unknown command\n");
    return 1;
}
