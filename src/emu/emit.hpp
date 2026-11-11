// emu/emit.hpp - what memu writes: one netlist per FPGA, the clock generator
// and memory models they share, a model of the board that wires the FPGAs
// together, and a testbench that runs the original netlist next to it.
#pragma once
#include <set>

#include "part.hpp"

namespace emu {

struct Probe {
    int net = -1, fpga = 0, bit = 0;
    std::string name;
};

struct PortGroup {
    std::string name;
    bool in = true, ranged = false;
    int lo = 0, hi = 0;
    int clock = -1;                 // a primary clock: its index
    std::vector<int> ports;         // indices into nl.ports
    int width() const { return hi - lo + 1; }
};

struct Emitter {
    const Design& d;
    const Board& board;
    const Plan& pl;
    std::string top;
    int traceDepth = 1024;
    std::vector<Probe> probes;
    std::string triggerName;
    int triggerNet = -1, triggerFpga = -1, triggerPost = 0;

    // One direction of a link, seen from one FPGA.
    struct Dir {
        int peer = 0, pins = 0, ratio = 1;
        const std::vector<int>* nets = nullptr;
        int direct = 0;  // with ratio > 1: the first so many nets have the first so many wires to themselves
    };
    std::vector<std::vector<int>> instsOf, piOf, poOf, probesOf;
    std::vector<std::vector<Dir>> txOf, rxOf;
    std::vector<PortGroup> groups;
    long unitPs = 1;

    int k() const { return board.k(); }
    int N() const { return d.dc + d.da + 1; }
    int NC() const { return std::max<int>(1, (int)d.clocks.size()); }
    int traceAw() const {
        int a = 1;
        while ((1 << a) < traceDepth) a++;
        return a;
    }
    static int bitsFor(long v) {
        int b = 1;
        while ((1L << b) <= v) b++;
        return b;
    }
    std::string fpgaMod(int f) const { return top + "_fpga" + std::to_string(f); }

    Emitter(const Design& d_, const Board& b_, const Plan& p_) : d(d_), board(b_), pl(p_) {
        top = d.nl.top;
        instsOf.resize(k());
        piOf.resize(k());
        poOf.resize(k());
        probesOf.resize(k());
        txOf.resize(k());
        rxOf.resize(k());
        for (size_t i = 0; i < d.ni(); i++) instsOf[pl.part[i]].push_back((int)i);
        for (size_t p = 0; p < d.nl.ports.size(); p++) {
            auto& pt = d.nl.ports[p];
            if (pt.net < 0) continue;
            int f = pl.partOfPort(d, (int)p);
            if (pt.dir == gnl::Dir::In) {
                if (d.drv[pt.net] == -2 - (int)p && d.clockOfNet[pt.net] < 0) piOf[f].push_back((int)p);
            } else {
                poOf[f].push_back((int)p);
            }
        }
        for (auto& l : pl.links) {
            if (!l.ab.empty()) {
                txOf[l.a].push_back({l.b, l.pinsAB, l.ratioAB, &l.ab, l.directAB});
                rxOf[l.b].push_back({l.a, l.pinsAB, l.ratioAB, &l.ab, l.directAB});
            }
            if (!l.ba.empty()) {
                txOf[l.b].push_back({l.a, l.pinsBA, l.ratioBA, &l.ba, l.directBA});
                rxOf[l.a].push_back({l.b, l.pinsBA, l.ratioBA, &l.ba, l.directBA});
            }
        }
        // Ports by name, buses put back together.
        std::unordered_map<std::string, int> gi;
        for (size_t p = 0; p < d.nl.ports.size(); p++) {
            auto& pt = d.nl.ports[p];
            std::string n = pt.bus.empty() ? pt.name : pt.bus;
            auto it = gi.find(n);
            if (it == gi.end()) {
                it = gi.emplace(n, (int)groups.size()).first;
                PortGroup g;
                g.name = n;
                g.in = pt.dir == gnl::Dir::In;
                g.ranged = !pt.bus.empty();
                g.lo = g.hi = pt.bus.empty() ? 0 : pt.bit;
                groups.push_back(g);
            }
            auto& g = groups[it->second];
            int bit = pt.bus.empty() ? 0 : pt.bit;
            g.lo = std::min(g.lo, bit);
            g.hi = std::max(g.hi, bit);
            g.ports.push_back((int)p);
            if (pt.dir == gnl::Dir::In && pt.net >= 0 && d.clockOfNet[pt.net] >= 0) {
                if (g.ranged) throw Error("clock " + pt.name + " is a bit of a bus; a clock has to be a port of its own");
                g.clock = d.clockOfNet[pt.net];
            }
        }
        // The time unit of the clock generator: the largest step every edge falls on.
        long g = 0;
        for (auto& c : d.clocks) {
            g = std::__gcd(g, c.periodPs / 2);
            if (c.phasePs) g = std::__gcd(g, c.phasePs);
        }
        unitPs = g ? g : 1;
    }

    void setProbes(const std::vector<std::string>& patterns, std::vector<std::string>& warnings) {
        if (patterns.empty()) return;
        std::vector<char> taken(d.nn(), 0);
        for (auto& pat : patterns) {
            int hits = 0;
            for (size_t n = 0; n < d.nn(); n++) {
                if (taken[n] || d.isConst((int)n) || !globMatch(pat.c_str(), d.nl.nets[n].c_str())) continue;
                if (d.clockOfNet[n] >= 0 || d.drv[n] == -1) continue;
                taken[n] = 1;
                hits++;
                Probe p;
                p.net = (int)n;
                p.name = d.nl.nets[n];
                p.fpga = pl.netPart(d, (int)n);
                probes.push_back(p);
            }
            if (!hits) warnings.push_back("-probe " + pat + " matches no net");
        }
        std::stable_sort(probes.begin(), probes.end(), [](const Probe& a, const Probe& b) { return a.fpga < b.fpga; });
        for (size_t i = 0; i < probes.size(); i++) {
            probes[i].bit = (int)probesOf[probes[i].fpga].size();
            probesOf[probes[i].fpga].push_back((int)i);
        }
    }

    // ------------------------------------------------------- clock generator

