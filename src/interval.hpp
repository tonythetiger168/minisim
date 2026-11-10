// interval.hpp - interval arithmetic for constraint solving, shared by the
// randomize() solver of msim (rand.inc) and the scenario solver of mpss
// (pss/solve.hpp). Nothing here knows about the simulator.
//
// Two layers:
//   ival::IV and ival::ivOp   an interval [lo, hi] of plain integers and the
//                             rule of each operator over intervals: the
//                             result holds every value the operator can give
//                             for operands inside theirs;
//   rnd::AV, wrap, conv       an interval read as w bits, signed or not, as
//                             SystemVerilog expressions are (rand.inc wraps
//                             the plain rule's result to the expression's
//                             type).
#pragma once
#include <algorithm>
#include <cstdint>

namespace ival {

using i128 = __int128;

enum class Op : uint8_t {
    Const, Var, Neg, LNot, BNot, Add, Sub, Mul, Div, Mod, Shl, Shr, Lt, Le, Gt, Ge, Eq, Ne,
    BAnd, BOr, BXor, LAnd, LOr, Pow, Cond
};

struct IV {
    i128 lo, hi;
    bool empty() const { return lo > hi; }
    bool single() const { return lo == hi; }
    bool canTrue() const { return !empty() && !(lo == 0 && hi == 0); }
    bool canFalse() const { return !empty() && lo <= 0 && hi >= 0; }
};
// Results are clamped to +-2^124: products of two 60-bit operands (the
// widest rand.inc multiplies) stay exact, and sums of two clamped values fit.
constexpr i128 kSat = (i128)1 << 124;
inline i128 sat(i128 v) { return v > kSat ? kSat : v < -kSat ? -kSat : v; }
inline IV ivOf(i128 v) { return {v, v}; }
inline IV ivEmpty() { return {1, 0}; }
inline IV ivBool(bool canF, bool canT) { return {canF ? 0 : 1, canT ? 1 : 0}; }
inline i128 mulSat(i128 a, i128 b) {
    i128 r;
    if (__builtin_mul_overflow(a, b, &r)) return ((a < 0) != (b < 0)) ? -kSat : kSat;
    return sat(r);
}
inline i128 powSat(i128 a, i128 b) {
    i128 r = 1;
    for (i128 i = 0; i < b && i < 200; i++) {
        r = mulSat(r, a);
        if (r == kSat || r == -kSat || r == 0) break;
    }
    return r;
}
inline i128 absMax(const IV& a) { return std::max(a.lo < 0 ? -a.lo : a.lo, a.hi < 0 ? -a.hi : a.hi); }
// The smallest 2^k - 1 that is >= v.
inline i128 ceilMask(i128 v) {
    i128 m = 0;
    while (m < v && m < kSat) m = m * 2 + 1;
    return m;
}

// One operator over intervals. `x` is BNot's mask: 2^w - 1 for an unsigned
// w-bit operand, -1 for a signed one. An empty interval is an undefined
// value (a division by zero) and makes the result empty.
inline IV ivOp(Op op, const IV& a, const IV& b, i128 x = 0) {
    if (a.empty() || (op > Op::BNot && b.empty())) return ivEmpty();
    bool ss = a.single() && (op <= Op::BNot || b.single());
    bool nn = a.lo >= 0 && (op <= Op::BNot || b.lo >= 0);
    switch (op) {
    case Op::Neg: return {-a.hi, -a.lo};
    case Op::LNot: return ivBool(a.canTrue(), a.canFalse());
    case Op::BNot: return x >= 0 ? IV{x - a.hi, x - a.lo} : IV{-a.hi - 1, -a.lo - 1};
    case Op::Add: return {sat(a.lo + b.lo), sat(a.hi + b.hi)};
    case Op::Sub: return {sat(a.lo - b.hi), sat(a.hi - b.lo)};
    case Op::Mul: {
        i128 p[4] = {mulSat(a.lo, b.lo), mulSat(a.lo, b.hi), mulSat(a.hi, b.lo), mulSat(a.hi, b.hi)};
        return {*std::min_element(p, p + 4), *std::max_element(p, p + 4)};
    }
    case Op::Div: {
        if (b.lo == 0 && b.hi == 0) return ivEmpty();
        if (b.lo <= 0 && b.hi >= 0) { i128 m = absMax(a); return {-m, m}; }
        i128 p[4] = {a.lo / b.lo, a.lo / b.hi, a.hi / b.lo, a.hi / b.hi};   // truncates toward 0
        return {*std::min_element(p, p + 4), *std::max_element(p, p + 4)};
    }
    case Op::Mod: {
        if (b.lo == 0 && b.hi == 0) return ivEmpty();
        if (ss) return ivOf(a.lo % b.lo);
        i128 m = absMax(b) - 1;
        if (a.lo >= 0) return {0, std::min(a.hi, m)};
        if (a.hi <= 0) return {std::max(a.lo, -m), 0};
        return {-m, m};
    }
    case Op::Shl:
        if (nn) return {b.lo >= 120 ? (a.lo ? kSat : 0) : mulSat(a.lo, (i128)1 << b.lo),
                        b.hi >= 120 ? (a.hi ? kSat : 0) : mulSat(a.hi, (i128)1 << b.hi)};
        return {-kSat, kSat};
    case Op::Shr:
        if (nn) return {b.hi >= 127 ? 0 : a.lo >> b.hi, b.lo >= 127 ? 0 : a.hi >> b.lo};
        { i128 m = absMax(a); return {-m, m}; }
    case Op::Lt: return ivBool(a.hi >= b.lo, a.lo < b.hi);
    case Op::Le: return ivBool(a.hi > b.lo, a.lo <= b.hi);
    case Op::Gt: return ivBool(a.lo <= b.hi, a.hi > b.lo);
    case Op::Ge: return ivBool(a.lo < b.hi, a.hi >= b.lo);
    case Op::Eq: return ivBool(!(ss && a.lo == b.lo), !(a.hi < b.lo || b.hi < a.lo));
    case Op::Ne: return ivBool(!(a.hi < b.lo || b.hi < a.lo), !(ss && a.lo == b.lo));
    case Op::BAnd:
        if (ss) return ivOf(a.lo & b.lo);
        if (nn) return {0, std::min(a.hi, b.hi)};
        if (a.lo >= 0) return {0, a.hi};
        if (b.lo >= 0) return {0, b.hi};
        return {-kSat, kSat};
    case Op::BOr:
        if (ss) return ivOf(a.lo | b.lo);
        if (nn) return {std::max(a.lo, b.lo), ceilMask(std::max(a.hi, b.hi))};
        return {-kSat, kSat};
    case Op::BXor:
        if (ss) return ivOf(a.lo ^ b.lo);
        if (nn) return {0, ceilMask(std::max(a.hi, b.hi))};
        return {-kSat, kSat};
    case Op::LAnd: return ivBool(a.canFalse() || b.canFalse(), a.canTrue() && b.canTrue());
    case Op::LOr: return ivBool(a.canFalse() && b.canFalse(), a.canTrue() || b.canTrue());
    case Op::Pow:
        if (b.lo < 0) return ss ? ivOf(0) : IV{-kSat, kSat};
        if (ss) return ivOf(powSat(a.lo, b.lo));
        if (nn) return {powSat(a.lo, b.lo), powSat(a.hi, b.hi)};
        return {-kSat, kSat};
    default: return ivEmpty();
    }
}

} // namespace ival

