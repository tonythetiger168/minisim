// mdebug.cpp - source-level debug for SystemVerilog on the msim front end.
//
//   mdebug tb.sv dut.sv -top tb -drivers tb.u.q        who assigns it
//   mdebug ...            -loads tb.u.q                who reads it
//   mdebug ...            -trace tb.u.q -depth 3       the fan-in cone
//   mdebug ... -vcd w.vcd -active tb.u.q -at 45        which assignment gave
//                                                      it its value at t=45,
//                                                      and what caused that
//   mdebug ... [-vcd w.vcd] -html debug.html           one-file viewer:
//                                                      hierarchy, source,
//                                                      drivers/loads,
//                                                      schematic, waves
//
// The model: every assignment that can give a signal a value is a Driver
// (a continuous assign, a procedural assignment with the if/case conditions
// that guard it, or a port connection). Loads are the same records read the
// other way. The active-driver trace evaluates the guards against the values
// in a VCD: a flop's guards are sampled just before the time its output
// changed, combinational logic at that time.
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>

#include "lintir.hpp"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "parallel.hpp"
#include "version.hpp"
#include "wave_reader.hpp"

using namespace lintir;

namespace {

#include "mdebug_view.inc"

// ------------------------------------------------------------------ source

struct Where {
    int file = -1;
    size_t line = 0, col = 0;
};

struct SrcFile {
    std::string name;
    BufferID buf;
    std::vector<std::string_view> lines;
};

struct Sources {
    const SourceManager* sm = nullptr;
    std::vector<SrcFile> files;
    std::map<uint32_t, int> byBuf;

    Where where(SourceLocation loc) {
        Where w;
        if (!sm || !loc) return w;
        auto full = sm->getFullyOriginalLoc(loc);
        BufferID b = full.buffer();
        auto it = byBuf.find(b.getId());
        if (it == byBuf.end()) {
            SrcFile f;
            f.name = std::string(sm->getFileName(full));
            f.buf = b;
            std::string_view text = sm->getSourceText(b);
            size_t p = 0;
            while (p <= text.size()) {
                size_t e = text.find('\n', p);
                if (e == std::string_view::npos) e = text.size();
                f.lines.push_back(text.substr(p, e - p));
                p = e + 1;
            }
            it = byBuf.emplace(b.getId(), (int)files.size()).first;
            files.push_back(std::move(f));
        }
        w.file = it->second;
        w.line = sm->getLineNumber(full);
        w.col = sm->getColumnNumber(full);
        return w;
    }

    std::string at(const Where& w) const {
        if (w.file < 0) return "<unknown>";
        return files[w.file].name + ":" + std::to_string(w.line);
    }

    // The source line, without its indentation or a trailing comment's spaces.
    std::string lineText(const Where& w) const {
        if (w.file < 0 || w.line == 0 || w.line > files[w.file].lines.size()) return "";
        std::string_view s = files[w.file].lines[w.line - 1];
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
        return std::string(s);
    }

    // The text of an expression as written.
    std::string text(SourceRange r) const {
        if (!sm || !r.start() || !r.end()) return "?";
        auto a = sm->getFullyOriginalLoc(r.start()), b = sm->getFullyOriginalLoc(r.end());
        if (a.buffer() != b.buffer() || b.offset() <= a.offset()) return "?";
        std::string_view t = sm->getSourceText(a.buffer());
        if (b.offset() > t.size()) return "?";
        std::string out;
        for (char c : t.substr(a.offset(), b.offset() - a.offset())) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            if (c == ' ' && !out.empty() && out.back() == ' ') continue;
            out += c;
        }
        return out;
    }
};

// ------------------------------------------------------------------- model

struct Guard {
    enum Kind { If, Else, CaseItem, CaseDefault, Opaque } kind = Opaque;
    const Expression* cond = nullptr;     // If, Else
    const CaseStatement* cs = nullptr;    // CaseItem, CaseDefault
    size_t item = 0;
};

struct Driver {
    const Symbol* target = nullptr;
    std::string kind;                     // assign, always_ff, always_comb, always, initial, final, port, "task name"
    SourceLocation loc;
    SourceRange range;                    // the assignment as written; empty for a port
    const Expression* rhs = nullptr;      // null for an output port
    std::vector<Guard> guards;
    bool seq = false;
    bool whole = false;                   // assigns the whole target, not a select of it
    std::string clock;                    // the event control as written, when seq
    std::string how;                      // "", or force, assign (procedural), call (an output argument)
    int block = -1;                       // which process; -1 outside one
    std::vector<const Symbol*> reads;     // data: the right-hand side
    std::vector<const Symbol*> creads;    // control: guards and select indices
};

struct ScopeRec {
    std::string path, def;
    SourceLocation loc;                   // the module definition
    SourceLocation inst;                  // where it is instantiated
};

struct DB {
    Sources src;
    std::vector<ScopeRec> scopes;
    std::vector<const ValueSymbol*> signals;
    std::unordered_map<const Symbol*, int> sigIx;
    std::map<std::string, const Symbol*> byPath;
    std::vector<Driver> drivers;
    std::unordered_map<const Symbol*, std::vector<int>> driversOf, loadsOf;

    void add(Driver d) {
        if (!d.target) return;
        auto uniq = [](std::vector<const Symbol*>& v) {
            std::vector<const Symbol*> o;
            for (const Symbol* s : v)
                if (std::find(o.begin(), o.end(), s) == o.end()) o.push_back(s);
            v = std::move(o);
        };
        uniq(d.reads);
        uniq(d.creads);
        int ix = (int)drivers.size();
        driversOf[d.target].push_back(ix);
        for (const Symbol* s : d.reads) loadsOf[s].push_back(ix);
        for (const Symbol* s : d.creads)
            if (std::find(d.reads.begin(), d.reads.end(), s) == d.reads.end()) loadsOf[s].push_back(ix);
        drivers.push_back(std::move(d));
    }
};

// The symbols an assignment's left side writes; select indices are reads.
void lhsTargets(const Expression& e, std::vector<const Symbol*>& targets, std::vector<const Symbol*>& idx) {
    switch (e.kind) {
        case ExpressionKind::NamedValue: targets.push_back(&e.as<NamedValueExpression>().symbol); break;
        case ExpressionKind::HierarchicalValue:
            targets.push_back(&e.as<HierarchicalValueExpression>().symbol);
            break;
        case ExpressionKind::ElementSelect: {
            const auto& s = e.as<ElementSelectExpression>();
            lhsTargets(s.value(), targets, idx);
            namesIn(s.selector(), idx);
            break;
        }
        case ExpressionKind::RangeSelect: {
            const auto& s = e.as<RangeSelectExpression>();
            lhsTargets(s.value(), targets, idx);
            namesIn(s.left(), idx);
            namesIn(s.right(), idx);
            break;
        }
        case ExpressionKind::MemberAccess: lhsTargets(e.as<MemberAccessExpression>().value(), targets, idx); break;
        case ExpressionKind::Conversion: lhsTargets(e.as<ConversionExpression>().operand(), targets, idx); break;
        case ExpressionKind::Concatenation:
            for (const Expression* op : e.as<ConcatenationExpression>().operands()) lhsTargets(*op, targets, idx);
            break;
        default: namesIn(e, targets); break;
    }
}

// One process body: every assignment, with the conditions on the way to it.
struct Walker {
    DB& db;
    Driver proto;               // kind, seq, clock, block
    std::vector<Guard> gs;

    void controls(Driver& d) {
        d.guards = gs;
        for (const Guard& g : gs) {
            if (g.cond) namesIn(*g.cond, d.creads);
            if (g.cs) {
                namesIn(g.cs->expr, d.creads);
                if (g.kind == Guard::CaseItem)
                    for (const Expression* e : g.cs->items[g.item].expressions) namesIn(*e, d.creads);
            }
        }
    }

