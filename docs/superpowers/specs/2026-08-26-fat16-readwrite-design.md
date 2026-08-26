# NeoOS — Milestone 7: Read-Write FAT16

## Goal

Extend the read-only FAT16 driver from milestone 4 into a full
read-write driver, and expose file I/O to user-mode programs through
the standard library: create, write (including random-access
`lseek`), truncate, delete, and directory creation, all backed by a
real per-process file descriptor table and a unified POSIX-style
syscall surface (`open`/`read`/`write`/`close`/`lseek`/`mkdir`/
`unlink`), following the standard-library convention in `/CLAUDE.md`.

## Success criteria

- A new userland test program creates a file, writes to it, `lseek`s
  back and overwrites bytes mid-file, closes it, reopens and reads the
  result back, `mkdir`s a new directory, writes a file into it, and
  `unlink`s a file — all verified correct via QEMU serial output.
- After boot, the resulting disk image is independently inspected with
  `mdir`/`mtype` (mtools) to confirm the on-disk FAT16 structure is
  actually correct — not just that NeoOS's own driver can read back
  what it wrote.
- All six existing test programs (`spin`, `child`, `parent`, `looper`,
  `yielder`, `faulter`) are updated for `write`'s new fd-taking
  signature and continue to reproduce milestone 6's exact observable
  behavior.
- `docs/stdlib.md` documents every new/changed function, the `fcntl.h`
  flags, and the `errno.h` codes.

## Out of scope (future work)

- Mount points / multiple mounted volumes — split off as its own
  future milestone (needs a second addressable block device and a
  minimal VFS layer; brainstormed separately once this lands).
- `rmdir`, `rename`, long filenames (8.3 only, unchanged from
  milestone 4).
- `.`/`..` directory entries — nothing in this driver's path-lookup
  code uses them, so `mkdir` doesn't write them.
- Syncing the redundant FAT table copy `mkfs.fat` creates (`num_fats`
  copies) — only the first copy is ever read or written by this
  driver.
- True sparse files — writing past current EOF (via `lseek`) allocates
  and zero-fills real clusters for the gap, not a logical hole.
- Fd inheritance across `spawn` — there's no `fork`, so no fds to
  inherit; every new process starts with an empty file table.
- Any locking finer-grained than a single global FS lock.
- A settable `errno` global/TLS variable — failing calls return a
  specific negative error code directly instead (see Error handling).
- Changing `spawn`/`wait`/`getpid`'s existing plain `-1`-on-failure
  convention — unaffected by this milestone.

## Architecture

### ATA: write support

`kernel/ata.c` gains `ata_write_sectors(lba, count, buffer)`, mirroring
the existing `ata_read_sectors`: selects the drive/LBA, issues
`WRITE_SECTORS` (0x30), and per sector waits for `BSY` clear then `DRQ`
set before outputting 256 words via `outw`. After the last sector, it
issues `CACHE_FLUSH` (0xE7) and waits for `BSY` to clear before
returning success, so the syscall that triggered the write doesn't
return until the data is durable.

### FAT16: cluster allocation and directory-entry writing

