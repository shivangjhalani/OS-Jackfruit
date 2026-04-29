
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#define DURATION_SECONDS 30

int main(void) {
    printf("[workload_cpu] PID %d starting CPU-bound work\n", (int)getpid());
    fflush(stdout);

    time_t start = time(NULL);
    volatile double result = 0.0;
    long iterations = 0;

    while (time(NULL) - start < DURATION_SECONDS) {
        for (int i = 1; i <= 10000; i++)
            result += sqrt((double)i) * log((double)i + 1.0);
        iterations++;
    }

    printf("[workload_cpu] done — %ld batches, result=%.2f\n",
           iterations, result);
    return 0;
}
