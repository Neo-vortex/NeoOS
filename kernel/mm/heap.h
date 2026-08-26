#ifndef NEOOS_HEAP_H
#define NEOOS_HEAP_H

#include <stddef.h>

void heap_init(void);
void heap_selftest(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

#endif
