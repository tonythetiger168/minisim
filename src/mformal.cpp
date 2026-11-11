// mformal.cpp - a formal property checker for SystemVerilog, on the same
// slang front end and the same lowering as msim.
//
//   mformal design.sv -top m
//
// It proves or falsifies the design's assertions for every input sequence,
// instead of the ones a testbench happens to drive:
//   assert property / immediate assert   proven, or falsified with a trace
//   assume property / immediate assume   a constraint on the inputs
//   cover property / cover sequence      covered with a trace, or unreachable
//
// How it works
//   1. lower() gives the process IR msim runs (prog.hpp) and the concurrent
//      assertions as terms (sva_engine.hpp).
//   2. Each process is executed symbolically, once: a combinational process
//      gives its outputs as functions of the signals it reads, a clocked one
//      the next value of every flop. Values are vectors of literals of an
//      and-inverter graph (AIG), two-state. What cannot be modelled (delays,
//      classes, another clock) is cut: its outputs become free inputs, which
//      keeps every proof sound and is reported.
//   3. Each assertion becomes a monitor automaton whose states are the terms
//      its attempts can become (the Brzozowski derivatives the simulator
//      computes, here for every valuation of the atoms), one latch per term.
//   4. Per property, the cone of influence is unrolled into the shared SAT
//      solver (src/sat.hpp, CaDiCaL):
//      bounded model checking from the reset state finds counterexamples and
//      cover traces; k-induction with simple-path constraints proves.
//   5. A counterexample is replayed on the AIG and written as a VCD of the
//      whole design (mformal_out/<property>.vcd).
//
// Properties are independent, so they are checked in parallel (-threads N);
// the result does not depend on the thread count.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aig.hpp"
#include "lower.hpp"
#include "parallel.hpp"
#include "sat.hpp"
#include "slang/driver/Driver.h"
#include "version.hpp"

namespace {

struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void unsup(const std::string& what) { throw Unsupported(what); }

// ------------------------------------------------------------------ AIG
// The graph is the shared aig::Graph (src/aig.hpp): literal = 2 * node +
// inverted, node 0 is constant false. This is only a view of it with the
// signed literals the checker uses (-1 means "none" in several places).
struct Aig {
    aig::Graph gr;
    int nIn = 0;
    // n[node] = {-1, input number} for an input, the two fanins for an AND.
    struct Nodes {
        const aig::Graph& gr;
        size_t size() const { return gr.nodes.size(); }
        std::array<int, 2> operator[](size_t i) const {
            const aig::Node& nd = gr.nodes[i];
            if (nd.k == aig::NK::Input) return {-1, nd.aux};
            return {(int)nd.a, (int)nd.b};
        }
    } n{gr};
    Aig() = default;
    Aig(const Aig&) = delete;
    Aig& operator=(const Aig&) = delete;
    int input() {
        nIn++;
        return (int)gr.input({});
    }
    bool isInput(int node) const { return node > 0 && gr.nodes[(size_t)node].k == aig::NK::Input; }
    int And(int a, int b) { return (int)gr.andl((aig::Lit)a, (aig::Lit)b); }
    int Or(int a, int b) { return (int)gr.orl((aig::Lit)a, (aig::Lit)b); }
    int Xor(int a, int b) { return (int)gr.xorl((aig::Lit)a, (aig::Lit)b); }
    int Mux(int c, int t, int e) { return (int)gr.mux((aig::Lit)c, (aig::Lit)t, (aig::Lit)e); }
};

// A symbolic value: bit 0 first.
struct SV {
    std::vector<int> b;
    bool sgn = false;
    int w() const { return (int)b.size(); }
};

struct Ops {
    Aig& g;
    SV konst(uint64_t v, int w, bool sgn = false) {
        SV r;
        r.sgn = sgn;
        for (int i = 0; i < w; i++) r.b.push_back(i < 64 && ((v >> i) & 1) ? 1 : 0);
        return r;
    }
    SV ext(SV a, int w, bool sign) {
        int fill = sign && !a.b.empty() ? a.b.back() : 0;
        a.b.resize((size_t)w, fill);
        return a;
    }
    // Both signed: sign-extend to the wider; else zero-extend, unsigned (expr.hpp align_signed).
    void align(SV& a, SV& b) {
        bool s = a.sgn && b.sgn;
        int w = std::max(a.w(), b.w());
        a = ext(a, w, s);
        b = ext(b, w, s);
        a.sgn = b.sgn = s;
    }
    int redOr(const SV& a) {
        int r = 0;
        for (int x : a.b) r = g.Or(r, x);
        return r;
    }
    int redAnd(const SV& a) {
        int r = 1;
        for (int x : a.b) r = g.And(r, x);
        return r;
    }
    int redXor(const SV& a) {
        int r = 0;
        for (int x : a.b) r = g.Xor(r, x);
        return r;
    }
    SV bit(int l) { SV r; r.b = {l}; return r; }
    SV add(const SV& a, const SV& b, int cin) {
        SV r;
        r.sgn = a.sgn;
        int c = cin;
        for (int i = 0; i < a.w(); i++) {
            int x = g.Xor(a.b[(size_t)i], b.b[(size_t)i]);
            r.b.push_back(g.Xor(x, c));
            c = g.Or(g.And(a.b[(size_t)i], b.b[(size_t)i]), g.And(x, c));
        }
        return r;
    }
    SV inv(SV a) {
        for (int& x : a.b) x ^= 1;
        return a;
    }
    SV sub(const SV& a, const SV& b) { return add(a, inv(b), 1); }
    SV neg(const SV& a) { return add(inv(a), konst(0, a.w()), 1); }
    SV mul(const SV& a, const SV& b) {
        int w = a.w();
        SV acc = konst(0, w, a.sgn);
        for (int i = 0; i < w; i++) {
            if (b.b[(size_t)i] == 0) continue;
            SV part = konst(0, w);
            for (int j = i; j < w; j++) part.b[(size_t)j] = g.And(a.b[(size_t)(j - i)], b.b[(size_t)i]);
            acc = add(acc, part, 0);
            acc.sgn = a.sgn;
        }
        return acc;
    }
    int ult(const SV& a, const SV& b) {   // a < b, unsigned, equal widths
        int lt = 0;
        for (int i = 0; i < a.w(); i++) {
            int x = a.b[(size_t)i], y = b.b[(size_t)i];
            lt = g.Mux(g.Xor(x, y), y, lt);
        }
        return lt;
    }
    int lt(SV a, SV b) {
        align(a, b);
        if (a.sgn && a.w() > 0) {
            a.b.back() ^= 1;
            b.b.back() ^= 1;
        }
        return ult(a, b);
    }
    int eq(SV a, SV b) {
        align(a, b);
        int r = 1;
        for (int i = 0; i < a.w(); i++) r = g.And(r, g.Xor(a.b[(size_t)i], b.b[(size_t)i]) ^ 1);
        return r;
    }
    int eqConst(const SV& a, int64_t v) {   // a (extended by its sign) == v
        int r = 1;
        for (int i = 0; i < 64; i++) {
            int bitv = (v >> i) & 1;
            int l = i < a.w() ? a.b[(size_t)i] : (a.sgn && a.w() > 0 ? a.b.back() : 0);
            r = g.And(r, bitv ? l : l ^ 1);
            if (r == 0) break;
            if (i >= a.w() && !(a.sgn && a.w() > 0)) {   // the rest of v must be 0
                if ((v >> i) != 0) r = 0;
                break;
            }
        }
        return r;
    }
    SV mux(int c, SV t, SV e) {
        if (c == 1) return t;
        if (c == 0) return e;
        bool s = t.sgn && e.sgn;
        int w = std::max(t.w(), e.w());
        t = ext(t, w, false);
        e = ext(e, w, false);
        SV r;
        r.sgn = s;
        for (int i = 0; i < w; i++) r.b.push_back(g.Mux(c, t.b[(size_t)i], e.b[(size_t)i]));
        return r;
    }
    // Unsigned division, restoring: quotient and remainder.
    void udivmod(const SV& a, const SV& b, SV& q, SV& r) {
        int w = a.w();
        q = konst(0, w);
        SV rem = konst(0, w + 1);
        SV bb = ext(b, w + 1, false);
        for (int i = w - 1; i >= 0; i--) {
            rem.b.insert(rem.b.begin(), a.b[(size_t)i]);
            rem.b.pop_back();
            int ge = ult(rem, bb) ^ 1;
            SV d = sub(rem, bb);
            rem = mux(ge, d, rem);
            q.b[(size_t)i] = ge;
        }
        rem.b.resize((size_t)w);
        r = rem;
    }
    SV divmod(SV a, SV b, bool rem) {
        align(a, b);
        bool s = a.sgn;
        if (a.w() == 0) return a;
        int sa = s ? a.b.back() : 0, sb = s ? b.b.back() : 0;
        SV ua = s ? mux(sa, neg(a), a) : a, ub = s ? mux(sb, neg(b), b) : b;
        SV q, r;
        udivmod(ua, ub, q, r);
        SV out = rem ? r : q;
        if (s) out = mux(rem ? sa : g.Xor(sa, sb), neg(out), out);
        out.sgn = s;
        return out;
    }
    // Shifts by a symbolic count. dir 0: left, 1: logical right, 2: arithmetic right.
    SV shift(const SV& a, const SV& n, int dir) {
        int w = a.w();
        SV r = a;
        int fill = dir == 2 && w > 0 ? a.b.back() : 0;
        int big = 0;   // the count is >= w
        for (int k = 0; k < n.w(); k++) {
            if (k >= 31 || (1LL << k) >= w) {
                big = g.Or(big, n.b[(size_t)k]);
                continue;
            }
            int sh = 1 << k;
            SV t = r;
            for (int i = 0; i < w; i++) {
                int from = dir == 0 ? i - sh : i + sh;
                t.b[(size_t)i] = from < 0 ? 0 : from >= w ? fill : r.b[(size_t)from];
            }
            r = mux(n.b[(size_t)k], t, r);
        }
        SV all = a;
        for (int& x : all.b) x = fill;
        r = mux(big, all, r);
        r.sgn = a.sgn;
        return r;
    }
    SV popcount(const SV& a, int w) {
        SV r = konst(0, w);
        for (int x : a.b) {
            if (x == 0) continue;
            SV one = konst(0, w);
            one.b[0] = x;
            r = add(r, one, 0);
        }
        return r;
    }
};

// ------------------------------------------------------------- the model
struct SigInfo {
    std::string name;
    int w = 1;
    bool net = false;
};

// One bit of state: its current-value input, its next value, its initial value.
struct StateBit {
    int in = 0;        // AIG literal (an input)
    int next = -1;     // AIG literal
    int8_t init = -1;  // 0, 1, or -1: any
    bool aux = false;  // a latch of a property's monitor, not of the design
};

struct Property {
    std::string name;
    int dir = AD_ASSERT;   // AD_ASSERT, AD_COVER, AD_ASSUME
    int bad = 0;           // assert / assume: fails in this cycle. cover: hit in this cycle.
    bool liveness = false; // has a strong unbounded obligation: also checked for a loop that never meets it
    int liveMark = 0;      // free input: follow the open obligations from this cycle (see buildMonitor)
    int liveBack = 0;      // assert: the obligation picked then is open again. assume: one followed since then is still open
    std::vector<int> liveState;   // the monitor's latches: part of the state a loop must repeat
    std::vector<int> liveOwn;     // the state bits that follow the chain: they need not repeat
    std::string liveInexact;      // assert: why the liveness part is not decided
    std::string fairInexact;      // assume: why its liveness part is not used
    int live = 0;          // the liveness part of `bad`: such a loop closes in this cycle
    int saved = 0;         // the loop's first state has been saved
    std::string skipped;   // not modelled: why
    int app = 0;           // a coverage target made by an app: 1 -app toggle, 2 a -connect condition
    std::string file;
    int line = 0;
};

struct Options {
    std::string clock, reset;
    bool resetLow = false, noReset = false;
    int resetCycles = 1;
    int depth = 20;
    int threads = 0;
    long long conflicts = 2000000;
    std::string outDir = "mformal_out";
    std::string json;
    std::string aiger;   // write the model here and stop
    std::vector<std::string> connect;   // -connect: connectivity specifications
    bool appToggle = false;   // -app toggle: can every signal bit be 0 and be 1?
    bool toggleDeep = false;  // -app toggle: spend the whole budget on each target
    int toggleMax = 4096;     // -app toggle: skip signals wider than this
    std::vector<std::string> only;
    bool quiet = false, verbose = false, noTrace = false, dumpIr = false, werror = false;
    bool bmcOnly = false;
    long long pdrCalls = 2000000;   // PDR: solver calls per property
    int pdrFrames = 500;
    bool noPdr = false;
    bool tb = false;   // also write a testbench that replays each trace in a simulator
};

struct Model {
    Aig g;
    Ops o{g};
    Options opt;
    std::vector<SigInfo> sigs;
    std::string top;   // the top instance's name: the root scope of a trace
    std::string topModule;
    std::vector<std::pair<std::string, int>> topInputs;   // input ports of the top: name, signal
    LowerResult lowered;

    enum PK { P_SKIP, P_COMB, P_CLOCKED, P_INIT, P_CUT };
    struct Ent {
        std::vector<int> b;
        std::vector<char> t;   // touched bits
    };
    using Env = std::unordered_map<int, Ent>;
    struct ProcInfo {
        PK kind = P_SKIP;
        size_t lo = 0, hi = 0;          // body: [lo, hi)
        std::vector<int> writes;        // signals it (or what it calls) assigns
        bool done = false, running = false;
        Env blk, nba;
        std::string why;                // P_CUT: the reason
    };
    std::vector<ProcInfo> pi;
    std::vector<std::vector<int>> writers;   // signal -> processes (not init)
    std::vector<std::vector<int8_t>> initVal; // signal -> bit -> 0 / 1 / -1
    std::unordered_map<int, int> shadowSrc;   // sampled-value shadow -> its signal
    struct Stage { int clockIdx, line, k; };
    std::unordered_map<int, Stage> stageOf;   // $past stage signal

    int clockSig = -1;
    bool hasReset = false;
    int startLit = 1;     // assertions are being checked: 0 while the reset is held
    std::vector<int8_t> piKind;   // per AIG input: 0 free, 1 state

    std::vector<std::optional<SV>> curMemo;
    std::vector<char> isState;
    std::vector<int> stateSigs;         // in the order found
    std::vector<StateBit> stateBits;
    std::unordered_map<int, int> stateBitOfInput;   // AIG node -> stateBits index
    std::vector<std::pair<int, int>> stateRange;    // per signal: first state bit, or -1
    std::vector<std::string> inputName;             // per AIG input number
    std::set<std::string> procLocals;               // variables declared inside a procedure (loop counters, temporaries)
    std::set<std::string> cutNotes;                 // what was cut, for the report
    std::vector<int> aliasOf;                       // signal -> the signal it is a plain copy of, or -1
    std::vector<Property> props;
    std::vector<std::pair<int, int>> immediate;     // assertion id, fails-now literal
    struct Sliced { Env blk; };
    std::map<std::pair<int, int>, bool> sliceRunning;

    // ---- signals ----
    int newInputs(int w, std::vector<int>& out, const std::string& name, int8_t kind) {
        for (int i = 0; i < w; i++) {
            out.push_back(g.input());
            piKind.push_back(kind);
            inputName.push_back(w == 1 ? name : name + "[" + std::to_string(i) + "]");
        }
        return 0;
    }
    SV freeSig(int s, const std::string& suffix = "") {
        SV v;
        newInputs(sigs[(size_t)s].w, v.b, sigs[(size_t)s].name + suffix, 0);
        return v;
    }
    int canon(int s) const {
        for (int n = 0; n < 64 && s >= 0 && (size_t)s < aliasOf.size() && aliasOf[(size_t)s] >= 0; n++) s = aliasOf[(size_t)s];
        return s;
    }

