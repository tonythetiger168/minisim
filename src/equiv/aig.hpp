#pragma once
// aig.hpp - mequiv's view of the shared and-inverter graph (src/aig.hpp).
//
// A literal is node*2 + inverted. Node 0 is constant false, so literal 0 is
// false and literal 1 is true. Every other node mequiv makes is an input or
// a two-input AND. AND nodes are structurally hashed, so the two designs
// under comparison share every piece of logic they build the same way and
// most compare points are settled before a solver runs.
//
// The graph, its hashing and the cone copies are aig::Graph and the free
// functions in src/aig.hpp; this file only keeps the names mequiv uses and
// an xor and mux with one canonical shape per function.
#include <cstdint>
#include <string>
#include <vector>

#include "../aig.hpp"

namespace equiv {

using Lit = aig::Lit;
inline constexpr Lit kFalse = aig::L0, kTrue = aig::L1;
inline constexpr Lit kNoLit = aig::NOLIT;
inline Lit neg(Lit a) { return a ^ 1u; }
inline uint32_t nodeOf(Lit a) { return a >> 1; }
inline bool isNeg(Lit a) { return a & 1u; }

class Aig {
public:
    using Node = aig::Node;  // fanins a and b are meaningful for AND nodes only

    aig::Graph graph;

    size_t size() const { return graph.nodes.size(); }
    const Node& node(uint32_t n) const { return graph.nodes[n]; }
    bool isInput(uint32_t n) const { return graph.nodes[n].k == aig::NK::Input; }
    bool isAnd(uint32_t n) const { return graph.nodes[n].k == aig::NK::And; }
    size_t numAnds() const { return graph.nodes.size() - 1 - graph.inputNames.size(); }

    Lit input() { return graph.input(std::string()); }
    Lit and_(Lit a, Lit b) { return graph.andl(a, b); }
    Lit or_(Lit a, Lit b) { return neg(and_(neg(a), neg(b))); }
    Lit xor_(Lit a, Lit b) {
        if (a == kFalse) return b;
        if (b == kFalse) return a;
        if (a == kTrue) return neg(b);
        if (b == kTrue) return neg(a);
        if (a == b) return kFalse;
        if (a == neg(b)) return kTrue;
        // Canonical polarity, so xor(a,b), xor(!a,!b) and !xor(!a,b) meet.
        bool inv = isNeg(a) ^ isNeg(b);
        a &= ~1u, b &= ~1u;
        Lit r = or_(and_(a, neg(b)), and_(neg(a), b));
        return inv ? neg(r) : r;
    }
    Lit mux(Lit s, Lit t, Lit e) {  // s ? t : e
        if (s == kTrue) return t;
        if (s == kFalse) return e;
        if (t == e) return t;
        if (t == neg(e)) return xor_(s, e);
        if (isNeg(s)) { s = neg(s); std::swap(t, e); }
        if (t == kTrue) return or_(s, e);
        if (t == kFalse) return and_(neg(s), e);
        if (e == kTrue) return or_(neg(s), t);
        if (e == kFalse) return and_(s, t);
        return or_(and_(s, t), and_(neg(s), e));
    }

    // Cone copies: see aig::rebuild, aig::resolve and aig::rebuildSparse.
    Lit rebuild(Lit root, std::vector<Lit>& memo) { return aig::rebuild(graph, root, memo); }
    template<typename Defs>
    Lit resolve(Lit root, std::vector<Lit>& memo, const Defs& defs, std::vector<uint8_t>& active,
                uint32_t* culprit = nullptr) {
        return aig::resolve(graph, root, memo, defs, active, culprit);
    }
    template<typename Map>
    Lit rebuildSparse(Lit root, Map& memo) { return aig::rebuildSparse(graph, root, memo); }

    // n is xnor(a, b) in the shape xor_ builds: !(a & !b) & !(!a & b).
    bool isXnor(uint32_t n, Lit& a, Lit& b) const {
        if (!isAnd(n)) return false;
        const Node& nd = node(n);
        if (!isNeg(nd.a) || !isNeg(nd.b) || !isAnd(nodeOf(nd.a)) || !isAnd(nodeOf(nd.b))) return false;
        const Node& p = node(nodeOf(nd.a));
        const Node& q = node(nodeOf(nd.b));
        if ((p.a == neg(q.a) && p.b == neg(q.b)) || (p.a == neg(q.b) && p.b == neg(q.a))) {
            a = p.a;
            b = neg(p.b);
            return true;
        }
        return false;
    }
    // rebuildSparse that puts every xor back through xor_, so that a cone with
    // some variables fixed gets the same shape as logic built that way from
    // the start (the polarity of an xor's operands decides how it is stored).
    template<typename Map>
    Lit cofactor(Lit root, Map& memo) {
        uint32_t rn = nodeOf(root);
        if (rn == 0) return root;
        std::vector<uint32_t> st{rn};
        auto get = [&](Lit l) { return nodeOf(l) ? memo.find(nodeOf(l))->second ^ (l & 1u) : l; };
        while (!st.empty()) {
            uint32_t n = st.back();
            if (memo.count(n)) { st.pop_back(); continue; }
            if (!isAnd(n)) { memo[n] = n * 2; st.pop_back(); continue; }
            Lit a, b;
            // (not when one of the xnor's inner nodes is itself given a value)
            bool x = isXnor(n, a, b) && !memo.count(nodeOf(node(n).a)) && !memo.count(nodeOf(node(n).b));
            if (!x) a = node(n).a, b = node(n).b;
            bool ready = true;
            for (Lit f : {a, b})
                if (nodeOf(f) && !memo.count(nodeOf(f))) { st.push_back(nodeOf(f)); ready = false; }
            if (!ready) continue;
            Lit r = x ? neg(xor_(get(a), get(b))) : and_(get(a), get(b));
            memo[n] = r;
            st.pop_back();
        }
        return memo[rn] ^ (root & 1u);
    }
};

}  // namespace equiv
