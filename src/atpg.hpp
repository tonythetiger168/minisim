// atpg.hpp - test generation on the combinational model of faultsim.hpp.
//
//   Podem     the PODEM test generator (Goel 1981) with SCOAP-guided
//             backtrace, an X-path check and a backtrack limit. It works on
//             the good and the faulty machine side by side, three-valued.
//   Engine    the flow: random patterns while they pay, then PODEM on what
//             is left, each new pattern fault-simulated against every
//             remaining fault, then reverse-order compaction. Also grade()
//             for given patterns and respond() for the expected values.
//
// Threads: fault simulation is split over the faults and PODEM runs on a
// fixed-size batch of target faults at a time, so the result is the same for
// any thread count.
#pragma once
#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "faultsim.hpp"

namespace atpg {

// A small fork-join pool: run(fn) calls fn(thread) on every thread.
class Pool {
public:
    explicit Pool(int n) : n_(std::max(1, n)) {
        for (int i = 1; i < n_; i++) th_.emplace_back([this, i] { loop(i); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
            gen_++;
        }
        cv_.notify_all();
        for (auto& t : th_) t.join();
    }
    int threads() const { return n_; }
    void run(const std::function<void(int)>& fn) {
        if (n_ == 1) { fn(0); return; }
        {
            std::lock_guard<std::mutex> l(m_);
            job_ = &fn;
            pending_ = n_ - 1;
            gen_++;
        }
        cv_.notify_all();
        fn(0);
        std::unique_lock<std::mutex> l(m_);
        done_.wait(l, [this] { return pending_ == 0; });
    }
    // fn(thread, index) for index in [0, count), in chunks.
    void each(int count, int chunk, const std::function<void(int, int)>& fn) {
        if (n_ == 1 || count <= chunk) {
            for (int i = 0; i < count; i++) fn(0, i);
            return;
        }
        std::atomic<int> next{0};
        run([&](int t) {
            for (;;) {
                int b = next.fetch_add(chunk);
                if (b >= count) return;
                int e = std::min(count, b + chunk);
                for (int i = b; i < e; i++) fn(t, i);
            }
        });
    }

private:
    void loop(int id) {
        int seen = 0;
        for (;;) {
            const std::function<void(int)>* job;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return gen_ != seen; });
                seen = gen_;
                if (stop_) return;
                job = job_;
            }
            (*job)(id);
            {
                std::lock_guard<std::mutex> l(m_);
                pending_--;
            }
            done_.notify_one();
        }
    }
    int n_;
    std::vector<std::thread> th_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int)>* job_ = nullptr;
    int gen_ = 0, pending_ = 0;
    bool stop_ = false;
};

// ----------------------------------------------------------------- PODEM ----
struct Podem {
    enum Result { TEST, UNTESTABLE, ABORT };
    const Circuit& c;
    bool free;   // lift the tester's holds and make X sources controllable
    std::vector<uint8_t> g, f;
    struct Undo { int n; uint8_t g, f; };
    std::vector<Undo> trail;
    std::vector<int> heap;
    std::vector<uint32_t> qs, seen, dead, xpS;
    std::vector<uint8_t> xpV;
    uint32_t qep = 0, sep = 0, dep = 0;
    int fnode = -1, ffi = -1, fsa = 0;
    std::vector<int> frontier;
    bool det = false;
    long backtracks = 0;

    Podem(const Circuit& cc, bool fr) : c(cc), free(fr) {
        int N = c.size();
        g.assign(N, 2);
        qs.assign(N, 0); seen.assign(N, 0); dead.assign(N, 0); xpS.assign(N, 0); xpV.assign(N, 0);
        for (int n = 0; n < N; n++) {
            switch (c.type[n]) {
                case IN: g[n] = (!free && c.fixed[n] >= 0) ? c.fixed[n] : 2; break;
                case XS: g[n] = 2; break;
                case C0: g[n] = 0; break;
                case C1: g[n] = 1; break;
                default: g[n] = ev(n, g, -1, 0);
            }
        }
        f = g;
    }

