#pragma once
// aig.hpp - bit-level logic shared by the synthesis, equivalence, formal and
// FPGA tools: an and-inverter graph with flops.
//
// A literal is node * 2 + complement. Node 0 is constant false, so literal 0
// is 0 and literal 1 is 1. Node kinds:
//   Input  a primary input bit (or a flop output, see Dff)
//   And    the AND of two literals, structurally hashed
//   Wire   a placeholder whose driver is set later (elaboration only; the
//          clean graph that cleanup() builds has none)
//   Dff    a flop output; its D, clock and async controls are in flops[]
//
// Word-level helpers (adders, comparators, shifters, multipliers,
// dividers) build on and/or/xor/mux and work on Bits, LSB first.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace aig {

using Lit = uint32_t;
constexpr Lit L0 = 0, L1 = 1;
constexpr Lit NOLIT = 0xffffffffu;
inline Lit lneg(Lit l) { return l ^ 1; }
inline uint32_t lnode(Lit l) { return l >> 1; }
inline bool lisneg(Lit l) { return l & 1; }
inline Lit mklit(uint32_t n, bool neg = false) { return (n << 1) | (neg ? 1 : 0); }
inline Lit lxorc(Lit l, bool c) { return l ^ (c ? 1 : 0); }

using Bits = std::vector<Lit>;

enum class NK : uint8_t { Const, Input, And, Wire, Dff };

struct Node {
    NK k = NK::Const;
    Lit a = 0, b = 0;   // And: fanins; Wire: a = driver (NOLIT until set)
    int aux = -1;       // Input: index in inputs; Dff: index in flops
};

struct Flop {
    Lit q = 0;              // the Dff node's literal
    Lit d = 0;              // next state
    Lit clk = 0;            // clock literal
    bool negedge = false;   // clocked on the falling edge
    Lit arst = NOLIT;       // async control, active when it equals 1 (NOLIT: none)
    bool rval = false;      // value under the async control: 0 = clear, 1 = set
    std::string name;       // e.g. u_core/cnt_reg[3]
    std::string var;        // the RTL variable, e.g. u_core/cnt
    int bit = 0;
};

struct Port {
    std::string name;
    bool out = false;
    int msb = 0, lsb = 0;   // declared range, for the netlist
    Bits bits;              // LSB first; inputs: Input literals, outputs: driver literals
};

struct Graph {
    std::vector<Node> nodes{Node{}};
    std::vector<Flop> flops;
    std::vector<std::string> inputNames;   // per Input node (aux)
    std::unordered_map<uint64_t, Lit> strash;
    bool hashing = true;

    size_t size() const { return nodes.size(); }
    const Node& at(Lit l) const { return nodes[lnode(l)]; }

    Lit input(const std::string& name) {
        Node n;
        n.k = NK::Input;
        n.aux = (int)inputNames.size();
        inputNames.push_back(name);
        nodes.push_back(n);
        return mklit((uint32_t)nodes.size() - 1);
    }
    // The don't-care value ('x in the source): a leaf that mux() resolves to
    // the other arm, and that cleanup() turns into 0 wherever it is left.
    Lit xl = NOLIT;
    Lit dontCare() {
        if (xl == NOLIT) xl = input("$x");
        return xl;
    }
    bool isX(Lit l) const { return xl != NOLIT && (l | 1u) == (xl | 1u); }
    Lit wire() {
        Node n;
        n.k = NK::Wire;
        n.a = NOLIT;
        nodes.push_back(n);
        return mklit((uint32_t)nodes.size() - 1);
    }
    Lit dff(Flop f) {
        Node n;
        n.k = NK::Dff;
        n.aux = (int)flops.size();
        nodes.push_back(n);
        f.q = mklit((uint32_t)nodes.size() - 1);
        flops.push_back(f);
        return f.q;
    }

