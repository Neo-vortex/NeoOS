#include "kernel.h"
#include "vga.h"
#include "serial.h"
#include "tss.h"
#include "gdt.h"

void kmain(void *multiboot_info) {
    (void)multiboot_info;

    serial_init();
    serial_write_string("NeoOS booting (milestone 2: interrupts)\n");

    vga_clear();
    vga_print_string("NeoOS booted");

    tss_init();
    gdt_init();
    serial_write_string("[gdt] loaded, tss_selector=0x18\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
