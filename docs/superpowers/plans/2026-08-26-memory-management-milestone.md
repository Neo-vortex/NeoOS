# NeoOS Memory Management Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn NeoOS from a kernel running out of a static, boot-time identity map into one that manages its own memory: a Multiboot2-driven buddy physical frame allocator, a kernel relinked to run in the higher half, a direct physical map plus a general paging API, physmap-relative addressing in the drivers that need it, and a kernel heap (`kmalloc`/`kfree`) structured after mimalloc's segregated size classes.

**Architecture:** `kmain` extends the milestone 2 sequence: serial → `pmm_init`/`pmm_selftest` → `paging_init`/`paging_selftest` → VGA clear → GDT/TSS → IDT → ACPI/MADT → legacy PIC disable → Local APIC/IOAPIC (now physmap-relative) → PIT-calibrated timer → keyboard routing → `heap_init`/`heap_selftest` → `sti` → idle loop. The kernel image is relinked to a fixed higher-half virtual address while `boot/boot.asm`'s 32-bit bootstrap stays low; a small extension to its existing static page tables maps that higher half before the C kernel's first instruction ever executes, so no C code needs to run at two different addresses across the transition.

**Tech Stack:** Same toolchain as milestones 1 and 2 (NASM, `x86_64-elf-gcc` cross-compiler, GNU Make, GRUB, QEMU).

**Spec:** `docs/superpowers/specs/2026-08-26-memory-management-design.md`

## Global Constraints

- Freestanding C (`-ffreestanding -nostdlib`), no libc — same as prior milestones.
- **Address layout, fixed for the whole milestone:**
  - `KERNEL_VIRT_BASE = 0xFFFFFFFF80000000` — where the C kernel (everything except `boot/boot.asm`) is linked. `boot.asm` stays linked low (`. = 1M`, unchanged from milestones 1-2).
  - `PHYSMAP_BASE = 0xFFFF800000000000` (PML4 index 256) — the direct map of the first 4GiB of physical address space (all detected RAM plus the sub-4GiB MMIO hole where the LAPIC/IOAPIC live), installed by `paging_init` in Task 3.
  - PML4 index 511 carries the kernel's own higher-half alias (`p3_table_high`, added to `boot.asm`'s existing static tables in Task 2); PML4 index 0 keeps the original 4GiB low identity map from milestone 2, unchanged.