    Lit andl(Lit a, Lit b) {
        if (a > b) std::swap(a, b);
        if (a == L0) return L0;
        if (a == L1) return b;
        if (a == b) return a;
        if (a == lneg(b)) return L0;
        // a & (a & x) = a & x ; a & (!a & x) = 0 (one level of lookahead)
        for (int side = 0; side < 2; side++) {
            Lit p = side ? b : a, q = side ? a : b;
            const Node& n = nodes[lnode(q)];
            if (n.k == NK::And && !lisneg(q)) {
                if (n.a == p || n.b == p) return q;
                if (n.a == lneg(p) || n.b == lneg(p)) return L0;
            }
            if (n.k == NK::And && lisneg(q)) {
                // a & !(a & x) = a & !x ; a & !(!a & x) = a
                if (n.a == lneg(p) || n.b == lneg(p)) return p;
                if (n.a == p) return andl(p, lneg(n.b));
                if (n.b == p) return andl(p, lneg(n.a));
            }
        }
        uint64_t key = ((uint64_t)a << 32) | b;
        if (hashing) {
            auto it = strash.find(key);
            if (it != strash.end()) return it->second;
        }
        Node n;
        n.k = NK::And;
        n.a = a;
        n.b = b;
        nodes.push_back(n);
        Lit r = mklit((uint32_t)nodes.size() - 1);
        if (hashing) strash[key] = r;
        return r;
    }
    Lit orl(Lit a, Lit b) { return lneg(andl(lneg(a), lneg(b))); }
    Lit xorl(Lit a, Lit b) {
        if (a == L0) return b;
        if (b == L0) return a;
        if (a == L1) return lneg(b);
        if (b == L1) return lneg(a);
        if (a == b) return L0;
        if (a == lneg(b)) return L1;
        // two ANDs of the same polarity pair: (a & !b) | (!a & b)
        return orl(andl(a, lneg(b)), andl(lneg(a), b));
    }
    Lit xnorl(Lit a, Lit b) { return lneg(xorl(a, b)); }
    Lit mux(Lit s, Lit t, Lit e) {   // s ? t : e
        if (s == L1) return t;
        if (s == L0) return e;
        if (t == e) return t;
        if (isX(t) || isX(s)) return e;
        if (isX(e)) return t;
        if (t == L1) return orl(s, e);
        if (t == L0) return andl(lneg(s), e);
        if (e == L1) return orl(lneg(s), t);
        if (e == L0) return andl(s, t);
        if (t == lneg(e)) return xnorl(s, t) == L0 ? L0 : lneg(xorl(s, t));
        // Nested selects with a shared arm fold into one select with a
        // combined condition (if (a) if (b) x = d; and priority chains), so
        // a word costs one mux per bit plus one shared gate.
        Lit s2, a, b;
        if (asMux(t, s2, a, b)) {
            if (b == e) return muxRaw(andl(s, s2), a, e);
            if (a == e) return muxRaw(andl(s, lneg(s2)), b, e);
        }
        if (asMux(e, s2, a, b)) {
            if (a == t) return muxRaw(orl(s, s2), t, b);
            if (b == t) return muxRaw(orl(s, lneg(s2)), t, a);
        }
        return muxRaw(s, t, e);
    }
    Lit muxRaw(Lit s, Lit t, Lit e) {
        if (s == L1) return t;
        if (s == L0) return e;
        return orl(andl(s, t), andl(lneg(s), e));
    }
    // l == s ? t : e, when l has the shape mux() builds: !(!(s & t) & !(!s & e)).
    bool asMux(Lit l, Lit& s, Lit& t, Lit& e) const {
        if (!lisneg(l)) return false;
        const Node& o = nodes[lnode(l)];
        if (o.k != NK::And || !lisneg(o.a) || !lisneg(o.b)) return false;
        const Node& x = nodes[lnode(o.a)];
        const Node& y = nodes[lnode(o.b)];
        if (x.k != NK::And || y.k != NK::And) return false;
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++) {
                Lit xs = i ? x.b : x.a, ys = j ? y.b : y.a;
                if (xs != lneg(ys)) continue;
                s = xs;
                t = i ? x.a : x.b;
                e = j ? y.a : y.b;
                return true;
            }
        return false;
    }
    Lit maj(Lit a, Lit b, Lit c) { return orl(andl(a, b), andl(c, orl(a, b))); }

    // ---- word level
    Bits konst(uint64_t v, int w) {
        Bits r(w);
        for (int i = 0; i < w; i++) r[i] = (i < 64 && ((v >> i) & 1)) ? L1 : L0;
        return r;
    }
    static Bits extend(const Bits& a, int w, bool sgn) {
        Bits r = a;
        Lit fill = sgn && !a.empty() ? a.back() : L0;
        r.resize(w, fill);
        return r;
    }
    Bits bnot(const Bits& a) {
        Bits r(a.size());
        for (size_t i = 0; i < a.size(); i++) r[i] = lneg(a[i]);
        return r;
    }
    Bits band(const Bits& a, const Bits& b) {
        Bits r(a.size());
        for (size_t i = 0; i < a.size(); i++) r[i] = andl(a[i], b[i]);
        return r;
    }
    Bits bor(const Bits& a, const Bits& b) {
        Bits r(a.size());
        for (size_t i = 0; i < a.size(); i++) r[i] = orl(a[i], b[i]);
        return r;
    }
    Bits bxor(const Bits& a, const Bits& b) {
        Bits r(a.size());
        for (size_t i = 0; i < a.size(); i++) r[i] = xorl(a[i], b[i]);
        return r;
    }
    Bits bmux(Lit s, const Bits& t, const Bits& e) {
        Bits r(t.size());
        for (size_t i = 0; i < t.size(); i++) r[i] = mux(s, t[i], e[i]);
        return r;
    }
    Lit redAnd(const Bits& a) { return tree(a, [&](Lit x, Lit y) { return andl(x, y); }, L1); }
    Lit redOr(const Bits& a) { return tree(a, [&](Lit x, Lit y) { return orl(x, y); }, L0); }
    Lit redXor(const Bits& a) { return tree(a, [&](Lit x, Lit y) { return xorl(x, y); }, L0); }
    template <class F>
    Lit tree(Bits v, F f, Lit unit) {
        if (v.empty()) return unit;
        while (v.size() > 1) {
            Bits n;
            for (size_t i = 0; i + 1 < v.size(); i += 2) n.push_back(f(v[i], v[i + 1]));
            if (v.size() & 1) n.push_back(v.back());
            v.swap(n);
        }
        return v[0];
    }

    // Adder style: ripple below prefixMin bits, Sklansky parallel prefix at
    // or above it (log depth for timing).
    int prefixMin = 8;

    // a + b + cin; returns sum (width of a) and the carry out.
    Bits add(const Bits& a, const Bits& b, Lit cin, Lit* cout = nullptr) {
        size_t w = a.size();
        Bits s(w);
        if ((int)w < prefixMin) {
            Lit c = cin;
            for (size_t i = 0; i < w; i++) {
                s[i] = xorl(xorl(a[i], b[i]), c);
                c = maj(a[i], b[i], c);
            }
            if (cout) *cout = c;
            return s;
        }
        // generate / propagate, carry-in folded into position 0
        std::vector<Lit> g(w), p(w), x(w);
        for (size_t i = 0; i < w; i++) {
            x[i] = xorl(a[i], b[i]);
            g[i] = andl(a[i], b[i]);
            p[i] = x[i];
        }
        g[0] = orl(g[0], andl(p[0], cin));
        // Sklansky: at level l, positions with bit l set take from the last
        // position of the previous block.
        for (size_t l = 1; l < w; l <<= 1)
            for (size_t i = 0; i < w; i++)
                if (i & l) {
                    size_t j = (i & ~(l - 1) & ~l) | (l - 1);
                    j = (i / (2 * l)) * (2 * l) + l - 1;
                    g[i] = orl(g[i], andl(p[i], g[j]));
                    p[i] = andl(p[i], p[j]);
                }
        s[0] = xorl(x[0], cin);
        for (size_t i = 1; i < w; i++) s[i] = xorl(x[i], g[i - 1]);
        if (cout) *cout = g[w - 1];
        return s;
    }
    Bits sub(const Bits& a, const Bits& b, Lit* cout = nullptr) { return add(a, bnot(b), L1, cout); }
    Bits negate(const Bits& a) { return add(konst(0, (int)a.size()), bnot(a), L1); }
    Lit eq(const Bits& a, const Bits& b) {
        Bits t(a.size());
        for (size_t i = 0; i < a.size(); i++) t[i] = xnorl(a[i], b[i]);
        return redAnd(t);
    }
    // a < b
    Lit lt(const Bits& a, const Bits& b, bool sgn) {
        if (a.empty()) return L0;
        Bits x = a, y = b;
        if (sgn) {   // flip the sign bits: signed order becomes unsigned order
            x.back() = lneg(x.back());
            y.back() = lneg(y.back());
        }
        // a < b  <=>  no carry out of a + ~b + 1
        Lit c;
        // comparator chain: from LSB, lt = (!a & b) | (!(a ^ b) & lt)
        if ((int)x.size() < prefixMin) {
            Lit r = L0;
            for (size_t i = 0; i < x.size(); i++)
                r = orl(andl(lneg(x[i]), y[i]), andl(xnorl(x[i], y[i]), r));
            return r;
        }
        sub(x, y, &c);
        return lneg(c);
    }
    Bits mul(const Bits& a, const Bits& b) {   // width of a, unsigned (two's complement low bits)
        size_t w = a.size();
        Bits acc = konst(0, (int)w);
        // Partial products, then a carry-save (Wallace-style) reduction per column.
        std::vector<std::vector<Lit>> cols(w);
        for (size_t j = 0; j < w && j < b.size(); j++)
            for (size_t i = 0; i + j < w; i++) {
                Lit pp = andl(a[i], b[j]);
                if (pp != L0) cols[i + j].push_back(pp);
            }
        for (size_t c = 0; c < w; c++) {
            while (cols[c].size() > 2) {
                Lit x = cols[c].back(); cols[c].pop_back();
                Lit y = cols[c].back(); cols[c].pop_back();
                Lit z = cols[c].back(); cols[c].pop_back();
                cols[c].insert(cols[c].begin(), xorl(xorl(x, y), z));
                if (c + 1 < w) cols[c + 1].push_back(maj(x, y, z));
            }
        }
        Bits r0(w, L0), r1(w, L0);
        for (size_t c = 0; c < w; c++) {
            if (cols[c].size() > 0) r0[c] = cols[c][0];
            if (cols[c].size() > 1) r1[c] = cols[c][1];
        }
        return add(r0, r1, L0);
    }
    // unsigned restoring division: q = a / b, r = a % b (b == 0: q = all ones, r = a)
    void divmod(const Bits& a, const Bits& b, Bits& q, Bits& r) {
        size_t w = a.size();
        q.assign(w, L0);
        Bits rem = konst(0, (int)w + 1);
        Bits bb = extend(b, (int)w + 1, false);
        for (int i = (int)w - 1; i >= 0; i--) {
            // rem = rem << 1 | a[i]
            Bits sh(w + 1);
            sh[0] = a[i];
            for (size_t k = 1; k <= w; k++) sh[k] = rem[k - 1];
            Lit c;
            Bits d = sub(sh, bb, &c);   // c = 1 when sh >= bb
            q[i] = c;
            rem = bmux(c, d, sh);
        }
        r = Bits(rem.begin(), rem.begin() + w);
    }
    // shifts by a variable amount: barrel shifter
    Bits shift(const Bits& a, const Bits& amt, bool left, bool arith) {
        size_t w = a.size();
        Lit fill = arith && !a.empty() ? a.back() : L0;
        Bits cur = a;
        for (size_t k = 0; k < amt.size(); k++) {
            uint64_t d = k < 63 ? (1ull << k) : ~0ull;
            Bits n(w);
            for (size_t i = 0; i < w; i++) {
                Lit moved;
                if (left) moved = (d <= i) ? cur[i - d] : L0;
                else moved = (d < w && i + d < w) ? cur[i + d] : fill;
                n[i] = mux(amt[k], moved, cur[i]);
            }
            cur.swap(n);
        }
        return cur;
    }
};

