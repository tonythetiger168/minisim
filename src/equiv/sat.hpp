#pragma once
// sat.hpp - a small CDCL SAT solver (two watched literals, first-UIP clause
// learning with minimization, VSIDS, phase saving, Luby restarts, learnt
// clause reduction). Deterministic: the same clauses in the same order give
// the same answer, the same model, and the same conflict count, which is what
// lets mequiv bound its effort in conflicts rather than seconds and still
// report the same result on any machine and any thread count.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace equiv {

class Sat {
public:
    // A solver literal: var*2 + negated.
    using L = uint32_t;
    static L mk(uint32_t v, bool negated = false) { return v * 2 + (negated ? 1 : 0); }
    static L no(L l) { return l ^ 1u; }

    enum Result { Unsat = 0, SatResult = 1, Unknown = -1 };

    uint32_t newVar() {
        uint32_t v = uint32_t(assign_.size());
        assign_.push_back(2);
        level_.push_back(0);
        reason_.push_back(kNoRef);
        activity_.push_back(0);
        phase_.push_back(1);  // default: false
        seen_.push_back(0);
        heapPos_.push_back(-1);
        watches_.emplace_back();
        watches_.emplace_back();
        heapInsert(v);
        return v;
    }
    size_t numVars() const { return assign_.size(); }
    uint64_t conflicts() const { return conflicts_; }

    // Add a clause at decision level 0. Returns false once the formula is
    // known unsatisfiable.
    bool addClause(std::vector<L> c) {
        if (!ok_) return false;
        std::sort(c.begin(), c.end());
        size_t j = 0;
        L prev = 0xFFFFFFFFu;
        for (L l : c) {
            if (value(l) == 1 || l == no(prev)) return true;  // satisfied or tautology
            if (value(l) == 0 || l == prev) continue;
            c[j++] = prev = l;
        }
        c.resize(j);
        if (c.empty()) return ok_ = false;
        if (c.size() == 1) {
            enqueue(c[0], kNoRef);
            return ok_ = (propagate() == kNoRef);
        }
        attach(alloc(c, false));
        return true;
    }
    bool addClause(L a) { return addClause(std::vector<L>{a}); }
    bool addClause(L a, L b) { return addClause(std::vector<L>{a, b}); }
    bool addClause(L a, L b, L c) { return addClause(std::vector<L>{a, b, c}); }

    // Solve under assumptions with a conflict budget (0: unlimited).
    Result solve(const std::vector<L>& assumptions, uint64_t budget = 0) {
        if (!ok_) return Unsat;
        uint64_t limit = budget ? conflicts_ + budget : ~0ull;
        Result r = Unknown;
        for (uint32_t run = 0; r == Unknown; ++run) {
            if (conflicts_ >= limit) break;
            r = search(assumptions, uint64_t(luby(run)) * 100, limit);
        }
        if (r == SatResult) {
            model_.resize(assign_.size());
            for (size_t v = 0; v < assign_.size(); ++v) model_[v] = assign_[v] == 1;
        }
        backtrack(0);
        return r;
    }
    bool modelValue(uint32_t v) const { return model_[v]; }

private:
    using CRef = uint32_t;
    static constexpr CRef kNoRef = 0xFFFFFFFFu;
    struct Watcher {
        CRef c;
        L blocker;
    };

    // Clause arena: [size<<2 | learnt<<1 | deleted][activity bits][lits...]
    uint32_t& hdr(CRef c) { return arena_[c]; }
    uint32_t size(CRef c) const { return arena_[c] >> 2; }
    bool learnt(CRef c) const { return arena_[c] & 2; }
    bool deleted(CRef c) const { return arena_[c] & 1; }
    float& act(CRef c) { return *reinterpret_cast<float*>(&arena_[c + 1]); }
    L* lits(CRef c) { return &arena_[c + 2]; }

    CRef alloc(const std::vector<L>& c, bool isLearnt) {
        CRef r = CRef(arena_.size());
        arena_.push_back(uint32_t(c.size()) << 2 | (isLearnt ? 2 : 0));
        arena_.push_back(0);
        arena_.insert(arena_.end(), c.begin(), c.end());
        return r;
    }
    void attach(CRef c) {
        L* l = lits(c);
        watches_[no(l[0])].push_back({c, l[1]});
        watches_[no(l[1])].push_back({c, l[0]});
    }

    // 1 true, 0 false, 2 unassigned.
    uint8_t value(L l) const {
        uint8_t a = assign_[l >> 1];
        return a == 2 ? 2 : uint8_t(a ^ (l & 1));
    }
    uint32_t decisionLevel() const { return uint32_t(trailLim_.size()); }

    void enqueue(L l, CRef from) {
        uint32_t v = l >> 1;
        assign_[v] = uint8_t(1 ^ (l & 1));
        level_[v] = decisionLevel();
        reason_[v] = from;
        trail_.push_back(l);
    }

