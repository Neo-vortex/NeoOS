# VFS, Mounts, and FAT32 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put a refcounted vnode/mount layer between the syscall boundary and the on-disk format, then prove it by running four unlike filesystems at once — FAT16, FAT32, ramfs, and devfs.

**Architecture:** A fixed static pool of refcounted `struct vnode` objects, cached by `(mount, inode_id)`, sits behind a `struct vfs_ops` function table implemented by each driver. A mount table maps path prefixes to driver instances, resolved by longest-prefix match. FAT16 and FAT32 are one driver distinguished by a runtime variant enum detected at mount from cluster count.

**Tech Stack:** C (gnu11, freestanding, `-mcmodel=kernel`), NASM, x86_64 cross-toolchain at `$HOME/opt/cross-x86_64-elf`, QEMU with `-cpu Nehalem`, GRUB/Multiboot2, `mkfs.fat`/`mtools` for disk images.

**Spec:** `docs/superpowers/specs/2026-08-27-vfs-milestone-design.md`

## Global Constraints

- **No host-runnable tests.** Per `CLAUDE.md`, this is bare-metal code with no host runtime. Every task's verification is: build, boot headless in QEMU, capture the serial log, and grep it. The build/boot cycle replaces a test-runner cycle; a "failing test" step means "a boot log that does not yet show the expected line".
- **Work directly on `main`.** No feature branches, by explicit user preference, matching every milestone so far.
- **Standard library convention is binding.** Any kernel feature reachable from user mode needs a `lib/` wrapper *and* a `docs/stdlib.md` entry in the same task. This applies to `mount`, `umount`, `getdents`, `opendir`, `readdir`, `closedir`.
- **Physmap covers the first 4GiB.** `phys_to_virt` is valid only within it.
- **8.3 filenames only.** VFAT long-name entries (attr `0x0F`) are skipped during directory scans, never parsed.
- **Every verification run must show zero `FAILED` lines and zero exceptions.**
- **Use a fresh disk image when checking FAT write selftests.** `fat16_write_selftest` creates `/NEWDIR`; a re-booted stale image makes it report `FAILED` spuriously. `rm -f build/disk.img build/disk2.img && make disk-image` before any run whose result you intend to trust.
- **QEMU invocation** (both drives, headless, serial to file):
  ```bash
  qemu-system-x86_64 -cpu Nehalem -boot order=d \
    -cdrom build/neoos.iso \
    -drive file=build/disk.img,format=raw \
    -drive file=build/disk2.img,format=raw \
    -display none -serial file:/tmp/neoos.log -no-reboot
  ```
  It never exits on its own; wrap it in `timeout 60`.

## New error codes

`kernel/errno.h` and `lib/include/errno.h` are duplicated, not shared. Both gain, in Task 1:

```c
#define EPERM   1
#define EBUSY  16
#define ENODEV 19
#define ENFILE 23
```

---

### Task 1: ATA drive parameter, second FAT32 disk image, new errno codes

**Files:**
- Modify: `kernel/ata.h`, `kernel/ata.c`, `kernel/fat16.c`, `kernel/kernel.c`, `kernel/errno.h`, `lib/include/errno.h`, `Makefile`

**Interfaces:**
- Produces: `int ata_identify(uint8_t drive, struct ata_identify_info *info)`, `int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buffer)`, `int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void *buffer)` — `drive` is 0 (primary master) or 1 (primary slave). Consumed by every later task through the FAT driver.
- Consumes: nothing new.

- [x] **Step 1: Add the new errno codes to both trees**

In `kernel/errno.h` and `lib/include/errno.h`, add above `#define ENOENT  2`:

```c
#define EPERM   1
```

and in numeric order among the rest:

```c
#define EBUSY  16
#define ENODEV 19
#define ENFILE 23
```

Both files must end up with an identical set of names and values — they are deliberate duplicates of each other.

- [x] **Step 2: Add the drive parameter to `kernel/ata.h`**

Replace the three declarations with:

```c
// `drive` selects the primary channel's master (0) or slave (1).
// Both share port base 0x1F0 and IRQ 14; drive select is bit 4 of the
// byte written to ATA_DRIVE_HEAD, so a second drive needs no new
// controller, IRQ wiring, or port range.
int ata_identify(uint8_t drive, struct ata_identify_info *info);
int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void *buffer);
```

- [x] **Step 3: Thread the parameter through `kernel/ata.c`**

Three call sites write the drive-head register. Change each:

`ata_identify` (currently `outb(ATA_DRIVE_HEAD, 0xA0); // master drive`):

```c
    outb(ATA_DRIVE_HEAD, 0xA0 | ((drive & 1) << 4));
```

`ata_read_sectors` and `ata_write_sectors` (currently `outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));`):

```c
    outb(ATA_DRIVE_HEAD, 0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F));
```

Add `uint8_t drive` as the first parameter of all three function definitions to match the header.

In `ata_identify`'s two log lines, include which drive it is so the boot log distinguishes them:

```c
        serial_write_string("[ata] identify FAILED: no drive present, drive=");
        serial_write_hex64(drive);
        serial_write_string("\n");
```

```c
    serial_write_string("[ata] drive identified, drive=");
    serial_write_hex64(drive);
    serial_write_string(" sectors=");
```

- [x] **Step 4: Update every existing caller to pass drive 0**

In `kernel/fat16.c`, every `ata_read_sectors(` and `ata_write_sectors(` call gains a leading `0`. Find them with:

```bash
grep -n "ata_read_sectors\|ata_write_sectors" kernel/fat16.c
```

Example — `fat16_mount`'s boot-sector read becomes:

```c
    if (!ata_read_sectors(0, 0, 1, sector)) {
```

In `kernel/kernel.c`, `ata_identify(&ata_info)` becomes `ata_identify(0, &ata_info)`.

- [x] **Step 5: Build a second 64MB FAT32 disk image in the Makefile**

FAT32 requires at least 65525 clusters; a 32MB image makes `mkfs.fat -F 32` warn that the count is below the supported minimum. 64MB is the floor and is what this plan uses.

Add near `DISK_IMG`:

```makefile
DISK2_IMG := $(BUILD_DIR)/disk2.img
```

Add a new target after the `$(DISK_IMG)` rule:

```makefile
$(DISK2_IMG):
	mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(DISK2_IMG) bs=1M count=64 status=none
	mkfs.fat -F 32 $(DISK2_IMG)
	printf 'Hello from the FAT32 volume!\n' > $(DISK_SRC)/FAT32.TXT
	mcopy -i $(DISK2_IMG) $(DISK_SRC)/FAT32.TXT ::FAT32.TXT
	mmd -i $(DISK2_IMG) ::SUB
	printf 'nested on fat32\n' > $(DISK_SRC)/F32NEST.TXT
	mcopy -i $(DISK2_IMG) $(DISK_SRC)/F32NEST.TXT ::SUB/F32NEST.TXT
```

The `$(DISK_SRC)` directory is created by the `$(DISK_IMG)` rule, so make `$(DISK2_IMG)` depend on it:

```makefile
$(DISK2_IMG): $(DISK_IMG)
```

(fold that dependency into the target line above rather than writing it twice).

Change the `disk-image` phony target to build both:

```makefile
disk-image: $(DISK_IMG) $(DISK2_IMG)
```

Update `run` to attach both drives:

```makefile
run: iso disk-image
	qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom $(BUILD_DIR)/neoos.iso -drive file=$(DISK_IMG),format=raw -drive file=$(DISK2_IMG),format=raw
```

- [x] **Step 6: Temporarily probe drive 1 to prove it is reachable**

In `kernel/kernel.c`, right after the existing `ata_identify(0, &ata_info);`:

```c
    struct ata_identify_info ata_info2;
    ata_identify(1, &ata_info2);
```

- [x] **Step 7: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "\[ata\]" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: two `[ata] drive identified` lines, `drive=0x0` with the 32MB image's sector count (0x10000) and `drive=0x1` with the 64MB image's (0x20000). `FAILED|exception` count is 0. Every milestone 5-9 program reproduces its prior behavior.

- [x] **Step 8: Remove the temporary probe and re-verify**

Delete the `ata_info2` lines added in Step 6. Rebuild and boot again; confirm one `[ata] drive identified` line, zero `FAILED`, zero exceptions.

- [x] **Step 9: Commit**

```bash
git add kernel/ata.h kernel/ata.c kernel/fat16.c kernel/kernel.c kernel/errno.h lib/include/errno.h Makefile
git commit -m "Add ATA drive selection and a second FAT32 disk image"
```

---

### Task 2: VFS core — vnode cache, mount table, path resolution, minimal ramfs

**Files:**
- Create: `kernel/fs/vfs.h`, `kernel/fs/vfs.c`, `kernel/fs/ramfs.h`, `kernel/fs/ramfs.c`
- Modify: `kernel/kernel.c`, `Makefile`

**Interfaces:**
- Produces: `struct vnode`, `struct vfs_ops`, `struct vfs_mount`, `struct dirent`; `vfs_init`, `vfs_mount_fs`, `vfs_umount`, `vnode_get`, `vnode_put`, `vfs_resolve`, `vfs_resolve_parent`, `vfs_selftest`; `ramfs_ops`. Consumed by every later task.
- Consumes: `pmm_alloc`/`pmm_free` and `phys_to_virt` (existing), the errno codes from Task 1.

This task lands the whole abstraction plus the smallest driver that can exercise it. ramfs is first rather than FAT deliberately: it has no disk I/O, so a failure here is unambiguously a VFS-core bug. `mkdir`, `unlink`, and `readdir` are deferred to Task 3 to keep this task's review surface to the cache and the resolver.

- [x] **Step 1: Add `kernel/fs/vfs.h`**

```c
#ifndef NEOOS_VFS_H
#define NEOOS_VFS_H

#include <stdint.h>

#define MAX_MOUNTS    8
#define MAX_VNODES    64
#define VFS_MAX_PATH  128
#define VFS_NAME_MAX  13   // 8.3 name, dot, NUL

// Directory entry type codes. This struct crosses the syscall boundary
// via getdents(), so its layout is shared contract with
// lib/include/dirent.h -- the kernel and library trees do not share
// headers, so the definition is DUPLICATED there and the two must stay
// in lockstep, exactly like the syscall numbers in kernel/syscall.c and
// lib/syscall.c. Fixed-size fields only, so there is no padding
// ambiguity between the two builds.
#define DT_REG 1
#define DT_DIR 2
#define DT_CHR 3

struct dirent {
    char    name[VFS_NAME_MAX];
    uint8_t type;
};

enum vnode_type { VNODE_FILE, VNODE_DIR, VNODE_DEVICE };

struct vfs_mount;

struct vnode {
    struct vfs_mount *mount;
    uint64_t          inode_id;   // driver-defined, unique within the mount
    enum vnode_type   type;
    uint32_t          size;
    uint32_t          refcount;   // live fds plus transient walk holds
    void             *fs_private; // driver state
    struct vnode     *next;       // hash-bucket chain
};

// No op pointer is ever NULL. A driver that cannot perform an
// operation supplies a stub returning -EPERM (or -ENOTDIR/-EISDIR
// where that is the truthful code), so callers never branch on which
// driver they are talking to.
struct vfs_ops {
    int     (*mount)(struct vfs_mount *m, const char *source);
    void    (*umount)(struct vfs_mount *m);
    int     (*read_inode)(struct vfs_mount *m, uint64_t inode_id, struct vnode *out);
    int     (*sync_inode)(struct vnode *vn);
    int     (*lookup)(struct vnode *dir, const char *name, uint64_t *out_inode_id);
    int64_t (*read)(struct vnode *vn, uint32_t pos, void *buf, uint32_t len);
    int64_t (*write)(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len);
    int     (*create)(struct vnode *dir, const char *name, uint64_t *out_inode_id);
    int     (*mkdir)(struct vnode *dir, const char *name);
    int     (*unlink)(struct vnode *dir, const char *name);
    int     (*truncate)(struct vnode *vn);
    int     (*readdir)(struct vnode *dir, uint32_t index, struct dirent *out);
};

struct vfs_mount {
    int   in_use;
    char  path[VFS_MAX_PATH];
    const struct vfs_ops *ops;
    void *fs_private;
    struct vnode *root;
};

void vfs_init(void);
void vfs_selftest(void);

// Number of vnode pool slots currently claimed. A quiesced system
// reads exactly one per mount (each mount holds its own root).
uint32_t vfs_vnode_in_use_count(void);

// fstype is "fat", "ramfs", or "devfs". source is "hd0"/"hd1" for
// "fat" and ignored otherwise.
int vfs_mount_fs(const char *source, const char *target, const char *fstype);
int vfs_umount(const char *target);

// Returns the cached vnode with refcount already incremented, or 0 if
// the pool is exhausted or the driver's read_inode failed.
struct vnode *vnode_get(struct vfs_mount *m, uint64_t inode_id);
void vnode_put(struct vnode *vn);

// Resolves an absolute path to a vnode whose refcount is already
// taken -- caller must vnode_put it. On failure returns 0 and sets
// *out_err to a negative errno.
struct vnode *vfs_resolve(const char *path, int *out_err);

// Resolves the parent directory of `path` and copies the final path
// component into out_name (VFS_NAME_MAX bytes). Used by create, mkdir,
// and unlink. Same refcount and error contract as vfs_resolve.
struct vnode *vfs_resolve_parent(const char *path, char *out_name, int *out_err);

#endif
```

- [x] **Step 2: Add `kernel/fs/vfs.c` — pool, cache, and mount table**

