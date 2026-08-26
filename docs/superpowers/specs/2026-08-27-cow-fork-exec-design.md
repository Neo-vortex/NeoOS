# NeoOS — Milestone 9: Copy-on-Write Paged VM (fork/exec)

## Goal

Give NeoOS real `fork()`/`exec()` process creation, backed by
copy-on-write (COW) physical-frame sharing instead of eagerly copying
an entire address space. Today `spawn()` is the only way to create a
process — always a fresh address space loaded directly from an ELF
image — and the processes milestone spec explicitly deferred
"`fork()`/`exec()` and copy-on-write" as a pair pending this
milestone. Order agreed with the user: COW → VFS → SMP (the SSE
milestone's spec recorded an earlier VFS-before-COW ordering; this
supersedes it).

## Success criteria

- A new `fork_test.c` calls `fork()`; the child (return value `0`) and
  parent (return value = child's PID) each print a distinguishing
  message and each write a different value into a shared-until-written
  local variable, then re-read and print it back — proving the write
  actually triggered a private copy rather than corrupting or sharing
  state between parent and child.
- After the divergence check above, `fork_test.c`'s child `exec()`s
  into a separate `exec_target.c` program; its distinct output
  replaces the forked copy's, proving the address space was fully
  replaced (not just partially overwritten) and that the parent is
  unaffected.
- Boot log shows zero `FAILED` lines and zero exceptions across every
  verification run.
- All milestone 5-8 test programs (`spin`, `child`, `parent`, `looper`,
  `yielder`, `faulter`, `fileio`, `sse_test`) continue to reproduce
  their exact prior behavior — this milestone changes process creation
  and memory teardown, not the existing syscalls' semantics.
- Repeated `fork()`+exit cycles do not leak physical memory: free frame
  count returns to its pre-loop baseline after all forked children
  have exited and been reaped.

## Out of scope (future work)

- **VFS, SMP.** Later milestones in the agreed sequence, each
  brainstormed separately.
- **Shared file offsets across `fork()`.** POSIX shares one open-file
  description (and its seek offset) between parent and child after
  `fork()`. NeoOS's `struct file_descriptor` embeds `position` directly
  per-task with no separate open-file-description object; `fork()`
  copies the fd table by value, so each side's `lseek`/`read`/`write`
  position diverges independently after the fork. Documented as a
  NeoOS-specific simplification, same as `wait()`'s single-PID-only
  semantics.
- **Read-only ELF segments (`.text`/`.rodata`).** `elf_load()` still
  maps every segment writable (milestone 5's simplification, unchanged
  here). This actually simplifies the COW fault handler: the *only*
  way a user PTE ever ends up marked read-only is via `fork()`, so any
  write fault on a present, user-mode page can be assumed to be a COW
  fault with no separate "is this actually read-only for a real
  reason" check. Making segments genuinely read-only is future work
  that would need to revisit this assumption.
- **Reclaiming a never-`wait()`-ed zombie's task slot or kernel
  stack.** This milestone frees a zombie's *address space*
  unconditionally at exit (the bulk of its memory), but the task slot
  and kernel stack still leak until reaped, exactly as they leak today
  — unrelated pre-existing gap, not this milestone's problem to fix.
- **Syscall argument/pointer validation.** Still not implemented,
  tracked security gap noted since the processes milestone. Unaffected
  by this milestone.
- **Multi-core-safe refcounting.** The new frame refcount increments
  and decrements are plain, unlocked reads-modify-writes — correct
  under NeoOS's current single-execution-context model, and one of the
  things the future SMP milestone will need to revisit across the
  whole kernel, not specific to this code.

## Architecture

### Frame reference counting (`kernel/mm/pmm.c`)

`pmm.c` already keeps a flat, frame-indexed bookkeeping array capped
at `PMM_MAX_FRAMES` (`frame_order[]`, 4GiB worth of frames). This
milestone adds a same-shaped `static uint16_t
frame_refcount[PMM_MAX_FRAMES]`:

- `pmm_alloc()` sets a freshly allocated block's frames to refcount 1
  (single owner — unchanged behavior for every existing caller).
- New: `void pmm_frame_share(uint64_t phys)` increments a single
  frame's refcount by 1. The only new entry point; called exclusively
  by `fork()`'s address-space duplication, once per shared page.
- `pmm_free(uint64_t phys_addr, unsigned order)` changes from
  "unconditionally return the block to the free list" to "decrement
  each covered frame's refcount, and only return the block to the free
  list once every frame in it reaches 0." Every existing caller
  (kernel stacks, page-table frames, ELF segment frames, user stack
  frames) allocates at refcount 1 and calls `pmm_free()` exactly once,
  so their behavior is bit-for-bit unchanged — only COW-shared order-0
  user pages ever have a refcount above 1, and sharing only ever
  happens at order 0.
- New: `unsigned pmm_frame_refcount(uint64_t phys)` — read-only lookup
  used by the COW fault handler to decide "copy" vs. "just re-enable
  write."

### `fork()`: address-space duplication (`kernel/process.c`)

A new `struct task *fork_task(struct syscall_frame *frame)` (called
from `syscall_dispatch`'s new `SYS_FORK` case), where `frame` is the
pointer described below:

1. Allocate a new task slot and a fresh kernel stack, same as `spawn()`.
2. Allocate a new PML4; copy in the same three shared kernel entries
   `spawn()` already copies (`pml4[0]`, `pml4[256]`, `pml4[511]`).
3. Walk the parent's page tables. For every present user-mode page:
   clear `PAGE_WRITABLE` on the *parent's* own PTE if it was set, call
   `pmm_frame_share()` on that physical frame, and map the same frame
   into the child's new PML4 at the same virtual address, also
   read-only. (Every user page is writable today per `elf_load()`, so
   this applies uniformly across code, data, and stack.)
4. `memcpy` `fpu_state` from parent to child — the child continues
   from the exact same point, mid-instruction-stream, with identical
   FPU/SSE register contents.
5. Copy `files[]` by value (see Out of Scope: offsets diverge after
   this point).
6. Build the child's kernel stack per the fork-trampoline plan below.
7. `parent_pid` = parent's pid; `pid` = freshly allocated; enqueue
   child as `TASK_READY`.
8. Return the child's task pointer to the caller (`syscall_dispatch`
   returns its pid to the parent as the normal syscall return value).

On any failure partway through (no free task slot, `pmm_alloc`
exhausted while duplicating page tables) unwind: `pmm_free()` back any
shares already taken on parent frames, free any child-side frames/page
tables allocated so far, restore any parent PTEs already flipped
read-only back to writable, and return `0`/`-1` — the parent must be
left exactly as it was before the failed `fork()`.

### Fork trampoline: resuming the child (new `kernel/fork_trampoline.asm` code)

`fork()` is entered via `SYSCALL`, so the child needs to "return" from
that same syscall — in a different task, on a different kernel stack —
with `RAX = 0`, while the parent's own return gives it the child's
PID. Rather than touching `syscall_entry.asm`'s already-correct,
carefully-sequenced epilogue, this mirrors the existing
`kernel_thread_trampoline` pattern `spawn()` already uses to bootstrap
a brand-new task into ring 3:

`fork_task()` writes the parent's saved user RIP/RSP/RFLAGS and
callee-saved registers (RBX/RBP/R12-R15 — the ones a compiler may
have kept live across the `syscall` instruction per SysV's calling
convention) onto the child's fresh kernel stack, below a pointer to a
new `fork_trampoline` label. When the scheduler first switches to the
child, `context_switch`'s trailing `ret` lands in `fork_trampoline`,
which pops those saved values, zeroes `RAX`, and manually builds an
`iretq` frame into ring 3 (`push SS, RSP, RFLAGS, CS, RIP; iretq`) —
the same technique `kernel_thread_trampoline` already uses, just
resuming mid-program instead of at a fresh entry point. No changes to
`syscall_entry.asm`.

### Exposing the saved syscall frame (`kernel/syscall_entry.asm`, `kernel/syscall.c`)

Both `fork()` (to copy it) and `exec()` (to overwrite it) need access
to the current task's saved user-mode context — the block
`syscall_entry.asm` already pushes onto the kernel stack at every
syscall entry (`user_rsp`, `rcx`/user RIP, `r11`/user RFLAGS, and the
saved GPRs). `syscall_dispatch` gains a 6th argument — a pointer to
the base of that saved frame — passed via `r9`, the one argument
register the current five-argument convention (`num, a1-a4`) leaves
free. A new `struct syscall_frame` (in `process.h`) gives C code a
named view onto it, with fields ordered lowest-address-first to match
memory layout exactly (`r9, r8, r10, rdx, rsi, rdi, r15, r14, r13,
r12, rbp, rbx, r11, rcx, user_rsp` — the reverse of push order, since
the last register pushed ends up at the lowest address). This is the
only change to `syscall_entry.asm`: setting `r9 = rsp` (which already
points at the base of this block) right after the existing
register-reorder shuffle and before `call syscall_dispatch`; the
epilogue is untouched.

### COW page-fault handler (`kernel/mm/paging.c`, hooked from `kernel/isr.c`)

New `int paging_handle_cow_fault(uint64_t pml4_phys, uint64_t
fault_addr)`. `isr.c`'s existing vector-14 handling gains a check
before falling into `exception_dump_and_halt`: if the fault's error
code has `present=1, write=1, user=1` set, call this function first.

Per the Out of Scope note above, since `fork()` is the only source of
a read-only user PTE in this milestone, any such fault is assumed to
be COW, not a real permission violation:
- Walk to the faulting PTE. If it's not present or already writable,
  return 0 (not a COW fault — let the existing fatal path run; this
  covers genuine bugs, wild pointers, and not-present accesses
  unchanged).
- Look up the frame's refcount via `pmm_frame_refcount()`. If `1`
  (this task is the last owner — the other side already wrote its own
  copy and dropped its share, or there never was another sharer): flip
  `PAGE_WRITABLE` back on in place, no copy needed.
- If `>1`: allocate a new frame, copy the 4KiB of content across
  (`phys_to_virt` on both sides), remap the PTE to the new frame with
  `PAGE_WRITABLE` set, and `pmm_free()` the old frame (dropping this
  task's share).
- Either way, `invlpg` the single faulting page, and return 1
  (handled).

### `exec()`: address-space replacement (`kernel/process.c`)

`spawn()`'s "allocate PML4 → copy shared kernel entries → `elf_load()`
→ map a fresh user stack" sequence is factored out into a shared
`build_user_address_space(const char *path, uint64_t *out_pml4_phys,
uint64_t *out_entry)`, reused by both `spawn()` (for a new task) and
the new `int exec_task(const char *path, struct syscall_frame *frame)`
(for the calling task in place):

1. Call `build_user_address_space()` for the *new* program fully
   first, exactly as `spawn()` would — if this fails (bad ELF, file
   not found, out of memory), return failure immediately with the
   calling task completely untouched. This ordering is required for
   correct `exec()` failure semantics (see Error handling).
2. Only once that fully succeeds: free the calling task's *old*
   address space via `free_address_space()` (below) — safe to do here,
   since kernel code executes via the shared higher-half mapping, not
   through the process's own user PTEs.
3. Install the new PML4 as `current_task()->pml4_phys`; load it into
   `CR3` immediately.
4. Reset `fpu_state` to the default template (a fresh program shouldn't
   inherit stale FPU/SSE register contents from whatever the old
   program left behind).
5. Overwrite `frame->user_rip` and `frame->user_rsp` (the syscall
   frame from the section above) with the new entry point and new user
   stack top. `syscall_entry.asm`'s ordinary, unmodified epilogue then
   `sysret`s into the new program instead of the old one.
6. Open files (`files[]`) are left untouched — `exec()` preserves file
   descriptors, matching POSIX default behavior (no `O_CLOEXEC`
   concept exists to make otherwise).

### Process teardown (`kernel/mm/paging.c`, `kernel/process.c`)

New `void free_address_space(uint64_t pml4_phys)`: walks the page
tables rooted at `pml4_phys`, calls `pmm_free()` on every mapped
user-space frame (dropping its refcount — a COW-shared frame only
actually returns to the allocator once every sharer has released it),
frees the page-table frames themselves (PT/PD/PDPT levels), then frees
the PML4 frame. The three shared kernel entries are never walked into
or freed — they point at kernel-owned tables no process owns.

Split across two points for safety:
- **`task_exit()`**: calls `free_address_space()` immediately. Safe —
  the exiting task's kernel-mode code runs via the shared higher-half
  mapping, never through its own now-freed user PTEs.
- **Kernel stack**: *not* freed in `task_exit()` — it's still running
  on it. Freed instead in `wait_for_pid()`, at reap time, since the
  reaper is by definition a different running task at that point.
- The task slot's existing `TASK_ZOMBIE`-until-reaped lifecycle is
  unchanged.

## File structure

```
kernel/
  mm/pmm.c/.h          # MODIFIED: frame_refcount[], pmm_frame_share(), pmm_frame_refcount(),
                        #           pmm_free() now decrement-then-free-if-zero
  mm/paging.c/.h       # MODIFIED: paging_handle_cow_fault(), free_address_space()
  process.c/.h         # MODIFIED: struct syscall_frame, fork_task(), exec_task(),
                        #           build_user_address_space() factored out of spawn(),
                        #           task_exit()/wait_for_pid() call free_address_space()/free kernel stack
  fork_trampoline.asm  # NEW: fork_trampoline (mirrors context_switch.asm's kernel_thread_trampoline)
  syscall_entry.asm    # MODIFIED: pass saved-frame pointer to syscall_dispatch via r9
  syscall.c            # MODIFIED: SYS_FORK, SYS_EXEC dispatch cases
  isr.c                # MODIFIED: vector-14 handling tries paging_handle_cow_fault() first
lib/
  include/unistd.h     # MODIFIED: int fork(void), int exec(const char *path)
userland/
  fork_test.c          # NEW: fork() + shared-until-written-variable divergence test;
                        #      the child, after that check passes, exec()s into EXEC_TARGET.ELF
  exec_target.c        # NEW: small program with a distinct message, the exec() target above
docs/
  stdlib.md            # MODIFIED: document fork()/exec()
```

## Data flow (kmain, extending milestone 8's sequence)

```
... existing milestones 2-8 sequence (interrupts, memory, storage, processes, stdlib, FAT16 rw, SSE) ...
  -> spawn("/BIN/FORK_TEST.ELF")   (NEW: exercises fork(); its child exec()s /BIN/EXEC_TARGET.ELF)
  -> sti
  -> idle loop
```

Syscall entry (extended): `syscall_entry.asm` now also passes a
pointer to the saved user-context frame into `syscall_dispatch`.
`SYS_FORK` calls `fork_task()`, which enqueues a new ready task whose
first scheduling-in runs through `fork_trampoline` instead of
`kernel_thread_trampoline`/`context_switch`'s normal path. `SYS_EXEC`
calls `exec_task()`, which mutates the *current* task's own saved
frame in place before returning normally.

Page-fault path (extended): `isr.c`'s vector-14 case tries
`paging_handle_cow_fault()` before falling into the existing fatal
dump-and-halt.

## Testing / verification

Same approach as every prior milestone — no host-runnable unit tests;
verification is via headless QEMU and serial log capture:
- **COW correctness:** `fork_test.c`'s parent and child each write a
  different value into what was, until that write, the same physical
  frame, then read it back and print it — divergent, correct values on
  both sides prove the copy-on-write fault actually copied rather than
  sharing or corrupting.
- **`exec()` correctness:** `fork_test.c`'s child's `exec()` into
  `exec_target.c` fully replaces its own forked identity — the program
  that was running before `exec()` never resumes.
- **Memory leak check:** compare `pmm_free_frame_count()` (already
  exposed) before and after a loop of several `fork()`+exit+reap
  cycles — should return to baseline, proving `free_address_space()`
  and the refcount-driven `pmm_free()` actually reclaim shared frames
  once both sides are done with them.
- **Regression:** milestones 5-8's full test suite (`spin`, `child`,
  `parent`, `looper`, `yielder`, `faulter`, `fileio`, `sse_test`)
  reproduce their exact prior behavior.

## Error handling

- `fork()` failure (task table full, out of memory mid-duplication):
  fully unwound as described in Architecture, parent left unaffected,
  returns `-1`.
- `exec()` failure: the new address space is built and validated to
  completion *before* the old one is torn down (Architecture,
  `exec_task()` step 1) — a failed `exec()` leaves the calling process
  completely unchanged and still runnable, matching POSIX semantics
  (`exec()` only fails by returning; it never destroys the caller).
- Kernel-side faults still produce the existing clean register dump
  and halt for anything that isn't a recognized COW fault — no new
  silent-corruption paths.
- As in every prior milestone, syscall argument pointers are not
  validated against the calling process's own memory — the same
  tracked, deferred security gap, unaffected by this milestone.
