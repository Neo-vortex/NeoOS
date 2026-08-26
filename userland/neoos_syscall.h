#ifndef NEOOS_USER_SYSCALL_H
#define NEOOS_USER_SYSCALL_H

// Temporary, pre-standard-library syscall wrappers for milestone 5's
// test programs only. Once the standard library milestone lands (see
// /CLAUDE.md), user programs should use it instead of this file.

#include <stdint.h>

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

static inline void sys_exit(int code) {
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

static inline int64_t sys_write(const char *buf, uint64_t len) {
    return syscall2(SYS_WRITE, (int64_t)(uint64_t)buf, (int64_t)len);
}

static inline void sys_yield(void) {
    syscall0(SYS_YIELD);
}

static inline int64_t sys_getpid(void) {
    return syscall0(SYS_GETPID);
}

static inline int64_t sys_spawn(const char *path, uint64_t path_len) {
    return syscall2(SYS_SPAWN, (int64_t)(uint64_t)path, (int64_t)path_len);
}

static inline int64_t sys_wait(int64_t pid) {
    return syscall1(SYS_WAIT, pid);
}

static inline uint64_t user_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static inline void print_num(int64_t n) {
    char buf[24];
    int i = 0;
    int negative = 0;
    if (n < 0) {
        negative = 1;
        n = -n;
    }
    if (n == 0) {
        buf[i++] = '0';
    }
    while (n > 0) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    if (negative) {
        buf[i++] = '-';
    }
    char out[24];
    for (int j = 0; j < i; j++) {
        out[j] = buf[i - 1 - j];
    }
    sys_write(out, (uint64_t)i);
}

#endif