```c
#include "vfs.h"
#include "ramfs.h"
#include "errno.h"
#include "serial.h"

static struct vfs_mount mounts[MAX_MOUNTS];
static struct vnode vnodes[MAX_VNODES];

#define VNODE_BUCKETS 16
static struct vnode *buckets[VNODE_BUCKETS];

static unsigned bucket_of(struct vfs_mount *m, uint64_t inode_id) {
    // Mix the mount pointer in so two volumes' identical inode ids
    // (both roots are id 0) do not always collide in one bucket.
    uint64_t h = inode_id ^ ((uint64_t)(uintptr_t)m >> 4);
    return (unsigned)(h % VNODE_BUCKETS);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, uint64_t dst_size) {
    uint64_t i = 0;
    while (src[i] && i < dst_size - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Number of pool slots currently claimed. A mount holds one (its
// root), so a quiesced system with N mounts reads exactly N. Used by
// vfs_selftest's leaked-reference check and by the end-of-milestone
// leak gate; also the first thing to look at when a refcount bug is
// suspected, since a count that rises across an operation localises it
// immediately.
uint32_t vfs_vnode_in_use_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_VNODES; i++) {
        if (vnodes[i].mount != 0) { n++; }
    }
    return n;
}

void vfs_init(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mounts[i].in_use = 0;
    }
    for (int i = 0; i < MAX_VNODES; i++) {
        vnodes[i].refcount = 0;
        vnodes[i].mount = 0;
        vnodes[i].next = 0;
    }
    for (int i = 0; i < VNODE_BUCKETS; i++) {
        buckets[i] = 0;
    }
    serial_write_string("[vfs] initialized\n");
}

struct vnode *vnode_get(struct vfs_mount *m, uint64_t inode_id) {
    unsigned b = bucket_of(m, inode_id);
    for (struct vnode *vn = buckets[b]; vn; vn = vn->next) {
        if (vn->mount == m && vn->inode_id == inode_id) {
            vn->refcount++;
            return vn;
        }
    }

    struct vnode *slot = 0;
    for (int i = 0; i < MAX_VNODES; i++) {
        if (vnodes[i].refcount == 0 && vnodes[i].mount == 0) {
            slot = &vnodes[i];
            break;
        }
    }
    if (!slot) {
        return 0; // pool exhausted -- caller reports -ENFILE
    }

    slot->mount = m;
    slot->inode_id = inode_id;
    slot->fs_private = 0;
    slot->refcount = 1;
    if (m->ops->read_inode(m, inode_id, slot) != 0) {
        slot->refcount = 0;
        slot->mount = 0;
        return 0;
    }

    slot->next = buckets[b];
    buckets[b] = slot;
    return slot;
}

void vnode_put(struct vnode *vn) {
    if (!vn || vn->refcount == 0) {
        return;
    }
    vn->refcount--;
    if (vn->refcount > 0) {
        return;
    }

    vn->mount->ops->sync_inode(vn);

    unsigned b = bucket_of(vn->mount, vn->inode_id);
    struct vnode **link = &buckets[b];
    while (*link && *link != vn) {
        link = &(*link)->next;
    }
    if (*link == vn) {
        *link = vn->next;
    }
    vn->next = 0;
    vn->mount = 0;
    vn->fs_private = 0;
}
```

- [x] **Step 3: Add mount, umount, and longest-prefix dispatch to `kernel/fs/vfs.c`**

Append:

```c
// Returns the mount owning `path` (longest matching mount-point
// prefix) and points *out_rel at the path relative to that mount,
// always starting with '/'. Resolution is done once, up front: unlike
// real Unix, no per-directory mount check happens during the walk.
// Results are identical here, including correct shadowing -- a mount
// at /mnt hides any real /mnt directory on the root volume.
static struct vfs_mount *mount_for(const char *path, const char **out_rel) {
    struct vfs_mount *best = 0;
    uint64_t best_len = 0;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            continue;
        }
        uint64_t mlen = str_len(mounts[i].path);
        // "/" matches everything; any other prefix must be followed by
        // '/' or end-of-string so that /mnt does not match /mnttab.
        int prefix_ok = 1;
        for (uint64_t k = 0; k < mlen; k++) {
            if (path[k] != mounts[i].path[k]) { prefix_ok = 0; break; }
        }
        if (!prefix_ok) {
            continue;
        }
        if (mlen > 1 && path[mlen] != '/' && path[mlen] != '\0') {
            continue;
        }
        if (mlen >= best_len) {
            best = &mounts[i];
            best_len = mlen;
        }
    }

    if (!best) {
        return 0;
    }
    *out_rel = (best_len > 1) ? path + best_len : path;
    if ((*out_rel)[0] == '\0') {
        *out_rel = "/";
    }
    return best;
}

int vfs_mount_fs(const char *source, const char *target, const char *fstype) {
    const char *rel;
    struct vfs_mount *existing = mount_for(target, &rel);
    if (existing && str_eq(existing->path, target)) {
        return -EEXIST;
    }

    struct vfs_mount *m = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) { m = &mounts[i]; break; }
    }
    if (!m) {
        return -ENOSPC;
    }

    if (str_eq(fstype, "ramfs")) {
        m->ops = &ramfs_ops;
    } else {
        return -ENODEV; // "fat" and "devfs" arrive in later tasks
    }

    str_copy(m->path, target, VFS_MAX_PATH);
    m->fs_private = 0;
    m->root = 0;
    m->in_use = 1;

    int rc = m->ops->mount(m, source);
    if (rc != 0) {
        m->in_use = 0;
        return rc;
    }

    m->root = vnode_get(m, 0); // reserved id 0 is every driver's root
    if (!m->root) {
        m->ops->umount(m);
        m->in_use = 0;
        return -ENFILE;
    }

    serial_write_string("[vfs] mounted ");
    serial_write_string(fstype);
    serial_write_string(" at ");
    serial_write_string(target);
    serial_write_string("\n");
    return 0;
}

int vfs_umount(const char *target) {
    struct vfs_mount *m = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].in_use && str_eq(mounts[i].path, target)) {
            m = &mounts[i];
            break;
        }
    }
    if (!m) {
        return -ENOENT;
    }

    // The root vnode's own reference is ours, so anything above 1 --
    // or any OTHER vnode on this mount still held -- means live users.
    for (int i = 0; i < MAX_VNODES; i++) {
        if (vnodes[i].mount != m) {
            continue;
        }
        if (&vnodes[i] == m->root) {
            if (vnodes[i].refcount > 1) { return -EBUSY; }
        } else if (vnodes[i].refcount > 0) {
            return -EBUSY;
        }
    }

    vnode_put(m->root);
    m->root = 0;
    m->ops->umount(m);
    m->in_use = 0;
    return 0;
}
```

- [x] **Step 4: Add path resolution to `kernel/fs/vfs.c`**

Append:

```c
// Copies the next '/'-delimited component of *p into out and advances
// *p past it. Returns 0 when there are no components left.
static int next_component(const char **p, char *out) {
    const char *s = *p;
    while (*s == '/') s++;
    if (*s == '\0') { *p = s; return 0; }

    uint64_t i = 0;
    while (*s && *s != '/' && i < VFS_NAME_MAX - 1) {
        out[i++] = *s++;
    }
    out[i] = '\0';
    while (*s && *s != '/') s++; // discard anything over VFS_NAME_MAX-1
    *p = s;
    return 1;
}

// Shared by vfs_resolve and vfs_resolve_parent. `stop_short` means
// "return the parent of the last component instead of the component
// itself", and then the last component is copied into out_name.
static struct vnode *resolve_walk(const char *path, int stop_short,
                                  char *out_name, int *out_err) {
    const char *rel;
    struct vfs_mount *m = mount_for(path, &rel);
    if (!m) {
        *out_err = -ENOENT;
        return 0;
    }

    struct vnode *dir = m->root;
    dir->refcount++; // we hand back a reference the caller must put

    char name[VFS_NAME_MAX];
    const char *cursor = rel;
    while (next_component(&cursor, name)) {
        if (stop_short) {
            // Peek: if nothing follows, `name` is the final component
            // and `dir` is already the parent we want.
            const char *peek = cursor;
            char discard[VFS_NAME_MAX];
            if (!next_component(&peek, discard)) {
                if (out_name) { str_copy(out_name, name, VFS_NAME_MAX); }
                return dir;
            }
        }

        if (dir->type != VNODE_DIR) {
            vnode_put(dir);
            *out_err = -ENOTDIR;
            return 0;
        }

        uint64_t child_id;
        int rc = dir->mount->ops->lookup(dir, name, &child_id);
        if (rc != 0) {
            vnode_put(dir);
            *out_err = rc;
            return 0;
        }

        struct vnode *child = vnode_get(dir->mount, child_id);
        // Release the parent whether or not the child materialised --
        // a failed walk must leak no references.
        vnode_put(dir);
        if (!child) {
            *out_err = -ENFILE;
            return 0;
        }
        dir = child;
    }

    if (stop_short) {
        // Path was the mount root itself, e.g. "/" -- no final
        // component exists to create or unlink.
        vnode_put(dir);
        *out_err = -EINVAL;
        return 0;
    }
    return dir;
}

struct vnode *vfs_resolve(const char *path, int *out_err) {
    return resolve_walk(path, 0, 0, out_err);
}

struct vnode *vfs_resolve_parent(const char *path, char *out_name, int *out_err) {
    return resolve_walk(path, 1, out_name, out_err);
}
```

- [x] **Step 5: Add `kernel/fs/ramfs.h`**

```c
#ifndef NEOOS_RAMFS_H
#define NEOOS_RAMFS_H

#include "vfs.h"

#define RAMFS_MAX_NODES 32
#define RAMFS_MAX_PAGES 4   // 16KiB ceiling per file

extern const struct vfs_ops ramfs_ops;

#endif
```

- [x] **Step 6: Add `kernel/fs/ramfs.c` — mount, lookup, create, read, write**

`mkdir`, `unlink`, and `readdir` are stubs returning `-EPERM` here; Task 3 implements them.

```c
#include "ramfs.h"
#include "errno.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"

struct ramfs_node {
    int             in_use;
    char            name[VFS_NAME_MAX];
    uint32_t        parent;                  // node index; node 0 is the root
    enum vnode_type type;
    uint32_t        size;
    uint64_t        pages[RAMFS_MAX_PAGES];  // physical frames, 0 = unallocated
};

// One global pool: this milestone mounts exactly one ramfs. A second
// ramfs mount would share it, which is why vfs_mount_fs rejects a
// duplicate target and nothing here mounts ramfs twice.
static struct ramfs_node nodes[RAMFS_MAX_NODES];

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void name_copy(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < VFS_NAME_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int ramfs_mount_op(struct vfs_mount *m, const char *source) {
    (void)source;
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        nodes[i].in_use = 0;
        for (int p = 0; p < RAMFS_MAX_PAGES; p++) { nodes[i].pages[p] = 0; }
    }
    nodes[0].in_use = 1;
    nodes[0].type = VNODE_DIR;
    nodes[0].parent = 0;
    nodes[0].size = 0;
    name_copy(nodes[0].name, "/");
    m->fs_private = nodes;
    return 0;
}

static void ramfs_umount_op(struct vfs_mount *m) {
    (void)m;
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        for (int p = 0; p < RAMFS_MAX_PAGES; p++) {
            if (nodes[i].pages[p]) {
                pmm_free(nodes[i].pages[p], 0);
                nodes[i].pages[p] = 0;
            }
        }
        nodes[i].in_use = 0;
    }
}

static int ramfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    (void)m;
    if (inode_id >= RAMFS_MAX_NODES || !nodes[inode_id].in_use) {
        return -ENOENT;
    }
    out->type = nodes[inode_id].type;
    out->size = nodes[inode_id].size;
    out->fs_private = &nodes[inode_id];
    return 0;
}

static int ramfs_sync_inode(struct vnode *vn) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    if (n) { n->size = vn->size; }
    return 0;
}

static int ramfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    struct ramfs_node *d = (struct ramfs_node *)dir->fs_private;
    uint32_t dir_index = (uint32_t)(d - nodes);
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].in_use && nodes[i].parent == dir_index &&
            (uint32_t)i != dir_index && name_eq(nodes[i].name, name)) {
            *out_inode_id = (uint64_t)i;
            return 0;
        }
    }
    return -ENOENT;
}

static int64_t ramfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    if (pos >= n->size) { return 0; }
    if (pos + len > n->size) { len = n->size - pos; }

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t done = 0; done < len; ) {
        uint32_t off = pos + done;
        uint32_t page = off / PMM_FRAME_SIZE;
        uint32_t in_page = off % PMM_FRAME_SIZE;
        uint32_t chunk = PMM_FRAME_SIZE - in_page;
        if (chunk > len - done) { chunk = len - done; }
        if (page >= RAMFS_MAX_PAGES || !n->pages[page]) { return (int64_t)done; }
        uint8_t *src = (uint8_t *)phys_to_virt(n->pages[page]) + in_page;
        for (uint32_t k = 0; k < chunk; k++) { dst[done + k] = src[k]; }
        done += chunk;
    }
    return (int64_t)len;
}

static int64_t ramfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    if (pos + len > RAMFS_MAX_PAGES * PMM_FRAME_SIZE) { return -ENOSPC; }

    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t done = 0; done < len; ) {
        uint32_t off = pos + done;
        uint32_t page = off / PMM_FRAME_SIZE;
        uint32_t in_page = off % PMM_FRAME_SIZE;
        uint32_t chunk = PMM_FRAME_SIZE - in_page;
        if (chunk > len - done) { chunk = len - done; }

        if (!n->pages[page]) {
            uint64_t frame = pmm_alloc(0);
            if (!frame) { return -ENOSPC; }
            uint8_t *z = (uint8_t *)phys_to_virt(frame);
            for (uint32_t k = 0; k < PMM_FRAME_SIZE; k++) { z[k] = 0; }
            n->pages[page] = frame;
        }
        uint8_t *dst = (uint8_t *)phys_to_virt(n->pages[page]) + in_page;
        for (uint32_t k = 0; k < chunk; k++) { dst[k] = src[done + k]; }
        done += chunk;
    }

    if (pos + len > n->size) {
        n->size = pos + len;
        vn->size = n->size;
    }
    return (int64_t)len;
}

static int ramfs_create(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    uint64_t existing;
    if (ramfs_lookup(dir, name, &existing) == 0) { return -EEXIST; }

    struct ramfs_node *d = (struct ramfs_node *)dir->fs_private;
    for (int i = 1; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].in_use) { continue; }
        nodes[i].in_use = 1;
        nodes[i].type = VNODE_FILE;
        nodes[i].parent = (uint32_t)(d - nodes);
        nodes[i].size = 0;
        name_copy(nodes[i].name, name);
        *out_inode_id = (uint64_t)i;
        return 0;
    }
    return -ENOSPC;
}

static int ramfs_truncate(struct vnode *vn) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    for (int p = 0; p < RAMFS_MAX_PAGES; p++) {
        if (n->pages[p]) { pmm_free(n->pages[p], 0); n->pages[p] = 0; }
    }
    n->size = 0;
    vn->size = 0;
    return 0;
}

static int ramfs_mkdir_stub(struct vnode *dir, const char *name) {
    (void)dir; (void)name;
    return -EPERM; // implemented in Task 3
}

static int ramfs_unlink_stub(struct vnode *dir, const char *name) {
    (void)dir; (void)name;
    return -EPERM; // implemented in Task 3
}

static int ramfs_readdir_stub(struct vnode *dir, uint32_t index, struct dirent *out) {
    (void)dir; (void)index; (void)out;
    return -EPERM; // implemented in Task 3
}

const struct vfs_ops ramfs_ops = {
    .mount      = ramfs_mount_op,
    .umount     = ramfs_umount_op,
    .read_inode = ramfs_read_inode,
    .sync_inode = ramfs_sync_inode,
    .lookup     = ramfs_lookup,
    .read       = ramfs_read,
    .write      = ramfs_write,
    .create     = ramfs_create,
    .mkdir      = ramfs_mkdir_stub,
    .unlink     = ramfs_unlink_stub,
    .truncate   = ramfs_truncate,
    .readdir    = ramfs_readdir_stub,
};
```