- **The low 4GiB identity map from milestone 2 is never removed.** The spec's bootstrap sketch describes dropping it once the higher half is live; this plan keeps it permanently instead — it costs no extra memory (Task 2's high alias reuses milestone 2's existing physical PD tables rather than duplicating them) and avoids the fragility of unmapping a range the CPU might still reference at the exact moment of a `CR3` switch. Every milestone-3 success criterion (kernel executing from a higher-half address, a real physmap, migrated drivers, a working heap) is still met — frame-allocator and page-table code below simply take advantage of the permanent low map to treat physical addresses as directly dereferenceable pointers while bootstrapping new page tables, the same way `boot.asm` always has.
- **Physical frame allocator cap:** the buddy allocator's metadata is sized for at most 4GiB of physical RAM (`PMM_MAX_FRAMES`, in `kernel/mm/pmm.c`). QEMU's default RAM (128MiB, no `-m` flag is passed by this project's `make run`) is far under this; RAM reported above 4GiB is silently not tracked. This is a known, acceptable limit for this milestone, not a bug.
- Buddy orders run `0..PMM_MAX_ORDER` (`PMM_MAX_ORDER = 10`, so the largest single block is `4KiB * 2^10 = 4MiB`).
- `Makefile` gains `-Ikernel` in `CFLAGS` (so files under `kernel/mm/` can `#include "serial.h"` etc. without a relative `../` path) and its object-file rule creates whatever subdirectory the target needs (`kernel/mm/*.c` compiles to `build/mm/*.o`).
- **`CFLAGS` also gains `-mcmodel=kernel`** (found necessary in Task 2, not anticipated when this plan was written): once the C kernel links at `KERNEL_VIRT_BASE` (`0xFFFFFFFF80000000`), GCC's default code model — which assumes all symbols fit within a 32-bit signed range near address 0 — produces `R_X86_64_32` relocations that overflow at link time (`relocation truncated to fit`) for every static/string-literal reference. `-mcmodel=kernel` tells GCC the code and data live in the top 2GiB of the address space, matching our link address exactly.
- **`kernel_phys_start` (in `linker.ld`) must bracket the *entire* image, boot block included** (found necessary in Task 2): it's tempting to set it right at the high-half C kernel's start (immediately after the low `.boot.*` block), but `boot.asm`'s own live `p4_table`/`p3_table`/`p3_table_high`/`p2_tables` and boot stack live in that low `.boot.bss` block — if `pmm_init` doesn't exclude them too, the buddy allocator hands out the running page tables' own memory as free RAM, and the first write into it (e.g. `pmm_selftest`'s alloc/free) corrupts the live mapping and triple-faults. Since the low and high blocks are physically contiguous (the high block's `AT()` picks up exactly where the low block ends), a single `kernel_phys_start = .;` placed right after `. = 1M;` (before `.multiboot`) correctly covers both.
- Verification throughout uses headless QEMU exactly as in milestone 2: `-serial file:<path>` for grep-able diagnostics, `screendump` for VGA, `sendkey` for keyboard simulation.

---

### Task 1: Multiboot2-Driven Physical Frame Allocator (Buddy)

**Files:**
- Create: `kernel/mm/pmm.h`
- Create: `kernel/mm/pmm.c`
- Modify: `Makefile` (build `kernel/mm/*.c`, add `-Ikernel`, fix the object rule's `mkdir`)
- Modify: `linker.ld` (add `kernel_phys_start`/`kernel_phys_end` symbols bracketing the kernel image)
- Modify: `kernel/kernel.c` (wire in `pmm_init`/`pmm_selftest`)

**Interfaces:**
- Produces: `void pmm_init(void *multiboot_info)`, `void pmm_selftest(void)`, `uint64_t pmm_alloc(unsigned order)` (returns a physical address or `0` on OOM), `void pmm_free(uint64_t phys_addr, unsigned order)`, `uint64_t pmm_free_frame_count(void)`, `#define PMM_FRAME_SIZE 4096`, `#define PMM_MAX_ORDER 10`. Later tasks (`paging.c`, `heap.c`) call `pmm_alloc`/`pmm_free` directly.
- Consumes: `kernel_phys_start[]`/`kernel_phys_end[]` (new linker symbols, this task), `serial_write_string`/`serial_write_hex64` (existing).

- [ ] **Step 1: Add kernel image bounds to the linker script**

Add two symbols bracketing the kernel sections so the frame allocator can exclude the kernel's own physical footprint. This wraps the existing `.text`/`.rodata`/`.data`/`.bss` block as-is; Task 2 replaces this same block with the full VMA/LMA split, carrying these two symbols forward unchanged:

```
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(8) :
    {
        *(.multiboot_header)
    }

    kernel_phys_start = .;

    .text ALIGN(4K) :
    {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) :
    {
        *(.data .data.*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    kernel_phys_end = .;
}
```

- [ ] **Step 2: Write the buddy allocator**

```c
#ifndef NEOOS_PMM_H
#define NEOOS_PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE 4096
#define PMM_MAX_ORDER  10 // largest block = 4096 * 2^10 = 4MiB

void pmm_init(void *multiboot_info);
void pmm_selftest(void);

// Allocates 2^order contiguous frames; returns the physical base address,
// or 0 on out-of-memory. order must be <= PMM_MAX_ORDER.
uint64_t pmm_alloc(unsigned order);

// Frees a block previously returned by pmm_alloc with the same order.
void pmm_free(uint64_t phys_addr, unsigned order);

uint64_t pmm_free_frame_count(void);

#endif
```

```c
#include "pmm.h"
#include "serial.h"

#define PMM_MAX_FRAMES ((4ULL * 1024 * 1024 * 1024) / PMM_FRAME_SIZE) // 4GiB cap, see Global Constraints
#define ORDER_NONE 0xFF

extern char kernel_phys_start[];
extern char kernel_phys_end[];

struct free_block {
    struct free_block *next;
    struct free_block *prev;
};

static struct free_block *free_lists[PMM_MAX_ORDER + 1];
// One byte per frame: the order of the free block starting at that frame,
// or ORDER_NONE if this frame isn't a free-block head. Lets pmm_free check
// "is my buddy free" in O(1) instead of walking a free list.
static uint8_t frame_order[PMM_MAX_FRAMES];
static uint64_t total_free_frames;

static inline uint64_t frame_to_phys(uint64_t frame) {
    return frame * PMM_FRAME_SIZE;
}

static inline uint64_t phys_to_frame(uint64_t phys) {
    return phys / PMM_FRAME_SIZE;
}

static void list_push(unsigned order, struct free_block *block) {
    block->prev = 0;
    block->next = free_lists[order];
    if (free_lists[order]) {
        free_lists[order]->prev = block;
    }
    free_lists[order] = block;
    frame_order[phys_to_frame((uint64_t)(uintptr_t)block)] = (uint8_t)order;
}

static void list_remove(unsigned order, struct free_block *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_lists[order] = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
    frame_order[phys_to_frame((uint64_t)(uintptr_t)block)] = ORDER_NONE;
}

uint64_t pmm_alloc(unsigned order) {
    if (order > PMM_MAX_ORDER) {
        return 0;
    }

    unsigned found_order = order;
    while (found_order <= PMM_MAX_ORDER && !free_lists[found_order]) {
        found_order++;
    }
    if (found_order > PMM_MAX_ORDER) {
        return 0; // out of memory
    }

    struct free_block *block = free_lists[found_order];
    list_remove(found_order, block);

    // Split the block down to the requested order, pushing each unused
    // buddy half onto its own free list.
    uint64_t phys = (uint64_t)(uintptr_t)block;
    while (found_order > order) {
        found_order--;
        uint64_t buddy_phys = phys + (PMM_FRAME_SIZE << found_order);
        list_push(found_order, (struct free_block *)(uintptr_t)buddy_phys);
    }

    total_free_frames -= (1ULL << order);
    return phys;
}

void pmm_free(uint64_t phys_addr, unsigned order) {
    uint64_t frame = phys_to_frame(phys_addr);
    total_free_frames += (1ULL << order); // caller's block wasn't counted as free before this call

    while (order < PMM_MAX_ORDER) {
        uint64_t buddy_frame = frame ^ (1ULL << order);
        if (buddy_frame >= PMM_MAX_FRAMES || frame_order[buddy_frame] != order) {
            break;
        }
        // Buddy is free at the same order: unlink it and merge upward.
        // Its frames are already counted in total_free_frames from when
        // it was freed, so no further accounting is needed here.
        list_remove(order, (struct free_block *)(uintptr_t)frame_to_phys(buddy_frame));
        frame = (frame < buddy_frame) ? frame : buddy_frame;
        order++;
    }

    list_push(order, (struct free_block *)(uintptr_t)frame_to_phys(frame));
}

uint64_t pmm_free_frame_count(void) {
    return total_free_frames;
}

static void add_region(uint64_t start, uint64_t end) {
    start = (start + PMM_FRAME_SIZE - 1) & ~(uint64_t)(PMM_FRAME_SIZE - 1);
    end = end & ~(uint64_t)(PMM_FRAME_SIZE - 1);

    for (uint64_t phys = start; phys + PMM_FRAME_SIZE <= end; phys += PMM_FRAME_SIZE) {
        if (phys_to_frame(phys) >= PMM_MAX_FRAMES) {
            break;
        }
        pmm_free(phys, 0);
    }
}

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot_mmap_entry entries[];
};

#define MULTIBOOT_TAG_TYPE_MMAP    6
#define MULTIBOOT_MEMORY_AVAILABLE 1

void pmm_init(void *multiboot_info) {
    for (unsigned i = 0; i <= PMM_MAX_ORDER; i++) {
        free_lists[i] = 0;
    }
    for (uint64_t i = 0; i < PMM_MAX_FRAMES; i++) {
        frame_order[i] = ORDER_NONE;
    }
    total_free_frames = 0;

    uint64_t kernel_start = (uint64_t)(uintptr_t)kernel_phys_start;
    uint64_t kernel_end = (uint64_t)(uintptr_t)kernel_phys_end;

    uint32_t total_size = *(uint32_t *)multiboot_info;
    uint8_t *ptr = (uint8_t *)multiboot_info + 8; // skip total_size + reserved
    uint8_t *end = (uint8_t *)multiboot_info + total_size;

    while (ptr < end) {
        struct multiboot_tag *tag = (struct multiboot_tag *)ptr;
        if (tag->type == 0) {
            break; // end tag
        }

        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
            uint32_t entry_count = (mmap->size - 16) / mmap->entry_size;

            for (uint32_t i = 0; i < entry_count; i++) {
                struct multiboot_mmap_entry *entry =
                    (struct multiboot_mmap_entry *)((uint8_t *)mmap->entries + i * mmap->entry_size);
                if (entry->type != MULTIBOOT_MEMORY_AVAILABLE) {
                    continue;
                }

                uint64_t start = entry->base_addr;
                uint64_t region_end = entry->base_addr + entry->length;
                if (start < 0x100000) {
                    start = 0x100000; // never hand out the real-mode/BIOS/EBDA area
                }

                if (start < kernel_end && region_end > kernel_start) {
                    if (start < kernel_start) {
                        add_region(start, kernel_start);
                    }
                    if (region_end > kernel_end) {
                        add_region(kernel_end, region_end);
                    }
                } else {
                    add_region(start, region_end);
                }
            }
        }

        ptr += (tag->size + 7) & ~7u; // tags are 8-byte aligned
    }

    serial_write_string("[pmm] free_frames=");
    serial_write_hex64(total_free_frames);
    serial_write_string(" (");
    serial_write_hex64(total_free_frames * PMM_FRAME_SIZE / (1024 * 1024));
    serial_write_string(" MiB)\n");
}

void pmm_selftest(void) {
    uint64_t before = total_free_frames;

    uint64_t block = pmm_alloc(3); // 8 frames
    if (!block) {
        serial_write_string("[pmm] selftest FAILED: alloc returned 0\n");
        return;
    }
    if (frame_order[phys_to_frame(block)] != ORDER_NONE) {
        serial_write_string("[pmm] selftest FAILED: allocated block still marked free\n");
        return;
    }

    // Free the two order-2 halves separately -- they are buddies of each
    // other (block is 8-frame-aligned, hence also 4-frame-aligned) and
    // must recombine into a single order-3 free block.
    uint64_t half_size = PMM_FRAME_SIZE << 2;
    pmm_free(block, 2);
    pmm_free(block + half_size, 2);

    if (frame_order[phys_to_frame(block)] != 3) {
        serial_write_string("[pmm] selftest FAILED: buddies did not coalesce back to order 3\n");
        return;
    }
    if (total_free_frames != before) {
        serial_write_string("[pmm] selftest FAILED: frame count did not return to baseline\n");
        return;
    }

    serial_write_string("[pmm] selftest passed, free_frames=");
    serial_write_hex64(total_free_frames);
    serial_write_string("\n");
}
```

- [ ] **Step 3: Extend the Makefile for `kernel/mm/`**

```makefile
CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -Wall -Wextra -std=gnu11 -O2 -Ikernel
ASFLAGS := -f elf64

BUILD_DIR := build
ISO_DIR := iso

C_SOURCES := $(wildcard kernel/*.c) $(wildcard kernel/mm/*.c)
C_OBJECTS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/isr_stubs.o
```

And the pattern rule, so `kernel/mm/pmm.c` correctly produces `build/mm/pmm.o`:

```makefile
$(BUILD_DIR)/%.o: kernel/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
```

- [ ] **Step 4: Wire into `kmain`**

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
#include "mm/pmm.h"

void kmain(void *multiboot_info) {
    serial_init();
    serial_write_string("NeoOS booting (milestone 3: memory management)\n");

    pmm_init(multiboot_info);
    pmm_selftest();

    vga_clear();
    vga_print_string("NeoOS booted");

    /* ... rest of the milestone 2 sequence, unchanged for this task ... */
```

- [ ] **Step 5: Build and verify**

Run: `make clean && make run` with serial redirected, e.g. `qemu-system-x86_64 -cdrom build/neoos.iso -serial file:/tmp/neoos.log -display none &` then, after a couple seconds, `grep -E '\[pmm\]' /tmp/neoos.log`.
Expected: two lines — `[pmm] free_frames=... MiB)` with a plausible MiB figure for QEMU's default RAM (well under 4GiB, comfortably above 0), and `[pmm] selftest passed, free_frames=...` whose count matches the first line's frame count (proving the selftest's alloc/free round-trip left no frames leaked). All milestone 2 behavior (banner, timer, keyboard, GDT/IDT logs) still appears in the log afterward.

- [ ] **Step 6: Commit**

```bash
git add kernel/mm/pmm.c kernel/mm/pmm.h Makefile linker.ld kernel/kernel.c
git commit -m "Add Multiboot2-driven buddy physical frame allocator"
```

---

### Task 2: Higher-Half Kernel Relink

**Files:**
- Modify: `boot/boot.asm` (rename its sections so only it stays low; add the static higher-half alias; call `kmain` via its full 64-bit address)
- Modify: `linker.ld` (VMA/LMA split for the C kernel)

**Interfaces:**
- Produces: `KERNEL_VIRT_BASE` (0xFFFFFFFF80000000, a linker constant — no C symbol needed, since nothing in this milestone needs to compute it at runtime beyond the debug print in this task's own verification step).
- Consumes: nothing new; this task changes *where* existing code is linked, not its behavior.

- [ ] **Step 1: Rename `boot.asm`'s sections and add the higher-half alias**

`boot/boot.asm`'s `.bss`, `.text`, and `.rodata` sections become `.boot.bss`, `.boot.text`, `.boot.rodata` so the linker script can keep only this file low while every other object (including `gdt_flush.asm`, `isr.asm`, and all of `kernel/`) links into the default, now-high-half, `.text`/`.rodata`/`.data`/`.bss`. A new static PDPT (`p3_table_high`) gives `boot.asm`'s existing page tables a second, high-half alias of the same physical memory the low identity map already covers — this is what lets `call kmain` land correctly the moment paging is enabled, with no C code needing to run at two different addresses.

```nasm
; boot/boot.asm — Multiboot2 header, long-mode transition, and entry into the C kernel

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
    dw 0
    dw 0
    dd 8
header_end:

section .boot.bss
align 4096
p4_table:
    resb 4096
p3_table:
    resb 4096
p3_table_high:      ; NEW: PDPT for the kernel's higher-half alias (PML4[511])
    resb 4096
p2_tables:
    resb 4096 * 4
align 16
stack_bottom:
    resb 16384
stack_top:

section .boot.text
[bits 32]
global _start
global p4_table      ; NEW: paging.c (Task 3) extends this same live table
extern kmain

_start:
    mov esp, stack_top
    mov edi, ebx

    call check_long_mode_supported
    call set_up_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

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

; Identity-maps the first 4GiB using 2MiB pages (unchanged from milestone
; 2: PML4[0] -> p3_table[0..3] -> p2_tables, 512 PD entries each), and
; ALSO aliases the same physical PD tables at PML4[511]/PDPT[510] --
; exactly the 1GiB virtual window starting at KERNEL_VIRT_BASE
; (0xFFFFFFFF80000000) where the C kernel is now linked (see linker.ld).
; Reusing p2_tables for both means no extra PD tables are needed: a 2MiB
; page's physical address doesn't care which PML4/PDPT path led to it.
set_up_page_tables:
    mov eax, p3_table
    or eax, 0b11
    mov [p4_table], eax

    mov eax, p3_table_high
    or eax, 0b11
    mov [p4_table + 511 * 8], eax

    mov eax, p2_tables
    or eax, 0b11
    mov [p3_table_high + 510 * 8], eax

    mov ecx, 0
.map_p3_table:
    mov eax, p2_tables
    mov edx, ecx
    shl edx, 12
    add eax, edx
    or eax, 0b11
    mov [p3_table + ecx * 8], eax

    inc ecx
    cmp ecx, 4
    jne .map_p3_table

    mov ecx, 0
.map_p2_table:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10000011
    mov [p2_tables + ecx * 8], eax

    inc ecx
    cmp ecx, 2048
    jne .map_p2_table
    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

section .boot.rodata
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
.data: equ $ - gdt64
    dq (1<<41) | (1<<44) | (1<<47)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .boot.text
[bits 64]
long_mode_start:
    mov edi, edi
    mov ax, gdt64.data
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; kmain is linked in the higher half (KERNEL_VIRT_BASE, see linker.ld).
    ; A plain `call kmain` assembles as a rel32 near call, which cannot
    ; reach across a gap this large -- load the full 64-bit address and
    ; call indirectly instead.
    mov rax, kmain
    call rax

    cli
.hang:
    hlt
    jmp .hang
```

- [ ] **Step 2: Split `linker.ld` into a low boot block and a high kernel block**

```
ENTRY(_start)

KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    /* Low half: Multiboot2 header and boot.asm's 32/64-bit bootstrap.
       VMA == LMA here -- this code runs before any higher-half mapping
       exists, so it cannot reference high addresses. */
    . = 1M;

    /* kernel_phys_start covers the WHOLE image, boot block included --
       pmm.c uses this to exclude the kernel's physical footprint from
       its free lists, and that must include boot.asm's live
       p4_table/p3_table/p2_tables and boot stack (all in .boot.bss
       below), not just the high-half C kernel. The low and high
       blocks are physically contiguous (the high block's AT() picks
       up right where the low block ends), so a single range here
       covers both. */
    kernel_phys_start = .;

    .multiboot ALIGN(8) :
    {
        *(.multiboot_header)
    }

    .boot.text ALIGN(4K) :
    {
        *(.boot.text)
    }

    .boot.rodata ALIGN(4K) :
    {
        *(.boot.rodata)
    }

    .boot.bss ALIGN(4K) :
    {
        *(.boot.bss)
    }

    /* High half: the C kernel (gdt_flush.asm and isr.asm's default
       .text included) and everything it calls. Linked at
       KERNEL_VIRT_BASE, loaded contiguously in physical memory right
       after the boot block via AT(). boot.asm's static page tables
       already map this physical range at this VMA (see
       set_up_page_tables), so `call kmain` lands correctly the moment
       paging is enabled -- no separate trampoline needed. */
    . = ALIGN(4K);
    . += KERNEL_VIRT_BASE;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRT_BASE)
    {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE)
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT_BASE)
    {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRT_BASE)
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    kernel_phys_end = . - KERNEL_VIRT_BASE;
}
```

This replaces Task 1 Step 1's placeholder `kernel_phys_start`/`kernel_phys_end` edit with the real VMA/LMA split; both symbols keep the same meaning (physical start/end of the C kernel image) that `pmm.c` already depends on.

- [ ] **Step 3: Build and verify the jump into the higher half**

Temporarily add one line at the very top of `kmain` (before `serial_init()`): `serial_init(); serial_write_string("kmain address check\n");` is not enough by itself — add a real address check:

```c
    serial_init();
    serial_write_string("[boot] kmain address=");
    serial_write_hex64((uint64_t)(uintptr_t)kmain);
    serial_write_string("\n");
