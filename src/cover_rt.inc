// cover_rt.inc - the covergroup runtime (IEEE 1800-2023 clause 19),
// included at the end of exec.inc. Instances are built by `new`, sampled on
// their event or by sample(), and report coverage as in 19.11.

using i128 = __int128;
namespace cg {
struct KR { uint64_t a, b; };   // keys a..b, inclusive

inline i128 tmin(const CgCpDef& c) { return c.sgn ? -((i128)1 << (c.w - 1)) : 0; }
inline i128 tmax(const CgCpDef& c) { return c.sgn ? ((i128)1 << (c.w - 1)) - 1 : ((i128)1 << c.w) - 1; }
inline uint64_t kmask(int w) { return w >= 64 ? ~0ull : (1ull << w) - 1; }
// A real's key: its bits, ordered like the reals (-0.0 is 0.0).
inline uint64_t rkey(double d) {
    if (d == 0) d = 0;
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return (b >> 63) ? ~b : b | (1ull << 63);
}
inline double rval(uint64_t k) {
    uint64_t b = (k >> 63) ? k & ~(1ull << 63) : ~k;
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
inline double realOfVal(const Val& v) {
    double d;
    std::memcpy(&d, &v.v, 8);
    return d;
}
// A real as in bin names: 1.0, 0.75 (19.5.1).
inline std::string realName(double d) {
    char buf[40];
    snprintf(buf, sizeof buf, "%.15g", d);
    std::string s = buf;
    if (s.find_first_of(".eni") == std::string::npos) s += ".0";
    return s;
}
// A coverpoint wider than 64 bits (CgCpDef::wide): the bin bounds of the
// instance being built or sampled, and a value's place among them.
inline const std::vector<Val>* g_cgWide = nullptr;
inline bool wideLt(const CgCpDef& c, Val a, Val b) {
    a.sgn = b.sgn = c.sgn;
    return v_lt(a, b).v & 1;
}
inline uint64_t wideKey(const CgCpDef& c, const Val& v) {
    if (!g_cgWide) return 0;
    const auto& wb = *g_cgWide;
    size_t lo = 0, hi = wb.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (wideLt(c, wb[mid], v)) lo = mid + 1;
        else hi = mid;
    }
    return lo < wb.size() && !wideLt(c, v, wb[lo]) ? 2 * lo + 1 : 2 * lo;
}
inline bool unknownAny(const Val& v) { return v.x || anyX(v); }
inline std::string wideDec(const CgCpDef& c, const Val& v) {   // a wide value in a report, in decimal
    std::vector<uint64_t> l((size_t)(c.w + 63) / 64, 0);
    for (size_t i = 0; i < l.size(); i++) l[i] = i == 0 ? v.v : i - 1 < v.hi.size() ? v.hi[i - 1].v : 0;
    if (c.w % 64) l.back() &= (1ull << (c.w % 64)) - 1;
    bool neg = c.sgn && (l.back() >> ((c.w - 1) % 64) & 1);
    if (neg) {   // two's complement on w bits
        unsigned __int128 carry = 1;
        for (auto& x : l) { carry += (unsigned __int128)~x; x = (uint64_t)carry; carry >>= 64; }
        if (c.w % 64) l.back() &= (1ull << (c.w % 64)) - 1;
    }
    std::string s;
    auto zero = [&] { for (auto x : l) if (x) return false; return true; };
    while (!zero()) {
        unsigned __int128 r = 0;
        for (size_t i = l.size(); i-- > 0;) {
            unsigned __int128 cur = (r << 64) | l[i];
            l[i] = (uint64_t)(cur / 10);
            r = cur % 10;
        }
        s.insert(s.begin(), char('0' + (int)r));
    }
    if (s.empty()) s = "0";
    return neg ? "-" + s : s;
}
// A sampled value's key: its w bits, the sign bit flipped when signed.
inline uint64_t keyOf(const CgCpDef& c, const Val& v) {
    if (c.isReal) return rkey(realOfVal(v));
    if (c.wide) return wideKey(c, v);
    uint64_t x = v.v & kmask(c.w);
    return c.sgn ? x ^ (1ull << (c.w - 1)) : x;
}
inline i128 valOfKey(const CgCpDef& c, uint64_t k) { return (i128)k + tmin(c); }
inline std::string str128(i128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    unsigned __int128 u = neg ? (unsigned __int128)(-v) : (unsigned __int128)v;
    std::string s;
    while (u) { s.insert(s.begin(), char('0' + (int)(u % 10))); u /= 10; }
    return neg ? "-" + s : s;
}
inline std::string keyName(const CgCpDef& c, uint64_t k) { return c.isReal ? realName(rval(k)) : str128(valOfKey(c, k)); }
inline i128 boundOf(const Val& v, bool sgn) { return sgn ? (i128)(int64_t)v.v : (i128)(uint64_t)v.v; }
inline bool inRanges(const std::vector<KR>& rs, uint64_t k) {
    for (auto& r : rs) if (k >= r.a && k <= r.b) return true;
    return false;
}
inline bool inPairs(const std::vector<std::pair<uint64_t, uint64_t>>& rs, uint64_t k) {
    for (auto& r : rs) if (k >= r.first && k <= r.second) return true;
    return false;
}

// One covergroup_range_list element as keys, in order (19.5.7: clipped to
// the coverpoint type; a value outside it is dropped). Values with x/z go to
// `xv` (matched with ===) or, for a wildcard bin, to `wild` as (care, value).
// A real item's bounds: lo, hi, and whether either is $ (19.5.1).
struct RealItem { double lo, hi; bool unbounded; bool range; };
inline RealItem realItem(const CgItem& it) {
    RealItem r{0, 0, false, it.range};
    if (!it.range) { r.lo = r.hi = realOfVal(eval(it.lo, rd)); return r; }
    if (it.tol) {   // [c +/- t], [c +%- p] (11.4.13)
        double ctr = realOfVal(eval(it.lo, rd)), t = realOfVal(eval(it.hi, rd));
        if (it.tol == 2) t = std::fabs(ctr) * t / 100.0;
        t = std::fabs(t);
        r.lo = ctr - t;
        r.hi = ctr + t;
        return r;
    }
    r.lo = it.loMin ? -INFINITY : realOfVal(eval(it.lo, rd));
    r.hi = it.hiMax ? INFINITY : realOfVal(eval(it.hi, rd));
    r.unbounded = it.loMin || it.hiMax;
    if (r.lo > r.hi) std::swap(r.lo, r.hi);
    return r;
}
inline void itemKeys(const CgCpDef& c, const CgItem& it, bool wildcard, std::vector<KR>& out,
                     std::vector<Val>* xv, std::vector<std::pair<uint64_t, uint64_t>>* wild) {
    if (c.isReal) {
        RealItem r = realItem(it);
        out.push_back({r.lo == -INFINITY ? 0 : rkey(r.lo), r.hi == INFINITY ? ~0ull : rkey(r.hi)});
        return;
    }
    if (c.wide) {   // keys among the bounds (keyOf)
        uint64_t top = 2 * (g_cgWide ? g_cgWide->size() : 0);
        if (!it.range) {
            Val v = eval(it.lo, rd);
            if (unknownAny(v)) { if (xv) xv->push_back(v); return; }
            uint64_t k = keyOf(c, v);
            out.push_back({k, k});
            return;
        }
        uint64_t a = 0, b = top;
        if (!it.loMin) { Val v = eval(it.lo, rd); if (unknownAny(v)) return; a = keyOf(c, v); }
        if (!it.hiMax) { Val v = eval(it.hi, rd); if (unknownAny(v)) return; b = keyOf(c, v); }
        if (a > b) std::swap(a, b);
        out.push_back({a, b});
        return;
    }
    i128 lo = tmin(c), hi = tmax(c);
    if (!it.range) {
        Val v = eval(it.lo, rd);
        uint64_t xm = v.x & kmask(c.w);
        if (xm) {
            if (wildcard && wild) {
                uint64_t care = ~xm & kmask(c.w), val = v.v & care;
                if (c.sgn) { /* keys flip the sign bit: so does the value when it is cared about */
                    uint64_t sb = 1ull << (c.w - 1);
                    if (care & sb) val ^= sb;
                }
                wild->push_back({care, val});
            } else if (xv) {
                Val x = v;
                x.w = c.w;
                xv->push_back(x);
            }
            return;
        }
        i128 b = boundOf(v, it.loSgn);
        if (b < lo || b > hi) return;
        uint64_t k = (uint64_t)(b - lo);
        out.push_back({k, k});
        return;
    }
    i128 a = lo, b = hi;
    if (!it.loMin) { Val v = eval(it.lo, rd); if (v.x & kmask(64)) return; a = boundOf(v, it.loSgn); }
    if (!it.hiMax) { Val v = eval(it.hi, rd); if (v.x & kmask(64)) return; b = boundOf(v, it.hiSgn); }
    if (a > b) std::swap(a, b);   // [hi:lo] is the same range (11.4.13)
    if (a < lo) a = lo;
    if (b > hi) b = hi;
    if (a > b) return;
    out.push_back({(uint64_t)(a - lo), (uint64_t)(b - lo)});
}
inline i128 rangesSize(const std::vector<KR>& rs) {
    i128 n = 0;
    for (auto& r : rs) n += (i128)r.b - r.a + 1;
    return n;
}
// Positions [from, to) of the value sequence `rs`, as key ranges.
inline std::vector<KR> slice(const std::vector<KR>& rs, i128 from, i128 to) {
    std::vector<KR> out;
    i128 p = 0;
    for (auto& r : rs) {
        i128 n = (i128)r.b - r.a + 1;
        i128 s = std::max(from, p), e = std::min(to, p + n);
        if (s < e) out.push_back({(uint64_t)(r.a + (s - p)), (uint64_t)(r.a + (e - p) - 1)});
        p += n;
        if (p >= to) break;
    }
    return out;
}
// Keys of rs not in any of `minus`.
inline bool allCovered(const std::vector<KR>& rs, const std::vector<KR>& minus) {
    for (auto r : rs) {
        uint64_t a = r.a;
        for (;;) {
            bool moved = false;
            for (auto& m : minus)
                if (a >= m.a && a <= m.b) {
                    if (m.b >= r.b) goto next;
                    a = m.b + 1;
                    moved = true;
                }
            if (!moved) return false;
        }
    next:;
    }
    return true;
}
inline int64_t optOr(const std::vector<CgOptSet>& opts, int which, int64_t dflt) {
    for (auto& o : opts)
        if (o.which == which) {
            Val v = eval(o.e, rd);
            dflt = (int64_t)v.v;
        }
    return dflt;
}
// A class's covergroup reads its object's properties: `this` is the object
// it was created for, whatever process samples it.
inline void (*g_cgInstStart)(int id) = nullptr;   // a class covergroup's own sampling process
inline void (*g_cgRun)(const Prog* p, const CgInst& in) = nullptr;   // run a coverpoint's code to its end
struct SelfScope {
    Frame* saved;
    explicit SelfScope(const CgInst& in) : saved(g_fr) { if (in.self) g_fr = in.self.get(); }
    ~SelfScope() { g_fr = saved; }
};
// A coverpoint's function calls (tr.ones() > 0), in the instance's scope,
// run to their end; a function cannot wait.
inline void runPre(const Prog& p) {
    Frame* sf = g_fr;
    Frame* sr = g_ret;
    FrameP fp(g_fr, [](Frame*) {});
    auto ctl = std::make_shared<ProcCtl>();
    Task t = exec(&p, ctl, fp);
    t.h.resume();
    if (!t.h.done()) {
        std::cerr << "error: a function called in a coverpoint waited (time " << S.now << ")\n";
        std::exit(1);
    }
    t.h.destroy();
    g_fr = sf;
    g_ret = sr;
}
inline void setArgs(const CgInst& in) {
    const CgDef& d = cgDefs()[in.def];
    for (size_t k = 0; k < d.argSigs.size(); k++)
        S.set(d.argSigs[k], d.argRef[k] ? S.get(in.refSigs[k]) : in.args[k]);
}
constexpr size_t kMaxBins = 1u << 20;
[[noreturn]] inline void fail(const std::string& m) { throw std::runtime_error("msim: covergroup: " + m); }

// The transition bins of `bd` (19.5.2): each sequence of steps is a
// sequence term over atoms (a step's value set); a bin array gets one bin
// per sequence of single values.
inline void transBins(const CgCpDef& c, const CgBinDef& bd, CgCpInst& ci) {
    auto& st = sva::store();
    auto stepTerm = [&](const CgStep& s, const std::vector<KR>& keys) {
        std::vector<std::pair<uint64_t, uint64_t>> set;
        for (auto& r : keys) set.push_back({r.a, r.b});
        int atom = (int)ci.atoms.size();
        ci.atoms.push_back(std::move(set));
        if (s.rep == 0) return st.boolean(atom);
        int lo = (int)eval(s.from, rd).v, hi = s.to ? (int)eval(s.to, rd).v : lo;
        if (s.rep == 1) return st.rep(st.boolean(atom), lo, hi);
        if (s.rep == 2) return st.gotoRep(atom, lo, hi);
        return st.nonconsec(atom, lo, hi);
    };
    auto keysOf = [&](const CgStep& s) {
        std::vector<KR> ks;
        std::vector<std::pair<uint64_t, uint64_t>> wild;
        for (auto& it : s.items) itemKeys(c, it, bd.wildcard, ks, nullptr, &wild);
        for (auto& [care, val] : wild) {   // a wildcard step: every value it matches
            for (uint64_t k = 0; k <= kmask(c.w); k++) {
                if ((k & care) == val) ks.push_back({k, k});
                if (ks.size() > kMaxBins) fail("too many values in a wildcard transition");
                if (k == kmask(c.w)) break;
            }
        }
        return ks;
    };
    auto add = [&](std::string name, const sva::Node* r) {
        CgBin b;
        b.name = std::move(name);
        b.base = bd.name;
        b.kind = bd.kind;
        b.iff = bd.iff;
        b.trans = st.covseq(r);
        ci.bins.push_back(std::move(b));
    };
    if (!bd.array) {
        std::vector<const sva::Node*> alts;
        for (auto& seq : bd.trans) {
            const sva::Node* r = nullptr;
            for (auto& s : seq) {
                const sva::Node* t = stepTerm(s, keysOf(s));
                r = r ? st.delay(r, 1, 1, t) : t;
            }
            alts.push_back(r);
        }
        add(bd.name, st.sor(alts));
        return;
    }
    // name[]: one bin per sequence of single values; a repeated step stays whole.
    for (auto& seq : bd.trans) {
        struct Part { std::string text; const CgStep* s; std::vector<KR> keys; };
        std::vector<std::vector<Part>> choices;
        for (auto& s : seq) {
            std::vector<KR> ks = keysOf(s);
            std::vector<Part> ps;
            if (s.rep != 0) {
                std::string t;
                for (auto& r : ks) t += (t.empty() ? "" : ",") + (r.a == r.b ? keyName(c, r.a) : keyName(c, r.a) + ":" + keyName(c, r.b));
                std::string rk = s.rep == 1 ? "*" : s.rep == 2 ? "->" : "=";
                Val f = eval(s.from, rd);
                t += "[" + rk + std::to_string(f.v) + (s.to ? ":" + std::to_string(eval(s.to, rd).v) : "") + "]";
                ps.push_back({t, &s, ks});
            } else
                for (auto& r : ks)
                    for (uint64_t k = r.a;; k++) {
                        ps.push_back({keyName(c, k), &s, {{k, k}}});
                        if (ps.size() > kMaxBins) fail("too many transition bins");
                        if (k == r.b) break;
                    }
            choices.push_back(std::move(ps));
        }
        std::vector<size_t> idx(choices.size(), 0);
        for (;;) {
            std::string name;
            const sva::Node* r = nullptr;
            for (size_t j = 0; j < choices.size(); j++) {
                if (choices[j].empty()) goto none;
                const Part& p = choices[j][idx[j]];
                name += (j ? "=>" : "") + p.text;
                const sva::Node* t = stepTerm(*p.s, p.keys);
                r = r ? st.delay(r, 1, 1, t) : t;
            }
            add(bd.name + "[" + name + "]", r);
            if (ci.bins.size() > kMaxBins) fail("too many transition bins");
            {
                size_t j = choices.size();
                while (j-- > 0) {
                    if (++idx[j] < choices[j].size()) break;
                    idx[j] = 0;
                }
                if (j == (size_t)-1) break;
            }
        }
    none:;
    }
}

// A coverpoint's bins for one instance (19.5).
// The bins of one bins construct of a real coverpoint (19.5.1): a single bin;
// name[], a bin per value and per interval of each range (identical ones
// merged); or name[N], the values and intervals spread over N bins.
inline void realBins(const CgCpDef& c, const CgBinDef& bd, CgCpInst& ci, double iv, std::vector<KR>& excl) {
    struct Piece { double lo, hi; bool hiIn; bool single; };
    std::vector<Piece> ps;
    for (const auto& it : bd.items) {
        RealItem r = realItem(it);
        if (!r.range) { ps.push_back({r.lo, r.lo, true, true}); continue; }
        double w = r.hi - r.lo;
        if (r.unbounded || !bd.array || !(iv > 0) || w <= iv) { ps.push_back({r.lo, r.hi, true, false}); continue; }
        double nf = std::ceil(w / iv - 1e-9);
        if (nf > (double)kMaxBins) fail("too many intervals in " + bd.name);
        int64_t n = (int64_t)nf;
        for (int64_t k = 0; k < n; k++) {
            double a = r.lo + (double)k * iv;
            if (k == n - 1) ps.push_back({a, r.hi, true, false});
            else ps.push_back({a, r.lo + (double)(k + 1) * iv, false, false});
        }
    }
    auto keys = [](const Piece& p) -> KR {
        uint64_t a = p.lo == -INFINITY ? 0 : rkey(p.lo);
        uint64_t b = p.hi == INFINITY ? ~0ull : rkey(p.hi);
        if (!p.hiIn && b > a) b--;   // [lo:hi): up to the real just below hi
        return {a, b};
    };
    auto name = [](const Piece& p) {
        if (p.single) return realName(p.lo);
        auto bound = [](double d) { return d == INFINITY || d == -INFINITY ? std::string("$") : realName(d); };
        return bound(p.lo) + ":" + bound(p.hi) + (p.hiIn ? "]" : ")");
    };
    if (bd.kind == 1 || bd.kind == 2)
        for (auto& p : ps) excl.push_back(keys(p));
    auto mkBin = [&](std::string nm, const std::vector<Piece>& in) {
        CgBin b;
        b.name = std::move(nm);
        b.base = bd.name;
        b.kind = bd.kind;
        b.iff = bd.iff;
        for (auto& p : in) { KR k = keys(p); b.ranges.push_back({k.a, k.b}); }
        ci.bins.push_back(std::move(b));
    };
    if (!bd.array) { mkBin(bd.name, ps); return; }
    if (!bd.count) {
        std::vector<Piece> seen;
        for (auto& p : ps) {
            bool dup = false;
            for (auto& q : seen) dup = dup || (q.lo == p.lo && q.hi == p.hi && q.hiIn == p.hiIn && q.single == p.single);
            if (dup) continue;
            seen.push_back(p);
            mkBin(bd.name + "[" + name(p) + "]", {p});
        }
        return;
    }
    Val nv = eval(bd.count, rd);
    int64_t n = (int64_t)nv.v;
    if (n <= 0 || (size_t)n > kMaxBins) fail("bad number of bins for " + bd.name);
    // Values after the intervals, in order (19.5.1: the items are the intervals, then the values).
    std::vector<Piece> items;
    for (auto& p : ps) if (!p.single) items.push_back(p);
    for (auto& p : ps) if (p.single) items.push_back(p);
    int64_t total = (int64_t)items.size(), per = std::max<int64_t>(1, total / n);
    for (int64_t k = 0; k < n; k++) {
        int64_t from = per * k, to = k == n - 1 ? total : std::min(total, per * (k + 1));
        std::vector<Piece> in;
        for (int64_t j = from; j < to; j++) in.push_back(items[j]);
        mkBin(bd.name + "[" + std::to_string(k) + "]", in);
    }
}
// The bins of name[] or name[N] (19.5.1) of a coverpoint wider than 64
// bits: for each bin its value runs [lo, hi]. The values are taken in the
// order written, as offsets from the lowest value (w + 2 bits, so a sum of
// sizes cannot wrap); name[] makes one bin per value, name[N] N bins of
// total / N values, the last taking the rest.
inline std::vector<std::vector<std::pair<Val, Val>>> wideArrayParts(const CgCpDef& c, const CgBinDef& bd) {
    int W = c.w + 2;
    auto off = [&](Val v) {   // a value as an offset from the lowest value
        v = v_resize(v, c.w, false);
        if (c.sgn) v = v_insert(v, mk(~v_slice(v, c.w - 1, c.w - 1).v & 1, 1), c.w - 1, c.w - 1);
        v.sgn = false;
        return v_resize(v, W, false);
    };
    auto val = [&](const Val& o) {   // and back
        Val v = v_resize(o, c.w, false);
        if (c.sgn) v = v_insert(v, mk(~v_slice(v, c.w - 1, c.w - 1).v & 1, 1), c.w - 1, c.w - 1);
        return v;
    };
    auto lt = [](const Val& a, const Val& b) { return (v_lt(a, b).v & 1) != 0; };
    std::vector<std::pair<Val, Val>> runs;   // offsets, as written
    for (const auto& it : bd.items) {
        Val lo, hi;
        if (!it.range) {
            Val v = eval(it.lo, rd);
            if (unknownAny(v)) continue;
            lo = hi = off(v);
        } else {
            lo = it.loMin ? mk(0, W) : off(eval(it.lo, rd));
            hi = it.hiMax ? v_sub(v_shl(mk(1, W), mk((uint64_t)c.w, 32)), mk(1, W)) : off(eval(it.hi, rd));
            if (unknownAny(lo) || unknownAny(hi)) continue;
            if (lt(hi, lo)) std::swap(lo, hi);
        }
        runs.push_back({lo, hi});
    }
    std::vector<std::vector<std::pair<Val, Val>>> parts;
    if (!bd.count) {   // name[]: each distinct value
        std::vector<Val> vals;
        for (auto& [lo, hi] : runs)
            for (Val v = lo;; v = v_add(v, mk(1, W))) {
                vals.push_back(v);
                if (vals.size() > kMaxBins) fail("too many bins in " + bd.name + "[]");
                if (!lt(v, hi)) break;
            }
        std::sort(vals.begin(), vals.end(), lt);
        vals.erase(std::unique(vals.begin(), vals.end(), [&](const Val& a, const Val& b) { return !lt(a, b) && !lt(b, a); }), vals.end());
        for (auto& v : vals) parts.push_back({{val(v), val(v)}});
        return parts;
    }
    Val nv = eval(bd.count, rd);
    int64_t n = (int64_t)nv.v;
    if (nv.x || n <= 0 || (size_t)n > kMaxBins) fail("bad number of bins for " + bd.name);
    Val total = mk(0, W);
    for (auto& [lo, hi] : runs) total = v_add(total, v_add(v_sub(hi, lo), mk(1, W)));
    Val per = v_div(total, mk((uint64_t)n, W));
    if (!lt(mk(0, W), per)) per = mk(1, W);
    // The values from global offset a up to (not including) b, as runs.
    auto slice = [&](Val a, Val b) {
        std::vector<std::pair<Val, Val>> out;
        Val base = mk(0, W);
        for (auto& [lo, hi] : runs) {
            Val sz = v_add(v_sub(hi, lo), mk(1, W)), end = v_add(base, sz);
            Val s = lt(base, a) ? a : base, e = lt(b, end) ? b : end;   // [s, e) of this run
            if (lt(s, e)) out.push_back({val(v_add(lo, v_sub(s, base))), val(v_add(lo, v_sub(v_sub(e, base), mk(1, W))))});
            base = end;
        }
        return out;
    };
    for (int64_t k = 0; k < n; k++) {
        Val a = v_mul(per, mk((uint64_t)k, W));
        Val b = k == n - 1 ? total : v_mul(per, mk((uint64_t)k + 1, W));
        if (lt(total, b)) b = total;
        parts.push_back(lt(a, b) ? slice(a, b) : std::vector<std::pair<Val, Val>>{});
    }
    return parts;
}
inline void buildCp(const CgCpDef& c, CgCpInst& ci, int64_t autoBinMax, double realInterval = 1.0) {
    std::vector<KR> excl;   // ignored and illegal values
    bool explicitBins = false;
    std::vector<std::pair<Val, Val>> wideAuto;   // a wide coverpoint's automatic bins, [lo, hi]
    std::unordered_map<size_t, std::vector<std::vector<std::pair<Val, Val>>>> wideParts;   // array bins of a wide coverpoint
    if (c.wide) {   // the bounds of every value bin, in order (keyOf)
        if (std::none_of(c.bins.begin(), c.bins.end(), [](const CgBinDef& b) { return b.kind == 0 || b.dflt; })) {
            // Automatic bins (19.5.3): the 2^w values in autoBinMax equal parts,
            // the last taking the rest, counted from the lowest value.
            int W = c.w + 2;
            int64_t n = std::max<int64_t>(autoBinMax, 1);
            Val space = v_shl(mk(1, W), mk((uint64_t)c.w, 32));
            Val per = v_div(space, mk((uint64_t)n, W));
            auto asVal = [&](const Val& off) {   // an offset from the lowest value, as a value
                Val v = v_resize(off, c.w, false);
                if (c.sgn) v = v_insert(v, mk(~v_slice(v, c.w - 1, c.w - 1).v & 1, 1), c.w - 1, c.w - 1);
                return v;
            };
            for (int64_t k = 0; k < n; k++) {
                Val a = v_mul(per, mk((uint64_t)k, W));
                Val b = k == n - 1 ? v_sub(space, mk(1, W)) : v_sub(v_mul(per, mk((uint64_t)k + 1, W)), mk(1, W));
                wideAuto.push_back({asVal(a), asVal(b)});
                ci.wb.push_back(wideAuto.back().first);
                ci.wb.push_back(wideAuto.back().second);
            }
        }
        auto bounds = [&](const CgItem& it) {
            for (const E& e : {it.range && it.loMin ? E() : it.lo, it.range && !it.hiMax ? it.hi : E()})
                if (e) {
                    Val v = eval(e, rd);
                    if (!unknownAny(v)) ci.wb.push_back(v);
                }
        };
        for (const auto& bd : c.bins) {
            for (const auto& it : bd.items) bounds(it);
            for (const auto& seq : bd.trans)   // transition steps (19.5.2)
                for (const auto& s : seq)
                    for (const auto& it : s.items) bounds(it);
        }
        // name[] and name[N] (19.5.1): the values that start or end a bin
        // become bounds too, so each bin is a run of keys.
        for (size_t bi = 0; bi < c.bins.size(); bi++) {
            const CgBinDef& bd = c.bins[bi];
            if (!bd.array) continue;
            auto parts = wideArrayParts(c, bd);
            for (auto& p : parts)
                for (auto& [a, b] : p) { ci.wb.push_back(a); ci.wb.push_back(b); }
            wideParts[bi] = std::move(parts);
        }
        std::sort(ci.wb.begin(), ci.wb.end(), [&](const Val& a, const Val& b) { return wideLt(c, a, b); });
        ci.wb.erase(std::unique(ci.wb.begin(), ci.wb.end(), [&](const Val& a, const Val& b) {
                        return !wideLt(c, a, b) && !wideLt(c, b, a); }), ci.wb.end());
    }
    g_cgWide = c.wide ? &ci.wb : nullptr;
    for (const auto& bd : c.bins) {
        if (!bd.trans.empty()) {
            transBins(c, bd, ci);
            if (bd.kind == 0) explicitBins = true;
            continue;
        }
        if (bd.dflt || bd.dfltSeq) {
            CgBin b;
            b.name = b.base = bd.name;
            b.kind = 3;
            b.dseq = bd.dfltSeq;
            b.iff = bd.iff;
            ci.bins.push_back(std::move(b));
            explicitBins = true;
            continue;
        }
        if (c.isReal) {
            realBins(c, bd, ci, realInterval, excl);
            if (bd.kind == 0) explicitBins = true;
            continue;
        }
        std::vector<KR> seq;
        std::vector<Val> xv;
        std::vector<std::pair<uint64_t, uint64_t>> wild;
        if (bd.allValues) seq.push_back({0, kmask(c.w)});
        for (auto& it : bd.items) itemKeys(c, it, bd.wildcard, seq, &xv, &wild);
        if (bd.setIdx >= 0) {   // bins b = q; (19.5.1.2): its values now
            Val n = eval(bd.setSize, rd);
            for (uint64_t k = 0; !n.x && k < n.v; k++) {
                S.set(bd.setIdx, mk(k, 32));
                itemKeys(c, bd.setItem, bd.wildcard, seq, &xv, &wild);
            }
        }
        if (bd.with) {   // keep the values for which the with expression is true (19.5.1.1)
            std::vector<KR> kept;
            size_t n = 0;
            for (auto& r : seq)
                for (uint64_t k = r.a;; k++) {
                    if (++n > (1u << 24)) fail("too many values for a with clause");
                    i128 v = valOfKey(c, k);
                    S.set(bd.itemSig, make((uint64_t)v & kmask(c.w), 0, c.w, 0));
                    if (truth(eval(bd.with, rd)) == 1) {
                        if (!kept.empty() && kept.back().b + 1 == k && kept.back().b != ~0ull) kept.back().b = k;
                        else kept.push_back({k, k});
                    }
                    if (k == r.b) break;
                }
            seq = std::move(kept);
        }
        if (bd.wildcard && bd.array)   // name[] of a wildcard: one value per bin
            for (auto& [care, val] : wild) {
                for (uint64_t k = 0;; k++) {
                    if ((k & care) == val) seq.push_back({k, k});
                    if (seq.size() > kMaxBins) fail("too many values in a wildcard bin array");
                    if (k == kmask(c.w)) break;
                }
            }
        if (bd.kind == 1 || bd.kind == 2)
            for (auto& r : seq) excl.push_back(r);
        if (bd.kind == 0) explicitBins = true;
        auto mkBin = [&](std::string name, std::vector<KR> rs) {
            CgBin b;
            b.name = std::move(name);
            b.base = bd.name;
            b.kind = bd.kind;
            b.iff = bd.iff;
            for (auto& r : rs) b.ranges.push_back({r.a, r.b});
            ci.bins.push_back(std::move(b));
        };
        if (bd.array && c.wide) {   // the parts found with the bounds: each its runs of keys
            const auto& parts = wideParts[(size_t)(&bd - c.bins.data())];
            for (size_t k = 0; k < parts.size(); k++) {
                std::vector<KR> rs;
                for (auto& [a, b] : parts[k]) rs.push_back({keyOf(c, a), keyOf(c, b)});
                mkBin(bd.name + "[" + (bd.count || parts[k].empty() ? std::to_string(k) : wideDec(c, parts[k].front().first)) + "]", rs);
            }
        } else if (!bd.array) {
            mkBin(bd.name, seq);
            if (!bd.wildcard || !bd.array) {
                ci.bins.back().wild = wild;
                ci.bins.back().xvals = xv;
            }
        } else if (!bd.count) {   // name[]: one bin per distinct value
            std::unordered_set<uint64_t> seen;
            for (auto& r : seq)
                for (uint64_t k = r.a;; k++) {
                    if (seen.insert(k).second) mkBin(bd.name + "[" + keyName(c, k) + "]", {{k, k}});
                    if (seen.size() > kMaxBins) fail("too many bins in " + bd.name + "[]");
                    if (k == r.b) break;
                }
        } else {   // name[N]: the values in order, B per bin, the rest in the last (19.5.1)
            Val nv = eval(bd.count, rd);
            int64_t n = (int64_t)nv.v;
            if (n <= 0 || (size_t)n > kMaxBins) fail("bad number of bins for " + bd.name);
            i128 total = rangesSize(seq);
            i128 per = std::max<i128>(1, total / n);
            for (int64_t k = 0; k < n; k++) {
                i128 from = per * k, to = k == n - 1 ? total : std::min(total, per * (k + 1));
                mkBin(bd.name + "[" + std::to_string(k) + "]", from < to ? slice(seq, from, to) : std::vector<KR>{});
            }
        }
    }
    if (!explicitBins) {   // automatic bins (19.5.3)
        ci.autoBins = true;
        auto add = [&](std::string name, uint64_t a, uint64_t b) {
            CgBin bin;
            bin.name = std::move(name);
            bin.base = "auto";
            bin.ranges.push_back({a, b});
            ci.bins.push_back(std::move(bin));
        };
        if (c.wide)
            for (auto& [a, b] : wideAuto) {
                uint64_t ka = keyOf(c, a), kb = keyOf(c, b);
                add(ka == kb ? "auto[" + wideDec(c, a) + "]" : "auto[" + wideDec(c, a) + ":" + wideDec(c, b) + "]", ka, kb);
            }
        else if (c.isEnum)
            for (auto& [name, v] : c.enumVals) {
                uint64_t k = keyOf(c, make(v, 0, c.w, 0));
                add("auto[" + name + "]", k, k);
            }
        else {
            i128 space = (i128)1 << c.w;
            i128 n = std::min<i128>(space, std::max<int64_t>(autoBinMax, 1));
            i128 per = space / n;
            for (i128 k = 0; k < n; k++) {
                uint64_t a = (uint64_t)(per * k), b = (uint64_t)(k == n - 1 ? space - 1 : per * (k + 1) - 1);
                add(a == b ? "auto[" + keyName(c, a) + "]" : "auto[" + keyName(c, a) + ":" + keyName(c, b) + "]", a, b);
            }
        }
    }
    // A bin whose values are all ignored or illegal, or that has none, is
    // left out of coverage (19.5.5, 19.11.1).
    for (auto& b : ci.bins) {
        if (b.kind != 0 || b.trans) continue;
        if (!b.wild.empty() || !b.xvals.empty()) continue;
        std::vector<KR> rs;
        for (auto& r : b.ranges) rs.push_back({r.first, r.second});
        if (rs.empty() || allCovered(rs, excl)) b.empty = true;
    }
}
inline bool binMatch(const CgBin& b, uint64_t k, const Val& v, bool hasX) {
    if (hasX) {
        for (auto& x : b.xvals)
            if (x.w > 64 ? x == v : ((x.v ^ v.v) & kmask(x.w)) == 0 && ((x.x ^ v.x) & kmask(x.w)) == 0 && ((x.z ^ v.z) & kmask(x.w)) == 0) return true;
        return false;
    }
    if (inPairs(b.ranges, k)) return true;
    for (auto& [care, val] : b.wild)
        if ((k & care) == val) return true;
    return false;
}
// A cross bin select expression on one tuple (19.6.1).
struct SelCtx {
    const CgDef* d;
    const CgInst* in;
    const CgCrossDef* xd;
    const CgCrossInst* xi;
    std::unordered_map<const CgSel*, std::vector<KR>> isect;
};
inline bool selEval(const CgSel& s, const std::vector<int>& tuple, SelCtx& cx) {
    switch (s.kind) {
        case 1: return !selEval(*s.kids[0], tuple, cx);
        case 2: return selEval(*s.kids[0], tuple, cx) && selEval(*s.kids[1], tuple, cx);
        case 3: return selEval(*s.kids[0], tuple, cx) || selEval(*s.kids[1], tuple, cx);
        case 4: return true;
        case 5: {   // the tuples of kids[0] for which enough value tuples make `with` true (19.6.1.2)
            if (!selEval(*s.kids[0], tuple, cx)) return false;
            size_t n = tuple.size();
            std::vector<std::vector<std::pair<uint64_t, uint64_t>>> rs(n);
            for (size_t j = 0; j < n; j++) {
                int bi = cx.xi->dims[j][tuple[j]];
                rs[j] = cx.in->cps[cx.xd->cps[j]].bins[bi].ranges;
                if (rs[j].empty()) return false;
            }
            uint64_t need = 1;
            if (s.matches) { Val m = eval(s.matches, rd); need = m.x ? 1 : m.v; }
            uint64_t yes = 0, tried = 0;
            std::vector<size_t> ri(n, 0);
            std::vector<uint64_t> k(n);
            for (size_t j = 0; j < n; j++) k[j] = rs[j][0].first;
            for (;;) {
                if (++tried > (1u << 22)) fail("too many value tuples for a cross with filter");
                for (size_t j = 0; j < n; j++) {
                    const CgCpDef& c = cx.d->cps[cx.xd->cps[j]];
                    S.set(s.valSigs[j], make((uint64_t)valOfKey(c, k[j]) & kmask(c.w), 0, c.w, 0));
                }
                bool t = truth(eval(s.with, rd)) == 1;
                if (t) yes++;
                if (!s.all && yes >= need) return true;
                if (s.all && !t) return false;
                size_t j = n;
                while (j-- > 0) {   // the next value tuple
                    if (k[j] < rs[j][ri[j]].second) { k[j]++; break; }
                    if (++ri[j] < rs[j].size()) { k[j] = rs[j][ri[j]].first; break; }
                    ri[j] = 0;
                    k[j] = rs[j][0].first;
                }
                if (j == (size_t)-1) break;
            }
            return s.all ? true : yes >= need;
        }
        default: break;
    }
    int bi = cx.xi->dims[s.cp][tuple[s.cp]];
    const CgBin& b = cx.in->cps[cx.xd->cps[s.cp]].bins[bi];
    if (!s.bin.empty() && b.base != s.bin) return false;
    if (s.isect.empty()) return true;
    const auto& is = cx.isect[&s];
    for (auto& r : b.ranges)
        for (auto& q : is)
            if (r.first <= q.b && q.a <= r.second) return true;
    return false;
}
inline void buildCross(const CgDef& d, CgInst& in, const CgCrossDef& xd, CgCrossInst& xi) {
    size_t total = 1;
    xi.pos.assign(xd.cps.size(), {});
    for (size_t j = 0; j < xd.cps.size(); j++) {
        const auto& cp = in.cps[xd.cps[j]];
        std::vector<int> dim;
        xi.pos[j].assign(cp.bins.size(), -1);
        for (size_t b = 0; b < cp.bins.size(); b++)
            if (cp.bins[b].kind == 0 && !cp.bins[b].empty) {
                xi.pos[j][b] = (int)dim.size();
                dim.push_back((int)b);
            }
        total *= std::max<size_t>(dim.size(), 1);
        if (dim.empty()) total = 0;
        if (total > kMaxBins) fail("cross " + xd.name + " has too many bins");
        xi.dims.push_back(std::move(dim));
    }
    xi.cls.assign(total, -1);
    xi.userOf.assign(total, {});
    xi.autoHits.assign(total, 0);
    SelCtx cx{&d, &in, &xd, &xi, {}};
    std::function<void(const CgSel&)> prep = [&](const CgSel& s) {
        if (!s.isect.empty()) {
            std::vector<KR> ks;
            g_cgWide = d.cps[xd.cps[s.cp]].wide ? &in.cps[xd.cps[s.cp]].wb : nullptr;   // keys among its bounds
            for (auto& it : s.isect) itemKeys(d.cps[xd.cps[s.cp]], it, false, ks, nullptr, nullptr);
            g_cgWide = nullptr;
            cx.isect[&s] = std::move(ks);
        }
        for (auto& k : s.kids) prep(*k);
    };
    for (auto& ub : xd.bins) prep(*ub.sel);
    std::vector<int> tuple(xd.cps.size());
    for (size_t u = 0; u < xd.bins.size(); u++) xi.ubins.push_back({xd.bins[u].name, xd.bins[u].kind, 0, 0});
    for (size_t t = 0; t < total; t++) {
        size_t r = t;
        for (size_t j = xd.cps.size(); j-- > 0;) {
            tuple[j] = (int)(r % xi.dims[j].size());
            r /= xi.dims[j].size();
        }
        bool ign = false, ill = false, any = false;
        for (size_t u = 0; u < xd.bins.size(); u++) {
            if (!selEval(*xd.bins[u].sel, tuple, cx)) continue;
            any = true;
            xi.ubins[u].size++;
            if (xd.bins[u].kind == 1) ign = true;
            else if (xd.bins[u].kind == 2) ill = true;
            else xi.userOf[t].push_back((int)u);
        }
        if (any) xi.bb++;
        xi.cls[t] = ill ? -3 : ign ? -2 : !xi.userOf[t].empty() ? xi.userOf[t][0] : -1;
    }
}
inline Val cgNew(const std::vector<Val>& a) {
    int d = (int)a[0].v;
    const CgDef& def = cgDefs()[d];
    CgInst in;
    in.def = d;
    in.name = def.name + "#" + std::to_string(def.insts.size());
    if (def.inClass) in.self = newFrame(nullptr, nullptr, (int)a.back().v);
    SelfScope ss(in);
    for (size_t k = 0; k < def.argSigs.size(); k++) {
        if (def.argRef[k]) { in.refSigs.push_back((int)a[k + 1].v); in.args.push_back(Val{}); }
        else { in.refSigs.push_back(-1); in.args.push_back(a[k + 1]); }
    }
    setArgs(in);
    // Options (19.7), evaluated now: they may read the arguments.
    in.opt[CO_WEIGHT] = optOr(def.opts, CO_WEIGHT, 1);
    in.opt[CO_GOAL] = optOr(def.opts, CO_GOAL, 100);
    in.opt[CO_AT_LEAST] = optOr(def.opts, CO_AT_LEAST, 1);
    in.opt[CO_AUTO_BIN_MAX] = optOr(def.opts, CO_AUTO_BIN_MAX, 64);
    in.opt[CO_PER_INSTANCE] = optOr(def.opts, CO_PER_INSTANCE, 0);
    in.opt[CO_GET_INST_COVERAGE] = optOr(def.opts, CO_GET_INST_COVERAGE, 0);
    in.opt[CO_TYPE_WEIGHT] = optOr(def.opts, CO_TYPE_WEIGHT, 1);
    in.opt[CO_TYPE_GOAL] = optOr(def.opts, CO_TYPE_GOAL, 100);
    in.opt[CO_TYPE_MERGE] = optOr(def.opts, CO_TYPE_MERGE, 0);
    for (const auto& c : def.cps) {
        std::vector<int64_t> o{optOr(c.opts, CO_WEIGHT, 1), optOr(c.opts, CO_GOAL, in.opt[CO_GOAL]),
                               optOr(c.opts, CO_AT_LEAST, in.opt[CO_AT_LEAST]),
                               optOr(c.opts, CO_AUTO_BIN_MAX, in.opt[CO_AUTO_BIN_MAX])};
        in.cpOpt.push_back(o);
        CgCpInst ci;
        double iv = 1.0;   // type_option.real_interval (19.7.1)
        if (const E& r = c.realInterval ? c.realInterval : def.realInterval) iv = realOfVal(eval(r, rd));
        buildCp(c, ci, o[3], iv);
        in.cps.push_back(std::move(ci));
    }
    for (const auto& x : def.crosses) {
        in.crOpt.push_back({optOr(x.opts, CO_WEIGHT, 1), optOr(x.opts, CO_GOAL, in.opt[CO_GOAL]),
                            optOr(x.opts, CO_AT_LEAST, in.opt[CO_AT_LEAST]), 0});
        CgCrossInst xi;
        buildCross(def, in, x, xi);
        in.crosses.push_back(std::move(xi));
    }
    cgInsts().push_back(std::move(in));
    int id = (int)cgInsts().size() - 1;
    cgDefs()[d].insts.push_back(id);
    if (def.inClass && !def.ev.empty()) g_cgInstStart(id);
    return make((uint64_t)id + 1, 0, 32, 0);
}
inline void report(const CgInst& in, const std::string& what, const std::string& bin, const std::string& value) {
    std::cout << "Error: covergroup " << cgDefs()[in.def].name << ": illegal " << what << (what.empty() ? "" : " ") << "bin " << bin
              << " hit (value " << value << ") at time " << S.now << "\n";
}
// One sample of an instance (19.3, 19.5).
inline void sampleInst(CgInst& in) {
    if (!in.on) return;
    SelfScope ss(in);
    const CgDef& d = cgDefs()[in.def];
    setArgs(in);
    for (size_t ci = 0; ci < d.cps.size(); ci++) {
        const CgCpDef& c = d.cps[ci];
        CgCpInst& cp = in.cps[ci];
        cp.hit.clear();
        cp.sampled = false;
        for (const auto& [t, e] : c.preIff) S.set(t, eval(e, rd));
        if (c.preIffProg) runPre(*c.preIffProg);
        if (c.iff && truth(eval(c.iff, rd)) != 1) continue;
        for (const auto& [t, e] : c.pre) S.set(t, eval(e, rd));
        if (c.preProg) runPre(*c.preProg);
        Val v = eval(c.expr, rd);
        bool hasX = !c.isReal && (c.wide ? unknownAny(v) : (v.x & kmask(c.w)) != 0);
        g_cgWide = c.wide ? &cp.wb : nullptr;
        uint64_t k = keyOf(c, v);
        auto iffOk = [&](const CgBin& b) { return !b.iff || truth(eval(b.iff, rd)) == 1; };
        bool illegal = false, ignored = false;
        for (auto& b : cp.bins)
            if (!b.trans && (b.kind == 1 || b.kind == 2) && binMatch(b, k, v, hasX) && iffOk(b)) {
                b.hits++;
                if (b.kind == 2) { illegal = true; report(in, "", c.name + "." + b.name, hasX ? "x" : c.wide ? wideDec(c, v) : keyName(c, k)); }
                else ignored = true;
            }
        if (!illegal && !ignored) {
            bool any = false;
            for (size_t bi = 0; bi < cp.bins.size(); bi++) {
                auto& b = cp.bins[bi];
                if (b.trans || b.kind != 0 || b.empty || !binMatch(b, k, v, hasX) || !iffOk(b)) continue;
                b.hits++;
                cp.hit.push_back((int)bi);
                any = true;
            }
            if (!any && !hasX)
                for (auto& b : cp.bins)
                    if (b.kind == 3 && !b.dseq && iffOk(b)) b.hits++;
        }
        if (!cp.atoms.empty()) {   // transitions (19.5.2)
            std::vector<char> val(cp.atoms.size());
            for (size_t a = 0; a < cp.atoms.size(); a++) val[a] = !hasX && inPairs(cp.atoms[a], k);
            sva::Deriv dv{sva::store(), val};
            bool anyTrans = false;
            for (size_t bi = 0; bi < cp.bins.size(); bi++) {
                auto& b = cp.bins[bi];
                if (!b.trans) continue;
                b.att.push_back({b.trans, 1});
                std::vector<std::pair<const sva::Node*, uint64_t>> next;
                uint64_t m = 0;
                for (auto& [t, n] : b.att) {
                    dv.memo.clear();
                    dv.matches = 0;
                    const sva::Node* r = dv.d(t);
                    m += n * (uint64_t)dv.matches;
                    if (r->k == sva::DONE || r->k == sva::NUL) continue;
                    bool merged = false;
                    for (auto& x : next) if (x.first == r) { x.second += n; merged = true; break; }
                    if (!merged) next.push_back({r, n});
                }
                b.att.swap(next);
                if (m && iffOk(b)) {
                    b.hits += m;
                    if (b.kind == 2) report(in, "transition", c.name + "." + b.name, keyName(c, k));
                    if (b.kind == 0) { cp.hit.push_back((int)bi); anyTrans = true; }
                }
            }
            if (!anyTrans)
                for (auto& b : cp.bins)
                    if (b.kind == 3 && b.dseq && iffOk(b)) b.hits++;
        }
        cp.sampled = true;
    }
    for (size_t xi = 0; xi < d.crosses.size(); xi++) {   // crosses (19.6)
        const CgCrossDef& xd = d.crosses[xi];
        CgCrossInst& x = in.crosses[xi];
        if (x.cls.empty()) continue;
        if (xd.iff && truth(eval(xd.iff, rd)) != 1) continue;
        std::vector<std::vector<int>> hits(xd.cps.size());
        bool ok = true;
        for (size_t j = 0; j < xd.cps.size() && ok; j++) {
            const CgCpInst& cp = in.cps[xd.cps[j]];
            if (!cp.sampled) { ok = false; break; }
            for (int b : cp.hit)
                if (x.pos[j][b] >= 0) hits[j].push_back(x.pos[j][b]);
            ok = !hits[j].empty();
        }
        if (!ok) continue;
        std::vector<size_t> idx(xd.cps.size(), 0);
        for (;;) {
            size_t t = 0;
            for (size_t j = 0; j < xd.cps.size(); j++) t = t * x.dims[j].size() + (size_t)hits[j][idx[j]];
            int cl = x.cls[t];
            if (cl == -1) x.autoHits[t]++;
            else if (cl == -3) {
                for (size_t u = 0; u < xd.bins.size(); u++)
                    if (xd.bins[u].kind == 2) { x.ubins[u].hits++; report(in, "cross", xd.name + "." + xd.bins[u].name, "tuple"); break; }
            } else if (cl >= 0)
                for (int u : x.userOf[t]) x.ubins[u].hits++;
            size_t j = xd.cps.size();
            while (j-- > 0) {
                if (++idx[j] < hits[j].size()) break;
                idx[j] = 0;
            }
            if (j == (size_t)-1) break;
        }
    }
}
// (covered, total) of an item (a coverpoint, or a cross after the
// coverpoints) in one instance.
inline std::pair<double, double> itemCount(const CgInst& in, int item) {
    int nc = (int)in.cps.size();
    return item < nc ? cgCpCount(in.cps[item], in.cpOpt[item][2]) : cgCrossCount(in.crosses[item - nc], in.crOpt[item - nc][2]);
}
inline int64_t itemWeight(const CgInst& in, int item) {
    int nc = (int)in.cps.size();
    return item < nc ? in.cpOpt[item][0] : in.crOpt[item - nc][0];
}
// Type coverage with merge_instances (19.11.3): the bins of all instances,
// joined by name; a bin is covered when its summed count reaches the
// largest at_least.
inline std::pair<double, double> mergedCount(const CgDef& d, int item) {
    std::map<std::string, uint64_t> hits;
    int64_t al = 1;
    int nc = (int)d.cps.size();
    for (int id : d.insts) {
        const CgInst& in = cgInsts()[id];
        if (item < nc) {
            al = std::max(al, in.cpOpt[item][2]);
            for (auto& b : in.cps[item].bins)
                if (b.kind == 0 && !b.empty) hits[b.name] += b.hits;
        } else {
            const CgCrossInst& x = in.crosses[item - nc];
            al = std::max(al, in.crOpt[item - nc][2]);
            const CgCrossDef& xd = d.crosses[item - nc];
            for (size_t t = 0; t < x.cls.size(); t++) {
                if (x.cls[t] != -1) continue;
                std::string n = "<";
                size_t r = t;
                std::vector<int> tuple(xd.cps.size());
                for (size_t j = xd.cps.size(); j-- > 0;) { tuple[j] = (int)(r % x.dims[j].size()); r /= x.dims[j].size(); }
                for (size_t j = 0; j < xd.cps.size(); j++)
                    n += (j ? "," : "") + in.cps[xd.cps[j]].bins[x.dims[j][tuple[j]]].name;
                hits[n + ">"] += x.autoHits[t];
            }
            for (auto& u : x.ubins)
                if (u.kind == 0 && u.size) hits[u.name] += u.hits;
        }
    }
    double cov = 0;
    for (auto& [n, h] : hits) if ((int64_t)h >= al) cov++;
    return {cov, (double)hits.size()};
}
inline double typeCoverage(const CgDef& d, int item, double* covered = nullptr, double* total = nullptr) {
    if (covered) *covered = 0;
    if (total) *total = 0;
    if (d.insts.empty()) return 0.0;
    int nitems = (int)(d.cps.size() + d.crosses.size());
    const CgInst& first = cgInsts()[d.insts[0]];
    if (first.opt[CO_TYPE_MERGE]) {
        auto one = [&](int it) {
            auto c = mergedCount(d, it);
            if (covered) *covered += c.first;
            if (total) *total += c.second;
            return c;
        };
        if (item >= 0) return cgPct(one(item), itemWeight(first, item));
        double num = 0, den = 0;
        for (int it = 0; it < nitems; it++) {
            auto c = one(it);
            if (c.second == 0) continue;
            num += itemWeight(first, it) * c.first / c.second;
            den += itemWeight(first, it);
        }
        return den == 0 ? (first.opt[CO_TYPE_WEIGHT] ? 0.0 : 100.0) : 100.0 * num / den;
    }
    double num = 0, den = 0;   // the weighted average of the instances
    for (int id : d.insts) {
        const CgInst& in = cgInsts()[id];
        if (item >= 0) {
            auto c = itemCount(in, item);
            if (covered) *covered += c.first;
            if (total) *total += c.second;
            num += itemWeight(in, item) * cgPct(c, itemWeight(in, item));
            den += itemWeight(in, item);
        } else {
            for (int it = 0; it < nitems; it++) {
                auto c = itemCount(in, it);
                if (covered) *covered += c.first;
                if (total) *total += c.second;
            }
            num += in.opt[CO_WEIGHT] * cgInstCoverage(in);
            den += in.opt[CO_WEIGHT];
        }
    }
    return den == 0 ? 0.0 : num / den;
}
inline CgInst& instOf(const Val& h) {
    if (h.x || h.v == 0 || h.v > cgInsts().size()) {
        std::cout << "Error: null covergroup handle at time " << S.now << "\n";
        throw std::runtime_error("msim: covergroup method on a null handle");
    }
    return cgInsts()[h.v - 1];
}
inline Val hook(int op, const std::vector<Val>& a) {
    switch (op) {
        case CG_NEW: return cgNew(a);
        case CG_SAMPLE: {
            CgInst& in = instOf(a[0]);
            const CgDef& d = cgDefs()[in.def];
            for (size_t k = 0; k < d.sampleSigs.size() && k + 1 < a.size(); k++) S.set(d.sampleSigs[k], a[k + 1]);
            sampleInst(in);
            return make(0, 0, 64, 0);
        }
        case CG_START: instOf(a[0]).on = true; return make(0, 0, 64, 0);
        case CG_STOP: instOf(a[0]).on = false; return make(0, 0, 64, 0);
        case CG_SETNAME: return make(0, 0, 64, 0);
        case CG_GETCOV: return mkreal(typeCoverage(cgDefs()[a[0].v], -1));
        case CG_INSTCOV: return mkreal(cgInstCoverage(instOf(a[0])));
        case CG_ITEMCOV: return mkreal(typeCoverage(cgDefs()[a[0].v], (int)a[1].v));
        case CG_ITEMINST: {
            const CgInst& in = instOf(a[0]);
            return mkreal(cgPct(itemCount(in, (int)a[1].v), itemWeight(in, (int)a[1].v)));
        }
        case CG_COUNT: {   // get_coverage(covered, total): kid 2 is 1 for an instance, kid 3 which count
            int item = (int)(int32_t)a[1].v;
            bool inst = a[2].v != 0;
            double cov = 0, tot = 0;
            if (inst) {
                const CgInst& in = instOf(a[0]);
                int n = (int)(in.cps.size() + in.crosses.size());
                for (int it = item >= 0 ? item : 0; it < (item >= 0 ? item + 1 : n); it++) {
                    auto c = itemCount(in, it);
                    cov += c.first;
                    tot += c.second;
                }
            } else
                typeCoverage(cgDefs()[a[0].v], item, &cov, &tot);
            return make((uint64_t)(int64_t)(a[3].v ? tot : cov), 0, 32, 0);
        }
        case CG_GLOBAL: {   // $get_coverage (19.9): the types, weighted by type_option.weight
            double num = 0, den = 0;
            for (auto& d : cgDefs()) {
                if (d.insts.empty()) continue;
                int64_t w = cgInsts()[d.insts[0]].opt[CO_TYPE_WEIGHT];
                num += w * typeCoverage(d, -1);
                den += w;
            }
            return mkreal(den == 0 ? 100.0 : num / den);
        }
        default: return mkreal(0);
    }
}
}   // namespace cg