    bool isInput(int n) const {
        return (c.type[n] == IN && (free || c.fixed[n] < 0)) || (free && c.type[n] == XS);
    }
    uint8_t ev(int n, const std::vector<uint8_t>& v, int force, uint8_t fv) const {
        const int* a = c.in(n);
        int k = c.fiN[n];
        auto in = [&](int i) -> uint8_t { return i == force ? fv : v[a[i]]; };
        switch (c.type[n]) {
            case BUF: return in(0);
            case NOT: { uint8_t x = in(0); return x == 2 ? 2 : x ^ 1; }
            case AND:
            case NAND: {
                uint8_t r = 1;
                for (int i = 0; i < k; i++) {
                    uint8_t x = in(i);
                    if (x == 0) { r = 0; break; }
                    if (x == 2) r = 2;
                }
                return r == 2 ? 2 : (c.type[n] == AND ? r : r ^ 1);
            }
            case OR:
            case NOR: {
                uint8_t r = 0;
                for (int i = 0; i < k; i++) {
                    uint8_t x = in(i);
                    if (x == 1) { r = 1; break; }
                    if (x == 2) r = 2;
                }
                return r == 2 ? 2 : (c.type[n] == OR ? r : r ^ 1);
            }
            case XOR:
            case XNOR: {
                uint8_t r = c.type[n] == XNOR;
                for (int i = 0; i < k; i++) {
                    uint8_t x = in(i);
                    if (x == 2) return 2;
                    r ^= x;
                }
                return r;
            }
            case MUX: {
                uint8_t x = in(0), y = in(1), s = in(2);
                if (s == 0) return x;
                if (s == 1) return y;
                return (x == y) ? x : 2;
            }
            default: return v[n];
        }
    }
    void push(int n) {
        for (int k = c.foStart[n]; k < c.foStart[n + 1]; k++) {
            int m = c.fo[k];
            if (qs[m] != qep) {
                qs[m] = qep;
                heap.push_back(m);
                std::push_heap(heap.begin(), heap.end(), std::greater<int>());
            }
        }
    }
    void set(int n, uint8_t gv, uint8_t fv) {
        if (g[n] == gv && f[n] == fv) return;
        trail.push_back({n, g[n], f[n]});
        g[n] = gv;
        f[n] = fv;
        push(n);
    }
    void propagate() {
        while (!heap.empty()) {
            std::pop_heap(heap.begin(), heap.end(), std::greater<int>());
            int n = heap.back();
            heap.pop_back();
            qs[n] = 0;
            recompute(n);
        }
    }
    void recompute(int n) {
        uint8_t gv = ev(n, g, -1, 0), fv;
        if (n == fnode) fv = ffi < 0 ? (uint8_t)fsa : ev(n, f, ffi, (uint8_t)fsa);
        else fv = ev(n, f, -1, 0);
        set(n, gv, fv);
    }
    void begin() {
        if (++qep == 0) { std::fill(qs.begin(), qs.end(), 0); qep = 1; }
    }
    void assign(int pi, uint8_t v) {
        begin();
        set(pi, v, (pi == fnode && ffi < 0) ? (uint8_t)fsa : v);
        propagate();
    }
    void undo(size_t mark) {
        while (trail.size() > mark) {
            Undo u = trail.back();
            trail.pop_back();
            g[u.n] = u.g;
            f[u.n] = u.f;
        }
    }

