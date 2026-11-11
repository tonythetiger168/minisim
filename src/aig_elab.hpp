#pragma once
// aig_elab.hpp - RTL to bits: walks slang's elaborated design and builds the
// and-inverter graph of aig.hpp.
//
// The hierarchy is flattened. Every net and variable is a row of Wire
// nodes; continuous assignments, port connections and processes give the
// wires their drivers, and cleanup() then resolves them.
//
// Processes are run symbolically. An always_comb / always @* body leaves
// each variable it assigns as a function of the block's inputs. A block on
// clock edges leaves each variable's next state, and a flop is made per bit;
// an asynchronous set or reset is the first `if` of the body, on a signal
// of the event list, assigning constants. Loops are unrolled (their bounds
// must be constant), functions and tasks are inlined, and break, continue
// and return become conditions on the assignments after them.
//
// Not synthesized: delays, initial blocks, system tasks, assertions (all
// ignored), latches, tristates, real numbers, dynamic types (errors).
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
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
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/AttributeSymbol.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/Type.h"
#include "slang/text/SourceManager.h"

#include "aig.hpp"

namespace aig {

struct ElabError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Elaborator {
    using Expression = slang::ast::Expression;
    using Statement = slang::ast::Statement;
    using Symbol = slang::ast::Symbol;
    using Type = slang::ast::Type;
    using EK = slang::ast::ExpressionKind;
    using SK = slang::ast::StatementKind;
    using SyK = slang::ast::SymbolKind;

    slang::ast::Compilation& comp;
    Design& des;
    Graph& g;
    const slang::SourceManager* sm;

    enum Mode { Cont, Blocking, NonBlocking };

    // The state of a process while it is run.
    struct Env {
        std::map<const Symbol*, Bits> blk, nba;
        Lit brk = L0, cont = L0, ret = L0, od = L0;   // od: dead in an enclosing loop or call
        std::map<int, Lit> wen;   // memory write ports reached so far: port -> its enable
    };
    // Memory ports seen while the current clocked block runs (Design::mems).
    struct PendingPort {
        const Symbol* sym;
        MemPort port;
        int ew, n;
    };
    std::vector<PendingPort> pendWr;
    std::vector<PendingPort> memRd;
    std::map<const Symbol*, size_t> memIdx;
    bool inSeq = false;
    Env cur;
    bool inProc = false;
    bool parallelCases = true;   // parallelCase() below
    bool forceLive = false;   // loop steps run even on paths that took a break
    int callDepth = 0;
    const Bits* emptyArg = nullptr;   // the value an EmptyArgument stands for (output ports)

    std::unordered_map<const Symbol*, Bits> sig;   // the wires of each net / variable
    std::unordered_map<const Symbol*, Bits> drv;   // their drivers (NOLIT: none yet)
    std::vector<const Symbol*> sigOrder;
    std::set<std::string> flopNames;
    std::set<std::string> warned;
    std::string topPrefix;

    // ---------------------------------------------------------- messages
    std::string where(slang::SourceLocation loc) const {
        if (!sm || !loc) return "";
        auto full = sm->getFullyOriginalLoc(loc);
        return std::string(sm->getFileName(full)) + ":" + std::to_string(sm->getLineNumber(full)) + ": ";
    }
    [[noreturn]] void fail(slang::SourceLocation loc, const std::string& what) const {
        throw ElabError(where(loc) + what);
    }
    [[noreturn]] void fail(const Expression& e, const std::string& what) const { fail(e.sourceRange.start(), what); }
    [[noreturn]] void fail(const Statement& s, const std::string& what) const { fail(s.sourceRange.start(), what); }
    void warn(slang::SourceLocation loc, const std::string& what) {
        std::string m = where(loc) + what;
        if (warned.insert(m).second) des.warnings.push_back(m);
    }

    // ---------------------------------------------------------- types
    int widthOf(const Type& t, slang::SourceLocation loc = {}) const {
        const Type& c = t.getCanonicalType();
        if (c.isFloating()) fail(loc, "real numbers are not synthesizable");
        if (!c.isFixedSize() || c.isString() || c.isClass() || c.isCHandle() || c.isEvent() || c.isVoid())
            fail(loc, "type '" + t.toString() + "' is not synthesizable");
        uint64_t w = c.getBitstreamWidth();
        if (w > (1u << 24)) fail(loc, "type '" + t.toString() + "' is too large to synthesize");
        return (int)w;
    }
    static bool arrayLike(const Type& t) {
        const Type& c = t.getCanonicalType();
        return c.isArray() || (c.isIntegral() && !c.isStruct() && !c.isEnum() && c.kind != SyK::PackedUnionType);
    }
    // Element width and range of something indexable.
    void arrayInfo(const Type& t, slang::ConstantRange& r, int& ew, slang::SourceLocation loc) const {
        const Type& c = t.getCanonicalType();
        if (c.isArray()) {
            r = c.getFixedRange();
            ew = widthOf(*c.getArrayElementType(), loc);
        } else if (c.isIntegral()) {
            r = c.getFixedRange();
            ew = 1;
        } else
            fail(loc, "cannot index type '" + t.toString() + "'");
    }
    // The position of element i, counted from the LSB end.
    static int64_t pos(const slang::ConstantRange& r, int64_t i) {
        return r.left >= r.right ? i - r.right : r.right - i;
    }
    static bool isLocal(const Symbol& s) {
        const slang::ast::Scope* sc = s.getParentScope();
        if (!sc) return false;
        auto k = sc->asSymbol().kind;
        return k == SyK::StatementBlock || k == SyK::Subroutine;
    }

    // ---------------------------------------------------------- constants
    Bits fromSV(const slang::SVInt& v, int w) {
        Bits r(w, L0);
        int n = (int)v.getBitWidth();
        for (int i = 0; i < w; i++) {
            if (i < n) {
                slang::logic_t b = v[i];
                r[i] = exactlyEqual(b, slang::logic_t::x) ? g.dontCare() : (!b.isUnknown() && (bool)b) ? L1 : L0;
            } else if (v.isSigned() && n > 0) {
                r[i] = r[n - 1];
            }
        }
        return r;
    }
    Bits fromCV(const slang::ConstantValue& cv, const Type& t, slang::SourceLocation loc) {
        int w = widthOf(t, loc);
        if (cv.isInteger()) return fromSV(cv.integer(), w);
        if (cv.isUnpacked()) {
            auto elems = cv.elements();
            const Type& c = t.getCanonicalType();
            Bits r(w, L0);
            if (c.isArray()) {
                const Type& et = *c.getArrayElementType();
                int ew = widthOf(et, loc);
                size_t n = elems.size();
                for (size_t k = 0; k < n; k++) {
                    Bits e = fromCV(elems[k], et, loc);
                    std::copy(e.begin(), e.end(), r.begin() + (n - 1 - k) * ew);
                }
                return r;
            }
            if (c.isUnpackedStruct()) {
                size_t k = 0;
                for (auto& f : c.as<slang::ast::UnpackedStructType>().fields) {
                    if (k >= elems.size()) break;
                    Bits e = fromCV(elems[k++], f->getType(), loc);
                    std::copy(e.begin(), e.end(), r.begin() + f->bitOffset);
                }
                return r;
            }
        }
        if (cv.isString()) {
            const std::string& s = cv.str();
            Bits r(w, L0);
            for (size_t k = 0; k < s.size(); k++)
                for (int b = 0; b < 8; b++) {
                    size_t p = (s.size() - 1 - k) * 8 + b;
                    if (p < (size_t)w) r[p] = ((s[k] >> b) & 1) ? L1 : L0;
                }
            return r;
        }
        fail(loc, "constant of type '" + t.toString() + "' is not synthesizable");
    }
    // The value of an expression that slang can fold (no signals in it).
    slang::ConstantValue tryConst(const Expression& e) {
        if (const auto* c = e.getConstant()) return *c;
        if (e.kind == EK::IntegerLiteral) return e.as<slang::ast::IntegerLiteral>().getValue();
        const slang::ast::Scope* sc = &comp.getRoot();
        slang::ast::ASTContext ac(*sc, slang::ast::LookupLocation::max);
        slang::ast::EvalContext ec(ac);
        return e.eval(ec);
    }
    // The value of a literal of an AIG word if every bit is constant.
    static bool constVal(const Bits& b, int64_t& v, bool sgn) {
        uint64_t u = 0;
        for (size_t i = 0; i < b.size(); i++) {
            if (b[i] > 1) return false;
            if (b[i] == L1) {
                if (i >= 64) return false;
                u |= 1ull << i;
            }
        }
        if (sgn && !b.empty() && b.size() < 64 && b.back() == L1) u |= ~0ull << b.size();
        v = (int64_t)u;
        return true;
    }

    // ---------------------------------------------------------- signals
    const Bits& wires(const Symbol& s) {
        auto it = sig.find(&s);
        if (it != sig.end()) return it->second;
        if (!slang::ast::ValueSymbol::isKind(s.kind)) fail(s.location, "'" + std::string(s.name) + "' is not a signal");
        int w = widthOf(s.as<slang::ast::ValueSymbol>().getType(), s.location);
        Bits b(w);
        for (int i = 0; i < w; i++) b[i] = g.wire();
        sigOrder.push_back(&s);
        drv[&s] = Bits(w, NOLIT);
        return sig.emplace(&s, std::move(b)).first->second;
    }
    Bits read(const Symbol& s) {
        if (inProc) {
            auto it = cur.blk.find(&s);
            if (it != cur.blk.end()) return it->second;
        }
        return wires(s);
    }
    Lit dead() {
        if (forceLive) return L0;
        return g.orl(g.orl(cur.od, cur.brk), g.orl(cur.cont, cur.ret));
    }