- [x] **Step 7: Add `vfs_selftest` to `kernel/fs/vfs.c`**

Append. This is the task's test: it proves mount, create, write, read-back, the vnode cache's aliasing, and `umount`'s busy check.

```c
void vfs_selftest(void) {
    int err = 0;

    char name[VFS_NAME_MAX];
    struct vnode *dir = vfs_resolve_parent("/T.TXT", name, &err);
    if (!dir) {
        serial_write_string("[vfs] selftest FAILED: resolve_parent /T.TXT\n");
        return;
    }
    uint64_t id;
    if (dir->mount->ops->create(dir, name, &id) != 0) {
        serial_write_string("[vfs] selftest FAILED: create\n");
        vnode_put(dir);
        return;
    }
    vnode_put(dir);

    struct vnode *a = vfs_resolve("/T.TXT", &err);
    if (!a) {
        serial_write_string("[vfs] selftest FAILED: resolve after create\n");
        return;
    }
    const char *msg = "vfs-ok";
    if (a->mount->ops->write(a, 0, msg, 6) != 6) {
        serial_write_string("[vfs] selftest FAILED: write\n");
        vnode_put(a);
        return;
    }

    // Second resolve of the same path MUST return the same vnode --
    // that is the whole point of the cache, and it is what makes the
    // write above visible here without reopening.
    struct vnode *b = vfs_resolve("/T.TXT", &err);
    if (b != a) {
        serial_write_string("[vfs] selftest FAILED: cache did not alias\n");
        vnode_put(a);
        if (b) { vnode_put(b); }
        return;
    }

    char buf[8] = {0};
    if (b->mount->ops->read(b, 0, buf, 6) != 6 ||
        buf[0] != 'v' || buf[5] != 'k') {
        serial_write_string("[vfs] selftest FAILED: readback mismatch\n");
        vnode_put(a); vnode_put(b);
        return;
    }

    if (vfs_umount("/tmp") != -EBUSY) {
        serial_write_string("[vfs] selftest FAILED: umount did not report busy\n");
        vnode_put(a); vnode_put(b);
        return;
    }

    // A failed walk must release every reference it took on the way
    // in. This is the milestone's most likely refcount bug, so it gets
    // a direct check rather than relying on the end-of-milestone leak
    // gate to notice it later.
    uint32_t before = vfs_vnode_in_use_count();
    int missing_err = 0;
    if (vfs_resolve("/tmp/NO/SUCH.TXT", &missing_err) != 0) {
        serial_write_string("[vfs] selftest FAILED: resolve of a missing path succeeded\n");
        vnode_put(a); vnode_put(b);
        return;
    }
    if (vfs_vnode_in_use_count() != before) {
        serial_write_string("[vfs] selftest FAILED: failed walk leaked a vnode\n");
        vnode_put(a); vnode_put(b);
        return;
    }

    vnode_put(a);
    vnode_put(b);
    serial_write_string("[vfs] selftest passed\n");
}
```

- [x] **Step 8: Wire the new directory into the Makefile**

`C_SOURCES` already globs `kernel/*.c` and `kernel/mm/*.c`. Add the new directory:

```makefile
C_SOURCES := $(wildcard kernel/*.c) $(wildcard kernel/mm/*.c) $(wildcard kernel/fs/*.c)
```

The existing pattern rule `$(BUILD_DIR)/%.o: kernel/%.c` already does `mkdir -p $(dir $@)`, so `build/fs/` is created automatically.

- [x] **Step 9: Call it from `kmain`**

In `kernel/kernel.c`, add `#include "fs/vfs.h"` and, after `fat16_write_selftest();`:

```c
    vfs_init();
    vfs_mount_fs(0, "/tmp", "ramfs");
    vfs_selftest();
```

- [x] **Step 10: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "\[vfs\]" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: `[vfs] initialized`, `[vfs] mounted ramfs at /tmp`, `[vfs] selftest passed`. `FAILED|exception` count 0, and all milestone 5-9 programs unchanged.

- [x] **Step 11: Commit**

```bash
git add kernel/fs/vfs.h kernel/fs/vfs.c kernel/fs/ramfs.h kernel/fs/ramfs.c kernel/kernel.c Makefile
git commit -m "Add VFS core: vnode cache, mount table, path resolution, and ramfs"
```

---

### Task 3: Complete ramfs — mkdir, unlink, readdir

**Files:**
- Modify: `kernel/fs/ramfs.c`, `kernel/fs/vfs.c`

**Interfaces:**
- Produces: working `mkdir`/`unlink`/`readdir` in `ramfs_ops`. `readdir`'s contract — return 0 and fill `*out` for a valid `index`, return `-ENOENT` once `index` is past the last entry — is the contract Task 10's `getdents` relies on and every other driver must match.
- Consumes: Task 2's `ramfs_node` pool and `struct dirent`.

- [x] **Step 1: Replace the three stubs in `kernel/fs/ramfs.c`**

Delete `ramfs_mkdir_stub`, `ramfs_unlink_stub`, and `ramfs_readdir_stub`, and add:

```c
static int ramfs_mkdir(struct vnode *dir, const char *name) {
    uint64_t existing;
    if (ramfs_lookup(dir, name, &existing) == 0) { return -EEXIST; }

    struct ramfs_node *d = (struct ramfs_node *)dir->fs_private;
    for (int i = 1; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].in_use) { continue; }
        nodes[i].in_use = 1;
        nodes[i].type = VNODE_DIR;
        nodes[i].parent = (uint32_t)(d - nodes);
        nodes[i].size = 0;
        name_copy(nodes[i].name, name);
        return 0;
    }
    return -ENOSPC;
}

static int ramfs_unlink(struct vnode *dir, const char *name) {
    uint64_t id;
    int rc = ramfs_lookup(dir, name, &id);
    if (rc != 0) { return rc; }
    if (nodes[id].type == VNODE_DIR) { return -EISDIR; }

    for (int p = 0; p < RAMFS_MAX_PAGES; p++) {
        if (nodes[id].pages[p]) { pmm_free(nodes[id].pages[p], 0); nodes[id].pages[p] = 0; }
    }
    nodes[id].in_use = 0;
    return 0;
}

// Enumerates the dir's children by ordinal. `index` counts only
// matching children, so callers can walk 0,1,2,... until -ENOENT
// without knowing anything about the pool's internal layout.
static int ramfs_readdir(struct vnode *dir, uint32_t index, struct dirent *out) {
    struct ramfs_node *d = (struct ramfs_node *)dir->fs_private;
    uint32_t dir_index = (uint32_t)(d - nodes);
    uint32_t seen = 0;

    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].in_use || nodes[i].parent != dir_index || (uint32_t)i == dir_index) {
            continue;
        }
        if (seen == index) {
            name_copy(out->name, nodes[i].name);
            out->type = (nodes[i].type == VNODE_DIR) ? DT_DIR : DT_REG;
            return 0;
        }
        seen++;
    }
    return -ENOENT; // past the last entry
}
```

Update the ops table's three entries to `.mkdir = ramfs_mkdir`, `.unlink = ramfs_unlink`, `.readdir = ramfs_readdir`.

- [x] **Step 2: Extend `vfs_selftest` to cover them**

In `kernel/fs/vfs.c`, insert before the final `serial_write_string("[vfs] selftest passed\n");` (and after the two `vnode_put` calls, so nothing is held):

```c
    struct vnode *root = vfs_resolve("/tmp", &err);
    if (!root) {
        serial_write_string("[vfs] selftest FAILED: resolve /tmp\n");
        return;
    }
    if (root->mount->ops->mkdir(root, "SUB") != 0) {
        serial_write_string("[vfs] selftest FAILED: mkdir\n");
        vnode_put(root);
        return;
    }

    // Walk the directory by ordinal until -ENOENT; we expect exactly
    // T.TXT (a file) and SUB (a directory), in either order.
    struct dirent de;
    int files = 0, dirs = 0;
    for (uint32_t i = 0; root->mount->ops->readdir(root, i, &de) == 0; i++) {
        if (de.type == DT_DIR) { dirs++; } else { files++; }
    }
    if (files != 1 || dirs != 1) {
        serial_write_string("[vfs] selftest FAILED: readdir count wrong\n");
        vnode_put(root);
        return;
    }

    if (root->mount->ops->unlink(root, "T.TXT") != 0) {
        serial_write_string("[vfs] selftest FAILED: unlink\n");
        vnode_put(root);
        return;
    }
    if (root->mount->ops->unlink(root, "SUB") != -EISDIR) {
        serial_write_string("[vfs] selftest FAILED: unlink of a dir should be EISDIR\n");
        vnode_put(root);
        return;
    }
    vnode_put(root);
```

- [x] **Step 3: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "\[vfs\]" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: `[vfs] selftest passed` still prints, now having exercised mkdir, readdir, and unlink. Zero `FAILED`, zero exceptions.

- [x] **Step 4: Commit**

```bash
git add kernel/fs/ramfs.c kernel/fs/vfs.c
git commit -m "Complete ramfs with mkdir, unlink, and readdir"
```

---

### Task 4: FAT per-volume state refactor (no behavior change)

**Files:**
- Modify: `kernel/fat16.c`

**Interfaces:**
- Produces: `struct fat_volume` and every internal FAT helper taking it as its first parameter. The public `fat16_*` API in `kernel/fat16.h` is **unchanged** — this task is deliberately invisible from outside the file.
- Consumes: Task 1's `ata_read_sectors(drive, ...)`.

This isolates the single largest mechanical change in the milestone — replacing eight file-scope globals across roughly twenty functions in a 927-line file — behind a guarantee of zero semantic change. Landing it separately means that if the next task regresses, the refactor is already known-good.

- [x] **Step 1: Define the volume struct and a single static instance**

In `kernel/fat16.c`, replace these eight globals:

```c
static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint32_t fat_start_lba;
static uint32_t root_dir_start_lba;
static uint32_t root_dir_sector_count;
static uint32_t data_start_lba;
static uint16_t root_entry_count;
static uint16_t sectors_per_fat_g;
```

with:

```c
// Per-volume geometry. Was eight file-scope globals; becoming a struct
// is what lets a second volume exist at all. FAT32 fields are unused
// until the variant work lands, but live here from the start so that
// change touches only the code that reads them.
struct fat_volume {
    uint8_t  drive;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t sectors_per_fat;
    uint32_t root_dir_start_lba;    // FAT16 only
    uint32_t root_dir_sector_count; // FAT16 only
    uint16_t root_entry_count;      // FAT16 only
    uint32_t root_cluster;          // FAT32 only
};

// The one volume the legacy fat16_* API operates on. It disappears
// when that API does; until then it keeps this refactor invisible to
// callers.
static struct fat_volume legacy_volume;
```

- [x] **Step 2: Thread the volume through every internal helper**

Give each of these a leading `struct fat_volume *v` parameter and replace every bare global reference inside with `v->field` (e.g. `data_start_lba` becomes `v->data_start_lba`, `sectors_per_fat_g` becomes `v->sectors_per_fat`):

```
cluster_to_lba
fat16_next_cluster
fat16_set_next_cluster
fat16_alloc_cluster
fat16_free_chain
cluster_at_offset
write_range
scan_sector_for_name
find_in_root
find_in_directory_cluster
write_dirent
create_entry_in_directory
resolve_parent
```

Each `ata_read_sectors(0, ...)` and `ata_write_sectors(0, ...)` inside them becomes `ata_read_sectors(v->drive, ...)` / `ata_write_sectors(v->drive, ...)`.

Verify none were missed — after this step the following must print nothing:

```bash
grep -n "bytes_per_sector\|sectors_per_cluster\|fat_start_lba\|root_dir_start_lba\|root_dir_sector_count\|data_start_lba\|root_entry_count\|sectors_per_fat_g" kernel/fat16.c \
  | grep -v "v->" | grep -v "struct fat_volume" | grep -v "bpb->" | grep -v "legacy_volume"
```

- [x] **Step 3: Point the public API at `legacy_volume`**

The nine public functions keep their exact signatures and each passes `&legacy_volume` down. For example:

```c
uint32_t fat16_read_file(uint16_t first_cluster, uint32_t size, void *buffer) {
    struct fat_volume *v = &legacy_volume;
    /* ...body unchanged except that helper calls now pass v... */
}
```

Apply the same one-line prologue to `fat16_find`, `fat16_read_at`, `fat16_write_file`, `fat16_truncate`, `fat16_create_file`, `fat16_mkdir`, `fat16_delete_entry`, and `fat16_update_entry_size`.

`fat16_mount` fills the struct instead of the globals:

```c
int fat16_mount(void) {
    struct fat_volume *v = &legacy_volume;
    v->drive = 0;

    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(v->drive, 0, 1, sector)) {
        serial_write_string("[fat16] mount FAILED: could not read boot sector\n");
        return 0;
    }

    struct fat16_bpb *bpb = (struct fat16_bpb *)sector;
    v->bytes_per_sector = bpb->bytes_per_sector;
    v->sectors_per_cluster = bpb->sectors_per_cluster;
    v->root_entry_count = bpb->root_entry_count;
    v->sectors_per_fat = bpb->sectors_per_fat;

    v->fat_start_lba = bpb->reserved_sector_count;
    v->root_dir_start_lba = v->fat_start_lba + (uint32_t)bpb->num_fats * bpb->sectors_per_fat;
    v->root_dir_sector_count = ((uint32_t)v->root_entry_count * sizeof(struct fat16_dirent)
                                 + v->bytes_per_sector - 1) / v->bytes_per_sector;
    v->data_start_lba = v->root_dir_start_lba + v->root_dir_sector_count;

    /* ...existing log lines, reading v->... instead of the globals... */
    return 1;
}
```

