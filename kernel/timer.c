#include "timer.h"
#include "pit.h"
#include "lapic.h"
#include "serial.h"

#define TICKS_PER_LOG 100 // 100Hz timer -> log once per second

static volatile uint64_t tick_count = 0;

void timer_handler(void) {
    tick_count++;
    if (tick_count % TICKS_PER_LOG == 0) {
        serial_write_string("[timer] tick=");
        serial_write_hex64(tick_count);
        serial_write_string("\n");
    }
}

void timer_init(void) {
    // Calibrating over exactly 10ms and targeting 100Hz (10ms period)
    // means the calibrated tick count IS the periodic initial count.
    uint32_t ticks_per_10ms = pit_calibrate_lapic_ticks_per_10ms();
    serial_write_string("[timer] calibrated lapic ticks per 10ms=");
    serial_write_hex64(ticks_per_10ms);
    serial_write_string("\n");

    lapic_timer_start_periodic(ticks_per_10ms, VECTOR_TIMER);
}
