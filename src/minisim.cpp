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
#include <atomic>
#include <functional>
#include <map>
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
#ifdef MINISIM_FST
#include "fstapi.h"
#endif
struct Sig { string name; int w; Val val; vector<Drv> drv; bool net; bool hide = false; };  // hide: array element, kept out of the VCD  // drv: one contribution per driver; net: wire, else reg
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
inline int defaultThreads() {
    int hw = (int)thread::hardware_concurrency();
    return hw < 1 ? 1 : (hw > 8 ? 8 : hw);
}

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
    };
    Slot slots[kMaxThreads];
    vector<thread> ts;
    atomic<uint64_t> gen{0};       // bumped once per batch; idle workers sleep on it
    alignas(64) atomic<size_t> finished{0};   // items of the current batch that are done
    atomic<size_t> total{0};       // items in the current batch
    Job job = nullptr;             // written before a batch is published, read after a claim
    void* ctx = nullptr;
    atomic<bool> stop{false};
    int size() const { return (int)ts.size() + 1; }
    void ensure(int n) {
        n = std::min(n, kMaxThreads);
        while ((int)ts.size() < n - 1) {
            int id = (int)ts.size() + 1;
            ts.emplace_back([this, id] { worker(id); });
        }
    }
    ~Pool() {
        stop = true;
        gen.fetch_add(1);
        gen.notify_all();
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
            if (finished.fetch_add(hi - lo, memory_order_acq_rel) + (hi - lo) == total.load(memory_order_relaxed))
                finished.notify_one();
            c = s.claim.load(memory_order_acquire);
        }
    }
    void work(int id, uint32_t b) {
        int T = size();
        for (int k = 0; k < T; k++)
            if (!drain(slots[(id + k) % T], b)) return;
    }
    void worker(int id) {
        uint64_t seen = 0;
        for (;;) {
            uint64_t g;
            int spins = 0;
            while ((g = gen.load(memory_order_acquire)) == seen) {
                // Spin for the next batch; yield, which lets any other
                // runnable thread have the CPU; then sleep.
                if (++spins < 4000) relax();
                else if (spins < 20000) this_thread::yield();
                else gen.wait(seen);
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
    void run(size_t n, Job j, void* c) {
        uint32_t b = (uint32_t)(gen.load(memory_order_relaxed) + 1);
        size_t T = (size_t)size();
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
        gen.notify_all();
        work(0, b);
        // Wait for chunks other threads are still running: spin, yield, then sleep.
        for (int spins = 0;; spins++) {
            size_t d = finished.load(memory_order_acquire);
            if (d >= n) break;
            if (spins < 4000) relax();
            else if (spins < 20000) this_thread::yield();
            else finished.wait(d, memory_order_acquire);
        }
    }
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
    string vcdTop = "top";   // name of the root VCD scope (the top module)
    string timescale = "1ns";                        // one tick, for the VCD header
    ofstream vcd;
    // FST output instead of VCD when vcdPath ends in ".fst" (built with libfst).
    void* fst = nullptr;
    vector<uint32_t> fstH;                           // FST handle per signal (0: not dumped)
    string fstBuf;
    bool waving() const { return vcd.is_open() || fst != nullptr; }

    Sim() { g_sformat = &Sim::sformat; }

    // ---- multi-core: deterministic parallel batches ----
    // `threads` workers (default: defaultThreads(); msim -threads N).
    // Only work that reads signals and writes into its own buffer runs in
    // parallel: the value of a combinational process, or the NBAs of a
    // clocked process. Results are committed afterwards on this thread in a
    // fixed order (rank, process index), so the output does not depend on the
    // thread count or on timing. -threads 1 runs the same code serially.
    int threads = 1;
    static constexpr size_t kMinParallel = 32;
    Pool pool;
    template <class F>
    void parallelFor(size_t n, F&& f) {
        // A small batch is cheaper on this thread than a wake-up of the pool.
        if (threads <= 1 || n < kMinParallel) { for (size_t i = 0; i < n; i++) f(i); return; }
        pool.ensure(threads);
        pool.run(n, [](void* c, size_t i) { (*static_cast<F*>(c))(i); }, &f);
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
    priority_queue<int, vector<int>, greater<int>> combq;
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
            parallelFor(combBatch.size(), [&](size_t k) {
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
    void setLocal(int s, Val nv) {
        Sig& g = sigs[s];
        if (nv == g.val) return;
        g.val = nv;
        Part& pt = *tlPart;
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
        if (parts.empty() || waving() || mon.on) {
            // Partial writes merge into the value as it is *now*, so two NBAs to
            // different bits of one reg in the same slot both take effect.
            for (auto& n : nbaRun) set(n.s, n.hi >= 0 ? v_insert(get(n.s), n.v, n.hi, n.lo) : n.v);
            return;
        }
        nbaSerial.clear();
        partsBusy.clear();
        for (auto& n : nbaRun) {
            int p = sigPart[n.s];
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
        parallelFor(partsBusy.size(), [&](size_t k) { runPart(parts[partsBusy[k]]); });
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
        parallelFor(seqq.size(), [&](size_t k) {
            seqBufs[k].clear();
            seqs[seqq[k]].fn(seqs[seqq[k]].ctx, seqBufs[k]);
        });
        for (size_t k = 0; k < seqq.size(); k++) {
            seqPend[seqq[k]] = 0;
            nbaq.insert(nbaq.end(), seqBufs[k].begin(), seqBufs[k].end());
        }
        seqq.clear();
    }

    ~Sim() {
        for (auto h : all) h.destroy();
        closeWaves();
    }
    void closeWaves() {
#ifdef MINISIM_FST
        if (fst) { fstWriterClose((fstWriterContext*)fst); fst = nullptr; }
#endif
        if (vcd.is_open()) vcd.close();
    }

    // A wire (`net`) starts as Z, including before the first drive().
    // A variable (reg/logic) starts as X. The flag is stored so the VCD
    // header can emit `$var wire` or `$var reg`.
    int sig(const string& n, int w, bool net = false) {
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
        // One driver that drives both 0 and 1 resolves to its own value
        // (X stays X, Z stays Z). Skip the per-bit strength walk.
        if (g.drv.size() == 1 && s0 != STR_HIZ && s1 != STR_HIZ) { set(s, g.drv[0].v); return; }
        set(s, resolveDrives(g.drv.data(), g.drv.size(), g.w));
    }
    Val get(int s) const {
        return sigs[s].w <= 64 ? make(sigs[s].val.v, sigs[s].val.x, sigs[s].w, sigs[s].val.z)
                               : clipRead(sigs[s].val, sigs[s].w);
    }
    void nba(int s, Val v, int hi = -1, int lo = 0) { nbaq.push_back({s, v, hi, lo}); }
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
            char lc = (char)tolower((unsigned char)c);
            if (lc == 'm') { cout << "top"; continue; }
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
#ifdef MINISIM_FST
        if (fst) {
            // FST takes one '0' / '1' / 'x' / 'z' character per bit, MSB first.
            fstBuf.resize((size_t)g.w);
            for (int i = g.w - 1, k = 0; i >= 0; i--, k++) {
                if (g.w <= 64)
                    fstBuf[k] = (g.val.x >> i & 1) ? ((g.val.z >> i & 1) ? 'z' : 'x') : char('0' + (g.val.v >> i & 1));
                else {
                    int t = bitAt(g.val, i);
                    fstBuf[k] = t == 3 ? 'z' : t == 2 ? 'x' : char('0' + t);
                }
            }
            fstWriterEmitValueChange((fstWriterContext*)fst, fstH[s], fstBuf.data());
            return;
        }
#endif
        string id = vcdId((size_t)s);
        if (g.w == 1) { vcd << ((g.val.x & 1) ? ((g.val.z & 1) ? 'z' : 'x') : char('0' + (g.val.v & 1))) << id << "\n"; return; }
        vcd << "b";
        for (int i = g.w - 1; i >= 0; i--) {
            if (g.w <= 64)
                vcd << ((g.val.x >> i & 1) ? ((g.val.z >> i & 1) ? 'z' : 'x') : char('0' + (g.val.v >> i & 1)));
            else {
                int t = bitAt(g.val, i);
                vcd << (t == 3 ? 'z' : t == 2 ? 'x' : char('0' + t));
            }
        }
        vcd << " " << id << "\n";
    }
    // VCD identifier: base-94 over the printable characters '!'..'~'.
    static string vcdId(size_t i) {
        string id;
        do { id += char('!' + i % 94); i /= 94; } while (i);
        return id;
    }
    void dump(int s) {
        if (sigs[s].hide) return;
#ifdef MINISIM_FST
        if (fst) {
            if (lastDump != now) { fstWriterEmitTimeChange((fstWriterContext*)fst, now); lastDump = now; }
            writeVal(s);
            return;
        }
#endif
        if (!vcd.is_open() || !vcd) return;
        if (lastDump != now) { vcd << "#" << now << "\n"; lastDump = now; }
        writeVal(s);
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
            for (size_t i = 0; i < sigs.size(); i++) if (!sigs[i].hide) writeVal((int)i);
            lastDump = now;
            return;
        }
#endif
        if (!vcd.is_open() || !vcd) return;
        vcd << "$dumpvars\n";
        for (size_t i = 0; i < sigs.size(); i++) if (!sigs[i].hide) writeVal((int)i);
        vcd << "$end\n";
    }

    void set(int s, Val nv) {
        if (sigs[s].w <= 64) {
            uint64_t m = sigs[s].w >= 64 ? ~0ULL : ((1ULL << sigs[s].w) - 1);
            nv.v &= m; nv.x &= m; nv.z &= m; nv.sgn = false;
            nv.hi.clear();
        } else nv = maskWidth(nv, sigs[s].w);
        if (tlPart) { setLocal(s, nv); return; }
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
        if ((size_t)s < combOf.size())
            for (int r : combOf[s])
                if (r != curComb) markComb(r);
        if ((size_t)s < seqOf.size())
            for (auto& [id, e] : seqOf[s])
                if (!seqPend[id] && (e == ANY || (e == POS && pos) || (e == NEG && neg))) {
                    seqPend[id] = 1;
                    seqq.push_back(id);
                }
    }

    void header() {
        if (vcdPath.empty()) return;
        bool asFst = vcdPath.size() > 4 && vcdPath.compare(vcdPath.size() - 4, 4, ".fst") == 0;
#ifdef MINISIM_FST
        if (asFst) {
            auto* f = fstWriterCreate(vcdPath.c_str(), 1);
            if (!f) { cerr << "cannot write " << vcdPath << "\n"; return; }
            fst = f;
            fstWriterSetPackType(f, FST_WR_PT_LZ4);
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
            if (sigs[i].hide) continue;
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
        if (!fst) vcd << "$enddefinitions $end\n";
    }

    // After the last slot ($finish, no more events, or the time limit):
    // final blocks run in the same time, in one pass (9.2.3).
    void runFinal(const vector<Task>& ts) {
        if (ts.empty()) return;
        wheel.clear(); laterNba.clear(); active.clear(); inactive.clear(); nbaq.clear();
        for (int id : seqq) seqPend[id] = 0;
        seqq.clear();
        finished = false;
        for (auto& t : ts) spawn(t);
        run(~0ULL, false);
    }
    void run(uint64_t end, bool start = true) {
        if (start) { header(); dumpvars(); }
        if (!combq.empty()) wheel[now];  // a design of only combinational logic still gets its time-0 slot
        while (!wheel.empty() && !finished) {
            auto it = wheel.begin();
            if (it->first > end) break;
            uint64_t t = it->first;
            bool any = laterNba.count(t) > 0 || !combq.empty();
            for (auto h : it->second)
                if (dead.empty() || !dead.count(h.address())) { active.push_back(h); any = true; }
            wheel.erase(it);
            if (!any) continue;   // only disabled processes were waiting here
            now = t;
            g_simTime = now;
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
