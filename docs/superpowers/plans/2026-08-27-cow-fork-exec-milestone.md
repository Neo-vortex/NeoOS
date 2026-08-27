# NeoOS COW/fork/exec Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give NeoOS `fork()`/`exec()` process creation backed by copy-on-write physical-frame sharing, plus process address-space teardown on exit (a pre-existing gap: nothing is currently freed when a process exits).

**Architecture:** Extend `pmm.c`'s frame bookkeeping with per-frame refcounts; `fork()` duplicates a page table read-only and shares frames instead of copying them, with a new COW page-fault handler in `paging.c` doing the actual copy lazily on write. A new `fork_trampoline.asm` (mirroring the existing `kernel_thread_trampoline`) resumes the child in ring 3. `exec()` reuses a `spawn()`-derived address-space builder, replacing the calling task's address space in place. `task_exit()`/`wait_for_pid()` gain the teardown this all depends on being safe (freeing frames as they're unshared).

**Tech Stack:** Same toolchain as every prior milestone (NASM, `x86_64-elf-gcc` cross-compiler, GNU Make, QEMU 10.2.1).

**Spec:** `docs/superpowers/specs/2026-08-27-cow-fork-exec-design.md`

## Global Constraints

- Every user page is mapped writable today (`elf_load()`'s `flags = PAGE_WRITABLE | PAGE_USER` always) — `fork()` is the *only* thing that will ever mark a user PTE read-only in this milestone, so the COW fault handler may assume any user-mode present-page write fault is a COW fault, with no separate "real permission" check.
- `fork()`/`exec()` failure must never corrupt or kill the calling process — `fork()` fully unwinds partial work on failure; `exec()` builds and validates the entire new address space before freeing the old one.
- Frame sharing/refcounting is single-execution-context only — no locking added (matches every existing kernel data structure; SMP is a later, separately-brainstormed milestone).
- Verification throughout uses headless QEMU exactly as in every prior milestone: `-cpu Nehalem -boot order=d`, `-serial file:<path>`, `-no-reboot -no-shutdown -d int,guest_errors -D <path>`. No host-runnable unit tests exist for this project — every task's verification is a boot + serial log read.
- Where a task's verification requires temporarily changing `kernel/kernel.c`'s spawn calls, revert to the standard four-process boot (`PARENT.ELF`, two `LOOPER.ELF`, `YIELDER.ELF`) afterward and confirm `git diff --stat kernel/kernel.c` prints nothing before committing — matching the SSE milestone's established pattern.

---

### Task 1: Frame Reference Counting in the Physical Memory Manager

**Files:**
- Modify: `kernel/mm/pmm.c`, `kernel/mm/pmm.h`

**Interfaces:**
- Produces: `void pmm_frame_share(uint64_t phys)`; `unsigned pmm_frame_refcount(uint64_t phys)`. Consumed by Task 4 (COW fault handler) and Task 5 (`fork_task`).
- Consumes: nothing new.

- [ ] **Step 1: Add the refcount array and update `pmm_alloc`/`pmm_free`**

In `kernel/mm/pmm.c`, add alongside the existing `frame_order` array:

```c
// One entry per frame: how many live mappings point at it. pmm_alloc()
// sets this to 1 (sole owner, as always); fork()'s COW duplication is
// the only caller that ever raises it above 1 (via pmm_frame_share()).
// pmm_free() decrements instead of unconditionally freeing, so a
// COW-shared frame only actually returns to the allocator once every
// sharer has released it -- every pre-existing caller (kernel stacks,
// page tables, ELF segments) allocates at refcount 1 and frees exactly
// once, so their behavior is unchanged.
static uint16_t frame_refcount[PMM_MAX_FRAMES];
```

In `pmm_init`, alongside the existing `frame_order` reset loop, add:

```c
    for (uint64_t i = 0; i < PMM_MAX_FRAMES; i++) {
        frame_refcount[i] = 0;
    }
```

In `pmm_alloc`, right before `total_free_frames -= (1ULL << order);`, set the refcount for every frame in the newly allocated block:

```c
    for (uint64_t i = 0; i < (1ULL << order); i++) {
        frame_refcount[phys_to_frame(phys) + i] = 1;
    }
    total_free_frames -= (1ULL << order);
    return phys;
```

Change `pmm_free`'s signature-adjacent behavior: at the very top of the function, before any coalescing logic, decrement every covered frame's refcount and bail out (without freeing anything) if any frame in the block is still shared:

```c
void pmm_free(uint64_t phys_addr, unsigned order) {
    uint64_t frame = phys_to_frame(phys_addr);

    for (uint64_t i = 0; i < (1ULL << order); i++) {
        frame_refcount[frame + i]--;
    }
    for (uint64_t i = 0; i < (1ULL << order); i++) {
        if (frame_refcount[frame + i] != 0) {
            return; // still shared -- not actually free yet
        }
    }

    total_free_frames += (1ULL << order); // caller's block wasn't counted as free before this call

    while (order < PMM_MAX_ORDER) {
        uint64_t buddy_frame = frame ^ (1ULL << order);
        if (buddy_frame >= PMM_MAX_FRAMES || frame_order[buddy_frame] != order) {
            break;
        }
        // Buddy is free at the same order: unlink it and merge upward.
        // Its frames are already counted in total_free_frames from when
        // it was freed, so no further accounting is needed here.
        list_remove(order, (struct free_block *)(uintptr_t)frame_to_phys(buddy_frame));
        frame = (frame < buddy_frame) ? frame : buddy_frame;
        order++;
    }

    list_push(order, (struct free_block *)(uintptr_t)frame_to_phys(frame));
}
```

Add the two new public functions at the end of the file, before `pmm_selftest`:

```c
void pmm_frame_share(uint64_t phys) {
    frame_refcount[phys_to_frame(phys)]++;
}

unsigned pmm_frame_refcount(uint64_t phys) {
    return frame_refcount[phys_to_frame(phys)];
}
```

- [ ] **Step 2: Declare the new functions in `kernel/mm/pmm.h`**

Add after `void pmm_free(uint64_t phys_addr, unsigned order);`:

```c
// Increments a single frame's reference count by 1. The only way a
// frame's count ever rises above 1 -- called once per shared page by
// fork()'s address-space duplication.
void pmm_frame_share(uint64_t phys);

// Returns a frame's current reference count. 1 means "sole owner";
// pmm_free() only actually returns a frame to the allocator once its
// count reaches 0.
unsigned pmm_frame_refcount(uint64_t phys);
```

- [ ] **Step 3: Extend `pmm_selftest` to cover sharing**

In `kernel/mm/pmm.c`, add to the end of `pmm_selftest` (before its final `serial_write_string("[pmm] selftest passed...")` call), reusing the same baseline-comparison style as the existing test:

```c
    uint64_t shared_block = pmm_alloc(0);
    if (!shared_block) {
        serial_write_string("[pmm] selftest FAILED: share test alloc returned 0\n");
        return;
    }
    if (pmm_frame_refcount(shared_block) != 1) {
        serial_write_string("[pmm] selftest FAILED: fresh alloc refcount != 1\n");
        return;
    }
    pmm_frame_share(shared_block);
    if (pmm_frame_refcount(shared_block) != 2) {
        serial_write_string("[pmm] selftest FAILED: refcount != 2 after share\n");
        return;
    }
    pmm_free(shared_block, 0); // drops to 1 -- must NOT return to the free list yet
    if (frame_order[phys_to_frame(shared_block)] != ORDER_NONE) {
        serial_write_string("[pmm] selftest FAILED: shared frame freed while still referenced\n");
        return;
    }
    pmm_free(shared_block, 0); // drops to 0 -- now it should actually free
    if (frame_order[phys_to_frame(shared_block)] == ORDER_NONE) {
        serial_write_string("[pmm] selftest FAILED: frame not returned to free list at refcount 0\n");
        return;
    }
```

- [ ] **Step 4: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with
`-cpu Nehalem -boot order=d -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown -d int,guest_errors -D /tmp/qemu-int.log`.
Expected: `[pmm] selftest passed, free_frames=...` still appears (now exercising the new share/refcount assertions too), rest of milestone 5-8's boot lifecycle unaffected, zero `FAILED`, zero exceptions.

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/pmm.c kernel/mm/pmm.h
git commit -m "Add per-frame reference counting to the physical memory manager"
```

---

### Task 2: Expose the Saved Syscall Frame to `syscall_dispatch`

**Files:**
- Modify: `kernel/syscall_entry.asm`, `kernel/syscall.c`, `kernel/process.h`

**Interfaces:**
- Produces: `struct syscall_frame` (in `process.h`); `syscall_dispatch`'s new 6th parameter `struct syscall_frame *frame`. Consumed by Task 5 (`fork_task`) and Task 6 (`exec_task`).
- Consumes: nothing new. This task is pure plumbing -- the new parameter is accepted but unused until Task 5.

- [ ] **Step 1: Define `struct syscall_frame` in `kernel/process.h`**

Add near the top of `kernel/process.h`, after the includes:

```c
// Mirrors syscall_entry.asm's saved-register block exactly, in
// increasing-address order (the reverse of push order, since the
// last register pushed ends up at the lowest address). A pointer to
// the base of this block -- which already equals RSP right after the
// pushes, before `call syscall_dispatch` -- is passed into
// syscall_dispatch as its 6th argument, letting fork() copy a
// caller's full user-mode context and exec() overwrite its own
// return RIP/RSP in place.
struct syscall_frame {
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r11;       // user RFLAGS
    uint64_t rcx;       // user RIP
    uint64_t user_rsp;
};
```

- [ ] **Step 2: Pass the frame pointer from `syscall_entry.asm`**

In `kernel/syscall_entry.asm`, the existing register-reorder block ends with `mov r8, rax`. Add one line right after it, before `call syscall_dispatch`:

```nasm
    mov rcx, r10
    mov r8, rax
    mov r9, rsp   ; base of the saved-register block -- syscall_dispatch's 6th argument

    call syscall_dispatch
```

(`r9` was only used transiently as scratch earlier in the reorder block, so overwriting it here is safe -- its scratch value is no longer needed.)

- [ ] **Step 3: Update `syscall_dispatch`'s signature in `kernel/syscall.c`**

Change:

```c
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    (void)a4;
```

to:

```c
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4, struct syscall_frame *frame) {
    (void)a4;
    (void)frame; // unused until Task 5 (fork) and Task 6 (exec)
```

Add `#include "process.h"` to `kernel/syscall.c` if not already present (it already is, per the existing `spawn`/`wait_for_pid`/`current_task` calls).

- [ ] **Step 4: Build and verify full regression**

Run: `make clean && make disk-image && make iso`, boot with `-cpu Nehalem` as in Task 1.
Expected: milestone 5-8's exact full lifecycle reproduces unchanged (this task adds a new argument that is read but never acted on -- a complete no-op for existing behavior, same verification logic as the SSE plan's Task 2). Zero `FAILED`, zero exceptions.

- [ ] **Step 5: Commit**

```bash
git add kernel/syscall_entry.asm kernel/syscall.c kernel/process.h
git commit -m "Expose the saved syscall frame to syscall_dispatch"
```

---

### Task 3: Process Address-Space Teardown on Exit

**Files:**
- Modify: `kernel/mm/paging.c`, `kernel/mm/paging.h`, `kernel/process.c`, `kernel/kernel.c` (temporary, reverted)

**Interfaces:**
- Produces: `void free_address_space(uint64_t pml4_phys)`. Consumed by Task 6 (`exec_task`, to free the old address space) and by `task_exit()` in this task.
- Consumes: `pmm_free` (Task 1's refcount-aware version already applies automatically -- no new dependency on Task 1's new functions specifically).

- [x] **Step 1: Write `free_address_space` in `kernel/mm/paging.c`**

Add near the end of the file, before `paging_selftest`:

```c
// Frees every user-mapped frame and page-table frame reachable from
// pml4_phys, then the PML4 frame itself. The three shared kernel
// entries (identity map, physmap, kernel higher-half alias -- see
// spawn()'s pml4[0]/[256]/[511] setup) are never walked into or
// freed: they point at kernel-owned tables no process owns.
// pmm_free() is refcount-aware (see pmm.c) -- a COW-shared frame only
// actually returns to the allocator once every sharer has released
// it, so calling this on a fork()'d child's or parent's address space
// is always safe regardless of sharing.
void free_address_space(uint64_t pml4_phys) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

    for (unsigned i4 = 0; i4 < 512; i4++) {
        if (i4 == 0 || i4 == PHYSMAP_PML4_INDEX || i4 == 511) {
            continue; // shared kernel entries -- not owned by this address space
        }
        if (!(pml4[i4] & PAGE_PRESENT)) {
            continue;
        }
        uint64_t pdpt_phys = pml4[i4] & PAGE_ADDR_MASK;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);

        for (unsigned i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PAGE_PRESENT)) {
                continue;
            }
            uint64_t pd_phys = pdpt[i3] & PAGE_ADDR_MASK;
            uint64_t *pd = (uint64_t *)phys_to_virt(pd_phys);

            for (unsigned i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PAGE_PRESENT)) {
                    continue;
                }
                uint64_t pt_phys = pd[i2] & PAGE_ADDR_MASK;
                uint64_t *pt = (uint64_t *)phys_to_virt(pt_phys);

                for (unsigned i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & PAGE_PRESENT) {
                        pmm_free(pt[i1] & PAGE_ADDR_MASK, 0);
                    }
                }
                pmm_free(pt_phys, 0);
            }
            pmm_free(pd_phys, 0);
        }
        pmm_free(pdpt_phys, 0);
    }

    pmm_free(pml4_phys, 0);
}
```

- [x] **Step 2: Declare it in `kernel/mm/paging.h`**

Add after `uint64_t paging_alloc_pml4(void);`:

```c
// Frees every frame belonging to the address space rooted at
// pml4_phys (user pages, page-table frames, and the PML4 itself),
// leaving the three shared kernel entries untouched. Safe to call on
// the currently-loaded CR3 -- kernel code runs via the shared
// higher-half mapping, never through the process's own user PTEs.
void free_address_space(uint64_t pml4_phys);
```

`PHYSMAP_PML4_INDEX` is already defined in `paging.c` (used by `paging_init`) and `free_address_space` lives in the same file, so no header change is needed for that constant.

- [x] **Step 3: Call `free_address_space` from `task_exit`, defer kernel-stack freeing to reap time**

In `kernel/process.c`, `struct task` needs its kernel stack's physical address and order recorded so it can be freed later -- add two fields to `kernel/process.h`'s `struct task`, right after `uint64_t kernel_stack_top;`:

```c
    uint64_t kernel_stack_phys; // for freeing at reap time (see wait_for_pid)
```

In `kernel/process.c`, set this new field in both `task_create_kernel_thread` (right after `uint64_t stack_phys = pmm_alloc(KERNEL_STACK_ORDER);`) and `spawn` (right after `uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);`):

```c
    t->kernel_stack_phys = stack_phys; // task_create_kernel_thread
```
```c
    t->kernel_stack_phys = kstack_phys; // spawn
```

In `task_exit`, right after the existing `serial_write_string(...)` block that logs the exit and before the loop that wakes waiters, free the address space:

```c
    if (current->pml4_phys) {
        free_address_space(current->pml4_phys);
        current->pml4_phys = 0;
    }
```

In `wait_for_pid`, both places that currently do:

```c
    if (tasks[i].pid == pid && tasks[i].state == TASK_ZOMBIE) {
        int code = tasks[i].exit_code;
        tasks[i].state = TASK_UNUSED;
        return code;
    }
```

change to also free the kernel stack now that it's safe (the reaper is a different running task):

```c
    if (tasks[i].pid == pid && tasks[i].state == TASK_ZOMBIE) {
        int code = tasks[i].exit_code;
        pmm_free(tasks[i].kernel_stack_phys, KERNEL_STACK_ORDER);
        tasks[i].state = TASK_UNUSED;
        return code;
    }
```

(Both occurrences in `wait_for_pid` need this change -- the immediate-zombie-found case and the resumed-after-blocking case.)

Add `#include "mm/paging.h"` to `kernel/process.c` if not already present (it already is, per the existing `paging_alloc_pml4`/`paging_map_into` calls).

**Required companion fix, found by actually running this task's verification loop (Step 5) rather than by inspection:** `schedule()` currently only reloads `CR3` when the *next* task has a nonzero `pml4_phys`:

```c
    if (next->pml4_phys) {
        __asm__ volatile ("mov %0, %%cr3" :: "r"(next->pml4_phys) : "memory");
    }
```

Switching to a kernel-mode-only task (`pml4_phys == 0`, e.g. `leak_test_thread` below) leaves `CR3` completely unchanged -- pointing at whatever the *previous* task's PML4 was. Before this step, that was harmless: an exited process's PML4 just leaked, unused-but-intact. Now that `task_exit` actually frees that PML4 frame back to the allocator, a stale `CR3` left pointing at it can get silently reused (and zeroed, by `alloc_table_frame`) by the very next `pmm_alloc(0)` -- corrupting the page table the CPU is still actively translating through, including the identity-map entry (`pml4[0]`) that `pmm.c`'s own free-list code relies on for its raw-physical-address pointers. This manifests as an unrelated-looking page fault deep inside `pmm_free`'s buddy-coalescing logic, on a physical address that "should" be identity-mapped. Fix `schedule()` to always establish a definite `CR3`:

```c
    // Always establish a definite CR3, even for a kernel-mode-only task
    // (pml4_phys == 0 -- falls back to the kernel's own never-freed
    // p4_table). Leaving CR3 unchanged in that case used to be harmless
    // (an exited process's now-zombie PML4 just leaked, unused-but-
    // intact memory), but now that task_exit() actually frees a
    // process's PML4 frame back to the allocator, a stale CR3 left
    // pointing at it could get silently reused and overwritten by the
    // very next pmm_alloc() -- corrupting the page table the CPU is
    // still actively translating through.
    uint64_t next_cr3 = next->pml4_phys ? next->pml4_phys : (uint64_t)(uintptr_t)p4_table;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(next_cr3) : "memory");
```

(`p4_table` is already declared `extern` in `process.c`, and its symbol address is already its physical address -- boot.asm's own low-memory data, never relocated to the kernel's higher-half link addresses, exactly as `spawn()`'s existing `pml4[0] = p4_table[0]` etc. already rely on.)

**Second required companion fix, also found by running the standard four-process boot after the above** (not just this task's own temporary verification loop): `task_exit()`'s critical section -- from marking itself `TASK_ZOMBIE` through calling `free_address_space()` and waking blocked waiters -- runs with interrupts enabled, same as it always has. Before this task, that section was a handful of `serial_write_string` calls and a short loop over `MAX_TASKS` (16) entries -- microseconds, far shorter than the ~10ms timer tick, so a preempting timer interrupt landing inside it was possible in principle but never actually observed. `free_address_space()`'s full page-table walk makes that window meaningfully longer, and a preemption mid-walk is fatal in a new way: the task's state is already `TASK_ZOMBIE`, not `TASK_READY`, so `schedule()`'s preemption path (`if (prev && prev->state == TASK_RUNNING) { ...re-enqueue... }`) never re-enqueues it -- abandoning `task_exit()` forever, mid-free, before it ever reaches its own trailing `schedule()` call.

The fix is the same uniprocessor-critical-section idiom `syscall.c`'s `fs_lock_acquire` already uses (see its comment: "cli/sti around the test-and-set is enough since interrupts are the only source of preemption here") -- but with one extra subtlety specific to `task_exit()`: **the trailing `sti` before its final `schedule()` call is required, not optional.** `EFLAGS.IF` is not part of a task's saved context (`context_switch.asm` never pushes/pops it), so for a voluntary (non-interrupt-driven) switch, whatever `IF` value is ambient at the moment of the switch simply carries over to the next task's resumption point. Every other `schedule()` call site in this kernel (`SYS_YIELD`, `wait_for_pid`'s blocking path, `kmain`'s one-time bootstrap call) already relies on `IF` being 1 at the point it calls `schedule()` -- leaving it cleared here would silently and permanently disable preemption for whichever task runs next, and everything switched to after that, until some unrelated `sti` elsewhere happens to fix it back up by luck.

In `kernel/process.c`'s `task_exit`, wrap from just before the `free_address_space` call through the end of the wake-loop:

```c
    // Uniprocessor critical section (same reasoning as syscall.c's
    // fs_lock): free_address_space() is now long enough (a full
    // page-table walk) that a timer interrupt landing mid-walk would
    // preempt this task -- and since its state is already ZOMBIE (not
    // READY), schedule() would never re-enqueue it, permanently
    // abandoning task_exit() before it reaches its own schedule() call
    // below. sti before that call is required, not optional: EFLAGS.IF
    // is not part of a task's saved context (context_switch.asm never
    // touches it), so every other schedule() caller in this kernel
    // relies on IF already being 1 -- leaving it cleared here would
    // silently disable preemption for whichever task runs next.
    __asm__ volatile ("cli");

    if (current->pml4_phys) {
        free_address_space(current->pml4_phys);
        current->pml4_phys = 0;
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].waiting_for_pid == current->pid) {
            tasks[i].state = TASK_READY;
            tasks[i].waiting_for_pid = 0;
            enqueue_ready(&tasks[i]);
        }
    }

    __asm__ volatile ("sti");
    schedule();
```

**Third required companion fix, also found by running the standard four-process boot:** `task_exit()` must switch `CR3` off the dying address space *before* calling `free_address_space()`. This is a distinct bug from the stale-`CR3` one fixed in `schedule()` above -- that one was about a *later* task translating through a freed PML4; this one corrupts the PML4 *during the very call that frees it*.

A frame stops being page-table data the instant `pmm_free()` takes it. `pmm.c`'s buddy allocator stores each free block's `next`/`prev` links **inside the block's own first 16 bytes** (`struct free_block`), so `free_address_space`'s closing `pmm_free(pml4_phys, 0)` writes a pointer pair straight over `pml4[0]` and `pml4[1]` of the table the CPU is still translating through. `pml4[0]` is the low identity map -- the one `pmm.c` itself dereferences free blocks through -- so `list_push`'s second store unmaps the identity map, and its third store faults on it:

```c
block->prev = 0;                                        // -> pml4[1] = 0        (harmless)
block->next = free_lists[order];                        // -> pml4[0] = 0x428000 (present bit now CLEAR)
if (free_lists[order]) free_lists[order]->prev = block; // -> #PF at 0x428008
```

Observed exactly that: `Page Fault error_code=0x2` (write, not-present), `cr2=0x428008`, `rax=0x428000`, with the debug log confirming `live_cr3 == pml4_phys` on every `free_address_space` call. It presents as an intermittent fault at a *varying* address, because it only triggers when the freed PML4 frame happens to coalesce into the **head** of the merged buddy block (the only frame `list_push` writes into) -- hence "usually the third or fourth process to exit."

In `kernel/process.c`'s `task_exit`, load the kernel's own PML4 first:

```c
    if (current->pml4_phys) {
        // Leave the dying address space BEFORE freeing it. [...see the
        // comment in process.c for the full explanation...]
        __asm__ volatile ("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)p4_table) : "memory");
        free_address_space(current->pml4_phys);
        current->pml4_phys = 0;
    }
```

Switching to `p4_table` is safe here for the same reason `schedule()`'s fallback is: kernel text (`PML4[511]`) and the physmap (`PML4[256]`) both live there, and nothing in the teardown path runs through user mappings. Note that this makes `free_address_space`'s "never freed while live in CR3" requirement a real precondition -- documented on the function in both `paging.c` and `paging.h`.

- [x] **Step 4: Temporarily verify reclamation with a repeated spawn+wait loop**

`wait_for_pid()` requires a valid `current` task -- `kmain()` itself runs *before* the scheduler's first switch and has no such task (`current` is `0` there), so calling `wait_for_pid()` directly from inline `kmain()` code null-derefs. Run the loop as a real kernel thread instead, via the existing `task_create_kernel_thread()` (already intended for early tests, per its own doc comment).

In `kernel/kernel.c`, add above `kmain`:

```c
// Temporary verification for the address-space-teardown-on-exit change:
// runs as a real kernel thread (not inline in kmain) because
// wait_for_pid() requires a valid current task -- kmain itself runs
// before the scheduler's first switch and has no such task.
static void leak_test_thread(void) {
    serial_write_string("[test] free_frames before loop=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string("\n");

    for (int i = 0; i < 5; i++) {
        struct task *child = spawn("/BIN/CHILD.ELF");
        if (!child) {
            serial_write_string("[test] spawn FAILED\n");
        } else {
            wait_for_pid(child->pid);
        }
    }

    serial_write_string("[test] free_frames after loop=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string("\n");

    task_exit(0);
}
```

Replace the four `spawn(...)` calls (and the `parent_task` FAILED check) with:

```c
    task_create_kernel_thread(leak_test_thread);
```

Add `#include "mm/pmm.h"` to `kernel/kernel.c` if not already present (it already is, per the existing `pmm_init`/`pmm_selftest` calls).

- [x] **Step 5: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with `-cpu Nehalem` as in Task 1.
Expected: `[test] free_frames before loop=X` and `[test] free_frames after loop=X` print the **exact same value** -- proving each spawn+exit+wait cycle now fully reclaims `CHILD.ELF`'s address space (before this task, the "after" value would be measurably lower, since each cycle's user pages, page tables, and PML4 leaked permanently). Zero `FAILED`, zero exceptions.

- [x] **Step 6: Revert the temporary loop and verify no regression**

Restore `kernel/kernel.c`'s original `spawn("/BIN/PARENT.ELF")` + `parent_task` FAILED check + two `spawn("/BIN/LOOPER.ELF")` + `spawn("/BIN/YIELDER.ELF")` calls. Confirm `git diff --stat kernel/kernel.c` prints nothing.

Rebuild and boot again with `-cpu Nehalem`. Expected: milestone 5-8's exact full lifecycle reproduces, zero `FAILED`, zero exceptions.

- [ ] **Step 7: Commit**

```bash
git add kernel/mm/paging.c kernel/mm/paging.h kernel/process.c kernel/process.h
git commit -m "Free a process's address space on exit; defer kernel-stack free to reap time"
```

---

### Task 4: COW Page-Fault Handler

**Files:**
- Modify: `kernel/mm/paging.c`, `kernel/mm/paging.h`, `kernel/isr.c`

**Interfaces:**
- Produces: `int paging_handle_cow_fault(uint64_t pml4_phys, uint64_t fault_addr)`. Consumed by `isr.c` in this task; exercised for the first time by Task 5's `fork_task`.
- Consumes: `pmm_frame_refcount`, `pmm_frame_share`-created shared state (Task 1); `current_task()->pml4_phys` (existing).

This task adds a dormant code path: nothing before Task 5 ever marks a user PTE read-only, so this handler is never actually triggered yet. Verification is a full-regression no-op check, same reasoning as Task 2.

- [x] **Step 1: Write `paging_handle_cow_fault` in `kernel/mm/paging.c`**

Add near the end of the file, after `free_address_space`:

```c
// Called from isr.c on a #PF with error_code bits present=1, write=1,
// user=1. Since fork() (Task 5) is the only thing in this kernel that
// ever marks a user PTE read-only, any such fault is assumed to be
// copy-on-write, never a genuine permission violation -- there is no
// separate "real read-only segment" concept yet (elf_load() always
// maps PAGE_WRITABLE). Returns 1 if handled (safe to return to the
// faulting instruction, which will now succeed), 0 if this wasn't
// actually a recognized COW fault (caller should fall through to the
// existing fatal exception path -- covers genuine bugs, wild
// pointers, and non-present accesses unchanged).
int paging_handle_cow_fault(uint64_t pml4_phys, uint64_t fault_addr) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    uint64_t *pdpt = table_entry(pml4, PML4_INDEX(fault_addr), 0, 0);
    uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(fault_addr), 0, 0) : 0;
    uint64_t *pt   = pd ? table_entry(pd, PD_INDEX(fault_addr), 0, 0) : 0;
    if (!pt) {
        return 0;
    }

    unsigned pt_index = PT_INDEX(fault_addr);
    uint64_t entry = pt[pt_index];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_WRITABLE)) {
        return 0; // not present, or already writable -- not a COW fault
    }

    uint64_t old_phys = entry & PAGE_ADDR_MASK;
    uint64_t flags_no_addr = entry & ~PAGE_ADDR_MASK & ~PAGE_PRESENT;

    if (pmm_frame_refcount(old_phys) == 1) {
        // Sole remaining owner -- no copy needed, just re-enable write.
        pt[pt_index] = entry | PAGE_WRITABLE;
    } else {
        uint64_t new_phys = pmm_alloc(0);
        if (!new_phys) {
            return 0; // out of memory -- caller's fatal path will report this fault
        }
        uint8_t *src = (uint8_t *)phys_to_virt(old_phys);
        uint8_t *dst = (uint8_t *)phys_to_virt(new_phys);
        for (int i = 0; i < 4096; i++) {
            dst[i] = src[i];
        }
        pt[pt_index] = (new_phys & PAGE_ADDR_MASK) | flags_no_addr | PAGE_PRESENT | PAGE_WRITABLE;
        pmm_free(old_phys, 0); // drops this task's share
    }

    __asm__ volatile ("invlpg (%0)" :: "r"(fault_addr) : "memory");
    return 1;
}
```

- [x] **Step 2: Declare it in `kernel/mm/paging.h`**

Add after `void free_address_space(uint64_t pml4_phys);`:

```c
// Handles a write fault on a copy-on-write page (see fork(), Task 5).
// Returns 1 if handled, 0 if this wasn't a recognized COW fault.
int paging_handle_cow_fault(uint64_t pml4_phys, uint64_t fault_addr);
```

- [x] **Step 3: Hook it into `kernel/isr.c`'s exception path**

`isr_handler` currently does, unconditionally for any vector < 32:

```c
void isr_handler(struct registers *regs) {
    if (regs->vector_number < 32) {
        exception_dump_and_halt(regs);
        return;
    }
```

Change to check for a COW-handleable page fault first:

```c
void isr_handler(struct registers *regs) {
    if (regs->vector_number == 14) {
        uint64_t present_write_user = 0x7; // P=1, W=1, U=1
        if ((regs->error_code & present_write_user) == present_write_user) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            if (paging_handle_cow_fault(current_task()->pml4_phys, cr2)) {
                return;
            }
        }
    }

    if (regs->vector_number < 32) {
        exception_dump_and_halt(regs);
        return;
    }
```

Add `#include "mm/paging.h"` and `#include "process.h"` to `kernel/isr.c`.

- [x] **Step 4: Build and verify full regression**

Run: `make clean && make disk-image && make iso`, boot with `-cpu Nehalem` as in Task 1.
Expected: milestone 5-8's exact full lifecycle reproduces unchanged -- no existing code path ever produces a present+read-only user PTE, so `paging_handle_cow_fault` is never actually invoked yet; this is a pure dormant-code-path addition. Zero `FAILED`, zero exceptions.

- [x] **Step 5: Commit**

```bash
git add kernel/mm/paging.c kernel/mm/paging.h kernel/isr.c
git commit -m "Add copy-on-write page-fault handler (dormant until fork() exists)"
```

---

### Task 5: `fork()` — Address-Space Duplication and Syscall

**Files:**
- Create: `kernel/fork_trampoline.asm`, `userland/fork_test.c`
- Modify: `kernel/process.c`, `kernel/process.h`, `kernel/syscall.c`, `lib/syscall.c`, `lib/include/unistd.h`, `docs/stdlib.md`, `Makefile`, `kernel/kernel.c` (temporary, reverted)

**Interfaces:**
- Produces: `struct task *fork_task(struct syscall_frame *frame)` (`process.c`); `SYS_FORK` syscall number 12; `int fork(void)` (stdlib).
- Consumes: `pmm_frame_share`/`pmm_frame_refcount` (Task 1), `struct syscall_frame` (Task 2), `paging_handle_cow_fault` (Task 4, exercised for the first time here).

- [x] **Step 1: Write `kernel/fork_trampoline.asm`**

```nasm
; kernel/fork_trampoline.asm — bootstraps a fork()'d child's very
; first entry into ring 3, resuming mid-program instead of at a fresh
; entry point (contrast kernel_thread_trampoline in context_switch.asm,
; used by spawn() for brand-new processes). Reached via a bare `ret`
; out of context_switch (see fork_task()'s initial stack setup in
; process.c) -- never called directly. The values it pops were planted
; on the child's kernel stack by fork_task(), copied from the parent's
; saved syscall frame at the moment of the fork() call.

section .text
[bits 64]
global fork_trampoline

fork_trampoline:
    ; rbx/rbp/r12-r15 are NOT popped here -- context_switch's own
    ; epilogue (the `pop r15/r14/r13/r12/rbx/rbp` sequence right before
    ; its `ret`) already consumed those stack slots and restored the
    ; real registers before landing here, exactly as for
    ; kernel_thread_entry_trampoline. Only the slots fork_task() placed
    ; ABOVE this trampoline's own return-address slot remain on the
    ; stack at this point.
    pop rcx   ; user RIP (parent's, at the point it called fork())
    pop r11   ; user RFLAGS
    pop rsi   ; user RSP

    xor eax, eax        ; fork() returns 0 in the child

    mov dx, 0x33        ; user data selector (RPL3)
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    push 0x33           ; SS
    push rsi            ; RSP (user stack)
    push r11            ; RFLAGS
    push 0x3B           ; CS (user code64, RPL3)
    push rcx            ; RIP
    iretq
```

- [x] **Step 2: Add `SYS_FORK`, `struct syscall_frame` field access, and `fork_task` in `kernel/process.c`**

Add near the top of `kernel/process.c`, alongside the other `extern` declarations:

```c
extern void fork_trampoline(void);
```

Add a new function, after `spawn`:

```c
// Duplicates the calling task into a new child, sharing physical
// frames read-only between the two (see paging_handle_cow_fault for
// the lazy-copy side). Returns the child task on success (the parent
// syscall path returns its pid), or 0 on failure -- leaving the
// parent completely unaffected (nothing is left partially modified).
struct task *fork_task(struct syscall_frame *frame) {
    struct task *parent = current;

    struct task *child = alloc_task_slot();
    if (!child) {
        serial_write_string("[process] fork FAILED: no free task slot\n");
        return 0;
    }

    uint64_t child_pml4_phys = paging_alloc_pml4();
    uint64_t *child_pml4 = (uint64_t *)phys_to_virt(child_pml4_phys);
    uint64_t *parent_pml4 = (uint64_t *)phys_to_virt(parent->pml4_phys);
    child_pml4[0] = parent_pml4[0];
    child_pml4[256] = parent_pml4[256];
    child_pml4[511] = parent_pml4[511];

    if (!fork_duplicate_user_pages(parent_pml4, child_pml4)) {
        serial_write_string("[process] fork FAILED: out of memory duplicating page tables\n");
        free_address_space(child_pml4_phys);
        child->state = TASK_UNUSED;
        return 0;
    }

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    if (!kstack_phys) {
        serial_write_string("[process] fork FAILED: out of memory for kernel stack\n");
        free_address_space(child_pml4_phys);
        child->state = TASK_UNUSED;
        return 0;
    }
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = frame->user_rsp;
    *(--sp) = frame->r11;   // user RFLAGS
    *(--sp) = frame->rcx;   // user RIP
    *(--sp) = (uint64_t)fork_trampoline; // context_switch's `ret` lands here
    *(--sp) = frame->rbp;
    *(--sp) = frame->rbx;
    *(--sp) = frame->r12;
    *(--sp) = frame->r13;
    *(--sp) = frame->r14;
    *(--sp) = frame->r15;

    child->pid = next_pid++;
    child->state = TASK_READY;
    child->saved_rsp = (uint64_t)sp;
    child->kernel_stack_top = kstack_top;
    child->kernel_stack_phys = kstack_phys;
    child->pml4_phys = child_pml4_phys;
    child->parent_pid = parent->pid;
    child->exit_code = 0;
    child->waiting_for_pid = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        child->files[i] = parent->files[i]; // copied by value -- see docs/stdlib.md
    }
    for (int i = 0; i < FPU_STATE_SIZE; i++) {
        child->fpu_state[i] = parent->fpu_state[i];
    }
    child->next = 0;

    enqueue_ready(child);
    return child;
}
```

The six `frame->rbp`/`rbx`/`r12`/`r13`/`r14`/`r15` writes above are consumed by `context_switch`'s own `pop r15/r14/r13/r12/rbx/rbp` epilogue (exactly like `task_create_kernel_thread`/`spawn`'s fake stacks) -- **not** by `fork_trampoline` itself, which only pops the three slots written above those (`user_rsp`, `r11`, `rcx`) per Step 1. Get this write order wrong (or add matching pops in `fork_trampoline`) and the child's register/stack state will be corrupted in a way that likely doesn't crash immediately but produces wrong values later -- double-check the memory layout (lowest address first, matching pop order): `r15, r14, r13, r12, rbx, rbp, fork_trampoline, rcx, r11, user_rsp`, which is exactly the reverse of the `*(--sp) =` write order above.

Add the page-table-duplication helper as a `static` function above `fork_task`:

```c
// Walks every present user-mode page in `parent_pml4`, clears its
// PAGE_WRITABLE bit (marking it copy-on-write), shares the frame via
// pmm_frame_share(), and maps the same frame at the same virtual
// address into `child_pml4`, also read-only. Returns 0 and leaves
// child_pml4 in a to-be-discarded state on out-of-memory (caller frees
// it via free_address_space); parent_pml4's PTEs already flipped
// read-only before the failure stay that way -- harmless, since the
// next write to any of them just takes the (correctly handled,
// refcount-1, no-copy-needed) COW fault path.
static int fork_duplicate_user_pages(uint64_t *parent_pml4, uint64_t *child_pml4) {
    for (unsigned i4 = 0; i4 < 512; i4++) {
        if (i4 == 0 || i4 == 256 || i4 == 511) {
            continue; // shared kernel entries, already copied by the caller
        }
        if (!(parent_pml4[i4] & PAGE_PRESENT)) {
            continue;
        }
        uint64_t *parent_pdpt = (uint64_t *)phys_to_virt(parent_pml4[i4] & PAGE_ADDR_MASK);

        for (unsigned i3 = 0; i3 < 512; i3++) {
            if (!(parent_pdpt[i3] & PAGE_PRESENT)) {
                continue;
            }
            uint64_t *parent_pd = (uint64_t *)phys_to_virt(parent_pdpt[i3] & PAGE_ADDR_MASK);

            for (unsigned i2 = 0; i2 < 512; i2++) {
                if (!(parent_pd[i2] & PAGE_PRESENT)) {
                    continue;
                }
                uint64_t *parent_pt = (uint64_t *)phys_to_virt(parent_pd[i2] & PAGE_ADDR_MASK);

                for (unsigned i1 = 0; i1 < 512; i1++) {
                    if (!(parent_pt[i1] & PAGE_PRESENT)) {
                        continue;
                    }
                    uint64_t virt = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                     ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);

                    parent_pt[i1] &= ~PAGE_WRITABLE;
                    // The parent's TLB may still cache a stale writable
                    // translation for this page from before the PTE
                    // change -- without this invlpg, a write from the
                    // parent right after fork() could silently succeed
                    // via the stale entry instead of taking the COW
                    // fault, corrupting the frame the child now shares.
                    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");

                    uint64_t phys = parent_pt[i1] & PAGE_ADDR_MASK;
                    pmm_frame_share(phys);

                    // Low 12 bits (permission/type flags) plus bit 63
                    // (PAGE_NO_EXECUTE) -- NOT just `& 0xFFF`, which
                    // would silently drop NX and make a non-executable
                    // page executable in the child.
                    uint64_t flags = parent_pt[i1] & (0xFFFULL | PAGE_NO_EXECUTE) & ~PAGE_PRESENT;
                    if (paging_map_into(child_pml4, virt, phys, flags) != 0) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}
```

`PAGE_ADDR_MASK` is currently private to `paging.c` -- add it to `kernel/mm/paging.h` (move the `#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ULL` from `paging.c` into `paging.h`, alongside the other `PAGE_*` defines; remove the now-duplicate definition from `paging.c`).

- [x] **Step 3: Add `SYS_FORK` to the dispatch table in `kernel/syscall.c`**

```c
#define SYS_LSEEK  11
#define SYS_FORK   12
```

Add a new case in `syscall_dispatch`'s `switch`:

```c
        case SYS_FORK: {
            struct task *child = fork_task(frame);
            return child ? child->pid : -1;
        }
```

- [x] **Step 4: Add the stdlib wrapper**

In `lib/syscall.c`:

```c
#define SYS_LSEEK  11
#define SYS_FORK   12
```

```c
int fork(void) {
    return (int)syscall0(SYS_FORK);
}
```

In `lib/include/unistd.h`, add after `int wait(int pid);`:

```c
// Duplicates the calling process. Returns 0 in the child, the
// child's PID in the parent, or -1 on failure (parent unaffected).
// Each side's open file descriptors are independent copies after
// this call -- reads/writes/lseeks on inherited fds no longer share
// a position between parent and child (unlike POSIX, which shares one
// underlying open-file description). NeoOS-specific simplification.
int fork(void);
```

- [x] **Step 5: Add the stdlib doc entry**

In `docs/stdlib.md`, add to the `<unistd.h>` section (after the existing `wait` entry):

```markdown
- `int fork(void)` — duplicates the calling process. Returns `0` in
  the child, the child's PID in the parent, or `-1` on failure (parent
  unaffected). Each side's open file descriptors are independent
  copies after this call -- reads/writes/`lseek`s on inherited fds no
  longer share a position between parent and child (unlike POSIX,
  which shares one underlying open-file description).
  NeoOS-specific simplification.
```

- [x] **Step 6: Write `userland/fork_test.c`**

```c
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    volatile int shared_until_written = 100;

    int pid = fork();
    if (pid < 0) {
        printf("[fork_test] fork FAILED\n");
        return 1;
    }

    if (pid == 0) {
        shared_until_written = 200;
        printf("[fork_test child pid=%d] wrote 200, read back %d\n", getpid(), shared_until_written);
        if (shared_until_written != 200) {
            printf("[fork_test child pid=%d] FAILED: readback mismatch\n", getpid());
            return 1;
        }
        printf("[fork_test child pid=%d] passed\n", getpid());
        return 0;
    }

    shared_until_written = 300;
    printf("[fork_test parent pid=%d, child=%d] wrote 300, read back %d\n", getpid(), pid, shared_until_written);
    if (shared_until_written != 300) {
        printf("[fork_test parent pid=%d] FAILED: readback mismatch\n", getpid());
        return 1;
    }
    int status = wait(pid);
    printf("[fork_test parent pid=%d] child exited code=%d, passed\n", getpid(), status);
    return 0;
}
```

- [x] **Step 7: Add `fork_test.c`'s Makefile build rule and disk-image entry**

In the `Makefile`, mirroring `SSE_TEST.ELF`'s rule:

```makefile
$(USERLAND_BUILD)/FORK_TEST.ELF: $(USERLAND_DIR)/fork_test.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/fork_test.c -L$(LIB_BUILD) -lneoos
```

Add `$(USERLAND_BUILD)/FORK_TEST.ELF` to the `$(DISK_IMG)` target's prerequisite list, and add after the existing `mcopy ... SSE_TEST.ELF` line:

```makefile
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/FORK_TEST.ELF ::BIN/FORK_TEST.ELF
```

Also add `kernel/fork_trampoline.asm` to the kernel's object build. Find the existing NASM build rule pattern for `.asm` files in the `Makefile` (the same pattern that already builds `context_switch.o`/`syscall_entry.o`) and confirm `fork_trampoline.o` is picked up automatically (it should be, if the kernel's object list is generated via a wildcard over `kernel/*.asm`; if the `Makefile` instead lists kernel asm objects explicitly, add `fork_trampoline.o` to that list next to `context_switch.o`).

