// boardsupport.c - Embench's board hooks for mvplat: turn the FPU on (some
// programs use floating point); the measured time is the host's wall clock
// around the whole run.
#include "boardsupport.h"
void initialise_board(void) {
#if defined(__riscv_flen)
    __asm__ volatile("csrs mstatus, %0" ::"r"(1ul << 13));  // FS = Initial
#endif
}
void start_trigger(void) {}
void stop_trigger(void) {}
