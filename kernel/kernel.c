#include "kernel.h"

static volatile unsigned short *const VGA_BUFFER = (unsigned short *)0xb8000;
static const unsigned short VGA_COLOR_WHITE_ON_BLACK = 0x0f;

void vga_print_string(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        VGA_BUFFER[i] = (unsigned short)((unsigned char)str[i]) |
                        (unsigned short)(VGA_COLOR_WHITE_ON_BLACK << 8);
    }
}

void kmain(void *multiboot_info) {
    (void)multiboot_info;

    vga_print_string("NeoOS booted");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
