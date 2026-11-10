#pragma once
// power.hpp - the power computation behind mpower, callable from other tools
// (the power optimizer measures a design before and after with it).
//
//   pwr::Options o;                       // libraries, activity file, UPF, ...
//   pwr::Result r;
//   int rc = pwr::analyze(o, {"tool", "design.v", "--top", "dut"}, threads, r);
//   r.total.total();                      // watts; r.groups, r.domains, r.instances ...
//
// What is added up, per cell and per net (docs/POWER.md has the equations):
//   switching  0.5 * C * V^2 for every toggle of a net, C = the pin
//              capacitances on the net plus the wire estimate;
//   internal   the Liberty internal_power tables: energy per output toggle
//              (looked up by input transition and output load, averaged over
//              the related pins by their toggle rates and over `when`
//              conditions by their probability) and energy per input toggle
//              (the clock pin of a flop);
//   leakage    cell_leakage_power, or the leakage_power groups weighted by
//              the probability of each `when` state.
//
// Activity is SAIF, VCD or FST (saif.hpp, wave_reader.hpp). Nets the file
// does not name get activity by propagation through the cells' functions
// (probability and transition density), so RTL activity on a gate netlist,
// or no activity at all with a clock period, still gives an answer.
//
// A design that is still RTL has no cells: each register bit is costed as the
// library's flop and each block of logic as a number of reference gates
// estimated from its operators. That is an estimate for comparing
// alternatives early, not a sign-off number.
//
// The design comes through the slang front end msim uses (lintir::FrontEnd),
// so RTL, gate netlists and a mix of both elaborate the same way and nets
// carry the names msim writes into its waveforms. Cells are library cells
// when their module name is a Liberty cell, with or without a Verilog model.
//
// Every per-instance step writes only that instance's results and the sums
// are taken in instance order, so the answer is the same on any thread count.
#include <chrono>
#include <cinttypes>
#include <iomanip>
#include <optional>
#include <thread>

#include "liberty.hpp"
#include "lintir.hpp"
#include "parallel.hpp"
#include "saif.hpp"
#include "spef.hpp"
#include "upf.hpp"
#include "version.hpp"
#include "wave_reader.hpp"

#include "slang/ast/EvalContext.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/types/AllTypes.h"

namespace pwr {

using namespace lintir;


// The generic technology used when no -lib is given: enough to rank RTL
// alternatives against each other. The numbers are illustrative values for a
// generic 1.0 V process, not any foundry's.
inline const char* const kGenericLib = R"LIB(
library (mpower_generic) {
  time_unit : "1ns";
  voltage_unit : "1V";
  leakage_power_unit : "1nW";
  capacitive_load_unit (1, ff);
  nom_voltage : 1.0;
  cell (INV) {
    area : 0.8; cell_leakage_power : 2.0;
    pin (A) { direction : input; capacitance : 0.9; }
    pin (Y) { direction : output; function : "!A";
      internal_power () { related_pin : "A";
        rise_power (scalar) { values ("0.30"); } fall_power (scalar) { values ("0.25"); } } }
  }
  cell (NAND2) {
    area : 1.1; cell_leakage_power : 3.0;
    pin (A) { direction : input; capacitance : 1.0; }
    pin (B) { direction : input; capacitance : 1.0; }
    pin (Y) { direction : output; function : "!(A & B)";
      internal_power () { related_pin : "A";
        rise_power (scalar) { values ("0.45"); } fall_power (scalar) { values ("0.40"); } }
      internal_power () { related_pin : "B";
        rise_power (scalar) { values ("0.50"); } fall_power (scalar) { values ("0.42"); } } }
  }
  cell (DFF) {
    area : 4.5; cell_leakage_power : 12.0;
    ff (IQ, IQN) { clocked_on : "CK"; next_state : "D"; }
    pin (D) { direction : input; capacitance : 1.1; }
    pin (CK) { direction : input; clock : true; capacitance : 0.9;
      internal_power () { rise_power (scalar) { values ("0.90"); } fall_power (scalar) { values ("0.80"); } } }
    pin (Q) { direction : output; function : "IQ";
      internal_power () { related_pin : "CK";
        rise_power (scalar) { values ("1.60"); } fall_power (scalar) { values ("1.50"); } } }
  }
}
)LIB";

// ------------------------------------------------------- library, per cell

// A boolean function of a cell's pins as a truth table, so its probability
// under independent pin probabilities is one pass over the table.
struct BoolFn {
    std::vector<int> pins;     // the cell pin of each variable
    std::vector<uint8_t> tt;   // 2^n rows; row bit k is variable k
    bool ok = false;

    double prob(const double* p) const {
        if (!ok) return 0.5;
        size_t n = pins.size();
        double sum = 0;
        for (size_t r = 0; r < tt.size(); r++) {
            if (!tt[r]) continue;
            double w = 1;
            for (size_t k = 0; k < n; k++) {
                double pk = p[pins[k]];
                w *= (r >> k) & 1 ? pk : 1 - pk;
            }
            sum += w;
        }
        return sum;
    }
    // Transition density (Najm): sum over inputs of P(df/dx) * D(x).
    double density(const double* p, const double* d) const {
        if (!ok) return 0;
        size_t n = pins.size();
        double sum = 0;
        for (size_t k = 0; k < n; k++) {
            if (d[pins[k]] == 0) continue;
            double pd = 0;   // probability that flipping variable k flips the output
            for (size_t r = 0; r < tt.size(); r++) {
                if ((r >> k) & 1) continue;
                if (tt[r] == tt[r | ((size_t)1 << k)]) continue;
                double w = 1;
                for (size_t j = 0; j < n; j++) {
                    if (j == k) continue;
                    double pj = p[pins[j]];
                    w *= (r >> j) & 1 ? pj : 1 - pj;
                }
                pd += w;
            }
            sum += pd * d[pins[k]];
        }
        return sum;
    }
};

struct PowerArcI {
    int related = -1;
    BoolFn when;
    bool hasWhen = false;
    const liberty::PowerArc* a = nullptr;
};
struct PinI {
    const liberty::Pin* p = nullptr;
    bool out = false;
    BoolFn fn;                 // an output's function of the input pins
    int stateOut = 0;          // 1: the function is the flop state (Q), 2: its inverse (QN)
    std::vector<PowerArcI> arcs;
    std::vector<std::pair<int, const liberty::Arc*>> trans;   // related pin, arc with transition tables
};
struct CellI {
    const liberty::Cell* c = nullptr;
    const liberty::Library* lib = nullptr;
    std::vector<PinI> pins;
    std::vector<std::pair<BoolFn, double>> leak;
    BoolFn nextState;
    bool seq = false, icg = false;
    int clockPin = -1;
    double eUnit = 1e-12;      // joules per library energy unit
    double leakUnit = 1e-9;    // watts per library leakage unit
    double vnom = 1.0;
};

inline double unitValue(const std::string& s, double dflt, const char* base) {
    if (s.empty()) return dflt;
    double v = std::atof(s.c_str());
    if (v == 0) v = 1;
    std::string u;
    for (char c : s)
        if (std::isalpha((unsigned char)c)) u += (char)std::tolower((unsigned char)c);
    std::string b = base;
    if (u.size() > b.size() && u.compare(u.size() - b.size(), b.size(), b) == 0) {
        std::string pre = u.substr(0, u.size() - b.size());
        if (pre == "m") return v * 1e-3;
        if (pre == "u") return v * 1e-6;
        if (pre == "n") return v * 1e-9;
        if (pre == "p") return v * 1e-12;
        if (pre == "f") return v * 1e-15;
        if (pre == "k") return v * 1e3;
    }
    return v;
}

struct Libs {
    std::vector<std::unique_ptr<liberty::Library>> libs;
    std::vector<std::unique_ptr<CellI>> infos;
    std::unordered_map<std::string, int> byName;   // cell name -> infos index, built on demand

    const liberty::Cell* find(const std::string& n, const liberty::Library** lib = nullptr) const {
        for (auto& l : libs)
            if (auto* c = l->cell(n)) {
                if (lib) *lib = l.get();
                return c;
            }
        return nullptr;
    }
    static BoolFn fnOf(const liberty::Cell& c, const std::string& text) {
        BoolFn f;
        if (text.empty()) return f;
        liberty::Expr e;
        try {
            e = liberty::parseFunction(text);
        } catch (const liberty::Error&) {
            return f;
        }
        auto vars = e.vars();
        if (vars.size() > 16) return f;
        for (auto& v : vars) {
            int idx = -1;
            for (size_t i = 0; i < c.pins.size(); i++)
                if (c.pins[i].name == v) idx = (int)i;
            if (idx < 0) return f;   // names a state variable (IQ): not a function of the pins
            f.pins.push_back(idx);
        }
        f.tt.resize((size_t)1 << vars.size());
        for (size_t r = 0; r < f.tt.size(); r++)
            f.tt[r] = e.eval([&](const std::string& name) {
                for (size_t k = 0; k < vars.size(); k++)
                    if (vars[k] == name) return (bool)((r >> k) & 1);
                return false;
            });
        f.ok = true;
        return f;
    }
    int info(const std::string& name) {
        auto it = byName.find(name);
        if (it != byName.end()) return it->second;
        const liberty::Library* lib = nullptr;
        const liberty::Cell* c = find(name, &lib);
        if (!c) return byName[name] = -1;
        auto ci = std::make_unique<CellI>();
        ci->c = c;
        ci->lib = lib;
        double vu = unitValue(lib->root->str("voltage_unit"), 1.0, "v");
        ci->eUnit = lib->capScale * 1e-12 * vu * vu;
        ci->leakUnit = unitValue(lib->leakagePowerUnit, 1e-9, "w");
        ci->vnom = lib->nomVoltage > 0 ? lib->nomVoltage * vu : 1.0;
        ci->seq = c->sequential();
        ci->icg = c->group && c->group->has("clock_gating_integrated_cell");
        auto pinIdx = [&](const std::string& n) {
            for (size_t i = 0; i < c->pins.size(); i++)
                if (c->pins[i].name == n) return (int)i;
            return -1;
        };
        std::string iq, iqn, clk;
        if (!c->ffs.empty()) {
            iq = c->ffs[0].iq;
            iqn = c->ffs[0].iqn;
            clk = c->ffs[0].clockedOn;
            ci->nextState = fnOf(*c, c->ffs[0].nextState);
        } else if (!c->latches.empty()) {
            iq = c->latches[0].iq;
            iqn = c->latches[0].iqn;
            clk = c->latches[0].enable;
            ci->nextState = fnOf(*c, c->latches[0].dataIn);
        }
        if (!clk.empty()) {
            BoolFn cf = fnOf(*c, clk);
            if (cf.ok && cf.pins.size() == 1) ci->clockPin = cf.pins[0];
        }
        for (size_t i = 0; i < c->pins.size(); i++) {
            const liberty::Pin& p = c->pins[i];
            PinI pi;
            pi.p = &p;
            pi.out = p.dir == liberty::Dir::Out || p.dir == liberty::Dir::InOut;
            if (ci->clockPin < 0 && p.clock && !pi.out) ci->clockPin = (int)i;
            if (pi.out && !p.function.empty()) {
                pi.fn = fnOf(*c, p.function);
                if (!pi.fn.ok && ci->seq) {
                    std::string f = p.function;
                    f.erase(std::remove_if(f.begin(), f.end(), [](char ch) { return std::isspace((unsigned char)ch); }),
                            f.end());
                    if (f == iq) pi.stateOut = 1;
                    else if (f == iqn || f == "!" + iq || f == iq + "'") pi.stateOut = 2;
                }
            }
            for (auto& a : p.power) {
                PowerArcI x;
                x.a = &a;
                // "A B": several related pins share the group; the first is enough for a weight
                std::string rel = a.related.substr(0, a.related.find(' '));
                x.related = pinIdx(rel);
                if (!a.when.empty()) {
                    x.when = fnOf(*c, a.when);
                    x.hasWhen = x.when.ok;
                }
                pi.arcs.push_back(std::move(x));
            }
            ci->pins.push_back(std::move(pi));
        }
        for (auto& a : c->arcs) {
            if (a.riseTrans.empty() && a.fallTrans.empty()) continue;
            int to = pinIdx(a.to), rel = pinIdx(a.related);
            if (to >= 0) ci->pins[to].trans.push_back({rel, &a});
        }
        for (auto& l : c->leakages) {
            if (l.when.empty()) continue;
            BoolFn w = fnOf(*c, l.when);
            if (w.ok) ci->leak.push_back({std::move(w), l.value});
        }
        infos.push_back(std::move(ci));
        return byName[name] = (int)infos.size() - 1;
    }
};

