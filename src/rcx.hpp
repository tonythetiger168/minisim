// rcx.hpp - parasitic RC extraction: routed layout in, RC network per net out.
//
// The engine behind mrcx. It works on a plain description of the routed
// design (rcx::Layout: wires, vias and pin shapes per net, in database
// units), so it does not depend on any file reader. src/mrcx.cpp fills the
// Layout from LEF/DEF and writes the result as SPEF (IEEE 1481).
//
// What it does, per net:
//   1. Connectivity. Wires are cut where another wire, a via or a pin of the
//      same net touches them; the cut points become nodes. A net whose pieces
//      do not all join is reported as open.
//   2. Resistance. Each wire piece is sheet resistance x length / width, each
//      via is resistance per cut / number of cuts, both corrected for
//      temperature (CRT1, CRT2). Long pieces are cut again (maxSeg) so the
//      network is a distributed RC line and not one lump.
//   3. Capacitance. Each piece gets
//        - capacitance to ground (substrate): area and fringe, less the area
//          hidden by metal on the layer below;
//        - coupling to the nearest wire on each side on the same layer, with
//          nearer wires shielding farther ones;
//        - coupling to wires of other nets it crosses one and two layers away.
//      Coupling to power and ground nets counts as capacitance to ground.
//
// The capacitance model is closed-form (Sakurai, "Closed-form expressions for
// interconnection delay, coupling, and crosstalk in VLSIs", IEEE Trans.
// Electron Devices 40(1), 1993) on the layer stack: conductor thickness and
// height and the permittivity of the dielectric between. The stack comes from
// an ITF-style process file (Stack::parseItf) or, without one, from the LEF
// layer values.
//
// Threads: nets are built in parallel and capacitance is computed in parallel
// over fixed-size blocks of wire pieces. Blocks are merged in block order, so
// the result is identical for any thread count.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "parallel.hpp"
#include "spef.hpp"

namespace rcx {

using i64 = std::int64_t;

struct Rect {
    i64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    i64 lo(int ax) const { return ax ? y0 : x0; }
    i64 hi(int ax) const { return ax ? y1 : x1; }
    bool touches(const Rect& o) const {
        return x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1;
    }
    bool overlaps(const Rect& o) const {
        return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
    }
    Rect meet(const Rect& o) const {
        return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
    }
};

// ---------------------------------------------------------------------------
// Process stack
// ---------------------------------------------------------------------------

inline constexpr double kEps0 = 8.8541878128e-3;   // fF/um

struct Dielectric {
    std::string name;
    double thickness = 0, er = 3.9, zbot = 0;
};
struct Conductor {
    std::string name;
    double thickness = 0, zbot = 0;
    double rpsq = 0, wmin = 0, smin = 0, crt1 = 0, crt2 = 0;
    // LEF-style overrides for the capacitance to ground, used when >= 0:
    double areaCap = -1;   // fF/um^2
    double edgeCap = -1;   // fF/um, per edge
};
struct ViaLayer {
    std::string name, from, to;
    double rpv = 0, area = 0, crt1 = 0, crt2 = 0;
};

struct Stack {
    std::string name;
    double tnom = 25;
    std::vector<Dielectric> diel;   // bottom to top
    std::vector<Conductor> cond;    // bottom to top
    std::vector<ViaLayer> vias;

    // Effective permittivity of the dielectric between two heights (the
    // layers act as capacitors in series).
    double erBetween(double z0, double z1) const {
        if (z1 < z0) std::swap(z0, z1);
        if (diel.empty() || z1 - z0 <= 0) return diel.empty() ? 3.9 : erAt(z0);
        double inv = 0;
        for (size_t i = 0; i < diel.size(); i++) {
            double a = std::max(z0, diel[i].zbot);
            double top = i + 1 == diel.size() ? 1e30 : diel[i].zbot + diel[i].thickness;
            double b = std::min(z1, top);
            if (b > a) inv += (b - a) / diel[i].er;
        }
        return inv > 0 ? (z1 - z0) / inv : diel.back().er;
    }
    double erAt(double z) const {
        for (auto& d : diel)
            if (z < d.zbot + d.thickness) return d.er;
        return diel.empty() ? 3.9 : diel.back().er;
    }
    const Conductor* findCond(const std::string& n) const {
        for (auto& c : cond)
            if (c.name == n) return &c;
        return nullptr;
    }

    // An ITF-style process file. Statements run from the top of the stack to
    // the bottom; a conductor sits on the dielectric listed after it:
    //   TECHNOLOGY = demo45
    //   GLOBAL_TEMPERATURE = 25
    //   DIELECTRIC ild2 { THICKNESS = 0.12 ER = 2.9 }
    //   CONDUCTOR metal2 { THICKNESS = 0.14 WMIN = 0.07 SMIN = 0.07 RPSQ = 0.25 CRT1 = 0.003 }
    //   DIELECTRIC ild1 { THICKNESS = 0.12 ER = 2.9 }
    //   CONDUCTOR metal1 { THICKNESS = 0.13 WMIN = 0.07 SMIN = 0.065 RPSQ = 0.38 }
    //   DIELECTRIC pmd { THICKNESS = 0.30 ER = 3.9 }
    //   VIA via1 { FROM = metal1 TO = metal2 AREA = 0.0049 RPV = 5 }
    // "$" starts a comment. Properties this model does not use are skipped.
    static Stack parseItf(const std::string& text, const std::string& fname) {
        std::vector<std::pair<std::string, int>> tok;
        int line = 1;
        for (size_t i = 0; i < text.size();) {
            char c = text[i];
            if (c == '\n') { line++; i++; }
            else if (std::isspace(static_cast<unsigned char>(c))) i++;
            else if (c == '$') { while (i < text.size() && text[i] != '\n') i++; }
            else if (c == '{' || c == '}' || c == '=') { tok.push_back({std::string(1, c), line}); i++; }
            else {
                size_t j = i;
                while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) &&
                       text[j] != '{' && text[j] != '}' && text[j] != '=' && text[j] != '$') j++;
                tok.push_back({text.substr(i, j - i), line});
                i = j;
            }
        }
        size_t p = 0;
        auto fail = [&](const std::string& m) -> void {
            int ln = p < tok.size() ? tok[p].second : line;
            throw std::runtime_error(fname + ":" + std::to_string(ln) + ": " + m);
        };
        auto upper = [](std::string s) {
            for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            return s;
        };
        auto next = [&]() -> std::string {
            if (p >= tok.size()) fail("unexpected end of file");
            return tok[p++].first;
        };
        auto num = [&](const std::string& s) -> double {
            try {
                size_t n = 0;
                double v = std::stod(s, &n);
                if (n == s.size()) return v;
            } catch (...) {}
            p--;
            fail("expected a number, got '" + s + "'");
            return 0;
        };
        // Reads "{ KEY = value ... }" into a map; nested blocks are skipped.
        auto block = [&]() {
            std::map<std::string, std::string> kv;
            if (next() != "{") { p--; fail("expected '{'"); }
            for (;;) {
                std::string k = next();
                if (k == "}") break;
                std::string e = next();
                if (e == "{") {   // a nested table, e.g. ETCH_VS_WIDTH_AND_SPACING
                    for (int depth = 1; depth;) {
                        std::string t = next();
                        if (t == "{") depth++;
                        if (t == "}") depth--;
                    }
                    continue;
                }
                if (e != "=") { p--; fail("expected '=' after " + k); }
                std::string v = next();
                if (v == "{") {
                    for (int depth = 1; depth;) {
                        std::string t = next();
                        if (t == "{") depth++;
                        if (t == "}") depth--;
                    }
                    continue;
                }
                kv[upper(k)] = v;
            }
            return kv;
        };
        Stack s;
        struct Item { bool isCond; Dielectric d; Conductor c; };
        std::vector<Item> items;   // top to bottom, as written
        while (p < tok.size()) {
            std::string k = upper(next());
            if (k == "TECHNOLOGY") { if (next() != "=") fail("expected '='"); s.name = next(); }
            else if (k == "GLOBAL_TEMPERATURE") { if (next() != "=") fail("expected '='"); s.tnom = num(next()); }
            else if (k == "DIELECTRIC") {
                Item it{false, {}, {}};
                it.d.name = next();
                auto kv = block();
                if (!kv.count("THICKNESS")) fail("DIELECTRIC " + it.d.name + " needs THICKNESS");
                it.d.thickness = num(kv["THICKNESS"]);
                if (kv.count("ER")) it.d.er = num(kv["ER"]);
                if (it.d.thickness < 0 || it.d.er <= 0) fail("DIELECTRIC " + it.d.name + ": bad THICKNESS or ER");
                items.push_back(it);
            } else if (k == "CONDUCTOR") {
                Item it{true, {}, {}};
                it.c.name = next();
                auto kv = block();
                if (!kv.count("THICKNESS")) fail("CONDUCTOR " + it.c.name + " needs THICKNESS");
                it.c.thickness = num(kv["THICKNESS"]);
                if (it.c.thickness <= 0) fail("CONDUCTOR " + it.c.name + ": THICKNESS must be positive");
                if (kv.count("WMIN")) it.c.wmin = num(kv["WMIN"]);
                if (kv.count("SMIN")) it.c.smin = num(kv["SMIN"]);
                if (kv.count("RPSQ")) it.c.rpsq = num(kv["RPSQ"]);
                if (kv.count("CRT1")) it.c.crt1 = num(kv["CRT1"]);
                if (kv.count("CRT2")) it.c.crt2 = num(kv["CRT2"]);
                items.push_back(it);
            } else if (k == "VIA") {
                ViaLayer v;
                v.name = next();
                auto kv = block();
                if (!kv.count("FROM") || !kv.count("TO")) fail("VIA " + v.name + " needs FROM and TO");
                v.from = kv["FROM"]; v.to = kv["TO"];
                if (kv.count("RPV")) v.rpv = num(kv["RPV"]);
                if (kv.count("AREA")) v.area = num(kv["AREA"]);
                if (kv.count("CRT1")) v.crt1 = num(kv["CRT1"]);
                if (kv.count("CRT2")) v.crt2 = num(kv["CRT2"]);
                s.vias.push_back(v);
            } else if (p < tok.size() && tok[p].first == "=") {   // an option this model ignores
                p++; next();
            } else if (p < tok.size() && tok[p].first == "{") {
                block();
            } else if (p + 1 < tok.size() && tok[p + 1].first == "{") {   // e.g. POLY name { ... }
                p++; block();
            } else { p--; fail("unknown statement '" + k + "'"); }
        }
        double z = 0;
        for (size_t i = items.size(); i-- > 0;) {
            if (items[i].isCond) {
                items[i].c.zbot = z;
                s.cond.push_back(items[i].c);
            } else {
                items[i].d.zbot = z;
                z += items[i].d.thickness;
                s.diel.push_back(items[i].d);
            }
        }
        if (s.cond.empty()) throw std::runtime_error(fname + ": no CONDUCTOR in the process file");
        return s;
    }
};

