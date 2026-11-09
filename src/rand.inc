// Constrained randomization (IEEE 1800-2023 clause 18): the solver behind
// obj.randomize() and std::randomize(). Included by minisim.cpp after
// exec.inc.
//
// The variables are picked one at a time (solve-before order first, randc
// before rand, then declaration order). A variable's value is found by
// splitting its range in halves, keeping a half only when every constraint
// on the variable can still be true there. "Can be true" comes from interval
// arithmetic over the constraint's expression, with the variables not yet
// picked standing for their whole ranges, so a half is never dropped while
// it holds a solution. Once all of a constraint's variables are picked, the
// same evaluation is exact. A dead end retries the whole pick with new
// random choices, a bounded number of times. When that fails and there are
// soft constraints, they are added back from the highest priority down, each
// kept only if a solution remains (18.5.13.1).

using i128 = __int128;

namespace rnd {

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

// The math value of a known Val read as (w, s); false if it has x/z bits or
// is wider than the solver handles.
inline bool valOf(const Val& v, i128& out) {
    if (v.w > 64 || v.real || (v.x & (v.w >= 64 ? ~0ULL : ((1ULL << v.w) - 1)))) return false;
    uint64_t bits = v.w >= 64 ? v.v : v.v & ((1ULL << v.w) - 1);
    out = (i128)bits;
    if (v.sgn && v.w < 128 && (bits >> (v.w - 1) & 1)) out -= (i128)1 << v.w;
    return true;
}

struct Var {
    int addr = 0, w = 1;
    bool sgn = false, randc = false;
    int owner = 0;            // the object (randc state)
    bool set = false;
    i128 val = 0;
    i128 lo = 0, hi = 0;      // the range being searched
    std::vector<int> items;   // the constraints that read it
    const RandItem* dist = nullptr;   // a `v dist {...}` on it
    Frame* distFr = nullptr;
    bool ordered = false;     // solved first on purpose (solve-before, a function argument): its values alike
};
struct Item {
    const RandItem* it;
    Frame* fr;                // the frame its ids resolve in
    std::vector<int> vars;    // the variables it reads
    bool on = true;
    int prio = 0;             // soft: higher wins (18.5.13.1)
    std::vector<int> fnVars;  // the variables its function calls read (18.5.12)
};

struct Ctx {
    std::vector<Var> vars;
    std::unordered_map<int, int> idx;   // signal -> variable
    std::vector<Item> items;
    bool touched = false;               // a concrete read hit an unsolved variable
    long budget = 0;
};
inline Ctx* g_ctx = nullptr;

inline Val rcRead(int s) {
    auto it = g_ctx->idx.find(s);
    if (it == g_ctx->idx.end()) return S.get(s);
    Var& v = g_ctx->vars[(size_t)it->second];
    if (v.set || v.lo == v.hi) {
        i128 x = v.set ? v.val : v.lo;
        return make((uint64_t)x, 0, v.w, 0, v.sgn);
    }
    g_ctx->touched = true;
    return make((uint64_t)v.lo, 0, v.w, 0, v.sgn);
}
inline const Reader& rcReader() { static const Reader r = rcRead; return r; }

inline AV fromVal(const Val& v) {
    i128 x;
    if (valOf(v, x)) return single(x, v.w, v.sgn);
    return full(v.w, v.sgn);
}
// Evaluate e for real; exact unless it read an unsolved variable.
inline AV concrete(const E& e, bool& exact) {
    bool saved = g_ctx->touched;
    g_ctx->touched = false;
    Val v = eval(e, rcReader());
    exact = !g_ctx->touched;
    g_ctx->touched = saved || g_ctx->touched;
    if (!exact) return full(v.w, v.sgn);
    return fromVal(v);
}

inline i128 tdiv(i128 a, i128 b) { return a / b; }   // C++ truncates toward 0, as SV does

AV aeval(const E& e, bool& exact);
inline AV aAddr(int addr, bool sgn, bool& exact);
inline AV aRef(const E& e, bool& exact) {
    int addr;
    if (e->at) {
        Val h = eval(e->at, rcReader());
        if (h.x || !h.v) { exact = true; return full(1, false); }
        addr = (int)h.v + e->sig;
    } else addr = R(e->sig);
    return aAddr(addr, e->sgn, exact);
}
inline AV aAddr(int addr, bool sgn, bool& exact) {
    auto it = g_ctx->idx.find(addr);
    if (it != g_ctx->idx.end()) {
        Var& v = g_ctx->vars[(size_t)it->second];
        if (v.set) { exact = true; return conv(single(v.val, v.w, v.sgn), v.w, sgn); }
        exact = v.lo == v.hi;
        return conv(AV{v.lo, v.hi, v.w, v.sgn}, v.w, sgn);
    }
    exact = true;
    Val v = S.get(addr);
    v.sgn = sgn;
    return fromVal(v);
}
inline AV aeval(const E& e, bool& exact) {
    switch (e->op) {
        case Op::Const: exact = true; return fromVal(e->k);
        case Op::Ref: return aRef(e, exact);
        case Op::ArrRead: {   // an element: a variable when the index is known
            const ArrShape& a = *e->arr;
            std::vector<int64_t> idx;
            for (const E& k : e->kids) {
                bool ek;
                AV iv = aeval(k, ek);
                if (!ek || !isSingle(iv)) return concrete(e, exact);
                idx.push_back((int64_t)iv.lo);
            }
            int64_t off = a.offset(idx);
            if (off < 0) return concrete(e, exact);
            return aAddr(R(a.base) + (int)off, a.sgn, exact);
        }
        case Op::Resize: {
            bool ex;
            AV a = aeval(e->kids[0], ex);
            exact = ex;
            // A known value: extend it here rather than re-evaluating the
            // subtree (a long sum() chain would cost O(n^2)).
            if (ex && !isSingle(a)) return concrete(e, exact);
            if (isFull(a)) return full(e->hi, e->sgn);
            // The source bits, sign- or zero-extended, then read as the result.
            AV b = wrap(a.lo, a.hi, a.w, e->lo != 0);
            return wrap(b.lo, b.hi, e->hi, e->sgn);
        }
        case Op::Add: case Op::Sub: case Op::Mul: {
            bool ea, eb;
            AV a = aeval(e->kids[0], ea), b = aeval(e->kids[1], eb);
            exact = ea && eb;
            int w = std::max(a.w, b.w);
            bool s = a.sgn && b.sgn;
            // Two known values of one width: interval arithmetic is exact, and
            // avoids re-evaluating the subtrees (O(n^2) for a long sum chain).
            if (exact && !(a.w == b.w && w <= kMaxW / 2 && isSingle(a) && isSingle(b))) return concrete(e, exact);
            if (w > kMaxW / 2) return full(w, s);
            a = conv(a, w, s); b = conv(b, w, s);
            if (e->op == Op::Add) return wrap(a.lo + b.lo, a.hi + b.hi, w, s);
            if (e->op == Op::Sub) return wrap(a.lo - b.hi, a.hi - b.lo, w, s);
            i128 c[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
            return wrap(*std::min_element(c, c + 4), *std::max_element(c, c + 4), w, s);
        }
        case Op::Div: case Op::Mod: {
            bool ea, eb;
            AV a = aeval(e->kids[0], ea), b = aeval(e->kids[1], eb);
            exact = ea && eb;
            if (exact) return concrete(e, exact);
            int w = std::max(a.w, b.w);
            bool s = a.sgn && b.sgn;
            if (w > kMaxW / 2) return full(w, s);
            a = conv(a, w, s); b = conv(b, w, s);
            if (b.lo <= 0 && b.hi >= 0) return full(w, s);   // may divide by 0: x
            if (e->op == Op::Div) {
                i128 c[4] = {tdiv(a.lo, b.lo), tdiv(a.lo, b.hi), tdiv(a.hi, b.lo), tdiv(a.hi, b.hi)};
                return wrap(*std::min_element(c, c + 4), *std::max_element(c, c + 4), w, s);
            }
            i128 m = std::max(b.lo < 0 ? -b.lo : b.lo, b.hi < 0 ? -b.hi : b.hi) - 1;
            if (a.lo >= 0) return wrap(0, std::min(a.hi, m), w, s);
            if (a.hi <= 0) return wrap(std::max(a.lo, -m), 0, w, s);
            return wrap(-m, m, w, s);
        }
        case Op::Eq: case Op::Ne: case Op::CaseEq: case Op::WildEq: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
            bool ea, eb;
            AV a = aeval(e->kids[0], ea), b = aeval(e->kids[1], eb);
            exact = ea && eb;
            if (exact) return concrete(e, exact);
            // x ==? c with no x / z bits in c is x == c.
            if (e->op == Op::WildEq && !(eb && isSingle(b))) return boolAV(true, true);
            int w = std::max(a.w, b.w);
            bool s = a.sgn && b.sgn;
            a = conv(a, w, s); b = conv(b, w, s);
            bool t = true, f = true;
            switch (e->op) {
                case Op::Eq: case Op::CaseEq: case Op::WildEq:
                    t = a.lo <= b.hi && b.lo <= a.hi;
                    f = !(isSingle(a) && isSingle(b) && a.lo == b.lo);
                    break;
                case Op::Ne:
                    f = a.lo <= b.hi && b.lo <= a.hi;
                    t = !(isSingle(a) && isSingle(b) && a.lo == b.lo);
                    break;
                case Op::Lt: t = a.lo < b.hi; f = a.hi >= b.lo; break;
                case Op::Le: t = a.lo <= b.hi; f = a.hi > b.lo; break;
                case Op::Gt: t = a.hi > b.lo; f = a.lo <= b.hi; break;
                default:     t = a.hi >= b.lo; f = a.lo < b.hi; break;
            }
            return boolAV(f, t);
        }
        case Op::LogNot: {
            AV a = aeval(e->kids[0], exact);
            if (exact) return concrete(e, exact);
            return boolAV(canTrue(a), canFalse(a));
        }
        case Op::LogAnd: case Op::LogOr: {
            bool ea, eb;
            AV a = aeval(e->kids[0], ea);
            bool isAnd = e->op == Op::LogAnd;
            // Short circuit: a known false (&&) or true (||) left side decides.
            if (ea && isAnd && !canTrue(a)) { exact = true; return boolAV(true, false); }
            if (ea && !isAnd && !canFalse(a)) { exact = true; return boolAV(false, true); }
            AV b = aeval(e->kids[1], eb);
            exact = ea && eb;
            if (exact) return concrete(e, exact);
            if (isAnd) return boolAV(canFalse(a) || canFalse(b), canTrue(a) && canTrue(b));
            return boolAV(canFalse(a) && canFalse(b), canTrue(a) || canTrue(b));
        }
        case Op::Mux: {
            bool ec, ea, eb;
            AV c = aeval(e->kids[0], ec);
            if (ec && !canFalse(c)) return aeval(e->kids[1], exact);
            if (ec && !canTrue(c)) return aeval(e->kids[2], exact);
            AV a = aeval(e->kids[1], ea), b = aeval(e->kids[2], eb);
            exact = ec && ea && eb;
            if (exact) return concrete(e, exact);
            int w = std::max(a.w, b.w);
            bool s = a.sgn && b.sgn;
            a = conv(a, w, s); b = conv(b, w, s);
            if (ec && !canFalse(c)) return a;
            if (ec && !canTrue(c)) return b;
            return {std::min(a.lo, b.lo), std::max(a.hi, b.hi), w, s};
        }
        case Op::Not: {
            AV a = aeval(e->kids[0], exact);
            if (exact) return concrete(e, exact);
            if (isFull(a)) return a;
            if (a.sgn) return {-a.hi - 1, -a.lo - 1, a.w, true};
            i128 m = rMax(a.w, false);
            return {m - a.hi, m - a.lo, a.w, false};
        }
        case Op::And: case Op::Or: case Op::Xor: {
            bool ea, eb;
            AV a = aeval(e->kids[0], ea), b = aeval(e->kids[1], eb);
            exact = ea && eb;
            if (exact) return concrete(e, exact);
            int w = std::max(a.w, b.w);
            if (w > 64) return full(w, a.sgn && b.sgn);
            AV ua = conv(a, w, false), ub = conv(b, w, false);
            bool s = a.sgn && b.sgn;
            auto ceilMask = [](i128 v) { i128 m = 0; while (m < v) m = m * 2 + 1; return m; };
            AV r;
            if (e->op == Op::And) r = {0, std::min(ua.hi, ub.hi), w, false};
            else if (e->op == Op::Or) r = {std::max(ua.lo, ub.lo), ceilMask(std::max(ua.hi, ub.hi)), w, false};
            else r = {0, ceilMask(std::max(ua.hi, ub.hi)), w, false};
            return conv(r, w, s);
        }
        case Op::Shl: case Op::Shr: {
            bool ea, eb;
            AV a = aeval(e->kids[0], ea), b = aeval(e->kids[1], eb);
            exact = ea && eb;
            if (exact) return concrete(e, exact);
            if (!eb || isFull(a) || b.lo < 0 || b.lo > 64) { bool x; Val v = eval(e, rcReader()); (void)x; return full(v.w, v.sgn); }
            int sh = (int)b.lo;
            AV u = conv(a, a.w, false);
            if (e->op == Op::Shr) return conv(AV{u.lo >> sh, u.hi >> sh, a.w, false}, a.w, a.sgn);
            if (a.w + sh > kMaxW) return full(a.w, a.sgn);
            return wrap(u.lo << sh, u.hi << sh, a.w, a.sgn);
        }
        default: {
            // Anything else: exact when nothing it reads is unsolved.
            return concrete(e, exact);
        }
    }
}

// The signals an expression of a constraint's function call reads, except
// the callee's result (in the frame the call returned from, gone by now).
inline void fnAddrs(const E& e, std::vector<int>& out) {
    if (!e) return;
    auto ret = [](int s) { return isRel(s) && (s >> kRelOffBits & 15) == kRelRet; };
    if (e->op == Op::Ref && !e->at) { if (!ret(e->sig)) out.push_back(R(e->sig)); }
    else if (e->op == Op::Ref) { collectAddrs(e, out); return; }
    else if (e->op == Op::ArrRead) { if (!ret(e->arr->base)) for (int i = 0; i < e->arr->count; i++) out.push_back(R(e->arr->base) + i); }
    for (auto& k : e->kids) fnAddrs(k, out);
}

// Run item k's function calls (18.5.12) with the values its variables have
// now. False, running nothing, while one of them has no single value yet.
inline bool runFns(Item& k) {
    for (int vi : k.fnVars) {
        const Var& v = g_ctx->vars[(size_t)vi];
        if (!v.set && v.lo != v.hi) return false;
    }
    std::vector<std::pair<int, Val>> old;
    for (int vi : k.fnVars) {
        const Var& v = g_ctx->vars[(size_t)vi];
        i128 x = v.set ? v.val : v.lo;
        old.push_back({v.addr, S.get(v.addr)});
        S.set(v.addr, make(v.w >= 64 ? (uint64_t)x : (uint64_t)x & ((1ULL << v.w) - 1), 0, v.w, 0, v.sgn));
    }
    Frame* sf = g_fr;
    Frame* sr = g_ret;
    g_fr = k.fr;
    FrameP fp(k.fr, [](Frame*) {});   // the item's frame, owned by randomize()
    for (const auto& p : k.it->fns) {
        auto ctl = std::make_shared<ProcCtl>();
        if (g_threadRng) ctl->rngp = g_threadRng;   // the calling process's generator
        Task t = exec(p.get(), ctl, fp);
        t.h.resume();
        if (!t.h.done()) {
            cerr << "error: a function called in a constraint waited (time " << S.now << ")\n";
            exit(1);
        }
        t.h.destroy();
    }
    g_fr = sf;
    g_ret = sr;
    for (auto& [a, v] : old) S.set(a, v);
    return true;
}

// Can item k still be true (and must it be: *sure)?
inline bool itemOk(Item& k, bool* sure = nullptr) {
    if (!k.it->fns.empty() && !runFns(k)) {   // not known until its calls run
        if (sure) *sure = false;
        return true;
    }
    Frame* saved = g_fr;
    g_fr = k.fr;
    bool ok = true;
    bool ex;
    if (k.it->isDist && k.it->guard && canFalse(aeval(k.it->guard, ex))) {
        ok = true;   // under an implication that need not hold
    } else if (k.it->isDist) {
        AV a = aeval(k.it->e, ex);
        ok = false;
        bool e0;
        // With a default weight, a value no item names may be taken too.
        if (k.it->distDefault && canTrue(aeval(k.it->distDefault, e0))) ok = true;
        for (const RandDist& d : k.it->dist) {
            if (ok) break;
            bool e1;
            AV lo = aeval(d.lo, e1);
            AV hi = d.hi ? aeval(d.hi, e1) : lo;
            AV wt = aeval(d.weight, e1);
            if (!canTrue(wt)) continue;
            int w = std::max(a.w, lo.w);
            bool s = a.sgn && lo.sgn;
            AV x = conv(a, w, s), l = conv(lo, w, s), h = conv(hi, w, s);
            if (x.lo <= h.hi && l.lo <= x.hi) { ok = true; break; }
        }
    } else {
        AV r = aeval(k.it->e, ex);
        ok = canTrue(r);
        if (sure) *sure = ok && !canFalse(r);
    }
    g_fr = saved;
    return ok;
}
inline bool varOk(const Var& v) {
    for (int k : v.items) {
        Item& it = g_ctx->items[(size_t)k];
        if (it.on && !itemOk(it)) return false;
    }
    return true;
}
// 0: no value of v's range can work; 2: every value meets v's constraints
// (given the other variables' ranges); 1: some may.
inline int varState(const Var& v) {
    bool all = true;
    for (int k : v.items) {
        Item& it = g_ctx->items[(size_t)k];
        if (!it.on) continue;
        bool sure = false;
        if (!itemOk(it, &sure)) return 0;
        all = all && sure;
    }
    return all ? 2 : 1;
}

inline uint64_t* g_rng = nullptr;   // the generator of this randomize() call (18.14)
inline uint64_t rnd64() {
    uint64_t& st = g_rng ? *g_rng : urandomState();
    uint64_t a = urandomNext(st), b = urandomNext(st);
    return a << 32 | b;
}
// A uniform value in [0, n) for n >= 1.
inline i128 below(i128 n) {
    if (n <= 1) return 0;
    if (n <= ((i128)1 << 64)) {
        uint64_t un = (uint64_t)(n - 1);
        if (un == ~0ULL) return (i128)rnd64();
        uint64_t m = un + 1, lim = ~0ULL - (~0ULL % m + 1) % m;
        uint64_t r;
        do r = rnd64(); while (r > lim);
        return (i128)(r % m);
    }
    return ((i128)rnd64() << 64 | rnd64()) % n;
}

// Find a value of v in [lo, hi] (DFS over halves, random order by size).
inline bool search(Var& v, i128 lo, i128 hi) {
    if (--g_ctx->budget < 0) return false;
    v.lo = lo; v.hi = hi;
    if (!varOk(v)) return false;
    if (lo == hi) { v.set = true; v.val = lo; return true; }
    // Small ranges: try the values in random order.
    if (hi - lo < 8) {
        int n = (int)(hi - lo + 1);
        i128 c[8];
        for (int k = 0; k < n; k++) c[k] = lo + k;
        for (int k = n - 1; k > 0; k--) std::swap(c[k], c[(int)below(k + 1)]);
        for (int k = 0; k < n; k++)
            if (search(v, c[k], c[k])) return true;
        v.lo = lo; v.hi = hi;
        return false;
    }
    i128 mid = lo + (hi - lo) / 2;
    i128 n1 = mid - lo + 1, n2 = hi - mid;
    bool firstLow = below(n1 + n2) < n1;
    for (int t = 0; t < 2; t++) {
        bool low = (t == 0) == firstLow;
        if (low ? search(v, lo, mid) : search(v, mid + 1, hi)) return true;
    }
    v.lo = lo; v.hi = hi;
    return false;
}

// Bounds propagation: shrink every unpicked variable's range to the
// smallest and largest values where its constraints can still hold, until
// nothing changes. False if some variable has no value left.
inline bool feasibleIn(Var& v, i128 lo, i128 hi) {
    i128 slo = v.lo, shi = v.hi;
    v.lo = lo; v.hi = hi;
    bool ok = varOk(v);
    v.lo = slo; v.hi = shi;
    return ok;
}
// Shrink one variable's range to its feasible bounds: -1 if empty, 1 if changed.
inline int narrowVar(Var& v) {
    if (v.set || v.items.empty()) return 0;
    if (--g_ctx->budget < 0) return -1;
    if (!feasibleIn(v, v.lo, v.hi)) return -1;
    // smallest m with [lo, m] feasible (lo itself, most of the time)
    i128 a = v.lo, b = v.hi;
    if (a < b && feasibleIn(v, a, a)) b = a;
    while (a < b) {
        i128 m = a + (b - a) / 2;
        if (feasibleIn(v, v.lo, m)) b = m; else a = m + 1;
    }
    i128 nlo = a;
    a = nlo; b = v.hi;
    if (a < b && feasibleIn(v, b, b)) a = b;
    while (a < b) {
        i128 m = a + (b - a + 1) / 2;
        if (feasibleIn(v, m, v.hi)) a = m; else b = m - 1;
    }
    i128 nhi = a;
    if (nlo == v.lo && nhi == v.hi) return 0;
    v.lo = nlo; v.hi = nhi;
    return 1;
}
inline bool propagate() {
    for (int round = 0; round < 8; round++) {
        bool changed = false;
        for (auto& v : g_ctx->vars) {
            int r = narrowVar(v);
            if (r < 0) return false;
            if (r) changed = true;
        }
        if (!changed) break;
    }
    return true;
}

// randc state (18.4.2): values not yet given in this cycle, per object and variable.
inline std::map<std::pair<int, int>, std::vector<i128>>& randcLeft() {
    static std::map<std::pair<int, int>, std::vector<i128>> m;
    return m;
}

// log2 of how many values u has left. Bounds alone miss holes (`inside`
// sets, data < addr * 2 with addr in {[a:b], c}), so with few variables a
// big range is cut into chunks and only the chunks that can hold count.
inline double volLog2(Var& u) {
    i128 n = u.hi - u.lo + 1;
    if (n < 64 || g_ctx->vars.size() > 8 || u.items.empty())
        return std::log2((double)n);
    i128 step = n / 16;
    int ok = 0;
    for (int j = 0; j < 16; j++) {
        i128 a = u.lo + step * j, b = j == 15 ? u.hi : a + step - 1;
        if (feasibleIn(u, a, b)) ok++;
    }
    return ok ? std::log2((double)n * ok / 16) : -1e300;
}
inline bool pickVar(Var& v) {
    i128 flo = v.lo, fhi = v.hi;   // the range left after propagation
    if (v.randc && v.w <= 16) {
        auto& left = randcLeft()[{v.owner, v.addr}];
        for (int pass = 0; pass < 2; pass++) {
            if (left.empty())
                for (i128 x = rMin(v.w, v.sgn); x <= rMax(v.w, v.sgn); x++) left.push_back(x);
            for (size_t k = left.size(); k > 1; k--) std::swap(left[k - 1], left[(size_t)below((i128)k)]);
            for (size_t k = 0; k < left.size(); k++) {
                v.lo = v.hi = left[k];
                if (varOk(v)) {
                    v.set = true; v.val = left[k];
                    left.erase(left.begin() + (long)k);
                    return true;
                }
            }
            left.clear();   // nothing left in this cycle is allowed: start a new one
        }
        v.lo = flo; v.hi = fhi;
        return false;
    }
    if (v.dist) {
        // Pick a range by weight (18.5.3), then a value in it.
        struct Opt { i128 lo, hi; double w; };
        std::vector<Opt> opts;
        Frame* saved = g_fr;
        g_fr = v.distFr;
        for (const RandDist& d : v.dist->dist) {
            Val lo = eval(d.lo, rcReader()), hi = d.hi ? eval(d.hi, rcReader()) : lo, wt = eval(d.weight, rcReader());
            i128 l, h, wv;
            if (!valOf(lo, l) || !valOf(hi, h) || !valOf(wt, wv) || wv <= 0) continue;
            AV cl = conv(single(l, lo.w, lo.sgn), v.w, v.sgn), ch = conv(single(h, hi.w, hi.sgn), v.w, v.sgn);
            l = std::max(cl.lo, flo); h = std::min(ch.lo, fhi);
            if (l > h) continue;
            double weight = d.perRange || !d.hi ? (double)wv : (double)wv * (double)(h - l + 1);
            opts.push_back({l, h, weight});
        }
        if (v.dist->distDefault) {
            // default :/ w (18.5.3): w over the values in [flo, fhi] that no
            // item names, each gap by its size.
            Val wt = eval(v.dist->distDefault, rcReader());
            i128 wv;
            if (valOf(wt, wv) && wv > 0) {
                std::vector<std::pair<i128, i128>> named;
                for (const RandDist& d : v.dist->dist) {
                    Val lo = eval(d.lo, rcReader()), hi = d.hi ? eval(d.hi, rcReader()) : lo;
                    i128 l, h;
                    if (!valOf(lo, l) || !valOf(hi, h)) continue;
                    AV cl = conv(single(l, lo.w, lo.sgn), v.w, v.sgn), ch = conv(single(h, hi.w, hi.sgn), v.w, v.sgn);
                    if (cl.lo <= ch.lo) named.push_back({cl.lo, ch.lo});
                }
                std::sort(named.begin(), named.end());
                std::vector<std::pair<i128, i128>> gaps;
                i128 at = flo;
                for (auto [l, h] : named) {
                    if (h < at) continue;
                    if (l > at) gaps.push_back({at, std::min(l - 1, fhi)});
                    if (h >= fhi) { at = fhi + 1; break; }
                    at = std::max(at, h + 1);
                }
                if (at <= fhi) gaps.push_back({at, fhi});
                double tot = 0;
                for (auto& [l, h] : gaps) if (l <= h) tot += (double)(h - l + 1);
                for (auto& [l, h] : gaps)
                    if (l <= h) opts.push_back({l, h, (double)wv * (double)(h - l + 1) / tot});
            }
        }
        g_fr = saved;
        while (!opts.empty()) {
            double tot = 0;
            for (auto& o : opts) tot += o.w;
            double r = (double)(rnd64() >> 11) / 9007199254740992.0 * tot;
            size_t k = 0;
            for (; k + 1 < opts.size(); k++) { if (r < opts[k].w) break; r -= opts[k].w; }
            if (search(v, opts[k].lo, opts[k].hi)) return true;
            opts.erase(opts.begin() + (long)k);
        }
    }
    // A variable with few values (a mode bit, an enum): each value weighted
    // by how many ways the other variables can still be completed, measured
    // as the product of their ranges after propagation, so the solutions
    // come out close to uniform (18.5.10: `s -> d == 0` gives s = 1 rarely).
    if (!v.ordered && fhi - flo < 16 && g_ctx->vars.size() <= 32 && !v.items.empty()) {
        struct Snap { i128 lo, hi; };
        std::vector<Snap> snap;
        for (auto& u : g_ctx->vars) snap.push_back({u.lo, u.hi});
        auto restore = [&]() {
            for (size_t k = 0; k < snap.size(); k++) { g_ctx->vars[k].lo = snap[k].lo; g_ctx->vars[k].hi = snap[k].hi; }
        };
        std::vector<std::pair<i128, double>> cand;   // value, log2 of its completions
        double best = -1e300;
        for (i128 x = flo; x <= fhi; x++) {
            v.lo = v.hi = x;
            if (varOk(v) && propagate()) {
                double lg = 0;
                for (auto& u : g_ctx->vars)
                    if (!u.set && &u != &v) lg += volLog2(u);
                cand.push_back({x, lg});
                best = std::max(best, lg);
            }
            restore();
        }
        while (!cand.empty()) {
            double tot = 0;
            for (auto& c : cand) tot += std::exp2(c.second - best);
            double r = (double)(rnd64() >> 11) / 9007199254740992.0 * tot;
            size_t k = 0;
            for (; k + 1 < cand.size(); k++) { double w = std::exp2(cand[k].second - best); if (r < w) break; r -= w; }
            if (search(v, cand[k].first, cand[k].first)) return true;
            restore();
            cand.erase(cand.begin() + (long)k);
        }
        v.lo = flo; v.hi = fhi;
        return false;
    }
    // Cover the range with pieces that can hold a value, splitting the
    // biggest undecided piece a bounded number of times, then pick a piece
    // by its size, so the values come out close to uniform (a piece of size
    // n stands for up to n values).
    struct Piece { i128 lo, hi; int st; };
    std::vector<Piece> ps;
    {
        v.lo = flo; v.hi = fhi;
        int st = varState(v);
        if (st) ps.push_back({flo, fhi, st});
    }
    for (int evals = 0; evals < 96 && ps.size() < 48; evals += 2) {
        size_t best = ps.size();
        for (size_t k = 0; k < ps.size(); k++)
            if (ps[k].st == 1 && ps[k].lo < ps[k].hi && (best == ps.size() || ps[k].hi - ps[k].lo > ps[best].hi - ps[best].lo)) best = k;
        if (best == ps.size()) break;
        Piece p = ps[best];
        ps.erase(ps.begin() + (long)best);
        i128 mid = p.lo + (p.hi - p.lo) / 2;
        for (auto [a, b] : {std::pair<i128, i128>{p.lo, mid}, {mid + 1, p.hi}}) {
            v.lo = a; v.hi = b;
            int st = varState(v);
            if (st) ps.push_back({a, b, st});
        }
    }
    // Each piece also weighted by how many ways the other variables can
    // still be completed (x < y: a small x leaves y more room), as above.
    // Big pieces are cut into chunks first, so the weight can vary inside
    // what would otherwise be one piece (data < addr * 2 with data first).
    bool weigh = !v.ordered && g_ctx->vars.size() > 1 && g_ctx->vars.size() <= 32;
    if (weigh && ps.size() < 16) {
        std::vector<Piece> cut;
        for (auto& p : ps) {
            i128 n = p.hi - p.lo + 1;
            int parts = n >= 64 ? 16 : 1;
            for (int j = 0; j < parts; j++) {
                i128 a = p.lo + n / parts * j, b = j == parts - 1 ? p.hi : a + n / parts - 1;
                cut.push_back({a, b, p.st});
            }
        }
        ps = std::move(cut);
    }
    weigh = weigh && ps.size() > 1;
    std::vector<double> lgs(ps.size(), 0.0);
    if (weigh) {
        std::vector<std::pair<i128, i128>> snap;
        for (auto& u : g_ctx->vars) snap.push_back({u.lo, u.hi});
        double best = -1e300;
        for (size_t k = 0; k < ps.size(); k++) {
            v.lo = ps[k].lo; v.hi = ps[k].hi;
            double lg = -1e300;
            if (propagate()) {
                lg = 0;
                // v counts too, narrowed: a piece [8000:80ff] may hold one value
                for (auto& u : g_ctx->vars)
                    if (!u.set) lg += volLog2(u);
            }
            lgs[k] = lg;
            best = std::max(best, lg);
            for (size_t j = 0; j < snap.size(); j++) { g_ctx->vars[j].lo = snap[j].first; g_ctx->vars[j].hi = snap[j].second; }
        }
        for (auto& l : lgs) l -= best;
    }
    while (!ps.empty()) {
        size_t k = 0;
        if (weigh) {
            double tot = 0;
            for (size_t j = 0; j < ps.size(); j++) tot += std::exp2(lgs[j]);
            double r = (double)(rnd64() >> 11) / 9007199254740992.0 * tot;
            for (; k + 1 < ps.size(); k++) {
                double w = std::exp2(lgs[k]);
                if (r < w) break;
                r -= w;
            }
        } else {
            i128 tot = 0;
            for (auto& p : ps) tot += p.hi - p.lo + 1;
            i128 r = below(tot);
            for (; k + 1 < ps.size(); k++) { if (r < ps[k].hi - ps[k].lo + 1) break; r -= ps[k].hi - ps[k].lo + 1; }
        }
        if (ps[k].st == 2) {   // every value works: take one directly
            v.set = true;
            v.val = ps[k].lo + below(ps[k].hi - ps[k].lo + 1);
            return true;
        }
        if (search(v, ps[k].lo, ps[k].hi)) return true;
        ps.erase(ps.begin() + (long)k);
        lgs.erase(lgs.begin() + (long)k);
    }
    v.lo = flo; v.hi = fhi;
    return false;
}

// One attempt at the variables in `order`.
inline bool solveOnce(const std::vector<int>& order) {
    for (auto& v : g_ctx->vars) { v.set = false; v.lo = rMin(v.w, v.sgn); v.hi = rMax(v.w, v.sgn); }
    if (!propagate()) return false;
    // Many variables (rand arrays): full propagation after every pick costs
    // O(n^2) item checks per pick, so narrow only the variable picked next.
    bool lazy = g_ctx->vars.size() > 32;
    for (size_t i = 0; i < order.size(); i++) {
        auto& v = g_ctx->vars[(size_t)order[i]];
        if (lazy && i > 0 && narrowVar(v) < 0) return false;
        if (!pickVar(v)) return false;
        if (!lazy && !propagate()) return false;
    }
    // Constraints that read no random variable still have to hold.
    for (auto& it : g_ctx->items)
        if (it.on && it.vars.empty() && !itemOk(it)) return false;
    return true;
}

}   // namespace rnd

