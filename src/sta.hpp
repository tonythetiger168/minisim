// sta.hpp - the static timing engine behind msta.
//
// Input: a flat gate netlist (gnetlist.hpp), Liberty libraries (liberty.hpp)
// and constraints (sdc.hpp). Output: slews, arc delays, arrival times per
// clock, and the slack of every timing check; the reports and the SDF writer
// are in sta_report.hpp.
//
// The model
//   * One graph node per port and per connected instance pin. Edges are net
//     arcs (driver to load) and cell arcs (the Liberty timing groups between
//     two pins, merged: the worst of the `when` variants).
//   * Delay calculation is NLDM: a cell arc's delay and output slew come from
//     the library tables at the input slew and the capacitance the output
//     drives (pin capacitances, set_load, and wire capacitance from SPEF or a
//     wire load model when given). Slews are merged per pin: the slowest for
//     late analysis, the fastest for early.
//   * Arrivals are kept per tag: (launch clock, launch edge, clock or data,
//     path-exception progress). Clock tags start at clock sources and travel
//     the clock network; at a register's clock pin they launch data tags.
//     Arrival times in a tag are relative to its clock edge.
//   * A clock is ideal unless set_propagated_clock names it: its network has
//     zero delay, set_clock_latency stands in for it.
//   * Checks: setup, hold, recovery, removal from the library's constraint
//     tables, and set_output_delay at ports. The capture edge follows the
//     usual rules: the first capture edge after the launch edge for setup, one
//     capture period earlier for hold, moved by set_multicycle_path.
//   * Latches are timed as registers at the edge of their timing arcs: a
//     latch's D pin is an endpoint and its D to Q arc is not a path (no time
//     borrowing).
//
// Everything that walks the whole graph runs level by level on a thread pool
// (parfor.hpp); each node only writes its own data, so the results do not
// depend on the number of threads.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gnetlist.hpp"
#include "liberty.hpp"
#include "parfor.hpp"
#include "sdc.hpp"

namespace sta {

enum { RISE = 0, FALL = 1 };
enum { LATE = 0, EARLY = 1 };  // max and min analysis
constexpr double INF = 1e30;
inline bool valid(double v) { return v > -INF / 2 && v < INF / 2; }

// Progress through the path exceptions that name pins: one bit per stage.
struct Mask {
    static constexpr int BITS = 256;
    uint64_t w[4] = {0, 0, 0, 0};
    bool test(int b) const { return (w[b >> 6] >> (b & 63)) & 1; }
    void set(int b) { w[b >> 6] |= 1ull << (b & 63); }
    bool operator==(const Mask& o) const { return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2] && w[3] == o.w[3]; }
    bool operator<(const Mask& o) const {
        for (int i = 0; i < 4; i++)
            if (w[i] != o.w[i]) return w[i] < o.w[i];
        return false;
    }
};

struct TagKey {
    int16_t clk = -1;
    uint8_t edge = RISE;  // the edge of the clock at its source
    uint8_t isClk = 0;    // still in the clock network
    Mask exc;
    bool operator==(const TagKey& o) const { return clk == o.clk && edge == o.edge && isClk == o.isClk && exc == o.exc; }
    bool operator<(const TagKey& o) const {
        if (clk != o.clk) return clk < o.clk;
        if (edge != o.edge) return edge < o.edge;
        if (isClk != o.isClk) return isClk > o.isClk;
        return exc < o.exc;
    }
};

struct Tag {
    TagKey key;
    double arr[2][2];       // [LATE/EARLY][RISE/FALL]
    int32_t prevEdge[2][2]; // the edge the arrival came over, -1: it starts here
    int32_t prevTag[2][2];  // index of the tag in the edge's from node
    uint8_t prevRf[2][2];
    Tag() {
        for (int m = 0; m < 2; m++)
            for (int r = 0; r < 2; r++) {
                arr[m][r] = m == LATE ? -INF : INF;
                prevEdge[m][r] = prevTag[m][r] = -1;
                prevRf[m][r] = 0;
            }
    }
};

struct Node {
    int inst = -1;  // -1: a port
    int net = -1;
    const liberty::Pin* pin = nullptr;  // instance pins of library cells
    bool driver = false;     // drives its net: an input port or a cell output
    bool clockSrc = false;   // a clock is defined here
    bool genSrc = false;     // a generated clock is defined here: other clocks stop
    bool idealClk = false;   // in the network of an ideal clock
    bool hasThrough = false, hasLaunch = false, hasSeed = false;
    bool endpoint = false;
    int level = 0;
    double cap = 0;          // as a load: the pin capacitance
    double load = 0;         // as a driver: everything it drives
    double slew[2][2] = {{0, 0}, {0, 0}};
};

enum : uint8_t { NetEdge = 0, CombEdge = 1, LaunchEdge = 2 };

struct Edge {
    int from = -1, to = -1;
    uint8_t kind = NetEdge;
    bool disabled = false, loopBreak = false;
    uint8_t ok = 0;          // bit (irf*2+orf): the transition pair exists
    int group = -1;          // the arc group of the cell (CellInfo::groups)
    double d[2][2][2] = {};  // [LATE/EARLY][irf][orf]
    bool has(int irf, int orf) const { return (ok >> (irf * 2 + orf)) & 1; }
};

// What the engine needs to know about a library cell, worked out once.
struct CellInfo {
    const liberty::Cell* cell = nullptr;
    struct Group {
        int from, to;  // indexes into cell->pins
        bool launch;   // an edge-triggered arc: clock to output
        std::vector<const liberty::Arc*> arcs;
    };
    struct Chk {
        int data, clk;
        const liberty::Arc* arc;
    };
    std::vector<Group> groups;
    std::vector<Chk> checks;
    std::unordered_map<std::string, int> pinIdx;
    bool latch = false;
};

struct Check {
    int data = -1, clk = -1, inst = -1;
    const liberty::Arc* arc = nullptr;
};

struct Clock {
    std::string name;
    double period = 0;
    double edgeT[2] = {0, 0};  // the time of the rising and falling edge
    std::vector<int> sources;  // nodes; empty: virtual
    bool propagated = false, generated = false;
    double srcLat[2] = {0, 0}, netLat[2] = {0, 0};  // [LATE/EARLY]
    double trans[2] = {0, 0};                       // set_clock_transition [rf]
    double unc[2] = {0, 0};                         // [setup, hold]
    const sdc::Clock* def = nullptr;
    int master = -1;
};

enum class EndKind { Setup, Hold, Recovery, Removal, OutSetup, OutHold };
inline bool isLate(EndKind k) { return k == EndKind::Setup || k == EndKind::Recovery || k == EndKind::OutSetup; }
inline const char* kindLabel(EndKind k) {
    switch (k) {
        case EndKind::Setup: return "setup";
        case EndKind::Hold: return "hold";
        case EndKind::Recovery: return "recovery";
        case EndKind::Removal: return "removal";
        case EndKind::OutSetup: return "output setup";
        case EndKind::OutHold: return "output hold";
    }
    return "";
}

// One constrained path end: the worst path into an endpoint for one capture
// clock and one kind of check.
struct PathEnd {
    EndKind kind = EndKind::Setup;
    int node = -1;         // the endpoint: a data pin or an output port
    int tag = -1, rf = RISE;
    int clkNode = -1;      // the register's clock pin, -1 at a port
    int clkTag = -1, clkRf = RISE;
    int launchClk = -1, capClk = -1;
    int launchEdge = RISE, capEdge = RISE;
    double launchT = 0, capT = 0;       // clock edge times
    double arrival = 0;                 // launchT + path delay
    double capArr = 0;                  // capture clock network delay
    double unc = 0, lib = 0, ext = 0;   // uncertainty, library time, external delay
    double required = 0, slack = 0;
    int exc = -1;                       // the exception that set the relation, or -1
    bool pathDelay = false;             // set_max_delay / set_min_delay
    double pathDelayValue = 0;
};

struct Violation {
    int node;
    double limit, actual, slack;
};

struct Exc {
    const sdc::Exception* def = nullptr;
    sdc::Exception::Type type;
    double value = 0;
    bool setup = true, hold = true;
    std::vector<int> fromClks, toClks;
    bool fromPins = false, toPins = false, hasTo = false;
    int fromBit = -1;
    std::vector<int> throughBits;
    int lastBit = -1;  // the bit that says from and every through were met; -1: no bits
    bool ignored = false;
};

struct Warning {
    std::string code, text;
};

class Sta {
public:
    // ----------------------------------------------------------- the design
    const gnl::Netlist* nl = nullptr;
    std::vector<const liberty::Library*> libs;
    std::unordered_map<std::string, const liberty::Cell*> cellByName;
    std::unordered_map<const liberty::Cell*, CellInfo> cellInfo;

    std::vector<Node> nodes;
    std::vector<const std::string*> nodePin;  // the pin name of an instance pin
    std::vector<Edge> edges;
    std::vector<int> finOff, finIdx, foutOff, foutIdx;  // CSR: edges into / out of a node
    std::vector<const CellInfo*> instInfo;
    std::vector<int> instPinOff, instPinNode;  // per instance: node of each cell pin, or -1
    std::vector<int> instNodeOff;              // per instance: its nodes are [off, next off)
    std::vector<Check> checks;
    std::vector<std::vector<int>> levels;
    std::vector<double> netWireCap;            // set_load on nets, SPEF totals
    std::vector<double> portLoad;
    // Name lookups. Pins are found through their instance, and the lists of
    // every pin, cell and net name are only built when a constraint asks for
    // them (a wildcard query), since they cost more than the analysis.
    std::unordered_map<std::string, int> portByName;
    mutable std::unordered_map<std::string, int> instByName_;
    mutable bool haveInstNames_ = false;
    std::vector<std::string> portNames;
    mutable std::vector<std::string> pinNames_, cellNames_;
    mutable std::unordered_map<std::string, int> netByName_;
    mutable bool haveNetNames_ = false;

