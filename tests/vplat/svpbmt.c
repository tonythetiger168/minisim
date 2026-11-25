// svpbmt.c - Svpbmt (page-based memory types) and Zalasr (load-acquire,
// store-release) on an RV64 hart. Svpbmt: loads through Sv39 pages whose
// PTEs carry PBMT 0 to 3, with menvcfg.PBMTE off and on, and a PBMT on a
// pointer PTE; the loads translate as supervisor through mstatus.MPRV, so a
// page fault comes back to the machine-mode handler, which skips the load.
// Zalasr: every width of LB..LD.AQ and SB..SD.RL (and the .AQRL forms), sign
// extension, misaligned addresses and the reserved encodings.
#include "rt.h"

void exit(int code) { exit_(code); }

static int fails, checks;
static void check(const char* what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        puts_("FAIL "); puts_(what); puts_(": got "); putx((uint32_t)(got >> 32)); putx((uint32_t)got);
        puts_(" want "); putx((uint32_t)(want >> 32)); putx((uint32_t)want); puts_("\n");
        fails++;
    }
}

// The trap handler: record mcause, step over the 4-byte instruction.
volatile uint64_t lastCause, lastTval;
__attribute__((naked, aligned(4))) void handler(void) {
    __asm__ volatile(
        "csrrw sp, mscratch, sp\n"
        "sd t0, 0(sp)\n"
        "sd t1, 8(sp)\n"
        "csrr t0, mcause\n"
        "la t1, lastCause\n"
        "sd t0, 0(t1)\n"
        "csrr t0, mtval\n"
        "la t1, lastTval\n"
        "sd t0, 0(t1)\n"
        "ld t1, 8(sp)\n"
        "csrr t0, mepc\n"
        "addi t0, t0, 4\n"
        "csrw mepc, t0\n"
        "ld t0, 0(sp)\n"
        "csrrw sp, mscratch, sp\n"
        "mret\n");
}
static uint64_t trapStack[64] __attribute__((aligned(16)));

// Sv39 tables: root[2] maps 0x80000000 (1 GiB, identity, code and data);
// root[1] maps VA 0x40000000 to the same memory with the PBMT under test;
// root[3] points to l1, which maps VA 0xc0000000 to it with 2 MiB pages.
static uint64_t root[512] __attribute__((aligned(4096)));
static uint64_t l1[512] __attribute__((aligned(4096)));
// root[4] -> l1b -> l0: VA 0x100000000 is one 4 KiB page, the next one is
// not mapped (an access across the two faults on its second part).
static uint64_t l1b[512] __attribute__((aligned(4096)));
static uint64_t l0[512] __attribute__((aligned(4096)));
static uint8_t page[4096] __attribute__((aligned(4096)));
static volatile uint64_t target = 0x1122334455667788ull;

#define PTE_V 1
#define PTE_RWXAD (2 | 4 | 8 | 0x40 | 0x80)
static uint64_t leaf(uint64_t pa, unsigned pbmt) { return (pa >> 12) << 10 | (uint64_t)pbmt << 61 | PTE_RWXAD | PTE_V; }

// A load as supervisor (MPRV, MPP = S; set each time, as the handler's MRET
// leaves MPP at U): the value, and the trap cause or 0.
static uint64_t sload(uint64_t va, uint64_t* cause) {
    uint64_t v = 0;
    lastCause = 0;
    __asm__ volatile("sfence.vma\n"
                     "csrs mstatus, %2\n"
                     "ld t1, 0(%1)\n"  // 4 bytes: the handler steps over it
                     "mv %0, t1\n"
                     "csrc mstatus, %2\n"
                     : "+r"(v)
                     : "r"(va), "r"(1ull << 17 | 1ull << 11)
                     : "t1", "memory");
    *cause = lastCause;
    return lastCause ? 0 : v;
}

