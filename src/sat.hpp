// SPDX-License-Identifier: Apache-2.0
// sat.hpp - the SAT solver interface shared by the minisim tools (mequiv,
// mformal). One class over CaDiCaL (cmake target `cadical`), so there is one
// solver in the tree and one place to change it.
//
//   sat::Solver s;                      // one object per thread, no globals
//   int a = s.newVar(), b = s.newVar(); // variables are 1, 2, 3, ...
//   s.addClause({a, -b});               // literals are signed, as in DIMACS
//   if (s.solve({b}, 10000) == sat::Sat) bool va = s.value(a);
//
// Clauses may be added between calls to solve (incremental). A conflict
// limit makes the answer depend on effort only, never on time, so results
// are the same on any machine.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "cadical.hpp"

namespace sat {

enum Result { Unsat = 0, Sat = 1, Unknown = -1 };

class Solver {
public:
    Solver() {
        s_.set("quiet", 1);
        s_.connect_learner(&count_);
    }
    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;
    ~Solver() { s_.disconnect_learner(); }

    int newVar() { return ++vars_; }
    int vars() const { return vars_; }

    void addClause(std::span<const int> lits) {
        for (int l : lits) s_.add(l);
        s_.add(0);
    }
    void addClause(std::initializer_list<int> lits) { addClause(std::span<const int>(lits.begin(), lits.size())); }

    // Solve under the assumptions. conflictLimit < 0: no limit; otherwise
    // Unknown is returned once that many conflicts were spent in this call.
    Result solve(std::span<const int> assumptions = {}, int64_t conflictLimit = -1) {
        for (int l : assumptions) s_.assume(l);
        if (conflictLimit >= 0) s_.limit("conflicts", int(conflictLimit > INT32_MAX ? INT32_MAX : conflictLimit));
        int r = s_.solve();
        return r == 10 ? Sat : r == 20 ? Unsat : Unknown;
    }
    Result solve(std::initializer_list<int> assumptions, int64_t conflictLimit = -1) {
        return solve(std::span<const int>(assumptions.begin(), assumptions.size()), conflictLimit);
    }

    // After Sat: the value of any variable, including ones never constrained.
    bool value(int var) { return var <= s_.vars() && s_.val(var) > 0; }
    // After Unsat: whether this assumption is part of the reason.
    bool failed(int assumption) { return s_.failed(assumption); }
    // Conflicts spent so far, over all calls.
    int64_t conflicts() const { return count_.n; }

    // For what this interface does not cover (options, proofs).
    CaDiCaL::Solver& raw() { return s_; }

private:
    struct Count : CaDiCaL::Learner {
        int64_t n = 0;
        bool learning(int) override { ++n; return false; }
        void learn(int) override {}
    };
    CaDiCaL::Solver s_;
    Count count_;
    int vars_ = 0;
};

} // namespace sat