    // Write bits [off, off + val.size()) of a symbol.
    void write(const Symbol& s, int off, const Bits& val, Lit en, Mode m, slang::SourceLocation loc) {
        const Bits& w = wires(s);
        if (off < 0 || off + (int)val.size() > (int)w.size()) return;   // out of range: no effect
        if (m == Cont) {
            Bits& d = drv[&s];
            for (size_t i = 0; i < val.size(); i++) {
                Lit old = d[off + i];
                if (old != NOLIT && en == L1) warn(loc, "'" + std::string(s.name) + "' has more than one driver; the last one is used");
                d[off + i] = en == L1 ? val[i] : g.mux(en, val[i], old == NOLIT ? L0 : old);
            }
            return;
        }
        en = g.andl(en, lneg(dead()));
        if (en == L0) return;
        auto& tab = m == NonBlocking ? cur.nba : cur.blk;
        auto it = tab.find(&s);
        if (it == tab.end()) it = tab.emplace(&s, w).first;
        Bits& t = it->second;
        for (size_t i = 0; i < val.size(); i++) t[off + i] = g.mux(en, val[i], t[off + i]);
    }

    // ---------------------------------------------------------- selects
    // t = the LSB-end position of the element a dynamic index picks.
    Bits dynPos(const Expression& idx, const slang::ConstantRange& r, int64_t delta) {
        Bits b = expr(idx);
        bool sgn = idx.type->isSigned();
        int w = std::max<int>((int)b.size() + 2, 34);
        Bits x = Graph::extend(b, w, sgn);
        // little endian: i - right + delta; big endian: right - i + delta
        if (r.left >= r.right) return g.add(x, g.konst((uint64_t)(int64_t)(delta - r.right), w), L0);
        return g.add(g.bnot(x), g.konst((uint64_t)(int64_t)(r.right + delta + 1), w), L0);
    }
    Bits muxTree(const Bits& v, int ew, int n, const Bits& t, int bit, int lo) {
        if (lo >= n) return Bits(ew, L0);
        if (bit < 0) return Bits(v.begin() + (size_t)lo * ew, v.begin() + (size_t)(lo + 1) * ew);
        Bits e = muxTree(v, ew, n, t, bit - 1, lo);
        if (lo + (1 << bit) >= n) {
            for (auto& x : e) x = g.andl(x, lneg(t[bit]));
            return e;
        }
        Bits h = muxTree(v, ew, n, t, bit - 1, lo + (1 << bit));
        return g.bmux(t[bit], h, e);
    }
    // Element t (dynamic) of n elements of width ew; 0 when out of range.
    Bits pick(const Bits& v, int ew, int n, const Bits& t) {
        int k = 0;
        while ((1 << k) < n) k++;
        Bits r = muxTree(v, ew, n, t, k - 1, 0);
        Bits hi(t.begin() + std::min<size_t>(k, t.size()), t.end());
        Lit ok = lneg(g.redOr(hi));
        for (auto& x : r) x = g.andl(x, ok);
        return r;
    }

    Bits selectElem(const slang::ast::ElementSelectExpression& e) {
        slang::ConstantRange r;
        int ew;
        arrayInfo(*e.value().type, r, ew, e.sourceRange.start());
        Bits v = expr(e.value());
        int n = (int)r.width();
        Bits ib = expr(e.selector());
        int64_t i;
        if (constVal(ib, i, e.selector().type->isSigned())) {
            int64_t p = pos(r, i);
            if (p < 0 || p >= n) return Bits(ew, L0);
            return Bits(v.begin() + p * ew, v.begin() + (p + 1) * ew);
        }
        Bits t = dynPos(e.selector(), r, 0);
        Bits out = pick(v, ew, n, t);
        // A word of an unpacked array read from its registers: a memory read port.
        if (e.value().kind == EK::NamedValue && e.value().type->isUnpackedArray()) {
            const Symbol* sym = &e.value().as<slang::ast::ValueExpressionBase>().symbol;
            if (!inProc || (!cur.blk.count(sym))) {
                PendingPort pp;
                pp.sym = sym;
                pp.port.addr = t;
                pp.port.data = out;
                pp.ew = ew;
                pp.n = n;
                memRd.push_back(std::move(pp));
            }
        }
        return out;
    }
    // Where a range select starts (LSB-end element position): constant or dynamic.
    bool rangeStart(const slang::ast::RangeSelectExpression& e, const slang::ConstantRange& r, int cnt, int64_t& p,
                    Bits& dyn) {
        using RK = slang::ast::RangeSelectionKind;
        bool le = r.left >= r.right;
        if (e.getSelectionKind() == RK::Simple) {
            int64_t a, b;
            if (!constVal(expr(e.left()), a, e.left().type->isSigned()) ||
                !constVal(expr(e.right()), b, e.right().type->isSigned()))
                fail(e, "range select bounds must be constant");
            p = std::min(pos(r, a), pos(r, b));
            return true;
        }
        bool up = e.getSelectionKind() == RK::IndexedUp;
        // The element of the selection nearest the LSB end, relative to the base index.
        int64_t delta = le ? (up ? 0 : -(cnt - 1)) : (up ? -(cnt - 1) : 0);
        int64_t b;
        if (constVal(expr(e.left()), b, e.left().type->isSigned())) {
            p = le ? pos(r, b) + delta : pos(r, b) + delta;
            return true;
        }
        dyn = dynPos(e.left(), r, delta);
        return false;
    }
    Bits selectRange(const slang::ast::RangeSelectExpression& e) {
        slang::ConstantRange r;
        int ew;
        arrayInfo(*e.value().type, r, ew, e.sourceRange.start());
        int w = widthOf(*e.type, e.sourceRange.start());
        int cnt = w / std::max(ew, 1);
        Bits v = expr(e.value());
        int64_t p;
        Bits dyn;
        Bits out(w, L0);
        if (rangeStart(e, r, cnt, p, dyn)) {
            for (int i = 0; i < w; i++) {
                int64_t q = p * ew + i;
                if (q >= 0 && q < (int64_t)v.size()) out[i] = v[q];
            }
            return out;
        }
        // dynamic: shift right by dyn * ew
        Bits amt = ew == 1 ? dyn : g.mul(dyn, g.konst(ew, (int)dyn.size()));
        Lit negv = amt.back();
        amt.pop_back();
        Bits sh = g.shift(v, amt, false, false);
        for (int i = 0; i < w && i < (int)sh.size(); i++) out[i] = g.andl(sh[i], lneg(negv));
        return out;
    }

    // Assign to bits [off, off + val.size()) of what an lvalue expression names.
    void assignSlice(const Expression& e, int off, const Bits& val, Lit en, Mode m) {
        switch (e.kind) {
            case EK::NamedValue:
            case EK::HierarchicalValue:
                write(e.as<slang::ast::ValueExpressionBase>().symbol, off, val, en, m, e.sourceRange.start());
                return;
            case EK::Conversion:
                assignSlice(e.as<slang::ast::ConversionExpression>().operand(), off, val, en, m);
                return;
            case EK::MemberAccess: {
                const auto& ma = e.as<slang::ast::MemberAccessExpression>();
                if (ma.member.kind != SyK::Field) fail(e, "unsupported member in an assignment target");
                assignSlice(ma.value(), (int)ma.member.as<slang::ast::FieldSymbol>().bitOffset + off, val, en, m);
                return;
            }
            case EK::ElementSelect: {
                const auto& es = e.as<slang::ast::ElementSelectExpression>();
                slang::ConstantRange r;
                int ew;
                arrayInfo(*es.value().type, r, ew, e.sourceRange.start());
                int n = (int)r.width();
                Bits ib = expr(es.selector());
                int64_t i;
                if (constVal(ib, i, es.selector().type->isSigned())) {
                    int64_t p = pos(r, i);
                    if (p < 0 || p >= n) return;
                    assignSlice(es.value(), (int)p * ew + off, val, en, m);
                    return;
                }
                Bits t = dynPos(es.selector(), r, 0);
                // A word of an unpacked array written in a clocked block: a memory write port.
                if (inSeq && m != Cont && es.value().kind == EK::NamedValue && es.value().type->isUnpackedArray() &&
                    off >= 0 && off + (int)val.size() <= ew) {
                    Lit live = g.andl(en, lneg(dead()));
                    if (live != L0) {
                        PendingPort pp;
                        pp.sym = &es.value().as<slang::ast::ValueExpressionBase>().symbol;
                        pp.port.addr = t;
                        pp.port.data = val;
                        pp.port.lo = off;
                        pp.ew = ew;
                        pp.n = n;
                        cur.wen[(int)pendWr.size()] = live;
                        pendWr.push_back(std::move(pp));
                    }
                }
                for (int p = 0; p < n; p++) {
                    Lit hit = g.andl(en, g.eq(t, g.konst(p, (int)t.size())));
                    if (hit != L0) assignSlice(es.value(), p * ew + off, val, hit, m);
                }
                return;
            }
            case EK::RangeSelect: {
                const auto& rs = e.as<slang::ast::RangeSelectExpression>();
                slang::ConstantRange r;
                int ew;
                arrayInfo(*rs.value().type, r, ew, e.sourceRange.start());
                int w = widthOf(*e.type, e.sourceRange.start());
                int cnt = w / std::max(ew, 1), n = (int)r.width();
                int64_t p;
                Bits dyn;
                if (rangeStart(rs, r, cnt, p, dyn)) {
                    assignSlice(rs.value(), (int)p * ew + off, val, en, m);
                    return;
                }
                for (int q = 0; q + cnt <= n; q++) {
                    Lit hit = g.andl(en, g.eq(dyn, g.konst(q, (int)dyn.size())));
                    if (hit != L0) assignSlice(rs.value(), q * ew + off, val, hit, m);
                }
                return;
            }
            default: fail(e, "unsupported assignment target");
        }
    }
    void assignTo(const Expression& lhs, const Bits& val, Lit en, Mode m) {
        if (lhs.kind == EK::Concatenation) {   // {a, b} = ...: the last operand takes the low bits
            const auto& c = lhs.as<slang::ast::ConcatenationExpression>();
            int off = 0;
            auto ops = c.operands();
            for (size_t k = ops.size(); k-- > 0;) {
                int w = widthOf(*ops[k]->type, lhs.sourceRange.start());
                assignTo(*ops[k], Bits(val.begin() + off, val.begin() + off + w), en, m);
                off += w;
            }
            return;
        }
        if (lhs.kind == EK::Streaming) fail(lhs, "streaming concatenation as an assignment target is not supported");
        Bits v = val;
        v.resize(widthOf(*lhs.type, lhs.sourceRange.start()), L0);
        assignSlice(lhs, 0, v, en, m);
    }

