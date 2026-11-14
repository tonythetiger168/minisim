// vec.c - the V extension: every kernel of vkern.c built twice, scalar
// (RV64GC) and vectorized by clang (RV64GCV), must give the same result.
#include "rt.h"

void exit(int code) { exit_(code); }

#if __riscv_xlen == 32
// RV32 has no 64-bit divide: the helpers the compiler calls for one.
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t* rem) {
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ull << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; __udivmoddi4(n, d, &r); return r; }
int64_t __divdi3(int64_t n, int64_t d) {
    uint64_t q = __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, 0);
    return (n < 0) != (d < 0) ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d) {
    uint64_t r;
    __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
#endif

void vk_init_s(void), vk_init_v(void);
extern uint64_t (*const vk_all_s[])(void);
extern uint64_t (*const vk_all_v[])(void);
static const char* const names[] = {"arith8", "arith16", "arith32", "arith64", "minmax", "reduce", "widen",
                                    "extend", "narrow", "saturate", "select", "gather", "strided", "float",
                                    "fconv", "freduce", "copy"};

int main(int hart) {
    if (hart) for (;;) {}
    csr_set(mstatus, (3u << 9) | (3u << 13));  // VS and FS on
    puts_("vec: VLEN ");
    putd((uint32_t)csr_read(vlenb) * 8);
    puts_("\n");
    vk_init_s();
    vk_init_v();
    int fails = 0, n = 0;
    for (int k = 0; vk_all_s[k]; k++, n++) {
        uint64_t s = vk_all_s[k](), v = vk_all_v[k]();
        if (s != v) {
            puts_("FAIL ");
            puts_(names[k]);
            puts_("\n");
            fails++;
        }
    }
    puts_("vec: ");
    putd((uint32_t)(n - fails));
    puts_(" of ");
    putd((uint32_t)n);
    puts_(" kernels agree\n");
    return fails;
}
