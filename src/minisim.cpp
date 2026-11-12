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
#include <array>
#include <cctype>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <atomic>
#include <array>
#include <set>
#include <bit>
#include <charconv>
#include <chrono>
#include <climits>
#include <functional>
#include <map>
#include <set>
#include <queue>
#include <sstream>
#include <memory>
#include <string>
#include <thread>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
using namespace std;
using Handle = coroutine_handle<>;

#include "prog.hpp"
#include "cpu.hpp"
#include "sva.hpp"
#include "sva_engine.hpp"
#include "clocking.hpp"
#include "cover.hpp"
#include "tchk.hpp"
#include "specify.hpp"
#include "sdf.hpp"
#ifdef MINISIM_FST
#include "fstapi.h"
#endif
struct Sig { string name; int w; Val val; vector<Drv> drv; bool net; bool hide = false; uint8_t kind = NR_WIRE; uint8_t fl = 0; uint8_t p0 = 2; uint64_t at = ~0ULL; };  // p0, at: bit 0 before the last change and its time (msim -xprop edges)  // fl: SF_HELD / SF_LISTEN (force, 10.6)  // hide: array element, kept out of the VCD  // drv: one contribution per driver; net: wire, else reg
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

// A small thread pool for Sim::parallelFor. Each batch is split into one
// contiguous range per thread (thread k takes items [n*k/T, n*(k+1)/T)), so a
// batch costs one wake-up and one counter update per thread, not per item,
// and neighbouring result slots are written by the same thread. Workers spin
// briefly between batches, then sleep on an atomic wait.
// Default worker count: the hardware threads, but at most 8. Measured on a
// 20-core / 28-thread i7-14700 (bench/wide.sv): 4 to 12 threads were fastest,
// 20 was slower than 8, and 24 or more was 5-50x slower, because one
// descheduled thread holds up every batch. -threads N still picks any count
// up to the hardware threads.
// Counting physical cores, not hardware threads: an SMT sibling shares its
// core's execution units, and on a hybrid part the E cores are slower and
// hold up every batch (cpu.hpp). So the default is one thread per P core
// (every core when not hybrid), at most 8; -threads N goes up to every
// logical CPU, siblings last.
inline int defaultThreads() {
    int n = cpuTopo().fastCores();
    return n < 1 ? 1 : (n > 8 ? 8 : n);
}
inline int maxThreads() { return std::max(1, cpuTopo().logical()); }
// Pin pool thread k to cpuTopo().order[k]: the same items of a batch then
// stay in the same core's cache, and N threads sit on N distinct physical
// cores. -1 (default): pin unless under a hypervisor, where a pinned thread
// waits for its virtual CPU (WSL2 on an i7-14700: 8 pinned threads 2.3 s,
// unpinned 0.8 s). msim -pin / -no-pin force it.
inline int g_pin = -1;
inline bool pinThreads() { return g_pin < 0 ? !cpuTopo().hypervisor : g_pin > 0; }

struct Pool {
    // Each thread starts on its own contiguous range of a batch (so the same
    // items keep landing on the same core's cache), takes it in chunks, and
    // then steals unclaimed chunks from the other ranges. A thread the OS has
    // descheduled (a busy desktop, a VM with as many threads as CPUs) then
    // holds up a batch only for the chunk it is in the middle of. With fixed
    // ranges and no stealing, a batch waited for every descheduled worker,
    // which made a loaded run 10x or more slower than one thread.
    //
    // A slot's claim packs the batch number (high 32 bits) with its next item
    // (low 32 bits), so a worker that wakes up late can never take an item of
    // a batch it did not see published.
    static constexpr int kMaxThreads = 64;
    using Job = void (*)(void*, size_t);
    struct alignas(64) Slot {
        atomic<uint64_t> claim{0};
        atomic<size_t> end{0}, chunk{1};
        // The batch this thread is called to. Only the threads a batch uses are
        // woken: one left out sleeps on, so a batch on 2 of 8 threads costs no
        // wake-up (a futex call, a context switch) of the other 6.
        // 32 bits: libstdc++ waits on a 4-byte atomic with a futex on its own
        // address. A 64-bit one shares 16 hashed buckets, and every notify
        // wakes the whole bucket: with 64-byte slots all gates hashed to one
        // bucket, so each wake-up woke every sleeping thread of the pool.
        atomic<uint32_t> gate{0};
        atomic<uint32_t> sleeping{0};   // the worker is in (or entering) gate.wait()
    };
    Slot slots[kMaxThreads];
    vector<thread> ts;
    atomic<uint64_t> gen{0};       // the last batch number (its low 32 bits)
    alignas(64) atomic<uint32_t> finished{0};   // items of the current batch that are done
    atomic<uint32_t> mainSleeping{0};
    atomic<size_t> total{0};       // items in the current batch
    Job job = nullptr;             // written before a batch is published, read after a claim
    void* ctx = nullptr;
    atomic<bool> stop{false};
    atomic<int> active{1};         // threads taking part in the current batch
    int size() const { return (int)ts.size() + 1; }
    bool pinned = false;
    static void pinSelf(int id) {
        const auto& o = cpuTopo().order;
        if (!o.empty()) pinToCpu(o[(size_t)id % o.size()]);
    }
    void ensure(int n) {
        n = std::min(n, kMaxThreads);
        if (n > 1 && !pinned && pinThreads()) { pinned = true; pinSelf(0); }
        while ((int)ts.size() < n - 1) {
            int id = (int)ts.size() + 1;
            bool pin = pinned;
            ts.emplace_back([this, id, pin] { if (pin) pinSelf(id); worker(id); });
        }
    }
    ~Pool() {
        stop = true;
        uint32_t g = (uint32_t)(gen.fetch_add(1) + 1);
        for (auto& sl : slots) { sl.gate.store(g); sl.gate.notify_all(); }
        for (auto& t : ts) t.join();
    }
    // Take chunks of slot s in batch b until it is empty. False once the
    // batch is over, so stealing can stop early.
    bool drain(Slot& s, uint32_t b) {
        uint64_t c = s.claim.load(memory_order_acquire);
        for (;;) {
            if ((uint32_t)(c >> 32) != b) return false;
            // Read after a claim of batch b was seen: a successful CAS below
            // proves batch b was still open, so these are b's values.
            size_t lo = (size_t)(uint32_t)c, n = s.end.load(memory_order_relaxed);
            if (lo >= n) return true;
            size_t hi = std::min(n, lo + s.chunk.load(memory_order_relaxed));
            if (!s.claim.compare_exchange_weak(c, ((uint64_t)b << 32) | hi,
                                               memory_order_acq_rel, memory_order_acquire)) continue;
            for (size_t i = lo; i < hi; i++) job(ctx, i);
            // The chunk that completes the batch wakes run() if it sleeps.
            if (finished.fetch_add((uint32_t)(hi - lo)) + (hi - lo) == total.load(memory_order_relaxed) &&
                mainSleeping.load())
                finished.notify_one();
            c = s.claim.load(memory_order_acquire);
        }
    }
    void work(int id, uint32_t b) {
        int T = active.load(memory_order_relaxed);
        if (id >= T) return;
        for (int k = 0; k < T; k++)
            if (!drain(slots[(id + k) % T], b)) return;
    }
    void worker(int id) {
        uint32_t seen = 0;
        Slot& me = slots[id];
        for (;;) {
            uint32_t g;
            int spins = 0;
            while ((g = me.gate.load(memory_order_acquire)) == seen) {
                // Spin for the next batch; yield, which lets any other
                // runnable thread have the CPU; then sleep.
                // A thread left out of the last batch sleeps at once, so it
                // does not spin on an SMT sibling of a thread that is working.
                if (id < active.load(memory_order_relaxed) && ++spins < 4000) relax();
                else if (id < active.load(memory_order_relaxed) && spins < 20000) this_thread::yield();
                else {
                    // Announce the sleep, then look again (both seq_cst): either
                    // run() sees `sleeping` and wakes us, or we see its gate.
                    me.sleeping.store(1);
                    if (me.gate.load() == seen) me.gate.wait(seen);
                    me.sleeping.store(0);
                }
            }
            seen = g;
            if (stop) return;
            work(id, (uint32_t)g);
        }
    }
    static void relax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
    // Run items [0, n) on the first `use` threads (0: all of them).
    void run(size_t n, Job j, void* c, int use = 0) {
        uint32_t b = (uint32_t)(gen.load(memory_order_relaxed) + 1);
        if (b == 0) b = 1;   // 0 is every gate's start value
        size_t T = (size_t)(use > 0 && use < size() ? use : size());
        active.store((int)T, memory_order_relaxed);
        job = j;
        ctx = c;
        total.store(n, memory_order_relaxed);
        finished.store(0, memory_order_relaxed);
        for (size_t t = 0; t < T; t++) {
            size_t lo = n * t / T, hi = n * (t + 1) / T;
            slots[t].end.store(hi, memory_order_relaxed);
            slots[t].chunk.store(std::max<size_t>(1, (hi - lo) / 4), memory_order_relaxed);
            slots[t].claim.store(((uint64_t)b << 32) | lo, memory_order_release);
        }
        gen.store(b, memory_order_release);   // the only writer while the pool runs
        for (size_t t = 1; t < T; t++) {
            slots[t].gate.store(b);
            if (slots[t].sleeping.load()) slots[t].gate.notify_one();   // no syscall for a spinning thread
        }
        work(0, b);
        // Wait for chunks other threads are still running: spin, yield, then sleep.
        for (int spins = 0;; spins++) {
            uint32_t d = finished.load(memory_order_acquire);
            if (d >= n) break;
            if (spins < 4000) relax();
            else if (spins < 20000) this_thread::yield();
            else {
                mainSleeping.store(1);
                if (finished.load() == d) finished.wait(d);
                mainSleeping.store(0);
            }
        }
    }
};

struct ProcCtl;
// A deferred assertion report waiting to mature (16.4.1): the assertion, the
// process that queued it and that process's flush count then, and the
// action's captured argument values.
struct DeferRep {
    int id;
    bool pass;
    shared_ptr<ProcCtl> ctl;
    uint64_t epoch, killGen;
    vector<pair<Expr*, Val>> caps;
};
// Event regions of one time slot (4.4.2), for the code that depends on them.
enum Region : uint8_t { R_ACTIVE, R_OBSERVED, R_REACTIVE, R_POSTPONED };
// The time wheel: pending times and the processes waiting for each. Kept as
// a vector sorted latest first, so the next time is at the back and taking it
// is a pop; emptied bucket vectors are kept for reuse. A std::map here cost
// two allocations and frees per time step (a clock toggling every few ticks
// is most of them).
struct Wheel {
    struct Bucket { uint64_t first; vector<Handle> second; };
    vector<Bucket> b;                 // descending by time
    vector<vector<Handle>> spare;
    vector<Handle>& operator[](uint64_t t) {
        // Most inserts are the latest time or already present: look from the front.
        size_t lo = 0, hi = b.size();
        while (lo < hi) {             // first index with time <= t
            size_t mid = (lo + hi) / 2;
            if (b[mid].first > t) lo = mid + 1; else hi = mid;
        }
        if (lo < b.size() && b[lo].first == t) return b[lo].second;
        vector<Handle> v;
        if (!spare.empty()) { v = std::move(spare.back()); spare.pop_back(); }
        b.insert(b.begin() + (ptrdiff_t)lo, Bucket{t, std::move(v)});
        return b[lo].second;
    }
    bool empty() const { return b.empty(); }
    Bucket* begin() { return &b.back(); }   // the earliest time
    void erase(Bucket* it) {                // only the earliest
        (void)it;
        b.back().second.clear();
        spare.push_back(std::move(b.back().second));
        b.pop_back();
    }
    void clear() { b.clear(); }
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
    // A postponed print: $strobe, $monitor (mon), or to a file with $fstrobe /
    // $fmonitor (file, fd; fmon is the $fmonitor's index).
    struct Post { bool mon = false; string fmt; vector<E> args; bool file = false; uint32_t fd = 0; int fmon = -1; };
    // $fmonitor (21.3.2): any number can be active at once; $fclose of the
    // descriptor ends one (on = false).
    struct FMon { bool on = true, queued = false; uint32_t fd; string fmt; vector<E> args; vector<int> watch; };
    vector<FMon> fmons;
    deque<Post> postq;
    // The reactive region set (4.4.2.6-4.4.2.8): reactive, re-inactive
    // (#0 from the reactive region) and re-NBA. Assertion action blocks run
    // here. `region` says which region the running code is in.
    deque<Handle> observedQ, reactive, reInactive;
    vector<NbaItem> reNbaq;
    Region region = R_ACTIVE;
    bool rare = false;   // something may be queued in observed, the reactive set, defFin or ticked
    // Pending deferred assertion reports (16.4.1): observed (#0) ones mature
    // in the observed region, final ones in the postponed region.
    vector<DeferRep> defObs, defFin;
    // Sampled values (16.5.1). Each (signal, shadow) pair: the shadow holds
    // the signal's value from the preponed region of this time slot. At time
    // 0 it keeps the default sampled value (the declaration's initial value).
    vector<pair<int, int>> shadows;
    // Free checker variables (17.7.2): signal, and whether it takes a new
    // random value in every time step (an active one).
    vector<pair<int, bool>> freeVars;
    Val freeValue(int s) {
        int w = sigs[s].w;
        uint64_t r = urandomNext(urandomState());
        if (w > 32) r |= (uint64_t)urandomNext(urandomState()) << 32;
        return make(w >= 64 ? r : (r & ((1ull << w) - 1)), 0, w, 0);
    }
    uint64_t freeAt = 0;   // the time step freeStep last ran in
    void freeStep() {
        for (auto [s, every] : freeVars)
            if (every) set(s, freeValue(s));
    }
    // Clocks of $past / $rose / ... (sva.hpp SampleClock); ticked: the ones
    // that ticked in this slot, shifted at the end of the slot.
    vector<SampleClock> sclocks;
    vector<int> ticked;
    uint64_t sampledAt = 0;
    struct Mon {
        bool on = false, queued = false;
        string fmt;
        vector<E> args;
        vector<int> watch;
    } mon;
    Wheel wheel;                                     // time wheel
    uint64_t now = 0, lastDump = ~0ULL;
    bool finished = false;
    int exitCode = 0;                                // the process exit status ($fatal sets 1)
    string vcdPath = "dump.vcd";                     // empty: no waveform
    string vcdTop = "top";   // name of the root VCD scope (the top module)
    // msim -vcd-arrays [N]: unpacked array elements go to the waveform too,
    // as m[3], for arrays of at most N elements. 0 (the default): memories
    // are left out, as $dumpvars does (21.7.1.2).
    size_t dumpArrays = 0;
    string timescale = "1ns";                        // one tick, for the VCD header
    ofstream vcd;
    // VCD text is built here and written in large blocks (an ostream insert
    // per character was most of the cost of dumping), with each signal's
    // identifier made once in header().
    string vbuf;
    vector<string> vcdIds;
    void flushVcd() {
        if (!vbuf.empty() && vcd.is_open()) vcd.write(vbuf.data(), (streamsize)vbuf.size());
        vbuf.clear();
    }
    // FST output instead of VCD when vcdPath ends in ".fst" (built with libfst).
    void* fst = nullptr;
    bool fstParallel = true;
    size_t fstPending = 0;                           // value-change bytes since the last block
    vector<uint32_t> fstH;                           // FST handle per signal (0: not dumped)
    string fstBuf;
    bool waving() const { return vcd.is_open() || fst != nullptr; }
    // VCD tasks (21.7.1). msim -vcd dumps every signal from time 0 and the
    // design's $dumpfile / $dumpvars are ignored; without -vcd, $dumpvars
    // opens $dumpfile's file (default dump.vcd) at the end of its time slot,
    // with the selected signals. $dumpoff / $dumpon / $dumpall / $dumpflush /
    // $dumplimit work on either.
    bool dumpCli = false, dumpPend = false, dumpOff = false, dumpStopped = false;
    string dumpFile = "dump.vcd";
    vector<char> dumpSel;                            // empty: every signal
    uint64_t dumpLimit = 0;                          // bytes; 0: no limit
    bool dumped(size_t i) const {
        return !sigs[i].hide && (dumpSel.empty() || (i < dumpSel.size() && dumpSel[i]));
    }

