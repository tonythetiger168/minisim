// $display / $strobe / $monitor. Same shape as test_sched: the kernel is
// src/minisim.cpp, included without its demo main.
//
// Expected lines are the stdout of the same schedule on Icarus Verilog 13.0
// (stable) (v13_0, dfeee909), not counting Icarus's own "$finish called" line.
// $display prints in the region the statement runs in (here, active or #0),
// so it still sees the pre-NBA value. $strobe and $monitor print from the
// postponed region, after NBA and after any #0 activity, so they see the
// post-NBA value. $monitor prints at most one line per time slot, and only
// when a watched value changed during the slot (or when $monitor is called).
//
// A $strobe that printed in active, or a postponed queue drained before NBA,
// would show 0 where these lines show 1 (C1S1, C1S2, C3S, FS, MF).
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#define MINISIM_NO_MAIN
#include "minisim.cpp"

static Instr K(IK k) { Instr i; i.k = k; return i; }
static void delay(Prog& p, uint64_t n) { Instr i = K(IK::WaitDelay); i.delay = n; p.emit(i); }
static void assign(Prog& p, int s, Val v) { Instr i = K(IK::Assign); i.sig = s; i.e = konst(v); p.emit(i); }
static void nba(Prog& p, int s, Val v) { Instr i = K(IK::Nba); i.sig = s; i.e = konst(v); p.emit(i); }
static void sys(Prog& p, IK k, const char* fmt, std::vector<E> args = {}) {
    Instr i = K(k); i.fmt = fmt; i.args = std::move(args); p.emit(i);
}
static void finish(Prog& p) { p.emit(K(IK::Finish)); }

