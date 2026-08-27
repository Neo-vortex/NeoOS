#include "process.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "tss.h"
#include "serial.h"
#include "fat16.h"
#include "elf.h"
#include "cpu.h"

extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);
extern void kernel_thread_entry_trampoline(void);
extern void kernel_thread_trampoline(void);
extern uint64_t p4_table[512]; // boot.asm's live PML4

static struct task tasks[MAX_TASKS];
static struct task *ready_head;
static struct task *ready_tail;
static struct task *current;
static int next_pid = 1;

// Wipes a freshly allocated physical block before it becomes a task's
// stack. pmm_alloc() hands back whatever the previous owner left there,
// and at boot that "previous owner" is GRUB, whose own code sits in the
// high end of the memory map it then reports back to us as available --
// so the first task spawned gets stack frames still full of GRUB's
// machine code. Beyond the obvious hygiene problem (a process could read
// it), leaving it there is catastrophic for performance under QEMU's
// TCG: a guest write to a physical page that still holds translated
// blocks takes the slow notdirty path, and QEMU only drops the blocks
// overlapping the bytes actually written -- so a stack whose hot slots
// never overlap the stale code stays on that path forever, running
// 150-2000x slower than normal RAM. Zeroing the whole block once
// evicts every stale block and settles the page for good.
// (elf_load() and paging.c's alloc_table_frame() already do the same
// for the frames they hand out.)
static void zero_frames(uint64_t phys, unsigned order) {
    uint64_t *p = (uint64_t *)phys_to_virt(phys);
    uint64_t words = (PMM_FRAME_SIZE << order) / sizeof(uint64_t);
    for (uint64_t i = 0; i < words; i++) {
        p[i] = 0;
    }
}

static void enqueue_ready(struct task *t) {
    t->next = 0;
    if (ready_tail) {
        ready_tail->next = t;
    } else {
        ready_head = t;
    }
    ready_tail = t;
}

static struct task *dequeue_ready(void) {
    struct task *t = ready_head;
    if (t) {
        ready_head = t->next;
        if (!ready_head) {
            ready_tail = 0;
        }
        t->next = 0;
    }
    return t;
}

void process_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
    }
    ready_head = 0;
    ready_tail = 0;
    current = 0;
    serial_write_string("[process] initialized\n");
}

static struct task *alloc_task_slot(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            return &tasks[i];
        }
    }
    return 0;
}

struct task *task_create_kernel_thread(void (*entry)(void)) {
    struct task *t = alloc_task_slot();
    if (!t) {
        return 0;
    }

    uint64_t stack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    zero_frames(stack_phys, KERNEL_STACK_ORDER);
    uint64_t stack_top = (uint64_t)(uintptr_t)phys_to_virt(stack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    uint64_t *sp = (uint64_t *)stack_top;
    *(--sp) = (uint64_t)entry;                            // popped by kernel_thread_entry_trampoline
    *(--sp) = (uint64_t)kernel_thread_entry_trampoline;    // context_switch's `ret` lands here
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->pid = next_pid++;
    t->state = TASK_READY;
    t->saved_rsp = (uint64_t)sp;
    t->kernel_stack_top = stack_top;
    t->kernel_stack_phys = stack_phys;
    t->pml4_phys = 0;
    t->parent_pid = 0;
    t->exit_code = 0;
    t->waiting_for_pid = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        t->files[i].in_use = 0;
    }
    cpu_default_fpu_state(t->fpu_state);
    t->next = 0;

    enqueue_ready(t);
    return t;
}

struct task *current_task(void) {
    return current;
}

void schedule(void) {
    struct task *next = dequeue_ready();
    if (!next) {
        return; // nothing else ready; keep running whatever's current
    }

    struct task *prev = current;
    if (prev && prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        enqueue_ready(prev);
    }

    next->state = TASK_RUNNING;
    current = next;
    tss.rsp0 = next->kernel_stack_top;

    // Always establish a definite CR3, even for a kernel-mode-only task
    // (pml4_phys == 0 -- falls back to the kernel's own never-freed
    // p4_table). Leaving CR3 unchanged in that case used to be harmless
    // (an exited process's now-zombie PML4 just leaked, unused-but-
    // intact memory), but now that task_exit() actually frees a
    // process's PML4 frame back to the allocator, a stale CR3 left
    // pointing at it could get silently reused and overwritten by the
    // very next pmm_alloc() -- corrupting the page table the CPU is
    // still actively translating through.
    uint64_t next_cr3 = next->pml4_phys ? next->pml4_phys : (uint64_t)(uintptr_t)p4_table;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(next_cr3) : "memory");

    if (prev == next) {
        return;
    }

    static uint64_t discarded_rsp; // used the first time schedule() is ever called, from kmain
    if (prev) {
        fpu_save(prev->fpu_state);
    }
    fpu_restore(next->fpu_state);
    context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);
}

