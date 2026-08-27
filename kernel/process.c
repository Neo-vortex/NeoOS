#include "process.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "tss.h"
#include "serial.h"
#include "fs/vfs.h"
#include "elf.h"
#include "cpu.h"
#include "cpu_local.h"

extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);
extern void kernel_thread_entry_trampoline(void);
extern void kernel_thread_trampoline(void);
extern void fork_trampoline(void);
extern uint64_t p4_table[512]; // boot.asm's live PML4

static struct process *proc_list;
static struct spinlock proc_lock;
static struct thread *ready_head;
static struct thread *ready_tail;
// Starts at 0 so the idle thread -- created first, by idle_init() --
// naturally takes id 0, which is reserved for idle threads and is
// never a valid pid. The first real process therefore gets pid 1.
static int next_id = 0;

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

static void enqueue_ready(struct thread *t) {
    t->next = 0;
    if (ready_tail) {
        ready_tail->next = t;
    } else {
        ready_head = t;
    }
    ready_tail = t;
}

static struct thread *dequeue_ready(void) {
    struct thread *t = ready_head;
    if (t) {
        ready_head = t->next;
        if (!ready_head) {
            ready_tail = 0;
        }
        t->next = 0;
    }
    return t;
}

void thread_enqueue_ready(struct thread *t) { enqueue_ready(t); }

// Removes `t` from the ready queue wherever it sits. Only used by
// idle_init, which has to un-enqueue the idle thread that
// thread_alloc_kernel just queued.
static void dequeue_specific(struct thread *t) {
    struct thread **pp = &ready_head;
    struct thread *prev = 0;
    while (*pp && *pp != t) { prev = *pp; pp = &(*pp)->next; }
    if (*pp) {
        *pp = t->next;
        if (ready_tail == t) { ready_tail = prev; }
    }
    t->next = 0;
}

struct thread  *current_thread(void) { return this_cpu()->current; }
struct process *current_proc(void) {
    struct thread *t = current_thread();
    return t ? t->proc : 0;
}

static int alloc_id(void) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    int id = next_id++;
    spin_unlock_irqrestore(&proc_lock, f);
    return id;
}

// thread->fpu_state is fxsave/fxrstor'd directly out of this
// allocation, and those #GP on an address that is not 16-byte aligned.
// heap.c's struct heap_page is 64-byte aligned specifically so every
// kmalloc slot satisfies that; see the comment there.
static struct thread *thread_alloc(struct process *p) {
    struct thread *t = (struct thread *)kmalloc(sizeof(struct thread));
    if (!t) { return 0; }
    for (unsigned i = 0; i < sizeof(struct thread); i++) { ((uint8_t *)t)[i] = 0; }
    // A process's FIRST thread takes the pid as its tid, matching
    // Linux (main thread: tid == pid). Later threads draw fresh ids
    // from the same counter, so a tid never collides with a pid, and a
    // single-threaded process consumes exactly one id -- which is what
    // keeps pids stable across this refactor.
    t->tid        = (p && !p->threads) ? p->pid : alloc_id();
    t->proc       = p;
    t->state      = THREAD_READY;
    t->stack_slot = -1;
    cpu_default_fpu_state(t->fpu_state);
    if (p) {
        t->proc_next = p->threads;
        p->threads   = t;
        p->refcount++;
    }
    return t;
}

static struct process *proc_alloc(void) {
    struct process *p = (struct process *)kmalloc(sizeof(struct process));
    if (!p) { return 0; }
    for (unsigned i = 0; i < sizeof(struct process); i++) { ((uint8_t *)p)[i] = 0; }
    p->pid   = alloc_id();
    p->state = PROC_ALIVE;

    uint64_t f = spin_lock_irqsave(&proc_lock);
    p->next   = proc_list;
    proc_list = p;
    spin_unlock_irqrestore(&proc_lock, f);
    return p;
}

static struct process *proc_find(int pid) {
    for (struct process *p = proc_list; p; p = p->next) {
        if (p->pid == pid) { return p; }
    }
    return 0;
}

