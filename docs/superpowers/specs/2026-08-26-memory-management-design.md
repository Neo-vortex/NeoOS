# NeoOS — Milestone 3: Memory Management (Physical Frames, Paging, Higher-Half, kmalloc)

## Goal

Turn NeoOS from a kernel running out of a static, boot-time-generated
identity map into one that manages its own memory: a physical frame
allocator driven by the real Multiboot2 memory map, dynamically-built
page tables with a direct physical map, a kernel relinked to run in the
higher half, and a kernel heap (`kmalloc`/`kfree`) on top of it. This is
the third milestone of NeoOS; multi-core/SMP and any userland/process
model remain explicitly deferred.

## Success criteria

Booting `neoos.iso` in QEMU and:
- Serial log shows the buddy allocator initializing with a frame count
  derived from the real Multiboot2 memory map (not a hardcoded size),
  and a self-test exercising alloc/split/free/coalesce across a few
  orders round-trips cleanly (a freed block recombines with its buddy
  back to its original larger block).
- Serial log confirms the kernel is executing from a higher-half
  virtual address (e.g. logs `&kmain` or an equivalent known symbol,
  which must be above the higher-half base) after the `CR3` switch,
  with no triple fault.
- All milestone 2 behavior still works after the switch to the new
  page tables: periodic Local APIC timer ticks, keyboard echo (serial
  + VGA), and the forced divide-by-zero exception dump all still work
  — this is the regression check that migrating `vga.c`, `lapic.c`,
  `ioapic.c`, and `acpi.c` to physmap-relative addressing didn't break
  anything.
- A `kmalloc`/`kfree` self-test allocates and frees objects across
  several size classes, pattern-fills and verifies each one, with no
  corruption or crash.
- No unexpected page faults or general-protection faults occur during
  normal boot; if paging setup is wrong, the existing milestone 2
  exception handlers must produce a clean register dump rather than a
  silent triple fault/reboot.

## Out of scope (future specs)

- Multi-core/SMP. The concurrency-oriented parts of modern allocator
  designs (per-CPU/per-page free-list sharding in SLUB and mimalloc)
  exist to avoid lock contention across cores; NeoOS has one execution
  context through this milestone, so `kmalloc` borrows the segregated
  size-class *structure* of those designs without building the
  sharding machinery. That gets revisited once SMP lands.
- Any userland, app runtime, or process model — this milestone manages
  kernel memory only. No user/supervisor page permission split is
  exercised yet (though the paging API supports the bit).
- Swapping/paging to disk, memory-mapped files, copy-on-write.
- NUMA awareness.
- Reclaiming or resizing the physmap/heap at runtime beyond simple
  growth (the heap grows by requesting more pages; it never shrinks
  back).

## Architecture

### Physical frame allocator (buddy allocator)

Source of truth for available RAM is the Multiboot2 memory map tag
(type 6), reachable from the `multiboot_info` pointer already threaded
into `kmain` but currently unused. Parse it to find the highest
available physical address and all reserved/unavailable ranges.

