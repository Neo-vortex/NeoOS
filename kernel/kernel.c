#include "kernel.h"
#include "vga.h"
#include "serial.h"

void kmain(void *multiboot_info) {
    (void)multiboot_info;

    serial_init();
    serial_write_string("NeoOS booting (milestone 2: interrupts)\n");

    vga_clear();
    vga_print_string("NeoOS booted");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
