# NeoOS — Milestone 10: Virtual Filesystem, Mounts, and FAT32

## Goal

Put a real virtual filesystem layer between the syscall boundary and
the on-disk format, then prove it carries four unlike filesystems at
once. Today `syscall.c` calls `fat16_*` functions by name in eight
places and `struct file_descriptor` embeds FAT16's own concepts
(`first_cluster`, `dir_entry_lba`, `dir_entry_offset`) — there is no
seam at which a second filesystem could exist. The FAT16 read-write
milestone explicitly split off "mount points / multiple mounted
volumes" as a future milestone "needs a second addressable block
device and a minimal VFS layer; brainstormed separately once this
lands". This is that milestone, expanded by the user to also cover
FAT32 and userland-visible mounting.

Order agreed with the user: COW → VFS → SMP.

## Success criteria

- Boot mounts four filesystems of three different driver types: `/`
  (FAT16 on `hd0`), `/dev` (devfs), `/tmp` (ramfs), and `/mnt` (FAT32
  on `hd1`). Each mount logs its type; the `/mnt` line reports the
  variant as auto-detected FAT32, which is itself the detection proof.
- A new `vfstest.c` writes a file, reads it back, and verifies the
  contents on each of the three writable filesystems (`/`, `/tmp`,
  `/mnt`) — one program exercising three unrelated drivers through one
  interface.
- `vfstest.c` then `opendir`s `/`, `/dev`, `/tmp`, and `/mnt` and
  prints each listing, showing four unlike filesystems behind one API.
- Two file descriptors opened on the same path share one `struct
  vnode`: a `write()` through one is visible to a `read()` through the
  other without reopening. This is only true if the vnode cache
  actually deduplicates.
- `umount("/mnt")` returns `-EBUSY` while a file on it is open; after
  closing that fd it succeeds, and a subsequent `open()` under `/mnt`
  fails with `-ENOENT`.
- `printf`/`write` to fd 1 still reaches the serial console — now via a
  real `/dev/console` vnode opened at process creation rather than the
  `fd < 3` special case in `syscall_dispatch`.
- Repeated mount/use/umount cycles leak nothing: `free_frames` returns
  to its pre-loop baseline and the vnode pool returns to zero occupied
  entries.
- All milestone 5-9 test programs (`spin`, `child`, `parent`, `looper`,
  `yielder`, `faulter`, `fileio`, `sse_test`, `fork_test`,
  `exec_target`) reproduce their exact prior behavior. This milestone
  changes where file operations are dispatched, not what they mean.
- Boot log shows zero `FAILED` lines and zero exceptions across every
  verification run.

## Out of scope (future work)

- **SMP.** The next milestone in the agreed sequence. Every structure
  added here — the vnode pool, the mount table, the refcounts — is
  unlocked and single-execution-context, joining the frame refcounts
  and the rest of the kernel on the list SMP must revisit.
- **Shared file offsets across `fork()`.** A vnode cache makes POSIX
  shared offsets *possible*, but POSIX keeps the offset in a separate
  open-file-description object, not the vnode. This milestone
  deliberately keeps `position` in `struct file_descriptor`, so the
  per-fd divergence documented in milestone 9 is unchanged. Adding a
  real open-file-description layer is future work.
- **Hard links.** The vnode cache is keyed by on-disk identity, which
  is the prerequisite, but FAT has no link count and nothing exposes
  `link()`.
- **FSInfo sector (FAT32).** The free-cluster hint is neither read nor
  maintained; cluster allocation scans the FAT exactly as the FAT16
  driver does today.
- **FAT12.** Detection classifies it (`<4085` clusters) and `mount`
  rejects it with `-ENODEV` rather than silently misreading it.
- **Long filenames.** 8.3 only, unchanged since milestone 4. VFAT
  long-name directory entries are skipped during scans, not parsed.
- **`rmdir`, `rename`.** Unchanged from the FAT16 milestone's deferral.
- **Secondary ATA channel.** Drive 1 is the primary channel's slave,
  reachable by a bit in the existing drive-select byte. A second
  controller at `0x170` would need its own IRQ wiring and is not
  needed for two volumes.
- **Block cache.** Every read still goes to ATA. A buffer cache is a
  natural follow-on but would obscure this milestone's correctness
  work.
- **Syscall argument/pointer validation.** Still not implemented,
  tracked security gap since the processes milestone. This milestone
  adds three more syscalls that trust their pointers, widening it.
- **Reclaiming a never-`wait()`-ed zombie's task slot or kernel
  stack.** Unchanged pre-existing gap. Note that this milestone *does*
  close the related fd leak, because it must (see Architecture).

## Risk note