    const std::vector<std::string>& pinNames() const {
        if (pinNames_.empty()) {
            size_t first = nl->ports.size();
            pinNames_.resize(nodes.size() - first);
            for (size_t n = first; n < nodes.size(); n++) pinNames_[n - first] = name((int)n);
        }
        return pinNames_;
    }
    const std::vector<std::string>& cellNames() const {
        if (cellNames_.empty()) {
            cellNames_.reserve(nl->insts.size());
            for (auto& i : nl->insts) cellNames_.push_back(i.name);
        }
        return cellNames_;
    }
    int findNet(const std::string& n) const {
        if (!haveNetNames_) {
            netByName_.reserve(nl->nets.size());
            for (size_t i = 0; i < nl->nets.size(); i++) netByName_[nl->nets[i]] = (int)i;
            haveNetNames_ = true;
        }
        auto it = netByName_.find(n);
        return it == netByName_.end() ? -1 : it->second;
    }
    int findInst(const std::string& n) const {
        if (!haveInstNames_) {
            instByName_.reserve(nl->insts.size());
            for (size_t i = 0; i < nl->insts.size(); i++) instByName_[nl->insts[i].name] = (int)i;
            haveInstNames_ = true;
        }
        auto it = instByName_.find(n);
        return it == instByName_.end() ? -1 : it->second;
    }
    // A port, or an instance pin "inst/PIN".
    int findNode(const std::string& n) const {
        auto it = portByName.find(n);
        if (it != portByName.end()) return it->second;
        size_t slash = n.rfind('/');
        if (slash == std::string::npos) return -1;
        int inst = findInst(n.substr(0, slash));
        if (inst < 0) return -1;
        for (int k = instNodeOff[inst]; k < instNodeOff[inst + 1]; k++)
            if (n.compare(slash + 1, std::string::npos, *nodePin[k]) == 0) return k;
        return -1;
    }
    std::vector<int> netDriver;                // one driver node of each net, or -1

    // ----------------------------------------------------------- constraints
    std::vector<Clock> clocks;
    std::unordered_map<std::string, int> clockByName;
    std::vector<std::vector<char>> clkExcl;    // set_clock_groups
    std::map<std::pair<int, int>, std::pair<double, double>> interUnc;  // (from, to) -> setup, hold
    std::vector<Exc> excs;
    std::unordered_map<int, std::vector<int>> excByTo;     // endpoint node -> exceptions
    std::vector<int> excGeneric;                           // exceptions with no pin in -to
    std::unordered_map<int, std::vector<std::pair<int, int>>> launchAt;  // node -> (exc, bit)
    std::vector<int> launchByClk;                                        // exceptions whose -from names clocks and has a bit
    struct Thr { int exc, need, set; };
    std::unordered_map<int, std::vector<Thr>> throughAt;
    struct Seed {
        TagKey key;
        double arr[2][2];
        double ext[2];  // the external delay part [LATE/EARLY]; clock seeds: 0
        bool input;
    };
    std::unordered_map<int, std::vector<Seed>> seeds;
    struct OutDelay { int clk; int edge; double v[2]; bool has[2]; };  // [LATE(max)/EARLY(min)]
    std::unordered_map<int, std::vector<OutDelay>> outDelays;
    struct Drive { const liberty::Cell* cell = nullptr; std::string pin, fromPin; double inSlew[2] = {0, 0}; };
    std::unordered_map<int, Drive> drives;
    std::unordered_map<int, std::array<double, 4>> inTrans;  // node -> slew [mm][rf]
    double maxTransition = -1, maxCapacitance = -1;          // set_max_transition / capacitance on the design
    std::unordered_map<int, double> maxTransAt, maxCapAt;
    double timeUnit = 1, capUnit = 1;  // ns and pF per user unit (the first library's units)

    // --------------------------------------------------------------- results
    std::vector<std::vector<Tag>> tags;
    std::vector<PathEnd> ends;
    std::vector<int> unclocked;      // register clock pins no clock reaches
    std::vector<int> unconstrained;  // endpoints with no constrained path
    std::vector<int> noInputDelay;   // input ports without set_input_delay
    std::vector<int> loopNodes;
    std::vector<Violation> transViol, capViol;
    std::vector<Warning> warnings;
    bool timed = false;
    par::Pool* pool = nullptr;

    void warn(const std::string& code, const std::string& text) { warnings.push_back({code, text}); }

    std::string name(int n) const {
        const Node& x = nodes[n];
        if (x.inst < 0) return nl->ports[n].name;
        return nl->insts[x.inst].name + "/" + *nodePin[n];
    }
    const liberty::Cell* cellOf(int n) const {
        return nodes[n].inst < 0 || !instInfo[nodes[n].inst] ? nullptr : instInfo[nodes[n].inst]->cell;
    }
    std::string refName(int n) const {
        if (nodes[n].inst < 0) return nl->ports[n].dir == gnl::Dir::In ? "in" : nl->ports[n].dir == gnl::Dir::Out ? "out" : "inout";
        return nl->insts[nodes[n].inst].cell;
    }
    bool isPort(int n) const { return nodes[n].inst < 0; }
    int pinNode(int inst, int pinIdx) const { return instPinNode[instPinOff[inst] + pinIdx]; }

    // ------------------------------------------------------------------ build
    void addLibrary(const liberty::Library& L) {
        if (libs.empty()) {
            timeUnit = L.timeScale;
            capUnit = L.capScale;
        }
        libs.push_back(&L);
        for (auto& c : L.cells) cellByName.emplace(c.name, &c);
    }

    const CellInfo* infoOf(const liberty::Cell* c) {
        auto it = cellInfo.find(c);
        if (it != cellInfo.end()) return &it->second;
        CellInfo ci;
        ci.cell = c;
        ci.latch = !c->latches.empty();
        for (size_t i = 0; i < c->pins.size(); i++) ci.pinIdx[c->pins[i].name] = (int)i;
        std::set<std::string> latchData;
        for (auto& l : c->latches)
            if (!l.dataIn.empty())
                try {
                    for (auto& v : liberty::parseFunction(l.dataIn).vars()) latchData.insert(v);
                } catch (liberty::Error&) {
                }
        std::map<std::tuple<int, int, bool>, int> gidx;
        for (auto& a : c->arcs) {
            auto f = ci.pinIdx.find(a.related), t = ci.pinIdx.find(a.to);
            if (f == ci.pinIdx.end() || t == ci.pinIdx.end()) continue;
            if (a.isCheck()) {
                ci.checks.push_back({t->second, f->second, &a});
                continue;
            }
            if (a.type == liberty::ArcType::Other) continue;
            // A latch's D to Q arc is not a path: the latch is timed at its clock.
            if (ci.latch && !a.isEdge() && latchData.count(a.related)) continue;
            auto key = std::make_tuple(f->second, t->second, a.isEdge());
            auto g = gidx.find(key);
            if (g == gidx.end()) {
                gidx[key] = (int)ci.groups.size();
                ci.groups.push_back({f->second, t->second, a.isEdge(), {&a}});
            } else {
                ci.groups[g->second].arcs.push_back(&a);
            }
        }
        return &(cellInfo[c] = std::move(ci));
    }

    void build(const gnl::Netlist& netlist) {
        nl = &netlist;
        size_t nPorts = nl->ports.size();
        nodes.assign(nPorts, Node());
        nodePin.assign(nPorts, nullptr);
        portLoad.assign(nPorts, 0);
        netWireCap.assign(nl->nets.size(), 0);
        std::set<std::string> unknown;
        for (size_t i = 0; i < nPorts; i++) {
            nodes[i].net = nl->ports[i].net;
            nodes[i].driver = nl->ports[i].dir != gnl::Dir::Out;
            portNames.push_back(nl->ports[i].name);
            portByName[nl->ports[i].name] = (int)i;
        }
        size_t nInst = nl->insts.size();
        instInfo.assign(nInst, nullptr);
        instPinOff.assign(nInst + 1, 0);
        instNodeOff.assign(nInst + 1, 0);
        // Bind cells (in parallel: the library map is only read), lay out the
        // node and pin tables, then fill in each instance's nodes in parallel.
        std::vector<const liberty::Cell*> bound(nInst, nullptr);
        forRange(nInst, 4096, [&](size_t b0, size_t e0) {
            for (size_t i = b0; i < e0; i++) {
                auto c = cellByName.find(nl->insts[i].cell);
                if (c != cellByName.end()) bound[i] = c->second;
            }
        });
        {
            const liberty::Cell* lastCell = nullptr;
            const CellInfo* lastInfo = nullptr;
            size_t pinOff = 0, nodeOff = nPorts;
            for (size_t i = 0; i < nInst; i++) {
                if (bound[i] != lastCell) {
                    lastCell = bound[i];
                    lastInfo = lastCell ? infoOf(lastCell) : nullptr;
                }
                instInfo[i] = lastInfo;
                instPinOff[i] = (int)pinOff;
                instNodeOff[i] = (int)nodeOff;
                if (lastInfo) pinOff += lastInfo->cell->pins.size();
                else unknown.insert(nl->insts[i].cell);
                nodeOff += nl->insts[i].conns.size();
            }
            instPinOff[nInst] = (int)pinOff;
            instNodeOff[nInst] = (int)nodeOff;
            instPinNode.assign(pinOff, -1);
            nodes.resize(nodeOff);
            nodePin.resize(nodeOff, nullptr);
        }
        std::mutex warnMutex;
        forRange(nInst, 2048, [&](size_t b0, size_t e0) {
            for (size_t i = b0; i < e0; i++) {
                const gnl::FlatInst& fi = nl->insts[i];
                const CellInfo* ci = instInfo[i];
                int n = instNodeOff[i];
                for (auto& cn : fi.conns) {
                    Node& x = nodes[n];
                    x.inst = (int)i;
                    x.net = cn.net;
                    if (ci) {
                        int pi = -1;
                        auto& pins = ci->cell->pins;
                        if (pins.size() <= 8) {
                            for (size_t k = 0; k < pins.size(); k++)
                                if (pins[k].name == cn.pin) { pi = (int)k; break; }
                        } else {
                            auto pit = ci->pinIdx.find(cn.pin);
                            if (pit != ci->pinIdx.end()) pi = pit->second;
                        }
                        if (pi < 0) {
                            std::lock_guard<std::mutex> lk(warnMutex);
                            warn("no-pin", "instance " + fi.name + ": cell " + fi.cell + " has no pin " + cn.pin);
                        } else {
                            x.pin = &pins[pi];
                            x.driver = x.pin->dir == liberty::Dir::Out || x.pin->dir == liberty::Dir::InOut;
                            x.cap = x.pin->cap;
                            instPinNode[instPinOff[i] + pi] = n;
                        }
                    }
                    nodePin[n] = &cn.pin;
                    n++;
                }
            }
        });
        for (auto& u : unknown) warn("no-cell", "cell " + u + " is not in any library: its instances are not timed");

        // Net arcs: every driver to every load of the net.
        std::vector<std::vector<int>> drv(nl->nets.size()), lds(nl->nets.size());
        for (size_t n = 0; n < nodes.size(); n++) {
            const Node& x = nodes[n];
            if (x.net < 0) continue;
            bool known = x.inst < 0 || x.pin;
            if (!known) continue;
            if (x.driver) drv[x.net].push_back((int)n);
            bool load = x.inst < 0 ? nl->ports[n].dir != gnl::Dir::In
                                   : (x.pin->dir == liberty::Dir::In || x.pin->dir == liberty::Dir::InOut);
            if (load) lds[x.net].push_back((int)n);
        }
        netDriver.assign(nl->nets.size(), -1);
        for (size_t net = 0; net < nl->nets.size(); net++) {
            if ((int)net == nl->const0 || (int)net == nl->const1) continue;
            if (!drv[net].empty()) netDriver[net] = drv[net][0];
            int cellDrivers = 0;
            for (int d : drv[net]) if (nodes[d].inst >= 0 && nodes[d].pin->dir == liberty::Dir::Out) cellDrivers++;
            if (cellDrivers > 1) warn("multi-driver", "net " + nl->nets[net] + " has " + std::to_string(cellDrivers) + " drivers");
            for (int d : drv[net])
                for (int l : lds[net]) {
                    if (d == l) continue;
                    Edge e;
                    e.from = d;
                    e.to = l;
                    e.kind = NetEdge;
                    e.ok = 0b1001;  // rise to rise, fall to fall
                    edges.push_back(e);
                }
        }
        // Cell arcs and checks.
        for (size_t i = 0; i < nInst; i++) {
            const CellInfo* ci = instInfo[i];
            if (!ci) continue;
            for (size_t g = 0; g < ci->groups.size(); g++) {
                auto& grp = ci->groups[g];
                int f = pinNode((int)i, grp.from), t = pinNode((int)i, grp.to);
                if (f < 0 || t < 0) continue;
                Edge e;
                e.from = f;
                e.to = t;
                e.kind = grp.launch ? LaunchEdge : CombEdge;
                e.group = (int)g;
                for (auto* a : grp.arcs) e.ok |= arcPairs(*a);
                edges.push_back(e);
            }
            for (auto& ck : ci->checks) {
                int d = pinNode((int)i, ck.data), c = pinNode((int)i, ck.clk);
                if (d < 0 || c < 0) continue;
                checks.push_back({d, c, (int)i, ck.arc});
                nodes[d].endpoint = true;
            }
        }
        for (size_t p = 0; p < nPorts; p++)
            if (nl->ports[p].dir != gnl::Dir::In) nodes[p].endpoint = true;
        csr();
    }

