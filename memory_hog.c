/*
 * io_pulse.c - I/O-oriented workload for scheduler experiments.
 *
 * Usage:
 *   /io_pulse [iterations] [sleep_ms]
 *
 * The program writes small bursts to a file and sleeps between them.
 * This gives students an easy I/O-heavy workload to compare with
 * cpu_hog when discussing responsiveness and scheduler behavior.
 *
 * If you copy this binary into an Alpine rootfs, make sure it is built in a
 * format that can run there.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_OUTPUT "/tmp/io_pulse.out"

static unsigned int parse_uint(const char *arg, unsigned int fallback)
{
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);

    if (!arg || *arg == '\0' || (end && *end != '\0') || value == 0)
        return fallback;
    return (unsigned int)value;
}

int main(int argc, char *argv[])
{
    const unsigned int iterations = (argc > 1) ? parse_uint(argv[1], 20) : 20;
    const unsigned int sleep_ms = (argc > 2) ? parse_uint(argv[2], 200) : 200;
    int fd;
    unsigned int i;

    fd = open(DEFAULT_OUTPUT, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    for (i = 0; i < iterations; i++) {
        char line[128];
        int len = snprintf(line, sizeof(line), "io_pulse iteration=%u\n", i + 1);

        if (write(fd, line, (size_t)len) != len) {
            perror("write");
            close(fd);
            return 1;
        }

        fsync(fd);
        printf("io_pulse wrote iteration=%u\n", i + 1);
        fflush(stdout);
        usleep(sleep_ms * 1000U);
    }

    close(fd);
    return 0;
}