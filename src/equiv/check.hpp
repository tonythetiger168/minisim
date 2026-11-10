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
#include <atomic>
#include <map>
#include <string>
#include <thread>
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
    std::vector<std::pair<std::string, std::string>> matches;  // ref name, impl name
};

struct Result {
    std::vector<Point> points;
    std::vector<std::string> refOnlyInputs, implOnlyInputs, refOnlyOutputs, implOnlyOutputs;
    std::vector<std::string> refOnlyStates, implOnlyStates;
    size_t matchedInputs = 0, matchedStates = 0, matchedOutputs = 0;
    size_t passed = 0, failed = 0, aborted = 0;
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

}  // namespace detail

inline Result check(Aig& g, const Design& ref, Design& impl, const CheckOptions& opt) {
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
        memo[nodeOf(impl.states[i].q)] = ref.states[stMap[i]].q;
        refSt[stMap[i]] = 1;
        ++res.matchedStates;
    }
    for (size_t r = 0; r < ref.inputs.size(); ++r) if (!refIn[r]) res.refOnlyInputs.push_back(ref.inputs[r].first);
    for (size_t r = 0; r < ref.states.size(); ++r) if (!refSt[r]) res.refOnlyStates.push_back(ref.states[r].name);
    auto move = [&](Bit b) { return Bit{g.rebuild(b.v, memo), g.rebuild(b.x, memo)}; };

    // ---- compare points
    auto add = [&](Point::Kind kind, const std::string& rn, const std::string& in, Bit r, Bit i) {
        Point p;
        p.kind = kind;
        p.name = rn;
        if (in != rn) p.implName = in;
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
        add(Point::Next, rs.name, is.name, rs.next, move(is.next));
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
            if (nd.a == kNoLit) {
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

    // ---- SAT, in parallel
    std::vector<size_t> work;
    for (size_t k = 0; k < res.points.size(); ++k) if (open[k]) work.push_back(k);
    unsigned nt = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
    nt = unsigned(std::min<size_t>(nt, std::max<size_t>(work.size(), 1)));
    std::atomic<size_t> nextJob{0};
    auto worker = [&] {
        for (;;) {
            size_t j = nextJob.fetch_add(1);
            if (j >= work.size()) return;
            Point& p = res.points[work[j]];
            Cnf cnf(g);
            int r = cnf.solve(p.miter, opt.effort);
            p.conflicts = cnf.sat.conflicts();
            p.how = "sat";
            if (r == 0) p.status = Point::Pass;
            else if (r < 0) p.status = Point::Abort;
            else {
                p.status = Point::Fail;
                finishCex(p, [&](uint32_t n) { return cnf.model(n); });
            }
        }
    };
    if (nt <= 1) worker();
    else {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < nt; ++t) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }

    for (auto& p : res.points) {
        if (p.status == Point::Pass) ++res.passed;
        else if (p.status == Point::Fail) ++res.failed;
        else ++res.aborted;
    }
    return res;
}

}  // namespace equiv
