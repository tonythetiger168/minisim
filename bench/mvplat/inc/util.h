// util.h - the part of riscv-tests' benchmark util.h that Dhrystone uses.
#pragma once
#include <stdint.h>
void setStats(int enable);
#if defined(__aarch64__)
#include "rt_bench.h"
#define read_csr(reg) ((unsigned long)bench_cycles())  /* mcycle and minstret: the counter */
#else
#define read_csr(reg) ({ unsigned long __tmp; __asm__ volatile("csrr %0, " #reg : "=r"(__tmp)); __tmp; })
#endif