    CRef propagate() {
        CRef confl = kNoRef;
        while (qhead_ < trail_.size()) {
            L p = trail_[qhead_++];  // p is true; visit clauses watching !p
            std::vector<Watcher>& ws = watches_[p];
            size_t i = 0, j = 0, n = ws.size();
            while (i < n) {
                Watcher w = ws[i++];
                if (value(w.blocker) == 1) { ws[j++] = w; continue; }
                CRef c = w.c;
                if (deleted(c)) continue;
                L* l = lits(c);
                L falseLit = no(p);
                if (l[0] == falseLit) std::swap(l[0], l[1]);
                L first = l[0];
                if (first != w.blocker && value(first) == 1) { ws[j++] = {c, first}; continue; }
                uint32_t sz = size(c);
                bool found = false;
                for (uint32_t k = 2; k < sz; ++k)
                    if (value(l[k]) != 0) {
                        std::swap(l[1], l[k]);
                        watches_[no(l[1])].push_back({c, first});
                        found = true;
                        break;
                    }
                if (found) continue;
                ws[j++] = {c, first};
                if (value(first) == 0) {
                    confl = c;
                    qhead_ = trail_.size();
                    while (i < n) ws[j++] = ws[i++];
                } else {
                    enqueue(first, c);
                }
            }
            ws.resize(j);
        }
        return confl;
    }

    void bumpVar(uint32_t v) {
        if ((activity_[v] += varInc_) > 1e100) {
            for (double& a : activity_) a *= 1e-100;
            varInc_ *= 1e-100;
        }
        if (heapPos_[v] >= 0) heapUp(heapPos_[v]);
    }
    void bumpClause(CRef c) {
        if ((act(c) += float(claInc_)) > 1e20f) {
            for (CRef l : learnts_) act(l) *= 1e-20f;
            claInc_ *= 1e-20;
        }
    }

    void analyze(CRef confl, std::vector<L>& out, uint32_t& btLevel) {
        out.clear();
        out.push_back(0);
        int pathC = 0;
        L p = 0xFFFFFFFFu;
        size_t index = trail_.size();
        do {
            if (learnt(confl)) bumpClause(confl);
            L* l = lits(confl);
            uint32_t sz = size(confl);
            for (uint32_t k = (p == 0xFFFFFFFFu ? 0 : 1); k < sz; ++k) {
                L q = l[k];
                uint32_t v = q >> 1;
                if (!seen_[v] && level_[v] > 0) {
                    bumpVar(v);
                    seen_[v] = 1;
                    if (level_[v] >= decisionLevel()) ++pathC;
                    else out.push_back(q);
                }
            }
            while (!seen_[trail_[--index] >> 1]) {}
            p = trail_[index];
            confl = reason_[p >> 1];
            seen_[p >> 1] = 0;
            --pathC;
        } while (pathC > 0);
        out[0] = no(p);

        // Minimize: drop a literal whose reason is covered by the others.
        toClear_.assign(out.begin(), out.end());
        size_t j = 1;
        for (size_t i = 1; i < out.size(); ++i) {
            uint32_t v = out[i] >> 1;
            if (reason_[v] == kNoRef || !redundant(out[i])) out[j++] = out[i];
        }
        out.resize(j);
        for (L l : toClear_) seen_[l >> 1] = 0;

        if (out.size() == 1) {
            btLevel = 0;
        } else {
            size_t best = 1;
            for (size_t i = 2; i < out.size(); ++i)
                if (level_[out[i] >> 1] > level_[out[best] >> 1]) best = i;
            std::swap(out[1], out[best]);
            btLevel = level_[out[1] >> 1];
        }
    }
    bool redundant(L p) {
        stack_.clear();
        stack_.push_back(p);
        size_t top = toClear_.size();
        while (!stack_.empty()) {
            L q = stack_.back();
            stack_.pop_back();
            CRef c = reason_[q >> 1];
            L* l = lits(c);
            uint32_t sz = size(c);
            for (uint32_t k = 1; k < sz; ++k) {
                uint32_t v = l[k] >> 1;
                if (seen_[v] || level_[v] == 0) continue;
                if (reason_[v] == kNoRef) {
                    for (size_t t = top; t < toClear_.size(); ++t) seen_[toClear_[t] >> 1] = 0;
                    toClear_.resize(top);
                    return false;
                }
                seen_[v] = 1;
                stack_.push_back(l[k]);
                toClear_.push_back(l[k]);
            }
        }
        return true;
    }

    void backtrack(uint32_t lvl) {
        if (decisionLevel() <= lvl) return;
        for (size_t i = trail_.size(); i-- > trailLim_[lvl];) {
            uint32_t v = trail_[i] >> 1;
            phase_[v] = uint8_t(trail_[i] & 1);
            assign_[v] = 2;
            if (heapPos_[v] < 0) heapInsert(v);
        }
        trail_.resize(trailLim_[lvl]);
        trailLim_.resize(lvl);
        qhead_ = trail_.size();
    }

