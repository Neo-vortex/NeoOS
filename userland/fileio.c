#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int fd = open("/FILEIO.TXT", O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) {
        printf("[fileio] open for write FAILED: %d\n", fd);
        return 1;
    }
    const char msg[] = "hello from fileio\n";
    uint64_t msg_len = strlen(msg);
    int64_t written = write(fd, msg, msg_len);
    if (written != (int64_t)msg_len) {
        printf("[fileio] write FAILED: %d\n", (int)written);
        return 1;
    }
    close(fd);

    fd = open("/FILEIO.TXT", O_RDONLY);
    if (fd < 0) {
        printf("[fileio] open for read FAILED: %d\n", fd);
        return 1;
    }
    char readback[64];
    int64_t got = read(fd, readback, sizeof(readback) - 1);
    close(fd);

    int mismatch = ((uint64_t)got != msg_len);
    if (!mismatch) {
        for (uint64_t i = 0; i < msg_len; i++) {
            if (readback[i] != msg[i]) {
                mismatch = 1;
                break;
            }
        }
    }
    if (mismatch) {
        printf("[fileio] readback mismatch\n");
        return 1;
    }

    printf("[fileio] create/write/read smoke test passed\n");
    return 0;
}
