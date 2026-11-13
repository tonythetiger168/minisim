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
//   CDC006  with -sdc: a flop's clock is not a clock in the SDC file.
//   CDC007  a clock read as data by logic.
//   CDC008  a clock made by combinational logic of a clock and other signals.
//   CDC009  with -sdc: a one-bit signal that can pulse for one cycle, from a
//           faster clock into a slower one through a plain synchronizer.
//   RDC001  an asynchronous reset generated in another clock domain: its
//           release is not synchronous to the flops it resets.
//   RDC002  one asynchronous reset used directly in several clock domains.
//   RDC003  a flop with one asynchronous reset feeds a flop with another, in
//           the same clock domain.
//   RDC004  (off by default) ... feeds a flop with no asynchronous reset.
//   RDC005  a reset synchronizer of one flop.
//   RDC006  an asynchronous reset made by combinational logic of several flops.
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
#include <deque>
#include <fstream>
#include <sstream>
#include "cdc_sdc.hpp"
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
    std::vector<std::string> sdcFiles;                  // -sdc file
    int depth = 2;                                      // -cdc-depth N
    std::string sva;                                    // -cdc-sva FILE
    std::string abstractOut;                            // -cdc-abstract FILE
    std::vector<std::string> models;                    // -cdc-model FILE
    std::string html;                                   // -cdc-html FILE
    int threads = 1;                                    // -threads N

    // Takes one command-line option if it is one of these. `next` returns the
    // option's value. -threads, -enable and the rule table are mlint's.
    template<typename Next>
    bool take(const std::string& a, Next&& next) {
        if (a == "-cdc-report") { report = true; return true; }
        if (a == "-static") { statics.push_back(next("a signal")); return true; }
        if (a == "-sdc") { sdcFiles.push_back(next("an SDC file")); return true; }
        if (a == "-cdc-sva") { sva = next("an output file"); return true; }
        if (a == "-cdc-html") { html = next("an output file"); return true; }
        if (a == "-cdc-abstract") { abstractOut = next("an output file"); return true; }
        if (a == "-cdc-model") { models.push_back(next("a CDC model file")); return true; }
        if (a == "-cdc-depth") {
            std::string v = next("a number of flops");
            char* e = nullptr;
            long k = strtol(v.c_str(), &e, 10);
            if (v.empty() || *e || k < 0 || k > 64) {
                std::cerr << "mlint: -cdc-depth needs a number of flops from 0 to 64\n";
                std::exit(2);
            }
            depth = (int)k;
            return true;
        }
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
        return false;
    }
};

