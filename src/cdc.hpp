#pragma once
// cdc.hpp - clock-domain (CDC) and reset-domain (RDC) crossing checks for
// mlint, on the elaborated design.
//
// The model is a signal graph. Every variable or net is one node, and the
// nodes a port connects are one node (a union-find). A node is a flop (written
// in an edge-triggered block), a combinational node (a continuous assign, an
// always_comb, a port connected to an expression), or a root nobody drives (a
// primary input). A flop has a clock domain, an asynchronous reset if it has
// one, and the signals its next value depends on: data (the right-hand sides)
// and control (the conditions around the assignment).
//
// A crossing is a flop in one clock domain whose next value depends, through
// combinational logic only, on a flop in another. What is recognized as safe:
//   * a synchronizer: two or more flops in the reading domain, each a plain
//     copy of the one before, written as separate blocks, in one block (with
//     or without a reset), or as a shift register (s <= {s[0], d});
//   * an enable-qualified bus: the data crosses unsynchronized into a flop
//     that also depends on a synchronized control from the same source domain
//     (the mux and and-gate recirculation schemes);
//   * a Gray-coded bus through a multi-flop synchronizer (b ^ (b >> 1)).
//
// Rules
//   CDC001  a crossing with no synchronizer, or one flop only.
//   CDC002  a multi-bit bus through a multi-flop synchronizer, not Gray coded.
//   CDC003  combinational logic between the source flop and the synchronizer:
//           a glitch on it can be captured.
//   CDC004  reconvergence: signals from one domain, synchronized separately,
//           meet again in the reading domain, where they can be a cycle apart.
//   CDC005  one signal synchronized more than once into the same domain.
//   RDC001  an asynchronous reset generated in another clock domain: its
//           release is not synchronous to the flops it resets.
//   RDC002  one asynchronous reset used directly in several clock domains.
//   RDC003  a flop with one asynchronous reset feeds a flop with another, in
//           the same clock domain.
//   RDC004  (off by default) ... feeds a flop with no asynchronous reset.
//
// Constraints: -clock-group a,b says clocks are synchronous to each other
// (one domain); -static s says a signal does not change while it is read.
//
// Threads (src/parallel.hpp): the graph is built on one thread (the front end's AST is lazy and
// not thread safe). The fan-in cone of every flop, which is where the time
// goes on a large design, is traced on all the threads, each flop its own
// task with its own result slot; the rules then read the results in flop
// order, so the findings are the same whatever the thread count.
#include <chrono>

#include "lintir.hpp"
#include "parallel.hpp"
#include "slang/ast/EvalContext.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/ParameterSymbols.h"

namespace cdc {

using namespace lintir;

struct Options {
    std::vector<std::vector<std::string>> clockGroups;  // -clock-group a,b,c
    std::vector<std::string> statics;                   // -static sig
    bool report = false;                                // -cdc-report
    int threads = 1;                                    // -threads N

    // Every logical CPU unless -threads says otherwise; RDC004 is off until
    // -enable RDC004.
    void defaults(std::set<std::string>& disabled) {
        threads = (int)par::defaultThreads();
        disabled.insert("RDC004");
    }

    // Takes one command-line option if it is one of these. `next` returns the
    // option's value; `fwd` is what goes on to the front end.
    template<typename Next>
    bool take(const std::string& a, Next&& next, std::set<std::string>& disabled, std::vector<std::string>& fwd) {
        if (a == "-enable") { disabled.erase(next("a rule name")); return true; }
        if (a == "-cdc-report") { report = true; return true; }
        if (a == "-static") { statics.push_back(next("a signal")); return true; }
        if (a == "-clock-group") {
            std::string list = next("clock names"), one;
            std::vector<std::string> g;
            for (char c : list + ",") {
                if (c != ',') { one += c; continue; }
                if (!one.empty()) g.push_back(one);
                one.clear();
            }
            clockGroups.push_back(g);
            return true;
        }
        if (a == "-threads") {
            std::string n = next("a count");
            if (n == "all") {
                threads = (int)par::defaultThreads();
            } else {
                try { threads = std::stoi(n); } catch (...) { threads = 0; }
                if (threads < 1) {
                    std::cerr << "mlint: bad -threads " << n << "\n";
                    std::exit(2);
                }
            }
            // The front end parses its files on the same number of threads.
            fwd.push_back("--threads");
            fwd.push_back(std::to_string(threads));
            return true;
        }
        return false;
    }
};

inline const char* usage() {
    return "  -enable RULE   turn on a rule that is off by default (RDC004)\n"
           "  -clock-group a,b  these clocks are synchronous to each other: one domain (repeatable)\n"
           "  -static SIG    SIG does not change while it is read: its crossings are not reported\n"
           "  -cdc-report    list the clock domains, the resets and every crossing, safe or not\n"
           "  -threads N     worker threads, or all (default: every logical CPU; 1 = none)\n";
}

inline const char* rules() {
    return "CDC001  a clock-domain crossing with no synchronizer, or only one flop\n"
           "CDC002  a multi-bit bus crossing through a synchronizer, not Gray coded\n"
           "CDC003  combinational logic in front of a synchronizer (a glitch can be captured)\n"
           "CDC004  separately synchronized signals of one domain meet again (reconvergence)\n"
           "CDC005  one signal synchronized more than once into the same domain\n"
           "RDC001  an asynchronous reset generated in another clock domain\n"
           "RDC002  one asynchronous reset used directly in several clock domains\n"
           "RDC003  a flop feeds a flop with a different asynchronous reset, in one clock domain\n"
           "RDC004  a reset flop feeds a flop with no asynchronous reset (off by default)\n";
}

using Sym = const Symbol*;
using SymSet = std::vector<Sym>;  // sorted, unique after seal()

inline void seal(SymSet& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}
inline bool has(const SymSet& v, Sym s) { return std::binary_search(v.begin(), v.end(), s); }

inline bool isSignal(const Symbol& s) {
    return s.kind == SymbolKind::Variable || s.kind == SymbolKind::Net;
}

inline const Expression& strip(const Expression& e) {
    const Expression* p = &e;
    while (p->kind == ExpressionKind::Conversion) p = &p->as<ConversionExpression>().operand();
    return *p;
}

// The variable or net a (possibly selected) reference names: x, x[3], x.f.
inline Sym baseOf(const Expression& ex) {
    const Expression* e = &strip(ex);
    for (;;) {
        switch (e->kind) {
            case ExpressionKind::NamedValue: return &e->as<NamedValueExpression>().symbol;
            case ExpressionKind::HierarchicalValue: return &e->as<HierarchicalValueExpression>().symbol;
            case ExpressionKind::ElementSelect: e = &strip(e->as<ElementSelectExpression>().value()); break;
            case ExpressionKind::RangeSelect: e = &strip(e->as<RangeSelectExpression>().value()); break;
            case ExpressionKind::MemberAccess: e = &strip(e->as<MemberAccessExpression>().value()); break;
            default: return nullptr;
        }
    }
}

// b ^ (b >> 1), in either order: the binary-to-Gray conversion.
inline bool isGrayOp(const Expression& ex) {
    const Expression& e = strip(ex);
    if (e.kind != ExpressionKind::BinaryOp) return false;
    const auto& b = e.as<BinaryExpression>();
    if (b.op != BinaryOperator::BinaryXor) return false;
    auto shr = [](const Expression& x) {
        const Expression& s = strip(x);
        if (s.kind != ExpressionKind::BinaryOp) return false;
        auto op = s.as<BinaryExpression>().op;
        return op == BinaryOperator::LogicalShiftRight || op == BinaryOperator::ArithmeticShiftRight;
    };
    return shr(b.left()) || shr(b.right());
}

// ------------------------------------------------------------------ the graph

struct Flop {
    Sym sym = nullptr;        // as the design names it, for messages
    Sym self = nullptr;       // canonical
    Sym clkSym = nullptr;     // the clock as written
    Sym dom = nullptr;        // canonical clock domain (set by Model::finish)
    SymSet arst;              // canonical asynchronous resets
    SourceLocation loc;
    SymSet data, ctrl;        // canonical, without self and without the resets
    // A plain copy: every right-hand side is a constant, the flop itself, or
    // one other signal `from`.
    bool plain = true;
    bool shifts = false;      // the flop itself is on a right-hand side
    Sym from = nullptr;
    uint64_t fromBits = 0;
    uint64_t bits = 1;
    std::vector<const Expression*> rhs;
    mutable std::string pathName;
};

// What drives a combinational node. An edge is a wire (assign y = x, a port
// connected to x or to x[3]) or logic.
struct Edge {
    Sym sym;
    bool logic;
};
struct Comb {
    std::vector<Edge> src;    // sorted, unique after Model::sealCombs
    int drivers = 0;
    const Expression* rhs = nullptr;  // the right-hand side, when there is one driver
    Sym sole() const { return src.size() == 1 ? src[0].sym : nullptr; }
};

// One signal a flop depends on, traced back through combinational logic.
struct Root {
    Sym sym;
    bool logic;  // some path to it goes through logic, not just wires
    bool operator<(const Root& o) const { return sym < o.sym; }
};

struct Model {
    // Not lintir's union-find: that one joins a port to the bus when the
    // connection is one bit of it, which makes clk[0] and clk[1] one clock.
    Join join;
    // One bit of a bus, as ports are connected to it: (bus, index) -> node.
    std::map<std::pair<Sym, int64_t>, Sym> bitOf;
    std::map<std::pair<Sym, int64_t>, Sym> bitCanon;  // the same, by canonical bus
    std::set<std::pair<Sym, int64_t>> bitDriven;      // bits a port or an assign drives
    std::unordered_set<Sym> bitNodes;                 // canonical, after seal
    std::vector<std::pair<Sym, std::string>> labels;  // a bit node's name: top.clk[2]
    std::unordered_map<Sym, int> flopIx;
    std::vector<Flop> flops;  // in source order of first assignment
    std::unordered_map<Sym, Comb> combs;
    std::unordered_map<Sym, int> labelDepth;
    std::unordered_map<Sym, Sym> group;  // clock -> its group's first clock
    std::unordered_set<Sym> statics;