    void reduceDb() {
        std::sort(learnts_.begin(), learnts_.end(), [&](CRef a, CRef b) {
            float x = act(a), y = act(b);
            return x < y || (x == y && a < b);
        });
        size_t j = 0, half = learnts_.size() / 2;
        for (size_t i = 0; i < learnts_.size(); ++i) {
            CRef c = learnts_[i];
            L first = lits(c)[0];
            bool locked = value(first) == 1 && reason_[first >> 1] == c;
            if (i < half && size(c) > 2 && !locked) hdr(c) |= 1;
            else learnts_[j++] = c;
        }
        learnts_.resize(j);
    }

    Result search(const std::vector<L>& assumptions, uint64_t nConfl, uint64_t limit) {
        uint64_t local = 0;
        std::vector<L> learntClause;
        for (;;) {
            CRef confl = propagate();
            if (confl != kNoRef) {
                ++conflicts_;
                ++local;
                if (decisionLevel() == 0) { ok_ = false; return Unsat; }
                uint32_t bt;
                analyze(confl, learntClause, bt);
                // Never undo an assumption because of a conflict under it: if
                // the learnt clause is falsified by assumptions alone, the
                // next decision loop notices and returns Unsat.
                backtrack(bt);
                if (learntClause.size() == 1) {
                    enqueue(learntClause[0], kNoRef);
                } else {
                    CRef c = alloc(learntClause, true);
                    learnts_.push_back(c);
                    attach(c);
                    bumpClause(c);
                    enqueue(learntClause[0], c);
                }
                varInc_ *= 1.0 / 0.95;
                claInc_ *= 1.0 / 0.999;
            } else {
                if (local >= nConfl || conflicts_ >= limit) {
                    backtrack(0);
                    return Unknown;
                }
                if (learnts_.size() > maxLearnts_ + trail_.size()) {
                    reduceDb();
                    maxLearnts_ += maxLearnts_ / 10;
                }
                L next = 0xFFFFFFFFu;
                while (decisionLevel() < assumptions.size()) {
                    L a = assumptions[decisionLevel()];
                    if (value(a) == 1) {
                        trailLim_.push_back(uint32_t(trail_.size()));  // dummy level
                    } else if (value(a) == 0) {
                        backtrack(0);
                        return Unsat;
                    } else {
                        next = a;
                        break;
                    }
                }
                if (next == 0xFFFFFFFFu) {
                    while (!heap_.empty()) {
                        uint32_t v = heapPop();
                        if (assign_[v] == 2) { next = mk(v, phase_[v]); break; }
                    }
                    if (next == 0xFFFFFFFFu) return SatResult;
                }
                trailLim_.push_back(uint32_t(trail_.size()));
                enqueue(next, kNoRef);
            }
        }
    }

    static double luby(uint32_t x) {
        uint32_t size = 1, seq = 0;
        while (size < x + 1) { ++seq; size = 2 * size + 1; }
        while (size - 1 != x) {
            size = (size - 1) >> 1;
            --seq;
            x %= size;
        }
        double r = 1;
        while (seq--) r *= 2;
        return r;
    }

    // Max-heap of variables by activity (ties: lower index first).
    bool heapLess(uint32_t a, uint32_t b) const {
        return activity_[a] > activity_[b] || (activity_[a] == activity_[b] && a < b);
    }
    void heapInsert(uint32_t v) {
        heapPos_[v] = int(heap_.size());
        heap_.push_back(v);
        heapUp(heapPos_[v]);
    }
    void heapUp(int i) {
        uint32_t v = heap_[i];
        while (i > 0) {
            int p = (i - 1) / 2;
            if (!heapLess(v, heap_[p])) break;
            heap_[i] = heap_[p];
            heapPos_[heap_[i]] = i;
            i = p;
        }
        heap_[i] = v;
        heapPos_[v] = i;
    }
    uint32_t heapPop() {
        uint32_t top = heap_[0];
        heapPos_[top] = -1;
        uint32_t last = heap_.back();
        heap_.pop_back();
        if (!heap_.empty()) {
            int i = 0, n = int(heap_.size());
            for (;;) {
                int c = 2 * i + 1;
                if (c >= n) break;
                if (c + 1 < n && heapLess(heap_[c + 1], heap_[c])) ++c;
                if (!heapLess(heap_[c], last)) break;
                heap_[i] = heap_[c];
                heapPos_[heap_[i]] = i;
                i = c;
            }
            heap_[i] = last;
            heapPos_[last] = i;
        }
        return top;
    }

    bool ok_ = true;
    std::vector<uint32_t> arena_;
    std::vector<CRef> learnts_;
    std::vector<std::vector<Watcher>> watches_;
    std::vector<uint8_t> assign_, phase_, seen_;
    std::vector<uint32_t> level_;
    std::vector<CRef> reason_;
    std::vector<double> activity_;
    std::vector<int> heapPos_;
    std::vector<uint32_t> heap_;
    std::vector<L> trail_, stack_, toClear_;
    std::vector<uint32_t> trailLim_;
    std::vector<bool> model_;
    size_t qhead_ = 0;
    double varInc_ = 1, claInc_ = 1;
    uint64_t conflicts_ = 0;
    size_t maxLearnts_ = 4000;
};

}  // namespace equiv