    bool hasD(int n) const { return g[n] != 2 && f[n] != 2 && g[n] != f[n]; }
    void walk(int n) {
        if (seen[n] == sep) return;
        seen[n] = sep;
        if (g[n] != 2 && f[n] != 2) {
            if (g[n] == f[n]) return;
            if (c.obs[n]) { det = true; return; }
            for (int k = c.foStart[n]; k < c.foStart[n + 1] && !det; k++) walk(c.fo[k]);
            return;
        }
        frontier.push_back(n);
    }
    bool xpath(int n) {
        if (xpS[n] == sep) return xpV[n];
        xpS[n] = sep;
        xpV[n] = 0;
        if (c.dist[n] == INT_MAX) return false;
        if (c.obs[n]) { xpV[n] = 1; return true; }
        for (int k = c.foStart[n]; k < c.foStart[n + 1]; k++) {
            int m = c.fo[k];
            if ((g[m] == 2 || f[m] == 2) && xpath(m)) { xpV[n] = 1; return true; }
        }
        return false;
    }
    // Trace an objective back to an input that is still free.
    bool backtrace(int n, uint8_t v, int& pi, uint8_t& pv) {
        if (dead[n] == dep) return false;
        if (g[n] != 2) return false;
        if (isInput(n)) { pi = n; pv = v; return true; }
        const int* a = c.in(n);
        int k = c.fiN[n];
        bool ok = false;
        switch (c.type[n]) {
            case BUF: ok = backtrace(a[0], v, pi, pv); break;
            case NOT: ok = backtrace(a[0], v ^ 1, pi, pv); break;
            case AND: case NAND: case OR: case NOR: {
                bool inv = c.type[n] == NAND || c.type[n] == NOR;
                bool isAnd = c.type[n] == AND || c.type[n] == NAND;
                uint8_t need = v ^ (uint8_t)inv;
                // need == the all-inputs value: every input must take it, so
                // start with the hardest; otherwise one input does, the easiest.
                bool all = isAnd ? need == 1 : need == 0;
                const std::vector<int>& cc = need ? c.cc1 : c.cc0;
                std::vector<int> ord;
                for (int i = 0; i < k; i++)
                    if (g[a[i]] == 2) ord.push_back(a[i]);
                std::sort(ord.begin(), ord.end(), [&](int x, int y) {
                    return all ? cc[x] > cc[y] : cc[x] < cc[y];
                });
                for (int m : ord)
                    if (backtrace(m, need, pi, pv)) { ok = true; break; }
                break;
            }
            case XOR: case XNOR: {
                uint8_t need = v ^ (uint8_t)(c.type[n] == XNOR);
                for (int i = 0; i < k; i++)
                    if (g[a[i]] == 1) need ^= 1;
                for (int i = 0; i < k && !ok; i++)
                    if (g[a[i]] == 2) ok = backtrace(a[i], need, pi, pv);
                break;
            }
            case MUX: {
                uint8_t s = g[a[2]];
                if (s == 0) ok = backtrace(a[0], v, pi, pv);
                else if (s == 1) ok = backtrace(a[1], v, pi, pv);
                else {
                    if (g[a[0]] == v) ok = backtrace(a[2], 0, pi, pv);
                    if (!ok && g[a[1]] == v) ok = backtrace(a[2], 1, pi, pv);
                    if (!ok && g[a[0]] == 2) ok = backtrace(a[0], v, pi, pv);
                    if (!ok && g[a[1]] == 2) ok = backtrace(a[1], v, pi, pv);
                    if (!ok) ok = backtrace(a[2], 0, pi, pv);
                }
                break;
            }
            default: break;
        }
        if (!ok) dead[n] = dep;
        return ok;
    }
    // An objective that moves the fault effect through frontier gate n.
    bool advance(int n, int depth, int& pi, uint8_t& pv) {
        const int* a = c.in(n);
        int k = c.fiN[n];
        auto d = [&](int i) { return (n == fnode && i == ffi) || hasD(a[i]); };
        if (c.type[n] == MUX) {
            if (g[a[2]] == 2) {
                if (d(0) && backtrace(a[2], 0, pi, pv)) return true;
                if (d(1) && backtrace(a[2], 1, pi, pv)) return true;
            }
            if (d(2)) {
                if (g[a[0]] == 2 && backtrace(a[0], g[a[1]] == 2 ? 0 : g[a[1]] ^ 1, pi, pv)) return true;
                if (g[a[1]] == 2 && backtrace(a[1], g[a[0]] == 2 ? 1 : g[a[0]] ^ 1, pi, pv)) return true;
            }
        }
        uint8_t nc = (c.type[n] == AND || c.type[n] == NAND) ? 1 : 0;
        for (int i = 0; i < k; i++)
            if (g[a[i]] == 2 && backtrace(a[i], nc, pi, pv)) return true;
        for (int i = 0; i < k; i++)
            if (g[a[i]] == 2 && backtrace(a[i], nc ^ 1, pi, pv)) return true;
        // Known in the good machine but unknown in the faulty one: the
        // unknown comes from further back.
        if (depth < 64)
            for (int i = 0; i < k; i++)
                if (g[a[i]] != 2 && f[a[i]] == 2 && advance(a[i], depth + 1, pi, pv)) return true;
        return false;
    }

    // 0: a test, 1: an objective in (pi, pv), 2: a conflict.
    int step(const Fault& ft, int& pi, uint8_t& pv) {
        if (++dep == 0) { std::fill(dead.begin(), dead.end(), 0); dep = 1; }
        if (ft.cond >= 0) {
            uint8_t cv = g[ft.cond];
            if (cv == 2) return backtrace(ft.cond, ft.condv, pi, pv) ? 1 : 2;
            if (cv != ft.condv) return 2;
        }
        int line = ffi < 0 ? fnode : c.in(fnode)[ffi];
        uint8_t lv = g[line];
        if (lv == (uint8_t)fsa) return 2;
        if (lv == 2) return backtrace(line, fsa ^ 1, pi, pv) ? 1 : 2;
        if (++sep == 0) {
            std::fill(seen.begin(), seen.end(), 0);
            std::fill(xpS.begin(), xpS.end(), 0);
            sep = 1;
        }
        frontier.clear();
        det = false;
        walk(fnode);
        if (det) return 0;
        std::sort(frontier.begin(), frontier.end(),
                  [&](int x, int y) { return c.dist[x] != c.dist[y] ? c.dist[x] < c.dist[y] : x < y; });
        for (int n : frontier) {
            if (!xpath(n)) continue;
            if (advance(n, 0, pi, pv)) return 1;
        }
        return 2;
    }