// A covergroup's sampling event (19.3): every started instance samples.
inline Task cgProc(int d) {
    CgDef& def = cgDefs()[d];
    for (;;) {
        if (def.ev.size() > 1) co_await WaitAny{&def.ev};
        else co_await Wait{def.ev[0].first, def.ev[0].second};
        if (def.evIff && truth(eval(def.evIff, rd)) != 1) continue;
        for (int id : def.insts) cg::sampleInst(cgInsts()[id]);
    }
}
// A class's covergroup (19.4): each instance waits on its own object's
// event, resolved when the instance is created (the event may name a
// property or another object's member: it has to exist by then).
inline Task cgInstProc(int id, std::vector<std::pair<int, Edge>> ev) {
    for (;;) {
        if (ev.size() > 1) co_await WaitAny{&ev};
        else co_await Wait{ev[0].first, ev[0].second};
        CgInst& in = cgInsts()[id];
        const CgDef& def = cgDefs()[in.def];
        if (def.evIff) {
            cg::SelfScope ss(in);
            if (truth(eval(def.evIff, rd)) != 1) continue;
        }
        cg::sampleInst(in);
    }
}
// The hooks covergroup new() needs, before any process runs: declaration
// initializers (cg_t cg = new()) may run before the simulation starts.
inline void installCovergroupHooks() {
    g_cgHook = cg::hook;
    cg::g_cgRun = [](const Prog* p, const CgInst& in) {
        Frame* sf = g_fr;
        Frame* sr = g_ret;
        auto ctl = make_shared<ProcCtl>();
        Task t = exec(p, ctl, in.self ? in.self : newFrame(nullptr, nullptr, 0));
        t.h.resume();
        if (!t.h.done()) {
            cerr << "error: a coverpoint expression waited (time " << S.now << ")\n";
            exit(1);
        }
        t.h.destroy();
        g_fr = sf;
        g_ret = sr;
    };
    cg::g_cgInstStart = [](int id) {
        const CgInst& in = cgInsts()[id];
        cg::SelfScope ss(in);
        std::vector<std::pair<int, Edge>> ev;
        for (auto& [s, e] : cgDefs()[in.def].ev) ev.push_back({R(s), e});
        S.spawn(cgInstProc(id, std::move(ev)));
    };
}
inline void startCovergroups() {
    installCovergroupHooks();
    for (size_t d = 0; d < cgDefs().size(); d++)
        if (!cgDefs()[d].ev.empty() && !cgDefs()[d].inClass) S.spawn(cgProc((int)d));
}

