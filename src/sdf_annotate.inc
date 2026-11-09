// SDF back-annotation (IEEE 1800-2023 clause 32), run time side: applies
// the annotations sdf::read() found to the design's path delays
// (pathRecs), interconnect delays (Sim::icDelay), timing check limits
// (timingChecks) and specparams. Included from exec.inc.

namespace sdfa {

using sdf::DelVal;
using sdf::Op;
using sdf::Triple;

struct Ctx {
    sdf::Sel sel;
    double unitSec = 1e-9;            // the SDF file's TIMESCALE
    double tickSec = 1e-9;
    bool early = true;                // before time 0: LABELs were applied when lowering
    std::string file, scope;
    std::ofstream log;
    int warnings = 0, annotated = 0;
    void warn(const Op* op, const std::string& m) {
        warnings++;
        std::string where = file + (op ? ":" + std::to_string(op->line) : std::string());
        std::cerr << "msim: SDF warning: " << where << ": " << m << "\n";
        if (log.is_open()) log << "Warning: " << where << ": " << m << "\n";
    }
    void note(const Op& op, const std::string& m) {
        annotated++;
        if (log.is_open()) log << file << ":" << op.line << ": " << m << "\n";
    }
};

// The selected member of a triple, scaled (32.9), in kernel ticks.
inline std::optional<int64_t> pick(const Triple& t, const Ctx& c) {
    auto v = sdf::pick(t, c.sel);
    if (!v) return std::nullopt;
    return (int64_t)std::llround(*v * c.unitSec / c.tickSec);
}

// Table 32-4: n SDF values (1, 2, 3, 6 or 12; any may be missing) as the
// 12 transitions. A transition whose value needs a missing one keeps its
// old value.
inline bool table32_4(const std::vector<std::optional<int64_t>>& v, std::array<std::optional<int64_t>, 12>& out) {
    // Each entry: 0 = v[a], 1 = min(v[a], v[b]), 2 = max(v[a], v[b]) (1-based a, b).
    struct R { int op, a, b; };
    static const R t2[12] = {{0,1,0},{0,2,0},{0,1,0},{0,1,0},{0,2,0},{0,2,0},{0,1,0},{0,1,0},{0,2,0},{0,2,0},{2,1,2},{1,1,2}};
    static const R t3[12] = {{0,1,0},{0,2,0},{0,3,0},{0,1,0},{0,3,0},{0,2,0},{1,1,3},{0,1,0},{1,2,3},{0,2,0},{0,3,0},{1,1,2}};
    static const R t6[12] = {{0,1,0},{0,2,0},{0,3,0},{0,4,0},{0,5,0},{0,6,0},{1,1,3},{2,1,4},{1,2,5},{2,2,6},{2,3,5},{1,4,6}};
    size_t n = v.size();
    for (auto& o : out) o.reset();
    if (n == 1) { for (auto& o : out) o = v[0]; return true; }
    if (n == 12) { for (size_t k = 0; k < 12; k++) out[k] = v[k]; return true; }
    const R* t = n == 2 ? t2 : n == 3 ? t3 : n == 6 ? t6 : nullptr;
    if (!t) return false;
    for (int k = 0; k < 12; k++) {
        const auto& a = v[(size_t)t[k].a - 1];
        if (t[k].op == 0) { out[(size_t)k] = a; continue; }
        const auto& b = v[(size_t)t[k].b - 1];
        if (!a || !b) continue;
        out[(size_t)k] = t[k].op == 1 ? std::min(*a, *b) : std::max(*a, *b);
    }
    return true;
}

// A delay list onto 12 transitions: ABSOLUTE replaces, INCREMENT adds; a
// negative result is 0 (30.5.1).
template <class D> bool apply12(Ctx& c, const Op& op, D& d) {
    std::vector<std::optional<int64_t>> v;
    for (const DelVal& dv : op.vals) v.push_back(pick(dv.d, c));
    std::array<std::optional<int64_t>, 12> t;
    if (!table32_4(v, t)) {
        c.warn(&op, std::to_string(op.vals.size()) + " delay values (1, 2, 3, 6 or 12 are allowed)");
        return false;
    }
    for (size_t k = 0; k < 12; k++) {
        if (!t[k]) continue;
        int64_t x = op.incr ? (int64_t)d[k] + *t[k] : *t[k];
        d[k] = (uint64_t)std::max<int64_t>(0, x);
    }
    return true;
}

inline std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "." + b;
}
inline std::string clean(const std::string& s) {
    std::string r;
    for (auto& p : sdf::splitPath(s)) r = joinPath(r, p);
    return r;
}
// "d" matches "d" and a bit of it ("d[3]"): the path or check is on the
// whole port, so a bit's annotation reaches it.
inline bool sameName(const std::string& sv, const std::string& sdfName) {
    std::string n = sdf::unescape(sdfName);
    if (sv == n) return true;
    auto b = n.find('[');
    return b != std::string::npos && sv == n.substr(0, b);
}
inline uint8_t edgeBits(const std::string& e) {
    if (e == "posedge") return TE_POS;
    if (e == "negedge") return TE_NEG;
    if (e == "01") return TE_01;
    if (e == "10") return TE_10;
    if (e == "0z") return TE_0X;
    if (e == "z1") return TE_X1;
    if (e == "1z") return TE_1X;
    if (e == "z0") return TE_X0;
    return 0;
}