Frames are tracked with a buddy allocator: an array of free-list heads,
one per order (order 0 = one 4KiB frame, doubling up to a maximum order
sized to the detected RAM, capped so the largest block is a few MiB).
Each free block's first bytes hold an intrusive next-pointer, so no
separate metadata array is needed — the free memory itself is the free
list. `alloc(order)` finds a free block at that order (splitting a
larger one if necessary, pushing the unused half onto the next order
down's free list). `free(ptr, order)` checks whether the block's buddy
(computed via address XOR block size) is also free, and if so merges
them repeatedly up the orders (coalescing) rather than just re-linking.

Reserved regions from the memory map (and the kernel image's own
physical footprint) are excluded from the initial free lists by simply
never inserting frames that overlap them.

### Bootstrap ordering (the chicken-and-egg problem)

The buddy allocator needs writable memory to store its free lists
before paging exists to give it any; paging needs frames from the
buddy allocator to build its tables. Resolved by staging:

1. Boot proceeds exactly as today through long-mode entry, using the
   existing low, static, boot-time identity-mapped 4GiB tables from
   `boot.asm` — unchanged and kept as the bootstrap scaffold.
2. In `kmain`, parse the Multiboot2 memory map and initialize the
   buddy allocator's free lists. The intrusive free-list writes land
   in physical frames that are, at this point, still identity-mapped
   (physical == virtual), so this is safe with no new paging code yet.
3. Using frames obtained *from* the buddy allocator, build fresh,
   dynamically-managed page tables: a direct physmap (all physical RAM
   1:1 at a fixed high virtual offset) plus the higher-half kernel
   mapping (the kernel image's physical load range, mapped a second
   time at its high link address, alongside the still-live low
   mapping).
4. Load the new `CR3`.
5. Jump to a higher-half entry symbol via the trampoline in `boot.asm`.
6. The low identity mapping for the kernel image is dropped — nothing
   needs it once the physmap and the high kernel mapping exist. From
   this point on, the buddy allocator (and everything else touching
   physical addresses) uses the physmap offset instead of raw identity
   addresses.

### Paging

A page-table management layer replaces the static boot-time tables.
Rather than the classic x86 recursive-mapping trick, page-table frames
are walked directly through the physmap: any physical address (a page
table's own frame included) is reachable as `phys + PHYSMAP_BASE`, so
table entries can simply be dereferenced as ordinary pointers.

API surface: map a range of virtual pages to physical frames with
given permissions (present/writable/no-execute/user-vs-supervisor —
user-mode is plumbed through for future milestones, not exercised
yet), unmap a range, and translate a virtual address to its backing
physical frame. NX support requires setting `EFER.NXE`, which the
current boot code does not set (only `LME`) — this milestone adds it.

Two fixed high-half regions are established:
- **Direct physmap** at a fixed offset (e.g. `0xFFFF800000000000`):
  all physical RAM, 1:1, present+writable, no-execute. This is how the
  allocator and the page-table code itself reach arbitrary physical
  memory.
- **Kernel image** at a fixed high link address (e.g.
  `0xFFFFFFFF80000000 + 1M`): the kernel's own `.text`/`.rodata`/
  `.data`/`.bss`, with permissions per section (`.text` executable,
  not writable; `.rodata` not writable, not executable; `.data`/`.bss`
  writable, not executable).

### Higher-half kernel relink

`linker.ld` gains a VMA/LMA split via `AT()` directives: sections are
*linked* at the high virtual address but *loaded* at the low physical
address where Multiboot2/GRUB actually places the bytes. `boot.asm`
gains the trampoline: after the `CR3` switch (step 4 above), a jump to
the high-half virtual address of the post-switch entry point, using an
address computed relative to the new mapping rather than the
assembler's default low link address.

### Migrating existing physical-address assumptions

Several milestone 2 modules assume the old flat identity map and must
be updated to add the physmap offset instead of using raw physical
addresses directly:

- **`vga.c`** — the VGA text buffer at physical `0xB8000` becomes
  `physmap_base + 0xB8000`.
- **`lapic.c` / `ioapic.c`** — MMIO base addresses (parsed from the
  ACPI MADT) go through the physmap offset.
- **`acpi.c`** — the RSDP scan over the EBDA and the BIOS ROM area
  (`0xE0000`–`0xFFFFF`) reads physical memory directly today; same
  fix.

No changes are needed to GDT/TSS/IDT — their addresses are
symbol-relative and resolve correctly once recompiled against the new
high link address.

### Kernel heap (`kmalloc`/`kfree`)

Structured after mimalloc's free-list-sharding design, minus the
per-core sharding (see Out of scope): segregated size classes (a fixed
set of power-of-two-ish bucket sizes), each backed by fixed-size pages
obtained from the paging layer, with one free list per page per size
class. Allocating pulls from the current page's free list for that
size class, requesting a fresh page from the paging/frame layer when
the current one is exhausted; freeing pushes back onto its owning
page's free list. This keeps the same locality and simplicity
benefits mimalloc gets from page-local free lists, without building
the cross-thread "thread-free list" machinery a single execution
context has no use for. Large allocations that don't fit any size
class bucket are handled by direct multi-page allocation.

## File structure

```
kernel/
  kernel.c/.h      # kmain: extends milestone 2 sequence with mm init below
  mm/
    pmm.c/.h       # buddy allocator: parses Multiboot2 memory map, alloc/free by order
    paging.c/.h    # page-table build/map/unmap/translate, physmap + kernel mapping setup
    heap.c/.h      # kmalloc/kfree: segregated size-class allocator on top of paging
  vga.c/.h         # updated to use physmap offset for the VGA buffer
  lapic.c/.h       # updated to use physmap offset for MMIO
  ioapic.c/.h      # updated to use physmap offset for MMIO
  acpi.c/.h        # updated to use physmap offset for EBDA/BIOS ROM scan
boot/
  boot.asm         # adds the post-CR3-switch higher-half trampoline jump
linker.ld          # VMA/LMA split for the higher-half link
```

## Data flow (kmain, extending milestone 2's sequence)

```
serial_init -> vga_clear + banner
  -> gdt/tss install, idt_init                    (unchanged from milestone 2)
  -> acpi_find_madt                                (unchanged)
  -> pmm_init(multiboot_info)                      (NEW: parse memory map, build buddy free lists)
  -> pmm_selftest()                                (NEW: alloc/free/coalesce round-trip, log to serial)
  -> paging_init()                                 (NEW: build physmap + kernel high mapping, load CR3)
  -> jump to higher-half entry                     (NEW: boot.asm trampoline)
  -> [now running higher-half, physmap live]
  -> pic_disable, lapic_init, ioapic_init, timer_init, keyboard routing  (unchanged, now physmap-relative)
  -> heap_init()                                   (NEW: carve first pages for kmalloc)
  -> heap_selftest()                               (NEW: alloc/free/pattern-check, log to serial)
  -> sti
  -> idle loop
```

## Testing / verification

Same approach as milestone 2 — no host-runnable unit tests (bare-metal,
no host runtime); verification is via QEMU, serial log capture, and
screendumps:

- **Frame allocator:** serial log shows the detected frame count and
  the self-test's alloc/split/free/coalesce sequence succeeding.
- **Higher-half switch:** serial log shows a known symbol's address
  above the higher-half base, and the kernel continues running (no
  triple fault, no silent reboot) immediately after the `CR3` load and
  trampoline jump.
- **Regression (milestone 2 features):** re-run the milestone 2
  verification steps (timer ticks over several seconds, `sendkey`
  keyboard echo, forced divide-by-zero dump) against the milestone 3
  build to confirm the physmap migration didn't break them.
- **Heap:** serial log shows the self-test allocating/freeing several
  differently-sized objects with pattern verification, no corruption.
- **Stability:** let the kernel idle for several seconds post-boot with
  no crash or unexpected reboot.

## Error handling

Unchanged convention from milestone 2: any fault that does occur (e.g.
a bad mapping producing a page fault) is caught by the existing
exception handlers, which dump registers (including `CR2`) to serial
and VGA and halt deterministically — never a silent triple fault. The
frame allocator and heap self-tests are designed to fail loudly (halt
with a serial message) rather than silently continue on an
inconsistency, since a subtly broken allocator is worse than a visibly
halted one at this stage.