    std::string clockgen() const {
        std::ostringstream o;
        int n = N(), B = pl.B, nc = NC();
        long maxT = 1;
        for (auto& c : d.clocks) maxT = std::max(maxT, (c.phasePs + c.periodPs) / unitPs);
        int tw = bitsFor(maxT);
        o << "// The clock generator: one copy in every FPGA, all in step.\n"
          << "//   one delta is " << B << " emu_clk cycle" << (B > 1 ? "s" : "") << " (emu_tick ends it)\n"
          << "//   one event is " << n << " delta" << (n > 1 ? "s" : "") << ": " << d.dc << " after the clocks move, "
          << d.da << " after the inputs change, 1 to sample\n"
          << "//   emu_time counts in units of " << unitPs << " ps\n"
          << "module " << top << "_emu_clockgen (\n"
          << "  input emu_clk, input emu_rst, input emu_run,\n"
          << "  output emu_tick, output emu_ready, output emu_ev, output emu_hold,\n"
          << "  output [" << nc - 1 << ":0] emu_pos, output [" << nc - 1 << ":0] emu_neg, output [" << nc - 1
          << ":0] emu_clkv,\n  output [" << nc - 1 << ":0] emu_npos, output [" << nc - 1
          << ":0] emu_nneg, output [63:0] emu_ntime,\n  output reg [63:0] emu_time);\n";
        if (B > 1) {
            int bw = bitsFor(B - 1);
            o << "  reg [" << bw - 1 << ":0] ph;\n"
              << "  assign emu_tick = (ph == " << B - 1 << ");\n"
              << "  always @(posedge emu_clk) if (emu_rst | emu_tick) ph <= 0; else ph <= ph + 1'b1;\n";
        } else {
            o << "  assign emu_tick = 1'b1;\n";
        }
        if (n > 1) {
            int dw = bitsFor(n - 1);
            o << "  reg [" << dw - 1 << ":0] dl;\n"
              << "  wire last = (dl == " << n - 1 << ");\n"
              << "  always @(posedge emu_clk)\n"
              << "    if (emu_rst) dl <= " << n - 1 << ";\n"
              << "    else if (emu_tick) begin\n"
              << "      if (!last) dl <= dl + 1'b1;\n"
              << "      else if (emu_run) dl <= 0;\n"
              << "    end\n";
            if (d.dc >= 1) o << "  assign emu_hold = emu_tick & (dl == " << d.dc - 1 << ");\n";
            else o << "  assign emu_hold = 1'b0;\n";
        } else {
            o << "  wire last = 1'b1;\n  assign emu_hold = 1'b0;\n";
        }
        o << "  assign emu_ready = emu_tick & last & ~emu_rst;\n"
          << "  assign emu_ev = emu_ready & emu_run;\n";
        // The schedule: each clock counts the time to its next edge; an event
        // jumps to the nearest one.
        std::vector<int> act;
        for (size_t i = 0; i < d.clocks.size(); i++)
            if (d.clocks[i].livePos || d.clocks[i].liveNeg) act.push_back((int)i);
        std::string mn = "1";
        if (!act.empty()) {
            for (int i : act) o << "  reg [" << tw - 1 << ":0] rem" << i << ";\n  reg val" << i << ";\n";
            std::string prev = "rem" + std::to_string(act[0]);
            for (size_t x = 1; x < act.size(); x++) {
                std::string m = "mn" + std::to_string(x);
                o << "  wire [" << tw - 1 << ":0] " << m << " = (rem" << act[x] << " < " << prev << ") ? rem" << act[x]
                  << " : " << prev << ";\n";
                prev = m;
            }
            mn = prev;
        }
        for (int i = 0; i < nc; i++) {
            bool on = std::find(act.begin(), act.end(), i) != act.end();
            if (!on) {
                o << "  assign emu_pos[" << i << "] = 1'b0;\n  assign emu_neg[" << i << "] = 1'b0;\n  assign emu_clkv["
                  << i << "] = 1'b0;\n  assign emu_npos[" << i << "] = 1'b0;\n  assign emu_nneg[" << i << "] = 1'b0;\n";
                continue;
            }
            auto& c = d.clocks[i];
            long half = c.periodPs / 2 / unitPs, per = c.periodPs / unitPs, ph = c.phasePs / unitPs;
            bool both = c.livePos && c.liveNeg;
            o << "  // " << c.name << ": period " << c.periodPs << " ps, phase " << c.phasePs << " ps"
              << (both ? "" : c.livePos ? ", only the rising edge is used" : ", only the falling edge is used") << "\n"
              << "  wire hit" << i << " = (rem" << i << " == " << mn << ");\n";
            if (both) {
                o << "  assign emu_npos[" << i << "] = hit" << i << " & ~val" << i << ";\n"
                  << "  assign emu_nneg[" << i << "] = hit" << i << " & val" << i << ";\n";
            } else {
                o << "  assign emu_npos[" << i << "] = " << (c.livePos ? "hit" + std::to_string(i) : "1'b0") << ";\n"
                  << "  assign emu_nneg[" << i << "] = " << (c.liveNeg ? "hit" + std::to_string(i) : "1'b0") << ";\n";
            }
            o << "  assign emu_pos[" << i << "] = emu_ev & emu_npos[" << i << "];\n"
              << "  assign emu_neg[" << i << "] = emu_ev & emu_nneg[" << i << "];\n";
            o << "  assign emu_clkv[" << i << "] = val" << i << ";\n"
              << "  always @(posedge emu_clk)\n"
              << "    if (emu_rst) begin rem" << i << " <= " << (both || c.livePos ? ph + half : ph + per) << "; val" << i
              << " <= 1'b0; end\n"
              << "    else if (emu_ev) begin\n";
            if (both)
                o << "      if (hit" << i << ") begin val" << i << " <= ~val" << i << "; rem" << i << " <= " << half
                  << "; end\n";
            else
                o << "      if (hit" << i << ") rem" << i << " <= " << per << ";\n";
            o << "      else rem" << i << " <= rem" << i << " - " << mn << ";\n    end\n";
        }
        o << "  always @(posedge emu_clk)\n"
          << "    if (emu_rst) emu_time <= 64'd0;\n"
          << "    else if (emu_ev) emu_time <= emu_ntime;\n"
          << "  assign emu_ntime = emu_time + " << mn << ";\n"
          << "endmodule\n";
        return o.str();
    }

    // --------------------------------------------------------------- memories