// The instances one CELL applies to: INSTANCE under the scope, or with
// INSTANCE * every instance of CELLTYPE under it.
inline std::vector<std::string> targets(Ctx& c, const Op& op) {
    std::vector<std::string> r;
    if (op.wild) {
        for (auto& [path, cell] : instCells())
            if ((path == c.scope || path.rfind(c.scope + ".", 0) == 0) && cell == op.cellType) r.push_back(path);
        if (r.empty()) c.warn(&op, "no instance of cell " + op.cellType + " under " + c.scope);
        return r;
    }
    std::string p = joinPath(c.scope, clean(op.inst));
    auto it = instCells().find(p);
    if (it == instCells().end()) { c.warn(&op, "no instance " + p); return r; }
    if (!op.cellType.empty() && it->second != op.cellType) {
        c.warn(&op, "instance " + p + " is a " + it->second + ", not the CELLTYPE " + op.cellType);
        return r;
    }
    r.push_back(p);
    return r;
}

inline void iopath(Ctx& c, const Op& op, const std::string& inst) {
    int hits = 0;
    for (auto& p : pathRecs()) {
        if (p.inst != inst || !sameName(p.in, op.a.name) || !sameName(p.out, op.b.name)) continue;
        if (op.hasCond && (!p.hasCond || p.cond != op.cond)) continue;
        if (op.condElse && !p.ifnone) continue;
        if (!op.a.edge.empty() && p.edge) {
            uint8_t e = op.a.edge == "posedge" ? 1 : op.a.edge == "negedge" ? 2 : 0;
            if (e && p.edge != 3 && p.edge != e) continue;
        }
        if (!apply12(c, op, p.d)) return;
        p.annotated = true;
        hits++;
    }
    std::string what = std::string(op.condElse ? "CONDELSE " : op.hasCond ? "COND " + op.cond + " " : "") +
                       "IOPATH " + op.a.name + " " + op.b.name;
    if (!hits) c.warn(&op, "no specify path " + inst + " (" + op.a.name + " => " + op.b.name + ")" +
                               (op.hasCond ? " with condition " + op.cond : op.condElse ? " (ifnone)" : "") +
                               " for " + what);
    else c.note(op, what + " -> " + inst + " (" + std::to_string(hits) + " path" + (hits > 1 ? "s" : "") + ")");
}