    // The (input transition, output transition) pairs an arc times, as Edge::ok bits.
    static uint8_t arcPairs(const liberty::Arc& a) {
        uint8_t ok = 0;
        auto add = [&](int irf, int orf) {
            if (orf == RISE && (a.fallOnly || a.cellRise.empty())) return;
            if (orf == FALL && (a.riseOnly || a.cellFall.empty())) return;
            ok |= 1 << (irf * 2 + orf);
        };
        if (a.isEdge()) {
            int irf = a.rising() ? RISE : FALL;
            add(irf, RISE);
            add(irf, FALL);
            return ok;
        }
        if (a.sense != liberty::Sense::Negative) { add(RISE, RISE); add(FALL, FALL); }
        if (a.sense != liberty::Sense::Positive) { add(RISE, FALL); add(FALL, RISE); }
        return ok;
    }

    void csr() {
        size_t n = nodes.size();
        finOff.assign(n + 1, 0);
        foutOff.assign(n + 1, 0);
        for (auto& e : edges) {
            finOff[e.to + 1]++;
            foutOff[e.from + 1]++;
        }
        for (size_t i = 0; i < n; i++) {
            finOff[i + 1] += finOff[i];
            foutOff[i + 1] += foutOff[i];
        }
        finIdx.assign(edges.size(), 0);
        foutIdx.assign(edges.size(), 0);
        std::vector<int> a(finOff.begin(), finOff.end() - 1), b(foutOff.begin(), foutOff.end() - 1);
        for (size_t i = 0; i < edges.size(); i++) {
            finIdx[a[edges[i].to]++] = (int)i;
            foutIdx[b[edges[i].from]++] = (int)i;
        }
    }

    // Levels: a node is one past its deepest fanin. Edges that close a
    // combinational loop are cut (and reported).
    void levelize() {
        size_t n = nodes.size();
        std::vector<int> pending(n, 0);
        for (auto& e : edges) {
            e.loopBreak = false;
            if (!e.disabled) pending[e.to]++;
        }
        levels.clear();
        loopNodes.clear();
        std::vector<int> cur, next;
        std::vector<char> done(n, 0);
        for (size_t i = 0; i < n; i++)
            if (!pending[i]) cur.push_back((int)i);
        size_t placed = 0;
        int level = 0;
        for (;;) {
            while (!cur.empty()) {
                for (int x : cur) {
                    nodes[x].level = level;
                    done[x] = 1;
                }
                placed += cur.size();
                next.clear();
                for (int x : cur)
                    for (int k = foutOff[x]; k < foutOff[x + 1]; k++) {
                        Edge& e = edges[foutIdx[k]];
                        if (e.disabled || e.loopBreak) continue;
                        if (--pending[e.to] == 0) next.push_back(e.to);
                    }
                std::sort(next.begin(), next.end());
                levels.push_back(std::move(cur));
                cur = std::move(next);
                next.clear();
                level++;
            }
            if (placed == n) break;
            // A loop: cut the edges into the lowest-numbered node left that
            // has an already placed fanin, or just the lowest-numbered.
            int pick = -1;
            for (size_t i = 0; i < n && pick < 0; i++) {
                if (done[i]) continue;
                for (int k = finOff[i]; k < finOff[i + 1]; k++) {
                    Edge& e = edges[finIdx[k]];
                    if (!e.disabled && done[e.from]) { pick = (int)i; break; }
                }
            }
            for (size_t i = 0; i < n && pick < 0; i++)
                if (!done[i]) pick = (int)i;
            for (int k = finOff[pick]; k < finOff[pick + 1]; k++) {
                Edge& e = edges[finIdx[k]];
                if (!e.disabled && !e.loopBreak && !done[e.from]) e.loopBreak = true;
            }
            pending[pick] = 0;
            loopNodes.push_back(pick);
            cur.push_back(pick);
        }
    }

    // ------------------------------------------------------------ constraints

    // The nodes an SDC object stands for.
    void nodesOf(const sdc::Obj& o, std::vector<int>& out) const {
        if (o.kind == sdc::Kind::Port || o.kind == sdc::Kind::Pin) {
            int n = findNode(o.name);
            if (n >= 0) out.push_back(n);
        } else if (o.kind == sdc::Kind::Cell) {
            int i = findInst(o.name);
            if (i >= 0)
                for (int n = instNodeOff[i]; n < instNodeOff[i + 1]; n++) out.push_back(n);
        } else if (o.kind == sdc::Kind::Net) {
            int i = findNet(o.name);
            if (i >= 0 && netDriver[i] >= 0) out.push_back(netDriver[i]);
        }
    }
    int clockIdx(const std::string& n) const {
        auto it = clockByName.find(n);
        return it == clockByName.end() ? -1 : it->second;
    }

    // Derives a generated clock's waveform from its master's.
    bool deriveGenerated(Clock& c) {
        const sdc::Clock& d = *c.def;
        int m = -1;
        if (!d.masterClock.empty()) m = clockIdx(d.masterClock);
        if (m < 0) {
            std::vector<int> mp;
            for (auto& o : d.masterPins) nodesOf(o, mp);
            for (size_t k = 0; k < clocks.size() && m < 0; k++) {
                if (&clocks[k] == &c || clocks[k].period <= 0) continue;
                for (int s : clocks[k].sources)
                    if (std::find(mp.begin(), mp.end(), s) != mp.end()) m = (int)k;
            }
        }
        if (m < 0) {
            // The only clock that is not generated, if there is exactly one.
            int cnt = 0;
            for (size_t k = 0; k < clocks.size(); k++)
                if (!clocks[k].generated && clocks[k].period > 0) { m = (int)k; cnt++; }
            if (cnt != 1) m = -1;
        }
        if (m < 0 || clocks[m].period <= 0) return false;
        const Clock& M = clocks[m];
        c.master = m;
        double P = M.period, r = M.edgeT[RISE], f = M.edgeT[FALL];
        if (!d.edges.empty() && d.edges.size() >= 3) {
            auto at = [&](size_t i) {
                int k = (int)d.edges[i] - 1;
                double t = (k % 2 == 0 ? r : f) + (k / 2) * P;
                if (i < d.edgeShift.size()) t += d.edgeShift[i] * timeUnit;
                return t;
            };
            c.edgeT[RISE] = at(0);
            c.edgeT[FALL] = at(1);
            c.period = at(2) - at(0);
        } else if (d.multiplyBy > 1) {
            c.period = P / d.multiplyBy;
            double high = d.dutyCycle > 0 ? c.period * d.dutyCycle / 100 : (f - r) / d.multiplyBy;
            c.edgeT[RISE] = r;
            c.edgeT[FALL] = r + high;
        } else {
            int n = d.divideBy > 1 ? d.divideBy : 1;
            c.period = P * n;
            c.edgeT[RISE] = r;
            c.edgeT[FALL] = n == 1 ? f : r + c.period / 2;
        }
        if (d.invert) {
            double high = c.edgeT[FALL] - c.edgeT[RISE];
            c.edgeT[RISE] = c.edgeT[FALL];
            c.edgeT[FALL] = c.edgeT[RISE] + (c.period - high);
        }
        return true;
    }

