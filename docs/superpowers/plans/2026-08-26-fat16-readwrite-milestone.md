# NeoOS Read-Write FAT16 Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the read-only FAT16 driver into a full read-write driver (create, write with random-access `lseek`, truncate, delete, `mkdir`), exposed to user-mode programs through a real per-process file descriptor table, a unified POSIX-style syscall surface, and distinct negative errno-style return codes.

**Architecture:** Three kernel layers, built bottom-up and each independently self-tested before the next depends on it: (1) `ata_write_sectors` + FAT16 cluster allocation, (2) FAT16 directory-entry creation/deletion/`mkdir`, (3) FAT16 file-content read/write at arbitrary byte offsets. On top of that, a per-task file descriptor table and seven syscalls (`open`/`close`/`read`/`write`(changed)/`mkdir`/`unlink`/`lseek`) guarded by a single global FS lock, exposed to userland via `fcntl.h`/`errno.h`/`unistd.h` additions to `libneoos.a`.

**Tech Stack:** Same toolchain as every prior milestone (NASM, `x86_64-elf-gcc` cross-compiler, GNU Make, QEMU, mtools).

**Spec:** `docs/superpowers/specs/2026-08-26-fat16-readwrite-design.md`

## Global Constraints

- Only FAT copy 1 is ever read or written; redundant copies `mkfs.fat` creates go stale (documented, not fixed).
- `mkdir` does not write `.`/`..` entries — nothing in this driver's lookup path uses them.
- Root directory entries live in a fixed-size, non-cluster-backed region; creating an entry there fails outright once full. Non-root directories are ordinary growable cluster chains.
- Writing past current EOF (via `lseek`) zero-fills the gap with real allocated clusters, not a logical sparse hole.
- `spawn`/`wait`/`getpid` are unaffected by this milestone and keep their existing plain `-1`-on-failure convention; only the new/changed file syscalls (`open`/`close`/`read`/`write`/`lseek`/`mkdir`/`unlink`) return specific negative `errno.h`-style codes.
- No mount points, `rmdir`, `rename`, long filenames, fd inheritance across `spawn`, or locking finer-grained than one global FS lock — all explicitly out of scope.
- Syscall argument pointers are not validated against the calling process's own memory — same tracked, deferred security gap as every prior milestone.
- Verification throughout uses headless QEMU exactly as in every prior milestone: `-boot order=d`, `-serial file:<path>`, `-no-reboot -no-shutdown -d int,guest_errors -D <path>`.

---

### Task 1: ATA Write Support and FAT16 Cluster Allocation

**Files:**
- Modify: `kernel/ata.c`, `kernel/ata.h` (add `ata_write_sectors`)
- Modify: `kernel/fat16.c`, `kernel/fat16.h` (add cluster allocation primitives, `sectors_per_fat_g`, and `fat16_write_selftest`)
- Modify: `kernel/kernel.c` (call `fat16_write_selftest()`)

**Interfaces:**
- Produces: `int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer)`; file-private `fat16_alloc_cluster()` → `uint16_t` (0 = disk full), `fat16_free_chain(uint16_t first_cluster)`, `fat16_set_next_cluster(uint16_t cluster, uint16_t value)` — all three stay `static` within `fat16.c`, consumed by Tasks 2-3 in the same file.
- Consumes: nothing new.

- [ ] **Step 1: Add `ata_write_sectors` to the ATA driver**

In `kernel/ata.h`, add after the existing `ata_read_sectors` declaration:

```c
// Writes `count` (1-255) sectors of `buffer` (must be at least count *
// ATA_SECTOR_SIZE bytes) to disk starting at `lba`, flushing the
// drive's cache before returning so the write is durable. Returns 1
// on success, 0 on failure (logged to serial).
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);
```

In `kernel/ata.c`, add after `ATA_CMD_READ_SECTORS`:

```c
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7
```

Then add, after `ata_read_sectors`:

```c
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    const uint16_t *in = (const uint16_t *)buffer;

    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); // LBA mode, master drive
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (uint8_t s = 0; s < count; s++) {
        if (!ata_wait_status(ATA_STATUS_BSY, 0)) {
            serial_write_string("[ata] write FAILED: BSY never cleared\n");
            return 0;
        }
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_STATUS_ERR) {
            serial_write_string("[ata] write FAILED: ERR bit set\n");
            return 0;
        }
        if (!(status & ATA_STATUS_DRQ) && !ata_wait_status(ATA_STATUS_DRQ, ATA_STATUS_DRQ)) {
            serial_write_string("[ata] write FAILED: DRQ never set\n");
            return 0;
        }

        for (int i = 0; i < 256; i++) {
            outw(ATA_DATA, in[(uint32_t)s * 256 + i]);
        }
    }

    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (!ata_wait_status(ATA_STATUS_BSY, 0)) {
        serial_write_string("[ata] write FAILED: cache flush BSY never cleared\n");
        return 0;
    }
    return 1;
}
```

- [ ] **Step 2: Track `sectors_per_fat` at mount time**

In `kernel/fat16.c`, add a new static alongside the existing ones:

```c
static uint16_t sectors_per_fat_g;
```

In `fat16_mount`, right after `root_entry_count = bpb->root_entry_count;`, add:

```c
    sectors_per_fat_g = bpb->sectors_per_fat;
```

- [ ] **Step 3: Add the FAT-entry write helper and cluster allocation/freeing**

In `kernel/fat16.c`, add right after the existing `fat16_next_cluster`:

```c
static void fat16_set_next_cluster(uint16_t cluster, uint16_t value) {
    uint32_t fat_offset = (uint32_t)cluster * 2;
    uint32_t fat_sector = fat_start_lba + fat_offset / bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    ata_read_sectors(fat_sector, 1, sector);
    uint16_t *entries = (uint16_t *)sector;
    entries[offset_in_sector / 2] = value;
    ata_write_sectors(fat_sector, 1, sector);
}

// Scans the FAT linearly from cluster 2 for a free (0x0000) entry,
// marks it as a fresh chain's end (0xFFFF), and returns it. Returns 0
// if the disk is full.
static uint16_t fat16_alloc_cluster(void) {
    uint32_t total_entries = ((uint32_t)sectors_per_fat_g * bytes_per_sector) / 2;
    for (uint16_t cluster = 2; cluster < total_entries; cluster++) {
        if (fat16_next_cluster(cluster) == 0x0000) {
            fat16_set_next_cluster(cluster, 0xFFFF);
            return cluster;
        }
    }
    return 0;
}

// Walks a cluster chain from first_cluster, zeroing every FAT entry.
static void fat16_free_chain(uint16_t first_cluster) {
    uint16_t cluster = first_cluster;
    while (cluster >= 2 && cluster < FAT16_EOC_MIN) {
        uint16_t next = fat16_next_cluster(cluster);
        fat16_set_next_cluster(cluster, 0x0000);
        cluster = next;
    }
}
```

- [ ] **Step 4: Add `fat16_write_selftest`**

In `kernel/fat16.h`, add after the `fat16_selftest` declaration:

```c
void fat16_write_selftest(void);
```

In `kernel/fat16.c`, add at the end of the file:

```c
void fat16_write_selftest(void) {
    uint16_t cluster = fat16_alloc_cluster();
    if (cluster == 0) {
        serial_write_string("[fat16] write selftest FAILED: alloc_cluster returned 0\n");
        return;
    }

    uint8_t write_buf[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }
    uint32_t lba = cluster_to_lba(cluster);
    if (!ata_write_sectors(lba, 1, write_buf)) {
        serial_write_string("[fat16] write selftest FAILED: ata_write_sectors failed\n");
        return;
    }

    uint8_t read_buf[SECTOR_SIZE];
    if (!ata_read_sectors(lba, 1, read_buf)) {
        serial_write_string("[fat16] write selftest FAILED: ata_read_sectors failed\n");
        return;
    }
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        if (read_buf[i] != write_buf[i]) {
            serial_write_string("[fat16] write selftest FAILED: byte mismatch at offset ");
            serial_write_hex64(i);
            serial_write_string("\n");
            return;
        }
    }

    if (fat16_next_cluster(cluster) < FAT16_EOC_MIN) {
        serial_write_string("[fat16] write selftest FAILED: newly allocated cluster not marked EOC\n");
        return;
    }

    fat16_free_chain(cluster);
    if (fat16_next_cluster(cluster) != 0x0000) {
        serial_write_string("[fat16] write selftest FAILED: freed cluster not zeroed in FAT\n");
        return;
    }

    serial_write_string("[fat16] write selftest passed\n");
}
```

- [ ] **Step 5: Call it from `kmain`**

In `kernel/kernel.c`, right after `fat16_selftest();`, add:

```c
    fat16_write_selftest();
```

- [ ] **Step 6: Build and verify**

