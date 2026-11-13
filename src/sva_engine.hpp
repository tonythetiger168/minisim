// sva_engine.hpp - the concurrent assertion engine (IEEE 1800-2023 16.5 to
// 16.18). No slang and no kernel here: lowering (lower_assert.inc) builds the
// terms, the kernel (exec.inc) feeds clock ticks.
//
// Sequences and properties are terms, hash-consed so equal terms are one
// node. An evaluation attempt is a term; at each clock tick the attempt
// becomes its derivative with respect to the values of the boolean atoms at
// that tick (Brzozowski derivatives, extended to SVA properties). A
// property term that reaches TRUE has passed, FALSE has failed. Attempts
// whose terms are equal merge and keep a count, so `a |-> ##[1:$] b` with a
// always true does not pile up attempts.
//
// Sequence semantics: a sequence denotes a set of finite words of clock
// ticks (each tick a valuation of the atoms). CAT is word concatenation
// (r ##1 s), FUSE overlaps the last tick of the left with the first of the
// right (r ##0 s), SAND is intersection (intersect), SOR union. The other
// SVA operators are rewritten into these when the term is built (16.9):
//   r ##[m:n] s          = r . 1[*m-1:n-1] . s   (m = 0 adds r : s)
//   b[->m:n]             = (!b[*0:$] . b)[*m:n]
//   b[=m:n]              = b[->m:n] . !b[*0:$]
//   r and s              = (r . 1[*0:$]) & s  |  r & (s . 1[*0:$])
//   e throughout s       = e[*0:$] & s
//   r within s           = (1[*0:$] . r . 1[*0:$]) & s
// A term is nullable when its set has the empty word: a match ends at the
// last tick consumed.
//
// Local variables (16.10): an environment is a vector of their values,
// interned by the kernel as an id. A term is read in a context environment;
// ENV(e, t) reads t in environment e instead. MATCH(r, act) runs the match
// items `act` at the end of each match of r, and INIT(act, t) runs the
// initializers of an instance's local variables when t starts. A sequence
// that ends hands its exit environment on to what follows (`exits`), so
// r ##1 s reads s in the environment r ended in. Terms without local
// variables never build ENV nodes, so they are stepped and merged as before.
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "prog.hpp"