// DEVICE: every path to the instance's outputs, or to the one output named.
inline void device(Ctx& c, const Op& op, const std::string& inst) {
    int hits = 0;
    std::string port = op.a.name.empty() ? "" : clean(op.a.name);
    std::string where = inst;
    // DEVICE u1.y: the port of a child instance.
    if (auto dot = port.rfind('.'); dot != std::string::npos) {
        where = joinPath(inst, port.substr(0, dot));
        port = port.substr(dot + 1);
    } else if (!port.empty() && instCells().count(joinPath(inst, port))) {   // DEVICE u1: a child instance
        where = joinPath(inst, port);
        port.clear();
    }
    for (auto& p : pathRecs()) {
        if (p.inst != where || (!port.empty() && !sameName(p.out, port))) continue;
        if (!apply12(c, op, p.d)) return;
        p.annotated = true;
        hits++;
    }
    if (!hits) c.warn(&op, "DEVICE: no specify path to " + (port.empty() ? "the outputs of " + where : where + "." + port) +
                               " (primitive delays are not annotated)");
    else c.note(op, "DEVICE -> " + std::to_string(hits) + " paths in " + where);
}

// PORT / NETDELAY / INTERCONNECT (32.4.4): a delay to a load port, an input
// or inout of an instance. The delay applies to every source of the net.
inline void interconnect(Ctx& c, const Op& op, const std::string& inst) {
    const sdf::Port& load = op.kind == sdf::INTERCONNECT ? op.b : op.a;
    std::string lp = joinPath(inst, clean(load.name));
    auto it = portRecs().find(lp);
    if (it == portRecs().end() || it->second.dir == 1) {
        c.warn(&op, "no input or inout port " + lp + " for an interconnect delay");
        return;
    }
    if (!it->second.net) {
        c.warn(&op, "port " + lp + " is a variable, not a net: no interconnect delay");
        return;
    }
    if (op.kind == sdf::INTERCONNECT) {
        std::string sp = joinPath(inst, clean(op.a.name));
        auto si = portRecs().find(sp);
        if (si == portRecs().end() || si->second.dir == 0)
            c.warn(&op, "no output or inout port " + sp + " (annotating the delay to " + lp + " from every source)");
    }
    auto [ic, fresh] = S.icDelay.try_emplace(it->second.sig);
    if (fresh) ic->second.fill(0);
    if (!apply12(c, op, ic->second)) return;
    if ((size_t)it->second.sig < S.timedOut.size()) S.timedOut[(size_t)it->second.sig] = 1;
    c.note(op, std::string(op.kind == sdf::PORT ? "PORT" : op.kind == sdf::NETDELAY ? "NETDELAY" : "INTERCONNECT") +
                   " -> " + lp);
}

// A limit in the module's time unit, as the violation report prints it.
inline std::string limStr(int64_t t, uint64_t unit) {
    if (unit <= 1 || t % (int64_t)unit == 0) return std::to_string(t / (int64_t)unit);
    std::ostringstream o;
    o << (double)t / (double)unit;
    return o.str();
}

