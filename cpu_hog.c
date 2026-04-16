#include <stdio.h>
#include <time.h>

int main(void)
{
    long long count = 0;
    time_t start = time(NULL);

    printf("cpu_hog: starting\n");
    fflush(stdout);

    while (1) {
        count++;
        if (count % 100000000 == 0) {
            printf("cpu_hog: iterations=%lld elapsed=%lds\n",
                   count, (long)(time(NULL) - start));
            fflush(stdout);
        }
    }
    return 0;
}
