// vkern.c - loops for the V extension test. tests/vplat/build.sh compiles
// this file twice: once for RV64GC (scalar) and once for RV64GCV, which
// clang's loop vectorizer turns into vector code (loads and stores unit,
// strided, indexed and segmented, masks, reductions, widening and narrowing,
// conversions). vec.c runs both and compares every kernel's result, so the
// compiler, not this test, says what each vector instruction must do.
#include <stdint.h>

#ifndef SFX
#define SFX _s
#endif
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define K(name) CAT(name, SFX)

#define N 203  // not a multiple of any VLMAX: tails everywhere

static int8_t a8[N], b8[N], c8[N];
static uint8_t ua8[N], ub8[N], uc8[N];
static int16_t a16[N], b16[N], c16[N];
static uint16_t ua16[N], uc16[N];
static int32_t a32[N], b32[N], c32[N];
static uint32_t ua32[N], ub32[N], uc32[N];
static int64_t a64[N], b64[N], c64[N];
static uint64_t ua64[N], uc64[N];
static float af[N], bf[N], cf[N];
static double ad[N], bd[N], cd[N];
static int32_t idx[N];
static struct { int32_t x, y, z; } s3[N];
static struct { float re, im; } cx[N], cy[N], cz[N];

static uint64_t seed;
static uint64_t rnd(void) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return seed >> 17;
}
static uint64_t h;
static void mix(uint64_t v) { h = (h ^ v) * 0x100000001b3ull; }
#define MIXA(arr) do { for (int i_ = 0; i_ < N; i_++) { uint64_t v_ = 0; __builtin_memcpy(&v_, &arr[i_], sizeof arr[i_]); mix(v_); } } while (0)

void K(vk_init)(void) {
    seed = 12345;
    for (int i = 0; i < N; i++) {
        a8[i] = (int8_t)rnd(); b8[i] = (int8_t)rnd();
        ua8[i] = (uint8_t)rnd(); ub8[i] = (uint8_t)rnd();
        a16[i] = (int16_t)rnd(); b16[i] = (int16_t)rnd(); ua16[i] = (uint16_t)rnd();
        a32[i] = (int32_t)rnd(); b32[i] = (int32_t)rnd();
        ua32[i] = (uint32_t)rnd(); ub32[i] = (uint32_t)rnd();
        a64[i] = (int64_t)(rnd() << 20 ^ rnd()); b64[i] = (int64_t)(rnd() << 20 ^ rnd());
        ua64[i] = rnd() << 23 ^ rnd();
        af[i] = (float)((int32_t)rnd() % 20000) / 64.0f;
        bf[i] = (float)((int32_t)rnd() % 20000) / 128.0f + 0.5f;
        ad[i] = (double)(int32_t)(rnd() % 2000000) / 1024.0;
        bd[i] = (double)(int32_t)(rnd() % 2000000) / 512.0 + 0.25;
        idx[i] = (int32_t)(rnd() % N);
        s3[i].x = (int32_t)rnd(); s3[i].y = (int32_t)rnd(); s3[i].z = (int32_t)rnd();
        cx[i].re = af[i]; cx[i].im = bf[i]; cy[i].re = bf[i] - 1; cy[i].im = af[i] + 3;
    }
    af[7] = 0.0f; af[9] = -0.0f; bf[11] = 0.0f;   // some signed zeros and a division by zero
    ad[5] = 1e300; bd[5] = 1e-300;                 // an overflow
}

