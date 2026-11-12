// The SAIF reader (src/saif.hpp): per-net activity, and the state- and
// path-dependent counts (COND / IOPATH / LEAKAGE) of a backward SAIF file.
#include <cassert>
#include <cmath>
#include <cstdio>

#include "saif.hpp"

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_saif gates_sdpd.saif\n");
        return 2;
    }
    saif::Db db;
    saif::read(argv[1], db);
    assert(near(db.duration, 1000));
    assert(near(db.tick, 1e-9));

    // plain nets, as before
    const saif::Bit* clk = db.find("tb.dut.clk");
    assert(clk && near(clk->tc, 200) && near(clk->t1, 500));
    const saif::Bit* n2 = db.find("tb.dut.n2");
    assert(n2 && near(n2->tc, 16));

    // the port of u2 keeps its totals ...
    const saif::Bit* y = db.find("tb.dut.u2.Y");
    assert(y && near(y->tc, 16) && near(y->t1, 580));
    // ... and gets its toggles per condition, edge and causing input
    auto it = db.paths.find("tb.dut.u2.Y");
    assert(it != db.paths.end());
    const auto& p = it->second;
    assert(p.size() == 5);
    assert(p[0].cond == "B" && p[0].edge == 1 && p[0].related == "A" && near(p[0].tc, 6));
    assert(p[1].cond == "B" && p[1].edge == 2 && p[1].related == "A" && near(p[1].tc, 6));
    assert(p[2].cond == "A" && p[2].edge == 1 && p[2].related == "B" && near(p[2].tc, 2));
    assert(p[3].cond == "A" && p[3].edge == 2 && p[3].related == "B" && near(p[3].tc, 1));
    assert(p[4].cond.empty() && p[4].edge == 0 && p[4].related.empty() && near(p[4].tc, 0));

    // the time u1 spent in each state
    auto st = db.states.find("tb.dut.u1");
    assert(st != db.states.end() && st->second.size() == 2);
    assert(st->second[0].cond == "A" && near(st->second[0].t1, 300));
    assert(st->second[1].cond == "!A" && near(st->second[1].t1, 700));

    // a parenthesized condition, IOPATH without COND, IOPATH_DEFAULT
    const char* text =
        "(SAIFILE (SAIFVERSION \"2.0\") (DIRECTION \"backward\") (DIVIDER / ) (TIMESCALE 1 ns) (DURATION 100)\n"
        " (INSTANCE top (INSTANCE g\n"
        "  (PORT (Z (T0 50) (T1 50) (TC 9)\n"
        "    (IOPATH A (TC 4) IOPATH_DEFAULT (TC 5))\n"
        "    (COND (A*!B) (RISE) (IOPATH B (TC 3)))))\n"
        "  (LEAKAGE (COND (A*B) (T1 20) (T0 80)) COND_DEFAULT (T1 80) (T0 20)))))\n";
    std::string path = "test_saif_inline.saif";   // in the directory ctest runs in
    {
        std::ofstream f(path);
        f << text;
    }
    saif::Db d2;
    saif::read(path, d2);
    std::remove(path.c_str());
    const auto& z = d2.paths.at("top.g.Z");
    assert(z.size() == 3);
    assert(z[0].related == "A" && z[0].cond.empty() && near(z[0].tc, 4));
    assert(z[1].related.empty() && near(z[1].tc, 5));
    assert(z[2].cond == "(A*!B)" && z[2].edge == 1 && z[2].related == "B" && near(z[2].tc, 3));
    assert(d2.find("top.g.Z") && near(d2.find("top.g.Z")->tc, 9));
    const auto& s2 = d2.states.at("top.g");
    assert(s2.size() == 2 && s2[0].cond == "(A*B)" && near(s2[0].t1, 20) && s2[1].cond.empty() && near(s2[1].t1, 80));

    std::printf("test_saif: ok\n");
    return 0;
}
