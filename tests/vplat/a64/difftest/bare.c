#include "rt.h"
void dumpall(void);
void emit(const char* s, unsigned n) { while (n--) { while (REG32(UART0 + 0x18) & 0x20) {} REG32(UART0) = (uint8_t)*s++; } }
void trap(int kind, struct frame* f) { if (kind == 4 && (SYSREG_READ(esr_el1) >> 26) == 0) { f->elr += 4; return; } puts_("trap "); putd(kind); puts_(" esr "); putx(SYSREG_READ(esr_el1)); puts_(" elr "); putx(SYSREG_READ(elr_el1)); puts_("\n"); exit_(9); }
void secondary(int cpu) { (void)cpu; }
void exit(int code) { exit_(code); }
int main(int cpu) { (void)cpu; dumpall(); return 0; }
