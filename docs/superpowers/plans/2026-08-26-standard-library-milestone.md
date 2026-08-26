# NeoOS Standard Library Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give NeoOS user-mode programs a real, linkable standard library (`libneoos.a`): a proper C runtime startup (`int main(int argc, char **argv)` instead of `void _start(void)`), POSIX-styled syscall wrappers, a small `string.h`, and a minimal `printf` — replacing the ad-hoc `userland/neoos_syscall.h` milestone 5 introduced as scaffolding.

**Architecture:** `lib/crt0.asm` provides `_start` (written in assembly to avoid a C-compiler-generated prologue disturbing ABI stack alignment at process entry), which calls `main` then `exit()`. `lib/syscall.c`, `lib/string.c`, and `lib/stdio.c` compile into `lib/libneoos.a`; `crt0.o` stays a separate object linked explicitly before the archive, matching how every real toolchain separates `crt0`/`crt1.o` from libc proper. All six existing test programs are migrated to the library and `userland/neoos_syscall.h` is deleted. This milestone touches only userland — no kernel-side changes at all.

**Tech Stack:** Same toolchain as prior milestones (NASM, `x86_64-elf-gcc` cross-compiler, GNU Make, `ar`, QEMU).

**Spec:** `docs/superpowers/specs/2026-08-26-standard-library-design.md`

## Global Constraints

