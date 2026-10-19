// lower.hpp - lower slang's elaborated AST into Sim signals + Prog processes.
// Written against slang v7-ish API from memory; NOT compiled (slang unavailable
// in the authoring environment). Expect small name/signature fixes.
//
// Supported subset (single module, flat):
//   statements : begin/end, if/else, case/casez/casex, blocking &
//                non-blocking assign to a variable, a[const], a[hi:lo],
//                @(posedge/negedge/any sig [or ...]), @*, always_comb,
//                for/while/do/forever/repeat(const), #<literal>, $finish, assign
//   expressions: literals, variable refs, unary/binary ops, ?:, {concat},
//                a[expr], a[hi:lo]
// Assumes vectors are declared [N-1:0] (zero-based) so index == bit position.
// Not yet:  compound assigns (+=), break/continue, net initializers (wire y = ..), a[i +: w], tasks/functions,
//           hierarchy, $display, signed arithmetic.
//
// Usage:
//   LowerResult r = lower(compilation,
//                         [&](const std::string& n, int w){ return S.sig(n, w); },
//                         [&](int sig){ return S.newDriver(sig); });
//   for (auto& pr : r.procs) S.spawn(exec(&pr.prog));
#pragma once
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "prog.hpp"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/MiscStatements.h"

struct LoweredProc { Prog prog; bool loops; std::string kind; };
struct LowerResult { std::vector<LoweredProc> procs; };

struct Lowerer {
    std::unordered_map<const slang::ast::Symbol*, int>& sigmap;
    Prog prog;
    std::vector<size_t> implicitWaits;  // WaitEdge slots for @*, filled by finishImplicit()

    explicit Lowerer(std::unordered_map<const slang::ast::Symbol*, int>& m) : sigmap(m) {}

    [[noreturn]] static void unsupported(const char* what) {
        throw std::runtime_error(std::string("lower: unsupported ") + what);
    }

    // Constant integer from a literal or a slang-folded constant expression.
    static int64_t constInt(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (e.kind == ExpressionKind::IntegerLiteral)
            return *e.as<IntegerLiteral>().getValue().as<int64_t>();
        if (e.kind == ExpressionKind::Conversion)
            return constInt(e.as<ConversionExpression>().operand());
        if (e.constant) {
            auto v = e.constant->integer().as<int64_t>();
            if (v) return *v;
        }
        unsupported("non-constant select bound");
    }

