// Scheduler tests. Same shape as test_expr / test_z: one translation unit,
// include path `src`. The kernel is src/minisim.cpp, which also contains the
// demo main, so include it without that main.
//
// Expected values were checked against Icarus Verilog 13.0 (stable) (v13_0).
// Re-run on that build (tag v13_0, dfeee909). Icarus 12.0 printed the same
// stdout for every case below, and the same demo VCD.
// Verilator 5.052 (--timing --binary) matched the 0/1 scheduling cases
// (#0 chain, regions, NBA swap, partial NBAs, pre/post NBA). It also
// printed "after" in the $finish repro, which Icarus does not. X/Z edges
// and wire values are 2-state there and are not these expected values.
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#define MINISIM_NO_MAIN
#include "minisim.cpp"

static std::vector<std::string> hash_log;
static std::vector<int> region_log;
static std::vector<std::pair<uint64_t, char>> edges; // 'p' posedge, 'n' negedge
static std::vector<std::pair<uint64_t, uint64_t>> mb_pos;

static int es, ra, sa, sb, sclk, pv, lw, qsig, rsig, mb, ixr;
static int before_sig, after_sig, other_sig, later_sig;
static int pre_q = -1, pre_r = -1;
static int other_saw_before = 0, other_saw_after_x = 0;

static int bit_class(Val v) {
    if ((v.z & 1) && (v.x & 1)) return 3; // z
    if (v.x & 1) return 2;                // x
    return (int)(v.v & 1);
}
static bool known(Val v, uint64_t n) { return v.x == 0 && v.z == 0 && v.v == n; }
static bool is_x(Val v) { return v.v == 0 && v.z == 0 && v.x == mask(v.w); }

static int drive2(Val a, Val b) {
    int s = S.sig("w", 1);
    int d0 = S.newDriver(s), d1 = S.newDriver(s);
    S.drive(s, d0, a);
    S.drive(s, d1, b);
    return bit_class(S.get(s));
}

// #0 chain. Icarus 13.0 (v13_0) prints HASH A, HASH C, HASH B (the second #0 runs after
// the first inactive batch).
Task pA() {
    co_await Delay{0};
    hash_log.push_back("A");
    co_await Delay{0};
    hash_log.push_back("B");
}
Task pC() {
    co_await Delay{0};
    hash_log.push_back("C");
}

// Edge table on a 1-bit signal that starts as X.
// Icarus 13.0 (v13_0): X->0 neg, 0->X pos, X->1 pos, 1->X neg, X->Z none,
//         Z->0 neg, 0->Z pos, Z->1 pos, 1->0 neg.
Task epos() {
    for (;;) {
        co_await Wait{es, POS};
        edges.push_back({S.now, 'p'});
    }
}
Task eneg() {
    for (;;) {
        co_await Wait{es, NEG};
        edges.push_back({S.now, 'n'});
    }
}
Task edge_stim() {
    co_await Delay{1};
    S.set(es, mk(0));
    co_await Delay{1};
    S.set(es, allx(1));
    co_await Delay{1};
    S.set(es, mk(1));
    co_await Delay{1};
    S.set(es, allx(1));
    co_await Delay{1};
    S.set(es, allz(1));
    co_await Delay{1};
    S.set(es, mk(0));
    co_await Delay{1};
    S.set(es, allz(1));
    co_await Delay{1};
    S.set(es, mk(1));
    co_await Delay{1};
    S.set(es, mk(0));
    co_await Delay{1};
}

// 4-bit posedge is the LSB only. Icarus 13.0 (v13_0) printed "MB pos" at t=1 (0001) and
// t=3 (0011), and not when the vector went 1 -> 2.
Task mbw() {
    for (;;) {
        co_await Wait{mb, POS};
        mb_pos.push_back({S.now, S.get(mb).v});
    }
}
Task mbs() {
    S.set(mb, mk(0, 4));
    co_await Delay{1};
    S.set(mb, mk(1, 4));
    co_await Delay{1};
    S.set(mb, mk(2, 4));
    co_await Delay{1};
    S.set(mb, mk(3, 4));
    co_await Delay{1};
}

