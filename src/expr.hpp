// expr.hpp - 4-state (0/1/X) values, Verilog operators and an expression tree.
// Replaces Val/mk/add from minisim2.cpp. Up to 64 bits per value.
//
// Encoding: bit i is unknown if x bit i = 1; otherwise its value is v bit i.
// An unknown bit is Z if z bit i = 1 (z is always a subset of x), else X.
// Operators treat Z as X and return X; only slice/concat/insert/index/mux
// and === / casez / casex preserve or inspect the Z distinction.
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
    bool operator==(const Val& o) const { return v == o.v && x == o.x && z == o.z; }
};

inline uint64_t mask(int w) { return w >= 64 ? ~0ULL : ((1ULL << w) - 1); }
inline Val make(uint64_t v, uint64_t x, int w, uint64_t z = 0) {
    uint64_t m = mask(w);
    x &= m;
    v &= m & ~x;
    z &= x;
    return Val{v, x, w, z};
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
    return make(a.v & b.v, (a.x | b.x) & ~(a0 | b0), wmax(a, b));
}
inline Val v_or(Val a, Val b) {
    uint64_t k1 = a.v | b.v;  // known ones
    return make(k1, (a.x | b.x) & ~k1, wmax(a, b));
}
inline Val v_xor(Val a, Val b) {
    uint64_t x = a.x | b.x;
    return make(a.v ^ b.v, x, wmax(a, b));
}
inline Val v_not(Val a) { return make(~a.v, a.x, a.w); }

// ---- arithmetic: any X input -> all-X result ----
inline Val v_add(Val a, Val b) { int w = wmax(a, b); return (a.x || b.x) ? allx(w) : mk(a.v + b.v, w); }
inline Val v_sub(Val a, Val b) { int w = wmax(a, b); return (a.x || b.x) ? allx(w) : mk(a.v - b.v, w); }
inline Val v_mul(Val a, Val b) { int w = wmax(a, b); return (a.x || b.x) ? allx(w) : mk(a.v * b.v, w); }

// ---- comparison (1-bit result) ----
inline Val v_eq(Val a, Val b) {
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
inline Val v_lt(Val a, Val b) { return (a.x || b.x) ? allx(1) : mk(a.v < b.v); }
inline Val v_le(Val a, Val b) { return (a.x || b.x) ? allx(1) : mk(a.v <= b.v); }
inline Val v_gt(Val a, Val b) { return v_lt(b, a); }
inline Val v_ge(Val a, Val b) { return v_le(b, a); }

// ---- shifts (logical); unknown shift amount -> all X ----
inline Val v_shl(Val a, Val n) {
    if (n.x) return allx(a.w);
    return n.v >= 64 ? mk(0, a.w) : make(a.v << n.v, a.x << n.v, a.w);
}
inline Val v_shr(Val a, Val n) {
    if (n.x) return allx(a.w);
    return n.v >= 64 ? mk(0, a.w) : make(a.v >> n.v, a.x >> n.v, a.w);
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
// Unknown condition: keep bits where both branches agree and are known, else X.
inline Val v_mux(Val c, Val a, Val b) {
    int t = truth(c);
    if (t == 1) return a;
    if (t == 0) return b;
    uint64_t diff = (a.v ^ b.v) | a.x | b.x;
    return make(a.v & ~diff, diff, wmax(a, b));
}

// Wire resolution of two drivers, per bit (no strengths):
// Z yields to the other driver; equal known values stay; any conflict or X -> X.
inline Val v_resolve(Val a, Val b) {
    uint64_t az = a.z, bz = b.z & ~a.z, both = ~(a.z | b.z);
    uint64_t x = (az & b.x) | (bz & a.x) | (both & (a.x | b.x | (a.v ^ b.v)));
    uint64_t z = (az & b.z) | (bz & a.z);
    uint64_t v = (az & b.v) | (bz & a.v) | (both & a.v);
    return make(v, x, wmax(a, b), z);
}

// ================= expression tree =================
enum class Op {
    Const, Ref,
    Not, And, Or, Xor, Add, Sub, Mul,
    Eq, Ne, CaseEq, CaseZEq, CaseXEq, Lt, Le, Gt, Ge, Shl, Shr,
    LogNot, LogAnd, LogOr, RedAnd, RedOr, RedXor,
    Slice, Index, Concat, Mux
};
struct Expr;
using E = std::shared_ptr<Expr>;
struct Expr {
    Op op;
    Val k;            // Const
    int sig = -1;     // Ref
    int hi = 0, lo = 0;  // Slice
    std::vector<E> kids;
};

inline E node(Op op, std::vector<E> kids = {}) {
    auto e = std::make_shared<Expr>();
    e->op = op;
    e->kids = std::move(kids);
    return e;
}
inline E konst(Val k) { auto e = node(Op::Const); e->k = k; return e; }
inline E ref(int sig) { auto e = node(Op::Ref); e->sig = sig; return e; }
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
        case Op::Ref: return rd(e->sig);
        case Op::Not: return v_not(k(0));
        case Op::And: return v_and(k(0), k(1));
        case Op::Or: return v_or(k(0), k(1));
        case Op::Xor: return v_xor(k(0), k(1));
        case Op::Add: return v_add(k(0), k(1));
        case Op::Sub: return v_sub(k(0), k(1));
        case Op::Mul: return v_mul(k(0), k(1));
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
