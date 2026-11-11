// mchar - cell library characterization, in the manner of PrimeLib,
// SiliconSmart or Liberate.
//
//   mchar -lib template.lib -spice cells.sp -models corner.sp -o out.lib
//
// The template is a Liberty file that says what to measure: the cells, their
// pins and functions, the timing arcs and the table indexes. mchar simulates
// every cell's transistor netlist on mspice (src/spice.hpp) and replaces the
// numbers: delay and transition tables, setup, hold, recovery and removal
// tables, internal energy, pin capacitance and leakage. Everything else in
// the template is written back unchanged. Liberty is read and written with
// the shared reader (src/liberty.hpp); the supply and the temperature come
// from the template's operating conditions.
//
// The stimulus for an arc is found from the Liberty functions alone: a
// search over single-pin changes for the shortest sequence that ends with
// the related pin's edge moving the output (or, for a constraint, with the
// clock edge capturing a value that depends on the constrained pin).
//
// Every table point is one independent simulation; they run on all logical
// CPUs (-threads N) and are stored by index, so the output is the same for
// any thread count. See docs/CHAR.md.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "liberty.hpp"
#include "parallel.hpp"
#include "spice.hpp"
#include "version.hpp"

namespace {

using liberty::Attr;
using liberty::Expr;
using liberty::Group;

std::string lowerOf(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ------------------------------------------------------------- options

struct Options {
    std::string lib, out, spiceFile;
    std::vector<std::string> models;
    std::vector<std::string> cells;   // names or prefixes ending in *
    std::set<std::string> keep;       // timing constraints power caps leakage
    unsigned threads = 0;
    double settle = 0.5e-9;            // s between two stimulus changes
    double range = 1e-9;              // s, constraint search range
    double degrade = 0.10;            // a constraint is where the clock-to-output delay grows by this
    double reltol = 1e-4;
    bool quiet = false;
};

// ------------------------------------------------------- the logic model

// What the Liberty cell group says the cell does: enough to plan stimulus.
struct Logic {
    std::vector<std::string> ins;                 // input pins, bit i of a vector
    std::vector<std::string> outs;
    std::vector<Expr> fn, tri;                    // per output: function, three_state
    std::vector<bool> hasFn, hasTri;
    std::string iq, iqn;
    bool ff = false, latch = false;
    Expr clockedOn, nextState, enable, dataIn, clear, preset;
    bool hasClear = false, hasPreset = false;
    int track = -1;                               // the output whose held level a State follows

    int in(const std::string& n) const {
        for (size_t i = 0; i < ins.size(); i++) if (ins[i] == n) return (int)i;
        return -1;
    }
    int out(const std::string& n) const {
        for (size_t i = 0; i < outs.size(); i++) if (outs[i] == n) return (int)i;
        return -1;
    }
    bool eval(const Expr& e, uint32_t v, bool q) const {
        return e.eval([&](const std::string& n) {
            if (n == iq) return q;
            if (n == iqn) return !q;
            int i = in(n);
            return i >= 0 && ((v >> i) & 1);
        });
    }
    // The state after the inputs go from ov to nv.
    bool step(uint32_t ov, uint32_t nv, bool q) const {
        if (ff && !eval(clockedOn, ov, q) && eval(clockedOn, nv, q)) q = eval(nextState, ov, q);
        if (latch && eval(enable, nv, q)) q = eval(dataIn, nv, q);
        if (hasPreset && eval(preset, nv, q)) q = true;
        if (hasClear && eval(clear, nv, q)) q = false;
        return q;
    }
    bool outv(int o, uint32_t v, bool q) const { return eval(fn[o], v, q); }
    bool driven(int o, uint32_t v, bool q) const { return !hasTri[o] || !eval(tri[o], v, q); }
};

Logic logicOf(const Group& cell) {
    Logic L;
    for (const Group* p : cell.all("pin")) {
        std::string dir = p->str("direction");
        if (dir == "input") L.ins.push_back(p->name());
        else if (dir == "output" || dir == "inout") {
            L.outs.push_back(p->name());
            L.hasFn.push_back(p->has("function"));
            L.fn.push_back(p->has("function") ? liberty::parseFunction(p->str("function")) : Expr());
            L.hasTri.push_back(p->has("three_state"));
            L.tri.push_back(p->has("three_state") ? liberty::parseFunction(p->str("three_state")) : Expr());
        }
    }
    if (const Group* f = cell.first("ff")) {
        L.ff = true;
        L.iq = f->names.size() > 0 ? f->names[0] : "";
        L.iqn = f->names.size() > 1 ? f->names[1] : "";
        L.clockedOn = liberty::parseFunction(f->str("clocked_on"));
        L.nextState = liberty::parseFunction(f->str("next_state"));
        if ((L.hasClear = f->has("clear"))) L.clear = liberty::parseFunction(f->str("clear"));
        if ((L.hasPreset = f->has("preset"))) L.preset = liberty::parseFunction(f->str("preset"));
    } else if (const Group* l = cell.first("latch")) {
        L.latch = true;
        L.iq = l->names.size() > 0 ? l->names[0] : "";
        L.iqn = l->names.size() > 1 ? l->names[1] : "";
        L.enable = liberty::parseFunction(l->str("enable", "0"));
        L.dataIn = liberty::parseFunction(l->str("data_in", "0"));
        if ((L.hasClear = l->has("clear"))) L.clear = liberty::parseFunction(l->str("clear"));
        if ((L.hasPreset = l->has("preset"))) L.preset = liberty::parseFunction(l->str("preset"));
    }
    return L;
}

// A stimulus state: the input vector and the state for both power-up values.
struct State {
    uint32_t v = 0;
    bool a = false, b = true;
    int held = 2;                     // Logic::track's level, kept on its load while undriven: 0, 1, 2 unknown
    uint32_t key() const { return v << 4 | (uint32_t)held << 2 | (a ? 2u : 0u) | (b ? 1u : 0u); }
};

int heldAfter(const Logic& L, const State& n, int before) {
    if (L.track < 0) return 2;
    if (!L.driven(L.track, n.v, n.a) && !L.driven(L.track, n.v, n.b)) return before;
    if (n.a != n.b || !L.driven(L.track, n.v, n.a)) return 2;
    return L.outv(L.track, n.v, n.a) ? 1 : 0;
}

State flip(const Logic& L, State s, int pin) {
    State n;
    n.v = s.v ^ (1u << pin);
    n.a = L.step(s.v, n.v, s.a);
    n.b = L.step(s.v, n.v, s.b);
    n.held = heldAfter(L, n, s.held);
    return n;
}

// Breadth-first over single-pin changes from "all inputs low": the shortest
// list of pins to flip that reaches a state `goal` accepts. The search never
// flips a pin goal() would then have to flip back. False if there is none.
template <class Goal>
bool plan(const Logic& L, Goal&& goal, std::vector<int>& flips, State& at) {
    State s0;
    s0.a = L.step(0, 0, false);
    s0.b = L.ff || L.latch ? L.step(0, 0, true) : s0.a;   // no state: one copy
    s0.held = heldAfter(L, s0, 2);
    std::map<uint32_t, std::pair<uint32_t, int>> from;   // key -> (previous key, pin)
    std::map<uint32_t, State> seen;
    std::vector<State> q{s0};
    seen[s0.key()] = s0;
    from[s0.key()] = {s0.key(), -1};
    for (size_t h = 0; h < q.size(); h++) {
        State s = q[h];
        if (goal(s)) {
            at = s;
            flips.clear();
            for (uint32_t k = s.key(); from[k].second >= 0; k = from[k].first) flips.push_back(from[k].second);
            std::reverse(flips.begin(), flips.end());
            return true;
        }
        for (int p = 0; p < (int)L.ins.size(); p++) {
            State n = flip(L, s, p);
            if (seen.count(n.key())) continue;
            seen[n.key()] = n;
            from[n.key()] = {s.key(), p};
            q.push_back(n);
        }
    }
    return false;
}

// --------------------------------------------------------------- tables

struct TableRef {
    Group* g = nullptr;                 // cell_rise (...) { ... }
    std::vector<double> i1, i2;         // library units
    liberty::Var v1 = liberty::Var::None, v2 = liberty::Var::None;
    size_t rows() const { return std::max<size_t>(1, i1.size()); }
    size_t cols() const { return std::max<size_t>(1, i2.size()); }
};

struct Templates {
    std::map<std::string, const Group*> byName;
    explicit Templates(const Group& lib) {
        for (auto& g : lib.groups)
            if (g.type == "lu_table_template" || g.type == "power_lut_template") byName[g.name()] = &g;
    }
};

bool tableRef(Group& g, const Templates& T, TableRef& r) {
    r.g = &g;
    auto it = T.byName.find(g.name());
    const Group* t = it == T.byName.end() ? nullptr : it->second;
    auto idx = [&](const char* n) {
        const Attr* a = g.attr(n);
        if (!a && t) a = t->attr(n);
        return a && !a->vals.empty() ? liberty::numbers(a->vals[0]) : std::vector<double>();
    };
    r.i1 = idx("index_1");
    r.i2 = idx("index_2");
    if (t) {
        r.v1 = liberty::varOf(t->str("variable_1"));
        r.v2 = liberty::varOf(t->str("variable_2"));
    }
    return !r.i1.empty();
}

std::string num(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.5f", v);
    std::string s = b;
    if (std::fabs(v) < 1e-3 && v != 0) {
        std::snprintf(b, sizeof b, "%.4g", v);
        return b;
    }
    return s;
}

void setValues(TableRef& r, const std::vector<double>& v) {
    Attr* a = nullptr;
    for (auto& x : r.g->attrs) if (x.name == "values") a = &x;
    if (!a) {
        r.g->attrs.push_back({});
        a = &r.g->attrs.back();
        a->name = "values";
    }
    a->complex = true;
    a->vals.clear();
    for (size_t i = 0; i < r.rows(); i++) {
        std::string row;
        for (size_t j = 0; j < r.cols(); j++) row += (j ? ", " : "") + num(v[i * r.cols() + j]);
        a->vals.push_back(row);
    }
}

void setAttr(Group& g, const std::string& name, const std::string& v) {
    for (auto& a : g.attrs)
        if (a.name == name) { a.vals = {v}; return; }
    Attr a;
    a.name = name;
    a.vals = {v};
    g.attrs.push_back(a);
}

Group* child(Group& g, const std::string& type) {
    for (auto& c : g.groups) if (c.type == type) return &c;
    return nullptr;
}

// ----------------------------------------------------------- simulation

struct Tech {
    double vdd = 1, temp = 25;
    double tUnit = 1e-9, cUnit = 1e-15;     // one library unit in s and F
    double eUnit = 1e-15, leakUnit = 1e-9;  // J and W
    double sLoR = 0.2, sHiR = 0.8, sLoF = 0.2, sHiF = 0.8;
    double inR = 0.5, inF = 0.5, outR = 0.5, outF = 0.5, derate = 1;
    // The 0-to-100% ramp time of an input with this Liberty slew (s).
    double ramp(double slew, bool rise) const {
        return slew * derate / (rise ? sHiR - sLoR : sHiF - sLoF);
    }
};

// The largest time step for an event with this ramp: mspice's own step
// control does not see the charge on femtofarad nodes.
inline double stepFor(double ramp) { return std::clamp(0.25 * ramp + 1e-12, 1e-12, 20e-12); }

struct Wave {
    std::vector<std::pair<std::string, std::string>> pts;   // time and value expressions
};

// One deck: the cell, a supply, a PWL source on every input and a capacitor
// on every output. Times and loads are expressions in the deck's
// parameters, which each run overrides.
struct Deck {
    std::string cell;
    std::vector<std::string> ins, outs;
    std::map<std::string, Wave> wave;
    std::map<std::string, std::string> load;        // output -> F expression
    std::map<std::string, double> params;           // defaults
    std::string tstop = "tstop";
    bool euler = false;                             // backward Euler: no trapezoidal ringing in a quiet current
};

struct Runner {
    const Options& opt;
    const Tech& tech;

    spice::Netlist read(const Deck& d, size_t id) const {
        std::ostringstream f;
        {
            f << "mchar " << d.cell << "\n";
            for (auto& m : opt.models) f << ".include \"" << m << "\"\n";
            f << ".include \"" << opt.spiceFile << "\"\n";
            f << ".temp " << tech.temp << "\n.option reltol=" << opt.reltol << " chgtol=1e-17 trtol=1" << (d.euler ? " method=euler" : "") << "\n";
            for (auto& [k, v] : d.params) {
                char b[64];
                std::snprintf(b, sizeof b, "%.12g", v);
                f << ".param " << k << "=" << b << "\n";
            }
            f << "vvdd vdd 0 " << tech.vdd << "\n";
            for (auto& p : d.ins) {
                f << "v_" << lowerOf(p) << " " << lowerOf(p) << " 0 pwl(";
                for (auto& [t, v] : d.wave.at(p).pts) f << " {" << t << "} {" << v << "}";
                f << " )\n";
            }
            for (auto& o : d.outs) f << "c_" << lowerOf(o) << " " << lowerOf(o) << " 0 {" << d.load.at(o) << "}\n";
            f << "x1";
            for (auto& p : d.ins) f << " " << lowerOf(p);
            for (auto& o : d.outs) f << " " << lowerOf(o);
            f << " vdd 0 " << lowerOf(d.cell) << "\n";
            f << ".tran {(" << d.tstop << ")/40} {" << d.tstop << "} 0 {tmax}\n";
            f << ".print tran i(vvdd)";
            for (auto& p : d.ins) f << " v(" << lowerOf(p) << ") i(v_" << lowerOf(p) << ")";
            for (auto& o : d.outs) f << " v(" << lowerOf(o) << ")";
            f << "\n.end\n";
        }
        return spice::Reader().readText(f.str(), "mchar deck " + std::to_string(id) + " (" + d.cell + ")");
    }
};

// A transient result with the measurements the tables need.
struct Trace {
    spice::Result r;
    int col(const std::string& label) const {
        int c = r.col(label);
        if (c < 0) throw std::runtime_error("no column " + label);
        return c;
    }
    int v(const std::string& pin) const { return col("v(" + lowerOf(pin) + ")"); }
    double last(int c) const { return r.y.back()[c]; }
    double at(int c, double t) const { return r.at(c, t); }
    // First time after `from` the column crosses `level` going up or down; NaN if never.
    double cross(int c, double level, bool up, double from) const {
        for (size_t k = 1; k < r.x.size(); k++) {
            if (r.x[k] < from) continue;
            double a = r.y[k - 1][c], b = r.y[k][c];
            if (up ? (a < level && b >= level) : (a > level && b <= level)) {
                double t = r.x[k - 1] + (r.x[k] - r.x[k - 1]) * (level - a) / (b - a);
                if (t >= from) return t;
            }
        }
        return NAN;
    }
    double integral(int c, double t0, double t1) const {
        double s = 0;
        for (size_t k = 1; k < r.x.size(); k++) {
            double a = std::max(r.x[k - 1], t0), b = std::min(r.x[k], t1);
            if (b <= a) continue;
            s += 0.5 * (at(c, a) + at(c, b)) * (b - a);
        }
        return s;
    }
};

Trace run(const spice::Netlist& nl, const std::map<std::string, double>& over) {
    spice::RunOutput o = spice::runNetlist(nl, over, nullptr);
    for (auto& r : o.results)
        if (r.kind == "tran" && !r.x.empty()) {
            Trace t;
            t.r = std::move(r);
            return t;
        }
    throw std::runtime_error("no transient result");
}

// The inputs' waveforms for "start with every input low, flip these pins
// one after another, a settle time apart". Returns the time the last flip
// has settled: where the measured event may start.
std::string stimulus(const Logic& L, const std::vector<int>& flips, Deck& d) {
    std::vector<bool> lvl(L.ins.size(), false);
    for (auto& p : L.ins) d.wave[p].pts = {{"0", "0"}};
    int k = 0;
    for (int p : flips) {
        k++;
        auto& w = d.wave[L.ins[p]].pts;
        std::string t = std::to_string(k) + "*ts";
        w.push_back({t, lvl[p] ? "vdd" : "0"});
        lvl[p] = !lvl[p];
        w.push_back({t + "+tedge", lvl[p] ? "vdd" : "0"});
    }
    return std::to_string(k + 1) + "*ts+tpre";
}

// ------------------------------------------------------------------ jobs

// One table point's result.
struct Point {
    double delay = NAN, trans = NAN, energy = NAN, value = NAN;
    std::string err;
};

struct ArcJob {       // one output edge of a delay arc
    Group* timing = nullptr;
    std::string out, related, when, type;
    bool rise = false;                      // the output edge
    TableRef delay, trans;
    std::vector<Point> pts[2];              // per input direction tried (non-unate: both)
    bool tried[2] = {false, false};
    std::string err;
    int toggling = 1;                       // outputs that move with this one and share the energy
};

struct ConsJob {      // one constraint table
    Group* timing = nullptr;
    std::string pin, related, type;
    bool rise = false;                      // the constrained pin's edge
    TableRef tab;
    std::vector<Point> pts;
    std::string err;
};

struct PulseJob {     // min_pulse_width_high or _low of a pin
    Group* pin = nullptr;
    bool high = false;
    Point pt;
};

struct CapJob {
    Group* pin = nullptr;
    Point pt;
};

struct LeakJob {
    Group* group = nullptr;                 // leakage_power group, or null for the cell value
    Point pt;
};

struct CellWork {
    Group* g = nullptr;
    Logic L;
    std::vector<ArcJob> arcs;
    std::vector<ConsJob> cons;
    std::vector<PulseJob> pulses;
    std::vector<CapJob> caps;
    std::vector<LeakJob> leaks;
    int kept = 0;                           // timing groups left as the template has them
    std::vector<std::string> notes;
};

struct Char {
    const Options& opt;
    Tech tech;
    Runner runner;
    double minSlew = 0, minLoad = 0;        // library units

    Deck deck(const CellWork& w) const {
        Deck d;
        d.cell = w.g->name();
        d.ins = w.L.ins;
        d.outs = w.L.outs;
        for (auto& o : d.outs) d.load[o] = "cmin";
        d.params = {{"vdd", tech.vdd}, {"ts", opt.settle}, {"tedge", 20e-12}, {"cmin", minLoad * tech.cUnit},
                    {"cl", minLoad * tech.cUnit}, {"tr", 20e-12}, {"tstop", 1e-9}, {"trc", 20e-12}, {"off", 0}, {"tmax", 5e-12}, {"tpre", 0}};
        return d;
    }

    // ------------------------------------------------------ delay arcs
    // Input direction pd (true: rising) for output edge `rise` of this arc.
    bool arcPlan(const CellWork& w, const ArcJob& j, bool pd, std::vector<int>& flips, State& at) const {
        int p = w.L.in(j.related), o = w.L.out(j.out);
        if (p < 0 || o < 0 || !w.L.hasFn[o]) return false;
        Expr when;
        bool hasWhen = !j.when.empty();
        if (hasWhen) when = liberty::parseFunction(j.when);
        if (j.type == "three_state_enable") {
            // The output starts undriven with the other level left on its
            // load, and the pin's edge makes the cell drive it.
            Logic L = w.L;
            L.track = o;
            return plan(L, [&](const State& s) {
                if ((((s.v >> p) & 1) != 0) == pd) return false;
                State n = flip(L, s, p);
                if (s.a != s.b || n.a != n.b) return false;
                if (L.driven(o, s.v, s.a) || !L.driven(o, n.v, n.a)) return false;
                if (hasWhen && (!L.eval(when, s.v, s.a) || !L.eval(when, n.v, n.a))) return false;
                return s.held == (j.rise ? 0 : 1) && L.outv(o, n.v, n.a) == j.rise;
            }, flips, at);
        }
        const Logic& L = w.L;
        return plan(L, [&](const State& s) {
            if ((((s.v >> p) & 1) != 0) == pd) return false;        // the pin must be at its old level
            State n = flip(L, s, p);
            if (s.a != s.b || n.a != n.b) return false;
            if (!L.driven(o, s.v, s.a) || !L.driven(o, n.v, n.a)) return false;
            if (hasWhen && (!L.eval(when, s.v, s.a) || !L.eval(when, n.v, n.a))) return false;
            return L.outv(o, s.v, s.a) != j.rise && L.outv(o, n.v, n.a) == j.rise;
        }, flips, at);
    }

    void arcPoint(const CellWork& w, ArcJob& j, int dirIdx, size_t idx, size_t deckId) const {
        Point& pt = j.pts[dirIdx][idx];
        try {
            bool pd = dirIdx == 1;
            std::vector<int> flips;
            State at;
            if (!arcPlan(w, j, pd, flips, at)) throw std::runtime_error("no stimulus");
            const Logic& L = w.L;
            Deck d = deck(w);
            std::string te = stimulus(L, flips, d);
            auto& wv = d.wave[j.related].pts;
            wv.push_back({te, pd ? "0" : "vdd"});
            wv.push_back({te + "+tr", pd ? "vdd" : "0"});
            d.load[j.out] = "cl";
            d.tstop = te + "+tr+twin";
            d.params["twin"] = 1e-9;
            spice::Netlist nl = runner.read(d, deckId);

            size_t i = idx / j.delay.cols(), k = idx % j.delay.cols();
            double slew = (j.delay.v1 == liberty::Var::Load ? j.delay.i2[k] : j.delay.i1[i]) * tech.tUnit;
            double load = (j.delay.v1 == liberty::Var::Load ? j.delay.i1[i] : j.delay.i2[k]) * tech.cUnit;
            double tr = tech.ramp(slew, pd);
            // The loaded output has to be at rest before the edge: wait for it.
            double tpre = 3e4 * load;
            double twin = 4 * tr + 0.2e-9 + 2e4 * load;       // a first guess: 20 kohm into the load
            std::map<std::string, double> over = {{"tr", tr}, {"cl", load}, {"tmax", stepFor(tr)}};
            for (int attempt = 0;; attempt++) {
                double t0 = (double)(flips.size() + 1) * opt.settle + tpre;
                over["twin"] = twin;
                over["tpre"] = tpre;
                Trace t = run(nl, over);
                int co = t.v(j.out);
                double V = tech.vdd;
                double tin = t0 + tr * (pd ? tech.inR : 1 - tech.inF);
                double lo = (j.rise ? tech.sLoR : tech.sLoF) * V, hi = (j.rise ? tech.sHiR : tech.sHiF) * V;
                double start = t.at(co, t0);
                double rest = j.type == "three_state_enable" ? 0.05 : 0.001;   // an undriven load drifts
                if (j.rise ? start > rest * V : start < (1 - rest) * V) {
                    if (attempt >= 7) throw std::runtime_error("the output did not settle before the edge");
                    tpre = 3 * tpre + 0.2e-9;
                    continue;
                }
                double tout = t.cross(co, (j.rise ? tech.outR : tech.outF) * V, j.rise, t0);
                double ta = t.cross(co, j.rise ? lo : hi, j.rise, t0);
                double tb = t.cross(co, j.rise ? hi : lo, j.rise, t0);
                double end = t.last(co);
                bool done = std::isfinite(tout) && std::isfinite(ta) && std::isfinite(tb) &&
                            (j.rise ? end > 0.995 * V : end < 0.005 * V);
                if (!done) {
                    if (attempt >= 7) throw std::runtime_error("the output did not switch");
                    twin *= 3;
                    continue;
                }
                pt.delay = (tout - tin) / tech.tUnit;
                pt.trans = (tb - ta) / tech.derate / tech.tUnit;
                // Energy from the supply over the event, less what stays on
                // the loads of the outputs that rose.
                double tEnd = t.r.x.back();
                double e = -V * t.integral(t.col("i(vvdd)"), t0, tEnd);
                double q0 = t0 - 0.3 * opt.settle;
                e -= -V * t.integral(t.col("i(vvdd)"), q0, t0) / (t0 - q0) * (tEnd - t0);   // leakage
                for (size_t o = 0; o < L.outs.size(); o++) {
                    int c = t.v(L.outs[o]);
                    double dv = t.last(c) - t.at(c, t0);
                    if (dv > 0) e -= (L.outs[o] == j.out ? load : minLoad * tech.cUnit) * V * dv;
                }
                pt.energy = e / j.toggling / tech.eUnit;
                break;
            }
        } catch (const std::exception& e) {
            pt.err = e.what();
        }
    }

    // ----------------------------------------------------- constraints
    struct ConsPlan {
        std::vector<int> flips;
        State at;
        int out = -1;
        bool expect = false;      // the output's final value when the constraint is met
        bool moves = false;       // meeting it shows as an output edge after the clock edge
    };

    static bool setupLike(const std::string& t) { return t.rfind("setup", 0) == 0 || t.rfind("recovery", 0) == 0; }

    bool consPlan(const CellWork& w, const ConsJob& j, ConsPlan& cp) const {
        const Logic& L = w.L;
        int p = L.in(j.pin), c = L.in(j.related);
        if (p < 0 || c < 0 || p == c) return false;
        bool ce = j.type.find("rising") != std::string::npos;
        bool setup = setupLike(j.type);
        for (int needMove = 1; needMove >= 0; needMove--) {
            bool ok = plan(L, [&](const State& s) {
                if ((((s.v >> p) & 1) != 0) == j.rise) return false;      // pin at its old level
                if ((((s.v >> c) & 1) != 0) == ce) return false;          // clock before its edge
                if (s.a != s.b) return false;
                State s1 = flip(L, s, p), a2 = flip(L, s1, c);            // pin, then clock
                State b1 = flip(L, s, c), b2 = flip(L, b1, p);            // clock, then pin
                if (a2.a != a2.b || b2.a != b2.b || s1.a != s1.b || b1.a != b1.b) return false;
                for (int o = 0; o < (int)L.outs.size(); o++) {
                    if (!L.hasFn[o] || L.hasTri[o]) continue;
                    bool va = L.outv(o, a2.v, a2.a), vb = L.outv(o, b2.v, b2.a);
                    if (va == vb) continue;
                    // Met: setup-like follows "pin first", hold-like "clock first".
                    bool before = setup ? L.outv(o, s1.v, s1.a) : L.outv(o, s.v, s.a);
                    bool afterClk = setup ? va : L.outv(o, b1.v, b1.a);
                    bool moves = before != afterClk && afterClk == (setup ? va : vb);
                    if (needMove && !moves) continue;
                    cp.out = o;
                    cp.expect = setup ? va : vb;
                    cp.moves = moves;
                    return true;
                }
                return false;
            }, cp.flips, cp.at);
            if (ok) return true;
        }
        return false;
    }

    void consPoint(const CellWork& w, ConsJob& j, size_t idx, size_t deckId) const {
        Point& pt = j.pts[idx];
        try {
            ConsPlan cp;
            if (!consPlan(w, j, cp)) throw std::runtime_error("no stimulus");
            const Logic& L = w.L;
            bool ce = j.type.find("rising") != std::string::npos;
            bool setup = setupLike(j.type);
            Deck d = deck(w);
            std::string te = stimulus(L, cp.flips, d);
            // The clock's 50% point is at tc; the pin's is `off` before it
            // (setup, recovery) or after it (hold, removal).
            std::string tc = te + "+range+tr+trc";
            std::string tp = setup ? tc + "-off" : tc + "+off";
            auto& wp = d.wave[j.pin].pts;
            wp.push_back({tp + "-tr/2", j.rise ? "0" : "vdd"});
            wp.push_back({tp + "+tr/2", j.rise ? "vdd" : "0"});
            auto& wc = d.wave[j.related].pts;
            wc.push_back({tc + "-trc/2", ce ? "0" : "vdd"});
            wc.push_back({tc + "+trc/2", ce ? "vdd" : "0"});
            d.params["range"] = opt.range;
            d.params["twin"] = 1e-9;
            d.tstop = tc + "+range+tr+trc+twin";
            spice::Netlist nl = runner.read(d, deckId);

            size_t i = idx / j.tab.cols(), k = idx % j.tab.cols();
            auto var = [&](liberty::Var v) { return (j.tab.v1 == v ? j.tab.i1[i] : j.tab.i2[k]) * tech.tUnit; };
            bool relFirst = j.tab.v1 == liberty::Var::RelSlew || j.tab.v2 == liberty::Var::ConSlew ||
                            (j.tab.v1 == liberty::Var::None && j.tab.v2 == liberty::Var::None);
            double clkSlew = relFirst ? j.tab.i1[i] * tech.tUnit : var(liberty::Var::RelSlew);
            double pinSlew = relFirst ? (j.tab.i2.empty() ? j.tab.i1[i] : j.tab.i2[k]) * tech.tUnit
                                      : var(liberty::Var::ConSlew);
            double tr = tech.ramp(pinSlew, j.rise), trc = tech.ramp(clkSlew, ce);
            double tcv = (double)(cp.flips.size() + 1) * opt.settle + opt.range + tr + trc;
            std::map<std::string, double> over = {{"tr", tr}, {"trc", trc}, {"tmax", stepFor(std::min(tr, trc))}};
            const std::string outName = L.outs[cp.out];
            double V = tech.vdd;
            // Does the cell behave as if the constraint were met, at this offset?
            double ref = NAN;
            auto met = [&](double off) {
                over["off"] = off;
                Trace t = run(nl, over);
                int co = t.v(outName);
                double end = t.last(co);
                if ((end > 0.5 * V) != cp.expect) return false;
                if (cp.expect ? end < 0.95 * V : end > 0.05 * V) return false;
                if (!cp.moves) {
                    // No edge to time: the output must not have left its rail on the way.
                    double worst = 0;
                    int cc = co;
                    for (size_t q = 0; q < t.r.x.size(); q++)
                        if (t.r.x[q] > tcv - trc) worst = std::max(worst, std::fabs(t.r.y[q][cc] - (cp.expect ? V : 0)));
                    return setup ? true : worst < 0.5 * V;
                }
                double tx = t.cross(co, 0.5 * V, cp.expect, tcv - trc / 2);
                if (!std::isfinite(tx)) return false;
                double dly = tx - tcv;
                if (!std::isfinite(ref)) ref = dly;
                return dly <= ref * (1 + opt.degrade) + 1e-13;
            };
            double hi = opt.range, lo = -opt.range;
            if (!met(hi)) throw std::runtime_error("not met even " + num(opt.range / tech.tUnit) + " away from the clock");
            if (met(lo)) {
                pt.value = lo / tech.tUnit;
                return;
            }
            double tol = std::max(2e-13, 1e-3 * std::min(tr, trc));
            while (hi - lo > tol) {
                double mid = 0.5 * (hi + lo);
                (met(mid) ? hi : lo) = mid;
            }
            pt.value = hi / tech.tUnit;
        } catch (const std::exception& e) {
            pt.err = e.what();
        }
    }

    // ------------------------------------------------ min pulse width
    struct PulsePlan {
        std::vector<int> flips;
        int out = -1;
        bool expect = false;      // the output's value after the pulse
        bool leading = false;     // it moves at the pulse's first edge
    };

    bool pulsePlan(const CellWork& w, const PulseJob& j, PulsePlan& pp) const {
        const Logic& L = w.L;
        int p = L.in(j.pin->name());
        if (p < 0) return false;
        State at;
        return plan(L, [&](const State& s) {
            if ((((s.v >> p) & 1) != 0) == j.high) return false;      // the pin idles at the other level
            State s1 = flip(L, s, p), s2 = flip(L, s1, p);
            if (s.a != s.b || s1.a != s1.b || s2.a != s2.b) return false;
            for (int o = 0; o < (int)L.outs.size(); o++) {
                if (!L.hasFn[o] || L.hasTri[o]) continue;
                bool v0 = L.outv(o, s.v, s.a), v1 = L.outv(o, s1.v, s1.a), v2 = L.outv(o, s2.v, s2.a);
                if (v2 == v0) continue;                               // the pulse leaves no mark here
                pp.out = o;
                pp.expect = v2;
                pp.leading = v1 == v2;
                return true;
            }
            return false;
        }, pp.flips, at);
    }

    // The narrowest pulse (between the input thresholds) after which the
    // cell ends where a wide pulse leaves it, no more than `degrade` later.
    void pulsePoint(const CellWork& w, PulseJob& j, size_t deckId) const {
        try {
            PulsePlan pp;
            if (!pulsePlan(w, j, pp)) throw std::runtime_error("no stimulus");
            const Logic& L = w.L;
            std::string pin = j.pin->name();
            Deck d = deck(w);
            std::string te = stimulus(L, pp.flips, d);
            const char* idle = j.high ? "0" : "vdd";
            const char* active = j.high ? "vdd" : "0";
            auto& wv = d.wave[pin].pts;
            wv.push_back({te, idle});
            wv.push_back({te + "+tr", active});
            wv.push_back({te + "+pw", active});
            wv.push_back({te + "+pw+tr", idle});
            d.params["pw"] = opt.range;
            d.tstop = te + "+pw+tr+2*ts";
            spice::Netlist nl = runner.read(d, deckId);
            double tr = tech.ramp(4 * minSlew * tech.tUnit, true);
            double t0 = (double)(pp.flips.size() + 1) * opt.settle;
            double V = tech.vdd;
            std::map<std::string, double> over = {{"tr", tr}, {"tmax", stepFor(tr)}};
            const std::string outName = L.outs[pp.out];
            double ref = NAN;
            auto met = [&](double pw) {
                over["pw"] = pw;
                Trace t = run(nl, over);
                int co = t.v(outName);
                double end = t.last(co);
                if (pp.expect ? end < 0.95 * V : end > 0.05 * V) return false;
                double edge = t0 + 0.5 * tr + (pp.leading ? 0 : pw);
                double tx = t.cross(co, 0.5 * V, pp.expect, t0);
                if (!std::isfinite(tx)) return false;
                if (!std::isfinite(ref)) ref = tx - edge;
                return tx - edge <= ref * (1 + opt.degrade) + 1e-13;
            };
            double hi = std::max(opt.range, 2 * tr), lo = 1.02 * tr;   // at `tr` the pulse just reaches the rail
            if (!met(hi)) throw std::runtime_error("no effect even with a pulse of " + num(hi / tech.tUnit));
            if (met(lo)) {
                j.pt.value = lo / tech.tUnit;
                return;
            }
            double tol = std::max(2e-13, 1e-3 * tr);
            while (hi - lo > tol) {
                double mid = 0.5 * (hi + lo);
                (met(mid) ? hi : lo) = mid;
            }
            j.pt.value = hi / tech.tUnit;
        } catch (const std::exception& e) {
            j.pt.err = e.what();
        }
    }

    // ------------------------------------------------- pin capacitance
    void capPoint(const CellWork& w, CapJob& j, size_t deckId) const {
        try {
            const Logic& L = w.L;
            std::string pin = j.pin->name();
            Deck d = deck(w);
            std::string te = stimulus(L, {}, d);
            auto& wv = d.wave[pin].pts;
            wv.push_back({te, "0"});
            wv.push_back({te + "+tr", "vdd"});
            wv.push_back({te + "+tr+ts", "vdd"});
            wv.push_back({te + "+2*tr+ts", "0"});
            d.tstop = te + "+2*tr+2*ts";
            spice::Netlist nl = runner.read(d, deckId);
            double tr = tech.ramp(4 * minSlew * tech.tUnit, true);
            Trace t = run(nl, {{"tr", tr}, {"tmax", stepFor(tr)}});
            int ci = t.col("i(v_" + lowerOf(pin) + ")");
            double t0 = opt.settle, mid = t0 + tr + opt.settle;
            double qr = std::fabs(t.integral(ci, t0, mid)), qf = std::fabs(t.integral(ci, mid, t.r.x.back()));
            j.pt.value = qr / tech.vdd / tech.cUnit;        // rise
            j.pt.delay = qf / tech.vdd / tech.cUnit;        // fall
        } catch (const std::exception& e) {
            j.pt.err = e.what();
        }
    }

    // --------------------------------------------------------- leakage
    void leakPoint(const CellWork& w, LeakJob& j, size_t deckId) const {
        try {
            const Logic& L = w.L;
            // The states to average: those the group's `when` allows.
            Expr when;
            bool hasWhen = j.group && j.group->has("when");
            if (hasWhen) when = liberty::parseFunction(j.group->str("when"));
            double sum = 0;
            int n = 0;
            size_t states = (size_t)1 << L.ins.size();
            for (size_t v = 0; v < states && n < 16; v++) {
                if (hasWhen && !L.eval(when, (uint32_t)v, false)) continue;
                Deck d = deck(w);
                std::vector<int> flips;
                for (size_t b = 0; b < L.ins.size(); b++) if ((v >> b) & 1) flips.push_back((int)b);
                std::string te = stimulus(L, flips, d);
                d.tstop = te + "+ts";
                d.euler = true;
                Trace t = run(runner.read(d, deckId), {});
                double end = t.r.x.back(), from = end - 0.3 * opt.settle;
                sum += std::fabs(t.integral(t.col("i(vvdd)"), from, end)) / (end - from) * tech.vdd;
                n++;
            }
            if (!n) throw std::runtime_error("no input state satisfies the condition");
            j.pt.value = sum / n / tech.leakUnit;
        } catch (const std::exception& e) {
            j.pt.err = e.what();
        }
    }
};

bool wanted(const Options& opt, const std::string& name) {
    if (opt.cells.empty()) return true;
    for (auto& c : opt.cells) {
        if (c == name) return true;
        if (!c.empty() && c.back() == '*' && name.rfind(c.substr(0, c.size() - 1), 0) == 0) return true;
    }
    return false;
}

void usage() {
    std::cout <<
        "mchar - characterize a cell library on mspice\n"
        "usage: mchar -lib template.lib -spice cells.sp [-models corner.sp] -o out.lib [options]\n"
        "  -lib FILE       the Liberty template: cells, arcs and table indexes to fill in\n"
        "  -spice FILE     transistor netlists, one .subckt per cell: the cell's inputs, then\n"
        "                  its outputs, in the template's pin order, then vdd and vss\n"
        "  -models FILE    device models to include (may repeat)\n"
        "  -o FILE         the characterized Liberty file\n"
        "  -cell NAME      only this cell (may repeat; NAME* matches a prefix); the others keep\n"
        "                  the template's numbers\n"
        "  -keep WHAT      leave the template's numbers for: timing, constraints, power, caps,\n"
        "                  leakage (may repeat)\n"
        "  -settle T       time between stimulus changes, in ns (default 0.5)\n"
        "  -range T        how far from the clock edge a constraint is searched, in ns (default 1)\n"
        "  -degrade F      a constraint is where clock-to-output delay grows by this (default 0.1)\n"
        "  -threads N      worker threads (default: every logical CPU; 1 for none)\n"
        "  -q              print errors only\n"
        "  -version        print the version\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    opt.threads = par::defaultThreads();
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << "mchar: " << a << " needs " << what << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
        if (a == "-version" || a == "--version") {
            std::cout << "mchar " << MINISIM_VERSION << (MINISIM_GIT[0] ? " (" MINISIM_GIT ")" : "") << "\n";
            return 0;
        }
        if (par::threadsArg(argc, argv, i, opt.threads, "mchar")) continue;
        if (a == "-lib") opt.lib = next("a Liberty file");
        else if (a == "-spice") opt.spiceFile = next("a SPICE file");
        else if (a == "-models") opt.models.push_back(next("a SPICE file"));
        else if (a == "-o") opt.out = next("a file");
        else if (a == "-cell") opt.cells.push_back(next("a cell name"));
        else if (a == "-keep") {
            std::string k = next("timing, constraints, power, caps or leakage");
            if (k != "timing" && k != "constraints" && k != "power" && k != "caps" && k != "leakage") {
                std::cerr << "mchar: -keep takes timing, constraints, power, caps or leakage\n";
                return 2;
            }
            opt.keep.insert(k);
        } else if (a == "-settle") opt.settle = std::atof(next("a time in ns").c_str()) * 1e-9;
        else if (a == "-range") opt.range = std::atof(next("a time in ns").c_str()) * 1e-9;
        else if (a == "-degrade") opt.degrade = std::atof(next("a fraction").c_str());
        else if (a == "-q") opt.quiet = true;
        else { std::cerr << "mchar: unknown option " << a << "\n"; return 2; }
    }
    if (opt.lib.empty() || opt.spiceFile.empty() || opt.out.empty()) { usage(); return 2; }
    if (opt.settle <= 0 || opt.range <= 0 || opt.degrade <= 0) {
        std::cerr << "mchar: -settle, -range and -degrade must be positive\n";
        return 2;
    }
    namespace fs = std::filesystem;
    auto t0 = std::chrono::steady_clock::now();
    try {
        for (auto* f : {&opt.spiceFile}) *f = fs::absolute(*f).string();
        for (auto& m : opt.models) m = fs::absolute(m).string();
        for (auto& f : opt.models) if (!fs::exists(f)) throw std::runtime_error("cannot open " + f);
        if (!fs::exists(opt.spiceFile)) throw std::runtime_error("cannot open " + opt.spiceFile);

        liberty::Library lib = liberty::load(opt.lib);
        Group& root = *lib.root;
        Templates templates(root);

        Tech tech;
        tech.tUnit = lib.timeScale * 1e-9;
        tech.cUnit = lib.capScale * 1e-12;
        tech.leakUnit = lib.leakageScale * 1e-9;
        const liberty::OperatingConditions* oc = lib.conditions();
        tech.vdd = oc ? oc->voltage * lib.voltageScale : lib.nomVoltage;
        tech.temp = oc ? oc->temperature : lib.nomTemperature;
        if (!(tech.vdd > 0)) throw std::runtime_error(opt.lib + ": no supply voltage (operating_conditions or nom_voltage)");
        tech.eUnit = tech.cUnit * lib.voltageScale * lib.voltageScale;   // capacitance unit times volts squared
        tech.sLoR = lib.slewLowerRise / 100; tech.sHiR = lib.slewUpperRise / 100;
        tech.sLoF = lib.slewLowerFall / 100; tech.sHiF = lib.slewUpperFall / 100;
        tech.inR = lib.inputThresholdRise / 100; tech.inF = lib.inputThresholdFall / 100;
        tech.outR = lib.outputThresholdRise / 100; tech.outF = lib.outputThresholdFall / 100;
        tech.derate = lib.slewDerate > 0 ? lib.slewDerate : 1;

        Char ch{opt, tech, Runner{opt, tech}};

        // The subcircuits there are, to tell "no netlist" from a failure.
        std::set<std::string> subckts;
        {
            spice::Netlist nl = spice::Reader().read(opt.spiceFile);
            for (auto& [n, b] : nl.top.subckts) subckts.insert(n);
        }

        // ---- collect the work
        std::vector<CellWork> work;
        int noNetlist = 0;
        double minSlew = 1e300, minLoad = 1e300;
        for (auto& g : root.groups) {
            if (g.type != "cell" || !wanted(opt, g.name())) continue;
            if (!subckts.count(lowerOf(g.name()))) { noNetlist++; continue; }
            CellWork w;
            w.g = &g;
            w.L = logicOf(g);
            if (w.L.ins.size() > 16) { w.notes.push_back("more than 16 inputs: left as it is"); work.push_back(std::move(w)); continue; }
            for (auto& p : g.groups) {
                if (p.type != "pin") continue;
                bool isOut = w.L.out(p.name()) >= 0;
                if (!isOut && !opt.keep.count("caps") && p.has("capacitance")) w.caps.push_back({&p, {}});
                if (!isOut && !opt.keep.count("constraints"))
                    for (bool high : {true, false})
                        if (p.has(high ? "min_pulse_width_high" : "min_pulse_width_low")) w.pulses.push_back({&p, high, {}});
                for (auto& t : p.groups) {
                    if (t.type != "timing") continue;
                    std::string type = t.str("timing_type", "combinational");
                    std::string rel = t.str("related_pin");
                    bool cons = type.rfind("setup", 0) == 0 || type.rfind("hold", 0) == 0 ||
                                type.rfind("recovery", 0) == 0 || type.rfind("removal", 0) == 0;
                    bool delay = type == "combinational" || type == "combinational_rise" || type == "combinational_fall" ||
                                 type == "rising_edge" || type == "falling_edge" || type == "preset" || type == "clear" ||
                                 type == "three_state_enable";
                    if (rel.find(' ') != std::string::npos || (!cons && !delay)) { w.kept++; continue; }
                    if (cons) {
                        if (opt.keep.count("constraints")) continue;
                        for (bool rise : {true, false}) {
                            Group* tg = child(t, rise ? "rise_constraint" : "fall_constraint");
                            ConsJob j;
                            if (!tg || !tableRef(*tg, templates, j.tab)) continue;
                            j.timing = &t; j.pin = p.name(); j.related = rel; j.type = type; j.rise = rise;
                            j.pts.resize(j.tab.rows() * j.tab.cols());
                            w.cons.push_back(std::move(j));
                        }
                        continue;
                    }
                    for (bool rise : {true, false}) {
                        Group* dg = child(t, rise ? "cell_rise" : "cell_fall");
                        Group* tg = child(t, rise ? "rise_transition" : "fall_transition");
                        ArcJob j;
                        if (!dg || !tableRef(*dg, templates, j.delay)) continue;
                        if (tg) tableRef(*tg, templates, j.trans);
                        j.timing = &t; j.out = p.name(); j.related = rel; j.type = type; j.rise = rise;
                        j.when = t.str("when");
                        std::string sense = t.str("timing_sense", "non_unate");
                        // The related pin's edge: 1 rising, 0 falling.
                        if (type == "three_state_enable") j.tried[0] = j.tried[1] = true;   // whichever edge enables
                        else if (type == "rising_edge") j.tried[1] = true;
                        else if (type == "falling_edge") j.tried[0] = true;
                        else if (sense == "positive_unate") j.tried[rise ? 1 : 0] = true;
                        else if (sense == "negative_unate") j.tried[rise ? 0 : 1] = true;
                        else j.tried[0] = j.tried[1] = true;
                        for (int d = 0; d < 2; d++)
                            if (j.tried[d]) j.pts[d].resize(j.delay.rows() * j.delay.cols());
                        const auto& slews = j.delay.v1 == liberty::Var::Load ? j.delay.i2 : j.delay.i1;
                        const auto& loads = j.delay.v1 == liberty::Var::Load ? j.delay.i1 : j.delay.i2;
                        for (double s : slews) minSlew = std::min(minSlew, s);
                        for (double c : loads) minLoad = std::min(minLoad, c);
                        w.arcs.push_back(std::move(j));
                    }
                }
            }
            if (!opt.keep.count("leakage")) {
                for (auto& lg : g.groups)
                    if (lg.type == "leakage_power") w.leaks.push_back({&lg, {}});
                w.leaks.push_back({nullptr, {}});
            }
            work.push_back(std::move(w));
        }
        if (minSlew == 1e300) minSlew = 0.01e-9 / tech.tUnit;
        if (minLoad == 1e300) minLoad = 1e-15 / tech.cUnit;
        ch.minSlew = minSlew;
        ch.minLoad = minLoad;
        if (opt.keep.count("timing"))
            for (auto& w : work) w.arcs.clear();

        // How many outputs move together and have an internal_power group
        // for the same pin: they share the event's energy.
        for (auto& w : work)
            for (auto& j : w.arcs) {
                int n = 0;
                for (int d = 0; d < 2 && n == 0; d++) {
                    if (!j.tried[d]) continue;
                    std::vector<int> flips;
                    State at;
                    if (!ch.arcPlan(w, j, d == 1, flips, at)) continue;
                    State after = flip(w.L, at, w.L.in(j.related));
                    for (size_t o = 0; o < w.L.outs.size(); o++) {
                        if (!w.L.hasFn[o] || w.L.outv((int)o, at.v, at.a) == w.L.outv((int)o, after.v, after.a)) continue;
                        for (auto& pg : w.g->groups)
                            if (pg.type == "pin" && pg.name() == w.L.outs[o])
                                for (auto& ip : pg.groups)
                                    if (ip.type == "internal_power" && ip.str("related_pin") == j.related) { n++; break; }
                    }
                }
                j.toggling = std::max(1, n);
            }

        // ---- one task per simulation (a constraint point is one search)
        struct Task { int kind; size_t cell, job, dir, idx; };
        std::vector<Task> tasks;
        for (size_t c = 0; c < work.size(); c++) {
            auto& w = work[c];
            for (size_t j = 0; j < w.arcs.size(); j++)
                for (size_t d = 0; d < 2; d++)
                    for (size_t k = 0; k < w.arcs[j].pts[d].size(); k++) tasks.push_back({0, c, j, d, k});
            for (size_t j = 0; j < w.cons.size(); j++)
                for (size_t k = 0; k < w.cons[j].pts.size(); k++) tasks.push_back({1, c, j, 0, k});
            for (size_t j = 0; j < w.caps.size(); j++) tasks.push_back({2, c, j, 0, 0});
            for (size_t j = 0; j < w.leaks.size(); j++) tasks.push_back({3, c, j, 0, 0});
            for (size_t j = 0; j < w.pulses.size(); j++) tasks.push_back({4, c, j, 0, 0});
        }
        if (!opt.quiet)
            std::cout << "mchar " << MINISIM_VERSION << ": " << work.size() << " cells, " << tasks.size()
                      << " measurements, " << tech.vdd << " V, " << tech.temp << " C, " << opt.threads
                      << " thread" << (opt.threads == 1 ? "" : "s") << "\n";
        par::parallelFor(tasks.size(), opt.threads, [&](size_t i) {
            const Task& t = tasks[i];
            CellWork& w = work[t.cell];
            switch (t.kind) {
            case 0: ch.arcPoint(w, w.arcs[t.job], (int)t.dir, t.idx, i); break;
            case 1: ch.consPoint(w, w.cons[t.job], t.idx, i); break;
            case 2: ch.capPoint(w, w.caps[t.job], i); break;
            case 4: ch.pulsePoint(w, w.pulses[t.job], i); break;
            default: ch.leakPoint(w, w.leaks[t.job], i); break;
            }
        });

        // ---- write the numbers back, in order
        int nArc = 0, nCons = 0, nCap = 0, nLeak = 0, nPower = 0, nPulse = 0, failed = 0, kept = 0;
        std::vector<std::string> problems;
        for (auto& w : work) {
            const std::string cn = w.g->name();
            kept += w.kept;
            for (auto& n : w.notes) problems.push_back(cn + ": " + n);
            // Energy tables by (output, related pin, edge), for internal_power.
            std::map<std::string, std::vector<double>> energy;
            for (auto& j : w.arcs) {
                size_t n = j.delay.rows() * j.delay.cols();
                std::vector<double> dly(n), trn(n), eng(n);
                std::string err;
                for (size_t k = 0; k < n && err.empty(); k++) {
                    // Non-unate: the slower of the two input edges, point by point.
                    int best = -1;
                    for (int d = 0; d < 2; d++) {
                        if (!j.tried[d]) continue;
                        const Point& p = j.pts[d][k];
                        if (!p.err.empty()) { if (p.err != "no stimulus") err = p.err; continue; }
                        if (best < 0 || p.delay > j.pts[best][k].delay) best = d;
                    }
                    if (best < 0) { if (err.empty()) err = "no stimulus moves the output"; break; }
                    dly[k] = j.pts[best][k].delay; trn[k] = j.pts[best][k].trans; eng[k] = j.pts[best][k].energy;
                }
                std::string what = cn + ": " + j.related + " -> " + j.out + (j.rise ? " rise" : " fall") +
                                   (j.when.empty() ? "" : " when " + j.when);
                if (!err.empty()) { problems.push_back(what + ": " + err + " (template values kept)"); failed++; continue; }
                setValues(j.delay, dly);
                if (j.trans.g) setValues(j.trans, trn);
                std::string key = j.out + "\n" + j.related + "\n" + (j.rise ? "r" : "f");
                if (!energy.count(key) || (j.type != "preset" && j.type != "clear")) energy[key] = eng;
                nArc++;
            }
            if (!opt.keep.count("power"))
                for (auto& pg : w.g->groups) {
                    if (pg.type != "pin") continue;
                    for (auto& ip : pg.groups) {
                        if (ip.type != "internal_power") continue;
                        for (bool rise : {true, false}) {
                            Group* tg = child(ip, rise ? "rise_power" : "fall_power");
                            auto it = energy.find(pg.name() + "\n" + ip.str("related_pin") + "\n" + (rise ? "r" : "f"));
                            TableRef r;
                            if (!tg || it == energy.end() || !tableRef(*tg, templates, r)) continue;
                            if (r.rows() * r.cols() != it->second.size()) continue;
                            setValues(r, it->second);
                            nPower++;
                        }
                    }
                }
            for (auto& j : w.cons) {
                std::vector<double> v;
                std::string err;
                for (auto& p : j.pts) {
                    if (!p.err.empty()) err = p.err;
                    v.push_back(p.value);
                }
                if (!err.empty()) {
                    problems.push_back(cn + ": " + j.type + " " + j.pin + " to " + j.related + (j.rise ? " rise: " : " fall: ") +
                                       err + " (template values kept)");
                    failed++;
                    continue;
                }
                setValues(j.tab, v);
                nCons++;
            }
            for (auto& j : w.pulses) {
                const char* attr = j.high ? "min_pulse_width_high" : "min_pulse_width_low";
                if (!j.pt.err.empty()) {
                    problems.push_back(cn + ": " + attr + " of " + j.pin->name() + ": " + j.pt.err + " (template value kept)");
                    failed++;
                    continue;
                }
                setAttr(*j.pin, attr, num(j.pt.value));
                nPulse++;
            }
            for (auto& j : w.caps) {
                if (!j.pt.err.empty()) {
                    problems.push_back(cn + ": capacitance of " + j.pin->name() + ": " + j.pt.err + " (template value kept)");
                    failed++;
                    continue;
                }
                setAttr(*j.pin, "capacitance", num(0.5 * (j.pt.value + j.pt.delay)));
                if (j.pin->has("rise_capacitance")) setAttr(*j.pin, "rise_capacitance", num(j.pt.value));
                if (j.pin->has("fall_capacitance")) setAttr(*j.pin, "fall_capacitance", num(j.pt.delay));
                nCap++;
            }
            for (auto& j : w.leaks) {
                if (!j.pt.err.empty()) {
                    problems.push_back(cn + ": leakage: " + j.pt.err + " (template value kept)");
                    failed++;
                    continue;
                }
                if (j.group) setAttr(*j.group, "value", num(j.pt.value));
                else setAttr(*w.g, "cell_leakage_power", num(j.pt.value));
                nLeak++;
            }
        }
        std::ofstream out(opt.out);
        if (!out) throw std::runtime_error("cannot write " + opt.out);
        out << "/* Characterized by mchar on mspice from " << fs::path(opt.lib).filename().string() << ". */\n"
            << liberty::write(root);
        out.close();
        for (auto& p : problems) std::cerr << "mchar: " << p << "\n";
        if (!opt.quiet) {
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("%d delay tables, %d constraint tables, %d pulse widths, %d power tables, %d pin capacitances, %d leakage values\n",
                        nArc, nCons, nPulse, nPower, nCap, nLeak);
            if (kept) std::printf("%d timing groups of other kinds kept from the template\n", kept);
            if (noNetlist) std::printf("%d cell%s without a netlist: the template's numbers are kept\n", noNetlist, noNetlist == 1 ? "" : "s");
            std::printf("%d failed; wrote %s in %.1f s\n", failed, opt.out.c_str(), s);
        }
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "mchar: " << e.what() << "\n";
        return 2;
    }
}