// pre_randomize / post_randomize (18.6.2): run a method to its end now.
static void runSync(const Prog* p, const FrameLayout* lay, int self) {
    Frame* sf = g_fr;
    Frame* sr = g_ret;
    auto ctl = make_shared<ProcCtl>();
    if (g_threadRng) ctl->rngp = g_threadRng;   // the calling process's generator
    Task t = exec(p, ctl, newFrame(lay, nullptr, self));
    t.h.resume();
    if (!t.h.done()) {
        cerr << "error: pre_randomize/post_randomize waited (time " << S.now << ")\n";
        exit(1);
    }
    t.h.destroy();
    g_fr = sf;
    g_ret = sr;
}

// The objects one randomize() covers: obj and, depth first, the objects of
// its rand class handles that are on and not null (18.4).
static void randObjects(int obj, std::vector<int>& out) {
    if (!obj || std::find(out.begin(), out.end(), obj) != out.end()) return;
    out.push_back(obj);
    const ClassRt& cr = classRt()[(size_t)S.sigs[obj].val.v];
    if (!cr.rand) return;
    for (const RandVar& rv : cr.rand->vars) {
        if (!rv.obj || (S.get(obj + rv.mode).v & 1) == 0) continue;
        if (rv.dyn) {   // a rand container of handles: each element's object, at every level
            std::vector<int> hs;
            std::function<void(int, int)> walk = [&](int cid, int depth) {
                const DynObj& d = dynStore()[cid];
                std::vector<Val> el(d.vec.begin(), d.vec.end());
                for (auto& kv : d.map) el.push_back(kv.second);
                for (const Val& h : el)
                    if (!h.x && h.v) {
                        if (depth > 1) walk((int)h.v, depth - 1);
                        else hs.push_back((int)h.v);
                    }
            };
            for (int k = 0; k < rv.count; k++) walk(obj + rv.off + k, rv.depth);
            for (int h : hs) randObjects(h, out);
            continue;
        }
        for (int k = 0; k < rv.count; k++) {
            Val h = S.get(obj + rv.off + k);
            if (!h.x && h.v) randObjects((int)h.v, out);
        }
    }
}

