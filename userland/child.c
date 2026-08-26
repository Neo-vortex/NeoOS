#include "neoos_syscall.h"

void _start(void) {
    const char msg[] = "child running, exiting with code 42\n";
    sys_write(msg, user_strlen(msg));
    sys_exit(42);
}
