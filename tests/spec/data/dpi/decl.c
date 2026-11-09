/* C side of 35.5.4-import-decl.sv. */
#include <stdio.h>
#include "svdpi.h"

int c_sum3(int a, int b, int c) { return a + b + c; }
int c_sq(int a) { return a * a; }
int c_twice(int a) { return 2 * a; }
static int calls;
int c_count(void) { return ++calls; }
/* an import task returns 0, or 1 when it was disabled (35.9) */
int c_task(int n, int* out) { *out = n + 1; return 0; }
void c_hello(void) { printf("hello from C\n"); fflush(stdout); }
