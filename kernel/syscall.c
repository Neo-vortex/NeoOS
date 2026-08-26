#include "syscall.h"
#include "gdt.h"
#include "serial.h"
#include "process.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

#define EFER_SCE (1ULL << 0)
#define EFER_NXE (1ULL << 11)

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_SPAWN  4
#define SYS_WAIT   5

extern void syscall_entry(void); // syscall_entry.asm

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

// Called only from syscall_entry.asm's `call syscall_dispatch`.
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    (void)a3;
    (void)a4;
    switch (num) {
        case SYS_EXIT:
            task_exit((int)a1);
            return 0; // unreachable -- task_exit never returns
        case SYS_WRITE:
            serial_write_string_n((const char *)(uintptr_t)a1, (uint64_t)a2);
            return a2;
        case SYS_GETPID:
            return current_task()->pid;
        case SYS_YIELD:
            schedule();
            return 0;
        case SYS_SPAWN: {
            char path_buf[64];
            uint64_t len = (uint64_t)a2;
            if (len > sizeof(path_buf) - 1) {
                len = sizeof(path_buf) - 1;
            }
            const char *user_path = (const char *)(uintptr_t)a1;
            for (uint64_t i = 0; i < len; i++) {
                path_buf[i] = user_path[i];
            }
            path_buf[len] = '\0';
            struct task *child = spawn(path_buf);
            return child ? child->pid : -1;
        }
        case SYS_WAIT:
            return wait_for_pid((int)a1);
        default:
            serial_write_string("[syscall] unknown syscall number\n");
            return -1;
    }
}

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    // EFER_NXE: elf_load (Task 5) is the first code in NeoOS to
    // actually set PAGE_NO_EXECUTE (bit 63) on a real PTE -- without
    // NXE enabled, that bit is reserved and setting it faults the
    // moment the page is walked. Grouped here since it's the same MSR
    // as EFER_SCE, not because it's conceptually part of SYSCALL setup.
    wrmsr(MSR_EFER, efer | EFER_SCE | EFER_NXE);

    // STAR[47:32] = kernel CS (kernel SS = that + 8, matches
    // GDT_KERNEL_DATA_SELECTOR at 0x10); STAR[63:48] = the SYSRET
    // base (user data at that+8, user code64 at that+16 -- see
    // Task 1's GDT layout).
    uint64_t star = ((uint64_t)GDT_USER_CODE32_SELECTOR << 48) | ((uint64_t)GDT_KERNEL_CODE_SELECTOR << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200); // mask IF (bit 9) on syscall entry

    serial_write_string("[syscall] SYSCALL/SYSRET configured\n");
}
