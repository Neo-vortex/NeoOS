#include "kernel.h"
#include "vga.h"
#include "serial.h"
#include "tss.h"
#include "gdt.h"
#include "idt.h"
#include "acpi.h"
#include "pic.h"
#include "lapic.h"
#include "ioapic.h"
#include "timer.h"
#include "keyboard.h"

void kmain(void *multiboot_info) {
    (void)multiboot_info;

    serial_init();
    serial_write_string("NeoOS booting (milestone 2: interrupts)\n");

    vga_clear();
    vga_print_string("NeoOS booted");

    tss_init();
    gdt_init();
    serial_write_string("[gdt] loaded, tss_selector=0x18\n");

    idt_init();
    serial_write_string("[idt] loaded\n");

    struct acpi_info acpi;
    acpi_find_madt(&acpi);

    pic_disable();
    serial_write_string("[pic] disabled\n");

    lapic_init(acpi.lapic_address);
    serial_write_string("[lapic] enabled, id="); serial_write_hex64(lapic_get_id());
    serial_write_string("\n");

    ioapic_init(acpi.ioapic_address);
    serial_write_string("[ioapic] initialized\n");

    timer_init();

    uint8_t keyboard_pin = acpi.irq1_gsi - acpi.ioapic_gsi_base;
    ioapic_set_redirection(keyboard_pin, VECTOR_KEYBOARD, acpi.irq1_polarity,
                            acpi.irq1_trigger, (uint8_t)lapic_get_id());
    serial_write_string("[ioapic] keyboard routed: gsi=");
    serial_write_hex64(acpi.irq1_gsi);
    serial_write_string(" vector=0x21\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