// Runs whenever no other thread is ready. Having a real idle thread
// removes schedule()'s old "nothing ready, keep running whatever's
// current" special case for the blocked/dead-current cases.
static void idle_entry(void) {
    for (;;) { __asm__ volatile ("sti; hlt"); }
}

static void idle_init(void) {
    struct thread *t = thread_alloc_kernel(idle_entry);
    // Reserved: idle threads are never a valid pid. thread_alloc()
    // already handed out id 0 here, since idle_init() runs before any
    // other allocation -- see next_id's initialiser.
    t->tid = 0;
    dequeue_specific(t);   // never on the ready queue; schedule() falls back to it
    this_cpu()->idle = t;
}

void process_init(void) {
    spin_init(&proc_lock, LOCK_RANK_PROCTABLE, "proc_list");
    proc_list  = 0;
    ready_head = 0;
    ready_tail = 0;
    this_cpu()->current = 0;
    idle_init();
    serial_write_string("[process] initialized\n");
}

struct thread *thread_alloc_kernel(void (*entry)(void)) {
    struct thread *t = thread_alloc(0);
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

    t->saved_rsp = (uint64_t)sp;
    t->kernel_stack_top = stack_top;
    t->kernel_stack_phys = stack_phys;

    enqueue_ready(t);
    return t;
}

// Restores EFLAGS.IF to whatever it was on entry to schedule(). Split
// out because schedule() has three exits (no-task, same-task, and the
// far side of a context switch, possibly milliseconds later in a
// different task).
static inline void schedule_restore_if(uint64_t saved_flags) {
    if (saved_flags & (1ULL << 9)) {
        __asm__ volatile ("sti");
    }
}

void schedule(void) {
    // schedule() is NOT reentrant, and until this cli it ran with
    // interrupts enabled. Between `current = next` and the
    // context_switch() below, `current` already names the incoming
    // task while execution is still on the OUTGOING task's stack -- so
    // a timer interrupt landing in that window re-enters schedule()
    // with prev == the incoming task, and context_switch's
    // `mov [rdi], rsp` stamps the outgoing task's RSP into the
    // incoming task's saved_rsp. That task is then resumed on a stack
    // that isn't its own (observed: pid 6 resumed with an RSP pointing
    // into pid 5's kernel stack, faulting in syscall_dispatch's
    // epilogue with a garbage RBP, escalating to a double fault).
    //
    // The window was always there, but nothing hit it until fork()
    // made it easy to have several tasks doing nothing but yield(),
    // which keeps schedule() executing a large fraction of the time.
    //
    // IF is restored rather than unconditionally set because
    // timer_handler() calls schedule() from an interrupt gate with
    // IF already 0, and must return to the ISR with it still 0 -- the
    // iretq there is what re-enables it. `flags` is a local, so it
    // lives on this task's own kernel stack and is still correct
    // whenever this task is eventually resumed.
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    struct cpu *c = this_cpu();

    struct thread *next = dequeue_ready();
    if (!next) {
        struct thread *cur = c->current;
        if (cur && cur->state == THREAD_RUNNING) {
            schedule_restore_if(flags);
            return; // nothing else ready; keep running whatever's current
        }
        next = c->idle; // current is blocked or dead -- park on idle
    }

    struct thread *prev = c->current;
    if (prev && prev->state == THREAD_RUNNING && prev != c->idle) {
        prev->state = THREAD_READY;
        enqueue_ready(prev);
    }

    next->state = THREAD_RUNNING;
    c->current = next;
    c->tss->rsp0    = next->kernel_stack_top;
    c->kernel_stack = next->kernel_stack_top;

    // Always establish a definite CR3, even for a kernel-mode-only task
    // (pml4_phys == 0 -- falls back to the kernel's own never-freed
    // p4_table). Leaving CR3 unchanged in that case used to be harmless
    // (an exited process's now-zombie PML4 just leaked, unused-but-
    // intact memory), but now that task_exit() actually frees a
    // process's PML4 frame back to the allocator, a stale CR3 left
    // pointing at it could get silently reused and overwritten by the
    // very next pmm_alloc() -- corrupting the page table the CPU is
    // still actively translating through.
    uint64_t next_cr3 = (next->proc && next->proc->pml4_phys)
                      ? next->proc->pml4_phys
                      : (uint64_t)(uintptr_t)p4_table;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(next_cr3) : "memory");

    if (prev == next) {
        schedule_restore_if(flags);
        return;
    }

    static uint64_t discarded_rsp; // used the first time schedule() is ever called, from kmain
    if (prev) {
        fpu_save(prev->fpu_state);
    }
    fpu_restore(next->fpu_state);
    context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);

    // Reached only when THIS task is scheduled back in, which may be
    // much later; `flags` is the IF state from its own entry above.
    schedule_restore_if(flags);
}

