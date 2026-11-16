// exprsimp.hpp - algebraic simplification of lowered expressions (msim -compile).
//
// A peephole rewriter over Expr trees. Every rule is exact under the 4-state
// semantics of the v_* functions in expr.hpp, X and Z included: the result
// of a rewritten expression is the same Val (value, X and Z bits, width and
// signedness) as the original for every input. Rules from 2-valued Boolean
// algebra that do not hold with X (a & ~a = 0, a ^ a = 0, adjacency,
// consensus, a + 0 = a) are left out on purpose: with a = x each of them
// turns an X into a known value (see the Kleene table in
// reports/design-reduction of the project, and 11.4.3 / 11.4.8 / 11.4.11).
//
// Rules that would drop a Z need the operand to be Z-free: an operator
// result (bitwise, arithmetic, compare, reduction, logical) never holds Z,
// a signal or a slice of one may. a & '1 = a is such a rule (z & 1 is x).
//
// Width and signedness come from the expression itself (constants, slices,
// resizes, operators) and from sigInfo for references; where they cannot
// be told, a rule does not fire.
#pragma once
#include "expr.hpp"
#include <functional>

namespace exprsimp {

struct Info { int w = -1; bool sgn = false; };

struct Stats { size_t rewrites = 0, folds = 0; };

class Simplifier {
public:
    // sigInfo(s): width of signal s (or -1 when unknown: real, string, class).
    explicit Simplifier(std::function<int(int)> sigWidth) : sigWidth_(std::move(sigWidth)) {}
    Stats stats;

    // The simplified expression; the same pointer when nothing changed
    // (expressions may be shared between instructions, so nodes are copied,
    // never edited in place).
    E run(const E& e) {
        if (!e) return e;
        if (e->op == Op::Const || e->op == Op::Ref || e->at) return e;
        if (!pureOp(e->op)) {
            // Not rewritten itself, but its operands may be.
            std::vector<E> kids;
            bool changed = false;
            kids.reserve(e->kids.size());
            for (auto& k : e->kids) { E f = run(k); changed |= f != k; kids.push_back(std::move(f)); }
            if (!changed) return e;
            auto n = std::make_shared<Expr>(*e);
            n->kids = std::move(kids);
            return n;
        }
        std::vector<E> kids;
        bool changed = false;
        kids.reserve(e->kids.size());
        for (auto& k : e->kids) { E f = run(k); changed |= f != k; kids.push_back(std::move(f)); }
        E cur = e;
        if (changed) {
            auto n = std::make_shared<Expr>(*e);
            n->kids = std::move(kids);
            cur = n;
        }
        // Rules may expose more rules (Resize removed, then a & '1): repeat a few times.
        for (int i = 0; i < 4; i++) {
            E r = rule(cur);
            if (r == cur) break;
            stats.rewrites++;
            cur = r;
            if (!cur || cur->op == Op::Const || cur->op == Op::Ref || cur->at || !pureOp(cur->op)) break;
        }
        return cur;
    }

    // Operators whose value depends only on their operands' values.
    static bool pureOp(Op o) {
        switch (o) {
            case Op::Not: case Op::And: case Op::Or: case Op::Xor: case Op::Add: case Op::Sub: case Op::Mul:
            case Op::Eq: case Op::Ne: case Op::CaseEq: case Op::CaseZEq: case Op::CaseXEq: case Op::WildEq:
            case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Shl: case Op::Shr: case Op::Asr: case Op::Resize:
            case Op::LogNot: case Op::LogAnd: case Op::LogOr: case Op::RedAnd: case Op::RedOr: case Op::RedXor:
            case Op::Slice: case Op::Concat: case Op::Mux:
                return true;
            default: return false;
        }
    }

