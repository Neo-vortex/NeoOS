# NeoOS — Milestone 6: Standard Library

## Goal

Give NeoOS user-mode programs a real, linkable standard library
(`libneoos.a`) instead of the ad-hoc `userland/neoos_syscall.h` header
milestone 5 introduced as scaffolding: a proper C runtime startup
(`crt0`, so programs write `int main(int argc, char **argv)` instead
of `void _start(void)`), POSIX-styled syscall wrappers, a small
`string.h`, and a minimal `printf`. This is milestone 6, and it's the
library `/CLAUDE.md`'s standard-library convention refers to — from
this point on, any kernel feature that becomes usable by a user-mode
program must come with a corresponding library update and a
`docs/stdlib.md` update, per that convention.

## Success criteria

- All six existing test programs (`spin`, `child`, `parent`, `looper`,
  `yielder`, `faulter`) are rewritten against the library
  (`int main(int argc, char **argv)`, `<unistd.h>`/`<string.h>`/
  `<stdio.h>`) and, when run through the exact same QEMU verification
  steps milestone 5 used, reproduce identical observable behavior:
  the same log lines, the same preemption/yield interleaving pattern,
  the same parent/child exit-code lifecycle, and the same ring-3 fault
  dump.
- `parent`'s output uses `printf("...%d\n", exit_code)` instead of the
  old hand-rolled `print_num`, and the formatted number is correct —
  demonstrating `printf` is genuinely exercised, not a stub.
- `userland/neoos_syscall.h` is deleted, and nothing in the tree
  references it.
- `docs/stdlib.md` documents every function the library exports.

## Out of scope (future work)

- Dynamic memory (`malloc`/`free`) — no heap allocator for user-mode
  programs yet.
- Buffered I/O (`FILE*`, `fopen`/`fread`/`fwrite`) — there is no file
  descriptor abstraction yet; `write()` always targets the same
  serial-backed console `write` already wrote to in milestone 5.
- `errno` — syscalls that fail return `-1` (or another sentinel);
  there is no `errno` global or thread-local mechanism yet.
- Real `argc`/`argv` — `spawn()` has no argument-passing mechanism, so
  `main` always receives `argc=0`, `argv=NULL`. The signature is ready
  for when that changes.
- Environment variables, signal handling, locale support, and any math
  library.
- `printf` beyond `%s`, `%d`, `%u`, `%x`, `%c`, `%%` — no floating
  point, no width/precision specifiers, no length modifiers.

## Architecture

### C runtime startup

`lib/crt0.asm`'s `_start` is written directly in NASM, not C — a
C-compiled `_start` risks GCC emitting a stack-frame prologue that
could violate the ABI's 16-byte-alignment-before-`call` requirement,
since nothing has established a normal call chain yet at process
entry. `_start` sets `argc=0`/`argv=NULL`, calls `main`, then calls
`exit()` with whatever `main` returned. This is exactly the
`crt0`/`crt1.o` pattern every real libc uses, and matches the ELF
entry point `userland/user.ld`'s `ENTRY(_start)` already expects —
`kernel_thread_trampoline`'s `iretq` (milestone 5) lands here
unchanged.

### Syscall layer

The raw `syscall0`/`syscall1`/`syscall2` inline-assembly plumbing
(`SYSCALL` instruction, `rcx`/`r11`/`memory` clobbers) moves into
`lib/syscall.c` as file-private helpers — unlike the old
`neoos_syscall.h`, they are no longer part of the public API.
`lib/include/unistd.h` declares only the real, POSIX-styled functions:

```c
void exit(int code) __attribute__((noreturn));
int64_t write(const char *buf, uint64_t len);
int getpid(void);
void yield(void);
int spawn(const char *path);
int wait(int pid);
```

`spawn` and `wait` are NeoOS-specific despite the POSIX-looking names
(`spawn` builds a fresh process directly from a path, not
`fork`+`exec`; `wait` takes one specific PID, not "any child") — the
header's doc comment says so explicitly. `write`'s signature also
drops the `fd` parameter real POSIX `write` has, since there's no file
descriptor table to select between; the comment says why rather than
pretending compatibility that doesn't exist. `spawn` now takes just a
NUL-terminated path (computing its length via `strlen` internally) —
an ergonomic improvement milestone 5's raw syscall wrapper couldn't
make without a real `string.h`.

### String/memory functions and `printf`

`lib/include/string.h` / `lib/string.c`: `strlen`, `memcpy`, `memset`,
`memmove`, with standard signatures (`uint64_t` for sizes, matching
this project's existing convention of using `stdint.h` types directly
rather than inventing `size_t`-style typedefs). `lib/include/stdio.h`
/ `lib/stdio.c`: `int printf(const char *fmt, ...)` using `<stdarg.h>`
(available in GCC's freestanding mode), formatting into a fixed local
buffer and writing it out via one `write()` call. No `FILE*`, no
streams — `printf` always targets the same console `write()` does.

### Build integration

`lib/*.c` compiles into `lib/libneoos.a` via `ar`; `lib/crt0.asm`
assembles to a standalone `crt0.o`, linked explicitly before the
archive in each program's link command (`crt0.o program.o -Llib
-lneoos`) — the same separation every real toolchain maintains between
`crt0`/`crt1.o` and the C library proper, since `crt0` must be first in
link order to provide `_start`. `USER_CFLAGS` compiles test programs
against `-Ilib/include` instead of `-Iuserland`.

### Migrating the existing test programs

All six test programs (`spin`, `child`, `parent`, `looper`, `yielder`,
`faulter`) are rewritten as `int main(int argc, char **argv)` against
the new headers. `parent.c` specifically switches from its hand-rolled
`print_num` helper to `printf("[parent] child exit code=%d\n",
exit_code)`, giving the milestone's success criteria a real,
meaningful `printf` exercise instead of a synthetic one. Once nothing
references it, `userland/neoos_syscall.h` is deleted.

## File structure

```
lib/
  include/
    unistd.h   # exit, write, getpid, yield, spawn, wait
    string.h   # strlen, memcpy, memset, memmove
    stdio.h    # printf
  crt0.asm     # _start: argc=0/argv=NULL, call main, call exit
  syscall.c    # private syscall0/1/2 + the public unistd.h functions
  string.c
  stdio.c
docs/
  stdlib.md    # one entry per exported function
```

## Testing / verification

Same approach as every prior milestone — no host-runnable unit tests;
verification is via QEMU and serial log capture, re-running milestone
5's exact verification steps against the rebuilt test programs:
- Full boot log check (all six programs' lifecycle, unchanged from
  milestone 5's log output).
- Multi-process preemption and yield-ordering (bursty looper
  interleaving, denser yielder interleaving).
- Parent/child spawn → run → exit → `wait` → reap lifecycle, with
  `parent`'s `printf`-formatted exit code visibly correct.
- Ring-3 fault path (`faulter`), same clean register dump.
- Regression check with the disk detached (every `spawn` fails
  cleanly, exactly as milestone 5 verified).

## Error handling

Unchanged convention from every prior milestone: kernel-side faults
still produce a clean register dump and halt, never a silent triple
fault. At the library level, syscalls that fail return a sentinel
value (`-1` for `spawn`/`wait`, matching the underlying kernel
syscalls' existing convention) rather than crashing or aborting — no
`errno`, no exceptions, matching C's traditional low-level error
signaling for exactly this class of function. As in milestone 5,
syscall arguments (including the buffers `write`/`spawn` pass
pointers into) are not validated against the calling process's own
memory — this remains the same tracked, deferred security gap, not
reintroduced or worsened by the library layer.
