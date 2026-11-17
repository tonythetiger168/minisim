// util.h - the part of riscv-tests' benchmark util.h that Dhrystone uses.
#pragma once
#include <stdint.h>
void setStats(int enable);
#define read_csr(reg) ({ unsigned long __tmp; __asm__ volatile("csrr %0, " #reg : "=r"(__tmp)); __tmp; })
