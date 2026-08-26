#include "neoos_syscall.h"

void _start(void) {
    int64_t pid = sys_getpid();
    for (int i = 0; i < 30; i++) {
        const char prefix[] = "[looper pid=";
        sys_write(prefix, user_strlen(prefix));
        print_num(pid);
        const char suffix[] = "] tick\n";
        sys_write(suffix, user_strlen(suffix));
        for (volatile uint32_t spin = 0; spin < 3000000; spin++) {
        }
    }
    sys_exit(0);
}