    // A task or function call: each output, inout or ref argument is driven
    // by the call, from the arguments that go in.
    void call(const CallExpression& c) {
        std::vector<const Expression*> outs;
        std::vector<const Symbol*> ins;
        for (const Expression* a : c.arguments()) {
            if (a->kind == ExpressionKind::Assignment &&
                a->as<AssignmentExpression>().right().kind == ExpressionKind::EmptyArgument)
                outs.push_back(&a->as<AssignmentExpression>().left());
            else
                namesIn(*a, ins);
        }
        // What goes in reaches the task's own assignments through its formals.
        if (c.subroutine.index() == 0) {
            auto formals = std::get<0>(c.subroutine)->getArguments();
            auto args = c.arguments();
            for (size_t i = 0; i < formals.size() && i < args.size(); i++) {
                if (args[i]->kind == ExpressionKind::Assignment || args[i]->kind == ExpressionKind::EmptyArgument) continue;
                Driver d = proto;
                d.how = "call";
                d.target = formals[i];
                d.loc = c.sourceRange.start();
                d.range = c.sourceRange;
                namesIn(*args[i], d.reads);
                controls(d);
                db.add(std::move(d));
            }
        }
        for (const Expression* o : outs) {
            std::vector<const Symbol*> targets, idx;
            lhsTargets(*o, targets, idx);
            for (const Symbol* t : targets) {
                Driver d = proto;
                d.how = "call";
                d.target = t;
                d.loc = c.sourceRange.start();
                d.range = c.sourceRange;
                d.reads = ins;
                d.creads = idx;
                controls(d);
                db.add(std::move(d));
            }
        }
    }

    // how: "" for a plain assignment, "force" or "assign" for the procedural
    // continuous forms, which override the other drivers while they hold.
    void assign(const AssignmentExpression& a, const char* how = "") {
        std::vector<const Symbol*> targets, idx;
        lhsTargets(a.left(), targets, idx);
        for (const Symbol* t : targets) {
            Driver d = proto;
            d.how = how;
            d.target = t;
            d.loc = a.sourceRange.start();
            d.range = a.sourceRange;
            d.rhs = &a.right();
            d.whole = a.left().kind == ExpressionKind::NamedValue ||
                      a.left().kind == ExpressionKind::HierarchicalValue;
            namesIn(a.right(), d.reads);
            d.creads = idx;
            controls(d);
            db.add(std::move(d));
        }
    }

    void opaque(const Statement& body) {
        gs.push_back(Guard{});
        walk(body);
        gs.pop_back();
    }

    void walk(const Statement& s) {
        switch (s.kind) {
            case StatementKind::List:
                for (const Statement* c : s.as<StatementList>().list) walk(*c);
                break;
            case StatementKind::Block: walk(s.as<BlockStatement>().body); break;
            case StatementKind::Timed: walk(s.as<TimedStatement>().stmt); break;
            case StatementKind::ExpressionStatement: {
                const Expression& e = s.as<ExpressionStatement>().expr;
                if (e.kind == ExpressionKind::Assignment) assign(e.as<AssignmentExpression>());
                else if (e.kind == ExpressionKind::Call) call(e.as<CallExpression>());
                break;
            }
            case StatementKind::ProceduralAssign: {
                const auto& pa = s.as<ProceduralAssignStatement>();
                if (pa.assignment.kind == ExpressionKind::Assignment)
                    assign(pa.assignment.as<AssignmentExpression>(), pa.isForce ? "force" : "assign");
                break;
            }
            case StatementKind::Conditional: {
                const auto& c = s.as<ConditionalStatement>();
                bool plain = c.conditions.size() == 1 && !c.conditions[0].pattern;
                Guard g;
                g.kind = plain ? Guard::If : Guard::Opaque;
                g.cond = c.conditions[0].expr;
                gs.push_back(g);
                walk(c.ifTrue);
                if (c.ifFalse) {
                    if (plain) gs.back().kind = Guard::Else;
                    walk(*c.ifFalse);
                }
                gs.pop_back();
                break;
            }
            case StatementKind::Case: {
                const auto& c = s.as<CaseStatement>();
                for (size_t i = 0; i < c.items.size(); i++) {
                    gs.push_back(Guard{Guard::CaseItem, nullptr, &c, i});
                    walk(*c.items[i].stmt);
                    gs.pop_back();
                }
                if (c.defaultCase) {
                    gs.push_back(Guard{Guard::CaseDefault, nullptr, &c, 0});
                    walk(*c.defaultCase);
                    gs.pop_back();
                }
                break;
            }
            case StatementKind::ForLoop: opaque(s.as<ForLoopStatement>().body); break;
            case StatementKind::ForeachLoop: opaque(s.as<ForeachLoopStatement>().body); break;
            case StatementKind::WhileLoop: opaque(s.as<WhileLoopStatement>().body); break;
            case StatementKind::DoWhileLoop: opaque(s.as<DoWhileLoopStatement>().body); break;
            case StatementKind::RepeatLoop: opaque(s.as<RepeatLoopStatement>().body); break;
            case StatementKind::ForeverLoop: walk(s.as<ForeverLoopStatement>().body); break;
            default: {
                // Forks, waits and the rest: the assignments inside are still
                // drivers, but their conditions are not followed.
                gs.push_back(Guard{});
                s.visit(makeVisitor(
                    [&](auto& v, std::derived_from<Statement> auto& inner) { v.visitDefault(inner); },
                    [&](auto&, const AssignmentExpression& a) {
                        if (a.right().kind != ExpressionKind::EmptyArgument) assign(a);
                    },
                    [&](auto& v, const CallExpression& c) {
                        call(c);
                        v.visitDefault(c);
                    }));
                gs.pop_back();
                break;
            }
        }
    }
};

struct Collect : ASTVisitor<Collect> {
    DB& db;
    int nblock = 0;
    explicit Collect(DB& d) : db(d) {}

    void signal(const ValueSymbol& v) {
        // Only what lives in the design hierarchy: not a task's locals.
        const Scope* sc = v.getParentScope();
        if (!sc) return;
        SymbolKind k = sc->asSymbol().kind;
        if (k != SymbolKind::InstanceBody && k != SymbolKind::GenerateBlock && k != SymbolKind::Package) return;
        if (db.sigIx.count(&v)) return;
        db.sigIx[&v] = (int)db.signals.size();
        db.signals.push_back(&v);
        db.byPath[path(v)] = &v;
    }
    void handle(const VariableSymbol& v) { signal(v); }
    void handle(const NetSymbol& v) {
        signal(v);
        // wire n = a + b; is a continuous assignment.
        if (const Expression* init = v.getInitializer(); init && db.sigIx.count(&v)) {
            Driver d;
            d.kind = "assign";
            d.target = &v;
            d.loc = v.location;
            d.rhs = init;
            d.whole = true;
            namesIn(*init, d.reads);
            db.add(std::move(d));
        }
    }

    void handle(const GenerateBlockSymbol& g) {
        if (g.isUninstantiated) return;
        visitDefault(g);
    }

