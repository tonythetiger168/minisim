// bf16.c - BFloat16 (Zfbfmin, Zvfbfmin, Zvfbfwma 1.0). FCVT.BF16.S against
// a model of the rounding written here from the specification (five rounding
// modes, overflow, tininess after rounding, NaNs) on special and random
// singles; FCVT.S.BF16 exact, NaN-boxing in and out; then each vector
// instruction against the scalar ones: VFNCVTBF16.F.F.W against FCVT.BF16.S,
// VFWCVTBF16.F.F.V against FCVT.S.BF16, VFWMACCBF16.VV/.VF against
// FCVT.S.BF16 and FMADD.S, results bit for bit and the accrued flags; and the
// SEW the vector forms reject. The new instructions are written as words
// (vector operands v8, v16, v20; scalar fa0, fa1) so no compiler support for
// them is needed.
#include <stddef.h>
#include "rt.h"

void exit(int code) { exit_(code); }

#define N 64
static int fails, checks;
static unsigned rm;
static void check(const char* what, int ok) {
    checks++;
    if (!ok) { puts_("FAIL "); puts_(what); puts_(" rm "); putd(rm); puts_("\n"); fails++; }
}
static uint32_t seed = 7;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 7 ^ seed << 11; }
static unsigned flags(void) { unsigned f; __asm__ volatile("csrrw %0, fflags, zero" : "=r"(f) :: "memory"); return f; }

// The trap handler: record mcause, step over the 4-byte instruction.
#if __riscv_xlen == 64
#define SR "sd "
#define LR "ld "
#else
#define SR "sw "
#define LR "lw "
#endif
volatile uintptr_t lastCause;
__attribute__((naked, aligned(4))) void handler(void) {
    __asm__ volatile(
        "csrrw sp, mscratch, sp\n" SR "t0, 0(sp)\n" SR "t1, 16(sp)\n"
        "csrr t0, mcause\n"
        "la t1, lastCause\n" SR "t0, 0(t1)\n" LR "t1, 16(sp)\n"
        "csrr t0, mepc\n"
        "addi t0, t0, 4\n"
        "csrw mepc, t0\n" LR "t0, 0(sp)\n"
        "csrrw sp, mscratch, sp\n"
        "mret\n");
}
static uint64_t trapStack[64] __attribute__((aligned(16)));

// ---------------------------------------------------------------- the model
enum { NX = 1, UF = 2, OF = 4, NV = 16 };
// mag >> sh rounded by the mode, for a value of sign neg.
static uint32_t roundAt(uint32_t mag, unsigned sh, int neg) {
    uint32_t q = mag >> sh, r = mag & ((1u << sh) - 1), h = 1u << (sh - 1);
    int up;
    switch (rm) {
        case 0: up = r > h || (r == h && (q & 1)); break;  // RNE
        case 1: up = 0; break;                             // RTZ
        case 2: up = neg && r; break;                      // RDN
        case 3: up = !neg && r; break;                     // RUP
        default: up = r >= h; break;                       // RMM
    }
    return q + up;
}
// Single to BFloat16. BFloat16 is the top half of a single, so rounding the
// magnitude's bit pattern at bit 16 is right for normals and subnormals
// alike, a carry running into the exponent.
static uint16_t model(uint32_t u, unsigned* fl) {
    const uint32_t mag = u & 0x7fffffff;
    const int neg = u >> 31;
    if (mag > 0x7f800000) { if (!(u & 0x400000)) *fl |= NV; return 0x7fc0; }
    if (mag == 0x7f800000) return u >> 16;
    uint32_t r = roundAt(mag, 16, neg);
    if (mag & 0xffff) {
        *fl |= NX;
        if (r == 0x7f80) *fl |= OF;
        // Tiny: below the least normal once rounded to 8 bits with an
        // unbounded exponent; only [2^-127, 2^-126) has 8 bits to round to.
        if (mag < 0x800000 && !(mag >= 0x400000 && roundAt(mag, 15, neg) >= 0x100)) *fl |= UF;
    }
    return (uint16_t)(neg << 15 | r);
}

// ---------------------------------------------------------------- scalar
static uint64_t boxed;  // the whole 64-bit register after the instruction
static uint16_t fcvt_bf16_s(uint32_t u) {
    __asm__ volatile("fmv.w.x fa1, %1\n"
                     ".word 0x4485f553\n"  // fcvt.bf16.s fa0, fa1
                     "fsd fa0, 0(%0)\n" ::"r"(&boxed), "r"(u) : "fa0", "fa1", "memory");
    return (uint16_t)boxed;
}
static uint32_t fcvt_s_bf16(uint64_t in) {  // in: the 64-bit register value
    static uint64_t src;
    src = in;
    __asm__ volatile("fld fa1, 0(%0)\n"
                     ".word 0x4065f553\n"  // fcvt.s.bf16 fa0, fa1
                     "fsd fa0, 0(%1)\n" ::"r"(&src), "r"(&boxed) : "fa0", "fa1", "memory");
    return (uint32_t)boxed;
}
static uint64_t box16(uint16_t b) { return 0xffffffffffff0000ull | b; }