    Result run(const Fault& ft, long limit, std::vector<uint8_t>& cube) {
        fnode = ft.node; ffi = ft.fi; fsa = ft.sa;
        backtracks = 0;
        struct Dec { int pi; uint8_t v; bool flipped; size_t mark; };
        std::vector<Dec> stack;
        begin();
        if (ffi < 0) set(fnode, g[fnode], (uint8_t)fsa);
        else { qs[fnode] = qep; heap.push_back(fnode); }
        propagate();
        Result res;
        for (;;) {
            int pi = -1;
            uint8_t pv = 0;
            int st = step(ft, pi, pv);
            if (st == 0) {
                cube.resize(c.inputs.size());
                for (size_t i = 0; i < c.inputs.size(); i++) cube[i] = g[c.inputs[i]];
                res = TEST;
                break;
            }
            if (st == 1) {
                stack.push_back({pi, pv, false, trail.size()});
                assign(pi, pv);
                continue;
            }
            while (!stack.empty() && stack.back().flipped) {
                undo(stack.back().mark);
                stack.pop_back();
            }
            if (stack.empty()) { res = UNTESTABLE; break; }
            if (++backtracks > limit) { res = ABORT; break; }
            Dec& d = stack.back();
            undo(d.mark);
            d.flipped = true;
            d.v ^= 1;
            assign(d.pi, d.v);
        }
        undo(0);
        fnode = -1;
        return res;
    }
};

// ------------------------------------------------------------------ flow ----
struct Options {
    long abortLimit = 10;
    int randomMax = 1024;
    uint64_t seed = 1;
    long retryBudget = 100000;    // backtracks the second PODEM pass may spend
    bool compact = true;
    bool proveRedundant = true;   // rerun an untestable fault with the holds lifted
};

struct Stats {
    int randomPatterns = 0, podemPatterns = 0, beforeCompaction = 0;
    long podemCalls = 0, aborted = 0, mismatches = 0;
};

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
};

using Pattern = std::vector<uint8_t>;   // one 0/1 per circuit input

struct Engine {
    const Circuit& c;
    std::vector<Fault>& faults;
    Options opt;
    Pool& pool;
    Stats st;
    Sim sim;
    std::vector<Sim::Work> work;
    std::vector<int> active;
    std::vector<uint64_t> masks;
    Rng rng;

    Engine(const Circuit& cc, std::vector<Fault>& fl, const Options& o, Pool& p)
        : c(cc), faults(fl), opt(o), pool(p), sim(cc), work(p.threads()), rng{o.seed} {
        for (auto& w : work) w.init(c.size());
    }

    void simulate(const std::vector<uint64_t>& words) {
        sim.good(words);
        masks.assign(active.size(), 0);
        pool.each((int)active.size(), 256, [&](int t, int i) {
            masks[i] = sim.detect(work[t], faults[active[i]]);
        });
    }
    void prune() {
        size_t k = 0;
        for (int i : active)
            if (faults[i].cls == NC || faults[i].cls == ND) active[k++] = i;
        active.resize(k);
    }
    Pattern column(const std::vector<uint64_t>& words, int bit) const {
        Pattern p(words.size());
        for (size_t i = 0; i < words.size(); i++) {
            int n = c.inputs[i];
            p[i] = c.fixed[n] >= 0 ? (uint8_t)c.fixed[n] : (uint8_t)((words[i] >> bit) & 1);
        }
        return p;
    }

