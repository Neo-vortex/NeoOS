# NeoOS Interrupts Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn NeoOS from a static print-and-halt kernel into an interruptible one: a full 256-entry IDT with real handlers for all 32 CPU exceptions, a Local APIC + IOAPIC interrupt path (no legacy PIC in the live interrupt path), a PIT-calibrated 100Hz periodic timer, and a working PS/2 keyboard driver, with a serial (COM1) driver as the primary diagnostics channel.

**Architecture:** `kmain` orchestrates a strict init sequence: serial → VGA clear → GDT/TSS → IDT → ACPI/MADT parse → legacy PIC disable → Local APIC enable → IOAPIC init → PIT-calibrated LAPIC timer → IOAPIC keyboard routing → `sti` → interruptible idle loop. Each subsystem gets its own `.c`/`.h` pair; exceptions and IRQs share one assembly trampoline (`isr.asm`) and one C dispatcher (`isr.c`) keyed by vector number.

**Tech Stack:** Same toolchain as the boot milestone (NASM, `x86_64-elf-gcc` cross-compiler, GNU Make, GRUB, QEMU).

**Spec:** `docs/superpowers/specs/2026-08-23-interrupts-milestone-design.md`

## Global Constraints

- Freestanding C (`-ffreestanding -nostdlib`), no libc — only GCC's own freestanding headers (`stdint.h`, `stddef.h`) are available, and they are always present regardless of libc.
- **All interrupts in this milestone occur while the CPU is already in ring 0** (no userland/ring 3 exists yet). This means the CPU does **not** push `SS`/`RSP` on exception/IRQ entry (that only happens on a privilege-level change) — every ISR/IRQ stub and register frame in this plan is built on that assumption. Getting this wrong is the single most common bug in this kind of code, so it is called out here once instead of per-task.
- The legacy 8259 PIC is remapped off the exception vector range and then masked forever — it is never unmasked again once the IOAPIC path is live.
- Serial (COM1, port `0x3F8`) is the primary diagnostics channel from Task 1 onward; VGA only ever shows a short banner/status line, not scrolling logs.
- GDT layout is fixed for the whole milestone once Task 2 lands: null (`0x00`), kernel code (`0x08`), kernel data (`0x10`), TSS (`0x18`–`0x27`, a 16-byte system descriptor). `idt.c` depends on the `0x08` code selector constant from `gdt.h`.
- Vector assignment: `0x00`–`0x1F` are the 32 CPU exceptions; `0x20` is the LAPIC timer; `0x21` is the keyboard IRQ (routed via IOAPIC); everything else in `0x22`–`0xFF` hits a generic "unhandled interrupt" halt.
- Verification throughout uses headless QEMU: `-serial file:<path>` for grep-able diagnostics, the QEMU monitor's `screendump` for VGA, and `sendkey` to simulate keypresses — same technique as the boot milestone's screendump verification, extended with serial capture.
- **The identity map covers the first 4GiB, not 1GiB.** The boot milestone's page tables only mapped 1GiB; the LAPIC (`0xfee00000`) and IOAPIC (`0xfec00000`) MMIO regions sit in the standard sub-4GiB MMIO hole, well above that. Task 6 discovered this via a page fault (`cr2=0xfee000f0`) and extended `boot/boot.asm`'s page tables (4 PDPT entries × 512 2MiB pages instead of 1×512) to cover it. Any future milestone reasoning about "the identity map" should assume 4GiB, not 1GiB.

---

### Task 1: Serial Driver, VGA Refactor, and Scalable Makefile

**Files:**
- Create: `kernel/io.h`
- Create: `kernel/serial.c`, `kernel/serial.h`
- Create: `kernel/vga.c`, `kernel/vga.h`
- Modify: `kernel/kernel.c`, `kernel/kernel.h`
- Modify: `Makefile`

**Interfaces:**
- Consumes: nothing new (first task of this milestone).
- Produces: `outb`/`inb`/`io_wait` (`kernel/io.h`, used by every later port-I/O driver: PIC, PIT, keyboard). `serial_init`, `serial_putc`, `serial_write_string`, `serial_write_hex64` (`kernel/serial.h`, used by every later task for diagnostics). `vga_clear`, `vga_print_string`, `vga_putc` (`kernel/vga.h`, used by Task 3's exception dump and Task 9's keyboard echo). A Makefile that auto-discovers `kernel/*.c` via `wildcard`, so later tasks that only add `.c` files need no Makefile edit — only tasks that add a new `.asm` file (Tasks 2 and 3) touch `ASM_OBJECTS`.

- [ ] **Step 1: Write the port I/O helpers**

Create `kernel/io.h`:
```c
#ifndef NEOOS_IO_H
#define NEOOS_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" :: "a"((uint8_t)0));
}

#endif
```

- [ ] **Step 2: Write the serial driver**

Create `kernel/serial.h`:
```c
#ifndef NEOOS_SERIAL_H
#define NEOOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_write_string(const char *str);
void serial_write_hex64(uint64_t value);

#endif
```

Create `kernel/serial.c`:
```c
#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00); // disable interrupts
    outb(COM1 + 3, 0x80); // enable DLAB
    outb(COM1 + 0, 0x03); // divisor low byte: 38400 baud
    outb(COM1 + 1, 0x00); // divisor high byte
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7); // enable FIFO, clear it, 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs disabled, RTS/DSR set
}

static int transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!transmit_empty()) { }
    outb(COM1, (uint8_t)c);
}

void serial_write_string(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') {
            serial_putc('\r');
        }
        serial_putc(str[i]);
    }
}

void serial_write_hex64(uint64_t value) {
    static const char hex_digits[] = "0123456789abcdef";
    serial_write_string("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_putc(hex_digits[(value >> shift) & 0xF]);
    }
}
```

- [ ] **Step 3: Extract VGA into its own module and add `vga_clear`/`vga_putc`**

Create `kernel/vga.h`:
```c
#ifndef NEOOS_VGA_H
#define NEOOS_VGA_H

void vga_clear(void);
void vga_print_string(const char *str);
void vga_putc(char c);

#endif
```

Create `kernel/vga.c`:
```c
#include "vga.h"

static volatile unsigned short *const VGA_BUFFER = (unsigned short *)0xb8000;
static const unsigned short VGA_COLOR_WHITE_ON_BLACK = 0x0f;
#define VGA_CELL_COUNT 2000
#define VGA_COLUMNS 80

static int vga_cursor = 0;

void vga_clear(void) {
    for (int i = 0; i < VGA_CELL_COUNT; i++) {
        VGA_BUFFER[i] = (unsigned short)(' ') | (unsigned short)(VGA_COLOR_WHITE_ON_BLACK << 8);
    }
    vga_cursor = 0;
}

void vga_print_string(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        VGA_BUFFER[i] = (unsigned short)((unsigned char)str[i]) |
                        (unsigned short)(VGA_COLOR_WHITE_ON_BLACK << 8);
    }
}

void vga_putc(char c) {
    if (c == '\n') {
        vga_cursor += VGA_COLUMNS - (vga_cursor % VGA_COLUMNS);
    } else {
        VGA_BUFFER[vga_cursor] = (unsigned short)((unsigned char)c) |
                                 (unsigned short)(VGA_COLOR_WHITE_ON_BLACK << 8);
        vga_cursor++;
    }
    if (vga_cursor >= VGA_CELL_COUNT) {
        vga_cursor = 0;
    }
}
```

