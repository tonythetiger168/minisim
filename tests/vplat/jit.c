// jit.c - work for mvplat's translator on an RV64 hart: every integer
// instruction it translates, on edge values and on a pseudo-random stream,
// in loops hot enough to be translated, with loads and stores of each width,
// loads from a device inside a translated loop (which leave for the
// interpreter), and blocks longer than the budget left in a quantum. Prints
// a checksum, which tests/mvplat.sh compares with -nojit's, and checks the
// division and high-multiply corner cases against the ISA's answers. Then
// all of it again in supervisor mode with Sv39 paging (translated through
// the data page caches), with loads across a page boundary, AMOs and
// CBO.ZERO, which has to give the same checksum.
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

#define OP(name, insn) \
    static uint64_t name(uint64_t a, uint64_t b) { uint64_t r; __asm__ volatile(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }
OP(div_, "div") OP(divu_, "divu") OP(rem_, "rem") OP(remu_, "remu")
OP(divw_, "divw") OP(divuw_, "divuw") OP(remw_, "remw") OP(remuw_, "remuw")
OP(mulh_, "mulh") OP(mulhu_, "mulhu") OP(mulhsu_, "mulhsu") OP(mulw_, "mulw")
OP(sll_, "sll") OP(srl_, "srl") OP(sra_, "sra") OP(sllw_, "sllw") OP(srlw_, "srlw") OP(sraw_, "sraw")
OP(slt_, "slt") OP(sltu_, "sltu") OP(addw_, "addw") OP(subw_, "subw")

static const uint64_t edge[] = {0, 1, 2, 3, 7, ~0ull, ~1ull, 0x8000000000000000ull, 0x7fffffffffffffffull,
                                0x80000000ull, 0x7fffffffull, 0xffffffff80000000ull, 0xffffffffull, 0x100000000ull,
                                0x123456789abcdef0ull, 0xfedcba9876543210ull, 63, 64, 31, 32};
#define NE (sizeof edge / sizeof edge[0])
static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

// Everything a translated block computes, mixed into one value.
static uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull + (h >> 29); }
static uint64_t ops(uint64_t a, uint64_t b) {
    uint64_t h = 0;
    h = mix(h, div_(a, b)); h = mix(h, divu_(a, b)); h = mix(h, rem_(a, b)); h = mix(h, remu_(a, b));
    h = mix(h, divw_(a, b)); h = mix(h, divuw_(a, b)); h = mix(h, remw_(a, b)); h = mix(h, remuw_(a, b));
    h = mix(h, mulh_(a, b)); h = mix(h, mulhu_(a, b)); h = mix(h, mulhsu_(a, b)); h = mix(h, mulw_(a, b));
    h = mix(h, sll_(a, b)); h = mix(h, srl_(a, b)); h = mix(h, sra_(a, b));
    h = mix(h, sllw_(a, b)); h = mix(h, srlw_(a, b)); h = mix(h, sraw_(a, b));
    h = mix(h, slt_(a, b)); h = mix(h, sltu_(a, b)); h = mix(h, addw_(a, b)); h = mix(h, subw_(a, b));
    h = mix(h, a * b); h = mix(h, a + b); h = mix(h, a - b); h = mix(h, a ^ b); h = mix(h, a | b); h = mix(h, a & b);
    h = mix(h, (int64_t)a < 5); h = mix(h, a < 5u); h = mix(h, (uint64_t)(int64_t)(int32_t)(a + 2047));
    h = mix(h, a << 13); h = mix(h, a >> 7); h = mix(h, (uint64_t)((int64_t)a >> 9));
    h = mix(h, (uint64_t)(int64_t)(int32_t)((uint32_t)a << 5)); h = mix(h, (uint64_t)(int64_t)(int32_t)((uint32_t)a >> 3));
    h = mix(h, (uint64_t)(int64_t)((int32_t)a >> 4));
    if ((int64_t)a < (int64_t)b) h = mix(h, 1);
    if (a < b) h = mix(h, 2);
    if (a == b) h = mix(h, 3);
    return h;
}

