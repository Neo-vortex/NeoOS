# NeoOS — Milestone 2: Rich Interrupt Handling (APIC/IOAPIC, Exceptions, Timer, Keyboard)

## Goal

Turn NeoOS from a static print-and-halt kernel into an interruptible one:
a full 256-entry IDT with real handlers for all 32 CPU exceptions, a
Local APIC + IOAPIC interrupt path (no legacy PIC in the live interrupt
path), a PIT-calibrated periodic timer, and a working PS/2 keyboard
driver. A serial (COM1) driver is added as the primary diagnostics
channel. This is the second milestone of NeoOS; memory management
(physical frame allocator, real paging, higher-half kernel) is
explicitly deferred to a later spec, as is any userland/app-runtime work.

## Success criteria

Booting `neoos.iso` in QEMU and:
- A periodic timer tick (Local APIC timer, calibrated against the
  legacy PIT) is visibly logged to serial at roughly its calibrated
  rate over a multi-second run, with no crash/reboot.
- Simulated keypresses (via QEMU monitor `sendkey`) are echoed as ASCII
  to serial and VGA.
- A deliberately forced divide-by-zero (vector 0) produces a clean
  register dump on serial and VGA, then a deterministic halt — not a
  triple fault or silent reboot.
- No spurious legacy-PIC interrupts occur (PIC is masked/disabled;
  all real IRQs arrive via IOAPIC → Local APIC).

## Out of scope (future specs)

- Physical frame allocator, virtual memory beyond the existing flat
  1GiB identity map, higher-half kernel.
- Multi-core / SMP (only the boot CPU's Local APIC is initialized).
- Any userland, app runtime, or process model — exception handlers halt
  because there is nothing to recover into yet.
- UEFI boot path, real-hardware testing.
- Full PS/2 layout support (only a basic US scancode-set-1 mapping).

## Architecture

### ACPI discovery

Scan the EBDA (via the segment word at `0x40E`, scaled by 16) and the
BIOS ROM area `0xE0000`–`0xFFFFF` (16-byte aligned) for the `RSD PTR `
signature, validate the RSDP checksum. If RSDP revision ≥ 2, walk the
XSDT (64-bit table pointers); otherwise walk the RSDT (32-bit
pointers). Locate the MADT ("APIC") table among the top-level entries.

### MADT parsing

Walk MADT entries and record:
- **Local APIC** entries (type 0) — the boot CPU's Local APIC is what
  matters; other entries are ignored (no SMP in this milestone).
- **IOAPIC** entry (type 1) — base MMIO address and GSI base.
- **Interrupt Source Override** entries (type 2) — legacy IRQ→GSI
  remaps and polarity/trigger-mode overrides. Both IRQ0 (PIT) and IRQ1
  (keyboard) must be checked against this table rather than assumed to
  map identity (`IRQ n → GSI n`); QEMU's default MADT commonly overrides
  IRQ0.

### Legacy PIC handling

Remap the 8259 PIC off the CPU exception vector range (defensive, in
case it fires once during the transition), then mask every line on
both PICs. The PIC is never unmasked again — all real interrupts flow
through the IOAPIC once it's initialized.

### Local APIC + IOAPIC initialization

Map the Local APIC's MMIO registers (address from MADT, within the
existing flat identity map) and enable it via the spurious-interrupt
vector register. Map the IOAPIC's MMIO registers and program
redirection table entries for the two GSIs actually needed (the
Local APIC timer does **not** go through the IOAPIC — it is a
per-core register, not a routed interrupt — only the keyboard IRQ is
routed via IOAPIC, using its real GSI/polarity from the override table).

### IDT and exception handling

A 256-entry IDT. Vectors 0–31 (CPU exceptions) each get a real handler
via a common ISR stub (assembly) that normalizes the register frame —
some vectors push a CPU error code (e.g. page fault, GPF, double fault)
and some don't, so the stub pads a dummy zero for the ones that don't,
producing one uniform frame layout for the C-level dispatcher. Each
exception handler dumps registers (plus `CR2` and the decoded error
code where applicable) to serial and a short banner to VGA, then halts
deterministically (`cli` + `hlt` loop) — there is no recovery path
without a process model, so halting is the correct behavior here, same
convention as the boot milestone's CPUID error path.

**Double fault (vector 8) uses a dedicated IST stack.** A TSS is added
(extending the existing GDT with a TSS descriptor, loaded via `ltr`)
with one IST entry pointing at a separate stack. Vector 8's IDT entry
references that IST index. This ensures a fault caused by a corrupted
kernel stack doesn't recurse into the same broken stack and silently
triple-fault.

Vectors 32–255 default to a generic "unhandled interrupt" handler that
logs the vector number and halts, except the two IRQs this milestone
actually wires up (timer, keyboard).

### Timer

