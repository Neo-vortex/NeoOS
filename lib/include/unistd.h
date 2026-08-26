#ifndef NEOOS_UNISTD_H
#define NEOOS_UNISTD_H

#include <stdint.h>

// NeoOS's standard library. Function names follow POSIX convention
// where the semantics match; spawn/wait are NeoOS-specific: spawn
// builds a fresh process directly from a path (not fork+exec), and
// wait takes one specific PID (not "any child").

void exit(int code) __attribute__((noreturn));

// Writes `len` bytes from `buf` to the console. Drops the `fd`
// parameter real POSIX write() has -- there's no file descriptor
// table yet, so there's nothing to select between.
int64_t write(const char *buf, uint64_t len);

int getpid(void);
void yield(void);

// Builds a fresh process directly from the ELF executable at `path`
// (NUL-terminated) and returns its PID, or -1 on failure.
int spawn(const char *path);

// Blocks until the process with the given PID exits, reaps it, and
// returns its exit code.
int wait(int pid);

#endif