// Builds a complete, freshly-loaded user address space from the ELF
// image at `path`: a new PML4 with the shared kernel entries, the
// loaded ELF segments, and a fresh user stack. On success, returns 1
// with *out_pml4_phys/*out_entry set; the caller (spawn() for a new
// task, exec_task() for an existing one) is responsible for wiring
// the result into a process. On failure, returns 0 having freed
// any partial address space it built -- the caller's own state (if
// any) is untouched.
static int build_user_address_space(const char *path, uint64_t *out_pml4_phys, uint64_t *out_entry) {
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) {
        serial_write_string("[process] FAILED: file not found: ");
        serial_write_string(path);
        serial_write_string("\n");
        return 0;
    }
    uint32_t size = vn->size;

    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) {
        serial_write_string("[process] FAILED: kmalloc failed for ELF image\n");
        vnode_put(vn);
        return 0;
    }
    vn->mount->ops->read(vn, 0, image, size);
    vnode_put(vn);

    uint64_t pml4_phys = paging_alloc_pml4();
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    pml4[0] = p4_table[0];     // low identity map -- pmm.c/paging.c internals rely on it
    pml4[256] = p4_table[256]; // physmap
    pml4[511] = p4_table[511]; // kernel higher-half alias

    uint64_t entry;
    if (!elf_load(image, size, pml4, &entry)) {
        kfree(image);
        free_address_space(pml4_phys);
        return 0;
    }
    kfree(image);

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc(0);
        if (!frame) {
            free_address_space(pml4_phys);
            return 0;
        }
        zero_frames(frame, 0);
        uint64_t vaddr = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_map_into(pml4, vaddr, frame, PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_USER);
    }

    *out_pml4_phys = pml4_phys;
    *out_entry = entry;
    return 1;
}

struct process *spawn(const char *path) {
    uint64_t pml4_phys, entry;
    if (!build_user_address_space(path, &pml4_phys, &entry)) {
        return 0;
    }

    struct process *p = proc_alloc();
    if (!p) {
        serial_write_string("[process] spawn FAILED: out of memory for process\n");
        free_address_space(pml4_phys);
        return 0;
    }
    p->pml4_phys   = pml4_phys;
    p->parent_pid  = current_proc() ? current_proc()->pid : 0;
    p->stack_slots = 1; // slot 0 is the main thread's stack

    struct thread *t = thread_alloc(p);
    if (!t) {
        serial_write_string("[process] spawn FAILED: out of memory for thread\n");
        free_address_space(pml4_phys);
        return 0;
    }
    t->stack_slot = 0;

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = USER_STACK_TOP;                     // user_rsp, popped by kernel_thread_trampoline
    *(--sp) = entry;                              // entry_rip, popped by kernel_thread_trampoline
    *(--sp) = (uint64_t)kernel_thread_trampoline; // context_switch's `ret` lands here
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->saved_rsp         = (uint64_t)sp;
    t->kernel_stack_top  = kstack_top;
    t->kernel_stack_phys = kstack_phys;