```

Run: `make clean && make run` with `-serial file:/tmp/neoos.log -display none`.
Expected: `grep '\[boot\] kmain address=' /tmp/neoos.log` shows an address `>= 0xffffffff80000000` (confirms the kernel is executing from the higher half), immediately followed by the same `[pmm]` lines and milestone-2 log lines as before, with no triple fault / reboot loop. Leave this log line in place (it's a one-line, permanently useful boot banner, not a throwaway check) — it does not need to be reverted.

- [ ] **Step 4: Commit**

```bash
git add boot/boot.asm linker.ld kernel/kernel.c
git commit -m "Relink kernel to the higher half via a static boot-time alias"
```

---

### Task 3: Direct Physical Map and Paging API

**Files:**
- Create: `kernel/mm/paging.h`
- Create: `kernel/mm/paging.c`
- Modify: `kernel/kernel.c` (wire in `paging_init`/`paging_selftest`, right after `pmm_selftest`)

**Interfaces:**
- Consumes: `pmm_alloc`/`pmm_free` (Task 1), `p4_table` (`extern uint64_t p4_table[512];`, exported by `boot.asm` in Task 2), `serial_write_string`/`serial_write_hex64`.
- Produces: `#define PHYSMAP_BASE 0xFFFF800000000000ULL`, `static inline void *phys_to_virt(uint64_t phys)`, `static inline uint64_t virt_to_phys_physmap(uint64_t virt)`, `int paging_map(uint64_t virt, uint64_t phys, uint64_t flags)`, `void paging_unmap(uint64_t virt)`, `uint64_t paging_translate(uint64_t virt)`, flag macros `PAGE_PRESENT`/`PAGE_WRITABLE`/`PAGE_NO_EXECUTE`/`PAGE_USER`, `void paging_init(void)`, `void paging_selftest(void)`. Task 4 (driver migration) and Task 5 (`heap.c`) both call `phys_to_virt`/`virt_to_phys_physmap`.

