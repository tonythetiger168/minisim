/* C side of 35.5.3-context.sv and 35.7-export-functions.sv. */
#include <stdio.h>
#include <string.h>
#include "svdpi.h"

/* exports defined in SystemVerilog */
extern int sv_get(void);
extern void sv_set(int v);
extern int sv_mul(int a, int b, int* hi);
extern double sv_half(double x);
extern const char* sv_name(int n);

void where(void) {
    const char* f; int line;
    svScope s = svGetScope();
    printf("scope %s", svGetNameFromScope(s));
    if (svGetCallerInfo(&f, &line)) {
        const char* base = strrchr(f, '/');
        printf(" called from %s:%d", base ? base + 1 : f, line);
    }
    printf("\n");
    fflush(stdout);
}
/* per-instance data kept with svPutUserData */
static int key;
int bump(void) {
    svScope s = svGetScope();
    int* n = (int*)svGetUserData(s, &key);
    if (!n) { static int store[8]; static int used; n = &store[used++]; svPutUserData(s, &key, n); }
    return ++*n;
}
/* call the export of each instance by name */
void poke_all(void) {
    const char* names[] = {"top.u0", "top.u1"};
    for (int i = 0; i < 2; i++) {
        svScope old = svSetScope(svGetScopeFromName(names[i]));
        sv_set(sv_get() + 100 * (i + 1));
        svSetScope(old);
    }
}
int use_exports(int a) {
    int hi;
    int lo = sv_mul(a, 1 << 20, &hi);
    printf("lo=%d hi=%d half=%g name=%s\n", lo, hi, sv_half(a), sv_name(a));
    fflush(stdout);
    return lo;
}