- [ ] **Step 4: Update `kernel.h`/`kernel.c` to use the new modules**

Replace `kernel/kernel.h`:
```c
#ifndef NEOOS_KERNEL_H
#define NEOOS_KERNEL_H

void kmain(void *multiboot_info);

#endif
```

Replace `kernel/kernel.c`:
```c
#include "kernel.h"
#include "vga.h"
#include "serial.h"

void kmain(void *multiboot_info) {
    (void)multiboot_info;

    serial_init();
    serial_write_string("NeoOS booting (milestone 2: interrupts)\n");

    vga_clear();
    vga_print_string("NeoOS booted");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
```

- [ ] **Step 5: Rewrite the Makefile to auto-discover kernel `.c` files**

Replace `Makefile`:
```makefile
CC := $(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-gcc
AS := nasm

CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -std=gnu11 -O2
ASFLAGS := -f elf64

BUILD_DIR := build
ISO_DIR := iso

C_SOURCES := $(wildcard kernel/*.c)
C_OBJECTS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD_DIR)/boot.o

.PHONY: all build iso run clean

all: build

build: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR)/boot.o: boot/boot.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) boot/boot.asm -o $(BUILD_DIR)/boot.o

$(BUILD_DIR)/%.o: kernel/%.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.elf: $(ASM_OBJECTS) $(C_OBJECTS) linker.ld
	$(CC) -T linker.ld -o $(BUILD_DIR)/kernel.elf -ffreestanding -O2 -nostdlib $(ASM_OBJECTS) $(C_OBJECTS) -lgcc

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

- [ ] **Step 6: Build and verify serial + VGA output**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
grep "NeoOS booting" /tmp/neoos-serial.log
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: `grep` finds the line `NeoOS booting (milestone 2: interrupts)`; the screenshot shows "NeoOS booted" in white-on-black near the top-left on an otherwise blank (cleared) screen.

- [ ] **Step 7: Commit**

```bash
git add kernel/io.h kernel/serial.c kernel/serial.h kernel/vga.c kernel/vga.h kernel/kernel.c kernel/kernel.h Makefile
git commit -m "Add serial driver, VGA refactor, and scalable Makefile"
```

---

### Task 2: GDT Extension with TSS/IST

**Files:**
- Create: `kernel/tss.c`, `kernel/tss.h`
- Create: `kernel/gdt.c`, `kernel/gdt.h`
- Create: `kernel/gdt_flush.asm`
- Modify: `kernel/kernel.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: nothing from Task 1 directly (independent subsystem), but `kernel.c`'s init sequence is extended.
- Produces: `GDT_KERNEL_CODE_SELECTOR` (`0x08`), `GDT_KERNEL_DATA_SELECTOR` (`0x10`), `GDT_TSS_SELECTOR` (`0x18`) in `gdt.h` — Task 3's `idt.c` uses `GDT_KERNEL_CODE_SELECTOR`. `extern struct tss_entry tss;` in `tss.h` with a populated `ist1` field — Task 3's IDT double-fault gate (vector 8) relies on IST index 1 pointing at this stack.

- [ ] **Step 1: Write the TSS**

Create `kernel/tss.h`:
```c
#ifndef NEOOS_TSS_H
#define NEOOS_TSS_H

#include <stdint.h>

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern struct tss_entry tss;

void tss_init(void);

#endif
```

Create `kernel/tss.c`:
```c
#include "tss.h"

#define IST1_STACK_SIZE 4096

struct tss_entry tss;
static unsigned char ist1_stack[IST1_STACK_SIZE] __attribute__((aligned(16)));

void tss_init(void) {
    unsigned char *raw = (unsigned char *)&tss;
    for (unsigned int i = 0; i < sizeof(tss); i++) {
        raw[i] = 0;
    }
    tss.ist1 = (uint64_t)(ist1_stack + IST1_STACK_SIZE);
    tss.iomap_base = sizeof(struct tss_entry);
}
```

- [ ] **Step 2: Write the CS-reload/TSS-load assembly helper**

Create `kernel/gdt_flush.asm`:
```nasm
; kernel/gdt_flush.asm — loads a new GDTR, reloads the data segment
; registers, reloads CS via a far return (required in 64-bit mode —
; CS cannot be loaded with a plain mov), then loads the TSS selector.

section .text
[bits 64]
global gdt_flush

; void gdt_flush(uint64_t gdtr_ptr, uint16_t data_selector,
;                uint16_t code_selector, uint16_t tss_selector)
; System V AMD64: rdi=gdtr_ptr, rsi=data_selector, rdx=code_selector, rcx=tss_selector
gdt_flush:
    lgdt [rdi]

    mov ax, si
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push rdx
    lea rax, [rel .reload_cs]
    push rax
    retfq
.reload_cs:
    ltr cx
    ret
```

- [ ] **Step 3: Write the GDT**

Create `kernel/gdt.h`:
```c
#ifndef NEOOS_GDT_H
#define NEOOS_GDT_H

#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10
#define GDT_TSS_SELECTOR         0x18

void gdt_init(void);

#endif
```

Create `kernel/gdt.c`:
```c
#include <stdint.h>
#include "gdt.h"
#include "tss.h"

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t gdt_entries[5];

extern void gdt_flush(uint64_t gdtr_ptr, uint16_t data_selector,
                       uint16_t code_selector, uint16_t tss_selector);

static void set_tss_descriptor(uint64_t base, uint32_t limit) {
    uint64_t low = limit & 0xFFFF;
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40;              // present, DPL0, type=0x9 (64-bit TSS, available)
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFF) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFF;

    gdt_entries[3] = low;
    gdt_entries[4] = high;
}

void gdt_init(void) {
    gdt_entries[0] = 0;                                                   // null
    gdt_entries[1] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (1ULL << 53); // code
    gdt_entries[2] = (1ULL << 41) | (1ULL << 44) | (1ULL << 47);                // data
    set_tss_descriptor((uint64_t)&tss, sizeof(struct tss_entry) - 1);

    struct gdtr gdtr = {
        .limit = sizeof(gdt_entries) - 1,
        .base = (uint64_t)&gdt_entries,
    };

    gdt_flush((uint64_t)&gdtr, GDT_KERNEL_DATA_SELECTOR,
              GDT_KERNEL_CODE_SELECTOR, GDT_TSS_SELECTOR);
}
```

- [ ] **Step 4: Wire into `kmain` and update the Makefile**

Edit `kernel/kernel.c` — add includes and calls between the VGA banner and the idle loop:
```c
#include "kernel.h"
#include "vga.h"
#include "serial.h"
#include "tss.h"
#include "gdt.h"

void kmain(void *multiboot_info) {
    (void)multiboot_info;

    serial_init();
    serial_write_string("NeoOS booting (milestone 2: interrupts)\n");

    vga_clear();
    vga_print_string("NeoOS booted");

    tss_init();
    gdt_init();
    serial_write_string("[gdt] loaded, tss_selector=0x18\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
```

