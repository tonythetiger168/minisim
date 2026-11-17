// smc.c - self-modifying code on an RV64 hart: FENCE.I has to make the
// harts see code that changed, while mvplat keeps what it decoded from pages
// that did not. Code written to a buffer and run hot enough to be in the
// fast loop's blocks (and translated, in machine mode), then rewritten and
// fenced, again and again: by word stores, by byte stores, a compressed
// instruction, a 4-byte instruction across two pages whose second half
// changes, and, in supervisor mode with Sv39, code run through one virtual
// address while it is rewritten through another that maps the same page.
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

// Two pages of code, and one more that never changes (its decoded
// instructions have to survive every FENCE.I).
static volatile uint32_t code[2048] __attribute__((aligned(4096)));
static volatile uint32_t steady[1024] __attribute__((aligned(4096)));

typedef long (*fn_t)(void);
static uint32_t addi_a0(int imm) { return (uint32_t)(imm & 0xfff) << 20 | 10u << 7 | 0x13; }
#define RET 0x00008067u
static uint16_t cli_a0(int imm) { return (uint16_t)(2u << 13 | ((imm >> 5) & 1u) << 12 | 10u << 7 | (imm & 31u) << 2 | 1); }
#define CJR_RA 0x8082u
static void fencei(void) { __asm__ volatile("fence.i" ::: "memory"); }

// Run f n times (hot: the fast loop makes a block of it, the translator
// code) and return the sum of its results.
static __attribute__((noinline)) long run(uintptr_t f, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) s += ((fn_t)f)();
    return s;
}

// `base` is where the code is run from (another mapping of the same page in
// supervisor mode).
static void rounds(const char* mode, uintptr_t base) {
    const uintptr_t off = base - (uintptr_t)code;
    steady[0] = addi_a0(77);
    steady[1] = RET;
    fencei();
    for (int k = 1; k <= 200; k++) {
        // words
        code[0] = addi_a0(k);
        code[1] = RET;
        fencei();
        check(mode, (uint64_t)run((uintptr_t)code + off, 50), (uint64_t)(50 * k));
        check(mode, (uint64_t)run((uintptr_t)steady + off, 3), 231);
        // bytes, at another place in the page
        uint32_t w = addi_a0(-k);
        volatile uint8_t* b = (volatile uint8_t*)&code[64];
        for (int i = 0; i < 4; i++) b[i] = (uint8_t)(w >> (8 * i));
        code[65] = RET;
        fencei();
        check(mode, (uint64_t)run((uintptr_t)&code[64] + off, 20), (uint64_t)(-20L * k));
        // a compressed instruction
        volatile uint16_t* hw = (volatile uint16_t*)&code[128];
        hw[0] = cli_a0(k & 31);
        hw[1] = CJR_RA;
        fencei();
        check(mode, (uint64_t)run((uintptr_t)&code[128] + off, 10), (uint64_t)(10L * (k & 31)));
        // across the two pages: c.nop at 0xffc, ADDI at 0xffe (its immediate
        // in the second page), RET at 0x1002
        volatile uint16_t* x = (volatile uint16_t*)code;
        const uint32_t a = addi_a0(k + 1000);
        x[0x7fe] = 0x0001;
        x[0x7ff] = (uint16_t)a;
        x[0x800] = (uint16_t)(a >> 16);
        x[0x801] = (uint16_t)RET;
        x[0x802] = (uint16_t)(RET >> 16);
        fencei();
        check(mode, (uint64_t)run((uintptr_t)&x[0x7fe] + off, 10), (uint64_t)(10L * (k + 1000)));
    }
}

// Sv39: root[0] maps the devices (identity, 1 GiB), root[2] the RAM
// (identity), root[1] VA 0x40000000 to the RAM as well.
static uint64_t root[512] __attribute__((aligned(4096)));
#define PTE(pa) (((uint64_t)(pa) >> 12) << 10 | 0xcf)  // V R W X A D

static void report(void) {
    puts_("smc: ");
    putd((uint32_t)(checks - fails));
    puts_(" of ");
    putd((uint32_t)checks);
    puts_(fails ? " checks passed\n" : " checks passed, all\n");
    exit_(fails);
}

static __attribute__((noinline, used)) void supervisor(void) {
    rounds("supervisor, Sv39, through an alias", (uintptr_t)code - 0x80000000u + 0x40000000u);
    rounds("supervisor, Sv39", (uintptr_t)code);
    report();
}

int main(int hart) {
    if (hart) for (;;) {}
    rounds("machine", (uintptr_t)code);
    csr_write(pmpaddr0, ~0ull >> 10);  // PMP: everything to S and U
    csr_write(pmpcfg0, 0x1f);
    root[0] = PTE(0);
    root[1] = PTE(0x80000000u);
    root[2] = PTE(0x80000000u);
    csr_write(satp, (8ull << 60) | ((uintptr_t)root >> 12));
    __asm__ volatile("sfence.vma" ::: "memory");
    __asm__ volatile("csrc mstatus, %0" ::"r"(3ull << 11));
    csr_set(mstatus, 1ull << 11);  // MPP = S
    csr_write(mepc, (uintptr_t)supervisor);
    __asm__ volatile("mret");
    return 1;
}
