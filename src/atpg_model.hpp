// atpg_model.hpp - from a gate netlist to the model the test tools run on.
//
//   Design      the flat netlist (gnetlist.hpp) with each instance bound to
//               the function of its library cell (liberty.hpp). Without a
//               library, the generic cells msynth writes are known by name.
//   traceScan() finds the scan structure the way a tester sees it: with scan
//               enable active, each scan flop takes its value from a scan-in
//               port or from the flop before it. Both styles of scan cell are
//               traced: a library scan flop (scan-in and scan-enable pins) and
//               a plain flop with a 2:1 mux on D that scan enable selects.
//   build()     cuts the scan flops open (their outputs become inputs the
//               tester loads, their captured values are observed) and makes
//               the combinational model for faultsim.hpp: one frame for
//               stuck-at faults, two for launch-on-capture transition faults.
//               It also lists the fault sites, one per cell pin and per port.
//   makeFaults() two faults per site.
#pragma once
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "faultsim.hpp"
#include "gnetlist.hpp"
#include "liberty.hpp"

namespace atpg {

struct ModelError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A cell function, in the shape the model wants it.
struct Fn {
    enum Shape { Tree, Xor, Xnor, Mux } shape = Tree;
    liberty::Expr e;
    std::vector<std::string> vars;
    std::vector<int> order;   // Xor/Xnor: the vars; Mux: a, b, s (indices into vars)

    static int count(const liberty::Expr& e, const std::string& v) {
        int n = e.kind == liberty::Expr::Var && e.var == v;
        for (auto& k : e.kids) n += count(k, v);
        return n;
    }
    int uses(const std::string& v) const {
        if (shape == Tree) return count(e, v);
        for (auto& x : vars)
            if (x == v) return 1;
        return 0;
    }
    static Fn make(const liberty::Expr& ex) {
        Fn f;
        f.e = ex;
        f.vars = ex.vars();
        size_t n = f.vars.size();
        if (n < 2 || n > 3) return f;
        uint64_t tt = ex.truthTable(f.vars), all = (1ull << (1u << n)) - 1;
        uint64_t par = 0;
        for (unsigned i = 0; i < (1u << n); i++)
            if (__builtin_popcount(i) & 1) par |= 1ull << i;
        if (tt == par || tt == (~par & all)) {
            f.shape = tt == par ? Xor : Xnor;
            for (size_t i = 0; i < n; i++) f.order.push_back((int)i);
            return f;
        }
        if (n == 3)
            for (int s = 0; s < 3; s++)
                for (int a = 0; a < 3; a++) {
                    if (a == s) continue;
                    int b = 3 - s - a;
                    uint64_t m = 0;
                    for (unsigned i = 0; i < 8; i++)
                        if ((i >> ((i >> s & 1) ? b : a)) & 1) m |= 1ull << i;
                    if (m == tt) {
                        f.shape = Mux;
                        f.order = {a, b, s};
                        return f;
                    }
                }
        return f;
    }
};

struct OutDef {
    std::string pin;
    Fn fn;
    int state = 0;   // a flop output: 1 the state, 2 its inverse, 0 a function
};

struct CellDef {
    std::string name;
    std::vector<std::string> in;
    std::vector<OutDef> out;
    bool flop = false, blackbox = false;
    std::string why;            // blackbox: the reason
    bool isBuf = false, isInv = false;
    // flop
    Fn next;
    std::string iq, iqn;
    int ck = -1;
    bool ckInv = false;
    int clr = -1, pre = -1;     // pins; -2: not a simple pin
    int clrActive = 0, preActive = 0;
    int si = -1, se = -1, d = -1;
    int seActive = 1;
    bool siInv = false;

    int pin(const std::string& n) const {
        for (size_t i = 0; i < in.size(); i++)
            if (in[i] == n) return (int)i;
        return -1;
    }
    int outPin(const std::string& n) const {
        for (size_t i = 0; i < out.size(); i++)
            if (out[i].pin == n) return (int)i;
        return -1;
    }
};

struct Cell {
    std::string name;
    const CellDef* def = nullptr;
    std::vector<int> in, out;   // nets, -1 open
};

struct Port {
    std::string name, bus;
    bool input = true;
    int net = -1;
};

struct Design {
    std::string top;
    std::vector<std::string> nets;
    std::vector<Cell> cells;
    std::vector<Port> ports;
    int c0 = -1, c1 = -1;
    std::deque<CellDef> defs;
    std::vector<std::string> warnings;

