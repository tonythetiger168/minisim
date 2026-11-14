// zvbb.c - the vector basic bit-manipulation extension (Zvbb 1.0, which
// contains Zvkb): every instruction at SEW 8, 16, 32 and 64 with LMUL 2
// groups and a strip-mined loop, checked against scalar models written from
// the specification; vror.vi with a 6-bit immediate, vwsll's widening, and
// a masked, tail-undisturbed form. clang 18 has no Zvbb intrinsics, so the
// instructions are written in assembly.
#include "rt.h"

void exit(int code) { exit_(code); }

#define N 37
static int fails, checks;
static void check(const char* what, unsigned sew, int ok) {
    checks++;
    if (!ok) { puts_("FAIL "); puts_(what); puts_(" e"); putd(sew); puts_("\n"); fails++; }
}
static uint32_t seed = 7;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 7 ^ seed << 11; }

static uint8_t A[N * 8], B[N * 8], R[N * 16], E[N * 16];
static uint64_t get(const uint8_t* p, unsigned i, unsigned nb) {
    uint64_t v = 0;
    for (unsigned k = 0; k < nb; k++) v |= (uint64_t)p[i * nb + k] << 8 * k;
    return v;
}
static void put(uint8_t* p, unsigned i, unsigned nb, uint64_t v) {
    for (unsigned k = 0; k < nb; k++) p[i * nb + k] = (uint8_t)(v >> 8 * k);
}
static int same(unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) if (R[i] != E[i]) return 0;
    return 1;
}

// The scalar models.
enum { ANDN, ROR, ROL, BREV8, REV8, BREV, CLZ, CTZ, CPOP };
static uint64_t model(int op, uint64_t a, uint64_t b, unsigned sew) {
    const uint64_t m = sew == 64 ? ~0ull : (1ull << sew) - 1;
    unsigned s = (unsigned)(b & (sew - 1));
    uint64_t r = 0;
    switch (op) {
        case ANDN: return a & ~b & m;
        case ROL: s = (sew - s) & (sew - 1); /* fall through */
        case ROR: return s ? ((a >> s) | (a << (sew - s))) & m : a;
        case BREV8:
            for (unsigned i = 0; i < sew; i++) if (a >> i & 1) r |= 1ull << ((i & ~7u) | (7 - (i & 7)));
            return r;
        case REV8:
            for (unsigned i = 0; i < sew / 8; i++) r |= (a >> 8 * i & 0xff) << 8 * (sew / 8 - 1 - i);
            return r;
        case BREV:
            for (unsigned i = 0; i < sew; i++) if (a >> i & 1) r |= 1ull << (sew - 1 - i);
            return r;
        case CLZ: { unsigned n = 0; for (int i = (int)sew - 1; i >= 0 && !(a >> i & 1); i--) n++; return n; }
        case CTZ: { unsigned n = 0; for (unsigned i = 0; i < sew && !(a >> i & 1); i++) n++; return n; }
        case CPOP: { unsigned n = 0; for (unsigned i = 0; i < sew; i++) n += a >> i & 1; return n; }
    }
    return 0;
}
// kind: 0 .vv (b from B), 1 .vx (b = x), 2 .vi (b = imm), 3 unary
static void expect(int op, unsigned sew, int kind, uint64_t x) {
    const unsigned nb = sew / 8;
    if (kind == 1) x = (uint64_t)(int64_t)(intptr_t)(uintptr_t)x;  // x[rs1], sign-extended from XLEN
    for (unsigned i = 0; i < N; i++) {
        uint64_t b = kind == 0 ? get(B, i, nb) : x;
        put(E, i, nb, model(op, get(A, i, nb), b, sew));
    }
}

// One strip-mined loop: v8 = A, v12 = B, the instruction writes v16.
#define LOOP(SEW, INSN, X)                                                                          \
    for (uintptr_t i = 0, vl; i < N; i += vl)                                                          \
        __asm__ volatile("vsetvli %0, %1, e" #SEW ", m2, ta, ma\n"                                  \
                         "vle" #SEW ".v v8, (%2)\n"                                                 \
                         "vle" #SEW ".v v12, (%3)\n" INSN "\n"                                      \
                         "vse" #SEW ".v v16, (%4)"                                                  \
                         : "=&r"(vl)                                                                \
                         : "r"(N - i), "r"(A + i * (SEW / 8)), "r"(B + i * (SEW / 8)),              \
                           "r"(R + i * (SEW / 8)), "r"((uintptr_t)(X))                              \
                         : "memory", "v8", "v9", "v12", "v13", "v16", "v17")

#define T(SEW, OP, KIND, X, INSN, NAME)            \
    do {                                           \
        expect(OP, SEW, KIND, X);                  \
        LOOP(SEW, INSN, X);                        \
        check(NAME, SEW, same(N * (SEW / 8)));     \
    } while (0)

