#ifndef NEOOS_DIRENT_H
#define NEOOS_DIRENT_H

#include <stdint.h>

// 8.3 name, dot, NUL. Must match kernel/fs/vfs.h's VFS_NAME_MAX: this
// struct crosses the syscall boundary via getdents(), and the kernel
// and library trees do not share headers, so the definition is
// duplicated there and the two must stay in lockstep -- exactly like
// the syscall numbers in kernel/syscall.c and lib/syscall.c.
#define DIRENT_NAME_MAX 13

#define DT_REG 1
#define DT_DIR 2
#define DT_CHR 3

struct dirent {
    char    name[DIRENT_NAME_MAX];
    uint8_t type;
};

// Opaque to callers: allocated from a fixed pool inside the library,
// never by the caller. Four directories may be open at once.
typedef struct DIR DIR;

// The raw syscall. Fills up to `count` entries from a directory fd,
// returning how many were written, 0 at end of directory, or a
// negative <errno.h> code.
int getdents(int fd, struct dirent *buf, int count);

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#endif
