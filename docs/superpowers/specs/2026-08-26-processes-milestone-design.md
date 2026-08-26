# NeoOS — Milestone 5: Processes (Scheduler, Syscalls, User Mode)

## Goal

Turn NeoOS into a multi-tasking kernel: preemptive round-robin
scheduling among kernel-managed tasks, a `SYSCALL`/`SYSRET`-based
syscall boundary into ring 3, spawn-based process creation that loads
real ELF64 executables from the FAT16 filesystem (milestone 4), and a
full process lifecycle (`spawn`/`exit`/`wait`) exercised by test
programs running in user mode. This is milestone 5. A standard library
that wraps these syscalls for user programs is deliberately deferred
to the next milestone (see `CLAUDE.md`'s standard-library convention)
— this milestone's test programs call syscalls directly via small
inline-assembly wrappers.

## Success criteria

Booting `neoos.iso` in QEMU and:
- Two spawned user-mode processes each print an identifying message
  in a loop; their output interleaves on serial in a pattern only
  explainable by preemptive round-robin switching (not one process
  running to completion before the other starts).
- A parent process `spawn`s a child, `wait`s on it, and the parent's
  own output (logged after `wait` returns) reflects the child's actual
  exit code — proving the full create → run → exit → reap lifecycle.
- A process that calls `yield()` visibly hands off to another ready
  task sooner than pure timer preemption alone would, distinguishing
  voluntary yielding from forced preemption in the observed ordering.
- A divide-by-zero triggered from user-mode code still produces
  milestone 2's clean register-dump-then-halt (not a triple fault),
  proving the exception path works correctly from ring 3, not only
  from the kernel's own ring 0 code as tested previously.

## Out of scope (future work)

- **The standard library.** Test programs in this milestone issue
  syscalls directly via inline assembly; wrapping them in a proper C
  library is the next milestone, per `CLAUDE.md`'s standard-library
  convention (once it exists, new user-facing kernel features must
  come with a library update — this milestone predates that library,
  so it doesn't yet apply).
- **Syscall argument/pointer validation.** No syscall validates that a
  user-supplied pointer (e.g. `write`'s buffer) actually belongs to
  the calling process's own mapped memory. Safe only because every
  test program in this milestone is trusted, known-good code, not an
  adversarial process — this is a tracked security gap to close before
  NeoOS ever runs untrusted user code.
- `fork()`/`exec()` and copy-on-write — process creation is spawn-style
  only (a fresh address space loaded directly from an ELF image).
- Priority scheduling, MLFQ, or any scheduler beyond single-queue
  round-robin.
- Multi-core/SMP — single execution context, as in every prior
  milestone.
- Filesystem writes, per-process file descriptors, or any I/O beyond
  `write`'s direct-to-serial behavior.
- Signals, pipes, shared memory, or any IPC beyond `wait`.
- Swapping, demand paging, or memory overcommit.

## Architecture

### Task representation and address space sharing

Every process gets its own PML4. Because `SYSCALL` doesn't switch
`CR3`, kernel code (the syscall handler, the scheduler, `pmm`/`paging`
internals) keeps running under whichever process's page tables were
active at the moment of entry — so every process's PML4 must still
map the kernel's own mappings. Each new PML4 is built by copying three
entries from the kernel's live PML4: index 0 (the milestone-3 low
identity map `pmm.c`/`paging.c` rely on internally), index 256 (the
physmap), and index 511 (the kernel's higher-half alias), plus its own
unique low entries for its ELF segments and user stack.

A per-task control block (PCB) holds: PID, state (`READY` / `RUNNING`
/ `BLOCKED` / `ZOMBIE`), its saved kernel `RSP` (for `context_switch`),
its kernel stack's base (for the TSS `RSP0` update on every switch),
its PML4's physical address, parent PID and exit code (for `wait`),
and a run-queue link.

### Preemption and context switch

The existing 100Hz LAPIC timer IRQ (milestone 2) drives preemption:
after a configurable number of ticks (a task's time slice), its
handler invokes the scheduler instead of just logging. The scheduler
picks the next `READY` task (round-robin) and calls
`context_switch(&current->saved_rsp, &next->saved_rsp)` — a minimal
assembly routine that pushes the outgoing task's callee-saved
registers (`RBX`, `RBP`, `R12`-`R15`) onto its own kernel stack, saves
`RSP` into its PCB, loads the incoming task's saved `RSP`, pops its
callee-saved registers, and returns.

That `ret` resumes wherever the incoming task last called
`context_switch` from — for a task that was itself preempted, that's
deep inside this same timer-IRQ call chain, so it naturally unwinds
back through the existing ISR common stub's `iretq`, resuming the task
exactly where it was interrupted, in whichever ring it was running.
`yield()` (a syscall, not an interrupt) calls the scheduler the same
way — one switch mechanism, not two. Before any switch, the TSS's
`RSP0` field is updated to the incoming task's kernel stack, so the
next ring 3 → ring 0 transition (syscall or interrupt) lands correctly.

### Syscall mechanism

`SYSCALL`/`SYSRET` setup uses three MSRs: `STAR` (packs the segment
selectors the CPU loads on entry/return), `LSTAR` (the kernel entry
point's address), and `SFMASK` (masks `RFLAGS.IF` on entry, so the
kernel starts with interrupts off until safely on a kernel stack).
`STAR`'s selector packing has a CPU-dictated layout that constrains
the GDT: on `SYSCALL`, `CS`/`SS` load from `STAR[47:32]` and
`STAR[47:32]+8`; on 64-bit `SYSRET`, `CS`/`SS` load from
`STAR[63:48]+16` and `STAR[63:48]+8`. Satisfying both directions from
one `STAR` value requires extending the existing GDT (currently null,
kernel code `0x08`, kernel data `0x10`, TSS `0x18`) with, in order:
a user-code32 placeholder, user data, and user code64 entries
positioned so the arithmetic above lands on the right descriptors —
the standard layout every 64-bit OS using `SYSCALL` converges on.

Since `SYSCALL` doesn't switch `RSP`, the entry stub's first job is to
swap onto the current task's kernel stack; being single-core, a global
"current task" pointer (updated on every switch, the same place that
updates the TSS's `RSP0`) is enough to find it — no per-CPU storage
needed. Calling convention matches the one every x86_64 syscall reader
already knows (Linux's): `RAX` = syscall number, args in `RDI`, `RSI`,
`RDX`, `R10` (not `RCX` — `SYSCALL` clobbers it with the return
address). The entry stub saves the clobbered registers, switches
stacks, dispatches through a table indexed by `RAX`, then restores and
`SYSRET`s.

The six syscalls:
- `exit(code)` — marks the calling task `ZOMBIE`, records `code`,
  wakes its parent if blocked in `wait`, switches away (never returns).
- `write(buf, len)` — writes `len` bytes from the user buffer to the
  serial console (this project's established diagnostics channel).
- `yield()` — voluntarily invokes the scheduler.
- `getpid()` — returns the calling task's PID.
- `spawn(path_ptr, path_len)` — builds a new process from an ELF64
  executable found at the given path on the FAT16 volume (Section
  "Process creation" below), enqueues it `READY`, returns its PID.
- `wait(pid)` — blocks the calling task until the child with the given
  PID becomes `ZOMBIE`, then reaps it and returns its exit code.

### Process creation (spawn + ELF loading)

`spawn` resolves `path` via `fat16_find`, reads the whole ELF64 image
into a temporary kernel buffer via `fat16_read_file`, then: parses the
ELF header and its `PT_LOAD` program headers; builds a new PML4
(copying in the three shared kernel entries above); for each
`PT_LOAD` segment, allocates backing frames via `pmm_alloc`, maps them
into the new PML4 at `p_vaddr` with permissions from the segment's
flags (`PF_X`→executable, `PF_W`→writable), and copies its bytes in
from the temporary buffer; frees that temporary buffer once every
segment is copied; allocates and maps a fixed-size user stack and a
kernel stack for the new task; builds a PCB whose initial resume state
is arranged so its first `context_switch`-return lands in a small
trampoline that transitions into ring 3 at the ELF's entry point
(rather than resuming kernel code); and enqueues it `READY`.

Test programs are real ELF64 executables — small, statically linked,
freestanding (no libc), calling syscalls via inline-assembly wrappers
matching the convention above — copied onto the FAT16 disk image
alongside milestone 4's existing test files (e.g. `/BIN/HELLO.ELF`,
`/BIN/CHILD.ELF`).

### Scheduler

A single ready queue (round-robin): each task runs for a fixed number
of timer ticks, then moves to the back of the queue. `BLOCKED` tasks
(waiting in `wait`) sit outside this queue entirely and are moved back
to `READY` (and re-enqueued) when the task they're waiting on exits.
When the ready queue is empty, the existing kernel idle loop (`hlt` in
a loop) serves as the implicit idle task.

## File structure

```
kernel/
  process.c/.h        # PCB struct, task table, ready/blocked queues, scheduler, spawn/exit/wait
  context_switch.asm  # context_switch(&old_rsp, &new_rsp) -- minimal callee-saved switch
  syscall.c/.h         # STAR/LSTAR/SFMASK setup, C-level dispatch table
  syscall_entry.asm    # SYSCALL entry stub: stack swap, register save, dispatch, SYSRET
  elf.c/.h             # ELF64 header/program header parsing, segment mapping into a target PML4
```

## Data flow (kmain, extending milestone 4's sequence)

```
... existing milestones 2-4 sequence (interrupts, memory, storage) ...
  -> gdt_init (extended: user code32/data/code64 descriptors added)
  -> syscall_init()                      (NEW: STAR/LSTAR/SFMASK MSRs)
  -> process_init()                      (NEW: task table, idle bookkeeping)
  -> spawn("/BIN/HELLO.ELF")             (NEW: first test process)
  -> spawn("/BIN/CHILD.ELF")             (NEW: second test process, or spawned by HELLO itself per the wait() test)
  -> sti
  -> idle loop (now the scheduler's implicit idle task)
```

Timer IRQ handler: on every tick, decrements the current task's time
slice; at zero, calls the scheduler. Syscall entry: dispatches through
the table built in `syscall_init`.

## Testing / verification

Same approach as every prior milestone — no host-runnable unit tests;
verification is via QEMU and serial log capture:
- **Preemption:** two processes each looping and printing their PID
  interleave on serial, confirmed by the pattern not being
  "all of process A then all of process B."
- **Lifecycle:** a parent's post-`wait` output shows the exact exit
  code its child passed to `exit`.
- **Yield:** comparing the interleave pattern with and without a
  `yield()` call in one process shows a visible ordering difference.
- **Ring 3 fault path:** a forced divide-by-zero in a user-mode test
  program produces the same clean register dump and halt as milestone
  2's ring-0 version, confirmed via serial log and `screendump`.
- **Storage regression:** milestone 4's `fat16_selftest` still passes,
  confirming `spawn`'s use of `fat16_find`/`fat16_read_file` didn't
  disturb the filesystem code's own state.

## Error handling

Unchanged convention from every prior milestone: any fault (a bad ELF,
a bad mapping, a genuine CPU exception) is caught by the existing
exception handlers, which dump registers and halt deterministically —
never a silent triple fault. `spawn` failing (file not found,
malformed ELF, out of memory) returns a clean failure indication to
the caller rather than crashing the kernel. As noted in Out of Scope,
syscall arguments themselves aren't validated against the calling
process's memory in this milestone — errors from a malformed argument
are the calling test program's own bug, not a kernel-enforced boundary
yet.
