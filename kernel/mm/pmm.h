#ifndef NEOOS_PMM_H
#define NEOOS_PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE 4096
#define PMM_MAX_ORDER  10 // largest block = 4096 * 2^10 = 4MiB

void pmm_init(void *multiboot_info);
void pmm_selftest(void);

// Allocates 2^order contiguous frames; returns the physical base address,
// or 0 on out-of-memory. order must be <= PMM_MAX_ORDER.
uint64_t pmm_alloc(unsigned order);

// Frees a block previously returned by pmm_alloc with the same order.
void pmm_free(uint64_t phys_addr, unsigned order);

uint64_t pmm_free_frame_count(void);

#endif