    void constrain(const sdc::Constraints& s) {
        timed = false;
        clocks.clear();
        clockByName.clear();
        clkExcl.clear();
        interUnc.clear();
        excs.clear();
        excByTo.clear();
        excGeneric.clear();
        launchAt.clear();
        launchByClk.clear();
        throughAt.clear();
        seeds.clear();
        outDelays.clear();
        drives.clear();
        inTrans.clear();
        maxTransAt.clear();
        maxCapAt.clear();
        maxTransition = maxCapacitance = -1;
        std::fill(netWireCap.begin(), netWireCap.end(), 0.0);
        std::fill(portLoad.begin(), portLoad.end(), 0.0);
        for (auto& n : nodes) n.clockSrc = n.genSrc = n.idealClk = n.hasThrough = n.hasLaunch = n.hasSeed = false;
        for (auto& e : edges) e.disabled = false;
        const double T = timeUnit, C = capUnit;

        // Clocks: the last definition of a name wins.
        for (auto& d : s.clocks) {
            if (s.clock(d.name) != &d) continue;
            Clock c;
            c.name = d.name;
            c.def = &d;
            c.generated = d.generated;
            for (auto& o : d.sources) nodesOf(o, c.sources);
            if (!d.generated) {
                c.period = d.period * T;
                c.edgeT[RISE] = d.waveform.size() >= 2 ? d.waveform[0] * T : 0;
                c.edgeT[FALL] = d.waveform.size() >= 2 ? d.waveform[1] * T : c.period / 2;
                if (c.period <= 0) {
                    warn("bad-clock", "clock " + c.name + " has no period (" + d.loc.str() + ")");
                    continue;
                }
            }
            clockByName[c.name] = (int)clocks.size();
            clocks.push_back(std::move(c));
        }
        for (int pass = 0; pass < 8; pass++) {
            bool again = false;
            for (auto& c : clocks)
                if (c.generated && c.period <= 0 && !deriveGenerated(c)) again = true;
            if (!again) break;
        }
        for (auto& c : clocks) {
            if (c.period <= 0) {
                warn("bad-clock", "generated clock " + c.name + ": no master clock found (" + c.def->loc.str() + ")");
                c.period = 0;
                continue;
            }
            for (int n : c.sources) {
                nodes[n].clockSrc = true;
                if (c.generated) nodes[n].genSrc = true;
            }
        }
        if (clocks.size() > 30000) warn("limit", "more than 30000 clocks");

        // Clock attributes.
        for (auto& o : s.propagated) {
            if (o.kind == sdc::Kind::Clock) {
                int k = clockIdx(o.name);
                if (k >= 0) clocks[k].propagated = true;
            } else {
                std::vector<int> ns;
                nodesOf(o, ns);
                for (auto& c : clocks)
                    for (int n : ns)
                        if (std::find(c.sources.begin(), c.sources.end(), n) != c.sources.end()) c.propagated = true;
            }
        }
        for (auto& l : s.latencies) {
            bool late = !(l.min || l.early), early = !(l.max || l.late);
            if (!l.min && !l.max && !l.early && !l.late) late = early = true;
            for (auto& o : l.objects) {
                if (o.kind != sdc::Kind::Clock) {
                    warn("unsupported", "set_clock_latency on a pin is not supported (" + l.loc.str() + ")");
                    continue;
                }
                int k = clockIdx(o.name);
                if (k < 0) continue;
                double* v = l.source ? clocks[k].srcLat : clocks[k].netLat;
                if (late) v[LATE] = l.value * T;
                if (early) v[EARLY] = l.value * T;
            }
        }
        for (auto& u : s.uncertainties) {
            bool su = u.setup || !u.hold, ho = u.hold || !u.setup;
            if (!u.from.empty() || !u.to.empty()) {
                for (auto& f : u.from)
                    for (auto& t : u.to) {
                        int a = clockIdx(f), b = clockIdx(t);
                        if (a < 0 || b < 0) continue;
                        auto it = interUnc.find({a, b});
                        std::pair<double, double> v = it == interUnc.end()
                            ? std::make_pair(-1.0, -1.0) : it->second;
                        if (su) v.first = u.value * T;
                        if (ho) v.second = u.value * T;
                        interUnc[{a, b}] = v;
                    }
                continue;
            }
            for (auto& o : u.objects) {
                if (o.kind != sdc::Kind::Clock) {
                    warn("unsupported", "set_clock_uncertainty on a pin is not supported (" + u.loc.str() + ")");
                    continue;
                }
                int k = clockIdx(o.name);
                if (k < 0) continue;
                if (su) clocks[k].unc[0] = u.value * T;
                if (ho) clocks[k].unc[1] = u.value * T;
            }
        }
        for (auto* c : s.of("set_clock_transition")) {
            bool r = c->has("-rise") || !c->has("-fall"), f = c->has("-fall") || !c->has("-rise");
            for (auto& o : c->objs("clocks")) {
                int k = clockIdx(o.name);
                if (k < 0) continue;
                if (r) clocks[k].trans[RISE] = c->num("transition") * T;
                if (f) clocks[k].trans[FALL] = c->num("transition") * T;
            }
        }
        clkExcl.assign(clocks.size(), std::vector<char>(clocks.size(), 0));
        for (auto& g : s.clockGroups) {
            std::vector<std::vector<int>> grp;
            std::vector<char> inAny(clocks.size(), 0);
            for (auto& names : g.groups) {
                grp.emplace_back();
                for (auto& n : names) {
                    int k = clockIdx(n);
                    if (k >= 0) { grp.back().push_back(k); inAny[k] = 1; }
                }
            }
            if (grp.size() == 1) {  // one group: against every other clock
                grp.emplace_back();
                for (size_t k = 0; k < clocks.size(); k++) if (!inAny[k]) grp.back().push_back((int)k);
            }
            for (size_t a = 0; a < grp.size(); a++)
                for (size_t b = 0; b < grp.size(); b++)
                    if (a != b)
                        for (int x : grp[a])
                            for (int y : grp[b]) clkExcl[x][y] = 1;
        }

        // Path exceptions.
        int bits = 0;
        for (auto& d : s.exceptions) {
            if (d.type == sdc::Exception::GroupPath) continue;
            Exc e;
            e.def = &d;
            e.type = d.type;
            e.value = d.type == sdc::Exception::Multicycle ? d.value : d.value * T;
            e.setup = d.setup || !d.hold;
            e.hold = d.hold || !d.setup;
            if (d.emptyEndpoint) e.ignored = true;  // an argument that names nothing matches nothing
            std::vector<int> fromNodes, toNodes;
            for (auto& o : d.path.from) {
                if (o.kind == sdc::Kind::Clock) {
                    int k = clockIdx(o.name);
                    if (k >= 0) e.fromClks.push_back(k);
                } else {
                    nodesOf(o, fromNodes);
                }
            }
            for (auto& o : d.path.to) {
                if (o.kind == sdc::Kind::Clock) {
                    int k = clockIdx(o.name);
                    if (k >= 0) e.toClks.push_back(k);
                } else {
                    nodesOf(o, toNodes);
                }
            }
            e.fromPins = !fromNodes.empty();
            e.toPins = !toNodes.empty();
            e.hasTo = !d.path.to.empty();
            bool hasFrom = !d.path.from.empty();
            if (hasFrom && e.fromClks.empty() && fromNodes.empty()) e.ignored = true;
            if (e.hasTo && e.toClks.empty() && toNodes.empty()) e.ignored = true;
            int need = (e.fromPins || (hasFrom && !d.path.through.empty()) ? 1 : 0) + (int)d.path.through.size();
            if (!e.ignored && bits + need > Mask::BITS) {
                e.ignored = true;
                warn("limit", "too many path exceptions with pins in -from or -through: the one at " + d.loc.str() + " is ignored");
            }
            int idx = (int)excs.size();
            if (!e.ignored) {
                int prev = -1;
                if (need > (int)d.path.through.size()) {
                    e.fromBit = bits++;
                    prev = e.fromBit;
                    for (int n : fromNodes) {
                        launchAt[n].push_back({idx, e.fromBit});
                        nodes[n].hasLaunch = true;
                    }
                    if (!e.fromClks.empty()) launchByClk.push_back(idx);
                }
                for (auto& th : d.path.through) {
                    int b = bits++;
                    std::vector<int> ns;
                    for (auto& o : th) nodesOf(o, ns);
                    for (int n : ns) {
                        throughAt[n].push_back({idx, prev, b});
                        nodes[n].hasThrough = true;
                    }
                    e.throughBits.push_back(b);
                    prev = b;
                }
                e.lastBit = prev;
                if (e.toPins) for (int n : toNodes) excByTo[n].push_back(idx);
                if (!e.toPins || !e.toClks.empty()) excGeneric.push_back(idx);
            }
            excs.push_back(std::move(e));
        }

        // Ports: transitions, driving cells, loads, input and output delays.
        for (auto* c : s.of("set_input_transition")) {
            bool r = c->has("-rise") || !c->has("-fall"), f = c->has("-fall") || !c->has("-rise");
            bool mx = c->has("-max") || !c->has("-min"), mn = c->has("-min") || !c->has("-max");
            double v = c->num("transition") * T;
            for (auto& o : c->objs("objects")) {
                std::vector<int> ns;
                nodesOf(o, ns);
                for (int n : ns) {
                    auto it = inTrans.find(n);
                    if (it == inTrans.end()) it = inTrans.emplace(n, std::array<double, 4>{0, 0, 0, 0}).first;
                    if (mx && r) it->second[LATE * 2 + RISE] = v;
                    if (mx && f) it->second[LATE * 2 + FALL] = v;
                    if (mn && r) it->second[EARLY * 2 + RISE] = v;
                    if (mn && f) it->second[EARLY * 2 + FALL] = v;
                }
            }
        }
        for (auto* c : s.of("set_driving_cell")) {
            auto lc = cellByName.find(c->str("-lib_cell"));
            if (lc == cellByName.end()) {
                warn("no-cell", "set_driving_cell: no library cell " + c->str("-lib_cell") + " (" + c->loc.str() + ")");
                continue;
            }
            Drive d;
            d.cell = lc->second;
            d.pin = c->str("-pin");
            d.fromPin = c->str("-from_pin");
            d.inSlew[RISE] = c->num("-input_transition_rise") * T;
            d.inSlew[FALL] = c->num("-input_transition_fall") * T;
            for (auto& o : c->objs("objects")) {
                std::vector<int> ns;
                nodesOf(o, ns);
                for (int n : ns) drives[n] = d;
            }
        }
        for (auto* c : s.of("set_load")) {
            double v = c->num("value") * C;
            for (auto& o : c->objs("objects")) {
                if (o.kind == sdc::Kind::Net) {
                    int i = findNet(o.name);
                    if (i >= 0) netWireCap[i] = v;
                } else {
                    int n = findNode(o.name);
                    if (n >= 0 && isPort(n)) portLoad[n] = v;
                }
            }
        }
        for (auto* c : s.of("set_max_transition")) {
            auto objs = c->objs("objects");
            double v = c->num("value") * T;
            bool design = objs.empty();
            for (auto& o : objs) {
                if (o.kind == sdc::Kind::Clock) continue;
                std::vector<int> ns;
                nodesOf(o, ns);
                for (int n : ns) maxTransAt[n] = v;
                if (ns.empty()) design = true;  // current_design
            }
            if (design) maxTransition = v;
        }
        for (auto* c : s.of("set_max_capacitance")) {
            auto objs = c->objs("objects");
            double v = c->num("value") * C;
            bool design = objs.empty();
            for (auto& o : objs) {
                if (o.kind == sdc::Kind::Clock) continue;
                std::vector<int> ns;
                nodesOf(o, ns);
                for (int n : ns) maxCapAt[n] = v;
                if (ns.empty()) design = true;
            }
            if (design) maxCapacitance = v;
        }
        for (auto* c : s.of("set_disable_timing")) {
            std::string from = c->str("-from"), to = c->str("-to");
            for (auto& o : c->objs("objects")) {
                if (o.kind == sdc::Kind::Cell || o.kind == sdc::Kind::LibCell) {
                    for (auto& e : edges) {
                        if (e.kind == NetEdge) continue;
                        int inst = nodes[e.from].inst;
                        bool hit = o.kind == sdc::Kind::Cell ? nl->insts[inst].name == o.name : nl->insts[inst].cell == o.name;
                        if (!hit) continue;
                        if (!from.empty() && *nodePin[e.from] != from) continue;
                        if (!to.empty() && *nodePin[e.to] != to) continue;
                        e.disabled = true;
                    }
                } else {
                    int n = findNode(o.name);
                    if (n < 0) continue;
                    for (int k = finOff[n]; k < finOff[n + 1]; k++) edges[finIdx[k]].disabled = true;
                    for (int k = foutOff[n]; k < foutOff[n + 1]; k++) edges[foutIdx[k]].disabled = true;
                }
            }
        }
        if (!s.caseAnalysis.empty())
            warn("unsupported", "set_case_analysis is read but not applied yet: constants do not disable arcs");

        // Clock seeds.
        for (size_t k = 0; k < clocks.size(); k++) {
            const Clock& c = clocks[k];
            if (c.period <= 0) continue;
            for (int n : c.sources)
                for (int edge = 0; edge < 2; edge++) {
                    Seed sd;
                    sd.key.clk = (int16_t)k;
                    sd.key.edge = (uint8_t)edge;
                    sd.key.isClk = 1;
                    sd.input = false;
                    sd.ext[0] = sd.ext[1] = 0;
                    for (int m = 0; m < 2; m++) {
                        sd.arr[m][edge] = c.srcLat[m] + (c.propagated ? 0 : c.netLat[m]);
                        sd.arr[m][!edge] = m == LATE ? -INF : INF;
                    }
                    seeds[n].push_back(sd);
                    nodes[n].hasSeed = true;
                }
        }
        // Input and output delays.
        for (auto& d : s.ioDelays) {
            int k = clockIdx(d.clock);
            if (k < 0 || clocks[k].period <= 0) {
                warn("no-clock", std::string(d.input ? "set_input_delay" : "set_output_delay") + " without a known clock is ignored (" + d.loc.str() + ")");
                continue;
            }
            bool mx = d.max || !d.min, mn = d.min || !d.max;
            bool r = d.rise || !d.fall, f = d.fall || !d.rise;
            int edge = d.clockFall ? FALL : RISE;
            std::vector<int> ns;
            for (auto& o : d.ports) nodesOf(o, ns);
            for (int n : ns) {
                if (d.input) {
                    const Clock& c = clocks[k];
                    TagKey key;
                    key.clk = (int16_t)k;
                    key.edge = (uint8_t)edge;
                    key.isClk = 0;
                    auto& list = seeds[n];
                    Seed* sd = nullptr;
                    for (auto& x : list)
                        if (x.input && x.key.clk == key.clk && x.key.edge == key.edge) sd = &x;
                    if (!sd) {
                        Seed nw;
                        nw.key = key;
                        nw.input = true;
                        nw.ext[0] = nw.ext[1] = 0;
                        for (int m = 0; m < 2; m++)
                            for (int rf = 0; rf < 2; rf++) nw.arr[m][rf] = m == LATE ? -INF : INF;
                        list.push_back(nw);
                        sd = &list.back();
                    }
                    for (int m = 0; m < 2; m++) {
                        if (!(m == LATE ? mx : mn)) continue;
                        double lat = c.srcLat[m] + c.netLat[m];
                        sd->ext[m] = d.value * T;
                        if (r) sd->arr[m][RISE] = lat + d.value * T;
                        if (f) sd->arr[m][FALL] = lat + d.value * T;
                    }
                    nodes[n].hasSeed = true;
                } else {
                    auto& list = outDelays[n];
                    OutDelay* od = nullptr;
                    for (auto& x : list)
                        if (x.clk == k && x.edge == edge) od = &x;
                    if (!od) {
                        list.push_back({k, edge, {0, 0}, {false, false}});
                        od = &list.back();
                    }
                    if (mx) { od->v[LATE] = d.value * T; od->has[LATE] = true; }
                    if (mn) { od->v[EARLY] = d.value * T; od->has[EARLY] = true; }
                }
            }
        }
        // The launch masks of input delay seeds are set when the tags are made.

        // The networks of ideal clocks: zero delay, the clock's transition.
        std::vector<int> stack;
        for (auto& c : clocks) {
            if (c.propagated || c.period <= 0) continue;
            for (int n : c.sources)
                if (!nodes[n].idealClk) { nodes[n].idealClk = true; stack.push_back(n); }
        }
        while (!stack.empty()) {
            int n = stack.back();
            stack.pop_back();
            for (int k = foutOff[n]; k < foutOff[n + 1]; k++) {
                const Edge& e = edges[foutIdx[k]];
                if (e.disabled || e.kind == LaunchEdge) continue;
                if (!nodes[e.to].idealClk) { nodes[e.to].idealClk = true; stack.push_back(e.to); }
            }
        }
        // A propagated clock's network is not ideal even where an ideal clock shares it.
        for (auto& c : clocks) {
            if (!c.propagated) continue;
            for (int n : c.sources) stack.push_back(n);
            std::unordered_set<int> seen(stack.begin(), stack.end());
            while (!stack.empty()) {
                int n = stack.back();
                stack.pop_back();
                nodes[n].idealClk = false;
                for (int k = foutOff[n]; k < foutOff[n + 1]; k++) {
                    const Edge& e = edges[foutIdx[k]];
                    if (e.disabled || e.kind == LaunchEdge) continue;
                    if (seen.insert(e.to).second) stack.push_back(e.to);
                }
            }
        }
    }