    void handle(const InstanceSymbol& inst) {
        if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return;
        ScopeRec sr;
        sr.path = path(inst);
        sr.def = std::string(inst.getDefinition().name);
        sr.loc = inst.getDefinition().location;
        sr.inst = inst.location;
        db.scopes.push_back(std::move(sr));
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn || conn->port.kind != SymbolKind::Port) continue;
            const auto& port = conn->port.as<PortSymbol>();
            const Symbol* in = port.internalSymbol;
            const Expression* ext = conn->getExpression();
            if (!in || !ext) continue;
            const Expression* outer = ext;
            if (outer->kind == ExpressionKind::Assignment) outer = &outer->as<AssignmentExpression>().left();
            Driver proto;
            proto.kind = "port";
            proto.loc = outer->sourceRange.start() ? outer->sourceRange.start() : inst.location;
            if (port.direction == ArgumentDirection::In || port.direction == ArgumentDirection::InOut) {
                Driver d = proto;
                d.target = in;
                d.rhs = outer;
                d.whole = true;
                namesIn(*outer, d.reads);
                db.add(std::move(d));
            }
            if (port.direction == ArgumentDirection::Out || port.direction == ArgumentDirection::InOut) {
                std::vector<const Symbol*> targets, idx;
                lhsTargets(*outer, targets, idx);
                for (const Symbol* t : targets) {
                    Driver d = proto;
                    d.target = t;
                    d.reads.push_back(in);
                    d.creads = idx;
                    db.add(std::move(d));
                }
            }
        }
        visitDefault(inst);
    }

    void handle(const ContinuousAssignSymbol& ca) {
        const Expression& e = ca.getAssignment();
        if (e.kind != ExpressionKind::Assignment) return;
        Walker w{db, {}, {}};
        w.proto.kind = "assign";
        w.assign(e.as<AssignmentExpression>());
    }

    // A task or function declared in the design writes its signals like any
    // process does; the body is one block.
    void handle(const SubroutineSymbol& sub) {
        const Scope* sc = sub.getParentScope();
        if (!sc) return;
        SymbolKind k = sc->asSymbol().kind;
        if (k != SymbolKind::InstanceBody && k != SymbolKind::GenerateBlock) return;
        if (sub.flags.has(MethodFlags::DPIImport) || sub.flags.has(MethodFlags::Pure)) return;
        Walker w{db, {}, {}};
        w.proto.kind = (sub.subroutineKind == SubroutineKind::Task ? "task " : "function ") + std::string(sub.name);
        w.proto.block = nblock++;
        w.walk(sub.getBody());
    }

    void handle(const ProceduralBlockSymbol& p) {
        Walker w{db, {}, {}};
        switch (p.procedureKind) {
            case ProceduralBlockKind::AlwaysFF: w.proto.kind = "always_ff"; break;
            case ProceduralBlockKind::AlwaysComb: w.proto.kind = "always_comb"; break;
            case ProceduralBlockKind::AlwaysLatch: w.proto.kind = "always_latch"; break;
            case ProceduralBlockKind::Always: w.proto.kind = "always"; break;
            case ProceduralBlockKind::Initial: w.proto.kind = "initial"; break;
            default: w.proto.kind = "final"; break;
        }
        w.proto.block = nblock++;
        const Statement& body = p.getBody();
        if (body.kind == StatementKind::Timed && w.proto.kind != "initial") {
            const auto& t = body.as<TimedStatement>();
            std::vector<const SignalEventControl*> evs;
            clocksOf(t.timing, evs);
            for (const SignalEventControl* e : evs)
                if (e->edge != EdgeKind::None) w.proto.seq = true;
            if (w.proto.seq) w.proto.clock = "@" + db.src.text(t.timing.sourceRange);
        }
        w.walk(body);
    }
};

// --------------------------------------------------------------- waveforms

using wave::Wave;
using wave::showBits;
using par::parallelFor;
using Vcd = wave::Reader;

// --------------------------------------------------------------- evaluator

// Expressions over waveform values, enough for the conditions RTL is written
// with. Anything it does not know gives nullopt, which the trace shows as "?".
struct Eval {
    const Vcd& vcd;
    uint64_t t = 0;

    std::optional<SVInt> lookup(const Symbol& s, const Type& type) {
        if (s.kind == SymbolKind::Parameter) {
            const ConstantValue& cv = s.as<ParameterSymbol>().getValue();
            if (cv.isInteger()) return cv.integer();
            return std::nullopt;
        }
        if (s.kind == SymbolKind::EnumValue) {
            const ConstantValue& cv = s.as<EnumValueSymbol>().getValue();
            if (cv.isInteger()) return cv.integer();
            return std::nullopt;
        }
        return named(path(s), type);
    }

    std::optional<SVInt> named(const std::string& name, const Type& type) {
        const Wave* w = vcd.find(name);
        if (!w || !type.isIntegral()) return std::nullopt;
        int i = w->at(t);
        std::string bits = i < 0 ? std::string(w->width, 'x') : w->bits(i);
        SVInt v = SVInt::fromString(std::to_string(bits.size()) + "'b" + bits);
        v.setSigned(type.isSigned());
        return v;
    }

    static SVInt bit(logic_t l) { return SVInt(l); }