namespace sva {

enum K : uint8_t {
    // sequences
    EMPTY,   // the empty word
    NUL,     // no word
    BOOL,    // one tick where atom is true (false when neg)
    ANY,     // one tick, any values (1'b1)
    CAT,     // kids[0] . kids[1]
    FUSE,    // kids[0] : kids[1]
    SOR,     // union of kids (sorted, unique)
    SAND,    // intersection of kids[0], kids[1]
    REP,     // kids[0] repeated lo to hi times (hi < 0: unbounded)
    FM,      // first_match(kids[0])
    // properties
    PTRUE, PFALSE,
    DONE,    // cover sequence: no more matches, neither pass nor fail
    PSEQ,    // the sequence property kids[0]; strong when flag
    PNOT,
    PAND, POR,     // of kids (sorted, unique)
    IMPL,    // kids[0] |-> kids[1] (|=> when flag NONOVERLAP)
    NEXT,    // nexttime[lo] kids[0]; s_nexttime when strong
    ALWAYS,  // always[lo:hi] kids[0]
    EVENT,   // eventually[lo:hi] kids[0]
    UNTIL,   // kids[0] until kids[1]; until_with when flag WITH
    IF,      // if (atom) kids[0] else kids[1] (kids[1] may be absent: true)
    ACCEPT,  // accept_on(atom) kids[0]
    REJECT,  // reject_on(atom) kids[0]
    COVSEQ,  // cover sequence kids[0]: count every match
    // local variables (16.10)
    ENV,     // kids[0] read in environment lo
    MATCH,   // kids[0], running match items lo at the end of each match (never nullable)
    INIT,    // kids[0], running initializers lo when it starts
    REC      // a recursive property instance (16.12.17): the term recBody[lo]
};
enum Flag : uint8_t { STRONG = 1, NONOVERLAP = 2, WITH = 4, NEG = 8, LOCAL = 16 /* the atom reads a local variable */ };

// How the kernel evaluates what depends on local variables.
struct LocalHook {
    virtual bool atom(int atom, int env) = 0;           // an atom's value in an environment
    virtual int apply(int act, int env) = 0;            // the environment after match items or initializers
    virtual int merge(int ctx, int a, int b) = 0;       // the two sides of an intersection, joined
    virtual ~LocalHook() = default;
};

struct Node {
    K k = NUL;
    uint8_t flags = 0;
    bool nullable = false;
    bool isect = false;   // has an intersection inside: may be empty without being NUL
    bool envDep = false;  // depends on local variables: reads or sets one, or has an ENV inside
    bool prop = false;    // a property term (not a sequence)
    // Multiclock (16.13): the clock of a node that waits for ticks (an index
    // into the assertion's clocks; 0 is the leading one), and the clocks of
    // the node and everything in it. A step where none of those ticked
    // leaves the node as it is.
    int clk = -1;
    uint32_t mask = 0;
    int atom = -1;
    int lo = 0, hi = 0;
    std::vector<const Node*> kids;
    uint32_t id = 0;
    size_t hash = 0;
};

class Store {
    struct H { size_t operator()(const Node* n) const { return n->hash; } };
    struct Eq {
        bool operator()(const Node* a, const Node* b) const {
            return a->k == b->k && a->flags == b->flags && a->atom == b->atom && a->lo == b->lo && a->hi == b->hi &&
                   a->clk == b->clk && a->kids == b->kids;
        }
    };
    std::deque<Node> arena;
    std::unordered_set<const Node*, H, Eq> set;

public:
    // While lowering an assertion with local variables: which of its atoms read one.
    const std::vector<char>* localAtoms = nullptr;
    int clk = 0;   // the clock new waiting nodes get (16.13)
    static bool clocked(K k) {
        return k == BOOL || k == ANY || k == NEXT || k == ALWAYS || k == EVENT || k == IF || k == ACCEPT || k == REJECT;
    }
    const Node* mk(Node n) {
        if (!clocked(n.k)) n.clk = 0;
        else if (n.clk < 0) n.clk = clk;
        if (localAtoms && n.atom >= 0 && (n.k == BOOL || n.k == IF || n.k == ACCEPT || n.k == REJECT) &&
            n.atom < (int)localAtoms->size() && (*localAtoms)[n.atom])
            n.flags |= LOCAL;
        size_t h = (size_t)n.k * 1000003u ^ (size_t)n.flags * 7919u ^ (size_t)(n.atom + 1) * 31u ^
                   (size_t)(n.lo + 3) * 131u ^ (size_t)(n.hi + 3) * 524287u ^ (size_t)n.clk * 2654435761u;
        for (const Node* k : n.kids) h = h * 1315423911u ^ k->id;
        n.hash = h;
        if (auto it = set.find(&n); it != set.end()) return *it;
        n.id = (uint32_t)arena.size() + 1;
        switch (n.k) {
            case EMPTY: n.nullable = true; break;
            case CAT: n.nullable = n.kids[0]->nullable && n.kids[1]->nullable; break;
            case SOR: n.nullable = std::any_of(n.kids.begin(), n.kids.end(), [](const Node* k) { return k->nullable; }); break;
            case SAND: n.nullable = n.kids[0]->nullable && n.kids[1]->nullable; break;
            case REP: n.nullable = n.lo == 0 || n.kids[0]->nullable; break;
            case FM: n.nullable = n.kids[0]->nullable; break;
            case ENV: case INIT: n.nullable = n.kids[0]->nullable; break;
            default: n.nullable = false;
        }
        n.isect = n.k == SAND;
        for (const Node* k : n.kids) n.isect |= k->isect;
        n.envDep = (n.flags & LOCAL) || n.k == ENV || n.k == MATCH || n.k == INIT;
        for (const Node* k : n.kids) n.envDep |= k->envDep;
        n.prop = (n.k >= PTRUE && n.k <= COVSEQ) || n.k == REC || ((n.k == ENV || n.k == INIT) && n.kids[0]->prop);
        n.mask = n.k == REC ? ~0u : clocked(n.k) ? 1u << (n.clk & 31) : 0;
        for (const Node* k : n.kids) n.mask |= k->mask;
        arena.push_back(std::move(n));
        set.insert(&arena.back());
        return &arena.back();
    }
    void clear() { set.clear(); arena.clear(); recBody.clear(); }
    // Recursive property instances (16.12.17): REC(k) stands for recBody[k],
    // set once the body (which holds REC(k) after a time advance) is built.
    std::vector<const Node*> recBody;
    int recNew() { recBody.push_back(nullptr); return (int)recBody.size() - 1; }
    const Node* rec(int k) { Node n; n.k = REC; n.lo = k; return mk(n); }
    size_t size() const { return arena.size(); }