    // ------------------------------------------------------------- delays

    // The capacitance a driver node sees.
    void computeLoads() {
        std::vector<double> netCap(nl->nets.size(), 0);
        for (size_t n = 0; n < nodes.size(); n++) {
            const Node& x = nodes[n];
            if (x.net < 0) continue;
            if (x.inst < 0) {
                if (nl->ports[n].dir != gnl::Dir::In) netCap[x.net] += portLoad[n];
            } else if (x.pin && x.pin->dir != liberty::Dir::Out) {
                netCap[x.net] += x.cap;
            }
        }
        for (size_t n = 0; n < nodes.size(); n++) {
            Node& x = nodes[n];
            x.load = x.driver && x.net >= 0 ? netCap[x.net] + netWireCap[x.net] : 0;
        }
    }

    // Slews and arc delays, level by level.
    void computeDelays() {
        computeLoads();
        // The transition of the ideal clocks reaching a node: the largest set.
        double idealTrans[2] = {0, 0};
        for (auto& c : clocks)
            if (!c.propagated)
                for (int rf = 0; rf < 2; rf++) idealTrans[rf] = std::max(idealTrans[rf], c.trans[rf]);
        auto doNode = [&](int n) {
            Node& x = nodes[n];
            double s[2][2] = {{-INF, -INF}, {INF, INF}};
            bool any[2][2] = {{false, false}, {false, false}};
            for (int k = finOff[n]; k < finOff[n + 1]; k++) {
                Edge& e = edges[finIdx[k]];
                if (e.disabled || e.loopBreak) continue;
                const Node& f = nodes[e.from];
                if (e.kind == NetEdge) {
                    for (int m = 0; m < 2; m++)
                        for (int rf = 0; rf < 2; rf++) {
                            double v = f.slew[m][rf];
                            if (m == LATE ? v > s[m][rf] : v < s[m][rf]) s[m][rf] = v;
                            any[m][rf] = true;
                            e.d[m][rf][rf] = 0;
                        }
                    continue;
                }
                const CellInfo::Group& g = instInfo[x.inst]->groups[e.group];
                for (int m = 0; m < 2; m++)
                    for (int irf = 0; irf < 2; irf++)
                        for (int orf = 0; orf < 2; orf++) {
                            if (!e.has(irf, orf)) continue;
                            double best = m == LATE ? -INF : INF, bestSlew = best;
                            for (auto* a : g.arcs) {
                                if (!((arcPairs(*a) >> (irf * 2 + orf)) & 1)) continue;
                                const liberty::Table& dt = orf == RISE ? a->cellRise : a->cellFall;
                                const liberty::Table& tt = orf == RISE ? a->riseTrans : a->fallTrans;
                                double dv = dt.lookup(f.slew[m][irf], x.load);
                                double tv = tt.empty() ? 0 : tt.lookup(f.slew[m][irf], x.load);
                                if (m == LATE ? dv > best : dv < best) best = dv;
                                if (m == LATE ? tv > bestSlew : tv < bestSlew) bestSlew = tv;
                            }
                            e.d[m][irf][orf] = best;
                            if (m == LATE ? bestSlew > s[m][orf] : bestSlew < s[m][orf]) s[m][orf] = bestSlew;
                            any[m][orf] = true;
                        }
            }
            for (int m = 0; m < 2; m++)
                for (int rf = 0; rf < 2; rf++) x.slew[m][rf] = any[m][rf] ? std::max(0.0, s[m][rf]) : 0;
            if (x.inst < 0 && x.driver) {
                auto it = inTrans.find(n);
                if (it != inTrans.end())
                    for (int m = 0; m < 2; m++)
                        for (int rf = 0; rf < 2; rf++) x.slew[m][rf] = it->second[m * 2 + rf];
                auto dr = drives.find(n);
                if (dr != drives.end()) {
                    const liberty::Arc* a = driveArc(dr->second);
                    if (a)
                        for (int m = 0; m < 2; m++)
                            for (int rf = 0; rf < 2; rf++) {
                                const liberty::Table& tt = rf == RISE ? a->riseTrans : a->fallTrans;
                                int irf = a->sense == liberty::Sense::Negative ? !rf : rf;
                                if (!tt.empty()) x.slew[m][rf] = tt.lookup(dr->second.inSlew[irf], x.load);
                            }
                }
            }
            if (x.idealClk)
                for (int m = 0; m < 2; m++)
                    for (int rf = 0; rf < 2; rf++) x.slew[m][rf] = idealTrans[rf];
        };
        for (auto& lv : levels) runLevel(lv, doNode);
    }