static void svpbmt(void) {
    csr_write(pmpaddr0, ~0ull >> 10);  // PMP: everything to S and U (NAPOT, RWX)
    csr_write(pmpcfg0, 0x1f);
    csr_set(mstatus, 1ull << 11);  // MPP = S
    __asm__ volatile("csrc mstatus, %0" ::"r"(1ull << 12));
    root[2] = leaf(0x80000000, 0);
    for (unsigned i = 0; i < 512; i++) l1[i] = leaf(0x80000000 + ((uint64_t)i << 21), 0);
    root[3] = ((uint64_t)(uintptr_t)l1 >> 12) << 10 | PTE_V;
    csr_write(satp, (8ull << 60) | ((uintptr_t)root >> 12));
    const uint64_t off = (uintptr_t)&target - 0x80000000;
    uint64_t c;

    check("menvcfg.PBMTE starts clear", csr_read(0x30a) >> 62 & 1, 0);
    root[1] = leaf(0x80000000, 0);
    check("PBMT=PMA, PBMTE=0: loads", sload(0x40000000 + off, &c), target);
    check("  no trap", c, 0);
    root[1] = leaf(0x80000000, 1);
    sload(0x40000000 + off, &c);
    check("PBMT=NC, PBMTE=0: load page fault", c, 13);
    root[1] = leaf(0x80000000, 2);
    sload(0x40000000 + off, &c);
    check("PBMT=IO, PBMTE=0: load page fault", c, 13);

    csr_set(0x30a, 1ull << 62);
    check("menvcfg.PBMTE sets", csr_read(0x30a) >> 62 & 1, 1);
    root[1] = leaf(0x80000000, 1);
    check("PBMT=NC, PBMTE=1: loads", sload(0x40000000 + off, &c), target);
    check("  no trap", c, 0);
    root[1] = leaf(0x80000000, 2);
    check("PBMT=IO, PBMTE=1: loads", sload(0x40000000 + off, &c), target);
    check("  no trap", c, 0);
    root[1] = leaf(0x80000000, 3);
    sload(0x40000000 + off, &c);
    check("PBMT=3 (reserved): load page fault", c, 13);
    check("2 MiB pages through a plain pointer: load", sload(0xc0000000 + off, &c), target);
    root[3] |= 1ull << 61;
    sload(0xc0000000 + off, &c);
    check("PBMT on a pointer PTE: load page fault", c, 13);
    root[3] &= ~(3ull << 61);
    root[1] = leaf(0x80000000, 0) | 1ull << 54;
    sload(0x40000000 + off, &c);
    check("a reserved PTE bit (54): load page fault", c, 13);
    // An 8-byte store across a mapped page and an unmapped one: a store page
    // fault whose mtval is the second page's address, the part that faulted.
    root[4] = ((uint64_t)(uintptr_t)l1b >> 12) << 10 | PTE_V;
    l1b[0] = ((uint64_t)(uintptr_t)l0 >> 12) << 10 | PTE_V;
    l0[0] = leaf((uintptr_t)page, 0);
    lastCause = 0;
    __asm__ volatile("sfence.vma\n"
                     "csrs mstatus, %1\n"
                     "sd %2, 0(%0)\n"
                     "csrc mstatus, %1\n"
                     :
                     : "r"(0x100000ffcull), "r"(1ull << 17 | 1ull << 11), "r"(0x1122334455667788ull)
                     : "memory");
    check("a store across into an unmapped page: store page fault", lastCause, 15);
    check("  mtval is the unmapped page's address", lastTval, 0x100001000ull);
    lastCause = 0;
    __asm__ volatile("sfence.vma\n"
                     "csrs mstatus, %1\n"
                     "sd %2, 0(%0)\n"
                     "csrc mstatus, %1\n"
                     :
                     : "r"(0xffcull | 0x0ffffff000ull), "r"(1ull << 17 | 1ull << 11), "r"(0x1122334455667788ull)
                     : "memory");
    check("a store from an unmapped page across: store page fault", lastCause, 15);
    check("  mtval is the access's own address", lastTval, 0xffffffffcull);
    csr_write(satp, 0);
    __asm__ volatile("csrc 0x30a, %0" ::"r"(1ull << 62));
    check("menvcfg.PBMTE clears", csr_read(0x30a) >> 62 & 1, 0);
}

// Zalasr through .insn (clang 18 has no mnemonics for it): funct7 is
// funct5 << 2 | aq << 1 | rl.
#define LAQ(f3, f7, rd, p) __asm__ volatile(".insn r 0x2f, " #f3 ", " #f7 ", %0, %1, x0" : "=r"(rd) : "r"(p) : "memory")
#define SRL(f3, f7, p, v) __asm__ volatile(".insn r 0x2f, " #f3 ", " #f7 ", x0, %0, %1" ::"r"(p), "r"(v) : "memory")
static volatile uint64_t word __attribute__((aligned(8)));

static void zalasr(void) {
    uint64_t r = 0, c;
    word = 0x80f1f2f38485f687ull;
    LAQ(0, 0x1a, r, &word); check("lb.aq sign-extends", r, 0xffffffffffffff87ull);
    LAQ(1, 0x1a, r, &word); check("lh.aq sign-extends", r, 0xfffffffffffff687ull);
    LAQ(2, 0x1a, r, &word); check("lw.aq sign-extends", r, 0xffffffff8485f687ull);
    LAQ(3, 0x1a, r, &word); check("ld.aq", r, 0x80f1f2f38485f687ull);
    LAQ(2, 0x1b, r, (volatile char*)&word + 4); check("lw.aqrl", r, 0xffffffff80f1f2f3ull);
    word = 0;
    SRL(0, 0x1d, &word, 0x1234567890abcdefull); check("sb.rl", word, 0xef);
    SRL(1, 0x1d, &word, 0x1234567890abcdefull); check("sh.rl", word, 0xcdef);
    SRL(2, 0x1d, &word, 0x1234567890abcdefull); check("sw.rl", word, 0x90abcdef);
    SRL(3, 0x1f, &word, 0x1234567890abcdefull); check("sd.aqrl", word, 0x1234567890abcdefull);
    // Misaligned: address-misaligned exceptions (4 for the load, 6 for the store).
    lastCause = 0;
    LAQ(2, 0x1a, r, (volatile char*)&word + 2);
    c = lastCause; check("lw.aq misaligned: load address misaligned", c, 4);
    lastCause = 0;
    SRL(3, 0x1d, (volatile char*)&word + 4, 0);
    c = lastCause; check("sd.rl misaligned: store/AMO address misaligned", c, 6);
    check("  memory untouched", word, 0x1234567890abcdefull);
    // Reserved: a load without aq, a store without rl.
    lastCause = 0;
    LAQ(2, 0x18, r, &word);
    c = lastCause; check("lw without aq: illegal instruction", c, 2);
    lastCause = 0;
    SRL(2, 0x1c, &word, 0);
    c = lastCause; check("sw without rl: illegal instruction", c, 2);
    check("  memory untouched", word, 0x1234567890abcdefull);
}

int main(int hart) {
    if (hart) for (;;) {}
    csr_write(mscratch, (uintptr_t)(trapStack + 62));
    csr_write(mtvec, (uintptr_t)handler);
    svpbmt();
    zalasr();
    puts_("svpbmt+zalasr: ");
    putd((uint32_t)(checks - fails));
    puts_(" of ");
    putd((uint32_t)checks);
    puts_(fails ? " checks passed\n" : " checks passed, all\n");
    return fails;
}