    // ---------------------------------------------------------- expressions
    Lit truth(const Expression& e) { return g.redOr(expr(e)); }

    Bits resize(Bits b, const Expression& e, bool sgn) {
        int w = widthOf(*e.type, e.sourceRange.start());
        return Graph::extend(b, w, sgn);
    }

    Bits wildEq(const Expression& l, const Expression& r, bool xToo, bool zToo, Lit* out) {
        Bits a = expr(l);
        slang::ConstantValue cv = tryConst(r);
        Bits b = expr(r);
        b.resize(a.size(), L0);
        Bits t;
        for (size_t i = 0; i < a.size(); i++) {
            if (cv.isInteger() && i < cv.integer().getBitWidth()) {
                slang::logic_t bit = cv.integer()[(int32_t)i];
                if (bit.isUnknown()) {
                    bool isZ = exactlyEqual(bit, slang::logic_t::z);
                    if ((isZ && zToo) || (!isZ && xToo)) continue;   // a wildcard bit
                }
            }
            t.push_back(g.xnorl(a[i], b[i]));
        }
        *out = g.redAnd(t);
        return {};
    }

    Bits call(const slang::ast::CallExpression& c) {
        auto loc = c.sourceRange.start();
        if (c.isSystemCall()) {
            std::string name(c.getSubroutineName());
            auto args = c.arguments();
            if (name == "$signed" || name == "$unsigned") return expr(*args[0]);
            if (name == "$countones" || name == "$onehot" || name == "$onehot0") {
                Bits a = expr(*args[0]);
                int w = 1;
                while ((1 << w) <= (int)a.size()) w++;
                Bits sum = g.konst(0, w);
                for (Lit b : a) {
                    Bits one(w, L0);
                    one[0] = b;
                    sum = g.add(sum, one, L0);
                }
                int ow = widthOf(*c.type, loc);
                if (name == "$countones") return Graph::extend(sum, ow, false);
                Lit hi = g.redOr(Bits(sum.begin() + 1, sum.end()));
                Lit r = name == "$onehot" ? g.andl(sum[0], lneg(hi)) : lneg(hi);
                Bits o(ow, L0);
                o[0] = r;
                return o;
            }
            if (name == "$isunknown") return Bits(widthOf(*c.type, loc), L0);
            slang::ConstantValue cv = tryConst(c);
            if (cv.isInteger()) return fromSV(cv.integer(), widthOf(*c.type, loc));
            fail(loc, "system function " + name + " is not synthesizable");
        }
        const auto& sub = *std::get<0>(c.subroutine);
        if (callDepth > 256) fail(loc, "function '" + std::string(sub.name) + "' recurses too deeply");
        if (!inProc) {   // a call in a continuous assignment: run it as a little process
            inProc = true;
            Env saved = std::move(cur);
            cur = Env{};
            Bits r = call(c);
            cur = std::move(saved);
            inProc = false;
            return r;
        }
        auto formals = sub.getArguments();
        auto actuals = c.arguments();
        std::vector<Bits> in(formals.size());
        for (size_t k = 0; k < formals.size(); k++) {
            auto dir = formals[k]->direction;
            if (dir == slang::ast::ArgumentDirection::In) in[k] = expr(*actuals[k]);
            else if (dir == slang::ast::ArgumentDirection::Ref) fail(loc, "ref arguments are not synthesizable");
        }
        // Save what this function's own variables hold (a recursive call reuses them).
        std::vector<std::pair<const Symbol*, std::optional<Bits>>> saved;
        auto save = [&](const Symbol* s) {
            auto it = cur.blk.find(s);
            saved.push_back({s, it == cur.blk.end() ? std::nullopt : std::optional<Bits>(it->second)});
        };
        for (auto* f : formals) save(f);
        if (sub.returnValVar) save(sub.returnValVar);
        Lit sBrk = cur.brk, sCont = cur.cont, sRet = cur.ret, sOd = cur.od;
        bool sForce = forceLive;
        cur.od = dead();
        cur.brk = cur.cont = cur.ret = L0;
        forceLive = false;
        for (size_t k = 0; k < formals.size(); k++) {
            int w = widthOf(formals[k]->getType(), loc);
            Bits v = in[k];
            v.resize(w, L0);
            cur.blk[formals[k]] = v;
        }
        if (sub.returnValVar) cur.blk[sub.returnValVar] = Bits(widthOf(sub.returnValVar->getType(), loc), L0);
        callDepth++;
        subStack.push_back(&sub);
        stmt(sub.getBody());
        subStack.pop_back();
        callDepth--;
        cur.brk = sBrk; cur.cont = sCont; cur.ret = sRet; cur.od = sOd;
        forceLive = sForce;
        Bits result;
        if (sub.returnValVar) result = cur.blk[sub.returnValVar];
        std::vector<Bits> outs(formals.size());
        for (size_t k = 0; k < formals.size(); k++)
            if (formals[k]->direction != slang::ast::ArgumentDirection::In) outs[k] = cur.blk[formals[k]];
        for (auto& [s, v] : saved) {
            if (v) cur.blk[s] = *v;
            else cur.blk.erase(s);
        }
        for (size_t k = 0; k < formals.size(); k++)
            if (formals[k]->direction != slang::ast::ArgumentDirection::In) {
                const Expression* a = actuals[k];
                if (a->kind == EK::Assignment) a = &a->as<slang::ast::AssignmentExpression>().left();
                assignTo(*a, outs[k], L1, Blocking);
            }
        return result;
    }

