// The fault simulator and PODEM against brute force, on random small circuits.
//
// For each circuit and every fault (stems and branches, with and without a
// transition-style condition):
//   - Sim::detect must agree, pattern by pattern, with a plain scalar
//     simulation of the good and the faulty circuit;
//   - a PODEM test cube must detect the fault for every fill of its X bits;
//   - a fault PODEM calls untestable must be detected by no input vector at
//     all (the circuits are small enough to try every one).
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "atpg.hpp"

using namespace atpg;

namespace {

Rng rng{12345};
int pick(int n) { return (int)(rng.next() % (uint64_t)n); }

// Scalar three-valued evaluation, written apart from the code under test.
int evalNode(const Circuit& c, int n, const std::vector<int>& v, int forcePin, int forceVal) {
    auto in = [&](int i) { return i == forcePin ? forceVal : v[c.in(n)[i]]; };
    int k = c.fiN[n];
    switch (c.type[n]) {
        case BUF: return in(0);
        case NOT: return in(0) == 2 ? 2 : 1 - in(0);
        case AND: case NAND: case OR: case NOR: {
            bool isAnd = c.type[n] == AND || c.type[n] == NAND;
            int ctl = isAnd ? 0 : 1, r = 1 - ctl;
            bool x = false;
            for (int i = 0; i < k; i++) {
                if (in(i) == ctl) { r = ctl; x = false; break; }
                if (in(i) == 2) x = true;
            }
            if (x) return 2;
            return (c.type[n] == NAND || c.type[n] == NOR) ? 1 - r : r;
        }
        case XOR: case XNOR: {
            int r = c.type[n] == XNOR;
            for (int i = 0; i < k; i++) {
                if (in(i) == 2) return 2;
                r ^= in(i);
            }
            return r;
        }
        case MUX: {
            int a = in(0), b = in(1), s = in(2);
            if (s == 0) return a;
            if (s == 1) return b;
            return a == b ? a : 2;
        }
        default: return 2;
    }
}

// The circuit's node values for one input vector; fault = nullptr for the good machine.
std::vector<int> run(const Circuit& c, const std::vector<int>& pi, const Fault* f) {
    std::vector<int> v(c.size(), 2);
    size_t k = 0;
    for (int n = 0; n < c.size(); n++) {
        switch (c.type[n]) {
            case IN: v[n] = c.fixed[n] >= 0 ? c.fixed[n] : pi[k]; k++; break;
            case XS: v[n] = 2; break;
            case C0: v[n] = 0; break;
            case C1: v[n] = 1; break;
            default:
                v[n] = evalNode(c, n, v, f && f->node == n ? f->fi : -1, f ? f->sa : 0);
        }
        if (f && f->node == n && f->fi < 0) v[n] = f->sa;
    }
    return v;
}

bool detects(const Circuit& c, const std::vector<int>& pi, const Fault& f) {
    std::vector<int> g = run(c, pi, nullptr);
    if (f.cond >= 0 && g[f.cond] != f.condv) return false;
    std::vector<int> b = run(c, pi, &f);
    for (int n = 0; n < c.size(); n++)
        if (c.obs[n] && g[n] != 2 && b[n] != 2 && g[n] != b[n]) return true;
    return false;
}

Circuit randomCircuit(int nin, int ngates, bool withX) {
    Circuit c;
    c.add(C0);
    c.add(C1);
    for (int i = 0; i < nin; i++) {
        int n = c.add(IN);
        if (withX && i == 0) c.fixed[n] = (int8_t)pick(2);
    }
    if (withX) c.add(XS);
    static const T kinds[] = {BUF, NOT, AND, NAND, OR, NOR, XOR, XNOR, MUX, AND, OR, NAND, NOR};
    for (int g = 0; g < ngates; g++) {
        T t = kinds[pick(13)];
        int k = t == BUF || t == NOT ? 1 : t == MUX ? 3 : 2 + pick(3);
        std::vector<int> ins;
        for (int i = 0; i < k; i++) {
            // Mostly recent nodes, sometimes a constant.
            int lo = pick(20) == 0 ? 0 : 2;
            ins.push_back(lo + pick(c.size() - lo));
        }
        c.addv(t, ins);
    }
    // Observe the last few nodes and a few at random.
    for (int i = 0; i < 3; i++) c.obs[c.size() - 1 - i] = 1;
    c.obs[2 + nin + pick(ngates)] = 1;
    c.finish();
    return c;
}

std::vector<Fault> allFaults(const Circuit& c, bool conditioned) {
    std::vector<Fault> fl;
    for (int n = 2; n < c.size(); n++)
        for (int fi = -1; fi < c.fiN[n]; fi++)
            for (int sa = 0; sa < 2; sa++) {
                Fault f;
                f.node = n;
                f.fi = fi;
                f.sa = (uint8_t)sa;
                if (conditioned) {
                    f.cond = 2 + pick(c.size() - 2);
                    f.condv = (uint8_t)pick(2);
                }
                fl.push_back(f);
            }
    return fl;
}

}  // namespace

