// expr.hpp - 4-state (0/1/X) values, Verilog operators and an expression tree.
// Replaces Val/mk/add from minisim2.cpp. Up to 64 bits per value.
//
// Encoding: bit i is unknown if x bit i = 1; otherwise its value is v bit i.
// An unknown bit is Z if z bit i = 1 (z is always a subset of x), else X.
// Most operators treat Z as X and return X (the result has no Z).
// Logical shifts (<<, >>) keep Z on bits that move and fill vacated bits
// with 0. An X or Z bit in the shift count makes the whole result X.
// >>> is an arithmetic right shift: a signed value fills vacated bits with
// its MSB (0, 1, X, or Z). An unsigned >>> is a logical >>.
// Val::sgn is the signedness of an expression result. Nets store bits only;
// a read comes back unsigned and the expression that reads it sets sgn.
// Add, subtract, multiply, divide, remainder, equality, and the relational
// operators are signed only when both operands are signed. Otherwise they
// zero-extend.
// ?: with a known 0/1 condition selects that arm, Z included. With an X or Z
// condition, a bit is kept when both arms have that same 0, 1, X, or Z;
// any other pair becomes X (IEEE 1364/1800 conditional operator).
// slice/concat/insert/index and === / casez / casex also preserve or inspect Z.
// Invariant (enforced by make()): v bits are 0 wherever x bits are 1,
// z is within x, and nothing is set above bit w-1.
//
// Integration with minisim2.cpp:
//   - #include "expr.hpp" and delete its Val / mk / add definitions.
//   - Sim::get(s) should return the stored Val with .w = sigs[s].w.
//   - Evaluate with: eval(expr, [&](int s){ return S.get(s); })
//   - `if (cond)` is: truth(eval(...)) == 1   (X counts as false, like Verilog)
//
// Example (cnt <= cnt + 4'd1):
//   auto e = bin(Op::Add, ref(cnt), konst(mk(1, 4)));
//   S.nba(cnt, eval(e, rd));
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct Val {
    uint64_t v = 0, x = ~0ULL;  // default: all X
    int w = 1;
    uint64_t z = 0;             // which unknown bits are Z (subset of x)
    bool sgn = false;           // expression result is signed; not stored on a net
    bool operator==(const Val& o) const { return v == o.v && x == o.x && z == o.z; }
};

inline uint64_t mask(int w) { return w >= 64 ? ~0ULL : ((1ULL << w) - 1); }
inline Val make(uint64_t v, uint64_t x, int w, uint64_t z = 0, bool sgn = false) {
    uint64_t m = mask(w);
    x &= m;
    v &= m & ~x;
    z &= x;
    return Val{v, x, w, z, sgn};
}
// Widen or truncate. signFill replicates bit (a.w-1) into the new high bits,
// including when that bit is X or Z. Truncation keeps the low bits.
inline Val v_resize(Val a, int w, bool signFill) {
    if (w == a.w) return a;
    if (w < a.w) return make(a.v, a.x, w, a.z);
    if (!signFill) return make(a.v, a.x, w, a.z);
    uint64_t sb = 1ULL << (a.w - 1);
    uint64_t hi = mask(w) & ~mask(a.w);
    return make(a.v | ((a.v & sb) ? hi : 0), a.x | ((a.x & sb) ? hi : 0), w,
                a.z | ((a.z & sb) ? hi : 0));
}
// Signed iff both operands are. Extend both to that common width first.
inline void align_signed(Val& a, Val& b) {
    bool s = a.sgn && b.sgn;
    int w = a.w > b.w ? a.w : b.w;
    a = v_resize(a, w, s);
    b = v_resize(b, w, s);
    a.sgn = b.sgn = s;
}
// Signed compare of two w-bit patterns. Same sign bit: unsigned order matches.
inline int scmp(uint64_t a, uint64_t b, int w) {
    uint64_t sign = 1ULL << (w >= 64 ? 63 : w - 1);
    bool as = a & sign, bs = b & sign;
    if (as != bs) return as ? -1 : 1;
    if (a == b) return 0;
    return a < b ? -1 : 1;
}
inline Val mk(uint64_t v, int w = 1) { return make(v, 0, w); }
inline Val allx(int w) { return make(0, ~0ULL, w); }
inline Val allz(int w) { return make(0, ~0ULL, w, ~0ULL); }
inline int wmax(const Val& a, const Val& b) { return a.w > b.w ? a.w : b.w; }

