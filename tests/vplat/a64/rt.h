// rt.h - what the AArch64 test programs need from mvplat's built-in board:
// the PL011 UART, the test finisher, the GICv3 and the firmware (PSCI).
#pragma once
#include <stdint.h>

#define UART0  0x09000000u
#define FINISH 0x00100000u
#define GICD   0x08000000u
#define GICR   0x080a0000u
#define REG32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define REG64(a) (*(volatile uint64_t*)(uintptr_t)(a))
#define SYSREG_READ(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " #r : "=r"(_v)); _v; })
#define SYSREG_WRITE(r, v) __asm__ volatile("msr " #r ", %0" :: "r"((uint64_t)(v)))

struct frame { uint64_t x[31]; uint64_t elr, spsr, pad; };

static inline void putc_(char c) {
    while (REG32(UART0 + 0x18) & 0x20) {}
    REG32(UART0) = (uint8_t)c;
}
static inline void puts_(const char* s) { while (*s) putc_(*s++); }
static inline void putx(uint64_t v) {
    for (int i = 60; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]);
}
static inline void putd(uint64_t v) {
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) putc_(b[--n]);
}
__attribute__((noreturn)) static inline void exit_(int code) {
    REG32(FINISH) = code ? (0x3333u | (uint32_t)code << 16) : 0x5555u;
    for (;;) {}
}
// HVC to the firmware: function, three arguments; x0 back
static inline int64_t psci(uint64_t fn, uint64_t a, uint64_t b, uint64_t c) {
    register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = a, x2 __asm__("x2") = b, x3 __asm__("x3") = c;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}
static int fails;
static inline void check(const char* what, int ok) {
    if (!ok) { puts_("FAIL "); puts_(what); puts_("\n"); fails++; }
}