This is the largest milestone in the project so far — roughly 11-13
tasks against milestone 9's 7, spanning a new subsystem, a 927-line
driver refactor, three new drivers, an ATA interface change, and six
new userland-visible functions. The size was raised with the user
during brainstorming and the single-milestone scope was reaffirmed
deliberately, with FAT32 added on top. It is recorded here so the
implementation plan can sequence defensively: the VFS core and the
FAT16 port must be landed and green before any new driver is added,
so that a regression is always attributable to one layer.

## Architecture

### The vnode and its cache (`kernel/fs/vfs.c`)

```c
struct vnode {
    struct vfs_mount *mount;    // which volume this lives on
    uint64_t          inode_id; // driver-defined, unique within the mount
    enum vnode_type   type;     // VNODE_FILE, VNODE_DIR, VNODE_DEVICE
    uint32_t          size;
    uint32_t          refcount; // live fds plus transient walk holds
    void             *fs_private; // driver state
    struct vnode     *next;     // hash-bucket chain
};
```

`vnode_get(mount, inode_id)` hashes to a bucket and returns the
existing vnode with `refcount++` if cached; otherwise it claims a free
pool slot and calls the driver's `read_inode` to populate it.
`vnode_put` decrements and, at zero, flushes any pending size/cluster
change via `sync_inode` and returns the slot to the free list.

Because lookup is by on-disk identity, two `open()`s of the same path
land on the same `struct vnode`. That is what makes a `write()` through
one fd immediately visible through another — and what makes `umount`'s
busy check possible.

**Fixed static pool, not `kmalloc`.** `MAX_VNODES` entries in a static
array with hash-bucket chaining, matching the existing `MAX_TASKS` and
`MAX_OPEN_FILES` style. Bounded memory, no allocation-failure path in
the middle of a path walk, and no lifetime coupling to the heap. Pool
exhaustion returns `-ENFILE` from `open()`.

**Inode identity for FAT** is the directory entry's on-disk location,
`(dir_entry_lba << 16) | dir_entry_offset`, which is unique per file
per volume. First cluster will not serve: every empty file has cluster
0. The root directory has no directory entry of its own and takes
reserved id 0, which cannot collide with a real entry: id 0 would
require `dir_entry_lba == 0`, and LBA 0 is the boot sector, never a
directory.

### The driver interface

```c
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
```

A driver that cannot perform an operation returns `-EPERM` (devfs
`create`) or `-ENOTDIR`/`-EISDIR` as appropriate; no op pointer is ever
NULL, so `syscall.c` never branches on driver identity.

### Mount table and path resolution

```c
struct vfs_mount {
    int   in_use;
    char  path[MAX_PATH];        // "/", "/dev", "/tmp", "/mnt"
    const struct vfs_ops *ops;
    void *fs_private;            // per-volume state
    struct vnode *root;
};
```

Resolution is a **longest-prefix match on the mount path, performed
once up front**. `/mnt/FILE.TXT` matches `/mnt` over `/`, so it
dispatches to the FAT32 volume with residual path `/FILE.TXT`. Within
the volume the walk proceeds component by component: `ops->lookup` on
the current directory vnode yields a child `inode_id`, `vnode_get`
takes it, and the parent is `vnode_put`.

Real Unix instead checks each directory encountered during the walk
for a mount stacked on it. Longest-prefix is simpler and produces
identical results here, including correct shadowing: a mount at `/mnt`
hides any real `/mnt` directory on the root volume, which is the
desired behavior.

**Mount points need not exist on the underlying filesystem.** The
mount table is consulted before the backing volume, so `/dev` works
without a `DEV` directory on the FAT16 disk. A NeoOS simplification.

**`umount` returns `-EBUSY`** if any vnode belonging to that mount has
a nonzero refcount, preventing a volume being torn out from under live
file descriptors.

### The unified FAT driver (`kernel/fs/fatfs.c`)

The eight current file-scope globals become one per-volume struct:

```c
struct fat_volume {
    uint8_t  drive;                       // ATA drive index
    enum { FAT_16, FAT_32 } variant;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t sectors_per_fat;
    uint32_t root_dir_start_lba;          // FAT16 only
    uint16_t root_entry_count;            // FAT16 only
    uint32_t root_cluster;                // FAT32 only
};
```

Threading this through the roughly twenty internal functions is the
single largest mechanical change in the milestone.

The variant differences are exactly four:

1. **FAT entry width.** FAT16 reads a 16-bit entry at `cluster * 2`.
   FAT32 reads a 32-bit entry at `cluster * 4`, uses only the low 28
   bits, must preserve the top 4 reserved bits on write, and tests
   end-of-chain as `>= 0x0FFFFFF8`.
