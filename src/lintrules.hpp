#pragma once
// lintrules.hpp - mlint's rule checks beyond the first four (L001..L004) and
// the clock-domain rules. The rule table, with what each rule means, is in
// lintcfg.hpp; docs/LINT.md is the user's view of it.
//
// Three passes, so that the work can be spread over threads:
//
//   * BodyRules  one elaborated module body at a time. Each distinct body
//                (a module with one set of parameter values) is checked once,
//                however many times it is instantiated. Bodies are
//                independent, so they run in parallel.
//   * Global     one walk of the whole hierarchy for what crosses module
//                boundaries: combinational loops, clocks used as data, and
//                resets used inconsistently. Signals are joined across port
//                connections.
//   * the definitions (files and module names), and the unused-signal rules,
//     which need every body's reads and writes merged.
#include <regex>

#include "lintcfg.hpp"
#include "fsm_extract.hpp"
#include "lintir.hpp"
#include "slang/ast/EvalContext.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/symbols/AttributeSymbol.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/syntax/AllSyntax.h"

namespace lintrules {

using namespace lintir;
using lintcfg::Config;

// ------------------------------------------------------------------ parameters

struct Pat {
    std::regex re;
    std::string src;
    bool ok(std::string_view n) const { return std::regex_search(n.begin(), n.end(), re); }
};

// What `set` can change, resolved once.
struct Params {
    Pat module, signal, param, clock, reset, activeLow, instance, portIn, portOut, typedefName, unusedOk, macro, interface, constraint;
    size_t lineLength = 100;
    size_t maxLen = 40;
    std::vector<std::string> tbGlobs;

    static Params from(const Config& cfg, std::vector<std::string>& errors) {
        Params p;
        auto pat = [&](Pat& out, const char* key, const char* dflt, bool icase = false) {
            out.src = cfg.param(key, dflt);
            try {
                out.re = std::regex(out.src, icase ? std::regex::icase | std::regex::ECMAScript : std::regex::ECMAScript);
            } catch (const std::regex_error&) {
                errors.push_back(std::string("set ") + key + ": '" + out.src + "' is not a regular expression");
                out.re = std::regex(dflt);
            }
        };
        pat(p.module, "name.module", "^[a-z][a-z0-9_]*$");
        pat(p.signal, "name.signal", "^[a-z][a-z0-9_]*$");
        pat(p.param, "name.param", "^[A-Z][A-Za-z0-9_]*$");
        pat(p.clock, "name.clock", "clk|clock|ck", true);
        pat(p.reset, "name.reset", "rst|reset|clr|clear", true);
        pat(p.activeLow, "name.active_low", "(_n|_ni|_n_i|_b|_an|n)$");
        pat(p.instance, "name.instance", "^(u|i|g)_");
        pat(p.portIn, "name.port_in", "_i$");
        pat(p.portOut, "name.port_out", "_o$");
        pat(p.typedefName, "name.typedef", "_(t|e|s|u)$");
        pat(p.macro, "name.macro", "^[A-Z_][A-Z0-9_]*$");
        pat(p.interface, "name.interface", "_if$");
        pat(p.constraint, "name.constraint", "_c$");
        p.lineLength = (size_t)std::max(20, std::atoi(cfg.param("fmt.line_length", "100").c_str()));
        pat(p.unusedOk, "unused.ignore", "^_?unused|_unused$|^_?nc_|_nc$", true);
        p.maxLen = (size_t)std::max(1, std::atoi(cfg.param("name.max_length", "40").c_str()));
        p.tbGlobs = lintcfg::splitList(cfg.param("tb.modules", ""));
        static const std::set<std::string> keys = {
            "name.module", "name.signal", "name.param", "name.clock", "name.reset", "name.active_low",
            "name.instance", "name.port_in", "name.port_out", "name.typedef", "unused.ignore",
            "name.max_length", "tb.modules", "name.macro", "fmt.line_length", "name.interface", "name.constraint", "formal.max_bits", "formal.reg_bits",
            // slang-tidy's settings (linttidy.hpp)
            "tidy.clock", "tidy.reset", "tidy.reset_active_high", "tidy.enum", "tidy.struct", "tidy.union",
            "tidy.covergroup", "tidy.coverpoint", "tidy.cross"};
        for (auto& [k, v] : cfg.params)
            if (!keys.count(k)) errors.push_back("set " + k + ": no such setting");
        return p;
    }
};

// ---------------------------------------------------------------------- helpers

inline const Expression& strip(const Expression& e) { return e.unwrapImplicitConversions(); }
inline bool integral(const Expression& e) { return e.type->isIntegral(); }
inline bitwidth_t widthOf(const Expression& e) { return e.type->getBitWidth(); }
inline bool isName(const Expression& e) {
    return e.kind == ExpressionKind::NamedValue || e.kind == ExpressionKind::HierarchicalValue;
}
inline const Symbol* nameOf(const Expression& e) {
    if (e.kind == ExpressionKind::NamedValue) return &e.as<NamedValueExpression>().symbol;
    if (e.kind == ExpressionKind::HierarchicalValue) return &e.as<HierarchicalValueExpression>().symbol;
    return nullptr;
}
inline bool isSignal(const Symbol& s) { return s.kind == SymbolKind::Variable || s.kind == SymbolKind::Net; }

// Declared in a module or a generate block, not inside a process or a function.
inline bool moduleLevel(const Symbol& s) {
    const Scope* sc = s.getParentScope();
    if (!sc) return false;
    SymbolKind k = sc->asSymbol().kind;
    return k == SymbolKind::InstanceBody || k == SymbolKind::GenerateBlock;
}

inline const InstanceBodySymbol* bodyOf(const Symbol& s) {
    for (const Scope* sc = s.getParentScope(); sc; sc = sc->asSymbol().getParentScope())
        if (sc->asSymbol().kind == SymbolKind::InstanceBody) return &sc->asSymbol().as<InstanceBodySymbol>();
    return nullptr;
}

// A testbench: a program, a module with no ports, or a name the user listed.
inline bool isTb(const InstanceBodySymbol& b, const Params& P) {
    const DefinitionSymbol& def = b.getDefinition();
    if (def.definitionKind == DefinitionKind::Program) return true;
    for (auto& g : P.tbGlobs)
        if (lintcfg::glob(g, def.name)) return true;
    return def.definitionKind == DefinitionKind::Module && b.getPortList().empty();
}

// A value known at elaboration: literals, parameters, and pure functions of them.
inline bool isConstExpr(const Expression& e) {
    bool c = true;
    e.visit(makeVisitor(
        [&](auto&, const NamedValueExpression& n) {
            SymbolKind k = n.symbol.kind;
            if (k != SymbolKind::Parameter && k != SymbolKind::EnumValue && k != SymbolKind::Genvar) c = false;
        },
        [&](auto&, const HierarchicalValueExpression& n) {
            if (n.symbol.kind != SymbolKind::Parameter) c = false;
        },
        [&](auto& v, const CallExpression& call) {
            if (call.isSystemCall()) {
                std::string_view n = call.getSubroutineName();
                if (n == "$bits" || n == "$size" || n == "$high" || n == "$low" || n == "$left" ||
                    n == "$right" || n == "$dimensions" || n == "$typename" || n == "$increment")
                    return;  // about the type, whatever the argument
                if (n != "$clog2" && n != "$signed" && n != "$unsigned" && n != "$countones" && n != "$onehot" &&
                    n != "$onehot0" && n != "$countbits" && n != "$unpacked_dimensions")
                    c = false;
            }
            v.visitDefault(call);
        }));
    return c;
}

// The value of a constant expression as true / false, if it has one.
inline std::optional<bool> constTruth(const Expression& e, const Scope& scope) {
    if (!isConstExpr(e)) return std::nullopt;
    EvalContext ctx(ASTContext(scope, LookupLocation::max));
    ConstantValue cv = e.eval(ctx);
    if (!cv.isInteger() || cv.integer().hasUnknown()) return std::nullopt;
    return (bool)cv.integer().reductionOr();
}

// The bits a value needs. For a constant, the bits of its value (Depth - 1
// with Depth = 8 needs 3); otherwise the front end's effective width, in
// which an unsized literal does not widen its expression (x - 1 is as wide as x).
inline bitwidth_t bitsOf(const Expression& e, const Scope& scope) {
    if (isConstExpr(e)) {
        EvalContext ctx(ASTContext(scope, LookupLocation::max));
        ConstantValue cv = e.eval(ctx);
        if (cv.isInteger() && !cv.integer().hasUnknown()) {
            const SVInt& v = cv.integer();
            if (v.isSigned() && v.isNegative()) return std::min(widthOf(e), v.getMinRepresentedBits());
            return std::max<bitwidth_t>(1, v.getActiveBits());
        }
    }
    return e.getEffectiveWidth().value_or(widthOf(e));
}

// X and Z in a literal: bit 0 set for an X, bit 1 for a Z.
inline int litXZ(const Expression& e0) {
    const Expression& e = strip(e0);
    if (e.kind == ExpressionKind::UnbasedUnsizedIntegerLiteral) {
        logic_t l = e.as<UnbasedUnsizedIntegerLiteral>().getLiteralValue();
        return l.value == logic_t::X_VALUE ? 1 : l.value == logic_t::Z_VALUE ? 2 : 0;
    }
    if (e.kind != ExpressionKind::IntegerLiteral) return 0;
    SVInt v = e.as<IntegerLiteral>().getValue();
    if (!v.hasUnknown()) return 0;
    int r = 0;
    for (bitwidth_t i = 0; i < v.getBitWidth(); i++) {
        logic_t l = v[(int32_t)i];
        if (l.value == logic_t::X_VALUE) r |= 1;
        if (l.value == logic_t::Z_VALUE) r |= 2;
    }
    return r;
}

inline uint64_t locKey(SourceLocation l) { return (uint64_t(l.buffer().getId()) << 32) | uint64_t(l.offset() & 0xffffffffu); }

inline SourceLocation at(const Expression& e, SourceLocation fallback) {
    return e.sourceRange.start() ? e.sourceRange.start() : fallback;
}

// The first name an lvalue assigns: x, x[3], x.f, {x, y} -> x.
inline const Symbol* firstTarget(const Expression& lhs) {
    std::vector<const Symbol*> n;
    namesIn(lhs, n);
    return n.empty() ? nullptr : n[0];
}

// ------------------------------------------------------- the shape of a process

enum class Cls { None, Init, Final, Seq, Comb, Latch, Other };

struct Shape {
    Cls cls = Cls::None;
    const Statement* outerTimed = nullptr;  // the "@(...)" at the top, if any
    const Statement* inner = nullptr;       // the statement under it
    std::vector<const SignalEventControl*> edges, levels;
    bool implicit = false;  // @*
    bool oldStyle = false;  // a plain always
};

inline Shape shapeOf(const ProceduralBlockSymbol& p) {
    Shape s;
    const Statement& body = p.getBody();
    s.inner = &body;
    if (body.kind == StatementKind::Timed) {
        const auto& t = body.as<TimedStatement>();
        TimingControlKind k = t.timing.kind;
        if (k == TimingControlKind::SignalEvent || k == TimingControlKind::EventList || k == TimingControlKind::ImplicitEvent) {
            s.outerTimed = &body;
            s.inner = &t.stmt;
            s.implicit = k == TimingControlKind::ImplicitEvent;
            std::vector<const SignalEventControl*> evs;
            clocksOf(t.timing, evs);
            for (auto* e : evs) (e->edge != EdgeKind::None ? s.edges : s.levels).push_back(e);
        }
    }
    switch (p.procedureKind) {
        case ProceduralBlockKind::Initial: s.cls = Cls::Init; break;
        case ProceduralBlockKind::Final: s.cls = Cls::Final; break;
        case ProceduralBlockKind::AlwaysComb: s.cls = Cls::Comb; break;
        case ProceduralBlockKind::AlwaysLatch: s.cls = Cls::Latch; break;
        case ProceduralBlockKind::AlwaysFF: s.cls = Cls::Seq; break;
        default:
            s.oldStyle = true;
            s.cls = !s.edges.empty() ? Cls::Seq : (s.implicit || !s.levels.empty()) ? Cls::Comb : Cls::Other;
            break;
    }
    return s;
}

// "rst", "!rst", "~rst", "rst == 1'b0": the signal, and whether it is tested low.
// A signal, or one bit or field of one: rst, rst[i], cfg.rst. The index
// may be a genvar, so this is not "the one name in the expression".
inline const Symbol* signalOf(const Expression& e0) {
    const Expression* e = &strip(e0);
    for (;;) {
        if (isName(*e)) return nameOf(*e);
        if (e->kind == ExpressionKind::ElementSelect) e = &strip(e->as<ElementSelectExpression>().value());
        else if (e->kind == ExpressionKind::MemberAccess) e = &strip(e->as<MemberAccessExpression>().value());
        else return nullptr;
    }
}

// An expression whose width is what was written: a name, a select, a
// concatenation, a cast. (The width of a + 1 depends on where it is used.)
inline bool exactWidth(const Expression& e) {
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
        case ExpressionKind::ElementSelect:
        case ExpressionKind::RangeSelect:
        case ExpressionKind::MemberAccess:
        case ExpressionKind::Concatenation:
        case ExpressionKind::Replication:
        case ExpressionKind::Conversion: return true;
        default: return false;
    }
}