// Each kernel leaves its result in the c arrays and returns a hash of them.
#define BEGIN h = 0xcbf29ce484222325ull
uint64_t K(k_arith8)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) c8[i] = (int8_t)(a8[i] + b8[i] * 3 - (a8[i] ^ b8[i]));
    MIXA(c8);
    for (int i = 0; i < N; i++) uc8[i] = (uint8_t)((ua8[i] | ub8[i]) & (ua8[i] - ub8[i]));
    MIXA(uc8);
    return h;
}
uint64_t K(k_arith16)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) c16[i] = (int16_t)(a16[i] * b16[i] + (a16[i] >> 3));
    MIXA(c16);
    for (int i = 0; i < N; i++) uc16[i] = (uint16_t)(ua16[i] >> (ua16[i] & 15)) ^ (uint16_t)(ua16[i] << 2);
    MIXA(uc16);
    return h;
}
uint64_t K(k_arith32)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) c32[i] = a32[i] * b32[i] - (a32[i] >> (b32[i] & 31)) + (int32_t)((uint32_t)a32[i] << (b32[i] & 7));
    MIXA(c32);
    for (int i = 0; i < N; i++) uc32[i] = ua32[i] / ((ub32[i] & 0xff) + 1) + ua32[i] % ((ub32[i] >> 24) + 3);
    MIXA(uc32);
    for (int i = 0; i < N; i++) c32[i] = a32[i] / ((b32[i] >> 20) | 1) - a32[i] % ((b32[i] & 0x3ff) + 7);
    MIXA(c32);
    return h;
}
uint64_t K(k_arith64)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) c64[i] = a64[i] * b64[i] + (a64[i] >> 7) - (b64[i] / ((a64[i] & 0xffff) + 1));
    MIXA(c64);
    for (int i = 0; i < N; i++) uc64[i] = (ua64[i] >> (ua64[i] & 63)) | (ua64[i] << 1);
    MIXA(uc64);
    return h;
}
uint64_t K(k_minmax)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) c8[i] = a8[i] < b8[i] ? a8[i] : b8[i];
    for (int i = 0; i < N; i++) uc8[i] = ua8[i] > ub8[i] ? ua8[i] : ub8[i];
    for (int i = 0; i < N; i++) c32[i] = a32[i] > b32[i] ? a32[i] : b32[i];
    for (int i = 0; i < N; i++) uc32[i] = ua32[i] < ub32[i] ? ua32[i] : ub32[i];
    for (int i = 0; i < N; i++) c64[i] = a64[i] < 0 ? -a64[i] : a64[i];
    MIXA(c8); MIXA(uc8); MIXA(c32); MIXA(uc32); MIXA(c64);
    return h;
}
uint64_t K(k_reduce)(void) {
    BEGIN;
    int32_t s = 0, mn = INT32_MAX, mx = INT32_MIN;
    uint32_t x = 0, o = 0, an = ~0u, umx = 0;
    int64_t s64 = 0;
    for (int i = 0; i < N; i++) {
        s += a32[i];
        x ^= ua32[i];
        o |= ua32[i] >> 5;
        an &= ua32[i] | 0x10101010u;
        mn = a32[i] < mn ? a32[i] : mn;
        mx = a32[i] > mx ? a32[i] : mx;
        umx = ua32[i] > umx ? ua32[i] : umx;
    }
    for (int i = 0; i < N; i++) s64 += a64[i] >> 3;
    int16_t s16 = 0;
    for (int i = 0; i < N; i++) s16 += a16[i];
    uint8_t m8 = 255;
    for (int i = 0; i < N; i++) m8 = ua8[i] < m8 ? ua8[i] : m8;
    mix((uint32_t)s); mix(x); mix(o); mix(an); mix((uint32_t)mn); mix((uint32_t)mx); mix(umx);
    mix((uint64_t)s64); mix((uint16_t)s16); mix(m8);
    return h;
}
uint64_t K(k_widen)(void) {
    BEGIN;
    int32_t dot = 0;
    for (int i = 0; i < N; i++) dot += a8[i] * b8[i];
    uint32_t usum = 0;
    for (int i = 0; i < N; i++) usum += ua8[i];
    int64_t dot64 = 0;
    for (int i = 0; i < N; i++) dot64 += (int64_t)a32[i] * b32[i];
    for (int i = 0; i < N; i++) uc16[i] = (uint16_t)(ua8[i] + ub8[i]);
    for (int i = 0; i < N; i++) c32[i] = a16[i] * b16[i];
    for (int i = 0; i < N; i++) c64[i] = (int64_t)a32[i] - b32[i];
    for (int i = 0; i < N; i++) uc64[i] = (uint64_t)ua32[i] * ub32[i];
    for (int i = 0; i < N; i++) c16[i] = (int16_t)(a8[i] * (uint8_t)ub8[i]);
    mix((uint32_t)dot); mix(usum); mix((uint64_t)dot64);
    MIXA(uc16); MIXA(c32); MIXA(c64); MIXA(uc64); MIXA(c16);
    return h;
}
uint64_t K(k_extend)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) c32[i] = a8[i];
    MIXA(c32);
    for (int i = 0; i < N; i++) uc32[i] = ua8[i];
    MIXA(uc32);
    for (int i = 0; i < N; i++) c64[i] = a16[i];
    MIXA(c64);
    for (int i = 0; i < N; i++) uc64[i] = ua8[i];
    MIXA(uc64);
    for (int i = 0; i < N; i++) c64[i] = a32[i];
    MIXA(c64);
    return h;
}
uint64_t K(k_narrow)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) c8[i] = (int8_t)(a32[i] >> 3);
    for (int i = 0; i < N; i++) uc16[i] = (uint16_t)(ua32[i] >> 9);
    for (int i = 0; i < N; i++) c32[i] = (int32_t)(a64[i] >> 17);
    for (int i = 0; i < N; i++) {                   // clamp to 0..255
        int32_t v = a16[i] + b16[i] / 4;
        uc8[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    for (int i = 0; i < N; i++) {                   // clamp to int16
        int32_t v = a32[i] >> 9;
        c16[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
    MIXA(c8); MIXA(uc16); MIXA(c32); MIXA(uc8); MIXA(c16);
    return h;
}
uint64_t K(k_saturate)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) { uint8_t s = (uint8_t)(ua8[i] + ub8[i]); uc8[i] = s < ua8[i] ? 255 : s; }
    MIXA(uc8);
    for (int i = 0; i < N; i++) uc8[i] = ua8[i] > ub8[i] ? (uint8_t)(ua8[i] - ub8[i]) : 0;
    MIXA(uc8);
    for (int i = 0; i < N; i++) {
        int v = a8[i] + b8[i];
        c8[i] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
    }
    MIXA(c8);
    for (int i = 0; i < N; i++) uc8[i] = (uint8_t)((ua8[i] + ub8[i] + 1) >> 1);
    MIXA(uc8);
    for (int i = 0; i < N; i++) c16[i] = (int16_t)((a16[i] * b16[i]) >> 16);
    MIXA(c16);
    for (int i = 0; i < N; i++) uc32[i] = (uint32_t)(((uint64_t)ua32[i] * ub32[i]) >> 32);
    MIXA(uc32);
    return h;
}
uint64_t K(k_select)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) c32[i] = a32[i] > b32[i] ? a32[i] - b32[i] : b32[i] * 2;
    MIXA(c32);
    for (int i = 0; i < N; i++) if (a16[i] > 0) c16[i] = b16[i]; else c16[i] = 0;
    MIXA(c16);
    for (int i = 0; i < N; i++) c32[i] = 7;
    for (int i = 0; i < N; i++) if (a32[i] & 1) c32[i] = b32[i];  // a masked store
    MIXA(c32);
    int cnt = 0;
    for (int i = 0; i < N; i++) cnt += ua8[i] > 100;
    mix((uint64_t)cnt);
    for (int i = 0; i < N; i++) uc64[i] = ua64[i] == (uint64_t)a64[i] || (ua64[i] & 3) == 1 ? 1 : 2;
    MIXA(uc64);
    for (int i = 0; i < N; i++) c8[i] = a8[i] != b8[i] && a8[i] <= 3 ? (int8_t)(a8[i] | 1) : b8[i];
    MIXA(c8);
    return h;
}
uint64_t K(k_gather)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) c32[i] = a32[idx[i]];
    MIXA(c32);
    for (int i = 0; i < N; i++) cd[i] = ad[idx[i]] + 1.0;
    MIXA(cd);
    for (int i = 0; i < N; i++) c16[i] = a16[idx[i]] + b16[i];
    MIXA(c16);
    for (int i = 0; i < N; i++) c32[i] = 0;
    for (int i = 0; i < N; i++) c32[(i * 7) % N] = a32[i];   // a scatter (a permutation: 7 and 203 are coprime... 203 = 7 * 29)
    MIXA(c32);
    for (int i = 0; i < N; i++) c64[i] = a64[N - 1 - i];     // reverse
    MIXA(c64);
    return h;
}
uint64_t K(k_strided)(void) {
    BEGIN;
    for (int i = 0; i < N / 2; i++) c32[i] = a32[2 * i] + a32[2 * i + 1];
    MIXA(c32);
    for (int i = 0; i < N / 3; i++) c64[i] = a64[3 * i];
    MIXA(c64);
    for (int i = 0; i < N; i++) c32[i] = s3[i].x + s3[i].y * s3[i].z;
    MIXA(c32);
    for (int i = 0; i < N; i++) { s3[i].x += 1; s3[i].z ^= s3[i].y; }
    for (int i = 0; i < N; i++) { mix((uint32_t)s3[i].x); mix((uint32_t)s3[i].z); }
    for (int i = 0; i < N; i++) {                       // complex multiply
        cz[i].re = cx[i].re * cy[i].re - cx[i].im * cy[i].im;
        cz[i].im = cx[i].re * cy[i].im + cx[i].im * cy[i].re;
    }
    for (int i = 0; i < N; i++) { uint32_t u; __builtin_memcpy(&u, &cz[i].re, 4); mix(u); __builtin_memcpy(&u, &cz[i].im, 4); mix(u); }
    return h;
}
uint64_t K(k_float)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) cf[i] = af[i] * bf[i] + cf[i] - af[i] / bf[i];
    MIXA(cf);
    for (int i = 0; i < N; i++) cd[i] = ad[i] * bd[i] - ad[i] / bd[i] + (ad[i] - bd[i]);
    MIXA(cd);
    for (int i = 0; i < N; i++) cf[i] = af[i] * bf[i] - cf[i];  // contracted to a fused multiply-add
    MIXA(cf);
    for (int i = 0; i < N; i++) cd[i] = cd[i] - ad[i] * bd[i];
    MIXA(cd);
    for (int i = 0; i < N; i++) cf[i] = __builtin_sqrtf(bf[i]) + __builtin_fabsf(af[i]);
    MIXA(cf);
    for (int i = 0; i < N; i++) cd[i] = __builtin_sqrt(bd[i]) - __builtin_copysign(ad[i], bd[i] - 100.0);
    MIXA(cd);
    for (int i = 0; i < N; i++) cf[i] = __builtin_fminf(af[i], bf[i]) - __builtin_fmaxf(af[i], -bf[i]);
    MIXA(cf);
    for (int i = 0; i < N; i++) cf[i] = af[i] < bf[i] ? af[i] : bf[i] * 2.0f;
    MIXA(cf);
    for (int i = 0; i < N; i++) cd[i] = ad[i] >= 1000.0 ? -ad[i] : ad[i] == bd[i] ? 1.0 : bd[i];
    MIXA(cd);
    for (int i = 0; i < N; i++) cf[i] = -af[i] * bf[i] - 1.5f;
    MIXA(cf);
    return h;
}
uint64_t K(k_fconv)(void) {
    BEGIN;
    for (int i = 0; i < N; i++) cf[i] = (float)a32[i];
    MIXA(cf);
    for (int i = 0; i < N; i++) cf[i] = (float)ua32[i];
    MIXA(cf);
    for (int i = 0; i < N; i++) c32[i] = (int32_t)af[i];
    MIXA(c32);
    for (int i = 0; i < N; i++) uc32[i] = (uint32_t)(bf[i] * 3.0f);
    MIXA(uc32);
    for (int i = 0; i < N; i++) cd[i] = (double)af[i] * 2.0;
    MIXA(cd);
    for (int i = 0; i < N; i++) cf[i] = (float)(ad[i] / 3.0);
    MIXA(cf);
    for (int i = 0; i < N; i++) cd[i] = (double)a32[i];
    MIXA(cd);
#if __riscv_xlen == 64  // RV32 converts 64-bit integers in software
    for (int i = 0; i < N; i++) c64[i] = (int64_t)(ad[i] * 7.0);
    MIXA(c64);
    for (int i = 0; i < N; i++) cd[i] = (double)a64[i];
    MIXA(cd);
#endif
    for (int i = 0; i < N; i++) c32[i] = (int32_t)(ad[i] / 64.0);
    MIXA(c32);
#if __riscv_xlen == 64
    for (int i = 0; i < N; i++) cf[i] = (float)a64[i];
    MIXA(cf);
    for (int i = 0; i < N; i++) c64[i] = (int64_t)af[i];
    MIXA(c64);
#endif
    for (int i = 0; i < N; i++) cf[i] = (float)a16[i];
    MIXA(cf);
    for (int i = 0; i < N; i++) c16[i] = (int16_t)(af[i] / 4.0f);
    MIXA(c16);
    return h;
}
uint64_t K(k_freduce)(void) {
    BEGIN;
    float s = 0.0f;
    for (int i = 0; i < N; i++) s += af[i] * 0.25f;
    double d = 1.0;
    for (int i = 0; i < N; i++) d += ad[i] / 7.0;
    float mx = -1e30f;
    for (int i = 0; i < N; i++) mx = af[i] > mx ? af[i] : mx;
    double wd = 0.0;
    for (int i = 0; i < N; i++) wd += (double)bf[i];
    uint32_t u; __builtin_memcpy(&u, &s, 4); mix(u);
    uint64_t v; __builtin_memcpy(&v, &d, 8); mix(v);
    __builtin_memcpy(&u, &mx, 4); mix(u);
    __builtin_memcpy(&v, &wd, 8); mix(v);
    return h;
}
uint64_t K(k_copy)(void) {
    BEGIN;
    static uint8_t buf[1000], dst[1000];
    for (int i = 0; i < 1000; i++) buf[i] = (uint8_t)(i * 37 + 11);
    for (int i = 0; i < 997; i++) dst[i + 3] = buf[i];
    for (int i = 0; i < 1000; i++) mix(dst[i]);
    for (int i = 0; i < 1000; i++) dst[i] = 0x5a;
    for (int i = 0; i < 1000; i += 3) mix(dst[i] + buf[i]);
    for (int i = 0; i < N; i++) c64[i] = i;
    MIXA(c64);
    for (int i = 0; i < N; i++) c32[i] = i * 3 - 100;
    MIXA(c32);
    return h;
}

uint64_t (*const K(vk_all)[])(void) = {
    K(k_arith8), K(k_arith16), K(k_arith32), K(k_arith64), K(k_minmax), K(k_reduce), K(k_widen),
    K(k_extend), K(k_narrow), K(k_saturate), K(k_select), K(k_gather), K(k_strided), K(k_float),
    K(k_fconv), K(k_freduce), K(k_copy), 0,
};