2. **Root directory.** FAT16's root is a fixed region of
   `root_entry_count` entries at `root_dir_start_lba`. FAT32's root is
   an ordinary cluster chain at `root_cluster`. This is the only
   structural change: today's separate `find_in_root` and
   `find_in_directory_cluster` paths converge under FAT32, because the
   root becomes just another directory.
3. **BPB layout.** FAT32 zeroes the 16-bit `sectors_per_fat` at offset
   22 and uses the 32-bit field at offset 36, plus `root_cluster` at
   offset 44.
4. **FSInfo sector.** Out of scope, as noted above.

Everything else — the 8.3 directory entry format, directory scanning,
cluster chain walking, allocation and free — is shared unchanged.

**Variant is auto-detected at mount** from the computed cluster count,
using the standard rule: `<4085` is FAT12 (rejected, `-ENODEV`),
`<65525` is FAT16, otherwise FAT32. `mount()` is never told which type
it is getting.

### devfs (`kernel/fs/devfs.c`)

A static node table mounted at `/dev`, holding `/dev/console` (writes
to serial and VGA, reads return EOF), `/dev/null`, and `/dev/zero`.
Device vnodes carry a read/write function pair in `fs_private`.
`create`, `mkdir`, and `unlink` return `-EPERM`.

### ramfs (`kernel/fs/ramfs.c`)

Mounted at `/tmp`. A fixed pool of nodes, each file's data backed by
`pmm_alloc`'d pages, supporting create, read, write, unlink, mkdir,
and readdir — a genuine third driver rather than a stub, so the
interface is exercised by something with no block device behind it at
all.

### File descriptors and the fd numbering change

`struct file_descriptor` becomes:

```c
struct file_descriptor {
    int in_use;
    struct vnode *vn;
    uint32_t position;
    int writable;
};
```

The FAT-specific fields move into the driver's `fs_private`, which is
what decouples `syscall.c` from FAT entirely. `position` stays per-fd,
preserving milestone 9's documented `fork()` semantics.

With `/dev/console` real, `spawn()` opens it on fds 0, 1, and 2 at
process creation. `files[]` becomes indexed directly by fd,
`MAX_OPEN_FILES` rises to 16, and every `fd - 3` offset and `fd < 3`
special case in `syscall_dispatch` is deleted.

Two lifecycle consequences follow directly from refcounted vnodes:

- **`fork()` must `vnode_get` each inherited fd**, because the fd table
  is copied by value.
- **`task_exit()` must `vnode_put` every open fd.** Today it closes
  nothing — a pre-existing leak that was invisible because nothing
  tracked file state beyond the task. With a vnode cache it would pin
  vnodes permanently and make `umount` return `-EBUSY` forever. The
  VFS forces this gap closed.

### ATA multi-drive (`kernel/ata.c`)

