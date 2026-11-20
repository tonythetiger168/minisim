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

// Zbb, by encoding (the programs are built for rv64imac).
#define ZU(name, op, f3, imm) static uint64_t name(uint64_t a) { uint64_t r; __asm__ volatile(".insn i " #op ", " #f3 ", %0, %1, " #imm : "=r"(r) : "r"(a)); return r; }
#define ZB(name, op, f3, f7) static uint64_t name(uint64_t a, uint64_t b) { uint64_t r; __asm__ volatile(".insn r " #op ", " #f3 ", " #f7 ", %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }
ZU(clz_, 0x13, 1, 0x600) ZU(ctz_, 0x13, 1, 0x601) ZU(cpop_, 0x13, 1, 0x602) ZU(sextb_, 0x13, 1, 0x604) ZU(sexth_, 0x13, 1, 0x605)
ZU(orcb_, 0x13, 5, 0x287) ZU(rev8_, 0x13, 5, 0x6b8) ZU(rori13_, 0x13, 5, 0x60d) ZU(rori0_, 0x13, 5, 0x600) ZU(rori63_, 0x13, 5, 0x63f)
ZU(clzw_, 0x1b, 1, 0x600) ZU(ctzw_, 0x1b, 1, 0x601) ZU(cpopw_, 0x1b, 1, 0x602) ZU(roriw5_, 0x1b, 5, 0x605) ZU(roriw31_, 0x1b, 5, 0x61f)
ZB(andn_, 0x33, 7, 0x20) ZB(orn_, 0x33, 6, 0x20) ZB(xnor_, 0x33, 4, 0x20) ZB(min_, 0x33, 4, 0x05) ZB(minu_, 0x33, 5, 0x05)
ZB(max_, 0x33, 6, 0x05) ZB(maxu_, 0x33, 7, 0x05) ZB(rol_, 0x33, 1, 0x30) ZB(ror_, 0x33, 5, 0x30) ZB(rolw_, 0x3b, 1, 0x30)
ZB(rorw_, 0x3b, 5, 0x30)
static uint64_t zexth_(uint64_t a) { uint64_t r; __asm__ volatile(".insn r 0x3b, 4, 0x04, %0, %1, x0" : "=r"(r) : "r"(a)); return r; }
static uint64_t zbb(uint64_t a, uint64_t b) {
    uint64_t h = 0;
    uint64_t (*const u[])(uint64_t) = {clz_, ctz_, cpop_, sextb_, sexth_, orcb_, rev8_, rori13_, rori0_, rori63_,
                                       clzw_, ctzw_, cpopw_, roriw5_, roriw31_, zexth_};
    uint64_t (*const bi[])(uint64_t, uint64_t) = {andn_, orn_, xnor_, min_, minu_, max_, maxu_, rol_, ror_, rolw_, rorw_};
    for (unsigned i = 0; i < sizeof u / sizeof u[0]; i++) h = mix(h, u[i](a));
    for (unsigned i = 0; i < sizeof bi / sizeof bi[0]; i++) h = mix(h, bi[i](a, b));
    return h;
}