inline const char* usage() {
    return "  -cdc-html FILE  write a page to browse the clock domains, crossings and reset synchronizers\n"
           "  -cdc-abstract FILE  write the top's CDC model (the clock of each port) for -cdc-model\n"
           "  -cdc-model FILE  use these block models instead of the blocks' RTL (repeatable)\n"
           "  -cdc-sva FILE  write SVA for every crossing to FILE, as module mlint_cdc_sva (compile it with the design)\n"
           "  -cdc-depth N   CDC004 follows synchronized signals through N more flops to where they meet (2)\n"
           "  -sdc FILE      clocks, generated clocks, clock groups and clock-to-clock false paths from an SDC file\n"
           "  -clock-group a,b  these clocks are synchronous to each other: one domain (repeatable)\n"
           "  -static SIG    SIG does not change while it is read: its crossings are not reported\n"
           "  -cdc-report    list the clock domains, the resets and every crossing, safe or not\n";
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

// The part of a variable an expression selects: r[3], r[7:4], r.field. Bits
// count from the least significant one; for an unpacked struct they are the
// fields. `exact` is false when a select index is not a constant, or the
// select goes inside an unpacked field: [lo, hi] is then all it can reach.
struct Sel {
    Sym base = nullptr;
    uint64_t lo = 0, hi = 0;
    bool exact = true, packed = true;
};
inline std::optional<int64_t> constInt(const Expression& e, Sym base) {
    ConstantValue cv;
    if (const ConstantValue* c = e.getConstant()) {
        cv = *c;
    } else {
        const Scope* sc = base->getParentScope();
        if (!sc) return std::nullopt;
        EvalContext ctx(ASTContext(*sc, LookupLocation::max));
        cv = e.eval(ctx);
    }
    if (!cv.isInteger()) return std::nullopt;
    return cv.integer().as<int64_t>();
}
inline bool bitRange(const Expression& ex, Sel& r) {
    const Expression& e = strip(ex);
    auto named = [](const Expression& x) {
        return x.kind == ExpressionKind::NamedValue || x.kind == ExpressionKind::HierarchicalValue;
    };
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue: {
            const Symbol& s = e.kind == ExpressionKind::NamedValue ? (const Symbol&)e.as<NamedValueExpression>().symbol
                                                                    : e.as<HierarchicalValueExpression>().symbol;
            if (!isSignal(s)) return false;
            const Type& t = s.as<ValueSymbol>().getType().getCanonicalType();
            r = {};
            r.base = &s;
            if (t.isIntegral()) {
                r.hi = std::max<uint64_t>(1, t.getBitstreamWidth()) - 1;
            } else if (t.isUnpackedStruct()) {
                auto fields = t.as<UnpackedStructType>().fields;
                if (fields.empty()) return false;
                r.hi = fields.size() - 1;
                r.packed = false;
            } else {
                return false;
            }
            return true;
        }
        case ExpressionKind::ElementSelect:
        case ExpressionKind::RangeSelect: {
            bool element = e.kind == ExpressionKind::ElementSelect;
            const Expression& v =
                strip(element ? e.as<ElementSelectExpression>().value() : e.as<RangeSelectExpression>().value());
            if (!bitRange(v, r)) return false;
            const Type& vt = v.type->getCanonicalType();
            if (!r.exact || !r.packed || !vt.isIntegral() || !vt.hasFixedRange()) {
                r.exact = false;
                return true;
            }
            ConstantRange rg = vt.getFixedRange();
            const Type* et = vt.getArrayElementType();
            uint64_t w = et ? std::max<uint64_t>(1, et->getBitstreamWidth()) : 1;
            std::optional<int64_t> a, b;
            if (element) {
                a = b = constInt(e.as<ElementSelectExpression>().selector(), r.base);
            } else {
                const auto& rs = e.as<RangeSelectExpression>();
                a = constInt(rs.left(), r.base);
                b = constInt(rs.right(), r.base);
                if (a && b && rs.getSelectionKind() == RangeSelectionKind::IndexedUp) b = *a + *b - 1;
                else if (a && b && rs.getSelectionKind() == RangeSelectionKind::IndexedDown) b = *a - *b + 1;
            }
            if (!a || !b || *a < INT32_MIN || *a > INT32_MAX || *b < INT32_MIN || *b > INT32_MAX ||
                !rg.containsPoint((int32_t)*a) || !rg.containsPoint((int32_t)*b)) {
                r.exact = false;
                return true;
            }
            uint64_t x = (uint64_t)rg.translateIndex((int32_t)*a), y = (uint64_t)rg.translateIndex((int32_t)*b);
            uint64_t lo = r.lo + std::min(x, y) * w;
            r.hi = r.lo + (std::max(x, y) + 1) * w - 1;
            r.lo = lo;
            return true;
        }
        case ExpressionKind::MemberAccess: {
            const auto& ma = e.as<MemberAccessExpression>();
            if (ma.member.kind != SymbolKind::Field) return false;
            const auto& field = ma.member.as<FieldSymbol>();
            const Expression& v = strip(ma.value());
            if (!bitRange(v, r)) return false;
            const Type& vt = v.type->getCanonicalType();
            if (r.exact && !r.packed && named(v) && vt.isUnpackedStruct()) {
                r.lo = r.hi = field.fieldIndex;
            } else if (r.exact && r.packed && vt.isIntegral() && !vt.isPackedUnion()) {
                r.lo += field.bitOffset;
                r.hi = r.lo + std::max<uint64_t>(1, field.getType().getBitstreamWidth()) - 1;
            } else {
                r.exact = false;
            }
            return true;
        }
        default:
            return false;
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
    bool reads = false;       // ... or in any expression of one (q <= ~q, q <= q | set)
    Sym from = nullptr;
    uint64_t fromBits = 0;
    uint64_t bits = 1;
    bool negedge = false;     // clocked on the falling edge
    bool arstHigh = false;    // the first asynchronous reset is a posedge event (active high)
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
    // A variable written piece by piece (r[0] <= a; r[1] <= b; or the fields
    // of a struct) is one node per piece, so two bits of one vector are two
    // signals. Disjoint, sorted.
    struct Slice {
        uint64_t lo, hi;
        Sym node;
    };
    std::unordered_map<Sym, std::vector<Slice>> sliced;
    std::deque<std::string> sliceNames;
    std::unordered_map<Sym, const std::string*> sliceLabel;
    // A piece is not in the design: its name is the one it was given.
    std::string pathOf(Sym s) const {
        if (!sliceLabel.empty())
            if (auto it = sliceLabel.find(s); it != sliceLabel.end()) return *it->second;
        return path(*s);
    }
    const std::vector<Slice>* slicesOf(Sym s) const {
        if (sliced.empty()) return nullptr;
        auto it = sliced.find(s);
        return it == sliced.end() ? nullptr : &it->second;
    }
    // The pieces an expression on a sliced variable touches. Returns true
    // when it is exactly one whole piece.
    bool pieces(const Expression& e, const std::vector<Slice>& sl, SymSet& out) {
        Sel r;
        if (!bitRange(e, r)) {
            for (const Slice& x : sl) out.push_back(canon(x.node));
            return false;
        }
        bool one = false;
        size_t n = 0;
        for (const Slice& x : sl)
            if (x.lo <= r.hi && r.lo <= x.hi) {
                out.push_back(canon(x.node));
                one = !n++ && r.exact && x.lo == r.lo && x.hi == r.hi;
            }
        return one && n == 1;
    }
    // -cdc-model: blocks described by their ports, not their RTL.
    struct PortModel {
        std::string port, kind, clock;  // kind: clock, sync, data, comb, mixed
        bool output = false;
    };
    std::map<std::string, std::vector<PortModel>> blockModels;  // module name -> ports
    std::vector<const InstanceSymbol*> modeled;                // instances that use one
    bool isModeled(const InstanceSymbol& inst) const {
        return !blockModels.empty() && blockModels.count(std::string(inst.getDefinition().name));
    }
    // The top's ports, for -cdc-abstract.
    struct TopPort {
        std::string name;
        bool output;
        Sym sym;
    };
    std::vector<TopPort> topPorts;
    std::string topName;
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
    // With -sdc: the clocks of the file, where each one starts, and how every
    // pair is related.
    enum Rel : uint8_t { Sync, Async, Exclusive };
    struct Sdc {
        bool on = false;
        std::vector<std::string> clocks;                 // by index
        std::unordered_map<Sym, std::vector<int>> at;    // node -> the clocks defined on it
        std::vector<std::vector<uint8_t>> rel;           // clock x clock
        std::unordered_map<Sym, std::vector<int>> of;    // domain -> the clocks that reach it
        std::vector<double> period;                      // by clock; 0 when not known
    } sdc;
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
        if (nit == named.end()) return it->second = pathOf(c);
        for (Sym s : nit->second.syms)
            if (std::string p = pathOf(s); best.empty() || p < best) best = std::move(p);
        for (const std::string& p : nit->second.labels)
            if (best.empty() || p < best) best = p;
        return it->second = best.empty() ? pathOf(c) : best;
    }
    // A flop's own name, as its module wrote it.
    const std::string& name(const Flop& f) const {
        if (f.pathName.empty()) f.pathName = pathOf(f.sym);
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
            if (sdc.on && sdc.at.count(c)) return c;  // a clock is defined here
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
    // The SDC clocks that reach a domain's root: the one defined on it, or,
    // behind a clock mux, every clock defined in the logic in front of it.
    std::vector<int> clocksReaching(Sym root) const {
        std::vector<int> out;
        std::vector<Sym> todo{root};
        std::unordered_set<Sym> seen{root};
        while (!todo.empty() && seen.size() < 4096) {
            Sym c = todo.back();
            todo.pop_back();
            if (auto it = sdc.at.find(c); it != sdc.at.end()) {
                out.insert(out.end(), it->second.begin(), it->second.end());
                continue;
            }
            if (flopIx.count(c)) continue;  // a divider with no create_generated_clock
            if (const Comb* n = combOf(c))
                for (const Edge& e : n->src)
                    if (seen.insert(e.sym).second) todo.push_back(e.sym);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    const std::vector<int>& clocksOf(Sym dom) const {
        static const std::vector<int> none;
        auto it = sdc.of.find(dom);
        return it == sdc.of.end() ? none : it->second;
    }
    // Is a path between two domains a crossing? Without an SDC file every
    // other domain is. With one, a domain is the clocks that reach it: two
    // domains cross when some pair of their clocks is asynchronous. Exclusive
    // clocks are never there together, so there is no path to check.
    bool async(Sym a, Sym b) const {
        if (a == b) return false;
        if (!sdc.on) return true;
        const auto& ca = clocksOf(a);
        const auto& cb = clocksOf(b);
        if (ca.empty() || cb.empty()) return true;  // no clock defined: a domain of its own
        for (int i : ca)
            for (int j : cb)
                if (i != j && sdc.rel[i][j] == Async) return true;
        return false;
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
            if (sdc.on && f.dom && !sdc.of.count(f.dom)) sdc.of[f.dom] = clocksReaching(f.dom);
        }
    }
};

// --------------------------------------------------------------- the gather

struct Walk {
    Model& m;
    const Scope* scope = nullptr;  // of a continuous assign, to evaluate a bit index
    bool seq = false;
    Sym clk = nullptr;
    bool neg = false;  // the clock's edge is negedge
    bool arstHigh = false;
    SymSet arst;  // canonical

    void name(Sym s, SymSet& out) {
        if (!isSignal(*s)) return;
        if (const auto* sl = m.slicesOf(s)) {
            for (const Model::Slice& x : *sl) out.push_back(m.canon(x.node));
            return;
        }
        out.push_back(m.canon(s));
    }
    // A select on a variable that is kept in pieces reads those pieces, and
    // the signals of its indexes.
    template<typename V, typename T>
    void select(V& v, const T& e, SymSet& out) {
        Sym b = m.sliced.empty() ? nullptr : baseOf(e);
        const std::vector<Model::Slice>* sl = b ? m.slicesOf(b) : nullptr;
        if (!sl) {
            v.visitDefault(e);
            return;
        }
        m.pieces(e, *sl, out);
        for (const Expression* x = &strip(e);;) {
            if (x->kind == ExpressionKind::ElementSelect) {
                x->as<ElementSelectExpression>().selector().visit(v);
                x = &strip(x->as<ElementSelectExpression>().value());
            } else if (x->kind == ExpressionKind::RangeSelect) {
                x->as<RangeSelectExpression>().left().visit(v);
                x->as<RangeSelectExpression>().right().visit(v);
                x = &strip(x->as<RangeSelectExpression>().value());
            } else if (x->kind == ExpressionKind::MemberAccess) {
                x = &strip(x->as<MemberAccessExpression>().value());
            } else {
                break;
            }
        }
    }
    void names(const Expression& e, SymSet& out) {
        e.visit(makeVisitor(
            [&](auto&, const NamedValueExpression& n) { name(&n.symbol, out); },
            [&](auto&, const HierarchicalValueExpression& n) { name(&n.symbol, out); },
            [&](auto& v, const ElementSelectExpression& x) { select(v, x, out); },
            [&](auto& v, const RangeSelectExpression& x) { select(v, x, out); },
            [&](auto& v, const MemberAccessExpression& x) { select(v, x, out); }));
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
                if (const auto* sl = m.slicesOf(b)) {
                    SymSet hit;
                    m.pieces(e, *sl, hit);
                    if (hit.empty()) return;  // bits nothing drives
                    if (hit.size() > 1) { t.other = true; return; }
                    c = hit[0];
                }
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
        Sym real = baseOf(lhs);
        if (!real || !isSignal(*real)) return;
        if (const auto* sl = m.slicesOf(real)) {
            // Each piece the left-hand side touches is assigned; it is a
            // whole assignment when that is exactly one piece.
            SymSet hit, own;
            bool exact = m.pieces(lhs, *sl, hit);
            for (const Model::Slice& x : *sl) own.push_back(m.canon(x.node));
            for (Sym node : hit) one(node, real, own, lhs, rhs, nonBlocking, ctrl0, whole && exact);
            return;
        }
        one(real, real, {m.canon(real)}, lhs, rhs, nonBlocking, ctrl0, whole);
    }
    // `base` is the node assigned: the variable, or one piece of it.
    void one(Sym base, Sym real, const SymSet& own, const Expression& lhs, const Expression& rhs, bool nonBlocking,
             const SymSet& ctrl0, bool whole) {
        Sym self = m.canon(base);
        SymSet ctrl = ctrl0;
        // A select index on the left chooses what is written: control.
        {
            SymSet ln;
            names(lhs, ln);
            for (Sym s : ln)
                if (std::find(own.begin(), own.end(), s) == own.end()) ctrl.push_back(s);
        }
        std::vector<const Expression*> lv;
        leaves(rhs, ctrl, lv);
        if (seq && (nonBlocking || !local(real))) {
            Flop& f = m.flop(base, lhs.sourceRange.start());
            if (!f.clkSym) {
                f.clkSym = clk;
                f.negedge = neg;
                f.arstHigh = arstHigh;
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
                if (!f.reads) {
                    SymSet n;
                    names(*e, n);
                    for (Sym x : n) f.reads = f.reads || x == self;
                }
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
            if (scope && base == real && l.kind == ExpressionKind::ElementSelect && !m.bitCanon.empty()) {
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

// Before anything else: which variables are written piece by piece. Such a
// variable becomes one node per piece. Left whole: a variable a port is
// connected to by name or by a select (the port passes are about those), and
// one that is also assigned whole from other signals.
struct Slicer : ASTVisitor<Slicer> {
    Model& m;
    Compilation& comp;
    struct Var {
        std::vector<std::pair<uint64_t, uint64_t>> writes;
        bool whole = false;
    };
    std::unordered_map<Sym, Var> vars;
    std::vector<Sym> order;
    Slicer(Model& mm, Compilation& c) : m(mm), comp(c) {}

    Var& var(Sym s) {
        auto [it, fresh] = vars.try_emplace(s);
        if (fresh) order.push_back(s);
        return it->second;
    }
    static bool constant(const Expression& e) {
        bool any = false;
        e.visit(makeVisitor([&](auto&, const NamedValueExpression& n) { any = any || isSignal(n.symbol); },
                            [&](auto&, const HierarchicalValueExpression& n) { any = any || isSignal(n.symbol); }));
        return !any;
    }
    void keepWhole(const Expression& e) {
        e.visit(makeVisitor([&](auto&, const NamedValueExpression& n) { if (isSignal(n.symbol)) var(&n.symbol).whole = true; },
                            [&](auto&, const HierarchicalValueExpression& n) { if (isSignal(n.symbol)) var(&n.symbol).whole = true; }));
    }
    void written(const Expression& lhs, bool constRhs) {
        const Expression& l = strip(lhs);
        if (l.kind == ExpressionKind::Concatenation) {
            for (const Expression* o : l.as<ConcatenationExpression>().operands()) written(*o, constRhs);
            return;
        }
        Sel r;
        if (!bitRange(l, r)) {
            if (Sym b = baseOf(l); b && isSignal(*b)) var(b).whole = true;
            return;
        }
        bool all = l.kind == ExpressionKind::NamedValue || l.kind == ExpressionKind::HierarchicalValue;
        if (all && constRhs) return;  // r <= '0 clears every piece
        if (all) var(r.base).whole = true;
        else var(r.base).writes.push_back({r.lo, r.hi});
    }
    void assignment(const Expression& e) {
        if (e.kind != ExpressionKind::Assignment) return;
        const auto& a = e.as<AssignmentExpression>();
        written(a.left(), constant(a.right()));
    }

    void handle(const GenerateBlockSymbol& g) {
        if (!g.isUninstantiated) visitDefault(g);
    }
    void handle(const InstanceSymbol& inst) {
        if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return;
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn || conn->port.kind != SymbolKind::Port) continue;
            const auto& port = conn->port.as<PortSymbol>();
            if (port.internalSymbol && isSignal(*port.internalSymbol)) var(port.internalSymbol).whole = true;
            const Expression* ext = conn->getExpression();
            if (!ext) continue;
            const Expression* outer = ext;
            if (outer->kind == ExpressionKind::Assignment) outer = &outer->as<AssignmentExpression>().left();
            // An input fed an expression only reads; anything else ties the
            // variable to the port.
            if (port.direction != ArgumentDirection::In || baseOf(*outer)) keepWhole(*outer);
        }
        if (!m.isModeled(inst)) visitDefault(inst);
    }
    void handle(const NetSymbol& n) {
        if (const Expression* init = n.getInitializer(); init && !constant(*init)) var(&n).whole = true;
    }
    void handle(const ContinuousAssignSymbol& ca) { assignment(ca.getAssignment()); }
    void handle(const ProceduralBlockSymbol& p) {
        if (p.procedureKind == ProceduralBlockKind::Initial || p.procedureKind == ProceduralBlockKind::Final)
            return;
        p.getBody().visit(makeVisitor([&](auto& v, const AssignmentExpression& a) {
            written(a.left(), constant(a.right()));
            v.visitDefault(a);
        }));
    }

    void finish() {
        for (Sym s : order) {
            Var& v = vars[s];
            if (v.whole || v.writes.size() < 2) continue;
            const Scope* sc = s->getParentScope();
            if (!sc || sc->asSymbol().kind == SymbolKind::StatementBlock) continue;  // a local of a block
            std::sort(v.writes.begin(), v.writes.end());
            std::vector<std::pair<uint64_t, uint64_t>> parts;
            for (auto& w : v.writes) {
                if (!parts.empty() && w.first <= parts.back().second)
                    parts.back().second = std::max(parts.back().second, w.second);
                else
                    parts.push_back(w);
            }
            if (parts.size() < 2 || parts.size() > 4096) continue;
            const Type& t = s->as<ValueSymbol>().getType().getCanonicalType();
            std::string base = path(*s);
            bool hidden = base == "<protected>";
            auto& out = m.sliced[s];
            for (auto& [lo, hi] : parts) {
                std::string label = base;
                const Type* pt = nullptr;
                if (t.isUnpackedStruct()) {
                    auto fields = t.as<UnpackedStructType>().fields;
                    if (!hidden) {
                        label += "." + std::string(fields[lo]->name);
                        if (hi != lo) label += ".." + std::string(fields[hi]->name);
                    }
                    pt = lo == hi ? &fields[lo]->getType() : nullptr;
                } else if (!hidden) {
                    label += bits(t, lo, hi);
                }
                if (!pt) pt = &comp.getType((bitwidth_t)std::min<uint64_t>(hi - lo + 1, 1u << 20), IntegralFlags::FourState);
                m.sliceNames.push_back(std::move(label));
                auto* node = comp.emplace<VariableSymbol>(std::string_view(m.sliceNames.back()), s->location,
                                                          VariableLifetime::Static);
                node->setType(*pt);
                m.sliceLabel[node] = &m.sliceNames.back();
                out.push_back({lo, hi, node});
            }
        }
    }
    // How the source would write bits lo..hi of a variable: the field of a
    // packed struct, the index of a vector, or the bit numbers.
    static std::string bits(const Type& t, uint64_t lo, uint64_t hi) {
        if (t.kind == SymbolKind::PackedStructType)
            for (const FieldSymbol& f : t.as<PackedStructType>().membersOfType<FieldSymbol>())
                if (f.bitOffset == lo && f.bitOffset + f.getType().getBitstreamWidth() - 1 == hi)
                    return "." + std::string(f.name);
        if (t.hasFixedRange() && (!t.getArrayElementType() || t.getArrayElementType()->getBitstreamWidth() == 1)) {
            ConstantRange rg = t.getFixedRange();
            auto ix = [&](uint64_t off) { return rg.isDescending() ? rg.lower() + (int64_t)off : rg.upper() - (int64_t)off; };
            return lo == hi ? "[" + std::to_string(ix(lo)) + "]"
                            : "[" + std::to_string(ix(hi)) + ":" + std::to_string(ix(lo)) + "]";
        }
        return "[bits " + std::to_string(hi) + ":" + std::to_string(lo) + "]";
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
        if (m.isModeled(inst)) {
            m.modeled.push_back(&inst);
            return;
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
            bool plain = w.plain;
            for (Sym s : w.from) {
                if (const auto* sl = m.slicesOf(s)) {
                    for (const Model::Slice& x : *sl) from.push_back(m.canon(x.node));
                    plain = false;
                } else {
                    from.push_back(m.canon(s));
                }
            }
            m.addComb(w.to, from, plain, plain ? w.rhs : nullptr);
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
        if (m.isModeled(inst)) return;  // its model stands in for it
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
            if (!src.empty() && !m.slicesOf(&n)) m.addComb(&n, src, baseOf(*init) != nullptr, init);
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
                    if (has(read, s)) {
                        if (w.arst.empty()) w.arstHigh = e->edge == EdgeKind::PosEdge;
                        w.arst.push_back(m.canon(s));
                    }
                    else if (!w.clk) {
                        w.clk = s;
                        w.neg = e->edge == EdgeKind::NegEdge;
                    }
                }
                if (!w.clk) {  // every edge is read: take the first, as lintir does
                    w.clk = oneName(edges[0]->expr);
                    w.neg = edges[0]->edge == EdgeKind::NegEdge;
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

    // What -cdc-sva can check about a crossing.
    enum Check : uint8_t { NoCheck, Stable, OneBit, Qualified };
    struct Crossing {
        int flop;
        Sym src;
        std::string how;
        Check check = NoCheck;
        Sym qual = nullptr;  // Qualified: the synchronized control the reader takes it with
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
        if (!s || !s->dom || !f.dom || !m.async(s->dom, f.dom) || m.statics.count(r)) return nullptr;
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
    // The period of a domain from the SDC file: its fastest clock (the
    // shortest pulse it can launch), or its slowest (the longest a reader can
    // miss). 0 when a clock that reaches it has no known period.
    static std::string num(double d) {
        std::ostringstream o;
        o << d;
        return o.str();
    }
    double period(Sym dom, bool slowest) const {
        double out = 0;
        for (int c : m.clocksOf(dom)) {
            double p = c < (int)m.sdc.period.size() ? m.sdc.period[c] : 0;
            if (!(p > 0)) return 0;
            out = out == 0 ? p : slowest ? std::max(out, p) : std::min(out, p);
        }
        return out;
    }

    // CDC007 / CDC008: how the clocks are used and made.
    void clockChecks() {
        std::map<Sym, int> domFlops;
        for (const Flop& f : m.flops)
            if (f.dom) domFlops[f.dom]++;
        // A node that is the root of a clock (as written by a flop, before
        // -clock-group joins domains).
        std::map<Sym, Sym> clockRootOf;  // root -> domain
        for (const Flop& f : m.flops)
            if (f.dom && f.clkSym) clockRootOf[m.clockRoot(m.canon(f.clkSym))] = f.dom;
        // CDC007: a flop whose data or control reads a clock.
        std::map<std::pair<Sym, Sym>, std::pair<int, SourceLocation>> asData;  // (clock, reading domain)
        for (size_t i = 0; i < m.flops.size(); i++) {
            const Flop& f = m.flops[i];
            if (!f.dom) continue;
            for (const Root& r : cones[i].all) {
                if (!clockRootOf.count(r.sym) || m.flopOf(r.sym) || m.statics.count(r.sym)) continue;
                auto& e = asData[{r.sym, f.dom}];
                if (!e.first++) e.second = f.loc;
            }
        }
        for (auto& [key, e] : asData)
            add("CDC007", e.second,
                "clock '" + m.name(key.first) + "' is read as data by " + std::to_string(e.first) +
                    (e.first == 1 ? " flop" : " flops") + " clocked by " + m.name(key.second) +
                    ": its edges are not synchronous to that clock, so sample a divided or toggled copy "
                    "through a synchronizer");
        // CDC008: a clock that is logic of another clock and of other signals
        // (a clock mux made of gates): a change of the other signals can cut
        // a pulse short.
        for (auto& [root, dom] : clockRootOf) {
            const Comb* c = m.combOf(root);
            if (!c || c->src.size() < 2) continue;
            Tracer tr(m);
            std::vector<std::string> clocks, other;
            for (const Root& r : tr.roots(root)) {
                if (clockRootOf.count(r.sym) || (m.sdc.on && m.sdc.at.count(r.sym))) clocks.push_back(m.name(r.sym));
                else if (!m.statics.count(r.sym)) other.push_back(m.name(r.sym));
            }
            if (clocks.empty() || other.empty() || (clocks.size() == 1 && clocks[0] == m.name(root))) continue;
            std::vector<std::string> all = clocks;
            all.insert(all.end(), other.begin(), other.end());
            std::sort(all.begin(), all.end());
            auto list = [](const std::vector<std::string>& v) {
                std::string o;
                for (size_t i = 0; i < v.size() && i < 4; i++) o += (i ? ", '" : "'") + v[i] + "'";
                if (v.size() > 4) o += ", ...";
                return o;
            };
            SourceLocation loc;
            for (const Flop& f : m.flops)
                if (f.dom == dom) { loc = f.loc; break; }
            add("CDC008", loc,
                "clock '" + m.name(root) + "' of " + std::to_string(domFlops[dom]) +
                    (domFlops[dom] == 1 ? " flop" : " flops") + " is combinational logic of " + list(all) + ": changing those can glitch the clock, so use a glitch-free clock "
                    "mux or a clock-gate cell, or declare them -static if they only change in reset");
        }
    }

    // CDC006: with an SDC file, a clock it does not define.
    void unclocked() {
        std::map<Sym, std::pair<int, SourceLocation>> none;
        for (const Flop& f : m.flops) {
            if (!f.dom || !m.clocksOf(f.dom).empty()) continue;
            auto& e = none[f.dom];
            if (!e.first++) e.second = f.loc;
        }
        for (auto& [d, e] : none)
            add("CDC006", e.second,
                "clock '" + m.name(d) + "' of " + std::to_string(e.first) + (e.first == 1 ? " flop" : " flops") +
                    " is not a clock in the SDC file: it is checked as a domain of its own, asynchronous to "
                    "every other (add a create_clock or create_generated_clock for it)");
    }

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
        if (m.sdc.on) unclocked();
        clockChecks();
        prof.lap("resets");
        flush();
        if (opt.report) report();
        if (!opt.sva.empty()) writeSva(opt.sva);
        if (!opt.html.empty()) writeHtml(opt.html);
        if (!opt.abstractOut.empty()) writeAbstract(opt.abstractOut);
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
            Sym qual = nullptr;
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
                std::vector<Sym> qualBy;  // the control flop, by quals entry
                for (const Root& q : all) {
                    const Flop* qf = m.flopOf(q.sym);
                    if (!qf || qf->dom != f.dom) continue;
                    const Member& mem = member[qf - m.flops.data()];
                    if (mem.pos >= 2) {
                        quals.push_back(mem.srcDom);
                        qualBy.push_back(q.sym);
                    }
                }
                for (const Root& r : all) {
                    const Flop* s = foreign(f, r.sym);
                    if (!s) continue;
                    auto qit = std::find(quals.begin(), quals.end(), s->dom);
                    if (qit != quals.end())
                        recs[i].push_back({r.sym, Qualified, r.logic, 0, qualBy[qit - quals.begin()]});
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
                    crossings.push_back({(int)i, r.src, "enable-qualified", Check::Qualified, r.qual});
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
                } else if (total == 1 && s.bits == 1 && !s.shifts && !s.reads) {
                    // CDC009: a value that can last one cycle, from a faster
                    // clock into a slower one.
                    double fast = period(s.dom, false), slow = period(f.dom, true);
                    if (fast > 0 && slow > 0 && fast < slow) {
                        add("CDC009", f.loc,
                                st.where + " through " + std::to_string(st.len) + " flops, from a clock of period " +
                                    num(fast) + " into one of period " + num(slow) +
                                    ": a pulse of one source cycle can come and go between two edges of the "
                                    "reading clock and be missed, so hold it for 1.5 reading periods, use a "
                                    "toggle (pulse) synchronizer, or a handshake");
                        sync += ", fast to slow (CDC009)";
                    }
                }
                Check check = st.logic ? NoCheck : total > 1 || s.bits > 1 ? OneBit : Stable;
                crossings.push_back({st.flop, key.first, sync, check});
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
    // CDC004. Signals of one domain that were synchronized separately and
    // meet again: in the logic of one flop, or after up to -cdc-depth more
    // flops of the reading domain (each side registered a different number of
    // times does not bring them back in step).
    void reconverge(const std::vector<char>& first) {
        const size_t n = m.flops.size();
        const int depth = std::max(0, opt.depth);
        // What a flop carries: synchronized sources, and how many flops after
        // its synchronizer each one is. Sorted by source; the smallest depth.
        using Carried = std::vector<std::pair<Sym, int>>;
        auto put = [](Carried& c, Sym s, int d) {
            auto it = std::lower_bound(c.begin(), c.end(), s, [](const auto& e, Sym k) { return e.first < k; });
            if (it != c.end() && it->first == s) {
                if (d >= it->second) return false;
                it->second = d;
                return true;
            }
            if (c.size() >= 64) return false;  // a flop where everything meets: enough to report
            c.insert(it, {s, d});
            return true;
        };
        auto depthIn = [](const Carried& c, Sym s) {
            auto it = std::lower_bound(c.begin(), c.end(), s, [](const auto& e, Sym k) { return e.first < k; });
            return it != c.end() && it->first == s ? it->second : -1;
        };
        std::vector<Carried> direct(n), reach;
        std::vector<std::vector<int>> preds(n);
        chunked(n, opt.threads, [&](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi; i++) {
                const Flop& f = m.flops[i];
                if (!f.dom || first[i]) continue;
                for (const Root& r : cones[i].all) {
                    const Flop* sf = m.flopOf(r.sym);
                    if (!sf || sf->dom != f.dom) continue;
                    size_t j = sf - m.flops.data();
                    if (member[j].pos >= 2) put(direct[i], member[j].src, 0);
                    else if (j != i && !first[j] && depth) preds[i].push_back((int)j);
                }
                std::sort(preds[i].begin(), preds[i].end());
                preds[i].erase(std::unique(preds[i].begin(), preds[i].end()), preds[i].end());
            }
        });
        reach = direct;
        for (int round = 0; round < depth; round++) {
            std::vector<Carried> grown = direct;
            std::atomic<bool> changed{false};
            chunked(n, opt.threads, [&](size_t lo, size_t hi) {
                for (size_t i = lo; i < hi; i++) {
                    for (int p : preds[i])
                        for (auto& [s, d] : reach[p])
                            if (d < depth) put(grown[i], s, d + 1);
                    if (grown[i] != reach[i]) changed = true;
                }
            });
            reach.swap(grown);
            if (!changed) break;
        }
        // The same signals meet at one flop and then, in a state machine, at
        // every flop behind it. One finding for each set of signals: at the
        // flops where they meet in one cone of logic (as before), otherwise
        // at the nearest flop, with a count of the others.
        struct Seq {
            int flop = -1, far = 0, more = 0;
            std::string where;
        };
        std::map<std::pair<Sym, std::vector<Sym>>, Seq> seqs;
        std::set<std::pair<Sym, std::vector<Sym>>> met;
        auto namesOf = [&](const std::vector<Sym>& v) {
            std::vector<std::string> sorted;
            for (Sym e : v) sorted.push_back(m.name(*m.flopOf(e)));
            std::sort(sorted.begin(), sorted.end());
            std::string names;
            for (auto& e : sorted) names += (names.empty() ? "'" : ", '") + e + "'";
            return names;
        };
        const char* why = ": each can arrive a cycle apart from the others, so cross one signal (or a Gray code) "
                          "and derive the rest from it";
        for (size_t i = 0; i < n; i++) {
            const Flop& f = m.flops[i];
            if (reach[i].size() < 2) continue;
            std::map<Sym, Carried> byDom;  // source domain -> what arrives from it
            for (auto& e : reach[i]) byDom[m.flopOf(e.first)->dom].push_back(e);
            for (auto& [d, v] : byDom) {
                if (v.size() < 2) continue;
                std::vector<Sym> here, all;
                int far = 0;
                for (auto& e : v) {
                    far = std::max(far, e.second);
                    all.push_back(e.first);
                    if (!e.second) here.push_back(e.first);
                }
                if (here.size() >= 2) {
                    met.insert({d, here});
                    add("CDC004", f.loc,
                            namesOf(here) + " (clocked by " + m.name(d) + ") are synchronized separately and meet " +
                                "again at '" + m.name(f) + "'" + why);
                }
                if (!far) continue;
                Seq& q = seqs[{d, all}];
                std::string where = m.name(f);
                if (q.flop < 0 || far < q.far || (far == q.far && where < q.where)) {
                    q.flop = (int)i;
                    q.far = far;
                    q.where = where;
                }
                q.more++;
            }
        }
        for (auto& [key, q] : seqs) {
            if (met.count(key)) continue;
            std::string others = q.more > 1 ? " (and at " + std::to_string(q.more - 1) +
                                                  (q.more == 2 ? " more flop)" : " more flops)")
                                            : "";
            add("CDC004", m.flops[q.flop].loc,
                    namesOf(key.second) + " (clocked by " + m.name(key.first) + ") are synchronized separately " +
                        "and meet again at '" + q.where + "', " + std::to_string(q.far) +
                        (q.far == 1 ? " flop" : " flops") + " after the synchronizers" + others + why);
        }
    }

    // A reset synchronizer: flops in a row with the same clock and the same
    // asynchronous reset, the first one fed a constant (asserted at once,
    // released on the clock), or flops with no reset that sample a reset
    // input. Returns how many flops, 0 when `g` is something else.
    int resetSyncStages(const Flop& g) const {
        if (!g.plain) return 0;
        int stages = 1;
        const Flop* f = &g;
        for (int guard = 0; guard < 16; guard++) {
            if (!f->from) return f->arst.empty() ? 0 : stages;  // a constant behind the reset
            Sym r = m.wireRoot(f->from);
            const Flop* p = m.flopOf(r);
            if (!p) {
                // An input, sampled: a synchronizer when the flops have no reset.
                const Comb* c = m.combOf(r);
                return f->arst.empty() && g.arst.empty() && (!c || c->src.empty()) ? stages : 0;
            }
            if (p == f || !p->plain || p->dom != g.dom || p->arst != g.arst) return 0;
            f = p;
            stages++;
        }
        return stages;
    }

    // RDC005 / RDC006: how the asynchronous resets are made.
    struct ResetSync {
        const Flop* out;
        int stages, flops;
    };
    std::vector<ResetSync> resetSyncs;
    void resetTree() {
        struct Use {
            int flops = 0;
            SourceLocation loc;
            Sym dom = nullptr;
        };
        std::map<Sym, Use> uses;  // the reset as wired -> the flops it resets
        std::vector<Sym> order;
        for (const Flop& f : m.flops)
            for (Sym a : f.arst) {
                Sym r = m.wireRoot(a);
                Use& u = uses[r];
                if (!u.flops++) {
                    u.loc = f.loc;
                    u.dom = f.dom;
                    order.push_back(r);
                }
            }
        Tracer tr(m);
        for (Sym r : order) {
            const Use& u = uses[r];
            if (m.statics.count(r)) continue;
            // The flops behind it: the reset itself, or what a scan mux or a
            // gate on it is fed.
            std::vector<const Flop*> made;
            bool logic = false;
            if (const Flop* g = m.flopOf(r)) {
                made.push_back(g);
            } else {
                for (const Root& root : tr.roots(r))
                    if (const Flop* g = m.flopOf(root.sym); g && !m.statics.count(root.sym)) {
                        made.push_back(g);
                        logic = logic || root.logic;
                    }
            }
            if (made.size() == 1) {
                const Flop* g = made[0];
                int stages = resetSyncStages(*g);
                if (!stages) continue;
                resetSyncs.push_back({g, stages, u.flops});
                if (stages == 1)
                    add("RDC005", g->loc,
                            "reset synchronizer '" + m.name(*g) + "' (clocked by " + clocked(*g) + ") is one flop: " +
                                "when the reset is released near a clock edge it can go metastable and release " +
                                "the " + std::to_string(u.flops) + (u.flops == 1 ? " flop" : " flops") +
                                " it resets at different times, so use two flops");
                continue;
            }
            // Logic on a reset: two flops that change together can glitch it.
            if (made.size() < 2 || !logic) continue;
            std::vector<std::string> from;
            for (const Flop* g : made) from.push_back(m.name(*g));
            std::sort(from.begin(), from.end());
            std::string names;
            for (size_t i = 0; i < from.size() && i < 4; i++) names += (i ? ", '" : "'") + from[i] + "'";
            if (from.size() > 4) names += ", ...";
            add("RDC006", u.loc,
                    "asynchronous reset '" + m.name(r) + "' of " + std::to_string(u.flops) +
                        (u.flops == 1 ? " flop" : " flops") + " is combinational logic of " +
                        std::to_string(from.size()) + " flops (" + names + "): when they change together the " +
                        "logic can glitch and reset by accident, so register the reset after the logic");
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
                    if (g->dom && m.async(g->dom, f.dom) && !m.statics.count(r)) {
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
            if (m.sdc.on) {
                // Domains the SDC file relates are one domain here.
                bool cross = false;
                for (size_t i = 0; i < v.size() && !cross; i++)
                    for (size_t j = i + 1; j < v.size() && !cross; j++) cross = m.async(v[i].first, v[j].first);
                if (!cross) continue;
            }
            std::string names;
            for (auto& e : v) names += (names.empty() ? "" : ", ") + m.name(e.first);
            add("RDC002", v[1].second,
                    "asynchronous reset '" + m.name(r) + "' is used directly by flops in " +
                        std::to_string(v.size()) + " clock domains (" + names + "): it cannot be released "
                        "synchronously to all of them, so give each domain a reset synchronizer");
        }

        resetTree();

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
    // -cdc-html: one self-contained page (no network) to browse the crossings:
    // the domains in colour, a table to filter, and for the selected crossing
    // its path from the source flop through the synchronizer stages.
    static std::string esc(const std::string& t) {
        std::string o;
        for (char c : t) {
            switch (c) {
                case '&': o += "&amp;"; break;
                case '<': o += "&lt;"; break;
                case '>': o += "&gt;"; break;
                case '"': o += "&quot;"; break;
                default: o += c;
            }
        }
        return o;
    }
    static std::string json(const std::string& t) {
        std::string o = "\"";
        for (char c : t) {
            if (c == '"' || c == '\\') o += '\\';
            if ((unsigned char)c < 0x20) { o += ' '; continue; }
            if (c == '<') { o += "\\u003c"; continue; }
            o += c;
        }
        return o + "\"";
    }
    void writeHtml(const std::string& file) {
        std::map<std::string, int> domIx;
        std::vector<std::string> domNames;
        std::map<std::string, int> domFlops;
        for (const Flop& f : m.flops)
            if (f.dom) domFlops[m.name(f.dom)]++;
        for (auto& [d, k] : domFlops) {
            domIx[d] = (int)domNames.size();
            domNames.push_back(d);
        }
        std::vector<std::string> rows;
        for (const Crossing& c : crossings) {
            const Flop& f = m.flops[c.flop];
            const Flop& s = *m.flopOf(c.src);
            // The path: source, then the reading flop and the plain copies
            // after it in its domain (the synchronizer), up to 8.
            std::string chain = "[";
            int at = c.flop;
            for (int k = 0; k < 8 && at >= 0; k++) {
                chain += (k ? "," : "") + json(m.name(m.flops[at]));
                int nx = -1;
                for (int g : next[at])
                    if (nx < 0 || m.name(m.flops[g]) < m.name(m.flops[nx])) nx = g;
                at = c.how.find("synchronizer") == std::string::npos ? -1 : nx;
            }
            chain += "]";
            std::string rule;
            if (auto p = c.how.find("(CDC"); p != std::string::npos) rule = c.how.substr(p + 1, 6);
            // The schematic between them: the nets and logic from the source
            // to the reader's input (breadth first, nearest way, up to 6).
            std::string via = "[";
            {
                std::unordered_map<Sym, std::pair<Sym, bool>> from;  // node -> (next toward the reader, logic)
                std::deque<Sym> todo;
                SymSet starts = f.data;
                starts.insert(starts.end(), f.ctrl.begin(), f.ctrl.end());
                for (Sym st : starts)
                    if (from.emplace(st, std::make_pair(nullptr, false)).second) todo.push_back(st);
                bool found = from.count(s.self) > 0;
                for (size_t steps = 0; !found && !todo.empty() && steps < 4000; steps++) {
                    Sym x = todo.front();
                    todo.pop_front();
                    if (m.flopIx.count(x)) continue;
                    const Comb* n = m.combOf(x);
                    if (!n) continue;
                    for (const Edge& e : n->src)
                        if (from.emplace(e.sym, std::make_pair(x, e.logic)).second) {
                            if (e.sym == s.self) { found = true; break; }
                            todo.push_back(e.sym);
                        }
                }
                if (found) {
                    std::vector<std::string> hops;
                    bool logic = from[s.self].second;
                    for (Sym x = from[s.self].first; x && hops.size() < 6; x = from[x].first) {
                        hops.push_back("{\"n\":" + json(m.name(x)) + ",\"g\":" + (logic ? "1" : "0") + "}");
                        logic = from[x].second;
                    }
                    for (size_t k = 0; k < hops.size(); k++) via += (k ? "," : "") + hops[k];
                }
            }
            via += "]";
            std::string qual = c.qual ? json(m.name(c.qual)) : "null";
            rows.push_back("{\"src\":" + json(m.name(s)) + ",\"sd\":" + std::to_string(domIx[m.name(s.dom)]) +
                           ",\"dst\":" + json(m.name(f)) + ",\"dd\":" + std::to_string(domIx[m.name(f.dom)]) +
                           ",\"how\":" + json(c.how) + ",\"rule\":" + json(rule) + ",\"chain\":" + chain +
                           ",\"via\":" + via + ",\"qual\":" + qual + "}");
        }
        std::sort(rows.begin(), rows.end());
        std::vector<std::string> rsts;
        for (const ResetSync& r : resetSyncs)
            rsts.push_back("{\"out\":" + json(m.name(*r.out)) + ",\"d\":" + std::to_string(domIx[m.name(r.out->dom)]) +
                           ",\"stages\":" + std::to_string(r.stages) + ",\"flops\":" + std::to_string(r.flops) + "}");
        std::sort(rsts.begin(), rsts.end());
        std::string doms = "[";
        for (size_t i = 0; i < domNames.size(); i++)
            doms += (i ? "," : "") + std::string("{\"name\":") + json(domNames[i]) + ",\"flops\":" +
                    std::to_string(domFlops[domNames[i]]) + "}";
        doms += "]";
        std::ofstream os(file);
        if (!os) {
            std::cerr << "mlint: -cdc-html " << file << ": cannot write\n";
            return;
        }
        os << R"(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CDC: )" << esc(m.topName) << R"(</title>
<style>
:root{--bg:#fff;--fg:#1f2328;--mut:#59636e;--line:#d1d9e0;--sel:#eef4ff;--bad:#b42318}
@media(prefers-color-scheme:dark){:root{--bg:#0d1117;--fg:#e6edf3;--mut:#9198a1;--line:#30363d;--sel:#1b2a44;--bad:#ff7b72}}
body{margin:0;font:14px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:16px 20px;border-bottom:1px solid var(--line)}h1{font-size:18px;margin:0 0 4px}
.mut{color:var(--mut)}main{display:grid;grid-template-columns:minmax(0,1.3fr) minmax(0,1fr);gap:0}
section{padding:12px 20px;min-width:0}#side{border-left:1px solid var(--line)}
.legend span{display:inline-flex;align-items:center;gap:6px;margin:0 14px 6px 0}
.sw{width:12px;height:12px;border-radius:3px;display:inline-block}
input,select{font:inherit;padding:4px 8px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg)}
table{border-collapse:collapse;width:100%;margin-top:8px}th,td{text-align:left;padding:5px 8px;border-bottom:1px solid var(--line);vertical-align:top}
th{font-weight:600;color:var(--mut);font-size:12px}td{font-family:ui-monospace,monospace;font-size:12px;word-break:break-all}
tr.r{cursor:pointer}tr.r:hover,tr.on{background:var(--sel)}.bad{color:var(--bad);font-weight:600}
svg text{font:12px ui-monospace,monospace;fill:var(--fg)}h2{font-size:14px;margin:16px 0 6px}
</style></head><body>
<header><h1>Clock-domain crossings of <span id="top"></span></h1><div class="mut" id="sum"></div></header>
<main><section><div class="legend" id="legend"></div>
<input id="q" placeholder="Filter by name" size="28"> <select id="kind"><option value="">All crossings</option><option value="bad">With a finding</option><option value="ok">Synchronized</option></select>
<table><thead><tr><th>Source</th><th>Reader</th><th>How</th></tr></thead><tbody id="rows"></tbody></table></section>
<section id="side"><h2>Path</h2><div id="path" class="mut">Pick a crossing.</div>
<h2>Reset synchronizers</h2><table><thead><tr><th>Output</th><th>Stages</th><th>Resets</th></tr></thead><tbody id="rst"></tbody></table></section></main>
<script>
const TOP=)" << json(m.topName) << ",DOMS=" << doms << ",X=[";
        for (size_t i = 0; i < rows.size(); i++) os << (i ? ",\n" : "\n") << rows[i];
        os << "],R=[";
        for (size_t i = 0; i < rsts.size(); i++) os << (i ? ",\n" : "\n") << rsts[i];
        os << R"(];
const PAL=['#2a78d6','#e0730b','#1a9b5a','#c43e9b','#7a5af5','#b8860b','#0f8c8c','#d1495b'];
const col=i=>PAL[i%PAL.length];
const el=(t,a={},...k)=>{const e=document.createElement(t);for(const[n,v]of Object.entries(a))e.setAttribute(n,v);for(const c of k)e.append(c);return e};
document.getElementById('top').textContent=TOP;
const bad=X.filter(x=>x.rule).length;
document.getElementById('sum').textContent=`${DOMS.length} clock domains, ${X.length} crossings, ${bad} with a finding, ${R.length} reset synchronizers`;
const lg=document.getElementById('legend');
DOMS.forEach((d,i)=>lg.append(el('span',{},el('i',{class:'sw',style:'background:'+col(i)}),`${d.name} (${d.flops})`)));
const sw=i=>el('i',{class:'sw',style:'background:'+col(i)+';margin-right:6px'});
function show(x,tr){document.querySelectorAll('tr.on').forEach(r=>r.classList.remove('on'));tr.classList.add('on');
 const n=[{t:x.src,d:x.sd,k:'source'}].concat(x.via.map(v=>({t:v.n,d:-1,k:v.g?'logic':'net'})),x.chain.map((t,i)=>({t,d:x.dd,k:i?'stage '+(i+1):'reader'})));
 if(x.qual)n.push({t:x.qual,d:x.dd,k:'qualifier'});const W=300,H=46,G=26;
 const s=document.createElementNS('http://www.w3.org/2000/svg','svg');s.setAttribute('width',W+20);s.setAttribute('height',n.length*(H+G)+10);
 n.forEach((b,i)=>{const y=5+i*(H+G);const r=document.createElementNS(s.namespaceURI,'rect');
  Object.entries({x:b.d<0?40:10,y,width:b.d<0?W-60:W,height:H,rx:b.d<0?(b.k=='logic'?22:2):6,fill:'none',stroke:b.d<0?'currentColor':col(b.d),'stroke-width':b.d<0?1:2,'stroke-dasharray':b.k=='qualifier'?'5 3':''}).forEach(([k,v])=>r.setAttribute(k,v));s.append(r);
  const t=document.createElementNS(s.namespaceURI,'text');t.setAttribute('x',b.d<0?54:20);t.setAttribute('y',y+19);t.textContent=b.t.length>40?'…'+b.t.slice(-39):b.t;s.append(t);
  const c=document.createElementNS(s.namespaceURI,'text');c.setAttribute('x',b.d<0?54:20);c.setAttribute('y',y+36);c.setAttribute('fill-opacity','.7');c.textContent=b.k+(b.d<0?'':', '+DOMS[b.d].name);s.append(c);
  if(i<n.length-1){const l=document.createElementNS(s.namespaceURI,'line');Object.entries({x1:160,y1:y+H,x2:160,y2:y+H+G,stroke:b.d<0||n[i+1].d!=b.d?'currentColor':col(b.d),'stroke-width':2,'stroke-dasharray':n[i+1].d!=b.d&&n[i+1].d>=0&&b.d!=n[i+1].d?'4 3':''}).forEach(([k,v])=>l.setAttribute(k,v));s.append(l)}});
 const p=document.getElementById('path');p.className='';p.replaceChildren(el('div',{class:x.rule?'bad':'mut'},x.how),s)}
function draw(){const q=document.getElementById('q').value.toLowerCase(),k=document.getElementById('kind').value;const tb=document.getElementById('rows');tb.replaceChildren();
 X.filter(x=>(!q||(x.src+' '+x.dst).toLowerCase().includes(q))&&(!k||(k=='bad')==!!x.rule)).forEach(x=>{
  const tr=el('tr',{class:'r'},el('td',{},sw(x.sd),x.src),el('td',{},sw(x.dd),x.dst),el('td',{class:x.rule?'bad':''},x.how));tr.onclick=()=>show(x,tr);tb.append(tr)})}
document.getElementById('q').oninput=draw;document.getElementById('kind').onchange=draw;draw();
const rb=document.getElementById('rst');R.forEach(r=>rb.append(el('tr',{},el('td',{},sw(r.d),r.out),el('td',{class:r.stages<2?'bad':''},String(r.stages)),el('td',{},String(r.flops)))));
</script></body></html>
)";
        std::cerr << "mlint: -cdc-html " << file << ": " << rows.size() << " crossings\n";
    }

    // -cdc-abstract: the top's model, port by port, for -cdc-model.
    void writeAbstract(const std::string& file) {
        std::map<Sym, std::string> clockName;  // domain -> the top port that is its clock
        std::set<Sym> doms;
        for (const Flop& f : m.flops)
            if (f.dom) doms.insert(f.dom);
        for (const Model::TopPort& p : m.topPorts)
            if (!p.output) {
                Sym d = m.domainOf(m.canon(p.sym));
                if (doms.count(d) && !clockName.count(d)) clockName[d] = p.name;
            }
        std::map<Sym, std::vector<int>> readers;  // input node -> flops that read it
        std::set<Sym> inputs;
        for (const Model::TopPort& p : m.topPorts)
            if (!p.output) inputs.insert(m.canon(p.sym));
        for (size_t i = 0; i < m.flops.size(); i++)
            for (const Root& r : cones[i].all)
                if (inputs.count(r.sym)) readers[r.sym].push_back((int)i);
        auto clockOf = [&](Sym d) -> std::string {
            auto it = clockName.find(d);
            return it == clockName.end() ? std::string() : it->second;
        };
        std::ofstream os(file);
        if (!os) {
            std::cerr << "mlint: -cdc-abstract " << file << ": cannot write\n";
            return;
        }
        os << "# CDC model of " << m.topName << ", written by mlint -cdc-abstract.\n"
              "# Use it with -cdc-model in place of the module's RTL.\n"
              "module " << m.topName << "\n";
        int unknown = 0;
        for (const Model::TopPort& p : m.topPorts) {
            Sym c = m.canon(p.sym);
            std::string kind;
            if (!p.output) {
                bool isClock = false;
                for (auto& [d, n] : clockName) isClock = isClock || n == p.name;
                if (isClock) {
                    kind = "clock";
                } else {
                    std::set<std::string> clocks;
                    bool synced = true, any = false;
                    for (int i : readers[c]) {
                        const Flop& f = m.flops[i];
                        if (!f.dom) continue;
                        any = true;
                        clocks.insert(clockOf(f.dom));
                        const auto& fr = cones[i].from;
                        bool copy = f.plain && fr.size() == 1 && fr[0].sym == c && !fr[0].logic;
                        synced = synced && copy && chainLen(i) >= 2;
                    }
                    if (!any) kind = "comb";
                    else if (clocks.size() != 1 || clocks.begin()->empty()) kind = "mixed";
                    else kind = std::string(synced ? "sync " : "data ") + *clocks.begin();
                }
            } else {
                Tracer tr(m);
                std::set<std::string> clocks;
                bool flops = false;
                for (const Root& r : tr.roots(c))
                    if (const Flop* f = m.flopOf(r.sym); f && f->dom) {
                        flops = true;
                        clocks.insert(clockOf(f->dom));
                    }
                if (!flops) kind = "comb";
                else if (clocks.size() != 1 || clocks.begin()->empty()) kind = "mixed";
                else kind = "data " + *clocks.begin();
            }
            if (kind == "mixed") unknown++;
            os << "  " << (p.output ? "output " : "input  ") << p.name << "  " << kind << "\n";
        }
        os << "end\n";
        std::cerr << "mlint: -cdc-abstract " << file << ": " << m.topPorts.size() << " ports";
        if (unknown) std::cerr << ", " << unknown << " mixed (more than one clock, or a clock that is not a port)";
        std::cerr << "\n";
    }

    // -cdc-sva: a module of assertions on the crossings, by hierarchical
    // name, to compile next to the design (an extra top, or bound into it).
    // What makes each kind of crossing safe:
    //   a one-bit synchronizer: the source holds a value for two reading
    //     edges, or a pulse can fall between them (counted on every change,
    //     since sampling on the reading clock can miss a fast toggle);
    //   a Gray-coded or multi-bit bus: at most one bit changes per source
    //     edge, so the reader sees the old or the new value;
    //   an enable-qualified bus: the data does not change on the reading edge
    //     where the synchronized control changes;
    //   a request that reads a synchronized acknowledge back (a handshake):
    //     it changes only when the acknowledge has caught up with it.
    void writeSva(const std::string& file) {
        auto ok = [](const std::string& n) { return !n.empty() && n.find("<protected>") == std::string::npos; };
        auto edge = [&](const Flop& f) {
            return std::string(f.negedge ? "negedge " : "posedge ") + m.name(f.dom);
        };
        // While a flop is in reset its value is not a crossing yet, and
        // $past has no history: the check starts when the reset is released.
        auto clocking = [&](const Flop& f) {
            std::string c = "@(" + edge(f) + ")";
            if (!f.arst.empty() && ok(m.name(f.arst[0])))
                c += std::string(" disable iff (") + (f.arstHigh ? "" : "!") + m.name(f.arst[0]) + ")";
            return c;
        };
        // A change while the source is in reset (its reset value) is not a
        // crossing.
        auto guard = [&](const Flop& f) {
            if (f.arst.empty() || !ok(m.name(f.arst[0]))) return std::string();
            return std::string("if (") + (f.arstHigh ? "!" : "") + m.name(f.arst[0]) + ") ";
        };
        auto ident = [](std::string n) {
            for (char& c : n)
                if (!isalnum((unsigned char)c) && c != '_') c = '_';
            while (n.find("__") != std::string::npos) n.erase(n.find("__"), 1);
            return n;
        };
        struct Prop {
            std::string key, text;
        };
        std::vector<Prop> props;
        std::set<std::string> seen;
        int skipped = 0;
        for (const Crossing& c : crossings) {
            if (c.check == NoCheck) continue;
            const Flop& f = m.flops[c.flop];
            const Flop& s = *m.flopOf(c.src);
            std::string src = m.name(s), dst = m.name(f);
            if (!ok(src) || !ok(dst) || !ok(m.name(f.dom)) || !ok(m.name(s.dom))) {
                skipped++;
                continue;
            }
            std::string label, body, what;
            switch (c.check) {
                case Stable:
                    // Sampled on the reading clock a fast toggle can look
                    // stable, so count the reading edges between changes.
                    label = "stable_" + ident(src) + "_to_" + ident(m.name(f.dom));
                    what = src + " changed again before two " + m.name(f.dom) + " edges: " + dst +
                           " can miss a value";
                    break;
                case OneBit:
                    label = "onebit_" + ident(src);
                    body = clocking(s) + " $countones(" + src + " ^ $past(" + src + ")) <= 1";
                    what = "more than one bit of " + src + " changed at once: " + dst + " can see a value it never had";
                    break;
                case Qualified: {
                    // A memory (a FIFO's storage, wider than what the reader
                    // takes, or an unpacked array) is written one entry at a
                    // time while the reader takes another: the whole of it is
                    // not held, so it is not checked as one value.
                    if (s.bits > f.bits ||
                        (s.sym && s.sym->isValue() && s.sym->as<ValueSymbol>().getType().isUnpackedArray()))
                        continue;
                    const Flop* q = c.qual ? m.flopOf(c.qual) : nullptr;
                    if (!q || !ok(m.name(*q))) {
                        skipped++;
                        continue;
                    }
                    label = "held_" + ident(src) + "_at_" + ident(m.name(*q));
                    body = clocking(f) + " $changed(" + m.name(*q) + ") |-> $stable(" + src + ")";
                    what = src + " changed as " + m.name(*q) + " said to take it into " + dst;
                    break;
                }
                default: break;
            }
            if (!seen.insert(label).second) continue;
            if (c.check == Stable) {
                // A handshake: the source also reads a synchronized copy of a
                // signal from the reading domain (an acknowledge). The
                // request may then change only once the acknowledge has
                // caught up with it: two-phase (toggle) or four-phase alike.
                // A plain copy (the responder's ack) just follows.
                const Flop* ack = nullptr;
                int deepest = 0;
                if (!s.plain)
                    for (const Root& r : cones[m.flopIx.at(c.src)].all) {
                        const Flop* af = m.flopOf(r.sym);
                        if (!af || af->dom != s.dom) continue;
                        const Member& mem = member[af - m.flops.data()];
                        const Flop* from = mem.src ? m.flopOf(mem.src) : nullptr;
                        if (mem.pos < 2 || !from || from->dom != f.dom || mem.pos <= deepest) continue;
                        ack = af;
                        deepest = mem.pos;
                    }
                if (ack && ok(m.name(*ack)) && !seen.count("handshake_" + ident(src))) {
                    std::string hl = "handshake_" + ident(src), a = m.name(*ack);
                    seen.insert(hl);
                    // Each side changes only after the other side's (synchronized)
                    // signal changed since its own last change; the first change
                    // after reset may start. That holds for the side that starts
                    // a transfer and the side that answers, two- or four-phase.
                    std::string px = "px_" + hl, py = "py_" + hl, sn = "seen_" + hl, fs = "first_" + hl;
                    std::string rst = s.arst.empty() || !ok(m.name(s.arst[0])) ? std::string("1'b0")
                                      : std::string(s.arstHigh ? "" : "!") + m.name(s.arst[0]);
                    props.push_back({hl, "  // " + src + " (" + m.name(s.dom) + ") -> " + dst + " (" + m.name(f.dom) +
                                             "): handshake, answered through " + a + "\n" +
                                             "  logic " + px + ", " + py + ";\n  bit " + sn + " = 0, " + fs + " = 1;\n" +
                                             "  always @(" + edge(s) + ") begin\n" +
                                             "    if (" + rst + ") begin " + fs + " <= 1; " + sn + " <= 0; end\n" +
                                             "    else if (" + src + " != " + px + ") begin\n" +
                                             "      cdc_" + hl + ": assert (" + sn + " || " + fs + ") else $error(\"[mlint cdc] " +
                                             src + " changed again before " + a + " answered its last change\");\n" +
                                             "      " + fs + " <= 0;\n      " + sn + " <= " + a + " != " + py + ";\n" +
                                             "    end else if (" + a + " != " + py + ") " + sn + " <= 1;\n" +
                                             "    " + px + " <= " + src + ";\n    " + py + " <= " + a + ";\n  end\n"});
                }
            }
            std::string t = "  // " + src + " (" + m.name(s.dom) + ") -> " + dst + " (" + m.name(f.dom) + "): " +
                            c.how + "\n";
            if (c.check == Stable) {
                std::string n = "n_" + label;
                t += "  int unsigned " + n + " = 2;  // " + m.name(f.dom) + " edges since " + src + " changed\n" +
                     "  always @(" + edge(f) + ") if (" + n + " < 2) " + n + "++;\n" +
                     "  always @(" + src + ") " + guard(s) + "begin\n    cdc_" + label + ": assert (" + n +
                     " >= 2) else $error(\"[mlint cdc] " + what + "\");\n    " + n + " = 0;\n  end\n";
            } else {
                t += "  cdc_" + label + ": assert property (" + body + ")\n    else $error(\"[mlint cdc] " + what +
                     "\");\n";
            }
            props.push_back({label, t});
        }
        std::sort(props.begin(), props.end(), [](const Prop& a, const Prop& b) { return a.key < b.key; });
        std::ofstream os(file);
        if (!os) {
            std::cerr << "mlint: -cdc-sva " << file << ": cannot write\n";
            return;
        }
        os << "// Clock-domain crossing assertions, written by mlint -cdc-sva.\n"
              "// Compile with the design and add mlint_cdc_sva as a second top\n"
              "// (msim ... -top <top> -top mlint_cdc_sva), or bind it into the top.\n"
              "module mlint_cdc_sva;\n";
        for (auto& p : props) os << p.text;
        os << "endmodule\n";
        std::cerr << "mlint: -cdc-sva " << file << ": " << props.size()
                  << (props.size() == 1 ? " assertion" : " assertions");
        if (skipped) std::cerr << " (" << skipped << " crossings in protected code skipped)";
        std::cerr << "\n";
    }

    void report() {
        std::map<std::string, int> doms, rsts;
        std::map<std::string, std::string> sdcClocks;
        for (const Flop& f : m.flops) {
            if (f.dom) doms[m.name(f.dom)]++;
            if (f.dom && m.sdc.on && !sdcClocks.count(m.name(f.dom))) {
                std::string t;
                for (int c : m.clocksOf(f.dom)) t += (t.empty() ? "" : ", ") + m.sdc.clocks[c];
                sdcClocks[m.name(f.dom)] = t.empty() ? "  no SDC clock" : "  SDC clock " + t;
            }
            for (Sym a : f.arst) rsts[m.name(m.resetRoot(a))]++;
        }
        std::ostream& os = std::cout;
        os << "clock domains: " << doms.size() << "\n";
        for (auto& [d, k] : doms) os << "  " << d << "  " << k << (k == 1 ? " flop" : " flops") << sdcClocks[d] << "\n";
        os << "asynchronous resets: " << rsts.size() << "\n";
        for (auto& [d, k] : rsts) os << "  " << d << "  " << k << (k == 1 ? " flop\n" : " flops\n");
        std::vector<std::string> lines;
        for (const ResetSync& r : resetSyncs)
            lines.push_back("  " + m.name(*r.out) + " (" + clocked(*r.out) + ")  " + std::to_string(r.stages) +
                            (r.stages == 1 ? " flop" : " flops") + ", resets " + std::to_string(r.flops) +
                            (r.flops == 1 ? " flop" : " flops") +
                            (r.out->arst.empty() ? "" : ", from " + m.name(m.resetRoot(r.out->arst[0]))));
        std::sort(lines.begin(), lines.end());
        os << "reset synchronizers: " << lines.size() << "\n";
        for (auto& l : lines) os << l << "\n";
        lines.clear();
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
            if (isSignal(*s)) offer(s, m.pathOf(s));
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

// ------------------------------------------------------------------ -sdc
//
// The design as an SDC file names it, in the convention of msdc: '/' between
// instance levels and no top name (u_a/u_b/clk), generate blocks as `name.`
// and `name[i].`, a register `q` as the cell `q_reg` with pins CK, D and Q,
// bus bits as name[3].
struct SdcView : cdcsdc::Design {
    Model& m;
    std::vector<std::string> ports, pins, cells, nets;
    std::unordered_map<std::string, int> dir;
    std::unordered_map<std::string, Sym> node;  // port, net or pin -> canonical node

    explicit SdcView(Model& m) : m(m) {}

    void put(std::vector<std::string>& into, const std::string& name, const Type& t, Sym s) {
        Sym c = m.canon(s);
        node[name] = c;
        if (t.isIntegral() && t.getBitWidth() > 1 && t.getBitWidth() <= 4096 && t.hasFixedRange()) {
            auto rg = t.getFixedRange();
            for (int i = rg.upper(); i >= rg.lower(); i--) {
                std::string b = name + "[" + std::to_string(i) + "]";
                auto it = m.bitCanon.find({c, i});
                node[b] = it == m.bitCanon.end() ? c : m.canon(it->second);
                into.push_back(std::move(b));
            }
        } else {
            into.push_back(name);
        }
    }
    void walk(const Scope& sc, const std::string& pre) {
        for (const Symbol& s : sc.members()) {
            if (namePC() && !namePC()->visible(s)) continue;  // protected IP keeps its names
            if (isSignal(s)) {
                std::string nm = pre + std::string(s.name);
                put(nets, nm, s.as<ValueSymbol>().getType(), &s);
                if (const Flop* f = m.flopOf(m.canon(&s)); f && f->sym == &s) {
                    cells.push_back(nm + "_reg");
                    for (const char* p : {"/Q", "/D", "/CK"}) pins.push_back(nm + "_reg" + p);
                    node[nm + "_reg/Q"] = m.canon(&s);
                    if (f->clkSym) node[nm + "_reg/CK"] = m.canon(f->clkSym);
                }
            } else if (s.kind == SymbolKind::Instance) {
                const auto& ch = s.as<InstanceSymbol>();
                if (!ch.isModule() || ch.body.flags.has(InstanceFlags::Uninstantiated)) continue;
                std::string p = pre + std::string(ch.name);
                cells.push_back(p);
                for (const Symbol* ps : ch.body.getPortList()) {
                    if (ps->kind != SymbolKind::Port) continue;
                    const auto& port = ps->as<PortSymbol>();
                    if (port.internalSymbol && isSignal(*port.internalSymbol))
                        put(pins, p + "/" + std::string(port.name), port.getType(), port.internalSymbol);
                }
                walk(ch.body, p + "/");
            } else if (s.kind == SymbolKind::GenerateBlock) {
                const auto& g = s.as<GenerateBlockSymbol>();
                if (!g.isUninstantiated) walk(g, pre + g.getExternalName() + ".");
            } else if (s.kind == SymbolKind::GenerateBlockArray) {
                const auto& ga = s.as<GenerateBlockArraySymbol>();
                std::string an = ga.name.empty() ? std::string("genblk") : std::string(ga.name);
                for (const auto* e : ga.entries)
                    if (!e->isUninstantiated)
                        walk(*e, pre + an + "[" + (e->getArrayIndex() ? e->getArrayIndex()->toString() : std::string("?")) + "].");
            }
        }
    }
    void build(Compilation& comp) {
        for (const InstanceSymbol* top : comp.getRoot().topInstances) {
            for (const Symbol* ps : top->body.getPortList()) {
                if (ps->kind != SymbolKind::Port) continue;
                const auto& port = ps->as<PortSymbol>();
                if (!port.internalSymbol || !isSignal(*port.internalSymbol)) continue;
                size_t at = ports.size();
                put(ports, std::string(port.name), port.getType(), port.internalSymbol);
                int d = port.direction == ArgumentDirection::In ? 0 : port.direction == ArgumentDirection::Out ? 1 : 2;
                dir[std::string(port.name)] = d;
                for (size_t i = at; i < ports.size(); i++) dir[ports[i]] = d;
            }
            walk(top->body, "");
        }
    }

    const std::vector<std::string>& names(cdcsdc::Kind k) const override {
        static const std::vector<std::string> none;
        switch (k) {
            case cdcsdc::Kind::Port: return ports;
            case cdcsdc::Kind::Pin: return pins;
            case cdcsdc::Kind::Cell: return cells;
            case cdcsdc::Kind::Net: return nets;
            default: return none;
        }
    }
    int portDir(const std::string& n) const override {
        auto it = dir.find(n);
        return it == dir.end() ? -1 : it->second;
    }
    Sym at(const cdcsdc::Obj& o) const {
        if (o.kind == cdcsdc::Kind::Cell) {  // a register cell stands for its output
            auto it = node.find(o.name + "/Q");
            return it == node.end() ? nullptr : it->second;
        }
        auto it = node.find(o.name);
        return it == node.end() ? nullptr : it->second;
    }
};


// ------------------------------------------------------------- block models
//
// A CDC model of a block says, port by port, what the crossing checks need to
// know about it, so a block can be checked once and then stand in for itself
// in the SoC (or be a black box whose RTL is not there):
//
//   module cdc_block
//     input  clk_a   clock               # a clock
//     input  d       data clk_a          # read by flops clocked by clk_a
//     input  req     sync clk_b          # synchronized into clk_b inside
//     output q       data clk_b          # launched by a flop clocked by clk_b
//     output y       comb                # combinational from inputs only
//   end
inline void readModels(Model& m, const std::vector<std::string>& files) {
    for (const std::string& file : files) {
        std::ifstream in(file);
        if (!in) {
            std::cerr << "mlint: -cdc-model " << file << ": cannot open\n";
            continue;
        }
        std::string line, mod;
        int n = 0;
        while (std::getline(in, line)) {
            n++;
            if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
            std::istringstream ws(line);
            std::vector<std::string> w;
            for (std::string x; ws >> x;) w.push_back(x);
            if (w.empty()) continue;
            auto bad = [&](const char* why) {
                std::cerr << "mlint: " << file << ":" << n << ": " << why << "\n";
            };
            if (w[0] == "module" && w.size() == 2) {
                mod = w[1];
                m.blockModels[mod];
            } else if (w[0] == "end") {
                mod.clear();
            } else if ((w[0] == "input" || w[0] == "output") && w.size() >= 3 && !mod.empty()) {
                Model::PortModel p{w[1], w[2], w.size() > 3 ? w[3] : "", w[0] == "output"};
                bool known = p.kind == "clock" || p.kind == "comb" || p.kind == "mixed" ||
                             ((p.kind == "data" || p.kind == "sync") && !p.clock.empty());
                if (!known) { bad("expected clock, data CLK, sync CLK, comb or mixed"); continue; }
                m.blockModels[mod].push_back(p);
            } else {
                bad("expected module NAME, input/output PORT KIND [CLOCK], or end");
            }
        }
    }
}

// After the design is gathered: each modeled instance becomes the flops its
// model describes, on the nets its ports connect to.
inline void applyModels(Model& m, Compilation& comp) {
    for (const InstanceSymbol* inst : m.modeled) {
        const auto& ports = m.blockModels.at(std::string(inst->getDefinition().name));
        std::map<std::string, const ValueSymbol*> sym;
        for (const Symbol* ps : inst->body.getPortList())
            if (ps->kind == SymbolKind::Port)
                if (const Symbol* in = ps->as<PortSymbol>().internalSymbol; in && isSignal(*in))
                    sym[std::string(ps->name)] = &in->as<ValueSymbol>();
        std::string here = path(*inst);
        for (const Model::PortModel& p : ports) {
            auto it = sym.find(p.port);
            const ValueSymbol* clk = p.clock.empty() ? nullptr : (sym.count(p.clock) ? sym[p.clock] : nullptr);
            if (it == sym.end() || (!p.clock.empty() && !clk)) {
                std::cerr << "mlint: -cdc-model " << inst->getDefinition().name << ": no port "
                          << (it == sym.end() ? p.port : p.clock) << "\n";
                continue;
            }
            const ValueSymbol* port = it->second;
            if (p.kind != "data" && p.kind != "sync") continue;
            if (p.output) {  // launched by a flop of that clock
                Flop& f = m.flop(port, inst->location);
                f.clkSym = clk;
                continue;
            }
            // Read by one flop (data), or by a two-flop synchronizer (sync).
            Sym from = m.canon(port);
            for (int stage = 1; stage <= (p.kind == "sync" ? 2 : 1); stage++) {
                m.sliceNames.push_back(here + "." + p.port + (p.kind == "sync" ? " (synchronizer " +
                                       std::to_string(stage) + " in the model)" : " (read in the model)"));
                auto* v = comp.emplace<VariableSymbol>(std::string_view(m.sliceNames.back()), inst->location,
                                                       VariableLifetime::Static);
                v->setType(port->getType());
                m.sliceLabel[v] = &m.sliceNames.back();
                Flop& f = m.flop(v, inst->location);
                f.clkSym = clk;
                f.data = {from};
                f.from = from;
                f.fromBits = f.bits;
                f.plain = p.kind == "sync";  // data: logic in the block reads it
                from = v;
            }
        }
    }
}

// Reads the SDC files: where each clock starts and how each pair is related.
//   - a generated clock is synchronous to its master;
//   - set_clock_groups -asynchronous makes the groups asynchronous to each
//     other, -logically_exclusive and -physically_exclusive make them never
//     present together (a path between them is not checked);
//   - set_false_path between two clocks makes them asynchronous: timing no
//     longer covers the path, so a synchronizer has to;
//   - any other pair is synchronous, as it is for timing. A file that relates
//     no clocks at all would make every crossing vanish, so its primary clocks
//     are taken as asynchronous to each other, with a note.
inline void readSdc(Model& m, Compilation& comp, const Options& opt) {
    if (opt.sdcFiles.empty()) return;
    SdcView view(m);
    view.build(comp);
    cdcsdc::Reader rd;
    rd.design = &view;
    size_t read = 0;
    for (auto& f : opt.sdcFiles) {
        try {
            rd.load(f);
            read++;
        } catch (const std::exception& e) {
            std::cerr << "mlint: -sdc " << f << ": " << e.what() << "\n";
        }
    }
    if (!read) return;  // nothing to go by: check as without -sdc
    for (auto& d : rd.diags)
        if (d.sev == cdcsdc::Diag::ErrorSev || d.code == "no-match" || d.code == "unknown-command")
            std::cerr << "mlint: " << d.loc.str() << ": " << d.text << "\n";

    Model::Sdc& s = m.sdc;
    s.on = true;
    const auto& clocks = rd.sdc.clocks;
    const int n = (int)clocks.size();
    std::unordered_map<std::string, int> ix;
    for (int i = 0; i < n; i++) {
        s.clocks.push_back(clocks[i].name);
        ix[clocks[i].name] = i;
        for (auto& o : clocks[i].sources)
            if (Sym c = view.at(o)) s.at[c].push_back(i);
    }
    // The master of a generated clock: named, or the clock that reaches its
    // -source pin.
    std::vector<int> master(n, -1);
    for (int i = 0; i < n; i++) {
        const cdcsdc::Clock& c = clocks[i];
        if (!c.generated) continue;
        if (auto it = ix.find(c.masterClock); it != ix.end()) master[i] = it->second;
        for (auto& o : c.masterPins) {
            if (master[i] >= 0) break;
            Sym at = view.at(o);
            if (!at) continue;
            for (int k : m.clocksReaching(at))
                if (k != i) { master[i] = k; break; }
        }
    }
    auto root = [&](int i) {
        for (int hops = 0; master[i] >= 0 && hops < n; hops++) i = master[i];
        return i;
    };
    // Periods: a generated clock's is its master's times divide_by over
    // multiply_by (none with -edges, or when the master's is not known).
    s.period.assign(n, 0);
    for (int i = 0; i < n; i++) s.period[i] = clocks[i].generated ? 0 : clocks[i].period;
    for (int pass = 0; pass < n; pass++)
        for (int i = 0; i < n; i++)
            if (clocks[i].generated && !clocks[i].edges && !s.period[i] && master[i] >= 0 && s.period[master[i]])
                s.period[i] = s.period[master[i]] * clocks[i].divideBy / clocks[i].multiplyBy;
    bool related = false;
    auto clockPair = [&](const cdcsdc::Exception& e) {
        if (e.type != cdcsdc::Exception::FalsePath || e.path.from.empty() || e.path.to.empty() || !e.path.through.empty())
            return false;
        for (auto* l : {&e.path.from, &e.path.to})
            for (auto& o : *l)
                if (o.kind != cdcsdc::Kind::Clock) return false;
        return true;
    };
    for (auto& g : rd.sdc.clockGroups) related = related || g.type != cdcsdc::ClockGroups::None;
    for (auto& e : rd.sdc.exceptions) related = related || clockPair(e);
    s.rel.assign(n, std::vector<uint8_t>(n, Model::Sync));
    if (!related) {
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (root(i) != root(j)) s.rel[i][j] = Model::Async;
        if (n > 1)
            std::cerr << "mlint: -sdc: no set_clock_groups and no false path between clocks: clocks that are "
                         "not generated from one another are taken as asynchronous\n";
    }
    for (auto& g : rd.sdc.clockGroups) {
        if (g.type == cdcsdc::ClockGroups::None) continue;
        // A clock's group; a generated clock that is not listed goes with its
        // master. One -group alone is against every other clock.
        std::vector<int> of(n, -1);
        for (size_t k = 0; k < g.groups.size(); k++)
            for (auto& name : g.groups[k])
                if (auto it = ix.find(name); it != ix.end()) of[it->second] = (int)k;
        for (int i = 0; i < n; i++)
            for (int j = i, hops = 0; of[i] < 0 && master[j] >= 0 && hops < n; hops++) of[i] = of[j = master[j]];
        if (g.groups.size() == 1)
            for (int i = 0; i < n; i++)
                if (of[i] < 0) of[i] = 1;
        uint8_t r = g.type == cdcsdc::ClockGroups::Asynchronous ? Model::Async : Model::Exclusive;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (of[i] >= 0 && of[j] >= 0 && of[i] != of[j]) s.rel[i][j] = r;
    }
    for (auto& e : rd.sdc.exceptions) {
        if (!clockPair(e)) continue;
        for (auto& a : e.path.from)
            for (auto& b : e.path.to) {
                auto i = ix.find(a.name), j = ix.find(b.name);
                if (i == ix.end() || j == ix.end() || i->second == j->second) continue;
                if (s.rel[i->second][j->second] == Model::Exclusive) continue;
                s.rel[i->second][j->second] = s.rel[j->second][i->second] = Model::Async;
            }
    }

    // set_case_analysis 0/1: the signal is constant, like -static.
    for (const cdcsdc::CaseAnalysis& ca : rd.sdc.caseAnalysis)
        if (ca.value == "0" || ca.value == "1" || ca.value == "zero" || ca.value == "one")
            for (const cdcsdc::Obj& o : ca.objects)
                if (Sym c = view.at(o)) m.statics.insert(c);

    // Ports: set_input_delay -clock C says a flop clocked by C (outside the
    // design) launches the input, set_output_delay -clock C that one captures
    // the output. Each becomes a flop of that clock on the port, so a path
    // from an input or to an output is checked like any other. A virtual
    // clock (no source) gets a node of its own.
    std::vector<Sym> clockNode(n, nullptr);
    for (auto& [node, list] : s.at)
        for (int i : list)
            if (!clockNode[i] || m.pathOf(node) < m.pathOf(clockNode[i])) clockNode[i] = node;
    auto pseudo = [&](const std::string& label, const Type& t, SourceLocation loc) {
        m.sliceNames.push_back(label);
        auto* v = comp.emplace<VariableSymbol>(std::string_view(m.sliceNames.back()), loc, VariableLifetime::Static);
        v->setType(t);
        m.sliceLabel[v] = &m.sliceNames.back();
        return v;
    };
    std::map<std::string, const PortSymbol*> topPort;
    for (const InstanceSymbol* top : comp.getRoot().topInstances)
        for (const Symbol* ps : top->body.getPortList())
            if (ps->kind == SymbolKind::Port && !topPort.count(std::string(ps->name)))
                topPort[std::string(ps->name)] = &ps->as<PortSymbol>();
    std::set<std::pair<std::string, int>> done;
    for (const cdcsdc::IoDelay& d : rd.sdc.ioDelays) {
        auto ci = ix.find(d.clock);
        if (d.clock.empty() || ci == ix.end()) continue;
        int c = ci->second;
        if (!clockNode[c]) {
            Sym v = pseudo("virtual clock " + d.clock, comp.getLogicType(), SourceLocation::NoLocation);
            s.at[v].push_back(c);
            clockNode[c] = v;
        }
        for (const cdcsdc::Obj& o : d.ports) {
            std::string base = o.name.substr(0, o.name.find('['));
            auto pit = topPort.find(base);
            if (pit == topPort.end() || !pit->second->internalSymbol || !isSignal(*pit->second->internalSymbol)) continue;
            if (!done.insert({base + (d.input ? "<" : ">"), c}).second) continue;
            Sym port = pit->second->internalSymbol;
            Sym node = m.canon(port);
            if (s.at.count(node)) continue;  // a clock pin
            const Type& t = port->as<ValueSymbol>().getType();
            std::string label = m.pathOf(port) + (d.input ? " (launched by " : " (captured by ") + d.clock + ")";
            Sym v = pseudo(label, t, port->location);
            Flop& f = m.flop(v, port->location);
            f.clkSym = clockNode[c];
            if (d.input) {
                m.addComb(port, {m.canon(v)}, true, nullptr);  // the input is that flop
            } else {
                f.data = {node};
                f.from = node;
                f.fromBits = f.bits;
                f.plain = false;  // the logic outside reads it
            }
        }
    }
}

inline void check(Compilation& comp, Report& rep, const Options& opt) {
    Prof prof;
    auto lap = [&](const char* what) { prof.lap(what); };
    Model m;
    readModels(m, opt.models);
    for (const InstanceSymbol* top : comp.getRoot().topInstances) {
        if (!m.topName.empty()) break;
        m.topName = std::string(top->getDefinition().name);
        for (const Symbol* ps : top->body.getPortList())
            if (ps->kind == SymbolKind::Port)
                if (const Symbol* in = ps->as<PortSymbol>().internalSymbol; in && isSignal(*in))
                    m.topPorts.push_back({std::string(ps->name),
                                          ps->as<PortSymbol>().direction == ArgumentDirection::Out, in});
    }
    Slicer slicer(m, comp);
    comp.getRoot().visit(slicer);
    slicer.finish();
    lap("slices");
    Ports ports(m);
    comp.getRoot().visit(ports);
    ports.finish();
    lap("ports");
    Gather g(m);
    comp.getRoot().visit(g);
    ports.finishBits();
    applyModels(m, comp);
    lap("gather");
    m.sealCombs();
    m.nameNodes();
    readSdc(m, comp, opt);
    constrain(m, opt);
    m.finish();
    lap("model");
    Checker c(m, rep, opt);
    c.run();
}

}  // namespace cdc