    // Width and signedness, or w = -1 when not known here.
    Info info(const E& e) const {
        Info r;
        if (!e || e->at) return r;
        switch (e->op) {
            case Op::Const:
                if (e->k.real || e->k.str) return r;
                r.w = e->k.w; r.sgn = e->k.sgn; return r;
            case Op::Ref: r.w = sigWidth_(e->sig); r.sgn = e->sgn; return r;
            case Op::Slice: if (e->hi < e->lo) return r; r.w = e->hi - e->lo + 1; return r;
            case Op::Resize: r.w = e->hi; r.sgn = e->sgn; return r;
            case Op::Concat: {
                int w = 0;
                for (auto& k : e->kids) { Info i = info(k); if (i.w < 0) return Info{}; w += i.w; }
                r.w = w; return r;
            }
            case Op::Not: return info(e->kids[0]);
            case Op::And: case Op::Or: case Op::Xor: {
                Info a = info(e->kids[0]), b = info(e->kids[1]);
                if (a.w < 0 || b.w < 0) return r;
                r.w = std::max(a.w, b.w); r.sgn = a.sgn && b.sgn; return r;
            }
            case Op::Mux: {
                Info a = info(e->kids[1]), b = info(e->kids[2]);
                if (a.w < 0 || b.w < 0) return r;
                r.w = std::max(a.w, b.w); r.sgn = a.sgn && b.sgn; return r;
            }
            case Op::Shl: case Op::Shr: case Op::Asr: return info(e->kids[0]);
            case Op::Eq: case Op::Ne: case Op::CaseEq: case Op::CaseZEq: case Op::CaseXEq: case Op::WildEq:
            case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            case Op::LogNot: case Op::LogAnd: case Op::LogOr: case Op::RedAnd: case Op::RedOr: case Op::RedXor:
                r.w = 1; return r;
            default: return r;
        }
    }

    // Never holds a Z bit.
    static bool noZ(const E& e) {
        if (!e) return false;
        switch (e->op) {
            case Op::Const: {
                if (e->k.z) return false;
                for (auto& h : e->k.hi) if (h.z) return false;
                return true;
            }
            case Op::Not: case Op::And: case Op::Or: case Op::Xor: case Op::Add: case Op::Sub: case Op::Mul:
            case Op::Eq: case Op::Ne: case Op::CaseEq: case Op::CaseZEq: case Op::CaseXEq: case Op::WildEq:
            case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            case Op::LogNot: case Op::LogAnd: case Op::LogOr: case Op::RedAnd: case Op::RedOr: case Op::RedXor:
                return !e->at;
            case Op::Mux: return !e->at && noZ(e->kids[1]) && noZ(e->kids[2]);
            case Op::Resize: case Op::Slice: case Op::Shl: case Op::Shr: case Op::Asr:
                return !e->at && noZ(e->kids[0]);
            case Op::Concat:
                if (e->at) return false;
                for (auto& k : e->kids) if (!noZ(k)) return false;
                return true;
            default: return false;
        }
    }

    // Same value for the same inputs: same tree of pure operators over the
    // same signals and constants.
    static bool same(const E& a, const E& b) {
        if (a == b) return true;
        if (!a || !b || a->op != b->op || a->at || b->at) return false;
        if (a->op == Op::Const) return a->k == b->k && a->k.w == b->k.w && a->k.sgn == b->k.sgn && !a->k.real && !b->k.real && !a->k.str && !b->k.str;
        if (a->op == Op::Ref) return a->sig == b->sig && a->sgn == b->sgn;
        if (!pureOp(a->op) || a->hi != b->hi || a->lo != b->lo || a->sgn != b->sgn || a->kids.size() != b->kids.size()) return false;
        for (size_t i = 0; i < a->kids.size(); i++)
            if (!same(a->kids[i], b->kids[i])) return false;
        return true;
    }

private:
    std::function<int(int)> sigWidth_;

    static bool isConst(const E& e) { return e && e->op == Op::Const && !e->k.real && !e->k.str; }
    // Every bit of the constant known and equal to bit (0 or 1), over w bits.
    static bool allBits(const E& e, int bit, int w) {
        if (!isConst(e) || e->k.w != w) return false;
        const Val& k = e->k;
        for (int i = 0; i < limbCount(w); i++) {
            uint64_t m = limbMask(w, i);
            if (limbX(k, i) & m) return false;
            uint64_t v = limbV(k, i) & m;
            if (bit ? v != m : v != 0) return false;
        }
        return true;
    }
    static E constOf(int bit, int w, bool sgn) {
        Val k = make(0, 0, w, 0, sgn);
        if (bit) {
            for (int i = 0; i < limbCount(w); i++) setLimb(k, i, limbMask(w, i), 0, 0);
            k = finish(k);
            k.sgn = sgn;
        }
        return konst(k);
    }
    // A subtree that may be dropped: no side effects, nothing read through a handle.
    static bool droppable(const E& e) {
        if (!e) return true;
        if (e->at) return false;
        if (e->op == Op::Const || e->op == Op::Ref) return true;
        if (!pureOp(e->op)) return false;
        for (auto& k : e->kids) if (!droppable(k)) return false;
        return true;
    }

