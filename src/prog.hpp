// prog.hpp - process IR. Each Verilog process (always/initial) is lowered to a
// flat instruction list; one coroutine (exec, in exec.inc) interprets it.
// Flat code avoids nested coroutines: if/else are jumps. A task or function
// is its own Prog. Call pushes a return address in that same coroutine, so a
// #delay inside the callee suspends the process and keeps the address.
#pragma once
#include <cstdint>
#include <string>
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
    Finish,     // $finish
    Display,    // $display: print now, in the region the process is running in
    Strobe,     // $strobe: one print from the postponed region
    Monitor,    // $monitor: replace the watch; at most one postponed line per slot
    Call,       // push (this prog, pc+1) and transfer to callee
    Ret         // pop that address; an empty stack ends the process
};

struct Prog;

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
    // Display/Strobe/Monitor. `args` are read when the line is printed, so a
    // $strobe/$monitor sees values after NBA. %b, %d, %%, and other text.
    std::string fmt;
    std::vector<E> args;
    const Prog* callee = nullptr; // Call
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
// Signals read by this prog and by any prog it calls. @* / always_comb must see
// a function body, or a call would hide the signals that should wake it.
inline void collectProgRefs(const Prog& p, std::vector<int>& sigs, std::vector<const Prog*>& seen) {
    for (const Prog* s : seen)
        if (s == &p) return;
    seen.push_back(&p);
    for (auto& i : p.code) {
        collectRefs(i.e, sigs);
        for (auto& a : i.args) collectRefs(a, sigs);  // $display / $strobe / $monitor
        if (i.k == IK::Call && i.callee) collectProgRefs(*i.callee, sigs, seen);
    }
}
inline std::vector<std::pair<int, Edge>> readSet(const Prog& p) {
    std::vector<int> sigs;
    std::vector<const Prog*> seen;
    collectProgRefs(p, sigs, seen);
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
