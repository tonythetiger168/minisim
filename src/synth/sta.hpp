#pragma once
// sta.hpp - the timing msynth needs to optimize and report: arrival times
// through the mapped netlist with the library's NLDM tables, rise and fall
// kept apart, against the clock period and the input / output delays.
//
// Clocks are ideal (no clock network delay), nets carry the pin capacitance
// of their sinks plus set_load on outputs, and every flop is assumed to be
// on the one clock. msta is the sign-off timer; this is the estimate the
// synthesis loop runs on.
#include <cstdio>

#include "map.hpp"

namespace synth {

struct Constraints {
    struct Clock {
        std::string name;
        double period = 0;
        std::vector<std::string> ports;
    };
    std::vector<Clock> clocks;
    std::map<std::string, double> inputDelay, outputDelay, load;   // by port bit name
    double defInputDelay = 0, defOutputDelay = 0, defLoad = 0;
    double uncertainty = 0;
    double inputSlew = 0.01;
    double maxArea = -1;
    int maxFanout = 12;   // set_max_fanout; high-fanout nets are buffered down to it

    const Clock* clock() const { return clocks.empty() ? nullptr : &clocks[0]; }
    bool isClockPort(const std::string& bit) const {
        for (auto& c : clocks)
            for (auto& p : c.ports)
                if (p == bit) return true;
        return false;
    }
};

struct Timing {
    struct NetT {
        double arr[2] = {0, 0};    // [0] rise, [1] fall
        double slew[2] = {0, 0};
        int fromInst = -1;         // the instance that drives it
        int fromPin[2] = {-1, -1}; // the input pin of that instance the worst edge came through
        int fromEdge[2] = {0, 0};  // and that pin's edge
        bool valid = false;        // reached from a start point
        bool clock = false;        // part of the clock network
    };
    struct End {
        std::string name;     // flop instance / output port bit
        int net = -1, inst = -1, pin = -1;
        int edge = 0;
        double arrival = 0, required = 0, slack = 0, setup = 0;
        bool constrained = false;
    };
    std::vector<NetT> nets;
    std::vector<double> load;
    std::vector<End> ends;   // sorted, worst slack first
    double wns = 0, tns = 0, critical = 0;
    int violations = 0;
    bool constrained = false;
};

inline Timing analyze(const Netlist& nl, const Constraints& sdc) {
    Timing t;
    size_t N = nl.nets.size();
    t.nets.assign(N, {});
    t.load.assign(N, 0);
    for (auto& in : nl.insts)
        for (size_t p = 0; p < in.pins.size(); p++)
            if (in.pins[p] >= 0 && in.cell->pins[p].dir == liberty::Dir::In) t.load[in.pins[p]] += in.cell->pins[p].cap;
    auto portVal = [](const std::map<std::string, double>& m, const std::string& k, double def) {
        auto it = m.find(k);
        return it == m.end() ? def : it->second;
    };
    for (auto& pb : nl.ports)
        for (size_t i = 0; i < pb.nets.size(); i++) {
            std::string bn = bitName(pb.name, pb.msb, pb.lsb, i, pb.nets.size());
            if (pb.out) t.load[pb.nets[i]] += portVal(sdc.load, bn, sdc.defLoad);
            else {
                auto& n = t.nets[pb.nets[i]];
                n.valid = true;
                n.clock = sdc.isClockPort(bn);
                double dly = n.clock ? 0 : portVal(sdc.inputDelay, bn, sdc.defInputDelay);
                n.arr[0] = n.arr[1] = dly;
                n.slew[0] = n.slew[1] = sdc.inputSlew;
            }
        }
    // alias nets (assign out = net) carry their source's load
    std::map<int, int> aliasOf;
    for (auto& pb : nl.ports)
        if (pb.out)
            for (size_t i = 0; i < pb.nets.size(); i++)
                for (auto& [bn, src] : nl.assigns)
                    if (bn == nl.nets[pb.nets[i]]) {
                        aliasOf[pb.nets[i]] = src;
                        t.load[src] += t.load[pb.nets[i]];
                    }
    auto propagate = [&](const Inst& in, int ii) {
        const liberty::Cell& c = *in.cell;
        for (size_t op = 0; op < c.pins.size(); op++) {
            if (c.pins[op].dir != liberty::Dir::Out || in.pins[op] < 0) continue;
            auto& o = t.nets[in.pins[op]];
            double ld = t.load[in.pins[op]];
            o.fromInst = ii;
            bool any = false;
            double best[2] = {-1e30, -1e30};
            bool isClk = false;
            for (auto& a : c.arcs) {
                if (a.to != c.pins[op].name) continue;
                int ip = LibInfo::pinIndex(c, a.related);
                if (ip < 0 || in.pins[ip] < 0) continue;
                const auto& src = t.nets[in.pins[ip]];
                if (in.seq) {
                    if (!a.isEdge()) continue;   // clock to Q only; async arcs are not timed here
                    for (int e = 0; e < 2; e++) {
                        const liberty::Table& dt = e == 0 ? a.cellRise : a.cellFall;
                        const liberty::Table& st = e == 0 ? a.riseTrans : a.fallTrans;
                        if (dt.empty()) continue;
                        double arr = dt.lookup(sdc.inputSlew, ld);
                        if (arr > best[e]) {
                            best[e] = arr;
                            o.arr[e] = arr;
                            o.slew[e] = st.empty() ? 0 : st.lookup(sdc.inputSlew, ld);
                            o.fromPin[e] = ip;
                            o.fromEdge[e] = a.rising() ? 0 : 1;
                        }
                    }
                    any = true;
                    continue;
                }
                if (a.type != liberty::ArcType::Comb || !src.valid) continue;
                if (src.clock) isClk = true;
                for (int e = 0; e < 2; e++) {       // output edge
                    if ((e == 0 && a.fallOnly) || (e == 1 && a.riseOnly)) continue;
                    const liberty::Table& dt = e == 0 ? a.cellRise : a.cellFall;
                    const liberty::Table& st = e == 0 ? a.riseTrans : a.fallTrans;
                    for (int ie = 0; ie < 2; ie++) {   // input edge
                        if (a.sense == liberty::Sense::Positive && ie != e) continue;
                        if (a.sense == liberty::Sense::Negative && ie == e) continue;
                        double arr = src.arr[ie] + (dt.empty() ? 0 : dt.lookup(src.slew[ie], ld));
                        if (arr > best[e]) {
                            best[e] = arr;
                            o.arr[e] = arr;
                            o.slew[e] = st.empty() ? src.slew[ie] : st.lookup(src.slew[ie], ld);
                            o.fromPin[e] = ip;
                            o.fromEdge[e] = ie;
                        }
                        any = true;
                    }
                }
            }
            if (!any && !in.seq) {
                // no arcs (a tie cell, or a library without timing): zero delay from the latest input
                for (size_t ip = 0; ip < c.pins.size(); ip++) {
                    if (c.pins[ip].dir != liberty::Dir::In || in.pins[ip] < 0) continue;
                    const auto& src = t.nets[in.pins[ip]];
                    if (!src.valid) continue;
                    for (int e = 0; e < 2; e++)
                        if (src.arr[e] > best[e]) {
                            best[e] = o.arr[e] = src.arr[e];
                            o.slew[e] = src.slew[e];
                            o.fromPin[e] = (int)ip;
                            o.fromEdge[e] = e;
                        }
                    any = true;
                }
            }
            o.valid = any || in.seq;
            o.clock = isClk && !in.seq;
            if (o.clock) o.arr[0] = o.arr[1] = 0;   // ideal clock
        }
    };
    for (size_t i = 0; i < nl.insts.size(); i++)
        if (nl.insts[i].seq) propagate(nl.insts[i], (int)i);
    // gates in topological order (the netlist need not be stored that way)
    {
        std::vector<int> driver(N, -1), waiting(nl.insts.size(), 0), ready;
        std::vector<std::vector<int>> sinks(N);
        for (size_t i = 0; i < nl.insts.size(); i++) {
            const Inst& in = nl.insts[i];
            if (in.seq) continue;
            for (size_t p = 0; p < in.pins.size(); p++)
                if (in.pins[p] >= 0 && in.cell->pins[p].dir == liberty::Dir::Out) driver[in.pins[p]] = (int)i;
        }
        for (size_t i = 0; i < nl.insts.size(); i++) {
            const Inst& in = nl.insts[i];
            if (in.seq) continue;
            for (size_t p = 0; p < in.pins.size(); p++)
                if (in.pins[p] >= 0 && in.cell->pins[p].dir == liberty::Dir::In && driver[in.pins[p]] >= 0) {
                    waiting[i]++;
                    sinks[in.pins[p]].push_back((int)i);
                }
            if (!waiting[i]) ready.push_back((int)i);
        }
        for (size_t k = 0; k < ready.size(); k++) {
            int i = ready[k];
            const Inst& in = nl.insts[i];
            propagate(in, i);
            for (size_t p = 0; p < in.pins.size(); p++)
                if (in.pins[p] >= 0 && in.cell->pins[p].dir == liberty::Dir::Out)
                    for (int s : sinks[in.pins[p]])
                        if (--waiting[s] == 0) ready.push_back(s);
        }
    }

    const Constraints::Clock* clk = sdc.clock();
    t.constrained = clk != nullptr;
    double period = clk ? clk->period : 0;
    auto addEnd = [&](Timing::End e) {
        const auto& n = t.nets[e.net];
        if (!n.valid || n.clock || e.net < 2) return;
        e.edge = n.arr[0] + 0 >= n.arr[1] ? 0 : 1;
        e.arrival = n.arr[e.edge];
        e.constrained = clk != nullptr;
        t.ends.push_back(e);
    };
    for (size_t i = 0; i < nl.insts.size(); i++) {
        const Inst& in = nl.insts[i];
        if (!in.seq) continue;
        const liberty::Cell& c = *in.cell;
        if (c.ffs.empty()) continue;
        std::string v;
        bool neg;
        if (!LibInfo::simpleVar(c.ffs[0].nextState, v, neg)) continue;
        int dp = LibInfo::pinIndex(c, v);
        if (dp < 0 || in.pins[dp] < 0) continue;
        Timing::End e;
        e.name = in.name;
        e.net = in.pins[dp];
        e.inst = (int)i;
        e.pin = dp;
        const auto& n = t.nets[e.net];
        // setup per data edge; the worse of (arrival + setup)
        double su[2] = {0, 0};
        for (auto& a : c.arcs)
            if (a.to == c.pins[dp].name && (a.type == liberty::ArcType::SetupRising || a.type == liberty::ArcType::SetupFalling)) {
                if (!a.riseCons.empty()) su[0] = std::max(su[0], a.riseCons.constraint(n.slew[0], sdc.inputSlew));
                if (!a.fallCons.empty()) su[1] = std::max(su[1], a.fallCons.constraint(n.slew[1], sdc.inputSlew));
            }
        if (!n.valid || n.clock || e.net < 2) continue;
        e.edge = n.arr[0] + su[0] >= n.arr[1] + su[1] ? 0 : 1;
        e.arrival = n.arr[e.edge];
        e.setup = su[e.edge];
        e.constrained = clk != nullptr;
        e.required = period - sdc.uncertainty - e.setup;
        t.ends.push_back(e);
    }
    for (auto& pb : nl.ports) {
        if (!pb.out) continue;
        for (size_t i = 0; i < pb.nets.size(); i++) {
            Timing::End e;
            e.name = bitName(pb.name, pb.msb, pb.lsb, i, pb.nets.size());
            e.net = pb.nets[i];
            if (auto it = aliasOf.find(e.net); it != aliasOf.end()) e.net = it->second;
            size_t before = t.ends.size();
            addEnd(e);
            if (t.ends.size() > before)
                t.ends.back().required = period - sdc.uncertainty - portVal(sdc.outputDelay, e.name, sdc.defOutputDelay);
        }
    }
    for (auto& e : t.ends) {
        e.slack = e.required - e.arrival;
        t.critical = std::max(t.critical, e.arrival);
    }
    std::stable_sort(t.ends.begin(), t.ends.end(), [&](const Timing::End& a, const Timing::End& b) {
        if (t.constrained) return a.slack < b.slack;
        return a.arrival > b.arrival;
    });
    if (t.constrained) {
        t.wns = 0;
        for (auto& e : t.ends)
            if (e.slack < 0) {
                t.wns = std::min(t.wns, e.slack);
                t.tns += e.slack;
                t.violations++;
            }
    }
    return t;
}

// High-fanout nets get a tree of buffers, so that no driver sees more than
// `limit` loads. Clock nets (anything on a flop's clock pin) are left for
// clock tree synthesis. Returns the number of buffers added.
inline int bufferFanout(Netlist& nl, const LibInfo& lib, int limit) {
    if (lib.buf < 0 || limit < 2) return 0;
    const Gate& bg = lib.gates[lib.buf];
    struct Sink {
        int inst, pin;
    };
    size_t N = nl.nets.size();
    std::vector<std::vector<Sink>> sinks(N);
    std::vector<uint8_t> isClock(N, 0);
    for (size_t i = 0; i < nl.insts.size(); i++) {
        const Inst& in = nl.insts[i];
        for (size_t p = 0; p < in.pins.size(); p++) {
            if (in.pins[p] < 2 || in.cell->pins[p].dir != liberty::Dir::In) continue;
            sinks[in.pins[p]].push_back({(int)i, (int)p});
            if (in.seq && in.cell->pins[p].clock) isClock[in.pins[p]] = 1;
        }
    }
    for (auto& fc : lib.flops)
        for (size_t i = 0; i < nl.insts.size(); i++)
            if (nl.insts[i].cell == fc.cell && nl.insts[i].pins[fc.ck] >= 0) isClock[nl.insts[i].pins[fc.ck]] = 1;
    int added = 0;
    for (size_t n = 2; n < N; n++) {
        if (isClock[n] || (int)sinks[n].size() <= limit) continue;
        std::vector<Sink> level = sinks[n];
        while ((int)level.size() > limit) {
            std::vector<Sink> next;
            for (size_t b = 0; b < level.size(); b += limit) {
                Inst buf;
                buf.cell = bg.cell;
                buf.name = "BUF" + std::to_string(++added) + "_" + std::to_string(n);
                buf.pins.assign(bg.cell->pins.size(), -1);
                int o = nl.newNet("n" + std::to_string(nl.nets.size()));
                buf.pins[bg.out] = o;
                buf.pins[bg.in[0]] = (int)n;   // re-pointed by the level above, if there is one
                for (size_t k = b; k < std::min(level.size(), b + limit); k++) nl.insts[level[k].inst].pins[level[k].pin] = o;
                nl.insts.push_back(std::move(buf));
                next.push_back({(int)nl.insts.size() - 1, bg.in[0]});
            }
            level.swap(next);
        }
    }
    return added;
}

// The path to one endpoint, dc_shell report_timing style.
inline std::string pathReport(const Netlist& nl, const Timing& t, const Constraints& sdc, const Timing::End& e) {
    struct Step {
        std::string point;
        double at;
        int edge;
    };
    std::vector<Step> steps;
    int net = e.net, edge = e.edge;
    std::string start, startKind;
    while (true) {
        const auto& n = t.nets[net];
        if (n.fromInst < 0) {   // an input port
            start = nl.nets[net];
            startKind = "(input port" + std::string(sdc.clock() ? " clocked by " + sdc.clock()->name : "") + ")";
            steps.push_back({nl.nets[net] + " (in)", n.arr[edge], edge});
            break;
        }
        const Inst& in = nl.insts[n.fromInst];
        std::string outPin;
        for (size_t p = 0; p < in.pins.size(); p++)
            if (in.pins[p] == net && in.cell->pins[p].dir == liberty::Dir::Out) outPin = in.cell->pins[p].name;
        steps.push_back({in.name + "/" + outPin + " (" + in.cell->name + ")", n.arr[edge], edge});
        if (in.seq) {
            start = in.name;
            int cp = n.fromPin[edge];
            bool rise = n.fromEdge[edge] == 0;
            startKind = std::string("(") + (rise ? "rising" : "falling") + " edge-triggered flip-flop" +
                        (sdc.clock() ? " clocked by " + sdc.clock()->name : "") + ")";
            steps.push_back({in.name + "/" + (cp >= 0 ? in.cell->pins[cp].name : "CK") + " (" + in.cell->name + ")", 0, rise ? 0 : 1});
            break;
        }
        int ip = n.fromPin[edge];
        if (ip < 0) break;
        int ie = n.fromEdge[edge];
        net = in.pins[ip];
        edge = ie;
    }
    std::reverse(steps.begin(), steps.end());
    std::string out;
    char buf[512];
    auto line = [&](const std::string& a, const char* fmt, double x, double y, const char* tail) {
        snprintf(buf, sizeof buf, fmt, a.c_str(), x, y, tail);
        out += buf;
    };
    bool endFlop = e.inst >= 0;
    out += "  Startpoint: " + start + "\n              " + startKind + "\n";
    out += "  Endpoint: " + e.name + "\n            " +
           (endFlop ? std::string("(rising edge-triggered flip-flop") : std::string("(output port")) +
           (sdc.clock() ? " clocked by " + sdc.clock()->name : "") + ")\n";
    out += "  Path Group: " + (sdc.clock() ? sdc.clock()->name : std::string("(none)")) + "\n  Path Type: max\n\n";
    size_t wcol = 40;
    for (auto& s : steps) wcol = std::max(wcol, s.point.size() + 2);
    if (endFlop) wcol = std::max(wcol, e.name.size() + nl.insts[e.inst].cell->name.size() + 8);
    std::string fmt2 = "  %-" + std::to_string(wcol) + "s%10.4f %10.4f%s\n";
    std::string fmt1 = "  %-" + std::to_string(wcol) + "s%10s %10.4f%s\n";
    snprintf(buf, sizeof buf, ("  %-" + std::to_string(wcol) + "s%10s %10s\n").c_str(), "Point", "Incr", "Path");
    out += buf;
    out += "  " + std::string(wcol + 21, '-') + "\n";
    if (sdc.clock()) line("clock " + sdc.clock()->name + " (rise edge)", fmt2.c_str(), 0, 0, "");
    double prev = 0;
    for (auto& s : steps) {
        line(s.point, fmt2.c_str(), s.at - prev, s.at, s.edge == 0 ? " r" : " f");
        prev = s.at;
    }
    if (endFlop) {
        const Inst& in = nl.insts[e.inst];
        line(in.name + "/" + in.cell->pins[e.pin].name + " (" + in.cell->name + ")", fmt2.c_str(), 0, e.arrival, e.edge == 0 ? " r" : " f");
    } else
        line(e.name + " (out)", fmt2.c_str(), 0, e.arrival, e.edge == 0 ? " r" : " f");
    auto single = [&](const std::string& a, double v) {
        snprintf(buf, sizeof buf, fmt1.c_str(), a.c_str(), "", v, "");
        out += buf;
    };
    single("data arrival time", e.arrival);
    out += "\n";
    if (e.constrained) {
        double period = sdc.clock()->period;
        line("clock " + sdc.clock()->name + " (rise edge)", fmt2.c_str(), period, period, "");
        double at = period;
        if (sdc.uncertainty != 0) {
            at -= sdc.uncertainty;
            line("clock uncertainty", fmt2.c_str(), -sdc.uncertainty, at, "");
        }
        if (endFlop) line("library setup time", fmt2.c_str(), -e.setup, at - e.setup, "");
        else line("output external delay", fmt2.c_str(), e.required - at, e.required, "");
        single("data required time", e.required);
        out += "  " + std::string(wcol + 21, '-') + "\n";
        single("data required time", e.required);
        single("data arrival time", -e.arrival);
        out += "  " + std::string(wcol + 21, '-') + "\n";
        single(e.slack < 0 ? "slack (VIOLATED)" : "slack (MET)", e.slack);
    } else {
        out += "  " + std::string(wcol + 21, '-') + "\n  (Path is unconstrained)\n";
    }
    return out;
}

}  // namespace synth
