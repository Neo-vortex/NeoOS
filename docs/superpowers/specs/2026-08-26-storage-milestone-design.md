# NeoOS — Milestone 4: Storage (ATA/IDE PIO Driver + Read-Only FAT16)

## Goal

Give NeoOS its first persistent storage path: a polled ATA/IDE driver
for the primary channel's master drive, and a read-only FAT16
filesystem parser that supports nested directories, so files can be
looked up by path and read fully into memory. This is milestone 4. It
exists independently of process management — verified entirely on its
own, with no scheduler, syscalls, or user mode involved — because
milestone 5 (processes) will need *some* way to load ELF executables
by name, and this is that way. Splitting it out lets the storage stack
be built and debugged in isolation before anything is layered on it.

## Success criteria

Booting `neoos.iso` in QEMU with a second drive attached (a FAT16
raw disk image built as part of the project's build process) and:
- The ATA driver identifies the master drive and logs its parameters
  to serial without error.
- `fat16_mount` parses the boot sector/BPB and logs the derived
  volume layout (cluster size, root directory location, data region
  start) to serial.
- A lookup for a known multi-component path (e.g. `/DIR/FILE.TXT`)
  succeeds, proving subdirectory traversal works, not just root-level
  lookup.
- Reading that file's full contents (large enough to span multiple
  clusters, so the FAT chain-walking path is actually exercised, not
  just a single-cluster shortcut) produces bytes that match the
  file's known contents exactly, verified byte-for-byte and logged to
  serial.
- A lookup for a path that doesn't exist returns a clean "not found"
  result — no crash, no hang.

## Out of scope (future work)

- Any write support — create, delete, modify, or resize files, or
  update FAT/directory entries. Strictly read-only.
- FAT12 and FAT32 — only FAT16.
- A partition table (MBR). The disk image is formatted as a single
  FAT16 volume starting at LBA 0 with no partition table (the
  "superfloppy" convention `mkfs.fat` supports directly) — parsing a
  partition table to find a filesystem's starting LBA is deferred
  until something actually needs multiple partitions.
- Long filenames (VFAT LFN entries) — only 8.3 short filenames.
- AHCI/SATA, NVMe, USB storage — PIO ATA/IDE only.
- Multiple drives or the secondary ATA channel — primary channel,
  master drive only.
- Caching of any kind — every read goes to the (virtual) disk fresh.
- Syscall-level file access (`open`/`read` from user mode) — that's
  milestone 5's concern, once a process exists to call it. This
  milestone's API is kernel-internal only.

## Architecture

### ATA/IDE PIO driver

Uses the primary IDE channel's fixed I/O ports (command block at
`0x1F0`-`0x1F7`, control block at `0x3F6`), talking to the master
drive (drive/head register bit 4 = 0) via 28-bit LBA addressing.
`ata_identify()` issues the `IDENTIFY DEVICE` command and logs the
drive's reported sector count and model string to serial — mostly a
diagnostic/hygiene step, matching how milestone 2 logged each hardware
subsystem's discovered parameters (ACPI, LAPIC, IOAPIC) before using
it. `ata_read_sectors(lba, count, buffer)` selects the drive and LBA,
issues `READ SECTORS` (`0x20`), and for each sector: polls the status
port for `BSY` clear and `DRQ` set (bounded — a poll that never
resolves logs an error and halts, consistent with this project's
"fail loudly, never hang silently" convention rather than spinning
forever on hardware that isn't responding), then reads 256 16-bit
words from the data port.

### FAT16 layout

`fat16_mount` parses the boot sector's BPB once: bytes per sector,
sectors per cluster, reserved sector count, number of FATs, root
entry count, and sectors per FAT. From these it derives the FAT
region's start LBA, the root directory region's start LBA and size,
and the data region's start LBA (cluster-to-LBA conversion:
`data_start + (cluster - 2) * sectors_per_cluster`).

FAT16's root directory is a **fixed-size region**, not a cluster
chain — a flat array of 32-byte directory entries read directly from
its known LBA range. Subdirectories, by contrast, **are** ordinary
cluster chains (their directory entry's size field is 0), read via
the same cluster-chain-following mechanism as file contents, then
interpreted as more 32-byte directory entries. Path lookup has to
know which of these two it's reading at each step: the root component
comes from the fixed region, every subsequent directory component
comes from a cluster chain.

Each 32-byte directory entry holds an 8.3 name (11 bytes, space-padded
short name and extension), an attribute byte (bit 4 = directory; the
lookup skips volume-label and LFN entries), a 16-bit first cluster
number, and a 32-bit file size (0 for directories). The FAT table
itself holds one 16-bit entry per cluster: `0x0000` = free,
`0x0002`-`0xFFEF` = the next cluster in the chain, `0xFFF8`-`0xFFFF`
= end of chain. Reading a file's contents means walking this chain
from its first cluster, converting each cluster number to an LBA
range, and calling `ata_read_sectors` for each one.

Path lookup splits a path like `/DIR/FILE.TXT` on `/`, starts at the
root region, and for each component reads directory entries (from the
root region or the current directory's cluster chain, per the above),
matches the component against each entry's 8.3 name (case-insensitive,
normalized to FAT's padded short-name form), and either descends into
a matched subdirectory or, on the last component, returns the
matched file's first cluster and size. A component with no match at
any step returns a clean "not found," not an error condition.

## File structure

```
kernel/
  ata.c/.h    # ata_identify, ata_read_sectors
  fat16.c/.h  # fat16_mount, fat16_find(path) -> {cluster, size}, fat16_read_file(cluster, size, buffer)
```

## Data flow (kmain, extending milestone 3's sequence)

```
... existing milestone 2/3 sequence (interrupts, memory management) ...
  -> ata_identify()                          (log drive parameters)
  -> fat16_mount()                           (parse BPB, log volume layout)
  -> fat16_selftest()                        (find a known path, read it, verify contents, log pass/fail)
```

`fat16_selftest` follows the same pattern milestone 3 established for
`pmm_selftest`/`paging_selftest`/`heap_selftest`: exercise the
subsystem end-to-end and log a single pass/fail line, checkable the
same way via a QEMU serial-log grep.

## Testing / verification

Same approach as prior milestones — no host-runnable unit tests
(bare-metal, no host runtime); verification is via QEMU and serial
log capture:
- A new build step creates a fixed-size raw disk image, formats it
  FAT16 via the host's `mkfs.fat -F 16` (no partition table), and
  copies known test files — including a nested subdirectory — into it
  via `mtools`' `mcopy` (no loopback mount, no root privileges needed,
  matching the project's existing pattern of shelling out to host
  tools like `grub-mkrescue`).
- QEMU is given this image via a second `-drive
  file=disk.img,format=raw` alongside the existing `-cdrom`.
- Serial log is checked for the `[ata]` identify line, the `[fat16]`
  mount line, and `fat16_selftest`'s pass/fail line, plus a negative
  lookup (nonexistent path) logging a clean "not found" rather than a
  crash or hang.

## Error handling

`ata_read_sectors`'s status-poll loop is bounded; a drive that never
asserts `DRQ` within a reasonable number of polls logs an error to
serial and halts deterministically, rather than spinning forever —
same "visible, deterministic failure" convention established by the
boot milestone's CPUID check and carried through every milestone
since. `fat16_find` returns a sentinel "not found" result (rather than
crashing or returning garbage) for both a missing path component and
a path that runs past a non-directory entry (e.g. treating a file as
if it had subdirectories) — both are exercised by the success
criteria's negative-lookup test.