// ---------------------------------------------------------------------------
// Layout (input)
// ---------------------------------------------------------------------------

struct Layer {              // a routing layer, as the LEF describes it
    std::string name;
    double width = 0, spacing = 0;            // um
    double rpersq = -1;                       // ohm/sq
    double areaCap = -1, edgeCap = -1;        // fF/um^2, fF/um
    double thickness = -1, height = -1;       // um
};
struct Wire {               // an axis-parallel centre line with a width
    int layer = 0;
    i64 x0 = 0, y0 = 0, x1 = 0, y1 = 0, width = 0;
    i64 ext0 = 0, ext1 = 0; // how far the metal runs past each end point
};
struct Via {
    int lo = 0, hi = 0;     // routing layers joined
    i64 x = 0, y = 0;
    Rect rlo, rhi;          // the via's metal on each layer
    int cuts = 1;
    double res = -1;        // ohm for the whole via, if the LEF gives one
};
struct Pin {
    std::string inst, pin;  // inst is empty for a port of the design
    std::string cell;
    char dir = 'B';         // I, O or B
    i64 x = 0, y = 0;       // where to report it when it has no shape
    std::vector<std::pair<int, Rect>> shapes;
};
struct Patch { int layer = 0; Rect r; };   // extra metal without a centre line
struct Net {
    std::string name;
    bool power = false;     // power or ground: loads its neighbours, not extracted
    std::vector<Wire> wires;
    std::vector<Via> vias;
    std::vector<Pin> pins;
    std::vector<Patch> patches;
};
struct Layout {
    std::string design;
    double dbu = 1000;      // database units per micron
    std::vector<Layer> layers;   // routing layers, bottom to top
    std::vector<Net> nets;
};

struct Options {
    double temp = 25;        // degrees C
    bool tempGiven = false;  // false: use the process file's nominal temperature
    double rscale = 1, cscale = 1, ccscale = 1;
    double maxSeg = 20;      // um; longest wire piece before it is cut again (0: never)
    double ccDist = 0;       // um; how far to look for a neighbour (0: 6 pitches)
    bool coupled = true;     // false: every coupling capacitance goes to ground
    double ccFactor = 1;     // multiplier when a coupling is grounded
    double ccThreshold = 0;  // fF; a smaller coupling is grounded
    int threads = 0;         // 0: all logical cores
};

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