// ------------------------------------------------------------- the design

enum Group { G_CLOCK, G_REG, G_SEQ, G_COMB, G_MEM, G_IN, G_COUNT };
inline const char* const kGroupName[G_COUNT] = {"clock_network", "register", "sequential", "combinational", "memory",
                                         "primary_inputs"};

struct Net {
    double cap = 0;        // pF: the pins on the net
    double wire = 0;       // pF: the wire estimate
    int fan = 0;
    int drv = -1;          // driving instance, -1: a primary input or undriven
    double p1 = 0.5;       // probability of 1
    double rate = 0;       // toggles per second
    double slew = 0;       // ns
    double energy = 0;     // J per toggle (switching + internal of the pins on it), for -peak
    int act = -1;          // the activity bit that annotated it
    bool ann = false, known = false, clock = false, konst = false, used = false;
    bool port = false;     // a port of the top module
};

struct Inst {
    std::string name;      // instance path below the top
    int ci = -1;           // library cell
    std::vector<int> net;  // per cell pin: net or -1
    bool cloud = false;    // RTL logic: `ge` reference gates, between the nets in io
    bool virt = false;     // a flop inferred from RTL
    double ge = 0;
    std::vector<int> io;   // cloud: nets read, then nets driven
    int nIn = 0;
    int group = G_COMB;
    int domain = -1;
    double pint = 0, psw = 0, pleak = 0;
    double pswClock = 0;   // the part of psw that is on clock nets
    std::vector<double> pinE;   // J per toggle of each pin's net (cloud: one value for every io net)
};

struct SymRec {
    const Symbol* sym = nullptr;
    std::string path;      // name below the top; empty until the walk reaches it
    int base = 0, width = 0;
    bool descending = true;
    int lower = 0;
    bool vector = false;   // a packed vector with a range: bits are name[i]
};

struct Options {
    std::string top, activity, strip, upfPath, powerTop, json, waveform, writeSaif, rtlFf, rtlGate, unit, spef;
    std::vector<std::string> libs;
    std::vector<std::pair<std::string, double>> clocks;   // name, period in ns
    double voltage = 0, inSlew = 0.05, wireCap = -1, toggle = 0.1, interval = 0;
    bool peak = false, quiet = false;
    int hier = 0, cells = 0;
    uint64_t memBits = 4096;
};

struct PDesign {
    Compilation* comp = nullptr;
    Libs* libs = nullptr;
    Options* opt = nullptr;
    // raw bits and the union-find over them; raw 0 and 1 are the constants
    std::vector<int> up;
    std::vector<SymRec> syms;
    std::unordered_map<const Symbol*, int> symIdx;
    std::vector<int> netOf;   // raw bit -> net
    std::vector<Net> nets;
    std::vector<Inst> insts;
    std::map<std::string, int> blackBoxes;
    std::vector<std::string> notes;
    std::vector<const Symbol*> topPorts;
    size_t deadGates = 0;
    int refFf = -1, refGate = -1;
    double refGateCap = 0;    // pF, the average input pin of the reference gate
    bool anyRtl = false;
    // RTL logic waiting for the nets to be final
    struct Logic {
        std::string scope;
        std::vector<const Symbol*> reads, writes, regs;
        const Symbol* clk = nullptr;
        double ge = 0;
        bool seq = false;
    };
    std::vector<Logic> logic;

    int find(int x) {
        while (up[x] != x) {
            up[x] = up[up[x]];
            x = up[x];
        }
        return x;
    }
    void join(int a, int b) {
        if (a < 0 || b < 0) return;
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (a > b) std::swap(a, b);   // the constants (0, 1) stay roots
        up[b] = a;
    }
    int rec(const Symbol& s) {
        auto it = symIdx.find(&s);
        if (it != symIdx.end()) return it->second;
        SymRec r;
        r.sym = &s;
        uint64_t w = 0;
        if (ValueSymbol::isKind(s.kind)) {
            const Type& t = s.as<ValueSymbol>().getType();
            w = t.isFixedSize() ? t.getBitstreamWidth() : 0;
            const Type& ct = t.getCanonicalType();
            if (ct.kind == SymbolKind::PackedArrayType && ct.getArrayElementType()->getBitWidth() == 1) {
                auto fr = ct.getFixedRange();
                r.vector = true;
                r.descending = fr.isDescending();
                r.lower = fr.lower();
            }
        }
        if (w > ((uint64_t)1 << 24)) w = 0;   // too big to follow per bit
        r.base = (int)up.size();
        r.width = (int)w;
        for (uint64_t i = 0; i < w; i++) up.push_back((int)up.size());
        syms.push_back(std::move(r));
        return symIdx[&s] = (int)syms.size() - 1;
    }
    std::string bitName(const SymRec& r, int k, bool zeroBased = false) const {
        if (r.width == 1 && !r.vector) return r.path;
        int idx = k;
        if (r.vector && !zeroBased) idx = r.descending ? r.lower + k : r.lower + (r.width - 1 - k);
        return r.path + "[" + std::to_string(idx) + "]";
    }

