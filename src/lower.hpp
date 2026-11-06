// lower.hpp - lower slang's elaborated AST into Sim signals + Prog processes.
// Built against slang v12.1 (the tag FetchContent pins in CMakeLists.txt).
//
// Supported subset (single module, flat):
//   statements : begin/end, if/else, case/casez/casex, blocking &
//                non-blocking assign to a variable, a[const], a[hi:lo],
//                compound assign (+= -= *= /= %= &= |= ^= <<= >>= <<<= >>>=),
//                @(posedge/negedge/any sig [or ...]), @*, always_comb,
//                for/while/do/forever/repeat(const), break/continue,
//                #<literal>, $display / $strobe / $monitor / $finish, assign
//   expressions: literals, variable refs, unary/binary ops, ?:, {concat},
//                a[expr], a[hi:lo]
// Assumes vectors are declared [N-1:0] (zero-based) so index == bit position.
// A ConversionExpression is lowered at the conversion's bit width so a
// context-determined operator (a = 4'hF + 4'h1 with a 8 bits wide) is not
// evaluated at the operands' self-determined width. Widening sign-extends
// when the operand is signed in that context (both operands signed, or an
// assignment of a signed expression). $signed / $unsigned retag the bits.
// Instantiating a module is an error until port connections are implemented.
// Not yet:  net initializers (wire y = ..), a[i +: w], tasks/functions.
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

// A parent asked for a width. fill sign-extends when widening. tag is the
// signedness after the resize. Propagated ops set fill == tag. An assignment
// sets fill from the expression and tag from the destination type.
struct ExprCtx {
    int w = -1;
    bool fill = false;
    bool tag = false;
    bool live = false;
};

struct Lowerer {
    std::unordered_map<const slang::ast::Symbol*, int>& sigmap;
    Prog prog;
    std::vector<size_t> implicitWaits;  // WaitEdge slots for @*, filled by finishImplicit()
    // Set while lowering a compound assignment. slang's right-hand side is the
    // binary operator, and its left operand is an LValueReference: the target's
    // type, and no symbol. The read is the assignment's left expression.
    const slang::ast::Expression* lvalue = nullptr;
    // Innermost loop is back(). break and continue emit Jmp; the pc is filled
    // in when that loop's end (break) or continuation (continue) is known.
    struct LoopJumps {
        std::vector<size_t> breaks;
        std::vector<size_t> continues;
    };
    std::vector<LoopJumps> loops;

    explicit Lowerer(std::unordered_map<const slang::ast::Symbol*, int>& m) : sigmap(m) {}

    [[noreturn]] static void unsupported(const char* what) {
        throw std::runtime_error(std::string("lower: unsupported ") + what);
    }

    size_t emitJmp() {
        Instr j;
        j.k = IK::Jmp;
        return prog.emit(j);
    }
    void patch(std::vector<size_t>& sites, size_t target) {
        for (size_t pc : sites) prog.code[pc].target = target;
        sites.clear();
    }

    // Constant integer from a literal or a slang-folded constant expression.
    static int64_t constInt(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (e.kind == ExpressionKind::IntegerLiteral)
            return *e.as<IntegerLiteral>().getValue().as<int64_t>();
        if (e.kind == ExpressionKind::Conversion)
            return constInt(e.as<ConversionExpression>().operand());
        if (const auto* cv = e.getConstant(); cv && cv->isInteger()) {
            if (auto v = cv->integer().as<int64_t>()) return *v;
        }
        unsupported("non-constant select bound");
    }

    static void needWidth(int w) {
        if (w <= 0 || w > 64) unsupported("width conversion outside 1..64 bits");
    }

    // Put selfTag on a constant or a ref. Other ops take their signedness
    // from eval, which reads the tags on their operands.
    static E tagExpr(E e, bool selfTag) {
        if (!e) return e;
        if (e->op == Op::Const) {
            if (e->k.sgn == selfTag) return e;
            Val k = e->k;
            k.sgn = selfTag;
            return konst(k);
        }
        if (e->op == Op::Ref) return e->sgn == selfTag ? e : ref(e->sig, selfTag);
        return e;
    }

