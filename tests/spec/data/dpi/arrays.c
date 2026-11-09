/* C side of 35.5.6.1-open-arrays.sv. */
#include <stdio.h>
#include "svdpi.h"

/* int a[] : sum through svGetArrElemPtr1, walking left to right */
int sum_open(const svOpenArrayHandle h) {
    int s = 0;
    for (int i = svLow(h, 1); i <= svHigh(h, 1); i++) s += *(int*)svGetArrElemPtr1(h, i);
    return s;
}
void shape(const svOpenArrayHandle h) {
    printf("dims=%d", svDimensions(h));
    for (int d = 1; d <= svDimensions(h); d++)
        printf(" [%d:%d] low=%d high=%d inc=%d size=%d", svLeft(h, d), svRight(h, d), svLow(h, d), svHigh(h, d),
               svIncrement(h, d), svSize(h, d));
    printf(" bytes=%d\n", svSizeOfArray(h));
    fflush(stdout);
}
/* output int a[]: a[i] = i * i */
void squares(const svOpenArrayHandle h) {
    for (int i = svLow(h, 1); i <= svHigh(h, 1); i++) *(int*)svGetArrElemPtr1(h, i) = i * i;
}
/* int m[][] : m[i][j] = 10*i + j through svGetArrElemPtr2 */
void fill2(const svOpenArrayHandle h) {
    for (int i = svLow(h, 1); i <= svHigh(h, 1); i++)
        for (int j = svLow(h, 2); j <= svHigh(h, 2); j++) *(int*)svGetArrElemPtr2(h, i, j) = 10 * i + j;
}
/* bit [11:0] v[] : reverse the bits of each element */
void rev12(const svOpenArrayHandle h) {
    for (int i = svLow(h, 1); i <= svHigh(h, 1); i++) {
        svBitVecVal v, r = 0;
        svGetBitArrElem1VecVal(&v, h, i);
        for (int b = 0; b < 12; b++) svPutBitselBit(&r, 11 - b, svGetBitselBit(&v, b));
        svPutBitArrElem1VecVal(h, &r, i);
    }
}
/* logic l[] : invert 0/1, keep x/z */
void lflip(const svOpenArrayHandle h) {
    for (int i = svLow(h, 1); i <= svHigh(h, 1); i++) {
        svLogic l = svGetLogicArrElem1(h, i);
        svPutLogicArrElem1(h, l == sv_0 ? sv_1 : l == sv_1 ? sv_0 : l, i);
    }
}
/* a sized array: int a[4] is a plain C array, left index first */
int dot4(const int* a, const int* b) {
    int s = 0;
    for (int i = 0; i < 4; i++) s += a[i] * b[i];
    return s;
}
void iota4(int* a) { for (int i = 0; i < 4; i++) a[i] = 100 + i; }
/* string s[] */
int total_len(const svOpenArrayHandle h) {
    int n = 0;
    for (int i = svLow(h, 1); i <= svHigh(h, 1); i++) {
        const char* s = *(const char**)svGetArrElemPtr1(h, i);
        while (*s++) n++;
    }
    return n;
}