    std::string memModule(const MemSpec& m) const {
        std::ostringstream o;
        std::map<std::string, std::pair<std::string, int>> pins;  // name -> direction, width
        std::vector<int> clocked;
        for (size_t p = 0; p < m.ports.size(); p++) {
            auto& pt = m.ports[p];
            pins[pt.addr] = {"input", m.abits};
            if (!pt.din.empty()) pins[pt.din] = {"input", m.width};
            if (!pt.wmask.empty()) pins[pt.wmask] = {"input", m.width};
            if (!pt.dout.empty()) pins[pt.dout] = {"output", m.width};
            for (auto* s : {&pt.we, &pt.cs, &pt.re})
                if (!s->empty()) pins[*s] = {"input", 1};
            if (!pt.clk.empty()) clocked.push_back((int)p);
        }
        o << "// " << m.cell << ": " << m.depth << " x " << m.width << ", ";
        if (!m.asyncRead()) o << "block RAM";
        else if (m.lagRead) o << "block RAM read on emu_clk (the netlist's read has no clock)";
        else o << "LUT RAM (a read without a clock)";
        o << "\nmodule emu_mem_" << m.cell << " (\n  input emu_clk";
        for (int p : clocked) o << ", input emu_ce" << p;
        for (auto& [n, dw] : pins) {
            o << ",\n  " << dw.first << " ";
            if (dw.second > 1) o << "[" << dw.second - 1 << ":0] ";
            o << vname(n);
        }
        o << ");\n  reg [" << m.width - 1 << ":0] mem [0:" << m.depth - 1 << "];\n";
        if (!m.init.empty()) o << "  initial $readmemh(\"" << m.init << "\", mem);\n";
        for (size_t p = 0; p < m.ports.size(); p++) {
            auto& pt = m.ports[p];
            auto act = [&](const std::string& pin, bool neg) {
                return pin.empty() ? std::string("1'b1") : (neg ? "~" : "") + vname(pin);
            };
            std::string q = "q" + std::to_string(p), a = vname(pt.addr);
            if (!pt.dout.empty() && (!pt.clk.empty() || m.lagRead))
                o << "  reg [" << m.width - 1 << ":0] " << q << ";\n  assign " << vname(pt.dout) << " = " << q << ";\n";
            if (pt.clk.empty()) {
                if (m.lagRead) o << "  always @(posedge emu_clk) " << q << " <= mem[" << a << "];\n";
                else o << "  assign " << vname(pt.dout) << " = mem[" << a << "];\n";
                continue;
            }
            o << "  always @(posedge emu_clk)\n    if (emu_ce" << p << " && " << act(pt.cs, pt.csNeg) << ") begin\n";
            std::string wr;
            if (!pt.din.empty()) {
                std::string dv = vname(pt.din);
                if (!pt.wmask.empty())
                    dv = "(" + dv + " & " + vname(pt.wmask) + ") | (mem[" + a + "] & ~" + vname(pt.wmask) + ")";
                wr = "mem[" + a + "] <= " + dv + ";";
            }
            std::string rd = pt.dout.empty() ? "" : q + " <= mem[" + a + "];";
            std::string re = act(pt.re, pt.reNeg);
            if (!wr.empty() && !rd.empty()) {
                o << "      if (" << act(pt.we, pt.weNeg) << ") " << wr << "\n      else if (" << re << ") " << rd << "\n";
            } else if (!wr.empty()) {
                o << "      if (" << act(pt.we, pt.weNeg) << ") " << wr << "\n";
            } else {
                o << "      if (" << re << ") " << rd << "\n";
            }
            o << "    end\n";
        }
        o << "endmodule\n";
        return o.str();
    }

    std::string library() const {
        std::string s = "// " + top + ": the clock generator and memory models every FPGA uses. Written by memu.\n";
        s += clockgen();
        std::set<std::string> done;
        for (auto& r : d.roles)
            if (r.kind == Kind::Mem && done.insert(r.cell).second) s += "\n" + memModule(*r.mem);
        return s;
    }

    // ----------------------------------------------------------------- an FPGA

    struct Header {
        bool debug = false;
        int np = 0;
    };

    bool hasDebug(int f) const { return !probes.empty() && (f == 0 || !probesOf[f].empty()); }