    std::optional<SVInt> eval(const Expression& e) {
        switch (e.kind) {
            case ExpressionKind::NamedValue: return lookup(e.as<NamedValueExpression>().symbol, *e.type);
            case ExpressionKind::HierarchicalValue:
                return lookup(e.as<HierarchicalValueExpression>().symbol, *e.type);
            case ExpressionKind::IntegerLiteral: return e.as<IntegerLiteral>().getValue();
            case ExpressionKind::UnbasedUnsizedIntegerLiteral: {
                if (!e.type->isIntegral()) return std::nullopt;
                logic_t l = e.as<UnbasedUnsizedIntegerLiteral>().getLiteralValue();
                bitwidth_t w = e.type->getBitWidth();
                SVInt one(l);
                return w <= 1 ? one : one.replicate(SVInt(32, w, false));
            }
            case ExpressionKind::Conversion: {
                const auto& c = e.as<ConversionExpression>();
                auto v = eval(c.operand());
                if (!v || !e.type->isIntegral() || !c.operand().type->isIntegral()) return std::nullopt;
                bitwidth_t w = e.type->getBitWidth();
                SVInt r = w > v->getBitWidth() ? v->extend(w, c.operand().type->isSigned())
                                               : (w < v->getBitWidth() ? v->trunc(w) : *v);
                r.setSigned(e.type->isSigned());
                return r;
            }
            case ExpressionKind::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                auto v = eval(u.operand());
                if (!v) return std::nullopt;
                switch (u.op) {
                    case UnaryOperator::Plus: return v;
                    case UnaryOperator::Minus: return -*v;
                    case UnaryOperator::BitwiseNot: return ~*v;
                    case UnaryOperator::BitwiseAnd: return bit(v->reductionAnd());
                    case UnaryOperator::BitwiseOr: return bit(v->reductionOr());
                    case UnaryOperator::BitwiseXor: return bit(v->reductionXor());
                    case UnaryOperator::BitwiseNand: return bit(!v->reductionAnd());
                    case UnaryOperator::BitwiseNor: return bit(!v->reductionOr());
                    case UnaryOperator::BitwiseXnor: return bit(!v->reductionXor());
                    case UnaryOperator::LogicalNot: return bit(!*v);
                    default: return std::nullopt;
                }
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                auto l = eval(b.left()), r = eval(b.right());
                if (!l || !r) return std::nullopt;
                switch (b.op) {
                    case BinaryOperator::Add: return *l + *r;
                    case BinaryOperator::Subtract: return *l - *r;
                    case BinaryOperator::Multiply: return *l * *r;
                    case BinaryOperator::Divide: return *l / *r;
                    case BinaryOperator::Mod: return *l % *r;
                    case BinaryOperator::BinaryAnd: return *l & *r;
                    case BinaryOperator::BinaryOr: return *l | *r;
                    case BinaryOperator::BinaryXor: return *l ^ *r;
                    case BinaryOperator::BinaryXnor: return l->xnor(*r);
                    case BinaryOperator::Equality: return bit(*l == *r);
                    case BinaryOperator::Inequality: return bit(*l != *r);
                    case BinaryOperator::CaseEquality: return bit(logic_t(exactlyEqual(*l, *r)));
                    case BinaryOperator::CaseInequality: return bit(logic_t(!exactlyEqual(*l, *r)));
                    case BinaryOperator::GreaterThanEqual: return bit(*l >= *r);
                    case BinaryOperator::GreaterThan: return bit(*l > *r);
                    case BinaryOperator::LessThanEqual: return bit(*l <= *r);
                    case BinaryOperator::LessThan: return bit(*l < *r);
                    case BinaryOperator::LogicalAnd: return bit(*l && *r);
                    case BinaryOperator::LogicalOr: return bit(*l || *r);
                    case BinaryOperator::LogicalShiftLeft:
                    case BinaryOperator::ArithmeticShiftLeft: return l->shl(*r);
                    case BinaryOperator::LogicalShiftRight: return l->lshr(*r);
                    case BinaryOperator::ArithmeticShiftRight:
                        return b.left().type->isSigned() ? l->ashr(*r) : l->lshr(*r);
                    default: return std::nullopt;
                }
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1 || c.conditions[0].pattern) return std::nullopt;
                auto p = eval(*c.conditions[0].expr), l = eval(c.left()), r = eval(c.right());
                if (!p || !l || !r) return std::nullopt;
                return SVInt::conditional(*p, *l, *r);
            }
            case ExpressionKind::ElementSelect: {
                const auto& s = e.as<ElementSelectExpression>();
                auto i = eval(s.selector());
                if (!i || i->hasUnknown()) return std::nullopt;
                auto ix = i->as<int32_t>();
                if (!ix) return std::nullopt;
                const Type& vt = *s.value().type;
                if (vt.isIntegral()) {
                    auto v = eval(s.value());
                    if (!v || e.type->getBitWidth() != 1) return std::nullopt;
                    ConstantRange range = vt.getFixedRange();
                    if (!range.containsPoint(*ix)) return bit(logic_t::x);
                    return bit((*v)[range.translateIndex(*ix)]);
                }
                // An unpacked array element is its own waveform: mem[3].
                if (const Symbol* sym = oneName(s.value());
                    sym && s.value().kind == ExpressionKind::NamedValue)
                    return named(path(*sym) + "[" + std::to_string(*ix) + "]", *e.type);
                return std::nullopt;
            }
            case ExpressionKind::RangeSelect: {
                const auto& s = e.as<RangeSelectExpression>();
                const Type& vt = *s.value().type;
                auto v = eval(s.value()), l = eval(s.left()), r = eval(s.right());
                if (!vt.isIntegral() || !v || !l || !r || l->hasUnknown() || r->hasUnknown()) return std::nullopt;
                auto a = l->as<int32_t>(), b = r->as<int32_t>();
                if (!a || !b) return std::nullopt;
                ConstantRange range = vt.getFixedRange();
                int32_t hi = *a, lo = *b;
                if (s.getSelectionKind() == RangeSelectionKind::IndexedUp) {
                    lo = *a; hi = *a + *b - 1;
                    if (!range.isDescending()) std::swap(hi, lo);
                } else if (s.getSelectionKind() == RangeSelectionKind::IndexedDown) {
                    hi = *a; lo = *a - *b + 1;
                    if (!range.isDescending()) std::swap(hi, lo);
                }
                if (!range.containsPoint(hi) || !range.containsPoint(lo)) return std::nullopt;
                return v->slice(range.translateIndex(hi), range.translateIndex(lo));
            }
            case ExpressionKind::Concatenation: {
                std::vector<SVInt> parts;
                for (const Expression* op : e.as<ConcatenationExpression>().operands()) {
                    auto v = eval(*op);
                    if (!v) return std::nullopt;
                    parts.push_back(*v);
                }
                if (parts.empty()) return std::nullopt;
                return SVInt::concat(parts);
            }
            case ExpressionKind::Replication: {
                const auto& r = e.as<ReplicationExpression>();
                auto n = eval(r.count()), v = eval(r.concat());
                if (!n || !v || n->hasUnknown()) return std::nullopt;
                return v->replicate(*n);
            }
            default: return std::nullopt;
        }
    }

    // Does this item of the case match? (12.5: ===, or the casez/casex rule)
    std::optional<bool> itemMatches(const CaseStatement& cs, size_t item, const SVInt& sel) {
        if (cs.condition == CaseStatementCondition::Inside) return std::nullopt;
        for (const Expression* ie : cs.items[item].expressions) {
            auto v = eval(*ie);
            if (!v) return std::nullopt;
            bitwidth_t w = std::max(sel.getBitWidth(), v->getBitWidth());
            SVInt a = sel.getBitWidth() < w ? sel.extend(w, sel.isSigned()) : sel;
            SVInt b = v->getBitWidth() < w ? v->extend(w, v->isSigned()) : *v;
            b.setSigned(a.isSigned());
            bool m = cs.condition == CaseStatementCondition::WildcardJustZ ? caseZWildcardEqual(a, b)
                   : cs.condition == CaseStatementCondition::WildcardXOrZ ? caseXWildcardEqual(a, b)
                   : exactlyEqual(a, b);
            if (m) return true;
        }
        return false;
    }

    std::optional<bool> holds(const Guard& g) {
        switch (g.kind) {
            case Guard::If:
            case Guard::Else: {
                auto v = eval(*g.cond);
                if (!v) return std::nullopt;
                bool taken = exactlyEqual(v->reductionOr(), logic_t(true));   // x takes the else
                return g.kind == Guard::If ? taken : !taken;
            }
            case Guard::CaseItem:
            case Guard::CaseDefault: {
                auto sel = eval(g.cs->expr);
                if (!sel) return std::nullopt;
                size_t upto = g.kind == Guard::CaseItem ? g.item : g.cs->items.size();
                for (size_t i = 0; i < upto; i++) {
                    auto m = itemMatches(*g.cs, i, *sel);
                    if (!m) return std::nullopt;
                    if (*m) return false;   // an earlier item wins
                }
                if (g.kind == Guard::CaseDefault) return true;
                return itemMatches(*g.cs, g.item, *sel);
            }
            default: return std::nullopt;
        }
    }
};

// ----------------------------------------------------------------- queries

struct Tool {
    DB db;
    Vcd vcd;
    bool haveVcd = false;
    unsigned threads = 1;

    std::string guardText(const Guard& g) {
        switch (g.kind) {
            case Guard::If: return db.src.text(g.cond->sourceRange);
            case Guard::Else: return "!(" + db.src.text(g.cond->sourceRange) + ")";
            case Guard::CaseItem: {
                std::string s = db.src.text(g.cs->expr.sourceRange) + " == ";
                bool first = true;
                for (const Expression* e : g.cs->items[g.item].expressions) {
                    s += (first ? "" : ", ") + db.src.text(e->sourceRange);
                    first = false;
                }
                return s;
            }
            case Guard::CaseDefault: return db.src.text(g.cs->expr.sourceRange) + ": default";
            default: return "...";
        }
    }

    std::string when(const Driver& d) {
        std::string s;
        for (const Guard& g : d.guards) s += (s.empty() ? "" : " && ") + guardText(g);
        return s;
    }

    std::string label(const Driver& d) {
        std::string s = d.kind;
        if (!d.clock.empty()) s += " " + d.clock;
        if (!d.how.empty()) s += ", " + d.how;
        return s;
    }

    // The assignment as written; a port connection shows its line.
    std::string text(const Driver& d) {
        if (d.range.start()) {
            std::string t = db.src.text(d.range);
            if (t != "?") return t;
        }
        return db.src.lineText(db.src.where(d.loc));
    }

    // "  dut.sv:12: q <= d;  [always_ff @(posedge clk)]  when en"
    std::string line(const Driver& d) {
        Where w = db.src.where(d.loc);
        std::string s = db.src.at(w) + ": " + text(d) + "  [" + label(d) + "]";
        if (std::string c = when(d); !c.empty()) s += "  when " + c;
        return s;
    }

