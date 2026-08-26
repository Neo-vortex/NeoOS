# NeoOS Storage Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give NeoOS a read-only storage path: a polled ATA/IDE PIO driver for the primary channel's master drive, and a FAT16 filesystem parser supporting nested directories, so files can be looked up by path and read fully into memory.

**Architecture:** A new build step creates a FAT16-formatted raw disk image (via the host's `mkfs.fat`/`mtools`, no loopback mount needed) containing known test files, including a subdirectory. `kernel/ata.c` talks to the primary IDE channel's fixed ports to read raw sectors. `kernel/fat16.c` parses the boot sector once (`fat16_mount`), then offers path lookup (`fat16_find`, walking the fixed-size root region and/or subdirectory cluster chains as needed) and full-file reads (`fat16_read_file`, walking the FAT cluster chain). `kmain` wires both in and runs a self-test that reads a root file, a multi-cluster root file, a nested file, and confirms a nonexistent path fails cleanly.

**Tech Stack:** Same toolchain as prior milestones (NASM, `x86_64-elf-gcc` cross-compiler, GNU Make, GRUB, QEMU), plus `dosfstools` (`mkfs.fat`) and `mtools` (`mcopy`, `mmd`) on the host for building the disk image — both already installed in this environment.

**Spec:** `docs/superpowers/specs/2026-08-26-storage-milestone-design.md`

## Global Constraints

- Freestanding C (`-ffreestanding -nostdlib`), no libc — same as prior milestones.
- Read-only: no write path anywhere in this milestone. No FAT12/FAT32, no partition table (the disk image is an unpartitioned "superfloppy" FAT16 volume starting at LBA 0), no long filenames (8.3 only), no AHCI/secondary channel/multiple drives.
- The disk image is a **separate QEMU drive** (`-drive file=...,format=raw`) alongside the existing `-cdrom` — it has no interaction with the boot/GRUB/higher-half machinery from milestones 1-3.
- Disk image layout (fixed for this milestone, built by a new Makefile target): a 32MiB raw image, FAT16, containing `/HELLO.TXT` (24 bytes, text), `/BIGFILE.TXT` (8192 bytes, spans exactly 4 clusters at this volume's 2048-byte cluster size — this is what exercises multi-cluster FAT chain-walking), and `/DIR/NESTED.TXT` (21 bytes, text) in a subdirectory.
- All new kernel sources (`ata.c/.h`, `fat16.c/.h`) go directly in `kernel/` (not `kernel/mm/`) — the existing `kernel/*.c` wildcard in the Makefile already picks them up; no Makefile source-discovery changes are needed, only the new disk-image target and updating `run` to attach the drive.
- Verification throughout uses headless QEMU exactly as in prior milestones: `-serial file:<path>` for grep-able diagnostics, plus host-side `mdir`/`mtype` (from `mtools`) to independently verify the disk image's own contents before any kernel code touches it.

---

### Task 1: Disk Image Build

**Files:**
- Modify: `Makefile` (new `disk-image` target and `DISK_IMG`/`DISK_SRC` variables; `run` updated to attach the drive)

**Interfaces:**
- Produces: `build/disk.img` (a 32MiB FAT16 raw image with `/HELLO.TXT`, `/BIGFILE.TXT`, `/DIR/NESTED.TXT`), and a `disk-image` Make target. Tasks 2-4 depend on this file existing before booting.

- [ ] **Step 1: Verify host tooling**

Run: `which mkfs.fat mcopy mmd mdir mtype`
Expected: all five resolve to a path (this environment already has `dosfstools` and `mtools` installed; if any are missing on a different machine, install `dosfstools` and `mtools` via the system package manager before continuing).

- [ ] **Step 2: Add the disk image Makefile target**

```makefile
DISK_IMG := $(BUILD_DIR)/disk.img
DISK_SRC := $(BUILD_DIR)/disk-src

$(DISK_IMG):
	mkdir -p $(DISK_SRC)/DIR
	printf 'Hello from NeoOS FAT16!\n' > $(DISK_SRC)/HELLO.TXT
	head -c 8192 /dev/zero | tr '\0' 'N' > $(DISK_SRC)/BIGFILE.TXT
	printf 'nested file contents\n' > $(DISK_SRC)/DIR/NESTED.TXT
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=32 status=none
	mkfs.fat -F 16 $(DISK_IMG)
	mcopy -i $(DISK_IMG) $(DISK_SRC)/HELLO.TXT ::HELLO.TXT
	mcopy -i $(DISK_IMG) $(DISK_SRC)/BIGFILE.TXT ::BIGFILE.TXT
	mmd -i $(DISK_IMG) ::DIR
	mcopy -i $(DISK_IMG) $(DISK_SRC)/DIR/NESTED.TXT ::DIR/NESTED.TXT

disk-image: $(DISK_IMG)
```

Add `disk-image` to the `.PHONY` line (it must always re-run when asked, since `$(DISK_IMG)` has no dependency that would otherwise trigger a rebuild):

```makefile
.PHONY: all build iso run clean disk-image
```

- [ ] **Step 3: Update `run` to attach the drive**

```makefile
run: iso disk-image
	qemu-system-x86_64 -cdrom $(BUILD_DIR)/neoos.iso -drive file=$(DISK_IMG),format=raw
```

- [ ] **Step 4: Build and verify the image independently of the kernel**

Run: `make clean && make disk-image`
Then inspect it with the host's own FAT tools (no kernel/QEMU involved yet):
```bash
mdir -i build/disk.img ::
mdir -i build/disk.img ::DIR
mtype -i build/disk.img ::HELLO.TXT
```
Expected: root listing shows `HELLO.TXT` (24 bytes), `BIGFILE.TXT` (8192 bytes), and a `DIR` subdirectory; `DIR`'s listing shows `NESTED.TXT` (21 bytes); `mtype` of `HELLO.TXT` prints `Hello from NeoOS FAT16!`.

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "Add FAT16 disk image build for storage milestone"
```

---

### Task 2: ATA/IDE PIO Driver

**Files:**
- Modify: `kernel/io.h` (add `outw`/`inw` for 16-bit port I/O)
- Create: `kernel/ata.h`
- Create: `kernel/ata.c`
- Modify: `kernel/kernel.c` (wire in `ata_identify`)

**Interfaces:**
- Consumes: `outb`/`inb` (existing), new `outw`/`inw` (this task), `serial_write_string`/`serial_write_hex64` (existing).
- Produces: `#define ATA_SECTOR_SIZE 512`, `struct ata_identify_info { uint32_t sector_count; }`, `int ata_identify(struct ata_identify_info *info)`, `int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer)` (`count` must be 1-255; both return 1 on success, 0 on failure, logged to serial). Task 3 (`fat16.c`) calls `ata_read_sectors` directly.

- [ ] **Step 1: Add 16-bit port I/O to `io.h`**

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

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" :: "a"((uint8_t)0));
}

#endif
```

- [ ] **Step 2: Write the ATA driver header**

```c
#ifndef NEOOS_ATA_H
#define NEOOS_ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

struct ata_identify_info {
    uint32_t sector_count;
};

// Issues IDENTIFY DEVICE to the primary channel's master drive. Returns
// 1 on success (info->sector_count filled in), 0 on failure (logged).
int ata_identify(struct ata_identify_info *info);

// Reads `count` (1-255) sectors starting at `lba` into `buffer`, which
// must be at least count * ATA_SECTOR_SIZE bytes. Returns 1 on success,
// 0 on failure (logged to serial).
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);

