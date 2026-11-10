// emu/design.hpp - the design as memu sees it: a flat gate netlist
// (gnetlist.hpp), each cell's role from the Liberty library (liberty.hpp) or
// a memory description, the clocks, and the delta levels of the clock network.
#pragma once
#include <cmath>

#include "../gnetlist.hpp"
#include "../liberty.hpp"
#include "base.hpp"

namespace emu {

enum class Kind : uint8_t { Comb, Buf, Inv, FF, Latch, Mem };
enum PinCls : uint8_t { PIn, POut, PClk, PAsync, PData };

// What memu needs to know about a library cell.
struct Role {
    Kind kind = Kind::Comb;
    std::string cell;
    std::unordered_map<std::string, PinCls> pins;  // a memory: by the pin's bus name
    std::string clkPin, dPin, qPin, qnPin;         // FF
    bool clkNeg = false, dNeg = false;
    std::string problem;                           // why an FF of this cell cannot be converted
    const MemSpec* mem = nullptr;
    double lut = 0, ff = 0, bramBits = 0;
};

// What makes a flop or a memory port take a step: an edge of a primary clock
// (prim) or of a net inside the design (root), rising unless neg.
struct Trigger {
    int root = -1, prim = -1;
    bool neg = false;
    bool none() const { return root < 0 && prim < 0; }
};

struct Clock {
    std::string name;
    int net = -1;
    long periodPs = 10000, phasePs = 0;
    bool pure = true;                  // only reaches clock pins, through buffers and inverters
    bool livePos = false, liveNeg = false;
};

struct Options {
    double gatesPerLut = 3;
    long lutramMaxBits = 4096;   // a clockless-read memory above this goes to block RAM
    bool forceEdge = false;      // edge detection even when every clock is pure
    int settle = -1;             // -settle N: the delta count, instead of the computed one
};

inline std::string baseOf(const std::string& pin) {
    auto b = pin.find('[');
    return b == std::string::npos ? pin : pin.substr(0, b);
}
inline int bitOf(const std::string& pin) {
    auto b = pin.find('[');
    return b == std::string::npos ? 0 : std::atoi(pin.c_str() + b + 1);
}

struct Design {
    gnl::Netlist nl;
    Options opt;
    std::vector<Role> roles;
    std::unordered_map<std::string, int> roleOf;
    std::vector<MemSpec> mems;
    std::vector<int> role;                 // per instance
    std::vector<int> connOff;              // per instance, into cls
    std::vector<uint8_t> cls;              // per connection
    std::vector<int> drv;                  // per net: instance, -1 none, -2-p for input port p
    std::vector<int> sinkOff, sinkInst;    // per net: the instances that read it (any pin)
    std::vector<int> clockOfNet;           // per net: primary clock index or -1
    std::vector<Clock> clocks;
    bool strobe = true;                    // every clock pure: flops use the generator's strobes
    std::vector<Trigger> trig;             // per instance (FF)
    std::unordered_map<int, std::vector<Trigger>> memTrig;  // per memory instance, per port
    std::vector<int> order;                // instances, drivers before readers of level-carrying pins
    int dc = 0, da = 0;                    // deltas after a clock event, and after the inputs change
    std::vector<std::string> warnings;

    size_t ni() const { return nl.insts.size(); }
    size_t nn() const { return nl.nets.size(); }
    const Role& roleOfInst(int i) const { return roles[role[i]]; }
    Kind kind(int i) const { return roles[role[i]].kind; }
    bool isConst(int n) const { return n >= 0 && (n == nl.const0 || n == nl.const1); }
    PinCls pinCls(int i, size_t k) const { return (PinCls)cls[connOff[i] + k]; }
    int pinNet(int i, const std::string& pin) const {
        for (auto& c : nl.insts[i].conns)
            if (c.pin == pin) return c.net;
        return -1;
    }

    // ----------------------------------------------------------------- roles

    static bool singleVar(const std::string& fn, std::string& var, bool& neg) {
        if (fn.empty()) return false;
        liberty::Expr e;
        try {
            e = liberty::parseFunction(fn);
        } catch (const std::exception&) {
            return false;
        }
        neg = false;
        while (e.kind == liberty::Expr::Not) {
            neg = !neg;
            liberty::Expr k = e.kids[0];
            e = k;
        }
        if (e.kind != liberty::Expr::Var) return false;
        var = e.var;
        return true;
    }