    std::string fpga(int f) const {
        std::ostringstream body, decl, holdAsg, o;
        std::vector<char> used(d.nn(), 0), isReg(d.nn(), 0), driven(d.nn(), 0);
        std::vector<int> usedList;
        auto ref = [&](int n) -> std::string {
            if (n < 0) return "1'bz";
            if (n == d.nl.const0) return "1'b0";
            if (n == d.nl.const1) return "1'b1";
            if (!used[n]) {
                used[n] = 1;
                usedList.push_back(n);
            }
            return "n" + std::to_string(n);
        };
        // name -> the signal it watches, and which edges are wanted
        struct Det {
            std::string sig;
            bool pos = false, neg = false;
        };
        std::map<std::string, Det> dets;
        bool needClkN = false;
        int ncWires = 0;
        auto ceOf = [&](const Trigger& t) -> std::string {
            if (t.none()) return "1'b0";
            if (t.prim >= 0 && d.strobe)
                return std::string(t.neg ? "emu_neg[" : "emu_pos[") + std::to_string(t.prim) + "]";
            std::string key = t.prim >= 0 ? "ck" + std::to_string(t.prim) : "n" + std::to_string(t.root);
            auto& dt = dets[key];
            dt.sig = t.prim >= 0 ? "emu_clkv[" + std::to_string(t.prim) + "]" : ref(t.root);
            (t.neg ? dt.neg : dt.pos) = true;
            return "ce_" + key + (t.neg ? "_n" : "_p");
        };

        for (int i : instsOf[f]) {
            auto& in = d.nl.insts[i];
            auto& r = d.roleOfInst(i);
            // pin (or bus) -> expression, in the order the pins first appear
            std::vector<std::pair<std::string, std::string>> scalars;
            std::vector<std::string> busOrder;
            std::map<std::string, std::map<int, std::pair<std::string, bool>>> buses;  // bit -> expr, is output
            std::string modName = vname(in.cell), ce;
            int dNet = -1, qNet = -1, qnNet = -1;
            bool sawQ = false;
            if (r.kind == Kind::FF) {
                ce = ceOf(d.trig[i]);
                for (auto& c : in.conns) {
                    if (c.pin == r.dPin) dNet = c.net;
                    if (c.pin == r.qPin && c.net >= 0) qNet = c.net, sawQ = true;
                    if (c.pin == r.qnPin && c.net >= 0) qnNet = c.net;
                }
            }
            for (size_t c = 0; c < in.conns.size(); c++) {
                auto& cn = in.conns[c];
                PinCls pc = d.pinCls(i, c);
                std::string e;
                bool rewired = r.kind == Kind::FF && (pc == PClk || (pc == PData && !d.trig[i].none()));
                if (cn.net < 0 && !rewired) continue;  // left open, as in the netlist
                if (r.kind == Kind::Mem) {
                    if (pc == PClk) continue;  // replaced by emu_clk and a clock enable
                    e = ref(cn.net);
                } else if (r.kind == Kind::FF && pc == PClk) {
                    if (d.trig[i].none()) e = "1'b0";
                    else if (r.clkNeg) e = "emu_clk_n", needClkN = true;
                    else e = "emu_clk";
                } else if (r.kind == Kind::FF && pc == PData && !d.trig[i].none()) {
                    e = "h" + std::to_string(i);
                } else {
                    e = ref(cn.net);
                }
                if (pc == POut && cn.net >= 0) driven[cn.net] = 1;
                if (cn.pin.find('[') != std::string::npos && cn.pin.back() == ']') {
                    std::string b = baseOf(cn.pin);
                    if (!buses.count(b)) busOrder.push_back(b);
                    buses[b][bitOf(cn.pin)] = {e, pc == POut};
                } else {
                    scalars.push_back({cn.pin, e});
                }
            }
            if (r.kind == Kind::FF && !d.trig[i].none()) {
                // The flop keeps its value unless its clock had an edge: next = ce ? D : Q.
                std::string keep;
                if (sawQ) {
                    keep = ref(qNet);
                } else if (qnNet >= 0) {
                    keep = "~" + ref(qnNet);
                } else {
                    keep = "q" + std::to_string(i);
                    decl << "  wire " << keep << ";\n";
                    scalars.push_back({r.qPin.empty() ? r.qnPin : r.qPin, keep});
                    if (r.qPin.empty()) keep = "~" + keep;
                }
                if (r.dNeg) keep = keep[0] == '~' ? keep.substr(1) : "~" + keep;
                // Declared here, assigned after the edge detectors it reads.
                decl << "  wire h" << i << ";\n";
                holdAsg << "  assign h" << i << " = " << ce << " ? " << (dNet >= 0 ? ref(dNet) : std::string("1'bx")) << " : "
                     << keep << ";\n";
            }
            if (r.kind == Kind::Mem) {
                modName = "emu_mem_" + in.cell;
                scalars.push_back({"emu_clk", "emu_clk"});
                auto& tr = d.memTrig.at(i);
                for (size_t p = 0; p < tr.size(); p++)
                    if (!r.mem->ports[p].clk.empty()) scalars.push_back({"emu_ce" + std::to_string(p), ceOf(tr[p])});
            }
            body << "  " << modName << " " << vname(in.name) << " (";
            bool first = true;
            for (auto& [pin, e] : scalars) {
                body << (first ? "" : ", ") << "." << vname(pin) << "(" << e << ")";
                first = false;
            }
            for (auto& b : busOrder) {
                auto& bits = buses[b];
                int hi = bits.rbegin()->first;
                if (r.kind == Kind::Mem) {
                    // The width is the memory's, not just the bits the netlist connected.
                    for (auto& pt : r.mem->ports) {
                        if (b == pt.addr) hi = std::max(hi, r.mem->abits - 1);
                        if (b == pt.din || b == pt.dout || b == pt.wmask) hi = std::max(hi, r.mem->width - 1);
                    }
                }
                bool isOut = bits.begin()->second.second;
                body << (first ? "" : ", ") << "." << vname(b) << "({";
                for (int x = hi; x >= 0; x--) {
                    auto it = bits.find(x);
                    std::string e;
                    if (it != bits.end()) e = it->second.first;
                    else if (!isOut) e = "1'b0";
                    if (e.empty() || (isOut && e[0] == '1')) {
                        e = "nc" + std::to_string(f) + "_" + std::to_string(ncWires++);
                        decl << "  wire " << e << ";\n";
                    }
                    body << e << (x ? ", " : "");
                }
                body << "})";
                first = false;
            }
            body << ");\n";
        }

        // Signals to and from the other FPGAs.
        std::set<int> ratios;
        std::ostringstream xfer;
        for (auto& t : txOf[f]) {
            std::string port = "tx_f" + std::to_string(t.peer);
            if (t.ratio == 1) {
                for (size_t x = 0; x < t.nets->size(); x++)
                    xfer << "  assign " << port << "[" << x << "] = " << ref((*t.nets)[x]) << ";\n";
                continue;
            }
            ratios.insert(t.ratio);
            int sh = t.pins - t.direct;  // the wires that are shared
            for (int x = 0; x < t.direct; x++)
                xfer << "  assign " << port << "[" << x << "] = " << ref((*t.nets)[x]) << ";\n";
            xfer << "  reg [" << sh - 1 << ":0] " << port << "_r;\n  assign " << port << "[" << t.pins - 1 << ":"
                 << t.direct << "] = " << port << "_r;\n"
                 << "  always @(posedge emu_clk)\n    case (tph" << t.ratio << ")\n";
            for (int s = 0; s < t.ratio; s++) {
                xfer << "      " << s << ": " << port << "_r <= {";
                for (int j = sh - 1; j >= 0; j--) {
                    size_t x = (size_t)t.direct + (size_t)s * sh + j;
                    xfer << (x < t.nets->size() ? ref((*t.nets)[x]) : std::string("1'b0")) << (j ? ", " : "");
                }
                xfer << "};\n";
            }
            xfer << "      default: ;\n    endcase\n";
        }
        for (auto& rx : rxOf[f]) {
            std::string port = "rx_f" + std::to_string(rx.peer);
            if (rx.ratio == 1) {
                for (size_t x = 0; x < rx.nets->size(); x++) {
                    xfer << "  assign " << ref((*rx.nets)[x]) << " = " << port << "[" << x << "];\n";
                    driven[(*rx.nets)[x]] = 1;
                }
                continue;
            }
            ratios.insert(rx.ratio);
            int sh = rx.pins - rx.direct;
            for (int x = 0; x < rx.direct; x++) {
                xfer << "  assign " << ref((*rx.nets)[x]) << " = " << port << "[" << x << "];\n";
                driven[(*rx.nets)[x]] = 1;
            }
            // The wires carry, one cycle later, the turn the sender's counter named.
            xfer << "  always @(posedge emu_clk)\n    case (tph" << rx.ratio << ")\n";
            for (int s = 0; s < rx.ratio; s++) {
                xfer << "      " << (s + 1) % rx.ratio << ": begin";
                for (int j = 0; j < sh; j++) {
                    size_t x = (size_t)rx.direct + (size_t)s * sh + j;
                    if (x >= rx.nets->size()) break;
                    int n = (*rx.nets)[x];
                    xfer << " " << ref(n) << " <= " << port << "[" << rx.direct + j << "];";
                    isReg[n] = 1;
                    driven[n] = 1;
                }
                xfer << " end\n";
            }
            xfer << "      default: ;\n    endcase\n";
        }

        // Ports of the design that live in this FPGA.
        std::ostringstream io;
        bool hold = d.dc >= 1 && !piOf[f].empty();
        if (hold)
            io << "  // The inputs change only after the clock event they arrived with has settled.\n"
               << "  reg [" << piOf[f].size() - 1 << ":0] pi_h;\n"
               << "  always @(posedge emu_clk) if (emu_rst | emu_hold) pi_h <= pi;\n";
        for (size_t x = 0; x < piOf[f].size(); x++) {
            int n = d.nl.ports[piOf[f][x]].net;
            io << "  assign " << ref(n) << " = " << (hold ? "pi_h[" : "pi[") << x << "];\n";
            driven[n] = 1;
        }
        for (size_t x = 0; x < poOf[f].size(); x++)
            io << "  assign po[" << x << "] = " << ref(d.nl.ports[poOf[f][x]].net) << ";\n";

        // Probes: one sample of every probed net per event, in a ring buffer.
        std::ostringstream dbg;
        bool debug = hasDebug(f);
        int np = (int)probesOf[f].size(), aw = traceAw();
        if (debug) {
            int words = std::max(1, (np + 31) / 32);
            dbg << "  reg [" << aw - 1 << ":0] trc_wp;\n  reg [" << aw << ":0] trc_n;\n"
                << "  assign dbg_wp = trc_wp;\n  assign dbg_count = trc_n;\n";
            std::string stop = "1'b0";
            if (f == 0 && triggerNet >= 0) {
                // The trigger stops the capture in every FPGA at the same event.
                dbg << "  reg [" << aw << ":0] trc_post;\n  reg trc_trig;\n"
                    << "  assign dbg_stopped = trc_trig & (trc_post == 0);\n";
                stop = "dbg_stopped";
            }
            dbg << "  wire trc_we = emu_ev & emu_trace_en & ~" << (f == 0 ? stop : std::string("dbg_stop_in")) << ";\n"
                << "  always @(posedge emu_clk)\n"
                << "    if (emu_rst) begin trc_wp <= 0; trc_n <= 0; end\n"
                << "    else if (trc_we) begin\n"
                << "      trc_wp <= trc_wp + 1'b1;\n"
                << "      if (!trc_n[" << aw << "]) trc_n <= trc_n + 1'b1;\n"
                << "    end\n";
            if (f == 0 && triggerNet >= 0)
                dbg << "  always @(posedge emu_clk)\n"
                    << "    if (emu_rst) begin trc_trig <= 1'b0; trc_post <= " << triggerPost << "; end\n"
                    << "    else if (trc_we) begin\n"
                    << "      if (dbg_trig_in) trc_trig <= 1'b1;\n"
                    << "      if ((trc_trig | dbg_trig_in) && trc_post != 0) trc_post <= trc_post - 1'b1;\n"
                    << "    end\n";
            dbg << "  wire [" << words * 32 - 1 << ":0] trc_q;\n";
            if (np) {
                dbg << "  reg [" << np - 1 << ":0] trc [0:" << (1 << aw) - 1 << "];\n  reg [" << np - 1 << ":0] trc_r;\n"
                    << "  always @(posedge emu_clk) begin\n    if (trc_we) trc[trc_wp] <= {";
                for (int x = np - 1; x >= 0; x--) dbg << ref(probes[probesOf[f][x]].net) << (x ? ", " : "");
                dbg << "};\n    trc_r <= trc[dbg_idx];\n  end\n  assign trc_q = trc_r;\n";
            } else {
                dbg << "  assign trc_q = 0;\n";
            }
            if (f == 0)
                dbg << "  reg [63:0] trc_t [0:" << (1 << aw) - 1 << "];\n  reg [63:0] trc_tr;\n"
                    << "  always @(posedge emu_clk) begin\n    if (trc_we) trc_t[trc_wp] <= emu_time_w;\n"
                    << "    trc_tr <= trc_t[dbg_idx];\n  end\n"
                    << "  assign dbg_rdata = (dbg_word == 8'd255) ? trc_tr[63:32] : (dbg_word == 8'd254) ? trc_tr[31:0] : "
                       "trc_q[dbg_word*32 +: 32];\n";
            else
                dbg << "  assign dbg_rdata = trc_q[dbg_word*32 +: 32];\n";
        }

        // Edge detectors: at each tick, which of the clock nets here moved since the last tick.
        std::ostringstream det;
        for (auto& [key, dt] : dets) {
            det << "  reg pv_" << key << ";\n  always @(posedge emu_clk) if (emu_rst | emu_tick) pv_" << key << " <= " << dt.sig
                << ";\n";
            // === and !== so that simulation treats an unknown clock the way
            // Verilog's posedge does; in hardware these are == and !=.
            if (dt.pos)
                det << "  wire ce_" << key << "_p = emu_tick & ~emu_rst & ((" << dt.sig << " === 1'b1) ? (pv_" << key
                    << " !== 1'b1) : ((pv_" << key << " === 1'b0) & (" << dt.sig << " !== 1'b0)));\n";
            if (dt.neg)
                det << "  wire ce_" << key << "_n = emu_tick & ~emu_rst & ((" << dt.sig << " === 1'b0) ? (pv_" << key
                    << " !== 1'b0) : ((pv_" << key << " === 1'b1) & (" << dt.sig << " !== 1'b1)));\n";
        }

        // The module.
        o << "// " << top << " on " << board.name << ": FPGA " << board.fpgas[f].name << ". Written by memu.\n"
          << "module " << fpgaMod(f) << " (\n  input emu_clk, input emu_rst, input emu_run";
        int nc = NC();
        if (f == 0)
            o << ",\n  output emu_ready, output [" << nc - 1 << ":0] emu_pos_o, output [" << nc - 1
              << ":0] emu_neg_o, output [63:0] emu_time_o,\n  output [" << nc - 1 << ":0] emu_npos_o, output [" << nc - 1
              << ":0] emu_nneg_o, output [63:0] emu_ntime_o";
        if (!piOf[f].empty()) o << ",\n  input [" << piOf[f].size() - 1 << ":0] pi";
        if (!poOf[f].empty()) o << ",\n  output [" << poOf[f].size() - 1 << ":0] po";
        for (auto& t : txOf[f]) o << ",\n  output [" << t.pins - 1 << ":0] tx_f" << t.peer;
        for (auto& r : rxOf[f]) o << ",\n  input [" << r.pins - 1 << ":0] rx_f" << r.peer;
        if (debug) {
            o << ",\n  input emu_trace_en, input [" << aw - 1 << ":0] dbg_idx, input [7:0] dbg_word,\n"
              << "  output [31:0] dbg_rdata, output [" << aw << ":0] dbg_count, output [" << aw - 1 << ":0] dbg_wp";
            if (f == 0 && triggerNet >= 0) o << ", input dbg_trig_in, output dbg_stopped";
            if (f != 0) o << ", input dbg_stop_in";
        }
        if (triggerNet >= 0 && f == triggerFpga) o << ",\n  output dbg_trig_out";
        o << ");\n";
        o << "  wire emu_tick, emu_ev, emu_hold, emu_ready_w;\n  wire [" << nc - 1 << ":0] emu_pos, emu_neg, emu_clkv;\n"
          << "  wire [" << nc - 1 << ":0] emu_npos, emu_nneg;\n  wire [63:0] emu_time_w, emu_ntime_w;\n"
          << "  " << top << "_emu_clockgen emu_cg (.emu_clk(emu_clk), .emu_rst(emu_rst), .emu_run(emu_run), "
             ".emu_tick(emu_tick),\n    .emu_ready(emu_ready_w), .emu_ev(emu_ev), .emu_hold(emu_hold), .emu_pos(emu_pos), "
             ".emu_neg(emu_neg),\n    .emu_clkv(emu_clkv), .emu_npos(emu_npos), .emu_nneg(emu_nneg), .emu_ntime(emu_ntime_w), "
             ".emu_time(emu_time_w));\n";
        if (f == 0)
            o << "  assign emu_ready = emu_ready_w;\n  assign emu_pos_o = emu_pos;\n  assign emu_neg_o = emu_neg;\n"
              << "  assign emu_time_o = emu_time_w;\n  assign emu_npos_o = emu_npos;\n  assign emu_nneg_o = emu_nneg;\n"
              << "  assign emu_ntime_o = emu_ntime_w;\n";
        if (needClkN) o << "  wire emu_clk_n = ~emu_clk;\n";
        for (int r : ratios) {
            int w = bitsFor(r - 1);
            o << "  reg [" << w - 1 << ":0] tph" << r << ";\n  always @(posedge emu_clk) if (emu_rst | (tph" << r
              << " == " << r - 1 << ")) tph" << r << " <= 0; else tph" << r << " <= tph" << r << " + 1'b1;\n";
        }
        std::string trigOut;
        if (triggerNet >= 0 && f == triggerFpga) trigOut = "  assign dbg_trig_out = " + ref(triggerNet) + ";\n";
        std::sort(usedList.begin(), usedList.end());
        for (int n : usedList) {
            o << "  " << (isReg[n] ? "reg " : "wire ") << "n" << n;
            if (d.clockOfNet[n] >= 0 && !driven[n])
                o << " = " << (d.strobe ? std::string("1'b0") : "emu_clkv[" + std::to_string(d.clockOfNet[n]) + "]");
            o << ";  // " << d.nl.nets[n] << "\n";
        }
        o << decl.str() << io.str() << xfer.str() << det.str() << holdAsg.str() << body.str() << dbg.str() << trigOut << "endmodule\n";
        return o.str();
    }

