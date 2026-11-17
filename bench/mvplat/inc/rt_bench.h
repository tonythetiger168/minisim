// rt_bench.h - what rt.c gives the benchmark ports.
#pragma once
#include <stdint.h>
#include <stdarg.h>
void bench_putc(char c);
uint64_t bench_cycles(void);
int bench_vprintf(const char* f, va_list ap);