struct NodeOut {
    int pin = -1;            // index into Net::pins, or -1 for an internal node
    int layer = 0;
    i64 x = 0, y = 0;
    double cg = 0;           // fF to ground
};
struct ResOut { int a = 0, b = 0; double r = 0; };
struct CcOut { int node = 0, onet = 0, onode = 0; double c = 0; };
struct NetOut {
    std::vector<NodeOut> nodes;
    std::vector<ResOut> res;
    std::vector<CcOut> cc;
    double wirelen = 0;      // um
    double cgTotal = 0, ccTotal = 0, rTotal = 0;
    int opens = 0;           // extra disconnected parts
};
struct Short { int netA = 0, netB = 0, layer = 0; Rect where; };
struct Result {
    std::vector<NetOut> nets;        // one per Layout::nets (empty for power nets)
    std::vector<Short> shorts;
    std::vector<std::string> warnings;
    std::vector<double> layerLen;    // um of signal wire per layer
    Stack stack;                     // the stack that was used
    double temp = 25;
    size_t pieces = 0;
    int threads = 1;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

namespace detail {

// Runs f(block) for block = 0..nblocks-1 on the shared thread helper.
template <class F>
void parBlocks(size_t nblocks, int threads, F&& f) {
    par::parallelFor(nblocks, static_cast<unsigned>(std::max(threads, 1)), f);
}

struct Piece {
    Rect r;
    int net = 0;       // index into Layout::nets; power nets have n0 = n1 = -1
    int n0 = -1, n1 = -1;
    std::uint8_t layer = 0, axis = 0;   // axis 0: runs along x
    bool power = false;
};

// Sakurai's fits, per unit length, as multiples of the permittivity.
inline double groundPlate(double w, double h) { return 1.15 * w / h; }
inline double groundFringe(double t, double h) { return 2.80 * std::pow(t / h, 0.222); }
// How strongly a neighbour at spacing s takes over the fringe field to the
// substrate: the share is kShield*h / (s + kShield*h).
inline constexpr double kShield = 0.2;
inline double lateral(double w, double t, double h, double s) {
    double k = 0.03 * w / h + 0.83 * t / h - 0.07 * std::pow(t / h, 0.222);
    return k > 0 ? k * std::pow(s / h, -1.34) : 0;
}

struct NetBuild {
    std::vector<Piece> pieces;
    std::vector<std::string> warnings;
};

// Step 1 and 2 for one net: nodes, resistors and wire pieces.
inline void buildNet(const Layout& L, int netIdx, const std::vector<double>& rsheet,
                     const std::vector<std::vector<double>>& viaRes, const Options& opt,
                     NetOut& out, NetBuild& nb) {
    const Net& net = L.nets[static_cast<size_t>(netIdx)];
    const double dbu = L.dbu;

    struct Pre { int layer; i64 x, y; int pin; };
    std::vector<Pre> pre;
    std::vector<int> uf;
    auto mk = [&](int layer, i64 x, i64 y, int pin = -1) {
        pre.push_back({layer, x, y, pin});
        uf.push_back(static_cast<int>(uf.size()));
        return static_cast<int>(uf.size()) - 1;
    };
    auto find = [&](int a) {
        while (uf[static_cast<size_t>(a)] != a) {
            uf[static_cast<size_t>(a)] = uf[static_cast<size_t>(uf[static_cast<size_t>(a)])];
            a = uf[static_cast<size_t>(a)];
        }
        return a;
    };
    auto unite = [&](int a, int b) {
        a = find(a); b = find(b);
        if (a == b) return;
        if (a > b) std::swap(a, b);   // the earlier node stays the representative
        uf[static_cast<size_t>(b)] = a;
    };

    struct Shape { int layer; Rect r; int wire; int node; bool pin; bool point = false; i64 px = 0, py = 0; };
    std::vector<Shape> shapes;
    struct WireN { Wire w; int axis; i64 a, b; std::vector<std::pair<i64, int>> bp; };
    std::vector<WireN> wires;
    struct PreRes { int a, b; double r; };
    std::vector<PreRes> pres;

    for (size_t p = 0; p < net.pins.size(); p++) {
        const Pin& pin = net.pins[p];
        i64 x = pin.x, y = pin.y;
        int layer = 0;
        if (!pin.shapes.empty()) {
            const Rect& r = pin.shapes[0].second;
            x = (r.x0 + r.x1) / 2; y = (r.y0 + r.y1) / 2; layer = pin.shapes[0].first;
        }
        int n = mk(layer, x, y, static_cast<int>(p));
        for (auto& s : pin.shapes) shapes.push_back({s.first, s.second, -1, n, true});
    }
    for (const Wire& w0 : net.wires) {
        WireN wn;
        wn.w = w0;
        Wire& w = wn.w;
        if (w.x0 != w.x1 && w.y0 != w.y1) {
            nb.warnings.push_back("net " + net.name + ": a diagonal wire is treated as its two end points");
            continue;
        }
        if (w.x0 > w.x1 || w.y0 > w.y1) {
            std::swap(w.x0, w.x1); std::swap(w.y0, w.y1); std::swap(w.ext0, w.ext1);
        }
        if (w.width <= 0) {
            nb.warnings.push_back("net " + net.name + ": a wire has no width and is skipped");
            continue;
        }
        wn.axis = w.x0 != w.x1 ? 0 : (w.y0 != w.y1 ? 1 : 0);
        wn.a = wn.axis ? w.y0 : w.x0;
        wn.b = wn.axis ? w.y1 : w.x1;
        wn.bp.push_back({wn.a, mk(w.layer, w.x0, w.y0)});
        wn.bp.push_back({wn.b, mk(w.layer, w.x1, w.y1)});
        Rect r;
        i64 h = w.width / 2;
        if (wn.axis == 0) r = {w.x0 - w.ext0, w.y0 - h, w.x1 + w.ext1, w.y0 + h};
        else r = {w.x0 - h, w.y0 - w.ext0, w.x0 + h, w.y1 + w.ext1};
        shapes.push_back({w.layer, r, static_cast<int>(wires.size()), -1, false});
        wires.push_back(std::move(wn));
    }
    for (const Via& v : net.vias) {
        int a = mk(v.lo, v.x, v.y), b = mk(v.hi, v.x, v.y);
        Rect pt{v.x, v.y, v.x, v.y};
        shapes.push_back({v.lo, v.rlo.x1 > v.rlo.x0 ? v.rlo : pt, -1, a, false, true, v.x, v.y});
        shapes.push_back({v.hi, v.rhi.x1 > v.rhi.x0 ? v.rhi : pt, -1, b, false, true, v.x, v.y});
        double r = viaRes[static_cast<size_t>(v.lo)][static_cast<size_t>(v.hi)];
        if (r < 0) r = v.res >= 0 ? v.res : 0;
        else r /= std::max(1, v.cuts);
        pres.push_back({a, b, std::max(r * opt.rscale, 1e-6)});
    }
    for (const Patch& pa : net.patches)
        shapes.push_back({pa.layer, pa.r, -1, mk(pa.layer, (pa.r.x0 + pa.r.x1) / 2, (pa.r.y0 + pa.r.y1) / 2), false});

    // Every pair of touching shapes on one layer is a connection.
    // The node on shape `s` where `other` joins it. On a wire that is a new
    // cut point: at the via's own position, on the other wire's centre line,
    // or at the wire's end when the end is inside the contact, so that no
    // tiny stub is left over.
    auto nodeOn = [&](const Shape& s, const Shape& other) {
        if (s.wire < 0) return s.node;
        WireN& wn = wires[static_cast<size_t>(s.wire)];
        Rect m = s.r.meet(other.r);
        const int ax = wn.axis;
        i64 lo = std::max(m.lo(ax), wn.a), hi = std::min(m.hi(ax), wn.b);
        i64 c;
        if (lo > hi) c = m.lo(ax) > wn.b ? wn.b : wn.a;   // only the extension touches
        else {
            i64 want;
            bool have = false;
            if (other.wire >= 0) {
                const WireN& ow = wires[static_cast<size_t>(other.wire)];
                if (ow.axis != ax || ow.a == ow.b) { want = ax ? ow.w.y0 : ow.w.x0; have = true; }
            } else if (other.point) {
                want = ax ? other.py : other.px; have = true;
            }
            if (have) c = std::clamp(want, lo, hi);
            else if (lo == wn.a) c = wn.a;
            else if (hi == wn.b) c = wn.b;
            else c = (lo + hi) / 2;
        }
        int n = ax ? mk(s.layer, wn.w.x0, c) : mk(s.layer, c, wn.w.y0);
        wn.bp.push_back({c, n});
        return n;
    };
    std::vector<int> order(shapes.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const Shape& A = shapes[static_cast<size_t>(a)];
        const Shape& B = shapes[static_cast<size_t>(b)];
        if (A.layer != B.layer) return A.layer < B.layer;
        if (A.r.x0 != B.r.x0) return A.r.x0 < B.r.x0;
        return a < b;
    });
    for (size_t i = 0; i < order.size(); i++) {
        const Shape A = shapes[static_cast<size_t>(order[i])];
        for (size_t j = i + 1; j < order.size(); j++) {
            const Shape B = shapes[static_cast<size_t>(order[j])];
            if (B.layer != A.layer || B.r.x0 > A.r.x1) break;
            if (B.r.y0 > A.r.y1 || A.r.y0 > B.r.y1) continue;
            if (A.pin && B.pin) continue;
            if (A.wire < 0 && B.wire < 0 && A.node == B.node) continue;
            int na = nodeOn(A, B), nbn = nodeOn(B, A);
            // A pin keeps a node of its own, so two pins never share one.
            if (A.pin || B.pin) pres.push_back({na, nbn, 1e-3});
            else unite(na, nbn);
        }
    }

