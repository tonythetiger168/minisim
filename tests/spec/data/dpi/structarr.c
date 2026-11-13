/* C side of 35.5.6-struct-arrays.sv: arrays of unpacked structs. */
#include <stdio.h>
#include "svdpi.h"

typedef struct { short x; double w; int tag; } item_t;

/* input item_t a[3]: a C array of structs, lowest index first */
double weigh(const item_t* a) {
    double s = 0;
    for (int i = 0; i < 3; i++) s += a[i].x * a[i].w + a[i].tag;
    return s;
}
/* inout item_t a[]: an open array of structs, read and written in place */
void scale(const svOpenArrayHandle h) {
    printf("C: %d items of %d bytes\n", svSize(h, 1), (int)(svSizeOfArray(h) / svSize(h, 1)));
    fflush(stdout);
    for (int i = svLow(h, 1); i <= svHigh(h, 1); i++) {
        item_t* p = (item_t*)svGetArrElemPtr1(h, i);
        p->x *= 2;
        p->w += 0.5;
        p->tag = i;
    }
}