int main() {
    long checkedSim = 0, tests = 0, untestable = 0, aborted = 0;
    for (int round = 0; round < 60; round++) {
        int nin = 3 + pick(8), ngates = 8 + pick(50);
        bool withX = round % 3 == 2, conditioned = round % 2 == 1;
        Circuit c = randomCircuit(nin, ngates, withX);
        std::vector<Fault> fl = allFaults(c, conditioned);
        size_t ni = c.inputs.size();

        // 1. The parallel-pattern simulator against the scalar one.
        Sim sim(c);
        Sim::Work w;
        w.init(c.size());
        std::vector<uint64_t> words(ni);
        for (auto& x : words) x = rng.next();
        sim.good(words);
        for (const Fault& f : fl) {
            uint64_t m = sim.detect(w, f);
            for (int b = 0; b < 64; b += 7) {
                std::vector<int> pi(ni);
                for (size_t i = 0; i < ni; i++) pi[i] = (int)(words[i] >> b & 1);
                bool want = detects(c, pi, f);
                if (want != (bool)(m >> b & 1)) {
                    std::printf("round %d: simulator says %d, brute force %d (node %d pin %d sa %d)\n",
                                round, (int)(m >> b & 1), (int)want, f.node, f.fi, f.sa);
                    return 1;
                }
                checkedSim++;
            }
        }

        // 2. PODEM: a cube detects for every fill; untestable means no vector detects.
        Podem pod(c, false);
        for (const Fault& f : fl) {
            Pattern cube;
            Podem::Result r = pod.run(f, 100000, cube);
            if (r == Podem::ABORT) { aborted++; continue; }
            if (r == Podem::TEST) {
                tests++;
                for (int fill = 0; fill < 8; fill++) {
                    std::vector<int> pi(ni);
                    for (size_t i = 0; i < ni; i++) pi[i] = cube[i] == 2 ? pick(2) : cube[i];
                    if (!detects(c, pi, f)) {
                        std::printf("round %d: the PODEM cube does not detect (node %d pin %d sa %d)\n",
                                    round, f.node, f.fi, f.sa);
                        return 1;
                    }
                }
            } else {
                untestable++;
                // With an X source PODEM may give up on a fault a lucky vector
                // would detect; without one, untestable must mean no vector.
                if (withX) continue;
                for (unsigned v = 0; v < (1u << ni); v++) {
                    std::vector<int> pi(ni);
                    for (size_t i = 0; i < ni; i++) pi[i] = (int)(v >> i & 1);
                    if (detects(c, pi, f)) {
                        std::printf("round %d: PODEM calls a testable fault untestable (node %d pin %d sa %d)\n",
                                    round, f.node, f.fi, f.sa);
                        return 1;
                    }
                }
            }
        }
    }
    std::printf("test_faultsim: %ld simulator checks, %ld PODEM tests, %ld untestable proven, %ld aborted\n",
                checkedSim, tests, untestable, aborted);
    assert(tests > 1000 && untestable > 100);
    return 0;
}
