#pragma once
// dft/scan.hpp - scan insertion on a gate netlist: mdft -insert_scan.
//
// Reads a structural netlist (gnetlist.hpp), finds the flops through the
// Liberty library (liberty.hpp) or a cell map, checks that the tester can
// reach each flop's clock and hold its asynchronous set/reset, and stitches
// the scannable flops into mux-D scan chains:
//
//   * a flop with a scan equivalent in the library is swapped for it; any
//     other flop gets a library mux in front of D;
//   * chains are balanced, one clock domain per chain by default; with
//     -mix_clocks falling-edge flops go first and a lockup latch is put
//     between two clocks;
//   * a clock gate's test-enable pin is tied to scan_en.
//
// Writes the stitched netlist (still pure gates), a chain description (JSON)
// for ATPG, logic BIST and compression, and a chain flush testbench that
// shifts a pattern through every chain.
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gnetlist.hpp"
#include "liberty.hpp"
#include "parallel.hpp"

namespace scan {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Run fn(i) for i in [0, n) on up to `threads` threads, a chunk of indices
// at a time. fn must only write to slot i, so the result does not depend on
// the thread count.
template <class F>
void forEach(size_t n, int threads, size_t chunk, F fn) {
    size_t chunks = (n + chunk - 1) / chunk;
    par::parallelFor(chunks, unsigned(std::max(1, threads)), [&](size_t c) {
        size_t end = std::min(n, (c + 1) * chunk);
        for (size_t i = c * chunk; i < end; i++) fn(i);
    });
}

// ------------------------------------------------------------------ cell map

// What a library cell is to scan insertion, and which pin plays which role.
// Roles: flop D CK Q QN R(async reset) S(async set) SI SE; buf/inv A Y;
// mux A B SEL Y (Y = SEL ? B : A); latch D G Q; clock gate CK E TE GCK.
struct CellDef {
    enum Kind { Flop, ScanFlop, Buf, Inv, Mux, Latch, Icg, Other } kind = Other;
    std::string name;
    std::string inst;                        // the cell to instantiate, when `name` is a family
    std::string family;                      // the library it came from
    std::map<std::string, std::string> pin;  // role -> pin name
    std::set<std::string> low;               // roles that are active low / falling edge
    std::string scanCell;                    // the scan equivalent of a plain flop
    std::set<std::string> outputs;           // Other: output pins, when a library told us
    double area = 0;
    bool has(const std::string& r) const { return pin.count(r) != 0; }
    const std::string& p(const std::string& r) const {
        static const std::string none;
        auto it = pin.find(r);
        return it == pin.end() ? none : it->second;
    }
    std::string roleOf(const std::string& pinName) const {
        for (auto& [r, n] : pin)
            if (n == pinName) return r;
        return "";
    }
    static bool isOutput(const std::string& role) {
        return role == "Q" || role == "QN" || role == "Y" || role == "GCK";
    }
    const std::string& instName() const { return inst.empty() ? name : inst; }
};

struct CellMap {
    std::map<std::string, CellDef> cells;
    std::string family = "user";  // of the cells being added
    // Every cell name looked up so far: a netlist has few distinct ones.
    mutable std::unordered_map<std::string, std::pair<const CellDef*, std::string>> cache;

