// svdpi_bits.inc - the svdpi.h functions that only work on C data (IEEE
// 1800-2023 annex H.10): bits and parts of svBitVecVal / svLogicVecVal.
// The interpreter's DPI (dpi.inc) and msim -compile's simv (codegen.hpp,
// with DPI-C imports) both define them, inside extern "C", after svdpi.h.
const char* svDpiVersion(void) { return "1800-2023"; }

svBit svGetBitselBit(const svBitVecVal* s, int i) { return (s[i / 32] >> (i % 32)) & 1; }
svLogic svGetBitselLogic(const svLogicVecVal* s, int i) {
    uint32_t a = (s[i / 32].aval >> (i % 32)) & 1, b = (s[i / 32].bval >> (i % 32)) & 1;
    return (svLogic)(b << 1 | a);   // 0/0 0, 1/0 1, 0/1 z(2), 1/1 x(3)
}
void svPutBitselBit(svBitVecVal* d, int i, svBit s) {
    uint32_t m = 1u << (i % 32);
    d[i / 32] = (s & 1) ? (d[i / 32] | m) : (d[i / 32] & ~m);
}
void svPutBitselLogic(svLogicVecVal* d, int i, svLogic s) {
    uint32_t m = 1u << (i % 32);
    uint32_t a = s == sv_1 || s == sv_x, b = s == sv_z || s == sv_x;
    d[i / 32].aval = a ? (d[i / 32].aval | m) : (d[i / 32].aval & ~m);
    d[i / 32].bval = b ? (d[i / 32].bval | m) : (d[i / 32].bval & ~m);
}
void svGetPartselBit(svBitVecVal* d, const svBitVecVal* s, int i, int w) {
    for (int k = 0; k < (w + 31) / 32; k++) d[k] = 0;
    for (int k = 0; k < w; k++) svPutBitselBit(d, k, svGetBitselBit(s, i + k));
}
void svGetPartselLogic(svLogicVecVal* d, const svLogicVecVal* s, int i, int w) {
    for (int k = 0; k < (w + 31) / 32; k++) d[k].aval = d[k].bval = 0;
    for (int k = 0; k < w; k++) svPutBitselLogic(d, k, svGetBitselLogic(s, i + k));
}
void svPutPartselBit(svBitVecVal* d, const svBitVecVal s, int i, int w) {
    for (int k = 0; k < w && k < 32; k++) svPutBitselBit(d, i + k, (s >> k) & 1);
}
void svPutPartselLogic(svLogicVecVal* d, const svLogicVecVal s, int i, int w) {
    for (int k = 0; k < w && k < 32; k++) svPutBitselLogic(d, i + k, svGetBitselLogic(&s, k));
}

