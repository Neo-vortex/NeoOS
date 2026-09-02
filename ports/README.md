# Ports — third-party applications on NeoOS

Applications live here, kernel work does not. The two move at different
speeds and are gated differently: a broken port must never be able to
fail the kernel's regression suite.

## Layout

    ports/<name>/upstream/       a PRISTINE upstream checkout (submodule)
    ports/<name>/...             everything NeoOS-specific: patches,
                                 shims, config, build glue

Upstream is never edited in place. This mirrors `third_party/shim` and
`third_party/busybox-config`, for the same reason: a version bump has to
be a submodule pointer move, not a merge.

## How ports are gated

`make ports` builds them. `make ports-test` runs their smoke checks in
their own boot, and they are deliberately **absent from the default
INITTAB**: the boot suite and the gauntlet are a KERNEL regression
harness, and an application failing in it would make the gate mean
something other than "the kernel is sound".

## What NeoOS offers a port

Know these before starting; each has cost a day when discovered late.

- **Static linking only.** There is no dynamic linker, so no `dlopen`,
  no plugins, no optional-library probing. A program whose architecture
  is "dlopen what is available" is not portable here yet.
- **`.nex` executables.** Built as ELF, stamped at disk-image time.
- **Case-SENSITIVE paths**, on FAT too.
- **`/proc` has two files per process**: `stat` and `cmdline`. There is
  no `/sys` at all.
- **No users, modes or ownership.** FAT cannot store them.
- **No symlinks**, for the same reason.
- **musl is the C library**, reached through the shim in
  `third_party/shim`, which translates Linux syscall numbers to NeoOS's.

## The workflow that works

Do not predict what a program needs. Build it, run it, and read the
`[shim] ENOSYS <n>` lines it produces: the shim reports every unmapped
syscall number once. BusyBox turned a predicted list of ~25 syscalls
into a measured list of 3 that way.

Then add the primitive **to the kernel**. Never emulate it in the shim,
and never add a special case for a port: if a port needs something, it
becomes a real primitive with its own test, or the port gets patched.
Otherwise applications quietly bend the OS into a shape nobody chose.
