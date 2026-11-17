// core_portme.c - CoreMark's port to a bare RISC-V hart (see core_portme.h).
// Time is the hart's cycle counter at BENCH_HZ (mvplat's default 100 MHz).
// The program's exit status is 0 only when CoreMark validated its results.
#include <stdarg.h>
#include "coremark.h"
#include "../inc/rt_bench.h"

void exit(int code) __attribute__((noreturn));

#ifndef BENCH_HZ
#define BENCH_HZ 100000000ull
#endif

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;
ee_u32 default_num_contexts = 1;

static CORE_TICKS start_time_val, stop_time_val;
void start_time(void) { start_time_val = bench_cycles(); }
void stop_time(void) { stop_time_val = bench_cycles(); }
CORE_TICKS get_time(void) { return stop_time_val - start_time_val; }
secs_ret time_in_secs(CORE_TICKS ticks) { return (secs_ret)(ticks / BENCH_HZ); }

// CoreMark reports a wrong result only in what it prints ("[n]ERROR! list
// crc ..."); watch for it to set the exit status. The run-time rule (at
// least 10 s) is not a wrong result and does not count.
static int failed;
static int has(const char* s, const char* w) {
    for (; *s; s++) {
        const char *a = s, *b = w;
        while (*b && *a == *b) a++, b++;
        if (!*b) return 1;
    }
    return 0;
}
int ee_printf(const char* fmt, ...) {
    if (has(fmt, "ERROR! list") || has(fmt, "ERROR! matrix") || has(fmt, "ERROR! state") ||
        has(fmt, "Cannot validate"))
        failed = 1;
    va_list ap;
    va_start(ap, fmt);
    int r = bench_vprintf(fmt, ap);
    va_end(ap);
    return r;
}
void portable_init(core_portable* p, int* argc, char* argv[]) {
    (void)argc; (void)argv;
    p->portable_id = 1;
}
void portable_fini(core_portable* p) {
    p->portable_id = 0;
    exit(failed);
}