    // -------------------------------------------------------------- the board

    std::string bitRef(int port) const {
        auto& pt = d.nl.ports[port];
        return pt.bus.empty() ? vname(pt.name) : vname(pt.bus) + "[" + std::to_string(pt.bit) + "]";
    }

    std::string portDecl(const PortGroup& g, const std::string& dir, const std::string& prefix = "") const {
        std::string s = dir + " ";
        if (g.ranged) s += "[" + std::to_string(g.hi) + ":" + std::to_string(g.lo) + "] ";
        return s + vname(prefix + g.name);
    }

    // The board: the FPGAs and the wires between them. Its ports are the
    // design's, without the clocks (the generator makes those), plus the
    // emulation controls.
    std::string wrapper() const {
        std::ostringstream o;
        int nc = NC(), aw = traceAw();
        o << "// " << top << " on " << board.name << ": the FPGAs wired as the board wires them. Written by memu.\n"
          << "//   emu_ready  high in the last cycle of an event: sample the outputs and change the\n"
          << "//              inputs at that emu_clk edge (the design then runs its next clock event)\n"
          << "//   emu_run    low holds the design there (a transactor waiting for its host)\n"
          << "//   emu_next_pos/neg/time  the clock edges and the time the next event will have, whether\n"
          << "//              or not emu_run lets it start (a transactor decides on these)\n"
          << "//   emu_time   in units of " << unitPs << " ps\n"
          << "//   emu_pos/emu_neg  the clock edges of the event that starts at this edge:";
        for (size_t i = 0; i < d.clocks.size(); i++) o << " [" << i << "] " << d.clocks[i].name;
        o << "\nmodule " << top << "_emu (\n  input emu_clk, input emu_rst, input emu_run,\n"
          << "  output emu_ready, output [" << nc - 1 << ":0] emu_pos, output [" << nc - 1
          << ":0] emu_neg, output [63:0] emu_time,\n  output [" << nc - 1 << ":0] emu_next_pos, output [" << nc - 1
          << ":0] emu_next_neg, output [63:0] emu_next_time";
        if (!probes.empty()) {
            o << ",\n  input emu_trace_en, input [7:0] emu_dbg_fpga, input [" << aw - 1
              << ":0] emu_dbg_idx, input [7:0] emu_dbg_word,\n  output [31:0] emu_dbg_rdata, output [" << aw
              << ":0] emu_dbg_count, output [" << aw - 1 << ":0] emu_dbg_wp";
            if (triggerNet >= 0) o << ", output emu_dbg_stopped";
        }
        for (auto& g : groups)
            if (g.clock < 0) o << ",\n  " << portDecl(g, g.in ? "input" : "output");
        o << ");\n";
        for (auto& l : pl.links) {
            if (!l.ab.empty()) o << "  wire [" << l.pinsAB - 1 << ":0] l_" << l.a << "_" << l.b << ";\n";
            if (!l.ba.empty()) o << "  wire [" << l.pinsBA - 1 << ":0] l_" << l.b << "_" << l.a << ";\n";
        }
        if (!probes.empty()) {
            for (int f = 0; f < k(); f++)
                if (hasDebug(f)) o << "  wire [31:0] dbg_rdata" << f << ";\n";
            o << "  assign emu_dbg_rdata =";
            for (int f = 1; f < k(); f++)
                if (hasDebug(f)) o << " (emu_dbg_fpga == 8'd" << f << ") ? dbg_rdata" << f << " :";
            o << " dbg_rdata0;\n";
            if (triggerNet >= 0) o << "  wire dbg_trig;\n";
        }
        for (int f = 0; f < k(); f++) {
            o << "  " << fpgaMod(f) << " f" << f << " (.emu_clk(emu_clk), .emu_rst(emu_rst), .emu_run(emu_run)";
            if (f == 0) o << ",\n    .emu_ready(emu_ready), .emu_pos_o(emu_pos), .emu_neg_o(emu_neg), .emu_time_o(emu_time),\n    .emu_npos_o(emu_next_pos), .emu_nneg_o(emu_next_neg), .emu_ntime_o(emu_next_time)";
            auto cat = [&](const std::vector<int>& ports) {
                std::string s = "{";
                for (size_t x = ports.size(); x-- > 0;) s += bitRef(ports[x]) + (x ? ", " : "");
                return s + "}";
            };
            if (!piOf[f].empty()) o << ",\n    .pi(" << cat(piOf[f]) << ")";
            if (!poOf[f].empty()) o << ",\n    .po(" << cat(poOf[f]) << ")";
            for (auto& t : txOf[f]) o << ",\n    .tx_f" << t.peer << "(l_" << f << "_" << t.peer << ")";
            for (auto& r : rxOf[f]) o << ",\n    .rx_f" << r.peer << "(l_" << r.peer << "_" << f << ")";
            if (hasDebug(f)) {
                o << ",\n    .emu_trace_en(emu_trace_en), .dbg_idx(emu_dbg_idx), .dbg_word(emu_dbg_word), .dbg_rdata(dbg_rdata"
                  << f << ")";
                if (f == 0) o << ", .dbg_count(emu_dbg_count), .dbg_wp(emu_dbg_wp)";
                else o << ", .dbg_count(), .dbg_wp()";
                if (f == 0 && triggerNet >= 0) o << ", .dbg_trig_in(dbg_trig), .dbg_stopped(emu_dbg_stopped)";
                if (f != 0) o << ", .dbg_stop_in(" << (triggerNet >= 0 ? "emu_dbg_stopped" : "1'b0") << ")";
            }
            if (triggerNet >= 0 && f == triggerFpga) o << ",\n    .dbg_trig_out(dbg_trig)";
            o << ");\n";
        }
        o << "endmodule\n";
        return o.str();
    }