Run: `make clean && make disk-image && make iso`, boot headlessly with `-boot order=d -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -serial file:/tmp/neoos.log -display none -no-reboot -no-shutdown -d int,guest_errors -D /tmp/qemu-int.log`.
Expected: `[fat16] write selftest passed` appears on serial, zero `FAILED` lines, zero exceptions in the QEMU int log, and every prior boot line (through milestone 6's parent/child/looper/yielder lifecycle) is unaffected.

- [ ] **Step 7: Commit**

```bash
git add kernel/ata.c kernel/ata.h kernel/fat16.c kernel/fat16.h kernel/kernel.c
git commit -m "Add ATA write support and FAT16 cluster allocation"
```

---

### Task 2: FAT16 Directory Entry Creation, Deletion, and mkdir

**Files:**
- Modify: `kernel/fat16.c`, `kernel/fat16.h` (`fat16_find` gains two out-parameters; add `resolve_parent`, `write_dirent`, `create_entry_in_directory`, `fat16_create_file`, `fat16_mkdir`, `fat16_delete_entry`, `fat16_update_entry_size`; add `kernel/errno.h` include)
- Create: `kernel/errno.h`
- Modify: `kernel/process.c:130` (update the one external `fat16_find` call site)

**Interfaces:**
- Consumes: `fat16_alloc_cluster`, `fat16_free_chain`, `fat16_set_next_cluster` (Task 1, same file).
- Produces: `int fat16_find(const char *path, uint16_t *out_cluster, uint32_t *out_size, uint32_t *out_dir_lba, uint16_t *out_dir_offset)` (signature change — the two new params are nullable); `int fat16_create_file(const char *path, uint32_t *out_dir_lba, uint16_t *out_dir_offset)`; `int fat16_mkdir(const char *path)`; `int fat16_delete_entry(const char *path)`; `void fat16_update_entry_size(uint32_t dir_lba, uint16_t dir_offset, uint16_t first_cluster, uint32_t size)` — all consumed by Task 3 (same file) and Task 4 (`syscall.c`). `kernel/errno.h`'s `ENOENT`/`EBADF`/`EEXIST`/`ENOTDIR`/`EISDIR`/`EINVAL`/`EMFILE`/`ENOSPC` constants, consumed by Tasks 2-4.

- [ ] **Step 1: Add `kernel/errno.h`**

```c
#ifndef NEOOS_KERNEL_ERRNO_H
#define NEOOS_KERNEL_ERRNO_H

// Kernel-side mirror of lib/include/errno.h's numeric values (the two
// trees don't share headers, so these are duplicated, not included --
// both use real Linux errno numbers for familiarity, with no need for
// binary compatibility with anything).

#define ENOENT  2
#define EBADF   9
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28

#endif
```

- [ ] **Step 2: Change `fat16_find`'s signature to report a matched entry's location**

In `kernel/fat16.c`, add `#include "errno.h"` to the top of the file (alongside the existing includes).

Replace `scan_sector_for_name` entirely:

```c
// Returns 1 (found, *out filled, *out_lba/*out_offset set to the
// entry's on-disk location, both nullable), 0 (not found in this
// sector, keep scanning), or -1 (hit the end-of-directory marker).
static int scan_sector_for_name(const uint8_t *sector, uint32_t sector_lba, const uint8_t *target_name,
                                  struct fat16_dirent *out, uint32_t *out_lba, uint16_t *out_offset) {
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
            if (out_lba) {
                *out_lba = sector_lba;
            }
            if (out_offset) {
                *out_offset = (uint16_t)(e * sizeof(struct fat16_dirent));
            }
            return 1;
        }
    }
    return 0;
}
```

Replace `find_in_root` entirely:

```c
static int find_in_root(const uint8_t *target_name, struct fat16_dirent *out,
                          uint32_t *out_lba, uint16_t *out_offset) {
    uint8_t sector[SECTOR_SIZE];
    for (uint32_t s = 0; s < root_dir_sector_count; s++) {
        uint32_t lba = root_dir_start_lba + s;
        ata_read_sectors(lba, 1, sector);
        int result = scan_sector_for_name(sector, lba, target_name, out, out_lba, out_offset);
        if (result != 0) {
            return result > 0;
        }
    }
    return 0;
}
```

Replace `find_in_directory_cluster` entirely:

```c
static int find_in_directory_cluster(uint16_t dir_cluster, const uint8_t *target_name, struct fat16_dirent *out,
                                       uint32_t *out_lba, uint16_t *out_offset) {
    uint8_t sector[SECTOR_SIZE];
    uint16_t cluster = dir_cluster;
    while (cluster < FAT16_EOC_MIN) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < sectors_per_cluster; s++) {
            ata_read_sectors(lba + s, 1, sector);
            int result = scan_sector_for_name(sector, lba + s, target_name, out, out_lba, out_offset);
            if (result != 0) {
                return result > 0;
            }
        }
        cluster = fat16_next_cluster(cluster);
    }
    return 0;
}
```

Replace `fat16_find` entirely:

```c
int fat16_find(const char *path, uint16_t *out_cluster, uint32_t *out_size,
               uint32_t *out_dir_lba, uint16_t *out_dir_offset) {
    if (path[0] == '/') {
        path++;
    }
    if (*path == '\0') {
        return 0; // empty path (or just "/") is not a valid file lookup
    }

    struct fat16_dirent entry;
    int in_root = 1;
    uint16_t current_dir_cluster = 0;
    uint32_t dir_lba = 0;
    uint16_t dir_offset = 0;

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

        int found = in_root ? find_in_root(fat_name, &entry, &dir_lba, &dir_offset)
                             : find_in_directory_cluster(current_dir_cluster, fat_name, &entry, &dir_lba, &dir_offset);
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
    if (out_dir_lba) {
        *out_dir_lba = dir_lba;
    }
    if (out_dir_offset) {
        *out_dir_offset = dir_offset;
    }
    return 1;
}
```

In `kernel/fat16.h`, replace the `fat16_find` declaration:

```c
// Looks up a path like "/DIR/FILE.TXT" (8.3 components only). On
// success returns 1 and fills *out_cluster/*out_size; if
// out_dir_lba/out_dir_offset are non-NULL, also fills them with the
// on-disk location of the matched directory entry (for callers that
// need to patch it later, e.g. after a write). On failure (any path
// component not found, or a non-directory component used as a
// directory) returns 0.
int fat16_find(const char *path, uint16_t *out_cluster, uint32_t *out_size,
               uint32_t *out_dir_lba, uint16_t *out_dir_offset);
```

- [ ] **Step 3: Fix the four in-file `fat16_find` calls and the one external caller**

In `kernel/fat16.c`'s `fat16_selftest`, change all four calls from e.g. `fat16_find("/HELLO.TXT", &cluster, &size)` to `fat16_find("/HELLO.TXT", &cluster, &size, NULL, NULL)` — do this for all four occurrences (`/HELLO.TXT`, `/BIGFILE.TXT`, `/DIR/NESTED.TXT`, `/DIR/MISSING.TXT`).

In `kernel/process.c`, line 130, change:

```c
    if (!fat16_find(path, &cluster, &size)) {
```

to:

```c
    if (!fat16_find(path, &cluster, &size, NULL, NULL)) {
```

- [ ] **Step 4: Add the directory-entry write helper and slot-creation engine**

In `kernel/fat16.c`, add after `find_in_directory_cluster`:

```c
static void write_dirent(struct fat16_dirent *entry, const uint8_t *fat_name, uint8_t attr,
                           uint16_t first_cluster, uint32_t size) {
    for (int i = 0; i < 11; i++) {
        entry->name[i] = fat_name[i];
    }
    entry->attr = attr;
    entry->nt_reserved = 0;
    entry->create_time_tenth = 0;
    entry->create_time = 0;
    entry->create_date = 0;
    entry->access_date = 0;
    entry->first_cluster_high = 0;
    entry->write_time = 0;
    entry->write_date = 0;
    entry->first_cluster_low = first_cluster;
    entry->file_size = size;
}

// Finds a free slot (0x00 never-used or 0xE5 deleted) in the given
// directory (in_root selects the fixed-size root directory over
// dir_cluster) and writes a new entry there. For a non-root directory
// that's completely full, allocates and links one more cluster before
// retrying. Returns 1 on success (fills *out_lba/*out_offset with the
// new entry's location), or -ENOSPC (root full, or disk full when a
// non-root directory needs to grow).
static int create_entry_in_directory(uint16_t dir_cluster, int in_root, const uint8_t *fat_name,
                                       uint8_t attr, uint16_t first_cluster, uint32_t size,
                                       uint32_t *out_lba, uint16_t *out_offset) {
    uint8_t sector[SECTOR_SIZE];

    if (in_root) {
        for (uint32_t s = 0; s < root_dir_sector_count; s++) {
            uint32_t lba = root_dir_start_lba + s;
            ata_read_sectors(lba, 1, sector);
            struct fat16_dirent *entries = (struct fat16_dirent *)sector;
            for (uint32_t e = 0; e < DIRENTS_PER_SECTOR; e++) {
                if (entries[e].name[0] == 0x00 || entries[e].name[0] == 0xE5) {
                    write_dirent(&entries[e], fat_name, attr, first_cluster, size);
                    ata_write_sectors(lba, 1, sector);
                    if (out_lba) {
                        *out_lba = lba;
                    }
                    if (out_offset) {
                        *out_offset = (uint16_t)(e * sizeof(struct fat16_dirent));
                    }
                    return 1;
                }
            }
        }
        return -ENOSPC;
    }

    uint16_t cluster = dir_cluster;
    uint16_t last_cluster = dir_cluster;
    while (cluster < FAT16_EOC_MIN) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < sectors_per_cluster; s++) {
            ata_read_sectors(lba + s, 1, sector);
            struct fat16_dirent *entries = (struct fat16_dirent *)sector;
            for (uint32_t e = 0; e < DIRENTS_PER_SECTOR; e++) {
                if (entries[e].name[0] == 0x00 || entries[e].name[0] == 0xE5) {
                    write_dirent(&entries[e], fat_name, attr, first_cluster, size);
                    ata_write_sectors(lba + s, 1, sector);
                    if (out_lba) {
                        *out_lba = lba + s;
                    }
                    if (out_offset) {
                        *out_offset = (uint16_t)(e * sizeof(struct fat16_dirent));
                    }
                    return 1;
                }
            }
        }
        last_cluster = cluster;
        cluster = fat16_next_cluster(cluster);
    }

    uint16_t new_cluster = fat16_alloc_cluster();
    if (new_cluster == 0) {
        return -ENOSPC;
    }
    fat16_set_next_cluster(last_cluster, new_cluster);

    uint8_t zero_sector[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        zero_sector[i] = 0;
    }
    uint32_t new_lba = cluster_to_lba(new_cluster);
    for (uint8_t s = 0; s < sectors_per_cluster; s++) {
        ata_write_sectors(new_lba + s, 1, zero_sector);
    }

    struct fat16_dirent *entries = (struct fat16_dirent *)zero_sector;
    write_dirent(&entries[0], fat_name, attr, first_cluster, size);
    ata_write_sectors(new_lba, 1, zero_sector);
    if (out_lba) {
        *out_lba = new_lba;
    }
    if (out_offset) {
        *out_offset = 0;
    }
    return 1;
}
```

- [ ] **Step 5: Add parent-path resolution**

In `kernel/fat16.c`, add after `create_entry_in_directory`:

```c
// Resolves the parent directory of `path` (its last '/'-separated
// component is the new name being created; everything before it must
// already exist and be a directory). On success (1), fills
// *out_in_root/*out_dir_cluster/*out_fat_name (11 bytes). On failure,
// returns -ENOENT (a parent component doesn't exist) or -ENOTDIR (a
// parent component exists but isn't a directory).
static int resolve_parent(const char *path, int *out_in_root, uint16_t *out_dir_cluster, uint8_t *out_fat_name) {
    if (path[0] == '/') {
        path++;
    }
    if (*path == '\0') {
        return -ENOENT;
    }

    int in_root = 1;
    uint16_t current_dir_cluster = 0;

    for (;;) {
        char component[13];
        int i = 0;
        while (path[i] != '\0' && path[i] != '/' && i < 12) {
            component[i] = path[i];
            i++;
        }
        component[i] = '\0';

        int is_last = (path[i] != '/');
        if (is_last) {
            to_fat_name(component, out_fat_name);
            *out_in_root = in_root;
            *out_dir_cluster = current_dir_cluster;
            return 1;
        }

        uint8_t fat_name[11];
        to_fat_name(component, fat_name);
        struct fat16_dirent entry;
        int found = in_root ? find_in_root(fat_name, &entry, NULL, NULL)
                             : find_in_directory_cluster(current_dir_cluster, fat_name, &entry, NULL, NULL);
        if (!found) {
            return -ENOENT;
        }
        if (!(entry.attr & FAT_ATTR_DIRECTORY)) {
            return -ENOTDIR;
        }
        current_dir_cluster = entry.first_cluster_low;
        in_root = 0;

        path += i + 1; // skip the '/'
    }
}
```

- [ ] **Step 6: Add the public create/mkdir/delete/update-entry functions**

In `kernel/fat16.c`, add after `resolve_parent`:

```c
int fat16_create_file(const char *path, uint32_t *out_dir_lba, uint16_t *out_dir_offset) {
    int in_root;
    uint16_t dir_cluster;
    uint8_t fat_name[11];
    int result = resolve_parent(path, &in_root, &dir_cluster, fat_name);
    if (result < 0) {
        return result;
    }

    struct fat16_dirent existing;
    int already_exists = in_root ? find_in_root(fat_name, &existing, NULL, NULL)
                                   : find_in_directory_cluster(dir_cluster, fat_name, &existing, NULL, NULL);
    if (already_exists) {
        return -EEXIST;
    }

    int created = create_entry_in_directory(dir_cluster, in_root, fat_name, 0, 0, 0, out_dir_lba, out_dir_offset);
    return created > 0 ? 0 : created;
}

int fat16_mkdir(const char *path) {
    int in_root;
    uint16_t dir_cluster;
    uint8_t fat_name[11];
    int result = resolve_parent(path, &in_root, &dir_cluster, fat_name);
    if (result < 0) {
        return result;
    }

    struct fat16_dirent existing;
    int already_exists = in_root ? find_in_root(fat_name, &existing, NULL, NULL)
                                   : find_in_directory_cluster(dir_cluster, fat_name, &existing, NULL, NULL);
    if (already_exists) {
        return -EEXIST;
    }

    uint16_t new_cluster = fat16_alloc_cluster();
    if (new_cluster == 0) {
        return -ENOSPC;
    }

    uint8_t zero_sector[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        zero_sector[i] = 0;
    }
    uint32_t lba = cluster_to_lba(new_cluster);
    for (uint8_t s = 0; s < sectors_per_cluster; s++) {
        ata_write_sectors(lba + s, 1, zero_sector);
    }

    uint32_t dir_lba;
    uint16_t dir_offset;
    int created = create_entry_in_directory(dir_cluster, in_root, fat_name, FAT_ATTR_DIRECTORY, new_cluster, 0, &dir_lba, &dir_offset);
    if (created <= 0) {
        fat16_free_chain(new_cluster);
        return created;
    }
    return 0;
}

int fat16_delete_entry(const char *path) {
    uint16_t cluster;
    uint32_t size;
    uint32_t dir_lba;
    uint16_t dir_offset;
    if (!fat16_find(path, &cluster, &size, &dir_lba, &dir_offset)) {
        return -ENOENT;
    }

    uint8_t sector[SECTOR_SIZE];
    ata_read_sectors(dir_lba, 1, sector);
    struct fat16_dirent *entry = (struct fat16_dirent *)(sector + dir_offset);
    if (entry->attr & FAT_ATTR_DIRECTORY) {
        return -EISDIR;
    }

    if (cluster != 0) {
        fat16_free_chain(cluster);
    }
    entry->name[0] = 0xE5;
    ata_write_sectors(dir_lba, 1, sector);
    return 0;
}

void fat16_update_entry_size(uint32_t dir_lba, uint16_t dir_offset, uint16_t first_cluster, uint32_t size) {
    uint8_t sector[SECTOR_SIZE];
    ata_read_sectors(dir_lba, 1, sector);
    struct fat16_dirent *entry = (struct fat16_dirent *)(sector + dir_offset);
    entry->first_cluster_low = first_cluster;
    entry->file_size = size;
    ata_write_sectors(dir_lba, 1, sector);
}
```

In `kernel/fat16.h`, add after the `fat16_find` declaration:

```c
// Creates a new, empty file at `path` (parent directory must already
// exist and not already contain this name). On success returns 0 and
// fills *out_dir_lba/*out_dir_offset with the new entry's location.
// On failure returns -ENOENT/-ENOTDIR (bad parent), -EEXIST (name
// taken), or -ENOSPC (directory or disk full).
int fat16_create_file(const char *path, uint32_t *out_dir_lba, uint16_t *out_dir_offset);

// Creates a new, empty directory at `path`. Same error codes as
// fat16_create_file.
int fat16_mkdir(const char *path);

// Deletes the file at `path`, freeing its cluster chain. Returns 0 on
// success, -ENOENT if not found, -EISDIR if it's a directory.
int fat16_delete_entry(const char *path);

// Patches an existing directory entry's first_cluster_low and
// file_size fields in place (used after a write() changes either).
void fat16_update_entry_size(uint32_t dir_lba, uint16_t dir_offset, uint16_t first_cluster, uint32_t size);
```

Also add `#include "errno.h"` to `kernel/fat16.h`'s includes (needed by any translation unit that checks these functions' negative return codes by name, matching the convention `errno.h`'s codes travel with the declarations that can return them).

- [ ] **Step 7: Extend `fat16_write_selftest` with directory entry checks**

In `kernel/fat16.c`'s `fat16_write_selftest`, replace the final `serial_write_string("[fat16] write selftest passed\n");` line with:

```c
    uint32_t size;
    if (fat16_find("/NEWFILE.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] write selftest FAILED: /NEWFILE.TXT already exists before creation\n");
        return;
    }
    if (fat16_create_file("/NEWFILE.TXT", NULL, NULL) != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_create_file(/NEWFILE.TXT) failed\n");
        return;
    }
    if (!fat16_find("/NEWFILE.TXT", &cluster, &size, NULL, NULL) || cluster != 0 || size != 0) {
        serial_write_string("[fat16] write selftest FAILED: /NEWFILE.TXT not found or not empty after creation\n");
        return;
    }
    if (fat16_create_file("/NEWFILE.TXT", NULL, NULL) != -EEXIST) {
        serial_write_string("[fat16] write selftest FAILED: creating /NEWFILE.TXT again did not return -EEXIST\n");
        return;
    }
    if (fat16_delete_entry("/NEWFILE.TXT") != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_delete_entry(/NEWFILE.TXT) failed\n");
        return;
    }
    if (fat16_find("/NEWFILE.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] write selftest FAILED: /NEWFILE.TXT still found after deletion\n");
        return;
    }
    if (fat16_delete_entry("/NEWFILE.TXT") != -ENOENT) {
        serial_write_string("[fat16] write selftest FAILED: deleting /NEWFILE.TXT again did not return -ENOENT\n");
        return;
    }

    if (fat16_mkdir("/NEWDIR") != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_mkdir(/NEWDIR) failed\n");
        return;
    }
    if (fat16_create_file("/NEWDIR/INNER.TXT", NULL, NULL) != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_create_file(/NEWDIR/INNER.TXT) failed\n");
        return;
    }
    if (!fat16_find("/NEWDIR/INNER.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] write selftest FAILED: /NEWDIR/INNER.TXT not found after creation\n");
        return;
    }

    serial_write_string("[fat16] write selftest passed\n");
```

- [ ] **Step 8: Build and verify**

Run: `make clean && make disk-image && make iso`, boot as in Task 1.
Expected: `[fat16] write selftest passed` still appears (now after exercising create/exists-check/delete/mkdir/nested-create), zero `FAILED`, zero exceptions, milestone 6's full lifecycle unaffected.

- [ ] **Step 9: Commit**

```bash
git add kernel/errno.h kernel/fat16.c kernel/fat16.h kernel/process.c
git commit -m "Add FAT16 directory entry creation, deletion, and mkdir"
```

---

### Task 3: FAT16 File Content Read/Write Engine

**Files:**
- Modify: `kernel/fat16.c`, `kernel/fat16.h` (add `cluster_at_offset`, `write_range`, `fat16_read_at`, `fat16_write_file`, `fat16_truncate`)

**Interfaces:**
- Consumes: `fat16_alloc_cluster`, `fat16_free_chain`, `fat16_set_next_cluster` (Task 1); `fat16_update_entry_size`, `ENOSPC` (Task 2) — all same file.
- Produces: `void fat16_read_at(uint16_t first_cluster, uint32_t position, void *buf, uint32_t len)`; `int fat16_write_file(uint16_t first_cluster, uint32_t current_size, uint32_t position, const void *buf, uint32_t len, uint16_t *out_first_cluster, uint32_t *out_new_size)`; `void fat16_truncate(uint16_t first_cluster, uint32_t dir_lba, uint16_t dir_offset, uint16_t *out_first_cluster)` — all consumed by Task 4 (`syscall.c`).

- [ ] **Step 1: Add `cluster_at_offset` and the shared range-write helper**

In `kernel/fat16.c`, add after `fat16_free_chain`:

```c
// Returns the cluster containing byte offset `byte_offset` within the
// chain starting at `first_cluster`, by walking from the start every
// call. O(chain length) per call -- acceptable at this project's file
// sizes; a future milestone could cache the last-accessed cluster per
// fd if it ever matters. Caller must ensure the chain is long enough.
static uint16_t cluster_at_offset(uint16_t first_cluster, uint32_t byte_offset) {
    uint32_t cluster_size_bytes = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint32_t cluster_index = byte_offset / cluster_size_bytes;
    uint16_t cluster = first_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        cluster = fat16_next_cluster(cluster);
    }
    return cluster;
}

// Writes `len` bytes into the cluster chain starting at chain_start,
// beginning at byte offset write_position. If zero_fill is set, zero
// bytes are written instead of reading from src (used for gap-filling
// past old EOF). Does read-modify-write for any partial sector.
// Assumes the chain already has enough clusters to cover
// write_position+len -- callers must extend it first.
static void write_range(uint16_t chain_start, uint32_t write_position, const void *src, uint32_t len, int zero_fill) {
    const uint8_t *in = (const uint8_t *)src;
    uint32_t cluster_size_bytes = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint32_t written = 0;

    while (written < len) {
        uint32_t abs_offset = write_position + written;
        uint32_t offset_in_cluster = abs_offset % cluster_size_bytes;
        uint32_t sector_index = offset_in_cluster / bytes_per_sector;
        uint32_t offset_in_sector = offset_in_cluster % bytes_per_sector;

        uint16_t cluster = cluster_at_offset(chain_start, abs_offset);
        uint32_t lba = cluster_to_lba(cluster) + sector_index;

        uint32_t to_write = bytes_per_sector - offset_in_sector;
        if (to_write > len - written) {
            to_write = len - written;
        }

        uint8_t sector[SECTOR_SIZE];
        if (offset_in_sector != 0 || to_write != bytes_per_sector) {
            ata_read_sectors(lba, 1, sector); // partial sector: preserve untouched bytes
        }
        for (uint32_t i = 0; i < to_write; i++) {
            sector[offset_in_sector + i] = zero_fill ? 0 : in[written + i];
        }
        ata_write_sectors(lba, 1, sector);

        written += to_write;
    }
}
```

- [ ] **Step 2: Add `fat16_read_at`**

In `kernel/fat16.c`, add after `write_range`:

```c
void fat16_read_at(uint16_t first_cluster, uint32_t position, void *buf, uint32_t len) {
    uint8_t *out = (uint8_t *)buf;
    uint32_t cluster_size_bytes = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint32_t read_so_far = 0;

    while (read_so_far < len) {
        uint32_t abs_offset = position + read_so_far;
        uint32_t offset_in_cluster = abs_offset % cluster_size_bytes;
        uint32_t sector_index = offset_in_cluster / bytes_per_sector;
        uint32_t offset_in_sector = offset_in_cluster % bytes_per_sector;

        uint16_t cluster = cluster_at_offset(first_cluster, abs_offset);
        uint32_t lba = cluster_to_lba(cluster) + sector_index;

        uint32_t to_read = bytes_per_sector - offset_in_sector;
        if (to_read > len - read_so_far) {
            to_read = len - read_so_far;
        }

        uint8_t sector[SECTOR_SIZE];
        ata_read_sectors(lba, 1, sector);
        for (uint32_t i = 0; i < to_read; i++) {
            out[read_so_far + i] = sector[offset_in_sector + i];
        }

        read_so_far += to_read;
    }
}
```

- [ ] **Step 3: Add `fat16_write_file` and `fat16_truncate`**

In `kernel/fat16.c`, add after `fat16_read_at`:

```c
int fat16_write_file(uint16_t first_cluster, uint32_t current_size, uint32_t position,
                      const void *buf, uint32_t len,
                      uint16_t *out_first_cluster, uint32_t *out_new_size) {
    if (len == 0) {
        *out_first_cluster = first_cluster;
        *out_new_size = current_size;
        return 0;
    }

    uint32_t cluster_size_bytes = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint32_t end_position = position + len;
    uint32_t clusters_needed = (end_position + cluster_size_bytes - 1) / cluster_size_bytes;

    uint16_t chain_start = first_cluster;
    uint16_t last_cluster = 0;
    uint32_t existing_clusters = 0;

    if (chain_start != 0) {
        uint16_t c = chain_start;
        existing_clusters = 1;
        while (fat16_next_cluster(c) < FAT16_EOC_MIN) {
            c = fat16_next_cluster(c);
            existing_clusters++;
        }
        last_cluster = c;
    }

    while (existing_clusters < clusters_needed) {
        uint16_t new_cluster = fat16_alloc_cluster();
        if (new_cluster == 0) {
            return -ENOSPC;
        }
        if (chain_start == 0) {
            chain_start = new_cluster;
        } else {
            fat16_set_next_cluster(last_cluster, new_cluster);
        }
        last_cluster = new_cluster;
        existing_clusters++;
    }

    if (position > current_size) {
        write_range(chain_start, current_size, NULL, position - current_size, 1);
    }
    write_range(chain_start, position, buf, len, 0);

    *out_first_cluster = chain_start;
    *out_new_size = end_position > current_size ? end_position : current_size;
    return (int)len;
}

void fat16_truncate(uint16_t first_cluster, uint32_t dir_lba, uint16_t dir_offset, uint16_t *out_first_cluster) {
    if (first_cluster != 0) {
        fat16_free_chain(first_cluster);
    }
    *out_first_cluster = 0;
    fat16_update_entry_size(dir_lba, dir_offset, 0, 0);
}
```

In `kernel/fat16.h`, add after `fat16_read_file`'s declaration:

```c
// Reads up to `len` bytes starting at byte offset `position` within
// the chain rooted at `first_cluster` into `buf`. Caller must clamp
// `len` to not read past the file's size.
void fat16_read_at(uint16_t first_cluster, uint32_t position, void *buf, uint32_t len);

// Writes `len` bytes from `buf` starting at byte offset `position`
// within the file (whose current first cluster/size are given).
// Allocates clusters as needed, zero-filling any gap between
// current_size and position. On success, returns len and fills
// *out_first_cluster/*out_new_size. Returns -ENOSPC if a needed
// cluster can't be allocated.
int fat16_write_file(uint16_t first_cluster, uint32_t current_size, uint32_t position,
                      const void *buf, uint32_t len,
                      uint16_t *out_first_cluster, uint32_t *out_new_size);

// Truncates a file to zero length: frees its cluster chain, patches
// its directory entry to size=0/cluster=0, and sets *out_first_cluster
// to 0.
void fat16_truncate(uint16_t first_cluster, uint32_t dir_lba, uint16_t dir_offset, uint16_t *out_first_cluster);
```

- [ ] **Step 4: Extend `fat16_write_selftest` with content read/write checks**

In `kernel/fat16.c`'s `fat16_write_selftest`, replace the final `serial_write_string("[fat16] write selftest passed\n");` line with:

```c
    uint32_t dir_lba;
    uint16_t dir_offset;
    if (fat16_create_file("/WDATA.TXT", &dir_lba, &dir_offset) != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_create_file(/WDATA.TXT) failed\n");
        return;
    }
    const char *phrase = "Hello, written file!"; // NOTE: 20 bytes, see the next line
    uint32_t phrase_len = 20;
    uint16_t new_cluster;
    uint32_t new_size;
    int written = fat16_write_file(0, 0, 0, phrase, phrase_len, &new_cluster, &new_size);
    if (written != (int)phrase_len || new_size != phrase_len) {
        serial_write_string("[fat16] write selftest FAILED: fat16_write_file(/WDATA.TXT) wrote wrong length\n");
        return;
    }
    fat16_update_entry_size(dir_lba, dir_offset, new_cluster, new_size);

    uint8_t readback[64];
    fat16_read_at(new_cluster, 0, readback, phrase_len);
    for (uint32_t i = 0; i < phrase_len; i++) {
        if (readback[i] != (uint8_t)phrase[i]) {
            serial_write_string("[fat16] write selftest FAILED: /WDATA.TXT readback mismatch\n");
            return;
        }
    }

    // Random-access overwrite mid-file: "written" (position 7) -> "WRITTEN".
    const char *patch = "WRITTEN";
    written = fat16_write_file(new_cluster, new_size, 7, patch, 7, &new_cluster, &new_size);
    if (written != 7) {
        serial_write_string("[fat16] write selftest FAILED: mid-file overwrite failed\n");
        return;
    }
    fat16_update_entry_size(dir_lba, dir_offset, new_cluster, new_size);
    fat16_read_at(new_cluster, 0, readback, new_size);
    if (!buffer_equals_string(readback, new_size, "Hello, WRITTEN file!")) {
        serial_write_string("[fat16] write selftest FAILED: mid-file overwrite readback mismatch\n");
        return;
    }

    // Write past current EOF (position 25, current size 20): the
    // 5-byte gap [20,25) must be zero-filled.
    written = fat16_write_file(new_cluster, new_size, new_size + 5, "END", 3, &new_cluster, &new_size);
    if (written != 3) {
        serial_write_string("[fat16] write selftest FAILED: past-EOF write failed\n");
        return;
    }
    fat16_update_entry_size(dir_lba, dir_offset, new_cluster, new_size);
    fat16_read_at(new_cluster, 0, readback, new_size);
    for (uint32_t i = 20; i < 25; i++) {
        if (readback[i] != 0) {
            serial_write_string("[fat16] write selftest FAILED: gap not zero-filled\n");
            return;
        }
    }
    if (readback[25] != 'E' || readback[26] != 'N' || readback[27] != 'D') {
        serial_write_string("[fat16] write selftest FAILED: past-EOF write data mismatch\n");
        return;
    }

    fat16_delete_entry("/WDATA.TXT");

    serial_write_string("[fat16] write selftest passed\n");
```

- [ ] **Step 5: Build and verify**

Run: `make clean && make disk-image && make iso`, boot as in Task 1.
Expected: `[fat16] write selftest passed` still appears (now after exercising whole-file write, mid-file random-access overwrite, and past-EOF zero-fill), zero `FAILED`, zero exceptions, milestone 6's full lifecycle unaffected.

- [ ] **Step 6: Commit**

```bash
git add kernel/fat16.c kernel/fat16.h
git commit -m "Add FAT16 file content read/write engine with random-access support"
```

---

### Task 4: Syscall Layer, Standard Library Exposure, and Existing-Program Migration

**Files:**
- Modify: `kernel/process.h` (add `struct file_descriptor`, `files[8]` on `struct task`)
- Modify: `kernel/process.c` (zero-initialize `files[]` in `task_create_kernel_thread` and `spawn`)
- Modify: `kernel/syscall.c` (fd numbering, global FS lock, `copy_user_path` helper, all new/changed syscall handlers)
- Create: `lib/include/fcntl.h`, `lib/include/errno.h`
- Modify: `lib/include/unistd.h` (`write`'s signature; add `read`/`close`/`lseek`/`mkdir`/`unlink`, `STD*_FILENO`, `SEEK_*`)
- Modify: `lib/syscall.c` (add `syscall3`, wrappers for the new/changed functions)
- Modify: `lib/stdio.c` (`printf`'s `write` call gains `STDOUT_FILENO`)
- Modify: `userland/spin.c`, `userland/child.c`, `userland/faulter.c` (`write` call sites gain the fd argument — `parent.c`/`looper.c`/`yielder.c` only use `printf`, so they need no changes)
- Create: `userland/fileio.c` (minimal open/write/read/close smoke test)
- Modify: `Makefile` (add `FILEIO.ELF`'s build rule and disk-image entry)
- Modify: `kernel/kernel.c` (temporarily spawn only `FILEIO.ELF` to verify, then revert)

**Interfaces:**
- Consumes: `fat16_find`, `fat16_create_file`, `fat16_mkdir`, `fat16_delete_entry`, `fat16_write_file`, `fat16_read_at`, `fat16_truncate`, `fat16_update_entry_size` (Tasks 2-3); `ENOENT`/`EBADF`/`EEXIST`/`ENOTDIR`/`EISDIR`/`EINVAL`/`EMFILE`/`ENOSPC` (Task 2's `kernel/errno.h`).
- Produces: syscalls `SYS_READ=6`/`SYS_OPEN=7`/`SYS_CLOSE=8`/`SYS_MKDIR=9`/`SYS_UNLINK=10`/`SYS_LSEEK=11`, `SYS_WRITE=1` with changed `(fd, buf, len)` signature; stdlib `int64_t write(int fd, const void *buf, uint64_t len)`, `int64_t read(int fd, void *buf, uint64_t len)`, `int close(int fd)`, `int64_t lseek(int fd, int64_t offset, int whence)`, `int mkdir(const char *path)`, `int unlink(const char *path)`, `int open(const char *path, int flags)` — all consumed by Task 5's extended `fileio.c`.

- [ ] **Step 1: Add the per-task file descriptor table**

In `kernel/process.h`, add before `struct task`:

```c
#define MAX_OPEN_FILES 8

struct file_descriptor {
    int in_use;
    uint16_t first_cluster; // 0 = no clusters allocated yet
    uint32_t size;
    uint32_t position;
    int writable;
    uint32_t dir_entry_lba;
    uint16_t dir_entry_offset;
};
```

Add a `files` member to `struct task`, right before `struct task *next;`:

```c
    struct file_descriptor files[MAX_OPEN_FILES];
```

- [ ] **Step 2: Zero-initialize `files[]` on task creation**

In `kernel/process.c`'s `task_create_kernel_thread`, right after `t->waiting_for_pid = 0;`, add:

```c
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        t->files[i].in_use = 0;
    }
```

In `kernel/process.c`'s `spawn`, right after `t->waiting_for_pid = 0;` (the second occurrence, in `spawn`), add the same loop:

```c
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        t->files[i].in_use = 0;
    }
```

This matters because `tasks[]` is a static array reused across processes (`alloc_task_slot` hands out the same struct memory to a new process once the old one is reaped) — without this, a new process could inherit a previous, unrelated process's "open" file descriptors.

- [ ] **Step 3: Rewrite `kernel/syscall.c`'s dispatch for the new syscall surface**

Replace the syscall number block at the top of `kernel/syscall.c`:

```c
#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_SPAWN  4
#define SYS_WAIT   5
#define SYS_READ   6
#define SYS_OPEN   7
#define SYS_CLOSE  8
#define SYS_MKDIR  9
#define SYS_UNLINK 10
#define SYS_LSEEK  11

// Mirrors lib/include/fcntl.h's O_* values exactly -- the two trees
// don't share headers, so these must be kept in sync by hand.
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

// Mirrors lib/include/unistd.h's SEEK_* values exactly.
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
```

Add `#include "fat16.h"` and `#include "errno.h"` to the top of `kernel/syscall.c`, alongside the existing includes.

Add, right after the `wrmsr` helper and before `syscall_dispatch`:

```c
static volatile int fs_lock = 0;

// Uniprocessor spinlock: cli/sti around the test-and-set is enough
// since interrupts are the only source of preemption here. A task
// that loses the race yields and retries rather than busy-spinning
// with interrupts disabled the whole time.
static void fs_lock_acquire(void) {
    for (;;) {
        __asm__ volatile ("cli");
        if (!fs_lock) {
            fs_lock = 1;
            __asm__ volatile ("sti");
            return;
        }
        __asm__ volatile ("sti");
        schedule();
    }
}

static void fs_lock_release(void) {
    fs_lock = 0;
}

// Copies up to out_size-1 bytes from a user-supplied (pointer, len)
// pair into a NUL-terminated kernel buffer. Shared by every syscall
// that takes a path (SPAWN/OPEN/MKDIR/UNLINK).
static void copy_user_path(int64_t user_ptr, int64_t user_len, char *out, uint64_t out_size) {
    uint64_t len = (uint64_t)user_len;
    if (len > out_size - 1) {
        len = out_size - 1;
    }
    const char *user_path = (const char *)(uintptr_t)user_ptr;
    for (uint64_t i = 0; i < len; i++) {
        out[i] = user_path[i];
    }
    out[len] = '\0';
}
```

Replace the entire `syscall_dispatch` function:

```c
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    (void)a4;
    switch (num) {
        case SYS_EXIT:
            task_exit((int)a1);
            return 0; // unreachable -- task_exit never returns
        case SYS_WRITE: {
            int fd = (int)a1;
            const char *buf = (const char *)(uintptr_t)a2;
            uint64_t len = (uint64_t)a3;
            if (fd == 1 || fd == 2) {
                serial_write_string_n(buf, len);
                return (int64_t)len;
            }
            if (fd < 3 || fd >= 3 + MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use || !f->writable) {
                return -EBADF;
            }
            fs_lock_acquire();
            uint16_t new_cluster;
            uint32_t new_size;
            int written = fat16_write_file(f->first_cluster, f->size, f->position, buf, (uint32_t)len, &new_cluster, &new_size);
            if (written < 0) {
                fs_lock_release();
                return written;
            }
            if (new_cluster != f->first_cluster || new_size != f->size) {
                fat16_update_entry_size(f->dir_entry_lba, f->dir_entry_offset, new_cluster, new_size);
            }
            f->first_cluster = new_cluster;
            f->size = new_size;
            f->position += (uint32_t)len;
            fs_lock_release();
            return written;
        }
        case SYS_READ: {
            int fd = (int)a1;
            char *buf = (char *)(uintptr_t)a2;
            uint64_t len = (uint64_t)a3;
            if (fd == 0) {
                return 0; // no keyboard-to-process input path yet
            }
            if (fd < 3 || fd >= 3 + MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use) {
                return -EBADF;
            }
            uint32_t remaining = f->size > f->position ? f->size - f->position : 0;
            uint32_t to_read = (uint32_t)len;
            if (to_read > remaining) {
                to_read = remaining;
            }
            fat16_read_at(f->first_cluster, f->position, buf, to_read);
            f->position += to_read;
            return to_read;
        }
        case SYS_GETPID:
            return current_task()->pid;
        case SYS_YIELD:
            schedule();
            return 0;
        case SYS_SPAWN: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            struct task *child = spawn(path_buf);
            return child ? child->pid : -1;
        }
        case SYS_WAIT:
            return wait_for_pid((int)a1);
        case SYS_OPEN: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            int flags = (int)a3;

            struct task *task = current_task();
            int slot = -1;
            for (int i = 0; i < MAX_OPEN_FILES; i++) {
                if (!task->files[i].in_use) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                return -EMFILE;
            }

            fs_lock_acquire();

            uint16_t cluster;
            uint32_t size;
            uint32_t dir_lba;
            uint16_t dir_offset;
            int found = fat16_find(path_buf, &cluster, &size, &dir_lba, &dir_offset);

            if (!found) {
                if (!(flags & O_CREAT)) {
                    fs_lock_release();
                    return -ENOENT;
                }
                int created = fat16_create_file(path_buf, &dir_lba, &dir_offset);
                if (created < 0) {
                    fs_lock_release();
                    return created;
                }
                cluster = 0;
                size = 0;
            } else if (flags & O_TRUNC) {
                fat16_truncate(cluster, dir_lba, dir_offset, &cluster);
                size = 0;
            }

            fs_lock_release();

            struct file_descriptor *f = &task->files[slot];
            f->in_use = 1;
            f->first_cluster = cluster;
            f->size = size;
            f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
            f->dir_entry_lba = dir_lba;
            f->dir_entry_offset = dir_offset;
            f->position = (flags & O_APPEND) ? size : 0;

            return slot + 3;
        }
        case SYS_CLOSE: {
            int fd = (int)a1;
            if (fd < 3 || fd >= 3 + MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use) {
                return -EBADF;
            }
            f->in_use = 0;
            return 0;
        }
        case SYS_MKDIR: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            fs_lock_acquire();
            int result = fat16_mkdir(path_buf);
            fs_lock_release();
            return result;
        }
        case SYS_UNLINK: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            fs_lock_acquire();
            int result = fat16_delete_entry(path_buf);
            fs_lock_release();
            return result;
        }
        case SYS_LSEEK: {
            int fd = (int)a1;
            int64_t offset = a2;
            int whence = (int)a3;
            if (fd < 3 || fd >= 3 + MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use) {
                return -EBADF;
            }
            int64_t base;
            switch (whence) {
                case SEEK_SET: base = 0; break;
                case SEEK_CUR: base = (int64_t)f->position; break;
                case SEEK_END: base = (int64_t)f->size; break;
                default: return -EINVAL;
            }
            int64_t new_position = base + offset;
            if (new_position < 0) {
                return -EINVAL;
            }
            f->position = (uint32_t)new_position;
            return new_position;
        }
        default:
            serial_write_string("[syscall] unknown syscall number\n");
            return -1;
    }
}
```

- [ ] **Step 4: Add `lib/include/errno.h` and `lib/include/fcntl.h`**

`lib/include/errno.h`:

```c
#ifndef NEOOS_ERRNO_H
#define NEOOS_ERRNO_H

// Every open/read/write/close/lseek/mkdir/unlink call in this library
// returns its negative error code directly, e.g. open() on a missing
// path returns -ENOENT -- there is no separate settable errno
// variable. spawn/wait/getpid are unaffected and keep their existing
// plain -1-on-failure convention.

#define ENOENT  2
#define EBADF   9
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28

#endif
```

`lib/include/fcntl.h`:

```c
#ifndef NEOOS_FCNTL_H
#define NEOOS_FCNTL_H

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

// Opens (or, with O_CREAT, creates) the file at `path` with the given
// flags. Returns a file descriptor, or a negative errno.h code on
// failure.
int open(const char *path, int flags);

#endif
```

- [ ] **Step 5: Update `lib/include/unistd.h`**

Replace the file's contents entirely:

```c
#ifndef NEOOS_UNISTD_H
#define NEOOS_UNISTD_H

#include <stdint.h>

// NeoOS's standard library. Function names follow POSIX convention
// where the semantics match; spawn/wait are NeoOS-specific: spawn
// builds a fresh process directly from a path (not fork+exec), and
// wait takes one specific PID (not "any child").

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

void exit(int code) __attribute__((noreturn));

// Writes `len` bytes from `buf` to the file (or console, for fd
// STDOUT_FILENO/STDERR_FILENO) open on `fd`. Returns the number of
// bytes written, or a negative errno.h code on failure.
int64_t write(int fd, const void *buf, uint64_t len);

// Reads up to `len` bytes from the file (or console, for fd
// STDIN_FILENO, which always returns 0 -- there is no
// keyboard-to-process input path yet) open on `fd` into `buf`.
// Returns the number of bytes actually read (0 at EOF), or a negative
// errno.h code on failure.
int64_t read(int fd, void *buf, uint64_t len);

// Closes `fd`. Returns 0, or a negative errno.h code on failure.
int close(int fd);

// Moves fd's read/write position. whence is SEEK_SET/SEEK_CUR/
// SEEK_END. Returns the new absolute position, or a negative errno.h
// code on failure.
int64_t lseek(int fd, int64_t offset, int whence);

int getpid(void);
void yield(void);

// Builds a fresh process directly from the ELF executable at `path`
// (NUL-terminated) and returns its PID, or -1 on failure.
int spawn(const char *path);

// Blocks until the process with the given PID exits, reaps it, and
// returns its exit code.
int wait(int pid);

// Creates a new, empty directory at `path`. Returns 0, or a negative
// errno.h code on failure.
int mkdir(const char *path);

// Deletes the file at `path`. Returns 0, or a negative errno.h code
// on failure (including -EISDIR if `path` is a directory).
int unlink(const char *path);

#endif
```

- [ ] **Step 6: Update `lib/syscall.c`**

Replace the syscall number block at the top:

```c
#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_SPAWN  4
#define SYS_WAIT   5
#define SYS_READ   6
#define SYS_OPEN   7
#define SYS_CLOSE  8
#define SYS_MKDIR  9
#define SYS_UNLINK 10
#define SYS_LSEEK  11
```

Add `#include "fcntl.h"` alongside the existing `#include "unistd.h"`/`#include "string.h"`.

Add, after `syscall2`:

```c
static inline int64_t syscall3(int64_t num, int64_t a1, int64_t a2, int64_t a3) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}
```

Replace the existing `write` function:

```c
int64_t write(int fd, const void *buf, uint64_t len) {
    return syscall3(SYS_WRITE, fd, (int64_t)(uint64_t)buf, (int64_t)len);
}
```

Add, after `write`:

```c
int64_t read(int fd, void *buf, uint64_t len) {
    return syscall3(SYS_READ, fd, (int64_t)(uint64_t)buf, (int64_t)len);
}

int open(const char *path, int flags) {
    uint64_t len = strlen(path);
    return (int)syscall3(SYS_OPEN, (int64_t)(uint64_t)path, (int64_t)len, flags);
}

int close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

int64_t lseek(int fd, int64_t offset, int whence) {
    return syscall3(SYS_LSEEK, fd, offset, whence);
}

int mkdir(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_MKDIR, (int64_t)(uint64_t)path, (int64_t)len);
}

int unlink(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_UNLINK, (int64_t)(uint64_t)path, (int64_t)len);
}
```

- [ ] **Step 7: Fix `lib/stdio.c`'s `write` call**

In `lib/stdio.c`'s `printf`, change:

```c
    write(buf, pos);
```

to:

```c
    write(STDOUT_FILENO, buf, pos);
```

- [ ] **Step 8: Update the three existing programs with direct `write` calls**

In `userland/spin.c`, change `write(msg, strlen(msg));` to `write(STDOUT_FILENO, msg, strlen(msg));`.

In `userland/child.c`, change `write(msg, strlen(msg));` to `write(STDOUT_FILENO, msg, strlen(msg));`.

In `userland/faulter.c`, change `write(msg, strlen(msg));` to `write(STDOUT_FILENO, msg, strlen(msg));`.

(`parent.c`/`looper.c`/`yielder.c` only call `printf`, never `write` directly, so they need no changes — `stdio.c`'s fix in Step 7 covers them.)

- [ ] **Step 9: Create the minimal `fileio.c` smoke test**

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int fd = open("/FILEIO.TXT", O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) {
        printf("[fileio] open for write FAILED: %d\n", fd);
        return 1;
    }
    const char msg[] = "hello from fileio\n";
    uint64_t msg_len = strlen(msg);
    int64_t written = write(fd, msg, msg_len);
    if (written != (int64_t)msg_len) {
        printf("[fileio] write FAILED: %d\n", (int)written);
        return 1;
    }
    close(fd);

    fd = open("/FILEIO.TXT", O_RDONLY);
    if (fd < 0) {
        printf("[fileio] open for read FAILED: %d\n", fd);
        return 1;
    }
    char readback[64];
    int64_t got = read(fd, readback, sizeof(readback) - 1);
    close(fd);

    int mismatch = ((uint64_t)got != msg_len);
    if (!mismatch) {
        for (uint64_t i = 0; i < msg_len; i++) {
            if (readback[i] != msg[i]) {
                mismatch = 1;
                break;
            }
        }
    }
    if (mismatch) {
        printf("[fileio] readback mismatch\n");
        return 1;
    }

    printf("[fileio] create/write/read smoke test passed\n");
    return 0;
}
```

- [ ] **Step 10: Add `fileio.c`'s Makefile build rule and disk-image entry**

In the `Makefile`, add after `FAULTER.ELF`'s build rule:

```makefile
$(USERLAND_BUILD)/FILEIO.ELF: $(USERLAND_DIR)/fileio.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/fileio.c -L$(LIB_BUILD) -lneoos
```

Change the `$(DISK_IMG)` target's prerequisite list to add `$(USERLAND_BUILD)/FILEIO.ELF` at the end:

```makefile
$(DISK_IMG): $(USERLAND_BUILD)/SPIN.ELF $(USERLAND_BUILD)/CHILD.ELF $(USERLAND_BUILD)/PARENT.ELF $(USERLAND_BUILD)/LOOPER.ELF $(USERLAND_BUILD)/YIELDER.ELF $(USERLAND_BUILD)/FAULTER.ELF $(USERLAND_BUILD)/FILEIO.ELF
```

Add, after the existing `mcopy ... FAULTER.ELF` line in that target's recipe:

```makefile
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/FILEIO.ELF ::BIN/FILEIO.ELF
```

- [ ] **Step 11: Temporarily spawn only `FILEIO.ELF` to verify**

In `kernel/kernel.c`, replace the four `spawn(...)` calls (and the `parent_task` FAILED check) with:

```c
    spawn("/BIN/FILEIO.ELF");
```

- [ ] **Step 12: Build and verify**

Run: `make clean && make disk-image && make iso`, boot as in Task 1.
Expected: `[fileio] create/write/read smoke test passed` appears on serial, zero `FAILED`, zero exceptions.

- [ ] **Step 13: Revert the temporary spawn change and verify no regression**

Restore `kernel/kernel.c`'s original `spawn("/BIN/PARENT.ELF")` + `parent_task` FAILED check + two `spawn("/BIN/LOOPER.ELF")` + `spawn("/BIN/YIELDER.ELF")` calls. Confirm `git diff --stat kernel/kernel.c` prints nothing.

Rebuild and boot again. Expected: milestone 6's exact full lifecycle (bursty looper interleave, dense yielder interleave, `child running, exiting with code 42`, `[parent] child exit code=42`) reproduces exactly, now flowing through the changed `write`/`printf` fd path. Zero `FAILED`, zero exceptions.

- [ ] **Step 14: Commit**

```bash
git add kernel/process.h kernel/process.c kernel/syscall.c lib/include/errno.h lib/include/fcntl.h lib/include/unistd.h lib/syscall.c lib/stdio.c userland/spin.c userland/child.c userland/faulter.c userland/fileio.c Makefile
git commit -m "Add file descriptor syscalls and expose them through the standard library"
```

---

### Task 5: Full File I/O Test Coverage and Independent Disk Verification

**Files:**
- Modify: `userland/fileio.c` (extend with `lseek` overwrite, reopen, `mkdir`, `unlink`)
- Modify: `kernel/kernel.c` (temporarily spawn only `FILEIO.ELF` again, then revert)

**Interfaces:**
- Consumes: everything from Task 4.
- Produces: nothing new — this task is verification-only.

- [ ] **Step 1: Extend `fileio.c` with the full exercise**

Replace `userland/fileio.c`'s contents entirely:

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static int check_bytes_equal(const char *a, const char *b, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Create, write, close.
    int fd = open("/FILEIO.TXT", O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) {
        printf("[fileio] open for write FAILED: %d\n", fd);
        return 1;
    }
    const char msg[] = "hello from fileio\n"; // 18 bytes
    uint64_t msg_len = strlen(msg);
    if (write(fd, msg, msg_len) != (int64_t)msg_len) {
        printf("[fileio] write FAILED\n");
        return 1;
    }
    close(fd);

    // Reopen for read+write, lseek back, overwrite mid-file:
    // "from f" (position 6, 6 bytes) -> "FILEIO".
    fd = open("/FILEIO.TXT", O_RDWR);
    if (fd < 0) {
        printf("[fileio] reopen for rdwr FAILED: %d\n", fd);
        return 1;
    }
    if (lseek(fd, 6, SEEK_SET) != 6) {
        printf("[fileio] lseek FAILED\n");
        return 1;
    }
    const char patch[] = "FILEIO";
    if (write(fd, patch, 6) != 6) {
        printf("[fileio] mid-file write FAILED\n");
        return 1;
    }
    close(fd);

    // Reopen for read, verify the overwrite landed correctly.
    fd = open("/FILEIO.TXT", O_RDONLY);
    if (fd < 0) {
        printf("[fileio] reopen for read FAILED: %d\n", fd);
        return 1;
    }
    char readback[64];
    int64_t got = read(fd, readback, sizeof(readback) - 1);
    close(fd);
    const char expected[] = "hello FILEIOileio\n";
    if (got != (int64_t)msg_len || !check_bytes_equal(readback, expected, (uint64_t)got)) {
        printf("[fileio] mid-file overwrite readback mismatch\n");
        return 1;
    }

    // mkdir + create/write/read a file inside it.
    if (mkdir("/FIODIR") != 0) {
        printf("[fileio] mkdir FAILED\n");
        return 1;
    }
    fd = open("/FIODIR/INNER.TXT", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("[fileio] create nested file FAILED: %d\n", fd);
        return 1;
    }
    const char nested_msg[] = "nested\n";
    uint64_t nested_len = strlen(nested_msg);
    if (write(fd, nested_msg, nested_len) != (int64_t)nested_len) {
        printf("[fileio] nested write FAILED\n");
        return 1;
    }
    close(fd);

    fd = open("/FIODIR/INNER.TXT", O_RDONLY);
    if (fd < 0) {
        printf("[fileio] reopen nested file FAILED: %d\n", fd);
        return 1;
    }
    got = read(fd, readback, sizeof(readback) - 1);
    close(fd);
    if (got != (int64_t)nested_len || !check_bytes_equal(readback, nested_msg, (uint64_t)got)) {
        printf("[fileio] nested readback mismatch\n");
        return 1;
    }

    // unlink and verify it's gone.
    if (unlink("/FILEIO.TXT") != 0) {
        printf("[fileio] unlink FAILED\n");
        return 1;
    }
    fd = open("/FILEIO.TXT", O_RDONLY);
    if (fd >= 0) {
        printf("[fileio] FILEIO.TXT still openable after unlink\n");
        return 1;
    }

    printf("[fileio] all checks passed\n");
    return 0;
}
```

- [ ] **Step 2: Temporarily spawn only `FILEIO.ELF`**

In `kernel/kernel.c`, replace the four `spawn(...)` calls (and the `parent_task` FAILED check) with:

```c
    spawn("/BIN/FILEIO.ELF");
```

- [ ] **Step 3: Build and verify**

Run: `make clean && make disk-image && make iso`, boot as in Task 1, with the disk attached.
Expected: `[fileio] all checks passed` appears on serial, zero `FAILED`, zero exceptions.

- [ ] **Step 4: Independently verify the on-disk FAT16 structure with mtools**

With `build/disk.img` as produced by the boot in Step 3 (do not re-run `make disk-image`, which would regenerate a fresh image and lose the writes):

```bash
mdir -i build/disk.img ::
mdir -i build/disk.img ::/FIODIR
mtype -i build/disk.img ::/FIODIR/INNER.TXT
```

Expected: the root listing does **not** show `FILEIO.TXT` (unlinked by the test) but **does** show `FIODIR`; the `FIODIR` listing shows `INNER.TXT`; `mtype` prints `nested` followed by a newline — confirming the on-disk structure NeoOS's own driver produced is independently readable by a real FAT16 implementation, not just self-consistent with its own reads.

- [ ] **Step 5: Revert the temporary spawn change and verify no regression**

Restore `kernel/kernel.c`'s original `spawn("/BIN/PARENT.ELF")` + `parent_task` FAILED check + two `spawn("/BIN/LOOPER.ELF")` + `spawn("/BIN/YIELDER.ELF")` calls. Confirm `git diff --stat kernel/kernel.c` prints nothing.

Rebuild (`make clean && make disk-image && make iso` — this regenerates a fresh `disk.img`, which is fine now that Step 4's inspection is done) and boot again. Expected: milestone 6's exact full lifecycle reproduces, zero `FAILED`, zero exceptions.

- [ ] **Step 6: Commit**

```bash
git add userland/fileio.c
git commit -m "Extend fileio test coverage: lseek, mkdir, unlink; verify on-disk structure with mtools"
```

---

### Task 6: Documentation

**Files:**
- Modify: `docs/stdlib.md`

**Interfaces:** None new.

- [ ] **Step 1: Update `docs/stdlib.md`**

Replace the `## <unistd.h>` section's `write` entry:

```markdown
- `int64_t write(int fd, const void *buf, uint64_t len)` — writes
  `len` bytes from `buf` to the file (or console, for fd
  `STDOUT_FILENO`/`STDERR_FILENO`) open on `fd`. Returns the number of
  bytes written, or a negative `<errno.h>` code on failure.
```

Add, after the (now-updated) `write` entry:

```markdown
- `int64_t read(int fd, void *buf, uint64_t len)` — reads up to `len`
  bytes from the file (or console, for fd `STDIN_FILENO`, which always
  returns 0 -- there is no keyboard-to-process input path yet) open on
  `fd` into `buf`. Returns the number of bytes actually read (0 at
  EOF), or a negative `<errno.h>` code on failure.
- `int close(int fd)` — closes `fd`. Returns 0, or a negative
  `<errno.h>` code on failure.
- `int64_t lseek(int fd, int64_t offset, int whence)` — moves `fd`'s
  read/write position. `whence` is `SEEK_SET`/`SEEK_CUR`/`SEEK_END`.
  Returns the new absolute position, or a negative `<errno.h>` code on
  failure. Writing past the current end of file (via a forward
  `lseek`) zero-fills the gap with real allocated bytes, not a logical
  sparse hole.
- `int mkdir(const char *path)` — creates a new, empty directory.
  Returns 0, or a negative `<errno.h>` code on failure.
- `int unlink(const char *path)` — deletes the file at `path`. Returns
  0, or a negative `<errno.h>` code on failure (including `-EISDIR` if
  `path` is a directory; there is no `rmdir`).
- `STDIN_FILENO`/`STDOUT_FILENO`/`STDERR_FILENO` (0/1/2) and
  `SEEK_SET`/`SEEK_CUR`/`SEEK_END` (0/1/2) constants.
```

Add a new section after `## <unistd.h>`:

```markdown
## `<fcntl.h>`

- `int open(const char *path, int flags)` — opens (or, with
  `O_CREAT`, creates) the file at `path`. Returns a file descriptor,
  or a negative `<errno.h>` code on failure.
- `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`
  flag constants.

## `<errno.h>`

Every `open`/`read`/`write`/`close`/`lseek`/`mkdir`/`unlink` call
returns its negative error code directly instead of a bare `-1` --
there is no separate settable `errno` variable. `spawn`/`wait`/
`getpid` are unaffected and keep their existing plain `-1`-on-failure
convention.

- `ENOENT` (2) — path/file not found.
- `EBADF` (9) — invalid or closed file descriptor.
- `EEXIST` (17) — `mkdir`/`open(O_CREAT)` target already exists.
- `ENOTDIR` (20) — a path component used as a directory isn't one.
- `EISDIR` (21) — `unlink` called on a directory.
- `EINVAL` (22) — bad argument (e.g. an `lseek` result would be
  negative, or an unrecognized `whence`).
- `EMFILE` (24) — the process's file descriptor table is full (8
  open files at once, maximum).
- `ENOSPC` (28) — disk full (no free cluster), or the root directory
  is full (it has a fixed maximum entry count).
```

- [ ] **Step 2: Commit**

```bash
git add docs/stdlib.md
git commit -m "Document the read-write FAT16 standard library additions"
```