// ------------------------------------------------------------ the design

// A word-level operator as the RTL had it, kept beside the bits it was
// built into: y = a op b. A tool that has a better implementation of the
// operator (a carry chain, a DSP block, a datapath cell) cuts the graph at
// y and drives it from a and b itself. Listed in creation order.
struct WordOp {
    enum Kind { Add, Sub, Mul, Lt, Eq, Shl, Shr, Sshr } kind = Add;
    Bits a, b, y;         // LSB first; Lt and Eq have a one-bit y
    bool sgn = false;     // Lt: signed compare; Mul: operands were sign-extended
};

// A memory as the RTL had it: an unpacked array written through a dynamic
// index in a clocked block. The graph holds it as flops and multiplexers
// (Flop::var == name, Flop::bit == word * width + bit); this table lets a
// tool that has a RAM (a block RAM, a compiled memory) take them out again:
// it drops the memory's flops, drives every read port's data from the RAM
// and connects the write ports.
//
// addr is the word position counted from the LSB end of the array, already
// corrected for the declared range (mem[4:19] has positions 0..15), and may
// be wider than the depth needs: a position outside 0..depth-1 writes
// nothing and reads zero.
struct MemPort {
    Bits addr;
    // Write: the value stored, into bits [lo, lo + data.size()) of the word
    // (a whole word, or a byte lane). Read: the word read, combinationally,
    // from the flops' outputs; a tool finds a registered read by the flops
    // that data feeds.
    Bits data;
    int lo = 0;
    // Write only: the port writes on its clock edge when en is 1.
    Lit en = L1;
    Lit clk = L0;
    bool negedge = false;
};
struct Memory {
    std::string name;     // as Flop::var
    int width = 0, depth = 0;
    // In statement order: when two write ports hit the same bits in the same
    // cycle, the later one wins.
    std::vector<MemPort> wr;
    std::vector<MemPort> rd;
};

