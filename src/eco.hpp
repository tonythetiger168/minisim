// eco.hpp - functional ECO on a gate netlist, the engine of meco.
//
// Given the netlist that is already implemented (old) and a netlist of the
// revised design (new: the revised RTL after synthesis), it finds the compare
// points whose function changed and writes the smallest set of edits to the
// old netlist that makes it equivalent to the new one:
//
//   1. Both netlists are read with gnetlist.hpp and the cells with
//      liberty.hpp, and turned into one and-inverter graph (equiv/aig.hpp)
//      over shared variables: primary inputs by name, a state variable per
//      flip-flop or latch instance by name, and one variable per output pin
//      of a cell without a logic model (a black box: a macro, a multi-bit
//      register).
//   2. Compare points are matched by name: primary outputs; the next-state,
//      clock, clear and preset functions of each flip-flop and latch (from
//      the Liberty ff/latch group, so a scan flop and a plain flop compare
//      equal once scan enable is tied off); every input pin of a black box.
//   3. The failing points are found by simulation and SAT (equiv/sat.hpp).
//   4. For each failing point the patch is the cone of new-netlist gates
//      between the point and the nearest nets that already exist in the old
//      netlist with the same function (found by simulation signatures, then
//      proved by SAT). Those gates are copied, the point is rewired to the
//      copy, and old gates that no longer drive anything are removed.
//   5. The patched module is turned into logic again from scratch and every
//      compare point is proved against the new netlist.
//
// Results do not depend on the thread count: the parallel steps are pure
// functions of their index (one fresh SAT instance per query, a fixed
// conflict budget), and every decision is made in index order.
#pragma once
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "equiv/aig.hpp"
#include "equiv/cnf.hpp"
#include "gnetlist.hpp"
#include "liberty.hpp"
#include "parallel.hpp"

namespace eco {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The logic engine is the equivalence checker's. These are the only names
// taken from it besides Cnf/Sat in Prover::prove, so a move of the AIG to a
// shared header changes this block and nothing else.
using Aig = equiv::Aig;
using Lit = equiv::Lit;
inline constexpr Lit kFalse = equiv::kFalse, kTrue = equiv::kTrue;

// ------------------------------------------------------------------ library

enum class Kind { Comb, State, Box };

// A pin function compiled to postfix: >= 0 pushes that pin, the rest are below.
struct Prog {
    enum { Not = -1, And = -2, Or = -3, Xor = -4, C0 = -5, C1 = -6, Iq = -7, Iqn = -8 };
    std::vector<int> code;
    bool empty() const { return code.empty(); }
    bool uses(int pin) const {
        for (int c : code)
            if (c == pin) return true;
        return false;
    }
};

struct CellInfo {
    const liberty::Cell* cell = nullptr;
    std::string name;
    Kind kind = Kind::Box;
    std::vector<std::string> pins;  // one per bit
    std::vector<uint8_t> isOut;
    std::unordered_map<std::string, int> pinIdx;
    std::vector<std::pair<std::string, std::vector<int>>> ports;  // a bus: its bits msb first
    std::vector<Prog> fn;  // per pin, for the outputs of Comb and State cells
    bool latch = false;
    Prog next, clock, clear, preset;  // State; clear and preset empty when absent
    double area = 0;
    int invIn = -1, invOut = -1;  // set when the cell is an inverter

    const std::vector<int>* port(const std::string& n) const {
        for (auto& p : ports)
            if (p.first == n) return &p.second;
        return nullptr;
    }
};

struct Lib {
    std::vector<std::unique_ptr<liberty::Library>> libs;
    std::unordered_map<std::string, CellInfo> cells;
    const CellInfo* inverter = nullptr;

    const CellInfo* find(const std::string& n) const {
        auto it = cells.find(n);
        return it == cells.end() ? nullptr : &it->second;
    }

    static void compile(const liberty::Expr& e, const CellInfo& ci, const std::string& iq, const std::string& iqn,
                        Prog& p) {
        using E = liberty::Expr;
        switch (e.kind) {
            case E::Const: p.code.push_back(e.value ? Prog::C1 : Prog::C0); return;
            case E::Var: {
                if (!iq.empty() && e.var == iq) { p.code.push_back(Prog::Iq); return; }
                if (!iqn.empty() && e.var == iqn) { p.code.push_back(Prog::Iqn); return; }
                auto it = ci.pinIdx.find(e.var);
                if (it == ci.pinIdx.end())
                    throw Error("cell " + ci.name + ": a function names '" + e.var + "', which is not a pin");
                p.code.push_back(it->second);
                return;
            }
            case E::Not: compile(e.kids[0], ci, iq, iqn, p); p.code.push_back(Prog::Not); return;
            case E::And:
            case E::Or:
            case E::Xor:
                compile(e.kids[0], ci, iq, iqn, p);
                compile(e.kids[1], ci, iq, iqn, p);
                p.code.push_back(e.kind == E::And ? Prog::And : e.kind == E::Or ? Prog::Or : Prog::Xor);
                return;
        }
    }

    void add(const std::string& path) {
        libs.push_back(std::make_unique<liberty::Library>(liberty::Library::read(path)));
        for (auto& c : libs.back()->cells) {
            if (cells.count(c.name)) continue;  // the first library wins
            CellInfo ci;
            ci.cell = &c;
            ci.name = c.name;
            ci.area = c.area;
            bool modeled = true;
            for (auto& p : c.pins) {
                if (p.dir == liberty::Dir::Internal) continue;
                int idx = (int)ci.pins.size();
                ci.pinIdx[p.name] = idx;
                ci.pins.push_back(p.name);
                bool out = p.dir != liberty::Dir::In;
                ci.isOut.push_back(out);
                if (p.dir == liberty::Dir::InOut || !p.threeState.empty() || !p.bus.empty()) modeled = false;
                if (out && !p.hasFunction()) modeled = false;
                std::string port = p.bus.empty() ? p.name : p.bus;
                if (ci.ports.empty() || ci.ports.back().first != port) ci.ports.push_back({port, {}});
                ci.ports.back().second.push_back(idx);
            }
            ci.fn.resize(ci.pins.size());
            if (c.group && c.group->first("statetable")) modeled = false;
            size_t seq = c.ffs.size() + c.latches.size();
            if (seq > 1) modeled = false;
            std::string iq, iqn;
            try {
                if (modeled && c.ffs.size() == 1) {
                    auto& f = c.ffs[0];
                    if (f.nextState.empty() || f.clockedOn.empty() || !f.clockedOnAlso.empty()) modeled = false;
                    else {
                        iq = f.iq, iqn = f.iqn;
                        compile(liberty::parseFunction(f.nextState), ci, iq, iqn, ci.next);
                        compile(liberty::parseFunction(f.clockedOn), ci, iq, iqn, ci.clock);
                        if (!f.clear.empty()) compile(liberty::parseFunction(f.clear), ci, iq, iqn, ci.clear);
                        if (!f.preset.empty()) compile(liberty::parseFunction(f.preset), ci, iq, iqn, ci.preset);
                    }
                } else if (modeled && c.latches.size() == 1) {
                    auto& l = c.latches[0];
                    if (l.dataIn.empty() || l.enable.empty()) modeled = false;
                    else {
                        ci.latch = true;
                        iq = l.iq, iqn = l.iqn;
                        compile(liberty::parseFunction(l.dataIn), ci, iq, iqn, ci.next);
                        compile(liberty::parseFunction(l.enable), ci, iq, iqn, ci.clock);
                        if (!l.clear.empty()) compile(liberty::parseFunction(l.clear), ci, iq, iqn, ci.clear);
                        if (!l.preset.empty()) compile(liberty::parseFunction(l.preset), ci, iq, iqn, ci.preset);
                    }
                }
                if (modeled)
                    for (auto& p : c.pins) {
                        if (p.dir != liberty::Dir::Out) continue;
                        compile(p.functionExpr(), ci, iq, iqn, ci.fn[(size_t)ci.pinIdx[p.name]]);
                    }
            } catch (liberty::Error&) {
                modeled = false;
            }
            ci.kind = !modeled ? Kind::Box : seq ? Kind::State : Kind::Comb;
            if (ci.kind == Kind::Comb && ci.pins.size() == 2 && ci.isOut[0] != ci.isOut[1]) {
                int o = ci.isOut[0] ? 0 : 1, i = 1 - o;
                if (ci.fn[(size_t)o].code == std::vector<int>{i, Prog::Not}) ci.invIn = i, ci.invOut = o;
            }
            cells.emplace(c.name, std::move(ci));
        }
        // The inverter used when a patch needs the complement of a register
        // output: the smallest one that may be used, by area then name.
        inverter = nullptr;
        for (auto& [n, ci] : cells) {
            if (ci.invIn < 0 || ci.cell->dontUse) continue;
            if (!inverter || ci.area < inverter->area || (ci.area == inverter->area && ci.name < inverter->name))
                inverter = &ci;
        }
    }
};

// ------------------------------------------------------------------- design

// A flat module as nets and cell pins. Built from a gnl::Module; the ECO
// edits it and emit() writes the edits back into the module.
struct Inst {
    std::string name;
    const CellInfo* ci = nullptr;
    std::vector<int> pin;  // net of each cell pin, -1: open
    int src = -1;          // index in Module::insts, -1: added by the ECO
    bool dead = false;
    std::vector<std::vector<int>> connPins;  // per connection, per bit (msb first): cell pin or -1
};

struct Net {
    std::string name;
    gnl::Ref ref;       // how to write it
    int drvInst = -1, drvPin = -1;
    int cval = -1;      // 0 or 1: a constant
    int inPort = -1;    // driven by this input port
    std::vector<gnl::Ref> aliases;  // all its names, when it has several
    bool aliased = false;
    int firstBit = -1, bestBit = -1;
    bool added = false;
};

struct NameBits {
    int base = 0;  // the first bit of this name (a bus: msb first, contiguous)
    int msb = 0, lsb = 0;
    bool ranged = false;
    int rank = 1;  // 0: input port, 1: wire, 2: output port
};

struct Port {
    std::string name;  // "y" or "y[3]"
    gnl::Ref ref;
    bool out = false;
    int net = -1, orig = -1;
    bool added = false;
};

struct Design {
    gnl::Module mod;
    std::vector<Net> nets;
    std::vector<Inst> insts;
    std::vector<Port> ports;
    std::unordered_map<std::string, int> instIdx, portIdx;  // instIdx: registers and macros
    std::vector<std::vector<int>> assignNets;  // per assign: the net of each bit, lsb first
    std::vector<std::string> warnings;
    std::unordered_map<std::string, NameBits> nameBits;    // every declared or used net name
    std::unordered_set<std::string> netNames, instNames;  // names the ECO added