    const Symbol* find(const std::string& name) {
        auto it = db.byPath.find(name);
        if (it != db.byPath.end()) return it->second;
        // A unique tail is enough: u.q for tb.u.q.
        const Symbol* hit = nullptr;
        int n = 0;
        for (auto& [p, s] : db.byPath)
            if (p.size() > name.size() && p.ends_with(name) && p[p.size() - name.size() - 1] == '.') {
                hit = s;
                n++;
            }
        if (n == 1) return hit;
        std::cerr << "mdebug: " << (n ? "'" + name + "' names " + std::to_string(n) + " signals; give the full path"
                                      : "no signal '" + name + "' in the design")
                  << "\n";
        return nullptr;
    }

    bool drivers(const std::string& name) {
        const Symbol* s = find(name);
        if (!s) return false;
        auto& v = db.driversOf[s];
        std::cout << "drivers of " << path(*s) << ": " << v.size() << "\n";
        for (int i : v) std::cout << "  " << line(db.drivers[i]) << "\n";
        return true;
    }

    bool loads(const std::string& name) {
        const Symbol* s = find(name);
        if (!s) return false;
        auto& v = db.loadsOf[s];
        std::cout << "loads of " << path(*s) << ": " << v.size() << "\n";
        for (int i : v) {
            const Driver& d = db.drivers[i];
            bool data = std::find(d.reads.begin(), d.reads.end(), s) != d.reads.end();
            std::cout << "  " << line(d) << "  -> " << path(*d.target) << (data ? "" : "  (control)") << "\n";
        }
        return true;
    }

    // The fan-in cone, one level per indent.
    void cone(const Symbol* s, int depth, int indent, std::set<const Symbol*>& onPath) {
        std::string pad(indent * 2, ' ');
        std::cout << pad << path(*s);
        auto it = db.driversOf.find(s);
        if (it == db.driversOf.end()) { std::cout << "  (no driver: an input or a testbench value)\n"; return; }
        if (onPath.count(s)) { std::cout << "  (loop)\n"; return; }
        std::cout << "\n";
        if (depth <= 0) return;
        onPath.insert(s);
        for (int i : it->second) {
            const Driver& d = db.drivers[i];
            std::cout << pad << "  <- " << line(d) << "\n";
            std::vector<const Symbol*> in = d.reads;
            for (const Symbol* c : d.creads)
                if (std::find(in.begin(), in.end(), c) == in.end()) in.push_back(c);
            for (const Symbol* r : in)
                if (db.sigIx.count(r) || db.driversOf.count(r)) cone(r, depth - 1, indent + 2, onPath);
        }
        onPath.erase(s);
    }

    // The block view of one scope: its instances, what each port connects
    // to, and which instance feeds which.
    bool blocks(const std::string& name) {
        auto parent = [](const std::string& p) { size_t d = p.rfind('.'); return d == std::string::npos ? std::string() : p.substr(0, d); };
        auto leaf = [](const std::string& p) { size_t d = p.rfind('.'); return d == std::string::npos ? p : p.substr(d + 1); };
        const ScopeRec* sc = nullptr;
        int n = 0;
        for (auto& r : db.scopes) {
            if (r.path == name) { sc = &r; n = 1; break; }
            if (r.path.size() > name.size() && r.path.compare(r.path.size() - name.size() - 1, std::string::npos, "." + name) == 0) { sc = &r; n++; }
        }
        if (n != 1) {
            std::cerr << "mdebug: " << (n ? "more than one instance ends in '" : "no instance '") << name << "'\n";
            return false;
        }
        struct Blk { const ScopeRec* r; std::vector<std::string> in, out; };
        std::vector<Blk> bs;
        std::map<std::string, size_t> ix;
        for (auto& r : db.scopes)
            if (parent(r.path) == sc->path) { ix[r.path] = bs.size(); bs.push_back({&r, {}, {}}); }
        // net in this scope -> the ports that drive it, and the ports it feeds
        std::map<std::string, std::vector<std::string>> from, to;
        for (const Driver& d : db.drivers) {
            if (d.kind != "port" || !db.sigIx.count(d.target)) continue;
            std::string tp = path(*d.target), ts = parent(tp);
            if (auto it = ix.find(ts); it != ix.end()) {
                if (!d.rhs) continue;   // an output of an instance one level further down
                std::string nets;
                for (const Symbol* r : d.reads) {
                    if (!db.sigIx.count(r)) continue;
                    std::string rp = path(*r);
                    nets += (nets.empty() ? "" : ", ") + rp;
                    to[rp].push_back(leaf(ts) + "." + leaf(tp));
                }
                if (nets.empty()) nets = d.rhs ? db.src.text(d.rhs->sourceRange) : "?";
                bs[it->second].in.push_back(leaf(tp) + " <- " + nets);
            }
            else if (ts == sc->path) {
                for (const Symbol* r : d.reads) {
                    if (!db.sigIx.count(r)) continue;
                    std::string rp = path(*r);
                    auto jt = ix.find(parent(rp));
                    if (jt == ix.end()) continue;
                    bs[jt->second].out.push_back(leaf(rp) + " -> " + tp);
                    from[tp].push_back(leaf(parent(rp)) + "." + leaf(rp));
                }
            }
        }
        std::cout << "blocks of " << sc->path << ": " << bs.size() << "\n";
        for (auto& b : bs) {
            std::cout << "  " << leaf(b.r->path) << " (" << b.r->def << ")\n";
            for (auto& l : b.in) std::cout << "    in  " << l << "\n";
            for (auto& l : b.out) std::cout << "    out " << l << "\n";
        }
        for (auto& [net, srcs] : from) {
            auto it = to.find(net);
            if (it == to.end()) continue;
            for (auto& a : srcs)
                for (auto& b : it->second) std::cout << "  " << a << " -> " << b << "  (" << net << ")\n";
        }
        return true;
    }

    bool trace(const std::string& name, int depth) {
        const Symbol* s = find(name);
        if (!s) return false;
        std::set<const Symbol*> onPath;
        cone(s, depth, 0, onPath);
        return true;
    }

    std::string valueAt(const Symbol* s, uint64_t t) {
        const Wave* w = vcd.find(path(*s));
        if (!w) return "?";
        int i = w->at(t);
        return i < 0 ? "x" : showBits(w->bits(i));
    }