    // ---- sequence builders, with the simplifications that keep the set of
    // derivatives finite ----
    const Node* leaf(K k) { Node n; n.k = k; return mk(n); }
    const Node* empty() { return leaf(EMPTY); }
    const Node* nul() { return leaf(NUL); }
    const Node* any() { return leaf(ANY); }
    const Node* boolean(int atom, bool neg = false) {
        Node n; n.k = BOOL; n.atom = atom; n.flags = neg ? NEG : 0;
        return mk(n);
    }
    const Node* cat(const Node* a, const Node* b) {
        if (a->k == NUL || b->k == NUL) return nul();
        if (a->k == EMPTY) return b;
        if (b->k == EMPTY) return a;
        if (a->k == CAT) return cat(a->kids[0], cat(a->kids[1], b));   // right-nested
        Node n; n.k = CAT; n.kids = {a, b};
        return mk(n);
    }
    const Node* fuse(const Node* a, const Node* b) {
        if (a->k == NUL || b->k == NUL || a->k == EMPTY || b->k == EMPTY) return nul();   // 16.9.2.1
        Node n; n.k = FUSE; n.kids = {a, b};
        return mk(n);
    }
    const Node* sor(std::vector<const Node*> ks) {
        std::vector<const Node*> v;
        for (const Node* k : ks) {
            if (k->k == NUL) continue;
            if (k->k == SOR) v.insert(v.end(), k->kids.begin(), k->kids.end());
            else v.push_back(k);
        }
        std::sort(v.begin(), v.end(), [](const Node* a, const Node* b) { return a->id < b->id; });
        v.erase(std::unique(v.begin(), v.end()), v.end());
        if (v.empty()) return nul();
        if (v.size() == 1) return v[0];
        Node n; n.k = SOR; n.kids = std::move(v);
        return mk(n);
    }
    const Node* sand(const Node* a, const Node* b) {
        if (a->k == NUL || b->k == NUL) return nul();
        if (a == b) return a;
        if (a->k == EMPTY && !b->envDep) return b->nullable ? a : nul();
        if (b->k == EMPTY && !a->envDep) return a->nullable ? b : nul();
        if (a->id > b->id) std::swap(a, b);
        Node n; n.k = SAND; n.kids = {a, b};
        return mk(n);
    }
    const Node* rep(const Node* a, int lo, int hi) {
        if (hi == 0) return empty();
        if (a->k == NUL) return lo == 0 ? empty() : nul();
        if (a->k == EMPTY) return empty();
        if (lo == 1 && hi == 1) return a;
        Node n; n.k = REP; n.kids = {a}; n.lo = lo; n.hi = hi;
        return mk(n);
    }
    const Node* fm(const Node* a) {
        if (a->k == NUL || a->k == EMPTY) return a;
        if (a->k == FM) return a;
        Node n; n.k = FM; n.kids = {a};
        return mk(n);
    }
    const Node* star() { return rep(any(), 0, -1); }   // 1[*0:$]
    // r ##[lo:hi] s (hi < 0: $), and a leading ##[lo:hi] s when r is null.
    const Node* delay(const Node* r, int lo, int hi, const Node* s) {
        if (!r) return cat(rep(any(), lo, hi), s);
        std::vector<const Node*> alts;
        if (lo == 0) alts.push_back(fuse(r, s));
        int l1 = lo == 0 ? 0 : lo - 1;
        if (hi != 0) alts.push_back(cat(r, cat(rep(any(), l1, hi < 0 ? -1 : hi - 1), s)));
        return sor(alts);
    }
    const Node* seqAnd(const Node* a, const Node* b) {
        return sor({sand(cat(a, star()), b), sand(a, cat(b, star()))});
    }
    const Node* within(const Node* a, const Node* b) { return sand(cat(star(), cat(a, star())), b); }
    const Node* gotoRep(int atom, int lo, int hi) {
        return rep(cat(rep(boolean(atom, true), 0, -1), boolean(atom)), lo, hi);
    }
    const Node* nonconsec(int atom, int lo, int hi) { return cat(gotoRep(atom, lo, hi), rep(boolean(atom, true), 0, -1)); }