    // One line of a cell map:  flop DFFR D=D CK=CK R=!RN Q=Q scan=SDFFR
    void addLine(const std::string& line, const std::string& where) {
        std::istringstream is(line.substr(0, line.find('#')));
        std::string kind, name, kv;
        if (!(is >> kind)) return;
        if (!(is >> name)) throw Error(where + ": a cell map line needs a cell name");
        CellDef c;
        c.name = name;
        static const std::map<std::string, CellDef::Kind> kinds{
            {"flop", CellDef::Flop}, {"scanflop", CellDef::ScanFlop}, {"buf", CellDef::Buf},
            {"inv", CellDef::Inv},   {"mux", CellDef::Mux},           {"latch", CellDef::Latch},
            {"icg", CellDef::Icg}};
        auto k = kinds.find(kind);
        if (k == kinds.end())
            throw Error(where + ": unknown cell kind '" + kind +
                        "' (flop, scanflop, buf, inv, mux, latch, icg)");
        c.kind = k->second;
        while (is >> kv) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) throw Error(where + ": expected ROLE=PIN, got '" + kv + "'");
            std::string role = kv.substr(0, eq), val = kv.substr(eq + 1);
            if (role == "scan") { c.scanCell = val; continue; }
            if (role == "inst") { c.inst = val; continue; }
            if (!val.empty() && val[0] == '!') { c.low.insert(role); val = val.substr(1); }
            c.pin[role] = val;
        }
        auto need = [&](std::initializer_list<const char*> roles) {
            for (const char* r : roles)
                if (!c.has(r)) throw Error(where + ": " + kind + " " + name + " needs a " + r + " pin");
        };
        switch (c.kind) {
            case CellDef::Flop: need({"D", "CK", "Q"}); break;
            case CellDef::ScanFlop: need({"D", "CK", "SI", "SE", "Q"}); break;
            case CellDef::Buf: case CellDef::Inv: need({"A", "Y"}); break;
            case CellDef::Mux: need({"A", "B", "SEL", "Y"}); break;
            case CellDef::Latch: need({"D", "G", "Q"}); break;
            case CellDef::Icg: need({"CK", "GCK"}); break;
            case CellDef::Other: break;
        }
        c.family = family;
        cells[name] = c;
        cache.clear();
        picked.clear();
    }
    void addText(const std::string& text, const std::string& where) {
        std::istringstream is(text);
        std::string line;
        int n = 0;
        while (std::getline(is, line)) addLine(line, where + ":" + std::to_string(++n));
    }
    void addFile(const std::string& file) {
        std::ifstream f(file);
        if (!f) throw Error("cannot read cell map '" + file + "'");
        std::stringstream ss;
        ss << f.rdbuf();
        addText(ss.str(), file);
    }

    // A cell by name. A drive-strength suffix (DFF_X2, DFFX1, dfxtp_1) falls
    // back to the base name; `suffix` gets what was cut off.
    const CellDef* find(const std::string& name, std::string* suffix = nullptr) const {
        auto hit = cache.find(name);
        if (hit == cache.end()) {
            std::string sfx;
            const CellDef* c = lookup(name, &sfx);
            hit = cache.emplace(name, std::make_pair(c, sfx)).first;
        }
        if (suffix) *suffix = hit->second.second;
        return hit->second.first;
    }
    // Fill the cache for these names, so find() only reads it afterwards and
    // threads can share the map.
    void warm(const std::vector<std::string>& names) const {
        for (const std::string& n : names) find(n);
    }
    const CellDef* lookup(const std::string& name, std::string* suffix) const {
        suffix->clear();
        auto it = cells.find(name);
        if (it != cells.end()) return &it->second;
        const CellDef* best = nullptr;
        size_t bestLen = 0;
        for (auto& [k, c] : cells) {
            if (k.size() <= bestLen || name.size() <= k.size() || name.compare(0, k.size(), k)) continue;
            // The rest must look like a drive strength: _X2, X2, _2, D1, _1.
            size_t i = k.size();
            if (name[i] == '_') i++;
            if (i < name.size() && (name[i] == 'X' || name[i] == 'x' || name[i] == 'D')) i++;
            size_t digits = i;
            while (i < name.size() && std::isdigit((unsigned char)name[i])) i++;
            if (i == digits || i != name.size()) continue;
            best = &c;
            bestLen = k.size();
        }
        if (best) *suffix = name.substr(bestLen);
        return best;
    }

    // The cell of one kind closest to `like` by name (the same library
    // family), preferring one with the wanted polarity of `role`.
    mutable std::map<std::tuple<int, std::string, std::string, bool>, const CellDef*> picked;
    const CellDef* pick(CellDef::Kind kind, const std::string& like, const std::string& role = "",
                        bool wantLow = false) const {
        auto key = std::make_tuple(int(kind), like, role, wantLow);
        auto hit = picked.find(key);
        if (hit != picked.end()) return hit->second;
        return picked[key] = pickSlow(kind, like, role, wantLow);
    }
    const CellDef* pickSlow(CellDef::Kind kind, const std::string& like, const std::string& role,
                            bool wantLow) const {
        const CellDef* best = nullptr;
        long bestScore = -1;
        const CellDef* likeDef = find(like);
        for (auto& [k, c] : cells) {
            if (c.kind != kind) continue;
            size_t common = 0;
            while (common < k.size() && common < like.size() && k[common] == like[common]) common++;
            long score = long(common) * 4;
            if (likeDef && c.family == likeDef->family) score += 100000;
            if (c.family == "user") score += 50000;
            if (!role.empty() && (c.low.count(role) != 0) == wantLow) score += 2;
            if (score > bestScore) { bestScore = score; best = &c; }
        }
        return best;
    }

    // The cells of a Liberty library: flops and latches from their ff/latch
    // groups, scan cells from test_cell, buffers, inverters and muxes from
    // their functions, clock gates from clock_gating_integrated_cell.
    void addLiberty(const liberty::Library& lib) {
        using liberty::Expr;
        // "CK" or "!CK": the pin and whether it is inverted. Anything else: no.
        auto lit = [](const std::string& text, std::string& pin, bool& inv) {
            if (text.empty()) return false;
            Expr e;
            try { e = liberty::parseFunction(text); } catch (const std::exception&) { return false; }
            inv = false;
            while (e.kind == Expr::Not) { inv = !inv; Expr k = e.kids[0]; e = k; }
            if (e.kind != Expr::Var) return false;
            pin = e.var;
            return true;
        };
        for (const liberty::Cell& lc : lib.cells) {
            CellDef c;
            c.name = lc.name;
            c.family = "lib:" + lib.name;
            c.area = lc.area;
            std::vector<const liberty::Pin*> ins, outs;
            for (const liberty::Pin& p : lc.pins) {
                if (p.dir == liberty::Dir::In) ins.push_back(&p);
                if (p.dir == liberty::Dir::Out) { outs.push_back(&p); c.outputs.insert(p.name); }
            }
            auto setRole = [&](const std::string& role, const std::string& text) {
                std::string pin;
                bool inv = false;
                if (!lit(text, pin, inv)) return false;
                c.pin[role] = pin;
                if (inv) c.low.insert(role);
                return true;
            };
            bool icg = lc.group && lc.group->has("clock_gating_integrated_cell");
            if (icg) {
                c.kind = CellDef::Icg;
                for (const liberty::Group* pg : lc.group->all("pin")) {
                    auto yes = [&](const char* a) { return pg->str(a) == "true"; };
                    if (yes("clock_gate_clock_pin")) c.pin["CK"] = pg->name();
                    if (yes("clock_gate_enable_pin")) c.pin["E"] = pg->name();
                    if (yes("clock_gate_test_pin")) c.pin["TE"] = pg->name();
                    if (yes("clock_gate_out_pin")) c.pin["GCK"] = pg->name();
                }
                if (!c.has("CK") || !c.has("GCK")) c.kind = CellDef::Other;
            } else if (lc.ffs.size() == 1) {
                const liberty::FF& ff = lc.ffs[0];
                c.kind = CellDef::Flop;
                bool ok = setRole("CK", ff.clockedOn);
                if (!ff.clear.empty()) ok = setRole("R", ff.clear) && ok;
                if (!ff.preset.empty()) ok = setRole("S", ff.preset) && ok;
                for (const liberty::Pin* p : outs) {
                    if (p->function == ff.iq) c.pin["Q"] = p->name;
                    if (p->function == ff.iqn) c.pin["QN"] = p->name;
                }
                // Scan pins come from the test_cell; the data pin is what is
                // left of next_state.
                for (const liberty::Pin& tp : lc.testPins) {
                    if (tp.signalType == "test_scan_in") c.pin["SI"] = tp.name;
                    if (tp.signalType == "test_scan_enable") c.pin["SE"] = tp.name;
                    if (tp.signalType == "test_scan_enable_inverted") { c.pin["SE"] = tp.name; c.low.insert("SE"); }
                }
                std::vector<std::string> vars;
                try { vars = liberty::parseFunction(ff.nextState).vars(); } catch (const std::exception&) { ok = false; }
                if (c.has("SI") && c.has("SE")) {
                    c.kind = CellDef::ScanFlop;
                    std::vector<std::string> data;
                    for (auto& v : vars)
                        if (v != c.p("SI") && v != c.p("SE")) data.push_back(v);
                    if (data.size() == 1) c.pin["D"] = data[0];
                    else ok = false;
                } else {
                    // A plain D flop only: with an enable or a synchronous
                    // reset in next_state a mux in front of D would not shift.
                    std::string d;
                    bool inv = false;
                    if (lit(ff.nextState, d, inv) && !inv) c.pin["D"] = d;
                    else ok = false;
                }
                if (!ok || !c.has("Q")) {
                    c.kind = CellDef::Other;  // sequential, but not something we can stitch
                    c.pin.clear();
                    c.pin["SEQ"] = "1";
                }
            } else if (lc.latches.size() == 1) {
                const liberty::Latch& l = lc.latches[0];
                c.kind = CellDef::Latch;
                bool ok = setRole("G", l.enable) && setRole("D", l.dataIn) && !c.low.count("D") &&
                          l.clear.empty() && l.preset.empty();
                for (const liberty::Pin* p : outs)
                    if (p->function == l.iq) c.pin["Q"] = p->name;
                if (!ok || !c.has("Q")) { c.kind = CellDef::Other; c.pin.clear(); c.pin["SEQ"] = "1"; }
            } else if (!lc.sequential() && outs.size() == 1 && outs[0]->hasFunction() && outs[0]->threeState.empty()) {
                Expr e;
                try { e = outs[0]->functionExpr(); } catch (const std::exception&) { e = Expr(); }
                std::vector<std::string> vars = e.vars();
                if (vars.size() == 1 && ins.size() == 1) {
                    uint64_t tt = e.truthTable(vars);
                    if (tt == 0b10) c.kind = CellDef::Buf;
                    if (tt == 0b01) c.kind = CellDef::Inv;
                    c.pin["A"] = vars[0];
                    c.pin["Y"] = outs[0]->name;
                } else if (vars.size() == 3 && ins.size() == 3) {
                    // Y = SEL ? B : A for some naming of the three inputs.
                    static const int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
                    for (auto& pm : perm) {
                        std::vector<std::string> abs{vars[pm[0]], vars[pm[1]], vars[pm[2]]};  // A, B, SEL
                        if (e.truthTable(abs) == 0b11001010) {
                            c.kind = CellDef::Mux;
                            c.pin["A"] = abs[0];
                            c.pin["B"] = abs[1];
                            c.pin["SEL"] = abs[2];
                            c.pin["Y"] = outs[0]->name;
                            break;
                        }
                    }
                }
            }
            if (lc.dontUse && (c.kind == CellDef::Mux || c.kind == CellDef::Latch)) c.kind = CellDef::Other;
            cells[c.name] = c;
        }
        // Each plain flop's scan equivalent: the smallest scan cell with the
        // same clock edge, the same set/reset and at least the same outputs.
        for (auto& [name, c] : cells) {
            if (c.kind != CellDef::Flop || !lib.cell(name)) continue;
            const CellDef* best = nullptr;
            for (auto& [sn, sc] : cells) {
                if (sc.kind != CellDef::ScanFlop || !lib.cell(sn) || lib.cell(sn)->dontUse) continue;
                bool same = true;
                for (const char* role : {"CK", "R", "S"})
                    if (c.has(role) != sc.has(role) || c.low.count(role) != sc.low.count(role)) same = false;
                if (c.has("QN") && !sc.has("QN")) same = false;
                if (!same) continue;
                if (!best || sc.area < best->area || (sc.area == best->area && sc.has("QN") < best->has("QN")))
                    best = &sc;
            }
            if (best) c.scanCell = best->name;
        }
        cache.clear();
        picked.clear();
    }

    // The built-in map, used when no library is given: msynth's generic
    // cells, yosys' internal gates and the sky130 high-density flops.
    static CellMap builtin() {
        CellMap m;
        m.family = "generic";
        m.addText(R"(
flop DFF      D=D CK=CK Q=Q QN=QN
flop DFFN     D=D CK=!CKN Q=Q QN=QN
flop DFFR     D=D CK=CK R=!RN Q=Q QN=QN
flop DFFS     D=D CK=CK S=!SN Q=Q QN=QN
flop DFFRS    D=D CK=CK R=!RN S=!SN Q=Q QN=QN
scanflop SDFF   D=D SI=SI SE=SE CK=CK Q=Q QN=QN
scanflop SDFFR  D=D SI=SI SE=SE CK=CK R=!RN Q=Q QN=QN
scanflop SDFFS  D=D SI=SI SE=SE CK=CK S=!SN Q=Q QN=QN
scanflop SDFFRS D=D SI=SI SE=SE CK=CK R=!RN S=!SN Q=Q QN=QN
buf BUF A=A Y=Y
buf CLKBUF A=A Y=Y
inv INV A=A Y=Y
inv CLKINV A=A Y=Y
mux MUX2 A=A B=B SEL=S Y=Y
latch LATCH D=D G=G Q=Q
latch LATCHN D=D G=!GN Q=Q
icg ICG CK=CK E=E TE=TE GCK=GCK
)", "builtin cell map");
        m.family = "sky130";
        m.addText(R"(
flop sky130_fd_sc_hd__dfxtp  D=D CK=CLK Q=Q scan=sky130_fd_sc_hd__sdfxtp
flop sky130_fd_sc_hd__dfxbp  D=D CK=CLK Q=Q QN=Q_N scan=sky130_fd_sc_hd__sdfxbp
flop sky130_fd_sc_hd__dfrtp  D=D CK=CLK R=!RESET_B Q=Q scan=sky130_fd_sc_hd__sdfrtp
flop sky130_fd_sc_hd__dfrbp  D=D CK=CLK R=!RESET_B Q=Q QN=Q_N scan=sky130_fd_sc_hd__sdfrbp
flop sky130_fd_sc_hd__dfstp  D=D CK=CLK S=!SET_B Q=Q scan=sky130_fd_sc_hd__sdfstp
flop sky130_fd_sc_hd__dfrtn  D=D CK=!CLK_N R=!RESET_B Q=Q
scanflop sky130_fd_sc_hd__sdfxtp D=D SI=SCD SE=SCE CK=CLK Q=Q
scanflop sky130_fd_sc_hd__sdfxbp D=D SI=SCD SE=SCE CK=CLK Q=Q QN=Q_N
scanflop sky130_fd_sc_hd__sdfrtp D=D SI=SCD SE=SCE CK=CLK R=!RESET_B Q=Q
scanflop sky130_fd_sc_hd__sdfrbp D=D SI=SCD SE=SCE CK=CLK R=!RESET_B Q=Q QN=Q_N
scanflop sky130_fd_sc_hd__sdfstp D=D SI=SCD SE=SCE CK=CLK S=!SET_B Q=Q
buf sky130_fd_sc_hd__buf A=A Y=X inst=sky130_fd_sc_hd__buf_1
buf sky130_fd_sc_hd__clkbuf A=A Y=X
inv sky130_fd_sc_hd__inv A=A Y=Y inst=sky130_fd_sc_hd__inv_1
inv sky130_fd_sc_hd__clkinv A=A Y=Y
mux sky130_fd_sc_hd__mux2 A=A0 B=A1 SEL=S Y=X inst=sky130_fd_sc_hd__mux2_1
latch sky130_fd_sc_hd__dlxtp D=D G=GATE Q=Q inst=sky130_fd_sc_hd__dlxtp_1
latch sky130_fd_sc_hd__dlxtn D=D G=!GATE_N Q=Q inst=sky130_fd_sc_hd__dlxtn_1
)", "builtin cell map");
        m.family = "yosys";
        m.addText(R"(
buf $_BUF_ A=A Y=Y
inv $_NOT_ A=A Y=Y
mux $_MUX_ A=A B=B SEL=S Y=Y
)", "builtin cell map");
        // yosys internal flops: $_DFF_P_, $_DFF_PN0_. Its enable and
        // synchronous-reset flops are left out: a mux in front of D would
        // not shift while the enable is low or the reset is high.
        for (char c : {'P', 'N'}) {
            std::string ck = c == 'P' ? "CK=C" : "CK=!C";
            m.addLine(std::string("flop $_DFF_") + c + "_ D=D Q=Q " + ck, "builtin");
            for (char r : {'P', 'N'})
                for (char v : {'0', '1'}) {
                    std::string rv{c, r, v};
                    std::string rst = std::string(v == '0' ? "R=" : "S=") + (r == 'N' ? "!R" : "R");
                    m.addLine("flop $_DFF_" + rv + "_ D=D Q=Q " + ck + " " + rst, "builtin");
                }
        }
        m.family = "user";
        return m;
    }
};