// obj.randomize() / std::randomize(): 1 when every constraint was met.
// Rand dynamic arrays and queues (18.4): their sizes are solved first, then
// their elements, each in a scratch signal while the solver works.
namespace rnd {
struct RCont {
    int cid = 0, w = 1, owner = 0;
    bool sgn = false;
    FrameP sizeFr;            // pass 1: the size, when a constraint reads it
    int64_t n = 0;            // the size for pass 2
    bool assoc = false;       // an associative array: its keys stay, only the values change
    std::vector<DynKey> keys;
    FrameP elems;             // pass 2: one signal per element
    int depth = 1;            // int m[][]: 2 until its size is fixed and its elements stand in for it
    bool sized = false;       // its size is fixed (an outer level was solved before)
};
constexpr int kSizeW = 17;    // a solved size is at most 2^17 - 1
inline const FrameLayout* scratchLayout(int w, bool sgn, int64_t n) {
    static std::map<std::tuple<int, bool, int64_t>, std::unique_ptr<FrameLayout>> m;
    auto& l = m[{w, sgn, n}];
    if (!l) {
        l = std::make_unique<FrameLayout>();
        l->name = "randomize scratch";
        for (int64_t k = 0; k < n; k++) l->add(w, make(0, 0, w, 0, sgn));
    }
    return l.get();
}
struct Rewrite {
    std::vector<RCont>* conts = nullptr;
    std::unordered_map<int, int>* contIdx = nullptr;
    int pass = 1;
    std::vector<std::pair<int, Val>> loops;       // loop variable -> index (or key)
    std::vector<std::pair<int, E>> items;          // `item` of a with clause -> the element
    bool dropped = false;                         // pass 1: reads an element
    std::vector<int>* sizeVars = nullptr;         // pass 1: containers whose size is read
    std::vector<const std::vector<std::pair<int, E>>*> refScope;   // RandItem::refs in force
    const E* refOf(int sig) {
        if (sig < kRel) return nullptr;
        for (size_t k = refScope.size(); k-- > 0;)
            for (auto& [d, r] : *refScope[k])
                if (d == sig) return &r;
        return nullptr;
    }
    // The container m[i] names now (loop indices in place), or -1 when m is
    // itself still being sized (the constraint waits for a later pass).
    int resolveRef(const E& r) {
        E x = (*this)(r);
        if (dropped || contOf(x->sig)) { dropped = true; return -1; }
        Val v = eval(x, rd);
        return (int)v.v - 1;
    }
    RCont* contOf(int sig) {
        auto it = contIdx->find(R(sig));
        return it == contIdx->end() ? nullptr : &(*conts)[(size_t)it->second];
    }
    E elem(RCont& c, int64_t k) {
        if (k < 0 || k >= c.n) return konst(make(0, 0, c.w, 0, c.sgn));
        return ref(c.elems->base + (int)k, c.sgn);
    }
    E operator()(const E& e0) {
        if (!e0) return e0;
        E e = e0;
        if (e->op != Op::Ref && !refScope.empty())
            if (const E* r = refOf(e->sig)) {   // m[i].size(), m[i][j]: on the container m[i] is now
                int cid = resolveRef(*r);
                if (cid < 0) return e0;
                auto c = std::make_shared<Expr>(*e);
                c->sig = cid;
                e = c;
            }
        if (e->op == Op::Ref && !e->at) {
            for (auto& [lv, k] : loops)
                if (e->sig == lv) return konst(k);
            for (auto& [iv, x] : items)
                if (e->sig == iv) return x;
        }
        if (e->op == Op::DynReduce && !e->kids.empty()) {   // sum() with (...) in a constraint: folded
            RCont* c = contOf(e->sig);
            if (c && pass == 1) { dropped = true; return e; }
            int64_t n = c ? c->n : (int64_t)dynStore()[R(e->sig)].size();
            int w = e->k.w;
            bool sg = e->k.sgn;
            static const Op ops[] = {Op::Add, Op::Mul, Op::And, Op::Or, Op::Xor};
            if (n == 0) return konst(make(e->hi == 1 ? 1 : e->hi == 2 ? ~0ULL : 0, 0, w, 0, sg));
            E acc;
            for (int64_t k = 0; k < n; k++) {
                E el;
                if (c) el = elem(*c, k);
                else {
                    const DynObj& d = dynStore()[R(e->sig)];
                    el = konst(d.kind == 2 ? std::next(d.map.begin(), (long)k)->second : d.vec[(size_t)k]);
                }
                items.push_back({e->lo, el});
                E x = (*this)(e->kids[0]);
                items.pop_back();
                acc = acc ? bin(ops[e->hi], acc, x) : x;
            }
            auto r = resize(acc, w, sg, false);
            r->sgn = sg;
            return r;
        }
        if (e->op == Op::DynSize)
            if (RCont* c = contOf(e->sig)) {
                if (pass == 2 || c->assoc || c->sized) return konst(make((uint64_t)c->n, 0, 32));   // an associative array keeps its keys
                if (!c->sizeFr) {
                    c->sizeFr = newFrame(scratchLayout(kSizeW, false, 1), nullptr, 0);
                    S.set(c->sizeFr->base, make((uint64_t)c->n, 0, kSizeW));
                    sizeVars->push_back((int)(c - conts->data()));
                }
                return ref(c->sizeFr->base, false);
            }
        if (e->op == Op::DynGet || e->op == Op::DynReduce)
            if (RCont* c = contOf(e->sig)) {
                if (pass == 1) { dropped = true; return e; }
                if (e->op == Op::DynGet) {
                    E ix = (*this)(e->kids[0]);
                    Val iv = eval(ix, rd);
                    if (iv.x) return konst(make(0, 0, c->w, 0, c->sgn));
                    if (c->assoc) {   // the element at that key
                        DynKey dk = dynStore()[c->cid].key(iv);
                        auto f = std::lower_bound(c->keys.begin(), c->keys.end(), dk);
                        if (f == c->keys.end() || dk < *f) return konst(dynStore()[c->cid].dflt);
                        return elem(*c, f - c->keys.begin());
                    }
                    int64_t k = (int64_t)iv.v;
                    if (iv.sgn && iv.w < 64 && (iv.v >> (iv.w - 1) & 1)) k -= (int64_t)1 << iv.w;
                    return elem(*c, k);
                }
                // sum / product / and / or / xor (7.12.3): folded over the elements
                int w = e->k.w;
                bool sg = e->k.sgn;
                static const Op ops[] = {Op::Add, Op::Mul, Op::And, Op::Or, Op::Xor};
                if (c->n == 0) {
                    uint64_t id = e->hi == 1 ? 1 : e->hi == 2 ? ~0ULL : 0;
                    return konst(make(id, 0, w, 0, sg));
                }
                E acc;
                for (int64_t k = 0; k < c->n; k++) {
                    E x = resize(elem(*c, k), w, c->sgn, false);
                    acc = acc ? bin(ops[e->hi], acc, x) : x;
                }
                acc->sgn = sg;
                return acc;
            }
        std::vector<E> kids;
        bool changed = false;
        for (auto& kd : e->kids) { kids.push_back((*this)(kd)); changed |= kids.back() != kd; }
        E at = (*this)(e->at);
        changed |= at != e->at;
        if (!changed) return e;
        auto c = std::make_shared<Expr>(*e);
        c->kids = std::move(kids);
        c->at = at;
        return c;
    }
};
}   // namespace rnd

