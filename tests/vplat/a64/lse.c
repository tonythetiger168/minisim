// lse.c - the Armv8.1 atomics (LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN,
// LDUMAX, LDUMIN, SWP in every size, CAS, CASP) and CRC32 / CRC32C, checked
// against results worked out in C and the standard check values.
#include "rt.h"
void secondary(int cpu) { (void)cpu; }
void exit(int code) { exit_(code); }
void trap(int kind, struct frame* f) { (void)f; puts_("unexpected exception "); putd((uint64_t)kind); puts_(" esr "); putx(SYSREG_READ(esr_el1)); puts_("\n"); exit_(1); }

static uint64_t mem[4] __attribute__((aligned(16)));

#define OP(name, insn, T, R)                                                                        \
    static uint64_t name(volatile void* p, uint64_t s) {                                             \
        uint64_t o;                                                                                  \
        __asm__ volatile(insn " %" R "1, %" R "0, [%2]" : "=&r"(o) : "r"(s), "r"(p) : "memory");     \
        return o;                                                                                    \
    }
#define OPS(op) OP(op##b, #op "alb", uint8_t, "w") OP(op##h, #op "h", uint16_t, "w") OP(op##w, #op "l", uint32_t, "w") OP(op##x, #op "a", uint64_t, "x")
OPS(ldadd) OPS(ldclr) OPS(ldeor) OPS(ldset) OPS(ldsmax) OPS(ldsmin) OPS(ldumax) OPS(ldumin) OPS(swp)

static int64_t sx(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
static uint64_t ref(int op, uint64_t a, uint64_t s, int bits) {
    const uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1;
    a &= m; s &= m;
    switch (op) {
        case 0: return (a + s) & m;
        case 1: return a & ~s;
        case 2: return a ^ s;
        case 3: return a | s;
        case 4: return sx(a, bits) > sx(s, bits) ? a : s;
        case 5: return sx(a, bits) < sx(s, bits) ? a : s;
        case 6: return a > s ? a : s;
        case 7: return a < s ? a : s;
        default: return s;
    }
}
typedef uint64_t (*fn)(volatile void*, uint64_t);
static const fn ops[9][4] = {
    {ldaddb, ldaddh, ldaddw, ldaddx}, {ldclrb, ldclrh, ldclrw, ldclrx}, {ldeorb, ldeorh, ldeorw, ldeorx},
    {ldsetb, ldseth, ldsetw, ldsetx}, {ldsmaxb, ldsmaxh, ldsmaxw, ldsmaxx}, {ldsminb, ldsminh, ldsminw, ldsminx},
    {ldumaxb, ldumaxh, ldumaxw, ldumaxx}, {lduminb, lduminh, lduminw, lduminx}, {swpb, swph, swpw, swpx},
};

static uint64_t rnd = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17; return rnd; }

static uint32_t crc32(uint32_t acc, const char* s, int c) {
    for (; *s; s++) {
        uint64_t v = (uint8_t)*s;
        if (c) __asm__("crc32cb %w0, %w0, %w1" : "+r"(acc) : "r"(v));
        else __asm__("crc32b %w0, %w0, %w1" : "+r"(acc) : "r"(v));
    }
    return acc;
}

int main(void) {
    int bad = 0;
    for (int op = 0; op < 9; op++)
        for (int sz = 0; sz < 4; sz++)
            for (int k = 0; k < 40; k++) {
                const int bits = 8 << sz;
                const uint64_t init = next(), s = next();
                mem[0] = init; mem[1] = 0x5555555555555555ull;
                const uint64_t m = bits == 64 ? ~0ull : (1ull << bits) - 1;
                const uint64_t old = ops[op][sz](mem, s);
                const uint64_t want = (init & ~m) | ref(op, init, s, bits);
                if (old != (init & m) || mem[0] != want || mem[1] != 0x5555555555555555ull) bad++;
            }
    check("LDADD .. LDUMIN and SWP, every size", bad == 0);

    // CAS: equal (stored) and not (left alone); the old value comes back either way
    uint64_t r;
    mem[0] = 0x1122334455667788ull;
    r = 0x1122334455667788ull;
    __asm__ volatile("casal %0, %1, [%2]" : "+r"(r) : "r"(0xabcdull), "r"(mem) : "memory");
    check("CAS, equal", r == 0x1122334455667788ull && mem[0] == 0xabcd);
    r = 7;
    __asm__ volatile("cas %0, %1, [%2]" : "+r"(r) : "r"(0x99ull), "r"(mem) : "memory");
    check("CAS, not equal", r == 0xabcd && mem[0] == 0xabcd);
    mem[0] = 0xffffffff000000f0ull;
    uint32_t w = 0xf0;
    __asm__ volatile("casb %w0, %w1, [%2]" : "+r"(w) : "r"(0x1ffu), "r"(mem) : "memory");
    check("CASB compares and stores one byte", w == 0xf0 && mem[0] == 0xffffffff000000ffull);
    // CASP of two 64-bit registers
    mem[0] = 1; mem[1] = 2;
    register uint64_t s0 __asm__("x4") = 1, s1 __asm__("x5") = 2, t0 __asm__("x6") = 30, t1 __asm__("x7") = 40;
    __asm__ volatile("caspal x4, x5, x6, x7, [%4]" : "+r"(s0), "+r"(s1), "+r"(t0), "+r"(t1) : "r"(mem) : "memory");
    check("CASP, equal", s0 == 1 && s1 == 2 && mem[0] == 30 && mem[1] == 40);
    s0 = 30; s1 = 41;
    __asm__ volatile("casp x4, x5, x6, x7, [%4]" : "+r"(s0), "+r"(s1), "+r"(t0), "+r"(t1) : "r"(mem) : "memory");
    check("CASP, not equal", s0 == 30 && s1 == 40 && mem[0] == 30 && mem[1] == 40);

    // the check values of "123456789"
    check("CRC32", ~crc32(~0u, "123456789", 0) == 0xcbf43926u);
    check("CRC32C", ~crc32(~0u, "123456789", 1) == 0xe3069283u);
    uint32_t a = ~0u;
    __asm__("crc32x %w0, %w0, %1" : "+r"(a) : "r"(0x3837363534333231ull));
    check("CRC32X is eight CRC32B", a == crc32(~0u, "12345678", 0));
    uint64_t isar0;
    __asm__("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    check("ID_AA64ISAR0_EL1 says LSE and CRC32", (isar0 >> 20 & 15) == 2 && (isar0 >> 16 & 15) == 1);
    if (!fails) puts_("lse: all passed\n");
    return fails;
}
