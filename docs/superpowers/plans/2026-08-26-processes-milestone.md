# NeoOS Processes Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn NeoOS into a multi-tasking kernel: preemptive round-robin scheduling among kernel-managed tasks, a `SYSCALL`/`SYSRET` boundary into ring 3, spawn-based process creation loading real ELF64 executables from the FAT16 filesystem, and a full lifecycle (`spawn`/`exit`/`wait`) exercised by user-mode test programs.

**Architecture:** A minimal callee-saved `context_switch` routine (xv6-style) switches between tasks' kernel stacks; the timer IRQ drives preemption by calling the scheduler after each task's time slice. `SYSCALL`/`SYSRET` (configured via `STAR`/`LSTAR`/`SFMASK`) is the syscall boundary; a small entry stub swaps onto the current task's kernel stack (found via `tss.rsp0`, the same field the scheduler already maintains) and dispatches through a table. `spawn` reads an ELF64 image via milestone 4's FAT16 driver, builds a fresh PML4 sharing the kernel's own mappings, maps each `PT_LOAD` segment, and arranges the new task's first resume to land in ring 3 via a manufactured initial stack frame plus a small trampoline. Development proceeds in stages: prove context switching with plain kernel threads first (no syscalls, no ring 3), then add preemption, then add the syscall/ring-3 boundary, then the full spawn/wait lifecycle, then the multi-process/yield/fault success criteria.

**Tech Stack:** Same toolchain as prior milestones (NASM, `x86_64-elf-gcc` cross-compiler, GNU Make, GRUB, QEMU, `dosfstools`/`mtools`), plus the same cross-compiler used freestanding (no libc, no `-mcmodel=kernel`) to build tiny user-mode ELF64 test programs.

**Spec:** `docs/superpowers/specs/2026-08-26-processes-milestone-design.md`

## Global Constraints