static int doRandomize(int obj, const RandCall* rc) {
    using namespace rnd;
    std::vector<int> objs;
    if (obj) randObjects(obj, objs);
    for (int o : objs) {
        const auto& sp = classRt()[(size_t)S.sigs[o].val.v].rand;
        if (sp && sp->pre) runSync(sp->pre, sp->preLay, o);
    }
    uint64_t* savedRng = g_rng;
    g_rng = obj ? &objRng()[obj] : nullptr;
    Frame* callerFr = g_fr;
    // with {...} and randomize(a, b) on an object: the caller's frame, with
    // `this` the object (18.7).
    Frame withFr;
    if (callerFr) { withFr.base = callerFr->base; withFr.up = callerFr->up; }
    withFr.self = obj;
    Frame* callFr = obj ? &withFr : callerFr;
    std::vector<std::unique_ptr<Frame>> selfFrames;
    auto frameFor = [&](int o) {
        auto f = std::make_unique<Frame>();
        f->self = o;
        selfFrames.push_back(std::move(f));
        return selfFrames.back().get();
    };
    // What takes part: scalar variables, containers, constraint lists.
    struct ScalarVar { int addr, w; bool sgn, randc; int owner; };
    std::vector<ScalarVar> scalars;
    std::vector<RCont> conts;
    std::unordered_map<int, int> contIdx;
    struct Src { const std::vector<RandItem>* items; Frame* fr; };
    std::vector<Src> srcs;
    std::vector<std::pair<const std::vector<std::pair<std::vector<E>, std::vector<E>>>*, Frame*>> befores;
    if (rc && rc->argsGiven) {
        // randomize(a, b) / std::randomize(a, b): just these (18.11, 18.12).
        g_fr = callFr;
        for (const E& a : rc->args) {
            int addr = a->at ? (int)eval(a->at, rd).v + a->sig : R(a->sig);
            scalars.push_back({addr, S.sigs[addr].w, a->sgn, false, obj});
        }
        for (const auto& ca : rc->contArgs) {   // randomize(q): its size and elements
            RCont c;
            c.cid = ca.addr ? (int)eval(ca.addr, rd).v - 1 : R(ca.sig); c.w = ca.w; c.sgn = ca.sgn; c.owner = obj; c.depth = ca.depth;
            const DynObj& dd = dynStore()[c.cid];
            c.n = (int64_t)dd.size();
            if (dd.kind == 2) {
                c.assoc = true;
                for (auto& kv : dd.map) c.keys.push_back(kv.first);
            }
            if (!contIdx.count(c.cid)) { contIdx[c.cid] = (int)conts.size(); conts.push_back(std::move(c)); }
        }
        g_fr = callerFr;
    }
    for (int o : objs) {
        const auto& sp = classRt()[(size_t)S.sigs[o].val.v].rand;
        if (!sp) continue;
        Frame* fr = frameFor(o);
        if (!(rc && rc->argsGiven))
            for (const RandVar& rv : sp->vars) {
                if (rv.obj || !(S.get(o + rv.mode).v & 1)) continue;
                if (rv.dyn) for (int k0 = 0; k0 < rv.count; k0++) {   // int q[4][$]: each queue
                    RCont c;
                    c.cid = o + rv.off + k0; c.w = rv.w; c.sgn = rv.sgn; c.owner = o; c.depth = rv.depth;
                    const DynObj& dd = dynStore()[c.cid];
                    c.n = (int64_t)dd.size();
                    if (dd.kind == 2) {
                        c.assoc = true;
                        for (auto& kv : dd.map) c.keys.push_back(kv.first);
                    }
                    if (!contIdx.count(c.cid)) { contIdx[c.cid] = (int)conts.size(); conts.push_back(std::move(c)); }
                }
                if (rv.dyn) continue;
                for (int k = 0; k < rv.count; k++) scalars.push_back({o + rv.off + k, rv.w, rv.sgn, rv.randc, o});
            }
        for (const auto& b : sp->blocks) {
            if (b->staticMode) { Val m = S.get(b->mode); if (!m.x && !(m.v & 1)) continue; }
            else if (b->mode >= 0 && (S.get(o + b->mode).v & 1) == 0) continue;
            srcs.push_back({&b->items, fr});
            befores.push_back({&b->before, fr});
        }
    }
    if (rc) {
        srcs.push_back({&rc->items, callFr});
        befores.push_back({&rc->before, callFr});
    }

    // One solve. Pass 0: no containers. Pass 1: their sizes, with the
    // constraints that read no element. Pass 2: elements, sizes fixed.
    auto solvePass = [&](int pass, std::vector<int>& sizeVars) -> bool {
        Ctx ctx;
        Ctx* savedCtx = g_ctx;
        g_ctx = &ctx;
        std::deque<RandItem> owned;
        auto addVar = [&](int addr, int w, bool sgn, bool randc, int owner) {
            if (ctx.idx.count(addr) || w > 64) return;
            ctx.idx[addr] = (int)ctx.vars.size();
            Var v;
            v.addr = addr; v.w = w; v.sgn = sgn; v.randc = randc; v.owner = owner;
            ctx.vars.push_back(v);
        };
        for (auto& sv : scalars) addVar(sv.addr, sv.w, sv.sgn, sv.randc, sv.owner);
        if (pass == 2)
            for (auto& c : conts)
                for (int64_t k = 0; k < c.n; k++) addVar(c.elems->base + (int)k, c.w, c.sgn, false, c.owner);
        int prio = 0;
        Rewrite rw;
        rw.conts = &conts; rw.contIdx = &contIdx; rw.pass = pass; rw.sizeVars = &sizeVars;
        std::function<void(const std::vector<RandItem>&, Frame*)> addItems = [&](const std::vector<RandItem>& its, Frame* fr) {
            for (const RandItem& it : its) {
                g_fr = fr;
                rw.refScope.push_back(&it.refs);
                struct Pop { Rewrite& r; ~Pop() { r.refScope.pop_back(); } } pop{rw};
                if (it.loopCont >= 0) {   // foreach over a container: once per element
                    int64_t n;
                    int lc = it.loopCont;
                    if (const E* r = rw.refOf(lc)) {   // foreach (m[i][j]): over the container m[i] is now
                        rw.dropped = false;
                        lc = rw.resolveRef(*r);
                        if (lc < 0) continue;
                    }
                    const DynObj& d = dynStore()[R(lc)];
                    if (RCont* c = rw.contOf(lc)) {
                        if (pass == 1) continue;   // its elements are not known yet
                        n = c->n;
                    } else n = (int64_t)d.size();
                    // An associative array (7.8): the loop variable is each key, in order.
                    std::vector<Val> keys;
                    if (d.kind == 2) for (auto& kv : d.map) keys.push_back(d.keyVal(kv.first));
                    for (int64_t k = 0; k < n; k++) {
                        rw.loops.push_back({it.loopVar, d.kind == 2 ? keys[(size_t)k] : make((uint64_t)k, 0, 32, 0, true)});
                        addItems(it.body, fr);
                        rw.loops.pop_back();
                    }
                    continue;
                }
                if (!it.uniqConts.empty()) {   // unique {q, ...}: pairwise differences now
                    std::vector<E> el;
                    bool wait = false;
                    rw.dropped = false;
                    for (int cs : it.uniqConts) {
                        if (const E* r = rw.refOf(cs)) { cs = rw.resolveRef(*r); if (cs < 0) { wait = true; break; } }
                        if (RCont* c = rw.contOf(cs)) {
                            if (pass == 1) { wait = true; break; }   // elements not known yet
                            for (int64_t k = 0; k < c->n; k++) el.push_back(rw.elem(*c, k));
                        } else {
                            const DynObj& d = dynStore()[R(cs)];
                            for (auto& v : d.vec) el.push_back(konst(v));
                            for (auto& kv : d.map) el.push_back(konst(kv.second));
                        }
                    }
                    if (wait) continue;
                    for (const E& x : it.uniqElems) el.push_back(rw(x));
                    E g = rw(it.guard);
                    if (rw.dropped) continue;
                    for (size_t a = 0; a < el.size(); a++)
                        for (size_t b = a + 1; b < el.size(); b++) {
                            RandItem x;
                            E ne = bin(Op::Ne, el[a], el[b]);
                            x.e = g ? bin(Op::LogOr, un(Op::LogNot, g), ne) : ne;
                            owned.push_back(std::move(x));
                            Item k;
                            k.it = &owned.back(); k.fr = fr;
                            ctx.items.push_back(k);
                        }
                    continue;
                }
                const RandItem* use = &it;
                {
                    rw.dropped = false;
                    RandItem x = it;
                    x.e = rw(it.e);
                    x.guard = rw(it.guard);
                    for (auto& d : x.dist) { d.lo = rw(d.lo); d.hi = rw(d.hi); d.weight = rw(d.weight); }
                    x.distDefault = rw(it.distDefault);
                    for (auto& fp : x.fns) {   // function arguments: loop indices, rand array elements
                        auto q = std::make_shared<Prog>(*fp);
                        for (Instr& ins : q->code) {
                            ins.e = rw(ins.e);
                            for (E& y : ins.args) y = rw(y);
                            for (auto& in : ins.init) in.second = rw(in.second);
                            ins.self = rw(ins.self);
                        }
                        fp = std::move(q);
                    }
                    if (rw.dropped) continue;
                    owned.push_back(std::move(x));
                    use = &owned.back();
                }
                Item k;
                k.it = use; k.fr = fr;
                k.prio = it.soft || it.disableSoft ? ++prio : 0;
                if (it.disableSoft) k.on = false;   // not a constraint itself
                ctx.items.push_back(k);
            }
        };
        for (auto& sr : srcs) addItems(*sr.items, sr.fr);
        if (pass == 1) {
            if (sizeVars.empty()) { g_ctx = savedCtx; g_fr = callerFr; return true; }   // no size is constrained
            for (int ci : sizeVars) addVar(conts[(size_t)ci].sizeFr->base, kSizeW, false, false, conts[(size_t)ci].owner);
        }
        // Which variables each constraint reads.
        for (size_t k = 0; k < ctx.items.size(); k++) {
            Item& it = ctx.items[k];
            g_fr = it.fr;
            std::vector<int> a;
            collectAddrs(it.it->e, a);
            if (it.it->guard) collectAddrs(it.it->guard, a);
            for (const RandDist& d : it.it->dist) {
                collectAddrs(d.lo, a);
                if (d.hi) collectAddrs(d.hi, a);
            }
            // What its function calls read: their arguments (18.5.12).
            std::vector<int> fa;
            for (const auto& p : it.it->fns)
                for (const Instr& ins : p->code) {
                    if (ins.e) fnAddrs(ins.e, fa);
                    for (const E& x : ins.args) fnAddrs(x, fa);
                    for (const auto& in : ins.init) fnAddrs(in.second, fa);
                    fnAddrs(ins.self, fa);
                }
            for (int s : fa)
                if (auto f = ctx.idx.find(s); f != ctx.idx.end()) it.fnVars.push_back(f->second);
            std::sort(it.fnVars.begin(), it.fnVars.end());
            it.fnVars.erase(std::unique(it.fnVars.begin(), it.fnVars.end()), it.fnVars.end());
            a.insert(a.end(), fa.begin(), fa.end());
            std::sort(a.begin(), a.end());
            a.erase(std::unique(a.begin(), a.end()), a.end());
            for (int s : a)
                if (auto f = ctx.idx.find(s); f != ctx.idx.end()) {
                    it.vars.push_back(f->second);
                    ctx.vars[(size_t)f->second].items.push_back((int)k);
                }
            const Expr* de = it.it->e.get();
            while (de && de->op == Op::Resize && !de->kids.empty() && de->kids[0]->op != Op::Const) de = de->kids[0].get();
            if (it.it->isDist && !it.it->guard && de && de->op == Op::Ref && it.vars.size() == 1 && !it.it->soft) {
                Var& v = ctx.vars[(size_t)it.vars[0]];
                if (!v.dist) { v.dist = it.it; v.distFr = it.fr; }
            }
        }
        // disable soft x: soft constraints on x of lower priority are dropped.
        for (auto& d : ctx.items) {
            if (!d.it->disableSoft) continue;
            for (auto& sft : ctx.items) {
                if (!sft.it->soft || sft.prio >= d.prio) continue;
                for (int a : sft.vars)
                    if (std::find(d.vars.begin(), d.vars.end(), a) != d.vars.end()) { sft.on = false; break; }
            }
        }
        // Order: solve-before (18.5.9), randc, then the rest.
        std::vector<int> order, rank(ctx.vars.size(), 2);
        for (size_t k = 0; k < ctx.vars.size(); k++)
            if (ctx.vars[k].randc) rank[k] = 1;
        for (auto& [bl, fr] : befores) {
            g_fr = fr;
            for (auto& [first, after] : *bl) {
                for (const E& e : first) {
                    std::vector<int> a;
                    collectAddrs(e, a);
                    for (int s : a) if (auto f = ctx.idx.find(s); f != ctx.idx.end()) rank[(size_t)f->second] = 0;
                }
            }
        }
        // Function arguments before the other variables of their constraint (18.5.12).
        for (auto& it : ctx.items)
            for (int v : it.fnVars) rank[(size_t)v] = std::min(rank[(size_t)v], 0);
        for (size_t k = 0; k < ctx.vars.size(); k++)
            if (rank[k] == 0) ctx.vars[k].ordered = true;
        if (pass == 1)   // sizes before everything else (18.4)
            for (int ci : sizeVars) rank[(size_t)ctx.idx.at(conts[(size_t)ci].sizeFr->base)] = -1;
        for (int r = -1; r <= 2; r++)
            for (size_t k = 0; k < ctx.vars.size(); k++)
                if (rank[k] == r) order.push_back((int)k);
        g_fr = callerFr;

        bool ok = false;
        std::vector<int> softs;
        for (size_t k = 0; k < ctx.items.size(); k++)
            if (ctx.items[k].it->soft && ctx.items[k].on) softs.push_back((int)k);
        std::sort(softs.begin(), softs.end(), [&](int a, int b) { return ctx.items[(size_t)a].prio < ctx.items[(size_t)b].prio; });
        auto attempt = [&]() {
            for (int a = 0; a < 16; a++) {
                ctx.budget = 20000 + 2000 * (long)ctx.vars.size();
                auto saveRandc = randcLeft();
                if (solveOnce(order)) return true;
                randcLeft() = saveRandc;
            }
            return false;
        };
        ok = attempt();
        if (!ok && !softs.empty()) {
            // Keep soft constraints from the highest priority down, each only if
            // it still leaves a solution (18.5.13.1).
            for (int k : softs) ctx.items[(size_t)k].on = false;
            ok = attempt();
            if (ok) {
                std::vector<i128> best;
                for (auto& v : ctx.vars) best.push_back(v.val);
                for (auto it = softs.rbegin(); it != softs.rend(); ++it) {
                    ctx.items[(size_t)*it].on = true;
                    if (attempt()) { best.clear(); for (auto& v : ctx.vars) best.push_back(v.val); }
                    else ctx.items[(size_t)*it].on = false;
                }
                for (size_t k = 0; k < ctx.vars.size(); k++) ctx.vars[k].val = best[k];
            }
        }
        g_fr = callerFr;
        if (ok) {
            for (auto& v : ctx.vars) {
                if (pass == 1 && !ctx.idx.count(v.addr)) continue;
                uint64_t bits = (uint64_t)v.val;
                Val nv = make(v.w >= 64 ? bits : bits & ((1ULL << v.w) - 1), 0, v.w, 0, false);
                if (pass == 1) {   // only the sizes are kept from this pass
                    bool isSize = false;
                    for (int ci : sizeVars) if (conts[(size_t)ci].sizeFr->base == v.addr) isSize = true;
                    if (!isSize) continue;
                }
                S.set(v.addr, nv);
            }
        }
        g_ctx = savedCtx;
        return ok;
    };

    bool ok;
    if (conts.empty()) {
        std::vector<int> none;
        ok = solvePass(0, none);
    } else {
        // Sizes are solved before elements (18.5.8.1), so a size may leave
        // the elements no solution: then the sizes are picked again.
        const std::vector<RCont> conts0 = conts;
        const auto contIdx0 = contIdx;
        std::map<int, DynObj> saved;   // containers resized on the way, restored on failure
        for (int attempt = 0; attempt < 8; attempt++) {
            if (attempt) { conts = conts0; contIdx = contIdx0; }
            ok = true;
            std::vector<int> sizeVars;
            // Containers of containers (int m[][]): the outer sizes are solved
            // first, the container resized, and its elements take its place for
            // the next level, down to the containers that hold values.
            for (;;) {
                bool nested = false;
                for (auto& c : conts) nested |= c.depth > 1;
                if (!nested) break;
                sizeVars.clear();
                ok = solvePass(1, sizeVars);
                if (!ok) break;
                for (int ci : sizeVars) conts[(size_t)ci].n = (int64_t)S.get(conts[(size_t)ci].sizeFr->base).v;
                std::vector<RCont> next;
                for (auto& c : conts) {
                    c.sizeFr = nullptr;
                    c.sized = true;
                    if (c.depth == 1) { next.push_back(std::move(c)); continue; }
                    auto& d = dynStore()[c.cid];
                    if (!c.assoc) {
                        if (!saved.count(c.cid)) saved[c.cid] = d;
                        size_t old = d.vec.size();
                        d.vec.resize((size_t)c.n);
                        for (size_t k = old; k < d.vec.size(); k++) d.vec[k] = dynNewElem(d);
                        dynBump(c.cid);
                    }
                    std::vector<Val*> els;
                    for (auto& v : d.vec) els.push_back(&v);
                    for (auto& kv : d.map) els.push_back(&kv.second);
                    for (Val* el : els) {
                        if (el->x || !el->v) *el = dynNewElem(d);
                        RCont k;
                        k.cid = (int)el->v; k.w = c.w; k.sgn = c.sgn; k.owner = c.owner; k.depth = c.depth - 1;
                        const DynObj& kd = dynStore()[k.cid];
                        k.n = (int64_t)kd.size();
                        if (kd.kind == 2) {
                            k.assoc = true;
                            for (auto& kv : kd.map) k.keys.push_back(kv.first);
                        }
                        next.push_back(std::move(k));
                    }
                }
                conts = std::move(next);
                contIdx.clear();
                for (size_t k = 0; k < conts.size(); k++) contIdx[conts[k].cid] = (int)k;
            }
            if (ok) { sizeVars.clear(); ok = solvePass(1, sizeVars); }
            if (ok) {
                for (int ci : sizeVars) conts[(size_t)ci].n = (int64_t)S.get(conts[(size_t)ci].sizeFr->base).v;
                for (auto& c : conts) {
                    c.elems = newFrame(scratchLayout(c.w, c.sgn, c.n), nullptr, 0);
                    const DynObj& dd = dynStore()[c.cid];
                    if (c.assoc) {
                        for (int64_t k = 0; k < c.n; k++) S.set(c.elems->base + (int)k, dd.map.at(c.keys[(size_t)k]));
                    } else
                        for (int64_t k = 0; k < c.n && k < (int64_t)dd.vec.size(); k++) S.set(c.elems->base + (int)k, dd.vec[(size_t)k]);
                }
                ok = solvePass(2, sizeVars);
            }
            if (ok) break;
            bool sizesVary = !sizeVars.empty();
            for (auto& c : conts0) sizesVary |= c.depth > 1;
            if (!sizesVary) break;
        }
        if (!ok)
            for (auto& [cid, o] : saved) { dynStore()[cid] = o; dynBump(cid); }
        if (ok)
            for (auto& c : conts) {
                auto& d = dynStore()[c.cid];
                if (!c.assoc) d.vec.assign((size_t)c.n, d.dflt);
                for (int64_t k = 0; k < c.n; k++) {
                    Val v = S.get(c.elems->base + (int)k);
                    v.sgn = d.dflt.sgn;
                    if (c.assoc) d.map[c.keys[(size_t)k]] = v;
                    else d.vec[(size_t)k] = v;
                }
                dynBump(c.cid);
            }
    }
    if (!ok) cerr << "warning: randomize() failed: the constraints cannot all be met (time " << S.now << ")\n";
    g_fr = callerFr;
    g_rng = savedRng;
    if (ok)
        for (int o : objs) {
            const auto& sp = classRt()[(size_t)S.sigs[o].val.v].rand;
            if (sp && sp->post) runSync(sp->post, sp->postLay, o);
        }
    return ok ? 1 : 0;
}

// obj.rand_mode(on) / obj.constraint_mode(on) on every variable or block (18.8, 18.9).
static void randModeAll(int obj, bool blocks, bool on) {
    const auto& sp = classRt()[(size_t)S.sigs[obj].val.v].rand;
    if (!sp) return;
    if (blocks) {
        for (const auto& b : sp->blocks) if (b->mode >= 0) S.set(b->staticMode ? b->mode : obj + b->mode, mk(on));
    } else {
        for (const RandVar& rv : sp->vars) if (rv.mode >= 0) S.set(obj + rv.mode, mk(on));
    }
}