inline const Symbol* resetCond(const Expression& e0, bool& low) {
    const Expression& e = strip(e0);
    low = false;
    if (const Symbol* s = signalOf(e)) return s;
    if (e.kind == ExpressionKind::UnaryOp) {
        const auto& u = e.as<UnaryExpression>();
        if (u.op != UnaryOperator::LogicalNot && u.op != UnaryOperator::BitwiseNot) return nullptr;
        low = true;
        return signalOf(u.operand());
    }
    if (e.kind == ExpressionKind::BinaryOp) {
        const auto& b = e.as<BinaryExpression>();
        if (b.op != BinaryOperator::Equality && b.op != BinaryOperator::Inequality) return nullptr;
        const Expression* n = &strip(b.left());
        const Expression* k = &strip(b.right());
        if (!signalOf(*n)) std::swap(n, k);
        if (!signalOf(*n) || k->kind != ExpressionKind::IntegerLiteral) return nullptr;
        SVInt v = k->as<IntegerLiteral>().getValue();
        if (v.hasUnknown()) return nullptr;
        bool zero = exactlyEqual(v, SVInt(v.getBitWidth(), 0, v.isSigned()));
        low = (b.op == BinaryOperator::Equality) == zero;
        return signalOf(*n);
    }
    return nullptr;
}

// One statement inside begin/end is that statement.
inline const Statement& unwrapStmt(const Statement& s0) {
    const Statement* s = &s0;
    for (;;) {
        if (s->kind == StatementKind::Block) s = &s->as<BlockStatement>().body;
        else if (s->kind == StatementKind::List && s->as<StatementList>().list.size() == 1) s = s->as<StatementList>().list[0];
        else return *s;
    }
}

struct Flop {
    const Symbol* clk = nullptr;
    EdgeKind clkEdge = EdgeKind::None;
    const Symbol* arst = nullptr;  // asynchronous reset
    bool arstLow = false;
    EdgeKind arstEdge = EdgeKind::None;
    bool tmplOk = true;  // if (reset) ... else ...
    const Statement* rstBranch = nullptr;
    const Statement* opBranch = nullptr;
    const Symbol* srst = nullptr;  // the signal the first if tests, when there is no asynchronous reset
    bool srstLow = false;
};

inline Flop flopOf(const Shape& s) {
    Flop f;
    const ConditionalStatement* top = nullptr;
    const Statement& in = unwrapStmt(*s.inner);
    if (in.kind == StatementKind::Conditional && in.as<ConditionalStatement>().conditions.size() == 1 &&
        !in.as<ConditionalStatement>().conditions[0].pattern)
        top = &in.as<ConditionalStatement>();
    bool low = false;
    const Symbol* cond = top ? resetCond(*top->conditions[0].expr, low) : nullptr;
    if (s.edges.size() == 1) {
        f.clk = signalOf(s.edges[0]->expr);
        f.clkEdge = s.edges[0]->edge;
        if (cond && cond != f.clk) {
            f.srst = cond;
            f.srstLow = low;
            f.rstBranch = &top->ifTrue;
            f.opBranch = top->ifFalse;
        }
        return f;
    }
    // Two or more edges: the one the first if tests is the reset; with a set
    // as well, the else branch tests it; the clock is what is left.
    std::set<const Symbol*> asyncs;
    for (auto* e : s.edges)
        if (cond && signalOf(e->expr) == cond) {
            f.arst = cond;
            f.arstLow = low;
            f.arstEdge = e->edge;
            f.rstBranch = &top->ifTrue;
            f.opBranch = top->ifFalse;
            asyncs.insert(cond);
        }
    if (f.arst && top->ifFalse) {
        const Statement* el = &unwrapStmt(*top->ifFalse);
        while (el->kind == StatementKind::Conditional && el->as<ConditionalStatement>().conditions.size() == 1) {
            bool l2;
            const auto& c2 = el->as<ConditionalStatement>();
            const Symbol* s2 = resetCond(*c2.conditions[0].expr, l2);
            bool isEdge = false;
            for (auto* e : s.edges) isEdge |= s2 && signalOf(e->expr) == s2;
            if (!isEdge || !c2.ifFalse) break;
            asyncs.insert(s2);
            el = &unwrapStmt(*c2.ifFalse);
        }
    }
    for (auto* e : s.edges) {
        const Symbol* n = signalOf(e->expr);
        if (!asyncs.count(n) && !f.clk) {
            f.clk = n;
            f.clkEdge = e->edge;
        }
    }
    if (!f.arst) {
        f.tmplOk = false;
        f.clk = signalOf(s.edges[0]->expr);
        f.clkEdge = s.edges[0]->edge;
    }
    return f;
}

inline const std::set<std::string_view>& vhdlKeywords() {
    static const std::set<std::string_view> k = {
        "abs", "access", "after", "all", "architecture", "array", "attribute", "block", "body", "buffer", "bus",
        "component", "configuration", "constant", "disconnect", "downto", "elsif", "entity", "exit", "file",
        "generic", "group", "guarded", "impure", "in", "inertial", "is", "label", "linkage", "literal", "loop",
        "map", "mod", "new", "next", "null", "of", "on", "open", "others", "out", "port", "postponed",
        "procedure", "process", "range", "record", "register", "reject", "rem", "report", "rol", "ror",
        "select", "severity", "shared", "signal", "sla", "sll", "sra", "srl", "subtype", "then", "to",
        "transport", "type", "unaffected", "units", "until", "variable", "when"};
    return k;
}

inline bool badSystemCall(std::string_view n) {
    static const std::set<std::string_view> bad = {
        "$random", "$urandom", "$urandom_range", "$time", "$stime", "$realtime", "$fopen", "$fclose", "$fgets",
        "$fgetc", "$fscanf", "$sscanf", "$fread", "$feof", "$readmemh", "$readmemb", "$writememh", "$writememb",
        "$finish", "$stop", "$value$plusargs", "$test$plusargs", "$sformatf", "$sformat", "$swrite", "$dumpvars",
        "$dumpfile", "$system", "$itor", "$rtoi", "$realtobits", "$bitstoreal", "$dist_uniform", "$dist_normal",
        "$dist_exponential", "$dist_poisson", "$fflush", "$fseek", "$ftell", "$rewind", "$ungetc", "$exit"};
    return bad.count(n) != 0;
}

inline bool writesArgs(std::string_view n) {
    static const std::set<std::string_view> w = {"$readmemh", "$readmemb", "$fscanf", "$sscanf", "$fgets", "$fread",
                                                 "$value$plusargs", "$cast", "$sformat", "$swrite", "$swriteh",
                                                 "$swriteb", "$swriteo", "$ferror"};
    return w.count(n) != 0;
}

// --------------------------------------------------------------- one module body

enum : uint8_t { UseR = 1, UseW = 2, UseBlk = 4, UseNba = 8 };

struct Cand {
    uint64_t key;
    std::string name;
    SourceLocation loc;
    uint8_t port;  // 0 not a port, 1 input, 2 output, 3 inout
};

// What one body's check hands back for the rules that need every body.
struct BodyOut {
    std::vector<Cand> cands;
    std::unordered_map<uint64_t, uint8_t> uses;
};

struct BodyRules : ASTVisitor<BodyRules, VisitFlags::AllGood> {
    const InstanceBodySymbol& body;
    Report& rep;
    const Params& P;
    BodyOut& outp;
    bool tb, iface;

    struct Use {
        uint8_t bits = 0;
        const void* reader = nullptr;
        bool multi = false;
    };
    std::unordered_map<const Symbol*, Use> use;
    std::unordered_map<const Symbol*, uint8_t> portDir;
    std::vector<std::pair<const Symbol*, const ProceduralBlockSymbol*>> seqBlocking;
    std::vector<const Symbol*> decls;
    std::map<std::pair<const Scope*, std::string>, std::string> lowerNames;

    // Where the walk is.
    const ProceduralBlockSymbol* proc = nullptr;
    Shape shape;
    bool inCont = false, inPort = false;
    int inAssert = 0, loopDepth = 0;
    const SubroutineSymbol* func = nullptr;