- [ ] **Step 1: Write the paging header**

```c
#ifndef NEOOS_PAGING_H
#define NEOOS_PAGING_H

#include <stdint.h>

#define PHYSMAP_BASE 0xFFFF800000000000ULL

#define PAGE_PRESENT     (1ULL << 0)
#define PAGE_WRITABLE    (1ULL << 1)
#define PAGE_USER        (1ULL << 2)
#define PAGE_NO_EXECUTE  (1ULL << 63)

// Converts a physical address to its always-valid virtual alias in the
// direct physmap (see paging_init). Valid for any address within the
// first 4GiB (the physmap's coverage -- see Global Constraints).
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)(PHYSMAP_BASE + phys);
}

// Inverse of phys_to_virt, for pointers that came from it (e.g. heap
// pages allocated via pmm_alloc + phys_to_virt).
static inline uint64_t virt_to_phys_physmap(uint64_t virt) {
    return virt - PHYSMAP_BASE;
}

void paging_init(void);
void paging_selftest(void);

// General-purpose 4KiB mapping API for future callers that need a
// virtual address NOT already covered by the physmap or the kernel's
// own higher-half alias -- neither of which this function should be
// used on, since both are mapped with 2MiB pages at the PD level, and
// this walks tables assuming 4KiB PT-level entries throughout.
int paging_map(uint64_t virt, uint64_t phys, uint64_t flags);
void paging_unmap(uint64_t virt);
uint64_t paging_translate(uint64_t virt); // returns the mapped physical address, or 0 if unmapped

#endif
```

