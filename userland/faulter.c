#include <unistd.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char msg[] = "faulter about to divide by zero\n";
    write(msg, strlen(msg));
    __asm__ volatile ("divb %0" :: "r"((uint8_t)0));
    return 0; // unreachable
}
