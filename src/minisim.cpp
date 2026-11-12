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
#include <stdexcept>
#include <exception>
#include <limits>
#include <functional>
#include <utility>
#include <vector>
using namespace std;
using Handle = coroutine_handle<>;

#include "prog.hpp"
#include "coverage.hpp"
struct Sig { string name; int w; Val val; vector<Val> drv; };  // drv: one value per driver
// `fired` is shared by all waiters of one multi-event wait: the first hit sets it,
// the others become stale and are dropped. Null for a plain single-event wait.
struct Waiter { Handle h; Edge e; shared_ptr<bool> fired; };

// A process: a coroutine that starts suspended and is owned by the Sim.
struct Task {
    struct promise_type {
        exception_ptr error;
        Task get_return_object() { return Task{coroutine_handle<promise_type>::from_promise(*this)}; }
        suspend_always initial_suspend() noexcept { return {}; }
        suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() noexcept { error = current_exception(); }
    };
    coroutine_handle<promise_type> h;
};

struct Sim {
    bool collect_coverage = false;
    CoverageGroup line_coverage, toggle_coverage;
    vector<Sig> sigs;
    vector<vector<Waiter>> waiters;                  // dynamic sensitivity
    vector<Handle> all;                              // owned coroutines
    deque<Handle> active, inactive;                  // active / inactive regions
    struct NbaItem { int s; Val v; int hi, lo; };    // hi < 0: whole signal
    vector<NbaItem> nbaq;                     // NBA region
    map<uint64_t, vector<Handle>> wheel;             // time wheel
    uint64_t now = 0, lastDump = ~0ULL;
    bool finished = false;
    uint64_t max_resumes_per_slot = 1000000; // positive, configurable; not wall-clock timeout
    uint64_t max_ir_instructions_per_resume = 1000000;
    string execution_error;
    bool header_written = false, resume_slot_valid = false;
    uint64_t resume_slot = 0, slot_resumes = 0;
    enum class Phase { Idle, ValueChange, ReadWrite, ReadOnly };
    Phase phase = Phase::Idle;
    struct Callback { function<void()> fn; int signal = -1; Edge edge = ANY; };
    map<uint64_t, Callback> callbacks;
    map<uint64_t, vector<uint64_t>> callback_timers;
    deque<uint64_t> value_callbacks, rw_callbacks, ro_callbacks;
    uint64_t next_callback = 1, callback_count = 0;
    uint64_t max_callbacks_per_slot = 100000;
    bool readonly_closed = false, running = false;
    uint64_t readonly_time = 0;

    void writable() const {
        if (phase == Phase::ReadOnly || (readonly_closed && readonly_time == now))
            throw runtime_error("write forbidden in ReadOnly at time " + to_string(now));
    }
    uint64_t add_callback(function<void()> fn, int signal = -1, Edge edge = ANY) {
        if (!fn || next_callback == numeric_limits<uint64_t>::max())
            throw invalid_argument("invalid callback or exhausted callback IDs");
        auto id = next_callback++;
        callbacks.emplace(id, Callback{move(fn),signal,edge}); return id;
    }
    uint64_t on_edge(int signal, Edge edge, function<void()> fn) {
        if (signal < 0 || size_t(signal) >= sigs.size() || (edge != ANY && edge != POS && edge != NEG))
            throw invalid_argument("invalid edge callback signal or edge");
        return add_callback(move(fn),signal,edge);
    }
    uint64_t on_timer(uint64_t delay, function<void()> fn) {
        if (delay == 0 || delay > numeric_limits<uint64_t>::max()-now)
            throw invalid_argument("timer requires positive non-overflowing delay");
        auto id = add_callback(move(fn)); callback_timers[now+delay].push_back(id); return id;
    }
    uint64_t on_readwrite(function<void()> fn) {
        writable(); auto id=add_callback(move(fn)); rw_callbacks.push_back(id); return id;
    }
    uint64_t on_readonly(function<void()> fn) {
        writable(); auto id=add_callback(move(fn)); ro_callbacks.push_back(id); return id;
    }
    bool cancel_callback(uint64_t id) { return callbacks.erase(id) != 0; }
    void dispatch_callback(deque<uint64_t>& queue, Phase target) {
        auto id=queue.front(); queue.pop_front(); auto it=callbacks.find(id);
        if (it==callbacks.end()) return;
        if (callback_count == max_callbacks_per_slot) {
            finished=true; throw runtime_error("callback budget exceeded at time " + to_string(now));
        }
        ++callback_count;
        auto fn=move(it->second.fn); callbacks.erase(it); // one-shot; rearm explicitly
        phase=target;
        try { fn(); } catch (...) { phase=Phase::Idle; finished=true; throw; }
        phase=Phase::Idle;
        if (target==Phase::ReadOnly) { readonly_closed=true; readonly_time=now; }
    }
    ofstream vcd;