    // Cut the wires at their nodes.
    const i64 maxSeg = opt.maxSeg > 0 ? std::max<i64>(1, static_cast<i64>(std::llround(opt.maxSeg * dbu))) : 0;
    struct PrePiece { Rect r; int n0, n1, layer, axis; };
    std::vector<PrePiece> pp;
    for (WireN& wn : wires) {
        const Wire& w = wn.w;
        std::stable_sort(wn.bp.begin(), wn.bp.end(),
                         [](const auto& x, const auto& y) { return x.first < y.first; });
        const double rs = rsheet[static_cast<size_t>(w.layer)];
        const i64 h = w.width / 2;
        auto rectOf = [&](i64 p, i64 q) {
            i64 lo = p == wn.a ? p - w.ext0 : p, hi = q == wn.b ? q + w.ext1 : q;
            return wn.axis == 0 ? Rect{lo, w.y0 - h, hi, w.y0 + h} : Rect{w.x0 - h, lo, w.x0 + h, hi};
        };
        if (wn.a == wn.b) {   // a single point: metal, but no resistance
            for (size_t k = 1; k < wn.bp.size(); k++) unite(wn.bp[0].second, wn.bp[k].second);
            Rect r = rectOf(wn.a, wn.b);
            if (r.x1 > r.x0 && r.y1 > r.y0) pp.push_back({r, wn.bp[0].second, wn.bp[0].second, w.layer, wn.axis});
            continue;
        }
        out.wirelen += static_cast<double>(wn.b - wn.a) / dbu;
        for (size_t k = 0; k + 1 < wn.bp.size(); k++) {
            i64 p = wn.bp[k].first, q = wn.bp[k + 1].first;
            int n0 = wn.bp[k].second, n1 = wn.bp[k + 1].second;
            if (p == q) { unite(n0, n1); continue; }
            i64 parts = maxSeg ? (q - p + maxSeg - 1) / maxSeg : 1;
            i64 from = p;
            int nfrom = n0;
            for (i64 s = 1; s <= parts; s++) {
                i64 to = s == parts ? q : p + (q - p) * s / parts;
                int nto = s == parts ? n1 : (wn.axis ? mk(w.layer, w.x0, to) : mk(w.layer, to, w.y0));
                double r = rs * static_cast<double>(to - from) / static_cast<double>(w.width) * opt.rscale;
                pres.push_back({nfrom, nto, r});
                pp.push_back({rectOf(from, to), nfrom, nto, w.layer, wn.axis});
                from = to; nfrom = nto;
            }
        }
    }

    // Number the nodes that are left: pins first, in pin order.
    std::vector<int> local(pre.size(), -1);
    auto localOf = [&](int n) {
        int r = find(n);
        if (local[static_cast<size_t>(r)] < 0) {
            local[static_cast<size_t>(r)] = static_cast<int>(out.nodes.size());
            const Pre& q = pre[static_cast<size_t>(r)];
            NodeOut no;
            no.pin = q.pin; no.layer = q.layer; no.x = q.x; no.y = q.y;
            out.nodes.push_back(no);
        }
        return local[static_cast<size_t>(r)];
    };
    for (size_t p = 0; p < net.pins.size(); p++) localOf(static_cast<int>(p));
    for (auto& r : pres) {
        int a = localOf(r.a), b = localOf(r.b);
        if (a == b) continue;
        out.res.push_back({a, b, r.r});
        out.rTotal += r.r;
    }
    nb.pieces.reserve(pp.size());
    for (auto& q : pp) {
        Piece pc;
        pc.r = q.r; pc.net = netIdx;
        pc.n0 = localOf(q.n0); pc.n1 = localOf(q.n1);
        pc.layer = static_cast<std::uint8_t>(q.layer); pc.axis = static_cast<std::uint8_t>(q.axis);
        nb.pieces.push_back(pc);
    }
    for (size_t i = 0; i < pre.size(); i++) localOf(static_cast<int>(i));   // patches, bare vias

    // Opens: parts of the net no resistor joins.
    size_t n = out.nodes.size();
    if (n > 1) {
        std::vector<int> comp(n);
        for (size_t i = 0; i < n; i++) comp[i] = static_cast<int>(i);
        auto f2 = [&](int a) {
            while (comp[static_cast<size_t>(a)] != a) {
                comp[static_cast<size_t>(a)] = comp[static_cast<size_t>(comp[static_cast<size_t>(a)])];
                a = comp[static_cast<size_t>(a)];
            }
            return a;
        };
        for (auto& r : out.res) {
            int a = f2(r.a), b = f2(r.b);
            if (a != b) comp[static_cast<size_t>(std::max(a, b))] = std::min(a, b);
        }
        int parts = 0;
        for (size_t i = 0; i < n; i++) parts += f2(static_cast<int>(i)) == static_cast<int>(i);
        out.opens = parts - 1;
    }
}

// A uniform grid over the pieces of one layer.
struct Grid {
    i64 x0 = 0, y0 = 0, cell = 1;
    int nx = 0, ny = 0;
    std::vector<std::uint32_t> start, item;
    int cx(i64 x) const { return static_cast<int>(std::clamp<i64>((x - x0) / cell, 0, nx - 1)); }
    int cy(i64 y) const { return static_cast<int>(std::clamp<i64>((y - y0) / cell, 0, ny - 1)); }
    void build(const std::vector<Piece>& pieces, const std::vector<std::uint32_t>& ids, i64 cellSize) {
        if (ids.empty()) return;
        Rect bb = pieces[ids[0]].r;
        for (auto id : ids) {
            const Rect& r = pieces[id].r;
            bb.x0 = std::min(bb.x0, r.x0); bb.y0 = std::min(bb.y0, r.y0);
            bb.x1 = std::max(bb.x1, r.x1); bb.y1 = std::max(bb.y1, r.y1);
        }
        x0 = bb.x0; y0 = bb.y0;
        // Keep the grid near one piece per cell, whatever the design's size.
        double area = static_cast<double>(bb.x1 - bb.x0 + 1) * static_cast<double>(bb.y1 - bb.y0 + 1);
        i64 fit = static_cast<i64>(std::sqrt(area / static_cast<double>(ids.size())));
        cell = std::max<i64>({cellSize, fit, 1});
        nx = static_cast<int>((bb.x1 - bb.x0) / cell) + 1;
        ny = static_cast<int>((bb.y1 - bb.y0) / cell) + 1;
        start.assign(static_cast<size_t>(nx) * static_cast<size_t>(ny) + 1, 0);
        for (auto id : ids) {
            const Rect& r = pieces[id].r;
            for (int y = cy(r.y0); y <= cy(r.y1); y++)
                for (int x = cx(r.x0); x <= cx(r.x1); x++)
                    start[static_cast<size_t>(y) * static_cast<size_t>(nx) + static_cast<size_t>(x) + 1]++;
        }
        for (size_t i = 1; i < start.size(); i++) start[i] += start[i - 1];
        item.resize(start.back());
        std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
        for (auto id : ids) {
            const Rect& r = pieces[id].r;
            for (int y = cy(r.y0); y <= cy(r.y1); y++)
                for (int x = cx(r.x0); x <= cx(r.x1); x++)
                    item[fill[static_cast<size_t>(y) * static_cast<size_t>(nx) + static_cast<size_t>(x)]++] = id;
        }
    }
    // Pieces whose cells meet the window, each once. `seen` is the caller's
    // scratch array (one entry per piece, zeroed once) and `epoch` a counter
    // that it raises on every call.
    void query(const Rect& w, std::vector<std::uint32_t>& outIds, std::vector<std::uint32_t>& seen,
               std::uint32_t epoch) const {
        outIds.clear();
        if (nx == 0) return;
        if (w.x1 < x0 || w.y1 < y0 || w.x0 > x0 + cell * nx || w.y0 > y0 + cell * ny) return;
        int xa = cx(w.x0), xb = cx(w.x1), ya = cy(w.y0), yb = cy(w.y1);
        for (int y = ya; y <= yb; y++)
            for (int x = xa; x <= xb; x++) {
                size_t c = static_cast<size_t>(y) * static_cast<size_t>(nx) + static_cast<size_t>(x);
                for (std::uint32_t k = start[c]; k < start[c + 1]; k++) {
                    std::uint32_t id = item[k];
                    if (seen[id] == epoch) continue;
                    seen[id] = epoch;
                    outIds.push_back(id);
                }
            }
    }
};

}  // namespace detail