    Sym canon(Sym s) { return join.find(s); }
    // A node is named by the outermost signal joined into it: the top's clk,
    // not u_core.u_regs.clk_i.
    // Building a hierarchical path is slow, and a clock has a symbol in every
    // module it reaches, so the candidates are ranked by depth first and a
    // name is only built for a node that a message mentions.
    struct Names {
        int depth = 1 << 30;
        std::vector<Sym> syms;            // the shallowest signals of the node
        std::vector<std::string> labels;  // ...and bit labels at that depth
    };
    std::unordered_map<Sym, Names> named;
    mutable std::unordered_map<Sym, std::string> shown;

    static int depth(Sym s) {
        int n = 0;
        for (const Scope* sc = s->getParentScope(); sc;) {
            const Symbol& o = sc->asSymbol();
            n++;
            if (o.kind == SymbolKind::InstanceBody) {
                const InstanceSymbol* in = o.as<InstanceBodySymbol>().parentInstance;
                sc = in ? in->getParentScope() : nullptr;
            } else {
                sc = o.getParentScope();
            }
        }
        return n;
    }
    void nameNodes() {
        std::vector<Sym> all;
        for (auto& [s, up] : join.up) all.push_back(s);
        auto offer = [&](Sym c, int d) -> Names* {
            Names& n = named[c];
            if (d > n.depth) return nullptr;
            if (d < n.depth) {
                n.depth = d;
                n.syms.clear();
                n.labels.clear();
            }
            return &n;
        };
        for (Sym s : all)
            if (isSignal(*s))
                if (Names* n = offer(join.find(s), depth(s)); n && n->syms.size() < 16) n->syms.push_back(s);
        for (auto& [s, label] : labels) {
            Sym c = join.find(s);
            bitNodes.insert(c);
            if (Names* n = offer(c, labelDepth[s])) n->labels.push_back(label);
        }
    }
    const std::string& name(Sym c) const {
        auto [it, fresh] = shown.try_emplace(c);
        if (!fresh) return it->second;
        std::string best;
        auto nit = named.find(c);
        if (nit == named.end()) return it->second = path(*c);
        for (Sym s : nit->second.syms)
            if (std::string p = path(*s); best.empty() || p < best) best = std::move(p);
        for (const std::string& p : nit->second.labels)
            if (best.empty() || p < best) best = p;
        return it->second = best.empty() ? path(*c) : best;
    }
    // A flop's own name, as its module wrote it.
    const std::string& name(const Flop& f) const {
        if (f.pathName.empty()) f.pathName = path(*f.sym);
        return f.pathName;
    }

    Flop& flop(Sym target, SourceLocation loc) {
        Sym c = canon(target);
        auto [it, fresh] = flopIx.try_emplace(c, (int)flops.size());
        if (fresh) {
            Flop f;
            f.sym = target;
            f.self = c;
            f.loc = loc;
            if (auto* v = target->as_if<ValueSymbol>()) f.bits = std::max<uint64_t>(1, v->getType().getBitstreamWidth());
            flops.push_back(std::move(f));
        }
        return flops[it->second];
    }
    const Flop* flopOf(Sym c) const {
        auto it = flopIx.find(c);
        return it == flopIx.end() ? nullptr : &flops[it->second];
    }
    const Comb* combOf(Sym c) const {
        auto it = combs.find(c);
        return it == combs.end() ? nullptr : &it->second;
    }

    void addComb(Sym target, const SymSet& src, bool plain, const Expression* rhs) {
        Sym t = canon(target);
        Comb& c = combs[t];
        for (Sym s : src)
            if (s != t) c.src.push_back({s, !plain});
        c.drivers++;
        c.rhs = c.drivers == 1 ? rhs : nullptr;
    }