    // ---- expressions ----
    using Reader = std::function<SV(int)>;
    SV constOf(const Val& k) {
        if (k.real || k.str) unsup("a real or string value");
        SV r;
        r.sgn = k.sgn;
        for (int i = 0; i < k.w; i++) r.b.push_back(bitAt(k, i) == 1 ? 1 : 0);   // x and z read as 0
        return r;
    }
    int truthOf(const SV& v) { return o.redOr(v); }
    // The care mask of a wildcard comparison's constant side.
    static bool wildBits(const E& e, int mode, std::vector<char>& care) {
        if (!e || e->op != Op::Const) return false;
        care.clear();
        for (int i = 0; i < e->k.w; i++) {
            int t = bitAt(e->k, i);
            care.push_back(mode == 0 ? t != 3 : t < 2);   // casez: z is a wildcard; casex, ==?: x and z
        }
        return true;
    }
    // Per dimension, the literal "index == value" for each value in range, combined per element.
    std::vector<int> elemSel(const ArrShape& a, const std::vector<E>& idx, const Reader& rd) {
        if (idx.size() != a.dims.size()) unsup("an array slice");
        std::vector<int> sel{1};
        for (size_t d = 0; d < a.dims.size(); d++) {
            SV iv = ev(idx[d], rd);
            auto [l, r] = a.dims[d];
            int64_t n = a.size(d);
            std::vector<int> next;
            next.reserve(sel.size() * (size_t)n);
            std::vector<int> eqs;
            for (int64_t k = 0; k < n; k++) eqs.push_back(o.eqConst(iv, l <= r ? l + k : l - k));
            for (int s : sel)
                for (int64_t k = 0; k < n; k++) next.push_back(g.And(s, eqs[(size_t)k]));
            sel.swap(next);
            if (sel.size() > 65536) unsup("an array of more than 65536 elements");
        }
        return sel;
    }
    SV ev(const E& e, const Reader& rd) {
        if (!e) unsup("an empty expression");
        auto k = [&](int i) { return ev(e->kids[(size_t)i], rd); };
        switch (e->op) {
            case Op::Const: return constOf(e->k);
            case Op::Ref: {
                if (e->at) unsup("a class property");
                if (isRel(e->sig)) unsup("an automatic variable");
                SV v = rd(e->sig);
                v.sgn = e->sgn;
                return v;
            }
            case Op::Not: { SV a = o.inv(k(0)); return a; }
            case Op::And: case Op::Or: case Op::Xor: {
                SV a = k(0), b = k(1);
                bool s = a.sgn && b.sgn;
                int w = std::max(a.w(), b.w());
                a = o.ext(a, w, false);
                b = o.ext(b, w, false);
                SV r;
                r.sgn = s;
                for (int i = 0; i < w; i++) {
                    int x = a.b[(size_t)i], y = b.b[(size_t)i];
                    r.b.push_back(e->op == Op::And ? g.And(x, y) : e->op == Op::Or ? g.Or(x, y) : g.Xor(x, y));
                }
                return r;
            }
            case Op::Add: case Op::Sub: case Op::Mul: {
                SV a = k(0), b = k(1);
                o.align(a, b);
                return e->op == Op::Add ? o.add(a, b, 0) : e->op == Op::Sub ? o.sub(a, b) : o.mul(a, b);
            }
            case Op::Div: return o.divmod(k(0), k(1), false);
            case Op::Mod: return o.divmod(k(0), k(1), true);
            case Op::Eq: return o.bit(o.eq(k(0), k(1)));
            case Op::Ne: return o.bit(o.eq(k(0), k(1)) ^ 1);
            case Op::CaseEq: {
                SV a = k(0), b = k(1);
                a.sgn = b.sgn = false;
                return o.bit(o.eq(a, b));
            }
            case Op::CaseZEq: case Op::CaseXEq: case Op::WildEq: {
                int mode = e->op == Op::CaseZEq ? 0 : 1;
                SV a = k(0), b = k(1);
                if (e->op == Op::WildEq) o.align(a, b);
                int w = std::max(a.w(), b.w());
                a = o.ext(a, w, false);
                b = o.ext(b, w, false);
                std::vector<char> c0, c1;
                bool h0 = e->op != Op::WildEq && wildBits(e->kids[0], mode, c0), h1 = wildBits(e->kids[1], mode, c1);
                int r = 1;
                for (int i = 0; i < w; i++) {
                    if (h0 && i < (int)c0.size() && !c0[(size_t)i]) continue;
                    if (h1 && i < (int)c1.size() && !c1[(size_t)i]) continue;
                    r = g.And(r, g.Xor(a.b[(size_t)i], b.b[(size_t)i]) ^ 1);
                }
                return o.bit(r);
            }
            case Op::Lt: return o.bit(o.lt(k(0), k(1)));
            case Op::Gt: return o.bit(o.lt(k(1), k(0)));
            case Op::Le: return o.bit(o.lt(k(1), k(0)) ^ 1);
            case Op::Ge: return o.bit(o.lt(k(0), k(1)) ^ 1);
            case Op::Shl: return o.shift(k(0), k(1), 0);
            case Op::Shr: return o.shift(k(0), k(1), 1);
            case Op::Asr: { SV a = k(0); return o.shift(a, k(1), a.sgn ? 2 : 1); }
            case Op::Resize: {
                SV v = k(0);
                SV r = o.ext(v, e->hi, e->lo && e->hi > v.w());
                r.sgn = e->sgn;
                return r;
            }
            case Op::LogNot: return o.bit(truthOf(k(0)) ^ 1);
            case Op::LogAnd: return o.bit(g.And(truthOf(k(0)), truthOf(k(1))));
            case Op::LogOr: return o.bit(g.Or(truthOf(k(0)), truthOf(k(1))));
            case Op::RedAnd: return o.bit(o.redAnd(k(0)));
            case Op::RedOr: return o.bit(o.redOr(k(0)));
            case Op::RedXor: return o.bit(o.redXor(k(0)));
            case Op::Slice: {
                SV a = k(0), r;
                for (int i = e->lo; i <= e->hi; i++) r.b.push_back(i >= 0 && i < a.w() ? a.b[(size_t)i] : 0);
                if (r.b.empty()) r.b.push_back(0);
                return r;
            }
            case Op::Index: {
                SV a = k(0), i = k(1);
                i.sgn = false;
                int r = 0;
                for (int b = 0; b < a.w(); b++) {
                    if (a.b[(size_t)b] == 0) continue;
                    r = g.Or(r, g.And(o.eqConst(i, b), a.b[(size_t)b]));
                }
                return o.bit(r);
            }
            case Op::Concat: {
                SV r;
                for (size_t i = e->kids.size(); i-- > 0;) {
                    SV p = k((int)i);
                    r.b.insert(r.b.end(), p.b.begin(), p.b.end());
                }
                return r;
            }
            case Op::Mux: {
                int c = truthOf(k(0));
                if (c == 1) return k(1);
                if (c == 0) return k(2);
                return o.mux(c, k(1), k(2));
            }
            case Op::Pow: {
                const E& a = e->kids[0];
                const E& b = e->kids[1];
                if (a->op == Op::Const && b->op == Op::Const) return constOf(v_pow(a->k, b->k));
                if (a->op == Op::Const && !anyX(a->k) && a->k.w <= 64 && a->k.v == 2) {
                    SV one = o.konst(1, a->k.w, a->k.sgn);
                    return o.shift(one, k(1), 0);
                }
                unsup("** with operands that are not constant");
            }
            case Op::ArrRead: {
                const ArrShape& a = *e->arr;
                if (isRel(a.base)) unsup("an automatic array");
                std::vector<int> sel = elemSel(a, e->kids, rd);
                SV r = o.konst(0, a.elemW, a.sgn);
                for (int64_t n = 0; n < (int64_t)sel.size() && n < a.count; n++) {
                    if (sel[(size_t)n] == 0) continue;
                    SV v = rd(a.base + (int)n);
                    for (int i = 0; i < a.elemW && i < v.w(); i++)
                        r.b[(size_t)i] = g.Or(r.b[(size_t)i], g.And(sel[(size_t)n], v.b[(size_t)i]));
                }
                return r;
            }
            case Op::DynSlice: {   // a[i +: w] / a[i -: w]
                SV a = k(0), i = k(1);
                int w = e->hi;
                SV r = o.konst(0, w);
                for (int s = -(w - 1); s < a.w(); s++) {   // s: the low bit's position
                    int at = o.eqConst(i, e->lo ? (int64_t)s + w - 1 : s);
                    if (at == 0) continue;
                    for (int b = 0; b < w; b++) {
                        int from = s + b;
                        if (from < 0 || from >= a.w()) continue;
                        r.b[(size_t)b] = g.Or(r.b[(size_t)b], g.And(at, a.b[(size_t)from]));
                    }
                }
                return r;
            }
            case Op::SysFn: {
                if (e->hi == SF_COUNTBITS) {
                    SV a = k(0);
                    SV r = o.konst(0, 32, true);
                    if (e->lo & 2) r = o.popcount(a, 32);
                    if (e->lo & 1) r = o.add(r, o.popcount(o.inv(a), 32), 0);
                    r.sgn = true;
                    return r;
                }
                if (e->hi == SF_CLOG2 && e->kids[0]->op == Op::Const) return constOf(v_sysfn(SF_CLOG2, e->lo, {e->kids[0]->k}));
                unsup("a system function");
            }
            default: break;
        }
        unsup("an expression of this kind (op " + std::to_string((int)e->op) + ")");
    }

    // ---- symbolic execution of one process ----
    struct St {
        int cond = 1;
        Env blk, nba;
    };
    struct Exec {
        Model& m;
        Reader base;          // a signal the process has not written
        bool initMode = false;
        const std::unordered_set<int>* only = nullptr;   // a slice: assign to these signals only
        std::vector<std::pair<int, int>> asserts;
        int depth = 0;
        long steps = 0;

        SV read(St& st, int s) {
            if (auto it = st.blk.find(s); it != st.blk.end()) {
                bool all = true;
                for (char c : it->second.t) all = all && c;
                SV v;
                if (all) { v.b = it->second.b; return v; }
                v = base(s);
                for (size_t i = 0; i < v.b.size(); i++)
                    if (it->second.t[i]) v.b[i] = it->second.b[i];
                return v;
            }
            return base(s);
        }
        Ent& entry(Env& env, int s) {
            auto [it, fresh] = env.try_emplace(s);
            if (fresh) {
                int w = m.sigs[(size_t)s].w;
                it->second.b.assign((size_t)w, 0);
                it->second.t.assign((size_t)w, 0);
            }
            return it->second;
        }
        // Merge b into a (two exclusive paths). A bit one path did not assign
        // keeps what it had before the process ran.
        void mergeEnv(Env& a, Env& b, int condB) {
            for (auto& [s, eb] : b) entry(a, s);
            for (auto& [s, ea] : a) {
                auto it = b.find(s);
                SV old;
                bool have = false;
                for (size_t i = 0; i < ea.b.size(); i++) {
                    bool tb = it != b.end() && it->second.t[i];
                    if (!ea.t[i] && !tb) continue;
                    if ((!ea.t[i] || !tb) && !have) { old = base(s); have = true; }
                    int va = ea.t[i] ? ea.b[i] : old.b[i], vb = tb ? it->second.b[i] : old.b[i];
                    ea.b[i] = m.g.Mux(condB, vb, va);
                    ea.t[i] = 1;
                }
            }
        }
        void merge(St& a, St& b) {
            mergeEnv(a.blk, b.blk, b.cond);
            mergeEnv(a.nba, b.nba, b.cond);
            a.cond = m.g.Or(a.cond, b.cond);
        }
        void put(std::map<size_t, St>& pend, size_t pc, St st) {
            if (st.cond == 0) return;
            auto it = pend.find(pc);
            if (it == pend.end()) pend.emplace(pc, std::move(st));
            else merge(it->second, st);
        }
        void write(St& st, const Instr& i, bool nba) {
            Reader rd = [&](int s) { return read(st, s); };
            if (i.dyn >= 0) unsup("an assignment to a queue or dynamic array");
            if (isRel(i.sig)) unsup("an assignment to an automatic variable");
            if (i.de || i.delay || i.nbaLater) unsup("an assignment with a delay");
            SV val = m.ev(i.e, rd);
            Env& env = nba ? st.nba : st.blk;
            // the old value of a signal, for partial and conditional writes
            auto old = [&](int s) -> SV {
                if (nba) {
                    SV v = base(s);
                    if (auto it = st.nba.find(s); it != st.nba.end())
                        for (size_t b = 0; b < v.b.size(); b++)
                            if (it->second.t[b]) v.b[b] = it->second.b[b];
                    return v;
                }
                return read(st, s);
            };
            // (signal, condition) targets
            std::vector<std::pair<int, int>> tg;
            if (i.arr) {
                if (isRel(i.arr->base)) unsup("an automatic array");
                std::vector<int> sel = m.elemSel(*i.arr, i.aidx, rd);
                for (int64_t n = 0; n < (int64_t)sel.size() && n < i.arr->count; n++)
                    if (sel[(size_t)n] != 0) tg.push_back({i.arr->base + (int)n, sel[(size_t)n]});
            } else
                tg.push_back({i.sig, 1});
            for (auto [s, c] : tg) {
                if (only && !only->count(s)) continue;
                int w = m.sigs[(size_t)s].w;
                Ent& en = entry(env, s);
                SV ov;
                bool haveOld = false;
                auto setBit = [&](int b, int v, int cond) {
                    if (b < 0 || b >= w || cond == 0) return;
                    if (cond != 1) {
                        if (!haveOld) { ov = old(s); haveOld = true; }
                        v = m.g.Mux(cond, v, ov.b[(size_t)b]);
                    }
                    en.b[(size_t)b] = v;
                    en.t[(size_t)b] = 1;
                };
                if (i.bidx) {
                    SV bi = m.ev(i.bidx, rd);
                    if (!i.pw) {
                        for (int b = 0; b < w; b++) setBit(b, val.w() ? val.b[0] : 0, m.g.And(c, m.o.eqConst(bi, (int64_t)b + i.lo)));
                    } else {
                        // the part's low bit l = (pdown ? idx - pw + 1 : idx) - lo
                        if (!haveOld) { ov = old(s); haveOld = true; }
                        std::vector<int> nb = ov.b;
                        for (int l = -(i.pw - 1); l < w; l++) {
                            int64_t idxv = (int64_t)l + i.lo + (i.pdown ? i.pw - 1 : 0);
                            int at = m.g.And(c, m.o.eqConst(bi, idxv));
                            if (at == 0) continue;
                            for (int j = 0; j < i.pw; j++) {
                                int b = l + j;
                                if (b < 0 || b >= w) continue;
                                nb[(size_t)b] = m.g.Mux(at, j < val.w() ? val.b[(size_t)j] : 0, nb[(size_t)b]);
                            }
                        }
                        for (int b = 0; b < w; b++) { en.b[(size_t)b] = nb[(size_t)b]; en.t[(size_t)b] = 1; }
                    }
                } else if (i.hi >= 0) {
                    for (int b = i.lo; b <= i.hi; b++) setBit(b, b - i.lo < val.w() ? val.b[(size_t)(b - i.lo)] : 0, c);
                } else {
                    for (int b = 0; b < w; b++) setBit(b, b < val.w() ? val.b[(size_t)b] : 0, c);
                }
            }
        }
        // Run p from `from` until `to` or a Ret; the merged state at the end.
        St run(const Prog& p, size_t from, size_t to, St st0) {
            if (++depth > 64) unsup("recursion");
            std::map<size_t, St> pend;
            St fin;
            fin.cond = 0;
            bool haveFin = false;
            auto finish = [&](St st) {
                if (!haveFin) { fin = std::move(st); haveFin = true; }
                else merge(fin, st);
            };
            put(pend, from, std::move(st0));
            while (!pend.empty()) {
                if (++steps > 2000000) unsup("a loop whose bounds are not constant");
                auto node = pend.extract(pend.begin());
                size_t pc = node.key();
                St st = std::move(node.mapped());
                if (pc >= to) { finish(std::move(st)); continue; }
                const Instr& i = p.code[pc];
                Reader rd = [&](int s) { return read(st, s); };
                switch (i.k) {
                    case IK::Assign: write(st, i, false); put(pend, pc + 1, std::move(st)); break;
                    case IK::Nba: write(st, i, true); put(pend, pc + 1, std::move(st)); break;
                    case IK::Jmp: put(pend, i.target, std::move(st)); break;
                    case IK::JmpIfNot: {
                        int c = m.truthOf(m.ev(i.e, rd));
                        if (c == 1) { put(pend, pc + 1, std::move(st)); break; }
                        if (c == 0) { put(pend, i.target, std::move(st)); break; }
                        St no = st;
                        no.cond = m.g.And(st.cond, c ^ 1);
                        st.cond = m.g.And(st.cond, c);
                        put(pend, pc + 1, std::move(st));
                        put(pend, i.target, std::move(no));
                        break;
                    }
                    case IK::Call: {
                        if (!i.callee) unsup("a call with no body");
                        if (i.lay || i.self || i.vslot >= 0 || !i.dinit.empty()) unsup("a call of an automatic subroutine or a method");
                        int c = st.cond;
                        st.cond = 1;
                        St r = run(*i.callee, 0, i.callee->code.size(), std::move(st));
                        r.cond = c;
                        put(pend, pc + 1, std::move(r));
                        break;
                    }
                    case IK::Assert: {
                        if (i.join == 4) unsup("an expect statement");
                        int c = m.truthOf(m.ev(i.e, rd));
                        if (!initMode) asserts.push_back({i.sig, m.g.And(st.cond, c ^ 1)});
                        if (i.join != 0) { put(pend, pc + 1, std::move(st)); break; }
                        St no = st;
                        no.cond = m.g.And(st.cond, c ^ 1);
                        st.cond = m.g.And(st.cond, c);
                        put(pend, pc + 1, std::move(st));
                        put(pend, i.target, std::move(no));
                        break;
                    }
                    case IK::Display: case IK::Strobe: case IK::Monitor: case IK::AssertCtl: case IK::Dump:
                        put(pend, pc + 1, std::move(st));
                        break;
                    case IK::Finish:
                        if (initMode) { finish(std::move(st)); pend.clear(); break; }
                        put(pend, pc + 1, std::move(st));
                        break;
                    case IK::Ret: finish(std::move(st)); break;
                    default:
                        if (initMode) { finish(std::move(st)); pend.clear(); break; }   // an initial block's stimulus: not part of the model
                        unsup(i.k == IK::WaitDelay ? "a delay" : i.k == IK::WaitEdge ? "an event control inside a process"
                                                                                     : "a statement of this kind (" + std::to_string((int)i.k) + ")");
                }
            }
            depth--;
            if (!haveFin) fin.cond = 0;
            return fin;
        }
    };

    static void collectWrites(const Prog& p, std::vector<int>& out, std::unordered_set<const Prog*>& seen) {
        if (!seen.insert(&p).second) return;
        for (auto& i : p.code) {
            if ((i.k == IK::Assign || i.k == IK::Nba) && i.sig >= 0 && !isRel(i.sig) && i.dyn < 0) {
                if (i.arr) {
                    if (!isRel(i.arr->base))
                        for (int k = 0; k < i.arr->count; k++) out.push_back(i.arr->base + k);
                } else
                    out.push_back(i.sig);
            }
            if (i.k == IK::Call && i.callee) collectWrites(*i.callee, out, seen);
            for (const Prog* b : i.branches) collectWrites(*b, out, seen);
        }
    }

