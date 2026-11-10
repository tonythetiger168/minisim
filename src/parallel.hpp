#pragma once
// parallel.hpp - the threads every minisim tool shares: the default thread
// count, the -threads N option, a parallel for, and a reusable pool.
//
//   unsigned threads = par::defaultThreads();          // every logical CPU we may use
//   if (par::threadsArg(argc, argv, i, threads, "mtool")) continue;   // -threads N, -j N
//   par::parallelFor(n, threads, [&](size_t i) { ... });   // one shot
//   par::Pool pool(threads);                               // many batches
//   pool.run(n, [&](size_t i) { ... });
//   pool.chunks(n, grain, [&](size_t begin, size_t end, size_t chunk) { ... });
//   double s = pool.sum(n, grain, [&](size_t begin, size_t end) { return ...; });
//
// Rules the tools keep, so that a run is the same on any thread count:
//   * work is split by index; f(i) writes only slot i of its output;
//   * results are combined in index order afterwards, on one thread.
// An exception thrown by f stops the batch and is rethrown to the caller.
//
// The default count is every logical CPU the process may run on (SMT
// siblings included; taskset and cgroup limits honoured, see cpu.hpp),
// capped at 64. $MINISIM_THREADS overrides it.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cpu.hpp"

namespace par {

inline unsigned defaultThreads() {
    if (const char* e = std::getenv("MINISIM_THREADS"))
        if (int n = std::atoi(e); n > 0) return (unsigned)std::min(n, 1024);
    int n = cpuTopo().logical();
    if (n <= 0) n = (int)std::thread::hardware_concurrency();
    return (unsigned)std::max(1, std::min(n, 64));
}

// The -threads N option (also -j N and -threadsN / -jN is not taken). When
// argv[i] is it, sets `threads`, steps i over the number and returns true.
// A missing or bad number is an error: prints and exits 2, as the tools do.
inline bool threadsArg(int argc, char** argv, int& i, unsigned& threads, const char* tool) {
    std::string a = argv[i];
    if (a != "-threads" && a != "--threads" && a != "-j") return false;
    char* end = nullptr;
    long n = i + 1 < argc ? std::strtol(argv[i + 1], &end, 10) : 0;
    if (i + 1 >= argc || !end || *end || n < 1) {
        std::cerr << tool << ": " << a << " needs a number of threads (1 or more)\n";
        std::exit(2);
    }
    threads = (unsigned)std::min(n, 1024L);
    i++;
    return true;
}

namespace detail {
// Workers pull indices from one counter; the first exception wins.
struct Batch {
    size_t n = 0;
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::mutex errMu;

    template<typename F>
    void work(F& f) {
        for (size_t i; !failed.load(std::memory_order_relaxed) &&
                       (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) {
            try {
                f(i);
            } catch (...) {
                std::lock_guard<std::mutex> g(errMu);
                if (!error) error = std::current_exception();
                failed = true;
            }
        }
    }
    void rethrow() {
        if (error) std::rethrow_exception(error);
    }
};
}  // namespace detail

// Run f(0), ..., f(n-1) on up to `threads` threads (the caller is one of
// them) and return when all are done.
template<typename F>
void parallelFor(size_t n, unsigned threads, F&& f) {
    if (threads <= 1 || n <= 1) {
        for (size_t i = 0; i < n; i++) f(i);
        return;
    }
    detail::Batch b;
    b.n = n;
    std::vector<std::thread> pool;
    unsigned k = (unsigned)std::min<size_t>(threads, n);
    for (unsigned t = 1; t < k; t++) pool.emplace_back([&] { b.work(f); });
    b.work(f);
    for (auto& th : pool) th.join();
    b.rethrow();
}

// The same, with threads that stay: for a tool that runs many batches (a
// solver's iterations, one batch per clock). Workers sleep between batches.
// A batch of one index runs inline, with no lock taken; a small batch wakes
// only the workers it has work for. run() is not reentrant: do not call it
// from inside f.
class Pool {
public:
    // 0 threads means the default: every logical CPU.
    explicit Pool(unsigned threads = 0) {
        if (threads == 0) threads = defaultThreads();
        for (unsigned t = 1; t < threads; t++)
            workers_.emplace_back([this] { loop(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> g(mu_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& w : workers_) w.join();
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    unsigned size() const { return (unsigned)workers_.size() + 1; }
    unsigned threads() const { return size(); }

    // [0, n) in chunks of `grain`: f(begin, end, chunk). The chunks depend
    // on n and grain only, never on the thread count.
    template<typename F>
    void chunks(size_t n, size_t grain, F&& f) {
        if (grain == 0) grain = 1;
        size_t count = (n + grain - 1) / grain;
        run(count, [&](size_t c) { f(c * grain, std::min(n, (c + 1) * grain), c); });
    }

    // A sum over [0, n): f(begin, end) gives one chunk's part, and the parts
    // are added in chunk order, so a floating-point result is the same bit
    // for bit on any thread count.
    template<typename F>
    double sum(size_t n, size_t grain, F&& f) {
        if (grain == 0) grain = 1;
        std::vector<double> part((n + grain - 1) / grain);
        chunks(n, grain, [&](size_t b, size_t e, size_t c) { part[c] = f(b, e); });
        double total = 0;
        for (double p : part) total += p;
        return total;
    }

    template<typename F>
    void run(size_t n, F&& f) {
        if (workers_.empty() || n <= 1) {
            for (size_t i = 0; i < n; i++) f(i);
            return;
        }
        detail::Batch b;
        b.n = n;
        std::function<void()> job = [&] { b.work(f); };   // two references: no allocation
        // Only as many workers as there is work for are woken: a batch of
        // three indices on a 64-thread pool wakes two.
        unsigned k = (unsigned)std::min<size_t>(workers_.size(), n - 1);
        {
            std::lock_guard<std::mutex> g(mu_);
            job_ = &job;
            slots_ = busy_ = k;
        }
        if (k == workers_.size()) wake_.notify_all();
        else for (unsigned i = 0; i < k; i++) wake_.notify_one();
        b.work(f);
        std::unique_lock<std::mutex> g(mu_);
        done_.wait(g, [&] { return busy_ == 0; });
        job_ = nullptr;
        g.unlock();
        b.rethrow();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()>* job;
            {
                std::unique_lock<std::mutex> g(mu_);
                wake_.wait(g, [&] { return stop_ || slots_ > 0; });
                if (stop_) return;
                slots_--;   // this worker takes one place in the batch
                job = job_;
            }
            (*job)();
            std::lock_guard<std::mutex> g(mu_);
            if (--busy_ == 0) done_.notify_one();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::condition_variable wake_, done_;
    std::function<void()>* job_ = nullptr;
    unsigned slots_ = 0;   // places in the current batch not yet taken
    unsigned busy_ = 0;    // workers the batch still waits for
    bool stop_ = false;
};

}  // namespace par
