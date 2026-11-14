// zvfh.c - vector half precision (Zvfh 1.0): each SEW 16 floating-point
// instruction against the scalar Zfh instruction that computes the same
// thing (itself checked by riscv-tests' rv64uzfh), in all five rounding
// modes, on values that include zeros, infinities, both NaNs, subnormals and
// overflow; results bit for bit and the accrued flags. Also the conversions
// between half and single, half and 8/16/32-bit integers, widening
// arithmetic and the reductions. Built without the vectorizer.
#include <riscv_vector.h>
#include "rt.h"

void exit(int code) { exit_(code); }

typedef _Float16 h16;
#define N 40
static int fails, checks;
static unsigned rm;
static void check(const char* what, int ok) {
    checks++;
    if (!ok) { puts_("FAIL "); puts_(what); puts_(" rm "); putd(rm); puts_("\n"); fails++; }
}
static uint32_t seed = 3;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 7 ^ seed << 11; }

static uint16_t A[N], B[N], C[N], R[N], E[N];
static uint32_t RW[N], EW[N], S32[N];
static int8_t I8[N];
static uint8_t R8[N], E8[N];
static const uint16_t special[] = {0x0000, 0x8000, 0x7c00, 0xfc00, 0x7e00, 0x7d00 /* sNaN */, 0x0001, 0x83ff,
                                   0x7bff, 0xfbff, 0x3c00, 0xbc00, 0x0400, 0x3555, 0x7800, 0x5bff};

static h16 hf(uint16_t b) { h16 v; __builtin_memcpy(&v, &b, 2); return v; }
static uint16_t hb(h16 v) { uint16_t b; __builtin_memcpy(&b, &v, 2); return b; }
static float sf(uint32_t b) { float v; __builtin_memcpy(&v, &b, 4); return v; }
static uint32_t sb(float v) { uint32_t b; __builtin_memcpy(&b, &v, 4); return b; }
static unsigned flags(void) { unsigned f; __asm__ volatile("csrrw %0, fflags, zero" : "=r"(f) :: "memory"); return f; }
// The flags once `dep` has been computed (a register result is not ordered by a memory clobber).
static unsigned flagsAfter(uint32_t dep) {
    unsigned f;
    __asm__ volatile("csrrw %0, fflags, zero" : "=r"(f) : "r"(dep) : "memory");
    return f;
}

static int same(void) { for (int i = 0; i < N; i++) if (R[i] != E[i]) return 0; return 1; }
static int sameW(void) { for (int i = 0; i < N; i++) if (RW[i] != EW[i]) return 0; return 1; }

// Run a vector body over N elements strip-mined at e16 m1 / e32 m2, then the
// scalar model, and compare the results and flags.
#define VEC16(BODY) do { flags(); for (size_t i = 0, vl; i < N; i += vl) { vl = __riscv_vsetvl_e16m1(N - i); BODY; } } while (0)
#define CMP(NAME, SAMEFN, ...)                                      \
    do {                                                            \
        unsigned vf = flags();                                      \
        for (int i = 0; i < N; i++) { __VA_ARGS__; }                \
        unsigned sf_ = flags();                                     \
        check(NAME, SAMEFN());                                      \
        check(NAME " flags", vf == sf_);                            \
    } while (0)
#define LD(X) __riscv_vle16_v_f16m1((const h16*)(X) + i, vl)
#define ST(V) __riscv_vse16_v_f16m1((h16*)R + i, V, vl)

#define BIN(NAME, VOP, ...)                                                       \
    do {                                                                          \
        VEC16(ST(VOP(LD(A), LD(B), vl)));                                         \
        CMP(NAME, same, h16 a = hf(A[i]); h16 b = hf(B[i]); E[i] = hb(__VA_ARGS__)); \
    } while (0)