// 0, 1, or 2 (= X). v is already 0 under X bits, so v != 0 means a known 1.
inline int truth(const Val& a) { return a.v ? 1 : (a.x ? 2 : 0); }
inline Val from_truth(int t) { return t == 2 ? allx(1) : mk(t, 1); }

// ---- bitwise (X-aware: 0&X=0, 1|X=1) ----
inline Val v_and(Val a, Val b) {
    uint64_t a0 = ~a.x & ~a.v, b0 = ~b.x & ~b.v;  // known zeros
    return make(a.v & b.v, (a.x | b.x) & ~(a0 | b0), wmax(a, b), 0, a.sgn && b.sgn);
}
inline Val v_or(Val a, Val b) {
    uint64_t k1 = a.v | b.v;  // known ones
    return make(k1, (a.x | b.x) & ~k1, wmax(a, b), 0, a.sgn && b.sgn);
}
inline Val v_xor(Val a, Val b) {
    uint64_t x = a.x | b.x;
    return make(a.v ^ b.v, x, wmax(a, b), 0, a.sgn && b.sgn);
}
inline Val v_not(Val a) { return make(~a.v, a.x, a.w, 0, a.sgn); }

// ---- arithmetic: any X input -> all-X result ----
// Both signed: sign-extend to the wider operand, result stays signed.
// Either unsigned: zero-extend, result is unsigned. Two's-complement
// add/sub/mul then matches an unsigned op on those extended bits.
template <class F>
inline Val arith(Val a, Val b, F f) {
    align_signed(a, b);
    if (a.x || b.x) return make(0, ~0ULL, a.w, 0, a.sgn);
    return make(f(a.v, b.v), 0, a.w, 0, a.sgn);
}
inline Val v_add(Val a, Val b) { return arith(a, b, [](uint64_t x, uint64_t y) { return x + y; }); }
inline Val v_sub(Val a, Val b) { return arith(a, b, [](uint64_t x, uint64_t y) { return x - y; }); }
inline Val v_mul(Val a, Val b) { return arith(a, b, [](uint64_t x, uint64_t y) { return x * y; }); }

// Negate a w-bit pattern. The minimum negative value has magnitude 2^(w-1),
// which fits in uint64_t through 64 bits. Negating that pattern with a
// signed integer is undefined at 64 bits, so the magnitude stays unsigned.
inline uint64_t neg_w(uint64_t v, int w) { return (0 - v) & mask(w); }
inline Val v_divmod(Val a, Val b, bool rem) {
    align_signed(a, b);
    if (a.x || b.x || b.v == 0) return make(0, ~0ULL, a.w, 0, a.sgn);
    uint64_t av = a.v, bv = b.v;
    bool nq = false, nr = false;
    if (a.sgn) {
        uint64_t sign = 1ULL << (a.w >= 64 ? 63 : a.w - 1);
        if (av & sign) { av = neg_w(av, a.w); nq = nr = true; }
        if (bv & sign) { bv = neg_w(bv, a.w); nq = !nq; }
    }
    uint64_t bits = rem ? av % bv : av / bv;
    if (rem ? nr : nq) bits = neg_w(bits, a.w);
    return make(bits, 0, a.w, 0, a.sgn);
}
inline Val v_div(Val a, Val b) { return v_divmod(a, b, false); }
inline Val v_mod(Val a, Val b) { return v_divmod(a, b, true); }

