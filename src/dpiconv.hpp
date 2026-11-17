// dpiconv.hpp - DPI-C values between SystemVerilog and C (IEEE 1800-2023
// 35.5.6, annex H): the C storage of each argument type. The interpreter's
// DPI (dpi.inc) and msim -compile's direct calls (codegen.hpp dpiCall) share it.
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>

namespace dpi {
// svLogic and svLogicVecVal as svdpi.h has them (annex H.10.1.1).
enum : uint8_t { kSv0 = 0, kSv1 = 1, kSvZ = 2, kSvX = 3 };
struct LogicVec { uint32_t aval, bval; };

inline int nwords(int w) { return (w + 31) / 32; }
inline size_t elemBytes(DpiType t, int w) {
    switch (t) {
        case DT_BYTE: case DT_BIT: case DT_LOGIC: return 1;
        case DT_SHORT: return 2;
        case DT_INT: case DT_SHORTREAL: return 4;
        case DT_LONG: case DT_REAL: case DT_CHANDLE: case DT_STRING: return 8;
        case DT_BITVEC: return 4 * (size_t)nwords(w);
        case DT_LOGICVEC: return 8 * (size_t)nwords(w);
        default: return 0;
    }
}
inline uint64_t bits64(const Val& v) { return v.v & ~v.x; }
// Bit i of v as svLogic: 0, 1, z = 2, x = 3.
inline uint8_t logicAt(const Val& v, int i) {
    int b = bitAt(v, i);
    return b == 0 ? kSv0 : b == 1 ? kSv1 : b == 3 ? kSvZ : kSvX;
}

// Write an SV value into C storage at p (C layout of type t).
// strs keeps the text of strings alive for the call.
inline void toC(void* p, DpiType t, int w, const Val& v, std::deque<std::string>& strs) {
    switch (t) {
        case DT_BYTE: { int8_t x = (int8_t)bits64(v); memcpy(p, &x, 1); break; }
        case DT_SHORT: { int16_t x = (int16_t)bits64(v); memcpy(p, &x, 2); break; }
        case DT_INT: { int32_t x = (int32_t)bits64(v); memcpy(p, &x, 4); break; }
        case DT_LONG: { int64_t x = (int64_t)bits64(v); memcpy(p, &x, 8); break; }
        case DT_REAL: { double d = v.real ? v_real(v) : (double)bits64(v); if (!v.real) memcpy(&d, &v.v, 8); memcpy(p, &d, 8); break; }
        case DT_SHORTREAL: { double d; memcpy(&d, &v.v, 8); float f = (float)d; memcpy(p, &f, 4); break; }
        case DT_CHANDLE: { void* x = (void*)(uintptr_t)bits64(v); memcpy(p, &x, 8); break; }
        case DT_STRING: {
            Val s = v;
            s.str = true;
            strs.push_back(v_str(s));
            const char* c = strs.back().c_str();
            memcpy(p, &c, sizeof c);
            break;
        }
        case DT_BIT: { uint8_t b = bitAt(v, 0) == 1; memcpy(p, &b, 1); break; }
        case DT_LOGIC: { uint8_t b = logicAt(v, 0); memcpy(p, &b, 1); break; }
        case DT_BITVEC: {
            auto* o = (uint32_t*)p;
            for (int k = 0; k < nwords(w); k++) {
                uint64_t l = limbV(v, k / 2) & ~limbX(v, k / 2);
                o[k] = (uint32_t)(l >> (32 * (k % 2)));
            }
            int r = w % 32;
            if (r) o[nwords(w) - 1] &= (1u << r) - 1;
            break;
        }
        case DT_LOGICVEC: {
            auto* o = (LogicVec*)p;
            for (int k = 0; k < nwords(w); k++) {
                int sh = 32 * (k % 2);
                uint64_t lv = limbV(v, k / 2), lx = limbX(v, k / 2), lz = limbZ(v, k / 2);
                uint32_t a = (uint32_t)(lv >> sh), x = (uint32_t)(lx >> sh), z = (uint32_t)(lz >> sh);
                o[k].aval = (a & ~x) | (x & ~z);   // x: 1/1, z: 0/1
                o[k].bval = x;
            }
            int r = w % 32;
            if (r) { o[nwords(w) - 1].aval &= (1u << r) - 1; o[nwords(w) - 1].bval &= (1u << r) - 1; }
            break;
        }
        default: break;
    }
}

// Read C storage at p back into an SV value of width w.
inline Val fromC(const void* p, DpiType t, int w, bool sgn) {
    Val r;
    switch (t) {
        case DT_BYTE: { int8_t x; memcpy(&x, p, 1); r = make((uint64_t)(int64_t)x, 0, w); break; }
        case DT_SHORT: { int16_t x; memcpy(&x, p, 2); r = make((uint64_t)(int64_t)x, 0, w); break; }
        case DT_INT: { int32_t x; memcpy(&x, p, 4); r = make((uint64_t)(int64_t)x, 0, w); break; }
        case DT_LONG: { int64_t x; memcpy(&x, p, 8); r = make((uint64_t)x, 0, w); break; }
        case DT_REAL: { double d; memcpy(&d, p, 8); return mkreal(d); }
        case DT_SHORTREAL: { float f; memcpy(&f, p, 4); return mkreal((double)f); }
        case DT_CHANDLE: { void* x; memcpy(&x, p, 8); r = make((uint64_t)(uintptr_t)x, 0, 64); break; }
        case DT_STRING: { const char* c; memcpy(&c, p, sizeof c); return mkstr(c ? c : ""); }
        case DT_BIT: { uint8_t b; memcpy(&b, p, 1); r = make(b & 1, 0, 1); break; }
        case DT_LOGIC: {
            uint8_t b; memcpy(&b, p, 1);
            r = b == kSv0 ? make(0, 0, 1) : b == kSv1 ? make(1, 0, 1) : b == kSvZ ? make(0, 1, 1, 1) : make(0, 1, 1);
            break;
        }
        case DT_BITVEC: {
            auto* in = (const uint32_t*)p;
            r = make(0, 0, w);
            for (int k = 0; k < nwords(w); k++) {
                int l = k / 2, sh = 32 * (k % 2);
                setLimb(r, l, limbV(r, l) | ((uint64_t)in[k] << sh), 0, 0);
            }
            r = finish(r);
            break;
        }
        case DT_LOGICVEC: {
            auto* in = (const LogicVec*)p;
            r = make(0, 0, w);
            for (int k = 0; k < nwords(w); k++) {
                int l = k / 2, sh = 32 * (k % 2);
                uint32_t a = in[k].aval, b = in[k].bval;
                uint64_t v = (uint64_t)(a & ~b) << sh, x = (uint64_t)b << sh, z = (uint64_t)(b & ~a) << sh;
                setLimb(r, l, limbV(r, l) | v, limbX(r, l) | x, limbZ(r, l) | z);
            }
            r = finish(r);
            break;
        }
        default: r = make(0, 0, w > 0 ? w : 1); break;
    }
    r.sgn = sgn;
    return r;
}

}  // namespace dpi