#define ALL(SEW)                                                                   \
    do {                                                                           \
        T(SEW, ANDN, 0, 0, "vandn.vv v16, v8, v12", "vandn.vv");                   \
        T(SEW, ANDN, 1, 0x5a5a5a5a5a5a5a5aull, "vandn.vx v16, v8, %5", "vandn.vx"); \
        T(SEW, ROR, 0, 0, "vror.vv v16, v8, v12", "vror.vv");                      \
        T(SEW, ROR, 1, 13, "vror.vx v16, v8, %5", "vror.vx");                      \
        T(SEW, ROR, 2, 3, "vror.vi v16, v8, 3", "vror.vi 3");                      \
        T(SEW, ROR, 2, 37, "vror.vi v16, v8, 37", "vror.vi 37");                   \
        T(SEW, ROR, 2, 63, "vror.vi v16, v8, 63", "vror.vi 63");                   \
        T(SEW, ROL, 0, 0, "vrol.vv v16, v8, v12", "vrol.vv");                      \
        T(SEW, ROL, 1, 70, "vrol.vx v16, v8, %5", "vrol.vx");                      \
        T(SEW, BREV8, 3, 0, "vbrev8.v v16, v8", "vbrev8.v");                       \
        T(SEW, REV8, 3, 0, "vrev8.v v16, v8", "vrev8.v");                          \
        T(SEW, BREV, 3, 0, "vbrev.v v16, v8", "vbrev.v");                          \
        T(SEW, CLZ, 3, 0, "vclz.v v16, v8", "vclz.v");                             \
        T(SEW, CTZ, 3, 0, "vctz.v v16, v8", "vctz.v");                             \
        T(SEW, CPOP, 3, 0, "vcpop.v v16, v8", "vcpop.v");                          \
    } while (0)

// vwsll: 2*SEW results from SEW sources (m1 in, m2 out), the shift amount
// masked to 2*SEW-1.
static void wexpect(unsigned sew, int kind, uint64_t x) {
    const unsigned nb = sew / 8, w = 2 * sew;
    for (unsigned i = 0; i < N; i++) {
        uint64_t s = (kind == 0 ? get(B, i, nb) : x) & (w - 1);
        uint64_t r = get(A, i, nb) << s;
        put(E, i, 2 * nb, w == 64 ? r : r & ((1ull << w) - 1));
    }
}
#define WLOOP(SEW, W, INSN, X)                                                                      \
    for (uintptr_t i = 0, vl; i < N; i += vl)                                                          \
        __asm__ volatile("vsetvli %0, %1, e" #SEW ", m1, ta, ma\n"                                  \
                         "vle" #SEW ".v v8, (%2)\n"                                                 \
                         "vle" #SEW ".v v12, (%3)\n" INSN "\n"                                      \
                         "vsetvli zero, zero, e" #W ", m2, ta, ma\n"                                \
                         "vse" #W ".v v16, (%4)"                                                    \
                         : "=&r"(vl)                                                                \
                         : "r"(N - i), "r"(A + i * (SEW / 8)), "r"(B + i * (SEW / 8)),              \
                           "r"(R + i * (W / 8)), "r"((uintptr_t)(X))                                \
                         : "memory", "v8", "v12", "v16", "v17")
#define W(SEW, WD)                                                                       \
    do {                                                                                 \
        wexpect(SEW, 0, 0); WLOOP(SEW, WD, "vwsll.vv v16, v8, v12", 0);                  \
        check("vwsll.vv", SEW, same(N * (WD / 8)));                                      \
        wexpect(SEW, 1, 5); WLOOP(SEW, WD, "vwsll.vx v16, v8, %5", 5);                   \
        check("vwsll.vx", SEW, same(N * (WD / 8)));                                      \
        wexpect(SEW, 1, SEW + 3); WLOOP(SEW, WD, "vwsll.vx v16, v8, %5", SEW + 3);       \
        check("vwsll.vx past SEW", SEW, same(N * (WD / 8)));                             \
        wexpect(SEW, 2, 31); WLOOP(SEW, WD, "vwsll.vi v16, v8, 31", 0);                  \
        check("vwsll.vi 31", SEW, same(N * (WD / 8)));                                   \
    } while (0)

int main(int hart) {
    if (hart) for (;;) {}
    csr_set(mstatus, 3u << 9);  // VS on
    for (int i = 0; i < N * 8; i++) { A[i] = (uint8_t)rnd(); B[i] = (uint8_t)rnd(); }
    for (int i = 0; i < 8; i++) A[8 + i] = 0, A[16 + i] = 0xff;  // all-zero and all-one elements (clz, ctz)
    A[24] = 1; A[39] = 0x80;
    ALL(8);
    ALL(16);
    ALL(32);
    ALL(64);
    W(8, 16);
    W(16, 32);
    W(32, 64);
    // Masked and tail-undisturbed: inactive and tail elements keep v16.
    {
        uintptr_t vl;
        uint32_t m = 0x5b5b5b5bu;
        for (unsigned i = 0; i < N; i++) put(R, i, 4, 0xdeadbeef), put(E, i, 4, 0xdeadbeef);
        __asm__ volatile("vsetvli zero, %5, e32, m1, tu, mu\n"
                         "vle32.v v16, (%4)\n"
                         "vsetvli %0, %1, e32, m1, tu, mu\n"
                         "vmv.s.x v0, %2\n"
                         "vle32.v v8, (%3)\n"
                         "vbrev.v v16, v8, v0.t\n"
                         "vsetvli zero, %5, e32, m1, tu, mu\n"
                         "vse32.v v16, (%4)"
                         : "=&r"(vl)
                         : "r"(3), "r"(m), "r"(A), "r"(R), "r"(4)
                         : "memory", "v0", "v8", "v16");
        for (unsigned i = 0; i < 3; i++)
            if (m >> i & 1) put(E, i, 4, model(BREV, get(A, i, 4), 0, 32));
        check("vbrev.v masked, tail undisturbed", 32, same(4 * 4));
    }
    puts_("zvbb: ");
    putd((uint32_t)(checks - fails));
    puts_(" of ");
    putd((uint32_t)checks);
    puts_(fails ? " checks passed\n" : " checks passed, all\n");
    return fails;
}
