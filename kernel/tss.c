#include "tss.h"

#define IST1_STACK_SIZE 4096

struct tss_entry tss[MAX_TSS];
static unsigned char ist1_stack[IST1_STACK_SIZE] __attribute__((aligned(16)));

void tss_init(void) {
    unsigned char *raw = (unsigned char *)&tss[0];
    for (unsigned int i = 0; i < sizeof(struct tss_entry); i++) {
        raw[i] = 0;
    }
    tss[0].ist1 = (uint64_t)(ist1_stack + IST1_STACK_SIZE);
    tss[0].iomap_base = sizeof(struct tss_entry);
}
