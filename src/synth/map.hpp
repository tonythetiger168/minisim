#pragma once
// map.hpp - technology mapping: the and-inverter graph onto the cells of a
// Liberty library, and the gate netlist that comes out.
//
// Combinational logic: for every AND node, the 4-input cuts are listed with
// their truth tables (merged from the fanins' cuts). A cut matches a cell
// when the cell's function equals the cut's function for some assignment of
// pins to leaves, with any leaf or the output inverted (inverters are then
// charged to the leaf or the output). A table indexed by the 16-bit truth
// table holds the best cell for every function, so matching is one lookup.
// Each node keeps its best implementation in both polarities, by area flow
// (area mode) or by arrival time (delay mode), and the cover is read back
// from the outputs.
//
// Flops: a library flop is picked by its ff group (clock edge, clear,
// preset). A missing variant is built from another one: a falling-edge
// flop from a rising-edge one and an inverter on the clock, a preset flop
// from a clear flop with D and Q inverted.
//
// Levels of the graph are mapped in parallel; the result does not depend on
// the number of threads.
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>

#include "../liberty.hpp"
#include "../aig.hpp"

namespace synth {

using namespace aig;

inline int& threadCount() {
    static int n = 0;
    return n;
}
inline int threads() {
    int n = threadCount();
    if (n <= 0) n = (int)std::thread::hardware_concurrency();
    return std::max(1, n);
}
// Calls fn(i) for i in [0, n) on all threads. fn must only write state of its own i.
template <class F>
void parallelFor(size_t n, size_t minPerThread, F fn) {
    size_t nt = std::min<size_t>(threads(), n / std::max<size_t>(1, minPerThread));
    if (nt <= 1) {
        for (size_t i = 0; i < n; i++) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    const size_t chunk = std::max<size_t>(16, n / (nt * 8));
    std::vector<std::thread> pool;
    auto work = [&] {
        for (;;) {
            size_t b = next.fetch_add(chunk);
            if (b >= n) return;
            size_t e = std::min(n, b + chunk);
            for (size_t i = b; i < e; i++) fn(i);
        }
    };
    for (size_t t = 1; t < nt; t++) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}

struct MapError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------ the library

struct Gate {
    const liberty::Cell* cell = nullptr;
    int out = -1;                 // index of the output pin in cell->pins
    std::vector<int> in;          // indexes of the input pins
    uint64_t tt = 0;              // function over the inputs
    double area = 0, delay = 0;   // delay: a nominal figure for mapping
};

struct FlopCell {
    const liberty::Cell* cell = nullptr;
    int d = -1, ck = -1, q = -1, clr = -1, pre = -1;
    bool negClk = false, clrLow = false, preLow = false;
    double area = 0;
};

struct LibInfo {
    std::vector<std::shared_ptr<liberty::Library>> libs;
    std::vector<Gate> gates;
    std::vector<FlopCell> flops;
    int inv = -1, buf = -1, tie0 = -1, tie1 = -1;
    std::set<std::string> dontUse;
    struct M {
        int32_t gate = -1;
        uint8_t perm[4] = {0, 0, 0, 0};
    };
    std::vector<M> byArea, byDelay;   // 65536 entries each
    double nomSlew = 0.05, nomLoad = 0.01;

    const liberty::Cell* cell(const std::string& n) const {
        for (auto& l : libs)
            if (auto* c = l->cell(n)) return c;
        return nullptr;
    }

    static bool simpleVar(const std::string& fn, std::string& var, bool& neg) {
        if (fn.empty()) return false;
        liberty::Expr e = liberty::parseFunction(fn);
        neg = false;
        const liberty::Expr* p = &e;
        while (p->kind == liberty::Expr::Not) {
            neg = !neg;
            p = &p->kids[0];
        }
        if (p->kind != liberty::Expr::Var) return false;
        var = p->var;
        return true;
    }
    static int pinIndex(const liberty::Cell& c, const std::string& n) {
        for (size_t i = 0; i < c.pins.size(); i++)
            if (c.pins[i].name == n) return (int)i;
        return -1;
    }

    void build() {
        gates.clear();
        flops.clear();
        inv = buf = tie0 = tie1 = -1;
        // nominal load: a few inverter inputs
        for (auto& l : libs)
            for (auto& c : l->cells) {
                if (c.dontUse || c.dontTouch || dontUse.count(c.name)) continue;
                if (!c.latches.empty()) continue;
                if (!c.ffs.empty()) {
                    if (c.ffs.size() != 1) continue;
                    const auto& ff = c.ffs[0];
                    FlopCell f;
                    f.cell = &c;
                    f.area = c.area;
                    std::string v;
                    bool neg;
                    try {
                        if (!simpleVar(ff.nextState, v, neg) || neg) continue;
                        f.d = pinIndex(c, v);
                        if (!simpleVar(ff.clockedOn, v, neg)) continue;
                        f.ck = pinIndex(c, v);
                        f.negClk = neg;
                        if (!ff.clear.empty()) {
                            if (!simpleVar(ff.clear, v, neg)) continue;
                            f.clr = pinIndex(c, v);
                            f.clrLow = neg;
                            if (f.clr < 0) continue;
                        }
                        if (!ff.preset.empty()) {
                            if (!simpleVar(ff.preset, v, neg)) continue;
                            f.pre = pinIndex(c, v);
                            f.preLow = neg;
                            if (f.pre < 0) continue;
                        }
                    } catch (liberty::Error&) {
                        continue;
                    }
                    for (size_t i = 0; i < c.pins.size(); i++)
                        if (c.pins[i].dir == liberty::Dir::Out && c.pins[i].function == ff.iq) f.q = (int)i;
                    if (f.d < 0 || f.ck < 0 || f.q < 0) continue;
                    // Pins beyond D, CK, clear, preset and the outputs (scan, enable): not a plain flop.
                    size_t ins = 0;
                    for (auto& p : c.pins)
                        if (p.dir == liberty::Dir::In) ins++;
                    if (ins != 2u + (f.clr >= 0) + (f.pre >= 0)) continue;
                    flops.push_back(f);
                    continue;
                }
                Gate gt;
                gt.cell = &c;
                gt.area = c.area;
                int outs = 0;
                bool ok = true;
                for (size_t i = 0; i < c.pins.size(); i++) {
                    const auto& p = c.pins[i];
                    if (p.dir == liberty::Dir::In) gt.in.push_back((int)i);
                    else if (p.dir == liberty::Dir::Out) {
                        outs++;
                        gt.out = (int)i;
                        if (!p.hasFunction() || !p.threeState.empty()) ok = false;
                    } else
                        ok = false;
                }
                if (!ok || outs != 1 || gt.in.size() > 4) continue;
                std::vector<std::string> names;
                for (int i : gt.in) names.push_back(c.pins[i].name);
                try {
                    liberty::Expr e = c.pins[gt.out].functionExpr();
                    bool known = true;
                    for (auto& v : e.vars())
                        if (std::find(names.begin(), names.end(), v) == names.end()) known = false;
                    if (!known) continue;
                    gt.tt = e.truthTable(names);
                } catch (liberty::Error&) {
                    continue;
                }
                gates.push_back(gt);
            }
        // nominal figures
        double invCap = 0;
        for (size_t k = 0; k < gates.size(); k++) {
            Gate& gt = gates[k];
            if (gt.in.size() == 1 && (gt.tt & 3) == 1) {
                if (inv < 0 || gt.area < gates[inv].area) inv = (int)k;
            }
            if (gt.in.size() == 1 && (gt.tt & 3) == 2) {
                if (buf < 0 || gt.area < gates[buf].area) buf = (int)k;
            }
            if (gt.in.empty()) {
                int& t = (gt.tt & 1) ? tie1 : tie0;
                if (t < 0 || gt.area < gates[t].area) t = (int)k;
            }
        }
        if (inv >= 0) invCap = gates[inv].cell->pins[gates[inv].in[0]].cap;
        nomLoad = invCap > 0 ? 3 * invCap : 0.005;
        for (Gate& gt : gates) {
            double worst = 0;
            for (auto& a : gt.cell->arcs) {
                if (a.type != liberty::ArcType::Comb) continue;
                worst = std::max(worst, 0.5 * (a.cellRise.lookup(nomSlew, nomLoad) + a.cellFall.lookup(nomSlew, nomLoad)));
            }
            gt.delay = worst > 0 ? worst : 0.01 * (1 + gt.in.size());
        }
        // the match tables
        byArea.assign(65536, M{});
        byDelay.assign(65536, M{});
        for (size_t k = 0; k < gates.size(); k++) {
            const Gate& gt = gates[k];
            size_t n = gt.in.size();
            if (n == 0) continue;
            std::array<int, 4> pi{0, 1, 2, 3};
            std::set<std::array<int, 4>> seen;
            do {
                std::array<int, 4> key{-1, -1, -1, -1};
                for (size_t j = 0; j < n; j++) key[j] = pi[j];
                if (!seen.insert(key).second) continue;
                uint16_t tt = 0;
                for (int m = 0; m < 16; m++) {
                    unsigned idx = 0;
                    for (size_t j = 0; j < n; j++) idx |= ((m >> pi[j]) & 1u) << j;
                    if ((gt.tt >> idx) & 1) tt |= (uint16_t)(1u << m);
                }
                M cand;
                cand.gate = (int32_t)k;
                for (size_t j = 0; j < 4; j++) cand.perm[j] = j < n ? (uint8_t)pi[j] : 0;
                auto better = [&](const M& cur, bool delay) {
                    if (cur.gate < 0) return true;
                    const Gate& o = gates[cur.gate];
                    if (delay) return std::tie(gt.delay, gt.area) < std::tie(o.delay, o.area);
                    return std::tie(gt.area, gt.delay) < std::tie(o.area, o.delay);
                };
                if (better(byArea[tt], false)) byArea[tt] = cand;
                if (better(byDelay[tt], true)) byDelay[tt] = cand;
            } while (std::next_permutation(pi.begin(), pi.end()));
        }
    }
};

// ------------------------------------------------------------ the netlist

struct Inst {
    const liberty::Cell* cell = nullptr;
    std::string name;
    std::vector<int> pins;   // the net of each cell pin, -1 when unconnected
    bool seq = false;
};

struct Netlist {
    std::string top;
    std::vector<std::string> nets;   // names; net 0 is 1'b0, net 1 is 1'b1
    std::vector<Inst> insts;         // flops first, then gates in topological order
    struct PortBits {
        std::string name;
        bool out = false;
        int msb = 0, lsb = 0;
        std::vector<int> nets;   // LSB first
    };
    std::vector<PortBits> ports;
    std::vector<std::pair<std::string, int>> assigns;   // output bit name = net

    int newNet(const std::string& name = "") {
        nets.push_back(name);
        return (int)nets.size() - 1;
    }
    double area() const {
        double a = 0;
        for (auto& i : insts) a += i.cell->area;
        return a;
    }
};

inline std::string bitName(const std::string& port, int msb, int lsb, size_t i, size_t width) {
    if (width == 1 && msb == lsb && msb == 0) return port;
    int idx = msb >= lsb ? lsb + (int)i : lsb - (int)i;
    return port + "[" + std::to_string(idx) + "]";
}

// ------------------------------------------------------------ the mapper

class Mapper {
    const Design& d;
    const Graph& g;
    const LibInfo& lib;
    bool delayMode;

    static constexpr int kCuts = 8;
    struct Cut {
        uint32_t leaf[4];
        uint8_t n = 0;
        uint16_t tt = 0;
        float key = 0, key2 = 0;
    };
    struct Choice {
        float af = std::numeric_limits<float>::infinity();
        float arr = std::numeric_limits<float>::infinity();
        int32_t gate = -1;
        bool viaInv = false;
        uint8_t nIn = 0;
        uint32_t leaf[4] = {0, 0, 0, 0};
        uint8_t phase[4] = {0, 0, 0, 0};
    };
    std::vector<std::vector<Cut>> cuts;
    std::vector<std::array<Choice, 2>> best;
    std::vector<float> fanout;

    bool better(float af, float arr, const Choice& c) const {
        if (delayMode) return arr < c.arr - 1e-6f || (arr <= c.arr + 1e-6f && af < c.af - 1e-6f);
        return af < c.af - 1e-6f || (af <= c.af + 1e-6f && arr < c.arr - 1e-6f);
    }

    static uint16_t flipVar(uint16_t t, int i) {
        static const uint16_t m[4] = {0xAAAA, 0xCCCC, 0xF0F0, 0xFF00};
        int sh = 1 << i;
        return (uint16_t)(((t & m[i]) >> sh) | ((t & (uint16_t)~m[i]) << sh));
    }
    // The truth table of a cut over the leaves of a larger cut.
    static uint16_t expand(const Cut& c, const Cut& u) {
        int at[4];
        for (int i = 0; i < c.n; i++) {
            at[i] = 0;
            for (int j = 0; j < u.n; j++)
                if (u.leaf[j] == c.leaf[i]) at[i] = j;
        }
        uint16_t t = 0;
        for (int m = 0; m < 16; m++) {
            unsigned idx = 0;
            for (int i = 0; i < c.n; i++) idx |= ((m >> at[i]) & 1u) << i;
            if ((c.tt >> idx) & 1) t |= (uint16_t)(1u << m);
        }
        return t;
    }

    void evalCut(uint32_t node, Cut& c) {
        const auto& tab = delayMode ? lib.byDelay : lib.byArea;
        c.key = c.key2 = std::numeric_limits<float>::infinity();
        // which variables the function depends on
        unsigned support = 0;
        for (int i = 0; i < c.n; i++)
            if (flipVar(c.tt, i) != c.tt) support |= 1u << i;
        for (unsigned mask = 0; mask < (1u << c.n); mask++) {
            if (mask & ~support) continue;
            uint16_t t = c.tt;
            for (int i = 0; i < c.n; i++)
                if (mask & (1u << i)) t = flipVar(t, i);
            for (int p = 0; p < 2; p++) {
                uint16_t want = p ? (uint16_t)~t : t;
                const LibInfo::M& m = tab[want];
                if (m.gate < 0) continue;
                const Gate& gt = lib.gates[m.gate];
                float af = (float)gt.area, arr = 0;
                Choice ch;
                bool ok = true;
                for (size_t k = 0; k < gt.in.size(); k++) {
                    int posn = m.perm[k];
                    if (posn >= c.n) { ok = false; break; }
                    uint32_t lf = c.leaf[posn];
                    int ph = (mask >> posn) & 1;
                    const Choice& lc = best[lf][ph];
                    af += lc.af / fanout[lf];
                    arr = std::max(arr, lc.arr);
                    ch.leaf[k] = lf;
                    ch.phase[k] = (uint8_t)ph;
                }
                if (!ok) continue;
                arr += (float)gt.delay;
                Choice& b = best[node][p];
                if (better(af, arr, b)) {
                    ch.af = af;
                    ch.arr = arr;
                    ch.gate = m.gate;
                    ch.nIn = (uint8_t)gt.in.size();
                    b = ch;
                }
                float k1 = delayMode ? arr : af, k2 = delayMode ? af : arr;
                if (k1 < c.key || (k1 == c.key && k2 < c.key2)) { c.key = k1; c.key2 = k2; }
            }
        }
    }

    void mapNode(uint32_t n) {
        const Node& nd = g.nodes[n];
        const auto& ca = cuts[lnode(nd.a)];
        const auto& cb = cuts[lnode(nd.b)];
        std::vector<Cut> out;
        out.reserve(ca.size() * cb.size());
        for (const Cut& x : ca)
            for (const Cut& y : cb) {
                Cut u;
                // merge the sorted leaf lists
                int i = 0, j = 0;
                bool over = false;
                while (i < x.n || j < y.n) {
                    uint32_t v;
                    if (j >= y.n || (i < x.n && x.leaf[i] < y.leaf[j])) v = x.leaf[i++];
                    else if (i >= x.n || y.leaf[j] < x.leaf[i]) v = y.leaf[j++];
                    else { v = x.leaf[i]; i++; j++; }
                    if (u.n == 4) { over = true; break; }
                    u.leaf[u.n++] = v;
                }
                if (over) continue;
                bool dup = false;
                for (const Cut& o : out)
                    if (o.n == u.n && std::equal(o.leaf, o.leaf + o.n, u.leaf)) { dup = true; break; }
                if (dup) continue;
                uint16_t ta = expand(x, u), tb = expand(y, u);
                if (lisneg(nd.a)) ta = (uint16_t)~ta;
                if (lisneg(nd.b)) tb = (uint16_t)~tb;
                u.tt = ta & tb;
                evalCut(n, u);
                out.push_back(u);
            }
        std::stable_sort(out.begin(), out.end(), [](const Cut& a, const Cut& b) {
            return std::tie(a.key, a.key2, a.n) < std::tie(b.key, b.key2, b.n);
        });
        if (out.size() > kCuts) out.resize(kCuts);
        relax(n);
        Cut self;
        self.n = 1;
        self.leaf[0] = n;
        self.tt = 0xAAAA;
        out.push_back(self);
        cuts[n] = std::move(out);
    }

    void relax(uint32_t n) {
        const Gate& iv = lib.gates[lib.inv];
        for (int p = 0; p < 2; p++) {
            Choice& a = best[n][p];
            const Choice& o = best[n][!p];
            if (o.viaInv || !std::isfinite(o.af)) continue;
            float af = o.af + (float)iv.area, arr = o.arr + (float)iv.delay;
            if (better(af, arr, a)) {
                a = Choice{};
                a.af = af;
                a.arr = arr;
                a.viaInv = true;
            }
        }
    }

public:
    Mapper(const Design& des, const LibInfo& l, bool delay) : d(des), g(des.g), lib(l), delayMode(delay) {}

    Netlist run() {
        if (lib.inv < 0) throw MapError("the target library has no inverter");
        size_t N = g.size();
        cuts.assign(N, {});
        best.assign(N, {});
        fanout.assign(N, 0);
        std::vector<int> level(N, 0);
        int maxLevel = 0;
        for (uint32_t n = 1; n < N; n++) {
            const Node& nd = g.nodes[n];
            if (nd.k == NK::And) {
                fanout[lnode(nd.a)] += 1;
                fanout[lnode(nd.b)] += 1;
                level[n] = 1 + std::max(level[lnode(nd.a)], level[lnode(nd.b)]);
                maxLevel = std::max(maxLevel, level[n]);
            }
        }
        for (auto& p : d.ports)
            if (p.out)
                for (Lit b : p.bits) fanout[lnode(b)] += 1;
        for (auto& f : g.flops) {
            fanout[lnode(f.d)] += 1;
            fanout[lnode(f.clk)] += 1;
            if (f.arst != NOLIT) fanout[lnode(f.arst)] += 1;
        }
        for (auto& f : fanout) f = std::max(f, 1.0f);
        // leaves
        const Gate& iv = lib.gates[lib.inv];
        for (uint32_t n = 0; n < N; n++) {
            const Node& nd = g.nodes[n];
            if (nd.k == NK::And) continue;
            best[n][0].af = best[n][0].arr = 0;
            best[n][1].af = (float)iv.area;
            best[n][1].arr = (float)iv.delay;
            best[n][1].viaInv = true;
            if (nd.k == NK::Const) best[n][1] = best[n][0];
            Cut self;
            self.n = 1;
            self.leaf[0] = n;
            self.tt = 0xAAAA;
            cuts[n].push_back(self);
        }
        // level by level, the nodes of a level in parallel
        std::vector<std::vector<uint32_t>> byLevel(maxLevel + 1);
        for (uint32_t n = 1; n < N; n++)
            if (g.nodes[n].k == NK::And) byLevel[level[n]].push_back(n);
        for (auto& lv : byLevel) parallelFor(lv.size(), 64, [&](size_t i) { mapNode(lv[i]); });
        for (uint32_t n = 1; n < N; n++)
            if (g.nodes[n].k == NK::And && !std::isfinite(best[n][0].af) && !std::isfinite(best[n][1].af))
                throw MapError("the target library cannot implement the design's logic (it needs at least an inverter and a 2-input NAND, NOR, AND or OR)");
        return cover();
    }

private:
    const FlopCell* pickFlop(bool neg, bool clr, bool pre) const {
        const FlopCell* bestF = nullptr;
        for (auto& f : lib.flops) {
            if (f.negClk != neg || (f.clr >= 0) != clr || (f.pre >= 0) != pre) continue;
            if (!bestF || f.area < bestF->area) bestF = &f;
        }
        return bestF;
    }

    Netlist cover() {
        size_t N = g.size();
        Netlist nl;
        nl.top = d.top;
        nl.newNet("1'b0");
        nl.newNet("1'b1");
        std::vector<std::array<int, 2>> net(N, {-1, -1});
        net[0] = {0, 1};
        std::vector<std::array<uint8_t, 2>> req(N, {0, 0});
        auto need = [&](Lit l) { if (l != NOLIT) req[lnode(l)][lisneg(l)] = 1; };

        // ports
        for (auto& p : d.ports) {
            Netlist::PortBits pb;
            pb.name = p.name;
            pb.out = p.out;
            pb.msb = p.msb;
            pb.lsb = p.lsb;
            if (!p.out)
                for (size_t i = 0; i < p.bits.size(); i++) {
                    int id = nl.newNet(bitName(p.name, p.msb, p.lsb, i, p.bits.size()));
                    net[lnode(p.bits[i])][0] = id;
                    pb.nets.push_back(id);
                }
            else
                for (Lit b : p.bits) need(b);
            nl.ports.push_back(std::move(pb));
        }
        // flops: the instance and its Q net now, its inputs after the logic exists
        struct FlopPlan {
            const FlopCell* fc;
            bool invD = false, invClk = false;
            int inst;
        };
        std::vector<FlopPlan> plans;
        int tmp = 0;
        for (auto& f : g.flops) {
            bool hasR = f.arst != NOLIT;
            bool wantClr = hasR && !f.rval, wantPre = hasR && f.rval;
            FlopPlan pl{};
            for (int tryInvClk = 0; tryInvClk < 2 && !pl.fc; tryInvClk++) {
                bool neg = f.negedge ^ (tryInvClk == 1);
                pl.invClk = tryInvClk == 1;
                pl.invD = false;
                pl.fc = pickFlop(neg, wantClr, wantPre);
                if (!pl.fc && hasR) {   // the other async kind, with D and Q inverted
                    pl.fc = pickFlop(neg, wantPre, wantClr);
                    pl.invD = pl.fc != nullptr;
                }
                if (!pl.fc && !hasR) {   // a flop with an async control tied off
                    for (auto& c : lib.flops)
                        if (c.negClk == neg && (!pl.fc || c.area < pl.fc->area)) pl.fc = &c;
                }
            }
            if (!pl.fc)
                throw MapError(std::string("the target library has no flip-flop for '") + f.name + "'" +
                               (hasR ? " (asynchronous set or reset)" : ""));
            Inst in;
            in.cell = pl.fc->cell;
            in.name = f.name;
            in.seq = true;
            in.pins.assign(in.cell->pins.size(), -1);
            int q = nl.newNet("");
            in.pins[pl.fc->q] = q;
            net[lnode(f.q)][pl.invD ? 1 : 0] = q;
            pl.inst = (int)nl.insts.size();
            nl.insts.push_back(std::move(in));
            plans.push_back(pl);
            need(lxorc(f.d, pl.invD));
            need(lxorc(f.clk, pl.invClk));
            if (hasR) {
                bool low = pl.invD ? (wantClr ? pl.fc->preLow : pl.fc->clrLow) : (wantClr ? pl.fc->clrLow : pl.fc->preLow);
                need(lxorc(f.arst, low));
            }
            (void)tmp;
        }
        // what the cover needs, from the outputs back
        for (uint32_t n = (uint32_t)N; n-- > 1;) {
            if (g.nodes[n].k != NK::And) continue;
            for (int p = 0; p < 2; p++)
                if (req[n][p] && best[n][p].viaInv) req[n][!p] = 1;
            for (int p = 0; p < 2; p++) {
                if (!req[n][p] || best[n][p].viaInv) continue;
                const Choice& c = best[n][p];
                for (int k = 0; k < c.nIn; k++) req[c.leaf[k]][c.phase[k]] = 1;
            }
        }
        int uid = 0;
        auto gateInst = [&](int gi, const std::vector<int>& ins) {
            const Gate& gt = lib.gates[gi];
            Inst in;
            in.cell = gt.cell;
            in.name = "U" + std::to_string(++uid);
            in.pins.assign(gt.cell->pins.size(), -1);
            for (size_t k = 0; k < gt.in.size(); k++) in.pins[gt.in[k]] = ins[k];
            int o = nl.newNet("");
            in.pins[gt.out] = o;
            nl.insts.push_back(std::move(in));
            return o;
        };
        for (uint32_t n = 1; n < N; n++) {
            const Node& nd = g.nodes[n];
            if (nd.k != NK::And) {   // an input or a flop output: the other polarity through an inverter
                for (int p = 0; p < 2; p++)
                    if (req[n][p] && net[n][p] < 0) {
                        if (net[n][!p] < 0) throw MapError("internal: leaf without a net");
                        net[n][p] = gateInst(lib.inv, {net[n][!p]});
                    }
                continue;
            }
            for (int pass = 0; pass < 2; pass++)
                for (int p = 0; p < 2; p++) {
                    if (!req[n][p] || net[n][p] >= 0) continue;
                    const Choice& c = best[n][p];
                    if (c.viaInv != (pass == 1)) continue;
                    if (c.viaInv) {
                        net[n][p] = gateInst(lib.inv, {net[n][!p]});
                        continue;
                    }
                    std::vector<int> ins(c.nIn);
                    for (int k = 0; k < c.nIn; k++) ins[k] = net[c.leaf[k]][c.phase[k]];
                    net[n][p] = gateInst(c.gate, ins);
                }
        }
        auto netOf = [&](Lit l) { return net[lnode(l)][lisneg(l)]; };
        // flop inputs
        for (size_t i = 0; i < g.flops.size(); i++) {
            const Flop& f = g.flops[i];
            const FlopPlan& pl = plans[i];
            Inst& in = nl.insts[pl.inst];
            in.pins[pl.fc->d] = netOf(lxorc(f.d, pl.invD));
            in.pins[pl.fc->ck] = netOf(lxorc(f.clk, pl.invClk));
            bool hasR = f.arst != NOLIT;
            bool wantClr = hasR && !f.rval;
            bool useClr = hasR && (pl.invD ? !wantClr : wantClr);
            if (hasR) {
                int pin = useClr ? pl.fc->clr : pl.fc->pre;
                bool low = useClr ? pl.fc->clrLow : pl.fc->preLow;
                in.pins[pin] = netOf(lxorc(f.arst, low));
            }
            // an async control the design does not use is tied inactive
            if (pl.fc->clr >= 0 && in.pins[pl.fc->clr] < 0) in.pins[pl.fc->clr] = pl.fc->clrLow ? 1 : 0;
            if (pl.fc->pre >= 0 && in.pins[pl.fc->pre] < 0) in.pins[pl.fc->pre] = pl.fc->preLow ? 1 : 0;
        }
        // outputs: the driving net takes the port bit's name when it can
        std::vector<uint8_t> named(nl.nets.size(), 0);
        for (auto& pb : nl.ports)
            if (!pb.out)
                for (int id : pb.nets) named[id] = 1;
        named[0] = named[1] = 1;
        for (size_t pi = 0; pi < d.ports.size(); pi++) {
            const Port& p = d.ports[pi];
            if (!p.out) continue;
            auto& pb = nl.ports[pi];
            for (size_t i = 0; i < p.bits.size(); i++) {
                int id = netOf(p.bits[i]);
                std::string bn = bitName(p.name, p.msb, p.lsb, i, p.bits.size());
                if (named[id]) {
                    nl.assigns.push_back({bn, id});
                    int alias = nl.newNet(bn);
                    named.push_back(1);
                    pb.nets.push_back(alias);
                } else {
                    nl.nets[id] = bn;
                    named[id] = 1;
                    pb.nets.push_back(id);
                }
            }
        }
        for (size_t i = 2; i < nl.nets.size(); i++)
            if (nl.nets[i].empty()) nl.nets[i] = "n" + std::to_string(i);
        return nl;
    }
};

}  // namespace synth
