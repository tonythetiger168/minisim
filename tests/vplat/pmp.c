// pmp.c - physical memory protection: user mode with no entries, NAPOT,
// NA4 and TOR regions, a load that only partly matches an entry, MPRV, and a
// locked entry that binds machine mode and can no longer be changed.
#include "rt.h"

static volatile uint32_t secret[1024] __attribute__((aligned(4096)));  // entry 0: no access
static volatile uint32_t ro[4] __attribute__((aligned(16)));          // entry 1: read only
static volatile uint32_t tor[64] __attribute__((aligned(64)));        // entries 2-3: execute only
static volatile uint32_t sealed[1024] __attribute__((aligned(4096))); // entry 4: locked, read only

static volatile uint32_t lastCause, nTraps;
static void (*volatile resume)(void);  // where a user-mode ecall or fetch fault comes back to
static int fails;

static void check(const char* what, uint32_t got, uint32_t want) {
    puts_(what); putc_(' ');
    if (got == want) puts_("ok\n");
    else { puts_("got "); putd(got); puts_(" want "); putd(want); putc_('\n'); fails++; }
}

__attribute__((interrupt("machine"), aligned(4))) void handler(void) {
    uint32_t cause = csr_read(mcause), epc = csr_read(mepc);
    lastCause = cause;
    nTraps++;
    if (cause == 8 || cause == 1) {  // back to machine mode, at `resume`
        csr_write(mepc, (uint32_t)(uintptr_t)resume);
        csr_set(mstatus, 3u << 11);
        return;
    }
    // Skip the faulting instruction (2 or 4 bytes).
    uint16_t lo = *(volatile uint16_t*)(uintptr_t)epc;
    csr_write(mepc, epc + ((lo & 3) == 3 ? 4 : 2));
}

__attribute__((noreturn)) static void user(void (*fn)(void), void (*next)(void)) {
    resume = next;
    csr_write(mepc, (uint32_t)(uintptr_t)fn);
    __asm__ volatile("csrc mstatus, %0" ::"r"(3u << 11));  // MPP = U
    __asm__ volatile("mret");
    __builtin_unreachable();
}

static uint32_t napot(const volatile void* p, uint32_t size) {
    return ((uint32_t)(uintptr_t)p >> 2) | ((size >> 3) - 1);
}
#define CFG(i, v) ((uint32_t)(v) << (8 * (i)))
enum { R = 1, W = 2, X = 4, TOR = 0x08, NA4 = 0x10, NAPOT = 0x18, L = 0x80 };

static uint32_t uLoad, uStore, uLoadRo, uStoreRo, uTor, uPartial;
static void u_ecall(void) { __asm__ volatile("ecall"); }

// User mode: each access either works or traps and is skipped; the cause
// that came back is recorded after each.
static void u_tests(void) {
    lastCause = 0; (void)secret[3];            uLoad = lastCause;
    lastCause = 0; secret[5] = 1;              uStore = lastCause;
    lastCause = 0; (void)ro[0];                uLoadRo = lastCause;
    lastCause = 0; ro[0] = 2;                  uStoreRo = lastCause;
    lastCause = 0; (void)tor[10];              uTor = lastCause;
    lastCause = 0;
    { uint32_t v; __asm__ volatile("lw %0, -2(%1)" : "=r"(v) : "r"((uintptr_t)secret)); (void)v; }
    uPartial = lastCause;
    u_ecall();
}

static void stage_lock(void);
static void stage_after_user(void) {
    check("user load from a no-access region: cause", uLoad, 5);
    check("user store to a no-access region: cause", uStore, 7);
    check("user load from a read-only word: cause", uLoadRo, 0);
    check("user store to a read-only word: cause", uStoreRo, 7);
    check("user load from an execute-only TOR range: cause", uTor, 5);
    check("user load half inside a region: cause", uPartial, 5);

    // MPRV: machine-mode loads checked as user mode.
    csr_set(mstatus, 1u << 17);  // MPRV, with MPP = U
    lastCause = 0; (void)secret[1];
    uint32_t c = lastCause;
    __asm__ volatile("csrc mstatus, %0" ::"r"(1u << 17));
    check("machine load with MPRV and MPP=U: cause", c, 5);
    lastCause = 0; (void)secret[1];
    check("the same load without MPRV: cause", lastCause, 0);
    stage_lock();
}

static void stage_lock(void) {
    // Unlocked entries do not bind machine mode; a locked one does.
    lastCause = 0; sealed[0] = 1;
    check("machine store before the lock: cause", lastCause, 0);
    csr_write(pmpaddr4, napot(sealed, 4096));
    csr_write(pmpcfg1, CFG(0, NAPOT | R | L) | CFG(1, NAPOT | R | W | X));
    lastCause = 0; sealed[0] = 2;
    check("machine store under the lock: cause", lastCause, 7);
    check("machine load under the lock", sealed[0], 1);
    csr_write(pmpcfg1, CFG(0, NAPOT | R | W | X) | CFG(1, NAPOT | R | W | X));
    check("a locked entry keeps its configuration", csr_read(pmpcfg1) & 0xff, NAPOT | R | L);
    csr_write(pmpaddr4, 0);
    check("a locked entry keeps its address", csr_read(pmpaddr4), napot(sealed, 4096));
    check("W without R reads back as no access", (csr_read(pmpcfg0) >> 8 * 2) & 0xff, 0);
    if (fails) { puts_("pmp: FAILED\n"); exit_(1); }
    puts_("pmp: all checks passed\n");
    exit_(0);
}

static void stage_setup(void) {
    check("user fetch with no PMP entry: cause", lastCause, 1);
    csr_write(pmpaddr0, napot(secret, 4096));
    csr_write(pmpaddr1, (uint32_t)(uintptr_t)ro >> 2);
    csr_write(pmpaddr2, (uint32_t)(uintptr_t)tor >> 2);
    csr_write(pmpaddr3, ((uint32_t)(uintptr_t)tor + sizeof tor) >> 2);
    csr_write(pmpaddr5, 0xffffffffu);  // NAPOT over everything
    // Entry 2 is off (it is only entry 3's lower bound); W alone is reserved.
    csr_write(pmpcfg0, CFG(0, NAPOT) | CFG(1, NA4 | R) | CFG(2, W) | CFG(3, TOR | X));
    csr_write(pmpcfg1, CFG(1, NAPOT | R | W | X));
    user(u_tests, stage_after_user);
}

void exit(int code) { exit_(code); }

int main(void) {
    csr_write(mtvec, (uint32_t)(uintptr_t)handler);
    // No PMP entry is active: user mode cannot even fetch.
    user(u_ecall, stage_setup);
}