    int makeRole(const liberty::Cell& c) {
        Role r;
        r.cell = c.name;
        int nIn = 0, nOut = 0;
        std::string in1, out1;
        for (auto& p : c.pins) {
            if (p.dir == liberty::Dir::Internal) continue;
            bool out = p.dir == liberty::Dir::Out || p.dir == liberty::Dir::InOut;
            r.pins[p.name] = out ? POut : PIn;
            (out ? nOut : nIn)++;
            (out ? out1 : in1) = p.name;
        }
        if (!c.ffs.empty()) {
            r.kind = Kind::FF;
            r.ff = 1;
            auto& f = c.ffs[0];
            if (c.ffs.size() > 1) r.problem = "more than one ff group";
            if (!f.clockedOnAlso.empty()) r.problem = "master-slave flop (clocked_on_also)";
            if (!singleVar(f.clockedOn, r.clkPin, r.clkNeg))
                r.problem = "clocked_on \"" + f.clockedOn + "\" is not one pin";
            if (!singleVar(f.nextState, r.dPin, r.dNeg))
                r.problem = "next_state \"" + f.nextState + "\" is not one pin";
            for (auto* fn : {&f.clear, &f.preset}) {
                if (fn->empty()) continue;
                try {
                    for (auto& v : liberty::parseFunction(*fn).vars())
                        if (r.pins.count(v)) r.pins[v] = PAsync;
                } catch (const std::exception&) {
                    r.problem = "cannot read \"" + *fn + "\"";
                }
            }
            if (r.problem.empty()) {
                r.pins[r.clkPin] = PClk;
                r.pins[r.dPin] = PData;
            }
            for (auto& p : c.pins) {
                if (p.function == f.iq) r.qPin = p.name;
                if (p.function == f.iqn) r.qnPin = p.name;
            }
            if (r.problem.empty() && r.qPin.empty() && r.qnPin.empty()) r.problem = "no output with the state as its function";
        } else if (!c.latches.empty()) {
            r.kind = Kind::Latch;
            r.lut = 1;
        } else {
            r.lut = 1.0 / opt.gatesPerLut;
            std::string v;
            bool neg = false;
            if (nIn == 1 && nOut == 1 && singleVar(c.pin(out1)->function, v, neg) && v == in1) {
                r.kind = neg ? Kind::Inv : Kind::Buf;
                r.lut = 0;
            }
        }
        roles.push_back(std::move(r));
        return (int)roles.size() - 1;
    }

    int makeRole(const MemSpec& m) {
        Role r;
        r.kind = Kind::Mem;
        r.cell = m.cell;
        r.mem = &m;
        for (auto& p : m.ports) {
            for (auto* s : {&p.addr, &p.din, &p.we, &p.cs, &p.re, &p.wmask})
                if (!s->empty()) r.pins[*s] = PIn;
            if (!p.dout.empty()) r.pins[p.dout] = POut;
            if (!p.clk.empty()) r.pins[p.clk] = PClk;
        }
        bool block = !m.asyncRead() || m.lagRead;
        if (block) r.bramBits = (double)m.bits();
        else r.lut = std::ceil(m.bits() / 64.0) * std::max<size_t>(1, m.ports.size());
        roles.push_back(std::move(r));
        return (int)roles.size() - 1;
    }