- [x] **Step 4: Build and verify no behavior changed**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "fat16\|selftest" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: `[fat16] mounted:` reports the identical geometry values as before the refactor, `[fat16] selftest passed` and `[fat16] write selftest passed` both appear, `fileio` behaves exactly as before. Zero `FAILED`, zero exceptions. This task changes nothing observable — any diff in the log is a bug.

- [x] **Step 5: Commit**

```bash
git add kernel/fat16.c
git commit -m "Move FAT16 geometry into a per-volume struct"
```

---

### Task 5: FAT as a VFS driver, mounted at `/`

**Files:**
- Create: `kernel/fs/fatfs.h`
- Rename: `kernel/fat16.c` → `kernel/fs/fatfs.c`, `kernel/fat16.h` → `kernel/fs/fatfs.h` (see Step 1)
- Modify: `kernel/fs/vfs.c`, `kernel/kernel.c`, `kernel/syscall.c`

**Interfaces:**
- Produces: `extern const struct vfs_ops fatfs_ops;` and `int fatfs_mount_volume(struct vfs_mount *m, const char *source)`. FAT inode ids are `(dir_entry_lba << 16) | dir_entry_offset`, with reserved id 0 for the root directory.
- Consumes: Task 4's `struct fat_volume`, Task 2's `vfs_ops` contract.

The legacy `fat16_*` API stays in place and `syscall.c` still uses it; both paths coexist for exactly one task so a failure here is attributable to the new driver alone. Task 6 removes the old path.

- [x] **Step 1: Move the file and update includes**

```bash
git mv kernel/fat16.c kernel/fs/fatfs.c
git mv kernel/fat16.h kernel/fs/fatfs.h
```

In `kernel/fs/fatfs.c`, fix the now-relative includes: `#include "fat16.h"` becomes `#include "fatfs.h"`, and `#include "ata.h"` / `#include "serial.h"` become `#include "../ata.h"` / `#include "../serial.h"`. In `kernel/fs/fatfs.h`, `#include "errno.h"` becomes `#include "../errno.h"`.

Every other file that included `"fat16.h"` (`kernel/kernel.c`, `kernel/syscall.c`, `kernel/process.c`) now includes `"fs/fatfs.h"`.

Rename the include guard in `fatfs.h` from `NEOOS_FAT16_H` to `NEOOS_FATFS_H`.

- [x] **Step 2: Declare the driver in `kernel/fs/fatfs.h`**

Add, above the existing legacy declarations:

```c
#include "vfs.h"

// FAT inode identity is the file's directory-entry location on disk,
// (dir_entry_lba << 16) | dir_entry_offset -- unique per file per
// volume. First cluster will not serve: every empty file has cluster
// 0. Reserved id 0 means the root directory, which has no directory
// entry of its own; that cannot collide with a real entry, because id
// 0 requires dir_entry_lba == 0 and LBA 0 is the boot sector.
#define FATFS_ROOT_INODE 0ULL

extern const struct vfs_ops fatfs_ops;
```

- [x] **Step 3: Add the per-vnode private struct and the ops in `kernel/fs/fatfs.c`**

Append to the file:

```c
// Per-open-file driver state, hung off vnode->fs_private. These are
// exactly the three fields that used to live in struct
// file_descriptor -- moving them here is what decouples syscall.c
// from FAT.
struct fatfs_inode {
    int      in_use;
    uint16_t first_cluster;
    uint32_t dir_entry_lba;
    uint16_t dir_entry_offset;
    uint32_t size;
    int      is_dir;
};

#define FATFS_MAX_INODES MAX_VNODES
static struct fatfs_inode inode_pool[FATFS_MAX_INODES];

// One volume struct per mount, indexed by however many mounts exist.
#define FATFS_MAX_VOLUMES 4
static struct fat_volume volumes[FATFS_MAX_VOLUMES];
static int volume_used[FATFS_MAX_VOLUMES];

static struct fatfs_inode *inode_alloc(void) {
    for (int i = 0; i < FATFS_MAX_INODES; i++) {
        if (!inode_pool[i].in_use) {
            inode_pool[i].in_use = 1;
            return &inode_pool[i];
        }
    }
    return 0;
}

static uint64_t inode_id_of(uint32_t lba, uint16_t offset) {
    return ((uint64_t)lba << 16) | offset;
}

static int fatfs_mount_op(struct vfs_mount *m, const char *source) {
    uint8_t drive;
    if (source && source[0] == 'h' && source[1] == 'd' && source[2] == '1') {
        drive = 1;
    } else if (source && source[0] == 'h' && source[1] == 'd' && source[2] == '0') {
        drive = 0;
    } else {
        return -ENODEV;
    }

    int slot = -1;
    for (int i = 0; i < FATFS_MAX_VOLUMES; i++) {
        if (!volume_used[i]) { slot = i; break; }
    }
    if (slot < 0) { return -ENOSPC; }

    struct fat_volume *v = &volumes[slot];
    v->drive = drive;
    if (!fat_read_bpb(v)) {   // added in Step 4
        return -ENODEV;
    }

    volume_used[slot] = 1;
    m->fs_private = v;
    return 0;
}

static void fatfs_umount_op(struct vfs_mount *m) {
    struct fat_volume *v = (struct fat_volume *)m->fs_private;
    for (int i = 0; i < FATFS_MAX_VOLUMES; i++) {
        if (&volumes[i] == v) { volume_used[i] = 0; }
    }
    m->fs_private = 0;
}

static int fatfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    struct fat_volume *v = (struct fat_volume *)m->fs_private;

    struct fatfs_inode *n = inode_alloc();
    if (!n) { return -ENFILE; }

    if (inode_id == FATFS_ROOT_INODE) {
        n->first_cluster = 0;      // FAT16: the fixed root region
        n->dir_entry_lba = 0;
        n->dir_entry_offset = 0;
        n->size = 0;
        n->is_dir = 1;
        out->type = VNODE_DIR;
        out->size = 0;
        out->fs_private = n;
        return 0;
    }

    uint32_t lba = (uint32_t)(inode_id >> 16);
    uint16_t off = (uint16_t)(inode_id & 0xFFFF);

    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(v->drive, lba, 1, sector)) {
        n->in_use = 0;
        return -ENOENT;
    }
    struct fat16_dirent *de = (struct fat16_dirent *)(sector + off);

    n->first_cluster = de->first_cluster_low;
    n->dir_entry_lba = lba;
    n->dir_entry_offset = off;
    n->size = de->file_size;
    n->is_dir = (de->attr & ATTR_DIRECTORY) != 0;

    out->type = n->is_dir ? VNODE_DIR : VNODE_FILE;
    out->size = n->size;
    out->fs_private = n;
    return 0;
}

static int fatfs_sync_inode(struct vnode *vn) {
    struct fatfs_inode *n = (struct fatfs_inode *)vn->fs_private;
    if (!n) { return 0; }
    // Push any size/cluster change back into the directory entry. The
    // root has no entry to patch.
    if (vn->inode_id != FATFS_ROOT_INODE &&
        (n->size != vn->size || n->dirty_cluster)) {
        fat16_update_entry_size(n->dir_entry_lba, n->dir_entry_offset,
                                n->first_cluster, vn->size);
    }
    n->in_use = 0;
    return 0;
}
```

Add `int dirty_cluster;` to `struct fatfs_inode` (set by `fatfs_write` below when the first cluster changes), initialised to 0 in `inode_alloc`.

- [x] **Step 4: Factor BPB parsing into a reusable helper**

`fat16_mount` currently parses the boot sector inline. Extract it so the driver's mount path can reuse it. In `kernel/fs/fatfs.c`, add above `fat16_mount`:

```c
// Reads and parses the boot sector into v, which must already have
// v->drive set. Returns 1 on success. Shared by the legacy
// fat16_mount() and the VFS driver's mount op.
static int fat_read_bpb(struct fat_volume *v) {
    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(v->drive, 0, 1, sector)) {
        return 0;
    }
    struct fat16_bpb *bpb = (struct fat16_bpb *)sector;
    v->bytes_per_sector = bpb->bytes_per_sector;
    v->sectors_per_cluster = bpb->sectors_per_cluster;
    v->root_entry_count = bpb->root_entry_count;
    v->sectors_per_fat = bpb->sectors_per_fat;
    v->fat_start_lba = bpb->reserved_sector_count;
    v->root_dir_start_lba = v->fat_start_lba + (uint32_t)bpb->num_fats * bpb->sectors_per_fat;
    v->root_dir_sector_count = ((uint32_t)v->root_entry_count * sizeof(struct fat16_dirent)
                                 + v->bytes_per_sector - 1) / v->bytes_per_sector;
    v->data_start_lba = v->root_dir_start_lba + v->root_dir_sector_count;
    return 1;
}
```

and rewrite `fat16_mount`'s body to call it:

```c
int fat16_mount(void) {
    struct fat_volume *v = &legacy_volume;
    v->drive = 0;
    if (!fat_read_bpb(v)) {
        serial_write_string("[fat16] mount FAILED: could not read boot sector\n");
        return 0;
    }
    /* ...existing log lines, unchanged... */
    return 1;
}
```

- [x] **Step 5: Implement lookup, read, write, create, mkdir, unlink, truncate, readdir**

Append to `kernel/fs/fatfs.c`. These wrap the existing internal helpers, which already do the real work:

```c
static int fatfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;

    uint8_t fat_name[11];
    to_fat_name(name, fat_name);

    struct fat16_dirent found;
    uint32_t lba;
    uint16_t off;
    int ok = (dir->inode_id == FATFS_ROOT_INODE)
           ? find_in_root(v, fat_name, &found, &lba, &off)
           : find_in_directory_cluster(v, d->first_cluster, fat_name, &found, &lba, &off);
    if (!ok) {
        return -ENOENT;
    }
    *out_inode_id = inode_id_of(lba, off);
    return 0;
}

static int64_t fatfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    struct fat_volume *v = (struct fat_volume *)vn->mount->fs_private;
    struct fatfs_inode *n = (struct fatfs_inode *)vn->fs_private;
    if (pos >= vn->size) { return 0; }
    if (pos + len > vn->size) { len = vn->size - pos; }
    fat16_read_at_v(v, n->first_cluster, pos, buf, len);
    return (int64_t)len;
}

static int64_t fatfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    struct fat_volume *v = (struct fat_volume *)vn->mount->fs_private;
    struct fatfs_inode *n = (struct fatfs_inode *)vn->fs_private;

    uint16_t new_cluster;
    uint32_t new_size;
    int rc = fat16_write_file_v(v, n->first_cluster, vn->size, pos, buf, len,
                                &new_cluster, &new_size);
    if (rc < 0) { return rc; }

    if (new_cluster != n->first_cluster) {
        n->first_cluster = new_cluster;
        n->dirty_cluster = 1;
    }
    vn->size = new_size;
    // Patch the directory entry now rather than at sync_inode time:
    // the entry must be correct on disk even if the machine stops
    // before the last fd closes.
    fat16_update_entry_size_v(v, n->dir_entry_lba, n->dir_entry_offset, new_cluster, new_size);
    n->size = new_size;
    return (int64_t)len;
}

static int fatfs_create(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;

    uint8_t fat_name[11];
    to_fat_name(name, fat_name);

    uint32_t lba;
    uint16_t off;
    int rc = create_entry_in_directory(v, d->first_cluster,
                                       dir->inode_id == FATFS_ROOT_INODE,
                                       fat_name, 0 /* not a directory */, &lba, &off);
    if (rc != 0) { return rc; }
    *out_inode_id = inode_id_of(lba, off);
    return 0;
}

static int fatfs_mkdir_op(struct vnode *dir, const char *name) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;

    uint8_t fat_name[11];
    to_fat_name(name, fat_name);

    uint32_t lba;
    uint16_t off;
    return create_entry_in_directory(v, d->first_cluster,
                                     dir->inode_id == FATFS_ROOT_INODE,
                                     fat_name, 1 /* directory */, &lba, &off);
}

static int fatfs_unlink_op(struct vnode *dir, const char *name) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;

    uint8_t fat_name[11];
    to_fat_name(name, fat_name);

    struct fat16_dirent found;
    uint32_t lba;
    uint16_t off;
    int ok = (dir->inode_id == FATFS_ROOT_INODE)
           ? find_in_root(v, fat_name, &found, &lba, &off)
           : find_in_directory_cluster(v, d->first_cluster, fat_name, &found, &lba, &off);
    if (!ok) { return -ENOENT; }
    if (found.attr & ATTR_DIRECTORY) { return -EISDIR; }

    fat16_free_chain(v, found.first_cluster_low);
    return mark_dirent_deleted(v, lba, off);   // added in Step 6
}

static int fatfs_truncate_op(struct vnode *vn) {
    struct fat_volume *v = (struct fat_volume *)vn->mount->fs_private;
    struct fatfs_inode *n = (struct fatfs_inode *)vn->fs_private;
    fat16_truncate_v(v, n->first_cluster, n->dir_entry_lba, n->dir_entry_offset,
                     &n->first_cluster);
    vn->size = 0;
    n->size = 0;
    return 0;
}

// Walks a directory by ordinal, skipping free (0x00), deleted (0xE5),
// and VFAT long-name (attr 0x0F) entries so the caller sees only real
// 8.3 entries. Contract matches ramfs: 0 and *out filled for a valid
// index, -ENOENT once past the last entry.
static int fatfs_readdir_op(struct vnode *dir, uint32_t index, struct dirent *out) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;
    return fat_dir_nth(v, d->first_cluster, dir->inode_id == FATFS_ROOT_INODE,
                       index, out);          // added in Step 6
}

const struct vfs_ops fatfs_ops = {
    .mount      = fatfs_mount_op,
    .umount     = fatfs_umount_op,
    .read_inode = fatfs_read_inode,
    .sync_inode = fatfs_sync_inode,
    .lookup     = fatfs_lookup,
    .read       = fatfs_read,
    .write      = fatfs_write,
    .create     = fatfs_create,
    .mkdir      = fatfs_mkdir_op,
    .unlink     = fatfs_unlink_op,
    .truncate   = fatfs_truncate_op,
    .readdir    = fatfs_readdir_op,
};
```

- [x] **Step 6: Add the three helpers the ops above call**

