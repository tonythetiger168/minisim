#pragma once
// bv.hpp - bit vectors over the AIG, with don't-care tracking.
//
// Every bit is a pair of literals: v, its value, and x, true when the value
// is an X the design itself produced (an 'x literal, an out-of-range select,
// a division by zero, an undriven net). X is carried pessimistically through
// every operator, the way simulation does, so that at a compare point the
// reference design's X means "the implementation may do anything here", which
// is the freedom synthesis is given (and takes). When no X is anywhere in a
// cone the x literals are all constant false and cost nothing.
#include <cstdint>
#include <vector>

#include "aig.hpp"

namespace equiv {

struct Bit {
    Lit v = kFalse, x = kFalse;
    bool operator==(const Bit&) const = default;
};
inline constexpr Bit kB0{kFalse, kFalse}, kB1{kTrue, kFalse}, kBX{kFalse, kTrue};
inline constexpr Bit kUnset{kNoLit, kNoLit};
inline bool isSet(Bit b) { return b.v != kNoLit; }
inline bool isConst(Bit b) { return b.v <= 1 && b.x == kFalse; }
inline bool isTrue(Bit b) { return b.v == kTrue && b.x == kFalse; }
inline bool isFalse(Bit b) { return b.v == kFalse && b.x == kFalse; }
using Bits = std::vector<Bit>;

struct Bv {
    Aig& g;
    // Pairs of literals that are equal by construction (the same product
    // built two ways). The checker may use them without proving them.
    std::vector<std::pair<Lit, Lit>>* lemmas = nullptr;
    explicit Bv(Aig& g) : g(g) {}

    Bit not_(Bit a) { return {neg(a.v), a.x}; }
    Bit and_(Bit a, Bit b) {
        Lit v = g.and_(a.v, b.v);
        if (a.x == kFalse && b.x == kFalse) return {v, kFalse};
        // X unless one side is a definite 0.
        Lit a0 = g.and_(neg(a.v), neg(a.x)), b0 = g.and_(neg(b.v), neg(b.x));
        Lit x = g.and_(g.or_(a.x, b.x), g.and_(neg(a0), neg(b0)));
        return {g.and_(v, neg(x)), x};
    }
    Bit or_(Bit a, Bit b) { return not_(and_(not_(a), not_(b))); }
    Bit xor_(Bit a, Bit b) {
        if (a.x == kFalse && b.x == kFalse) return {g.xor_(a.v, b.v), kFalse};
        Lit x = g.or_(a.x, b.x);
        return {g.and_(g.xor_(a.v, b.v), neg(x)), x};
    }
    Bit mux(Bit s, Bit t, Bit e) {
        if (s.x == kFalse) {
            if (s.v == kTrue) return t;
            if (s.v == kFalse) return e;
            return {g.mux(s.v, t.v, e.v), g.mux(s.v, t.x, e.x)};
        }
        // An X select gives X unless both arms agree on a definite value.
        Lit differ = g.or_(g.or_(t.x, e.x), g.xor_(t.v, e.v));
        Lit x = g.mux(s.x, differ, g.mux(s.v, t.x, e.x));
        return {g.and_(g.mux(s.v, t.v, e.v), neg(x)), x};
    }

