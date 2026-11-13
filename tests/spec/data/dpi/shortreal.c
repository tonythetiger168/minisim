// C side of tests/spec/35.5.6-shortreal.sv: shortreal is a C float (35.5.6).
#include "svdpi.h"

float c_scale(float a, float k) { return a * k; }
void c_split(float a, float* whole, float* frac) {
    *whole = (float)(int)a;
    *frac = a - *whole;
}
void c_half(float* x) { *x = *x / 2.0f; }
