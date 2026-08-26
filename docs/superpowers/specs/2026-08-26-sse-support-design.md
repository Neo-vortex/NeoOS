# NeoOS — Milestone 8: SSE/SSE2/SSE3/SSE4 Support

## Goal

Let user-mode programs use SSE, SSE2, SSE3, SSSE3, SSE4.1, and SSE4.2
floating-point and vector instructions. Today both kernel `CFLAGS` and
`USER_CFLAGS` pass `-mno-mmx -mno-sse -mno-sse2` — the userland flag
was added in milestone 5 specifically to avoid `#UD`, since FPU/SSE
CPU state is never initialized (no CR0/CR4 setup, no FXSAVE/FXRSTOR
anywhere, `context_switch.asm` only saves callee-saved GPRs xv6-style).
This milestone adds that missing state management for user processes.
First of a four-milestone sequence agreed with the user: SSE → VFS →
copy-on-write VM → SMP.

## Success criteria

- Boot log confirms SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2 detected via
  CPUID before any user process runs.
- Two concurrently-spawned instances of a new `sse_test.c` — each
  accumulating a different floating-point value via both scalar
  `double` arithmetic and explicit SSE/SSE4.1 vector intrinsics across
  preemption-friendly iterations — each self-check their own final
  result and print `passed`, proving per-task FPU/SSE state survives
  real preemption without leaking or corrupting across tasks.
- All milestone 6/7 test programs continue to reproduce their exact
  prior behavior (this milestone touches user-mode CPU state and
  build flags, not the syscall ABI or filesystem).
- Zero `FAILED` lines, zero exceptions, across every verification run.

## Out of scope (future work)

- Kernel-side SSE/floating-point usage — kernel `CFLAGS` are
  unchanged; only user-mode code gets SSE. Extending this to the
  kernel would require FPU/SSE save/restore around every interrupt
  and exception entry (`isr.asm`, `syscall_entry.asm`), not just task
  context switches — meaningfully larger scope, not needed today.
- Lazy FPU/SSE state switching via `CR0.TS` + `#NM` — this milestone
  eagerly saves/restores on every context switch instead. Simpler, no
  new exception handling, and the performance difference doesn't
  matter at this project's task-count scale.
- AVX/AVX2/AVX-512/XSAVE — not requested; FXSAVE/FXRSTOR (the legacy,
  fixed 512-byte format) fully covers SSE1-4 state.
- MMX — stays disabled (`-mno-mmx` unchanged in `USER_CFLAGS`); not
  requested, and it carries its own x87-register-aliasing complexity.
- Any syscall or API for user programs to customize MXCSR exception
  masks — defaults (`0x1F80`, all exceptions masked) are used
  unconditionally.
- Mount points / VFS / copy-on-write VM / SMP — later milestones in
  the agreed sequence, each brainstormed separately.

## Architecture

### CPU feature enablement

A new `kernel/cpu.c` / `kernel/cpu.h` (single responsibility: detect
and enable CPU features) adds `cpu_init()`, called in `kmain` after
`idt_init()` (so any unexpected fault during CR0/CR4/FXSAVE setup gets
a clean diagnostic dump instead of a triple fault) and before
`process_init()` (so the default state template is ready before any
task is created):

1. **CPUID check** — query leaf 1: EDX bits 25/26 (SSE/SSE2), ECX bits
   0/9/19/20 (SSE3/SSSE3/SSE4.1/SSE4.2). If any are missing, print
   which and halt — the kernel binary is compiled assuming their
   presence (userland gets `-msse3 -mssse3 -msse4.1 -msse4.2`), so
   continuing would just mean a `#UD` the moment user code executes an
   affected instruction. No fallback mode, matching the project's
   existing "clean failure over silent corruption" convention (e.g.
   ATA/ACPI failure handling).
2. **CR0/CR4 setup** — clear `CR0.EM` (bit 2, disables FPU emulation
   trapping), set `CR0.MP` (bit 1). Set `CR4.OSFXSR` (bit 9, required
   for any SSE instruction to not `#UD`) and `CR4.OSXMMEXCPT` (bit 10,
   so unmasked SIMD FP exceptions raise `#XM` instead of `#UD`). `#XM`
   is vector 19; `idt_init` already wires all 256 vectors to a generic
   ISR stub, so this needs no new exception-handling code — it already
   gets the same clean register-dump-and-halt as every other fault.
