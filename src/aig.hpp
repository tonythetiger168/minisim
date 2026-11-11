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
        if (t == L1) return orl(s, e);
        if (t == L0) return andl(lneg(s), e);
        if (e == L1) return orl(lneg(s), t);
        if (e == L0) return andl(s, t);
        if (t == lneg(e)) return xnorl(s, t) == L0 ? L0 : lneg(xorl(s, t));
        return orl(andl(s, t), andl(lneg(s), e));
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

struct Design {
    std::string top;
    Graph g;
    std::vector<Port> ports;
    std::vector<std::string> warnings;
};

// Rebuild into a clean graph: wires resolved, constants propagated, logic
// that reaches no output or flop dropped, combinational loops reported.
// Flops come first in the new graph, so feedback through them is fine.
inline Design cleanup(const Design& in) {
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
        if (g.nodes[n].k == NK::Input) map[n] = ng.input(g.inputNames[g.nodes[n].aux]);
    for (uint32_t n = 1; n < g.size(); n++)
        if (g.nodes[n].k == NK::Dff) {
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

}  // namespace aig