    // Honor ctx when a parent asked for a width. Otherwise keep selfTag.
    E sized(E e, int srcW, bool selfTag, ExprCtx ctx) {
        if (!ctx.live) return tagExpr(e, selfTag);
        needWidth(ctx.w);
        if (ctx.w == srcW && ctx.tag == selfTag) return tagExpr(e, selfTag);
        return resize(std::move(e), ctx.w, ctx.fill && ctx.w > srcW, ctx.tag);
    }

    static bool isShift(slang::ast::BinaryOperator op) {
        using slang::ast::BinaryOperator;
        switch (op) {
            case BinaryOperator::LogicalShiftLeft:
            case BinaryOperator::LogicalShiftRight:
            case BinaryOperator::ArithmeticShiftLeft:
            case BinaryOperator::ArithmeticShiftRight:
                return true;
            default:
                return false;
        }
    }

    static bool widthPropagates(slang::ast::BinaryOperator op) {
        using slang::ast::BinaryOperator;
        switch (op) {
            case BinaryOperator::Add:
            case BinaryOperator::Subtract:
            case BinaryOperator::Multiply:
            case BinaryOperator::BinaryAnd:
            case BinaryOperator::BinaryOr:
            case BinaryOperator::BinaryXor:
                return true;
            default:
                return false;
        }
    }

    static bool isDivMod(slang::ast::BinaryOperator op) {
        using slang::ast::BinaryOperator;
        return op == BinaryOperator::Divide || op == BinaryOperator::Mod;
    }

    static const slang::ast::Expression& peel(const slang::ast::Expression& e) {
        using namespace slang::ast;
        const Expression* p = &e;
        while (p->kind == ExpressionKind::Conversion)
            p = &p->as<ConversionExpression>().operand();
        return *p;
    }

