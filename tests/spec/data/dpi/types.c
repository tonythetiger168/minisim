/* C side of 35.5.6-arg-types.sv and 35.5.4-import-decl.sv. */
#include <string.h>
#include "svdpi.h"

char c_byte(char a) { return (char)(a * 2); }
short c_short(short a) { return (short)(a - 1); }
int c_add(int a, int b) { return a + b; }
long long c_long(long long a) { return a * 1000000000LL; }
double c_real(double a, double b) { return a / b; }
svBit c_not(svBit b) { return !b; }
svLogic c_logic(svLogic l) { return l; }   /* passes 0, 1, z, x through */
const char* c_greet(const char* name) {
    static char buf[64];
    strcpy(buf, "hello ");
    strncat(buf, name, sizeof buf - 7);
    return buf;
}
int c_strlen(const char* s) { return (int)strlen(s); }

static int counter;
void* c_handle(void) { return &counter; }
int c_bump(void* h) { return ++*(int*)h; }

/* outputs and inouts */
void c_outs(int a, int* twice, double* half, svBit* odd, const char** name) {
    *twice = 2 * a;
    *half = a / 2.0;
    *odd = a & 1;
    *name = (a & 1) ? "odd" : "even";
}
void c_inc(int* x, char* b) { *x += 1; *b += 1; }

/* packed vectors in canonical form */
void c_vec(const svBitVecVal* in, svBitVecVal* out) {   /* bit [69:0] */
    out[0] = ~in[0];
    out[1] = ~in[1];
    out[2] = ~in[2] & 0x3f;
}
void c_lvec(const svLogicVecVal* in, svLogicVecVal* out) {   /* logic [7:0]: swap nibbles */
    out[0].aval = ((in[0].aval & 0xf) << 4) | ((in[0].aval >> 4) & 0xf);
    out[0].bval = ((in[0].bval & 0xf) << 4) | ((in[0].bval >> 4) & 0xf);
}
int c_bits(const svBitVecVal* v) {   /* bit [39:0]: count ones with svGetBitselBit */
    int n = 0;
    for (int i = 0; i < 40; i++) n += svGetBitselBit(v, i);
    return n;
}
void c_part(const svLogicVecVal* v, svLogicVecVal* hi) {   /* logic [15:0] -> its [15:8] */
    svGetPartselLogic(hi, v, 8, 8);
}
int c_integer(const svLogicVecVal* v) { return (int)v[0].aval + (v[0].bval ? 1000 : 0); }

/* a C name other than the SV name */
int c_sum3(int a, int b, int c) { return a + b + c; }