    // Reads the netlists, flattens below `top`, and gives every instance a role.
    void load(const std::vector<std::string>& netlists, const std::vector<std::string>& libs,
              const std::vector<std::string>& memFiles, std::string top) {
        gnl::Source src;
        for (auto& f : netlists) gnl::parse(readFile(f), f, src);
        if (top.empty()) {
            auto t = gnl::tops(src);
            if (t.size() != 1) {
                std::string all;
                for (auto& n : t) all += " " + n;
                throw Error("pick the top module with -top; candidates:" + all);
            }
            top = t[0];
        }
        std::vector<liberty::Library> L;
        for (auto& f : libs) L.push_back(liberty::load(f));
        for (auto& f : memFiles)
            for (auto& m : loadMemSpecs(f)) mems.push_back(std::move(m));
        for (auto& m : mems) m.lagRead = m.asyncRead() && m.bits() > opt.lutramMaxBits;
        std::unordered_map<std::string, const MemSpec*> memOf;
        for (auto& m : mems) memOf[m.cell] = &m;
        auto libCell = [&](const std::string& n) -> const liberty::Cell* {
            for (auto& l : L)
                if (auto* c = l.cell(n)) return c;
            return nullptr;
        };
        auto busPins = [&](const std::string& cell, const std::string& pin, int w) {
            std::vector<std::string> r;
            if (memOf.count(cell)) return r;  // pin[w-1] .. pin[0]
            if (auto* c = libCell(cell)) {
                std::vector<std::pair<int, std::string>> bits;
                for (auto& p : c->pins)
                    if (p.bus == pin) bits.push_back({p.busBit, p.name});
                std::sort(bits.rbegin(), bits.rend());
                for (auto& b : bits) r.push_back(b.second);
                if ((int)r.size() != w) r.clear();
            }
            return r;
        };
        auto pinOrder = [&](const std::string& cell) {
            std::vector<std::string> r;
            if (auto* c = libCell(cell))
                for (auto& p : c->pins) r.push_back(p.name);
            return r;
        };
        nl = gnl::flatten(src, top, busPins, pinOrder);
        for (auto& w : nl.warnings) warnings.push_back(w);

        role.resize(ni());
        connOff.resize(ni() + 1);
        std::map<std::string, int> unknown;
        size_t total = 0;
        for (size_t i = 0; i < ni(); i++) {
            auto& in = nl.insts[i];
            connOff[i] = (int)total;
            total += in.conns.size();
            auto it = roleOf.find(in.cell);
            if (it == roleOf.end()) {
                int r = -1;
                if (memOf.count(in.cell)) r = makeRole(*memOf[in.cell]);
                else if (auto* c = libCell(in.cell)) r = makeRole(*c);
                it = roleOf.emplace(in.cell, r).first;
            }
            role[i] = it->second;
            if (role[i] < 0) unknown[in.cell]++;
        }
        connOff[ni()] = (int)total;
        if (!unknown.empty()) {
            std::string m = "cells that are in no library (-lib) and no memory file (-mem):";
            for (auto& [c, n] : unknown) m += " " + c + " (" + std::to_string(n) + ")";
            throw Error(m);
        }
        cls.resize(total);
        drv.assign(nn(), -1);
        std::vector<int> cnt(nn() + 1, 0);
        for (size_t i = 0; i < ni(); i++) {
            auto& in = nl.insts[i];
            auto& r = roles[role[i]];
            for (size_t k = 0; k < in.conns.size(); k++) {
                auto& c = in.conns[k];
                auto pit = r.pins.find(r.kind == Kind::Mem ? baseOf(c.pin) : c.pin);
                if (pit == r.pins.end())
                    throw Error("instance " + in.name + ": cell " + in.cell + " has no pin " + c.pin);
                cls[connOff[i] + k] = pit->second;
                if (c.net < 0) continue;
                if (pit->second == POut) {
                    if (drv[c.net] >= 0 && drv[c.net] != (int)i)
                        warnings.push_back("net " + nl.nets[c.net] + " has more than one driver");
                    drv[c.net] = (int)i;
                } else {
                    cnt[c.net]++;
                }
            }
        }
        for (size_t p = 0; p < nl.ports.size(); p++)
            if (nl.ports[p].dir == gnl::Dir::In && nl.ports[p].net >= 0 && drv[nl.ports[p].net] == -1)
                drv[nl.ports[p].net] = -2 - (int)p;
        sinkOff.assign(nn() + 1, 0);
        for (size_t n = 0; n < nn(); n++) sinkOff[n + 1] = sinkOff[n] + cnt[n];
        sinkInst.resize(sinkOff[nn()]);
        std::vector<int> fill(sinkOff.begin(), sinkOff.end() - 1);
        for (size_t i = 0; i < ni(); i++)
            for (size_t k = 0; k < nl.insts[i].conns.size(); k++) {
                int n = nl.insts[i].conns[k].net;
                if (n >= 0 && cls[connOff[i] + k] != POut) sinkInst[fill[n]++] = (int)i;
            }
    }

