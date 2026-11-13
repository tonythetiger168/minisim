// C side of tests/spec/35.7-export-array-args.sv: sized array arguments of
// exports (35.7).
#include <stdio.h>
#include "svdpi.h"

extern int sv_weighted(const int* a);
extern void sv_fill(char* o, const char* tag);
extern int sv_scale(double* r);

int c_run(void) {
    int a[4] = {1, 2, 3, 4};
    char o[3] = {0, 0, 0};
    double r[2] = {1.5, 2.0};
    int s = sv_weighted(a);
    sv_fill(o, "x");
    sv_scale(r);
    printf("C: sum=%d o=%x %x %x r=%g %g\n", s, o[0], o[1], o[2], r[0], r[1]);
    fflush(stdout);
    return s;
}