    // ---- processes ----
    void classify() {
        size_t np = lowered.procs.size();
        pi.assign(np, {});
        writers.assign(sigs.size(), {});
        aliasOf.assign(sigs.size(), -1);
        for (size_t k = 0; k < np; k++) {
            auto& lp = lowered.procs[k];
            auto& code = lp.prog.code;
            ProcInfo& p = pi[k];
            size_t n = code.size();
            std::unordered_set<const Prog*> seen;
            collectWrites(lp.prog, p.writes, seen);
            std::sort(p.writes.begin(), p.writes.end());
            p.writes.erase(std::unique(p.writes.begin(), p.writes.end()), p.writes.end());
            if (lp.kind == "sampled" || lp.kind == "Final") { p.kind = P_SKIP; p.writes.clear(); continue; }
            if (lp.kind == "init" || lp.kind == "Initial") { p.kind = P_INIT; p.lo = 0; p.hi = n; continue; }
            auto allAny = [](const Instr& w) {
                if (w.k != IK::WaitEdge || w.dynWait) return false;
                if (w.events.size() > 1) {
                    for (auto& [s, e] : w.events) if (e != ANY) return false;
                    return true;
                }
                return w.edge == ANY;
            };
            auto edged = [](const Instr& w) {
                if (w.k != IK::WaitEdge || w.dynWait) return false;
                if (w.events.size() > 1) {
                    for (auto& [s, e] : w.events) if (e != ANY) return true;
                    return false;
                }
                return w.edge != ANY;
            };
            bool loopBack = n >= 2 && code[n - 1].k == IK::Jmp && code[n - 1].target == 0;
            if (n >= 3 && loopBack && allAny(code[n - 2])) { p.kind = P_COMB; p.lo = 0; p.hi = n - 2; }
            else if (n >= 2 && loopBack && allAny(code[0])) { p.kind = P_COMB; p.lo = 1; p.hi = n - 1; }
            else if (n >= 2 && loopBack && edged(code[0])) { p.kind = P_CLOCKED; p.lo = 1; p.hi = n - 1; }
            else if (!lp.loops && n >= 1 && lp.kind != "Always") {   // a constant continuous assign: runs once
                bool plain = true;
                for (auto& i : code) plain = plain && i.k == IK::Assign;
                if (plain) { p.kind = P_COMB; p.lo = 0; p.hi = n; }
                else { p.kind = P_CUT; p.why = "a process that is not combinational or clocked"; }
            } else { p.kind = P_CUT; p.why = "a process that is not combinational or clocked (delays or several event controls)"; }
            // inner waits make it something else
            if (p.kind == P_COMB || p.kind == P_CLOCKED)
                for (size_t q = p.lo; q < p.hi; q++)
                    if (code[q].k == IK::WaitEdge || code[q].k == IK::WaitDelay || code[q].k == IK::CycleWait || code[q].k == IK::Fork) {
                        p.kind = P_CUT;
                        p.why = "a process with a delay or an event control in its body";
                        break;
                    }
            // a whole-signal copy (a port connection): the same clock under another name
            if (p.kind == P_COMB && p.hi - p.lo == 1) {
                const Instr& a = code[p.lo];
                if (a.k == IK::Assign && a.hi < 0 && !a.arr && !a.bidx && a.e && a.e->op == Op::Ref && !a.e->at && !isRel(a.e->sig) && a.sig >= 0 &&
                    sigs[(size_t)a.sig].w == sigs[(size_t)a.e->sig].w)
                    aliasOf[(size_t)a.sig] = a.e->sig;
            }
        }
        for (size_t k = 0; k < np; k++) {
            if (pi[k].kind == P_INIT || pi[k].kind == P_SKIP) continue;
            for (int s : pi[k].writes) writers[(size_t)s].push_back((int)k);
        }
        for (size_t s = 0; s < sigs.size(); s++)
            if (writers[s].size() != 1) aliasOf[s] = -1;   // a copy only when it is the one driver
    }
    std::vector<std::pair<int, Edge>> eventsOf(const Instr& w) const {
        if (w.events.size() > 1) return w.events;
        return {{w.sig, w.edge}};
    }
    // The clock: named, or the signal most clocked processes and assertions wait on.
    void findClock() {
        if (!opt.clock.empty()) {
            clockSig = findSig(opt.clock);
            if (clockSig < 0) throw std::runtime_error("no signal named '" + opt.clock + "' for -clock");
            clockSig = canon(clockSig);
            return;
        }
        std::map<int, int> votes;
        for (size_t k = 0; k < pi.size(); k++)
            if (pi[k].kind == P_CLOCKED)
                for (auto& [s, e] : eventsOf(lowered.procs[k].prog.code[0]))
                    if (e == POS) votes[canon(s)] += 1;
        for (auto& c : concAsserts())
            for (auto& [s, e] : c.ev)
                if (e == POS) votes[canon(s)] += 2;
        // a reset also appears as an edge: prefer what the assertions use, then the most common
        int best = -1, bv = 0;
        for (auto& [s, v] : votes)
            if (v > bv) { best = s; bv = v; }
        clockSig = best;
    }
    int findSig(const std::string& name) const {
        for (size_t s = 0; s < sigs.size(); s++)
            if (sigs[s].name == name) return (int)s;
        // without the top's name, or with it
        for (size_t s = 0; s < sigs.size(); s++) {
            const std::string& n = sigs[s].name;
            auto dot = n.find('.');
            if (dot != std::string::npos && n.substr(dot + 1) == name) return (int)s;
        }
        return -1;
    }
    bool onClock(const std::vector<std::pair<int, Edge>>& ev) const {
        for (auto& [s, e] : ev)
            if (e == POS && canon(s) == clockSig) return true;
        return false;
    }
    void findReset() {
        if (opt.noReset) return;
        int rs = -1;
        bool low = opt.resetLow;
        if (!opt.reset.empty()) {
            rs = findSig(opt.reset);
            if (rs < 0) throw std::runtime_error("no signal named '" + opt.reset + "' for -reset");
        } else {
            static const char* lows[] = {"rst_n", "rstn", "rst_ni", "resetn", "reset_n", "arst_n", "aresetn", "nreset", "rst_b"};
            static const char* highs[] = {"rst", "reset", "arst", "rst_i", "areset"};
            for (size_t s = 0; s < sigs.size() && rs < 0; s++) {
                const std::string& n = sigs[s].name;
                if (std::count(n.begin(), n.end(), '.') > 1 || sigs[s].w != 1 || !writers[s].empty()) continue;
                std::string leaf = n.substr(n.find('.') == std::string::npos ? 0 : n.find('.') + 1);
                for (const char* c : lows) if (leaf == c) { rs = (int)s; low = true; }
                for (const char* c : highs) if (leaf == c) { rs = (int)s; low = false; }
            }
        }
        if (rs < 0) return;
        if (!writers[(size_t)rs].empty()) throw std::runtime_error("the reset '" + sigs[(size_t)rs].name + "' is driven inside the design; it must be an input");
        opt.reset = sigs[(size_t)rs].name;
        opt.resetLow = low;
        // The reset sequence is part of the model, so the model is the same in
        // every cycle: a chain of flops counts the reset cycles, the reset is
        // active until the last one is set, and assertions are checked after.
        int done = 1;
        for (int k = 0; k < opt.resetCycles; k++) {
            std::vector<int> l;
            newInputs(1, l, "$reset_done" + std::to_string(k), 1);
            StateBit sb;
            sb.in = l[0];
            sb.init = 0;
            sb.next = k == 0 ? 1 : done;
            stateBitOfInput[sb.in >> 1] = (int)stateBits.size();
            stateBits.push_back(sb);
            done = l[0];
        }
        SV v;
        v.b = {low ? done : done ^ 1};
        curMemo[(size_t)rs] = v;
        startLit = done;
        hasReset = true;
    }

    // ---- initial values: declaration initializers and initial blocks ----
    void runInits() {
        initVal.assign(sigs.size(), {});
        for (size_t k = 0; k < pi.size(); k++) {
            if (pi[k].kind != P_INIT) continue;
            Exec ex{*this};
            ex.initMode = true;
            ex.base = [&](int s) -> SV {
                auto& iv = initVal[(size_t)s];
                if (iv.empty()) unsup("reads a value with no initial value");
                SV v;
                for (int8_t b : iv) {
                    if (b < 0) unsup("reads a value with no initial value");
                    v.b.push_back(b);
                }
                return v;
            };
            try {
                St st = ex.run(lowered.procs[k].prog, pi[k].lo, pi[k].hi, St{});
                for (auto* env : {&st.blk, &st.nba})
                    for (auto& [s, en] : *env) {
                        auto& iv = initVal[(size_t)s];
                        if (iv.empty()) iv.assign(en.b.size(), -1);
                        for (size_t b = 0; b < en.b.size(); b++)
                            if (en.t[b]) iv[b] = en.b[b] == 0 ? 0 : en.b[b] == 1 ? 1 : -1;
                    }
            } catch (const Unsupported&) {
                // an initial block that is stimulus, not initialization
            }
        }
    }