    Bits pattern(const Expression& e) {
        auto loc = e.sourceRange.start();
        const Type& t = e.type->getCanonicalType();
        int w = widthOf(t, loc);
        std::vector<const Expression*> els;
        if (e.kind == EK::ReplicatedAssignmentPattern) {
            const auto& rp = e.as<slang::ast::ReplicatedAssignmentPatternExpression>();
            int64_t n;
            if (!constVal(expr(rp.count()), n, false)) fail(e, "replication count must be constant");
            for (int64_t k = 0; k < n; k++)
                for (auto* x : rp.elements()) els.push_back(x);
        } else
            for (auto* x : static_cast<const slang::ast::AssignmentPatternExpressionBase&>(e).elements()) els.push_back(x);
        Bits r(w, L0);
        if (t.isStruct() || t.isUnpackedStruct()) {
            size_t k = 0;
            for (auto& f : t.as<slang::ast::Scope>().membersOfType<slang::ast::FieldSymbol>()) {
                if (k >= els.size()) break;
                Bits v = expr(*els[k++]);
                v.resize(widthOf(f.getType(), loc), L0);
                std::copy(v.begin(), v.end(), r.begin() + f.bitOffset);
            }
            return r;
        }
        if (!arrayLike(t)) fail(e, "assignment pattern for type '" + e.type->toString() + "' is not supported");
        slang::ConstantRange rg;
        int ew;
        arrayInfo(t, rg, ew, loc);
        size_t n = els.size();
        for (size_t k = 0; k < n; k++) {
            Bits v = expr(*els[k]);
            v.resize(ew, L0);
            size_t at = (n - 1 - k) * ew;
            if (at + ew <= r.size()) std::copy(v.begin(), v.end(), r.begin() + at);
        }
        return r;
    }

public:
    Bits expr(const Expression& e) {
        using namespace slang::ast;
        auto loc = e.sourceRange.start();
        if (const auto* cv = e.getConstant(); cv && (cv->isInteger() || cv->isUnpacked()))
            return fromCV(*cv, *e.type, loc);
        switch (e.kind) {
            case EK::IntegerLiteral: return fromSV(e.as<IntegerLiteral>().getValue(), widthOf(*e.type, loc));
            case EK::UnbasedUnsizedIntegerLiteral: {
                auto v = e.as<UnbasedUnsizedIntegerLiteral>().getLiteralValue();
                return Bits(widthOf(*e.type, loc),
                            exactlyEqual(v, slang::logic_t::x) ? g.dontCare() : (!v.isUnknown() && (bool)v) ? L1 : L0);
            }
            case EK::StringLiteral: return fromCV(tryConst(e), *e.type, loc);
            case EK::NamedValue:
            case EK::HierarchicalValue: {
                const Symbol& s = e.as<ValueExpressionBase>().symbol;
                if (s.kind == SyK::Parameter) return fromCV(s.as<ParameterSymbol>().getValue(), *e.type, loc);
                if (s.kind == SyK::EnumValue) return fromCV(s.as<EnumValueSymbol>().getValue(), *e.type, loc);
                if (s.kind == SyK::Specparam) fail(e, "specparams are not synthesizable");
                return read(s);
            }
            case EK::EmptyArgument:
                if (!emptyArg) fail(e, "unexpected empty argument");
                return *emptyArg;
            case EK::MinTypMax: return expr(e.as<MinTypMaxExpression>().typ());
            case EK::Conversion: {
                const auto& cv = e.as<ConversionExpression>();
                const Type& from = cv.operand().type->getCanonicalType();
                if (from.isFloating() || e.type->getCanonicalType().isFloating()) {
                    slang::ConstantValue k = tryConst(e);
                    if (k.isInteger()) return fromSV(k.integer(), widthOf(*e.type, loc));
                    fail(e, "real numbers are not synthesizable");
                }
                Bits b = expr(cv.operand());
                return resize(std::move(b), e, from.isIntegral() && from.isSigned());
            }
            case EK::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                int w = widthOf(*e.type, loc);
                switch (u.op) {
                    case UnaryOperator::Plus: return expr(u.operand());
                    case UnaryOperator::Minus: return g.negate(expr(u.operand()));
                    case UnaryOperator::BitwiseNot: return g.bnot(expr(u.operand()));
                    case UnaryOperator::BitwiseAnd: return one(g.redAnd(expr(u.operand())), w);
                    case UnaryOperator::BitwiseOr: return one(g.redOr(expr(u.operand())), w);
                    case UnaryOperator::BitwiseXor: return one(g.redXor(expr(u.operand())), w);
                    case UnaryOperator::BitwiseNand: return one(lneg(g.redAnd(expr(u.operand()))), w);
                    case UnaryOperator::BitwiseNor: return one(lneg(g.redOr(expr(u.operand()))), w);
                    case UnaryOperator::BitwiseXnor: return one(lneg(g.redXor(expr(u.operand()))), w);
                    case UnaryOperator::LogicalNot: return one(lneg(truth(u.operand())), w);
                    case UnaryOperator::Preincrement:
                    case UnaryOperator::Predecrement:
                    case UnaryOperator::Postincrement:
                    case UnaryOperator::Postdecrement: {
                        if (!inProc) fail(e, "increment outside a process");
                        Bits old = expr(u.operand());
                        bool inc = u.op == UnaryOperator::Preincrement || u.op == UnaryOperator::Postincrement;
                        Bits nw = inc ? g.add(old, g.konst(1, (int)old.size()), L0)
                                      : g.sub(old, g.konst(1, (int)old.size()));
                        assignTo(u.operand(), nw, L1, Blocking);
                        bool pre = u.op == UnaryOperator::Preincrement || u.op == UnaryOperator::Predecrement;
                        return pre ? nw : old;
                    }
                }
                fail(e, "unsupported unary operator");
            }
            case EK::BinaryOp: return binary(e.as<BinaryExpression>());
            case EK::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1 || c.conditions[0].pattern) fail(e, "pattern in a conditional expression");
                Lit s = truth(*c.conditions[0].expr);
                if (s == L1) return resize(expr(c.left()), e, false);
                if (s == L0) return resize(expr(c.right()), e, false);
                Bits a = resize(expr(c.left()), e, false), b = resize(expr(c.right()), e, false);
                return g.bmux(s, a, b);
            }
            case EK::Concatenation: {
                const auto& c = e.as<ConcatenationExpression>();
                Bits r;
                auto ops = c.operands();
                for (size_t k = ops.size(); k-- > 0;) {
                    if (ops[k]->type->isVoid()) continue;
                    Bits b = expr(*ops[k]);
                    r.insert(r.end(), b.begin(), b.end());
                }
                r.resize(widthOf(*e.type, loc), L0);
                return r;
            }
            case EK::Replication: {
                const auto& rp = e.as<ReplicationExpression>();
                int64_t n;
                if (!constVal(expr(rp.count()), n, false)) fail(e, "replication count must be constant");
                Bits b = expr(rp.concat()), r;
                for (int64_t k = 0; k < n; k++) r.insert(r.end(), b.begin(), b.end());
                r.resize(widthOf(*e.type, loc), L0);
                return r;
            }
            case EK::ElementSelect: return selectElem(e.as<ElementSelectExpression>());
            case EK::RangeSelect: return selectRange(e.as<RangeSelectExpression>());
            case EK::MemberAccess: {
                const auto& ma = e.as<MemberAccessExpression>();
                if (ma.member.kind != SyK::Field) fail(e, "unsupported member access");
                Bits v = expr(ma.value());
                size_t off = ma.member.as<FieldSymbol>().bitOffset;
                int w = widthOf(*e.type, loc);
                if (off + w > v.size()) fail(e, "member out of range");
                return Bits(v.begin() + off, v.begin() + off + w);
            }
            case EK::Call: return call(e.as<CallExpression>());
            case EK::Assignment: {
                if (!inProc) fail(e, "assignment inside an expression outside a process");
                assignment(e.as<AssignmentExpression>());
                return expr(e.as<AssignmentExpression>().left());
            }
            case EK::LValueReference:
                if (!lvalRef) fail(e, "unexpected lvalue reference");
                return *lvalRef;
            case EK::Inside: {
                const auto& in = e.as<InsideExpression>();
                Bits a = expr(in.left());
                bool sgn = in.left().type->isSigned();
                Lit acc = L0;
                for (const Expression* it : in.rangeList()) {
                    if (it->kind == EK::ValueRange) {
                        const auto& vr = it->as<ValueRangeExpression>();
                        Bits lo = expr(vr.left()), hi = expr(vr.right());
                        size_t w = std::max({a.size(), lo.size(), hi.size()});
                        Bits x = Graph::extend(a, (int)w, sgn);
                        lo = Graph::extend(lo, (int)w, vr.left().type->isSigned());
                        hi = Graph::extend(hi, (int)w, vr.right().type->isSigned());
                        acc = g.orl(acc, g.andl(lneg(g.lt(x, lo, sgn)), lneg(g.lt(hi, x, sgn))));
                    } else {
                        Lit m;
                        wildEq(in.left(), *it, true, true, &m);
                        acc = g.orl(acc, m);
                    }
                }
                return one(acc, widthOf(*e.type, loc));
            }
            case EK::SimpleAssignmentPattern:
            case EK::StructuredAssignmentPattern:
            case EK::ReplicatedAssignmentPattern: return pattern(e);
            default: break;
        }
        slang::ConstantValue cv = tryConst(e);
        if (cv.isInteger()) return fromSV(cv.integer(), widthOf(*e.type, loc));
        fail(e, "unsupported expression (" + std::string(toString(e.kind)) + ")");
    }

