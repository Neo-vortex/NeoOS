#ifndef NEOOS_ACPI_H
#define NEOOS_ACPI_H

#include <stdint.h>

struct acpi_info {
    uint32_t lapic_address;
    uint32_t ioapic_address;
    uint32_t ioapic_gsi_base;
    uint8_t  irq0_gsi;
    uint8_t  irq0_polarity; // 0 = active-high, 1 = active-low
    uint8_t  irq0_trigger;  // 0 = edge, 1 = level
    uint8_t  irq1_gsi;
    uint8_t  irq1_polarity;
    uint8_t  irq1_trigger;
};

void acpi_find_madt(struct acpi_info *info);

#endif
