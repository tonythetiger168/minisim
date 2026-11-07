// lower.hpp - lower slang's elaborated AST into Sim signals + Prog processes.
// Built against slang v12.1 (the tag FetchContent pins in CMakeLists.txt).
//
// Supported subset (a parent may instantiate a child; ports connect):
//   statements : begin/end, if/else, case/casez/casex, blocking &
//                non-blocking assign to a variable, a[const], a[hi:lo],
//                compound assign (+= -= *= /= %= &= |= ^= <<= >>= <<<= >>>=),
//                @(posedge/negedge/any sig [or ...]), @*, always_comb,
//                for/while/do/forever/repeat(const), break/continue,
//                #<literal>, $display / $strobe / $monitor / $finish, assign
//                (optional drive strength), buf/not/and/nand/or/nor/xor/xnor
//                and pullup/pulldown (same strength spec), module-level task
//                and function calls (input, output, inout)
//   expressions: literals, variable refs, unary/binary ops, ?:, {concat},
//                a[expr], a[hi:lo], a function call
// Assumes vectors are declared [N-1:0] (zero-based) so index == bit position.
// A ConversionExpression is lowered at the conversion's bit width so a
// context-determined operator (a = 4'hF + 4'h1 with a 8 bits wide) is not
// evaluated at the operands' self-determined width. Widening sign-extends
// when the operand is signed in that context (both operands signed, or an
// assignment of a signed expression). $signed / $unsigned retag the bits.
// A module instance connects each port with a continuous assign. An input
// expression drives the child's formal. An output's external expression is an
// assignment whose right-hand side is an EmptyArgument (the formal), the same
// shape as a subroutine output write-back. An inout net is both directions.
// Not yet:  nmos/pmos/cmos/tran, bufif/notif, trireg, a[i +: w], recursive
//           tasks/functions, ref arguments, DPI, class methods,
//           hierarchical calls, instance arrays, interfaces, hierarchical
//           references into a child. Arguments are one static copy per subroutine.
//
// Usage:
//   LowerResult r = lower(compilation,
//                         [&](const std::string& n, int w, bool net){ return S.sig(n, w, net); },
//                         [&](int sig){ return S.newDriver(sig); });
//   for (auto& pr : r.procs) S.spawn(exec(&pr.prog));
#pragma once
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
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
#include "slang/ast/SemanticFacts.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"

// name, width, and whether the signal is a net. A net starts at Z.
using MkSig = std::function<int(const std::string&, int, bool)>;

struct LoweredProc { Prog prog; bool loops; std::string kind; };

// One module-level task or function. Arguments and the function result are
// signals (the formal and slang's returnValVar), shared by every call.
// `prog` is owned by LowerResult::subs so a Call's pointer stays valid after
// lower() returns.
struct Subr {
    Prog* prog = nullptr;
    const slang::ast::SubroutineSymbol* sym = nullptr;
    int retSig = -1;
    int retW = 1;
    bool retSgn = false;
    struct Arg {
        int sig = -1;
        slang::ast::ArgumentDirection dir = slang::ast::ArgumentDirection::In;
        int w = 1;
        bool sgn = false;
    };
    std::vector<Arg> args;
    std::vector<Subr*> calls;
};

