#pragma once
// aig.hpp - the and-inverter graph mequiv proves equivalence on.
//
// A literal is node*2 + inverted. Node 0 is constant false, so literal 0 is
// false and literal 1 is true. Every other node is a primary input or a
// two-input AND. AND nodes are structurally hashed, so the two designs under
// comparison share every piece of logic they build the same way and most
// compare points are settled before a solver runs.
#include <cstdint>
#include <vector>

namespace equiv {

using Lit = uint32_t;
inline constexpr Lit kFalse = 0, kTrue = 1;
inline constexpr Lit kNoLit = 0xFFFFFFFFu;
inline Lit neg(Lit a) { return a ^ 1u; }
inline uint32_t nodeOf(Lit a) { return a >> 1; }
inline bool isNeg(Lit a) { return a & 1u; }

class Aig {
public:
    struct Node {
        Lit a, b;  // both kNoLit for an input (and for node 0)
    };

    Aig() {
        nodes_.push_back({kNoLit, kNoLit});
        table_.assign(1u << 16, 0);
    }

    size_t size() const { return nodes_.size(); }
    const Node& node(uint32_t n) const { return nodes_[n]; }
    bool isInput(uint32_t n) const { return n != 0 && nodes_[n].a == kNoLit; }
    bool isAnd(uint32_t n) const { return nodes_[n].a != kNoLit; }
    size_t numAnds() const { return ands_; }

    Lit input() {
        nodes_.push_back({kNoLit, kNoLit});
        return Lit((nodes_.size() - 1) << 1);
    }

    Lit and_(Lit a, Lit b) {
        if (a > b) std::swap(a, b);
        if (a == kFalse) return kFalse;
        if (a == kTrue) return b;
        if (a == b) return a;
        if (a == neg(b)) return kFalse;
        uint64_t key = (uint64_t(a) << 32) | b;
        size_t mask = table_.size() - 1;
        size_t h = hash(key) & mask;
        while (uint32_t n = table_[h]) {
            if (nodes_[n].a == a && nodes_[n].b == b) return Lit(n << 1);
            h = (h + 1) & mask;
        }
        nodes_.push_back({a, b});
        uint32_t n = uint32_t(nodes_.size() - 1);
        table_[h] = n;
        if (++ands_ * 2 > table_.size()) grow();
        return Lit(n << 1);
    }
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

    // Copy the cone of `root` with some nodes replaced: memo[n] is the literal
    // that stands for node n (kNoLit: not visited yet). Seed it with the
    // substitutions; it must be sized to at least nodeOf(root)+1. Nodes this
    // call creates are appended past that and never revisited.
    Lit rebuild(Lit root, std::vector<Lit>& memo) {
        uint32_t rn = nodeOf(root);
        if (rn == 0) return root;
        if (memo[rn] == kNoLit) {
            stack_.clear();
            stack_.push_back(rn);
            while (!stack_.empty()) {
                uint32_t n = stack_.back();
                if (memo[n] != kNoLit) { stack_.pop_back(); continue; }
                Node nd = nodes_[n];
                if (nd.a == kNoLit) { memo[n] = Lit(n << 1); stack_.pop_back(); continue; }
                uint32_t na = nodeOf(nd.a), nb = nodeOf(nd.b);
                bool ready = true;
                if (na && memo[na] == kNoLit) { stack_.push_back(na); ready = false; }
                if (nb && memo[nb] == kNoLit) { stack_.push_back(nb); ready = false; }
                if (!ready) continue;
                Lit a = na ? memo[na] ^ (nd.a & 1u) : nd.a;
                Lit b = nb ? memo[nb] ^ (nd.b & 1u) : nd.b;
                Lit r = and_(a, b);
                memo[n] = r;
                stack_.pop_back();
            }
        }
        return memo[rn] ^ (root & 1u);
    }

    // Copy the cone of `root`, replacing each placeholder input by the logic
    // it stands for (defs: input node -> literal), which may itself contain
    // placeholders. Returns kNoLit if the definitions are circular; *culprit
    // is then a placeholder on the cycle.
    template<typename Defs>
    Lit resolve(Lit root, std::vector<Lit>& memo, const Defs& defs, std::vector<uint8_t>& active,
                uint32_t* culprit = nullptr) {
        uint32_t rn = nodeOf(root);
        if (rn == 0) return root;
        std::vector<uint32_t> st{rn};
        while (!st.empty()) {
            uint32_t n = st.back();
            if (memo[n] != kNoLit) { st.pop_back(); continue; }
            Node nd = nodes_[n];
            if (nd.a == kNoLit) {
                auto it = defs.find(n);
                if (it == defs.end()) { memo[n] = Lit(n << 1); st.pop_back(); continue; }
                uint32_t dn = nodeOf(it->second);
                if (dn == 0) { memo[n] = it->second; st.pop_back(); continue; }
                if (memo[dn] != kNoLit) { memo[n] = memo[dn] ^ (it->second & 1u); st.pop_back(); continue; }
                if (active[n]) {
                    if (culprit) *culprit = n;
                    return kNoLit;
                }
                active[n] = 1;
                st.push_back(dn);
                continue;
            }
            uint32_t na = nodeOf(nd.a), nb = nodeOf(nd.b);
            bool ready = true;
            if (na && memo[na] == kNoLit) { st.push_back(na); ready = false; }
            if (nb && memo[nb] == kNoLit) { st.push_back(nb); ready = false; }
            if (!ready) continue;
            Lit a = na ? memo[na] ^ (nd.a & 1u) : nd.a;
            Lit b = nb ? memo[nb] ^ (nd.b & 1u) : nd.b;
            Lit r = and_(a, b);
            memo[n] = r;
            st.pop_back();
        }
        return memo[rn] ^ (root & 1u);
    }

    // The same, with a sparse memo: for small cones of a large graph.
    template<typename Map>
    Lit rebuildSparse(Lit root, Map& memo) {
        uint32_t rn = nodeOf(root);
        if (rn == 0) return root;
        std::vector<uint32_t> st{rn};
        while (!st.empty()) {
            uint32_t n = st.back();
            if (memo.count(n)) { st.pop_back(); continue; }
            Node nd = nodes_[n];
            if (nd.a == kNoLit) { memo[n] = Lit(n << 1); st.pop_back(); continue; }
            uint32_t na = nodeOf(nd.a), nb = nodeOf(nd.b);
            auto ia = memo.find(na), ib = memo.find(nb);
            bool ready = true;
            if (na && ia == memo.end()) { st.push_back(na); ready = false; }
            if (nb && ib == memo.end()) { st.push_back(nb); ready = false; }
            if (!ready) continue;
            Lit a = na ? ia->second ^ (nd.a & 1u) : nd.a;
            Lit b = nb ? ib->second ^ (nd.b & 1u) : nd.b;
            Lit r = and_(a, b);
            memo[n] = r;
            st.pop_back();
        }
        return memo[rn] ^ (root & 1u);
    }

private:
    static size_t hash(uint64_t k) {
        k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33;
        return size_t(k);
    }
    void grow() {
        std::vector<uint32_t> t(table_.size() * 4, 0);
        size_t mask = t.size() - 1;
        for (uint32_t n : table_) {
            if (!n) continue;
            size_t h = hash((uint64_t(nodes_[n].a) << 32) | nodes_[n].b) & mask;
            while (t[h]) h = (h + 1) & mask;
            t[h] = n;
        }
        table_.swap(t);
    }

    std::vector<Node> nodes_;
    std::vector<uint32_t> table_;
    std::vector<uint32_t> stack_;
    size_t ands_ = 0;
};

}  // namespace equiv