    // The assignment that gave s its value at time t, then what caused it.
    void active(const Symbol* s, uint64_t t, int depth, int indent, std::set<std::pair<const Symbol*, uint64_t>>& seen) {
        std::string pad(indent * 2, ' ');
        const Wave* w = vcd.find(path(*s));
        if (!w) { std::cout << pad << path(*s) << "  (not in the waveform)\n"; return; }
        int ci = w->at(t);
        uint64_t tc = ci < 0 ? 0 : w->ch[ci].t;
        std::cout << pad << path(*s) << " = " << (ci < 0 ? "x" : showBits(w->bits(ci))) << " @" << t;
        if (tc != t) std::cout << "  (since " << tc << ")";
        std::cout << "\n";
        auto it = db.driversOf.find(s);
        if (it == db.driversOf.end()) { std::cout << pad << "  no driver: an input or a testbench value\n"; return; }
        if (!seen.insert({s, tc}).second) { std::cout << pad << "  (shown above)\n"; return; }

        // Which drivers ran? A flop's conditions are sampled before the edge.
        struct Hit { int ix; int state; uint64_t ts; };   // state: 1 yes, 0 no, -1 unknown
        std::vector<Hit> hits;
        for (int i : it->second) {
            const Driver& d = db.drivers[i];
            uint64_t ts = d.seq && tc > 0 ? tc - 1 : tc;
            Eval ev{vcd, ts};
            int state = 1;
            for (const Guard& g : d.guards) {
                auto h = ev.holds(g);
                if (!h) { if (state == 1) state = -1; }
                else if (!*h) { state = 0; break; }
            }
            hits.push_back({i, state, ts});
        }
        // An assignment whose right side does not give the value seen is not
        // the one (two assignments in one initial block, at different times).
        if (ci >= 0) {
            std::vector<bool> differs(hits.size(), false);
            bool match = false;
            for (size_t k = 0; k < hits.size(); k++) {
                const Driver& d = db.drivers[hits[k].ix];
                if (hits[k].state == 0 || !d.whole || !d.rhs) continue;
                Eval ev{vcd, hits[k].ts};
                auto v = ev.eval(*d.rhs);
                if (!v) continue;
                const std::string bits = w->bits(ci);
                SVInt seen = SVInt::fromString(std::to_string(bits.size()) + "'b" + bits);
                SVInt got = v->getBitWidth() < seen.getBitWidth() ? v->extend(seen.getBitWidth(), v->isSigned())
                          : v->getBitWidth() > seen.getBitWidth() ? v->trunc(seen.getBitWidth()) : *v;
                got.setSigned(false);
                if (exactlyEqual(got, seen)) match = true;
                else differs[k] = true;
            }
            if (match)
                for (size_t k = 0; k < hits.size(); k++)
                    if (differs[k]) hits[k].state = 0;
        }
        // In one process the last assignment that ran is the one that counts.
        std::map<int, int> last;
        for (size_t k = 0; k < hits.size(); k++)
            if (hits[k].state == 1 && db.drivers[hits[k].ix].block >= 0) last[db.drivers[hits[k].ix].block] = (int)k;
        bool any = false;
        for (size_t k = 0; k < hits.size(); k++) {
            const Driver& d = db.drivers[hits[k].ix];
            if (hits[k].state == 0) continue;
            if (hits[k].state == 1 && d.block >= 0 && last[d.block] != (int)k) continue;
            any = true;
            std::cout << pad << "  " << (hits[k].state == 1 ? "<- " : "?? ") << line(d) << "\n";
            if (depth <= 0) continue;
            uint64_t ts = hits[k].ts;
            // Conditions: their values at the sample time.
            std::string ctl;
            for (const Symbol* c : d.creads)
                if (db.sigIx.count(c)) ctl += (ctl.empty() ? "" : ", ") + std::string(c->name) + "=" + valueAt(c, ts);
            if (!ctl.empty()) std::cout << pad << "     with " << ctl << " @" << ts << "\n";
            // Combinational: a condition that moved at that time is a cause too.
            std::vector<const Symbol*> follow = d.reads;
            if (!d.seq)
                for (const Symbol* c : d.creads) {
                    const Wave* cw = db.sigIx.count(c) ? vcd.find(path(*c)) : nullptr;
                    int k2 = cw ? cw->at(tc) : -1;
                    if (k2 >= 0 && cw->ch[k2].t == tc && tc > 0 &&
                        std::find(follow.begin(), follow.end(), c) == follow.end())
                        follow.push_back(c);
                }
            for (const Symbol* r : follow) {
                if (!db.sigIx.count(r)) continue;
                const Wave* rw = vcd.find(path(*r));
                // Combinational: follow the input that moved at the same time.
                // With several inputs, the quiet ones are listed, not followed.
                if (!d.seq && rw && follow.size() > 1) {
                    int ri = rw->at(tc);
                    if (ri < 0 || rw->ch[ri].t != tc) {
                        std::cout << pad << "    " << path(*r) << " = " << valueAt(r, tc) << "  (unchanged @" << tc << ")\n";
                        continue;
                    }
                }
                active(r, ts, depth - 1, indent + 2, seen);
            }
        }
        if (!any) std::cout << pad << "  no assignment's conditions hold at " << tc << " (" << it->second.size() << " drivers)\n";
    }

    bool activeTrace(const std::string& name, std::optional<uint64_t> at, int depth) {
        if (!haveVcd) { std::cerr << "mdebug: -active needs -vcd <file>\n"; return false; }
        const Symbol* s = find(name);
        if (!s) return false;
        std::set<std::pair<const Symbol*, uint64_t>> seen;
        active(s, at ? *at : vcd.end, depth, 0, seen);
        return true;
    }

    // ------------------------------------------------------------ database

