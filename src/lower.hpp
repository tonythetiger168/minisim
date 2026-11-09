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
// Automatic tasks, class methods, and automatic functions that recurse or
// fork run each call in a frame (expr.hpp: Frame); their variables are
// relative ids. Everything else is a static signal.
// Not yet:  nmos/pmos/cmos/tran, bufif/notif, trireg, a[i +: w],
//           ref arguments, DPI, class methods,
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
#include <deque>
#include <set>
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
#include "slang/text/SourceManager.h"
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
#include "slang/ast/symbols/SpecifySymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/text/SourceManager.h"

// Fixed-size unpacked arrays (7.4), keyed by the array's symbol. SigCollector
// fills it; Lowerer reads it. lower() clears it first, since symbols from an
// earlier compilation may share addresses with this one's.
using ArrMap = std::unordered_map<const slang::ast::Symbol*, std::shared_ptr<ArrShape>>;
inline ArrMap& arrMap() { static ArrMap m; return m; }
constexpr int64_t kMaxArrayElems = 1 << 20;
// The design's source manager, for file names and line numbers in messages.
inline const slang::SourceManager*& srcMgr() { static const slang::SourceManager* m = nullptr; return m; }
// Net types that resolve differently from wire (6.6): signal -> kind.
// alias a = b = c; (10.11): every net in an alias maps to one of them, which
// is the only one that gets a signal.
inline std::unordered_map<const slang::ast::Symbol*, const slang::ast::Symbol*>& aliasOf() {
    static std::unordered_map<const slang::ast::Symbol*, const slang::ast::Symbol*> m;
    return m;
}
inline bool& usesForce() { static bool f = false; return f; }   // some force / assign (10.6): no partitions
inline std::vector<std::pair<int, uint8_t>>& netKinds() { static std::vector<std::pair<int, uint8_t>> v; return v; }
// Dynamic arrays, queues, and associative arrays: symbol -> handle signal.
inline std::unordered_map<const slang::ast::Symbol*, int>& dynMap() {
    static std::unordered_map<const slang::ast::Symbol*, int> m;
    return m;
}

