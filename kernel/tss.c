#include "tss.h"

#define IST1_STACK_SIZE 4096

struct tss_entry tss;
static unsigned char ist1_stack[IST1_STACK_SIZE] __attribute__((aligned(16)));

void tss_init(void) {
    unsigned char *raw = (unsigned char *)&tss;
    for (unsigned int i = 0; i < sizeof(tss); i++) {
        raw[i] = 0;
    }
    tss.ist1 = (uint64_t)(ist1_stack + IST1_STACK_SIZE);
    tss.iomap_base = sizeof(struct tss_entry);
}