    static std::string js(std::string_view s) {
        std::string o = "\"";
        for (unsigned char c : s) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': break;
                case '\t': o += "\\t"; break;
                case '<': o += "\\u003c"; break;   // safe inside <script>
                default:
                    if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                    else o += (char)c;
            }
        }
        return o + "\"";
    }

    static constexpr size_t kPageChanges = 2000000;

    // An expression for the viewer's own evaluator (the active-driver trace
    // in the page): nested arrays, "null" for what it does not follow.
    static std::string bitsOf(const SVInt& v) {
        std::string b;
        for (int i = (int)v.getBitWidth() - 1; i >= 0; i--) {
            logic_t l = v[i];
            b += l.isUnknown() ? (exactlyEqual(l, logic_t::z) ? 'z' : 'x') : (bool(l) ? '1' : '0');
        }
        return b;
    }
    static std::string constJson(const SVInt& v) {
        if (v.getBitWidth() > 4096) return "null";
        return "[\"c\",\"" + bitsOf(v) + "\"," + (v.isSigned() ? "1" : "0") + "]";
    }
    std::string symJson(const Symbol& sym, const Type& type) {
        if (sym.kind == SymbolKind::Parameter) {
            const ConstantValue& cv = sym.as<ParameterSymbol>().getValue();
            return cv.isInteger() ? constJson(cv.integer()) : "null";
        }
        if (sym.kind == SymbolKind::EnumValue) {
            const ConstantValue& cv = sym.as<EnumValueSymbol>().getValue();
            return cv.isInteger() ? constJson(cv.integer()) : "null";
        }
        auto it = db.sigIx.find(&sym);
        if (it == db.sigIx.end() || !type.isIntegral()) return "null";
        return "[\"s\"," + std::to_string(it->second) + "," + (type.isSigned() ? "1" : "0") + "]";
    }
    std::string exprJson(const Expression& e) {
        auto bad = [](const std::string& x) { return x == "null"; };
        switch (e.kind) {
            case ExpressionKind::NamedValue: return symJson(e.as<NamedValueExpression>().symbol, *e.type);
            case ExpressionKind::HierarchicalValue: return symJson(e.as<HierarchicalValueExpression>().symbol, *e.type);
            case ExpressionKind::IntegerLiteral: return constJson(e.as<IntegerLiteral>().getValue());
            case ExpressionKind::UnbasedUnsizedIntegerLiteral: {
                if (!e.type->isIntegral() || e.type->getBitWidth() > 4096) return "null";
                logic_t l = e.as<UnbasedUnsizedIntegerLiteral>().getLiteralValue();
                char c = l.isUnknown() ? (exactlyEqual(l, logic_t::z) ? 'z' : 'x') : (bool(l) ? '1' : '0');
                return "[\"c\",\"" + std::string(e.type->getBitWidth(), c) + "\",0]";
            }
            case ExpressionKind::Conversion: {
                const auto& c = e.as<ConversionExpression>();
                std::string v = exprJson(c.operand());
                if (bad(v) || !e.type->isIntegral() || !c.operand().type->isIntegral()) return "null";
                return "[\"cv\"," + v + "," + std::to_string(e.type->getBitWidth()) + "," +
                       (c.operand().type->isSigned() ? "1" : "0") + "," + (e.type->isSigned() ? "1" : "0") + "]";
            }
            case ExpressionKind::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                std::string v = exprJson(u.operand());
                if (bad(v)) return "null";
                return "[\"u\",\"" + std::string(toString(u.op)) + "\"," + v + "]";
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                std::string l = exprJson(b.left()), r = exprJson(b.right());
                if (bad(l) || bad(r)) return "null";
                return "[\"b\",\"" + std::string(toString(b.op)) + "\"," + l + "," + r + "]";
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1 || c.conditions[0].pattern) return "null";
                std::string p = exprJson(*c.conditions[0].expr), l = exprJson(c.left()), r = exprJson(c.right());
                if (bad(p) || bad(l) || bad(r)) return "null";
                return "[\"?:\"," + p + "," + l + "," + r + "]";
            }
            case ExpressionKind::ElementSelect: {
                const auto& x = e.as<ElementSelectExpression>();
                const Type& vt = *x.value().type;
                if (!vt.isIntegral() || e.type->getBitWidth() != 1) return "null";
                std::string v = exprJson(x.value()), i = exprJson(x.selector());
                if (bad(v) || bad(i)) return "null";
                ConstantRange r = vt.getFixedRange();
                return "[\"bit\"," + v + "," + i + "," + std::to_string(r.left) + "," + std::to_string(r.right) + "]";
            }
            case ExpressionKind::RangeSelect: {
                const auto& x = e.as<RangeSelectExpression>();
                const Type& vt = *x.value().type;
                if (!vt.isIntegral()) return "null";
                std::string v = exprJson(x.value()), l = exprJson(x.left()), r = exprJson(x.right());
                if (bad(v) || bad(l) || bad(r)) return "null";
                ConstantRange rg = vt.getFixedRange();
                int kind = x.getSelectionKind() == RangeSelectionKind::Simple ? 0
                         : x.getSelectionKind() == RangeSelectionKind::IndexedUp ? 1 : 2;
                return "[\"rng\"," + v + "," + l + "," + r + "," + std::to_string(kind) + "," +
                       std::to_string(rg.left) + "," + std::to_string(rg.right) + "]";
            }
            case ExpressionKind::Concatenation: {
                std::string o = "[\"cat\",[";
                bool first = true;
                for (const Expression* op : e.as<ConcatenationExpression>().operands()) {
                    std::string v = exprJson(*op);
                    if (bad(v)) return "null";
                    o += (first ? "" : ",") + v;
                    first = false;
                }
                return first ? "null" : o + "]]";
            }
            case ExpressionKind::Replication: {
                const auto& r = e.as<ReplicationExpression>();
                std::string n = exprJson(r.count()), v = exprJson(r.concat());
                if (bad(n) || bad(v)) return "null";
                return "[\"rep\"," + n + "," + v + "]";
            }
            default: return "null";
        }
    }
    // Case statements are shared by the drivers under them: one table.
    std::map<const CaseStatement*, int> caseIx;
    std::vector<const CaseStatement*> cases;
    std::string guardsJson(const Driver& d) {
        std::string o = "[";
        for (const Guard& g : d.guards) {
            if (o.size() > 1) o += ",";
            switch (g.kind) {
                case Guard::If: o += "[\"if\"," + exprJson(*g.cond) + "]"; break;
                case Guard::Else: o += "[\"else\"," + exprJson(*g.cond) + "]"; break;
                case Guard::CaseItem:
                case Guard::CaseDefault: {
                    auto it = caseIx.find(g.cs);
                    if (it == caseIx.end()) {
                        it = caseIx.emplace(g.cs, (int)cases.size()).first;
                        cases.push_back(g.cs);
                    }
                    o += "[\"case\"," + std::to_string(it->second) + "," +
                         (g.kind == Guard::CaseDefault ? "-1" : std::to_string(g.item)) + "]";
                    break;
                }
                default: o += "[\"?\"]"; break;
            }
        }
        return o + "]";
    }
    std::string casesJson() {
        std::string o = "[";
        for (size_t i = 0; i < cases.size(); i++) {
            const CaseStatement& c = *cases[i];
            const char* k = c.condition == CaseStatementCondition::Normal ? "n"
                          : c.condition == CaseStatementCondition::WildcardJustZ ? "z"
                          : c.condition == CaseStatementCondition::WildcardXOrZ ? "x" : "?";
            o += std::string(i ? ",\n" : "") + "{\"k\":\"" + k + "\",\"sel\":" + exprJson(c.expr) + ",\"items\":[";
            for (size_t j = 0; j < c.items.size(); j++) {
                o += j ? ",[" : "[";
                bool first = true;
                for (const Expression* e : c.items[j].expressions) {
                    o += (first ? "" : ",") + exprJson(*e);
                    first = false;
                }
                o += "]";
            }
            o += "]}";
        }
        return o + "]";
    }

    std::optional<uint64_t> center;   // -at T

    void json(std::ostream& os, size_t maxChanges) {
        // Resolve every place first, so the file table is complete.
        std::vector<Where> sw, sd, si, dw;
        for (auto* s : db.signals) sw.push_back(db.src.where(s->location));
        for (auto& s : db.scopes) { sd.push_back(db.src.where(s.loc)); si.push_back(db.src.where(s.inst)); }
        for (auto& d : db.drivers) dw.push_back(db.src.where(d.loc));
        os << "{\"version\":" << js(MINISIM_VERSION) << ",\n\"files\":[";
        for (size_t i = 0; i < db.src.files.size(); i++) {
            auto& f = db.src.files[i];
            os << (i ? ",\n" : "") << "{\"name\":" << js(f.name) << ",\"text\":"
               << js(db.src.sm->getSourceText(f.buf)) << "}";
        }
        os << "],\n\"scopes\":[";
        for (size_t i = 0; i < db.scopes.size(); i++)
            os << (i ? ",\n" : "") << "{\"path\":" << js(db.scopes[i].path) << ",\"def\":" << js(db.scopes[i].def)
               << ",\"file\":" << sd[i].file << ",\"line\":" << sd[i].line << ",\"ifile\":" << si[i].file
               << ",\"iline\":" << si[i].line << "}";
        os << "],\n\"signals\":[";
        for (size_t i = 0; i < db.signals.size(); i++) {
            const ValueSymbol* s = db.signals[i];
            const Type& ty = s->getType();
            os << (i ? ",\n" : "") << "{\"path\":" << js(path(*s)) << ",\"width\":"
               << (ty.isIntegral() ? (int)ty.getBitWidth() : 0) << ",\"type\":" << js(ty.toString())
               << ",\"file\":" << sw[i].file << ",\"line\":" << sw[i].line << "}";
        }
        os << "],\n\"drivers\":[";
        auto ids = [&](const std::vector<const Symbol*>& v) {
            std::string o = "[";
            for (const Symbol* s : v)
                if (auto it = db.sigIx.find(s); it != db.sigIx.end())
                    o += (o.size() > 1 ? "," : "") + std::to_string(it->second);
            return o + "]";
        };
        bool first = true;
        for (size_t i = 0; i < db.drivers.size(); i++) {
            const Driver& d = db.drivers[i];
            auto it = db.sigIx.find(d.target);
            if (it == db.sigIx.end()) continue;
            os << (first ? "" : ",\n") << "{\"sig\":" << it->second << ",\"kind\":" << js(label(d))
               << ",\"file\":" << dw[i].file << ",\"line\":" << dw[i].line << ",\"text\":" << js(text(d))
               << ",\"when\":" << js(when(d)) << ",\"seq\":" << (d.seq ? 1 : 0) << ",\"reads\":" << ids(d.reads)
               << ",\"ctl\":" << ids(d.creads) << ",\"blk\":" << d.block << ",\"whole\":" << (d.whole ? 1 : 0)
               << ",\"g\":" << guardsJson(d) << ",\"rhs\":" << (d.rhs && d.whole ? exprJson(*d.rhs) : "null") << "}";
            first = false;
        }
        os << "],\n\"cases\":" << casesJson();
        if (haveVcd) {
            os << ",\n\"end\":" << vcd.end << ",\"at\":" << (center ? std::min(*center, vcd.end) : vcd.end) << ",\"timescale\":" << js(vcd.timescale) << ",\"waves\":{";
            first = true;
            std::vector<std::string> part(db.signals.size());
            std::vector<const Wave*> wv(db.signals.size());
            for (size_t i = 0; i < db.signals.size(); i++) wv[i] = vcd.find(path(*db.signals[i]));
            // A page a browser can open: a budget of changes over all signals.
            size_t waved = 0, cutSigs = 0;
            for (const Wave* w : wv) waved += w != nullptr;
            maxChanges = std::min(maxChanges, std::max<size_t>(64, kPageChanges / std::max<size_t>(waved, 1)));
            for (const Wave* w : wv) cutSigs += w && w->ch.size() > maxChanges;
            if (cutSigs)
                std::cerr << "mdebug: the waves of " << cutSigs << " signals are cut to " << maxChanges << " changes each in the page, "
                          << (center ? "around -at" : "from the start; -at T keeps the changes around T")
                          << " (-max-changes N for more per signal)\n";
            parallelFor(db.signals.size(), threads, [&](size_t i) {
                const Wave* w = wv[i];
                if (!w) return;
                std::string& o = part[i];
                o = "\"" + std::to_string(i) + "\":[";
                // Cut waves keep the window around -at T (else the start).
                size_t n = std::min(w->ch.size(), maxChanges), k0 = 0;
                if (center && n < w->ch.size()) {
                    int c = w->at(*center);
                    k0 = (size_t)std::max(0, c - (int)(n / 2));
                    k0 = std::min(k0, w->ch.size() - n);
                }
                for (size_t k = k0; k < k0 + n; k++) {
                    if (k > k0) o += ',';
                    o += '[';
                    o += std::to_string(w->ch[k].t);
                    o += ",\"";
                    o += w->bits((int)k);
                    o += "\"]";
                }
                o += ']';
            });
            for (size_t i = 0; i < part.size(); i++) {
                if (!wv[i]) continue;
                os << (first ? "" : ",\n") << part[i];
                first = false;
            }
            os << "}";
        }
        os << "}\n";
    }
};

