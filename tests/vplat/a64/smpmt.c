// smpmt.c - four processors (run with -harts 4, also with -mt, where they
// run at once on host threads): each adds to shared counters with
// LDXR/STXR, LDADD, a CAS loop, CASP on a pair and under a spin lock, many
// times, so that lost updates would show.
#include "rt.h"
void exit(int code) { exit_(code); }
void trap(int kind, struct frame* f) { (void)f; puts_("trap "); putd(kind); puts_(" esr "); putx(SYSREG_READ(esr_el1)); putc_('\n'); exit_(9); }
extern char _secondary[];
#define N 20000
// in .data: processor 0 clears .bss after the others may have started counting
static volatile uint64_t ll_count = 1, lse_count = 1, cas_count = 1, locked_count = 1, lock = 0, done_mask = 1;
static volatile uint64_t pair[2] __attribute__((aligned(16))) = {1, 1};

static void ll_add(volatile uint64_t* p, uint64_t v) {
    uint64_t t, fail;
    __asm__ volatile("1: ldxr %0, [%2]\n add %0, %0, %3\n stxr %w1, %0, [%2]\n cbnz %w1, 1b"
                     : "=&r"(t), "=&r"(fail) : "r"(p), "r"(v) : "memory");
}
static void cas_add(volatile uint64_t* p) {
    uint64_t old = *p, seen;
    for (;;) {
        seen = old;
        __asm__ volatile("casal %0, %2, [%1]" : "+r"(seen) : "r"(p), "r"(old + 1) : "memory");
        if (seen == old) return;
        old = seen;
    }
}
static void casp_add(void) {
    for (;;) {
        register uint64_t s0 __asm__("x4") = pair[0], s1 __asm__("x5") = pair[1];
        const uint64_t o0 = s0, o1 = s1;
        register uint64_t t0 __asm__("x6") = o0 + 1, t1 __asm__("x7") = o1 + 2;
        __asm__ volatile("caspal x4, x5, x6, x7, [%4]" : "+r"(s0), "+r"(s1), "+r"(t0), "+r"(t1) : "r"(pair) : "memory");
        if (s0 == o0 && s1 == o1) return;
    }
}
static void spin_lock(volatile uint64_t* l) {
    uint64_t t, fail;
    __asm__ volatile("sevl\n1: wfe\n2: ldaxr %0, [%2]\n cbnz %0, 1b\n stxr %w1, %3, [%2]\n cbnz %w1, 2b"
                     : "=&r"(t), "=&r"(fail) : "r"(l), "r"(1ull) : "memory");
}
static void spin_unlock(volatile uint64_t* l) { __asm__ volatile("stlr xzr, [%0]\n sev" :: "r"(l) : "memory"); }

static void work(int cpu) {
    for (int i = 0; i < N; i++) {
        ll_add(&ll_count, 1);
        __asm__ volatile("ldaddal %1, xzr, [%0]" :: "r"(&lse_count), "r"(1ull) : "memory");
        cas_add(&cas_count);
        casp_add();
        spin_lock(&lock);
        locked_count = locked_count + 1;
        spin_unlock(&lock);
    }
    __asm__ volatile("ldsetal %1, xzr, [%0]" :: "r"(&done_mask), "r"(2ull << cpu) : "memory");
}

void secondary(int cpu) {
    work(cpu);
    psci(0x84000002u, 0, 0, 0);
    exit_(8);
}

int main(int cpu) {
    for (int i = 1; i < 4; i++) check("cpu_on", psci(0xc4000003u, (uint64_t)i, (uint64_t)_secondary, (uint64_t)i) == 0);
    work(cpu);
    while (done_mask != 31) __asm__ volatile("yield");
    check("LDXR/STXR count", ll_count == 1 + 4 * N);
    check("LDADD count", lse_count == 1 + 4 * N);
    check("CAS count", cas_count == 1 + 4 * N);
    check("CASP pair", pair[0] == 1 + 4 * N && pair[1] == 1 + 8 * N);
    check("locked count", locked_count == 1 + 4 * N);
    if (!fails) puts_("smpmt: all passed\n");
    return fails;
}
