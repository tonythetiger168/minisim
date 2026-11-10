// faultsim.hpp - the fault simulator the test tools share (matpg, logic BIST,
// scan coverage): a combinational gate model, the fault list on it, and a
// parallel-pattern single-fault-propagation simulator.
//
//   Circuit   a levelized graph of simple gates. Node ids are in topological
//             order. Build it with add()/addv(), mark observe points in obs,
//             then finish(). atpg_model.hpp builds one from a gate netlist.
//   Fault     a stuck value at a node output (fi = -1) or at one input pin of
//             a gate, with an optional condition "node cond holds condv",
//             which is how a transition fault is posed on a two-frame model.
//   Sim       good(words) simulates 64 patterns at once (one word per circuit
//             input, three-valued); detect(work, fault) returns the mask of
//             those patterns that detect the fault. One Work per thread.
//   collapse  fault equivalence classes (Fault::rep).
#pragma once
#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
#include <vector>

namespace atpg {

enum T : uint8_t { IN, XS, C0, C1, BUF, NOT, AND, NAND, OR, NOR, XOR, XNOR, MUX };

struct Circuit {
    std::vector<uint8_t> type;
    std::vector<int> fiStart, fiN, fi;
    std::vector<int> foStart, fo;
    std::vector<int8_t> fixed;      // IN nodes the tester holds: 0, 1, or -1 free
    std::vector<uint8_t> obs;       // observed by the tester
    std::vector<int> inputs;        // the IN nodes, in pattern order
    std::vector<int> dist;          // gates to the nearest observe point
    std::vector<int> cc0, cc1;      // SCOAP controllability
    bool hasX = false;              // an X source or a held input exists

    int size() const { return (int)type.size(); }
    int add(T t, std::initializer_list<int> ins = {}) {
        type.push_back(t);
        fiStart.push_back((int)fi.size());
        fiN.push_back((int)ins.size());
        for (int i : ins) fi.push_back(i);
        fixed.push_back(-1);
        obs.push_back(0);
        if (t == IN) inputs.push_back(size() - 1);
        return size() - 1;
    }
    int addv(T t, const std::vector<int>& ins) {
        int n = add(t);
        fiN[n] = (int)ins.size();
        for (int i : ins) fi.push_back(i);
        return n;
    }
    const int* in(int n) const { return fi.data() + fiStart[n]; }

    void finish() {
        int N = size();
        std::vector<int> cnt(N + 1, 0);
        for (int f : fi) cnt[f + 1]++;
        foStart.assign(N + 1, 0);
        for (int i = 0; i < N; i++) foStart[i + 1] = foStart[i] + cnt[i + 1];
        fo.assign(fi.size(), 0);
        std::vector<int> pos(foStart.begin(), foStart.end() - 1);
        for (int n = 0; n < N; n++)
            for (int k = 0; k < fiN[n]; k++) fo[pos[in(n)[k]]++] = n;
        dist.assign(N, INT_MAX);
        for (int n = N - 1; n >= 0; n--) {
            if (obs[n]) { dist[n] = 0; continue; }
            for (int k = foStart[n]; k < foStart[n + 1]; k++)
                if (dist[fo[k]] != INT_MAX) dist[n] = std::min(dist[n], dist[fo[k]] + 1);
        }
        const int INF = 100000000;
        cc0.assign(N, INF);
        cc1.assign(N, INF);
        auto cap = [&](long v) { return (int)std::min<long>(v, INF); };
        for (int n = 0; n < N; n++) {
            const int* a = in(n);
            int k = fiN[n];
            long s0 = 0, s1 = 0, m0 = INF, m1 = INF;
            for (int i = 0; i < k; i++) {
                s0 += cc0[a[i]];
                s1 += cc1[a[i]];
                m0 = std::min<long>(m0, cc0[a[i]]);
                m1 = std::min<long>(m1, cc1[a[i]]);
            }
            switch (type[n]) {
                case IN:
                    if (fixed[n] != 1) cc0[n] = 1;
                    if (fixed[n] != 0) cc1[n] = 1;
                    if (fixed[n] >= 0) hasX = true;
                    break;
                case XS: hasX = true; break;
                case C0: cc0[n] = 0; break;
                case C1: cc1[n] = 0; break;
                case BUF: cc0[n] = cap(s0 + 1); cc1[n] = cap(s1 + 1); break;
                case NOT: cc0[n] = cap(s1 + 1); cc1[n] = cap(s0 + 1); break;
                case AND: cc0[n] = cap(m0 + 1); cc1[n] = cap(s1 + 1); break;
                case NAND: cc1[n] = cap(m0 + 1); cc0[n] = cap(s1 + 1); break;
                case OR: cc1[n] = cap(m1 + 1); cc0[n] = cap(s0 + 1); break;
                case NOR: cc0[n] = cap(m1 + 1); cc1[n] = cap(s0 + 1); break;
                case XOR:
                case XNOR: cc0[n] = cc1[n] = cap(std::min(s0, s1) + 1); break;
                case MUX:
                    cc0[n] = cap(std::min<long>((long)cc0[a[0]] + cc0[a[2]], (long)cc0[a[1]] + cc1[a[2]]) + 1);
                    cc1[n] = cap(std::min<long>((long)cc1[a[0]] + cc0[a[2]], (long)cc1[a[1]] + cc1[a[2]]) + 1);
                    break;
            }
        }
    }
};

enum Cls : uint8_t { NC, DS, DI, UU, UR, AU, ND };

struct Fault {
    int node = -1;       // -1: a fault outside the model (a clock or reset pin)
    int fi = -1;         // the input pin of `node`, or -1 for its output
    uint8_t sa = 0;
    int cond = -1;       // transition: the first-frame node that must hold condv
    uint8_t condv = 0;
    uint8_t cls = NC;
    int rep = -1;        // the fault this one is equivalent to (itself if none)
    int site = -1;       // the caller's index
};

// ------------------------------------------------------- fault simulator ----
// Values are two words per node: bit i of v0 says "0 in pattern i", bit i of
// v1 says "1 in pattern i", neither says X.
struct Sim {
    const Circuit& c;
    std::vector<uint64_t> g0, g1;

