// fpu.hpp - the F, D and Zfh extensions: IEEE 754 half, single and double arithmetic
// with RISC-V's rounding modes, exception flags, NaN boxing and canonical
// NaNs (RISC-V unprivileged ISA, chapters 20 and 21).
//
// The arithmetic runs on the host's own IEEE hardware with its rounding mode
// set from the instruction and its exception flags read back, so results and
// flags are exact. Each operation goes through volatile operands between the
// fenv calls, so the compiler can neither fold it nor move it out of the
// rounding mode it was asked for. What the host gives differently from RISC-V
// is fixed here: NaN results are the canonical NaN, conversions to integers
// saturate, and min/max, compare and classify follow the RISC-V rules.
// Round-to-nearest-ties-to-max-magnitude (RMM) is exact for conversions to
// integers; elsewhere it rounds as round-to-nearest-even, which differs only
// for a result exactly half way between two representable values.
#pragma once

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace vp::fpu {

enum : uint32_t { NX = 1, UF = 2, OF = 4, DZ = 8, NV = 16 };
enum : unsigned { RNE = 0, RTZ = 1, RDN = 2, RUP = 3, RMM = 4 };

constexpr uint32_t QNAN32 = 0x7fc00000u;
constexpr uint64_t QNAN64 = 0x7ff8000000000000ull;
constexpr uint64_t BOX = 0xffffffff00000000ull;

inline float f32(uint32_t b) { float v; memcpy(&v, &b, 4); return v; }
inline double f64(uint64_t b) { double v; memcpy(&v, &b, 8); return v; }
inline uint32_t b32(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
inline uint64_t b64(double v) { uint64_t b; memcpy(&b, &v, 8); return b; }

// A single from a register: a value that is not NaN-boxed reads as the
// canonical NaN.
inline uint32_t unbox(uint64_t r) { return (r >> 32) == 0xffffffffu ? (uint32_t)r : QNAN32; }
inline uint64_t box(uint32_t v) { return BOX | v; }

inline bool snan32(uint32_t b) { return (b & 0x7f800000u) == 0x7f800000u && (b & 0x7fffffu) && !(b & 0x400000u); }
inline bool snan64(uint64_t b) {
    return (b & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (b & 0xfffffffffffffull) && !(b & (1ull << 51));
}

// Both widths through one set of templates.
template <class T> struct Fmt;
template <> struct Fmt<float> {
    using U = uint32_t;
    static constexpr U qnan = QNAN32;
    static U bits(float v) { return b32(v); }
    static float val(U b) { return f32(b); }
    static bool snan(U b) { return snan32(b); }
};
template <> struct Fmt<double> {
    using U = uint64_t;
    static constexpr U qnan = QNAN64;
    static U bits(double v) { return b64(v); }
    static double val(U b) { return f64(b); }
    static bool snan(U b) { return snan64(b); }
};

// The host's rounding mode set to `rm` and its flags cleared for one
// operation; flags() reads what it raised.
class Env {
public:
    explicit Env(unsigned rm) : old_(fegetround()) {
        static const int m[5] = {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD, FE_TONEAREST};
        fesetround(m[rm < 5 ? rm : 0]);
        feclearexcept(FE_ALL_EXCEPT);
    }
    ~Env() { fesetround(old_); }
    static uint32_t flags() {
        int e = fetestexcept(FE_ALL_EXCEPT);
        uint32_t f = 0;
        if (e & FE_INEXACT) f |= NX;
        if (e & FE_UNDERFLOW) f |= UF;
        if (e & FE_OVERFLOW) f |= OF;
        if (e & FE_DIVBYZERO) f |= DZ;
        if (e & FE_INVALID) f |= NV;
        return f;
    }

private:
    int old_;
};

enum class Op { Add, Sub, Mul, Div, Sqrt, Madd, Msub, Nmsub, Nmadd };

// a op b (op c), rounded by `rm`; adds the exception flags to `fl`.
template <class T>
typename Fmt<T>::U arith(Op op, typename Fmt<T>::U ua, typename Fmt<T>::U ub, typename Fmt<T>::U uc, unsigned rm,
                         uint32_t& fl) {
    using F = Fmt<T>;
    volatile T a = F::val(ua), b = F::val(ub), c = F::val(uc);
    volatile T r;
    uint32_t extra = 0;
    {
        Env env(rm);
        switch (op) {
            case Op::Add: r = a + b; break;
            case Op::Sub: r = a - b; break;
            case Op::Mul: r = a * b; break;
            case Op::Div: r = a / b; break;
            case Op::Sqrt: r = std::sqrt((T)a); break;
            case Op::Madd: r = std::fma((T)a, (T)b, (T)c); break;
            case Op::Msub: r = std::fma((T)a, (T)b, -(T)c); break;
            case Op::Nmsub: r = std::fma(-(T)a, (T)b, (T)c); break;
            case Op::Nmadd: r = std::fma(-(T)a, (T)b, -(T)c); break;
        }
        extra = Env::flags();
    }
    if (op >= Op::Madd) {
        // inf * 0 is invalid even when the addend is a quiet NaN.
        T x = a, y = b;
        if ((std::isinf(x) && y == 0) || (x == 0 && std::isinf(y))) extra |= NV;
        if (F::snan(ua) || F::snan(ub) || F::snan(uc)) extra |= NV;
    }
    fl |= extra;
    T v = r;
    return std::isnan(v) ? F::qnan : F::bits(v);
}

// FSGNJ, FSGNJN, FSGNJX (kind 0, 1, 2) on the raw bits.
template <class U> U sgnj(U a, U b, unsigned kind) {
    const U sign = (U)1 << (sizeof(U) * 8 - 1);
    U s = kind == 0 ? b & sign : kind == 1 ? ~b & sign : (a ^ b) & sign;
    return (a & ~sign) | s;
}

// FMIN, FMAX: a NaN operand yields the other; -0 is below +0.
template <class T> typename Fmt<T>::U minmax(typename Fmt<T>::U ua, typename Fmt<T>::U ub, bool max, uint32_t& fl) {
    using F = Fmt<T>;
    T a = F::val(ua), b = F::val(ub);
    if (F::snan(ua) || F::snan(ub)) fl |= NV;
    if (std::isnan(a) && std::isnan(b)) return F::qnan;
    if (std::isnan(a)) return ub;
    if (std::isnan(b)) return ua;
    if (a == b) {  // equal, or +0 and -0
        bool na = std::signbit(a);
        return max ? (na ? ub : ua) : (na ? ua : ub);
    }
    return (max ? a > b : a < b) ? ua : ub;
}

// FEQ (kind 2), FLT (1), FLE (0): FEQ signals only for a signaling NaN,
// FLT and FLE for any NaN.
template <class T> uint64_t compare(typename Fmt<T>::U ua, typename Fmt<T>::U ub, unsigned kind, uint32_t& fl) {
    using F = Fmt<T>;
    T a = F::val(ua), b = F::val(ub);
    if (std::isnan(a) || std::isnan(b)) {
        if (kind != 2 || F::snan(ua) || F::snan(ub)) fl |= NV;
        return 0;
    }
    return kind == 2 ? a == b : kind == 1 ? a < b : a <= b;
}

// FCLASS: one bit of ten.
template <class T> uint64_t classify(typename Fmt<T>::U u) {
    using F = Fmt<T>;
    T v = F::val(u);
    bool neg = std::signbit(v);
    switch (std::fpclassify(v)) {
        case FP_INFINITE: return neg ? 1 << 0 : 1 << 7;
        case FP_NORMAL: return neg ? 1 << 1 : 1 << 6;
        case FP_SUBNORMAL: return neg ? 1 << 2 : 1 << 5;
        case FP_ZERO: return neg ? 1 << 3 : 1 << 4;
        default: return F::snan(u) ? 1 << 8 : 1 << 9;
    }
}

// FCVT.W/WU/L/LU: to a signed or unsigned integer of `bits` bits, rounded
// by `rm`; a NaN or a value out of range saturates and is invalid. The
// result is the integer's bit pattern, sign-extended from `bits`.
template <class T> uint64_t toInt(typename Fmt<T>::U u, unsigned rm, bool sign, unsigned bits, uint32_t& fl) {
    T v = Fmt<T>::val(u);
    const long double maxS = std::ldexp(1.0L, (int)bits - 1), maxU = std::ldexp(1.0L, (int)bits);
    auto sat = [&](bool neg) -> uint64_t {
        fl |= NV;
        if (sign) return neg ? (uint64_t)(-(int64_t)(maxS - 1) - 1) : (uint64_t)(int64_t)(maxS - 1);
        if (neg) return 0;
        return bits == 64 ? ~0ull : (uint64_t)(int64_t)(int32_t)0xffffffffu;
    };
    if (std::isnan(v)) return sat(false);
    long double r;
    switch (rm) {
        case RTZ: r = std::trunc((long double)v); break;
        case RDN: r = std::floor((long double)v); break;
        case RUP: r = std::ceil((long double)v); break;
        case RMM: r = std::round((long double)v); break;
        default: r = std::nearbyint((long double)v); break;  // the host is in round-to-nearest-even
    }
    if (sign ? (r >= maxS || r < -maxS) : (r >= maxU || r < 0)) return sat(r < 0);
    if (r != (long double)v) fl |= NX;
    uint64_t out = sign ? (uint64_t)(int64_t)r : (uint64_t)r;
    if (bits == 32) out = (uint64_t)(int64_t)(int32_t)(uint32_t)out;
    return out;
}

// FCVT.S/D.W/WU/L/LU: from an integer, rounded by `rm`.
template <class T> typename Fmt<T>::U fromInt(uint64_t x, bool sign, unsigned bits, unsigned rm, uint32_t& fl) {
    volatile T r;
    {
        Env env(rm);
        if (bits == 32) {
            if (sign) { volatile int32_t i = (int32_t)x; r = (T)i; }
            else { volatile uint32_t i = (uint32_t)x; r = (T)i; }
        } else {
            if (sign) { volatile int64_t i = (int64_t)x; r = (T)i; }
            else { volatile uint64_t i = x; r = (T)i; }
        }
        fl |= Env::flags();
    }
    return Fmt<T>::bits(r);
}

// FCVT.S.D and FCVT.D.S.
inline uint32_t dToS(uint64_t u, unsigned rm, uint32_t& fl) {
    volatile double d = f64(u);
    volatile float r;
    {
        Env env(rm);
        r = (float)d;
        fl |= Env::flags();
    }
    if (snan64(u)) fl |= NV;
    float v = r;
    return std::isnan(v) ? QNAN32 : b32(v);
}
inline uint64_t sToD(uint32_t u, uint32_t& fl) {
    if (snan32(u)) fl |= NV;
    float s = f32(u);
    return std::isnan(s) ? QNAN64 : b64((double)s);
}

// Zfh: IEEE 754 half precision. A half widens to a double exactly; an
// operation runs in double rounded toward zero with the inexact bit kept in
// the last place (round to odd, which has more than two bits to spare), and
// that rounds to half once by the instruction's mode. Rounding to half is done
// here, with RISC-V's tininess after rounding and every mode including RMM.
constexpr uint16_t QNAN16 = 0x7e00;
inline uint16_t unbox16(uint64_t r) { return (r >> 16) == 0xffffffffffffull ? (uint16_t)r : QNAN16; }
inline uint64_t box16(uint16_t v) { return 0xffffffffffff0000ull | v; }
inline bool snan16(uint16_t b) { return (b & 0x7c00) == 0x7c00 && (b & 0x3ff) && !(b & 0x200); }

// Half to double, exact. A NaN keeps its quiet bit, so snan64 still sees a
// signaling one.
inline uint64_t h2d(uint16_t h) {
    const uint64_t s = (uint64_t)(h >> 15) << 63;
    const unsigned e = (h >> 10) & 31;
    const uint64_t m = h & 0x3ff;
    if (e == 31) return s | 0x7ff0000000000000ull | (m << 42);
    if (e == 0) return m ? s | b64(std::ldexp((double)m, -24)) : s;
    return s | ((uint64_t)(e - 15 + 1023) << 52) | (m << 42);
}

// m >> sh rounded by rm for a value of sign `neg`; nx if bits were lost.
inline uint64_t shiftRound(uint64_t m, unsigned sh, unsigned rm, bool neg, bool& nx) {
    if (sh == 0) { nx = false; return m; }
    uint64_t q, rem, half;
    if (sh >= 63) { q = 0; rem = m; half = ~0ull; }  // m < 2^54: below one half
    else { q = m >> sh; rem = m & ((1ull << sh) - 1); half = 1ull << (sh - 1); }
    nx = rem != 0;
    bool up = false;
    switch (rm) {
        case RTZ: break;
        case RDN: up = neg && rem; break;
        case RUP: up = !neg && rem; break;
        case RMM: up = rem >= half && rem; break;
        default: up = rem > half || (rem == half && (q & 1)); break;
    }
    return q + up;
}

// Double to half, rounded by rm.
inline uint16_t d2h(uint64_t u, unsigned rm, uint32_t& fl) {
    const bool neg = u >> 63;
    const uint16_t s = neg ? 0x8000 : 0;
    const int e = (int)((u >> 52) & 0x7ff);
    uint64_t m = u & ((1ull << 52) - 1);
    if (e == 0x7ff) {
        if (!m) return s | 0x7c00;
        if (!(m >> 51)) fl |= NV;
        return QNAN16;
    }
    if (e == 0 && m == 0) return s;
    int E = e - 1023;                       // the exponent of the leading bit
    if (e == 0) E = -1023; else m |= 1ull << 52;
    bool nx;
    if (E >= -14) {
        uint64_t q = shiftRound(m, 42, rm, neg, nx);
        if (q >> 11) { q >>= 1; E++; }
        if (E > 15) {                       // overflow
            fl |= OF | NX;
            bool toInf = rm == RNE || rm == RMM || (rm == RDN && neg) || (rm == RUP && !neg);
            return s | (toInf ? 0x7c00 : 0x7bff);
        }
        if (nx) fl |= NX;
        return s | (uint16_t)((E + 15) << 10) | (uint16_t)(q & 0x3ff);
    }
    uint64_t q = shiftRound(m, (unsigned)(42 + (-14 - E)), rm, neg, nx);  // subnormal; 0x400 is the least normal
    if (nx) {
        bool t;
        bool tiny = !(E == -15 && (shiftRound(m, 42, rm, neg, t) >> 11));  // tininess after rounding
        fl |= NX | (tiny ? UF : 0);
    }
    return s | (uint16_t)q;
}

// A half result from `fn`, which computes in double: rounded to odd, then to half.
template <class Fn> uint16_t halfOp(Fn fn, unsigned rm, uint32_t& fl) {
    volatile double r;
    uint32_t f;
    {
        Env env(RTZ);
        r = fn();
        f = Env::flags();
    }
    double v = r;
    if (std::isnan(v)) { fl |= f & NV; return QNAN16; }
    uint64_t b = b64(v);
    if (v == 0 && !(f & NX)) {  // an exact zero takes its sign from the real mode
        Env env(rm == RMM ? RNE : rm);
        volatile double z = fn();
        b = b64(z);
    } else if ((f & NX) && !std::isinf(v)) b |= 1;
    fl |= f & (NV | DZ);
    return d2h(b, rm, fl);
}

inline uint16_t harith(Op op, uint16_t ha, uint16_t hb, uint16_t hc, unsigned rm, uint32_t& fl) {
    volatile double a = f64(h2d(ha)), b = f64(h2d(hb)), c = f64(h2d(hc));
    uint16_t r = halfOp([&]() -> double {
        switch (op) {
            case Op::Add: return a + b;
            case Op::Sub: return a - b;
            case Op::Mul: return a * b;
            case Op::Div: return a / b;
            case Op::Sqrt: return std::sqrt((double)a);
            case Op::Madd: return std::fma((double)a, (double)b, (double)c);
            case Op::Msub: return std::fma((double)a, (double)b, -(double)c);
            case Op::Nmsub: return std::fma(-(double)a, (double)b, (double)c);
            default: return std::fma(-(double)a, (double)b, -(double)c);
        }
    }, rm, fl);
    if (op >= Op::Madd) {
        double x = a, y = b;
        if ((std::isinf(x) && y == 0) || (x == 0 && std::isinf(y))) fl |= NV;
        if (snan16(ha) || snan16(hb) || snan16(hc)) fl |= NV;
    }
    return r;
}

inline uint16_t hminmax(uint16_t a, uint16_t b, bool max, uint32_t& fl) {
    uint64_t da = h2d(a), r = minmax<double>(da, h2d(b), max, fl);
    return std::isnan(f64(r)) ? QNAN16 : r == da ? a : b;  // a NaN only when both are
}

inline uint64_t hclassify(uint16_t h) {
    const bool neg = h >> 15;
    const unsigned e = (h >> 10) & 31, m = h & 0x3ff;
    if (e == 31) return m ? (snan16(h) ? 1 << 8 : 1 << 9) : neg ? 1 << 0 : 1 << 7;
    if (e == 0) return m ? (neg ? 1 << 2 : 1 << 5) : neg ? 1 << 3 : 1 << 4;
    return neg ? 1 << 1 : 1 << 6;
}

inline uint16_t hFromInt(uint64_t x, bool sign, unsigned bits, unsigned rm, uint32_t& fl) {
    return halfOp([&]() -> double {
        uint32_t f = 0;
        return f64(fromInt<double>(x, sign, bits, RTZ, f));
    }, rm, fl);
}

}  // namespace vp::fpu