Edit `Makefile` — add `gdt_flush.o` to `ASM_OBJECTS` and its build rule:
```makefile
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o
```
Add this rule alongside the existing `$(BUILD_DIR)/boot.o` rule:
```makefile
$(BUILD_DIR)/gdt_flush.o: kernel/gdt_flush.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/gdt_flush.asm -o $(BUILD_DIR)/gdt_flush.o
```

- [ ] **Step 5: Build and verify the GDT/TSS reload doesn't crash**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
grep "\[gdt\] loaded" /tmp/neoos-serial.log
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: `grep` finds `[gdt] loaded, tss_selector=0x18` (proving execution continued past the far-return CS reload and `ltr` without a fault); the screenshot still shows "NeoOS booted" (a triple fault here would instead show GRUB reloading or a blank/garbled screen from a reboot).

- [ ] **Step 6: Commit**

```bash
git add kernel/tss.c kernel/tss.h kernel/gdt.c kernel/gdt.h kernel/gdt_flush.asm kernel/kernel.c Makefile
git commit -m "Add GDT extension with TSS and IST1 stack"
```

---

### Task 3: IDT, Exception Handling, and the ISR/IRQ Common Stub

**Files:**
- Create: `kernel/isr.asm`
- Create: `kernel/isr.c`, `kernel/isr.h`
- Create: `kernel/idt.c`, `kernel/idt.h`
- Modify: `kernel/kernel.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `GDT_KERNEL_CODE_SELECTOR` from `gdt.h` (Task 2); `serial_write_string`/`serial_write_hex64` from `serial.h` (Task 1); `vga_print_string` from `vga.h` (Task 1).
- Produces: `struct registers` (`kernel/isr.h`) — the uniform register-frame layout Task 8's timer handler and Task 9's keyboard handler do **not** need (they take no arguments), but `isr_handler` dispatches to them by vector number. `idt_init(void)` (`kernel/idt.h`), called once from `kmain` and never again. The vector constants `VECTOR_TIMER` (`0x20`, defined in Task 8's `timer.h`) and `VECTOR_KEYBOARD` (`0x21`, defined in Task 9's `keyboard.h`) are referenced by `isr.c`'s dispatch `switch` — until Tasks 8/9 exist, those two vectors simply fall into the generic "unhandled interrupt" path, which is safe since nothing generates those interrupts yet.

- [ ] **Step 1: Write the ISR/IRQ entry stubs and common trampoline**

Create `kernel/isr.asm`:
```nasm
; kernel/isr.asm — one stub per IDT vector (0-255) plus a shared
; trampoline that builds a uniform register frame and calls into C.
;
; Vectors 8, 10, 11, 12, 13, 14, 17 have a CPU-pushed error code;
; every other vector (including all IRQs, which never have one) gets
; a fake zero pushed so every vector produces the same stack layout.
;
; Because every interrupt in this kernel is taken while already at
; CPL0 (no ring 3 exists yet), the CPU does NOT push SS/RSP on entry
; — only RIP, CS, RFLAGS (and the error code, where applicable).

extern isr_handler

section .text
[bits 64]

%macro ISR_NOERR 1
isr%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr%1:
    push %1
    jmp isr_common_stub
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

%assign i 32
%rep 224
ISR_NOERR i
%assign i i+1
%endrep

isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld
    mov rdi, rsp
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16     ; drop vector_number + error_code
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr%+i
%assign i i+1
%endrep
```

- [ ] **Step 2: Write the register frame and C-level dispatcher**

Create `kernel/isr.h`:
```c
#ifndef NEOOS_ISR_H
#define NEOOS_ISR_H

#include <stdint.h>

// Layout mirrors isr.asm's push order exactly (low to high address).
// No rsp/ss fields: every interrupt is taken at CPL0, so the CPU
// never pushes them (see Global Constraints in the plan).
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector_number;
    uint64_t error_code;
    uint64_t rip, cs, rflags;
} __attribute__((packed));

void isr_handler(struct registers *regs);

#endif
```

Create `kernel/isr.c`:
```c
#include "isr.h"
#include "serial.h"
#include "vga.h"

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 Floating-Point Exception", "Alignment Check",
    "Machine Check", "SIMD Floating-Point Exception", "Virtualization Exception",
    "Control Protection Exception", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Hypervisor Injection Exception",
    "VMM Communication Exception", "Security Exception", "Reserved",
};

