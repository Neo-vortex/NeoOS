#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int pid = getpid();
    for (int i = 0; i < 30; i++) {
        printf("[yielder pid=%d] tick\n", pid);
        yield();
    }
    return 0;
}