    std::vector<Pattern> generate() {
        std::vector<Pattern> pats;
        size_t ni = c.inputs.size();
        for (size_t i = 0; i < faults.size(); i++) {
            Fault& f = faults[i];
            if (f.node < 0 || f.rep != (int)i || f.cls != NC) continue;
            if (c.dist[f.node] == INT_MAX) { f.cls = UU; continue; }
            active.push_back((int)i);
        }
        std::vector<uint64_t> words(ni);

        // Random patterns, while they still pay.
        for (int done = 0; done < opt.randomMax && !active.empty(); done += 64) {
            for (auto& w : words) w = rng.next();
            simulate(words);
            uint64_t keep = 0;
            int hit = 0;
            for (size_t i = 0; i < active.size(); i++) {
                if (!masks[i]) continue;
                keep |= masks[i] & -masks[i];
                faults[active[i]].cls = DS;
                hit++;
            }
            for (int b = 0; b < 64; b++)
                if (keep >> b & 1) { pats.push_back(column(words, b)); st.randomPatterns++; }
            int before = (int)active.size();
            prune();
            if (hit * 200 < before) break;
        }

        // PODEM on a fixed-size batch of targets at a time: the batch does
        // not depend on the thread count, so neither does the result.
        const int kBatch = 32;
        std::vector<Podem> eng;
        eng.reserve(pool.threads());
        for (int t = 0; t < pool.threads(); t++) eng.emplace_back(c, false);
        std::vector<Podem> freeEng;
        struct Job { int fault; Podem::Result r; Pattern cube; long backtracks; };
        // Two passes: every fault at the abort limit, then the aborted ones
        // again at ten times the limit while a budget of backtracks lasts.
        for (int pass = 0; pass < 2; pass++) {
        long limit = pass ? opt.abortLimit * 10 : opt.abortLimit;
        long budget = opt.retryBudget;
        uint8_t want = pass ? ND : NC;
        size_t cursor = 0;
        std::vector<int> order = active;
        while (cursor < order.size() && (pass == 0 || budget > 0)) {
            std::vector<Job> jobs;
            while (cursor < order.size() && (int)jobs.size() < kBatch) {
                if (faults[order[cursor]].cls == want) jobs.push_back({order[cursor], Podem::ABORT, {}, 0});
                cursor++;
            }
            pool.each((int)jobs.size(), 1, [&](int t, int i) {
                jobs[i].r = eng[t].run(faults[jobs[i].fault], limit, jobs[i].cube);
                jobs[i].backtracks = eng[t].backtracks;
            });
            st.podemCalls += (long)jobs.size();
            for (Job& j : jobs) {
                budget -= j.backtracks;
                if (pass && faults[j.fault].cls == ND) { faults[j.fault].cls = NC; st.aborted--; }
            }
            for (size_t jk = 0; jk < jobs.size(); jk++) {
                Job& j = jobs[jk];
                Fault& f = faults[j.fault];
                if (f.cls != NC) continue;   // an earlier pattern of this batch got it
                if (j.r == Podem::ABORT) { f.cls = ND; st.aborted++; continue; }
                if (j.r == Podem::UNTESTABLE) {
                    f.cls = AU;
                    if (opt.proveRedundant) {
                        if (!c.hasX) {
                            f.cls = UR;
                        } else {
                            if (freeEng.empty()) freeEng.emplace_back(c, true);
                            Pattern tmp;
                            if (freeEng[0].run(f, limit, tmp) == Podem::UNTESTABLE) f.cls = UR;
                        }
                    }
                    continue;
                }
                // Fold in the later cubes of the batch that do not contradict
                // this one: one pattern then tests several targets.
                for (size_t k = jk + 1; k < jobs.size(); k++) {
                    Job& o = jobs[k];
                    if (o.r != Podem::TEST || faults[o.fault].cls != NC) continue;
                    bool ok = true;
                    for (size_t i = 0; i < ni && ok; i++)
                        ok = j.cube[i] == 2 || o.cube[i] == 2 || j.cube[i] == o.cube[i];
                    if (!ok) continue;
                    for (size_t i = 0; i < ni; i++)
                        if (o.cube[i] != 2) j.cube[i] = o.cube[i];
                }
                // 64 random fills of the cube; keep the one that detects most.
                for (size_t i = 0; i < ni; i++)
                    words[i] = j.cube[i] == 2 ? rng.next() : j.cube[i] ? ~0ull : 0;
                prune();
                simulate(words);
                int cnt[64] = {0};
                uint64_t mine = 0;
                for (size_t i = 0; i < active.size(); i++) {
                    uint64_t m = masks[i];
                    if (active[i] == j.fault) mine = m;
                    while (m) { cnt[__builtin_ctzll(m)]++; m &= m - 1; }
                }
                if (!mine) {
                    // The generator and the simulator disagree: never trust
                    // the pattern, and say so.
                    f.cls = ND;
                    st.mismatches++;
                    continue;
                }
                int best = __builtin_ctzll(mine);
                for (int b = 0; b < 64; b++)
                    if ((mine >> b & 1) && cnt[b] > cnt[best]) best = b;
                for (size_t i = 0; i < active.size(); i++)
                    if (masks[i] >> best & 1) {
                        if (faults[active[i]].cls == ND) st.aborted--;
                        faults[active[i]].cls = DS;
                    }
                pats.push_back(column(words, best));
                st.podemPatterns++;
            }
        }
        prune();
        }
        st.beforeCompaction = (int)pats.size();
        if (opt.compact && pats.size() > 1) pats = compact(pats);
        return pats;
    }

