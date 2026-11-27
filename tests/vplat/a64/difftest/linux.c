#include <stdint.h>
void dumpall(void);
static long sys(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
void emit(const char* s, unsigned n) { sys(64, 1, (long)s, n); }
void _start(void) { dumpall(); sys(93, 0, 0, 0); for (;;) {} }
