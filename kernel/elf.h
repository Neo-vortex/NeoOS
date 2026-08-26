#ifndef NEOOS_ELF_H
#define NEOOS_ELF_H

#include <stdint.h>

// Parses the ELF64 image in `data` (length `size`) and maps its
// PT_LOAD segments into `pml4` (a fresh PML4 from paging_alloc_pml4,
// not yet loaded into CR3), copying each segment's bytes in from
// `data`. Returns 1 on success with *out_entry set to the image's
// entry point, 0 on any parse/mapping failure (logged to serial).
int elf_load(const uint8_t *data, uint32_t size, uint64_t *pml4, uint64_t *out_entry);

#endif
