#ifndef NEOOS_ERRNO_H
#define NEOOS_ERRNO_H

// Every open/read/write/close/lseek/mkdir/unlink call in this library
// returns its negative error code directly, e.g. open() on a missing
// path returns -ENOENT -- there is no separate settable errno
// variable. spawn/wait/getpid are unaffected and keep their existing
// plain -1-on-failure convention.

#define ENOENT  2
#define EBADF   9
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28

#endif