// -------------------------------------------------------------- connectivity

// Where a signal comes from, followed back through buffers, inverters and
// clock gates to a port of the top module. (Assigns and hierarchy are already
// gone: gnl::flatten merged them into nets.)
struct Source {
    enum K { Port, Const, Unknown } k = Unknown;
    int net = -1;            // Port: the port's net
    int value = 0;           // Const
    bool inverted = false;
    std::vector<int> gates;  // the clock gates on the way
    std::string why;         // Unknown: what stopped the trace
};

struct Tracer {
    const gnl::Netlist& nl;
    const CellMap& cm;
    struct Driver {
        int inst = -1;
        const CellDef* def = nullptr;
        std::string pin;
    };
    std::vector<Driver> drv;        // per net
    std::vector<int> inPort;        // per net: index into nl.ports of an input port, or -1
    std::vector<const CellDef*> defs;  // per instance

    Tracer(const gnl::Netlist& n, const CellMap& c, int threads) : nl(n), cm(c) {
        drv.resize(nl.nets.size());
        inPort.assign(nl.nets.size(), -1);
        defs.resize(nl.insts.size());
        for (size_t i = 0; i < nl.ports.size(); i++)
            if (nl.ports[i].dir != gnl::Dir::Out && nl.ports[i].net >= 0 && inPort[nl.ports[i].net] < 0)
                inPort[nl.ports[i].net] = int(i);
        {
            std::vector<std::string> names;
            std::unordered_set<std::string> seen;
            for (const gnl::FlatInst& in : nl.insts)
                if (seen.insert(in.cell).second) names.push_back(in.cell);
            cm.warm(names);
        }
        forEach(nl.insts.size(), threads, 8192, [&](size_t k) { defs[k] = cm.find(nl.insts[k].cell); });
        for (size_t k = 0; k < nl.insts.size(); k++) {
            const CellDef* c = defs[k];
            if (!c) continue;
            for (const gnl::FlatConn& fc : nl.insts[k].conns) {
                if (fc.net < 0) continue;
                bool out = c->kind == CellDef::Other ? c->outputs.count(fc.pin) != 0
                                                     : (CellDef::isOutput(c->roleOf(fc.pin)) || c->outputs.count(fc.pin));
                if (out) drv[fc.net] = {int(k), c, fc.pin};
            }
        }
    }