// Regions. Icarus 13.0 (v13_0): "R active 0", "R hash0 0", "R hash0b 2", "R t1 1".
// a = 0; a <= 1; #0; (still 0); a = 2; then the NBA overwrites with 1.
Task region() {
    co_await Delay{20};
    S.set(ra, mk(0, 4));
    region_log.push_back(S.get(ra).x ? -1 : (int)S.get(ra).v);
    S.nba(ra, mk(1, 4));
    co_await Delay{0};
    region_log.push_back(S.get(ra).x ? -1 : (int)S.get(ra).v);
    S.set(ra, mk(2, 4));
    region_log.push_back(S.get(ra).x ? -1 : (int)S.get(ra).v);
}
Task region2() {
    co_await Delay{21};
    region_log.push_back(S.get(ra).x ? -1 : (int)S.get(ra).v);
}

// NBA swap. Icarus 13.0 (v13_0): a<=b; b<=a on one posedge, from a=1 b=0, ends a=0 b=1.
Task sw() {
    for (;;) {
        co_await Wait{sclk, POS};
        Val av = S.get(sa), bv = S.get(sb);
        S.nba(sa, bv);
        S.nba(sb, av);
    }
}
Task swstim() {
    S.set(sa, mk(1));
    S.set(sb, mk(0));
    S.set(sclk, mk(0));
    co_await Delay{30};
    S.set(sclk, mk(1));
    co_await Delay{1};
}

// Same-slot partial NBAs. Icarus 13.0 (v13_0): pv[0]<=1; pv[3:2]<=2'b11 from 0 -> 1101.
// Whole-word NBA of 0 then lw[0]<=1 from 1111 -> 0001.
Task partial() {
    co_await Delay{40};
    S.set(pv, mk(0, 4));
    S.nba(pv, mk(1, 1), 0, 0);
    S.nba(pv, mk(0b11, 2), 3, 2);
    co_await Delay{1};
}
Task lastp() {
    co_await Delay{50};
    S.set(lw, mk(0xF, 4));
    S.nba(lw, mk(0, 4));
    S.nba(lw, mk(1, 1), 0, 0);
    co_await Delay{1};
}

// Blocking read sees the pre-NBA value. Icarus 13.0 (v13_0): q<=5; r=q leaves r==0 at #0,
// and q==5 at the next time.
Task pre() {
    co_await Delay{60};
    S.set(qsig, mk(0, 4));
    S.set(rsig, mk(9, 4));
    S.nba(qsig, mk(5, 4));
    S.set(rsig, S.get(qsig));
    co_await Delay{0};
    pre_q = (int)S.get(qsig).v;
    pre_r = (int)S.get(rsig).v;
}
Task post() { co_await Delay{61}; }

// Same time slot as $finish. Icarus 13.0 (v13_0) prints "other" and does not print "after".
Task other_at_finish() {
    co_await Delay{80};
    other_saw_before = known(S.get(before_sig), 1) ? 1 : 0;
    other_saw_after_x = is_x(S.get(after_sig)) ? 1 : 0;
    S.set(other_sig, mk(1));
}
Task later_time() {
    co_await Delay{81};
    S.set(later_sig, mk(1));
}

static int fail(const std::string& msg) {
    std::cerr << "FAIL " << msg << "\n";
    return 1;
}