    struct Work {
        std::vector<uint64_t> f0, f1;
        std::vector<uint32_t> stamp, qstamp;
        std::vector<int> heap;
        uint32_t ep = 0;
        void init(int n) {
            f0.assign(n, 0);
            f1.assign(n, 0);
            stamp.assign(n, 0);
            qstamp.assign(n, 0);
        }
    };

    explicit Sim(const Circuit& cc) : c(cc), g0(cc.size()), g1(cc.size()) {}

    template <class Get>
    static inline void eval(const Circuit& c, int n, Get&& get, int force, uint64_t v0, uint64_t v1,
                            uint64_t& o0, uint64_t& o1) {
        const int* a = c.in(n);
        int k = c.fiN[n];
        auto in = [&](int i, uint64_t& x0, uint64_t& x1) {
            if (i == force) { x0 = v0; x1 = v1; }
            else get(a[i], x0, x1);
        };
        uint64_t x0, x1, z0, z1;
        switch (c.type[n]) {
            case BUF: in(0, o0, o1); break;
            case NOT: in(0, o1, o0); break;
            case AND:
            case NAND:
                z0 = 0; z1 = ~0ull;
                for (int i = 0; i < k; i++) { in(i, x0, x1); z0 |= x0; z1 &= x1; }
                if (c.type[n] == AND) { o0 = z0; o1 = z1; } else { o0 = z1; o1 = z0; }
                break;
            case OR:
            case NOR:
                z0 = ~0ull; z1 = 0;
                for (int i = 0; i < k; i++) { in(i, x0, x1); z0 &= x0; z1 |= x1; }
                if (c.type[n] == OR) { o0 = z0; o1 = z1; } else { o0 = z1; o1 = z0; }
                break;
            case XOR:
            case XNOR:
                in(0, z0, z1);
                for (int i = 1; i < k; i++) {
                    in(i, x0, x1);
                    uint64_t n1 = (z1 & x0) | (z0 & x1), n0 = (z0 & x0) | (z1 & x1);
                    z0 = n0; z1 = n1;
                }
                if (c.type[n] == XOR) { o0 = z0; o1 = z1; } else { o0 = z1; o1 = z0; }
                break;
            case MUX: {
                uint64_t a0, a1, b0, b1, s0, s1;
                in(0, a0, a1); in(1, b0, b1); in(2, s0, s1);
                o1 = (s0 & a1) | (s1 & b1) | (a1 & b1);
                o0 = (s0 & a0) | (s1 & b0) | (a0 & b0);
                break;
            }
            default: o0 = o1 = 0; break;
        }
    }

    // The good machine for 64 patterns: w[i] is the word of input i.
    void good(const std::vector<uint64_t>& w) {
        size_t pi = 0;
        int N = c.size();
        auto get = [&](int m, uint64_t& x0, uint64_t& x1) { x0 = g0[m]; x1 = g1[m]; };
        for (int n = 0; n < N; n++) {
            switch (c.type[n]) {
                case IN: {
                    uint64_t v = w[pi++];
                    if (c.fixed[n] >= 0) v = c.fixed[n] ? ~0ull : 0;
                    g1[n] = v; g0[n] = ~v;
                    break;
                }
                case XS: g0[n] = g1[n] = 0; break;
                case C0: g0[n] = ~0ull; g1[n] = 0; break;
                case C1: g0[n] = 0; g1[n] = ~0ull; break;
                default: eval(c, n, get, -1, 0, 0, g0[n], g1[n]);
            }
        }
    }