Two of the existing public functions need volume-taking twins, and two helpers are genuinely new. Add to `kernel/fs/fatfs.c`:

```c
// Volume-taking twins of the legacy public API. The legacy functions
// become one-line wrappers passing &legacy_volume, so there is exactly
// one implementation of each behaviour.
void fat16_read_at_v(struct fat_volume *v, uint16_t first_cluster,
                     uint32_t position, void *buf, uint32_t len);
int  fat16_write_file_v(struct fat_volume *v, uint16_t first_cluster,
                        uint32_t current_size, uint32_t position,
                        const void *buf, uint32_t len,
                        uint16_t *out_first_cluster, uint32_t *out_new_size);
void fat16_truncate_v(struct fat_volume *v, uint16_t first_cluster, uint32_t dir_lba,
                      uint16_t dir_offset, uint16_t *out_first_cluster);
void fat16_update_entry_size_v(struct fat_volume *v, uint32_t dir_lba, uint16_t dir_offset,
                               uint16_t first_cluster, uint32_t size);
```

Rename the four existing definitions to the `_v` names, give each a leading `struct fat_volume *v`, and replace each original with a wrapper, e.g.:

```c
void fat16_read_at(uint16_t first_cluster, uint32_t position, void *buf, uint32_t len) {
    fat16_read_at_v(&legacy_volume, first_cluster, position, buf, len);
}
```

Then add the two new helpers:

```c
// Marks a directory entry deleted in place by writing 0xE5 over the
// first name byte -- the same thing fat16_delete_entry already does
// inline; this exposes it for the VFS path.
static int mark_dirent_deleted(struct fat_volume *v, uint32_t lba, uint16_t offset) {
    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(v->drive, lba, 1, sector)) { return -EIO_SUBSTITUTE; }
    sector[offset] = 0xE5;
    if (!ata_write_sectors(v->drive, lba, 1, sector)) { return -EIO_SUBSTITUTE; }
    return 0;
}

// Returns the index'th real 8.3 entry of a directory. `in_root` picks
// the FAT16 fixed root region over a cluster chain.
static int fat_dir_nth(struct fat_volume *v, uint16_t dir_cluster, int in_root,
                       uint32_t index, struct dirent *out) {
    uint32_t seen = 0;
    uint32_t sectors = in_root ? v->root_dir_sector_count : v->sectors_per_cluster;
    uint16_t cluster = dir_cluster;

    for (;;) {
        uint32_t base = in_root ? v->root_dir_start_lba : cluster_to_lba(v, cluster);
        for (uint32_t s = 0; s < sectors; s++) {
            uint8_t sector[SECTOR_SIZE];
            if (!ata_read_sectors(v->drive, base + s, 1, sector)) { return -ENOENT; }
            for (uint32_t o = 0; o < SECTOR_SIZE; o += sizeof(struct fat16_dirent)) {
                struct fat16_dirent *de = (struct fat16_dirent *)(sector + o);
                if (de->name[0] == 0x00) { return -ENOENT; } // end of directory
                if (de->name[0] == 0xE5) { continue; }       // deleted
                if ((de->attr & 0x0F) == 0x0F) { continue; } // VFAT long-name
                if (seen == index) {
                    from_fat_name(de->name, out->name);      // added below
                    out->type = (de->attr & ATTR_DIRECTORY) ? DT_DIR : DT_REG;
                    return 0;
                }
                seen++;
            }
        }
        if (in_root) { return -ENOENT; }
        cluster = fat16_next_cluster(v, cluster);
        if (cluster >= 0xFFF8 || cluster < 2) { return -ENOENT; }
    }
}

// Inverse of to_fat_name: "FILE    TXT" -> "FILE.TXT".
static void from_fat_name(const uint8_t *raw, char *out) {
    int o = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) { out[o++] = (char)raw[i]; }
    if (raw[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) { out[o++] = (char)raw[i]; }
    }
    out[o] = '\0';
}
```

`EIO_SUBSTITUTE` is not a real code — replace both uses with `-ENOSPC`, which is what the existing FAT code already returns for a failed device write, and delete this note.

- [x] **Step 7: Register `"fat"` in `vfs_mount_fs`**

In `kernel/fs/vfs.c`, add `#include "fatfs.h"` and extend the driver selection:

```c
    if (str_eq(fstype, "ramfs")) {
        m->ops = &ramfs_ops;
    } else if (str_eq(fstype, "fat")) {
        m->ops = &fatfs_ops;
    } else {
        return -ENODEV; // "devfs" arrives in a later task
    }
```

- [x] **Step 8: Mount `/` at boot and read a known file through the VFS**

In `kernel/kernel.c`, change the VFS block to:

```c
    vfs_init();
    vfs_mount_fs("hd0", "/", "fat");
    vfs_mount_fs(0, "/tmp", "ramfs");
    vfs_selftest();
```

`/` must be mounted before `/tmp`, because `vfs_mount_fs` resolves nothing but the mount table itself — order only matters for the log's readability, but keep the root first for clarity.

Add to the end of `vfs_selftest` in `kernel/fs/vfs.c`, before the final pass line:

```c
    // Read a file that exists on the FAT16 root volume, proving the
    // driver works through the same interface ramfs just did.
    struct vnode *hello = vfs_resolve("/HELLO.TXT", &err);
    if (!hello) {
        serial_write_string("[vfs] selftest FAILED: resolve /HELLO.TXT\n");
        return;
    }
    char hbuf[16] = {0};
    if (hello->mount->ops->read(hello, 0, hbuf, 5) != 5 || hbuf[0] != 'H') {
        serial_write_string("[vfs] selftest FAILED: FAT read through VFS\n");
        vnode_put(hello);
        return;
    }
    vnode_put(hello);

    // And one in a subdirectory, proving multi-component resolution.
    struct vnode *nested = vfs_resolve("/DIR/NESTED.TXT", &err);
    if (!nested) {
        serial_write_string("[vfs] selftest FAILED: resolve /DIR/NESTED.TXT\n");
        return;
    }
    vnode_put(nested);
```

- [x] **Step 9: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "\[vfs\]\|\[fat16\]" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: `[vfs] mounted fat at /`, `[vfs] mounted ramfs at /tmp`, `[vfs] selftest passed`. The legacy FAT16 selftests still pass — both paths are live. Zero `FAILED`, zero exceptions.

- [x] **Step 10: Commit**

```bash
git add kernel/fs/fatfs.c kernel/fs/fatfs.h kernel/fs/vfs.c kernel/kernel.c kernel/syscall.c kernel/process.c
git commit -m "Add FAT as a VFS driver and mount it at /"
```

---

### Task 6: Rewrite the syscall layer against the VFS

**Files:**
- Modify: `kernel/syscall.c`, `kernel/process.h`, `kernel/process.c`, `kernel/fs/fatfs.h`, `kernel/fs/fatfs.c`

**Interfaces:**
- Produces: `struct file_descriptor { int in_use; struct vnode *vn; uint32_t position; int writable; }`. Consumed by Task 7's fd renumbering and Task 10's `getdents`.
- Consumes: Task 5's `fatfs_ops` and Task 2's resolver.

This is the task that deletes the legacy `fat16_*` API and closes the fd leak in `task_exit`. fds stay 3-based here; Task 7 renumbers them.

- [x] **Step 1: Change `struct file_descriptor` in `kernel/process.h`**

Replace it with:

```c
struct file_descriptor {
    int in_use;
    struct vnode *vn;   // reference held; released by close/exit
    uint32_t position;  // per-fd, NOT shared across fork -- see docs/stdlib.md
    int writable;
};
```

Add `#include "fs/vfs.h"` near the top of `kernel/process.h`.

- [x] **Step 2: Rewrite `SYS_OPEN` in `kernel/syscall.c`**

```c
        case SYS_OPEN: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            int flags = (int)a3;

            struct task *task = current_task();
            int slot = -1;
            for (int i = 0; i < MAX_OPEN_FILES; i++) {
                if (!task->files[i].in_use) { slot = i; break; }
            }
            if (slot < 0) { return -EMFILE; }

            int err = 0;
            struct vnode *vn = vfs_resolve(path_buf, &err);
            if (!vn && (flags & O_CREAT)) {
                char name[VFS_NAME_MAX];
                struct vnode *dir = vfs_resolve_parent(path_buf, name, &err);
                if (!dir) { return err; }
                uint64_t new_id;
                int rc = dir->mount->ops->create(dir, name, &new_id);
                if (rc != 0) { vnode_put(dir); return rc; }
                vn = vnode_get(dir->mount, new_id);
                vnode_put(dir);
                if (!vn) { return -ENFILE; }
            }
            if (!vn) { return err; }
            if (vn->type == VNODE_DIR && (flags & (O_WRONLY | O_RDWR))) {
                vnode_put(vn);
                return -EISDIR;
            }
            if (flags & O_TRUNC) {
                vn->mount->ops->truncate(vn);
            }

            struct file_descriptor *f = &task->files[slot];
            f->in_use = 1;
            f->vn = vn;   // the reference vfs_resolve/vnode_get took is now the fd's
            f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
            f->position = (flags & O_APPEND) ? vn->size : 0;
            return slot + 3;
        }
```

- [x] **Step 3: Rewrite `SYS_READ`, `SYS_WRITE`, `SYS_CLOSE`, and `SYS_LSEEK`**

The file branches of read and write become driver-agnostic:

```c
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use) { return -EBADF; }
            int64_t n = f->vn->mount->ops->read(f->vn, f->position, buf, (uint32_t)len);
            if (n < 0) { return n; }
            f->position += (uint32_t)n;
            return n;
```

```c
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use || !f->writable) { return -EBADF; }
            int64_t n = f->vn->mount->ops->write(f->vn, f->position, buf, (uint32_t)len);
            if (n < 0) { return n; }
            f->position += (uint32_t)n;
            return n;
```

`SYS_CLOSE` must release the vnode reference:

```c
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use) { return -EBADF; }
            vnode_put(f->vn);
            f->vn = 0;
            f->in_use = 0;
            return 0;
```

`SYS_LSEEK`'s `SEEK_END` reads `f->vn->size` instead of `f->size`; everything else is unchanged.

- [x] **Step 4: Rewrite `SYS_MKDIR` and `SYS_UNLINK`**

```c
        case SYS_MKDIR: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            char name[VFS_NAME_MAX];
            int err = 0;
            struct vnode *dir = vfs_resolve_parent(path_buf, name, &err);
            if (!dir) { return err; }
            int rc = dir->mount->ops->mkdir(dir, name);
            vnode_put(dir);
            return rc;
        }
        case SYS_UNLINK: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            char name[VFS_NAME_MAX];
            int err = 0;
            struct vnode *dir = vfs_resolve_parent(path_buf, name, &err);
            if (!dir) { return err; }
            int rc = dir->mount->ops->unlink(dir, name);
            vnode_put(dir);
            return rc;
        }
```

Add `#include "fs/vfs.h"` to `kernel/syscall.c` and drop `#include "fs/fatfs.h"` once nothing there references it.

- [x] **Step 5: Make `fork()` take a reference on every inherited fd**

In `kernel/process.c`'s `fork_task`, the fd-copy loop currently copies by value only. Replace it with:

```c
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        child->files[i] = parent->files[i]; // copied by value -- see docs/stdlib.md
        // The copy duplicates the vnode POINTER, so the child owes the
        // cache its own reference; without this the first close on
        // either side would free a vnode the other still holds.
        if (child->files[i].in_use && child->files[i].vn) {
            child->files[i].vn->refcount++;
        }
    }
```

- [x] **Step 6: Make `task_exit()` release every open fd**

In `kernel/process.c`'s `task_exit`, inside the existing `cli` critical section and before `free_address_space`:

```c
    // Release this task's file descriptors. task_exit() has never
    // closed them -- an invisible leak until now, because nothing
    // tracked file state beyond the task. With refcounted vnodes it
    // would pin them permanently and make umount report -EBUSY
    // forever, so the VFS forces this gap closed.
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (current->files[i].in_use && current->files[i].vn) {
            vnode_put(current->files[i].vn);
            current->files[i].vn = 0;
            current->files[i].in_use = 0;
        }
    }
```

Add `#include "fs/vfs.h"` to `kernel/process.c` if not already present via `process.h`.

- [x] **Step 7: Delete the legacy FAT API**

From `kernel/fs/fatfs.h`, remove the declarations of `fat16_find`, `fat16_read_file`, `fat16_read_at`, `fat16_write_file`, `fat16_truncate`, `fat16_create_file`, `fat16_mkdir`, `fat16_delete_entry`, and `fat16_update_entry_size`. Keep `fat16_mount`, `fat16_selftest`, and `fat16_write_selftest` — `kmain` still calls them and they still exercise the driver's internals directly.

From `kernel/fs/fatfs.c`, delete each corresponding one-line legacy wrapper added in Task 5 Step 6. The `_v` implementations stay.

`spawn()` in `kernel/process.c` still calls `fat16_find` and `fat16_read_file` to load an ELF image. Rewrite `build_user_address_space`'s file-loading prologue to use the VFS:

```c
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) {
        serial_write_string("[process] FAILED: file not found: ");
        serial_write_string(path);
        serial_write_string("\n");
        return 0;
    }
    uint32_t size = vn->size;

    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) {
        serial_write_string("[process] FAILED: kmalloc failed for ELF image\n");
        vnode_put(vn);
        return 0;
    }
    vn->mount->ops->read(vn, 0, image, size);
    vnode_put(vn);
```

The rest of the function (PML4 setup, `elf_load`, user stack) is unchanged.

- [x] **Step 8: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "fileio\|\[vfs\]\|task exited" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: `fileio` reproduces its exact milestone-7 output (open, write, read back, lseek, mkdir, unlink), every program still spawns — proving ELF loading now goes through the VFS — and all milestone 5-9 behavior is unchanged. Zero `FAILED`, zero exceptions.

- [x] **Step 9: Commit**

```bash
git add kernel/syscall.c kernel/process.c kernel/process.h kernel/fs/fatfs.c kernel/fs/fatfs.h
git commit -m "Rewrite the syscall layer against the VFS and close the fd leak at exit"
```

---

### Task 7: devfs, and fds numbered directly

**Files:**
- Create: `kernel/fs/devfs.h`, `kernel/fs/devfs.c`
- Modify: `kernel/fs/vfs.c`, `kernel/syscall.c`, `kernel/process.c`, `kernel/process.h`, `kernel/kernel.c`