- [ ] **Step 2: Write `paging_init` (the direct physmap) and the map/unmap/translate API**

```c
#include "paging.h"
#include "pmm.h"
#include "serial.h"

#define PAGE_HUGE (1ULL << 7) // 2MiB page at the PD level
#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PHYSMAP_SIZE_BYTES (4ULL * 1024 * 1024 * 1024) // first 4GiB: all supported RAM plus the sub-4GiB MMIO hole (LAPIC/IOAPIC)
#define PHYSMAP_PML4_INDEX 256

#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)

extern uint64_t p4_table[512]; // boot.asm's live PML4 -- see boot/boot.asm

static uint64_t alloc_table_frame(void) {
    uint64_t phys = pmm_alloc(0);
    uint64_t *table = (uint64_t *)(uintptr_t)phys;
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }
    return phys;
}

// Walks one level, allocating a fresh table if `create` is set and the
// entry isn't present yet. Assumes 4KiB-page-tree structure throughout
// (not valid on huge-page-mapped regions -- see paging.h).
static uint64_t *table_entry(uint64_t *table, unsigned index, int create, uint64_t create_flags) {
    if (!(table[index] & PAGE_PRESENT)) {
        if (!create) {
            return 0;
        }
        uint64_t new_table_phys = alloc_table_frame();
        table[index] = new_table_phys | create_flags;
    }
    uint64_t next_phys = table[index] & PAGE_ADDR_MASK;
    return (uint64_t *)(uintptr_t)next_phys;
}

int paging_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t default_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t *pdpt = table_entry(p4_table, PML4_INDEX(virt), 1, default_flags);
    uint64_t *pd   = table_entry(pdpt, PDPT_INDEX(virt), 1, default_flags);
    uint64_t *pt   = table_entry(pd, PD_INDEX(virt), 1, default_flags);

    pt[PT_INDEX(virt)] = (phys & PAGE_ADDR_MASK) | flags | PAGE_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

void paging_unmap(uint64_t virt) {
    uint64_t *pdpt = table_entry(p4_table, PML4_INDEX(virt), 0, 0);
    uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(virt), 0, 0) : 0;
    uint64_t *pt   = pd ? table_entry(pd, PD_INDEX(virt), 0, 0) : 0;
    if (pt) {
        pt[PT_INDEX(virt)] = 0;
        __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    }
}

uint64_t paging_translate(uint64_t virt) {
    uint64_t *pdpt = table_entry(p4_table, PML4_INDEX(virt), 0, 0);
    uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(virt), 0, 0) : 0;
    uint64_t *pt   = pd ? table_entry(pd, PD_INDEX(virt), 0, 0) : 0;
    if (!pt || !(pt[PT_INDEX(virt)] & PAGE_PRESENT)) {
        return 0;
    }
    return (pt[PT_INDEX(virt)] & PAGE_ADDR_MASK) | (virt & 0xFFF);
}

void paging_init(void) {
    uint64_t pdpt_phys = alloc_table_frame();
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;

    uint64_t total_pages = PHYSMAP_SIZE_BYTES / (2 * 1024 * 1024);
    uint64_t pages_mapped = 0;
    for (uint64_t pdpt_index = 0; pages_mapped < total_pages; pdpt_index++) {
        uint64_t pd_phys = alloc_table_frame();
        uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys;

        for (unsigned pd_index = 0; pd_index < 512 && pages_mapped < total_pages; pd_index++, pages_mapped++) {
            uint64_t page_phys = pages_mapped * (2ULL * 1024 * 1024);
            pd[pd_index] = page_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
        }

        pdpt[pdpt_index] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    p4_table[PHYSMAP_PML4_INDEX] = pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE;

    serial_write_string("[paging] physmap installed: base=");
    serial_write_hex64(PHYSMAP_BASE);
    serial_write_string(" size=");
    serial_write_hex64(PHYSMAP_SIZE_BYTES);
    serial_write_string("\n");
}

#define PAGING_SELFTEST_VA 0xFFFF900000000000ULL

void paging_selftest(void) {
    uint64_t scratch_phys = pmm_alloc(0);
    if (!scratch_phys) {
        serial_write_string("[paging] selftest FAILED: pmm_alloc returned 0\n");
        return;
    }

    if (paging_map(PAGING_SELFTEST_VA, scratch_phys, PAGE_WRITABLE) != 0) {
        serial_write_string("[paging] selftest FAILED: paging_map error\n");
        return;
    }

    volatile uint8_t *scratch = (volatile uint8_t *)(uintptr_t)PAGING_SELFTEST_VA;
    *scratch = 0x42;
    if (*scratch != 0x42) {
        serial_write_string("[paging] selftest FAILED: pattern mismatch through new mapping\n");
        return;
    }

    if (paging_translate(PAGING_SELFTEST_VA) != scratch_phys) {
        serial_write_string("[paging] selftest FAILED: translate did not round-trip\n");
        return;
    }

    paging_unmap(PAGING_SELFTEST_VA);
    if (paging_translate(PAGING_SELFTEST_VA) != 0) {
        serial_write_string("[paging] selftest FAILED: translate still resolves after unmap\n");
        return;
    }

    pmm_free(scratch_phys, 0);
    serial_write_string("[paging] selftest passed\n");
}
```

