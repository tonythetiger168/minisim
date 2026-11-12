// A minimal tool outside the minisim repository: it uses the kernel the way a
// closed-source tool does - the thread pool, the lint IR and the instruction
// stream - and prints the version it was built against. If the minisimCore
// package misses a header or a link dependency, this fails to build, which is
// the whole point of the test.
#include <atomic>
#include <cstdio>

#include "parallel.hpp"
#include "version.hpp"

// The headers that carry the elaborated design and the lowered program. They
// are what a tool kept in another repository actually builds against, and they
// pull in the slang front end, so they also prove the link line is right.
#include "lintir.hpp"
#include "prog.hpp"

int main() {
    // The pool: the same one every tool uses for its per-core work.
    par::Pool pool(2);
    std::atomic<int> sum{0};
    pool.run(4, [&](size_t i) { sum += (int)i + 1; });

    // The lint IR and the instruction stream are reachable as types, which is
    // what proves the headers were installed rather than merely found.
    lintir::Report report;
    Prog prog;
    prog.emit(Instr{});

    std::printf("core_consumer: minisim %s, pool sum %d, %zu findings, %zu instructions\n",
                MINISIM_VERSION, sum.load(), report.out.size(), prog.code.size());
    return (sum == 10 && prog.code.size() == 1) ? 0 : 1;
}