// The stack to extract with: the process file's, or one made from the LEF
// layer values when there is no process file. `map` renames LEF layers to
// process-file conductors.
inline Stack makeStack(const Layout& L, const Stack* itf,
                       const std::map<std::string, std::string>& map = {}) {
    Stack s;
    if (itf) {
        s.name = itf->name; s.tnom = itf->tnom; s.diel = itf->diel; s.vias = itf->vias;
        for (const Layer& l : L.layers) {
            auto m = map.find(l.name);
            const std::string& want = m == map.end() ? l.name : m->second;
            const Conductor* c = itf->findCond(want);
            if (!c) {
                std::string have;
                for (auto& k : itf->cond) have += (have.empty() ? "" : ", ") + k.name;
                throw std::runtime_error("routing layer '" + l.name + "' has no CONDUCTOR '" + want +
                                         "' in the process file (it has: " + have +
                                         "); name it with -map");
            }
            Conductor k = *c;
            if (k.rpsq <= 0 && l.rpersq > 0) k.rpsq = l.rpersq;
            if (k.wmin <= 0) k.wmin = l.width;
            if (k.smin <= 0) k.smin = l.spacing;
            s.cond.push_back(k);
        }
        for (auto& v : s.vias) {
            for (auto* n : {&v.from, &v.to})
                for (auto& kv : map)
                    if (kv.second == *n) { *n = kv.first; break; }
        }
        for (size_t i = 0; i < s.cond.size(); i++) s.cond[i].name = L.layers[i].name;
        return s;
    }
    s.name = "lef";
    double top = 0;
    for (size_t i = 0; i < L.layers.size(); i++) {
        const Layer& l = L.layers[i];
        Conductor c;
        c.name = l.name;
        c.wmin = l.width; c.smin = l.spacing;
        c.rpsq = l.rpersq > 0 ? l.rpersq : 0;
        c.thickness = l.thickness > 0 ? l.thickness : (l.width > 0 ? 1.8 * l.width : 0.2);
        // No height in the LEF: put the layer one thickness above the last.
        c.zbot = l.height > 0 ? l.height : (i == 0 ? 2.0 * c.thickness : top + c.thickness);
        if (c.zbot <= top && i > 0) c.zbot = top + 0.5 * c.thickness;
        c.areaCap = l.areaCap; c.edgeCap = l.edgeCap;
        top = c.zbot + c.thickness;
        s.cond.push_back(c);
    }
    s.diel.push_back({"oxide", top + 1.0, 3.9, 0});
    return s;
}

