#include "unistd.h"
#include "string.h"

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_SPAWN  4
#define SYS_WAIT   5

static inline int64_t syscall0(int64_t num) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall1(int64_t num, int64_t a1) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall2(int64_t num, int64_t a1, int64_t a2) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

void exit(int code) {
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

int64_t write(const char *buf, uint64_t len) {
    return syscall2(SYS_WRITE, (int64_t)(uint64_t)buf, (int64_t)len);
}

int getpid(void) {
    return (int)syscall0(SYS_GETPID);
}

void yield(void) {
    syscall0(SYS_YIELD);
}

int spawn(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_SPAWN, (int64_t)(uint64_t)path, (int64_t)len);
}

int wait(int pid) {
    return (int)syscall1(SYS_WAIT, pid);
}