3. **Default FPU/SSE state template** — execute `FNINIT` (resets x87
   to its defined default) and `LDMXCSR` with `0x1F80` (all exceptions
   masked, round-to-nearest — the real hardware reset default, set
   explicitly rather than assumed), then one `FXSAVE` into a static
   512-byte template buffer. Every new task's FXSAVE-format state is
   initialized from this template rather than left as zeroed BSS — an
   all-zero MXCSR would unmask every SSE FP exception, not standard
   behavior.

### Per-task state and context-switch integration

`struct task` (`kernel/process.h`) gains
`uint8_t fpu_state[512] __attribute__((aligned(16)));` — FXSAVE/
FXRSTOR require 16-byte-aligned memory or they `#GP`; the attribute on
this one field forces the compiler to align the whole struct (and
therefore every element of the static `tasks[]` array) to 16 bytes, no
other changes needed for that guarantee.

Both `task_create_kernel_thread` and `spawn` initialize the new task's
`fpu_state` by copying the default template from `cpu.c` (a hand-rolled
byte-copy loop, matching this codebase's existing style). Kernel
threads get this too, not just user processes — special-casing by task
type would add a branch to `schedule()` to save a few microseconds
that doesn't matter here; uniform treatment keeps the scheduler
simpler.

Placement is the subtle part: since `context_switch` performs a
coroutine-style stack swap (the C code "after" the call doesn't resume
until *this* task is scheduled back in, using its own stale locals
from whenever it originally called `schedule()`), both the outgoing
save and incoming restore happen **before** the call, in `schedule()`:

```c
if (prev) {
    fxsave(prev->fpu_state);
}
fxrstor(next->fpu_state);
context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);
```

`fxsave`/`fxrstor` are small `static inline` wrappers (in `cpu.h`)
around the single corresponding instruction with a memory operand — no
changes to `context_switch.asm` itself, since GPR/stack switching and
FPU/SSE state are fully independent concerns.

### Build flags and QEMU CPU pinning

`USER_CFLAGS` drops `-mno-sse -mno-sse2` and adds `-msse3 -mssse3
-msse4.1 -msse4.2` (SSE/SSE2 are already part of the x86-64 ABI
baseline once not explicitly disabled; `-mno-mmx` stays, since MMX is
out of scope). Kernel `CFLAGS` are unchanged.

The `Makefile`'s `run` target pins QEMU's CPU model to `-cpu Nehalem`
— verified via QEMU's `query-cpu-model-expansion` QMP command on this
project's installed QEMU (10.2.1) to report `sse`/`sse2`/`sse3`/
`ssse3`/`sse4.1`/`sse4.2` all `true`. Nehalem is a real Intel
microarchitecture that introduced SSE4.2, so the model is
self-documenting about exactly which extensions it guarantees, unlike
a generic baseline model (`qemu64`) or the deliberately-everything
`max` model.

## Testing / verification

Same QEMU + serial-log approach as every prior milestone:
- Full boot log check: CPUID detection message, zero `FAILED`, zero
  exceptions.
- The new `sse_test.c`, spawned twice concurrently (mirroring
  `looper.c`'s two-instance pattern): each instance picks a different
  floating-point increment from its own PID (odd/even), accumulates it
  over ~20 preemption-friendly iterations via both scalar `double`
  arithmetic and explicit SSE/SSE4.1 vector intrinsics (`__m128`,
  `_mm_add_ps`, `_mm_floor_ps`), then self-checks its own final result
  and prints `passed`/`FAILED`. This is the milestone's key test: it
  proves per-task FPU/SSE state survives real preemption without
  leaking or corrupting across tasks, not just that SSE instructions
  execute without `#UD`.
- Regression: all milestone 6/7 test programs (`spin`, `child`,
  `parent`, `looper`, `yielder`, `faulter`, `fileio`) reproduce their
  exact prior behavior.

## Error handling

Unchanged convention: kernel-side faults still produce a clean
register dump and halt, never silent corruption. The one new failure
mode this milestone introduces — the boot-time CPUID check finding a
required SSE extension missing — halts immediately with a diagnostic
message, for the reason given in Architecture: the kernel binary
already assumes their presence, so continuing serves no purpose. As in
every prior milestone, syscall argument pointers are not validated
against the calling process's own memory — the same tracked, deferred
security gap, unaffected by this milestone (which touches no
syscalls).
