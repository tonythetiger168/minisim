// fpu.hpp - the F and D extensions: IEEE 754 single and double arithmetic
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

}  // namespace vp::fpu