struct task *spawn(const char *path) {
    uint16_t cluster;
    uint32_t size;
    if (!fat16_find(path, &cluster, &size, NULL, NULL)) {
        serial_write_string("[process] spawn FAILED: file not found: ");
        serial_write_string(path);
        serial_write_string("\n");
        return 0;
    }

    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) {
        serial_write_string("[process] spawn FAILED: kmalloc failed for ELF image\n");
        return 0;
    }
    fat16_read_file(cluster, size, image);

    uint64_t pml4_phys = paging_alloc_pml4();
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    pml4[0] = p4_table[0];     // low identity map -- pmm.c/paging.c internals rely on it
    pml4[256] = p4_table[256]; // physmap
    pml4[511] = p4_table[511]; // kernel higher-half alias

    uint64_t entry;
    if (!elf_load(image, size, pml4, &entry)) {
        kfree(image);
        return 0;
    }
    kfree(image);

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc(0);
        zero_frames(frame, 0);
        uint64_t vaddr = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_map_into(pml4, vaddr, frame, PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_USER);
    }

    struct task *t = alloc_task_slot();
    if (!t) {
        serial_write_string("[process] spawn FAILED: no free task slot\n");
        return 0;
    }

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = USER_STACK_TOP;                    // user_rsp, popped by kernel_thread_trampoline
    *(--sp) = entry;                             // entry_rip, popped by kernel_thread_trampoline
    *(--sp) = (uint64_t)kernel_thread_trampoline; // context_switch's `ret` lands here
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->pid = next_pid++;
    t->state = TASK_READY;
    t->saved_rsp = (uint64_t)sp;
    t->kernel_stack_top = kstack_top;
    t->kernel_stack_phys = kstack_phys;
    t->pml4_phys = pml4_phys;
    t->parent_pid = current ? current->pid : 0;
    t->exit_code = 0;
    t->waiting_for_pid = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        t->files[i].in_use = 0;
    }
    cpu_default_fpu_state(t->fpu_state);
    t->next = 0;

    enqueue_ready(t);
    return t;
}

void task_exit(int code) {
    current->state = TASK_ZOMBIE;
    current->exit_code = code;
    serial_write_string("[process] task exited, pid=");
    serial_write_hex64((uint64_t)current->pid);
    serial_write_string(" code=");
    serial_write_hex64((uint64_t)(int64_t)code);
    serial_write_string("\n");

    // Uniprocessor critical section (same reasoning as syscall.c's
    // fs_lock): free_address_space() is now long enough (a full
    // page-table walk) that a timer interrupt landing mid-walk would
    // preempt this task -- and since its state is already ZOMBIE (not
    // READY), schedule() would never re-enqueue it, permanently
    // abandoning task_exit() before it reaches its own schedule() call
    // below. sti before that call is required, not optional: EFLAGS.IF
    // is not part of a task's saved context (context_switch.asm never
    // touches it), so every other schedule() caller in this kernel
    // relies on IF already being 1 -- leaving it cleared here would
    // silently disable preemption for whichever task runs next.
    __asm__ volatile ("cli");

    if (current->pml4_phys) {
        // Leave the dying address space BEFORE freeing it. A freed frame
        // stops being page-table data the instant pmm_free() takes it:
        // the buddy allocator stores each free block's next/prev links in
        // the block's own first 16 bytes, so freeing the PML4 frame
        // writes a pointer pair straight over pml4[0] and pml4[1] -- and
        // pml4[0] is the low identity map that pmm itself dereferences
        // free blocks through. Freeing while this table is still in CR3
        // therefore unmaps the identity map out from under the allocator
        // mid-call (observed: `free_lists[order]->prev = block` faulting
        // on the head pointer written one statement earlier). Switching
        // to the kernel's own never-freed p4_table is safe here for the
        // same reason schedule() falls back to it for kernel-only tasks:
        // kernel text (PML4[511]) and the physmap (PML4[256]) live there
        // too, and nothing below runs through user mappings.
        __asm__ volatile ("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)p4_table) : "memory");
        free_address_space(current->pml4_phys);
        current->pml4_phys = 0;
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].waiting_for_pid == current->pid) {
            tasks[i].state = TASK_READY;
            tasks[i].waiting_for_pid = 0;
            enqueue_ready(&tasks[i]);
        }
    }

    __asm__ volatile ("sti");
    schedule();
    for (;;) {
        __asm__ volatile ("hlt"); // unreachable: schedule() never resumes a ZOMBIE task
    }
}

int64_t wait_for_pid(int pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid == pid && tasks[i].state == TASK_ZOMBIE) {
            int code = tasks[i].exit_code;
            pmm_free(tasks[i].kernel_stack_phys, KERNEL_STACK_ORDER);
            tasks[i].state = TASK_UNUSED;
            return code;
        }
    }

    current->waiting_for_pid = pid;
    current->state = TASK_BLOCKED;
    schedule();

    // Resumed here once task_exit() (for our target pid) re-enqueued us.
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid == pid && tasks[i].state == TASK_ZOMBIE) {
            int code = tasks[i].exit_code;
            pmm_free(tasks[i].kernel_stack_phys, KERNEL_STACK_ORDER);
            tasks[i].state = TASK_UNUSED;
            return code;
        }
    }
    return -1; // shouldn't happen given task_exit's wake-up guarantee
}