// ---- comparison (1-bit result) ----
inline Val v_eq(Val a, Val b) {
    align_signed(a, b);  // sign-extend only when both sides are signed
    if ((a.v ^ b.v) & ~a.x & ~b.x) return mk(0);  // a known bit differs
    return (a.x || b.x) ? allx(1) : mk(1);
}
inline Val v_ne(Val a, Val b) { Val e = v_eq(a, b); return e.x ? e : mk(!e.v); }
inline Val v_case_eq(Val a, Val b) { return mk(a.v == b.v && a.x == b.x && a.z == b.z); }
// casez: Z bits on either side are wildcards. casex: X and Z bits are wildcards.
// Remaining bits compare exactly (an X outside the wildcards must match X).
inline Val v_casez_eq(Val a, Val b) {
    uint64_t care = ~(a.z | b.z);
    return mk((((a.v ^ b.v) | (a.x ^ b.x)) & care) == 0);
}
inline Val v_casex_eq(Val a, Val b) {
    uint64_t care = ~(a.x | b.x);
    return mk(((a.v ^ b.v) & care) == 0);
}
inline Val v_lt(Val a, Val b) {
    align_signed(a, b);
    if (a.x || b.x) return allx(1);
    if (a.sgn) return mk(scmp(a.v, b.v, a.w) < 0);
    return mk(a.v < b.v);
}
inline Val v_le(Val a, Val b) {
    align_signed(a, b);
    if (a.x || b.x) return allx(1);
    if (a.sgn) return mk(scmp(a.v, b.v, a.w) <= 0);
    return mk(a.v <= b.v);
}
inline Val v_gt(Val a, Val b) { return v_lt(b, a); }
inline Val v_ge(Val a, Val b) { return v_le(b, a); }

// Logical << / >>. Vacated bits are 0. Z that remains in range stays Z.
// Any X or Z in the count (z is part of x) -> all X. The result keeps the
// left operand's signedness so a later widen can sign-extend it.
// v_asr (>>>) sign-fills when the left operand is signed, else it is v_shr.
inline Val v_shl(Val a, Val n) {
    if (n.x) return make(0, ~0ULL, a.w, 0, a.sgn);
    if (n.v >= 64) return make(0, 0, a.w, 0, a.sgn);
    return make(a.v << n.v, a.x << n.v, a.w, a.z << n.v, a.sgn);
}
inline Val v_shr(Val a, Val n) {
    if (n.x) return make(0, ~0ULL, a.w, 0, a.sgn);
    if (n.v >= 64) return make(0, 0, a.w, 0, a.sgn);
    return make(a.v >> n.v, a.x >> n.v, a.w, a.z >> n.v, a.sgn);
}
inline Val v_asr(Val a, Val n) {
    if (!a.sgn) return v_shr(a, n);
    if (n.x) return make(0, ~0ULL, a.w, 0, true);
    if (n.v == 0) return a;
    uint64_t sb = 1ULL << (a.w - 1);
    uint64_t m = mask(a.w);
    if (n.v >= (uint64_t)a.w) {
        return make((a.v & sb) ? m : 0, (a.x & sb) ? m : 0, a.w, (a.z & sb) ? m : 0, true);
    }
    Val r = v_shr(a, n);
    uint64_t vac = mask((int)n.v) << (a.w - (int)n.v);
    return make(r.v | ((a.v & sb) ? vac : 0), r.x | ((a.x & sb) ? vac : 0), a.w,
                r.z | ((a.z & sb) ? vac : 0), true);
}

// ---- reductions and logical ops ----
inline Val v_red_and(Val a) {
    if (mask(a.w) & ~a.x & ~a.v) return mk(0);  // some known 0
    return a.x ? allx(1) : mk(1);
}
inline Val v_red_or(Val a) { return a.v ? mk(1) : (a.x ? allx(1) : mk(0)); }
inline Val v_red_xor(Val a) { return a.x ? allx(1) : mk(__builtin_parityll(a.v)); }
inline Val v_lnot(Val a) { int t = truth(a); return t == 2 ? allx(1) : mk(!t); }
inline Val v_land(Val a, Val b) {
    int p = truth(a), q = truth(b);
    if (p == 0 || q == 0) return mk(0);
    return (p == 1 && q == 1) ? mk(1) : allx(1);
}
inline Val v_lor(Val a, Val b) {
    int p = truth(a), q = truth(b);
    if (p == 1 || q == 1) return mk(1);
    return (p == 0 && q == 0) ? mk(0) : allx(1);
}