int main() {
    // Wire resolution. Icarus 13.0 (v13_0): 0 fighting 1 -> x, 0 with z -> 0, z with z -> z,
    // 0 with x -> x.
    if (drive2(mk(0), mk(1)) != 2) return fail("resolve 0/1 -> x");
    if (drive2(mk(0), allz(1)) != 0) return fail("resolve 0/z -> 0");
    if (drive2(allz(1), allz(1)) != 3) return fail("resolve z/z -> z");
    if (drive2(mk(0), allx(1)) != 2) return fail("resolve 0/x -> x");

    es = S.sig("e", 1);
    ra = S.sig("ra", 4);
    sa = S.sig("sa", 1);
    sb = S.sig("sb", 1);
    sclk = S.sig("sclk", 1);
    pv = S.sig("pv", 4);
    lw = S.sig("lw", 4);
    qsig = S.sig("q", 4);
    rsig = S.sig("r", 4);
    mb = S.sig("mb", 4);
    ixr = S.sig("ixr", 4);
    before_sig = S.sig("before", 4);
    after_sig = S.sig("after", 4);
    other_sig = S.sig("other", 1);
    later_sig = S.sig("later", 1);

    // if (1'bx) takes the else. Icarus 13.0 (v13_0) prints IFX 2.
    static Prog ifp;
    { Instr i; i.k = IK::JmpIfNot; i.e = konst(allx(1)); i.target = 2; ifp.emit(i); }
    { Instr i; i.k = IK::Assign; i.sig = ixr; i.e = konst(mk(1, 4)); ifp.emit(i); }
    { Instr i; i.k = IK::Assign; i.sig = ixr; i.e = konst(mk(2, 4)); ifp.emit(i); }

    // Three-initial $finish repro, through IK::Finish:
    //   #80 before = 1; $finish; after = 1;
    //   #80 other runs
    //   #81 later must not run
    // Icarus 13.0 (v13_0) prints "before" and "other", not "after", and nothing at #11
    // (here the same shape at time 80 / 81). Same-slot processes already
    // queued still run; this process does not continue; later times do not.
    static Prog fp;
    { Instr i; i.k = IK::WaitDelay; i.delay = 80; fp.emit(i); }
    { Instr i; i.k = IK::Assign; i.sig = before_sig; i.e = konst(mk(1, 4)); fp.emit(i); }
    { Instr i; i.k = IK::Finish; fp.emit(i); }
    { Instr i; i.k = IK::Assign; i.sig = after_sig; i.e = konst(mk(1, 4)); fp.emit(i); }

    S.spawn(exec(&ifp));
    S.spawn(pA());
    S.spawn(pC());
    S.spawn(epos());
    S.spawn(eneg());
    S.spawn(edge_stim());
    S.spawn(region());
    S.spawn(region2());
    S.spawn(sw());
    S.spawn(swstim());
    S.spawn(partial());
    S.spawn(lastp());
    S.spawn(pre());
    S.spawn(post());
    S.spawn(mbw());
    S.spawn(mbs());
    S.spawn(exec(&fp));
    S.spawn(other_at_finish());
    S.spawn(later_time());
    S.run(1000);

    if (hash_log != std::vector<std::string>{"A", "C", "B"}) {
        std::cerr << "FAIL #0 chain";
        for (auto& s : hash_log) std::cerr << " " << s;
        std::cerr << "\n";
        return 1;
    }
    const std::vector<std::pair<uint64_t, char>> want_e = {
        {1, 'n'}, {2, 'p'}, {3, 'p'}, {4, 'n'}, {6, 'n'}, {7, 'p'}, {8, 'p'}, {9, 'n'}};
    if (edges != want_e) {
        std::cerr << "FAIL edges";
        for (auto& e : edges) std::cerr << " " << e.first << e.second;
        std::cerr << "\n";
        return 1;
    }
    const std::vector<std::pair<uint64_t, uint64_t>> want_mb = {{1, 1}, {3, 3}};
    if (mb_pos != want_mb) {
        std::cerr << "FAIL multibit posedge";
        for (auto& e : mb_pos) std::cerr << " t=" << e.first << " v=" << e.second;
        std::cerr << "\n";
        return 1;
    }
    if (region_log != std::vector<int>{0, 0, 2, 1}) {
        std::cerr << "FAIL regions";
        for (int n : region_log) std::cerr << " " << n;
        std::cerr << "\n";
        return 1;
    }
    if (!known(S.get(ra), 1)) return fail("region final a != 1");
    if (!known(S.get(sa), 0) || !known(S.get(sb), 1)) return fail("NBA swap");
    if (!known(S.get(pv), 0b1101)) return fail("partial NBA != 1101");
    if (!known(S.get(lw), 0b0001)) return fail("overlapping NBA != 0001");
    if (pre_q != 0 || pre_r != 0) return fail("blocking read saw post-NBA value at #0");
    if (!known(S.get(qsig), 5) || !known(S.get(rsig), 0)) return fail("NBA q<=5 did not land");
    if (!known(S.get(ixr), 2)) return fail("if (1'bx) did not take else");

    if (!known(S.get(before_sig), 1)) return fail("$finish: before-assign did not run");
    if (!is_x(S.get(after_sig))) {
        Val a = S.get(after_sig);
        std::cerr << "FAIL assign after $finish stored v=" << a.v << " x=" << a.x << "\n";
        return 1;
    }
    if (!other_saw_before || !other_saw_after_x) return fail("$finish: other did not observe before-then-still-X");
    if (!known(S.get(other_sig), 1)) return fail("$finish: same-slot other process did not run");
    if (!is_x(S.get(later_sig))) return fail("$finish: a later time step ran");
    if (S.now != 80 || !S.finished) return fail("$finish: sim did not stop at the finish time");

    std::cout << "ok\n";
    return 0;
}
