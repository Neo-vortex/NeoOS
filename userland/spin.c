#include "neoos_syscall.h"

void _start(void) {
    const char msg[] = "spin test program running\n";
    sys_write(msg, user_strlen(msg));
    sys_exit(0);
}
