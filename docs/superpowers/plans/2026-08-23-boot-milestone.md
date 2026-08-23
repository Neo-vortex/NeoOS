# NeoOS Boot Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot a 64-bit kernel under QEMU via legacy BIOS + GRUB (Multiboot2), ending with the C kernel printing "NeoOS booted" to the VGA text buffer and halting cleanly.

**Architecture:** GRUB loads a Multiboot2 ELF in 32-bit protected mode. A NASM boot stub (`boot/boot.asm`) checks CPUID for long-mode support, builds identity-mapped page tables, switches to 64-bit long mode, and calls into a freestanding C kernel (`kernel/kernel.c`) which writes directly to VGA memory at `0xB8000`.

**Tech Stack:** NASM (assembler), a from-source `x86_64-elf-gcc`/`binutils` cross-compiler, GNU Make, GRUB (`grub-mkrescue`) for the BIOS-bootable ISO, QEMU for testing.

**Spec:** `docs/superpowers/specs/2026-08-23-boot-milestone-design.md`

## Global Constraints

- Target triple: `x86_64-elf` (freestanding, no libc, no host OS assumptions).
- Boot path: legacy BIOS via GRUB + Multiboot2 only — no UEFI in this milestone.
- CPU mode: 64-bit long mode, identity-mapped first 1GiB via 2MiB pages — no higher-half mapping in this milestone.
- Assembler: NASM, output format `elf64` (mixed `bits 32` / `bits 64` sections in one file).
- Cross-compiler versions: binutils `2.47`, GCC `15.3.0` (matches host GCC 15.2.0's major series), built with `--enable-languages=c --without-headers` (no libstdc++, no libc headers).
- Toolchain is built and installed outside the repo at `$HOME/opt/cross-x86_64-elf` — never committed.
- No unit tests in the traditional sense: this is boot-level code with no host runtime. Every task's verification step boots `neoos.iso` in QEMU and inspects the VGA text output, either interactively (`make run`) or headlessly via QEMU monitor `screendump` + a PNG conversion an agent can view with the Read tool.

---

### Task 1: Toolchain Setup & Repo Scaffolding

**Files:**
- Create: `.gitignore`
- Create: `toolchain/env.sh`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: `$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-gcc` and `x86_64-elf-ld` (used by Task 2's `Makefile` and Task 5's C compilation step); `nasm` on `PATH` (used by Task 2's `Makefile`); `.gitignore` covering build output.

- [ ] **Step 1: Install host build dependencies and OS-dev tools**

Run:
```bash
sudo apt-get update
sudo apt-get install -y build-essential bison flex libgmp-dev libmpc-dev \
    libmpfr-dev texinfo libisl-dev nasm xorriso grub-pc-bin mtools wget
```
Expected: all packages install without error.

- [ ] **Step 2: Download and extract binutils and GCC sources**

Run:
```bash
mkdir -p ~/src ~/opt/cross-x86_64-elf
cd ~/src
wget -nc https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz
wget -nc https://ftp.gnu.org/gnu/gcc/gcc-15.3.0/gcc-15.3.0.tar.xz
tar xf binutils-2.47.tar.xz
tar xf gcc-15.3.0.tar.xz
```
Expected: `~/src/binutils-2.47/` and `~/src/gcc-15.3.0/` directories exist.

- [ ] **Step 3: Build and install the cross binutils**

Run:
```bash
export PREFIX="$HOME/opt/cross-x86_64-elf"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

mkdir -p ~/src/build-binutils
cd ~/src/build-binutils
../binutils-2.47/configure --target=$TARGET --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$(nproc)"
make install
```
Expected: exits 0; `$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-ld` exists.

- [ ] **Step 4: Build and install the cross GCC (C only, no libc)**

Run:
```bash
export PREFIX="$HOME/opt/cross-x86_64-elf"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

mkdir -p ~/src/build-gcc
cd ~/src/build-gcc
../gcc-15.3.0/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j"$(nproc)" all-gcc
make -j"$(nproc)" all-target-libgcc
make install-gcc
make install-target-libgcc
```
Expected: exits 0; `$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-gcc` exists.

- [ ] **Step 5: Verify the toolchain**

Run:
```bash
~/opt/cross-x86_64-elf/bin/x86_64-elf-gcc --version
nasm -v
```
Expected: both print version strings (GCC 15.3.0, NASM 3.0x) with no "command not found" errors.

- [ ] **Step 6: Create repo scaffolding**

Create `/home/neo/projects/personal/NeoOS/.gitignore`:
```
build/
iso/
*.o
*.elf
*.iso
```

Create `/home/neo/projects/personal/NeoOS/toolchain/env.sh`:
```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
```

- [ ] **Step 7: Commit**

```bash
git add .gitignore toolchain/env.sh
git commit -m "Add cross-compiler toolchain setup and repo scaffolding"
```

---

### Task 2: Multiboot2 Header, Minimal Boot Stub, Build Pipeline

**Files:**
- Create: `boot/boot.asm`
- Create: `linker.ld`
- Create: `boot/grub.cfg`
- Create: `Makefile`

**Interfaces:**
- Consumes: `x86_64-elf-gcc` at `$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-gcc` and `nasm` on `PATH` (Task 1). Host `grub-mkrescue`, `qemu-system-x86_64` (already present).
- Produces: `build/neoos.iso` (via `make iso`), `make run` target. `boot/boot.asm` exports `global _start` — Task 3 will extend this same file.

- [ ] **Step 1: Write the Multiboot2 header and minimal 32-bit entry point**

Create `boot/boot.asm`:
```nasm
; boot/boot.asm — Multiboot2 header + minimal 32-bit entry point

MULTIBOOT2_MAGIC    equ 0xe85250d6
MULTIBOOT2_ARCH     equ 0
MULTIBOOT2_LEN      equ (header_end - header_start)
MULTIBOOT2_CHECKSUM equ -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_LEN)

section .multiboot_header
header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd MULTIBOOT2_LEN
    dd MULTIBOOT2_CHECKSUM
    ; required end tag
    dw 0    ; type
    dw 0    ; flags
    dd 8    ; size
header_end:

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
[bits 32]
global _start

_start:
    mov esp, stack_top

    mov byte [0xb8000], 'B'
    mov byte [0xb8001], 0x0a
    mov byte [0xb8002], 'O'
    mov byte [0xb8003], 0x0a
    mov byte [0xb8004], 'O'
    mov byte [0xb8005], 0x0a
    mov byte [0xb8006], 'T'
    mov byte [0xb8007], 0x0a
    mov byte [0xb8008], 'O'
    mov byte [0xb8009], 0x0a
    mov byte [0xb800a], 'K'
    mov byte [0xb800b], 0x0a

    cli
.hang:
    hlt
    jmp .hang
```

- [ ] **Step 2: Write the linker script**

Create `linker.ld`:
```
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(8) :
    {
        *(.multiboot_header)
    }

    .text ALIGN(4K) :
    {
        *(.text)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata)
    }

    .data ALIGN(4K) :
    {
        *(.data)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
    }
}
```

- [ ] **Step 3: Write the GRUB config**

Create `boot/grub.cfg`:
```
menuentry "NeoOS" {
    multiboot2 /boot/kernel.elf
    boot
}
```

- [ ] **Step 4: Write the Makefile**

Create `Makefile`:
```makefile
CC := $(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-gcc
AS := nasm

ASFLAGS := -f elf64

BUILD_DIR := build
ISO_DIR := iso

.PHONY: all build iso run clean

all: build

build: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR)/boot.o: boot/boot.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) boot/boot.asm -o $(BUILD_DIR)/boot.o

$(BUILD_DIR)/kernel.elf: $(BUILD_DIR)/boot.o linker.ld
	$(CC) -T linker.ld -o $(BUILD_DIR)/kernel.elf -ffreestanding -O2 -nostdlib $(BUILD_DIR)/boot.o -lgcc

iso: $(BUILD_DIR)/kernel.elf
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD_DIR)/kernel.elf $(ISO_DIR)/boot/kernel.elf
	cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD_DIR)/neoos.iso $(ISO_DIR)

run: iso
	qemu-system-x86_64 -cdrom $(BUILD_DIR)/neoos.iso

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR)
```

- [ ] **Step 5: Build the ISO**

Run:
```bash
make iso
```
Expected: exits 0; `build/neoos.iso` exists.

- [ ] **Step 6: Verify boot output headlessly**

Run:
```bash
rm -f /tmp/neoos-monitor.sock /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
```
Then view `/tmp/neoos-screen.png` with the Read tool.
Expected: the image shows "BOOTOK" in light green text near the top-left of an otherwise black screen — proving GRUB accepted the Multiboot2 header and ran the kernel (not a GRUB "invalid magic"/error screen, not a reboot loop).

- [ ] **Step 7: Commit**

```bash
git add boot/boot.asm linker.ld boot/grub.cfg Makefile
git commit -m "Add minimal Multiboot2 boot stub and build/iso/run pipeline"
```

---

### Task 3: CPUID Long-Mode Detection

**Files:**
- Modify: `boot/boot.asm`

**Interfaces:**
- Consumes: `_start` entry point from Task 2 (same file, extended in place).
- Produces: `check_long_mode_supported` routine — called by `_start` before continuing; halts with a visible error pattern if the CPU can't do long mode. Task 4 will call this same routine before building page tables.

- [ ] **Step 1: Add the long-mode check, gating the existing success path on it**

Edit `boot/boot.asm` — replace the `_start:` block and everything below it with:
```nasm
_start:
    mov esp, stack_top
    mov edi, ebx            ; save multiboot info pointer before cpuid clobbers ebx

    call check_long_mode_supported

    mov byte [0xb8000], 'B'
    mov byte [0xb8001], 0x0a
    mov byte [0xb8002], 'O'
    mov byte [0xb8003], 0x0a
    mov byte [0xb8004], 'O'
    mov byte [0xb8005], 0x0a
    mov byte [0xb8006], 'T'
    mov byte [0xb8007], 0x0a
    mov byte [0xb8008], 'O'
    mov byte [0xb8009], 0x0a
    mov byte [0xb800a], 'K'
    mov byte [0xb800b], 0x0a

    cli
.hang:
    hlt
    jmp .hang

; Halts with a red "ERR" pattern on VGA if the CPU lacks CPUID's
; extended long-mode leaf, or long mode itself isn't supported.
check_long_mode_supported:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .no_long_mode

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret

.no_long_mode:
    mov byte [0xb8000], 'E'
    mov byte [0xb8001], 0x4f
    mov byte [0xb8002], 'R'
    mov byte [0xb8003], 0x4f
    mov byte [0xb8004], 'R'
    mov byte [0xb8005], 0x4f
    cli
.hang2:
    hlt
    jmp .hang2
```

- [ ] **Step 2: Temporarily force the failure branch and verify the error pattern shows**

Temporarily add a line right after the `check_long_mode_supported:` label:
```nasm
check_long_mode_supported:
    jmp .no_long_mode
    pushfd
```
Run:
```bash
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: "ERR" in white-on-red near the top-left, not "BOOTOK".

- [ ] **Step 3: Remove the forced jump and verify normal success path still works**

Remove the `jmp .no_long_mode` line added in Step 2 so `check_long_mode_supported` reads exactly as written in Step 1.
Run:
```bash
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: "BOOTOK" in light green again (QEMU's virtual CPU supports long mode, so the check passes).

- [ ] **Step 4: Commit**

```bash
git add boot/boot.asm
git commit -m "Add CPUID long-mode detection with VGA error path"
```

---

### Task 4: Page Tables and Long-Mode Transition

**Files:**
- Modify: `boot/boot.asm`

**Interfaces:**
- Consumes: `check_long_mode_supported` from Task 3 (called before building page tables).
- Produces: `long_mode_start` — the 64-bit entry point reached after paging is enabled and the far jump completes. Task 5 will replace this routine's body with a call into the C kernel.

- [ ] **Step 1: Add page table setup, paging/long-mode enablement, GDT64, and the mode switch**

Edit `boot/boot.asm`:

Replace the `section .bss` block with:
```nasm
section .bss
align 4096
p4_table:
    resb 4096
p3_table:
    resb 4096
p2_table:
    resb 4096
align 16
stack_bottom:
    resb 16384
stack_top:
```

Replace the `_start:` block's success path (the `mov byte [0xb8000], 'B'` ... `.hang:` lines) with:
```nasm
_start:
    mov esp, stack_top
    mov edi, ebx            ; save multiboot info pointer before cpuid clobbers ebx

    call check_long_mode_supported
    call set_up_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start
```

Immediately after `check_long_mode_supported`'s closing `.hang2: hlt / jmp .hang2` block, add:
```nasm
; Identity-maps the first 1GiB using 2MiB pages: PML4[0] -> PDPT[0] -> 512 PD entries.
set_up_page_tables:
    mov eax, p3_table
    or eax, 0b11
    mov [p4_table], eax

    mov eax, p2_table
    or eax, 0b11
    mov [p3_table], eax

    mov ecx, 0
.map_p2_table:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10000011      ; present + writable + huge page (2MiB)
    mov [p2_table + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jne .map_p2_table
    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5           ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080      ; EFER MSR
    rdmsr
    or eax, 1 << 8           ; LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31          ; PG
    mov cr0, eax
    ret

section .rodata
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
.data: equ $ - gdt64
    dq (1<<41) | (1<<44) | (1<<47)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .text
[bits 64]
long_mode_start:
    mov ax, gdt64.data
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov byte [0xb8000], 'L'
    mov byte [0xb8001], 0x0b
    mov byte [0xb8002], 'O'
    mov byte [0xb8003], 0x0b
    mov byte [0xb8004], 'N'
    mov byte [0xb8005], 0x0b
    mov byte [0xb8006], 'G'
    mov byte [0xb8007], 0x0b
    mov byte [0xb8008], '6'
    mov byte [0xb8009], 0x0b
    mov byte [0xb800a], '4'
    mov byte [0xb800b], 0x0b

    cli
.hang:
    hlt
    jmp .hang
```

The full file should now read top to bottom as: Multiboot2 header, `.bss` (page tables + stack), 32-bit `.text` (`_start`, `check_long_mode_supported`, `set_up_page_tables`, `enable_paging`), `.rodata` (`gdt64`), 64-bit `.text` (`long_mode_start`).

- [ ] **Step 2: Rebuild and verify the 64-bit marker appears**

Run:
```bash
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: "LONG64" in light cyan near the top-left (not "BOOTOK") — proving execution actually reached 64-bit code after the page tables and mode switch, not just that the 32-bit stub ran.

- [ ] **Step 3: Commit**

```bash
git add boot/boot.asm
git commit -m "Add identity-mapped page tables and long-mode transition"
```

---

### Task 5: C Kernel Entry Point

**Files:**
- Create: `kernel/kernel.h`
- Create: `kernel/kernel.c`
- Modify: `boot/boot.asm`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `long_mode_start` from Task 4 (modified to call into C); `x86_64-elf-gcc` from Task 1.
- Produces: `kmain(void *multiboot_info)` and `vga_print_string(const char *str)`, defined in `kernel/kernel.c` and declared in `kernel/kernel.h`. This is the final task — its output must satisfy the spec's success criteria exactly.

- [ ] **Step 1: Write the kernel header**

Create `kernel/kernel.h`:
```c
#ifndef NEOOS_KERNEL_H
#define NEOOS_KERNEL_H

void kmain(void *multiboot_info);
void vga_print_string(const char *str);

#endif
```

- [ ] **Step 2: Write the kernel**

Create `kernel/kernel.c`:
```c
#include "kernel.h"

static volatile unsigned short *const VGA_BUFFER = (unsigned short *)0xb8000;
static const unsigned short VGA_COLOR_WHITE_ON_BLACK = 0x0f;

void vga_print_string(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        VGA_BUFFER[i] = (unsigned short)((unsigned char)str[i]) |
                        (unsigned short)(VGA_COLOR_WHITE_ON_BLACK << 8);
    }
}

void kmain(void *multiboot_info) {
    (void)multiboot_info;

    vga_print_string("NeoOS booted");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
```

- [ ] **Step 3: Wire the 64-bit boot stub to call kmain**

Edit `boot/boot.asm` — add `extern kmain` under `global _start`:
```nasm
section .text
[bits 32]
global _start
extern kmain
```

Replace the `long_mode_start:` body (everything from `mov byte [0xb8000], 'L'` through the `.hang:`/`jmp .hang` at the end of the file) with:
```nasm
long_mode_start:
    mov ax, gdt64.data
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call kmain

    cli
.hang:
    hlt
    jmp .hang
```

- [ ] **Step 4: Update the Makefile to compile and link the C kernel**

Edit `Makefile`:
```makefile
CC := $(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-gcc
AS := nasm

CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -std=gnu11 -O2
ASFLAGS := -f elf64

BUILD_DIR := build
ISO_DIR := iso

.PHONY: all build iso run clean

all: build

build: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR)/boot.o: boot/boot.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) boot/boot.asm -o $(BUILD_DIR)/boot.o

$(BUILD_DIR)/kernel.o: kernel/kernel.c kernel/kernel.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c kernel/kernel.c -o $(BUILD_DIR)/kernel.o

$(BUILD_DIR)/kernel.elf: $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o linker.ld
	$(CC) -T linker.ld -o $(BUILD_DIR)/kernel.elf -ffreestanding -O2 -nostdlib $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o -lgcc

iso: $(BUILD_DIR)/kernel.elf
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD_DIR)/kernel.elf $(ISO_DIR)/boot/kernel.elf
	cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD_DIR)/neoos.iso $(ISO_DIR)

run: iso
	qemu-system-x86_64 -cdrom $(BUILD_DIR)/neoos.iso

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR)
```

- [ ] **Step 5: Build and verify the final spec success criteria**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: "NeoOS booted" in white-on-black near the top-left of the screen. This is the milestone's final success criterion — also confirm no reboot loop occurred (QEMU process exited only after the monitor's `quit` command, not on its own).

Also do a final interactive sanity check:
```bash
make run
```
Expected: a QEMU window opens showing "NeoOS booted"; close the window when done (or use the QEMU monitor `quit` command / Ctrl+Alt+2 then `quit`).

- [ ] **Step 6: Commit**

```bash
git add kernel/kernel.h kernel/kernel.c boot/boot.asm Makefile
git commit -m "Add C kernel entry point printing NeoOS booted to VGA"
```
