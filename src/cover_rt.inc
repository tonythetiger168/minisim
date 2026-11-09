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
// A sampled value's key: its w bits, the sign bit flipped when signed.
inline uint64_t keyOf(const CgCpDef& c, const Val& v) {
    if (c.isReal) return rkey(realOfVal(v));
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
inline void buildCp(const CgCpDef& c, CgCpInst& ci, int64_t autoBinMax, double realInterval = 1.0) {
    std::vector<KR> excl;   // ignored and illegal values
    bool explicitBins = false;
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
        if (!bd.array) {
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
        if (c.isEnum)
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
            if (((x.v ^ v.v) & kmask(x.w)) == 0 && ((x.x ^ v.x) & kmask(x.w)) == 0 && ((x.z ^ v.z) & kmask(x.w)) == 0) return true;
        return false;
    }
    if (inPairs(b.ranges, k)) return true;
    for (auto& [care, val] : b.wild)
        if ((k & care) == val) return true;
    return false;
}
// A cross bin select expression on one tuple (19.6.1).
struct SelCtx {
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
    SelCtx cx{&in, &xd, &xi, {}};
    std::function<void(const CgSel&)> prep = [&](const CgSel& s) {
        if (!s.isect.empty()) {
            std::vector<KR> ks;
            for (auto& it : s.isect) itemKeys(d.cps[xd.cps[s.cp]], it, false, ks, nullptr, nullptr);
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
    return make((uint64_t)id + 1, 0, 32, 0);
}
inline void report(const CgInst& in, const std::string& what, const std::string& bin, const std::string& value) {
    std::cout << "Error: covergroup " << cgDefs()[in.def].name << ": illegal " << what << (what.empty() ? "" : " ") << "bin " << bin
              << " hit (value " << value << ") at time " << S.now << "\n";
}
// One sample of an instance (19.3, 19.5).
inline void sampleInst(CgInst& in) {
    if (!in.on) return;
    const CgDef& d = cgDefs()[in.def];
    setArgs(in);
    for (size_t ci = 0; ci < d.cps.size(); ci++) {
        const CgCpDef& c = d.cps[ci];
        CgCpInst& cp = in.cps[ci];
        cp.hit.clear();
        cp.sampled = false;
        if (c.iff && truth(eval(c.iff, rd)) != 1) continue;
        Val v = eval(c.expr, rd);
        bool hasX = !c.isReal && (v.x & kmask(c.w)) != 0;
        uint64_t k = keyOf(c, v);
        auto iffOk = [&](const CgBin& b) { return !b.iff || truth(eval(b.iff, rd)) == 1; };
        bool illegal = false, ignored = false;
        for (auto& b : cp.bins)
            if (!b.trans && (b.kind == 1 || b.kind == 2) && binMatch(b, k, v, hasX) && iffOk(b)) {
                b.hits++;
                if (b.kind == 2) { illegal = true; report(in, "", c.name + "." + b.name, hasX ? "x" : keyName(c, k)); }
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
inline void startCovergroups() {
    g_cgHook = cg::hook;
    for (size_t d = 0; d < cgDefs().size(); d++)
        if (!cgDefs()[d].ev.empty()) S.spawn(cgProc((int)d));
}
