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
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "ata.h"
#include "fat16.h"
#include "process.h"
#include "syscall.h"

void kmain(void *multiboot_info) {
    serial_init();
    serial_write_string("NeoOS booting (milestone 4: storage)\n");
    serial_write_string("[boot] kmain address=");
    serial_write_hex64((uint64_t)(uintptr_t)kmain);
    serial_write_string("\n");

    pmm_init(multiboot_info);
    pmm_selftest();

    paging_init();
    paging_selftest();

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

    heap_init();
    heap_selftest();

    struct ata_identify_info ata_info;
    ata_identify(&ata_info);

    fat16_mount();
    fat16_selftest();

    process_init();
    syscall_init();

    struct task *spin_task = spawn("/BIN/SPIN.ELF");
    if (!spin_task) {
        serial_write_string("[process] spawn FAILED for /BIN/SPIN.ELF\n");
    }

    serial_write_string("NeoOS: interrupts enabled, starting scheduler\n");
    __asm__ volatile ("sti");

    schedule(); // never returns in practice -- control passes permanently into the task system
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