struct Design {
    std::string top;
    Graph g;
    std::vector<Port> ports;
    std::vector<std::string> warnings;
    std::vector<WordOp> ops;
    std::vector<Memory> mems;   // in declaration order of first write
};

// d = en ? data : q, for a flop whose next state holds unless `en`. False
// when the D logic is not of that shape.
inline bool flopEnable(const Graph& g, const Flop& f, Lit& en, Lit& data) {
    // mux(s, t, e) is built as !(!(s & t) & !(!s & e))
    if (!lisneg(f.d)) return false;
    const Node& o = g.at(f.d);
    if (o.k != NK::And || !lisneg(o.a) || !lisneg(o.b)) return false;
    const Node& x = g.at(o.a);
    const Node& y = g.at(o.b);
    if (x.k != NK::And || y.k != NK::And) return false;
    for (int swap = 0; swap < 2; swap++) {
        const Node& hold = swap ? x : y;
        const Node& load = swap ? y : x;
        for (int hs = 0; hs < 2; hs++) {
            Lit hq = hs ? hold.b : hold.a, hc = hs ? hold.a : hold.b;   // hold = hc & q
            if (hq != f.q) continue;
            for (int ls = 0; ls < 2; ls++) {
                Lit lc = ls ? load.b : load.a, ld = ls ? load.a : load.b;   // load = lc & data
                if (lc == lneg(hc)) {
                    en = lc;
                    data = ld;
                    return true;
                }
            }
        }
    }
    return false;
}
// d = rst ? value : data, a synchronous set or reset in front of the D logic.
inline bool flopSyncReset(const Graph& g, Lit d, Lit& rst, bool& value, Lit& data) {
    const Node& n = g.at(d);
    if (n.k != NK::And) return false;
    // reset to 0: d = !rst & data. set to 1: d = !(!rst & !data).
    value = lisneg(d);
    for (int s = 0; s < 2; s++) {
        Lit c = s ? n.b : n.a, o = s ? n.a : n.b;
        const Node& cn = g.at(c);
        if (cn.k == NK::And) continue;   // the control is a signal, not logic
        rst = lneg(c);
        data = lxorc(o, value);
        return true;
    }
    return false;
}