**Interfaces:**
- Produces: `extern const struct vfs_ops devfs_ops;`, `int vfs_open_into(const char *path, struct task *t, int fd, int writable)` for opening console fds at process creation.
- Consumes: Task 6's vnode-backed `struct file_descriptor`.

- [x] **Step 1: Add `kernel/fs/devfs.h`**

```c
#ifndef NEOOS_DEVFS_H
#define NEOOS_DEVFS_H

#include "vfs.h"

extern const struct vfs_ops devfs_ops;

#endif
```

- [x] **Step 2: Add `kernel/fs/devfs.c`**

```c
#include "devfs.h"
#include "../errno.h"
#include "../serial.h"
#include "../vga.h"

// Static device table. inode_id is the index; id 0 is the root
// directory, so real devices start at 1.
struct devfs_node {
    const char     *name;
    enum vnode_type type;
    int64_t (*read)(void *buf, uint32_t len);
    int64_t (*write)(const void *buf, uint32_t len);
};

static int64_t console_read(void *buf, uint32_t len) {
    (void)buf; (void)len;
    return 0; // no keyboard-to-process input path yet; always EOF
}

static int64_t console_write(const void *buf, uint32_t len) {
    const char *s = (const char *)buf;
    for (uint32_t i = 0; i < len; i++) {
        serial_write_char(s[i]);
        vga_put_char(s[i]);
    }
    return (int64_t)len;
}

static int64_t null_read(void *buf, uint32_t len)  { (void)buf; (void)len; return 0; }
static int64_t null_write(const void *buf, uint32_t len) { (void)buf; return (int64_t)len; }

static int64_t zero_read(void *buf, uint32_t len) {
    uint8_t *b = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) { b[i] = 0; }
    return (int64_t)len;
}

static const struct devfs_node devices[] = {
    { "/",       VNODE_DIR,    0,           0            },
    { "CONSOLE", VNODE_DEVICE, console_read, console_write },
    { "NULL",    VNODE_DEVICE, null_read,    null_write    },
    { "ZERO",    VNODE_DEVICE, zero_read,    null_write    },
};
#define DEVFS_COUNT (sizeof(devices) / sizeof(devices[0]))

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int devfs_mount_op(struct vfs_mount *m, const char *source) {
    (void)source;
    m->fs_private = 0;
    return 0;
}

static void devfs_umount_op(struct vfs_mount *m) { m->fs_private = 0; }

static int devfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    (void)m;
    if (inode_id >= DEVFS_COUNT) { return -ENOENT; }
    out->type = devices[inode_id].type;
    out->size = 0;
    out->fs_private = (void *)&devices[inode_id];
    return 0;
}

static int devfs_sync_inode(struct vnode *vn) { (void)vn; return 0; }

static int devfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    (void)dir;
    for (uint64_t i = 1; i < DEVFS_COUNT; i++) {
        if (name_eq(devices[i].name, name)) { *out_inode_id = i; return 0; }
    }
    return -ENOENT;
}

static int64_t devfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    (void)pos; // devices are not seekable; position is ignored
    const struct devfs_node *d = (const struct devfs_node *)vn->fs_private;
    if (!d->read) { return -EPERM; }
    return d->read(buf, len);
}

static int64_t devfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    (void)pos;
    const struct devfs_node *d = (const struct devfs_node *)vn->fs_private;
    if (!d->write) { return -EPERM; }
    return d->write(buf, len);
}

// devfs is read-only as a namespace: its node set is fixed at compile
// time. These return -EPERM rather than being NULL so callers never
// have to know which driver they are talking to.
static int devfs_create(struct vnode *dir, const char *name, uint64_t *out_id) {
    (void)dir; (void)name; (void)out_id; return -EPERM;
}
static int devfs_mkdir(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EPERM;
}
static int devfs_unlink(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EPERM;
}
static int devfs_truncate(struct vnode *vn) { (void)vn; return -EPERM; }

static int devfs_readdir(struct vnode *dir, uint32_t index, struct dirent *out) {
    (void)dir;
    uint64_t id = index + 1; // skip the root entry at index 0
    if (id >= DEVFS_COUNT) { return -ENOENT; }
    int i = 0;
    while (devices[id].name[i] && i < VFS_NAME_MAX - 1) { out->name[i] = devices[id].name[i]; i++; }
    out->name[i] = '\0';
    out->type = DT_CHR;
    return 0;
}

const struct vfs_ops devfs_ops = {
    .mount      = devfs_mount_op,
    .umount     = devfs_umount_op,
    .read_inode = devfs_read_inode,
    .sync_inode = devfs_sync_inode,
    .lookup     = devfs_lookup,
    .read       = devfs_read,
    .write      = devfs_write,
    .create     = devfs_create,
    .mkdir      = devfs_mkdir,
    .unlink     = devfs_unlink,
    .truncate   = devfs_truncate,
    .readdir    = devfs_readdir,
};
```

Check the exact names of the serial and VGA character-output functions before writing `console_write`:

```bash
grep -n "void .*char\|serial_write_char\|vga_put" kernel/serial.h kernel/vga.h
```

Use whatever they are actually called; if only string-writing functions exist, build a 2-byte NUL-terminated stack buffer per character rather than adding new functions to those modules.

- [x] **Step 3: Register `"devfs"` in `vfs_mount_fs`**

In `kernel/fs/vfs.c`, add `#include "devfs.h"` and the third branch:

```c
    } else if (str_eq(fstype, "devfs")) {
        m->ops = &devfs_ops;
    } else {
```

- [x] **Step 4: Renumber file descriptors**

In `kernel/process.h`, raise the table size and document why:

```c
// 16 entries indexed DIRECTLY by fd. Before the VFS, fds 0-2 were
// special-cased integers in syscall_dispatch and this array started at
// fd 3; now /dev/console is a real vnode opened on 0, 1, and 2 at
// process creation, so the fd IS the index.
#define MAX_OPEN_FILES 16
```

In `kernel/syscall.c`, delete every `fd < 3` special case in `SYS_READ` and `SYS_WRITE` (the console branches — devfs handles them now), and replace every `&current_task()->files[fd - 3]` with:

```c
            if (fd < 0 || fd >= MAX_OPEN_FILES) { return -EBADF; }
            struct file_descriptor *f = &current_task()->files[fd];
```

In `SYS_OPEN`, the free-slot search now starts at 3 (0-2 belong to the standard streams) and returns `slot` rather than `slot + 3`:

```c
            int slot = -1;
            for (int i = 3; i < MAX_OPEN_FILES; i++) {
                if (!task->files[i].in_use) { slot = i; break; }
            }
            if (slot < 0) { return -EMFILE; }
```

```c
            return slot;
```

Confirm nothing was missed:

```bash
grep -n "fd - 3\|fd < 3\|slot + 3" kernel/syscall.c   # must print nothing
```

- [x] **Step 5: Open `/dev/console` on fds 0-2 at process creation**

Add to `kernel/fs/vfs.c` and declare in `vfs.h`:

```c
// Opens `path` into task t's fd slot `fd`, taking the vnode reference
// the slot will own. Used to give every new process its standard
// streams; ordinary opens go through SYS_OPEN instead.
int vfs_open_into(const char *path, struct task *t, int fd, int writable) {
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) { return err; }
    t->files[fd].in_use = 1;
    t->files[fd].vn = vn;
    t->files[fd].position = 0;
    t->files[fd].writable = writable;
    return 0;
}
```

`vfs.h` cannot include `process.h` (which already includes `vfs.h`), so forward-declare `struct task;` in `vfs.h` above the prototype.

In `kernel/process.c`'s `spawn`, replace the existing fd-clearing loop:

```c
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        t->files[i].in_use = 0;
        t->files[i].vn = 0;
    }
    // Standard streams as real /dev/console vnodes. stdin is opened
    // read-only and always returns EOF; stdout and stderr both write
    // to the console.
    vfs_open_into("/dev/CONSOLE", t, 0, 0);
    vfs_open_into("/dev/CONSOLE", t, 1, 1);
    vfs_open_into("/dev/CONSOLE", t, 2, 1);
```

Apply the same `t->files[i].vn = 0;` addition to `task_create_kernel_thread`'s fd loop, but do **not** open console fds there — kernel threads never issue syscalls.

- [x] **Step 6: Mount `/dev` at boot**

In `kernel/kernel.c`:

```c
    vfs_init();
    vfs_mount_fs("hd0", "/",    "fat");
    vfs_mount_fs(0,     "/dev", "devfs");
    vfs_mount_fs(0,     "/tmp", "ramfs");
    vfs_selftest();
```

`/dev` must be mounted before the first `spawn()`, which is already true — `kmain` mounts everything before creating any task.

- [x] **Step 7: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "\[vfs\] mounted\|parent\|looper\|yielder\|fork_test" /tmp/neoos.log | head -20
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: three mount lines including `devfs at /dev`. Every existing program's `printf` output still appears — it now travels through `/dev/CONSOLE` rather than the deleted `fd < 3` branch, which is the whole proof of this task. `fileio`'s fd numbers shift from 3 to 3 (unchanged, since the search starts at 3), so its output is byte-identical. Zero `FAILED`, zero exceptions.

- [x] **Step 8: Commit**

```bash
git add kernel/fs/devfs.h kernel/fs/devfs.c kernel/fs/vfs.c kernel/fs/vfs.h kernel/syscall.c kernel/process.c kernel/process.h kernel/kernel.c
git commit -m "Add devfs and route standard streams through /dev/CONSOLE"
```

---

### Task 8: FAT32 support and the second volume at `/mnt`

**Files:**
- Modify: `kernel/fs/fatfs.c`, `kernel/fs/fatfs.h`, `kernel/kernel.c`

**Interfaces:**
- Produces: variant detection at mount; `fat_volume.variant`. No signature changes — every existing helper keeps its shape and branches internally.
- Consumes: Task 5's driver, Task 1's drive 1.

- [x] **Step 1: Extend the BPB struct with the FAT32 fields**

In `kernel/fs/fatfs.c`, `struct fat16_bpb` currently stops at `total_sectors_32` (offset 32). Append the FAT32 extended fields:

```c
    // FAT32 extended BPB, offsets 36-51. On a FAT16 volume these bytes
    // hold the drive number / boot signature / volume label instead and
    // are simply never read, because the variant is decided before
    // anything here is touched.
    uint32_t sectors_per_fat_32;   // offset 36
    uint16_t ext_flags;            // offset 40
    uint16_t fs_version;           // offset 42
    uint32_t root_cluster;         // offset 44
    uint16_t fs_info_sector;       // offset 48
    uint16_t backup_boot_sector;   // offset 50
```

Keep `__attribute__((packed))`.

- [x] **Step 2: Add the variant field and detect it in `fat_read_bpb`**

Add to `struct fat_volume`:

```c
    enum { FAT_16, FAT_32 } variant;
```

Rewrite `fat_read_bpb`:

```c
static int fat_read_bpb(struct fat_volume *v) {
    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(v->drive, 0, 1, sector)) {
        return 0;
    }
    struct fat16_bpb *bpb = (struct fat16_bpb *)sector;

    v->bytes_per_sector = bpb->bytes_per_sector;
    v->sectors_per_cluster = bpb->sectors_per_cluster;
    v->root_entry_count = bpb->root_entry_count;
    v->fat_start_lba = bpb->reserved_sector_count;

    // FAT32 zeroes the 16-bit sectors_per_fat and uses the 32-bit
    // field at offset 36 instead.
    v->sectors_per_fat = bpb->sectors_per_fat ? bpb->sectors_per_fat
                                              : bpb->sectors_per_fat_32;

    v->root_dir_sector_count = ((uint32_t)v->root_entry_count * sizeof(struct fat16_dirent)
                                 + v->bytes_per_sector - 1) / v->bytes_per_sector;
    v->root_dir_start_lba = v->fat_start_lba + (uint32_t)bpb->num_fats * v->sectors_per_fat;
    v->data_start_lba = v->root_dir_start_lba + v->root_dir_sector_count;

    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16
                                                   : bpb->total_sectors_32;
    uint32_t data_sectors = total_sectors - (v->data_start_lba - 0);
    uint32_t cluster_count = v->sectors_per_cluster ? data_sectors / v->sectors_per_cluster : 0;

    // The official determination rule. FAT12 is detected only so it can
    // be rejected rather than silently misread as FAT16.
    if (cluster_count < 4085) {
        return 0;                       // FAT12 -- caller returns -ENODEV
    } else if (cluster_count < 65525) {
        v->variant = FAT_16;
        v->root_cluster = 0;
    } else {
        v->variant = FAT_32;
        v->root_cluster = bpb->root_cluster;
        // FAT32 has no fixed root region: data begins right after the
        // FATs, and the root is an ordinary cluster chain.
        v->root_dir_sector_count = 0;
        v->data_start_lba = v->root_dir_start_lba;
    }

    serial_write_string("[fatfs] mounted drive=");
    serial_write_hex64(v->drive);
    serial_write_string(" variant=");
    serial_write_string(v->variant == FAT_32 ? "FAT32" : "FAT16");
    serial_write_string(" clusters=");
    serial_write_hex64(cluster_count);
    serial_write_string("\n");
    return 1;
}
```

- [x] **Step 3: Make FAT entry access variant-aware**

`fat16_next_cluster` and `fat16_set_next_cluster` currently assume 16-bit entries. The cluster number type also has to widen — change every `uint16_t cluster` in the driver's internals and in `struct fatfs_inode.first_cluster` to `uint32_t`, since FAT32 cluster numbers exceed 16 bits.

```c
static uint32_t fat16_next_cluster(struct fat_volume *v, uint32_t cluster) {
    uint32_t entry_size = (v->variant == FAT_32) ? 4 : 2;
    uint32_t fat_offset = cluster * entry_size;
    uint32_t sector_lba = v->fat_start_lba + fat_offset / v->bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % v->bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(v->drive, sector_lba, 1, sector)) {
        return 0x0FFFFFFF;
    }
    if (v->variant == FAT_32) {
        // Only the low 28 bits are the cluster number; the top 4 are
        // reserved and must be ignored on read.
        return (*(uint32_t *)(sector + offset_in_sector)) & 0x0FFFFFFF;
    }
    return *(uint16_t *)(sector + offset_in_sector);
}

static void fat16_set_next_cluster(struct fat_volume *v, uint32_t cluster, uint32_t value) {
    uint32_t entry_size = (v->variant == FAT_32) ? 4 : 2;
    uint32_t fat_offset = cluster * entry_size;
    uint32_t sector_lba = v->fat_start_lba + fat_offset / v->bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % v->bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    if (!ata_read_sectors(v->drive, sector_lba, 1, sector)) {
        return;
    }
    if (v->variant == FAT_32) {
        uint32_t *slot = (uint32_t *)(sector + offset_in_sector);
        // Preserve the top 4 reserved bits rather than clobbering them.
        *slot = (*slot & 0xF0000000) | (value & 0x0FFFFFFF);
    } else {
        *(uint16_t *)(sector + offset_in_sector) = (uint16_t)value;
    }
    ata_write_sectors(v->drive, sector_lba, 1, sector);
}
```