    bool netNameUsed(const std::string& n) const { return nameBits.count(n) || netNames.count(n); }
    std::string newNetName(const std::string& base) {
        std::string n = base;
        for (int k = 1; netNameUsed(n); k++) n = base + "_" + std::to_string(k);
        netNames.insert(n);
        return n;
    }
    // Instance names are only collected when the ECO has to invent one.
    bool instNameUsed(const std::string& n) {
        if (!haveInstNames) {
            haveInstNames = true;
            instNames.reserve(insts.size() * 2);
            for (auto& x : insts) instNames.insert(x.name);
        }
        return instNames.count(n) > 0;
    }
    std::string newInstName(const std::string& base) {
        std::string n = base;
        for (int k = 1; instNameUsed(n); k++) n = base + "_" + std::to_string(k);
        instNames.insert(n);
        return n;
    }
    bool haveInstNames = false;
};

inline bool sameRef(const gnl::Ref& a, const gnl::Ref& b) {
    return a.kind == b.kind && a.name == b.name && a.msb == b.msb && a.lsb == b.lsb && a.bits == b.bits;
}

inline gnl::Ref constRef(int v) {
    gnl::Ref r;
    r.kind = gnl::Ref::Const;
    r.bits = {(uint8_t)v};
    return r;
}

inline gnl::Ref netRef(const std::string& n) {
    gnl::Ref r;
    r.kind = gnl::Ref::Net;
    r.name = n;
    return r;
}

// The bits of a declared name, msb first, as references.
inline void declBits(const gnl::Module& m, const std::string& name, std::vector<gnl::Ref>& out) {
    const gnl::Decl* d = m.find(name);
    if (!d || !d->ranged) {
        out.push_back(netRef(name));
        return;
    }
    int step = d->msb >= d->lsb ? -1 : 1;
    for (int i = d->msb;; i += step) {
        gnl::Ref r;
        r.kind = gnl::Ref::Bit;
        r.name = name;
        r.msb = r.lsb = i;
        out.push_back(r);
        if (i == d->lsb) break;
    }
}

// A connection or assign side as one reference per bit, msb first.
inline std::vector<gnl::Ref> expandRefs(const gnl::Module& m, const std::vector<gnl::Ref>& refs) {
    std::vector<gnl::Ref> out;
    for (auto& r : refs) {
        switch (r.kind) {
            case gnl::Ref::Const:
                for (auto b : r.bits) {
                    gnl::Ref c;
                    c.kind = gnl::Ref::Const;
                    c.bits = {b};
                    out.push_back(c);
                }
                break;
            case gnl::Ref::Net: declBits(m, r.name, out); break;
            case gnl::Ref::Bit:
            case gnl::Ref::Part: {
                int step = r.msb >= r.lsb ? -1 : 1;
                for (int i = r.msb;; i += step) {
                    gnl::Ref b;
                    b.kind = gnl::Ref::Bit;
                    b.name = r.name;
                    b.msb = b.lsb = i;
                    out.push_back(b);
                    if (i == r.lsb) break;
                }
                break;
            }
            case gnl::Ref::Open: break;
        }
    }
    return out;
}

inline std::string refKey(const gnl::Ref& r) {
    return r.kind == gnl::Ref::Bit ? r.name + "[" + std::to_string(r.msb) + "]" : r.name;
}

inline Design build(const gnl::Module& m, const Lib& lib, bool keepModule = true) {
    Design d;
    if (keepModule) d.mod = m;
    // Bits are numbered per declared name, a bus contiguously and msb first,
    // so a reference resolves with one lookup of its name.
    std::vector<int> parent;
    std::vector<int> bitName, bitIdx;  // per bit: index into names (-1: constant), bus index (or the constant)
    std::vector<const std::string*> names;
    int c0 = -1, c1 = -1;
    parent.reserve(m.decls.size() * 2 + 16);
    d.nameBits.reserve(m.decls.size() * 2 + 16);
    auto declare = [&](const std::string& name, const gnl::Decl* dc) -> const NameBits& {
        auto [it, isNew] = d.nameBits.try_emplace(name);
        NameBits& nb = it->second;
        if (!isNew) return nb;
        nb.base = (int)parent.size();
        if (dc && dc->ranged) nb.ranged = true, nb.msb = dc->msb, nb.lsb = dc->lsb;
        nb.rank = dc && dc->isPort ? (dc->dir == gnl::Dir::Out ? 2 : 0) : 1;
        int w = nb.ranged ? std::abs(nb.msb - nb.lsb) + 1 : 1;
        int step = nb.msb >= nb.lsb ? -1 : 1;
        for (int k = 0; k < w; k++) {
            parent.push_back((int)parent.size());
            bitName.push_back((int)names.size());
            bitIdx.push_back(nb.ranged ? nb.msb + k * step : -1);
        }
        names.push_back(&it->first);
        return nb;
    };
    auto find = [&](int a) {
        while (parent[(size_t)a] != a) a = parent[(size_t)a] = parent[(size_t)parent[(size_t)a]];
        return a;
    };
    auto unite = [&](int a, int b) {
        a = find(a), b = find(b);
        if (a != b) parent[(size_t)std::max(a, b)] = std::min(a, b);
    };
    auto constBit = [&](int v) {
        int& c = v ? c1 : c0;
        if (c < 0) {
            c = (int)parent.size();
            parent.push_back(c);
            bitName.push_back(-1);
            bitIdx.push_back(v);
        }
        return c;
    };
    for (auto& dc : m.decls) {
        const NameBits& nb = declare(dc.name, &dc);
        if (dc.tie >= 0)
            for (int k = 0, w = nb.ranged ? std::abs(nb.msb - nb.lsb) + 1 : 1; k < w; k++) unite(constBit(dc.tie), nb.base + k);
    }
    auto resolve = [&](const std::vector<gnl::Ref>& refs, std::vector<int>& bits) {
        bits.clear();
        for (auto& r : refs) {
            if (r.kind == gnl::Ref::Open) continue;
            if (r.kind == gnl::Ref::Const) {
                for (auto b : r.bits) bits.push_back(b <= 1 ? constBit(b) : -1);
                continue;
            }
            auto it = d.nameBits.find(r.name);
            const NameBits& nb = it != d.nameBits.end() ? it->second : declare(r.name, nullptr);
            if (r.kind == gnl::Ref::Net) {
                for (int k = 0, w = nb.ranged ? std::abs(nb.msb - nb.lsb) + 1 : 1; k < w; k++) bits.push_back(nb.base + k);
                continue;
            }
            int lo = std::min(nb.msb, nb.lsb), hi = std::max(nb.msb, nb.lsb);
            int step = r.msb >= r.lsb ? -1 : 1;
            for (int i = r.msb;; i += step) {
                if (!nb.ranged || i < lo || i > hi)
                    throw Error("module " + m.name + ": index " + std::to_string(i) + " is outside " + r.name);
                bits.push_back(nb.base + (nb.msb >= nb.lsb ? nb.msb - i : i - nb.msb));
                if (i == r.lsb) break;
            }
        }
    };
    std::vector<int> a, b, bits;
    d.assignNets.reserve(m.assigns.size());
    for (auto& [lhs, rhs] : m.assigns) {
        resolve(lhs, a);
        resolve(rhs, b);
        size_t n = std::min(a.size(), b.size());
        std::vector<int> ab;
        for (size_t k = 0; k < n; k++) {
            int x = a[a.size() - 1 - k], y = b[b.size() - 1 - k];
            if (x >= 0 && y >= 0) unite(x, y);
            ab.push_back(x);
        }
        d.assignNets.push_back(std::move(ab));
    }
    d.insts.reserve(m.insts.size());
    const CellInfo* lastCell = nullptr;
    for (size_t ii = 0; ii < m.insts.size(); ii++) {
        auto& in = m.insts[ii];
        const CellInfo* ci = lastCell && lastCell->name == in.cell ? lastCell : lib.find(in.cell);
        if (!ci)
            throw Error("instance " + in.name + ": cell " + in.cell +
                        " is not in a library (a hierarchical netlist is flattened first; a macro needs a .lib)");
        lastCell = ci;
        Inst x;
        x.name = in.name;
        x.ci = ci;
        x.src = (int)ii;
        x.pin.assign(ci->pins.size(), -1);
        x.connPins.reserve(in.conns.size());
        bool positional = !in.conns.empty() && in.conns[0].port.empty();
        for (size_t k = 0; k < in.conns.size(); k++) {
            auto& c = in.conns[k];
            const std::vector<int>* pp = nullptr;
            if (positional) {
                if (k >= ci->ports.size()) throw Error("instance " + in.name + ": too many connections for " + in.cell);
                pp = &ci->ports[k].second;
            } else {
                // the same order as the library is the common case
                pp = k < ci->ports.size() && ci->ports[k].first == c.port ? &ci->ports[k].second : ci->port(c.port);
                if (!pp) throw Error("instance " + in.name + ": cell " + in.cell + " has no pin " + c.port);
            }
            resolve(c.refs, bits);
            std::vector<int> cp(bits.size(), -1);
            size_t n = std::min(bits.size(), pp->size());
            for (size_t j = 0; j < n; j++) {
                int pinI = (*pp)[pp->size() - 1 - j];
                x.pin[(size_t)pinI] = bits[bits.size() - 1 - j];
                cp[bits.size() - 1 - j] = pinI;
            }
            x.connPins.push_back(std::move(cp));
        }
        if (ci->kind != Kind::Comb && !d.instIdx.emplace(x.name, (int)d.insts.size()).second)
            throw Error("instance " + x.name + " is defined twice");
        d.insts.push_back(std::move(x));
    }
    // Number the nets; a net is written with the name of an input port,
    // else a wire, else an output port.
    std::vector<int> id(parent.size(), -1), rank(parent.size(), 9);
    auto bitRef = [&](size_t i) {
        gnl::Ref r;
        r.name = *names[(size_t)bitName[i]];
        if (bitIdx[i] < 0) r.kind = gnl::Ref::Net;
        else r.kind = gnl::Ref::Bit, r.msb = r.lsb = bitIdx[i];
        return r;
    };
    for (size_t i = 0; i < parent.size(); i++) {
        int r = find((int)i);
        if (id[(size_t)r] < 0) {
            id[(size_t)r] = (int)d.nets.size();
            d.nets.emplace_back();
        }
        int ni = id[(size_t)r];
        Net& n = d.nets[(size_t)ni];
        if (bitName[i] < 0) {
            n.cval = bitIdx[i];
            continue;
        }
        if (n.firstBit < 0) n.firstBit = (int)i;
        else n.aliased = true;
        int rk = d.nameBits.find(*names[(size_t)bitName[i]])->second.rank;
        if (rk < rank[(size_t)ni]) rank[(size_t)ni] = rk, n.bestBit = (int)i;
    }
    for (auto& n : d.nets) {
        if (n.cval >= 0) {
            n.ref = constRef(n.cval);
            n.name = n.cval ? "1'b1" : "1'b0";
        } else {
            n.ref = bitRef((size_t)n.bestBit);
            n.name = refKey(n.ref);
        }
    }
    // The other names of the nets that have several (assigns).
    for (size_t i = 0; i < parent.size(); i++) {
        if (bitName[i] < 0) continue;
        Net& n = d.nets[(size_t)id[(size_t)find((int)i)]];
        if (n.aliased) n.aliases.push_back(bitRef(i));
    }
    auto map = [&](int bit) { return bit < 0 ? -1 : id[(size_t)find(bit)]; };
    for (auto& x : d.insts)
        for (auto& p : x.pin) p = map(p);
    for (auto& ab : d.assignNets)
        for (auto& bit : ab) bit = map(bit);
    for (auto& pn : m.portOrder) {
        const gnl::Decl* dc = m.find(pn);
        const NameBits& nb = declare(pn, dc);
        for (int k = 0, w = nb.ranged ? std::abs(nb.msb - nb.lsb) + 1 : 1; k < w; k++) {
            Port p;
            p.ref = bitRef((size_t)(nb.base + k));
            p.name = refKey(p.ref);
            p.out = dc && dc->dir == gnl::Dir::Out;
            if (dc && dc->dir == gnl::Dir::InOut && k == 0)
                d.warnings.push_back("inout port " + pn + " is read as an input");
            p.net = p.orig = map(nb.base + k);
            d.portIdx[p.name] = (int)d.ports.size();
            if (!p.out) {
                Net& n = d.nets[(size_t)p.net];
                if (n.inPort < 0) n.inPort = (int)d.ports.size();
            }
            d.ports.push_back(std::move(p));
        }
    }
    // Drivers.
    for (size_t i = 0; i < d.insts.size(); i++) {
        auto& x = d.insts[i];
        for (size_t k = 0; k < x.pin.size(); k++) {
            if (!x.ci->isOut[k] || x.pin[k] < 0) continue;
            Net& n = d.nets[(size_t)x.pin[k]];
            if (n.drvInst >= 0 || n.inPort >= 0 || n.cval >= 0) {
                d.warnings.push_back("net " + n.name + " has more than one driver; " + x.name + "/" + x.ci->pins[k] + " is ignored");
                continue;
            }
            n.drvInst = (int)i;
            n.drvPin = (int)k;
        }
    }
    return d;
}

// A flattened hierarchy as one module, so that a hierarchical netlist goes
// through the same path (it is then written back flat).
inline gnl::Module flatModule(const gnl::Source& src, const std::string& top, const Lib& lib) {
    const gnl::Module* tm = src.module(top);
    if (!tm) throw Error("no module named " + top);
    bool flat = true;
    for (auto& i : tm->insts)
        if (src.module(i.cell)) flat = false;
    if (flat) return *tm;
    gnl::Netlist nl = gnl::flatten(
        src, top,
        [&](const std::string& cell, const std::string& pin, int) {
            std::vector<std::string> r;
            if (auto* ci = lib.find(cell))
                if (auto* pp = ci->port(pin))
                    if (pp->size() > 1)
                        for (int i : *pp) r.push_back(ci->pins[(size_t)i]);
            return r;
        },
        [&](const std::string& cell) {
            std::vector<std::string> r;
            if (auto* ci = lib.find(cell))
                for (auto& p : ci->ports) r.push_back(p.first);
            return r;
        });
    gnl::Module m;
    m.name = tm->name;
    m.portOrder = tm->portOrder;
    for (auto& pn : tm->portOrder) {
        const gnl::Decl* dc = tm->find(pn);
        gnl::Decl& nd = m.decl(pn);
        if (dc) nd = *dc;
        nd.name = pn;
        nd.isPort = true;
    }
    std::vector<gnl::Ref> ref(nl.nets.size());
    std::vector<uint8_t> have(nl.nets.size(), 0);
    if (nl.const0 >= 0) ref[(size_t)nl.const0] = constRef(0), have[(size_t)nl.const0] = 1;
    if (nl.const1 >= 0) ref[(size_t)nl.const1] = constRef(1), have[(size_t)nl.const1] = 1;
    auto portRef = [&](const gnl::FlatPort& p) {
        gnl::Ref r;
        if (p.bus.empty()) return netRef(p.name);
        r.kind = gnl::Ref::Bit;
        r.name = p.bus;
        r.msb = r.lsb = p.bit;
        return r;
    };
    for (int pass = 0; pass < 2; pass++)  // inputs name their net first
        for (auto& p : nl.ports) {
            if (p.net < 0 || (pass == 0) != (p.dir != gnl::Dir::Out)) continue;
            if (!have[(size_t)p.net]) {
                ref[(size_t)p.net] = portRef(p), have[(size_t)p.net] = 1;
            } else if (p.dir == gnl::Dir::Out) {
                m.assigns.push_back({{portRef(p)}, {ref[(size_t)p.net]}});
            }
        }
    std::unordered_set<std::string> used(m.portOrder.begin(), m.portOrder.end());
    for (size_t i = 0; i < nl.nets.size(); i++) {
        if (have[i]) continue;
        std::string n = nl.nets[i];
        while (!used.insert(n).second) n += "_";
        m.decl(n);
        ref[i] = netRef(n);
    }
    for (auto& fi : nl.insts) {
        gnl::Inst in;
        in.cell = fi.cell;
        in.name = fi.name;
        const CellInfo* ci = lib.find(fi.cell);
        if (!ci) throw Error("instance " + fi.name + ": cell " + fi.cell + " is not in a library");
        std::unordered_map<std::string, int> netOf;
        for (auto& c : fi.conns) netOf[c.pin] = c.net;
        for (auto& [port, bits] : ci->ports) {
            gnl::Conn c;
            c.port = port;
            bool any = false;
            for (int b : bits) {
                auto it = netOf.find(ci->pins[(size_t)b]);
                if (it != netOf.end() && it->second >= 0) any = true;
            }
            if (!any) continue;
            for (int b : bits) {
                auto it = netOf.find(ci->pins[(size_t)b]);
                if (it != netOf.end() && it->second >= 0) c.refs.push_back(ref[(size_t)it->second]);
                else {
                    gnl::Ref z;  // an open bit of a bus pin
                    z.kind = gnl::Ref::Const;
                    z.bits = {3};
                    c.refs.push_back(z);
                }
            }
            in.conns.push_back(std::move(c));
        }
        m.insts.push_back(std::move(in));
    }
    return m;
}

// -------------------------------------------------------------------- logic

// The variables both designs are functions of.
struct Vars {
    Aig g;
    std::unordered_map<std::string, Lit> byKey;
    std::map<std::string, int> ties;
    Lit var(const std::string& key) {
        auto it = byKey.find(key);
        if (it != byKey.end()) return it->second;
        Lit l = g.input();
        byKey.emplace(key, l);
        return l;
    }
    Lit input(const std::string& port) {
        auto it = ties.find(port);
        if (it != ties.end()) return it->second ? kTrue : kFalse;
        return var("pi:" + port);
    }
};

template <class PinLit>
inline Lit evalProg(Aig& g, const Prog& p, PinLit&& pinLit, Lit iq) {
    Lit st[64];
    int sp = 0;
    for (int c : p.code) {
        if (c >= 0) st[sp++] = pinLit(c);
        else if (c == Prog::Not) st[sp - 1] ^= 1;
        else if (c == Prog::C0) st[sp++] = kFalse;
        else if (c == Prog::C1) st[sp++] = kTrue;
        else if (c == Prog::Iq) st[sp++] = iq;
        else if (c == Prog::Iqn) st[sp++] = iq ^ 1;
        else {
            Lit b = st[--sp], a = st[sp - 1];
            st[sp - 1] = c == Prog::And ? g.and_(a, b) : c == Prog::Or ? g.or_(a, b) : g.xor_(a, b);
        }
        if (sp >= 63) throw Error("a cell function is too deep");
    }
    return sp ? st[sp - 1] : kFalse;
}

struct Model {
    const Design* d = nullptr;
    Vars* v = nullptr;
    std::vector<Lit> lit;  // per net
    std::vector<std::string> warnings;

    Lit pinLit(const Inst& x, int pin) const { return x.pin[(size_t)pin] < 0 ? kFalse : lit[(size_t)x.pin[(size_t)pin]]; }
    Lit stateLit(const Inst& x) const { return x.ci->kind == Kind::State ? v->var("st:" + x.name) : kFalse; }
    // What output pin `pin` of x computes, connected or not.
    Lit outLit(const Inst& x, int pin) const {
        if (x.ci->kind == Kind::Box) return v->var("bx:" + x.name + "/" + x.ci->pins[(size_t)pin]);
        return evalProg(v->g, x.ci->fn[(size_t)pin], [&](int p) { return pinLit(x, p); }, stateLit(x));
    }
    // A state function (next, clock, clear, preset) with one pin optionally replaced.
    Lit comp(const Inst& x, const Prog& p, int overPin = -1, Lit over = 0) const {
        if (p.empty()) return kFalse;
        return evalProg(v->g, p, [&](int q) { return q == overPin ? over : pinLit(x, q); }, stateLit(x));
    }