    // ---------- expressions ----------
    E expr(const slang::ast::Expression& e) {
        using namespace slang::ast;
        switch (e.kind) {
            case ExpressionKind::IntegerLiteral: {
                const auto& sv = e.as<IntegerLiteral>().getValue();
                int w = (int)sv.getBitWidth();
                if (sv.hasUnknown()) {   // x / z / ? bits (VERIFY logic_t API for your slang version)
                    uint64_t v = 0, x = 0, z = 0;
                    for (int b = 0; b < w && b < 64; b++) {
                        auto bit = sv[b];
                        if (bit.isUnknown()) {
                            x |= 1ULL << b;
                            if (bit.value == slang::logic_t::Z_VALUE) z |= 1ULL << b;
                        } else if (bit.value) {
                            v |= 1ULL << b;
                        }
                    }
                    return konst(make(v, x, w, z));
                }
                return konst(mk(*sv.as<uint64_t>(), w));
            }
            case ExpressionKind::NamedValue: {
                const Symbol* sym = &e.as<NamedValueExpression>().symbol;
                auto it = sigmap.find(sym);
                if (it == sigmap.end()) unsupported("reference to non-signal symbol");
                return ref(it->second);
            }
            case ExpressionKind::Conversion:  // implicit width/sign conversion; set() masks
                return expr(e.as<ConversionExpression>().operand());
            case ExpressionKind::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                E a = expr(u.operand());
                switch (u.op) {
                    case UnaryOperator::BitwiseNot: return un(Op::Not, a);
                    case UnaryOperator::LogicalNot: return un(Op::LogNot, a);
                    case UnaryOperator::BitwiseAnd: return un(Op::RedAnd, a);
                    case UnaryOperator::BitwiseOr: return un(Op::RedOr, a);
                    case UnaryOperator::BitwiseXor: return un(Op::RedXor, a);
                    default: unsupported("unary operator");
                }
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                E l = expr(b.left()), r = expr(b.right());
                switch (b.op) {
                    case BinaryOperator::Add: return bin(Op::Add, l, r);
                    case BinaryOperator::Subtract: return bin(Op::Sub, l, r);
                    case BinaryOperator::Multiply: return bin(Op::Mul, l, r);
                    case BinaryOperator::BinaryAnd: return bin(Op::And, l, r);
                    case BinaryOperator::BinaryOr: return bin(Op::Or, l, r);
                    case BinaryOperator::BinaryXor: return bin(Op::Xor, l, r);
                    case BinaryOperator::Equality: return bin(Op::Eq, l, r);
                    case BinaryOperator::Inequality: return bin(Op::Ne, l, r);
                    case BinaryOperator::CaseEquality: return bin(Op::CaseEq, l, r);
                    case BinaryOperator::LessThan: return bin(Op::Lt, l, r);
                    case BinaryOperator::LessThanEqual: return bin(Op::Le, l, r);
                    case BinaryOperator::GreaterThan: return bin(Op::Gt, l, r);
                    case BinaryOperator::GreaterThanEqual: return bin(Op::Ge, l, r);
                    case BinaryOperator::LogicalShiftLeft: return bin(Op::Shl, l, r);
                    case BinaryOperator::LogicalShiftRight: return bin(Op::Shr, l, r);
                    case BinaryOperator::LogicalAnd: return bin(Op::LogAnd, l, r);
                    case BinaryOperator::LogicalOr: return bin(Op::LogOr, l, r);
                    default: unsupported("binary operator");
                }
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1) unsupported("pattern conditional");
                return mux(expr(*c.conditions[0].expr), expr(c.left()), expr(c.right()));
            }
            case ExpressionKind::Concatenation: {
                std::vector<E> parts;
                for (auto* op : e.as<ConcatenationExpression>().operands()) parts.push_back(expr(*op));
                return concat(parts);
            }
            case ExpressionKind::ElementSelect: {   // a[i]
                const auto& sel = e.as<ElementSelectExpression>();
                E base = expr(sel.value());
                const auto& idx = sel.selector();
                if (idx.kind == ExpressionKind::IntegerLiteral || idx.constant) {
                    int i = (int)constInt(idx);
                    return slice(base, i, i);
                }
                return bin(Op::Index, base, expr(idx));
            }
            case ExpressionKind::RangeSelect: {     // a[hi:lo]
                const auto& r = e.as<RangeSelectExpression>();
                if (r.getSelectionKind() != RangeSelectionKind::Simple) unsupported("indexed part-select (+: / -:)");
                return slice(expr(r.value()), (int)constInt(r.left()), (int)constInt(r.right()));
            }
            default: unsupported("expression kind");
        }
    }

    // Fill i.sig (+ i.hi/i.lo for partial writes) from an assignment target.
    void lhs(const slang::ast::Expression& e, Instr& i) {
        using namespace slang::ast;
        switch (e.kind) {
            case ExpressionKind::NamedValue:
                i.sig = sigmap.at(&e.as<NamedValueExpression>().symbol);
                return;
            case ExpressionKind::ElementSelect: {
                const auto& sel = e.as<ElementSelectExpression>();
                if (sel.value().kind != ExpressionKind::NamedValue) unsupported("nested select target");
                lhs(sel.value(), i);
                i.hi = i.lo = (int)constInt(sel.selector());   // constant index only
                return;
            }
            case ExpressionKind::RangeSelect: {
                const auto& r = e.as<RangeSelectExpression>();
                if (r.getSelectionKind() != RangeSelectionKind::Simple) unsupported("indexed part-select target");
                if (r.value().kind != ExpressionKind::NamedValue) unsupported("nested select target");
                lhs(r.value(), i);
                i.hi = (int)constInt(r.left());
                i.lo = (int)constInt(r.right());
                return;
            }
            case ExpressionKind::Conversion:
                lhs(e.as<ConversionExpression>().operand(), i);
                return;
            default: unsupported("assignment target");
        }
    }

    // ---------- statements ----------
    // Collect (signal, edge) pairs from a SignalEvent or EventList control.
    // BothEdges is approximated as ANY. (@* is handled in timing()/finishImplicit().)
    void collectEvents(const slang::ast::TimingControl& t, std::vector<std::pair<int, Edge>>& ev) {
        using namespace slang::ast;
        if (t.kind == TimingControlKind::SignalEvent) {
            const auto& s = t.as<SignalEventControl>();
            if (s.expr.kind != ExpressionKind::NamedValue) unsupported("event on non-signal");
            int sig = sigmap.at(&s.expr.as<NamedValueExpression>().symbol);
            Edge e = s.edge == EdgeKind::PosEdge ? POS : s.edge == EdgeKind::NegEdge ? NEG : ANY;
            ev.push_back({sig, e});
        } else if (t.kind == TimingControlKind::EventList) {
            for (auto* sub : t.as<EventListControl>().events) collectEvents(*sub, ev);
        } else {
            unsupported("timing control");
        }
    }

    void timing(const slang::ast::TimingControl& t) {
        using namespace slang::ast;
        switch (t.kind) {
            case TimingControlKind::Delay: {
                const auto& d = t.as<DelayControl>();
                if (d.expr.kind != ExpressionKind::IntegerLiteral) unsupported("non-literal delay");
                Instr i; i.k = IK::WaitDelay;
                i.delay = *d.expr.as<IntegerLiteral>().getValue().as<uint64_t>();
                prog.emit(i);
                break;
            }
            case TimingControlKind::ImplicitEvent: {   // @*  -> patched once the whole block is lowered
                Instr i; i.k = IK::WaitEdge;
                implicitWaits.push_back(prog.emit(i));
                break;
            }
            default: {  // SignalEvent or EventList: @(posedge a or negedge b ...)
                Instr i; i.k = IK::WaitEdge;
                collectEvents(t, i.events);
                i.sig = i.events[0].first;
                i.edge = i.events[0].second;
                prog.emit(i);
            }
        }
    }

    // Assignments, ++/--, and system calls used as statements.
    void exprStmt(const slang::ast::Expression& ex) {
        using namespace slang::ast;
        if (ex.kind == ExpressionKind::Assignment) {
            const auto& a = ex.as<AssignmentExpression>();
            if (a.isCompound()) unsupported("compound assignment (+=, -=, ...)");
            Instr i;
            i.k = a.isNonBlocking() ? IK::Nba : IK::Assign;
            lhs(a.left(), i);
            i.e = expr(a.right());
            prog.emit(i);
        } else if (ex.kind == ExpressionKind::UnaryOp) {   // i++ / ++i / i-- / --i
            const auto& u = ex.as<UnaryExpression>();
            bool inc = u.op == UnaryOperator::Preincrement || u.op == UnaryOperator::Postincrement;
            bool dec = u.op == UnaryOperator::Predecrement || u.op == UnaryOperator::Postdecrement;
            if (!inc && !dec) unsupported("unary operator as statement");
            Instr i; i.k = IK::Assign;
            lhs(u.operand(), i);
            i.e = bin(inc ? Op::Add : Op::Sub, expr(u.operand()), konst(mk(1, 32)));
            prog.emit(i);
        } else if (ex.kind == ExpressionKind::Call) {
            std::string_view n = ex.as<CallExpression>().getSubroutineName();
            if (n == "$finish" || n == "$stop") { Instr i; i.k = IK::Finish; prog.emit(i); }
            else std::cerr << "lower: ignoring system call " << n << "\n";
        } else {
            unsupported("expression statement");
        }
    }

    // @* waits on every signal the block reads (known only after lowering).
    void finishImplicit() {
        if (implicitWaits.empty()) return;
        auto rs = readSet(prog);
        if (rs.empty()) unsupported("@* in a block that reads no signals");
        for (size_t pc : implicitWaits) prog.code[pc] = waitInstr(rs);
    }

    void stmt(const slang::ast::Statement& s) {
        using namespace slang::ast;
        switch (s.kind) {
            case StatementKind::Empty: break;
            case StatementKind::List:
                for (auto* st : s.as<StatementList>().list) stmt(*st);
                break;
            case StatementKind::Block:
                stmt(s.as<BlockStatement>().getStatements());
                break;
            case StatementKind::Timed: {
                const auto& t = s.as<TimedStatement>();
                timing(t.timing);
                stmt(t.stmt);
                break;
            }
            case StatementKind::Conditional: {
                const auto& c = s.as<ConditionalStatement>();
                if (c.conditions.size() != 1) unsupported("pattern if");
                Instr j; j.k = IK::JmpIfNot; j.e = expr(*c.conditions[0].expr);
                size_t jf = prog.emit(j);
                stmt(c.ifTrue);
                if (c.ifFalse) {
                    Instr skip; skip.k = IK::Jmp;
                    size_t js = prog.emit(skip);
                    prog.code[jf].target = prog.here();
                    stmt(*c.ifFalse);
                    prog.code[js].target = prog.here();
                } else {
                    prog.code[jf].target = prog.here();
                }
                break;
            }
            case StatementKind::ExpressionStatement:
                exprStmt(s.as<ExpressionStatement>().expr);
                break;
            case StatementKind::VariableDeclaration: {   // int x = 5;  inside a block
                const auto& vd = s.as<VariableDeclStatement>();
                if (const Expression* init = vd.symbol.getInitializer()) {
                    Instr i; i.k = IK::Assign; i.sig = sigmap.at(&vd.symbol); i.e = expr(*init);
                    prog.emit(i);
                }
                break;
            }
            case StatementKind::ForLoop: {
                const auto& f = s.as<ForLoopStatement>();
                for (const VariableSymbol* v : f.loopVars)
                    if (const Expression* init = v->getInitializer()) {
                        Instr i; i.k = IK::Assign; i.sig = sigmap.at(v); i.e = expr(*init);
                        prog.emit(i);
                    }
                for (const Expression* e : f.initializers) exprStmt(*e);
                size_t top = prog.here(), jf = (size_t)-1;
                if (f.stopExpr) { Instr j; j.k = IK::JmpIfNot; j.e = expr(*f.stopExpr); jf = prog.emit(j); }
                stmt(f.body);
                for (const Expression* e : f.steps) exprStmt(*e);
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                if (jf != (size_t)-1) prog.code[jf].target = prog.here();
                break;
            }
            case StatementKind::WhileLoop: {
                const auto& w = s.as<WhileLoopStatement>();
                size_t top = prog.here();
                Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                size_t jf = prog.emit(j);
                stmt(w.body);
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                prog.code[jf].target = prog.here();
                break;
            }
            case StatementKind::DoWhileLoop: {
                const auto& w = s.as<DoWhileLoopStatement>();
                size_t top = prog.here();
                stmt(w.body);
                Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                size_t jf = prog.emit(j);
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                prog.code[jf].target = prog.here();
                break;
            }
            case StatementKind::ForeverLoop: {
                size_t top = prog.here();
                stmt(s.as<ForeverLoopStatement>().body);
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                break;
            }
            case StatementKind::RepeatLoop: {   // constant count only: unrolled
                const auto& r = s.as<RepeatLoopStatement>();
                int64_t n = constInt(r.count);
                if (n > 1024) unsupported("repeat count > 1024 (unrolled)");
                for (int64_t k = 0; k < n; k++) stmt(r.body);
                break;
            }
            case StatementKind::Case: {
                const auto& c = s.as<CaseStatement>();
                Op cmp = c.condition == CaseStatementCondition::Normal        ? Op::CaseEq
                         : c.condition == CaseStatementCondition::WildcardJustZ ? Op::CaseZEq
                                                                                : Op::CaseXEq;
                std::vector<size_t> toEnd;
                for (const auto& item : c.items) {
                    E cond;   // OR of (selector === item_expr) over this item's labels
                    for (const Expression* x : item.expressions) {
                        E m = bin(cmp, expr(c.expr), expr(*x));
                        cond = cond ? bin(Op::LogOr, cond, m) : m;
                    }
                    Instr j; j.k = IK::JmpIfNot; j.e = cond;
                    size_t jf = prog.emit(j);
                    stmt(*item.stmt);
                    Instr skip; skip.k = IK::Jmp;
                    toEnd.push_back(prog.emit(skip));
                    prog.code[jf].target = prog.here();
                }
                if (c.defaultCase) stmt(*c.defaultCase);
                for (size_t k : toEnd) prog.code[k].target = prog.here();
                break;
            }
            default: unsupported("statement kind");
        }
    }
};