    // ---------------------------------------------------------- the testbench

    static bool looksLikeReset(const std::string& name, bool& activeLow) {
        std::string s;
        for (char c : name) s += (char)tolower((unsigned char)c);
        if (s.find("rst") == std::string::npos && s.find("reset") == std::string::npos) return false;
        activeLow = s.back() == 'n' || s.back() == 'b' || s.find("rstn") != std::string::npos ||
                    s.find("resetn") != std::string::npos || s.find("rst_n") != std::string::npos ||
                    s.find("reset_n") != std::string::npos || s[0] == 'n';
        return true;
    }

    // Runs the netlist memu was given and the emulation side by side, on the
    // same random inputs, and compares every output at every event.
    std::string testbench() const {
        std::ostringstream o;
        int nc = NC(), aw = traceAw();
        o << "// " << top << ": the netlist and its emulation on the same inputs. Written by memu.\n"
          << "//   +slots=N   events to run (2000)      +seed=N   random seed (1)\n"
          << "//   +trace=F   write the probe samples to F when done (memu -trace turns it into a VCD)\n"
          << "`timescale 1ns/1ps\nmodule " << top << "_emu_tb;\n"
          << "  reg emu_clk = 1'b0, emu_rst = 1'b1, emu_run = 1'b0, done = 1'b0;\n  always #5 emu_clk = ~emu_clk;\n"
          << "  wire emu_ready;\n  wire [" << nc - 1 << ":0] emu_pos, emu_neg;\n  reg [" << nc - 1
          << ":0] pos_s, neg_s;\n  wire [63:0] emu_time;\n";
        for (size_t i = 0; i < d.clocks.size(); i++)
            o << "  reg rc" << i << " = 1'b" << (!d.clocks[i].livePos && d.clocks[i].liveNeg ? 1 : 0) << ";  // "
              << d.clocks[i].name << "\n";
        for (auto& g : groups) {
            if (g.clock >= 0) continue;
            if (g.in) o << "  " << portDecl(g, "reg") << ";\n";
            else o << "  " << portDecl(g, "wire", "gold_") << ";\n  " << portDecl(g, "wire", "emu_") << ";\n";
        }
        if (!probes.empty())
            o << "  reg emu_trace_en = 1'b1;\n  reg [7:0] emu_dbg_fpga = 0, emu_dbg_word = 0;\n  reg [" << aw - 1
              << ":0] emu_dbg_idx = 0;\n  wire [31:0] emu_dbg_rdata;\n  wire [" << aw << ":0] emu_dbg_count;\n  wire ["
              << aw - 1 << ":0] emu_dbg_wp;\n";
        o << "  " << vname(top) << " gold (";
        bool first = true;
        for (auto& g : groups) {
            o << (first ? "" : ", ") << "." << vname(g.name) << "(";
            if (g.clock >= 0) o << "rc" << g.clock;
            else o << vname((g.in ? "" : "gold_") + g.name);
            o << ")";
            first = false;
        }
        o << ");\n  " << top << "_emu emu (.emu_clk(emu_clk), .emu_rst(emu_rst), .emu_run(emu_run), .emu_ready(emu_ready),\n"
          << "    .emu_pos(emu_pos), .emu_neg(emu_neg), .emu_time(emu_time),\n"
          << "    .emu_next_pos(), .emu_next_neg(), .emu_next_time()";
        if (!probes.empty()) {
            o << ",\n    .emu_trace_en(emu_trace_en), .emu_dbg_fpga(emu_dbg_fpga), .emu_dbg_idx(emu_dbg_idx), "
                 ".emu_dbg_word(emu_dbg_word),\n    .emu_dbg_rdata(emu_dbg_rdata), .emu_dbg_count(emu_dbg_count), "
                 ".emu_dbg_wp(emu_dbg_wp)";
            if (triggerNet >= 0) o << ", .emu_dbg_stopped()";
        }
        for (auto& g : groups)
            if (g.clock < 0) o << ",\n    ." << vname(g.name) << "(" << vname((g.in ? "" : "emu_") + g.name) << ")";
        o << ");\n\n  integer slots, seed, slot, events, bad, known, i, t, trf, w;\n  string trname;\n";

        // New random inputs. Resets are held at first, then pulse now and then.
        o << "  task drive;\n    begin\n";
        for (auto& g : groups) {
            if (g.clock >= 0 || !g.in) continue;
            bool low = false;
            if (!g.ranged && looksLikeReset(g.name, low)) {
                o << "      " << vname(g.name) << " = (slot < 4 || ($random(seed) & 127) == 0) ? 1'b" << (low ? 0 : 1)
                  << " : 1'b" << (low ? 1 : 0) << ";\n";
                continue;
            }
            int words = (g.width() + 31) / 32;
            o << "      " << vname(g.name) << " = ";
            if (words == 1) {
                o << "$random(seed);\n";
            } else {
                o << "{";
                for (int x = 0; x < words; x++) o << (x ? ", " : "") << "$random(seed)";
                o << "};\n";
            }
        }
        o << "    end\n  endtask\n";
        // Where the netlist's value is known, the emulation's must be the same.
        o << "  task check;\n    begin\n";
        for (auto& g : groups) {
            if (g.in) continue;
            std::string r = vname("gold_" + g.name), e = vname("emu_" + g.name);
            if (g.ranged)
                o << "      for (i = " << g.lo << "; i <= " << g.hi << "; i = i + 1)\n        if (" << r << "[i] !== 1'bx && "
                  << r << "[i] !== 1'bz) begin\n          known = known + 1;\n          if (" << e << "[i] !== " << r
                  << "[i]) begin\n          bad = bad + 1;\n"
                  << "          if (bad <= 20) $display(\"memu: MISMATCH at event %0d (time %0d): " << g.name
                  << "[%0d] netlist %b emulation %b\", slot, emu_time, i, " << r << "[i], " << e << "[i]);\n          end\n        end\n";
            else
                o << "      if (" << r << " !== 1'bx && " << r << " !== 1'bz) begin\n        known = known + 1;\n        if (" << e
                  << " !== " << r << ") begin\n        bad = bad + 1;\n        if (bad <= 20) $display(\"memu: MISMATCH at event %0d (time %0d): "
                  << g.name << " netlist %b emulation %b\", slot, emu_time, " << r << ", " << e << ");\n        end\n      end\n";
        }
        o << "    end\n  endtask\n\n"
          << "  initial begin\n    slots = 2000; seed = 1; slot = 0; events = 0; bad = 0; known = 0;\n"
          << "    i = $value$plusargs(\"slots=%d\", slots);\n    i = $value$plusargs(\"seed=%d\", seed);\n"
          << "    drive;\n    repeat (" << 8 + 2 * pl.B * N() << ") @(posedge emu_clk);\n    #1 emu_rst = 1'b0;\n  end\n\n"
          << "  // In the middle of a cycle everything has settled: when the emulation is at the\n"
          << "  // end of an event, compare, then at the clock edge move the netlist's clocks the\n"
          << "  // way the emulation is about to, and after it change the inputs of both.\n"
          << "  always @(negedge emu_clk) if (!emu_rst && !done) begin\n"
          << "    if (emu_ready && emu_run) begin\n"
          << "      check;\n      pos_s = emu_pos; neg_s = emu_neg;\n      @(posedge emu_clk);\n      #1;\n";
        for (size_t i = 0; i < d.clocks.size(); i++)
            o << "      if (pos_s[" << i << "]) rc" << i << " = 1'b1;\n      if (neg_s[" << i << "]) rc" << i << " = 1'b0;\n";
        o << "      if (pos_s != 0 || neg_s != 0) events = events + 1;\n"
          << "      slot = slot + 1;\n      #1 drive;\n";
        bool anyDead = false;
        for (size_t i = 0; i < d.clocks.size(); i++) {
            auto& c = d.clocks[i];
            if (c.livePos && c.liveNeg) continue;
            if (!anyDead) o << "      #1;  // the edges nothing in the design listens to\n";
            anyDead = true;
            if (c.livePos) o << "      rc" << i << " = 1'b0;\n";
            else if (c.liveNeg) o << "      rc" << i << " = 1'b1;\n";
        }
        o << "    end else begin\n      @(posedge emu_clk);\n      #1;\n    end\n"
          << "    if (slot >= slots) done = 1'b1;\n"
          << "    // The host stalls the emulation now and then.\n"
          << "    emu_run = !done && ($random(seed) & 7) != 0;\n"
          << "    if (done) begin\n";
        if (!probes.empty()) {
            o << "      emu_trace_en = 1'b0;\n"
              << "      if ($value$plusargs(\"trace=%s\", trname)) begin\n        trf = $fopen(trname, \"w\");\n"
              << "        for (t = 0; t < emu_dbg_count; t = t + 1) begin\n"
              << "          emu_dbg_idx = emu_dbg_count[" << aw << "] ? emu_dbg_wp + t : t;\n"
              << "          emu_dbg_fpga = 0; emu_dbg_word = 8'd255; @(posedge emu_clk); @(negedge emu_clk);\n"
              << "          $fwrite(trf, \"%08x\", emu_dbg_rdata); emu_dbg_word = 8'd254; #1 $fwrite(trf, \"%08x\", emu_dbg_rdata);\n";
            for (int f = 0; f < k(); f++) {
                int words = ((int)probesOf[f].size() + 31) / 32;
                if (!words) {
                    o << "          $fwrite(trf, \" -\");\n";
                    continue;
                }
                o << "          emu_dbg_fpga = " << f << "; $fwrite(trf, \" \");\n"
                  << "          for (w = " << words - 1 << "; w >= 0; w = w - 1) begin\n"
                  << "            emu_dbg_word = w; #1 $fwrite(trf, \"%08x\", emu_dbg_rdata);\n"
                  << "          end\n";
            }
            o << "          $fwrite(trf, \"\\n\");\n        end\n        $fclose(trf);\n      end\n";
        }
        o << "      if (bad == 0 && known != 0)\n        $display(\"memu: PASS, %0d events (%0d with a clock edge), %0d output bits compared, netlist and "
             "emulation agree\", slot, events, known);\n      else if (bad == 0) $display(\"memu: FAIL, no output of the netlist ever had a known value\");\n"
          << "      else $display(\"memu: FAIL, %0d mismatches in %0d events\", bad, slot);\n"
          << "      $finish;\n    end\n  end\nendmodule\n";
        return o.str();
    }

    // The probe map: which bit of which FPGA's sample is which net.
    std::string probeMap() const {
        std::ostringstream o;
        o << "memu-probes 1\ntop " << top << "\nunit_ps " << unitPs << "\nfpgas " << k() << "\n";
        for (auto& p : probes) o << "probe " << p.fpga << " " << p.bit << " " << p.name << "\n";
        return o.str();
    }
};

}  // namespace emu
