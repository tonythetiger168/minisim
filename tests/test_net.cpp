// Net initial value. Same shape as the other tests: one translation unit.
// The kernel lives in minisim.cpp, so include it without the demo main.
//
// Expected values checked against Icarus Verilog 13.0 (stable) (v13_0).
// Icarus 12.0 printed the same lines. Verilator 5.052 is 2-state and is
// not the source of these asserts.
#include <iostream>

#define MINISIM_NO_MAIN
#include "minisim.cpp"

static bool is_z(Val v) {
    uint64_t m = mask(v.w);
    return v.v == 0 && v.x == m && v.z == m;
}
static bool is_x(Val v) {
    uint64_t m = mask(v.w);
    return v.v == 0 && v.x == m && v.z == 0;
}
static int bit_class(Val v) {
    if ((v.z & 1) && (v.x & 1)) return 3; // z
    if (v.x & 1) return 2;                // x
    return (int)(v.v & 1);
}
static int fail(const char* msg) {
    std::cerr << "FAIL " << msg << "\n";
    return 1;
}

int main() {
    // wire w; initial $display("%b", w);  -> z
    int w = S.sig("w", 1, true);
    if (!is_z(S.get(w))) return fail("1-bit undriven wire is not z");
    // wire [7:0] w8; -> zzzzzzzz
    int w8 = S.sig("w8", 8, true);
    if (S.get(w8).w != 8 || !is_z(S.get(w8))) return fail("8-bit undriven wire is not z");
    // reg r; logic l; still x. Default sig() is a variable.
    int r = S.sig("r", 1, false);
    int r8 = S.sig("r8", 8);
    int l = S.sig("l", 1);
    if (!is_x(S.get(r)) || !is_x(S.get(r8)) || !is_x(S.get(l))) return fail("variable did not start as x");

    // Undriven net stays z before the first drive(), then takes the driven value.
    // Icarus: d is z while the driver contributes z, then 1 after it drives 1.
    int d = S.sig("d", 1, true);
    int drv = S.newDriver(d);
    if (!is_z(S.get(d))) return fail("net was not z before drive()");
    S.drive(d, drv, mk(1));
    if (bit_class(S.get(d)) != 1) return fail("net did not take the driven 1");

    // Resolution is unchanged. Icarus: 0+1=x, 0+z=0, z+z=z, 0+x=x.
    auto resolve = [](Val a, Val b) {
        int s = S.sig("y", 1, true);
        int d0 = S.newDriver(s), d1 = S.newDriver(s);
        S.drive(s, d0, a);
        S.drive(s, d1, b);
        return bit_class(S.get(s));
    };
    if (resolve(mk(0), mk(1)) != 2) return fail("resolve 0/1 -> x");
    if (resolve(mk(0), allz(1)) != 0) return fail("resolve 0/z -> 0");
    if (resolve(allz(1), allz(1)) != 3) return fail("resolve z/z -> z");
    if (resolve(mk(0), allx(1)) != 2) return fail("resolve 0/x -> x");

    std::cout << "ok\n";
    return 0;
}
