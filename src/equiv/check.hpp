#pragma once
// check.hpp - match two Designs and prove their compare points equal.
//
// Compare points are what Formality calls them: every primary output (and
// black-box input pin), and for every state element its next-state function
// and its trigger (clock and asynchronous controls). State elements are cut:
// the stored value Q of a matched pair is one shared free variable, so the
// whole check is combinational. That is sound; it is incomplete only where
// synthesis changed the state encoding (retiming, register merging), which
// shows up as unmatched or failing points rather than a wrong "equivalent".
//
// Matching is by name: exact, then normalized (hierarchy separators, bus
// brackets and the "_reg" suffix synthesis adds are ignored), then by the name
// of the cell instance that holds the state. -match adds pairs by hand.
//
// Proof, per point, cheapest first:
//   1. structural: both sides hash to the same AIG literal;
//   2. simulation: 64 random patterns per round find most real differences;
//   3. SAT on the miter, with a conflict budget.
// Points are independent, so step 3 runs on all cores; each point gets its
// own solver, which makes every answer and counterexample the same whatever
// the thread count.
#include <algorithm>
#include <set>
#include <unordered_set>
#include "../parallel.hpp"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "aig.hpp"
#include "bv.hpp"
#include "cnf.hpp"
#include "elab.hpp"

namespace equiv {

struct Point {
    enum Kind { Output, Next, Trigger } kind = Output;
    enum Status { Pass, Fail, Abort } status = Pass;
    std::string name;      // reference name
    std::string implName;  // implementation name, when it differs
    Bit ref, impl;
    Lit miter = kFalse;    // true exactly when the two sides differ
    const char* how = "";  // "structural", "simulation", "sat"
    uint64_t conflicts = 0;
    std::string note;
    std::vector<std::pair<std::string, bool>> cex;  // input values showing a failure
    int refValue = -1, implValue = -1;              // 0, 1, or 2 for X under the counterexample
};

struct CheckOptions {
    unsigned threads = 0;           // 0: all logical cores
    uint64_t effort = 2'000'000;    // SAT conflicts per compare point
    unsigned simRounds = 8;         // 64 patterns each
    uint64_t sweepEffort = 300;     // SAT conflicts per internal equivalence tried
    std::vector<std::pair<std::string, std::string>> matches;  // ref name, impl name
    uint64_t splitEffort = 20000;   // SAT conflicts per case of a case split
    unsigned splitDepth = 10;       // variables one case of a hard point may fix
    unsigned splitCases = 24;       // cases of one point that may go to the solver
    bool induction = true;          // prove failing points by induction over the reset state
    bool constRegs = true;          // read unmatched reference flops stuck at their reset value as that value
};

struct Result {
    std::vector<Point> points;
    std::vector<std::string> refOnlyInputs, implOnlyInputs, refOnlyOutputs, implOnlyOutputs;
    std::vector<std::string> refOnlyStates, implOnlyStates;
    size_t matchedInputs = 0, matchedStates = 0, matchedOutputs = 0, invertedStates = 0;
    size_t inductive = 0;          // points that hold in every state reachable after 'inductionReset'
    std::string inductionReset;    // e.g. rst_n=0
    std::vector<std::string> constStates;  // reference flops read as a constant (CheckOptions::constRegs)
    size_t passed = 0, failed = 0, aborted = 0;
    size_t merged = 0;  // internal nodes SAT sweeping proved equal
    bool equivalent() const { return failed == 0 && aborted == 0 && refOnlyOutputs.empty(); }
};

// u_core.cnt[3], u_core_cnt_reg_3_, \u_core/cnt_reg[3] -> u_core_cnt_3
inline std::string normalize(std::string_view s) {
    std::string r;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) r += c;
        else if (!r.empty() && r.back() != '_') r += '_';
    }
    while (!r.empty() && r.back() == '_') r.pop_back();
    for (size_t p = 0; (p = r.find("_reg", p)) != std::string::npos;) {
        if (p + 4 == r.size() || r[p + 4] == '_') r.erase(p, 4);
        else p += 4;
    }
    return r;
}