inline Result extract(const Layout& L, const Stack& stack, const Options& opt) {
    using namespace detail;
    Result R;
    R.stack = stack;
    const size_t nl = L.layers.size();
    if (stack.cond.size() != nl) throw std::runtime_error("the stack does not match the routing layers");
    if (nl > 255) throw std::runtime_error("too many routing layers");
    const double dbu = L.dbu;
    const double temp = opt.tempGiven ? opt.temp : stack.tnom;
    R.temp = temp;
    const double dT = temp - stack.tnom;
    int threads = opt.threads > 0 ? opt.threads : static_cast<int>(par::defaultThreads());
    R.threads = threads;

    std::vector<double> rsheet(nl);
    for (size_t i = 0; i < nl; i++) {
        const Conductor& c = stack.cond[i];
        rsheet[i] = c.rpsq * (1 + c.crt1 * dT + c.crt2 * dT * dT);
        if (c.rpsq <= 0) R.warnings.push_back("layer " + c.name + " has no sheet resistance; its wires get 0 ohm");
    }
    // Resistance per cut between two layers from the process file, or -1.
    std::vector<std::vector<double>> viaRes(nl, std::vector<double>(nl, -1));
    for (auto& v : stack.vias)
        for (size_t a = 0; a < nl; a++)
            for (size_t b = 0; b < nl; b++)
                if ((stack.cond[a].name == v.from && stack.cond[b].name == v.to) ||
                    (stack.cond[a].name == v.to && stack.cond[b].name == v.from))
                    viaRes[a][b] = v.rpv * (1 + v.crt1 * dT + v.crt2 * dT * dT);

    // --- nets -> nodes, resistors, pieces
    const size_t nn = L.nets.size();
    R.nets.resize(nn);
    std::vector<NetBuild> nb(nn);
    const size_t netBlock = 64;
    parBlocks((nn + netBlock - 1) / netBlock, threads, [&](size_t b) {
        for (size_t i = b * netBlock; i < std::min(nn, (b + 1) * netBlock); i++) {
            const Net& net = L.nets[i];
            if (!net.power) {
                buildNet(L, static_cast<int>(i), rsheet, viaRes, opt, R.nets[i], nb[i]);
                continue;
            }
            // Power metal: pieces only, cut so that none spans the whole die.
            const i64 chunk = static_cast<i64>(std::llround(std::max(opt.maxSeg, 20.0) * dbu));
            auto add = [&](int layer, const Rect& r) {
                if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
                int axis = (r.x1 - r.x0) >= (r.y1 - r.y0) ? 0 : 1;
                for (i64 p = r.lo(axis); p < r.hi(axis); p += chunk) {
                    Piece pc;
                    i64 q = std::min(p + chunk, r.hi(axis));
                    pc.r = axis == 0 ? Rect{p, r.y0, q, r.y1} : Rect{r.x0, p, r.x1, q};
                    pc.net = static_cast<int>(i); pc.power = true;
                    pc.layer = static_cast<std::uint8_t>(layer); pc.axis = static_cast<std::uint8_t>(axis);
                    nb[i].pieces.push_back(pc);
                }
            };
            for (const Wire& w : net.wires) {
                i64 h = w.width / 2;
                Rect r{std::min(w.x0, w.x1), std::min(w.y0, w.y1), std::max(w.x0, w.x1), std::max(w.y0, w.y1)};
                bool horiz = w.y0 == w.y1;
                i64 e0 = w.x0 > w.x1 || w.y0 > w.y1 ? w.ext1 : w.ext0, e1 = w.x0 > w.x1 || w.y0 > w.y1 ? w.ext0 : w.ext1;
                if (horiz) r = {r.x0 - e0, r.y0 - h, r.x1 + e1, r.y1 + h};
                else r = {r.x0 - h, r.y0 - e0, r.x1 + h, r.y1 + e1};
                add(w.layer, r);
            }
            for (const Patch& pa : net.patches) add(pa.layer, pa.r);
        }
    });
    for (size_t i = 0; i < nn; i++)
        for (auto& w : nb[i].warnings) R.warnings.push_back(w);

    std::vector<Piece> pieces;
    {
        size_t total = 0;
        for (auto& b : nb) total += b.pieces.size();
        pieces.reserve(total);
        for (auto& b : nb) {
            pieces.insert(pieces.end(), b.pieces.begin(), b.pieces.end());
            std::vector<Piece>().swap(b.pieces);
        }
    }
    R.pieces = pieces.size();
    if (pieces.size() >= 0xffffffffu) throw std::runtime_error("too many wire pieces");
    R.layerLen.assign(nl, 0);
    for (size_t i = 0; i < nn; i++)
        if (!L.nets[i].power)
            for (const Wire& w : L.nets[i].wires)
                R.layerLen[static_cast<size_t>(w.layer)] +=
                    static_cast<double>(std::llabs(w.x1 - w.x0) + std::llabs(w.y1 - w.y0)) / dbu;

    // --- one grid per layer
    std::vector<i64> reach(nl);
    std::vector<Grid> grid(nl);
    {
        std::vector<std::vector<std::uint32_t>> ids(nl);
        for (size_t i = 0; i < pieces.size(); i++) ids[pieces[i].layer].push_back(static_cast<std::uint32_t>(i));
        for (size_t l = 0; l < nl; l++) {
            const Conductor& c = stack.cond[l];
            double d = opt.ccDist > 0 ? opt.ccDist : 6 * (c.wmin + c.smin);
            if (d <= 0) d = 2.0;
            reach[l] = static_cast<i64>(std::llround(d * dbu));
        }
        parBlocks(nl, threads, [&](size_t l) { grid[l].build(pieces, ids[l], 2 * reach[l]); });
    }

    // Per-layer constants (permittivity folded in), in fF and um.
    struct LC { double t, h, epsG, epsL; };
    std::vector<LC> lc(nl);
    for (size_t l = 0; l < nl; l++) {
        const Conductor& c = stack.cond[l];
        LC k{};
        k.t = c.thickness;
        k.h = c.zbot > 0 ? c.zbot : c.thickness;   // height above the substrate
        k.epsG = kEps0 * stack.erBetween(0, k.h);
        k.epsL = kEps0 * stack.erAt(c.zbot + c.thickness / 2);
        lc[l] = k;
    }

    // Dielectric gap and permittivity between layer l and layer l+1, l+2.
    std::vector<std::array<double, 3>> pairGap(nl), pairEps(nl);
    for (size_t l = 0; l < nl; l++)
        for (size_t d = 1; d <= 2 && l + d < nl; d++) {
            const Conductor& lo = stack.cond[l];
            const Conductor& hi = stack.cond[l + d];
            double top = lo.zbot + lo.thickness;
            pairGap[l][d] = std::max(hi.zbot - top, 0.01 * hi.thickness);
            pairEps[l][d] = kEps0 * stack.erBetween(top, top + pairGap[l][d]);
        }

    // --- capacitance, in fixed blocks of pieces
    struct CcRec { std::uint64_t a, b; double c; };   // a < b: global node keys
    struct BlockOut { std::vector<CcRec> cc; std::vector<Short> shorts; };
    std::vector<double> g0(pieces.size(), 0), g1(pieces.size(), 0);
    const size_t blk = 1024;
    const size_t nblk = (pieces.size() + blk - 1) / blk;
    std::vector<BlockOut> bo(nblk);
    auto key = [](int net, int node) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(net)) << 32) | static_cast<std::uint32_t>(node);
    };
    auto nearNode = [](const Piece& p, i64 x, i64 y) {
        i64 c = p.axis ? y : x;
        return 2 * c < p.r.lo(p.axis) + p.r.hi(p.axis) ? p.n0 : p.n1;
    };
    struct Nb { i64 lo, hi, gap; std::uint32_t id; };
    struct Scratch {
        std::vector<std::uint32_t> cand, seen;
        std::uint32_t epoch = 0;
        std::vector<Nb> side;
        std::vector<i64> cuts;
        std::vector<Rect> between;
    };
    // Scratch space is handed out per block and taken back, so a thread
    // reuses what an earlier block left (the `seen` array is as long as the
    // piece list).
    std::vector<std::unique_ptr<Scratch>> spare;
    std::mutex spareMu;
    parBlocks(nblk, threads, [&](size_t b) {
        std::unique_ptr<Scratch> held;
        {
            std::lock_guard<std::mutex> g(spareMu);
            if (!spare.empty()) { held = std::move(spare.back()); spare.pop_back(); }
        }
        if (!held) held = std::make_unique<Scratch>();
        struct Return {
            std::unique_ptr<Scratch>& h; std::vector<std::unique_ptr<Scratch>>& pool; std::mutex& mu;
            ~Return() { std::lock_guard<std::mutex> g(mu); pool.push_back(std::move(h)); }
        } giveBack{held, spare, spareMu};
        Scratch& sc = *held;
        if (sc.seen.size() != pieces.size()) sc.seen.assign(pieces.size(), 0);
        auto& cand = sc.cand;
        auto& seen = sc.seen;
        auto& epoch = sc.epoch;
        auto& side = sc.side;
        auto& cuts = sc.cuts;
        auto& between = sc.between;
        BlockOut& out = bo[b];
        for (size_t i = b * blk; i < std::min(pieces.size(), (b + 1) * blk); i++) {
            const Piece& A = pieces[i];
            if (A.power) continue;
            const LC& k = lc[A.layer];
            const Conductor& cd = stack.cond[A.layer];
            const int ax = A.axis, py = 1 - ax;
            const double len = static_cast<double>(A.r.hi(ax) - A.r.lo(ax)) / dbu;
            const double w = static_cast<double>(A.r.hi(py) - A.r.lo(py)) / dbu;
            const i64 mid = A.r.lo(ax) + A.r.hi(ax);   // twice the middle
            double ga = 0, gb = 0;   // ground capacitance at n0, n1
            // Fringe capacitance to the substrate per unit length, one side.
            const double fringeSide = cd.areaCap >= 0 || cd.edgeCap >= 0 ? std::max(cd.edgeCap, 0.0)
                                                                         : 0.5 * k.epsG * groundFringe(k.t, k.h);
            auto addCc = [&](int nodeA, const Piece& B, int nodeB, double c) {
                std::uint64_t ka = key(A.net, nodeA), kb = key(B.net, nodeB);
                if (ka > kb) std::swap(ka, kb);
                out.cc.push_back({ka, kb, c});
            };

            // Same layer: the nearest neighbour on each side, interval by interval.
            Rect win = A.r;
            (py ? win.y0 : win.x0) -= reach[A.layer];
            (py ? win.y1 : win.x1) += reach[A.layer];
            grid[A.layer].query(win, cand, seen, ++epoch);
            for (int sgn = 0; sgn < 2; sgn++) {
                side.clear();
                for (auto id : cand) {
                    if (id == i) continue;
                    const Piece& B = pieces[id];
                    i64 lo = std::max(A.r.lo(ax), B.r.lo(ax)), hi = std::min(A.r.hi(ax), B.r.hi(ax));
                    if (lo >= hi) continue;
                    i64 gap = sgn ? B.r.lo(py) - A.r.hi(py) : A.r.lo(py) - B.r.hi(py);
                    if (gap <= 0) {
                        if (sgn == 0 && B.net != A.net && A.r.overlaps(B.r) && (B.power || A.net < B.net))
                            out.shorts.push_back({A.net, B.net, A.layer, A.r.meet(B.r)});
                        continue;
                    }
                    if (gap > reach[A.layer]) continue;
                    side.push_back({lo, hi, gap, id});
                }
                if (side.empty()) continue;
                cuts.clear();
                for (auto& n : side) { cuts.push_back(n.lo); cuts.push_back(n.hi); }
                std::sort(cuts.begin(), cuts.end());
                cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
                for (size_t c = 0; c + 1 < cuts.size(); c++) {
                    const Nb* best = nullptr;
                    for (auto& n : side)
                        if (n.lo <= cuts[c] && n.hi >= cuts[c + 1] &&
                            (!best || n.gap < best->gap || (n.gap == best->gap && n.id < best->id)))
                            best = &n;
                    if (!best) continue;
                    const Piece& B = pieces[best->id];
                    if (B.net == A.net) continue;   // shields, adds nothing
                    double l = static_cast<double>(cuts[c + 1] - cuts[c]) / dbu;
                    double s = static_cast<double>(best->gap) / dbu;
                    double cc = k.epsL * lateral(w, k.t, k.h, s) * l * opt.cscale * opt.ccscale;
                    i64 m2 = cuts[c] + cuts[c + 1];
                    int na = m2 < mid ? A.n0 : A.n1;
                    // Where along the piece this stretch sits: 0 at n0, 1 at n1.
                    const double f = A.r.hi(ax) > A.r.lo(ax)
                        ? static_cast<double>(m2 - 2 * A.r.lo(ax)) / static_cast<double>(2 * (A.r.hi(ax) - A.r.lo(ax)))
                        : 0.5;
                    if (B.power) {
                        ga += cc * (1 - f);
                        gb += cc * f;
                    } else {
                        // A close neighbour takes part of the fringe field
                        // that would have gone to the substrate on this side:
                        // that part is coupling, not ground. Both wires give
                        // up the same amount, so each net's total is unchanged.
                        double moved = fringeSide * l * (kShield * k.h / (s + kShield * k.h)) * opt.cscale;
                        ga -= moved * (1 - f);
                        gb -= moved * f;
                        cc += moved * opt.ccscale;
                        // Each side sees this pair once; half from each view.
                        i64 mx = ax ? 0 : m2 / 2, my = ax ? m2 / 2 : 0;
                        addCc(na, B, nearNode(B, B.axis == ax ? mx : (B.r.x0 + B.r.x1) / 2,
                                              B.axis == ax ? my : (B.r.y0 + B.r.y1) / 2), 0.5 * cc);
                    }
                }
            }

            // Other layers. A wire couples to what it crosses one and two
            // layers away; for two layers, only through the part of the
            // crossing that metal on the layer between does not cover. Each
            // pair of signal wires is handled once, from the upper wire;
            // power metal is handled from the signal wire, above or below.
            double covered = 0;   // area hidden from the substrate
            auto crossing = [&](int ol, bool below) {
                const int mlayer = std::abs(ol - A.layer) == 2 ? (ol + A.layer) / 2 : -1;
                const int up = std::max<int>(ol, A.layer), dn = std::min<int>(ol, A.layer);
                const double gap = pairGap[static_cast<size_t>(dn)][static_cast<size_t>(up - dn)];
                const double eps = pairEps[static_cast<size_t>(dn)][static_cast<size_t>(up - dn)];
                const double tbar = 0.5 * (k.t + lc[static_cast<size_t>(ol)].t);
                if (mlayer >= 0) {
                    between.clear();
                    grid[static_cast<size_t>(mlayer)].query(A.r, cand, seen, ++epoch);
                    for (auto id : cand)
                        if (A.r.overlaps(pieces[id].r)) between.push_back(A.r.meet(pieces[id].r));
                }
                grid[static_cast<size_t>(ol)].query(A.r, cand, seen, ++epoch);
                for (auto id : cand) {
                    const Piece& B = pieces[id];
                    if (!A.r.overlaps(B.r)) continue;
                    if (!below && !B.power) continue;
                    Rect m = A.r.meet(B.r);
                    double mw = static_cast<double>(m.x1 - m.x0) / dbu, mh = static_cast<double>(m.y1 - m.y0) / dbu;
                    double seenFrac = 1;
                    if (mlayer >= 0 && !between.empty()) {
                        double hid = 0;
                        for (const Rect& q : between)
                            if (q.overlaps(m)) {
                                Rect z = q.meet(m);
                                hid += static_cast<double>(z.x1 - z.x0) * static_cast<double>(z.y1 - z.y0);
                            }
                        seenFrac = std::clamp(1.0 - hid / (static_cast<double>(m.x1 - m.x0) * static_cast<double>(m.y1 - m.y0)), 0.0, 1.0);
                    }
                    if (below) covered += mw * mh * seenFrac;
                    if (B.net == A.net || seenFrac <= 0) continue;
                    double cc = eps * (1.15 * mw * mh / gap + 0.70 * 2 * (mw + mh) * std::pow(tbar / gap, 0.222)) *
                                seenFrac * opt.cscale * opt.ccscale;
                    i64 cx = (m.x0 + m.x1) / 2, cy = (m.y0 + m.y1) / 2;
                    int na = nearNode(A, cx, cy);
                    if (B.power) (na == A.n0 ? ga : gb) += cc;
                    else addCc(na, B, nearNode(B, cx, cy), cc);
                }
            };
            if (A.layer > 0) crossing(A.layer - 1, true);
            if (A.layer > 1) crossing(A.layer - 2, true);
            if (static_cast<size_t>(A.layer) + 1 < nl) crossing(A.layer + 1, false);
            if (static_cast<size_t>(A.layer) + 2 < nl) crossing(A.layer + 2, false);

            // Ground.
            double open = len * w > 0 ? std::clamp(1.0 - covered / (len * w), 0.0, 1.0) : 1.0;
            double cg;
            if (cd.areaCap >= 0 || cd.edgeCap >= 0)
                cg = std::max(cd.areaCap, 0.0) * len * w * open + std::max(cd.edgeCap, 0.0) * 2 * len;
            else
                cg = k.epsG * (groundPlate(w, k.h) * open + groundFringe(k.t, k.h)) * len;
            cg *= opt.cscale;
            g0[i] = ga + 0.5 * cg;
            g1[i] = gb + 0.5 * cg;
        }
    });

    // --- merge, in piece order
    for (size_t i = 0; i < pieces.size(); i++) {
        const Piece& p = pieces[i];
        if (p.power) continue;
        NetOut& n = R.nets[static_cast<size_t>(p.net)];
        n.nodes[static_cast<size_t>(p.n0)].cg += g0[i];
        n.nodes[static_cast<size_t>(p.n1)].cg += g1[i];
    }
    // Coupling records: group by the first net (a counting sort, in block
    // order), then sort and add up each net's records in parallel.
    std::vector<CcRec> all;
    std::vector<size_t> first(nn + 1, 0);
    {
        for (auto& b : bo) {
            for (auto& c : b.cc) first[(c.a >> 32) + 1]++;
            for (auto& s : b.shorts) R.shorts.push_back(s);
        }
        for (size_t i = 0; i < nn; i++) first[i + 1] += first[i];
        all.resize(first[nn]);
        std::vector<size_t> fill(first.begin(), first.end() - 1);
        for (auto& b : bo) {
            for (auto& c : b.cc) all[fill[c.a >> 32]++] = c;
            std::vector<CcRec>().swap(b.cc);
        }
    }
    std::vector<size_t> uniq(nn, 0);   // records left per net after adding up
    parBlocks((nn + netBlock - 1) / netBlock, threads, [&](size_t b) {
        for (size_t n = b * netBlock; n < std::min(nn, (b + 1) * netBlock); n++) {
            CcRec* lo = all.data() + first[n];
            CcRec* hi = all.data() + first[n + 1];
            std::stable_sort(lo, hi, [](const CcRec& x, const CcRec& y) {
                return x.a != y.a ? x.a < y.a : x.b < y.b;
            });
            CcRec* o = lo;
            for (CcRec* i = lo; i < hi;) {
                CcRec r = *i++;
                for (; i < hi && i->a == r.a && i->b == r.b; i++) r.c += i->c;
                *o++ = r;
            }
            uniq[n] = static_cast<size_t>(o - lo);
        }
    });
    {
        std::vector<std::uint32_t> count(nn, 0);
        for (size_t n = 0; n < nn; n++)
            for (size_t i = first[n]; i < first[n] + uniq[n]; i++)
                if (opt.coupled && all[i].c >= opt.ccThreshold) { count[n]++; count[all[i].b >> 32]++; }
        for (size_t n = 0; n < nn; n++) R.nets[n].cc.reserve(count[n]);
    }
    for (size_t n = 0; n < nn; n++)
        for (size_t i = first[n]; i < first[n] + uniq[n]; i++) {
            const CcRec& r = all[i];
            double c = r.c;
            int na = static_cast<int>(r.a >> 32), xa = static_cast<int>(r.a & 0xffffffffu);
            int nbn = static_cast<int>(r.b >> 32), xb = static_cast<int>(r.b & 0xffffffffu);
            NetOut& A = R.nets[static_cast<size_t>(na)];
            NetOut& B = R.nets[static_cast<size_t>(nbn)];
            if (!opt.coupled || c < opt.ccThreshold) {
                A.nodes[static_cast<size_t>(xa)].cg += c * opt.ccFactor;
                B.nodes[static_cast<size_t>(xb)].cg += c * opt.ccFactor;
            } else {
                A.cc.push_back({xa, nbn, xb, c});
                B.cc.push_back({xb, na, xa, c});
            }
        }
    std::vector<CcRec>().swap(all);
    {   // one report per pair of shorted nets and layer
        std::sort(R.shorts.begin(), R.shorts.end(), [](const Short& x, const Short& y) {
            if (x.netA != y.netA) return x.netA < y.netA;
            if (x.netB != y.netB) return x.netB < y.netB;
            if (x.layer != y.layer) return x.layer < y.layer;
            if (x.where.x0 != y.where.x0) return x.where.x0 < y.where.x0;
            return x.where.y0 < y.where.y0;
        });
        R.shorts.erase(std::unique(R.shorts.begin(), R.shorts.end(), [](const Short& x, const Short& y) {
            return x.netA == y.netA && x.netB == y.netB && x.layer == y.layer;
        }), R.shorts.end());
    }
    parBlocks((nn + netBlock - 1) / netBlock, threads, [&](size_t b) {
        for (size_t i = b * netBlock; i < std::min(nn, (b + 1) * netBlock); i++) {
            NetOut& n = R.nets[i];
            std::sort(n.cc.begin(), n.cc.end(), [](const CcOut& x, const CcOut& y) {
                if (x.node != y.node) return x.node < y.node;
                if (x.onet != y.onet) return x.onet < y.onet;
                return x.onode < y.onode;
            });
            for (auto& nd : n.nodes) n.cgTotal += nd.cg;
            for (auto& c : n.cc) n.ccTotal += c.c;
        }
    });
    return R;
}

