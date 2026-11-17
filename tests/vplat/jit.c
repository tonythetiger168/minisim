// jit.c - work for mvplat's translator on an RV64 hart: every integer
// instruction it translates, on edge values and on a pseudo-random stream,
// in loops hot enough to be translated, with loads and stores of each width,
// loads from a device inside a translated loop (which leave for the
// interpreter), and blocks longer than the budget left in a quantum. Prints
// a checksum, which tests/mvplat.sh compares with -nojit's, and checks the
// division and high-multiply corner cases against the ISA's answers.
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

int main(int hart) {
    if (hart) for (;;) {}
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
    }
    puts_("jit checksum ");
    putx((uint32_t)(h >> 32));
    putx((uint32_t)h);
    puts_("\njit: ");
    putd((uint32_t)(checks - fails));
    puts_(" of ");
    putd((uint32_t)checks);
    puts_(fails ? " checks passed\n" : " checks passed, all\n");
    return fails;
}