// Rebuild into a clean graph: wires resolved, constants propagated, logic
// that reaches no output or flop dropped, combinational loops reported.
// Flops come first in the new graph, so feedback through them is fine.
// With `keep` (per node of the old graph), only the flops marked there are
// carried over; the caller guarantees that nothing kept reads the others.
inline Design cleanup(const Design& in, const std::vector<uint8_t>* keep = nullptr) {
    Design out;
    out.top = in.top;
    out.warnings = in.warnings;
    Graph& ng = out.g;
    const Graph& g = in.g;
    std::vector<Lit> map(g.size(), NOLIT);
    std::vector<uint8_t> state(g.size(), 0);
    map[0] = L0;
    for (size_t i = 0; i < g.inputNames.size(); i++) (void)0;
    // inputs keep their order
    for (uint32_t n = 1; n < g.size(); n++)
        if (g.nodes[n].k == NK::Input) map[n] = g.isX(mklit(n)) ? L0 : ng.input(g.inputNames[g.nodes[n].aux]);
    for (uint32_t n = 1; n < g.size(); n++)
        if (g.nodes[n].k == NK::Dff && (!keep || (*keep)[n])) {
            Flop f = g.flops[g.nodes[n].aux];
            map[n] = ng.dff(f);
        }
    // iterative DFS
    auto build = [&](Lit root) -> Lit {
        std::vector<uint32_t> stack{lnode(root)};
        while (!stack.empty()) {
            uint32_t n = stack.back();
            if (map[n] != NOLIT) { stack.pop_back(); continue; }
            const Node& nd = g.nodes[n];
            if (nd.k == NK::Wire) {
                if (nd.a == NOLIT) { map[n] = L0; stack.pop_back(); continue; }
                uint32_t c = lnode(nd.a);
                if (map[c] == NOLIT) {
                    if (state[n] == 1) throw std::runtime_error("combinational loop");
                    state[n] = 1;
                    stack.push_back(c);
                    continue;
                }
                map[n] = lxorc(map[c], lisneg(nd.a));
                state[n] = 2;
                stack.pop_back();
                continue;
            }
            // And
            uint32_t ca = lnode(nd.a), cb = lnode(nd.b);
            if (map[ca] == NOLIT || map[cb] == NOLIT) {
                if (state[n] == 1) throw std::runtime_error("combinational loop");
                state[n] = 1;
                if (map[ca] == NOLIT) stack.push_back(ca);
                if (map[cb] == NOLIT) stack.push_back(cb);
                continue;
            }
            map[n] = ng.andl(lxorc(map[ca], lisneg(nd.a)), lxorc(map[cb], lisneg(nd.b)));
            state[n] = 2;
            stack.pop_back();
        }
        return lxorc(map[lnode(root)], lisneg(root));
    };
    for (auto& p : in.ports) {
        Port np = p;
        if (p.out)
            for (auto& b : np.bits) b = build(b);
        else
            for (auto& b : np.bits) b = map[lnode(b)] ^ (b & 1);
        out.ports.push_back(np);
    }
    for (size_t i = 0; i < ng.flops.size(); i++) {
        Flop& f = ng.flops[i];
        f.d = build(f.d);
        f.clk = build(f.clk);
        if (f.arst != NOLIT) {
            f.arst = build(f.arst);
            if (f.arst == L0) f.arst = NOLIT;
        }
    }
    // Word operators survive when all of their bits are still in the graph.
    {
        auto get = [&](Lit l, Lit& o) {
            uint32_t n = lnode(l);
            if (map[n] == NOLIT && g.nodes[n].k == NK::Wire && state[n] == 0) {
                // a wire nothing reached: resolve it if its driver was built
                uint32_t c = n;
                int hops = 0;
                bool neg = false;
                while (g.nodes[c].k == NK::Wire && g.nodes[c].a != NOLIT && map[c] == NOLIT && hops++ < 64) {
                    neg ^= lisneg(g.nodes[c].a);
                    c = lnode(g.nodes[c].a);
                }
                if (map[c] == NOLIT) return false;
                o = lxorc(map[c], neg ^ lisneg(l));
                return true;
            }
            if (map[n] == NOLIT) return false;
            o = lxorc(map[n], lisneg(l));
            return true;
        };
        for (const WordOp& op : in.ops) {
            WordOp no = op;
            bool ok = true;
            for (Bits* v : {&no.a, &no.b, &no.y})
                for (Lit& l : *v)
                    if (ok && !get(l, l)) ok = false;
            bool anyLogic = false;
            for (Lit l : no.y) anyLogic |= l > 1;
            if (ok && anyLogic) out.ops.push_back(std::move(no));
        }
        // A memory port survives when all of its bits do; a memory, when it
        // still has flops and a write port.
        std::set<std::string> flopVars;
        for (auto& f : ng.flops) flopVars.insert(f.var);
        for (const Memory& m : in.mems) {
            if (!flopVars.count(m.name)) continue;
            Memory nm;
            nm.name = m.name;
            nm.width = m.width;
            nm.depth = m.depth;
            for (int rd = 0; rd < 2; rd++)
                for (const MemPort& mp : rd ? m.rd : m.wr) {
                    MemPort np = mp;
                    bool ok = true;
                    for (Bits* v : {&np.addr, &np.data})
                        for (Lit& l : *v)
                            if (ok && !get(l, l)) ok = false;
                    if (!rd && ok) ok = get(np.en, np.en) && get(np.clk, np.clk);
                    if (ok && !(!rd && np.en == L0)) (rd ? nm.rd : nm.wr).push_back(std::move(np));
                }
            if (!nm.wr.empty()) out.mems.push_back(std::move(nm));
        }
    }
    return out;
}

