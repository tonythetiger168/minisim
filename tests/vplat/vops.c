// vops.c - V extension instructions a loop vectorizer seldom emits, each
// checked against a scalar model written from the specification (RISC-V V
// 1.0): slides, gathers, compress, the mask instructions, fixed point with
// every rounding mode and saturation, segment, strided and indexed
// accesses, fault-only-first loads, vstart after a trap, vsetvl corner
// cases, carries, the widening multiply-adds and the floating-point
// estimates, conversions and reductions. The scalar models are compiled
// without the vectorizer (build.sh passes -fno-vectorize).
#include <riscv_vector.h>
#include "rt.h"

void exit(int code) { exit_(code); }

#define N 37
static int fails;
static void check(const char* what, int ok) {
    if (!ok) { puts_("FAIL "); puts_(what); puts_("\n"); fails++; }
}
static uint32_t seed = 1;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 7 ^ seed << 11; }

static uint8_t a8[N * 8], b8[N * 8], r8[N * 8], e8[N * 8];
static uint16_t a16[N * 8], b16[N], r16[N * 8], e16[N * 8];
static uint32_t a32[N], b32[N], r32[N], e32[N];
static uint64_t r64[N], e64[N];
static float af[N], rf[N], ef[N];
static double rd[N], ed[N];

static void fill(void) {
    for (int i = 0; i < N * 8; i++) { a8[i] = (uint8_t)rnd(); b8[i] = (uint8_t)rnd(); a16[i] = (uint16_t)rnd(); }
    for (int i = 0; i < N; i++) {
        b16[i] = (uint16_t)rnd(); a32[i] = rnd(); b32[i] = rnd();
        af[i] = (float)(int)(rnd() % 4000 - 2000) / 16.0f;
    }
    a8[3] = 0x80; b8[3] = 0x80; a8[4] = 0x7f; b8[4] = 0x7f; a8[5] = 0; b8[5] = 0xff;  // saturation corners
}
static int same8(int n) { for (int i = 0; i < n; i++) if (r8[i] != e8[i]) return 0; return 1; }
static int same16(int n) { for (int i = 0; i < n; i++) if (r16[i] != e16[i]) return 0; return 1; }
static int same32(int n) { for (int i = 0; i < n; i++) if (r32[i] != e32[i]) return 0; return 1; }
static int same64(int n) { for (int i = 0; i < n; i++) if (r64[i] != e64[i]) return 0; return 1; }

