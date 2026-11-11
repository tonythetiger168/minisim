// emu/part.hpp - splitting the design over the FPGAs of a board.
//
// The netlist becomes a hypergraph: one vertex per cell and per port bit, one
// hyperedge per net, pin 0 the driver. The cost of a net is the number of
// board hops from its driver's FPGA to each other FPGA that reads it, so on a
// board where every pair is wired it is the usual "FPGAs spanned minus one".
// The partitioner is multilevel: cluster strongly connected vertices, split
// the small graph, then undo the clustering level by level and let single
// vertices move to where they cost less. It minimizes the cut under each
// FPGA's capacity and does not try to fill the FPGAs evenly: a design that
// fits one FPGA stays in one.
//
// Several starts with different seeds run at once, one per thread; the best
// (then the lowest seed) wins, so the answer does not depend on the thread
// count.
#pragma once
#include <mutex>
#include <thread>
#include <array>
#include <chrono>

#include "design.hpp"

namespace emu {

using Wt = std::array<double, 3>;  // LUTs, flops, block RAM bits

struct HGraph {
    int nv = 0;
    std::vector<Wt> w;
    std::vector<int> fixed;             // the FPGA a vertex must be in, or -1
    std::vector<int> netOff, netPins;   // netPins[netOff[e]] is the driver
    std::vector<int> netW;
    std::vector<int> vOff, vNets;       // the nets of a vertex

    int ne() const { return (int)netW.size(); }
    void addNet(const std::vector<int>& pins, int weight) {
        if (netOff.empty()) netOff.push_back(0);
        netPins.insert(netPins.end(), pins.begin(), pins.end());
        netOff.push_back((int)netPins.size());
        netW.push_back(weight);
    }
    void index() {
        if (netOff.empty()) netOff.push_back(0);
        vOff.assign(nv + 1, 0);
        for (int p : netPins) vOff[p + 1]++;
        for (int v = 0; v < nv; v++) vOff[v + 1] += vOff[v];
        vNets.resize(netPins.size());
        std::vector<int> fill(vOff.begin(), vOff.end() - 1);
        for (int e = 0; e < ne(); e++)
            for (int x = netOff[e]; x < netOff[e + 1]; x++) vNets[fill[netPins[x]]++] = e;
    }
};

struct PartStats {
    double cost = 0;
    double over = 0;  // how far past capacity, summed; 0 when everything fits
    int seed = 0;
};

class Partitioner {
public:
    Partitioner(const Board& b) : B(b), k(b.k()) {
        for (int p = 0; p < k; p++) {
            auto& f = b.fpgas[p];
            auto lim = [&](double v) { return v > 0 ? v * b.util : 1e300; };
            cap.push_back({lim(f.lut), lim(f.ff), lim(f.bramKb * 1024)});
        }
    }

    std::vector<int> run(const HGraph& g, int seed, PartStats& st) const {
        Rng rng((uint64_t)seed);
        std::vector<HGraph> levels;
        std::vector<std::vector<int>> maps;
        const HGraph* cur = &g;
        int limit = std::max(200, 60 * k);
        while (cur->nv > limit) {
            std::vector<int> cl;
            int nc = cluster(*cur, rng, cl);
            if (nc > cur->nv * 0.9) break;
            levels.push_back(contract(*cur, cl, nc));
            maps.push_back(std::move(cl));
            cur = &levels.back();
        }
        // The smallest graph is cheap to split: try several ways and keep the best.
        std::vector<int> part;
        double bestOver = 0, bestCost = 0;
        for (int t = 0; t < 12; t++) {
            std::vector<int> p = initial(*cur, rng);
            refine(*cur, p, rng);
            double o = overflow(*cur, p), c = cost(*cur, p);
            if (part.empty() || o < bestOver - 1e-12 || (o <= bestOver + 1e-12 && c < bestCost)) {
                part = std::move(p);
                bestOver = o;
                bestCost = c;
            }
        }
        for (int l = (int)levels.size() - 1; l >= 0; l--) {
            const HGraph& fine = l ? levels[l - 1] : g;
            std::vector<int> p(fine.nv);
            for (int v = 0; v < fine.nv; v++) p[v] = part[maps[l][v]];
            part = std::move(p);
            refine(fine, part, rng);
        }
        st.seed = seed;
        st.cost = cost(g, part);
        st.over = overflow(g, part);
        return part;
    }

    // A partition from somewhere else (memu -partitioner): held to the fixed
    // vertices and the capacities, and improved, by the same passes.
    std::vector<int> polish(const HGraph& g, std::vector<int> part, PartStats& st) const {
        Rng rng(0x9e3779b9ull);
        for (int v = 0; v < g.nv; v++) {
            if (part[v] < 0 || part[v] >= k) part[v] = 0;
            if (g.fixed[v] >= 0) part[v] = g.fixed[v];
        }
        refine(g, part, rng);
        st.seed = 0;
        st.cost = cost(g, part);
        st.over = overflow(g, part);
        return part;
    }

    double cost(const HGraph& g, const std::vector<int>& part) const {
        double c = 0;
        std::vector<char> seen(k);
        for (int e = 0; e < g.ne(); e++) {
            std::fill(seen.begin(), seen.end(), 0);
            int d = part[g.netPins[g.netOff[e]]];
            for (int x = g.netOff[e] + 1; x < g.netOff[e + 1]; x++) {
                int p = part[g.netPins[x]];
                if (p != d && !seen[p]) {
                    seen[p] = 1;
                    c += (double)g.netW[e] * B.dist[d][p];
                }
            }
        }
        return c;
    }

    double overflow(const HGraph& g, const std::vector<int>& part) const {
        std::vector<Wt> load(k, Wt{0, 0, 0});
        for (int v = 0; v < g.nv; v++)
            for (int r = 0; r < 3; r++) load[part[v]][r] += g.w[v][r];
        double o = 0;
        for (int p = 0; p < k; p++)
            for (int r = 0; r < 3; r++)
                if (load[p][r] > cap[p][r] * (1 + 1e-9)) o += load[p][r] / cap[p][r] - 1;
        return o;
    }

private:
    const Board& B;
    int k;
    std::vector<Wt> cap;

