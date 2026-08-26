#include "neoos_syscall.h"

void _start(void) {
    const char path[] = "/BIN/CHILD.ELF";
    int64_t child_pid = sys_spawn(path, user_strlen(path));
    int64_t exit_code = sys_wait(child_pid);

    const char prefix[] = "[parent] child exit code=";
    sys_write(prefix, user_strlen(prefix));
    print_num(exit_code);
    sys_write("\n", 1);
    sys_exit(0);
}