#endif
```

- [ ] **Step 3: Write the ATA driver**

```c
#include "ata.h"
#include "io.h"
#include "serial.h"

#define ATA_DATA        0x1F0
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READ_SECTORS  0x20

#define ATA_POLL_MAX_ITERATIONS 100000

// Bounded poll -- a drive that never reaches the requested status
// within this many reads is treated as a hardware failure, logged and
// reported to the caller, rather than hanging forever.
static int ata_wait_status(uint8_t mask, uint8_t value) {
    for (uint32_t i = 0; i < ATA_POLL_MAX_ITERATIONS; i++) {
        if ((inb(ATA_STATUS) & mask) == value) {
            return 1;
        }
    }
    return 0;
}

int ata_identify(struct ata_identify_info *info) {
    outb(ATA_DRIVE_HEAD, 0xA0); // master drive
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(ATA_STATUS) == 0) {
        serial_write_string("[ata] identify FAILED: no drive present\n");
        return 0;
    }
    if (!ata_wait_status(ATA_STATUS_BSY, 0)) {
        serial_write_string("[ata] identify FAILED: BSY never cleared\n");
        return 0;
    }
    if (!ata_wait_status(ATA_STATUS_DRQ, ATA_STATUS_DRQ)) {
        serial_write_string("[ata] identify FAILED: DRQ never set\n");
        return 0;
    }

    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(ATA_DATA);
    }

    info->sector_count = (uint32_t)identify_data[61] << 16 | identify_data[60];

    serial_write_string("[ata] drive identified, sectors=");
    serial_write_hex64(info->sector_count);
    serial_write_string(" (");
    serial_write_hex64((uint64_t)info->sector_count * ATA_SECTOR_SIZE / (1024 * 1024));
    serial_write_string(" MiB)\n");
    return 1;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    uint16_t *out = (uint16_t *)buffer;

    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); // LBA mode, master drive
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);

    for (uint8_t s = 0; s < count; s++) {
        if (!ata_wait_status(ATA_STATUS_BSY, 0)) {
            serial_write_string("[ata] read FAILED: BSY never cleared\n");
            return 0;
        }
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_STATUS_ERR) {
            serial_write_string("[ata] read FAILED: ERR bit set\n");
            return 0;
        }
        if (!(status & ATA_STATUS_DRQ) && !ata_wait_status(ATA_STATUS_DRQ, ATA_STATUS_DRQ)) {
            serial_write_string("[ata] read FAILED: DRQ never set\n");
            return 0;
        }

        for (int i = 0; i < 256; i++) {
            out[(uint32_t)s * 256 + i] = inw(ATA_DATA);
        }
    }
    return 1;
}
```

- [ ] **Step 4: Wire into `kmain`**

```c
#include "mm/heap.h"
#include "ata.h"

