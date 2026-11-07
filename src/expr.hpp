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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct Val {
    uint64_t v = 0, x = ~0ULL;  // default: all X
    int w = 1;
    uint64_t z = 0;             // which unknown bits are Z (subset of x)
    bool sgn = false;           // expression result is signed; not stored on a net
    bool real = false;          // v holds an IEEE double (6.12); w is 64
    bool str = false;           // v is a string id (6.16); see strPool
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
inline double v_real(const Val& a) {
    if (!a.real) {   // an integer used as a real
        uint64_t v = a.v & ~a.x;
        if (a.sgn && a.w < 64 && a.w > 0 && ((v >> (a.w - 1)) & 1)) v |= ~mask(a.w);
        return a.sgn ? (double)(int64_t)v : (double)v;
    }
    double d;
    std::memcpy(&d, &a.v, 8);
    return d;
}
inline Val mkreal(double d) {
    Val r;
    std::memcpy(&r.v, &d, 8);
    r.x = 0; r.z = 0; r.w = 64; r.real = true;
    return r;
}
// Real to integer (6.12.1): round to nearest, ties away from zero.
inline Val v_rtoi(const Val& a, int w, bool sgn, bool trunc) {
    double d = v_real(a);
    if (std::isnan(d)) return allx(w);
    double r = trunc ? std::trunc(d) : std::round(d);
    uint64_t bits = r < 0 ? (uint64_t)(int64_t)r : (r >= 18446744073709551616.0 ? ~0ULL : (uint64_t)r);
    Val v = make(bits, 0, w);
    v.sgn = sgn;
    return v;
}
enum MathFn { M_LN, M_LOG10, M_EXP, M_SQRT, M_POW, M_FLOOR, M_CEIL, M_SIN, M_COS, M_TAN, M_ASIN, M_ACOS,
              M_ATAN, M_ATAN2, M_HYPOT, M_SINH, M_COSH, M_TANH, M_ASINH, M_ACOSH, M_ATANH };
inline double mathFn(int f, double a, double b) {
    switch (f) {
        case M_LN: return std::log(a);       case M_LOG10: return std::log10(a);
        case M_EXP: return std::exp(a);      case M_SQRT: return std::sqrt(a);
        case M_POW: return std::pow(a, b);   case M_FLOOR: return std::floor(a);
        case M_CEIL: return std::ceil(a);    case M_SIN: return std::sin(a);
        case M_COS: return std::cos(a);      case M_TAN: return std::tan(a);
        case M_ASIN: return std::asin(a);    case M_ACOS: return std::acos(a);
        case M_ATAN: return std::atan(a);    case M_ATAN2: return std::atan2(a, b);
        case M_HYPOT: return std::hypot(a, b); case M_SINH: return std::sinh(a);
        case M_COSH: return std::cosh(a);    case M_TANH: return std::tanh(a);
        case M_ASINH: return std::asinh(a);  case M_ACOSH: return std::acosh(a);
        case M_ATANH: return std::atanh(a);
    }
    return 0;
}
struct StrPool {
    std::vector<std::string> text{""};
    std::unordered_map<std::string, uint64_t> id{{"", 0}};
    uint64_t of(const std::string& s) {
        auto [it, fresh] = id.try_emplace(s, text.size());
        if (fresh) text.push_back(s);
        return it->second;
    }
};
inline StrPool& strPool() { static StrPool p; return p; }
inline Val mkstr(const std::string& s) {
    Val r = make(strPool().of(s), 0, 64);
    r.str = true;
    return r;
}
inline const std::string& v_str(const Val& a) {
    static const std::string empty;
    if (!a.str || a.x || a.v >= strPool().text.size()) return empty;
    return strPool().text[a.v];
}
inline std::string bytesToStr(const Val& a) {   // integral to string: 0 bytes dropped
    std::string s;
    for (int b = (a.w + 7) / 8 - 1; b >= 0; b--) {
        char c = char(((a.v & ~a.x) >> (8 * b)) & 0xff);
        if (c) s += c;
    }
    return s;
}
enum StrFn { S_LEN, S_GETC, S_TOUPPER, S_TOLOWER, S_COMPARE, S_ICOMPARE, S_SUBSTR, S_ATOI, S_ATOHEX,
             S_ATOOCT, S_ATOBIN, S_ATOREAL, S_PUTC, S_ITOA, S_HEXTOA, S_OCTTOA, S_BINTOA, S_REALTOA };
// The hook $sformatf uses: the kernel's format engine (set by the kernel).
using Reader = std::function<Val(int)>;
struct Expr;
using E = std::shared_ptr<Expr>;
inline std::string (*g_sformat)(const std::string&, const std::vector<E>&, const Reader&) = nullptr;
// The contents of every dynamic container, by handle signal. A change bumps
// the handle's value, so a process waiting on the container wakes.
inline int64_t v_to_i64(Val v);
struct DynKey {
    int64_t i = 0;     // integral key (unsigned keys have the top bit flipped, so order holds)
    std::string s;     // string key
    bool operator<(const DynKey& o) const { return s != o.s ? s < o.s : i < o.i; }
};
struct DynObj {
    int kind = 0;               // 0 dynamic array, 1 queue, 2 associative array
    std::vector<Val> vec;
    std::map<DynKey, Val> map;
    Val dflt;                   // what a missing element reads as
    int64_t bound = -1;         // queue [$:N]: at most N + 1 elements
    bool strKey = false;        // associative: string index
    int keyW = 64;
    bool keySgn = true;
    DynKey key(const Val& k) const {
        DynKey d;
        if (strKey) {   // a signal holds a string as its pool id, without the flag
            Val t = k;
            t.str = true;
            d.s = v_str(t);
            return d;
        }
        uint64_t u = k.v & ~k.x & mask(keyW);
        if (keySgn && keyW < 64 && ((u >> (keyW - 1)) & 1)) u |= ~mask(keyW);
        d.i = keySgn ? (int64_t)u : (int64_t)(u ^ (1ULL << 63));
        return d;
    }
    Val keyVal(const DynKey& d) const {
        if (strKey) return mkstr(d.s);
        uint64_t u = keySgn ? (uint64_t)d.i : ((uint64_t)d.i ^ (1ULL << 63));
        Val v = make(u, 0, keyW);
        v.sgn = keySgn;
        return v;
    }
    size_t size() const { return kind == 2 ? map.size() : vec.size(); }
    Val get(const Val& k) const {
        if (kind == 2) {
            if (k.x && !strKey) return dflt;
            auto it = map.find(key(k));
            return it == map.end() ? dflt : it->second;
        }
        if (k.x) return dflt;
        int64_t i = v_to_i64(k);
        if (i < 0 || i >= (int64_t)vec.size()) return dflt;
        return vec[(size_t)i];
    }
};
inline std::unordered_map<int, DynObj>& dynStore() { static std::unordered_map<int, DynObj> m; return m; }
inline Val v_wild_eq(Val a, Val b) {
    align_signed(a, b);
    uint64_t care = ~b.x;
    if ((a.v ^ b.v) & ~a.x & care) return mk(0);
    return (a.x & care) ? allx(1) : mk(1);
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
// w bits of a starting at bit lo; a bit outside a reads x (11.5.1).
inline Val v_dynslice(Val a, int64_t lo, int w) {
    uint64_t v = 0, x = 0, z = 0;
    for (int b = 0; b < w; b++) {
        int64_t s = lo + b;
        if (s < 0 || s >= a.w) { x |= 1ULL << b; continue; }
        v |= ((a.v >> s) & 1) << b;
        x |= ((a.x >> s) & 1) << b;
        z |= ((a.z >> s) & 1) << b;
    }
    return make(v, x, w, z);
}
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
    Slice, Index, Concat, Mux,
    Pow, Time,
    WildEq,    // a ==? b: x / z bits of b match anything (11.4.6)
    // real (6.12). Values are doubles in v, with Val::real set.
    FAdd, FSub, FMul, FDiv, FPow, FNeg,
    FCmp,      // hi: 0 <, 1 <=, 2 >, 3 >=, 4 ==, 5 !=; 1-bit result
    IToR,      // integer (signed if its Val is) to real; x and z bits count as 0
    RToI,      // real to an integer of hi bits, rounded (lo = 1: truncated, $rtoi)
    AsReal,    // the 64 bits of kid as a real ($bitstoreal, a real variable)
    FMath,     // hi: a math function id (20.8), one or two kids
    RealTime,  // $realtime: k.v ticks per unit
    // string (6.16). A string value is an id into strPool(); equal text has
    // one id, so == and a change on a string signal compare ids.
    AsStr,     // kid's bits are a string id
    IntToStr,  // the bytes of kid, leading zero bytes dropped (6.16)
    StrToInt,  // the text as bytes, the last char in the low byte, hi bits
    StrCat,    // {a, b, ...}
    StrRep,    // {hi{a}}
    StrCmp,    // hi as FCmp: <, <=, >, >=, ==, !=
    StrMethod, // hi: StrFn; kid 0 is the string
    SFormat,   // $sformatf: fmt, then kids as the arguments
    // Dynamic arrays, queues, associative arrays (7.5, 7.10, 7.8). sig is the
    // container's handle signal; its contents live in dynStore().
    DynGet,    // element kids[0] (index or key); a missing one reads the default
    DynSize,   // size() / num()
    DynExists, // exists(kids[0])
    DynReduce, // hi: 0 sum, 1 product, 2 and, 3 or, 4 xor (7.12.3), at k.w bits
    ArrRead,   // element of an unpacked array; kids are one index per dimension
    DynSlice   // a[i +: w] / a[i -: w] with i known at run time: hi = w, lo = 1 for -:
};

// A fixed-size unpacked array (IEEE 1800-2023 7.4) is `count` consecutive
// signals starting at `base`, one per element, in row-major order. Each
// dimension keeps its declared [left:right]; offset 0 is the left bound.
struct ArrShape {
    int base = -1;
    int count = 0;
    int elemW = 1;
    bool sgn = false;
    bool twoState = false;   // bit / int elements: an invalid read is 0, not X (7.4.6)
    std::vector<std::pair<int64_t, int64_t>> dims;   // [left, right]
    // Flat element offset, or -1 when an index is outside its dimension.
    int64_t offset(const std::vector<int64_t>& idx) const {
        int64_t off = 0;
        for (size_t d = 0; d < dims.size(); d++) {
            auto [l, r] = dims[d];
            int64_t n = (l > r ? l - r : r - l) + 1;
            int64_t i = idx[d];
            int64_t o = l <= r ? i - l : l - i;
            if (o < 0 || o >= n) return -1;
            off = off * n + o;
        }
        return off;
    }
    int64_t size(size_t d) const { auto [l, r] = dims[d]; return (l > r ? l - r : r - l) + 1; }
};
struct Expr {
    std::string fmt;  // SFormat
    Op op;
    Val k;            // Const
    int sig = -1;     // Ref
    int hi = 0, lo = 0;  // Slice, or Resize dest width / sign-fill
    bool sgn = false;    // Ref result, or Resize result
    std::vector<E> kids;
    std::shared_ptr<const ArrShape> arr;   // ArrRead
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

// Current simulation time, for $time / $stime / $realtime (IEEE 1800 20.3).
// The kernel sets it when time advances.
inline uint64_t g_simTime = 0;

// a ** b (IEEE 1800 11.4.5, table 11-4). The result has a's width and sign.
// Any X/Z operand gives X. A negative exponent gives 0 except for a base of
// 1 (1), -1 (+-1 by parity), and 0 (X).
inline Val v_pow(Val a, Val b) {
    int w = a.w;
    if (a.x || b.x) return make(0, ~0ULL, w, 0, a.sgn);
    uint64_t m = mask(w);
    uint64_t base = a.v & m;
    bool bneg = b.sgn && b.w > 0 && ((b.v >> (b.w - 1)) & 1);
    if (bneg) {
        bool aneg1 = a.sgn && base == m;  // -1
        if (base == 0) return make(0, ~0ULL, w, 0, a.sgn);
        if (base == 1) return make(1, 0, w, 0, a.sgn);
        if (aneg1) return make((b.v & 1) ? m : 1, 0, w, 0, a.sgn);
        return make(0, 0, w, 0, a.sgn);
    }
    uint64_t e = b.v & mask(b.w), r = 1;
    while (e) {
        if (e & 1) r *= base;
        base *= base;
        e >>= 1;
    }
    return make(r & m, 0, w, 0, a.sgn);
}

// Integer value of a known Val, sign-extended when it is signed.
inline int64_t v_to_i64(Val v) {
    uint64_t m = mask(v.w);
    uint64_t u = v.v & m;
    if (v.sgn && v.w < 64 && ((u >> (v.w - 1)) & 1)) u |= ~m;
    return (int64_t)u;
}

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
        case Op::WildEq: return v_wild_eq(k(0), k(1));
        case Op::FAdd: return mkreal(v_real(k(0)) + v_real(k(1)));
        case Op::FSub: return mkreal(v_real(k(0)) - v_real(k(1)));
        case Op::FMul: return mkreal(v_real(k(0)) * v_real(k(1)));
        case Op::FDiv: return mkreal(v_real(k(0)) / v_real(k(1)));
        case Op::FPow: return mkreal(std::pow(v_real(k(0)), v_real(k(1))));
        case Op::FNeg: return mkreal(-v_real(k(0)));
        case Op::FCmp: {
            double a = v_real(k(0)), b = v_real(k(1));
            bool r = false;
            switch (e->hi) {
                case 0: r = a < b; break;  case 1: r = a <= b; break;
                case 2: r = a > b; break;  case 3: r = a >= b; break;
                case 4: r = a == b; break; default: r = a != b; break;
            }
            return mk(r);
        }
        case Op::IToR: return mkreal(v_real(k(0)));
        case Op::RToI: return v_rtoi(k(0), e->hi, e->sgn, e->lo);
        case Op::AsReal: { Val v = k(0); Val r = mkreal(0); r.v = v.v & ~v.x; return r; }
        case Op::FMath: return mkreal(mathFn(e->hi, v_real(k(0)), e->kids.size() > 1 ? v_real(k(1)) : 0));
        case Op::RealTime: return mkreal((double)g_simTime / (double)(e->k.v ? e->k.v : 1));
        case Op::AsStr: { Val v = k(0); Val r = make(v.v, v.x, 64); r.str = true; return r; }
        case Op::IntToStr: { Val v = k(0); return v.str ? v : mkstr(bytesToStr(v)); }
        case Op::StrToInt: {
            const std::string& s = v_str(k(0));
            uint64_t v = 0;
            for (size_t i = 0; i < s.size(); i++) v = (v << 8) | (unsigned char)s[i];
            return make(v, 0, e->hi);
        }
        case Op::StrCat: {
            std::string s;
            for (size_t i = 0; i < e->kids.size(); i++) { Val v = k((int)i); s += v.str ? v_str(v) : bytesToStr(v); }
            return mkstr(s);
        }
        case Op::StrRep: {
            std::string one = v_str(k(0)), s;
            for (int i = 0; i < e->hi; i++) s += one;
            return mkstr(s);
        }
        case Op::StrCmp: {
            int c = v_str(k(0)).compare(v_str(k(1)));
            bool r = false;
            switch (e->hi) {
                case 0: r = c < 0; break;  case 1: r = c <= 0; break;
                case 2: r = c > 0; break;  case 3: r = c >= 0; break;
                case 4: r = c == 0; break; default: r = c != 0; break;
            }
            return mk(r);
        }
        case Op::StrMethod: {
            std::string s = v_str(k(0));
            auto ival = [&](int i) { return v_to_i64(k(i)); };
            auto num = [](int64_t v, int w) { Val r = make((uint64_t)v, 0, w); r.sgn = true; return r; };
            auto parse = [&](int base) {
                int64_t v = 0;
                size_t i = 0;
                bool neg = false;
                if (base == 10 && i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
                for (; i < s.size(); i++) {
                    char c = (char)std::tolower((unsigned char)s[i]);
                    if (c == '_') continue;
                    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
                    if (d >= base) break;
                    v = v * base + d;
                }
                return num(neg ? -v : v, 32);
            };
            auto radix = [&](int base) {
                Val v = k(1);
                uint64_t u = v.v & ~v.x;
                if (base == 10) return std::to_string(v.sgn ? v_to_i64(v) : (int64_t)u);
                if (!u) return std::string("0");
                std::string r;
                while (u) { r += "0123456789abcdef"[u % base]; u /= base; }
                return std::string(r.rbegin(), r.rend());
            };
            switch (e->hi) {
                case S_LEN: return num((int64_t)s.size(), 32);
                case S_GETC: { int64_t i = ival(1); return make(i >= 0 && i < (int64_t)s.size() ? (unsigned char)s[i] : 0, 0, 8); }
                case S_TOUPPER: for (auto& c : s) c = (char)std::toupper((unsigned char)c); return mkstr(s);
                case S_TOLOWER: for (auto& c : s) c = (char)std::tolower((unsigned char)c); return mkstr(s);
                case S_COMPARE: { int c = s.compare(v_str(k(1))); return num(c < 0 ? -1 : c > 0, 32); }
                case S_ICOMPARE: {
                    std::string a = s, b = v_str(k(1));
                    for (auto& c : a) c = (char)std::tolower((unsigned char)c);
                    for (auto& c : b) c = (char)std::tolower((unsigned char)c);
                    int c = a.compare(b);
                    return num(c < 0 ? -1 : c > 0, 32);
                }
                case S_SUBSTR: {   // s.substr(i, j): chars i..j, or "" when out of range
                    int64_t i = ival(1), j = ival(2);
                    if (i < 0 || j >= (int64_t)s.size() || i > j) return mkstr("");
                    return mkstr(s.substr((size_t)i, (size_t)(j - i + 1)));
                }
                case S_ATOI: return parse(10);
                case S_ATOHEX: return parse(16);
                case S_ATOOCT: return parse(8);
                case S_ATOBIN: return parse(2);
                case S_ATOREAL: return mkreal(std::strtod(s.c_str(), nullptr));
                case S_PUTC: {   // a new string; the statement writes it back
                    int64_t i = ival(1);
                    Val c = k(2);
                    if (i >= 0 && i < (int64_t)s.size() && (c.v & 0xff)) s[i] = (char)(c.v & 0xff);
                    return mkstr(s);
                }
                case S_ITOA: return mkstr(radix(10));
                case S_HEXTOA: return mkstr(radix(16));
                case S_OCTTOA: return mkstr(radix(8));
                case S_BINTOA: return mkstr(radix(2));
                case S_REALTOA: { char b[64]; snprintf(b, sizeof b, "%g", v_real(k(1))); return mkstr(b); }
            }
            return mkstr(s);
        }
        case Op::DynGet: return dynStore()[e->sig].get(k(0));
        case Op::DynSize: { Val r = make(dynStore()[e->sig].size(), 0, 32); r.sgn = true; return r; }
        case Op::DynExists: {
            auto& d = dynStore()[e->sig];
            Val key = k(0);
            if (d.kind != 2) { int64_t i = v_to_i64(key); return mk(!key.x && i >= 0 && i < (int64_t)d.vec.size()); }
            return mk(d.map.count(d.key(key)) > 0);
        }
        case Op::DynReduce: {
            auto& d = dynStore()[e->sig];
            int w = e->k.w;
            Val acc = make(e->hi == 1 ? 1 : e->hi == 2 ? ~0ULL : 0, 0, w);
            auto one = [&](const Val& x) {
                Val v = v_resize(x, w, x.sgn);
                switch (e->hi) {
                    case 0: acc = v_add(acc, v); break;
                    case 1: acc = v_mul(acc, v); break;
                    case 2: acc = v_and(acc, v); break;
                    case 3: acc = v_or(acc, v); break;
                    default: acc = v_xor(acc, v); break;
                }
            };
            if (d.kind == 2) for (auto& [key, v] : d.map) one(v);
            else for (auto& v : d.vec) one(v);
            acc.sgn = e->k.sgn;
            return acc;
        }
        case Op::SFormat: return mkstr(g_sformat ? g_sformat(e->fmt, std::vector<E>(e->kids.begin(), e->kids.end()), rd) : "");
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
            r.real = false;   // the bits of a real are an integer now ($realtobits)
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
        case Op::Pow: return v_pow(k(0), k(1));
        case Op::Time: {   // k.v ticks per time unit of the calling module
            uint64_t d = e->k.v ? e->k.v : 1;
            return make((g_simTime + d / 2) / d, 0, 64, 0);
        }
        case Op::DynSlice: {
            Val a = k(0), i = k(1);
            if (i.x) return allx(e->hi);
            int64_t s = v_to_i64(i);
            return v_dynslice(a, e->lo ? s - e->hi + 1 : s, e->hi);
        }
        case Op::ArrRead: {
            // An index with x/z bits, or one outside the bounds, reads the
            // element type's default: X for 4-state, 0 for 2-state (7.4.6).
            const ArrShape& a = *e->arr;
            Val bad = a.twoState ? make(0, 0, a.elemW, 0, a.sgn) : make(0, ~0ULL, a.elemW, 0, a.sgn);
            std::vector<int64_t> idx;
            for (size_t i = 0; i < e->kids.size(); i++) {
                Val v = k((int)i);
                if (v.x) return bad;
                idx.push_back(v_to_i64(v));
            }
            int64_t off = a.offset(idx);
            if (off < 0) return bad;
            Val r = rd(a.base + (int)off);
            r.sgn = a.sgn;
            return r;
        }
    }
    return allx(1);
}