    BodyRules(const InstanceBodySymbol& b, Report& r, const Params& p, BodyOut& o)
        : body(b), rep(r), P(p), outp(o), tb(isTb(b, p)),
          iface(b.getDefinition().definitionKind == DefinitionKind::Interface) {}

    bool on(std::string_view rule) const { return rep.rules.enabled(rule); }
    bool design() const { return !tb && inAssert == 0; }
    bool inAlways() const { return proc && (shape.cls == Cls::Seq || shape.cls == Cls::Comb || shape.cls == Cls::Latch || shape.cls == Cls::Other); }
    // Code that synthesis turns into gates.
    bool hw() const { return design() && (inAlways() || inCont || (func && !proc)); }

    void run() {
        for (const Symbol* p : body.getPortList()) {
            if (!p || p->kind != SymbolKind::Port) continue;
            const auto& ps = p->as<PortSymbol>();
            uint8_t d = ps.direction == ArgumentDirection::In ? 1 : ps.direction == ArgumentDirection::Out ? 2 : 3;
            if (ps.internalSymbol) portDir[ps.internalSymbol] = d;
            if (d == 1 && on("NAME008") && !P.portIn.ok(ps.name))
                rep.add("NAME008", ps.location, "input port '" + shown(ps) + "' does not match " + P.portIn.src);
            if (d == 2 && on("NAME008") && !P.portOut.ok(ps.name))
                rep.add("NAME008", ps.location, "output port '" + shown(ps) + "' does not match " + P.portOut.src);
        }
        visitDefault(body);
        finish();
    }

    // ------------------------------------------------------------ reads, writes

    const void* readerId() const { return proc ? (const void*)proc : (const void*)this; }

    void markRead(const Symbol& s) {
        Use& u = use[&s];
        u.bits |= UseR;
        const void* r = readerId();
        if (!u.reader) u.reader = r;
        else if (u.reader != r) u.multi = true;
    }

    void markWrite(const Symbol& s, bool nba) {
        Use& u = use[&s];
        u.bits |= UseW;
        if (proc && inAlways() && moduleLevel(s)) {
            u.bits |= nba ? UseNba : UseBlk;
            if (!nba && shape.cls == Cls::Seq && shape.oldStyle) seqBlocking.push_back({&s, proc});
        }
    }

