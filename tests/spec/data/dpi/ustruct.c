// C side of tests/spec/35.5.6-unpacked-struct.sv: unpacked structs as C
// structs (35.5.6, H.7.6).
#include "svdpi.h"

typedef struct { short x, y; } point_t;
typedef struct {
    char id;
    point_t at;
    int hist[3];
    double gain;
    float bias;
    svBit on;
    svBitVecVal mask[SV_PACKED_DATA_NELEMS(41)];
    svLogicVecVal nib[SV_PACKED_DATA_NELEMS(4)];
    long long big;
} rec_t;

// 1 when every member arrived as the SV side set it.
int c_check(const rec_t* r) {
    int ok = r->id == -5 && r->at.x == 100 && r->at.y == -200;
    ok = ok && r->hist[0] == 1 && r->hist[1] == 2 && r->hist[2] == 3;
    ok = ok && r->gain == 2.5 && r->bias == -0.75f && r->on == 1;
    ok = ok && r->mask[0] == 0x3456789Au && r->mask[1] == 0x12;
    // nib = 4'b1x0z: aval 1 1 0 0 -> x and z set bval; aval bit 2 (x) = 1
    ok = ok && (r->nib[0].bval & 0xF) == 0x5 && (r->nib[0].aval & 0xF) == 0xC;
    ok = ok && r->big == -123456789012LL;
    return ok;
}
void c_make(int seed, rec_t* r) {
    r->id = (char)seed;
    r->at.x = (short)(seed + 1);
    r->at.y = (short)-(seed + 2);
    for (int k = 0; k < 3; k++) r->hist[k] = seed * 10 + k;
    r->gain = seed / 2.0;
    r->bias = 0.5f;
    r->on = 0;
    r->mask[0] = 3;
    r->mask[1] = 0x100;
    r->nib[0].aval = 0x6;   // z 1 x 0, from bit 3 down
    r->nib[0].bval = 0xA;
    r->big = -seed;
}
void c_bump(point_t* p) {
    p->x += 1;
    p->y += 1;
}