// a[i] with a run-time index; out-of-range or unknown index -> X
inline Val v_index(Val a, Val i) {
    if (i.x || i.v >= (uint64_t)a.w) return allx(1);
    return make(a.v >> i.v, a.x >> i.v, 1, a.z >> i.v);
}
// base with bits [hi:lo] replaced by `part` (for a[hi:lo] = ... style assignments)
inline Val v_insert(Val base, Val part, int hi, int lo) {
    uint64_t m = mask(hi - lo + 1) << lo;
    uint64_t pv = lo >= 64 ? 0 : part.v << lo;
    uint64_t px = lo >= 64 ? 0 : part.x << lo;
    uint64_t pz = lo >= 64 ? 0 : part.z << lo;
    return make((base.v & ~m) | (pv & m), (base.x & ~m) | (px & m), base.w, (base.z & ~m) | (pz & m));
}

// ---- slice a[hi:lo], concat {a,b,...} (first = most significant), ?: ----
inline Val v_slice(Val a, int hi, int lo) { return make(a.v >> lo, a.x >> lo, hi - lo + 1, a.z >> lo); }
inline Val v_concat(const std::vector<Val>& parts) {
    Val r{0, 0, 0};
    for (auto& p : parts) {
        uint64_t rv = p.w >= 64 ? 0 : r.v << p.w, rx = p.w >= 64 ? 0 : r.x << p.w,
                 rz = p.w >= 64 ? 0 : r.z << p.w;
        r = make(rv | p.v, rx | p.x, r.w + p.w, rz | p.z);
    }
    return r;
}
// X/Z condition: per bit, both arms equal (0, 1, X, or Z) keeps that value;
// otherwise X. A known condition selects that arm unchanged.
inline Val v_mux(Val c, Val a, Val b) {
    int t = truth(c);
    if (t == 1) return a;
    if (t == 0) return b;
    int w = wmax(a, b);
    uint64_t m = mask(w);
    uint64_t same = ~a.x & ~b.x & ~(a.v ^ b.v) & m; // both 0 or both 1
    uint64_t bothz = a.z & b.z & m;
    return make(a.v & same, m & ~same, w, bothz, a.sgn && b.sgn);
}

// IEEE 1364 drive strength, weakest to strongest. A driver has one level for
// the 0 it can drive and one for the 1. HighZ contributes nothing.
enum : uint8_t {
    STR_HIZ = 0,
    STR_SMALL = 1,
    STR_MEDIUM = 2,
    STR_WEAK = 3,
    STR_LARGE = 4,
    STR_PULL = 5,
    STR_STRONG = 6,
    STR_SUPPLY = 7
};

// One continuous driver. s0 is the strength of a driven 0, s1 of a driven 1.
// The resolved net value is still a plain Val. Strength stays on the driver.
struct Drv {
    Val v;
    uint8_t s0 = STR_STRONG;
    uint8_t s1 = STR_STRONG;
};

