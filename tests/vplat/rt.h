// rt.h - the few things the test programs need from the platform: the UART,
// the CLINT, the PLIC and the test finisher of mvplat's built-in board.
#pragma once
#include <stdint.h>

#define UART0   0x10000000u
#define CLINT   0x02000000u
#define PLIC    0x0c000000u
#define FINISH  0x00100000u
#define REG8(a)  (*(volatile uint8_t*)(uintptr_t)(a))
#define REG32(a) (*(volatile uint32_t*)(uintptr_t)(a))

static inline void putc_(char c) {
    while (!(REG8(UART0 + 5) & 0x20)) {}
    REG8(UART0) = (uint8_t)c;
}
static inline void puts_(const char* s) { while (*s) putc_(*s++); }
static inline void putx(uint32_t v) {
    for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]);
}
static inline void putd(uint32_t v) {
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) putc_(b[--n]);
}
__attribute__((noreturn)) static inline void exit_(int code) {
    REG32(FINISH) = code ? (0x3333u | (uint32_t)code << 16) : 0x5555u;
    for (;;) {}
}
// CSRs are XLEN wide, so these work on RV32 and RV64 alike.
#define csr_read(r) ({ uintptr_t _v; __asm__ volatile("csrr %0, " #r : "=r"(_v)); _v; })
#define csr_write(r, v) __asm__ volatile("csrw " #r ", %0" ::"r"((uintptr_t)(v)))
#define csr_set(r, v) __asm__ volatile("csrs " #r ", %0" ::"r"((uintptr_t)(v)))
// mcause of an interrupt: the top bit, and the cause.
#define MCAUSE_INT(c) (((uintptr_t)1 << (8 * sizeof(uintptr_t) - 1)) | (c))