- [x] **Step 8: Temporarily spawn `FORK_TEST.ELF` to verify**

In `kernel/kernel.c`, replace the four `spawn(...)` calls with:

```c
    spawn("/BIN/FORK_TEST.ELF");
```

- [x] **Step 9: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with `-cpu Nehalem` as in Task 1.
Expected serial output, in order: the child's `wrote 200, read back 200` and `passed` lines, then (after the child exits and the parent's `wait` returns) the parent's `wrote 300, read back 300` and `child exited code=0, passed` lines. Both sides must show their own distinct value (200 vs. 300) with no corruption -- this is the proof the COW fault handler (Task 4) actually copies on write rather than sharing or corrupting. Zero `FAILED`, zero exceptions.

- [x] **Step 10: Revert the temporary spawn change and verify no regression**

Restore `kernel/kernel.c`'s original four-process boot. Confirm `git diff --stat kernel/kernel.c` prints nothing. Rebuild and boot again with `-cpu Nehalem`. Expected: milestone 5-8's exact full lifecycle reproduces, zero `FAILED`, zero exceptions.

- [x] **Step 11: Verify the fork-failure path (task-slot exhaustion)**

Temporarily change `userland/fork_test.c`'s `main` to call `fork()` in a loop up to `MAX_TASKS` times without ever calling `wait()`, printing each return value, to confirm that once the task table fills up, `fork()` returns `-1` cleanly (not a crash) and the calling process keeps running normally afterward (e.g. prints one more line after the failed call). Spawn `FORK_TEST.ELF` alone in `kernel/kernel.c` as in Step 8, build, boot, and confirm: some number of successful forks (each printing a PID `>0`), then `-1` once the table is full, then the process's own trailing print still executes. Revert `fork_test.c` back to the Step 6 version afterward (`git diff --stat userland/fork_test.c` prints nothing) and revert `kernel/kernel.c` again (Step 10).