namespace detail {

// Pair up two name lists. Returns, for each impl index, the ref index or -1.
inline std::vector<int> matchNames(const std::vector<std::vector<std::string>>& refKeys,
                                   const std::vector<std::vector<std::string>>& implKeys,
                                   const std::vector<std::pair<int, int>>& forced) {
    std::vector<int> implToRef(implKeys.size(), -1);
    std::vector<char> refUsed(refKeys.size(), 0);
    for (auto [r, i] : forced)
        if (r >= 0 && i >= 0 && !refUsed[r] && implToRef[i] < 0) implToRef[i] = r, refUsed[r] = 1;
    size_t levels = 0;
    for (auto& k : refKeys) levels = std::max(levels, k.size());
    for (auto& k : implKeys) levels = std::max(levels, k.size());
    // Level by level: exact names first, looser keys later. A key that names
    // two unmatched signals on one side is ambiguous and matches nothing.
    for (size_t lr = 0; lr < levels; ++lr)
        for (size_t li = 0; li < levels; ++li) {
            std::unordered_map<std::string, int> table;
            for (size_t r = 0; r < refKeys.size(); ++r) {
                if (refUsed[r] || lr >= refKeys[r].size() || refKeys[r][lr].empty()) continue;
                auto [it, fresh] = table.try_emplace(refKeys[r][lr], int(r));
                if (!fresh) it->second = -1;
            }
            std::unordered_map<std::string, int> seen;
            for (size_t i = 0; i < implKeys.size(); ++i) {
                if (implToRef[i] >= 0 || li >= implKeys[i].size() || implKeys[i][li].empty()) continue;
                auto [it, fresh] = seen.try_emplace(implKeys[i][li], int(i));
                if (!fresh) it->second = -1;
            }
            for (auto& [key, i] : seen) {
                if (i < 0) continue;
                auto it = table.find(key);
                if (it == table.end() || it->second < 0 || refUsed[it->second]) continue;
                implToRef[i] = it->second;
                refUsed[it->second] = 1;
            }
        }
    return implToRef;
}

inline uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// SAT sweeping (fraiging) over the cones of `roots`: nodes that simulate alike
// are proved equal one by one, smallest first, each proof using the ones
// before it; then the roots are rebuilt on the merged graph. Returns the
// number of nodes merged. Single solver, fixed order: deterministic.
inline size_t sweep(Aig& g, const std::vector<Lit*>& roots, uint64_t effort,
                    const std::vector<std::pair<Lit, Lit>>& lemmas = {}) {
    const size_t N = g.size();
    const size_t K = 4;  // base signature words
    std::vector<char> in(N, 0);
    {
        std::vector<uint32_t> st;
        auto flood = [&] {
            while (!st.empty()) {
                uint32_t n = st.back();
                st.pop_back();
                if (n == 0 || in[n]) continue;
                in[n] = 1;
                if (g.isAnd(n)) st.push_back(nodeOf(g.node(n).a)), st.push_back(nodeOf(g.node(n).b));
            }
        };
        for (Lit* r : roots) st.push_back(nodeOf(*r));
        flood();
        // A lemma that touches the cone brings its other side in.
        for (bool grew = true; grew;) {
            grew = false;
            for (auto& [x, y] : lemmas)
                if (in[nodeOf(x)] != in[nodeOf(y)]) {
                    st.push_back(nodeOf(x)), st.push_back(nodeOf(y));
                    flood();
                    grew = true;
                }
        }
    }
    std::vector<uint32_t> cone;
    for (uint32_t n = 1; n < N; ++n) if (in[n]) cone.push_back(n);
    size_t maxWords = std::max<size_t>(K, std::min<size_t>(1024, (size_t(256) << 20) / (8 * (cone.size() + 1))));
    std::vector<std::vector<uint64_t>> sig(1, std::vector<uint64_t>(N, 0));  // sig[word][node]
    auto simWord = [&](size_t w) {
        auto& s = sig[w];
        for (uint32_t n : cone) {
            if (!g.isAnd(n)) continue;
            Aig::Node nd = g.node(n);
            s[n] = (isNeg(nd.a) ? ~s[nodeOf(nd.a)] : s[nodeOf(nd.a)]) & (isNeg(nd.b) ? ~s[nodeOf(nd.b)] : s[nodeOf(nd.b)]);
        }
    };
    sig.assign(K, std::vector<uint64_t>(N, 0));
    for (size_t w = 0; w < K; ++w) {
        for (uint32_t n : cone)
            if (!g.isAnd(n)) sig[w][n] = mix(uint64_t(n) * 0x9E3779B1ULL + w * 0x51ED27ULL + 77);
        simWord(w);
    }
    auto phaseOf = [&](uint32_t n) { return bool(sig[0][n] & 1); };
    auto keyOf = [&](uint32_t n) {
        uint64_t h = 0, flip = phaseOf(n) ? ~0ull : 0;
        for (size_t w = 0; w < K; ++w) h = mix(h ^ (sig[w][n] ^ flip));
        return h;
    };
    auto same = [&](uint32_t a, uint32_t b, bool inv) {
        uint64_t flip = inv ? ~0ull : 0;
        for (size_t w = 0; w < sig.size(); ++w)
            if (sig[w][a] != (sig[w][b] ^ flip)) return false;
        return true;
    };

    Cnf cnf(g);
    std::unordered_map<uint64_t, std::vector<uint32_t>> table;
    table[keyOf(0)].push_back(0);
    std::vector<Lit> rep(N, kNoLit);  // node -> an equal, earlier literal
    size_t merged = 0;
    for (auto& [x, y] : lemmas) {
        uint32_t nx = nodeOf(x), ny = nodeOf(y);
        if (nx == ny || !in[nx] || !in[ny]) continue;
        Lit late = nx > ny ? x : y, early = nx > ny ? y : x;
        if (!g.isAnd(nodeOf(late)) || rep[nodeOf(late)] != kNoLit) continue;
        rep[nodeOf(late)] = early ^ (late & 1u);
        int a = cnf.lit(x), b = cnf.lit(y);
        cnf.sat.addClause({-a, b});
        cnf.sat.addClause({a, -b});
    }
    for (uint32_t n : cone) {
        if (rep[n] != kNoLit) continue;  // settled by a lemma
        auto& bucket = table[keyOf(n)];
        if (!g.isAnd(n)) { bucket.push_back(n); continue; }
        bool done = false;
        int tries = 0;
        for (size_t c = 0; c < bucket.size() && !done && tries < 3; ++c) {
            uint32_t r = bucket[c];
            bool inv = phaseOf(n) != phaseOf(r);
            if (!same(n, r, inv)) continue;
            ++tries;
            // Is n != r possible?
            int a = cnf.lit(Lit(n << 1)), b = cnf.lit(Lit(r << 1) ^ (inv ? 1u : 0u));
            int t = cnf.sat.newVar();
            cnf.sat.addClause({-t, a, b});
            cnf.sat.addClause({-t, -a, -b});
            sat::Result res = cnf.sat.solve({t}, int64_t(effort));
            if (res == sat::Unsat) {
                cnf.sat.addClause({-t});
                cnf.sat.addClause({-a, b});
                cnf.sat.addClause({a, -b});
                rep[n] = Lit(r << 1) ^ (inv ? 1u : 0u);
                ++merged;
                done = true;
            } else if (res == sat::Sat) {
                // A pattern that tells them apart, and 63 neighbours of it.
                if (sig.size() < maxWords) {
                    sig.emplace_back(N, 0);
                    auto& s = sig.back();
                    size_t w = sig.size() - 1;
                    for (uint32_t m : cone) {
                        if (g.isAnd(m)) continue;
                        uint64_t r1 = mix(uint64_t(m) * 1315423911ULL + w), r2 = mix(r1), r3 = mix(r2);
                        s[m] = (cnf.model(m) ? ~0ull : 0) ^ (r1 & r2 & r3 & ~1ull);
                    }
                    simWord(w);
                }
                cnf.sat.addClause({-t});
            } else {
                cnf.sat.addClause({-t});
            }
        }
        if (!done) bucket.push_back(n);
    }
    if (!merged) return 0;
    // Rebuild the cone on the merged graph.
    std::vector<Lit> now(N, kNoLit);
    now[0] = kFalse;
    for (uint32_t n : cone) {
        if (!g.isAnd(n)) { now[n] = Lit(n << 1); continue; }
        if (rep[n] != kNoLit) { now[n] = now[nodeOf(rep[n])] ^ (rep[n] & 1u); continue; }
        Aig::Node nd = g.node(n);
        now[n] = g.and_(now[nodeOf(nd.a)] ^ (nd.a & 1u), now[nodeOf(nd.b)] ^ (nd.b & 1u));
    }
    for (Lit* r : roots) *r = now[nodeOf(*r)] ^ (*r & 1u);
    return merged;
}

}  // namespace detail

