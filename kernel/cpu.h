#ifndef NEOOS_CPU_H
#define NEOOS_CPU_H

#include <stdint.h>

#define FPU_STATE_SIZE 512

// Detects required CPU features via CPUID, enables SSE (CR0/CR4), and
// captures a default FXSAVE-format state template every new task's
// fpu_state is initialized from. Halts with a diagnostic if a
// required feature (SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2) is missing --
// the kernel binary is compiled assuming their presence.
void cpu_init(void);

// Copies the default FXSAVE-format state (FPU_STATE_SIZE bytes)
// captured at cpu_init() time into `dest`, which must be at least
// FPU_STATE_SIZE bytes. Used to initialize a newly created task's
// fpu_state before it ever runs.
void cpu_default_fpu_state(void *dest);

// Saves/restores the calling CPU's current FPU/SSE register state
// to/from a FPU_STATE_SIZE-byte, 16-byte-aligned buffer. Used by
// schedule() around every context switch.
static inline void fpu_save(void *buffer) {
    __asm__ volatile ("fxsave (%0)" :: "r"(buffer) : "memory");
}

static inline void fpu_restore(void *buffer) {
    __asm__ volatile ("fxrstor (%0)" :: "r"(buffer) : "memory");
}

#endif
