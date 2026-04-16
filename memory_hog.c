#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    size_t chunk = 1024 * 1024; /* 1 MB per step */
    size_t total = 0;
    char *p;

    printf("memory_hog: starting\n");
    fflush(stdout);

    while (1) {
        p = malloc(chunk);
        if (!p) {
            printf("memory_hog: malloc failed at %zu MB\n", total);
            fflush(stdout);
            break;
        }
        memset(p, 0xAB, chunk); /* actually touch the pages */
        total++;
        printf("memory_hog: allocated %zu MB\n", total);
        fflush(stdout);
        sleep(1);
    }
    return 0;
}