// Functional coverage for the coverage database (msim -cov; mcov and mplan
// read it): two JSON members, written after the code coverage sections.
//   "covergroups": one entry per covergroup type that was instantiated,
//     [type, weight, goal, [[item, "point" | "cross", weight, goal,
//     at_least, bins in total, [[bin, hits], ...]], ...]]. The bins of all
//     instances are joined by name, as merge_instances does (19.11.3). A
//     coverpoint lists every counted bin; a cross lists the bins that were
//     hit (its total says how many there are).
//   "assertions": [name, "assert" | "assume" | "cover", evaluations,
//     passes, failures] per assertion (16.3 statistics).
// Covergroups and assertions declared in protected code (34) are left out,
// unless its coverage right is on, as for line and branch coverage.
inline void writeFuncCov(std::ostream& f) {
    auto js = [](const std::string& x) {
        std::string r = "\"";
        for (char c : x) {
            if (c == '"' || c == '\\') { r += '\\'; r += c; }
            else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
            else r += c;
        }
        return r + "\"";
    };
    f << " \"covergroups\": [";
    bool firstG = true;
    for (const CgDef& d : cgDefs()) {
        if (d.insts.empty() || d.prot) continue;
        const CgInst& first = cgInsts()[d.insts[0]];
        f << (firstG ? "\n  " : ",\n  ") << "[" << js(d.name) << ", " << first.opt[CO_TYPE_WEIGHT] << ", "
          << first.opt[CO_TYPE_GOAL] << ", [";
        firstG = false;
        int nc = (int)d.cps.size(), n = nc + (int)d.crosses.size();
        for (int item = 0; item < n; item++) {
            bool cross = item >= nc;
            // Bins in declaration order; a name seen again in a later instance adds to it.
            std::vector<std::pair<std::string, uint64_t>> bins;
            std::unordered_map<std::string, size_t> at;
            auto add = [&](const std::string& name, uint64_t hits) {
                auto [it, fresh] = at.emplace(name, bins.size());
                if (fresh) bins.push_back({name, hits});
                else bins[it->second].second += hits;
            };
            int64_t al = 1;
            for (int id : d.insts) {
                const CgInst& in = cgInsts()[id];
                if (!cross) {
                    al = std::max(al, in.cpOpt[item][2]);
                    for (auto& b : in.cps[item].bins)
                        if (b.kind == 0 && !b.empty) add(b.name, b.hits);
                } else {
                    const CgCrossInst& x = in.crosses[item - nc];
                    const CgCrossDef& xd = d.crosses[item - nc];
                    al = std::max(al, in.crOpt[item - nc][2]);
                    for (size_t t = 0; t < x.cls.size(); t++) {
                        if (x.cls[t] != -1) continue;
                        std::string name = "<";
                        size_t r = t;
                        std::vector<int> tuple(xd.cps.size());
                        for (size_t j = xd.cps.size(); j-- > 0;) { tuple[j] = (int)(r % x.dims[j].size()); r /= x.dims[j].size(); }
                        for (size_t j = 0; j < xd.cps.size(); j++)
                            name += (j ? "," : "") + in.cps[xd.cps[j]].bins[x.dims[j][tuple[j]]].name;
                        add(name + ">", x.autoHits[t]);
                    }
                    for (auto& u : x.ubins)
                        if (u.kind == 0 && u.size) add(u.name, u.hits);
                }
            }
            const auto& o = cross ? first.crOpt[item - nc] : first.cpOpt[item];
            f << (item ? ",\n   " : "\n   ") << "[" << js(cross ? d.crosses[item - nc].name : d.cps[item].name) << ", "
              << (cross ? "\"cross\"" : "\"point\"") << ", " << o[0] << ", " << o[1] << ", " << al << ", " << bins.size() << ", [";
            bool firstB = true;
            for (auto& [name, hits] : bins) {
                if (cross && !hits) continue;
                f << (firstB ? "" : ", ") << "[" << js(name) << ", " << hits << "]";
                firstB = false;
            }
            f << "]]";
        }
        f << "]]";
    }
    f << "\n ],\n \"assertions\": [";
    bool firstA = true;
    for (const AssertInfo& a : asserts()) {
        if (a.hidden || a.prot) continue;
        f << (firstA ? "\n  " : ",\n  ") << "[" << js(a.name) << ", "
          << (a.dir == AD_COVER ? "\"cover\"" : a.dir == AD_ASSUME ? "\"assume\"" : "\"assert\"") << ", "
          << a.evals << ", " << a.passes << ", " << a.fails << "]";
        firstA = false;
    }
    f << "\n ]";
}