    // ---------------------------------------------------------------- clocks

    // Follows buffers and inverters back from a net to where its value is made.
    int rootOf(int net, bool& inv) const {
        inv = false;
        for (int guard = 0; net >= 0 && guard < 100000; guard++) {
            int d = drv[net];
            if (d < 0) break;
            Kind k = kind(d);
            if (k != Kind::Buf && k != Kind::Inv) break;
            int in = -1;
            for (size_t c = 0; c < nl.insts[d].conns.size(); c++)
                if (pinCls(d, c) == PIn) in = nl.insts[d].conns[c].net;
            if (in < 0) break;
            if (k == Kind::Inv) inv = !inv;
            net = in;
        }
        return net;
    }

    Trigger triggerOf(int net, bool cellNeg) const {
        Trigger t;
        if (net < 0 || isConst(net)) return t;
        bool inv = false;
        int r = rootOf(net, inv);
        if (r < 0 || isConst(r)) return t;
        t.neg = cellNeg != inv;
        if (clockOfNet[r] >= 0) t.prim = clockOfNet[r];
        else t.root = r;
        return t;
    }

    // -clock name[:period_ns[:phase_ns]]; with none given, every input port
    // that reaches a clock pin through buffers and inverters only is a clock.
    void findClocks(const std::vector<std::string>& specs) {
        clockOfNet.assign(nn(), -1);
        std::unordered_map<std::string, int> portOf;
        for (size_t p = 0; p < nl.ports.size(); p++)
            if (nl.ports[p].dir == gnl::Dir::In) portOf[nl.ports[p].name] = (int)p;
        auto add = [&](const std::string& name, long per, long ph) {
            auto it = portOf.find(name);
            if (it == portOf.end()) throw Error("-clock " + name + ": no input port of that name");
            int net = nl.ports[it->second].net;
            if (net < 0 || clockOfNet[net] >= 0) return;
            clockOfNet[net] = (int)clocks.size();
            Clock c;
            c.name = name;
            c.net = net;
            c.periodPs = per;
            c.phasePs = ph;
            clocks.push_back(c);
        };
        for (auto& s : specs) {
            std::vector<std::string> f;
            std::stringstream ss(s);
            std::string part;
            while (std::getline(ss, part, ':')) f.push_back(part);
            if (f.empty() || f[0].empty()) throw Error("-clock needs name[:period_ns[:phase_ns]]");
            long per = f.size() > 1 ? std::lround(std::atof(f[1].c_str()) * 1000) : 10000;
            long ph = f.size() > 2 ? std::lround(std::atof(f[2].c_str()) * 1000) : 0;
            if (per < 2 || per % 2) throw Error("-clock " + f[0] + ": the period must be an even number of ps");
            if (ph < 0 || ph >= per) throw Error("-clock " + f[0] + ": the phase is from 0 up to the period");
            add(f[0], per, ph);
        }
        if (specs.empty()) {
            std::vector<char> isClk(nl.ports.size(), 0);
            auto mark = [&](int net) {
                bool inv;
                int r = rootOf(net, inv);
                if (r >= 0 && drv[r] <= -2) isClk[-2 - drv[r]] = 1;
            };
            for (size_t i = 0; i < ni(); i++) {
                if (kind((int)i) != Kind::FF && kind((int)i) != Kind::Mem) continue;
                for (size_t k = 0; k < nl.insts[i].conns.size(); k++)
                    if (pinCls((int)i, k) == PClk) mark(nl.insts[i].conns[k].net);
            }
            for (size_t p = 0; p < nl.ports.size(); p++)
                if (isClk[p]) add(nl.ports[p].name, 10000, 0);
        }

        // Triggers, and which clock edges anything listens to.
        trig.assign(ni(), {});
        for (size_t i = 0; i < ni(); i++) {
            auto& r = roles[role[i]];
            if (r.kind == Kind::FF) {
                if (!r.problem.empty())
                    throw Error("instance " + nl.insts[i].name + ": flop cell " + r.cell +
                                " cannot be moved onto the emulation clock: " + r.problem);
                trig[i] = triggerOf(pinNet((int)i, r.clkPin), r.clkNeg);
                if (trig[i].none())
                    warnings.push_back("flop " + nl.insts[i].name + " has no clock: it keeps its value");
            } else if (r.kind == Kind::Mem) {
                auto& v = memTrig[(int)i];
                for (auto& p : r.mem->ports)
                    v.push_back(p.clk.empty() ? Trigger{} : triggerOf(pinNet((int)i, p.clk), p.clkNeg));
            }
        }
        // A clock is pure when nothing but clock pins (through buffers and
        // inverters) reads it: then it never has to exist as a signal.
        for (auto& c : clocks) {
            std::vector<int> q{c.net};
            std::vector<char> seen(nn(), 0);
            seen[c.net] = 1;
            for (size_t h = 0; h < q.size() && c.pure; h++) {
                int n = q[h];
                for (auto& p : nl.ports)
                    if (p.dir != gnl::Dir::In && p.net == n) c.pure = false;
                for (int s = sinkOff[n]; s < sinkOff[n + 1] && c.pure; s++) {
                    int i = sinkInst[s];
                    Kind k = kind(i);
                    for (size_t x = 0; x < nl.insts[i].conns.size(); x++) {
                        auto& cn = nl.insts[i].conns[x];
                        if (cn.net != n || pinCls(i, x) == POut) continue;
                        if (k == Kind::Buf || k == Kind::Inv) {
                            for (auto& o : nl.insts[i].conns)
                                if (o.net >= 0 && o.net != n && !seen[o.net]) {
                                    seen[o.net] = 1;
                                    q.push_back(o.net);
                                }
                        } else if (pinCls(i, x) != PClk) {
                            c.pure = false;
                        }
                    }
                }
            }
        }
        strobe = !opt.forceEdge;
        for (auto& c : clocks)
            if (!c.pure) strobe = false;
        auto live = [&](const Trigger& t) {
            if (t.prim < 0) return;
            (t.neg ? clocks[t.prim].liveNeg : clocks[t.prim].livePos) = true;
        };
        for (auto& t : trig) live(t);
        for (auto& [i, v] : memTrig)
            for (auto& t : v) live(t);
        if (!strobe)  // the clock is a signal: both of its edges happen
            for (auto& c : clocks) c.livePos = c.liveNeg = true;
    }