    const liberty::Arc* driveArc(const Drive& d) const {
        const liberty::Arc* best = nullptr;
        for (auto& a : d.cell->arcs) {
            if (a.isCheck() || a.type == liberty::ArcType::Other) continue;
            if (!d.pin.empty() && a.to != d.pin) continue;
            if (!d.fromPin.empty() && a.related != d.fromPin) continue;
            const liberty::Pin* p = d.cell->pin(a.to);
            if (!p || p->dir == liberty::Dir::In) continue;
            if (!best) best = &a;
        }
        return best;
    }
    // The extra delay a driving cell adds at an input port: its delay into the
    // real load, less its delay into no load.
    double driveDelay(int n, int m, int rf) const {
        auto dr = drives.find(n);
        if (dr == drives.end()) return 0;
        const liberty::Arc* a = driveArc(dr->second);
        if (!a) return 0;
        const liberty::Table& dt = rf == RISE ? a->cellRise : a->cellFall;
        if (dt.empty()) return 0;
        int irf = a->sense == liberty::Sense::Negative ? !rf : rf;
        (void)m;
        return dt.lookup(dr->second.inSlew[irf], nodes[n].load) - dt.lookup(dr->second.inSlew[irf], 0);
    }

    template <class F>
    void forRange(size_t n, size_t grain, F&& f) {
        if (pool) pool->run(n, grain, f);
        else f((size_t)0, n);
    }
    template <class F>
    void runLevel(const std::vector<int>& lv, F&& f) {
        if (pool && lv.size() >= 512) {
            pool->run(lv.size(), 128, [&](size_t b, size_t e) {
                for (size_t i = b; i < e; i++) f(lv[i]);
            });
        } else {
            for (int n : lv) f(n);
        }
    }

    // ------------------------------------------------------------ arrivals

    static Tag& tagFor(std::vector<Tag>& list, const TagKey& key) {
        for (auto& t : list)
            if (t.key == key) return t;
        list.emplace_back();
        list.back().key = key;
        return list.back();
    }

    Mask launchMask(int fromNode, int toNode, int clk) const {
        Mask m;
        for (int n : {fromNode, toNode}) {
            if (n < 0 || !nodes[n].hasLaunch) continue;
            auto it = launchAt.find(n);
            if (it != launchAt.end())
                for (auto& [exc, bit] : it->second) m.set(bit);
        }
        for (int x : launchByClk)
            if (std::find(excs[x].fromClks.begin(), excs[x].fromClks.end(), clk) != excs[x].fromClks.end())
                m.set(excs[x].fromBit);
        return m;
    }

    void applyThrough(int n, TagKey& key) const {
        auto it = throughAt.find(n);
        if (it == throughAt.end()) return;
        for (auto& th : it->second) {
            const Exc& e = excs[th.exc];
            if (th.need >= 0) {
                if (!key.exc.test(th.need)) continue;
            } else if (!e.fromClks.empty()) {
                if (std::find(e.fromClks.begin(), e.fromClks.end(), key.clk) == e.fromClks.end()) continue;
            }
            key.exc.set(th.set);
        }
    }

    void propagate() {
        tags.assign(nodes.size(), {});
        auto doNode = [&](int n) {
            Node& x = nodes[n];
            std::vector<Tag>& out = tags[n];
            for (int k = finOff[n]; k < finOff[n + 1]; k++) {
                int ei = finIdx[k];
                const Edge& e = edges[ei];
                if (e.disabled || e.loopBreak) continue;
                const std::vector<Tag>& in = tags[e.from];
                for (size_t ti = 0; ti < in.size(); ti++) {
                    const Tag& t = in[ti];
                    TagKey key = t.key;
                    bool zero = false;
                    if (t.key.isClk) {
                        if (e.kind == LaunchEdge) {
                            key.isClk = 0;
                            key.exc = launchMask(e.from, n, key.clk);
                        } else {
                            zero = !clocks[key.clk].propagated;
                        }
                    } else if (e.kind == LaunchEdge) {
                        continue;  // data does not pass through a register's clock pin
                    }
                    if (x.hasThrough && !key.isClk) applyThrough(n, key);
                    Tag* o = nullptr;
                    for (int m = 0; m < 2; m++)
                        for (int irf = 0; irf < 2; irf++) {
                            double a = t.arr[m][irf];
                            if (!valid(a)) continue;
                            for (int orf = 0; orf < 2; orf++) {
                                if (!e.has(irf, orf)) continue;
                                double v = a + (zero ? 0 : e.d[m][irf][orf]);
                                if (!o) o = &tagFor(out, key);
                                if (m == LATE ? v > o->arr[m][orf] : v < o->arr[m][orf]) {
                                    o->arr[m][orf] = v;
                                    o->prevEdge[m][orf] = ei;
                                    o->prevTag[m][orf] = (int32_t)ti;
                                    o->prevRf[m][orf] = (uint8_t)irf;
                                }
                            }
                        }
                }
            }
            if (x.genSrc) {
                // A generated clock starts here. With a propagated master its
                // source latency is the master's arrival at this pin; the
                // other clocks stop.
                double lat[2][2];
                bool have = false;
                for (int m = 0; m < 2; m++)
                    for (int rf = 0; rf < 2; rf++) lat[m][rf] = m == LATE ? -INF : INF;
                auto sit = seeds.find(n);
                for (auto& t : out)
                    for (auto& sd : sit->second) {
                        if (sd.input || clocks[sd.key.clk].master != t.key.clk) continue;
                        if (!clocks[t.key.clk].propagated) continue;
                        for (int m = 0; m < 2; m++)
                            for (int rf = 0; rf < 2; rf++) {
                                double v = t.arr[m][rf];
                                if (!valid(v)) continue;
                                if (m == LATE ? v > lat[m][rf] : v < lat[m][rf]) lat[m][rf] = v;
                                have = true;
                            }
                    }
                out.erase(std::remove_if(out.begin(), out.end(), [](const Tag& t) { return t.key.isClk; }), out.end());
                for (auto& sd : sit->second) {
                    if (sd.input) continue;
                    Tag& t = tagFor(out, sd.key);
                    for (int m = 0; m < 2; m++) {
                        int rf = sd.key.edge;
                        double v = sd.arr[m][rf] + (have && valid(lat[m][rf]) ? lat[m][rf] : 0);
                        if (m == LATE ? v > t.arr[m][rf] : v < t.arr[m][rf]) t.arr[m][rf] = v;
                    }
                }
            }
            if (x.hasSeed) {  // clock sources and input delays
                auto it = seeds.find(n);
                if (it != seeds.end())
                    for (auto& sd : it->second) {
                        if (x.genSrc && !sd.input) continue;  // done above
                        TagKey key = sd.key;
                        if (sd.input) key.exc = launchMask(n, -1, key.clk);
                        if (x.hasThrough && !key.isClk) applyThrough(n, key);
                        Tag& t = tagFor(out, key);
                        for (int m = 0; m < 2; m++)
                            for (int rf = 0; rf < 2; rf++) {
                                if (!valid(sd.arr[m][rf])) continue;
                                double v = sd.arr[m][rf] + (sd.input ? driveDelay(n, m, rf) : 0);
                                if (m == LATE ? v > t.arr[m][rf] : v < t.arr[m][rf]) t.arr[m][rf] = v;
                            }
                    }
            }
        };
        for (auto& lv : levels) runLevel(lv, doNode);
    }

    // ------------------------------------------------------------- checks

    // The setup relation: the launch edge and the first capture edge after it.
    void relation(const Clock& L, int le, const Clock& Cc, int ce, double& launch, double& capture) const {
        const double eps = 1e-9;
        double tl = L.edgeT[le], tc = Cc.edgeT[ce];
        if (std::fabs(L.period - Cc.period) < eps) {
            double c = tc;
            while (c <= tl + eps) c += Cc.period;
            while (c - Cc.period > tl + eps) c -= Cc.period;
            launch = tl;
            capture = c;
            return;
        }
        // Different periods: over the common period, the launch edge with
        // the closest capture edge after it.
        long long a = std::llround(L.period * 1e6), b = std::llround(Cc.period * 1e6);
        long long g = std::gcd(a, b);
        long long nL = g > 0 ? b / g : 1;
        if (nL > 1000) nL = 1000;
        double best = INF;
        for (long long i = 0; i < nL; i++) {
            double t = tl + (double)i * L.period;
            double k = std::floor((t - tc + eps) / Cc.period) + 1;
            double c = tc + k * Cc.period;
            if (c - t < best - eps) {
                best = c - t;
                launch = t;
                capture = c;
            }
        }
    }

