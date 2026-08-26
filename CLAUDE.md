# NeoOS

A hobby x86_64 kernel built from scratch: Multiboot2 boot, interrupts,
memory management, storage, and onward, one milestone at a time.

## Project conventions

- Development proceeds in milestones: brainstorm -> design spec
  (`docs/superpowers/specs/`) -> implementation plan
  (`docs/superpowers/plans/`) -> implementation, verified via headless
  QEMU and serial log capture (no host-runnable unit tests -- this is
  bare-metal code with no host runtime to run tests in).
- Work happens directly on `main` (no feature branches), matching
  every milestone so far, by explicit user preference.

## Standard library convention

NeoOS will have a standard library that user-mode programs link
against instead of issuing raw syscalls directly (planned as the
milestone right after process management). Once that library exists:

**Any kernel feature that becomes usable by an external/user-mode
application (a new syscall, a new syscall argument, a new capability)
MUST be accompanied by:**
1. A corresponding wrapper or update in the standard library.
2. An update to the standard library's own documentation describing
   the new or changed function.

Do not leave a user-facing kernel feature exposed only via a raw
syscall number with no library support — that defeats the purpose of
having the library. Until the standard library milestone lands, this
doesn't yet apply (early process-management test programs use raw
syscalls directly), but treat it as binding from that point on.