// LR/SC: a reservation that holds, one another address breaks, and an SC
// with none.
static volatile uint64_t lrCell[4] __attribute__((aligned(64)));
static uint64_t lrsc(uint64_t a) {
    uint64_t h = 0, v, f;
    lrCell[0] = a;
    __asm__ volatile("lr.d %0, (%2)\n sc.d %1, %3, (%2)" : "=&r"(v), "=&r"(f) : "r"(&lrCell[0]), "r"(a + 1) : "memory");
    h = mix(h, v); h = mix(h, f); h = mix(h, lrCell[0]);
    __asm__ volatile("lr.w %0, (%2)\n sc.w %1, %3, (%2)" : "=&r"(v), "=&r"(f) : "r"(&lrCell[1]), "r"(a >> 3) : "memory");
    h = mix(h, v); h = mix(h, f); h = mix(h, lrCell[1]);
    __asm__ volatile("lr.d %0, (%2)\n sc.d %1, %3, (%4)" : "=&r"(v), "=&r"(f) : "r"(&lrCell[2]), "r"(a), "r"(&lrCell[3]) : "memory");
    h = mix(h, v); h = mix(h, f); h = mix(h, lrCell[3]);
    __asm__ volatile("sc.d %0, %1, (%2)" : "=&r"(f) : "r"(a), "r"(&lrCell[2]) : "memory");  // no reservation now
    h = mix(h, f); h = mix(h, lrCell[2]);
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
        check("clz 0", clz_(0), 64);
        check("clz", clz_(0x0000100000000000ull), 19);
        check("ctz 0", ctz_(0), 64);
        check("ctz", ctz_(0x80), 7);
        check("cpop", cpop_(0xf0f0000000000001ull), 9);
        check("clzw 0 (the upper half ignored)", clzw_(0xffffffff00000000ull), 32);
        check("clzw", clzw_(0x10000), 15);
        check("ctzw 0", ctzw_(0x100000000ull), 32);
        check("cpopw", cpopw_(0xffffffff000000ffull), 8);
        check("sext.b", sextb_(0x1280), 0xffffffffffffff80ull);
        check("sext.h", sexth_(0x18000), 0xffffffffffff8000ull);
        check("zext.h", zexth_(0xffffffffffff8001ull), 0x8001);
        check("orc.b", orcb_(0x0100ff0000800002ull), 0xff00ff0000ff00ffull);
        check("rev8", rev8_(0x0102030405060708ull), 0x0807060504030201ull);
        check("rori 13", rori13_(1), 1ull << 51);
        check("roriw 5 (sign-extended)", roriw5_(0x10), 0xffffffff80000000ull);
        check("rolw", rolw_(0x80000001ull, 1), 3);
        check("rorw by 33 (5 bits)", rorw_(2, 33), 1);
        check("rol", rol_(0x8000000000000001ull, 65), 3);
        check("min", min_(~0ull, 1), ~0ull);
        check("minu", minu_(~0ull, 1), 1);
        check("max", max_(~0ull, 1), 1);
        check("maxu", maxu_(~0ull, 1), ~0ull);
        check("andn", andn_(0xff, 0x0f), 0xf0);
        check("orn", orn_(0, ~0xfull), 0xf);
        check("xnor", xnor_(0xf0, 0xff), ~0x0full);
    }
    uint64_t h = 0;
    for (unsigned i = 0; i < NE; i++)
        for (unsigned j = 0; j < NE; j++) h = mix(h, ops(edge[i], edge[j])), h = mix(h, zbb(edge[i], edge[j]));
    for (int i = 0; i < 20000; i++) {
        uint64_t a = rnd(), b = rnd();
        if (i & 1) b >>= (a & 63);
        if (i % 7 == 0) b = 0;
        h = mix(h, ops(a, b));
        if (i % 3 == 0) h = mix(h, memops(a));
        if (i % 1000 == 0) h = mix(h, REG8(UART0 + 5) & 0x60);  // a device load in a hot loop
        if (i % 50 == 0) h = mix(h, straddle(a));
        if (i % 5 == 0) h = mix(h, amos(a, b));
        if (i % 4 == 1) h = mix(h, lrsc(a));
        if (i % 2 == 0) h = mix(h, zbb(a, i & 4 ? b : a >> (b & 63)));
    }
    return h;
}

// Sv39: root[0] maps the devices (identity, 1 GiB), root[2] the RAM.
static uint64_t root[512] __attribute__((aligned(4096)));
#define PTE(pa) (((uint64_t)(pa) >> 12) << 10 | 0xcf)  // V R W X A D
static uint64_t machineSum;

