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
// Selects use the declared range ([7:0], [8:1], [0:7], [3:0][7:0]); see packedSel.
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
#include <cmath>
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
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/types/AllTypes.h"

// Fixed-size unpacked arrays (7.4), keyed by the array's symbol. SigCollector
// fills it; Lowerer reads it. lower() clears it first, since symbols from an
// earlier compilation may share addresses with this one's.
using ArrMap = std::unordered_map<const slang::ast::Symbol*, std::shared_ptr<ArrShape>>;
inline ArrMap& arrMap() { static ArrMap m; return m; }
constexpr int64_t kMaxArrayElems = 1 << 20;

// Time (22.7, 3.14.3): one kernel tick is the simulation precision, the finest
// time precision of any module, as a power of ten of a second (-9 = 1ns).
inline int& simPrecExp() { static int p = -9; return p; }
inline int tsExp(const slang::TimeScaleValue& v) {
    int e = 0;
    switch (v.unit) {
        case slang::TimeUnit::Seconds: e = 0; break;
        case slang::TimeUnit::Milliseconds: e = -3; break;
        case slang::TimeUnit::Microseconds: e = -6; break;
        case slang::TimeUnit::Nanoseconds: e = -9; break;
        case slang::TimeUnit::Picoseconds: e = -12; break;
        case slang::TimeUnit::Femtoseconds: e = -15; break;
    }
    if (v.magnitude == slang::TimeScaleMagnitude::Ten) e += 1;
    if (v.magnitude == slang::TimeScaleMagnitude::Hundred) e += 2;
    return e;
}
// Named blocks (9.6.2): an id for each block some `disable` names from outside
// the block, and the blocks that are a whole process (Prog::roots).
struct BlockIds {
    std::unordered_map<const slang::ast::Symbol*, int> id;
    std::unordered_set<int> roots;
    std::vector<const slang::ast::Symbol*> disabledFromOutside;
    int of(const slang::ast::Symbol* s) {
        auto [it, fresh] = id.try_emplace(s, (int)id.size());
        return it->second;
    }
};
inline BlockIds& blockIds() { static BlockIds b; return b; }
// Bits a value of this type takes in a signal: a string is a 64-bit id (6.16).
inline int storeW(const slang::ast::Type& t) { return t.isString() ? 64 : (int)t.getBitWidth(); }
inline uint64_t pow10u(int n) { uint64_t r = 1; while (n-- > 0) r *= 10; return r; }

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
    std::vector<LoweredProc> finals;  // final blocks (9.2.3), run once at the end
    int precExp = -9;                 // one kernel tick = 10^precExp s
    // The tick as a VCD $timescale ("1ns", "100ps").
    std::string timescale() const {
        static const char* u[] = {"s", "ms", "us", "ns", "ps", "fs"};
        int e = precExp, m = 1;
        while (e % 3) { e--; m *= 10; }  // e.g. -10 = 100ps
        int k = -e / 3;
        if (k < 0) k = 0;
        if (k > 5) k = 5;
        return std::to_string(m) + u[k];
    }
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
    // Named sequential blocks around the statement being lowered, each with
    // the jumps a `disable` of it makes to its end.
    std::vector<std::pair<const slang::ast::Symbol*, std::vector<size_t>>> named;
    // A statement that is one named begin...end block.
    static const slang::ast::Symbol* namedSeq(const slang::ast::Statement& st) {
        using namespace slang::ast;
        if (st.kind != StatementKind::Block) return nullptr;
        const auto& b = st.as<BlockStatement>();
        if (b.blockKind != StatementBlockKind::Sequential || !b.blockSymbol || b.blockSymbol->name.empty()) return nullptr;
        return b.blockSymbol;
    }
    static int rootId(const slang::ast::Symbol* s) {
        int id = blockIds().of(s);
        blockIds().roots.insert(id);
        return id;
    }
    // The event signal an event expression names (e, sub.e).
    int eventSig(const slang::ast::Expression& x) {
        using namespace slang::ast;
        if (!ValueExpressionBase::isKind(x.kind)) unsupported("event expression");
        auto it = sigmap.find(&x.as<ValueExpressionBase>().symbol);
        if (it == sigmap.end()) unsupported("event expression");
        return it->second;
    }
    std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* subs = nullptr;
    MkSig mkSig;
    int* tempSeq = nullptr;
    // The enclosing module's time unit and precision (powers of ten).
    int tsUnit = -9, tsPrec = -9;
    void setScope(const slang::ast::Scope* sc) {
        if (!sc) return;
        if (auto ts = sc->getTimeScale()) { tsUnit = tsExp(ts->base); tsPrec = tsExp(ts->precision); }
    }
    uint64_t ticksPerUnit() const { return pow10u(tsUnit - simPrecExp()); }
    // A delay in this module's units, as kernel ticks: rounded to the module's
    // precision first (22.7), then scaled to the simulation precision.
    uint64_t ticksOf(double units) const {
        double inPrec = units * (double)pow10u(tsUnit - tsPrec);
        double r = inPrec < 0 ? 0 : std::floor(inPrec + 0.5);
        return (uint64_t)r * pow10u(tsPrec - simPrecExp());
    }
    // #d: a constant becomes Instr::delay; anything else is an expression
    // evaluated when the statement runs (9.4.1). x or z waits zero time.
    void delayInto(const slang::ast::Expression& d, Instr& i) {
        using namespace slang::ast;
        const Expression& x = peel(d);
        if (x.kind == ExpressionKind::RealLiteral) { i.delay = ticksOf(x.as<RealLiteral>().getValue()); return; }
        if (x.kind == ExpressionKind::TimeLiteral) { i.delay = ticksOf(x.as<TimeLiteral>().getValue()); return; }
        if (const auto* cv = d.getConstant()) {
            if (cv->isReal()) { i.delay = ticksOf(cv->real()); return; }
            if (cv->isInteger() && !cv->integer().hasUnknown()) {
                auto v = cv->integer().as<int64_t>();
                if (v) { i.delay = ticksOf((double)*v); return; }
            }
        }
        if (x.kind == ExpressionKind::IntegerLiteral && !x.as<IntegerLiteral>().getValue().hasUnknown()) {
            i.delay = ticksOf((double)*x.as<IntegerLiteral>().getValue().as<uint64_t>());
            return;
        }
        if (!d.type->isIntegral()) unsupported("non-integral delay expression");
        E v = resize(expr(d), 64, false, false);
        i.de = bin(Op::Mul, v, konst(mk(ticksPerUnit(), 64)));
    }
    int retSig = -1;     // function result signal while lowering a function body
    Subr* current = nullptr;
    // Set while lowering an output/inout write-back. slang's actual is an
    // assignment whose right-hand side is an EmptyArgument (the formal),
    // possibly wrapped in a conversion to the actual's type.
    struct OutSlot { int sig; int w; bool sgn; };
    const OutSlot* outSlot = nullptr;

    explicit Lowerer(std::unordered_map<const slang::ast::Symbol*, int>& m) : sigmap(m) {}

    // Owns the Prog of each fork branch, so Fork's pointers outlive lower().
    std::vector<std::unique_ptr<Prog>>* store = nullptr;
    void bind(std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* s,
              MkSig mk, int* seq, std::vector<std::unique_ptr<Prog>>* st = nullptr) {
        subs = s;
        mkSig = std::move(mk);
        tempSeq = seq;
        store = st;
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
        if (w <= 0) unsupported("non-positive width");
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
            case BinaryOperator::BinaryXnor:
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
    // real (6.12): a real-typed expression is lowered by exprReal. Its
    // fallback lowers the same expression as plain bits once (rawNext).
    bool rawNext = false;
    static E realK(double d) { return konst(mkreal(d)); }
    E realOf(const slang::ast::Expression& e) {   // any expression, as a real
        if (e.type->isFloating()) return exprReal(e);
        return un(Op::IToR, expr(e));
    }
    E fcmp(int kind, E a, E b) { auto n = node(Op::FCmp, {std::move(a), std::move(b)}); n->hi = kind; return n; }
    E rtoi(E a, int w, bool sgn, bool trunc) {
        auto n = node(Op::RToI, {std::move(a)});
        n->hi = w; n->sgn = sgn; n->lo = trunc ? 1 : 0;
        return n;
    }
    static int mathId(std::string_view n) {
        static const std::pair<const char*, int> t[] = {
            {"$ln", M_LN}, {"$log10", M_LOG10}, {"$exp", M_EXP}, {"$sqrt", M_SQRT}, {"$pow", M_POW},
            {"$floor", M_FLOOR}, {"$ceil", M_CEIL}, {"$sin", M_SIN}, {"$cos", M_COS}, {"$tan", M_TAN},
            {"$asin", M_ASIN}, {"$acos", M_ACOS}, {"$atan", M_ATAN}, {"$atan2", M_ATAN2},
            {"$hypot", M_HYPOT}, {"$sinh", M_SINH}, {"$cosh", M_COSH}, {"$tanh", M_TANH},
            {"$asinh", M_ASINH}, {"$acosh", M_ACOSH}, {"$atanh", M_ATANH}};
        for (auto& [s, id] : t) if (n == s) return id;
        return -1;
    }
    E exprReal(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (e.type->getBitWidth() != 64) unsupported("shortreal");
        switch (e.kind) {
            case ExpressionKind::RealLiteral: return realK(e.as<RealLiteral>().getValue());
            case ExpressionKind::TimeLiteral: return realK(e.as<TimeLiteral>().getValue());
            case ExpressionKind::Conversion: {
                const Expression& op = e.as<ConversionExpression>().operand();
                return realOf(op);
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                Op op;
                switch (b.op) {
                    case BinaryOperator::Add: op = Op::FAdd; break;
                    case BinaryOperator::Subtract: op = Op::FSub; break;
                    case BinaryOperator::Multiply: op = Op::FMul; break;
                    case BinaryOperator::Divide: op = Op::FDiv; break;
                    case BinaryOperator::Power: op = Op::FPow; break;
                    default: unsupported("operator on real operands");
                }
                return bin(op, realOf(b.left()), realOf(b.right()));
            }
            case ExpressionKind::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                if (u.op == UnaryOperator::Minus) return un(Op::FNeg, realOf(u.operand()));
                if (u.op == UnaryOperator::Plus) return realOf(u.operand());
                break;
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1) unsupported("pattern conditional");
                return mux(expr(*c.conditions[0].expr), realOf(c.left()), realOf(c.right()));
            }
            case ExpressionKind::Call: {
                const auto& call = e.as<CallExpression>();
                if (!call.isSystemCall()) break;
                std::string_view n = call.getSubroutineName();
                auto args = call.arguments();
                if (isStrMethodCall(call)) return strMethod(call, strMethodId(n));   // s.atoreal()
                if (n == "$realtime") { E t = node(Op::RealTime); t->k = mk(ticksPerUnit(), 64); return t; }
                if (n == "$itor" && args.size() == 1) return un(Op::IToR, expr(*args[0]));
                if (n == "$bitstoreal" && args.size() == 1) return un(Op::AsReal, expr(*args[0]));
                if (int id = mathId(n); id >= 0) {
                    std::vector<E> ks;
                    for (auto* a : args) ks.push_back(realOf(*a));
                    auto m = node(Op::FMath, std::move(ks));
                    m->hi = id;
                    return m;
                }
                break;
            }
            default: break;
        }
        rawNext = true;   // a real variable, array element, function result...
        return un(Op::AsReal, expr(e));
    }

    // string (6.16): a string-typed expression is lowered by exprStr.
    E strOf(const slang::ast::Expression& e) {   // a string, or an integral value as one
        if (e.type->isString()) return exprStr(e);
        return un(Op::IntToStr, expr(e));
    }
    static int strMethodId(std::string_view n) {
        static const std::pair<const char*, int> t[] = {
            {"len", S_LEN}, {"getc", S_GETC}, {"toupper", S_TOUPPER}, {"tolower", S_TOLOWER},
            {"compare", S_COMPARE}, {"icompare", S_ICOMPARE}, {"substr", S_SUBSTR}, {"atoi", S_ATOI},
            {"atohex", S_ATOHEX}, {"atooct", S_ATOOCT}, {"atobin", S_ATOBIN}, {"atoreal", S_ATOREAL},
            {"putc", S_PUTC}, {"itoa", S_ITOA}, {"hextoa", S_HEXTOA}, {"octtoa", S_OCTTOA},
            {"bintoa", S_BINTOA}, {"realtoa", S_REALTOA}};
        for (auto& [s, id] : t) if (n == s) return id;
        return -1;
    }
    // s.len(), s.substr(i, j), ...: kid 0 is the string, then the arguments.
    E strMethod(const slang::ast::CallExpression& call, int id) {
        auto args = call.arguments();
        std::vector<E> ks;
        ks.push_back(exprStr(*args[0]));
        for (size_t a = 1; a < args.size(); a++) {
            if (id == S_COMPARE || id == S_ICOMPARE) ks.push_back(strOf(*args[a]));
            else if (id == S_REALTOA) ks.push_back(realOf(*args[a]));
            else ks.push_back(expr(*args[a]));
        }
        auto m = node(Op::StrMethod, std::move(ks));
        m->hi = id;
        return m;
    }
    bool isStrMethodCall(const slang::ast::CallExpression& call) {
        return call.isSystemCall() && !call.arguments().empty() && call.arguments()[0]->type->isString() &&
               strMethodId(call.getSubroutineName()) >= 0 && call.getSubroutineName()[0] != '$';
    }
    E exprStr(const slang::ast::Expression& e) {
        using namespace slang::ast;
        switch (e.kind) {
            case ExpressionKind::StringLiteral:
                return konst(mkstr(std::string(e.as<StringLiteral>().getValue())));
            case ExpressionKind::Conversion: return strOf(e.as<ConversionExpression>().operand());
            case ExpressionKind::Concatenation: {
                std::vector<E> parts;
                for (auto* op : e.as<ConcatenationExpression>().operands()) parts.push_back(strOf(*op));
                return node(Op::StrCat, std::move(parts));
            }
            case ExpressionKind::Replication: {
                const auto& r = e.as<ReplicationExpression>();
                auto n = node(Op::StrRep, {strOf(r.concat())});
                n->hi = (int)constInt(r.count());
                return n;
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1) unsupported("pattern conditional");
                return mux(expr(*c.conditions[0].expr), strOf(c.left()), strOf(c.right()));
            }
            case ExpressionKind::Call: {
                const auto& call = e.as<CallExpression>();
                if (!call.isSystemCall()) break;
                std::string_view n = call.getSubroutineName();
                if (n == "$sformatf" || n == "$psprintf") {
                    auto args = call.arguments();
                    auto f = node(Op::SFormat);
                    formatInto(args, 'd', f->fmt, f->kids);
                    return f;
                }
                if (isStrMethodCall(call)) return strMethod(call, strMethodId(n));
                break;
            }
            default: break;
        }
        rawNext = true;   // a string variable, array element, function result...
        return un(Op::AsStr, expr(e));
    }

    E expr(const slang::ast::Expression& e, ExprCtx ctx = {}) {
        using namespace slang::ast;
        bool raw = rawNext;
        rawNext = false;
        if (!raw && e.type->isFloating()) return exprReal(e);
        if (!raw && e.type->isString()) return exprStr(e);
        switch (e.kind) {
            case ExpressionKind::IntegerLiteral:
                return svLit(e.as<IntegerLiteral>().getValue(), e.type->isSigned(), ctx);
            case ExpressionKind::StringLiteral: {   // "abc" is 8 bits per char (5.9)
                const auto& sv = e.as<StringLiteral>().getIntValue().integer();
                if (sv.getBitWidth() > 64) unsupported("string literal longer than 8 characters");
                return svLit(sv, false, ctx);
            }
            case ExpressionKind::Replication: {     // {n{a}} (11.4.12.1)
                const auto& r = e.as<ReplicationExpression>();
                int64_t n = constInt(r.count());
                if (n < 0) unsupported("replication count");
                if (n == 0) unsupported("zero replication");
                E one = expr(r.concat());
                std::vector<E> parts((size_t)n, one);
                return sized(concat(parts), (int)e.type->getBitWidth(), false, ctx);
            }
            default: break;
        }
        return expr2(e, ctx);
    }

    // A constant from slang's 4-state integer, at its own width and sign.
    E svLit(const slang::SVInt& sv, bool self, ExprCtx ctx) {
        int w = (int)sv.getBitWidth();
        if (w <= 0) unsupported("constant");
        E lit;
        if (w <= 64) {
            if (sv.hasUnknown()) {   // x / z / ? bits
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
                lit = konst(make(sv.getRawPtr()[0], 0, w, 0, self));
            }
            return sized(lit, w, self, ctx);
        }
        // Wider than one limb. A known value is little-endian words. An
        // unknown value is walked a bit at a time: slang stores Z as a 1 in
        // the value word and a 1 in the unknown word, X as a 0 and a 1.
        Val bits = make(0, 0, w, 0, self);
        if (!sv.hasUnknown()) {
            bits = wordsToVal(w, self, sv.getRawPtr(), (w + 63) / 64);
        } else {
            for (int b = 0; b < w; b++) {
                auto bit = sv[b];
                if (bit.isUnknown())
                    setBit(bits, b, bit.value == slang::logic_t::Z_VALUE ? 3 : 2);
                else if (bit.value)
                    setBit(bits, b, 1);
            }
            bits = finish(bits);
            bits.sgn = self;
        }
        return sized(konst(bits), w, self, ctx);
    }

    // a[i][j] on an unpacked array: the array and one index per dimension.
    // Fewer indices than dimensions (a slice) is not supported yet.
    struct ArrSel {
        std::shared_ptr<const ArrShape> a;
        std::vector<const slang::ast::Expression*> idx;
        bool constant = true;
    };
    std::optional<ArrSel> arraySel(const slang::ast::Expression& e) {
        using namespace slang::ast;
        std::vector<const Expression*> idx;
        const Expression* cur = &e;
        while (cur->kind == ExpressionKind::ElementSelect &&
               cur->as<ElementSelectExpression>().value().type->isUnpackedArray()) {
            idx.push_back(&cur->as<ElementSelectExpression>().selector());
            cur = &cur->as<ElementSelectExpression>().value();
        }
        if (idx.empty()) return std::nullopt;
        if (cur->kind != ExpressionKind::NamedValue) unsupported("array select on a non-variable");
        auto it = arrMap().find(&cur->as<NamedValueExpression>().symbol);
        if (it == arrMap().end()) unsupported("array select");
        ArrSel s;
        s.a = it->second;
        s.idx.assign(idx.rbegin(), idx.rend());
        if (s.idx.size() != s.a->dims.size()) unsupported("array slice (fewer indices than dimensions)");
        for (auto* x : s.idx)
            if (!(x->kind == ExpressionKind::IntegerLiteral || x->getConstant())) s.constant = false;
        return s;
    }
    int64_t constOffset(const ArrSel& s) {
        std::vector<int64_t> v;
        for (auto* x : s.idx) v.push_back(constInt(*x));
        return s.a->offset(v);
    }
    E arrayRead(const ArrSel& s) {
        if (s.constant) {
            int64_t off = constOffset(s);
            if (off >= 0) return ref(s.a->base + (int)off, s.a->sgn);
        }
        auto n = node(Op::ArrRead);
        n->arr = s.a;
        for (auto* x : s.idx) n->kids.push_back(expr(*x));
        return n;
    }

    // A select of a packed vector (11.5.1, 7.4.1), as a bit range of the
    // signal. Indices are in declared numbering: [7:0], [8:1], or [0:7] all
    // work, and an element of a packed array (logic [3:0][7:0] x; x[1]) is
    // its whole element. Either the start bit is a constant (cbit) or it is
    // computed at run time (vbit); a part outside the vector reads x.
    struct PSel { enum Kind { Index, Up, Down, Range }; bool known = true; int64_t cbit = 0; E vbit; int bits = 1; };
    PSel packedSel(const slang::ast::Type& vt, const slang::ast::Expression* idx,
                   const slang::ast::Expression* idx2, int64_t count, PSel::Kind kind) {
        using namespace slang::ast;
        int W = (int)vt.getBitWidth();
        int64_t left = W - 1, right = 0;
        if (vt.hasFixedRange()) { auto fr = vt.getFixedRange(); left = fr.left; right = fr.right; }
        bool desc = left >= right;
        int64_t n = (desc ? left - right : right - left) + 1;
        int ew = n > 0 ? (int)(W / n) : 1;
        PSel ps;
        auto isConst = [](const Expression* x) { return x->kind == ExpressionKind::IntegerLiteral || x->getConstant(); };
        // first declared index covered by the select, lowest position first
        auto startPos = [&](int64_t i) -> int64_t {
            switch (kind) {
                case PSel::Index: return desc ? i - right : right - i;
                case PSel::Up:    return desc ? i - right : right - i - count + 1;
                case PSel::Down:  return desc ? i - count + 1 - right : right - i;
                case PSel::Range: return 0;
            }
            return 0;
        };
        if (kind == PSel::Range) {   // a[x:y], both constant
            int64_t x = constInt(*idx), y = constInt(*idx2);
            int64_t lo = desc ? std::min(x, y) - right : right - std::max(x, y);
            ps.cbit = lo * ew;
            ps.bits = (int)((x > y ? x - y : y - x) + 1) * ew;
            return ps;
        }
        ps.bits = (int)count * ew;
        if (isConst(idx)) { ps.cbit = startPos(constInt(*idx)) * ew; return ps; }
        ps.known = false;
        E ix = resize(expr(*idx), 64, idx->type->isSigned(), true);
        auto K = [](int64_t v) { return konst(make((uint64_t)v, 0, 64, 0, true)); };
        E pos;
        switch (kind) {
            case PSel::Index: pos = desc ? bin(Op::Sub, ix, K(right)) : bin(Op::Sub, K(right), ix); break;
            case PSel::Up:    pos = desc ? bin(Op::Sub, ix, K(right)) : bin(Op::Sub, K(right - count + 1), ix); break;
            case PSel::Down:  pos = desc ? bin(Op::Sub, ix, K(count - 1 + right)) : bin(Op::Sub, K(right), ix); break;
            default: break;
        }
        ps.vbit = ew == 1 ? pos : bin(Op::Mul, pos, K(ew));
        return ps;
    }
    PSel rangePSel(const slang::ast::RangeSelectExpression& r) {
        using namespace slang::ast;
        const Type& vt = *r.value().type;
        if (r.getSelectionKind() == RangeSelectionKind::Simple)
            return packedSel(vt, &r.left(), &r.right(), 0, PSel::Range);
        bool down = r.getSelectionKind() == RangeSelectionKind::IndexedDown;
        return packedSel(vt, &r.left(), nullptr, constInt(r.right()), down ? PSel::Down : PSel::Up);
    }
    // s.f on a packed struct or union (7.2.1, 7.3.1): a constant slice.
    PSel memberPSel(const slang::ast::MemberAccessExpression& m) {
        using namespace slang::ast;
        if (!m.value().type->isIntegral())
            unsupported("member of an unpacked struct or a class");
        if (m.member.kind != SymbolKind::Field) unsupported("member access");
        const auto& f = m.member.as<FieldSymbol>();
        PSel ps;
        ps.cbit = (int64_t)f.bitOffset;
        ps.bits = (int)f.getType().getBitWidth();
        return ps;
    }
    E readPSel(E base, const PSel& ps) {
        // A constant select is a slice even when the bits sit above bit 63.
        // v_slice reads a bit past the vector as X.
        if (ps.known && ps.cbit >= 0 && ps.bits > 0)
            return slice(std::move(base), (int)(ps.cbit + ps.bits - 1), (int)ps.cbit);
        E start = ps.known ? konst(make((uint64_t)ps.cbit, 0, 64, 0, true)) : ps.vbit;
        auto n = node(Op::DynSlice, {std::move(base), start});
        n->hi = ps.bits;
        n->lo = 0;
        return n;
    }
    // The same select as an assignment target. When the target already has a
    // select (p[3][7:4] = ...), this one is inside it: offsets add up.
    void writePSel(Instr& i, const PSel& ps) {
        bool inner = i.hi >= 0 || i.bidx;
        if (inner) {
            auto K = [](int64_t v) { return konst(make((uint64_t)v, 0, 64, 0, true)); };
            if (!i.bidx && ps.known) {
                if (ps.cbit < 0 || ps.cbit + ps.bits > i.hi - i.lo + 1) unsupported("constant select outside the element");
                i.lo += (int)ps.cbit;
                i.hi = i.lo + ps.bits - 1;
                return;
            }
            E outer = ps.known ? K(ps.cbit) : ps.vbit;
            E base = i.bidx ? i.bidx : K(i.lo);
            i.bidx = bin(Op::Add, base, outer);
            i.pw = ps.bits;
            i.pdown = false;
            i.hi = 0;
            i.lo = 0;
            return;
        }
        if (ps.known) {
            if (ps.cbit < 0) unsupported("constant select outside the vector");
            i.lo = (int)ps.cbit;
            i.hi = (int)(ps.cbit + ps.bits - 1);
            return;
        }
        i.bidx = ps.vbit;
        i.pw = ps.bits;
        i.pdown = false;
        i.hi = 0;
        i.lo = 0;
    }

    // A parameter, localparam, or enum value (6.20, 6.19.3) is its constant.
    E constSym(const slang::ast::Symbol& sym, const slang::ast::Expression& e, ExprCtx ctx) {
        using namespace slang::ast;
        const slang::ConstantValue* cv = nullptr;
        if (sym.kind == SymbolKind::Parameter) cv = &sym.as<ParameterSymbol>().getValue();
        else if (sym.kind == SymbolKind::EnumValue) cv = &sym.as<EnumValueSymbol>().getValue();
        else if (const auto* c = e.getConstant()) cv = c;
        if (!cv || !cv->isInteger()) unsupported("reference to non-signal symbol");
        return svLit(cv->integer(), e.type->isSigned(), ctx);
    }

    E expr2(const slang::ast::Expression& e, ExprCtx ctx) {
        using namespace slang::ast;
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {   // x, or u.x / blk[2].x (23.6)
                const Symbol* sym = &e.as<ValueExpressionBase>().symbol;
                auto it = sigmap.find(sym);
                if (it == sigmap.end()) return constSym(*sym, e, ctx);
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
                if (c.operand().type->isString() && c.type->isIntegral()) {   // the text as bytes (6.16)
                    int w = (int)c.type->getBitWidth();
                    needWidth(w);
                    auto n = node(Op::StrToInt, {exprStr(c.operand())});
                    n->hi = w;
                    return sized(n, w, c.type->isSigned(), ctx);
                }
                if (c.operand().type->isFloating() && c.type->isIntegral()) {   // 6.12.1: rounded
                    int w = (int)c.type->getBitWidth();
                    needWidth(w);
                    return sized(rtoi(exprReal(c.operand()), w, c.type->isSigned(), false), w, c.type->isSigned(), ctx);
                }
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
                    int w = storeW(*e.type);
                    needWidth(w);
                    return sized(std::move(v), w, e.type->isSigned(), ctx);
                }
                std::string_view n = call.getSubroutineName();
                // $time / $stime / $realtime (20.3.1): the current time. minisim
                // has one time unit, so $realtime is the same integer.
                if (n == "$time" || n == "$stime") {
                    // In the calling module's time unit (20.3.1), rounded.
                    int w = n == "$stime" ? 32 : 64;
                    E t = node(Op::Time);
                    t->k = mk(ticksPerUnit(), 64);
                    if (w != 64) t = resize(std::move(t), w, false, false);
                    return sized(std::move(t), w, false, ctx);
                }
                if (isStrMethodCall(call)) {   // s.len(), s.getc(i), s.compare(t), s.atoi() ...
                    int w = (int)e.type->getBitWidth();
                    needWidth(w);
                    return sized(strMethod(call, strMethodId(n)), w, e.type->isSigned(), ctx);
                }
                if ((n == "$rtoi" || n == "$realtobits") && call.arguments().size() == 1) {
                    E r = realOf(*call.arguments()[0]);
                    int w = (int)e.type->getBitWidth();
                    needWidth(w);
                    if (n == "$rtoi") return sized(rtoi(r, w, e.type->isSigned(), true), w, e.type->isSigned(), ctx);
                    auto bits = node(Op::Resize, {r});   // the same 64 bits, as an integer
                    bits->hi = 64;
                    return sized(bits, 64, false, ctx);
                }
                // e.triggered (15.5.3): e was triggered in this time slot.
                if (n == "triggered" && call.arguments().size() == 1) {
                    int s = eventSig(*call.arguments()[0]);
                    E t = node(Op::Time);
                    t->k = mk(1, 64);   // raw ticks
                    E v = bin(Op::Eq, ref(s + 1), bin(Op::Add, t, konst(mk(1, 64))));
                    return sized(std::move(v), 1, false, ctx);
                }
                // $signed / $unsigned keep the bits and change the tag.
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
                if (u.op == UnaryOperator::Preincrement || u.op == UnaryOperator::Postincrement ||
                    u.op == UnaryOperator::Predecrement || u.op == UnaryOperator::Postdecrement) {
                    // ++ / -- inside an expression (11.4.2): the update runs
                    // just before the statement, and the expression reads a
                    // copy (the old value for i++, the new one for ++i).
                    if (!stmtDepth) unsupported("++ / -- outside a procedural statement");
                    int w = (int)u.operand().type->getBitWidth();
                    needWidth(w);
                    bool post = u.op == UnaryOperator::Postincrement || u.op == UnaryOperator::Postdecrement;
                    int tmp = fresh(w);
                    Instr save; save.k = IK::Assign; save.sig = tmp; save.e = expr(u.operand());
                    if (post) prog.emit(save);
                    exprStmt(e);
                    if (!post) { save.e = expr(u.operand()); prog.emit(save); }
                    return sized(ref(tmp, u.operand().type->isSigned()), w, u.operand().type->isSigned(), ctx);
                }
                // ~a, -a, +a are context-determined (11.6.1): the operand is
                // extended to the context width before the operator.
                bool bw = u.op == UnaryOperator::BitwiseNot || u.op == UnaryOperator::Minus ||
                          u.op == UnaryOperator::Plus;
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
                    case UnaryOperator::Plus: out = a; break;
                    case UnaryOperator::Minus: {   // 0 - a at a's width and sign
                        Val zero = mk(0, ctx.live && ctx.w > 0 ? ctx.w : (int)e.type->getBitWidth());
                        zero.sgn = e.type->isSigned();
                        out = bin(Op::Sub, konst(zero), a);
                        break;
                    }
                    case UnaryOperator::BitwiseNand: out = un(Op::LogNot, un(Op::RedAnd, a)); break;
                    case UnaryOperator::BitwiseNor: out = un(Op::LogNot, un(Op::RedOr, a)); break;
                    case UnaryOperator::BitwiseXnor: out = un(Op::Not, un(Op::RedXor, a)); break;
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
                if (b.left().type->isString() || b.right().type->isString()) {   // 6.16: string compare
                    int kind = -1;
                    switch (b.op) {
                        case BinaryOperator::LessThan: kind = 0; break;
                        case BinaryOperator::LessThanEqual: kind = 1; break;
                        case BinaryOperator::GreaterThan: kind = 2; break;
                        case BinaryOperator::GreaterThanEqual: kind = 3; break;
                        case BinaryOperator::Equality: case BinaryOperator::CaseEquality: kind = 4; break;
                        case BinaryOperator::Inequality: case BinaryOperator::CaseInequality: kind = 5; break;
                        default: unsupported("operator on string operands");
                    }
                    auto n = node(Op::StrCmp, {strOf(b.left()), strOf(b.right())});
                    n->hi = kind;
                    return sized(n, 1, false, ctx);
                }
                if (b.left().type->isFloating() || b.right().type->isFloating()) {
                    // A comparison or logical operator on reals: a 1-bit result.
                    auto truthOf = [&](const Expression& x) {
                        return x.type->isFloating() ? fcmp(5, exprReal(x), realK(0)) : expr(x);
                    };
                    E out;
                    switch (b.op) {
                        case BinaryOperator::LessThan: out = fcmp(0, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::LessThanEqual: out = fcmp(1, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::GreaterThan: out = fcmp(2, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::GreaterThanEqual: out = fcmp(3, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::Equality: out = fcmp(4, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::Inequality: out = fcmp(5, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::LogicalAnd: out = bin(Op::LogAnd, truthOf(b.left()), truthOf(b.right())); break;
                        case BinaryOperator::LogicalOr: out = bin(Op::LogOr, truthOf(b.left()), truthOf(b.right())); break;
                        default: unsupported("operator on real operands");
                    }
                    return sized(out, 1, false, ctx);
                }
                bool sizeBoth = widthPropagates(b.op) || isDivMod(b.op);
                // a ** b sizes like a shift: the result is a's width, b is
                // self-determined (11.6.1).
                bool shift = isShift(b.op) || b.op == BinaryOperator::Power;
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
                    case BinaryOperator::Power: kop = Op::Pow; break;
                    case BinaryOperator::CaseInequality: kop = Op::CaseEq; break;
                    case BinaryOperator::WildcardEquality: kop = Op::WildEq; break;
                    case BinaryOperator::WildcardInequality: kop = Op::WildEq; break;
                    case BinaryOperator::BinaryXnor: kop = Op::Xor; break;
                    case BinaryOperator::LogicalImplication: kop = Op::LogOr; l = un(Op::LogNot, l); break;
                    case BinaryOperator::LogicalEquivalence: kop = Op::Eq; l = un(Op::RedOr, l); r = un(Op::RedOr, r); break;
                    default: unsupported("binary operator");
                }
                E out = bin(kop, l, r);
                if (b.op == BinaryOperator::CaseInequality || b.op == BinaryOperator::WildcardInequality)
                    out = un(Op::LogNot, out);
                if (b.op == BinaryOperator::BinaryXnor) out = un(Op::Not, out);
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
                if (auto as = arraySel(e))
                    return sized(arrayRead(*as), as->a->elemW, as->a->sgn, ctx);
                const auto& sel = e.as<ElementSelectExpression>();
                PSel ps = packedSel(*sel.value().type, &sel.selector(), nullptr, 1, PSel::Index);
                return sized(readPSel(expr(sel.value()), ps), (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
            }
            case ExpressionKind::RangeSelect: {     // a[hi:lo], a[i +: w], a[i -: w]
                const auto& r = e.as<RangeSelectExpression>();
                PSel ps = rangePSel(r);
                return sized(readPSel(expr(r.value()), ps), (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
            }
            case ExpressionKind::Assignment: {      // (a = b) inside an expression (11.3.6)
                if (!stmtDepth) unsupported("assignment inside an expression here");
                const auto& a = e.as<AssignmentExpression>();
                if (a.isNonBlocking() || a.timingControl) unsupported("assignment inside an expression");
                exprStmt(e);
                return expr(a.left(), ctx);
            }
            case ExpressionKind::Inside: {          // x inside {a, [lo:hi]} (11.4.13)
                const auto& in = e.as<InsideExpression>();
                E acc;
                auto pair = [&](const Expression& x, const Expression& y) {
                    int w = std::max((int)x.type->getBitWidth(), (int)y.type->getBitWidth());
                    needWidth(w);
                    bool sg = x.type->isSigned() && y.type->isSigned();
                    ExprCtx c{w, sg, sg, true};
                    return std::pair<E, E>{expr(x, c), expr(y, c)};
                };
                for (const Expression* it : in.rangeList()) {
                    E m;
                    if (it->kind == ExpressionKind::ValueRange) {
                        const auto& vr = it->as<ValueRangeExpression>();
                        if (vr.rangeKind != ValueRangeKind::Simple) unsupported("tolerance range in inside");
                        auto [l1, lo] = pair(in.left(), vr.left());
                        auto [l2, hi] = pair(in.left(), vr.right());
                        m = bin(Op::LogAnd, bin(Op::Ge, l1, lo), bin(Op::Le, l2, hi));
                    } else {
                        if (!it->type->isIntegral()) unsupported("inside on a non-integral item");
                        auto [l, r] = pair(in.left(), *it);
                        m = bin(Op::WildEq, l, r);
                    }
                    acc = acc ? bin(Op::LogOr, acc, m) : m;
                }
                return sized(acc ? acc : konst(mk(0, 1)), 1, false, ctx);
            }
            case ExpressionKind::MemberAccess: {    // s.f
                const auto& m = e.as<MemberAccessExpression>();
                return sized(readPSel(expr(m.value()), memberPSel(m)), (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
            }
            case ExpressionKind::SimpleAssignmentPattern:
            case ExpressionKind::StructuredAssignmentPattern:
            case ExpressionKind::ReplicatedAssignmentPattern: {   // '{a, b}, '{f: x}, '{n{a}} (10.9)
                if (!e.type->isIntegral()) unsupported("assignment pattern of an unpacked type");
                const auto& ap = static_cast<const AssignmentPatternExpressionBase&>(e);
                std::vector<E> parts;
                for (auto* el : ap.elements()) parts.push_back(expr(*el));
                if (e.kind == ExpressionKind::ReplicatedAssignmentPattern) {
                    int64_t n = constInt(e.as<ReplicatedAssignmentPatternExpression>().count());
                    std::vector<E> rep;
                    for (int64_t k = 0; k < n; k++) rep.insert(rep.end(), parts.begin(), parts.end());
                    parts = std::move(rep);
                }
                return sized(concat(parts), (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
            }
            default:
                throw std::runtime_error("lower: unsupported expression kind " + std::string(toString(e.kind)));
        }
    }

    // Fill i.sig (+ i.hi/i.lo for partial writes) from an assignment target.
    void lhs(const slang::ast::Expression& e, Instr& i) {
        using namespace slang::ast;
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {
                auto it = sigmap.find(&e.as<ValueExpressionBase>().symbol);
                if (it == sigmap.end()) unsupported("assignment to a non-signal");
                i.sig = it->second;
                return;
            }
            case ExpressionKind::ElementSelect: {
                if (auto as = arraySel(e)) {   // mem[i] = ...
                    int64_t off = as->constant ? constOffset(*as) : -1;
                    if (off >= 0) { i.sig = as->a->base + (int)off; return; }
                    i.sig = as->a->base;
                    i.arr = as->a;
                    for (auto* x : as->idx) i.aidx.push_back(expr(*x));
                    return;
                }
                const auto& sel = e.as<ElementSelectExpression>();
                lhs(sel.value(), i);
                writePSel(i, packedSel(*sel.value().type, &sel.selector(), nullptr, 1, PSel::Index));
                return;
            }
            case ExpressionKind::RangeSelect: {   // v[7:4] = ..., v[i +: w] = ... (11.5.1)
                const auto& r = e.as<RangeSelectExpression>();
                lhs(r.value(), i);
                writePSel(i, rangePSel(r));
                return;
            }
            case ExpressionKind::MemberAccess: {   // s.f = ...
                const auto& m = e.as<MemberAccessExpression>();
                lhs(m.value(), i);
                writePSel(i, memberPSel(m));
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
            if (!ValueExpressionBase::isKind(s.expr.kind)) unsupported("event on non-signal");
            int sig = sigmap.at(&s.expr.as<ValueExpressionBase>().symbol);
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
                Instr i; i.k = IK::WaitDelay;
                delayInto(d.expr, i);
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

    // Each string-literal argument is format text; any other argument
    // fills the next specifier, or prints with the default if none is
    // waiting ($display(a, b) prints both in decimal).
    template <typename Args>
    void formatInto(const Args& argv, char dflt, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        int pending = 0;
        auto countSpecs = [](const std::string& f) {
            int c = 0;
            for (size_t p = 0; p < f.size(); p++) {
                if (f[p] != '%') continue;
                if (p + 1 < f.size() && f[p + 1] == '%') { p++; continue; }
                size_t q = p + 1;
                while (q < f.size() && (isdigit((unsigned char)f[q]) || f[q] == '.' || f[q] == '-')) q++;
                if (q < f.size() && f[q] != 'm' && f[q] != 'M') c++;
            }
            return c;
        };
        for (const Expression* a : argv) {
            if (a->kind == ExpressionKind::EmptyArgument) { fmt += ' '; continue; }
            const Expression& pe = peel(*a);
            if (pe.kind == ExpressionKind::StringLiteral && pending == 0) {
                std::string f(pe.as<StringLiteral>().getValue());
                fmt += f;
                pending = countSpecs(f);
                continue;
            }
            if (pending > 0) pending--;
            else { fmt += '%'; fmt += a->type->isFloating() ? 'R' : a->type->isString() ? 's' : dflt; }
            out.push_back(expr(*a));
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
        if (isStrMethodCall(call)) {   // s.putc(i, c), s.itoa(v), ...: s gets the new string
            int id = strMethodId(n);
            if (id < S_PUTC) return;   // a value method used as a statement: no effect
            Instr i; i.k = IK::Assign;
            lhs(*call.arguments()[0], i);
            i.e = strMethod(call, id);
            prog.emit(i);
            return;
        }
        if (n == "$sformat" || n == "$swrite") {   // $sformat(s, fmt, ...) (21.3.3)
            auto args = call.arguments();
            if (args.empty()) unsupported("$sformat");
            const Expression* dst = args[0];
            if (dst->kind == ExpressionKind::Assignment) dst = &dst->as<AssignmentExpression>().left();
            Instr i; i.k = IK::Assign;
            lhs(*dst, i);
            auto f = node(Op::SFormat);
            formatInto(args.subspan(1), 'd', f->fmt, f->kids);
            i.e = dst->type->isString() ? f : E(node(Op::StrToInt, {f}));
            if (!dst->type->isString()) i.e->hi = (int)dst->type->getBitWidth();
            prog.emit(i);
            return;
        }
        if (n == "$readmemh" || n == "$readmemb") {   // 21.4
            auto args = call.arguments();
            if (args.size() < 2) unsupported("$readmem arguments");
            const Expression& fn = peel(*args[0]);
            if (fn.kind != ExpressionKind::StringLiteral) unsupported("$readmem file name that is not a string literal");
            // slang binds the memory as an output argument: an assignment to it.
            const Expression* mp = &peel(*args[1]);
            if (mp->kind == ExpressionKind::Assignment) mp = &peel(mp->as<AssignmentExpression>().left());
            const Expression& mem = *mp;
            const Symbol* msym = nullptr;
            if (mem.kind == ExpressionKind::NamedValue) msym = &mem.as<NamedValueExpression>().symbol;
            else if (mem.kind == ExpressionKind::ArbitrarySymbol) msym = mem.as<ArbitrarySymbolExpression>().symbol;
            if (!msym) unsupported("$readmem target that is not a whole memory");
            auto it = arrMap().find(msym);
            if (it == arrMap().end()) unsupported("$readmem target that is not an unpacked array");
            if (it->second->dims.size() != 1) unsupported("$readmem into a multidimensional array");
            Instr i; i.k = IK::ReadMem;
            i.fmt = std::string(fn.as<StringLiteral>().getValue());
            i.arr = it->second;
            i.hex = n == "$readmemh";
            for (size_t a = 2; a < args.size() && a < 4; a++) i.args.push_back(expr(*args[a]));
            prog.emit(i);
            return;
        }
        // $display / $write and their b / o / h variants (21.2.1). The suffix
        // only changes the default format of an argument with no specifier.
        IK k;
        bool nonl = false;
        char dflt = 'd';
        std::string_view base = n;
        if (base.size() > 1 && (base.ends_with('b') || base.ends_with('o') || base.ends_with('h')) &&
            (base.starts_with("$display") || base.starts_with("$write") || base.starts_with("$strobe") ||
             base.starts_with("$monitor")) &&
            base != "$monitoron") {
            dflt = base.back();
            base.remove_suffix(1);
        }
        if (base == "$display") k = IK::Display;
        else if (base == "$write") { k = IK::Display; nonl = true; }
        else if (base == "$strobe") k = IK::Strobe;
        else if (base == "$monitor") k = IK::Monitor;
        else {
            std::cerr << "lower: ignoring system call " << n << "\n";
            return;
        }
        Instr i; i.k = k; i.nonl = nonl;
        formatInto(call.arguments(), dflt, i.fmt, i.args);
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
            if (const TimingControl* tc = a.timingControl) {   // a = #d b; a <= #d b (9.4.5)
                if (tc->kind != TimingControlKind::Delay) unsupported("intra-assignment event control");
                Instr d;
                delayInto(tc->as<DelayControl>().expr, d);
                if (a.isNonBlocking()) {   // the update is scheduled d later; the process goes on
                    i.delay = d.delay;
                    i.de = d.de;
                    i.nbaLater = true;
                    prog.emit(i);
                    return;
                }
                // Blocking: the value is taken now, written after the wait.
                int w = (int)a.left().type->getBitWidth();
                needWidth(w);
                int tmp = fresh(w);
                Instr hold; hold.k = IK::Assign; hold.sig = tmp; hold.e = i.e;
                prog.emit(hold);
                d.k = IK::WaitDelay;
                prog.emit(d);
                i.e = ref(tmp);
                prog.emit(i);
                return;
            }
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

    int stmtDepth = 0;   // > 0 while lowering a procedural statement
    void stmt(const slang::ast::Statement& s) {
        struct Depth { int& d; Depth(int& x) : d(x) { d++; } ~Depth() { d--; } } depth(stmtDepth);
        using namespace slang::ast;
        switch (s.kind) {
            case StatementKind::Empty: break;
            case StatementKind::List:
                for (auto* st : s.as<StatementList>().list) stmt(*st);
                break;
            case StatementKind::Block: {
                const auto& b = s.as<BlockStatement>();
                if (b.blockKind == StatementBlockKind::Sequential) {
                    if (!b.blockSymbol || b.blockSymbol->name.empty()) { stmt(b.body); break; }
                    size_t k = named.size();
                    named.push_back({b.blockSymbol, {}});
                    stmt(b.body);
                    patch(named[k].second, prog.here());   // disable blk: jump past it
                    named.pop_back();
                    break;
                }
                // fork ... join / join_any / join_none (9.3.2): each statement
                // of the block is its own process. Fork starts them all, then
                // waits for all, for the first, or not at all.
                if (!store) unsupported("fork here");
                Instr f; f.k = IK::Fork;
                f.join = b.blockKind == StatementBlockKind::JoinAll ? 0
                       : b.blockKind == StatementBlockKind::JoinAny ? 1 : 2;
                auto branch = [&](const Statement& st) {
                    Lowerer lw(sigmap);
                    lw.bind(subs, mkSig, tempSeq, store);
                    lw.tsUnit = tsUnit; lw.tsPrec = tsPrec;
                    lw.current = current;
                    lw.retSig = retSig;
                    lw.stmt(st);
                    lw.finishImplicit();
                    // disable <named fork> ends every branch; disable <named
                    // branch block> ends that branch.
                    if (b.blockSymbol && !b.blockSymbol->name.empty()) lw.prog.roots.push_back(rootId(b.blockSymbol));
                    if (const auto* nb = namedSeq(st)) lw.prog.roots.push_back(rootId(nb));
                    store->push_back(std::make_unique<Prog>(std::move(lw.prog)));
                    f.branches.push_back(store->back().get());
                };
                if (b.body.kind == StatementKind::List) {
                    for (const Statement* st : b.body.as<StatementList>().list) {
                        // A declaration in a fork block belongs to the parent.
                        if (st->kind == StatementKind::VariableDeclaration) { stmt(*st); continue; }
                        branch(*st);
                    }
                } else if (b.body.kind != StatementKind::Empty) {
                    branch(b.body);
                }
                prog.emit(f);
                break;
            }
            case StatementKind::Wait: {   // wait (cond) stmt (9.4.3)
                // top: if (cond) goto body; @(signals in cond); goto top
                const auto& w = s.as<WaitStatement>();
                size_t top = prog.here();
                Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                size_t jf = prog.emit(j);
                size_t jbody = emitJmp();
                prog.code[jf].target = prog.here();
                std::vector<int> refs;
                collectRefs(prog.code[jf].e, refs);
                if (refs.empty()) unsupported("wait on a constant");
                std::vector<std::pair<int, Edge>> rs;
                for (int r : refs) rs.push_back({r, ANY});
                prog.emit(waitInstr(rs));
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                prog.code[jbody].target = prog.here();
                stmt(w.stmt);
                break;
            }
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
            case StatementKind::ForeachLoop: {   // foreach (a[i, j]) (12.7.3)
                // One counting loop per named dimension, outermost first, each
                // from its left bound to its right bound. break leaves the whole
                // foreach; continue steps the innermost loop.
                const auto& f = s.as<ForeachLoopStatement>();
                struct Lvl { int var; int64_t right; bool up; size_t top, jf; };
                std::vector<Lvl> lv;
                for (const auto& d : f.loopDims) {
                    if (!d.loopVar) continue;
                    if (!d.range) unsupported("foreach over a dynamic dimension");
                    int v = fresh(32);
                    sigmap[d.loopVar] = v;
                    Instr init; init.k = IK::Assign; init.sig = v;
                    init.e = konst(make((uint64_t)d.range->left, 0, 32, 0, true));
                    prog.emit(init);
                    Lvl L{v, d.range->right, d.range->left <= d.range->right, prog.here(), 0};
                    Instr j; j.k = IK::JmpIfNot;
                    j.e = bin(L.up ? Op::Le : Op::Ge, ref(v, true), konst(make((uint64_t)L.right, 0, 32, 0, true)));
                    L.jf = prog.emit(j);
                    lv.push_back(L);
                }
                loops.push_back({});
                stmt(f.body);
                patch(loops.back().continues, prog.here());
                for (size_t k = lv.size(); k-- > 0;) {
                    const Lvl& L = lv[k];
                    Instr st; st.k = IK::Assign; st.sig = L.var;
                    st.e = bin(L.up ? Op::Add : Op::Sub, ref(L.var, true), konst(make(1, 0, 32, 0, true)));
                    prog.emit(st);
                    Instr back; back.k = IK::Jmp; back.target = L.top; prog.emit(back);
                    // The inner loop's init sits after the outer test, so each
                    // outer pass starts the inner loop at its left bound again.
                    prog.code[L.jf].target = prog.here();
                }
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
            case StatementKind::EventTrigger: {   // -> e, ->> e (15.5.1)
                const auto& t = s.as<EventTriggerStatement>();
                int sig = eventSig(t.target);
                Instr cnt; cnt.k = t.isNonBlocking ? IK::Nba : IK::Assign;
                cnt.sig = sig;
                cnt.e = bin(Op::Add, ref(sig), konst(mk(1, 32)));
                Instr at = cnt;
                at.sig = sig + 1;
                E now = node(Op::Time);
                now->k = mk(1, 64);
                at.e = bin(Op::Add, now, konst(mk(1, 64)));
                if (t.timing) {
                    if (!t.isNonBlocking || t.timing->kind != TimingControlKind::Delay)
                        unsupported("event trigger with an event control");
                    Instr d;
                    delayInto(t.timing->as<DelayControl>().expr, d);
                    for (Instr* x : {&cnt, &at}) { x->delay = d.delay; x->de = d.de; x->nbaLater = true; }
                    // The trigger time is when it fires: now + d.
                    E fire = node(Op::Time); fire->k = mk(1, 64);
                    at.e = bin(Op::Add, bin(Op::Add, fire, d.de ? d.de : konst(mk(d.delay, 64))), konst(mk(1, 64)));
                }
                prog.emit(at);
                prog.emit(cnt);
                break;
            }
            case StatementKind::Disable: {   // disable blk (9.6.2)
                const auto& d = s.as<DisableStatement>();
                if (d.target.kind != ExpressionKind::ArbitrarySymbol) unsupported("disable target");
                const Symbol* sym = d.target.as<ArbitrarySymbolExpression>().symbol;
                if (sym->kind != SymbolKind::StatementBlock) unsupported("disable of a task");
                for (size_t k = named.size(); k-- > 0;)
                    if (named[k].first == sym) { named[k].second.push_back(emitJmp()); return; }
                // A block another process is running: end that process.
                Instr i; i.k = IK::Disable;
                i.sig = blockIds().of(sym);
                blockIds().disabledFromOutside.push_back(sym);
                prog.emit(i);
                break;
            }
            case StatementKind::WaitFork: { Instr i; i.k = IK::WaitFork; prog.emit(i); break; }
            case StatementKind::DisableFork: { Instr i; i.k = IK::DisableFork; prog.emit(i); break; }
            default:
                throw std::runtime_error("lower: unsupported statement kind " + std::string(toString(s.kind)));
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
        if (s.getType().isUnpackedArray()) { regArray(s, net); return; }
        if (s.getType().isEvent()) {
            // A named event (15.5) is two signals: a trigger count, which an
            // @e waits on, and the time of the last trigger + 1, which .triggered reads.
            int id = mkSig(prefix + std::string(s.name), 32, false);
            if (mkSig(prefix + std::string(s.name) + "$trig", 64, false) != id + 1)
                Lowerer::unsupported("event signals that are not consecutive");
            sigmap[&s] = id;
            return;
        }
        int w = storeW(s.getType());
        if (w <= 0) unsupported_width(s.name);
        sigmap[&s] = mkSig(prefix + std::string(s.name), w, net);
    }
    // One signal per element, named with its indices ("m[3]", "g[1][0]"),
    // in row-major order with each dimension running left to right.
    template <typename T> void regArray(const T& s, bool net) {
        using namespace slang::ast;
        auto shape = std::make_shared<ArrShape>();
        const Type* t = &s.getType().getCanonicalType();
        int64_t total = 1;
        while (t->isUnpackedArray()) {
            if (t->kind != SymbolKind::FixedSizeUnpackedArrayType)
                throw std::runtime_error("lower: unsupported dynamic, associative, or queue array: " + std::string(s.name));
            const auto& ft = t->as<FixedSizeUnpackedArrayType>();
            shape->dims.push_back({ft.range.left, ft.range.right});
            total *= (int64_t)ft.range.width();
            if (total > kMaxArrayElems)
                throw std::runtime_error("lower: unsupported array with more than 2^20 elements: " + std::string(s.name));
            t = &ft.elementType.getCanonicalType();
        }
        int w = storeW(*t);
        if (w <= 0) unsupported_width(s.name);
        shape->count = (int)total;
        shape->elemW = w;
        shape->sgn = t->isSigned();
        shape->twoState = !t->isFourState();
        std::string base = prefix + std::string(s.name);
        std::vector<int64_t> idx(shape->dims.size());
        for (int64_t n = 0; n < total; n++) {
            int64_t rem = n;
            for (size_t d = shape->dims.size(); d-- > 0;) {
                int64_t sz = shape->size(d), o = rem % sz;
                rem /= sz;
                auto [l, r] = shape->dims[d];
                idx[d] = l <= r ? l + o : l - o;
            }
            std::string name = base;
            for (int64_t i : idx) name += "[" + std::to_string(i) + "]";
            int id = mkSig(name, w, net);
            if (n == 0) shape->base = id;
            else if (id != shape->base + n) Lowerer::unsupported("array elements that are not consecutive signals");
        }
        arrMap()[&s] = shape;
    }
    static void unsupported_width(std::string_view n) {
        throw std::runtime_error("lower: signal has a non-positive width: " + std::string(n));
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
    // Generate blocks (27): a signal inside one is named with its block,
    // "gen[2].x" for a loop iteration and "genblk1.x" for an unnamed block
    // (27.6). A block that was not selected is skipped.
    void handle(const slang::ast::GenerateBlockSymbol& g) {
        if (g.isUninstantiated) return;
        std::string saved = prefix;
        const auto* parent = g.getParentScope() ? &g.getParentScope()->asSymbol() : nullptr;
        if (parent && parent->kind == slang::ast::SymbolKind::GenerateBlockArray) {
            std::string an = parent->name.empty() ? std::string("genblk") : std::string(parent->name);
            prefix += an + "[" + (g.getArrayIndex() ? g.getArrayIndex()->toString() : std::string("?")) + "].";
        } else {
            prefix += g.getExternalName() + ".";
        }
        visitDefault(g);
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

    void prep(Lowerer& lw) { lw.bind(subs, mkSig, tempSeq, &out.subs); }

    // One driver, then wait on every signal the right-hand side reads.
    // Several assigns to one wire each own a driver slot and resolve.
    void finishAssign(Lowerer& lw, Instr i) {
        // assign v[3] = ... / assign v[7:4] = ... (10.3): this driver drives
        // only those bits; the rest of its slot stays Z, so other drivers of
        // the other bits resolve with it.
        if (i.arr || i.bidx) Lowerer::unsupported("continuous assign to a variable index");
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

    // Module-level `logic x = v;` (IEEE 1800-2023 10.5): a static variable's
    // declaration initializer runs before any initial or always procedure
    // starts. All of them go into one process that lower() places first, so
    // it is the first thing to run at time 0. Locals of a block, task or
    // function are not module members and are handled by Lowerer.
    std::unique_ptr<Lowerer> init;
    void handle(const slang::ast::VariableSymbol& v) {
        const slang::ast::Expression* e = v.getInitializer();
        if (v.getType().isEvent()) {   // both counters start at 0: not triggered
            if (e) Lowerer::unsupported("event initializer (event aliasing)");
            if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
            for (int k = 0; k < 2; k++) {
                Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v) + k;
                i.e = konst(mk(0, k ? 64 : 32));
                init->prog.emit(i);
            }
            return;
        }
        const slang::ast::Scope* sc = v.getParentScope();
        if (!sc) return;
        auto pk = sc->asSymbol().kind;
        if (pk != slang::ast::SymbolKind::InstanceBody && pk != slang::ast::SymbolKind::GenerateBlock) return;
        const auto& t = v.getType();
        // 6.8: with no initializer, a 2-state variable (int, bit, real) starts at 0.
        bool zero = !e && !t.isFourState() && (t.isIntegral() || t.isFloating() || t.isString());
        if (!e && !zero) return;
        if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
        Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v);
        i.e = e ? init->expr(*e) : konst(mk(0, (int)t.getBitWidth()));
        init->prog.emit(i);
    }
    // `wire y = a & b;` (IEEE 1800-2023 10.3.1) is a continuous assignment to y.
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
            if (w <= 0) Lowerer::unsupported("gate width");
            driveOut(*ports[0], konst(name == "pullup" ? v_ones(w) : make(0, 0, w)), s0, s1);
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
        if (w <= 0) Lowerer::unsupported("port width");
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
    void handle(const slang::ast::GenerateBlockSymbol& g) {
        if (!g.isUninstantiated) visitDefault(g);
    }
    void handle(const slang::ast::ProceduralBlockSymbol& p) {
        using slang::ast::ProceduralBlockKind;
        // A final block (9.2.3) runs once when simulation ends. Lowering it as
        // a process would run it at time 0 and, looping, forever.
        Lowerer lw(sigmap);
        prep(lw);
        lw.setScope(p.getParentScope());
        lw.stmt(p.getBody());
        lw.finishImplicit();
        if (p.procedureKind == ProceduralBlockKind::Initial)
            if (const auto* nb = Lowerer::namedSeq(p.getBody())) lw.prog.roots.push_back(Lowerer::rootId(nb));
        // A final block (9.2.3) runs once, after the last time slot. slang
        // already rejects delays and event waits in it.
        if (p.procedureKind == ProceduralBlockKind::Final) {
            out.finals.push_back({std::move(lw.prog), false, "final"});
            return;
        }
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
    arrMap().clear();
    blockIds() = BlockIds{};
    SigCollector sc(mkSig, sigmap);
    comp.getRoot().visit(sc);

    struct Finder : slang::ast::ASTVisitor<Finder> {
        std::vector<const slang::ast::SubroutineSymbol*> found;
        int prec = 1000;
        void handle(const slang::ast::InstanceSymbol& inst) {
            if (!enterInstance(inst)) return;
            if (auto ts = inst.body.getTimeScale()) prec = std::min(prec, tsExp(ts->precision));
            visitDefault(inst);
        }
        void handle(const slang::ast::SubroutineSymbol& s) { found.push_back(&s); }
        void handle(const slang::ast::GenerateBlockSymbol& g) {
            if (!g.isUninstantiated) visitDefault(g);
        }
    };
    Finder finder;
    comp.getRoot().visit(finder);
    // The simulation precision is the finest precision of any module (3.14.3).
    simPrecExp() = finder.prec == 1000 ? -9 : finder.prec;
    out.precExp = simPrecExp();

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
            sub->retW = storeW(sym->getReturnType());
            if (sub->retW <= 0) Lowerer::unsupported("function return width");
            sub->retSgn = sym->getReturnType().isSigned();
        }
        for (const slang::ast::FormalArgumentSymbol* formal : sym->getArguments()) {
            auto it = sigmap.find(formal);
            if (it == sigmap.end()) Lowerer::unsupported("subroutine argument");
            int w = storeW(formal->getType());
            if (w <= 0) Lowerer::unsupported("subroutine argument width");
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
        lw.bind(&subMap, mkSig, &tempSeq, &out.subs);
        lw.setScope(sub->sym);
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
    for (const slang::ast::Symbol* b : blockIds().disabledFromOutside)
        if (!blockIds().roots.count(blockIds().of(b)))
            Lowerer::unsupported(("disable of block '" + std::string(b->name) +
                                  "' from outside it, unless it is a whole initial or fork branch").c_str());
    return out;
}
