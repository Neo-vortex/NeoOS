#ifndef NEOOS_SERIAL_H
#define NEOOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_write_string(const char *str);
void serial_write_hex64(uint64_t value);

#endif