// Which nodes feed an output or a flop (others are dead).
inline std::vector<uint8_t> liveNodes(const Design& d) {
    const Graph& g = d.g;
    std::vector<uint8_t> live(g.size(), 0);
    std::vector<uint32_t> st;
    auto push = [&](Lit l) {
        if (l == NOLIT) return;
        uint32_t n = lnode(l);
        if (!live[n]) { live[n] = 1; st.push_back(n); }
    };
    for (auto& p : d.ports)
        if (p.out)
            for (Lit b : p.bits) push(b);
    // flops are live only if their output is used; iterate to a fixpoint
    while (true) {
        while (!st.empty()) {
            uint32_t n = st.back();
            st.pop_back();
            const Node& nd = g.nodes[n];
            if (nd.k == NK::And) { push(nd.a); push(nd.b); }
            if (nd.k == NK::Dff) {
                const Flop& f = g.flops[nd.aux];
                push(f.d);
                push(f.clk);
                push(f.arst);
            }
        }
        break;
    }
    return live;
}

// Sequential sweep, as a synthesis tool does it: flops whose output reaches
// no output port are dropped, and a flop that can only ever hold its
// asynchronous reset value (its next state is that constant, itself, or
// itself gated) becomes the constant. Repeats until nothing changes. `removed`, when
// given, gets the number of flops taken out. cleanup() alone keeps every
// flop, which is what tools that match registers by name want.
inline Design sweep(Design d, size_t* removed = nullptr) {
    size_t before = d.g.flops.size();
    for (;;) {
        Graph& g = d.g;
        bool changed = false;
        for (uint32_t n = 1; n < g.size(); n++) {
            Node& nd = g.nodes[n];
            if (nd.k != NK::Dff) continue;
            const Flop& f = g.flops[nd.aux];
            Lit c = f.rval ? L1 : L0;
            if (f.arst == NOLIT) {
                // No reset: a constant next state is that constant after the
                // first clock edge, and a register that only keeps its
                // value never had one; both become constants (0 for the latter).
                if (f.d == L0 || f.d == L1 || f.d == mklit(n)) {
                    nd.k = NK::Wire;
                    nd.a = f.d == L1 ? L1 : L0;
                    changed = true;
                }
                continue;
            }
            // stays at c: d is c, q itself, q & x (c = 0) or q | x (c = 1)
            bool stuck = f.d == c || f.d == mklit(n);
            const Node& dn = g.nodes[lnode(f.d)];
            Lit qc = mklit(n, f.rval);   // q, as the AND under d sees it
            if (!stuck && dn.k == NK::And && lisneg(f.d) == f.rval && (dn.a == qc || dn.b == qc)) stuck = true;
            if (!stuck) continue;
            nd.k = NK::Wire;
            nd.a = c;
            changed = true;
        }
        std::vector<uint8_t> live = liveNodes(d);
        for (uint32_t n = 1; n < g.size() && !changed; n++)
            if (g.nodes[n].k == NK::Dff && !live[n]) changed = true;
        if (!changed) break;
        d = cleanup(d, &live);
    }
    if (removed) *removed = before - d.g.flops.size();
    return d;
}