    // Standard streams as real /dev/CONSOLE vnodes. stdin is opened
    // read-only and always returns EOF; stdout and stderr both write
    // to the console. The table belongs to the process, so every
    // thread of it shares these.
    vfs_open_into("/dev/CONSOLE", p, 0, 0);
    vfs_open_into("/dev/CONSOLE", p, 1, 1);
    vfs_open_into("/dev/CONSOLE", p, 2, 1);

    enqueue_ready(t);
    return p;
}

// Replaces the calling task's address space in place with the ELF
// image at `path`. Open files, pid, and parent_pid are preserved
// (POSIX default: exec() does not close file descriptors). Returns 1
// on success (the syscall path never actually returns to the old
// program -- frame's saved RIP/RSP are overwritten so the ordinary
// sysret lands in the new one instead), or 0 on failure, leaving the
// calling task completely unchanged and still runnable -- the new
// address space is built and validated to completion before the old
// one is freed, so a bad path or OOM never destroys the caller.
int exec_task(const char *path, struct syscall_frame *frame) {
    uint64_t new_pml4_phys, new_entry;
    if (!build_user_address_space(path, &new_pml4_phys, &new_entry)) {
        return 0;
    }

    struct process *p = current_proc();

    // Switch to the new address space BEFORE freeing the old one, for
    // the same reason task_exit() does: pmm stores each free block's
    // links inside the block itself, so freeing the old PML4 while it
    // is still live in CR3 overwrites pml4[0] -- the identity map pmm
    // dereferences those links through. free_address_space() walks via
    // the physmap (PML4[256]), which the new address space shares, so
    // the old space stays reachable after the switch.
    uint64_t old_pml4_phys = p->pml4_phys;
    p->pml4_phys = new_pml4_phys;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(new_pml4_phys) : "memory");
    free_address_space(old_pml4_phys);

    cpu_default_fpu_state(current_thread()->fpu_state);

    frame->rcx = new_entry;       // user RIP the ordinary sysret epilogue will return to
    frame->user_rsp = USER_STACK_TOP;

    return 1;
}