static volatile uint8_t buf8[64];
static volatile uint64_t buf64[16];
static uint64_t memops(uint64_t a) {
    uint64_t h = 0;
    for (int i = 0; i < 8; i++) buf64[i] = a * (uint64_t)(i + 1);
    for (int i = 0; i < 64; i++) buf8[i] = (uint8_t)(a >> (i & 63));
    const volatile int8_t* s8 = (const volatile int8_t*)buf8;
    const volatile int16_t* s16 = (const volatile int16_t*)buf8;
    const volatile int32_t* s32 = (const volatile int32_t*)buf8;
    const volatile uint16_t* u16 = (const volatile uint16_t*)buf8;
    const volatile uint32_t* u32 = (const volatile uint32_t*)buf8;
    for (int i = 0; i < 8; i++) {
        h = mix(h, (uint64_t)s8[i * 7]); h = mix(h, buf8[i * 5]);
        h = mix(h, (uint64_t)s16[i * 3]); h = mix(h, u16[i * 2]);
        h = mix(h, (uint64_t)s32[i]); h = mix(h, u32[i + 3]);
        h = mix(h, buf64[i]);
        ((volatile uint16_t*)buf8)[i] = (uint16_t)h;
        ((volatile uint32_t*)buf8)[i + 4] = (uint32_t)(h >> 7);
    }
    return h;
}

// 8-byte loads and stores at every offset around a page boundary.
static volatile uint8_t pages[8192] __attribute__((aligned(4096)));
static uint64_t straddle(uint64_t a) {
    uint64_t h = 0;
    for (int o = 4080; o < 4100; o++) {
        *(volatile uint64_t*)&pages[o] = a + (uint64_t)o;
        *(volatile uint32_t*)&pages[o + 1] ^= (uint32_t)a;
        h = mix(h, *(volatile uint64_t*)&pages[o - 3]);
        h = mix(h, *(volatile uint16_t*)&pages[o]);
    }
    return h;
}

// The AMOs (translated with paging on, one hart) and CBO.ZERO.
#define AMO(name, insn, T) \
    static uint64_t name(volatile T* p, uint64_t b) { uint64_t r; __asm__ volatile(insn " %0, %2, (%1)" : "=r"(r) : "r"(p), "r"(b) : "memory"); return r; }
AMO(amoaddd, "amoadd.d", uint64_t) AMO(amoswapd, "amoswap.d", uint64_t) AMO(amoxord, "amoxor.d", uint64_t)
AMO(amoord, "amoor.d", uint64_t) AMO(amoandd, "amoand.d", uint64_t) AMO(amomind, "amomin.d", uint64_t)
AMO(amomaxd, "amomax.d", uint64_t) AMO(amominud, "amominu.d", uint64_t) AMO(amomaxud, "amomaxu.d", uint64_t)
AMO(amoaddw, "amoadd.w", uint32_t) AMO(amoswapw, "amoswap.w", uint32_t) AMO(amoxorw, "amoxor.w", uint32_t)
AMO(amoorw, "amoor.w", uint32_t) AMO(amoandw, "amoand.w", uint32_t) AMO(amominw, "amomin.w", uint32_t)
AMO(amomaxw, "amomax.w", uint32_t) AMO(amominuw, "amominu.w", uint32_t) AMO(amomaxuw, "amomaxu.w", uint32_t)
static volatile uint64_t amoCell[8] __attribute__((aligned(64)));
static uint64_t amos(uint64_t a, uint64_t b) {
    uint64_t h = 0;
    volatile uint64_t* d = &amoCell[1];
    volatile uint32_t* w = (volatile uint32_t*)&amoCell[2] + 1;
    uint64_t (*const fd[])(volatile uint64_t*, uint64_t) = {amoaddd, amoswapd, amoxord, amoord, amoandd, amomind, amomaxd, amominud, amomaxud};
    uint64_t (*const fw[])(volatile uint32_t*, uint64_t) = {amoaddw, amoswapw, amoxorw, amoorw, amoandw, amominw, amomaxw, amominuw, amomaxuw};
    for (int i = 0; i < 9; i++) {
        *d = a; h = mix(h, fd[i](d, b)); h = mix(h, *d);
        *w = (uint32_t)a; h = mix(h, fw[i](w, b)); h = mix(h, *w); h = mix(h, amoCell[2]);
    }
    for (int i = 0; i < 8; i++) amoCell[i] = ~a;
    __asm__ volatile(".insn i 0x0f, 2, x0, %0, 4" ::"r"((uintptr_t)&amoCell[3] + 5) : "memory");  // cbo.zero
    for (int i = 0; i < 8; i++) h = mix(h, amoCell[i]);
    return h;
}