// SFENCE.VMA with an address: VA 0xc0000000 (root[3]) goes through l1;
// l1[0] is a 2 MiB page, l1[1] a table of 4 KiB pages (l0). Remapping one
// and fencing its address has to show the new page to loads and to code
// already translated, and for the 2 MiB page to every 4 KiB of it.
static uint64_t l1[512] __attribute__((aligned(4096)));
static uint64_t l0[512] __attribute__((aligned(4096)));
static volatile uint32_t codeA[1024] __attribute__((aligned(4096)));
static volatile uint32_t codeB[1024] __attribute__((aligned(4096)));
#define NONLEAF(pa) (((uint64_t)(pa) >> 12) << 10 | 0x01)
#define MEG1 0x80400000u  // two 2 MiB regions of RAM the program does not use
#define MEG2 0x80600000u
#define VA4K 0xc0280000u  // (its fetch page record entry is not one the program's own pages take)
static void sfenceVa(uintptr_t va) { __asm__ volatile("sfence.vma %0, zero" ::"r"(va) : "memory"); }
static long callVa(uintptr_t va, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) s += ((long (*)(void))va)();
    return s;
}
static void remaps(void) {
    codeA[0] = 0x00b00513;  // li a0, 11
    codeA[1] = 0x00008067;  // ret
    codeB[0] = 0x01600513;  // li a0, 22
    codeB[1] = 0x00008067;
    __asm__ volatile("fence.i" ::: "memory");
    for (int k = 0; k < 40; k++) {
        const uintptr_t pa = (k & 1) ? (uintptr_t)codeB : (uintptr_t)codeA;
        l0[0x80] = PTE(pa);
        sfenceVa(VA4K);
        check("SFENCE.VMA va: the 4 KiB page's new code runs", (uint64_t)callVa(VA4K, 30), (k & 1) ? 660 : 330);
        check("SFENCE.VMA va: loads see the new 4 KiB page", *(volatile uint32_t*)(VA4K + 4), 0x00008067);
        check("SFENCE.VMA va: the new 4 KiB page's word", *(volatile uint32_t*)VA4K, (k & 1) ? 0x01600513u : 0x00b00513u);
        // the 2 MiB page: fence one address, read another 4 KiB of it
        l1[0] = PTE((k & 1) ? MEG2 : MEG1) | 0;
        sfenceVa(0xc0000000u + 0x1000);
        check("SFENCE.VMA va: the whole 2 MiB page is remapped", *(volatile uint64_t*)(0xc0000000u + 0x123000),
              (k & 1) ? 0x2222u : 0x1111u);
        // a load from it again and again (in the page caches) before the next round
        uint64_t t = 0;
        for (int i = 0; i < 50; i++) t += *(volatile uint64_t*)(0xc0000000u + 0x123000);
        check("SFENCE.VMA va: the 2 MiB page, hot", t, 50 * ((k & 1) ? 0x2222u : 0x1111u));
    }
}

static __attribute__((noinline, used)) void supervisor(void) {
    remaps();
    seed = 0x9e3779b97f4a7c15ull;
    for (unsigned i = 0; i < sizeof pages; i++) pages[i] = 0;
    for (unsigned i = 0; i < 4; i++) lrCell[i] = 0;
    check("supervisor mode, Sv39: the same checksum", work(), machineSum);
    // CSRRCI on sstatus (translated in supervisor mode): SIE cleared and set
    // again, the old values in the checksum
    uint64_t hs = 0;
    for (int i = 0; i < 3000; i++) {
        uint64_t o;
        if (i & 1) __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(o));
        else __asm__ volatile("csrrci %0, sstatus, 0" : "=r"(o));
        hs = mix(hs, o);
        // plain reads (translated too)
        if (i % 5 == 0) __asm__ volatile("csrr %0, sstatus" : "=r"(o));
        else if (i % 5 == 1) __asm__ volatile("csrrc %0, sstatus, x0" : "=r"(o));
        else __asm__ volatile("csrrsi %0, sstatus, 0" : "=r"(o));
        hs = mix(hs, o);
        if (i % 3 == 0) __asm__ volatile("csrsi sstatus, 2");
    }
    puts_("sstatus checksum ");
    putx((uint32_t)(hs >> 32));
    putx((uint32_t)hs);
    puts_("\n");
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
    root[3] = NONLEAF(l1);
    l1[0] = PTE(MEG1);
    l1[1] = NONLEAF(l0);
    l0[0x80] = PTE(codeA);
    *(volatile uint64_t*)(MEG1 + 0x123000) = 0x1111;
    *(volatile uint64_t*)(MEG2 + 0x123000) = 0x2222;
    csr_write(satp, (8ull << 60) | ((uintptr_t)root >> 12));
    __asm__ volatile("sfence.vma" ::: "memory");
    __asm__ volatile("csrc mstatus, %0" ::"r"(3ull << 11));
    csr_set(mstatus, 1ull << 11);  // MPP = S
    csr_write(mepc, (uintptr_t)supervisor);
    __asm__ volatile("mret");
    return 1;
}