// One bit of a resolved net, in the form Icarus Verilog uses.
// A 0 or a 1 is unambiguous: both strengths are that side's strength, and
// both flags match. An X keeps s0 and s1 apart and stays X, even when one
// strength is larger. Z is no drive. Combining bits takes the stronger
// unambiguous value; equal strengths on opposite values become X.
struct SBit {
    unsigned s0 = 0, s1 = 0;
    bool f0 = false, f1 = false;
};
inline bool sbit_z(SBit a) { return a.s0 == 0 && a.s1 == 0; }
inline bool sbit_unamb(SBit a) { return a.s0 == a.s1 && a.f0 == a.f1; }
inline SBit sbit_0(unsigned s) { return s ? SBit{s, s, false, false} : SBit{}; }
inline SBit sbit_1(unsigned s) { return s ? SBit{s, s, true, true} : SBit{}; }
inline SBit sbit_x(unsigned s0, unsigned s1) {
    if (s0 == 0 && s1 == 0) return {};
    return SBit{s0, s1, false, true};
}
inline int sbit_logic(SBit a) {
    if (sbit_z(a)) return 3;
    if (!a.f0 && !a.f1) return 0;
    if (a.f0 && a.f1) return 1;
    return 2;
}
inline SBit sbit_resolv(SBit a, SBit b) {
    if (sbit_z(a)) return b;
    if (sbit_z(b)) return a;
    if (a.s0 == b.s0 && a.s1 == b.s1 && a.f0 == b.f0 && a.f1 == b.f1) return a;
    if (sbit_unamb(a) && sbit_unamb(b)) {
        if (b.s0 > a.s0) return b;
        if (b.s0 == a.s0) return sbit_x(a.s0, a.s0);
        return a;
    }
    if (sbit_unamb(a) || sbit_unamb(b)) {
        SBit u = sbit_unamb(a) ? a : b;
        SBit m = sbit_unamb(a) ? b : a;
        SBit r;
        if (u.s1 > m.s1) { r.s1 = u.s1; r.f1 = u.f1; }
        else { r.s1 = m.s1; r.f1 = m.f1; }
        if (u.s0 > m.s0) { r.s0 = u.s0; r.f0 = u.f0; }
        else { r.s0 = m.s0; r.f0 = m.f0; }
        return r;
    }
    int sv1a = a.f1 ? (int)a.s1 : -(int)a.s1;
    int sv0a = a.f0 ? (int)a.s0 : -(int)a.s0;
    int sv1b = b.f1 ? (int)b.s1 : -(int)b.s1;
    int sv0b = b.f0 ? (int)b.s0 : -(int)b.s0;
    int sv1 = sv1a, sv0 = sv0a;
    if (sv0a > sv1) sv1 = sv0a;
    if (sv1b > sv1) sv1 = sv1b;
    if (sv0b > sv1) sv1 = sv0b;
    if (sv1a < sv0) sv0 = sv1a;
    if (sv1b < sv0) sv0 = sv1b;
    if (sv0b < sv0) sv0 = sv0b;
    SBit r;
    if (sv1 > 0) { r.f1 = true; r.s1 = (unsigned)sv1; }
    else { r.f1 = a.f1 && b.f1; r.s1 = (unsigned)(-sv1); }
    if (sv0 > 0) { r.f0 = true; r.s0 = (unsigned)sv0; }
    else r.s0 = (unsigned)(-sv0);
    if (sbit_z(r)) return {};
    return r;
}

inline Val resolveDrives(const Drv* d, size_t n, int w) {
    uint64_t rv = 0, rx = 0, rz = 0;
    for (int b = 0; b < w && b < 64; b++) {
        uint64_t bit = 1ULL << b;
        SBit acc;
        for (size_t i = 0; i < n; i++) {
            SBit cur;
            if (d[i].v.z & bit) cur = {};
            else if (d[i].v.x & bit) cur = sbit_x(d[i].s0, d[i].s1);
            else if (d[i].v.v & bit) cur = sbit_1(d[i].s1);
            else cur = sbit_0(d[i].s0);
            acc = sbit_resolv(acc, cur);
        }
        int lg = sbit_logic(acc);
        if (lg == 1) rv |= bit;
        else if (lg == 2) rx |= bit;
        else if (lg == 3) { rx |= bit; rz |= bit; }
    }
    return make(rv, rx, w, rz);
}

// ================= expression tree =================
enum class Op {
    Const, Ref,
    Not, And, Or, Xor, Add, Sub, Mul, Div, Mod,
    Eq, Ne, CaseEq, CaseZEq, CaseXEq, Lt, Le, Gt, Ge, Shl, Shr, Asr, Resize,
    LogNot, LogAnd, LogOr, RedAnd, RedOr, RedXor,
    Slice, Index, Concat, Mux
};
struct Expr;
using E = std::shared_ptr<Expr>;
struct Expr {
    Op op;
    Val k;            // Const
    int sig = -1;     // Ref
    int hi = 0, lo = 0;  // Slice, or Resize dest width / sign-fill
    bool sgn = false;    // Ref result, or Resize result
    std::vector<E> kids;
};

