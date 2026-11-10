// parallel.hpp: every index runs once, on any thread count; the pool is
// reusable; an exception reaches the caller; -threads N parses.
#include <cassert>
#include <numeric>
#include <stdexcept>

#include "parallel.hpp"

int main() {
    assert(par::defaultThreads() >= 1);
    for (unsigned threads : {1u, 2u, 3u, 8u, 33u}) {
        std::vector<int> hit(1000, 0);
        par::parallelFor(hit.size(), threads, [&](size_t i) { hit[i]++; });
        for (int h : hit) assert(h == 1);

        par::Pool pool(threads);
        assert(pool.size() == threads);
        std::vector<long> out(257);
        for (int round = 0; round < 50; round++) {
            pool.run(out.size(), [&](size_t i) { out[i] = (long)i * round; });
            assert(std::accumulate(out.begin(), out.end(), 0L) == 256L * 257 / 2 * round);
        }
        pool.run(0, [&](size_t) { assert(false); });
        // Thousands of small batches (a timing graph's levels): each index once.
        std::vector<int> small(3, 0);
        for (int round = 0; round < 5000; round++)
            pool.run(1 + round % 3, [&](size_t i) { small[i]++; });
        assert(small[0] == 5000 && small[1] == 3333 && small[2] == 1666);
        // The same with workers that look for the next batch before they sleep.
        pool.spin(2000);
        for (int round = 0; round < 5000; round++)
            pool.run(1 + round % 3, [&](size_t i) { small[i]++; });
        assert(small[0] == 10000 && small[1] == 6666 && small[2] == 3332);
        for (int round = 0; round < 50; round++) {
            pool.run(out.size(), [&](size_t i) { out[i] = (long)i * round; });
            assert(std::accumulate(out.begin(), out.end(), 0L) == 256L * 257 / 2 * round);
        }
        pool.spin(0);

        // Chunks cover [0, n) once; a sum is the same bits on any thread count.
        std::vector<int> cover(1003, 0);
        pool.chunks(cover.size(), 64, [&](size_t b, size_t e, size_t c) {
            assert(b == c * 64 && e <= cover.size());
            for (size_t i = b; i < e; i++) cover[i]++;
        });
        for (int h : cover) assert(h == 1);
        auto part = [](size_t b, size_t e) { double s = 0; for (size_t i = b; i < e; i++) s += 1.0 / (double)(i + 1); return s; };
        static double first = 0;
        double total = pool.sum(100000, 1000, part);
        if (threads == 1) first = total;
        assert(total == first);

        bool caught = false;
        try {
            pool.run(100, [&](size_t i) { if (i == 7) throw std::runtime_error("seven"); });
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "seven";
        }
        assert(caught);
        caught = false;
        try {
            par::parallelFor(100, threads, [&](size_t i) { if (i == 7) throw std::runtime_error("seven"); });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        assert(caught);
        pool.run(out.size(), [&](size_t i) { out[i] = 1; });   // still usable
        assert(std::accumulate(out.begin(), out.end(), 0L) == 257);
    }

    char a0[] = "tool", a1[] = "-threads", a2[] = "6", a3[] = "x.sv", a4[] = "-j", a5[] = "2";
    char* argv[] = {a0, a1, a2, a3, a4, a5};
    unsigned threads = 0;
    int i = 1;
    assert(par::threadsArg(6, argv, i, threads, "tool") && threads == 6 && i == 2);
    i = 3;
    assert(!par::threadsArg(6, argv, i, threads, "tool") && i == 3);
    i = 4;
    assert(par::threadsArg(6, argv, i, threads, "tool") && threads == 2 && i == 5);
    return 0;
}