    // A clock through buffers, inverters and clock gates is still that clock:
    // follow a combinational node with one source, or with one source that no
    // logic drives (clk & en_latched: the latch has inputs, the clock has none).
    Sym clockRoot(Sym c) const {
        for (int guard = 0; guard < 32; guard++) {
            if (flopIx.count(c) || bitNodes.count(c)) return c;
            const Comb* n = combOf(c);
            if (!n) return c;
            Sym next = n->sole();
            int free = next ? 1 : 0;
            if (!next)
                for (const Edge& e : n->src) {
                    const Comb* sc = combOf(e.sym);
                    if (!flopIx.count(e.sym) && (!sc || sc->src.empty())) { next = e.sym; free++; }
                }
            if (free != 1) return c;
            c = next;
        }
        return c;
    }
    Sym domainOf(Sym clk) const {
        Sym r = clockRoot(clk);
        auto g = group.find(r);
        return g == group.end() ? r : g->second;
    }

    // The reset a reset comes from: through wires and inverters, and through a
    // reset synchronizer (a flop that is itself asynchronously reset asserts
    // when its own reset does).
    Sym resetRoot(Sym c) const {
        for (int guard = 0; guard < 32; guard++) {
            if (const Flop* f = flopOf(c)) {
                if (f->arst.empty()) return c;
                c = f->arst[0];
                continue;
            }
            const Comb* n = bitNodes.count(c) ? nullptr : combOf(c);
            if (!n || !n->sole()) return c;
            c = n->sole();
        }
        return c;
    }
    // The same, but stopping at the first flop.
    Sym wireRoot(Sym c) const {
        for (int guard = 0; guard < 32; guard++) {
            if (flopIx.count(c) || bitNodes.count(c)) return c;
            const Comb* n = combOf(c);
            if (!n || !n->sole()) return c;
            c = n->sole();
        }
        return c;
    }

    // The one signal a signal is a function of, through wires and
    // single-input logic, or itself.
    Sym soleSource(Sym c) const {
        for (int guard = 0; guard < 8; guard++) {
            const Comb* n = flopIx.count(c) ? nullptr : combOf(c);
            if (!n || !n->sole()) return c;
            c = n->sole();
        }
        return c;
    }
    // Binary to Gray: x ^ (x >> 1), or x ^ y where y is x moved down by a
    // select or a concatenation.
    bool grayOp(const Expression& ex) {
        const Expression& e = strip(ex);
        if (isGrayOp(e)) return true;
        if (e.kind != ExpressionKind::BinaryOp) return false;
        const auto& b = e.as<BinaryExpression>();
        if (b.op != BinaryOperator::BinaryXor) return false;
        Sym l = baseOf(b.left()), r = baseOf(b.right());
        if (!l || !r || !isSignal(*l) || !isSignal(*r)) return false;
        bool moved = strip(b.left()).kind != strip(b.right()).kind || canon(l) != canon(r);
        return moved && soleSource(canon(l)) == soleSource(canon(r));
    }
    bool gray(const Flop& f) {
        for (const Expression* e : f.rhs) {
            if (grayOp(*e)) return true;
            if (Sym b = baseOf(*e)) {
                Sym c = canon(b);
                for (int guard = 0; guard < 4; guard++) {
                    const Comb* n = combOf(c);
                    if (!n || !n->rhs) break;
                    if (grayOp(*n->rhs)) return true;
                    if (!n->sole()) break;
                    c = n->sole();
                }
            }
        }
        return false;
    }

    void sealCombs() {
        for (auto& [s, c] : combs) {
            std::sort(c.src.begin(), c.src.end(), [](const Edge& a, const Edge& b) {
                return a.sym != b.sym ? a.sym < b.sym : a.logic > b.logic;
            });
            c.src.erase(std::unique(c.src.begin(), c.src.end(),
                                    [](const Edge& a, const Edge& b) { return a.sym == b.sym; }),
                        c.src.end());
        }
    }
    void finish() {
        for (Flop& f : flops) {
            seal(f.arst);
            auto drop = [&](SymSet& v) {
                seal(v);
                std::erase_if(v, [&](Sym s) { return s == f.self || has(f.arst, s); });
            };
            drop(f.data);
            drop(f.ctrl);
            if (f.clkSym) f.dom = domainOf(canon(f.clkSym));
            if (f.from && has(f.arst, f.from)) f.plain = false;
        }
    }
};

// --------------------------------------------------------------- the gather

struct Walk {
    Model& m;
    const Scope* scope = nullptr;  // of a continuous assign, to evaluate a bit index
    bool seq = false;
    Sym clk = nullptr;
    SymSet arst;  // canonical

    void names(const Expression& e, SymSet& out) {
        e.visit(makeVisitor(
            [&](auto&, const NamedValueExpression& n) { if (isSignal(n.symbol)) out.push_back(m.canon(&n.symbol)); },
            [&](auto&, const HierarchicalValueExpression& n) { if (isSignal(n.symbol)) out.push_back(m.canon(&n.symbol)); }));
    }

    static bool local(Sym s) {
        const Scope* sc = s->getParentScope();
        return sc && sc->asSymbol().kind == SymbolKind::StatementBlock;
    }

    // The leaves of a right-hand side under its ?: operators, whose conditions
    // are control, not data.
    void leaves(const Expression& ex, SymSet& ctrl, std::vector<const Expression*>& out) {
        const Expression& e = strip(ex);
        if (e.kind == ExpressionKind::ConditionalOp) {
            const auto& c = e.as<ConditionalExpression>();
            for (auto& cond : c.conditions) names(*cond.expr, ctrl);
            leaves(c.left(), ctrl, out);
            leaves(c.right(), ctrl, out);
        } else {
            out.push_back(&e);
        }
    }

    struct Terms {
        bool other = false, self = false;
        Sym ext = nullptr;
        bool many = false;
        uint64_t bits = 0;
    };
    void terms(const Expression& ex, Sym self, Terms& t) {
        const Expression& e = strip(ex);
        switch (e.kind) {
            case ExpressionKind::IntegerLiteral:
            case ExpressionKind::UnbasedUnsizedIntegerLiteral:
            case ExpressionKind::RealLiteral:
            case ExpressionKind::StringLiteral:
                return;
            case ExpressionKind::Concatenation:
                for (const Expression* o : e.as<ConcatenationExpression>().operands()) terms(*o, self, t);
                return;
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
            case ExpressionKind::ElementSelect:
            case ExpressionKind::RangeSelect:
            case ExpressionKind::MemberAccess: {
                Sym b = baseOf(e);
                if (!b) { t.other = true; return; }
                if (!isSignal(*b)) return;  // a parameter or an enum value
                Sym c = m.canon(b);
                if (c == self) { t.self = true; return; }
                if (t.ext && t.ext != c) t.many = true;
                t.ext = c;
                t.bits += e.type->getBitstreamWidth();
                return;
            }
            default:
                SymSet n;
                names(e, n);
                if (!n.empty()) t.other = true;  // else a constant expression
        }
    }