static uint64_t work(void) {
    // The corner cases, against the ISA (M extension, division by zero and overflow).
    const uint64_t MIN = 0x8000000000000000ull, M32 = 0xffffffff80000000ull;
    for (int rep = 0; rep < 50; rep++) {  // again and again, so the checks run translated
        check("div by 0", div_(7, 0), ~0ull);
        check("divu by 0", divu_(7, 0), ~0ull);
        check("rem by 0", rem_(7, 0), 7);
        check("remu by 0", remu_(7, 0), 7);
        check("div overflow", div_(MIN, ~0ull), MIN);
        check("rem overflow", rem_(MIN, ~0ull), 0);
        check("div -7/2", div_((uint64_t)-7, 2), (uint64_t)-3);
        check("rem -7%2", rem_((uint64_t)-7, 2), (uint64_t)-1);
        check("divu", divu_(~0ull, 3), 0x5555555555555555ull);
        check("divw by 0", divw_(7, 0), ~0ull);
        check("divuw by 0", divuw_(7, 0), ~0ull);
        check("remw by 0", remw_(0x1234567887654321ull, 0), 0xffffffff87654321ull);
        check("remuw by 0", remuw_(0x1234567807654321ull, 0), 0x07654321ull);
        check("divw overflow", divw_(M32, ~0ull), M32);
        check("remw overflow", remw_(M32, ~0ull), 0);
        check("divuw", divuw_(0xffffffffull, 2), 0x7fffffffull);
        check("divw ignores the upper half", divw_(0x500000006ull, 0x700000003ull), 2);
        check("mulh", mulh_(MIN, MIN), 0x4000000000000000ull);
        check("mulh -1*-1", mulh_(~0ull, ~0ull), 0);
        check("mulhu", mulhu_(~0ull, ~0ull), ~1ull);
        check("mulhsu -1*max", mulhsu_(~0ull, ~0ull), ~0ull);
        check("mulhsu 2*max", mulhsu_(2, ~0ull), 1);
        check("mulhsu min*2", mulhsu_(MIN, 2), ~0ull);
        check("mulw", mulw_(0x80000000ull, 2), 0);
        check("sraw by 63 (5 bits)", sraw_(M32, 63), ~0ull);
        check("sll by 65 (6 bits)", sll_(1, 65), 2);
    }
    uint64_t h = 0;
    for (unsigned i = 0; i < NE; i++)
        for (unsigned j = 0; j < NE; j++) h = mix(h, ops(edge[i], edge[j]));
    for (int i = 0; i < 20000; i++) {
        uint64_t a = rnd(), b = rnd();
        if (i & 1) b >>= (a & 63);
        if (i % 7 == 0) b = 0;
        h = mix(h, ops(a, b));
        if (i % 3 == 0) h = mix(h, memops(a));
        if (i % 1000 == 0) h = mix(h, REG8(UART0 + 5) & 0x60);  // a device load in a hot loop
        if (i % 50 == 0) h = mix(h, straddle(a));
        if (i % 5 == 0) h = mix(h, amos(a, b));
    }
    return h;
}

// Sv39: root[0] maps the devices (identity, 1 GiB), root[2] the RAM.
static uint64_t root[512] __attribute__((aligned(4096)));
#define PTE(pa) (((uint64_t)(pa) >> 12) << 10 | 0xcf)  // V R W X A D
static uint64_t machineSum;

static __attribute__((noinline, used)) void supervisor(void) {
    seed = 0x9e3779b97f4a7c15ull;
    for (unsigned i = 0; i < sizeof pages; i++) pages[i] = 0;
    check("supervisor mode, Sv39: the same checksum", work(), machineSum);
    puts_("jit: ");
    putd((uint32_t)(checks - fails));
    puts_(" of ");
    putd((uint32_t)checks);
    puts_(fails ? " checks passed\n" : " checks passed, all\n");
    exit_(fails);
}

int main(int hart) {
    if (hart) for (;;) {}
    const uint64_t h = machineSum = work();
    puts_("jit checksum ");
    putx((uint32_t)(h >> 32));
    putx((uint32_t)h);
    puts_("\n");
    csr_write(pmpaddr0, ~0ull >> 10);  // PMP: everything to S and U
    csr_write(pmpcfg0, 0x1f);
    root[0] = PTE(0);
    root[2] = PTE(0x80000000u);
    csr_write(satp, (8ull << 60) | ((uintptr_t)root >> 12));
    __asm__ volatile("sfence.vma" ::: "memory");
    __asm__ volatile("csrc mstatus, %0" ::"r"(3ull << 11));
    csr_set(mstatus, 1ull << 11);  // MPP = S
    csr_write(mepc, (uintptr_t)supervisor);
    __asm__ volatile("mret");
    return 1;
}