- [ ] **Step 3: Wire into `kmain`**

```c
#include "mm/pmm.h"
#include "mm/paging.h"
/* ... */

    pmm_init(multiboot_info);
    pmm_selftest();

    paging_init();
    paging_selftest();

    vga_clear();
```

- [ ] **Step 4: Build and verify**

Run: `make clean && make run` with `-serial file:/tmp/neoos.log -display none`.
Expected: log shows, in order, `[pmm] free_frames=...`, `[pmm] selftest passed`, `[paging] physmap installed: base=ffff800000000000 size=100000000`, `[paging] selftest passed`, then the milestone-2 lines unchanged, with no page fault or reboot.

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/paging.c kernel/mm/paging.h kernel/kernel.c
git commit -m "Add direct physical map and 4KiB paging API"
```

---

### Task 4: Migrate VGA/LAPIC/IOAPIC/ACPI to Physmap-Relative Addressing

**Files:**
- Modify: `kernel/vga.c`
- Modify: `kernel/lapic.c`
- Modify: `kernel/ioapic.c`
- Modify: `kernel/acpi.c`

**Interfaces:**
- Consumes: `phys_to_virt` (Task 3).
- No signature changes to any of these files' public headers (`vga.h`, `lapic.h`, `ioapic.h`, `acpi.h` are untouched) — this task only changes how each file resolves a physical address it already had, into a pointer.

- [ ] **Step 1: `vga.c`**

```c
#include "vga.h"
#include "mm/paging.h"

static volatile unsigned short *VGA_BUFFER;
static const unsigned short VGA_COLOR_WHITE_ON_BLACK = 0x0f;
#define VGA_CELL_COUNT 2000
#define VGA_COLUMNS 80

static int vga_cursor = 0;

void vga_clear(void) {
    VGA_BUFFER = (volatile unsigned short *)phys_to_virt(0xb8000);
    for (int i = 0; i < VGA_CELL_COUNT; i++) {
        VGA_BUFFER[i] = (unsigned short)(' ') | (unsigned short)(VGA_COLOR_WHITE_ON_BLACK << 8);
    }
    vga_cursor = 0;
}
```

(`vga_print_string`/`vga_putc` are unchanged — they already only reference `VGA_BUFFER`, which is now set once by `vga_clear`. `vga_clear` runs after `paging_init` in `kmain`'s Task 3 ordering, so the physmap is live by the time this executes.)

- [ ] **Step 2: `lapic.c`**

```c
#include "lapic.h"
#include "mm/paging.h"

/* ... register #defines unchanged ... */

static volatile uint32_t *lapic_base;

/* ... lapic_read/lapic_write unchanged ... */

void lapic_init(uint32_t address) {
    lapic_base = (volatile uint32_t *)phys_to_virt(address);
    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | 0x100 | 0xFF);
}
```

- [ ] **Step 3: `ioapic.c`**

```c
#include "ioapic.h"
#include "mm/paging.h"

/* ... #defines and ioapic_read/ioapic_write unchanged ... */

void ioapic_init(uint32_t address) {
    ioapic_base = (volatile uint32_t *)phys_to_virt(address);
    (void)ioapic_read(0x00);
}
```

- [ ] **Step 4: `acpi.c`**

```c
#include "acpi.h"
#include "serial.h"
#include "mm/paging.h"

/* ... struct definitions, sum_bytes, is_rsdp_signature, is_apic_signature unchanged ... */

static struct acpi_rsdp *find_rsdp_in_range(uint32_t start, uint32_t end) {
    for (uint32_t addr = start; addr < end; addr += 16) {
        struct acpi_rsdp *candidate = (struct acpi_rsdp *)phys_to_virt(addr);
        if (is_rsdp_signature(candidate) && sum_bytes(candidate, 20) == 0) {
            return candidate;
        }
    }
    return 0;
}

static struct acpi_rsdp *find_rsdp(void) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    uint16_t ebda_segment = *(volatile uint16_t *)phys_to_virt(0x40E);
#pragma GCC diagnostic pop
    uint32_t ebda_addr = (uint32_t)ebda_segment << 4;

    struct acpi_rsdp *rsdp = find_rsdp_in_range(ebda_addr, ebda_addr + 1024);
    if (rsdp) {
        return rsdp;
    }
    return find_rsdp_in_range(0xE0000, 0x100000);
}

