// smp.c - four processors (run with -harts 4): PSCI CPU_ON starts 1 to 3 at
// _secondary, each adds to a counter with LDXR/STXR and to another under a
// spin lock (LDAXR/STLR, WFE/SEV), then calls CPU_OFF; AFFINITY_INFO
// reports them off.
#include "rt.h"
void exit(int code) { exit_(code); }
void trap(int kind, struct frame* f) { (void)f; puts_("trap "); putd(kind); puts_(" esr "); putx(SYSREG_READ(esr_el1)); putc_('\n'); exit_(9); }
extern char _secondary[];
#define N 3000
static volatile uint64_t atomic_count, locked_count, lock, done_mask;

static void atomic_add(volatile uint64_t* p, uint64_t v) {
    uint64_t t, fail;
    __asm__ volatile("1: ldxr %0, [%2]\n add %0, %0, %3\n stxr %w1, %0, [%2]\n cbnz %w1, 1b"
                     : "=&r"(t), "=&r"(fail) : "r"(p), "r"(v) : "memory");
}
static void spin_lock(volatile uint64_t* l) {
    uint64_t t, fail;
    __asm__ volatile("sevl\n1: wfe\n2: ldaxr %0, [%2]\n cbnz %0, 1b\n stxr %w1, %3, [%2]\n cbnz %w1, 2b"
                     : "=&r"(t), "=&r"(fail) : "r"(l), "r"(1ull) : "memory");
}
static void spin_unlock(volatile uint64_t* l) { __asm__ volatile("stlr xzr, [%0]\n sev" :: "r"(l) : "memory"); }

static void work(int cpu) {
    for (int i = 0; i < N; i++) {
        atomic_add(&atomic_count, 1);
        spin_lock(&lock);
        locked_count = locked_count + 1;
        spin_unlock(&lock);
    }
    atomic_add(&done_mask, 1ull << cpu);
}

void secondary(int cpu) {
    check("mpidr", (SYSREG_READ(mpidr_el1) & 0xff) == (uint64_t)cpu);
    work(cpu);
    psci(0x84000002u, 0, 0, 0);    // CPU_OFF: does not return
    puts_("FAIL cpu_off returned\n");
    exit_(8);
}

int main(int cpu) {
    for (int i = 1; i < 4; i++) {
        int64_t r = psci(0xc4000003u, (uint64_t)i, (uint64_t)_secondary, (uint64_t)i);
        check("cpu_on", r == 0);
    }
    check("cpu_on again", psci(0xc4000003u, 1, (uint64_t)_secondary, 1) == -4);   // ALREADY_ON
    work(cpu);
    while (done_mask != 15) __asm__ volatile("yield");
    for (int i = 1; i < 4; i++) {
        int64_t s;
        for (int k = 0; k < 1000 && (s = psci(0xc4000004u, (uint64_t)i, 0, 0)) != 1; k++) {}
        check("affinity off", s == 1);
    }
    check("atomic count", atomic_count == 4 * N);
    check("locked count", locked_count == 4 * N);
    if (!fails) puts_("smp: all passed\n");
    return fails;
}
