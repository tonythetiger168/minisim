#pragma once
// cnf.hpp - Tseitin encoding of AIG cones into a Sat instance, on demand.
#include <unordered_map>
#include <vector>

#include "aig.hpp"
#include "sat.hpp"

namespace equiv {

struct Cnf {
    const Aig& g;
    Sat sat;
    std::unordered_map<uint32_t, uint32_t> var;  // AIG node -> solver variable
    std::vector<uint32_t> stack;

    explicit Cnf(const Aig& g) : g(g) {
        uint32_t f = sat.newVar();  // node 0: constant false
        var[0] = f;
        sat.addClause(Sat::mk(f, true));
    }

    Sat::L lit(Lit l) {
        uint32_t root = nodeOf(l);
        if (!var.count(root)) {
            stack.assign(1, root);
            while (!stack.empty()) {
                uint32_t n = stack.back();
                if (var.count(n)) { stack.pop_back(); continue; }
                if (!g.isAnd(n)) { var[n] = sat.newVar(); stack.pop_back(); continue; }
                Aig::Node nd = g.node(n);
                auto ia = var.find(nodeOf(nd.a)), ib = var.find(nodeOf(nd.b));
                if (ia == var.end() || ib == var.end()) {
                    if (ia == var.end()) stack.push_back(nodeOf(nd.a));
                    if (ib == var.end()) stack.push_back(nodeOf(nd.b));
                    continue;
                }
                Sat::L a = Sat::mk(ia->second, isNeg(nd.a)), b = Sat::mk(ib->second, isNeg(nd.b));
                uint32_t v = sat.newVar();
                var[n] = v;
                Sat::L o = Sat::mk(v);
                sat.addClause(Sat::no(o), a);
                sat.addClause(Sat::no(o), b);
                sat.addClause(o, Sat::no(a), Sat::no(b));
                stack.pop_back();
            }
        }
        return Sat::mk(var[root], isNeg(l));
    }

    // Is `l` satisfiable? 1 yes (model available), 0 no, -1 budget exhausted.
    int solve(Lit l, uint64_t budget) {
        if (l == kFalse) return 0;
        Sat::L s = lit(l);
        return int(sat.solve({s}, budget));
    }
    bool model(uint32_t node) const {
        auto it = var.find(node);
        return it != var.end() && sat.modelValue(it->second);
    }
};

}  // namespace equiv