    Sim() {
        g_sformat = &Sim::sformat;
        g_fcloseHook = &Sim::closeHook;
    }

    // ---- multi-core: deterministic parallel batches ----
    // `threads` workers (default: defaultThreads(); msim -threads N).
    // Only work that reads signals and writes into its own buffer runs in
    // parallel: the value of a combinational process, or the NBAs of a
    // clocked process. Results are committed afterwards on this thread in a
    // fixed order (rank, process index), so the output does not depend on the
    // thread count or on timing. -threads 1 runs the same code serially.
    int threads = 1;
    static constexpr size_t kMinParallel = 32;
    static inline thread_local bool tlInPar = false;   // inside a parallelFor task
    Pool pool;
    // `use`: run on at most this many threads (0: `threads`).
    template <class F>
    void parallelFor(size_t n, F&& f0, int use = 0) {
        auto& f = f0;
        using FF = std::remove_reference_t<F>;
        // A small batch is cheaper on this thread than a wake-up of the pool.
        // Nested (a task that calls parallelFor) runs on its own thread.
        int T = use > 0 ? std::min(use, threads) : threads;
        if (T <= 1 || n < kMinParallel || tlInPar) { for (size_t i = 0; i < n; i++) f(i); return; }
        pool.ensure(threads);
        tlInPar = true;
        pool.run(n, [](void* c, size_t i) { tlInPar = true; (*static_cast<FF*>(c))(i); }, &f, T);
        tlInPar = false;
    }

    // Picks the thread count of one repeated parallel step by measuring it:
    // each candidate (1, 2, 4, ... up to `threads`) runs kTry steps, the
    // fastest is used for the next kKeep, then they are measured again. A
    // step of a few microseconds is slower on 8 threads than on 2 (the batch
    // hand-off costs more than the work), and the best count depends on the
    // machine (cores, SMT, P/E cores) and the design, so no fixed default
    // fits. Results never depend on it: only the speed does.
    struct Tuner {
        // kWarm steps of each candidate are not timed: right after a switch,
        // threads that just left the batch may still be spinning.
        static constexpr int kWarm = 32, kTry = 256, kKeep = 1 << 16;
        int cand = 0, n = 0, best = 1;   // cand: index into 1, 2, 4, ...; -1 = exploiting
        int64_t sum = 0, bestNs = INT64_MAX;
        std::chrono::steady_clock::time_point t0;
        int pick(int maxT) {
            if (maxT <= 1) return 1;
            if (cand < 0) {
                if (++n < kKeep) return best;
                cand = 0; n = 0; sum = 0; bestNs = INT64_MAX;   // re-measure
            }
            t0 = std::chrono::steady_clock::now();
            return std::min(1 << cand, maxT);
        }
        void done(int maxT) {
            if (maxT <= 1 || cand < 0) return;
            if (++n <= kWarm) return;
            sum += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            if (n < kWarm + kTry) return;
            int t = std::min(1 << cand, maxT);
            bool worse = bestNs != INT64_MAX && sum > 2 * bestNs;   // more threads will not win back a 2x loss
            if (sum < bestNs) { bestNs = sum; best = t; }
            n = 0; sum = 0;
            if (t >= maxT || worse) { cand = -1; return; }
            cand++;
        }
    };
    Tuner domainTune[3];   // cycle domain (codegen.hpp): fused step, NBA compute, commit
    Tuner combTune, partTune, seqTune;   // the event-driven batches: comb levels, partitions, clocked processes
    // parallelFor with the thread count `t` picks. A batch too small for the
    // pool runs here and is not measured.
    template <class F>
    void parallelTuned(Tuner& t, size_t n, F&& f) {
        if (threads <= 1 || n < kMinParallel || tlInPar) { for (size_t i = 0; i < n; i++) f(i); return; }
        int use = t.pick(threads);
        parallelFor(n, std::forward<F>(f), use);
        t.done(threads);
    }

    // ---- static combinational schedule (prog.hpp: combShape / combSchedule) ----
    // combs[r] is the process of rank r. Ranks are in topological order and
    // sorted by level (longest path from a non-combinational input), so two
    // ranks of one level never feed each other. A change on a signal marks
    // the ranks that read it; when the active region is empty the kernel takes
    // the lowest pending level, computes every marked rank of it (in parallel
    // when `compute` is set), and commits them in rank order. Each rank runs
    // at most once per wave. `curComb` is the committing one: its own writes
    // do not mark it again (a process is not woken by itself).
    struct Comb {
        void (*fn)(const void*);            // whole body, serial
        Val (*compute)(const void*);        // optional: the value of a one-assign body (no side effects)
        void (*commit)(const void*, Val);   // ...and its write
        const void* ctx;
        int level;
    };
    vector<Comb> combs;
    vector<vector<int>> combOf;                      // signal -> ranks that read it
    vector<char> combPend;
    // The pending combinational processes, lowest rank first: one bit per
    // rank. A heap push and pop per wake was a tenth of Ibex's run time.
    struct RankQ {
        vector<uint64_t> bits;
        size_t n = 0, lo = 0;   // count; no bit is set below word `lo`
        bool empty() const { return n == 0; }
        size_t size() const { return n; }
        void push(int r) {
            size_t w = (size_t)r >> 6;
            if (w >= bits.size()) bits.resize(w + 1, 0);
            uint64_t b = 1ULL << (r & 63);
            if (bits[w] & b) return;
            bits[w] |= b;
            n++;
            if (w < lo) lo = w;
        }
        int top() {
            while (!bits[lo]) lo++;
            return (int)(lo * 64 + (size_t)__builtin_ctzll(bits[lo]));
        }
        void pop() {
            int r = top();
            bits[(size_t)r >> 6] &= bits[(size_t)r >> 6] - 1;
            if (--n == 0) lo = 0;
        }
    };
    RankQ combq;
    int curComb = -1;
    vector<int> combBatch;
    vector<Val> combVals;
    // Register the next rank. `ins` is the process's wait list.
    void addComb(void (*fn)(const void*), const void* ctx, const vector<int>& ins, int level = 0,
                 Val (*compute)(const void*) = nullptr, void (*commit)(const void*, Val) = nullptr) {
        int r = (int)combs.size();
        combs.push_back({fn, compute, commit, ctx, level});
        combPend.push_back(0);
        if (combOf.size() < sigs.size()) combOf.resize(sigs.size());
        for (int s : ins) combOf[s].push_back(r);
        markComb(r);  // runs once at time 0, as the process would have
    }
    void markComb(int r) {
        if (combPend[r]) return;
        combPend[r] = 1;
        combq.push(r);
    }
    void runCombs() {
        while (!combq.empty()) {
            int level = combs[combq.top()].level;
            combBatch.clear();
            while (!combq.empty() && combs[combq.top()].level == level) {
                int r = combq.top();
                combq.pop();
                combPend[r] = 0;
                combBatch.push_back(r);
            }
            combVals.resize(combBatch.size());
            parallelTuned(combTune, combBatch.size(), [&](size_t k) {
                const Comb& c = combs[combBatch[k]];
                if (c.compute) combVals[k] = c.compute(c.ctx);
            });
            for (size_t k = 0; k < combBatch.size(); k++) {
                int r = combBatch[k];
                curComb = r;
                if (combs[r].compute) combs[r].commit(combs[r].ctx, combVals[k]);
                else combs[r].fn(combs[r].ctx);
                curComb = -1;
            }
        }
    }

    // ---- partitions: independent islands of logic, updated in parallel ----
    // Like Verilator's macro-tasks. Signals written by combinational processes
    // or by a clocked process's NBAs are grouped with every combinational
    // process that reads them (union-find), so two partitions share no written
    // signal and no combinational process. In the NBA region, the NBAs that
    // land in a partition are applied, and its combinational processes
    // re-evaluated in rank order, by one thread, one partition per task.
    // A partition is used only when nothing outside it can observe its
    // signals mid-region: no VCD, no $monitor, no coroutine waiting on one of
    // its signals, and no clocked process triggered by one. Otherwise its NBAs
    // go through the ordinary serial path, which gives the same values.
    struct Part {
        vector<int> combs;     // ranks, ascending
        vector<int> written;   // signals owned by this partition
        bool edgeSource = false;  // some clocked process is triggered by one of `written`
        vector<NbaItem> items; // this region's NBAs, in queue order
        vector<char> pend;     // per entry of `combs`
        int cur = -1;          // entry being evaluated
        vector<pair<int, Val>> changed;   // with a waveform open: changes to dump afterwards, in order
        bool wave = false;
    };
    vector<Part> parts;
    vector<int> sigPart;       // signal -> partition, or -1
    vector<int> combLocal;     // rank -> index in its partition's `combs`
    vector<int> partsBusy;
    vector<NbaItem> nbaSerial;
    static inline thread_local Part* tlPart = nullptr;
    // `seqTargets`: the fixed NBA targets of each clocked process (an NBA whose
    // target is chosen at run time falls back to the serial path by itself).
    void buildPartitions(const vector<vector<int>>& combOuts, const vector<vector<int>>& combIns,
                         const vector<int>& seqTargets) {
        size_t ns = sigs.size();
        combOf.resize(ns);
        seqOf.resize(ns);
        vector<int> up(ns);
        for (size_t i = 0; i < ns; i++) up[i] = (int)i;
        auto find = [&](int x) { while (up[x] != x) x = up[x] = up[up[x]]; return x; };
        auto unite = [&](int a, int b) { a = find(a); b = find(b); if (a != b) up[max(a, b)] = min(a, b); };
        vector<char> owned(ns, 0);
        for (auto& o : combOuts) for (int x : o) owned[x] = 1;
        for (int x : seqTargets) owned[x] = 1;
        for (size_t r = 0; r < combOuts.size(); r++) {
            int first = -1;
            for (int x : combOuts[r]) { if (first < 0) first = x; else unite(first, x); }
            for (int x : combIns[r])
                if (owned[x]) { if (first < 0) first = x; else unite(first, x); }
        }
        vector<int> id(ns, -1);
        parts.clear();
        sigPart.assign(ns, -1);
        for (size_t x = 0; x < ns; x++) {
            if (!owned[x]) continue;
            int root = find((int)x);
            if (id[root] < 0) { id[root] = (int)parts.size(); parts.emplace_back(); }
            sigPart[x] = id[root];
            parts[id[root]].written.push_back((int)x);
        }
        combLocal.assign(combs.size(), -1);
        for (size_t r = 0; r < combOuts.size(); r++) {
            if (combOuts[r].empty()) continue;
            Part& pt = parts[sigPart[combOuts[r][0]]];
            combLocal[r] = (int)pt.combs.size();
            pt.combs.push_back((int)r);
        }
        for (auto& pt : parts) {
            pt.pend.assign(pt.combs.size(), 0);
            for (int x : pt.written)
                if ((size_t)x < seqOf.size() && !seqOf[x].empty()) pt.edgeSource = true;
        }
    }
    // set() inside a partition task: only this partition reads the signal.
    // Stores a value nothing watches (Instr::tmp): no change detection, no
    // wake-ups, safe from a clocked batch running in parallel.
    void poke(int s, Val nv, int hi, int lo) {
        Sig& g = sigs[s];
        if (hi >= 0) nv = v_insert(g.val, nv, hi, lo);
        nv = maskWidth(nv, g.w);
        nv.sgn = false;
        g.val = nv;
    }
    void setLocal(int s, Val nv) {
        Sig& g = sigs[s];
        if (nv == g.val) return;
        Part& pt = *tlPart;
        // Toggle masks of a partition's own signals are written only by the
        // thread running it (the per-signal table is sized beforehand).
        if (g_cov) covToggle(s, g.val, nv);
        if (pt.wave) pt.changed.emplace_back(s, nv);
        g.val = nv;
        for (int r : combOf[s]) {
            int k = combLocal[r];
            if (k != pt.cur) pt.pend[k] = 1;
        }
    }
    void runPart(Part& pt) {
        tlPart = &pt;
        for (auto& n : pt.items) set(n.s, n.hi >= 0 ? v_insert(get(n.s), n.v, n.hi, n.lo) : n.v);
        pt.items.clear();
        // A partition's ranks only feed higher ranks: one forward sweep.
        for (size_t k = 0; k < pt.combs.size(); k++) {
            if (!pt.pend[k]) continue;
            pt.pend[k] = 0;
            pt.cur = (int)k;
            const Comb& c = combs[pt.combs[k]];
            c.fn(c.ctx);
        }
        pt.cur = -1;
        tlPart = nullptr;
    }
    bool partUsable(const Part& pt) const {
        if (pt.edgeSource) return false;
        for (int x : pt.written)
            if (!waiters[x].empty()) return false;
        return true;
    }
    void applyNba() {
        if (fastCommit) {
            // A compiled cycle domain: its NBAs are one marker item (s < 0).
            // Partitions are never built alongside it.
            for (auto& n : nbaRun) {
                if (n.s < 0) { fastCommit(); continue; }
                set(n.s, n.hi >= 0 ? v_insert(get(n.s), n.v, n.hi, n.lo) : n.v);
            }
            return;
        }
        // A waveform and coverage keep the partitions parallel: each records
        // its changes (dumped below, in partition order) and toggles.
        if (parts.empty() || mon.on || !fmons.empty()) {
            // Partial writes merge into the value as it is *now*, so two NBAs to
            // different bits of one reg in the same slot both take effect.
            for (auto& n : nbaRun) set(n.s, n.hi >= 0 ? v_insert(get(n.s), n.v, n.hi, n.lo) : n.v);
            return;
        }
        nbaSerial.clear();
        partsBusy.clear();
        for (auto& n : nbaRun) {
            int p = (size_t)n.s < sigPart.size() ? sigPart[n.s] : -1;   // frame and object signals come later
            if (p >= 0) {
                if (parts[p].items.empty()) partsBusy.push_back(p);
                parts[p].items.push_back(n);
            } else
                nbaSerial.push_back(n);
        }
        // A partition that something outside could observe goes serial, keeping queue order.
        size_t keep = 0;
        for (int p : partsBusy) {
            if (partUsable(parts[p])) { partsBusy[keep++] = p; continue; }
            for (auto& n : parts[p].items) nbaSerial.push_back(n);
            parts[p].items.clear();
        }
        partsBusy.resize(keep);
        for (auto& n : nbaSerial) set(n.s, n.hi >= 0 ? v_insert(get(n.s), n.v, n.hi, n.lo) : n.v);
        if (!std::is_sorted(partsBusy.begin(), partsBusy.end())) std::sort(partsBusy.begin(), partsBusy.end());
        bool wave = waving();
        if (g_cov && togRise.size() < sigs.size()) { togRise.resize(sigs.size()); togFall.resize(sigs.size()); }
        for (int p : partsBusy) parts[p].wave = wave;
        parallelTuned(partTune, partsBusy.size(), [&](size_t k) { runPart(parts[partsBusy[k]]); });
        if (wave)
            for (int p : partsBusy) {
                for (auto& [sg, v] : parts[p].changed) dump(sg, v);
                parts[p].changed.clear();
            }
    }

    // ---- clocked processes as functions (prog.hpp: seqShape) ----
    // A process that waits on fixed events and then only schedules NBAs
    // (always @(posedge clk) RTL) runs as a function that writes its NBAs into
    // its own buffer. All triggered ones in a slot run as one batch, in
    // parallel, and their NBAs join the queue in process order.
    struct Seq { void (*fn)(const void*, vector<NbaItem>&); const void* ctx; };
    vector<Seq> seqs;
    vector<vector<pair<int, Edge>>> seqOf;           // signal -> (process, edge)
    vector<char> seqPend;
    vector<int> seqq;
    vector<vector<NbaItem>> seqBufs;
    void addSeq(void (*fn)(const void*, vector<NbaItem>&), const void* ctx, const vector<pair<int, Edge>>& ev) {
        int id = (int)seqs.size();
        seqs.push_back({fn, ctx});
        seqPend.push_back(0);
        if (seqOf.size() < sigs.size()) seqOf.resize(sigs.size());
        for (auto& [sg, e] : ev) seqOf[sg].push_back({id, e});
    }
    void runSeqs() {
        if (!std::is_sorted(seqq.begin(), seqq.end())) std::sort(seqq.begin(), seqq.end());
        if (seqBufs.size() < seqq.size()) seqBufs.resize(seqq.size());
        parallelTuned(seqTune, seqq.size(), [&](size_t k) {
            seqBufs[k].clear();
            seqs[seqq[k]].fn(seqs[seqq[k]].ctx, seqBufs[k]);
        });
        for (size_t k = 0; k < seqq.size(); k++) {
            seqPend[seqq[k]] = 0;
            nbaq.insert(nbaq.end(), seqBufs[k].begin(), seqBufs[k].end());
        }
        seqq.clear();
    }

