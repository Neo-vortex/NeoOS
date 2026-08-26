#ifndef NEOOS_PAGING_H
#define NEOOS_PAGING_H

#include <stdint.h>

#define PHYSMAP_BASE 0xFFFF800000000000ULL

#define PAGE_PRESENT     (1ULL << 0)
#define PAGE_WRITABLE    (1ULL << 1)
#define PAGE_USER        (1ULL << 2)
#define PAGE_NO_EXECUTE  (1ULL << 63)

// Converts a physical address to its always-valid virtual alias in the
// direct physmap (see paging_init). Valid for any address within the
// first 4GiB (the physmap's coverage -- see Global Constraints).
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)(PHYSMAP_BASE + phys);
}

// Inverse of phys_to_virt, for pointers that came from it (e.g. heap
// pages allocated via pmm_alloc + phys_to_virt).
static inline uint64_t virt_to_phys_physmap(uint64_t virt) {
    return virt - PHYSMAP_BASE;
}

void paging_init(void);
void paging_selftest(void);

// General-purpose 4KiB mapping API for future callers that need a
// virtual address NOT already covered by the physmap or the kernel's
// own higher-half alias -- neither of which this function should be
// used on, since both are mapped with 2MiB pages at the PD level, and
// this walks tables assuming 4KiB PT-level entries throughout.
int paging_map(uint64_t virt, uint64_t phys, uint64_t flags);
void paging_unmap(uint64_t virt);
uint64_t paging_translate(uint64_t virt); // returns the mapped physical address, or 0 if unmapped

#endif