    // ---- property builders ----
    const Node* ptrue() { return leaf(PTRUE); }
    const Node* pfalse() { return leaf(PFALSE); }
    const Node* done() { return leaf(DONE); }
    const Node* pseq(const Node* r, bool strong) {
        Node n; n.k = PSEQ; n.kids = {r}; n.flags = strong ? STRONG : 0;
        return mk(n);
    }
    const Node* pnot(const Node* p) {
        if (p->k == PTRUE) return pfalse();
        if (p->k == PFALSE) return ptrue();
        if (p->k == PNOT) return p->kids[0];
        Node n; n.k = PNOT; n.kids = {p};
        return mk(n);
    }
    const Node* pjunct(K k, std::vector<const Node*> ks) {
        K unit = k == PAND ? PTRUE : PFALSE, zero = k == PAND ? PFALSE : PTRUE;
        std::vector<const Node*> v;
        for (const Node* x : ks) {
            if (x->k == zero) return x;
            if (x->k == unit) continue;
            if (x->k == k) v.insert(v.end(), x->kids.begin(), x->kids.end());
            else v.push_back(x);
        }
        std::sort(v.begin(), v.end(), [](const Node* a, const Node* b) { return a->id < b->id; });
        v.erase(std::unique(v.begin(), v.end()), v.end());
        if (v.empty()) return leaf(unit);
        if (v.size() == 1) return v[0];
        Node n; n.k = k; n.kids = std::move(v);
        return mk(n);
    }
    const Node* pand(std::vector<const Node*> ks) { return pjunct(PAND, std::move(ks)); }
    const Node* por(std::vector<const Node*> ks) { return pjunct(POR, std::move(ks)); }
    const Node* impl(const Node* r, const Node* p, bool nonOverlap) {
        if (r->k == NUL) return ptrue();
        Node n; n.k = IMPL; n.kids = {r, p}; n.flags = nonOverlap ? NONOVERLAP : 0;
        return mk(n);
    }
    // nexttime[k] p: p starts k ticks after the current one. nexttime[0] p
    // is p (16.12.10). As a residual (after a tick), NEXT with lo = 0 still
    // means "p from the next tick on", which at the end of simulation is a
    // strong obligation for s_nexttime and none for nexttime.
    const Node* next(const Node* p, int k, bool strong) {
        if (k <= 0) return p;
        return nextR(p, k, strong);
    }
    const Node* nextR(const Node* p, int k, bool strong) {
        Node n; n.k = NEXT; n.kids = {p}; n.lo = k; n.flags = strong ? STRONG : 0;
        return mk(n);
    }
    const Node* ranged(K k, const Node* p, int lo, int hi, bool strong) {
        Node n; n.k = k; n.kids = {p}; n.lo = lo; n.hi = hi; n.flags = strong ? STRONG : 0;
        return mk(n);
    }
    const Node* until(const Node* p, const Node* q, bool strong, bool with) {
        Node n; n.k = UNTIL; n.kids = {p, q};
        n.flags = (strong ? STRONG : 0) | (with ? WITH : 0);
        return mk(n);
    }
    const Node* pif(int atom, const Node* p, const Node* q) {
        Node n; n.k = IF; n.atom = atom; n.kids = {p};
        if (q) n.kids.push_back(q);
        return mk(n);
    }
    const Node* abort(K k, int atom, const Node* p) {
        if (p->k == PTRUE || p->k == PFALSE) return p;
        Node n; n.k = k; n.atom = atom; n.kids = {p};
        return mk(n);
    }
    const Node* covseq(const Node* r) {
        if (r->k == NUL) return done();
        Node n; n.k = COVSEQ; n.kids = {r};
        return mk(n);
    }
    // ---- local variables ----
    // t with the environment ids of its ENV nodes renamed by `to`.
    const Node* renumber(const Node* t, const std::unordered_map<int, int>& to, std::unordered_map<const Node*, const Node*>& memo) {
        if (!t->envDep) return t;
        if (auto it = memo.find(t); it != memo.end()) return it->second;
        std::vector<const Node*> kids;
        bool same = true;
        for (const Node* k : t->kids) { kids.push_back(renumber(k, to, memo)); same = same && kids.back() == k; }
        const Node* r;
        if (t->k == ENV) {
            int e = to.at(t->lo);
            r = e == t->lo && same ? t : env(e, kids[0]);
        } else if (same) r = t;
        else if (t->k == SOR) r = sor(std::move(kids));
        else if (t->k == PAND || t->k == POR) r = pjunct(t->k, std::move(kids));
        else { Node n; n.k = t->k; n.flags = t->flags; n.atom = t->atom; n.lo = t->lo; n.hi = t->hi; n.clk = t->clk; n.kids = std::move(kids); r = mk(n); }
        memo[t] = r;
        return r;
    }
    // The environment ids t uses, in order of first use.
    static void envsUsed(const Node* t, std::vector<int>& out, std::unordered_set<const Node*>& seen) {
        if (!t->envDep || !seen.insert(t).second) return;
        if (t->k == ENV && std::find(out.begin(), out.end(), t->lo) == out.end()) out.push_back(t->lo);
        for (const Node* k : t->kids) envsUsed(k, out, seen);
    }
    // t read in environment e. Verdicts and terms that do not depend on local
    // variables need no environment; a sequence keeps it, since what follows
    // it starts in the environment it ends in.
    const Node* env(int e, const Node* t) {
        if (t->k == NUL || t->k == PTRUE || t->k == PFALSE || t->k == DONE || t->k == ENV) return t;
        if (t->prop && !t->envDep) return t;
        Node n; n.k = ENV; n.lo = e; n.kids = {t};
        return mk(n);
    }
    const Node* match(const Node* r, int act) {
        if (act < 0) return r;
        if (r->k == NUL) return r;
        Node n; n.k = MATCH; n.lo = act; n.kids = {r};
        return mk(n);
    }
    const Node* init(int act, const Node* t) {
        if (act < 0) return t;
        Node n; n.k = INIT; n.lo = act; n.kids = {t};
        return mk(n);
    }
};
inline Store& store() { static Store s; return s; }

using Memo = std::unordered_map<uint64_t, const Node*>;
inline const Node* derivSeq(const Node* n, const std::vector<char>& val, Memo& memo);
inline bool seqLive(const Node* r);

// One derivative step: the term after a clock tick where atom i has value
// val[i]. `nonvac` is set when the step makes the attempt nonvacuous
// (16.14.8: an antecedent matched, a sequence property was evaluated);
// `matches` counts cover sequence matches.
struct Deriv {
    Store& st;
    const std::vector<char>& val;
    bool nonvac = false;
    int matches = 0;
    Memo memo;
    LocalHook* lh = nullptr;   // null: no local variables
    int env = 0;               // the context environment
    uint32_t ticking = ~0u;    // the clocks that ticked in this step (16.13): all of them for one clock
    bool ticks(const Node* n) const { return (ticking >> (n->clk & 31)) & 1; }
    bool atom(const Node* n) const {
        bool v = (n->flags & LOCAL) && lh ? lh->atom(n->atom, env) : val[n->atom] != 0;
        return v != ((n->flags & NEG) != 0);
    }

