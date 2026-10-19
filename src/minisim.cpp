// minisim.cpp - event-driven HDL simulator kernel with C++20 coroutines
// Build: g++ -std=c++20 -O2 minisim.cpp -o minisim && ./minisim
// View:  gtkwave dump.vcd
//
// Verilog            -> here
// initial begin..end -> Task with straight-line code
// always @(posedge c)-> Task with for(;;){ co_await Wait{S,c,POS}; ... }
// #10                -> co_await Delay{S,10}   (#0 goes to the inactive region)
// @(sig)             -> co_await Wait{S,sig,ANY}
// a <= b             -> S.nba(a, b)
// $finish            -> S.finished = true
#include <coroutine>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
using namespace std;
using Handle = coroutine_handle<>;

#include "prog.hpp"
struct Sig { string name; int w; Val val; vector<Val> drv; };  // drv: one value per driver
// `fired` is shared by all waiters of one multi-event wait: the first hit sets it,
// the others become stale and are dropped. Null for a plain single-event wait.
struct Waiter { Handle h; Edge e; shared_ptr<bool> fired; };

// A process: a coroutine that starts suspended and is owned by the Sim.
struct Task {
    struct promise_type {
        Task get_return_object() { return Task{coroutine_handle<promise_type>::from_promise(*this)}; }
        suspend_always initial_suspend() noexcept { return {}; }
        suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { terminate(); }
    };
    coroutine_handle<promise_type> h;
};

struct Sim {
    vector<Sig> sigs;
    vector<vector<Waiter>> waiters;                  // dynamic sensitivity
    vector<Handle> all;                              // owned coroutines
    deque<Handle> active, inactive;                  // active / inactive regions
    struct NbaItem { int s; Val v; int hi, lo; };    // hi < 0: whole signal
    vector<NbaItem> nbaq;                     // NBA region
    map<uint64_t, vector<Handle>> wheel;             // time wheel
    uint64_t now = 0, lastDump = ~0ULL;
    bool finished = false;
    ofstream vcd;

    ~Sim() { for (auto h : all) h.destroy(); }

    int sig(const string& n, int w) {
        sigs.push_back({n, w, Val{}});
        waiters.emplace_back();
        return (int)sigs.size() - 1;
    }
    void spawn(Task t) { all.push_back(t.h); wheel[now].push_back(t.h); }
    // Resolved nets: each continuous assign owns a driver slot (starts as Z).
    int newDriver(int s) { sigs[s].drv.push_back(allz(sigs[s].w)); return (int)sigs[s].drv.size() - 1; }
    void drive(int s, int d, Val v) {
        auto& g = sigs[s];
        g.drv[d] = make(v.v, v.x, g.w, v.z);
        Val r = g.drv[0];
        for (size_t i = 1; i < g.drv.size(); i++) r = v_resolve(r, g.drv[i]);
        set(s, r);
    }
    Val get(int s) const { return make(sigs[s].val.v, sigs[s].val.x, sigs[s].w, sigs[s].val.z); }
    void nba(int s, Val v, int hi = -1, int lo = 0) { nbaq.push_back({s, v, hi, lo}); }
    static int bit0(const Val& v) { return (v.x & 1) ? 2 : (int)(v.v & 1); }

    void dump(int s) {
        if (!vcd) return;
        if (lastDump != now) { vcd << "#" << now << "\n"; lastDump = now; }
        const Sig& g = sigs[s];
        string id(1, char('!' + s));
        if (g.w == 1) { vcd << ((g.val.x & 1) ? ((g.val.z & 1) ? 'z' : 'x') : char('0' + (g.val.v & 1))) << id << "\n"; return; }
        vcd << "b";
        for (int i = g.w - 1; i >= 0; i--)
            vcd << ((g.val.x >> i & 1) ? ((g.val.z >> i & 1) ? 'z' : 'x') : char('0' + (g.val.v >> i & 1)));
        vcd << " " << id << "\n";
    }

    void set(int s, Val nv) {
        uint64_t m = sigs[s].w >= 64 ? ~0ULL : ((1ULL << sigs[s].w) - 1);
        nv.v &= m; nv.x &= m; nv.z &= m;
        if (nv == sigs[s].val) return;
        int o = bit0(sigs[s].val), n = bit0(nv);
        sigs[s].val = nv;
        dump(s);
        bool pos = (o != 1 && n == 1) || (o == 0 && n != 0);
        bool neg = (o != 0 && n == 0) || (o == 1 && n != 1);
        vector<Waiter> keep;
        for (auto& w : waiters[s]) {
            if (w.fired && *w.fired) continue;  // stale: sibling event already woke it
            if (w.e == ANY || (w.e == POS && pos) || (w.e == NEG && neg)) {
                if (w.fired) *w.fired = true;
                active.push_back(w.h);
            } else keep.push_back(w);
        }
        waiters[s].swap(keep);
    }