    // ---------------------------------------------------------------- levels
    //
    // An emulation tick stands for one delta cycle of the simulation. After a
    // clock event a flop on a primary clock steps first; a flop clocked by a
    // net that another flop drives steps one delta later, and so on. dc is how
    // many deltas a clock event needs, da how many a change of the data inputs
    // needs (inputs that reach a clock pin).

    // The instances whose outputs an instance's level depends on.
    template <class F>
    void levelInputs(int i, F&& f) const {
        auto& in = nl.insts[i];
        Kind k = kind(i);
        auto from = [&](int net) {
            if (net >= 0 && !isConst(net) && clockOfNet[net] < 0 && drv[net] >= 0) f(drv[net]);
        };
        if (k == Kind::FF) {
            if (trig[i].root >= 0) from(trig[i].root);
            for (size_t c = 0; c < in.conns.size(); c++)
                if (pinCls(i, c) == PAsync) from(in.conns[c].net);
        } else if (k == Kind::Mem) {
            for (auto& t : memTrig.at(i))
                if (t.root >= 0) from(t.root);
            if (roles[role[i]].mem->asyncRead())
                for (size_t c = 0; c < in.conns.size(); c++)
                    if (pinCls(i, c) == PIn) from(in.conns[c].net);
        } else {
            for (size_t c = 0; c < in.conns.size(); c++)
                if (pinCls(i, c) != POut) from(in.conns[c].net);
        }
    }