private:
    const Bits* lvalRef = nullptr;
    Bits one(Lit l, int w) {
        Bits r(std::max(w, 1), L0);
        r[0] = l;
        return r;
    }

    // Records a word operator whose operands are not all constant (Design::ops).
    Bits note(WordOp::Kind k, const Bits& a, const Bits& b, Bits y, bool sgn) {
        bool ca = true, cb = true;
        for (Lit l : a) ca &= l <= 1;
        for (Lit l : b) cb &= l <= 1;
        if (!(ca && cb)) des.ops.push_back(WordOp{k, a, b, y, sgn});
        return y;
    }

    Bits binary(const slang::ast::BinaryExpression& b) {
        using BO = slang::ast::BinaryOperator;
        auto loc = b.sourceRange.start();
        int w = widthOf(*b.type, loc);
        bool sgn = b.left().type->isSigned() && b.right().type->isSigned();
        auto same = [&](Bits& x, Bits& y) {
            size_t m = std::max(x.size(), y.size());
            x = Graph::extend(x, (int)m, b.left().type->isSigned());
            y = Graph::extend(y, (int)m, b.right().type->isSigned());
        };
        switch (b.op) {
            case BO::LogicalAnd: {
                Lit l = truth(b.left());
                if (l == L0) return one(L0, w);
                return one(g.andl(l, truth(b.right())), w);
            }
            case BO::LogicalOr: {
                Lit l = truth(b.left());
                if (l == L1) return one(L1, w);
                return one(g.orl(l, truth(b.right())), w);
            }
            case BO::LogicalImplication: return one(g.orl(lneg(truth(b.left())), truth(b.right())), w);
            case BO::LogicalEquivalence: return one(g.xnorl(truth(b.left()), truth(b.right())), w);
            case BO::WildcardEquality:
            case BO::WildcardInequality: {
                Lit m;
                wildEq(b.left(), b.right(), true, true, &m);
                return one(lxorc(m, b.op == BO::WildcardInequality), w);
            }
            default: break;
        }
        Bits x = expr(b.left()), y = expr(b.right());
        switch (b.op) {
            case BO::Add: same(x, y); return Graph::extend(note(WordOp::Add, x, y, g.add(x, y, L0), false), w, false);
            case BO::Subtract: same(x, y); return Graph::extend(note(WordOp::Sub, x, y, g.sub(x, y), false), w, false);
            case BO::Multiply: same(x, y); return Graph::extend(note(WordOp::Mul, x, y, g.mul(x, y), sgn), w, false);
            case BO::Divide:
            case BO::Mod: {
                same(x, y);
                bool s = b.type->isSigned();
                Lit sx = s ? x.back() : L0, sy = s ? y.back() : L0;
                if (s) {
                    x = g.bmux(sx, g.negate(x), x);
                    y = g.bmux(sy, g.negate(y), y);
                }
                Bits q, r;
                g.divmod(x, y, q, r);
                if (b.op == BO::Divide) {
                    if (s) q = g.bmux(g.xorl(sx, sy), g.negate(q), q);
                    return Graph::extend(q, w, false);
                }
                if (s) r = g.bmux(sx, g.negate(r), r);
                return Graph::extend(r, w, false);
            }
            case BO::BinaryAnd: same(x, y); return g.band(x, y);
            case BO::BinaryOr: same(x, y); return g.bor(x, y);
            case BO::BinaryXor: same(x, y); return g.bxor(x, y);
            case BO::BinaryXnor: same(x, y); return g.bnot(g.bxor(x, y));
            case BO::Equality:
            case BO::CaseEquality: same(x, y); return one(note(WordOp::Eq, x, y, Bits{g.eq(x, y)}, false)[0], w);
            case BO::Inequality:
            case BO::CaseInequality: same(x, y); return one(lneg(g.eq(x, y)), w);
            case BO::LessThan: same(x, y); return one(note(WordOp::Lt, x, y, Bits{g.lt(x, y, sgn)}, sgn)[0], w);
            case BO::GreaterThan: same(x, y); return one(note(WordOp::Lt, y, x, Bits{g.lt(y, x, sgn)}, sgn)[0], w);
            case BO::LessThanEqual: same(x, y); return one(lneg(g.lt(y, x, sgn)), w);
            case BO::GreaterThanEqual: same(x, y); return one(lneg(g.lt(x, y, sgn)), w);
            case BO::LogicalShiftLeft:
            case BO::ArithmeticShiftLeft: return Graph::extend(note(WordOp::Shl, x, y, g.shift(x, y, true, false), false), w, false);
            case BO::LogicalShiftRight: return Graph::extend(note(WordOp::Shr, x, y, g.shift(x, y, false, false), false), w, false);
            case BO::ArithmeticShiftRight:
                return Graph::extend(note(b.type->isSigned() ? WordOp::Sshr : WordOp::Shr, x, y, g.shift(x, y, false, b.type->isSigned()), false), w, false);
            case BO::Power: {
                int64_t base, ex;
                if (constVal(x, base, false) && base == 2) {
                    Bits o = g.konst(1, w);
                    return g.shift(o, y, true, false);
                }
                if (constVal(y, ex, false) && ex >= 0 && ex < 4096) {
                    Bits r = g.konst(1, w), m = Graph::extend(x, w, b.left().type->isSigned());
                    for (int64_t k = 0; k < ex; k++) r = g.mul(r, m);
                    return r;
                }
                fail(b, "** needs a constant exponent or a base of 2");
            }
            default: fail(b, "unsupported binary operator");
        }
    }

    void assignment(const slang::ast::AssignmentExpression& a) {
        Mode m = !inProc ? Cont : a.isNonBlocking() ? NonBlocking : Blocking;
        if (a.timingControl) warn(a.sourceRange.start(), "delay ignored");
        Bits v;
        if (a.isCompound()) {
            // a op= b: slang's right side is (lvalue op b), with the lvalue as a reference node.
            Bits old = expr(a.left());
            const Bits* prev = lvalRef;
            lvalRef = &old;
            v = expr(a.right());
            lvalRef = prev;
        } else
            v = expr(a.right());
        assignTo(a.left(), v, L1, m);
    }

    // ---------------------------------------------------------- statements
    template <class T, class E>
    void branch(Lit c, T thenFn, E elseFn) {
        if (c == L1) { thenFn(); return; }
        if (c == L0) { elseFn(); return; }
        Env saved = cur;
        thenFn();
        Env t = std::move(cur);
        cur = std::move(saved);
        elseFn();
        merge(cur, t, c);
    }
    // e = c ? t : e
    void merge(Env& e, Env& t, Lit c) {
        for (int pass = 0; pass < 2; pass++) {
            auto& te = pass ? t.nba : t.blk;
            auto& ee = pass ? e.nba : e.blk;
            for (auto& [s, tb] : te) {
                auto it = ee.find(s);
                if (it == ee.end()) it = ee.emplace(s, wires(*s)).first;
                it->second = g.bmux(c, tb, it->second);
            }
            for (auto& [s, eb] : ee)
                if (!te.count(s)) eb = g.bmux(c, wires(*s), eb);
        }
        for (auto& [id, tl] : t.wen) {
            auto it = e.wen.find(id);
            if (it == e.wen.end()) e.wen[id] = g.andl(c, tl);
            else it->second = g.mux(c, tl, it->second);
        }
        for (auto& [id, el] : e.wen)
            if (!t.wen.count(id)) el = g.andl(lneg(c), el);
        e.brk = g.mux(c, t.brk, e.brk);
        e.cont = g.mux(c, t.cont, e.cont);
        e.ret = g.mux(c, t.ret, e.ret);
        e.od = g.mux(c, t.od, e.od);
    }

    void declare(const slang::ast::VariableSymbol& v) {
        int w = widthOf(v.getType(), v.location);
        Bits val(w, L0);
        if (const Expression* init = v.getInitializer()) {
            val = expr(*init);
            val.resize(w, L0);
        }
        bool f = forceLive;
        forceLive = true;   // a declaration is not undone by an earlier break
        cur.blk.erase(&v);
        write(v, 0, val, L1, Blocking, v.location);
        forceLive = f;
    }

    struct LoopScope {
        Elaborator& el;
        Lit brk, cont, od;
        explicit LoopScope(Elaborator& e) : el(e), brk(e.cur.brk), cont(e.cur.cont), od(e.cur.od) {
            el.cur.od = el.g.orl(el.cur.od, el.g.orl(el.cur.brk, el.cur.cont));
            el.cur.brk = el.cur.cont = L0;
        }
        ~LoopScope() {
            el.cur.brk = brk;
            el.cur.cont = cont;
            el.cur.od = od;
        }
    };
    static constexpr int kMaxIter = 1 << 16;

    // Runs the body while cond() is constant true.
    template <class C, class S>
    void loop(const Statement& at, const Statement& body, C cond, S step, bool testFirst) {
        LoopScope ls(*this);
        for (int n = 0;; n++) {
            if (n > kMaxIter) fail(at, "loop does not end within 65536 iterations");
            if (testFirst || n > 0) {
                bool f = forceLive;
                forceLive = true;
                Lit c = cond();
                forceLive = f;
                if (c == L0) break;
                if (c != L1) fail(at, "loop condition is not constant: the loop cannot be unrolled");
            }
            if (cur.brk == L1 || cur.ret == L1) break;
            stmt(body);
            cur.cont = L0;
            bool f = forceLive;
            forceLive = true;
            step();
            forceLive = f;
        }
    }

    void caseStmt(const slang::ast::CaseStatement& c) {
        using CC = slang::ast::CaseStatementCondition;
        std::vector<Lit> hits;
        for (auto& item : c.items) {
            Lit h = L0;
            for (const Expression* ie : item.expressions) {
                Lit m;
                if (c.condition == CC::Inside && ie->kind == EK::ValueRange) {
                    const auto& vr = ie->as<slang::ast::ValueRangeExpression>();
                    Bits a = expr(c.expr), lo = expr(vr.left()), hi = expr(vr.right());
                    bool sgn = c.expr.type->isSigned();
                    size_t w = std::max({a.size(), lo.size(), hi.size()});
                    a = Graph::extend(a, (int)w, sgn);
                    lo = Graph::extend(lo, (int)w, sgn);
                    hi = Graph::extend(hi, (int)w, sgn);
                    m = g.andl(lneg(g.lt(a, lo, sgn)), lneg(g.lt(hi, a, sgn)));
                } else {
                    bool x = c.condition == CC::WildcardXOrZ || c.condition == CC::Inside;
                    bool z = x || c.condition == CC::WildcardJustZ;
                    wildEq(c.expr, *ie, x, z, &m);
                }
                h = g.orl(h, m);
            }
            hits.push_back(h);
        }
        // unique / priority case and (* full_case *) with no default: the
        // items cover every value that occurs, so the last one needs no test.
        bool full = c.check == slang::ast::UniquePriorityCheck::Unique || c.check == slang::ast::UniquePriorityCheck::Priority;
        for (auto* attr : comp.getAttributes(c))
            if (attr->name == "full_case") full = true;
        // Items that list every value of the selector are full too.
        if (!full && !c.defaultCase && c.condition == CC::Normal) {
            int w = widthOf(*c.expr.type, c.sourceRange.start());
            std::set<int64_t> vals;
            bool allConst = w <= 16;
            for (auto& item : c.items)
                for (const Expression* ie : item.expressions) {
                    int64_t v;
                    Bits b = allConst ? expr(*ie) : Bits{};
                    b.resize(w, L0);
                    if (allConst && constVal(b, v, false)) vals.insert(v);
                    else allConst = false;
                }
            if (allConst && vals.size() == (size_t)1 << w) full = true;
        }
        if (parallelCase(c, hits, full && !c.defaultCase)) return;
        if (full && !c.defaultCase && !hits.empty()) hits.back() = L1;
        std::function<void(size_t)> chain = [&](size_t k) {
            if (k == c.items.size()) {
                if (c.defaultCase) stmt(*c.defaultCase);
                return;
            }
            branch(hits[k], [&] { stmt(*c.items[k].stmt); }, [&] { chain(k + 1); });
        };
        chain(0);
    }

    // A case whose items cannot match together (unique case, parallel_case,
    // or distinct constant labels) needs no priority among them: each
    // variable that every item and the default assign is OR(hit_k & value_k)
    // instead of a chain of selects. False (and
    // nothing done) when the case is not of that kind. lastIsRest: a full
    // case with no default, whose last item takes every remaining value.
    // v as a select tree over `base`: returns data with keep set so that
    // v == keep ? base : data.
    Lit splitHold(Lit v, Lit base, Lit& keep, int depth) {
        keep = L0;
        if (v == base) { keep = L1; return L0; }
        Lit ms, mt, me;
        if (depth > 12 || !g.asMux(v, ms, mt, me)) return v;
        Lit kt, ke;
        Lit dt = splitHold(mt, base, kt, depth + 1), de = splitHold(me, base, ke, depth + 1);
        if (kt == L0 && ke == L0) return v;
        keep = g.mux(ms, kt, ke);
        return kt == L1 ? de : ke == L1 ? dt : g.mux(ms, dt, de);
    }
    bool parallelCase(const slang::ast::CaseStatement& c, const std::vector<Lit>& hits, bool lastIsRest) {
        using CC = slang::ast::CaseStatementCondition;
        if (!parallelCases || c.items.size() < 3) return false;
        bool par = c.check == slang::ast::UniquePriorityCheck::Unique || c.check == slang::ast::UniquePriorityCheck::Unique0;
        for (auto* attr : comp.getAttributes(c))
            if (attr->name == "parallel_case") par = true;
        if (!par && c.condition == CC::Normal) {
            int w = widthOf(*c.expr.type, c.sourceRange.start());
            std::set<int64_t> vals;
            par = w <= 62;
            for (auto& item : c.items)
                for (const Expression* ie : item.expressions) {
                    if (!par) break;
                    int64_t v;
                    Bits b = expr(*ie);
                    if ((int)b.size() > w) { par = false; break; }
                    b.resize(w, L0);
                    if (!constVal(b, v, false) || !vals.insert(v).second) par = false;
                }
        }
        if (!par) return false;
        for (Lit h : hits)
            if (h == L1) return false;
        // every item from the same starting point
        const Env saved = cur;
        size_t n = c.items.size() - (lastIsRest ? 1 : 0);
        std::vector<Env> envs(n);
        std::vector<Lit> hs(hits.begin(), hits.begin() + n);
        for (size_t k = 0; k < n; k++) {
            if (hs[k] == L0) { envs[k] = saved; continue; }
            cur = saved;
            stmt(*c.items[k].stmt);
            envs[k] = std::move(cur);
        }
        cur = saved;
        if (lastIsRest) stmt(*c.items.back().stmt);
        else if (c.defaultCase) stmt(*c.defaultCase);
        Env def = cur;
        // control state (break, return, memory write enables) by priority
        for (size_t k = n; k-- > 0;)
            if (hs[k] != L0) {
                Env t = envs[k];
                merge(cur, t, hs[k]);
            }
        Lit any = L0;
        for (Lit h : hs) any = g.orl(any, h);
        for (int pass = 0; pass < 2; pass++) {
            std::set<const Symbol*> syms;
            for (auto& e : envs)
                for (auto& [sy, b] : pass ? e.nba : e.blk) syms.insert(sy);
            for (auto& [sy, b] : pass ? def.nba : def.blk) syms.insert(sy);
            auto& out = pass ? cur.nba : cur.blk;
            for (const Symbol* sy : syms) {
                auto valueIn = [&](const Env& e, const Bits& base) -> const Bits& {
                    auto& m = pass ? e.nba : e.blk;
                    auto it = m.find(sy);
                    return it == m.end() ? base : it->second;
                };
                const Bits base = valueIn(saved, wires(*sy));
                const Bits& dv = valueIn(def, base);
                const Bits chain = out.at(sy);   // from the priority merge above
                Bits res(base.size());
                for (size_t i = 0; i < base.size(); i++) {
                    Lit terms = L0, changing = L0, holding = L0;
                    for (size_t k = 0; k < n; k++) {
                        if (hs[k] == L0) continue;
                        Lit v = valueIn(envs[k], base)[i];
                        // an item that itself keeps the value under a
                        // condition (if without else) changes it only on
                        // the rest of its hit
                        Lit h = hs[k];
                        if (v != base[i]) {
                            Lit keep = L0;
                            v = splitHold(v, base[i], keep, 0);
                            if (keep == L1) v = base[i];
                            else if (keep != L0) h = g.andl(h, lneg(keep));
                        }
                        if (v == base[i]) { holding = g.orl(holding, hs[k]); continue; }
                        if (h != hs[k]) holding = g.orl(holding, g.andl(hs[k], lneg(h)));
                        changing = g.orl(changing, h);
                        if (!g.isX(v)) terms = g.orl(terms, g.andl(h, v));
                    }
                    // Where an item or the default keeps the earlier value,
                    // the chain of selects stays: the hold condition is
                    // found there (aig::holdEnables), and that is worth
                    // more than the flatter form.
                    if (dv[i] == base[i] || holding != L0) res[i] = chain[i];
                    else res[i] = g.isX(dv[i]) ? terms : g.orl(terms, g.andl(lneg(any), dv[i]));
                }
                out[sy] = std::move(res);
            }
        }
        return true;
    }

    void foreachDims(const slang::ast::ForeachLoopStatement& f, size_t d) {
        if (d == f.loopDims.size()) {
            stmt(f.body);
            cur.cont = L0;
            return;
        }
        auto& dim = f.loopDims[d];
        if (!dim.loopVar) { foreachDims(f, d + 1); return; }
        if (!dim.range) fail(f, "foreach over a dynamic dimension is not synthesizable");
        int w = widthOf(dim.loopVar->getType(), f.sourceRange.start());
        int step = dim.range->left <= dim.range->right ? 1 : -1;
        for (int i = dim.range->left;; i += step) {
            cur.blk[dim.loopVar] = g.konst((uint64_t)(int64_t)i, w);
            foreachDims(f, d + 1);
            if (i == dim.range->right || cur.brk == L1 || cur.ret == L1) break;
        }
    }