struct LowerResult {
    std::vector<LoweredProc> procs;
    std::vector<std::unique_ptr<Prog>> subs;
};

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
    std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* subs = nullptr;
    MkSig mkSig;
    int* tempSeq = nullptr;
    int retSig = -1;     // function result signal while lowering a function body
    Subr* current = nullptr;
    // Set while lowering an output/inout write-back. slang's actual is an
    // assignment whose right-hand side is an EmptyArgument (the formal),
    // possibly wrapped in a conversion to the actual's type.
    struct OutSlot { int sig; int w; bool sgn; };
    const OutSlot* outSlot = nullptr;

    explicit Lowerer(std::unordered_map<const slang::ast::Symbol*, int>& m) : sigmap(m) {}

    void bind(std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* s,
              MkSig mk, int* seq) {
        subs = s;
        mkSig = std::move(mk);
        tempSeq = seq;
    }

    int fresh(int w) {
        if (!mkSig) unsupported("temporary");
        int n = tempSeq ? (*tempSeq)++ : 0;
        return mkSig("_tf" + std::to_string(n), w, false);
    }

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

    // Copy inputs in, call, copy outputs back. A function result is snapshotted
    // into a fresh signal: f(a)+f(b) would otherwise read the same return signal
    // twice and see only the second call.
    E emitUserCall(const slang::ast::CallExpression& call, bool wantValue) {
        using namespace slang::ast;
        if (call.thisClass()) unsupported("class method");
        if (call.isSystemCall()) unsupported("system call");
        const auto* sym = std::get<const SubroutineSymbol*>(call.subroutine);
        if (!sym || !subs) unsupported("subroutine call");
        if (sym->flags.has(MethodFlags::DPIImport)) unsupported("DPI import");
        auto it = subs->find(sym);
        if (it == subs->end()) unsupported("subroutine call");
        Subr* sub = it->second;
        if (current) current->calls.push_back(sub);

        auto args = call.arguments();
        if (args.size() != sub->args.size()) unsupported("subroutine argument count");

        for (size_t n = 0; n < args.size(); n++) {
            const auto& formal = sub->args[n];
            if (formal.dir == ArgumentDirection::Ref) unsupported("ref argument");
            const Expression& a = *args[n];
            if (formal.dir == ArgumentDirection::In) {
                if (a.kind == ExpressionKind::EmptyArgument) unsupported("empty argument");
                Instr i;
                i.k = IK::Assign;
                i.sig = formal.sig;
                i.e = expr(a);
                prog.emit(i);
            } else if (formal.dir == ArgumentDirection::InOut) {
                if (a.kind != ExpressionKind::Assignment) unsupported("inout argument");
                Instr i;
                i.k = IK::Assign;
                i.sig = formal.sig;
                i.e = expr(a.as<AssignmentExpression>().left());
                prog.emit(i);
            } else if (a.kind != ExpressionKind::Assignment &&
                       a.kind != ExpressionKind::EmptyArgument) {
                unsupported("output argument");
            }
        }

        Instr c;
        c.k = IK::Call;
        c.callee = sub->prog;
        prog.emit(c);

        for (size_t n = 0; n < args.size(); n++) {
            const auto& formal = sub->args[n];
            if (formal.dir != ArgumentDirection::Out && formal.dir != ArgumentDirection::InOut)
                continue;
            const Expression& a = *args[n];
            if (a.kind == ExpressionKind::EmptyArgument) continue;
            if (a.kind != ExpressionKind::Assignment) unsupported("output argument");
            const auto& as = a.as<AssignmentExpression>();
            Instr i;
            i.k = IK::Assign;
            lhs(as.left(), i);
            OutSlot slot{formal.sig, formal.w, formal.sgn};
            const OutSlot* saved = outSlot;
            outSlot = &slot;
            i.e = expr(as.right());
            outSlot = saved;
            prog.emit(i);
        }

        if (!wantValue) return {};
        if (sub->retSig < 0) unsupported("subroutine call has no return value");
        int tmp = fresh(sub->retW);
        Instr i;
        i.k = IK::Assign;
        i.sig = tmp;
        i.e = ref(sub->retSig, sub->retSgn);
        prog.emit(i);
        return ref(tmp, sub->retSgn);
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
            case ExpressionKind::EmptyArgument: {
                if (!outSlot) unsupported("empty argument");
                return sized(ref(outSlot->sig, outSlot->sgn), outSlot->w, outSlot->sgn, ctx);
            }
            case ExpressionKind::Call: {
                const auto& call = e.as<CallExpression>();
                if (!call.isSystemCall()) {
                    E v = emitUserCall(call, true);
                    int w = (int)e.type->getBitWidth();
                    needWidth(w);
                    return sized(std::move(v), w, e.type->isSigned(), ctx);
                }
                // $signed / $unsigned keep the bits and change the tag.
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
            const auto& call = ex.as<CallExpression>();
            if (call.isSystemCall()) sysCall(call);
            else emitUserCall(call, false);
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
                const Expression* init = vd.symbol.getInitializer();
                if (!init) break;
                // A static local in a task or function is initialized on the
                // first call, then left alone. The flag starts as X, so the
                // first call fails the "already done" test and takes the init.
                bool once = current && vd.symbol.lifetime == slang::ast::VariableLifetime::Static;
                if (!once) {
                    Instr i; i.k = IK::Assign; i.sig = sigmap.at(&vd.symbol); i.e = expr(*init);
                    prog.emit(i);
                    break;
                }
                int flag = fresh(1);
                Instr j; j.k = IK::JmpIfNot; j.e = ref(flag);
                size_t jf = prog.emit(j);
                size_t js = emitJmp();
                prog.code[jf].target = prog.here();
                Instr set; set.k = IK::Assign; set.sig = flag; set.e = konst(mk(1, 1));
                prog.emit(set);
                Instr i; i.k = IK::Assign; i.sig = sigmap.at(&vd.symbol); i.e = expr(*init);
                prog.emit(i);
                prog.code[js].target = prog.here();
                break;
            }
            case StatementKind::Return: {
                const auto& r = s.as<ReturnStatement>();
                if (r.expr) {
                    if (retSig < 0) unsupported("return value");
                    Instr i;
                    i.k = IK::Assign;
                    i.sig = retSig;
                    i.e = expr(*r.expr);
                    prog.emit(i);
                }
                Instr ret;
                ret.k = IK::Ret;
                prog.emit(ret);
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

// Skip bodies slang built only for name lookup. Arrays and interfaces are
// later limits, so they stay a hard error instead of a silent wrong netlist.
inline bool enterInstance(const slang::ast::InstanceSymbol& inst) {
    using namespace slang::ast;
    if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return false;
    if (!inst.arrayPath.empty()) Lowerer::unsupported("instance array");
    if (inst.isInterface()) Lowerer::unsupported("interface instance");
    if (!inst.isTopLevel() && !inst.isModule()) Lowerer::unsupported("non-module instance");
    return true;
}

// Pass 1: register every variable / net as a simulator signal.
// Nested signals are named from the instance path (`u.y`) so two children
// with the same local name stay distinct. The top module keeps bare names.
struct SigCollector : slang::ast::ASTVisitor<SigCollector> {
    MkSig mkSig;
    std::unordered_map<const slang::ast::Symbol*, int>& sigmap;
    std::string prefix;
    SigCollector(MkSig f, std::unordered_map<const slang::ast::Symbol*, int>& m)
        : mkSig(std::move(f)), sigmap(m) {}
    template <typename T> void reg(const T& s, bool net) {
        int w = (int)s.getType().getBitWidth();
        if (w > 64) unsupported_width(s.name);
        sigmap[&s] = mkSig(prefix + std::string(s.name), w, net);
    }
    static void unsupported_width(std::string_view n) {
        throw std::runtime_error("lower: signal wider than 64 bits: " + std::string(n));
    }
    void handle(const slang::ast::VariableSymbol& v) { reg(v, false); }
    void handle(const slang::ast::FormalArgumentSymbol& v) { reg(v, false); }
    void handle(const slang::ast::NetSymbol& n) { reg(n, true); }
    void handle(const slang::ast::InstanceSymbol& inst) {
        if (!enterInstance(inst)) return;
        std::string saved = prefix;
        if (!inst.isTopLevel()) {
            if (inst.name.empty()) Lowerer::unsupported("unnamed instance");
            prefix += std::string(inst.name);
            prefix.push_back('.');
        }
        visitDefault(inst);
        prefix = std::move(saved);
    }
};

// Pass 2: lower every procedural block and connect instance ports.
struct ProcCollector : slang::ast::ASTVisitor<ProcCollector> {
    std::unordered_map<const slang::ast::Symbol*, int>& sigmap;
    LowerResult& out;
    std::function<int(int)> newDriver;
    std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* subs = nullptr;
    MkSig mkSig;
    int* tempSeq = nullptr;
    ProcCollector(std::unordered_map<const slang::ast::Symbol*, int>& m, LowerResult& o,
                  std::function<int(int)> nd,
                  std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* s,
                  MkSig mk, int* seq)
        : sigmap(m), out(o), newDriver(std::move(nd)), subs(s), mkSig(std::move(mk)), tempSeq(seq) {}

    void prep(Lowerer& lw) { lw.bind(subs, mkSig, tempSeq); }

    // One driver, then wait on every signal the right-hand side reads.
    // Several assigns to one wire each own a driver slot and resolve.
    void finishAssign(Lowerer& lw, Instr i) {
        if (i.hi >= 0) Lowerer::unsupported("assign to a bit/part select");
        i.drv = newDriver(i.sig);
        lw.prog.emit(i);
        auto rs = readSet(lw.prog);
        bool loops = !rs.empty();
        if (loops) {
            lw.prog.emit(waitInstr(rs));
            Instr j; j.k = IK::Jmp; j.target = 0; lw.prog.emit(j);
        }
        out.procs.push_back({std::move(lw.prog), loops, "assign"});
    }

    // Module-level `logic x = v;` (IEEE 1800 6.8): a static variable's
    // declaration initializer runs before any initial or always procedure
    // starts. All of them go into one process that lower() places first, so
    // it is the first thing to run at time 0. Locals of a block, task or
    // function are not module members and are handled by Lowerer.
    std::unique_ptr<Lowerer> init;
    void handle(const slang::ast::VariableSymbol& v) {
        const slang::ast::Expression* e = v.getInitializer();
        if (!e) return;
        const slang::ast::Scope* sc = v.getParentScope();
        if (!sc || sc->asSymbol().kind != slang::ast::SymbolKind::InstanceBody) return;
        if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
        Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v); i.e = init->expr(*e);
        init->prog.emit(i);
    }
    // `wire y = a & b;` (IEEE 1800 6.7.1) is a continuous assignment to y.
    void handle(const slang::ast::NetSymbol& n) {
        const slang::ast::Expression* e = n.getInitializer();
        if (!e) return;
        Lowerer lw(sigmap);
        prep(lw);
        Instr i; i.k = IK::Assign; i.sig = sigmap.at(&n); i.e = lw.expr(*e);
        finishAssign(lw, i);
    }
    // Moves the initializer process to the front of `out.procs`.
    void placeInit() {
        if (!init) return;
        out.procs.insert(out.procs.begin(), {std::move(init->prog), false, "init"});
        init.reset();
    }

    static uint8_t strengthOf(slang::ast::DriveStrength s) {
        using slang::ast::DriveStrength;
        switch (s) {
            case DriveStrength::Supply: return STR_SUPPLY;
            case DriveStrength::Strong: return STR_STRONG;
            case DriveStrength::Pull: return STR_PULL;
            case DriveStrength::Weak: return STR_WEAK;
            case DriveStrength::HighZ: return STR_HIZ;
        }
        return STR_STRONG;
    }

    // slang's pair is (0-strength, 1-strength). A missing side keeps `fallback`.
    static void applyStrength(Instr& i, std::optional<slang::ast::DriveStrength> ds0,
                              std::optional<slang::ast::DriveStrength> ds1,
                              uint8_t fb0, uint8_t fb1) {
        i.s0 = ds0 ? strengthOf(*ds0) : fb0;
        i.s1 = ds1 ? strengthOf(*ds1) : fb1;
    }

    void driveOut(const slang::ast::Expression& out, E rhs, uint8_t s0, uint8_t s1) {
        Lowerer lw(sigmap);
        prep(lw);
        Instr i;
        i.k = IK::Assign;
        i.s0 = s0;
        i.s1 = s1;
        // A gate output port is sometimes an assignment whose left side is the net.
        const slang::ast::Expression* tgt = &out;
        if (tgt->kind == slang::ast::ExpressionKind::Assignment)
            tgt = &tgt->as<slang::ast::AssignmentExpression>().left();
        lw.lhs(*tgt, i);
        i.e = rhs;
        finishAssign(lw, i);
    }

    void handle(const slang::ast::ContinuousAssignSymbol& ca) {
        using namespace slang::ast;
        const auto& a = ca.getAssignment().as<AssignmentExpression>();
        Lowerer lw(sigmap);
        prep(lw);
        Instr i; i.k = IK::Assign;
        auto [ds0, ds1] = ca.getDriveStrength();
        applyStrength(i, ds0, ds1, STR_STRONG, STR_STRONG);
        lw.lhs(a.left(), i);
        i.e = lw.expr(a.right());
        finishAssign(lw, i);
    }

    // buf/not/and/nand/or/nor/xor/xnor and pullup/pulldown drive the same way
    // as a continuous assign. Other primitives (mos, tran, bufif) do not.
    void handle(const slang::ast::PrimitiveInstanceSymbol& g) {
        using namespace slang::ast;
        if (g.getDelay()) Lowerer::unsupported("gate delay");
        std::string_view name = g.primitiveType.name;
        auto ports = g.getPortConnections();
        auto [ds0, ds1] = g.getDriveStrength();
        uint8_t fb0 = STR_STRONG, fb1 = STR_STRONG;
        bool pull = name == "pullup" || name == "pulldown";
        if (name == "pullup") { fb0 = STR_HIZ; fb1 = STR_PULL; }
        else if (name == "pulldown") { fb0 = STR_PULL; fb1 = STR_HIZ; }
        uint8_t s0 = ds0 ? strengthOf(*ds0) : fb0;
        uint8_t s1 = ds1 ? strengthOf(*ds1) : fb1;

        bool ninput = name == "and" || name == "nand" || name == "or" || name == "nor" ||
                      name == "xor" || name == "xnor";
        bool noutput = name == "buf" || name == "not";
        if (!pull && !ninput && !noutput)
            throw std::runtime_error("lower: unsupported gate " + std::string(name));

        if (pull) {
            if (ports.size() != 1 || !ports[0]) Lowerer::unsupported("pull gate port");
            int w = (int)ports[0]->type->getBitWidth();
            if (w <= 0 || w > 64) Lowerer::unsupported("gate width");
            driveOut(*ports[0], konst(mk(name == "pullup" ? mask(w) : 0, w)), s0, s1);
            return;
        }

        Lowerer lw(sigmap);
        prep(lw);
        if (noutput) {
            if (ports.size() < 2 || !ports.back()) Lowerer::unsupported("gate ports");
            E in = lw.expr(*ports.back());
            // not(not) is a buf. Z becomes X, and 0 and 1 pass through.
            E rhs = name == "not" ? un(Op::Not, in) : un(Op::Not, un(Op::Not, in));
            for (size_t i = 0; i + 1 < ports.size(); i++) {
                if (!ports[i]) Lowerer::unsupported("gate ports");
                driveOut(*ports[i], rhs, s0, s1);
            }
            return;
        }

        if (ports.size() < 2 || !ports[0]) Lowerer::unsupported("gate ports");
        Op op = (name == "and" || name == "nand") ? Op::And
              : (name == "or" || name == "nor") ? Op::Or : Op::Xor;
        if (!ports[1]) Lowerer::unsupported("gate ports");
        E acc = lw.expr(*ports[1]);
        for (size_t i = 2; i < ports.size(); i++) {
            if (!ports[i]) Lowerer::unsupported("gate ports");
            acc = bin(op, acc, lw.expr(*ports[i]));
        }
        if (name == "nand" || name == "nor" || name == "xnor") acc = un(Op::Not, acc);
        driveOut(*ports[0], acc, s0, s1);
    }

    // The child's formal signal. A null port (no internal connection) is skipped.
    struct Formal { int sig; int w; bool sgn; };
    std::optional<Formal> portFormal(const slang::ast::PortSymbol& ps) {
        using namespace slang::ast;
        const Symbol* sym = ps.internalSymbol;
        if (!sym) {
            const Expression* in = ps.getInternalExpr();
            if (!in) return std::nullopt;
            const Expression& peeled = Lowerer::peel(*in);
            if (peeled.kind != ExpressionKind::NamedValue)
                Lowerer::unsupported("port internal connection");
            sym = &peeled.as<NamedValueExpression>().symbol;
        }
        auto it = sigmap.find(sym);
        if (it == sigmap.end())
            throw std::runtime_error("lower: port '" + std::string(ps.name) + "' is not a signal");
        int w = (int)ps.getType().getBitWidth();
        if (w <= 0 || w > 64) Lowerer::unsupported("port width");
        return Formal{it->second, w, ps.getType().isSigned()};
    }

    void driveSig(int sig, const slang::ast::Expression& rhs) {
        Lowerer lw(sigmap);
        prep(lw);
        Instr i; i.k = IK::Assign; i.sig = sig; i.e = lw.expr(rhs);
        finishAssign(lw, i);
    }

    // Drive the parent's actual from the child's formal. `ext` is the
    // assignment slang built for an output or inout; its right-hand side is
    // an EmptyArgument standing in for that formal.
    void driveActual(const slang::ast::AssignmentExpression& ext, const Formal& formal) {
        Lowerer lw(sigmap);
        prep(lw);
        Lowerer::OutSlot slot{formal.sig, formal.w, formal.sgn};
        lw.outSlot = &slot;
        Instr i; i.k = IK::Assign;
        lw.lhs(ext.left(), i);
        i.e = lw.expr(ext.right());
        finishAssign(lw, i);
    }

    void connectInstance(const slang::ast::InstanceSymbol& inst) {
        using namespace slang::ast;
        if (inst.isTopLevel()) return;
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn || conn->port.kind != SymbolKind::Port)
                Lowerer::unsupported("interface port");
            const auto& ps = conn->port.as<PortSymbol>();
            const Expression* ext = conn->getExpression();
            if (!ext || ps.isNullPort) continue;
            auto formal = portFormal(ps);
            if (!formal) continue;
            if (ps.direction == ArgumentDirection::In) {
                driveSig(formal->sig, *ext);
                continue;
            }
            if (ps.direction != ArgumentDirection::Out && ps.direction != ArgumentDirection::InOut)
                Lowerer::unsupported("ref port");
            if (ext->kind != ExpressionKind::Assignment)
                Lowerer::unsupported("output port connection");
            const auto& asg = ext->as<AssignmentExpression>();
            driveActual(asg, *formal);
            if (ps.direction != ArgumentDirection::InOut) continue;
            if (!ps.isNetPort()) Lowerer::unsupported("inout port that is not a net");
            if ((int)asg.left().type->getBitWidth() != formal->w)
                Lowerer::unsupported("inout width mismatch");
            driveSig(formal->sig, asg.left());
        }
    }

    void handle(const slang::ast::InstanceSymbol& inst) {
        if (!enterInstance(inst)) return;
        connectInstance(inst);
        visitDefault(inst);
    }
    void handle(const slang::ast::ProceduralBlockSymbol& p) {
        using slang::ast::ProceduralBlockKind;
        Lowerer lw(sigmap);
        prep(lw);
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

// @* lowered inside a subroutine is a WaitEdge with no events yet. Patch it
// after every callee body is installed, so the wait sees those reads too.
inline void patchImplicit(Prog& prog) {
    std::vector<size_t> slots;
    for (size_t pc = 0; pc < prog.code.size(); pc++) {
        const Instr& i = prog.code[pc];
        if (i.k == IK::WaitEdge && i.events.empty() && i.sig < 0) slots.push_back(pc);
    }
    if (slots.empty()) return;
    auto rs = readSet(prog);
    if (rs.empty()) Lowerer::unsupported("@* in a block that reads no signals");
    for (size_t pc : slots) prog.code[pc] = waitInstr(rs);
}

inline void rejectRecursive(const std::vector<Subr*>& subs) {
    enum class Color { White, Gray, Black };
    std::unordered_map<const Subr*, Color> color;
    for (Subr* s : subs) color[s] = Color::White;
    auto dfs = [&](auto& self, Subr* s) -> void {
        color[s] = Color::Gray;
        for (Subr* c : s->calls) {
            if (color[c] == Color::Gray) Lowerer::unsupported("recursive subroutine");
            if (color[c] == Color::White) self(self, c);
        }
        color[s] = Color::Black;
    };
    for (Subr* s : subs)
        if (color[s] == Color::White) dfs(dfs, s);
}

inline LowerResult lower(slang::ast::Compilation& comp, MkSig mkSig,
                         std::function<int(int)> newDriver) {
    std::unordered_map<const slang::ast::Symbol*, int> sigmap;
    LowerResult out;
    SigCollector sc(mkSig, sigmap);
    comp.getRoot().visit(sc);

    struct Finder : slang::ast::ASTVisitor<Finder> {
        std::vector<const slang::ast::SubroutineSymbol*> found;
        void handle(const slang::ast::InstanceSymbol& inst) {
            if (!enterInstance(inst)) return;
            visitDefault(inst);
        }
        void handle(const slang::ast::SubroutineSymbol& s) { found.push_back(&s); }
    };
    Finder finder;
    comp.getRoot().visit(finder);

    std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*> subMap;
    std::vector<std::unique_ptr<Subr>> owned;
    std::vector<Subr*> order;
    for (const slang::ast::SubroutineSymbol* sym : finder.found) {
        if (subMap.count(sym)) continue;
        auto sub = std::make_unique<Subr>();
        out.subs.push_back(std::make_unique<Prog>());
        sub->prog = out.subs.back().get();
        sub->sym = sym;
        if (sym->returnValVar) {
            auto it = sigmap.find(sym->returnValVar);
            if (it == sigmap.end()) Lowerer::unsupported("function return");
            sub->retSig = it->second;
            sub->retW = (int)sym->getReturnType().getBitWidth();
            if (sub->retW <= 0 || sub->retW > 64) Lowerer::unsupported("function return width");
            sub->retSgn = sym->getReturnType().isSigned();
        }
        for (const slang::ast::FormalArgumentSymbol* formal : sym->getArguments()) {
            auto it = sigmap.find(formal);
            if (it == sigmap.end()) Lowerer::unsupported("subroutine argument");
            int w = (int)formal->getType().getBitWidth();
            if (w <= 0 || w > 64) Lowerer::unsupported("subroutine argument width");
            sub->args.push_back({it->second, formal->direction, w, formal->getType().isSigned()});
        }
        subMap[sym] = sub.get();
        order.push_back(sub.get());
        owned.push_back(std::move(sub));
    }

    int tempSeq = 0;
    for (Subr* sub : order) {
        if (sub->sym->flags.has(slang::ast::MethodFlags::DPIImport)) continue;
        Lowerer lw(sigmap);
        lw.bind(&subMap, mkSig, &tempSeq);
        lw.retSig = sub->retSig;
        lw.current = sub;
        lw.stmt(sub->sym->getBody());
        *sub->prog = std::move(lw.prog);
    }
    for (Subr* sub : order) patchImplicit(*sub->prog);

    ProcCollector pc(sigmap, out, std::move(newDriver), &subMap, mkSig, &tempSeq);
    comp.getRoot().visit(pc);
    pc.placeInit();
    rejectRecursive(order);
    return out;
}