    // The raw bits an expression is wired to, least significant first. -1 is
    // a bit that is not plain wiring; `pure` is cleared when there is one.
    // A select chain on one symbol (x, x[3], x[7:4], x.f, x[2][5]) is a run
    // of that symbol's raw bits: where it starts and how long it is. Nothing
    // is copied, so one bit of a 100,000-bit bus costs the same as a scalar.
    bool locate(const Expression& e, const Scope& scope, int& base, uint64_t& width) {
        auto constIdx = [&](const Expression& x) -> std::optional<int32_t> {
            if (const ConstantValue* k = x.getConstant(); k && k->isInteger()) return k->integer().as<int32_t>();
            EvalContext ec(ASTContext(scope, LookupLocation::max));
            ConstantValue cv = x.eval(ec);
            if (!cv.isInteger()) return std::nullopt;
            return cv.integer().as<int32_t>();
        };
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {
                const Symbol& s = e.kind == ExpressionKind::NamedValue ? (const Symbol&)e.as<NamedValueExpression>().symbol
                                                                      : (const Symbol&)e.as<HierarchicalValueExpression>().symbol;
                if (s.kind == SymbolKind::Parameter || s.kind == SymbolKind::EnumValue) return false;
                const SymRec& r = syms[rec(s)];
                if (r.width == 0) return false;
                base = r.base;
                width = (uint64_t)r.width;
                return true;
            }
            case ExpressionKind::ElementSelect: {
                const auto& es = e.as<ElementSelectExpression>();
                if (!locate(es.value(), scope, base, width)) return false;
                auto idx = constIdx(es.selector());
                const Type& vt = es.value().type->getCanonicalType();
                if (!idx || !vt.hasFixedRange()) return false;
                auto fr = vt.getFixedRange();
                uint64_t ew = e.type->getBitstreamWidth();
                if (!fr.containsPoint(*idx)) return false;
                uint64_t off = (uint64_t)fr.translateIndex(*idx) * ew;
                if (off + ew > width) return false;
                base += (int)off;
                width = ew;
                return true;
            }
            case ExpressionKind::RangeSelect: {
                const auto& rs = e.as<RangeSelectExpression>();
                if (rs.getSelectionKind() != RangeSelectionKind::Simple) return false;
                if (!locate(rs.value(), scope, base, width)) return false;
                auto l = constIdx(rs.left()), r = constIdx(rs.right());
                const Type& vt = rs.value().type->getCanonicalType();
                if (!l || !r || !vt.hasFixedRange()) return false;
                auto fr = vt.getFixedRange();
                if (!fr.containsPoint(*l) || !fr.containsPoint(*r)) return false;
                uint64_t ew = vt.getArrayElementType() ? vt.getArrayElementType()->getBitstreamWidth() : 1;
                uint64_t a = (uint64_t)std::min(fr.translateIndex(*l), fr.translateIndex(*r)) * ew;
                uint64_t w = e.type->getBitstreamWidth();
                if (a + w > width) return false;
                base += (int)a;
                width = w;
                return true;
            }
            case ExpressionKind::MemberAccess: {
                const auto& ma = e.as<MemberAccessExpression>();
                if (ma.member.kind != SymbolKind::Field || !ma.value().type->isIntegral()) return false;
                if (!locate(ma.value(), scope, base, width)) return false;
                uint64_t off = ma.member.as<FieldSymbol>().bitOffset, w = e.type->getBitstreamWidth();
                if (off + w > width) return false;
                base += (int)off;
                width = w;
                return true;
            }
            default: return false;
        }
    }

    std::vector<int> bitsOf(const Expression& e, const Scope& scope, bool& pure) {
        std::vector<int> out;
        {
            int base = 0;
            uint64_t width = 0;
            if (locate(e, scope, base, width)) {
                out.resize((size_t)width);
                for (size_t i = 0; i < out.size(); i++) out[i] = base + (int)i;
                return out;
            }
        }
        auto unknown = [&](const Expression& x) {
            pure = false;
            out.assign(x.type->isFixedSize() ? (size_t)std::min<uint64_t>(x.type->getBitstreamWidth(), 1 << 20) : 0,
                       -1);
            return out;
        };
        auto constIdx = [&](const Expression& x) -> std::optional<int32_t> {
            EvalContext ec(ASTContext(scope, LookupLocation::max));
            ConstantValue cv = x.eval(ec);
            if (!cv.isInteger()) return std::nullopt;
            return cv.integer().as<int32_t>();
        };
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {
                const Symbol& s = e.kind == ExpressionKind::NamedValue ? (const Symbol&)e.as<NamedValueExpression>().symbol
                                                                      : (const Symbol&)e.as<HierarchicalValueExpression>().symbol;
                if (s.kind == SymbolKind::Parameter || s.kind == SymbolKind::EnumValue) break;   // a constant, below
                const SymRec& r = syms[rec(s)];
                if (r.width == 0) return unknown(e);
                for (int i = 0; i < r.width; i++) out.push_back(r.base + i);
                return out;
            }
            case ExpressionKind::ElementSelect: {
                const auto& es = e.as<ElementSelectExpression>();
                auto v = bitsOf(es.value(), scope, pure);
                auto idx = constIdx(es.selector());
                const Type& vt = es.value().type->getCanonicalType();
                if (!idx || !vt.hasFixedRange()) return unknown(e);
                auto fr = vt.getFixedRange();
                uint64_t ew = e.type->getBitstreamWidth();
                if (!fr.containsPoint(*idx)) return unknown(e);
                uint64_t off = (uint64_t)fr.translateIndex(*idx) * ew;
                if (off + ew > v.size()) return unknown(e);
                return std::vector<int>(v.begin() + (long)off, v.begin() + (long)(off + ew));
            }
            case ExpressionKind::RangeSelect: {
                const auto& rs = e.as<RangeSelectExpression>();
                auto v = bitsOf(rs.value(), scope, pure);
                auto l = constIdx(rs.left()), r = constIdx(rs.right());
                const Type& vt = rs.value().type->getCanonicalType();
                if (!l || !r || !vt.hasFixedRange() || rs.getSelectionKind() != RangeSelectionKind::Simple)
                    return unknown(e);
                auto fr = vt.getFixedRange();
                if (!fr.containsPoint(*l) || !fr.containsPoint(*r)) return unknown(e);
                uint64_t ew = vt.getArrayElementType() ? vt.getArrayElementType()->getBitstreamWidth() : 1;
                uint64_t a = (uint64_t)std::min(fr.translateIndex(*l), fr.translateIndex(*r)) * ew;
                uint64_t w = e.type->getBitstreamWidth();
                if (a + w > v.size()) return unknown(e);
                return std::vector<int>(v.begin() + (long)a, v.begin() + (long)(a + w));
            }
            case ExpressionKind::MemberAccess: {
                const auto& ma = e.as<MemberAccessExpression>();
                if (ma.member.kind != SymbolKind::Field || !ma.value().type->isIntegral()) return unknown(e);
                auto v = bitsOf(ma.value(), scope, pure);
                uint64_t off = ma.member.as<FieldSymbol>().bitOffset, w = e.type->getBitstreamWidth();
                if (off + w > v.size()) return unknown(e);
                return std::vector<int>(v.begin() + (long)off, v.begin() + (long)(off + w));
            }
            case ExpressionKind::Concatenation: {
                const auto& c = e.as<ConcatenationExpression>();
                auto ops = c.operands();
                for (size_t i = ops.size(); i-- > 0;) {
                    auto v = bitsOf(*ops[i], scope, pure);
                    out.insert(out.end(), v.begin(), v.end());
                }
                return out;
            }
            case ExpressionKind::Conversion: {
                const auto& cv = e.as<ConversionExpression>();
                if (!e.type->isIntegral() || !cv.operand().type->isIntegral()) return unknown(e);
                auto v = bitsOf(cv.operand(), scope, pure);
                size_t w = e.type->getBitWidth();
                bool sign = cv.operand().type->isSigned();
                int ext = sign && !v.empty() ? v.back() : 0;
                v.resize(w, ext);
                return v;
            }
            case ExpressionKind::Assignment:   // an output port's connection
                return bitsOf(e.as<AssignmentExpression>().left(), scope, pure);
            case ExpressionKind::EmptyArgument:
                return out;
            default: break;
        }
        // a constant: tied to 0 or 1
        if (e.type->isIntegral()) {
            EvalContext ec(ASTContext(scope, LookupLocation::max));
            ConstantValue cv = e.eval(ec);
            if (cv.isInteger()) {
                const SVInt& v = cv.integer();
                for (bitwidth_t i = 0; i < v.getBitWidth(); i++) {
                    logic_t b = v[(int32_t)i];
                    out.push_back(b.isUnknown() ? -1 : (b.value ? 1 : 0));
                }
                return out;
            }
        }
        return unknown(e);
    }

    // ---- RTL: how many reference gates a block of logic is worth ----
    struct Ge : ASTVisitor<Ge, VisitFlags(int(VisitFlags::Statements) | int(VisitFlags::Expressions))> {
        double ge = 0;
        static double w(const Expression& e) {
            return e.type->isFixedSize() ? (double)std::min<uint64_t>(e.type->getBitstreamWidth(), 4096) : 1;
        }
        static bool isConst(const Expression& e) {
            return e.kind == ExpressionKind::IntegerLiteral || e.kind == ExpressionKind::UnbasedUnsizedIntegerLiteral ||
                   (e.kind == ExpressionKind::Conversion && isConst(e.as<ConversionExpression>().operand())) ||
                   (e.kind == ExpressionKind::NamedValue &&
                    e.as<NamedValueExpression>().symbol.kind == SymbolKind::Parameter);
        }
        // The width an operand had before it was extended to the expression's width.
        static double nw(const Expression& e) {
            if (e.kind == ExpressionKind::Conversion && e.as<ConversionExpression>().isImplicit()) {
                const Expression& op = e.as<ConversionExpression>().operand();
                if (op.type->isIntegral()) return std::min(w(e), nw(op));
            }
            return w(e);
        }
        void handle(const BinaryExpression& e) {
            if (fixedIndex(e)) return;   // arithmetic on loop variables and constants: no hardware
            double n = std::max(nw(e.left()), nw(e.right()));
            bool k = isConst(e.left()) || isConst(e.right());
            double c = 0;
            switch (e.op) {
                case BinaryOperator::Add:
                case BinaryOperator::Subtract: c = 7 * n; break;
                case BinaryOperator::Multiply: c = 6 * nw(e.left()) * nw(e.right()); break;
                case BinaryOperator::Divide:
                case BinaryOperator::Mod: c = 12 * n * n; break;
                case BinaryOperator::BinaryAnd:
                case BinaryOperator::BinaryOr: c = n; break;
                case BinaryOperator::BinaryXor:
                case BinaryOperator::BinaryXnor: c = 2.5 * n; break;
                case BinaryOperator::Equality:
                case BinaryOperator::Inequality:
                case BinaryOperator::CaseEquality:
                case BinaryOperator::CaseInequality:
                case BinaryOperator::WildcardEquality:
                case BinaryOperator::WildcardInequality: c = 1.75 * n; break;
                case BinaryOperator::GreaterThanEqual:
                case BinaryOperator::GreaterThan:
                case BinaryOperator::LessThanEqual:
                case BinaryOperator::LessThan: c = 3 * n; break;
                case BinaryOperator::LogicalAnd:
                case BinaryOperator::LogicalOr:
                case BinaryOperator::LogicalImplication:
                case BinaryOperator::LogicalEquivalence: c = 1 + (w(e.left()) - 1) + (w(e.right()) - 1); break;
                case BinaryOperator::LogicalShiftLeft:
                case BinaryOperator::LogicalShiftRight:
                case BinaryOperator::ArithmeticShiftLeft:
                case BinaryOperator::ArithmeticShiftRight:
                    // a barrel shifter: one mux per bit per bit of the shift amount
                    c = isConst(e.right()) ? 0
                        : 2.0 * w(e.left()) * std::min(nw(e.right()), std::ceil(std::log2(std::max(2.0, w(e.left())))));
                    k = false;
                    break;
                default: c = n; break;
            }
            ge += k ? c * 0.4 : c;   // a constant operand folds more than half of it away
            visitDefault(e);
        }
        void handle(const UnaryExpression& e) {
            if (fixedIndex(e)) return;
            double n = w(e.operand());
            switch (e.op) {
                case UnaryOperator::BitwiseNot: ge += 0.7 * n; break;
                case UnaryOperator::Minus: ge += 3.5 * n; break;
                case UnaryOperator::BitwiseAnd:
                case UnaryOperator::BitwiseOr:
                case UnaryOperator::BitwiseNand:
                case UnaryOperator::BitwiseNor:
                case UnaryOperator::LogicalNot: ge += std::max(1.0, n - 1); break;
                case UnaryOperator::BitwiseXor:
                case UnaryOperator::BitwiseXnor: ge += 2.5 * std::max(1.0, n - 1); break;
                case UnaryOperator::Preincrement:
                case UnaryOperator::Predecrement:
                case UnaryOperator::Postincrement:
                case UnaryOperator::Postdecrement: ge += 3.5 * n; break;
                default: break;
            }
            visitDefault(e);
        }
        void handle(const ConditionalExpression& e) {
            ge += 2.5 * w(e);
            visitDefault(e);
        }
        // A select whose index is a loop variable is a fixed wire once the loop is unrolled.
        static bool fixedIndex(const Expression& e) {
            if (isConst(e)) return true;
            if (e.kind == ExpressionKind::Conversion) return fixedIndex(e.as<ConversionExpression>().operand());
            if (e.kind == ExpressionKind::BinaryOp)
                return fixedIndex(e.as<BinaryExpression>().left()) && fixedIndex(e.as<BinaryExpression>().right());
            if (e.kind == ExpressionKind::UnaryOp) return fixedIndex(e.as<UnaryExpression>().operand());
            if (e.kind != ExpressionKind::NamedValue) return false;
            const Symbol& s = e.as<NamedValueExpression>().symbol;
            return s.kind == SymbolKind::Variable && s.as<VariableSymbol>().lifetime == VariableLifetime::Automatic;
        }
        bool onLhs = false;
        std::set<std::pair<const Symbol*, const Symbol*>> muxes;
        void handle(const AssignmentExpression& a) {
            // the target side: a variable index there is an address decoder, not a mux
            onLhs = true;
            a.left().visit(*this);
            onLhs = false;
            a.right().visit(*this);
        }
        void handle(const ElementSelectExpression& e) {
            if (!fixedIndex(e.selector())) {
                const Type& vt = e.value().type->getCanonicalType();
                double n = vt.hasFixedRange() ? (double)std::min<uint64_t>(vt.getFixedRange().width(), 4096) : 2;
                // the same array read with the same index twice in a block is one mux
                bool again = !onLhs && !muxes.insert({oneName(e.value()), oneName(e.selector())}).second &&
                             oneName(e.value()) && oneName(e.selector());
                if (onLhs) ge += n;                                    // one enable per element
                else if (!again) ge += 1.8 * w(e) * std::max(1.0, n - 1);   // a mux tree over the elements
            }
            visitDefault(e);
        }
        // The bits a statement assigns, each target once: what an if or a case
        // puts a mux in front of. A target that only ever gets constants (a
        // decoder's control outputs) costs a fraction of a mux per bit.
        static double assigned(const Statement& s) {
            std::map<const Symbol*, std::pair<double, bool>> seen;   // width, all constant
            s.visit(makeVisitor([&](auto& v, const AssignmentExpression& a) {
                if (const Symbol* t = lhsSymbol(a.left())) {
                    auto [it, fresh] = seen.try_emplace(t, w(a.left()), true);
                    it->second.first = std::max(it->second.first, w(a.left()));
                    if (!isConst(a.right()) && !a.right().getConstant()) it->second.second = false;
                }
                v.visitDefault(a);
            }));
            double bits = 0;
            for (auto& [t, x] : seen) bits += x.second ? 0.25 * x.first : x.first;
            return bits;
        }
        void handle(const ConditionalStatement& s) {
            ge += 2.0 * assigned(s);
            visitDefault(s);
        }
        void handle(const CaseStatement& s) {
            double items = 0;
            for (auto& it : s.items) items += (double)it.expressions.size();
            // the item decode (a comparison with a constant each), and an AND-OR mux of the items;
            // nested ifs and cases inside the items add their own
            ge += 0.4 * w(s.expr) * items + 0.6 * std::max(1.0, items) * assigned(s);
            visitDefault(s);
        }
        // A for loop is that many copies of its body: for (i = 0; i < N; i++).
        void handle(const ForLoopStatement& s) {
            double trips = 1;
            if (s.stopExpr && s.stopExpr->kind == ExpressionKind::BinaryOp) {
                const auto& b = s.stopExpr->as<BinaryExpression>();
                if (const ConstantValue* cv = b.right().getConstant(); cv && cv->isInteger()) {
                    if (auto n = cv->integer().as<int32_t>(); n && *n > 0)
                        trips = b.op == BinaryOperator::LessThanEqual ? *n + 1.0 : *n;
                }
            }
            double before = ge;
            visitDefault(s);
            ge = before + (ge - before) * std::min(trips, 4096.0);
        }
    };

    // Is this continuous assign only wiring (assign a = b; assign y = {a, b[3:0]})?
    bool wiring(const AssignmentExpression& a, const Scope& scope) {
        bool pure = true;
        auto l = bitsOf(a.left(), scope, pure);
        auto r = bitsOf(a.right(), scope, pure);
        if (!pure || l.size() != r.size() || l.empty()) return false;
        for (size_t i = 0; i < l.size(); i++) join(l[i], r[i]);
        return true;
    }

    void addCell(const std::string& path, int ci, const std::vector<std::pair<std::string, std::vector<int>>>& conns) {
        const CellI& info = *libs->infos[ci];
        Inst in;
        in.name = path;
        in.ci = ci;
        in.net.assign(info.pins.size(), -1);
        for (auto& [port, bits] : conns) {
            for (size_t p = 0; p < info.pins.size(); p++) {
                const liberty::Pin& lp = *info.pins[p].p;
                if (lp.name == port && !bits.empty()) in.net[p] = bits[0];
                else if (lp.bus == port && lp.busBit >= 0) {
                    // a bus pin D[3]: the port's bit 3, counting from its least significant bit
                    int lo = INT32_MAX;
                    for (auto& q : info.pins)
                        if (q.p->bus == port && q.p->busBit >= 0) lo = std::min(lo, q.p->busBit);
                    size_t k = (size_t)(lp.busBit - lo);
                    if (k < bits.size()) in.net[p] = bits[k];
                }
            }
        }
        insts.push_back(std::move(in));
    }

    void walk(const Scope& sc, const std::string& path) {
        auto in = [&](std::string_view n) { return path.empty() ? std::string(n) : path + "." + std::string(n); };
        for (const Symbol& m : sc.members()) {
            switch (m.kind) {
                case SymbolKind::Variable:
                case SymbolKind::Net: {
                    SymRec& r = syms[rec(m)];
                    if (r.path.empty()) r.path = in(m.name);
                    break;
                }
                case SymbolKind::Instance: instance(m.as<InstanceSymbol>(), in(m.name), sc); break;
                case SymbolKind::InstanceArray: {
                    const auto& arr = m.as<InstanceArraySymbol>();
                    int32_t idx = arr.range.left, step = arr.range.left <= arr.range.right ? 1 : -1;
                    for (const Symbol* e : arr.elements) {
                        if (e->kind == SymbolKind::Instance)
                            instance(e->as<InstanceSymbol>(), in(std::string(arr.name) + "[" + std::to_string(idx) + "]"),
                                     sc);
                        idx += step;
                    }
                    break;
                }
                case SymbolKind::UninstantiatedDef: {
                    const auto& u = m.as<UninstantiatedDefSymbol>();
                    int ci = libs->info(std::string(u.definitionName));
                    if (ci < 0) {
                        blackBoxes[std::string(u.definitionName)]++;
                        break;
                    }
                    auto names = u.getPortNames();
                    auto conns = u.getPortConnections();
                    std::vector<std::pair<std::string, std::vector<int>>> cv;
                    const CellI& info = *libs->infos[ci];
                    for (size_t i = 0; i < conns.size(); i++) {
                        if (!conns[i] || conns[i]->kind != AssertionExprKind::Simple) continue;
                        bool pure = true;
                        auto bits = bitsOf(conns[i]->as<SimpleAssertionExpr>().expr, sc, pure);
                        // connected by position: the library's pin order
                        std::string pn = i < names.size() && !names[i].empty() ? std::string(names[i])
                                         : i < info.pins.size() ? info.pins[i].p->name : std::string();
                        cv.push_back({pn, std::move(bits)});
                    }
                    addCell(in(m.name), ci, cv);
                    break;
                }
                case SymbolKind::GenerateBlock: {
                    const auto& g = m.as<GenerateBlockSymbol>();
                    if (!g.isUninstantiated) walk(g, in(g.getExternalName()));
                    break;
                }
                case SymbolKind::GenerateBlockArray: {
                    const auto& ga = m.as<GenerateBlockArraySymbol>();
                    std::string an = ga.name.empty() ? std::string("genblk") : std::string(ga.name);
                    for (const auto* e : ga.entries)
                        if (!e->isUninstantiated)
                            walk(*e, in(an + "[" + (e->getArrayIndex() ? e->getArrayIndex()->toString() : std::string("?")) + "]"));
                    break;
                }
                case SymbolKind::ContinuousAssign: {
                    const Expression& e = m.as<ContinuousAssignSymbol>().getAssignment();
                    if (e.kind != ExpressionKind::Assignment) break;
                    const auto& a = e.as<AssignmentExpression>();
                    if (wiring(a, sc)) break;
                    Logic lg;
                    lg.scope = path;
                    namesIn(a.right(), lg.reads);
                    if (const Symbol* t = lhsSymbol(a.left())) lg.writes.push_back(t);
                    Ge g;
                    a.right().visit(g);
                    lg.ge = g.ge;
                    logic.push_back(std::move(lg));
                    break;
                }
                case SymbolKind::ProceduralBlock: {
                    const auto& p = m.as<ProceduralBlockSymbol>();
                    if (p.procedureKind == ProceduralBlockKind::Initial || p.procedureKind == ProceduralBlockKind::Final)
                        break;
                    Report rep;
                    Join j;
                    std::vector<Block> blocks;
                    Gather g(rep, j, blocks);
                    g.handle(p);
                    if (blocks.empty()) break;
                    const Block& b = blocks[0];
                    Logic lg;
                    lg.scope = path;
                    lg.seq = b.seq || p.procedureKind == ProceduralBlockKind::AlwaysLatch;
                    lg.clk = b.clk;
                    for (const Symbol* s : b.reads)
                        if (!b.locals.count(s)) lg.reads.push_back(s);
                    std::set<const Symbol*> targets = b.nba;
                    for (const Symbol* t : b.blocking)
                        if (!b.locals.count(t)) targets.insert(t);
                    // in the design's order, not pointer order, so runs repeat exactly
                    std::vector<const Symbol*> ts(targets.begin(), targets.end());
                    std::sort(ts.begin(), ts.end(), [](const Symbol* x, const Symbol* y) {
                        return x->location != y->location ? x->location < y->location : x->name < y->name;
                    });
                    std::sort(lg.reads.begin(), lg.reads.end(), [](const Symbol* x, const Symbol* y) {
                        return x->location != y->location ? x->location < y->location : x->name < y->name;
                    });
                    (lg.seq ? lg.regs : lg.writes) = ts;
                    Ge gv;
                    p.getBody().visit(gv);
                    lg.ge = gv.ge;
                    logic.push_back(std::move(lg));
                    break;
                }
                default: break;
            }
        }
    }

    void instance(const InstanceSymbol& inst, const std::string& p, const Scope& sc) {
        if (!inst.isModule()) return;
        int ci = libs->info(std::string(inst.getDefinition().name));
        if (ci >= 0) {
            // a library cell with a Verilog model: the model is for simulation, the power is the library's
            std::vector<std::pair<std::string, std::vector<int>>> cv;
            for (const PortConnection* c : inst.getPortConnections()) {
                if (!c || c->port.kind != SymbolKind::Port) continue;
                const Expression* x = c->getExpression();
                if (!x) continue;
                bool pure = true;
                cv.push_back({std::string(c->port.name), bitsOf(*x, sc, pure)});
            }
            addCell(p, ci, cv);
            return;
        }
        for (const PortConnection* c : inst.getPortConnections()) {
            if (!c || c->port.kind != SymbolKind::Port) continue;
            const Expression* x = c->getExpression();
            const Symbol* internal = c->port.as<PortSymbol>().internalSymbol;
            if (!x || !internal) continue;
            bool pure = true;
            auto outer = bitsOf(*x, sc, pure);
            const SymRec& r = syms[rec(*internal)];
            if (pure && (int)outer.size() == r.width) {
                for (int i = 0; i < r.width; i++) join(r.base + i, outer[(size_t)i]);
            } else if (c->port.as<PortSymbol>().direction == ArgumentDirection::In && x->kind != ExpressionKind::Assignment) {
                // an expression on an input port (.a(x & y)): logic in the parent
                Logic lg;
                lg.scope = path_of(p);
                namesIn(*x, lg.reads);
                lg.writes.push_back(internal);
                Ge g;
                x->visit(g);
                lg.ge = g.ge;
                // partly wired bits are still wires
                if ((int)outer.size() == r.width)
                    for (int i = 0; i < r.width; i++) join(r.base + i, outer[(size_t)i]);
                if (lg.ge > 0) logic.push_back(std::move(lg));
            } else if ((int)outer.size() == r.width) {
                for (int i = 0; i < r.width; i++) join(r.base + i, outer[(size_t)i]);
            }
        }
        walk(inst.body, p);
    }
    static std::string path_of(const std::string& p) {
        size_t d = p.rfind('.');
        return d == std::string::npos ? std::string() : p.substr(0, d);
    }

    // ---- after the walk ----
    int net(int raw) { return raw < 0 ? -1 : netOf[(size_t)find(raw)]; }

    void pickReference() {
        Options& o = *opt;
        auto better = [&](int cand, int cur) {
            if (cur < 0) return true;
            const liberty::Cell* a = libs->infos[cand]->c;
            const liberty::Cell* b = libs->infos[cur]->c;
            return a->area != b->area ? a->area < b->area : a->name < b->name;
        };
        if (!o.rtlFf.empty()) refFf = libs->info(o.rtlFf);
        if (!o.rtlGate.empty()) refGate = libs->info(o.rtlGate);
        if ((!o.rtlFf.empty() && refFf < 0) || (!o.rtlGate.empty() && refGate < 0))
            throw std::runtime_error("the cell named by -rtl_ff / -rtl_gate is not in the library");
        int nandLike = -1, any2 = -1, plainFf = -1, anyFf = -1;
        for (auto& l : libs->libs)
            for (auto& c : l->cells) {
                if (c.dontUse) continue;
                int nIn = 0, nOut = 0;
                for (auto& p : c.pins) (p.dir == liberty::Dir::In ? nIn : nOut)++;
                if (c.ffs.size() == 1 && nOut >= 1) {
                    int ci = libs->info(c.name);
                    if (libs->infos[ci]->clockPin < 0) continue;
                    if (better(ci, anyFf)) anyFf = ci;
                    if (c.ffs[0].clear.empty() && c.ffs[0].preset.empty() && nIn == 2 && better(ci, plainFf)) plainFf = ci;
                } else if (!c.sequential() && nIn == 2 && nOut == 1) {
                    int ci = libs->info(c.name);
                    const BoolFn* f = nullptr;
                    for (auto& p : libs->infos[ci]->pins)
                        if (p.out) f = &p.fn;
                    if (!f || !f->ok) continue;
                    if (better(ci, any2)) any2 = ci;
                    if (f->tt.size() == 4 && f->tt[0] && f->tt[1] && f->tt[2] && !f->tt[3] && better(ci, nandLike))
                        nandLike = ci;
                }
            }
        if (refFf < 0) refFf = plainFf >= 0 ? plainFf : anyFf;
        if (refGate < 0) refGate = nandLike >= 0 ? nandLike : any2;
        if (refGate >= 0) {
            double sum = 0;
            int n = 0;
            for (auto& p : libs->infos[refGate]->pins)
                if (!p.out) {
                    sum += p.p->cap;
                    n++;
                }
            refGateCap = n ? sum / n : 0;
        }
    }

    void finish() {
        // canonical nets
        netOf.assign(up.size(), -1);
        for (size_t i = 0; i < up.size(); i++) {
            int r = find((int)i);
            if (netOf[(size_t)r] < 0) {
                netOf[(size_t)r] = (int)nets.size();
                nets.emplace_back();
            }
            netOf[i] = netOf[(size_t)r];
        }
        nets[(size_t)netOf[0]].konst = nets[(size_t)netOf[1]].konst = true;
        nets[(size_t)netOf[0]].p1 = 0;
        nets[(size_t)netOf[1]].p1 = 1;
        for (Inst& in : insts)
            for (int& n : in.net) n = net(n);

        // RTL logic: flops for the registers, a cloud of reference gates for the rest
        if (!logic.empty()) {
            anyRtl = true;
            pickReference();
            if (refFf < 0 || refGate < 0)
                throw std::runtime_error("the design has RTL logic, and the library has no plain flop or 2-input gate "
                                         "to cost it with (name them with -rtl_ff and -rtl_gate)");
        }
        for (Logic& lg : logic) {
            auto bitsOfSym = [&](const Symbol* s, std::vector<int>& out) {
                if (!s || !ValueSymbol::isKind(s->kind)) return;
                const SymRec& r = syms[rec(*s)];
                if (r.width > 65536) return;
                for (int i = 0; i < r.width; i++) out.push_back(netOf[(size_t)find(r.base + i)]);
            };
            // rec() may have grown `up` for a symbol only an expression names
            auto ensure = [&](const Symbol* s) {
                if (!s || !ValueSymbol::isKind(s->kind)) return;
                rec(*s);
                while (netOf.size() < up.size()) {
                    netOf.push_back((int)nets.size());
                    nets.emplace_back();
                }
            };
            for (auto* s : lg.reads) ensure(s);
            for (auto* s : lg.writes) ensure(s);
            for (auto* s : lg.regs) ensure(s);
            ensure(lg.clk);
            Inst cl;
            cl.cloud = true;
            cl.ci = refGate;
            cl.ge = lg.ge;
            cl.name = lg.scope;
            for (auto* s : lg.reads) bitsOfSym(s, cl.io);
            std::sort(cl.io.begin(), cl.io.end());
            cl.io.erase(std::unique(cl.io.begin(), cl.io.end()), cl.io.end());
            cl.nIn = (int)cl.io.size();
            int clkNet = -1;
            if (lg.clk && ValueSymbol::isKind(lg.clk->kind)) {
                const SymRec& r = syms[rec(*lg.clk)];
                if (r.width >= 1) clkNet = netOf[(size_t)find(r.base)];
            }
            const CellI& ff = *libs->infos[refFf];
            for (auto* s : lg.regs) {
                if (!ValueSymbol::isKind(s->kind)) continue;
                const SymRec& r = syms[rec(*s)];
                bool unpacked = s->as<ValueSymbol>().getType().isUnpackedArray();
                if (unpacked && (uint64_t)r.width > opt->memBits) {
                    // a memory: not flops. Its leakage and one word's worth of clocking are counted.
                    Inst mem;
                    mem.cloud = true;
                    mem.ci = refFf;
                    mem.group = G_MEM;
                    mem.name = r.path.empty() ? std::string(s->name) : r.path;
                    mem.ge = (double)r.width;
                    if (clkNet >= 0) mem.io.push_back(clkNet);
                    mem.nIn = (int)mem.io.size();
                    insts.push_back(std::move(mem));
                    continue;
                }
                for (int i = 0; i < r.width; i++) {
                    Inst f;
                    f.ci = refFf;
                    f.virt = true;
                    f.name = (r.path.empty() ? std::string(s->name) : r.path) + "_reg" +
                             (r.width == 1 && !r.vector ? "" : "[" + std::to_string(i) + "]");
                    f.net.assign(ff.pins.size(), -1);
                    if (ff.clockPin >= 0) f.net[(size_t)ff.clockPin] = clkNet;
                    for (size_t p = 0; p < ff.pins.size(); p++)
                        if (ff.pins[p].out && ff.pins[p].stateOut != 2) {
                            f.net[p] = netOf[(size_t)find(r.base + i)];
                            break;
                        }
                    insts.push_back(std::move(f));
                }
            }
            if (!lg.seq) {
                std::vector<int> outs;
                for (auto* s : lg.writes) bitsOfSym(s, outs);
                cl.io.insert(cl.io.end(), outs.begin(), outs.end());
            }
            if (cl.ge > 0 || !cl.io.empty()) insts.push_back(std::move(cl));
        }
        logic.clear();

        // Logic nothing reads (the unused_* signals RTL keeps for lint): synthesis removes it.
        for (const Symbol* p : topPorts) {
            const SymRec& r = syms[rec(*p)];
            for (int i = 0; i < r.width && (size_t)(r.base + i) < netOf.size(); i++)
                nets[(size_t)netOf[(size_t)find(r.base + i)]].port = true;
        }
        for (int pass = 0; pass < 8; pass++) {
            std::vector<int> readers(nets.size(), 0);
            for (const Inst& in : insts) {
                if (in.cloud) {
                    for (int k = 0; k < in.nIn; k++) readers[(size_t)in.io[(size_t)k]]++;
                } else {
                    const CellI& info = *libs->infos[(size_t)in.ci];
                    for (size_t p = 0; p < in.net.size(); p++)
                        if (!info.pins[p].out && in.net[p] >= 0) readers[(size_t)in.net[p]]++;
                }
            }
            size_t before = insts.size();
            std::erase_if(insts, [&](const Inst& in) {
                if (!in.cloud || in.group == G_MEM || (int)in.io.size() <= in.nIn) return false;
                for (size_t k = (size_t)in.nIn; k < in.io.size(); k++)
                    if (readers[(size_t)in.io[k]] || nets[(size_t)in.io[k]].port) return false;
                deadGates += (size_t)in.ge;
                return true;
            });
            if (insts.size() == before) break;
        }

        // drivers, loads
        for (size_t i = 0; i < insts.size(); i++) {
            Inst& in = insts[i];
            const CellI& info = *libs->infos[(size_t)in.ci];
            if (in.cloud) {
                for (int k = 0; k < (int)in.io.size(); k++) {
                    Net& n = nets[(size_t)in.io[(size_t)k]];
                    n.used = true;
                    if (in.group == G_MEM) {
                        n.cap += info.clockPin >= 0 ? info.pins[(size_t)info.clockPin].p->cap * 8 : 0;
                        n.fan++;
                    } else if (k < in.nIn) {
                        n.cap += refGateCap;
                        n.fan++;
                    } else if (n.drv < 0) {
                        n.drv = (int)i;
                    }
                }
                continue;
            }
            for (size_t p = 0; p < in.net.size(); p++) {
                if (in.net[p] < 0) continue;
                Net& n = nets[(size_t)in.net[p]];
                n.used = true;
                if (info.pins[p].out) {
                    if (n.drv < 0 || insts[(size_t)n.drv].cloud) n.drv = (int)i;
                } else {
                    n.cap += info.pins[p].p->cap;
                    n.fan++;
                }
            }
            in.group = info.icg ? G_CLOCK : !info.c->ffs.empty() ? G_REG : info.seq ? G_SEQ : G_COMB;
        }
    }
};

