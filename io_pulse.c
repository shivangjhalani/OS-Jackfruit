#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define BUF_SIZE (64 * 1024)
#define CYCLES 60

int main(void)
{
    char buf[BUF_SIZE];
    int fd, i;
    time_t start = time(NULL);

    memset(buf, 0xCD, BUF_SIZE);
    printf("io_pulse: starting\n");
    fflush(stdout);

    for (i = 0; i < CYCLES; i++) {
        fd = open("/tmp/io_test.dat", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, buf, BUF_SIZE);
            close(fd);
        }
        fd = open("/tmp/io_test.dat", O_RDONLY);
        if (fd >= 0) {
            read(fd, buf, BUF_SIZE);
            close(fd);
        }
        printf("io_pulse: cycle=%d elapsed=%lds\n",
               i + 1, (long)(time(NULL) - start));
        fflush(stdout);
        sleep(1);
    }
    return 0;
}