    void markLhs(const Expression& e, bool nba) {
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: markWrite(*nameOf(e), nba); break;
            case ExpressionKind::ElementSelect:
                markLhs(e.as<ElementSelectExpression>().value(), nba);
                e.as<ElementSelectExpression>().selector().visit(*this);
                break;
            case ExpressionKind::RangeSelect:
                markLhs(e.as<RangeSelectExpression>().value(), nba);
                e.as<RangeSelectExpression>().left().visit(*this);
                e.as<RangeSelectExpression>().right().visit(*this);
                break;
            case ExpressionKind::MemberAccess: markLhs(e.as<MemberAccessExpression>().value(), nba); break;
            case ExpressionKind::Concatenation:
                for (const Expression* o : e.as<ConcatenationExpression>().operands()) markLhs(*o, nba);
                break;
            case ExpressionKind::Conversion: markLhs(e.as<ConversionExpression>().operand(), nba); break;
            case ExpressionKind::Streaming:
                for (auto& st : e.as<StreamingConcatenationExpression>().streams()) markLhs(*st.operand, nba);
                break;
            default: {
                std::vector<const Symbol*> n;
                namesIn(e, n);
                for (const Symbol* s : n) markWrite(*s, nba);
                break;
            }
        }
    }

    void markBoth(const Expression& e) {
        std::vector<const Symbol*> n;
        namesIn(e, n);
        for (const Symbol* s : n) {
            markRead(*s);
            use[s].bits |= UseW;
        }
    }

    void handle(const NamedValueExpression& n) { markRead(n.symbol); }

    void handle(const HierarchicalValueExpression& n) {
        markRead(n.symbol);
        if (!design() || iface || !on("SYN017")) return;
        if (n.ref.isViaIfacePort()) return;
        const InstanceBodySymbol* b = bodyOf(n.symbol);
        if (b && b->getDefinition().definitionKind == DefinitionKind::Interface) return;  // u_if.sig
        if (n.symbol.kind == SymbolKind::Parameter) return;
        rep.add("SYN017", at(n, body.location),
                "hierarchical reference to '" + shown(n.symbol) + "' in design code; pass it through a port");
    }

    // ------------------------------------------------------------- declarations

    void clash(const Symbol& s) {
        if (s.name.empty() || !on("NAME009")) return;
        std::string lo = lintcfg::lower(s.name);
        auto [it, fresh] = lowerNames.try_emplace({s.getParentScope(), lo}, shown(s));
        if (!fresh && it->second != s.name)
            rep.add("NAME009", s.location,
                    "'" + shown(s) + "' and '" + it->second + "' differ only in case");
    }

    void declCheck(const ValueSymbol& s, bool isNet) {
        if (!moduleLevel(s) || s.name.empty()) return;
        std::string name = shown(s);
        const Type& ty = s.getType().getCanonicalType();
        bool isPort = portDir.count(&s) != 0;
        clash(s);
        if (!isPort && on("NAME002") && !P.signal.ok(name))
            rep.add("NAME002", s.location, "signal '" + name + "' does not match " + P.signal.src);
        if (on("NAME010") && name.size() > P.maxLen)
            rep.add("NAME010", s.location,
                    "'" + name + "' is " + std::to_string(name.size()) + " characters long; the limit is " +
                        std::to_string(P.maxLen));
        if (on("NAME012") && vhdlKeywords().count(lintcfg::lower(name)))
            rep.add("NAME012", s.location, "'" + name + "' is a VHDL keyword");
        if (isNet && s.as<NetSymbol>().isImplicit)
            rep.add("CON004", s.location,
                    "'" + name + "' is not declared, so it is an implicit 1-bit net; declare it (or fix the typo)");
        if (tb || iface) return;
        if (ty.isFloating())
            rep.add("SYN006", s.location, "'" + name + "' is a real number, which is not synthesizable");
        else if (ty.isString() || ty.isDynamicallySizedArray() || ty.isClass() || ty.isEvent() || ty.isCHandle() ||
                 ty.isVirtualInterface())
            rep.add("SYN013", s.location, "'" + name + "' has a dynamic type (" + ty.toString() + "), which is not synthesizable");
        else
            decls.push_back(&s);
    }

    void handle(const VariableSymbol& v) {
        declCheck(v, false);
        if (v.getInitializer()) use[&v].bits |= UseW;
        visitDefault(v);
    }
    void handle(const NetSymbol& n) {
        declCheck(n, true);
        if (n.getInitializer()) use[&n].bits |= UseW;
        visitDefault(n);
    }

    void handle(const ParameterSymbol& p) {
        if (on("NAME003") && !p.isFromGenvar() && !P.param.ok(p.name))
            rep.add("NAME003", p.location, "parameter '" + shown(p) + "' does not match " + P.param.src);
        visitDefault(p);
    }

    void handle(const TypeAliasType& t) {
        if (on("NAME013") && !P.typedefName.ok(t.name))
            rep.add("NAME013", t.location, "typedef '" + shown(t) + "' does not match " + P.typedefName.src);
    }

    void handle(const DefParamSymbol& d) {
        if (!tb) rep.add("SYN015", d.location, "defparam is deprecated and not supported by many tools; use #(.P(value))");
    }

    void handle(const ClassType&) {}
    // An instance of a module the front end was not given: anything
    // connected to it may be read and may be driven.
    void handle(const UninstantiatedDefSymbol& u) {
        for (const AssertionExpr* c : u.getPortConnections())
            if (c && c->kind == AssertionExprKind::Simple) markBoth(c->as<SimpleAssertionExpr>().expr);
    }
    void handle(const CheckerInstanceSymbol&) {}

    void handle(const GenerateBlockSymbol& g) {
        if (g.isUninstantiated) return;
        if (g.isUnnamed && !g.getArrayIndex() && on("NAME011")) unnamed(g, g.location);
        visitDefault(g);
    }
    void handle(const GenerateBlockArraySymbol& g) {
        if (on("NAME011") && !g.entries.empty() && g.name.empty()) unnamed(*g.entries[0], g.location);
        visitDefault(g);
    }
    // Only where a hierarchical name goes through the block.
    void unnamed(const GenerateBlockSymbol& g, SourceLocation loc) {
        for (const Symbol& m : g.members())
            if (m.kind == SymbolKind::Variable || m.kind == SymbolKind::Net || m.kind == SymbolKind::Instance ||
                m.kind == SymbolKind::InstanceArray) {
                rep.add("NAME011", loc,
                        "this generate block has no label, so '" + shown(m) +
                            "' inside it is reached through a generated name (genblk1...); add ': label'");
                return;
            }
    }

    void handle(const PrimitiveInstanceSymbol& p) {
        for (const Expression* e : p.getPortConnections())
            if (e) markBoth(*e);
    }

    void handle(const InstanceSymbol& inst) {
        std::string iname = shown(inst);
        clash(inst);
        if (on("NAME007") && inst.isModule() && !iname.empty() && !P.instance.ok(iname))
            rep.add("NAME007", inst.location, "instance '" + iname + "' does not match " + P.instance.src);
        std::string mod = shown(inst.getDefinition());

        if (on("CON003") && inst.getSyntax() && inst.getSyntax()->kind == syntax::SyntaxKind::HierarchicalInstance) {
            int ordered = 0;
            for (auto* c : inst.getSyntax()->as<syntax::HierarchicalInstanceSyntax>().connections)
                if (c->kind == syntax::SyntaxKind::OrderedPortConnection) ordered++;
            if (ordered >= 2)
                rep.add("CON003", inst.location,
                        "instance '" + iname + "' of '" + mod + "' connects " + std::to_string(ordered) +
                            " ports by position; connect them by name");
        }

        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn) continue;
            const Expression* x = conn->getExpression();
            if (conn->port.kind != SymbolKind::Port) {
                if (x && conn->port.kind == SymbolKind::MultiPort) markBoth(*x);
                continue;
            }
            const auto& ps = conn->port.as<PortSymbol>();
            std::string pn = shown(ps);
            if (!x) {
                if (ps.direction == ArgumentDirection::In && !ps.getInitializer())
                    rep.add("CON001", inst.location,
                            "input '" + pn + "' of instance '" + iname + "' (" + mod + ") is not connected");
                else if (ps.direction == ArgumentDirection::Out)
                    rep.add("CON002", inst.location,
                            "output '" + pn + "' of instance '" + iname + "' (" + mod + ") is not connected");
                continue;
            }
            // Width of what is connected against the port's.
            const Expression* actual = x;
            if (x->kind == ExpressionKind::Assignment) actual = &x->as<AssignmentExpression>().left();
            const Expression& a = strip(*actual);
            const Type& pt = ps.getType();
            if (on("W003") && pt.isIntegral() && a.type->isIntegral() && a.kind != ExpressionKind::EmptyArgument) {
                bitwidth_t pw = pt.getBitWidth(), aw = widthOf(a);
                bool report = aw != pw;
                if (report && isConstExpr(a)) {
                    aw = bitsOf(a, body);  // a constant that fits is fine
                    report = aw > pw;
                } else if (report && !exactWidth(a)) {
                    aw = bitsOf(a, body);  // a + 1 is as wide as a
                    report = aw > pw;
                }
                if (report)
                    rep.add("W003", at(*x, inst.location),
                            "port '" + pn + "' of '" + mod + "' is " + std::to_string(pw) + (pw == 1 ? " bit" : " bits") +
                                " wide but is connected to " + std::to_string(aw) + (aw == 1 ? " bit" : " bits"));
            }
            inPort = true;
            if (ps.direction == ArgumentDirection::In) {
                x->visit(*this);
            } else if (x->kind == ExpressionKind::Assignment) {
                x->visit(*this);
                if (ps.direction != ArgumentDirection::Out) markBoth(x->as<AssignmentExpression>().left());
            } else {
                markBoth(*x);
                x->visit(*this);
            }
            inPort = false;
        }
        // The body is checked on its own, once.
    }

    // ---------------------------------------------------------------- functions

    void handle(const SubroutineSymbol& f) {
        if (f.flags.has(MethodFlags::DPIImport) || f.flags.has(MethodFlags::InterfaceExtern)) return;
        if (!tb && !iface && moduleLevel(f) && f.subroutineKind == SubroutineKind::Function && on("FUNC001")) {
            std::vector<std::string> outside;
            f.getBody().visit(makeVisitor([&](auto&, const NamedValueExpression& n) {
                if (!isSignal(n.symbol) || !moduleLevel(n.symbol)) return;
                std::string nm = shown(n.symbol);
                if (std::find(outside.begin(), outside.end(), nm) == outside.end()) outside.push_back(nm);
            }));
            if (!outside.empty()) {
                std::string list;
                for (size_t i = 0; i < outside.size() && i < 3; i++) list += (i ? ", '" : "'") + outside[i] + "'";
                if (outside.size() > 3) list += ", ...";
                rep.add("FUNC001", f.location,
                        "function '" + shown(f) + "' uses " + list +
                            " from the module around it; pass it as an argument");
            }
        }
        const SubroutineSymbol* saved = func;
        func = &f;
        visitDefault(f);
        func = saved;
    }

    // ------------------------------------------------------- continuous assigns

    void handle(const ContinuousAssignSymbol& ca) {
        const Expression& e = ca.getAssignment();
        if (design() && !iface) {
            if (ca.getDelay()) rep.add("SYN002", ca.location, "a delay on a continuous assignment is ignored by synthesis");
            if (e.kind == ExpressionKind::Assignment && on("SYN016")) {
                const auto& a = e.as<AssignmentExpression>();
                bool z = false;
                a.right().visit(makeVisitor([&](auto&, const IntegerLiteral& l) { z |= (litXZ(l) & 2) != 0; },
                                            [&](auto&, const UnbasedUnsizedIntegerLiteral& l) { z |= (litXZ(l) & 2) != 0; }));
                const Symbol* t = firstTarget(a.left());
                auto pd = t ? portDir.find(t) : portDir.end();
                if (z && t && (pd == portDir.end() || pd->second != 3))
                    rep.add("SYN016", ca.location,
                            "'" + shown(*t) + "' is driven to Z, but it is not an inout port: an internal "
                            "tri-state; use a mux");
            }
        }
        inCont = true;
        visitDefault(ca);
        inCont = false;
    }

    // ------------------------------------------------------------------ process

    void handle(const ProceduralBlockSymbol& p) {
        proc = &p;
        shape = shapeOf(p);
        const Shape& s = shape;
        SourceLocation loc = p.location;

        if (s.oldStyle && (s.cls == Cls::Seq || s.cls == Cls::Comb) && on("STY003"))
            rep.add("STY003", loc, s.cls == Cls::Seq ? "use always_ff for this flop" : "use always_comb for this logic");
        if (!tb && !s.edges.empty() && !s.levels.empty())
            rep.add("SYN007", loc,
                    "the sensitivity list mixes an edge with the level of '" + exprName(s.levels[0]->expr) +
                        "': neither a flop nor combinational logic");
        if (s.cls == Cls::Latch) rep.add("LAT002", loc, "a latch (always_latch)");

        if (s.cls == Cls::Init && !tb && !iface && on("SYN001")) {
            std::vector<std::string> names;
            s.inner->visit(makeVisitor([&](auto& v, const AssignmentExpression& a) {
                const Symbol* t = firstTarget(a.left());
                // Not the loop index of "for (i = 0; ...) mem[i] = 0".
                bool index = t && isSignal(*t) && t->as<ValueSymbol>().getType().isPredefinedInteger();
                if (t && !index && moduleLevel(*t) && std::find(names.begin(), names.end(), t->name) == names.end())
                    names.push_back(shown(*t));
                v.visitDefault(a);
            }));
            if (!names.empty())
                rep.add("SYN001", loc,
                        "this initial block sets '" + names[0] + "'" + (names.size() > 1 ? " and " + std::to_string(names.size() - 1) + " more" : "") +
                            "; ASIC synthesis ignores it, so the hardware starts from an unknown value: use a reset");
        }

        if (s.cls == Cls::Comb && s.oldStyle) oldComb(p, s);
        if (s.cls == Cls::Seq) flop(p, s);

        visitDefault(p);
        proc = nullptr;
        shape = Shape{};
    }

    static std::string exprName(const Expression& e) {
        const Symbol* s = oneName(e);
        return s ? shown(*s) : std::string("?");
    }

    // always @* and always @(a or b): latches, the sensitivity list, <=.
    void oldComb(const ProceduralBlockSymbol& p, const Shape& s) {
        Block tmp;
        BodyWalk w(tmp, rep);
        std::set<const Symbol*> def = w.walk(*s.inner);
        std::set<const Symbol*> targets = tmp.blocking;
        targets.insert(tmp.nba.begin(), tmp.nba.end());
        for (const Symbol* t : targets) {
            if (tmp.locals.count(t) || def.count(t) || !tmp.whole.count(t) || !moduleLevel(*t)) continue;
            rep.add("LAT001", p.location,
                    "'" + shown(*t) +
                        "' is not assigned on every path of this combinational block, so it holds its old value "
                        "(a latch); add an else or a default");
        }
        for (const Symbol* t : tmp.nba)
            rep.add("SEQ003", p.location,
                    "non-blocking assignment to '" + shown(*t) + "' in a combinational always; use =");
        if (!s.implicit && on("COMB001")) {
            std::set<const Symbol*> listed;
            for (auto* e : s.levels) {
                std::vector<const Symbol*> n;
                namesIn(e->expr, n);
                listed.insert(n.begin(), n.end());
            }
            std::vector<std::string> missing;
            for (const Symbol* r : tmp.reads) {
                if (!isSignal(*r) || !moduleLevel(*r) || listed.count(r) || targets.count(r)) continue;
                missing.push_back(shown(*r));
            }
            std::sort(missing.begin(), missing.end());
            if (!missing.empty()) {
                std::string list;
                for (size_t i = 0; i < missing.size() && i < 4; i++) list += (i ? ", '" : "'") + missing[i] + "'";
                if (missing.size() > 4) list += ", ...";
                rep.add("COMB001", p.location,
                        list + (missing.size() == 1 ? " is" : " are") +
                            " read here but missing from the sensitivity list, so simulation does not follow "
                            "the hardware; use always_comb or @*");
            }
        }
    }

    static void targetsOf(const Statement& st, std::set<const Symbol*>& out) {
        st.visit(makeVisitor([&](auto& v, const AssignmentExpression& a) {
            if (const Symbol* t = firstTarget(a.left())) out.insert(t);
            v.visitDefault(a);
        }));
    }

    void flop(const ProceduralBlockSymbol& p, const Shape& s) {
        Flop f = flopOf(s);
        SourceLocation loc = p.location;
        if (f.clk) {
            if (f.clkEdge == EdgeKind::NegEdge)
                rep.add("CLK002", loc, "this flop is clocked on the falling edge of '" + shown(*f.clk) + "'");
            if (on("NAME004") && !P.clock.ok(f.clk->name))
                rep.add("NAME004", f.clk->location, "clock '" + shown(*f.clk) + "' does not match " + P.clock.src);
        }
        if (s.edges.size() >= 2 && !f.tmplOk) {
            rep.add("RST007", loc,
                    "this block has an asynchronous event besides its clock but does not start with "
                    "if (reset) ... else ...; synthesis may not map it to a flop with asynchronous reset");
            return;
        }
        if (!f.arst) {
            bool named = f.srst && P.reset.ok(f.srst->name);
            if (!named) rep.add("RST004", loc, "this flop has no reset");
            if (named && on("NAME006") && f.srstLow && !P.activeLow.ok(f.srst->name))
                rep.add("NAME006", f.srst->location,
                        "reset '" + shown(*f.srst) + "' is active low but does not match " + P.activeLow.src);
            return;
        }
        std::string rn = shown(*f.arst);
        if (on("NAME005") && !P.reset.ok(rn))
            rep.add("NAME005", f.arst->location, "reset '" + rn + "' does not match " + P.reset.src);
        if (on("NAME006") && f.arstLow && !P.activeLow.ok(rn))
            rep.add("NAME006", f.arst->location, "reset '" + rn + "' is active low but does not match " + P.activeLow.src);
        if ((f.arstEdge == EdgeKind::PosEdge && f.arstLow) || (f.arstEdge == EdgeKind::NegEdge && !f.arstLow))
            rep.add("RST005", loc,
                    std::string("the block is sensitive to the ") + (f.arstEdge == EdgeKind::PosEdge ? "rising" : "falling") +
                        " edge of '" + rn + "' but resets while it is " + (f.arstLow ? "low" : "high") +
                        ": the reset is released, not applied, on that edge");
        if (f.rstBranch && on("RST006"))
            f.rstBranch->visit(makeVisitor([&](auto& v, const AssignmentExpression& a) {
                if (!isConstExpr(a.right())) {
                    const Symbol* t = firstTarget(a.left());
                    rep.add("RST006", at(a, loc),
                            "'" + (t ? shown(*t) : std::string("?")) +
                                "' is reset asynchronously to a value that is not constant");
                }
                v.visitDefault(a);
            }));
        if (f.rstBranch && f.opBranch && on("RST003")) {
            std::set<const Symbol*> inRst, inOp;
            targetsOf(*f.rstBranch, inRst);
            targetsOf(*f.opBranch, inOp);
            if (!inRst.empty())
                for (const Symbol* t : inOp) {
                    if (inRst.count(t) || !moduleLevel(*t)) continue;
                    if (auto* v = t->as_if<ValueSymbol>(); v && v->getType().isUnpackedArray()) continue;  // a memory
                    rep.add("RST003", loc,
                            "'" + shown(*t) + "' is assigned in this block but not in its reset branch, so '" +
                                rn + "' becomes its enable; reset it, or move it to a block without reset");
                }
        }
    }

    // --------------------------------------------------------------- statements

    void handle(const TimedStatement& t) {
        TimingControlKind k = t.timing.kind;
        bool delay = k == TimingControlKind::Delay || k == TimingControlKind::Delay3 ||
                     k == TimingControlKind::OneStepDelay || k == TimingControlKind::CycleDelay;
        if (design() && !iface && inAlways()) {
            SourceLocation loc = t.sourceRange.start() ? t.sourceRange.start() : proc->location;
            if (loopDepth > 0)
                rep.add("SYN008", loc, "a timing control inside a loop: synthesis cannot unroll it");
            else if (delay)
                rep.add("SYN002", loc, "a delay in an always block is ignored by synthesis");
            else if (&t != shape.outerTimed)
                rep.add("SYN012", loc, "an event control inside the body of an always block (an implicit state machine) is not synthesizable by most tools");
        }
        visitDefault(t);
    }

    template<typename T>
    void loop(const T& l) {
        loopDepth++;
        visitDefault(l);
        loopDepth--;
    }
    void handle(const ForLoopStatement& l) { loop(l); }
    void handle(const WhileLoopStatement& l) { loop(l); }
    void handle(const DoWhileLoopStatement& l) { loop(l); }
    void handle(const RepeatLoopStatement& l) { loop(l); }
    void handle(const ForeachLoopStatement& l) { loop(l); }
    void handle(const ForeverLoopStatement& l) {
        if (design() && !iface && inAlways())
            rep.add("SYN008", l.sourceRange.start() ? l.sourceRange.start() : proc->location,
                    "a forever loop in an always block is not synthesizable");
        loop(l);
    }

    void notSynth(const Statement& s, const char* what, const char* rule) {
        if (!design() || iface || !proc) return;
        rep.add(rule, s.sourceRange.start() ? s.sourceRange.start() : proc->location, std::string(what) + " is not synthesizable");
    }
    void handle(const BlockStatement& b) {
        if (b.blockKind != StatementBlockKind::Sequential && inAlways() && design() && !iface)
            rep.add("SYN005", b.syntax ? b.syntax->getFirstToken().location() : proc->location, "fork is not synthesizable");
        visitDefault(b);
    }
    void handle(const WaitStatement& s) {
        if (inAlways()) notSynth(s, "wait", "SYN005");
        visitDefault(s);
    }
    void handle(const WaitForkStatement& s) {
        if (inAlways()) notSynth(s, "wait fork", "SYN005");
    }
    void handle(const WaitOrderStatement& s) {
        if (inAlways()) notSynth(s, "wait_order", "SYN005");
        visitDefault(s);
    }
    void handle(const EventTriggerStatement& s) {
        if (inAlways()) notSynth(s, "an event trigger (->)", "SYN005");
        visitDefault(s);
    }
    void handle(const ProceduralAssignStatement& s) {
        notSynth(s, s.isForce ? "force" : "a procedural assign", "SYN004");
        visitDefault(s);
    }
    void handle(const ProceduralDeassignStatement& s) {
        notSynth(s, s.isRelease ? "release" : "deassign", "SYN004");
        markBoth(s.lvalue);
    }
    void handle(const ImmediateAssertionStatement& s) {
        inAssert++;
        visitDefault(s);
        inAssert--;
    }
    void handle(const ConcurrentAssertionStatement& s) {
        inAssert++;
        visitDefault(s);
        inAssert--;
    }

    void multiBitCond(const Expression& c) {
        const Expression& e = strip(c);
        if (integral(e) && widthOf(e) > 1)
            rep.add("W006", at(c, body.location),
                    "a value of " + std::to_string(widthOf(e)) + " bits is used as a condition; write != 0 if any set bit is meant");
    }

    void handle(const ConditionalStatement& c) {
        if (on("W006"))
            for (auto& cond : c.conditions) multiBitCond(*cond.expr);
        visitDefault(c);
    }

    void handle(const CaseStatement& c) {
        SourceLocation loc = c.sourceRange.start() ? c.sourceRange.start() : body.location;
        if (inAssert == 0) caseRules(c, loc);
        visitDefault(c);
    }

    void caseRules(const CaseStatement& c, SourceLocation loc) {
        if (c.condition == CaseStatementCondition::WildcardXOrZ)
            rep.add("CASE002", loc, "casex also treats X and Z in the selector as wildcards; use casez or case ... inside");
        bool wild = c.condition == CaseStatementCondition::WildcardXOrZ || c.condition == CaseStatementCondition::WildcardJustZ;
        bool inside = c.condition == CaseStatementCondition::Inside;
        std::vector<SVInt> vals;
        bool allConst = !inside, anyUnknown = false;
        size_t n = 0;
        EvalContext ctx(ASTContext(body, LookupLocation::max));
        for (auto& g : c.items)
            for (const Expression* e : g.expressions) {
                n++;
                if (c.condition == CaseStatementCondition::Normal && litXZ(*e) && on("CASE003"))
                    rep.add("CASE003", at(*e, loc), "this item has X or Z bits, which a plain case never matches in hardware; use casez with ?");
                if (!allConst || n > 4096) continue;
                if (!isConstExpr(*e)) {
                    allConst = false;
                    continue;
                }
                ConstantValue cv = e->eval(ctx);
                if (!cv.isInteger()) {
                    allConst = false;
                    continue;
                }
                const SVInt& v = cv.integer();
                anyUnknown |= v.hasUnknown();
                bool dup = false;
                for (const SVInt& o : vals)
                    if (o.getBitWidth() == v.getBitWidth() && exactlyEqual(o, v)) dup = true;
                if (dup)
                    rep.add("CASE004", at(*e, loc), "the value " + v.toString(LiteralBase::Decimal, false) + " is already an item of this case; this one never runs");
                else
                    vals.push_back(v);
            }
        // No default, in combinational logic: is every value there?
        if (c.defaultCase || c.check != UniquePriorityCheck::None || !proc || shape.cls != Cls::Comb || !on("CASE001")) return;
        for (const AttributeSymbol* at : body.getCompilation().getAttributes(c))
            if (at->name == "full_case") return;  // the designer's word that the rest cannot happen
        const Type& st = c.expr.type->getCanonicalType();
        bool full = false;
        if (allConst && !anyUnknown && !wild && n <= 4096) {
            if (st.kind == SymbolKind::EnumType) {
                size_t members = 0, hit = 0;
                for (auto& ev : st.as<EnumType>().values()) {
                    members++;
                    const SVInt& mv = ev.getValue().integer();
                    for (const SVInt& o : vals)
                        if (o.getBitWidth() == mv.getBitWidth() && exactlyEqual(o, mv)) {
                            hit++;
                            break;
                        }
                }
                full = hit == members;
            }
            bitwidth_t w = st.getBitWidth();
            if (!full && st.isIntegral() && w <= 12) full = vals.size() == (size_t(1) << w);
        }
        if (!full)
            rep.add("CASE001", loc,
                    "this case has no default and does not list every value; for the rest nothing here runs, so "
                    "the outputs keep what they had (add a default, or unique / priority)");
    }

    // -------------------------------------------------------------- expressions

    void handle(const AssignmentExpression& a) {
        SourceLocation loc = at(a, proc ? proc->location : body.location);
        const Expression& rhs = strip(a.right());
        const Expression& lhs = a.left();
        bool arg = rhs.kind == ExpressionKind::EmptyArgument;
        if (!arg && !a.isCompound()) {
            if (isName(lhs) && isName(rhs) && nameOf(lhs) == nameOf(rhs))
                rep.add("EXP001", loc, "'" + shown(*nameOf(lhs)) + "' is assigned to itself");
            if (!inPort && lhs.type->isIntegral() && rhs.type->isIntegral()) {
                bitwidth_t lw = lhs.type->getBitWidth();
                bitwidth_t rw = bitsOf(rhs, body);
                const Symbol* t = firstTarget(lhs);
                std::string tn = t ? shown(*t) : std::string("the target");
                if (rw > lw && !litXZ(rhs))  // 'bx is 32 bits of X by the book; nobody means that
                    rep.add("W001", loc,
                            std::to_string(rw) + " bits are assigned to '" + tn + "', which is " + std::to_string(lw) +
                                (lw == 1 ? " bit" : " bits") + " wide: the upper " + std::to_string(rw - lw) +
                                (rw - lw == 1 ? " bit is" : " bits are") + " dropped");
                else if (on("W002") && widthOf(rhs) < lw && !isConstExpr(rhs))
                    rep.add("W002", loc,
                            std::to_string(widthOf(rhs)) + (widthOf(rhs) == 1 ? " bit is" : " bits are") + " extended to the " +
                                std::to_string(lw) + " bits of '" + tn + "'");
            }
            if (hw() && !iface && on("SYN011") && (litXZ(rhs) & 1))
                rep.add("SYN011", loc, "an X is assigned");
        }
        if (a.timingControl && design() && !iface && inAlways() && a.timingControl->kind == TimingControlKind::Delay)
            rep.add("SYN002", loc, "an intra-assignment delay is ignored by synthesis");
        markLhs(lhs, a.isNonBlocking());
        if (a.isCompound()) {
            std::vector<const Symbol*> n;
            namesIn(lhs, n);
            for (const Symbol* s : n) markRead(*s);
        }
        a.right().visit(*this);
        if (a.timingControl) a.timingControl->visit(*this);
    }

    static bool isCompare(BinaryOperator op) {
        switch (op) {
            case BinaryOperator::Equality:
            case BinaryOperator::Inequality:
            case BinaryOperator::CaseEquality:
            case BinaryOperator::CaseInequality:
            case BinaryOperator::WildcardEquality:
            case BinaryOperator::WildcardInequality:
            case BinaryOperator::GreaterThan:
            case BinaryOperator::GreaterThanEqual:
            case BinaryOperator::LessThan:
            case BinaryOperator::LessThanEqual: return true;
            default: return false;
        }
    }

    void handle(const BinaryExpression& b) {
        SourceLocation loc = b.opRange.start() ? b.opRange.start() : at(b, body.location);
        const Expression& l = strip(b.left());
        const Expression& r = strip(b.right());
        if (isCompare(b.op) && integral(l) && integral(r) && inAssert == 0) {
            bool lc = isConstExpr(l), rc = isConstExpr(r);
            bool rel = b.op == BinaryOperator::GreaterThan || b.op == BinaryOperator::GreaterThanEqual ||
                       b.op == BinaryOperator::LessThan || b.op == BinaryOperator::LessThanEqual;
            if (!lc && !rc) {
                bitwidth_t lw = widthOf(l), rw = widthOf(r);
                if (lw != rw && exactWidth(l) && exactWidth(r))
                    rep.add("W004", loc,
                            "the two sides of this comparison are " + std::to_string(lw) + " and " +
                                std::to_string(rw) + " bits wide");
                if (rel && l.type->isSigned() != r.type->isSigned())
                    rep.add("W005", loc,
                            std::string("a signed value is compared with an unsigned one; the comparison is unsigned, "
                                        "so a negative value counts as a large one"));
            } else if (lc != rc && on("W004")) {
                const Expression& k = lc ? l : r;
                const Expression& v = lc ? r : l;
                bitwidth_t kw = bitsOf(k, body);
                if (kw > widthOf(v) && !litXZ(k) && !(k.type->isSigned() && v.type->isSigned()))
                    rep.add("W004", loc,
                            "a value of " + std::to_string(widthOf(v)) + (widthOf(v) == 1 ? " bit" : " bits") +
                                " is compared with a constant that needs " + std::to_string(kw));
            }
        }
        if (hw() && !iface) {
            if ((b.op == BinaryOperator::Equality || b.op == BinaryOperator::Inequality) && (litXZ(l) || litXZ(r)))
                rep.add("SYN009", loc, "a comparison with X or Z is X in simulation and constant in hardware");
            if (b.op == BinaryOperator::CaseEquality || b.op == BinaryOperator::CaseInequality)
                rep.add("SYN010", loc, "=== and !== are not synthesizable as written; hardware has no X to compare with");
            if ((b.op == BinaryOperator::Divide || b.op == BinaryOperator::Mod || b.op == BinaryOperator::Power) &&
                on("SYN014") && !func && integral(l) && integral(r) && !(isConstExpr(l) && isConstExpr(r))) {
                // A loop index is a constant once the loop is unrolled.
                bool ok = true;
                b.visit(makeVisitor([&](auto&, const NamedValueExpression& n) {
                    if (isSignal(n.symbol) && moduleLevel(n.symbol)) ok = false;
                }));
                const Expression& k = b.op == BinaryOperator::Power ? l : r;
                if (isConstExpr(k)) {
                    EvalContext ctx(ASTContext(body, LookupLocation::max));
                    ConstantValue cv = k.eval(ctx);
                    if (cv.isInteger() && !cv.integer().hasUnknown()) {
                        auto v = cv.integer().as<uint64_t>();
                        ok = v && *v != 0 && (*v & (*v - 1)) == 0;
                    }
                }
                if (!ok)
                    rep.add("SYN014", loc,
                            b.op == BinaryOperator::Divide ? "division by a value that is not a constant power of two builds a divider"
                            : b.op == BinaryOperator::Mod  ? "modulo by a value that is not a constant power of two builds a divider"
                                                           : "a power whose base is not a constant power of two builds a multiplier chain");
            }
        }
        if (on("W007") && (b.op == BinaryOperator::LogicalAnd || b.op == BinaryOperator::LogicalOr))
            for (const Expression* o : {&l, &r})
                if (integral(*o) && widthOf(*o) > 1)
                    rep.add("W007", loc, "a logical operator is applied to a value of " + std::to_string(widthOf(*o)) + " bits");
        visitDefault(b);
    }

    void handle(const UnaryExpression& u) {
        const Expression& o = strip(u.operand());
        if (on("W007") && u.op == UnaryOperator::LogicalNot && integral(o) && widthOf(o) > 1)
            rep.add("W007", at(u, body.location),
                    "! is applied to a value of " + std::to_string(widthOf(o)) + " bits; ~ is the bitwise operator");
        visitDefault(u);
    }

    void handle(const ConditionalExpression& c) {
        if (on("W006"))
            for (auto& cond : c.conditions) multiBitCond(*cond.expr);
        const Expression& l = strip(c.left());
        const Expression& r = strip(c.right());
        if (on("W008") && integral(l) && integral(r) && !isConstExpr(l) && !isConstExpr(r) && exactWidth(l) && exactWidth(r)) {
            bitwidth_t lw = widthOf(l), rw = widthOf(r);
            if (lw != rw)
                rep.add("W008", c.opLoc ? c.opLoc : at(c, body.location),
                        "the branches of ?: are " + std::to_string(lw) + " and " + std::to_string(rw) + " bits wide");
        }
        visitDefault(c);
    }

    void handle(const CallExpression& c) {
        if (c.isSystemCall()) {
            std::string_view n = c.getSubroutineName();
            if (hw() && !iface && badSystemCall(n))
                rep.add("SYN003", at(c, body.location), std::string(n) + " is not synthesizable");
            if (writesArgs(n))
                for (const Expression* a : c.arguments()) markBoth(*a);
        }
        visitDefault(c);
    }

    // ------------------------------------------------------------------ the end

    void finish() {
        for (auto& [s, u] : use)
            if ((u.bits & UseBlk) && (u.bits & UseNba))
                rep.add("SEQ001", s->location,
                        "'" + shown(*s) + "' has both blocking and non-blocking assignments");
        for (auto& [s, blk] : seqBlocking) {
            const Use& u = use[s];
            auto pd = portDir.find(s);
            bool elsewhere = u.multi || (u.reader && u.reader != blk) || (pd != portDir.end() && pd->second != 1);
            if (elsewhere)
                rep.add("SEQ002", blk->location,
                        "blocking assignment to '" + shown(*s) +
                            "' in an edge-triggered always, and it is read outside the block; use <=");
        }
        // For the unused rules: every symbol's use, by where it is declared,
        // so that the bodies of one module add up.
        for (auto& [s, u] : use)
            if (s->location) outp.uses[locKey(s->location)] |= u.bits & (UseR | UseW);
        for (const Symbol* s : decls) {
            if (!s->location || P.unusedOk.ok(s->name)) continue;
            auto pd = portDir.find(s);
            outp.cands.push_back({locKey(s->location), shown(*s), s->location,
                                  pd == portDir.end() ? uint8_t(0) : pd->second});
        }
    }
};