// ---- cone copies (used by the equivalence checker)
//
// rebuild: copy the cone of `root` with some nodes replaced. memo[n] is the
// literal that stands for node n (NOLIT: not visited yet). Seed it with the
// substitutions; it must be sized to at least lnode(root)+1. Nodes this call
// creates are appended past that and never revisited. Only And nodes are
// followed; every other kind stands for itself.
inline Lit rebuild(Graph& g, Lit root, std::vector<Lit>& memo) {
    uint32_t rn = lnode(root);
    if (rn == 0) return root;
    std::vector<uint32_t> st{rn};
    while (!st.empty()) {
        uint32_t n = st.back();
        if (memo[n] != NOLIT) { st.pop_back(); continue; }
        Node nd = g.nodes[n];
        if (nd.k != NK::And) { memo[n] = mklit(n); st.pop_back(); continue; }
        uint32_t na = lnode(nd.a), nb = lnode(nd.b);
        bool ready = true;
        if (na && memo[na] == NOLIT) { st.push_back(na); ready = false; }
        if (nb && memo[nb] == NOLIT) { st.push_back(nb); ready = false; }
        if (!ready) continue;
        Lit a = na ? memo[na] ^ (nd.a & 1u) : nd.a;
        Lit b = nb ? memo[nb] ^ (nd.b & 1u) : nd.b;
        Lit r = g.andl(a, b);
        memo[n] = r;
        st.pop_back();
    }
    return memo[rn] ^ (root & 1u);
}

