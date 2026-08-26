#include "neoos_syscall.h"

void _start(void) {
    const char msg[] = "faulter about to divide by zero\n";
    sys_write(msg, user_strlen(msg));
    __asm__ volatile ("divb %0" :: "r"((uint8_t)0));
    sys_exit(0); // unreachable
}