    void one(const Expression& lhs, const Expression& rhs, bool nonBlocking, const SymSet& ctrl0, bool whole) {
        Sym base = baseOf(lhs);
        if (!base || !isSignal(*base)) return;
        Sym self = m.canon(base);
        SymSet ctrl = ctrl0;
        // A select index on the left chooses what is written: control.
        {
            SymSet ln;
            names(lhs, ln);
            for (Sym s : ln)
                if (s != self) ctrl.push_back(s);
        }
        std::vector<const Expression*> lv;
        leaves(rhs, ctrl, lv);
        if (seq && (nonBlocking || !local(base))) {
            Flop& f = m.flop(base, lhs.sourceRange.start());
            if (!f.clkSym) {
                f.clkSym = clk;
                f.arst = arst;
            }
            f.ctrl.insert(f.ctrl.end(), ctrl.begin(), ctrl.end());
            for (const Expression* e : lv) {
                names(*e, f.data);
                Terms t;
                terms(*e, self, t);
                if (!whole && (t.ext || t.self)) t.other = true;  // {a, b} <= '0 is still plain
                if (t.other || t.many) f.plain = false;
                if (t.self) f.shifts = true;
                if (t.ext) {
                    if (f.from && f.from != t.ext) f.plain = false;
                    f.from = t.ext;
                    f.fromBits = std::max(f.fromBits, t.bits);
                }
                f.rhs.push_back(e);
            }
        } else {
            SymSet src = ctrl;
            for (const Expression* e : lv) names(*e, src);
            bool plain = !seq && ctrl.empty() && lv.size() == 1 && whole && baseOf(*lv[0]) != nullptr;
            m.addComb(base, src, plain, lv.size() == 1 ? lv[0] : nullptr);
            // assign bus[3] = x, where ports are connected to bus[3]: that bit
            // is x, not the whole bus.
            const Expression& l = strip(lhs);
            if (scope && l.kind == ExpressionKind::ElementSelect && !m.bitCanon.empty()) {
                const auto& sel = l.as<ElementSelectExpression>();
                const Expression& v = strip(sel.value());
                if (v.kind == ExpressionKind::NamedValue || v.kind == ExpressionKind::HierarchicalValue) {
                    EvalContext ctx(ASTContext(*scope, LookupLocation::max));
                    ConstantValue cv = sel.selector().eval(ctx);
                    if (auto ix = cv.isInteger() ? cv.integer().as<int64_t>() : std::nullopt) {
                        auto it = m.bitCanon.find({self, *ix});
                        if (it != m.bitCanon.end()) {
                            m.addComb(it->second, src, plain, nullptr);
                            m.bitDriven.insert({self, *ix});
                        }
                    }
                }
            }
        }
    }

    void assign(const AssignmentExpression& a, const SymSet& ctrl) {
        const Expression& l = strip(a.left());
        const Expression& r = strip(a.right());
        if (l.kind == ExpressionKind::Concatenation) {
            auto lo = l.as<ConcatenationExpression>().operands();
            if (r.kind == ExpressionKind::Concatenation) {
                auto ro = r.as<ConcatenationExpression>().operands();
                bool match = lo.size() == ro.size();
                for (size_t i = 0; match && i < lo.size(); i++)
                    match = lo[i]->type->getBitstreamWidth() == ro[i]->type->getBitstreamWidth();
                if (match) {  // {s2, s1} <= {s1, d}: two assignments
                    for (size_t i = 0; i < lo.size(); i++) one(*lo[i], *ro[i], a.isNonBlocking(), ctrl, true);
                    return;
                }
            }
            for (const Expression* o : lo) one(*o, r, a.isNonBlocking(), ctrl, false);
            return;
        }
        one(l, r, a.isNonBlocking(), ctrl, true);
    }

    void stmt(const Statement& s, const SymSet& ctrl) {
        switch (s.kind) {
            case StatementKind::List:
                for (const Statement* c : s.as<StatementList>().list) stmt(*c, ctrl);
                break;
            case StatementKind::Block: stmt(s.as<BlockStatement>().body, ctrl); break;
            case StatementKind::Timed: stmt(s.as<TimedStatement>().stmt, ctrl); break;
            case StatementKind::ExpressionStatement: {
                const Expression& e = s.as<ExpressionStatement>().expr;
                if (e.kind == ExpressionKind::Assignment) assign(e.as<AssignmentExpression>(), ctrl);
                break;
            }
            case StatementKind::Conditional: {
                const auto& c = s.as<ConditionalStatement>();
                SymSet in = ctrl;
                for (auto& cond : c.conditions) names(*cond.expr, in);
                stmt(c.ifTrue, in);
                if (c.ifFalse) stmt(*c.ifFalse, in);
                break;
            }
            case StatementKind::Case: {
                const auto& c = s.as<CaseStatement>();
                SymSet in = ctrl;
                names(c.expr, in);
                for (auto& it : c.items)
                    for (const Expression* e : it.expressions) names(*e, in);
                for (auto& it : c.items) stmt(*it.stmt, in);
                if (c.defaultCase) stmt(*c.defaultCase, in);
                break;
            }
            default: {
                // Loops and the rest: every expression that is not an
                // assignment is control for every assignment inside.
                SymSet in = ctrl;
                std::vector<const AssignmentExpression*> as;
                s.visit(makeVisitor(
                    [&](auto& v, std::derived_from<Statement> auto& inner) { v.visitDefault(inner); },
                    [&](auto&, const Expression& e) {
                        if (e.kind == ExpressionKind::Assignment)
                            as.push_back(&e.as<AssignmentExpression>());
                        else
                            names(e, in);
                    }));
                for (auto* a : as) assign(*a, in);
                break;
            }
        }
    }
};

// The first pass: what the ports connect. It has to finish before any node is
// looked up, because joining two nodes changes which symbol stands for them.
struct Ports : ASTVisitor<Ports> {
    Model& m;
    struct Wire {
        Sym to;
        SymSet from;
        bool plain;
        const Expression* rhs;
    };
    std::vector<Wire> wires;
    struct Bit {
        Sym bus;
        int64_t index;
        Sym node;
        bool input;
    };
    std::vector<Bit> bits;
    explicit Ports(Model& mm) : m(mm) {}

    static void names(const Expression& e, SymSet& out) {
        e.visit(makeVisitor(
            [&](auto&, const NamedValueExpression& n) { if (isSignal(n.symbol)) out.push_back(&n.symbol); },
            [&](auto&, const HierarchicalValueExpression& n) { if (isSignal(n.symbol)) out.push_back(&n.symbol); }));
    }

    void handle(const GenerateBlockSymbol& g) {
        if (g.isUninstantiated) return;
        visitDefault(g);
    }
    void handle(const ProceduralBlockSymbol&) {}
    void handle(const ContinuousAssignSymbol&) {}