    int pinNet(int inst, const std::string& pin) const {
        for (const gnl::FlatConn& fc : nl.insts[inst].conns)
            if (fc.pin == pin) return fc.net;
        return -1;
    }

    Source trace(int net) const {
        Source s;
        for (int depth = 0; depth < 100000; depth++) {
            if (net < 0) { s.why = "an unconnected pin"; return s; }
            if (net == nl.const0 || net == nl.const1) {
                s.k = Source::Const;
                s.value = (net == nl.const1) != s.inverted;
                return s;
            }
            const Driver& d = drv[net];
            if (d.inst < 0) {
                if (inPort[net] >= 0) {
                    s.k = Source::Port;
                    s.net = net;
                } else {
                    s.why = "'" + nl.nets[net] + "' is not driven from a port through buffers";
                }
                return s;
            }
            const CellDef& c = *d.def;
            std::string from;
            if (c.kind == CellDef::Buf) from = "A";
            else if (c.kind == CellDef::Inv) { from = "A"; s.inverted = !s.inverted; }
            else if (c.kind == CellDef::Icg) { from = "CK"; s.gates.push_back(d.inst); }
            else {
                const char* what = c.kind == CellDef::Flop || c.kind == CellDef::ScanFlop ? "flop" :
                                   c.kind == CellDef::Latch ? "latch" : "cell";
                s.why = "'" + nl.nets[net] + "' is driven by " + what + " '" + nl.insts[d.inst].name + "' (" +
                        nl.insts[d.inst].cell + ")";
                return s;
            }
            net = pinNet(d.inst, c.p(from));
        }
        s.why = "a loop of buffers";
        return s;
    }
};

// A flat netlist as one editable module: the top's ports, one scalar wire per
// internal net, the leaf instances in the flat order.
inline gnl::Module fromFlat(const gnl::Netlist& nl, const gnl::Module& top) {
    gnl::Module w;
    w.name = top.name;
    w.portOrder = top.portOrder;
    for (const std::string& p : top.portOrder) {
        const gnl::Decl* d = top.find(p);
        if (d) w.decl(p) = *d;
    }
    std::vector<gnl::Ref> ref(nl.nets.size());
    std::vector<char> have(nl.nets.size(), 0);
    auto constRef = [](int v) {
        gnl::Ref r;
        r.kind = gnl::Ref::Const;
        r.bits = {uint8_t(v)};
        return r;
    };
    if (nl.const0 >= 0) { ref[nl.const0] = constRef(0); have[nl.const0] = 1; }
    if (nl.const1 >= 0) { ref[nl.const1] = constRef(1); have[nl.const1] = 1; }
    auto portRef = [](const gnl::FlatPort& p) {
        gnl::Ref r;
        if (p.bus.empty()) { r.kind = gnl::Ref::Net; r.name = p.name; }
        else { r.kind = gnl::Ref::Bit; r.name = p.bus; r.msb = r.lsb = p.bit; }
        return r;
    };
    // A net is called by its input port if it has one, else by an output port.
    for (int pass = 0; pass < 2; pass++)
        for (const gnl::FlatPort& p : nl.ports) {
            if (p.net < 0 || (pass == 0) != (p.dir != gnl::Dir::Out)) continue;
            if (!have[p.net]) {
                ref[p.net] = portRef(p);
                have[p.net] = 1;
            } else if (p.dir == gnl::Dir::Out) {
                w.assigns.push_back({{portRef(p)}, {ref[p.net]}});  // a feedthrough or a tie
            }
        }
    for (size_t n = 0; n < nl.nets.size(); n++) {
        if (have[n]) continue;
        std::string name = nl.nets[n];
        while (w.declIdx.count(name)) name += "_";
        w.decl(name);
        ref[n].kind = gnl::Ref::Net;
        ref[n].name = name;
    }
    int spare = 0;
    w.insts.reserve(nl.insts.size());
    for (const gnl::FlatInst& fi : nl.insts) {
        gnl::Inst in;
        in.cell = fi.cell;
        in.name = fi.name;
        // The bits of a bus pin ("D[3]") go back into one connection.
        std::map<std::string, std::map<int, int>> buses;
        for (const gnl::FlatConn& fc : fi.conns) {
            if (fc.net < 0) continue;
            auto lb = fc.pin.rfind('[');
            if (lb != std::string::npos && fc.pin.back() == ']') {
                buses[fc.pin.substr(0, lb)][std::stoi(fc.pin.substr(lb + 1))] = fc.net;
                continue;
            }
            in.conns.push_back({fc.pin, {ref[fc.net]}});
        }
        for (auto& [pin, bits] : buses) {
            gnl::Conn c;
            c.port = pin;
            for (int i = bits.rbegin()->first; i >= 0; i--) {
                auto it = bits.find(i);
                if (it != bits.end()) { c.refs.push_back(ref[it->second]); continue; }
                gnl::Ref r;  // an unconnected bit in the middle of a bus
                r.kind = gnl::Ref::Net;
                r.name = "mdft_open_" + std::to_string(spare++);
                w.decl(r.name);
                c.refs.push_back(r);
            }
            in.conns.push_back(std::move(c));
        }
        w.insts.push_back(std::move(in));
    }
    return w;
}

// ------------------------------------------------------------------ insertion

struct Options {
    std::vector<std::string> files, libs;
    std::string top, out, scandef, tb, cellmap, models;
    std::string si = "scan_in", so = "scan_out", se = "scan_en";
    int chains = 1;
    int maxLength = 0;
    bool mixClocks = false;
    std::string style = "auto";  // auto, mux, cell
    std::vector<std::string> exclude;
    bool quiet = false;
    int threads = 0;
};

struct Flop {
    int inst = -1;
    std::string name, cell;      // the cell after insertion
    int clk = -1;                // the clock port's net
    std::string clkName;
    bool neg = false;            // shifts on the falling edge of the port
    bool pos = true;
    bool cellNeg = false;        // the cell itself is falling-edge
    std::string method;          // "cell" or "mux"
    std::string q;               // the net that carries Q
};

struct Chain {
    std::vector<int> flops;      // indices into Result::flops
    std::string si, so;          // port expressions
    std::vector<std::pair<int, std::string>> lockups;  // after flop #k of the chain, instance name
};

struct Result {
    std::string top;
    std::vector<Flop> flops;
    std::vector<Chain> chains;
    std::vector<std::pair<std::string, std::string>> unscanned;  // instance, reason
    size_t notFlops = 0;                               // of those, latches
    std::map<std::string, int> clocks;                 // clock port -> flops
    std::map<std::string, int> resets;                 // reset port -> active level
    int gatesTied = 0, lockups = 0, muxes = 0, swapped = 0;
    bool needLockup = false, needLockupN = false;
};

inline bool globMatch(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return globMatch(p + 1, s) || (*s && globMatch(p, s + 1));
    return *s && (*p == '?' || *p == *s) && globMatch(p + 1, s + 1);
}