struct P3 {
    double i = 0, s = 0, l = 0;
    double total() const { return i + s + l; }
    void add(const P3& o) {
        i += o.i;
        s += o.s;
        l += o.l;
    }
};

inline double exprProb(const upf::ExP& e, const std::vector<std::string>& ids, const std::vector<double>& p) {
    // enumerate the control signals; they are few
    std::function<int(const upf::ExP&, size_t)> ev = [&](const upf::ExP& x, size_t row) -> int {
        using K = upf::Ex;
        if (!x) return 1;
        switch (x->k) {
            case K::Id:
                for (size_t k = 0; k < ids.size(); k++)
                    if (ids[k] == x->s) return (row >> k) & 1;
                return 0;
            case K::Num: return x->v != 0;
            case K::Not: return !ev(x->kids[0], row);
            case K::And:
            case K::BAnd: return ev(x->kids[0], row) && ev(x->kids[1], row);
            case K::Or:
            case K::BOr: return ev(x->kids[0], row) || ev(x->kids[1], row);
            case K::BXor: return ev(x->kids[0], row) != ev(x->kids[1], row);
            case K::Eq: return ev(x->kids[0], row) == ev(x->kids[1], row);
            case K::Ne: return ev(x->kids[0], row) != ev(x->kids[1], row);
            default: return 1;
        }
    };
    if (ids.size() > 16) return 1;
    double sum = 0;
    for (size_t r = 0; r < ((size_t)1 << ids.size()); r++) {
        if (!ev(e, r)) continue;
        double w = 1;
        for (size_t k = 0; k < ids.size(); k++) w *= (r >> k) & 1 ? p[k] : 1 - p[k];
        sum += w;
    }
    return sum;
}


