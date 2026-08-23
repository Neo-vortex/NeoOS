# NeoOS — Milestone 1: Boot to VGA Message

## Goal

Get a minimal 64-bit kernel booting under QEMU via legacy BIOS + GRUB,
proving the full chain (bootloader → long-mode transition → C kernel)
works end to end. This is the first milestone of NeoOS; app runtime,
shell, and userland design are explicitly out of scope and will be
decided in a later spec.

## Success criteria

Running `make run` boots `neoos.iso` in QEMU and:
- The screen shows the text "NeoOS booted" (VGA text mode).
- The CPU halts cleanly afterward (no reboot loop, no triple fault).
- If the host CPU lacks long-mode support, a visible error pattern
  is shown instead of a silent failure (PAE is implied by long-mode
  support on all real hardware, so no separate PAE check is needed).

## Architecture

### Boot flow

1. **GRUB** (via `grub-mkrescue`) loads a Multiboot2-compliant kernel
   ELF from a BIOS-bootable ISO. On entry the CPU is in 32-bit
   protected mode, paging disabled, A20 line enabled — guaranteed by
   the Multiboot2 spec.
2. **`boot/boot.asm`** (NASM, mixed `bits 32` / `bits 64`):
   - Multiboot2 header so GRUB recognizes the kernel image.
   - Sets up a small 32-bit stack.
   - Checks CPUID for long-mode and PAE support. If unsupported,
     writes an error pattern directly to the VGA text buffer and
     halts (`cli; hlt` loop) rather than continuing into an undefined
     state.
   - Builds minimal identity-mapped page tables (PML4 → PDPT → PD
     using 2MB pages) covering the first 1GiB — no higher-half
     kernel mapping in this milestone.
   - Enables PAE (CR4.PAE), sets the LME bit in the EFER MSR, enables
     paging (CR0.PG), loads a 64-bit GDT, and far-jumps into 64-bit
     code.
   - Sets up a 64-bit stack and calls `kmain(multiboot_info_ptr)`.
3. **`kernel/kernel.c`** (C, freestanding, compiled with
   `x86_64-elf-gcc`): `kmain` writes "NeoOS booted" directly into the
   VGA text buffer at `0xB8000`, then halts in an infinite `hlt` loop.

### Repo layout

```
NeoOS/
  boot/boot.asm       # multiboot2 header + real/protected -> long mode transition
  kernel/kernel.c     # kmain, VGA text output
  kernel/kernel.h
  linker.ld           # entry point, section layout
  Makefile            # build, iso, run targets
  .gitignore          # excludes build output, iso staging, cross-compiler build dir
  docs/superpowers/specs/  # design specs (this file)
```

### Toolchain

- **Cross-compiler**: `x86_64-elf-gcc` / `x86_64-elf-binutils`, built
  from source per the standard OSDev "GCC Cross-Compiler" recipe (or
  installed from a distro/AUR package if one is available — confirmed
  during implementation).
- **NASM** assembles `boot.asm`.
- **Host packages needed**: `xorriso`, `grub-pc-bin` (for
  `grub-mkrescue` to produce a BIOS-bootable ISO), `qemu-system-x86_64`.

### Build system

`Makefile` targets:
- `make build` — assemble `boot.asm`, compile `kernel.c`, link into
  `kernel.elf` via `linker.ld`.
- `make iso` — stage `kernel.elf` + a GRUB config under
  `iso/boot/grub/`, run `grub-mkrescue` to produce `neoos.iso`.
- `make run` — boot `neoos.iso` in `qemu-system-x86_64 -cdrom`.

### Testing / verification

No unit tests at this level — this is boot-level code with no host
environment to run it in. Verification is manual: `make run` boots
the ISO in QEMU and the operator confirms the success criteria above
(message displayed, clean halt, error path visible if long mode is
unsupported).

## Out of scope (future specs)

- Higher-half kernel mapping.
- Keyboard input / interrupt handling (IDT, PIC remap).
- App runtime and userland language/interpreter choice (BASIC/Forth
  vs. something else) — deferred per explicit prior discussion.
- UEFI boot path.
- Real-hardware testing (USB boot).
