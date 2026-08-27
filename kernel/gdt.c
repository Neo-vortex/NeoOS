#include <stdint.h>
#include "gdt.h"
#include "tss.h"

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t gdt_entries[8];

extern void gdt_flush(uint64_t gdtr_ptr, uint16_t data_selector,
                       uint16_t code_selector, uint16_t tss_selector);

static void set_tss_descriptor(uint64_t base, uint32_t limit) {
    uint64_t low = limit & 0xFFFF;
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40;              // present, DPL0, type=0x9 (64-bit TSS, available)
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFF) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFF;

    gdt_entries[3] = low;
    gdt_entries[4] = high;
}

void gdt_init(void) {
    gdt_entries[0] = 0;                                                           // null
    gdt_entries[1] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (1ULL << 53);    // kernel code (0x08)
    gdt_entries[2] = (1ULL << 41) | (1ULL << 44) | (1ULL << 47);                   // kernel data (0x10)
    set_tss_descriptor((uint64_t)&tss[0], sizeof(struct tss_entry) - 1);              // TSS (0x18-0x27)
    gdt_entries[5] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (3ULL << 45);    // user code32 placeholder (0x28)
    gdt_entries[6] = (1ULL << 41) | (1ULL << 44) | (1ULL << 47) | (3ULL << 45);    // user data (0x30)
    gdt_entries[7] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (1ULL << 53) | (3ULL << 45); // user code64 (0x38)

    struct gdtr gdtr = {
        .limit = sizeof(gdt_entries) - 1,
        .base = (uint64_t)&gdt_entries,
    };

    gdt_flush((uint64_t)&gdtr, GDT_KERNEL_DATA_SELECTOR,
              GDT_KERNEL_CODE_SELECTOR, GDT_TSS_SELECTOR);
}