    // ---- the value of a signal in the current cycle ----
    void cutProc(int p, const std::string& why) {
        pi[(size_t)p].kind = P_CUT;
        pi[(size_t)p].why = why;
    }
    bool runProc(int p) {
        ProcInfo& q = pi[(size_t)p];
        if (q.done) return true;
        if (q.kind != P_COMB && q.kind != P_CLOCKED) return false;
        if (q.kind == P_CLOCKED && !onClock(eventsOf(lowered.procs[(size_t)p].prog.code[0]))) {
            cutProc(p, "a process on another clock or edge than posedge " + (clockSig >= 0 ? sigs[(size_t)clockSig].name : std::string("?")));
            return false;
        }
        q.running = true;
        Exec ex{*this};
        ex.base = [this, p](int s) { return baseRead(p, s); };
        try {
            St st = ex.run(lowered.procs[(size_t)p].prog, q.lo, q.hi, St{});
            q.blk = std::move(st.blk);
            q.nba = std::move(st.nba);
            for (auto& a : ex.asserts) immediate.push_back(a);
            q.done = true;
        } catch (const Unsupported& u) {
            cutProc(p, u.what());
        }
        q.running = false;
        return q.done;
    }
    // A read, in process p, of a signal p has not assigned yet on this path.
    SV baseRead(int p, int s) {
        ProcInfo& q = pi[(size_t)p];
        if (q.kind == P_COMB && std::binary_search(q.writes.begin(), q.writes.end(), s)) {
            // its own output before it assigns it: a latch, or bits another process drives
            bool others = false;
            for (int w : writers[(size_t)s]) others = others || w != p;
            if (!others) return latchBase(s);
        }
        return cur(s);
    }
    std::unordered_map<int, SV> latchMemo;
    SV latchBase(int s) {
        if (auto it = latchMemo.find(s); it != latchMemo.end()) return it->second;
        SV v = freeSig(s, "$undriven");
        latchMemo[s] = v;
        return v;
    }
    // A comb process asked for one of its outputs while it is running: the
    // block computes independent outputs, so run only what that output needs.
    std::map<std::pair<int, int>, Ent> sliceMemo;
    const Ent* sliceOf(int p, int s) {
        auto key = std::make_pair(p, s);
        if (auto it = sliceMemo.find(key); it != sliceMemo.end()) return &it->second;
        if (sliceRunning[key]) unsup("a combinational loop through " + sigs[(size_t)s].name);
        sliceRunning[key] = true;
        ProcInfo& q = pi[(size_t)p];
        const Prog& prog = lowered.procs[(size_t)p].prog;
        std::unordered_set<int> need{s};
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t k = q.lo; k < q.hi; k++) {
                const Instr& i = prog.code[k];
                if (i.k == IK::Call) unsup("a combinational loop through a block that calls a subroutine");
                if (i.k != IK::Assign) continue;
                bool hit = i.arr ? false : need.count(i.sig) > 0;
                if (i.arr)
                    for (int e = 0; e < i.arr->count && !hit; e++) hit = need.count(i.arr->base + e) > 0;
                if (!hit) continue;
                std::vector<int> rs;
                collectRefs(i.e, rs);
                for (auto& a : i.aidx) collectRefs(a, rs);
                if (i.bidx) collectRefs(i.bidx, rs);
                for (int r : rs)
                    if (std::binary_search(q.writes.begin(), q.writes.end(), r) && need.insert(r).second) grew = true;
            }
        }
        Exec ex{*this};
        ex.only = &need;
        ex.base = [this, p, &need](int r) -> SV {
            ProcInfo& qq = pi[(size_t)p];
            if (std::binary_search(qq.writes.begin(), qq.writes.end(), r)) {
                if (need.count(r)) return latchBase(r);
                return cur(r);   // another output of the block: through its own slice
            }
            return cur(r);
        };
        St st = ex.run(prog, q.lo, q.hi, St{});
        sliceRunning[key] = false;
        Ent en;
        if (auto it = st.blk.find(s); it != st.blk.end()) en = it->second;
        else { en.b.assign((size_t)sigs[(size_t)s].w, 0); en.t.assign((size_t)sigs[(size_t)s].w, 0); }
        return &(sliceMemo[key] = std::move(en));
    }
    SV makeState(int s, const std::string& why = "") {
        (void)why;
        SV v;
        int first = (int)stateBits.size();
        newInputs(sigs[(size_t)s].w, v.b, sigs[(size_t)s].name, 1);
        for (int b = 0; b < sigs[(size_t)s].w; b++) {
            StateBit sb;
            sb.in = v.b[(size_t)b];
            auto& iv = initVal[(size_t)s];
            sb.init = b < (int)iv.size() ? iv[(size_t)b] : -1;
            sb.aux = stageOf.count(s) > 0;   // a $past stage belongs to the properties
            stateBitOfInput[sb.in >> 1] = (int)stateBits.size();
            stateBits.push_back(sb);
        }
        stateRange[(size_t)s] = {first, sigs[(size_t)s].w};
        isState[(size_t)s] = 1;
        stateSigs.push_back(s);
        return v;
    }
    SV cur(int s) {
        if (s < 0 || (size_t)s >= sigs.size()) unsup("a signal outside the design");
        if (curMemo[(size_t)s]) return *curMemo[(size_t)s];
        SV v = curNew(s);
        v.sgn = false;
        v.b.resize((size_t)sigs[(size_t)s].w, 0);
        curMemo[(size_t)s] = v;
        return v;
    }
    SV curNew(int s) {
        if (auto it = shadowSrc.find(s); it != shadowSrc.end()) return cur(it->second);
        if (clockSig >= 0 && canon(s) == clockSig) return o.konst(0, sigs[(size_t)s].w);   // sampled before its rising edge
        if (stageOf.count(s)) return makeState(s);
        auto& ws = writers[(size_t)s];
        if (ws.empty()) {
            bool hasInit = false;
            for (int8_t b : initVal[(size_t)s]) hasInit = hasInit || b >= 0;
            if (hasInit) return makeState(s);   // a variable with an initializer that nothing assigns: it holds
            return freeSig(s);
        }
        bool anyClocked = false, anyComb = false;
        for (int p : ws) {
            if (pi[(size_t)p].kind == P_CLOCKED) anyClocked = true;
            else if (pi[(size_t)p].kind == P_COMB) anyComb = true;
            else if (pi[(size_t)p].kind == P_CUT) {
                cutNotes.insert(sigs[(size_t)s].name + ": " + pi[(size_t)p].why);
                return freeSig(s);
            }
        }
        if (anyClocked && anyComb) {
            cutNotes.insert(sigs[(size_t)s].name + ": assigned by both clocked and combinational processes");
            return freeSig(s);
        }
        if (anyClocked) return makeState(s);
        // combinational: every driver's bits side by side
        int w = sigs[(size_t)s].w;
        SV v;
        v.b.assign((size_t)w, 0);
        std::vector<char> have((size_t)w, 0);
        for (int p : ws) {
            ProcInfo& q = pi[(size_t)p];
            const Ent* en = nullptr;
            if (q.running) {
                try {
                    en = sliceOf(p, s);
                } catch (const Unsupported& u) {
                    cutNotes.insert(sigs[(size_t)s].name + ": " + u.what());
                    return freeSig(s);
                }
            } else {
                if (!runProc(p)) {
                    cutNotes.insert(sigs[(size_t)s].name + ": " + pi[(size_t)p].why);
                    return freeSig(s);
                }
                auto it = q.blk.find(s);
                if (it == q.blk.end()) continue;
                en = &it->second;
            }
            for (int b = 0; b < w; b++)
                if (en->t[(size_t)b]) {
                    if (have[(size_t)b]) {
                        cutNotes.insert(sigs[(size_t)s].name + ": more than one driver");
                        return freeSig(s);
                    }
                    v.b[(size_t)b] = en->b[(size_t)b];
                    have[(size_t)b] = 1;
                }
        }
        bool missing = false;
        for (char h : have) missing = missing || !h;
        if (missing) {
            SV base = latchBase(s);
            for (int b = 0; b < w; b++)
                if (!have[(size_t)b]) v.b[(size_t)b] = base.b[(size_t)b];
        }
        return v;
    }

    // ---- next-state functions ----
    void closeStates() {
        for (size_t k = 0; k < stateSigs.size(); k++) {   // grows as cones pull in more state
            int s = stateSigs[k];
            int w = sigs[(size_t)s].w;
            auto [first, cnt] = stateRange[(size_t)s];
            SV nx = cur(s);
            if (auto it = stageOf.find(s); it != stageOf.end()) {
                auto& c = sampleClocks()[(size_t)it->second.clockIdx];
                auto& ln = c.lines[(size_t)it->second.line];
                Reader rd = [this](int r) { return cur(r); };
                try {
                    if (!onClock(c.ev)) unsup("a sampled value function on another clock");
                    SV src = it->second.k == 0 ? ev(ln.src, rd) : cur(ln.stages[(size_t)it->second.k - 1]);
                    src = o.ext(src, w, false);
                    int gate = c.gate ? truthOf(ev(c.gate, rd)) : 1;
                    nx = o.mux(gate, src, nx);
                } catch (const Unsupported& u) {
                    cutNotes.insert(sigs[(size_t)s].name + ": " + u.what());
                    nx = freeSig(s, "$next");
                }
            } else {
                bool cut = false;
                for (int p : writers[(size_t)s]) {
                    if (!runProc(p)) {
                        cutNotes.insert(sigs[(size_t)s].name + ": " + pi[(size_t)p].why);
                        cut = true;
                        break;
                    }
                }
                if (cut) nx = freeSig(s, "$next");
                else {
                    std::vector<char> have((size_t)w, 0);
                    for (int pass = 0; pass < 2 && !cut; pass++)   // blocking writes, then nonblocking over them
                        for (int p : writers[(size_t)s]) {
                            ProcInfo& q = pi[(size_t)p];
                            Env& env = pass ? q.nba : q.blk;
                            auto it = env.find(s);
                            if (it == env.end()) continue;
                            for (int b = 0; b < w; b++)
                                if (it->second.t[(size_t)b]) {
                                    if (have[(size_t)b] == pass + 1) cut = true;   // two processes assign the bit
                                    nx.b[(size_t)b] = it->second.b[(size_t)b];
                                    have[(size_t)b] = (char)(pass + 1);
                                }
                        }
                    if (cut) {
                        cutNotes.insert(sigs[(size_t)s].name + ": assigned by more than one clocked process");
                        nx = freeSig(s, "$next");
                    }
                }
            }
            for (int b = 0; b < cnt; b++) stateBits[(size_t)(first + b)].next = nx.b[(size_t)b];
        }
    }

    // ---- concurrent assertions as monitor automata ----
    static void atomsIn(const sva::Node* t, std::set<int>& out, std::unordered_set<const sva::Node*>& seen) {
        if (!seen.insert(t).second) return;
        if (t->atom >= 0) out.insert(t->atom);
        for (auto* k : t->kids) atomsIn(k, out, seen);
    }
    void buildMonitor(ConcAssert& c) {
        AssertInfo& a = asserts()[(size_t)c.id];
        if (a.hidden) return;
        Property pr;
        pr.name = a.name;
        pr.dir = a.dir;
        try {
            if (c.procedural) unsup("a concurrent assertion in a procedure");
            if (c.expect) unsup("an expect statement");
            if (!c.more.empty()) unsup("a multiclock assertion");
            if (!c.localSig.empty() || !c.acts.empty()) unsup("local variables or match items");
            if (!c.matchFlags.empty() || !c.clearFlags.empty() || c.endSig >= 0) unsup("a sequence end point (.triggered, .matched)");
            if (!onClock(c.ev)) unsup("a clock other than posedge " + (clockSig >= 0 ? sigs[(size_t)clockSig].name : std::string("?")));
            Reader rd = [this](int r) { return cur(r); };
            std::vector<int> atom;
            for (auto& e : c.atoms) atom.push_back(truthOf(ev(e, rd)));
            int tick = c.clkIff ? truthOf(ev(c.clkIff, rd)) : 1;
            int kill = c.disable ? truthOf(ev(c.disable, rd)) : 0;

            // The terms an attempt can become, with whether it is nonvacuous so far.
            struct MS { const sva::Node* t; bool nonvac; };
            std::vector<MS> states{{c.root, false}};
            std::map<std::pair<const sva::Node*, bool>, int> idx{{{c.root, false}, 0}};
            // per state: the atoms it reads and, per valuation, where it goes
            // (-1 fail, -2 pass nonvacuously, -3 ends without a verdict that counts) and whether a cover sequence matched
            struct Tr { std::vector<int> atoms; std::vector<int> to; std::vector<char> hit; };
            std::vector<Tr> trs;
            std::vector<char> val(c.atoms.size(), 0);
            for (size_t si = 0; si < states.size(); si++) {
                if (states.size() > 4000) unsup("a property with more than 4000 automaton states");
                MS ms = states[si];
                Tr tr;
                std::set<int> as;
                std::unordered_set<const sva::Node*> seen;
                atomsIn(ms.t, as, seen);
                tr.atoms.assign(as.begin(), as.end());
                if (tr.atoms.size() > 14) unsup("a property step that reads more than 14 boolean expressions");
                size_t nv = (size_t)1 << tr.atoms.size();
                tr.to.resize(nv);
                tr.hit.resize(nv);
                for (size_t mv = 0; mv < nv; mv++) {
                    for (size_t b = 0; b < tr.atoms.size(); b++) val[(size_t)tr.atoms[b]] = (mv >> b) & 1;
                    sva::Deriv dv{sva::store(), val};
                    dv.nonvac = ms.nonvac;
                    const sva::Node* t = dv.d(ms.t);
                    tr.hit[mv] = dv.matches > 0;
                    int to;
                    if (t->k == sva::PFALSE) to = -1;
                    else if (t->k == sva::PTRUE) to = dv.nonvac ? -2 : -3;
                    else if (t->k == sva::DONE || (c.coverSeq && t->k == sva::NUL)) to = -3;
                    else {
                        auto [it, fresh] = idx.try_emplace({t, dv.nonvac}, (int)states.size());
                        if (fresh) states.push_back({t, dv.nonvac});
                        to = it->second;
                    }
                    tr.to[mv] = to;
                }
                trs.push_back(std::move(tr));
            }
            // one latch per state: an attempt is at that term
            size_t ns = states.size();
            std::vector<int> latch(ns), act(ns);
            std::vector<int> firstBit(ns);
            for (size_t i = 0; i < ns; i++) {
                std::vector<int> l;
                newInputs(1, l, pr.name + "$state" + std::to_string(i), 1);
                latch[i] = l[0];
                StateBit sb;
                sb.in = l[0];
                sb.init = 0;
                sb.aux = true;
                stateBitOfInput[sb.in >> 1] = (int)stateBits.size();
                firstBit[i] = (int)stateBits.size();
                stateBits.push_back(sb);
                act[i] = i == 0 ? g.Or(latch[i], startLit) : latch[i];
            }
            // guard(i, pred): the literal "state i's step goes somewhere pred accepts"
            auto guard = [&](size_t i, const std::function<bool(size_t)>& pred) {
                const Tr& tr = trs[i];
                std::function<int(int, size_t)> tree = [&](int level, size_t mv) -> int {
                    if (level < 0) return pred(mv) ? 1 : 0;
                    int hi = tree(level - 1, mv | ((size_t)1 << level)), lo = tree(level - 1, mv);
                    return g.Mux(atom[(size_t)tr.atoms[(size_t)level]], hi, lo);
                };
                return tree((int)tr.atoms.size() - 1, 0);
            };
            std::vector<int> nextAct(ns, 0);
            int fail = 0, pass = 0, hit = 0;
            for (size_t i = 0; i < ns; i++) {
                const Tr& tr = trs[i];
                std::set<int> targets(tr.to.begin(), tr.to.end());
                for (int to : targets) {
                    int gd = g.And(act[i], guard(i, [&](size_t mv) { return tr.to[mv] == to; }));
                    if (to == -1) fail = g.Or(fail, gd);
                    else if (to == -2) pass = g.Or(pass, gd);
                    else if (to >= 0) nextAct[(size_t)to] = g.Or(nextAct[(size_t)to], gd);
                }
                bool anyHit = false;
                for (char h : tr.hit) anyHit = anyHit || h;
                if (anyHit) hit = g.Or(hit, g.And(act[i], guard(i, [&](size_t mv) { return tr.hit[mv] != 0; })));
            }
            int live = g.And(tick, kill ^ 1);
            for (size_t i = 0; i < ns; i++)
                stateBits[(size_t)firstBit[i]].next = g.And(kill ^ 1, g.Mux(tick, nextAct[i], latch[i]));
            if (pr.dir == AD_COVER) pr.bad = g.And(live, c.coverSeq ? hit : pass);
            else pr.bad = g.And(live, fail);
            // Liveness. A property is a conjunction of obligations: the
            // conjuncts of the terms its attempts are at. One step turns an
            // obligation into the conjuncts of its derivative. The property
            // fails without a failing step when one chain of obligations goes
            // on forever, each of them held open by a strong unbounded
            // operator (s_eventually, s_until, a strong unbounded sequence)
            // that nothing weak can satisfy in its place. addLiveness() looks
            // for a loop of the design with such a chain all the way round,
            // back to the obligation it began with.
            // Where a weak unbounded alternative could satisfy the term
            // instead (s_eventually always q), or under `not` / accept_on,
            // this does not decide the property: liveInexact.
            struct Ob { const sva::Node* t; std::vector<int> atoms; std::vector<std::vector<int>> to; };
            std::vector<Ob> obs;
            std::map<const sva::Node*, int> obIdx;
            std::string inexact;
            std::function<bool(const sva::Node*)> seqUnbounded = [&](const sva::Node* n) {
                if (n->k == sva::REP && n->hi < 0) return true;
                for (auto* k : n->kids)
                    if (seqUnbounded(k)) return true;
                return false;
            };
            std::function<bool(const sva::Node*)> unboundedIn = [&](const sva::Node* n) {
                if ((n->k == sva::REP || n->k == sva::ALWAYS || n->k == sva::EVENT) && n->hi < 0) return true;
                if (n->k == sva::UNTIL) return true;
                for (auto* k : n->kids)
                    if (unboundedIn(k)) return true;
                return false;
            };
            // strong unbounded: nothing but its own fulfilment ends it
            auto isSU = [&](const sva::Node* n) {
                bool strong = n->flags & sva::STRONG;
                return (n->k == sva::EVENT && strong && n->hi < 0) || (n->k == sva::UNTIL && strong) ||
                       (n->k == sva::PSEQ && strong && seqUnbounded(n->kids[0]));
            };
            // The value of a term if nothing more happened: strong unbounded
            // operators fail, weak unbounded ones hold, and what ends by itself
            // in bounded time counts as failing (it cannot keep a term true forever).
            std::function<bool(const sva::Node*)> holds = [&](const sva::Node* n) -> bool {
                switch (n->k) {
                    case sva::PTRUE: case sva::DONE: case sva::COVSEQ: return true;
                    case sva::PFALSE: return false;
                    case sva::PSEQ: return !(n->flags & sva::STRONG) && seqUnbounded(n->kids[0]);
                    case sva::NEXT: return false;
                    case sva::EVENT: return !(n->flags & sva::STRONG) && n->hi < 0;
                    case sva::ALWAYS: return n->hi < 0;
                    case sva::UNTIL: return !(n->flags & sva::STRONG);
                    case sva::IMPL: return true;
                    case sva::IF: return holds(n->kids[0]) && (n->kids.size() < 2 || holds(n->kids[1]));
                    case sva::PNOT:   // of something that ends by itself: so does the negation
                        if (!unboundedIn(n->kids[0])) return false;
                        inexact = "the negation of an unbounded property";
                        return true;
                    case sva::ACCEPT: case sva::REJECT: inexact = "accept_on or reject_on"; return holds(n->kids[0]);
                    case sva::ENV: case sva::INIT: return holds(n->kids[0]);
                    case sva::PAND: { bool r = true; for (auto* k : n->kids) r = holds(k) && r; return r; }
                    case sva::POR: { bool r = false; for (auto* k : n->kids) r = holds(k) || r; return r; }
                    default: return true;
                }
            };
            std::function<bool(const sva::Node*)> exposed = [&](const sva::Node* n) -> bool {
                if (isSU(n)) return true;
                if (n->k != sva::PAND && n->k != sva::POR) return false;
                for (auto* k : n->kids)
                    if (exposed(k)) return true;
                return false;
            };
            auto conjuncts = [](const sva::Node* t) {
                return t->k == sva::PAND ? t->kids : std::vector<const sva::Node*>{t};
            };
            // an open obligation's index, or -1 when it is not one
            auto obOf = [&](const sva::Node* t) -> int {
                if (t->k == sva::PTRUE || t->k == sva::PFALSE || t->k == sva::DONE) return -1;
                if (holds(t)) {
                    if (exposed(t)) inexact = "a strong operator next to a weak unbounded alternative";
                    return -1;
                }
                auto [it, fresh] = obIdx.try_emplace(t, (int)obs.size());
                if (fresh) obs.push_back({t, {}, {}});
                return it->second;
            };
            std::vector<std::vector<int>> stateObs(ns);
            bool wantLive = pr.dir == AD_ASSERT || pr.dir == AD_ASSUME;
            if (wantLive) {
                for (size_t i = 0; i < ns; i++)
                    for (auto* cj : conjuncts(states[i].t)) {
                        int o = obOf(cj);
                        if (o >= 0) stateObs[i].push_back(o);
                    }
                for (size_t oi = 0; oi < obs.size() && inexact.empty(); oi++) {
                    if (obs.size() > 2000) { inexact = "more than 2000 open obligations"; break; }
                    std::set<int> as;
                    std::unordered_set<const sva::Node*> seen;
                    atomsIn(obs[oi].t, as, seen);
                    std::vector<int> atomsOf(as.begin(), as.end());
                    if (atomsOf.size() > 14) { inexact = "a step that reads more than 14 boolean expressions"; break; }
                    size_t nv = (size_t)1 << atomsOf.size();
                    std::vector<std::vector<int>> to(nv);
                    for (size_t mv = 0; mv < nv; mv++) {
                        for (size_t b = 0; b < atomsOf.size(); b++) val[(size_t)atomsOf[b]] = (mv >> b) & 1;
                        sva::Deriv dv{sva::store(), val};
                        const sva::Node* d = dv.d(obs[oi].t);
                        for (auto* cj : conjuncts(d)) {
                            int o = obOf(cj);
                            if (o >= 0) to[mv].push_back(o);
                        }
                    }
                    obs[oi].atoms = std::move(atomsOf);
                    obs[oi].to = std::move(to);
                }
                // an obligation matters only if a chain through it can go on forever
                bool anySU = false;
                for (auto& o : obs) anySU = anySU || exposed(o.t);
                if (!inexact.empty()) {
                    if (pr.dir == AD_ASSERT) pr.liveInexact = inexact;
                    else pr.fairInexact = inexact;
                } else if (anySU) {
                    size_t no = obs.size();
                    auto obGuard = [&](size_t o, const std::function<bool(size_t)>& pred) {
                        const Ob& ob = obs[o];
                        std::function<int(int, size_t)> tree = [&](int level, size_t mv) -> int {
                            if (level < 0) return pred(mv) ? 1 : 0;
                            int hi = tree(level - 1, mv | ((size_t)1 << level)), lo = tree(level - 1, mv);
                            return g.Mux(atom[(size_t)ob.atoms[(size_t)level]], hi, lo);
                        };
                        return tree((int)ob.atoms.size() - 1, 0);
                    };
                    auto newLatch = [&](const std::string& name, int8_t init) {
                        std::vector<int> l;
                        newInputs(1, l, pr.name + name, 1);
                        StateBit sb;
                        sb.in = l[0];
                        sb.init = init;
                        sb.aux = true;
                        stateBitOfInput[sb.in >> 1] = (int)stateBits.size();
                        stateBits.push_back(sb);
                        return (int)stateBits.size() - 1;
                    };
                    // open now: a conjunct of a term some attempt is at
                    std::vector<int> open(no, 0);
                    for (size_t i = 0; i < ns; i++)
                        for (int o : stateObs[i]) open[(size_t)o] = g.Or(open[(size_t)o], act[i]);
                    // `mark`: a free input, the cycle the chains are followed from
                    std::vector<int> mk;
                    newInputs(1, mk, pr.name + "$mark", 0);
                    pr.liveMark = mk[0];
                    // which obligations to follow: one, picked by free inputs (assert: one
                    // failing chain is enough), or all of them (assume: none may go on)
                    std::vector<int> pick(no, 1);
                    if (pr.dir == AD_ASSERT) {
                        int bits = 0;
                        while (((size_t)1 << bits) < no) bits++;
                        std::vector<int> ix;
                        if (bits) newInputs(bits, ix, pr.name + "$pick", 0);
                        for (size_t o = 0; o < no; o++)
                            for (int b = 0; b < bits; b++) pick[o] = g.And(pick[o], ((o >> b) & 1) ? ix[(size_t)b] : (ix[(size_t)b] ^ 1));
                    }
                    std::vector<int> B(no), P(no), bBit(no), pBit(no), curB(no), nxtB(no, 0);
                    for (size_t o = 0; o < no; o++) {
                        bBit[o] = newLatch("$open" + std::to_string(o), 0);
                        pr.liveOwn.push_back(bBit[o]);
                        B[o] = stateBits[(size_t)bBit[o]].in;
                        int first = g.And(g.And(pick[o], open[o]), startLit);
                        curB[o] = g.Mux(pr.liveMark, first, B[o]);
                        if (pr.dir == AD_ASSERT) {
                            pBit[o] = newLatch("$first" + std::to_string(o), 0);
                            pr.liveOwn.push_back(pBit[o]);
                            P[o] = stateBits[(size_t)pBit[o]].in;
                            stateBits[(size_t)pBit[o]].next = g.Mux(pr.liveMark, first, P[o]);
                            pr.liveBack = g.Or(pr.liveBack, g.And(P[o], B[o]));
                        } else
                            pr.liveBack = g.Or(pr.liveBack, B[o]);
                    }
                    for (size_t o = 0; o < no; o++) {
                        std::set<int> targets;
                        for (auto& v : obs[o].to) targets.insert(v.begin(), v.end());
                        for (int to : targets)
                            nxtB[(size_t)to] = g.Or(nxtB[(size_t)to], g.And(curB[o], obGuard(o, [&](size_t mv) {
                                auto& v = obs[o].to[mv];
                                return std::find(v.begin(), v.end(), to) != v.end();
                            })));
                    }
                    for (size_t o = 0; o < no; o++) stateBits[(size_t)bBit[o]].next = g.And(kill ^ 1, g.Mux(tick, nxtB[o], curB[o]));
                    for (size_t i = 0; i < ns; i++) pr.liveState.push_back(latch[i]);
                    if (pr.dir == AD_ASSERT) pr.liveness = true;
                }
            }
        } catch (const Unsupported& u) {
            pr.skipped = u.what();
        }
        props.push_back(std::move(pr));
    }

    // ---- connectivity (-connect FILE)
    // One connection per line: "destination, source [, condition]". A name is a
    // signal (with or without the top's name) and may select bits: sig[3],
    // sig[7:4]. A condition is terms joined by &&, each term [!]name or
    // name == number / name != number. '#' starts a comment.
    struct ConnParser {
        Model& m;
        std::string t;
        size_t p = 0;
        void ws() { while (p < t.size() && std::isspace((unsigned char)t[p])) p++; }
        bool eat(const char* w) {
            ws();
            size_t n = std::strlen(w);
            if (t.compare(p, n, w) != 0) return false;
            p += n;
            return true;
        }
        [[noreturn]] void fail(const std::string& why) { throw std::runtime_error(why + " in '" + t + "'"); }
        long num() {
            ws();
            size_t b = p;
            while (p < t.size() && std::isdigit((unsigned char)t[p])) p++;
            if (b == p) fail("a number is expected");
            return std::stol(t.substr(b, p - b));
        }
        // 12, 'h1f, 8'b1010_0000
        uint64_t literal() {
            ws();
            size_t b = p;
            while (p < t.size() && std::isdigit((unsigned char)t[p])) p++;
            if (p < t.size() && t[p] == '\'') {
                p++;
                if (p < t.size() && (t[p] == 's' || t[p] == 'S')) p++;
                int base = 0;
                char c = p < t.size() ? (char)std::tolower((unsigned char)t[p]) : ' ';
                base = c == 'h' ? 16 : c == 'd' ? 10 : c == 'b' ? 2 : c == 'o' ? 8 : 0;
                if (!base) fail("a number base (h, d, b, o) is expected");
                p++;
                uint64_t v = 0;
                size_t d0 = p;
                for (; p < t.size() && (std::isxdigit((unsigned char)t[p]) || t[p] == '_'); p++) {
                    if (t[p] == '_') continue;
                    int d = std::isdigit((unsigned char)t[p]) ? t[p] - '0' : std::tolower((unsigned char)t[p]) - 'a' + 10;
                    if (d >= base) fail("a digit outside the base");
                    v = v * (uint64_t)base + (uint64_t)d;
                }
                if (d0 == p) fail("digits are expected");
                return v;
            }
            if (b == p) fail("a number is expected");
            return std::stoull(t.substr(b, p - b));
        }
        SV ref() {
            ws();
            size_t b = p;
            while (p < t.size() && (std::isalnum((unsigned char)t[p]) || t[p] == '_' || t[p] == '.' || t[p] == '$')) p++;
            // an index that is part of the name: an array element or a generate block (a.b[2].c, mem[3])
            std::string name = t.substr(b, p - b);
            if (name.empty()) fail("a signal name is expected");
            auto find = [&](const std::string& n) {
                int sg = m.findSig(n);
                if (sg < 0 && !m.top.empty() && n.compare(0, m.top.size() + 1, m.top + ".") == 0) sg = m.findSig(n.substr(m.top.size() + 1));
                return sg;
            };
            int sg = find(name);
            while (p < t.size() && t[p] == '[') {
                size_t close = t.find(']', p);
                if (close == std::string::npos) fail("a ']' is missing");
                size_t q = close + 1, e = q;
                while (e < t.size() && (std::isalnum((unsigned char)t[e]) || t[e] == '_' || t[e] == '.' || t[e] == '$')) e++;
                std::string longer = name + t.substr(p, e - p);
                bool more = e < t.size() && t[e] == '[';
                if (find(longer) >= 0 || (sg < 0 && (e > q || more))) {
                    name = longer;
                    p = e;
                    sg = find(name);
                    continue;
                }
                break;
            }
            if (sg < 0) fail("no signal named '" + name + "'");
            SV v = m.cur(sg);
            if (eat("[")) {
                long hi = num(), lo = hi;
                if (eat(":")) lo = num();
                if (!eat("]")) fail("a ']' is missing");
                if (lo > hi) std::swap(lo, hi);
                if (hi >= v.w()) fail("a bit select outside '" + name + "'");
                SV r;
                for (long i = lo; i <= hi; i++) r.b.push_back(v.b[(size_t)i]);
                return r;
            }
            return v;
        }
        int cond() {
            int c = 1;
            do {
                bool neg = eat("!");
                SV v = ref();
                int term;
                if (eat("==") || eat("!=")) {
                    bool ne = t[p - 2] == '!';
                    uint64_t k = literal();
                    term = m.o.eq(v, m.o.konst(k, v.w())) ^ (ne ? 1 : 0);
                } else {
                    term = m.o.redOr(v);
                }
                c = m.g.And(c, term ^ (neg ? 1 : 0));
            } while (eat("&&"));
            return c;
        }
        void end() {
            ws();
            if (p != t.size()) fail("unexpected text");
        }
    };
    void addConnectivity(const std::string& file) {
        std::ifstream in(file);
        if (!in) throw std::runtime_error("cannot read " + file);
        std::string line;
        for (int ln = 1; std::getline(in, line); ln++) {
            if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            std::vector<std::string> f;
            std::stringstream ss(line);
            for (std::string x; std::getline(ss, x, ',');) f.push_back(x);
            std::string where = file + ":" + std::to_string(ln) + ": ";
            if (f.size() < 2 || f.size() > 3) throw std::runtime_error(where + "expected 'destination, source [, condition]'");
            try {
                ConnParser pd{*this, f[0]}, ps{*this, f[1]};
                SV d = pd.ref(), sr = ps.ref();
                pd.end();
                ps.end();
                if (d.w() != sr.w())
                    throw std::runtime_error("destination is " + std::to_string(d.w()) + " bits, source is " + std::to_string(sr.w()));
                int c = 1;
                if (f.size() == 3) {
                    ConnParser pc{*this, f[2]};
                    c = pc.cond();
                    pc.end();
                }
                auto trim = [](std::string x) {
                    x.erase(0, x.find_first_not_of(" \t\r"));
                    x.erase(x.find_last_not_of(" \t\r") + 1);
                    return x;
                };
                Property pr;
                pr.name = "connect." + std::to_string(ln) + "." + trim(f[0]);
                pr.dir = AD_ASSERT;
                pr.file = file;
                pr.line = ln;
                pr.bad = g.And(startLit, g.And(c, o.eq(d, sr) ^ 1));
                props.push_back(pr);
                if (f.size() == 3) {   // a connection whose condition never holds is checked vacuously
                    Property en;
                    en.name = pr.name + ".enabled";
                    en.dir = AD_COVER;
                    en.app = 2;
                    en.file = file;
                    en.line = ln;
                    en.bad = g.And(startLit, c);
                    props.push_back(en);
                }
            } catch (const Unsupported& u) {
                throw std::runtime_error(where + u.what());
            } catch (const std::runtime_error& e) {
                throw std::runtime_error(where + e.what());
            }
        }
    }

    // -app toggle (unreachable coverage): two covers per signal bit, "is 0" and
    // "is 1" in some cycle after the reset. An unreachable one is a bit that is
    // stuck, so toggle coverage on it can never close in simulation.
    void addToggleTargets() {
        std::set<int> skip;
        for (auto& [n, sg] : topInputs) skip.insert(canon(sg));
        if (clockSig >= 0) skip.insert(clockSig);
        std::set<int> seen;
        for (size_t s = 0; s < sigs.size(); s++) {
            const std::string& n = sigs[s].name;
            if (n.empty() || n[0] == '$' || n.find('$') != std::string::npos) continue;
            int c = canon((int)s);
            if (skip.count(c) || procLocals.count(n) || !seen.insert(c).second) continue;
            if (sigs[s].w > opt.toggleMax) {
                if (!opt.quiet) std::cerr << "mformal: -app toggle: " << n << " is wider than " << opt.toggleMax << " bits, not checked (-toggle-max)\n";
                continue;
            }
            SV v;
            try { v = cur((int)s); } catch (const Unsupported&) { continue; }
            for (int b = 0; b < v.w(); b++)
                for (int val = 0; val < 2; val++) {
                    Property pr;
                    pr.name = (top.empty() ? "" : top + ".") + n + (v.w() > 1 ? "[" + std::to_string(b) + "]" : "") + (val ? "==1" : "==0");
                    pr.dir = AD_COVER;
                    pr.app = 1;
                    pr.bad = g.And(startLit, v.b[(size_t)b] ^ (val ? 0 : 1));
                    props.push_back(std::move(pr));
                }
        }
    }

    void build() {
        curMemo.assign(sigs.size(), std::nullopt);
        isState.assign(sigs.size(), 0);
        stateRange.assign(sigs.size(), {-1, 0});
        for (auto [src, sh] : shadowPairs()) shadowSrc[sh] = src;
        for (size_t ci = 0; ci < sampleClocks().size(); ci++)
            for (size_t li = 0; li < sampleClocks()[ci].lines.size(); li++)
                for (size_t k = 0; k < sampleClocks()[ci].lines[li].stages.size(); k++)
                    stageOf[sampleClocks()[ci].lines[li].stages[k]] = {(int)ci, (int)li, (int)k};
        classify();
        findClock();
        runInits();
        findReset();
        for (auto& c : concAsserts()) buildMonitor(c);
        // immediate assertions: run every process that has one
        for (size_t k = 0; k < pi.size(); k++) {
            if (pi[k].kind != P_COMB && pi[k].kind != P_CLOCKED) continue;
            bool has = false;
            std::function<void(const Prog&, int)> scan = [&](const Prog& p, int d) {
                for (auto& i : p.code) {
                    if (i.k == IK::Assert) has = true;
                    if (i.k == IK::Call && i.callee && d < 8) scan(*i.callee, d + 1);
                }
            };
            scan(lowered.procs[k].prog, 0);
            if (has) runProc((int)k);
        }
        std::map<int, int> imm;   // one property per assertion, whichever paths reach it
        for (auto [id, bad] : immediate) {
            auto [it, fresh] = imm.try_emplace(id, 0);
            it->second = g.Or(it->second, bad);
        }
        for (auto [id, bad] : imm) {
            AssertInfo& a = asserts()[(size_t)id];
            if (a.hidden) continue;
            Property pr;
            pr.name = a.name;
            pr.dir = a.dir;
            pr.bad = g.And(startLit, bad);
            if (a.dir == AD_COVER) pr.skipped = "an immediate cover (use cover property)";
            props.push_back(std::move(pr));
        }
        // every named signal, for the traces (cones are cut out per property later)
        for (size_t s = 0; s < sigs.size(); s++) {
            if (sigs[s].name.empty() || sigs[s].name[0] == '$') continue;
            try { cur((int)s); } catch (const Unsupported&) {}
        }
        if (opt.appToggle) addToggleTargets();
        for (auto& f : opt.connect) addConnectivity(f);
        closeStates();
        std::sort(props.begin(), props.end(), [](const Property& a, const Property& b) { return a.name < b.name; });
    }
};