    void compute(const Design& des, Vars& vars) {
        d = &des;
        v = &vars;
        size_t n = des.nets.size();
        lit.assign(n, 0);
        std::vector<uint8_t> state(n, 0);
        size_t floating = 0, loops = 0;
        std::string firstFloat, firstLoop;
        std::vector<std::pair<int, int>> stack;  // net, next pin to visit
        for (size_t root = 0; root < n; root++) {
            if (state[root]) continue;
            stack.push_back({(int)root, 0});
            state[root] = 1;
            while (!stack.empty()) {
                int net = stack.back().first;
                const Net& nn = des.nets[(size_t)net];
                bool wait = false;
                if (nn.cval < 0 && nn.inPort < 0 && nn.drvInst >= 0 && des.insts[(size_t)nn.drvInst].ci->kind != Kind::Box) {
                    const Inst& x = des.insts[(size_t)nn.drvInst];
                    const Prog& fn = x.ci->fn[(size_t)nn.drvPin];
                    for (int& k = stack.back().second; k < (int)fn.code.size();) {
                        int c = fn.code[(size_t)k++];
                        if (c < 0 || x.pin[(size_t)c] < 0) continue;
                        int dep = x.pin[(size_t)c];
                        if (state[(size_t)dep] == 2) continue;
                        if (state[(size_t)dep] == 1) {  // a combinational loop: cut it here
                            if (!loops++) firstLoop = des.nets[(size_t)dep].name;
                            lit[(size_t)dep] = vars.var("loop:" + des.nets[(size_t)dep].name);
                            continue;
                        }
                        state[(size_t)dep] = 1;
                        stack.push_back({dep, 0});
                        wait = true;
                        break;
                    }
                    if (wait) continue;
                    lit[(size_t)net] = outLit(x, nn.drvPin);
                } else if (nn.cval >= 0) {
                    lit[(size_t)net] = nn.cval ? kTrue : kFalse;
                } else if (nn.inPort >= 0) {
                    lit[(size_t)net] = vars.input(des.ports[(size_t)nn.inPort].name);
                } else if (nn.drvInst >= 0) {
                    lit[(size_t)net] = outLit(des.insts[(size_t)nn.drvInst], nn.drvPin);
                } else {
                    if (!floating++) firstFloat = nn.name;
                    lit[(size_t)net] = vars.var("float:" + nn.name);
                }
                state[(size_t)net] = 2;
                stack.pop_back();
            }
        }
        if (floating)
            warnings.push_back(std::to_string(floating) + " undriven net(s), read as free inputs (first: " + firstFloat + ")");
        if (loops)
            warnings.push_back(std::to_string(loops) + " combinational loop(s) cut (first at " + firstLoop + ")");
    }
};

// The value of a net on the simulation patterns.
struct Sig {
    enum { Max = 8 };
    uint64_t w[Max] = {};
};

template <class PinSig>
inline Sig evalProgSig(const Prog& p, PinSig&& pinSig, const Sig& iq, int words) {
    Sig st[64];
    int sp = 0;
    for (int c : p.code) {
        if (c >= 0) st[sp++] = pinSig(c);
        else if (c == Prog::Not) { for (int w = 0; w < words; w++) st[sp - 1].w[w] = ~st[sp - 1].w[w]; }
        else if (c == Prog::C0) st[sp++] = Sig();
        else if (c == Prog::C1) { Sig one; for (int w = 0; w < words; w++) one.w[w] = ~0ull; st[sp++] = one; }
        else if (c == Prog::Iq) st[sp++] = iq;
        else if (c == Prog::Iqn) { st[sp] = iq; for (int w = 0; w < words; w++) st[sp].w[w] = ~st[sp].w[w]; sp++; }
        else {
            Sig& a = st[sp - 2];
            const Sig& b = st[sp - 1];
            for (int w = 0; w < words; w++)
                a.w[w] = c == Prog::And ? a.w[w] & b.w[w] : c == Prog::Or ? a.w[w] | b.w[w] : a.w[w] ^ b.w[w];
            sp--;
        }
    }
    return sp ? st[sp - 1] : Sig();
}

// Simulation signatures and SAT proofs over the shared graph.
struct Prover {
    Aig& g;
    int words;  // at most Sig::Max
    long budget;
    std::vector<uint64_t> sig;  // node * words
    size_t done = 0;
    std::unordered_map<uint32_t, Lit> lemma;  // node -> an equal literal of a lower node

    // Records a == b (proved). Not to be called while proofs run.
    void learn(Lit a, Lit b) {
        if ((a >> 1) == (b >> 1)) return;
        if ((a >> 1) < (b >> 1)) std::swap(a, b);
        lemma.emplace(a >> 1, b ^ (a & 1));
    }

    Prover(Aig& a, int w, long b) : g(a), words(w), budget(b) {}

    static uint64_t mix(uint64_t x) {
        x += 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }
    // Simulates the nodes added since the last call.
    void sync(unsigned threads) {
        size_t n = g.size();
        if (done >= n) return;
        sig.resize(n * (size_t)words);
        size_t from = done;
        par::parallelFor((size_t)words, threads, [&](size_t w) {
            for (size_t i = from; i < n; i++) {
                uint64_t r;
                if (i == 0) r = 0;
                else if (g.isInput((uint32_t)i)) r = mix(i * 1315423911ull + w * 2654435761ull + 12345);
                else {
                    Lit a = g.node((uint32_t)i).a, b = g.node((uint32_t)i).b;
                    uint64_t x = sig[(a >> 1) * (size_t)words + w] ^ (a & 1 ? ~0ull : 0);
                    uint64_t y = sig[(b >> 1) * (size_t)words + w] ^ (b & 1 ? ~0ull : 0);
                    r = x & y;
                }
                sig[i * (size_t)words + w] = r;
            }
        });
        done = n;
    }
    uint64_t word(Lit a, int w) const { return sig[(a >> 1) * (size_t)words + (size_t)w] ^ (a & 1 ? ~0ull : 0); }
    bool sigEqual(Lit a, Lit b) const {
        for (int w = 0; w < words; w++)
            if (word(a, w) != word(b, w)) return false;
        return true;
    }
    Sig sigOf(Lit a) const {
        Sig s;
        for (int w = 0; w < words; w++) s.w[w] = word(a, w);
        return s;
    }
    bool sigIs(const Sig& s, Lit a) const {
        for (int w = 0; w < words; w++)
            if (s.w[w] != word(a, w)) return false;
        return true;
    }
    uint64_t sigHash(const Sig& s) const {
        uint64_t h = 1469598103934665603ull;
        for (int w = 0; w < words; w++) h = mix(h ^ s.w[w]);
        return h;
    }
    uint64_t sigHash(Lit a) const {
        uint64_t h = 1469598103934665603ull;
        for (int w = 0; w < words; w++) h = mix(h ^ word(a, w));
        return h;
    }
    // 1: equal, 0: different, -1: undecided within the budget. Reads the
    // graph only, so calls may run in parallel (after sync()).
    int prove(Lit a, Lit b, long limit = 0) const {
        if (a == b) return 1;
        if ((a ^ 1) == b) return 0;
        if (!sigEqual(a, b)) return 0;
        equiv::Cnf c(g);
        int x = c.lit(a), y = c.lit(b);
        // Equivalences proved earlier are given to the solver: a proof over
        // a patched cone then ends at the old nets the patch reads. In node
        // order, so the clause order (and the search) is always the same.
        if (!lemma.empty()) {
            std::vector<std::pair<uint32_t, int>> vs(c.var.begin(), c.var.end());
            std::sort(vs.begin(), vs.end());
            for (auto& [node, var] : vs) {
                auto it = lemma.find(node);
                if (it == lemma.end()) continue;
                auto other = c.var.find(it->second >> 1);
                if (other == c.var.end()) continue;
                int q = (it->second & 1) ? -other->second : other->second;
                c.sat.addClause({-var, q});
                c.sat.addClause({var, -q});
            }
        }
        c.sat.addClause({x, y});  // a != b
        c.sat.addClause({-x, -y});
        auto r = c.sat.solve({}, (int64_t)(limit ? limit : budget));
        return r == sat::Unsat ? 1 : r == sat::Sat ? 0 : -1;
    }
};

// ---------------------------------------------------------------------- eco

struct Options {
    std::map<std::string, int> ties;  // primary inputs held at 0 or 1 in both designs
    unsigned threads = 0;             // 0: all logical cores
    long budget = 100000;             // SAT conflicts per compare point
    long netBudget = 2000;            // SAT conflicts per candidate for an existing net
    int simWords = 8;                 // 64 patterns each, at most 8
    bool rectify = true;              // look for the gate to change before copying its fanout
    long rectifyWork = 20000000;      // gate evaluations the search may spend
    std::string prefix = "ECO_";
    bool sweep = true;                // remove old cells the patch leaves without a load
    std::set<std::string> replace;    // registers to swap for the new cell (set by the retry)
    bool invert = true;               // reuse an old net with the inverse function through an inverter
    bool spare = false;               // metal-only: build the patch from cells already in the netlist
    std::string spareGlob = "*";      // which unloaded old cells are spare cells (instance name, * and ?)
};

struct Change {
    enum Kind { AddCell, RemoveCell, AddNet, AddPort, Connect, Disconnect } kind;
    std::string a, b;  // cell: name, cell type; net/port: name, direction; (dis)connect: net, inst/pin
};

struct Result {
    gnl::Module patched;
    std::vector<Change> changes;
    std::vector<std::string> failing;     // compare points that differed
    std::vector<std::string> unverified;  // still different after the patch
    std::vector<std::string> warnings;
    size_t points = 0, added = 0, removed = 0, rewired = 0, reused = 0;
    size_t spares = 0, freed = 0;         // metal-only: spare cells put to use, old cells left as spares
    std::vector<std::string> unplaced;    // metal-only: added cells no spare cell could stand in for
    double areaAdded = 0, areaRemoved = 0;
    bool verified = false;
};

struct Point {
    std::string name;
    Lit o = 0, n = 0;
    int status = 1;
    enum What { Out, Next, Clock, Clear, Preset, BoxPin } what = Out;
    int oi = -1, ni = -1;  // instance (or port) in each design
    int pin = -1;          // BoxPin
};

namespace detail {

inline const Prog& compProg(const CellInfo& ci, Point::What w) {
    return w == Point::Next ? ci.next : w == Point::Clock ? ci.clock : w == Point::Clear ? ci.clear : ci.preset;
}

// The compare points of `a` against the reference `n`: outputs, state
// functions and black-box pins that exist in both by name.
inline std::vector<Point> points(const Design& a, const Model& am, const Design& n, const Model& nm) {
    std::vector<Point> pts;
    for (size_t i = 0; i < n.ports.size(); i++) {
        auto& p = n.ports[i];
        if (!p.out) continue;
        auto it = a.portIdx.find(p.name);
        if (it == a.portIdx.end() || !a.ports[(size_t)it->second].out) continue;
        Point pt;
        pt.name = p.name;
        pt.what = Point::Out;
        pt.oi = it->second;
        pt.ni = (int)i;
        pt.o = am.lit[(size_t)a.ports[(size_t)it->second].net];
        pt.n = nm.lit[(size_t)p.net];
        pts.push_back(std::move(pt));
    }
    for (size_t i = 0; i < n.insts.size(); i++) {
        auto& x = n.insts[i];
        if (x.dead || x.ci->kind == Kind::Comb) continue;
        auto it = a.instIdx.find(x.name);
        if (it == a.instIdx.end()) continue;
        auto& y = a.insts[(size_t)it->second];
        if (y.dead || y.ci->kind != x.ci->kind) continue;
        if (x.ci->kind == Kind::State) {
            if (x.ci->latch != y.ci->latch) continue;
            static const char* nm4[] = {".next", ".clock", ".clear", ".preset"};
            for (int w = 0; w < 4; w++) {
                Point pt;
                pt.what = (Point::What)(Point::Next + w);
                pt.name = x.name + nm4[w];
                pt.oi = it->second;
                pt.ni = (int)i;
                pt.o = am.comp(y, compProg(*y.ci, pt.what));
                pt.n = nm.comp(x, compProg(*x.ci, pt.what));
                pts.push_back(std::move(pt));
            }
        } else {
            if (x.ci != y.ci) continue;
            for (size_t k = 0; k < x.pin.size(); k++) {
                if (x.ci->isOut[k]) continue;
                Point pt;
                pt.what = Point::BoxPin;
                pt.name = x.name + "/" + x.ci->pins[k];
                pt.oi = it->second;
                pt.ni = (int)i;
                pt.pin = (int)k;
                pt.o = am.pinLit(y, (int)k);
                pt.n = nm.pinLit(x, (int)k);
                pts.push_back(std::move(pt));
            }
        }
    }
    return pts;
}

// The cells without a load: nothing reads their outputs, directly or through
// other such cells. Only cells for which `may` holds are considered.
template <class May>
inline std::vector<uint8_t> unloaded(const Design& d, May&& may) {
    std::vector<int> loads(d.nets.size(), 0);
    for (auto& x : d.insts) {
        if (x.dead) continue;
        for (size_t k = 0; k < x.pin.size(); k++)
            if (!x.ci->isOut[k] && x.pin[k] >= 0) loads[(size_t)x.pin[k]]++;
    }
    for (auto& p : d.ports)
        if (p.out && p.net >= 0) loads[(size_t)p.net]++;
    std::vector<uint8_t> gone(d.insts.size(), 0);
    auto free = [&](size_t i) {
        auto& x = d.insts[i];
        if (x.dead || gone[i] || !may(i)) return false;
        bool anyOut = false;
        for (size_t k = 0; k < x.pin.size(); k++) {
            if (!x.ci->isOut[k]) continue;
            anyOut = true;
            if (x.pin[k] >= 0 && loads[(size_t)x.pin[k]] > 0) return false;
        }
        return anyOut;
    };
    std::vector<size_t> work;
    for (size_t i = 0; i < d.insts.size(); i++)
        if (free(i)) work.push_back(i);
    while (!work.empty()) {
        size_t i = work.back();
        work.pop_back();
        if (!free(i)) continue;
        gone[i] = 1;
        auto& x = d.insts[i];
        for (size_t k = 0; k < x.pin.size(); k++) {
            if (x.ci->isOut[k] || x.pin[k] < 0) continue;
            int net = x.pin[k];
            if (--loads[(size_t)net] == 0 && d.nets[(size_t)net].drvInst >= 0) {
                size_t drv = (size_t)d.nets[(size_t)net].drvInst;
                if (!d.insts[drv].dead && d.insts[drv].pin[(size_t)d.nets[(size_t)net].drvPin] == net) work.push_back(drv);
            }
        }
    }
    return gone;
}

inline bool globMatch(const char* p, const char* s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return true;
            for (; *s; s++)
                if (globMatch(p, s)) return true;
            return false;
        }
        if (!*s || (*p != '?' && *p != *s)) return false;
    }
    return !*s;
}