static h16 fma16(h16 a, h16 b, h16 c) { h16 r; __asm__ volatile("fmadd.h %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(b), "f"(c)); return r; }
static h16 fnmsub16(h16 a, h16 b, h16 c) { h16 r; __asm__ volatile("fnmsub.h %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(b), "f"(c)); return r; }
static h16 sqrt16(h16 a) { h16 r; __asm__ volatile("fsqrt.h %0, %1" : "=f"(r) : "f"(a)); return r; }
static h16 min16(h16 a, h16 b) { h16 r; __asm__ volatile("fmin.h %0, %1, %2" : "=f"(r) : "f"(a), "f"(b)); return r; }
static h16 max16(h16 a, h16 b) { h16 r; __asm__ volatile("fmax.h %0, %1, %2" : "=f"(r) : "f"(a), "f"(b)); return r; }
static h16 sgnjn16(h16 a, h16 b) { h16 r; __asm__ volatile("fsgnjn.h %0, %1, %2" : "=f"(r) : "f"(a), "f"(b)); return r; }
static h16 sgnjx16(h16 a, h16 b) { h16 r; __asm__ volatile("fsgnjx.h %0, %1, %2" : "=f"(r) : "f"(a), "f"(b)); return r; }
static unsigned feq16(h16 a, h16 b) { unsigned r; __asm__ volatile("feq.h %0, %1, %2" : "=r"(r) : "f"(a), "f"(b)); return r; }
static unsigned flt16(h16 a, h16 b) { unsigned r; __asm__ volatile("flt.h %0, %1, %2" : "=r"(r) : "f"(a), "f"(b)); return r; }
static unsigned fle16(h16 a, h16 b) { unsigned r; __asm__ volatile("fle.h %0, %1, %2" : "=r"(r) : "f"(a), "f"(b)); return r; }
static unsigned fclass16(h16 a) { unsigned r; __asm__ volatile("fclass.h %0, %1" : "=r"(r) : "f"(a)); return r; }
static int32_t cvtw16(h16 a) { int32_t r; __asm__ volatile("fcvt.w.h %0, %1" : "=r"(r) : "f"(a)); return r; }
static uint32_t cvtwu16(h16 a) { uint32_t r; __asm__ volatile("fcvt.wu.h %0, %1" : "=r"(r) : "f"(a)); return r; }
static int32_t cvtwrtz16(h16 a) { int32_t r; __asm__ volatile("fcvt.w.h %0, %1, rtz" : "=r"(r) : "f"(a)); return r; }
static h16 cvthw(int32_t x) { h16 r; __asm__ volatile("fcvt.h.w %0, %1" : "=f"(r) : "r"(x)); return r; }
static h16 cvthwu(uint32_t x) { h16 r; __asm__ volatile("fcvt.h.wu %0, %1" : "=f"(r) : "r"(x)); return r; }
static float cvtsh(h16 a) { float r; __asm__ volatile("fcvt.s.h %0, %1" : "=f"(r) : "f"(a)); return r; }
static h16 cvths(float a) { h16 r; __asm__ volatile("fcvt.h.s %0, %1" : "=f"(r) : "f"(a)); return r; }
// The scalar saturating conversions to narrow integers: through 32 bits, then clamped (NV on overflow).
static unsigned nv;
static int32_t sat(int32_t v, int32_t lo, int32_t hi) { if (v < lo) { nv = 1; return lo; } if (v > hi) { nv = 1; return hi; } return v; }

static void fill(int round) {
    for (int i = 0; i < N; i++) {
        A[i] = (uint16_t)rnd(); B[i] = (uint16_t)rnd(); C[i] = (uint16_t)rnd();
        S32[i] = rnd();
        I8[i] = (int8_t)rnd();
        if ((rnd() & 3) == 0) A[i] = special[rnd() % 16];
        if ((rnd() & 3) == 0) B[i] = special[rnd() % 16];
        if ((rnd() & 7) == 0) C[i] = special[rnd() % 16];
        if (round & 1) { A[i] = (A[i] & 0x83ff) | 0x3800 | (rnd() & 0x0c00); B[i] = (B[i] & 0x83ff) | 0x3800; }  // near 1
        if ((rnd() & 3) == 0) S32[i] = sb((float)(int)(rnd() % 200000 - 100000) / 3.0f);
    }
    A[0] = 0x7d00; B[1] = 0x7d00; A[2] = 0x7c00; B[2] = 0; A[3] = 0x7bff; B[3] = 0x7bff;  // sNaN, inf*0, overflow
}

static void run(void) {
    BIN("vfadd", __riscv_vfadd_vv_f16m1, a + b);
    BIN("vfsub", __riscv_vfsub_vv_f16m1, a - b);
    BIN("vfmul", __riscv_vfmul_vv_f16m1, a * b);
    BIN("vfdiv", __riscv_vfdiv_vv_f16m1, a / b);
    BIN("vfmin", __riscv_vfmin_vv_f16m1, min16(a, b));
    BIN("vfmax", __riscv_vfmax_vv_f16m1, max16(a, b));
    BIN("vfsgnjn", __riscv_vfsgnjn_vv_f16m1, sgnjn16(a, b));
    BIN("vfsgnjx", __riscv_vfsgnjx_vv_f16m1, sgnjx16(a, b));
    // .vf with the scalar from f[rs1]
    VEC16(ST(__riscv_vfmul_vf_f16m1(LD(A), hf(B[0x11]), vl)));
    CMP("vfmul.vf", same, E[i] = hb(hf(A[i]) * hf(B[0x11])));
    VEC16(ST(__riscv_vfrdiv_vf_f16m1(LD(A), hf(B[5]), vl)));
    CMP("vfrdiv.vf", same, E[i] = hb(hf(B[5]) / hf(A[i])));
    VEC16(ST(__riscv_vfsqrt_v_f16m1(LD(A), vl)));
    CMP("vfsqrt", same, E[i] = hb(sqrt16(hf(A[i]))));
    // vd = vs1 * vs2 + vd and vd = -(vs1 * vs2) + vd
    VEC16(ST(__riscv_vfmacc_vv_f16m1(LD(C), LD(A), LD(B), vl)));
    CMP("vfmacc", same, E[i] = hb(fma16(hf(A[i]), hf(B[i]), hf(C[i]))));
    VEC16(ST(__riscv_vfnmsac_vv_f16m1(LD(C), LD(A), LD(B), vl)));
    CMP("vfnmsac", same, E[i] = hb(fnmsub16(hf(A[i]), hf(B[i]), hf(C[i]))));
    // vd = vs1 * vd + vs2
    VEC16(ST(__riscv_vfmadd_vv_f16m1(LD(C), LD(A), LD(B), vl)));
    CMP("vfmadd", same, E[i] = hb(fma16(hf(A[i]), hf(C[i]), hf(B[i]))));
    // compares and classify (into R as 0/1 and the class bits)
    VEC16(ST(__riscv_vreinterpret_v_u16m1_f16m1(__riscv_vmerge_vxm_u16m1(__riscv_vmv_v_x_u16m1(0, vl), 1,
                                                    __riscv_vmflt_vv_f16m1_b16(LD(A), LD(B), vl), vl))));
    CMP("vmflt", same, E[i] = (uint16_t)flt16(hf(A[i]), hf(B[i])));
    VEC16(ST(__riscv_vreinterpret_v_u16m1_f16m1(__riscv_vmerge_vxm_u16m1(__riscv_vmv_v_x_u16m1(0, vl), 1,
                                                    __riscv_vmfle_vv_f16m1_b16(LD(A), LD(B), vl), vl))));
    CMP("vmfle", same, E[i] = (uint16_t)fle16(hf(A[i]), hf(B[i])));
    VEC16(ST(__riscv_vreinterpret_v_u16m1_f16m1(__riscv_vmerge_vxm_u16m1(__riscv_vmv_v_x_u16m1(0, vl), 1,
                                                    __riscv_vmfeq_vv_f16m1_b16(LD(A), LD(B), vl), vl))));
    CMP("vmfeq", same, E[i] = (uint16_t)feq16(hf(A[i]), hf(B[i])));
    VEC16(ST(__riscv_vreinterpret_v_u16m1_f16m1(__riscv_vfclass_v_u16m1(LD(A), vl))));
    CMP("vfclass", same, E[i] = (uint16_t)fclass16(hf(A[i])));
    // half <-> 16-bit integers (saturating)
    VEC16(ST(__riscv_vreinterpret_v_i16m1_f16m1(__riscv_vfcvt_x_f_v_i16m1(LD(A), vl))));
    nv = 0;
    CMP("vfcvt.x.f.v", same, { int32_t v = cvtw16(hf(A[i])); unsigned f = flags(); nv = 0;
                               v = sat(v, -32768, 32767); __asm__ volatile("csrs fflags, %0" :: "r"(nv ? 16u : f)); E[i] = (uint16_t)v; });
    VEC16(ST(__riscv_vreinterpret_v_u16m1_f16m1(__riscv_vfcvt_xu_f_v_u16m1(LD(A), vl))));
    CMP("vfcvt.xu.f.v", same, { uint32_t v = cvtwu16(hf(A[i])); unsigned f = flags(); nv = 0;
                                if (v > 65535) { v = 65535; nv = 1; } __asm__ volatile("csrs fflags, %0" :: "r"(nv ? 16u : f)); E[i] = (uint16_t)v; });
    VEC16(ST(__riscv_vfcvt_f_x_v_f16m1(__riscv_vle16_v_i16m1((const int16_t*)A + i, vl), vl)));
    CMP("vfcvt.f.x.v", same, E[i] = hb(cvthw((int16_t)A[i])));
    VEC16(ST(__riscv_vfcvt_f_xu_v_f16m1(__riscv_vle16_v_u16m1(A + i, vl), vl)));
    CMP("vfcvt.f.xu.v", same, E[i] = hb(cvthwu(A[i])));
    // half -> single (exact) and single -> half (rounded, and rounded to odd)
    VEC16(__riscv_vse32_v_f32m2((float*)RW + i, __riscv_vfwcvt_f_f_v_f32m2(LD(A), vl), vl));
    CMP("vfwcvt.f.f.v", sameW, EW[i] = sb(cvtsh(hf(A[i]))));
    VEC16(ST(__riscv_vfncvt_f_f_w_f16m1(__riscv_vle32_v_f32m2((const float*)S32 + i, vl), vl)));
    CMP("vfncvt.f.f.w", same, E[i] = hb(cvths(sf(S32[i]))));
    // half -> int32 widening, int8 -> half widening, half -> int8 narrowing
    VEC16(__riscv_vse32_v_i32m2((int32_t*)RW + i, __riscv_vfwcvt_rtz_x_f_v_i32m2(LD(A), vl), vl));
    CMP("vfwcvt.rtz.x.f.v", sameW, EW[i] = (uint32_t)cvtwrtz16(hf(A[i])));
    VEC16(ST(__riscv_vfwcvt_f_x_v_f16m1(__riscv_vle8_v_i8mf2(I8 + i, vl), vl)));
    CMP("vfwcvt.f.x.v (int8)", same, E[i] = hb(cvthw(I8[i])));
    flags();
    for (size_t i = 0, vl; i < N; i += vl) {
        vl = __riscv_vsetvl_e16m1(N - i);
        __riscv_vse8_v_i8mf2((int8_t*)R8 + i, __riscv_vfncvt_x_f_w_i8mf2(LD(A), vl), vl);
    }
    {
        unsigned vf = flags(), acc = 0;
        for (int i = 0; i < N; i++) {
            int32_t v = cvtw16(hf(A[i]));
            unsigned f = flags();
            nv = 0;
            v = sat(v, -128, 127);
            acc |= nv ? 16u : f;
            E8[i] = (uint8_t)v;
        }
        int ok = 1;
        for (int i = 0; i < N; i++) ok &= R8[i] == E8[i];
        check("vfncvt.x.f.w (int8)", ok);
        check("vfncvt.x.f.w (int8) flags", vf == acc);
    }
    // widening arithmetic: products and sums in single
    VEC16(__riscv_vse32_v_f32m2((float*)RW + i, __riscv_vfwmul_vv_f32m2(LD(A), LD(B), vl), vl));
    CMP("vfwmul", sameW, EW[i] = sb(cvtsh(hf(A[i])) * cvtsh(hf(B[i]))));
    VEC16(__riscv_vse32_v_f32m2((float*)RW + i, __riscv_vfwadd_vv_f32m2(LD(A), LD(B), vl), vl));
    CMP("vfwadd", sameW, EW[i] = sb(cvtsh(hf(A[i])) + cvtsh(hf(B[i]))));
    // the 7-bit estimates at SEW 16: 1/1 and 1/sqrt(1) are both 127/128
    {
        size_t vl = __riscv_vsetvl_e16m1(1);
        vfloat16m1_t one = __riscv_vfmv_v_f_f16m1(hf(0x3c00), vl);
        uint16_t r7 = hb(__riscv_vfmv_f_s_f16m1_f16(__riscv_vfrec7_v_f16m1(one, vl)));
        uint16_t q7 = hb(__riscv_vfmv_f_s_f16m1_f16(__riscv_vfrsqrt7_v_f16m1(one, vl)));
        check("vfrec7/vfrsqrt7 at SEW 16", r7 == 0x3bf8 && q7 == 0x3bf8);
    }
    // ordered sum and max reductions (one vl = N group at m4)
    {
        size_t vl = __riscv_vsetvl_e16m4(N);
        flags();
        vfloat16m1_t z = __riscv_vfmv_s_f_f16m1(hf(C[7]), 1);
        h16 rs = __riscv_vfmv_f_s_f16m1_f16(__riscv_vfredosum_vs_f16m4_f16m1(__riscv_vle16_v_f16m4((const h16*)A, vl), z, vl));
        unsigned vf = flagsAfter(hb(rs));
        h16 s = hf(C[7]);
        for (size_t i = 0; i < vl; i++) s = s + hf(A[i]);
        check("vfredosum", hb(rs) == hb(s) && vf == flagsAfter(hb(s)));
        vfloat32m1_t zw = __riscv_vfmv_s_f_f32m1(1.0f, 1);
        float ws = __riscv_vfmv_f_s_f32m1_f32(__riscv_vfwredosum_vs_f16m4_f32m1(__riscv_vle16_v_f16m4((const h16*)B, vl), zw, vl));
        vf = flagsAfter(sb(ws));
        float t = 1.0f;
        for (size_t i = 0; i < vl; i++) t = t + cvtsh(hf(B[i]));
        check("vfwredosum", sb(ws) == sb(t) && vf == flagsAfter(sb(t)));
        h16 mx = __riscv_vfmv_f_s_f16m1_f16(__riscv_vfredmax_vs_f16m4_f16m1(__riscv_vle16_v_f16m4((const h16*)B, vl), z, vl));
        vf = flagsAfter(hb(mx));
        h16 m = hf(C[7]);
        for (size_t i = 0; i < vl; i++) m = max16(m, hf(B[i]));
        check("vfredmax", hb(mx) == hb(m) && vf == flagsAfter(hb(m)));
    }
}

// Kept out of main, whose prologue would otherwise save FP registers
// before the FPU is on.
__attribute__((noinline)) static int all(void) {
    for (int round = 0; round < 4; round++) {
        fill(round);
        for (rm = 0; rm < 5; rm++) {
            __asm__ volatile("fsrm zero, %0" :: "r"(rm));
            run();
        }
    }
    puts_("zvfh: ");
    putd((uint32_t)(checks - fails));
    puts_(" of ");
    putd((uint32_t)checks);
    puts_(fails ? " checks passed\n" : " checks passed, all\n");
    return fails;
}

int main(int hart) {
    if (hart) for (;;) {}
    csr_set(mstatus, (3u << 9) | (3u << 13));  // VS and FS on
    return all();
}