// Automatic variables that live in a frame (expr.hpp): symbol -> frame
// shape and slot. An unpacked array's shape has its first slot as base; a
// container is one slot whose empty shape is in the layout's dyn list.
struct LocalSlot { FrameLayout* lay = nullptr; int off = 0; };
inline std::unordered_map<const slang::ast::Symbol*, LocalSlot>& localMap() {
    static std::unordered_map<const slang::ast::Symbol*, LocalSlot> m;
    return m;
}
inline std::unordered_map<const slang::ast::Symbol*, std::pair<FrameLayout*, std::shared_ptr<ArrShape>>>& localArr() {
    static std::unordered_map<const slang::ast::Symbol*, std::pair<FrameLayout*, std::shared_ptr<ArrShape>>> m;
    return m;
}
inline std::unordered_map<const slang::ast::Symbol*, LocalSlot>& localDyn() {
    static std::unordered_map<const slang::ast::Symbol*, LocalSlot> m;
    return m;
}
// Owner of every frame shape made by this lowering (LowerResult::layouts).
inline std::vector<std::unique_ptr<FrameLayout>>*& layoutStore() {
    static std::vector<std::unique_ptr<FrameLayout>>* v = nullptr;
    return v;
}
inline FrameLayout* newLayout(std::string name) {
    if (!layoutStore()) throw std::runtime_error("lower: unsupported automatic variables here");
    layoutStore()->push_back(std::make_unique<FrameLayout>());
    layoutStore()->back()->name = std::move(name);
    return layoutStore()->back().get();
}

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
// Unpacked structs and unions (7.2, 7.3) are kept as one vector: the
// members in declared order, the first at the top, each in its own storage
// (a real as its 64 bits, a string as its 64-bit pool id). A union's members
// all start at bit 0. -1: a member that cannot be laid out this way.
inline int uWidth(const slang::ast::Type& t0);
// A string is a 64-bit pool id; a shortreal is kept as a double, rounded to
// single precision whenever a shortreal value is made (6.12).
inline int storeW(const slang::ast::Type& t0) {
    const auto& t = t0.getCanonicalType();
    if (t.isUnpackedStruct() || t.isUnpackedUnion()) return uWidth(t);
    return t.isString() || t.isFloating() ? 64 : (int)t.getBitWidth();
}
// Tag bits of a tagged union (7.3.2): enough to number its members. The tag
// is at the top; a member's value is right-justified below it.
inline int tagBitsOf(const slang::ast::Type& t0) {
    using namespace slang::ast;
    const auto& t = t0.getCanonicalType();
    if (t.kind == SymbolKind::PackedUnionType) return (int)t.as<PackedUnionType>().tagBits;
    size_t n = t.as<UnpackedUnionType>().fields.size();
    int b = 1;
    while (((size_t)1 << b) < n) b++;
    return b;
}
inline bool isTaggedUnion(const slang::ast::Type& t0) {
    using namespace slang::ast;
    const auto& t = t0.getCanonicalType();
    if (t.kind == SymbolKind::PackedUnionType) return t.as<PackedUnionType>().isTagged;
    if (t.kind == SymbolKind::UnpackedUnionType) return t.as<UnpackedUnionType>().isTagged;
    return false;
}
inline int tagIndexOf(const slang::ast::Type& t0, const slang::ast::Symbol& f) {
    using namespace slang::ast;
    int k = 0;
    for (const auto& g : t0.getCanonicalType().as<Scope>().membersOfType<FieldSymbol>()) {
        if (&g == &f) return k;
        k++;
    }
    return -1;
}
inline int uWidth(const slang::ast::Type& t0) {
    using namespace slang::ast;
    const auto& t = t0.getCanonicalType();
    if (isTaggedUnion(t) && t.isUnpackedUnion()) {
        int w = 0;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
            if (f.getType().isVoid()) continue;
            int fw = uWidth(f.getType());
            if (fw <= 0) return -1;
            w = std::max(w, fw);
        }
        return w + tagBitsOf(t);
    }
    if (t.isUnpackedStruct() || t.isUnpackedUnion()) {
        int w = 0;
        bool u = t.isUnpackedUnion();
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
            int fw = uWidth(f.getType());
            if (fw <= 0) return -1;
            w = u ? std::max(w, fw) : w + fw;
        }
        return w;
    }
    if (t.isUnpackedArray() || t.isClass() || t.isEvent() || t.isCHandle() || t.isVirtualInterface()) return -1;
    return storeW(t);
}
// Offset and width of field f inside unpacked struct or union t.
inline std::pair<int, int> uField(const slang::ast::Type& t0, const slang::ast::FieldSymbol& f) {
    using namespace slang::ast;
    const auto& t = t0.getCanonicalType();
    int fw = uWidth(f.getType());
    if (t.isUnpackedUnion()) return {0, fw};
    int after = 0;
    bool seen = false;
    for (const auto& g : t.as<Scope>().membersOfType<FieldSymbol>()) {
        if (seen) after += uWidth(g.getType());
        if (&g == &f) seen = true;
    }
    return {after, fw};
}
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
    FrameLayout* lay = nullptr;   // automatic (13.3.2): arguments, result and locals are frame slots
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
    std::vector<std::unique_ptr<FrameLayout>> layouts;   // frame shapes of automatic subroutines and fork branches
    std::vector<std::pair<int, std::vector<uint64_t>>> netDelays;   // wire #d w (28.16): signal, ticks
    std::vector<std::pair<int, uint8_t>> netKinds;                   // wand, wor, tri0, ... (6.6): signal, NetResolve
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
        int s = sigFind(&x.as<ValueExpressionBase>().symbol);
        if (s < 0) unsupported("event expression");
        return s;
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
    // The rise / fall / turn-off delays of a continuous assign, gate or net
    // (10.3.3, 28.16), in ticks. They must be constant.
    std::vector<uint64_t> delays3(const slang::ast::TimingControl* tc) {
        using namespace slang::ast;
        std::vector<uint64_t> out;
        if (!tc) return out;
        auto one = [&](const Expression& e) {
            Instr t;
            delayInto(e, t);
            if (t.de) unsupported("a delay on a continuous assignment, gate or net that is not constant");
            out.push_back(t.delay);
        };
        if (tc->kind == TimingControlKind::Delay) one(tc->as<DelayControl>().expr);
        else if (tc->kind == TimingControlKind::Delay3) {
            const auto& d3 = tc->as<Delay3Control>();
            one(d3.expr1);
            if (d3.expr2) one(*d3.expr2);
            if (d3.expr3) one(*d3.expr3);
        } else unsupported("this kind of delay");
        bool any = false;
        for (auto v : out) any |= v != 0;
        if (!any) out.clear();   // #0 is no delay
        return out;
    }
    void delayInto(const slang::ast::Expression& d, Instr& i) {
        using namespace slang::ast;
        if (peel(d).kind == ExpressionKind::MinTypMax) {   // #(1:2:3) uses the typical value (11.11)
            delayInto(peel(d).as<MinTypMaxExpression>().typ(), i);
            return;
        }
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

    // ---------- frames (automatic variables) ----------
    // The frame shapes around the code being lowered, outermost first: an
    // automatic subroutine's, then a fork block's, then a branch's. Empty in
    // static code, where every variable is a signal.
    std::vector<FrameLayout*> frames;
    int relIn(const FrameLayout* L, int off) const {
        for (size_t k = frames.size(); k-- > 0;)
            if (frames[k] == L) {
                int depth = (int)(frames.size() - 1 - k);
                if (depth >= kRelThis) unsupported("fork nested more than 7 deep in an automatic context");
                return relSig(depth, off);
            }
        unsupported("automatic variable used outside its frame");
    }
    FrameLayout* layOf(int rel) const {
        int depth = relMode(rel);
        if (depth >= (int)frames.size()) unsupported("automatic variable outside a frame");
        return frames[frames.size() - 1 - depth];
    }
    int sigFind(const slang::ast::Symbol* sym) const {
        if (auto it = localMap().find(sym); it != localMap().end()) return relIn(it->second.lay, it->second.off);
        auto it = sigmap.find(sym);
        return it == sigmap.end() ? -1 : it->second;
    }
    int sigAt(const slang::ast::Symbol* sym) const {
        int s = sigFind(sym);
        if (s < 0) unsupported("reference to a variable with no signal");
        return s;
    }
    std::shared_ptr<const ArrShape> arrFind(const slang::ast::Symbol* sym) const {
        if (auto it = localArr().find(sym); it != localArr().end()) {
            auto a = std::make_shared<ArrShape>(*it->second.second);
            a->base = relIn(it->second.first, it->second.second->base);
            return a;
        }
        auto it = arrMap().find(sym);
        return it == arrMap().end() ? nullptr : it->second;
    }
    int dynFind(const slang::ast::Symbol* sym) const {
        if (auto it = localDyn().find(sym); it != localDyn().end()) return relIn(it->second.lay, it->second.off);
        auto it = dynMap().find(sym);
        return it == dynMap().end() ? -1 : it->second;
    }
    // The empty shape of container d (a signal, or a frame slot).
    const DynObj& dynShape(int d) const {
        if (!isRel(d)) return dynStore()[d];
        const FrameLayout* L = layOf(d);
        for (auto& [off, o] : L->dyn)
            if (off == relOff(d)) return o;
        unsupported("container slot");
    }
    // A variable as a frame slot, by its type (6.21): 4-state starts X,
    // 2-state 0, string "", real 0.0, a container empty.
    static Val initOf(const slang::ast::Type& t0) {
        const auto& t = t0.getCanonicalType();
        if (t.isFloating()) return mkreal(0);
        if (t.isString()) return mkstr("");
        int w = storeW(t);
        if (w <= 0) unsupported("automatic variable of this type");
        if (!t.isFourState()) return make(0, 0, w);
        return w > 64 ? allx(w) : Val{};
    }
    static DynObj dynShapeOf(const slang::ast::Type& t0);
    void declLocal(const slang::ast::VariableSymbol& v, FrameLayout* L) {
        using namespace slang::ast;
        if (localMap().count(&v) || localArr().count(&v) || localDyn().count(&v)) return;
        const Type& t = v.getType().getCanonicalType();
        if (isDynType(t)) {
            int off = L->add(64, make(0, 0, 64));
            L->dyn.push_back({off, dynShapeOf(t)});
            localDyn()[&v] = {L, off};
            return;
        }
        if (t.isEvent()) {
            int off = L->add(32, make(0, 0, 32));
            L->add(64, make(0, 0, 64));
            localMap()[&v] = {L, off};
            return;
        }
        if (t.isUnpackedArray()) {
            auto shape = std::make_shared<ArrShape>();
            const Type* et = &t;
            int64_t total = 1;
            while (et->isUnpackedArray()) {
                if (et->kind != SymbolKind::FixedSizeUnpackedArrayType) unsupported("automatic array of this kind");
                const auto& ft = et->as<FixedSizeUnpackedArrayType>();
                shape->dims.push_back({ft.range.left, ft.range.right});
                total *= (int64_t)ft.range.width();
                if (total > (1 << 16)) unsupported("automatic array with more than 65536 elements");
                et = &ft.elementType.getCanonicalType();
            }
            Val iv = initOf(*et);
            shape->count = (int)total;
            shape->elemW = storeW(*et);
            shape->sgn = et->isSigned();
            shape->twoState = !et->isFourState();
            for (int64_t k = 0; k < total; k++) {
                int off = L->add(shape->elemW, iv);
                if (!k) shape->base = off;
            }
            localArr()[&v] = {L, shape};
            return;
        }
        localMap()[&v] = {L, L->add(storeW(t), initOf(t))};
    }
    // Every automatic variable that `st` declares, outside nested fork
    // branches (which get their own frames), as a slot of frame L.
    void declLocals(const slang::ast::Statement& st, FrameLayout* L);
    // A loop variable made by the lowerer (foreach): a static signal, or in
    // a frame a slot of the innermost frame.
    void bindVar(const slang::ast::Symbol* sym, int sig) {
        if (isRel(sig)) localMap()[sym] = {frames.back(), relOff(sig)};
        else sigmap[sym] = sig;
    }

    // Owns the Prog of each fork branch, so Fork's pointers outlive lower().
    std::vector<std::unique_ptr<Prog>>* store = nullptr;
    void bind(std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* s,
              MkSig mk, int* seq, std::vector<std::unique_ptr<Prog>>* st = nullptr) {
        subs = s;
        mkSig = std::move(mk);
        tempSeq = seq;
        store = st;
    }

    int freshStatic(int w) {
        if (!mkSig) unsupported("temporary");
        int n = tempSeq ? (*tempSeq)++ : 0;
        return mkSig("_tf" + std::to_string(n), w, false);
    }
    int fresh(int w) {
        if (!frames.empty()) return relSig(0, frames.back()->add(w, w > 64 ? allx(w) : Val{}));
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
        if (sub->lay) return emitAutoCall(*sub, args, wantValue);

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

    // A call of an automatic subroutine (13.3.2): its arguments go straight
    // into the new frame; outputs and the result are copied out of the frame
    // it returned from (relative mode kRelRet) right after the call.
    E emitAutoCall(const Subr& sub, std::span<const slang::ast::Expression* const> args, bool wantValue) {
        using namespace slang::ast;
        Instr c;
        c.k = IK::Call;
        c.callee = sub.prog;
        c.lay = sub.lay;
        for (size_t n = 0; n < args.size(); n++) {
            const auto& formal = sub.args[n];
            const Expression& a = *args[n];
            if (formal.dir == ArgumentDirection::Ref) unsupported("ref argument");
            if (formal.dir == ArgumentDirection::In) {
                if (a.kind == ExpressionKind::EmptyArgument) unsupported("empty argument");
                c.init.push_back({formal.sig, expr(a)});
            } else if (formal.dir == ArgumentDirection::InOut) {
                if (a.kind != ExpressionKind::Assignment) unsupported("inout argument");
                c.init.push_back({formal.sig, expr(a.as<AssignmentExpression>().left())});
            } else if (a.kind != ExpressionKind::Assignment && a.kind != ExpressionKind::EmptyArgument) {
                unsupported("output argument");
            }
        }
        prog.emit(c);
        for (size_t n = 0; n < args.size(); n++) {
            const auto& formal = sub.args[n];
            if (formal.dir != ArgumentDirection::Out && formal.dir != ArgumentDirection::InOut) continue;
            const Expression& a = *args[n];
            if (a.kind == ExpressionKind::EmptyArgument) continue;
            const auto& as = a.as<AssignmentExpression>();
            Instr i;
            i.k = IK::Assign;
            lhs(as.left(), i);
            OutSlot slot{relSig(kRelRet, formal.sig), formal.w, formal.sgn};
            const OutSlot* saved = outSlot;
            outSlot = &slot;
            i.e = expr(as.right());
            outSlot = saved;
            prog.emit(i);
        }
        if (!wantValue) return {};
        if (sub.retSig < 0) unsupported("subroutine call has no return value");
        int tmp = fresh(sub.retW);
        Instr i;
        i.k = IK::Assign;
        i.sig = tmp;
        i.e = ref(relSig(kRelRet, sub.retSig), sub.retSgn);
        prog.emit(i);
        return ref(tmp, sub.retSgn);
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
        if (e.type->getBitWidth() == 32) {   // shortreal (6.12): computed as a real, then rounded
            E r = exprReal64(e);
            if (r->op == Op::FMath && r->hi == M_F32) return r;
            auto m = node(Op::FMath, {r});
            m->hi = M_F32;
            return m;
        }
        return exprReal64(e);
    }
    E exprReal64(const slang::ast::Expression& e) {
        using namespace slang::ast;
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
                if (n == "$bitstoshortreal" && args.size() == 1) return sysFnNode(SF_BITS2SR, {expr(*args[0])});
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
        const auto& p = peel(e);   // a literal keeps its text, whatever its length
        if (p.kind == slang::ast::ExpressionKind::StringLiteral)
            return konst(mkstr(std::string(p.as<slang::ast::StringLiteral>().getValue())));
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
    // ---- array query functions (20.7) ----
    static bool isArrayQuery(std::string_view n) {
        return n == "$left" || n == "$right" || n == "$low" || n == "$high" || n == "$increment" || n == "$size" ||
               n == "$dimensions" || n == "$unpacked_dimensions";
    }
    // The type as $typename writes it (20.6.1): typedefs resolved, no
    // default signing, ranges as decimal numbers.
    static std::string typeName(const slang::ast::Type& t) {
        std::string s = t.getCanonicalType().toString();
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {   // "logic [3:0]" -> "logic[3:0]"
            if (s[i] == ' ' && i + 1 < s.size() && s[i + 1] == '[') continue;
            out += s[i];
        }
        return out;
    }
    struct QDim { int kind; int64_t l, r; };   // kind: 0 fixed, 1 dynamic array or queue, 2 associative
    static void queryDims(const slang::ast::Type& t0, std::vector<QDim>& dims, int& unpacked, int64_t& assocMax) {
        using namespace slang::ast;
        const Type* t = &t0.getCanonicalType();
        unpacked = 0;
        assocMax = 0;
        for (;;) {
            if (t->kind == SymbolKind::FixedSizeUnpackedArrayType) {
                const auto& f = t->as<FixedSizeUnpackedArrayType>();
                dims.push_back({0, f.range.left, f.range.right});
                t = &f.elementType.getCanonicalType();
            } else if (t->kind == SymbolKind::DynamicArrayType) {
                dims.push_back({1, 0, 0});
                t = &t->as<DynamicArrayType>().elementType.getCanonicalType();
            } else if (t->kind == SymbolKind::QueueType) {
                dims.push_back({1, 0, 0});
                t = &t->as<QueueType>().elementType.getCanonicalType();
            } else if (t->kind == SymbolKind::AssociativeArrayType) {
                const auto& a = t->as<AssociativeArrayType>();
                if (a.indexType && a.indexType->isIntegral()) {
                    int kw = (int)a.indexType->getBitWidth();
                    bool ks = a.indexType->isSigned();
                    assocMax = kw >= 64 ? INT64_MAX : ks ? ((int64_t)1 << (kw - 1)) - 1 : ((int64_t)1 << kw) - 1;
                } else assocMax = -1;
                dims.push_back({2, 0, 0});
                t = &a.elementType.getCanonicalType();
            } else break;
            unpacked++;
        }
        if (t->kind == SymbolKind::EnumType) t = &t->as<EnumType>().baseType.getCanonicalType();
        while (t->kind == SymbolKind::PackedArrayType) {
            const auto& p = t->as<PackedArrayType>();
            dims.push_back({0, p.range.left, p.range.right});
            t = &p.elementType.getCanonicalType();
            if (t->kind == SymbolKind::EnumType) t = &t->as<EnumType>().baseType.getCanonicalType();
        }
        if (t->kind == SymbolKind::PredefinedIntegerType || t->kind == SymbolKind::PackedStructType || t->kind == SymbolKind::PackedUnionType)
            dims.push_back({0, (int64_t)t->getBitWidth() - 1, 0});   // int is [31:0] (20.7)
    }
    E arrayQuery(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (args.empty()) unsupported("array query without an argument");
        auto num = [](int64_t v) { Val r = make((uint64_t)v, 0, 32); r.sgn = true; return konst(r); };
        E xv = konst(xfill(32, true));
        std::vector<QDim> dims;
        int unpacked = 0;
        int64_t assocMax = 0;
        const Type& t = *args[0]->type;
        queryDims(t, dims, unpacked, assocMax);
        if (n == "$dimensions") {
            if (dims.empty() && (t.isString() || t.isIntegral())) return num(t.isString() ? 1 : 1);
            return num((int64_t)dims.size());
        }
        if (n == "$unpacked_dimensions") return num(unpacked);
        if (dims.empty() && t.isIntegral() && t.getBitWidth() == 1) dims.push_back({0, 0, 0});   // a scalar
        int64_t d = 1;
        if (args.size() > 1) {
            const auto* cv = args[1]->getConstant();
            bool known = (cv && cv->isInteger() && !cv->integer().hasUnknown()) || args[1]->kind == ExpressionKind::IntegerLiteral;
            if (!known) {   // a dimension known only at run time: pick among the answers
                E dv = resize(expr(*args[1]), 64, args[1]->type->isSigned(), true);
                E r = xv;
                for (size_t k = dims.size(); k-- > 0;) {
                    if (dims[k].kind != 0 && k != 0) continue;
                    Val kk = make(k + 1, 0, 64); kk.sgn = true;
                    r = mux(bin(Op::Eq, dv, konst(kk)), queryDim(args, n, dims[k], (int64_t)k + 1, assocMax), r);
                }
                return r;
            }
            d = constInt(*args[1]);
        }
        if (d < 1 || d > (int64_t)dims.size()) return xv;
        return queryDim(args, n, dims[(size_t)d - 1], d, assocMax);
    }
    E queryDim(std::span<const slang::ast::Expression* const> args, std::string_view n, const QDim& q, int64_t d,
               int64_t assocMax) {
        auto num = [](int64_t v) { Val r = make((uint64_t)v, 0, 32); r.sgn = true; return konst(r); };
        E xv = konst(xfill(32, true));
        if (q.kind == 0) {
            int64_t l = q.l, r = q.r, inc = l >= r ? 1 : -1;
            int64_t lo = std::min(l, r), hi = std::max(l, r);
            if (n == "$left") return num(l);
            if (n == "$right") return num(r);
            if (n == "$low") return num(lo);
            if (n == "$high") return num(hi);
            if (n == "$increment") return num(inc);
            return num(hi - lo + 1);
        }
        int h = d == 1 ? dynSig(*args[0]) : -1;
        if (h < 0) unsupported("array query of a dynamic dimension that is not a variable's first dimension");
        E size = dynSize(h);
        Val one = make(1, 0, 32); one.sgn = true;
        if (n == "$increment") return num(-1);
        if (n == "$size") return size;
        if (q.kind == 1) {   // dynamic array or queue: [0 : size-1]
            if (n == "$left" || n == "$low") return num(0);
            return bin(Op::Sub, size, konst(one));
        }
        if (n == "$left") return num(0);   // associative (20.7)
        if (n == "$right") return assocMax < 0 ? xv : num(assocMax);
        auto b = node(Op::SysFn);
        b->hi = SF_ASSOC_BOUND;
        b->lo = n == "$high" ? 1 : 0;
        b->sig = h;
        return b;
    }

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
                    hierName = scopeName(call);
                    formatInto(args, 'd', f->fmt, f->kids);
                    return f;
                }
                if (isStrMethodCall(call)) return strMethod(call, strMethodId(n));
                if (E r = enumMethod(call)) return r;   // e.name()
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
            case ExpressionKind::UnbasedUnsizedIntegerLiteral: {   // '0 '1 'x 'z fill their context (5.7.1)
                auto b = e.as<UnbasedUnsizedIntegerLiteral>().getLiteralValue();
                int w = ctx.w > 0 ? ctx.w : (int)e.type->getBitWidth();
                if (w <= 0) w = 1;
                needWidth(w);
                int bit = b.value == slang::logic_t::Z_VALUE ? 3 : b.isUnknown() ? 2 : b.value ? 1 : 0;
                Val v;
                if (w <= 64) {
                    uint64_t m = mask(w);
                    v = make(bit == 1 ? m : 0, bit >= 2 ? m : 0, w, bit == 3 ? m : 0, false);
                } else {
                    v = make(0, 0, w, 0, false);
                    for (int k = 0; k < w; k++) setBit(v, k, bit);
                    v = finish(v);
                }
                return sized(konst(v), w, false, ctx);
            }
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
        auto shape = arrFind(&cur->as<NamedValueExpression>().symbol);
        if (!shape) unsupported("array select");
        ArrSel s;
        s.a = shape;
        s.idx.assign(idx.rbegin(), idx.rend());
        if (s.idx.size() != s.a->dims.size()) unsupported("array slice (fewer indices than dimensions)");
        for (auto* x : s.idx)
            if (!(x->kind == ExpressionKind::IntegerLiteral || x->getConstant())) s.constant = false;
        return s;
    }
    // The element values of an unpacked-array value of type t, flattened in
    // row-major order (7.4, 10.9.1): an assignment pattern ('{a, b},
    // '{n{a}}, '{0: a, default: b}), or another fixed-size array.
    void arrayElems(const slang::ast::Type& t0, const slang::ast::Expression& x0, std::vector<E>& out) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        if (t.kind != SymbolKind::FixedSizeUnpackedArrayType) {
            if (t.isUnpackedArray()) unsupported("a container inside a fixed-size array");
            if (!t.isIntegral() && !t.isFloating() && !t.isString()) unsupported("array of this element type");
            int w = (int)t.getBitWidth();
            const Expression& x = peel(x0);
            E v = expr(x0);
            if (t.isIntegral() && x.type->isIntegral()) v = resize(expr(x), w, x.type->isSigned(), t.isSigned());
            out.push_back(v);
            return;
        }
        const auto& ft = t.as<FixedSizeUnpackedArrayType>();
        const Type& et = ft.elementType;
        int64_t n = (int64_t)ft.range.width();
        const Expression& x = peel(x0);
        switch (x.kind) {
            case ExpressionKind::SimpleAssignmentPattern:
                for (auto* el : x.as<SimpleAssignmentPatternExpression>().elements()) arrayElems(et, *el, out);
                return;
            case ExpressionKind::ReplicatedAssignmentPattern: {
                const auto& r = x.as<ReplicatedAssignmentPatternExpression>();
                int64_t c = constInt(r.count());
                for (int64_t k = 0; k < c; k++)
                    for (auto* el : r.elements()) arrayElems(et, *el, out);
                return;
            }
            case ExpressionKind::StructuredAssignmentPattern: {
                const auto& sp = x.as<StructuredAssignmentPatternExpression>();
                if (!sp.typeSetters.empty()) unsupported("type: key in an array pattern");
                for (int64_t k = 0; k < n; k++) {
                    int64_t idx = ft.range.left >= ft.range.right ? ft.range.left - k : ft.range.left + k;
                    const Expression* v = nullptr;
                    for (const auto& is : sp.indexSetters)
                        if (constInt(*is.index) == idx) v = is.expr;
                    if (v) { arrayElems(et, *v, out); continue; }
                    if (!sp.defaultSetter) unsupported("array pattern without a value for every index");
                    fillElems(et, *sp.defaultSetter, out);
                }
                return;
            }
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {
                auto a = arrFind(&x.as<ValueExpressionBase>().symbol);
                if (!a) unsupported("array value");
                for (int k = 0; k < a->count; k++) out.push_back(ref(a->base + k, a->sgn));
                return;
            }
            case ExpressionKind::Concatenation: {   // {a, 4, 5} into an unpacked array (10.10)
                for (auto* op : x.as<ConcatenationExpression>().operands()) {
                    if (op->type->isUnpackedArray()) arrayElems(*op->type, *op, out);
                    else arrayElems(et, *op, out);
                }
                return;
            }
            case ExpressionKind::RangeSelect: {   // a[1:2], a[i +: 2] with a constant start (7.4.6)
                const auto& rs = x.as<RangeSelectExpression>();
                const Expression& base = peel(rs.value());
                if (base.kind != ExpressionKind::NamedValue && base.kind != ExpressionKind::HierarchicalValue)
                    unsupported("slice of an array expression");
                auto it = arrMap().find(&base.as<ValueExpressionBase>().symbol);
                if (it == arrMap().end() || it->second->dims.size() != 1) unsupported("slice of a multidimensional array");
                const ArrShape& A = *it->second;
                int64_t l, r;
                if (rs.getSelectionKind() == RangeSelectionKind::Simple) { l = constInt(rs.left()); r = constInt(rs.right()); }
                else {
                    int64_t st = constInt(rs.left()), wd = constInt(rs.right());
                    bool up = rs.getSelectionKind() == RangeSelectionKind::IndexedUp;
                    auto [dl, dr] = A.dims[0];
                    bool asc = dl <= dr;   // a[i +: w] runs from i upward in index value
                    int64_t lo = up ? st : st - wd + 1, hi = up ? st + wd - 1 : st;
                    l = asc ? lo : hi; r = asc ? hi : lo;
                }
                int64_t step = l <= r ? 1 : -1;
                for (int64_t k = l;; k += step) {
                    int64_t off = A.offset({k});
                    if (off < 0) unsupported("array slice outside the array");
                    out.push_back(ref(A.base + (int)off, A.sgn));
                    if (k == r) break;
                }
                return;
            }
            default: unsupported("fixed-size array value of this kind");
        }
    }
    // default: v fills every element below type t (10.9.1), unless v is
    // itself an array value of type t.
    void fillElems(const slang::ast::Type& t0, const slang::ast::Expression& v, std::vector<E>& out) {
        const auto& t = t0.getCanonicalType();
        if (t.kind != slang::ast::SymbolKind::FixedSizeUnpackedArrayType || v.type->isUnpackedArray()) {
            arrayElems(t, v, out);
            return;
        }
        const auto& ft = t.as<slang::ast::FixedSizeUnpackedArrayType>();
        for (int64_t k = 0; k < (int64_t)ft.range.width(); k++) fillElems(ft.elementType, v, out);
    }
    // a = <array value>; for a whole fixed-size array. The values are taken
    // before any element is written, so a = '{a[1], a[0]} swaps.
    bool assignWholeArray(const slang::ast::Expression& l0, const slang::ast::Expression& rhs, bool nba) {
        using namespace slang::ast;
        const Expression& l = peel(l0);
        if (l.kind != ExpressionKind::NamedValue && l.kind != ExpressionKind::HierarchicalValue) return false;
        return assignWholeArray(l.as<ValueExpressionBase>().symbol, *l.type, rhs, nba);
    }
    bool assignWholeArray(const slang::ast::Symbol& sym, const slang::ast::Type& lt, const slang::ast::Expression& rhs, bool nba) {
        auto ap = arrFind(&sym);
        if (!ap) return false;
        const ArrShape& A = *ap;
        std::vector<E> vals;
        arrayElems(lt, rhs, vals);
        if ((int)vals.size() != A.count) unsupported("array value with the wrong number of elements");
        bool direct = nba || A.count == 1;
        if (!direct) {
            direct = true;
            for (auto& v : vals) if (v->op != Op::Const) { direct = false; break; }
        }
        if (!direct) {
            for (auto& v : vals) {
                int tmp = fresh(A.elemW);
                Instr h; h.k = IK::Assign; h.sig = tmp; h.e = v;
                prog.emit(h);
                v = ref(tmp, A.sgn);
            }
        }
        for (int k = 0; k < A.count; k++) {
            Instr i; i.k = nba ? IK::Nba : IK::Assign; i.sig = A.base + k; i.e = vals[k];
            prog.emit(i);
        }
        return true;
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

    // ---------- dynamic arrays, queues, associative arrays (7.5, 7.8, 7.10) ----------
    // A container is a hidden 64-bit handle signal whose value counts its
    // changes; the elements live in dynStore()[handle].
    int dollarQ = -1;   // the queue whose `$` (last index) the selector being lowered uses
    int dynSig(const slang::ast::Expression& e0) const {
        using namespace slang::ast;
        const Expression& e = peel(e0);
        if (e.kind != ExpressionKind::NamedValue && e.kind != ExpressionKind::HierarchicalValue) return -1;
        return dynFind(&e.as<ValueExpressionBase>().symbol);
    }
    static bool isDynType(const slang::ast::Type& t0) {
        const auto& t = t0.getCanonicalType();
        return t.isQueue() || t.isAssociativeArray() ||
               t.kind == slang::ast::SymbolKind::DynamicArrayType;
    }
    // An index or key of container d, with `$` meaning its last index.
    E dynKey(int d, const slang::ast::Expression& x) {
        if (dynShape(d).strKey) return strOf(x);
        int saved = dollarQ;
        dollarQ = d;
        E k = expr(x);
        dollarQ = saved;
        return k;
    }
    E dynGet(int d, const slang::ast::Expression& key) {
        auto n = node(Op::DynGet, {dynKey(d, key)});
        n->sig = d;
        return n;
    }
    E dynSize(int d) {
        auto n = node(Op::DynSize);
        n->sig = d;
        return n;
    }
    Instr dynCmd(int d, int cmd) {
        Instr i; i.k = IK::Dyn; i.sig = d; i.dcmd = cmd;
        return i;
    }
    // A scratch container shaped like d, to build a new value in before
    // copying it over, so q = {q[1:$], q[0]} reads the old q throughout.
    int dynTemp(int d) {
        DynObj o = dynShape(d);
        o.vec.clear(); o.map.clear();
        int t = fresh(64);
        if (isRel(t)) frames.back()->dyn.push_back({relOff(t), o});
        else dynStore()[t] = o;
        return t;
    }
    static const slang::ast::Expression& outArg(const slang::ast::Expression& a) {
        using namespace slang::ast;
        const Expression* x = &peel(a);
        if (x->kind == ExpressionKind::Assignment) x = &peel(x->as<AssignmentExpression>().left());
        return *x;
    }
    // Append the value of `x` (an element, a container, or a queue slice) to t.
    void dynAppend(int t, const slang::ast::Expression& x0) {
        using namespace slang::ast;
        const Expression& x = peel(x0);
        if (int s = dynSig(x); s >= 0) {
            Instr i = dynCmd(t, D_APPEND); i.dsrc = s; prog.emit(i); return;
        }
        if (x.kind == ExpressionKind::RangeSelect) {
            const auto& r = x.as<RangeSelectExpression>();
            if (int s = dynSig(r.value()); s >= 0) {
                if (r.getSelectionKind() != RangeSelectionKind::Simple) unsupported("+: / -: slice of a queue");
                Instr i = dynCmd(t, D_APPEND_SLICE); i.dsrc = s;
                i.args = {dynKey(s, r.left()), dynKey(s, r.right())};
                prog.emit(i);
                return;
            }
        }
        if (x.kind == ExpressionKind::NamedValue && x.type->isUnpackedArray()) {
            auto a = arrFind(&x.as<NamedValueExpression>().symbol);
            if (!a || a->dims.size() != 1) unsupported("array in a container value");
            for (int k = 0; k < a->count; k++) {
                Instr i = dynCmd(t, D_PUSH_BACK); i.e = ref(a->base + k, a->sgn); prog.emit(i);
            }
            return;
        }
        if (x.kind == ExpressionKind::Concatenation && isDynType(*x.type)) {
            for (auto* op : x.as<ConcatenationExpression>().operands()) dynAppend(t, *op);
            return;
        }
        if (x.kind == ExpressionKind::Call && isDynType(*x.type)) {   // r = q.find(x) with (...)
            const auto& c = x.as<CallExpression>();
            if (c.isSystemCall() && isLocator(c.getSubroutineName())) {
                Instr i = dynCmd(t, D_APPEND); i.dsrc = locatorCall(c); prog.emit(i);
                return;
            }
        }
        if (x.type->isUnpackedArray()) unsupported("container value");
        Instr i = dynCmd(t, D_PUSH_BACK); i.e = expr(x); prog.emit(i);
    }
    // dst = rhs for a whole container: new[n](src), '{...}, {a, q, b}, {},
    // another container, a fixed-size array, or a queue slice.
    void assignContainer(int dst, const slang::ast::Expression& rhs0) {
        using namespace slang::ast;
        const Expression& rhs = peel(rhs0);
        if (rhs.kind == ExpressionKind::NewArray) {   // 7.5.1
            const auto& na = rhs.as<NewArrayExpression>();
            Instr i = dynCmd(dst, D_NEW);
            i.args = {expr(na.sizeExpr())};
            if (const Expression* init = na.initExpr()) {
                i.dsrc = dynSig(*init);
                if (i.dsrc < 0) {   // new[n]('{...}): build the source first
                    i.dsrc = dynTemp(dst);
                    prog.emit(dynCmd(i.dsrc, D_DELETE));
                    assignInto(i.dsrc, *init);
                }
            }
            prog.emit(i);
            return;
        }
        if (int s = dynSig(rhs); s >= 0) {
            if (s == dst) return;
            Instr i = dynCmd(dst, D_COPY); i.dsrc = s; prog.emit(i); return;
        }
        int t = dynTemp(dst);
        prog.emit(dynCmd(t, D_DELETE));
        assignInto(t, rhs);
        Instr i = dynCmd(dst, D_COPY); i.dsrc = t; prog.emit(i);
    }
    // Build rhs into the empty container t.
    void assignInto(int t, const slang::ast::Expression& rhs0) {
        using namespace slang::ast;
        const Expression& rhs = peel(rhs0);
        switch (rhs.kind) {
            case ExpressionKind::SimpleAssignmentPattern:
            case ExpressionKind::ReplicatedAssignmentPattern: {
                if (dynShape(t).kind == 2) unsupported("positional pattern for an associative array");
                const auto& ap = static_cast<const AssignmentPatternExpressionBase&>(rhs);
                int64_t n = rhs.kind == ExpressionKind::ReplicatedAssignmentPattern
                                ? constInt(rhs.as<ReplicatedAssignmentPatternExpression>().count()) : 1;
                for (int64_t k = 0; k < n; k++)
                    for (auto* el : ap.elements()) { Instr i = dynCmd(t, D_PUSH_BACK); i.e = expr(*el); prog.emit(i); }
                return;
            }
            case ExpressionKind::StructuredAssignmentPattern: {   // '{key: v, default: d} (10.9.2)
                const auto& sp = rhs.as<StructuredAssignmentPatternExpression>();
                if (dynShape(t).kind != 2) unsupported("keyed pattern for this container");
                if (!sp.memberSetters.empty() || !sp.typeSetters.empty()) unsupported("pattern member setter");
                if (sp.defaultSetter) unsupported("default: in an associative array pattern");
                for (auto& is : sp.indexSetters) {
                    Instr i = dynCmd(t, D_SET); i.args = {dynKey(t, *is.index)}; i.e = expr(*is.expr); prog.emit(i);
                }
                return;
            }
            case ExpressionKind::EmptyArgument: return;
            default: dynAppend(t, rhs);
        }
    }
    static bool isReduction(std::string_view n) {
        return n == "sum" || n == "product" || n == "and" || n == "or" || n == "xor";
    }
    static bool isLocator(std::string_view n) {
        return n == "find" || n == "find_index" || n == "find_first" || n == "find_first_index" ||
               n == "find_last" || n == "find_last_index" || n == "min" || n == "max" ||
               n == "unique" || n == "unique_index";
    }
    // A fixed-size array, numbered [0:N-1], for an array method: a scratch
    // queue holding a copy of its elements.
    static bool isFixedArr(const slang::ast::Expression& x) {
        return x.type->getCanonicalType().kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType;
    }
    int fixedAsQueue(const slang::ast::Expression& x) {
        using namespace slang::ast;
        const Type& at = x.type->getCanonicalType();
        if (!isFixedArr(x)) unsupported("array method of this array");
        if (!stmtDepth) unsupported("array method of a fixed-size array outside a procedural statement");
        auto r = at.as<FixedSizeUnpackedArrayType>().range;
        if (r.left != 0 || r.right < 0) unsupported("array method of an array not declared [N] or [0:N-1]");
        int q = keyQueue(*at.getArrayElementType());
        dynAppend(q, x);
        return q;
    }
    // An element of container d for an array method: the with expression
    // evaluated with the iterator set to element ix, or the element itself.
    struct ElemKey { int d; const slang::ast::Expression* withE; const slang::ast::ValueSymbol* it; int itSig; };
    E elemAt(int d, int ix) {
        auto g = node(Op::DynGet, {ref(ix, true)});
        g->sig = d;
        return g;
    }
    E elemKey(const ElemKey& k, int ix) {
        if (!k.withE) return elemAt(k.d, ix);
        Instr a; a.k = IK::Assign; a.sig = k.itSig; a.e = elemAt(k.d, ix);
        prog.emit(a);
        return expr(*k.withE);
    }
    // An empty scratch queue whose elements have type t.
    int keyQueue(const slang::ast::Type& t) {
        int q = fresh(64);
        DynObj o;
        o.kind = 1;
        const auto& ct = t.getCanonicalType();
        if (ct.isFloating()) o.dflt = mkreal(0);
        else if (ct.isString()) o.dflt = mkstr("");
        else {
            int w = (int)ct.getBitWidth();
            needWidth(w);
            o.dflt = ct.isFourState() ? xfill(w) : mk(0, w);
            o.dflt.sgn = ct.isSigned();
        }
        if (isRel(q)) frames.back()->dyn.push_back({relOff(q), o});
        else dynStore()[q] = o;
        prog.emit(dynCmd(q, D_DELETE));
        return q;
    }
    // for (ix = 0; ix < d.size(); ix++) body, or from the end when back is
    // set. body may add jumps to the end of the loop to exits.
    template <class F>
    void dynLoop(int d, bool back, F body) {
        int ix = fresh(32);
        Val one = mk(1, 32); one.sgn = true;
        Val zero = mk(0, 32); zero.sgn = true;
        Instr i0; i0.k = IK::Assign; i0.sig = ix;
        i0.e = back ? bin(Op::Sub, resize(dynSize(d), 32, false, true), konst(one)) : konst(zero);
        prog.emit(i0);
        size_t top = prog.here();
        Instr j; j.k = IK::JmpIfNot;
        j.e = back ? bin(Op::Ge, ref(ix, true), konst(zero))
                   : bin(Op::Lt, ref(ix, true), resize(dynSize(d), 32, false, true));
        std::vector<size_t> exits{prog.emit(j)};
        body(ix, exits);
        Instr st; st.k = IK::Assign; st.sig = ix;
        st.e = bin(back ? Op::Sub : Op::Add, ref(ix, true), konst(one));
        prog.emit(st);
        Instr bk; bk.k = IK::Jmp; bk.target = top; prog.emit(bk);
        for (size_t x : exits) prog.code[x].target = prog.here();
    }
    // q.find(x) with (...), q.min(), q.unique_index() ... (7.12.1): the
    // result goes into a scratch queue, which is returned.
    int locatorCall(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        std::string_view n = call.getSubroutineName();
        auto args = call.arguments();
        if (!stmtDepth) unsupported("locator method outside a procedural statement");
        int d = dynSig(*args[0]);
        if (d < 0) d = fixedAsQueue(*args[0]);
        if (dynShape(d).kind == 2) unsupported("locator method of an associative array");
        const Expression* withE = nullptr;
        const ValueSymbol* itSym = nullptr;
        if (auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine))
            std::tie(withE, itSym) = sci->getIteratorInfo();
        ElemKey ek{d, withE, itSym, -1};
        if (itSym) {
            int w = storeW(itSym->getType());
            needWidth(w);
            ek.itSig = fresh(w);
            sigmap[itSym] = ek.itSig;
        }
        bool index = n.ends_with("_index");
        const Type& et = call.type->getCanonicalType().getArrayElementType()->getCanonicalType();
        int res = keyQueue(et);
        auto push = [&](int ix) {
            Instr p = dynCmd(res, D_PUSH_BACK);
            p.e = index ? ref(ix, true) : elemAt(d, ix);
            prog.emit(p);
        };
        if (n.starts_with("find")) {
            if (!withE) unsupported("find without a with clause");
            bool first = n.starts_with("find_first"), last = n.starts_with("find_last");
            dynLoop(d, last, [&](int ix, std::vector<size_t>& exits) {
                Instr j; j.k = IK::JmpIfNot; j.e = elemKey(ek, ix);
                size_t jf = prog.emit(j);
                push(ix);
                if (first || last) { Instr x; x.k = IK::Jmp; exits.push_back(prog.emit(x)); }
                prog.code[jf].target = prog.here();
            });
            return res;
        }
        const Type& kt = (withE ? *withE->type : *args[0]->type->getArrayElementType()).getCanonicalType();
        if (!kt.isIntegral()) unsupported("min / max / unique on a real, string or aggregate key");
        int kw = (int)kt.getBitWidth();
        needWidth(kw);
        bool ks = kt.isSigned();
        if (n == "min" || n == "max") {
            int best = fresh(32), bk = fresh(kw);
            Val m1 = mk(-1, 32); m1.sgn = true; m1.v = mask(32);
            Instr b0; b0.k = IK::Assign; b0.sig = best; b0.e = konst(m1); prog.emit(b0);
            dynLoop(d, false, [&](int ix, std::vector<size_t>&) {
                int cur = fresh(kw);
                Instr c; c.k = IK::Assign; c.sig = cur; c.e = resize(elemKey(ek, ix), kw, ks, ks);
                prog.emit(c);
                Val zero = mk(0, 32); zero.sgn = true;
                E better = n == "min" ? bin(Op::Lt, ref(cur, ks), ref(bk, ks)) : bin(Op::Gt, ref(cur, ks), ref(bk, ks));
                Instr j; j.k = IK::JmpIfNot;
                j.e = bin(Op::LogOr, bin(Op::Lt, ref(best, true), konst(zero)), better);
                size_t jf = prog.emit(j);
                Instr s1; s1.k = IK::Assign; s1.sig = bk; s1.e = ref(cur, ks); prog.emit(s1);
                Instr s2; s2.k = IK::Assign; s2.sig = best; s2.e = ref(ix, true); prog.emit(s2);
                prog.code[jf].target = prog.here();
            });
            Val zero = mk(0, 32); zero.sgn = true;
            Instr j; j.k = IK::JmpIfNot; j.e = bin(Op::Ge, ref(best, true), konst(zero));
            size_t jf = prog.emit(j);
            push(best);
            prog.code[jf].target = prog.here();
            return res;
        }
        if (n == "unique" || n == "unique_index") {
            // element ix is kept when no earlier element has an equal key
            int cur = fresh(kw), dup = fresh(1);
            dynLoop(d, false, [&](int ix, std::vector<size_t>&) {
                Instr c; c.k = IK::Assign; c.sig = cur; c.e = resize(elemKey(ek, ix), kw, ks, ks); prog.emit(c);
                Instr z; z.k = IK::Assign; z.sig = dup; z.e = konst(mk(0, 1)); prog.emit(z);
                int jx = fresh(32);
                Val zero = mk(0, 32); zero.sgn = true;
                Val one = mk(1, 32); one.sgn = true;
                Instr j0; j0.k = IK::Assign; j0.sig = jx; j0.e = konst(zero); prog.emit(j0);
                size_t top = prog.here();
                Instr jt; jt.k = IK::JmpIfNot; jt.e = bin(Op::Lt, ref(jx, true), ref(ix, true));
                size_t jf = prog.emit(jt);
                E eq = bin(Op::CaseEq, resize(elemKey(ek, jx), kw, ks, ks), ref(cur, ks));
                Instr sd; sd.k = IK::Assign; sd.sig = dup;
                sd.e = bin(Op::LogOr, ref(dup, false), eq);
                prog.emit(sd);
                Instr inc; inc.k = IK::Assign; inc.sig = jx; inc.e = bin(Op::Add, ref(jx, true), konst(one)); prog.emit(inc);
                Instr bk; bk.k = IK::Jmp; bk.target = top; prog.emit(bk);
                prog.code[jf].target = prog.here();
                Instr k; k.k = IK::JmpIfNot; k.e = un(Op::LogNot, ref(dup, false));
                size_t kf = prog.emit(k);
                push(ix);
                prog.code[kf].target = prog.here();
            });
            return res;
        }
        unsupported("locator method");
    }

    // Built-in methods of a container (7.5.2, 7.8, 7.9, 7.10.2, 7.12): as a
    // value, or as a statement when wantValue is false.
    E dynMethod(const slang::ast::CallExpression& call, bool wantValue) {
        using namespace slang::ast;
        std::string_view n = call.getSubroutineName();
        auto args = call.arguments();
        int d = dynSig(*args[0]);
        if (d < 0) d = fixedAsQueue(*args[0]);
        const DynObj& o = dynShape(d);
        const Expression* withE = nullptr;
        const ValueSymbol* itSym = nullptr;
        if (auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine))
            std::tie(withE, itSym) = sci->getIteratorInfo();
        if (withE || isLocator(n)) {
            if (!stmtDepth) unsupported("array method with a `with` clause outside a procedural statement");
            if (o.kind == 2) unsupported("locator or with method of an associative array");
            ElemKey ek{d, withE, itSym, -1};
            if (itSym) {
                int w = storeW(itSym->getType());
                needWidth(w);
                ek.itSig = fresh(w);
                sigmap[itSym] = ek.itSig;
            }
            static const std::pair<const char*, Op> redw[] = {
                {"sum", Op::Add}, {"product", Op::Mul}, {"and", Op::And}, {"or", Op::Or}, {"xor", Op::Xor}};
            for (auto& [s, op] : redw)
                if (n == s) {   // 7.12.3: the result has the type of the with expression
                    int w = (int)call.type->getBitWidth();
                    bool sg = call.type->isSigned();
                    if (!call.type->isIntegral()) unsupported("reduction with a non-integral with clause");
                    int acc = fresh(w);
                    Val init = n == "product" ? mk(1, w) : n == "and" ? v_not(mk(0, w)) : mk(0, w);
                    Instr a0; a0.k = IK::Assign; a0.sig = acc; a0.e = konst(init); prog.emit(a0);
                    dynLoop(d, false, [&](int ix, std::vector<size_t>&) {
                        Instr a; a.k = IK::Assign; a.sig = acc;
                        a.e = bin(op, ref(acc, sg), resize(elemKey(ek, ix), w, withE ? withE->type->isSigned() : sg, sg));
                        prog.emit(a);
                    });
                    return ref(acc, sg);
                }
            if (n == "sort" || n == "rsort") {   // 7.12.2: order by the with expression
                if (wantValue) unsupported("sort used as a value");
                int kc = keyQueue(*withE->type);
                dynLoop(d, false, [&](int ix, std::vector<size_t>&) {
                    Instr p = dynCmd(kc, D_PUSH_BACK); p.e = elemKey(ek, ix); prog.emit(p);
                });
                Instr i = dynCmd(d, n == "sort" ? D_SORT_BY : D_RSORT_BY);
                i.dsrc = kc;
                prog.emit(i);
                return nullptr;
            }
            if (isLocator(n)) unsupported("locator method here (assign its result to a queue)");
            unsupported("array method with a `with` clause");
        }
        if (n == "size" || n == "num" || n == "$size") return dynSize(d);
        if (n == "exists") {
            auto x = node(Op::DynExists, {dynKey(d, *args[1])});
            x->sig = d;
            return x;
        }
        static const std::pair<const char*, int> red[] = {{"sum", 0}, {"product", 1}, {"and", 2}, {"or", 3}, {"xor", 4}};
        for (auto& [s, id] : red)
            if (n == s) {
                if (o.dflt.real || o.dflt.str) unsupported("reduction of a real or string array");
                auto x = node(Op::DynReduce);
                x->sig = d;
                x->hi = id;
                x->k = mk(0, (int)call.type->getBitWidth());
                x->k.sgn = call.type->isSigned();
                return x;
            }
        if (n == "pop_front" || n == "pop_back") {
            if (wantValue && !stmtDepth) unsupported("pop outside a procedural statement");
            Instr i = dynCmd(d, n == "pop_front" ? D_POP_FRONT : D_POP_BACK);
            int w = storeW(*call.type);
            needWidth(w);
            i.dres = fresh(w);
            prog.emit(i);
            return ref(i.dres, call.type->isSigned());
        }
        if (n == "first" || n == "last" || n == "next" || n == "prev") {   // 7.9.4-7.9.7
            if (wantValue && !stmtDepth) unsupported("first/next outside a procedural statement");
            Instr i = dynCmd(d, n == "first" ? D_FIRST : n == "last" ? D_LAST : n == "next" ? D_NEXT : D_PREV);
            Instr k; k.k = IK::Assign;
            lhs(outArg(*args[1]), k);
            if (k.hi >= 0 || k.bidx || k.arr || k.dyn >= 0) unsupported("first/next into a part of a variable");
            i.dkey = k.sig;
            i.dres = fresh(32);
            prog.emit(i);
            return ref(i.dres, true);
        }
        if (wantValue) unsupported("container method used as a value");
        static const std::pair<const char*, int> cmds[] = {{"push_back", D_PUSH_BACK}, {"push_front", D_PUSH_FRONT},
            {"insert", D_INSERT}, {"reverse", D_REVERSE}, {"sort", D_SORT}, {"rsort", D_RSORT}, {"shuffle", D_SHUFFLE}};
        Instr i;
        if (n == "delete") i = dynCmd(d, args.size() > 1 ? D_DELETE_AT : D_DELETE);
        else {
            bool found = false;
            for (auto& [s, c] : cmds) if (n == s) { i = dynCmd(d, c); found = true; }
            if (!found) unsupported("container method");
        }
        if (i.dcmd == D_DELETE_AT) i.args = {dynKey(d, *args[1])};
        if (i.dcmd == D_PUSH_BACK || i.dcmd == D_PUSH_FRONT) i.e = expr(*args[1]);
        if (i.dcmd == D_INSERT) { i.args = {dynKey(d, *args[1])}; i.e = expr(*args[2]); }
        prog.emit(i);
        return nullptr;
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
        if (m.member.kind != SymbolKind::Field) unsupported("member access");
        const auto& f = m.member.as<FieldSymbol>();
        const Type& vt = m.value().type->getCanonicalType();
        if (vt.isUnpackedStruct() || vt.isUnpackedUnion()) {
            auto [off, w] = uField(vt, f);
            if (w <= 0 || uWidth(vt) <= 0) unsupported("member of an unpacked struct that holds an array, class or event");
            PSel ps;
            ps.cbit = off;
            ps.bits = w;
            return ps;
        }
        if (!vt.isIntegral()) unsupported("member of a class");
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
                if (dynFind(sym) >= 0) unsupported("a whole dynamic array, queue, or associative array as a value");
                int sg = sigFind(sym);
                if (sg < 0) return constSym(*sym, e, ctx);
                int src = (int)e.type->getBitWidth();
                bool self = e.type->isSigned();
                return sized(ref(sg, self), src, self, ctx);
            }
            case ExpressionKind::LValueReference: {
                if (!lvalue) unsupported("lvalue reference");
                return expr(*lvalue, ctx);
            }
            case ExpressionKind::TaggedUnion:
                return sized(taggedValue(e.as<TaggedUnionExpression>()), storeW(*e.type), false, ctx);
            case ExpressionKind::Streaming: {
                const auto& sc = e.as<StreamingConcatenationExpression>();
                int w = 0;
                E v = streamBits(sc, w);
                return sized(v, w, false, ctx);
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
                if (peel(c.operand()).kind == ExpressionKind::Streaming && c.type->isIntegral()) {
                    // a stream into a wider integral type is left-aligned,
                    // padded with zeros on the right (11.4.14)
                    int w = (int)c.type->getBitWidth();
                    needWidth(w);
                    return sized(streamTo(peel(c.operand()).as<StreamingConcatenationExpression>(), w),
                                 w, c.type->isSigned(), ctx);
                }
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
            case ExpressionKind::MinTypMax:   // (1:2:3) is its typical value (11.11)
                return expr(e.as<MinTypMaxExpression>().typ(), ctx);
            case ExpressionKind::UnboundedLiteral: {   // q[$]: the last index (7.10.1)
                if (dollarQ < 0) unsupported("$ outside a queue index");
                Val one = mk(1, 32); one.sgn = true;
                return sized(bin(Op::Sub, dynSize(dollarQ), konst(one)), 32, true, ctx);
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
                if (isArrayQuery(n)) return sized(arrayQuery(call, n), 32, true, ctx);
                if (n == "$typename" && call.arguments().size() == 1) {   // 20.6.1
                    if (ctx.live && !e.type->isString()) unsupported("$typename used as a number");
                    return konst(mkstr(typeName(*call.arguments()[0]->type)));
                }
                if (!call.arguments().empty() &&
                    (dynSig(*call.arguments()[0]) >= 0 ||
                     (isFixedArr(*call.arguments()[0]) && isReduction(n)))) {   // q.size(), aa.exists(k), a.sum() ...
                    int w = storeW(*e.type);
                    needWidth(w);
                    return sized(dynMethod(call, true), w, e.type->isSigned(), ctx);
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
                if (n == "$bits" && call.arguments().size() == 1 && call.arguments()[0]->type->isFixedSize()) {   // 20.6.2
                    Val b = mk(call.arguments()[0]->type->getBitstreamWidth(), 32);
                    b.sgn = true;
                    return sized(konst(b), 32, true, ctx);
                }
                if (E r = sysFn(call, n)) return sized(r, (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
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
                // Already at the context width; a cast such as unsigned'(-a) can
                // still change the sign the parent sees (6.24.1).
                if (ctx.live && ctx.w > 0 && ctx.tag != e.type->isSigned()) {
                    int w = ctx.w;
                    return resize(std::move(out), w, false, ctx.tag);
                }
                return out;
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                if (b.left().type->isUnpackedArray() &&
                    (b.op == BinaryOperator::Equality || b.op == BinaryOperator::Inequality ||
                     b.op == BinaryOperator::CaseEquality || b.op == BinaryOperator::CaseInequality)) {
                    // Whole fixed-size arrays (7.6): every element pair compares
                    // equal, as == does on the elements (x if none differ but
                    // some compare x), or === for case equality.
                    std::vector<E> l, r;
                    arrayElems(*b.left().type, b.left(), l);
                    arrayElems(*b.right().type, b.right(), r);
                    if (l.size() != r.size()) unsupported("compare of arrays of different sizes");
                    bool cs = b.op == BinaryOperator::CaseEquality || b.op == BinaryOperator::CaseInequality;
                    E acc;
                    for (size_t k = 0; k < l.size(); k++) {
                        E m = bin(cs ? Op::CaseEq : Op::Eq, l[k], r[k]);
                        acc = acc ? bin(Op::LogAnd, acc, m) : m;
                    }
                    if (!acc) acc = konst(mk(1, 1));
                    if (b.op == BinaryOperator::Inequality || b.op == BinaryOperator::CaseInequality) acc = un(Op::LogNot, acc);
                    return sized(acc, 1, false, ctx);
                }
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
                if (int d = dynSig(e.as<ElementSelectExpression>().value()); d >= 0) {   // q[i], aa[key]
                    int w = storeW(*e.type);
                    needWidth(w);
                    return sized(dynGet(d, e.as<ElementSelectExpression>().selector()), w, e.type->isSigned(), ctx);
                }
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
                for (const Expression* it : in.rangeList()) {
                    E m = insideItem(in.left(), *it);
                    acc = acc ? bin(Op::LogOr, acc, m) : m;
                }
                return sized(acc ? acc : konst(mk(0, 1)), 1, false, ctx);
            }
            case ExpressionKind::MemberAccess: {    // s.f
                const auto& m = e.as<MemberAccessExpression>();
                return sized(readPSel(expr(m.value()), memberPSel(m)), storeW(*e.type), e.type->isSigned(), ctx);
            }
            case ExpressionKind::SimpleAssignmentPattern:
            case ExpressionKind::StructuredAssignmentPattern:
            case ExpressionKind::ReplicatedAssignmentPattern: {   // '{a, b}, '{f: x}, '{n{a}} (10.9)
                if (e.type->isUnpackedStruct() || e.type->isUnpackedUnion()) return sized(structValue(e), storeW(*e.type), false, ctx);
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
                if (dynFind(&e.as<ValueExpressionBase>().symbol) >= 0) unsupported("container assignment here");
                int sg = sigFind(&e.as<ValueExpressionBase>().symbol);
                if (sg < 0) unsupported("assignment to a non-signal");
                i.sig = sg;
                return;
            }
            case ExpressionKind::ElementSelect: {
                if (int d = dynSig(e.as<ElementSelectExpression>().value()); d >= 0) {   // q[i] = ..., aa[k] = ...
                    i.sig = d;
                    i.dyn = d;
                    i.args = {dynKey(d, e.as<ElementSelectExpression>().selector())};
                    return;
                }
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
                if (i.dyn >= 0) unsupported("write to part of a container element");
                writePSel(i, packedSel(*sel.value().type, &sel.selector(), nullptr, 1, PSel::Index));
                return;
            }
            case ExpressionKind::RangeSelect: {   // v[7:4] = ..., v[i +: w] = ... (11.5.1)
                const auto& r = e.as<RangeSelectExpression>();
                if (dynSig(r.value()) >= 0) unsupported("write to a queue slice");
                lhs(r.value(), i);
                if (i.dyn >= 0) unsupported("write to part of a container element");
                writePSel(i, rangePSel(r));
                return;
            }
            case ExpressionKind::MemberAccess: {   // s.f = ...
                const auto& m = e.as<MemberAccessExpression>();
                lhs(m.value(), i);
                if (i.dyn >= 0) unsupported("write to part of a container element");
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
            int sig = sigAt(&s.expr.as<ValueExpressionBase>().symbol);
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
    // The hierarchical name %m prints (21.2.1.1): the scope of the call, such
    // as top.u1 or top.u1.blk.
    static std::string scopeName(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        const auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine);
        if (!sci || !sci->scope) return "top";
        const Symbol* s = &sci->scope->asSymbol();
        if (s->kind == SymbolKind::InstanceBody) {
            const auto* inst = s->as<InstanceBodySymbol>().parentInstance;
            if (inst) return inst->getHierarchicalPath();
        }
        return s->getHierarchicalPath();
    }
    std::string hierName = "top";   // what %m prints for the call being lowered
    static std::string substM(const std::string& f, const std::string& name) {
        std::string esc, r;
        for (char c : name) { esc += c; if (c == '%') esc += '%'; }
        for (size_t p = 0; p < f.size(); p++) {
            if (f[p] != '%') { r += f[p]; continue; }
            if (p + 1 < f.size() && f[p + 1] == '%') { r += "%%"; p++; continue; }
            size_t q = p + 1;
            while (q < f.size() && (isdigit((unsigned char)f[q]) || f[q] == '.' || f[q] == '-')) q++;
            if (q < f.size() && (f[q] == 'm' || f[q] == 'M')) { r += esc; p = q; continue; }
            r += f[p];
        }
        return r;
    }
    // A value read as text with format letter c (a string for %s, a real for
    // %f %e %g, else a 64-bit integer), converted for a write to type dst.
    E readAs(E v, char c, const slang::ast::Type& dst, const char* what) {
        bool txt = c == 's', rl = c == 'f' || c == 'e' || c == 'g';
        int w = (int)dst.getBitWidth();
        if (dst.isString()) {
            if (!txt) unsupported((std::string(what) + " into a string with a number format").c_str());
            return v;
        }
        if (dst.isFloating()) return rl ? v : txt ? v : un(Op::IToR, v);
        if (txt) { v = node(Op::StrToInt, {v}); v->hi = w; return v; }
        if (rl) return rtoi(v, w, dst.isSigned(), false);
        return resize(v, w, true, dst.isSigned());
    }
    // $fscanf / $sscanf (21.3.4): one node reads everything; then each output
    // that a conversion reached is written. Returns the count.
    E scanCall(const slang::ast::CallExpression& call, bool file) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (args.size() < 2) unsupported("$fscanf / $sscanf arguments");
        if (!stmtDepth) unsupported("$fscanf / $sscanf outside a procedural statement");
        const Expression& f = peel(*args[1]);
        if (f.kind != ExpressionKind::StringLiteral) unsupported("$fscanf / $sscanf format that is not a literal");
        std::string fs(f.as<StringLiteral>().getValue());
        std::vector<char> letters;   // one per conversion that is stored
        for (size_t i = 0; i < fs.size(); i++) {
            if (fs[i] != '%' || i + 1 >= fs.size()) continue;
            i++;
            if (fs[i] == '%') continue;
            bool skip = fs[i] == '*';
            if (skip) i++;
            while (i < fs.size() && isdigit((unsigned char)fs[i])) i++;
            if (i >= fs.size()) break;
            char c = (char)tolower((unsigned char)fs[i]);
            if (std::string_view("dhxobcsfegtum").find(c) == std::string_view::npos)
                unsupported(("$fscanf / $sscanf format %" + std::string(1, fs[i])).c_str());
            if (!skip) letters.push_back(c);
        }
        if (letters.size() != args.size() - 2) unsupported("$fscanf / $sscanf with a different number of formats and outputs");
        E src = file ? expr(*args[0]) : strOf(*args[0]);
        int cnt = fresh(32);
        Instr t; t.k = IK::Assign; t.sig = cnt; t.e = sysFnNode(SF_SCAN, {src, konst(mkstr(fs))}, file ? 1 : 0);
        prog.emit(t);
        for (size_t k = 0; k < letters.size(); k++) {
            if (letters[k] == 'm') continue;
            Instr j; j.k = IK::JmpIfNot;
            j.e = bin(Op::Gt, ref(cnt, true), konst(make(k, 0, 32, 0, true)));
            size_t jf = prog.emit(j);
            const Expression& dst = outArg(*args[k + 2]);
            Instr w; w.k = IK::Assign;
            lhs(dst, w);
            w.e = readAs(sysFnNode(SF_SCAN_GET, {konst(mk(k, 32))}), letters[k], *dst.type, "$fscanf / $sscanf");
            prog.emit(w);
            prog.code[jf].target = prog.here();
        }
        return ref(cnt, true);
    }
    // $fdisplay / $fwrite and their b / o / h forms (21.3.2).
    E fileWriteCall(const slang::ast::CallExpression& call, std::string_view n) {
        auto args = call.arguments();
        if (args.empty()) unsupported("$fdisplay / $fwrite without a descriptor");
        char dflt = 'd';
        std::string_view base = n;
        if (base.ends_with('b') || base.ends_with('o') || base.ends_with('h')) { dflt = base.back(); base.remove_suffix(1); }
        auto f = node(Op::SFormat);
        hierName = scopeName(call);
        formatInto(args.subspan(1), dflt, f->fmt, f->kids);
        return sysFnNode(SF_FWRITE, {expr(*args[0]), f}, base == "$fdisplay" ? 1 : 0);
    }
    // $fgets(str, fd) (21.3.4): the line is written only if one was read.
    E fgetsCall(const slang::ast::CallExpression& call) {
        auto args = call.arguments();
        if (args.size() != 2) unsupported("$fgets arguments");
        if (!stmtDepth) unsupported("$fgets outside a procedural statement");
        int cnt = fresh(32);
        Instr t; t.k = IK::Assign; t.sig = cnt; t.e = sysFnNode(SF_FGETS, {expr(*args[1])});
        prog.emit(t);
        Instr j; j.k = IK::JmpIfNot; j.e = ref(cnt);
        size_t jf = prog.emit(j);
        const auto& dst = outArg(*args[0]);
        Instr w; w.k = IK::Assign;
        lhs(dst, w);
        w.e = readAs(sysFnNode(SF_SCAN_TEXT, {}), 's', *dst.type, "$fgets");
        prog.emit(w);
        prog.code[jf].target = prog.here();
        return ref(cnt, true);
    }
    // Enumeration methods (6.19.5): first, last, num, next(N), prev(N), name.
    // Each is a chain of compares against the declared values. A value that
    // is not a member gives the type's default for next / prev and "" for name.
    E enumMethod(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (!call.isSystemCall() || args.empty() || call.getSubroutineName()[0] == '$') return nullptr;
        const Type& t = args[0]->type->getCanonicalType();
        if (t.kind != SymbolKind::EnumType) return nullptr;
        std::string_view n = call.getSubroutineName();
        std::vector<const EnumValueSymbol*> vals;
        for (const auto& ev : t.as<EnumType>().values()) vals.push_back(&ev);
        int w = (int)t.getBitWidth();
        bool sg = t.isSigned();
        auto lit = [&](size_t k) { return resize(svLit(vals[k]->getValue().integer(), sg, {}), w, sg, sg); };
        if (vals.empty()) unsupported("method of an empty enum");
        if (n == "first") return lit(0);
        if (n == "last") return lit(vals.size() - 1);
        if (n == "num") return konst(make(vals.size(), 0, 32, 0, true));
        E v = expr(*args[0]);
        if (n == "name") {
            E r = konst(mkstr(""));
            for (size_t k = vals.size(); k-- > 0;)
                r = mux(bin(Op::CaseEq, v, lit(k)), konst(mkstr(std::string(vals[k]->name))), r);
            return r;
        }
        if (n == "next" || n == "prev") {
            int64_t step = 1;
            if (args.size() > 1) {
                if (!args[1]->getConstant() && args[1]->kind != ExpressionKind::IntegerLiteral)
                    unsupported("enum next(N) / prev(N) with N not a constant");
                step = constInt(*args[1]);
            }
            int64_t cnt = (int64_t)vals.size();
            int64_t sh = ((n == "next" ? step : -step) % cnt + cnt) % cnt;
            Val d = t.isFourState() ? xfill(w, sg) : make(0, 0, w, 0, sg);
            E r = konst(d);
            for (size_t k = vals.size(); k-- > 0;)
                r = mux(bin(Op::CaseEq, v, lit(k)), lit((size_t)(((int64_t)k + sh) % cnt)), r);
            return r;
        }
        return nullptr;
    }
    // $cast(dst, src) (6.24.2) for integral, real and enum types: the cast
    // is valid unless dst is an enum and src is not one of its values. A valid
    // cast writes dst and gives 1; an invalid one leaves dst alone and gives 0,
    // and as a task it is a run-time error.
    bool asTask = false;   // the system call being lowered is a statement
    E castCall(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (args.size() != 2) unsupported("$cast arguments");
        if (!stmtDepth) unsupported("$cast outside a procedural statement");
        bool task = asTask;
        const Expression& dst = outArg(*args[0]);
        const Expression& src = *args[1];
        const Type& dt = dst.type->getCanonicalType();
        const Type& st = src.type->getCanonicalType();
        if (dt.isClass() || st.isClass() || !(dt.isIntegral() || dt.isFloating()) || !(st.isIntegral() || st.isFloating()))
            unsupported("$cast of these types");
        int w = (int)dt.getBitWidth();
        E v;
        if (dt.isFloating()) v = realOf(src);
        else if (st.isFloating()) v = rtoi(realOf(src), w, dt.isSigned(), false);
        else v = resize(expr(src), w, st.isSigned(), dt.isSigned());
        int tmp = fresh(dt.isFloating() ? 64 : w);
        Instr h; h.k = IK::Assign; h.sig = tmp; h.e = v;
        prog.emit(h);
        E val = ref(tmp, dt.isSigned());
        E okE = konst(mk(1, 1));
        if (dt.kind == SymbolKind::EnumType) {
            okE = nullptr;
            for (const auto& ev : dt.as<EnumType>().values()) {
                E m = bin(Op::CaseEq, val, resize(svLit(ev.getValue().integer(), dt.isSigned(), {}), w, dt.isSigned(), dt.isSigned()));
                okE = okE ? bin(Op::LogOr, okE, m) : m;
            }
            if (!okE) okE = konst(mk(0, 1));
        }
        int ok = fresh(1);
        Instr o; o.k = IK::Assign; o.sig = ok; o.e = okE;
        prog.emit(o);
        Instr j; j.k = IK::JmpIfNot; j.e = ref(ok);
        size_t jf = prog.emit(j);
        Instr wr; wr.k = IK::Assign;
        lhs(dst, wr);
        wr.e = dt.isFloating() ? un(Op::AsReal, ref(tmp)) : val;
        prog.emit(wr);
        if (task) {
            size_t js = emitJmp();
            prog.code[jf].target = prog.here();
            Instr d; d.k = IK::Display;
            d.fmt = "Error: $cast failed: the value is not a valid " + escPct(std::string(dt.name.empty() ? "enum" : dt.name)) + " value";
            prog.emit(d);
            prog.code[js].target = prog.here();
        } else {
            prog.code[jf].target = prog.here();
        }
        return resize(ref(ok), 32, false, true);
    }
    // One item of an inside list (11.4.13), also a case inside label
    // (12.5.4): [lo:hi] is a range, anything else matches with ==?.
    E insideItem(const slang::ast::Expression& l, const slang::ast::Expression& it) {
        using namespace slang::ast;
        auto pair = [&](const Expression& x, const Expression& y) {
            int w = std::max((int)x.type->getBitWidth(), (int)y.type->getBitWidth());
            needWidth(w);
            bool sg = x.type->isSigned() && y.type->isSigned();
            ExprCtx c{w, sg, sg, true};
            return std::pair<E, E>{expr(x, c), expr(y, c)};
        };
        if (it.kind == ExpressionKind::ValueRange) {
            const auto& vr = it.as<ValueRangeExpression>();
            if (vr.rangeKind != ValueRangeKind::Simple) unsupported("tolerance range in inside");
            auto [l1, lo] = pair(l, vr.left());
            auto [l2, hi] = pair(l, vr.right());
            return bin(Op::LogAnd, bin(Op::Ge, l1, lo), bin(Op::Le, l2, hi));
        }
        if (!it.type->isIntegral() || !l.type->isIntegral()) unsupported("inside on a non-integral item");
        auto [a, b] = pair(l, it);
        return bin(Op::WildEq, a, b);
    }
    // The storage bits of a value of type t (see uWidth): a real as its 64
    // bits, a string as its pool id, a struct as its members.
    E storageOf(const slang::ast::Type& t0, const slang::ast::Expression& x) {
        const auto& t = t0.getCanonicalType();
        if (t.isFloating()) { auto b = node(Op::Resize, {realOf(x)}); b->hi = 64; return b; }
        if (t.isString()) return resize(strOf(x), 64, false, false);
        int w = storeW(t);
        if (t.isIntegral()) return resize(expr(x), w, x.type->isSigned(), false);
        return resize(expr(x), w, false, false);
    }
    // '{a, b, c} or '{f: v, default: d} for an unpacked struct (10.9.2): slang
    // gives one element per member, in member order.
    E structValue(const slang::ast::Expression& e) {
        using namespace slang::ast;
        const Type& t = e.type->getCanonicalType();
        if (t.isUnpackedUnion()) unsupported("assignment pattern of an unpacked union");
        if (e.kind == ExpressionKind::ReplicatedAssignmentPattern) unsupported("replicated pattern of a struct");
        const auto& ap = static_cast<const AssignmentPatternExpressionBase&>(e);
        std::vector<const FieldSymbol*> fs;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) fs.push_back(&f);
        auto els = ap.elements();
        if (els.size() != fs.size()) unsupported("struct pattern");
        std::vector<E> parts;
        for (size_t k = 0; k < fs.size(); k++) parts.push_back(storageOf(fs[k]->getType(), *els[k]));
        return parts.size() == 1 ? parts[0] : node(Op::Concat, std::move(parts));
    }
    // The default value of an unpacked struct (6.8): x for 4-state members,
    // 0 for 2-state members and reals, "" for strings.
    E structDefault(const slang::ast::Type& t0) {
        using namespace slang::ast;
        const auto& t = t0.getCanonicalType();
        std::vector<E> parts;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
            const Type& ft = f.getType().getCanonicalType();
            int w = uWidth(ft);
            if (ft.isUnpackedStruct()) parts.push_back(structDefault(ft));
            else if (ft.isIntegral() && ft.isFourState()) parts.push_back(konst(xfill(w)));
            else if (ft.isString()) parts.push_back(konst(make(mkstr("").v, 0, 64, 0)));
            else parts.push_back(konst(make(0, 0, w, 0)));
            if (t.isUnpackedUnion()) break;
        }
        if (t.isUnpackedUnion()) {
            int w = uWidth(t);
            return resize(parts[0], w, false, false);
        }
        return parts.size() == 1 ? parts[0] : node(Op::Concat, std::move(parts));
    }
    // $clog2, $countbits and friends (20.8, 20.9), $random / $dist_uniform
    // (20.14), $urandom / $urandom_range (18.13). Null if n is none of them.
    E sysFnNode(int id, std::vector<E> kids, int ctl = 0) {
        auto n = node(Op::SysFn, std::move(kids));
        n->hi = id;
        n->lo = ctl;
        return n;
    }
    // The control bits of $countbits: each argument is a 1-bit constant.
    int countCtl(std::span<const slang::ast::Expression* const> ctl) {
        using namespace slang::ast;
        int m = 0;
        for (const Expression* c : ctl) {
            const Expression& x = peel(*c);
            if (x.kind != ExpressionKind::IntegerLiteral) unsupported("$countbits control that is not a literal");
            auto bit = x.as<IntegerLiteral>().getValue()[0];
            m |= bit.value == slang::logic_t::Z_VALUE ? 8 : bit.isUnknown() ? 4 : bit.value ? 2 : 1;
        }
        return m;
    }
    // f(seed, ...) where seed is inout: one node gives {new seed, value}; the
    // seed is written back just before the statement, the value read after.
    E seededCall(int id, const slang::ast::Expression& seedArg, std::vector<E> more) {
        if (!stmtDepth) unsupported("$random(seed) outside a procedural statement");
        const slang::ast::Expression& sd = outArg(seedArg);
        std::vector<E> kids{expr(sd)};
        for (auto& m : more) kids.push_back(m);
        int tmp = fresh(64);
        Instr h; h.k = IK::Assign; h.sig = tmp; h.e = sysFnNode(id, kids);
        prog.emit(h);
        Instr w; w.k = IK::Assign;
        lhs(sd, w);
        int sw = (int)sd.type->getBitWidth();
        w.e = resize(slice(ref(tmp), 63, 32), sw, false, sd.type->isSigned());
        prog.emit(w);
        return resize(slice(ref(tmp), 31, 0), 32, false, true);
    }
    E sysFn(const slang::ast::CallExpression& call, std::string_view n) {
        if (E r = enumMethod(call)) return r;
        auto args = call.arguments();
        auto a = [&](size_t i) { return expr(*args[i]); };
        if (n == "$shortrealtobits" && args.size() == 1) return sysFnNode(SF_SR2BITS, {realOf(*args[0])});
        if (n == "$clog2" && args.size() == 1) return sysFnNode(SF_CLOG2, {a(0)});
        if (n == "$countones" && args.size() == 1) return sysFnNode(SF_COUNTBITS, {a(0)}, 2);
        if (n == "$countbits" && args.size() >= 2) return sysFnNode(SF_COUNTBITS, {a(0)}, countCtl(args.subspan(1)));
        if ((n == "$onehot" || n == "$onehot0") && args.size() == 1) {
            E c = sysFnNode(SF_COUNTBITS, {a(0)}, 2);
            return bin(n == "$onehot" ? Op::Eq : Op::Le, c, konst(make(1, 0, 32, 0, true)));
        }
        if (n == "$isunknown" && args.size() == 1)
            return bin(Op::Ne, sysFnNode(SF_COUNTBITS, {a(0)}, 12), konst(make(0, 0, 32, 0, true)));
        if (n == "$random") {
            if (args.empty()) return sysFnNode(SF_RANDOM, {});
            return seededCall(SF_RANDOM_SEED, *args[0], {});
        }
        if (n == "$dist_uniform" && args.size() == 3)
            return seededCall(SF_DIST_UNIFORM, *args[0], {resize(a(1), 32, args[1]->type->isSigned(), true),
                                                          resize(a(2), 32, args[2]->type->isSigned(), true)});
        {   // $dist_normal and the rest of 20.14 / Annex N
            static const std::pair<const char*, int> dists[] = {
                {"$dist_normal", SF_DIST_NORMAL}, {"$dist_exponential", SF_DIST_EXPONENTIAL},
                {"$dist_poisson", SF_DIST_POISSON}, {"$dist_chi_square", SF_DIST_CHI_SQUARE},
                {"$dist_t", SF_DIST_T}, {"$dist_erlang", SF_DIST_ERLANG}};
            for (auto& [dn, id] : dists)
                if (n == dn && !args.empty()) {
                    std::vector<E> more;
                    for (size_t i = 1; i < args.size(); i++) more.push_back(resize(a(i), 32, args[i]->type->isSigned(), true));
                    return seededCall(id, *args[0], more);
                }
        }
        if (n == "$system") return args.empty() ? sysFnNode(SF_SYSTEM, {}) : sysFnNode(SF_SYSTEM, {strOf(*args[0])});
        if (n == "$urandom") return args.empty() ? sysFnNode(SF_URANDOM, {}) : sysFnNode(SF_URANDOM_SEED, {a(0)});
        if (n == "$urandom_range" && !args.empty()) {
            std::vector<E> k{a(0)};
            if (args.size() > 1) k.push_back(a(1));
            return sysFnNode(SF_URANDOM_RANGE, k);
        }
        if (n == "$fopen" && !args.empty() && args.size() <= 2) {
            std::vector<E> k{strOf(*args[0])};
            if (args.size() == 2) k.push_back(strOf(*args[1]));
            return sysFnNode(SF_FOPEN, k, args.size() == 2 ? 1 : 0);
        }
        if (n == "$fclose" && args.size() == 1) return sysFnNode(SF_FCLOSE, {a(0)});
        if (n == "$fflush") return args.empty() ? sysFnNode(SF_FFLUSH, {}) : sysFnNode(SF_FFLUSH, {a(0)});
        if (n == "$fgetc" && args.size() == 1) return sysFnNode(SF_FGETC, {a(0)});
        if (n == "$ungetc" && args.size() == 2) return sysFnNode(SF_UNGETC, {a(0), a(1)});
        if (n == "$feof" && args.size() == 1) return sysFnNode(SF_FEOF, {a(0)});
        if (n == "$ftell" && args.size() == 1) return sysFnNode(SF_FTELL, {a(0)});
        if (n == "$fseek" && args.size() == 3) return sysFnNode(SF_FSEEK, {a(0), a(1), a(2)});
        if (n == "$rewind" && args.size() == 1) return sysFnNode(SF_FSEEK, {a(0)});
        if (n == "$fgets") return fgetsCall(call);
        if (n == "$fscanf") return scanCall(call, true);
        if (n == "$sscanf") return scanCall(call, false);
        if (n == "$fdisplay" || n == "$fdisplayb" || n == "$fdisplayo" || n == "$fdisplayh" ||
            n == "$fwrite" || n == "$fwriteb" || n == "$fwriteo" || n == "$fwriteh")
            return fileWriteCall(call, n);
        if (n == "$cast") return castCall(call);
        if (n == "$timeformat") {
            std::vector<E> k;
            for (size_t i = 0; i < args.size() && i < 4; i++) k.push_back(i == 2 ? strOf(*args[i]) : expr(*args[i]));
            return sysFnNode(SF_TIMEFORMAT, k);
        }
        if (n == "$test$plusargs" && args.size() == 1) return sysFnNode(SF_PLUS_TEST, {strOf(*args[0])});
        if (n == "$value$plusargs" && args.size() == 2) {
            // "N=%d": the prefix is the text before the %, the letter says how
            // to read the rest. The variable is written only on a match.
            const auto& f = peel(*args[0]);
            if (f.kind != slang::ast::ExpressionKind::StringLiteral) unsupported("$value$plusargs format that is not a literal");
            std::string fs(f.as<slang::ast::StringLiteral>().getValue());
            size_t pc = fs.find('%');
            if (pc == std::string::npos) unsupported("$value$plusargs format without a specifier");
            size_t q = pc + 1;
            while (q < fs.size() && isdigit((unsigned char)fs[q])) q++;
            if (q >= fs.size()) unsupported("$value$plusargs format");
            char c = (char)tolower((unsigned char)fs[q]);
            if (std::string_view("dhxobsfeg").find(c) == std::string_view::npos) unsupported("$value$plusargs format letter");
            if (!stmtDepth) unsupported("$value$plusargs outside a procedural statement");
            E pre = konst(mkstr(fs.substr(0, pc)));
            int found = fresh(32);
            Instr t; t.k = IK::Assign; t.sig = found; t.e = sysFnNode(SF_PLUS_TEST, {pre});
            prog.emit(t);
            Instr j; j.k = IK::JmpIfNot; j.e = ref(found);
            size_t jf = prog.emit(j);
            const slang::ast::Expression& dst = outArg(*args[1]);
            Instr w; w.k = IK::Assign;
            lhs(dst, w);
            E v = readAs(sysFnNode(SF_PLUS_VALUE, {pre}, c), c, *dst.type, "$value$plusargs");
            w.e = v;
            prog.emit(w);
            prog.code[jf].target = prog.here();
            return ref(found, true);
        }
        return nullptr;
    }
    // %p (21.2.1.6): the format text and arguments that print value v of
    // type t as an assignment pattern. Integral values print in decimal (as
    // unformatted), enums by name, strings in quotes, packed structs and
    // arrays as '{...}. A fixed-size array is expanded here, element by
    // element; a dynamic container is printed at run time by %Q.
    static std::string escPct(const std::string& s) {
        std::string r;
        for (char c : s) { r += c; if (c == '%') r += '%'; }
        return r;
    }
    void pfmtSingular(const slang::ast::Type& t0, E v, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        if (t.isString()) { fmt += "\"%s\""; out.push_back(v); return; }
        if (t.isFloating()) { fmt += "%R"; out.push_back(v); return; }
        if (t.kind == SymbolKind::EnumType) {
            // The name of a valid value, else the value as its base type.
            auto fb = node(Op::SFormat, {v});
            fb->fmt = "%0d";
            E name = fb;
            std::vector<const EnumValueSymbol*> vals;
            for (const auto& ev : t.as<EnumType>().values()) vals.push_back(&ev);
            for (size_t k = vals.size(); k-- > 0;) {
                const auto& cv = vals[k]->getValue();
                if (!cv.isInteger()) continue;
                E eq = bin(Op::CaseEq, v, svLit(cv.integer(), t.isSigned(), {}));
                name = mux(eq, konst(mkstr(std::string(vals[k]->name))), name);
            }
            fmt += "%s";
            out.push_back(name);
            return;
        }
        if (t.kind == SymbolKind::PackedStructType || t.kind == SymbolKind::PackedUnionType) {
            fmt += "'{";
            bool first = true;
            for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
                if (!first) fmt += ", ";
                first = false;
                fmt += escPct(std::string(f.name)) + ":";
                int w = (int)f.getType().getBitWidth();
                E part = slice(v, (int)f.bitOffset + w - 1, (int)f.bitOffset);
                if (f.getType().isSigned()) part = resize(part, w, false, true);
                pfmtSingular(f.getType(), part, fmt, out);
                if (t.kind == SymbolKind::PackedUnionType) break;   // only the first member
            }
            fmt += "}";
            return;
        }
        if (t.isIntegral()) { fmt += "%0d"; out.push_back(v); return; }
        unsupported("%p of this type");
    }
    // An unpacked struct prints with member names; a union, its first member (21.2.1.6).
    void pfmtStruct(const slang::ast::Type& t0, E v, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        fmt += "'{";
        bool first = true;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
            if (!first) fmt += ", ";
            first = false;
            fmt += escPct(std::string(f.name)) + ":";
            auto [off, w] = uField(t, f);
            E part = slice(v, off + w - 1, off);
            const Type& ft = f.getType().getCanonicalType();
            if (ft.isUnpackedStruct() || ft.isUnpackedUnion()) pfmtStruct(ft, part, fmt, out);
            else if (ft.isString()) pfmtSingular(ft, un(Op::AsStr, part), fmt, out);
            else if (ft.isFloating()) pfmtSingular(ft, un(Op::AsReal, part), fmt, out);
            else pfmtSingular(ft, ft.isSigned() ? resize(part, w, false, true) : part, fmt, out);
            if (t.isUnpackedUnion()) break;
        }
        fmt += "}";
    }
    void pfmt(const slang::ast::Expression& a, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        const Expression& x = peel(a);
        if (int d = dynSig(x); d >= 0) {
            const Type* et = x.type->getCanonicalType().getArrayElementType();
            const Type& ec = et->getCanonicalType();
            if (!(ec.isIntegral() && ec.kind != SymbolKind::EnumType && !ec.isStruct()) && !ec.isString() && !ec.isFloating())
                unsupported("%p of a container of this element type");
            fmt += "%Q";
            fmt += ec.isString() ? 's' : ec.isFloating() ? 'R' : 'd';
            out.push_back(ref(d));
            return;
        }
        if (x.type->isUnpackedArray()) {
            const Symbol* sym = (x.kind == ExpressionKind::NamedValue || x.kind == ExpressionKind::HierarchicalValue)
                                    ? &x.as<ValueExpressionBase>().symbol : nullptr;
            auto ap = sym ? arrFind(sym) : nullptr;
            if (!ap) unsupported("%p of an unpacked array expression");
            const ArrShape& A = *ap;
            const Type* et = x.type;
            for (size_t k = 0; k < A.dims.size(); k++) et = et->getCanonicalType().getArrayElementType();
            int flat = 0;
            std::function<void(size_t)> walk = [&](size_t dim) {
                auto [l, r] = A.dims[dim];
                int64_t n = (l > r ? l - r : r - l) + 1;
                fmt += "'{";
                for (int64_t k = 0; k < n; k++) {
                    if (k) fmt += ", ";
                    if (dim + 1 < A.dims.size()) walk(dim + 1);
                    else if (et->isUnpackedStruct() || et->isUnpackedUnion()) pfmtStruct(*et, ref(A.base + flat++), fmt, out);
                    else pfmtSingular(*et, ref(A.base + flat++, A.sgn), fmt, out);
                }
                fmt += "}";
            };
            walk(0);
            return;
        }
        if (x.type->isUnpackedStruct() || x.type->isUnpackedUnion()) { pfmtStruct(*x.type, expr(x), fmt, out); return; }
        pfmtSingular(*x.type, expr(x), fmt, out);
    }
    template <typename Args>
    void formatInto(const Args& argv, char dflt, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        // The specifiers still waiting for an argument: where each one sits
        // in fmt, its length, and its letter. A %p is replaced by its
        // expansion once its argument is known.
        struct Spec { size_t pos, len; char c; };
        std::deque<Spec> pending;
        auto addSpecs = [&](const std::string& f, size_t at) {
            for (size_t p = 0; p < f.size(); p++) {
                if (f[p] != '%') continue;
                if (p + 1 < f.size() && f[p + 1] == '%') { p++; continue; }
                size_t q = p + 1;
                while (q < f.size() && (isdigit((unsigned char)f[q]) || f[q] == '.' || f[q] == '-')) q++;
                if (q < f.size() && f[q] != 'm' && f[q] != 'M')
                    pending.push_back({at + p, q - p + 1, (char)tolower((unsigned char)f[q])});
            }
        };
        for (const Expression* a : argv) {
            if (a->kind == ExpressionKind::EmptyArgument) { fmt += ' '; continue; }
            const Expression& pe = peel(*a);
            if (pe.kind == ExpressionKind::StringLiteral && pending.empty()) {
                std::string f = substM(std::string(pe.as<StringLiteral>().getValue()), hierName);
                size_t at = fmt.size();
                fmt += f;
                addSpecs(f, at);
                continue;
            }
            if (!pending.empty()) {
                Spec sp = pending.front();
                pending.pop_front();
                if (sp.c == 't') {   // %t (20.4.3): the value is in this module's units
                    const Expression& x = peel(*a);
                    E v = expr(*a);
                    if (x.type->isFloating()) v = bin(Op::FMul, realOf(*a), konst(mkreal((double)ticksPerUnit())));
                    else if (ticksPerUnit() != 1) v = bin(Op::Mul, resize(v, 64, x.type->isSigned(), false), konst(mk(ticksPerUnit(), 64)));
                    out.push_back(v);
                    continue;
                }
                if (sp.c == 'p') {
                    std::string x;
                    pfmt(*a, x, out);
                    fmt.replace(sp.pos, sp.len, x);
                    for (auto& o : pending) o.pos += x.size() - sp.len;
                    continue;
                }
            } else { fmt += '%'; fmt += a->type->isFloating() ? 'R' : a->type->isString() ? 's' : dflt; }
            out.push_back(expr(*a));
        }
    }

    // $fatal / $error / $warning / $info (20.10): a header line with the
    // severity, the file and line of the call, its scope, and the time, then
    // the user's message (formatted like $display) if there is one. $fatal
    // then ends the run like $finish, and msim exits with status 1.
    void severity(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (n == "$fatal" && !args.empty()) {
            const Expression& a0 = peel(*args[0]);
            if (a0.kind != ExpressionKind::StringLiteral) args = args.subspan(1);   // finish_number
        }
        std::string file = "?";
        size_t line = 0;
        if (const auto* sm = srcMgr()) {
            auto loc = sm->getFullyOriginalLoc(call.sourceRange.start());
            file = std::string(sm->getFileName(loc));
            line = sm->getLineNumber(loc);
        }
        const char* sev = n == "$fatal" ? "Fatal" : n == "$error" ? "Error" : n == "$warning" ? "Warning" : "Info";
        Instr i; i.k = IK::Display;
        hierName = scopeName(call);
        std::string escFile;   // a '%' in a file name prints as itself
        for (char c : file) { escFile += c; if (c == '%') escFile += '%'; }
        i.fmt = substM(std::string(sev) + ": \"" + escFile + "\", " + std::to_string(line) + ": %m: at time %0t", hierName);
        E t = node(Op::Time);
        t->k = mk(1, 64);   // %t takes kernel ticks
        i.args.push_back(t);
        if (!args.empty()) {
            i.fmt += "\n";
            formatInto(args, 'd', i.fmt, i.args);
        }
        prog.emit(i);
        if (n == "$fatal") {
            Instr f; f.k = IK::Finish; f.hi = 1;   // exit status 1
            prog.emit(f);
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
        if (n == "$fatal" || n == "$error" || n == "$warning" || n == "$info") { severity(call, n); return; }
        if (!call.arguments().empty() && dynSig(*call.arguments()[0]) >= 0) {   // q.push_back(x), aa.delete(k) ...
            dynMethod(call, false);
            return;
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
            hierName = scopeName(call);
            formatInto(args.subspan(1), 'd', f->fmt, f->kids);
            i.e = dst->type->isString() ? f : E(node(Op::StrToInt, {f}));
            if (!dst->type->isString()) i.e->hi = (int)dst->type->getBitWidth();
            prog.emit(i);
            return;
        }
        if (n == "$readmemh" || n == "$readmemb" || n == "$writememh" || n == "$writememb") {   // 21.4, 21.5
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
            auto ap = arrFind(msym);
            if (!ap) unsupported("$readmem target that is not an unpacked array");
            if (ap->dims.size() != 1) unsupported("$readmem into a multidimensional array");
            Instr i; i.k = n[1] == 'r' ? IK::ReadMem : IK::WriteMem;
            i.fmt = std::string(fn.as<StringLiteral>().getValue());
            i.arr = ap;
            i.hex = n.ends_with('h');
            for (size_t a = 2; a < args.size() && a < 4; a++) i.args.push_back(expr(*args[a]));
            prog.emit(i);
            return;
        }
        if (n == "$printtimescale") {   // 20.4.2
            auto expStr = [](int e) {
                static const char* u[] = {"s", "ms", "us", "ns", "ps", "fs"};
                int m = 1;
                while (((e % 3) + 3) % 3) { e--; m *= 10; }
                int k = -e / 3;
                return std::to_string(m) + (k >= 0 && k <= 5 ? u[k] : "?");
            };
            auto args = call.arguments();
            std::string name = scopeName(call);
            int u = tsUnit, pr = tsPrec;
            if (!args.empty()) {
                const Expression& x = peel(*args[0]);
                const Symbol* sym = x.kind == ExpressionKind::ArbitrarySymbol ? x.as<ArbitrarySymbolExpression>().symbol.get() : nullptr;
                if (sym && sym->kind == SymbolKind::Root) { name = "$root"; u = pr = simPrecExp(); }
                else if (sym && sym->kind == SymbolKind::Instance) {
                    name = sym->getHierarchicalPath();
                    auto ts = sym->as<InstanceSymbol>().body.getTimeScale();
                    if (ts) { u = tsExp(ts->base); pr = tsExp(ts->precision); }
                } else unsupported("$printtimescale of this argument");
            }
            Instr i; i.k = IK::Display;
            i.fmt = escPct("Time scale of (" + name + ") is " + expStr(u) + " / " + expStr(pr));
            prog.emit(i);
            return;
        }
        asTask = true;
        E rt = sysFn(call, n);
        asTask = false;
        if (E r = rt) {   // a system function called as a task: its effect only
            Instr i; i.k = IK::Assign; i.sig = fresh(64); i.e = resize(r, 64, false, false);
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
            // Waveform tasks only change what msim -vcd writes, so skipping
            // them changes no result. Any other task would be a silent wrong
            // answer if it were skipped, so it is an error.
            if (n == "$dumpfile" || n == "$dumpvars" || n == "$dumpon" || n == "$dumpoff" || n == "$dumpall" ||
                n == "$dumpflush" || n == "$dumplimit") {
                lowerDump(call, n);
                return;
            }
            static const std::set<std::string_view> harmless = {
                "$dumpports", "$fsdbDumpfile", "$fsdbDumpvars", "$fsdbDumpon", "$fsdbDumpoff",
                "$vcdpluson", "$vcdplusoff", "$vcdplusfile", "$shm_open", "$shm_probe", "$recordvars"};
            if (!harmless.count(n)) unsupported(("system task " + std::string(n)).c_str());
            std::cerr << "lower: ignoring system call " << n << "\n";
            return;
        }
        Instr i; i.k = k; i.nonl = nonl;
        hierName = scopeName(call);
        formatInto(call.arguments(), dflt, i.fmt, i.args);
        prog.emit(i);
    }

    // VCD tasks (21.7.1). $dumpvars scope and variable arguments become
    // signal-name prefixes: msim names a signal by its path below the top
    // module ("u1.x"), so top.u1 is "u1." and the top itself is "".
    void lowerDump(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        Instr i;
        i.k = IK::Dump;
        auto args = call.arguments();
        if (n == "$dumpfile") {
            i.dcmd = DC_FILE;
            if (!args.empty()) i.args.push_back(strOf(*args[0]));
        } else if (n == "$dumpvars") {
            i.dcmd = DC_VARS;
            if (!args.empty()) i.args.push_back(expr(*args[0]));
            for (size_t a = 1; a < args.size(); a++) {
                const Expression* x = args[a];
                const Symbol* sym = nullptr;
                if (x->kind == ExpressionKind::ArbitrarySymbol) sym = x->as<ArbitrarySymbolExpression>().symbol.get();
                else if (x->kind == ExpressionKind::NamedValue || x->kind == ExpressionKind::HierarchicalValue)
                    sym = &x->as<ValueExpressionBase>().symbol;
                if (!sym) unsupported("$dumpvars argument");
                std::string path = sym->getHierarchicalPath();
                size_t d = path.find('.');
                std::string rel = path.find("::") != std::string::npos ? path   // a package variable: "pkg::x"
                                : d == std::string::npos ? std::string() : path.substr(d + 1);
                bool var = sym->kind != SymbolKind::Instance;
                if (!var && !rel.empty()) rel.push_back('.');
                i.dsel.push_back({rel, var});
            }
        } else {
            i.dcmd = n == "$dumpon" ? DC_ON : n == "$dumpoff" ? DC_OFF : n == "$dumpall" ? DC_ALL
                   : n == "$dumpflush" ? DC_FLUSH : DC_LIMIT;
            if (i.dcmd == DC_LIMIT && !args.empty()) i.args.push_back(expr(*args[0]));
        }
        prog.emit(i);
    }

    // {<<s{...}} (11.4.14.2): the stream cut into s-bit slices from the
    // right, in reverse order, so the rightmost slice comes first and the
    // leftmost one, which may be shorter, comes last.
    static E reverseSlices(E v, int w, int s) {
        if (s <= 0 || s >= w) return v;
        std::vector<E> parts;
        for (int lo = 0; lo < w; lo += s) parts.push_back(slice(v, std::min(lo + s, w) - 1, lo));
        return concat(std::move(parts));
    }
    // The inverse, for unpacking (11.4.14.3): the first s bits of the stream
    // are the rightmost slice of the targets.
    static E unreverseSlices(E v, int w, int s) {
        if (s <= 0 || s >= w) return v;
        std::vector<E> parts;   // slices of the targets, rightmost first
        int hi = w - 1;
        for (int lo = 0; lo < w; lo += s) {
            int len = std::min(lo + s, w) - lo;
            parts.push_back(slice(v, hi, hi - len + 1));
            hi -= len;
        }
        std::reverse(parts.begin(), parts.end());
        return concat(std::move(parts));
    }
    // The bits of a streaming concatenation of integral operands, and fixed
    // unpacked arrays of them (elements left to right).
    E streamBits(const slang::ast::StreamingConcatenationExpression& sc, int& w) {
        using namespace slang::ast;
        std::vector<E> parts;
        w = 0;
        for (auto& st : sc.streams()) {
            if (st.withExpr) unsupported("with [] in a streaming concatenation");
            const Expression& op = peel(*st.operand);
            int pw = 0;
            if (op.kind == ExpressionKind::Streaming) {
                parts.push_back(streamBits(op.as<StreamingConcatenationExpression>(), pw));
            } else if (op.type->isIntegral()) {
                pw = (int)op.type->getBitWidth();
                parts.push_back(expr(op));
            } else if (isFixedArr(op) && op.kind == ExpressionKind::NamedValue) {
                auto it = arrMap().find(&op.as<NamedValueExpression>().symbol);
                if (it == arrMap().end() || it->second->dims.size() != 1 ||
                    !op.type->getArrayElementType()->isIntegral())
                    unsupported("this array in a streaming concatenation");
                int ew = (int)op.type->getArrayElementType()->getBitWidth();
                auto r = op.type->getCanonicalType().as<FixedSizeUnpackedArrayType>().range;
                (void)r;
                for (int k = 0; k < it->second->count; k++) {   // foreach order: left index first
                    parts.push_back(ref(it->second->base + k));
                    pw += ew;
                }
            } else {
                unsupported("this operand in a streaming concatenation");
            }
            w += pw;
        }
        needWidth(w);
        E v = parts.size() == 1 ? parts[0] : concat(std::move(parts));
        return reverseSlices(v, w, (int)sc.getSliceSize());
    }
    E streamTo(const slang::ast::StreamingConcatenationExpression& sc, int w) {
        int sw = 0;
        E v = streamBits(sc, sw);
        if (w == sw) return v;
        if (w < sw) return slice(v, sw - 1, sw - w);
        return concat({v, konst(mk(0, w - sw))});
    }

    // x matches p (12.6): x is held in a temporary, so pattern variables
    // bound along the way read a stable value.
    E matchExpr(const slang::ast::Expression& x, const slang::ast::Pattern& p,
                slang::ast::CaseStatementCondition cc) {
        int w = storeW(*x.type);
        needWidth(w);
        int v = fresh(w);
        Instr h; h.k = IK::Assign; h.sig = v; h.e = storageOf(*x.type, x); prog.emit(h);
        return matchP(p, ref(v), *x.type, w, cc);
    }
    // Whether v (w bits, the storage of a value of type t) matches p. A
    // pattern variable is bound here, whether or not the match succeeds.
    E matchP(const slang::ast::Pattern& p, E v, const slang::ast::Type& t0, int w,
             slang::ast::CaseStatementCondition cc) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        Op eqOp = cc == CaseStatementCondition::Normal ? Op::CaseEq
                  : cc == CaseStatementCondition::WildcardJustZ ? Op::CaseZEq : Op::CaseXEq;
        switch (p.kind) {
            case PatternKind::Wildcard: return konst(mk(1, 1));
            case PatternKind::Variable: {
                const auto& var = p.as<VariablePattern>().variable;
                int sig = fresh(w);
                sigmap[&var] = sig;
                Instr a; a.k = IK::Assign; a.sig = sig; a.e = v; prog.emit(a);
                return konst(mk(1, 1));
            }
            case PatternKind::Constant: {
                const Expression& ce = p.as<ConstantPattern>().expr;
                if (t.isFloating() || t.isString()) unsupported("constant pattern of a real or string");
                return bin(eqOp, v, resize(storageOf(t, ce), w, ce.type->isSigned(), false));
            }
            case PatternKind::Tagged: {
                const auto& tp = p.as<TaggedPattern>();
                if (!isTaggedUnion(t)) unsupported("tagged pattern");
                int tb = tagBitsOf(t);
                int idx = tagIndexOf(t, tp.member);
                E cond = tb > 0 ? bin(Op::CaseEq, slice(v, w - 1, w - tb), konst(mk((uint64_t)idx, tb)))
                                : konst(mk(1, 1));
                if (tp.valuePattern) {
                    const Type& mt = tp.member.getType();
                    int fw = t.isUnpackedUnion() ? uWidth(mt) : (int)mt.getBitWidth();
                    if (fw > 0) cond = bin(Op::LogAnd, cond, matchP(*tp.valuePattern, slice(v, fw - 1, 0), mt, fw, cc));
                }
                return cond;
            }
            case PatternKind::Structure: {
                const auto& sp = p.as<StructurePattern>();
                E cond = konst(mk(1, 1));
                for (auto& fp : sp.patterns) {
                    const Type& ft = fp.field->getType();
                    int off, fw;
                    if (t.isUnpackedStruct()) std::tie(off, fw) = uField(t, *fp.field);
                    else { off = (int)fp.field->bitOffset; fw = (int)ft.getBitWidth(); }
                    if (fw <= 0) unsupported("structure pattern member");
                    cond = bin(Op::LogAnd, cond, matchP(*fp.pattern, slice(v, off + fw - 1, off), ft, fw, cc));
                }
                return cond;
            }
            default: unsupported("pattern");
        }
    }
    // tagged M v (11.9): the tag on top, the member value right-justified.
    E taggedValue(const slang::ast::TaggedUnionExpression& te) {
        using namespace slang::ast;
        const Type& t = te.type->getCanonicalType();
        int w = t.isUnpackedUnion() ? uWidth(t) : (int)t.getBitWidth();
        needWidth(w);
        int tb = tagBitsOf(t);
        int idx = tagIndexOf(t, te.member);
        const Type& mt = te.member.as<FieldSymbol>().getType();
        int fw = 0;
        E val;
        if (te.valueExpr && !mt.isVoid()) {
            fw = t.isUnpackedUnion() ? uWidth(mt) : (int)mt.getBitWidth();
            val = t.isUnpackedUnion() ? storageOf(mt, *te.valueExpr)
                                      : resize(expr(*te.valueExpr), fw, te.valueExpr->type->isSigned(), false);
        }
        std::vector<E> parts;
        if (tb > 0) parts.push_back(konst(mk((uint64_t)idx, tb)));
        if (w - tb - fw > 0) parts.push_back(konst(mk(0, w - tb - fw)));
        if (val) parts.push_back(val);
        return parts.size() == 1 ? parts[0] : concat(std::move(parts));
    }

    // The parts of a concatenation assignment target, first (most
    // significant) first, with their offsets in the whole (11.4.12, 10.4).
    static void concatParts(const slang::ast::Expression& e, int hi,
                            std::vector<std::tuple<const slang::ast::Expression*, int, int>>& out) {
        using namespace slang::ast;
        const Expression& x = peel(e);
        if (x.kind != ExpressionKind::Concatenation) {
            int w = (int)x.type->getBitWidth();
            needWidth(w);
            out.push_back({&x, hi, hi - w + 1});
            return;
        }
        for (auto* op : x.as<ConcatenationExpression>().operands()) {
            int w = (int)op->type->getBitWidth();
            concatParts(*op, hi, out);
            hi -= w;
        }
    }

    // {a, b} = v; {a, b} <= v; {a, b} = #d v: v is evaluated once, into a
    // temporary as wide as the target, and each part takes its slice.
    void concatAssign(const slang::ast::AssignmentExpression& a) {
        using namespace slang::ast;
        int w = (int)a.left().type->getBitWidth();
        needWidth(w);
        std::vector<std::tuple<const Expression*, int, int>> parts;
        concatParts(a.left(), w - 1, parts);
        int tmp = fresh(w);
        Instr hold; hold.k = IK::Assign; hold.sig = tmp;
        const Expression* saved = lvalue;
        if (a.isCompound()) lvalue = &a.left();
        hold.e = resize(expr(a.right()), w, false, false);
        lvalue = saved;
        prog.emit(hold);
        Instr d;   // the delay of a <= #d v
        bool later = false;
        if (const TimingControl* tc = a.timingControl) {
            if (tc->kind == TimingControlKind::Delay && a.isNonBlocking()) {
                delayInto(tc->as<DelayControl>().expr, d);
                later = true;
            } else if (a.isNonBlocking()) {
                unsupported("nonblocking assignment with an intra-assignment event control");
            } else if (tc->kind == TimingControlKind::RepeatedEvent) {
                unsupported("repeat event control on a concatenation assignment");
            } else {
                timing(*tc);
            }
        }
        for (auto& [x, hi, lo] : parts) {
            Instr i;
            i.k = a.isNonBlocking() ? IK::Nba : IK::Assign;
            lhs(*x, i);
            if (i.dyn >= 0 && a.isNonBlocking()) unsupported("nonblocking write to a container element");
            i.e = slice(ref(tmp), hi, lo);
            if (later) { i.delay = d.delay; i.de = d.de; i.nbaLater = true; }
            prog.emit(i);
        }
    }

    // {>>{a, b}} = v; {<<8{a, b}} = v (11.4.14.3): the leftmost bits of v,
    // slices reversed for <<, are spread over the targets, first one on top.
    void streamAssign(const slang::ast::AssignmentExpression& a) {
        using namespace slang::ast;
        if (a.timingControl || a.isCompound()) unsupported("delayed or compound streaming assignment");
        const auto& sc = peel(a.left()).as<StreamingConcatenationExpression>();
        std::vector<std::pair<const Expression*, int>> tg;   // target, width
        int sw = 0;
        for (auto& st : sc.streams()) {
            if (st.withExpr) unsupported("with [] in a streaming concatenation");
            const Expression& op = peel(*st.operand);
            if (!op.type->isIntegral()) unsupported("this target of a streaming assignment");
            int w = (int)op.type->getBitWidth();
            tg.push_back({&op, w});
            sw += w;
        }
        needWidth(sw);
        const Expression& r = peel(a.right());
        int rw = (int)r.type->getBitWidth();
        if (!r.type->isIntegral() || rw < sw) unsupported("streaming assignment from a narrower or non-integral value");
        E v = expr(r);
        if (rw > sw) v = slice(v, rw - 1, rw - sw);
        int tmp = fresh(sw);
        Instr hold; hold.k = IK::Assign; hold.sig = tmp; hold.e = unreverseSlices(v, sw, (int)sc.getSliceSize());
        prog.emit(hold);
        int hi = sw - 1;
        for (auto& [x, w] : tg) {
            Instr i;
            i.k = a.isNonBlocking() ? IK::Nba : IK::Assign;
            lhs(*x, i);
            i.e = slice(ref(tmp), hi, hi - w + 1);
            prog.emit(i);
            hi -= w;
        }
    }

    // Assignments, ++/--, and system calls used as statements.
    void exprStmt(const slang::ast::Expression& ex) {
        using namespace slang::ast;
        if (ex.kind == ExpressionKind::Assignment) {
            const auto& a = ex.as<AssignmentExpression>();
            if (int d = dynSig(a.left()); d >= 0) {   // q = '{1, 2}; d = new[8]; aa = other;
                if (a.isNonBlocking() || a.timingControl || a.isCompound()) unsupported("this container assignment");
                assignContainer(d, a.right());
                return;
            }
            if (a.left().type->isUnpackedArray() && !a.timingControl && !a.isCompound() &&
                assignWholeArray(a.left(), a.right(), a.isNonBlocking()))
                return;
            if (peel(a.left()).kind == ExpressionKind::Concatenation) {
                concatAssign(a);
                return;
            }
            if (peel(a.left()).kind == ExpressionKind::Streaming) {
                streamAssign(a);
                return;
            }
            Instr i;
            i.k = a.isNonBlocking() ? IK::Nba : IK::Assign;
            lhs(a.left(), i);
            if (i.dyn >= 0 && (a.isNonBlocking() || a.timingControl))
                unsupported("nonblocking or delayed write to a container element");
            const Expression* saved = lvalue;
            if (a.isCompound()) lvalue = &a.left();
            i.e = expr(a.right());
            lvalue = saved;
            if (const TimingControl* tc = a.timingControl; tc && tc->kind != TimingControlKind::Delay) {
                // a = @(e) b; a = repeat (n) @(e) b (9.4.5): b is taken now and
                // written after the event (n events for repeat).
                if (a.isNonBlocking()) unsupported("nonblocking assignment with an intra-assignment event control");
                int w = (int)a.left().type->getBitWidth();
                needWidth(w);
                int tmp = fresh(w);
                Instr hold; hold.k = IK::Assign; hold.sig = tmp; hold.e = i.e;
                prog.emit(hold);
                if (tc->kind == TimingControlKind::RepeatedEvent) {
                    const auto& re = tc->as<RepeatedEventControl>();
                    int cnt = fresh(32);
                    Instr c0; c0.k = IK::Assign; c0.sig = cnt; c0.e = resize(expr(re.expr), 32, re.expr.type->isSigned(), true);
                    prog.emit(c0);
                    size_t top = prog.here();
                    Instr j; j.k = IK::JmpIfNot; j.e = bin(Op::Gt, ref(cnt, true), konst(make(0, 0, 32, 0, true)));
                    size_t jf = prog.emit(j);
                    timing(re.event);
                    Instr dec; dec.k = IK::Assign; dec.sig = cnt; dec.e = bin(Op::Sub, ref(cnt, true), konst(make(1, 0, 32, 0, true)));
                    prog.emit(dec);
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    prog.code[jf].target = prog.here();
                } else {
                    timing(*tc);
                }
                i.e = ref(tmp, a.left().type->isSigned());
                prog.emit(i);
                return;
            }
            if (const TimingControl* tc = a.timingControl) {   // a = #d b; a <= #d b (9.4.5)
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

    // foreach over a dynamic array or queue: index 0 up to size() - 1, read
    // again each pass. Over an associative array: first() then next().
    void foreachDyn(const slang::ast::ForeachLoopStatement& f, int d) {
        if (f.loopDims.size() != 1) unsupported("foreach over more than one dimension of a container");
        const auto* var = f.loopDims[0].loopVar;
        if (!var) { stmt(f.body); return; }
        const DynObj o = dynShape(d);
        loops.push_back({});
        if (o.kind != 2) {
            int v = fresh(32);
            bindVar(var, v);
            Instr init; init.k = IK::Assign; init.sig = v; init.e = konst(make(0, 0, 32, 0, true));
            prog.emit(init);
            size_t top = prog.here();
            Instr j; j.k = IK::JmpIfNot; j.e = bin(Op::Lt, ref(v, true), dynSize(d));
            size_t jf = prog.emit(j);
            stmt(f.body);
            patch(loops.back().continues, prog.here());
            Instr st; st.k = IK::Assign; st.sig = v;
            st.e = bin(Op::Add, ref(v, true), konst(make(1, 0, 32, 0, true)));
            prog.emit(st);
            Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
            prog.code[jf].target = prog.here();
        } else {
            int v = fresh(o.strKey ? 64 : o.keyW);
            bindVar(var, v);
            int ok = fresh(32);
            Instr first = dynCmd(d, D_FIRST); first.dkey = v; first.dres = ok;
            prog.emit(first);
            size_t top = prog.here();
            Instr j; j.k = IK::JmpIfNot; j.e = ref(ok);
            size_t jf = prog.emit(j);
            stmt(f.body);
            patch(loops.back().continues, prog.here());
            Instr nx = dynCmd(d, D_NEXT); nx.dkey = v; nx.dres = ok;
            prog.emit(nx);
            Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
            prog.code[jf].target = prog.here();
        }
        patch(loops.back().breaks, prog.here());
        loops.pop_back();
    }

    int stmtDepth = 0;   // > 0 while lowering a procedural statement
    // Source of a location, for coverage (set by lower() when -cov is on).
    static const slang::SourceManager*& covSM() { static const slang::SourceManager* sm = nullptr; return sm; }
    static void covLoc(slang::SourceLocation loc) {
        auto* sm = covSM();
        if (!sm || !loc.valid()) return;
        auto ol = sm->getFullyOriginalLoc(loc);
        g_emitLoc = {covFileId(std::string(sm->getFullPath(ol.buffer()).string())), (uint32_t)sm->getLineNumber(ol)};
    }
    void stmt(const slang::ast::Statement& s) {
        struct Depth { int& d; Depth(int& x) : d(x) { d++; } ~Depth() { d--; } } depth(stmtDepth);
        // Each statement's instructions carry its line; a block's own
        // instructions keep the line of the statement around them.
        SrcLoc savedLoc = g_emitLoc;
        struct RestoreLoc { SrcLoc& l; SrcLoc v; ~RestoreLoc() { g_emitLoc = v; } } restoreLoc{g_emitLoc, savedLoc};
        if (s.kind != slang::ast::StatementKind::List && s.kind != slang::ast::StatementKind::Block)
            covLoc(s.sourceRange.start());
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
                // Automatic variables declared in the fork block itself get
                // one frame per execution of the fork, shared by its branches
                // and set before they start (9.3.2: fork automatic int j = i;).
                FrameLayout* flay = nullptr;
                if (b.body.kind == StatementKind::List)
                    for (const Statement* st : b.body.as<StatementList>().list) {
                        if (st->kind != StatementKind::VariableDeclaration) continue;
                        const auto& v = st->as<VariableDeclStatement>().symbol;
                        if (v.lifetime != VariableLifetime::Automatic) continue;
                        const Type& vt = v.getType().getCanonicalType();
                        if (vt.isUnpackedArray() || vt.isEvent()) unsupported("automatic array, container or event declared in a fork block");
                        if (!flay) flay = newLayout("fork");
                        declLocal(v, flay);
                        const Expression* init = v.getInitializer();
                        f.init.push_back({localMap().at(&v).off, init ? expr(*init) : konst(initOf(v.getType()))});
                    }
                f.lay = flay;
                auto branch = [&](const Statement& st) {
                    Lowerer lw(sigmap);
                    lw.bind(subs, mkSig, tempSeq, store);
                    lw.tsUnit = tsUnit; lw.tsPrec = tsPrec;
                    lw.current = current;
                    lw.retSig = retSig;
                    // Each branch has a frame for its own automatic variables
                    // (and temporaries), unless it is plain static code.
                    FrameLayout* bl = newLayout("fork branch");
                    lw.frames = frames;
                    if (flay) lw.frames.push_back(flay);
                    lw.frames.push_back(bl);
                    lw.declLocals(st, bl);
                    bool framed = !frames.empty() || flay || !bl->w.empty();
                    if (!framed) lw.frames.clear();
                    f.blays.push_back(framed ? bl : nullptr);
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
                        // A static declaration in a fork block belongs to the parent.
                        if (st->kind == StatementKind::VariableDeclaration) {
                            if (st->as<VariableDeclStatement>().symbol.lifetime != VariableLifetime::Automatic) stmt(*st);
                            continue;
                        }
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
                if (hasRel(prog.code[jf].e)) {   // automatic variables: resolved when the wait starts
                    Instr w2; w2.k = IK::WaitEdge; w2.dynWait = true; w2.e = prog.code[jf].e;
                    prog.emit(w2);
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    prog.code[jbody].target = prog.here();
                    stmt(w.stmt);
                    break;
                }
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
                // if (a &&& b matches p &&& c) (12.6.2): each condition in
                // turn; a pattern binds its variables for the ones after it.
                const auto& c = s.as<ConditionalStatement>();
                std::vector<size_t> jfs;
                for (auto& cd : c.conditions) {
                    Instr j; j.k = IK::JmpIfNot;
                    j.e = cd.pattern ? matchExpr(*cd.expr, *cd.pattern, CaseStatementCondition::Normal)
                                     : expr(*cd.expr);
                    jfs.push_back(prog.emit(j));
                }
                stmt(c.ifTrue);
                if (c.ifFalse) {
                    Instr skip; skip.k = IK::Jmp;
                    size_t js = prog.emit(skip);
                    for (size_t jf : jfs) prog.code[jf].target = prog.here();
                    stmt(*c.ifFalse);
                    prog.code[js].target = prog.here();
                } else {
                    for (size_t jf : jfs) prog.code[jf].target = prog.here();
                }
                break;
            }
            case StatementKind::PatternCase: {   // case (x) matches (12.6.1)
                const auto& c = s.as<PatternCaseStatement>();
                int w = storeW(*c.expr.type);
                needWidth(w);
                int v = fresh(w);
                Instr h; h.k = IK::Assign; h.sig = v; h.e = storageOf(*c.expr.type, c.expr); prog.emit(h);
                std::vector<size_t> toEnd;
                for (auto& it : c.items) {
                    std::vector<size_t> next;
                    Instr j; j.k = IK::JmpIfNot; j.e = matchP(*it.pattern, ref(v), *c.expr.type, w, c.condition);
                    next.push_back(prog.emit(j));
                    if (it.filter) {
                        Instr g; g.k = IK::JmpIfNot; g.e = expr(*it.filter);
                        next.push_back(prog.emit(g));
                    }
                    stmt(*it.stmt);
                    Instr skip; skip.k = IK::Jmp;
                    toEnd.push_back(prog.emit(skip));
                    for (size_t x : next) prog.code[x].target = prog.here();
                }
                if (c.defaultCase) stmt(*c.defaultCase);
                for (size_t x : toEnd) prog.code[x].target = prog.here();
                break;
            }
            case StatementKind::ExpressionStatement:
                exprStmt(s.as<ExpressionStatement>().expr);
                break;
            case StatementKind::VariableDeclaration: {   // int x = 5;  inside a block
                const auto& vd = s.as<VariableDeclStatement>();
                const Expression* init = vd.symbol.getInitializer();
                if (localDyn().count(&vd.symbol)) {   // an automatic container starts empty at each entry
                    int d = dynFind(&vd.symbol);
                    prog.emit(dynCmd(d, D_DELETE));
                    if (init) assignContainer(d, *init);
                    break;
                }
                if (localArr().count(&vd.symbol)) {
                    if (init) { assignWholeArray(vd.symbol, vd.symbol.getType(), *init, false); break; }
                    auto a = arrFind(&vd.symbol);
                    const Type* et = &vd.symbol.getType().getCanonicalType();
                    while (et->isUnpackedArray()) et = &et->getArrayElementType()->getCanonicalType();
                    Val iv = initOf(*et);
                    for (int k = 0; k < a->count; k++) { Instr i; i.k = IK::Assign; i.sig = a->base + k; i.e = konst(iv); prog.emit(i); }
                    break;
                }
                if (localMap().count(&vd.symbol)) {   // an automatic variable starts over at each entry (6.21)
                    if (vd.symbol.getType().isEvent()) break;
                    Instr i; i.k = IK::Assign; i.sig = sigAt(&vd.symbol);
                    i.e = init ? expr(*init) : konst(initOf(vd.symbol.getType()));
                    prog.emit(i);
                    break;
                }
                if (auto it = dynMap().find(&vd.symbol); it != dynMap().end()) {
                    if (vd.symbol.lifetime == slang::ast::VariableLifetime::Static && current)
                        unsupported("static container in a task or function");
                    prog.emit(dynCmd(it->second, D_DELETE));   // a block entry starts it empty
                    if (init) assignContainer(it->second, *init);
                    break;
                }
                if (!init) {
                    // An automatic scalar starts over at its default on every
                    // entry (6.21), also in a function that keeps static signals.
                    const Type& vt = vd.symbol.getType().getCanonicalType();
                    if (vd.symbol.lifetime == VariableLifetime::Automatic && current && !vt.isUnpackedArray() &&
                        !vt.isEvent() && (vt.isIntegral() || vt.isFloating() || vt.isString())) {
                        Instr i; i.k = IK::Assign; i.sig = sigAt(&vd.symbol); i.e = konst(initOf(vt));
                        prog.emit(i);
                    }
                    break;
                }
                // A static local in a task or function is initialized on the
                // first call, then left alone. The flag starts as X, so the
                // first call fails the "already done" test and takes the init.
                bool once = current && vd.symbol.lifetime == slang::ast::VariableLifetime::Static;
                if (!once) {
                    Instr i; i.k = IK::Assign; i.sig = sigAt(&vd.symbol); i.e = expr(*init);
                    prog.emit(i);
                    break;
                }
                int flag = freshStatic(1);
                Instr j; j.k = IK::JmpIfNot; j.e = ref(flag);
                size_t jf = prog.emit(j);
                size_t js = emitJmp();
                prog.code[jf].target = prog.here();
                Instr set; set.k = IK::Assign; set.sig = flag; set.e = konst(mk(1, 1));
                prog.emit(set);
                Instr i; i.k = IK::Assign; i.sig = sigAt(&vd.symbol); i.e = expr(*init);
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
                        Instr i; i.k = IK::Assign; i.sig = sigAt(v); i.e = expr(*init);
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
                if (int d = dynSig(f.arrayRef); d >= 0) { foreachDyn(f, d); break; }
                struct Lvl { int var; int64_t right; bool up; size_t top, jf; };
                std::vector<Lvl> lv;
                for (const auto& d : f.loopDims) {
                    if (!d.loopVar) continue;
                    if (!d.range) unsupported("foreach over a dynamic dimension");
                    int v = fresh(32);
                    bindVar(d.loopVar, v);
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
                        E m = c.condition == CaseStatementCondition::Inside ? insideItem(c.expr, *x)
                                                                             : bin(cmp, expr(c.expr), expr(*x));
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
            case StatementKind::WaitOrder: {   // wait_order (a, b, c) pass; else fail (15.5.4)
                // For each event in turn, wait for it or any later one. A
                // later one triggering first is out of order and fails.
                const auto& w = s.as<WaitOrderStatement>();
                std::vector<int> ev;
                for (auto* x : w.events) ev.push_back(eventSig(*x));
                std::vector<size_t> toFail;
                for (size_t k = 0; k < ev.size(); k++) {
                    std::vector<int> snap;
                    std::vector<std::pair<int, Edge>> on;
                    for (size_t j = k; j < ev.size(); j++) {
                        int sn = fresh(32);
                        Instr c; c.k = IK::Assign; c.sig = sn; c.e = ref(ev[j]); prog.emit(c);
                        snap.push_back(sn);
                        on.push_back({ev[j], ANY});
                    }
                    size_t top = prog.here();
                    prog.emit(waitInstr(on));
                    for (size_t j = k + 1; j < ev.size(); j++) {
                        Instr f; f.k = IK::JmpIfNot; f.e = bin(Op::CaseEq, ref(ev[j]), ref(snap[j - k]));
                        toFail.push_back(prog.emit(f));
                    }
                    Instr again; again.k = IK::JmpIfNot; again.e = un(Op::LogNot, bin(Op::CaseEq, ref(ev[k]), ref(snap[0])));
                    again.target = top;
                    prog.emit(again);
                }
                if (w.ifTrue) stmt(*w.ifTrue);
                Instr skip; skip.k = IK::Jmp;
                size_t js = prog.emit(skip);
                for (size_t f : toFail) prog.code[f].target = prog.here();
                if (w.ifFalse) stmt(*w.ifFalse);
                else {   // no else: the failure is an error (15.5.4)
                    std::string file = "?";
                    size_t line = 0;
                    if (const auto* sm = srcMgr()) {
                        auto loc = sm->getFullyOriginalLoc(s.sourceRange.start());
                        file = std::string(sm->getFileName(loc));
                        line = sm->getLineNumber(loc);
                    }
                    Instr i; i.k = IK::Display;
                    i.fmt = "Error: \"" + file + "\", " + std::to_string(line) +
                            ": wait_order: events triggered out of order at time %0t";
                    E t = node(Op::Time); t->k = mk(1, 64);
                    i.args.push_back(t);
                    prog.emit(i);
                }
                prog.code[js].target = prog.here();
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
            case StatementKind::ProceduralAssign:     // assign / force (10.6)
            case StatementKind::ProceduralDeassign: { // deassign / release
                bool assign = s.kind == StatementKind::ProceduralAssign;
                const Expression* tgt;
                const Expression* val = nullptr;
                bool force;
                if (assign) {
                    const auto& pa = s.as<ProceduralAssignStatement>();
                    force = pa.isForce;
                    const Expression& a = pa.assignment;
                    if (a.kind != ExpressionKind::Assignment) unsupported("force / assign");
                    tgt = &a.as<AssignmentExpression>().left();
                    val = &a.as<AssignmentExpression>().right();
                } else {
                    const auto& pd = s.as<ProceduralDeassignStatement>();
                    force = pd.isRelease;
                    tgt = &pd.lvalue;
                }
                Instr i;
                lhs(*tgt, i);
                if (i.arr || i.bidx || i.hi >= 0 || i.dyn >= 0 || i.drv >= 0)
                    unsupported("force / assign of a bit, part, or array element (only whole signals)");
                i.k = assign ? IK::Force : IK::Release;
                i.hi = force ? 1 : 0;
                i.lo = 0;
                if (val) i.e = expr(*val);
                usesForce() = true;
                prog.emit(i);
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
// The empty shape of a container type (7.5, 7.8, 7.10).
inline DynObj Lowerer::dynShapeOf(const slang::ast::Type& t0) {
    using namespace slang::ast;
    const Type& t = t0.getCanonicalType();
    DynObj o;
    const Type* et = nullptr;
    if (t.isQueue()) {
        o.kind = 1;
        const auto& q = t.as<QueueType>();
        et = &q.elementType;
        if (q.maxBound) o.bound = (int64_t)q.maxBound;
    } else if (t.isAssociativeArray()) {
        o.kind = 2;
        const auto& a = t.as<AssociativeArrayType>();
        et = &a.elementType;
        if (a.indexType) {
            const Type& kt = a.indexType->getCanonicalType();
            if (kt.isString()) o.strKey = true;
            else if (kt.isIntegral() && kt.getBitWidth() <= 64) {
                o.keyW = (int)kt.getBitWidth();
                o.keySgn = kt.isSigned();
            } else unsupported("associative array index type");
        } else { o.keyW = 64; o.keySgn = false; }   // [*]: integral keys, unsigned order
    } else {
        et = &t.as<DynamicArrayType>().elementType;
    }
    const Type& e = et->getCanonicalType();
    if (e.isFloating()) o.dflt = mkreal(0);
    else if (e.isString()) o.dflt = mkstr("");
    else if (e.isIntegral()) {
        int w = (int)e.getBitWidth();
        if (w > 64) unsupported("container element wider than 64 bits");
        o.dflt = e.isFourState() ? allx(w) : make(0, 0, w);
        o.dflt.sgn = e.isSigned();
    } else unsupported("container element type");
    return o;
}

inline void Lowerer::declLocals(const slang::ast::Statement& st, FrameLayout* L) {
    using namespace slang::ast;
    struct W : ASTVisitor<W, VisitFlags::Statements> {
        Lowerer& lw;
        FrameLayout* L;
        W(Lowerer& l, FrameLayout* f) : lw(l), L(f) {}
        void handle(const BlockStatement& b) {
            if (b.blockKind == StatementBlockKind::Sequential) visitDefault(b);
            // fork: its declarations and branches get their own frames
        }
        void handle(const VariableDeclStatement& v) {
            if (v.symbol.lifetime == VariableLifetime::Automatic) lw.declLocal(v.symbol, L);
        }
        void handle(const ForLoopStatement& f) {
            for (const VariableSymbol* v : f.loopVars)
                if (v->lifetime == VariableLifetime::Automatic) lw.declLocal(*v, L);
            visitDefault(f);
        }
    } w(*this, L);
    st.visit(w);
}

inline bool enterInstance(const slang::ast::InstanceSymbol& inst) {
    using namespace slang::ast;
    if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return false;
    if (!inst.arrayPath.empty()) Lowerer::unsupported("instance array");
    // An interface instance (25.3) is lowered like a module instance: its
    // signals are named "bus.data", and its processes and tasks run as usual.
    if (!inst.isTopLevel() && !inst.isModule() && !inst.isInterface()) Lowerer::unsupported("non-module instance");
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
    std::vector<const slang::ast::Symbol*> aliased;   // nets that share another net's signal
    template <typename T> void reg(const T& s, bool net) {
        if (auto a = aliasOf().find(&s); a != aliasOf().end() && a->second != &s) { aliased.push_back(&s); return; }
        if (Lowerer::isDynType(s.getType())) { regDyn(s); return; }
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
    // A dynamic array, queue, or associative array: a hidden handle signal
    // (its name has '$') and its store entry.
    template <typename T> void regDyn(const T& s) {
        using namespace slang::ast;
        if (s.kind == SymbolKind::FormalArgument) Lowerer::unsupported("container as a task or function argument");
        DynObj o = Lowerer::dynShapeOf(s.getType());
        int id = mkSig(prefix + std::string(s.name) + "$", 64, false);
        dynStore()[id] = o;
        dynMap()[&s] = id;
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
    // A modport port (25.5) is another name for a signal of its interface.
    // They are aliased after every signal is registered.
    std::vector<const slang::ast::ModportPortSymbol*> modportPorts;
    void handle(const slang::ast::ModportPortSymbol& m) { modportPorts.push_back(&m); }
    // A package's variables are named "pkg::x" (26.3), apart from any module's x.
    void handle(const slang::ast::PackageSymbol& p) {
        std::string saved = prefix;
        prefix = std::string(p.name) + "::";
        visitDefault(p);
        prefix = std::move(saved);
    }
    void finishAliases() {
        for (const auto* s : aliased) {
            auto it = sigmap.find(aliasOf().at(s));
            if (it == sigmap.end()) Lowerer::unsupported("alias of a net that has no signal");
            sigmap[s] = it->second;
        }
    }
    void aliasModports() {
        for (const auto* m : modportPorts) {
            if (m->explicitConnection) Lowerer::unsupported("modport expression (25.5.4)");
            const slang::ast::Symbol* in = m->internalSymbol;
            if (!in) continue;
            if (auto it = sigmap.find(in); it != sigmap.end()) sigmap[m] = it->second;
            if (auto it = arrMap().find(in); it != arrMap().end()) arrMap()[m] = it->second;
            if (auto it = dynMap().find(in); it != dynMap().end()) dynMap()[m] = it->second;
        }
    }
    void handle(const slang::ast::NetSymbol& n) {
        reg(n, true);
        if (auto a = aliasOf().find(&n); a != aliasOf().end() && a->second != &n) return;
        using NK = slang::ast::NetType::NetKind;
        uint8_t k = NR_WIRE;
        switch (n.netType.netKind) {
            case NK::WAnd: case NK::TriAnd: k = NR_WAND; break;
            case NK::WOr: case NK::TriOr: k = NR_WOR; break;
            case NK::Tri0: k = NR_TRI0; break;
            case NK::Tri1: k = NR_TRI1; break;
            case NK::TriReg: k = NR_TRIREG; break;
            case NK::Supply0: k = NR_SUPPLY0; break;
            case NK::Supply1: k = NR_SUPPLY1; break;
            case NK::Interconnect: Lowerer::unsupported("interconnect net (6.6.8)");
            default: break;
        }
        if (k == NR_WIRE) return;
        if (n.getType().isUnpackedArray()) Lowerer::unsupported("an array of wand/wor/tri0/tri1/trireg/supply nets");
        netKinds().push_back({sigmap.at(&n), k});
    }
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
        if (pk != slang::ast::SymbolKind::InstanceBody && pk != slang::ast::SymbolKind::GenerateBlock &&
            pk != slang::ast::SymbolKind::Package) return;   // package variables too (26.2)
        if (auto it = dynMap().find(&v); it != dynMap().end()) {   // int q[$] = '{1, 2};
            if (!e) return;
            if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
            init->assignContainer(it->second, *e);
            return;
        }
        if (e && arrMap().count(&v)) {   // int a[3] = '{1, 2, 3};
            if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
            init->assignWholeArray(v, v.getType(), *e, false);
            return;
        }
        const auto& t = v.getType();
        if (!e && (t.isUnpackedStruct() || t.isUnpackedUnion())) {   // 7.2: each member's default
            if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
            Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v);
            i.e = init->structDefault(t);
            init->prog.emit(i);
            return;
        }
        // 6.8: with no initializer, a 2-state variable (int, bit, real) starts at 0.
        bool zero = !e && !t.isFourState() && (t.isIntegral() || t.isFloating() || t.isString());
        if (!e && !zero) return;
        if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
        Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v);
        i.e = e ? init->expr(*e) : konst(mk(0, (int)t.getBitWidth()));
        init->prog.emit(i);
    }
    // Module path delays (30.4): (a => b) = d delays every change of output
    // net b by d, like a net delay. Paths to one output must all have the same
    // delays, since minisim does not pick a path by which input changed last.
    std::map<int, std::vector<uint64_t>> pathDelays;
    void handle(const slang::ast::TimingPathSymbol& tp) {
        using namespace slang::ast;
        Lowerer d(sigmap);
        prep(d);
        d.setScope(tp.getParentScope());
        std::vector<uint64_t> dl;
        auto ds = tp.getDelays();
        if (ds.size() != 1 && ds.size() != 2 && ds.size() != 3) Lowerer::unsupported("a path delay with 6 or 12 values");
        for (const Expression* e : ds) {
            Instr t;
            d.delayInto(*e, t);
            if (t.de) Lowerer::unsupported("a path delay that is not constant");
            dl.push_back(t.delay);
        }
        for (const Expression* o : tp.getOutputs()) {
            const Expression& x = Lowerer::peel(*o);
            if (x.kind != ExpressionKind::NamedValue) Lowerer::unsupported("a path to a select of an output");
            const Symbol& sym = x.as<NamedValueExpression>().symbol;
            if (sym.kind != SymbolKind::Net) Lowerer::unsupported("a path delay to a variable output");
            int sg = sigmap.at(&sym);
            auto [it, fresh] = pathDelays.emplace(sg, dl);
            if (!fresh && it->second != dl) Lowerer::unsupported("paths with different delays to one output");
        }
    }
    void finishPaths() {
        for (auto& [sg, dl] : pathDelays) {
            bool any = false;
            for (auto v : dl) any |= v != 0;
            if (!any) continue;
            for (auto& nd : out.netDelays)
                if (nd.first == sg) Lowerer::unsupported("a path delay to a net that also has a net delay");
            out.netDelays.push_back({sg, dl});
        }
    }
    // `wire y = a & b;` (IEEE 1800-2023 10.3.1) is a continuous assignment to y.
    // `wire #d w;` is a net delay: every change of w's resolved value takes
    // d to arrive (28.16). `wire #d w = a;` is instead a delayed continuous
    // assignment (10.3.3), and the net itself has no delay.
    void handle(const slang::ast::NetSymbol& n) {
        const slang::ast::Expression* e = n.getInitializer();
        std::vector<uint64_t> dl;
        if (n.getDelay()) {
            Lowerer d(sigmap);
            prep(d);
            d.setScope(n.getParentScope());
            dl = d.delays3(n.getDelay());
        }
        if (!e) {
            if (!dl.empty()) out.netDelays.push_back({sigmap.at(&n), dl});
            return;
        }
        Lowerer lw(sigmap);
        prep(lw);
        lw.setScope(n.getParentScope());
        Instr i; i.k = IK::Assign; i.sig = sigmap.at(&n); i.e = lw.expr(*e);
        i.dly = dl;
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

    const std::vector<uint64_t>* gateDelay = nullptr;   // the delays of the gate being lowered
    void driveOut(const slang::ast::Expression& out, E rhs, uint8_t s0, uint8_t s1) {
        Lowerer lw(sigmap);
        prep(lw);
        Instr i;
        i.k = IK::Assign;
        i.s0 = s0;
        i.s1 = s1;
        if (gateDelay) i.dly = *gateDelay;
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
        Lowerer::covLoc(ca.location);
        Lowerer lw(sigmap);
        prep(lw);
        auto [ds0, ds1] = ca.getDriveStrength();
        if (Lowerer::peel(a.left()).kind == ExpressionKind::Concatenation) {
            // assign {c, s} = x + y: one driver per part, each driving its
            // slice of the right-hand side.
            int w = (int)a.left().type->getBitWidth();
            std::vector<std::tuple<const Expression*, int, int>> parts;
            Lowerer::concatParts(a.left(), w - 1, parts);
            bool first = true;
            for (auto& [x, hi, lo] : parts) {
                Lowerer pw(sigmap);
                if (!first) prep(pw);
                Lowerer& L = first ? lw : pw;
                first = false;
                Instr i; i.k = IK::Assign;
                applyStrength(i, ds0, ds1, STR_STRONG, STR_STRONG);
                L.lhs(*x, i);
                i.e = slice(resize(L.expr(a.right()), w, false, false), hi, lo);
                L.setScope(ca.getParentScope());
                i.dly = L.delays3(ca.getDelay());
                finishAssign(L, i);
            }
            g_emitLoc = {};
            return;
        }
        Instr i; i.k = IK::Assign;
        applyStrength(i, ds0, ds1, STR_STRONG, STR_STRONG);
        lw.lhs(a.left(), i);
        i.e = lw.expr(a.right());
        lw.setScope(ca.getParentScope());
        i.dly = lw.delays3(ca.getDelay());
        finishAssign(lw, i);
        g_emitLoc = {};
    }

    // buf/not/and/nand/or/nor/xor/xnor and pullup/pulldown drive the same way
    // as a continuous assign. Other primitives (mos, tran, bufif) do not.
    void handle(const slang::ast::PrimitiveInstanceSymbol& g) {
        using namespace slang::ast;
        std::vector<uint64_t> gdly;
        {
            Lowerer dl(sigmap);
            prep(dl);
            dl.setScope(g.getParentScope());
            gdly = dl.delays3(g.getDelay());
        }
        gateDelay = &gdly;
        struct Reset { const std::vector<uint64_t>*& p; ~Reset() { p = nullptr; } } reset{gateDelay};
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
            if (conn && conn->port.kind == SymbolKind::InterfacePort) continue;   // b.x names the instance's own signal (25.3)
            if (!conn || conn->port.kind != SymbolKind::Port)
                Lowerer::unsupported("port kind");
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

// Class methods are always automatic (8.6); other subroutines when declared
// so or inside an automatic module, program or interface (13.3.2).
inline bool isClassMethod(const slang::ast::SubroutineSymbol& s) {
    const slang::ast::Scope* p = s.getParentScope();
    return p && p->asSymbol().kind == slang::ast::SymbolKind::ClassType;
}
inline bool isAutomatic(const slang::ast::SubroutineSymbol& s) {
    return s.defaultLifetime == slang::ast::VariableLifetime::Automatic || isClassMethod(s);
}
// The automatic subroutines that need a frame per call: tasks (they can
// wait, so two calls can be live at once), class methods, and functions
// that recurse or fork. Any other automatic function runs one call at a
// time and to completion, so it keeps static signals: that is the same
// behavior (its locals are set again on every call) and -compile handles it.
inline std::unordered_set<const slang::ast::SubroutineSymbol*>
framedSubroutines(const std::vector<const slang::ast::SubroutineSymbol*>& all) {
    using namespace slang::ast;
    struct Scan : ASTVisitor<Scan, VisitFlags::Statements | VisitFlags::Expressions> {
        std::vector<const SubroutineSymbol*> callees;
        bool forks = false;
        void handle(const CallExpression& c) {
            if (!c.isSystemCall())
                if (auto* s = std::get<const SubroutineSymbol*>(c.subroutine)) callees.push_back(s);
            visitDefault(c);
        }
        void handle(const BlockStatement& b) {
            if (b.blockKind != StatementBlockKind::Sequential) forks = true;
            visitDefault(b);
        }
    };
    std::unordered_map<const SubroutineSymbol*, std::vector<const SubroutineSymbol*>> calls;
    std::unordered_set<const SubroutineSymbol*> out;
    for (auto* s : all) {
        Scan sc;
        s->getBody().visit(sc);
        calls[s] = std::move(sc.callees);
        if (!isAutomatic(*s)) continue;
        if (s->subroutineKind == SubroutineKind::Task || isClassMethod(*s) || sc.forks) out.insert(s);
    }
    // Automatic functions on a call cycle (13.4.2).
    for (auto* s : all) {
        if (!isAutomatic(*s) || out.count(s)) continue;
        std::vector<const SubroutineSymbol*> todo(calls[s].begin(), calls[s].end());
        std::unordered_set<const SubroutineSymbol*> seen;
        while (!todo.empty()) {
            auto* c = todo.back();
            todo.pop_back();
            if (c == s) { out.insert(s); break; }
            if (!seen.insert(c).second) continue;
            for (auto* d : calls[c]) todo.push_back(d);
        }
    }
    return out;
}

inline void rejectRecursive(const std::vector<Subr*>& subs) {
    enum class Color { White, Gray, Black };
    std::unordered_map<const Subr*, Color> color;
    for (Subr* s : subs) color[s] = Color::White;
    auto dfs = [&](auto& self, Subr* s) -> void {
        color[s] = Color::Gray;
        for (Subr* c : s->calls) {
            if (c->lay) continue;   // an automatic subroutine may recurse (13.4.2)
            if (color[c] == Color::Gray) Lowerer::unsupported("recursive static subroutine");
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
    localMap().clear();
    localArr().clear();
    localDyn().clear();
    layoutStore() = &out.layouts;
    netKinds().clear();
    usesForce() = false;
    srcMgr() = comp.getSourceManager();
    dynMap().clear();
    dynStore().clear();
    blockIds() = BlockIds{};
    Lowerer::covSM() = g_cov ? comp.getSourceManager() : nullptr;
    {   // alias statements (10.11), before any signal is made
        aliasOf().clear();
        struct AliasFinder : slang::ast::ASTVisitor<AliasFinder> {
            std::unordered_map<const slang::ast::Symbol*, const slang::ast::Symbol*> up;
            const slang::ast::Symbol* root(const slang::ast::Symbol* s) {
                while (up.count(s) && up[s] != s) s = up[s];
                return s;
            }
            void handle(const slang::ast::InstanceSymbol& inst) { if (enterInstance(inst)) visitDefault(inst); }
            void handle(const slang::ast::NetAliasSymbol& a) {
                using namespace slang::ast;
                const Symbol* first = nullptr;
                for (const Expression* e : a.getNetReferences()) {
                    const Expression& x = Lowerer::peel(*e);
                    if (x.kind != ExpressionKind::NamedValue && x.kind != ExpressionKind::HierarchicalValue)
                        Lowerer::unsupported("alias of part of a net");
                    const Symbol* s = &x.as<ValueExpressionBase>().symbol;
                    if (!up.count(s)) up[s] = s;
                    if (!first) { first = s; continue; }
                    const Symbol* ra = root(first), *rb = root(s);
                    if (ra != rb) up[rb] = ra;
                }
            }
        } af;
        comp.getRoot().visit(af);
        for (auto& [s, p] : af.up) aliasOf()[s] = af.root(s);
    }
    SigCollector sc(mkSig, sigmap);
    comp.getRoot().visit(sc);
    sc.finishAliases();
    sc.aliasModports();
    out.netKinds = netKinds();

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
    Lowerer setup(sigmap);
    auto framed = framedSubroutines(finder.found);
    for (const slang::ast::SubroutineSymbol* sym : finder.found) {
        if (subMap.count(sym)) continue;
        auto sub = std::make_unique<Subr>();
        out.subs.push_back(std::make_unique<Prog>());
        sub->prog = out.subs.back().get();
        sub->sym = sym;
        if (framed.count(sym)) {
            // Arguments, the result, and every automatic local are slots of
            // one frame per call (13.3.2), so the subroutine can recurse and
            // run in several processes at once.
            using namespace slang::ast;
            sub->lay = newLayout(std::string(sym->name));
            if (sym->returnValVar) {
                setup.declLocal(*sym->returnValVar, sub->lay);
                sub->retSig = localMap().at(sym->returnValVar).off;
                sub->retW = storeW(sym->getReturnType());
                if (sub->retW <= 0) Lowerer::unsupported("function return width");
                sub->retSgn = sym->getReturnType().isSigned();
            }
            for (const FormalArgumentSymbol* formal : sym->getArguments()) {
                if (Lowerer::isDynType(formal->getType()) || formal->getType().isUnpackedArray())
                    Lowerer::unsupported("array or container as an argument of an automatic subroutine");
                setup.declLocal(*formal, sub->lay);
                int w = storeW(formal->getType());
                if (w <= 0) Lowerer::unsupported("subroutine argument width");
                sub->args.push_back({localMap().at(formal).off, formal->direction, w, formal->getType().isSigned()});
            }
            setup.declLocals(sym->getBody(), sub->lay);
            subMap[sym] = sub.get();
            order.push_back(sub.get());
            owned.push_back(std::move(sub));
            continue;
        }
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
        if (sub->lay) { lw.frames = {sub->lay}; lw.retSig = sub->retSig >= 0 ? relSig(0, sub->retSig) : -1; }
        lw.current = sub;
        lw.stmt(sub->sym->getBody());
        *sub->prog = std::move(lw.prog);
    }
    for (Subr* sub : order) patchImplicit(*sub->prog);

    ProcCollector pc(sigmap, out, std::move(newDriver), &subMap, mkSig, &tempSeq);
    comp.getRoot().visit(pc);
    pc.finishPaths();
    pc.placeInit();
    rejectRecursive(order);
    for (const slang::ast::Symbol* b : blockIds().disabledFromOutside)
        if (!blockIds().roots.count(blockIds().of(b)))
            Lowerer::unsupported(("disable of block '" + std::string(b->name) +
                                  "' from outside it, unless it is a whole initial or fork branch").c_str());
    return out;
}