// ---------------------------------------------------------------------------
// SPEF out (the format itself is src/spef.hpp)
// ---------------------------------------------------------------------------

struct SpefOptions {
    std::string program = "mrcx", version, date;
    bool nameMap = true;
    int threads = 1;
};

inline void writeSpef(std::FILE* f, const Layout& L, const Result& R, const SpefOptions& so) {
    const double dbu = L.dbu;
    std::unordered_map<std::string, int> ids;
    std::vector<const std::string*> names;
    auto idOf = [&](const std::string& s) {
        auto it = ids.find(s);
        if (it != ids.end()) return it->second;
        int id = static_cast<int>(names.size()) + 1;
        auto ins = ids.emplace(s, id);
        names.push_back(&ins.first->first);
        return id;
    };
    std::vector<int> netId(L.nets.size(), 0);
    std::vector<std::vector<int>> instId(L.nets.size());
    for (size_t i = 0; i < L.nets.size(); i++) {
        if (L.nets[i].power) continue;
        netId[i] = idOf(L.nets[i].name);
    }
    for (size_t i = 0; i < L.nets.size(); i++) {
        if (L.nets[i].power) continue;
        for (auto& p : L.nets[i].pins) instId[i].push_back(idOf(p.inst.empty() ? p.pin : p.inst));
    }
    auto ref = [&](std::string& o, int id, const std::string& raw) {
        if (so.nameMap) { o += '*'; o += std::to_string(id); }
        else o += spef::escape(raw);
    };
    spef::Header h;
    h.design = L.design; h.date = so.date; h.vendor = "minisim"; h.program = so.program; h.version = so.version;
    h.flow = {"EXTRACTION", "PIN_CAP NONE", "NAME_SCOPE LOCAL"};
    spef::writeHeader(f, h);
    if (so.nameMap) {
        std::fputs("*NAME_MAP\n", f);
        for (size_t i = 0; i < names.size(); i++)
            std::fprintf(f, "*%zu %s\n", i + 1, spef::escape(*names[i]).c_str());
        std::fputc('\n', f);
    }
    bool anyPort = false;
    for (size_t i = 0; i < L.nets.size(); i++) {
        if (L.nets[i].power) continue;
        for (size_t p = 0; p < L.nets[i].pins.size(); p++) {
            const Pin& pin = L.nets[i].pins[p];
            if (!pin.inst.empty()) continue;
            if (!anyPort) { std::fputs("*PORTS\n", f); anyPort = true; }
            std::string o;
            ref(o, instId[i][p], pin.pin);
            std::fprintf(f, "%s %c\n", o.c_str(), pin.dir);
        }
    }
    if (anyPort) std::fputc('\n', f);
    // The nets are formatted in parallel, a wave of blocks at a time, and
    // written in order.
    auto num = [](std::string& o, double v) { spef::appendNum(o, v); };
    auto integer = [](std::string& o, int v) { spef::appendInt(o, v); };
    auto node = [&](std::string& o, size_t net, int nodeIdx) {
        const NodeOut& n = R.nets[net].nodes[static_cast<size_t>(nodeIdx)];
        if (n.pin >= 0) {
            const Pin& p = L.nets[net].pins[static_cast<size_t>(n.pin)];
            ref(o, instId[net][static_cast<size_t>(n.pin)], p.inst.empty() ? p.pin : p.inst);
            if (!p.inst.empty()) { o += ':'; o += spef::escape(p.pin); }
        } else {
            ref(o, netId[net], L.nets[net].name);
            o += ':';
            integer(o, nodeIdx + 1);
        }
    };
    auto formatNet = [&](size_t i, std::string& buf) {
        const Net& net = L.nets[i];
        if (net.power) return;
        const NetOut& n = R.nets[i];
        char tmp[128];
        buf += "*D_NET ";
        ref(buf, netId[i], net.name);
        buf += ' '; num(buf, n.cgTotal + n.ccTotal); buf += '\n';
        if (!net.pins.empty()) buf += "*CONN\n";
        for (size_t p = 0; p < net.pins.size(); p++) {
            const Pin& pin = net.pins[p];
            const NodeOut& nd = n.nodes[p];
            buf += pin.inst.empty() ? "*P " : "*I ";
            node(buf, i, static_cast<int>(p));
            std::snprintf(tmp, sizeof tmp, " %c *C %.4f %.4f", pin.dir, static_cast<double>(nd.x) / dbu,
                          static_cast<double>(nd.y) / dbu);
            buf += tmp;
            if (!pin.inst.empty() && !pin.cell.empty()) { buf += " *D "; buf += spef::escape(pin.cell); }
            buf += '\n';
        }
        int k = 0;
        bool head = false;
        for (size_t x = 0; x < n.nodes.size(); x++) {
            if (n.nodes[x].cg <= 0) continue;
            if (!head) { buf += "*CAP\n"; head = true; }
            integer(buf, ++k); buf += ' '; node(buf, i, static_cast<int>(x));
            buf += ' '; num(buf, n.nodes[x].cg); buf += '\n';
        }
        for (auto& c : n.cc) {
            if (!head) { buf += "*CAP\n"; head = true; }
            integer(buf, ++k); buf += ' '; node(buf, i, c.node); buf += ' ';
            node(buf, static_cast<size_t>(c.onet), c.onode);
            buf += ' '; num(buf, c.c); buf += '\n';
        }
        if (!n.res.empty()) buf += "*RES\n";
        k = 0;
        for (auto& r : n.res) {
            integer(buf, ++k); buf += ' '; node(buf, i, r.a); buf += ' '; node(buf, i, r.b);
            buf += ' '; num(buf, r.r); buf += '\n';
        }
        buf += "*END\n\n";
    };
    const size_t blockNets = 128, nblocks = (L.nets.size() + blockNets - 1) / blockNets;
    const size_t wave = static_cast<size_t>(std::max(1, so.threads)) * 8;
    std::vector<std::string> bufs(wave);
    for (size_t w0 = 0; w0 < nblocks; w0 += wave) {
        size_t cnt = std::min(wave, nblocks - w0);
        detail::parBlocks(cnt, so.threads, [&](size_t b) {
            // A local string: appending to bufs[b] directly would have the
            // threads fight over the cache lines that hold the string headers.
            std::string buf = std::move(bufs[b]);
            buf.clear();
            for (size_t i = (w0 + b) * blockNets; i < std::min(L.nets.size(), (w0 + b + 1) * blockNets); i++)
                formatNet(i, buf);
            bufs[b] = std::move(buf);
        });
        for (size_t b = 0; b < cnt; b++) std::fwrite(bufs[b].data(), 1, bufs[b].size(), f);
    }
}

}  // namespace rcx