public:
    void stmt(const Statement& s) {
        using namespace slang::ast;
        switch (s.kind) {
            case SK::Empty: return;
            case SK::List:
                for (auto* x : s.as<StatementList>().list) stmt(*x);
                return;
            case SK::Block: {
                const auto& b = s.as<BlockStatement>();
                if (b.blockKind != StatementBlockKind::Sequential) fail(s, "fork is not synthesizable");
                stmt(b.body);
                return;
            }
            case SK::ExpressionStatement: {
                const Expression& e = s.as<ExpressionStatement>().expr;
                if (e.kind == EK::Assignment) { assignment(e.as<AssignmentExpression>()); return; }
                if (e.kind == EK::Call && e.as<CallExpression>().isSystemCall()) return;   // $display ...
                expr(e);
                return;
            }
            case SK::VariableDeclaration: declare(s.as<VariableDeclStatement>().symbol); return;
            case SK::Conditional: {
                const auto& c = s.as<ConditionalStatement>();
                Lit cond = L1;
                for (auto& cd : c.conditions) {
                    if (cd.pattern) fail(s, "pattern matching is not supported");
                    cond = g.andl(cond, truth(*cd.expr));
                }
                branch(cond, [&] { stmt(c.ifTrue); }, [&] { if (c.ifFalse) stmt(*c.ifFalse); });
                return;
            }
            case SK::Case: caseStmt(s.as<CaseStatement>()); return;
            case SK::ForLoop: {
                const auto& f = s.as<ForLoopStatement>();
                for (auto* v : f.loopVars) declare(*v);
                {
                    bool fl = forceLive;
                    forceLive = true;
                    for (auto* i : f.initializers) expr(*i);
                    forceLive = fl;
                }
                loop(s, f.body, [&] { return f.stopExpr ? truth(*f.stopExpr) : L1; },
                     [&] { for (auto* st : f.steps) expr(*st); }, true);
                return;
            }
            case SK::RepeatLoop: {
                const auto& r = s.as<RepeatLoopStatement>();
                int64_t n;
                if (!constVal(expr(r.count), n, r.count.type->isSigned())) fail(s, "repeat count must be constant");
                int64_t k = 0;
                loop(s, r.body, [&] { return k < n ? L1 : L0; }, [&] { k++; }, true);
                return;
            }
            case SK::WhileLoop: {
                const auto& w = s.as<WhileLoopStatement>();
                loop(s, w.body, [&] { return truth(w.cond); }, [] {}, true);
                return;
            }
            case SK::DoWhileLoop: {
                const auto& w = s.as<DoWhileLoopStatement>();
                loop(s, w.body, [&] { return truth(w.cond); }, [] {}, false);
                return;
            }
            case SK::ForeachLoop: {
                LoopScope ls(*this);
                foreachDims(s.as<ForeachLoopStatement>(), 0);
                return;
            }
            case SK::Return: {
                const auto& r = s.as<ReturnStatement>();
                if (r.expr) {
                    const auto* sub = currentSub(s);
                    if (sub && sub->returnValVar) {
                        Bits v = expr(*r.expr);
                        v.resize(widthOf(sub->returnValVar->getType(), s.sourceRange.start()), L0);
                        write(*sub->returnValVar, 0, v, L1, Blocking, s.sourceRange.start());
                    }
                }
                cur.ret = g.orl(cur.ret, lneg(dead()));
                return;
            }
            case SK::Break: cur.brk = g.orl(cur.brk, lneg(dead())); return;
            case SK::Continue: cur.cont = g.orl(cur.cont, lneg(dead())); return;
            case SK::Timed: {
                const auto& t = s.as<TimedStatement>();
                if (t.timing.kind != TimingControlKind::Delay) fail(s, "event control inside a process is not synthesizable");
                warn(s.sourceRange.start(), "delay ignored");
                stmt(t.stmt);
                return;
            }
            case SK::ImmediateAssertion:
            case SK::ConcurrentAssertion: return;
            default: fail(s, "statement is not synthesizable (" + std::string(toString(s.kind)) + ")");
        }
    }