/* ... */

    heap_init();
    heap_selftest();

    struct ata_identify_info ata_info;
    ata_identify(&ata_info);

    serial_write_string("NeoOS: interrupts enabled, entering idle loop\n");
```

- [ ] **Step 5: Build and verify**

Run: `make clean && make disk-image && make iso`, then boot with the drive attached and serial redirected:
```bash
qemu-system-x86_64 -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown
```
Expected: `grep '\[ata\]' /tmp/neoos.log` shows `[ata] drive identified, sectors=0x0000000000010000 (0x0000000000000020 MiB)` (65536 sectors = 32MiB, matching the image built in Task 1), with all prior milestones' log lines still present and no exceptions.

- [ ] **Step 6: Commit**

```bash
git add kernel/io.h kernel/ata.c kernel/ata.h kernel/kernel.c
git commit -m "Add ATA/IDE PIO driver"
```

---

### Task 3: FAT16 Filesystem (Mount, Path Lookup, File Read) and Self-Test

**Files:**
- Create: `kernel/fat16.h`
- Create: `kernel/fat16.c`
- Modify: `kernel/kernel.c` (wire in `fat16_mount`/`fat16_selftest`)

**Interfaces:**
- Consumes: `ata_read_sectors` (Task 2), `kmalloc`/`kfree` (milestone 3's `mm/heap.h` — the self-test's read buffer is heap-allocated, not stack-allocated, since the kernel is still running on the single 16KiB boot stack from `boot.asm` and an 8KiB local array would risk overflowing it), `serial_write_string`/`serial_write_hex64`.
- Produces: `int fat16_mount(void)`, `void fat16_selftest(void)`, `int fat16_find(const char *path, uint16_t *out_cluster, uint32_t *out_size)`, `uint32_t fat16_read_file(uint16_t first_cluster, uint32_t size, void *buffer)`.

- [ ] **Step 1: Write the FAT16 header**

```c
#ifndef NEOOS_FAT16_H
#define NEOOS_FAT16_H

#include <stdint.h>

int fat16_mount(void);
void fat16_selftest(void);

// Looks up a path like "/DIR/FILE.TXT" (8.3 components only). On
// success returns 1 and fills *out_cluster/*out_size; on failure (any
// path component not found, or a non-directory component used as a
// directory) returns 0.
int fat16_find(const char *path, uint16_t *out_cluster, uint32_t *out_size);

// Reads a file's full contents (given the cluster/size fat16_find
// returned) into buffer, which must be at least size bytes. Returns
// the number of bytes actually read (equals size on success).
uint32_t fat16_read_file(uint16_t first_cluster, uint32_t size, void *buffer);