    void handle(const InstanceSymbol& inst) {
        if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return;
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn || conn->port.kind != SymbolKind::Port) continue;
            const auto& port = conn->port.as<PortSymbol>();
            Sym in = port.internalSymbol;
            const Expression* ext = conn->getExpression();
            if (!ext || !in || !isSignal(*in)) continue;
            bool input = port.direction == ArgumentDirection::In;
            const Expression* outer = ext;
            if (outer->kind == ExpressionKind::Assignment) outer = &outer->as<AssignmentExpression>().left();
            const Expression& o = strip(*outer);
            Sym base = baseOf(o);
            if (base && !isSignal(*base)) base = nullptr;
            if (base && (o.kind == ExpressionKind::NamedValue || o.kind == ExpressionKind::HierarchicalValue)) {
                m.join.join(base, in);  // the same wire
                continue;
            }
            // One bit of a bus: every port on that bit is one node, and the
            // bus is wired to it. clk[0] and clk[1] stay two clocks.
            if (base && o.kind == ExpressionKind::ElementSelect) {
                const auto& sel = o.as<ElementSelectExpression>();
                const Expression& v = strip(sel.value());
                if (v.kind == ExpressionKind::NamedValue || v.kind == ExpressionKind::HierarchicalValue) {
                    EvalContext ctx(ASTContext(*inst.getParentScope(), LookupLocation::max));
                    ConstantValue cv = sel.selector().eval(ctx);
                    if (cv.isInteger()) {
                        if (auto ix = cv.integer().as<int64_t>()) {
                            auto [it, fresh] = m.bitOf.try_emplace({base, *ix}, in);
                            if (fresh)
                            {
                                m.labels.push_back({in, path(*base) + "[" + std::to_string(*ix) + "]"});
                                m.labelDepth[in] = Model::depth(base);
                            }
                            else
                                m.join.join(it->second, in);
                            bits.push_back({base, *ix, in, input});
                            continue;
                        }
                    }
                }
            }
            SymSet n;
            names(*outer, n);
            if (n.empty()) continue;
            if (input)
                wires.push_back({in, n, base != nullptr, ext});  // a part select is still a wire
            else
                for (Sym t : n) wires.push_back({t, {in}, base != nullptr, nullptr});
        }
        visitDefault(inst);
    }

    // The bits of one bus are one node per (canonical bus, index), whichever
    // name of the bus the connection used.
    void finish() {
        for (auto& [key, node] : m.bitOf) {
            auto [it, fresh] = m.bitCanon.try_emplace({m.join.find(key.first), key.second}, node);
            if (!fresh) m.join.join(it->second, node);
        }
        for (const Bit& b : bits)
            if (!b.input) m.bitDriven.insert({m.join.find(b.bus), b.index});
        for (Wire& w : wires) {
            SymSet from;
            for (Sym s : w.from) from.push_back(m.canon(s));
            m.addComb(w.to, from, w.plain, w.rhs);
        }
    }
    // After the assigns are known: a bit a port or an assign drives is that
    // value, and the bus sees it. A bit nothing drives by itself is whatever
    // drives the bus.
    void finishBits() {
        for (const Bit& b : bits) {
            Sym bus = m.canon(b.bus);
            if (!b.input)
                m.addComb(b.bus, {m.canon(b.node)}, true, nullptr);
            else if (!m.bitDriven.count({bus, b.index}))
                m.addComb(b.node, {bus}, true, nullptr);
        }
    }
};

struct Gather : ASTVisitor<Gather> {
    Model& m;
    explicit Gather(Model& mm) : m(mm) {}

    void handle(const InstanceSymbol& inst) {
        // A module nobody instantiates is elaborated for its errors only.
        if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return;
        visitDefault(inst);
    }

    void handle(const GenerateBlockSymbol& g) {
        if (g.isUninstantiated) return;
        visitDefault(g);
    }

    void handle(const NetSymbol& n) {
        if (const Expression* init = n.getInitializer()) {  // wire x = a & b;
            Walk w{m};
            SymSet src;
            w.names(*init, src);
            if (!src.empty()) m.addComb(&n, src, baseOf(*init) != nullptr, init);
        }
    }

    void handle(const ContinuousAssignSymbol& ca) {
        const Expression& e = ca.getAssignment();
        if (e.kind != ExpressionKind::Assignment) return;
        Walk w{m};
        w.scope = ca.getParentScope();
        w.assign(e.as<AssignmentExpression>(), {});
    }

    void handle(const ProceduralBlockSymbol& p) {
        if (p.procedureKind == ProceduralBlockKind::Initial || p.procedureKind == ProceduralBlockKind::Final)
            return;
        Walk w{m};
        const Statement* inner = &p.getBody();
        if (inner->kind == StatementKind::Timed) {
            const auto& t = inner->as<TimedStatement>();
            std::vector<const SignalEventControl*> evs, edges;
            clocksOf(t.timing, evs);
            for (const SignalEventControl* e : evs)
                if (e->edge != EdgeKind::None) edges.push_back(e);
            if (!edges.empty()) {
                w.seq = true;
                // The edges the body reads are asynchronous resets (if (!rst_n)
                // ...); the one it does not read is the clock.
                SymSet read;
                t.stmt.visit(makeVisitor([&](auto&, const NamedValueExpression& n) { read.push_back(&n.symbol); },
                                         [&](auto&, const HierarchicalValueExpression& n) { read.push_back(&n.symbol); }));
                seal(read);
                for (const SignalEventControl* e : edges) {
                    Sym s = oneName(e->expr);
                    if (!s) continue;
                    if (has(read, s))
                        w.arst.push_back(m.canon(s));
                    else if (!w.clk)
                        w.clk = s;
                }
                if (!w.clk) {  // every edge is read: take the first, as lintir does
                    w.clk = oneName(edges[0]->expr);
                    if (w.clk) std::erase(w.arst, m.canon(w.clk));
                }
                if (!w.clk) w.seq = false;
            }
            inner = &t.stmt;
        }
        w.stmt(*inner, {});
    }
};

// ------------------------------------------------------------------ tracing

// The flops and primary inputs a signal depends on through combinational
// logic. One Tracer per thread: the memo is private, the model is read only.
struct Tracer {
    const Model& m;
    std::unordered_map<Sym, std::vector<Root>> memo;
    std::unordered_set<Sym> open;

    explicit Tracer(const Model& mm) : m(mm) {}

    static void merge(std::vector<Root>& into, const std::vector<Root>& from, bool logic) {
        for (Root r : from) {
            r.logic = r.logic || logic;
            into.push_back(r);
        }
    }
    static void tidy(std::vector<Root>& v) {
        std::sort(v.begin(), v.end(), [](const Root& a, const Root& b) {
            return a.sym != b.sym ? a.sym < b.sym : a.logic > b.logic;
        });
        v.erase(std::unique(v.begin(), v.end(), [](const Root& a, const Root& b) { return a.sym == b.sym; }),
                v.end());
    }

    const std::vector<Root>& roots(Sym c) {
        static const std::vector<Root> none;
        auto it = memo.find(c);
        if (it != memo.end()) return it->second;
        const Comb* n = m.flopIx.count(c) ? nullptr : m.combOf(c);
        if (!n) return memo[c] = {Root{c, false}};
        if (!open.insert(c).second) return none;  // a combinational loop
        std::vector<Root> out;
        for (const Edge& e : n->src) merge(out, roots(e.sym), e.logic);
        open.erase(c);
        tidy(out);
        return memo[c] = std::move(out);
    }
    std::vector<Root> rootsOf(const SymSet& v) {
        std::vector<Root> out;
        for (Sym s : v) merge(out, roots(s), false);
        tidy(out);
        return out;
    }
};

struct Cone {
    std::vector<Root> all, from;  // data and control together; the plain source alone
};