**Correction, found while executing:** the plan named the test binary
`FORK_TEST.ELF`, which is 9 characters. `mcopy` mangles that to the 8.3
short name `FORK_T~1.ELF` plus a VFAT long-name entry this FAT16 driver
doesn't read, so `spawn("/BIN/FORK_TEST.ELF")` fails with "file not
found". Every other userland binary here is <=8 characters. Renamed to
`FORKTEST.ELF` throughout.

**Second correction, a pre-existing scheduler bug this task exposed:**
Step 11's task-table-exhaustion test double-faulted. Root cause was not
in fork() at all -- `schedule()` ran with interrupts enabled while in an
inconsistent state (`current` already updated to the incoming task,
execution still on the outgoing task's stack), so a timer interrupt
re-entering it made `context_switch` write the wrong task's RSP into
`saved_rsp`. Fixed in its own commit ("Make schedule() non-reentrant
..."); see that message for the full trace. Nothing before fork()
reached the window, because it takes several tasks doing nothing but
`yield()` to keep `schedule()` executing that large a fraction of the
time.

- [x] **Step 12: Commit**

```bash
git add kernel/fork_trampoline.asm kernel/process.c kernel/process.h kernel/mm/paging.h kernel/mm/paging.c \
        kernel/syscall.c lib/syscall.c lib/include/unistd.h docs/stdlib.md Makefile userland/fork_test.c
git commit -m "Add fork() via copy-on-write address-space duplication"
```

---

### Task 6: `exec()` — Address-Space Replacement and Syscall

**Files:**
- Create: `userland/exec_target.c`
- Modify: `kernel/process.c`, `kernel/process.h`, `kernel/syscall.c`, `lib/syscall.c`, `lib/include/unistd.h`, `docs/stdlib.md`, `Makefile`, `userland/fork_test.c`, `kernel/kernel.c` (temporary, reverted)

**Interfaces:**
- Produces: `int exec_task(const char *path, struct syscall_frame *frame)` (`process.c`); `SYS_EXEC` syscall number 13; `int exec(const char *path)` (stdlib).
- Consumes: `struct syscall_frame` (Task 2), `free_address_space` (Task 3), a `build_user_address_space` helper factored out of `spawn()` in this task.

- [x] **Step 1: Factor `build_user_address_space` out of `spawn()` in `kernel/process.c`**

`spawn()` currently does, inline:

```c
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
        zero_frames(frame, 0);
        uint64_t vaddr = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_map_into(pml4, vaddr, frame, PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_USER);
    }
```

Extract this into a new `static` helper, placed above `spawn()`:

```c
// Builds a complete, freshly-loaded user address space from the ELF
// image at `path`: a new PML4 with the shared kernel entries, the
// loaded ELF segments, and a fresh user stack. On success, returns 1
// with *out_pml4_phys/*out_entry set; the caller (spawn() for a new
// task, exec_task() for an existing one) is responsible for wiring
// the result into a struct task. On failure, returns 0 having freed
// any partial address space it built -- the caller's own state (if
// any) is untouched.
static int build_user_address_space(const char *path, uint64_t *out_pml4_phys, uint64_t *out_entry) {
    uint16_t cluster;
    uint32_t size;
    if (!fat16_find(path, &cluster, &size, NULL, NULL)) {
        serial_write_string("[process] FAILED: file not found: ");
        serial_write_string(path);
        serial_write_string("\n");
        return 0;
    }

    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) {
        serial_write_string("[process] FAILED: kmalloc failed for ELF image\n");
        return 0;
    }
    fat16_read_file(cluster, size, image);

    uint64_t pml4_phys = paging_alloc_pml4();
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    pml4[0] = p4_table[0];
    pml4[256] = p4_table[256];
    pml4[511] = p4_table[511];

    uint64_t entry;
    if (!elf_load(image, size, pml4, &entry)) {
        kfree(image);
        free_address_space(pml4_phys);
        return 0;
    }
    kfree(image);

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc(0);
        if (!frame) {
            free_address_space(pml4_phys);
            return 0;
        }
        zero_frames(frame, 0);
        uint64_t vaddr = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_map_into(pml4, vaddr, frame, PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_USER);
    }

    *out_pml4_phys = pml4_phys;
    *out_entry = entry;
    return 1;
}
```

Update `spawn()` to call it:

```c
struct task *spawn(const char *path) {
    uint64_t pml4_phys, entry;
    if (!build_user_address_space(path, &pml4_phys, &entry)) {
        return 0;
    }

    struct task *t = alloc_task_slot();
    if (!t) {
        serial_write_string("[process] spawn FAILED: no free task slot\n");
        free_address_space(pml4_phys);
        return 0;
    }

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
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
    t->kernel_stack_phys = kstack_phys;
    t->pml4_phys = pml4_phys;
    t->parent_pid = current ? current->pid : 0;
    t->exit_code = 0;
    t->waiting_for_pid = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        t->files[i].in_use = 0;
    }
    cpu_default_fpu_state(t->fpu_state);
    t->next = 0;

    enqueue_ready(t);
    return t;
}
```

- [x] **Step 2: Write `exec_task` in `kernel/process.c`**

Add after `spawn`:

```c
// Replaces the calling task's address space in place with the ELF
// image at `path`. Open files, pid, and parent_pid are preserved
// (POSIX default: exec() does not close file descriptors). Returns 1
// on success (the syscall path never actually returns to the old
// program -- frame's saved RIP/RSP are overwritten so the ordinary
// sysret lands in the new one instead), or 0 on failure, leaving the
// calling task completely unchanged and still runnable -- the new
// address space is built and validated to completion before the old
// one is freed, so a bad path or OOM never destroys the caller.
int exec_task(const char *path, struct syscall_frame *frame) {
    uint64_t new_pml4_phys, new_entry;
    if (!build_user_address_space(path, &new_pml4_phys, &new_entry)) {
        return 0;
    }

    struct task *t = current;
    free_address_space(t->pml4_phys);
    t->pml4_phys = new_pml4_phys;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(new_pml4_phys) : "memory");

    cpu_default_fpu_state(t->fpu_state);

    frame->rcx = new_entry;       // user RIP the ordinary sysret epilogue will return to
    frame->user_rsp = USER_STACK_TOP;

    return 1;
}
```

- [x] **Step 3: Add `SYS_EXEC` to the dispatch table in `kernel/syscall.c`**

```c
#define SYS_FORK   12
#define SYS_EXEC   13
```

```c
        case SYS_EXEC: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            return exec_task(path_buf, frame) ? 0 : -1;
        }
```

- [x] **Step 4: Add the stdlib wrapper**

In `lib/syscall.c`:

```c
#define SYS_FORK   12
#define SYS_EXEC   13
```

```c
int exec(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_EXEC, (int64_t)(uint64_t)path, (int64_t)len);
}
```

In `lib/include/unistd.h`, add after `int fork(void);`:

```c
// Replaces the calling process's address space with the ELF
// executable at `path` (NUL-terminated). Open file descriptors, pid,
// and parent are preserved. On success, never returns -- execution
// continues at the new program's entry point. Returns -1 on failure
// (bad path, out of memory), leaving the calling process completely
// unchanged and still running its original code.
int exec(const char *path);
```

- [x] **Step 5: Add the stdlib doc entry**

In `docs/stdlib.md`, add to the `<unistd.h>` section (after the new `fork` entry):

```markdown
- `int exec(const char *path)` — replaces the calling process's
  address space with the ELF executable at `path`. Open file
  descriptors, PID, and parent are preserved. On success, never
  returns. Returns `-1` on failure (bad path, out of memory), leaving
  the calling process completely unchanged and still running its
  original code.
```

- [x] **Step 6: Write `userland/exec_target.c`**

```c
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("[exec_target pid=%d] running, exec succeeded\n", getpid());
    return 0;
}
```

- [x] **Step 7: Wire `fork_test.c`'s child to `exec()` into it**

In `userland/fork_test.c`, change the child branch (after the existing `printf("[fork_test child pid=%d] passed\n", getpid());` line) to:

```c
        printf("[fork_test child pid=%d] passed\n", getpid());

        int exec_result = exec("/BIN/EXEC_TARGET.ELF");
        printf("[fork_test child pid=%d] exec FAILED, result=%d\n", getpid(), exec_result);
        return 1; // only reached if exec() failed
```

- [x] **Step 8: Add `exec_target.c`'s Makefile build rule and disk-image entry**

```makefile
$(USERLAND_BUILD)/EXEC_TARGET.ELF: $(USERLAND_DIR)/exec_target.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/exec_target.c -L$(LIB_BUILD) -lneoos
```

Add `$(USERLAND_BUILD)/EXEC_TARGET.ELF` to the `$(DISK_IMG)` target's prerequisites, and after the `mcopy ... FORK_TEST.ELF` line:

```makefile
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/EXEC_TARGET.ELF ::BIN/EXEC_TARGET.ELF
```

- [x] **Step 9: Temporarily spawn `FORK_TEST.ELF` to verify**

In `kernel/kernel.c`, replace the four `spawn(...)` calls with `spawn("/BIN/FORK_TEST.ELF");` (same as Task 5 Step 8).

- [x] **Step 10: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with `-cpu Nehalem` as in Task 1.
Expected serial output: the child's `wrote 200, read back 200` and `passed` lines, immediately followed by `[exec_target pid=N] running, exec succeeded` (same pid as the forked child, since `exec()` preserves it) -- the forked child's own identity (and the code that would have printed an "exec FAILED" line) is fully replaced, never resumes. Then, after the exec'd process exits, the parent's `wrote 300, read back 300` and `child exited code=0, passed` lines (the exit code comes from `exec_target.c`'s `return 0`). Zero `FAILED`, zero exceptions.

- [x] **Step 11: Revert the temporary spawn change and verify no regression**

Restore `kernel/kernel.c`'s original four-process boot. Confirm `git diff --stat kernel/kernel.c` prints nothing. Rebuild and boot again. Expected: milestone 5-8's exact full lifecycle reproduces, zero `FAILED`, zero exceptions.

- [x] **Step 12: Verify the exec-failure path**

Temporarily change `userland/fork_test.c`'s child branch to call `exec("/BIN/NOPE.ELF")` (a nonexistent path) instead of `/BIN/EXEC_TARGET.ELF`, and print an extra confirmation line after the failed call to prove the process is still alive and running its original code:

```c
        int exec_result = exec("/BIN/NOPE.ELF");
        printf("[fork_test child pid=%d] exec correctly failed, result=%d, still alive\n", getpid(), exec_result);
```

Spawn `FORK_TEST.ELF` alone (Step 9), build, boot. Expected: `exec correctly failed, result=-1, still alive` prints, proving a failed `exec()` leaves the process running rather than crashing or corrupting it. Revert `fork_test.c` back to the Step 7 version (`git diff --stat userland/fork_test.c` prints nothing) and revert `kernel/kernel.c` again (Step 11).

**Corrections, found while executing:**
1. `EXEC_TARGET.ELF` is 11 characters -- same 8.3 short-name mangling
   that hit `FORK_TEST.ELF` in Task 5. Renamed to `EXECTARG.ELF`.
2. The `exec_task` snippet above called `free_address_space(t->pml4_phys)`
   *before* loading the new CR3, which is the same fatal ordering bug
   Task 3 fixed in `task_exit()`: freeing the live PML4 lets pmm write
   free-list links over `pml4[0]`. Corrected to switch CR3 first, then
   free the old space (reachable afterward via the physmap, which the
   new address space shares).

- [x] **Step 13: Commit**

```bash
git add kernel/process.c kernel/process.h kernel/syscall.c lib/syscall.c lib/include/unistd.h \
        docs/stdlib.md Makefile userland/exec_target.c userland/fork_test.c
git commit -m "Add exec() via address-space replacement"
```

---

### Task 7: Full Regression and Memory-Leak Verification

**Files:**
- Modify: `kernel/kernel.c` (temporary, reverted)

**Interfaces:** None new — this task only exercises everything built in Tasks 1-6 together.

- [ ] **Step 1: Temporarily verify no leak across repeated fork()+exec()+exit cycles**

As in Task 3's leak check, `wait_for_pid()` needs a valid `current` task, so this runs as a kernel thread, not inline in `kmain`. In `kernel/kernel.c`, add above `kmain` (or reuse/rename Task 3's now-removed `leak_test_thread` if it's still present in the file at this point -- it isn't, since Task 3's revert step removed it along with the rest of that temporary change):

```c
static void fork_leak_test_thread(void) {
    serial_write_string("[test] free_frames before fork loop=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string("\n");

    for (int i = 0; i < 5; i++) {
        struct task *t = spawn("/BIN/FORK_TEST.ELF");
        if (!t) {
            serial_write_string("[test] spawn FAILED\n");
        } else {
            wait_for_pid(t->pid);
        }
    }

    serial_write_string("[test] free_frames after fork loop=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string("\n");

    task_exit(0);
}
```

Replace the four `spawn(...)` calls with:

```c
    task_create_kernel_thread(fork_leak_test_thread);
```

(Note: `FORK_TEST.ELF`'s own top-level task exits only after its internal `wait()` on the COW-forked-then-exec'd child completes, so `wait_for_pid(t->pid)` here waits on the whole chain.)

- [ ] **Step 2: Build and verify**

Run: `make clean && make disk-image && make iso`, boot with `-cpu Nehalem` as in Task 1.
Expected: `[test] free_frames before fork loop=X` and `[test] free_frames after fork loop=X` print the exact same value -- proving that across 5 full fork()+COW-write+exec()+exit+reap cycles, every shared and unshared frame is correctly reclaimed (no leak from refcounting, page-table teardown, or the fork/exec address-space-replacement paths). All 5 iterations' `fork_test`/`exec_target` output appears correctly interleaved with no `FAILED`. Zero exceptions.

- [ ] **Step 3: Revert the temporary loop and verify final regression**

Restore `kernel/kernel.c`'s original four-process boot exactly. Confirm `git diff --stat kernel/kernel.c` prints nothing.

Rebuild and boot with `-cpu Nehalem`. Expected: milestone 5-8's exact full lifecycle reproduces (bursty looper interleave, dense yielder interleave, `[parent] child exit code=42`), zero `FAILED`, zero exceptions.

- [ ] **Step 4: Commit**

Only if Step 1's temporary loop required any non-`kernel.c` fixes discovered during this pass (unlikely, but if so, commit them here with a message describing what the leak-loop test caught). If `kernel.c` is the only file touched and it's now fully reverted, there is nothing to commit for this task -- it exists purely as a verification gate.
