#include "unistd.h"
#include "string.h"
#include "fcntl.h"

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_SPAWN  4
#define SYS_WAIT   5
#define SYS_READ   6
#define SYS_OPEN   7
#define SYS_CLOSE  8
#define SYS_MKDIR  9
#define SYS_UNLINK 10
#define SYS_LSEEK  11

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

static inline int64_t syscall3(int64_t num, int64_t a1, int64_t a2, int64_t a3) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

void exit(int code) {
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

int64_t write(int fd, const void *buf, uint64_t len) {
    return syscall3(SYS_WRITE, fd, (int64_t)(uint64_t)buf, (int64_t)len);
}

int64_t read(int fd, void *buf, uint64_t len) {
    return syscall3(SYS_READ, fd, (int64_t)(uint64_t)buf, (int64_t)len);
}

int open(const char *path, int flags) {
    uint64_t len = strlen(path);
    return (int)syscall3(SYS_OPEN, (int64_t)(uint64_t)path, (int64_t)len, flags);
}

int close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

int64_t lseek(int fd, int64_t offset, int whence) {
    return syscall3(SYS_LSEEK, fd, offset, whence);
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

int mkdir(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_MKDIR, (int64_t)(uint64_t)path, (int64_t)len);
}

int unlink(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_UNLINK, (int64_t)(uint64_t)path, (int64_t)len);
}
