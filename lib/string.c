#include "string.h"

uint64_t strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

void *memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *s, int c, uint64_t n) {
    uint8_t *p = (uint8_t *)s;
    for (uint64_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

void *memmove(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (uint64_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (uint64_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}