// m is the editable module whose instances are nl's instances in order.
inline Result insert(gnl::Module& m, const gnl::Netlist& nl, const CellMap& cm, const Options& o) {
    Result r;
    r.top = m.name;
    for (const std::string& n : {o.si, o.so, o.se})
        if (m.find(n))
            throw Error("the design already has a net named '" + n +
                        "'; pick another name with -scan_in / -scan_out / -scan_en");

    Tracer tr(nl, cm, o.threads);
    auto portName = [&](int net) { return nl.ports[tr.inPort[net]].name; };

    // 1. Which flops can be scanned. Each instance is judged on its own (the
    // traces only read the netlist), so the instances are split over the
    // threads; the verdicts are merged in netlist order afterwards, which
    // makes the result the same for any thread count.
    struct Verdict {
        bool flop = false, latch = false;
        std::string skip;
        Flop f;
        std::vector<std::pair<std::string, int>> rsts;
        std::vector<int> gates;
    };
    std::vector<Verdict> verdicts(nl.insts.size());
    forEach(nl.insts.size(), o.threads, 4096, [&](size_t k) {
        const gnl::FlatInst& in = nl.insts[k];
        Verdict& v = verdicts[k];
        const CellDef* c = tr.defs[k];
        if (!c) return;
        if (c->kind == CellDef::Latch) {
            v.latch = true;
            v.skip = "a latch (" + in.cell + ") is not scannable";
            return;
        }
        if (c->kind == CellDef::Other && c->has("SEQ")) {
            v.skip = in.cell + " is sequential but not a plain D flop, and has no scan equivalent";
            return;
        }
        if (c->kind != CellDef::Flop && c->kind != CellDef::ScanFlop) return;
        for (const std::string& g : o.exclude)
            if (globMatch(g.c_str(), in.name.c_str())) { v.skip = "excluded with -exclude"; return; }

        Source ck = tr.trace(tr.pinNet(int(k), c->p("CK")));
        if (ck.k != Source::Port) {
            v.skip = "clock not controllable: " + (ck.k == Source::Const ? "tied to a constant" : ck.why);
            return;
        }
        for (const char* role : {"R", "S"}) {
            if (!c->has(role)) continue;
            int net = tr.pinNet(int(k), c->p(role));
            if (net < 0) continue;
            const char* what = role[0] == 'R' ? "reset" : "set";
            Source rs = tr.trace(net);
            bool lowActive = c->low.count(role) != 0;
            if (rs.k == Source::Const) {
                if ((rs.value == 1) != lowActive) {
                    v.skip = std::string("asynchronous ") + what + " is tied active";
                    return;
                }
                continue;
            }
            if (rs.k != Source::Port) {
                v.skip = std::string("asynchronous ") + what + " not controllable: " + rs.why;
                return;
            }
            v.rsts.emplace_back(portName(rs.net), (lowActive != rs.inverted) ? 0 : 1);
        }
        if (c->kind == CellDef::ScanFlop) {
            // Already a scan cell: usable only if nobody stitched it yet.
            for (const char* role : {"SI", "SE"}) {
                int net = tr.pinNet(int(k), c->p(role));
                if (net >= 0 && tr.trace(net).k != Source::Const) {
                    v.skip = "already a stitched scan cell";
                    return;
                }
            }
        }
        v.flop = true;
        v.gates = ck.gates;
        v.f.inst = int(k);
        v.f.name = in.name;
        v.f.clk = ck.net;
        v.f.clkName = portName(ck.net);
        v.f.cellNeg = c->low.count("CK") != 0;
        v.f.neg = v.f.cellNeg != ck.inverted;
        v.f.pos = !v.f.neg;
    });
    std::set<int> gates;
    for (size_t k = 0; k < verdicts.size(); k++) {
        Verdict& v = verdicts[k];
        if (!v.skip.empty()) {
            r.unscanned.emplace_back(nl.insts[k].name, v.skip);
            r.notFlops += v.latch;
        }
        if (!v.flop) continue;
        bool ok = true;
        for (auto& [p, lvl] : v.rsts) {
            auto it = r.resets.find(p);
            if (it != r.resets.end() && it->second != lvl) {
                r.unscanned.emplace_back(v.f.name, "'" + p + "' is used as a reset at both levels");
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        for (auto& [p, lvl] : v.rsts) r.resets[p] = lvl;
        for (int g : v.gates) gates.insert(g);
        r.flops.push_back(std::move(v.f));
    }
    verdicts.clear();
    verdicts.shrink_to_fit();
    // A port cannot be both a clock and a reset the tester holds.
    for (auto it = r.flops.begin(); it != r.flops.end();) {
        if (r.resets.count(it->clkName)) {
            r.unscanned.emplace_back(it->name, "'" + it->clkName + "' is both a clock and a reset");
            it = r.flops.erase(it);
        } else {
            ++it;
        }
    }
    for (const Flop& f : r.flops) r.clocks[f.clkName]++;

    // 2. The chains. Falling-edge flops first: a falling-edge flop feeding a
    // rising-edge one of the same clock is safe, the other order loses a bit.
    std::vector<int> order(r.flops.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
    auto domKey = [&](int i) { return std::tie(r.flops[i].pos, r.flops[i].clkName); };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return domKey(a) < domKey(b); });

    std::vector<std::vector<int>> groups;  // what may share a chain
    if (o.mixClocks) {
        if (!order.empty()) groups.push_back(order);
    } else {
        for (int i : order) {
            if (groups.empty() || domKey(groups.back().back()) != domKey(i)) groups.emplace_back();
            groups.back().push_back(i);
        }
    }
    std::vector<int> per(groups.size(), 1);
    if (o.maxLength > 0)
        for (size_t g = 0; g < groups.size(); g++)
            per[g] = int((groups[g].size() + o.maxLength - 1) / o.maxLength);
    int total = 0;
    for (int n : per) total += n;
    // Spend the chains that are left on the longest ones.
    while (total < o.chains) {
        size_t best = groups.size();
        double worst = 1.0;  // no point splitting a chain of one flop
        for (size_t g = 0; g < groups.size(); g++) {
            double len = double(groups[g].size()) / per[g];
            if (len > worst) { worst = len; best = g; }
        }
        if (best == groups.size()) break;
        per[best]++;
        total++;
    }
    for (size_t g = 0; g < groups.size(); g++) {
        size_t n = groups[g].size(), at = 0;
        for (int c = 0; c < per[g]; c++) {
            size_t len = n / per[g] + (size_t(c) < n % per[g] ? 1 : 0);
            Chain ch;
            ch.flops.assign(groups[g].begin() + at, groups[g].begin() + at + len);
            at += len;
            if (!ch.flops.empty()) r.chains.push_back(ch);
        }
    }
    int nch = int(r.chains.size());
    if (nch == 0) return r;

    // 3. Ports.
    auto addPort = [&](const std::string& name, gnl::Dir dir, int width) {
        gnl::Decl& d = m.decl(name);
        d.isPort = true;
        d.dir = dir;
        if (width > 1) { d.ranged = true; d.msb = width - 1; d.lsb = 0; }
        m.portOrder.push_back(name);
    };
    addPort(o.se, gnl::Dir::In, 1);
    addPort(o.si, gnl::Dir::In, nch);
    addPort(o.so, gnl::Dir::Out, nch);
    auto netRef = [](const std::string& n) {
        gnl::Ref x;
        x.kind = gnl::Ref::Net;
        x.name = n;
        return x;
    };
    auto bitRef = [&](const std::string& n, int i) {
        gnl::Ref x = netRef(n);
        if (nch > 1) { x.kind = gnl::Ref::Bit; x.msb = x.lsb = i; }
        return x;
    };
    auto freshNet = [&](std::string want) {
        while (m.declIdx.count(want)) want += "_";
        m.decl(want);
        return netRef(want);
    };
    std::unordered_set<std::string> newInsts;
    auto conn = [](gnl::Inst& in, const std::string& pin) -> gnl::Conn* {
        for (gnl::Conn& c : in.conns)
            if (c.port == pin) return &c;
        return nullptr;
    };
    auto setConn = [&](gnl::Inst& in, const std::string& pin, const gnl::Ref& x) {
        if (gnl::Conn* c = conn(in, pin)) c->refs = {x};
        else in.conns.push_back({pin, {x}});
    };
    auto addInst = [&](const std::string& cell, const std::string& name,
                       std::initializer_list<std::pair<std::string, gnl::Ref>> pins) {
        gnl::Inst in;
        in.cell = cell;
        in.name = name;
        for (auto& [p, x] : pins) in.conns.push_back({p, {x}});
        m.insts.push_back(std::move(in));
    };
    gnl::Ref seRef = netRef(o.se);
    gnl::Ref seLow;  // scan_en inverted, made on first use
    auto inverted = [&](const gnl::Ref& x, const std::string& like, const std::string& name) {
        const CellDef* inv = cm.pick(CellDef::Inv, like);
        if (!inv) throw Error("an inverter is needed (" + name + ") but the library has none");
        gnl::Ref w = freshNet(name);
        addInst(inv->instName(), name + "_inv", {{inv->p("A"), x}, {inv->p("Y"), w}});
        return w;
    };

    // 4. Stitch.
    for (int ci = 0; ci < nch; ci++) {
        Chain& ch = r.chains[ci];
        gnl::Ref prev = bitRef(o.si, ci);
        ch.si = gnl::vref(prev);
        if (ch.si.back() == ' ') ch.si.pop_back();
        for (size_t k = 0; k < ch.flops.size(); k++) {
            Flop& f = r.flops[ch.flops[k]];
            std::string suffix;
            const CellDef* c = cm.find(m.insts[f.inst].cell, &suffix);

            // A lockup latch when the clock changes inside a chain.
            if (k > 0) {
                const Flop& pf = r.flops[ch.flops[k - 1]];
                if (pf.clk != f.clk && pf.neg == f.neg) {
                    // Open while the launching flop's clock pin is inactive.
                    const gnl::Inst& pin = m.insts[pf.inst];
                    const CellDef* pc = cm.find(pin.cell);
                    gnl::Ref ck;
                    for (const gnl::Conn& cc : pin.conns)
                        if (cc.port == pc->p("CK") && cc.refs.size() == 1) ck = cc.refs[0];
                    std::string name = "mdft_lockup_" + std::to_string(r.lockups++);
                    gnl::Ref w = freshNet(name + "_q");
                    bool openLow = !pf.cellNeg;
                    if (const CellDef* l = cm.pick(CellDef::Latch, pin.cell, "G", openLow)) {
                        gnl::Ref g = ck;
                        if ((l->low.count("G") != 0) != openLow) g = inverted(ck, pin.cell, name + "_g");
                        addInst(l->instName(), name, {{l->p("D"), prev}, {l->p("G"), g}, {l->p("Q"), w}});
                    } else {
                        (openLow ? r.needLockup : r.needLockupN) = true;
                        addInst(openLow ? "mdft_lockup" : "mdft_lockup_n", name, {{"D", prev}, {"CK", ck}, {"Q", w}});
                    }
                    ch.lockups.emplace_back(int(k) - 1, name);
                    prev = w;
                }
            }

            const CellDef* sc = nullptr;
            if (c->kind == CellDef::ScanFlop) sc = c;
            else if (o.style != "mux" && !c->scanCell.empty()) {
                sc = cm.find(c->scanCell);
                if (!sc || sc->kind != CellDef::ScanFlop)
                    throw Error("the scan cell '" + c->scanCell + "' of '" + c->name + "' is not a scan flop");
            }
            if (o.style == "cell" && !sc)
                throw Error("-style cell: '" + m.insts[f.inst].cell + "' (instance '" + f.name +
                            "') has no scan equivalent in the library");
            if (sc) {
                gnl::Inst& in = m.insts[f.inst];
                if (sc != c) {
                    // Swap the cell, pin by role.
                    for (gnl::Conn& cc : in.conns) {
                        std::string role = c->roleOf(cc.port);
                        if (role.empty() || !sc->has(role))
                            throw Error("scan cell '" + sc->name + "' has no pin for " + in.cell + "." + cc.port);
                        cc.port = sc->p(role);
                    }
                    in.cell = sc->name + suffix;
                    r.swapped++;
                }
                setConn(in, sc->p("SI"), prev);
                if (sc->low.count("SE")) {
                    if (seLow.name.empty()) seLow = inverted(seRef, in.cell, "mdft_scan_en_n");
                    setConn(m.insts[f.inst], sc->p("SE"), seLow);
                } else {
                    setConn(in, sc->p("SE"), seRef);
                }
                f.method = "cell";
            } else {
                const CellDef* mux = cm.pick(CellDef::Mux, m.insts[f.inst].cell);
                if (!mux)
                    throw Error("'" + m.insts[f.inst].cell + "' has no scan equivalent and the library has no "
                                "2-to-1 mux to put in front of it");
                gnl::Ref w = freshNet(f.name + "_scan_d");
                gnl::Ref d;
                d.kind = gnl::Ref::Const;
                d.bits = {0};
                if (gnl::Conn* dc = conn(m.insts[f.inst], c->p("D"))) {
                    if (dc->refs.size() > 1) throw Error("D of '" + f.name + "' is a concatenation");
                    if (dc->refs.size() == 1) d = dc->refs[0];
                }
                addInst(mux->instName(), f.name + "_scan_mux",
                        {{mux->p("A"), d}, {mux->p("B"), prev}, {mux->p("SEL"), seRef}, {mux->p("Y"), w}});
                setConn(m.insts[f.inst], c->p("D"), w);
                r.muxes++;
                f.method = "mux";
            }
            gnl::Inst& in = m.insts[f.inst];  // addInst may have moved it
            f.cell = in.cell;
            const CellDef* fc = sc ? sc : c;
            // The next scan input is this flop's Q.
            gnl::Conn* qc = conn(in, fc->p("Q"));
            if (qc && qc->refs.size() > 1) throw Error("Q of '" + f.name + "' is a concatenation");
            if (!qc || qc->refs.empty() || qc->refs[0].kind == gnl::Ref::Const) {
                setConn(in, fc->p("Q"), freshNet(f.name + "_scan_q"));
                qc = conn(in, fc->p("Q"));
            }
            prev = qc->refs[0];
            f.q = gnl::vref(prev);
            if (f.q.back() == ' ') f.q.pop_back();
        }
        gnl::Ref so = bitRef(o.so, ci);
        ch.so = gnl::vref(so);
        if (ch.so.back() == ' ') ch.so.pop_back();
        m.assigns.push_back({{so}, {prev}});
    }

    // 5. Clock gates on a scan clock stay open while shifting.
    for (int g : gates) {
        const CellDef* c = tr.defs[g];
        if (!c->has("TE")) continue;
        int net = tr.pinNet(g, c->p("TE"));
        if (net < 0 || net == nl.const0) {
            setConn(m.insts[g], c->p("TE"), seRef);
            r.gatesTied++;
        }
    }
    return r;
}

// The lockup latches, as models, for a library that has no latch:
// transparent while the launching flop's clock pin is at its inactive level,
// so the next clock domain sees the bit half a cycle late and a skew between
// the two clocks cannot shift it twice.
inline void writeModels(std::ostream& os, const Result& r) {
    os << "// Models of the cells mdft -insert_scan added that the library does not have.\n";
    if (r.needLockup)
        os << "// Lockup latch: transparent while CK is low.\n"
              "module mdft_lockup (D, CK, Q);\n  input D, CK;\n  output Q;\n  reg Q;\n"
              "  always @(CK or D) if (!CK) Q <= D;\nendmodule\n";
    if (r.needLockupN)
        os << "// Lockup latch: transparent while CK is high.\n"
              "module mdft_lockup_n (D, CK, Q);\n  input D, CK;\n  output Q;\n  reg Q;\n"
              "  always @(CK or D) if (CK) Q <= D;\nendmodule\n";
}

inline std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

// The chain description ATPG, logic BIST and compression read.
inline void writeScandef(std::ostream& os, const Result& r, const Options& o) {
    os << "{\n  \"tool\": \"mdft\",\n  \"format\": 1,\n  \"top\": " << jstr(r.top) << ",\n";
    os << "  \"scan_enable\": {\"port\": " << jstr(o.se) << ", \"active\": 1},\n";
    os << "  \"clocks\": [";
    bool first = true;
    for (auto& [c, n] : r.clocks) {
        os << (first ? "" : ", ") << "{\"port\": " << jstr(c) << ", \"off_state\": 0, \"flops\": " << n << "}";
        first = false;
    }
    os << "],\n  \"resets\": [";
    first = true;
    for (auto& [p, lvl] : r.resets) {
        os << (first ? "" : ", ") << "{\"port\": " << jstr(p) << ", \"active\": " << lvl << "}";
        first = false;
    }
    os << "],\n  \"procedures\": {\n"
          "    \"shift\": \"scan_enable active, resets inactive, apply scan_in, pulse every clock, strobe scan_out\",\n"
          "    \"capture\": \"scan_enable inactive, apply inputs, strobe outputs, pulse one clock\"\n  },\n";
    os << "  \"chains\": [\n";
    for (size_t ci = 0; ci < r.chains.size(); ci++) {
        const Chain& ch = r.chains[ci];
        os << "    {\"name\": \"chain" << ci << "\", \"scan_in\": " << jstr(ch.si) << ", \"scan_out\": "
           << jstr(ch.so) << ", \"length\": " << ch.flops.size() << ",\n     \"cells\": [\n";
        for (size_t k = 0; k < ch.flops.size(); k++) {
            const Flop& f = r.flops[ch.flops[k]];
            os << "       {\"inst\": " << jstr(f.name) << ", \"cell\": " << jstr(f.cell) << ", \"clock\": "
               << jstr(f.clkName) << ", \"edge\": \"" << (f.neg ? "neg" : "pos") << "\", \"q\": " << jstr(f.q)
               << ", \"scan\": \"" << f.method << "\"}" << (k + 1 < ch.flops.size() ? "," : "") << "\n";
        }
        os << "     ],\n     \"lockups\": [";
        for (size_t k = 0; k < ch.lockups.size(); k++)
            os << (k ? ", " : "") << "{\"after\": " << jstr(r.flops[ch.flops[ch.lockups[k].first]].name)
               << ", \"inst\": " << jstr(ch.lockups[k].second) << "}";
        os << "]}" << (ci + 1 < r.chains.size() ? "," : "") << "\n";
    }
    os << "  ],\n  \"unscanned\": [";
    for (size_t k = 0; k < r.unscanned.size(); k++)
        os << (k ? ", " : "") << "\n    {\"inst\": " << jstr(r.unscanned[k].first) << ", \"reason\": "
           << jstr(r.unscanned[k].second) << "}";
    os << (r.unscanned.empty() ? "" : "\n  ") << "]\n}\n";
}

// A chain flush test: with scan_en high, shift a pattern into every chain and
// check that it comes out of scan_out exactly `length` clocks later.
inline void writeTb(std::ostream& os, const gnl::Module& m, const Result& r, const Options& o) {
    size_t maxLen = 0;
    for (const Chain& c : r.chains) maxLen = std::max(maxLen, c.flops.size());
    size_t cycles = 2 * maxLen + 8;
    std::string tb = r.top + "_scan_tb";
    os << "// Scan chain flush test for " << r.top << ", written by mdft -insert_scan.\n"
       << "`timescale 1ns/1ps\nmodule " << gnl::vname(tb) << ";\n";
    std::vector<const gnl::Decl*> ports;
    for (const std::string& p : m.portOrder)
        if (const gnl::Decl* d = m.find(p)) ports.push_back(d);
    for (const gnl::Decl* d : ports) {
        std::string rng = d->ranged ? "[" + std::to_string(d->msb) + ":" + std::to_string(d->lsb) + "] " : "";
        os << "  " << (d->dir == gnl::Dir::In ? "reg " : "wire ") << rng << gnl::vname(d->name) << ";\n";
    }
    os << "  " << gnl::vname(r.top) << " dut (";
    for (size_t i = 0; i < ports.size(); i++)
        os << (i ? ", " : "") << "." << gnl::vname(ports[i]->name) << "(" << gnl::vname(ports[i]->name) << ")";
    os << ");\n";
    os << "  integer c, k, errors, seed;\n";
    os << "  reg pat [0:" << r.chains.size() * cycles - 1 << "];\n";
    os << "  task pulse;\n    begin\n      #3;\n";
    for (auto& [c, n] : r.clocks) os << "      " << c << " = 1'b1;\n";
    os << "      #3;\n";
    for (auto& [c, n] : r.clocks) os << "      " << c << " = 1'b0;\n";
    os << "      #3;\n    end\n  endtask\n";
    os << "  initial begin\n    errors = 0;\n    seed = 32'h1234abcd;\n";
    os << "    for (k = 0; k < " << r.chains.size() * cycles << "; k = k + 1) begin\n"
          "      seed = seed * 1103515245 + 12345;\n      pat[k] = seed[16];\n    end\n";
    for (const gnl::Decl* d : ports)
        if (d->dir == gnl::Dir::In) os << "    " << gnl::vname(d->name) << " = 0;\n";
    for (auto& [p, lvl] : r.resets) os << "    " << p << " = 1'b" << (lvl ? 0 : 1) << ";\n";
    os << "    " << gnl::vname(o.se) << " = 1'b1;\n";
    os << "    for (c = 0; c < " << cycles << "; c = c + 1) begin\n";
    for (size_t ci = 0; ci < r.chains.size(); ci++)
        os << "      " << r.chains[ci].si << " = pat[" << ci * cycles << " + c];\n";
    os << "      pulse;\n";
    for (size_t ci = 0; ci < r.chains.size(); ci++) {
        size_t len = r.chains[ci].flops.size();
        std::string want = "pat[" + std::to_string(ci * cycles) + " + c - " + std::to_string(len - 1) + "]";
        os << "      if (c >= " << len - 1 << " && " << r.chains[ci].so << " !== " << want << ") begin\n"
           << "        errors = errors + 1;\n"
           << "        if (errors <= 10) $display(\"chain" << ci << " cycle %0d: scan_out=%b expected %b\", c, "
           << r.chains[ci].so << ", " << want << ");\n      end\n";
    }
    os << "      #1;\n    end\n";
    os << "    if (errors == 0) $display(\"SCAN FLUSH PASS: " << r.chains.size() << " chain"
       << (r.chains.size() == 1 ? "" : "s") << ", " << r.flops.size() << " flops\");\n"
       << "    else $display(\"SCAN FLUSH FAIL: %0d errors\", errors);\n    $finish;\n  end\nendmodule\n";
}

inline void usage() {
    std::cout <<
        "usage: mdft -insert_scan netlist.v ... -o scanned.v [options]\n"
        "  stitch the flops of a gate netlist into scan chains\n\n"
        "  -top M           the top module (default: the one nothing instantiates)\n"
        "  -o FILE          the scan-inserted netlist (default: standard output)\n"
        "  -lib FILE        a Liberty library: flops, scan cells, muxes, latches, clock gates\n"
        "                   (repeatable; without one, msynth's generic cells are assumed)\n"
        "  -cellmap FILE    more cells, or overrides: flop/scanflop/buf/inv/mux/latch/icg lines\n"
        "  -chains N        how many chains (default 1; at least one per clock domain)\n"
        "  -max_length L    no chain longer than L flops\n"
        "  -mix_clocks      let clock domains share a chain, with lockup latches\n"
        "  -style S         auto: a scan cell when the library has one, else a mux before D\n"
        "                   mux: always a mux;  cell: always a scan cell\n"
        "  -scandef FILE    the chain description for ATPG (JSON)\n"
        "  -tb FILE         a chain flush testbench\n"
        "  -models FILE     models of added cells the library lacks (lockup latches)\n"
        "  -exclude GLOB    leave these flop instances out (repeatable)\n"
        "  -scan_in NAME  -scan_out NAME  -scan_en NAME   port names\n"
        "  -threads N       worker threads (default: every core; the result does not depend on N)\n"
        "  -quiet           no report\n";
}

inline int main(const std::vector<std::string>& args) {
    Options o;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw Error(a + " needs a value");
            return args[++i];
        };
        auto num = [&]() {
            std::string v = next();
            try {
                size_t used = 0;
                int n = std::stoi(v, &used);
                if (used != v.size() || n < 1) throw 0;
                return n;
            } catch (...) {
                throw Error(a + " needs a positive number, got '" + v + "'");
            }
        };
        if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
        else if (a == "-top") o.top = next();
        else if (a == "-o") o.out = next();
        else if (a == "-lib") o.libs.push_back(next());
        else if (a == "-chains") o.chains = num();
        else if (a == "-max_length") o.maxLength = num();
        else if (a == "-mix_clocks") o.mixClocks = true;
        else if (a == "-style") {
            o.style = next();
            if (o.style != "auto" && o.style != "mux" && o.style != "cell")
                throw Error("-style is auto, mux or cell");
        }
        else if (a == "-cellmap") o.cellmap = next();
        else if (a == "-scandef") o.scandef = next();
        else if (a == "-tb") o.tb = next();
        else if (a == "-models") o.models = next();
        else if (a == "-exclude") o.exclude.push_back(next());
        else if (a == "-scan_in") o.si = next();
        else if (a == "-scan_out") o.so = next();
        else if (a == "-scan_en") o.se = next();
        else if (a == "-threads") o.threads = num();
        else if (a == "-quiet" || a == "-q") o.quiet = true;
        else if (!a.empty() && a[0] == '-') throw Error("unknown option '" + a + "' (see mdft -insert_scan -help)");
        else o.files.push_back(a);
    }
    if (o.files.empty()) throw Error("no netlist given (see mdft -insert_scan -help)");
    if (o.threads <= 0) o.threads = int(par::defaultThreads());

    CellMap cm;
    if (o.libs.empty()) cm = CellMap::builtin();
    for (const std::string& l : o.libs) cm.addLiberty(liberty::load(l));
    if (!o.cellmap.empty()) cm.addFile(o.cellmap);

    // Parse the files, each on its own thread; the modules are then taken in
    // command-line order.
    gnl::Source src;
    {
        std::vector<gnl::Source> parts(o.files.size());
        forEach(o.files.size(), o.threads, 1, [&](size_t i) { gnl::read(o.files[i], parts[i]); });
        for (size_t i = 0; i < parts.size(); i++)
            for (gnl::Module& mod : parts[i].modules) {
                if (src.byName.count(mod.name))
                    throw Error(o.files[i] + ": module '" + mod.name + "' is defined twice");
                src.byName[mod.name] = int(src.modules.size());
                src.modules.push_back(std::move(mod));
            }
    }
    if (o.top.empty()) {
        std::vector<std::string> tops = gnl::tops(src);
        if (tops.empty()) throw Error("no top module found; name one with -top");
        if (tops.size() > 1) {
            std::string all;
            for (auto& s : tops) all += " " + s;
            throw Error("more than one top module (" + all.substr(1) + "); name one with -top");
        }
        o.top = tops[0];
    }
    const gnl::Module* top = src.module(o.top);
    if (!top) throw Error("no module named '" + o.top + "'");
    gnl::Netlist nl = gnl::flatten(src, o.top);
    bool hier = false;
    for (const gnl::Inst& in : top->insts)
        if (src.module(in.cell)) hier = true;
    // An already flat top is edited as it was written; a hierarchy is
    // rebuilt flat, an instance u/v of the hierarchy keeping that name.
    gnl::Module work = hier ? fromFlat(nl, *top) : std::move(src.modules[src.byName[o.top]]);
    src = gnl::Source();
    Result r = insert(work, nl, cm, o);

    auto put = [&](const std::string& file, const std::string& text) {
        std::ofstream f(file, std::ios::binary);
        if (!f) throw Error("cannot write '" + file + "'");
        f << text;
    };
    {
        std::ostringstream net;
        net << "// Scan-inserted by mdft: " << r.flops.size() << " flops in " << r.chains.size() << " chain"
            << (r.chains.size() == 1 ? "" : "s") << ".\n";
        gnl::write(work, net);
        if (o.out.empty()) std::cout << net.str();
        else put(o.out, net.str());
    }
    if (!o.scandef.empty()) {
        std::ostringstream s;
        writeScandef(s, r, o);
        put(o.scandef, s.str());
    }
    if (!o.tb.empty()) {
        if (r.chains.empty()) throw Error("-tb: there is no chain to test");
        std::ostringstream s;
        writeTb(s, work, r, o);
        put(o.tb, s.str());
    }
    if (!o.models.empty()) {
        std::ostringstream s;
        writeModels(s, r);
        put(o.models, s.str());
    }

    std::ostream& os = o.out.empty() ? std::cerr : std::cout;
    if (!o.quiet) {
        size_t all = r.flops.size() + r.unscanned.size() - r.notFlops;
        os << "mdft: " << r.top << ": " << r.flops.size() << " of " << all << " flops scanned";
        if (all) os << " (" << (100 * r.flops.size() / all) << "%)";
        os << ", " << r.chains.size() << " chain" << (r.chains.size() == 1 ? "" : "s") << "\n";
        for (size_t ci = 0; ci < r.chains.size(); ci++) {
            const Chain& ch = r.chains[ci];
            std::set<std::string> doms;
            for (int f : ch.flops) doms.insert(r.flops[f].clkName + (r.flops[f].neg ? " (falling)" : ""));
            os << "  chain" << ci << ": " << ch.flops.size() << (ch.flops.size() == 1 ? " flop" : " flops")
               << ", " << ch.si << " -> " << ch.so << ", clock";
            bool first = true;
            for (auto& s : doms) { os << (first ? " " : ", ") << s; first = false; }
            if (!ch.lockups.empty())
                os << ", " << ch.lockups.size() << (ch.lockups.size() == 1 ? " lockup latch" : " lockup latches");
            os << "\n";
        }
        if (r.swapped || r.muxes)
            os << "  " << r.swapped << " swapped for scan cells, " << r.muxes << " given a mux\n";
        if (r.gatesTied)
            os << "  " << r.gatesTied << (r.gatesTied == 1 ? " clock gate" : " clock gates")
               << " held open by " << o.se << "\n";
        size_t shown = 0;
        for (auto& [inst, why] : r.unscanned) {
            if (++shown > 50) {
                os << "  ... and " << r.unscanned.size() - 50 << " more (all are in the -scandef file)\n";
                break;
            }
            os << "  not scanned: " << inst << ": " << why << "\n";
        }
    }
    if ((r.needLockup || r.needLockupN) && o.models.empty())
        std::cerr << "mdft: warning: the library has no latch, so the lockup latches are instances of "
                     "mdft_lockup; write its model with -models FILE\n";
    return 0;
}

}  // namespace scan
