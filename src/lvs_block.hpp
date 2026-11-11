// lvs_block.hpp - layout versus schematic for a placed and routed block.
//
// The layout is a routed DEF with its LEF; the schematic is the gate netlist
// of the same block. The cells are the devices: what is checked is that the
// wires connect the cell pins and the block's pins the way the netlist says.
//
// extractBlock() finds the nets from the geometry alone. The net names and
// the connection lists of the DEF are not used for connectivity (they only
// give the extracted nets their names in messages): metal shapes of one
// layer that overlap or share part of a side are one net, a cut joins the
// metal under and over it, and all port shapes of one cell pin are one node.
//
// compareBlock() pairs instances and ports by name, as place and route keeps
// them, and reports each net of the netlist that is in several pieces in the
// layout (an open) and each net of the layout that joins several nets of the
// netlist (a short), with the pins involved.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "gnetlist.hpp"
#include "layout_def.hpp"

namespace lvs {

// A flat cell-level netlist, from either side.
struct BlockPin {
    std::string pin;
    int net = -1;          // -1: connected to nothing
    bool supply = false;   // a power or ground pin of the cell
};
struct BlockInst {
    std::string name, cell;
    std::vector<BlockPin> pins;
};
struct BlockPort {
    std::string name;
    int net = -1;
    bool supply = false;   // a power or ground pin of the block
};
struct BlockNetlist {
    std::string name;
    std::vector<std::string> nets;
    std::vector<BlockInst> insts;
    std::vector<BlockPort> ports;
    size_t floating = 0;   // layout: pieces of wire that reach no pin
    std::vector<std::string> notes;   // layout: nets of the DEF found in several pieces, by name
};

struct BlockResult {
    bool match = false;
    size_t insts = 0, nets = 0, ports = 0;   // compared
    size_t opens = 0, shorts = 0;
    std::vector<std::string> messages;
};

namespace detail {

struct Dsu {
    std::vector<uint32_t> p;
    uint32_t add() { p.push_back(uint32_t(p.size())); return uint32_t(p.size() - 1); }
    uint32_t find(uint32_t a) {
        while (p[a] != a) a = p[a] = p[p[a]];
        return a;
    }
    // The smaller index stays the root, so the result does not depend on the order of the calls.
    void join(uint32_t a, uint32_t b) {
        a = find(a); b = find(b);
        if (a == b) return;
        if (a < b) p[b] = a; else p[a] = b;
    }
};

struct BShape {
    phys::Rect r;
    uint32_t node;
};

// Pairs (i, j) of a[i] and b[j] that touch: with `side`, an overlap or a
// shared part of a side (not a corner); without, an overlap of the interiors.
// With same = true, b is a and each pair is given once.
template <class F>
void touching(const std::vector<BShape>& a, const std::vector<BShape>& b, bool same, bool side, F&& f) {
    if (a.empty() || b.empty()) return;
    // Sweep over x with the active rectangles of b in a map by y0.
    struct Ev { int64_t x; uint32_t idx; bool isB, start; };
    std::vector<Ev> ev;
    ev.reserve(2 * (a.size() + (same ? 0 : b.size())));
    for (uint32_t i = 0; i < a.size(); ++i) { ev.push_back({a[i].r.x0, i, false, true}); ev.push_back({a[i].r.x1, i, false, false}); }
    if (!same)
        for (uint32_t i = 0; i < b.size(); ++i) { ev.push_back({b[i].r.x0, i, true, true}); ev.push_back({b[i].r.x1, i, true, false}); }
    // Starts before ends at one x, so that abutting rectangles meet.
    std::sort(ev.begin(), ev.end(), [](const Ev& p, const Ev& q) {
        if (p.x != q.x) return p.x < q.x;
        if (p.start != q.start) return p.start;
        if (p.isB != q.isB) return p.isB;
        return p.idx < q.idx;
    });
    std::multimap<int64_t, uint32_t> act[2];   // y0 -> index, for a and for b
    int64_t tallest[2] = {0, 0};
    auto meet = [&](const phys::Rect& p, const phys::Rect& q) {
        int64_t dx = std::min(p.x1, q.x1) - std::max(p.x0, q.x0), dy = std::min(p.y1, q.y1) - std::max(p.y0, q.y0);
        if (side) return dx >= 0 && dy >= 0 && (dx > 0 || dy > 0);
        return dx > 0 && dy > 0;
    };
    for (const Ev& e : ev) {
        int me = e.isB ? 1 : 0, other = same ? 0 : 1 - me;
        const std::vector<BShape>& mine = e.isB ? b : a;
        const std::vector<BShape>& theirs = same ? a : (e.isB ? a : b);
        const phys::Rect& r = mine[e.idx].r;
        if (!e.start) {
            auto range = act[me].equal_range(r.y0);
            for (auto it = range.first; it != range.second; ++it)
                if (it->second == e.idx) { act[me].erase(it); break; }
            continue;
        }
        // Active rectangles of the other side with y0 <= r.y1 and y1 >= r.y0.
        auto& o = act[other];
        for (auto it = o.lower_bound(r.y0 - tallest[other]); it != o.end() && it->first <= r.y1; ++it) {
            if (!meet(r, theirs[it->second].r)) continue;
            if (same) f(std::min(e.idx, it->second), std::max(e.idx, it->second));
            else if (e.isB) f(it->second, e.idx);
            else f(e.idx, it->second);
        }
        act[me].insert({r.y0, e.idx});
        tallest[me] = std::max(tallest[me], r.y1 - r.y0);
    }
}

}  // namespace detail

// The netlist of a routed DEF, from its geometry. False (with err) when the
// DEF and the LEF do not fit together.
inline bool extractBlock(const phys::Tech& tech, const phys::Design& d, BlockNetlist& out, std::string& err,
                         unsigned threads = 1) {
    using namespace detail;
    out = BlockNetlist();
    out.name = d.name;
    Dsu dsu;
    // One node per cell pin and per design pin, then one per shape.
    std::vector<uint32_t> compBase(d.comps.size(), 0);
    std::vector<const phys::Macro*> macro(d.comps.size(), nullptr);
    for (size_t i = 0; i < d.comps.size(); ++i) {
        macro[i] = tech.macro(d.comps[i].macro);
        if (!macro[i]) { err = "component " + d.comps[i].name + ": no macro " + d.comps[i].macro + " in the LEF"; return false; }
        compBase[i] = uint32_t(dsu.p.size());
        for (size_t k = 0; k < macro[i]->pins.size(); ++k) dsu.add();
    }
    uint32_t pinBase = uint32_t(dsu.p.size());
    for (size_t i = 0; i < d.pins.size(); ++i) dsu.add();

    std::unordered_map<std::string, int> layerIdx;
    for (size_t i = 0; i < tech.layers.size(); ++i) layerIdx[tech.layers[i].name] = int(i);
    std::vector<std::vector<BShape>> shapes(tech.layers.size());
    std::vector<std::pair<uint32_t, uint32_t>> wireNet;   // node -> net index (special nets after the nets)
    size_t skipped = 0;
    bool ok = layout::defShapes(tech, d, false, skipped, err, [&](const std::string& layer, const phys::Rect& r, layout::DefShape s) {
        auto it = layerIdx.find(layer);
        if (it == layerIdx.end()) return;
        uint32_t node = dsu.add();
        shapes[size_t(it->second)].push_back({r, node});
        if (s.kind == layout::DefShape::CompPin) dsu.join(node, compBase[s.a] + s.b);
        else if (s.kind == layout::DefShape::IoPin) dsu.join(node, pinBase + s.a);
        else if (s.kind == layout::DefShape::Wire) wireNet.push_back({node, s.a});
        else if (s.kind == layout::DefShape::SpecialWire) wireNet.push_back({node, uint32_t(d.nets.size()) + s.a});
    });
    if (!ok) return false;

    // The pairs of every layer, and of every cut with the metal under and
    // over it, are found side by side; the joins are made afterwards.
    struct Job { int a, b; bool same; };
    std::vector<Job> jobs;
    for (size_t i = 0; i < tech.layers.size(); ++i) {
        const phys::Layer& l = tech.layers[i];
        if (shapes[i].empty()) continue;
        if (l.type == phys::Layer::Routing) jobs.push_back({int(i), int(i), true});
        if (l.type != phys::Layer::Cut) continue;
        for (int k = int(i) - 1; k >= 0; --k)
            if (tech.layers[size_t(k)].type == phys::Layer::Routing) { jobs.push_back({int(i), k, false}); break; }
        for (size_t k = i + 1; k < tech.layers.size(); ++k)
            if (tech.layers[k].type == phys::Layer::Routing) { jobs.push_back({int(i), int(k), false}); break; }
    }
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> found(jobs.size());
    geom::parallelFor(jobs.size(), threads, [&](size_t j) {
        const std::vector<BShape> &a = shapes[size_t(jobs[j].a)], &b = shapes[size_t(jobs[j].b)];
        touching(a, b, jobs[j].same, jobs[j].same, [&](uint32_t x, uint32_t y) { found[j].push_back({a[x].node, b[y].node}); });
    });
    for (const auto& v : found)
        for (auto [x, y] : v) dsu.join(x, y);

    // Nets: the roots that hold a terminal, numbered in terminal order.
    std::unordered_map<uint32_t, int> netOf;
    std::vector<std::set<std::string>> portNames, wireNames;
    auto netFor = [&](uint32_t node) {
        uint32_t r = dsu.find(node);
        auto it = netOf.find(r);
        if (it != netOf.end()) return it->second;
        int id = int(out.nets.size());
        netOf[r] = id;
        out.nets.emplace_back();
        portNames.emplace_back();
        wireNames.emplace_back();
        return id;
    };
    for (size_t i = 0; i < d.comps.size(); ++i) {
        if (d.comps[i].status.empty()) continue;
        BlockInst inst;
        inst.name = d.comps[i].name;
        inst.cell = d.comps[i].macro;
        for (size_t k = 0; k < macro[i]->pins.size(); ++k) {
            const phys::MacroPin& p = macro[i]->pins[k];
            BlockPin bp;
            bp.pin = p.name;
            bp.supply = p.use == "POWER" || p.use == "GROUND";
            if (!p.ports.empty()) bp.net = netFor(compBase[i] + uint32_t(k));
            inst.pins.push_back(std::move(bp));
        }
        out.insts.push_back(std::move(inst));
    }
    for (size_t i = 0; i < d.pins.size(); ++i) {
        BlockPort p;
        p.name = d.pins[i].name;
        p.supply = d.pins[i].use == "POWER" || d.pins[i].use == "GROUND";
        if (!d.pins[i].shapes.empty()) {
            p.net = netFor(pinBase + uint32_t(i));
            portNames[size_t(p.net)].insert(p.name);
        }
        out.ports.push_back(std::move(p));
    }
    // Names: the block's pin, else the DEF's name of a wire of the net, else a cell pin.
    std::set<uint32_t> floatingRoots;
    for (auto [node, ni] : wireNet) {
        uint32_t r = dsu.find(node);
        auto it = netOf.find(r);
        if (it == netOf.end()) { floatingRoots.insert(r); continue; }
        wireNames[size_t(it->second)].insert(ni < d.nets.size() ? d.nets[ni].name : d.specialNets[ni - d.nets.size()].name);
    }
    out.floating = floatingRoots.size();
    for (const BlockInst& inst : out.insts)
        for (const BlockPin& p : inst.pins)
            if (p.net >= 0 && out.nets[size_t(p.net)].empty() && portNames[size_t(p.net)].empty() && wireNames[size_t(p.net)].empty())
                out.nets[size_t(p.net)] = inst.name + "/" + p.pin;
    for (size_t i = 0; i < out.nets.size(); ++i) {
        if (!portNames[i].empty()) out.nets[i] = *portNames[i].begin();
        else if (!wireNames[i].empty()) out.nets[i] = *wireNames[i].begin();
    }
    // Two extracted nets with one name are one net of the DEF in pieces
    // (supply rails that nothing ties together, for one): the later ones get
    // a number, so that every net has its own name.
    std::map<std::string, int> seen;
    for (std::string& n : out.nets) {
        int k = ++seen[n];
        if (k > 1) n += "$" + std::to_string(k);
    }
    for (const auto& [n, k] : seen)
        if (k > 1) out.notes.push_back("net " + n + " of the DEF is " + std::to_string(k) + " separate nets in the layout");
    return true;
}

// The same netlist form from a gate netlist flattened to its leaf cells.
inline BlockNetlist fromGates(const gnl::Netlist& g) {
    BlockNetlist out;
    out.name = g.top;
    out.nets = g.nets;
    for (const gnl::FlatInst& i : g.insts) {
        BlockInst inst;
        inst.name = i.name;
        inst.cell = i.cell;
        for (const gnl::FlatConn& c : i.conns) inst.pins.push_back({c.pin, c.net, false});
        out.insts.push_back(std::move(inst));
    }
    for (const gnl::FlatPort& p : g.ports) out.ports.push_back({p.name, p.net, false});
    return out;
}

// Structural Verilog of an extracted block, supply pins included.
inline std::string toVerilog(const BlockNetlist& n) {
    std::string s = "// " + n.name + ": extracted from the layout by mpv lvs.\nmodule " + gnl::vname(n.name) + " (";
    for (size_t i = 0; i < n.ports.size(); ++i) s += (i ? ", " : "") + gnl::vname(n.ports[i].name);
    s += ");\n";
    std::set<std::string> portNets;
    for (const BlockPort& p : n.ports) {
        s += "  inout " + gnl::vname(p.name) + ";\n";
        portNets.insert(p.name);
    }
    for (const std::string& w : n.nets)
        if (!portNets.count(w)) s += "  wire " + gnl::vname(w) + ";\n";
    for (const BlockInst& i : n.insts) {
        s += "  " + gnl::vname(i.cell) + " " + gnl::vname(i.name) + " (";
        bool first = true;
        for (const BlockPin& p : i.pins) {
            if (p.net < 0) continue;
            s += std::string(first ? "" : ", ") + "." + gnl::vname(p.pin) + "(" + gnl::vname(n.nets[size_t(p.net)]) + ")";
            first = false;
        }
        s += ");\n";
    }
    s += "endmodule\n";
    return s;
}

inline BlockResult compareBlock(const BlockNetlist& lay, const BlockNetlist& sch, size_t maxMessages = 50) {
    BlockResult res;
    std::vector<std::string> msgs;
    auto say = [&](std::string m) { msgs.push_back(std::move(m)); };

    // One terminal: a cell pin or a port, with its net on each side.
    struct Term { std::string name; int l = -1, s = -1; };
    std::vector<Term> terms;

    std::unordered_map<std::string, const BlockInst*> schInst, layInst;
    for (const BlockInst& i : sch.insts) schInst[i.name] = &i;
    for (const BlockInst& i : lay.insts) layInst[i.name] = &i;
    for (const BlockInst& li : lay.insts) {
        auto it = schInst.find(li.name);
        if (it == schInst.end()) {
            // A cell with nothing but supply pins (a filler, a tap) need not be in the netlist.
            bool physical = true;
            for (const BlockPin& p : li.pins) physical &= p.supply;
            if (!physical) say("instance " + li.name + " (" + li.cell + ") is in the layout but not in the netlist");
            continue;
        }
        const BlockInst& si = *it->second;
        ++res.insts;
        if (si.cell != li.cell) {
            say("instance " + li.name + " is " + li.cell + " in the layout and " + si.cell + " in the netlist");
            continue;
        }
        std::unordered_map<std::string, int> sNet;
        std::set<std::string> sPins;
        for (const BlockPin& p : si.pins) { sNet[p.pin] = p.net; sPins.insert(p.pin); }
        for (const BlockPin& p : li.pins) {
            auto sp = sNet.find(p.pin);
            sPins.erase(p.pin);
            // A supply pin is compared only when the netlist connects it.
            if (p.supply && (sp == sNet.end() || sp->second < 0)) continue;
            terms.push_back({li.name + "/" + p.pin, p.net, sp == sNet.end() ? -1 : sp->second});
        }
        for (const std::string& p : sPins) say("pin " + si.name + "/" + p + " is in the netlist, but " + li.cell + " has no such pin in the LEF");
    }
    for (const BlockInst& si : sch.insts)
        if (!layInst.count(si.name)) {
            bool connected = false;
            for (const BlockPin& p : si.pins) connected |= p.net >= 0;
            if (connected) say("instance " + si.name + " (" + si.cell + ") is in the netlist but not in the layout");
        }
    {
        std::unordered_map<std::string, int> sPort;
        std::set<std::string> left;
        for (const BlockPort& p : sch.ports) { sPort[p.name] = p.net; left.insert(p.name); }
        for (const BlockPort& p : lay.ports) {
            auto it = sPort.find(p.name);
            if (it == sPort.end()) {
                // The supply pins of the block are not in a netlist without supplies.
                if (!p.supply) say("port " + p.name + " is in the layout but not in the netlist");
                continue;
            }
            left.erase(p.name);
            ++res.ports;
            if (p.net < 0) { say("port " + p.name + " has no shape in the layout"); continue; }
            terms.push_back({"port " + p.name, p.net, it->second});
        }
        for (const std::string& p : left) say("port " + p + " is in the netlist but not in the layout");
    }

    // Opens and shorts: the nets of one side seen from the other.
    std::map<int, std::vector<size_t>> byS, byL;
    for (size_t i = 0; i < terms.size(); ++i) {
        if (terms[i].s >= 0) byS[terms[i].s].push_back(i);
        if (terms[i].l >= 0) byL[terms[i].l].push_back(i);
    }
    auto some = [&](const std::vector<size_t>& ts) {
        std::string s;
        for (size_t i = 0; i < ts.size() && i < 4; ++i) s += (i ? ", " : "") + terms[ts[i]].name;
        if (ts.size() > 4) s += ", ... (" + std::to_string(ts.size()) + " pins)";
        return s;
    };
    for (const auto& [s, ts] : byS) {
        if (ts.size() < 2) continue;   // a net with one pin has nothing to connect
        ++res.nets;
        std::map<int, std::vector<size_t>> pieces;
        for (size_t t : ts) pieces[terms[t].l < 0 ? -1 - int(t) : terms[t].l].push_back(t);
        if (pieces.size() == 1) continue;
        ++res.opens;
        std::string m = "open: net " + sch.nets[size_t(s)] + " is in " + std::to_string(pieces.size()) + " pieces in the layout:";
        size_t shown = 0;
        for (const auto& [l, ps] : pieces) {
            if (shown++ == 4) { m += " ..."; break; }
            m += " [" + some(ps) + "]";
        }
        say(m);
    }
    for (const auto& [l, ts] : byL) {
        std::map<int, std::vector<size_t>> nets;
        std::vector<size_t> loose;
        for (size_t t : ts) {
            if (terms[t].s < 0) loose.push_back(t); else nets[terms[t].s].push_back(t);
        }
        if (nets.size() > 1) {
            ++res.shorts;
            std::string m = "short: " + std::to_string(nets.size()) + " nets of the netlist are one net in the layout (" + lay.nets[size_t(l)] + "):";
            size_t shown = 0;
            for (const auto& [s, ps] : nets) {
                if (shown++ == 4) { m += " ..."; break; }
                m += " " + sch.nets[size_t(s)] + " [" + some(ps) + "]";
            }
            say(m);
        }
        // A pin the netlist leaves open must not be wired to other pins.
        if (!loose.empty() && ts.size() > 1)
            for (size_t t : loose)
                say(terms[t].name + " is not connected in the netlist but is on net " + lay.nets[size_t(l)] + " in the layout");
    }
    std::sort(msgs.begin(), msgs.end());
    res.match = msgs.empty();
    if (msgs.size() > maxMessages) {
        size_t more = msgs.size() - maxMessages;
        msgs.resize(maxMessages);
        msgs.push_back("... and " + std::to_string(more) + " more");
    }
    res.messages = std::move(msgs);
    return res;
}

}  // namespace lvs
