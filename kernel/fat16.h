#ifndef NEOOS_FAT16_H
#define NEOOS_FAT16_H

#include <stdint.h>

int fat16_mount(void);
void fat16_selftest(void);
void fat16_write_selftest(void);

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
