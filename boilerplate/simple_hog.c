#include <stdio.h>
int main() {
    while(1) {
        for(volatile long i = 0; i < 10000000; i++);
    }
    return 0;
}
