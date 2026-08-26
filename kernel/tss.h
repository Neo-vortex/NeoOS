#ifndef NEOOS_TSS_H
#define NEOOS_TSS_H

#include <stdint.h>

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern struct tss_entry tss;

void tss_init(void);

#endif