    // ---- compiled cycle domain (msim -compile, codegen.hpp) ----
    // Like Verilator's eval(): the clocked processes that share one event
    // list run as one function that stores their NBAs in fixed slots, and
    // `fastCommit` writes those slots and then re-evaluates every static
    // combinational process in rank order (registered as the single comb
    // rank `fastComb`). Writes go through fw(), which stores the value
    // directly when nothing else can see the signal change; otherwise it is
    // an ordinary set(), so VCD, $monitor and waiting processes still work.
    void (*fastCommit)() = nullptr;
    // Called from a clocked batch: true when nothing but the domain's own
    // comb rank can run before the NBA region (processes woken by the same
    // edge already ran in the active region). The domain then commits and
    // settles its logic at once, which also covers that pending comb rank.
    bool fuseOk() {
        if (!inactive.empty()) return false;
        if (combq.empty()) return true;
        if (combq.size() != 1 || combq.top() != fastComb) return false;
        combq.pop();
        combPend[fastComb] = 0;
        return true;
    }
    int fastSeqId = -1, fastComb = -1;
    vector<char> quiet;   // signal -> at most 64 bits, and only the cycle domain reacts to it
    void fastInit() {
        quiet.assign(sigs.size(), 1);
        for (size_t s = 0; s < sigs.size(); s++) {
            if (sigs[s].w > 64) quiet[s] = 0;   // (a net whose one continuous assign msim merged into a plain write has no driver and is stored like a variable)
            if (s < tranOf.size() && !tranOf[s].empty()) quiet[s] = 0;   // a tran net: drive() resolves its group
            if (s < seqOf.size())
                if (!seqOf[s].empty()) quiet[s] = 0;   // a clock: set() detects its edges
            if (s < combOf.size())
                for (int r : combOf[s]) if (r != fastComb) quiet[s] = 0;
        }
    }
    bool observed = false;   // a VCD is open or $monitor was armed: every change is seen
    // The common case inline: a narrow signal nobody else watches is a store.
    __attribute__((always_inline)) void fw(int s, const Val& nv) {
        Sig& g = sigs[s];
        if (__builtin_expect(quiet[s] && !observed && !g_cov && waiters[s].empty(), 1)) {
            uint64_t m = mask(g.w);
            g.val.v = nv.v & m;
            g.val.x = nv.x & m;
            g.val.z = nv.z & m;
            g.val.real = nv.real;
            g.val.str = nv.str;
            return;
        }
        fwSlow(s, nv);
    }
    // The same for a known (2-state) value, without building a Val.
    __attribute__((always_inline)) void fwv(int s, uint64_t v) {
        Sig& g = sigs[s];
        if (__builtin_expect(quiet[s] && !observed && !g_cov && waiters[s].empty(), 1)) {
            g.val.v = v & mask(g.w);
            g.val.x = 0;
            g.val.z = 0;
            g.val.real = false;
            g.val.str = false;
            return;
        }
        fwSlow(s, make(v & mask(g.w), 0, g.w));
    }
    __attribute__((always_inline)) void fwdv(int s, int d, uint64_t v, int s0, int s1) {
        Sig& g = sigs[s];
        if (__builtin_expect(quiet[s] && g.drv.size() == 1 && s0 != STR_HIZ && s1 != STR_HIZ, 1)) {
            Drv& dr = g.drv[0];
            dr.v.v = v & mask(g.w);
            dr.v.x = 0;
            dr.v.z = 0;
            dr.s0 = (uint8_t)s0;
            dr.s1 = (uint8_t)s1;
            fwv(s, v);
            return;
        }
        drive(s, d, make(v & mask(g.w), 0, g.w), s0, s1);
    }
    // Bits [hi:lo] (at most 64) of signal s already hold the known value u.
    bool samePart(int s, int hi, int lo, uint64_t u) const {
        const Val& c = sigs[s].val;
        if (hi >= sigs[s].w) return false;
        int n = hi - lo + 1, li = lo / 64, off = lo % 64;
        uint64_t v = limbV(c, li) >> off, x = limbX(c, li) >> off;
        if (off && off + n > 64) { v |= limbV(c, li + 1) << (64 - off); x |= limbX(c, li + 1) << (64 - off); }
        uint64_t m = mask(n);
        return (v & m) == (u & m) && (x & m) == 0;
    }
    __attribute__((noinline)) void fwSlow(int s, const Val& in) {
        Sig& g = sigs[s];
        // Only a waveform (or coverage) watches a quiet signal: compare, store
        // and record the change. A full set() would also queue the domain's
        // comb pass and scan for waiters, which made -vcd runs 20x slower.
        // Most writes of a comb pass store what is already there.
        if (g.w <= 64 && g.val.hi.empty() && !g_cov) {
            uint64_t m = mask(g.w);
            if ((in.v & m) == g.val.v && (in.x & m) == g.val.x && (in.z & m) == g.val.z && in.real == g.val.real && in.str == g.val.str) return;
        }
        bool waveOnly = !mon.on && !g.fl;
        if (!quiet[s] || !waiters[s].empty() || (observed && !waveOnly)) {
#ifdef MSIM_FWSTAT
            { static struct St { std::map<std::string, long> m; ~St() { std::vector<std::pair<long, std::string>> v; for (auto& [k, c] : m) v.push_back({c, k});
                std::sort(v.rbegin(), v.rend()); for (size_t i = 0; i < v.size() && i < 400; i++) std::fprintf(stderr, "FW %ld %s\n", v[i].first, v[i].second.c_str()); } } st;
              std::string why = !quiet[s] ? (!seqOf[s].empty() ? "seq " : "comb ") : !waiters[s].empty() ? "wait " : "obs ";
              if (!quiet[s] && seqOf[s].empty()) { why = "comb"; if (sigs[s].w > 64) why += ":wide"; if ((size_t)s < combOf.size()) for (int r : combOf[s]) why += r == fastComb ? ":all" : ":r" + std::to_string(r); why += " "; }
              st.m[why + sigs[s].name]++; }
#endif
            set(s, in); return; }
        if (g.w <= 64 && g.val.hi.empty()) {   // the common case, without copying a Val
            uint64_t m = mask(g.w), v = in.v & m, x = in.x & m, z = in.z & m;
            if (v == g.val.v && x == g.val.x && z == g.val.z) return;
            if (g_cov) covToggle(s, g.val, Val{v, x, g.w, z});
            g.val.v = v; g.val.x = x; g.val.z = z;
            g.val.real = in.real; g.val.str = in.str; g.val.sgn = false;
            if (observed) dump(s);
            return;
        }
        Val nv = maskWidth(in, g.w);
        if (nv == g.val) return;
        if (g_cov) covToggle(s, g.val, nv);
        g.val = std::move(nv);
        if (observed) dump(s);
    }
    // A continuous assign: the net's only driver, strong 0 and 1, resolves to its value.
    __attribute__((always_inline)) void fwd(int s, int d, const Val& v, int s0, int s1) {
        Sig& g = sigs[s];
        if (__builtin_expect(quiet[s] && g.drv.size() == 1 && s0 != STR_HIZ && s1 != STR_HIZ, 1)) {
            uint64_t m = mask(g.w);
            Drv& dr = g.drv[0];
            dr.v.v = v.v & m & ~v.x;   // make(): an unknown bit's v is 0
            dr.v.x = v.x & m;
            dr.v.z = v.z & dr.v.x;
            dr.s0 = (uint8_t)s0;
            dr.s1 = (uint8_t)s1;
            fw(s, dr.v);
            return;
        }
        drive(s, d, v, s0, s1);
    }
    // A testbench wrote a private signal: re-evaluate the domain's comb logic.
    void markFast() { if (fastComb >= 0) markComb(fastComb); }
    // fastCommit's own comb pass: its writes must not queue the pass again.
    void fastCombs(void (*all)(const void*)) {
        int saved = curComb;
        curComb = fastComb;
        all(nullptr);
        curComb = saved;
    }

    ~Sim() {
        for (auto h : all) h.destroy();
        closeWaves();
    }
    void closeWaves() {
        evcdClose();
#ifdef MINISIM_FST
        if (fst) { fstWriterClose((fstWriterContext*)fst); fst = nullptr; }
#endif
        if (vcd.is_open()) { flushVcd(); vcd.close(); }
    }

    // A wire (`net`) starts as Z, including before the first drive().
    // A variable (reg/logic) starts as X. The flag is stored so the VCD
    // header can emit `$var wire` or `$var reg`.
    // msim -xprop (SF_XPROP_EDGE): set() keeps each signal's bit 0 from
    // before its last change and the time of that change (Sig::p0, at).
    bool edgeTrack = false;
    uint64_t xAt = ~0ULL;   // the last time step in which bit 0 of a signal changed from or to x (kept with edgeTrack)
    int sig(const string& n, int w, bool net = false) {
        self = this;
        // An unpacked array element is named "m[3]". The VCD leaves memories
        // out, as $dumpvars does by default (21.7.1.2).
        // A name with '$' is internal (an event's last-trigger time).
        // A variable starts at X and a net at Z, across every limb. Widths of
        // 64 or less keep the single-word initial value they had before.
        Val init = net ? allz(w) : (w > 64 ? allx(w) : Val{});
        // Only the last name component decides "array element": u.m[3] is
        // one, blk[2].x (a signal in a generate block) is not.
        size_t dot = n.rfind('.');
        bool elem = n.find('[', dot == string::npos ? 0 : dot + 1) != string::npos;
        sigs.push_back({n, w, init, {}, net, elem || n.find('$') != string::npos});
        if (n.starts_with("_xp$")) edgeTrack = true;   // a temporary of msim -xprop: the design asks about edges
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
        g.drv[d].v = g.w <= 64 ? make(v.v, v.x, g.w, v.z) : clipRead(v, g.w);
        g.drv[d].s0 = (uint8_t)s0;
        g.drv[d].s1 = (uint8_t)s1;
        if (__builtin_expect(!varPathDrv.empty(), 0) && !applyingVar) {   // a variable path output's port (30.4)
            auto vp = varPathDrv.find({s, d});
            if (vp != varPathDrv.end()) {
                Val nv = g.drv[d].v, cur = vp->second.second;
                g.drv[d].v = cur;   // the port keeps its old value until the path delay passes
                Val from = projected(s, PQ_VAR - d, cur);
                if (sameVal(from, nv)) return;
                Lim L = pathLim(vp->second.first, from, nv);
                pulseSchedule(s, PQ_VAR - d, cur, nv, L, styleOf(vp->second.first), (uint8_t)s0, (uint8_t)s1);
                return;
            }
        }
        if (__builtin_expect(!loadsOf.empty(), 0)) shadowDrive(s, d);   // 32.4.4: loads that see each source's delay
        if (__builtin_expect(!tranOf.empty(), 0) && (size_t)s < tranOf.size() && !tranOf[(size_t)s].empty()) { tranResolve(s); return; }
        // One driver that drives both 0 and 1 resolves to its own value
        // (X stays X, Z stays Z). Skip the per-bit strength walk.
        Val r = g.kind != NR_WIRE ? resolveNet(g.kind, g.drv.data(), g.drv.size(), g.w, g.val)   // wand, tri0, ... (6.6)
              : (g.drv.size() == 1 && s0 != STR_HIZ && s1 != STR_HIZ) ? g.drv[0].v
                                                                       : resolveDrives(g.drv.data(), g.drv.size(), g.w);
        if (__builtin_expect(!decay.empty(), 0) && g.kind == NR_TRIREG) decayArm(s, r);   // 28.16.2
        if (!timedOut.empty() && (size_t)s < timedOut.size() && timedOut[s]) {   // 30.5.3, 30.7, 32.4.4
            if (shadowed(s)) return;   // its value comes from the delayed sources (shadowOf)
            Val from = projected(s, PQ_PATH, g.val);
            if (sameVal(from, r)) return;   // already on its way, or there
            Lim L = pathLim(s, from, r);
            if (!netDelay.empty()) {   // a net delay on a path output is a distributed delay too (30.6)
                auto nd = netDelay.find(s);
                if (nd != netDelay.end()) L.d = std::max<uint64_t>(L.d, pickDelay(nd->second, from, r));
            }
            pulseSchedule(s, PQ_PATH, g.val, r, L, styleOf(s), STR_STRONG, STR_STRONG);
            return;
        }
        if (!netDelay.empty()) {   // wire #d w (28.16): the resolved value arrives d later
            auto nd = netDelay.find(s);
            if (nd != netDelay.end()) { schedule(s, -1, r, g.val, nd->second, STR_STRONG, STR_STRONG); return; }
        }
        set(s, r);
    }

