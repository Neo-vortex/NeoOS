#include "syscall.h"
#include "gdt.h"
#include "serial.h"
#include "process.h"
#include "fat16.h"
#include "errno.h"

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
#define SYS_READ   6
#define SYS_OPEN   7
#define SYS_CLOSE  8
#define SYS_MKDIR  9
#define SYS_UNLINK 10
#define SYS_LSEEK  11

// Mirrors lib/include/fcntl.h's O_* values exactly -- the two trees
// don't share headers, so these must be kept in sync by hand.
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

// Mirrors lib/include/unistd.h's SEEK_* values exactly.
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

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

static volatile int fs_lock = 0;

// Uniprocessor spinlock: cli/sti around the test-and-set is enough
// since interrupts are the only source of preemption here. A task
// that loses the race yields and retries rather than busy-spinning
// with interrupts disabled the whole time.
static void fs_lock_acquire(void) {
    for (;;) {
        __asm__ volatile ("cli");
        if (!fs_lock) {
            fs_lock = 1;
            __asm__ volatile ("sti");
            return;
        }
        __asm__ volatile ("sti");
        schedule();
    }
}

static void fs_lock_release(void) {
    fs_lock = 0;
}

// Copies up to out_size-1 bytes from a user-supplied (pointer, len)
// pair into a NUL-terminated kernel buffer. Shared by every syscall
// that takes a path (SPAWN/OPEN/MKDIR/UNLINK).
static void copy_user_path(int64_t user_ptr, int64_t user_len, char *out, uint64_t out_size) {
    uint64_t len = (uint64_t)user_len;
    if (len > out_size - 1) {
        len = out_size - 1;
    }
    const char *user_path = (const char *)(uintptr_t)user_ptr;
    for (uint64_t i = 0; i < len; i++) {
        out[i] = user_path[i];
    }
    out[len] = '\0';
}

// Called only from syscall_entry.asm's `call syscall_dispatch`.
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    (void)a4;
    switch (num) {
        case SYS_EXIT:
            task_exit((int)a1);
            return 0; // unreachable -- task_exit never returns
        case SYS_WRITE: {
            int fd = (int)a1;
            const char *buf = (const char *)(uintptr_t)a2;
            uint64_t len = (uint64_t)a3;
            if (fd == 1 || fd == 2) {
                serial_write_string_n(buf, len);
                return (int64_t)len;
            }
            if (fd < 3 || fd >= 3 + MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use || !f->writable) {
                return -EBADF;
            }
            fs_lock_acquire();
            uint16_t new_cluster;
            uint32_t new_size;
            int written = fat16_write_file(f->first_cluster, f->size, f->position, buf, (uint32_t)len, &new_cluster, &new_size);
            if (written < 0) {
                fs_lock_release();
                return written;
            }
            if (new_cluster != f->first_cluster || new_size != f->size) {
                fat16_update_entry_size(f->dir_entry_lba, f->dir_entry_offset, new_cluster, new_size);
            }
            f->first_cluster = new_cluster;
            f->size = new_size;
            f->position += (uint32_t)len;
            fs_lock_release();
            return written;
        }
        case SYS_READ: {
            int fd = (int)a1;
            char *buf = (char *)(uintptr_t)a2;
            uint64_t len = (uint64_t)a3;
            if (fd == 0) {
                return 0; // no keyboard-to-process input path yet
            }
            if (fd < 3 || fd >= 3 + MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use) {
                return -EBADF;
            }
            uint32_t remaining = f->size > f->position ? f->size - f->position : 0;
            uint32_t to_read = (uint32_t)len;
            if (to_read > remaining) {
                to_read = remaining;
            }
            fat16_read_at(f->first_cluster, f->position, buf, to_read);
            f->position += to_read;
            return to_read;
        }
        case SYS_GETPID:
            return current_task()->pid;
        case SYS_YIELD:
            schedule();
            return 0;
        case SYS_SPAWN: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            struct task *child = spawn(path_buf);
            return child ? child->pid : -1;
        }
        case SYS_WAIT:
            return wait_for_pid((int)a1);
        case SYS_OPEN: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            int flags = (int)a3;

            struct task *task = current_task();
            int slot = -1;
            for (int i = 0; i < MAX_OPEN_FILES; i++) {
                if (!task->files[i].in_use) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                return -EMFILE;
            }

            fs_lock_acquire();

            uint16_t cluster;
            uint32_t size;
            uint32_t dir_lba;
            uint16_t dir_offset;
            int found = fat16_find(path_buf, &cluster, &size, &dir_lba, &dir_offset);

            if (!found) {
                if (!(flags & O_CREAT)) {
                    fs_lock_release();
                    return -ENOENT;
                }
                int created = fat16_create_file(path_buf, &dir_lba, &dir_offset);
                if (created < 0) {
                    fs_lock_release();
                    return created;
                }
                cluster = 0;
                size = 0;
            } else if (flags & O_TRUNC) {
                fat16_truncate(cluster, dir_lba, dir_offset, &cluster);
                size = 0;
            }

            fs_lock_release();

            struct file_descriptor *f = &task->files[slot];
            f->in_use = 1;
            f->first_cluster = cluster;
            f->size = size;
            f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
            f->dir_entry_lba = dir_lba;
            f->dir_entry_offset = dir_offset;
            f->position = (flags & O_APPEND) ? size : 0;

            return slot + 3;
        }
        case SYS_CLOSE: {
            int fd = (int)a1;
            if (fd < 3 || fd >= 3 + MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use) {
                return -EBADF;
            }
            f->in_use = 0;
            return 0;
        }
        case SYS_MKDIR: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            fs_lock_acquire();
            int result = fat16_mkdir(path_buf);
            fs_lock_release();
            return result;
        }
        case SYS_UNLINK: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            fs_lock_acquire();
            int result = fat16_delete_entry(path_buf);
            fs_lock_release();
            return result;
        }
        case SYS_LSEEK: {
            int fd = (int)a1;
            int64_t offset = a2;
            int whence = (int)a3;
            if (fd < 3 || fd >= 3 + MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_task()->files[fd - 3];
            if (!f->in_use) {
                return -EBADF;
            }
            int64_t base;
            switch (whence) {
                case SEEK_SET: base = 0; break;
                case SEEK_CUR: base = (int64_t)f->position; break;
                case SEEK_END: base = (int64_t)f->size; break;
                default: return -EINVAL;
            }
            int64_t new_position = base + offset;
            if (new_position < 0) {
                return -EINVAL;
            }
            f->position = (uint32_t)new_position;
            return new_position;
        }
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