inline E node(Op op, std::vector<E> kids = {}) {
    auto e = std::make_shared<Expr>();
    e->op = op;
    e->kids = std::move(kids);
    return e;
}
inline E konst(Val k) { auto e = node(Op::Const); e->k = k; return e; }
inline E ref(int sig, bool sgn = false) {
    auto e = node(Op::Ref);
    e->sig = sig;
    e->sgn = sgn;
    return e;
}
// Resize to dstW. fill sign-extends when widening; tag is the result's signedness.
// A constant is folded. lo on a Resize node is the sign-fill flag.
inline E resize(E a, int dstW, bool fill, bool tag) {
    if (a && a->op == Op::Const) {
        Val r = v_resize(a->k, dstW, fill && dstW > a->k.w);
        r.sgn = tag;
        return konst(r);
    }
    auto e = node(Op::Resize, {std::move(a)});
    e->hi = dstW;
    e->lo = fill ? 1 : 0;
    e->sgn = tag;
    return e;
}
inline E un(Op op, E a) { return node(op, {a}); }
inline E bin(Op op, E a, E b) { return node(op, {a, b}); }
inline E slice(E a, int hi, int lo) { auto e = node(Op::Slice, {a}); e->hi = hi; e->lo = lo; return e; }
inline E concat(std::vector<E> parts) { return node(Op::Concat, std::move(parts)); }
inline E mux(E c, E a, E b) { return node(Op::Mux, {c, a, b}); }

using Reader = std::function<Val(int)>;

inline Val eval(const E& e, const Reader& rd) {
    auto k = [&](int i) { return eval(e->kids[i], rd); };
    switch (e->op) {
        case Op::Const: return e->k;
        case Op::Ref: {
            Val v = rd(e->sig);
            v.sgn = e->sgn;
            return v;
        }
        case Op::Not: return v_not(k(0));
        case Op::And: return v_and(k(0), k(1));
        case Op::Or: return v_or(k(0), k(1));
        case Op::Xor: return v_xor(k(0), k(1));
        case Op::Add: return v_add(k(0), k(1));
        case Op::Sub: return v_sub(k(0), k(1));
        case Op::Mul: return v_mul(k(0), k(1));
        case Op::Div: return v_div(k(0), k(1));
        case Op::Mod: return v_mod(k(0), k(1));
        case Op::Eq: return v_eq(k(0), k(1));
        case Op::Ne: return v_ne(k(0), k(1));
        case Op::CaseEq: return v_case_eq(k(0), k(1));
        case Op::CaseZEq: return v_casez_eq(k(0), k(1));
        case Op::CaseXEq: return v_casex_eq(k(0), k(1));
        case Op::Lt: return v_lt(k(0), k(1));
        case Op::Le: return v_le(k(0), k(1));
        case Op::Gt: return v_gt(k(0), k(1));
        case Op::Ge: return v_ge(k(0), k(1));
        case Op::Shl: return v_shl(k(0), k(1));
        case Op::Shr: return v_shr(k(0), k(1));
        case Op::Asr: return v_asr(k(0), k(1));
        case Op::Resize: {
            Val v = k(0);
            Val r = v_resize(v, e->hi, e->lo && e->hi > v.w);
            r.sgn = e->sgn;
            return r;
        }
        case Op::LogNot: return v_lnot(k(0));
        case Op::LogAnd: return v_land(k(0), k(1));
        case Op::LogOr: return v_lor(k(0), k(1));
        case Op::RedAnd: return v_red_and(k(0));
        case Op::RedOr: return v_red_or(k(0));
        case Op::RedXor: return v_red_xor(k(0));
        case Op::Slice: return v_slice(k(0), e->hi, e->lo);
        case Op::Index: return v_index(k(0), k(1));
        case Op::Concat: {
            std::vector<Val> parts;
            for (size_t i = 0; i < e->kids.size(); i++) parts.push_back(k((int)i));
            return v_concat(parts);
        }
        case Op::Mux: return v_mux(k(0), k(1), k(2));
    }
    return allx(1);
}