// TIMINGCHECK (32.4.2, Table 32-2).
inline void timingCheck(Ctx& c, const Op& op, const std::string& inst) {
    // Which SystemVerilog checks each SDF check annotates, and which of
    // its values go to lim1 / lim2 (-1: none).
    struct Map { int kind; int v1, v2; };
    std::vector<Map> maps;
    const std::string& k = op.tchk;
    if (k == "SETUP") maps = {{TC_SETUP, 0, -1}, {TC_SETUPHOLD, 0, -1}};
    else if (k == "HOLD") maps = {{TC_HOLD, 0, -1}, {TC_SETUPHOLD, -1, 0}};
    else if (k == "SETUPHOLD") maps = {{TC_SETUP, 0, -1}, {TC_HOLD, 1, -1}, {TC_SETUPHOLD, 0, 1}};
    else if (k == "RECOVERY") maps = {{TC_RECOVERY, 0, -1}, {TC_RECREM, 0, -1}};
    else if (k == "REMOVAL") maps = {{TC_REMOVAL, 0, -1}, {TC_RECREM, -1, 0}};
    else if (k == "RECREM") maps = {{TC_RECOVERY, 0, -1}, {TC_REMOVAL, 1, -1}, {TC_RECREM, 0, 1}};
    else if (k == "SKEW") maps = {{TC_SKEW, 0, -1}, {TC_TIMESKEW, 0, -1}};
    else if (k == "BIDIRECTSKEW") maps = {{TC_FULLSKEW, 0, 1}};
    else if (k == "WIDTH") maps = {{TC_WIDTH, 0, -1}};
    else if (k == "PERIOD") maps = {{TC_PERIOD, 0, -1}};
    else if (k == "NOCHANGE") maps = {{TC_NOCHANGE, 0, 1}};
    size_t need = (k == "SETUPHOLD" || k == "RECREM" || k == "BIDIRECTSKEW" || k == "NOCHANGE") ? 2 : 1;
    if (op.vals.size() != need) {
        c.warn(&op, k + " needs " + std::to_string(need) + " value" + (need > 1 ? "s" : ""));
        return;
    }
    if (!op.scond.empty() || !op.ccond.empty())
        c.warn(&op, k + ": SCOND / CCOND are not matched; annotating as if they were absent");
    // SDF names the data signal first for SETUP, HOLD and SETUPHOLD, and the
    // reference first for the others (as SystemVerilog writes them).
    bool dataFirst = k == "SETUP" || k == "HOLD" || k == "SETUPHOLD";
    const sdf::Port* refP = dataFirst ? &op.b : &op.a;
    const sdf::Port* dataP = (k == "WIDTH" || k == "PERIOD") ? nullptr : dataFirst ? &op.a : &op.b;
    auto match = [&](const TcEvent& ev, const sdf::Port& p) {
        if (!sameName(ev.port, p.name)) return false;
        if (!p.edge.empty() && edgeBits(p.edge) != ev.edges) return false;
        if (p.hasCond && ev.condText != p.cond) return false;
        return true;
    };
    std::optional<int64_t> v[2];
    for (size_t i = 0; i < op.vals.size(); i++) v[i] = pick(op.vals[i].d, c);
    int hits = 0;
    for (auto& tc : timingChecks()) {
        if (tc.inst != inst) continue;
        const Map* m = nullptr;
        for (auto& mm : maps) if (mm.kind == tc.kind) m = &mm;
        if (!m) continue;
        if (!match(tc.ev[tc.ref], *refP)) continue;
        if (dataP && tc.nev == 2 && !match(tc.ev[tc.data], *dataP)) continue;
        bool neg = tc.kind == TC_SETUPHOLD || tc.kind == TC_RECREM;   // 31.9: negative limits
        auto put = [&](int64_t& lim, int vi) {
            if (vi < 0 || !v[vi]) return;
            int64_t x = op.incr ? lim + *v[vi] : *v[vi];
            lim = neg ? x : std::max<int64_t>(0, x);
        };
        put(tc.lim1, m->v1);
        put(tc.lim2, m->v2);
        bool two = tc.limText.find(',') != std::string::npos;
        tc.limText = limStr(tc.lim1, tc.unit);
        if (two) tc.limText += ", " + limStr(tc.kind == TC_WIDTH ? tc.thresh : tc.lim2, tc.unit);
        hits++;
    }
    std::string what = k + " " + (dataFirst ? "" : "") + op.a.name + (op.b.name.empty() ? "" : " " + op.b.name);
    if (!hits) c.warn(&op, "no timing check in " + inst + " for " + what);
    else c.note(op, what + " -> " + std::to_string(hits) + " check" + (hits > 1 ? "s" : "") + " in " + inst);
}

// LABEL (32.4.3): specparams take their SDF values when the design is
// lowered (sdfLabels in lower.hpp), so every expression that uses one is
// computed with the new value. A $sdf_annotate whose arguments are known
// only at run time comes too late for that.
inline void label(Ctx& c, const Op& op, const std::string& inst) {
    std::string name = joinPath(inst, sdf::unescape(op.label));
    if (!c.early) { c.warn(&op, "LABEL " + name + ": a $sdf_annotate with run-time arguments cannot change specparams"); return; }
    if (!labelledNames().count(name)) { c.warn(&op, "no specparam " + name); return; }
    c.note(op, "LABEL " + op.label + " -> " + name);
}

