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

    if (next->pml4_phys) {
        __asm__ volatile ("mov %0, %%cr3" :: "r"(next->pml4_phys) : "memory");
    }

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
        uint64_t vaddr = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_map_into(pml4, vaddr, frame, PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_USER);
    }

    struct task *t = alloc_task_slot();
    if (!t) {
        serial_write_string("[process] spawn FAILED: no free task slot\n");
        return 0;
    }

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
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

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].waiting_for_pid == current->pid) {
            tasks[i].state = TASK_READY;
            tasks[i].waiting_for_pid = 0;
            enqueue_ready(&tasks[i]);
        }
    }

    schedule();
    for (;;) {
        __asm__ volatile ("hlt"); // unreachable: schedule() never resumes a ZOMBIE task
    }
}

int64_t wait_for_pid(int pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid == pid && tasks[i].state == TASK_ZOMBIE) {
            int code = tasks[i].exit_code;
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
            tasks[i].state = TASK_UNUSED;
            return code;
        }
    }
    return -1; // shouldn't happen given task_exit's wake-up guarantee
}