private:
    std::vector<const slang::ast::SubroutineSymbol*> subStack;
    const slang::ast::SubroutineSymbol* currentSub(const Statement&) { return subStack.empty() ? nullptr : subStack.back(); }

    // ---------------------------------------------------------- processes
    struct Edge {
        Lit sig;
        bool neg;
    };

    void edgesOf(const slang::ast::TimingControl& t, std::vector<Edge>& edges, bool& level, const Statement& at) {
        using namespace slang::ast;
        switch (t.kind) {
            case TimingControlKind::SignalEvent: {
                const auto& se = t.as<SignalEventControl>();
                if (se.iffCondition) fail(at, "iff in an event control is not supported");
                if (se.edge == EdgeKind::None) { level = true; return; }
                if (se.edge == EdgeKind::BothEdges) fail(at, "a flop on both edges is not synthesizable");
                Bits b = expr(se.expr);
                edges.push_back({b.empty() ? L0 : b[0], se.edge == EdgeKind::NegEdge});
                return;
            }
            case TimingControlKind::EventList:
                for (auto* ev : t.as<EventListControl>().events) edgesOf(*ev, edges, level, at);
                return;
            case TimingControlKind::ImplicitEvent: level = true; return;
            default: fail(at, "this timing control is not synthesizable");
        }
    }

    std::string flopBase(const Symbol& s) {
        std::string p = s.getHierarchicalPath();
        if (p.compare(0, topPrefix.size(), topPrefix) == 0) p = p.substr(topPrefix.size());
        std::string r;
        for (char c : p) {
            if (isalnum((unsigned char)c) || c == '_') r += c;
            else if (c == ']') continue;
            else r += '_';
        }
        return r;
    }

    void runBody(const Statement& body, Env& out) {
        cur = Env{};
        inProc = true;
        stmt(body);
        inProc = false;
        out = std::move(cur);
        cur = Env{};
    }
    static const Bits* lookup(const Env& e, const Symbol* s) {
        if (auto it = e.nba.find(s); it != e.nba.end()) return &it->second;
        if (auto it = e.blk.find(s); it != e.blk.end()) return &it->second;
        return nullptr;
    }

    void combBlock(const Statement& body, slang::SourceLocation loc) {
        Env e;
        runBody(body, e);
        std::vector<std::pair<const Symbol*, Bits>> outs;
        std::set<const Symbol*> seen;
        for (auto* tab : {&e.nba, &e.blk})
            for (auto& [s, b] : *tab)
                if (!isLocal(*s) && seen.insert(s).second) outs.push_back({s, b});
        // A bit that depends on its own wire holds its value: a latch.
        std::unordered_set<uint32_t> reach;
        std::vector<uint32_t> st;
        for (auto& [s, b] : outs)
            for (Lit l : b) st.push_back(lnode(l));
        std::unordered_set<uint32_t> vis;
        while (!st.empty()) {
            uint32_t n = st.back();
            st.pop_back();
            if (!vis.insert(n).second) continue;
            const Node& nd = g.nodes[n];
            if (nd.k == NK::And) { st.push_back(lnode(nd.a)); st.push_back(lnode(nd.b)); }
            else if (nd.k == NK::Wire) reach.insert(n);
        }
        for (auto& [s, b] : outs) {
            const Bits& w = wires(*s);
            bool any = false;
            for (size_t i = 0; i < b.size(); i++) {
                if (b[i] == w[i]) continue;   // not assigned here
                any = true;
                if (reach.count(lnode(w[i])))
                    fail(loc, "'" + std::string(s->name) + "' is not assigned on every path of this combinational block (latches are not supported)");
            }
            if (!any) continue;
            Bits& d = drv[s];
            for (size_t i = 0; i < b.size(); i++)
                if (b[i] != w[i]) {
                    if (d[i] != NOLIT) warn(loc, "'" + std::string(s->name) + "' has more than one driver; the last one is used");
                    d[i] = b[i];
                }
        }
    }

    static const Statement* unwrap(const Statement* s) {
        using namespace slang::ast;
        while (s) {
            if (s->kind == SK::Block) s = &s->as<BlockStatement>().body;
            else if (s->kind == SK::List && s->as<StatementList>().list.size() == 1) s = s->as<StatementList>().list[0];
            else break;
        }
        return s;
    }

    void seqBlock(std::vector<Edge> edges, const Statement& body, slang::SourceLocation loc) {
        using namespace slang::ast;
        const Statement* s = &body;
        bool hasRst = false;
        Lit rstC = L0;
        Env rst;
        while (edges.size() > 1) {
            if (hasRst) fail(loc, "a flop with both an asynchronous set and reset is not supported");
            s = unwrap(s);
            if (!s || s->kind != SK::Conditional || s->as<ConditionalStatement>().conditions.size() != 1)
                fail(loc, "a block on several edges must start with the `if` of its asynchronous reset");
            const auto& c = s->as<ConditionalStatement>();
            inProc = true;
            cur = Env{};
            Lit cond = truth(*c.conditions[0].expr);
            inProc = false;
            size_t hit = edges.size();
            for (size_t k = 0; k < edges.size(); k++)
                if (cond == lxorc(edges[k].sig, edges[k].neg)) hit = k;
            if (hit == edges.size())
                fail(loc, "the first `if` of the block does not test one of its edge signals at its active level");
            runBody(c.ifTrue, rst);
            hasRst = true;
            rstC = cond;
            edges.erase(edges.begin() + hit);
            s = c.ifFalse;
        }
        Env d;
        pendWr.clear();
        inSeq = true;
        if (s) runBody(*s, d);
        inSeq = false;
        // The memory write ports of this block: enable, clock, and the table entry.
        for (size_t k = 0; k < pendWr.size(); k++) {
            PendingPort& pp = pendWr[k];
            auto en = d.wen.find((int)k);
            pp.port.en = en == d.wen.end() ? L0 : en->second;
            if (hasRst) pp.port.en = g.andl(pp.port.en, lneg(rstC));
            if (pp.port.en == L0) continue;
            pp.port.clk = edges[0].sig;
            pp.port.negedge = edges[0].neg;
            auto [it, fresh] = memIdx.emplace(pp.sym, des.mems.size());
            if (fresh) {
                Memory m;
                m.name = flopBase(*pp.sym);
                m.width = pp.ew;
                m.depth = pp.n;
                des.mems.push_back(std::move(m));
            }
            des.mems[it->second].wr.push_back(std::move(pp.port));
        }
        pendWr.clear();
        std::vector<const Symbol*> syms;
        std::set<const Symbol*> seen;
        for (const Env* e : {&d, &rst})
            for (auto* tab : {&e->nba, &e->blk})
                for (auto& [sym, b] : *tab)
                    if (!isLocal(*sym) && seen.insert(sym).second) syms.push_back(sym);
        std::sort(syms.begin(), syms.end(), [&](const Symbol* a, const Symbol* b) {
            return wires(*a)[0] < wires(*b)[0];
        });
        for (const Symbol* sym : syms) {
            const Bits& w = wires(*sym);
            const Bits* dv = lookup(d, sym);
            const Bits* rv = hasRst ? lookup(rst, sym) : nullptr;
            std::string base = flopBase(*sym);
            Bits& dr = drv[sym];
            for (size_t i = 0; i < w.size(); i++) {
                Lit q = w[i];
                Lit dl = dv ? (*dv)[i] : q;
                Lit rl = rv ? (*rv)[i] : q;
                if (dl == q && rl == q) continue;
                Flop f;
                f.clk = edges[0].sig;
                f.negedge = edges[0].neg;
                f.d = dl;
                if (hasRst) {
                    if (rl == L0 || rl == L1) {
                        f.arst = rstC;
                        f.rval = rl == L1;
                    } else if (rl == q) {
                        f.d = g.mux(rstC, q, dl);   // not reset: it holds while the reset is active
                    } else
                        fail(loc, "'" + std::string(sym->name) + "' is loaded with a value that is not constant under the asynchronous reset");
                }
                f.var = base;
                f.bit = (int)i;
                std::string name = base + "_reg" + (w.size() > 1 ? "_" + std::to_string(i) + "_" : "");
                std::string uniq = name;
                for (int k = 1; !flopNames.insert(uniq).second; k++) uniq = name + "_" + std::to_string(k);
                f.name = uniq;
                if (dr[i] != NOLIT) warn(loc, "'" + std::string(sym->name) + "' has more than one driver; the last one is used");
                dr[i] = g.dff(f);
            }
        }
    }

    void process(const slang::ast::ProceduralBlockSymbol& p) {
        using namespace slang::ast;
        const Statement& body = p.getBody();
        switch (p.procedureKind) {
            case ProceduralBlockKind::Initial:
                warn(p.location, "initial block ignored");
                return;
            case ProceduralBlockKind::Final: return;
            case ProceduralBlockKind::AlwaysLatch: fail(p.location, "always_latch: latches are not supported");
            default: break;
        }
        if (body.kind == SK::Timed) {
            const auto& t = body.as<TimedStatement>();
            std::vector<Edge> edges;
            bool level = false;
            edgesOf(t.timing, edges, level, body);
            if (!edges.empty() && level) fail(p.location, "an event list mixing edges and levels is not synthesizable");
            if (!edges.empty()) seqBlock(edges, t.stmt, p.location);
            else combBlock(t.stmt, p.location);
            return;
        }
        if (p.procedureKind == ProceduralBlockKind::AlwaysComb) { combBlock(body, p.location); return; }
        fail(p.location, "an always block with no event control is not synthesizable");
    }

    void primitive(const slang::ast::PrimitiveInstanceSymbol& p) {
        std::string t(p.primitiveType.name);
        auto conns = p.getPortConnections();
        auto loc = p.location;
        auto in = [&](size_t k) {
            const Expression* e = conns[k];
            if (e->kind == EK::Assignment) e = &e->as<slang::ast::AssignmentExpression>().left();
            Bits b = expr(*e);
            return b.empty() ? L0 : b[0];
        };
        auto out = [&](size_t k, Lit v) {
            const Expression* e = conns[k];
            if (e->kind == EK::Assignment) e = &e->as<slang::ast::AssignmentExpression>().left();
            assignTo(*e, Bits{v}, L1, Cont);
        };
        if (t == "buf" || t == "not") {
            Lit v = lxorc(in(conns.size() - 1), t == "not");
            for (size_t k = 0; k + 1 < conns.size(); k++) out(k, v);
            return;
        }
        static const std::set<std::string> gates{"and", "nand", "or", "nor", "xor", "xnor"};
        if (!gates.count(t)) fail(loc, "primitive '" + t + "' is not synthesizable");
        Lit acc = in(1);
        for (size_t k = 2; k < conns.size(); k++) {
            Lit x = in(k);
            acc = (t == "and" || t == "nand") ? g.andl(acc, x) : (t == "or" || t == "nor") ? g.orl(acc, x) : g.xorl(acc, x);
        }
        out(0, lxorc(acc, t == "nand" || t == "nor" || t == "xnor"));
    }

    // ---------------------------------------------------------- hierarchy
    void connect(const slang::ast::InstanceSymbol& inst) {
        using namespace slang::ast;
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn) continue;
            if (conn->port.kind == SyK::InterfacePort) continue;   // members are reached by name
            if (conn->port.kind != SyK::Port) fail(inst.location, "port '" + std::string(conn->port.name) + "' of this kind is not supported");
            const auto& port = conn->port.as<PortSymbol>();
            if (!port.internalSymbol) fail(inst.location, "port '" + std::string(port.name) + "' has no internal signal");
            const Expression* e = conn->getExpression();
            const Bits& in = wires(*port.internalSymbol);
            if (port.direction == ArgumentDirection::In) {
                Bits v = e ? expr(*e) : Bits(in.size(), L0);
                v.resize(in.size(), L0);
                write(*port.internalSymbol, 0, v, L1, Cont, inst.location);
            } else if (port.direction == ArgumentDirection::Out) {
                if (!e) continue;
                if (e->kind == EK::Assignment) {
                    const auto& a = e->as<AssignmentExpression>();
                    const Bits* prev = emptyArg;
                    Bits iv = in;
                    emptyArg = &iv;
                    Bits v = expr(a.right());
                    emptyArg = prev;
                    assignTo(a.left(), v, L1, Cont);
                } else
                    assignTo(*e, in, L1, Cont);
            } else
                fail(inst.location, "inout port '" + std::string(port.name) + "' is not supported");
        }
    }

    void scope(const slang::ast::Scope& sc) {
        using namespace slang::ast;
        for (const Symbol& m : sc.members()) {
            switch (m.kind) {
                case SyK::Instance: {
                    const auto& inst = m.as<InstanceSymbol>();
                    connect(inst);
                    scope(inst.body);
                    break;
                }
                case SyK::InstanceArray: scope(m.as<InstanceArraySymbol>()); break;
                case SyK::GenerateBlock:
                    if (!m.as<GenerateBlockSymbol>().isUninstantiated) scope(m.as<GenerateBlockSymbol>());
                    break;
                case SyK::GenerateBlockArray: scope(m.as<GenerateBlockArraySymbol>()); break;
                case SyK::ContinuousAssign: {
                    const Expression& e = m.as<ContinuousAssignSymbol>().getAssignment();
                    if (e.kind != EK::Assignment) fail(m.location, "unsupported continuous assignment");
                    assignment(e.as<AssignmentExpression>());
                    break;
                }
                case SyK::ProceduralBlock: process(m.as<ProceduralBlockSymbol>()); break;
                case SyK::Net: {
                    const auto& n = m.as<NetSymbol>();
                    if (const Expression* init = n.getInitializer()) {
                        Bits v = expr(*init);
                        v.resize(wires(n).size(), L0);
                        write(n, 0, v, L1, Cont, n.location);
                    }
                    break;
                }
                case SyK::PrimitiveInstance: primitive(m.as<PrimitiveInstanceSymbol>()); break;
                case SyK::UninstantiatedDef:
                    fail(m.location, "module '" + std::string(m.as<UninstantiatedDefSymbol>().definitionName) + "' is not defined");
                default: break;
            }
        }
    }

