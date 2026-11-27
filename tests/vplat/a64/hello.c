// hello.c - prints through the PL011, echoes what arrives on it (-input)
// within a short time, and exits 0 through the finisher.
#include "rt.h"
void trap(int kind, struct frame* f) { puts_("trap "); putd(kind); puts_(" esr "); putx(SYSREG_READ(esr_el1)); puts_("\n"); exit_(9); }
void secondary(int cpu) { (void)cpu; }
int main(int cpu) {
    (void)cpu;
    puts_("hello from aarch64\n");
    REG32(UART0 + 0x38) = 0x10;   // IMSC.RX: input is held until a program asks for it
    char got[32];
    int n = 0;
    for (int spin = 0; spin < 400000 && n < 31; spin++)
        if (!(REG32(UART0 + 0x18) & 0x10)) { got[n++] = (char)REG32(UART0); spin = 0; }   // FR.RXFE
    got[n] = 0;
    if (n) { puts_("received '"); puts_(got); puts_("'\n"); }
    return 0;
}
void exit(int code) { exit_(code); }
