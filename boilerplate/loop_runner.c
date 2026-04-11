#include <stdio.h>
#include <unistd.h>

int main() {
    while (1) {
        printf("loop_runner alive...\n");
        fflush(stdout);
        sleep(1);
    }
    return 0;
}