    Bits not_(const Bits& a) {
        Bits r(a.size());
        for (size_t i = 0; i < a.size(); ++i) r[i] = not_(a[i]);
        return r;
    }
    Bits mux(Bit s, const Bits& t, const Bits& e) {
        if (isTrue(s)) return t;
        if (isFalse(s)) return e;
        Bits r(t.size());
        for (size_t i = 0; i < t.size(); ++i) r[i] = mux(s, t[i], e[i]);
        return r;
    }
    Bit redAnd(const Bits& a) {
        Bit r = kB1;
        for (Bit b : a) r = and_(r, b);
        return r;
    }
    Bit redOr(const Bits& a) {
        Bit r = kB0;
        for (Bit b : a) r = or_(r, b);
        return r;
    }
    Bit redXor(const Bits& a) {
        Bit r = kB0;
        for (Bit b : a) r = xor_(r, b);
        return r;
    }
    Bit anyX(const Bits& a) {
        Lit x = kFalse;
        for (Bit b : a) x = g.or_(x, b.x);
        return {kFalse, x};
    }
    // Arithmetic and comparison results are all X when any operand bit is.
    void poison(Bits& r, const Bits& a, const Bits& b) {
        Lit x = g.or_(anyX(a).x, anyX(b).x);
        if (x == kFalse) return;
        for (Bit& bit : r) bit = {g.and_(bit.v, neg(x)), g.or_(bit.x, x)};
    }
    Bit poison(Bit r, const Bits& a, const Bits& b) {
        Bits t{r};
        poison(t, a, b);
        return t[0];
    }