// Runs fn(begin, end) over [0, n) in chunks on the shared pool
// (parallel.hpp). Every index writes its own slot, so the result does not
// depend on the chunks or the schedule. A chunk is large enough to be worth
// a thread, and a small design runs on one.
template<typename F>
void chunked(size_t n, int threads, F&& fn) {
    size_t k = std::min<size_t>(std::max(1, threads), std::max<size_t>(1, n / 256));
    if (k <= 1) {
        if (n) fn(size_t(0), n);
        return;
    }
    size_t grain = std::max<size_t>(64, n / (k * 4));
    size_t count = (n + grain - 1) / grain;
    par::parallelFor(count, (unsigned)k, [&](size_t c) { fn(c * grain, std::min(n, (c + 1) * grain)); });
}

// MLINT_PROFILE=1 prints where the time went, to stderr.
struct Prof {
    bool on = std::getenv("MLINT_PROFILE") != nullptr;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    void lap(const char* what) {
        if (!on) return;
        auto t1 = std::chrono::steady_clock::now();
        std::cerr << "mlint: cdc " << what << " " << std::chrono::duration<double, std::milli>(t1 - t0).count()
                  << " ms\n";
        t0 = t1;
    }
};

// ------------------------------------------------------------------- checks

struct Checker {
    Model& m;
    Report& rep;
    const Options& opt;
    std::vector<Cone> cones;

    // A flop that is part of a synchronizer: which signal it carries, from
    // which domain, and how many flops deep it is.
    struct Member {
        Sym src = nullptr, srcDom = nullptr;
        int pos = 0;
    };
    std::vector<Member> member;
    std::vector<std::vector<int>> next;  // flop -> the plain flops that copy it

    struct Crossing {
        int flop;
        Sym src;
        std::string how;
    };
    std::vector<Crossing> crossings;

    Checker(Model& mm, Report& r, const Options& o) : m(mm), rep(r), opt(o) {}