    bool fits(const Wt& load, const Wt& w, int p) const {
        for (int r = 0; r < 3; r++)
            if (w[r] > 0 && load[r] + w[r] > cap[p][r] * (1 + 1e-9)) return false;
        return true;
    }

    // Each vertex joins the neighbouring cluster it shares the most (small)
    // nets with, while the cluster stays small enough to move around later.
    int cluster(const HGraph& g, Rng& rng, std::vector<int>& cl) const {
        int n = g.nv;
        cl.resize(n);
        std::vector<Wt> cw(g.w);
        std::vector<int> size(n, 1), order(n), touched;
        std::vector<double> score(n, 0);
        Wt maxW{1e300, 1e300, 1e300};
        for (int p = 0; p < k; p++)
            for (int r = 0; r < 3; r++) maxW[r] = std::min(maxW[r], cap[p][r] / 8);
        for (int v = 0; v < n; v++) cl[v] = order[v] = v;
        rng.shuffle(order);
        for (int v : order) {
            if (size[cl[v]] > 1) continue;
            touched.clear();
            for (int x = g.vOff[v]; x < g.vOff[v + 1]; x++) {
                int e = g.vNets[x];
                int sz = g.netOff[e + 1] - g.netOff[e];
                if (sz > 64) continue;
                double s = (double)g.netW[e] / (sz - 1);
                for (int y = g.netOff[e]; y < g.netOff[e + 1]; y++) {
                    int c = cl[g.netPins[y]];
                    if (c == cl[v]) continue;
                    if (score[c] == 0) touched.push_back(c);
                    score[c] += s;
                }
            }
            int best = -1;
            double bs = 0;
            for (int c : touched) {
                bool ok = g.fixed[v] < 0 || g.fixed[c] < 0 || g.fixed[v] == g.fixed[c];
                for (int r = 0; r < 3 && ok; r++)
                    if (g.w[v][r] > 0 && cw[c][r] + g.w[v][r] > maxW[r]) ok = false;
                if (ok && (score[c] > bs || (score[c] == bs && best >= 0 && c < best))) {
                    bs = score[c];
                    best = c;
                }
                score[c] = 0;
            }
            if (best < 0) continue;
            cl[v] = best;
            size[best]++;
            for (int r = 0; r < 3; r++) cw[best][r] += g.w[v][r];
        }
        // Number the clusters 0..nc-1 in vertex order.
        std::vector<int> id(n, -1);
        int nc = 0;
        for (int v = 0; v < n; v++)
            if (id[cl[v]] < 0) id[cl[v]] = nc++;
        for (int v = 0; v < n; v++) cl[v] = id[cl[v]];
        return nc;
    }

    HGraph contract(const HGraph& g, const std::vector<int>& cl, int nc) const {
        HGraph c;
        c.nv = nc;
        c.w.assign(nc, Wt{0, 0, 0});
        c.fixed.assign(nc, -1);
        for (int v = 0; v < g.nv; v++) {
            for (int r = 0; r < 3; r++) c.w[cl[v]][r] += g.w[v][r];
            if (g.fixed[v] >= 0) c.fixed[cl[v]] = g.fixed[v];
        }
        // Nets with the same driver and readers merge into one, heavier net.
        std::unordered_map<uint64_t, std::vector<int>> byHash;
        std::vector<int> pins;
        for (int e = 0; e < g.ne(); e++) {
            pins.clear();
            int d = cl[g.netPins[g.netOff[e]]];
            for (int x = g.netOff[e] + 1; x < g.netOff[e + 1]; x++) {
                int p = cl[g.netPins[x]];
                if (p != d) pins.push_back(p);
            }
            if (pins.empty()) continue;
            std::sort(pins.begin(), pins.end());
            pins.erase(std::unique(pins.begin(), pins.end()), pins.end());
            pins.insert(pins.begin(), d);
            uint64_t h = 1469598103934665603ull;
            for (int p : pins) h = (h ^ (uint64_t)(p + 1)) * 1099511628211ull;
            bool merged = false;
            for (int o : byHash[h]) {
                int len = c.netOff[o + 1] - c.netOff[o];
                if (len == (int)pins.size() && std::equal(pins.begin(), pins.end(), c.netPins.begin() + c.netOff[o])) {
                    c.netW[o] += g.netW[e];
                    merged = true;
                    break;
                }
            }
            if (merged) continue;
            byHash[h].push_back(c.ne());
            c.addNet(pins, g.netW[e]);
        }
        c.index();
        return c;
    }

    // Fills the FPGAs one after another along a breadth-first walk of the graph.
    std::vector<int> initial(const HGraph& g, Rng& rng) const {
        int n = g.nv;
        std::vector<int> part(n, -1), order;
        std::vector<Wt> load(k, Wt{0, 0, 0});
        for (int v = 0; v < n; v++)
            if (g.fixed[v] >= 0) {
                part[v] = g.fixed[v];
                for (int r = 0; r < 3; r++) load[part[v]][r] += g.w[v][r];
            }
        std::vector<char> seen(n, 0);
        std::vector<int> starts(n);
        for (int v = 0; v < n; v++) starts[v] = v;
        rng.shuffle(starts);
        for (int s : starts) {
            if (seen[s]) continue;
            seen[s] = 1;
            size_t h = order.size();
            order.push_back(s);
            for (; h < order.size(); h++) {
                int v = order[h];
                for (int x = g.vOff[v]; x < g.vOff[v + 1]; x++) {
                    int e = g.vNets[x];
                    if (g.netOff[e + 1] - g.netOff[e] > 256) continue;
                    for (int y = g.netOff[e]; y < g.netOff[e + 1]; y++) {
                        int u = g.netPins[y];
                        if (!seen[u]) {
                            seen[u] = 1;
                            order.push_back(u);
                        }
                    }
                }
            }
        }
        int p = 0;
        for (int v : order) {
            if (part[v] >= 0) continue;
            int q = p;
            while (q < k && !fits(load[q], g.w[v], q)) q++;
            if (q == k) {
                // Nothing has room after p: try the earlier ones, then the emptiest.
                for (q = 0; q < p && !fits(load[q], g.w[v], q); q++) {}
                if (q == p) {
                    double best = 1e300;
                    for (int t = 0; t < k; t++) {
                        double f = 0;
                        for (int r = 0; r < 3; r++) f = std::max(f, (load[t][r] + g.w[v][r]) / cap[t][r]);
                        if (f < best) {
                            best = f;
                            q = t;
                        }
                    }
                }
            } else {
                p = q;
            }
            part[v] = q;
            for (int r = 0; r < 3; r++) load[q][r] += g.w[v][r];
        }
        return part;
    }

