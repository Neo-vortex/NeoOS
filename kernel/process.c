#include "process.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "tss.h"
#include "serial.h"

extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);
extern void kernel_thread_entry_trampoline(void);

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

    if (prev == next) {
        return;
    }

    static uint64_t discarded_rsp; // used the first time schedule() is ever called, from kmain
    context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);
}