static struct acpi_madt *find_madt_via_xsdt(uint64_t xsdt_address) {
    struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)phys_to_virt(xsdt_address);
    uint64_t *entries = (uint64_t *)((uint8_t *)xsdt + sizeof(struct acpi_sdt_header));
    uint32_t entry_count = (xsdt->length - sizeof(struct acpi_sdt_header)) / sizeof(uint64_t);

    for (uint32_t i = 0; i < entry_count; i++) {
        struct acpi_sdt_header *table = (struct acpi_sdt_header *)phys_to_virt(entries[i]);
        if (is_apic_signature(table)) {
            return (struct acpi_madt *)table;
        }
    }
    return 0;
}

static struct acpi_madt *find_madt_via_rsdt(uint32_t rsdt_address) {
    struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)phys_to_virt(rsdt_address);
    uint32_t *entries = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_sdt_header));
    uint32_t entry_count = (rsdt->length - sizeof(struct acpi_sdt_header)) / sizeof(uint32_t);

    for (uint32_t i = 0; i < entry_count; i++) {
        struct acpi_sdt_header *table = (struct acpi_sdt_header *)phys_to_virt(entries[i]);
        if (is_apic_signature(table)) {
            return (struct acpi_madt *)table;
        }
    }
    return 0;
}

/* ... parse_madt and acpi_find_madt unchanged (they only touch info->*
   raw physical values and already-resolved pointers) ... */
```

- [ ] **Step 5: Build and verify (regression check)**

Run: `make clean && make run` with `-serial file:/tmp/neoos.log -display none`, and separately with the QEMU monitor available to send a keypress: `qemu-system-x86_64 -cdrom build/neoos.iso -serial file:/tmp/neoos.log -display none -monitor stdio`, then from the monitor `sendkey a` after boot settles.
Expected: identical milestone-2 behavior to before this task — `[acpi]` line with the same lapic/ioapic addresses, `[lapic]`/`[ioapic]` init lines, periodic timer ticks over several seconds, and the sent keypress echoed to the serial log — proving the physmap migration didn't change behavior, only how addresses are resolved.

- [ ] **Step 6: Commit**

```bash
git add kernel/vga.c kernel/lapic.c kernel/ioapic.c kernel/acpi.c
git commit -m "Migrate VGA/LAPIC/IOAPIC/ACPI to physmap-relative addressing"
```

---

### Task 5: Kernel Heap (`kmalloc`/`kfree`)

**Files:**
- Create: `kernel/mm/heap.h`
- Create: `kernel/mm/heap.c`
- Modify: `kernel/kernel.c` (wire in `heap_init`/`heap_selftest`)

**Interfaces:**
- Consumes: `pmm_alloc`/`pmm_free` (Task 1), `phys_to_virt`/`virt_to_phys_physmap` (Task 3).
- Produces: `void heap_init(void)`, `void heap_selftest(void)`, `void *kmalloc(size_t size)`, `void kfree(void *ptr)`.

- [ ] **Step 1: Write the heap header**

```c
#ifndef NEOOS_HEAP_H
#define NEOOS_HEAP_H

#include <stddef.h>

void heap_init(void);
void heap_selftest(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

#endif
```

- [ ] **Step 2: Write the segregated size-class allocator**

Structured after mimalloc's free-list sharding (per the design spec): segregated size classes, each backed by 4KiB pages with their own free list, minus mimalloc's per-core sharding (there's one execution context in this milestone). A page's header lives at its own start, found by rounding any pointer it handed out down to the nearest 4KiB boundary — this works uniformly for both size-class slots (small offset into the page) and large allocations (also given a small offset, on their first page), so `kfree` doesn't need to know in advance which kind of pointer it was given.

```c
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "serial.h"

static const uint32_t heap_size_classes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
#define HEAP_NUM_CLASSES (sizeof(heap_size_classes) / sizeof(heap_size_classes[0]))
#define HEAP_LARGE_MARKER 0xFFFFFFFFu

struct heap_free_slot {
    struct heap_free_slot *next;
};

struct heap_page {
    struct heap_page *next;
    struct heap_free_slot *free_list;
    uint32_t size_class; // bytes per slot, or HEAP_LARGE_MARKER for a large (multi-page) allocation
    uint32_t meta;        // size-class pages: free slot count. Large allocations: the pmm buddy order.
};

static struct heap_page *class_pages[HEAP_NUM_CLASSES];

static int size_class_for(size_t size) {
    for (unsigned i = 0; i < HEAP_NUM_CLASSES; i++) {
        if (size <= heap_size_classes[i]) {
            return (int)i;
        }
    }
    return -1;
}

static struct heap_page *heap_new_page(uint32_t size_class) {
    uint64_t phys = pmm_alloc(0);
    if (!phys) {
        return 0;
    }

    struct heap_page *page = (struct heap_page *)phys_to_virt(phys);
    page->size_class = size_class;
    page->free_list = 0;
    page->next = 0;

    uint8_t *area = (uint8_t *)page + sizeof(struct heap_page);
    uint32_t slot_count = (uint32_t)((PMM_FRAME_SIZE - sizeof(struct heap_page)) / size_class);
    for (uint32_t i = 0; i < slot_count; i++) {
        struct heap_free_slot *slot = (struct heap_free_slot *)(area + i * size_class);
        slot->next = page->free_list;
        page->free_list = slot;
    }
    page->meta = slot_count;
    return page;
}

void heap_init(void) {
    for (unsigned i = 0; i < HEAP_NUM_CLASSES; i++) {
        class_pages[i] = 0;
    }
    serial_write_string("[heap] initialized\n");
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return 0;
    }
    if (size < sizeof(struct heap_free_slot)) {
        size = sizeof(struct heap_free_slot);
    }

    int class_index = size_class_for(size);
    if (class_index >= 0) {
        struct heap_page *page = class_pages[class_index];
        // Only the front page is ever checked -- a full front page is
        // replaced with a fresh one rather than scanning older pages
        // for a free slot. This can waste memory that was freed back
        // into a non-front page, matching the design spec's decision
        // not to build page reclamation/reuse in this milestone.
        if (!page || !page->free_list) {
            page = heap_new_page(heap_size_classes[class_index]);
            if (!page) {
                return 0;
            }
            page->next = class_pages[class_index];
            class_pages[class_index] = page;
        }

        struct heap_free_slot *slot = page->free_list;
        page->free_list = slot->next;
        page->meta--;
        return (void *)slot;
    }

    // Large allocation: header lives on the first page; the block spans
    // ceil((size + header) / 4096) frames, rounded up to a buddy order.
    uint64_t needed = size + sizeof(struct heap_page);
    uint64_t frames_needed = (needed + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    unsigned order = 0;
    while ((1ULL << order) < frames_needed) {
        order++;
    }

    uint64_t phys = pmm_alloc(order);
    if (!phys) {
        return 0;
    }
    struct heap_page *page = (struct heap_page *)phys_to_virt(phys);
    page->size_class = HEAP_LARGE_MARKER;
    page->meta = order;
    page->free_list = 0;
    page->next = 0;
    return (void *)((uint8_t *)page + sizeof(struct heap_page));
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }

    struct heap_page *page = (struct heap_page *)((uintptr_t)ptr & ~(uint64_t)0xFFF);
    if (page->size_class == HEAP_LARGE_MARKER) {
        pmm_free(virt_to_phys_physmap((uint64_t)(uintptr_t)page), page->meta);
        return;
    }

    struct heap_free_slot *slot = (struct heap_free_slot *)ptr;
    slot->next = page->free_list;
    page->free_list = slot;
    page->meta++;
}