    // Findings wait here and go to the report in one order, whatever order
    // the maps (keyed by address) produced them in.
    struct Pending {
        std::string rule, text;
        SourceLocation loc;
    };
    std::vector<Pending> pending;
    void add(std::string_view rule, SourceLocation loc, std::string text) {
        if (rep.rules.enabled(rule)) pending.push_back({std::string(rule), std::move(text), loc});
    }
    void flush() {
        std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
            return std::tie(a.rule, a.text) < std::tie(b.rule, b.text);
        });
        for (Pending& p : pending) rep.add(p.rule, p.loc, std::move(p.text));
    }

    const Flop* foreign(const Flop& f, Sym r) const {
        const Flop* s = m.flopOf(r);
        if (!s || !s->dom || !f.dom || s->dom == f.dom || m.statics.count(r)) return nullptr;
        return s;
    }
    int stages(const Flop& f) const {
        if (!f.shifts || !f.fromBits) return 1;
        return (int)std::clamp<uint64_t>(f.bits / f.fromBits, 1, 64);
    }
    int chainLen(int i, int guard = 0) const {
        int best = 0;
        if (guard < 8)
            for (int g : next[i]) best = std::max(best, chainLen(g, guard + 1));
        return stages(m.flops[i]) + best;
    }
    void mark(int i, Sym src, Sym srcDom, int before, int guard = 0) {
        int pos = before + stages(m.flops[i]);
        if (member[i].pos >= pos) return;
        member[i] = {src, srcDom, pos};
        if (guard < 8)
            for (int g : next[i]) mark(g, src, srcDom, pos, guard + 1);
    }

    std::string clocked(const Flop& f) const { return m.name(f.dom); }

    void run() {
        Prof prof;
        const size_t n = m.flops.size();
        cones.resize(n);
        {
            // One tracer, with its own memo, per chunk of flops.
            chunked(n, opt.threads, [&](size_t lo, size_t hi) {
                Tracer tr(m);
                for (size_t i = lo; i < hi; i++) {
                    const Flop& f = m.flops[i];
                    SymSet in = f.data;
                    in.insert(in.end(), f.ctrl.begin(), f.ctrl.end());
                    cones[i].all = tr.rootsOf(in);
                    if (f.plain && f.from) cones[i].from = tr.roots(f.from);
                }
            });
        }

        prof.lap("cones");

        // Synchronizer chains: s1 <= d; s2 <= s1; ...
        next.assign(n, {});
        member.assign(n, {});
        for (size_t i = 0; i < n; i++) {
            const Flop& f = m.flops[i];
            const auto& fr = cones[i].from;
            if (fr.size() != 1 || fr[0].logic) continue;
            const Flop* p = m.flopOf(fr[0].sym);
            if (p && p->dom && p->dom == f.dom && p != &f) next[p - m.flops.data()].push_back((int)i);
        }
        std::vector<char> first(n, 0);
        for (size_t i = 0; i < n; i++) {
            const Flop& f = m.flops[i];
            for (const Root& r : cones[i].from)
                if (const Flop* s = foreign(f, r.sym)) {
                    first[i] = 1;
                    mark((int)i, r.sym, s->dom, 0);
                    break;
                }
        }

        prof.lap("chains");
        cdc(first);
        prof.lap("crossings");
        reconverge(first);
        prof.lap("reconvergence");
        rdc();
        prof.lap("resets");
        flush();
        if (opt.report) report();
        prof.lap("report");
    }

    void cdc(const std::vector<char>& first) {
        struct Stage {
            int flop, len;
            bool logic;
            std::string where;
        };
        // (source, reading domain) -> first stages. Ordered by flop index, not
        // by pointer, so the report does not depend on the allocator.
        auto byFlop = [&](const std::pair<Sym, Sym>& a, const std::pair<Sym, Sym>& b) {
            auto ix = [&](Sym x) { auto it = m.flopIx.find(x); return it == m.flopIx.end() ? -1 : it->second; };
            int ia = ix(a.first), ib = ix(b.first);
            return ia != ib ? ia < ib : a.second < b.second;
        };
        std::map<std::pair<Sym, Sym>, std::vector<Stage>, decltype(byFlop)> syncs(byFlop);
        Prof prof;
        // Classify every crossing, each flop on its own: no strings yet.
        enum Kind : uint8_t { Qualified, OneFlop, Synced, Bare };
        struct Rec {
            Sym src;
            Kind kind;
            bool logic;
            int len;
        };
        const size_t n = m.flops.size();
        std::vector<std::vector<Rec>> recs(n);
        chunked(n, opt.threads, [&](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi; i++) {
                const Flop& f = m.flops[i];
                if (!f.dom) continue;
                const std::vector<Root>& all = cones[i].all;
                auto inFrom = [&](Sym s) {
                    for (const Root& r : cones[i].from)
                        if (r.sym == s) return true;
                    return false;
                };
                // A bus may cross with no synchronizer of its own when a synchronized
                // control from its own domain says when to take it: the domains
                // this flop has such a control from.
                SymSet quals;
                for (const Root& q : all) {
                    const Flop* qf = m.flopOf(q.sym);
                    if (!qf || qf->dom != f.dom) continue;
                    const Member& mem = member[qf - m.flops.data()];
                    if (mem.pos >= 2) quals.push_back(mem.srcDom);
                }
                for (const Root& r : all) {
                    const Flop* s = foreign(f, r.sym);
                    if (!s) continue;
                    bool qualified = std::find(quals.begin(), quals.end(), s->dom) != quals.end();
                    if (qualified)
                        recs[i].push_back({r.sym, Qualified, r.logic, 0});
                    else if (first[i] && inFrom(r.sym)) {
                        int len = chainLen((int)i);
                        recs[i].push_back({r.sym, len < 2 ? OneFlop : Synced, r.logic, len});
                    } else
                        recs[i].push_back({r.sym, Bare, r.logic, 0});
                }
            }
        });
        prof.lap("  classify");
        // The names the messages need, built on every thread: a hierarchical
        // path is the expensive part of a finding.
        {
            std::vector<char> need(n, 0);
            for (size_t i = 0; i < n; i++)
                for (const Rec& r : recs[i]) need[i] = need[m.flopIx.at(r.src)] = 1;
            chunked(n, opt.threads, [&](size_t lo, size_t hi) {
                for (size_t i = lo; i < hi; i++)
                    if (need[i]) m.name(m.flops[i]);
            });
        }
        prof.lap("  names");
        for (size_t i = 0; i < n; i++) {
            const Flop& f = m.flops[i];
            for (const Rec& r : recs[i]) {
                const Flop& s = *m.flopOf(r.src);
                if (r.kind == Qualified) {
                    crossings.push_back({(int)i, r.src, "enable-qualified"});
                    continue;
                }
                std::string where = "'" + m.name(s) + "' (clocked by " + clocked(s) +
                                    ") is read by logic clocked by " + clocked(f);
                if (r.kind == Synced) {
                    syncs[{r.src, f.dom}].push_back({(int)i, r.len, r.logic, std::move(where)});
                } else if (r.kind == OneFlop) {
                    add("CDC001", f.loc,
                        where + " through one flop only; a single stage still settles too late, "
                                "so use two");
                    crossings.push_back({(int)i, r.src, "one flop only (CDC001)"});
                } else {
                    add("CDC001", f.loc,
                        where + " with no synchronizer; two flops in the reading domain are the "
                                "minimum for a one-bit signal");
                    crossings.push_back({(int)i, r.src, "no synchronizer (CDC001)"});
                }
            }
        }
        prof.lap("  emit");
        // The synchronizers of one signal into one domain, together: a bus
        // may be synchronized bit by bit, which is one crossing of the bus.
        for (auto& [key, list] : syncs) {
            const Flop& s = *m.flopOf(key.first);
            uint64_t total = 0, widest = 0;
            for (const Stage& st : list) {
                total += m.flops[st.flop].fromBits;
                widest = std::max(widest, m.flops[st.flop].fromBits);
            }
            bool fanout = list.size() > 1 && total > s.bits;
            bool gray = m.gray(s);
            bool bus = !gray && (widest > 1 || (total > 1 && !fanout));
            bool busSaid = false;
            for (const Stage& st : list) {
                const Flop& f = m.flops[st.flop];
                std::string sync = std::to_string(st.len) + "-flop synchronizer";
                if (st.logic) {
                    add("CDC003", f.loc,
                            st.where + " through combinational logic in front of the synchronizer '" +
                                m.name(f) + "': a glitch on it can be captured, so register the "
                                               "signal in its own domain first");
                    sync += ", logic in front (CDC003)";
                } else if (bus) {
                    if (!busSaid)
                        add("CDC002", f.loc,
                                st.where + " through " + std::to_string(st.len) + " flops, but it is " +
                                    std::to_string(fanout ? widest : total) +
                                    " bits wide: the bits settle in different cycles, so use a handshake, "
                                    "a FIFO, or Gray coding");
                    busSaid = true;
                    sync += ", multi-bit (CDC002)";
                } else if (total > 1 && gray) {
                    sync += ", Gray coded";
                }
                crossings.push_back({st.flop, key.first, sync});
            }
            if (!fanout) continue;
            std::string names;
            for (const Stage& st : list) names += (names.empty() ? "'" : ", '") + m.name(m.flops[st.flop]) + "'";
            add("CDC005", m.flops[list[1].flop].loc,
                    "'" + m.name(s) + "' (clocked by " + clocked(s) + ") is synchronized " +
                        std::to_string(list.size()) + " times into the domain of " + m.name(key.second) +
                        " (" + names + "): the copies can differ for a cycle, so synchronize once and "
                                       "share the result");
        }
    }

    // CDC004: two signals of one domain, each through its own synchronizer,
    // meet in one flop. Each synchronizer can take one cycle more or less, so
    // the pair can show a combination its source never had.
    void reconverge(const std::vector<char>& first) {
        for (size_t i = 0; i < m.flops.size(); i++) {
            const Flop& f = m.flops[i];
            if (!f.dom || first[i]) continue;
            const std::vector<Root>& all = cones[i].all;
            std::map<Sym, std::vector<std::pair<std::string, Sym>>> byDom;  // source domain -> (name, source)
            for (const Root& r : all) {
                const Flop* sf = m.flopOf(r.sym);
                if (!sf || sf->dom != f.dom) continue;
                const Member& mem = member[sf - m.flops.data()];
                if (mem.pos < 2) continue;
                auto& v = byDom[mem.srcDom];
                bool dup = false;
                for (auto& e : v) dup = dup || e.second == mem.src;
                if (!dup) v.push_back({m.name(*m.flopOf(mem.src)), mem.src});
            }
            for (auto& [d, v] : byDom) {
                if (v.size() < 2) continue;
                std::sort(v.begin(), v.end());
                std::string names;
                for (auto& e : v) names += (names.empty() ? "'" : ", '") + e.first + "'";
                add("CDC004", f.loc,
                        names + " (clocked by " + m.name(d) + ") are synchronized separately and meet " +
                            "again at '" + m.name(f) + "': each can arrive a cycle apart from the " +
                            "others, so cross one signal (or a Gray code) and derive the rest from it");
            }
        }
    }

    void rdc() {
        // Flops that make a reset (a reset synchronizer and the flops in front
        // of it) are where a raw reset is supposed to go.
        std::vector<char> makesReset(m.flops.size(), 0);
        for (const Flop& f : m.flops)
            for (Sym a : f.arst)
                if (const Flop* g = m.flopOf(m.wireRoot(a))) makesReset[g - m.flops.data()] = 1;
        for (bool again = true; again;) {
            again = false;
            for (size_t i = 0; i < m.flops.size(); i++) {
                if (!makesReset[i] || !m.flops[i].from) continue;
                const Flop* g = m.flopOf(m.wireRoot(m.flops[i].from));
                if (g && !makesReset[g - m.flops.data()]) makesReset[g - m.flops.data()] = again = 1;
            }
        }

        struct Use {
            int flops = 0;
            SourceLocation loc;
        };
        std::map<std::pair<Sym, Sym>, Use> gen;                  // RDC001: (reset flop, reading domain)
        std::map<Sym, std::vector<std::pair<Sym, SourceLocation>>> raw;  // RDC002: reset -> domains
        for (size_t i = 0; i < m.flops.size(); i++) {
            const Flop& f = m.flops[i];
            if (!f.dom) continue;
            for (Sym a : f.arst) {
                Sym r = m.wireRoot(a);
                if (const Flop* g = m.flopOf(r)) {
                    if (g->dom && g->dom != f.dom && !m.statics.count(r)) {
                        Use& u = gen[{r, f.dom}];
                        if (!u.flops++) u.loc = f.loc;
                    }
                } else if (!makesReset[i] && !m.statics.count(r)) {
                    auto& v = raw[r];
                    bool seen = false;
                    for (auto& e : v) seen = seen || e.first == f.dom;
                    if (!seen) v.push_back({f.dom, f.loc});
                }
            }
        }
        for (auto& [key, u] : gen) {
            const Flop& g = *m.flopOf(key.first);
            add("RDC001", u.loc,
                    "asynchronous reset '" + m.name(g) + "' (clocked by " + clocked(g) + ") resets " +
                        std::to_string(u.flops) + (u.flops == 1 ? " flop" : " flops") + " clocked by " +
                        m.name(key.second) + ": its release is not synchronous to that clock, so pass it "
                                             "through a reset synchronizer in that domain");
        }
        for (auto& [r, v] : raw) {
            if (v.size() < 2) continue;
            std::string names;
            for (auto& e : v) names += (names.empty() ? "" : ", ") + m.name(e.first);
            add("RDC002", v[1].second,
                    "asynchronous reset '" + m.name(r) + "' is used directly by flops in " +
                        std::to_string(v.size()) + " clock domains (" + names + "): it cannot be released "
                        "synchronously to all of them, so give each domain a reset synchronizer");
        }

        // RDC003 / RDC004: within one clock domain, from a flop one reset can
        // clear at any time into a flop that reset does not clear.
        bool strict = rep.rules.enabled("RDC004");
        for (size_t i = 0; i < m.flops.size(); i++) {
            const Flop& f = m.flops[i];
            if (!f.dom || (f.arst.empty() && !strict)) continue;
            SymSet mine;
            for (Sym a : f.arst) mine.push_back(m.resetRoot(a));
            seal(mine);
            const std::vector<Root>& all = cones[i].all;
            // One finding per flop and source reset, naming the first source.
            std::map<Sym, std::pair<std::string, int>> bad;
            for (const Root& r : all) {
                const Flop* s = m.flopOf(r.sym);
                if (!s || s == &f || s->dom != f.dom || s->arst.empty() || m.statics.count(r.sym)) continue;
                for (Sym a : s->arst) {
                    Sym rr = m.resetRoot(a);
                    if (has(mine, rr)) continue;
                    auto& b = bad[rr];
                    std::string nm = m.name(*s);
                    if (!b.second++ || nm < b.first) b.first = nm;
                }
            }
            std::string mineName;
            for (Sym x : mine)
                if (std::string nm = m.name(x); mineName.empty() || nm < mineName) mineName = nm;
            for (auto& [rr, b] : bad) {
                std::string src = "'" + b.first + "'" +
                                  (b.second > 1 ? " (and " + std::to_string(b.second - 1) + " more)" : "") +
                                  ", reset by '" + m.name(rr) + "', feeds '" + m.name(f) + "'";
                if (!f.arst.empty())
                    add("RDC003", f.loc,
                            src + ", reset by '" + mineName + "', in the domain of " + clocked(f) +
                                ": when '" + m.name(rr) + "' asserts alone, '" + m.name(f) +
                                "' can sample a value that is changing; reset both together, or "
                                "synchronize or gate the path");
                else
                    add("RDC004", f.loc,
                            src + ", which has no asynchronous reset, in the domain of " + clocked(f) +
                                ": when '" + m.name(rr) + "' asserts, '" + m.name(f) +
                                "' can sample a value that is changing");
            }
        }
    }

    // -cdc-report: the domains and every crossing, clean or not.
    void report() {
        std::map<std::string, int> doms, rsts;
        for (const Flop& f : m.flops) {
            if (f.dom) doms[m.name(f.dom)]++;
            for (Sym a : f.arst) rsts[m.name(m.resetRoot(a))]++;
        }
        std::ostream& os = std::cout;
        os << "clock domains: " << doms.size() << "\n";
        for (auto& [d, k] : doms) os << "  " << d << "  " << k << (k == 1 ? " flop\n" : " flops\n");
        os << "asynchronous resets: " << rsts.size() << "\n";
        for (auto& [d, k] : rsts) os << "  " << d << "  " << k << (k == 1 ? " flop\n" : " flops\n");
        std::vector<std::string> lines;
        for (const Crossing& c : crossings) {
            const Flop& f = m.flops[c.flop];
            const Flop& s = *m.flopOf(c.src);
            lines.push_back("  " + m.name(s) + " (" + clocked(s) + ") -> " + m.name(f) + " (" +
                            clocked(f) + ")  " + c.how);
        }
        std::sort(lines.begin(), lines.end());
        os << "crossings: " << lines.size() << "\n";
        for (auto& l : lines) os << l << "\n";
    }
};