namespace detail {

// Everything that follows from literal l being true, as node values: an AND
// that is true has both inputs true, all the way down. (A select written as
// a && b && c fixes a, b and c when it is set, and an operand that was ANDed
// with it may have been folded into those.)
inline void assumeTrue(const Aig& g, Lit l, std::unordered_map<uint32_t, Lit>& fix) {
    std::vector<Lit> st{l};
    while (!st.empty() && fix.size() < 4096) {
        Lit x = st.back();
        st.pop_back();
        uint32_t n = nodeOf(x);
        if (!n || fix.count(n)) continue;
        fix[n] = isNeg(x) ? kFalse : kTrue;
        if (!isNeg(x) && g.isAnd(n)) st.push_back(g.node(n).a), st.push_back(g.node(n).b);
    }
}

// The variables that must have one value for 'root' to be true: the
// variable leaves of the AND tree at its top. A compare point's miter is
// (the register loads) & (the two sides differ), and what the load needs is
// often what makes the two sides the same logic.
inline size_t rootFacts(const Aig& g, Lit root, std::unordered_map<uint32_t, Lit>& fix) {
    size_t added = 0;
    std::vector<Lit> st{root};
    std::unordered_set<uint32_t> seen;
    while (!st.empty() && seen.size() < 4096) {
        Lit x = st.back();
        st.pop_back();
        uint32_t n = nodeOf(x);
        if (!n || !seen.insert(n).second) continue;
        if (g.isAnd(n)) {
            if (!isNeg(x)) st.push_back(g.node(n).a), st.push_back(g.node(n).b);
        } else if (fix.emplace(n, isNeg(x) ? kFalse : kTrue).second) {
            ++added;
        }
    }
    return added;
}

// The free variable in the cone of 'root' that most shrinks the cone when it
// is fixed: a hard miter is usually two datapaths that become the same logic
// once a select between them is decided (an isolated operand, a bypass).
inline uint32_t splitVar(Aig& g, Lit root, size_t slack) {
    // Candidates: the variables with the most fanout in the cone, and the few
    // variables behind the internal nodes with the most fanout (a select such
    // as !rst && en is one node that every bit of an operand goes through).
    std::unordered_map<uint32_t, uint32_t> fan;
    std::unordered_set<uint32_t> seen;
    std::vector<uint32_t> st{nodeOf(root)};
    while (!st.empty()) {
        uint32_t n = st.back();
        st.pop_back();
        if (n == 0 || !seen.insert(n).second) continue;
        if (!g.isAnd(n)) continue;
        for (Lit f : {g.node(n).a, g.node(n).b}) {
            if (nodeOf(f)) ++fan[nodeOf(f)];
            st.push_back(nodeOf(f));
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> vars, ands;  // fanout, node
    for (auto& [n, f] : fan) (g.isAnd(n) ? ands : vars).push_back({f, n});
    auto byFan = [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; };
    std::sort(vars.begin(), vars.end(), byFan);
    std::sort(ands.begin(), ands.end(), byFan);
    if (vars.size() > 8) vars.resize(8);
    if (ands.size() > 8) ands.resize(8);
    std::vector<uint32_t> cand;
    auto addCand = [&](uint32_t n) { if (std::find(cand.begin(), cand.end(), n) == cand.end()) cand.push_back(n); };
    for (auto& [f, n] : vars) addCand(n);
    for (auto& [f, n] : ands) {
        std::vector<uint32_t> sup, todo{n};
        std::unordered_set<uint32_t> in;
        while (!todo.empty() && sup.size() <= 4 && in.size() < 64) {
            uint32_t m = todo.back();
            todo.pop_back();
            if (m == 0 || !in.insert(m).second) continue;
            if (!g.isAnd(m)) { sup.push_back(m); continue; }
            todo.push_back(nodeOf(g.node(m).a));
            todo.push_back(nodeOf(g.node(m).b));
        }
        if (todo.empty() && sup.size() <= 4) for (uint32_t v : sup) addCand(v);
    }
    // And those nodes themselves: a select with many variables behind it is
    // still one signal that is either set or not.
    for (auto& [f, n] : ands) addCand(n);
    auto coneSize = [&](Lit r) {
        std::unordered_set<uint32_t> in;
        std::vector<uint32_t> s2{nodeOf(r)};
        while (!s2.empty()) {
            uint32_t n = s2.back();
            s2.pop_back();
            if (n == 0 || !g.isAnd(n) || !in.insert(n).second) continue;
            s2.push_back(nodeOf(g.node(n).a));
            s2.push_back(nodeOf(g.node(n).b));
        }
        return in.size();
    };
    uint32_t best = 0;
    // Worth it only when the two cases together are under slack/10 of the cone
    // (20: any gain at all; less: only a variable that removes real work).
    size_t bestSize = coneSize(root) * slack / 10;
    for (uint32_t n : cand) {
        size_t sz = 0;
        for (bool v : {false, true}) {
            std::unordered_map<uint32_t, Lit> m;
            assumeTrue(g, Lit(n * 2 + (v ? 0 : 1)), m);
            sz += coneSize(g.cofactor(root, m));
        }
        if (sz < bestSize) bestSize = sz, best = n;
    }
    return best;
}

// One pass with a given polarity for every matched register: `flips` names
// the implementation states to take with the opposite polarity from what the
// names suggest.
inline Result checkOnce(Aig& g, const Design& ref, Design& impl, const CheckOptions& opt,
                        const std::set<std::string>& flips) {
    Result res;
    using Keys = std::vector<std::vector<std::string>>;
    auto forcedPairs = [&](auto& rlist, auto& ilist, auto nameOf) {
        std::vector<std::pair<int, int>> out;
        for (auto& [rn, in] : opt.matches) {
            int r = -1, i = -1;
            for (size_t k = 0; k < rlist.size(); ++k) if (nameOf(rlist[k]) == rn) r = int(k);
            for (size_t k = 0; k < ilist.size(); ++k) if (nameOf(ilist[k]) == in) i = int(k);
            if (r >= 0 && i >= 0) out.push_back({r, i});
        }
        return out;
    };
    auto pinName = [](const auto& p) -> const std::string& { return p.first; };
    auto stName = [](const State& s) -> const std::string& { return s.name; };

    // ---- match inputs
    Keys rk, ik;
    for (auto& [n, l] : ref.inputs) rk.push_back({n, normalize(n)});
    for (auto& [n, l] : impl.inputs) ik.push_back({n, normalize(n)});
    std::vector<int> inMap = detail::matchNames(rk, ik, forcedPairs(ref.inputs, impl.inputs, pinName));

    // ---- match state elements
    auto stateKeys = [](const std::vector<State>& v) {
        std::unordered_map<std::string, int> perInst;
        for (auto& s : v) ++perInst[s.inst];
        Keys k;
        for (auto& s : v) {
            std::string byInst = !s.inst.empty() && perInst[s.inst] == 1 ? normalize(s.inst) : std::string();
            k.push_back({s.name, normalize(s.name), byInst});
        }
        return k;
    };
    std::vector<int> stMap = detail::matchNames(stateKeys(ref.states), stateKeys(impl.states),
                                                forcedPairs(ref.states, impl.states, stName));
    // Still unmatched: by the nets that carry Q (a flop cell is known by the
    // net on its output pin), then by nets that carry !Q: synthesis stores a
    // register inverted when the library's reset polarity suits it better. A
    // name must point at one state per side.
    std::vector<char> stInv(impl.states.size(), 0);
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<char> refUsed(ref.states.size(), 0);
        for (int r : stMap) if (r >= 0) refUsed[r] = 1;
        // A cell that holds one state is also known by its instance name.
        auto instCount = [](const std::vector<State>& v) {
            std::unordered_map<std::string, int> n;
            for (auto& s : v) ++n[s.inst];
            return n;
        };
        auto refInst = instCount(ref.states), implInst = instCount(impl.states);
        auto keysOf = [](const State& s, bool inv, std::unordered_map<std::string, int>& perInst) {
            std::vector<std::string> k;
            if (!inv) {
                k.push_back(normalize(s.name));
                if (!s.inst.empty() && perInst[s.inst] == 1) k.push_back(normalize(s.inst));
            }
            for (auto& a : inv ? s.invAliases : s.aliases) k.push_back(normalize(a));
            return k;
        };
        std::unordered_map<std::string, int> table;
        for (size_t r = 0; r < ref.states.size(); ++r) {
            if (refUsed[r]) continue;
            for (auto& k : keysOf(ref.states[r], false, refInst)) {
                auto [it, fresh] = table.try_emplace(k, int(r));
                if (!fresh && it->second != int(r)) it->second = -1;
            }
        }
        std::unordered_map<int, int> claim;  // ref -> impl, -1 when two impl states want it
        std::vector<int> want(impl.states.size(), -1);
        for (size_t i = 0; i < impl.states.size(); ++i) {
            if (stMap[i] >= 0) continue;
            int found = -1;
            for (auto& k : keysOf(impl.states[i], pass == 1, implInst)) {
                auto it = table.find(k);
                if (it == table.end() || it->second < 0) continue;
                if (found >= 0 && found != it->second) { found = -2; break; }
                found = it->second;
            }
            if (found < 0) continue;
            want[i] = found;
            auto [it, fresh] = claim.try_emplace(found, int(i));
            if (!fresh) it->second = -1;
        }
        for (size_t i = 0; i < impl.states.size(); ++i)
            if (want[i] >= 0 && claim[want[i]] == int(i)) stMap[i] = want[i], stInv[i] = pass == 1;
    }
    // A register matched by its cell's name may still be stored inverted: a
    // library with only a clear-type flop holds a set-type register as its
    // complement. The nets tell: the reference's name is on a net that
    // carries !Q and on none that carries Q.
    for (size_t i = 0; i < impl.states.size(); ++i) {
        if (stMap[i] < 0 || stInv[i]) continue;
        const State& is = impl.states[i];
        const State& rs = ref.states[stMap[i]];
        std::vector<std::string> names{normalize(rs.name)};
        for (auto& a : rs.aliases) names.push_back(normalize(a));
        auto carries = [&](const std::vector<std::string>& nets) {
            for (auto& a : nets)
                if (std::find(names.begin(), names.end(), normalize(a)) != names.end()) return true;
            return false;
        };
        if (carries(is.invAliases) && !carries(is.aliases) &&
            std::find(names.begin(), names.end(), normalize(is.name)) == names.end())
            stInv[i] = 1;
    }
    for (size_t i = 0; i < impl.states.size(); ++i)
        if (stMap[i] >= 0 && flips.count(impl.states[i].name)) stInv[i] = !stInv[i];

    // ---- match outputs
    rk.clear(), ik.clear();
    for (auto& [n, b] : ref.outputs) rk.push_back({n, normalize(n)});
    for (auto& [n, b] : impl.outputs) ik.push_back({n, normalize(n)});
    std::vector<int> outMap = detail::matchNames(rk, ik, forcedPairs(ref.outputs, impl.outputs, pinName));

    // ---- put the implementation on the reference's inputs
    std::vector<Lit> memo(g.size(), kNoLit);
    std::vector<char> refIn(ref.inputs.size(), 0), refSt(ref.states.size(), 0), refOut(ref.outputs.size(), 0);
    for (size_t i = 0; i < impl.inputs.size(); ++i) {
        if (inMap[i] < 0) { res.implOnlyInputs.push_back(impl.inputs[i].first); continue; }
        memo[nodeOf(impl.inputs[i].second)] = ref.inputs[inMap[i]].second;
        refIn[inMap[i]] = 1;
        ++res.matchedInputs;
    }
    for (size_t i = 0; i < impl.states.size(); ++i) {
        if (stMap[i] < 0) { res.implOnlyStates.push_back(impl.states[i].name); continue; }
        memo[nodeOf(impl.states[i].q)] = stInv[i] ? neg(ref.states[stMap[i]].q) : ref.states[stMap[i]].q;
        refSt[stMap[i]] = 1;
        ++res.matchedStates;
    }
    for (size_t r = 0; r < ref.inputs.size(); ++r) if (!refIn[r]) res.refOnlyInputs.push_back(ref.inputs[r].first);
    // ---- constant registers
    // A reference flop the implementation does not have, with an asynchronous
    // set or reset to c and a next state that is c whenever it holds c, never
    // leaves c once reset. Synthesis replaces it with c; read it that way.
    // This takes the reset as applied, which is what such a netlist assumes.
    // The same goes for a flop that only ever loads one constant (taken as
    // clocked once) and one that is never given a definite value at all.
    std::unordered_map<uint32_t, Lit> konst;
    auto orTerms = [&](Lit root) {
        std::vector<Lit> out, todo{root};
        while (!todo.empty()) {
            Lit l = todo.back();
            todo.pop_back();
            if (isNeg(l) && g.isAnd(nodeOf(l))) {
                todo.push_back(neg(g.node(nodeOf(l)).a));
                todo.push_back(neg(g.node(nodeOf(l)).b));
            } else {
                out.push_back(l);
            }
        }
        return out;
    };
    if (opt.constRegs) {
        // The value the flop takes under 'term', if that is a constant.
        auto under = [&](const State& st, Lit term, std::unordered_map<uint32_t, Lit> m) -> Lit {
            if (nodeOf(term) == 0) return kNoLit;
            m[nodeOf(term)] = isNeg(term) ? kFalse : kTrue;
            auto mx = m;
            Lit v = g.rebuildSparse(st.next.v, m);
            if (g.rebuildSparse(st.next.x, mx) != kFalse || nodeOf(v) != 0) return kNoLit;
            return v;
        };
        for (bool again = true; again;) {
            again = false;
            for (size_t r = 0; r < ref.states.size(); ++r) {
                const State& st = ref.states[r];
                if (refSt[r] || st.latch || st.trig.x != kFalse || konst.count(nodeOf(st.q))) continue;
                // No definite value other than c is ever stored: every load is c
                // (or X), whatever the flop holds. With both c, nothing definite is
                // ever stored and the flop is unknown for good; synthesis ties it
                // to 0 and any fixed value is a legitimate reading of it.
                {
                    auto m = konst;
                    Lit v = g.rebuildSparse(st.next.v, m), x = g.rebuildSparse(st.next.x, m);
                    bool is0 = g.or_(x, neg(v)) == kTrue, is1 = g.or_(x, v) == kTrue;
                    if (!is0 && !is1) {
                        // Or it only ever keeps what it has.
                        bool keep[2];
                        for (int k = 0; k < 2; ++k) {
                            auto mk = konst;
                            mk[nodeOf(st.q)] = k ? kTrue : kFalse;
                            Lit vk = g.rebuildSparse(st.next.v, mk), xk = g.rebuildSparse(st.next.x, mk);
                            keep[k] = g.or_(xk, k ? vk : neg(vk)) == kTrue;
                        }
                        is0 = keep[0] && keep[1];
                    }
                    if (is0 || is1) {
                        konst[nodeOf(st.q)] = is0 ? kFalse : kTrue;
                        res.constStates.push_back(st.name);
                        again = true;
                        continue;
                    }
                }
                auto terms = orTerms(st.trig.v);
                if (terms.size() < 2) continue;
                Lit c = kNoLit;
                size_t clocks = 0;  // events that load something else
                for (Lit t : terms) {
                    Lit v = under(st, t, konst);
                    if (v == kNoLit) ++clocks;
                    else if (c == kNoLit) c = v;
                    else if (c != v) { c = kNoLit; clocks = 0; break; }  // both a set and a reset
                }
                if (c == kNoLit || clocks == 0) continue;
                auto m = konst;
                m[nodeOf(st.q)] = isNeg(st.q) ? neg(c) : c;
                auto mx = m;
                if (g.rebuildSparse(st.next.v, m) != c || g.rebuildSparse(st.next.x, mx) != kFalse) continue;
                konst[nodeOf(st.q)] = isNeg(st.q) ? neg(c) : c;
                res.constStates.push_back(st.name);
                again = true;
            }
        }
    }
    for (size_t r = 0; r < ref.states.size(); ++r)
        if (!refSt[r] && !konst.count(nodeOf(ref.states[r].q))) res.refOnlyStates.push_back(ref.states[r].name);
    auto move = [&](Bit b) { return Bit{g.rebuild(b.v, memo), g.rebuild(b.x, memo)}; };
    auto kmemo = konst;
    auto fold = [&](Bit b) {
        if (konst.empty()) return b;
        return Bit{g.rebuildSparse(b.v, kmemo), g.rebuildSparse(b.x, kmemo)};
    };

    // ---- compare points
    auto add = [&](Point::Kind kind, const std::string& rn, const std::string& in, Bit r, Bit i) {
        Point p;
        p.kind = kind;
        p.name = rn;
        if (in != rn) p.implName = in;
        r = fold(r);
        p.ref = r;
        p.impl = i;
        // Differ: the reference is a definite value and the implementation is
        // not that value. A reference X accepts anything.
        p.miter = g.and_(neg(r.x), g.or_(g.xor_(r.v, i.v), i.x));
        res.points.push_back(std::move(p));
    };
    for (size_t i = 0; i < impl.outputs.size(); ++i) {
        if (outMap[i] < 0) { res.implOnlyOutputs.push_back(impl.outputs[i].first); continue; }
        refOut[outMap[i]] = 1;
        ++res.matchedOutputs;
        add(Point::Output, ref.outputs[outMap[i]].first, impl.outputs[i].first, ref.outputs[outMap[i]].second,
            move(impl.outputs[i].second));
    }
    for (size_t r = 0; r < ref.outputs.size(); ++r) if (!refOut[r]) res.refOnlyOutputs.push_back(ref.outputs[r].first);
    for (size_t i = 0; i < impl.states.size(); ++i) {
        if (stMap[i] < 0) continue;
        const State& rs = ref.states[stMap[i]];
        const State& is = impl.states[i];
        if (rs.latch != is.latch) {
            Point p;
            p.kind = Point::Next;
            p.name = rs.name;
            p.implName = is.name;
            p.status = Point::Fail;
            p.note = rs.latch ? "a latch in the reference, a flop in the implementation"
                              : "a flop in the reference, a latch in the implementation";
            p.miter = kTrue;
            res.points.push_back(std::move(p));
            continue;
        }
        Bit inext = move(is.next);
        if (stInv[i]) inext.v = neg(inext.v), ++res.invertedStates;
        add(Point::Next, rs.name, is.name, rs.next, inext);
        if (!rs.latch) add(Point::Trigger, rs.name, is.name, rs.trig, move(is.trig));
    }

    // ---- names of the free variables, for counterexamples
    std::unordered_map<uint32_t, std::string> varName;
    for (auto& [n, l] : impl.inputs) varName[nodeOf(l)] = n;
    for (auto& s : impl.states) varName[nodeOf(s.q)] = s.name;
    for (auto& [n, l] : ref.inputs) varName[nodeOf(l)] = n;
    for (auto& s : ref.states) varName[nodeOf(s.q)] = s.name;

    // ---- simulation
    size_t N = g.size();
    std::vector<uint64_t> sim(N);
    std::vector<char> open(res.points.size(), 0);
    size_t nOpen = 0;
    for (size_t k = 0; k < res.points.size(); ++k) {
        Point& p = res.points[k];
        if (p.status == Point::Fail) continue;
        if (p.miter == kFalse) { p.how = "structural"; continue; }
        open[k] = 1;
        ++nOpen;
    }
    auto word = [&](Lit l) { return isNeg(l) ? ~sim[nodeOf(l)] : sim[nodeOf(l)]; };
    // Evaluate one literal under an assignment to the inputs in its cone.
    auto finishCex = [&](Point& p, const std::function<bool(uint32_t)>& value) {
        std::unordered_map<uint32_t, bool> val;
        std::vector<uint32_t> st;
        auto eval = [&](Lit root) {
            st.assign(1, nodeOf(root));
            while (!st.empty()) {
                uint32_t n = st.back();
                if (val.count(n)) { st.pop_back(); continue; }
                if (n == 0) { val[n] = false; st.pop_back(); continue; }
                if (!g.isAnd(n)) {
                    bool v = value(n);
                    val[n] = v;
                    auto it = varName.find(n);
                    if (it != varName.end()) p.cex.push_back({it->second, v});
                    st.pop_back();
                    continue;
                }
                Aig::Node nd = g.node(n);
                auto ia = val.find(nodeOf(nd.a)), ib = val.find(nodeOf(nd.b));
                if (ia == val.end() || ib == val.end()) {
                    if (ia == val.end()) st.push_back(nodeOf(nd.a));
                    if (ib == val.end()) st.push_back(nodeOf(nd.b));
                    continue;
                }
                val[n] = (ia->second ^ isNeg(nd.a)) && (ib->second ^ isNeg(nd.b));
                st.pop_back();
            }
            return val[nodeOf(root)] ^ isNeg(root);
        };
        auto side = [&](Bit b) { return eval(b.x) ? 2 : int(eval(b.v)); };
        p.refValue = side(p.ref);
        p.implValue = side(p.impl);
        std::sort(p.cex.begin(), p.cex.end());
    };
    for (unsigned round = 0; round < opt.simRounds && nOpen; ++round) {
        sim[0] = 0;
        for (uint32_t n = 1; n < N; ++n) {
            Aig::Node nd = g.node(n);
            if (!g.isAnd(n)) {
                // Round 0 and 1 are all-zeros and all-ones in pattern 0, random after.
                uint64_t r = detail::mix(uint64_t(n) * 0x100000001b3ULL + round);
                if (round == 0) r &= ~1ull;
                if (round == 1) r |= 1ull;
                sim[n] = r;
            } else {
                sim[n] = word(nd.a) & word(nd.b);
            }
        }
        for (size_t k = 0; k < res.points.size(); ++k) {
            if (!open[k]) continue;
            Point& p = res.points[k];
            uint64_t diff = word(p.miter);
            if (!diff) continue;
            int bit = __builtin_ctzll(diff);
            p.status = Point::Fail;
            p.how = "simulation";
            finishCex(p, [&](uint32_t n) { return bool(sim[n] >> bit & 1); });
            open[k] = 0;
            --nOpen;
        }
    }
    sim.clear();
    sim.shrink_to_fit();

    // ---- SAT, in parallel: a quick pass, then (after sweeping) the full effort
    unsigned nt0 = opt.threads ? opt.threads : par::defaultThreads();
    auto satPass = [&](uint64_t budget, bool last, const char* how) {
        std::vector<size_t> work;
        for (size_t k = 0; k < res.points.size(); ++k) if (open[k]) work.push_back(k);
        par::parallelFor(work.size(), nt0, [&](size_t j) {
            Point& p = res.points[work[j]];
            Cnf cnf(g);
            int r = cnf.solve(p.miter, budget);
            p.conflicts += cnf.sat.conflicts();
            if (r < 0) {
                if (last) p.status = Point::Abort, p.how = how;
                return;
            }
            p.how = how;
            open[work[j]] = 0;
            if (r == 0) p.status = Point::Pass;
            else {
                p.status = Point::Fail;
                finishCex(p, [&](uint32_t n) { return cnf.model(n); });
            }
        });
    };
    size_t splitSlack = 13;
    unsigned splitCalls = 0;
    // split(p, m, when, depth, hint): can m be true while 'when' holds?
    // 0 no, 1 yes (p gets the counterexample), -1 not settled.
    std::function<int(Point&, Lit, Lit, unsigned, uint32_t)> split =
        [&](Point& p, Lit m, Lit when, unsigned depth, uint32_t hint) -> int {
        if (m == kFalse || when == kFalse) return 0;
        // What m needs of single variables, put back into m.
        for (int round = 0; round < 8; ++round) {
            std::unordered_map<uint32_t, Lit> facts;
            if (!detail::rootFacts(g, m, facts)) break;
            Lit all = kTrue;
            for (auto& [n, v] : facts) all = g.and_(all, Lit(n * 2) ^ (v == kTrue ? 0u : 1u));
            Lit m2 = g.cofactor(m, facts);
            when = g.and_(when, all);
            if (m2 == kFalse || when == kFalse) return 0;
            bool same = m2 == kTrue;
            m = m2;
            if (same) break;
        }
        // A case that fixing one more variable would shrink is split again
        // (one side of such a split usually vanishes, so this is a chain, not
        // a tree); a case that would not is the solver's.
        uint32_t v = depth ? (hint ? hint : detail::splitVar(g, m, splitSlack)) : 0;
        if (when != kTrue) {
            Lit q = g.and_(when, m);
            if (q == kFalse) return 0;
            if (splitCalls++ >= opt.splitCases) return -1;
            if (!v) {
                // The two sides are now the same function built two ways (one
                // was built knowing what this case fixes, the other was not):
                // their internal nodes pair up, which is what sweeping finds.
                std::vector<Lit*> roots{&q};
                res.merged += detail::sweep(g, roots, opt.sweepEffort, {});
                if (q == kFalse) return 0;
            }
            Cnf cnf(g);
            int r = cnf.solve(q, v ? std::min<uint64_t>(opt.splitEffort, 300) : opt.splitEffort);
            p.conflicts += cnf.sat.conflicts();
            if (r == 1) finishCex(p, [&](uint32_t n) { return cnf.model(n); });
            if (r >= 0) return r;
        }
        if (!v) return -1;
        int all = 0;
        for (bool b : {false, true}) {
            // v may be a signal inside the cone, not only a variable: m with v
            // replaced by b is what m is whenever v is b.
            std::unordered_map<uint32_t, Lit> fix;
            detail::assumeTrue(g, Lit(v * 2 + (b ? 0 : 1)), fix);
            int r = split(p, g.cofactor(m, fix), g.and_(when, Lit(v * 2 + (b ? 0 : 1))), depth - 1, 0);
            if (r == 1) return 1;
            if (r < 0) all = -1;
        }
        return all;
    };
    uint64_t quick = std::min<uint64_t>(opt.effort, 2000);
    satPass(quick, quick == opt.effort, "sat");
    bool anyOpen = false;
    for (char o : open) anyOpen |= bool(o);
    if (anyOpen && quick < opt.effort) {
        // Open points tend to be the bits of one bus: when a few in a row have
        // no variable worth splitting on, the rest will not either.
        unsigned dry = 0;
        for (size_t k = 0; k < res.points.size() && opt.splitDepth && dry < 8; ++k) {
            if (!open[k]) continue;
            Point& p = res.points[k];
            splitCalls = 0;
            int r = split(p, p.miter, kTrue, opt.splitDepth, 0);
            dry = r < 0 ? dry + 1 : 0;
            if (r < 0) continue;
            open[k] = 0;
            p.how = "case split";
            p.status = r == 0 ? Point::Pass : Point::Fail;
        }
        anyOpen = false;
        for (char o : open) anyOpen |= bool(o);
    }
    if (anyOpen && quick < opt.effort) {
        // The hard points are datapaths built differently on the two sides.
        // Find internal nodes that are equal, merge them, and try again.
        std::vector<Lit*> roots;
        for (size_t k = 0; k < res.points.size(); ++k) if (open[k]) roots.push_back(&res.points[k].miter);
        std::vector<std::pair<Lit, Lit>> lemmas = ref.lemmas;
        for (auto& [x, y] : impl.lemmas) lemmas.push_back({g.rebuild(x, memo), g.rebuild(y, memo)});
        res.merged = detail::sweep(g, roots, opt.sweepEffort, lemmas);
        for (size_t k = 0; k < res.points.size(); ++k)
            if (open[k] && res.points[k].miter == kFalse) {
                open[k] = 0;
                res.points[k].status = Point::Pass;
                res.points[k].how = "sweep";
            }
        satPass(opt.effort, true, "sweep+sat");
    }

    // ---- induction
    // A point that fails for some state may still hold in every state the
    // design can reach: the implementation has state of its own (a flop that
    // remembers that nothing changed, and gates a clock with it) and the
    // failing states are ones where that flop is wrong about the rest. Take
    // "no failing point fails" as the invariant J(state, inputs) and show
    //   step: J(s, in) and J(s, in') imply J(s', in') for s' after any one
    //         event (a clock edge, an asynchronous set or reset) with inputs in;
    //   base: J holds, from any state, after one event with some input held
    //         at one value: the reset.
    // Matched registers are one variable, moved by the implementation's
    // next-state function; the points that pass say that is the reference's
    // too. Events are taken one at a time.
    [&] {
        if (!opt.induction || impl.states.size() == res.matchedStates) return;
        std::vector<size_t> F;
        for (size_t k = 0; k < res.points.size(); ++k) {
            const Point& p = res.points[k];
            if (p.status == Point::Pass) continue;
            // A point that ran out of effort joins the invariant: it may be
            // provable in the reachable states, and nothing is lost by trying.
            if (!p.note.empty() || p.kind == Point::Trigger) return;
            F.push_back(k);
        }
        if (F.empty()) return;
        struct Flop {
            uint32_t var;
            Bit next;
            std::vector<Lit> terms;
        };
        std::vector<Flop> flops;
        std::unordered_set<uint32_t> stateVars;
        for (size_t i = 0; i < impl.states.size(); ++i) {
            const State& is = impl.states[i];
            uint32_t var = stMap[i] >= 0 ? nodeOf(ref.states[stMap[i]].q) : nodeOf(is.q);
            stateVars.insert(var);
            if (is.latch) continue;  // left free
            Bit nx = move(is.next), tr = move(is.trig);
            if (stMap[i] >= 0 && stInv[i]) nx.v = neg(nx.v);
            if (nx.x != kFalse || tr.x != kFalse) continue;
            flops.push_back({var, nx, orTerms(tr.v)});
        }
        for (size_t r = 0; r < ref.states.size(); ++r) {
            const State& rs = ref.states[r];
            if (refSt[r] || konst.count(nodeOf(rs.q))) continue;
            stateVars.insert(nodeOf(rs.q));
            Bit nx = fold(rs.next), tr = fold(rs.trig);
            if (rs.latch || nx.x != kFalse || tr.x != kFalse) continue;
            flops.push_back({nodeOf(rs.q), nx, orTerms(tr.v)});
        }
        std::vector<Lit> events;
        for (auto& f : flops)
            for (Lit t : f.terms)
                if (std::find(events.begin(), events.end(), t) == events.end()) events.push_back(t);
        if (events.empty() || events.size() > 16) return;
        std::unordered_map<uint32_t, Lit> fresh;
        std::unordered_set<uint32_t> flopVars;
        for (auto& f : flops) flopVars.insert(f.var);
        // The reset after which the points F hold for good, or "".
        auto prove = [&](const std::vector<size_t>& F) -> std::string {
        Lit M = kFalse;
        for (size_t k : F) M = g.or_(M, res.points[k].miter);
        // Fresh inputs for the step after: every free variable that is not a flop.
        auto freshen = [&](Lit root) {
            std::vector<uint32_t> st{nodeOf(root)};
            std::unordered_set<uint32_t> seen;
            while (!st.empty()) {
                uint32_t n = st.back();
                st.pop_back();
                if (!n || !seen.insert(n).second) continue;
                if (g.isAnd(n)) { st.push_back(nodeOf(g.node(n).a)); st.push_back(nodeOf(g.node(n).b)); continue; }
                bool isFlop = false;
                for (auto& f : flops) isFlop |= f.var == n;
                if (!isFlop && !fresh.count(n)) fresh[n] = g.input();
            }
        };
        freshen(M);
        // The state after event 'ev' (with 'forced' also held), as a substitution.
        auto after = [&](Lit ev, Lit forced) {
            std::unordered_map<uint32_t, Lit> fix;
            for (Lit l : {ev, forced})
                if (nodeOf(l) && !g.isAnd(nodeOf(l)) && !flopVars.count(nodeOf(l))) fix[nodeOf(l)] = isNeg(l) ? kFalse : kTrue;
            std::unordered_map<uint32_t, Lit> sub = fresh;
            for (auto& f : flops) {
                if (std::find(f.terms.begin(), f.terms.end(), ev) == f.terms.end()) continue;
                auto m = fix;
                sub[f.var] = g.cofactor(f.next.v, m);
            }
            return std::pair{sub, fix};
        };
        bool unsure = false;  // some query ran out of effort
        auto holds = [&](Lit bad) {  // 'bad' cannot be true
            if (bad == kFalse) return true;
            Cnf cnf(g);
            int r = cnf.solve(bad, opt.splitEffort);
            if (r >= 0) return r == 0;
            Point scratch;
            splitSlack = 20;  // one query decides many points: any split that helps
            splitCalls = 0;
            r = split(scratch, bad, kTrue, opt.splitDepth, 0);
            if (r < 0) {
                Cnf full(g);
                r = full.solve(bad, opt.effort);
            }
            if (r < 0) unsure = true;
            return r == 0;
        };
        auto Jnext = fresh;
        Lit hypNext = neg(g.cofactor(M, Jnext));  // J(s, in')
        for (Lit ev : events) {
            auto [sub, fix] = after(ev, kFalse);
            Lit hyp = g.and_(neg(g.cofactor(M, fix)), hypNext);
            if (!holds(g.and_(hyp, g.cofactor(M, sub)))) return unsure ? "?" : "";
        }
        // Base: an asynchronous event first, then an input held through a clock edge.
        auto name = [&](Lit l) {
            auto it = varName.find(nodeOf(l));
            return (it == varName.end() ? std::string("?") : it->second) + (isNeg(l) ? "=0" : "=1");
        };
        std::string reset;
        for (Lit ev : events) {
            if (!reset.empty()) break;
            if (g.isAnd(nodeOf(ev)) || flopVars.count(nodeOf(ev))) continue;
            auto [sub, fix] = after(ev, kFalse);
            if (holds(g.cofactor(M, sub))) reset = name(ev);
        }
        if (reset.empty()) {
            std::vector<uint32_t> ins;
            for (auto& [n, l] : fresh) if (!stateVars.count(n)) ins.push_back(n);
            std::sort(ins.begin(), ins.end());
            if (ins.size() > 64) ins.resize(64);
            for (Lit ev : events)
                for (uint32_t n : ins)
                    for (Lit l : {Lit(n * 2 + 1), Lit(n * 2)}) {
                        if (!reset.empty() || nodeOf(ev) == n) continue;
                        auto [sub, fix] = after(ev, l);
                        if (holds(g.cofactor(M, sub))) reset = name(l);
                    }
        }
        return reset.empty() && unsure ? std::string("?") : reset;
        };
        // Points that share implementation-only state share an invariant; the
        // rest are proved apart, each from its own reset. If that leaves any,
        // try them all as one.
        std::unordered_set<uint32_t> own;
        for (uint32_t v : stateVars) own.insert(v);
        for (size_t i = 0; i < impl.states.size(); ++i)
            if (stMap[i] >= 0) own.erase(nodeOf(ref.states[stMap[i]].q));
        std::map<uint32_t, uint32_t> parent;
        std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
            auto it = parent.find(x);
            if (it == parent.end() || it->second == x) return x;
            return it->second = find(it->second);
        };
        std::vector<uint32_t> first(F.size(), 0);
        for (size_t j = 0; j < F.size(); ++j) {
            std::vector<uint32_t> st{nodeOf(res.points[F[j]].miter)};
            std::unordered_set<uint32_t> seen;
            while (!st.empty()) {
                uint32_t n = st.back();
                st.pop_back();
                if (!n || !seen.insert(n).second) continue;
                if (g.isAnd(n)) { st.push_back(nodeOf(g.node(n).a)); st.push_back(nodeOf(g.node(n).b)); continue; }
                if (!own.count(n)) continue;
                parent.emplace(n, n);
                if (first[j]) parent[find(n)] = find(first[j]);
                else first[j] = n;
            }
        }
        std::map<uint32_t, std::vector<size_t>> groups;
        for (size_t j = 0; j < F.size(); ++j) groups[first[j] ? find(first[j]) : 0].push_back(F[j]);
        std::vector<std::string> resets;
        auto accept = [&](const std::vector<size_t>& pts, const std::string& reset) {
            if (std::find(resets.begin(), resets.end(), reset) == resets.end()) resets.push_back(reset);
            res.inductive += pts.size();
            for (size_t k : pts) {
                Point& p = res.points[k];
                p.status = Point::Pass;
                p.how = "induction";
                p.cex.clear();
            }
        };
        std::vector<size_t> left, open;  // not inductive; ran out of effort
        for (auto& [root, pts] : groups) {
            std::string reset = root ? prove(pts) : std::string();
            if (reset == "?") open.insert(open.end(), pts.begin(), pts.end());
            else if (!reset.empty()) accept(pts, reset);
            else left.insert(left.end(), pts.begin(), pts.end());
        }
        if (!left.empty() && groups.size() > 1 && left.size() == F.size()) {
            std::string reset = prove(F);
            if (reset == "?") open = F;
            else if (!reset.empty()) accept(F, reset);
        }
        // A point that fails in some state and whose induction was not settled
        // is not known to differ: the state may be unreachable.
        for (size_t k : open) {
            Point& p = res.points[k];
            if (p.status != Point::Fail) continue;
            p.status = Point::Abort;
            p.how = "induction";
        }
        for (auto& r : resets) res.inductionReset += (res.inductionReset.empty() ? "" : ", ") + r;
    }();

    for (auto& p : res.points) {
        if (p.status == Point::Pass) ++res.passed;
        else if (p.status == Point::Fail) ++res.failed;
        else ++res.aborted;
    }
    return res;
}
}  // namespace detail

// Check the two designs. When registers were matched by name but their
// next-state functions differ, synthesis may have stored them inverted with
// no net left to say so (an internal set-type register on a library whose
// flops only clear). Those are retried with the opposite polarity. Any
// polarity under which every point passes is a proof: the outputs agree and
// the matched registers stay in step, reset values included.
inline Result check(Aig& g, const Design& ref, Design& impl, const CheckOptions& opt) {
    std::set<std::string> flips;
    Result best = detail::checkOnce(g, ref, impl, opt, flips);
    for (int round = 0; round < 4 && best.failed > 0; ++round) {
        std::set<std::string> forced;
        for (auto& [r, i] : opt.matches) forced.insert(i);
        std::set<std::string> next = flips;
        for (auto& p : best.points)
            if (p.kind == Point::Next && p.status == Point::Fail && p.note.empty()) {
                const std::string& in = p.implName.empty() ? p.name : p.implName;
                if (!forced.count(in)) next.insert(in);
            }
        if (next.size() == flips.size()) break;
        Result r = detail::checkOnce(g, ref, impl, opt, next);
        if (r.failed >= best.failed) break;
        best = std::move(r);
        flips = std::move(next);
    }
    return best;
}

}  // namespace equiv
