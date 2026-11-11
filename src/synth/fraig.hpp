// synth/fraig.hpp - functional reduction of the and-inverter graph.
//
// The elaborator shares logic that is built the same way (structural
// hashing). Logic that computes the same function another way is found
// here: nodes are simulated with random patterns, nodes with the same
// signature (up to inversion) are candidates, and a SAT call proves or
// refutes each pair. A proven node is replaced by the earlier one; a
// refuted pair gives a pattern that tells the two apart, which goes back
// into the simulation.
//
// The solver is the shared one (src/sat.hpp, CaDiCaL) through the
// equivalence checker's encoder (src/equiv/cnf.hpp). One thread: the
// result does not depend on the thread count.
#pragma once
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../aig.hpp"
#include "../equiv/cnf.hpp"

namespace synth {

struct FraigStats {
    size_t merged = 0, proved = 0, refuted = 0, undecided = 0;
};

// conflicts: the budget of one SAT call; tries: candidates checked per node.
inline aig::Design fraig(const aig::Design& in, FraigStats* stats = nullptr, int conflicts = 30, int tries = 3) {
    using namespace aig;
    const Graph& g = in.g;
    const uint32_t N = (uint32_t)g.size();
    // needs a clean graph: no wires, fanins before their node
    for (uint32_t n = 1; n < N; n++) {
        const Node& nd = g.nodes[n];
        if (nd.k == NK::Wire) return in;
        if (nd.k == NK::And && (lnode(nd.a) >= n || lnode(nd.b) >= n)) return in;
    }
    constexpr int W = 4;         // random words per node
    constexpr int kCexWords = 16;
    std::vector<std::array<uint64_t, W>> sig(N);
    std::vector<std::vector<uint64_t>> cex;   // words of refuting patterns, per word per node
    int cexBit = 64;                          // next free bit of the last word
    size_t cexWord = 0;
    bool cexFull = false;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    auto rnd = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    auto isLeaf = [&](uint32_t n) { return g.nodes[n].k == NK::Input || g.nodes[n].k == NK::Dff; };
    auto simWord = [&](std::vector<uint64_t>& w, uint32_t n) {
        const Node& nd = g.nodes[n];
        w[n] = (w[lnode(nd.a)] ^ (lisneg(nd.a) ? ~0ull : 0)) & (w[lnode(nd.b)] ^ (lisneg(nd.b) ? ~0ull : 0));
    };

    equiv::Aig snap;
    snap.graph = g;
    equiv::Cnf cnf(snap);
    std::vector<Lit> rep(N);
    std::unordered_map<uint64_t, std::vector<uint32_t>> classes;
    auto keyOf = [&](uint32_t n) {
        uint64_t flip = (sig[n][0] & 1) ? ~0ull : 0, h = 1469598103934665603ull;
        for (int k = 0; k < W; k++) h = (h ^ (sig[n][k] ^ flip)) * 1099511628211ull;
        return h;
    };
    FraigStats st;
    uint32_t done = 0;   // nodes below this have all their words
    for (uint32_t n = 0; n < N; n++) {
        const Node& nd = g.nodes[n];
        rep[n] = mklit(n);
        if (nd.k == NK::Const) sig[n] = {0, 0, 0, 0};
        else if (nd.k != NK::And) sig[n] = {rnd(), rnd(), rnd(), rnd()};
        else
            for (int k = 0; k < W; k++)
                sig[n][k] = (sig[lnode(nd.a)][k] ^ (lisneg(nd.a) ? ~0ull : 0)) & (sig[lnode(nd.b)][k] ^ (lisneg(nd.b) ? ~0ull : 0));
        for (auto& w : cex) {
            if (nd.k == NK::Const) w[n] = 0;
            else if (nd.k != NK::And) w[n] = rnd();
            else simWord(w, n);
        }
        done = n + 1;
        auto& cls = classes[keyOf(n)];
        if (nd.k != NK::And) {
            cls.push_back(n);
            continue;
        }
        bool nph = sig[n][0] & 1;
        int tried = 0;
        bool merged = false;
        for (size_t ci = 0; ci < cls.size() && !merged; ci++) {
            uint32_t c = cls[ci];
            bool ph = nph ^ (bool)(sig[c][0] & 1);   // n == c ^ ph on every pattern so far?
            bool same = true;
            for (int k = 0; k < W && same; k++) same = sig[n][k] == (sig[c][k] ^ (ph ? ~0ull : 0));
            for (size_t k = 0; k < cex.size() && same; k++) same = cex[k][n] == (cex[k][c] ^ (ph ? ~0ull : 0));
            if (!same) continue;
            if (tried++ >= tries) break;
            int a = cnf.lit(mklit(n)), b = cnf.lit(mklit(c, ph));
            sat::Result r = cnf.sat.solve({a, -b}, conflicts);
            if (r == sat::Unsat) r = cnf.sat.solve({-a, b}, conflicts);
            if (r == sat::Unsat) {
                cnf.sat.addClause({-a, b});
                cnf.sat.addClause({a, -b});
                rep[n] = mklit(c, ph);
                merged = true;
                st.proved++;
                continue;
            }
            if (r == sat::Unknown) {
                st.undecided++;
                continue;
            }
            st.refuted++;
            // the pattern that tells them apart joins the simulation
            if (cexBit == 64) {
                cexBit = 0;
                if (cex.size() < (size_t)kCexWords && !cexFull) {
                    cex.emplace_back(N, 0);
                    cexWord = cex.size() - 1;
                    auto& w = cex[cexWord];
                    for (uint32_t m = 1; m < done; m++)
                        if (isLeaf(m)) w[m] = rnd();
                } else {
                    cexFull = true;
                    cexWord = (cexWord + 1) % cex.size();
                }
            }
            auto& w = cex[cexWord];
            uint64_t bit = 1ull << cexBit++;
            for (uint32_t m = 1; m < done; m++) {
                if (isLeaf(m)) w[m] = cnf.model(m) ? (w[m] | bit) : (w[m] & ~bit);
                else simWord(w, m);
            }
        }
        if (merged) st.merged++;
        else cls.push_back(n);
    }
    if (stats) *stats = st;
    if (!st.merged) return in;
    Design d = in;
    for (uint32_t n = 1; n < N; n++)
        if (rep[n] != mklit(n)) {
            d.g.nodes[n].k = NK::Wire;
            d.g.nodes[n].a = rep[n];
        }
    return cleanup(d);
}

}  // namespace synth