    // The (cell, output) that drives each net.
    std::vector<std::pair<int, int>> drivers() const {
        std::vector<std::pair<int, int>> d(nets.size(), {-1, -1});
        for (int i = 0; i < (int)cells.size(); i++)
            for (int k = 0; k < (int)cells[i].out.size(); k++)
                if (cells[i].out[k] >= 0) d[cells[i].out[k]] = {i, k};
        return d;
    }
};

// The generic cells msynth writes, for a run without -lib.
inline std::string genericLib() {
    static const char* abcd[] = {"A", "B", "C", "D"};
    std::string s = "library(generic) {\n";
    auto comb = [&](const std::string& name, int nin, const std::string& fn, const char* third = nullptr) {
        s += "  cell(" + name + ") {";
        for (int i = 0; i < nin; i++)
            s += std::string(" pin(") + (third && i == 2 ? third : abcd[i]) + ") { direction: input; }";
        s += " pin(Y) { direction: output; function: \"" + fn + "\"; } }\n";
    };
    comb("INV", 1, "!A");
    comb("BUF", 1, "A");
    for (int k = 2; k <= 4; k++) {
        std::string a, o;
        for (int i = 0; i < k; i++) {
            a += std::string(i ? "&" : "") + abcd[i];
            o += std::string(i ? "|" : "") + abcd[i];
        }
        std::string n = std::to_string(k);
        comb("AND" + n, k, a);
        comb("NAND" + n, k, "!(" + a + ")");
        comb("OR" + n, k, o);
        comb("NOR" + n, k, "!(" + o + ")");
    }
    comb("XOR2", 2, "A^B");
    comb("XNOR2", 2, "!(A^B)");
    comb("MUX2", 3, "(A&!S)|(B&S)", "S");
    comb("AOI21", 3, "!((A&B)|C)");
    comb("OAI21", 3, "!((A|B)&C)");
    comb("AOI22", 4, "!((A&B)|(C&D))");
    comb("OAI22", 4, "!((A|B)&(C|D))");
    comb("TIE0", 0, "0");
    comb("TIE1", 0, "1");
    for (int scan = 0; scan < 2; scan++)
        for (int r = 0; r < 2; r++)
            for (int st = 0; st < 2; st++) {
                s += std::string("  cell(") + (scan ? "S" : "") + "DFF" + (r ? "R" : "") + (st ? "S" : "") +
                     ") { ff(IQ,IQN) { clocked_on: \"CK\"; next_state: \"" +
                     (scan ? "(D&!SE)|(SI&SE)" : "D") + "\";";
                if (r) s += " clear: \"!RN\";";
                if (st) s += " preset: \"!SN\";";
                s += " } pin(D) { direction: input; }";
                if (scan) s += " pin(SI) { direction: input; } pin(SE) { direction: input; }";
                s += " pin(CK) { direction: input; clock: true; }";
                if (r) s += " pin(RN) { direction: input; }";
                if (st) s += " pin(SN) { direction: input; }";
                s += " pin(Q) { direction: output; function: \"IQ\"; }"
                     " pin(QN) { direction: output; function: \"IQN\"; } }\n";
            }
    return s + "}\n";
}

// One library cell as the model sees it.
inline CellDef defOf(const liberty::Cell& lc) {
    CellDef d;
    d.name = lc.name;
    auto black = [&](const std::string& why) {
        d.blackbox = true;
        d.why = why;
        d.flop = false;
        return d;
    };
    for (const liberty::Pin& p : lc.pins) {
        if (p.dir == liberty::Dir::In || p.dir == liberty::Dir::InOut) d.in.push_back(p.name);
        else if (p.dir == liberty::Dir::Out) {
            OutDef o;
            o.pin = p.name;
            d.out.push_back(o);
        }
    }
    if (!lc.latches.empty()) return black("a latch");
    if (lc.ffs.size() > 1) return black("more than one flop in the cell");
    try {
        if (!lc.ffs.empty()) {
            const liberty::FF& ff = lc.ffs[0];
            d.flop = true;
            d.iq = ff.iq;
            d.iqn = ff.iqn;
            if (ff.nextState.empty() || ff.clockedOn.empty()) return black("a flop without next_state or clocked_on");
            d.next = Fn::make(liberty::parseFunction(ff.nextState));
            auto simple = [&](const std::string& text, int& pin, bool& inv) {
                liberty::Expr e = liberty::parseFunction(text);
                inv = false;
                if (e.kind == liberty::Expr::Not) {
                    inv = true;
                    liberty::Expr k = e.kids[0];
                    e = k;
                }
                pin = e.kind == liberty::Expr::Var ? d.pin(e.var) : -1;
                if (pin < 0) pin = -2;
            };
            simple(ff.clockedOn, d.ck, d.ckInv);
            bool inv;
            if (!ff.clear.empty()) { simple(ff.clear, d.clr, inv); d.clrActive = inv ? 0 : 1; }
            if (!ff.preset.empty()) { simple(ff.preset, d.pre, inv); d.preActive = inv ? 0 : 1; }
            // The scan pins: marked in test_cell, or by their usual names.
            auto mark = [&](const liberty::Pin& p) {
                if (p.signalType.rfind("test_scan_in", 0) == 0 && d.si < 0) d.si = d.pin(p.name);
                if (p.signalType.rfind("test_scan_enable", 0) == 0 && d.se < 0) d.se = d.pin(p.name);
            };
            for (const liberty::Pin& tp : lc.testPins) mark(tp);
            for (const liberty::Pin& p : lc.pins) mark(p);
            if (d.si < 0 && d.se < 0 && d.pin("SI") >= 0 && d.pin("SE") >= 0 &&
                d.next.uses("SI") && d.next.uses("SE")) {
                d.si = d.pin("SI");
                d.se = d.pin("SE");
            }
            if (d.si >= 0 && d.se >= 0 && d.next.vars.size() <= 6) {
                // Which value of scan enable makes the next state the scan input?
                const std::string sin = d.in[d.si], sen = d.in[d.se];
                bool found = false;
                size_t n = d.next.vars.size();
                for (int act = 1; act >= 0 && !found; act--)
                    for (int iv = 0; iv < 2 && !found; iv++) {
                        bool ok = true;
                        for (unsigned i = 0; i < (1u << n) && ok; i++) {
                            bool siv = false, sev = false;
                            for (size_t k = 0; k < n; k++) {
                                if (d.next.vars[k] == sin) siv = i >> k & 1;
                                if (d.next.vars[k] == sen) sev = i >> k & 1;
                            }
                            if ((int)sev != act) continue;
                            bool v = d.next.e.eval([&](const std::string& nm) {
                                for (size_t k = 0; k < n; k++)
                                    if (d.next.vars[k] == nm) return (i >> k & 1) != 0;
                                return false;
                            });
                            ok = v == (siv != (bool)iv);
                        }
                        if (ok) { found = true; d.seActive = act; d.siInv = iv; }
                    }
                if (!found) d.si = d.se = -1;
            } else {
                d.si = d.se = -1;
            }
            if (d.next.e.kind == liberty::Expr::Var) d.d = d.pin(d.next.e.var);
        }
        for (OutDef& o : d.out) {
            const liberty::Pin* p = lc.pin(o.pin);
            if (!p || p->function.empty()) return black("output '" + o.pin + "' has no function");
            if (!p->threeState.empty()) return black("a three-state output");
            liberty::Expr e = liberty::parseFunction(p->function);
            if (d.flop && e.kind == liberty::Expr::Var && e.var == d.iq) o.state = 1;
            else if (d.flop && e.kind == liberty::Expr::Var && e.var == d.iqn) o.state = 2;
            o.fn = Fn::make(e);
            for (const std::string& v : o.fn.vars) {
                bool st = d.flop && (v == d.iq || v == d.iqn);
                if (d.flop ? !st : d.pin(v) < 0)
                    return black("output '" + o.pin + "' reads '" + v + "'");
            }
        }
        if (d.flop)
            for (const std::string& v : d.next.vars)
                if (d.pin(v) < 0 && v != d.iq && v != d.iqn)
                    return black("next_state reads '" + v + "', which is not an input pin");
    } catch (const std::exception& e) {
        return black(e.what());
    }
    if (!d.flop && d.in.size() == 1 && d.out.size() == 1) {
        const liberty::Expr& e = d.out[0].fn.e;
        d.isBuf = e.kind == liberty::Expr::Var;
        d.isInv = e.kind == liberty::Expr::Not && e.kids[0].kind == liberty::Expr::Var;
    }
    return d;
}

// Read the netlist files, flatten under `top`, and bind every instance to a
// library cell (from `libs`, then the generic cells, then the gate primitives).
inline Design readDesign(const std::vector<std::string>& files, const std::vector<std::string>& libs,
                         std::string top = "") {
    Design ds;
    std::map<std::string, const CellDef*> byName;
    auto addLib = [&](const liberty::Library& L) {
        for (const liberty::Cell& c : L.cells) {
            if (byName.count(c.name)) continue;
            ds.defs.push_back(defOf(c));
            byName[c.name] = &ds.defs.back();
        }
    };
    for (const std::string& l : libs) addLib(liberty::Library::read(l));
    addLib(liberty::Library::load(liberty::parse(genericLib(), "<generic cells>")));

    static const std::map<std::string, std::string> prims = {
        {"and", "&"}, {"nand", "!&"}, {"or", "|"}, {"nor", "!|"},
        {"xor", "^"}, {"xnor", "!^"}, {"buf", ""}, {"not", "!"}};
    auto primDef = [&](const std::string& kind, int nin) -> const CellDef* {
        std::string key = kind + "#" + std::to_string(nin);
        auto it = byName.find(key);
        if (it != byName.end()) return it->second;
        std::string op = prims.at(kind), fn;
        bool neg = !op.empty() && op[0] == '!';
        if (neg) op = op.substr(1);
        CellDef d;
        d.name = kind;
        for (int i = 0; i < nin; i++) {
            d.in.push_back("I" + std::to_string(i));
            fn += (i ? op : "") + d.in.back();
        }
        if (neg) fn = "!(" + fn + ")";
        OutDef o;
        o.pin = "O";
        o.fn = Fn::make(liberty::parseFunction(fn));
        d.out.push_back(o);
        d.isBuf = kind == "buf";
        d.isInv = kind == "not";
        ds.defs.push_back(d);
        return byName[key] = &ds.defs.back();
    };

    gnl::Source src;
    for (const std::string& f : files) gnl::read(f, src);
    if (top.empty()) {
        std::vector<std::string> roots;
        for (const std::string& t : gnl::tops(src))
            if (!byName.count(t)) roots.push_back(t);
        if (roots.empty()) throw ModelError("no top module found");
        if (roots.size() > 1) {
            std::string all;
            for (auto& r : roots) all += " " + r;
            throw ModelError("more than one top module; pick one with -top:" + all);
        }
        top = roots[0];
    }
    auto pinOrder = [&](const std::string& cell) -> std::vector<std::string> {
        std::vector<std::string> r;
        if (prims.count(cell)) {
            // A gate primitive: the output, then the inputs.
            r.push_back("O");
            for (int i = 0; i < 256; i++) r.push_back("I" + std::to_string(i));
            return r;
        }
        auto it = byName.find(cell);
        if (it == byName.end()) return r;
        for (auto& p : it->second->in) r.push_back(p);
        for (auto& o : it->second->out) r.push_back(o.pin);
        return r;
    };
    gnl::Netlist flat = gnl::flatten(src, top, nullptr, pinOrder);
    ds.top = flat.top;
    ds.nets = flat.nets;
    ds.c0 = flat.const0;
    ds.c1 = flat.const1;
    ds.warnings = flat.warnings;
    for (const gnl::FlatPort& p : flat.ports) {
        Port q;
        q.name = p.name;
        q.bus = p.bus;
        q.input = p.dir != gnl::Dir::Out;
        q.net = p.net;
        if (p.dir == gnl::Dir::InOut) ds.warnings.push_back("inout port '" + p.name + "' is driven as an input");
        ds.ports.push_back(q);
    }
    std::map<std::string, int> unknown;
    for (const gnl::FlatInst& fi : flat.insts) {
        Cell c;
        c.name = fi.name;
        if (prims.count(fi.cell)) {
            int nin = 0;
            for (auto& cn : fi.conns) nin += cn.pin != "O";
            if ((fi.cell == "buf" || fi.cell == "not") && nin != 1)
                throw ModelError("gate '" + fi.name + "': a " + fi.cell + " with several outputs is not read");
            c.def = primDef(fi.cell, std::max(1, nin));
        } else {
            auto it = byName.find(fi.cell);
            if (it == byName.end()) {
                unknown[fi.cell]++;
                continue;
            }
            c.def = it->second;
        }
        c.in.assign(c.def->in.size(), -1);
        c.out.assign(c.def->out.size(), -1);
        for (const gnl::FlatConn& cn : fi.conns) {
            int i = c.def->pin(cn.pin), o = c.def->outPin(cn.pin);
            if (i >= 0) c.in[i] = cn.net;
            else if (o >= 0) c.out[o] = cn.net;
            else throw ModelError("instance '" + fi.name + "': cell " + fi.cell + " has no pin '" + cn.pin + "'");
        }
        ds.cells.push_back(std::move(c));
    }
    if (!unknown.empty()) {
        std::string all;
        for (auto& [n, k] : unknown) all += " " + n;
        throw ModelError("cells that are in no library (pass their .lib with -lib):" + all);
    }
    return ds;
}

// ------------------------------------------------------------------ scan ----

struct ScanCell {
    int cell;
    bool inv;   // the value inverts between the element before and this cell
};
struct Chain {
    std::string siName, soName;
    int siNet = -1, soNet = -1;
    bool soInv = false;
    int clock = -1;
    std::vector<ScanCell> cells;   // from scan-in to scan-out
};
struct Scan {
    std::vector<Chain> chains;
    int seNet = -1;
    int seActive = 1;                 // the port value that shifts
    std::string seName;
    std::vector<int> clocks;          // clock port nets
    std::map<int, int> hold;          // port net -> the value the tester holds in capture
    std::vector<int> chainOf;         // per cell: its chain, or -1
    std::vector<int> scanMux;         // per cell: 1 + the scan-data var of a flop's scan mux, or 0
    std::vector<uint8_t> scanOnly;    // per net: only the scan path reads it
    std::vector<std::string> notes;   // why a flop is not scanned
    int flops = 0, scanned = 0;
};

struct Trace {
    int net;
    bool inv;
    int flop;   // the flop whose output this is, or -1
};

struct Tracer {
    const Design& ds;
    std::vector<std::pair<int, int>> drv;
    explicit Tracer(const Design& d) : ds(d), drv(d.drivers()) {}
    Trace operator()(int net) const {
        Trace t{net, false, -1};
        for (int guard = 0; guard < 100000 && t.net >= 0; guard++) {
            int c = drv[t.net].first;
            if (c < 0) break;
            const Cell& cell = ds.cells[c];
            if (cell.def->isBuf) t.net = cell.in[0];
            else if (cell.def->isInv) { t.net = cell.in[0]; t.inv = !t.inv; }
            else if (cell.def->flop) {
                int st = cell.def->out[drv[t.net].second].state;
                if (st == 0) break;
                if (st == 2) t.inv = !t.inv;
                t.flop = c;
                break;
            } else break;
        }
        return t;
    }
};

inline std::map<int, std::string> portNames(const Design& ds, bool inputs) {
    std::map<int, std::string> m;
    for (const Port& p : ds.ports)
        if (p.input == inputs && p.net >= 0) m.emplace(p.net, p.name);
    return m;
}

inline Scan traceScan(const Design& ds, const std::string& seWanted = "") {
    Scan s;
    Tracer trace(ds);
    int NC = (int)ds.cells.size();
    s.chainOf.assign(NC, -1);
    s.scanMux.assign(NC, 0);
    s.scanOnly.assign(ds.nets.size(), 0);
    auto inName = portNames(ds, true);
    auto isPI = [&](int n) { return n >= 0 && inName.count(n) > 0; };

    // Scan enable: the named port, or the port the scan-enable pins lead to,
    // or a port with one of the usual names.
    std::map<std::pair<int, int>, int> votes;
    for (const Cell& c : ds.cells)
        if (c.def->flop) {
            s.flops++;
            if (c.def->se >= 0) {
                Trace t = trace(c.in[c.def->se]);
                if (t.flop < 0 && isPI(t.net)) votes[{t.net, c.def->seActive ^ (t.inv ? 1 : 0)}]++;
            }
        }
    if (!seWanted.empty()) {
        std::string want = seWanted;
        if (want[0] == '!') { s.seActive = 0; want = want.substr(1); }
        for (auto& [n, name] : inName)
            if (name == want) s.seNet = n;
        if (s.seNet < 0) throw ModelError("no input port named '" + want + "' for scan enable");
    } else if (!votes.empty()) {
        int best = 0;
        for (auto& [k, v] : votes)
            if (v > best) { best = v; s.seNet = k.first; s.seActive = k.second; }
    } else {
        for (const char* cand : {"scan_en", "test_se", "scan_enable", "se", "SE", "scan_se"})
            for (auto& [n, name] : inName)
                if (s.seNet < 0 && name == cand) s.seNet = n;
    }
    if (s.seNet >= 0) s.seName = inName[s.seNet];

    // Each flop: its clock, its asynchronous pins, and where it shifts from.
    struct Info {
        bool ok = false;
        int src = -1;
        bool srcInv = false;
        int clock = -1;
        int mux = -1, muxVar = -1;
        std::vector<std::pair<int, int>> holds;
    };
    std::vector<Info> info(NC);
    for (int i = 0; i < NC; i++) {
        const Cell& c = ds.cells[i];
        const CellDef& d = *c.def;
        if (!d.flop) continue;
        Info& in = info[i];
        auto note = [&](const std::string& why) { s.notes.push_back("flop '" + c.name + "' is not scanned: " + why); };
        if (s.seNet < 0) continue;
        if (d.ck < 0) { note("its clock is not a simple pin"); continue; }
        Trace ck = trace(c.in[d.ck]);
        if (ck.flop >= 0 || !isPI(ck.net)) { note("the tester cannot reach its clock"); continue; }
        if (ck.inv != d.ckInv) { note("it is a falling-edge flop"); continue; }
        if (ck.net == s.seNet) { note("its clock is scan enable"); continue; }
        in.clock = ck.net;
        if (d.se >= 0) {
            Trace t = trace(c.in[d.se]);
            if (t.flop >= 0 || t.net != s.seNet || (d.seActive ^ (t.inv ? 1 : 0)) != s.seActive) {
                note("its scan-enable pin does not come from '" + s.seName + "'");
                continue;
            }
            in.src = c.in[d.si];
            in.srcInv = d.siInv;
        } else {
            int dn = d.d >= 0 ? c.in[d.d] : -1;
            int m = dn >= 0 ? trace.drv[dn].first : -1;
            if (m < 0 || ds.cells[m].def->flop || ds.cells[m].def->blackbox ||
                ds.cells[m].def->out[0].fn.shape != Fn::Mux) {
                note("it has no scan input");
                continue;
            }
            const Cell& mc = ds.cells[m];
            const Fn& fn = mc.def->out[0].fn;
            auto netOf = [&](int var) { return mc.in[mc.def->pin(fn.vars[var])]; };
            Trace t = trace(netOf(fn.order[2]));
            if (t.flop >= 0 || t.net != s.seNet) { note("it has no scan input"); continue; }
            int sel = s.seActive ^ (t.inv ? 1 : 0);
            in.muxVar = fn.order[sel];
            in.src = netOf(in.muxVar);
            in.mux = m;
        }
        bool good = true;
        for (auto [pin, act] : {std::pair<int, int>{d.clr, d.clrActive}, std::pair<int, int>{d.pre, d.preActive}}) {
            if (pin == -1) continue;
            if (pin == -2) { note("its asynchronous reset or set is not a simple pin"); good = false; break; }
            int a = c.in[pin];
            if (a < 0) continue;
            if ((a == ds.c1 && act == 0) || (a == ds.c0 && act == 1)) continue;
            Trace t = trace(a);
            if (t.flop >= 0 || !isPI(t.net)) { note("the tester cannot hold its asynchronous reset or set"); good = false; break; }
            in.holds.push_back({t.net, (act ^ 1) ^ (t.inv ? 1 : 0)});
        }
        if (!good) continue;
        in.ok = true;
    }
    // The tester's holds must agree; a flop that needs the other value loses.
    std::map<int, int> hold;
    for (int i = 0; i < NC; i++) {
        Info& in = info[i];
        if (!in.ok) continue;
        for (auto& [n, v] : in.holds) {
            auto it = hold.find(n);
            if (n == s.seNet || (it != hold.end() && it->second != v)) {
                s.notes.push_back("flop '" + ds.cells[i].name + "' is not scanned: its reset needs a value the tester holds otherwise");
                in.ok = false;
            }
        }
        if (in.ok)
            for (auto& [n, v] : in.holds) hold[n] = v;
    }

    // Chains: from each scan-in port, follow the flop that shifts from it.
    std::map<int, std::vector<std::pair<int, bool>>> fromFlop, fromPort;
    for (int i = 0; i < NC; i++) {
        if (!info[i].ok) continue;
        Trace t = trace(info[i].src);
        bool inv = t.inv != info[i].srcInv;
        if (t.flop >= 0) fromFlop[t.flop].push_back({i, inv});
        else if (isPI(t.net) && t.net != s.seNet) fromPort[t.net].push_back({i, inv});
        else s.notes.push_back("flop '" + ds.cells[i].name + "' is not scanned: its scan input does not come from a port or a flop");
    }
    auto outName = portNames(ds, false);
    std::map<int, std::vector<std::pair<int, bool>>> poOf;
    for (auto& [n, name] : outName) {
        Trace t = trace(n);
        if (t.flop >= 0) poOf[t.flop].push_back({n, t.inv});
    }
    // In port order, so that chain 1 is the first scan-in port declared.
    std::vector<int> scanIns;
    for (const Port& p : ds.ports)
        if (p.input && fromPort.count(p.net)) scanIns.push_back(p.net);
    for (int port : scanIns) {
        auto& heads = fromPort[port];
        if (hold.count(port)) continue;
        Chain ch;
        ch.siNet = port;
        ch.siName = inName[port];
        std::vector<uint8_t> seen(NC, 0);
        std::pair<int, bool> cur = heads[0];
        for (;;) {
            if (seen[cur.first] || !info[cur.first].ok || s.chainOf[cur.first] >= 0) break;
            seen[cur.first] = 1;
            ch.cells.push_back({cur.first, cur.second});
            auto it = fromFlop.find(cur.first);
            if (it == fromFlop.end()) break;
            cur = it->second[0];
        }
        // The chain ends at the last flop that reaches an output port.
        while (!ch.cells.empty() && !poOf.count(ch.cells.back().cell)) ch.cells.pop_back();
        if (ch.cells.empty()) {
            s.notes.push_back("the chain from '" + ch.siName + "' reaches no output port");
            continue;
        }
        auto so = poOf[ch.cells.back().cell][0];
        for (auto& cand : poOf[ch.cells.back().cell]) {
            const std::string& nm = outName[cand.first];
            if (nm.find("so") != std::string::npos || nm.find("scan") != std::string::npos) { so = cand; break; }
        }
        ch.soNet = so.first;
        ch.soInv = so.second;
        ch.soName = outName[so.first];
        ch.clock = info[ch.cells[0].cell].clock;
        int id = (int)s.chains.size();
        for (auto& sc : ch.cells) {
            s.chainOf[sc.cell] = id;
            if (info[sc.cell].mux >= 0) s.scanMux[info[sc.cell].mux] = 1 + info[sc.cell].muxVar;
            s.scanned++;
        }
        s.chains.push_back(std::move(ch));
    }
    for (int i = 0; i < NC; i++)
        if (info[i].ok && s.chainOf[i] < 0)
            s.notes.push_back("flop '" + ds.cells[i].name + "' is not scanned: it is on no chain that runs from a port to a port");

    // What the tester holds in capture: clocks off, scan enable off, resets off.
    std::map<int, int> clk;
    for (int i = 0; i < NC; i++) {
        if (s.chainOf[i] < 0) continue;
        clk[info[i].clock] = 1;
        for (auto& [n, v] : info[i].holds) s.hold[n] = v;
    }
    for (auto& [n, v] : clk) {
        s.clocks.push_back(n);
        s.hold[n] = 0;
    }
    if (s.seNet >= 0 && !s.chains.empty()) s.hold[s.seNet] = s.seActive ^ 1;

    // Nets only the scan path reads (the scan-enable tree, scan-in wiring).
    if (!s.chains.empty()) {
        // use: 0 functional, 1 scan, 2 through a buffer or inverter
        std::vector<std::vector<std::pair<int, int>>> sinks(ds.nets.size());
        for (int i = 0; i < NC; i++) {
            const Cell& c = ds.cells[i];
            const CellDef& d = *c.def;
            for (int p = 0; p < (int)c.in.size(); p++) {
                if (c.in[p] < 0) continue;
                int k = 0;
                if (d.flop) k = s.chainOf[i] >= 0 && (p == d.si || p == d.se);
                else if (s.scanMux[i]) {
                    const Fn& fn = d.out[0].fn;
                    k = d.in[p] == fn.vars[s.scanMux[i] - 1] || d.in[p] == fn.vars[fn.order[2]];
                } else if (d.isBuf || d.isInv) k = 2;
                sinks[c.in[p]].push_back({i, k});
            }
        }
        std::vector<uint8_t> isPO(ds.nets.size(), 0);
        for (auto& [n, name] : outName) isPO[n] = 1;
        std::vector<int8_t> memo(ds.nets.size(), -1);
        std::function<bool(int)> only = [&](int n) -> bool {
            if (n < 0) return false;
            if (memo[n] >= 0) return memo[n];
            memo[n] = 0;
            if (isPO[n] || sinks[n].empty()) return false;
            for (auto& [c, k] : sinks[n]) {
                if (k == 0) return false;
                if (k == 2 && !only(ds.cells[c].out[0])) return false;
            }
            memo[n] = 1;
            return true;
        };
        for (size_t n = 0; n < ds.nets.size(); n++)
            if ((int)n != ds.c0 && (int)n != ds.c1) s.scanOnly[n] = only((int)n);
    }
    return s;
}

// ----------------------------------------------------------------- model ----

// Where a fault can sit: a pin of a cell or a port.
struct Site {
    std::string name;
    int node = -1, fi = -1;
    int cond = -1;          // the same line in the first frame (two-frame model)
    uint8_t special = 0;
    int8_t active = -1;     // kAsync: the stuck value that a chain test sees
};
enum : uint8_t { kNone, kClock, kAsync, kScan, kNonScan, kUnused };

struct Model {
    Circuit c;
    int frames = 1;
    std::vector<Site> sites;
    std::vector<int> piNets;       // the nets of c.inputs[0 .. piNets.size())
    std::vector<int> scanCells;    // the cells of the remaining inputs, chain by chain
    std::vector<int> poNets;       // output port bits
    std::vector<int> poNodes;      // their observed nodes
    std::vector<int> capNodes;     // the value each scan cell captures
    int loops = 0, floating = 0, blackboxes = 0;
};

// Emit a function as gates. var(name) gives the node of a variable; each
// place a variable enters a gate is reported through use(name, node, pin).
struct Emitter {
    Circuit& c;
    int c0, c1;
    std::function<int(const std::string&)> var;
    std::function<void(const std::string&, int, int)> use;

