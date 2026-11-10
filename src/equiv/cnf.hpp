#pragma once
// cnf.hpp - Tseitin encoding of AIG cones into a Sat instance, on demand.
#include <unordered_map>
#include <vector>

#include "aig.hpp"
#include "../sat.hpp"

namespace equiv {

struct Cnf {
    const Aig& g;
    sat::Solver sat;
    std::unordered_map<uint32_t, int> var;  // AIG node -> solver variable
    std::vector<uint32_t> stack;

    explicit Cnf(const Aig& g) : g(g) {
        int f = sat.newVar();  // node 0: constant false
        var[0] = f;
        sat.addClause({-f});
    }

    int lit(Lit l) {
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
                int a = isNeg(nd.a) ? -ia->second : ia->second, b = isNeg(nd.b) ? -ib->second : ib->second;
                int v = sat.newVar();
                var[n] = v;
                sat.addClause({-v, a});
                sat.addClause({-v, b});
                sat.addClause({v, -a, -b});
                stack.pop_back();
            }
        }
        return isNeg(l) ? -var[root] : var[root];
    }

    // Is `l` satisfiable? 1 yes (model available), 0 no, -1 budget exhausted.
    int solve(Lit l, uint64_t budget) {
        if (l == kFalse) return 0;
        int s = lit(l);
        return int(sat.solve({s}, budget ? int64_t(budget) : -1));
    }
    bool model(uint32_t node) {
        auto it = var.find(node);
        return it != var.end() && sat.value(it->second);
    }
};

}  // namespace equiv