static const uint32_t specialS[] = {
    0x00000000, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001 /* sNaN */, 0xffa00000 /* sNaN */,
    0x3f800000, 0xbf800000, 0x3f808000 /* tie, even */, 0x3f818000 /* tie, odd */, 0x3f808001, 0x3f807fff,
    0x7f7fffff, 0xff7fffff, 0x7f7f8000, 0x7f7f7fff, 0x7f7effff, 0x00000001, 0x80000001, 0x00008000, 0x00018000,
    0x007fffff, 0x807fffff, 0x007f8000, 0x007fa000, 0x007fbfff, 0x007fc000, 0x00800000, 0x0080ffff, 0x00400000,
    0x00c08000, 0x3eaaaaab, 0x40490fdb};
#define NS (sizeof specialS / sizeof specialS[0])
static uint32_t S[N];      // singles
static uint16_t BF[N], BG[N], R16[N], E16[N];
static uint32_t ACC[N], R32[N], E32[N];

static void fill(int round) {
    for (int i = 0; i < N; i++) {
        S[i] = round == 0 && i < (int)NS ? specialS[i] : rnd();
        if (round == 1 && (i & 3) == 0) S[i] = (S[i] & 0x807fffff) | (i & 4 ? 0 : 0x00800000);  // near the bottom
        BF[i] = (uint16_t)(round == 0 && i < (int)NS ? specialS[i] >> 16 : rnd());
        BG[i] = (uint16_t)rnd();
        ACC[i] = rnd();
        if (i % 7 == 0) BG[i] = 0x7f81;   // sNaN
        if (i % 11 == 0) BF[i] = 0;
        if (i % 13 == 0) ACC[i] = 0x7f800000;
    }
}

static void scalar(void) {
    for (int i = 0; i < N; i++) {
        unsigned want = 0;
        uint16_t e = model(S[i], &want);
        flags();
        uint16_t g = fcvt_bf16_s(S[i]);
        unsigned got = flags();
        if (g != e || got != want) {
            puts_("  fcvt.bf16.s "); putx(S[i]); puts_(" got "); putx(g); puts_("/"); putx(got);
            puts_(" want "); putx(e); puts_("/"); putx(want); puts_("\n");
        }
        check("fcvt.bf16.s against the model", g == e && got == want);
        check("fcvt.bf16.s NaN-boxes", boxed >> 16 == 0xffffffffffffull);
        uint32_t w = fcvt_s_bf16(box16(BF[i]));
        got = flags();
        uint32_t x = (uint32_t)BF[i] << 16;
        int nan = (x & 0x7fffffff) > 0x7f800000;
        check("fcvt.s.bf16 is exact", w == (nan ? 0x7fc00000 : x));
        check("fcvt.s.bf16 flags", got == (nan && !(x & 0x400000) ? NV : 0));
        check("fcvt.s.bf16 NaN-boxes", boxed >> 32 == 0xffffffffu);
    }
    check("fcvt.s.bf16 of an unboxed value: canonical NaN", fcvt_s_bf16(0x3f80) == 0x7fc00000);
}