    // Pulse filtering (30.7): the changes on their way to a path output
    // (slot PQ_PATH), or of one source's driver as a load sees it through an
    // interconnect delay (slot PQ_SRC - driver, 32.4.4). Each entry is one
    // future change; an unlimited number may be pending.
    enum { PQ_PATH = -2, PQ_DECAY = -3, PQ_SRC = -10, PQ_VAR = -1000000 };
    // trireg nets (28.16.2): the charge strength when not medium, and the
    // charge decay time. When the drivers turn off, the held 0 or 1 becomes
    // x after the decay time unless a driver turns on first.
    struct Decay { uint64_t ticks = 0; uint64_t gen = 0; bool on = false; };
    unordered_map<int, Decay> decay;
    unordered_map<int, uint8_t> charge;
    uint8_t chargeOf(int s) const {
        if (charge.empty()) return STR_MEDIUM;
        auto it = charge.find(s);
        return it == charge.end() ? STR_MEDIUM : it->second;
    }
    // r: the net's new resolved value. A decay time of 0 makes held bits x at once.
    void decayArm(int s, Val& r) {
        auto it = decay.find(s);
        if (it == decay.end() || !it->second.on) return;
        Decay& dc = it->second;
        dc.gen++;
        const Sig& g = sigs[(size_t)s];
        Val dr = resolveDrives(g.drv.data(), g.drv.size(), g.w);
        bool held = false;
        for (int b = 0; b < g.w; b++)
            if (bitAt(dr, b) == 3) {
                held = true;
                if (dc.ticks == 0 && bitAt(r, b) < 2) setBit(r, b, 2);
            }
        if (!held || dc.ticks == 0) return;
        laterDrv[now + dc.ticks].push_back({s, PQ_DECAY, dc.gen, 0, 0});
        wheel[now + dc.ticks];
    }
    void decayFire(int s, uint64_t gen) {
        auto it = decay.find(s);
        if (it == decay.end() || it->second.gen != gen) return;   // a driver changed since
        const Sig& g = sigs[(size_t)s];
        Val dr = resolveDrives(g.drv.data(), g.drv.size(), g.w);
        Val nv = g.val;
        for (int b = 0; b < g.w; b++)
            if (bitAt(dr, b) == 3 && bitAt(nv, b) < 2) setBit(nv, b, 2);
        if (!sameVal(nv, g.val)) set(s, nv);
    }
    // The port connections of variable path outputs: (net, driver) -> the
    // variable and the value the driver last passed on.
    map<pair<int, int>, pair<int, Val>> varPathDrv;
    bool applyingVar = false;
    struct PEv { uint64_t t, gen; Val v; uint8_t s0, s1; };
    map<pair<int, int>, vector<PEv>> pq;
    struct Lim { uint64_t d = 0, rj = 0, er = 0; };
    Val projected(int s, int slot, const Val& cur) {
        auto it = pq.find({s, slot});
        return it == pq.end() || it->second.empty() ? cur : it->second.back().v;
    }
    uint8_t styleOf(int s) const {
        uint8_t st = 0;
        if (auto it = pulseStyles().find(s); it != pulseStyles().end()) st = it->second;
        const PulseOpts& o = pulseOpts();
        if (o.detect >= 0) st = (uint8_t)(o.detect ? st | PS_DETECT : st & ~PS_DETECT);
        if (o.showCancelled >= 0) st = (uint8_t)(o.showCancelled ? st | PS_SHOWCANCELLED : st & ~PS_SHOWCANCELLED);
        return st;
    }
    void pqPush(int s, int slot, uint64_t t, const Val& v, uint8_t s0, uint8_t s1) {
        uint64_t gen = ++pendGen;
        auto& q = pq[{s, slot}];
        auto at = q.end();   // keep the queue in time order
        while (at != q.begin() && (at - 1)->t > t) --at;
        q.insert(at, PEv{t, gen, v, s0, s1});
        laterDrv[t].push_back({s, slot, gen, s0, s1});
        wheel[t];
    }
    // The bits of `lead` that differ from `before`, as x: a pulse filtered to x.
    static Val pulseX(const Val& before, const Val& lead) {
        Val x = lead;
        for (int b = 0; b < lead.w; b++)
            if (bitAt(before, b) != bitAt(lead, b)) setBit(x, b, 2);
        return x;
    }
    // A new change v of signal s (slot), with the delay and pulse limits of
    // its transition. The last pending change is the pulse's leading edge
    // and v its trailing edge: a pulse at least as wide as the error limit
    // passes, one at least as wide as the reject limit becomes x (from the
    // leading edge, or at once with on-detect), a narrower one disappears.
    // A trailing edge due before its leading edge cancels it, or with
    // showcancelled makes x between the two (from now with on-detect)
    // (30.7.4).
    void pulseSchedule(int s, int slot, const Val& cur, const Val& v, Lim L, uint8_t style, uint8_t s0, uint8_t s1) {
        L.rj = std::min(L.rj, L.d);
        L.er = std::min(std::max(L.er, L.rj), L.d);
        auto& q = pq[{s, slot}];
        uint64_t tB = now + L.d;
        if (q.empty()) {
            if (L.d == 0) { applySlot(s, slot, v, s0, s1); return; }
            pqPush(s, slot, tB, v, s0, s1);
            return;
        }
        PEv lead = q.back();
        Val before = q.size() > 1 ? q[q.size() - 2].v : cur;
        bool detect = style & PS_DETECT;
        if (tB < lead.t) {   // a negative pulse (30.7.4.2)
            q.pop_back();
            if (style & PS_SHOWCANCELLED) {
                Val x = pulseX(before, lead.v);
                if (detect) applyOrPush(s, slot, now, x, lead.s0, lead.s1);
                else pqPush(s, slot, tB, x, lead.s0, lead.s1);
                pqPush(s, slot, lead.t, v, s0, s1);
                return;
            }
            if (!sameVal(v, before)) pqPush(s, slot, tB, v, s0, s1);
            return;
        }
        uint64_t width = tB - lead.t;
        if (width >= L.er) { pqPush(s, slot, tB, v, s0, s1); return; }
        if (width >= L.rj) {   // filtered to x
            Val x = pulseX(before, lead.v);
            q.pop_back();
            if (detect) applyOrPush(s, slot, now, x, lead.s0, lead.s1);
            else pqPush(s, slot, lead.t, x, lead.s0, lead.s1);
            pqPush(s, slot, tB, v, s0, s1);
            return;
        }
        q.pop_back();   // rejected
        if (!sameVal(v, before)) pqPush(s, slot, tB, v, s0, s1);
    }
    void applyOrPush(int s, int slot, uint64_t t, const Val& v, uint8_t s0, uint8_t s1) {
        if (t == now) applySlot(s, slot, v, s0, s1);
        else pqPush(s, slot, t, v, s0, s1);
    }
    void applySlot(int s, int slot, const Val& v, uint8_t s0, uint8_t s1) {
        if (slot == PQ_PATH) { set(s, v); return; }
        if (slot <= PQ_VAR) {
            int d = PQ_VAR - slot;
            auto vp = varPathDrv.find({s, d});
            if (vp != varPathDrv.end()) vp->second.second = v;
            applyingVar = true;
            drive(s, d, v, s0, s1);
            applyingVar = false;
            return;
        }
        auto& sh = shadowOf[s];
        int d = PQ_SRC - slot;
        if ((size_t)d >= sh.drv.size()) return;
        sh.drv[(size_t)d] = Drv{v, s0, s1};
        const Sig& net = sigs[(size_t)sh.net];
        Val r = net.kind != NR_WIRE ? resolveNet(net.kind, sh.drv.data(), sh.drv.size(), sigs[(size_t)s].w, sigs[(size_t)s].val)
                                    : resolveDrives(sh.drv.data(), sh.drv.size(), sigs[(size_t)s].w);
        set(s, r);
    }
    // A pending change (laterDrv) of a pulse queue is due: apply it unless
    // it was cancelled.
    bool runPulse(int s, int slot, uint64_t gen) {
        auto it = pq.find({s, slot});
        if (it == pq.end()) return true;
        auto& q = it->second;
        for (size_t k = 0; k < q.size(); k++) {
            if (q[k].gen != gen) continue;
            PEv e = q[k];
            q.erase(q.begin() + (long)k);
            applySlot(s, slot, e.v, e.s0, e.s1);
            break;
        }
        return true;
    }

    // Interconnect delays from each source of a net with several sources to
    // one load (32.4.4): the load's value is the resolution of the net's
    // drivers, each seen through its own delay. shadowOf[load] keeps those
    // delayed drivers; loadsOf[net] lists the loads.
    struct Shadow {
        int net = -1;
        vector<Drv> drv;
        Delay12 all;                        // PORT / NETDELAY: from every source
        map<int, Delay12> from;             // INTERCONNECT: from one driver of the net
    };
    unordered_map<int, Shadow> shadowOf;
    unordered_map<int, vector<int>> loadsOf;
    bool shadowed(int s) const { return !shadowOf.empty() && shadowOf.count(s); }
    Shadow& addShadow(int load, int net) {
        auto [it, fresh] = shadowOf.try_emplace(load);
        if (fresh) {
            it->second.net = net;
            it->second.drv = sigs[(size_t)net].drv;
            loadsOf[net].push_back(load);
            if ((size_t)load < timedOut.size()) timedOut[(size_t)load] = 1;
        }
        return it->second;
    }
    void shadowDrive(int net, int d) {
        auto li = loadsOf.find(net);
        if (li == loadsOf.end()) return;
        const Drv& now_ = sigs[(size_t)net].drv[(size_t)d];
        for (int load : li->second) {
            Shadow& sh = shadowOf[load];
            if ((size_t)d >= sh.drv.size()) continue;
            auto fi = sh.from.find(d);
            const Delay12& D = fi != sh.from.end() ? fi->second : sh.all;
            int slot = PQ_SRC - d;
            Val from = projected(load, slot, sh.drv[(size_t)d].v);
            if (sameVal(from, now_.v)) continue;
            Lim L = lim12(D, from, now_.v);
            pulseSchedule(load, slot, sh.drv[(size_t)d].v, now_.v, L, 0, now_.s0, now_.s1);
        }
    }
    // A gate or UDP driver with SDF DEVICE delays (32.4.1): inertial, by
    // the transition's delay.
    void driveGate(int s, int d, Val v, uint8_t s0, uint8_t s1, const uint64_t* dl) {
        auto& g = sigs[s];
        Val nv = g.w <= 64 ? make(v.v, v.x, g.w, v.z) : clipRead(v, g.w);
        const Val& cur = g.drv[(size_t)d].v;
        scheduleIn(s, d, nv, cur, delay12(dl, cur, nv), s0, s1);
    }

    // Specify path outputs and interconnect loads (30.4, 32.4.4). timedOut[s]
    // marks a net whose resolved value takes a path or interconnect delay;
    // lastChg is the time each path input last changed (30.5.3).
    vector<uint8_t> timedOut, pathIn;
    vector<uint64_t> lastChg;
    unordered_map<int, vector<int>> pathsTo;               // output -> pathRecs() indices
    unordered_map<int, Delay12> icDelay;                   // load port -> interconnect delay (every source)
    bool (*pathCond)(const E&) = nullptr;                  // a path condition holds (x counts)
    // Sizes the tables once the design is lowered and annotated.
    void setupPaths() {
        timedOut.assign(sigs.size(), 0);
        pathIn.assign(sigs.size(), 0);
        lastChg.assign(sigs.size(), 0);
        pathsTo.clear();
        auto& P = pathRecs();
        varPathDrv.clear();
        for (size_t k = 0; k < P.size(); k++) {
            pathsTo[P[k].dst].push_back((int)k);
            pathIn[(size_t)P[k].src] = 1;
            if (!P[k].dstVar) { timedOut[(size_t)P[k].dst] = 1; continue; }
            for (const PortLink& l : outLinks())
                if (l.formal == P[k].dst) {
                    varPathDrv[{l.net, l.drv}] = {P[k].dst, sigs[(size_t)l.net].drv[(size_t)l.drv].v};
                    if (now == 0) sigs[(size_t)l.net].val = allx(sigs[(size_t)l.net].w);   // x until the first value arrives
                }
        }
        for (auto& [sg, d] : icDelay) timedOut[(size_t)sg] = 1;
        for (auto& [sg, sh] : shadowOf) timedOut[(size_t)sg] = 1;
        // Like a net delay, a delayed net with drivers is x until its first
        // value arrives (28.16).
        for (size_t k = 0; k < timedOut.size(); k++)
            if (timedOut[k] && !sigs[k].drv.empty() && now == 0) sigs[k].val = allx(sigs[k].w);
    }
    // The delay of one transition: each changed bit's from/to levels pick
    // one of the 12 delays; a vector takes the smallest.
    template <class D> static uint64_t delay12(const D& d, const Val& from, const Val& to) {
        uint64_t best = ~0ULL;
        int w = to.w;
        for (int k = 0; k < w; k++) {
            int f = bitAt(from, k), t = bitAt(to, k);
            if (f == t) continue;
            best = std::min<uint64_t>(best, d[(size_t)transIdx(f, t)]);
        }
        return best == ~0ULL ? 0 : best;
    }
    // Delay and pulse limits of a change: a vector takes the transition
    // with the smallest delay among its changed bits.
    template <class D> static Lim lim12(const D& x, const Val& from, const Val& to) {
        Lim L;
        bool any = false;
        for (int k = 0; k < to.w; k++) {
            int f = bitAt(from, k), t = bitAt(to, k);
            if (f == t) continue;
            int i = transIdx(f, t);
            if (!any || x.d[i] < L.d) { L = {x.d[i], x.rj[i], x.er[i]}; any = true; }
        }
        return L;
    }
    // 30.5.3: the paths to s whose input changed most recently and whose
    // condition holds (an ifnone path when no conditional path between the
    // same ports holds) are active; the delay is the smallest of theirs.
    // With no active path there is no path delay. An interconnect delay to
    // a load port comes after the path delay of a path ending there.
    Lim pathLim(int s, const Val& from, const Val& to) {
        Lim t;
        auto pt = pathsTo.find(s);
        if (pt != pathsTo.end()) {
            auto& P = pathRecs();
            auto& ids = pt->second;
            auto on = [&](const PathRec& p) {
                if (p.hasCond) return !pathCond || pathCond(p.condE);
                if (!p.ifnone) return true;
                for (int j : ids) {
                    const PathRec& q = P[(size_t)j];
                    if (q.hasCond && q.src == p.src && (!pathCond || pathCond(q.condE))) return false;
                }
                return true;
            };
            uint64_t latest = 0;
            bool any = false;
            for (int j : ids) {
                const PathRec& p = P[(size_t)j];
                if (!on(p)) continue;
                uint64_t lc = lastChg[(size_t)p.src];
                Lim d = lim12(p, from, to);
                // 30.6: delays inside the module (gates, assignments) that
                // already passed count toward the path delay; the larger of
                // the two applies.
                d.d = lc + d.d > now ? lc + d.d - now : 0;
                if (!any || lc > latest || (lc == latest && d.d < t.d)) { latest = lc; t = d; any = true; }
            }
        }
        if (!icDelay.empty()) {
            auto ic = icDelay.find(s);
            if (ic != icDelay.end()) {
                Lim d = lim12(ic->second, from, to);
                t.d += d.d; t.rj += d.rj; t.er += d.er;
            }
        }
        return t;
    }
    // Bidirectional switches (28.8). Nets joined by switches that are on form
    // one group; every net in it takes the resolution of all the group's
    // drivers. A driver seen through a switch is reduced (28.13.1): supply
    // becomes strong through a tran, and each rtran lowers it one more step.
    // A switch with an X or Z control is resolved both ways: bits that
    // differ become X.
    struct TranSw { int a, b; bool resist; int state = -1; };   // state: 0 off, 1 on, 2 unknown
    vector<TranSw> trans;
    vector<vector<int>> tranOf;   // net -> its switches
    void addTran(int a, int b, bool resist) {
        if (tranOf.size() != sigs.size()) tranOf.resize(sigs.size());
        tranOf[(size_t)a].push_back((int)trans.size());
        tranOf[(size_t)b].push_back((int)trans.size());
        trans.push_back({a, b, resist});
    }
    void tranCtl(int id, int st) {
        if (trans[(size_t)id].state == st) return;
        trans[(size_t)id].state = st;
        tranResolve(trans[(size_t)id].a);
        tranResolve(trans[(size_t)id].b);
    }
    static uint8_t tranReduce(uint8_t s, bool resist) {
        static const uint8_t r[8] = {STR_HIZ, STR_SMALL, STR_SMALL, STR_MEDIUM, STR_MEDIUM, STR_WEAK, STR_PULL, STR_PULL};
        if (resist) return r[s & 7];
        return s == STR_SUPPLY ? STR_STRONG : s;
    }
    // The value net n sees: the drivers of every net it reaches through
    // switches that are on (and, when unkOn, those with an unknown control).
    Val tranValue(int n, bool unkOn, vector<int>& seen) {
        // Best path to each net: the fewest resistive steps, then any switch at all.
        map<int, pair<int, bool>> best;   // net -> (resistive steps, crossed a switch)
        deque<int> q{n};
        best[n] = {0, false};
        while (!q.empty()) {
            int m = q.front(); q.pop_front();
            auto [k, crossed] = best[m];
            for (int id : tranOf[(size_t)m]) {
                const TranSw& t = trans[(size_t)id];
                if (t.state == 0 || t.state < 0 || (t.state == 2 && !unkOn)) continue;
                int o = t.a == m ? t.b : t.a;
                pair<int, bool> nb{k + (t.resist ? 1 : 0), true};
                auto it = best.find(o);
                if (it != best.end() && it->second.first <= nb.first) continue;
                best[o] = nb;
                q.push_back(o);
            }
        }
        vector<Drv> all;
        for (auto& [m, kc] : best) {
            seen.push_back(m);
            for (Drv d : sigs[(size_t)m].drv) {
                if (kc.second) {
                    d.s0 = tranReduce(d.s0, false);
                    d.s1 = tranReduce(d.s1, false);
                }
                for (int r = 0; r < kc.first; r++) { d.s0 = tranReduce(d.s0, true); d.s1 = tranReduce(d.s1, true); }
                all.push_back(d);
            }
        }
        const Sig& g = sigs[(size_t)n];
        return g.kind != NR_WIRE ? resolveNet(g.kind, all.data(), all.size(), g.w, g.val)
                                 : resolveDrives(all.data(), all.size(), g.w);
    }
    void tranResolve(int s) {
        vector<int> group, scratch;
        Val v = tranValue(s, true, group);
        bool unknown = false;
        for (int m : group)
            for (int id : tranOf[(size_t)m]) if (trans[(size_t)id].state == 2) unknown = true;
        vector<pair<int, Val>> vals;
        for (int m : group) {
            Val on = m == s ? v : tranValue(m, true, scratch);
            if (unknown) {
                Val off = tranValue(m, false, scratch);
                for (int b = 0; b < sigs[(size_t)m].w; b++)
                    if (bitAt(on, b) != bitAt(off, b)) setBit(on, b, 2);
            }
            vals.push_back({m, on});
        }
        for (auto& [m, val] : vals) set(m, val);
    }
    Val get(int s) const {
        return sigs[s].w <= 64 ? make(sigs[s].val.v, sigs[s].val.x, sigs[s].w, sigs[s].val.z)
                               : clipRead(sigs[s].val, sigs[s].w);
    }
    // A nonblocking assignment from the reactive region set goes to re-NBA (4.4.2.8).
    void nba(int s, Val v, int hi = -1, int lo = 0) {
        if (region == R_REACTIVE) { reNbaq.push_back({s, v, hi, lo}); rare = true; }
        else nbaq.push_back({s, v, hi, lo});
    }