    static Bits constant(uint64_t value, size_t w) {
        Bits r(w, kB0);
        for (size_t i = 0; i < w && i < 64; ++i)
            if (value >> i & 1) r[i] = kB1;
        return r;
    }
    static Bits resize(Bits a, size_t w, bool sign) {
        Bit fill = sign && !a.empty() ? a.back() : kB0;
        a.resize(w, fill);
        return a;
    }
    static bool constValue(const Bits& a, uint64_t& out) {
        out = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!isConst(a[i])) return false;
            if (a[i].v == kTrue) {
                if (i >= 64) return false;
                out |= 1ull << i;
            }
        }
        return true;
    }

    // Value-only arithmetic (callers poison the result).
    Bits addV(const Bits& a, const Bits& b, Lit cin, Lit* cout = nullptr) {
        Bits r(a.size());
        Lit c = cin;
        for (size_t i = 0; i < a.size(); ++i) {
            Lit p = g.xor_(a[i].v, b[i].v);
            r[i] = {g.xor_(p, c), kFalse};
            c = g.or_(g.and_(a[i].v, b[i].v), g.and_(p, c));
        }
        if (cout) *cout = c;
        return r;
    }
    Bits add(const Bits& a, const Bits& b) {
        Bits r = addV(a, b, kFalse);
        poison(r, a, b);
        return r;
    }
    Bits sub(const Bits& a, const Bits& b) {
        Bits r = addV(a, not_(b), kTrue);
        poison(r, a, b);
        return r;
    }
    Bits negate(const Bits& a) { return sub(Bits(a.size(), kB0), a); }
    // Shift-and-add, one row per bit of b.
    Bits mulV(const Bits& a, const Bits& b) {
        size_t w = a.size();
        Bits acc(w, kB0);
        for (size_t i = 0; i < w; ++i) {
            if (b[i].v == kFalse) continue;
            Bits pp(w, kB0);
            for (size_t j = i; j < w; ++j) pp[j] = {g.and_(a[j - i].v, b[i].v), kFalse};
            acc = addV(acc, pp, kFalse);
        }
        return acc;
    }
    Bits mul(const Bits& a, const Bits& b) {
        Bits acc = mulV(a, b);
        // A netlist may add the rows in the other order (one per bit of a).
        // Its internal sums then match nothing in ours, and proving two
        // multipliers equal from their outputs alone is hopeless for SAT. So
        // build that order too and record that the two products are equal.
        uint64_t ca, cb;
        if (lemmas && a.size() >= 6 && !constValue(a, ca) && !constValue(b, cb)) {
            Bits alt = mulV(b, a);
            for (size_t i = 0; i < acc.size(); ++i)
                if (alt[i].v != acc[i].v) lemmas->push_back({acc[i].v, alt[i].v});
        }
        poison(acc, a, b);
        return acc;
    }
    // a < b, unsigned, on values.
    Lit ltV(const Bits& a, const Bits& b) {
        Lit lt = kFalse;
        for (size_t i = 0; i < a.size(); ++i)
            lt = g.mux(g.xor_(a[i].v, b[i].v), b[i].v, lt);
        return lt;
    }
    Bit lt(Bits a, Bits b, bool sign) {
        if (sign && !a.empty()) {
            a.back() = not_(a.back());
            b.back() = not_(b.back());
        }
        return poison(Bit{ltV(a, b), kFalse}, a, b);
    }
    Bit eq(const Bits& a, const Bits& b) {
        Bit r = kB1;
        for (size_t i = 0; i < a.size(); ++i) r = and_(r, not_(xor_(a[i], b[i])));
        return r;
    }
    // Unsigned divide on values (restoring). q and r have a's width.
    void udivV(const Bits& a, const Bits& b, Bits& q, Bits& r) {
        size_t w = a.size();
        q.assign(w, kB0);
        r.assign(w, kB0);
        for (size_t i = w; i-- > 0;) {
            // r = {r, a[i]}, one bit wider so the compare cannot overflow.
            Bits rr(w + 1, kB0);
            rr[0] = {a[i].v, kFalse};
            for (size_t j = 0; j < w; ++j) rr[j + 1] = r[j];
            Bits bb = b;
            for (Bit& bit : bb) bit.x = kFalse;
            bb.push_back(kB0);
            Lit ge = neg(ltV(rr, bb));
            Bits d = addV(rr, not_(bb), kTrue);
            for (size_t j = 0; j < w; ++j) r[j] = {g.mux(ge, d[j].v, rr[j].v), kFalse};
            q[i] = {ge, kFalse};
        }
    }
    Bits divmod(const Bits& a, const Bits& b, bool sign, bool wantMod) {
        size_t w = a.size();
        Bits ua = a, ub = b;
        Lit sa = kFalse, sb = kFalse;
        if (sign && w) {
            sa = a.back().v, sb = b.back().v;
            ua = mux(Bit{sa, kFalse}, addV(not_(a), Bits(w, kB0), kTrue), a);
            ub = mux(Bit{sb, kFalse}, addV(not_(b), Bits(w, kB0), kTrue), b);
        }
        Bits q, r;
        udivV(ua, ub, q, r);
        Bits out = wantMod ? r : q;
        Lit flip = wantMod ? sa : g.xor_(sa, sb);
        if (sign) out = mux(Bit{flip, kFalse}, addV(not_(out), Bits(w, kB0), kTrue), out);
        for (Bit& bit : out) bit.x = kFalse;
        poison(out, a, b);
        Lit zero = kTrue;  // division by zero is X
        for (Bit bit : b) zero = g.and_(zero, neg(bit.v));
        if (zero != kFalse)
            for (Bit& bit : out) bit = {g.and_(bit.v, neg(zero)), g.or_(bit.x, zero)};
        return out;
    }
    // kind: 0 left, 1 logical right, 2 arithmetic right.
    Bits shift(const Bits& a, const Bits& amt, int kind) {
        size_t w = a.size();
        Bits r = a;
        Bit fill = kind == 2 && w ? a.back() : kB0;
        for (size_t j = 0; j < amt.size(); ++j) {
            Bit s{amt[j].v, kFalse};
            if (s.v == kFalse) continue;
            Bits sh(w, fill);
            if (j < 32 && (size_t(1) << j) < w) {
                size_t k = size_t(1) << j;
                for (size_t i = 0; i < w; ++i) {
                    if (kind == 0) { if (i >= k) sh[i] = r[i - k]; }
                    else if (i + k < w) sh[i] = r[i + k];
                }
            }
            r = mux(s, sh, r);
        }
        Lit x = anyX(amt).x;
        if (x != kFalse)
            for (Bit& bit : r) bit = {g.and_(bit.v, neg(x)), g.or_(bit.x, x)};
        return r;
    }
};

}  // namespace equiv
