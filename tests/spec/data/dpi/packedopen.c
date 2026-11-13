/* C side of 35.5.6.1-packed-open-arrays.sv. */
#include <stdio.h>
#include "svdpi.h"

/* input bit [] v: count the ones, whatever the width */
int popcount(const svOpenArrayHandle h) {
    int w = svSize(h, 0), n = 0;
    svBitVecVal buf[8] = {0};
    svGetBitArrElemVecVal(buf, h, 0);
    for (int i = 0; i < w; i++) n += svGetBitselBit(buf, i);
    printf("C: [%d:%d] width %d ones %d\n", svLeft(h, 0), svRight(h, 0), w, n);
    fflush(stdout);
    return n;
}
/* output logic [] v: bit i set for even i, X on the top bit */
void evens(const svOpenArrayHandle h) {
    int w = svSize(h, 0);
    svLogicVecVal buf[8] = {{0, 0}};
    for (int i = 0; i < w; i++) svPutBitselLogic(buf, i, i == w - 1 ? sv_x : (i % 2 == 0 ? sv_1 : sv_0));
    svPutLogicArrElemVecVal(h, buf, 0);
}
/* input bit [] a []: the elements' sum, each element up to 64 bits */
long long sum_rows(const svOpenArrayHandle h) {
    long long s = 0;
    printf("C: %d rows of %d bits\n", svSize(h, 1), svSize(h, 0));
    for (int i = svLow(h, 1); i <= svHigh(h, 1); i++) {
        svBitVecVal buf[2] = {0, 0};
        svGetBitArrElem1VecVal(buf, h, i);
        s += (long long)buf[0] | ((long long)buf[1] << 32);
    }
    fflush(stdout);
    return s;
}
