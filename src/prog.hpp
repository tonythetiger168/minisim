// prog.hpp - process IR. Each Verilog process (always/initial) is lowered to a
// flat instruction list; one coroutine (exec, in exec.inc) interprets it.
// Flat code avoids recursive coroutines: if/else become jumps.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "expr.hpp"

enum Edge { ANY, POS, NEG };

enum class IK {
    Assign,     // sig = e            (blocking)
    Nba,        // sig <= e           (non-blocking)
    JmpIfNot,   // if (e is not true) pc = target
    Jmp,        // pc = target
    WaitEdge,   // @(edge sig)  or  @(e1 or e2 ...) when events is non-empty
    WaitDelay,  // #delay
    Finish      // $finish
};

struct Instr {
    IK k = IK::Jmp;
    int sig = -1;
    E e;
    Edge edge = ANY;
    size_t target = 0;
    uint64_t delay = 0;
    int drv = -1;         // Assign from a continuous assign: driver id on a resolved net
    int hi = -1, lo = 0;  // Assign/Nba: hi >= 0 means partial write sig[hi:lo]
    std::vector<std::pair<int, Edge>> events;  // WaitEdge with 2+ events: @(a or b ...)
};

struct Prog {
    std::vector<Instr> code;
    size_t emit(Instr i) { code.push_back(i); return code.size() - 1; }
    size_t here() const { return code.size(); }
};

// ---- helpers shared by lowering and tests ----
inline void collectRefs(const E& e, std::vector<int>& out) {
    if (!e) return;
    if (e->op == Op::Ref) {
        for (int s : out) if (s == e->sig) goto recurse;
        out.push_back(e->sig);
    }
recurse:
    for (auto& k : e->kids) collectRefs(k, out);
}
// Every signal read by any expression in the program (for @* / always_comb).
inline std::vector<std::pair<int, Edge>> readSet(const Prog& p) {
    std::vector<int> sigs;
    for (auto& i : p.code) collectRefs(i.e, sigs);
    std::vector<std::pair<int, Edge>> ev;
    for (int s : sigs) ev.push_back({s, ANY});
    return ev;
}
// A WaitEdge on one or more events (ev must be non-empty).
inline Instr waitInstr(const std::vector<std::pair<int, Edge>>& ev) {
    Instr i;
    i.k = IK::WaitEdge;
    i.events = ev;
    i.sig = ev[0].first;
    i.edge = ev[0].second;
    return i;
}