#endif
```

- [ ] **Step 2: Write the mount logic and on-disk structures**

```c
#include "fat16.h"
#include "ata.h"
#include "serial.h"

#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_LONG_NAME 0x0F
#define FAT16_EOC_MIN      0xFFF8

#define SECTOR_SIZE 512

struct fat16_bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} __attribute__((packed));

struct fat16_dirent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_high; // unused in FAT16
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed));

#define DIRENTS_PER_SECTOR (SECTOR_SIZE / sizeof(struct fat16_dirent))

static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint32_t fat_start_lba;
static uint32_t root_dir_start_lba;
static uint32_t root_dir_sector_count;
static uint32_t data_start_lba;
static uint16_t root_entry_count;

static uint32_t cluster_to_lba(uint16_t cluster) {
    return data_start_lba + (uint32_t)(cluster - 2) * sectors_per_cluster;
}

int fat16_mount(void) {
    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(0, 1, sector)) {
        serial_write_string("[fat16] mount FAILED: could not read boot sector\n");
        return 0;
    }

    struct fat16_bpb *bpb = (struct fat16_bpb *)sector;
    bytes_per_sector = bpb->bytes_per_sector;
    sectors_per_cluster = bpb->sectors_per_cluster;
    root_entry_count = bpb->root_entry_count;

    fat_start_lba = bpb->reserved_sector_count;
    root_dir_start_lba = fat_start_lba + (uint32_t)bpb->num_fats * bpb->sectors_per_fat;
    root_dir_sector_count = ((uint32_t)root_entry_count * sizeof(struct fat16_dirent) + bytes_per_sector - 1) / bytes_per_sector;
    data_start_lba = root_dir_start_lba + root_dir_sector_count;

    serial_write_string("[fat16] mounted: bytes_per_sector=");
    serial_write_hex64(bytes_per_sector);
    serial_write_string(" sectors_per_cluster=");
    serial_write_hex64(sectors_per_cluster);
    serial_write_string(" root_dir_lba=");
    serial_write_hex64(root_dir_start_lba);
    serial_write_string(" data_start_lba=");
    serial_write_hex64(data_start_lba);
    serial_write_string("\n");
    return 1;
}
```

- [ ] **Step 3: Write cluster-chain reading**

```c
static uint16_t fat16_next_cluster(uint16_t cluster) {
    uint32_t fat_offset = (uint32_t)cluster * 2;
    uint32_t fat_sector = fat_start_lba + fat_offset / bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    ata_read_sectors(fat_sector, 1, sector);
    uint16_t *entries = (uint16_t *)sector;
    return entries[offset_in_sector / 2];
}

uint32_t fat16_read_file(uint16_t first_cluster, uint32_t size, void *buffer) {
    uint8_t *out = (uint8_t *)buffer;
    uint32_t bytes_read = 0;
    uint16_t cluster = first_cluster;
    uint8_t sector_buf[SECTOR_SIZE];

    while (cluster < FAT16_EOC_MIN && bytes_read < size) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < sectors_per_cluster && bytes_read < size; s++) {
            ata_read_sectors(lba + s, 1, sector_buf);
            uint32_t to_copy = size - bytes_read;
            if (to_copy > bytes_per_sector) {
                to_copy = bytes_per_sector;
            }
            for (uint32_t i = 0; i < to_copy; i++) {
                out[bytes_read + i] = sector_buf[i];
            }
            bytes_read += to_copy;
        }
        cluster = fat16_next_cluster(cluster);
    }
    return bytes_read;
}
```

- [ ] **Step 4: Write name matching and directory scanning**

```c
static void to_fat_name(const char *name, uint8_t *out11) {
    for (int i = 0; i < 11; i++) {
        out11[i] = ' ';
    }
    int out_i = 0;
    int i = 0;
    while (name[i] != '\0' && name[i] != '.' && out_i < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out11[out_i] = (uint8_t)c;
        out_i++;
        i++;
    }
    while (name[i] != '\0' && name[i] != '.') {
        i++; // skip name characters beyond 8 -- 8.3 only, per this milestone's scope
    }
    if (name[i] == '.') {
        i++;
        int ext_i = 8;
        while (name[i] != '\0' && ext_i < 11) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            out11[ext_i] = (uint8_t)c;
            ext_i++;
            i++;
        }
    }
}

