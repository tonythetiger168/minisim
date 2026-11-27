// trap.c - exceptions: SVC from EL1 and EL0, an undefined instruction, a
// data abort on an unmapped address, an alignment fault, a trapped system
// register read from EL0, HVC to the firmware (PSCI_VERSION) and the
// exception return state (ELR, SPSR).
#include "rt.h"
static volatile int last_kind, last_ec, count;
static volatile uint64_t last_far, last_iss;
void secondary(int cpu) { (void)cpu; }
void exit(int code) { exit_(code); }

void trap(int kind, struct frame* f) {
    uint64_t esr = SYSREG_READ(esr_el1);
    last_kind = kind;
    last_ec = (int)(esr >> 26);
    last_iss = esr & 0x1ffffff;
    last_far = SYSREG_READ(far_el1);
    count++;
    if (last_ec == 0x15 && (f->spsr & 0xf) == 0 && (esr & 0xffff) == 0x42) {
        // SVC #0x42 from EL0: back to EL1 at the address in x0
        f->elr = f->x[0];
        f->spsr = 0x3c5;   // EL1h, interrupts masked
        return;
    }
    if (last_ec == 0x15 || last_ec == 0x16) return;   // ELR is already the next instruction
    f->elr += 4;                                      // skip the faulting instruction
}

extern void el0_code(void);
extern void back_in_el1(void);
__asm__(
    ".global el0_code\n el0_code:\n"
    "  mrs x1, sctlr_el1\n"            // EL1 register from EL0: undefined (EC 0)
    "  adr x0, back_in_el1\n"
    "  svc #0x42\n"
    "  b .\n"
    ".global back_in_el1\n back_in_el1:\n"
    "  ret\n");

int main(int cpu) {
    (void)cpu;
    __asm__ volatile("svc #7");
    check("svc ec", last_ec == 0x15 && (last_iss & 0xffff) == 7 && last_kind == 4);

    __asm__ volatile(".inst 0x00000000");  // UDF #0
    check("udf ec", last_ec == 0 && last_kind == 4);

    volatile uint32_t* bad = (volatile uint32_t*)0x20000000ull;
    uint32_t v = *bad;
    (void)v;
    check("dabt ec", last_ec == 0x25 && last_far == 0x20000000ull);

    // alignment: an exclusive load at an odd address always checks it
    uint64_t dummy[2];
    uintptr_t odd = (uintptr_t)dummy + 1;
    uint64_t t;
    __asm__ volatile("ldxr %0, [%1]" : "=r"(t) : "r"(odd) : "memory");
    check("align ec", last_ec == 0x25 && (last_iss & 0x3f) == 0x21 && last_far == odd);

    int64_t ver = psci(0x84000000u, 0, 0, 0);
    check("psci version", ver == 0x10001);
    check("psci unknown", psci(0x8400ff00u, 0, 0, 0) == -1);

    // to EL0 and back: SPSR = EL0t, ELR = el0_code; the EL0 code returns
    // through SVC #0x42 and the trap handler resumes back_in_el1, whose RET
    // returns from the asm block below via x30
    int before = count;
    __asm__ volatile(
        "msr spsr_el1, xzr\n"
        "msr elr_el1, %0\n"
        "adr x30, 1f\n"
        "eret\n"
        "1:\n" :: "r"(el0_code) : "x0", "x1", "x30", "memory");
    check("el0 traps", count == before + 2);
    check("el0 svc", last_ec == 0x15);
    check("current el", (SYSREG_READ(CurrentEL) >> 2) == 1);

    uint64_t daif = SYSREG_READ(daif);
    check("daif", daif == 0x3c0);
    if (!fails) puts_("trap: all passed\n");
    return fails;
}