// ------------------------------------------------------- checking one property
enum Verdict { V_PROVEN, V_FALSIFIED, V_COVERED, V_UNREACHABLE, V_BOUNDED, V_INCONCLUSIVE, V_SKIPPED, V_ASSUMED };

struct Result {
    Verdict v = V_BOUNDED;
    int depth = -1;          // the cycle of the failure / hit (0 = first after reset), or the induction depth
    int bound = 0;           // cycles checked without a counterexample
    std::string note, trace;
    int coneState = 0, coneAnd = 0;
    double seconds = 0;
    // the counterexample: per frame, the value of every AIG input
    std::vector<std::vector<char>> inputs;
    int failFrame = -1;
};

// The cone of influence of a set of literals, through the next-state functions.
struct Cone {
    std::vector<int> nodes;          // AIG nodes, ascending (children first)
    std::vector<int> stateBits;      // indices into Model::stateBits
};

Cone coneOf(const Model& m, const std::vector<int>& roots) {
    Cone c;
    std::vector<char> seen(m.g.n.size(), 0);
    std::vector<int> stack;
    auto push = [&](int lit) {
        int n = lit >> 1;
        if (n && !seen[(size_t)n]) { seen[(size_t)n] = 1; stack.push_back(n); }
    };
    for (int r : roots) push(r);
    while (!stack.empty()) {
        int n = stack.back();
        stack.pop_back();
        c.nodes.push_back(n);
        auto nd = m.g.n[(size_t)n];
        if (nd[0] < 0) {
            if (auto it = m.stateBitOfInput.find(n); it != m.stateBitOfInput.end()) {
                c.stateBits.push_back(it->second);
                push(m.stateBits[(size_t)it->second].next);
            }
        } else {
            push(nd[0]);
            push(nd[1]);
        }
    }
    std::sort(c.nodes.begin(), c.nodes.end());
    std::sort(c.stateBits.begin(), c.stateBits.end());
    return c;
}

// Liveness to safety. For each assertion with a strong obligation: a free
// input saves the state once, and the assertion fails when the same state
// comes back with the obligation picked at that moment open again, down a
// chain of open obligations (a loop the design can go round forever without
// meeting it). The loop must be fair: of an assumption's obligations open
// when the state was saved, none is still open when the loop closes. (A fair
// loop that needs more rounds to close them all is found as the longer loop.)
// The saved copy covers the state bits in the cone of the monitors and the
// assumptions.
void addLiveness(Model& m) {
    std::vector<int> constraints;
    for (auto& p : m.props)
        if (p.dir == AD_ASSUME && p.skipped.empty()) constraints.push_back(p.bad);
    for (auto& pr : m.props) {
        if (!pr.liveness || !pr.skipped.empty()) continue;
        std::vector<int> roots{pr.liveBack};
        roots.insert(roots.end(), pr.liveState.begin(), pr.liveState.end());
        roots.insert(roots.end(), constraints.begin(), constraints.end());
        for (auto& f : m.props)
            if (f.dir == AD_ASSUME && f.liveMark && f.skipped.empty()) roots.insert(roots.end(), f.liveState.begin(), f.liveState.end());
        Cone cone = coneOf(m, roots);
        Aig& g = m.g;
        auto latch = [&](const std::string& name, int8_t init) {
            std::vector<int> l;
            m.newInputs(1, l, pr.name + name, 1);
            StateBit sb;
            sb.in = l[0];
            sb.init = init;
            sb.aux = true;
            m.stateBitOfInput[sb.in >> 1] = (int)m.stateBits.size();
            m.stateBits.push_back(sb);
            return (int)m.stateBits.size() - 1;
        };
        int savedB = latch("$saved", 0);
        int saved = m.stateBits[(size_t)savedB].in;
        // the mark is the save: once, after reset
        int saveNow = pr.liveMark;
        int okB = latch("$marks", 1);
        int ok = m.stateBits[(size_t)okB].in;
        int marksOk = g.And(g.Or(saveNow ^ 1, g.And(m.startLit, saved ^ 1)), 1);
        // every fair assumption is marked in the same cycle, and never again
        for (auto& f : m.props)
            if (f.dir == AD_ASSUME && f.liveMark && f.skipped.empty()) marksOk = g.And(marksOk, g.Xor(f.liveMark, saveNow) ^ 1);
        m.stateBits[(size_t)okB].next = g.And(ok, marksOk);
        std::set<int> own(pr.liveOwn.begin(), pr.liveOwn.end());   // the chain's own latches change round the loop
        int eq = 1;
        for (int b : cone.stateBits) {
            if (b == savedB || b == okB || own.count(b)) continue;
            int cur = m.stateBits[(size_t)b].in;
            int sh = latch("$loop" + std::to_string(b), -1);
            int shl = m.stateBits[(size_t)sh].in;
            m.stateBits[(size_t)sh].next = g.Mux(saveNow, cur, shl);
            eq = g.And(eq, g.Xor(cur, shl) ^ 1);
        }
        m.stateBits[(size_t)savedB].next = g.Or(saved, saveNow);
        pr.live = g.And(g.And(saved, g.And(ok, marksOk)), g.And(eq, pr.liveBack));
        for (auto& f : m.props)
            if (f.dir == AD_ASSUME && f.liveMark && f.skipped.empty()) pr.live = g.And(pr.live, f.liveBack ^ 1);
        pr.saved = saved;
        pr.bad = g.Or(pr.bad, pr.live);
    }
}