    E rule(const E& e) {
        const auto& K = e->kids;
        switch (e->op) {
            case Op::Resize: {
                // Same width, same signedness: the value as it is (v_resize returns it unchanged).
                Info a = info(K[0]);
                if (a.w >= 0 && a.w == e->hi && a.sgn == e->sgn) return K[0];
                // Resize of a resize to a width no wider than the inner one: the outer alone.
                if (K[0] && K[0]->op == Op::Resize && !K[0]->at) {
                    Info in = info(K[0]->kids[0]);
                    if (in.w >= 0 && e->hi <= K[0]->hi && e->hi <= in.w) {
                        auto n = std::make_shared<Expr>(*e);
                        n->kids = {K[0]->kids[0]};
                        return n;
                    }
                }
                return e;
            }
            case Op::Slice: {
                Info a = info(K[0]);
                if (a.w < 0 || e->lo < 0 || e->hi >= a.w) return e;
                // The whole of an unsigned value.
                if (e->lo == 0 && e->hi == a.w - 1 && !a.sgn) return K[0];
                // A slice of a slice.
                if (K[0]->op == Op::Slice && !K[0]->at && K[0]->lo >= 0) {
                    auto n = std::make_shared<Expr>(*e);
                    n->kids = {K[0]->kids[0]};
                    n->hi = K[0]->lo + e->hi;
                    n->lo = K[0]->lo + e->lo;
                    return n;
                }
                // A slice that falls inside one part of a concatenation.
                if (K[0]->op == Op::Concat && !K[0]->at) {
                    int top = a.w;
                    for (auto& p : K[0]->kids) {
                        Info pi = info(p);
                        if (pi.w < 0) return e;
                        int plo = top - pi.w;
                        top = plo;
                        if (e->lo >= plo && e->hi < plo + pi.w) {
                            auto n = std::make_shared<Expr>(*e);
                            n->kids = {p};
                            n->hi = e->hi - plo;
                            n->lo = e->lo - plo;
                            return n;
                        }
                    }
                }
                return e;
            }
            case Op::Concat: {
                if (K.size() == 1) {
                    Info a = info(K[0]);
                    if (a.w >= 0 && !a.sgn) return K[0];
                    return e;
                }
                // Neighbouring slices of the same value: one slice ({r[7:4], r[3:0]} -> r[7:0]).
                bool merged = false;
                std::vector<E> parts;
                parts.reserve(K.size());
                for (auto& p : K) {
                    if (!parts.empty()) {
                        E& q = parts.back();
                        if (q && p && q->op == Op::Slice && p->op == Op::Slice && !q->at && !p->at && q->lo == p->hi + 1 &&
                            p->lo >= 0 && same(q->kids[0], p->kids[0])) {
                            Info b = info(q->kids[0]);
                            if (b.w >= 0 && q->hi < b.w) {
                                auto n = std::make_shared<Expr>(*q);
                                n->lo = p->lo;
                                q = n;
                                merged = true;
                                continue;
                            }
                        }
                    }
                    parts.push_back(p);
                }
                if (!merged) return e;
                if (parts.size() == 1) return parts[0];
                auto n = std::make_shared<Expr>(*e);
                n->kids = std::move(parts);
                return n;
            }
            case Op::Not: {
                // ~~a = a, unless a may hold Z (~z is x).
                if (K[0] && K[0]->op == Op::Not && !K[0]->at && noZ(K[0]->kids[0])) return K[0]->kids[0];
                return e;
            }
            case Op::And: case Op::Or: case Op::Xor: {
                Info a = info(K[0]), b = info(K[1]), r = info(e);
                if (a.w < 0 || b.w < 0 || a.w != b.w) return e;
                const int w = a.w;
                for (int side = 0; side < 2; side++) {
                    const E& c = K[side];
                    const E& x = K[1 - side];
                    const Info& xi = side ? a : b;
                    if (!isConst(c)) continue;
                    // Annihilators: a & 0 = 0, a | 1 = 1, for X and Z too.
                    if (e->op == Op::And && allBits(c, 0, w) && droppable(x)) return constOf(0, w, r.sgn);
                    if (e->op == Op::Or && allBits(c, 1, w) && droppable(x)) return constOf(1, w, r.sgn);
                    // Identities: a & 1 = a, a | 0 = a, a ^ 0 = a; z & 1 is x, so a must be Z-free.
                    const bool ident = (e->op == Op::And && allBits(c, 1, w)) || (e->op != Op::And && allBits(c, 0, w));
                    if (ident && xi.sgn == r.sgn && noZ(x)) return x;
                    // a ^ 1 = ~a (z ^ 1 and ~z are both x).
                    if (e->op == Op::Xor && allBits(c, 1, w) && xi.sgn == r.sgn) return node(Op::Not, {x});
                }
                // Idempotence: a & a = a | a = a (z & z is x).
                if (e->op != Op::Xor && same(K[0], K[1]) && noZ(K[0]) && a.sgn == r.sgn) return K[0];
                // Absorption: a | (a & b) = a, a & (a | b) = a; holds with X (Kleene logic).
                for (int side = 0; side < 2; side++) {
                    const E& x = K[side];
                    const E& y = K[1 - side];
                    const Op inner = e->op == Op::Or ? Op::And : e->op == Op::And ? Op::Or : Op::Xor;
                    if (inner == Op::Xor || !y || y->op != inner || y->at || !noZ(x)) continue;
                    Info xi = side ? b : a;
                    if (xi.sgn != r.sgn || info(y).w != w) continue;
                    for (int j = 0; j < 2; j++)
                        if (same(y->kids[j], x) && info(y->kids[1 - j]).w == w && droppable(y->kids[1 - j])) return x;
                }
                return e;
            }
            case Op::Shl: case Op::Shr: case Op::Asr: {
                // A shift by a known 0: the value unchanged, Z bits too.
                if (isConst(K[1]) && K[1]->k.w <= 64 && K[1]->k.hi.empty() && !K[1]->k.x && K[1]->k.v == 0 && info(K[0]).w >= 0)
                    return K[0];
                return e;
            }
            case Op::Mux: {
                Info a = info(K[1]), b = info(K[2]), c = info(K[0]);
                // c ? a : a = a: an unknown c keeps the bits where both arms agree, so all of them,
                // except Z (z merged with z is x).
                if (same(K[1], K[2]) && noZ(K[1]) && droppable(K[0])) return K[1];
                // A 1-bit condition picking 1 or 0.
                if (c.w == 1 && !c.sgn && a.w == 1 && b.w == 1 && !a.sgn && !b.sgn) {
                    if (allBits(K[1], 1, 1) && allBits(K[2], 0, 1) && noZ(K[0])) return K[0];
                    if (allBits(K[1], 0, 1) && allBits(K[2], 1, 1)) return node(Op::Not, {K[0]});
                }
                return e;
            }
            case Op::LogAnd: case Op::LogOr: {
                // A known operand: 1 && a and 0 || a are a's truth value (|a gives the same 1, 0 or x);
                // 0 && a is 0 and 1 || a is 1.
                const int dec = e->op == Op::LogAnd ? 0 : 1;
                for (int side = 0; side < 2; side++) {
                    const E& c = K[side];
                    const E& x = K[1 - side];
                    if (!isConst(c)) continue;
                    int t = truth(c->k);
                    if (t == 2) continue;
                    if (t == dec && (side == 0 || droppable(x))) return constOf(dec, 1, false);
                    if (t != dec) return node(Op::RedOr, {x});
                }
                return e;
            }
            case Op::LogNot: {
                // !!a is |a.
                if (K[0] && K[0]->op == Op::LogNot && !K[0]->at) return node(Op::RedOr, {K[0]->kids[0]});
                return e;
            }
            case Op::RedOr: case Op::RedAnd: case Op::RedXor: {
                // The reduction of one bit is that bit, when it cannot be Z.
                Info a = info(K[0]);
                if (a.w == 1 && !a.sgn && noZ(K[0])) return K[0];
                return e;
            }
            default: return e;
        }
    }
};

}   // namespace exprsimp