    const Node* d(const Node* n) {
        if (n->mask && !(n->mask & ticking)) return n;   // none of its clocks ticked: it waits (16.13)
        switch (n->k) {   // leaves without the memo
            case EMPTY: case NUL: return st.nul();
            case ANY: return ticks(n) ? st.empty() : n;
            case BOOL: return !ticks(n) ? n : atom(n) ? st.empty() : st.nul();
            case PTRUE: case PFALSE: case DONE: return n;
            default: break;
        }
        uint64_t key = (uint64_t)(n->envDep ? (uint32_t)env : 0u) << 32 | n->id;
        if (auto it = memo.find(key); it != memo.end()) return it->second;
        int savedClk = st.clk;
        st.clk = n->clk;   // nodes this step builds wait on the same clock
        const Node* r = step(n);
        st.clk = savedClk;
        memo[key] = r;
        return r;
    }
    // The derivative of n read in environment e, as a term read in the context.
    const Node* dIn(int e, const Node* n) {
        if (e == env) return d(n);
        int saved = env;
        env = e;
        const Node* r = d(n);
        env = saved;
        return st.env(e, r);
    }
    // The environments a nullable term can end in now, from context ctx.
    void exits(const Node* n, int ctx, std::vector<int>& out) {
        if (!n->nullable) return;
        if (!n->envDep || !lh) { out.push_back(ctx); return; }
        const auto& k = n->kids;
        switch (n->k) {
            case ENV: exits(k[0], n->lo, out); return;
            case INIT: exits(k[0], lh->apply(n->lo, ctx), out); return;
            case SOR: for (const Node* x : k) exits(x, ctx, out); return;
            case CAT: {
                std::vector<int> m;
                exits(k[0], ctx, m);
                for (int e : m) exits(k[1], e, out);
                return;
            }
            case SAND: {
                std::vector<int> a, b;
                exits(k[0], ctx, a);
                exits(k[1], ctx, b);
                for (int x : a)
                    for (int y : b) out.push_back(lh->merge(ctx, x, y));
                return;
            }
            case REP:
                if (n->lo == 0) out.push_back(ctx);
                exits(k[0], ctx, out);
                return;
            case FM: exits(k[0], ctx, out); return;
            default: out.push_back(ctx); return;
        }
    }
    // What follows a sequence that can end now: `next` started in each
    // environment it can end in.
    void follow(const Node* a, const Node* next, std::vector<const Node*>& out) {
        if (!a->envDep || !lh) { out.push_back(d(next)); return; }
        std::vector<int> ex;
        exits(a, env, ex);
        std::sort(ex.begin(), ex.end());
        ex.erase(std::unique(ex.begin(), ex.end()), ex.end());
        for (int e : ex) out.push_back(dIn(e, next));
    }
    // A derivative that can match now but never at a later tick: EMPTY, also
    // in an environment, with match items run on it (16.10), or a choice of
    // such.
    static bool endsNow(const Node* r) {
        switch (r->k) {
            case NUL: case EMPTY: return true;
            case ENV: case MATCH: return endsNow(r->kids[0]);
            case SOR:
                for (const Node* x : r->kids) if (!endsNow(x)) return false;
                return true;
            default: return false;
        }
    }
    const Node* step(const Node* n) {
        const auto& k = n->kids;
        switch (n->k) {
            case CAT: {
                const Node* r = st.cat(d(k[0]), k[1]);
                if (!k[0]->nullable) return r;
                std::vector<const Node*> v{r};
                follow(k[0], k[1], v);
                return st.sor(std::move(v));
            }
            case FUSE: {
                const Node* da = d(k[0]);
                const Node* r = st.fuse(da, k[1]);
                if (!da->nullable) return r;
                std::vector<const Node*> v{r};
                follow(da, k[1], v);
                return st.sor(std::move(v));
            }
            case ENV: return dIn(n->lo, k[0]);
            case INIT: return dIn(lh ? lh->apply(n->lo, env) : env, k[0]);
            case MATCH: {   // the items run at the end of each match of k[0] (16.10)
                const Node* r = d(k[0]);
                std::vector<const Node*> v{st.match(r, n->lo)};
                if (r->nullable) {
                    std::vector<int> ex;
                    exits(r, env, ex);
                    std::sort(ex.begin(), ex.end());
                    ex.erase(std::unique(ex.begin(), ex.end()), ex.end());
                    for (int e : ex) v.push_back(st.env(lh ? lh->apply(n->lo, e) : e, st.empty()));
                }
                return st.sor(std::move(v));
            }
            case SOR: {
                std::vector<const Node*> v;
                for (const Node* x : k) v.push_back(d(x));
                return st.sor(std::move(v));
            }
            case SAND: return st.sand(d(k[0]), d(k[1]));
            case REP: {
                int lo1 = n->lo > 0 ? n->lo - 1 : 0, hi1 = n->hi < 0 ? -1 : n->hi - 1;
                const Node* r = st.cat(d(k[0]), st.rep(k[0], lo1, hi1));
                if (n->lo > 0 && k[0]->nullable) r = st.sor({r, d(st.rep(k[0], lo1, hi1))});
                return r;
            }
            case FM: return k[0]->nullable ? st.nul() : st.fm(d(k[0]));
            case PSEQ: {
                nonvac = true;
                const Node* r = d(k[0]);
                if (r->nullable) return st.ptrue();
                if (!seqLive(r)) return st.pfalse();
                return st.pseq(r, n->flags & STRONG);
            }
            case PNOT: return st.pnot(d(k[0]));
            case PAND: case POR: {
                std::vector<const Node*> v;
                for (const Node* x : k) v.push_back(d(x));
                return n->k == PAND ? st.pand(std::move(v)) : st.por(std::move(v));
            }
            case IMPL: {
                const Node* r = d(k[0]);
                // No later match: an antecedent that can only end now (EMPTY) has no more.
                bool over = endsNow(r);
                const Node* later = over ? st.ptrue() : st.impl(r, k[1], n->flags & NONOVERLAP);
                if (!r->nullable) return later;
                nonvac = true;
                // s |=> p is s ##1 1 |-> p: with no later tick there is no match (16.12.7).
                if (n->flags & NONOVERLAP) {
                    std::vector<int> ex;
                    if (r->envDep && lh) exits(r, env, ex);
                    else ex.push_back(env);
                    std::vector<const Node*> v{later};
                    for (int e : ex) v.push_back(e == env ? st.nextR(k[1], 0, false) : st.env(e, st.nextR(k[1], 0, false)));
                    return st.pand(std::move(v));
                }
                std::vector<const Node*> v{later};
                follow(r, k[1], v);
                return st.pand(std::move(v));
            }
            // nexttime[0] as a residual is "from the next step on": it does not wait for its own clock.
            case NEXT: return n->lo == 0 ? d(k[0]) : !ticks(n) ? n : st.nextR(k[0], n->lo - 1, n->flags & STRONG);
            case ALWAYS: {
                if (!ticks(n)) return n;
                const Node* now = n->lo == 0 ? d(k[0]) : st.ptrue();
                const Node* rest = n->hi == 0 ? st.ptrue()
                                              : st.ranged(ALWAYS, k[0], n->lo ? n->lo - 1 : 0, n->hi < 0 ? -1 : n->hi - 1,
                                                          n->flags & STRONG);
                return st.pand({now, rest});
            }
            case EVENT: {
                if (!ticks(n)) return n;
                const Node* now = n->lo == 0 ? d(k[0]) : st.pfalse();
                const Node* rest = n->hi == 0 ? st.pfalse()
                                              : st.ranged(EVENT, k[0], n->lo ? n->lo - 1 : 0, n->hi < 0 ? -1 : n->hi - 1,
                                                          n->flags & STRONG);
                return st.por({now, rest});
            }
            case UNTIL: {
                const Node* dp = d(k[0]);
                const Node* dq = d(k[1]);
                const Node* stop = (n->flags & WITH) ? st.pand({dp, dq}) : dq;
                return st.por({stop, st.pand({dp, n})});
            }
            case IF:
                if (!ticks(n)) return n;
                return atom(n) ? d(k[0]) : k.size() > 1 ? d(k[1]) : st.ptrue();
            case ACCEPT: return ticks(n) && atom(n) ? st.ptrue() : st.abort(ACCEPT, n->atom, d(k[0]));
            case REJECT: return ticks(n) && atom(n) ? st.pfalse() : st.abort(REJECT, n->atom, d(k[0]));
            case COVSEQ: {
                const Node* r = d(k[0]);
                if (r->nullable) matches++;
                return st.covseq(r);
            }
            case REC: return d(st.recBody[(size_t)n->lo]);
            default: return st.nul();
        }
    }
};

inline const Node* derivSeq(const Node* n, const std::vector<char>& val, Memo& memo) {
    Deriv dv{store(), val};
    dv.memo.swap(memo);
    const Node* r = dv.d(n);
    dv.memo.swap(memo);
    return r;
}

// Whether a sequence term can still match. NUL cannot; a term with an
// intersection inside may be empty without being NUL ((c[*2] intersect b)),
// so its derivatives are explored for every valuation of its atoms. Too
// many atoms or terms: assumed live.
inline bool seqLive(const Node* r) {
    if (r->k == NUL) return false;
    if (!r->isect || r->nullable) return true;
    static std::unordered_map<const Node*, bool> cache;
    if (auto it = cache.find(r); it != cache.end()) return it->second;
    std::vector<int> atoms;
    std::vector<const Node*> todo{r};
    std::unordered_set<const Node*> seen{r};
    for (size_t i = 0; i < todo.size(); i++) {
        if (todo[i]->k == BOOL) atoms.push_back(todo[i]->atom);
        for (const Node* k : todo[i]->kids)
            if (seen.insert(k).second) todo.push_back(k);
    }
    std::sort(atoms.begin(), atoms.end());
    atoms.erase(std::unique(atoms.begin(), atoms.end()), atoms.end());
    bool live = true;
    if (atoms.size() <= 10) {
        int maxAtom = atoms.empty() ? 0 : atoms.back();
        std::vector<char> val((size_t)maxAtom + 1, 0);
        std::vector<const Node*> q{r};
        std::unordered_set<const Node*> vis{r};
        live = false;
        for (size_t i = 0; i < q.size() && !live; i++) {
            if (q.size() > 20000) { live = true; break; }
            for (uint32_t m = 0; m < (1u << atoms.size()) && !live; m++) {
                for (size_t b = 0; b < atoms.size(); b++) val[atoms[b]] = (m >> b) & 1;
                Memo memo;
                const Node* x = derivSeq(q[i], val, memo);
                if (x->nullable) live = true;
                else if (x->k != NUL && vis.insert(x).second) q.push_back(x);
            }
        }
    }
    cache[r] = live;
    return live;
}

// The verdict of a pending property when there are no more clock ticks
// (16.12.21): weak operators hold, strong ones fail.
inline bool endValue(const Node* n) {
    switch (n->k) {
        case PTRUE: case DONE: case COVSEQ: return true;
        case PFALSE: return false;
        case PSEQ: case NEXT: case EVENT: case UNTIL: return !(n->flags & STRONG);
        case ALWAYS: return !(n->flags & STRONG) || n->hi < 0;
        case PNOT: return !endValue(n->kids[0]);
        case PAND: for (auto* k : n->kids) if (!endValue(k)) return false; return true;
        case POR: for (auto* k : n->kids) if (endValue(k)) return true; return false;
        case IMPL: return true;
        case ACCEPT: case REJECT: case ENV: case INIT: return endValue(n->kids[0]);
        case REC: {   // a recursive instance only recurs after a time advance (16.12.17), so this ends
            static int depth = 0;
            if (depth > 64) return true;
            depth++;
            bool r = endValue(store().recBody[(size_t)n->lo]);
            depth--;
            return r;
        }
        default: return true;
    }
}

}  // namespace sva

