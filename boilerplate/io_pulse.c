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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DURATION_SECONDS 30
#define CHUNK_SIZE       4096

int main(void) {
    printf("[workload_io] PID %d starting I/O-bound work\n", (int)getpid());
    fflush(stdout);

    char buf[CHUNK_SIZE];
    memset(buf, 'A', sizeof(buf));

    time_t start = time(NULL);
    long   cycles = 0;

    while (time(NULL) - start < DURATION_SECONDS) {
        FILE *f = fopen("/tmp/io_workload_tmp", "w");
        if (!f) { perror("fopen"); sleep(1); continue; }
        for (int i = 0; i < 256; i++) fwrite(buf, 1, sizeof(buf), f);
        fclose(f);

        f = fopen("/tmp/io_workload_tmp", "r");
        if (!f) { perror("fopen read"); sleep(1); continue; }
        while (fread(buf, 1, sizeof(buf), f) > 0) {}
        fclose(f);

        cycles++;
    }

    remove("/tmp/io_workload_tmp");
    printf("[workload_io] done — %ld cycles\n", cycles);
    return 0;
}