// The lemmas that say something about a property's own state: those whose
// cone lies inside the property's cone. The others would only make the
// problem larger.
// The model as an AIGER 1.9 file (binary for .aig, ASCII otherwise), so that
// other model checkers can run on it: one bad-state property per assertion
// (reachable = falsified) and per cover (reachable = covered), in the order
// of the report, and one invariant constraint per assumption. Liveness is
// already folded into the bad states (see addLiveness), and the reset
// sequence is part of the latches.
bool writeAiger(const Model& m, const std::vector<size_t>& todo, const std::vector<int>& constraints, const std::string& path) {
    std::vector<int> roots = constraints;
    std::vector<size_t> bads;
    for (size_t i : todo) {
        const Property& p = m.props[i];
        if (p.dir == AD_ASSUME || !p.skipped.empty()) continue;
        bads.push_back(i);
        roots.push_back(p.bad);
    }
    Cone cone = coneOf(m, roots);
    // AIGER order: inputs, latches, ANDs (children first: cone.nodes ascends).
    std::vector<unsigned> var(m.g.n.size(), 0);
    std::vector<int> ins, ands;
    for (int n : cone.nodes) {
        if (m.g.n[(size_t)n][0] >= 0) ands.push_back(n);
        else if (!m.stateBitOfInput.count(n)) ins.push_back(n);
    }
    unsigned v = 0;
    for (int n : ins) var[(size_t)n] = ++v;
    for (int sb : cone.stateBits) var[(size_t)(m.stateBits[(size_t)sb].in >> 1)] = ++v;
    for (int n : ands) var[(size_t)n] = ++v;
    auto lit = [&](int l) -> unsigned { return l < 0 ? 0u : 2 * var[(size_t)(l >> 1)] + (unsigned)(l & 1); };
    bool binary = path.size() > 4 && path.compare(path.size() - 4, 4, ".aig") == 0;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << (binary ? "aig " : "aag ") << v << " " << ins.size() << " " << cone.stateBits.size() << " 0 " << ands.size() << " " << bads.size() << " "
      << constraints.size() << "\n";
    if (!binary)
        for (int n : ins) f << 2 * var[(size_t)n] << "\n";
    for (int sbi : cone.stateBits) {
        const StateBit& sb = m.stateBits[(size_t)sbi];
        unsigned self = lit(sb.in);
        if (!binary) f << self << " ";
        f << lit(sb.next);
        if (sb.init == 1) f << " 1";
        else if (sb.init < 0) f << " " << self;   // not initialized
        f << "\n";
    }
    for (size_t i : bads) f << lit(m.props[i].bad) << "\n";
    for (int c : constraints) f << (lit(c) ^ 1u) << "\n";
    for (int n : ands) {
        auto nd = m.g.n[(size_t)n];
        unsigned lhs = 2 * var[(size_t)n], a = lit(nd[0]), b = lit(nd[1]);
        if (a < b) std::swap(a, b);
        if (!binary) {
            f << lhs << " " << a << " " << b << "\n";
            continue;
        }
        for (unsigned d : {lhs - a, a - b}) {
            while (d & ~0x7fu) {
                f.put((char)((d & 0x7f) | 0x80));
                d >>= 7;
            }
            f.put((char)d);
        }
    }
    for (size_t k = 0; k < ins.size(); k++) f << "i" << k << " " << m.inputName[(size_t)m.g.n[(size_t)ins[k]][1]] << "\n";
    for (size_t k = 0; k < cone.stateBits.size(); k++)
        f << "l" << k << " " << m.inputName[(size_t)m.g.n[(size_t)(m.stateBits[(size_t)cone.stateBits[k]].in >> 1)][1]] << "\n";
    for (size_t k = 0; k < bads.size(); k++) f << "b" << k << " " << m.props[bads[k]].name << "\n";
    size_t k = 0;
    for (auto& p : m.props)
        if (p.dir == AD_ASSUME && p.skipped.empty()) f << "c" << k++ << " " << p.name << "\n";
    f << "c\nmformal " << m.topModule << "\n";
    for (size_t j = 0; j < bads.size(); j++) f << "b" << j << " " << (m.props[bads[j]].dir == AD_COVER ? "cover " : "assert ") << m.props[bads[j]].name << "\n";
    return (bool)f;
}


std::vector<int> usefulLemmas(const Model& m, int bad, const std::vector<int>& constraints, const std::vector<int>& lemmas) {
    std::vector<int> roots{bad};
    roots.insert(roots.end(), constraints.begin(), constraints.end());
    Cone own = coneOf(m, roots);
    std::vector<int> out;
    for (int l : lemmas) {
        if (l == bad) continue;
        Cone c = coneOf(m, {l});
        std::vector<int> design;   // the lemma's own monitor always comes with it
        for (int b : c.stateBits)
            if (!m.stateBits[(size_t)b].aux) design.push_back(b);
        if (std::includes(own.stateBits.begin(), own.stateBits.end(), design.begin(), design.end())) out.push_back(l);
    }
    return out;
}

// One unrolling of a cone in a SAT solver.
struct Unroll {
    const Model& m;
    const Cone& c;
    bool fromReset;   // frame 0 is the initial state, with the reset sequence; else any state, after reset
    sat::Solver s;   // the solver every tool shares (CaDiCaL behind src/sat.hpp)
    std::vector<int> assumed;
    int T;
    std::vector<std::vector<int>> map;   // frame -> cone position -> SAT literal
    std::vector<int> posOf;              // AIG node -> cone position, or -1

    Unroll(const Model& m_, const Cone& c_, bool fromReset_, const std::vector<int>& posOf_) : m(m_), c(c_), fromReset(fromReset_), posOf(posOf_) {
        T = s.newVar();
        s.addClause({T});
    }
    int newVar() { return s.newVar(); }
    void assume(int l) { assumed.push_back(l); }
    // 10: satisfiable, 20: unsatisfiable, 0: the conflict limit was reached
    int solve(long long conflicts) {
        sat::Result r = s.solve(assumed, conflicts > 0 ? conflicts : -1);
        assumed.clear();
        return r == sat::Sat ? 10 : r == sat::Unsat ? 20 : 0;
    }
    int frames() const { return (int)map.size(); }
    int lit(int f, int a) const {
        int n = a >> 1;
        int l = n == 0 ? -T : map[(size_t)f][(size_t)posOf[(size_t)n]];
        return (a & 1) ? -l : l;
    }
    void unit(int l) { s.addClause({l}); }
    void addFrame() {
        int f = frames();
        map.emplace_back(c.nodes.size(), 0);
        auto& mp = map.back();
        for (size_t p = 0; p < c.nodes.size(); p++) {
            int n = c.nodes[p];
            auto nd = m.g.n[(size_t)n];
            if (nd[0] < 0) {
                auto it = m.stateBitOfInput.find(n);
                if (it == m.stateBitOfInput.end()) {
                    mp[p] = newVar();
                } else {
                    const StateBit& sb = m.stateBits[(size_t)it->second];
                    if (f == 0) {
                        int v = newVar();
                        mp[p] = v;
                        if (fromReset && sb.init >= 0) unit(sb.init ? v : -v);
                    } else
                        mp[p] = lit(f - 1, sb.next);
                }
            } else {
                int a = lit(f, nd[0]), b = lit(f, nd[1]);
                int v = newVar();
                mp[p] = v;
                s.addClause({-v, a});
                s.addClause({-v, b});
                s.addClause({v, -a, -b});
            }
        }
    }
    bool val(int l) { return l > 0 ? s.value(l) : !s.value(-l); }
};

// With `lemmas` (the fail literals of assertions already proven): the bounded
// check has been done, so only the induction runs, with every lemma holding
// in every frame.
Result checkProperty(const Model& m, const Property& pr, const std::vector<int>& constraints, const std::vector<int>* lemmas = nullptr, int frames = -1,
                     bool lastFrameOnly = false, int stepLimit = -1) {
    auto t0 = std::chrono::steady_clock::now();
    Result r;
    std::vector<int> roots{pr.bad};
    roots.insert(roots.end(), constraints.begin(), constraints.end());
    std::vector<int> lem;
    if (lemmas) {
        lem = usefulLemmas(m, pr.bad, constraints, *lemmas);
        if (lem.empty()) return r;   // nothing new to try
        roots.insert(roots.end(), lem.begin(), lem.end());
    }
    Cone cone = coneOf(m, roots);
    r.coneState = (int)cone.stateBits.size();
    for (int n : cone.nodes) r.coneAnd += m.g.n[(size_t)n][0] >= 0;
    std::vector<int> posOf(m.g.n.size(), -1);
    for (size_t p = 0; p < cone.nodes.size(); p++) posOf[(size_t)cone.nodes[p]] = (int)p;

    bool cover = pr.dir == AD_COVER;
    auto done = [&](Verdict v) {
        r.v = v;
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return r;
    };
    if (pr.bad == 0) {   // never fails / never hit, by construction
        r.note = "trivially";
        return done(cover ? V_UNREACHABLE : V_PROVEN);
    }
    Unroll bmc(m, cone, true, posOf), step(m, cone, false, posOf);
    int rc = m.hasReset ? m.opt.resetCycles : 0;
    bool stepAlive = !m.opt.bmcOnly;
    int distinctUpTo = 0;   // simple-path constraints cover every pair of step frames below this
    // frames > 0: a bounded check of exactly that many frames (PDR found a failure that deep)
    if (frames > 0) stepAlive = false;
    for (int k = 0; k < (frames > 0 ? frames : rc + m.opt.depth); k++) {
        if (!lemmas) bmc.addFrame();
        if (!lemmas)
            for (int cst : constraints) bmc.unit(-bmc.lit(k, cst));
        int res = 20;
        // lastFrameOnly: PDR found a failure exactly frames - 1 steps deep, so the earlier frames need no call
        if (!lemmas && !(lastFrameOnly && k < frames - 1)) {
            bmc.assume(bmc.lit(k, pr.bad));
            res = bmc.solve(m.opt.conflicts);
        }
        if (res == 10) {
            r.depth = k - rc;
            r.failFrame = k;
            r.inputs.assign((size_t)k + 1, std::vector<char>((size_t)m.g.nIn, 0));
            for (int f = 0; f <= k; f++)
                for (size_t p = 0; p < cone.nodes.size(); p++) {
                    int n = cone.nodes[p];
                    auto nd = m.g.n[(size_t)n];
                    if (nd[0] >= 0) continue;
                    if (f > 0 && m.stateBitOfInput.count(n)) continue;
                    r.inputs[(size_t)f][(size_t)nd[1]] = bmc.val(bmc.map[(size_t)f][p]);
                }
            return done(cover ? V_COVERED : V_FALSIFIED);
        }
        if (res != 20) {
            r.note = "the solver's conflict limit was reached at cycle " + std::to_string(k - rc);
            r.bound = k - rc;
            return done(V_INCONCLUSIVE);
        }
        if (!lemmas) bmc.unit(-bmc.lit(k, pr.bad));
        r.bound = std::max(0, k - rc + 1);
        if (!stepAlive) continue;
        if (stepLimit >= 0 && k >= stepLimit) { stepAlive = false; continue; }
        // k-induction: no failure in j consecutive cycles from any state, then none in the next
        int j = k;
        step.addFrame();
        for (int cst : constraints) step.unit(-step.lit(j, cst));
        if (lemmas)
            for (int l : lem) step.unit(-step.lit(j, l));
        if (j > 0) step.unit(-step.lit(j - 1, pr.bad));
        for (;;) {
            step.assume(step.lit(j, pr.bad));
            int sr = step.solve(m.opt.conflicts);
            if (sr == 20) {
                r.depth = j;
                return done(cover ? V_UNREACHABLE : V_PROVEN);
            }
            if (sr != 10) { stepAlive = false; break; }
            // simple path: a counterexample to induction that visits a state twice is not
            // one. The constraints are added the first time such a path shows up.
            bool added = false;
            if (!cone.stateBits.empty()) {
                std::vector<std::vector<char>> sv((size_t)j + 1);
                for (int f = 0; f <= j; f++)
                    for (int sbi : cone.stateBits) sv[(size_t)f].push_back(step.val(step.lit(f, m.stateBits[(size_t)sbi].in)));
                bool repeat = false;
                for (int a = 0; a <= j && !repeat; a++)
                    for (int b = a + 1; b <= j && !repeat; b++) repeat = sv[(size_t)a] == sv[(size_t)b];
                if (repeat)
                    for (int a = 0; a <= j; a++)
                        for (int b = a + 1; b <= j; b++) {
                            if (b < distinctUpTo) continue;   // this pair is already constrained
                            std::vector<int> diff;
                            for (int sbi : cone.stateBits) {
                                int x = step.lit(a, m.stateBits[(size_t)sbi].in), y = step.lit(b, m.stateBits[(size_t)sbi].in);
                                if (x == y) continue;
                                if (x == -y) { diff.clear(); diff.push_back(step.T); break; }
                                int d = step.newVar();
                                step.s.addClause({-d, x, y});
                                step.s.addClause({-d, -x, -y});
                                diff.push_back(d);
                            }
                            step.s.addClause(diff);
                            added = true;
                        }
                if (added) distinctUpTo = j + 1;
            }
            if (!added) break;
        }
    }
    return done(V_BOUNDED);
}

// ------------------------------------------------------------- PDR
// Property-directed reachability (IC3), for what k-induction cannot prove:
// it finds the invariant itself, as clauses over the state bits of the cone.
// F[0] is the initial states; F[i] over-approximates the states reachable in
// at most i steps and excludes every state that reaches a failure. A cube is
// a list of state literals: +(n+1) is cone state bit n true, -(n+1) false.
struct Pdr {
    const Model& m;
    const Cone& cone;
    int bad;
    const std::vector<int>& never;   // literals that never hold: assumptions' and lemmas' fail literals
    const std::vector<int>& posOf;
    using Cube = std::vector<int>;
    std::vector<std::unique_ptr<Unroll>> F;
    std::vector<std::vector<Cube>> blocked;   // per level: the cubes blocked up to exactly that level
    long long budget;                          // solver calls left
    int cexSteps = -1;
    int invariantClauses = 0;