    // The level of a net's last change in a phase: -1 when it does not change.
    int netLevel(int net, const std::vector<int>& S, bool inputPhase) const {
        if (net < 0 || isConst(net)) return -1;
        if (clockOfNet[net] >= 0) return inputPhase ? -1 : 0;
        int d = drv[net];
        if (d >= 0) return S[d];
        return (d <= -2 && inputPhase) ? 0 : -1;
    }

    int stepLevel(const Trigger& t, const std::vector<int>& S, bool inputPhase) const {
        if (t.none()) return -1;
        if (t.prim >= 0) return inputPhase ? -1 : (strobe ? 0 : 1);
        int s = netLevel(t.root, S, inputPhase);
        return s < 0 ? -1 : s + 1;
    }

    // One instance: the level of its outputs, and the latest step it takes.
    int evalLevel(int i, const std::vector<int>& S, bool inputPhase, int& step) const {
        auto& in = nl.insts[i];
        Kind k = kind(i);
        int s = -1;
        auto levelIn = [&](PinCls want, bool any) {
            for (size_t c = 0; c < in.conns.size(); c++) {
                PinCls pc = pinCls(i, c);
                if (pc == POut || (!any && pc != want)) continue;
                s = std::max(s, netLevel(in.conns[c].net, S, inputPhase));
            }
        };
        if (k == Kind::FF) {
            int u = stepLevel(trig[i], S, inputPhase);
            step = std::max(step, u);
            s = u;
            levelIn(PAsync, false);
        } else if (k == Kind::Mem) {
            for (auto& t : memTrig.at(i)) {
                int u = stepLevel(t, S, inputPhase);
                step = std::max(step, u);
                s = std::max(s, u);
            }
            if (roles[role[i]].mem->asyncRead()) levelIn(PIn, false);
        } else {
            levelIn(PIn, true);
        }
        return s;
    }

    void computeLevels() {
        size_t n = ni();
        std::vector<int> indeg(n, 0);
        std::vector<std::vector<int>> out(n);
        for (size_t i = 0; i < n; i++)
            levelInputs((int)i, [&](int d) {
                out[d].push_back((int)i);
                indeg[i]++;
            });
        order.clear();
        order.reserve(n);
        for (size_t i = 0; i < n; i++)
            if (!indeg[i]) order.push_back((int)i);
        for (size_t h = 0; h < order.size(); h++)
            for (int j : out[order[h]])
                if (!--indeg[j]) order.push_back(j);
        std::vector<int> loop;
        for (size_t i = 0; i < n; i++)
            if (indeg[i]) loop.push_back((int)i);
        size_t sorted = order.size();
        order.insert(order.end(), loop.begin(), loop.end());

        int steps[2] = {0, 0};
        for (int phase = 0; phase < 2; phase++) {
            std::vector<int> S(n, -1);
            int step = -1;
            for (size_t h = 0; h < sorted; h++) S[order[h]] = evalLevel(order[h], S, phase == 1, step);
            // What is left sits on a loop. A loop of gates or latches settles;
            // a loop through a clock pin (a flop that clocks itself) never does.
            if (!loop.empty()) {
                const int cap = 64;
                bool changed = true;
                for (int round = 0; changed && round <= cap; round++) {
                    changed = false;
                    for (int i : loop) {
                        int s = evalLevel(i, S, phase == 1, step);
                        if (s != S[i]) {
                            S[i] = s;
                            changed = true;
                        }
                    }
                }
                if (changed && opt.settle < 0) {
                    std::string who;
                    for (int i : loop)
                        if (kind(i) == Kind::FF) {
                            who = nl.insts[i].name;
                            break;
                        }
                    throw Error("a clock loop: " + (who.empty() ? nl.insts[loop[0]].name : who) +
                                " is clocked by logic that it drives itself, so the deltas never end. "
                                "Give the number of deltas to run with -settle N.");
                }
                if (phase == 0)
                    warnings.push_back(std::to_string(loop.size()) + " cells are on a combinational loop (first: " +
                                       nl.insts[loop[0]].name + ")");
            }
            steps[phase] = std::max(0, step);
        }
        dc = steps[0];
        da = steps[1];
        if (opt.settle >= 0) {
            dc = std::max(dc, opt.settle);
            if (da) da = std::max(da, opt.settle);
        }
    }
};

}  // namespace emu