Add a shared end-of-chain test and use it everywhere a bare `>= 0xFFF8` comparison currently appears:

```c
// End-of-chain markers differ by width. Every chain walk in this file
// must use this rather than comparing against a hardcoded FAT16 value.
static int fat_is_eoc(struct fat_volume *v, uint32_t cluster) {
    return (v->variant == FAT_32) ? (cluster >= 0x0FFFFFF8) : (cluster >= 0xFFF8);
}
```

Find every site to convert:

```bash
grep -n "0xFFF8\|0xFFFF" kernel/fs/fatfs.c
```

`fat16_alloc_cluster`'s scan limit also becomes variant-aware: its upper bound is `v->sectors_per_fat * v->bytes_per_sector / entry_size` rather than a hardcoded FAT16 cluster ceiling.

One site is easy to miss because it was written in Task 5, not inherited from the original driver: `fat_dir_nth`'s chain walk ends with

```c
        cluster = fat16_next_cluster(v, cluster);
        if (cluster >= 0xFFF8 || cluster < 2) { return -ENOENT; }
```

which must become

```c
        cluster = fat16_next_cluster(v, cluster);
        if (fat_is_eoc(v, cluster) || cluster < 2) { return -ENOENT; }
```

Left as-is, FAT32 directory listings would run off the end of the chain instead of terminating, because a FAT32 chain never reaches `0xFFF8`.

- [x] **Step 4: Make the root directory variant-aware**

Under FAT32 the root is an ordinary cluster chain, so the `in_root` special case collapses into the normal path. In `fatfs_read_inode`, the root's first cluster becomes the volume's `root_cluster`:

```c
    if (inode_id == FATFS_ROOT_INODE) {
        n->first_cluster = (v->variant == FAT_32) ? v->root_cluster : 0;
        /* ...rest unchanged... */
    }
```

Everywhere a call currently passes `dir->inode_id == FATFS_ROOT_INODE` as an `in_root` flag — `fatfs_lookup`, `fatfs_create`, `fatfs_mkdir_op`, `fatfs_unlink_op`, `fatfs_readdir_op` — that flag becomes:

```c
    int in_root = (dir->inode_id == FATFS_ROOT_INODE) && (v->variant == FAT_16);
```

so FAT32's root takes the cluster-chain path with `first_cluster = root_cluster`. `find_in_root`, `create_entry_in_directory`, and `fat_dir_nth` need no internal change — they already branch on the flag they are handed.

- [x] **Step 5: Mount the FAT32 volume at `/mnt`**

In `kernel/kernel.c`:

```c
    vfs_init();
    vfs_mount_fs("hd0", "/",    "fat");
    vfs_mount_fs(0,     "/dev", "devfs");
    vfs_mount_fs(0,     "/tmp", "ramfs");
    vfs_mount_fs("hd1", "/mnt", "fat");
    vfs_selftest();
```

Add to `vfs_selftest`, before the final pass line:

```c
    // Read a file that exists only on the FAT32 volume. Reaching it
    // proves variant detection, 32-bit FAT entries, and the
    // cluster-chained root all work.
    struct vnode *f32 = vfs_resolve("/mnt/FAT32.TXT", &err);
    if (!f32) {
        serial_write_string("[vfs] selftest FAILED: resolve /mnt/FAT32.TXT\n");
        return;
    }
    char f32buf[8] = {0};
    if (f32->mount->ops->read(f32, 0, f32buf, 5) != 5 || f32buf[0] != 'H') {
        serial_write_string("[vfs] selftest FAILED: FAT32 read\n");
        vnode_put(f32);
        return;
    }
    vnode_put(f32);

    // And one in a FAT32 subdirectory.
    struct vnode *f32n = vfs_resolve("/mnt/SUB/F32NEST.TXT", &err);
    if (!f32n) {
        serial_write_string("[vfs] selftest FAILED: resolve /mnt/SUB/F32NEST.TXT\n");
        return;
    }
    vnode_put(f32n);
```

- [x] **Step 6: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "\[fatfs\]\|\[vfs\]" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: two `[fatfs] mounted` lines — `drive=0x0 variant=FAT16` and `drive=0x1 variant=FAT32` — four `[vfs] mounted` lines, and `[vfs] selftest passed`. Zero `FAILED`, zero exceptions. FAT16 behavior on drive 0 must be completely unchanged.

- [x] **Step 7: Commit**

```bash
git add kernel/fs/fatfs.c kernel/fs/fatfs.h kernel/kernel.c
git commit -m "Add FAT32 support with variant auto-detection and mount it at /mnt"
```

---

### Task 9: `mount` and `umount` syscalls, wrappers, and docs

**Files:**
- Modify: `kernel/syscall.c`, `lib/syscall.c`, `lib/include/unistd.h`, `docs/stdlib.md`

**Interfaces:**
- Produces: `SYS_MOUNT` (14), `SYS_UMOUNT` (15); `int mount(const char *source, const char *target, const char *fstype)`, `int umount(const char *target)`; `copy_user_string`.
- Consumes: Task 2's `vfs_mount_fs`/`vfs_umount`.

- [x] **Step 1: Add `copy_user_string` to `kernel/syscall.c`**

`mount` takes three strings but `syscall_dispatch` has only four argument slots, and the existing convention spends two slots per string as a `(pointer, length)` pair. Three pairs need six. So `mount` alone passes NUL-terminated pointers:

```c
// Bounded copy of a NUL-terminated user string. Used only by
// SYS_MOUNT: every other path-taking syscall passes an explicit
// (pointer, length) pair via copy_user_path, but mount needs three
// strings and syscall_dispatch has only four argument slots
// (a1-a4, from rdi/rsi/rdx/r10 in syscall_entry.asm). Widening the
// syscall ABI to six arguments for one call was rejected.
static void copy_user_string(int64_t user_ptr, char *out, uint64_t out_size) {
    const char *s = (const char *)(uintptr_t)user_ptr;
    uint64_t i = 0;
    while (i < out_size - 1 && s[i]) { out[i] = s[i]; i++; }
    out[i] = '\0';
}
```

- [x] **Step 2: Add the two dispatch cases**

```c
#define SYS_MOUNT  14
#define SYS_UMOUNT 15
```

```c
        case SYS_MOUNT: {
            char source[16], target[VFS_MAX_PATH], fstype[16];
            copy_user_string(a1, source, sizeof(source));
            copy_user_string(a2, target, sizeof(target));
            copy_user_string(a3, fstype, sizeof(fstype));
            return vfs_mount_fs(source, target, fstype);
        }
        case SYS_UMOUNT: {
            char target[VFS_MAX_PATH];
            copy_user_path(a1, a2, target, sizeof(target));
            return vfs_umount(target);
        }
```

`umount` takes one string and fits the existing `(pointer, length)` convention, so it uses `copy_user_path` unchanged.

- [x] **Step 3: Add the library wrappers**

In `lib/syscall.c`:

```c
#define SYS_MOUNT  14
#define SYS_UMOUNT 15
```

```c
int mount(const char *source, const char *target, const char *fstype) {
    // Three NUL-terminated pointers, no lengths -- see the note on
    // copy_user_string in kernel/syscall.c for why mount differs from
    // every other path-taking call here.
    return (int)syscall3(SYS_MOUNT, (int64_t)(uint64_t)source,
                          (int64_t)(uint64_t)target, (int64_t)(uint64_t)fstype);
}

int umount(const char *target) {
    uint64_t len = strlen(target);
    return (int)syscall2(SYS_UMOUNT, (int64_t)(uint64_t)target, (int64_t)len);
}
```

- [x] **Step 4: Declare them in `lib/include/unistd.h`**

```c
// Mounts the filesystem `fstype` ("fat", "ramfs", or "devfs") at
// `target`. `source` is "hd0" or "hd1" for "fat" and ignored
// otherwise; FAT16 versus FAT32 is auto-detected. Returns 0, or
// -ENODEV (unknown type or unreadable volume), -EEXIST (already
// mounted there), or -ENOSPC (mount table full).
int mount(const char *source, const char *target, const char *fstype);

// Unmounts the filesystem at `target`. Returns 0, -ENOENT if nothing
// is mounted there, or -EBUSY if any file on it is still open.
int umount(const char *target);
```

- [x] **Step 5: Add the `docs/stdlib.md` entries**

In the `<unistd.h>` section, after the `exec` entry:

```markdown
- `int mount(const char *source, const char *target, const char *fstype)`
  — mounts a filesystem at `target`. `fstype` is `"fat"`, `"ramfs"`, or
  `"devfs"`; `source` is `"hd0"` or `"hd1"` for `"fat"` and ignored
  otherwise. FAT16 versus FAT32 is auto-detected from the volume's
  cluster count. Returns 0, or `-ENODEV`, `-EEXIST`, or `-ENOSPC`.
- `int umount(const char *target)` — unmounts the filesystem at
  `target`. Returns 0, `-ENOENT` if nothing is mounted there, or
  `-EBUSY` if any file on it is still open. The mount is left
  completely intact on `-EBUSY`.
```

- [x] **Step 6: Verify with a temporary userland check**

Create `userland/mounttest.c`:

```c
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("/mnt/FAT32.TXT", O_RDONLY);
    if (fd < 0) { printf("[mounttest] FAILED: open returned %d\n", fd); return 1; }

    int busy = umount("/mnt");
    printf("[mounttest] umount while open returned %d (want -16)\n", busy);

    close(fd);
    int ok = umount("/mnt");
    printf("[mounttest] umount after close returned %d (want 0)\n", ok);

    int gone = open("/mnt/FAT32.TXT", O_RDONLY);
    printf("[mounttest] open after umount returned %d (want negative)\n", gone);

    int re = mount("hd1", "/mnt", "fat");
    printf("[mounttest] remount returned %d (want 0)\n", re);
    return 0;
}
```

Add its Makefile rule alongside the others (note the 8.3 limit — the ELF name must be at most 8 characters before the extension, so `MOUNTTST.ELF`):

```makefile
$(USERLAND_BUILD)/MOUNTTST.ELF: $(USERLAND_DIR)/mounttest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/mounttest.c -L$(LIB_BUILD) -lneoos
```

Add `$(USERLAND_BUILD)/MOUNTTST.ELF` to the `$(DISK_IMG)` prerequisites and an `mcopy` line for it. Temporarily replace `kmain`'s four `spawn(...)` calls with `spawn("/BIN/MOUNTTST.ELF");`.

- [x] **Step 7: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "mounttest" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected, in order: `umount while open returned -16` (`-EBUSY`), `umount after close returned 0`, `open after umount returned` a negative value, `remount returned 0`. Zero `FAILED`, zero exceptions.

- [x] **Step 8: Restore the four-process boot and re-verify**

Restore `kmain`'s original `spawn("/BIN/PARENT.ELF")` block plus the two `LOOPER` and one `YIELDER` calls. Confirm `git diff --stat kernel/kernel.c` shows only the mount lines added in earlier tasks. Rebuild, boot, and confirm zero `FAILED` and zero exceptions.

Keep `mounttest.c` and its Makefile rule — Task 12 reuses it.

- [x] **Step 9: Commit**

```bash
git add kernel/syscall.c lib/syscall.c lib/include/unistd.h docs/stdlib.md userland/mounttest.c Makefile kernel/kernel.c
git commit -m "Add mount and umount syscalls with library wrappers"
```

---

### Task 10: `getdents` syscall and the userland `dirent.h` API

**Files:**
- Create: `lib/include/dirent.h`, `lib/dirent.c`
- Modify: `kernel/syscall.c`, `lib/syscall.c`, `docs/stdlib.md`, `Makefile`

**Interfaces:**
- Produces: `SYS_GETDENTS` (16); `int getdents(int fd, struct dirent *buf, int count)`; `DIR`, `opendir`, `readdir`, `closedir`.
- Consumes: the `readdir` op every driver implements (Tasks 3, 5, 7, 8) and its contract — 0 with `*out` filled for a valid index, `-ENOENT` once past the last entry.

- [x] **Step 1: Add the `SYS_GETDENTS` dispatch case**

In `kernel/syscall.c`:

```c
#define SYS_GETDENTS 16
```

```c
        case SYS_GETDENTS: {
            int fd = (int)a1;
            struct dirent *out = (struct dirent *)(uintptr_t)a2;
            int count = (int)a3;
            if (fd < 0 || fd >= MAX_OPEN_FILES || count <= 0) { return -EBADF; }

            struct file_descriptor *f = &current_task()->files[fd];
            if (!f->in_use) { return -EBADF; }
            if (f->vn->type != VNODE_DIR) { return -ENOTDIR; }

            // position doubles as the directory cursor for a dir fd,
            // so repeated calls walk forward exactly like read() does
            // on a file.
            int written = 0;
            while (written < count) {
                if (f->vn->mount->ops->readdir(f->vn, f->position, &out[written]) != 0) {
                    break; // past the last entry
                }
                f->position++;
                written++;
            }
            return written;
        }
```

`SYS_OPEN` must stop rejecting directories opened read-only — check that its `O_WRONLY|O_RDWR` guard (Task 6, Step 2) already permits `O_RDONLY` on a `VNODE_DIR`. It does; no change needed.

- [x] **Step 2: Add the raw wrapper in `lib/syscall.c`**

```c
#define SYS_GETDENTS 16
```

```c
int getdents(int fd, struct dirent *buf, int count) {
    return (int)syscall3(SYS_GETDENTS, fd, (int64_t)(uint64_t)buf, count);
}
```

Add `#include "dirent.h"` at the top of `lib/syscall.c`.

- [x] **Step 3: Add `lib/include/dirent.h`**

