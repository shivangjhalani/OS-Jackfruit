/*
 * memory_hog.c - Memory pressure workload for soft / hard limit testing.
 *
 * Default behavior:
 *   - allocate 8 MiB every second
 *   - touch each page so RSS actually grows
 *
 * Usage:
 *   /memory_hog [chunk_mb] [sleep_ms]
 *
 * If you plan to copy this binary into an Alpine rootfs, build it in a way
 * that is runnable inside that filesystem, such as static linking or
 * rebuilding it from inside the rootfs/toolchain you choose.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t parse_size_mb(const char *arg, size_t fallback)
{
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);

    if (!arg || *arg == '\0' || (end && *end != '\0') || value == 0)
        return fallback;
    return (size_t)value;
}

static useconds_t parse_sleep_ms(const char *arg, useconds_t fallback)
{
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);

    if (!arg || *arg == '\0' || (end && *end != '\0'))
        return fallback;
    return (useconds_t)(value * 1000U);
}

int main(int argc, char *argv[])
{
    const size_t chunk_mb = (argc > 1) ? parse_size_mb(argv[1], 8) : 8;
    const useconds_t sleep_us = (argc > 2) ? parse_sleep_ms(argv[2], 1000U) : 1000U * 1000U;
    const size_t chunk_bytes = chunk_mb * 1024U * 1024U;
    int count = 0;

    while (1) {
     char *mem = malloc(chunk_bytes);
        if (!mem) {
            printf("malloc failed after %d allocations\n", count);
            fflush(stdout);
            // KEEP PROCESS ALIVE AFTER FAILURE
            while (1) {
                sleep(1);
            }
        }

        memset(mem, 'A', chunk_bytes);
        count++;

        printf("allocation=%d chunk=%zuMB total=%zuMB\n",
               count, chunk_mb, (size_t)count * chunk_mb);
        fflush(stdout);

        usleep(sleep_us);
    }	
      
    return 0;
}
