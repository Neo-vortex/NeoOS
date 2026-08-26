#include "lapic.h"
#include "mm/paging.h"

#define LAPIC_REG_ID         0x020
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_SVR        0x0F0
#define LAPIC_REG_LVT_TIMER  0x320
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

#define LVT_MASKED         (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)

static volatile uint32_t *lapic_base;

static uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t value) {
    lapic_base[reg / 4] = value;
}

void lapic_init(uint32_t address) {
    lapic_base = (volatile uint32_t *)phys_to_virt(address);
    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | 0x100 | 0xFF); // software-enable, spurious vector 0xFF
}

void lapic_send_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_timer_start_oneshot_max(void) {
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3); // divide by 16
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);
}

uint32_t lapic_timer_stop_and_read(void) {
    uint32_t current = lapic_read(LAPIC_REG_TIMER_CUR);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
    return current;
}

void lapic_timer_start_periodic(uint32_t initial_count, uint8_t vector) {
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3); // divide by 16
    lapic_write(LAPIC_REG_LVT_TIMER, (uint32_t)vector | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_INIT, initial_count);
}
