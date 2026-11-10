// parfor.hpp - a small thread pool for data-parallel loops, for the tools
// that work on a whole netlist at once (msta).
//
//   par::Pool pool(threads);            // 0: one per logical core
//   pool.run(n, grain, [&](size_t begin, size_t end) { ... });
//
// run() splits [0, n) into chunks of `grain`, hands them to the workers and
// the calling thread, and returns when every chunk is done. Chunks are
// claimed with an atomic counter, so which thread runs which chunk varies,
// but a loop body that writes only to its own indexes gives the same result
// with any number of threads. Idle workers sleep on a condition variable.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace par {

class Pool {
public:
    explicit Pool(int threads = 0) {
        if (threads <= 0) threads = (int)std::thread::hardware_concurrency();
        if (threads <= 0) threads = 1;
        n_ = threads;
        for (int i = 1; i < n_; i++) workers_.emplace_back([this] { work(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    int threads() const { return n_; }

    template <class F>
    void run(size_t n, size_t grain, F&& body) {
        if (n == 0) return;
        if (grain == 0) grain = 1;
        if (n_ == 1 || n <= grain) {
            body((size_t)0, n);
            return;
        }
        std::function<void(size_t, size_t)> fn = std::ref(body);
        {
            std::lock_guard<std::mutex> lk(m_);
            fn_ = &fn;
            total_ = n;
            grain_ = grain;
            next_.store(0, std::memory_order_relaxed);
            active_ = (int)workers_.size();
            gen_++;
        }
        cv_.notify_all();
        chunks();
        std::unique_lock<std::mutex> lk(m_);
        done_.wait(lk, [this] { return active_ == 0; });
        fn_ = nullptr;
        if (err_) {
            auto e = err_;
            err_ = nullptr;
            std::rethrow_exception(e);
        }
    }

private:
    void chunks() {
        for (;;) {
            size_t b = next_.fetch_add(grain_, std::memory_order_relaxed);
            if (b >= total_) return;
            size_t e = b + grain_ < total_ ? b + grain_ : total_;
            try {
                (*fn_)(b, e);
            } catch (...) {
                std::lock_guard<std::mutex> lk(m_);
                if (!err_) err_ = std::current_exception();
                next_.store(total_, std::memory_order_relaxed);
            }
        }
    }
    void work() {
        unsigned long seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
                if (stop_) return;
                seen = gen_;
            }
            chunks();
            {
                std::lock_guard<std::mutex> lk(m_);
                active_--;
            }
            done_.notify_one();
        }
    }

    int n_ = 1;
    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    bool stop_ = false;
    unsigned long gen_ = 0;
    int active_ = 0;
    std::function<void(size_t, size_t)>* fn_ = nullptr;
    size_t total_ = 0, grain_ = 1;
    std::atomic<size_t> next_{0};
    std::exception_ptr err_;
};

}  // namespace par