struct Row {
    std::string name, cell;
    P3 p;
};
struct DomainRow {
    std::string name;
    double volt = 0, on = 1;
    bool switched = false;
    P3 p;
    size_t cells = 0;
};
// Everything a report needs. Power is in watts.
struct Result {
    std::string design, library;
    double voltage = 0;
    size_t cells = 0, rtlFlops = 0, nets = 0;
    double rtlGates = 0;
    std::string rtlFf, rtlGate;
    bool anyRtl = false, genericLib = false;
    bool haveActivity = false;
    double seconds = 0, annotated = 0, clockHz = 0;   // annotated: percent of nets
    std::string activityScope, sampleActivityName, sampleNetName;
    P3 total;
    P3 groups[G_COUNT];
    size_t groupCells[G_COUNT] = {};
    std::vector<DomainRow> domains;
    std::vector<Row> instances;   // Options::hier levels deep, largest first
    std::vector<Row> topCells;    // Options::cells of them, largest first
    bool peak = false;
    double peakW = 0, peakT = 0, windowS = 0;
    std::vector<double> wave;     // watts per window
    std::map<std::string, int> blackBoxes;
    std::vector<std::string> notes;
};

// Elaborates `fwd` (a slang command line: tool name, files, --top ...) and
// computes its power. Returns 0, or the front end's exit code when the design
// does not elaborate (-1: it only printed help). Throws std::exception with a
// message for everything else (a missing file, a library without a flop).
inline int analyze(Options& o, std::vector<std::string> fwd, unsigned threads, Result& R) {
    if (threads == 0) threads = par::defaultThreads();
    for (auto& s : o.strip)
        if (s == '/') s = '.';
    auto t0 = std::chrono::steady_clock::now();
    bool timing = std::getenv("MPOWER_TIME") != nullptr;
    auto lap = [&](const char* what) {
        if (!timing) return;
        auto t1 = std::chrono::steady_clock::now();
        std::cerr << "mpower:   " << what << " " << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << " ms\n";
        t0 = t1;
    };
    auto pfor = [&](size_t n, auto&& f) {
        const size_t grain = 256;
        par::parallelFor((n + grain - 1) / grain, threads, [&](size_t c) {
            for (size_t i = c * grain, e = std::min(n, (c + 1) * grain); i < e; i++) f(i);
        });
    };
    // The activity file is read on its own thread while the design is elaborated.
    saif::Db db;
    wave::Reader wr;
    std::string actError;
    std::thread actThread;
    if (!o.activity.empty()) {
        auto job = [&] {
            try {
                saif::load(o.activity, db, threads, &wr);
            } catch (const std::exception& e) {
                actError = e.what();
            }
        };
        if (threads > 1) actThread = std::thread(job);
        else job();
    }
    struct Joiner {
        std::thread& t;
        ~Joiner() {
            if (t.joinable()) t.join();
        }
    } joiner{actThread};
    {
        Libs libs;
        if (o.libs.empty()) {
            libs.libs.push_back(std::make_unique<liberty::Library>(
                liberty::Library::load(liberty::parse(kGenericLib, "<built-in>"), "<built-in>")));
        } else {
            // each library on its own thread; the order of -lib decides which cell wins a name
            libs.libs.resize(o.libs.size());
            std::vector<std::string> errs(o.libs.size());
            size_t nl = o.libs.size();
            std::vector<std::thread> th;
            auto loadOne = [&](size_t k) {
                try {
                    libs.libs[k] = std::make_unique<liberty::Library>(liberty::load(o.libs[k]));
                } catch (const std::exception& e) {
                    errs[k] = e.what();
                }
            };
            for (size_t k = 1; k < nl && threads > 1; k++) th.emplace_back(loadOne, k);
            loadOne(0);
            if (threads <= 1)
                for (size_t k = 1; k < nl; k++) loadOne(k);
            for (auto& t : th) t.join();
            for (auto& e : errs)
                if (!e.empty()) throw std::runtime_error(e);
        }

        if (std::none_of(fwd.begin(), fwd.end(), [](const std::string& s) { return s.rfind("--std", 0) == 0; }))
            fwd.push_back("--std=1800-2023");
        if (std::none_of(fwd.begin(), fwd.end(), [](const std::string& s) { return s.rfind("--timescale", 0) == 0; }))
            fwd.push_back("--timescale=1ns/1ps");   // as msim: a file without `timescale next to one with it
        fwd.push_back("--ignore-unknown-modules");
        fwd.push_back("-Wno-unused");
        FrontEnd fe;
        if (int rc = fe.run(fwd, "mpower front end")) return rc;

        lap("libraries and front end");
        PDesign d;
        d.comp = fe.comp.get();
        d.libs = &libs;
        d.opt = &o;
        d.up = {0, 1};
        auto tops = fe.comp->getRoot().topInstances;
        if (tops.empty()) throw std::runtime_error("no top module");
        if (tops.size() > 1 && o.top.empty())
            throw std::runtime_error("the design has " + std::to_string(tops.size()) + " top modules: name one with -top");
        const InstanceSymbol& top = *tops[0];
        for (const Symbol* p : top.body.getPortList())
            if (p && p->kind == SymbolKind::Port && p->as<PortSymbol>().internalSymbol)
                d.topPorts.push_back(p->as<PortSymbol>().internalSymbol);
        d.walk(top.body, "");
        d.finish();
        auto& nets = d.nets;
        auto& insts = d.insts;

        lap("design walk");
        if (actThread.joinable()) actThread.join();
        if (!actError.empty()) throw std::runtime_error(actError);
        lap("wait for activity");
        if (!o.writeSaif.empty() && !o.activity.empty()) {
            std::ofstream f(o.writeSaif);
            if (!f) throw std::runtime_error("cannot write " + o.writeSaif);
            saif::write(db, f, std::string(top.name), "mpower", MINISIM_VERSION);
        }

        // ---- names: a name to print for each net, and nets by name for -clock and UPF
        std::vector<int> netSym(nets.size(), -1), netBit(nets.size(), 0);
        std::vector<int> order(d.syms.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
        // shallowest name first, then by name: the printed name does not depend on walk order
        auto depth = [](const std::string& s) { return std::count(s.begin(), s.end(), '.'); };
        std::vector<long> deep(d.syms.size());
        for (size_t i = 0; i < deep.size(); i++) deep[i] = depth(d.syms[i].path);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return deep[(size_t)a] != deep[(size_t)b] ? deep[(size_t)a] < deep[(size_t)b]
                                                      : d.syms[(size_t)a].path < d.syms[(size_t)b].path;
        });
        std::unordered_map<std::string, int> symByPath;
        for (int si : order) {
            const SymRec& r = d.syms[(size_t)si];
            if (r.path.empty()) continue;
            symByPath.emplace(r.path, si);
            for (int k = 0; k < r.width; k++) {
                int n = d.netOf[(size_t)d.find(r.base + k)];
                if (netSym[(size_t)n] < 0) {
                    netSym[(size_t)n] = si;
                    netBit[(size_t)n] = k;
                }
            }
        }
        auto netName = [&](int n) {
            return netSym[(size_t)n] < 0 ? std::string("?") : d.bitName(d.syms[(size_t)netSym[(size_t)n]], netBit[(size_t)n]);
        };
        // "clk", "u1/en", "bus[3]": the net, or -1
        auto findNet = [&](std::string name) {
            for (char& c : name)
                if (c == '/') c = '.';
            auto it = symByPath.find(name);
            int idx = -1;
            if (it == symByPath.end() && !name.empty() && name.back() == ']') {
                size_t br = name.rfind('[');
                if (br == std::string::npos) return -1;
                idx = std::atoi(name.c_str() + br + 1);
                it = symByPath.find(name.substr(0, br));
            }
            if (it == symByPath.end()) return -1;
            const SymRec& r = d.syms[(size_t)it->second];
            int k = 0;
            if (idx >= 0 || r.width > 1) {
                if (idx < 0) return -1;
                k = r.vector ? (r.descending ? idx - r.lower : r.lower + r.width - 1 - idx) : idx;
                if (k < 0 || k >= r.width) return -1;
            }
            return d.netOf[(size_t)d.find(r.base + k)];
        };

        // ---- annotate
        size_t usedNets = 0, annNets = 0;
        std::string prefix = o.strip.empty() ? std::string() : o.strip + ".";
        bool haveAct = !o.activity.empty();
        double seconds = db.seconds();
        if (haveAct) {
            // the simulation names the testbench top too: without -strip_path, try the top module's name
            // Without -strip_path: where in the activity file is the design? A few of the top's
            // own nets are looked for under every scope; the scope most of them are under wins.
            if (o.strip.empty() && !db.sigs.empty()) {
                std::vector<std::string> probes;
                for (int si : order) {
                    const SymRec& r = d.syms[(size_t)si];
                    if (r.path.empty() || r.path.find('.') != std::string::npos) continue;
                    probes.push_back(r.path);
                    if (probes.size() >= 16) break;
                }
                std::map<std::string, int> votes;
                for (auto& [name, sg] : db.sigs)
                    for (auto& p : probes) {
                        if (name == p) votes[""]++;
                        else if (name.size() > p.size() + 1 && name[name.size() - p.size() - 1] == '.' &&
                                 name.compare(name.size() - p.size(), p.size(), p) == 0)
                            votes[name.substr(0, name.size() - p.size())]++;
                    }
                int best = 0;
                for (auto& [pre, n] : votes)
                    if (n > best || (n == best && pre.size() < prefix.size())) {
                        best = n;
                        prefix = pre;
                    }
            }
            if (seconds <= 0) {
                // SAIF without DURATION: the longest T0 + T1 + TX
                double m = 0;
                for (auto& b : db.bits) m = std::max(m, b.t0 + b.t1 + b.tx);
                db.duration = m;
                seconds = db.seconds();
            }
            auto apply = [&](int n, int bi, bool invert) {
                Net& net = nets[(size_t)n];
                if (net.ann || net.konst) return;
                const saif::Bit& b = db.bits[(size_t)bi];
                double known = b.t0 + b.t1;
                net.p1 = known > 0 ? b.t1 / known : 0.5;
                if (invert) net.p1 = 1 - net.p1;
                net.rate = seconds > 0 ? b.tc / seconds : 0;
                net.ann = true;
                net.act = bi;
            };
            for (int si : order) {
                const SymRec& r = d.syms[(size_t)si];
                if (r.path.empty()) continue;
                // A waveform has the vector as one signal: its bits by position, no names built.
                if (auto it = db.sigs.find(prefix + r.path); it != db.sigs.end() && it->second.width == r.width) {
                    const saif::Sig& sg = it->second;
                    for (int k = 0; k < r.width; k++) {
                        int n = d.netOf[(size_t)d.find(r.base + k)];
                        if (nets[(size_t)n].ann) continue;
                        // bit k of the symbol counts from its least significant bit; the signal's
                        // bits are stored most significant first
                        apply(n, (int)sg.base + (r.width - 1 - k), false);
                    }
                    continue;
                }
                for (int k = 0; k < r.width; k++) {
                    int n = d.netOf[(size_t)d.find(r.base + k)];
                    if (nets[(size_t)n].ann) continue;
                    int bi = db.findIndex(prefix + d.bitName(r, k));
                    // a waveform that does not record ranges numbers the bits from 0
                    if (bi < 0 && r.vector) bi = db.findIndex(prefix + d.bitName(r, k, true));
                    if (bi < 0 && r.width == 1 && r.vector) bi = db.findIndex(prefix + r.path);
                    if (bi >= 0) apply(n, bi, false);
                }
            }
            // RTL activity on a gate netlist: the flop q_reg[3] holds the RTL signal q[3]
            for (Inst& in : insts) {
                if (in.cloud || in.virt) continue;
                const CellI& info = *libs.infos[(size_t)in.ci];
                if (!info.seq) continue;
                for (size_t p = 0; p < in.net.size(); p++) {
                    int so = info.pins[p].stateOut;
                    if (!so || in.net[p] < 0 || nets[(size_t)in.net[p]].ann) continue;
                    std::string nm = in.name, idx;
                    if (!nm.empty() && nm.back() == ']') {
                        size_t br = nm.rfind('[');
                        idx = nm.substr(br);
                        nm.erase(br);
                    } else if (nm.size() > 2 && nm.back() == '_') {
                        size_t us = nm.rfind('_', nm.size() - 2);
                        if (us != std::string::npos && us + 1 < nm.size() - 1 &&
                            std::all_of(nm.begin() + (long)us + 1, nm.end() - 1, [](char c) { return std::isdigit((unsigned char)c); })) {
                            idx = "[" + nm.substr(us + 1, nm.size() - us - 2) + "]";
                            nm.erase(us);
                        }
                    }
                    if (nm.size() < 5 || nm.compare(nm.size() - 4, 4, "_reg") != 0) continue;
                    nm.erase(nm.size() - 4);
                    int bi = db.findIndex(prefix + nm + idx);
                    if (bi < 0 && idx == "[0]") bi = db.findIndex(prefix + nm);
                    if (bi >= 0) apply(in.net[p], bi, so == 2);
                }
            }
        }
        lap("annotate");
        // ---- clocks: the nets on clock pins, and what drives them
        for (auto& [name, period] : o.clocks) {
            int n = findNet(name);
            if (n < 0) throw std::runtime_error("-clock: no net '" + name + "' in the design");
            if (period <= 0) throw std::runtime_error("-clock " + name + ": the period must be positive");
            Net& net = nets[(size_t)n];
            if (!net.ann) {
                net.p1 = 0.5;
                net.rate = 2.0 / (period * 1e-9);
                net.ann = true;
            }
            net.clock = true;
        }
        double fclk = 0;   // the fastest clock, cycles per second
        {
            std::vector<int> work;
            for (Inst& in : insts) {
                const CellI& info = *libs.infos[(size_t)in.ci];
                int cn = -1;
                if (in.cloud) {
                    if (in.group == G_MEM && !in.io.empty()) cn = in.io[0];
                } else if (info.seq && info.clockPin >= 0) {
                    cn = in.net[(size_t)info.clockPin];
                }
                if (cn >= 0 && !nets[(size_t)cn].clock) {
                    nets[(size_t)cn].clock = true;
                    work.push_back(cn);
                }
            }
            for (auto& n : nets)
                if (n.clock && n.ann) fclk = std::max(fclk, n.rate / 2);
            for (size_t n = 0; n < nets.size(); n++)
                if (nets[n].clock && std::find(work.begin(), work.end(), (int)n) == work.end()) work.push_back((int)n);
            // back through buffers, inverters, clock gates and the clock input of a gating cell
            while (!work.empty()) {
                int n = work.back();
                work.pop_back();
                int di = nets[(size_t)n].drv;
                if (di < 0) continue;
                Inst& drv = insts[(size_t)di];
                if (drv.cloud || drv.virt) continue;
                const CellI& info = *libs.infos[(size_t)drv.ci];
                if (info.seq && !info.icg) continue;   // a divided clock: the flop is a register
                drv.group = G_CLOCK;
                int nIn = 0;
                for (size_t p = 0; p < drv.net.size(); p++)
                    if (!info.pins[p].out && drv.net[p] >= 0) nIn++;
                for (size_t p = 0; p < drv.net.size(); p++) {
                    if (info.pins[p].out || drv.net[p] < 0) continue;
                    Net& in = nets[(size_t)drv.net[p]];
                    if (in.clock || in.konst) continue;
                    bool follow = nIn == 1 || (int)p == info.clockPin || info.pins[p].p->clock ||
                                  (in.ann && nets[(size_t)n].ann && in.rate >= 0.5 * nets[(size_t)n].rate && in.rate > 0);
                    if (!follow) continue;
                    in.clock = true;
                    work.push_back(drv.net[p]);
                }
            }
        }

        // ---- activity of the nets the file does not name
        for (auto& n : nets) {
            n.known = n.ann || n.konst;
            if (n.used) usedNets++;
            if (n.used && (n.ann || n.konst)) annNets++;
        }
        double inRate = o.toggle * fclk;
        for (auto& n : nets)
            if (!n.known && n.drv < 0) {
                n.p1 = 0.5;
                n.rate = n.clock ? 0 : inRate;
                n.known = true;
            }
        {
            // sinks of each net, to release an instance when its inputs are known
            std::vector<int> head(nets.size() + 1, 0), sinks;
            auto eachIn = [&](const Inst& in, auto f) {
                if (in.cloud) {
                    for (int k = 0; k < in.nIn; k++) f(in.io[(size_t)k]);
                } else {
                    const CellI& info = *libs.infos[(size_t)in.ci];
                    for (size_t p = 0; p < in.net.size(); p++)
                        if (!info.pins[p].out && in.net[p] >= 0) f(in.net[p]);
                }
            };
            for (auto& in : insts) eachIn(in, [&](int n) { head[(size_t)n + 1]++; });
            for (size_t i = 0; i < nets.size(); i++) head[i + 1] += head[i];
            sinks.resize((size_t)head[nets.size()]);
            {
                std::vector<int> fill(head.begin(), head.end() - 1);
                for (size_t i = 0; i < insts.size(); i++) eachIn(insts[i], [&](int n) { sinks[(size_t)fill[(size_t)n]++] = (int)i; });
            }
            auto evalInst = [&](Inst& in, bool seqPass) {
                const CellI& info = *libs.infos[(size_t)in.ci];
                if (in.cloud) {
                    double sum = 0;
                    int cnt = 0;
                    for (int k = 0; k < in.nIn; k++) {
                        const Net& n = nets[(size_t)in.io[(size_t)k]];
                        if (n.clock || n.konst) continue;
                        sum += n.rate;
                        cnt++;
                    }
                    for (size_t k = (size_t)in.nIn; k < in.io.size(); k++) {
                        Net& n = nets[(size_t)in.io[k]];
                        if (n.ann || n.konst || n.drv < 0 || &insts[(size_t)n.drv] != &in) continue;
                        n.p1 = 0.5;
                        n.rate = cnt ? sum / cnt : 0;
                    }
                    return;
                }
                double p[64], dd[64];
                size_t np = std::min<size_t>(in.net.size(), 64);
                for (size_t k = 0; k < np; k++) {
                    p[k] = in.net[k] >= 0 ? nets[(size_t)in.net[k]].p1 : 0.0;
                    dd[k] = in.net[k] >= 0 ? nets[(size_t)in.net[k]].rate : 0.0;
                }
                for (size_t k = 0; k < np; k++) {
                    if (!info.pins[k].out || in.net[k] < 0) continue;
                    Net& n = nets[(size_t)in.net[k]];
                    if (n.ann || n.konst) continue;
                    const PinI& pi = info.pins[k];
                    if (pi.fn.ok && !info.seq) {
                        bool wide = false;
                        for (int v : pi.fn.pins) wide |= v >= 64;
                        if (wide) continue;
                        n.p1 = pi.fn.prob(p);
                        n.rate = pi.fn.density(p, dd);
                    } else if (info.icg && info.clockPin >= 0) {
                        // a clock gate passes the clock while its enable is high
                        double en = 1;
                        for (size_t q = 0; q < np; q++)
                            if (!info.pins[q].out && (int)q != info.clockPin && in.net[q] >= 0 &&
                                info.pins[q].p->signalType.empty())
                                en = std::min(en, p[q]);
                        n.p1 = p[info.clockPin] * en;
                        n.rate = dd[info.clockPin] * en;
                    } else if (pi.stateOut && seqPass) {
                        double f = info.clockPin >= 0 ? dd[info.clockPin] / 2 : fclk;
                        double q = 0.5, r = o.toggle * f;
                        bool wide = false;
                        for (int v : info.nextState.pins) wide |= v >= 64;
                        if (info.nextState.ok && !wide) {
                            q = info.nextState.prob(p);
                            // a flop cannot toggle more often than its input does, nor than once a cycle
                            r = std::min(2 * q * (1 - q) * f, info.nextState.density(p, dd));
                        }
                        n.p1 = pi.stateOut == 2 ? 1 - q : q;
                        n.rate = r;
                    }
                }
            };
            auto isSeq = [&](const Inst& in) { return !in.cloud && libs.infos[(size_t)in.ci]->seq && !libs.infos[(size_t)in.ci]->icg; };
            // flop outputs first, with the default rate; then two rounds: logic from flops, flops from logic
            for (auto& in : insts) {
                if (!isSeq(in)) continue;
                const CellI& info = *libs.infos[(size_t)in.ci];
                double f = info.clockPin >= 0 && in.net[(size_t)info.clockPin] >= 0 && nets[(size_t)in.net[(size_t)info.clockPin]].known
                               ? nets[(size_t)in.net[(size_t)info.clockPin]].rate / 2 : fclk;
                for (size_t k = 0; k < in.net.size(); k++) {
                    if (!info.pins[k].out || in.net[k] < 0) continue;
                    Net& n = nets[(size_t)in.net[k]];
                    if (n.known) continue;
                    n.p1 = 0.5;
                    n.rate = o.toggle * f;
                    n.known = true;
                }
            }
            std::vector<char> base(nets.size());
            for (size_t i = 0; i < nets.size(); i++) base[i] = nets[i].known;
            for (int round = 0; round < 3; round++) {
                for (size_t i = 0; i < nets.size(); i++) nets[i].known = base[i];
                std::vector<int> pending(insts.size(), 0), ready;
                for (size_t i = 0; i < insts.size(); i++) {
                    if (isSeq(insts[i])) continue;
                    eachIn(insts[i], [&](int n) { pending[i] += !nets[(size_t)n].known; });
                    if (pending[i] == 0) ready.push_back((int)i);
                }
                size_t qh = 0;
                while (qh < ready.size()) {
                    Inst& in = insts[(size_t)ready[qh++]];
                    evalInst(in, false);
                    auto release = [&](int nn) {
                        Net& n = nets[(size_t)nn];
                        if (n.known) return;
                        n.known = true;
                        for (int s = head[(size_t)nn]; s < head[(size_t)nn + 1]; s++) {
                            int si = sinks[(size_t)s];
                            if (isSeq(insts[(size_t)si])) continue;
                            if (--pending[(size_t)si] == 0) ready.push_back(si);
                        }
                    };
                    if (in.cloud) {
                        for (size_t k = (size_t)in.nIn; k < in.io.size(); k++) release(in.io[k]);
                    } else {
                        const CellI& info = *libs.infos[(size_t)in.ci];
                        for (size_t k = 0; k < in.net.size(); k++)
                            if (info.pins[k].out && in.net[k] >= 0) release(in.net[k]);
                    }
                }
                if (round == 2) break;
                for (auto& in : insts)
                    if (isSeq(in)) evalInst(in, true);
            }
            for (auto& n : nets) n.known = true;
        }

        lap("clocks and propagation");
        // ---- UPF: the domain of each instance, its voltage, and how long it is on
        struct Dom {
            std::string name;
            double volt = 0, on = 1;
            bool switched = false;
            P3 p;
            size_t cells = 0;
        };
        std::vector<Dom> doms;
        if (!o.upfPath.empty()) {
            upf::Model m;
            m.load(o.upfPath, o.powerTop);
            upf::DomainMap dm;
            dm.build(m);
            upf::SupplyGraph g;
            g.build(m);
            auto si = upf::supplyInfo(m, g);
            std::map<std::string, int> domIdx;
            for (auto& dn : m.domainOrder) {
                Dom x;
                x.name = dn;
                auto ss = m.sets.find(m.primaryOf(dn));
                std::string pnet = ss != m.sets.end() && ss->second.fn.count("power") ? ss->second.fn.at("power") : std::string();
                std::string root = pnet.empty() ? std::string() : g.find(pnet);
                if (auto it = si.find(root); it != si.end() && !root.empty()) {
                    for (double v : it->second.volts) x.volt = std::max(x.volt, v);
                    x.switched = it->second.switchable;
                }
                for (auto& [sn, sw] : m.switches) {
                    if (root.empty() || sw.outNet.empty() || g.find(sw.outNet) != root) continue;
                    x.switched = true;
                    double on = 0;
                    for (auto& st : sw.on) {
                        std::vector<std::string> ids;
                        upf::exprIds(st.e, ids);
                        std::sort(ids.begin(), ids.end());
                        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                        std::vector<double> pr;
                        for (auto& id : ids) {
                            std::string netn = id;
                            for (auto& [port, cn] : sw.ctrls)
                                if (port == id) netn = cn;
                            int n = findNet(m.design(netn));
                            if (n < 0) {
                                d.notes.push_back("UPF: the control net '" + netn + "' of switch " + sn +
                                                  " is not in the design; domain " + dn + " is taken as always on");
                                pr.push_back(1);
                            } else {
                                pr.push_back(nets[(size_t)n].p1);
                            }
                        }
                        on += exprProb(st.e, ids, pr);
                    }
                    x.on = sw.on.empty() ? 1 : std::min(1.0, on);
                }
                domIdx[dn] = (int)doms.size();
                doms.push_back(x);
            }
            for (Inst& in : insts) {
                std::string dn = dm.of(in.name);
                auto it = domIdx.find(dn);
                in.domain = it == domIdx.end() ? -1 : it->second;
            }
        }

        // ---- loads, transition times
        const liberty::Library& lib0 = *libs.libs[0];
        const liberty::WireLoad* wl = lib0.defaultWireLoad.empty() ? nullptr : lib0.wireLoad(lib0.defaultWireLoad);
        for (auto& n : nets) {
            if (o.wireCap >= 0) n.wire = o.wireCap * 1e-3 * n.fan;
            else if (wl) n.wire = wl->capacitance * wl->length(n.fan) * lib0.capScale;
            n.slew = o.inSlew;
        }
        // Extracted wires (SPEF): a net's total capacitance replaces the estimate.
        if (!o.spef.empty()) {
            spef::File sf;
            try {
                sf = spef::read(o.spef, (int)threads);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "mpower: %s\n", e.what());
                return 1;
            }
            size_t hit = 0;
            for (const spef::Net& sn : sf.nets) {
                std::string nm = sn.name;
                if (sf.h.divider != '.')
                    for (char& c : nm)
                        if (c == sf.h.divider) c = '.';
                if (sf.h.busOpen != '[' || sf.h.busClose != ']')
                    for (char& c : nm) {
                        if (c == sf.h.busOpen) c = '[';
                        else if (c == sf.h.busClose) c = ']';
                    }
                int n = findNet(nm);
                if (n < 0) continue;
                double c = sn.total;
                if (c <= 0)
                    for (const spef::Cap& k : sn.caps) c += k.c;
                nets[(size_t)n].wire = c * sf.cScale * 1e12;
                hit++;
            }
            d.notes.push_back("SPEF: " + std::to_string(hit) + " of " + std::to_string(sf.nets.size()) +
                              " nets in " + o.spef + " matched the design");
            if (!hit && !sf.nets.empty()) {
                std::fprintf(stderr, "mpower: no net of %s is in the design\n", o.spef.c_str());
                return 1;
            }
        }
        for (int pass = 0; pass < 3; pass++) {
            std::vector<std::vector<std::pair<int, double>>> per(insts.size());
            pfor(insts.size(), [&](size_t i) {
                const Inst& in = insts[i];
                if (in.cloud) return;
                const CellI& info = *libs.infos[(size_t)in.ci];
                for (size_t p = 0; p < in.net.size(); p++) {
                    if (!info.pins[p].out || in.net[p] < 0 || info.pins[p].trans.empty()) continue;
                    const Net& n = nets[(size_t)in.net[p]];
                    double load = n.cap + n.wire, s = 0;
                    for (auto& [rel, arc] : info.pins[p].trans) {
                        double is = rel >= 0 && in.net[(size_t)rel] >= 0 ? nets[(size_t)in.net[(size_t)rel]].slew : o.inSlew;
                        double r = arc->riseTrans.empty() ? 0 : arc->riseTrans.lookup(is, load);
                        double f = arc->fallTrans.empty() ? r : arc->fallTrans.lookup(is, load);
                        if (arc->riseTrans.empty()) r = f;
                        s = std::max(s, 0.5 * (r + f));
                    }
                    if (s > 0) per[i].push_back({in.net[p], s});
                }
            });
            for (auto& v : per)
                for (auto& [n, s] : v) nets[(size_t)n].slew = s;
        }

        // ---- power, one instance at a time
        pfor(insts.size(), [&](size_t i) {
            Inst& in = insts[i];
            const CellI& info = *libs.infos[(size_t)in.ci];
            double v = o.voltage > 0 ? o.voltage : info.vnom;
            double on = 1;
            if (in.domain >= 0) {
                if (doms[(size_t)in.domain].volt > 0 && o.voltage <= 0) v = doms[(size_t)in.domain].volt;
                on = doms[(size_t)in.domain].on;
            }
            double vs = (v / info.vnom) * (v / info.vnom);   // dynamic energy scales with V^2
            double ls = v / info.vnom;                       // leakage, to first order, with V
            if (in.cloud) {
                if (in.group == G_MEM) {
                    // leakage of the bits (a bit cell leaks less than a flop), clocking of about one word
                    in.pleak = in.ge * 0.25 * info.c->leakage * info.leakUnit * ls * on;
                    if (!in.io.empty() && info.clockPin >= 0) {
                        const PinI& ck = info.pins[(size_t)info.clockPin];
                        double e = 0;
                        int k = 0;
                        for (auto& a : ck.arcs) {
                            e += 0.5 * (a.a->risePower.lookup(o.inSlew, 0) + a.a->fallPower.lookup(o.inSlew, 0));
                            k++;
                        }
                        double word = std::sqrt(in.ge);
                        double ePer = (k ? e / k : 0) * info.eUnit * vs * word;
                        in.pint = ePer * nets[(size_t)in.io[0]].rate;
                        in.pinE.assign(1, ePer);
                    }
                    return;
                }
                // reference gates: internal energy and one gate load per internal node, at the block's mean toggle rate
                double sum = 0;
                int cnt = 0;
                for (int nn : in.io) {
                    const Net& n = nets[(size_t)nn];
                    if (n.clock || n.konst) continue;
                    sum += n.rate;
                    cnt++;
                }
                double rate = cnt ? sum / cnt : 0;
                double eInt = 0;
                int k = 0;
                for (auto& pi : info.pins)
                    for (auto& a : pi.arcs) {
                        eInt += 0.5 * (a.a->risePower.lookup(o.inSlew, d.refGateCap) + a.a->fallPower.lookup(o.inSlew, d.refGateCap));
                        k++;
                    }
                eInt = (k ? eInt / k : 0) * info.eUnit * vs;
                double outs = (double)in.io.size() - in.nIn;
                double inner = std::max(0.0, in.ge - outs);
                double eSw = 0.5 * d.refGateCap * 1e-12 * v * v;
                in.pint = in.ge * eInt * rate;
                in.psw = inner * eSw * rate;
                in.pleak = in.ge * info.c->leakage * info.leakUnit * ls * on;
                in.pinE.assign(1, cnt ? (in.ge * eInt + inner * eSw) / cnt : 0);
                // the nets it drives
                for (size_t q = (size_t)in.nIn; q < in.io.size(); q++) {
                    const Net& n = nets[(size_t)in.io[q]];
                    if (n.drv != (int)i || n.konst) continue;
                    double x = 0.5 * (n.cap + n.wire) * 1e-12 * v * v * n.rate;
                    in.psw += x;
                    if (n.clock) in.pswClock += x;
                }
                return;
            }
            size_t np = in.net.size();
            std::vector<double> p(np), r(np), sl(np);
            for (size_t k = 0; k < np; k++) {
                const Net* n = in.net[k] >= 0 ? &nets[(size_t)in.net[k]] : nullptr;
                p[k] = n ? n->p1 : 0;
                r[k] = n ? n->rate : 0;
                sl[k] = n ? n->slew : o.inSlew;
            }
            in.pinE.assign(np, 0);
            for (size_t k = 0; k < np; k++) {
                const PinI& pi = info.pins[k];
                if (in.net[k] < 0) continue;
                const Net& n = nets[(size_t)in.net[k]];
                if (n.konst) continue;
                double load = pi.out ? n.cap + n.wire : 0;
                double eSw = 0;
                if (pi.out && n.drv == (int)i) {
                    eSw = 0.5 * load * 1e-12 * v * v;
                    in.psw += eSw * r[k];
                    if (n.clock) in.pswClock += eSw * r[k];
                }
                if (pi.arcs.empty()) {
                    in.pinE[k] = eSw;
                    continue;
                }
                // energy of a rise and of a fall: the arcs weighted by how often each one is the cause
                double wsum = 0, er = 0, ef = 0, plain = 0, plainR = 0, plainF = 0;
                for (auto& a : pi.arcs) {
                    double is = a.related >= 0 ? sl[(size_t)a.related] : sl[k];
                    double rise = a.a->risePower.lookup(is, load), fall = a.a->fallPower.lookup(is, load);
                    plain += 1;
                    plainR += rise;
                    plainF += fall;
                    double w = a.hasWhen ? a.when.prob(p.data()) : 1.0;
                    if (pi.out && a.related >= 0) w *= r[(size_t)a.related];
                    wsum += w;
                    er += w * rise;
                    ef += w * fall;
                }
                if (wsum > 0) {
                    er /= wsum;
                    ef /= wsum;
                } else {
                    er = plainR / plain;
                    ef = plainF / plain;
                }
                double e = 0.5 * (er + ef) * info.eUnit * vs;
                // An input that toggles while the cell's domain is switched off (its clock keeps
                // running) costs the cell nothing; an output only toggles while it is on.
                if (!pi.out) e *= on;
                in.pint += e * r[k];
                in.pinE[k] = e + eSw;
            }
            double pw = 0, lk = 0;
            for (auto& [when, val] : info.leak) {
                double q = when.prob(p.data());
                pw += q;
                lk += q * val;
            }
            if (pw > 1) {
                lk /= pw;
                pw = 1;
            }
            if (info.leak.empty() || info.c->leakage > 0) lk += (1 - pw) * info.c->leakage;
            else if (pw > 0) lk /= pw;   // no default value: the states listed are all there is
            in.pleak = lk * info.leakUnit * ls * on;
        });

        // ---- sums, in instance order
        P3 groups[G_COUNT];
        P3 total;
        size_t groupCells[G_COUNT] = {};
        double geTotal = 0;
        size_t nCells = 0, nVirt = 0;
        for (size_t i = 0; i < insts.size(); i++) {
            Inst& in = insts[i];
            P3 x{in.pint, in.psw, in.pleak};
            // a clock net is clock network power whatever drives it (a gate still in RTL, say)
            groups[in.group].add({in.pint, in.psw - in.pswClock, in.pleak});
            groups[G_CLOCK].s += in.pswClock;
            groupCells[in.group] += in.cloud ? 0 : 1;
            total.add(x);
            if (in.domain >= 0) {
                doms[(size_t)in.domain].p.add(x);
                doms[(size_t)in.domain].cells++;
            }
            if (in.cloud && in.group != G_MEM) geTotal += in.ge;
            else if (in.virt) nVirt++;
            else if (!in.cloud) nCells++;
            // per-net energy for -peak
            if (in.cloud) {
                if (in.pinE.empty()) continue;
                if (in.group == G_MEM) {
                    nets[(size_t)in.io[0]].energy += in.pinE[0];
                    continue;
                }
                const CellI& info = *libs.infos[(size_t)in.ci];
                double v = o.voltage > 0 ? o.voltage : in.domain >= 0 && doms[(size_t)in.domain].volt > 0 ? doms[(size_t)in.domain].volt : info.vnom;
                for (size_t q = 0; q < in.io.size(); q++) {
                    Net& n = nets[(size_t)in.io[q]];
                    if (n.clock || n.konst) continue;
                    n.energy += in.pinE[0];
                    if ((int)q >= in.nIn && n.drv == (int)i) n.energy += 0.5 * (n.cap + n.wire) * 1e-12 * v * v;
                }
            } else {
                for (size_t k = 0; k < in.net.size(); k++)
                    if (in.net[k] >= 0) nets[(size_t)in.net[k]].energy += in.pinE[k];
            }
        }
        // nets a primary input drives: their switching belongs to no cell
        {
            double v = o.voltage > 0 ? o.voltage : libs.infos.empty() ? 1.0 : libs.infos[0]->vnom;
            for (auto& n : nets) {
                if (n.drv >= 0 || !n.used || n.konst) continue;
                double e = 0.5 * (n.cap + n.wire) * 1e-12 * v * v;
                n.energy += e;
                P3 x{0, e * n.rate, 0};
                groups[n.clock ? G_CLOCK : G_IN].add(x);
                total.add(x);
            }
        }

        lap("power");
        // ---- peak power: the energy of each toggle into the window it falls in
        if (o.peak) {
            if (!haveAct || wr.waves.empty())
                throw std::runtime_error("-peak needs a waveform (.vcd or .fst): SAIF has no times");
            double interval = o.interval > 0 ? o.interval * 1e-9 : fclk > 0 ? 1.0 / fclk : seconds / 100;
            if (interval <= 0 || seconds <= 0) throw std::runtime_error("-peak: the waveform has no duration");
            if (seconds / interval > 1e6) {
                interval = seconds / 1e6;
                d.notes.push_back("-peak: the window was widened to " + std::to_string(interval * 1e9) +
                                  " ns to keep it to a million windows");
            }
            size_t nb = std::max<size_t>(1, (size_t)std::ceil(seconds / interval - 1e-9));
            std::vector<double> ebit(db.bits.size(), 0);
            double annotated = 0;
            for (auto& n : nets)
                if (n.act >= 0 && (size_t)n.act < ebit.size()) {
                    ebit[(size_t)n.act] += n.energy;
                    annotated += n.energy * n.rate;
                }
            std::vector<uint32_t> wbase(wr.waves.size(), UINT32_MAX);
            for (auto& [name, sg] : db.sigs)
                if (sg.wave >= 0) wbase[(size_t)sg.wave] = sg.base;
            // Signals in fixed groups, each with its own windows, added up in group order: the
            // sum does not depend on which thread took which group.
            size_t nw = wr.waves.size(), grain = std::max<size_t>(1, (nw + 63) / 64), ng = (nw + grain - 1) / grain;
            std::vector<std::vector<double>> part(ng);
            double ticks = interval / db.tick;
            par::parallelFor(ng, threads, [&](size_t g) {
                for (size_t w = g * grain, e = std::min(nw, (g + 1) * grain); w < e; w++) {
                    if (wbase[w] == UINT32_MAX) continue;
                    const double* eb = ebit.data() + wbase[w];
                    bool any = false;
                    for (int j = 0; j < wr.waves[w].width; j++) any |= eb[j] != 0;
                    if (!any) continue;
                    if (part[g].empty()) part[g].assign(nb, 0);
                    double* bins = part[g].data();
                    saif::scan(wr.waves[w], db.start, wr.end, nullptr, [&](int j, uint64_t t, bool) {
                        // a toggle at the very end of the run is in no window
                        if (eb[j] == 0 || t >= wr.end) return;
                        size_t k = (size_t)((double)(t - db.start) / ticks);
                        bins[k < nb ? k : nb - 1] += eb[j];
                    });
                }
            });
            std::vector<double> bins(nb, 0);
            for (auto& p : part)
                for (size_t k = 0; k < p.size(); k++) bins[k] += p[k];
            // what propagation estimated has no times: it is spread evenly
            double dyn = total.i + total.s;
            double baseline = std::max(0.0, dyn - annotated) + total.l;
            R.wave.resize(nb);
            for (size_t k = 0; k < nb; k++) {
                double w = k + 1 == nb ? seconds - interval * (double)(nb - 1) : interval;
                if (w <= 0) w = interval;
                R.wave[k] = bins[k] / w + baseline;
                if (R.wave[k] > R.peakW) {
                    R.peakW = R.wave[k];
                    R.peakT = (double)k * interval;
                }
            }
            R.windowS = interval;
            R.peak = true;
        }

        lap("peak");
        // ---- the result
        R.design = std::string(top.name);
        for (auto& l : libs.libs) R.library += (R.library.empty() ? "" : ", ") + l->name;
        R.voltage = o.voltage > 0 ? o.voltage : libs.infos.empty() ? 0 : libs.infos[0]->vnom;
        R.cells = nCells;
        R.rtlFlops = nVirt;
        R.rtlGates = geTotal;
        R.nets = usedNets;
        R.anyRtl = d.anyRtl;
        R.genericLib = o.libs.empty();
        if (d.anyRtl) {
            R.rtlFf = libs.infos[(size_t)d.refFf]->c->name;
            R.rtlGate = libs.infos[(size_t)d.refGate]->c->name;
        }
        R.haveActivity = haveAct;
        R.seconds = seconds;
        R.annotated = usedNets ? 100.0 * (double)annNets / (double)usedNets : 0;
        R.clockHz = fclk;
        R.activityScope = prefix.empty() ? std::string() : prefix.substr(0, prefix.size() - 1);
        if (!db.sigs.empty()) {
            // the smallest name, so the hint is the same on every run
            const std::string* m = nullptr;
            for (auto& [name, sg] : db.sigs)
                if (!m || name < *m) m = &name;
            R.sampleActivityName = *m;
        }
        for (size_t n = 0; n < nets.size() && R.sampleNetName.empty(); n++)
            if (nets[n].used && !nets[n].konst && netSym[n] >= 0) R.sampleNetName = netName((int)n);
        R.total = total;
        for (int g = 0; g < G_COUNT; g++) {
            R.groups[g] = groups[g];
            R.groupCells[g] = groupCells[g];
        }
        for (auto& x : doms) R.domains.push_back({x.name, x.volt, x.on, x.switched, x.p, x.cells});
        if (o.hier > 0) {
            std::map<std::string, P3> hier;
            for (auto& in : insts) {
                std::string scope = in.cloud && in.group != G_MEM ? in.name : PDesign::path_of(in.name);
                std::string key;
                int level = 0;
                size_t from = 0;
                while (level < o.hier && from < scope.size()) {
                    size_t dot = scope.find('.', from);
                    key = scope.substr(0, dot);
                    level++;
                    if (dot == std::string::npos) break;
                    from = dot + 1;
                }
                hier[key.empty() ? "(top)" : key].add({in.pint, in.psw, in.pleak});
            }
            for (auto& [k, p] : hier) R.instances.push_back({k, "", p});
            std::stable_sort(R.instances.begin(), R.instances.end(),
                             [](const Row& a, const Row& b) { return a.p.total() > b.p.total(); });
        }
        if (o.cells > 0) {
            std::vector<size_t> idx;
            for (size_t i = 0; i < insts.size(); i++)
                if (!insts[i].cloud) idx.push_back(i);
            auto tot = [&](size_t i) { return insts[i].pint + insts[i].psw + insts[i].pleak; };
            std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return tot(a) > tot(b); });
            if (idx.size() > (size_t)o.cells) idx.resize((size_t)o.cells);
            for (size_t i : idx)
                R.topCells.push_back({insts[i].name, libs.infos[(size_t)insts[i].ci]->c->name,
                                      {insts[i].pint, insts[i].psw, insts[i].pleak}});
        }
        R.blackBoxes = d.blackBoxes;
        R.notes = d.notes;
    }
    return 0;
}

}  // namespace pwr