    struct Resolved {
        bool falsePath = false, pathDelay = false;
        double delay = 0;
        int mcSetup = 1, mcHold = 0;
        bool setupStart = false, holdEnd = false;
        int exc = -1;
    };

    // The exceptions that apply to a path arriving with `key` at `node`,
    // captured by capClk, for late (setup) or early (hold) analysis.
    Resolved resolve(const TagKey& key, int node, int capClk, int rf, bool late) {
        Resolved r;
        if (excs.empty()) return r;
        int rank = 0;  // 3 false path, 2 path delay, 1 multicycle
        auto consider = [&](int xi) {
            Exc& e = excs[xi];
            if (e.ignored) return;
            if (late ? !e.setup : !e.hold) {
                // A multicycle -hold number is used by the hold analysis only;
                // a -setup one moves both.
                if (!(e.type == sdc::Exception::Multicycle && !late && e.setup)) return;
            }
            if (e.type == sdc::Exception::MaxDelay && !late) return;
            if (e.type == sdc::Exception::MinDelay && late) return;
            const sdc::PathSpec& p = e.def->path;
            if (e.lastBit >= 0) {
                if (!key.exc.test(e.lastBit)) return;
            } else if (!e.fromClks.empty()) {
                if (std::find(e.fromClks.begin(), e.fromClks.end(), key.clk) == e.fromClks.end()) return;
            }
            if (p.riseFrom && key.edge != RISE) return;
            if (p.fallFrom && key.edge != FALL) return;
            if (e.hasTo) {
                bool hit = false;
                if (e.toPins) {
                    auto it = excByTo.find(node);
                    hit = it != excByTo.end() && std::find(it->second.begin(), it->second.end(), xi) != it->second.end();
                }
                if (!hit && std::find(e.toClks.begin(), e.toClks.end(), capClk) != e.toClks.end()) hit = true;
                if (!hit) return;
            }
            if (p.riseTo && rf != RISE) return;
            if (p.fallTo && rf != FALL) return;
            switch (e.type) {
                case sdc::Exception::FalsePath:
                    r.falsePath = true;
                    rank = 3;
                    r.exc = xi;
                    break;
                case sdc::Exception::MaxDelay:
                case sdc::Exception::MinDelay:
                    if (rank < 2 || (rank == 2 && (late ? e.value < r.delay : e.value > r.delay))) {
                        if (rank < 3) {
                            r.pathDelay = true;
                            r.delay = e.value;
                            rank = 2;
                            r.exc = xi;
                        }
                    }
                    break;
                case sdc::Exception::Multicycle:
                    if (e.def->setup || !e.def->hold) {
                        r.mcSetup = (int)e.value;
                        r.setupStart = e.def->start;
                    } else {
                        r.mcHold = (int)e.value;
                        r.holdEnd = e.def->end;
                    }
                    if (rank < 1) { rank = 1; r.exc = xi; }
                    break;
                default: break;
            }
        };
        // Exceptions are applied in file order, so a later multicycle wins.
        auto it = excByTo.find(node);
        if (it == excByTo.end()) {
            for (int xi : excGeneric) consider(xi);
        } else {
            std::vector<int> all(excGeneric);
            all.insert(all.end(), it->second.begin(), it->second.end());
            std::sort(all.begin(), all.end());
            all.erase(std::unique(all.begin(), all.end()), all.end());
            for (int xi : all) consider(xi);
        }
        return r;
    }

    double uncertainty(int from, int to, bool setup) const {
        auto it = interUnc.find({from, to});
        if (it != interUnc.end()) {
            double v = setup ? it->second.first : it->second.second;
            if (v >= 0) return v;
        }
        return clocks[to].unc[setup ? 0 : 1];
    }

    // Times one data tag against one capture edge; keeps the worst per
    // (kind, capture clock) in `best`.
    void timeEnd(std::vector<PathEnd>& best, EndKind kind, int node, int tagIdx, int rf, int capClk, int capEdge,
                 double capArr, double lib, double ext, int clkNode, int clkTag, int clkRf) {
        const Tag& t = tags[node][tagIdx];
        bool late = isLate(kind);
        double a = t.arr[late ? LATE : EARLY][rf];
        if (!valid(a)) return;
        int lc = t.key.clk;
        if (clkExcl[lc][capClk]) return;
        Resolved r = resolve(t.key, node, capClk, rf, late);
        if (r.falsePath) return;
        const Clock& L = clocks[lc];
        const Clock& Cc = clocks[capClk];
        PathEnd pe;
        pe.kind = kind;
        pe.node = node;
        pe.tag = tagIdx;
        pe.rf = rf;
        pe.clkNode = clkNode;
        pe.clkTag = clkTag;
        pe.clkRf = clkRf;
        pe.launchClk = lc;
        pe.capClk = capClk;
        pe.launchEdge = t.key.edge;
        pe.capEdge = capEdge;
        pe.capArr = capArr;
        pe.lib = lib;
        pe.ext = ext;
        pe.exc = r.exc;
        pe.unc = uncertainty(lc, capClk, late);
        if (r.pathDelay) {
            pe.pathDelay = true;
            pe.pathDelayValue = r.delay;
            pe.launchT = 0;
            pe.capT = 0;
            pe.arrival = a;
            if (late) pe.required = r.delay + capArr - pe.unc - lib - ext;
            else pe.required = r.delay + capArr + pe.unc + lib - ext;
        } else {
            double launch = 0, capture = 0;
            relation(L, t.key.edge, Cc, capEdge, launch, capture);
            if (r.mcSetup != 1) {
                if (r.setupStart) launch -= (r.mcSetup - 1) * L.period;
                else capture += (r.mcSetup - 1) * Cc.period;
            }
            if (!late) {
                capture -= Cc.period;
                if (r.mcHold != 0) {
                    if (r.holdEnd) capture -= r.mcHold * Cc.period;
                    else launch += r.mcHold * L.period;
                }
            }
            pe.launchT = launch;
            pe.capT = capture;
            pe.arrival = launch + a;
            if (late) pe.required = capture + capArr - pe.unc - lib - ext;
            else pe.required = capture + capArr + pe.unc + lib - ext;
        }
        pe.slack = late ? pe.required - pe.arrival : pe.arrival - pe.required;
        for (auto& b : best)
            if (b.kind == kind && b.capClk == capClk) {
                if (pe.slack < b.slack) b = pe;
                return;
            }
        best.push_back(pe);
    }

    void runChecks() {
        ends.clear();
        unclocked.clear();
        unconstrained.clear();
        noInputDelay.clear();
        size_t nChecks = checks.size(), nPorts = nl->ports.size();
        std::vector<std::vector<PathEnd>> perCheck(nChecks), perPort(nPorts);
        std::vector<char> noClock(nChecks, 0);
        auto doCheck = [&](size_t ci) {
            const Check& ck = checks[ci];
            const liberty::Arc& a = *ck.arc;
            int crf = a.rising() ? RISE : FALL;
            EndKind kind = a.type == liberty::ArcType::SetupRising || a.type == liberty::ArcType::SetupFalling ? EndKind::Setup
                         : a.type == liberty::ArcType::HoldRising || a.type == liberty::ArcType::HoldFalling ? EndKind::Hold
                         : a.type == liberty::ArcType::RecoveryRising || a.type == liberty::ArcType::RecoveryFalling ? EndKind::Recovery
                         : EndKind::Removal;
            bool late = isLate(kind);
            const std::vector<Tag>& ct = tags[ck.clk];
            const std::vector<Tag>& dt = tags[ck.data];
            bool clocked = false;
            for (size_t c = 0; c < ct.size(); c++) {
                if (!ct[c].key.isClk) continue;
                double capArr = ct[c].arr[late ? EARLY : LATE][crf];
                if (!valid(capArr)) continue;
                clocked = true;
                for (size_t d = 0; d < dt.size(); d++)
                    for (int rf = 0; rf < 2; rf++) {
                        const liberty::Table& tb = rf == RISE ? a.riseCons : a.fallCons;
                        if (tb.empty()) continue;
                        int m = late ? LATE : EARLY;
                        double lib = tb.constraint(nodes[ck.data].slew[m][rf], nodes[ck.clk].slew[late ? EARLY : LATE][crf]);
                        timeEnd(perCheck[ci], kind, ck.data, (int)d, rf, ct[c].key.clk, ct[c].key.edge, capArr, lib, 0,
                                ck.clk, (int)c, crf);
                    }
            }
            if (!clocked) noClock[ci] = 1;
        };
        auto doPort = [&](size_t p) {
            auto it = outDelays.find((int)p);
            if (it == outDelays.end()) return;
            const std::vector<Tag>& dt = tags[p];
            for (auto& od : it->second) {
                const Clock& c = clocks[od.clk];
                for (int m = 0; m < 2; m++) {
                    if (!od.has[m]) continue;
                    // The capture clock is ideal outside the design: its latencies.
                    double capArr = c.srcLat[m == LATE ? EARLY : LATE] + c.netLat[m == LATE ? EARLY : LATE];
                    for (size_t d = 0; d < dt.size(); d++)
                        for (int rf = 0; rf < 2; rf++)
                            timeEnd(perPort[p], m == LATE ? EndKind::OutSetup : EndKind::OutHold, (int)p, (int)d, rf,
                                    od.clk, od.edge, capArr, 0, od.v[m], -1, -1, RISE);
                }
            }
        };
        if (pool && nChecks >= 1024) {
            pool->run(nChecks, 256, [&](size_t b, size_t e) { for (size_t i = b; i < e; i++) doCheck(i); });
        } else {
            for (size_t i = 0; i < nChecks; i++) doCheck(i);
        }
        for (size_t p = 0; p < nPorts; p++) doPort(p);

        std::vector<char> hasEnd(nodes.size(), 0);
        std::set<int> unclk;
        for (size_t ci = 0; ci < nChecks; ci++) {
            if (noClock[ci]) unclk.insert(checks[ci].clk);
            for (auto& pe : perCheck[ci]) {
                hasEnd[pe.node] = 1;
                ends.push_back(pe);
            }
        }
        for (size_t p = 0; p < nPorts; p++)
            for (auto& pe : perPort[p]) {
                hasEnd[pe.node] = 1;
                ends.push_back(pe);
            }
        unclocked.assign(unclk.begin(), unclk.end());
        for (size_t n = 0; n < nodes.size(); n++)
            if (nodes[n].endpoint && !hasEnd[n]) unconstrained.push_back((int)n);
        for (size_t p = 0; p < nPorts; p++) {
            if (nl->ports[p].dir != gnl::Dir::In || nodes[p].clockSrc) continue;
            auto it = seeds.find((int)p);
            bool has = false;
            if (it != seeds.end())
                for (auto& sd : it->second) if (sd.input) has = true;
            if (!has && foutOff[p + 1] > foutOff[p]) noInputDelay.push_back((int)p);
        }
        // Worst first; ties by name so the order does not depend on anything else.
        std::stable_sort(ends.begin(), ends.end(), [&](const PathEnd& a, const PathEnd& b) {
            if (a.slack != b.slack) return a.slack < b.slack;
            return a.node < b.node;
        });
    }