// Walks every present user-mode page in `parent_pml4`, clears its
// PAGE_WRITABLE bit (marking it copy-on-write), shares the frame via
// pmm_frame_share(), and maps the same frame at the same virtual
// address into `child_pml4`, also read-only. Returns 0 and leaves
// child_pml4 in a to-be-discarded state on out-of-memory (caller frees
// it via free_address_space); parent_pml4's PTEs already flipped
// read-only before the failure stay that way -- harmless, since the
// next write to any of them just takes the (correctly handled,
// refcount-1, no-copy-needed) COW fault path.
static int fork_duplicate_user_pages(uint64_t *parent_pml4, uint64_t *child_pml4) {
    for (unsigned i4 = 0; i4 < 512; i4++) {
        if (i4 == 0 || i4 == 256 || i4 == 511) {
            continue; // shared kernel entries, already copied by the caller
        }
        if (!(parent_pml4[i4] & PAGE_PRESENT)) {
            continue;
        }
        uint64_t *parent_pdpt = (uint64_t *)phys_to_virt(parent_pml4[i4] & PAGE_ADDR_MASK);

        for (unsigned i3 = 0; i3 < 512; i3++) {
            if (!(parent_pdpt[i3] & PAGE_PRESENT)) {
                continue;
            }
            uint64_t *parent_pd = (uint64_t *)phys_to_virt(parent_pdpt[i3] & PAGE_ADDR_MASK);

            for (unsigned i2 = 0; i2 < 512; i2++) {
                if (!(parent_pd[i2] & PAGE_PRESENT)) {
                    continue;
                }
                uint64_t *parent_pt = (uint64_t *)phys_to_virt(parent_pd[i2] & PAGE_ADDR_MASK);

                for (unsigned i1 = 0; i1 < 512; i1++) {
                    if (!(parent_pt[i1] & PAGE_PRESENT)) {
                        continue;
                    }
                    uint64_t virt = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                     ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);

                    parent_pt[i1] &= ~PAGE_WRITABLE;
                    // The parent's TLB may still cache a stale writable
                    // translation for this page from before the PTE
                    // change -- without this invlpg, a write from the
                    // parent right after fork() could silently succeed
                    // via the stale entry instead of taking the COW
                    // fault, corrupting the frame the child now shares.
                    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");

                    uint64_t phys = parent_pt[i1] & PAGE_ADDR_MASK;
                    pmm_frame_share(phys);

                    // Low 12 bits (permission/type flags) plus bit 63
                    // (PAGE_NO_EXECUTE) -- NOT just `& 0xFFF`, which
                    // would silently drop NX and make a non-executable
                    // page executable in the child.
                    uint64_t flags = parent_pt[i1] & (0xFFFULL | PAGE_NO_EXECUTE) & ~PAGE_PRESENT;
                    if (paging_map_into(child_pml4, virt, phys, flags) != 0) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

// Duplicates the calling task into a new child, sharing physical
// frames read-only between the two (see paging_handle_cow_fault for
// the lazy-copy side). Returns the child task on success (the parent
// syscall path returns its pid), or 0 on failure -- leaving the
// parent completely unaffected (nothing is left partially modified).
struct thread *fork_task(struct syscall_frame *frame) {
    struct process *parent = current_proc();

    struct process *child_proc = proc_alloc();
    if (!child_proc) {
        serial_write_string("[process] fork FAILED: out of memory for process\n");
        return 0;
    }

    uint64_t child_pml4_phys = paging_alloc_pml4();
    uint64_t *child_pml4 = (uint64_t *)phys_to_virt(child_pml4_phys);
    uint64_t *parent_pml4 = (uint64_t *)phys_to_virt(parent->pml4_phys);
    child_pml4[0] = parent_pml4[0];
    child_pml4[256] = parent_pml4[256];
    child_pml4[511] = parent_pml4[511];

    if (!fork_duplicate_user_pages(parent_pml4, child_pml4)) {
        serial_write_string("[process] fork FAILED: out of memory duplicating page tables\n");
        free_address_space(child_pml4_phys);
        return 0;
    }

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    if (!kstack_phys) {
        serial_write_string("[process] fork FAILED: out of memory for kernel stack\n");
        free_address_space(child_pml4_phys);
        return 0;
    }
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    // Memory layout, lowest address first (i.e. pop order):
    //   r15, r14, r13, r12, rbx, rbp, fork_trampoline, rcx, r11, user_rsp
    // The first six are consumed by context_switch's own epilogue, the
    // seventh by its `ret`, and only the last three by fork_trampoline.
    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = frame->user_rsp;
    *(--sp) = frame->r11;   // user RFLAGS
    *(--sp) = frame->rcx;   // user RIP
    *(--sp) = (uint64_t)fork_trampoline; // context_switch's `ret` lands here
    *(--sp) = frame->rbp;
    *(--sp) = frame->rbx;
    *(--sp) = frame->r12;
    *(--sp) = frame->r13;
    *(--sp) = frame->r14;
    *(--sp) = frame->r15;

    child_proc->pml4_phys   = child_pml4_phys;
    child_proc->parent_pid  = parent->pid;
    child_proc->stack_slots = parent->stack_slots;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        child_proc->files[i] = parent->files[i]; // copied by value -- see docs/stdlib.md
        // The copy duplicates the vnode POINTER, so the child owes the
        // cache its own reference; without this the first close on
        // either side would free a vnode the other still holds.
        if (child_proc->files[i].in_use && child_proc->files[i].vn) {
            child_proc->files[i].vn->refcount++;
        }
    }

    // POSIX: only the CALLING thread is duplicated. The child starts
    // single-threaded no matter how many threads the parent had.
    struct thread *child = thread_alloc(child_proc);
    if (!child) {
        serial_write_string("[process] fork FAILED: out of memory for thread\n");
        pmm_free(kstack_phys, KERNEL_STACK_ORDER);
        free_address_space(child_pml4_phys);
        return 0;
    }
    child->saved_rsp = (uint64_t)sp;
    child->kernel_stack_top = kstack_top;
    child->kernel_stack_phys = kstack_phys;
    child->stack_slot = current_thread()->stack_slot;
    for (int i = 0; i < FPU_STATE_SIZE; i++) {
        child->fpu_state[i] = current_thread()->fpu_state[i];
    }

    enqueue_ready(child);
    return child;
}

// Interim: wakes every thread blocked in wait_for_pid on `pid`. The
// next task replaces this scan with a per-process wait queue.
static void wake_pid_waiters(int pid) {
    for (struct process *q = proc_list; q; q = q->next) {
        for (struct thread *t = q->threads; t; t = t->proc_next) {
            if (t->state == THREAD_BLOCKED && t->waiting_for_pid == pid) {
                t->state = THREAD_READY;
                t->waiting_for_pid = 0;
                enqueue_ready(t);
            }
        }
    }
}

void proc_get(struct process *p) { p->refcount++; }

// Drops one live-thread reference. On the last one, frees the address
// space and the file descriptors, and turns the process into a zombie
// carrying only its exit code -- the struct itself outlives its
// address space and is freed by wait_for_pid's reap.
void proc_put(struct process *p) {
    if (--p->refcount > 0) { return; }

    if (p->pml4_phys) {
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
        // same reason schedule() falls back to it for kernel-only
        // threads: kernel text (PML4[511]) and the physmap (PML4[256])
        // live there too, and nothing below runs through user mappings.
        __asm__ volatile ("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)p4_table) : "memory");
        free_address_space(p->pml4_phys);
        p->pml4_phys = 0;
    }

    // Release the process's file descriptors. With refcounted vnodes,
    // leaving these open would pin them permanently and make umount
    // report -EBUSY forever.
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (p->files[i].in_use && p->files[i].vn) {
            vnode_put(p->files[i].vn);
            p->files[i].vn = 0;
            p->files[i].in_use = 0;
        }
    }

    p->state = PROC_ZOMBIE;
    wake_pid_waiters(p->pid);
}

void thread_exit_self(int code) {
    struct thread *t = current_thread();
    struct process *p = t->proc;

    __asm__ volatile ("cli");

    t->exit_code = code;
    t->state     = THREAD_ZOMBIE;

    if (p) {
        // Unlink from the live list, then park on the zombie list. We
        // cannot free our own kernel stack -- we are running on it --
        // so thread_join or wait_for_pid's reap frees it later.
        struct thread **pp = &p->threads;
        while (*pp && *pp != t) { pp = &(*pp)->proc_next; }
        if (*pp) { *pp = t->proc_next; }
        t->proc_next = p->zombies;
        p->zombies   = t;

        proc_put(p);
    }

    __asm__ volatile ("sti");
    schedule();
    for (;;) {
        __asm__ volatile ("hlt"); // unreachable: schedule() never resumes a ZOMBIE
    }
}

void process_exit(int code) {
    struct process *p = current_proc();
    p->exiting   = 1;
    p->exit_code = code;

    serial_write_string("[process] task exited, pid=");
    serial_write_hex64((uint64_t)p->pid);
    serial_write_string(" code=");
    serial_write_hex64((uint64_t)(int64_t)code);
    serial_write_string("\n");

    thread_exit_self(code);
}

int64_t wait_for_pid(int pid) {
    struct process *p = proc_find(pid);
    if (!p) { return -1; }

    while (p->state != PROC_ZOMBIE) {
        current_thread()->state = THREAD_BLOCKED;
        current_thread()->waiting_for_pid = pid;
        schedule();
    }

    int code = p->exit_code;

    // Free every zombie thread's kernel stack and struct, then the
    // process itself. Safe here: none of them is running.
    struct thread *z = p->zombies;
    while (z) {
        struct thread *next = z->proc_next;
        pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);
        kfree(z);
        z = next;
    }
    p->zombies = 0;

    uint64_t f = spin_lock_irqsave(&proc_lock);
    struct process **pp = &proc_list;
    while (*pp && *pp != p) { pp = &(*pp)->next; }
    if (*pp) { *pp = p->next; }
    spin_unlock_irqrestore(&proc_lock, f);

    kfree(p);
    return code;
}
