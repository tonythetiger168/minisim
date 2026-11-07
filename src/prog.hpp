// prog.hpp - process IR. Each Verilog process (always/initial) is lowered to a
// flat instruction list; one coroutine (exec, in exec.inc) interprets it.
// Flat code avoids nested coroutines: if/else are jumps. A task or function
// is its own Prog. Call pushes a return address in that same coroutine, so a
// #delay inside the callee suspends the process and keeps the address.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "expr.hpp"

enum Edge { ANY, POS, NEG };

// IK::Dyn commands (7.5, 7.8, 7.10).
enum DynCmd { D_SET, D_NEW, D_DELETE, D_DELETE_AT, D_PUSH_BACK, D_PUSH_FRONT, D_INSERT, D_POP_FRONT,
              D_POP_BACK, D_FIRST, D_LAST, D_NEXT, D_PREV, D_COPY, D_APPEND, D_APPEND_SLICE, D_REVERSE,
              D_SORT, D_RSORT, D_SHUFFLE };
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
    Fork,       // start each of `branches` as a process; join 0 = all, 1 = any, 2 = none
    ReadMem,    // $readmemh / $readmemb into `arr` (fmt is the file name)
    Disable,    // disable <block> run by other processes: end every process whose root is block `sig`
    WaitFork,   // wait until every child process of this one has ended (9.6.1)
    DisableFork,// end every descendant process of this one (9.6.3)
    Dyn,        // a dynamic array / queue / associative array command: dcmd on handle `sig`
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
    int drv = -1;         // Assign from a continuous driver: driver id on a resolved net
    uint8_t s0 = STR_STRONG, s1 = STR_STRONG;  // that driver's 0-strength and 1-strength
    int hi = -1, lo = 0;  // Assign/Nba: hi >= 0 means partial write sig[hi:lo]
    std::vector<std::pair<int, Edge>> events;  // WaitEdge with 2+ events: @(a or b ...)
    // Display/Strobe/Monitor. `args` are read when the line is printed, so a
    // $strobe/$monitor sees values after NBA. %b, %d, %%, and other text.
    std::string fmt;
    bool nonl = false;    // Display from $write: no newline at the end
    std::vector<E> args;
    const Prog* callee = nullptr; // Call
    std::vector<const Prog*> branches;  // Fork
    int join = 0;                       // Fork: 0 join, 1 join_any, 2 join_none
    // Assign/Nba to an array element chosen at run time: the target is
    // arr->base + offset(aidx). An invalid index skips the write (7.4.6).
    // ReadMem: arr is the memory, args hold [start [, finish]], hex selects $readmemh.
    std::shared_ptr<const ArrShape> arr;
    std::vector<E> aidx;
    // Assign/Nba to a bit chosen at run time (v[i] = ...): hi = lo = bidx.
    // With pw > 0 it is v[bidx +: pw] (or -: when pdown).
    E bidx;
    // WaitDelay: `de`, when set, is the delay in ticks, computed at run time.
    // Nba with nbaLater: the update happens `delay` / `de` ticks later (9.4.5).
    E de;
    bool nbaLater = false;
    // Dyn: the command, its operands in args, the value in e, another
    // container dsrc, a result signal dres, and the key variable dkey.
    // Assign with dyn >= 0 writes element args[0] of container dyn.
    int dcmd = 0, dsrc = -1, dres = -1, dkey = -1, dyn = -1;
    int pw = 0;
    bool pdown = false;
    bool hex = false;
};

struct Prog {
    std::vector<Instr> code;
    // Named blocks this process runs as a whole (an initial's named body, a
    // fork branch that is a named block, every branch of a named fork).
    // `disable` of one of them from another process ends the process.
    std::vector<int> roots;
    size_t emit(Instr i) { code.push_back(i); return code.size() - 1; }
    size_t here() const { return code.size(); }
};

// ---- helpers shared by lowering and tests ----
// Signals an expression reads, each once, in first-read order. A variable
// array index reads every element, so a wait on it wakes on any of them.
inline void collectRefsInto(const E& e, std::vector<int>& out, std::unordered_set<int>& seen) {
    if (!e) return;
    auto add = [&](int s) { if (seen.insert(s).second) out.push_back(s); };
    if (e->op == Op::Ref) add(e->sig);
    if (e->op == Op::ArrRead)
        for (int i = 0; i < e->arr->count; i++) add(e->arr->base + i);
    if (e->op == Op::DynGet || e->op == Op::DynSize || e->op == Op::DynExists || e->op == Op::DynReduce) add(e->sig);
    for (auto& k : e->kids) collectRefsInto(k, out, seen);
}
inline void collectRefs(const E& e, std::vector<int>& out) {
    std::unordered_set<int> seen(out.begin(), out.end());
    collectRefsInto(e, out, seen);
}
// Signals read by this prog and by any prog it calls. @* / always_comb must see
// a function body, or a call would hide the signals that should wake it.
inline void collectProgRefsInto(const Prog& p, std::vector<int>& sigs, std::unordered_set<int>& have,
                                std::vector<const Prog*>& seen) {
    for (const Prog* s : seen)
        if (s == &p) return;
    seen.push_back(&p);
    for (auto& i : p.code) {
        collectRefsInto(i.e, sigs, have);
        for (auto& a : i.args) collectRefsInto(a, sigs, have);  // $display / $strobe / $monitor
        for (auto& a : i.aidx) collectRefsInto(a, sigs, have);  // a[i] = ... reads i
        if (i.k == IK::Dyn || i.dyn >= 0)
            for (auto& a : i.args) collectRefsInto(a, sigs, have);
        collectRefsInto(i.bidx, sigs, have);
        if (i.k == IK::Call && i.callee) collectProgRefsInto(*i.callee, sigs, have, seen);
        for (const Prog* b : i.branches) collectProgRefsInto(*b, sigs, have, seen);
    }
}
inline void collectProgRefs(const Prog& p, std::vector<int>& sigs, std::vector<const Prog*>& seen) {
    std::unordered_set<int> have(sigs.begin(), sigs.end());
    collectProgRefsInto(p, sigs, have, seen);
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