// A pin function on 64 input patterns at once.
template <class PinWord>
inline uint64_t evalProg64(const Prog& p, PinWord&& pinWord) {
    uint64_t st[64];
    int sp = 0;
    for (int c : p.code) {
        if (c >= 0) st[sp++] = pinWord(c);
        else if (c == Prog::Not) st[sp - 1] = ~st[sp - 1];
        else if (c == Prog::C0 || c == Prog::Iq) st[sp++] = 0;
        else if (c == Prog::C1 || c == Prog::Iqn) st[sp++] = ~0ull;
        else {
            uint64_t b = st[--sp];
            st[sp - 1] = c == Prog::And ? st[sp - 1] & b : c == Prog::Or ? st[sp - 1] | b : st[sp - 1] ^ b;
        }
    }
    return sp ? st[sp - 1] : 0;
}

// A combinational cell with one output and at most six inputs, as a truth
// table over its inputs in pin order.
struct Gate1 {
    std::vector<int> in;
    int out = -1;
    uint64_t tt = 0;
};
inline bool gate1(const CellInfo& ci, Gate1& g) {
    static const uint64_t pat[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
                                    0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
    if (ci.kind != Kind::Comb) return false;
    g = Gate1();
    for (size_t k = 0; k < ci.pins.size(); k++) {
        if (!ci.isOut[k]) g.in.push_back((int)k);
        else if (g.out >= 0) return false;
        else g.out = (int)k;
    }
    if (g.out < 0 || g.in.size() > 6) return false;
    std::vector<uint64_t> w(ci.pins.size(), 0);
    for (size_t j = 0; j < g.in.size(); j++) w[(size_t)g.in[j]] = pat[j];
    g.tt = evalProg64(ci.fn[(size_t)g.out], [&](int pin) { return w[(size_t)pin]; });
    return true;
}

// How cell `s` can compute the function `tt` of `k` inputs: for each input
// of s, the input of the function it reads (0..k-1), or k for constant 0,
// k+1 for constant 1. Empty when it cannot.
inline std::vector<int> standIn(const Gate1& s, uint64_t tt, int k) {
    const int m = (int)s.in.size();
    const uint64_t mask = k == 6 ? ~0ull : (1ull << (1u << k)) - 1;
    double combos = 1;
    for (int i = 0; i < m; i++) combos *= k + 2;
    if (m == 0 || combos > 300000) return {};
    std::vector<int> a((size_t)m, 0);
    for (;;) {
        // The table of s under this assignment, one pattern at a time over
        // the 2^m rows of s folded onto the 2^k rows of the function.
        uint64_t r = 0;
        for (unsigned row = 0; row < (1u << k); row++) {
            unsigned idx = 0;
            for (int i = 0; i < m; i++) {
                int v = a[(size_t)i];
                unsigned bit = v < k ? (row >> v) & 1u : v == k ? 0u : 1u;
                idx |= bit << i;
            }
            r |= ((s.tt >> idx) & 1ull) << row;
        }
        if (((r ^ tt) & mask) == 0) return a;
        int i = 0;
        while (i < m && ++a[(size_t)i] == k + 2) a[(size_t)i++] = 0;
        if (i == m) break;
    }
    return {};
}

// Writes the edited design back into its module.
inline void emit(Design& d, const std::vector<std::vector<int>>& origPin, size_t origNets, const Options& opt,
                 Result& res) {
    gnl::Module& m = d.mod;
    // A net is dirty when an output port that was one of its names now
    // carries something else: every reference to it is then rewritten.
    std::vector<uint8_t> dirty(d.nets.size(), 0), named(d.nets.size(), 0);
    std::unordered_map<std::string, int> portOf;
    for (size_t i = 0; i < d.ports.size(); i++) portOf[d.ports[i].name] = (int)i;
    for (auto& p : d.ports)
        if (p.out && !p.added && p.net != p.orig && p.orig >= 0) dirty[(size_t)p.orig] = 1;
    std::vector<uint8_t> used(d.nets.size(), 0);
    for (auto& x : d.insts)
        if (!x.dead)
            for (int p : x.pin)
                if (p >= 0) used[(size_t)p] = 1;
    for (auto& p : d.ports)
        if (p.net >= 0) used[(size_t)p.net] = 1;
    for (size_t i = 0; i < origNets; i++) {
        Net& n = d.nets[i];
        named[i] = 1;
        if (!dirty[i] || n.cval >= 0) continue;
        int best = -1, rank = 9;
        if (n.aliases.empty()) n.aliases.push_back(n.ref);
        for (size_t k = 0; k < n.aliases.size(); k++) {
            auto it = portOf.find(refKey(n.aliases[k]));
            int r = 1;
            if (it != portOf.end()) {
                auto& p = d.ports[(size_t)it->second];
                r = !p.out ? 0 : p.net == (int)i ? 2 : 9;
            }
            if (r < rank) rank = r, best = (int)k;
        }
        if (rank < 9) n.ref = n.aliases[(size_t)best];
        else if (!used[i]) continue;  // nothing is left on it
        else {
            std::string nn = d.newNetName(opt.prefix + "old_" + n.name);
            m.decl(nn);
            n.ref = netRef(nn);
            res.changes.push_back({Change::AddNet, nn, ""});
        }
        n.name = refKey(n.ref);
    }
    // An added net that feeds an output port takes the port's name.
    for (auto& p : d.ports) {
        if (!p.out || p.net < 0 || (size_t)p.net < origNets || named[(size_t)p.net]) continue;
        if (!p.added && p.net == p.orig) continue;
        Net& n = d.nets[(size_t)p.net];
        if (n.inPort >= 0 || n.cval >= 0) continue;
        n.ref = p.ref;
        n.name = p.name;
        named[(size_t)p.net] = 1;
    }
    for (size_t i = origNets; i < d.nets.size(); i++) {
        Net& n = d.nets[i];
        if (named[i] || n.cval >= 0 || n.inPort >= 0) continue;
        m.decl(n.name);
        n.ref = netRef(n.name);
        res.changes.push_back({Change::AddNet, n.name, ""});
    }
    auto refOf = [&](int net) { return d.nets[(size_t)net].ref; };
    // New ports.
    for (auto& p : d.ports) {
        if (!p.added) continue;
        m.portOrder.push_back(p.name);
        gnl::Decl& dc = m.decl(p.name);
        dc.isPort = true;
        dc.dir = p.out ? gnl::Dir::Out : gnl::Dir::In;
        res.changes.push_back({Change::AddPort, p.name, p.out ? "output" : "input"});
    }
    // Connections of the cells that stay.
    for (auto& x : d.insts) {
        if (x.src < 0 || x.dead) continue;
        gnl::Inst& in = m.insts[(size_t)x.src];
        auto& was = origPin[(size_t)x.src];
        std::vector<uint8_t> inConn(x.pin.size(), 0);
        for (size_t k = 0; k < in.conns.size(); k++) {
            auto& cp = x.connPins[k];
            bool rewrite = false;
            for (int pinI : cp) {
                if (pinI < 0) continue;
                inConn[(size_t)pinI] = 1;
                int now = x.pin[(size_t)pinI], old = was[(size_t)pinI];
                if (now != old || (old >= 0 && dirty[(size_t)old])) rewrite = true;
            }
            if (!rewrite) continue;
            auto bits = expandRefs(m, in.conns[k].refs);
            for (size_t j = 0; j < cp.size() && j < bits.size(); j++) {
                int pinI = cp[j];
                if (pinI < 0) continue;
                int now = x.pin[(size_t)pinI], old = was[(size_t)pinI];
                if (now == old && !(old >= 0 && dirty[(size_t)old])) continue;
                if (now >= 0) bits[j] = refOf(now);
                if (now != old) {
                    std::string pn = x.name + "/" + x.ci->pins[(size_t)pinI];
                    if (old >= 0) res.changes.push_back({Change::Disconnect, d.nets[(size_t)old].name, pn});
                    if (now >= 0) res.changes.push_back({Change::Connect, d.nets[(size_t)now].name, pn});
                    res.rewired++;
                }
            }
            in.conns[k].refs = std::move(bits);
        }
        // A pin that had no connection at all (an unused output now read).
        for (size_t pinI = 0; pinI < x.pin.size(); pinI++) {
            if (inConn[pinI] || x.pin[pinI] < 0) continue;
            if (!in.conns.empty() && in.conns[0].port.empty())
                for (size_t k = 0; k < in.conns.size(); k++) in.conns[k].port = x.ci->ports[k].first;
            gnl::Conn* open = nullptr;  // written as .Y(): filled in
            for (auto& c : in.conns)
                if (c.port == x.ci->pins[pinI] && c.refs.empty()) open = &c;
            if (open) {
                open->refs = {refOf(x.pin[pinI])};
            } else {
                gnl::Conn c;
                c.port = x.ci->pins[pinI];
                c.refs = {refOf(x.pin[pinI])};
                in.conns.push_back(std::move(c));
            }
            res.changes.push_back({Change::Connect, d.nets[(size_t)x.pin[pinI]].name, x.name + "/" + x.ci->pins[pinI]});
            res.rewired++;
        }
    }
    // Assigns that named a dirty net are dropped bit by bit.
    std::vector<int> assignMap(m.assigns.size(), -1);
    std::vector<std::pair<std::vector<gnl::Ref>, std::vector<gnl::Ref>>> assigns, extra;
    for (size_t i = 0; i < m.assigns.size(); i++) {
        auto& nets = d.assignNets[i];
        bool hit = false;
        for (int n : nets)
            if (n >= 0 && dirty[(size_t)n]) hit = true;
        if (!hit) {
            assignMap[i] = (int)assigns.size();
            assigns.push_back(m.assigns[i]);
            continue;
        }
        auto l = expandRefs(m, m.assigns[i].first), r = expandRefs(m, m.assigns[i].second);
        for (size_t k = 0; k < nets.size(); k++)
            if (nets[k] < 0 || !dirty[(size_t)nets[k]]) extra.push_back({{l[l.size() - 1 - k]}, {r[r.size() - 1 - k]}});
    }
    // Output ports that are not the name of the net they carry.
    for (auto& p : d.ports) {
        if (!p.out || p.net < 0) continue;
        bool touched = p.added || p.net != p.orig || dirty[(size_t)p.net];
        if (!touched || sameRef(refOf(p.net), p.ref)) continue;
        extra.push_back({{p.ref}, {refOf(p.net)}});
    }
    // Removed cells, then the added ones.
    std::vector<int> instMap(m.insts.size(), -1);
    std::vector<gnl::Inst> insts;
    for (auto& x : d.insts) {
        if (x.src < 0) continue;
        if (x.dead) {
            res.changes.push_back({Change::RemoveCell, x.name, x.ci->name});
            res.removed++;
            res.areaRemoved += x.ci->area;
            continue;
        }
        instMap[(size_t)x.src] = (int)insts.size();
        insts.push_back(std::move(m.insts[(size_t)x.src]));
    }
    std::vector<std::pair<char, int>> body;
    for (auto& [k, i] : m.body) {
        int j = k == 'a' ? assignMap[(size_t)i] : instMap[(size_t)i];
        if (j >= 0) body.push_back({k, j});
    }
    for (auto& x : d.insts) {
        if (x.src >= 0 || x.dead) continue;
        gnl::Inst in;
        in.cell = x.ci->name;
        in.name = x.name;
        res.changes.push_back({Change::AddCell, x.name, x.ci->name});
        res.added++;
        res.areaAdded += x.ci->area;
        for (auto& [port, bits] : x.ci->ports) {
            bool any = false;
            for (int b : bits)
                if (x.pin[(size_t)b] >= 0) any = true;
            if (!any) continue;
            gnl::Conn c;
            c.port = port;
            for (int b : bits) {
                if (x.pin[(size_t)b] >= 0) {
                    c.refs.push_back(refOf(x.pin[(size_t)b]));
                    res.changes.push_back({Change::Connect, d.nets[(size_t)x.pin[(size_t)b]].name, x.name + "/" + x.ci->pins[(size_t)b]});
                } else if (x.ci->isOut[(size_t)b]) {
                    std::string nn = d.newNetName(opt.prefix + "nc");
                    m.decl(nn);
                    c.refs.push_back(netRef(nn));
                } else {
                    c.refs.push_back(constRef(0));
                }
            }
            in.conns.push_back(std::move(c));
        }
        insts.push_back(std::move(in));
    }
    for (auto& e : extra) assigns.push_back(std::move(e));
    m.insts = std::move(insts);
    m.assigns = std::move(assigns);
    m.body = std::move(body);
}

}  // namespace detail