inline int annotate(const SdfRequest& r, bool early) {
    Ctx c;
    c.file = r.file;
    c.scope = r.scope;
    c.early = early;
    c.sel.mtm = mtmSelect();
    c.tickSec = std::pow(10.0, timeFmt().tickExp);
    if (!r.log.empty()) {
        c.log.open(r.log);
        if (!c.log) std::cerr << "msim: $sdf_annotate: cannot write log file " << r.log << "\n";
    }
    std::vector<std::string> cw;
    if (!r.config.empty()) sdf::readConfig(c.sel, r.config, mtmSelect(), cw);
    for (auto& w : cw) c.warn(nullptr, w);
    if (!r.mtm.empty() && !sdf::setMtm(c.sel, r.mtm, mtmSelect())) c.warn(nullptr, "bad mtm_spec \"" + r.mtm + "\"");
    if (!r.scale.empty() && !sdf::setScale(c.sel, r.scale)) c.warn(nullptr, "bad scale_factors \"" + r.scale + "\"");
    if (!r.scaleType.empty() && !sdf::setScaleType(c.sel, r.scaleType)) c.warn(nullptr, "bad scale_type \"" + r.scaleType + "\"");
    sdf::File f;
    sdf::Error err;
    if (!sdf::read(r.file, f, err)) {
        std::cerr << "msim: " << r.where << "$sdf_annotate: " << r.file << (err.line ? ":" + std::to_string(err.line) : "")
                  << ": " << err.msg << "\n";
        return -1;
    }
    c.unitSec = f.timescale;
    for (auto& w : f.warnings) c.warn(nullptr, w);
    if (c.log.is_open())
        c.log << "SDF " << r.file << " -> " << r.scope << " (" << (c.sel.mtm == 0 ? "MINIMUM" : c.sel.mtm == 2 ? "MAXIMUM" : "TYPICAL") << ")\n";
    for (const Op& op : f.ops) {
        // 32.7: pulse limits are not modeled; a delay's pulse limits follow it.
        if (op.kind == sdf::PATHPULSE || op.kind == sdf::PATHPULSEPERCENT) {
            c.warn(&op, "PATHPULSE / PATHPULSEPERCENT: pulse limits are not modeled (pulses shorter than the delay are rejected)");
            continue;
        }
        for (const std::string& inst : targets(c, op)) {
            switch (op.kind) {
                case sdf::IOPATH: iopath(c, op, inst); break;
                case sdf::DEVICE: device(c, op, inst); break;
                case sdf::PORT:
                case sdf::NETDELAY:
                case sdf::INTERCONNECT: interconnect(c, op, inst); break;
                case sdf::TCHK: timingCheck(c, op, inst); break;
                case sdf::LABEL: label(c, op, inst); break;
                default: break;
            }
        }
    }
    if (c.log.is_open()) c.log << c.annotated << " annotations, " << c.warnings << " warnings\n";
    return c.warnings;
}

// $sdf_annotate with arguments known only when it runs.
inline void hook(const std::vector<std::string>& a) {
    SdfRequest r;
    std::string* f[] = {&r.file, &r.scope, &r.config, &r.log, &r.mtm, &r.scale, &r.scaleType};
    for (size_t k = 0; k < a.size() && k < 7; k++) *f[k] = a[k];
    annotate(r, false);
    for (auto& [sg, d] : S.icDelay)
        if ((size_t)sg < S.timedOut.size()) S.timedOut[(size_t)sg] = 1;
}

inline bool condHolds(const E& c) {   // 30.4.4.1: x or z counts as true
    Val v = eval(c, rd);
    if (v.x) return true;
    return v.v != 0 || !v.hi.empty();
}

}   // namespace sdfa

// Before time 0: the $sdf_annotate calls with known arguments and msim
// -sdf, in order; then the path tables.
inline int startSdf() {
    int bad = 0;
    for (auto& r : sdfRequests())
        if (sdfa::annotate(r, true) < 0) bad++;
    g_sdfHook = sdfa::hook;
    S.pathCond = sdfa::condHolds;
    S.setupPaths();
    return bad;
}
