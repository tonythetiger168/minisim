// calc.c - integer, floating-point and vector arithmetic. It prints one line
// per group; the same file built for the host (-DHOST) prints the expected
// output, so any difference is an instruction the model gets wrong.
#include <stdint.h>
#ifdef HOST
#include <stdio.h>
#define OUT(name, v) printf("%s %016llx\n", name, (unsigned long long)(v))
#else
#include "rt.h"
#define OUT(name, v) (puts_(name), putc_(' '), putx(v), putc_('\n'))
void trap(int kind, struct frame* f) { (void)f; puts_("trap "); putd(kind); puts_(" esr "); putx(SYSREG_READ(esr_el1)); puts_("\n"); exit_(9); }
void secondary(int cpu) { (void)cpu; }
void exit(int code) { exit_(code); }
#endif

static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
#define NOINLINE __attribute__((noinline))
static volatile uint64_t sink;
static uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull + (h >> 29); }

NOINLINE uint64_t ints(void) {
    uint64_t h = 1;
    for (int i = 0; i < 2000; i++) {
        uint64_t a = rnd(), b = rnd();
        uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
        unsigned s = b & 63;
        h = mix(h, a + b); h = mix(h, a - b); h = mix(h, a * b);
        h = mix(h, (uint64_t)(((unsigned __int128)a * b) >> 64));
        h = mix(h, (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64));
        h = mix(h, b ? a / b : 0); h = mix(h, b ? a % b : 0);
        h = mix(h, (b >> 40) ? (uint64_t)((int64_t)a / (int64_t)(b >> 40)) : 0);
        h = mix(h, b32 ? a32 / b32 : 0);
        h = mix(h, (b32 >> 8) ? (uint32_t)((int32_t)a32 / (int32_t)(b32 >> 8)) : 0);
        h = mix(h, a << s); h = mix(h, a >> s); h = mix(h, (uint64_t)((int64_t)a >> s));
        h = mix(h, a32 << (s & 31)); h = mix(h, (uint32_t)((int32_t)a32 >> (s & 31)));
        h = mix(h, (a >> s) | (a << ((64 - s) & 63)));
        h = mix(h, a ? (uint64_t)__builtin_clzll(a) : 64); h = mix(h, a32 ? (uint64_t)__builtin_clz(a32) : 32);
        h = mix(h, a ? (uint64_t)__builtin_ctzll(a) : 64);
        h = mix(h, (uint64_t)__builtin_popcountll(a));
        h = mix(h, __builtin_bswap64(a)); h = mix(h, __builtin_bswap32(a32)); h = mix(h, __builtin_bswap16((uint16_t)a));
        h = mix(h, (int64_t)a < (int64_t)b ? 1 : 2); h = mix(h, a32 < b32 ? 3 : 4);
        h = mix(h, (uint64_t)(int64_t)(int8_t)a); h = mix(h, (uint64_t)(int64_t)(int16_t)a); h = mix(h, (uint64_t)(int64_t)(int32_t)a);
        h = mix(h, (a >> 13) & 0x7ff); h = mix(h, (a & ~0xff0ull) | ((b & 0xff) << 4));
        h = mix(h, a * 7 + b * 9); h = mix(h, a32 * b32 + a32);
        h = mix(h, (uint64_t)(int64_t)(int32_t)a32 * (uint64_t)(int64_t)(int32_t)b32);
        h = mix(h, (uint64_t)a32 * b32);
        h = mix(h, ~a & b); h = mix(h, a | ~b); h = mix(h, a ^ ~b);
        h = mix(h, a > b ? a : b); h = mix(h, (int64_t)a > (int64_t)b ? a - b : b - a);
        int64_t sa = (int64_t)a >> 3;
        h = mix(h, (uint64_t)(sa < 0 ? -sa : sa));
        // add with carry
        unsigned __int128 w = (unsigned __int128)a + ((unsigned __int128)b << 64) ;
        w += ((unsigned __int128)rnd() << 64) | rnd();
        h = mix(h, (uint64_t)w); h = mix(h, (uint64_t)(w >> 64));
        unsigned long long ov;
        h = mix(h, __builtin_add_overflow(a, b, &ov)); h = mix(h, ov);
        long long sov;
        h = mix(h, __builtin_mul_overflow((long long)a, (long long)b, &sov)); h = mix(h, (uint64_t)sov);
    }
    return h;
}

static uint64_t fbits(double d) { union { double d; uint64_t u; } x = {d}; return x.u; }
static uint64_t fbits32(float f) { union { float f; uint32_t u; } x = {f}; return x.u; }