// The same with a sparse memo (any map from node to literal): for small
// cones of a large graph.
template<typename Map>
Lit rebuildSparse(Graph& g, Lit root, Map& memo) {
    uint32_t rn = lnode(root);
    if (rn == 0) return root;
    std::vector<uint32_t> st{rn};
    while (!st.empty()) {
        uint32_t n = st.back();
        if (memo.count(n)) { st.pop_back(); continue; }
        Node nd = g.nodes[n];
        if (nd.k != NK::And) { memo[n] = mklit(n); st.pop_back(); continue; }
        uint32_t na = lnode(nd.a), nb = lnode(nd.b);
        auto ia = memo.find(na), ib = memo.find(nb);
        bool ready = true;
        if (na && ia == memo.end()) { st.push_back(na); ready = false; }
        if (nb && ib == memo.end()) { st.push_back(nb); ready = false; }
        if (!ready) continue;
        Lit a = na ? ia->second ^ (nd.a & 1u) : nd.a;
        Lit b = nb ? ib->second ^ (nd.b & 1u) : nd.b;
        Lit r = g.andl(a, b);
        memo[n] = r;
        st.pop_back();
    }
    return memo[rn] ^ (root & 1u);
}

// resolve: copy the cone of `root`, replacing each placeholder node by the
// logic it stands for (defs: node -> literal), which may itself contain
// placeholders. Unlike Wire nodes and cleanup(), the definitions live
// outside the graph, so a loop through placeholders that is false for the
// cone asked about is not an error. Returns NOLIT if the definitions are
// circular; *culprit is then a placeholder on the cycle. memo and active
// are sized to g.size() by the caller (NOLIT and 0).
template<typename Defs>
Lit resolve(Graph& g, Lit root, std::vector<Lit>& memo, const Defs& defs, std::vector<uint8_t>& active,
            uint32_t* culprit = nullptr) {
    uint32_t rn = lnode(root);
    if (rn == 0) return root;
    std::vector<uint32_t> st{rn};
    while (!st.empty()) {
        uint32_t n = st.back();
        if (memo[n] != NOLIT) { st.pop_back(); continue; }
        Node nd = g.nodes[n];
        if (nd.k != NK::And) {
            auto it = defs.find(n);
            if (it == defs.end()) { memo[n] = mklit(n); st.pop_back(); continue; }
            uint32_t dn = lnode(it->second);
            if (dn == 0) { memo[n] = it->second; st.pop_back(); continue; }
            if (memo[dn] != NOLIT) { memo[n] = memo[dn] ^ (it->second & 1u); st.pop_back(); continue; }
            if (active[n]) {
                if (culprit) *culprit = n;
                return NOLIT;
            }
            active[n] = 1;
            st.push_back(dn);
            continue;
        }
        uint32_t na = lnode(nd.a), nb = lnode(nd.b);
        bool ready = true;
        if (na && memo[na] == NOLIT) { st.push_back(na); ready = false; }
        if (nb && memo[nb] == NOLIT) { st.push_back(nb); ready = false; }
        if (!ready) continue;
        Lit a = na ? memo[na] ^ (nd.a & 1u) : nd.a;
        Lit b = nb ? memo[nb] ^ (nd.b & 1u) : nd.b;
        Lit r = g.andl(a, b);
        memo[n] = r;
        st.pop_back();
    }
    return memo[rn] ^ (root & 1u);
}

}  // namespace aig
