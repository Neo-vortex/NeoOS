#ifndef NEOOS_STRING_H
#define NEOOS_STRING_H

#include <stdint.h>

uint64_t strlen(const char *s);
void *memcpy(void *dst, const void *src, uint64_t n);
void *memset(void *s, int c, uint64_t n);
void *memmove(void *dst, const void *src, uint64_t n);

#endif