// Resolves the names of -clock-group and -static against the signals of the
// design: a full path (top.u.clk) or one relative to the top (u.clk).
inline void constrain(Model& m, const Options& opt) {
    auto find = [&](const std::string& n, const char* what) -> Sym {
        Sym best = nullptr;
        std::string bestPath;
        auto offer = [&](Sym s, const std::string& p) {
            bool hit = p == n || (p.size() > n.size() && p.ends_with(n) && p[p.size() - n.size() - 1] == '.');
            if (hit && (!best || p.size() < bestPath.size() || (p.size() == bestPath.size() && p < bestPath))) {
                best = s;
                bestPath = p;
            }
        };
        // Any name of the node will do: look at every joined symbol.
        std::vector<Sym> all;
        for (auto& [s, up] : m.join.up) all.push_back(s);
        for (Sym s : all)
            if (isSignal(*s)) offer(s, path(*s));
        for (auto& [s, label] : m.labels) offer(s, label);
        if (!best) std::cerr << "mlint: " << what << " " << n << ": no such signal\n";
        return best ? m.join.find(best) : nullptr;
    };
    for (auto& g : opt.clockGroups) {
        Sym head = nullptr;
        for (auto& n : g) {
            Sym c = find(n, "-clock-group");
            if (!c) continue;
            c = m.clockRoot(c);
            if (!head) head = c;
            m.group[c] = head;
        }
    }
    for (auto& n : opt.statics)
        if (Sym c = find(n, "-static")) m.statics.insert(c);
}

inline void check(Compilation& comp, Report& rep, const Options& opt) {
    Prof prof;
    auto lap = [&](const char* what) { prof.lap(what); };
    Model m;
    Ports ports(m);
    comp.getRoot().visit(ports);
    ports.finish();
    lap("ports");
    Gather g(m);
    comp.getRoot().visit(g);
    ports.finishBits();
    lap("gather");
    m.sealCombs();
    m.nameNodes();
    constrain(m, opt);
    m.finish();
    lap("model");
    Checker c(m, rep, opt);
    c.run();
}

}  // namespace cdc
