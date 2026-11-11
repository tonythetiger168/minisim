// The memory table of the shared RTL-to-AIG front half (aig::Design::mems).
//
//   test_aig_mem tests/synth/mem.sv
//
// The design is elaborated and its graph is simulated with random inputs.
// Beside it runs a model built from the table alone: an array per memory,
// written through the write ports (address, data, lane, enable, in order)
// and read through the read ports. After every clock each flop of a memory
// must hold the model's bit, and every read port must show the model's word.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "slang/driver/Driver.h"

#include "aig_elab.hpp"

using namespace aig;

static int fails = 0;
#define CHECK(c, ...)                                  \
    do {                                               \
        if (!(c)) {                                    \
            if (fails++ < 20) {                        \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                std::printf(__VA_ARGS__);              \
                std::printf("\n");                     \
            }                                          \
        }                                              \
    } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_aig_mem mem.sv\n"); return 2; }
    slang::driver::Driver drv;
    drv.addStandardArgs();
    const char* args[] = {"test_aig_mem", argv[1], "--top", "mem", "-Wnone"};
    if (!drv.parseCommandLine(5, args) || !drv.processOptions() || !drv.parseAllSources()) return 2;
    auto comp = drv.createCompilation();
    drv.reportCompilation(*comp, true);
    if (!drv.reportDiagnostics(true)) return 2;
    Design d = elaborate(*comp);
    const Graph& g = d.g;

    // The table itself.
    std::map<std::string, const Memory*> by;
    for (auto& m : d.mems) by[m.name] = &m;
    CHECK(d.mems.size() == 3, "%zu memories", d.mems.size());
    auto shape = [&](const char* n, int depth, int width, size_t wr, size_t rd) {
        auto it = by.find(n);
        CHECK(it != by.end(), "no memory %s", n);
        if (it == by.end()) return;
        const Memory& m = *it->second;
        CHECK(m.depth == depth && m.width == width && m.wr.size() == wr && m.rd.size() == rd,
              "%s: depth %d width %d, %zu write and %zu read ports", n, m.depth, m.width, m.wr.size(), m.rd.size());
    };
    shape("ram", 16, 8, 1, 1);
    shape("bram", 8, 16, 2, 1);   // one write port per byte lane
    shape("rf", 8, 4, 2, 1);
    if (by.count("bram") && by["bram"]->wr.size() == 2) {
        const Memory& m = *by["bram"];
        CHECK(m.wr[0].lo == 0 && m.wr[0].data.size() == 8 && m.wr[1].lo == 8 && m.wr[1].data.size() == 8, "bram lanes");
    }

    // The flops of each memory, by word and bit.
    std::vector<std::vector<int>> flopOf(d.mems.size());
    for (size_t k = 0; k < d.mems.size(); k++) flopOf[k].assign((size_t)d.mems[k].depth * d.mems[k].width, -1);
    for (size_t i = 0; i < g.flops.size(); i++)
        for (size_t k = 0; k < d.mems.size(); k++)
            if (g.flops[i].var == d.mems[k].name) {
                CHECK(g.flops[i].bit >= 0 && g.flops[i].bit < (int)flopOf[k].size(), "flop bit out of range");
                flopOf[k][g.flops[i].bit] = (int)i;
            }
    for (size_t k = 0; k < d.mems.size(); k++)
        for (int f : flopOf[k]) CHECK(f >= 0, "%s: a bit has no flop", d.mems[k].name.c_str());
    if (fails) { std::printf("%d checks failed\n", fails); return 1; }

    // Simulate. Nodes are in topological order after cleanup().
    std::vector<uint8_t> val(g.nodes.size(), 0), state(g.flops.size(), 0);
    auto lit = [&](Lit l) -> int { return val[lnode(l)] ^ (lisneg(l) ? 1 : 0); };
    auto word = [&](const Bits& b) {
        uint64_t v = 0;
        for (size_t i = 0; i < b.size(); i++) v |= (uint64_t)lit(b[i]) << i;
        return v;
    };
    Lit clk = NOLIT;
    for (auto& p : d.ports)
        if (p.name == "clk") clk = p.bits[0];
    std::vector<std::vector<uint64_t>> model(d.mems.size());
    for (size_t k = 0; k < d.mems.size(); k++) model[k].assign(d.mems[k].depth, 0);
    uint32_t rng = 12345;
    auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 16) & 1; };
    for (int cyc = 0; cyc < 3000 && !fails; cyc++) {
        for (size_t n = 0; n < g.nodes.size(); n++) {
            const Node& nd = g.nodes[n];
            switch (nd.k) {
                case NK::Const: val[n] = 0; break;
                case NK::Input: val[n] = mklit((uint32_t)n) == clk ? 0 : (uint8_t)rnd(); break;
                case NK::Dff: val[n] = state[nd.aux]; break;
                case NK::And: val[n] = (uint8_t)(lit(nd.a) & lit(nd.b)); break;
                case NK::Wire: val[n] = (uint8_t)lit(nd.a); break;
            }
        }
        // read ports show the model's word (zero outside the array)
        for (size_t k = 0; k < d.mems.size(); k++)
            for (auto& rp : d.mems[k].rd) {
                uint64_t a = word(rp.addr), want = a < (uint64_t)d.mems[k].depth ? model[k][a] : 0;
                CHECK(word(rp.data) == want, "%s read at %llu: %llx, model %llx (cycle %d)", d.mems[k].name.c_str(),
                      (unsigned long long)a, (unsigned long long)word(rp.data), (unsigned long long)want, cyc);
            }
        // write ports, in order
        for (size_t k = 0; k < d.mems.size(); k++)
            for (auto& wp : d.mems[k].wr) {
                CHECK(wp.clk == clk && !wp.negedge, "%s: write clock", d.mems[k].name.c_str());
                uint64_t a = word(wp.addr);
                if (!lit(wp.en) || a >= (uint64_t)d.mems[k].depth) continue;
                uint64_t mask = ((wp.data.size() >= 64 ? 0 : (1ull << wp.data.size())) - 1) << wp.lo;
                model[k][a] = (model[k][a] & ~mask) | (word(wp.data) << wp.lo);
            }
        // the clock edge
        std::vector<uint8_t> next(g.flops.size());
        for (size_t i = 0; i < g.flops.size(); i++) next[i] = (uint8_t)lit(g.flops[i].d);
        state = next;
        for (size_t k = 0; k < d.mems.size(); k++)
            for (size_t b = 0; b < flopOf[k].size(); b++) {
                int want = (int)((model[k][b / d.mems[k].width] >> (b % d.mems[k].width)) & 1);
                CHECK(state[flopOf[k][b]] == want, "%s word %zu bit %zu after cycle %d", d.mems[k].name.c_str(),
                      b / d.mems[k].width, b % d.mems[k].width, cyc);
            }
    }
    if (fails) { std::printf("%d checks failed\n", fails); return 1; }
    std::printf("PASS: 3 memories, 5 write ports and 3 read ports match their flops over 3000 cycles\n");
    return 0;
}