The legacy PIT is used exactly once, as a one-shot stopwatch: program
it for a known interval, count how many Local APIC timer ticks occur
in that interval, and use that measurement to compute the divisor and
initial count for a real periodic Local APIC timer interrupt at a
target rate (100Hz). The PIT is never used again afterward. The timer
IRQ handler increments a tick counter and periodically logs to serial.

### Keyboard

The PS/2 keyboard IRQ, routed via IOAPIC using its real GSI from the
MADT override table, is handled with a scancode-set-1 to ASCII
translation table (basic US layout — no full layout system). The
handler echoes translated characters to serial and VGA.

### Serial (COM1) driver

A simple polling PIO driver on `0x3F8` (`serial_init`,
`serial_write_string`/`serial_putc`). This becomes the primary channel
for verbose diagnostics (exception dumps, timer tick log, keyboard
echo) throughout this milestone and beyond — QEMU can redirect COM1 to
a log file, making automated verification (`grep`-able) far easier than
parsing VGA screendumps.

### VGA

Extends the existing `vga_print_string` with `vga_clear` (fills all
2000 cells with a blank white-on-black cell) — addresses a deferred
item from the boot milestone (residual BIOS text framing prior output)
and gives interrupt-driven output a clean area to write into.

## File structure

```
kernel/
  kernel.c/.h      # kmain: orchestrates init sequence below, then idles
  vga.c/.h         # vga_print_string (existing), vga_clear (new)
  serial.c/.h      # serial_init, serial_write_string, serial_putc
  gdt.c/.h         # extends existing GDT with a TSS descriptor
  tss.c/.h         # TSS struct + one IST stack, loaded via ltr
  idt.c/.h         # 256-entry IDT, idt_set_gate, exception name table
  isr.asm          # 32 exception stubs + common stub; IRQ stubs + common stub
  isr.c/.h         # C-level dispatch, register-frame struct, per-exception handlers
  acpi.c/.h        # RSDP scan -> RSDT/XSDT -> MADT parsing
  lapic.c/.h       # Local APIC enable, EOI, periodic timer start
  ioapic.c/.h      # IOAPIC redirection table entries
  pit.c/.h         # one-shot calibration stopwatch only
  timer.c/.h       # ties PIT calibration + Local APIC timer together, tick counter
  keyboard.c/.h    # scancode-set-1 -> ASCII, IRQ handler
```

## Data flow (kmain)

```
serial_init
  -> vga_clear + banner
  -> gdt/tss install (extend GDT, build TSS with IST1, ltr)
  -> idt_init (32 exception vectors wired, 33-255 default handler)
  -> acpi_find_madt (RSDP -> RSDT/XSDT -> MADT)
  -> parse MADT (Local APIC address, IOAPIC address+GSI base, IRQ overrides)
  -> pic_disable (remap off exception range, then mask all lines)
  -> lapic_enable (spurious-interrupt vector register)
  -> pit_calibrate_lapic_timer (one-shot PIT stopwatch, never used again)
  -> lapic_timer_start_periodic (100Hz target)
  -> ioapic_route_keyboard (real GSI/polarity from override table)
  -> sti
  -> idle loop (hlt, now interruptible)
```

Timer IRQ handler: increments tick counter, periodically logs to serial.
Keyboard IRQ handler: reads scancode, translates via set-1 table, echoes
to serial + VGA.

## Testing / verification

No unit tests at this level (same reasoning as the boot milestone: this
is bare-metal code with no host runtime to run tests in). Verification
is via QEMU, serial log capture, and screendumps:

- **Timer:** run with `-serial file:<path>` (or an equivalent
  redirection), let it run several seconds, confirm periodic tick
  messages appear in the log at roughly the calibrated rate.
- **Keyboard:** use QEMU monitor's `sendkey` command to simulate
  keypresses, confirm the corresponding characters appear in the
  serial log and/or a VGA screendump.
- **Exception handling:** temporarily force a divide-by-zero (same
  "forced branch, verify, then revert" technique used for the CPUID
  check in the boot milestone), confirm a clean register dump appears
  on serial/VGA and the machine halts deterministically rather than
  triple-faulting/rebooting.
- **No spurious PIC activity / stability:** let the kernel run for
  several seconds under continuous timer ticks with no crash or
  unexpected reboot.

## Error handling

Every one of the 32 exception handlers dumps a full register frame
(plus `CR2` and the decoded error code where the vector provides one)
to serial and a short banner to VGA, then halts via a deterministic
`cli`/`hlt` loop. This matches the existing project convention
(established by the boot milestone's CPUID failure path): visible,
deterministic failure, never a silent triple fault. Vector 8 (double
fault) additionally runs on its own IST stack so a corrupted kernel
stack doesn't turn a diagnosable fault into an unexplained reset.