`fat16_alloc_cluster()` scans the FAT linearly from cluster 2 for a
`0x0000` (free) entry, marks it as the chain's new end (`0xFFFF`), and
returns it (0 = disk full). `fat16_free_chain(cluster)` walks a chain
zeroing every entry. Only FAT copy 1 is ever touched; the redundant
copy(ies) `mkfs.fat` creates go stale, a documented simplification
(NeoOS's own driver never reads them).

Root directory entries live in a fixed-size, non-cluster-backed region
(a real FAT16 constraint) — creating an entry there fails outright
once all `root_entry_count` slots are full. Non-root directories are
ordinary cluster chains: `mkdir` allocates one cluster, zeroes it, and
links a new directory entry (attr=`FAT_ATTR_DIRECTORY`, size=0) into
its parent directory; writing into an existing directory that's full
allocates and links one more cluster before retrying. New file/dir
entries reuse a `0x00` (never-used) or `0xE5` (deleted) slot before
extending a directory.

`fat16_find`'s signature gains two nullable out-parameters,
`out_dir_lba`/`out_dir_offset`, giving the location of a matched
entry's on-disk directory slot so callers can patch its size/
first-cluster fields after a write without a second directory scan.
`kernel/elf.c`'s existing call passes `NULL, NULL` and needs no other
change.

`cluster_at_offset(first_cluster, byte_offset)` walks a chain from its
start to find the cluster containing a given byte offset — used by
both read and write now that access isn't purely sequential-from-zero
(`lseek` support). This is O(chain length) per call; acceptable at
this project's file-size scale, and named here as a known inefficiency
rather than silently accepted — a future milestone could cache the
last-accessed cluster per fd if it ever matters.

`fat16_write_file(first_cluster, position, buf, len, ...)` writes `len`
bytes starting at byte offset `position` within the chain rooted at
`first_cluster` (0 if the file has no clusters yet), allocating new
clusters as needed to cover `position + len`, zero-filling any gap
between the file's current size and `position` if `position` is past
current EOF, and doing read-modify-write per sector for writes that
don't cover a whole sector. It returns the (possibly new)
`first_cluster` and updated size, which the caller (the `write`
syscall handler) patches into the file's directory entry.

### Syscall layer: file descriptors and the new syscalls

`struct task` (`kernel/process.h`) gains
`struct file_descriptor files[8]`, each tracking `in_use`,
`first_cluster`, `size`, `position`, `writable`, and the owning
directory entry's `dir_entry_lba`/`dir_entry_offset`. `spawn()`
initializes a fresh task's table empty.

Fds 0/1/2 are reserved and special-cased in the syscall dispatcher
directly, not real table entries: `read(0, ...)` always returns 0 (no
keyboard-to-process input path exists), `write(1/2, ...)` goes to the
console exactly as in milestone 6. Real files use fds 3-10
(`files[fd-3]`); opening an 11th file returns `-EMFILE`.

New/changed syscalls (existing numbers 0-5 unchanged):

| Syscall | Number | Signature | Returns |
|---|---|---|---|
| `SYS_WRITE` | 1 | `(fd, buf, len)` | bytes written, or `-CODE` |
| `SYS_READ` | 6 | `(fd, buf, len)` | bytes read (0 at EOF), or `-CODE` |
| `SYS_OPEN` | 7 | `(path, path_len, flags)` | fd, or `-CODE` |
| `SYS_CLOSE` | 8 | `(fd)` | 0, or `-CODE` |
| `SYS_MKDIR` | 9 | `(path, path_len)` | 0, or `-CODE` |
| `SYS_UNLINK` | 10 | `(path, path_len)` | 0, or `-CODE` |
| `SYS_LSEEK` | 11 | `(fd, offset, whence)` | new position, or `-CODE` |

`SYS_WRITE`'s signature change (was `(buf, len)` in milestone 6) is the
one backward-incompatible change in this milestone.

A single global FS lock (`static volatile int fs_lock`, acquired via
`cli`/`sti` around the test-and-set — sufficient on this uniprocessor
kernel, where interrupts are the only preemption source) is held for
the duration of any mutating call: `open` with `O_CREAT`/`O_TRUNC`,
`write`, `mkdir`, `unlink`. A task that can't acquire it calls
`schedule()` and retries. Plain reads don't take it.

### Error handling

`lib/include/errno.h` defines named constants, using real Linux
numeric values (no compatibility need, but free and familiar):
`ENOENT=2`, `EBADF=9`, `EEXIST=17`, `ENOTDIR=20`, `EISDIR=21`,
`EINVAL=22`, `EMFILE=24`, `ENOSPC=28`. Every new/changed file syscall
(`open`/`close`/`read`/`write`/`lseek`/`mkdir`/`unlink`) returns
`-CODE` directly on failure instead of a bare `-1` — no separate
settable `errno` variable, matching the Linux kernel's own convention
of returning the negative error code as the function's result.
`spawn`/`wait`/`getpid` are unaffected and keep their existing plain
`-1`-on-failure convention.

Specific cases: `open` without `O_CREAT` on a missing path returns
`-ENOENT`; `open` with `O_CREAT` whose parent directory doesn't exist
returns `-ENOENT` (no recursive creation); `mkdir` on an existing name
returns `-EEXIST`, on a missing/non-directory parent returns
`-ENOENT`/`-ENOTDIR`; `unlink` on a directory returns `-EISDIR`, on a
missing path returns `-ENOENT`; any call needing a new cluster or
directory slot when none are available returns `-ENOSPC`; `read`/
`write`/`close`/`lseek` on an invalid or unopened fd return `-EBADF`.
As in every prior milestone, syscall argument pointers are not
validated against the calling process's own memory — the same
tracked, deferred security gap, not worsened here.

### Standard library

New header `lib/include/fcntl.h`: `int open(const char *path, int flags)`
plus `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`.

`lib/include/unistd.h` changes/additions: `write`'s signature becomes
`int64_t write(int fd, const void *buf, uint64_t len)`; new
`int64_t read(int fd, void *buf, uint64_t len)`, `int close(int fd)`,
`int64_t lseek(int fd, int64_t offset, int whence)` (with
`SEEK_SET=0`/`SEEK_CUR=1`/`SEEK_END=2`), `int mkdir(const char *path)`,
`int unlink(const char *path)`, and `STDIN_FILENO=0`/
`STDOUT_FILENO=1`/`STDERR_FILENO=2`.

`lib/stdio.c`'s `printf` changes its `write(buf, pos)` call to
`write(STDOUT_FILENO, buf, pos)`. All six existing test programs get
the same one-line change to their `write` calls — the one mechanical,
repo-wide fallout from the fd-model change, even though none of them
are otherwise part of this milestone's feature work.

## File structure

```
kernel/
  ata.c / ata.h        # + ata_write_sectors
  fat16.c / fat16.h    # + alloc_cluster, free_chain, write_file,
                        #   directory-entry creation/deletion, mkdir;
                        #   fat16_find gains two out-parameters
  process.h            # struct task + file_descriptor[8]
  syscall.c            # + OPEN/CLOSE/READ/MKDIR/UNLINK/LSEEK,
                        #   WRITE's handler changed for the fd model;
                        #   the global FS lock
lib/
  include/
    fcntl.h            # NEW: open, O_* flags
    errno.h            # NEW: ENOENT/EBADF/EEXIST/ENOTDIR/EISDIR/
                        #   EINVAL/EMFILE/ENOSPC
    unistd.h           # write's signature changes; + read/close/
                        #   lseek/mkdir/unlink, STD*_FILENO, SEEK_*
  syscall.c            # + wrappers for the new syscalls
  stdio.c              # printf's write() call updated for the fd arg
userland/
  spin.c / child.c / parent.c / looper.c / yielder.c / faulter.c
                        # write() call sites updated for the fd arg
  fileio.c             # NEW: exercises create/write/lseek/overwrite/
                        #   close/reopen/read/mkdir/unlink end-to-end
docs/
  stdlib.md            # updated: write's new signature, new
                        #   functions, fcntl.h flags, errno.h codes
```

## Testing / verification

Same QEMU + serial-log approach as every prior milestone:
- Full boot log check: all six existing programs reproduce milestone
  6's exact behavior with the updated `write` call sites.
- The new `fileio` test program's create/write/lseek-overwrite/close/
  reopen/read/mkdir/unlink sequence, verified by comparing the bytes
  it reads back against what it wrote (including the mid-file
  overwrite actually landing at the right offset).
- Independent verification with `mdir`/`mtype` (mtools, already a
  build dependency) against the post-boot disk image, confirming the
  on-disk FAT16 structure NeoOS's driver produced is structurally
  correct — not just self-consistent with its own reads.
- Regression: zero `FAILED` lines, zero exceptions in the QEMU
  interrupt log, across every run above.
