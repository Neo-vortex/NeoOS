# NeoOS Standard Library Reference

Every exported function in `libneoos.a`, grouped by header. Per
`/CLAUDE.md`'s standard-library convention: any kernel feature that
becomes usable by a user-mode program must come with an update here
alongside the library code that exposes it.

## `<unistd.h>`

- `void exit(int code)` — terminates the calling process with the
  given exit code. Never returns.
- `int64_t write(int fd, const void *buf, uint64_t len)` — writes
  `len` bytes from `buf` to the file (or console, for fd
  `STDOUT_FILENO`/`STDERR_FILENO`) open on `fd`. Returns the number of
  bytes written, or a negative `<errno.h>` code on failure.
- `int64_t read(int fd, void *buf, uint64_t len)` — reads up to `len`
  bytes from the file (or console, for fd `STDIN_FILENO`, which always
  returns 0 -- there is no keyboard-to-process input path yet) open on
  `fd` into `buf`. Returns the number of bytes actually read (0 at
  EOF), or a negative `<errno.h>` code on failure.
- `int close(int fd)` — closes `fd`. Returns 0, or a negative
  `<errno.h>` code on failure.
- `int64_t lseek(int fd, int64_t offset, int whence)` — moves `fd`'s
  read/write position. `whence` is `SEEK_SET`/`SEEK_CUR`/`SEEK_END`.
  Returns the new absolute position, or a negative `<errno.h>` code on
  failure. Writing past the current end of file (via a forward
  `lseek`) zero-fills the gap with real allocated bytes, not a logical
  sparse hole.
- `int getpid(void)` — returns the calling process's PID.
- `void yield(void)` — voluntarily gives up the remaining CPU time
  slice to the scheduler.
- `int spawn(const char *path)` — builds a fresh process directly from
  the ELF executable at `path` (NUL-terminated) and returns its PID,
  or `-1` on failure. NeoOS-specific: not `fork`+`exec`.
- `int wait(int pid)` — blocks until the process with the given PID
  exits, reaps it, and returns its exit code. NeoOS-specific: takes
  one specific PID, not "any child".
- `int mkdir(const char *path)` — creates a new, empty directory.
  Returns 0, or a negative `<errno.h>` code on failure.
- `int unlink(const char *path)` — deletes the file at `path`. Returns
  0, or a negative `<errno.h>` code on failure (including `-EISDIR` if
  `path` is a directory; there is no `rmdir`).
- `STDIN_FILENO`/`STDOUT_FILENO`/`STDERR_FILENO` (0/1/2) and
  `SEEK_SET`/`SEEK_CUR`/`SEEK_END` (0/1/2) constants.

## `<fcntl.h>`

- `int open(const char *path, int flags)` — opens (or, with
  `O_CREAT`, creates) the file at `path`. Returns a file descriptor,
  or a negative `<errno.h>` code on failure.
- `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`
  flag constants.

## `<errno.h>`

Every `open`/`read`/`write`/`close`/`lseek`/`mkdir`/`unlink` call
returns its negative error code directly instead of a bare `-1` --
there is no separate settable `errno` variable. `spawn`/`wait`/
`getpid` are unaffected and keep their existing plain `-1`-on-failure
convention.

- `ENOENT` (2) — path/file not found.
- `EBADF` (9) — invalid or closed file descriptor.
- `EEXIST` (17) — `mkdir`/`open(O_CREAT)` target already exists.
- `ENOTDIR` (20) — a path component used as a directory isn't one.
- `EISDIR` (21) — `unlink` called on a directory.
- `EINVAL` (22) — bad argument (e.g. an `lseek` result would be
  negative, or an unrecognized `whence`).
- `EMFILE` (24) — the process's file descriptor table is full (8
  open files at once, maximum).
- `ENOSPC` (28) — disk full (no free cluster), or the root directory
  is full (it has a fixed maximum entry count).

## `<string.h>`

- `uint64_t strlen(const char *s)`
- `void *memcpy(void *dst, const void *src, uint64_t n)`
- `void *memset(void *s, int c, uint64_t n)`
- `void *memmove(void *dst, const void *src, uint64_t n)`

## `<stdio.h>`

- `int printf(const char *fmt, ...)` — supports `%s`, `%d`, `%u`,
  `%x`, `%c`, `%%` only. No floating point, no width/precision, no
  length modifiers. Formats into a fixed internal buffer and writes it
  out via one `write()` call; there is no `FILE*`/streams concept, so
  `printf` always targets the same console `write()` does.

## SSE/SSE2/SSE3/SSE4

User-mode programs may freely use SSE, SSE2, SSE3, SSSE3, SSE4.1, and
SSE4.2 floating-point and vector instructions (including via GCC's
`<xmmintrin.h>`/`<emmintrin.h>`/`<smmintrin.h>` intrinsic headers) --
there is no library function to call for this, it's a CPU/build
capability, not an API. Each process's FPU/SSE register state is
saved and restored across context switches automatically. MMX and
AVX/AVX2/AVX-512 are not supported.