// Pass 1: register every variable / net as a simulator signal.
struct SigCollector : slang::ast::ASTVisitor<SigCollector, false, false> {
    std::function<int(const std::string&, int)> mkSig;
    std::unordered_map<const slang::ast::Symbol*, int>& sigmap;
    SigCollector(std::function<int(const std::string&, int)> f,
                 std::unordered_map<const slang::ast::Symbol*, int>& m) : mkSig(std::move(f)), sigmap(m) {}
    template <typename T> void reg(const T& s) {
        int w = (int)s.getType().getBitWidth();
        if (w > 64) unsupported_width(s.name);
        sigmap[&s] = mkSig(std::string(s.name), w);
    }
    static void unsupported_width(std::string_view n) {
        throw std::runtime_error("lower: signal wider than 64 bits: " + std::string(n));
    }
    void handle(const slang::ast::VariableSymbol& v) { reg(v); }
    void handle(const slang::ast::NetSymbol& n) { reg(n); }
};

// Pass 2: lower every procedural block.
struct ProcCollector : slang::ast::ASTVisitor<ProcCollector, false, false> {
    std::unordered_map<const slang::ast::Symbol*, int>& sigmap;
    LowerResult& out;
    std::function<int(int)> newDriver;
    ProcCollector(std::unordered_map<const slang::ast::Symbol*, int>& m, LowerResult& o,
                  std::function<int(int)> nd) : sigmap(m), out(o), newDriver(std::move(nd)) {}

