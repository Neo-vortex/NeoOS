#include "lock.h"
#include "cpu_local.h"
#include "serial.h"

static void lock_panic(const char *msg, const char *a, const char *b) {
    __asm__ volatile ("cli");
    serial_write_string("[lock] PANIC: ");
    serial_write_string(msg);
    serial_write_string(" acquiring=");
    serial_write_string(a ? a : "(null)");
    serial_write_string(" holding=");
    serial_write_string(b ? b : "(none)");
    serial_write_string("\n");
    for (;;) { __asm__ volatile ("hlt"); }
}

void spin_init(struct spinlock *l, uint8_t rank, const char *name) {
    l->locked = 0;
    l->rank   = rank;
    l->name   = name;
}

int lock_held_depth(void) { return this_cpu()->held_depth; }

int lock_rank_ok(uint8_t rank) {
    struct cpu *c = this_cpu();
    if (c->held_depth == 0) { return 1; }
    return rank > c->held_ranks[c->held_depth - 1];
}

uint64_t spin_lock_irqsave(struct spinlock *l) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    if (!lock_rank_ok(l->rank)) {
        lock_panic("rank inversion", l->name, "see previous acquire");
    }
    struct cpu *c = this_cpu();
    if (c->held_depth >= LOCK_MAX_HELD) {
        lock_panic("held-lock stack overflow", l->name, 0);
    }

    // Uncontended on one CPU, but a real atomic so the SMP milestone
    // changes nothing here.
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }

    c->held_ranks[c->held_depth++] = l->rank;
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags) {
    struct cpu *c = this_cpu();
    if (c->held_depth <= 0) {
        lock_panic("unlock with nothing held", l->name, 0);
    }
    c->held_depth--;
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
    if (flags & (1ULL << 9)) {
        __asm__ volatile ("sti");
    }
}

void lock_selftest(void) {
    struct spinlock outer, inner;
    spin_init(&outer, LOCK_RANK_PROCESS, "selftest-outer");
    spin_init(&inner, LOCK_RANK_RUNQUEUE, "selftest-inner");

    if (lock_held_depth() != 0) {
        serial_write_string("[lock] selftest FAILED: depth not 0 at entry\n");
        return;
    }

    uint64_t f1 = spin_lock_irqsave(&outer);
    if (lock_held_depth() != 1) {
        serial_write_string("[lock] selftest FAILED: depth after one acquire\n");
        return;
    }
    // Ascending is legal, descending is not -- checked WITHOUT
    // acquiring, so the panic path is never entered.
    if (!lock_rank_ok(LOCK_RANK_RUNQUEUE)) {
        serial_write_string("[lock] selftest FAILED: ascending rank rejected\n");
        return;
    }
    if (lock_rank_ok(LOCK_RANK_PROCTABLE)) {
        serial_write_string("[lock] selftest FAILED: inversion not detected\n");
        return;
    }
    if (lock_rank_ok(LOCK_RANK_PROCESS)) {
        serial_write_string("[lock] selftest FAILED: equal rank accepted\n");
        return;
    }

    uint64_t f2 = spin_lock_irqsave(&inner);
    if (lock_held_depth() != 2) {
        serial_write_string("[lock] selftest FAILED: depth after two acquires\n");
        return;
    }
    spin_unlock_irqrestore(&inner, f2);
    spin_unlock_irqrestore(&outer, f1);

    if (lock_held_depth() != 0) {
        serial_write_string("[lock] selftest FAILED: depth not 0 at exit\n");
        return;
    }
    if (outer.locked != 0 || inner.locked != 0) {
        serial_write_string("[lock] selftest FAILED: lock still held\n");
        return;
    }
    serial_write_string("[lock] selftest passed\n");
}