    ~Sim() { for (auto h : all) h.destroy(); }

    int sig(const string& n, int w) {
        if (w < 1 || w > 64) throw invalid_argument("signal width must be 1..64");
        if (collect_coverage)
            for (int bit = 0; bit < w; ++bit) {
                auto key = "sig:" + to_string(sigs.size()) + ":bit:" + to_string(bit);
                toggle_coverage.add(key + ":rise");
                toggle_coverage.add(key + ":fall");
            }
        sigs.push_back({n, w, Val{}});
        waiters.emplace_back();
        return (int)sigs.size() - 1;
    }
    void spawn(Task t) {
        writable(); all.push_back(t.h);
        if (running) active.push_back(t.h); else wheel[now].push_back(t.h);
    }
    // Resolved nets: each continuous assign owns a driver slot (starts as Z).
    int newDriver(int s) { sigs[s].drv.push_back(allz(sigs[s].w)); return (int)sigs[s].drv.size() - 1; }
    void drive(int s, int d, Val v) {
        writable();
        auto& g = sigs[s];
        g.drv[d] = make(v.v, v.x, g.w, v.z);
        Val r = g.drv[0];
        for (size_t i = 1; i < g.drv.size(); i++) r = v_resolve(r, g.drv[i]);
        set(s, r);
    }
    Val get(int s) const { return make(sigs[s].val.v, sigs[s].val.x, sigs[s].w, sigs[s].val.z); }
    void nba(int s, Val v, int hi = -1, int lo = 0) { writable(); nbaq.push_back({s, v, hi, lo}); }
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
        writable();
        uint64_t m = sigs[s].w >= 64 ? ~0ULL : ((1ULL << sigs[s].w) - 1);
        nv.v &= m; nv.x &= m; nv.z &= m;
        if (nv == sigs[s].val) return;
        if (collect_coverage) {
            const auto& old = sigs[s].val;
            uint64_t known = ~(old.x | nv.x) & m;
            uint64_t rises = ~old.v & nv.v & known;
            uint64_t falls = old.v & ~nv.v & known;
            for (int bit = 0; bit < sigs[s].w; ++bit) {
                auto key = "sig:" + to_string(s) + ":bit:" + to_string(bit);
                if ((rises >> bit) & 1) toggle_coverage.hit(key + ":rise");
                if ((falls >> bit) & 1) toggle_coverage.hit(key + ":fall");
            }
        }
        int o = bit0(sigs[s].val), n = bit0(nv);
        sigs[s].val = nv;
        dump(s);
        bool pos = (o != 1 && n == 1) || (o == 0 && n != 0);
        bool neg = (o != 0 && n == 0) || (o == 1 && n != 1);
        for (const auto& [id, cb]:callbacks)
            if (cb.signal==s && (cb.edge==ANY || (cb.edge==POS && pos) || (cb.edge==NEG && neg)))
                value_callbacks.push_back(id);
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
        if (header_written) return;
        header_written = true;
        vcd.open("dump.vcd");
        vcd << "$timescale 1ns $end\n$scope module top $end\n";
        for (size_t i = 0; i < sigs.size(); i++)
            vcd << "$var wire " << sigs[i].w << " " << char('!' + i) << " " << sigs[i].name << " $end\n";
        vcd << "$upscope $end\n$enddefinitions $end\n";
        // Unchanged signals still need an initial value in the waveform.
        for (size_t i = 0; i < sigs.size(); ++i) dump((int)i);
    }

    void run(uint64_t end) {
        if (running) throw runtime_error("reentrant simulation advance forbidden");
        struct Guard { bool& flag; Guard(bool& f):flag(f){flag=true;} ~Guard(){flag=false;} } guard(running);
        if (max_resumes_per_slot == 0) throw runtime_error("max_resumes_per_slot must be positive");
        if (max_ir_instructions_per_resume == 0) throw runtime_error("max_ir_instructions_per_resume must be positive");
        if (max_callbacks_per_slot == 0) throw runtime_error("max_callbacks_per_slot must be positive");
        header();
        while (!finished) {
            auto it = wheel.begin();
            bool pending = !active.empty() || !inactive.empty() || !nbaq.empty() ||
                           !value_callbacks.empty() || !rw_callbacks.empty() || !ro_callbacks.empty();
            if (!pending) {
                if (it == wheel.end() && callback_timers.empty()) break;
                uint64_t next = it == wheel.end() ? callback_timers.begin()->first : it->first;
                if (!callback_timers.empty()) next=min(next,callback_timers.begin()->first);
                if (next > end) break;
                now = next;
            }
            auto timer=callback_timers.find(now);
            if (timer!=callback_timers.end()) {
                for (auto id:timer->second) value_callbacks.push_back(id);
                callback_timers.erase(timer);
            }
            if (it != wheel.end() && it->first == now) {
                for (auto h : it->second) active.push_back(h);
                wheel.erase(it);
            }
            if (!resume_slot_valid || resume_slot != now) {
                resume_slot_valid = true; resume_slot = now; slot_resumes = 0; callback_count=0;
            }
            while (!finished) {                                // one time slot
                if (!value_callbacks.empty()) {
                    dispatch_callback(value_callbacks,Phase::ValueChange);
                } else if (!active.empty()) {                         // active
                    Handle h = active.front(); active.pop_front();
                    if (!h.done()) {
                        if (slot_resumes == max_resumes_per_slot) {
                            finished = true;
                            throw runtime_error("process resume budget exceeded at time " + to_string(now));
                        }
                        ++slot_resumes;
                        h.resume();
                        auto typed = coroutine_handle<Task::promise_type>::from_address(h.address());
                        if (typed.promise().error) {
                            finished = true;
                            try { rethrow_exception(typed.promise().error); }
                            catch (const exception& error) {
                                throw runtime_error("process error at time " + to_string(now) + ": " + error.what());
                            } catch (...) {
                                throw runtime_error("process error at time " + to_string(now) + ": unknown exception");
                            }
                        }
                        if (!execution_error.empty()) throw runtime_error(execution_error);
                    }
                } else if (!inactive.empty()) {                // inactive (#0)
                    for (auto h : inactive) active.push_back(h);
                    inactive.clear();
                } else if (!nbaq.empty()) {                    // NBA
                    auto q = move(nbaq); nbaq.clear();
                    // Partial writes merge into the value as it is *now*, so two NBAs to
                    // different bits of one reg in the same slot both take effect.
                    for (auto& n : q) set(n.s, n.hi >= 0 ? v_insert(get(n.s), n.v, n.hi, n.lo) : n.v);
                } else if (!rw_callbacks.empty()) {
                    dispatch_callback(rw_callbacks,Phase::ReadWrite);
                } else if (!ro_callbacks.empty()) {
                    dispatch_callback(ro_callbacks,Phase::ReadOnly);
                } else break;
            }
        }
    }
    // External driver: settle current reactions/NBAs, process timers through end,
    // then advance an idle kernel to end. Reads after return see settled values.
    void advance_to(uint64_t end) {
        if (end < now) throw invalid_argument("cannot advance simulation backwards");
        run(end);
        if (!finished) now = end;
    }
};

Sim S;

// ---- awaiters ----
struct Delay {
    uint64_t n;
    bool await_ready() const { return false; }
    void await_suspend(Handle h) const {
        if (n == 0) S.inactive.push_back(h);
        else {
            if (n > numeric_limits<uint64_t>::max() - S.now)
                throw overflow_error("delay overflow");
            S.wheel[S.now + n].push_back(h);
        }
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

#ifndef MINISIM_NO_DEMO
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
#endif