    // Delayed drivers and net delays (10.3.3, 28.16). Each (signal, driver)
    // has at most one update in flight; d = -1 is the net delay of signal s,
    // applied to its resolved value. A new value that differs from the one in
    // flight cancels it, and one equal to the current value schedules nothing.
    unordered_map<int, vector<uint64_t>> netDelay;   // signal -> rise, fall, turn-off ticks
    struct Pend { Val v; uint64_t gen = 0; bool live = false; };
    map<pair<int, int>, Pend> pend;
    struct LaterDrv { int s, d; uint64_t gen; uint8_t s0, s1; };
    map<uint64_t, vector<LaterDrv>> laterDrv;
    uint64_t pendGen = 0;
    static bool sameVal(const Val& a, const Val& b) {
        return a.w == b.w && a == b;
    }
    // Table 28-9 for one bit (0, 1, x = 2, z = 3); 10.3.3 for vectors.
    static uint64_t pickDelay(const vector<uint64_t>& dl, const Val& from, const Val& to) {
        size_t n = dl.size();
        if (n == 1) return dl[0];
        uint64_t rise = dl[0], fall = dl[1];
        uint64_t off = n == 3 ? dl[2] : std::min(rise, fall);
        uint64_t lo = std::min({rise, fall, off});
        if (to.w == 1) {
            int b = (to.x & 1) ? ((to.z & 1) ? 3 : 2) : (int)(to.v & 1);
            return b == 1 ? rise : b == 0 ? fall : b == 3 ? off : lo;
        }
        bool allZ = true, zero = true, fromZero = true;
        for (int k = 0; k < to.w; k++) {
            int t = bitAt(to, k), f = bitAt(from, k);   // 0, 1, 2 = x, 3 = z
            if (t != 3) allZ = false;
            if (t != 0) zero = false;
            if (f != 0) fromZero = false;
        }
        if (allZ) return off;
        if (zero && !fromZero) return fall;
        return rise;
    }
    void schedule(int s, int d, Val v, const Val& cur, const vector<uint64_t>& dl, uint8_t s0, uint8_t s1) {
        uint64_t t = sameVal(v, cur) ? 0 : pickDelay(dl, cur, v);
        scheduleIn(s, d, std::move(v), cur, t, s0, s1);
    }
    // The same with the delay t already chosen for the change from cur to v.
    void scheduleIn(int s, int d, Val v, const Val& cur, uint64_t t, uint8_t s0, uint8_t s1) {
        auto& p = pend[{s, d}];
        if (p.live) {
            if (sameVal(v, p.v)) return;   // already on its way
            p.live = false;
        }
        if (sameVal(v, cur)) return;
        if (t == 0) { apply(s, d, v, s0, s1); return; }
        p.v = v;
        p.gen = ++pendGen;
        p.live = true;
        laterDrv[now + t].push_back({s, d, p.gen, s0, s1});
        wheel[now + t];
    }
    void apply(int s, int d, const Val& v, uint8_t s0, uint8_t s1) {
        if (d >= 0) drive(s, d, v, s0, s1);
        else set(s, v);
    }
    // A driver with a delay: schedule its new value.
    void driveLater(int s, int d, Val v, uint8_t s0, uint8_t s1, const vector<uint64_t>& dl) {
        auto& g = sigs[s];
        Val nv = g.w <= 64 ? make(v.v, v.x, g.w, v.z) : clipRead(v, g.w);
        schedule(s, d, nv, g.drv[d].v, dl, s0, s1);
    }
    void runLaterDrv() {
        auto it = laterDrv.find(now);
        if (it == laterDrv.end()) return;
        auto batch = std::move(it->second);
        laterDrv.erase(it);
        for (auto& l : batch) {
            if (l.d == PQ_DECAY) { decayFire(l.s, l.gen); continue; }   // 28.16.2
            if (l.d <= PQ_PATH) { runPulse(l.s, l.d, l.gen); continue; }   // 30.7, 32.4.4
            auto pi = pend.find({l.s, l.d});
            if (pi == pend.end() || !pi->second.live || pi->second.gen != l.gen) continue;   // cancelled
            Val v = pi->second.v;
            pi->second.live = false;
            apply(l.s, l.d, v, l.s0, l.s1);
        }
    }
    // a <= #d b: an NBA in the slot d ticks from now (9.4.5). The empty wheel
    // entry makes the kernel stop at that time even if nothing else does.
    map<uint64_t, vector<NbaItem>> laterNba;
    // Processes ended by disable / disable fork (9.6). Their pending delays
    // are dropped, so they do not keep time going.
    unordered_set<void*> dead;
    void nbaAt(uint64_t t, int s, Val v, int hi = -1, int lo = 0) {
        if (t == now) { nba(s, v, hi, lo); return; }
        laterNba[t].push_back({s, v, hi, lo});
        wheel[t];
    }
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
    // %f %e %g (21.2.1.3): a real, or an integer converted to one; prec < 0
    // is the C default of 6. `R` is a real argument with no specifier.
    static string fmtReal(char c, double d, int minW, int prec, bool zero) {
        char spec[16], buf[512];
        if (c == 'R') { snprintf(buf, sizeof buf, "%#g", d); return buf; }
        if (prec < 0) prec = 6;
        snprintf(spec, sizeof spec, "%%%s*.*%c", zero ? "" : "", c);
        snprintf(buf, sizeof buf, spec, zero ? 0 : minW, prec, d);
        return buf;
    }
    // %b/%o/%h/%d of a value that does not fit in one limb. Digit rules match
    // the single-word formatter: an all-x digit is x, a mixed digit is X.
    static string fmtWide(char c, Val v, int minW, bool zero) {
        auto digits = [&](int bits) {
            int n = (v.w + bits - 1) / bits;
            string s(n, '0');
            for (int d = 0; d < n; d++) {
                int lo = d * bits, hi = std::min(lo + bits, v.w);
                int xz = 0, zz = 0, val = 0, cnt = hi - lo;
                for (int b = 0; b < cnt; b++) {
                    int t = bitAt(v, lo + b);
                    if (t == 3) zz++;
                    else if (t == 2) xz++;
                    else if (t == 1) val |= 1 << b;
                }
                char ch;
                if (zz == cnt) ch = 'z';
                else if (xz + zz == cnt && zz == 0) ch = 'x';
                else if (zz) ch = 'Z';
                else if (xz) ch = 'X';
                else ch = "0123456789abcdef"[val];
                s[n - 1 - d] = ch;
            }
            if (zero) {
                size_t k = s.find_first_not_of('0');
                s = k == string::npos ? "0" : s.substr(k);
            }
            return s;
        };
        string body;
        bool pad = true;
        int natural = 0;
        switch (c) {
            case 'b': body = digits(1); pad = false; break;
            case 'o': body = digits(3); pad = false; break;
            case 'h': case 'x': body = digits(4); pad = false; break;
            case 'c': {
                unsigned char ch = 0;
                for (int b = 0; b < 8; b++) if (bitAt(v, b) == 1) ch |= (unsigned char)(1u << b);
                body = string(1, (char)ch);
                pad = false;
                break;
            }
            case 's': body = bytesToStr(v); pad = false; break;
            case 't':
            case 'd': {
                int nx, nz;
                countXZ(v, nx, nz);
                bool neg = v.sgn && v.w > 0 && !anyX(v) && bitAt(v, v.w - 1) == 1;
                if (nx == v.w) body = "x";
                else if (nx) body = "X";
                else if (nz == v.w) body = "z";
                else if (nz) body = "Z";
                else if (neg) body = "-" + uToDec(negVal(v));
                else body = uToDec(v);
                if (c == 't') natural = 20;
                else {
                    // Same width Icarus uses: floor((w - signed)*log10(2)) + 1, plus the sign.
                    int nb = v.sgn ? v.w - 1 : v.w;
                    natural = (int)((nb * 146L + 484) / 485);
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
    static string fmtVal(char c, Val v, int minW, bool zero) {
        if (v.str) {   // a string (6.16) prints its text with %s, and as bytes otherwise
            if (c == 's') {
                string t = v_str(v);
                if (minW > 0 && (int)t.size() < minW) t = string(minW - t.size(), ' ') + t;
                return t;
            }
            const string& t = v_str(v);
            uint64_t b = 0;
            for (char ch : t) b = (b << 8) | (unsigned char)ch;
            v = make(b, 0, std::max(8, std::min(64, (int)t.size() * 8)));
        }
        if (v.real && (c == 'd' || c == 't' || c == 'b' || c == 'o' || c == 'h' || c == 'x' || c == 'c'))
            v = v_rtoi(v, 64, true, false);   // a real printed as an integer is rounded
        if (v.w > 64) return fmtWide(c, v, minW, zero);
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
    // A time in kernel ticks, in the units, precision and suffix $timeformat
    // set (20.4.3), right-justified to its minimum width (or minW if given).
    static string fmtTime(Val v, int minW) {
        const TimeFmt& tf = timeFmt();
        string body;
        if (v.real) {
            double d = v_real(v) * std::pow(10.0, tf.tickExp - tf.units);
            char buf[512];
            snprintf(buf, sizeof buf, "%.*f", std::max(0, tf.prec), d);
            body = buf;
        } else if (anyX(v)) {
            body = fmtVal('d', v, 0, true);
        } else {
            unsigned __int128 t = (unsigned __int128)(v.v & mask(std::min(64, v.w)));
            auto p10 = [](int n) { unsigned __int128 r = 1; while (n-- > 0) r *= 10; return r; };
            int sh = tf.tickExp - tf.units;   // ticks -> units: * 10^sh
            int pr = std::max(0, tf.prec);
            // value * 10^pr, rounded to an integer
            unsigned __int128 q;
            if (sh + pr >= 0) q = t * p10(sh + pr);
            else { unsigned __int128 d = p10(-(sh + pr)); q = (t + d / 2) / d; }
            auto u128 = [](unsigned __int128 x) {
                string r;
                do { r += char('0' + (int)(x % 10)); x /= 10; } while (x);
                return string(r.rbegin(), r.rend());
            };
            unsigned __int128 P = p10(pr);
            body = u128(q / P);
            if (pr > 0) {
                string f = u128(q % P);
                body += "." + string(pr - f.size(), '0') + f;
            }
        }
        body += tf.suffix;
        int w = minW >= 0 ? minW : tf.width;
        if ((int)body.size() < w) body = string(w - body.size(), ' ') + body;
        return body;
    }
    static inline Sim* self = nullptr;   // the simulator, for %v
    // %v (21.2.1.4): two strength characters and a value. The strength of a
    // net bit comes from its drivers, resolved as the net is (28.12); a
    // variable, or a net with no driver that still has a value (tri0, tri1,
    // supply, trireg), gets the strength its kind implies.
    static string strengthText(const E& a0, const Reader& rd) {
        static const char* MN[8] = {"Hi", "Sm", "Me", "We", "La", "Pu", "St", "Su"};
        E a = a0;
        while (a && a->op == Op::Resize && !a->kids.empty()) a = a->kids[0];
        int sig = -1, bit = 0;
        if (a && a->op == Op::Ref && !isRel(a->sig) && !a->at) sig = a->sig;
        else if (a && a->op == Op::Slice && a->hi == a->lo && !a->kids.empty() && a->kids[0] &&
                 a->kids[0]->op == Op::Ref && !isRel(a->kids[0]->sig) && !a->kids[0]->at) {
            sig = a->kids[0]->sig; bit = a->lo;
        }
        int v = bitAt(eval(a0, rd), 0);   // 0, 1, 2 = x, 3 = z
        SBit sb;
        bool have = false;
        if (sig >= 0 && self) {
            const Sig& g = self->sigs[sig];
            if (!g.drv.empty() && g.kind == NR_WIRE) {
                for (const Drv& d : g.drv) {
                    int t = bitAt(d.v, bit);
                    SBit cur = t == 3 ? SBit{} : t == 2 ? sbit_x(d.s0, d.s1) : t == 1 ? sbit_1(d.s1) : sbit_0(d.s0);
                    sb = sbit_resolv(sb, cur);
                }
                have = true;
            } else if (g.net) {
                unsigned st = g.kind == NR_SUPPLY0 || g.kind == NR_SUPPLY1 ? STR_SUPPLY
                            : g.kind == NR_TRIREG && g.drv.empty() ? self->chargeOf(sig)
                            : (g.kind == NR_TRI0 || g.kind == NR_TRI1) && v == (g.kind == NR_TRI1 ? 1 : 0) ? STR_PULL
                            : STR_STRONG;
                if (v == 3) sb = SBit{};
                else if (g.kind == NR_TRIREG && !g.drv.empty()) {   // a driven trireg: its drivers, else its charge
                    for (const Drv& d : g.drv) {
                        int t = bitAt(d.v, bit);
                        SBit cur = t == 3 ? SBit{} : t == 2 ? sbit_x(d.s0, d.s1) : t == 1 ? sbit_1(d.s1) : sbit_0(d.s0);
                        sb = sbit_resolv(sb, cur);
                    }
                    if (sbit_z(sb)) {
                        uint8_t c = self->chargeOf(sig);
                        sb = v == 1 ? sbit_1(c) : v == 0 ? sbit_0(c) : sbit_x(c, c);
                    }
                } else sb = v == 1 ? sbit_1(st) : v == 0 ? sbit_0(st) : sbit_x(st, st);
                have = true;
            }
        }
        if (!have) sb = v == 3 ? SBit{} : v == 1 ? sbit_1(STR_STRONG) : v == 0 ? sbit_0(STR_STRONG) : sbit_x(STR_STRONG, STR_STRONG);
        if (sbit_z(sb)) return "HiZ";
        int lg = sbit_logic(sb);
        if (lg == 0 || lg == 1) return string(MN[sb.s0]) + (lg ? "1" : "0");
        // x: one strength on both sides, else the two levels as digits
        if (sb.s0 == sb.s1) return string(MN[sb.s0]) + "X";
        return std::to_string(sb.s0) + std::to_string(sb.s1) + "X";
    }
    // The text of a format and its arguments ($display, $sformatf).
    static string sformat(const string& fmt, const vector<E>& args, const Reader& rd) {
        std::ostringstream cout;
        size_t ai = 0;
        for (size_t i = 0; i < fmt.size(); ++i) {
            char c = fmt[i];
            if (c != '%') { cout << c; continue; }
            size_t start = i;
            if (i + 1 >= fmt.size()) { cout << '%'; break; }
            c = fmt[++i];
            if (c == '%') { cout << '%'; continue; }
            bool zero = false, left = false;
            int minW = 0, prec = -1;
            if (c == '-' && i + 1 < fmt.size()) { left = true; c = fmt[++i]; }   // %-8d: pad on the right
            if (c == '0') { zero = true; if (i + 1 < fmt.size()) c = fmt[++i]; }
            while (isdigit((unsigned char)c) && i + 1 < fmt.size()) { minW = minW * 10 + (c - '0'); c = fmt[++i]; }
            if (c == '.' && i + 1 < fmt.size()) {
                prec = 0;
                c = fmt[++i];
                while (isdigit((unsigned char)c) && i + 1 < fmt.size()) { prec = prec * 10 + (c - '0'); c = fmt[++i]; }
            }
            if (c == 'R') {   // internal: a real argument with no specifier
                if (ai >= args.size()) continue;
                Val v = eval(args[ai++], rd);
                cout << fmtReal('R', v_real(v), 0, -1, false);
                continue;
            }
            if (c == 'Q' && i + 1 < fmt.size()) {   // internal: %p of a container (21.2.1.6)
                char style = fmt[++i];
                if (ai >= args.size()) continue;
                const E& h = args[ai++];
                E slot, sub;   // %QF: each element set into slot, printed by sub
                if (style == 'F') {
                    if (ai + 2 > args.size()) continue;
                    slot = args[ai++];
                    sub = args[ai++];
                }
                auto one = [&](Val v, char st) -> string {
                    if (st == 'F') {   // the element in place of the slot
                        std::function<E(const E&)> put = [&](const E& e) -> E {
                            if (!e) return e;
                            if (e->op == Op::Ref && !e->at && e->sig == slot->sig) {
                                Val k = v;
                                k.sgn = e->sgn;
                                return konst(k);
                            }
                            bool ch = false;
                            std::vector<E> kids;
                            for (auto& kd : e->kids) { kids.push_back(put(kd)); ch |= kids.back() != kd; }
                            if (!ch) return e;
                            auto c = std::make_shared<Expr>(*e);
                            c->kids = std::move(kids);
                            return c;
                        };
                        return v_str(eval(put(sub), rd));
                    }
                    if (st == 's') { v.str = true; return "\"" + v_str(v) + "\""; }
                    if (st == 'R') return fmtReal('R', v_real(v), 0, -1, false);
                    return fmtVal('d', v, 0, true);
                };
                // A container of containers prints each element container nested.
                std::function<void(const DynObj&)> show = [&](const DynObj& o) {
                    auto el = [&](const Val& v) {
                        if (!o.elem) { cout << one(v, style); return; }
                        if (v.x || !v.v) { cout << "'{}"; return; }
                        show(dynStore()[(int)v.v]);
                    };
                    cout << "'{";
                    if (o.kind == 2) {
                        bool first = true;
                        for (const auto& [k, v] : o.map) {
                            if (!first) cout << ", ";
                            first = false;
                            cout << one(o.keyVal(k), o.strKey ? 's' : 'd') << ':';
                            el(v);
                        }
                    } else {
                        for (size_t k = 0; k < o.vec.size(); k++) { if (k) cout << ", "; el(o.vec[k]); }
                    }
                    cout << '}';
                };
                show(dynStore()[R(h->sig)]);   // R: a property through a handle, or an automatic container
                continue;
            }
            if ((c == 't' || c == 'T') && ai < args.size()) {   // %t (20.4.3): ticks, printed per $timeformat
                Val v = eval(args[ai++], rd);
                string out = fmtTime(v, (zero || minW > 0) ? minW : -1);
                if (left && (int)out.size() < minW) out += string(minW - out.size(), ' ');
                cout << out;
                continue;
            }
            char lc = (char)tolower((unsigned char)c);
            if (lc == 'm') { cout << "top"; continue; }
            if (lc == 'v' && ai < args.size()) {   // %v (21.2.1.4): strength of a scalar net
                cout << strengthText(args[ai++], rd);
                continue;
            }
            bool known = string_view("bohxdcstfeg").find(lc) != string_view::npos;
            if (!known || ai >= args.size()) { cout << fmt.substr(start, i - start + 1); continue; }
            Val v = eval(args[ai++], rd);
            string out = (lc == 'f' || lc == 'e' || lc == 'g') ? fmtReal(lc, v_real(v), left ? 0 : minW, prec, zero)
                                                               : fmtVal(lc, v, left ? 1 : minW, zero);
            if (left && (int)out.size() < minW) out += string(minW - out.size(), ' ');
            cout << out;
        }
        return cout.str();
    }
    void printSys(const string& fmt, const vector<E>& args, bool newline = true) {
        std::cout << sformat(fmt, args, [&](int s) { return get(s); });
        if (newline) std::cout << '\n';
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
        observed = true;
        mon.fmt = fmt;
        mon.args = args;
        mon.watch.clear();
        for (const auto& a : args) collectRefs(a, mon.watch);
        queueMon();
    }
    static void closeHook(uint32_t fd);
    void strobeF(uint32_t fd, const string& fmt, const vector<E>& args) {
        Post p{false, fmt, args, true, fd};
        postq.push_back(std::move(p));
    }
    void queueFMon(size_t k) {
        FMon& m = fmons[k];
        if (!m.on || m.queued) return;
        Post p{false, m.fmt, m.args, true, m.fd, (int)k};
        postq.push_back(std::move(p));
        m.queued = true;
    }
    void armFMon(uint32_t fd, const string& fmt, const vector<E>& args) {
        FMon m;
        m.fd = fd;
        m.fmt = fmt;
        m.args = args;
        for (const auto& a : args) collectRefs(a, m.watch);
        fmons.push_back(std::move(m));
        observed = true;
        queueFMon(fmons.size() - 1);
    }
    // $fclose: the closed descriptor's pending $fstrobe / $fmonitor lines and
    // its $fmonitors end. Closing channels of a multichannel descriptor drops
    // them from it.
    void fileClosed(uint32_t fd) {
        auto left = [&](uint32_t d) -> uint32_t {
            if ((fd & 0x80000000u) || (d & 0x80000000u)) return d == fd ? 0 : d;
            return d & ~(fd & ~1u);   // channel 0 (stdout) is never closed
        };
        for (auto& m : fmons) if (m.on && !(m.fd = left(m.fd))) m.on = false;
        for (auto it = postq.begin(); it != postq.end();) {
            if (it->file && !(it->fd = left(it->fd))) {
                if (it->fmon >= 0) fmons[(size_t)it->fmon].queued = false;
                it = postq.erase(it);
            } else ++it;
        }
    }
    void noteChange(int s) {
        if (__builtin_expect(!fmons.empty(), 0))
            for (size_t k = 0; k < fmons.size(); k++)
                if (fmons[k].on && !fmons[k].queued)
                    for (int w : fmons[k].watch) if (w == s) { queueFMon(k); break; }
        if (!mon.on || mon.queued) return;
        for (int w : mon.watch) if (w == s) { queueMon(); return; }
    }
    void drainPost() {
        while (!postq.empty()) {
            Post p = move(postq.front());
            postq.pop_front();
            if (p.mon) mon.queued = false;
            if (p.file) {
                if (p.fmon >= 0) fmons[(size_t)p.fmon].queued = false;
                fileWrite(p.fd, sformat(p.fmt, p.args, [&](int s) { return get(s); }) + "\n");
                continue;
            }
            printSys(p.fmt, p.args);
        }
    }

    // A value's w bits as '0' / '1' / 'x' / 'z' characters, MSB first.
    static void putBits(char* p, const Val& val, int w) {
        if (w <= 64) {
            uint64_t v = val.v, x = val.x, z = val.z;
            if (!x && std::endian::native == std::endian::little) {
                // Known bits: eight characters per table lookup.
                int i = w;
                for (int r = i % 8; r > 0; r--) *p++ = char('0' + (v >> --i & 1));
                for (; i > 0; i -= 8, p += 8) memcpy(p, &kBitChars[(v >> (i - 8)) & 0xFF], 8);
            } else
                for (int i = w - 1; i >= 0; i--)
                    *p++ = (x >> i & 1) ? ((z >> i & 1) ? 'z' : 'x') : char('0' + (v >> i & 1));
            return;
        }
        for (int i = w - 1; i >= 0; i--) {
            int t = bitAt(val, i);
            *p++ = t == 3 ? 'z' : t == 2 ? 'x' : char('0' + t);
        }
    }
    // Byte b as eight ASCII digits, MSB first, in memory order.
    static constexpr std::array<uint64_t, 256> kBitChars = [] {
        std::array<uint64_t, 256> t{};
        for (int b = 0; b < 256; b++)
            for (int k = 0; k < 8; k++) t[b] |= uint64_t('0' + (b >> (7 - k) & 1)) << (8 * k);
        return t;
    }();
    // One value line, no timestamp. Shared so $dumpvars and later changes match.
    void writeVal(int s) { writeVal(s, sigs[s].val); }
    void writeVal(int s, const Val& val) {
        const int w = sigs[s].w;
#ifdef MINISIM_FST
        if (fst) {
            // FST takes one '0' / '1' / 'x' / 'z' character per bit, MSB first.
            fstBuf.resize((size_t)w);
            putBits(fstBuf.data(), val, w);
            fstWriterEmitValueChange((fstWriterContext*)fst, fstH[s], fstBuf.data());
            // Hand a block to the writer thread every few MB, so it compresses
            // while the simulation goes on. libfst's own block is 128 MB, so a
            // run wrote one or two blocks and then flushed the rest at close.
            fstPending += (size_t)w + 8;
            if (fstPending >= (16u << 20)) { fstWriterFlushContext((fstWriterContext*)fst); fstPending = 0; }
            return;
        }
#endif
        // Signals made after header() (frames, objects) get their id here.
        if ((size_t)s >= vcdIds.size()) {
            size_t from = vcdIds.size();
            vcdIds.resize(sigs.size());
            for (size_t i = from; i < vcdIds.size(); i++) vcdIds[i] = vcdId(i);
        }
        const string& id = vcdIds[s];
        if (w == 1) {
            vbuf += (val.x & 1) ? ((val.z & 1) ? 'z' : 'x') : char('0' + (val.v & 1));
        } else {
            size_t at = vbuf.size();
            vbuf.resize(at + 1 + (size_t)w);
            char* p = vbuf.data() + at;
            *p++ = 'b';
            putBits(p, val, w);
            vbuf += ' ';
        }
        vbuf += id;
        vbuf += '\n';
        if (vbuf.size() >= (1u << 16)) flushVcd();
    }
    // VCD identifier: base-94 over the printable characters '!'..'~'.
    static string vcdId(size_t i) {
        string id;
        do { id += char('!' + i % 94); i /= 94; } while (i);
        return id;
    }
    void dump(int s) { dump(s, sigs[s].val); }
    void dump(int s, const Val& v) {
        if (sigs[s].hide) return;
#ifdef MINISIM_FST
        if (fst) {
            if (dumpOff || !dumped((size_t)s)) return;
            if (lastDump != now) { fstWriterEmitTimeChange((fstWriterContext*)fst, now); lastDump = now; }
            writeVal(s, v);
            return;
        }
#endif
        if (!vcd.is_open() || !vcd) return;
        if (dumpOff || dumpStopped || !dumped((size_t)s)) return;
        stamp();
        writeVal(s, v);
        if (dumpLimit && (uint64_t)vcd.tellp() + vbuf.size() >= dumpLimit) {
            vbuf += "$comment Note: $dumplimit reached; dumping stops $end\n";
            dumpStopped = true;
        }
    }
    void stamp() {
#ifdef MINISIM_FST
        if (fst) { if (lastDump != now) { fstWriterEmitTimeChange((fstWriterContext*)fst, now); lastDump = now; } return; }
#endif
        if (lastDump != now) {
            char t[24];
            auto r = std::to_chars(t, t + sizeof t, now);
            vbuf += '#';
            vbuf.append(t, r.ptr);
            vbuf += '\n';
            lastDump = now;
        }
    }
    // Every dumped signal, as X ($dumpoff) or its value ($dumpon, $dumpall).
    void dumpBlock(const char* kw, bool asX) {
        stamp();
        if (!fst) { vbuf += kw; vbuf += '\n'; }
        for (size_t i = 0; i < sigs.size(); i++) {
            if (!dumped(i)) continue;
            if (asX) writeVal((int)i, allx(sigs[i].w));
            else writeVal((int)i);
        }
        if (!fst) vbuf += "$end\n";
    }
    // Selects signals for $dumpvars: sel holds name prefixes of scopes
    // (levels deep, 0 = all below) and exact names of variables.
    void dumpSelect(int64_t levels, const vector<pair<string, bool>>& sel) {
        if (dumpSel.size() != sigs.size()) dumpSel.assign(sigs.size(), 0);
        vector<pair<string, bool>> all;
        if (sel.empty()) all.push_back({"", false});
        const auto& use = sel.empty() ? all : sel;
        if (levels < 0) levels = 0;
        for (size_t i = 0; i < sigs.size(); i++) {
            const string& n = sigs[i].name;
            for (const auto& [p, var] : use) {
                if (n.compare(0, p.size(), p) != 0) continue;
                if (var) {
                    if (n.size() == p.size() || n[p.size()] == '[') { dumpSel[i] = 1; break; }
                    continue;
                }
                if (p.find("::") == string::npos && n.find("::") != string::npos && !sel.empty()) continue;
                int depth = 0, br = 0;
                for (size_t k = p.size(); k < n.size(); k++) {
                    if (n[k] == '[') br++;
                    else if (n[k] == ']') br--;
                    else if (n[k] == '.' && !br) depth++;
                }
                if (levels == 0 || depth < levels) { dumpSel[i] = 1; break; }
            }
        }
    }
    void dumpTask(int cmd, const string& str, int64_t num, const vector<pair<string, bool>>& sel) {
        switch (cmd) {
            case DC_FILE:
                if (dumpCli || waving() || dumpPend) return;
                if (!str.empty()) dumpFile = str;
                return;
            case DC_VARS:
                if (dumpCli) return;
                if (waving()) { cerr << "msim: $dumpvars after the dump has started is ignored (21.7.1.2)\n"; return; }
                dumpPend = true;
                rare = true;   // slotTail() opens the file at the end of this slot
                if (sel.empty() && num < 0) { dumpSel.assign(sigs.size(), 1); return; }
                dumpSelect(num, sel);
                return;
            case DC_OFF:
                if (dumpOff) return;
                dumpOff = true;
                if (!waving() || dumpStopped) return;
#ifdef MINISIM_FST
                if (fst) { stamp(); fstWriterEmitDumpActive((fstWriterContext*)fst, 0); return; }
#endif
                dumpBlock("$dumpoff", true);
                return;
            case DC_ON:
                if (!dumpOff) return;
                dumpOff = false;
                if (!waving() || dumpStopped) return;
#ifdef MINISIM_FST
                if (fst) { stamp(); fstWriterEmitDumpActive((fstWriterContext*)fst, 1); }
#endif
                dumpBlock("$dumpon", false);
                return;
            case DC_ALL:
                if (waving() && !dumpOff && !dumpStopped) dumpBlock("$dumpall", false);
                return;
            case DC_FLUSH:
#ifdef MINISIM_FST
                if (fst) { fstWriterFlushContext((fstWriterContext*)fst); return; }
#endif
                if (vcd.is_open()) { flushVcd(); vcd.flush(); }
                return;
            case DC_LIMIT:
                dumpLimit = num > 0 ? (uint64_t)num : 0;
                return;
        }
    }
    // Extended VCD (21.7.3): one file per $dumpports call, holding the ports
    // of the modules it names. Values are compared at the end of each time
    // slot, so a run without $dumpports pays nothing. Which side drives a
    // port is not looked at: an input shows D/U/N/Z, an output L/H/X/T and
    // an inout 0/1/?/F, each bit at strong strength (or highz for z).
    struct EvcdPort { int sig; char dir; string id; };
    struct Evcd {
        string path;
        std::ofstream f;
        vector<EvcdPort> ports;
        vector<string> last;
        bool started = false, off = false, stopped = false;
        uint64_t limit = 0, lastT = ~0ULL;
    };
    std::deque<Evcd> evcds;
    string evcdValue(const EvcdPort& p, bool asX) {
        const int w = sigs[p.sig].w;
        string bits((size_t)w, 'x');
        if (!asX) putBits(bits.data(), sigs[p.sig].val, w);
        string v = "p", s0, s1;
        for (char c : bits) {
            int k = c == '0' ? 0 : c == '1' ? 1 : c == 'z' ? 3 : 2;
            v += (p.dir == 'i' ? "DUNZ" : p.dir == 'o' ? "LHXT" : "01?F")[k];
            s0 += k == 0 || k == 2 ? '6' : '0';
            s1 += k == 1 || k == 2 ? '6' : '0';
        }
        return v + " " + s0 + " " + s1 + " " + p.id + "\n";
    }
    void evcdStamp(Evcd& e) {
        if (e.lastT != now) { e.f << '#' << now << '\n'; e.lastT = now; }
    }
    void evcdBlock(Evcd& e, const char* kw, bool asX) {
        evcdStamp(e);
        e.f << kw << '\n';
        for (size_t k = 0; k < e.ports.size(); k++) {
            string v = evcdValue(e.ports[k], asX);
            e.f << v;
            if (!asX) e.last[k] = v;
        }
        e.f << "$end\n";
    }
    // The end of a time slot: the first values, then the ports that changed.
    void evcdTail() {
        for (Evcd& e : evcds) {
            if (!e.f.is_open() || e.stopped) continue;
            if (!e.started) {
                e.started = true;
                evcdBlock(e, "$dumpports", false);
                if (e.off) evcdBlock(e, "$dumpportsoff", true);
                continue;
            }
            if (e.off) continue;
            for (size_t k = 0; k < e.ports.size(); k++) {
                string v = evcdValue(e.ports[k], false);
                if (v == e.last[k]) continue;
                evcdStamp(e);
                e.f << v;
                e.last[k] = std::move(v);
            }
            if (e.limit && (uint64_t)e.f.tellp() >= e.limit) {
                e.f << "$comment Note: $dumpportslimit reached; dumping stops $end\n";
                e.stopped = true;
            }
        }
    }
    void evcdClose() {
        if (evcds.empty()) return;
        evcdTail();
        for (Evcd& e : evcds)
            if (e.f.is_open()) { e.f << "$vcdclose #" << now << " $end\n"; e.f.close(); }
        evcds.clear();
    }
    void dumpPorts(int cmd, const string& file, int64_t num, const vector<pair<string, bool>>& sel) {
        if (cmd == DC_PORTS) {
            string path = file.empty() ? "dumpports.vcd" : file;
            for (Evcd& e : evcds)
                if (e.path == path) { cerr << "msim: $dumpports: " << path << " is already being written; the call is ignored (21.7.3.1)\n"; return; }
            evcds.emplace_back();
            Evcd& e = evcds.back();
            e.path = path;
            e.f.open(path);
            if (!e.f) { cerr << "msim: $dumpports: cannot write " << path << "\n"; evcds.pop_back(); return; }
            std::unordered_map<string, int> byName;
            for (size_t i = 0; i < sigs.size(); i++) byName.emplace(sigs[i].name, (int)i);
            string top = vcdTop.empty() ? string("top") : vcdTop;
            std::ostringstream defs;
            string scope, scopes;
            for (const auto& [enc, vec] : sel) {
                vector<string> fld;
                size_t b = 0, t;
                while ((t = enc.find('\t', b)) != string::npos) { fld.push_back(enc.substr(b, t - b)); b = t + 1; }
                fld.push_back(enc.substr(b));
                if (fld.size() != 6) continue;
                if (fld[5].size() > 1 && fld[5][0] == '#') {   // the signal id itself
                    int id = std::atoi(fld[5].c_str() + 1);
                    if (id >= 0 && (size_t)id < sigs.size()) byName[fld[5]] = id;
                }
                auto it = byName.find(fld[5]);
                if (it == byName.end()) { cerr << "msim: $dumpports: port " << fld[1] << "." << fld[2] << " has no signal and is left out\n"; continue; }
                if (fld[1] != scope) {
                    if (!scope.empty()) defs << "$upscope $end\n";
                    scope = fld[1];
                    scopes += (scopes.empty() ? "" : ", ") + scope;
                    defs << "$scope module " << scope << " $end\n";
                }
                EvcdPort p{it->second, fld[0][0], "<" + std::to_string(e.ports.size())};
                defs << "$var port " << (vec ? "[" + fld[3] + ":" + fld[4] + "]" : string("1")) << " " << p.id << " " << fld[2] << " $end\n";
                e.ports.push_back(p);
            }
            if (!scope.empty()) defs << "$upscope $end\n";
            e.last.assign(e.ports.size(), string());
            e.f << "$version msim $dumpports(" << scopes << ", \"" << path << "\") $end\n"
                << "$timescale " << timescale << " $end\n" << defs.str() << "$enddefinitions $end\n";
            rare = true;   // slotTail() writes the first values at the end of this slot
            return;
        }
        for (Evcd& e : evcds) {
            if (!file.empty() && e.path != file) continue;   // an unknown name: ignored (21.7.3.7)
            switch (cmd) {
                case DC_POFF:
                    if (e.off) break;
                    e.off = true;
                    if (e.started && !e.stopped) evcdBlock(e, "$dumpportsoff", true);
                    break;
                case DC_PON:
                    if (!e.off) break;
                    e.off = false;
                    if (e.started && !e.stopped) evcdBlock(e, "$dumpportson", false);
                    break;
                case DC_PALL:
                    if (e.started && !e.off && !e.stopped) evcdBlock(e, "$dumpportsall", false);
                    break;
                case DC_PFLUSH: e.f.flush(); break;
                case DC_PLIMIT: e.limit = num > 0 ? (uint64_t)num : 0; break;
            }
        }
    }
    // $dumpvars without -vcd: the header and the values at the end of the slot.
    void startDump() {
        dumpPend = false;
        vcdPath = dumpFile;
        header();
        if (!waving()) return;
        if (!fst) { lastDump = now - 1; stamp(); }
        dumpvars();
        if (dumpOff) {
#ifdef MINISIM_FST
            if (fst) { fstWriterEmitDumpActive((fstWriterContext*)fst, 0); return; }
#endif
            dumpBlock("$dumpoff", true);
        }
    }
    // set() only writes a line when a value changes, so a signal still holding
    // its starting value (cnt stays X until the first clock) never appears.
    // Checkpoint every signal at time 0, before the first event. lastDump is
    // left alone so the change records that follow stay byte-for-byte the same.
    void dumpvars() {
#ifdef MINISIM_FST
        if (fst) {
            // FST has no $dumpvars block: the time-0 values are ordinary changes.
            fstWriterEmitTimeChange((fstWriterContext*)fst, now);
            for (size_t i = 0; i < sigs.size(); i++) if (dumped(i)) writeVal((int)i);
            lastDump = now;
            return;
        }
#endif
        if (!vcd.is_open() || !vcd) return;
        vbuf += "$dumpvars\n";
        for (size_t i = 0; i < sigs.size(); i++) if (dumped(i)) writeVal((int)i);
        vbuf += "$end\n";
    }

    // force / assign (10.6). A held signal ignores every other write; its
    // value follows the force expression (or, if there is no force, the
    // procedural continuous assignment), re-evaluated when one of the
    // expression's signals changes. release lets drivers resolve the net
    // again; a variable keeps its value until it is next written.
    // force v[3:0] = e holds those bits only (SF_PART): other writes still
    // change the other bits.
    enum : uint8_t { SF_HELD = 1, SF_LISTEN = 2, SF_PART = 4, SF_VPI = 8 };   // SF_VPI: a VPI value-change callback watches it
    struct PartHold { int hi, lo; E fe, ae; };
    struct Hold { E fe, ae; vector<PartHold> parts; };
    unordered_map<int, Hold> holds;
    unordered_map<int, vector<int>> holdListeners;   // signal -> held signals whose expression reads it
    void listenTo(int s, const E& e) {
        vector<int> refs;
        collectRefs(e, refs);
        for (int r : refs) {
            auto& v = holdListeners[r];
            if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
            sigs[r].fl |= SF_LISTEN;
        }
    }
    // A write to a partly held signal keeps the held bits.
    Val keepHeld(int s, Val nv) {
        auto it = holds.find(s);
        if (it == holds.end()) return nv;
        for (auto& p : it->second.parts)
            if (p.fe || p.ae) nv = v_insert(nv, v_slice(sigs[s].val, p.hi, p.lo), p.hi, p.lo);
        return nv;
    }
    void applyHold(int s) {
        auto it = holds.find(s);
        if (it == holds.end()) return;
        auto rd = [&](int x) { return get(x); };
        const E& e = it->second.fe ? it->second.fe : it->second.ae;
        uint8_t fl = sigs[s].fl;
        if (e) {
            Val v = eval(e, rd);
            sigs[s].fl &= (uint8_t)~(SF_HELD | SF_PART);
            set(s, v);
            sigs[s].fl = fl;
            return;
        }
        Val v = sigs[s].val;
        for (auto& p : it->second.parts) {
            const E& pe = p.fe ? p.fe : p.ae;
            if (pe) v = v_insert(v, eval(pe, rd), p.hi, p.lo);
        }
        sigs[s].fl &= (uint8_t)~(SF_HELD | SF_PART);
        set(s, v);
        sigs[s].fl = fl;
    }
    // Recompute the flags of s from its holds.
    void holdFlags(int s, const Hold& h) {
        sigs[s].fl &= (uint8_t)~(SF_HELD | SF_PART);
        if (h.fe || h.ae) { sigs[s].fl |= SF_HELD; return; }
        for (auto& p : h.parts) if (p.fe || p.ae) { sigs[s].fl |= SF_PART; return; }
    }
    void holdOn(int s, const E& e, bool force, int hi = -1, int lo = 0) {
        auto& h = holds[s];
        if (hi < 0) (force ? h.fe : h.ae) = e;
        else {
            PartHold* ph = nullptr;
            for (auto& p : h.parts) if (p.hi == hi && p.lo == lo) ph = &p;
            if (!ph) { h.parts.push_back({hi, lo, nullptr, nullptr}); ph = &h.parts.back(); }
            (force ? ph->fe : ph->ae) = e;
        }
        listenTo(s, e);
        holdFlags(s, h);
        applyHold(s);
    }
    void holdOff(int s, bool force, int hi = -1, int lo = 0) {
        auto it = holds.find(s);
        if (it == holds.end()) return;
        Hold& h = it->second;
        if (hi < 0) {
            (force ? h.fe : h.ae) = nullptr;
            // release of the whole signal also ends forces of its parts
            if (force) for (auto& p : h.parts) p.fe = nullptr;
        } else {
            for (auto& p : h.parts) if (p.hi == hi && p.lo == lo) (force ? p.fe : p.ae) = nullptr;
        }
        std::erase_if(h.parts, [](const PartHold& p) { return !p.fe && !p.ae; });
        holdFlags(s, h);
        if (h.fe || h.ae) { applyHold(s); return; }
        if (h.parts.empty()) holds.erase(it);
        else applyHold(s);
        auto& g = sigs[s];
        if (!g.drv.empty()) {   // a net (or a continuously assigned variable): its drivers take over
            Val r = g.kind != NR_WIRE ? resolveNet(g.kind, g.drv.data(), g.drv.size(), g.w, g.val)
                                      : resolveDrives(g.drv.data(), g.drv.size(), g.w);
            set(s, r);
        }
    }
    void heldInputChanged(int s) {
        auto it = holdListeners.find(s);
        if (it == holdListeners.end()) return;
        vector<int> targets = it->second;
        for (int t : targets) applyHold(t);
    }

    // Toggle coverage (msim -cov): per signal, the bits seen rising 0->1 and
    // falling 1->0 (X and Z do not count), one 64-bit word per 64 bits.
    vector<vector<uint64_t>> togRise, togFall;
    // FSM coverage (msim -cov): for each watched state register, how often
    // it entered each value and made each change. A signal is written by one
    // thread at a time, so its two maps need no lock.
    vector<int> fsmSigs, fsmOf;   // watched signals; signal -> index in fsmSigs, or -1
    vector<std::map<uint64_t, uint64_t>> fsmSeen;
    vector<std::map<std::pair<uint64_t, uint64_t>, uint64_t>> fsmTrans;
    // A one-hot register (hot[i]) is followed from one one-hot value to the
    // next: it may be cleared and have a bit set in the same time step.
    vector<char> fsmHot, fsmHasLast;
    vector<uint64_t> fsmLast;
    void fsmWatch(const vector<int>& watch, const vector<char>& hot = {}) {
        fsmSigs = watch;
        fsmHot = hot;
        fsmHot.resize(watch.size(), 0);
        fsmHasLast.assign(watch.size(), 0);
        fsmLast.assign(watch.size(), 0);
        fsmOf.assign(sigs.size(), -1);
        for (size_t i = 0; i < watch.size(); i++) fsmOf[watch[i]] = (int)i;
        fsmSeen.assign(watch.size(), {});
        fsmTrans.assign(watch.size(), {});
    }
    void covFsm(int f, const Val& o, const Val& n) {
        if (fsmHot[f]) {
            auto hot = [](const Val& v) { return !(v.x | v.z) && v.v && !(v.v & (v.v - 1)); };
            if (!fsmHasLast[f] && hot(o)) { fsmHasLast[f] = 1; fsmLast[f] = o.v; fsmSeen[f][o.v] = 1; }
            if (!hot(n) || (fsmHasLast[f] && fsmLast[f] == n.v)) return;
            if (fsmHasLast[f]) fsmTrans[f][{fsmLast[f], n.v}]++;
            fsmSeen[f][n.v]++;
            fsmHasLast[f] = 1;
            fsmLast[f] = n.v;
            return;
        }
        if (n.x | n.z) return;
        if (!(o.x | o.z)) {
            fsmSeen[f].try_emplace(o.v, 1);   // the value it started with
            fsmTrans[f][{o.v, n.v}]++;
        }
        fsmSeen[f][n.v]++;
    }
    // A register that never changed is in the state it started in.
    void fsmFinish() {
        for (size_t f = 0; f < fsmSigs.size(); f++) {
            const Val& v = sigs[fsmSigs[f]].val;
            if (fsmSeen[f].empty() && !(v.x | v.z)) fsmSeen[f][v.v] = 1;
        }
    }
    void covToggle(int s, const Val& o, const Val& n) {
        if ((size_t)s < fsmOf.size() && fsmOf[s] >= 0) covFsm(fsmOf[s], o, n);
        if (togRise.size() <= (size_t)s) { togRise.resize(sigs.size()); togFall.resize(sigs.size()); }
        int w = sigs[s].w;
        auto& R = togRise[s];
        auto& F = togFall[s];
        if (R.empty()) { R.assign((size_t)(w + 63) / 64, 0); F.assign(R.size(), 0); }
        if (w <= 64) {
            uint64_t known = ~(o.x | n.x);
            R[0] |= ~o.v & n.v & known;
            F[0] |= o.v & ~n.v & known;
            return;
        }
        for (int i = 0; i < w; i++) {
            int a = bitAt(o, i), b = bitAt(n, i);
            if (a == 0 && b == 1) R[(size_t)i / 64] |= 1ULL << (i % 64);
            else if (a == 1 && b == 0) F[(size_t)i / 64] |= 1ULL << (i % 64);
        }
    }

    // VPI (36-38): hooks set by vpi.inc. vpiChange runs the value-change
    // callbacks of a signal; vpiRegion runs the callbacks of a point in the
    // time slot (VR_*) and says whether any ran. vpiAt counts the
    // cbAfterDelay callbacks due at each time, so that slot is not skipped.
    enum { VR_START, VR_RW, VR_RO };
    void (*vpiChange)(int) = nullptr;
    bool (*vpiRegion)(int) = nullptr;
    std::map<uint64_t, int> vpiAt;
    void set(int s, Val nv) {
        Sig& g = sigs[s];
        if (__builtin_expect(g.fl & (SF_HELD | SF_PART), 0)) {
            if (g.fl & SF_HELD) return;   // forced: other writes are ignored
            nv = keepHeld(s, nv);         // only some bits are held
        }
        int o, n;
        if (g.w <= 64) {
            uint64_t m = g.w >= 64 ? ~0ULL : ((1ULL << g.w) - 1);
            nv.v &= m; nv.x &= m; nv.z &= m; nv.sgn = false;
            nv.hi.clear();
            if (tlPart) { setLocal(s, nv); return; }
            Val& cur = g.val;   // no limbs: compare and copy the words, as == and = would
            if (cur.hi.empty() ? nv.v == cur.v && nv.x == cur.x && nv.z == cur.z : nv == cur) return;
            o = bit0(cur); n = bit0(nv);
            if (g_cov) covToggle(s, cur, nv);
            if (cur.hi.empty()) {
                cur.v = nv.v; cur.x = nv.x; cur.z = nv.z; cur.w = nv.w;
                cur.sgn = false; cur.real = nv.real; cur.str = nv.str;
            } else cur = nv;
        } else {
            nv = maskWidth(nv, g.w);
            if (tlPart) { setLocal(s, nv); return; }
            if (nv == g.val) return;
            o = bit0(g.val); n = bit0(nv);
            if (g_cov) covToggle(s, g.val, nv);
            g.val = nv;
        }
        if (__builtin_expect(!pathIn.empty(), 0) && (size_t)s < pathIn.size() && pathIn[s]) lastChg[s] = now;
        if (__builtin_expect(edgeTrack, 0)) {   // msim -xprop
            g.p0 = (uint8_t)o; g.at = now;
            if (o == 2 || n == 2) xAt = now;
        }
        dump(s);
        noteChange(s);  // $monitor: one postponed line if a watched signal changed
        if (__builtin_expect(sigs[s].fl & SF_VPI, 0)) vpiChange(s);   // cbValueChange (38.36.1)
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
        if ((size_t)s < combOf.size())
            for (int r : combOf[s])
                if (r != curComb) {
#ifdef MSIM_FWSTAT
                    if (r == fastComb) { static struct St { std::map<std::string, long> m; ~St() { for (auto& [k, c] : m) std::fprintf(stderr, "MARK %ld %s\n", c, k.c_str()); } } st; st.m[sigs[s].name]++; }
#endif
                    markComb(r);
                }
        if ((size_t)s < seqOf.size())
            for (auto& [id, e] : seqOf[s])
                if (!seqPend[id] && (e == ANY || (e == POS && pos) || (e == NEG && neg))) {
                    seqPend[id] = 1;
                    seqq.push_back(id);
                }
        if (__builtin_expect(sigs[s].fl & SF_LISTEN, 0)) heldInputChanged(s);
    }

    void runObserved();
    void matureFinal();
    // The regions after NBA (4.5), once the active set is empty: observed,
    // the reactive set, postponed, and the end-of-slot $past shift. False
    // when all are empty and time can advance. `rare` is set whenever work is
    // queued for any of them but postponed, so a design without assertions
    // or programs pays one flag test per pass.
    bool slotTail() {
        if (vpiRegion && vpiRegion(VR_RW)) return true;   // cbReadWriteSynch (38.36.2)
        if (!observedQ.empty() || !defObs.empty() || !concPending().empty()) { runObserved(); return true; }   // observed (4.4.2.5)
        if (!reactive.empty() || !reInactive.empty() || !reNbaq.empty()) {
            // Once the reactive set starts, it runs until it is empty, and
            // only then does the active set run again (4.5).
            for (;;) {
                if (!reactive.empty()) {                   // reactive (4.4.2.6)
                    Handle h = reactive.front(); reactive.pop_front();
                    region = R_REACTIVE;
                    if (!h.done()) h.resume();
                    region = R_ACTIVE;
                } else if (!reInactive.empty()) {          // re-inactive (#0 in the reactive set)
                    for (auto h : reInactive) reactive.push_back(h);
                    reInactive.clear();
                } else if (!reNbaq.empty()) {              // re-NBA
                    nbaRun.clear();
                    nbaRun.swap(reNbaq);
                    applyNba();
                } else break;
            }
            return true;
        }
        if (!postq.empty() || !defFin.empty()) {           // postponed ($strobe, $monitor, assert final)
            region = R_POSTPONED;
            matureFinal();
            region = R_ACTIVE;
            drainPost();
            return true;
        }
        if (dumpPend) { startDump(); return true; }        // $dumpvars in this slot: the values it ends with
        if (vpiRegion && vpiRegion(VR_RO)) return true;   // cbReadOnlySynch
        if (!ticked.empty()) { shiftSampled(); return true; }   // end of slot: $past lines shift
        if (!evcds.empty()) { evcdTail(); return false; }   // $dumpports: rare stays set, every slot ends here
        rare = false;
        return false;
    }
    // The end of a slot where sample clocks ticked: shift each $past line
    // (sva.hpp SampleClock). Shadows still hold this slot's sampled values.
    void shiftSampled() {
        auto rd = [&](int s) { return get(s); };
        for (int c : ticked) {
            SampleClock& k = sclocks[c];
            if (k.gate && truth(eval(k.gate, rd)) != 1) continue;
            for (auto& ln : k.lines) {
                for (size_t j = ln.stages.size(); j-- > 1;) sigs[ln.stages[j]].val = sigs[ln.stages[j - 1]].val;
                Val v = eval(ln.src, rd);
                int s0 = ln.stages[0];
                sigs[s0].val = sigs[s0].w <= 64 ? make(v.v, v.x, sigs[s0].w, v.z) : maskWidth(v, sigs[s0].w);
            }
        }
        ticked.clear();
    }

    void header() {
        if (vcdPath.empty()) return;
        if (dumpArrays) {
            // Elements are hidden when they are made; show those of the small
            // arrays. Only the last name component counts: blk[2].x is not one.
            auto base = [](const string& n) {
                size_t dot = n.rfind('.');
                size_t br = n.find('[', dot == string::npos ? 0 : dot + 1);
                return br == string::npos ? string() : n.substr(0, br);
            };
            unordered_map<string, size_t> count;
            for (const Sig& g : sigs)
                if (g.hide && g.name.find('$') == string::npos)
                    if (string b = base(g.name); !b.empty()) count[b]++;
            for (Sig& g : sigs)
                if (g.hide && g.name.find('$') == string::npos)
                    if (string b = base(g.name); !b.empty() && count[b] <= dumpArrays) g.hide = false;
        }
        observed = true;   // every change is written: the cycle domain uses set()
        bool asFst = vcdPath.size() > 4 && vcdPath.compare(vcdPath.size() - 4, 4, ".fst") == 0;
#ifdef MINISIM_FST
        if (asFst) {
            auto* f = fstWriterCreate(vcdPath.c_str(), 1);
            if (!f) { cerr << "cannot write " << vcdPath << "\n"; return; }
            fst = f;
            fstWriterSetPackType(f, FST_WR_PT_LZ4);
#ifdef MINISIM_FST_PARALLEL
            // Compress and write finished blocks on libfst's own thread. Only
            // with a libfst built with FST_WRITER_PARALLEL, which the build
            // says with MINISIM_FST_PARALLEL (libfst exits otherwise).
            if (fstParallel && !getenv("MINISIM_FST_SERIAL")) fstWriterSetParallelMode(f, 1);
#endif
            fstWriterSetTimescaleFromString(f, timescale.c_str());
            fstH.assign(sigs.size(), 0);
        }
#else
        if (asFst) { cerr << "msim: this build has no FST support (built without libfst); writing VCD to " << vcdPath << "\n"; }
#endif
        if (!fst) {
            vcd.open(vcdPath);
            if (!vcd) { cerr << "cannot write " << vcdPath << "\n"; return; }
            vcd << "$timescale " << timescale << " $end\n";
        }
        // Hierarchical scopes: u.v.x is variable x in scope v inside scope u
        // under the top module, so a waveform viewer shows the instance tree.
        // Scopes keep the order their first signal was declared in.
        struct Scope { string name; vector<size_t> vars; vector<Scope> kids; };
        Scope root{vcdTop.empty() ? string("top") : vcdTop, {}, {}};
        for (size_t i = 0; i < sigs.size(); i++) {
            if (!dumped(i)) continue;
            Scope* sc = &root;
            const string& n = sigs[i].name;
            size_t b = 0, e;
            while ((e = n.find('.', b)) != string::npos) {
                string part = n.substr(b, e - b);
                auto it = std::find_if(sc->kids.begin(), sc->kids.end(), [&](const Scope& k) { return k.name == part; });
                if (it == sc->kids.end()) { sc->kids.push_back({part, {}, {}}); sc = &sc->kids.back(); }
                else sc = &*it;
                b = e + 1;
            }
            sc->vars.push_back(i);
        }
        std::function<void(const Scope&)> emit = [&](const Scope& sc) {
#ifdef MINISIM_FST
            if (fst) {
                auto* f = (fstWriterContext*)fst;
                fstWriterSetScope(f, FST_ST_VCD_MODULE, sc.name.c_str(), nullptr);
                for (size_t i : sc.vars) {
                    const string& n = sigs[i].name;
                    size_t d = n.rfind('.');
                    string leaf = d == string::npos ? n : n.substr(d + 1);
                    fstH[i] = fstWriterCreateVar(f, sigs[i].net ? FST_VT_VCD_WIRE : FST_VT_VCD_REG, FST_VD_IMPLICIT,
                                                 (uint32_t)sigs[i].w, leaf.c_str(), 0);
                }
                for (const Scope& k : sc.kids) emit(k);
                fstWriterSetUpscope(f);
                return;
            }
#endif
            vcd << "$scope module " << sc.name << " $end\n";
            for (size_t i : sc.vars) {
                const string& n = sigs[i].name;
                size_t d = n.rfind('.');
                vcd << "$var " << (sigs[i].net ? "wire" : "reg") << " " << sigs[i].w << " " << vcdId(i) << " "
                    << (d == string::npos ? n : n.substr(d + 1)) << " $end\n";
            }
            for (const Scope& k : sc.kids) emit(k);
            vcd << "$upscope $end\n";
        };
        emit(root);
        if (!fst) {
            vcd << "$enddefinitions $end\n";
            vcdIds.resize(sigs.size());
            for (size_t i = 0; i < sigs.size(); i++) vcdIds[i] = vcdId(i);
        }
    }

    // After the last slot ($finish, no more events, or the time limit):
    // final blocks run in the same time, in one pass (9.2.3).
    void runFinal(const vector<Task>& ts) {
        if (ts.empty()) return;
        wheel.clear(); laterNba.clear(); laterDrv.clear(); active.clear(); inactive.clear(); nbaq.clear();
        observedQ.clear(); reactive.clear(); reInactive.clear(); reNbaq.clear(); defObs.clear(); defFin.clear();
        for (int id : seqq) seqPend[id] = 0;
        seqq.clear();
        finished = false;
        for (auto& t : ts) spawn(t);
        run(~0ULL, false);
    }
    void run(uint64_t end, bool start = true) {
        if (start) { dumpCli = !vcdPath.empty(); header(); dumpvars(); }
        if (!combq.empty()) wheel[now];  // a design of only combinational logic still gets its time-0 slot
        while (!wheel.empty() && !finished) {
            auto it = wheel.begin();
            if (it->first > end) break;
            uint64_t t = it->first;
            bool any = laterNba.count(t) > 0 || laterDrv.count(t) > 0 || !combq.empty() || (vpiRegion && vpiAt.count(t));
            for (auto h : it->second)
                if (dead.empty() || !dead.count(h.address())) { active.push_back(h); any = true; }
            wheel.erase(it);
            if (!any) continue;   // only disabled processes were waiting here
            now = t;
            g_simTime = now;
            if (!freeVars.empty() && now && now != freeAt) { freeStep(); freeAt = now; }   // active free checker variables (17.7.2)
            if (vpiRegion) vpiRegion(VR_START);   // cbNextSimTime, cbAtStartOfSimTime, cbAfterDelay
            if (!shadows.empty() && now && now != sampledAt) {   // preponed (4.4.2.1): once per time step
                for (auto [src, sh] : shadows) sigs[sh].val = sigs[src].val;
                sampledAt = now;
            }
            runLaterDrv();   // delayed continuous assignments and net delays due now (active region)
            if (auto ln = laterNba.find(now); ln != laterNba.end()) {
                for (auto& n : ln->second) nbaq.push_back(n);
                laterNba.erase(ln);
            }
            for (;;) {                                         // one time slot
                if (!active.empty()) {                         // active
                    Handle h = active.front(); active.pop_front();
                    if (!h.done()) h.resume();
                } else if (!seqq.empty()) {                    // clocked processes, one parallel batch
                    runSeqs();
                } else if (!combq.empty()) {                   // levelized combinational logic
                    runCombs();
                } else if (!inactive.empty()) {                // inactive (#0)
                    for (auto h : inactive) active.push_back(h);
                    inactive.clear();
                } else if (!nbaq.empty()) {                    // NBA
                    // Swap, not move: both buffers keep their capacity across slots.
                    nbaRun.clear();
                    nbaRun.swap(nbaq);
                    applyNba();
                } else if (rare || !postq.empty()) {           // observed, reactive set, postponed
                    if (!slotTail()) break;                    // then, and only then, time advances
                } else break;
            }
        }
    }
};

inline Sim S;
// msim -xprop (SF_XPROP_EDGE): how signal s changed in this time step, seen
// as a posedge (or negedge). With `level` the process tests the signal's
// level, as for an asynchronous reset: once the level is known it does not
// matter whether the edge happened, so x -> active is a certain edge.
inline int edgeClassNow(int s, bool neg, bool level) {
    if (s < 0 || (size_t)s >= S.sigs.size()) return 0;
    const Sig& g = S.sigs[s];
    if (g.at != S.now) return 0;
    int o = g.p0, n = Sim::bit0(g.val), from = neg ? 1 : 0, to = 1 - from;
    if (level) return n == to && o != to ? 1 : o == from && n == 2 ? 2 : 0;
    if (o == from && n == to) return 1;
    return (o == 2 && n == to) || (o == from && n == 2) ? 2 : 0;
}
inline const bool g_edgeClassSet = (g_edgeClass = edgeClassNow, true);
inline void Sim::closeHook(uint32_t fd) { S.fileClosed(fd); }

// Frames and objects (expr.hpp): blocks of hidden signals made at run time.
static int allocFrameSigs(const FrameLayout& L) {
    int base = -1;
    for (size_t k = 0; k < L.w.size(); k++) {
        int s = S.sig("$f", L.w[k]);
        if (!k) base = s;
    }
    return base;
}
// A frame's slots start at their initial values, with no events: nothing
// can be waiting on a frame that is just being made.
static void initFrameSigs(const FrameLayout& L, int base) {
    for (size_t k = 0; k < L.w.size(); k++) {
        Val v = L.init[k];
        int w = L.w[k];
        if (w <= 64) { uint64_t m = mask(w); v.v &= m; v.x &= m; v.z &= m; v.hi.clear(); }
        v.sgn = false;
        S.sigs[base + (int)k].val = v;
    }
    for (auto& [off, o] : L.dyn) dynStore()[base + off] = o;
}
[[noreturn]] static void nullAccess() {
    cerr << "error: null object access at time " << S.now << runLocStr() << "\n";
    exit(1);
}
static Val getSig(int s) { return S.get(s); }
static bool isA(int obj, int cls);
static const bool frameHooks = (g_allocSigs = allocFrameSigs, g_initSigs = initFrameSigs, g_nullAccess = nullAccess,
                                g_getSig = getSig, g_isA = isA, true);

// ---- awaiters ----
struct Delay {
    uint64_t n;
    bool await_ready() const { return false; }
    void await_suspend(Handle h) const {
        if (n == 0) {
            if (S.region == R_REACTIVE) { S.reInactive.push_back(h); S.rare = true; }
            else S.inactive.push_back(h);
        }
        else S.wheel[S.now + n].push_back(h);
    }
    void await_resume() const {}
};
// A program process (24.3.1) woken in the active region set moves to the
// reactive region, and runs there once the design is quiet.
struct ToReactive {
    bool await_ready() const { return false; }
    void await_suspend(Handle h) const { S.reactive.push_back(h); S.rare = true; }
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
        for (auto& [sg, e] : *ev) {
            auto& ws = S.waiters[sg];
            // A sibling event that keeps firing leaves stale entries on a signal
            // that never changes (always @(posedge clk or negedge rst_n)): drop
            // them whenever the list doubles, so it stays bounded.
            if (ws.size() >= 16 && (ws.size() & (ws.size() - 1)) == 0)
                std::erase_if(ws, [](const Waiter& w) { return w.fired && *w.fired; });
            ws.push_back({h, e, token});
        }
    }
    void await_resume() const {}
};

#include "exec.inc"
#include "rand.inc"
static bool isA(int obj, int cls) { return classIsA((int)S.sigs[obj].val.v, cls); }

// ---- the design + testbench ----
inline int clk, rst, cnt;

// always #5 clk = ~clk;
inline Task clockgen() {
    for (;;) {
        co_await Delay{5};
        S.set(clk, mk(!(S.get(clk).v & 1)));
    }
}
// always @(cnt) $display(...)
inline Task monitor() {
    for (;;) {
        co_await Wait{cnt, ANY};
        Val c = S.get(cnt);
        cout << "t=" << S.now << " cnt=";
        if (c.x & 0xF) cout << "x"; else cout << (c.v & 0xF);
        cout << "\n";
    }
}
// initial begin clk=0; rst=1; #22 rst=0; #100 $finish; end
inline Task stimulus() {
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
