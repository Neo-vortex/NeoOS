# Threads Milestone

**Date:** 2026-08-27
**Status:** Approved
**Roadmap position:** Milestone 1 of 15
(see `2026-08-27-roadmap-architecture-design.md`)

## Purpose

Split `struct task` into `struct process` and `struct thread`, add the
locking and wait-queue primitives the rest of the roadmap depends on,
and expose threads to user mode.

Five later milestones sit directly on this one: extended CPU state
(per-thread XSAVE areas), TLS (per-thread FS base), SMP (per-CPU run
queues), scheduling classes (per-thread class and priority), and IPC
(everything blocks on the wait queues introduced here).

## What the current code forces

Read from `kernel/process.c` (601 lines) as it stands:

- `struct task tasks[MAX_TASKS]` is a **static array of 16**, and slot
  reuse is load-bearing: `wait_for_pid` reaps by setting
  `state = TASK_UNUSED` and freeing `kernel_stack_phys`.
- **Three linear scans** over that array — `alloc_task_slot`,
  `task_exit`'s waiter wakeup, and `wait_for_pid`'s reap. All three
  become wrong or slow once threads outnumber processes.
- `task_exit` frees the address space and file descriptors;
  `wait_for_pid` frees the kernel stack. With threads, the first is a
  *process* event and the second a *thread* event, so that split must
  be redrawn.
- `spawn`, `fork_task`, and `task_create_kernel_thread` each open-code
  the same ~20 lines of struct initialization.
- `current` is a file-static global (`process.c:20`) and `tss` is a
  single global read by `syscall_entry.asm:26`. Both are SMP-fatal.

## Decisions

| # | Decision | Chosen |
|---|---|---|
| 1 | Storage | Heap-allocated, refcounted `struct process` |
| 2 | `exit()` with many threads | Kills all threads; waits are interruptible |
| 3 | Per-CPU staging | Introduce `struct cpu` + `swapgs` **now** |
| 4 | Thread user stacks | Kernel-allocated (forced: no `malloc`, no `mmap` yet) |
| 5 | TID namespace | Shared with PIDs, one `next_id++` |

Decision 4 is not a preference: `lib/` has no `malloc` and `mmap` does
not arrive until milestone 3, so userland cannot supply a stack and a
`clone()`-style API is unavailable.

Decision 5 keeps serial logs unambiguous, which matters in a project
debugged entirely through serial output.

## Data structures

```c
struct process {
    int pid, parent_pid;
    uint32_t refcount;              /* == live thread count */
    uint64_t pml4_phys;
    struct file_descriptor files[MAX_OPEN_FILES];
    struct thread *threads;         /* list via thread->proc_next */
    uint16_t stack_slots;           /* bitmap of live thread stacks */
    int exiting, exit_code;
    enum { PROC_ALIVE, PROC_ZOMBIE } state;
    struct waitq exit_waiters;
    struct spinlock lock;
    struct process *next;           /* global list */
};

struct thread {
    int tid;
    struct process *proc;
    enum thread_state state;        /* READY RUNNING BLOCKED ZOMBIE */
    uint64_t saved_rsp, kernel_stack_top, kernel_stack_phys;
    int stack_slot;
    int kill_pending, exit_code;
    struct waitq join_waiters;
    struct waitq *blocked_on;       /* so a kill can find and wake it */
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    struct thread *proc_next, *next;
};
```

`struct file_descriptor` is unchanged.

## Lifetime

Two reclamations, deliberately independent. This is where the
milestone's bugs will live.

**Address space** — freed when `refcount` reaches zero, by whichever
thread exits last. That thread switches `CR3` to `p4_table` *before*
calling `free_address_space`, for the reason documented at
`process.c:519`: the buddy allocator writes free-list links into the
freed block itself, so freeing a live PML4 overwrites `pml4[0]` — the
identity map the allocator dereferences through. The hazard does not
change; it only moves.

**Kernel stacks and structs** — a thread can never free the stack it
is running on.

- `thread_join` frees a joined thread's stack and struct immediately.
- Threads never joined accumulate on the process's zombie list and are
  freed wholesale by `wait_for_pid` at reap, along with the
  `struct process`.

This mirrors how `wait_for_pid` frees `kernel_stack_phys` today: the
existing pattern generalized, not a new one. Consequently a
`struct process` outlives its own address space, surviving as a
`PROC_ZOMBIE` carrying only `exit_code` and the zombie thread list
until a parent reaps it.

