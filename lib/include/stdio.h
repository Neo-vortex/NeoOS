#ifndef NEOOS_STDIO_H
#define NEOOS_STDIO_H

// Minimal printf: %s, %d, %u, %x, %c, %% only -- no floating point,
// no width/precision, no length modifiers. Formats into a fixed
// internal buffer and writes it out via one write() call; there is
// no FILE*/streams concept yet, so printf always targets the same
// console write() does.
int printf(const char *fmt, ...);

#endif