    using E = liberty::Expr;
    void flatten(const E& e, E::Kind k, std::vector<const E*>& out) {
        if (e.kind == k) for (auto& kid : e.kids) flatten(kid, k, out);
        else out.push_back(&e);
    }
    int gate(T t, const std::vector<const E*>& ops) {
        std::vector<int> ins;
        for (const E* o : ops) ins.push_back(o->kind == E::Var ? var(o->var) : tree(*o, false));
        int n = c.addv(t, ins);
        for (size_t i = 0; i < ops.size(); i++)
            if (ops[i]->kind == E::Var) use(ops[i]->var, n, (int)i);
        return n;
    }
    int tree(const E& e, bool neg) {
        switch (e.kind) {
            case E::Const: return c.add(BUF, {(e.value != neg) ? c1 : c0});
            case E::Var: return gate(neg ? NOT : BUF, {&e});
            case E::Not: return tree(e.kids[0], !neg);
            case E::And: case E::Or: case E::Xor: {
                std::vector<const E*> ops;
                flatten(e, e.kind, ops);
                T t = e.kind == E::And ? (neg ? NAND : AND) : e.kind == E::Or ? (neg ? NOR : OR) : (neg ? XNOR : XOR);
                return gate(t, ops);
            }
        }
        return c0;
    }
    int emit(const Fn& f) {
        if (f.shape == Fn::Tree) return tree(f.e, false);
        std::vector<int> ins;
        for (int i : f.order) ins.push_back(var(f.vars[i]));
        int n = c.addv(f.shape == Fn::Mux ? MUX : f.shape == Fn::Xor ? XOR : XNOR, ins);
        for (size_t i = 0; i < f.order.size(); i++) use(f.vars[f.order[i]], n, (int)i);
        return n;
    }
};

inline Model build(const Design& ds, const Scan& scan, int frames) {
    Model m;
    m.frames = frames;
    Circuit& c = m.c;
    int NC = (int)ds.cells.size(), NN = (int)ds.nets.size();
    auto drv = ds.drivers();
    auto inName = portNames(ds, true);
    auto outName = portNames(ds, false);
    auto seq = [&](int cell) { return ds.cells[cell].def->flop || ds.cells[cell].def->blackbox; };

    // Order the combinational cells; cut loops.
    std::vector<uint8_t> ready(NN, 0), cut(NN, 0);
    std::vector<int> order;
    {
        std::vector<std::vector<int>> sinks(NN);
        std::vector<int> need(NC, 0), queue;
        for (int n = 0; n < NN; n++)
            if (drv[n].first < 0 || seq(drv[n].first)) ready[n] = 1;
        for (int i = 0; i < NC; i++) {
            if (seq(i)) continue;
            for (int n : ds.cells[i].in)
                if (n >= 0 && !ready[n]) { need[i]++; sinks[n].push_back(i); }
            if (!need[i]) queue.push_back(i);
        }
        std::vector<uint8_t> done(NC, 0);
        size_t head = 0;
        int scanFrom = 0;
        auto release = [&](int n) {
            if (n < 0 || ready[n]) return;
            ready[n] = 1;
            for (int s : sinks[n])
                if (--need[s] == 0) queue.push_back(s);
        };
        for (;;) {
            while (head < queue.size()) {
                int i = queue[head++];
                done[i] = 1;
                order.push_back(i);
                for (int n : ds.cells[i].out) release(n);
            }
            while (scanFrom < NC && (done[scanFrom] || seq(scanFrom))) scanFrom++;
            if (scanFrom >= NC) break;
            m.loops++;
            for (int n : ds.cells[scanFrom].in)
                if (n >= 0 && !ready[n]) { cut[n] = 1; release(n); }
        }
    }

    int c0 = c.add(C0), c1 = c.add(C1);
    std::vector<int> nn(NN, -1), nn1;
    for (const Port& p : ds.ports) {
        int n = p.net;
        if (!p.input || n < 0 || n == ds.c0 || n == ds.c1 || nn[n] >= 0) continue;
        nn[n] = c.add(IN);
        auto h = scan.hold.find(n);
        if (h != scan.hold.end()) c.fixed[nn[n]] = (int8_t)h->second;
        m.piNets.push_back(n);
    }
    std::vector<int> qNode(NC, -1);
    for (const Chain& ch : scan.chains)
        for (const ScanCell& sc : ch.cells) {
            qNode[sc.cell] = c.add(IN);
            m.scanCells.push_back(sc.cell);
        }
    if (ds.c0 >= 0) nn[ds.c0] = c0;
    if (ds.c1 >= 0) nn[ds.c1] = c1;
    for (int n = 0; n < NN; n++)
        if (nn[n] < 0 && (drv[n].first < 0 || cut[n] || ds.cells[drv[n].first].def->blackbox)) {
            nn[n] = c.add(XS);
            if (drv[n].first < 0) m.floating++;
        }
    for (const Cell& cell : ds.cells) m.blackboxes += cell.def->blackbox;
    int xs = -1;
    std::vector<int> fixedNet = nn;
    std::vector<int> cap(NC, -1), capPrev, state(NC, -1), stateN(NC, -1);

    struct PinSite { int cell, pin, node, fi; };   // pin: input index, or -1-k for output k
    std::vector<PinSite> pinSites;

    for (int f = 0; f < frames; f++) {
        bool last = f == frames - 1;
        if (f > 0) {
            nn1 = nn;
            capPrev = cap;
            nn = fixedNet;
        }
        auto node = [&](int n) {
            if (n >= 0) return nn[n];
            if (xs < 0) xs = c.add(XS);
            return xs;
        };
        // The state of every flop, and the nets its outputs drive.
        for (int i = 0; i < NC; i++) {
            const Cell& cell = ds.cells[i];
            const CellDef& d = *cell.def;
            if (!d.flop) continue;
            int q = f > 0 ? c.add(BUF, {capPrev[i]}) : qNode[i] >= 0 ? qNode[i] : c.add(XS);
            state[i] = q;
            stateN[i] = -1;
            for (int k = 0; k < (int)d.out.size(); k++) {
                int nd;
                if (cell.out[k] < 0) continue;
                if (d.out[k].state == 1) nd = q;
                else {
                    if (stateN[i] < 0) stateN[i] = c.add(NOT, {q});
                    if (d.out[k].state == 2) nd = stateN[i];
                    else {
                        Emitter em{c, c0, c1,
                                   [&](const std::string& v) { return v == d.iq ? q : stateN[i]; },
                                   [](const std::string&, int, int) {}};
                        nd = em.emit(d.out[k].fn);
                    }
                }
                if (cell.out[k] >= 0) nn[cell.out[k]] = nd;
                if (last) pinSites.push_back({i, -1 - k, nd, -1});
            }
        }
        // One cell: a buffer node for a pin that enters its functions more than once.
        auto emitCell = [&](int i, const std::vector<const Fn*>& fns, std::vector<int>& outs) {
            const Cell& cell = ds.cells[i];
            const CellDef& d = *cell.def;
            std::vector<int> pinNode(d.in.size(), -1), cnt(d.in.size(), 0);
            for (const Fn* fn : fns)
                for (size_t p = 0; p < d.in.size(); p++) cnt[p] += fn->uses(d.in[p]);
            for (size_t p = 0; p < d.in.size(); p++) {
                if (cnt[p] == 1) pinNode[p] = node(cell.in[p]);
                else if (cnt[p] > 1) {
                    pinNode[p] = c.add(BUF, {node(cell.in[p])});
                    if (last) pinSites.push_back({i, (int)p, pinNode[p], 0});
                }
            }
            Emitter em{c, c0, c1,
                       [&](const std::string& v) {
                           if (d.flop && v == d.iq) return state[i];
                           if (d.flop && v == d.iqn) {
                               if (stateN[i] < 0) stateN[i] = c.add(NOT, {state[i]});
                               return stateN[i];
                           }
                           int p = d.pin(v);
                           return p >= 0 && pinNode[p] >= 0 ? pinNode[p] : c0;
                       },
                       [&](const std::string& v, int nd, int fi) {
                           int p = d.pin(v);
                           if (last && p >= 0 && cnt[p] == 1) pinSites.push_back({i, p, nd, fi});
                       }};
            for (const Fn* fn : fns) outs.push_back(em.emit(*fn));
        };
        for (int i : order) {
            const Cell& cell = ds.cells[i];
            std::vector<const Fn*> fns;
            for (auto& o : cell.def->out) fns.push_back(&o.fn);
            std::vector<int> outs;
            emitCell(i, fns, outs);
            for (size_t k = 0; k < outs.size(); k++) {
                if (cell.out[k] >= 0 && !cut[cell.out[k]]) nn[cell.out[k]] = outs[k];
                if (last) pinSites.push_back({i, -1 - (int)k, outs[k], -1});
            }
        }
        for (int i = 0; i < NC; i++) {
            if (!ds.cells[i].def->flop) continue;
            std::vector<int> outs;
            emitCell(i, {&ds.cells[i].def->next}, outs);
            cap[i] = outs[0];
        }
        if (last) {
            for (const Port& p : ds.ports) {
                if (p.input || p.net < 0) continue;
                int b = c.add(BUF, {node(p.net)});
                c.obs[b] = 1;
                m.poNets.push_back(p.net);
                m.poNodes.push_back(b);
            }
            for (int cellIdx : m.scanCells) {
                c.obs[cap[cellIdx]] = 1;
                m.capNodes.push_back(cap[cellIdx]);
            }
        }
    }
    c.finish();

    // The fault sites.
    auto condOf = [&](int net) { return frames > 1 && net >= 0 ? nn1[net] : -1; };
    auto add = [&](const std::string& name, int node, int fi, int cond, uint8_t special = kNone, int8_t active = -1) {
        Site s;
        s.name = name;
        s.node = node; s.fi = fi; s.cond = cond; s.special = special; s.active = active;
        m.sites.push_back(std::move(s));
    };
    std::map<int, int> isClock;
    for (int n : scan.clocks) isClock[n] = 1;
    for (int n : m.piNets) {
        uint8_t sp = kNone;
        int8_t act = -1;
        auto h = scan.hold.find(n);
        if (isClock.count(n)) sp = kClock;
        else if (n == scan.seNet || scan.scanOnly[n]) sp = kScan;
        else if (h != scan.hold.end()) { sp = kAsync; act = (int8_t)(h->second ^ 1); }
        add(inName[n], fixedNet[n], -1, fixedNet[n], sp, act);
    }
    for (size_t k = 0; k < m.poNets.size(); k++)
        add(outName[m.poNets[k]], m.poNodes[k], 0, condOf(m.poNets[k]));
    std::vector<std::vector<PinSite>> byCell(NC);
    for (const PinSite& p : pinSites) byCell[p.cell].push_back(p);
    for (int i = 0; i < NC; i++) {
        const Cell& cell = ds.cells[i];
        const CellDef& d = *cell.def;
        if (d.blackbox) continue;
        bool sc = !d.flop || scan.chainOf[i] >= 0;
        std::vector<const PinSite*> inSite(d.in.size(), nullptr), outSite(d.out.size(), nullptr);
        for (const PinSite& p : byCell[i]) {
            if (p.pin >= 0) inSite[p.pin] = &p;
            else outSite[-1 - p.pin] = &p;
        }
        std::string scanVar, selVar;
        if (scan.scanMux[i]) {
            const Fn& fn = d.out[0].fn;
            scanVar = fn.vars[scan.scanMux[i] - 1];
            selVar = fn.vars[fn.order[2]];
        }
        for (size_t p = 0; p < d.in.size(); p++) {
            std::string name = cell.name + "/" + d.in[p];
            int net = cell.in[p];
            if (d.flop && !sc) { add(name, -1, -1, -1, kNonScan); continue; }
            if (d.flop && (int)p == d.ck) { add(name, -1, -1, -1, kClock); continue; }
            if (d.flop && (int)p == d.clr) { add(name, -1, -1, -1, kAsync, (int8_t)d.clrActive); continue; }
            if (d.flop && (int)p == d.pre) { add(name, -1, -1, -1, kAsync, (int8_t)d.preActive); continue; }
            uint8_t sp = kNone;
            if (d.flop && ((int)p == d.si || (int)p == d.se)) sp = kScan;
            if (net >= 0 && scan.scanOnly[net]) sp = kScan;
            if (!scanVar.empty() && (d.in[p] == scanVar || d.in[p] == selVar)) sp = kScan;
            const PinSite* ps = inSite[p];
            if (!ps) { add(name, -1, -1, -1, sp ? sp : (uint8_t)kUnused); continue; }
            add(name, ps->node, ps->fi, condOf(net), sp);
        }
        for (size_t k = 0; k < d.out.size(); k++) {
            const PinSite* ps = outSite[k];
            int net = cell.out[k];
            if (!ps || net < 0) continue;
            uint8_t sp = kNone;
            if (d.flop && !sc) sp = kNonScan;
            else if (scan.scanOnly[net]) sp = kScan;
            add(cell.name + "/" + d.out[k].pin, ps->node, ps->fi, condOf(net), sp);
        }
    }
    return m;
}

// Two faults per site. For a transition fault `sa` is the value the line is
// slow to leave: sa 0 is slow-to-rise, sa 1 is slow-to-fall.
inline std::vector<Fault> makeFaults(const Model& m, bool transition) {
    std::vector<Fault> out;
    for (size_t i = 0; i < m.sites.size(); i++) {
        const Site& s = m.sites[i];
        for (int v = 0; v < 2; v++) {
            Fault f;
            f.site = (int)i;
            f.sa = (uint8_t)v;
            f.node = s.node;
            f.fi = s.fi;
            if (transition) { f.cond = s.cond; f.condv = (uint8_t)v; }
            switch (s.special) {
                case kClock: f.cls = DI; break;
                case kAsync: f.cls = (!transition && v == s.active) ? DI : AU; break;
                case kScan: f.cls = DI; break;
                case kNonScan: f.cls = AU; break;
                case kUnused: f.cls = UU; break;
                default: break;
            }
            if (f.cls != NC) f.node = -1;
            else if (f.node < 0 || (transition && f.cond < 0)) { f.cls = AU; f.node = -1; }
            out.push_back(f);
        }
    }
    return out;
}

}  // namespace atpg
