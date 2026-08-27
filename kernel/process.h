#ifndef NEOOS_PROCESS_H
#define NEOOS_PROCESS_H

#include <stdint.h>
#include "cpu.h"
#include "fs/vfs.h"

#define MAX_TASKS 16
#define KERNEL_STACK_ORDER 2 // 4 frames = 16KiB
#define MAX_OPEN_FILES 8

// Mirrors syscall_entry.asm's saved-register block exactly, in
// increasing-address order (the reverse of push order, since the
// last register pushed ends up at the lowest address). A pointer to
// the base of this block -- which already equals RSP right after the
// pushes, before `call syscall_dispatch` -- is passed into
// syscall_dispatch as its 6th argument, letting fork() copy a
// caller's full user-mode context and exec() overwrite its own
// return RIP/RSP in place.
struct syscall_frame {
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r11;       // user RFLAGS
    uint64_t rcx;       // user RIP
    uint64_t user_rsp;
};

enum task_state { TASK_UNUSED, TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_ZOMBIE };

struct file_descriptor {
    int in_use;
    struct vnode *vn;   // reference held; released by close/exit
    uint32_t position;  // per-fd, NOT shared across fork -- see docs/stdlib.md
    int writable;
};

struct task {
    int pid;
    enum task_state state;
    uint64_t saved_rsp;
    uint64_t kernel_stack_top;
    uint64_t kernel_stack_phys; // for freeing at reap time (see wait_for_pid)
    uint64_t pml4_phys; // 0 = share the kernel's own address space (kernel-mode-only task)
    int parent_pid;
    int exit_code;
    int waiting_for_pid; // 0 = not blocked in wait(); else the PID this task is waiting on
    struct file_descriptor files[MAX_OPEN_FILES];
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    struct task *next; // ready-queue link
};

void process_init(void);

// Creates a task that starts executing `entry` directly in ring 0,
// sharing the kernel's own address space. Used only by this
// milestone's early tests (Tasks 3-4) -- real processes (Task 5
// onward) are built by spawn() instead.
struct task *task_create_kernel_thread(void (*entry)(void));

void schedule(void);
struct task *current_task(void);

#define USER_STACK_PAGES 4
#define USER_STACK_TOP 0x0000700000000000ULL

struct task *spawn(const char *path);

// Duplicates the calling task, sharing its user frames copy-on-write.
// Returns the new child task, or 0 on failure (parent unaffected).
struct task *fork_task(struct syscall_frame *frame);

// Replaces the calling task's address space with the ELF at `path`,
// preserving its pid, parent, and open files. Returns 1 on success
// (the syscall's sysret lands in the new program), or 0 on failure
// with the caller left completely unchanged.
int exec_task(const char *path, struct syscall_frame *frame);

void task_exit(int code);
int64_t wait_for_pid(int pid);

#endif
