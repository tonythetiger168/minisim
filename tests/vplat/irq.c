// irq.c - interrupts: the CLINT timer (mtimecmp, WFI), a software interrupt
// (msip), and an external interrupt from the UART's receiver through the PLIC
// (claim, complete). Prints what it saw and passes if all three arrived.
#include "rt.h"

void exit(int code) { exit_(code); }

static volatile int ticks, soft, rx;
static volatile char got[8];

static uint64_t mtime(void) {
    uint32_t hi, lo;
    do { hi = REG32(CLINT + 0xbffc); lo = REG32(CLINT + 0xbff8); } while (hi != REG32(CLINT + 0xbffc));
    return (uint64_t)hi << 32 | lo;
}
static void set_cmp(uint64_t t) {
    REG32(CLINT + 0x4000 + 4) = 0xffffffff;
    REG32(CLINT + 0x4000) = (uint32_t)t;
    REG32(CLINT + 0x4000 + 4) = (uint32_t)(t >> 32);
}

__attribute__((interrupt("machine"), aligned(4))) void handler(void) {
    uint32_t cause = csr_read(mcause);
    if (cause == 0x80000007u) {          // machine timer
        ticks++;
        if (ticks < 5) set_cmp(mtime() + 1000);
        else set_cmp(~0ull);
    } else if (cause == 0x80000003u) {   // machine software
        soft++;
        REG32(CLINT) = 0;
    } else if (cause == 0x8000000bu) {   // machine external
        uint32_t id = REG32(PLIC + 0x200004);
        if (id == 10) {
            while (REG8(UART0 + 5) & 1) { if (rx < 7) got[rx] = (char)REG8(UART0); rx++; }
        }
        REG32(PLIC + 0x200004) = id;
    } else {
        puts_("unexpected trap ");
        putx(cause);
        putc_('\n');
        exit_(9);
    }
}

int main(int hart) {
    (void)hart;
    csr_write(mtvec, (uint32_t)(uintptr_t)handler);
    // PLIC: source 10 (the UART) at priority 1, enabled for hart 0, threshold 0.
    REG32(PLIC + 4 * 10) = 1;
    REG32(PLIC + 0x2000) = 1u << 10;
    REG32(PLIC + 0x200000) = 0;
    REG8(UART0 + 1) = 1;  // IER: received data available
    csr_write(mie, (1u << 7) | (1u << 3) | (1u << 11));
    set_cmp(mtime() + 1000);
    csr_set(mstatus, 1u << 3);
    while (ticks < 5) __asm__ volatile("wfi");
    REG32(CLINT) = 1;  // raise msip on ourselves
    while (!soft) {}
    while (rx < 3) __asm__ volatile("wfi");
    got[rx < 7 ? rx : 7] = 0;
    puts_("timer ticks ");
    putd(ticks);
    puts_(", software ");
    putd(soft);
    puts_(", received '");
    puts_((const char*)got);
    puts_("'\n");
    return ticks == 5 && soft == 1 && rx == 3 ? 0 : 1;
}