// ------------------------------------------------------------- across modules

struct Global : ASTVisitor<Global, VisitFlags::Symbols> {
    const Params& P;
    explicit Global(const Params& p) : P(p) {}

    // Signals that a port connects are one signal. Only a plain name of the
    // port's own width is joined: a bit of a bus is not the bus.
    std::unordered_map<const Symbol*, const Symbol*> up;
    const Symbol* find(const Symbol* s) {
        const Symbol* r = s;
        for (auto it = up.find(r); it != up.end() && it->second != r; it = up.find(r)) r = it->second;
        if (r != s) up[s] = r;
        return r;
    }
    void join(const Symbol* a, const Symbol* b) {
        a = find(a);
        b = find(b);
        if (a != b) up[b] = a;
    }

    // target <- the signals it is made from, whole words only.
    std::vector<std::pair<const Symbol*, const Symbol*>> edges;
    // The first place each 1-bit signal is read as data.
    std::unordered_map<const Symbol*, SourceLocation> dataRead;
    std::vector<const Symbol*> dataOrder;
    struct FlopRec {
        Flop f;
        SourceLocation loc;
    };
    std::vector<FlopRec> flops;
    bool tb = false;
    const Scope* scope = nullptr;  // the module body the walk is in

    void handle(const InstanceSymbol& inst) {
        if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return;
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn || conn->port.kind != SymbolKind::Port) continue;
            const auto& ps = conn->port.as<PortSymbol>();
            const Expression* x = conn->getExpression();
            if (!x || !ps.internalSymbol) continue;
            if (x->kind == ExpressionKind::Assignment) x = &x->as<AssignmentExpression>().left();
            const Expression& a = strip(*x);
            if (isName(a) && a.type->getBitWidth() == ps.getType().getBitWidth()) join(nameOf(a), ps.internalSymbol);
        }
        bool saved = tb;
        const Scope* savedScope = scope;
        tb = isTb(inst.body, P);
        scope = &inst.body;
        visitDefault(inst);
        tb = saved;
        scope = savedScope;
    }

    void handle(const GenerateBlockSymbol& g) {
        if (!g.isUninstantiated) visitDefault(g);
    }

    void noteRead(const Symbol& s, SourceLocation loc) {
        if (tb || !isSignal(s)) return;
        const Type& t = s.as<ValueSymbol>().getType();
        if (!t.isIntegral() || t.getBitWidth() != 1) return;
        if (dataRead.try_emplace(&s, loc).second) dataOrder.push_back(&s);
    }

    // Every name read in an expression, as data.
    void readsAsData(const Expression& e, const Symbol* except = nullptr) {
        e.visit(makeVisitor([&](auto&, const NamedValueExpression& n) {
            if (&n.symbol != except) noteRead(n.symbol, n.sourceRange.start());
        }));
    }

    struct Dep {
        const Symbol* s;
        bool strong;
    };
    using Deps = std::vector<Dep>;

    // The dependencies of each assignment in a combinational block, in
    // statement order: a value assigned earlier in the block stands for what
    // it was made from, so "y = a; y = y + 1" is not a loop.
    struct CombWalk {
        Global& g;
        std::unordered_map<const Symbol*, Deps> cur;
        std::vector<Deps> ctrl;

        static void add(Deps& d, const Symbol* s, bool strong) {
            for (Dep& x : d)
                if (x.s == s) {
                    x.strong |= strong;
                    return;
                }
            d.push_back({s, strong});
        }

        struct Reader : ASTVisitor<Reader, VisitFlags::Expressions> {
            CombWalk& w;
            Deps& out;
            bool weak = false;
            Reader(CombWalk& cw, Deps& o) : w(cw), out(o) {}
            void name(const Symbol& s, SourceLocation loc) {
                if (!isSignal(s)) return;
                w.g.noteRead(s, loc);
                auto it = w.cur.find(&s);
                if (it == w.cur.end()) add(out, &s, !weak);
                else
                    for (const Dep& d : it->second) add(out, d.s, d.strong && !weak);
            }
            void handle(const NamedValueExpression& n) { name(n.symbol, n.sourceRange.start()); }
            void handle(const HierarchicalValueExpression& n) { name(n.symbol, n.sourceRange.start()); }
            template<typename T>
            void select(const T& e) {
                bool s = weak;
                weak = true;
                e.value().visit(*this);
                weak = s;
            }
            void handle(const ElementSelectExpression& e) {
                select(e);
                e.selector().visit(*this);
            }
            void handle(const RangeSelectExpression& e) {
                select(e);
                e.left().visit(*this);
                e.right().visit(*this);
            }
            void handle(const MemberAccessExpression& e) { select(e); }
            // What a parameter switches off is not a dependency:
            // "empty & ~(FALL_THROUGH & push)" with FALL_THROUGH = 0 does not depend on push.
            void handle(const BinaryExpression& b) {
                if (w.g.scope && (b.op == BinaryOperator::BinaryAnd || b.op == BinaryOperator::LogicalAnd))
                    for (const Expression* side : {&b.left(), &b.right()})
                        if (auto t = constTruth(*side, *w.g.scope); t && !*t) return;
                if (w.g.scope && b.op == BinaryOperator::LogicalOr)
                    for (const Expression* side : {&b.left(), &b.right()})
                        if (auto t = constTruth(*side, *w.g.scope); t && *t) return;
                visitDefault(b);
            }
            void handle(const ConditionalExpression& c) {
                if (w.g.scope && c.conditions.size() == 1 && !c.conditions[0].pattern)
                    if (auto t = constTruth(*c.conditions[0].expr, *w.g.scope)) {
                        (*t ? c.left() : c.right()).visit(*this);
                        return;
                    }
                visitDefault(c);
            }
        };

        void reads(const Expression& e, Deps& out) {
            Reader r(*this, out);
            e.visit(r);
        }

        void targets(const Expression& e, std::vector<std::pair<const Symbol*, bool>>& out, Deps& d) {
            switch (e.kind) {
                case ExpressionKind::NamedValue:
                case ExpressionKind::HierarchicalValue: out.push_back({nameOf(e), true}); break;
                case ExpressionKind::Conversion: targets(e.as<ConversionExpression>().operand(), out, d); break;
                case ExpressionKind::Concatenation:
                    for (const Expression* o : e.as<ConcatenationExpression>().operands()) targets(*o, out, d);
                    break;
                case ExpressionKind::ElementSelect:
                    reads(e.as<ElementSelectExpression>().selector(), d);
                    if (const Symbol* t = firstTarget(e.as<ElementSelectExpression>().value())) out.push_back({t, false});
                    break;
                case ExpressionKind::RangeSelect:
                case ExpressionKind::MemberAccess:
                default:
                    if (const Symbol* t = firstTarget(e)) out.push_back({t, false});
                    break;
            }
        }

        void assign(const AssignmentExpression& a) {
            Deps d;
            reads(a.right(), d);
            if (a.isCompound()) reads(a.left(), d);
            for (const Deps& c : ctrl)
                for (const Dep& x : c) add(d, x.s, x.strong);
            std::vector<std::pair<const Symbol*, bool>> ts;
            targets(a.left(), ts, d);
            for (auto& [t, whole] : ts) {
                if (!t || !isSignal(*t)) continue;
                auto it = cur.find(t);
                if (whole) {
                    if (!g.tb)
                        for (const Dep& x : d)
                            if (x.strong) g.edges.push_back({t, x.s});
                    if (it == cur.end() || ctrl.empty()) cur[t] = d;
                    else
                        for (const Dep& x : d) add(it->second, x.s, x.strong);
                } else if (it != cur.end()) {
                    for (const Dep& x : d) add(it->second, x.s, false);
                }
            }
        }

        void expr(const Expression& e) {
            if (e.kind == ExpressionKind::Assignment) {
                assign(e.as<AssignmentExpression>());
            } else {
                Deps d;
                reads(e, d);
            }
        }

        void under(const Deps& c, const Statement& s) {
            ctrl.push_back(c);
            stmt(s);
            ctrl.pop_back();
        }

        void stmt(const Statement& s) {
            switch (s.kind) {
                case StatementKind::List:
                    for (const Statement* c : s.as<StatementList>().list) stmt(*c);
                    break;
                case StatementKind::Block: stmt(s.as<BlockStatement>().body); break;
                case StatementKind::ExpressionStatement: expr(s.as<ExpressionStatement>().expr); break;
                case StatementKind::Timed: stmt(s.as<TimedStatement>().stmt); break;
                case StatementKind::VariableDeclaration: {
                    const VariableSymbol& v = s.as<VariableDeclStatement>().symbol;
                    Deps d;
                    if (const Expression* i = v.getInitializer()) reads(*i, d);
                    cur[&v] = d;
                    break;
                }
                case StatementKind::Conditional: {
                    const auto& c = s.as<ConditionalStatement>();
                    if (g.scope && c.conditions.size() == 1 && !c.conditions[0].pattern)
                        if (auto t = constTruth(*c.conditions[0].expr, *g.scope)) {
                            if (*t) stmt(c.ifTrue);
                            else if (c.ifFalse) stmt(*c.ifFalse);
                            break;
                        }
                    Deps d;
                    for (auto& cond : c.conditions) reads(*cond.expr, d);
                    ctrl.push_back(d);
                    stmt(c.ifTrue);
                    if (c.ifFalse) stmt(*c.ifFalse);
                    ctrl.pop_back();
                    break;
                }
                case StatementKind::Case: {
                    const auto& c = s.as<CaseStatement>();
                    Deps d;
                    reads(c.expr, d);
                    for (auto& it : c.items)
                        for (const Expression* e : it.expressions) reads(*e, d);
                    ctrl.push_back(d);
                    for (auto& it : c.items) stmt(*it.stmt);
                    if (c.defaultCase) stmt(*c.defaultCase);
                    ctrl.pop_back();
                    break;
                }
                case StatementKind::ForLoop: {
                    const auto& l = s.as<ForLoopStatement>();
                    for (const VariableSymbol* v : l.loopVars) cur[v] = {};
                    for (const Expression* e : l.initializers)
                        if (e->kind == ExpressionKind::Assignment)
                            if (const Symbol* t = firstTarget(e->as<AssignmentExpression>().left())) cur[t] = {};
                    Deps d;
                    if (l.stopExpr) reads(*l.stopExpr, d);
                    under(d, l.body);
                    break;
                }
                case StatementKind::ForeachLoop: {
                    const auto& l = s.as<ForeachLoopStatement>();
                    for (auto& dim : l.loopDims)
                        if (dim.loopVar) cur[dim.loopVar] = {};
                    under({}, l.body);
                    break;
                }
                case StatementKind::WhileLoop: {
                    Deps d;
                    reads(s.as<WhileLoopStatement>().cond, d);
                    under(d, s.as<WhileLoopStatement>().body);
                    break;
                }
                case StatementKind::DoWhileLoop: {
                    Deps d;
                    reads(s.as<DoWhileLoopStatement>().cond, d);
                    under(d, s.as<DoWhileLoopStatement>().body);
                    break;
                }
                case StatementKind::RepeatLoop: {
                    Deps d;
                    reads(s.as<RepeatLoopStatement>().count, d);
                    under(d, s.as<RepeatLoopStatement>().body);
                    break;
                }
                case StatementKind::ForeverLoop: under({}, s.as<ForeverLoopStatement>().body); break;
                default: break;
            }
        }
    };

    void handle(const ContinuousAssignSymbol& ca) {
        const Expression& e = ca.getAssignment();
        if (e.kind != ExpressionKind::Assignment) return;
        const auto& a = e.as<AssignmentExpression>();
        CombWalk w{*this, {}, {}};
        if (isName(strip(a.right()))) {
            // A plain buffer: an edge for the loop search, but not a read of a clock as data.
            if (const Symbol* t = isName(a.left()) ? nameOf(a.left()) : nullptr; t && !tb)
                edges.push_back({t, nameOf(strip(a.right()))});
            return;
        }
        w.assign(a);
    }

    void handle(const ProceduralBlockSymbol& p) {
        Shape s = shapeOf(p);
        if (s.cls == Cls::Comb) {
            CombWalk w{*this, {}, {}};
            w.stmt(*s.inner);
        } else if (s.cls == Cls::Seq || s.cls == Cls::Latch) {
            if (s.cls == Cls::Seq && !tb) flops.push_back({flopOf(s), p.location});
            // q <= ~q is a divider, which the DFT rules report; here only
            // reads by other logic count.
            s.inner->visit(makeVisitor(
                [&](auto& v, const AssignmentExpression& a) {
                    readsAsData(a.right(), firstTarget(a.left()));
                    (void)v;
                },
                [&](auto&, const NamedValueExpression& n) { noteRead(n.symbol, n.sourceRange.start()); }));
        }
    }

    // ----------------------------------------------------------------- results

    static std::string nameIn(const Symbol& s, bool local) { return local ? shown(s) : path(s); }

    void analyze(Report& rep) {
        // Clocks and resets, by joined signal.
        std::unordered_set<const Symbol*> clocks, aresets;
        struct RstUse {
            SourceLocation high, low, async, sync;
            bool hasHigh = false, hasLow = false, hasAsync = false, hasSync = false;
            const Symbol* sym = nullptr;
        };
        std::unordered_map<const Symbol*, RstUse> rst;
        std::vector<const Symbol*> rstOrder;
        for (auto& fr : flops) {
            if (fr.f.clk) clocks.insert(find(fr.f.clk));
            if (fr.f.arst) aresets.insert(find(fr.f.arst));
        }
        auto note = [&](const Symbol* s, bool low, bool async, SourceLocation loc) {
            const Symbol* c = find(s);
            auto [it, fresh] = rst.try_emplace(c);
            RstUse& u = it->second;
            if (fresh) {
                u.sym = s;
                rstOrder.push_back(c);
            }
            if (low && !u.hasLow) u.low = loc, u.hasLow = true;
            if (!low && !u.hasHigh) u.high = loc, u.hasHigh = true;
            if (async && !u.hasAsync) u.async = loc, u.hasAsync = true;
            if (!async && !u.hasSync) u.sync = loc, u.hasSync = true;
        };
        for (auto& fr : flops) {
            if (fr.f.arst) note(fr.f.arst, fr.f.arstLow, true, fr.loc);
            else if (fr.f.srst && (aresets.count(find(fr.f.srst)) || P.reset.ok(fr.f.srst->name)))
                note(fr.f.srst, fr.f.srstLow, false, fr.loc);
        }
        for (const Symbol* c : rstOrder) {
            const RstUse& u = rst[c];
            std::string n = path(*c);
            if (u.hasHigh && u.hasLow)
                rep.add("RST001", std::max(locKey(u.high), locKey(u.low)) == locKey(u.high) ? u.high : u.low,
                        "reset '" + n + "' is active low in one block and active high in another");
            if (u.hasAsync && u.hasSync)
                rep.add("RST002", u.sync,
                        "reset '" + n + "' is synchronous here but asynchronous in another block");
            if (clocks.count(c))
                rep.add("CLK003", c->location, "'" + n + "' is used both as a clock and as a reset");
        }
        for (const Symbol* s : dataOrder)
            if (clocks.count(find(s)))
                rep.add("CLK001", dataRead[s],
                        "clock '" + shown(*s) + "' is used as data; keep clock gating and muxing in dedicated cells");
        loops(rep);
    }

    // Strongly connected components of the dependency graph (Tarjan, with an
    // explicit stack: a combinational chain can be thousands of signals deep).
    void loops(Report& rep) {
        std::unordered_map<const Symbol*, int> ix;
        std::vector<const Symbol*> nodes;
        auto id = [&](const Symbol* s) {
            s = find(s);
            auto [it, fresh] = ix.try_emplace(s, (int)nodes.size());
            if (fresh) nodes.push_back(s);
            return it->second;
        };
        std::vector<std::pair<int, int>> es;
        es.reserve(edges.size());
        for (auto& [t, s] : edges) {
            int a = id(t);
            int b = id(s);
            es.push_back({a, b});
        }
        std::sort(es.begin(), es.end());
        es.erase(std::unique(es.begin(), es.end()), es.end());
        int n = (int)nodes.size();
        std::vector<int> start(n + 1, 0);
        for (auto& e : es) start[e.first + 1]++;
        for (int i = 0; i < n; i++) start[i + 1] += start[i];
        auto dep = [&](int v, int k) { return es[start[v] + k].second; };
        auto ndep = [&](int v) { return start[v + 1] - start[v]; };

        std::vector<int> low(n, 0), num(n, -1), comp(n, -1), stack, pos(n, 0);
        std::vector<char> onStack(n, 0);
        int counter = 0, ncomp = 0;
        std::vector<int> call;
        for (int root = 0; root < n; root++) {
            if (num[root] != -1) continue;
            call.push_back(root);
            while (!call.empty()) {
                int v = call.back();
                if (num[v] == -1) {
                    num[v] = low[v] = counter++;
                    stack.push_back(v);
                    onStack[v] = 1;
                }
                bool descended = false;
                while (pos[v] < ndep(v)) {
                    int w = dep(v, pos[v]++);
                    if (num[w] == -1) {
                        call.push_back(w);
                        descended = true;
                        break;
                    }
                    if (onStack[w]) low[v] = std::min(low[v], num[w]);
                }
                if (descended) continue;
                if (low[v] == num[v]) {
                    for (;;) {
                        int w = stack.back();
                        stack.pop_back();
                        onStack[w] = 0;
                        comp[w] = ncomp;
                        if (w == v) break;
                    }
                    ncomp++;
                }
                call.pop_back();
                if (!call.empty()) low[call.back()] = std::min(low[call.back()], low[v]);
            }
        }
        // One finding per component with a cycle: the shortest way round from
        // its first signal.
        std::vector<char> done(ncomp, 0);
        for (int v = 0; v < n; v++) {
            int c = comp[v];
            if (done[c]) continue;
            done[c] = 1;
            // Breadth first from v back to v, inside the component.
            std::unordered_map<int, int> prev;
            std::vector<int> q{v};
            int last = -1;
            for (size_t h = 0; h < q.size() && last < 0; h++) {
                int x = q[h];
                for (int k = 0; k < ndep(x); k++) {
                    int w = dep(x, k);
                    if (comp[w] != c) continue;
                    if (w == v) {
                        last = x;
                        break;
                    }
                    if (prev.try_emplace(w, x).second) q.push_back(w);
                }
            }
            if (last < 0) continue;  // one node, no self edge
            std::vector<int> cyc{v};  // v depends on ... depends on last depends on v
            std::vector<int> back;
            for (int x = last; x != v; x = prev[x]) back.push_back(x);
            // Printed in the direction the value flows: a -> b means a drives b.
            cyc.insert(cyc.end(), back.begin(), back.end());
            const InstanceBodySymbol* b0 = bodyOf(*nodes[v]);
            bool local = true;
            for (int x : cyc) local &= bodyOf(*nodes[x]) == b0;
            std::string text;
            size_t shown = 0;
            for (int x : cyc) {
                if (shown++ == 8) {
                    text += " -> ...";
                    break;
                }
                text += (text.empty() ? "" : " -> ") + nameIn(*nodes[x], local);
            }
            text += " -> " + nameIn(*nodes[v], local);
            rep.add("COMB002", nodes[v]->location,
                    "combinational loop: " + text + "; break it with a flop or remove the feedback");
        }
    }
};