`ata_identify`, `ata_read_sectors`, and `ata_write_sectors` gain a
leading `uint8_t drive` parameter. Drive select is already a bit in the
byte written to port `0x1F6`: bit 4 chooses master (0) or slave (1).
Drive 0 is the primary master (today's disk) and drive 1 the primary
slave, sharing a channel, IRQ, and port base — so this is a parameter
thread rather than a second controller.

## File structure

```
kernel/fs/vfs.c/.h       vnode pool and cache, mount table, path resolution
kernel/fs/fatfs.c/.h     unified FAT16+FAT32 driver (grown from fat16.c)
kernel/fs/ramfs.c/.h     in-memory filesystem
kernel/fs/devfs.c/.h     console/null/zero device nodes
kernel/ata.c/.h          gains a drive parameter
kernel/syscall.c         rewritten against vfs_*; fd 0-2 special cases removed
kernel/process.c/.h      fd table indexed by fd; fork vnode_get, exit vnode_put
lib/syscall.c            mount, umount, getdents wrappers
lib/dirent.c             opendir/readdir/closedir over getdents
lib/include/unistd.h     mount, umount
lib/include/dirent.h     DIR, struct dirent, opendir/readdir/closedir
docs/stdlib.md           entries for all six new functions
userland/vfstest.c       the cross-filesystem test program
Makefile                 second 64MB FAT32 image, second QEMU -drive
```

`kernel/fs/` mirrors the existing `kernel/mm/` subdirectory
convention. `kernel/fat16.c` moves to `kernel/fs/fatfs.c` rather than
being duplicated.

## Syscall surface

| Num | Signature | Returns |
|-----|-----------|---------|
| 14 | `mount(const char *source, const char *target, const char *fstype)` | 0, or `-ENODEV`/`-ENOENT`/`-EEXIST`/`-ENOSPC` |
| 15 | `umount(const char *target)` | 0, or `-ENOENT`/`-EBUSY` |
| 16 | `getdents(int fd, struct dirent *buf, int count)` | entries written, 0 at end, or `-EBADF`/`-ENOTDIR` |

`fstype` is `"fat"`, `"ramfs"`, or `"devfs"`. `source` is `"hd0"` or
`"hd1"` for `"fat"` and ignored otherwise. FAT16-versus-FAT32 remains
auto-detected within `"fat"`.

**`mount` breaks the existing string-passing convention, deliberately.**
Every current path-taking syscall passes a `(pointer, length)` pair
(`copy_user_path(a1, a2, ...)`), but `syscall_dispatch` accepts only
four arguments (`a1`-`a4`, mapped from `rdi/rsi/rdx/r10` in
`syscall_entry.asm`), and three such pairs would need six. `mount`
therefore passes three NUL-terminated pointers in `a1`-`a3`, read by a
new bounded `copy_user_string(user_ptr, out, out_size)` helper that
copies until NUL or `out_size - 1`, whichever comes first. `umount`
and `getdents` fit the existing convention and keep it unchanged.

Widening the syscall ABI to six arguments was considered and rejected:
it would touch `syscall_entry.asm`'s register shuffle, the
`syscall_dispatch` signature, and every existing wrapper, for the
benefit of one call.

`opendir`/`readdir`/`closedir` are userland-only, built on `open()` and
`getdents` with a buffer inside `DIR`; they add no syscalls of their
own.

`struct dirent` crosses the syscall boundary, so its layout is shared
contract between `kernel/fs/vfs.h` and `lib/include/dirent.h`. The
kernel and the standard library do not share a header tree, so the
definition is duplicated and each copy carries a comment naming the
other as the thing it must stay in lockstep with — the same
arrangement the syscall numbers already have between `kernel/syscall.c`
and `lib/syscall.c`. Fields: a fixed 8.3-sized `char name[13]`, a
`uint8_t type` (`DT_REG`/`DT_DIR`/`DT_CHR`), and nothing else, so the
struct has no padding ambiguity between the two builds.

## Data flow (kmain, extending milestone 9's sequence)

```
... existing milestones 2-9 sequence (interrupts, memory, storage, processes, stdlib, FAT16 rw, SSE, COW) ...
ata_identify(0) / ata_identify(1)     both drives probed
vfs_init()                            vnode pool and mount table cleared
vfs_mount("hd0", "/",    "fat")       FAT16 detected, root volume
vfs_mount(NULL,  "/dev", "devfs")
vfs_mount(NULL,  "/tmp", "ramfs")
vfs_mount("hd1", "/mnt", "fat")       FAT32 detected
vfs_selftest()
spawn(...)                            each new task opens /dev/console on fds 0-2
```

## Testing / verification

Headless QEMU with serial capture, as every prior milestone. The
`-drive` list gains a second 64MB image built with `mkfs.fat -F 32`
(verified during design: a 32MB FAT32 image warns that its cluster
count is below the supported minimum, so 64MB is the floor).

- **Mount log** shows all four mounts with their detected types.
- **`vfstest.c`** writes, reads back, and verifies a file on `/`,
  `/tmp`, and `/mnt`, then lists all four mount points.
- **Vnode aliasing**: two fds on one path; a write through the first is
  read back through the second without reopening.
- **Mount lifecycle**: `umount("/mnt")` with a file open returns
  `-EBUSY`; after `close()` it succeeds; a later `open()` under `/mnt`
  returns `-ENOENT`.
- **fd 0-2 via devfs**: existing programs' `printf` output still
  appears, now routed through `/dev/console`.
- **Leak gate**: repeated mount/use/umount cycles return `free_frames`
  to baseline and the vnode pool to zero occupancy, run as a kernel
  thread (as in milestones 3 and 9, since `wait_for_pid` needs a
  current task).
- **Full regression**: the standard four-process boot and all
  milestone 5-9 programs reproduce their exact prior behavior.

## Error handling

- **Pool and table exhaustion.** `open()` returns `-ENFILE` when the
  vnode pool is full, `-EMFILE` when the calling task's fd table is
  full; `mount` returns `-ENOSPC` when the mount table is full.
- **Bad mounts.** Unknown `fstype` or unreadable/FAT12 volume returns
  `-ENODEV`; a target already mounted returns `-EEXIST`.
- **Busy umount.** `-EBUSY` while any vnode on the mount is
  referenced; the mount is left entirely intact.
- **Partial failure during a path walk** releases every vnode taken so
  far before returning, so a failed `open()` leaks no references. This
  is the single most likely source of a refcount bug and is called out
  for explicit test coverage.
- **Driver errors propagate unchanged.** `-ENOSPC`, `-ENOENT`,
  `-EEXIST`, `-EISDIR`, `-ENOTDIR` keep the meanings the FAT16
  milestone gave them; the VFS adds no translation layer.