static std::vector<std::string> linesOf(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

int main() {
    int a = S.sig("a", 1);
    int b = S.sig("b", 1);
    int c = S.sig("c", 1);
    int w = S.sig("w", 4);

    // Format, immediate. %b keeps the width; %d is right-justified.
    // x vs X and z vs Z follow Icarus: all-unknown is lower case, partial is upper.
    static Prog fmt;
    assign(fmt, w, mk(0b0010, 4));
    sys(fmt, IK::Display, "F4b [%b]", {ref(w)});
    sys(fmt, IK::Display, "F4d [%d]", {ref(w)});
    assign(fmt, w, allx(4));
    sys(fmt, IK::Display, "Fx [%b] [%d]", {ref(w), ref(w)});
    assign(fmt, w, allz(4));
    sys(fmt, IK::Display, "Fz [%b] [%d]", {ref(w), ref(w)});
    assign(fmt, w, make(0b0001, 0b1100, 4));             // xx01
    sys(fmt, IK::Display, "Fmx [%b] [%d]", {ref(w), ref(w)});
    assign(fmt, w, make(0b0001, 0b1100, 4, 0b1100));      // zz01
    sys(fmt, IK::Display, "Fmz [%b] [%d]", {ref(w), ref(w)});
    assign(fmt, a, mk(1));
    sys(fmt, IK::Display, "F1 [%b] [%d]", {ref(a), ref(a)});
    assign(fmt, a, allx(1));
    sys(fmt, IK::Display, "F1x [%b] [%d]", {ref(a), ref(a)});
    assign(fmt, a, allz(1));
    sys(fmt, IK::Display, "F1z [%b] [%d]", {ref(a), ref(a)});
    sys(fmt, IK::Display, "Flit hello");
    sys(fmt, IK::Display, "Fpct %%");

    // a=0; a<=1; strobe, display, strobe. Display is 0; both strobes are 1.
    static Prog c1;
    delay(c1, 10);
    assign(c1, a, mk(0));
    nba(c1, a, mk(1));
    sys(c1, IK::Strobe, "C1S1 %b", {ref(a)});
    sys(c1, IK::Display, "C1D %b", {ref(a)});
    sys(c1, IK::Strobe, "C1S2 %b", {ref(a)});

    // $display after the NBA statement, still in this process, still pre-NBA.
    static Prog c3;
    delay(c3, 20);
    assign(c3, a, mk(0));
    nba(c3, a, mk(1));
    sys(c3, IK::Display, "C3D %b", {ref(a)});
    assign(c3, a, mk(0));
    sys(c3, IK::Display, "C3D2 %b", {ref(a)});
    sys(c3, IK::Strobe, "C3S %b", {ref(a)});

    // #0 writes 1111 before NBA commits 0101. Strobe sees 0101.
    static Prog wh;
    delay(wh, 25);
    assign(wh, w, mk(0, 4));
    nba(wh, w, mk(0b0101, 4));
    sys(wh, IK::Strobe, "WS %b", {ref(w)});
    static Prog wh0;
    delay(wh0, 25);
    delay(wh0, 0);
    sys(wh0, IK::Display, "WH %b", {ref(w)});
    assign(wh0, w, mk(0b1111, 4));
    sys(wh0, IK::Display, "WH2 %b", {ref(w)});

    // $monitor: one line, post-NBA, even though #0 runs first and writes b.
    static Prog mon;
    delay(mon, 30);
    sys(mon, IK::Monitor, "M %b %b", {ref(a), ref(b)});
    assign(mon, a, mk(0));
    assign(mon, b, mk(0));
    nba(mon, a, mk(1));
    sys(mon, IK::Strobe, "C4S %b", {ref(a)});
    static Prog mon0;
    delay(mon0, 30);
    delay(mon0, 0);
    sys(mon0, IK::Display, "C4H %b", {ref(a)});
    assign(mon0, a, mk(0));
    assign(mon0, b, mk(1));

    static Prog t40;
    delay(t40, 40);
    sys(t40, IK::Display, "T40");  // no watched change: no monitor line

    // Changed and changed back in the slot: still one line (final value).
    static Prog t50;
    delay(t50, 50);
    assign(t50, a, mk(0));
    assign(t50, a, mk(1));

    static Prog t60;
    delay(t60, 60);
    assign(t60, c, mk(1));         // c is not watched
    sys(t60, IK::Display, "T60");

    static Prog t70;
    delay(t70, 70);
    assign(t70, a, mk(1));         // same value: not a change
    sys(t70, IK::Display, "T70");

    // The NBA is what changes a. Display still sees 1; strobe was queued
    // before the change, so it prints before the monitor line. Both see 0.
    static Prog t80;
    delay(t80, 80);
    nba(t80, a, mk(0));
    assign(t80, a, mk(1));
    sys(t80, IK::Display, "C80D %b", {ref(a)});
    sys(t80, IK::Strobe, "C80S %b", {ref(a)});

    // New $monitor replaces the old one and is queued after the strobe.
    static Prog t90;
    delay(t90, 90);
    sys(t90, IK::Strobe, "C90S %b", {ref(b)});
    sys(t90, IK::Monitor, "M2 %b", {ref(b)});
    nba(t90, b, mk(0));

    // $finish in active. Postponed lines already queued still print the
    // post-NBA value. The statement after $finish does not run. The other
    // process runs its assign and its $display, then stops, so b stays 1
    // and FOTHER2 is absent. #101 does not run.
    static Prog fin;
    delay(fin, 100);
    assign(fin, a, mk(0));
    nba(fin, a, mk(1));
    sys(fin, IK::Display, "FD %b", {ref(a)});
    sys(fin, IK::Strobe, "FS %b", {ref(a)});
    sys(fin, IK::Strobe, "FB %b", {ref(b)});
    sys(fin, IK::Monitor, "MF %b", {ref(a)});
    finish(fin);
    sys(fin, IK::Display, "FAFTER");
    static Prog fother;
    delay(fother, 100);
    assign(fother, b, mk(1));
    sys(fother, IK::Display, "FOTHER %b", {ref(b)});
    assign(fother, b, mk(0));
    sys(fother, IK::Display, "FOTHER2");
    static Prog later;
    delay(later, 101);
    sys(later, IK::Display, "LATER");

    S.spawn(exec(&fmt));
    S.spawn(exec(&c1));
    S.spawn(exec(&c3));
    S.spawn(exec(&wh));
    S.spawn(exec(&wh0));
    S.spawn(exec(&mon));
    S.spawn(exec(&mon0));
    S.spawn(exec(&t40));
    S.spawn(exec(&t50));
    S.spawn(exec(&t60));
    S.spawn(exec(&t70));
    S.spawn(exec(&t80));
    S.spawn(exec(&t90));
    S.spawn(exec(&fin));
    S.spawn(exec(&fother));
    S.spawn(exec(&later));

    std::stringstream buf;
    std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
    S.run(1000);
    std::cout.rdbuf(old);

    const std::vector<std::string> want = {
        "F4b [0010]",
        "F4d [ 2]",
        "Fx [xxxx] [ x]",
        "Fz [zzzz] [ z]",
        "Fmx [xx01] [ X]",
        "Fmz [zz01] [ Z]",
        "F1 [1] [1]",
        "F1x [x] [x]",
        "F1z [z] [z]",
        "Flit hello",
        "Fpct %",
        "C1D 0",
        "C1S1 1",
        "C1S2 1",
        "C3D 0",
        "C3D2 0",
        "C3S 1",
        "WH 0000",
        "WH2 1111",
        "WS 0101",
        "C4H 0",
        "M 1 1",
        "C4S 1",
        "T40",
        "M 1 1",
        "T60",
        "T70",
        "C80D 1",
        "C80S 0",
        "M 0 1",
        "C90S 0",
        "M2 0",
        "FD 0",
        "FOTHER 1",
        "FS 1",
        "FB 1",
        "MF 1",
    };
    auto got = linesOf(buf.str());
    if (got != want) {
        std::cerr << "FAIL system-task lines\n--- got ---\n";
        for (auto& s : got) std::cerr << s << "\n";
        std::cerr << "--- want ---\n";
        for (auto& s : want) std::cerr << s << "\n";
        return 1;
    }
    if (S.now != 100 || !S.finished) {
        std::cerr << "FAIL $finish stopped at t=" << S.now << " finished=" << S.finished << "\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
