# NeoOS

A 64-bit operating system for x86 machines, written from scratch, one
milestone at a time.

It boots on real hardware and under QEMU, runs user programs in ring 3
with their own address spaces, preempts them, delivers POSIX signals to
them, and lets them open files across four mounted filesystems. It is
about 9,400 lines of kernel. There is no host test suite, because there
is no host — everything is verified by booting the thing and reading the
serial log.

The goal is not a teaching toy that stops at "hello world from ring 3".
The goal is a system that a real C library runs on unmodified, that
scales to multiple cores, and that talks to real hardware. That means
doing the unglamorous parts properly: a lock-ordering discipline before
there are two CPUs to deadlock, an address-space manager before anything
calls `malloc`, signal delivery that survives a fault in a signal
handler.

## Why it looks the way it does

**A real libc, not a homegrown one.** NeoOS is being pointed at
[musl](https://musl.libc.org/) rather than growing its own `printf`
forever. The kernel is *not* reshaped into Linux to make that work —
instead it provides Linux-*shaped* primitives (`mmap`, `futex`,
`clone`, signals) under its own syscall numbers, and a thin shim
translates. The rule is **translation, never emulation**: the moment the
shim starts emulating something instead of forwarding to it, that's the
signal the primitive belongs in the kernel.

**Nothing ships unverified.** Bare-metal code has no unit tests, so
every subsystem carries an in-kernel selftest that announces itself in
the boot log, and every milestone ends with a userland program that
proves the feature from the other side of the syscall boundary. A boot
currently runs eleven selftests before it reaches userland.

**Bugs get written down.** Several comments in this codebase are longer
than the code they explain, because they record something that cost
hours: that `mov gs, ax` silently zeroes `GS_BASE`, that the buddy
allocator stores its free-list links *inside* free blocks so freeing a
live page table corrupts the identity map, that `XRSTOR` faults on a
header a user program can write. Those aren't decorations. They're the
actual findings.

## Try it

You need a `x86_64-elf` cross toolchain, `nasm`, `grub-mkrescue`,
`mtools`, and `qemu-system-x86_64`.

```sh
make            # build the kernel
make iso disk-image
make run        # boot it in QEMU
```

Headless, the way development actually happens:

```sh
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw \
  -display none -no-reboot -serial file:/tmp/neoos.log
```

Then read `/tmp/neoos.log`. Every subsystem reports itself. If anything
says `FAILED`, something regressed.

Two things that will waste your time if nobody tells you:

- **Regenerate the disk images between runs.** A write selftest creates
  `/NEWDIR`, so a second boot on the same image reports a failure that
  isn't one. `rm -f build/disk.img build/disk2.img && make disk-image`.
- **QEMU never exits on its own.** Always wrap it in `timeout`.

## What works today

### Boot and CPU

- [x] Multiboot2 / GRUB, higher-half kernel at `0xFFFFFFFF80000000`
- [x] GDT, IDT, TSS, ring 3 with `SYSCALL`/`SYSRET`
- [x] ACPI table parsing, LAPIC and IOAPIC
- [x] LAPIC timer, preemptive scheduling
- [x] SSE through SSE4.2 (required), MMX
- [x] AVX and AVX2 via `XSAVE`/`XSAVEOPT`, runtime-detected so pre-AVX
      machines still boot
- [x] Per-CPU data through `swapgs`, ready for a second CPU
- [ ] SMP: AP bring-up, TLB shootdown IPIs
- [ ] x2APIC

### Memory

- [x] Buddy physical allocator
- [x] 4-level paging, full physical map, per-process address spaces
- [x] Kernel heap with size classes
- [x] Copy-on-write `fork`
- [x] Per-process memory mappings: `mmap`, `munmap`, `mprotect`
- [x] Demand paging — mappings are recorded, pages arrive on first touch
- [ ] Zoned allocation (`ZONE_DMA` below 16MiB, `ZONE_DMA32` below 4GiB)
- [ ] An executable region for loadable modules

### Processes and threads

- [x] ELF loading, `spawn`, `fork`, `exec`
- [x] Separate `struct process` and `struct thread`, refcounted
- [x] Kernel threads and a per-CPU idle thread
- [x] Thread stacks with unmapped guard pages
- [x] Spinlocks, sleeping mutexes, and a **lock-order checker** that
      panics on rank inversion — it has already caught a real one
- [x] Wait queues with interruptible sleep
- [ ] Scheduling classes (real-time / fair / idle), SMT-aware balancing

### Signals

- [x] 64 signals including real-time, with queued payloads
- [x] `sigaction`, `sigprocmask`, `sigpending`, `sigsuspend`,
      `sigaltstack`, `sigtimedwait`
- [x] `SA_RESTART`, `SA_SIGINFO`, `SA_ONSTACK`, nested handlers
- [x] CPU faults become signals — a divide-by-zero kills the process,
      not the machine
- [x] Job control: `SIGSTOP`/`SIGCONT`, `wait4` with POSIX status,
      process groups and sessions
- [ ] Core dumps (deliberately not — the status bit is defined, never set)

### Filesystems

- [x] VFS with a refcounted vnode cache and mount points
- [x] FAT16 and FAT32, variant auto-detected from the volume
- [x] `ramfs` and `devfs`
- [x] `readdir`/`getdents`, `mount`/`umount` from userland
- [x] Standard streams are real `/dev/CONSOLE` vnodes, not special-cased
      integers
- [ ] exFAT
- [ ] Generic block-device layer
- [ ] AHCI / SATA

### Userland

- [x] `libneoos`, a small native C library
- [x] 40 syscalls
- [x] 16 test programs covering every subsystem
- [ ] **musl libc, statically linked** — in progress
- [ ] Dynamic linking (musl's own `ldso`)
- [ ] pthreads
- [ ] `getrandom` backed by a real entropy pool

### Drivers

- [x] ATA PIO, serial, VGA text, PS/2 keyboard
- [ ] PCI enumeration, MSI, DMA
- [ ] USB: xHCI, then EHCI/UHCI/OHCI, HID and mass storage
- [ ] Audio: AC97, Intel HDA, SB16
- [ ] Floppy (8237 ISA DMA)

### IPC

- [ ] Pipes, semaphores, message ports
- [ ] An MPI library on top of them

## Where it's going

Seventeen milestones are planned; fourteen have shipped. The current one
brings up musl statically — memory mappings and demand paging landed,
and the process startup ABI, `clone`, and the entropy pool are next.

After that: dynamic linking, full thread-local storage, SMP, scheduling
classes, loadable kernel modules, PCI and AHCI, a block layer, exFAT,
IPC, USB and audio. The full dependency reasoning lives in
[`docs/superpowers/specs/2026-08-27-roadmap-architecture-design.md`](docs/superpowers/specs/2026-08-27-roadmap-architecture-design.md),
which is worth reading before proposing changes — it records *why* the
order is what it is, and which decisions are already settled.

## How the work happens

Each milestone goes brainstorm → design spec → implementation plan →
implementation, and every one of those documents is committed. The specs
in `docs/superpowers/specs/` record the decisions and, importantly, the
ones deliberately *not* taken. The plans in `docs/superpowers/plans/`
carry the step-by-step work, and get amended in place when reality
disagrees with them — a plan that was wrong is more useful annotated
than quietly fixed.

Development happens directly on `main`.

## Layout

```
kernel/
  mm/      physical allocator, paging, heap, address-space mappings
  fs/      VFS, FAT16/32, ramfs, devfs
  sched/   processes, threads, the scheduler
  drivers  ATA, serial, VGA, keyboard, APIC, ACPI, PIT
lib/       libneoos: the native C library and its startup code
userland/  test programs, one per subsystem
third_party/musl/   vendored musl 1.2.5
docs/      the standard library reference, specs, and plans
```

## License

Not yet chosen. The vendored musl under `third_party/musl` is MIT,
with its own copyright notice.