## Address-space layout

New threads need user stacks; there is exactly one today at
`USER_STACK_TOP`. Stacks are placed by slot index with a guard page
between them:

```
USER_STACK_TOP  0x0000700000000000
  slot 0  [TOP - 16K, TOP)              main thread
          [TOP - 20K, TOP - 16K)        guard (unmapped)
  slot 1  [TOP - 36K, TOP - 20K)
          ...
  stride = (USER_STACK_PAGES + 1) * 4K = 20K,  16 slots
```

`stack_slots` is a 16-bit bitmap, so allocating a slot is a bit scan
rather than a walk of the thread list.

Side effect worth having: **the main thread gains a guard page**,
which it lacks today — user stack overflow currently writes silently
into whatever is mapped below.

## Semantics

- `fork()` in a multithreaded process duplicates **only the calling
  thread** (POSIX). The child starts single-threaded.
- `thread_create` returns `-EINVAL` once `proc->exiting` is set, so a
  dying process cannot grow threads faster than `exit()` kills them.
- `exit()` terminates every thread of the process (see Interruption).
- `thread_join` is the only reaping mechanism for threads; there is no
  detached-thread concept.

Both changed semantics are documented in `docs/stdlib.md`.

## Per-CPU

While in user mode, `IA32_KERNEL_GS_BASE` holds the per-CPU block and
`GS_BASE` holds userland's value; `swapgs` exchanges them on kernel
entry and again on exit. `struct cpu` carries a self-pointer at offset
0, so `gs:0` yields the block's own address.

```c
struct cpu {
    struct cpu *self;           /* gs:0 */
    struct thread *current;
    struct thread *idle;
    struct tss *tss;
    uint64_t user_rsp_scratch;  /* was a global */
    uint32_t lapic_id;
    /* debug: held lock ranks + depth */
};
```

The C struct and the assembly must agree on offsets. Rather than a
generated `asm-offsets` step, hand-written `#define`s are guarded by
`_Static_assert(offsetof(struct cpu, current) == CPU_CURRENT, ...)`,
so drift fails the build instead of corrupting a register at runtime.

### Entry-path changes

`syscall_entry.asm` loses both globals:

```
    swapgs
    mov gs:[CPU_USER_RSP], rsp
    mov rsp, gs:[CPU_KSTACK]
    ...
    mov rsp, gs:[CPU_USER_RSP]
    swapgs
    sysretq
```

The ISR stubs need the same treatment, but **conditionally**: an
interrupt taken from ring 0 must not `swapgs`, or `GS` ends up holding
userland's value inside the kernel. The standard test on the interrupt
frame's saved `CS` handles it. This is the fiddliest code in the
milestone and the likeliest place to spend debugging time.

### Idle thread

Each CPU gets an idle thread. `schedule()` today has a "nothing ready,
keep running whatever's current" path; with an idle thread there is
always something runnable and that special case disappears. `current`
becomes `gs:[CPU_CURRENT]`; `tss.rsp0` becomes `cpu->tss->rsp0`.

## Locking

```c
struct spinlock { uint32_t locked; uint8_t rank; const char *name; };
struct mutex    { int locked; struct waitq waiters; uint8_t rank; };
```

`spin_lock_irqsave` saves `RFLAGS`, clears `IF`, then acquires with a
real atomic — uncontended on one CPU, but present so milestone 5
changes nothing.

In debug builds the per-CPU block records held ranks, and the checker
panics on:

- non-ascending rank acquisition,
- taking a mutex while holding a spinlock,
- sleeping while holding a spinlock.

All three name both locks involved. Ranks follow the roadmap
hierarchy (process table 0 … PMM zone 10).

Two existing critical sections convert: `syscall.c`'s `fs_lock`
becomes a **mutex** (it wraps disk I/O), and `task_exit`'s bare `cli`
at `process.c:501` becomes ordinary lock usage.

## Wait queues

```c
int  waitq_sleep(struct waitq *q, struct spinlock *release); /* 0 | -EINTR */
void waitq_wake_one(struct waitq *q);
void waitq_wake_all(struct waitq *q);
```

`waitq_sleep` enqueues the caller, sets `BLOCKED` and `blocked_on`,
releases the caller's lock, and schedules.

`wait_for_pid` loses its linear scan entirely: it sleeps on
`proc->exit_waiters`, and process teardown calls `waitq_wake_all`.