static void exception_dump_and_halt(struct registers *regs) {
    serial_write_string("\n[exception] ");
    serial_write_string(exception_names[regs->vector_number]);
    serial_write_string(" (vector=");
    serial_write_hex64(regs->vector_number);
    serial_write_string(", error_code=");
    serial_write_hex64(regs->error_code);
    serial_write_string(")\n  rip="); serial_write_hex64(regs->rip);
    serial_write_string(" cs=");      serial_write_hex64(regs->cs);
    serial_write_string(" rflags=");  serial_write_hex64(regs->rflags);
    serial_write_string("\n  rax="); serial_write_hex64(regs->rax);
    serial_write_string(" rbx=");    serial_write_hex64(regs->rbx);
    serial_write_string(" rcx=");    serial_write_hex64(regs->rcx);
    serial_write_string(" rdx=");    serial_write_hex64(regs->rdx);
    serial_write_string("\n  rsi="); serial_write_hex64(regs->rsi);
    serial_write_string(" rdi=");    serial_write_hex64(regs->rdi);
    serial_write_string(" rbp=");    serial_write_hex64(regs->rbp);

    if (regs->vector_number == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        serial_write_string("\n  cr2="); serial_write_hex64(cr2);
    }
    serial_write_string("\n");

    vga_print_string("EXCEPTION - HALTED");

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void unhandled_interrupt(uint64_t vector) {
    serial_write_string("[isr] unhandled interrupt vector=");
    serial_write_hex64(vector);
    serial_write_string("\n");

    vga_print_string("UNHANDLED IRQ - HALTED");

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void isr_handler(struct registers *regs) {
    if (regs->vector_number < 32) {
        exception_dump_and_halt(regs);
        return;
    }

    unhandled_interrupt(regs->vector_number);
}
```

- [ ] **Step 3: Write the IDT**

Create `kernel/idt.h`:
```c
#ifndef NEOOS_IDT_H
#define NEOOS_IDT_H

void idt_init(void);

#endif
```

Create `kernel/idt.c`:
```c
#include <stdint.h>
#include "idt.h"
#include "gdt.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern uint64_t isr_stub_table[256];

static struct idt_entry idt_entries[256];

static void idt_set_gate(int vector, uint64_t handler, uint16_t selector, uint8_t ist, uint8_t type_attr) {
    idt_entries[vector].offset_low = handler & 0xFFFF;
    idt_entries[vector].selector = selector;
    idt_entries[vector].ist = ist & 0x7;
    idt_entries[vector].type_attr = type_attr;
    idt_entries[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt_entries[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt_entries[vector].zero = 0;
}

void idt_init(void) {
    for (int vector = 0; vector < 256; vector++) {
        uint8_t ist = (vector == 8) ? 1 : 0; // double fault runs on its own IST stack
        idt_set_gate(vector, isr_stub_table[vector], GDT_KERNEL_CODE_SELECTOR, ist, 0x8E);
    }

    struct idt_ptr idtr = {
        .limit = sizeof(idt_entries) - 1,
        .base = (uint64_t)&idt_entries,
    };

    __asm__ volatile ("lidt %0" :: "m"(idtr));
}
```

- [ ] **Step 4: Wire into `kmain` and update the Makefile**

Edit `kernel/kernel.c`:
```c
#include "kernel.h"
#include "vga.h"
#include "serial.h"
#include "tss.h"
#include "gdt.h"
#include "idt.h"

void kmain(void *multiboot_info) {
    (void)multiboot_info;

    serial_init();
    serial_write_string("NeoOS booting (milestone 2: interrupts)\n");

    vga_clear();
    vga_print_string("NeoOS booted");

    tss_init();
    gdt_init();
    serial_write_string("[gdt] loaded, tss_selector=0x18\n");

    idt_init();
    serial_write_string("[idt] loaded\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
```

Edit `Makefile` — add the ISR stub object. **Name it `isr_stubs.o`, not `isr.o`**: `kernel/isr.c` (Step 2) is picked up by the wildcard C rule and would also produce `build/isr.o`, colliding with the assembly object of the same name (caught during execution — `ld` reported "multiple definition of isr_stub_table").
```makefile
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/isr_stubs.o
```
```makefile
$(BUILD_DIR)/isr_stubs.o: kernel/isr.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/isr.asm -o $(BUILD_DIR)/isr_stubs.o
```

- [ ] **Step 5: Build and verify normal boot still works**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
grep "\[idt\] loaded" /tmp/neoos-serial.log
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: `grep` finds `[idt] loaded`; screenshot still shows "NeoOS booted".

- [ ] **Step 6: Temporarily force a real divide-by-zero and verify the exception dump**

Edit `kernel/kernel.c` — add this block immediately after the `idt_init();`/`serial_write_string("[idt] loaded\n");` lines:
```c
    __asm__ volatile (
        "xor %%edx, %%edx\n\t"
        "mov $1, %%eax\n\t"
        "xor %%ecx, %%ecx\n\t"
        "div %%ecx\n\t"
        ::: "eax", "ecx", "edx"
    );
```
Run:
```bash
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
grep "Divide Error" /tmp/neoos-serial.log
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: `grep` finds a line like `[exception] Divide Error (vector=0x0, error_code=0x0)` followed by register values; the screenshot shows "EXCEPTION - HALTED"; QEMU did not exit on its own before the `quit` command was sent (i.e., it halted deterministically, not a reboot loop).

- [ ] **Step 7: Remove the forced fault and verify normal boot resumes**

Remove the inline-asm block added in Step 6 from `kernel/kernel.c` so it reads exactly as in Step 4.
Run:
```bash
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
grep "\[idt\] loaded" /tmp/neoos-serial.log
```
Expected: same as Step 5 (normal boot, no exception dump).

- [ ] **Step 8: Commit**

```bash
git add kernel/isr.asm kernel/isr.c kernel/isr.h kernel/idt.c kernel/idt.h kernel/kernel.c Makefile
git commit -m "Add IDT, 32-vector exception handling, and ISR/IRQ common stub"
```

---

### Task 4: ACPI RSDP/RSDT/XSDT/MADT Parsing

**Files:**
- Create: `kernel/acpi.c`, `kernel/acpi.h`
- Modify: `kernel/kernel.c`

**Interfaces:**
- Consumes: `serial_write_string`/`serial_write_hex64` (Task 1).
- Produces: `struct acpi_info` and `void acpi_find_madt(struct acpi_info *info)` (`kernel/acpi.h`) — Task 5 uses nothing from it, Task 6 uses `info.lapic_address`, Task 7 uses `info.ioapic_address`/`info.ioapic_gsi_base`, Task 9 uses `info.irq1_gsi`/`info.irq1_polarity`/`info.irq1_trigger`.

- [ ] **Step 1: Write ACPI table parsing**

Create `kernel/acpi.h`:
```c
#ifndef NEOOS_ACPI_H
#define NEOOS_ACPI_H

#include <stdint.h>

struct acpi_info {
    uint32_t lapic_address;
    uint32_t ioapic_address;
    uint32_t ioapic_gsi_base;
    uint8_t  irq0_gsi;
    uint8_t  irq0_polarity; // 0 = active-high, 1 = active-low
    uint8_t  irq0_trigger;  // 0 = edge, 1 = level
    uint8_t  irq1_gsi;
    uint8_t  irq1_polarity;
    uint8_t  irq1_trigger;
};

void acpi_find_madt(struct acpi_info *info);

#endif
```

Create `kernel/acpi.c`:
```c
#include "acpi.h"
#include "serial.h"

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
} __attribute__((packed));

struct madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct madt_ioapic {
    struct madt_entry_header header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso {
    struct madt_entry_header header;
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

static uint8_t sum_bytes(const void *ptr, uint32_t length) {
    uint8_t sum = 0;
    const uint8_t *bytes = (const uint8_t *)ptr;
    for (uint32_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum;
}

static int is_rsdp_signature(const struct acpi_rsdp *candidate) {
    static const char signature[8] = { 'R', 'S', 'D', ' ', 'P', 'T', 'R', ' ' };
    for (int i = 0; i < 8; i++) {
        if (candidate->signature[i] != signature[i]) {
            return 0;
        }
    }
    return 1;
}

static struct acpi_rsdp *find_rsdp_in_range(uint32_t start, uint32_t end) {
    for (uint32_t addr = start; addr < end; addr += 16) {
        struct acpi_rsdp *candidate = (struct acpi_rsdp *)(uintptr_t)addr;
        if (is_rsdp_signature(candidate) && sum_bytes(candidate, 20) == 0) {
            return candidate;
        }
    }
    return 0;
}

static struct acpi_rsdp *find_rsdp(void) {
    uint16_t ebda_segment = *(volatile uint16_t *)(uintptr_t)0x40E;
    uint32_t ebda_addr = (uint32_t)ebda_segment << 4;

    struct acpi_rsdp *rsdp = find_rsdp_in_range(ebda_addr, ebda_addr + 1024);
    if (rsdp) {
        return rsdp;
    }
    return find_rsdp_in_range(0xE0000, 0x100000);
}

static int is_apic_signature(const struct acpi_sdt_header *header) {
    return header->signature[0] == 'A' && header->signature[1] == 'P' &&
           header->signature[2] == 'I' && header->signature[3] == 'C';
}

static struct acpi_madt *find_madt_via_xsdt(uint64_t xsdt_address) {
    struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)(uintptr_t)xsdt_address;
    uint64_t *entries = (uint64_t *)((uint8_t *)xsdt + sizeof(struct acpi_sdt_header));
    uint32_t entry_count = (xsdt->length - sizeof(struct acpi_sdt_header)) / sizeof(uint64_t);

    for (uint32_t i = 0; i < entry_count; i++) {
        struct acpi_sdt_header *table = (struct acpi_sdt_header *)(uintptr_t)entries[i];
        if (is_apic_signature(table)) {
            return (struct acpi_madt *)table;
        }
    }
    return 0;
}

static struct acpi_madt *find_madt_via_rsdt(uint32_t rsdt_address) {
    struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)(uintptr_t)rsdt_address;
    uint32_t *entries = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_sdt_header));
    uint32_t entry_count = (rsdt->length - sizeof(struct acpi_sdt_header)) / sizeof(uint32_t);

    for (uint32_t i = 0; i < entry_count; i++) {
        struct acpi_sdt_header *table = (struct acpi_sdt_header *)(uintptr_t)entries[i];
        if (is_apic_signature(table)) {
            return (struct acpi_madt *)table;
        }
    }
    return 0;
}

static void parse_madt(struct acpi_madt *madt, struct acpi_info *info) {
    info->lapic_address = madt->local_apic_address;
    info->irq0_gsi = 0;
    info->irq0_polarity = 0;
    info->irq0_trigger = 0;
    info->irq1_gsi = 1;
    info->irq1_polarity = 0;
    info->irq1_trigger = 0;

    uint8_t *ptr = (uint8_t *)madt + sizeof(struct acpi_madt);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        struct madt_entry_header *entry = (struct madt_entry_header *)ptr;

        if (entry->type == 1) {
            struct madt_ioapic *ioapic = (struct madt_ioapic *)ptr;
            info->ioapic_address = ioapic->ioapic_address;
            info->ioapic_gsi_base = ioapic->gsi_base;
        } else if (entry->type == 2) {
            struct madt_iso *iso = (struct madt_iso *)ptr;
            uint8_t polarity_bits = iso->flags & 0x3;
            uint8_t trigger_bits = (iso->flags >> 2) & 0x3;
            uint8_t polarity = (polarity_bits == 3) ? 1 : 0;
            uint8_t trigger = (trigger_bits == 3) ? 1 : 0;

            if (iso->irq_source == 0) {
                info->irq0_gsi = (uint8_t)iso->gsi;
                info->irq0_polarity = polarity;
                info->irq0_trigger = trigger;
            } else if (iso->irq_source == 1) {
                info->irq1_gsi = (uint8_t)iso->gsi;
                info->irq1_polarity = polarity;
                info->irq1_trigger = trigger;
            }
        }

        ptr += entry->length;
    }
}

void acpi_find_madt(struct acpi_info *info) {
    struct acpi_rsdp *rsdp = find_rsdp();

    struct acpi_madt *madt;
    if (rsdp->revision >= 2) {
        madt = find_madt_via_xsdt(rsdp->xsdt_address);
    } else {
        madt = find_madt_via_rsdt(rsdp->rsdt_address);
    }

    parse_madt(madt, info);

    serial_write_string("[acpi] lapic="); serial_write_hex64(info->lapic_address);
    serial_write_string(" ioapic="); serial_write_hex64(info->ioapic_address);
    serial_write_string(" ioapic_gsi_base="); serial_write_hex64(info->ioapic_gsi_base);
    serial_write_string("\n[acpi] irq0_gsi="); serial_write_hex64(info->irq0_gsi);
    serial_write_string(" irq1_gsi="); serial_write_hex64(info->irq1_gsi);
    serial_write_string("\n");
}
```

- [ ] **Step 2: Wire into `kmain`**

Edit `kernel/kernel.c` — add the include and call after `[idt] loaded`:
```c
#include "acpi.h"
```
```c
    idt_init();
    serial_write_string("[idt] loaded\n");

    struct acpi_info acpi;
    acpi_find_madt(&acpi);
```

- [ ] **Step 3: Build and verify parsed ACPI values**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'quit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
grep "\[acpi\]" /tmp/neoos-serial.log
```
Expected: two `[acpi]` lines. QEMU's default MADT places the Local APIC at `0xfee00000` and the IOAPIC at `0xfec00000` with `ioapic_gsi_base=0x0` — confirm those two addresses appear. `irq0_gsi`/`irq1_gsi` values are QEMU-version-dependent (QEMU's default MADT commonly overrides IRQ0 to GSI `0x2`); if either looks implausible (e.g., not a small integer), that indicates an MADT entry-walk bug worth investigating before moving on — Tasks 7 and 9 depend on these values being correct.

- [ ] **Step 4: Commit**

```bash
git add kernel/acpi.c kernel/acpi.h kernel/kernel.c
git commit -m "Add ACPI RSDP/RSDT/XSDT/MADT parsing"
```

---

### Task 5: Legacy PIC Disable

**Files:**
- Create: `kernel/pic.c`, `kernel/pic.h`
- Modify: `kernel/kernel.c`

**Interfaces:**
- Consumes: `outb` (`kernel/io.h`, Task 1).
- Produces: `void pic_disable(void)` (`kernel/pic.h`) — called once from `kmain`, never again; no later task depends on any symbol from this one beyond the fact that it ran.

- [ ] **Step 1: Write the PIC remap-then-mask routine**

Create `kernel/pic.h`:
```c
#ifndef NEOOS_PIC_H
#define NEOOS_PIC_H

void pic_disable(void);

#endif
```

Create `kernel/pic.c`:
```c
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_disable(void) {
    outb(PIC1_CMD, 0x11); // begin init sequence, cascade mode
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, 0x20); // remap IRQ0-7 to vectors 0x20-0x27 (defensive: off the exception range)
    outb(PIC2_DATA, 0x28); // remap IRQ8-15 to vectors 0x28-0x2F
    outb(PIC1_DATA, 0x04); // PIC1 has a slave on IRQ2
    outb(PIC2_DATA, 0x02); // PIC2's cascade identity
    outb(PIC1_DATA, 0x01); // 8086 mode
    outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, 0xFF); // mask every line
    outb(PIC2_DATA, 0xFF);
}
```

- [ ] **Step 2: Wire into `kmain`**

Edit `kernel/kernel.c`:
```c
#include "pic.h"
```
```c
    struct acpi_info acpi;
    acpi_find_madt(&acpi);

    pic_disable();
    serial_write_string("[pic] disabled\n");
```

- [ ] **Step 3: Build and verify**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'quit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
grep "\[pic\] disabled" /tmp/neoos-serial.log
```
Expected: the line is present, proving the remap/mask I/O sequence completed without a fault.

- [ ] **Step 4: Commit**

```bash
git add kernel/pic.c kernel/pic.h kernel/kernel.c
git commit -m "Add legacy PIC remap-then-mask disable"
```

---

### Task 6: Local APIC Enable

**Files:**
- Create: `kernel/lapic.c`, `kernel/lapic.h`
- Modify: `kernel/kernel.c`

**Interfaces:**
- Consumes: `acpi.lapic_address` (Task 4).
- Produces: `lapic_init(uint32_t address)`, `lapic_send_eoi(void)`, `lapic_get_id(void)`, `lapic_timer_start_oneshot_max(void)`, `lapic_timer_stop_and_read(void)`, `lapic_timer_start_periodic(uint32_t initial_count, uint8_t vector)` (`kernel/lapic.h`). Task 8's `pit.c` uses the oneshot/stop-and-read pair for calibration and `timer.c` uses `lapic_timer_start_periodic`. Task 9's keyboard routing uses `lapic_get_id()` as the IOAPIC redirection destination. Task 3's `isr.c` dispatcher (extended in Tasks 8/9) uses `lapic_send_eoi()`.

- [ ] **Step 1: Write the Local APIC driver**

Create `kernel/lapic.h`:
```c
#ifndef NEOOS_LAPIC_H
#define NEOOS_LAPIC_H

#include <stdint.h>

void lapic_init(uint32_t address);
void lapic_send_eoi(void);
uint32_t lapic_get_id(void);
void lapic_timer_start_oneshot_max(void);
uint32_t lapic_timer_stop_and_read(void);
void lapic_timer_start_periodic(uint32_t initial_count, uint8_t vector);

#endif
```

Create `kernel/lapic.c`:
```c
#include "lapic.h"

#define LAPIC_REG_ID         0x020
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_SVR        0x0F0
#define LAPIC_REG_LVT_TIMER  0x320
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

#define LVT_MASKED         (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)

static volatile uint32_t *lapic_base;

static uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t value) {
    lapic_base[reg / 4] = value;
}

void lapic_init(uint32_t address) {
    lapic_base = (volatile uint32_t *)(uintptr_t)address;
    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | 0x100 | 0xFF); // software-enable, spurious vector 0xFF
}

void lapic_send_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_timer_start_oneshot_max(void) {
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3); // divide by 16
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);
}

uint32_t lapic_timer_stop_and_read(void) {
    uint32_t current = lapic_read(LAPIC_REG_TIMER_CUR);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
    return current;
}

void lapic_timer_start_periodic(uint32_t initial_count, uint8_t vector) {
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3); // divide by 16
    lapic_write(LAPIC_REG_LVT_TIMER, (uint32_t)vector | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_INIT, initial_count);
}
```

- [ ] **Step 2: Wire into `kmain`**

Edit `kernel/kernel.c`:
```c
#include "lapic.h"
```
```c
    pic_disable();
    serial_write_string("[pic] disabled\n");

    lapic_init(acpi.lapic_address);
    serial_write_string("[lapic] enabled, id="); serial_write_hex64(lapic_get_id());
    serial_write_string("\n");
```

- [ ] **Step 3: Build and verify**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'quit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
grep "\[lapic\] enabled" /tmp/neoos-serial.log
```
Expected: `[lapic] enabled, id=0x0` (QEMU's single boot CPU has APIC ID 0 by default).

- [ ] **Step 4: Commit**

```bash
git add kernel/lapic.c kernel/lapic.h kernel/kernel.c
git commit -m "Add Local APIC enable, EOI, and timer register helpers"
```

---

### Task 7: IOAPIC Init

**Files:**
- Create: `kernel/ioapic.c`, `kernel/ioapic.h`
- Modify: `kernel/kernel.c`

**Interfaces:**
- Consumes: `acpi.ioapic_address` (Task 4).
- Produces: `ioapic_init(uint32_t address)`, `ioapic_set_redirection(uint8_t pin, uint8_t vector, uint8_t polarity, uint8_t trigger, uint8_t dest_apic_id)` (`kernel/ioapic.h`). Task 9 calls `ioapic_set_redirection` for the keyboard IRQ; `pin` there is the GSI **relative to the IOAPIC's own GSI base** (`acpi.irq1_gsi - acpi.ioapic_gsi_base`), not the raw GSI.

- [ ] **Step 1: Write the IOAPIC driver**

Create `kernel/ioapic.h`:
```c
#ifndef NEOOS_IOAPIC_H
#define NEOOS_IOAPIC_H

#include <stdint.h>

void ioapic_init(uint32_t address);
void ioapic_set_redirection(uint8_t pin, uint8_t vector, uint8_t polarity, uint8_t trigger, uint8_t dest_apic_id);

#endif
```

Create `kernel/ioapic.c`:
```c
#include "ioapic.h"

#define IOAPIC_REGSEL 0
#define IOAPIC_REGWIN 4 // 32-bit-word index of the IOWIN register (byte offset 0x10)

static volatile uint32_t *ioapic_base;

static uint32_t ioapic_read(uint8_t reg) {
    ioapic_base[IOAPIC_REGSEL] = reg;
    return ioapic_base[IOAPIC_REGWIN];
}

static void ioapic_write(uint8_t reg, uint32_t value) {
    ioapic_base[IOAPIC_REGSEL] = reg;
    ioapic_base[IOAPIC_REGWIN] = value;
}

void ioapic_init(uint32_t address) {
    ioapic_base = (volatile uint32_t *)(uintptr_t)address;
    (void)ioapic_read(0x00); // touch IOAPICID to confirm the MMIO mapping is live
}

void ioapic_set_redirection(uint8_t pin, uint8_t vector, uint8_t polarity, uint8_t trigger, uint8_t dest_apic_id) {
    uint32_t low = vector;
    if (polarity) {
        low |= (1u << 13); // active-low
    }
    if (trigger) {
        low |= (1u << 15); // level-triggered
    }

    uint8_t reg = 0x10 + pin * 2;
    ioapic_write(reg, low);
    ioapic_write(reg + 1, (uint32_t)dest_apic_id << 24);
}
```

- [ ] **Step 2: Wire into `kmain`**

Edit `kernel/kernel.c`:
```c
#include "ioapic.h"
```
```c
    lapic_init(acpi.lapic_address);
    serial_write_string("[lapic] enabled, id="); serial_write_hex64(lapic_get_id());
    serial_write_string("\n");

    ioapic_init(acpi.ioapic_address);
    serial_write_string("[ioapic] initialized\n");
```

- [ ] **Step 3: Build and verify**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'quit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
grep "\[ioapic\] initialized" /tmp/neoos-serial.log
```
Expected: the line is present (proving the IOAPIC MMIO read succeeded without a fault). No redirection entries are programmed yet — that happens in Task 9.

- [ ] **Step 4: Commit**

```bash
git add kernel/ioapic.c kernel/ioapic.h kernel/kernel.c
git commit -m "Add IOAPIC MMIO init and redirection-table helper"
```

---

### Task 8: PIT Calibration and the Periodic LAPIC Timer

**Files:**
- Create: `kernel/pit.c`, `kernel/pit.h`
- Create: `kernel/timer.c`, `kernel/timer.h`
- Modify: `kernel/kernel.c`

**Interfaces:**
- Consumes: `outb`/`inb` (Task 1); `lapic_timer_start_oneshot_max`/`lapic_timer_stop_and_read`/`lapic_timer_start_periodic` (Task 6).
- Produces: `uint32_t pit_calibrate_lapic_ticks_per_10ms(void)` (`kernel/pit.h`), used only by `timer.c`. `VECTOR_TIMER` (`0x20`, `kernel/timer.h`) and `void timer_handler(void)` — Task 3's `isr.c` dispatcher is extended in this task to call it. `void timer_init(void)`, called once from `kmain`.

**Note on this task's verification:** `sti` is not added until Task 10 (the spec's own data-flow order puts it last), so the periodic timer is armed here but interrupts are still globally disabled (`IF=0`) — no tick log lines will appear yet. This task's verification only confirms calibration completes with a plausible value; Task 10 verifies real ticking.

- [ ] **Step 1: Write the PIT one-shot calibration stopwatch**

Create `kernel/pit.h`:
```c
#ifndef NEOOS_PIT_H
#define NEOOS_PIT_H

#include <stdint.h>

uint32_t pit_calibrate_lapic_ticks_per_10ms(void);

#endif
```

Create `kernel/pit.c`:
```c
#include "pit.h"
#include "io.h"
#include "lapic.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_FREQUENCY     1193182

// One-shot stopwatch: program PIT channel 0 for a known ~10ms interval,
// start the LAPIC timer counting down from its max, and measure how far
// it counted down by the time the PIT interval elapses. The PIT is
// never touched again after this function returns.
uint32_t pit_calibrate_lapic_ticks_per_10ms(void) {
    uint16_t divisor = (uint16_t)(PIT_FREQUENCY / 100); // ~10ms

    outb(PIT_COMMAND, 0x30); // channel 0, lobyte/hibyte access, mode 0, binary
    outb(PIT_CHANNEL0_DATA, divisor & 0xFF);
    outb(PIT_CHANNEL0_DATA, (divisor >> 8) & 0xFF);

    lapic_timer_start_oneshot_max();

    uint8_t status;
    do {
        outb(PIT_COMMAND, 0xE2); // read-back command: latch status for channel 0
        status = inb(PIT_CHANNEL0_DATA);
    } while (!(status & 0x80)); // bit 7 = OUT pin; goes high at terminal count (mode 0)

    uint32_t remaining = lapic_timer_stop_and_read();
    return 0xFFFFFFFF - remaining;
}
```

- [ ] **Step 2: Write the timer subsystem**

Create `kernel/timer.h`:
```c
#ifndef NEOOS_TIMER_H
#define NEOOS_TIMER_H

#define VECTOR_TIMER 0x20

void timer_init(void);
void timer_handler(void);

#endif
```

Create `kernel/timer.c`:
```c
#include "timer.h"
#include "pit.h"
#include "lapic.h"
#include "serial.h"

#define TICKS_PER_LOG 100 // 100Hz timer -> log once per second

static volatile uint64_t tick_count = 0;

void timer_handler(void) {
    tick_count++;
    if (tick_count % TICKS_PER_LOG == 0) {
        serial_write_string("[timer] tick=");
        serial_write_hex64(tick_count);
        serial_write_string("\n");
    }
}

void timer_init(void) {
    // Calibrating over exactly 10ms and targeting 100Hz (10ms period)
    // means the calibrated tick count IS the periodic initial count.
    uint32_t ticks_per_10ms = pit_calibrate_lapic_ticks_per_10ms();
    serial_write_string("[timer] calibrated lapic ticks per 10ms=");
    serial_write_hex64(ticks_per_10ms);
    serial_write_string("\n");

    lapic_timer_start_periodic(ticks_per_10ms, VECTOR_TIMER);
}
```

- [ ] **Step 3: Extend the ISR dispatcher to call the timer handler**

Edit `kernel/isr.c` — add the include and extend `isr_handler`:
```c
#include "timer.h"
#include "lapic.h"
```
```c
void isr_handler(struct registers *regs) {
    if (regs->vector_number < 32) {
        exception_dump_and_halt(regs);
        return;
    }

    if (regs->vector_number == VECTOR_TIMER) {
        timer_handler();
        lapic_send_eoi();
        return;
    }

    unhandled_interrupt(regs->vector_number);
}
```

- [ ] **Step 4: Wire `timer_init` into `kmain`**

Edit `kernel/kernel.c`:
```c
#include "timer.h"
```
```c
    ioapic_init(acpi.ioapic_address);
    serial_write_string("[ioapic] initialized\n");

    timer_init();
```

- [ ] **Step 5: Build and verify calibration runs without crashing**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'quit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
grep "\[timer\] calibrated" /tmp/neoos-serial.log
```
Expected: a line like `[timer] calibrated lapic ticks per 10ms=0x...` with a nonzero, plausible value (on typical QEMU/TCG timing, expect roughly `0x2000`-`0x100000`-ish depending on host speed and LAPIC bus frequency emulation — the important checks are: nonzero, and not `0xFFFFFFFF` or `0x0`, either of which would indicate the PIT poll loop or the LAPIC counter read is broken).

- [ ] **Step 6: Commit**

```bash
git add kernel/pit.c kernel/pit.h kernel/timer.c kernel/timer.h kernel/isr.c kernel/kernel.c
git commit -m "Add PIT calibration and periodic LAPIC timer"
```

---

### Task 9: Keyboard Driver and IOAPIC Routing

**Files:**
- Create: `kernel/keyboard.c`, `kernel/keyboard.h`
- Modify: `kernel/isr.c`
- Modify: `kernel/kernel.c`

**Interfaces:**
- Consumes: `inb` (Task 1); `serial_putc`/`serial_write_string` (Task 1); `vga_putc` (Task 1); `ioapic_set_redirection` (Task 7); `acpi.irq1_gsi`/`irq1_polarity`/`irq1_trigger`/`ioapic_gsi_base` (Task 4); `lapic_get_id`/`lapic_send_eoi` (Task 6).
- Produces: `VECTOR_KEYBOARD` (`0x21`, `kernel/keyboard.h`) and `void keyboard_handler(void)` — Task 3's `isr.c` dispatcher is extended again to call it.

- [ ] **Step 1: Write the keyboard driver**

Create `kernel/keyboard.h`:
```c
#ifndef NEOOS_KEYBOARD_H
#define NEOOS_KEYBOARD_H

#define VECTOR_KEYBOARD 0x21

void keyboard_handler(void);

#endif
```

Create `kernel/keyboard.c`:
```c
#include "keyboard.h"
#include "io.h"
#include "serial.h"
#include "vga.h"

#define KEYBOARD_DATA_PORT 0x60

// Scancode Set 1, basic US layout, make codes only (no shift state,
// no extended 0xE0 prefix handling) — entries left at 0 are unmapped.
static const char scancode_to_ascii[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'','`', 0,  '\\','z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0,  '*', 0,   ' ',
};

void keyboard_handler(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (scancode & 0x80) {
        return; // key release, ignore
    }

    char c = scancode_to_ascii[scancode];
    if (c != 0) {
        serial_putc(c);
        vga_putc(c);
    }
}
```

- [ ] **Step 2: Extend the ISR dispatcher**

Edit `kernel/isr.c`:
```c
#include "keyboard.h"
```
```c
    if (regs->vector_number == VECTOR_TIMER) {
        timer_handler();
        lapic_send_eoi();
        return;
    }

    if (regs->vector_number == VECTOR_KEYBOARD) {
        keyboard_handler();
        lapic_send_eoi();
        return;
    }

    unhandled_interrupt(regs->vector_number);
```

- [ ] **Step 3: Route the keyboard IRQ through the IOAPIC using the ACPI-parsed GSI**

Edit `kernel/kernel.c`:
```c
#include "keyboard.h"
```
```c
    timer_init();

    uint8_t keyboard_pin = acpi.irq1_gsi - acpi.ioapic_gsi_base;
    ioapic_set_redirection(keyboard_pin, VECTOR_KEYBOARD, acpi.irq1_polarity,
                            acpi.irq1_trigger, (uint8_t)lapic_get_id());
    serial_write_string("[ioapic] keyboard routed: gsi=");
    serial_write_hex64(acpi.irq1_gsi);
    serial_write_string(" vector=0x21\n");
```

- [ ] **Step 4: Build and verify routing is programmed without crashing**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'quit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
grep "\[ioapic\] keyboard routed" /tmp/neoos-serial.log
```
Expected: the line is present. Interrupts are still globally disabled (`sti` lands in Task 10), so no keypress echo test is possible yet — that happens in Task 10.

- [ ] **Step 5: Commit**

```bash
git add kernel/keyboard.c kernel/keyboard.h kernel/isr.c kernel/kernel.c
git commit -m "Add PS/2 keyboard driver and IOAPIC keyboard routing"
```

---

### Task 10: Final Integration — Enable Interrupts and Verify All Success Criteria

**Files:**
- Modify: `kernel/kernel.c`

**Interfaces:**
- Consumes: everything from Tasks 1-9.
- Produces: the final `kmain`, matching the spec's data-flow order exactly, ending in `sti` and an interruptible `hlt` idle loop. This is the last task — its verification must satisfy every success criterion in the spec simultaneously.

- [ ] **Step 1: Add `sti` and finalize `kmain`**

Replace `kernel/kernel.c` in full:
```c
#include "kernel.h"
#include "vga.h"
#include "serial.h"
#include "tss.h"
#include "gdt.h"
#include "idt.h"
#include "acpi.h"
#include "pic.h"
#include "lapic.h"
#include "ioapic.h"
#include "timer.h"
#include "keyboard.h"

void kmain(void *multiboot_info) {
    (void)multiboot_info;

    serial_init();
    serial_write_string("NeoOS booting (milestone 2: interrupts)\n");

    vga_clear();
    vga_print_string("NeoOS booted");

    tss_init();
    gdt_init();
    serial_write_string("[gdt] loaded, tss_selector=0x18\n");

    idt_init();
    serial_write_string("[idt] loaded\n");

    struct acpi_info acpi;
    acpi_find_madt(&acpi);

    pic_disable();
    serial_write_string("[pic] disabled\n");

    lapic_init(acpi.lapic_address);
    serial_write_string("[lapic] enabled, id="); serial_write_hex64(lapic_get_id());
    serial_write_string("\n");

    ioapic_init(acpi.ioapic_address);
    serial_write_string("[ioapic] initialized\n");

    timer_init();

    uint8_t keyboard_pin = acpi.irq1_gsi - acpi.ioapic_gsi_base;
    ioapic_set_redirection(keyboard_pin, VECTOR_KEYBOARD, acpi.irq1_polarity,
                            acpi.irq1_trigger, (uint8_t)lapic_get_id());
    serial_write_string("[ioapic] keyboard routed: gsi=");
    serial_write_hex64(acpi.irq1_gsi);
    serial_write_string(" vector=0x21\n");

    serial_write_string("NeoOS: interrupts enabled, entering idle loop\n");
    __asm__ volatile ("sti");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
```
(This differs from Task 9's version only by the addition of the trailing `serial_write_string("NeoOS: interrupts enabled...")` and `sti` lines before the idle loop.)

- [ ] **Step 2: Build and verify the timer ticks periodically over several seconds**

Run:
```bash
make clean
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 4
printf 'quit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
grep -c "\[timer\] tick=" /tmp/neoos-serial.log
grep "\[isr\] unhandled" /tmp/neoos-serial.log
echo "exit code of second grep (1 = no unhandled interrupts, correct):" $?
```
Expected: the tick count is roughly 2-3 (one log line per second, ~4 seconds of run time minus boot overhead); the "unhandled interrupts" grep finds nothing (exit code 1) — confirming no spurious PIC/legacy activity and no unexpected vectors firing.

- [ ] **Step 3: Verify keyboard input is echoed to serial and VGA**

Run:
```bash
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'sendkey a\nsendkey b\nsendkey c\n' | nc -U -q1 /tmp/neoos-monitor.sock
sleep 1
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
cat /tmp/neoos-serial.log | tail -c 200
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: the tail of the serial log contains `abc` (the three sent keys, in order, appended after the boot log); the screenshot shows `abc` appended after "NeoOS booted" (via `vga_putc`'s linear cursor).

- [ ] **Step 4: Re-verify the forced divide-by-zero exception path against the fully integrated build**

Edit `kernel/kernel.c` — temporarily add the same forced-fault block as Task 3 Step 6, this time right after the `idt_init();`/`serial_write_string("[idt] loaded\n");` lines:
```c
    __asm__ volatile (
        "xor %%edx, %%edx\n\t"
        "mov $1, %%eax\n\t"
        "xor %%ecx, %%ecx\n\t"
        "div %%ecx\n\t"
        ::: "eax", "ecx", "edx"
    );
```
Run:
```bash
make iso
rm -f /tmp/neoos-monitor.sock /tmp/neoos-serial.log /tmp/neoos-screen.ppm /tmp/neoos-screen.png
qemu-system-x86_64 -cdrom build/neoos.iso -display none \
    -serial file:/tmp/neoos-serial.log \
    -monitor unix:/tmp/neoos-monitor.sock,server,nowait &
QEMU_PID=$!
sleep 2
printf 'screendump /tmp/neoos-screen.ppm\nquit\n' | nc -U -q1 /tmp/neoos-monitor.sock
wait $QEMU_PID 2>/dev/null
python3 -c "from PIL import Image; Image.open('/tmp/neoos-screen.ppm').save('/tmp/neoos-screen.png')"
grep "Divide Error" /tmp/neoos-serial.log
```
View `/tmp/neoos-screen.png` with the Read tool.
Expected: same as Task 3 Step 6 — a full register dump and "EXCEPTION - HALTED" on VGA, with a clean deterministic halt (no reboot).

Remove the forced-fault block again so `kernel/kernel.c` matches Step 1 exactly. Run the Step 2 verification once more to confirm the final committed state is the clean (non-faulting) version.

- [ ] **Step 5: Final interactive sanity check**

Run:
```bash
make run
```
Expected: a QEMU window opens showing "NeoOS booted"; typing a few keys echoes them on screen; the window stays open (not stuck in a reboot loop). Close the window when done, or use the QEMU monitor (Ctrl+Alt+2, then `quit`).

- [ ] **Step 6: Commit**

```bash
git add kernel/kernel.c
git commit -m "Enable interrupts: wire sti and verify all milestone 2 success criteria"
```