    // The patterns (as a bit mask) that detect the fault.
    uint64_t detect(Work& w, const Fault& f) const {
        uint64_t cm = ~0ull;
        if (f.cond >= 0) cm = f.condv ? g1[f.cond] : g0[f.cond];
        int line = f.fi < 0 ? f.node : c.in(f.node)[f.fi];
        uint64_t exc = (f.sa ? g0[line] : g1[line]) & cm;
        if (!exc) return 0;
        if (++w.ep == 0) {
            std::fill(w.stamp.begin(), w.stamp.end(), 0);
            std::fill(w.qstamp.begin(), w.qstamp.end(), 0);
            w.ep = 1;
        }
        uint32_t ep = w.ep;
        uint64_t det = 0;
        auto& heap = w.heap;
        heap.clear();
        auto get = [&](int m, uint64_t& x0, uint64_t& x1) {
            if (w.stamp[m] == ep) { x0 = w.f0[m]; x1 = w.f1[m]; }
            else { x0 = g0[m]; x1 = g1[m]; }
        };
        auto set = [&](int n, uint64_t v0, uint64_t v1) {
            w.f0[n] = v0; w.f1[n] = v1; w.stamp[n] = ep;
            if (c.obs[n]) det |= (g0[n] & v1) | (g1[n] & v0);
            for (int k = c.foStart[n]; k < c.foStart[n + 1]; k++) {
                int m = c.fo[k];
                if (w.qstamp[m] != ep) {
                    w.qstamp[m] = ep;
                    heap.push_back(m);
                    std::push_heap(heap.begin(), heap.end(), std::greater<int>());
                }
            }
        };
        uint64_t s0 = f.sa ? 0 : ~0ull, s1 = f.sa ? ~0ull : 0;
        if (f.fi < 0) {
            set(f.node, s0, s1);
        } else {
            uint64_t o0, o1;
            eval(c, f.node, get, f.fi, s0, s1, o0, o1);
            if (o0 == g0[f.node] && o1 == g1[f.node]) return 0;
            set(f.node, o0, o1);
        }
        while (!heap.empty()) {
            std::pop_heap(heap.begin(), heap.end(), std::greater<int>());
            int n = heap.back();
            heap.pop_back();
            uint64_t o0, o1;
            eval(c, n, get, -1, 0, 0, o0, o1);
            if (o0 == g0[n] && o1 == g1[n]) continue;
            set(n, o0, o1);
        }
        return det & cm;
    }
};

// Fault equivalence: a fault that is detected exactly when another is. Only
// the classes that hold for a conditioned (transition) fault as well as a
// stuck one are used when `transition` is set.
inline void collapse(const Circuit& c, std::vector<Fault>& faults, bool transition) {
    std::vector<int> par(faults.size());
    for (size_t i = 0; i < par.size(); i++) par[i] = (int)i;
    std::function<int(int)> find = [&](int x) {
        while (par[x] != x) x = par[x] = par[par[x]];
        return x;
    };
    // (node, pin+1, sa) -> fault
    std::vector<std::vector<std::pair<long, int>>> at(c.size());
    for (size_t i = 0; i < faults.size(); i++)
        if (faults[i].node >= 0)
            at[faults[i].node].push_back({(long)(faults[i].fi + 1) * 2 + faults[i].sa, (int)i});
    auto get = [&](int node, int fi, int sa) {
        for (auto& [k, v] : at[node])
            if (k == (long)(fi + 1) * 2 + sa) return v;
        return -1;
    };
    auto join = [&](int a, int b) {
        if (a < 0 || b < 0) return;
        if (faults[a].cond != faults[b].cond || faults[a].condv != faults[b].condv) return;
        a = find(a); b = find(b);
        if (a != b) par[std::max(a, b)] = std::min(a, b);
    };
    for (int n = 0; n < c.size(); n++) {
        int k = c.fiN[n];
        const int* a = c.in(n);
        for (int i = 0; i < k; i++) {
            // A stem with one branch: the branch fault is the stem fault.
            int drv = a[i];
            if (c.foStart[drv + 1] - c.foStart[drv] == 1 && !c.obs[drv])
                for (int v = 0; v < 2; v++) join(get(drv, -1, v), get(n, i, v));
            switch (c.type[n]) {
                case BUF: for (int v = 0; v < 2; v++) join(get(n, i, v), get(n, -1, v)); break;
                case NOT: for (int v = 0; v < 2; v++) join(get(n, i, v), get(n, -1, v ^ 1)); break;
                case AND: if (!transition) join(get(n, i, 0), get(n, -1, 0)); break;
                case NAND: if (!transition) join(get(n, i, 0), get(n, -1, 1)); break;
                case OR: if (!transition) join(get(n, i, 1), get(n, -1, 1)); break;
                case NOR: if (!transition) join(get(n, i, 1), get(n, -1, 0)); break;
                default: break;
            }
        }
    }
    for (size_t i = 0; i < faults.size(); i++) faults[i].rep = find((int)i);
}

}  // namespace atpg
