#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int pid = getpid();
    for (int i = 0; i < 30; i++) {
        printf("[looper pid=%d] tick\n", pid);
        for (volatile uint32_t spin = 0; spin < 3000000; spin++) {
        }
    }
    return 0;
}