// ------------------------------------------------------------------ the bodies

// Each distinct module body, once, in hierarchy order.
struct BodyCollector : ASTVisitor<BodyCollector, VisitFlags::Symbols> {
    std::vector<const InstanceBodySymbol*> out;
    std::unordered_set<const InstanceBodySymbol*> seen;
    void handle(const InstanceSymbol& i) {
        // A module nobody instantiates is elaborated only to check it; its
        // own instances are placeholders, so it is not part of the design.
        if (i.body.flags.has(InstanceFlags::Uninstantiated)) return;
        const InstanceBodySymbol* b = i.getCanonicalBody();
        if (!b) b = &i.body;
        if (!seen.insert(b).second) return;
        out.push_back(b);
        visitDefault(*b);
    }
    void handle(const GenerateBlockSymbol& g) {
        if (!g.isUninstantiated) visitDefault(g);
    }
};

// Files and module names.
inline void checkDefinitions(Compilation& comp, const Params& P, Report& rep) {
    const SourceManager& sm = *comp.getSourceManager();
    std::map<std::string, std::vector<const DefinitionSymbol*>> byFile;
    for (const Symbol* s : comp.getDefinitions()) {
        if (s->kind != SymbolKind::Definition) continue;
        const auto& d = s->as<DefinitionSymbol>();
        if (d.definitionKind != DefinitionKind::Module || !d.location) continue;
        std::string name = shown(d);
        if (rep.rules.enabled("NAME001") && !P.module.ok(name))
            rep.add("NAME001", d.location, "module '" + name + "' does not match " + P.module.src);
        if (rep.rules.enabled("NAME010") && name.size() > P.maxLen)
            rep.add("NAME010", d.location,
                    "'" + name + "' is " + std::to_string(name.size()) + " characters long; the limit is " + std::to_string(P.maxLen));
        std::string file(sm.getFileName(sm.getFullyOriginalLoc(d.location)));
        byFile[file].push_back(&d);
        size_t slash = file.find_last_of("/\\");
        std::string stem = slash == std::string::npos ? file : file.substr(slash + 1);
        stem = stem.substr(0, stem.find('.'));
        if (stem != name)
            rep.add("STY002", d.location, "module '" + name + "' is in a file named '" + stem + "'");
    }
    for (auto& [file, defs] : byFile) {
        if (defs.size() < 2) continue;
        std::vector<const DefinitionSymbol*> v = defs;
        std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->location.offset() < b->location.offset(); });
        for (size_t i = 1; i < v.size(); i++)
            rep.add("STY001", v[i]->location,
                    "module '" + std::string(v[i]->name) + "' shares its file with '" + std::string(v[0]->name) +
                        "'; one module per file is easier to find and to compile");
    }
}

