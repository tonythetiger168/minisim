// smp.c - several harts on shared memory: an atomic counter (AMOADD), a
// spinlock (LR/SC) around a plain counter, and a flag-and-data handoff with
// fences. Hart 0 prints the totals; with N harts both counters must be N*ITER.
#include "rt.h"

#define ITER 2000
#define MAXH 64

static volatile uint32_t atomic_count, locked_count, lock, done, go;
static volatile uint32_t per_hart[MAXH];
static volatile uint32_t mailbox;

void exit(int code) {
    if (csr_read(mhartid) == 0) exit_(code);
    for (;;) __asm__ volatile("wfi");
}

static void acquire(volatile uint32_t* l) {
    uint32_t t;
    do {
        __asm__ volatile("1: lr.w %0, (%1)\n bnez %0, 1b\n sc.w %0, %2, (%1)" : "=&r"(t) : "r"(l), "r"(1) : "memory");
    } while (t);
}
static void release(volatile uint32_t* l) { __asm__ volatile("fence rw, w\n sw zero, 0(%0)" ::"r"(l) : "memory"); }

static uint32_t nharts(void) {
    // Harts count themselves in at start.
    return __atomic_load_n(&go, __ATOMIC_ACQUIRE);
}

int main(int hart) {
    __atomic_fetch_add(&go, 1, __ATOMIC_ACQ_REL);
    if (hart == 0) {
        mailbox = 0x1234;
        __asm__ volatile("fence" ::: "memory");
    }
    // Let every hart arrive (each hart is started at the same time).
    for (volatile int i = 0; i < 2000; i++) {}
    uint32_t n = nharts();
    for (int i = 0; i < ITER; i++) {
        __atomic_fetch_add(&atomic_count, 1, __ATOMIC_RELAXED);
        acquire(&lock);
        locked_count = locked_count + 1;
        per_hart[hart]++;
        release(&lock);
    }
    uint32_t seen = mailbox;
    __atomic_fetch_add(&done, 1, __ATOMIC_ACQ_REL);
    if (hart != 0) return seen == 0x1234 ? 0 : 1;
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != n) {}
    puts_("harts ");
    putd(n);
    puts_(" atomic ");
    putd(atomic_count);
    puts_(" locked ");
    putd(locked_count);
    puts_("\n");
    for (uint32_t h = 0; h < n; h++) {
        puts_("  hart ");
        putd(h);
        puts_(" ");
        putd(per_hart[h]);
        puts_("\n");
    }
    return atomic_count == n * ITER && locked_count == n * ITER ? 0 : 1;
}