void usage() {
    std::cout << "usage: mdebug [options] file.sv ... <queries>\n"
                 "  source-level debug on the msim front end: drivers, loads, tracing,\n"
                 "  and the active driver at a time in a waveform\n\n"
                 "  -drivers SIG     every assignment that can give SIG a value\n"
                 "  -loads SIG       every assignment that reads SIG\n"
                 "  -trace SIG       the fan-in cone of SIG (-depth N levels, default 2)\n"
                 "  -active SIG      with -vcd: the assignment that gave SIG its value at\n"
                 "                   -at T (default: the end), then what caused it,\n"
                 "                   -depth N steps back through time\n"
                 "  -vcd <file>      the waveform msim wrote: VCD or FST (-fst, -wave are the same)\n"
                 "  -at T            a time, in the VCD's time unit\n"
                 "  -threads N       threads for parsing and for reading the waveform\n"
                 "                   (default: every logical core; the output is the same for any N)\n"
                 "  -scopes          list the instances    -signals  list the signals\n"
                 "  -blocks INST     the instances inside INST, their port connections and what feeds what\n"
                 "  -html <file>     write a one-file viewer: hierarchy, source, drivers and\n"
                 "                   loads, a schematic of the cone, and waves when -vcd is given\n"
                 "  -json <file>     write the debug database the viewer reads\n"
                 "  -top M           the top module    -f <file>  an argument file\n"
                 "  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
                 "  -version         print the version\n"
                 "  --help-slang     list every front-end option\n\n"
                 "  SIG is a hierarchical path (tb.u.q); a unique tail (u.q) is enough.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> fwd{argv[0]};
    struct Query { std::string what, arg; int depth; std::optional<uint64_t> at; };
    std::vector<Query> queries;
    std::string vcdPath, htmlPath, jsonPath;
    int depth = -1;
    std::optional<uint64_t> at;
    size_t maxChanges = 20000;
    // Every logical core by default (hyper-threads included); -threads 1 for one.
    unsigned threads = par::defaultThreads();
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mdebug: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-version" || a == "--version" || a == "-v") {
            std::cout << "mdebug " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-blocks") { queries.push_back({"blocks", next("an instance"), -1, std::nullopt}); continue; }
        if (a == "-drivers" || a == "-loads" || a == "-trace" || a == "-active") {
            queries.push_back({a.substr(1), next("a signal"), -1, std::nullopt});
            continue;
        }
        if (a == "-scopes" || a == "-signals") { queries.push_back({a.substr(1), "", -1, std::nullopt}); continue; }
        if (a == "-depth") { depth = std::atoi(next("a number").c_str()); continue; }
        if (a == "-at") { at = std::strtoull(next("a time").c_str(), nullptr, 10); continue; }
        if (a == "-vcd" || a == "-fst" || a == "-wave") { vcdPath = next("a VCD file"); continue; }
        if (a == "-html") { htmlPath = next("a file"); continue; }
        if (a == "-json") { jsonPath = next("a file"); continue; }
        if (par::threadsArg(argc, argv, i, threads, "mdebug")) continue;
        if (a == "-max-changes") { maxChanges = std::strtoull(next("a number").c_str(), nullptr, 10); continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        fwd.push_back(a);
    }
    if (queries.empty() && htmlPath.empty() && jsonPath.empty() && fwd.size() == 1) { usage(); return 2; }
    // Every instance gets its own symbols, so tb.a.q and tb.b.q are two signals.
    fwd.push_back("--disable-instance-caching");
    fwd.push_back("--threads");   // the front end parses files in parallel
    fwd.push_back(std::to_string(threads));

    FrontEnd fe;
    if (int rc = fe.run(fwd, "mdebug front end")) return rc < 0 ? 0 : rc;

    Tool tool;
    tool.threads = threads;
    tool.center = at;
    tool.db.src.sm = fe.comp->getSourceManager();
    Collect c(tool.db);
    fe.comp->getRoot().visit(c);

    if (!vcdPath.empty()) {
        std::string err;
        if (!tool.vcd.open(vcdPath, threads, err)) { std::cerr << "mdebug: " << err << "\n"; return 2; }
        tool.haveVcd = true;
    }

    bool ok = true;
    for (const Query& q : queries) {
        if (q.what == "drivers") ok &= tool.drivers(q.arg);
        else if (q.what == "loads") ok &= tool.loads(q.arg);
        else if (q.what == "blocks") ok &= tool.blocks(q.arg);
        else if (q.what == "trace") ok &= tool.trace(q.arg, depth < 0 ? 2 : depth);
        else if (q.what == "active") ok &= tool.activeTrace(q.arg, at, depth < 0 ? 3 : depth);
        else if (q.what == "scopes")
            for (auto& s : tool.db.scopes)
                std::cout << s.path << "  (" << s.def << ")  " << tool.db.src.at(tool.db.src.where(s.loc)) << "\n";
        else if (q.what == "signals")
            for (auto* s : tool.db.signals)
                std::cout << path(*s) << "  " << s->getType().toString() << "  "
                          << tool.db.src.at(tool.db.src.where(s->location)) << "\n";
    }

    if (!jsonPath.empty()) {
        std::ofstream os(jsonPath);
        if (!os) { std::cerr << "mdebug: cannot write " << jsonPath << "\n"; return 2; }
        tool.json(os, maxChanges);
    }
    if (!htmlPath.empty()) {
        std::ofstream os(htmlPath);
        if (!os) { std::cerr << "mdebug: cannot write " << htmlPath << "\n"; return 2; }
        std::ostringstream data;
        tool.json(data, maxChanges);
        std::string page = kViewer;
        const std::string mark = "/*MDEBUG_DATA*/null";
        size_t p = page.find(mark);
        os << page.substr(0, p) << data.str() << page.substr(p + mark.size());
        std::cout << "mdebug: wrote " << htmlPath << " (" << tool.db.scopes.size() << " instances, "
                  << tool.db.signals.size() << " signals, " << tool.db.drivers.size() << " drivers)\n";
    }
    return ok ? 0 : 1;
}
