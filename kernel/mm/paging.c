#include "paging.h"
#include "pmm.h"
#include "serial.h"

#define PAGE_HUGE (1ULL << 7) // 2MiB page at the PD level
#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PHYSMAP_SIZE_BYTES (4ULL * 1024 * 1024 * 1024) // first 4GiB: all supported RAM plus the sub-4GiB MMIO hole (LAPIC/IOAPIC)
#define PHYSMAP_PML4_INDEX 256

#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)

extern uint64_t p4_table[512]; // boot.asm's live PML4 -- see boot/boot.asm

static uint64_t alloc_table_frame(void) {
    uint64_t phys = pmm_alloc(0);
    uint64_t *table = (uint64_t *)(uintptr_t)phys;
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }
    return phys;
}

// Walks one level, allocating a fresh table if `create` is set and the
// entry isn't present yet. Assumes 4KiB-page-tree structure throughout
// (not valid on huge-page-mapped regions -- see paging.h).
static uint64_t *table_entry(uint64_t *table, unsigned index, int create, uint64_t create_flags) {
    if (!(table[index] & PAGE_PRESENT)) {
        if (!create) {
            return 0;
        }
        uint64_t new_table_phys = alloc_table_frame();
        table[index] = new_table_phys | create_flags;
    }
    uint64_t next_phys = table[index] & PAGE_ADDR_MASK;
    return (uint64_t *)(uintptr_t)next_phys;
}

int paging_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t default_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t *pdpt = table_entry(p4_table, PML4_INDEX(virt), 1, default_flags);
    uint64_t *pd   = table_entry(pdpt, PDPT_INDEX(virt), 1, default_flags);
    uint64_t *pt   = table_entry(pd, PD_INDEX(virt), 1, default_flags);

    pt[PT_INDEX(virt)] = (phys & PAGE_ADDR_MASK) | flags | PAGE_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

void paging_unmap(uint64_t virt) {
    uint64_t *pdpt = table_entry(p4_table, PML4_INDEX(virt), 0, 0);
    uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(virt), 0, 0) : 0;
    uint64_t *pt   = pd ? table_entry(pd, PD_INDEX(virt), 0, 0) : 0;
    if (pt) {
        pt[PT_INDEX(virt)] = 0;
        __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    }
}

uint64_t paging_translate(uint64_t virt) {
    uint64_t *pdpt = table_entry(p4_table, PML4_INDEX(virt), 0, 0);
    uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(virt), 0, 0) : 0;
    uint64_t *pt   = pd ? table_entry(pd, PD_INDEX(virt), 0, 0) : 0;
    if (!pt || !(pt[PT_INDEX(virt)] & PAGE_PRESENT)) {
        return 0;
    }
    return (pt[PT_INDEX(virt)] & PAGE_ADDR_MASK) | (virt & 0xFFF);
}

void paging_init(void) {
    uint64_t pdpt_phys = alloc_table_frame();
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;

    uint64_t total_pages = PHYSMAP_SIZE_BYTES / (2 * 1024 * 1024);
    uint64_t pages_mapped = 0;
    for (uint64_t pdpt_index = 0; pages_mapped < total_pages; pdpt_index++) {
        uint64_t pd_phys = alloc_table_frame();
        uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys;

        for (unsigned pd_index = 0; pd_index < 512 && pages_mapped < total_pages; pd_index++, pages_mapped++) {
            uint64_t page_phys = pages_mapped * (2ULL * 1024 * 1024);
            pd[pd_index] = page_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
        }

        pdpt[pdpt_index] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    p4_table[PHYSMAP_PML4_INDEX] = pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE;

    serial_write_string("[paging] physmap installed: base=");
    serial_write_hex64(PHYSMAP_BASE);
    serial_write_string(" size=");
    serial_write_hex64(PHYSMAP_SIZE_BYTES);
    serial_write_string("\n");
}

#define PAGING_SELFTEST_VA 0xFFFF900000000000ULL

void paging_selftest(void) {
    uint64_t scratch_phys = pmm_alloc(0);
    if (!scratch_phys) {
        serial_write_string("[paging] selftest FAILED: pmm_alloc returned 0\n");
        return;
    }

    if (paging_map(PAGING_SELFTEST_VA, scratch_phys, PAGE_WRITABLE) != 0) {
        serial_write_string("[paging] selftest FAILED: paging_map error\n");
        return;
    }

    volatile uint8_t *scratch = (volatile uint8_t *)(uintptr_t)PAGING_SELFTEST_VA;
    *scratch = 0x42;
    if (*scratch != 0x42) {
        serial_write_string("[paging] selftest FAILED: pattern mismatch through new mapping\n");
        return;
    }

    if (paging_translate(PAGING_SELFTEST_VA) != scratch_phys) {
        serial_write_string("[paging] selftest FAILED: translate did not round-trip\n");
        return;
    }

    paging_unmap(PAGING_SELFTEST_VA);
    if (paging_translate(PAGING_SELFTEST_VA) != 0) {
        serial_write_string("[paging] selftest FAILED: translate still resolves after unmap\n");
        return;
    }

    pmm_free(scratch_phys, 0);
    serial_write_string("[paging] selftest passed\n");
}