public:
    Elaborator(slang::ast::Compilation& c, Design& d) : comp(c), des(d), g(d.g), sm(c.getSourceManager()) {}

    void run() {
        using namespace slang::ast;
        auto tops = comp.getRoot().topInstances;
        if (tops.empty()) throw ElabError("no top module");
        const InstanceSymbol& top = *tops[0];
        des.top = std::string(top.getDefinition().name);
        topPrefix = std::string(top.name) + ".";
        for (const Symbol* ps : top.body.getPortList()) {
            if (!ps) continue;
            if (ps->kind != SyK::Port) fail(ps->location, "top-level port '" + std::string(ps->name) + "' of this kind is not supported");
            const auto& port = ps->as<PortSymbol>();
            if (!port.internalSymbol) fail(port.location, "port '" + std::string(port.name) + "' has no internal signal");
            const Bits& w = wires(*port.internalSymbol);
            Port p;
            p.name = std::string(port.name);
            const Type& ct = port.getType().getCanonicalType();
            if (ct.isIntegral() && !ct.isStruct() && ct.isSimpleBitVector() && w.size() > 1) {
                auto r = ct.getFixedRange();
                p.msb = r.left;
                p.lsb = r.right;
            } else {
                p.msb = (int)w.size() - 1;
                p.lsb = 0;
            }
            if (port.direction == ArgumentDirection::In) {
                for (size_t i = 0; i < w.size(); i++) {
                    int idx = p.msb >= p.lsb ? p.lsb + (int)i : p.lsb - (int)i;
                    Lit in = g.input(w.size() > 1 ? p.name + "[" + std::to_string(idx) + "]" : p.name);
                    p.bits.push_back(in);
                    drv[port.internalSymbol][i] = in;
                }
            } else if (port.direction == ArgumentDirection::Out) {
                p.out = true;
                p.bits = w;
            } else
                fail(port.location, "inout port '" + p.name + "' is not supported");
            des.ports.push_back(std::move(p));
        }
        scope(top.body);
        for (auto& pp : memRd)
            if (auto it = memIdx.find(pp.sym); it != memIdx.end()) des.mems[it->second].rd.push_back(std::move(pp.port));
        // Give every wire its driver.
        for (const Symbol* s : sigOrder) {
            const Bits& w = sig[s];
            const Bits& d = drv[s];
            for (size_t i = 0; i < w.size(); i++) g.nodes[lnode(w[i])].a = d[i];
        }
    }
};

// Elaborate the compilation's first top module into a clean graph.
inline Design elaborate(slang::ast::Compilation& comp) {
    Design raw;
    Elaborator el(comp, raw);
    el.run();
    try {
        return cleanup(raw);
    } catch (std::runtime_error& e) {
        throw ElabError(std::string(e.what()) + " in the design (a net that depends on itself with no flop in between)");
    }
}

}  // namespace aig