### Interruption

`thread_kill(t)` sets `kill_pending` and, if the target is `BLOCKED`,
dequeues it from `blocked_on` and makes it `READY`. The sleeper
observes `kill_pending` and returns `-EINTR`, unwinds its syscall, and
exits.

`exit()` therefore: sets `proc->exiting`, calls `thread_kill` on every
sibling, and exits the caller. The last thread to leave frees the
address space.

### Recorded limitation

The classic lost-wakeup window lies between releasing the caller's
lock and calling `schedule()`. This milestone closes it with
interrupts off, which is genuinely sufficient on one CPU — no waker
can run. Milestone 5 replaces those internals with a lock handoff.

**The API is already the SMP-ready one**, so no caller changes when
that happens; only `waitq.c`'s internals do.

## Syscalls and library

```c
SYS_THREAD_CREATE  17   (entry, arg)      -> tid
SYS_THREAD_EXIT    18   (code)            -> never returns
SYS_THREAD_JOIN    19   (tid, int *code)  -> 0 | -errno
SYS_THREAD_SELF    20   ()                -> tid
```

`lib/include/thread.h`:

```c
typedef int thread_t;
int      thread_create(thread_t *out, void (*fn)(void *), void *arg);
void     thread_exit(int code) __attribute__((noreturn));
int      thread_join(thread_t t, int *exit_code);
thread_t thread_self(void);
```

The kernel starts a thread at `RIP = entry` with `RDI = arg`, but a
raw `fn` that returns would fall off the end of its stack. The library
therefore passes its own `__thread_trampoline` as `entry`, which calls
`fn(arg)` and then `thread_exit`. Since `lib/` has no `malloc`, the
`(fn, arg)` pair lives in a **static 16-entry table** matching the
stack-slot count — no allocation, and the same bound the kernel
enforces.

`docs/stdlib.md` gains the `<thread.h>` section plus amended `exit()`
and `fork()` entries, per `CLAUDE.md`'s standard-library convention.

## Implementation sequence

A large refactor of code with no host tests. The order is chosen so
each step is verifiable before the next destabilizes anything.

```
1  lock.c        spinlock, mutex, rank checker   + selftest
2  waitq.c       sleep/wake/interrupt            + selftest
3  struct cpu    swapgs, entry asm, idle thread  -- boot log UNCHANGED
4  proc/thread   split, heap-allocate, refcount  -- boot log UNCHANGED
5  stacks        slot bitmap, guard pages
6  syscalls      create/exit/join/self + lib + docs
7  interrupt     kill_pending, exit kills siblings
8  test program + leak gate
```

Steps 3 and 4 are the dangerous ones, and both are verified the same
way: **the four-process boot plus `vfstest` must produce output
identical to before the step**. Neither is supposed to change any
observable behavior, so any diff is a bug. This is the check that
caught string-literal corruption during the VFS milestone.

## Verification

Existing convention: in-kernel selftests announcing `passed`/`FAILED`,
a userland test program, headless QEMU under `timeout`, serial log
grepping. `-cpu Nehalem` still suffices; AVX does not arrive until
milestone 2.

In-kernel selftests for `lock.c` (rank inversion detected, mutex
blocks and wakes) and `waitq.c` (sleep, wake_one, wake_all,
interruption).

`userland/threadtest.c` proves the five properties that distinguish
threads from processes:

- a global written by one thread is visible to another
  (**shared address space**)
- each thread's local variable has a distinct address
  (**separate stacks**)
- `thread_join` returns the exact exit code
- a file opened by one thread is usable by another
  (**shared fd table**)
- `exit()` from the main thread terminates a sibling blocked in
  `wait()` (**interruptible waits**)

Plus the leak gate this project now uses by default: `free_frames` and
`vfs_vnode_in_use_count()` compared across repeated spawn-and-wait
cycles, with any delta explained rather than tolerated.

## Out of scope

- Per-CPU run queues and AP bring-up (milestone 5).
- Scheduling classes and priorities (milestone 6).
- Per-thread XSAVE areas — `fpu_state` stays a fixed 512-byte inline
  array until milestone 2.
- Per-thread `fs_base` / TLS (milestone 4).
- Detached threads, thread cancellation points beyond `kill_pending`,
  and signals.
- Userland mutexes or condition variables; `threadtest.c` uses GCC
  atomic builtins directly.