    // assign y = expr;  -> process: drive y, then re-run when any input changes.
    // Each assign gets its own driver slot, so several assigns to one wire resolve.
    void handle(const slang::ast::ContinuousAssignSymbol& ca) {
        using namespace slang::ast;
        const auto& a = ca.getAssignment().as<AssignmentExpression>();
        Lowerer lw(sigmap);
        Instr i; i.k = IK::Assign;
        lw.lhs(a.left(), i);
        if (i.hi >= 0) Lowerer::unsupported("assign to a bit/part select");
        i.e = lw.expr(a.right());
        i.drv = newDriver(i.sig);
        lw.prog.emit(i);
        auto rs = readSet(lw.prog);
        bool loops = !rs.empty();
        if (loops) { lw.prog.emit(waitInstr(rs)); Instr j; j.k = IK::Jmp; j.target = 0; lw.prog.emit(j); }
        out.procs.push_back({std::move(lw.prog), loops, "assign"});
    }
    void handle(const slang::ast::ProceduralBlockSymbol& p) {
        using slang::ast::ProceduralBlockKind;
        Lowerer lw(sigmap);
        lw.stmt(p.getBody());
        lw.finishImplicit();
        bool loops = p.procedureKind != ProceduralBlockKind::Initial;
        if (p.procedureKind == ProceduralBlockKind::AlwaysComb ||
            p.procedureKind == ProceduralBlockKind::AlwaysLatch) {
            // runs once at t=0, then re-runs whenever any signal it reads changes
            auto rs = readSet(lw.prog);
            if (rs.empty()) loops = false;
            else lw.prog.emit(waitInstr(rs));
        }
        if (loops) { Instr j; j.k = IK::Jmp; j.target = 0; lw.prog.emit(j); }  // repeat forever
        out.procs.push_back({std::move(lw.prog), loops, std::string(slang::ast::toString(p.procedureKind))});
    }
};

inline LowerResult lower(slang::ast::Compilation& comp,
                         std::function<int(const std::string&, int)> mkSig,
                         std::function<int(int)> newDriver) {
    std::unordered_map<const slang::ast::Symbol*, int> sigmap;
    LowerResult out;
    SigCollector sc(std::move(mkSig), sigmap);
    comp.getRoot().visit(sc);
    ProcCollector pc(sigmap, out, std::move(newDriver));
    comp.getRoot().visit(pc);
    return out;
}
