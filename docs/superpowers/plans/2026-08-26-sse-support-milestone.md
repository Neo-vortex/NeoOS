# NeoOS SSE Support Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let user-mode programs use SSE, SSE2, SSE3, SSSE3, SSE4.1, and SSE4.2 by enabling the required CPU state (CR0/CR4), giving every task its own FXSAVE-format FPU/SSE state saved/restored on every context switch, and updating the userland build flags.

**Architecture:** A new `kernel/cpu.c`/`cpu.h` detects the required CPUID features and enables SSE at the CR0/CR4 level, capturing a default FXSAVE-format state template. `struct task` gains a 16-byte-aligned `fpu_state` buffer, initialized from that template and eagerly saved/restored around every `schedule()` context switch. `USER_CFLAGS` drops the SSE-disabling flags and adds `-msse3 -mssse3 -msse4.1 -msse4.2`; QEMU is pinned to `-cpu Nehalem`, verified to report all six required features.

**Tech Stack:** Same toolchain as every prior milestone (NASM, `x86_64-elf-gcc` cross-compiler, GNU Make, QEMU 10.2.1).

**Spec:** `docs/superpowers/specs/2026-08-26-sse-support-design.md`

## Global Constraints

- Kernel-side code stays SSE-free; `CFLAGS` (kernel) are never changed in this milestone — only `USER_CFLAGS`.
- Eager save/restore only — no `CR0.TS`/`#NM` lazy switching.
- No AVX/AVX2/AVX-512/XSAVE; FXSAVE/FXRSTOR (fixed 512-byte format) is sufficient for SSE1-4.
- MMX stays disabled (`-mno-mmx` unchanged in `USER_CFLAGS`).
- No syscall or API for user programs to customize MXCSR exception masks.
- `-cpu Nehalem` is required for every QEMU invocation used to verify this milestone (not just the Makefile's `run` target) — verified via QMP's `query-cpu-model-expansion` to report `sse`/`sse2`/`sse3`/`ssse3`/`sse4.1`/`sse4.2` all `true` on this project's QEMU (10.2.1). Without it, a default/unpinned CPU model may lack SSE4.1/4.2 and the boot-time CPUID check (Task 1) would halt for reasons unrelated to the code under test.
- Verification throughout uses headless QEMU exactly as in every prior milestone: `-cpu Nehalem -boot order=d`, `-serial file:<path>`, `-no-reboot -no-shutdown -d int,guest_errors -D <path>`.

---

### Task 1: CPU Feature Detection and SSE Enablement

**Files:**
- Create: `kernel/cpu.c`, `kernel/cpu.h`
- Modify: `kernel/kernel.c` (call `cpu_init()`)

**Interfaces:**
- Produces: `void cpu_init(void)`; `void cpu_default_fpu_state(void *dest)`; `static inline void fpu_save(void *buffer)` / `static inline void fpu_restore(void *buffer)` (both defined directly in `cpu.h`) — all consumed by Task 2 (`process.c`).
- Consumes: nothing new.

- [ ] **Step 1: Write `kernel/cpu.h`**

```c
#ifndef NEOOS_CPU_H
#define NEOOS_CPU_H

#include <stdint.h>

#define FPU_STATE_SIZE 512

// Detects required CPU features via CPUID, enables SSE (CR0/CR4), and
// captures a default FXSAVE-format state template every new task's
// fpu_state is initialized from. Halts with a diagnostic if a
// required feature (SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2) is missing --
// the kernel binary is compiled assuming their presence.
void cpu_init(void);

// Copies the default FXSAVE-format state (FPU_STATE_SIZE bytes)
// captured at cpu_init() time into `dest`, which must be at least
// FPU_STATE_SIZE bytes. Used to initialize a newly created task's
// fpu_state before it ever runs.
void cpu_default_fpu_state(void *dest);

// Saves/restores the calling CPU's current FPU/SSE register state
// to/from a FPU_STATE_SIZE-byte, 16-byte-aligned buffer. Used by
// schedule() around every context switch.
static inline void fpu_save(void *buffer) {
    __asm__ volatile ("fxsave (%0)" :: "r"(buffer) : "memory");
}

static inline void fpu_restore(void *buffer) {
    __asm__ volatile ("fxrstor (%0)" :: "r"(buffer) : "memory");
}

#endif
```

- [ ] **Step 2: Write `kernel/cpu.c`**

```c
#include "cpu.h"
#include "serial.h"

#define CPUID_LEAF_1_EDX_SSE   (1u << 25)
#define CPUID_LEAF_1_EDX_SSE2  (1u << 26)
#define CPUID_LEAF_1_ECX_SSE3  (1u << 0)
#define CPUID_LEAF_1_ECX_SSSE3 (1u << 9)
#define CPUID_LEAF_1_ECX_SSE41 (1u << 19)
#define CPUID_LEAF_1_ECX_SSE42 (1u << 20)

#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR4_OSFXSR     (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)

static uint8_t default_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                       : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                       : "a"(leaf));
}

static void check_features(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);

    int missing = 0;
    if (!(edx & CPUID_LEAF_1_EDX_SSE)) {
        serial_write_string("[cpu] MISSING: SSE\n");
        missing = 1;
    }
    if (!(edx & CPUID_LEAF_1_EDX_SSE2)) {
        serial_write_string("[cpu] MISSING: SSE2\n");
        missing = 1;
    }
    if (!(ecx & CPUID_LEAF_1_ECX_SSE3)) {
        serial_write_string("[cpu] MISSING: SSE3\n");
        missing = 1;
    }
    if (!(ecx & CPUID_LEAF_1_ECX_SSSE3)) {
        serial_write_string("[cpu] MISSING: SSSE3\n");
        missing = 1;
    }
    if (!(ecx & CPUID_LEAF_1_ECX_SSE41)) {
        serial_write_string("[cpu] MISSING: SSE4.1\n");
        missing = 1;
    }
    if (!(ecx & CPUID_LEAF_1_ECX_SSE42)) {
        serial_write_string("[cpu] MISSING: SSE4.2\n");
        missing = 1;
    }

    if (missing) {
        serial_write_string("[cpu] required SSE extension(s) missing -- halting\n");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    serial_write_string("[cpu] SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2 detected\n");
}

static uint64_t read_cr0(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(value));
    return value;
}

static void write_cr0(uint64_t value) {
    __asm__ volatile ("mov %0, %%cr0" :: "r"(value) : "memory");
}

static uint64_t read_cr4(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void write_cr4(uint64_t value) {
    __asm__ volatile ("mov %0, %%cr4" :: "r"(value) : "memory");
}

static void enable_sse(void) {
    uint64_t cr0 = read_cr0();
    cr0 &= ~CR0_EM;
    cr0 |= CR0_MP;
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    write_cr4(cr4);
}

static void capture_default_fpu_state(void) {
    __asm__ volatile ("fninit");
    uint32_t mxcsr_default = 0x1F80;
    __asm__ volatile ("ldmxcsr %0" :: "m"(mxcsr_default));
    fpu_save(default_fpu_state);
}

void cpu_init(void) {
    check_features();
    enable_sse();
    capture_default_fpu_state();
    serial_write_string("[cpu] SSE enabled, default FPU/SSE state captured\n");
}

void cpu_default_fpu_state(void *dest) {
    uint8_t *out = (uint8_t *)dest;
    for (int i = 0; i < FPU_STATE_SIZE; i++) {
        out[i] = default_fpu_state[i];
    }
}
```

- [ ] **Step 3: Call `cpu_init()` from `kmain`**

In `kernel/kernel.c`, add `#include "cpu.h"` alongside the existing includes, then add `cpu_init();` right before `process_init();`:

```c
    cpu_init();

    process_init();
    syscall_init();
```

- [ ] **Step 4: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with
`-cpu Nehalem -boot order=d -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown -d int,guest_errors -D /tmp/qemu-int.log`.
Expected: `[cpu] SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2 detected` and `[cpu] SSE enabled, default FPU/SSE state captured` appear on serial, milestone 6/7's full lifecycle is otherwise unaffected, zero `FAILED`, zero exceptions.

- [ ] **Step 5: Commit**

```bash
git add kernel/cpu.c kernel/cpu.h kernel/kernel.c
git commit -m "Add CPUID-gated SSE/SSE2/SSE3/SSE4 enablement (CR0/CR4)"
```

---

### Task 2: Per-Task FPU/SSE State and Context-Switch Integration

**Files:**
- Modify: `kernel/process.h` (add `fpu_state` to `struct task`)
- Modify: `kernel/process.c` (initialize `fpu_state` on task creation; save/restore around context switches)

**Interfaces:**
- Consumes: `FPU_STATE_SIZE`, `cpu_default_fpu_state`, `fpu_save`, `fpu_restore` (Task 1).
- Produces: `struct task`'s new `fpu_state` field, consumed by Task 3 indirectly (no new code references it directly — this task is entirely internal plumbing).

- [ ] **Step 1: Add `fpu_state` to `struct task`**

In `kernel/process.h`, add `#include "cpu.h"` alongside the existing `#include <stdint.h>`. Add a new field to `struct task`, right after `struct file_descriptor files[MAX_OPEN_FILES];` and before `struct task *next;`:

```c
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
```

- [ ] **Step 2: Initialize `fpu_state` on task creation**

In `kernel/process.c`, add `#include "cpu.h"` alongside the existing includes.

In `task_create_kernel_thread`, right after the `files[]` zeroing loop, add:

```c
    cpu_default_fpu_state(t->fpu_state);
```

In `spawn`, right after its own `files[]` zeroing loop, add the same line:

```c
    cpu_default_fpu_state(t->fpu_state);
```

- [ ] **Step 3: Save/restore FPU/SSE state around every context switch**

In `kernel/process.c`'s `schedule()`, change:

```c
    static uint64_t discarded_rsp; // used the first time schedule() is ever called, from kmain
    context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);
```

to:

```c
    static uint64_t discarded_rsp; // used the first time schedule() is ever called, from kmain
    if (prev) {
        fpu_save(prev->fpu_state);
    }
    fpu_restore(next->fpu_state);
    context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);
```

This must come after the earlier `if (prev == next) { return; }` check in the same function — when yielding with no other task ready, `prev == next` and no actual switch happens, so no save/restore should happen either (the CPU's registers already hold the correct, only-running task's state).

- [ ] **Step 4: Build and verify full regression**

Run: `make clean && make disk-image && make iso`, boot with `-cpu Nehalem` as in Task 1.
Expected: milestone 6/7's exact full lifecycle reproduces unchanged (bursty looper interleave, dense yielder interleave, `[parent] child exit code=42`, `fileio` smoke test not spawned by default — just the standard four-process boot). Since no code exercises SSE yet (`USER_CFLAGS` still has the SSE-disabling flags until Task 3), this proves the new save/restore plumbing is a complete no-op for existing behavior. Zero `FAILED`, zero exceptions.

- [ ] **Step 5: Commit**

```bash
git add kernel/process.h kernel/process.c
git commit -m "Add per-task FPU/SSE state with eager save/restore on context switch"
```

---

### Task 3: Enable SSE for Userland, Pin QEMU's CPU Model, and Verify Isolation Under Preemption

**Files:**
- Modify: `Makefile` (`USER_CFLAGS`; `run` target's `-cpu` flag; `SSE_TEST.ELF`'s build rule and disk-image entry)
- Create: `userland/sse_test.c`
- Modify: `kernel/kernel.c` (temporarily spawn two `SSE_TEST.ELF` instances to verify, then revert)
- Modify: `docs/stdlib.md` (brief note on SSE availability)

**Interfaces:** None new — this task exercises everything built in Tasks 1-2.

- [ ] **Step 1: Update `USER_CFLAGS` and pin QEMU's CPU model**

In the `Makefile`, change:

```makefile
USER_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=large -fno-pic -static -nostdlib -Wall -Wextra -std=gnu11 -O2 -I$(LIB_DIR)/include
```

to:

```makefile
USER_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -msse3 -mssse3 -msse4.1 -msse4.2 -mcmodel=large -fno-pic -static -nostdlib -Wall -Wextra -std=gnu11 -O2 -I$(LIB_DIR)/include
```

(SSE/SSE2 are already part of the x86-64 ABI baseline once not explicitly disabled; `-mno-mmx` stays, MMX is out of scope.)

Change the `run` target:

```makefile
run: iso disk-image
	qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom $(BUILD_DIR)/neoos.iso -drive file=$(DISK_IMG),format=raw
```

- [ ] **Step 2: Write `userland/sse_test.c`**

```c
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <xmmintrin.h>
#include <smmintrin.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int pid = getpid();
    double increment = (pid % 2 == 0) ? 1.5 : 2.5;
    double scalar_sum = 0.0;

    __m128 vec_inc = _mm_set1_ps((float)increment);
    __m128 vec_sum = _mm_setzero_ps();

    for (int i = 0; i < 20; i++) {
        scalar_sum += increment;
        vec_sum = _mm_add_ps(vec_sum, vec_inc);
        for (volatile uint32_t spin = 0; spin < 3000000; spin++) {
        }
    }
    vec_sum = _mm_floor_ps(vec_sum); // exercises SSE4.1, once after accumulation

    int scalar_result = (int)(scalar_sum * 10.0);
    float vec_lane0 = _mm_cvtss_f32(vec_sum);
    int vec_result = (int)vec_lane0;

    int expected_scalar = (int)(increment * 20.0 * 10.0);
    int expected_vec = (int)(increment * 20.0);

    printf("[sse pid=%d] scalar=%d vec=%d\n", pid, scalar_result, vec_result);

    if (scalar_result != expected_scalar || vec_result != expected_vec) {
        printf("[sse pid=%d] FAILED: expected scalar=%d vec=%d\n", pid, expected_scalar, expected_vec);
        return 1;
    }

    printf("[sse pid=%d] passed\n", pid);
    return 0;
}
```

Note: the SSE4.1 `_mm_floor_ps` is applied once, after the loop finishes accumulating — applying it every iteration would floor the running sum's fractional part away each time, changing the accumulated value (e.g. `floor(floor(1.5) + 1.5) = floor(2.5) = 2`, not `3`), which is not what this test intends to measure.

- [ ] **Step 3: Add `sse_test.c`'s Makefile build rule and disk-image entry**

In the `Makefile`, add after `FILEIO.ELF`'s build rule:

```makefile
$(USERLAND_BUILD)/SSE_TEST.ELF: $(USERLAND_DIR)/sse_test.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/sse_test.c -L$(LIB_BUILD) -lneoos
```

Add `$(USERLAND_BUILD)/SSE_TEST.ELF` to the `$(DISK_IMG)` target's prerequisite list, and add after the existing `mcopy ... FILEIO.ELF` line in that target's recipe:

```makefile
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SSE_TEST.ELF ::BIN/SSE_TEST.ELF
```

- [ ] **Step 4: Temporarily spawn two `SSE_TEST.ELF` instances to verify**

In `kernel/kernel.c`, replace the four `spawn(...)` calls (and the `parent_task` FAILED check) with:

```c
    spawn("/BIN/SSE_TEST.ELF");
    spawn("/BIN/SSE_TEST.ELF");
```

- [ ] **Step 5: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with `-cpu Nehalem` as in Task 1.
Expected: two `[sse pid=N] scalar=... vec=...` lines followed by two `[sse pid=N] passed` lines (one per spawned instance, one with `increment=1.5` giving `scalar=300 vec=30`, the other with `increment=2.5` giving `scalar=500 vec=50` — whichever PID lands on which parity), zero `FAILED`, zero exceptions. Both instances passing with their own distinct expected values, despite running concurrently under preemption, is the proof that FPU/SSE state doesn't leak or corrupt across context switches.

- [ ] **Step 6: Revert the temporary spawn change and verify no regression**

Restore `kernel/kernel.c`'s original `spawn("/BIN/PARENT.ELF")` + `parent_task` FAILED check + two `spawn("/BIN/LOOPER.ELF")` + `spawn("/BIN/YIELDER.ELF")` calls. Confirm `git diff --stat kernel/kernel.c` prints nothing.

Rebuild and boot again with `-cpu Nehalem`. Expected: milestone 6/7's exact full lifecycle reproduces, zero `FAILED`, zero exceptions.

- [ ] **Step 7: Add a brief note to `docs/stdlib.md`**

Add a new section at the end of `docs/stdlib.md`:

```markdown
## SSE/SSE2/SSE3/SSE4

User-mode programs may freely use SSE, SSE2, SSE3, SSSE3, SSE4.1, and
SSE4.2 floating-point and vector instructions (including via GCC's
`<xmmintrin.h>`/`<emmintrin.h>`/`<smmintrin.h>` intrinsic headers) --
there is no library function to call for this, it's a CPU/build
capability, not an API. Each process's FPU/SSE register state is
saved and restored across context switches automatically. MMX and
AVX/AVX2/AVX-512 are not supported.
```

- [ ] **Step 8: Commit**

```bash
git add Makefile userland/sse_test.c kernel/kernel.c docs/stdlib.md
git commit -m "Enable SSE/SSE2/SSE3/SSE4 for userland and verify isolation under preemption"
```
