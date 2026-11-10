// The SAT solver behind mequiv, against brute force and known-hard formulas.
#include <cassert>
#include <cstdio>
#include <random>

#include "equiv/aig.hpp"
#include "equiv/cnf.hpp"
#include "equiv/sat.hpp"

using equiv::Sat;

static bool brute(int n, const std::vector<std::vector<Sat::L>>& cls, uint32_t fixMask = 0, uint32_t fixVal = 0) {
    for (uint32_t m = 0; m < (1u << n); ++m) {
        if ((m & fixMask) != fixVal) continue;
        bool all = true;
        for (auto& c : cls) {
            bool any = false;
            for (Sat::L l : c) any |= bool(m >> (l >> 1) & 1) != bool(l & 1);
            if (!any) { all = false; break; }
        }
        if (all) return true;
    }
    return false;
}

int main() {
    std::mt19937 rng(12345);
    // Random 3-SAT near the threshold: the answer and the model must be right.
    for (int iter = 0; iter < 400; ++iter) {
        int n = 6 + int(rng() % 10), m = int(n * 4.3);
        std::vector<std::vector<Sat::L>> cls;
        Sat s;
        for (int i = 0; i < n; ++i) s.newVar();
        for (int i = 0; i < m; ++i) {
            std::vector<Sat::L> c;
            for (int k = 0; k < 3; ++k) c.push_back(Sat::mk(rng() % n, rng() & 1));
            cls.push_back(c);
            s.addClause(c);
        }
        bool want = brute(n, cls);
        Sat::Result r = s.solve({});
        assert((r == Sat::SatResult) == want);
        if (r == Sat::SatResult)
            for (auto& c : cls) {
                bool any = false;
                for (Sat::L l : c) any |= s.modelValue(l >> 1) != bool(l & 1);
                assert(any);
            }
        // The same formula under two assumptions, twice (incremental use).
        for (int rep = 0; rep < 2; ++rep) {
            uint32_t a = rng() % n, b = rng() % n;
            bool av = rng() & 1, bvv = rng() & 1;
            if (a == b) continue;
            bool w2 = brute(n, cls, (1u << a) | (1u << b), (uint32_t(av) << a) | (uint32_t(bvv) << b));
            Sat::Result r2 = s.solve({Sat::mk(a, !av), Sat::mk(b, !bvv)});
            assert((r2 == Sat::SatResult) == w2);
            if (r2 == Sat::SatResult) assert(s.modelValue(a) == av && s.modelValue(b) == bvv);
        }
    }
    // Pigeonhole: 8 pigeons, 7 holes. Unsatisfiable, and hard enough to need
    // restarts and clause deletion.
    {
        const int P = 8, H = 7;
        Sat s;
        for (int i = 0; i < P * H; ++i) s.newVar();
        for (int p = 0; p < P; ++p) {
            std::vector<Sat::L> c;
            for (int h = 0; h < H; ++h) c.push_back(Sat::mk(p * H + h));
            s.addClause(c);
        }
        for (int h = 0; h < H; ++h)
            for (int p = 0; p < P; ++p)
                for (int q = p + 1; q < P; ++q) s.addClause(Sat::mk(p * H + h, true), Sat::mk(q * H + h, true));
        Sat t = s;
        assert(t.solve({}, 50) == Sat::Unknown);  // a budget is honoured
        assert(s.solve({}) == Sat::Unsat);
        std::printf("pigeonhole 8/7: %llu conflicts\n", static_cast<unsigned long long>(s.conflicts()));
    }
    // Two 6-bit adders built differently are equal; break one and they are not.
    {
        equiv::Aig g;
        std::vector<equiv::Lit> a, b;
        for (int i = 0; i < 6; ++i) a.push_back(g.input()), b.push_back(g.input());
        equiv::Lit c1 = equiv::kFalse, c2 = equiv::kFalse, diff = equiv::kFalse, diffBad = equiv::kFalse;
        for (int i = 0; i < 6; ++i) {
            equiv::Lit s1 = g.xor_(g.xor_(a[i], b[i]), c1);
            c1 = g.or_(g.and_(a[i], b[i]), g.and_(c1, g.xor_(a[i], b[i])));
            equiv::Lit s2 = g.xor_(a[i], g.xor_(b[i], c2));
            equiv::Lit c2n = g.mux(c2, g.or_(a[i], b[i]), g.and_(a[i], b[i]));
            diff = g.or_(diff, g.xor_(s1, s2));
            diffBad = g.or_(diffBad, g.xor_(s1, i == 4 ? g.xor_(a[i], b[i]) : s2));
            c2 = c2n;
        }
        equiv::Cnf cnf(g);
        assert(cnf.solve(diff, 0) == 0);
        assert(cnf.solve(diffBad, 0) == 1);
    }
    std::puts("test_sat: ok");
    return 0;
}