// A concurrent assertion (16.14): what lowering builds and the kernel runs.
struct ConcAssert {
    int id = -1;                                // asserts() entry
    std::vector<std::pair<int, Edge>> ev;       // the leading clock
    E clkIff;                                   // @(posedge clk iff c): c, current value
    E disable;                                  // disable iff, current values (16.12); null: none
    std::vector<E> atoms;                       // boolean atoms, on sampled values
    std::vector<const Prog*> pre;               // their function calls: run at each tick before the atoms are read
    const sva::Node* root = nullptr;
    bool coverSeq = false;
    const Prog* pass = nullptr;
    const Prog* fail = nullptr;
    // A procedural assertion (16.14.6) starts attempts only when its
    // procedure reaches it. Automatic variables in it are read as they were
    // then: each is a constant slot, filled from the attempt's captured values.
    bool procedural = false;
    std::vector<int> capSig;                    // the automatic variables
    std::vector<E> capSlot;                     // their slots in the atoms
    // runtime: the live attempts, merged by term, vacuity and captured values
    using Caps = std::shared_ptr<const std::vector<Val>>;
    // An attempt's environments (16.10): ENV(i, ...) in its term reads (*envs)[i];
    // 0 is the initial one. Numbered in the order the term uses them, so equal
    // attempts have equal terms and the store does not grow with the values.
    using Envs = std::shared_ptr<const std::vector<std::vector<Val>>>;
    // wait: an expect statement's process, resumed with the verdict (16.17).
    struct Attempt { const sva::Node* t; bool nonvac; uint64_t count; uint64_t killGen; Caps caps; Envs envs = nullptr; std::shared_ptr<void> wait = nullptr; uint64_t start = 0; };
    std::vector<Attempt> att;
    // Procedural instances (16.14.6): pending ones on the process that queued
    // them (dropped at its next flush point), and matured ones waiting for
    // the next clock tick.
    struct Pending { std::shared_ptr<void> ctl; uint64_t epoch; Caps caps; std::shared_ptr<void> wait = nullptr; };
    std::vector<Pending> pending;
    std::vector<Pending> matured;
    bool expect = false;   // an expect statement (16.17)
    // Local variables (16.10): each is a hidden signal, set from an
    // environment before the atoms that read it are evaluated. acts[i] is a
    // list of match items or initializers: (local variable, value).
    // A match item or initializer: local variable k takes rhs; or k < 0: a
    // subroutine call (16.11), whose arguments `caps` are evaluated at the
    // match into the constant slots `slots` of `call`, run in the reactive region.
    struct Item { int k = -1; E rhs; const Prog* call = nullptr; std::vector<E> caps, slots; };
    std::vector<int> localSig;
    std::vector<std::vector<Item>> acts;
    // A sequence's end point tracker (16.13.6): at a tick where the sequence
    // matches, endSig (a count, for @(s)) and endSig + 1 (the time + 1, for
    // .triggered) are set, and so is each flag in matchFlags (.matched).
    int endSig = -1;
    // The clocks after the leading one (16.13): each event and iff.
    std::vector<std::pair<std::vector<std::pair<int, Edge>>, E>> more;
    uint32_t ticked = 0;   // runtime: the clocks that ticked in this time step
    bool due = false;      // runtime: a step is waiting for the observed region
    // An assume set of a checker (17.7.2): at each tick of its clock, before
    // the observed region, the active free variables solveVars take random
    // values, tried again while solveOk (the assumptions' checks in this time
    // step) is false, up to a limit; an unsuccessful attempt keeps the last.
    std::vector<int> solveVars;
    E solveOk;
    std::vector<int> matchFlags;
    std::vector<int> clearFlags;   // .matched flags this assertion reads: cleared after each of its ticks
};
inline std::vector<ConcAssert>& concAsserts() { static std::vector<ConcAssert> v; return v; }
inline std::vector<int>& concPending() { static std::vector<int> v; return v; }   // procedural ones with pending instances