void heap_selftest(void) {
    void *ptrs[16];
    for (int i = 0; i < 16; i++) {
        size_t size = heap_size_classes[i % HEAP_NUM_CLASSES];
        ptrs[i] = kmalloc(size);
        if (!ptrs[i]) {
            serial_write_string("[heap] selftest FAILED: kmalloc returned NULL\n");
            return;
        }
        uint8_t pattern = (uint8_t)(i + 1);
        for (size_t b = 0; b < size; b++) {
            ((uint8_t *)ptrs[i])[b] = pattern;
        }
    }

    void *large = kmalloc(9000); // exercises the multi-page path
    if (!large) {
        serial_write_string("[heap] selftest FAILED: large kmalloc returned NULL\n");
        return;
    }
    for (size_t b = 0; b < 9000; b++) {
        ((uint8_t *)large)[b] = 0xAB;
    }

    for (int i = 0; i < 16; i++) {
        size_t size = heap_size_classes[i % HEAP_NUM_CLASSES];
        uint8_t pattern = (uint8_t)(i + 1);
        for (size_t b = 0; b < size; b++) {
            if (((uint8_t *)ptrs[i])[b] != pattern) {
                serial_write_string("[heap] selftest FAILED: pattern mismatch\n");
                return;
            }
        }
    }
    for (size_t b = 0; b < 9000; b++) {
        if (((uint8_t *)large)[b] != 0xAB) {
            serial_write_string("[heap] selftest FAILED: large pattern mismatch\n");
            return;
        }
    }

    for (int i = 0; i < 16; i++) {
        kfree(ptrs[i]);
    }
    kfree(large);

    serial_write_string("[heap] selftest passed\n");
}
```

- [ ] **Step 3: Wire into `kmain`**

```c
#include "mm/heap.h"
/* ... */

    ioapic_set_redirection(keyboard_pin, VECTOR_KEYBOARD, acpi.irq1_polarity,
                            acpi.irq1_trigger, (uint8_t)lapic_get_id());
    serial_write_string("[ioapic] keyboard routed: gsi=");
    serial_write_hex64(acpi.irq1_gsi);
    serial_write_string(" vector=0x21\n");

    heap_init();
    heap_selftest();

    serial_write_string("NeoOS: interrupts enabled, entering idle loop\n");
    __asm__ volatile ("sti");
```

- [ ] **Step 4: Build and verify**

Run: `make clean && make run` with `-serial file:/tmp/neoos.log -display none`.
Expected: `[heap] initialized` and `[heap] selftest passed` appear in the log, after the keyboard routing line and before the idle-loop banner, with no crash.

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/heap.c kernel/mm/heap.h kernel/kernel.c
git commit -m "Add mimalloc-structured segregated size-class kernel heap"
```

---

### Task 6: Final Integration and Full Verification

**Files:**
- None (verification-only task; fixes anything Steps 1-3 turn up).

**Interfaces:** None new — this task exercises everything Tasks 1-5 produced.

- [ ] **Step 1: Full boot log check**

Run: `make clean && make run` with `-serial file:/tmp/neoos.log -display none`, let it run 5+ seconds, then stop QEMU.
Expected, in order: `[boot] kmain address=` (>= `0xffffffff80000000`), `[pmm] free_frames=...`, `[pmm] selftest passed`, `[paging] physmap installed...`, `[paging] selftest passed`, `[gdt]`/`[idt]` lines, `[acpi]` line, `[pic] disabled`, `[lapic] enabled`, `[ioapic] initialized`, periodic timer tick lines, `[ioapic] keyboard routed`, `[heap] initialized`, `[heap] selftest passed`, `NeoOS: interrupts enabled, entering idle loop` — no `FAILED`, no exception dump, no reboot.

- [ ] **Step 2: Keyboard regression check**

Run: `qemu-system-x86_64 -cdrom build/neoos.iso -serial file:/tmp/neoos.log -display none -monitor stdio`; after boot settles, send `sendkey a` then `sendkey b` from the monitor.
Expected: `a` and `b` appear in the serial log via the existing keyboard echo path, confirming the physmap migration (Task 4) didn't regress milestone 2's keyboard handling.

- [ ] **Step 3: Forced divide-by-zero regression check**

Temporarily add `__asm__ volatile ("divb %0" :: "r"((uint8_t)0));` (or reuse whatever forced-fault technique milestone 2's Task 3 used) right before the `sti` in `kmain`, rebuild, and run with `-serial file:/tmp/neoos.log -display none`.
Expected: a full register dump (including `cr2` is irrelevant here since vector 0 has none, but `rip`/`cs`/`rflags`/GP registers all print) appears in the log and on a `screendump`, followed by a halt — no triple fault, no silent reboot. Then remove the forced fault and confirm normal boot resumes (repeat Step 1).

- [ ] **Step 4: Commit**

Only if Steps 1-3 required fixes; otherwise this task produces no diff and needs no commit. If fixes were needed:

```bash
git add -A
git commit -m "Fix milestone 3 integration issues found during full verification"
```