namespace rnd {

using ival::i128;

// An abstract value: every value in [lo, hi], read as w bits, signed or not.
struct AV {
    i128 lo = 0, hi = 0;
    int w = 1;
    bool sgn = false;
};
constexpr int kMaxW = 120;
inline i128 rMin(int w, bool s) { return s ? -((i128)1 << (w - 1)) : 0; }
inline i128 rMax(int w, bool s) { return s ? ((i128)1 << (w - 1)) - 1 : ((i128)1 << w) - 1; }
inline AV full(int w, bool s) {
    int cw = w > kMaxW ? kMaxW : (w < 1 ? 1 : w);
    return {rMin(cw, s), rMax(cw, s), w, s};
}
inline bool isFull(const AV& a) { return a.w > kMaxW; }
// [lo, hi] taken mod 2^w and read as (w, s).
inline AV wrap(i128 lo, i128 hi, int w, bool s) {
    if (w > kMaxW || w < 1) return full(w, s);
    i128 m = (i128)1 << w;
    if (hi - lo + 1 >= m) return full(w, s);
    auto norm = [&](i128 v) {
        v %= m;
        if (v < 0) v += m;
        if (s && v > rMax(w, true)) v -= m;
        return v;
    };
    i128 a = norm(lo), b = norm(hi);
    if (a <= b) return {a, b, w, s};
    return full(w, s);
}
inline AV conv(const AV& a, int w, bool s) {
    if (a.w == w && a.sgn == s) return a;
    if (a.w > kMaxW) return full(w, s);
    return wrap(a.lo, a.hi, w, s);
}
inline AV single(i128 v, int w, bool s) { return wrap(v, v, w, s); }
inline AV boolAV(bool canFalse, bool canTrue) { return {canFalse ? 0 : 1, canTrue ? 1 : 0, 1, false}; }
inline bool canTrue(const AV& a) { return !(a.lo == 0 && a.hi == 0); }
inline bool canFalse(const AV& a) { return a.lo <= 0 && a.hi >= 0; }
inline bool isSingle(const AV& a) { return a.w <= kMaxW && a.lo == a.hi; }

inline ival::IV iv(const AV& a) { return {a.lo, a.hi}; }
// The plain rule of `op` on a and b, read back as (w, s).
inline AV wrapOp(ival::Op op, const AV& a, const AV& b, int w, bool s) {
    ival::IV r = ival::ivOp(op, iv(a), iv(b));
    return wrap(r.lo, r.hi, w, s);
}
inline AV boolOp(ival::Op op, const AV& a, const AV& b) {
    ival::IV r = ival::ivOp(op, iv(a), iv(b));
    return {r.lo, r.hi, 1, false};
}

} // namespace rnd