    // Reverse-order fault simulation: a pattern stays only if it is the
    // first, from the end, to detect some fault.
    std::vector<Pattern> compact(const std::vector<Pattern>& pats) {
        active.clear();
        for (size_t i = 0; i < faults.size(); i++)
            if (faults[i].node >= 0 && faults[i].rep == (int)i && faults[i].cls == DS)
                active.push_back((int)i);
        std::vector<uint8_t> found(faults.size(), 0);
        std::vector<Pattern> out;
        std::vector<uint64_t> words(c.inputs.size());
        for (int end = (int)pats.size(); end > 0 && !active.empty(); end -= 64) {
            int n = std::min(64, end);
            std::fill(words.begin(), words.end(), 0);
            for (int b = 0; b < n; b++) {
                const Pattern& p = pats[end - 1 - b];
                for (size_t i = 0; i < words.size(); i++) words[i] |= (uint64_t)p[i] << b;
            }
            simulate(words);
            uint64_t keep = 0, valid = n == 64 ? ~0ull : ((1ull << n) - 1);
            for (size_t i = 0; i < active.size(); i++) {
                uint64_t m = masks[i] & valid;
                if (!m) continue;
                keep |= m & -m;
                found[active[i]] = 1;
            }
            for (int b = 0; b < n; b++)
                if (keep >> b & 1) out.push_back(pats[end - 1 - b]);
            size_t k = 0;
            for (int i : active)
                if (!found[i]) active[k++] = i;
            active.resize(k);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    // Grade given patterns: mark what they detect.
    void grade(const std::vector<Pattern>& pats) {
        active.clear();
        for (size_t i = 0; i < faults.size(); i++) {
            Fault& f = faults[i];
            if (f.node < 0 || f.rep != (int)i || f.cls != NC) continue;
            if (c.dist[f.node] == INT_MAX) { f.cls = UU; continue; }
            active.push_back((int)i);
        }
        std::vector<uint64_t> words(c.inputs.size());
        for (size_t b0 = 0; b0 < pats.size() && !active.empty(); b0 += 64) {
            size_t n = std::min<size_t>(64, pats.size() - b0);
            std::fill(words.begin(), words.end(), 0);
            for (size_t b = 0; b < n; b++)
                for (size_t i = 0; i < words.size(); i++) words[i] |= (uint64_t)pats[b0 + b][i] << b;
            simulate(words);
            uint64_t valid = n == 64 ? ~0ull : ((1ull << n) - 1);
            for (size_t i = 0; i < active.size(); i++)
                if (masks[i] & valid) faults[active[i]].cls = DS;
            prune();
        }
    }

    // The good-machine response of each pattern at the given nodes: 0, 1 or 2 (X).
    std::vector<std::vector<uint8_t>> respond(const std::vector<Pattern>& pats,
                                              const std::vector<int>& nodes) {
        std::vector<std::vector<uint8_t>> out(pats.size(), std::vector<uint8_t>(nodes.size(), 2));
        std::vector<uint64_t> words(c.inputs.size());
        for (size_t b0 = 0; b0 < pats.size(); b0 += 64) {
            size_t n = std::min<size_t>(64, pats.size() - b0);
            std::fill(words.begin(), words.end(), 0);
            for (size_t b = 0; b < n; b++)
                for (size_t i = 0; i < words.size(); i++) words[i] |= (uint64_t)pats[b0 + b][i] << b;
            sim.good(words);
            for (size_t b = 0; b < n; b++)
                for (size_t k = 0; k < nodes.size(); k++) {
                    int nd = nodes[k];
                    out[b0 + b][k] = (sim.g1[nd] >> b & 1) ? 1 : (sim.g0[nd] >> b & 1) ? 0 : 2;
                }
        }
        return out;
    }
};

}  // namespace atpg
