// irq.c - the GICv3 and the generic timer: the distributor and the
// redistributor brought up, the virtual (PPI 27) and the physical (PPI 30)
// timer interrupts taken through WFI, a software-generated interrupt to
// itself, priority masking with ICC_PMR_EL1, and a pending SPI.
#include "rt.h"
void secondary(int cpu) { (void)cpu; }
void exit(int code) { exit_(code); }
static volatile int got[64], order[8], norder;

void trap(int kind, struct frame* f) {
    (void)f;
    if (kind != 5) { puts_("unexpected trap "); putd(kind); puts_(" esr "); putx(SYSREG_READ(esr_el1)); putc_('\n'); exit_(9); }
    uint64_t id = SYSREG_READ(S3_0_C12_C12_0);   // ICC_IAR1_EL1
    if (id < 64) got[id]++;
    if (norder < 8) order[norder++] = (int)id;
    if (id == 27) SYSREG_WRITE(cntv_ctl_el0, 0);
    if (id == 30) SYSREG_WRITE(cntp_ctl_el0, 0);
    if (id == 40) REG32(GICD + 0x280 + 4) = 1u << 8;          // ICPENDR1: SPI 40
    SYSREG_WRITE(S3_0_C12_C12_1, id);                         // ICC_EOIR1_EL1
}

int main(int cpu) {
    (void)cpu;
    REG32(GICD) = 0x12;                                       // ARE, EnableGrp1
    REG32(GICR + 0x14) &= ~2u;                                // WAKER.ProcessorSleep
    while (REG32(GICR + 0x14) & 4) {}
    const uint32_t sgi = GICR + 0x10000;
    REG32(sgi + 0x80) = ~0u;                                  // IGROUPR0: group 1
    REG32(sgi + 0x100) = 1u << 27 | 1u << 30 | 1u << 3;       // ISENABLER0
    for (int i = 0; i < 8; i++) REG32(sgi + 0x400 + 4 * i) = 0x80808080u;
    REG32(GICD + 0x84) = ~0u;                                 // IGROUPR1
    REG32(GICD + 0x400 + 40) = 0x40404040u;                   // IPRIORITYR: SPI 40 at 0x40
    REG64(GICD + 0x6000 + 8 * 40) = 0;                        // IROUTER: cpu 0
    REG32(GICD + 0x104) = 1u << 8;                            // ISENABLER1: SPI 40
    SYSREG_WRITE(S3_0_C12_C12_5, 7);                          // ICC_SRE_EL1
    SYSREG_WRITE(S3_0_C4_C6_0, 0xff);                         // ICC_PMR_EL1
    SYSREG_WRITE(S3_0_C12_C12_7, 1);                          // ICC_IGRPEN1_EL1
    __asm__ volatile("isb");

    uint64_t frq = SYSREG_READ(cntfrq_el0);
    check("cntfrq", frq != 0);
    uint64_t t0 = SYSREG_READ(cntvct_el0);
    SYSREG_WRITE(cntv_cval_el0, t0 + frq / 1000);             // 1 ms
    SYSREG_WRITE(cntv_ctl_el0, 1);
    __asm__ volatile("msr daifclr, #2; isb");
    while (!got[27]) __asm__ volatile("wfi");
    uint64_t t1 = SYSREG_READ(cntvct_el0);
    check("vtimer", got[27] == 1 && t1 - t0 >= frq / 1000);

    SYSREG_WRITE(cntp_tval_el0, frq / 2000);
    SYSREG_WRITE(cntp_ctl_el0, 1);
    while (!got[30]) __asm__ volatile("wfi");
    check("ptimer", got[30] == 1);

    SYSREG_WRITE(S3_0_C12_C11_5, 1ull | 3ull << 24);          // ICC_SGI1R_EL1: SGI 3 to cpu 0
    __asm__ volatile("isb");
    for (int i = 0; i < 100 && !got[3]; i++) {}
    check("sgi", got[3] == 1);

    // masked by priority: PMR 0x80 lets 0x40 through but not 0x80
    SYSREG_WRITE(S3_0_C4_C6_0, 0x80);
    SYSREG_WRITE(cntv_tval_el0, 10);
    SYSREG_WRITE(cntv_ctl_el0, 1);
    REG32(GICD + 0x204) = 1u << 8;                            // ISPENDR1: SPI 40
    for (volatile int i = 0; i < 20000; i++) {}
    check("pmr spi", got[40] == 1);
    check("pmr masks ppi", got[27] == 1);
    uint64_t ctl = SYSREG_READ(cntv_ctl_el0);
    check("istatus", (ctl & 4) != 0);
    SYSREG_WRITE(S3_0_C4_C6_0, 0xff);
    __asm__ volatile("isb");
    for (volatile int i = 0; i < 100 && got[27] < 2; i++) {}
    check("unmasked ppi", got[27] == 2);
    check("order", order[0] == 27 && order[1] == 30 && order[2] == 3 && order[3] == 40 && order[4] == 27);
    if (!fails) puts_("irq: all passed\n");
    return fails;
}
