// mmu.c - stage 1 translation with the 4 KiB granule: identity blocks for
// the devices and RAM through TTBR0, 4 KiB pages in the upper range through
// TTBR1, a permission fault on a read-only page, a translation fault on an
// invalid one, AT S1E1R into PAR_EL1, and TLBI after a table change.
#include "rt.h"
void secondary(int cpu) { (void)cpu; }
void exit(int code) { exit_(code); }
static volatile uint64_t esr, far;
static volatile int traps;
void trap(int kind, struct frame* f) {
    (void)kind;
    esr = SYSREG_READ(esr_el1);
    far = SYSREG_READ(far_el1);
    traps++;
    f->elr += 4;
}

extern uint64_t __pt[];
static uint64_t data[512] __attribute__((aligned(4096)));
#define AF (1ull << 10)
#define SH (3ull << 8)
#define NORMAL (1ull << 2)
#define RO (2ull << 6)
#define HI 0xffffff8000000000ull

int main(int cpu) {
    (void)cpu;
    uint64_t* l1lo = __pt;        // TTBR0: 39-bit, level 1
    uint64_t* l1hi = __pt + 512;  // TTBR1
    uint64_t* l2hi = __pt + 1024;
    uint64_t* l3hi = __pt + 1536;
    for (int i = 0; i < 2048; i++) __pt[i] = 0;
    l1lo[0] = 0x00000000ull | AF | 1;               // devices, 1 GiB block, attr 0
    l1lo[1] = 0x40000000ull | AF | SH | NORMAL | 1; // RAM
    l1hi[0] = (uint64_t)l2hi | 3;
    l2hi[0] = (uint64_t)l3hi | 3;
    l3hi[0] = (uint64_t)data | AF | SH | NORMAL | 3;
    l3hi[1] = (uint64_t)data | AF | SH | NORMAL | RO | 3;
    l3hi[2] = 0;
    SYSREG_WRITE(mair_el1, 0xff00ull);              // attr 0 device nGnRnE, attr 1 normal WB
    SYSREG_WRITE(tcr_el1, 25ull | 1ull << 8 | 1ull << 10 | 3ull << 12 | 25ull << 16 | 1ull << 24 | 1ull << 26 | 3ull << 28 | 2ull << 30 | 2ull << 32);
    SYSREG_WRITE(ttbr0_el1, (uint64_t)l1lo);
    SYSREG_WRITE(ttbr1_el1, (uint64_t)l1hi);
    __asm__ volatile("dsb sy; isb");
    SYSREG_WRITE(sctlr_el1, SYSREG_READ(sctlr_el1) | 1 | 1 << 2 | 1 << 12);
    __asm__ volatile("isb");
    puts_("mmu on\n");

    volatile uint64_t* hi = (volatile uint64_t*)HI;
    hi[3] = 0x1234;
    check("alias write", data[3] == 0x1234);
    data[5] = 0x5678;
    check("alias read", hi[5] == 0x5678);
    volatile uint64_t* ro = (volatile uint64_t*)(HI + 4096);
    check("ro read", ro[5] == 0x5678);
    ro[6] = 1;
    check("perm fault", traps == 1 && (esr >> 26) == 0x25 && (esr & 0x3f) == 0x0f && (esr >> 6 & 1) && far == HI + 4096 + 48);
    volatile uint64_t* inv = (volatile uint64_t*)(HI + 8192);
    uint64_t x = inv[0];
    (void)x;
    check("trans fault", traps == 2 && (esr & 0x3f) == 0x07 && far == HI + 8192);

    __asm__ volatile("at s1e1r, %0; isb" :: "r"(HI + 4096 + 0x18));
    uint64_t par = SYSREG_READ(par_el1);
    check("at mapped", !(par & 1) && (par & 0xfffffffff000ull) == (uint64_t)data);
    __asm__ volatile("at s1e1r, %0; isb" :: "r"(HI + 8192));
    par = SYSREG_READ(par_el1);
    check("at unmapped", (par & 1) && ((par >> 1) & 0x3f) == 0x07);

    l3hi[2] = (uint64_t)data | AF | SH | NORMAL | 3;
    __asm__ volatile("dsb ishst; tlbi vae1is, %0; dsb ish; isb" :: "r"((HI + 8192) >> 12));
    check("after tlbi", inv[5] == 0x5678 && traps == 2);
    // change a mapping that is in the TLB: without TLBI the old one may stay,
    // after TLBI VMALLE1 the new one must be used
    l3hi[0] = 0;
    __asm__ volatile("dsb ish; tlbi vmalle1; dsb ish; isb");
    x = hi[0];
    check("after vmalle1", traps == 3 && (esr & 0x3f) == 0x07);

    if (!fails) puts_("mmu: all passed\n");
    return fails;
}