static int fat_name_matches(const uint8_t *entry_name, const uint8_t *target_name) {
    for (int i = 0; i < 11; i++) {
        if (entry_name[i] != target_name[i]) {
            return 0;
        }
    }
    return 1;
}

// Scans one sector's worth of directory entries for target_name.
// Returns 1 (found, *out filled), 0 (not found in this sector, keep
// scanning), or -1 (hit the end-of-directory marker, stop entirely).
static int scan_sector_for_name(const uint8_t *sector, const uint8_t *target_name, struct fat16_dirent *out) {
    const struct fat16_dirent *entries = (const struct fat16_dirent *)sector;
    for (uint32_t e = 0; e < DIRENTS_PER_SECTOR; e++) {
        if (entries[e].name[0] == 0x00) {
            return -1;
        }
        if (entries[e].name[0] == 0xE5) {
            continue; // deleted entry
        }
        if ((entries[e].attr & FAT_ATTR_LONG_NAME) == FAT_ATTR_LONG_NAME) {
            continue; // long-filename entry -- not supported, 8.3 only
        }
        if (entries[e].attr & FAT_ATTR_VOLUME_ID) {
            continue;
        }
        if (fat_name_matches(entries[e].name, target_name)) {
            *out = entries[e];
            return 1;
        }
    }
    return 0;
}

static int find_in_root(const uint8_t *target_name, struct fat16_dirent *out) {
    uint8_t sector[SECTOR_SIZE];
    for (uint32_t s = 0; s < root_dir_sector_count; s++) {
        ata_read_sectors(root_dir_start_lba + s, 1, sector);
        int result = scan_sector_for_name(sector, target_name, out);
        if (result != 0) {
            return result > 0;
        }
    }
    return 0;
}

static int find_in_directory_cluster(uint16_t dir_cluster, const uint8_t *target_name, struct fat16_dirent *out) {
    uint8_t sector[SECTOR_SIZE];
    uint16_t cluster = dir_cluster;
    while (cluster < FAT16_EOC_MIN) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < sectors_per_cluster; s++) {
            ata_read_sectors(lba + s, 1, sector);
            int result = scan_sector_for_name(sector, target_name, out);
            if (result != 0) {
                return result > 0;
            }
        }
        cluster = fat16_next_cluster(cluster);
    }
    return 0;
}
```

- [ ] **Step 5: Write path resolution**

```c
int fat16_find(const char *path, uint16_t *out_cluster, uint32_t *out_size) {
    if (path[0] == '/') {
        path++;
    }
    if (*path == '\0') {
        return 0; // empty path (or just "/") is not a valid file lookup
    }

    struct fat16_dirent entry;
    int in_root = 1;
    uint16_t current_dir_cluster = 0;

    while (*path != '\0') {
        char component[13]; // 8 + '.' + 3 + NUL -- 8.3 only
        int i = 0;
        while (path[i] != '\0' && path[i] != '/' && i < 12) {
            component[i] = path[i];
            i++;
        }
        component[i] = '\0';

        uint8_t fat_name[11];
        to_fat_name(component, fat_name);

        int found = in_root ? find_in_root(fat_name, &entry)
                             : find_in_directory_cluster(current_dir_cluster, fat_name, &entry);
        if (!found) {
            return 0;
        }

        path += i;
        if (*path == '/') {
            path++;
            if (!(entry.attr & FAT_ATTR_DIRECTORY)) {
                return 0; // tried to descend into a non-directory
            }
            current_dir_cluster = entry.first_cluster_low;
            in_root = 0;
        }
    }

    *out_cluster = entry.first_cluster_low;
    *out_size = entry.file_size;
    return 1;
}
```

- [ ] **Step 6: Write the self-test**

```c
#include "mm/heap.h"

static int buffer_equals_string(const uint8_t *buffer, uint32_t len, const char *expected) {
    for (uint32_t i = 0; i < len; i++) {
        if ((char)buffer[i] != expected[i]) {
            return 0;
        }
    }
    return expected[len] == '\0';
}

