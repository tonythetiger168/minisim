/* C side of 35.8-export-tasks.sv. */
#include <stdio.h>
#include "svdpi.h"

extern int sv_wait(int cycles);          /* exported tasks return int (35.9) */
extern int sv_drive(int v, int* echo);
extern int sv_now(void);

/* an import task that waits on SV time through exported tasks */
int run_seq(int n) {
    for (int i = 0; i < n; i++) {
        int echo;
        sv_drive(i * 3, &echo);
        sv_wait(2);
        printf("C: step %d at %d echo %d\n", i, sv_now(), echo);
        fflush(stdout);
    }
    return 0;
}

/* 35.9: an import task whose process is disabled while it waits in an
 * exported task sees the export return 1, acknowledges, and returns 1. */
int long_job(void) {
    for (int i = 0; i < 10; i++) {
        if (sv_wait(1)) {
            printf("C: disabled after %d waits, state %d\n", i, svIsDisabledState());
            svAckDisabledState();
            fflush(stdout);
            return 1;
        }
    }
    return 0;
}