    Pdr(const Model& m_, const Cone& c_, int bad_, const std::vector<int>& never_, const std::vector<int>& posOf_, long long budget_)
        : m(m_), cone(c_), bad(bad_), never(never_), posOf(posOf_), budget(budget_) {}
    size_t n() const { return cone.stateBits.size(); }
    const StateBit& sb(int lit) const { return m.stateBits[(size_t)cone.stateBits[(size_t)(std::abs(lit) - 1)]]; }
    int at(Unroll& u, int f, int lit) const {
        int l = u.lit(f, sb(lit).in);
        return lit > 0 ? l : -l;
    }
    bool meetsInit(const Cube& c) const {
        for (int l : c) {
            int8_t iv = sb(l).init;
            if (iv >= 0 && (iv == 1) != (l > 0)) return false;
        }
        return true;
    }
    void newFrame() {
        auto u = std::make_unique<Unroll>(m, cone, F.empty(), posOf);
        u->addFrame();
        u->addFrame();
        for (int l : never) u->unit(-u->lit(0, l));
        F.push_back(std::move(u));
        blocked.emplace_back();
    }
    int solve(Unroll& u) {
        if (--budget < 0) throw Unsupported("budget");
        int r = u.solve(m.opt.conflicts);
        if (r != 10 && r != 20) throw Unsupported("conflicts");
        return r;
    }
    Cube stateOf(Unroll& u) {
        Cube c;
        for (size_t i = 0; i < n(); i++) c.push_back(u.val(u.lit(0, m.stateBits[(size_t)cone.stateBits[i]].in)) ? (int)i + 1 : -((int)i + 1));
        return c;
    }
    // Lifting: the state the solver found, with the inputs it chose, reaches
    // the target (a cube in the next state, or the failure now). Most state
    // bits do not matter for that; the ones that do are the reason
    // "state and inputs and not target" has no solution.
    std::unique_ptr<Unroll> lifter;
    std::vector<size_t> inputPos;
    Cube lift(Unroll& u, const Cube* target) {
        Cube full = stateOf(u);
        if (!lifter) {
            lifter = std::make_unique<Unroll>(m, cone, false, posOf);
            lifter->addFrame();
            lifter->addFrame();
            for (size_t p = 0; p < cone.nodes.size(); p++) {
                int nd = cone.nodes[p];
                if (m.g.n[(size_t)nd][0] < 0 && !m.stateBitOfInput.count(nd)) inputPos.push_back(p);
            }
        }
        Unroll& L = *lifter;
        int act = L.newVar();
        std::vector<int> cl{-act};
        if (target)
            for (int l : *target) cl.push_back(-at(L, 1, l));
        else
            cl.push_back(-L.lit(0, bad));
        for (int l : never) cl.push_back(L.lit(0, l));
        L.s.addClause(cl);
        L.assume(act);
        for (size_t p : inputPos) L.assume(u.val(u.map[0][p]) ? L.map[0][p] : -L.map[0][p]);
        for (int l : full) L.assume(at(L, 0, l));
        int r = L.solve(m.opt.conflicts);
        Cube c;
        if (r == 20) {
            for (int l : full)
                if (L.s.failed(at(L, 0, l))) c.push_back(l);
        } else
            c = full;
        L.unit(-act);
        return c;
    }
    void addClause(Unroll& u, const Cube& c) {
        std::vector<int> cl;
        for (int l : c) cl.push_back(-at(u, 0, l));
        u.s.addClause(cl);
    }
    // Is c unreachable in one step from F[level] minus c itself? If so, `core`
    // is a part of c that already is; if not, `pred` is a state that reaches it.
    bool inductive(const Cube& c, int level, bool minusSelf, Cube* core, Cube* pred) {
        Unroll& u = *F[(size_t)level];
        int act = 0;
        if (minusSelf) {
            act = u.newVar();
            std::vector<int> cl{-act};
            for (int l : c) cl.push_back(-at(u, 0, l));
            u.s.addClause(cl);
            u.assume(act);
        }
        for (int l : c) u.assume(at(u, 1, l));
        int r = solve(u);
        bool unsat = r == 20;
        if (unsat && core) {
            core->clear();
            for (int l : c)
                if (u.s.failed(at(u, 1, l))) core->push_back(l);
        }
        if (!unsat && pred) *pred = lift(u, &c);
        if (act) u.unit(-act);
        return unsat;
    }
    // A cube must exclude the initial states: put back a literal that does.
    void keepOffInit(Cube& g, const Cube& from) {
        if (!meetsInit(g)) return;
        for (int l : from) {
            int8_t iv = sb(l).init;
            if (iv >= 0 && (iv == 1) != (l > 0)) { g.push_back(l); std::sort(g.begin(), g.end(), [](int a, int b) { return std::abs(a) < std::abs(b); }); return; }
        }
    }
    void generalize(Cube& g, int level) {
        int fails = 0;
        for (size_t i = 0; i < g.size() && g.size() > 1 && fails < 8;) {
            Cube t = g;
            t.erase(t.begin() + (ptrdiff_t)i);
            Cube core;
            if (!meetsInit(t) && inductive(t, level - 1, true, &core, nullptr)) {
                keepOffInit(core, t);
                g = core;
                fails = 0;
            } else {
                i++;
                fails++;
            }
        }
    }
    void block(const Cube& g, int level) {
        for (int j = 1; j <= level; j++) addClause(*F[(size_t)j], g);
        blocked[(size_t)level].push_back(g);
    }
    // false: the failure is reachable from an initial state.
    bool blockAll(Cube s, int k) {
        struct Ob { int level; long seq; int dist; Cube c; };
        auto later = [](const Ob& a, const Ob& b) { return a.level != b.level ? a.level > b.level : a.seq > b.seq; };
        std::priority_queue<Ob, std::vector<Ob>, decltype(later)> q(later);
        long seq = 0;
        q.push({k, seq++, 0, std::move(s)});
        while (!q.empty()) {
            Ob ob = q.top();
            q.pop();
            if (ob.level == 0 || meetsInit(ob.c)) { cexSteps = ob.dist; return false; }
            Cube core, pred;
            if (inductive(ob.c, ob.level - 1, true, &core, &pred)) {
                keepOffInit(core, ob.c);
                generalize(core, ob.level);
                int lv = ob.level;
                while (lv < k && inductive(core, lv, true, nullptr, nullptr)) lv++;   // as far forward as it holds
                block(core, lv);
                if (lv < k) q.push({lv + 1, seq++, ob.dist, std::move(ob.c)});
            } else {
                q.push({ob.level - 1, seq++, ob.dist + 1, std::move(pred)});
                q.push(std::move(ob));
            }
        }
        return true;
    }
    // 1: proven. 0: a counterexample of cexSteps transitions exists. -1: gave up.
    int run(int maxFrames) {
        try {
            newFrame();
            {   // a failure in an initial state
                Unroll& u = *F[0];
                u.assume(u.lit(0, bad));
                if (solve(u) == 10) { cexSteps = 0; return 0; }
            }
            newFrame();
            for (int k = 1; k < maxFrames; k++) {
                for (;;) {
                    Unroll& u = *F[(size_t)k];
                    u.assume(u.lit(0, bad));
                    if (solve(u) == 20) break;
                    if (!blockAll(lift(u, nullptr), k)) return 0;
                }
                newFrame();
                for (int i = 1; i <= k; i++) {
                    std::vector<Cube> keep;
                    for (auto& c : blocked[(size_t)i]) {
                        if (inductive(c, i, false, nullptr, nullptr)) {
                            addClause(*F[(size_t)i + 1], c);
                            blocked[(size_t)i + 1].push_back(c);
                        } else
                            keep.push_back(c);
                    }
                    blocked[(size_t)i].swap(keep);
                    if (blocked[(size_t)i].empty()) {   // F[i] = F[i+1]: an inductive invariant
                        for (size_t j = (size_t)i + 1; j < blocked.size(); j++) invariantClauses += (int)blocked[j].size();
                        return 1;
                    }
                }
            }
        } catch (const Unsupported&) {
        }
        return -1;
    }
};

// PDR on one property. `never`: the assumptions' and proven lemmas' fail literals.
Result checkPdr(const Model& m, const Property& pr, const std::vector<int>& constraints, const std::vector<int>& lemmas, long long calls) {
    auto t0 = std::chrono::steady_clock::now();
    Result r;
    std::vector<int> never = constraints;
    for (int l : usefulLemmas(m, pr.bad, constraints, lemmas)) never.push_back(l);
    std::vector<int> roots{pr.bad};
    roots.insert(roots.end(), never.begin(), never.end());
    Cone cone = coneOf(m, roots);
    std::vector<int> posOf(m.g.n.size(), -1);
    for (size_t p = 0; p < cone.nodes.size(); p++) posOf[(size_t)cone.nodes[p]] = (int)p;
    Pdr pdr(m, cone, pr.bad, never, posOf, calls);
    int res = pdr.run(m.opt.pdrFrames);
    r.v = V_BOUNDED;
    if (res == 1) {
        r.v = pr.dir == AD_COVER ? V_UNREACHABLE : V_PROVEN;
        r.note = "pdr, " + std::to_string(pdr.invariantClauses) + (pdr.invariantClauses == 1 ? " clause" : " clauses");
    } else if (res == 0)
        r.depth = pdr.cexSteps;   // the caller gets the trace from a bounded check this deep
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

// ------------------------------------------------------------- traces
// Replay a counterexample on the AIG: the value of every node in every frame.
std::vector<std::vector<char>> replay(const Model& m, const Result& r) {
    size_t nf = r.inputs.size();
    std::vector<std::vector<char>> val(nf, std::vector<char>(m.g.n.size(), 0));
    for (size_t f = 0; f < nf; f++) {
        auto& v = val[f];
        for (size_t n = 1; n < m.g.n.size(); n++) {
            auto nd = m.g.n[n];
            if (nd[0] < 0) {
                auto it = m.stateBitOfInput.find((int)n);
                if (it != m.stateBitOfInput.end()) {
                    const StateBit& sb = m.stateBits[(size_t)it->second];
                    if (f == 0) v[n] = sb.init >= 0 ? sb.init : r.inputs[0][(size_t)nd[1]];
                    else v[n] = sb.next < 0 ? 0 : (val[f - 1][(size_t)(sb.next >> 1)] ^ (sb.next & 1));
                } else v[n] = r.inputs[f][(size_t)nd[1]];
            } else
                v[n] = (v[(size_t)(nd[0] >> 1)] ^ (nd[0] & 1)) & (v[(size_t)(nd[1] >> 1)] ^ (nd[1] & 1));
        }
    }
    return val;
}

bool writeVcd(const Model& m, const Result& r, const Property& pr, const std::string& path) {
    auto val = replay(m, r);
    std::ofstream out(path);
    if (!out) return false;
    out << "$comment mformal " << (pr.dir == AD_COVER ? "cover trace" : "counterexample") << " for " << pr.name << " $end\n";
    out << "$timescale 1ns $end\n";
    // scopes from the hierarchical names
    struct V { std::string leaf; int sig; std::string id; };
    std::map<std::vector<std::string>, std::vector<V>> scopes;
    auto idOf = [](int n) {
        std::string s;
        do { s += (char)(33 + n % 94); n /= 94; } while (n > 0);
        return s;
    };
    int nid = 0;
    for (size_t s = 0; s < m.sigs.size(); s++) {
        const std::string& name = m.sigs[s].name;
        if (name.empty() || name.find('$') != std::string::npos || !m.curMemo[s]) continue;
        std::vector<std::string> path2;
        if (!m.top.empty()) path2.push_back(m.top);
        size_t st = 0;
        int depth = 0;
        for (size_t i = 0; i < name.size(); i++) {
            if (name[i] == '[') depth++;
            if (name[i] == ']') depth--;
            if (name[i] == '.' && depth == 0) { path2.push_back(name.substr(st, i - st)); st = i + 1; }
        }
        std::string leaf = name.substr(st);
        scopes[path2].push_back({leaf, (int)s, idOf(nid++)});
    }
    std::vector<std::string> open;
    std::vector<V> all;
    for (auto& [sc, vars] : scopes) {
        size_t common = 0;
        while (common < open.size() && common < sc.size() && open[common] == sc[common]) common++;
        while (open.size() > common) { out << "$upscope $end\n"; open.pop_back(); }
        while (open.size() < sc.size()) { out << "$scope module " << sc[open.size()] << " $end\n"; open.push_back(sc[open.size()]); }
        for (auto& v : vars) {
            int w = m.sigs[(size_t)v.sig].w;
            std::string leaf = v.leaf;
            for (char& ch : leaf) if (ch == ' ') ch = '_';
            out << "$var wire " << w << " " << v.id << " " << leaf;
            if (w > 1 && leaf.find('[') == std::string::npos) out << " [" << w - 1 << ":0]";
            out << " $end\n";
            all.push_back(v);
        }
    }
    while (!open.empty()) { out << "$upscope $end\n"; open.pop_back(); }
    out << "$enddefinitions $end\n";
    std::vector<std::string> last(all.size());
    bool clkShown = m.clockSig >= 0;
    for (size_t f = 0; f < val.size(); f++) {
        out << "#" << f * 10 << "\n";
        for (size_t i = 0; i < all.size(); i++) {
            const SV& sv = *m.curMemo[(size_t)all[i].sig];
            std::string bits;
            for (int b = sv.w() - 1; b >= 0; b--) {
                int l = sv.b[(size_t)b];
                bits += (val[f][(size_t)(l >> 1)] ^ (l & 1)) ? '1' : '0';
            }
            if (clkShown && m.canon(all[i].sig) == m.clockSig) bits = "0";
            if (bits == last[i]) continue;
            last[i] = bits;
            if (bits.size() == 1) out << bits << all[i].id << "\n";
            else out << "b" << bits << " " << all[i].id << "\n";
        }
        out << "#" << f * 10 + 5 << "\n";
        for (size_t i = 0; i < all.size(); i++)
            if (clkShown && m.canon(all[i].sig) == m.clockSig) { out << "1" << all[i].id << "\n"; last[i] = "1"; }
    }
    out << "#" << val.size() * 10 << "\n";
    return true;
}

// A testbench that drives the trace into the design, so a simulator shows
// the same failure: inputs change while the clock is low, flops with no
// reset value are deposited at time 0.
bool writeTb(const Model& m, const Result& r, const Property& pr, const std::string& path, std::string& why) {
    if (m.clockSig < 0) { why = "there is no clock"; return false; }
    std::string clk;
    for (auto& [n, s] : m.topInputs)
        if (m.canon(s) == m.clockSig) clk = n;
    if (clk.empty()) { why = "the clock is not an input of the top module"; return false; }
    auto val = replay(m, r);
    auto hex = [&](int sig, size_t f) {
        const SV& sv = *m.curMemo[(size_t)sig];
        std::string bits;
        for (int b = sv.w() - 1; b >= 0; b--) {
            int l = sv.b[(size_t)b];
            bits += (val[f][(size_t)(l >> 1)] ^ (l & 1)) ? '1' : '0';
        }
        return std::to_string(sv.w()) + "'b" + bits;
    };
    std::ofstream out(path);
    if (!out) { why = "cannot write " + path; return false; }
    out << "// mformal: replay of the " << (pr.dir == AD_COVER ? "cover trace" : "counterexample") << " for " << pr.name << "\n"
        << "// " << (pr.dir == AD_COVER ? "hit" : "fails") << " at the rising edge at time " << r.failFrame * 10 + 5 << " ns (cycle " << r.depth << " after reset)\n"
        << "//   msim <design files> " << path.substr(path.find_last_of('/') + 1) << " -top mformal_replay --relax-enum-conversions\n"
        << "`timescale 1ns/1ns\nmodule mformal_replay;\n";
    for (auto& [n, s] : m.topInputs) {
        int w = m.sigs[(size_t)s].w;
        out << "  logic " << (w > 1 ? "[" + std::to_string(w - 1) + ":0] " : "") << n << ";\n";
    }
    out << "  " << m.topModule << " " << m.top << " (";
    bool first = true;
    for (auto& [n, s] : m.topInputs) { out << (first ? "" : ", ") << "." << n << "(" << n << ")"; first = false; }
    out << ");\n  initial begin\n    " << clk << " = 0;\n";
    for (int s : m.stateSigs) {
        const std::string& n = m.sigs[(size_t)s].name;
        if (n.find('$') != std::string::npos || !m.curMemo[(size_t)s]) continue;
        bool free = false;
        auto [firstBit, cnt] = m.stateRange[(size_t)s];
        for (int b = 0; b < cnt; b++) free = free || m.stateBits[(size_t)(firstBit + b)].init < 0;
        bool flop = false;   // not a loop variable or another temporary of a clocked block
        for (int p : m.writers[(size_t)s]) flop = flop || m.pi[(size_t)p].nba.count(s) > 0;
        // msim does not take a hierarchical assignment to an array element yet: left as a comment
        if (free && flop) out << (n.find('[') != std::string::npos ? "    // " : "    ") << m.top << "." << n << " = " << hex(s, 0) << ";\n";
    }
    for (size_t f = 0; f < val.size(); f++) {
        out << "    // time " << f * 10 << "\n   ";
        for (auto& [n, s] : m.topInputs)
            if (n != clk && m.curMemo[(size_t)s]) out << " " << n << " = " << hex(s, f) << ";";
        out << "\n    #5 " << clk << " = 1; #5 " << clk << " = 0;\n";
    }
    out << "    $finish;\n  end\nendmodule\n";
    return true;
}

std::string fileSafe(std::string s) {
    for (char& c : s)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) c = '_';
    return s;
}
std::string jsonStr(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if (c == '\n') r += "\\n";
        else r += c;
    }
    return r + "\"";
}