- Freestanding C (`-ffreestanding -nostdlib`), same `USER_CFLAGS` base as milestone 5 (`-mno-mmx -mno-sse -mno-sse2 -mcmodel=large -fno-pic -static`) — both are still required for exactly the reasons milestone 5 discovered (FPU/SSE state is never initialized; user code links at `0x200000000000`, far outside the small code model's range).
- **Function signatures, fixed for this milestone:**
  - `void exit(int code) __attribute__((noreturn));`
  - `int64_t write(const char *buf, uint64_t len);` (no `fd` parameter — no file descriptor table exists)
  - `int getpid(void);`
  - `void yield(void);`
  - `int spawn(const char *path);` (takes a NUL-terminated path only; computes its length via `strlen` internally — simpler than milestone 5's raw `spawn(path, path_len)` syscall wrapper)
  - `int wait(int pid);`
  - `uint64_t strlen(const char *s);`, `void *memcpy(void *dst, const void *src, uint64_t n);`, `void *memset(void *s, int c, uint64_t n);`, `void *memmove(void *dst, const void *src, uint64_t n);`
  - `int printf(const char *fmt, ...);` — supports `%s`, `%d`, `%u`, `%x`, `%c`, `%%` only.
- `spawn`/`wait` are NeoOS-specific despite POSIX-looking names (spawn builds a process directly from a path, not `fork`+`exec`; `wait` takes one specific PID, not "any child") — every occurrence of their declaration/doc comment says so.
- `crt0.o` is never part of `libneoos.a` — it's linked as a separate object, always first, in every program's link command (`crt0.o program.o -Llib/build -lneoos`).
- Verification throughout uses headless QEMU exactly as in prior milestones: `-boot order=d` (disk attached), `-serial file:<path>`, `-no-reboot -no-shutdown -d int,guest_errors -D <path>` to catch faults as clean logs instead of silent reboots.

---

### Task 1: C Runtime, Syscall Layer, and String Functions

**Files:**
- Create: `lib/crt0.asm`
- Create: `lib/syscall.c`, `lib/string.c`
- Create: `lib/include/unistd.h`, `lib/include/string.h`
- Modify: `Makefile` (library build infrastructure; `USER_CFLAGS`'s include path switches from `userland/` to `lib/include`; `SPIN.ELF`'s build rule links against the new library)
- Modify: `userland/spin.c` (rewritten as `int main(int argc, char **argv)`)
- Modify: `kernel/kernel.c` (temporarily spawn `/BIN/SPIN.ELF` to verify, then revert)

**Interfaces:**
- Produces: `void exit(int code)`, `int64_t write(const char *buf, uint64_t len)`, `int getpid(void)`, `void yield(void)`, `int spawn(const char *path)`, `int wait(int pid)` (declared in `lib/include/unistd.h`, defined in `lib/syscall.c`); `uint64_t strlen(const char *s)`, `void *memcpy/memset/memmove(...)` (declared in `lib/include/string.h`, defined in `lib/string.c`); `lib/build/libneoos.a` and `lib/build/crt0.o` as Make targets. Task 2 adds `stdio.c`/`stdio.h` to the same archive; Tasks 2-3 link the remaining test programs against this same library.
- Consumes: nothing new — this is the foundation task.

- [ ] **Step 1: Write the C runtime startup**

```nasm
; lib/crt0.asm — C runtime startup. The ELF entry point
; (userland/user.ld's ENTRY(_start)); milestone 5's
; kernel_thread_trampoline `iretq` lands here.
;
; Written in assembly, not C, to avoid GCC generating a stack-frame
; prologue for _start that could disturb the SysV ABI's
; 16-byte-alignment-before-`call` requirement -- nothing has
; established a normal call chain yet at process entry. USER_STACK_TOP
; (milestone 5's 0x0000700000000000) is already 16-byte aligned, and
; _start pushes nothing before `call main`, so alignment holds.

extern main
extern exit

section .text
[bits 64]
global _start

_start:
    xor edi, edi   ; argc = 0
    xor esi, esi   ; argv = NULL
    call main
    mov edi, eax   ; exit(main's return value)
    call exit
.hang:             ; exit() never returns, but halt safely if it somehow does
    hlt
    jmp .hang
```

- [ ] **Step 2: Write the syscall header and implementation**

```c
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
```

```c
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
```

- [ ] **Step 3: Write the string header and implementation**

```c
#ifndef NEOOS_STRING_H
#define NEOOS_STRING_H

#include <stdint.h>

uint64_t strlen(const char *s);
void *memcpy(void *dst, const void *src, uint64_t n);
void *memset(void *s, int c, uint64_t n);
void *memmove(void *dst, const void *src, uint64_t n);

#endif
```

```c
#include "string.h"

uint64_t strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

void *memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *s, int c, uint64_t n) {
    uint8_t *p = (uint8_t *)s;
    for (uint64_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

void *memmove(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (uint64_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (uint64_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}
```

- [ ] **Step 4: Add the library build infrastructure to the Makefile**

Add these lines right before the existing `USERLAND_DIR := userland` block:

```makefile
LIB_DIR := lib
LIB_BUILD := $(BUILD_DIR)/lib
LIB_SOURCES := $(wildcard $(LIB_DIR)/*.c)
LIB_OBJECTS := $(patsubst $(LIB_DIR)/%.c,$(LIB_BUILD)/%.o,$(LIB_SOURCES))
```

Change `USER_CFLAGS`'s include path (it no longer needs `userland/`, since the last remaining consumer of that directory's own header is migrated away by Task 3):

```makefile
USER_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=large -fno-pic -static -nostdlib -Wall -Wextra -std=gnu11 -O2 -I$(LIB_DIR)/include
```

Add the library build rules (right after `USER_CFLAGS`, before the `SPIN.ELF` rule):

```makefile
$(LIB_BUILD)/%.o: $(LIB_DIR)/%.c
	mkdir -p $(LIB_BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(LIB_BUILD)/crt0.o: $(LIB_DIR)/crt0.asm
	mkdir -p $(LIB_BUILD)
	$(AS) $(ASFLAGS) $(LIB_DIR)/crt0.asm -o $(LIB_BUILD)/crt0.o

$(LIB_BUILD)/libneoos.a: $(LIB_OBJECTS)
	ar rcs $(LIB_BUILD)/libneoos.a $(LIB_OBJECTS)
```

- [ ] **Step 5: Rewrite `spin.c` and its build rule**

```c
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char msg[] = "spin test program running\n";
    write(msg, strlen(msg));
    return 0;
}
```

```makefile
$(USERLAND_BUILD)/SPIN.ELF: $(USERLAND_DIR)/spin.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/spin.c -L$(LIB_BUILD) -lneoos
```

- [ ] **Step 6: Temporarily spawn `SPIN.ELF` to verify the runtime end-to-end**

`SPIN.ELF` isn't part of milestone 5's final spawn list (only `PARENT`/`LOOPER`×2/`YIELDER` are), so temporarily replace `kmain`'s spawn calls to exercise it in isolation:

```c
    spawn("/BIN/SPIN.ELF");
```

- [ ] **Step 7: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with `-boot order=d -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown -d int,guest_errors -D /tmp/qemu-int.log`.
Expected: `spin test program running` appears on serial (proving `crt0` → `main` → `write` → `exit` all work through the new library), zero `check_exception` in the QEMU int log, zero `FAILED` in the serial log.

- [ ] **Step 8: Revert the temporary spawn change**

Restore `kmain`'s original `spawn("/BIN/PARENT.ELF")` + two `spawn("/BIN/LOOPER.ELF")` + `spawn("/BIN/YIELDER.ELF")` calls. Rebuild and confirm milestone 5's full lifecycle still works (the four still-unmigrated programs — `child`, `parent`, `looper`, `yielder` — continue building against `neoos_syscall.h` exactly as before; this task hasn't touched them).

- [ ] **Step 9: Commit**

```bash
git add lib/crt0.asm lib/syscall.c lib/string.c lib/include/unistd.h lib/include/string.h Makefile userland/spin.c
git commit -m "Add C runtime, syscall layer, and string functions (libneoos.a foundation)"
```

(`kernel/kernel.c` has no net diff after the revert in Step 8, so it isn't part of this commit.)

---

### Task 2: `printf` and Parent/Child Migration

**Files:**
- Create: `lib/stdio.c`, `lib/include/stdio.h`
- Modify: `userland/parent.c`, `userland/child.c` (rewritten as `int main(int argc, char **argv)`)
- Modify: `Makefile` (`PARENT.ELF`/`CHILD.ELF` build rules link against the library)

**Interfaces:**
- Consumes: `write` (Task 1).
- Produces: `int printf(const char *fmt, ...)` (declared in `lib/include/stdio.h`, defined in `lib/stdio.c`, added to `libneoos.a` automatically via `LIB_SOURCES`'s wildcard).

- [ ] **Step 1: Write the printf header and implementation**

```c
#ifndef NEOOS_STDIO_H
#define NEOOS_STDIO_H

// Minimal printf: %s, %d, %u, %x, %c, %% only -- no floating point,
// no width/precision, no length modifiers. Formats into a fixed
// internal buffer and writes it out via one write() call; there is
// no FILE*/streams concept yet, so printf always targets the same
// console write() does.
int printf(const char *fmt, ...);

#endif
```

```c
#include "stdio.h"
#include "unistd.h"
#include <stdint.h>
#include <stdarg.h>

#define PRINTF_BUFFER_SIZE 512

static void append_char(char *buf, uint64_t *pos, char c) {
    if (*pos < PRINTF_BUFFER_SIZE - 1) {
        buf[(*pos)++] = c;
    }
}

static void append_string(char *buf, uint64_t *pos, const char *s) {
    while (*s) {
        append_char(buf, pos, *s);
        s++;
    }
}

static void append_uint(char *buf, uint64_t *pos, uint64_t value, int base, int uppercase) {
    static const char *digits_lower = "0123456789abcdef";
    static const char *digits_upper = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_upper : digits_lower;
    char tmp[32];
    int i = 0;
    if (value == 0) {
        tmp[i++] = '0';
    }
    while (value > 0) {
        tmp[i++] = digits[value % (uint64_t)base];
        value /= (uint64_t)base;
    }
    while (i > 0) {
        append_char(buf, pos, tmp[--i]);
    }
}

static void append_int(char *buf, uint64_t *pos, int64_t value) {
    if (value < 0) {
        append_char(buf, pos, '-');
        append_uint(buf, pos, (uint64_t)(-value), 10, 0);
    } else {
        append_uint(buf, pos, (uint64_t)value, 10, 0);
    }
}

int printf(const char *fmt, ...) {
    char buf[PRINTF_BUFFER_SIZE];
    uint64_t pos = 0;

    va_list args;
    va_start(args, fmt);

    for (uint64_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            append_char(buf, &pos, fmt[i]);
            continue;
        }

        i++;
        if (fmt[i] == '\0') {
            append_char(buf, &pos, '%');
            break;
        }

        switch (fmt[i]) {
            case 's': {
                const char *s = va_arg(args, const char *);
                append_string(buf, &pos, s);
                break;
            }
            case 'd': {
                int value = va_arg(args, int);
                append_int(buf, &pos, value);
                break;
            }
            case 'u': {
                unsigned int value = va_arg(args, unsigned int);
                append_uint(buf, &pos, value, 10, 0);
                break;
            }
            case 'x': {
                unsigned int value = va_arg(args, unsigned int);
                append_uint(buf, &pos, value, 16, 0);
                break;
            }
            case 'c': {
                int value = va_arg(args, int);
                append_char(buf, &pos, (char)value);
                break;
            }
            case '%':
                append_char(buf, &pos, '%');
                break;
            default:
                append_char(buf, &pos, '%');
                append_char(buf, &pos, fmt[i]);
                break;
        }
    }

    va_end(args);

    write(buf, pos);
    return (int)pos;
}
```

- [ ] **Step 2: Rewrite `child.c` and `parent.c`**

```c
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char msg[] = "child running, exiting with code 42\n";
    write(msg, strlen(msg));
    return 42;
}
```

```c
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int child_pid = spawn("/BIN/CHILD.ELF");
    int exit_code = wait(child_pid);
    printf("[parent] child exit code=%d\n", exit_code);
    return 0;
}
```

- [ ] **Step 3: Update their build rules**

```makefile
$(USERLAND_BUILD)/CHILD.ELF: $(USERLAND_DIR)/child.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/child.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/PARENT.ELF: $(USERLAND_DIR)/parent.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/parent.c -L$(LIB_BUILD) -lneoos
```

- [ ] **Step 4: Build and verify**

Run: `make clean && make disk-image && make iso`, boot as in Task 1 with the disk attached.
Expected: `child running, exiting with code 42`, then `[parent] child exit code=42` — the `%d` format specifier producing the exact right number proves `printf` genuinely works, not just that it links. `looper`/`yielder` (still on `neoos_syscall.h`, unmigrated until Task 3) continue their bursty-interleave/yield-ordering behavior unchanged. Zero `FAILED`, zero exceptions.

- [ ] **Step 5: Commit**

```bash
git add lib/stdio.c lib/include/stdio.h userland/parent.c userland/child.c Makefile
git commit -m "Add printf and migrate parent/child to the standard library"
```

---

### Task 3: Migrate Looper/Yielder/Faulter, Delete `neoos_syscall.h`, Full Regression

**Files:**
- Modify: `userland/looper.c`, `userland/yielder.c`, `userland/faulter.c` (rewritten as `int main(int argc, char **argv)`)
- Modify: `Makefile` (their build rules link against the library)
- Delete: `userland/neoos_syscall.h`

**Interfaces:** None new — this task finishes the migration Tasks 1-2 started.

- [ ] **Step 1: Rewrite `looper.c`, `yielder.c`, `faulter.c`**

```c
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int pid = getpid();
    for (int i = 0; i < 30; i++) {
        printf("[looper pid=%d] tick\n", pid);
        for (volatile uint32_t spin = 0; spin < 3000000; spin++) {
        }
    }
    return 0;
}
```

```c
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int pid = getpid();
    for (int i = 0; i < 30; i++) {
        printf("[yielder pid=%d] tick\n", pid);
        yield();
    }
    return 0;
}
```

```c
#include <unistd.h>
#include <stdint.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char msg[] = "faulter about to divide by zero\n";
    write(msg, strlen(msg));
    __asm__ volatile ("divb %0" :: "r"((uint8_t)0));
    return 0; // unreachable
}
```

- [ ] **Step 2: Update their build rules**

```makefile
$(USERLAND_BUILD)/LOOPER.ELF: $(USERLAND_DIR)/looper.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/looper.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/YIELDER.ELF: $(USERLAND_DIR)/yielder.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/yielder.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/FAULTER.ELF: $(USERLAND_DIR)/faulter.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/faulter.c -L$(LIB_BUILD) -lneoos
```

- [ ] **Step 3: Delete the old scaffolding header**

```bash
rm userland/neoos_syscall.h
```

Confirm nothing else references it: `grep -rn neoos_syscall userland/ Makefile` should return nothing.

- [ ] **Step 4: Build and verify full regression (preemption, yield ordering, lifecycle)**

Run: `make clean && make disk-image && make iso`, boot with the disk attached, `-no-reboot -no-shutdown -d int,guest_errors`, let it run several seconds.
Expected: identical behavior to milestone 5's final state — `[looper pid=2]`/`[looper pid=3]` interleave in bursts, `[yielder pid=4]` interleaves far more densely, `child running, exiting with code 42` then `[parent] child exit code=42`, all five processes eventually exit and the system settles into the idle loop with periodic `[timer] tick=` lines. Zero `FAILED`, zero exceptions.

- [ ] **Step 5: Verify the ring-3 fault path still works**

Temporarily replace `kmain`'s four spawn calls with just `spawn("/BIN/FAULTER.ELF");`, rebuild, boot with `-serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown`.
Expected: `faulter about to divide by zero`, then the same clean `[exception] Divide Error` register dump with `cs=0x000000000000003b` (ring 3) milestone 5 produced, followed by a halt (no further output, no reboot). Revert the temporary spawn change afterward and confirm `kernel/kernel.c` has no net diff (`git diff --stat kernel/kernel.c` prints nothing).

- [ ] **Step 6: Verify the disk-detached regression still holds**

Run the same boot command but omitting `-drive file=build/disk.img,format=raw`.
Expected: every `spawn(...)` call fails cleanly (`[process] spawn FAILED: file not found: ...`), no crash, no hang, the system falls through to the idle loop with periodic ticks.

- [ ] **Step 7: Commit**

```bash
git add userland/looper.c userland/yielder.c userland/faulter.c Makefile
git rm userland/neoos_syscall.h
git commit -m "Migrate looper/yielder/faulter to the standard library and delete neoos_syscall.h"
```

---

### Task 4: Documentation and Final Verification

**Files:**
- Create: `docs/stdlib.md`

**Interfaces:** None new.

- [ ] **Step 1: Write the standard library reference**

```markdown
# NeoOS Standard Library Reference

Every exported function in `libneoos.a`, grouped by header. Per
`/CLAUDE.md`'s standard-library convention: any kernel feature that
becomes usable by a user-mode program must come with an update here
alongside the library code that exposes it.

## `<unistd.h>`

- `void exit(int code)` — terminates the calling process with the
  given exit code. Never returns.
- `int64_t write(const char *buf, uint64_t len)` — writes `len` bytes
  from `buf` to the console. No `fd` parameter (no file descriptor
  table exists yet) and no return-value distinction for partial
  writes yet — matches the underlying kernel `write` syscall exactly.
- `int getpid(void)` — returns the calling process's PID.
- `void yield(void)` — voluntarily gives up the remaining CPU time
  slice to the scheduler.
- `int spawn(const char *path)` — builds a fresh process directly from
  the ELF executable at `path` (NUL-terminated) and returns its PID,
  or `-1` on failure. NeoOS-specific: not `fork`+`exec`.
- `int wait(int pid)` — blocks until the process with the given PID
  exits, reaps it, and returns its exit code. NeoOS-specific: takes
  one specific PID, not "any child".

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
```

- [ ] **Step 2: Final full verification**

Run: `make clean && make disk-image && make iso`, boot with the disk attached, `-no-reboot -no-shutdown -d int,guest_errors -D /tmp/qemu-int.log`, let it run several seconds.
Expected: `grep -c FAILED /tmp/neoos.log` and `grep -c check_exception /tmp/qemu-int.log` both `0`; the full milestone 5 behavior (preemption, yield ordering, parent/child lifecycle with `printf`-formatted output) reproduces exactly.

- [ ] **Step 3: Commit**

```bash
git add docs/stdlib.md
git commit -m "Add standard library reference documentation"
```