// ---------------------------------------------------------------- vsetvl
static void t_vsetvl(void) {
    size_t vlmax = __riscv_vsetvlmax_e32m1();
    check("vlenb", vlmax == csr_read(vlenb) / 4);
    check("AVL above VLMAX gives VLMAX", __riscv_vsetvl_e32m1(1000) == vlmax);
    check("AVL 3", __riscv_vsetvl_e32m1(3) == 3);
    check("e8 m8 VLMAX", __riscv_vsetvlmax_e8m8() == csr_read(vlenb) * 8);
    check("e64 mf... e16 mf4", __riscv_vsetvlmax_e16mf4() == csr_read(vlenb) / 8);
    size_t vl;
    __asm__ volatile("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(vl) : "r"(5));
    uintptr_t vt = csr_read(vtype), vln = csr_read(vl);
    check("vtype e32 m1 ta ma", vt == 0xd0 && vln == vl && vl == (vlmax < 5 ? vlmax : 5));
    __asm__ volatile("vsetivli %0, 7, e16, m2, tu, mu" : "=r"(vl));
    check("vsetivli", vl == 7 && csr_read(vtype) == 0x09);
    // SEW 128 is reserved: vill, vl 0.
    uintptr_t bad = 4u << 3;
    __asm__ volatile("vsetvl %0, %1, %2" : "=r"(vl) : "r"(4), "r"(bad));
    check("a reserved vtype sets vill", vl == 0 && csr_read(vl) == 0 && csr_read(vtype) == (uintptr_t)1 << (8 * sizeof(uintptr_t) - 1));
    // e64 with mf8 needs ELEN 512: reserved too.
    __asm__ volatile("vsetvli %0, %1, e64, mf8, ta, ma" : "=r"(vl) : "r"(4));
    check("e64 mf8 is reserved", vl == 0 && (csr_read(vtype) >> (8 * sizeof(uintptr_t) - 1)));
    // rs1 = x0, rd != x0: VLMAX.
    __asm__ volatile("vsetvli %0, x0, e8, m1, ta, ma" : "=r"(vl));
    check("x0 AVL means VLMAX", vl == csr_read(vlenb));
    // rs1 = rd = x0 keeps vl when SEW/LMUL stays.
    __asm__ volatile("vsetvli x0, %0, e8, m1, ta, ma\n vsetvli x0, x0, e16, m2, ta, ma" ::"r"(9));
    check("x0, x0 keeps vl", csr_read(vl) == 9 && csr_read(vtype) == 0xc9);
    csr_write(vxrm, 2);
    csr_write(vxsat, 1);
    check("vcsr", csr_read(vcsr) == 5);
    csr_write(vcsr, 2);
    check("vcsr writes vxrm and vxsat", csr_read(vxrm) == 1 && csr_read(vxsat) == 0);
}

// ---------------------------------------------------------------- permutes
static void t_permute(void) {
    size_t vlmax = __riscv_vsetvlmax_e32m2();
    size_t n = vlmax < N ? vlmax : N, vl = __riscv_vsetvl_e32m2(n);
    vuint32m2_t a = __riscv_vle32_v_u32m2(a32, vl), b = __riscv_vle32_v_u32m2(b32, vl);
    for (size_t off = 0; off < 5; off++) {
        __riscv_vse32_v_u32m2(r32, __riscv_vslideup_vx_u32m2(b, a, off, vl), vl);
        for (size_t i = 0; i < vl; i++) e32[i] = i < off ? b32[i] : a32[i - off];
        check("vslideup.vx", same32(vl));
        __riscv_vse32_v_u32m2(r32, __riscv_vslidedown_vx_u32m2(a, off, vl), vl);
        // elements of a past vl are its tail: undisturbed, so not compared
        int ok = 1;
        for (size_t i = 0; i < vl; i++)
            if (i + off < vl ? r32[i] != a32[i + off] : i + off >= vlmax && r32[i] != 0) ok = 0;
        check("vslidedown.vx", ok);
    }
    __riscv_vse32_v_u32m2(r32, __riscv_vslidedown_vx_u32m2(a, (size_t)-3, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = 0;
    check("vslidedown past the end", same32(vl));
    __riscv_vse32_v_u32m2(r32, __riscv_vslide1up_vx_u32m2(a, 0xabcdu, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = i ? a32[i - 1] : 0xabcdu;
    check("vslide1up", same32(vl));
    __riscv_vse32_v_u32m2(r32, __riscv_vslide1down_vx_u32m2(a, 0x1234u, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = i + 1 < vl ? a32[i + 1] : 0x1234u;
    check("vslide1down", same32(vl));
    // gathers: indices beyond VLMAX read 0
    for (size_t i = 0; i < vl; i++) b32[i] = rnd() % (vlmax + 3);
    vuint32m2_t ix = __riscv_vle32_v_u32m2(b32, vl);
    __riscv_vse32_v_u32m2(r32, __riscv_vrgather_vv_u32m2(a, ix, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = b32[i] < vlmax ? (b32[i] < vl ? a32[b32[i]] : r32[i]) : 0;
    // elements of a past vl are a's tail: undisturbed, so read them back
    int ok = 1;
    for (size_t i = 0; i < vl; i++) if (b32[i] < vl && r32[i] != e32[i]) ok = 0; else if (b32[i] >= vlmax && r32[i]) ok = 0;
    check("vrgather.vv", ok);
    __riscv_vse32_v_u32m2(r32, __riscv_vrgather_vx_u32m2(a, 2, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[2];
    check("vrgather.vx", same32(vl));
    __riscv_vse32_v_u32m2(r32, __riscv_vrgather_vx_u32m2(a, vlmax, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = 0;
    check("vrgather.vx past VLMAX", same32(vl));
    for (size_t i = 0; i < vl; i++) a16[i] = (uint16_t)(rnd() % vl);
    __riscv_vse32_v_u32m2(r32, __riscv_vrgatherei16_vv_u32m2(a, __riscv_vle16_v_u16m1(a16, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[a16[i]];
    check("vrgatherei16", same32(vl));
    // compress, iota, id, cpop, first, sbf/sif/sof
    vbool16_t m = __riscv_vmsgtu_vx_u32m2_b16(a, 0x80000000u, vl);
    for (size_t i = 0; i < vl; i++) r32[i] = 0x5555;
    vuint32m2_t dst = __riscv_vle32_v_u32m2(r32, vl);
    __riscv_vse32_v_u32m2(r32, __riscv_vcompress_vm_u32m2_tu(dst, a, m, vl), vl);
    size_t k = 0;
    for (size_t i = 0; i < vl; i++) if (a32[i] > 0x80000000u) e32[k++] = a32[i];
    for (size_t i = k; i < vl; i++) e32[i] = 0x5555;
    check("vcompress", same32(vl));
    check("vcpop", __riscv_vcpop_m_b16(m, vl) == k);
    long first = -1;
    for (size_t i = 0; i < vl; i++) if (a32[i] > 0x80000000u) { first = (long)i; break; }
    check("vfirst", __riscv_vfirst_m_b16(m, vl) == first);
    check("vfirst of nothing", __riscv_vfirst_m_b16(__riscv_vmclr_m_b16(vl), vl) == -1);
    __riscv_vse32_v_u32m2(r32, __riscv_viota_m_u32m2(m, vl), vl);
    k = 0;
    for (size_t i = 0; i < vl; i++) { e32[i] = (uint32_t)k; if (a32[i] > 0x80000000u) k++; }
    check("viota", same32(vl));
    __riscv_vse32_v_u32m2(r32, __riscv_vid_v_u32m2(vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = (uint32_t)i;
    check("vid", same32(vl));
    vuint32m2_t one = __riscv_vmv_v_x_u32m2(1, vl), zero = __riscv_vmv_v_x_u32m2(0, vl);
    __riscv_vse32_v_u32m2(r32, __riscv_vmerge_vvm_u32m2(zero, one, __riscv_vmsbf_m_b16(m, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = first < 0 || (long)i < first;
    check("vmsbf", same32(vl));
    __riscv_vse32_v_u32m2(r32, __riscv_vmerge_vvm_u32m2(zero, one, __riscv_vmsif_m_b16(m, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = first < 0 || (long)i <= first;
    check("vmsif", same32(vl));
    __riscv_vse32_v_u32m2(r32, __riscv_vmerge_vvm_u32m2(zero, one, __riscv_vmsof_m_b16(m, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = (long)i == first;
    check("vmsof", same32(vl));
    // vmv.s.x, vmv.x.s sign-extend
    vint8m1_t s8 = __riscv_vmv_s_x_i8m1(-128, 1);
    check("vmv.x.s sign-extends", __riscv_vmv_x_s_i8m1_i8(s8) == -128);
    __riscv_vsetvl_e8m1(1);
    long xv;
    __asm__ volatile("vmv.x.s %0, %1" : "=r"(xv) : "vr"(s8));
    check("vmv.x.s writes XLEN bits", xv == -128);
    // whole-register moves
    vuint8m4_t w4 = __riscv_vle8_v_u8m4(a8, __riscv_vsetvlmax_e8m4());
    vuint8m4_t c4;
    __asm__ volatile("vmv4r.v %0, %1" : "=vr"(c4) : "vr"(w4));
    __riscv_vse8_v_u8m4(r8, c4, __riscv_vsetvlmax_e8m4());
    size_t nb = __riscv_vsetvlmax_e8m4();
    if (nb > N * 8) nb = N * 8;
    for (size_t i = 0; i < nb; i++) e8[i] = a8[i];
    check("vmv4r.v", same8((int)nb));
}

// ---------------------------------------------------------------- fixed point
static int64_t rshift_round(int64_t v, unsigned d, unsigned rm) {  // signed v >> d rounded by vxrm
    if (!d) return v;
    uint64_t u = (uint64_t)v;
    uint64_t bit = (u >> (d - 1)) & 1, rest = d > 1 ? u & ((1ull << (d - 1)) - 1) : 0, lsb = (u >> d) & 1;
    uint64_t r = rm == 0 ? bit : rm == 1 ? bit & ((rest != 0) | lsb) : rm == 2 ? 0 : (!lsb && (bit | (rest != 0)));
    return (v >> d) + (int64_t)r;
}
static uint64_t urshift_round(uint64_t u, unsigned d, unsigned rm) {
    if (!d) return u;
    uint64_t bit = (u >> (d - 1)) & 1, rest = d > 1 ? u & ((1ull << (d - 1)) - 1) : 0, lsb = (u >> d) & 1;
    uint64_t r = rm == 0 ? bit : rm == 1 ? bit & ((rest != 0) | lsb) : rm == 2 ? 0 : (!lsb && (bit | (rest != 0)));
    return (u >> d) + r;
}
// The fixed-point checks for one rounding mode (the intrinsics want a constant).
#define FIXED_RM(RM) do { const unsigned rm = RM; \
    __riscv_vse8_v_i8m1((int8_t*)r8, __riscv_vaadd_vv_i8m1(a, b, RM, vl), vl); \
    for (size_t i = 0; i < vl; i++) e8[i] = (uint8_t)rshift_round((int8_t)a8[i] + (int8_t)b8[i], 1, rm); \
    check("vaadd", same8((int)vl)); \
    __riscv_vse8_v_u8m1(r8, __riscv_vaaddu_vv_u8m1(ua, ub, RM, vl), vl); \
    for (size_t i = 0; i < vl; i++) e8[i] = (uint8_t)urshift_round((uint64_t)a8[i] + b8[i], 1, rm); \
    check("vaaddu", same8((int)vl)); \
    __riscv_vse8_v_i8m1((int8_t*)r8, __riscv_vasub_vx_i8m1(a, -77, RM, vl), vl); \
    for (size_t i = 0; i < vl; i++) e8[i] = (uint8_t)rshift_round((int8_t)a8[i] + 77, 1, rm); \
    check("vasub.vx", same8((int)vl)); \
    __riscv_vse8_v_u8m1(r8, __riscv_vasubu_vv_u8m1(ua, ub, RM, vl), vl); \
    for (size_t i = 0; i < vl; i++) e8[i] = (uint8_t)rshift_round((int64_t)a8[i] - b8[i], 1, rm); \
    check("vasubu", same8((int)vl)); \
    csr_write(vxsat, 0); \
    __riscv_vse8_v_i8m1((int8_t*)r8, __riscv_vsmul_vv_i8m1(a, b, RM, vl), vl); \
    sat = 0; \
    for (size_t i = 0; i < vl; i++) { \
        int64_t p = rshift_round((int64_t)(int8_t)a8[i] * (int8_t)b8[i], 7, rm); \
        if (p > 127) { p = 127; sat = 1; } \
        if (p < -128) { p = -128; sat = 1; } \
        e8[i] = (uint8_t)p; \
    } \
    check("vsmul", same8((int)vl) && csr_read(vxsat) == (uintptr_t)sat); \
    __riscv_vse8_v_u8m1(r8, __riscv_vssrl_vv_u8m1(ua, ub, RM, vl), vl); \
    for (size_t i = 0; i < vl; i++) e8[i] = (uint8_t)urshift_round(a8[i], b8[i] & 7, rm); \
    check("vssrl", same8((int)vl)); \
    __riscv_vse8_v_i8m1((int8_t*)r8, __riscv_vssra_vx_i8m1(a, 3, RM, vl), vl); \
    for (size_t i = 0; i < vl; i++) e8[i] = (uint8_t)rshift_round((int8_t)a8[i], 3, rm); \
    check("vssra", same8((int)vl)); \
 \
    vint16m2_t w = __riscv_vle16_v_i16m2((int16_t*)a16, vl); \
    csr_write(vxsat, 0); \
    __riscv_vse8_v_i8m1((int8_t*)r8, __riscv_vnclip_wx_i8m1(w, 5, RM, vl), vl); \
    sat = 0; \
    for (size_t i = 0; i < vl; i++) { \
        int64_t p = rshift_round((int16_t)a16[i], 5, rm); \
        if (p > 127) { p = 127; sat = 1; } \
        if (p < -128) { p = -128; sat = 1; } \
        e8[i] = (uint8_t)p; \
    } \
    check("vnclip", same8((int)vl) && csr_read(vxsat) == (uintptr_t)sat); \
    __riscv_vse8_v_u8m1(r8, __riscv_vnclipu_wv_u8m1(__riscv_vreinterpret_v_i16m2_u16m2(w), ub, RM, vl), vl); \
    for (size_t i = 0; i < vl; i++) { \
        uint64_t p = urshift_round(a16[i], b8[i] & 15, rm); \
        e8[i] = (uint8_t)(p > 255 ? 255 : p); \
    } \
    check("vnclipu", same8((int)vl)); \
} while (0)
static void t_fixed(void) {
    size_t vl = __riscv_vsetvl_e8m1(N);
    vint8m1_t a = __riscv_vle8_v_i8m1((int8_t*)a8, vl), b = __riscv_vle8_v_i8m1((int8_t*)b8, vl);
    vuint8m1_t ua = __riscv_vle8_v_u8m1(a8, vl), ub = __riscv_vle8_v_u8m1(b8, vl);
    int sat;
    csr_write(vxsat, 0);
    __riscv_vse8_v_i8m1((int8_t*)r8, __riscv_vsadd_vv_i8m1(a, b, vl), vl);
    sat = 0;
    for (size_t i = 0; i < vl; i++) {
        int s = (int8_t)a8[i] + (int8_t)b8[i];
        if (s > 127) { s = 127; sat = 1; } else if (s < -128) { s = -128; sat = 1; }
        e8[i] = (uint8_t)s;
    }
    check("vsadd", same8((int)vl) && csr_read(vxsat) == (uintptr_t)sat);
    csr_write(vxsat, 0);
    __riscv_vse8_v_u8m1(r8, __riscv_vssubu_vv_u8m1(ua, ub, vl), vl);
    for (size_t i = 0; i < vl; i++) e8[i] = a8[i] > b8[i] ? (uint8_t)(a8[i] - b8[i]) : 0;
    check("vssubu", same8((int)vl) && csr_read(vxsat) == 1);
    csr_write(vxsat, 0);
    __riscv_vse8_v_i8m1((int8_t*)r8, __riscv_vssub_vx_i8m1(a, 100, vl), vl);
    for (size_t i = 0; i < vl; i++) { int s = (int8_t)a8[i] - 100; e8[i] = (uint8_t)(s < -128 ? -128 : s); }
    check("vssub.vx", same8((int)vl) && csr_read(vxsat) == 1);
    __riscv_vse8_v_u8m1(r8, __riscv_vsaddu_vx_u8m1(ua, 200, vl), vl);
    for (size_t i = 0; i < vl; i++) { int s = a8[i] + 200; e8[i] = (uint8_t)(s > 255 ? 255 : s); }
    check("vsaddu.vx", same8((int)vl));
    FIXED_RM(0);
    FIXED_RM(1);
    FIXED_RM(2);
    FIXED_RM(3);
}

// ---------------------------------------------------------------- integer odds and ends
static void t_int(void) {
    size_t vl = __riscv_vsetvl_e32m1(N);
    vuint32m1_t a = __riscv_vle32_v_u32m1(a32, vl), b = __riscv_vle32_v_u32m1(b32, vl);
    vbool32_t c = __riscv_vmsltu_vv_u32m1_b32(a, b, vl);
    __riscv_vse32_v_u32m1(r32, __riscv_vadc_vvm_u32m1(a, b, c, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[i] + b32[i] + (a32[i] < b32[i]);
    check("vadc", same32(vl));
    __riscv_vse32_v_u32m1(r32, __riscv_vsbc_vvm_u32m1(a, b, c, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[i] - b32[i] - (a32[i] < b32[i]);
    check("vsbc", same32(vl));
    vuint32m1_t one = __riscv_vmv_v_x_u32m1(1, vl), zero = __riscv_vmv_v_x_u32m1(0, vl);
    __riscv_vse32_v_u32m1(r32, __riscv_vmerge_vvm_u32m1(zero, one, __riscv_vmadc_vvm_u32m1_b32(a, b, c, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = ((uint64_t)a32[i] + b32[i] + (a32[i] < b32[i])) >> 32;
    check("vmadc.vvm", same32(vl));
    __riscv_vse32_v_u32m1(r32, __riscv_vmerge_vvm_u32m1(zero, one, __riscv_vmsbc_vv_u32m1_b32(a, b, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[i] < b32[i];
    check("vmsbc.vv", same32(vl));
    __riscv_vse32_v_u32m1(r32, __riscv_vmerge_vvm_u32m1(zero, one, __riscv_vmsbc_vvm_u32m1_b32(a, b, c, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = (uint64_t)a32[i] < (uint64_t)b32[i] + (a32[i] < b32[i]);
    check("vmsbc.vvm", same32(vl));
    vint32m1_t sa = __riscv_vreinterpret_v_u32m1_i32m1(a), sb = __riscv_vreinterpret_v_u32m1_i32m1(b);
    __riscv_vse32_v_i32m1((int32_t*)r32, __riscv_vmulhsu_vv_i32m1(sa, b, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = (uint32_t)(((int64_t)(int32_t)a32[i] * (int64_t)(uint64_t)b32[i]) >> 32);
    check("vmulhsu", same32(vl));
    a32[0] = 0x80000000u; b32[0] = 0xffffffffu; b32[1] = 0;
    a = __riscv_vle32_v_u32m1(a32, vl); b = __riscv_vle32_v_u32m1(b32, vl);
    sa = __riscv_vreinterpret_v_u32m1_i32m1(a); sb = __riscv_vreinterpret_v_u32m1_i32m1(b);
    __riscv_vse32_v_i32m1((int32_t*)r32, __riscv_vdiv_vv_i32m1(sa, sb, vl), vl);
    for (size_t i = 0; i < vl; i++) {
        int32_t x = (int32_t)a32[i], y = (int32_t)b32[i];
        e32[i] = y == 0 ? 0xffffffffu : (x == INT32_MIN && y == -1) ? 0x80000000u : (uint32_t)(x / y);
    }
    check("vdiv: overflow and zero", same32(vl));
    __riscv_vse32_v_i32m1((int32_t*)r32, __riscv_vrem_vv_i32m1(sa, sb, vl), vl);
    for (size_t i = 0; i < vl; i++) {
        int32_t x = (int32_t)a32[i], y = (int32_t)b32[i];
        e32[i] = y == 0 ? (uint32_t)x : (x == INT32_MIN && y == -1) ? 0 : (uint32_t)(x % y);
    }
    check("vrem: overflow and zero", same32(vl));
    __riscv_vse32_v_u32m1(r32, __riscv_vremu_vv_u32m1(a, b, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = b32[i] ? a32[i] % b32[i] : a32[i];
    check("vremu", same32(vl));
    vuint32m1_t d = __riscv_vle32_v_u32m1(r32, vl);
    __riscv_vse32_v_u32m1(e32, d, vl);
    __riscv_vse32_v_u32m1(r32, __riscv_vnmsub_vv_u32m1(d, a, b, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = b32[i] - a32[i] * e32[i];
    check("vnmsub", same32(vl));
    d = __riscv_vle32_v_u32m1(r32, vl);
    __riscv_vse32_v_u32m1(e32, d, vl);
    __riscv_vse32_v_u32m1(r32, __riscv_vnmsac_vx_u32m1(d, 77, b, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = e32[i] - 77 * b32[i];
    check("vnmsac.vx", same32(vl));
    __riscv_vse32_v_u32m1(r32, __riscv_vrsub_vx_u32m1(a, 5, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = 5 - a32[i];
    check("vrsub", same32(vl));
    // compares with immediates, which are sign-extended
    __riscv_vse32_v_u32m1(r32, __riscv_vmerge_vvm_u32m1(zero, one, __riscv_vmsleu_vx_u32m1_b32(a, 0xfffffff0u, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[i] <= 0xfffffff0u;
    check("vmsleu", same32(vl));
    vbool32_t gt;
    __asm__ volatile("vsetvli zero, %1, e32, m1, ta, ma\n vmsgtu.vi %0, %2, -16" : "=vr"(gt) : "r"(vl), "vr"(a));
    __riscv_vse32_v_u32m1(r32, __riscv_vmerge_vvm_u32m1(zero, one, gt, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[i] > 0xfffffff0u;
    check("vmsgtu.vi with a negative immediate", same32(vl));
    // widening multiply-adds, mixed signs
    vl = __riscv_vsetvl_e16m1(N);
    vint16m1_t x = __riscv_vle16_v_i16m1((int16_t*)a16, vl);
    vuint16m1_t ux = __riscv_vle16_v_u16m1(a16, vl), uy = __riscv_vle16_v_u16m1(b16, vl);
    vint32m2_t acc = __riscv_vmv_v_x_i32m2(1000, vl);
    acc = __riscv_vwmaccsu_vv_i32m2(acc, x, uy, vl);
    acc = __riscv_vwmaccus_vx_i32m2(acc, 40000, x, vl);
    acc = __riscv_vwmaccsu_vx_i32m2(acc, -3, uy, vl);
    __riscv_vse32_v_i32m2((int32_t*)r32, acc, vl);
    for (size_t i = 0; i < vl; i++)
        e32[i] = (uint32_t)(1000 + (int16_t)a16[i] * (int32_t)b16[i] + 40000u * (uint32_t)(int32_t)(int16_t)a16[i] + -3 * (int32_t)b16[i]);
    check("vwmaccsu, vwmaccus", same32(vl));
    vuint32m2_t uacc = __riscv_vwmaccu_vv_u32m2(__riscv_vmv_v_x_u32m2(7, vl), ux, uy, vl);
    __riscv_vse32_v_u32m2(r32, __riscv_vwaddu_wv_u32m2(uacc, ux, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = 7 + (uint32_t)a16[i] * b16[i] + a16[i];
    check("vwmaccu, vwaddu.wv", same32(vl));
    __riscv_vse32_v_i32m2((int32_t*)r32, __riscv_vwsub_wx_i32m2(__riscv_vreinterpret_v_u32m2_i32m2(uacc), -5, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = 7 + (uint32_t)a16[i] * b16[i] + 5;
    check("vwsub.wx", same32(vl));
    // vsext.vf8 / vzext.vf8
    vl = __riscv_vsetvl_e64m1(N);
    __riscv_vse64_v_i64m1((int64_t*)r64, __riscv_vsext_vf8_i64m1(__riscv_vle8_v_i8mf8((int8_t*)a8, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e64[i] = (uint64_t)(int64_t)(int8_t)a8[i];
    check("vsext.vf8", same64(vl));
    __riscv_vse64_v_u64m1(r64, __riscv_vzext_vf8_u64m1(__riscv_vle8_v_u8mf8(a8, vl), vl), vl);
    for (size_t i = 0; i < vl; i++) e64[i] = a8[i];
    check("vzext.vf8", same64(vl));
    // vmv.v.x at SEW 64 sign-extends the scalar on RV32
    __riscv_vse64_v_i64m1((int64_t*)r64, __riscv_vmv_v_x_i64m1(-2, vl), vl);
    for (size_t i = 0; i < vl; i++) e64[i] = (uint64_t)-2;
    check("vmv.v.x at SEW 64", same64(vl));
    // reductions with a mask, and the tail of vd
    vl = __riscv_vsetvl_e32m1(N);
    a = __riscv_vle32_v_u32m1(a32, vl);
    vbool32_t odd = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(a, 1, vl), 0, vl);
    vuint32m1_t s = __riscv_vredmaxu_vs_u32m1_u32m1_m(odd, a, __riscv_vmv_s_x_u32m1(5, 1), vl);
    uint32_t mx = 5;
    for (size_t i = 0; i < vl; i++) if ((a32[i] & 1) && a32[i] > mx) mx = a32[i];
    check("vredmaxu masked", __riscv_vmv_x_s_u32m1_u32(s) == mx);
}

// ---------------------------------------------------------------- loads and stores
static void t_mem(void) {
    // segments: 4 fields of 16 bits
    size_t vl = __riscv_vsetvl_e16m1(N);
    vuint16m1x4_t s4 = __riscv_vlseg4e16_v_u16m1x4(a16, vl);
    __riscv_vse16_v_u16m1(r16, __riscv_vget_v_u16m1x4_u16m1(s4, 2), vl);
    for (size_t i = 0; i < vl; i++) e16[i] = a16[4 * i + 2];
    check("vlseg4e16", same16(vl));
    for (int i = 0; i < N * 8; i++) r16[i] = 0;
    __riscv_vsseg4e16_v_u16m1x4(r16, s4, vl);
    for (size_t i = 0; i < 4 * vl; i++) e16[i] = a16[i];
    check("vsseg4e16", same16(4 * vl));
    // 8 fields of bytes
    vl = __riscv_vsetvl_e8mf2(N);
    vuint8mf2x8_t s8 = __riscv_vlseg8e8_v_u8mf2x8(a8, vl);
    __riscv_vse8_v_u8mf2(r8, __riscv_vget_v_u8mf2x8_u8mf2(s8, 7), vl);
    for (size_t i = 0; i < vl; i++) e8[i] = a8[8 * i + 7];
    check("vlseg8e8", same8(vl));
    // strided, with a negative and a zero stride
    vl = __riscv_vsetvl_e32m1(9);
    __riscv_vse32_v_u32m1(r32, __riscv_vlse32_v_u32m1(&a32[30], -12, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[30 - 3 * i];
    check("vlse32, stride -12", same32(vl));
    __riscv_vse32_v_u32m1(r32, __riscv_vlse32_v_u32m1(&a32[4], 0, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[4];
    check("vlse32, stride 0", same32(vl));
    for (int i = 0; i < N; i++) r32[i] = 0;
    __riscv_vsse32_v_u32m1(&r32[1], 8, __riscv_vle32_v_u32m1(a32, vl), vl);
    for (int i = 0; i < N; i++) e32[i] = 0;
    for (size_t i = 0; i < vl; i++) e32[1 + 2 * i] = a32[i];
    check("vsse32", same32(N));
    // strided segments; indexed with 8-bit offsets, ordered and not
    vl = __riscv_vsetvl_e16m1(5);
    vuint16m1x2_t sg = __riscv_vlsseg2e16_v_u16m1x2(a16, 10, vl);
    __riscv_vse16_v_u16m1(r16, __riscv_vget_v_u16m1x2_u16m1(sg, 1), vl);
    for (size_t i = 0; i < vl; i++) e16[i] = a16[5 * i + 1];
    check("vlsseg2e16", same16(vl));
    vl = __riscv_vsetvl_e32m1(N);
    for (size_t i = 0; i < vl; i++) b8[i] = (uint8_t)(4 * (rnd() % N > 63 ? 0 : rnd() % N));
    vuint8mf4_t off = __riscv_vle8_v_u8mf4(b8, vl);
    __riscv_vse32_v_u32m1(r32, __riscv_vluxei8_v_u32m1(a32, off, vl), vl);
    for (size_t i = 0; i < vl; i++) e32[i] = a32[b8[i] / 4];
    check("vluxei8", same32(vl));
    __riscv_vse32_v_u32m1(r32, __riscv_vloxei8_v_u32m1(a32, off, vl), vl);
    check("vloxei8", same32(vl));
    vuint16m1x2_t ig = __riscv_vluxseg2ei8_v_u16m1x2(a16, __riscv_vle8_v_u8mf2(b8, vl > 16 ? 16 : vl), vl > 16 ? 16 : vl);
    __riscv_vse16_v_u16m1(r16, __riscv_vget_v_u16m1x2_u16m1(ig, 1), vl > 16 ? 16 : vl);
    for (size_t i = 0; i < (vl > 16 ? 16 : vl); i++) e16[i] = a16[b8[i] / 2 + 1];
    check("vluxseg2ei8", same16(vl > 16 ? 16 : (int)vl));
    // masked unit-stride store and the mask load/store
    vl = __riscv_vsetvl_e8m1(N);
    vbool8_t m = __riscv_vmsgtu_vx_u8m1_b8(__riscv_vle8_v_u8m1(a8, vl), 100, vl);
    for (int i = 0; i < N; i++) r8[i] = 0xee;
    __riscv_vse8_v_u8m1_m(m, r8, __riscv_vle8_v_u8m1(b8, vl), vl);
    for (size_t i = 0; i < vl; i++) e8[i] = a8[i] > 100 ? b8[i] : 0xee;
    check("masked vse8", same8(vl));
    for (int i = 0; i < N; i++) r8[i] = 0;
    __riscv_vsm_v_b8(r8, m, vl);
    for (int i = 0; i < N; i++) e8[i] = 0;
    for (size_t i = 0; i < vl; i++) if (a8[i] > 100) e8[i / 8] |= (uint8_t)(1u << (i % 8));
    int ok = r8[(vl + 7) / 8] == 0;
    for (size_t i = 0; i < vl; i++) if (((r8[i / 8] ^ e8[i / 8]) >> (i % 8)) & 1) ok = 0;  // the mask's tail is not compared
    check("vsm", ok);
}

// A load or store that faults: vstart is the element, mtval its address.
volatile uintptr_t t_cause, t_tval, t_vstart, t_epc;
#if __riscv_xlen == 64
#define SX "sd"
#else
#define SX "sw"
#endif
void trap_entry(void);
__asm__(".align 2\n"
        "trap_entry:\n"
        "  csrw mscratch, t0\n"
        "  csrr t0, mcause\n  la t1, t_cause\n  " SX " t0, 0(t1)\n"
        "  csrr t0, mtval\n  la t1, t_tval\n  " SX " t0, 0(t1)\n"
        "  csrr t0, vstart\n  la t1, t_vstart\n  " SX " t0, 0(t1)\n"
        "  csrr t0, mepc\n  la t1, t_epc\n  " SX " t0, 0(t1)\n"
        "  addi t0, t0, 4\n  csrw mepc, t0\n"
        "  csrw vstart, zero\n"
        "  csrr t0, mscratch\n"
        "  mret\n");
static void t_fault(void) {
    csr_write(mtvec, (uintptr_t)trap_entry);
    // RAM ends at 0x80000000 + 128 MiB on mvplat's built-in board; the
    // bytes past it are not memory. Copy a few bytes to just below the end.
    uint8_t* end = (uint8_t*)(uintptr_t)0x88000000u;
    for (int i = 1; i <= 6; i++) end[-i] = (uint8_t)(0x40 + i);
    size_t vl = __riscv_vsetvl_e8m1(16);
    t_cause = 0;
    vuint8m1_t v = __riscv_vle8ff_v_u8m1(end - 6, &vl, vl);
    check("vle8ff trims vl at the fault", t_cause == 0 && vl == 6);
    __riscv_vse8_v_u8m1(r8, v, vl);
    for (size_t i = 0; i < 6; i++) e8[i] = (uint8_t)(0x40 + 6 - i);
    check("vle8ff loads up to the fault", same8(6));
    // a fault on element 0 of a fault-only-first load traps
    vl = __riscv_vsetvl_e8m1(16);
    t_cause = 0;
    v = __riscv_vle8ff_v_u8m1(end, &vl, vl);
    check("vle8ff traps on element 0", t_cause == 5 && t_tval == 0x88000000u && t_vstart == 0);
    // an ordinary load: vstart says where it stopped
    vl = __riscv_vsetvl_e16m1(8);
    t_cause = 0;
    __asm__ volatile("vle16.v v4, (%0)" ::"r"(end - 6) : "memory");
    check("vle16 faults on element 3 with vstart 3", t_cause == 5 && t_tval == 0x88000000u && t_vstart == 3);
    t_cause = 0;
    __asm__ volatile("vse16.v v4, (%0)" ::"r"(end - 4) : "memory");
    check("vse16 faults on element 2", t_cause == 7 && t_tval == 0x88000000u && t_vstart == 2);
    // an illegal vtype: the next vector instruction is illegal
    t_cause = 0;
    __asm__ volatile("vsetvl zero, %0, %1\n vadd.vv v1, v2, v3" ::"r"(4), "r"((uintptr_t)7 << 3));
    check("vill makes vadd illegal", t_cause == 2);
    // a widening operation on a misaligned group is illegal
    __riscv_vsetvl_e16m2(4);
    t_cause = 0;
    __asm__ volatile("vwadd.vv v2, v4, v6" ::: "memory");  // vd v2 is fine for m2 -> m4? no: m4 needs a multiple of 4
    check("vwadd.vv into a misaligned group", t_cause == 2);
    csr_write(mtvec, 0);
}

// ---------------------------------------------------------------- floating point
static float frec(float x) { return 1.0f / x; }
static void t_float(void) {
    size_t vl = __riscv_vsetvl_e32m1(N);
    vfloat32m1_t a = __riscv_vle32_v_f32m1(af, vl);
    // estimates: within 2^-7 of the exact values
    __riscv_vse32_v_f32m1(rf, __riscv_vfrec7_v_f32m1(a, vl), vl);
    int ok = 1;
    for (size_t i = 0; i < vl; i++) {
        if (af[i] == 0.0f) continue;
        float r = frec(af[i]), d = (rf[i] - r) / r;
        if (d > 1.0f / 128 || d < -1.0f / 128) ok = 0;
    }
    check("vfrec7 within 2^-7", ok);
    vfloat32m1_t pos = __riscv_vfabs_v_f32m1(a, vl);
    __riscv_vse32_v_f32m1(rf, __riscv_vfrsqrt7_v_f32m1(pos, vl), vl);
    ok = 1;
    for (size_t i = 0; i < vl; i++) {
        float x = af[i] < 0 ? -af[i] : af[i];
        if (x == 0.0f) continue;
        float r = 1.0f / __builtin_sqrtf(x), d = (rf[i] - r) / r;
        if (d > 1.0f / 128 || d < -1.0f / 128) ok = 0;
    }
    check("vfrsqrt7 within 2^-7", ok);
    // exact table points of the specification's examples
    float t[4] = {1.0f, 2.0f, 4.0f, 0.5f};
    __riscv_vse32_v_f32m1(rf, __riscv_vfrec7_v_f32m1(__riscv_vle32_v_f32m1(t, 4), 4), 4);
    check("vfrec7(1.0) is 0.99609375 (0x3f7f0000)", rf[0] == 0.99609375f && rf[1] == 0.498046875f);
    __riscv_vse32_v_f32m1(rf, __riscv_vfrsqrt7_v_f32m1(__riscv_vle32_v_f32m1(t, 4), 4), 4);
    check("vfrsqrt7(1.0), (4.0)", rf[0] == 0.99609375f && rf[2] == 0.498046875f);
    // classify, sign injection, merge, scalar moves
    float sp[8] = {-__builtin_inff(), -1.0f, -1e-40f, -0.0f, 0.0f, 1e-40f, 1.0f, __builtin_inff()};
    __riscv_vse32_v_u32m2(r32, __riscv_vfclass_v_u32m2(__riscv_vle32_v_f32m2(sp, 8), 8), 8);  // VLMAX 8 at least
    ok = 1;
    for (int i = 0; i < 8; i++) if (r32[i] != 1u << i) ok = 0;
    check("vfclass", ok);
    vl = __riscv_vsetvl_e32m1(N);
    __riscv_vse32_v_f32m1(rf, __riscv_vfsgnjx_vf_f32m1(a, -2.0f, vl), vl);
    for (size_t i = 0; i < vl; i++) ef[i] = -af[i];
    ok = 1;
    for (size_t i = 0; i < vl; i++) { uint32_t x, y; __builtin_memcpy(&x, &rf[i], 4); __builtin_memcpy(&y, &ef[i], 4); if (x != y) ok = 0; }
    check("vfsgnjx", ok);
    __riscv_vse32_v_f32m1(rf, __riscv_vfslide1down_vf_f32m1(a, 9.5f, vl), vl);
    ok = rf[vl - 1] == 9.5f;
    for (size_t i = 0; i + 1 < vl; i++) if (rf[i] != af[i + 1]) ok = 0;
    check("vfslide1down", ok);
    check("vfmv.f.s", __riscv_vfmv_f_s_f32m1_f32(__riscv_vfslide1up_vf_f32m1(a, -7.25f, vl)) == -7.25f);
    // widening: products in double, an accumulation of products
    vl = __riscv_vsetvl_e32m1(N);
    vfloat64m2_t w = __riscv_vfwmul_vf_f64m2(a, 3.0f, vl);
    w = __riscv_vfwmacc_vf_f64m2(w, 0.5f, a, vl);
    w = __riscv_vfwnmsac_vv_f64m2(w, a, a, vl);
    w = __riscv_vfwsub_wf_f64m2(w, 1.25f, vl);
    __riscv_vse64_v_f64m2(rd, w, vl);
    ok = 1;
    for (size_t i = 0; i < vl; i++) {
        double x = af[i];
        double e = x * 3.0;
        e = 0.5 * x + e;
        e = -(x * x) + e;
        e = e - 1.25;
        if (rd[i] != e) ok = 0;
    }
    check("vfwmul, vfwmacc, vfwnmsac, vfwsub.wf", ok);
    // narrowing with round-to-odd
    double dd[4] = {1.0 + 0x1p-30, 1.0, -(1.0 + 0x1p-40), 3.0 + 0x1p-24 + 0x1p-50};
    __riscv_vse32_v_f32m1(rf, __riscv_vfncvt_rod_f_f_w_f32m1(__riscv_vle64_v_f64m2(dd, 4), 4), 4);
    check("vfncvt.rod", rf[0] == 1.0f + 0x1p-23f && rf[1] == 1.0f && rf[2] == -(1.0f + 0x1p-23f) && rf[3] == 3.0f + 0x1p-22f);
    // reductions: ordered sum, min with NaN
    vl = __riscv_vsetvl_e32m1(N);
    float s = 10.0f;
    for (size_t i = 0; i < vl; i++) s += af[i];
    check("vfredosum", __riscv_vfmv_f_s_f32m1_f32(__riscv_vfredosum_vs_f32m1_f32m1(a, __riscv_vfmv_s_f_f32m1(10.0f, 1), vl)) == s);
    float mn = 1e30f;
    for (size_t i = 0; i < vl; i++) mn = af[i] < mn ? af[i] : mn;
    float qn = __builtin_nanf("");
    check("vfredmin ignores a NaN start", __riscv_vfmv_f_s_f32m1_f32(__riscv_vfredmin_vs_f32m1_f32m1(a, __riscv_vfmv_s_f_f32m1(qn, 1), vl)) == mn);
    // flags: 1/0 sets DZ
    csr_write(fflags, 0);
    vl = __riscv_vsetvl_e32m1(2);
    float z[2] = {0.0f, 2.0f};
    __riscv_vse32_v_f32m1(rf, __riscv_vfrdiv_vf_f32m1(__riscv_vle32_v_f32m1(z, 2), 1.0f, 2), 2);
    check("vfrdiv and DZ", rf[0] == __builtin_inff() && rf[1] == 0.5f && (csr_read(fflags) & 8));
    // conversions from 16-bit integers, to 16-bit integers
    vl = __riscv_vsetvl_e16mf2(N);
    __riscv_vse32_v_f32m1(rf, __riscv_vfwcvt_f_x_v_f32m1(__riscv_vle16_v_i16mf2((int16_t*)a16, vl), vl), vl);
    ok = 1;
    for (size_t i = 0; i < vl; i++) if (rf[i] != (float)(int16_t)a16[i]) ok = 0;
    check("vfwcvt.f.x.v from 16 bits", ok);
    __riscv_vse16_v_i16mf2((int16_t*)r16, __riscv_vfncvt_rtz_x_f_w_i16mf2(__riscv_vfmul_vf_f32m1(a, 100.0f, vl), vl), vl);
    ok = 1;
    for (size_t i = 0; i < vl; i++) {
        float x = af[i] * 100.0f;
        int16_t e = x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (int16_t)x;
        if ((int16_t)r16[i] != e) ok = 0;
    }
    check("vfncvt.rtz.x.f.w to 16 bits, saturating", ok);
}

int main(int hart) {
    if (hart) for (;;) {}
    csr_set(mstatus, (3u << 9) | (3u << 13));  // VS and FS on
    fill();
    t_vsetvl();
    t_permute();
    t_fixed();
    t_int();
    t_mem();
    t_fault();
    t_float();
    if (!fails) puts_("vops: all checks passed\n");
    return fails;
}