void usage() {
    std::cout << "usage: mformal [options] file.sv ...\n"
                 "  formal property checking on the msim front end: proves or falsifies\n"
                 "  assert, assume and cover (concurrent and immediate) for every input sequence\n\n"
                 "  -top M            the top module\n"
                 "  -clock SIG        the clock (default: the one the assertions and flops use)\n"
                 "  -reset SIG        the reset input, active high; -reset_n SIG: active low\n"
                 "                    (default: an input named rst_n, resetn, rst, reset, ...)\n"
                 "  -no-reset         start from the declared initial values only\n"
                 "  -reset-cycles N   cycles the reset is held (default 1)\n"
                 "  -depth N          cycles to check after reset (default 20)\n"
                 "  -bmc              bounded check only: no proof\n"
                 "  -no-pdr           prove by k-induction only\n"
                 "  -prop NAME        check only this property (repeatable; a name or its suffix)\n"
                 "  -threads N        worker threads (default: every logical core; the result does not depend on N)\n"
                 "  -conflicts N      SAT conflict limit per solver call (default 2000000; 0: none)\n"
                 "  -o DIR            where the traces go (default mformal_out); -no-trace: write none\n"
                 "  -tb               next to each trace, a testbench that replays it in msim (<property>_tb.sv)\n"
                 "  -json FILE        the results as JSON\n"
                 "  -app toggle       unreachable coverage: report every signal bit that can never be 0\n"
                 "                    or never be 1 after the reset (-toggle-max N: skip wider signals;\n"
                 "                    -toggle-deep: the whole -depth and PDR budget per target)\n"
                 "  -connect FILE     connectivity check: each line 'destination, source [, condition]' is\n"
                 "                    proven as: whenever the condition holds, destination == source\n"
                 "  -aiger FILE       write the model as AIGER 1.9 (.aig binary, else ASCII) for another\n"
                 "                    model checker, and stop without checking\n"
                 "  -Werror           exit non-zero also for unreachable covers and inconclusive results\n"
                 "  -quiet            the result lines only         -v  what was cut and why\n"
                 "  -f <file>  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
                 "  -version          print the version             --help-slang  every front-end option\n\n"
                 "exit status: 0 nothing falsified, 1 an assertion was falsified, 2 usage or front-end error\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> fwd{argv[0]};
    Options opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mformal: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        auto num = [&](const char* what) -> long long {
            std::string v = next(what);
            char* end = nullptr;
            long long n = std::strtoll(v.c_str(), &end, 10);
            if (!end || *end || n < 0) { std::cerr << "mformal: " << a << " needs " << what << ", not '" << v << "'\n"; std::exit(2); }
            return n;
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-version" || a == "--version") {
            std::cout << "mformal " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-clock") { opt.clock = next("a signal name"); continue; }
        if (a == "-reset") { opt.reset = next("a signal name"); opt.resetLow = false; continue; }
        if (a == "-reset_n") { opt.reset = next("a signal name"); opt.resetLow = true; continue; }
        if (a == "-no-reset") { opt.noReset = true; continue; }
        if (a == "-reset-cycles") { opt.resetCycles = (int)std::max<long long>(1, num("a number of cycles")); continue; }
        if (a == "-depth") { opt.depth = (int)num("a number of cycles"); continue; }
        if (a == "-bmc") { opt.bmcOnly = true; continue; }
        if (a == "-no-pdr") { opt.noPdr = true; continue; }
        if (a == "-prop") { opt.only.push_back(next("a property name")); continue; }
        if (a == "-threads" || a == "-j") { opt.threads = (int)num("a number of threads"); continue; }
        if (a == "-conflicts") { opt.conflicts = num("a number of conflicts"); continue; }
        if (a == "-o") { opt.outDir = next("a directory"); continue; }
        if (a == "-no-trace") { opt.noTrace = true; continue; }
        if (a == "-tb") { opt.tb = true; continue; }
        if (a == "-json") { opt.json = next("a file name"); continue; }
        if (a == "-app") {
            std::string app = next("an app name");
            if (app == "toggle") opt.appToggle = true;
            else { std::cerr << "mformal: unknown app '" << app << "' (apps: toggle)\n"; return 2; }
            continue;
        }
        if (a == "-connect") { opt.connect.push_back(next("a file name")); continue; }
        if (a == "-toggle-deep") { opt.toggleDeep = true; continue; }
        if (a == "-toggle-max") { opt.toggleMax = std::atoi(next("a number").c_str()); continue; }
        if (a == "-aiger") { opt.aiger = next("a file name"); continue; }
        if (a == "-Werror") { opt.werror = true; continue; }
        if (a == "-quiet" || a == "-q") { opt.quiet = true; continue; }
        if (a == "-v" || a == "-verbose") { opt.verbose = true; continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        fwd.push_back(a);
    }

    slang::driver::Driver driver;
    driver.addStandardArgs();
    {
        std::vector<const char*> cargv;
        for (auto& s : fwd) cargv.push_back(s.c_str());
        if (!driver.parseCommandLine((int)cargv.size(), cargv.data())) return 2;
        for (auto& s : fwd)
            if (s == "--help") {
                std::cout << driver.cmdLine.getHelpText("mformal front end") << "\n";
                return 0;
            }
        if (!driver.processOptions()) return 2;
        if (!driver.parseAllSources()) return 2;
    }
    auto comp = driver.createCompilation();
    driver.reportCompilation(*comp, /*quiet*/ true);
    if (!driver.reportDiagnostics(/*quiet*/ true)) return 2;

    auto t0 = std::chrono::steady_clock::now();
    Model m;
    m.opt = opt;
    if (auto tops = comp->getRoot().topInstances; tops.size() == 1) m.top = std::string(tops[0]->name);
    try {
        m.lowered = lower(*comp,
                          [&](const std::string& n, int w, bool net) {
                              m.sigs.push_back({n, w, net});
                              return (int)m.sigs.size() - 1;
                          },
                          [](int) { return 0; });
        if (auto tops = comp->getRoot().topInstances; tops.size() == 1) {
            m.topModule = std::string(tops[0]->getDefinition().name);
            for (auto* ps : tops[0]->body.getPortList()) {
                if (ps->kind != slang::ast::SymbolKind::Port) continue;
                auto& port = ps->as<slang::ast::PortSymbol>();
                if (port.direction != slang::ast::ArgumentDirection::In) continue;
                int sg = m.findSig(std::string(port.name));
                if (sg >= 0) m.topInputs.push_back({std::string(port.name), sg});
            }
        }
        if (opt.appToggle && comp->getRoot().topInstances.size() == 1) {
            // Loop counters and other variables local to a procedure are not coverage targets.
            std::string topPath = std::string(comp->getRoot().topInstances[0]->name) + ".";
            comp->getRoot().visit(slang::ast::makeVisitor([&](auto& v, const slang::ast::VariableSymbol& var) {
                const slang::ast::Scope* sc = var.getParentScope();
                bool local = false;
                while (sc) {
                    auto k = sc->asSymbol().kind;
                    if (k != slang::ast::SymbolKind::StatementBlock && k != slang::ast::SymbolKind::Subroutine) break;
                    local = true;
                    sc = sc->asSymbol().getParentScope();
                }
                if (local && sc) {
                    std::string path = sc->asSymbol().getHierarchicalPath() + "." + std::string(var.name);
                    if (path.compare(0, topPath.size(), topPath) == 0) m.procLocals.insert(path.substr(topPath.size()));
                }
                v.visitDefault(var);
            }));
        }
        m.build();
        addLiveness(m);
    } catch (const std::exception& ex) {
        std::cerr << "mformal: " << ex.what() << "\n";
        return 2;
    }
    opt = m.opt;

    // which properties to check
    std::vector<int> constraints;
    for (auto& p : m.props)
        if (p.dir == AD_ASSUME && p.skipped.empty()) constraints.push_back(p.bad);
    std::vector<size_t> todo;
    for (size_t i = 0; i < m.props.size(); i++) {
        auto& p = m.props[i];
        if (!opt.only.empty()) {
            bool hit = false;
            for (auto& o : opt.only)
                hit = hit || p.name == o || (p.name.size() > o.size() && p.name.compare(p.name.size() - o.size(), o.size(), o) == 0 &&
                                             p.name[p.name.size() - o.size() - 1] == '.');
            if (!hit) continue;
        }
        todo.push_back(i);
    }
    if (!opt.aiger.empty()) {
        if (!writeAiger(m, todo, constraints, opt.aiger)) {
            std::cerr << "mformal: cannot write " << opt.aiger << "\n";
            return 2;
        }
        if (!opt.quiet) std::cout << "mformal: wrote " << opt.aiger << "\n";
        return 0;
    }
    std::vector<Result> results(m.props.size());
    unsigned threads = opt.threads > 0 ? (unsigned)opt.threads : par::defaultThreads();
    // Each job writes only its own result, so the outcome is the same for any thread count.
    threads = (unsigned)std::max<size_t>(1, std::min<size_t>(threads, todo.size()));
    par::Pool pool(threads);
    auto parallel = [&](size_t n, const std::function<void(size_t)>& job) { pool.run(n, job); };
    std::vector<char> safetyOnly(m.props.size(), 0);
    // Induction over many cycles is expensive and PDR often proves the same
    // thing at once, so the first pass keeps both short: induction up to
    // quickSteps cycles, then PDR with a small budget. What is still open gets
    // induction up to -depth and PDR with its whole budget.
    const int quickSteps = 10;
    const long long quickPdrCalls = 3000;
    bool quick = !opt.bmcOnly && !opt.noPdr && opt.depth > quickSteps;
    parallel(todo.size(), [&](size_t j) {
        const Property& p = m.props[todo[j]];
        Result& r = results[todo[j]];
        if (!p.skipped.empty()) { r.v = V_SKIPPED; r.note = p.skipped; return; }
        if (p.dir == AD_ASSUME) { r.v = V_ASSUMED; return; }
        r = checkProperty(m, p, constraints, nullptr, -1, false, quick ? quickSteps : -1);
    });
    // What induction alone could not prove: again, with everything proven so
    // far as lemmas, until a round proves nothing new.
    for (int stage = 0; stage < (quick ? 2 : 1); stage++) {
    bool deep = quick && stage == 1;
    for (size_t had = deep ? (size_t)-1 : 0;;) {
        std::vector<int> lemmas;
        std::vector<size_t> open;
        for (size_t i : todo) {
            const Result& r = results[i];
            if ((r.v == V_PROVEN || r.v == V_UNREACHABLE || safetyOnly[i]) && m.props[i].bad != 0) lemmas.push_back(m.props[i].bad);
            if (r.v == V_BOUNDED && !safetyOnly[i] && !(deep && m.props[i].app && !opt.toggleDeep)) open.push_back(i);
        }
        if (opt.bmcOnly || open.empty() || lemmas.size() == had) break;
        had = lemmas.size();
        std::vector<Result> again(open.size());
        parallel(open.size(), [&](size_t j) { again[j] = checkProperty(m, m.props[open[j]], constraints, &lemmas, -1, false, quick && !deep ? quickSteps : -1); });
        for (size_t j = 0; j < open.size(); j++) {
            if (again[j].v != V_PROVEN && again[j].v != V_UNREACHABLE) continue;
            Result& r = results[open[j]];
            again[j].seconds += r.seconds;
            r = again[j];
            r.note = (lemmas.empty() ? "induction, k=" : "induction with lemmas, k=") + std::to_string(r.depth + 1);
        }
    }
    // What is still open: PDR, which finds the invariant a proof needs. When it
    // finds a failure deeper than -depth instead, a bounded check that deep gives the trace.
    if (!opt.bmcOnly && !opt.noPdr) {
        std::vector<int> lemmas;
        std::vector<size_t> open;
        for (size_t i : todo) {
            const Result& r = results[i];
            if ((r.v == V_PROVEN || r.v == V_UNREACHABLE || safetyOnly[i]) && m.props[i].bad != 0) lemmas.push_back(m.props[i].bad);
            if (r.v == V_BOUNDED && !safetyOnly[i] && !(deep && m.props[i].app && !opt.toggleDeep)) open.push_back(i);
        }
        std::vector<Result> again(open.size());
        parallel(open.size(), [&](size_t j) {
            const Property& p = m.props[open[j]];
            Result pr = checkPdr(m, p, constraints, lemmas, quick && !deep ? quickPdrCalls : opt.pdrCalls);
            if (pr.v == V_BOUNDED && pr.depth >= 0) {
                Result br = checkProperty(m, p, constraints, nullptr, pr.depth + 1, true);
                if (br.v != V_FALSIFIED && br.v != V_COVERED) br = checkProperty(m, p, constraints, nullptr, pr.depth + 1);
                br.seconds += pr.seconds;
                pr = br;
            }
            again[j] = pr;
        });
        for (size_t j = 0; j < open.size(); j++) {
            Result& r = results[open[j]];
            if (again[j].v == V_BOUNDED && again[j].depth < 0 && again[j].inputs.empty()) { r.seconds += again[j].seconds; continue; }
            again[j].seconds += r.seconds;
            if (again[j].v == V_BOUNDED) again[j].bound = std::max(again[j].bound, r.bound);
            r = again[j];
        }
    }
    }
    // Liveness that this engine does not decide: only the safety part was checked.
    for (size_t i : todo) {
        Result& r = results[i];
        const Property& p = m.props[i];
        if (p.liveInexact.empty()) continue;
        if (r.v == V_PROVEN) {
            r.v = V_BOUNDED;
            r.bound = std::max(r.bound, opt.depth);
            r.note = "the safety part is proven; liveness not decided (" + p.liveInexact + ")";
        } else if (r.v == V_BOUNDED)
            r.note = "liveness not decided (" + p.liveInexact + ")";
    }
    for (auto& p : m.props)
        if (!p.fairInexact.empty() && !opt.quiet)
            std::cerr << "mformal: warning: " << p.name << ": the liveness part of this assumption is not used (" << p.fairInexact << ")\n";
    // a liveness counterexample: where its loop starts
    for (size_t i : todo) {
        Result& r = results[i];
        const Property& p = m.props[i];
        if (r.v != V_FALSIFIED || p.live == 0) continue;
        auto val = replay(m, r);
        auto at = [&](size_t f, int l) { return (val[f][(size_t)(l >> 1)] ^ (l & 1)) != 0; };
        if (!at(val.size() - 1, p.live)) continue;
        int rc = m.hasReset ? opt.resetCycles : 0;
        for (size_t f = 0; f + 1 < val.size(); f++)
            if (!at(f, p.saved) && at(f + 1, p.saved)) r.note = "liveness: from here the design can repeat cycles " + std::to_string((int)f - rc) + " to " + std::to_string(r.depth - 1) + " forever";
    }
    // traces, in property order
    if (!opt.noTrace) {
        bool made = false;
        for (size_t i : todo) {
            Result& r = results[i];
            if (r.v != V_FALSIFIED && r.v != V_COVERED) continue;
            if (m.props[i].app) continue;   // a reachable coverage target needs no trace
            if (!made) {
                std::error_code ec;
                std::filesystem::create_directories(opt.outDir, ec);
                made = true;
            }
            std::string path = opt.outDir + "/" + fileSafe(m.props[i].name) + ".vcd";
            if (writeVcd(m, r, m.props[i], path)) r.trace = path;
            else std::cerr << "mformal: cannot write " << path << "\n";
            if (opt.tb) {
                std::string why;
                if (!writeTb(m, r, m.props[i], opt.outDir + "/" + fileSafe(m.props[i].name) + "_tb.sv", why))
                    std::cerr << "mformal: no replay testbench for " << m.props[i].name << ": " << why << "\n";
            }
        }
    }

    // report
    static const char* vname[] = {"proven", "falsified", "covered", "unreachable", "bounded", "inconclusive", "skipped", "assumed"};
    int count[8] = {}, appCount[8] = {};
    size_t nameW = 8;
    for (size_t i : todo) nameW = std::max(nameW, m.props[i].name.size());
    if (!opt.quiet) {
        std::cout << "mformal: clock " << (m.clockSig >= 0 ? m.sigs[(size_t)m.clockSig].name : std::string("(none)"));
        if (m.hasReset) std::cout << ", reset " << opt.reset << (opt.resetLow ? " (active low" : " (active high") << ", " << opt.resetCycles << (opt.resetCycles == 1 ? " cycle)" : " cycles)");
        else std::cout << ", no reset";
        int ands = 0;
        for (auto& nd : m.g.gr.nodes) ands += nd.k == aig::NK::And;
        std::cout << ", " << m.stateBits.size() << " state bits, " << ands << " gates, depth " << opt.depth << ", " << threads << (threads == 1 ? " thread\n" : " threads\n");
    }
    for (size_t i : todo) {
        const Property& p = m.props[i];
        const Result& r = results[i];
        if (p.app == 1) appCount[r.v]++;
        else count[r.v]++;
        if (p.app && r.v == V_COVERED && !opt.verbose) continue;
        const char* kind = p.app == 1 ? "toggle" : p.dir == AD_COVER ? "cover " : p.dir == AD_ASSUME ? "assume" : "assert";
        std::cout << "  " << kind << "  " << p.name << std::string(nameW - p.name.size() + 2, ' ') << vname[r.v];
        switch (r.v) {
            case V_PROVEN: case V_UNREACHABLE:
                if (r.note.empty()) std::cout << "  (induction, k=" << r.depth + 1 << ")";
                else std::cout << "  (" << r.note << ")";
                break;
            case V_FALSIFIED: case V_COVERED:
                std::cout << "  at cycle " << r.depth;
                if (!r.trace.empty()) std::cout << "  " << r.trace;
                if (!r.note.empty()) std::cout << "  (" << r.note << ")";
                break;
            case V_BOUNDED:
                std::cout << "  no " << (p.dir == AD_COVER ? "hit" : "counterexample") << " in " << r.bound << " cycles";
                if (!r.note.empty()) std::cout << "; " << r.note;
                break;
            case V_INCONCLUSIVE: case V_SKIPPED: std::cout << "  " << r.note; break;
            default: break;
        }
        std::cout << "\n";
    }
    if (opt.verbose && !m.cutNotes.empty()) {
        std::cout << "cut (free in every cycle):\n";
        for (auto& n : m.cutNotes) std::cout << "  " << n << "\n";
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!opt.quiet) {
        int nApp = 0;
        for (int v = 0; v < 8; v++) nApp += appCount[v];
        if (nApp) {
            int open = nApp - appCount[V_COVERED] - appCount[V_UNREACHABLE];
            std::cout << "mformal: toggle: " << nApp << " targets on " << nApp / 2 << " signal bits: " << appCount[V_COVERED] << " reachable, "
                      << appCount[V_UNREACHABLE] << " unreachable";
            if (open) std::cout << ", " << open << " not decided";
            std::cout << "\n";
        }
        size_t nProps = todo.size() - (size_t)nApp;
        std::cout << "mformal: " << nProps << (nProps == 1 ? " property: " : " properties: ");
        bool first = true;
        for (int v = 0; v < 8; v++)
            if (count[v]) { std::cout << (first ? "" : ", ") << count[v] << " " << vname[v]; first = false; }
        if (first) std::cout << "nothing to check";
        if (!m.cutNotes.empty() && !opt.verbose) std::cout << "; " << m.cutNotes.size() << " signals cut (-v lists them)";
        char buf[32];
        snprintf(buf, sizeof buf, "%.2f", secs);
        std::cout << "  [" << buf << " s]\n";
    }
    if (!opt.json.empty()) {
        std::ofstream js(opt.json);
        js << "{\n  \"tool\": \"mformal\",\n  \"version\": " << jsonStr(MINISIM_VERSION) << ",\n  \"clock\": "
           << jsonStr(m.clockSig >= 0 ? m.sigs[(size_t)m.clockSig].name : "") << ",\n  \"reset\": " << jsonStr(m.hasReset ? opt.reset : "")
           << ",\n  \"depth\": " << opt.depth << ",\n  \"state_bits\": " << m.stateBits.size() << ",\n  \"properties\": [\n";
        for (size_t k = 0; k < todo.size(); k++) {
            const Property& p = m.props[todo[k]];
            const Result& r = results[todo[k]];
            js << "    {\"name\": " << jsonStr(p.name) << ", \"kind\": " << jsonStr(p.app == 1 ? "toggle" : p.dir == AD_COVER ? "cover" : p.dir == AD_ASSUME ? "assume" : "assert")
               << ", \"result\": " << jsonStr(vname[r.v]);
            if (r.depth >= 0) js << ", \"cycle\": " << r.depth;
            if (r.v == V_BOUNDED || r.v == V_INCONCLUSIVE) js << ", \"bound\": " << r.bound;
            if (!r.trace.empty()) js << ", \"trace\": " << jsonStr(r.trace);
            if (!r.note.empty()) js << ", \"note\": " << jsonStr(r.note);
            js << ", \"cone_state_bits\": " << r.coneState << ", \"cone_gates\": " << r.coneAnd << "}" << (k + 1 < todo.size() ? "," : "") << "\n";
        }
        js << "  ],\n  \"cut\": [";
        bool first = true;
        for (auto& n : m.cutNotes) { js << (first ? "" : ", ") << jsonStr(n); first = false; }
        js << "]\n}\n";
    }
    if (count[V_FALSIFIED]) return 1;
    if (opt.werror && (count[V_UNREACHABLE] || count[V_INCONCLUSIVE] || count[V_SKIPPED] || appCount[V_UNREACHABLE])) return 1;
    return 0;
}
