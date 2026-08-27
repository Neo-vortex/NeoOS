#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    volatile int shared_until_written = 100;

    int pid = fork();
    if (pid < 0) {
        printf("[fork_test] fork FAILED\n");
        return 1;
    }

    if (pid == 0) {
        shared_until_written = 200;
        printf("[fork_test child pid=%d] wrote 200, read back %d\n", getpid(), shared_until_written);
        if (shared_until_written != 200) {
            printf("[fork_test child pid=%d] FAILED: readback mismatch\n", getpid());
            return 1;
        }
        printf("[fork_test child pid=%d] passed\n", getpid());
        return 0;
    }

    shared_until_written = 300;
    printf("[fork_test parent pid=%d, child=%d] wrote 300, read back %d\n", getpid(), pid, shared_until_written);
    if (shared_until_written != 300) {
        printf("[fork_test parent pid=%d] FAILED: readback mismatch\n", getpid());
        return 1;
    }
    int status = wait(pid);
    printf("[fork_test parent pid=%d] child exited code=%d, passed\n", getpid(), status);
    return 0;
}