// Signals nobody reads, or nobody drives, over every body of each module.
inline void checkUnused(std::vector<BodyOut>& outs, Report& rep) {
    std::unordered_map<uint64_t, uint8_t> uses;
    for (auto& o : outs)
        for (auto& [k, b] : o.uses) uses[k] |= b;
    std::unordered_set<uint64_t> done;
    for (auto& o : outs)
        for (const Cand& c : o.cands) {
            if (!done.insert(c.key).second) continue;
            auto it = uses.find(c.key);
            uint8_t b = it == uses.end() ? 0 : it->second;
            bool r = b & UseR, w = b & UseW;
            const std::string q = "'" + c.name + "'";
            if (c.port == 1) {
                if (!r) rep.add("UNU004", c.loc, "input " + q + " is never read");
            } else if (c.port == 2) {
                if (!w) rep.add("UNU005", c.loc, "output " + q + " is never driven");
            } else if (c.port == 0) {
                if (!r && !w) rep.add("UNU001", c.loc, q + " is declared but never used");
                else if (!r) rep.add("UNU002", c.loc, q + " is driven but never read");
                else if (!w) rep.add("UNU003", c.loc, q + " is read but never driven");
            }
        }
}

// -------------------------------------------------------------------- FSMs

// FSM001..FSM004, on the state machines the shared extractor finds
// (fsm_extract.hpp, the one FSM coverage uses). One finding per register of
// a module, whatever the number of instances: the text names no instance.
inline void checkFsms(const std::vector<ExtractedFsm>& fsms, Report& rep) {
    for (const ExtractedFsm& f : fsms) {
        if (!f.var || f.states.empty()) continue;
        SourceLocation loc = f.var->location;
        std::string reg = "'" + std::string(shown(*f.var)) + "'";
        auto label = [&](uint64_t v, const std::string& n) {
            return n.empty() ? std::to_string(v) : n;
        };
        std::set<uint64_t> in, out, reset(f.resetStates.begin(), f.resetStates.end());
        for (auto& t : f.trans) {
            if (t.reset) continue;   // the reset branch leaves every state
            in.insert(t.to);
            out.insert(t.from);
        }
        for (auto& [v, n] : f.states) {
            if (!in.count(v) && !reset.count(v))
                rep.add("FSM001", loc,
                        "state " + label(v, n) + " of " + reg + " is never entered: no transition and no reset leads to it");
            // A machine of one state is a flag, not a trap.
            if (!out.count(v) && f.states.size() > 1 && (in.count(v) || reset.count(v)))
                rep.add("FSM002", loc,
                        "state " + label(v, n) + " of " + reg + " has no way out: once entered it is left only by reset");
        }
        if (f.caseOnState && !f.caseDefault && !f.caseFull) {   // unique, priority and full_case say it is meant
            // Every encoding a state: nothing for a default to catch.
            bool full = !f.onehot && f.w < 63 && f.states.size() == (uint64_t(1) << f.w);
            if (!full)
                rep.add("FSM003", loc,
                        "the case on state register " + reg + " has no default, and " + std::to_string(f.states.size()) +
                            " of its " + (f.w < 63 ? std::to_string(uint64_t(1) << f.w) : std::string("many")) +
                            " encodings are states: any other value is held forever");
        }
        if (f.resetStates.empty())
            rep.add("FSM004", loc, "state register " + reg + " has no reset state: the machine starts in an unknown state");
    }
}

}  // namespace lintrules
