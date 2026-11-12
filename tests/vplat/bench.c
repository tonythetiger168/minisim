// bench.c - a compute-bound workload for measuring speed: every hart runs a
// sieve of Eratosthenes and a CRC-32 over its own buffer, then hart 0 prints
// the checksums. Harts share nothing until the end.
#include "rt.h"

#ifndef ROUNDS
#define ROUNDS 40
#endif
#define N 8192
#define MAXH 16

static uint8_t sieve[MAXH][N];
static volatile uint32_t result[MAXH], done, arrived;

void exit(int code) {
    if (csr_read(mhartid) == 0) exit_(code);
    for (;;) __asm__ volatile("wfi");
}

static uint32_t crc32(const uint8_t* p, uint32_t n, uint32_t c) {
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

int main(int hart) {
    __atomic_fetch_add(&arrived, 1, __ATOMIC_ACQ_REL);
    for (volatile int i = 0; i < 500; i++) {}
    uint32_t nh = __atomic_load_n(&arrived, __ATOMIC_ACQUIRE);
    uint8_t* s = sieve[hart];
    uint32_t acc = (uint32_t)hart;
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < N; i++) s[i] = 1;
        for (int i = 2; i * i < N; i++)
            if (s[i]) for (int j = i * i; j < N; j += i) s[j] = 0;
        uint32_t primes = 0;
        for (int i = 2; i < N; i++) primes += s[i];
        acc = crc32(s, 1024, acc) + primes * (uint32_t)(r + 1);
    }
    result[hart] = acc;
    __atomic_fetch_add(&done, 1, __ATOMIC_ACQ_REL);
    if (hart) return 0;
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != nh) __asm__ volatile("nop");
    for (uint32_t h = 0; h < nh; h++) { puts_("hart "); putd(h); puts_(" "); putx(result[h]); puts_("\n"); }
    return 0;
}
