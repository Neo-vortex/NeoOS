#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("[exec_target pid=%d] running, exec succeeded\n", getpid());
    return 0;
}