    void header() {
        vcd.open("dump.vcd");
        vcd << "$timescale 1ns $end\n$scope module top $end\n";
        for (size_t i = 0; i < sigs.size(); i++)
            vcd << "$var wire " << sigs[i].w << " " << char('!' + i) << " " << sigs[i].name << " $end\n";
        vcd << "$upscope $end\n$enddefinitions $end\n";
    }

    void run(uint64_t end) {
        header();
        while (!wheel.empty() && !finished) {
            auto it = wheel.begin();
            if (it->first > end) break;
            now = it->first;
            for (auto h : it->second) active.push_back(h);
            wheel.erase(it);
            for (;;) {                                         // one time slot
                if (!active.empty()) {                         // active
                    Handle h = active.front(); active.pop_front();
                    if (!h.done()) h.resume();
                } else if (!inactive.empty()) {                // inactive (#0)
                    for (auto h : inactive) active.push_back(h);
                    inactive.clear();
                } else if (!nbaq.empty()) {                    // NBA
                    auto q = move(nbaq); nbaq.clear();
                    // Partial writes merge into the value as it is *now*, so two NBAs to
                    // different bits of one reg in the same slot both take effect.
                    for (auto& n : q) set(n.s, n.hi >= 0 ? v_insert(get(n.s), n.v, n.hi, n.lo) : n.v);
                } else break;
            }
        }
    }
};

Sim S;

// ---- awaiters ----
struct Delay {
    uint64_t n;
    bool await_ready() const { return false; }
    void await_suspend(Handle h) const {
        if (n == 0) S.inactive.push_back(h);
        else S.wheel[S.now + n].push_back(h);
    }
    void await_resume() const {}
};
struct Wait {
    int sig; Edge e;
    bool await_ready() const { return false; }
    void await_suspend(Handle h) const { S.waiters[sig].push_back({h, e}); }
    void await_resume() const {}
};
// @(e1 or e2 or ...): resumes once, on whichever event happens first.
struct WaitAny {
    vector<pair<int, Edge>> ev;
    bool await_ready() const { return false; }
    void await_suspend(Handle h) const {
        auto token = make_shared<bool>(false);
        for (auto& [sg, e] : ev) S.waiters[sg].push_back({h, e, token});
    }
    void await_resume() const {}
};

#include "exec.inc"

// ---- the design + testbench ----
int clk, rst, cnt;

// always #5 clk = ~clk;
Task clockgen() {
    for (;;) {
        co_await Delay{5};
        S.set(clk, mk(!(S.get(clk).v & 1)));
    }
}
// always @(cnt) $display(...)
Task monitor() {
    for (;;) {
        co_await Wait{cnt, ANY};
        Val c = S.get(cnt);
        cout << "t=" << S.now << " cnt=";
        if (c.x & 0xF) cout << "x"; else cout << (c.v & 0xF);
        cout << "\n";
    }
}
// initial begin clk=0; rst=1; #22 rst=0; #100 $finish; end
Task stimulus() {
    S.set(clk, mk(0));
    S.set(rst, mk(1));
    co_await Delay{22};
    S.set(rst, mk(0));
    co_await Delay{100};
    cout << "$finish at t=" << S.now << "\n";
    S.finished = true;
}

int main() {
    clk = S.sig("clk", 1);
    rst = S.sig("rst", 1);
    cnt = S.sig("cnt", 4);
    // always @(posedge clk) if (rst) cnt <= 0; else cnt <= cnt + 1;   (lowered by hand)
    static Prog cp;
    auto I = [](IK k) { Instr i; i.k = k; return i; };
    { Instr i = I(IK::WaitEdge);  i.sig = clk; i.edge = POS; cp.emit(i); }          // 0
    { Instr i = I(IK::JmpIfNot);  i.e = ref(rst); i.target = 4; cp.emit(i); }       // 1
    { Instr i = I(IK::Nba);       i.sig = cnt; i.e = konst(mk(0, 4)); cp.emit(i); } // 2
    { Instr i = I(IK::Jmp);       i.target = 5; cp.emit(i); }                       // 3
    { Instr i = I(IK::Nba);       i.sig = cnt; i.e = bin(Op::Add, ref(cnt), konst(mk(1, 4))); cp.emit(i); } // 4
    { Instr i = I(IK::Jmp);       i.target = 0; cp.emit(i); }                       // 5 (always loops)
    S.spawn(exec(&cp));   // spawn waiters first so they're listening before clk moves
    S.spawn(monitor());
    S.spawn(clockgen());
    S.spawn(stimulus());
    S.run(1000);
}
