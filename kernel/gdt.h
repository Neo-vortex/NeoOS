#ifndef NEOOS_GDT_H
#define NEOOS_GDT_H

#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10
#define GDT_TSS_SELECTOR         0x18
#define GDT_USER_CODE32_SELECTOR 0x28 // never loaded -- exists only for STAR's SYSRET offset arithmetic
#define GDT_USER_DATA_SELECTOR   (0x30 | 3)
#define GDT_USER_CODE_SELECTOR   (0x38 | 3)

void gdt_init(void);

#endif
