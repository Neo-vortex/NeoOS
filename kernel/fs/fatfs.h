#ifndef NEOOS_FATFS_H
#define NEOOS_FATFS_H

#include <stdint.h>
#include "../errno.h"
#include "vfs.h"

// FAT inode identity is the file's directory-entry location on disk,
// (dir_entry_lba << 16) | dir_entry_offset -- unique per file per
// volume. First cluster will not serve: every empty file has cluster
// 0. Reserved id 0 means the root directory, which has no directory
// entry of its own; that cannot collide with a real entry, because id
// 0 requires dir_entry_lba == 0 and LBA 0 is the boot sector.
#define FATFS_ROOT_INODE 0ULL

extern const struct vfs_ops fatfs_ops;

int fat16_mount(void);
void fat16_selftest(void);
void fat16_write_selftest(void);

// Looks up a path like "/DIR/FILE.TXT" (8.3 components only). On
// success returns 1 and fills *out_cluster/*out_size; if
// out_dir_lba/out_dir_offset are non-NULL, also fills them with the
// on-disk location of the matched directory entry (for callers that
// need to patch it later, e.g. after a write). On failure (any path
// component not found, or a non-directory component used as a
// directory) returns 0.
int fat16_find(const char *path, uint16_t *out_cluster, uint32_t *out_size,
               uint32_t *out_dir_lba, uint16_t *out_dir_offset);

// Reads a file's full contents (given the cluster/size fat16_find
// returned) into buffer, which must be at least size bytes. Returns
// the number of bytes actually read (equals size on success).
uint32_t fat16_read_file(uint16_t first_cluster, uint32_t size, void *buffer);

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

#endif
