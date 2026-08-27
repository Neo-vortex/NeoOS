#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// Buffering exists so a listing loop is not one syscall per entry. The
// batch is small on purpose: a DIR sits in .bss, and there are only a
// handful of entries in a FAT 8.3 directory anyway.
#define DIRENT_BATCH   8
#define MAX_OPEN_DIRS  4

struct DIR {
    int           in_use;
    int           fd;
    struct dirent buf[DIRENT_BATCH];
    int           count;    // entries currently in buf
    int           next;     // index of the next entry to hand out
    int           at_end;   // the kernel has reported end of directory
};

static struct DIR dirs[MAX_OPEN_DIRS];

DIR *opendir(const char *path) {
    struct DIR *d = 0;
    for (int i = 0; i < MAX_OPEN_DIRS; i++) {
        if (!dirs[i].in_use) { d = &dirs[i]; break; }
    }
    if (!d) { return 0; }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { return 0; }

    // Reject a regular file here rather than at the first readdir, so a
    // successful opendir means the listing loop is safe to enter.
    struct dirent probe;
    int rc = getdents(fd, &probe, 1);
    if (rc < 0) { close(fd); return 0; }

    d->in_use = 1;
    d->fd     = fd;
    d->count  = rc;
    d->next   = 0;
    d->at_end = (rc == 0);
    if (rc > 0) { d->buf[0] = probe; }
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d || !d->in_use) { return 0; }
    if (d->next >= d->count) {
        if (d->at_end) { return 0; }
        int rc = getdents(d->fd, d->buf, DIRENT_BATCH);
        if (rc <= 0) { d->at_end = 1; return 0; }
        d->count = rc;
        d->next  = 0;
        if (rc < DIRENT_BATCH) { d->at_end = 1; }
    }
    return &d->buf[d->next++];
}

int closedir(DIR *d) {
    if (!d || !d->in_use) { return -EBADF; }
    int rc = close(d->fd);
    d->in_use = 0;
    d->fd     = -1;
    d->count  = 0;
    d->next   = 0;
    d->at_end = 0;
    return rc;
}