// ---------------------------------------------------------------- vector
static void vector(void) {
    size_t vl;
    // vfncvtbf16.f.f.w v8, v16
    flags();
    for (size_t i = 0; i < N; i += vl)
        __asm__ volatile("vsetvli %0, %1, e32, m2, ta, ma\n"
                         "vle32.v v16, (%2)\n"
                         "vsetvli zero, %1, e16, m1, ta, ma\n"
                         ".word 0x4b0e9457\n"
                         "vse16.v v8, (%3)\n"
                         : "=&r"(vl) : "r"(N - i), "r"(S + i), "r"(R16 + i) : "memory", "v8", "v16", "v17");
    unsigned vf = flags();
    for (int i = 0; i < N; i++) E16[i] = fcvt_bf16_s(S[i]);
    unsigned sf = flags();
    int same = 1;
    for (int i = 0; i < N; i++) same &= R16[i] == E16[i];
    check("vfncvtbf16.f.f.w", same);
    check("vfncvtbf16.f.f.w flags", vf == sf);

    // vfwcvtbf16.f.f.v v8, v16
    for (size_t i = 0; i < N; i += vl)
        __asm__ volatile("vsetvli %0, %1, e16, m1, ta, ma\n"
                         "vle16.v v16, (%2)\n"
                         ".word 0x4b069457\n"
                         "vsetvli zero, %1, e32, m2, ta, ma\n"
                         "vse32.v v8, (%3)\n"
                         : "=&r"(vl) : "r"(N - i), "r"(BG + i), "r"(R32 + i) : "memory", "v8", "v9", "v16");
    vf = flags();
    for (int i = 0; i < N; i++) E32[i] = fcvt_s_bf16(box16(BG[i]));
    sf = flags();
    same = 1;
    for (int i = 0; i < N; i++) same &= R32[i] == E32[i];
    check("vfwcvtbf16.f.f.v", same);
    check("vfwcvtbf16.f.f.v flags", vf == sf);

    // vfwmaccbf16.vv v8, v16, v20: v8 += v16 * v20
    for (size_t i = 0; i < N; i += vl)
        __asm__ volatile("vsetvli %0, %1, e32, m2, ta, ma\n"
                         "vle32.v v8, (%2)\n"
                         "vsetvli zero, %1, e16, m1, ta, ma\n"
                         "vle16.v v16, (%3)\n"
                         "vle16.v v20, (%4)\n"
                         ".word 0xef481457\n"
                         "vsetvli zero, %1, e32, m2, ta, ma\n"
                         "vse32.v v8, (%5)\n"
                         : "=&r"(vl) : "r"(N - i), "r"(ACC + i), "r"(BF + i), "r"(BG + i), "r"(R32 + i)
                         : "memory", "v8", "v9", "v16", "v20");
    vf = flags();
    for (int i = 0; i < N; i++) {
        float a, b, c, r;
        uint32_t ua = fcvt_s_bf16(box16(BF[i])), ub = fcvt_s_bf16(box16(BG[i]));
        __builtin_memcpy(&a, &ua, 4); __builtin_memcpy(&b, &ub, 4); __builtin_memcpy(&c, &ACC[i], 4);
        __asm__ volatile("fmadd.s %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(b), "f"(c));
        __builtin_memcpy(&E32[i], &r, 4);
    }
    sf = flags();
    same = 1;
    for (int i = 0; i < N; i++) same &= R32[i] == E32[i];
    check("vfwmaccbf16.vv", same);
    check("vfwmaccbf16.vv flags", vf == sf);

    // vfwmaccbf16.vf v8, fa0, v20: v8 += fa0 * v20, fa0 a NaN-boxed BFloat16
    // (the last one is not boxed, so it reads as the canonical NaN)
    static uint64_t sc;
    for (int k = 0; k < 4; k++) {
        const uint16_t s = k == 0 ? 0x3fc0 /* 1.5 */ : k == 1 ? 0xc2f7 : 0x0003;
        sc = k == 3 ? 0x3fc0 : box16(s);
        for (size_t i = 0; i < N; i += vl)
            __asm__ volatile("fld fa0, 0(%6)\n"
                             "vsetvli %0, %1, e32, m2, ta, ma\n"
                             "vle32.v v8, (%2)\n"
                             "vsetvli zero, %1, e16, m1, ta, ma\n"
                             "vle16.v v20, (%4)\n"
                             ".word 0xef455457\n"
                             "vsetvli zero, %1, e32, m2, ta, ma\n"
                             "vse32.v v8, (%5)\n"
                             : "=&r"(vl) : "r"(N - i), "r"(ACC + i), "r"(0), "r"(BG + i), "r"(R32 + i), "r"(&sc)
                             : "memory", "fa0", "v8", "v9", "v20");
        vf = flags();
        for (int i = 0; i < N; i++) {
            float a, b, c, r;
            uint32_t ua = fcvt_s_bf16(sc), ub = fcvt_s_bf16(box16(BG[i]));
            __builtin_memcpy(&a, &ua, 4); __builtin_memcpy(&b, &ub, 4); __builtin_memcpy(&c, &ACC[i], 4);
            __asm__ volatile("fmadd.s %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(b), "f"(c));
            __builtin_memcpy(&E32[i], &r, 4);
        }
        sf = flags();
        same = 1;
        for (int i = 0; i < N; i++) same &= R32[i] == E32[i];
        check("vfwmaccbf16.vf", same);
        check("vfwmaccbf16.vf flags", vf == sf);
    }
}

// The vector forms need SEW 16: at SEW 32 each is an illegal instruction.
static void reserved(void) {
    static const char* names[] = {"vfwcvtbf16 at SEW 32: illegal", "vfncvtbf16 at SEW 32: illegal",
                                  "vfwmaccbf16 at SEW 32: illegal"};
    for (int k = 0; k < 3; k++) {
        lastCause = 0;
        if (k == 0) __asm__ volatile("vsetivli zero, 4, e32, m1, ta, ma\n.word 0x4b069457" ::: "v8", "v9");
        if (k == 1) __asm__ volatile("vsetivli zero, 4, e32, m1, ta, ma\n.word 0x4b0e9457" ::: "v8");
        if (k == 2) __asm__ volatile("vsetivli zero, 4, e32, m1, ta, ma\n.word 0xef481457" ::: "v8", "v9");
        check(names[k], lastCause == 2);
    }
}

int main(int hart) {
    if (hart) for (;;) {}
    csr_write(mscratch, (uintptr_t)(trapStack + 60));
    csr_write(mtvec, (uintptr_t)handler);
    csr_set(mstatus, (3u << 9) | (3u << 13));  // VS and FS on
    for (int round = 0; round < 4; round++) {
        fill(round);
        for (rm = 0; rm < 5; rm++) {
            __asm__ volatile("fsrm zero, %0" ::"r"(rm));
            scalar();
            vector();
        }
    }
    rm = 0;
    __asm__ volatile("fsrm zero, zero");
    reserved();
    puts_("bf16: ");
    putd((uint32_t)(checks - fails));
    puts_(" of ");
    putd((uint32_t)checks);
    puts_(fails ? " checks passed\n" : " checks passed, all\n");
    return fails;
}
