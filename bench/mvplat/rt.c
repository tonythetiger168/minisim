// rt.c - the bare-metal run time the standard benchmarks need on mvplat
// (and on Spike or QEMU): a console, an exit with a status, the cycle
// counter, and the few C library functions they call. Written for minisim;
// no outside code.
//
// Build with -DEXIT_HTIF to leave through the HTIF tohost word (mvplat and
// Spike) and print nothing; without it the program prints on the 16550 UART
// at 0x10000000 and leaves through the SiFive test finisher at 0x100000
// (mvplat's built-in board and QEMU's virt machine).
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "inc/rt_bench.h"

#ifdef EXIT_HTIF
volatile uint64_t tohost __attribute__((section(".tohost"), aligned(8)));
volatile uint64_t fromhost __attribute__((section(".tohost"), aligned(8)));
void bench_putc(char c) { (void)c; }
void exit(int code) {
    tohost = ((uint64_t)(unsigned)code << 1) | 1;
    for (;;) {}
}
#else
#define REG8(a) (*(volatile uint8_t*)(uintptr_t)(a))
void bench_putc(char c) {
    while (!(REG8(0x10000005) & 0x20)) {}
    REG8(0x10000000) = (uint8_t)c;
}
void exit(int code) {
    *(volatile uint32_t*)(uintptr_t)0x100000 = code ? (0x3333u | (uint32_t)code << 16) : 0x5555u;
    for (;;) {}
}
#endif

uint64_t bench_cycles(void) {
    uint64_t c;
#if __riscv_xlen == 32
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("csrr %0, mcycleh" : "=r"(hi));
        __asm__ volatile("csrr %0, mcycle" : "=r"(lo));
        __asm__ volatile("csrr %0, mcycleh" : "=r"(hi2));
    } while (hi != hi2);
    c = (uint64_t)hi << 32 | lo;
#else
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
#endif
    return c;
}

// printf: %d %i %u %x %s %c %%, with l and ll, a width and 0 padding.
static void out_num(unsigned long long v, int neg, unsigned base, int width, char pad) {
    char b[24];
    int n = 0;
    do { b[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    if (neg) b[n++] = '-';
    while (n < width--) bench_putc(pad);
    while (n) bench_putc(b[--n]);
}
int bench_vprintf(const char* f, va_list ap) {
    for (; *f; f++) {
        if (*f != '%') { bench_putc(*f); continue; }
        f++;
        char pad = ' ';
        int width = 0, l = 0;
        if (*f == '0') pad = '0', f++;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') { f++; while (*f >= '0' && *f <= '9') f++; }
        while (*f == 'l') l++, f++;
        switch (*f) {
            case 'd': case 'i': {
                long long v = l ? va_arg(ap, long long) : va_arg(ap, int);
                if (l == 1) v = (long)v;
                out_num(v < 0 ? -(unsigned long long)v : (unsigned long long)v, v < 0, 10, width, pad);
                break;
            }
            case 'u': case 'x': {
                unsigned long long v = l ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
                out_num(v, 0, *f == 'u' ? 10 : 16, width, pad);
                break;
            }
            case 's': { const char* s = va_arg(ap, const char*); while (*s) bench_putc(*s++); break; }
            case 'c': bench_putc((char)va_arg(ap, int)); break;
            case '%': bench_putc('%'); break;
            default: return -1;
        }
    }
    return 0;
}
int printf(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = bench_vprintf(f, ap);
    va_end(ap);
    return r;
}
int puts(const char* s) { while (*s) bench_putc(*s++); bench_putc('\n'); return 0; }
int putchar(int c) { bench_putc((char)c); return c; }

// The C library's memory and string functions.
void* memcpy(void* d, const void* s, size_t n) {
    unsigned char* a = d; const unsigned char* b = s;
    while (n--) *a++ = *b++;
    return d;
}
void* memmove(void* d, const void* s, size_t n) {
    unsigned char* a = d; const unsigned char* b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void* memset(void* d, int c, size_t n) {
    unsigned char* a = d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}
int memcmp(const void* x, const void* y, size_t n) {
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; }
    return 0;
}
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
char* strchr(const char* s, int c) {
    for (;; s++) { if (*s == (char)c) return (char*)s; if (!*s) return 0; }
}
void abort(void) { exit(134); }
#if defined(__riscv_flen) && __riscv_flen >= 64
double sqrt(double x) {
    double r;
    __asm__("fsqrt.d %0, %1" : "=f"(r) : "f"(x));
    return r;
}
#endif