    // ---------- expressions ----------
    // ctx.live: produce a value of that width. An outer conversion wins over
    // the self-determined widening slang inserts underneath it, so
    // `cnt <= cnt + 1` with cnt 4 bits is a 4-bit add, not a 32-bit add sliced
    // down. Add/sub/mul stay congruent mod 2^w, signed or not, once operands
    // are extended to that width. Sign-extension is not congruent that way
    // for shifts or compares, so those extend before the operator.
    E expr(const slang::ast::Expression& e, ExprCtx ctx = {}) {
        using namespace slang::ast;
        switch (e.kind) {
            case ExpressionKind::IntegerLiteral: {
                const auto& sv = e.as<IntegerLiteral>().getValue();
                int w = (int)sv.getBitWidth();
                bool self = e.type->isSigned();
                E lit;
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
                    lit = konst(make(v, x, w, z, self));
                } else {
                    lit = konst(make(*sv.as<uint64_t>(), 0, w, 0, self));
                }
                return sized(lit, w, self, ctx);
            }
            case ExpressionKind::NamedValue: {
                const Symbol* sym = &e.as<NamedValueExpression>().symbol;
                auto it = sigmap.find(sym);
                if (it == sigmap.end()) unsupported("reference to non-signal symbol");
                int src = (int)e.type->getBitWidth();
                bool self = e.type->isSigned();
                return sized(ref(it->second, self), src, self, ctx);
            }
            case ExpressionKind::LValueReference: {
                if (!lvalue) unsupported("lvalue reference");
                return expr(*lvalue, ctx);
            }
            case ExpressionKind::Conversion: {
                // 4'hF inside an 8-bit add is a 4-bit literal under a conversion
                // to 8. cnt+1 is a 32-bit add under a conversion to 4. Either
                // way the operand is lowered at the width that applies here.
                // Propagated (11.8.2): the context type's signedness replaces
                // the operand's, then the value is extended. Implicit assignment
                // (11.8.3): extend with the operand's signedness. An outer width
                // still wins, so a truncating assign stays narrow.
                const auto& c = e.as<ConversionExpression>();
                int dst = (int)c.type->getBitWidth();
                bool fill, tag;
                if (ctx.live && ctx.w > 0) {
                    dst = ctx.w;
                    fill = ctx.fill;
                    tag = ctx.tag;
                } else if (c.conversionKind == ConversionKind::Propagated) {
                    fill = tag = c.type->isSigned();
                } else {
                    fill = c.operand().type->isSigned();
                    tag = c.type->isSigned();
                }
                needWidth(dst);
                return expr(c.operand(), ExprCtx{dst, fill, tag, true});
            }
            case ExpressionKind::Call: {
                // $signed / $unsigned keep the bits and change the tag.
                // Any other call used as a value is still unsupported.
                const auto& call = e.as<CallExpression>();
                std::string_view n = call.getSubroutineName();
                if (n != "$signed" && n != "$unsigned") unsupported("system function");
                auto args = call.arguments();
                if (args.size() != 1 || args[0]->kind == ExpressionKind::EmptyArgument)
                    unsupported("system function");
                int aw = (int)args[0]->type->getBitWidth();
                needWidth(aw);
                bool toS = n == "$signed";
                E arg = expr(*args[0]);
                E cast = resize(std::move(arg), aw, false, toS);
                return sized(cast, aw, toS, ctx);
            }
            case ExpressionKind::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                bool bw = u.op == UnaryOperator::BitwiseNot;
                E a;
                if (bw) {
                    int w = ctx.live && ctx.w > 0 ? ctx.w : (int)e.type->getBitWidth();
                    needWidth(w);
                    bool sg = e.type->isSigned();
                    a = expr(u.operand(), ExprCtx{w, sg, sg, true});
                } else {
                    a = expr(u.operand());
                }
                E out;
                switch (u.op) {
                    case UnaryOperator::BitwiseNot: out = un(Op::Not, a); break;
                    case UnaryOperator::LogicalNot: out = un(Op::LogNot, a); break;
                    case UnaryOperator::BitwiseAnd: out = un(Op::RedAnd, a); break;
                    case UnaryOperator::BitwiseOr: out = un(Op::RedOr, a); break;
                    case UnaryOperator::BitwiseXor: out = un(Op::RedXor, a); break;
                    default: unsupported("unary operator");
                }
                if (!bw) return sized(out, (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
                return out;
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                bool sizeBoth = widthPropagates(b.op) || isDivMod(b.op);
                bool shift = isShift(b.op);
                E l, r;
                int w = 0;
                if (sizeBoth || shift) {
                    // Shifts size only the left operand. The count stays
                    // self-determined and is always treated as unsigned.
                    // A narrower context shrinks an unsized literal (`cnt + 1`
                    // is 4 bits, not 32). A sized operand that is already wider
                    // stays wide; the result is truncated afterwards
                    // (`logic [3:0] a; logic [7:0] b; a = a / b`).
                    int opW = (int)e.type->getBitWidth();
                    w = opW;
                    if (ctx.live && ctx.w > 0 && ctx.w != opW) {
                        if (ctx.w > opW) {
                            w = ctx.w;
                        } else {
                            auto eff = e.getEffectiveWidth();
                            if (!eff || (int)*eff <= ctx.w) w = ctx.w;
                        }
                    }
                    needWidth(w);
                    bool sg = e.type->isSigned();
                    ExprCtx op{w, sg, sg, true};
                    l = expr(b.left(), op);
                    r = shift ? expr(b.right()) : expr(b.right(), op);
                } else {
                    l = expr(b.left());
                    r = expr(b.right());
                }
                Op kop;
                switch (b.op) {
                    case BinaryOperator::Add: kop = Op::Add; break;
                    case BinaryOperator::Subtract: kop = Op::Sub; break;
                    case BinaryOperator::Multiply: kop = Op::Mul; break;
                    case BinaryOperator::Divide: kop = Op::Div; break;
                    case BinaryOperator::Mod: kop = Op::Mod; break;
                    case BinaryOperator::BinaryAnd: kop = Op::And; break;
                    case BinaryOperator::BinaryOr: kop = Op::Or; break;
                    case BinaryOperator::BinaryXor: kop = Op::Xor; break;
                    case BinaryOperator::Equality: kop = Op::Eq; break;
                    case BinaryOperator::Inequality: kop = Op::Ne; break;
                    case BinaryOperator::CaseEquality: kop = Op::CaseEq; break;
                    case BinaryOperator::LessThan: kop = Op::Lt; break;
                    case BinaryOperator::LessThanEqual: kop = Op::Le; break;
                    case BinaryOperator::GreaterThan: kop = Op::Gt; break;
                    case BinaryOperator::GreaterThanEqual: kop = Op::Ge; break;
                    case BinaryOperator::LogicalShiftLeft:
                    case BinaryOperator::ArithmeticShiftLeft: kop = Op::Shl; break;
                    case BinaryOperator::LogicalShiftRight: kop = Op::Shr; break;
                    case BinaryOperator::ArithmeticShiftRight:
                        kop = e.type->isSigned() ? Op::Asr : Op::Shr;
                        break;
                    case BinaryOperator::LogicalAnd: kop = Op::LogAnd; break;
                    case BinaryOperator::LogicalOr: kop = Op::LogOr; break;
                    default: unsupported("binary operator");
                }
                E out = bin(kop, l, r);
                if (sizeBoth || shift) {
                    if (ctx.live && ctx.w > 0 && w != ctx.w)
                        return resize(std::move(out), ctx.w, false, ctx.tag);
                    return out;
                }
                return sized(out, (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1) unsupported("pattern conditional");
                int w = ctx.live && ctx.w > 0 ? ctx.w : (int)e.type->getBitWidth();
                needWidth(w);
                bool sg = e.type->isSigned();
                ExprCtx arm{w, sg, sg, true};
                return mux(expr(*c.conditions[0].expr), expr(c.left(), arm), expr(c.right(), arm));
            }
            case ExpressionKind::Concatenation: {
                std::vector<E> parts;
                for (auto* op : e.as<ConcatenationExpression>().operands()) parts.push_back(expr(*op));
                return sized(concat(parts), (int)e.type->getBitWidth(), false, ctx);
            }
            case ExpressionKind::ElementSelect: {   // a[i]
                const auto& sel = e.as<ElementSelectExpression>();
                E base = expr(sel.value());
                const auto& idx = sel.selector();
                int tw = (int)e.type->getBitWidth();
                if (idx.kind == ExpressionKind::IntegerLiteral || idx.getConstant()) {
                    int i = (int)constInt(idx);
                    return sized(slice(base, i, i), tw, e.type->isSigned(), ctx);
                }
                return sized(bin(Op::Index, base, expr(idx)), tw, e.type->isSigned(), ctx);
            }
            case ExpressionKind::RangeSelect: {     // a[hi:lo]
                const auto& r = e.as<RangeSelectExpression>();
                if (r.getSelectionKind() != RangeSelectionKind::Simple) unsupported("indexed part-select (+: / -:)");
                return sized(slice(expr(r.value()), (int)constInt(r.left()), (int)constInt(r.right())),
                             (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
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

    // $display / $strobe / $monitor use the kernel format (a string of %b / %d /
    // literal text, arguments evaluated when the line is printed). $finish and
    // $stop stop the sim. Anything else is still ignored.
    void sysCall(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        std::string_view n = call.getSubroutineName();
        if (n == "$finish" || n == "$stop") {
            Instr i; i.k = IK::Finish; prog.emit(i); return;
        }
        IK k;
        if (n == "$display") k = IK::Display;
        else if (n == "$strobe") k = IK::Strobe;
        else if (n == "$monitor") k = IK::Monitor;
        else {
            std::cerr << "lower: ignoring system call " << n << "\n";
            return;
        }
        Instr i; i.k = k;
        auto args = call.arguments();
        size_t ai = 0;
        if (!args.empty() && args[0]->kind != ExpressionKind::EmptyArgument) {
            const Expression& first = peel(*args[0]);
            if (first.kind != ExpressionKind::StringLiteral)
                unsupported("system task without a format string");
            i.fmt = std::string(first.as<StringLiteral>().getValue());
            ai = 1;
        }
        for (; ai < args.size(); ai++) {
            if (args[ai]->kind == ExpressionKind::EmptyArgument)
                unsupported("empty system task argument");
            i.args.push_back(expr(*args[ai]));
        }
        prog.emit(i);
    }

    // Assignments, ++/--, and system calls used as statements.
    void exprStmt(const slang::ast::Expression& ex) {
        using namespace slang::ast;
        if (ex.kind == ExpressionKind::Assignment) {
            const auto& a = ex.as<AssignmentExpression>();
            Instr i;
            i.k = a.isNonBlocking() ? IK::Nba : IK::Assign;
            lhs(a.left(), i);
            const Expression* saved = lvalue;
            if (a.isCompound()) lvalue = &a.left();
            i.e = expr(a.right());
            lvalue = saved;
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
            sysCall(ex.as<CallExpression>());
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
                stmt(s.as<BlockStatement>().body);
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
            case StatementKind::Break: {
                if (loops.empty()) unsupported("break outside a loop");
                loops.back().breaks.push_back(emitJmp());
                break;
            }
            case StatementKind::Continue: {
                if (loops.empty()) unsupported("continue outside a loop");
                loops.back().continues.push_back(emitJmp());
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
                loops.push_back({});
                stmt(f.body);
                // continue lands on the step. The step then falls into the
                // back-edge, which checks the condition again.
                patch(loops.back().continues, prog.here());
                for (const Expression* e : f.steps) exprStmt(*e);
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                if (jf != (size_t)-1) prog.code[jf].target = prog.here();
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::WhileLoop: {
                const auto& w = s.as<WhileLoopStatement>();
                size_t top = prog.here();
                Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                size_t jf = prog.emit(j);
                loops.push_back({});
                stmt(w.body);
                patch(loops.back().continues, prog.here());
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                prog.code[jf].target = prog.here();
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::DoWhileLoop: {
                const auto& w = s.as<DoWhileLoopStatement>();
                size_t top = prog.here();
                loops.push_back({});
                stmt(w.body);
                // continue tests the condition before another trip through the body.
                patch(loops.back().continues, prog.here());
                Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                size_t jf = prog.emit(j);
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                prog.code[jf].target = prog.here();
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::ForeverLoop: {
                size_t top = prog.here();
                loops.push_back({});
                stmt(s.as<ForeverLoopStatement>().body);
                patch(loops.back().continues, prog.here());
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::RepeatLoop: {   // constant count only: unrolled
                const auto& r = s.as<RepeatLoopStatement>();
                int64_t n = constInt(r.count);
                if (n > 1024) unsupported("repeat count > 1024 (unrolled)");
                loops.push_back({});
                for (int64_t k = 0; k < n; k++) {
                    stmt(r.body);
                    // The next copy is the next iteration. After the last copy,
                    // continue leaves the loop.
                    patch(loops.back().continues, prog.here());
                }
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
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

// Nested instances are rejected until ports are actually connected. A top-level
// instance (the design root) is visited; anything deeper is a hard error.
inline void rejectNestedInstance(const slang::ast::InstanceSymbol& inst) {
    if (inst.isTopLevel()) return;
    throw std::runtime_error(
        "lower: instantiating module '" + std::string(inst.getDefinition().name) +
        "' (instance '" + std::string(inst.name) +
        "') is not supported; port connections are not implemented");
}

// Pass 1: register every variable / net as a simulator signal.
struct SigCollector : slang::ast::ASTVisitor<SigCollector> {
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
    void handle(const slang::ast::InstanceSymbol& inst) {
        rejectNestedInstance(inst);
        visitDefault(inst);
    }
};

// Pass 2: lower every procedural block.
struct ProcCollector : slang::ast::ASTVisitor<ProcCollector> {
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
    void handle(const slang::ast::InstanceSymbol& inst) {
        rejectNestedInstance(inst);
        visitDefault(inst);
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