    void designRules() {
        transViol.clear();
        capViol.clear();
        double libTrans = -1;
        for (auto* L : libs)
            if (L->defaultMaxTransition > 0) libTrans = libTrans < 0 ? L->defaultMaxTransition : std::min(libTrans, L->defaultMaxTransition);
        for (size_t n = 0; n < nodes.size(); n++) {
            const Node& x = nodes[n];
            if (x.net < 0) continue;
            double lim = -1;
            auto tighter = [&](double v) { if (v > 0 && (lim < 0 || v < lim)) lim = v; };
            if (x.pin) tighter(x.pin->maxTransition);
            if (lim < 0) tighter(libTrans);
            tighter(maxTransition);
            auto it = maxTransAt.find((int)n);
            if (it != maxTransAt.end()) tighter(it->second);
            if (lim > 0) {
                double act = std::max(x.slew[LATE][RISE], x.slew[LATE][FALL]);
                if (act > lim + 1e-12) transViol.push_back({(int)n, lim, act, lim - act});
            }
            if (!x.driver) continue;
            lim = -1;
            if (x.pin) tighter(x.pin->maxCapacitance);
            tighter(maxCapacitance);
            auto ic = maxCapAt.find((int)n);
            if (ic != maxCapAt.end()) tighter(ic->second);
            if (lim > 0 && x.load > lim + 1e-15) capViol.push_back({(int)n, lim, x.load, lim - x.load});
        }
        auto bySlack = [](const Violation& a, const Violation& b) { return a.slack != b.slack ? a.slack < b.slack : a.node < b.node; };
        std::sort(transViol.begin(), transViol.end(), bySlack);
        std::sort(capViol.begin(), capViol.end(), bySlack);
    }

    // Everything: call after build() and constrain().
    std::vector<std::pair<std::string, double>> phases;  // seconds, for -stats
    template <class F>
    void phase(const char* what, F&& f) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        phases.push_back({what, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()});
    }
    void update() {
        if (timed) return;
        phase("levelize", [&] { levelize(); });
        phase("delays", [&] { computeDelays(); });
        phase("arrivals", [&] { propagate(); });
        phase("checks", [&] { runChecks(); });
        phase("design rules", [&] { designRules(); });
        timed = true;
    }

    // ------------------------------------------------------------- queries

    struct Step {
        int node, rf, tag;
        double arr;   // relative to the launch clock edge
        int edge;     // the edge it came over, -1 at the start
    };

    // The worst path behind an arrival, start first.
    std::vector<Step> trace(int node, int tagIdx, int m, int rf) const {
        std::vector<Step> path;
        while (node >= 0 && tagIdx >= 0) {
            const Tag& t = tags[node][tagIdx];
            int ei = t.prevEdge[m][rf];
            path.push_back({node, rf, tagIdx, t.arr[m][rf], ei});
            if (ei < 0) break;
            int pt = t.prevTag[m][rf], prf = t.prevRf[m][rf];
            node = edges[ei].from;
            tagIdx = pt;
            rf = prf;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    struct Summary {
        double wns = 0, tns = 0;
        int violations = 0, total = 0;
    };
    Summary summary(bool late, int capClk = -1) const {
        Summary s;
        s.wns = INF;
        // One slack per endpoint: its worst.
        std::unordered_map<int, double> worst;
        for (auto& pe : ends) {
            if (isLate(pe.kind) != late) continue;
            if (capClk >= 0 && pe.capClk != capClk) continue;
            auto it = worst.find(pe.node);
            if (it == worst.end() || pe.slack < it->second) worst[pe.node] = pe.slack;
        }
        for (auto& [n, sl] : worst) {
            s.total++;
            if (sl < s.wns) s.wns = sl;
            if (sl < 0) {
                s.violations++;
                s.tns += sl;
            }
        }
        if (!s.total) s.wns = 0;
        return s;
    }
};

// The design as the SDC reader sees it.
struct SdcDesign : sdc::Design {
    const Sta& s;
    std::vector<std::string> empty;
    explicit SdcDesign(const Sta& sta) : s(sta) {}
    const std::vector<std::string>& names(sdc::Kind k) const override {
        switch (k) {
            case sdc::Kind::Port: return s.portNames;
            case sdc::Kind::Pin: return s.pinNames();
            case sdc::Kind::Cell: return s.cellNames();
            case sdc::Kind::Net: return s.nl->nets;
            default: return empty;
        }
    }
    int portDir(const std::string& n) const override {
        auto it = s.portByName.find(n);
        if (it == s.portByName.end()) return -1;
        gnl::Dir d = s.nl->ports[it->second].dir;
        return d == gnl::Dir::In ? 0 : d == gnl::Dir::Out ? 1 : 2;
    }
    sdc::ObjList registers(const sdc::RegQuery& q) const override {
        sdc::ObjList out;
        for (size_t i = 0; i < s.instInfo.size(); i++) {
            const CellInfo* ci = s.instInfo[i];
            if (!ci || !ci->cell->sequential()) continue;
            bool latch = ci->latch;
            if (latch ? !q.levelSensitive : !q.edgeTriggered) continue;
            if (q.cells) out.push_back({sdc::Kind::Cell, s.nl->insts[i].name, true});
            std::set<int> clk, data, outp;
            for (auto& ck : ci->checks) {
                clk.insert(ck.clk);
                data.insert(ck.data);
            }
            for (auto& g : ci->groups)
                if (g.launch) {
                    clk.insert(g.from);
                    outp.insert(g.to);
                }
            auto add = [&](const std::set<int>& pins) {
                for (int p : pins) {
                    int n = s.pinNode((int)i, p);
                    if (n >= 0) out.push_back({sdc::Kind::Pin, s.name(n), true});
                }
            };
            if (q.clockPins) add(clk);
            if (q.dataPins) add(data);
            if (q.outputPins) add(outp);
        }
        return out;
    }
    sdc::ObjList related(sdc::Kind want, const sdc::Obj& of) const override {
        sdc::ObjList out;
        if (of.kind == sdc::Kind::Cell && want == sdc::Kind::Pin) {
            int i = s.findInst(of.name);
            if (i >= 0)
                for (int n = s.instNodeOff[i]; n < s.instNodeOff[i + 1]; n++)
                    out.push_back({sdc::Kind::Pin, s.name(n), true});
        } else if (of.kind == sdc::Kind::Net && (want == sdc::Kind::Pin || want == sdc::Kind::Port)) {
            int net = s.findNet(of.name);
            if (net >= 0)
                for (size_t n = 0; n < s.nodes.size(); n++)
                    if (s.nodes[n].net == net && s.isPort((int)n) == (want == sdc::Kind::Port))
                        out.push_back({want, s.name((int)n), true});
        } else if ((of.kind == sdc::Kind::Pin || of.kind == sdc::Kind::Port) && want == sdc::Kind::Net) {
            int n = s.findNode(of.name);
            if (n >= 0 && s.nodes[n].net >= 0) out.push_back({sdc::Kind::Net, s.nl->nets[s.nodes[n].net], true});
        } else if (of.kind == sdc::Kind::Pin && want == sdc::Kind::Cell) {
            int n = s.findNode(of.name);
            if (n >= 0 && s.nodes[n].inst >= 0) out.push_back({sdc::Kind::Cell, s.nl->insts[s.nodes[n].inst].name, true});
        }
        return out;
    }
    std::optional<std::string> attribute(const sdc::Obj& o, const std::string& attr) const override {
        if (o.kind == sdc::Kind::Cell) {
            int i = s.findInst(o.name);
            if (i < 0) return std::nullopt;
            const CellInfo* ci = s.instInfo[i];
            if (attr == "ref_name") return s.nl->insts[i].cell;
            if (attr == "is_sequential") return ci && ci->cell->sequential() ? "true" : "false";
            if (attr == "is_combinational") return ci && !ci->cell->sequential() ? "true" : "false";
        } else if (o.kind == sdc::Kind::Pin || o.kind == sdc::Kind::Port) {
            int n = s.findNode(o.name);
            if (n < 0) return std::nullopt;
            const Node& x = s.nodes[n];
            if (attr == "direction") {
                if (x.inst < 0) {
                    gnl::Dir d = s.nl->ports[n].dir;
                    return d == gnl::Dir::In ? "in" : d == gnl::Dir::Out ? "out" : "inout";
                }
                if (!x.pin) return std::nullopt;
                return x.pin->dir == liberty::Dir::In ? "in" : x.pin->dir == liberty::Dir::Out ? "out" : "inout";
            }
            if (attr == "is_clock_pin" && x.pin) return x.pin->clock ? "true" : "false";
        }
        return std::nullopt;
    }
};

}  // namespace sta