    // Moves single vertices to the FPGA where the cut costs least, as long as
    // they fit. First greedily (only moves that gain; a vertex in an overfull
    // FPGA moves out even at a cost), then Fiduccia-Mattheyses passes: always
    // take the best move left, even a losing one, lock the vertex, and at the
    // end keep the prefix of the sequence that cost least. That gets out of
    // the local minima the greedy pass stops in.
    void refine(const HGraph& g, std::vector<int>& part, Rng& rng) const {
        int n = g.nv, ne = g.ne();
        std::vector<int> cnt((size_t)ne * k, 0);  // readers of net e in FPGA p
        std::vector<Wt> load(k, Wt{0, 0, 0});
        for (int v = 0; v < n; v++)
            for (int r = 0; r < 3; r++) load[part[v]][r] += g.w[v][r];
        for (int e = 0; e < ne; e++)
            for (int x = g.netOff[e] + 1; x < g.netOff[e + 1]; x++) cnt[(size_t)e * k + part[g.netPins[x]]]++;
        // ncut[e]: FPGAs other than the driver's that read e. A vertex whose
        // nets are all uncut cannot gain by moving, and most are like that.
        std::vector<int> ncut(ne, 0);
        auto countCut = [&](int e) {
            int d = part[g.netPins[g.netOff[e]]], c = 0;
            for (int p = 0; p < k; p++) c += (p != d && cnt[(size_t)e * k + p] != 0);
            return c;
        };
        for (int e = 0; e < ne; e++) ncut[e] = countCut(e);
        std::vector<double> delta(k);
        auto over = [&](int p) {
            for (int r = 0; r < 3; r++)
                if (load[p][r] > cap[p][r] * (1 + 1e-9)) return true;
            return false;
        };
        auto onCut = [&](int v) {
            for (int x = g.vOff[v]; x < g.vOff[v + 1]; x++)
                if (ncut[g.vNets[x]]) return true;
            return false;
        };
        // delta[b]: what the cut cost changes by when v moves to FPGA b.
        auto evalMoves = [&](int v) {
            int a = part[v];
            std::fill(delta.begin(), delta.end(), 0.0);
            for (int x = g.vOff[v]; x < g.vOff[v + 1]; x++) {
                int e = g.vNets[x];
                const int* c = &cnt[(size_t)e * k];
                double w = g.netW[e];
                int dv = g.netPins[g.netOff[e]];
                if (dv == v) {
                    // v drives e: the cost is the distance from v's FPGA to each reader's.
                    double ca = 0;
                    for (int p = 0; p < k; p++)
                        if (c[p] && p != a) ca += B.dist[a][p];
                    for (int b = 0; b < k; b++) {
                        if (b == a) continue;
                        double cb = 0;
                        for (int p = 0; p < k; p++)
                            if (c[p] && p != b) cb += B.dist[b][p];
                        delta[b] += w * (cb - ca);
                    }
                } else {
                    int d = part[dv];
                    double leave = (c[a] == 1 && a != d) ? -B.dist[d][a] : 0;
                    for (int b = 0; b < k; b++) {
                        if (b == a) continue;
                        delta[b] += w * (leave + ((c[b] == 0 && b != d) ? B.dist[d][b] : 0));
                    }
                }
            }
        };
        // The cheapest FPGA v fits in, or -1.
        auto bestTarget = [&](int v) {
            int a = part[v], best = -1;
            for (int b = 0; b < k; b++) {
                if (b == a || !fits(load[b], g.w[v], b)) continue;
                if (best < 0 || delta[b] < delta[best]) best = b;
            }
            return best;
        };
        auto apply = [&](int v, int b) {
            int a = part[v];
            part[v] = b;
            for (int x = g.vOff[v]; x < g.vOff[v + 1]; x++) {
                int e = g.vNets[x];
                if (g.netPins[g.netOff[e]] != v) {
                    cnt[(size_t)e * k + a]--;
                    cnt[(size_t)e * k + b]++;
                }
                ncut[e] = countCut(e);
            }
            for (int r = 0; r < 3; r++) {
                load[a][r] -= g.w[v][r];
                load[b][r] += g.w[v][r];
            }
        };

        std::vector<int> order(n);
        for (int v = 0; v < n; v++) order[v] = v;
        for (int pass = 0; pass < 12; pass++) {
            rng.shuffle(order);
            int moves = 0;
            for (int v : order) {
                if (g.fixed[v] >= 0) continue;
                bool mustLeave = over(part[v]);
                if (!mustLeave && !onCut(v)) continue;
                evalMoves(v);
                int best = bestTarget(v);
                if (best < 0) continue;
                bool weightless = g.w[v][0] == 0 && g.w[v][1] == 0 && g.w[v][2] == 0;
                if (!(delta[best] < 0 || (mustLeave && !weightless))) continue;
                apply(v, best);
                moves++;
            }
            if (!moves || (n > 20000 && moves * 2000 < n)) break;  // the last 0.05% is not worth a pass
        }

        // Fiduccia-Mattheyses passes over the vertices on the cut.
        struct Cand {
            double d;
            int v, ver;
            bool operator<(const Cand& o) const { return d != o.d ? d > o.d : v > o.v; }  // min-heap on (d, v)
        };
        std::vector<int> ver(n, 0);
        std::vector<char> locked(n, 0);
        std::vector<Cand> heap;
        std::vector<std::pair<int, int>> trail;  // vertex, the FPGA it came from
        auto push = [&](int v) {
            if (locked[v] || g.fixed[v] >= 0) return;
            evalMoves(v);
            int b = bestTarget(v);
            if (b < 0) return;
            heap.push_back({delta[b], v, ver[v]});
            std::push_heap(heap.begin(), heap.end());
        };
        for (int pass = 0; pass < 4; pass++) {
            heap.clear();
            trail.clear();
            std::fill(locked.begin(), locked.end(), 0);
            for (int v : order)
                if (onCut(v)) push(v);
            double cur = 0, best = 0;
            size_t bestLen = 0;
            size_t patience = std::max<size_t>(32, std::min<size_t>(250, heap.size() / 8));
            while (!heap.empty() && trail.size() - bestLen < patience) {
                std::pop_heap(heap.begin(), heap.end());
                Cand c = heap.back();
                heap.pop_back();
                if (locked[c.v] || c.ver != ver[c.v]) continue;
                evalMoves(c.v);
                int b = bestTarget(c.v);
                if (b < 0) continue;
                if (delta[b] > c.d + 1e-9) {  // its neighbourhood changed: queue it again at today's price
                    heap.push_back({delta[b], c.v, ver[c.v]});
                    std::push_heap(heap.begin(), heap.end());
                    continue;
                }
                cur += delta[b];
                int a = part[c.v];
                trail.push_back({c.v, a});
                locked[c.v] = 1;
                apply(c.v, b);
                if (cur < best - 1e-9) {
                    best = cur;
                    bestLen = trail.size();
                }
                for (int x = g.vOff[c.v]; x < g.vOff[c.v + 1]; x++) {
                    int e = g.vNets[x];
                    if (g.netOff[e + 1] - g.netOff[e] > 64) continue;
                    // The move changes what the others on e would gain only when it
                    // made or removed the first or second reader in an FPGA, or moved the driver.
                    const int* ce = &cnt[(size_t)e * k];
                    if (g.netPins[g.netOff[e]] != c.v && ce[a] > 1 && ce[b] > 2) continue;
                    for (int y = g.netOff[e]; y < g.netOff[e + 1]; y++) {
                        int u = g.netPins[y];
                        if (locked[u]) continue;
                        ver[u]++;
                        push(u);
                    }
                }
            }
            while (trail.size() > bestLen) {
                apply(trail.back().first, trail.back().second);
                trail.pop_back();
            }
            if (best > -1e-9) break;
        }
    }
};

// ---------------------------------------------------------------------------

// The inputs of an instance that carry a value into it: its data and control
// pins, and for a clock pin the net the clock is really made on (never a
// primary clock: every FPGA makes those itself).
template <class F>
void forEachInput(const Design& d, int i, F&& f) {
    auto& in = d.nl.insts[i];
    for (size_t c = 0; c < in.conns.size(); c++) {
        PinCls pc = d.pinCls(i, c);
        int n = in.conns[c].net;
        if (pc == POut || pc == PClk || n < 0 || d.isConst(n) || d.clockOfNet[n] >= 0) continue;
        f(n);
    }
    if (d.kind(i) == Kind::FF) {
        if (d.trig[i].root >= 0) f(d.trig[i].root);
    } else if (d.kind(i) == Kind::Mem) {
        for (auto& t : d.memTrig.at(i))
            if (t.root >= 0) f(t.root);
    }
}

struct LinkUse {
    int a = 0, b = 0, pins = 0;
    std::vector<int> ab, ba;       // the nets that go a to b, and b to a
    int pinsAB = 0, pinsBA = 0;    // wires given to each direction
    int ratioAB = 1, ratioBA = 1;  // signals per wire; 1: its own wire
    // On a multiplexed direction, the first so many nets of the list still
    // have a wire each: the ones whose delay costs the most.
    int directAB = 0, directBA = 0;
};

// Where everything went: the FPGA of each cell and port bit, what crosses
// each link, and how long a value takes to settle.
struct Plan {
    std::vector<int> part;               // per instance, then per port
    std::vector<LinkUse> links;
    int B = 1;                           // emulation cycles per delta
    PartStats stats;
    int starts = 0, routed = 1;          // partitioning attempts, and how many of the best were routed
    std::vector<Wt> load;
    double givenCost = -1;               // the cut cost of the -partitioner attempt, if there was one
    int Bshared = 1;                     // what B would be with every signal of a short link taking turns
    double tGraph = 0, tPart = 0, tRoute = 0;  // seconds spent
    int partOfPort(const Design& d, int p) const { return part[d.ni() + p]; }
    // The FPGA a net is driven in.
    int netPart(const Design& d, int n) const {
        return d.drv[n] >= 0 ? part[d.drv[n]] : d.drv[n] <= -2 ? partOfPort(d, -2 - d.drv[n]) : 0;
    }
};

inline HGraph buildGraph(const Design& d, const std::vector<int>& fixed) {
    HGraph g;
    size_t ni = d.ni();
    g.nv = (int)(ni + d.nl.ports.size());
    g.w.assign(g.nv, Wt{0, 0, 0});
    g.fixed = fixed;
    g.fixed.resize(g.nv, -1);
    for (size_t i = 0; i < ni; i++) {
        auto& r = d.roleOfInst((int)i);
        g.w[i] = {r.lut, r.ff, r.bramBits};
    }
    std::vector<std::vector<int>> readers(d.nn());
    for (size_t i = 0; i < ni; i++) forEachInput(d, (int)i, [&](int n) { readers[n].push_back((int)i); });
    for (size_t p = 0; p < d.nl.ports.size(); p++) {
        auto& pt = d.nl.ports[p];
        if (pt.dir != gnl::Dir::In && pt.net >= 0 && !d.isConst(pt.net) && d.clockOfNet[pt.net] < 0)
            readers[pt.net].push_back((int)(ni + p));
    }
    // Bits of one bus usually have the same driver and readers: one net, weighted.
    std::unordered_map<uint64_t, std::vector<int>> byHash;
    std::vector<int> pins;
    for (size_t n = 0; n < d.nn(); n++) {
        if (readers[n].empty() || d.isConst((int)n) || d.clockOfNet[n] >= 0) continue;
        int dv = d.drv[n] >= 0 ? d.drv[n] : d.drv[n] <= -2 ? (int)ni + (-2 - d.drv[n]) : -1;
        auto& rd = readers[n];
        std::sort(rd.begin(), rd.end());
        rd.erase(std::unique(rd.begin(), rd.end()), rd.end());
        pins.clear();
        if (dv < 0) {
            dv = rd[0];  // nothing drives it: its readers still belong together
        }
        pins.push_back(dv);
        for (int r : rd)
            if (r != dv) pins.push_back(r);
        if (pins.size() < 2) continue;
        uint64_t h = 1469598103934665603ull;
        for (int p : pins) h = (h ^ (uint64_t)(p + 1)) * 1099511628211ull;
        bool merged = false;
        for (int o : byHash[h]) {
            int len = g.netOff[o + 1] - g.netOff[o];
            if (len == (int)pins.size() && std::equal(pins.begin(), pins.end(), g.netPins.begin() + g.netOff[o])) {
                g.netW[o]++;
                merged = true;
                break;
            }
        }
        if (merged) continue;
        byHash[h].push_back(g.ne());
        g.addNet(pins, 1);
    }
    g.index();
    return g;
}

// Partition, then route every cut net over the board's links, share wires
// where a link has too few, and work out the cycles per delta.
// Given the partition: routes the crossing nets over the board, shares out
// the wires, and works out the cycles per delta.
inline void routePlan(Plan& pl, const Design& d, const Board& board, const HGraph& g, unsigned threads,
                      bool tuneWires = true) {
    int k = board.k();
    size_t ni = d.ni();
    pl.links.clear();
    pl.load.assign(k, Wt{0, 0, 0});
    for (int v = 0; v < g.nv; v++)
        for (int r = 0; r < 3; r++) pl.load[pl.part[v]][r] += g.w[v][r];
    if (pl.stats.over > 0) {
        std::ostringstream m;
        m << "the design does not fit the board at " << board.util * 100 << "% fill:";
        for (int p = 0; p < k; p++) {
            auto& f = board.fpgas[p];
            m << "\n  " << f.name << ": " << (long)std::ceil(pl.load[p][0]) << " LUTs";
            if (f.lut > 0) m << " of " << (long)f.lut;
            m << ", " << (long)pl.load[p][1] << " flops";
            if (f.ff > 0) m << " of " << (long)f.ff;
            m << ", " << (long)std::ceil(pl.load[p][2] / 1024) << " kb block RAM";
            if (f.bramKb > 0) m << " of " << (long)f.bramKb;
        }
        throw Error(m.str());
    }

    // Routes: from the driver's FPGA to every FPGA that reads the net, one
    // board hop at a time. An FPGA on the way passes the net through.
    for (auto& l : board.links) {
        LinkUse u;
        u.a = l.a;
        u.b = l.b;
        u.pins = l.pins;
        pl.links.push_back(u);
    }
    std::vector<uint64_t> readers(d.nn(), 0);
    for (size_t i = 0; i < ni; i++)
        forEachInput(d, (int)i, [&](int n) { readers[n] |= 1ull << pl.part[i]; });
    for (size_t p = 0; p < d.nl.ports.size(); p++) {
        auto& port = d.nl.ports[p];
        if (port.dir != gnl::Dir::In && port.net >= 0 && !d.isConst(port.net) && d.clockOfNet[port.net] < 0)
            readers[port.net] |= 1ull << pl.partOfPort(d, (int)p);
    }
    std::vector<int> last((size_t)k * k, -1);
    for (size_t n = 0; n < d.nn(); n++) {
        if (!readers[n] || d.drv[n] == -1) continue;
        int a = pl.netPart(d, (int)n);
        for (int b = 0; b < k; b++) {
            if (b == a || !(readers[n] >> b & 1)) continue;
            if (board.dist[a][b] >= (1 << 20))
                throw Error("the board has no route from " + board.fpgas[a].name + " to " + board.fpgas[b].name);
            // Back from b along the shortest-path tree rooted at a, so an
            // FPGA is handed a net by one neighbour only.
            for (int v = b; v != a;) {
                int u = board.prev(a, v);
                if (last[(size_t)u * k + v] != (int)n) {
                    last[(size_t)u * k + v] = (int)n;
                    auto& l = pl.links[board.linkOf[u][v]];
                    (l.a == u ? l.ab : l.ba).push_back((int)n);
                }
                v = u;
            }
        }
    }
    // A link with fewer wires than signals sends several signals down each
    // wire in turn (time-division multiplexing). A signal then takes up to
    // ratio + 1 emulation cycles to arrive: ratio to get its turn, one for the
    // receiving register. Not every signal has to: some keep a wire of their
    // own, and the rest share what is left at a higher ratio.
    struct DirUse {
        LinkUse* l;
        bool ab;
        int from, to, pins;
        std::vector<int>* nets;
        std::unordered_map<int, int> pos;
        bool shared() const { return (int)nets->size() > pins; }
        int maxDirect(int tdmMax) const {  // the most wires that can go to single signals
            int x = (int)nets->size(), best = 0;
            for (int D = 1; D < pins; D++)
                if ((x - D + pins - D - 1) / (pins - D) <= tdmMax) best = D;
            return best;
        }
        int ratio(int D) const {
            int x = (int)nets->size();
            return x <= pins ? 1 : (x - D + pins - D - 1) / (pins - D);
        }
    };
    std::vector<DirUse> dirs;
    std::vector<std::vector<int>> dirOf(k, std::vector<int>(k, -1));
    for (auto& l : pl.links) {
        int x = (int)l.ab.size(), y = (int)l.ba.size();
        if (x + y <= l.pins) {
            l.pinsAB = x;
            l.pinsBA = y;
        } else {
            if ((x > 0) + (y > 0) > l.pins)
                throw Error("link " + board.fpgas[l.a].name + "-" + board.fpgas[l.b].name + " has " +
                            std::to_string(l.pins) + " wire, and signals go both ways");
            if (x && y) {
                l.pinsAB = std::min(l.pins - 1, std::max(1, (int)std::lround((double)l.pins * x / (x + y))));
                l.pinsBA = l.pins - l.pinsAB;
            } else {
                l.pinsAB = x ? l.pins : 0;
                l.pinsBA = y ? l.pins : 0;
            }
            l.pinsAB = std::min(l.pinsAB, x);
            l.pinsBA = std::min(l.pinsBA, y);
            int worst = std::max(x ? (x + l.pinsAB - 1) / l.pinsAB : 1, y ? (y + l.pinsBA - 1) / l.pinsBA : 1);
            if (worst > board.tdmMax)
                throw Error("link " + board.fpgas[l.a].name + "-" + board.fpgas[l.b].name + ": " + std::to_string(x + y) +
                            " signals over " + std::to_string(l.pins) + " wires needs " + std::to_string(worst) +
                            " signals per wire, more than tdm_max " + std::to_string(board.tdmMax));
        }
        if (x) {
            dirOf[l.a][l.b] = (int)dirs.size();
            dirs.push_back({&l, true, l.a, l.b, l.pinsAB, &l.ab, {}});
        }
        if (y) {
            dirOf[l.b][l.a] = (int)dirs.size();
            dirs.push_back({&l, false, l.b, l.a, l.pinsBA, &l.ba, {}});
        }
    }
    int nd = (int)dirs.size();
    auto index = [&](DirUse& du) {
        du.pos.clear();
        for (size_t x = 0; x < du.nets->size(); x++) du.pos[(*du.nets)[x]] = (int)x;
    };
    for (auto& du : dirs) index(du);
    // The nets that cross, and for each the FPGAs it must reach.
    std::vector<int> cutIdx(d.nn(), -1), cutNets;
    for (auto& du : dirs)
        for (int n : *du.nets)
            if (cutIdx[n] < 0) {
                cutIdx[n] = (int)cutNets.size();
                cutNets.push_back(n);
            }

    // Cycles per delta: one, plus the slowest way a value can travel between
    // two steps (through multiplexed links, and through block RAM that stands
    // in for a memory read without a clock). `direct` is how many nets of
    // each direction have their own wire.
    // Only what lies after a crossing net (or a memory read that takes a
    // cycle) can be late at all: everything else arrives at 0, and is left
    // out of the walk.
    std::vector<int> cone;
    {
        // in: something late reaches the instance. out: its output can be
        // late too (a flop's only through its set and reset pins).
        std::vector<char> in(ni, 0), out(ni, 0);
        auto lateNet = [&](int n) { return n >= 0 && (cutIdx[n] >= 0 || (d.drv[n] >= 0 && out[d.drv[n]])); };
        for (int pass = 0; pass < 2; pass++)
            for (int i : d.order) {
                if (in[i] && out[i]) continue;
                Kind kd = d.kind(i);
                bool any = false, async = false;
                auto& inst = d.nl.insts[i];
                for (size_t c = 0; c < inst.conns.size(); c++) {
                    PinCls pc = d.pinCls(i, c);
                    if (pc == POut || pc == PClk || !lateNet(inst.conns[c].net)) continue;
                    any = true;
                    async |= pc == PAsync;
                }
                forEachInput(d, i, [&](int n) { any |= lateNet(n); });
                bool lagRead = kd == Kind::Mem && d.roleOfInst(i).mem->lagRead;
                in[i] = any || lagRead;
                out[i] = kd == Kind::FF ? async : kd == Kind::Mem ? d.roleOfInst(i).mem->asyncRead() && in[i] : any;
            }
        for (int i : d.order)
            if (in[i]) cone.push_back(i);
    }
    // One array of arrival times per thread, zero outside the cone for good.
    std::mutex scratchMu;
    std::unordered_map<std::thread::id, std::vector<int>> scratch;
    auto scratchA = [&]() -> std::vector<int>& {
        std::lock_guard<std::mutex> lock(scratchMu);
        auto& v = scratch[std::this_thread::get_id()];
        if (v.empty()) v.assign(ni, 0);
        return v;
    };
    auto analyze = [&](const std::vector<int>& direct, std::vector<int>* crit) {
        // How late each crossing net is in each FPGA it is routed to.
        std::vector<int> late(cutNets.size() * (size_t)k, 0);
        for (size_t c = 0; c < cutNets.size(); c++) {
            int n = cutNets[c], a = pl.netPart(d, n);
            for (int b = 0; b < k; b++) {
                if (b == a || board.dist[a][b] >= (1 << 20)) continue;
                int t = 0;
                for (int v = b; v != a;) {
                    int u = board.prev(a, v), di = dirOf[u][v];
                    if (di < 0) {
                        t = 0;
                        break;
                    }
                    auto& du = dirs[di];
                    auto it = du.pos.find(n);
                    if (it == du.pos.end()) {  // not routed this way: nothing in b reads it
                        t = 0;
                        break;
                    }
                    int r = du.ratio(direct[di]);
                    if (r > 1 && it->second >= direct[di]) t += r + 1;
                    v = u;
                }
                late[c * k + b] = t;
            }
        }
        std::vector<int>& A = scratchA();
        for (int i : cone) A[i] = 0;
        int maxA = 0;
        auto lagOf = [&](int n, int to) { return cutIdx[n] < 0 ? 0 : late[(size_t)cutIdx[n] * k + to]; };
        auto arrive = [&](int n, int to) {
            if (n < 0 || d.isConst(n) || d.clockOfNet[n] >= 0 || d.drv[n] == -1) return 0;
            return (d.drv[n] >= 0 ? A[d.drv[n]] : 0) + lagOf(n, to);
        };
        auto eval = [&](int i) {
            int me = pl.part[i], a = 0, all = 0;
            Kind kd = d.kind(i);
            auto& in = d.nl.insts[i];
            for (size_t c = 0; c < in.conns.size(); c++) {
                PinCls pc = d.pinCls(i, c);
                if (pc == POut || pc == PClk) continue;
                int t = arrive(in.conns[c].net, me);
                all = std::max(all, t);
                if (kd == Kind::FF ? pc == PAsync : true) a = std::max(a, t);
            }
            if (kd == Kind::FF && d.trig[i].root >= 0) all = std::max(all, arrive(d.trig[i].root, me));
            if (kd == Kind::Mem) {
                for (auto& t : d.memTrig.at(i))
                    if (t.root >= 0) all = std::max(all, arrive(t.root, me));
                auto* m = d.roleOfInst(i).mem;
                a = m->asyncRead() ? a + (m->lagRead ? 1 : 0) : 0;
            }
            maxA = std::max(maxA, std::max(all, a));
            return a;
        };
        for (int round = 0; round < 3; round++) {
            bool changed = false;
            for (int i : cone) {
                int a = eval(i);
                if (a != A[i]) {
                    A[i] = a;
                    changed = true;
                }
            }
            if (!changed) break;
        }
        for (size_t p = 0; p < d.nl.ports.size(); p++)
            if (d.nl.ports[p].dir != gnl::Dir::In)
                maxA = std::max(maxA, arrive(d.nl.ports[p].net, pl.partOfPort(d, (int)p)));
        if (crit) {
            // For each crossing net, the longest way through it: what came
            // before its driver, and what still follows its slowest reader.
            // Q is what follows an instance's output.
            std::vector<int> Q(ni, 0), qNet(d.nn(), 0);
            for (size_t p = 0; p < d.nl.ports.size(); p++) {
                auto& port = d.nl.ports[p];
                if (port.dir != gnl::Dir::In && port.net >= 0)
                    qNet[port.net] = std::max(qNet[port.net], lagOf(port.net, pl.partOfPort(d, (int)p)));
            }
            // Readers come after their drivers in the order, so backwards.
            for (size_t x = d.order.size(); x-- > 0;) {
                int i = d.order[x];
                auto& in = d.nl.insts[i];
                for (size_t c = 0; c < in.conns.size(); c++) {
                    int n = in.conns[c].net;
                    if (d.pinCls(i, c) == POut && n >= 0) Q[i] = std::max(Q[i], qNet[n]);
                }
                Kind kd = d.kind(i);
                bool through = kd != Kind::FF && (kd != Kind::Mem || d.roleOfInst(i).mem->asyncRead());
                int qi = through ? Q[i] : 0;
                forEachInput(d, i, [&](int n) {
                    int t = lagOf(n, pl.part[i]) + qi;
                    if (t > qNet[n]) qNet[n] = t;
                });
            }
            crit->assign(cutNets.size(), 0);
            for (size_t c = 0; c < cutNets.size(); c++) {
                int n = cutNets[c];
                (*crit)[c] = (d.drv[n] >= 0 ? A[d.drv[n]] : 0) + qNet[n];
            }
        }
        return maxA;
    };

    std::vector<int> direct(nd, 0);
    int bestA = analyze(direct, nullptr);
    pl.Bshared = 1 + bestA;
    bool anyShared = false;
    for (auto& du : dirs) anyShared |= du.shared() && du.maxDirect(board.tdmMax) > 0;
    if (tuneWires && anyShared && bestA > 0) {
        auto bestDirect = direct;
        std::vector<std::vector<int>> bestNets;
        for (auto& du : dirs) bestNets.push_back(*du.nets);
        for (int pass = 0; pass < 4; pass++) {
            // Order each shared direction by how much its nets' delay costs,
            // then give the first of them wires of their own while that helps.
            std::vector<int> crit;
            analyze(direct, &crit);
            for (auto& du : dirs) {
                if (!du.shared()) continue;
                std::stable_sort(du.nets->begin(), du.nets->end(), [&](int x, int y) {
                    int cx = crit[cutIdx[x]], cy = crit[cutIdx[y]];
                    return cx != cy ? cx > cy : x < y;
                });
                index(du);
            }
            // First every direction at once: own wires for the nets on paths
            // longer than a threshold, for a spread of thresholds.
            std::vector<int> levels;
            for (auto& du : dirs)
                if (du.shared())
                    for (int n : *du.nets) levels.push_back(crit[cutIdx[n]]);
            std::sort(levels.begin(), levels.end(), std::greater<int>());
            levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
            if (levels.size() > 12) {
                std::vector<int> some;
                for (int x = 0; x < 12; x++) some.push_back(levels[x * (levels.size() - 1) / 11]);
                levels = some;
            }
            std::vector<std::vector<int>> tries(levels.size() + 1, direct);
            for (size_t x = 0; x < levels.size(); x++)
                for (int di = 0; di < nd; di++) {
                    auto& du = dirs[di];
                    if (!du.shared()) continue;
                    int D = 0;
                    while (D < (int)du.nets->size() && crit[cutIdx[(*du.nets)[D]]] >= levels[x]) D++;
                    tries[x][di] = std::min(D, du.maxDirect(board.tdmMax));
                }
            std::vector<int> got(tries.size());
            parallelFor(tries.size(), threads, [&](size_t x) { got[x] = analyze(tries[x], nullptr); });
            size_t bt = tries.size() - 1;  // as it was, unless something is better
            for (size_t x = 0; x < levels.size(); x++)
                if (got[x] < got[bt]) bt = x;
            direct = tries[bt];
            int passA = got[bt];
            // Then one direction at a time, where the design is small enough to afford it.
            bool fine = cone.size() * (size_t)nd < 4000000;
            for (int di = 0; di < nd && fine; di++) {
                auto& du = dirs[di];
                if (!du.shared()) continue;
                int top = du.maxDirect(board.tdmMax);
                std::vector<int> cand;
                for (int D = 0; D <= top; D = D ? D * 2 : 1) cand.push_back(D);
                for (int D = top; D > top - 4 && D > 0; D--) cand.push_back(D);
                std::sort(cand.begin(), cand.end());
                cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
                std::vector<int> res(cand.size());
                parallelFor(cand.size(), threads, [&](size_t x) {
                    auto dd = direct;
                    dd[di] = cand[x];
                    res[x] = analyze(dd, nullptr);
                });
                size_t bx = 0;
                for (size_t x = 1; x < cand.size(); x++)
                    if (res[x] < res[bx]) bx = x;
                direct[di] = cand[bx];
                passA = res[bx];
            }
            if (passA >= bestA) break;
            bestA = passA;
            bestDirect = direct;
            for (int di = 0; di < nd; di++) bestNets[di] = *dirs[di].nets;
        }
        direct = bestDirect;
        for (int di = 0; di < nd; di++) {
            *dirs[di].nets = bestNets[di];
            index(dirs[di]);
        }
    }
    for (int di = 0; di < nd; di++) {
        auto& du = dirs[di];
        int r = du.ratio(direct[di]);
        (du.ab ? du.l->ratioAB : du.l->ratioBA) = r;
        (du.ab ? du.l->directAB : du.l->directBA) = r > 1 ? direct[di] : 0;
    }
    pl.B = 1 + analyze(direct, nullptr);
}

inline Plan makePlan(const Design& d, const Board& board, const std::vector<int>& fixed, int starts,
                     int tries, unsigned threads, const std::vector<int>* given = nullptr) {
    Plan pl;
    int k = board.k();
    size_t ni = d.ni();
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&] {
        auto t1 = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        t0 = t1;
        return s;
    };
    HGraph g = buildGraph(d, fixed);
    pl.tGraph = lap();
    Partitioner pt(board);
    if (k == 1) {
        pl.part.assign(g.nv, 0);
        pl.stats.over = pt.overflow(g, pl.part);
        pl.starts = 1;
    } else {
        std::vector<std::vector<int>> parts(starts);
        std::vector<PartStats> st(starts);
        parallelFor((size_t)starts, threads, [&](size_t s) { parts[s] = pt.run(g, (int)s + 1, st[s]); });
        if (given) {  // one more attempt: the other partitioner's, polished
            if ((int)given->size() != g.nv) throw Error("the partitioner's answer has the wrong number of lines");
            parts.emplace_back();
            st.emplace_back();
            parts.back() = pt.polish(g, *given, st.back());
            pl.givenCost = st.back().cost;
            starts++;
            tries++;
        }
        // The attempts, best first: least overflow, then least cost.
        std::vector<int> rank(starts);
        for (int s = 0; s < starts; s++) rank[s] = s;
        std::stable_sort(rank.begin(), rank.end(), [&](int x, int y) {
            if (std::abs(st[x].over - st[y].over) > 1e-12) return st[x].over < st[y].over;
            return st[x].cost < st[y].cost;
        });
        // The cut is a means: what the user waits for is emu_clk cycles per
        // delta. Route the few best cuts and keep the fastest of them.
        std::vector<int> cand;
        for (int s : rank) {
            if (std::abs(st[s].over - st[rank[0]].over) > 1e-12 || (int)cand.size() >= tries) break;
            bool dup = false;
            for (int c : cand) dup |= parts[c] == parts[s];
            if (!dup) cand.push_back(s);
        }
        pl.tPart = lap();
        std::vector<Plan> plans(cand.size());
        std::vector<std::string> errs(cand.size());
        parallelFor(cand.size(), threads, [&](size_t x) {
            plans[x].part = parts[cand[x]];
            plans[x].stats = st[cand[x]];
            try {
                routePlan(plans[x], d, board, g, 1, false);
            } catch (const Error& e) {
                errs[x] = e.what();
                if (errs[x].empty()) errs[x] = "error";
            }
        });
        // Sharing the wires out well takes longer: only for the two that
        // are fastest before it.
        std::vector<int> byB;
        for (size_t x = 0; x < cand.size(); x++)
            if (errs[x].empty()) byB.push_back((int)x);
        if (byB.empty()) throw Error(errs[0]);
        std::stable_sort(byB.begin(), byB.end(), [&](int x, int y) { return plans[x].B < plans[y].B; });
        int best = byB[0];
        if (plans[best].B > 1) {
            if (byB.size() > 2) byB.resize(2);
            for (int x : byB) routePlan(plans[x], d, board, g, threads);
            if (byB.size() > 1 && plans[byB[1]].B < plans[byB[0]].B) best = byB[1];
        }
        double tg = pl.tGraph, tp = pl.tPart, gc = pl.givenCost;
        pl = std::move(plans[best]);
        pl.tGraph = tg;
        pl.tPart = tp;
        pl.starts = starts - (given ? 1 : 0);
        pl.givenCost = gc;
        pl.routed = (int)cand.size();
        pl.tRoute = lap();
        return pl;
    }
    pl.tPart = lap();
    routePlan(pl, d, board, g, threads);
    pl.tRoute = lap();
    return pl;
}


}  // namespace emu