NOINLINE uint64_t floats(void) {
    uint64_t h = 1;
    double acc = 0.5;
    float facc = 0.25f;
    for (int i = 0; i < 2000; i++) {
        int64_t r = (int64_t)rnd();
        double a = (double)(r >> 20) / 1024.0, b = (double)((int64_t)rnd() >> 30) / 3.0 + 0.125;
        float fa = (float)a, fb = (float)b;
        h = mix(h, fbits(a + b)); h = mix(h, fbits(a - b)); h = mix(h, fbits(a * b)); h = mix(h, fbits(a / b));
        h = mix(h, fbits(__builtin_sqrt(a < 0 ? -a : a)));
        h = mix(h, fbits(__builtin_fma(a, b, acc)));
        h = mix(h, fbits32(fa + fb)); h = mix(h, fbits32(fa * fb)); h = mix(h, fbits32(fa / fb));
        h = mix(h, fbits32(__builtin_sqrtf(fb < 0 ? -fb : fb)));
        h = mix(h, fbits32(__builtin_fmaf(fa, fb, facc)));
        h = mix(h, (uint64_t)(int64_t)a); h = mix(h, (uint64_t)(int64_t)fb);
        h = mix(h, a < 0 ? 0 : (uint64_t)a); h = mix(h, (uint64_t)(uint32_t)(fb < 0 ? 0 : fb));
        h = mix(h, fbits((double)r)); h = mix(h, fbits((double)(uint64_t)r)); h = mix(h, fbits32((float)r));
        h = mix(h, fbits32((float)(uint32_t)r)); h = mix(h, fbits((double)(int32_t)r));
        h = mix(h, fbits((double)fa)); h = mix(h, fbits32((float)b));
        h = mix(h, a < b); h = mix(h, a == b); h = mix(h, fa >= fb);
        h = mix(h, fbits(a < b ? a : b)); h = mix(h, fbits(__builtin_fmax(a, b))); h = mix(h, fbits(__builtin_fmin(a, b)));
        h = mix(h, fbits(__builtin_floor(a / 7))); h = mix(h, fbits(__builtin_ceil(a / 7))); h = mix(h, fbits(__builtin_trunc(a / 7)));
        h = mix(h, fbits(__builtin_round(a / 7))); h = mix(h, fbits32(__builtin_floorf(fb)));
        h = mix(h, fbits(-a)); h = mix(h, fbits(__builtin_fabs(a)));
        h = mix(h, (uint64_t)__builtin_lround(a / 3));
        acc = acc * 0.999 + a * 1e-6;
        facc = facc * 0.5f + fb;
    }
    h = mix(h, fbits(acc)); h = mix(h, fbits32(facc));
    double z = 0.0, inf = 1.0 / z, nan = z / z;
    h = mix(h, fbits(inf)); h = mix(h, nan != nan);
    h = mix(h, fbits(1e-310 * 0.5)); h = mix(h, fbits32((float)1e-40));
    return h;
}

// written so clang vectorises it at -O2 (NEON loads, arithmetic, reductions)
static uint8_t b8[1024], c8[1024];
static uint16_t s16[512];
static int32_t i32[256], j32[256];
static float f32a[256], f32b[256];
NOINLINE uint64_t vectors(void) {
    uint64_t h = 1;
    for (int i = 0; i < 1024; i++) { b8[i] = (uint8_t)rnd(); c8[i] = (uint8_t)rnd(); }
    for (int i = 0; i < 512; i++) s16[i] = (uint16_t)rnd();
    for (int i = 0; i < 256; i++) { i32[i] = (int32_t)rnd(); j32[i] = (int32_t)rnd(); f32a[i] = (float)(int16_t)rnd() / 8; f32b[i] = (float)(int16_t)rnd() / 3; }
    uint32_t sum = 0; for (int i = 0; i < 1024; i++) sum += b8[i]; h = mix(h, sum);
    uint8_t mx = 0; for (int i = 0; i < 1024; i++) mx = b8[i] > mx ? b8[i] : mx; h = mix(h, mx);
    for (int i = 0; i < 1024; i++) c8[i] = (uint8_t)((b8[i] + c8[i] + 1) >> 1);
    for (int i = 0; i < 1024; i++) c8[i] = b8[i] > c8[i] ? b8[i] - c8[i] : c8[i] - b8[i];
    uint64_t sad = 0; for (int i = 0; i < 1024; i++) sad += c8[i]; h = mix(h, sad);
    int64_t dot = 0; for (int i = 0; i < 256; i++) dot += (int64_t)i32[i] * j32[i]; h = mix(h, (uint64_t)dot);
    for (int i = 0; i < 256; i++) i32[i] = (i32[i] >> 3) + (j32[i] < 0 ? -j32[i] : j32[i]);
    int32_t mn = 0x7fffffff; for (int i = 0; i < 256; i++) mn = i32[i] < mn ? i32[i] : mn; h = mix(h, (uint64_t)mn);
    uint32_t x = 0; for (int i = 0; i < 256; i++) x ^= (uint32_t)i32[i] * 2654435761u; h = mix(h, x);
    uint32_t s2 = 0; for (int i = 0; i < 512; i++) s2 += (uint32_t)s16[i] * s16[i]; h = mix(h, s2);
    for (int i = 0; i < 512; i++) s16[i] = (uint16_t)((s16[i] << 2) | (s16[i] >> 14));
    for (int i = 0; i < 512; i++) h = mix(h, s16[i]);
    float fs = 0; for (int i = 0; i < 256; i++) fs += f32a[i] * f32b[i]; h = mix(h, fbits32(fs));
    for (int i = 0; i < 256; i++) f32a[i] = f32a[i] * 0.5f + f32b[i];
    for (int i = 0; i < 256; i++) h = mix(h, fbits32(f32a[i]));
    for (int i = 0; i < 256; i++) h = mix(h, (uint64_t)(int32_t)f32a[i]);
    uint8_t sat[64]; for (int i = 0; i < 64; i++) { int v = b8[i] + b8[i + 64]; sat[i] = v > 255 ? 255 : (uint8_t)v; }
    for (int i = 0; i < 64; i++) h = mix(h, sat[i]);
    for (int i = 0; i < 1024; i++) h = mix(h, c8[i]);
    return h;
}


int main(int cpu) {
    (void)cpu;
    OUT("ints", ints());
    OUT("floats", floats());
    OUT("vectors", vectors());
    return 0;
}