```c
#ifndef NEOOS_DIRENT_H
#define NEOOS_DIRENT_H

#include <stdint.h>

#define DIRENT_NAME_MAX 13   // 8.3 name, dot, NUL

// MUST stay in lockstep with struct dirent in kernel/fs/vfs.h -- the
// kernel and library trees do not share headers, so this is a
// deliberate duplicate, exactly like the syscall numbers in
// kernel/syscall.c and lib/syscall.c. Fixed-size fields only, so
// there is no padding ambiguity between the two builds.
#define DT_REG 1
#define DT_DIR 2
#define DT_CHR 3

struct dirent {
    char    name[DIRENT_NAME_MAX];
    uint8_t type;
};

#define DIR_BUF_ENTRIES 8

typedef struct {
    int  fd;
    int  count;   // entries currently in buf
    int  index;   // next entry to hand out
    struct dirent buf[DIR_BUF_ENTRIES];
} DIR;

// Raw syscall: fills up to `count` entries, returns how many were
// written (0 at end of directory) or a negative errno.
int getdents(int fd, struct dirent *buf, int count);

// Opens `path` as a directory. Returns 0 on failure.
DIR *opendir(const char *path);

// Returns the next entry, or 0 at end of directory. The returned
// pointer is into the DIR's own buffer and is invalidated by the next
// readdir or closedir call on that DIR.
struct dirent *readdir(DIR *d);

int closedir(DIR *d);

#endif
```

- [x] **Step 4: Add `lib/dirent.c`**

```c
#include "dirent.h"
#include "unistd.h"
#include "fcntl.h"

// A tiny static pool rather than a heap: the library has no allocator,
// and nothing in NeoOS needs more than a couple of concurrent
// directory walks.
#define MAX_OPEN_DIRS 4
static DIR dirs[MAX_OPEN_DIRS];
static int dir_used[MAX_OPEN_DIRS];

DIR *opendir(const char *path) {
    int slot = -1;
    for (int i = 0; i < MAX_OPEN_DIRS; i++) {
        if (!dir_used[i]) { slot = i; break; }
    }
    if (slot < 0) { return 0; }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { return 0; }

    dir_used[slot] = 1;
    dirs[slot].fd = fd;
    dirs[slot].count = 0;
    dirs[slot].index = 0;
    return &dirs[slot];
}

struct dirent *readdir(DIR *d) {
    if (!d) { return 0; }
    if (d->index >= d->count) {
        int n = getdents(d->fd, d->buf, DIR_BUF_ENTRIES);
        if (n <= 0) { return 0; }
        d->count = n;
        d->index = 0;
    }
    return &d->buf[d->index++];
}

int closedir(DIR *d) {
    if (!d) { return -1; }
    int rc = close(d->fd);
    for (int i = 0; i < MAX_OPEN_DIRS; i++) {
        if (&dirs[i] == d) { dir_used[i] = 0; }
    }
    return rc;
}
```

`LIB_SOURCES` already globs `$(LIB_DIR)/*.c`, so `lib/dirent.c` is picked up with no Makefile change. Confirm:

```bash
grep -n "LIB_SOURCES" Makefile
```

- [x] **Step 5: Add the `docs/stdlib.md` entries**

Add a new `<dirent.h>` section:

```markdown
## `<dirent.h>`

- `DIR *opendir(const char *path)` — opens a directory for reading.
  Returns `0` on failure (path missing, not a directory, or more than
  four directories already open).
- `struct dirent *readdir(DIR *d)` — returns the next entry, or `0` at
  end of directory. `d->name` is an 8.3 name and `d->type` is
  `DT_REG`, `DT_DIR`, or `DT_CHR`. The returned pointer is into the
  `DIR`'s own buffer and is invalidated by the next `readdir` or
  `closedir` on that `DIR`.
- `int closedir(DIR *d)` — closes the directory. Returns 0, or a
  negative `<errno.h>` code.
- `int getdents(int fd, struct dirent *buf, int count)` — the raw
  syscall the three functions above are built on. Fills up to `count`
  entries from a directory fd, returning how many were written, `0` at
  end of directory, or `-EBADF`/`-ENOTDIR`.
```

- [x] **Step 6: Build and verify with a temporary listing check**

Temporarily change `userland/mounttest.c`'s `main` to list the root directory instead:

```c
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    DIR *d = opendir("/");
    if (!d) { printf("[dirtest] FAILED: opendir /\n"); return 1; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != 0) {
        printf("[dirtest] / entry: %s type=%d\n", e->name, e->type);
        n++;
    }
    closedir(d);
    printf("[dirtest] listed %d entries\n", n);
    return 0;
}
```

Temporarily spawn `MOUNTTST.ELF` alone from `kmain`, then:

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "dirtest" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: entries for `HELLO.TXT`, `BIGFILE.TXT`, `DIR`, and `BIN` with `DIR` and `BIN` reported as `type=2` (`DT_DIR`), and a nonzero count. Zero `FAILED`, zero exceptions.

- [x] **Step 7: Restore `mounttest.c` and the four-process boot**

Restore `mounttest.c` to its Task 9 Step 6 content and `kmain` to the four-process boot. Confirm `git diff --stat userland/mounttest.c` shows no change from the committed version. Rebuild and boot; zero `FAILED`, zero exceptions.

- [x] **Step 8: Commit**

```bash
git add kernel/syscall.c lib/syscall.c lib/dirent.c lib/include/dirent.h docs/stdlib.md
git commit -m "Add getdents syscall and the opendir/readdir/closedir library API"
```

---

### Task 11: The `vfstest` cross-filesystem program

**Files:**
- Create: `userland/vfstest.c`
- Modify: `Makefile`, `kernel/kernel.c`

**Interfaces:**
- Consumes: everything built so far. Produces no new kernel interface.

This is the milestone's headline proof: one program, four filesystems, three of them writable, all through one API.

- [x] **Step 1: Write `userland/vfstest.c`**

```c
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>

// Writes a known string to `path`, reads it back, and reports whether
// it survived. Returns 1 on success.
static int roundtrip(const char *path, const char *label) {
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) {
        printf("[vfstest] %s FAILED: open returned %d\n", label, fd);
        return 0;
    }

    const char *msg = "vfs-roundtrip";
    int64_t w = write(fd, msg, 13);
    if (w != 13) {
        printf("[vfstest] %s FAILED: write returned %d\n", label, (int)w);
        close(fd);
        return 0;
    }

    if (lseek(fd, 0, SEEK_SET) != 0) {
        printf("[vfstest] %s FAILED: lseek\n", label);
        close(fd);
        return 0;
    }

    char buf[16];
    for (int i = 0; i < 16; i++) { buf[i] = 0; }
    int64_t r = read(fd, buf, 13);
    close(fd);

    if (r != 13) {
        printf("[vfstest] %s FAILED: read returned %d\n", label, (int)r);
        return 0;
    }
    for (int i = 0; i < 13; i++) {
        if (buf[i] != msg[i]) {
            printf("[vfstest] %s FAILED: content mismatch at %d\n", label, i);
            return 0;
        }
    }
    printf("[vfstest] %s roundtrip passed\n", label);
    return 1;
}

static void list(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        printf("[vfstest] listing %s FAILED: opendir\n", path);
        return;
    }
    printf("[vfstest] listing %s:\n", path);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        printf("[vfstest]   %s type=%d\n", e->name, e->type);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int ok = 1;
    ok &= roundtrip("/RT.TXT",     "fat16 (/)");
    ok &= roundtrip("/tmp/RT.TXT", "ramfs (/tmp)");
    ok &= roundtrip("/mnt/RT.TXT", "fat32 (/mnt)");

    // Two fds on one path must share a vnode: the write through `a`
    // has to be visible through `b` with no reopen in between.
    int a = open("/tmp/ALIAS.TXT", O_CREAT | O_RDWR | O_TRUNC);
    int b = open("/tmp/ALIAS.TXT", O_RDONLY);
    if (a < 0 || b < 0) {
        printf("[vfstest] alias FAILED: open a=%d b=%d\n", a, b);
        ok = 0;
    } else {
        write(a, "shared", 6);
        char buf[8];
        for (int i = 0; i < 8; i++) { buf[i] = 0; }
        int64_t r = read(b, buf, 6);
        if (r != 6 || buf[0] != 's' || buf[5] != 'd') {
            printf("[vfstest] alias FAILED: read %d bytes, buf=%s\n", (int)r, buf);
            ok = 0;
        } else {
            printf("[vfstest] vnode aliasing passed\n");
        }
        close(a);
        close(b);
    }

    list("/");
    list("/dev");
    list("/tmp");
    list("/mnt");

    printf("[vfstest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
```

- [x] **Step 2: Add the Makefile rule and disk entry**

```makefile
$(USERLAND_BUILD)/VFSTEST.ELF: $(USERLAND_DIR)/vfstest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/vfstest.c -L$(LIB_BUILD) -lneoos
```

Add `$(USERLAND_BUILD)/VFSTEST.ELF` to the `$(DISK_IMG)` prerequisites and, with the other `mcopy` lines:

```makefile
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/VFSTEST.ELF ::BIN/VFSTEST.ELF
```

- [x] **Step 3: Spawn it alongside the standard boot**

In `kernel/kernel.c`, add after the existing `spawn("/BIN/YIELDER.ELF");`:

```c
    spawn("/BIN/VFSTEST.ELF");
```

Unlike the temporary spawns in earlier tasks, this one **stays** — it is a permanent part of the boot, like `PARENT`/`LOOPER`/`YIELDER`.

- [x] **Step 4: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "vfstest" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: three `roundtrip passed` lines (one per writable filesystem), `vnode aliasing passed`, four listings — `/` showing the FAT16 disk's files, `/dev` showing `CONSOLE`/`NULL`/`ZERO` as `type=3`, `/tmp` showing the ramfs files just written, `/mnt` showing `FAT32.TXT` and `SUB` — and `[vfstest] ALL PASSED`. Zero `FAILED`, zero exceptions.

- [x] **Step 5: Commit**

```bash
git add userland/vfstest.c Makefile kernel/kernel.c
git commit -m "Add vfstest exercising all four mounted filesystems"
```

---

### Task 12: Leak gate and final regression

**Files:**
- Modify: `kernel/fs/vfs.c`, `kernel/fs/vfs.h`, `kernel/kernel.c` (temporary, reverted)

**Interfaces:**
- Produces: nothing new. `vfs_vnode_in_use_count()` already exists from Task 2, where `vfs_selftest` uses it to check that a failed path walk leaks no references.
- Consumes: everything.

- [x] **Step 1: Add the temporary leak-gate thread**

As in milestones 3 and 9, `wait_for_pid` needs a valid current task, so this runs as a kernel thread rather than inline in `kmain`. In `kernel/kernel.c`, above `kmain`:

```c
static void vfs_leak_test_thread(void) {
    serial_write_string("[test] before: free_frames=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string(" vnodes=");
    serial_write_hex64(vfs_vnode_in_use_count());
    serial_write_string("\n");

    for (int i = 0; i < 5; i++) {
        struct task *t = spawn("/BIN/VFSTEST.ELF");
        if (!t) {
            serial_write_string("[test] spawn FAILED\n");
        } else {
            wait_for_pid(t->pid);
        }
    }

    serial_write_string("[test] after: free_frames=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string(" vnodes=");
    serial_write_hex64(vfs_vnode_in_use_count());
    serial_write_string("\n");

    task_exit(0);
}
```

Temporarily replace `kmain`'s five `spawn(...)` calls with:

```c
    task_create_kernel_thread(vfs_leak_test_thread);
```

- [x] **Step 2: Build and verify no leak**

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -n "\[test\]" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: the `before:` and `after:` lines report **identical** `free_frames` and **identical** `vnodes` values, and `vnodes` equals the mount count (4). A rising vnode count means a path in `vfstest` takes a reference it never releases; a falling frame count means ramfs pages or FAT clusters leak. Zero `FAILED`, zero exceptions.

Note that `vfstest` creates `RT.TXT` on `/` and `/mnt`, so iterations after the first reopen an existing file rather than creating one — that is intentional, since `O_TRUNC` exercises the truncate path too.

- [x] **Step 3: Revert the temporary thread and run the final regression**

Delete `vfs_leak_test_thread` and restore `kmain`'s five `spawn(...)` calls (`PARENT`, two `LOOPER`, `YIELDER`, `VFSTEST`). Confirm `git diff --stat kernel/kernel.c` shows no change against the Task 11 commit.

```bash
rm -f build/disk.img build/disk2.img
make clean && make disk-image && make iso
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -serial file:/tmp/neoos.log -no-reboot
grep -c "FAILED\|exception" /tmp/neoos.log
grep -n "selftest\|child exit code\|vfstest\|task exited" /tmp/neoos.log
```

Expected, checked against the spec's success criteria one by one:

- Four `[vfs] mounted` lines, and `[fatfs] mounted` reporting `variant=FAT16` for drive 0 and `variant=FAT32` for drive 1.
- `[vfs] selftest passed`, plus every prior milestone's selftest (`pmm`, `paging`, `heap`, `fat16`, `fat16 write`).
- `[vfstest] ALL PASSED` with its three roundtrips, aliasing check, and four listings.
- `[parent] child exit code=42`, the bursty looper interleave, and the dense yielder interleave — milestone 5-9 behavior unchanged.
- Zero `FAILED`, zero exceptions.

- [x] **Step 4: Commit (only if the gate caught something)**

Nothing to commit if the leak gate passed and `kernel.c` is the only file this task touched and is now reverted — it exists purely as a verification gate, like milestone 9's Task 7. Confirm with:

```bash
git status --short   # must show no modified tracked files
```

If the gate *did* catch a leak, the fix belongs in whichever file owns it, committed here with a message describing what the gate caught.

---

## Notes for the implementer

- **`kernel/fs/fatfs.c` will be well over 1000 lines** after the FAT32 work. That is a known cost of the unified-driver decision recorded in the spec; splitting it is explicitly out of scope for this milestone. If a natural seam appears (for example, directory-entry handling separating cleanly from cluster-chain management), note it for a follow-up rather than acting on it mid-milestone.
- **Refcount bugs are this milestone's characteristic failure.** The spec calls out failed path walks as the most likely source. When something misbehaves, check `vfs_vnode_in_use_count()` first — a count that rises across an operation localises the bug immediately.
- **Every task's verification needs a fresh disk image.** `fat16_write_selftest` creates `/NEWDIR`, and a stale image makes it report `FAILED` on the second and later boots. This has already caused one false alarm in this project's history.
- **QEMU never exits on its own.** Always wrap it in `timeout`.
