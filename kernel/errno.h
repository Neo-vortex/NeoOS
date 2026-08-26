#ifndef NEOOS_KERNEL_ERRNO_H
#define NEOOS_KERNEL_ERRNO_H

// Kernel-side mirror of lib/include/errno.h's numeric values (the two
// trees don't share headers, so these are duplicated, not included --
// both use real Linux errno numbers for familiarity, with no need for
// binary compatibility with anything).

#define ENOENT  2
#define EBADF   9
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28

#endif