inline Result runCore(const gnl::Module& oldM, const gnl::Module& newM, const Lib& lib, Options opt) {
    Result res;
    if (opt.threads == 0) opt.threads = par::defaultThreads();
    opt.simWords = std::max(1, std::min<int>(opt.simWords, Sig::Max));
    const unsigned T = opt.threads;
    Design O, N;  // read side by side
    par::parallelFor(2, T, [&](size_t i) { (i ? N : O) = build(i ? newM : oldM, lib, i == 0); });
    for (auto& w : O.warnings) res.warnings.push_back("old: " + w);
    for (auto& w : N.warnings) res.warnings.push_back("new: " + w);
    Vars V;
    V.ties = opt.ties;
    Model om, nm;
    om.compute(O, V);
    nm.compute(N, V);
    for (auto& w : om.warnings) res.warnings.push_back("old: " + w);
    for (auto& w : nm.warnings) res.warnings.push_back("new: " + w);
    Prover pv(V.g, opt.simWords, opt.budget);

    // ---- what differs
    std::vector<Point> pts = detail::points(O, om, N, nm);
    pv.sync(T);
    par::parallelFor(pts.size(), T, [&](size_t i) { pts[i].status = pv.prove(pts[i].o, pts[i].n); });
    res.points = pts.size();
    for (auto& pt : pts)
        if (pt.status == 1) pv.learn(pt.o, pt.n);
    for (auto& pt : pts)
        if (pt.status != 1) res.failing.push_back(pt.name + (pt.status < 0 ? " (undecided, treated as different)" : ""));
    // old instance / port -> its points (those of one instance are consecutive)
    struct PtIndex {
        std::vector<int> first, count;
        struct Range {
            int b = 0, e = 0;
            struct It {
                int i;
                int operator*() const { return i; }
                It& operator++() { ++i; return *this; }
                bool operator!=(const It& o) const { return i != o.i; }
            };
            It begin() const { return {b}; }
            It end() const { return {e}; }
            bool empty() const { return b == e; }
        };
        Range of(int k) const {
            if (k < 0 || (size_t)k >= first.size() || first[(size_t)k] < 0) return {};
            return {first[(size_t)k], first[(size_t)k] + count[(size_t)k]};
        }
        void add(int k, int i) {
            if ((size_t)k >= first.size()) first.resize((size_t)k + 1, -1), count.resize((size_t)k + 1, 0);
            if (first[(size_t)k] < 0) first[(size_t)k] = i;
            count[(size_t)k]++;
        }
    } ptsOfInst, ptsOfPort;
    for (size_t i = 0; i < pts.size(); i++) (pts[i].what == Point::Out ? ptsOfPort : ptsOfInst).add(pts[i].oi, (int)i);

    struct Rewire { int oi, pin, nnet; };
    struct Bind { int port, nnet; };  // port: index in O.ports
    std::vector<Rewire> rewires;
    std::vector<Bind> binds;
    std::vector<int> addInsts;                      // N instances to add
    std::vector<std::pair<int, int>> replaceInsts;  // O instance, N instance
    std::vector<int> removeInsts;
    std::set<int> replaced;  // O instances

    for (size_t i = 0; i < N.insts.size(); i++) {
        auto& x = N.insts[i];
        if (x.ci->kind == Kind::Comb) continue;
        auto it = O.instIdx.find(x.name);
        if (it == O.instIdx.end()) {
            if (O.instNameUsed(x.name))
                throw Error("register or macro " + x.name + " of the new netlist has the name of a gate in the old one");
            addInsts.push_back((int)i);
            res.failing.push_back(x.name + " (new)");
            continue;
        }
        auto& y = O.insts[(size_t)it->second];
        bool same = y.ci->kind == x.ci->kind &&
                    (x.ci->kind == Kind::State ? x.ci->latch == y.ci->latch : x.ci == y.ci);
        if (!same) res.failing.push_back(x.name + " (different kind of cell)");
        if (!same || opt.replace.count(x.name)) {
            replaceInsts.push_back({it->second, (int)i});
            replaced.insert(it->second);
        }
    }
    for (size_t i = 0; i < O.insts.size(); i++) {
        auto& y = O.insts[i];
        if (y.ci->kind != Kind::Comb && !N.instIdx.count(y.name)) { removeInsts.push_back((int)i); res.failing.push_back(y.name + " (removed)"); }
    }
    for (size_t i = 0; i < N.ports.size(); i++) {
        auto& p = N.ports[i];
        if (!p.out) continue;
        auto it = O.portIdx.find(p.name);
        if (it != O.portIdx.end()) {
            if (!O.ports[(size_t)it->second].out) throw Error("port " + p.name + " is an output in the new netlist and an input in the old one");
            continue;
        }
        if (p.ref.kind != gnl::Ref::Net) throw Error("output " + p.name + " is a bit of a bus the old netlist does not have");
        Port np;
        np.name = p.name;
        np.ref = p.ref;
        np.out = true;
        np.added = true;
        O.portIdx[np.name] = (int)O.ports.size();
        binds.push_back({(int)O.ports.size(), p.net});
        O.ports.push_back(std::move(np));
        res.failing.push_back(p.name + " (new output)");
    }
    for (auto& p : O.ports)
        if (p.out && !p.added && !N.portIdx.count(p.name))
            res.warnings.push_back("output " + p.name + " is not in the new netlist and is left as it is");

    // ---- the working state of the old netlist
    std::vector<std::vector<int>> origPin(O.insts.size());
    for (size_t i = 0; i < O.insts.size(); i++) origPin[i] = O.insts[i].pin;
    const size_t origNets = O.nets.size();
    std::vector<uint8_t> deadBefore;
    if (opt.sweep) deadBefore = detail::unloaded(O, [&](size_t i) { return O.insts[i].ci->kind == Kind::Comb; });
    const size_t origInsts = O.insts.size();
    std::vector<uint8_t> spareBefore;
    if (opt.spare) spareBefore = detail::unloaded(O, [&](size_t i) { return O.insts[i].ci->kind == Kind::Comb; });

    std::vector<uint8_t> going(O.insts.size(), 0);  // its outputs may not be reused
    for (int i : removeInsts) going[(size_t)i] = 1;
    for (auto& [oi, ni] : replaceInsts) going[(size_t)oi] = 1;
    std::vector<uint8_t> fresh(N.insts.size(), 0);  // N instances that are instantiated as they are
    for (int i : addInsts) fresh[(size_t)i] = 1;

    std::vector<std::vector<std::pair<int, int>>> sinks(O.nets.size());  // net -> (instance, input pin)
    for (size_t i = 0; i < O.insts.size(); i++)
        for (size_t k = 0; k < O.insts[i].pin.size(); k++)
            if (!O.insts[i].ci->isOut[k] && O.insts[i].pin[k] >= 0) sinks[(size_t)O.insts[i].pin[k]].push_back({(int)i, (int)k});
    std::vector<std::vector<int>> nSinks(N.nets.size());  // new-netlist net -> gates reading it
    for (size_t i = 0; i < N.insts.size(); i++)
        if (N.insts[i].ci->kind == Kind::Comb)
            for (size_t k = 0; k < N.insts[i].pin.size(); k++)
                if (!N.insts[i].ci->isOut[k] && N.insts[i].pin[k] >= 0) nSinks[(size_t)N.insts[i].pin[k]].push_back((int)i);

    // Functions the old netlist has: by literal, and by simulation signature.
    // `exact` is indexed by literal; the signatures of the nets known at
    // the start are a sorted table, the ones added later a small map.
    struct LitSet {
        std::vector<uint8_t> has;
        void add(Lit l) { if (l >= has.size()) has.resize((size_t)l + 1024, 0); has[l] = 1; }
        size_t count(Lit l) const { return l < has.size() && has[l]; }
    } nLits;
    nLits.has.assign(V.g.size() * 2, 0);
    for (Lit l : nm.lit) nLits.add(l);
    std::vector<int> exact(V.g.size() * 2, -1);
    std::vector<std::pair<uint64_t, int>> sigTable;
    std::unordered_map<uint64_t, std::vector<int>> bySig;
    auto noteLit = [&](int net) {
        Lit l = om.lit[(size_t)net];
        if (l >= exact.size()) exact.resize((size_t)l + 1024, -1);
        int& e = exact[l];
        if (e >= 0 && e != net && om.lit[(size_t)e] == l) return;
        e = net;
        if (pv.done < V.g.size()) pv.sync(T);
        auto& v = bySig[pv.sigHash(l)];
        if (std::find(v.begin(), v.end(), net) == v.end()) v.push_back(net);
    };
    {
        std::vector<int> keep;
        for (size_t i = 0; i < O.nets.size(); i++) {
            auto& n = O.nets[i];
            if (n.drvInst >= 0 && going[(size_t)n.drvInst]) continue;
            if (n.cval < 0 && n.inPort < 0 && n.drvInst < 0) continue;  // floating
            int& e = exact[om.lit[i]];
            if (e < 0) e = (int)i, keep.push_back((int)i);
        }
        sigTable.resize(keep.size());
        par::parallelFor((keep.size() + 4095) / 4096, T, [&](size_t c) {
            for (size_t k = c * 4096, end = std::min(keep.size(), k + 4096); k < end; k++)
                sigTable[k] = {pv.sigHash(om.lit[(size_t)keep[k]]), keep[k]};
        });
        std::sort(sigTable.begin(), sigTable.end());
    }
    // The old net with the function `l`, or -1. Reads only: safe in parallel.
    auto lookup = [&](Lit l) -> int {
        if (l < exact.size() && exact[l] >= 0 && om.lit[(size_t)exact[l]] == l) return exact[l];
        uint64_t h = pv.sigHash(l);
        int tried = 0, found = -1;
        auto tryNet = [&](int on) {
            Lit ol = om.lit[(size_t)on];
            if (ol == l) { found = on; return true; }
            if (!pv.sigEqual(ol, l)) return false;
            if (tried++ >= 8) return true;
            if (pv.prove(ol, l, opt.netBudget) == 1) { found = on; return true; }
            return false;
        };
        for (auto it = std::lower_bound(sigTable.begin(), sigTable.end(), std::make_pair(h, -1)); it != sigTable.end() && it->first == h; ++it)
            if (tryNet(it->second)) return found;
        auto s = bySig.find(h);
        if (s != bySig.end())
            for (int on : s->second)
                if (tryNet(on)) return found;
        return -1;
    };
    int epoch = 0;  // counts the edits that change functions of old nets
    std::vector<int> eqNet(N.nets.size(), -3), eqEpoch(N.nets.size(), -1);
    std::vector<Lit> eqLit(N.nets.size(), 0);
    auto eqValid = [&](int nn) {
        int e = eqNet[(size_t)nn];
        if (e == -3) return false;
        if (e < 0) return eqEpoch[(size_t)nn] == epoch;
        return om.lit[(size_t)e] == eqLit[(size_t)nn];
    };
    auto eqSet = [&](int nn, int on) {
        eqNet[(size_t)nn] = on;
        eqEpoch[(size_t)nn] = epoch;
        if (on >= 0) {
            eqLit[(size_t)nn] = om.lit[(size_t)on];
            pv.learn(om.lit[(size_t)on], nm.lit[(size_t)nn]);
        }
    };
    auto equivOf = [&](int nn) {
        if (!eqValid(nn)) eqSet(nn, lookup(nm.lit[(size_t)nn]));
        return eqNet[(size_t)nn];
    };
    // The same for the inverse: an old net that is the complement of nn. An
    // inverter on it is then the whole cone of nn.
    const bool useInv = opt.invert && lib.inverter;
    std::vector<int> cpNet(useInv ? N.nets.size() : 0, -3), cpEpoch(useInv ? N.nets.size() : 0, -1);
    std::vector<Lit> cpLit(useInv ? N.nets.size() : 0, 0);
    auto cpValid = [&](int nn) {
        int e = cpNet[(size_t)nn];
        if (e == -3) return false;
        if (e < 0) return cpEpoch[(size_t)nn] == epoch;
        return om.lit[(size_t)e] == cpLit[(size_t)nn];
    };
    auto cpSet = [&](int nn, int on) {
        cpNet[(size_t)nn] = on;
        cpEpoch[(size_t)nn] = epoch;
        if (on >= 0) {
            cpLit[(size_t)nn] = om.lit[(size_t)on];
            pv.learn(om.lit[(size_t)on], nm.lit[(size_t)nn] ^ 1);
        }
    };
    bool invNow = false;  // on for the final copy only: the search for the gate to change needs the plain region
    auto complOf = [&](int nn) {
        if (!useInv || !invNow) return -1;
        if (!cpValid(nn)) cpSet(nn, lookup(nm.lit[(size_t)nn] ^ 1));
        return cpNet[(size_t)nn];
    };

    std::vector<int> memo(N.nets.size(), -1);  // new-netlist net -> net of the patched design
    std::vector<Lit> memoLit(N.nets.size(), 0);
    auto memoGet = [&](int nn) {
        int r = memo[(size_t)nn];
        return r >= 0 && om.lit[(size_t)r] == memoLit[(size_t)nn] ? r : -1;
    };
    auto memoSet = [&](int nn, int r) {
        memo[(size_t)nn] = r;
        memoLit[(size_t)nn] = om.lit[(size_t)r];
    };

    auto addNet = [&](const std::string& base, Lit l) {
        Net n;
        n.name = O.newNetName(base);
        n.added = true;
        O.nets.push_back(std::move(n));
        om.lit.push_back(l);
        sinks.emplace_back();
        return (int)O.nets.size() - 1;
    };
    auto constNet = [&](int v) {
        for (size_t i = 0; i < O.nets.size(); i++)
            if (O.nets[i].cval == v) return (int)i;
        Net n;
        n.cval = v;
        n.ref = constRef(v);
        n.name = v ? "1'b1" : "1'b0";
        O.nets.push_back(std::move(n));
        om.lit.push_back(v ? kTrue : kFalse);
        sinks.emplace_back();
        return (int)O.nets.size() - 1;
    };
    auto addInst = [&](const std::string& name, const CellInfo* ci) {
        Inst x;
        x.name = name;
        x.ci = ci;
        x.pin.assign(ci->pins.size(), -1);
        O.insts.push_back(std::move(x));
        going.push_back(0);
        return (int)O.insts.size() - 1;
    };
    auto drive = [&](int inst, int pin, int net) {
        O.insts[(size_t)inst].pin[(size_t)pin] = net;
        O.nets[(size_t)net].drvInst = inst;
        O.nets[(size_t)net].drvPin = pin;
    };
    auto setIn = [&](int inst, int pin, int net) {
        int& cur = O.insts[(size_t)inst].pin[(size_t)pin];
        if (cur == net) return;
        if (cur >= 0) {
            auto& v = sinks[(size_t)cur];
            v.erase(std::remove(v.begin(), v.end(), std::make_pair(inst, pin)), v.end());
        }
        cur = net;
        if (net >= 0) sinks[(size_t)net].push_back({inst, pin});
    };
    auto inverterTo = [&](int from, int to, const std::string& why) {
        if (!lib.inverter) throw Error(why + " needs an inverter, and the library has none that may be used");
        int inv = addInst(O.newInstName(opt.prefix + "inv"), lib.inverter);
        setIn(inv, lib.inverter->invIn, from);
        drive(inv, lib.inverter->invOut, to);
    };

    // A register or macro that is swapped for the new cell, or added, keeps
    // its name, so that the next comparison matches it.
    std::vector<std::pair<int, int>> pendingInputs;  // (patched instance, N instance): inputs to connect
    auto doReplace = [&](int oi, int ni) {
        Inst& x = N.insts[(size_t)ni];
        O.insts[(size_t)oi].dead = true;
        going[(size_t)oi] = 1;
        fresh[(size_t)ni] = 1;
        int ki = addInst(x.name, x.ci);
        O.instIdx[x.name] = ki;
        std::vector<std::pair<int, Lit>> oldOuts;  // old output nets and what they carry
        {
            const Inst& y = O.insts[(size_t)oi];
            for (size_t k = 0; k < y.pin.size(); k++)
                if (y.ci->isOut[k] && y.pin[k] >= 0 && O.nets[(size_t)y.pin[k]].drvInst == oi)
                    oldOuts.push_back({y.pin[k], om.lit[(size_t)y.pin[k]]});
        }
        std::vector<Lit> pl(x.pin.size(), 0);
        for (size_t k = 0; k < x.pin.size(); k++) {
            if (!x.ci->isOut[k]) continue;
            pl[k] = nm.outLit(x, (int)k);
            for (auto& [on, l] : oldOuts)
                if (on >= 0 && l == pl[k]) {
                    drive(ki, (int)k, on);
                    if (x.pin[k] >= 0) memoSet(x.pin[k], on);
                    noteLit(on);
                    on = -1;
                    break;
                }
            if (O.insts[(size_t)ki].pin[k] < 0 && x.pin[k] >= 0) {
                auto& nn = N.nets[(size_t)x.pin[k]];
                int net = addNet(O.netNameUsed(nn.name) ? opt.prefix + nn.name : nn.name, pl[k]);
                drive(ki, (int)k, net);
                memoSet(x.pin[k], net);
                noteLit(net);
            }
        }
        for (auto& [on, l] : oldOuts) {
            if (on < 0) continue;
            bool done = false;
            for (size_t k = 0; k < x.pin.size() && !done; k++) {
                if (!x.ci->isOut[k] || pl[k] != (l ^ 1)) continue;
                if (O.insts[(size_t)ki].pin[k] < 0) {
                    int net = addNet(opt.prefix + x.name + "_" + x.ci->pins[k], pl[k]);
                    drive(ki, (int)k, net);
                    noteLit(net);
                }
                inverterTo(O.insts[(size_t)ki].pin[k], on, "replacing " + x.name);
                noteLit(on);
                done = true;
            }
            if (!done) {
                O.nets[(size_t)on].drvInst = -1;
                res.warnings.push_back("net " + O.nets[(size_t)on].name + " loses its driver: the new cell of " + x.name + " has no such output");
            }
        }
        pendingInputs.push_back({ki, ni});
    };
    for (auto& [oi, ni] : replaceInsts) doReplace(oi, ni);
    for (int ni : addInsts) {
        Inst& x = N.insts[(size_t)ni];
        int ki = addInst(x.name, x.ci);
        O.instIdx[x.name] = ki;
        O.instNames.insert(x.name);
        for (size_t k = 0; k < x.pin.size(); k++) {
            if (!x.ci->isOut[k] || x.pin[k] < 0) continue;
            auto& nn = N.nets[(size_t)x.pin[k]];
            int net = addNet(O.netNameUsed(nn.name) ? opt.prefix + nn.name : nn.name, nm.lit[(size_t)x.pin[k]]);
            drive(ki, (int)k, net);
            memoSet(x.pin[k], net);
            noteLit(net);
        }
        pendingInputs.push_back({ki, ni});
    }

    // Finds, in parallel, which of the new-netlist nets under `roots` exist
    // in the old netlist; walks into the gates of those that do not.
    std::vector<int> seenStamp(N.nets.size(), 0);
    int stampNow = 0;
    auto prefill = [&](const std::vector<int>& roots) {
        stampNow++;
        pv.sync(T);
        std::vector<int> wave;
        auto want = [&](int nn) {
            if (nn < 0 || seenStamp[(size_t)nn] == stampNow) return;
            seenStamp[(size_t)nn] = stampNow;
            auto& n = N.nets[(size_t)nn];
            if (n.cval >= 0 || memoGet(nn) >= 0) return;
            if (n.drvInst >= 0 && fresh[(size_t)n.drvInst]) return;
            wave.push_back(nn);
        };
        for (int r : roots) want(r);
        while (!wave.empty()) {
            std::vector<int> cur;
            cur.swap(wave);
            std::vector<int> found(cur.size(), -2);
            par::parallelFor(cur.size(), T, [&](size_t i) {
                if (!eqValid(cur[i])) found[i] = lookup(nm.lit[(size_t)cur[i]]);
            });
            for (size_t i = 0; i < cur.size(); i++)
                if (found[i] != -2) eqSet(cur[i], found[i]);
            if (useInv && invNow) {
                std::vector<int> foundC(cur.size(), -2);
                par::parallelFor(cur.size(), T, [&](size_t i) {
                    int nn = cur[i];
                    if (eqNet[(size_t)nn] >= 0 || cpValid(nn)) return;
                    auto& n = N.nets[(size_t)nn];
                    if (n.drvInst < 0 || N.insts[(size_t)n.drvInst].ci->kind != Kind::Comb) return;
                    foundC[i] = lookup(nm.lit[(size_t)nn] ^ 1);
                });
                for (size_t i = 0; i < cur.size(); i++)
                    if (foundC[i] != -2) cpSet(cur[i], foundC[i]);
            }
            for (size_t i = 0; i < cur.size(); i++) {
                if (eqNet[(size_t)cur[i]] >= 0) continue;
                auto& n = N.nets[(size_t)cur[i]];
                if (n.drvInst < 0 || N.insts[(size_t)n.drvInst].ci->kind != Kind::Comb) continue;
                if (useInv && invNow && cpValid(cur[i]) && cpNet[(size_t)cur[i]] >= 0) continue;
                auto& x = N.insts[(size_t)n.drvInst];
                for (size_t k = 0; k < x.pin.size(); k++)
                    if (!x.ci->isOut[k]) want(x.pin[k]);
            }
        }
    };

    // The net of the patched design that carries new-netlist net nn: an old
    // net with that function, or a copy of the gate that drives nn.
    std::function<int(int)> mapNet = [&](int nn) -> int {
        if (nn == -2) return constNet(0);
        if (nn < 0) return -1;
        if (int m = memoGet(nn); m >= 0) return m;
        const Net& n = N.nets[(size_t)nn];
        int r = -1;
        bool isFresh = n.drvInst >= 0 && fresh[(size_t)n.drvInst];
        if (n.cval >= 0) r = constNet(n.cval);
        else if (!isFresh && equivOf(nn) >= 0) { r = eqNet[(size_t)nn]; res.reused++; }
        else if (n.inPort >= 0) {
            // An input the old netlist does not have.
            const Port& p = N.ports[(size_t)n.inPort];
            if (O.portIdx.count(p.name)) throw Error("input " + p.name + " is not usable in the old netlist");
            if (p.ref.kind != gnl::Ref::Net) throw Error("input " + p.name + " is a bit of a bus the old netlist does not have");
            if (O.netNameUsed(p.name)) throw Error("new input " + p.name + " has the name of a net in the old netlist");
            Port np;
            np.name = p.name;
            np.ref = p.ref;
            np.added = true;
            r = addNet(p.name, nm.lit[(size_t)nn]);
            O.nets[(size_t)r].ref = p.ref;
            O.nets[(size_t)r].inPort = (int)O.ports.size();
            np.net = np.orig = r;
            O.portIdx[np.name] = (int)O.ports.size();
            O.ports.push_back(std::move(np));
            noteLit(r);
        } else if (n.drvInst < 0) {
            res.warnings.push_back("new netlist net " + n.name + " has no driver; the patch ties it to 0");
            r = constNet(0);
        } else if (isFresh) {
            throw Error("internal: output " + n.name + " of an added cell has no net");
        } else if (N.insts[(size_t)n.drvInst].ci->kind == Kind::Comb) {
            // The old netlist has the inverse: one inverter is the cone.
            if (int c = complOf(nn); c >= 0) {
                r = addNet(opt.prefix + n.name, nm.lit[(size_t)nn]);
                inverterTo(c, r, "reading " + n.name);
                memoSet(nn, r);
                noteLit(r);
                res.reused++;
                return r;
            }
            // A gate of the patch: copy it, with its inputs mapped the same way.
            const Inst& x = N.insts[(size_t)n.drvInst];
            int ki = addInst(O.newInstName(opt.prefix + x.name), x.ci);
            for (size_t k = 0; k < x.pin.size(); k++) {
                if (!x.ci->isOut[k] || x.pin[k] < 0) continue;
                if (N.nets[(size_t)x.pin[k]].drvInst != n.drvInst) continue;
                if (x.pin[k] != nn && (memoGet(x.pin[k]) >= 0 || equivOf(x.pin[k]) >= 0)) continue;
                int net = addNet(opt.prefix + N.nets[(size_t)x.pin[k]].name, nm.lit[(size_t)x.pin[k]]);
                drive(ki, (int)k, net);
                memoSet(x.pin[k], net);
                noteLit(net);
            }
            r = memoGet(nn);
            for (size_t k = 0; k < x.pin.size(); k++)
                if (!x.ci->isOut[k]) { int in = mapNet(x.pin[k]); setIn(ki, (int)k, in); }
            return r;
        } else {
            // An output of a register or macro both netlists have, on a pin
            // the old netlist left open (or only has the complement of).
            const Inst& x = N.insts[(size_t)n.drvInst];
            int oi = O.instIdx.at(x.name);
            Lit l = nm.lit[(size_t)nn];
            int inv = -1;
            for (size_t k = 0; k < O.insts[(size_t)oi].pin.size() && r < 0; k++) {
                const Inst& y = O.insts[(size_t)oi];
                if (!y.ci->isOut[k]) continue;
                Lit yl = om.outLit(y, (int)k);
                if (yl == l) {
                    if (y.pin[k] < 0) {
                        int net = addNet(opt.prefix + x.name + "_" + y.ci->pins[k], yl);
                        drive(oi, (int)k, net);
                        noteLit(net);
                    }
                    r = O.insts[(size_t)oi].pin[k];
                } else if (yl == (l ^ 1) && inv < 0) inv = (int)k;
            }
            if (r < 0 && inv >= 0) {
                const CellInfo* yc = O.insts[(size_t)oi].ci;
                if (O.insts[(size_t)oi].pin[(size_t)inv] < 0) {
                    int net = addNet(opt.prefix + x.name + "_" + yc->pins[(size_t)inv], l ^ 1);
                    drive(oi, inv, net);
                    noteLit(net);
                }
                r = addNet(opt.prefix + n.name, l);
                inverterTo(O.insts[(size_t)oi].pin[(size_t)inv], r, "reading " + n.name);
                noteLit(r);
            }
            if (r < 0) throw Error("the old cell of " + x.name + " has no pin for " + n.name);
        }
        memoSet(nn, r);
        return r;
    };

    // ---- rectification: fix a change where it is, not at the points it reaches
    //
    // The cone copied for a failing point ends at the old nets the new
    // netlist still has, so a changed gate deep in the logic would drag
    // everything between it and the compare points into the patch. Before
    // that, each new-netlist net without an old equivalent is tried as the
    // replacement of an old net (all its loads) or of one old gate input,
    // near the nets its own cone reads. A replacement is taken when, on the
    // simulation patterns, it turns failing points into passing ones or
    // makes the logic after it meet the new netlist again, and breaks no
    // point that passes. Whatever it gets wrong is still caught: the points
    // are proved again afterwards and the remaining ones get their cone.
    auto failRoots = [&]() {
        std::vector<int> roots;
        for (auto& pt : pts) {
            if (pt.status == 1) continue;
            if (pt.what != Point::Out && replaced.count(pt.oi)) continue;
            const Inst* x = pt.what == Point::Out ? nullptr : &N.insts[(size_t)pt.ni];
            if (pt.what == Point::Out) roots.push_back(N.ports[(size_t)pt.ni].net);
            else if (pt.what == Point::BoxPin) roots.push_back(x->pin[(size_t)pt.pin]);
            else
                for (size_t k = 0; k < x->pin.size(); k++)
                    if (!x->ci->isOut[k] && detail::compProg(*x->ci, pt.what).uses((int)k)) roots.push_back(x->pin[k]);
        }
        for (auto& b : binds) roots.push_back(b.nnet);
        for (size_t i = 0; i < N.insts.size(); i++)
            if (fresh[i])
                for (size_t k = 0; k < N.insts[i].pin.size(); k++)
                    if (!N.insts[i].ci->isOut[k]) roots.push_back(N.insts[i].pin[k]);
        return roots;
    };
    std::unordered_map<int, std::vector<int>> portsOfNet;
    for (size_t i = 0; i < O.ports.size(); i++)
        if (O.ports[i].out && O.ports[i].net >= 0) portsOfNet[O.ports[i].net].push_back((int)i);

    if (opt.rectify) {
        std::vector<int> roots = failRoots();
        prefill(roots);
        // The unmatched region, inputs before outputs.
        std::vector<int> region;
        {
            std::vector<uint8_t> state(N.nets.size(), 0);
            std::vector<std::pair<int, int>> stack;
            auto open = [&](int nn) {
                if (nn < 0 || state[(size_t)nn]) return false;
                auto& n = N.nets[(size_t)nn];
                if (n.cval >= 0 || memoGet(nn) >= 0 || n.drvInst < 0 || fresh[(size_t)n.drvInst] ||
                    N.insts[(size_t)n.drvInst].ci->kind != Kind::Comb || equivOf(nn) >= 0) {
                    state[(size_t)nn] = 2;
                    return false;
                }
                state[(size_t)nn] = 1;
                stack.push_back({nn, 0});
                return true;
            };
            for (int r : roots) {
                open(r);
                while (!stack.empty()) {
                    int nn = stack.back().first;
                    auto& x = N.insts[(size_t)N.nets[(size_t)nn].drvInst];
                    bool went = false;
                    for (int& k = stack.back().second; k < (int)x.pin.size() && !went;) {
                        int kk = k++;
                        if (!x.ci->isOut[(size_t)kk]) went = open(x.pin[(size_t)kk]);
                    }
                    if (went) continue;
                    region.push_back(nn);
                    stack.pop_back();
                }
            }
        }
        std::vector<int> mark(O.nets.size(), 0), imark(O.insts.size(), 0);
        int markNow = 0;
        long work = 0;
        auto sigNet = [&](int net, const std::unordered_map<int, Sig>& sub) {
            if (net < 0) return Sig();
            auto it = sub.find(net);
            return it != sub.end() ? it->second : pv.sigOf(om.lit[(size_t)net]);
        };
        // The gates after `start` in evaluation order, the registers and
        // macros that read them, and (in mark) the nets on the way.
        auto fanout = [&](const std::vector<int>& start, std::vector<int>& gates, std::vector<int>& readers) {
            markNow++;
            mark.resize(O.nets.size(), 0);
            imark.resize(O.insts.size(), 0);
            std::vector<std::pair<int, int>> stack;  // gate, next output pin
            std::vector<int> post;
            auto visitNet = [&](int net) {
                if (mark[(size_t)net] == markNow) return;
                mark[(size_t)net] = markNow;
                for (auto& [inst, pin] : sinks[(size_t)net]) {
                    (void)pin;
                    if (O.insts[(size_t)inst].dead || imark[(size_t)inst] == markNow) continue;
                    imark[(size_t)inst] = markNow;
                    if (O.insts[(size_t)inst].ci->kind == Kind::Comb) stack.push_back({inst, 0});
                    else readers.push_back(inst);
                }
            };
            for (int s : start) {
                visitNet(s);
                while (!stack.empty()) {
                    int inst = stack.back().first;
                    auto& x = O.insts[(size_t)inst];
                    size_t depth = stack.size();
                    for (int& k = stack.back().second; k < (int)x.pin.size() && stack.size() == depth;) {
                        int kk = k++;
                        if (x.ci->isOut[(size_t)kk] && x.pin[(size_t)kk] >= 0 && O.nets[(size_t)x.pin[(size_t)kk]].drvInst == inst)
                            visitNet(x.pin[(size_t)kk]);
                    }
                    if (stack.size() != depth) continue;
                    post.push_back(inst);
                    stack.pop_back();
                }
            }
            gates.assign(post.rbegin(), post.rend());
        };
        // net >= 0: every load of that old net reads `val` instead; else
        // input `pin` of gate `inst` does. val is a new-netlist net.
        struct Cand { int net, inst, pin, val; int score = 0; };
        std::unordered_map<std::string, int> oNetByName;
        for (size_t i = 0; i < origNets; i++) oNetByName.emplace(O.nets[i].name, (int)i);
        // The old net a new-netlist net is (without building anything), -1: none yet.
        auto image = [&](int v) -> int {
            if (v < 0) return -1;
            auto& n = N.nets[(size_t)v];
            if (n.cval >= 0) return constNet(n.cval);
            if (int m = memoGet(v); m >= 0) return m;
            if (n.drvInst >= 0 && fresh[(size_t)n.drvInst]) return -1;
            return equivOf(v);
        };

        std::vector<uint8_t> inRegion(N.nets.size(), 0), isRoot(N.nets.size(), 0);
        for (int nn : region) inRegion[(size_t)nn] = 1;
        for (int r : roots)
            if (r >= 0) isRoot[(size_t)r] = 1;
        for (int nn : region) {
            if (work > opt.rectifyWork) break;
            if (memoGet(nn) >= 0 || equivOf(nn) >= 0) continue;
            {
                // Still wanted? Not when everything that reads it has since
                // been matched (the logic after an earlier fix).
                bool wanted = isRoot[(size_t)nn];
                for (size_t gi = 0; gi < nSinks[(size_t)nn].size() && !wanted; gi++) {
                    auto& x = N.insts[(size_t)nSinks[(size_t)nn][gi]];
                    for (size_t k = 0; k < x.pin.size() && !wanted; k++)
                        if (x.ci->isOut[k] && x.pin[k] >= 0 && inRegion[(size_t)x.pin[k]] && image(x.pin[k]) < 0) wanted = true;
                }
                if (!wanted) continue;
            }
            const Inst& h = N.insts[(size_t)N.nets[(size_t)nn].drvInst];
            // What a copy of nn's cone would read.
            std::vector<int> front;
            {
                std::vector<int> stack{nn};
                std::unordered_set<int> seen{nn};
                size_t gates = 0;
                bool tooBig = false;
                while (!stack.empty() && !tooBig) {
                    int c = stack.back();
                    stack.pop_back();
                    auto& x = N.insts[(size_t)N.nets[(size_t)c].drvInst];
                    if (++gates > 64) tooBig = true;
                    for (size_t k = 0; k < x.pin.size(); k++) {
                        int in = x.pin[k];
                        if (x.ci->isOut[k] || in < 0 || !seen.insert(in).second) continue;
                        auto& n = N.nets[(size_t)in];
                        if (n.cval >= 0) continue;
                        if (int e = image(in); e >= 0) { front.push_back(e); continue; }
                        if (n.drvInst >= 0 && !fresh[(size_t)n.drvInst] && N.insts[(size_t)n.drvInst].ci->kind == Kind::Comb) stack.push_back(in);
                    }
                }
                if (tooBig) continue;
                std::sort(front.begin(), front.end());
                front.erase(std::unique(front.begin(), front.end()), front.end());
            }
            // Candidates, next to the nets the cone reads: old-only nets and
            // gate inputs that would take nn; and, on an old gate of the
            // same cell as nn's, the inputs that differ from the new gate's.
            std::vector<Cand> cands;
            {
                std::set<std::pair<int, int>> seenNet;
                auto oldOnly = [&](int o) {
                    if (o < 0 || O.nets[(size_t)o].cval >= 0) return false;
                    int drv = O.nets[(size_t)o].drvInst;
                    if (drv < 0 || O.insts[(size_t)drv].dead || O.insts[(size_t)drv].ci->kind != Kind::Comb) return false;
                    return !nLits.count(om.lit[(size_t)o]);  // the new netlist does not have it
                };
                auto netCand = [&](int o, int val) {
                    if (oldOnly(o) && seenNet.insert({o, val}).second) cands.push_back({o, -1, -1, val});
                };
                auto byName = oNetByName.find(N.nets[(size_t)nn].name);
                if (byName != oNetByName.end()) netCand(byName->second, nn);
                for (int f : front) {
                    for (auto [inst, pin] : sinks[(size_t)f]) {
                        auto& x = O.insts[(size_t)inst];
                        if (x.dead || x.ci->kind != Kind::Comb) continue;
                        if (x.ci == h.ci) {
                            for (size_t k = 0; k < x.pin.size(); k++) {
                                if (x.ci->isOut[k] || h.pin[k] < 0) continue;
                                auto& vn = N.nets[(size_t)h.pin[k]];
                                int e = image(h.pin[k]);
                                bool newInput = e < 0 && vn.inPort >= 0;
                                if ((e < 0 && !newInput) || e == x.pin[k]) continue;
                                cands.push_back({-1, inst, (int)k, h.pin[k]});
                                netCand(x.pin[k], h.pin[k]);
                            }
                        }
                        cands.push_back({-1, inst, pin, nn});
                        for (size_t k = 0; k < x.pin.size(); k++) {
                            if (!x.ci->isOut[k] || x.pin[k] < 0) continue;
                            netCand(x.pin[k], nn);
                            for (auto [i2, p2] : sinks[(size_t)x.pin[k]]) {
                                (void)p2;
                                auto& x2 = O.insts[(size_t)i2];
                                if (x2.dead || x2.ci->kind != Kind::Comb) continue;
                                for (size_t k2 = 0; k2 < x2.pin.size(); k2++)
                                    if (x2.ci->isOut[k2]) netCand(x2.pin[k2], nn);
                            }
                        }
                        if (cands.size() >= 64) break;
                    }
                    if (cands.size() >= 64) break;
                }
            }
            if (cands.empty()) continue;
            // First on the simulation patterns, over everything after the
            // candidate: it may break no passing point.
            std::vector<Cand> ok;
            for (auto& c : cands) {
                std::vector<int> start, gates, readers;
                if (c.net >= 0) start.push_back(c.net);
                else {
                    auto& x = O.insts[(size_t)c.inst];
                    for (size_t k = 0; k < x.pin.size(); k++)
                        if (x.ci->isOut[k] && x.pin[k] >= 0) start.push_back(x.pin[k]);
                }
                fanout(start, gates, readers);
                work += (long)gates.size() + 1;
                bool loop = false;
                if (c.val == nn) {
                    for (int f : front)
                        if (mark[(size_t)f] == markNow) loop = true;
                } else if (int e = image(c.val); e >= 0 && (size_t)e < mark.size() && mark[(size_t)e] == markNow) loop = true;
                if (c.net < 0 && imark[(size_t)c.inst] == markNow) loop = true;
                if (loop) continue;
                const Sig want = pv.sigOf(nm.lit[(size_t)c.val]);
                std::unordered_map<int, Sig> sub;
                auto evalGate = [&](int inst, int overPin) {
                    auto& x = O.insts[(size_t)inst];
                    for (size_t k = 0; k < x.pin.size(); k++) {
                        if (!x.ci->isOut[k] || x.pin[k] < 0 || O.nets[(size_t)x.pin[k]].drvInst != inst) continue;
                        sub[x.pin[k]] = evalProgSig(x.ci->fn[k], [&](int p) { return p == overPin ? want : sigNet(x.pin[(size_t)p], sub); }, Sig(), pv.words);
                    }
                };
                if (c.net >= 0) sub[c.net] = want;
                else evalGate(c.inst, c.pin);
                for (int g : gates) evalGate(g, -1);
                int gain = 0, broke = 0;
                auto judge = [&](Point& pt, const Sig& s) {
                    bool now = pv.sigIs(s, pt.n);
                    if (pt.status == 1 && !now) broke++;
                    if (pt.status == 0 && now) gain++;  // undecided points count neither way
                };
                for (int r : readers) {
                    auto pi = ptsOfInst.of(r);
                    if (pi.empty()) continue;
                    auto& y = O.insts[(size_t)r];
                    for (int pidx : pi) {
                        Point& pt = pts[(size_t)pidx];
                        if (pt.what == Point::BoxPin) judge(pt, sigNet(y.pin[(size_t)pt.pin], sub));
                        else
                            judge(pt, evalProgSig(detail::compProg(*y.ci, pt.what), [&](int p) { return sigNet(y.pin[(size_t)p], sub); },
                                                  pv.sigOf(om.stateLit(y)), pv.words));
                    }
                }
                for (auto& [net, s] : sub) {
                    auto po = portsOfNet.find(net);
                    if (po == portsOfNet.end()) continue;
                    for (int p : po->second) {
                        auto pp = ptsOfPort.of(p);
                        for (int pidx : pp) judge(pts[(size_t)pidx], s);
                    }
                }
                if (broke) continue;
                c.score = gain;
                ok.push_back(c);
            }
            std::stable_sort(ok.begin(), ok.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
            if (ok.size() > 8) ok.resize(8);
            // Then by proof, one level after the candidate. Simulation alone
            // is not trusted here: a difference may need a rare pattern.
            // Every gate that would read the new value must then compute
            // something the new netlist has (a net after `val`), and every
            // compare point that would read it directly must pass. For a
            // net that means all of its loads, so a net with one load that
            // must keep the old value is left to the single-input candidates.
            int best = -1;
            for (size_t ci = 0; ci < ok.size() && best < 0; ci++) {
                auto& c = ok[ci];
                Lit vl = nm.lit[(size_t)c.val];
                std::vector<Lit> after;
                for (int g : nSinks[(size_t)c.val]) {
                    auto& x = N.insts[(size_t)g];
                    for (size_t k = 0; k < x.pin.size(); k++)
                        if (x.ci->isOut[k] && x.pin[k] >= 0) after.push_back(nm.lit[(size_t)x.pin[k]]);
                }
                std::vector<std::pair<int, int>> loads;
                if (c.net >= 0) {
                    for (auto& l : sinks[(size_t)c.net])
                        if (!O.insts[(size_t)l.first].dead) loads.push_back(l);
                } else loads.push_back({c.inst, c.pin});
                if (loads.size() > 32) continue;
                // One entry per thing that must hold: the literal pairs of
                // which one must be equal (empty second list: already holds).
                struct Need { bool held = false; std::vector<std::pair<Lit, Lit>> any; };
                std::vector<Need> needs;
                bool fail = false;
                size_t useful = 0;
                for (auto [inst, pin] : loads) {
                    auto& x = O.insts[(size_t)inst];
                    if (x.ci->kind == Kind::Comb) {
                        for (size_t k = 0; k < x.pin.size(); k++) {
                            if (!x.ci->isOut[k] || x.pin[k] < 0) continue;
                            Lit l = evalProg(V.g, x.ci->fn[k], [&](int p) { return p == pin ? vl : om.pinLit(x, p); }, kFalse);
                            Need nd;
                            nd.held = nLits.count(l) > 0;
                            if (!nd.held)
                                for (Lit a : after) nd.any.push_back({l, a});
                            if (!nd.held && nd.any.empty()) fail = true;
                            needs.push_back(std::move(nd));
                            useful++;
                        }
                    } else {
                        auto pi = ptsOfInst.of(inst);
                        if (pi.empty()) continue;  // a register that is going away
                        for (int pidx : pi) {
                            Point& pt = pts[(size_t)pidx];
                            if (pt.what == Point::BoxPin && pt.pin != pin) continue;
                            if (pt.what != Point::BoxPin && !detail::compProg(*x.ci, pt.what).uses(pin)) continue;
                            Lit l = pt.what == Point::BoxPin ? vl : om.comp(x, detail::compProg(*x.ci, pt.what), pin, vl);
                            Need nd;
                            nd.held = l == pt.n;
                            if (!nd.held) nd.any.push_back({l, pt.n});
                            needs.push_back(std::move(nd));
                            if (pt.status != 1) useful++;
                        }
                    }
                }
                if (c.net >= 0) {
                    auto po = portsOfNet.find(c.net);
                    if (po != portsOfNet.end())
                        for (int p : po->second) {
                            auto pp = ptsOfPort.of(p);
                            if (pp.empty()) { fail = true; continue; }  // an output only the old netlist has
                            for (int pidx : pp) {
                                Need nd;
                                nd.held = pts[(size_t)pidx].n == vl;
                                if (!nd.held) nd.any.push_back({vl, pts[(size_t)pidx].n});
                                needs.push_back(std::move(nd));
                                if (pts[(size_t)pidx].status != 1) useful++;
                            }
                        }
                }
                if (fail || !useful) continue;
                std::vector<std::pair<Lit, Lit>> goals;
                std::vector<size_t> goalOf;
                pv.sync(T);
                for (size_t i = 0; i < needs.size() && !fail; i++) {
                    if (needs[i].held) continue;
                    size_t before = goals.size();
                    for (auto& g : needs[i].any)
                        if (pv.sigEqual(g.first, g.second)) goals.push_back(g), goalOf.push_back(i);
                    if (goals.size() == before) fail = true;
                }
                if (fail || goals.size() > 256) continue;
                std::vector<int> r(goals.size(), 0);
                par::parallelFor(goals.size(), T, [&](size_t i) { r[i] = pv.prove(goals[i].first, goals[i].second, opt.netBudget); });
                for (size_t i = 0; i < goals.size(); i++)
                    if (r[i] == 1) needs[goalOf[i]].held = true, pv.learn(goals[i].first, goals[i].second);
                for (auto& nd : needs)
                    if (!nd.held) fail = true;
                if (!fail) best = (int)ci;
            }
            if (best < 0) continue;
            // Take it: build what `val` needs and move the loads.
            Cand c = ok[(size_t)best];
            int repl = mapNet(c.val);
            if (c.net >= 0) {
                auto moved = sinks[(size_t)c.net];
                for (auto [inst, pin] : moved)
                    if (!O.insts[(size_t)inst].dead) setIn(inst, pin, repl);
                auto po = portsOfNet.find(c.net);
                if (po != portsOfNet.end()) {
                    auto ports = po->second;
                    portsOfNet.erase(c.net);
                    for (int p : ports) {
                        O.ports[(size_t)p].net = repl;
                        portsOfNet[repl].push_back(p);
                    }
                }
            } else {
                setIn(c.inst, c.pin, repl);
            }
            // The functions after it, for real this time.
            std::vector<int> gates, readers, changed;
            if (c.net >= 0) {
                fanout({repl}, gates, readers);
            } else {
                std::vector<int> start;
                auto& x = O.insts[(size_t)c.inst];
                for (size_t k = 0; k < x.pin.size(); k++)
                    if (x.ci->isOut[k] && x.pin[k] >= 0) start.push_back(x.pin[k]);
                fanout(start, gates, readers);
                gates.insert(gates.begin(), c.inst);
            }
            for (int g : gates) {
                auto& x = O.insts[(size_t)g];
                for (size_t k = 0; k < x.pin.size(); k++) {
                    if (!x.ci->isOut[k] || x.pin[k] < 0 || O.nets[(size_t)x.pin[k]].drvInst != g) continue;
                    Lit l = om.outLit(x, (int)k);
                    if (l != om.lit[(size_t)x.pin[k]]) {
                        om.lit[(size_t)x.pin[k]] = l;
                        changed.push_back(x.pin[k]);
                    }
                }
            }
            pv.sync(T);
            for (int net : changed) noteLit(net);
            std::vector<int> redo;
            if (c.net >= 0) {
                // registers and ports that read the replaced net directly
                for (auto [inst, pin] : sinks[(size_t)repl]) {
                    (void)pin;
                    if (!O.insts[(size_t)inst].dead && O.insts[(size_t)inst].ci->kind != Kind::Comb) readers.push_back(inst);
                }
                changed.push_back(repl);
            }
            std::sort(readers.begin(), readers.end());
            readers.erase(std::unique(readers.begin(), readers.end()), readers.end());
            for (int r : readers) {
                auto pi = ptsOfInst.of(r);
                if (pi.empty()) continue;
                auto& y = O.insts[(size_t)r];
                for (int pidx : pi) {
                    Point& pt = pts[(size_t)pidx];
                    pt.o = pt.what == Point::BoxPin ? om.pinLit(y, pt.pin) : om.comp(y, detail::compProg(*y.ci, pt.what));
                    redo.push_back(pidx);
                }
            }
            for (int net : changed) {
                auto po = portsOfNet.find(net);
                if (po == portsOfNet.end()) continue;
                for (int p : po->second) {
                    auto pp = ptsOfPort.of(p);
                    if (pp.empty()) continue;
                    for (int pidx : pp) {
                        pts[(size_t)pidx].o = om.lit[(size_t)net];
                        redo.push_back(pidx);
                    }
                }
            }
            pv.sync(T);
            par::parallelFor(redo.size(), T, [&](size_t i) { pts[(size_t)redo[i]].status = pv.prove(pts[(size_t)redo[i]].o, pts[(size_t)redo[i]].n, opt.netBudget); });
            for (int pidx : redo)
                if (pts[(size_t)pidx].status == 1) pv.learn(pts[(size_t)pidx].o, pts[(size_t)pidx].n);
            epoch++;
        }
    }

    // ---- the points that still fail get the cone of the new netlist
    {
        // Points the search left undecided on its small budget get the full one.
        std::vector<int> redo;
        for (size_t i = 0; i < pts.size(); i++)
            if (pts[i].status < 0) redo.push_back((int)i);
        pv.sync(T);
        par::parallelFor(redo.size(), T, [&](size_t i) { pts[(size_t)redo[i]].status = pv.prove(pts[(size_t)redo[i]].o, pts[(size_t)redo[i]].n); });
        for (int pidx : redo)
            if (pts[(size_t)pidx].status == 1) pv.learn(pts[(size_t)pidx].o, pts[(size_t)pidx].n);
    }
    // A state function is fixed at the pin that carries it; when no pin of
    // the old cell can, the register is swapped for the new one.
    std::map<std::pair<int, int>, int> pinPlan;  // (O inst, pin) -> N net
    std::vector<std::pair<int, int>> late;
    for (auto& pt : pts) {
        if (pt.status == 1) continue;
        if (pt.what != Point::Out && replaced.count(pt.oi)) continue;
        if (pt.what == Point::Out) {
            binds.push_back({pt.oi, N.ports[(size_t)pt.ni].net});
        } else if (pt.what == Point::BoxPin) {
            int nn = N.insts[(size_t)pt.ni].pin[(size_t)pt.pin];
            rewires.push_back({pt.oi, pt.pin, nn < 0 ? -2 : nn});  // open in the new netlist: constant 0
        } else {
            const Inst& y = O.insts[(size_t)pt.oi];
            const Inst& x = N.insts[(size_t)pt.ni];
            const Prog& prog = detail::compProg(*y.ci, pt.what);
            struct Try { int p, q; Lit l; };
            std::vector<Try> tries;
            int foundP = -1, foundQ = -1;
            for (int p = 0; p < (int)y.pin.size() && foundP < 0; p++) {
                if (y.ci->isOut[(size_t)p] || !prog.uses(p)) continue;
                std::vector<int> qs;
                auto same = x.ci->pinIdx.find(y.ci->pins[(size_t)p]);
                if (same != x.ci->pinIdx.end()) qs.push_back(same->second);
                for (int q = 0; q < (int)x.pin.size(); q++)
                    if (qs.empty() || q != qs[0]) qs.push_back(q);
                for (int q : qs) {
                    if (x.ci->isOut[(size_t)q] || x.pin[(size_t)q] < 0) continue;
                    Lit l = om.comp(y, prog, p, nm.pinLit(x, q));
                    if (l == pt.n) { foundP = p, foundQ = q; break; }
                    tries.push_back({p, q, l});
                }
            }
            if (foundP < 0 && !tries.empty()) {
                pv.sync(T);
                std::vector<int> ok(tries.size(), 0);
                par::parallelFor(tries.size(), T, [&](size_t k) { ok[k] = pv.prove(tries[k].l, pt.n); });
                for (size_t k = 0; k < tries.size() && foundP < 0; k++)
                    if (ok[k] == 1) foundP = tries[k].p, foundQ = tries[k].q;
            }
            int nn = foundP < 0 ? -1 : x.pin[(size_t)foundQ];
            auto key = std::make_pair(pt.oi, foundP);
            if (foundP < 0 || (pinPlan.count(key) && pinPlan[key] != nn)) {
                late.push_back({pt.oi, pt.ni});
                replaced.insert(pt.oi);
            } else {
                pinPlan[key] = nn;
            }
        }
    }
    std::sort(late.begin(), late.end());
    late.erase(std::unique(late.begin(), late.end()), late.end());
    for (auto& [oi, ni] : late) doReplace(oi, ni);
    for (auto& [key, nn] : pinPlan)
        if (!replaced.count(key.first)) rewires.push_back({key.first, key.second, nn});

    invNow = true;
    {
        std::vector<int> roots;
        for (auto& r : rewires) roots.push_back(r.nnet);
        for (auto& b : binds) roots.push_back(b.nnet);
        for (auto& [ki, ni] : pendingInputs)
            for (size_t k = 0; k < N.insts[(size_t)ni].pin.size(); k++)
                if (!N.insts[(size_t)ni].ci->isOut[k]) roots.push_back(N.insts[(size_t)ni].pin[k]);
        prefill(roots);
    }
    for (size_t i = 0; i < pendingInputs.size(); i++) {
        auto [ki, ni] = pendingInputs[i];
        for (size_t k = 0; k < N.insts[(size_t)ni].pin.size(); k++)
            if (!N.insts[(size_t)ni].ci->isOut[k]) { int in = mapNet(N.insts[(size_t)ni].pin[k]); setIn(ki, (int)k, in); }
    }
    for (auto& r : rewires) { int in = mapNet(r.nnet); setIn(r.oi, r.pin, in); }
    for (auto& b : binds) { int in = mapNet(b.nnet); O.ports[(size_t)b.port].net = in; }

    // Metal-only: no cell is added and none is removed. Old cells nothing
    // reads any more stay where they are, inputs tied low, and are spare
    // cells from now on; every added gate is moved onto a spare cell that
    // can compute it (the same cell, or one that can with inputs tied or
    // shared), with a spare inverter behind it when only the inverse fits.
    if (opt.spare) {
        std::vector<uint8_t> rm(O.insts.size(), 0);
        for (int i : removeInsts) rm[(size_t)i] = 1;
        auto dead = detail::unloaded(O, [&](size_t i) { return i < origInsts && (O.insts[i].ci->kind == Kind::Comb || rm[i]); });
        std::vector<int> pool;
        std::vector<detail::Gate1> gate(origInsts);
        for (size_t i = 0; i < origInsts; i++) {
            Inst& x = O.insts[i];
            if (x.dead) continue;
            // A spare cell whose output the patch now reads is in use.
            bool before = spareBefore[i] && dead[i] && detail::globMatch(opt.spareGlob.c_str(), x.name.c_str());
            bool freed = dead[i] && !spareBefore[i];
            if (freed) {
                res.freed++;
                for (size_t k = 0; k < x.pin.size(); k++)
                    if (!x.ci->isOut[k]) setIn((int)i, (int)k, constNet(0));
            }
            if ((before || freed) && detail::gate1(*x.ci, gate[i])) pool.push_back((int)i);
        }
        std::vector<uint8_t> taken(origInsts, 0);
        std::map<std::pair<const CellInfo*, std::pair<uint64_t, int>>, std::vector<int>> memo;
        auto fit = [&](const CellInfo* ci, const detail::Gate1& g, uint64_t tt, int k) -> const std::vector<int>& {
            auto key = std::make_pair(ci, std::make_pair(tt, k));
            auto it = memo.find(key);
            if (it == memo.end()) it = memo.emplace(key, detail::standIn(g, tt, k)).first;
            return it->second;
        };
        // The free spare cell for a function: the same cell first, then the
        // one with the fewest inputs, the smallest, the first in the netlist.
        auto pick = [&](const CellInfo* same, uint64_t tt, int k) {
            int best = -1;
            for (int i : pool) {
                if (taken[(size_t)i]) continue;
                const CellInfo* ci = O.insts[(size_t)i].ci;
                if (ci == same) return i;
                if (fit(ci, gate[(size_t)i], tt, k).empty()) continue;
                if (best < 0) { best = i; continue; }
                const CellInfo* bc = O.insts[(size_t)best].ci;
                if (std::make_pair(gate[(size_t)i].in.size(), ci->area) < std::make_pair(gate[(size_t)best].in.size(), bc->area)) best = i;
            }
            return best;
        };
        auto place = [&](int s, uint64_t tt, int k, const std::vector<int>& inNets, int outNet) {
            taken[(size_t)s] = 1;
            res.spares++;
            const detail::Gate1& g = gate[(size_t)s];
            const std::vector<int>& a = fit(O.insts[(size_t)s].ci, g, tt, k);
            for (size_t j = 0; j < g.in.size(); j++) {
                int v = a[j];
                setIn(s, g.in[j], v < k ? inNets[(size_t)v] : constNet(v - k));
            }
            int old = O.insts[(size_t)s].pin[(size_t)g.out];
            if (old >= 0 && O.nets[(size_t)old].drvInst == s) O.nets[(size_t)old].drvInst = -1;
            drive(s, g.out, outNet);
        };
        const size_t nAll = O.insts.size();
        for (size_t ai = origInsts; ai < nAll; ai++) {
            if (O.insts[ai].dead) continue;
            const CellInfo* ci = O.insts[ai].ci;
            detail::Gate1 g;
            bool placed = false;
            if (detail::gate1(*ci, g) && O.insts[ai].pin[(size_t)g.out] >= 0) {
                int k = (int)g.in.size();
                uint64_t mask = k == 6 ? ~0ull : (1ull << (1u << k)) - 1;
                std::vector<int> inNets;
                for (int p : g.in) inNets.push_back(O.insts[ai].pin[(size_t)p] >= 0 ? O.insts[ai].pin[(size_t)p] : constNet(0));
                int outNet = O.insts[ai].pin[(size_t)g.out];
                int s = pick(ci, g.tt & mask, k);
                if (s >= 0) {
                    place(s, g.tt & mask, k, inNets, outNet);
                    placed = true;
                } else if ((s = pick(nullptr, ~g.tt & mask, k)) >= 0) {
                    taken[(size_t)s] = 1;
                    int inv = pick(lib.inverter, 1, 1);  // tt 01: out = !in
                    taken[(size_t)s] = 0;
                    if (inv >= 0) {
                        int mid = addNet(opt.prefix + "n", om.lit[(size_t)outNet] ^ 1);
                        place(s, ~g.tt & mask, k, inNets, mid);
                        place(inv, 1, 1, {mid}, outNet);
                        placed = true;
                    }
                }
            }
            if (placed) {
                for (size_t k = 0; k < O.insts[ai].pin.size(); k++)
                    if (!O.insts[ai].ci->isOut[k]) setIn((int)ai, (int)k, -1);
                O.insts[ai].dead = true;
            } else {
                res.unplaced.push_back(O.insts[ai].name + " (" + ci->name + ")");
            }
        }
    } else if (opt.sweep) {
        std::vector<uint8_t> rm(O.insts.size(), 0);
        for (int i : removeInsts) rm[(size_t)i] = 1;
        auto dead = detail::unloaded(O, [&](size_t i) { return O.insts[i].ci->kind == Kind::Comb || rm[i]; });
        for (size_t i = 0; i < O.insts.size(); i++) {
            if (dead[i] && !(i < deadBefore.size() && deadBefore[i])) O.insts[i].dead = true;
            else if (rm[i]) res.warnings.push_back(O.insts[i].name + " is not in the new netlist but still drives logic; it is kept");
        }
    } else {
        for (int i : removeInsts) res.warnings.push_back(O.insts[(size_t)i].name + " is not in the new netlist; kept (-no-sweep)");
    }

    detail::emit(O, origPin, origNets, opt, res);
    res.patched = std::move(O.mod);

    // ---- verify the patched module, built again from scratch
    {
        Design P = build(res.patched, lib, false);
        Model pm;
        pm.compute(P, V);
        std::vector<Point> vp = detail::points(P, pm, N, nm);
        pv.sync(T);
        par::parallelFor(vp.size(), T, [&](size_t i) { vp[i].status = pv.prove(vp[i].o, vp[i].n); });
        std::set<std::string> seen, retry;
        for (auto& p : vp) {
            seen.insert(p.name);
            if (p.status == 1) continue;
            res.unverified.push_back(p.name + (p.status < 0 ? " (undecided)" : ""));
            if (p.what >= Point::Next && p.what <= Point::Preset) retry.insert(N.insts[(size_t)p.ni].name);
        }
        // Everything the new netlist has must have been compared.
        for (auto& p : N.ports)
            if (p.out && !seen.count(p.name)) res.unverified.push_back(p.name + " (missing)");
        for (auto& x : N.insts) {
            if (x.ci->kind == Kind::Comb) continue;
            auto it = P.instIdx.find(x.name);
            bool ok = it != P.instIdx.end() && P.insts[(size_t)it->second].ci->kind == x.ci->kind &&
                      (x.ci->kind == Kind::State ? P.insts[(size_t)it->second].ci->latch == x.ci->latch
                                                 : P.insts[(size_t)it->second].ci == x.ci);
            if (!ok) res.unverified.push_back(x.name + " (missing)");
        }
        res.verified = res.unverified.empty();
        // A fix that did not hold: first without the rectification search
        // (it decides on simulation patterns), then with the registers
        // concerned swapped for the new cells.
        if (!res.verified) {
            Options o2 = opt;
            bool again = false;
            if (opt.rectify) o2.rectify = false, again = true;
            else
                for (auto& n : retry)
                    if (o2.replace.insert(n).second) again = true;
            if (again) return runCore(oldM, newM, lib, o2);
        }
    }
    return res;
}

// The patch. The search for the gate to change gives the small patch when
// the two netlists are built alike; when the new one is a fresh synthesis
// with another structure, copying the changed cones can come out smaller.
// So a patch that is not small is also made the second way, and the
// smaller of the two is kept.
inline Result run(const gnl::Module& oldM, const gnl::Module& newM, const Lib& lib, Options opt) {
    Result a = runCore(oldM, newM, lib, opt);
    if (!opt.rectify || !a.verified || a.added + a.spares <= 8) return a;
    Options o2 = opt;
    o2.rectify = false;
    Result b = runCore(oldM, newM, lib, o2);
    auto cost = [](const Result& r) { return std::make_tuple(r.unplaced.size(), r.added + r.spares, r.areaAdded, r.rewired); };
    return b.verified && cost(b) < cost(a) ? std::move(b) : std::move(a);
}

// The edits as a Tcl script in the form place-and-route tools read.
inline void writeScript(const Result& r, std::ostream& os) {
    auto q = [](const std::string& s) { return "{" + s + "}"; };
    os << "# ECO script written by meco\n";
    for (auto& c : r.changes)
        if (c.kind == Change::AddPort) os << "create_port -direction " << (c.b == "input" ? "in" : "out") << " " << q(c.a) << "\n";
    for (auto& c : r.changes)
        if (c.kind == Change::AddNet) os << "create_net " << q(c.a) << "\n";
    for (auto& c : r.changes)
        if (c.kind == Change::AddCell) os << "create_cell " << q(c.a) << " " << q(c.b) << "\n";
    for (auto& c : r.changes)
        if (c.kind == Change::Disconnect) os << "disconnect_net " << q(c.a) << " " << q(c.b) << "\n";
    for (auto& c : r.changes)
        if (c.kind == Change::Connect) os << "connect_net " << q(c.a) << " " << q(c.b) << "\n";
    for (auto& c : r.changes)
        if (c.kind == Change::RemoveCell) os << "remove_cell " << q(c.a) << "\n";
}

}  // namespace eco