- Freestanding C (`-ffreestanding -nostdlib`), no libc — same as prior milestones. User-mode test programs are equally freestanding/libc-free, using inline-assembly syscall wrappers (`userland/neoos_syscall.h`) — this is explicitly *not* the standard library (see `/CLAUDE.md`); that comes in the next milestone.
- **GDT layout, fixed for this milestone:** null (`0x00`), kernel code (`0x08`), kernel data (`0x10`), TSS (`0x18`-`0x27`, unchanged from milestone 2), user-code32 placeholder (`0x28`, never actually loaded — exists only so `STAR`'s `SYSRET` offset arithmetic lands on the right descriptors), user data (`0x30`, loaded with RPL 3 → selector `0x33`), user code64 (`0x38`, RPL 3 → selector `0x3B`). This exact ordering and spacing is dictated by the `STAR` MSR's packing convention, not a free choice — see Task 1.
- **`SYSCALL`/`SYSRET`, not `INT 0x80`.** Requires `EFER.SCE` set (bit 0) in addition to the `EFER.LME` milestone 1 already sets. `SYSCALL` does **not** switch `RSP` or reload `DS`/`ES`/`FS`/`GS` — the entry stub and the ring-3-entry trampoline handle both explicitly.
- **Every process's PML4 shares three entries with the kernel's own live PML4:** index 0 (milestone 3's permanent low identity map — `pmm.c`/`paging.c` internals still dereference physical addresses directly and rely on it), index 256 (the physmap), index 511 (the kernel's higher-half alias). Without these, kernel code executing on behalf of a syscall or interrupt taken while that process's page tables are loaded would immediately fault.
- **Context switching is the minimal callee-saved kind** (`RBX`, `RBP`, `R12`-`R15`, plus `RSP`) via `context_switch(&old_rsp, &new_rsp)` in `kernel/context_switch.asm` — not a full trap-frame swap. A brand-new task's kernel stack is pre-populated with a fake initial frame so `context_switch`'s own `ret` lands it in either a plain C entry function (kernel-mode-only tasks, Task 3) or `kernel_thread_trampoline` (ring-3 tasks, Task 5), which then transitions into user mode via a manufactured `iretq` frame — not `SYSRET`, since this isn't a return from a syscall.
- **Scheduler is single-queue round-robin**, no priorities. `MAX_TASKS = 16`. Time slice = 5 timer ticks (at the existing 100Hz LAPIC timer, 50ms). `BLOCKED` tasks (waiting in `wait`) sit outside the ready queue and are re-enqueued when the task they're waiting on exits.
- **A brand-new task's first entry must explicitly `sti`** (found necessary in Task 4, not anticipated when this plan was written): its fake initial stack frame makes `context_switch`'s `ret` jump straight into code with no `iretq` in between, so if that task's first scheduling-in happened from inside an interrupt handler (timer preemption of some other task), the CPU's `IF` flag — cleared by the interrupt-gate entry — would otherwise stay 0 forever, permanently masking the timer. `context_switch.asm`'s `kernel_thread_entry_trampoline` (Task 4) and `kernel_thread_trampoline` (Task 5) both handle this — the former with an explicit `sti`, the latter because its manufactured `iretq` frame sets `RFLAGS` with `IF` already on.
- **The timer IRQ's `lapic_send_eoi()` must run *before* calling anything that might `schedule()` away, not after** (found necessary in Task 4): `timer_handler()` can switch to a different task via a `ret` that doesn't "return" here in the traditional sense until the *preempted* task is itself resumed later — EOI'ing after the call would defer it indefinitely, and the LAPIC withholds all further timer interrupts of that vector until it arrives, deadlocking preemption entirely after exactly one switch. `isr.c`'s `VECTOR_TIMER` case sends EOI first.
- **Round-robin fairness only guarantees turn *order*, not equal work done per turn.** Two tasks running the identical tight busy-loop can complete very different amounts of work within their nominally-equal time slices under QEMU's TCG emulation (observed: one consistently completing exactly one iteration per turn, the other tens) — this is JIT/translation-cache behavior in the emulator, not a scheduler defect; verified separately via direct tick-count instrumentation that every `schedule()` call fires at a precise 5-tick boundary with correct alternation.
- **No syscall argument/pointer validation** (tracked security gap, deferred — see the spec's Out of Scope). **No stack reclamation on process exit** — a reaped task's kernel/user stack frames are never freed back to the buddy allocator in this milestone; an accepted limitation given the small, finite number of test processes involved.
- **User code must never be linked under PML4 index 0** (found necessary in Task 5, not anticipated when this plan was written): every process's `pml4[0]` is a direct *copy* of the kernel's own `p4_table[0]`, which `boot.asm` built with flags `PRESENT|WRITABLE` only — no `PAGE_USER` — since it was only ever meant for kernel-internal low-identity-map use (`pmm.c`/`paging.c` dereferencing physical addresses directly). PML4-level permissions gate everything beneath them, so *any* address under index 0 (the first 512GiB) is permanently inaccessible to user mode no matter what the PDPT/PD/PT entries beneath it say — confirmed via a direct page-table walk showing a correctly-built leaf PTE (`PRESENT|WRITABLE|USER`, no NX) that still faulted on execution. `userland/user.ld` links at `0x200000000000` (PML4 index 64) for exactly this reason — any address outside indices 0, 256, and 511 works, since `paging_map_into`'s `table_entry` creates those levels fresh with proper `PAGE_USER` flags via its own `default_flags`.
- **Userland `CFLAGS` must disable SSE/MMX exactly like the kernel's own `CFLAGS` does** (found necessary in Task 5): without `-mno-sse -mno-mmx -mno-sse2`, GCC compiles even trivial local-array initialization into `movdqa`/`movaps`, and nothing in this kernel ever initializes FPU/SSE CPU state (`CR0.EM`/`MP`, `CR4.OSFXSR`) — so those instructions raise `#UD` (Invalid Opcode) the moment they execute. `USER_CFLAGS` in the `Makefile` carries the same three flags as the kernel's `CFLAGS`.
- **`pmm_selftest` (milestone 3) needed a one-line relaxation** (found necessary in Task 5, though the affected file belongs to an earlier milestone): it originally asserted that freeing an order-3 block's two order-2 halves coalesces back to *exactly* order 3. As the kernel image grew across this milestone's tasks, `pmm_alloc(3)` started needing to split a larger free block (order 5) to satisfy the request — and since nothing else had allocated memory yet at that point in boot, the split-off neighboring halves were legitimately still free, so coalescing correctly continued merging past order 3 up to order 5. This is the buddy allocator behaving exactly as designed, not a bug — the test's assertion was too strict (`== 3` instead of `>= 3`). `pmm_alloc`/`pmm_free` themselves needed no changes.
- **Userland `-mcmodel=large`** (found necessary in Task 6, once a program larger than `SPIN.ELF` was linked): at `0x200000000000`, GCC's default "small" code model (which assumes symbols fit a 32-bit signed displacement) produces the same `relocation truncated to fit` failure the kernel itself hit in the memory-management milestone at its own high link address. `SPIN.ELF`/`CHILD.ELF` happened to be small enough that the compiler used RIP-relative addressing throughout and never hit it; `PARENT.ELF` (more string literals, more control flow) did. `USER_CFLAGS` gains `-mcmodel=large`.
- **`syscall_entry.asm` must preserve `RDI`/`RSI`/`RDX`/`R8`/`R9`/`R10` across the call to `syscall_dispatch`, not just the callee-saved registers** (found necessary in Task 6): the register shuffle and `syscall_dispatch` itself (an ordinary C function, free to clobber any SysV caller-saved register) destroy the original argument registers, but every userland syscall wrapper's clobber list (`rcx`, `r11` only — the standard, minimal convention real syscall ABIs rely on) assumes the kernel preserves everything else. Confirmed via direct evidence: `wait_for_pid` returned the correct value (`0x2a`) at the point `syscall_dispatch` returned it, but by the time userland's `exit_code` was used (after one more intervening `sys_write` call), it held a stack-address-like garbage value — a register GCC assumed survived the syscall had actually been clobbered. Fixed by pushing/popping those six registers around the call, so only `RAX` (the intended return value) changes from the caller's perspective.
- Verification throughout uses headless QEMU exactly as in prior milestones: `-serial file:<path>` for grep-able diagnostics, `-boot order=d` whenever the FAT16 disk is attached (milestone 4's fix), `-no-reboot -no-shutdown -d int,guest_errors -D <path>` to catch faults as clean logs instead of silent reboots.

---

### Task 1: GDT Extension for User-Mode Segments

**Files:**
- Modify: `kernel/gdt.h` (new selector constants)
- Modify: `kernel/gdt.c` (three new descriptors)

**Interfaces:**
- Produces: `GDT_USER_CODE32_SELECTOR` (`0x28`, never loaded), `GDT_USER_DATA_SELECTOR` (`0x33`), `GDT_USER_CODE_SELECTOR` (`0x3B`). Task 5's `syscall_init` and `kernel_thread_trampoline` consume these.
- Consumes: nothing new.

- [ ] **Step 1: Add the new selector constants**

```c
#ifndef NEOOS_GDT_H
#define NEOOS_GDT_H

#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10
#define GDT_TSS_SELECTOR         0x18
#define GDT_USER_CODE32_SELECTOR 0x28 // never loaded -- exists only for STAR's SYSRET offset arithmetic
#define GDT_USER_DATA_SELECTOR   (0x30 | 3)
#define GDT_USER_CODE_SELECTOR   (0x38 | 3)

void gdt_init(void);

#endif
```

- [ ] **Step 2: Add the three descriptors**

```c
#include <stdint.h>
#include "gdt.h"
#include "tss.h"

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t gdt_entries[8];

extern void gdt_flush(uint64_t gdtr_ptr, uint16_t data_selector,
                       uint16_t code_selector, uint16_t tss_selector);

static void set_tss_descriptor(uint64_t base, uint32_t limit) {
    uint64_t low = limit & 0xFFFF;
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40;
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFF) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFF;

    gdt_entries[3] = low;
    gdt_entries[4] = high;
}

void gdt_init(void) {
    gdt_entries[0] = 0;                                                           // null
    gdt_entries[1] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (1ULL << 53);    // kernel code (0x08)
    gdt_entries[2] = (1ULL << 41) | (1ULL << 44) | (1ULL << 47);                   // kernel data (0x10)
    set_tss_descriptor((uint64_t)&tss, sizeof(struct tss_entry) - 1);              // TSS (0x18-0x27)
    gdt_entries[5] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (3ULL << 45);    // user code32 placeholder (0x28)
    gdt_entries[6] = (1ULL << 41) | (1ULL << 44) | (1ULL << 47) | (3ULL << 45);    // user data (0x30)
    gdt_entries[7] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (1ULL << 53) | (3ULL << 45); // user code64 (0x38)

    struct gdtr gdtr = {
        .limit = sizeof(gdt_entries) - 1,
        .base = (uint64_t)&gdt_entries,
    };

    gdt_flush((uint64_t)&gdtr, GDT_KERNEL_DATA_SELECTOR,
              GDT_KERNEL_CODE_SELECTOR, GDT_TSS_SELECTOR);
}
```

- [ ] **Step 3: Build and verify (regression only -- nothing loads the new descriptors yet)**

Run: `make clean && make disk-image && make iso`, then boot with `-boot order=d -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown`.
Expected: identical log output to the end of milestone 4 (no `FAILED`, no exceptions) — this task only adds unused GDT entries, so behavior is unchanged.

- [ ] **Step 4: Commit**

```bash
git add kernel/gdt.c kernel/gdt.h
git commit -m "Extend GDT with user-mode segments for SYSCALL/SYSRET"
```

---

### Task 2: Userland Toolchain and First Test Program

**Files:**
- Create: `userland/user.ld`
- Create: `userland/neoos_syscall.h`
- Create: `userland/spin.c`
- Modify: `Makefile` (userland build variables, `SPIN.ELF` rule, disk-image recipe extended)

**Interfaces:**
- Produces: `userland/neoos_syscall.h`'s `sys_exit`, `sys_write`, `sys_yield`, `sys_getpid`, `sys_spawn`, `sys_wait`, `user_strlen` (inline wrappers matching the syscall ABI the spec defines: `RAX`=number, args in `RDI`/`RSI`/`RDX`/`R10`). Only `sys_exit`/`sys_write` are backed by working kernel code until Task 5; the rest are declared now since the ABI is already fixed, avoiding repeated edits to this shared header later. `build/disk.img` now also contains `/BIN/SPIN.ELF`.
- Consumes: nothing kernel-side yet -- this task is pure userland build tooling, verified via host tools only (no kernel changes, no boot needed).

- [ ] **Step 1: Write the user-mode linker script**

```
ENTRY(_start)

/* PML4 index 64 (0x0000200000000000) -- deliberately NOT under PML4
   index 0, 256, or 511 (the three entries every process's PML4 shares
   with the kernel's own p4_table -- see this plan's Global Constraints
   note on why index 0 specifically can never host user code). */
SECTIONS
{
    . = 0x200000000000;

    .text ALIGN(4K) : { *(.text .text.*) }
    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }
    .data ALIGN(4K) : { *(.data .data.*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss .bss.*) }
}
```

- [ ] **Step 2: Write the syscall wrapper header**

```c
#ifndef NEOOS_USER_SYSCALL_H
#define NEOOS_USER_SYSCALL_H

// Temporary, pre-standard-library syscall wrappers for milestone 5's
// test programs only. Once the standard library milestone lands (see
// /CLAUDE.md), user programs should use it instead of this file.

#include <stdint.h>

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

static inline void sys_exit(int code) {
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

static inline int64_t sys_write(const char *buf, uint64_t len) {
    return syscall2(SYS_WRITE, (int64_t)(uint64_t)buf, (int64_t)len);
}

static inline void sys_yield(void) {
    syscall0(SYS_YIELD);
}

static inline int64_t sys_getpid(void) {
    return syscall0(SYS_GETPID);
}

static inline int64_t sys_spawn(const char *path, uint64_t path_len) {
    return syscall2(SYS_SPAWN, (int64_t)(uint64_t)path, (int64_t)path_len);
}

static inline int64_t sys_wait(int64_t pid) {
    return syscall1(SYS_WAIT, pid);
}

static inline uint64_t user_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

#endif
```

- [ ] **Step 3: Write the first test program**

```c
#include "neoos_syscall.h"

void _start(void) {
    const char msg[] = "spin test program running\n";
    sys_write(msg, user_strlen(msg));
    sys_exit(0);
}
```

- [ ] **Step 4: Add the userland build to the Makefile**

```makefile
USERLAND_DIR := userland
USERLAND_BUILD := $(BUILD_DIR)/userland
USER_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=large -fno-pic -static -nostdlib -Wall -Wextra -std=gnu11 -O2 -I$(USERLAND_DIR)

$(USERLAND_BUILD)/SPIN.ELF: $(USERLAND_DIR)/spin.c $(USERLAND_DIR)/user.ld $(USERLAND_DIR)/neoos_syscall.h
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(USERLAND_DIR)/spin.c
```

- [ ] **Step 5: Extend the disk image to include `/BIN/SPIN.ELF`**

```makefile
$(DISK_IMG): $(USERLAND_BUILD)/SPIN.ELF
	mkdir -p $(DISK_SRC)/DIR
	printf 'Hello from NeoOS FAT16!\n' > $(DISK_SRC)/HELLO.TXT
	head -c 8192 /dev/zero | tr '\0' 'N' > $(DISK_SRC)/BIGFILE.TXT
	printf 'nested file contents\n' > $(DISK_SRC)/DIR/NESTED.TXT
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=32 status=none
	mkfs.fat -F 16 $(DISK_IMG)
	mcopy -i $(DISK_IMG) $(DISK_SRC)/HELLO.TXT ::HELLO.TXT
	mcopy -i $(DISK_IMG) $(DISK_SRC)/BIGFILE.TXT ::BIGFILE.TXT
	mmd -i $(DISK_IMG) ::DIR
	mcopy -i $(DISK_IMG) $(DISK_SRC)/DIR/NESTED.TXT ::DIR/NESTED.TXT
	mmd -i $(DISK_IMG) ::BIN
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SPIN.ELF ::BIN/SPIN.ELF
```

This replaces Task 1 (of the storage milestone)'s `$(DISK_IMG)` rule with the same recipe plus a prerequisite on `SPIN.ELF`, a `::BIN` directory, and the one new `mcopy`. Later tasks in this plan extend this same rule the same way as each new test program is introduced.

- [ ] **Step 6: Build and verify independently of the kernel**

Run: `make clean && make disk-image`
Then inspect the built ELF and its placement on the image with host tools:
```bash
$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-readelf -h build/userland/SPIN.ELF
mdir -i build/disk.img ::BIN
```
Expected: `readelf -h` shows `Type: EXEC`, `Machine: Advanced Micro Devices X86-64`, and an entry point of `0x400000` (matching `user.ld`); `mdir` shows `SPIN.ELF` present under `::BIN`.

- [ ] **Step 7: Commit**

```bash
git add userland/user.ld userland/neoos_syscall.h userland/spin.c Makefile
git commit -m "Add userland toolchain and first test program (SPIN.ELF)"
```

---

### Task 3: Task Representation, Context Switch, and Round-Robin Scheduler

**Files:**
- Create: `kernel/process.h`
- Create: `kernel/process.c`
- Create: `kernel/context_switch.asm`
- Modify: `Makefile` (add `context_switch.asm` to `ASM_OBJECTS`)
- Modify: `kernel/kernel.c` (two temporary kernel-mode test threads, replaced in Task 5)

**Interfaces:**
- Consumes: `pmm_alloc` (milestone 3), `phys_to_virt` (milestone 3), `tss` (milestone 2, for `rsp0`).
- Produces: `enum task_state { TASK_UNUSED, TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_ZOMBIE }`, `struct task` (fields: `pid`, `state`, `saved_rsp`, `kernel_stack_top`, `pml4_phys`, `parent_pid`, `exit_code`, `next`), `void process_init(void)`, `struct task *task_create_kernel_thread(void (*entry)(void))`, `void schedule(void)`, `struct task *current_task(void)`, and (from `context_switch.asm`) `void context_switch(uint64_t *old_rsp, uint64_t *new_rsp)`. Task 4 (preemption) calls `schedule()` from the timer handler; Task 5 extends `struct task`'s use (`pml4_phys` becomes non-zero) and adds `kernel_thread_trampoline` to `context_switch.asm`.

- [ ] **Step 1: Write the process header**

```c
#ifndef NEOOS_PROCESS_H
#define NEOOS_PROCESS_H

#include <stdint.h>

#define MAX_TASKS 16
#define KERNEL_STACK_ORDER 2 // 4 frames = 16KiB

enum task_state { TASK_UNUSED, TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_ZOMBIE };

struct task {
    int pid;
    enum task_state state;
    uint64_t saved_rsp;
    uint64_t kernel_stack_top;
    uint64_t pml4_phys; // 0 = share the kernel's own address space (kernel-mode-only task)
    int parent_pid;
    int exit_code;
    struct task *next; // ready-queue link
};

void process_init(void);

// Creates a task that starts executing `entry` directly in ring 0,
// sharing the kernel's own address space. Used only by this
// milestone's early tests (Tasks 3-4) -- real processes (Task 5
// onward) are built by spawn() instead.
struct task *task_create_kernel_thread(void (*entry)(void));

void schedule(void);
struct task *current_task(void);

#endif
```

- [ ] **Step 2: Write the context switch routine**

```nasm
; kernel/context_switch.asm — minimal callee-saved context switch.
;
; void context_switch(uint64_t *old_rsp, uint64_t *new_rsp)
; System V: rdi = old_rsp, rsi = new_rsp
;
; Saves the outgoing task's callee-saved registers and RSP onto its
; own kernel stack, then loads the incoming task's saved RSP and pops
; its callee-saved registers. The final `ret` resumes execution
; wherever the incoming task last called context_switch from -- for a
; brand-new task, that's a fake frame set up by
; task_create_kernel_thread (or spawn(), from Task 5 on) rather than a
; real prior call.

section .text
[bits 64]
global context_switch

context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
```

- [ ] **Step 3: Write the scheduler and task creation**

```c
#include "process.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "tss.h"
#include "serial.h"

extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);

static struct task tasks[MAX_TASKS];
static struct task *ready_head;
static struct task *ready_tail;
static struct task *current;
static int next_pid = 1;

static void enqueue_ready(struct task *t) {
    t->next = 0;
    if (ready_tail) {
        ready_tail->next = t;
    } else {
        ready_head = t;
    }
    ready_tail = t;
}

static struct task *dequeue_ready(void) {
    struct task *t = ready_head;
    if (t) {
        ready_head = t->next;
        if (!ready_head) {
            ready_tail = 0;
        }
        t->next = 0;
    }
    return t;
}

void process_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
    }
    ready_head = 0;
    ready_tail = 0;
    current = 0;
    serial_write_string("[process] initialized\n");
}

static struct task *alloc_task_slot(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            return &tasks[i];
        }
    }
    return 0;
}

struct task *task_create_kernel_thread(void (*entry)(void)) {
    struct task *t = alloc_task_slot();
    if (!t) {
        return 0;
    }

    uint64_t stack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    uint64_t stack_top = (uint64_t)(uintptr_t)phys_to_virt(stack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    uint64_t *sp = (uint64_t *)stack_top;
    *(--sp) = (uint64_t)entry;
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->pid = next_pid++;
    t->state = TASK_READY;
    t->saved_rsp = (uint64_t)sp;
    t->kernel_stack_top = stack_top;
    t->pml4_phys = 0;
    t->parent_pid = 0;
    t->exit_code = 0;
    t->next = 0;

    enqueue_ready(t);
    return t;
}

struct task *current_task(void) {
    return current;
}

void schedule(void) {
    struct task *next = dequeue_ready();
    if (!next) {
        return; // nothing else ready; keep running whatever's current
    }

    struct task *prev = current;
    if (prev && prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        enqueue_ready(prev);
    }

    next->state = TASK_RUNNING;
    current = next;
    tss.rsp0 = next->kernel_stack_top;

    if (prev == next) {
        return;
    }

    static uint64_t discarded_rsp; // used the first time schedule() is ever called, from kmain
    context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);
}
```

- [ ] **Step 4: Add `context_switch.asm` to the Makefile**

```makefile
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/isr_stubs.o $(BUILD_DIR)/context_switch.o

$(BUILD_DIR)/context_switch.o: kernel/context_switch.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/context_switch.asm -o $(BUILD_DIR)/context_switch.o
```

- [ ] **Step 5: Wire in two test kernel threads (temporary -- removed in Task 5)**

```c
#include "process.h"

/* ... */

static void kernel_thread_a(void) {
    for (int i = 0; i < 20; i++) {
        serial_write_string("[thread-a] iteration=");
        serial_write_hex64((uint64_t)i);
        serial_write_string("\n");
        schedule();
    }
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void kernel_thread_b(void) {
    for (int i = 0; i < 20; i++) {
        serial_write_string("[thread-b] iteration=");
        serial_write_hex64((uint64_t)i);
        serial_write_string("\n");
        schedule();
    }
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
```

Replace the end of `kmain` (from `heap_init` -- keep everything before that -- through the final idle loop) with:

```c
    heap_init();
    heap_selftest();

    struct ata_identify_info ata_info;
    ata_identify(&ata_info);

    fat16_mount();
    fat16_selftest();

    process_init();
    task_create_kernel_thread(kernel_thread_a);
    task_create_kernel_thread(kernel_thread_b);

    serial_write_string("NeoOS: interrupts enabled, starting scheduler\n");
    __asm__ volatile ("sti");

    schedule(); // never returns in practice -- control passes permanently into the task system
    for (;;) {
        __asm__ volatile ("hlt");
    }
```

- [ ] **Step 6: Build and verify**

Run: `make clean && make disk-image && make iso`, boot as before with `-boot order=d`.
Expected: after the usual milestone 2-4 log lines and `[process] initialized`, serial shows strictly alternating `[thread-a] iteration=0x0`, `[thread-b] iteration=0x0`, `[thread-a] iteration=0x1`, `[thread-b] iteration=0x1`, ... up through `0x13` (19 in hex) for both threads, then no further output (both threads reach their terminal `hlt` loop) — no `FAILED`, no exceptions. This confirms `context_switch` correctly alternates between the two threads on every voluntary `schedule()` call.

- [ ] **Step 7: Commit**

```bash
git add kernel/process.c kernel/process.h kernel/context_switch.asm Makefile kernel/kernel.c
git commit -m "Add task representation, context switch, and round-robin scheduler"
```

---

### Task 4: Timer-Driven Preemption

**Files:**
- Modify: `kernel/timer.c` (time-slice countdown calls `schedule()`)
- Modify: `kernel/kernel.c` (test threads no longer call `schedule()` manually; each does enough work per iteration that the timer visibly preempts it mid-loop)
- Modify: `kernel/isr.c` (send the timer's EOI *before* calling `timer_handler()`, not after — see Step 3)
- Modify: `kernel/context_switch.asm`, `kernel/process.c` (a brand-new task's first entry must explicitly re-enable interrupts — see Step 4)

**Interfaces:**
- Consumes: `schedule()` (Task 3).
- Produces: `kernel_thread_entry_trampoline` (assembly-only symbol, referenced from `process.c`'s `task_create_kernel_thread`). Nothing else — this task otherwise only changes *when* `schedule()` gets called, not its signature.

- [ ] **Step 1: Add time-slice countdown to the timer handler**

```c
#include "timer.h"
#include "pit.h"
#include "lapic.h"
#include "serial.h"
#include "process.h"

#define TICKS_PER_LOG 100 // 100Hz timer -> log once per second
#define TIMESLICE_TICKS 5  // 100Hz timer, 5 ticks = 50ms per time slice

static volatile uint64_t tick_count = 0;
static uint32_t timeslice_remaining = TIMESLICE_TICKS;

void timer_handler(void) {
    tick_count++;
    if (tick_count % TICKS_PER_LOG == 0) {
        serial_write_string("[timer] tick=");
        serial_write_hex64(tick_count);
        serial_write_string("\n");
    }

    if (--timeslice_remaining == 0) {
        timeslice_remaining = TIMESLICE_TICKS;
        schedule();
    }
}
```

(`timer_init` is unchanged.)

- [ ] **Step 2: Remove manual `schedule()` calls from the test threads and give each iteration enough work to make preemption visible**

A tight print-only loop completes far faster than one 50ms time slice, so pure timer preemption would never visibly interrupt it. Add a busy-wait per iteration and more iterations, so multiple time slices are guaranteed to elapse across the run:

```c
static void kernel_thread_a(void) {
    for (int i = 0; i < 200; i++) {
        serial_write_string("[thread-a] iteration=");
        serial_write_hex64((uint64_t)i);
        serial_write_string("\n");
        for (volatile uint32_t spin = 0; spin < 5000000; spin++) {
        }
    }
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void kernel_thread_b(void) {
    for (int i = 0; i < 200; i++) {
        serial_write_string("[thread-b] iteration=");
        serial_write_hex64((uint64_t)i);
        serial_write_string("\n");
        for (volatile uint32_t spin = 0; spin < 5000000; spin++) {
        }
    }
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
```

The initial `schedule()` call in `kmain` that kicks off multitasking stays exactly as Task 3 left it.

- [ ] **Step 3: Send the timer's EOI before, not after, calling the handler that may switch tasks**

`timer_handler()` can call `schedule()`, which can switch to a different task via a bare `ret` that doesn't "return" here in the traditional sense until the task being *preempted* is itself resumed later. If EOI is sent only after `timer_handler()` returns, it gets deferred indefinitely — and the LAPIC withholds all further interrupts of that vector until its EOI arrives, deadlocking preemption after exactly one switch (confirmed: without this fix, `[timer] tick=` logging — which needs 100 ticks to fire once — never appears again after the first switch, even after 15+ seconds).

```c
// kernel/isr.c's VECTOR_TIMER case in isr_handler, reordered:
    if (regs->vector_number == VECTOR_TIMER) {
        lapic_send_eoi();
        timer_handler();
        return;
    }
```

- [ ] **Step 4: Make a brand-new task's first entry re-enable interrupts**

A brand-new task's fake initial stack frame makes `context_switch`'s `ret` jump straight into C code with no `iretq` in between — but `iretq` is the only thing that normally restores `RFLAGS` (including `IF`) when resuming a task. If this task's first scheduling-in happens from inside an interrupt handler (timer preemption of some other task), the CPU's `IF` flag is 0 at that point (cleared by the interrupt-gate entry) and would stay 0 forever once this task starts running normally, permanently masking the timer (confirmed via QEMU's `info registers`: `RFL` showed `IF` clear while the newly-scheduled thread ran). Add a small trampoline that explicitly re-enables interrupts before running the real entry point:

```nasm
; Added to kernel/context_switch.asm, after context_switch's `ret`:

; Bootstraps a brand-new kernel-mode task's very first run. Reached
; via a bare `ret` out of context_switch (see task_create_kernel_thread's
; initial stack setup in process.c), never called directly.
global kernel_thread_entry_trampoline

kernel_thread_entry_trampoline:
    pop rax   ; entry function pointer, planted by task_create_kernel_thread
    sti
    call rax
.hang:        ; entry should never return, but halt safely if it does
    hlt
    jmp .hang
```

```c
// kernel/process.c: task_create_kernel_thread's initial stack setup
// gains one more planted value (shown in context):
extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);
extern void kernel_thread_entry_trampoline(void);

/* ... inside task_create_kernel_thread, replacing the stack setup: */
    uint64_t *sp = (uint64_t *)stack_top;
    *(--sp) = (uint64_t)entry;                            // popped by kernel_thread_entry_trampoline
    *(--sp) = (uint64_t)kernel_thread_entry_trampoline;    // context_switch's `ret` lands here
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15
```

- [ ] **Step 5: Build and verify**

Run: `make clean && make disk-image && make iso`, boot as before, let it run several seconds before stopping QEMU.
Expected: serial shows `[thread-a]` and `[thread-b]` lines interleaved in **bursts** (several consecutive lines from one thread, then a switch to the other) rather than Task 3's strict single-line alternation — proof that the *timer*, not a manual call, is doing the switching. `[timer] tick=` lines continue appearing throughout (proof the timer keeps firing after multiple switches, not just the first one). Burst *sizes* may vary substantially and unevenly between the two threads (a QEMU TCG JIT/translation-cache characteristic of two competing tight loops, not a scheduler defect) — what matters is that both threads keep getting turns throughout the run, not that turns are equal-sized. If the log instead shows one thread completing all 200 iterations before the other starts at all, the busy-wait isn't long enough relative to `TIMESLICE_TICKS`; increase the spin count and rebuild. No `FAILED`, no exceptions either way.

- [ ] **Step 6: Commit**

```bash
git add kernel/timer.c kernel/kernel.c kernel/isr.c kernel/context_switch.asm kernel/process.c
git commit -m "Wire scheduler into timer IRQ for preemptive multitasking"
```

---

### Task 5: ELF Loading, `spawn()`, and SYSCALL/SYSRET

**Files:**
- Modify: `kernel/mm/paging.h`, `kernel/mm/paging.c` (expose `paging_alloc_pml4`/`paging_map_into`, generalized from the existing kernel-only `paging_map`)
- Create: `kernel/elf.h`, `kernel/elf.c`
- Modify: `kernel/serial.h`, `kernel/serial.c` (add a length-bounded write, since user buffers aren't NUL-terminated)
- Modify: `kernel/context_switch.asm` (add `kernel_thread_trampoline`, the ring-3 entry bootstrap)
- Modify: `kernel/process.h`, `kernel/process.c` (add `spawn`, `task_exit`; `schedule` gains a `CR3` reload)
- Create: `kernel/syscall.h`, `kernel/syscall.c`
- Create: `kernel/syscall_entry.asm`
- Modify: `Makefile` (add `syscall_entry.o` to `ASM_OBJECTS`; `USER_CFLAGS` gains `-mno-mmx -mno-sse -mno-sse2` -- see Global Constraints)
- Modify: `kernel/kernel.c` (remove the Task 3/4 kernel-thread test; wire in `syscall_init` and `spawn("/BIN/SPIN.ELF")`)
- Modify: `userland/user.ld` (link address moves to `0x200000000000`, PML4 index 64 -- see Global Constraints)
- Modify: `kernel/mm/pmm.c` (relax `pmm_selftest`'s coalescing assertion from `== 3` to `>= 3` -- see Global Constraints; `pmm_alloc`/`pmm_free` themselves are unchanged)

**Interfaces:**
- Consumes: `pmm_alloc` (milestone 3), `fat16_find`/`fat16_read_file` (milestone 4), `kmalloc`/`kfree` (milestone 3), `GDT_USER_CODE32_SELECTOR`/`GDT_KERNEL_CODE_SELECTOR` (Task 1), `schedule`/`struct task` (Task 3).
- Produces: `uint64_t paging_alloc_pml4(void)`, `int paging_map_into(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)`, `int elf_load(const uint8_t *data, uint32_t size, uint64_t *pml4, uint64_t *out_entry)`, `void serial_write_string_n(const char *str, uint64_t len)`, `struct task *spawn(const char *path)`, `void task_exit(int code)`, `void syscall_init(void)`, and (called only from assembly) `int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4)`. Task 6 extends `syscall_dispatch`'s `switch` with `SYS_YIELD`/`SYS_SPAWN`/`SYS_WAIT`.

- [ ] **Step 1: Generalize the paging API to target an arbitrary PML4**

```c
// Added to kernel/mm/paging.h, alongside the existing declarations:

// Allocates a fresh, zeroed page-table frame -- suitable as a new
// PML4 for a process's own address space. Caller must copy in
// whatever shared kernel entries it needs (see process.c's spawn()).
uint64_t paging_alloc_pml4(void);

// Like paging_map, but targets an arbitrary PML4 (a phys_to_virt
// pointer, not necessarily the one currently loaded in CR3) instead
// of the kernel's own live p4_table -- used by spawn() to build a new
// process's address space before it's ever loaded into CR3.
int paging_map_into(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
```

```c
// kernel/mm/paging.c: add these two functions, and change paging_map
// to delegate to paging_map_into (table_entry, alloc_table_frame, and
// everything else in the file is unchanged).

uint64_t paging_alloc_pml4(void) {
    return alloc_table_frame();
}

int paging_map_into(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t default_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t *pdpt = table_entry(pml4, PML4_INDEX(virt), 1, default_flags);
    uint64_t *pd   = table_entry(pdpt, PDPT_INDEX(virt), 1, default_flags);
    uint64_t *pt   = table_entry(pd, PD_INDEX(virt), 1, default_flags);

    pt[PT_INDEX(virt)] = (phys & PAGE_ADDR_MASK) | flags | PAGE_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

int paging_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    return paging_map_into(p4_table, virt, phys, flags);
}
```

- [ ] **Step 2: Write the ELF64 loader**

```c
#ifndef NEOOS_ELF_H
#define NEOOS_ELF_H

#include <stdint.h>

// Parses the ELF64 image in `data` (length `size`) and maps its
// PT_LOAD segments into `pml4` (a fresh PML4 from paging_alloc_pml4,
// not yet loaded into CR3), copying each segment's bytes in from
// `data`. Returns 1 on success with *out_entry set to the image's
// entry point, 0 on any parse/mapping failure (logged to serial).
int elf_load(const uint8_t *data, uint32_t size, uint64_t *pml4, uint64_t *out_entry);

#endif
```

```c
#include "elf.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "serial.h"

struct elf64_header {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

#define ELF_PT_LOAD 1
#define ELF_PF_X    1

static int is_valid_elf64(const struct elf64_header *hdr) {
    return hdr->e_ident[0] == 0x7F && hdr->e_ident[1] == 'E' &&
           hdr->e_ident[2] == 'L' && hdr->e_ident[3] == 'F' &&
           hdr->e_ident[4] == 2; // ELFCLASS64
}

int elf_load(const uint8_t *data, uint32_t size, uint64_t *pml4, uint64_t *out_entry) {
    if (size < sizeof(struct elf64_header)) {
        serial_write_string("[elf] load FAILED: image too small\n");
        return 0;
    }

    const struct elf64_header *hdr = (const struct elf64_header *)data;
    if (!is_valid_elf64(hdr)) {
        serial_write_string("[elf] load FAILED: not a valid ELF64 image\n");
        return 0;
    }

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(data + hdr->e_phoff + (uint64_t)i * hdr->e_phentsize);
        if (ph->p_type != ELF_PT_LOAD) {
            continue;
        }

        uint64_t flags = PAGE_WRITABLE | PAGE_USER; // always writable -- no read-only .text/.rodata this milestone
        if (!(ph->p_flags & ELF_PF_X)) {
            flags |= PAGE_NO_EXECUTE;
        }

        uint64_t seg_start = ph->p_vaddr & ~(uint64_t)0xFFF;
        uint64_t seg_end = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~(uint64_t)0xFFF;

        for (uint64_t page_addr = seg_start; page_addr < seg_end; page_addr += 4096) {
            uint64_t frame_phys = pmm_alloc(0);
            if (!frame_phys) {
                serial_write_string("[elf] load FAILED: out of memory mapping segment\n");
                return 0;
            }

            uint8_t *frame_virt = (uint8_t *)phys_to_virt(frame_phys);
            for (int z = 0; z < 4096; z++) {
                frame_virt[z] = 0;
            }

            // Copy whatever part of this page falls within
            // [p_vaddr, p_vaddr+p_filesz) -- the rest (pure .bss, or
            // the tail of a page beyond filesz) stays zeroed above.
            uint64_t page_end = page_addr + 4096;
            uint64_t copy_start = ph->p_vaddr > page_addr ? ph->p_vaddr : page_addr;
            uint64_t file_end = ph->p_vaddr + ph->p_filesz;
            uint64_t copy_end = file_end < page_end ? file_end : page_end;
            if (copy_end > copy_start) {
                uint64_t src_offset = ph->p_offset + (copy_start - ph->p_vaddr);
                uint64_t dst_offset = copy_start - page_addr;
                for (uint64_t b = 0; b < copy_end - copy_start; b++) {
                    frame_virt[dst_offset + b] = data[src_offset + b];
                }
            }

            paging_map_into(pml4, page_addr, frame_phys, flags);
        }
    }

    *out_entry = hdr->e_entry;
    return 1;
}
```

- [ ] **Step 3: Add a length-bounded serial write**

```c
// Added to kernel/serial.h:
void serial_write_string_n(const char *str, uint64_t len);
```

```c
// Added to kernel/serial.c:
void serial_write_string_n(const char *str, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        serial_putc(str[i]);
    }
}
```

- [ ] **Step 4: Add the ring-3 entry trampoline**

```nasm
; Added to kernel/context_switch.asm.
;
; Bootstraps a brand-new task's very first entry into ring 3. Reached
; via a bare `ret` out of context_switch (see spawn()'s initial stack
; setup in process.c) -- never called directly. The two values it
; pops were planted on the stack by spawn(), right below the
; trampoline's own "return address" slot.
global kernel_thread_trampoline

kernel_thread_trampoline:
    pop rdi   ; entry_rip, planted by spawn()
    pop rsi   ; user_rsp, planted by spawn()

    mov ax, 0x33        ; user data selector (RPL3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push 0x33           ; SS
    push rsi            ; RSP (user stack)
    push 0x202          ; RFLAGS: reserved bit 1 set, IF (bit 9) set
    push 0x3B           ; CS (user code64, RPL3)
    push rdi            ; RIP (entry point)
    iretq
```

- [ ] **Step 5: Add `spawn()`/`task_exit()` and CR3 switching to the scheduler**

```c
// Added to kernel/process.h:
#define USER_STACK_PAGES 4
#define USER_STACK_TOP 0x0000700000000000ULL

struct task *spawn(const char *path);
void task_exit(int code);
```

```c
// Added #includes at the top of kernel/process.c:
#include "mm/heap.h"
#include "fat16.h"
#include "elf.h"

extern uint64_t p4_table[512];             // boot.asm's live PML4
extern void kernel_thread_trampoline(void); // context_switch.asm
```

```c
// Replaces schedule()'s body in kernel/process.c -- adds the CR3
// reload for tasks with their own address space (pml4_phys != 0);
// kernel-mode-only tasks (pml4_phys == 0, none exist after this task,
// but the check stays harmless) skip it entirely.
void schedule(void) {
    struct task *next = dequeue_ready();
    if (!next) {
        return;
    }

    struct task *prev = current;
    if (prev && prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        enqueue_ready(prev);
    }

    next->state = TASK_RUNNING;
    current = next;
    tss.rsp0 = next->kernel_stack_top;

    if (next->pml4_phys) {
        __asm__ volatile ("mov %0, %%cr3" :: "r"(next->pml4_phys) : "memory");
    }

    if (prev == next) {
        return;
    }

    static uint64_t discarded_rsp;
    context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);
}
```

```c
// Added to the end of kernel/process.c:

struct task *spawn(const char *path) {
    uint16_t cluster;
    uint32_t size;
    if (!fat16_find(path, &cluster, &size)) {
        serial_write_string("[process] spawn FAILED: file not found: ");
        serial_write_string(path);
        serial_write_string("\n");
        return 0;
    }

    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) {
        serial_write_string("[process] spawn FAILED: kmalloc failed for ELF image\n");
        return 0;
    }
    fat16_read_file(cluster, size, image);

    uint64_t pml4_phys = paging_alloc_pml4();
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    pml4[0] = p4_table[0];     // low identity map -- pmm.c/paging.c internals rely on it
    pml4[256] = p4_table[256]; // physmap
    pml4[511] = p4_table[511]; // kernel higher-half alias

    uint64_t entry;
    if (!elf_load(image, size, pml4, &entry)) {
        kfree(image);
        return 0;
    }
    kfree(image);

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc(0);
        uint64_t vaddr = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_map_into(pml4, vaddr, frame, PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_USER);
    }

    struct task *t = alloc_task_slot();
    if (!t) {
        serial_write_string("[process] spawn FAILED: no free task slot\n");
        return 0;
    }

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = USER_STACK_TOP;                    // user_rsp, popped by kernel_thread_trampoline
    *(--sp) = entry;                             // entry_rip, popped by kernel_thread_trampoline
    *(--sp) = (uint64_t)kernel_thread_trampoline; // context_switch's `ret` lands here
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->pid = next_pid++;
    t->state = TASK_READY;
    t->saved_rsp = (uint64_t)sp;
    t->kernel_stack_top = kstack_top;
    t->pml4_phys = pml4_phys;
    t->parent_pid = current ? current->pid : 0;
    t->exit_code = 0;
    t->next = 0;

    enqueue_ready(t);
    return t;
}

void task_exit(int code) {
    current->state = TASK_ZOMBIE;
    current->exit_code = code;
    serial_write_string("[process] task exited, pid=");
    serial_write_hex64((uint64_t)current->pid);
    serial_write_string(" code=");
    serial_write_hex64((uint64_t)(int64_t)code);
    serial_write_string("\n");
    schedule();
    for (;;) {
        __asm__ volatile ("hlt"); // unreachable: schedule() never resumes a ZOMBIE task
    }
}
```

- [ ] **Step 6: Write the syscall dispatch table and MSR setup**

```c
#ifndef NEOOS_SYSCALL_H
#define NEOOS_SYSCALL_H

void syscall_init(void);

#endif
```

```c
#include "syscall.h"
#include "gdt.h"
#include "serial.h"
#include "process.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

#define EFER_SCE (1ULL << 0)
#define EFER_NXE (1ULL << 11)

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_SPAWN  4
#define SYS_WAIT   5

extern void syscall_entry(void); // syscall_entry.asm

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

// Called only from syscall_entry.asm's `call syscall_dispatch`.
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    (void)a3;
    (void)a4;
    switch (num) {
        case SYS_EXIT:
            task_exit((int)a1);
            return 0; // unreachable -- task_exit never returns
        case SYS_WRITE:
            serial_write_string_n((const char *)(uintptr_t)a1, (uint64_t)a2);
            return a2;
        case SYS_GETPID:
            return current_task()->pid;
        default:
            serial_write_string("[syscall] unknown or not-yet-implemented syscall number\n");
            return -1;
    }
}

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    // EFER_NXE: elf_load (this task) is the first code in NeoOS to
    // actually set PAGE_NO_EXECUTE (bit 63) on a real PTE -- without
    // NXE enabled, that bit is reserved and setting it faults the
    // moment the page is walked. Grouped here since it's the same MSR
    // as EFER_SCE, not because it's conceptually part of SYSCALL setup.
    wrmsr(MSR_EFER, efer | EFER_SCE | EFER_NXE);

    // STAR[47:32] = kernel CS (kernel SS = that + 8, matches
    // GDT_KERNEL_DATA_SELECTOR at 0x10); STAR[63:48] = the SYSRET
    // base (user data at that+8, user code64 at that+16 -- see
    // Task 1's GDT layout).
    uint64_t star = ((uint64_t)GDT_USER_CODE32_SELECTOR << 48) | ((uint64_t)GDT_KERNEL_CODE_SELECTOR << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200); // mask IF (bit 9) on syscall entry

    serial_write_string("[syscall] SYSCALL/SYSRET configured\n");
}
```

- [ ] **Step 7: Write the SYSCALL entry stub**

```nasm
; kernel/syscall_entry.asm — SYSCALL entry point (target of LSTAR).
;
; Entered with: RCX = user RIP (return address), R11 = user RFLAGS,
; RAX = syscall number, RDI/RSI/RDX/R10 = args 1-4. CS/SS are already
; switched to kernel selectors (via STAR); RSP is UNCHANGED (still the
; user stack) -- SYSCALL never switches stacks automatically.

extern syscall_dispatch
extern tss

section .bss
align 8
user_rsp_scratch: resq 1

section .text
[bits 64]
global syscall_entry

syscall_entry:
    ; Swap onto the current task's kernel stack. tss.rsp0 is kept up
    ; to date by the scheduler on every context switch (see
    ; process.c's schedule()), so it always names the right stack
    ; regardless of which task is running. tss_entry.rsp0 sits at
    ; offset 4 (right after the packed struct's 4-byte reserved0).
    mov [rel user_rsp_scratch], rsp
    mov rsp, [rel tss + 4]

    push qword [rel user_rsp_scratch] ; user RSP
    push rcx                           ; user RIP
    push r11                           ; user RFLAGS
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Safe now that we're on the kernel stack with everything saved --
    ; keeps the system preemptible during (potentially long) syscall
    ; processing, mirroring SFMASK's guarantee that only the brief
    ; stack-swap above ran with interrupts off.
    sti

    ; Reorder incoming syscall args (rax=num, rdi=a1, rsi=a2, rdx=a3,
    ; r10=a4) into SysV call registers for syscall_dispatch (rdi=num,
    ; rsi=a1, rdx=a2, rcx=a3, r8=a4).
    mov r9, rax
    mov rax, r10
    mov r10, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, r9
    mov rcx, r10
    mov r8, rax

    call syscall_dispatch

    cli   ; mask again before restoring user state, mirroring SFMASK's entry guarantee
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11
    pop rcx
    pop qword [rel user_rsp_scratch]
    mov rsp, [rel user_rsp_scratch]

    o64 sysret
```

- [ ] **Step 8: Add `syscall_entry.asm` to the Makefile**

```makefile
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/isr_stubs.o $(BUILD_DIR)/context_switch.o $(BUILD_DIR)/syscall_entry.o

$(BUILD_DIR)/syscall_entry.o: kernel/syscall_entry.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/syscall_entry.asm -o $(BUILD_DIR)/syscall_entry.o
```

- [ ] **Step 9: Replace the kernel-thread test with a real spawned process**

Remove `kernel_thread_a`/`kernel_thread_b` from `kernel/kernel.c` entirely, and replace the end of `kmain` with:

```c
    process_init();
    syscall_init();

    struct task *spin_task = spawn("/BIN/SPIN.ELF");
    if (!spin_task) {
        serial_write_string("[process] spawn FAILED for /BIN/SPIN.ELF\n");
    }

    serial_write_string("NeoOS: interrupts enabled, starting scheduler\n");
    __asm__ volatile ("sti");

    schedule();
    for (;;) {
        __asm__ volatile ("hlt");
    }
```

Add `#include "syscall.h"` alongside the other includes.

- [ ] **Step 10: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with `-boot order=d ... -no-reboot -no-shutdown -d int,guest_errors -D /tmp/qemu-int.log`.
Expected: `[syscall] SYSCALL/SYSRET configured`, then `spin test program running` (written via the real `SYSCALL` → `write` path from ring 3), then `[process] task exited, pid=0x1 code=0x0` (via `sys_exit` → `SYS_EXIT` → `task_exit`). Zero `check_exception` count in the QEMU int log, zero `FAILED` in the serial log.

- [ ] **Step 11: Commit**

```bash
git add kernel/mm/paging.c kernel/mm/paging.h kernel/elf.c kernel/elf.h kernel/serial.c kernel/serial.h \
        kernel/context_switch.asm kernel/process.c kernel/process.h kernel/syscall.c kernel/syscall.h \
        kernel/syscall_entry.asm Makefile kernel/kernel.c
git commit -m "Add ELF loading, spawn(), and SYSCALL/SYSRET syscall boundary"
```

---

### Task 6: `yield`/`spawn`/`wait` Syscalls and the Parent/Child Lifecycle

**Files:**
- Modify: `kernel/process.h`, `kernel/process.c` (`waiting_for_pid` field, `wait_for_pid`, wake-up logic in `task_exit`)
- Modify: `kernel/syscall.c` (dispatch `SYS_YIELD`/`SYS_SPAWN`/`SYS_WAIT`)
- Modify: `userland/neoos_syscall.h` (add a shared `print_num` helper)
- Create: `userland/child.c`, `userland/parent.c`
- Modify: `Makefile` (build rules + disk-image entries for `CHILD.ELF`/`PARENT.ELF`; `USER_CFLAGS` gains `-mcmodel=large` -- see Global Constraints)
- Modify: `kernel/kernel.c` (spawn `/BIN/PARENT.ELF` instead of `/BIN/SPIN.ELF`)
- Modify: `kernel/syscall_entry.asm` (preserve `RDI`/`RSI`/`RDX`/`R8`/`R9`/`R10` across `syscall_dispatch`, not just the callee-saved registers -- see Global Constraints)

**Interfaces:**
- Consumes: `spawn`/`schedule`/`struct task` (Task 5).
- Produces: `int64_t wait_for_pid(int pid)` (blocks until the given PID becomes `TASK_ZOMBIE`, reaps it, returns its exit code).

- [ ] **Step 1: Add wait/wake-up bookkeeping to `struct task`**

```c
// process.h: struct task gains one field (shown in context):
struct task {
    int pid;
    enum task_state state;
    uint64_t saved_rsp;
    uint64_t kernel_stack_top;
    uint64_t pml4_phys;
    int parent_pid;
    int exit_code;
    int waiting_for_pid; // 0 = not blocked in wait(); else the PID this task is waiting on
    struct task *next;
};

int64_t wait_for_pid(int pid);
```

Initialize `waiting_for_pid = 0;` alongside the other field initializations in both `task_create_kernel_thread` and `spawn` (in `process.c`).

- [ ] **Step 2: Write `wait_for_pid` and the exit-time wake-up**

```c
// Added to kernel/process.c:

int64_t wait_for_pid(int pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid == pid && tasks[i].state == TASK_ZOMBIE) {
            int code = tasks[i].exit_code;
            tasks[i].state = TASK_UNUSED;
            return code;
        }
    }

    current->waiting_for_pid = pid;
    current->state = TASK_BLOCKED;
    schedule();

    // Resumed here once task_exit() (for our target pid) re-enqueued us.
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid == pid && tasks[i].state == TASK_ZOMBIE) {
            int code = tasks[i].exit_code;
            tasks[i].state = TASK_UNUSED;
            return code;
        }
    }
    return -1; // shouldn't happen given task_exit's wake-up guarantee
}
```

```c
// task_exit's body in kernel/process.c gains a wake-up scan, inserted
// right after logging and before the call to schedule():
void task_exit(int code) {
    current->state = TASK_ZOMBIE;
    current->exit_code = code;
    serial_write_string("[process] task exited, pid=");
    serial_write_hex64((uint64_t)current->pid);
    serial_write_string(" code=");
    serial_write_hex64((uint64_t)(int64_t)code);
    serial_write_string("\n");

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].waiting_for_pid == current->pid) {
            tasks[i].state = TASK_READY;
            tasks[i].waiting_for_pid = 0;
            enqueue_ready(&tasks[i]);
        }
    }

    schedule();
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
```

- [ ] **Step 3: Dispatch the three new syscalls**

```c
// kernel/syscall.c: syscall_dispatch's switch gains three cases
// (SYS_EXIT/SYS_WRITE/SYS_GETPID from Task 5 are unchanged):
        case SYS_YIELD:
            schedule();
            return 0;
        case SYS_SPAWN: {
            char path_buf[64];
            uint64_t len = (uint64_t)a2;
            if (len > sizeof(path_buf) - 1) {
                len = sizeof(path_buf) - 1;
            }
            const char *user_path = (const char *)(uintptr_t)a1;
            for (uint64_t i = 0; i < len; i++) {
                path_buf[i] = user_path[i];
            }
            path_buf[len] = '\0';
            struct task *child = spawn(path_buf);
            return child ? child->pid : -1;
        }
        case SYS_WAIT:
            return wait_for_pid((int)a1);
```

(These replace the `default:` fallthrough those three numbers previously hit.)

- [ ] **Step 4: Add a shared `print_num` helper to the userland syscall header**

```c
// Added to userland/neoos_syscall.h, before the #endif:
static inline void print_num(int64_t n) {
    char buf[24];
    int i = 0;
    int negative = 0;
    if (n < 0) {
        negative = 1;
        n = -n;
    }
    if (n == 0) {
        buf[i++] = '0';
    }
    while (n > 0) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    if (negative) {
        buf[i++] = '-';
    }
    char out[24];
    for (int j = 0; j < i; j++) {
        out[j] = buf[i - 1 - j];
    }
    sys_write(out, (uint64_t)i);
}
```

- [ ] **Step 5: Write the child and parent test programs**

```c
#include "neoos_syscall.h"

void _start(void) {
    const char msg[] = "child running, exiting with code 42\n";
    sys_write(msg, user_strlen(msg));
    sys_exit(42);
}
```

```c
#include "neoos_syscall.h"

void _start(void) {
    const char path[] = "/BIN/CHILD.ELF";
    int64_t child_pid = sys_spawn(path, user_strlen(path));
    int64_t exit_code = sys_wait(child_pid);

    const char prefix[] = "[parent] child exit code=";
    sys_write(prefix, user_strlen(prefix));
    print_num(exit_code);
    sys_write("\n", 1);
    sys_exit(0);
}
```

- [ ] **Step 6: Add build rules and disk-image entries**

```makefile
$(USERLAND_BUILD)/CHILD.ELF: $(USERLAND_DIR)/child.c $(USERLAND_DIR)/user.ld $(USERLAND_DIR)/neoos_syscall.h
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(USERLAND_DIR)/child.c

$(USERLAND_BUILD)/PARENT.ELF: $(USERLAND_DIR)/parent.c $(USERLAND_DIR)/user.ld $(USERLAND_DIR)/neoos_syscall.h
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(USERLAND_DIR)/parent.c
```

```makefile
$(DISK_IMG): $(USERLAND_BUILD)/SPIN.ELF $(USERLAND_BUILD)/CHILD.ELF $(USERLAND_BUILD)/PARENT.ELF
	mkdir -p $(DISK_SRC)/DIR
	printf 'Hello from NeoOS FAT16!\n' > $(DISK_SRC)/HELLO.TXT
	head -c 8192 /dev/zero | tr '\0' 'N' > $(DISK_SRC)/BIGFILE.TXT
	printf 'nested file contents\n' > $(DISK_SRC)/DIR/NESTED.TXT
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=32 status=none
	mkfs.fat -F 16 $(DISK_IMG)
	mcopy -i $(DISK_IMG) $(DISK_SRC)/HELLO.TXT ::HELLO.TXT
	mcopy -i $(DISK_IMG) $(DISK_SRC)/BIGFILE.TXT ::BIGFILE.TXT
	mmd -i $(DISK_IMG) ::DIR
	mcopy -i $(DISK_IMG) $(DISK_SRC)/DIR/NESTED.TXT ::DIR/NESTED.TXT
	mmd -i $(DISK_IMG) ::BIN
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SPIN.ELF ::BIN/SPIN.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/CHILD.ELF ::BIN/CHILD.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/PARENT.ELF ::BIN/PARENT.ELF
```

- [ ] **Step 7: Spawn `PARENT.ELF` from `kmain` instead of `SPIN.ELF`**

```c
    struct task *parent_task = spawn("/BIN/PARENT.ELF");
    if (!parent_task) {
        serial_write_string("[process] spawn FAILED for /BIN/PARENT.ELF\n");
    }
```

- [ ] **Step 8: Build and verify**

Run: `make clean && make disk-image && make iso`, boot as before with `-no-reboot -no-shutdown -d int,guest_errors`.
Expected serial log, after `[syscall] SYSCALL/SYSRET configured`: `child running, exiting with code 42`, `[process] task exited, pid=0x2 code=0x2a` (42 in hex), `[parent] child exit code=42`, `[process] task exited, pid=0x1 code=0x0` — proving the full spawn → run → exit → `wait` → reap lifecycle, with the parent's own output only appearing *after* it observes the child's real exit code. Zero exceptions, zero `FAILED`.

- [ ] **Step 9: Commit**

```bash
git add kernel/process.c kernel/process.h kernel/syscall.c userland/neoos_syscall.h \
        userland/child.c userland/parent.c Makefile kernel/kernel.c
git commit -m "Add yield/spawn/wait syscalls and parent/child lifecycle test"
```

---

### Task 7: Multi-Process Preemption and Yield-vs-Preemption Ordering

**Files:**
- Create: `userland/looper.c`, `userland/yielder.c`
- Modify: `Makefile` (build rules + disk-image entries for `LOOPER.ELF`/`YIELDER.ELF`)
- Modify: `kernel/kernel.c` (spawn two `LOOPER.ELF` instances and one `YIELDER.ELF` alongside `PARENT.ELF`)

**Interfaces:** None new — this task only adds test programs and spawns them.

- [ ] **Step 1: Write the looper and yielder test programs**

```c
#include "neoos_syscall.h"

void _start(void) {
    int64_t pid = sys_getpid();
    for (int i = 0; i < 30; i++) {
        const char prefix[] = "[looper pid=";
        sys_write(prefix, user_strlen(prefix));
        print_num(pid);
        const char suffix[] = "] tick\n";
        sys_write(suffix, user_strlen(suffix));
        for (volatile uint32_t spin = 0; spin < 3000000; spin++) {
        }
    }
    sys_exit(0);
}
```

```c
#include "neoos_syscall.h"

void _start(void) {
    int64_t pid = sys_getpid();
    for (int i = 0; i < 30; i++) {
        const char prefix[] = "[yielder pid=";
        sys_write(prefix, user_strlen(prefix));
        print_num(pid);
        const char suffix[] = "] tick\n";
        sys_write(suffix, user_strlen(suffix));
        sys_yield();
    }
    sys_exit(0);
}
```

`LOOPER.ELF` burns CPU between prints (like Task 4's kernel threads) so it only gives up the CPU when the timer forces it; `YIELDER.ELF` gives up the CPU immediately every iteration — the contrast between their interleave granularity is what Step 4 checks.

- [ ] **Step 2: Add build rules**

```makefile
$(USERLAND_BUILD)/LOOPER.ELF: $(USERLAND_DIR)/looper.c $(USERLAND_DIR)/user.ld $(USERLAND_DIR)/neoos_syscall.h
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(USERLAND_DIR)/looper.c

$(USERLAND_BUILD)/YIELDER.ELF: $(USERLAND_DIR)/yielder.c $(USERLAND_DIR)/user.ld $(USERLAND_DIR)/neoos_syscall.h
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(USERLAND_DIR)/yielder.c
```

- [ ] **Step 3: Add them to the disk image**

```makefile
$(DISK_IMG): $(USERLAND_BUILD)/SPIN.ELF $(USERLAND_BUILD)/CHILD.ELF $(USERLAND_BUILD)/PARENT.ELF $(USERLAND_BUILD)/LOOPER.ELF $(USERLAND_BUILD)/YIELDER.ELF
	mkdir -p $(DISK_SRC)/DIR
	printf 'Hello from NeoOS FAT16!\n' > $(DISK_SRC)/HELLO.TXT
	head -c 8192 /dev/zero | tr '\0' 'N' > $(DISK_SRC)/BIGFILE.TXT
	printf 'nested file contents\n' > $(DISK_SRC)/DIR/NESTED.TXT
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=32 status=none
	mkfs.fat -F 16 $(DISK_IMG)
	mcopy -i $(DISK_IMG) $(DISK_SRC)/HELLO.TXT ::HELLO.TXT
	mcopy -i $(DISK_IMG) $(DISK_SRC)/BIGFILE.TXT ::BIGFILE.TXT
	mmd -i $(DISK_IMG) ::DIR
	mcopy -i $(DISK_IMG) $(DISK_SRC)/DIR/NESTED.TXT ::DIR/NESTED.TXT
	mmd -i $(DISK_IMG) ::BIN
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SPIN.ELF ::BIN/SPIN.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/CHILD.ELF ::BIN/CHILD.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/PARENT.ELF ::BIN/PARENT.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/LOOPER.ELF ::BIN/LOOPER.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/YIELDER.ELF ::BIN/YIELDER.ELF
```

- [ ] **Step 4: Spawn two loopers and a yielder alongside the parent**

```c
    struct task *parent_task = spawn("/BIN/PARENT.ELF");
    if (!parent_task) {
        serial_write_string("[process] spawn FAILED for /BIN/PARENT.ELF\n");
    }
    spawn("/BIN/LOOPER.ELF");
    spawn("/BIN/LOOPER.ELF");
    spawn("/BIN/YIELDER.ELF");
```

With spawns happening in this order before the scheduler ever runs, PIDs are assigned deterministically: `PARENT`=1, the two `LOOPER` instances=2 and 3, `YIELDER`=4, and (later, spawned at runtime by `PARENT`) `CHILD`=5.

- [ ] **Step 5: Build and verify**

Run: `make clean && make disk-image && make iso`, boot as before, let it run several seconds.
Expected: `[looper pid=0x2]` and `[looper pid=0x3]` lines interleave in **bursts** (several lines from one PID, then a switch — the same pattern Task 4 proved for kernel threads, now proven for genuinely separate processes with their own address spaces, switched via real `CR3` reloads). `[yielder pid=0x4]` lines appear far more frequently interspersed among those bursts — often one yielder line between every burst, or even between individual looper lines — visibly distinguishing voluntary `yield()` from pure timer preemption. The parent/child lifecycle output from Task 6 still appears correctly, unaffected by the extra concurrent processes. Zero `FAILED`, zero exceptions.

- [ ] **Step 6: Commit**

```bash
git add userland/looper.c userland/yielder.c Makefile kernel/kernel.c
git commit -m "Add multi-process preemption and yield-ordering test programs"
```

---

### Task 8: Ring-3 Fault Path Verification

**Files:**
- Create: `userland/faulter.c`
- Modify: `Makefile` (build rule + disk-image entry for `FAULTER.ELF`)
- Modify: `kernel/kernel.c` (temporarily spawn only `FAULTER.ELF` to verify, then revert)

**Interfaces:** None new.

- [ ] **Step 1: Write the faulter test program**

```c
#include "neoos_syscall.h"

void _start(void) {
    const char msg[] = "faulter about to divide by zero\n";
    sys_write(msg, user_strlen(msg));
    __asm__ volatile ("divb %0" :: "r"((uint8_t)0));
    sys_exit(0); // unreachable
}
```

- [ ] **Step 2: Add the build rule and disk-image entry**

```makefile
$(USERLAND_BUILD)/FAULTER.ELF: $(USERLAND_DIR)/faulter.c $(USERLAND_DIR)/user.ld $(USERLAND_DIR)/neoos_syscall.h
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(USERLAND_DIR)/faulter.c
```

Add `$(USERLAND_BUILD)/FAULTER.ELF` to the `$(DISK_IMG)` rule's prerequisites and one more `mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/FAULTER.ELF ::BIN/FAULTER.ELF` line, following the same pattern as every prior task's disk-image extension.

- [ ] **Step 3: Temporarily spawn only the faulter**

The machine halts entirely once the fault handler runs, so isolate this check from Task 7's other processes rather than mixing their output in. Temporarily comment out the four `spawn(...)` calls Task 7 added and replace them with:

```c
    spawn("/BIN/FAULTER.ELF");
```

- [ ] **Step 4: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with `-serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown`, then also capture a `screendump` via the QEMU monitor.
Expected: serial shows `faulter about to divide by zero`, then `[exception] Divide Error (vector=0x0, error_code=0x0)` with `cs=0x000000000000003b` — confirming the fault was taken from ring 3 (user code64's selector, RPL 3), not ring 0 — followed by the same register dump format milestone 2 established, and the machine halts (no further ticks, no reboot). The VGA screendump shows the `EXCEPTION - HALTED` banner.

- [ ] **Step 5: Revert the temporary spawn change**

Restore Task 7's four `spawn(...)` calls (`PARENT.ELF`, two `LOOPER.ELF`, `YIELDER.ELF`), removing the temporary `FAULTER.ELF`-only spawn. Rebuild and confirm normal boot resumes exactly as Task 7 verified (same interleaving, same lifecycle output).

- [ ] **Step 6: Commit**

```bash
git add userland/faulter.c Makefile
git commit -m "Add ring-3 fault path test program (FAULTER.ELF)"
```

(`kernel/kernel.c` has no net diff after the revert in Step 5, so it isn't part of this commit.)

---

### Task 9: Final Integration and Full Verification

**Files:**
- None (verification-only task; fixes anything Steps 1-3 turn up).

**Interfaces:** None new — this task exercises everything Tasks 1-8 produced.

- [ ] **Step 1: Full boot log check**

Run: `make clean && make disk-image && make iso`, boot with `-boot order=d -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown -d int,guest_errors -D /tmp/qemu-int.log`, let it run several seconds.
Expected: `grep -c FAILED /tmp/neoos.log` and `grep -c check_exception /tmp/qemu-int.log` both `0`. The log shows, in order: milestones 2-4's unchanged sequence (`[pmm]`, `[paging]`, `[gdt]`, `[idt]`, `[acpi]`, `[pic]`, `[lapic]`, `[ioapic]`, `[timer] calibrated...`, `[heap]`, `[ata]`, `[fat16]`), `[process] initialized`, `[syscall] SYSCALL/SYSRET configured`, then the interleaved parent/child/looper/yielder output from Tasks 6-7, and periodic `[timer] tick=` lines continuing throughout.

- [ ] **Step 2: Regression check without the disk attached**

Run the same boot command but omitting `-drive file=build/disk.img,format=raw`.
Expected: every `spawn(...)` call in `kmain` fails cleanly (`[process] spawn FAILED: file not found: ...`, since `fat16_find` itself already fails cleanly per milestone 4's own regression test), the scheduler's ready queue ends up empty, and `schedule()` from `kmain` returns immediately (its `if (!next) return;` path) rather than switching into anything — falling through to `kmain`'s trailing `for(;;) hlt`. No crash, no hang, milestones 2-4's own initialization and self-tests still complete normally before this point.

- [ ] **Step 3: Commit**

Only if Steps 1-2 required fixes; otherwise this task produces no diff and needs no commit. If fixes were needed:

```bash
git add -A
git commit -m "Fix processes milestone integration issues found during full verification"
```
