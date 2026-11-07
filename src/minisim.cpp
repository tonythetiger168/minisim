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
// $display           -> print in the region the process is running in
// $strobe            -> postponed region, after NBA
// $monitor           -> one postponed line per slot if a watched value changed
// $finish            -> S.finished = true
// task/function      -> Call / Ret in this coroutine (exec.inc)
#include <algorithm>
#include <cctype>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
using namespace std;
using Handle = coroutine_handle<>;

#include "prog.hpp"
struct Sig { string name; int w; Val val; vector<Drv> drv; bool net; };  // drv: one contribution per driver; net: wire, else reg
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
    vector<NbaItem> nbaq, nbaRun;                    // NBA region; nbaRun is the batch being applied
    // Postponed region, after NBA and before time advances. `mon` marks the
    // single $monitor slot; anything else is a $strobe, in schedule order.
    struct Post { bool mon = false; string fmt; vector<E> args; };
    deque<Post> postq;
    struct Mon {
        bool on = false, queued = false;
        string fmt;
        vector<E> args;
        vector<int> watch;
    } mon;
    map<uint64_t, vector<Handle>> wheel;             // time wheel
    uint64_t now = 0, lastDump = ~0ULL;
    bool finished = false;
    string vcdPath = "dump.vcd";                     // empty: no waveform
    ofstream vcd;

    ~Sim() { for (auto h : all) h.destroy(); }

    // A wire (`net`) starts as Z, including before the first drive().
    // A variable (reg/logic) starts as X. The flag is stored so the VCD
    // header can emit `$var wire` or `$var reg`.
    int sig(const string& n, int w, bool net = false) {
        sigs.push_back({n, w, net ? allz(w) : Val{}, {}, net});
        waiters.emplace_back();
        return (int)sigs.size() - 1;
    }
    void spawn(Task t) { all.push_back(t.h); wheel[now].push_back(t.h); }
    // Resolved nets: each continuous driver owns a slot (starts as Z).
    // s0/s1 are that driver's strength spec. Omitted means strong0 and strong1,
    // which is the default for a continuous assign with no strength written.
    int newDriver(int s) {
        sigs[s].drv.push_back(Drv{allz(sigs[s].w), STR_STRONG, STR_STRONG});
        return (int)sigs[s].drv.size() - 1;
    }
    void drive(int s, int d, Val v, int s0 = STR_STRONG, int s1 = STR_STRONG) {
        auto& g = sigs[s];
        g.drv[d].v = make(v.v, v.x, g.w, v.z);
        g.drv[d].s0 = (uint8_t)s0;
        g.drv[d].s1 = (uint8_t)s1;
        // One driver that drives both 0 and 1 resolves to its own value
        // (X stays X, Z stays Z). Skip the per-bit strength walk.
        if (g.drv.size() == 1 && s0 != STR_HIZ && s1 != STR_HIZ) { set(s, g.drv[0].v); return; }
        set(s, resolveDrives(g.drv.data(), g.drv.size(), g.w));
    }
    Val get(int s) const { return make(sigs[s].val.v, sigs[s].val.x, sigs[s].w, sigs[s].val.z); }
    void nba(int s, Val v, int hi = -1, int lo = 0) { nbaq.push_back({s, v, hi, lo}); }
    static int bit0(const Val& v) { return (v.x & 1) ? 2 : (int)(v.v & 1); }

    // Format a $display / $write line (IEEE 1800 21.2.1).
    //   %b %o %h/%x  full width, MSB first. A digit whose bits are all x is x,
    //                all z is z; a digit that is only partly unknown is X / Z.
    //   %d           right-justified to the width of the largest value (as
    //                Icarus does). A signed value prints with a minus sign.
    //                An all-X value is "x", all-Z is "z", partly unknown "X"/"Z".
    //   %c           the low 8 bits as a character
    //   %s           the value as 8-bit characters, leading zero bytes dropped
    //   %t           a time, right-justified to 20 columns
    //   %m           the scope name (minisim has one: the top module)
    //   %0<f>        no padding: %0d / %0t drop the spaces, %0b / %0h / %0o
    //                drop leading zeros. %<n><f> pads to at least n columns.
    //   %%           a percent sign
    // An argument without a matching specifier is ignored, and a specifier
    // without an argument is printed as written.
    static string fmtVal(char c, Val v, int minW, bool zero) {
        auto digits = [&](int bits) {
            int n = (v.w + bits - 1) / bits;
            string s(n, '0');
            for (int d = 0; d < n; d++) {
                int lo = d * bits, hi = std::min(lo + bits, v.w);
                uint64_t m = mask(hi - lo) << lo;
                uint64_t x = v.x & m, z = v.z & m;
                char ch;
                if (z == m) ch = 'z';
                else if (x == m && !z) ch = 'x';
                else if (z) ch = 'Z';
                else if (x) ch = 'X';
                else ch = "0123456789abcdef"[(v.v & m) >> lo];
                s[n - 1 - d] = ch;
            }
            if (zero) {
                size_t k = s.find_first_not_of('0');
                s = k == string::npos ? "0" : s.substr(k);
            }
            return s;
        };
        string body;
        bool pad = true;   // right-justify %d / %t to their natural width
        int natural = 0;
        switch (c) {
            case 'b': body = digits(1); pad = false; break;
            case 'o': body = digits(3); pad = false; break;
            case 'h': case 'x': body = digits(4); pad = false; break;
            case 'c': body = string(1, char(v.v & 0xff)); pad = false; break;
            case 's': {
                for (int b = (v.w + 7) / 8 - 1; b >= 0; b--) {
                    char ch = char((v.v >> (8 * b)) & 0xff);
                    if (ch || !body.empty()) body += ch;
                }
                pad = false;
                break;
            }
            case 't':
            case 'd': {
                uint64_t m = mask(v.w);
                int nx = __builtin_popcountll(v.x & ~v.z & m);
                int nz = __builtin_popcountll(v.z & m);
                bool neg = v.sgn && v.w > 0 && !v.x && ((v.v >> (v.w - 1)) & 1);
                if (nx == v.w) body = "x";
                else if (nx) body = "X";
                else if (nz == v.w) body = "z";
                else if (nz) body = "Z";
                else if (neg) {
                    uint64_t mag = (~v.v + 1) & m;
                    if (v.w == 64) mag = ~v.v + 1;
                    body = "-" + to_string(mag);
                } else body = to_string(v.v & m);
                if (c == 't') natural = 20;
                else {
                    natural = (int)((v.w * 146L + 484) / 485);
                    if (v.sgn) natural++;
                    if (natural < 1) natural = 1;
                }
                break;
            }
        }
        int width = zero ? 0 : (minW > 0 ? minW : (pad ? natural : 0));
        if ((int)body.size() < width) body = string(width - body.size(), ' ') + body;
        return body;
    }
    void printSys(const string& fmt, const vector<E>& args, bool newline = true) {
        size_t ai = 0;
        for (size_t i = 0; i < fmt.size(); ++i) {
            char c = fmt[i];
            if (c != '%') { cout << c; continue; }
            size_t start = i;
            if (i + 1 >= fmt.size()) { cout << '%'; break; }
            c = fmt[++i];
            if (c == '%') { cout << '%'; continue; }
            bool zero = false;
            int minW = 0;
            if (c == '0') { zero = true; if (i + 1 < fmt.size()) c = fmt[++i]; }
            while (isdigit((unsigned char)c) && i + 1 < fmt.size()) { minW = minW * 10 + (c - '0'); c = fmt[++i]; }
            char lc = (char)tolower((unsigned char)c);
            if (lc == 'm') { cout << "top"; continue; }
            bool known = string_view("bohxdcst").find(lc) != string_view::npos;
            if (!known || ai >= args.size()) { cout << fmt.substr(start, i - start + 1); continue; }
            Val v = eval(args[ai++], [&](int s) { return get(s); });
            cout << fmtVal(lc, v, minW, zero);
        }
        if (newline) cout << '\n';
    }
    void strobe(const string& fmt, const vector<E>& args) { postq.push_back({false, fmt, args}); }
    void queueMon() {
        if (!mon.on || mon.queued) return;
        postq.push_back({true, mon.fmt, mon.args});
        mon.queued = true;
    }
    // A new $monitor cancels the previous one. The call itself schedules a
    // print, like $strobe; a later change in this slot does not schedule another.
    void armMon(const string& fmt, const vector<E>& args) {
        if (mon.queued) {
            for (auto it = postq.begin(); it != postq.end(); ++it)
                if (it->mon) { postq.erase(it); break; }
            mon.queued = false;
        }
        mon.on = true;
        mon.fmt = fmt;
        mon.args = args;
        mon.watch.clear();
        for (const auto& a : args) collectRefs(a, mon.watch);
        queueMon();
    }
    void noteChange(int s) {
        if (!mon.on || mon.queued) return;
        for (int w : mon.watch) if (w == s) { queueMon(); return; }
    }
    void drainPost() {
        while (!postq.empty()) {
            Post p = move(postq.front());
            postq.pop_front();
            if (p.mon) mon.queued = false;
            printSys(p.fmt, p.args);
        }
    }

    // One value line, no timestamp. Shared so $dumpvars and later changes match.
    void writeVal(int s) {
        const Sig& g = sigs[s];
        string id(1, char('!' + s));
        if (g.w == 1) { vcd << ((g.val.x & 1) ? ((g.val.z & 1) ? 'z' : 'x') : char('0' + (g.val.v & 1))) << id << "\n"; return; }
        vcd << "b";
        for (int i = g.w - 1; i >= 0; i--)
            vcd << ((g.val.x >> i & 1) ? ((g.val.z >> i & 1) ? 'z' : 'x') : char('0' + (g.val.v >> i & 1)));
        vcd << " " << id << "\n";
    }
    void dump(int s) {
        if (!vcd.is_open() || !vcd) return;
        if (lastDump != now) { vcd << "#" << now << "\n"; lastDump = now; }
        writeVal(s);
    }
    // set() only writes a line when a value changes, so a signal still holding
    // its starting value (cnt stays X until the first clock) never appears.
    // Checkpoint every signal at time 0, before the first event. lastDump is
    // left alone so the change records that follow stay byte-for-byte the same.
    void dumpvars() {
        if (!vcd.is_open() || !vcd) return;
        vcd << "$dumpvars\n";
        for (size_t i = 0; i < sigs.size(); i++) writeVal((int)i);
        vcd << "$end\n";
    }

    void set(int s, Val nv) {
        uint64_t m = sigs[s].w >= 64 ? ~0ULL : ((1ULL << sigs[s].w) - 1);
        nv.v &= m; nv.x &= m; nv.z &= m; nv.sgn = false;
        if (nv == sigs[s].val) return;
        int o = bit0(sigs[s].val), n = bit0(nv);
        sigs[s].val = nv;
        dump(s);
        noteChange(s);  // $monitor: one postponed line if a watched signal changed
        bool pos = (o != 1 && n == 1) || (o == 0 && n != 0);
        bool neg = (o != 0 && n == 0) || (o == 1 && n != 1);
        // Wake matching waiters and compact the rest in place (no allocation).
        auto& ws = waiters[s];
        size_t keep = 0;
        for (size_t j = 0; j < ws.size(); j++) {
            Waiter& w = ws[j];
            if (w.fired && *w.fired) continue;  // stale: sibling event already woke it
            if (w.e == ANY || (w.e == POS && pos) || (w.e == NEG && neg)) {
                if (w.fired) *w.fired = true;
                active.push_back(w.h);
            } else {
                if (keep != j) ws[keep] = std::move(w);
                keep++;
            }
        }
        ws.erase(ws.begin() + (ptrdiff_t)keep, ws.end());
    }

    void header() {
        if (vcdPath.empty()) return;
        vcd.open(vcdPath);
        if (!vcd) { cerr << "cannot write " << vcdPath << "\n"; return; }
        vcd << "$timescale 1ns $end\n$scope module top $end\n";
        for (size_t i = 0; i < sigs.size(); i++)
            vcd << "$var " << (sigs[i].net ? "wire" : "reg") << " " << sigs[i].w << " " << char('!' + i) << " " << sigs[i].name << " $end\n";
        vcd << "$upscope $end\n$enddefinitions $end\n";
    }

    void run(uint64_t end) {
        header();
        dumpvars();
        while (!wheel.empty() && !finished) {
            auto it = wheel.begin();
            if (it->first > end) break;
            now = it->first;
            g_simTime = now;
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
                    // Swap, not move: both buffers keep their capacity across slots.
                    nbaRun.clear();
                    nbaRun.swap(nbaq);
                    // Partial writes merge into the value as it is *now*, so two NBAs to
                    // different bits of one reg in the same slot both take effect.
                    for (auto& n : nbaRun) set(n.s, n.hi >= 0 ? v_insert(get(n.s), n.v, n.hi, n.lo) : n.v);
                } else if (!postq.empty()) {                   // postponed ($strobe/$monitor)
                    drainPost();                               // then, and only then, time advances
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
// `ev` points at the instruction's event list, which outlives the wait.
struct WaitAny {
    const vector<pair<int, Edge>>* ev;
    bool await_ready() const { return false; }
    void await_suspend(Handle h) const {
        auto token = make_shared<bool>(false);
        for (auto& [sg, e] : *ev) S.waiters[sg].push_back({h, e, token});
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

#ifndef MINISIM_NO_MAIN
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
#endif