void fat16_selftest(void) {
    uint16_t cluster;
    uint32_t size;
    uint8_t *buffer = (uint8_t *)kmalloc(8192);
    if (!buffer) {
        serial_write_string("[fat16] selftest FAILED: kmalloc returned NULL\n");
        return;
    }

    if (!fat16_find("/HELLO.TXT", &cluster, &size)) {
        serial_write_string("[fat16] selftest FAILED: /HELLO.TXT not found\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size ||
        !buffer_equals_string(buffer, size, "Hello from NeoOS FAT16!\n")) {
        serial_write_string("[fat16] selftest FAILED: /HELLO.TXT contents mismatch\n");
        return;
    }

    if (!fat16_find("/BIGFILE.TXT", &cluster, &size) || size != 8192) {
        serial_write_string("[fat16] selftest FAILED: /BIGFILE.TXT not found or wrong size\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size) {
        serial_write_string("[fat16] selftest FAILED: /BIGFILE.TXT short read\n");
        return;
    }
    for (uint32_t i = 0; i < size; i++) {
        if (buffer[i] != 'N') {
            serial_write_string("[fat16] selftest FAILED: /BIGFILE.TXT byte mismatch at offset ");
            serial_write_hex64(i);
            serial_write_string("\n");
            return;
        }
    }

    if (!fat16_find("/DIR/NESTED.TXT", &cluster, &size)) {
        serial_write_string("[fat16] selftest FAILED: /DIR/NESTED.TXT not found\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size ||
        !buffer_equals_string(buffer, size, "nested file contents\n")) {
        serial_write_string("[fat16] selftest FAILED: /DIR/NESTED.TXT contents mismatch\n");
        return;
    }

    if (fat16_find("/DIR/MISSING.TXT", &cluster, &size)) {
        serial_write_string("[fat16] selftest FAILED: /DIR/MISSING.TXT should not be found\n");
        return;
    }

    kfree(buffer);
    serial_write_string("[fat16] selftest passed\n");
}
```

- [ ] **Step 7: Wire into `kmain`**

```c
#include "fat16.h"

/* ... */

    struct ata_identify_info ata_info;
    ata_identify(&ata_info);

    fat16_mount();
    fat16_selftest();

    serial_write_string("NeoOS: interrupts enabled, entering idle loop\n");
```

- [ ] **Step 8: Build and verify**

Run: `make clean && make disk-image && make iso`, then boot as in Task 2's Step 5.
Expected: `[fat16] mounted: bytes_per_sector=0x200 sectors_per_cluster=0x4 root_dir_lba=... data_start_lba=...` followed by `[fat16] selftest passed`, with no `FAILED` lines and no exceptions.

- [ ] **Step 9: Commit**

```bash
git add kernel/fat16.c kernel/fat16.h kernel/kernel.c
git commit -m "Add FAT16 filesystem: mount, path lookup, and file reading"
```

---

### Task 4: Final Integration and Full Verification

**Files:**
- None (verification-only task; fixes anything Steps 1-2 turn up).

**Interfaces:** None new — this task exercises everything Tasks 1-3 produced.

- [ ] **Step 1: Full boot log check**

Run: `make clean && make disk-image && make iso`, then:
```bash
rm -f /tmp/neoos.log
qemu-system-x86_64 -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown -d int,guest_errors -D /tmp/qemu-int.log &
sleep 8
kill %1 2>/dev/null
grep -c FAILED /tmp/neoos.log
grep -c check_exception /tmp/qemu-int.log
cat /tmp/neoos.log
```
Expected: `0` for both `FAILED` and `check_exception` counts. The log shows, in order, everything from milestones 2-3 (unchanged), then `[ata] drive identified, sectors=0x10000 (0x20 MiB)`, `[fat16] mounted: ...`, `[fat16] selftest passed`, then the existing `NeoOS: interrupts enabled, entering idle loop` and periodic timer ticks.

- [ ] **Step 2: Regression check without the disk attached**

Run the same boot command but omitting `-drive file=build/disk.img,format=raw`.
Expected: `[ata] identify FAILED: no drive present` (or equivalent) is logged, `fat16_mount`/`fat16_selftest` fail cleanly (logged `FAILED`, not a crash or hang), and the kernel continues on to finish booting and idle — a missing disk must not prevent the rest of the kernel (milestones 2-3's functionality) from working, since storage is a new, independent subsystem, not a boot dependency.

- [ ] **Step 3: Commit**

Only if Steps 1-2 required fixes; otherwise this task produces no diff and needs no commit. If fixes were needed:

```bash
git add -A
git commit -m "Fix storage milestone integration issues found during full verification"
```
