// lower_xprop.inc - X-propagation (msim -xprop), included in struct Lowerer.
//
// Standard Verilog is optimistic about an unknown control value: if (x)
// takes the else branch, a case with an x selector takes the default, and
// a clock going 0 -> x counts as a rising edge. With -xprop an always
// block treats such a control as "either could have happened": every
// branch that might be taken runs, and each variable they assign gets the
// merge of the results.
//   tmerge  bits on which all the branches agree keep their value, the
//           others become x (what a real mux or flop could output)
//   xmerge  every bit assigned in one of the branches becomes x
//
// All of it is done here, in lowering, with ordinary instructions, so the
// interpreter, -compile and every thread count run the same program. For
//     if (c) S1 else S2
// the code is
//         JmpIfNot (c is x) -> N ;  Jmp PRE
//     N:  JmpIfNot c -> E
//     T:  S1
//         JmpIfNot t -> Z ;  Jmp MID
//     E:  S2
//         JmpIfNot t -> Z ;  Jmp MID
//     Z:
// and, out of line (reached only with an unknown control):
//     PRE   t = 1; save every variable S1 or S2 assigns; go to T
//     MID   fold the variables into the merge, restore them, and go on to
//           the next branch that may be taken (or to POST after the last)
//     POST  write the merge to the variables; t = 0; go to Z
// A case statement and an else-if chain are one such region with more
// branches. A nonblocking assignment cannot be merged after it is
// scheduled, so in an unknown branch it is written to a shadow variable
// (value and assigned-bits mask) that is merged like any other variable
// and scheduled when the outermost unknown region ends.

enum { XC_DIRECT, XC_LAZY, XC_EAGER };
struct XpShadow { int n = -1, m = -1, w = 1, cls = XC_DIRECT; };
struct XpUnit { int sig = -1, hi = -1, lo = 0, w = 1; };   // hi < 0: the whole signal
struct XpRegionRec { std::string where, what, why; bool ok = false; };
static int& xpropMode() { static int m = 0; return m; }   // 0 off, 1 tmerge, 2 xmerge
static std::vector<XpRegionRec>& xpReport() { static std::vector<XpRegionRec> v; return v; }

// ---- the configuration file (msim -xprop=<file>, as VCS's) ----
//     merge = tmerge | xmerge | vmerge;
//     tree     {top.dut ...}  {xpropOn | xpropOff};   an instance and all below it
//     instance {top.dut.u1}   {xpropOn | xpropOff};   that instance only
//     module   {fifo ...}     {xpropOn | xpropOff};   every instance of a module
// Names may use * and ?. An instance rule wins over a module rule, which
// wins over tree rules; among trees the deepest match wins, and among equal
// ones the last. Instances that no rule names are on.
struct XpRule { int kind = 0; std::string pat; bool on = true; size_t order = 0; };   // kind 0 tree, 1 instance, 2 module
static std::vector<XpRule>& xpRules() { static std::vector<XpRule> v; return v; }
static bool xpGlob(const char* p, const char* s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return true;
            for (; *s; s++) if (xpGlob(p + 1, s)) return true;
            return false;
        }
        if (!*s || (*p != '?' && *p != *s)) return false;
    }
    return !*s;
}
// Reads a configuration file into xpRules() and xpropMode(); "" when it is
// fine, else what is wrong with it.
static std::string xpReadConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "cannot read " + path;
    std::string text, line;
    while (std::getline(f, line)) {
        size_t c = line.find("//");
        if (c != std::string::npos) line.resize(c);
        if (size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        text += line + "\n";
    }
    xpRules().clear();
    if (!xpropMode()) xpropMode() = 1;
    size_t at = 0, n = 0;
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    while (at < text.size()) {
        size_t semi = text.find(';', at);
        std::string st = trim(text.substr(at, semi == std::string::npos ? std::string::npos : semi - at));
        at = semi == std::string::npos ? text.size() : semi + 1;
        if (st.empty()) continue;
        n++;
        if (st.rfind("merge", 0) == 0) {
            size_t eq = st.find('=');
            std::string m = eq == std::string::npos ? "" : trim(st.substr(eq + 1));
            if (m == "tmerge") xpropMode() = 1;
            else if (m == "xmerge") xpropMode() = 2;
            else if (m == "vmerge") xpropMode() = 0;
            else return "statement " + std::to_string(n) + ": merge = tmerge, xmerge or vmerge";
            continue;
        }
        int kind = st.rfind("tree", 0) == 0 ? 0 : st.rfind("instance", 0) == 0 ? 1 : st.rfind("module", 0) == 0 ? 2 : -1;
        size_t a1 = st.find('{'), b1 = a1 == std::string::npos ? a1 : st.find('}', a1);
        size_t a2 = b1 == std::string::npos ? b1 : st.find('{', b1), b2 = a2 == std::string::npos ? a2 : st.find('}', a2);
        if (kind < 0 || b2 == std::string::npos)
            return "statement " + std::to_string(n) + ": expected tree, instance or module {names} {xpropOn|xpropOff}";
        std::string sw = trim(st.substr(a2 + 1, b2 - a2 - 1));
        bool on = sw == "xpropOn";
        if (!on && sw != "xpropOff") return "statement " + std::to_string(n) + ": " + sw + " is not xpropOn or xpropOff";
        std::istringstream names(st.substr(a1 + 1, b1 - a1 - 1));
        for (std::string nm; names >> nm;) {
            if (nm.back() == ',') nm.pop_back();
            if (!nm.empty()) xpRules().push_back({kind, nm, on, xpRules().size()});
        }
    }
    return "";
}
// Whether -xprop applies to the code of instance `path` of module `mod`.
static bool xpConfigOn(const std::string& path, std::string_view mod) {
    const XpRule* best = nullptr;
    size_t bestLen = 0;
    auto better = [&](const XpRule& r, size_t len) {
        if (!best || r.kind > best->kind || (r.kind == best->kind && (len > bestLen || (len == bestLen && r.order > best->order)))) {
            best = &r;
            bestLen = len;
        }
    };
    std::string m(mod);
    for (const XpRule& r : xpRules()) {
        if (r.kind == 1 && xpGlob(r.pat.c_str(), path.c_str())) better(r, path.size());
        if (r.kind == 2 && xpGlob(r.pat.c_str(), m.c_str())) better(r, 0);
        if (r.kind == 0)   // the instance itself or any instance above it
            for (size_t e = path.size();;) {
                if (xpGlob(r.pat.c_str(), path.substr(0, e).c_str())) { better(r, e); break; }
                size_t d = path.rfind('.', e - 1);
                if (d == std::string::npos || d == 0) break;
                e = d;
            }
    }
    return !best || best->on;
}
struct Xp {
    bool on = false;
    int xm = -1;   // 1 while an unknown region is running
    int guardDepth = 0;
    std::unordered_map<const slang::ast::Symbol*, int> cls;
    std::unordered_map<int, XpShadow> shadow;   // by target signal
    std::unordered_map<int, int> shadowOf;      // shadow value or mask -> target signal
    std::unordered_set<int> temps;              // this process's temporaries
    std::unordered_set<int> bad;                // targets that cannot be merged (real, string, ...)
    std::unordered_set<size_t> okNba, okArr, oolJmp;
    // The clean copy (see xpProc): `sites` holds, per region in the order
    // the regions are lowered, where the merging copy takes over when
    // condition k is unknown (empty: the region is not merged).
    bool clean = false;
    // Keyed by the statement; a statement lowered more than once has one entry per time.
    std::unordered_map<const void*, std::vector<std::vector<size_t>>> sites;
    std::unordered_map<const void*, size_t> siteUse;
    const void* key = nullptr;                // of the region xpChain is called for
    std::vector<size_t>* lastSite = nullptr;
    // The clean copy's entry for condition k of the region `key`, or Instr::NOX.
    std::vector<size_t> cleanSite(const void* key) {
        size_t n = siteUse[key]++;
        auto it = sites.find(key);
        return it != sites.end() && n < it->second.size() ? it->second[n] : std::vector<size_t>{};
    }
    std::unordered_set<const slang::ast::Symbol*> once;   // nonblocking targets assigned at most once a pass
    std::vector<std::pair<size_t, size_t>> guarded, badRanges;
    std::vector<std::function<void()>> ool;
} xp;

// A temporary of the merge. A name with '$' stays out of waveforms and toggle coverage.
int xpTemp(int w) {
    if (!mkSig) unsupported("temporary");
    int n = tempSeq ? (*tempSeq)++ : 0;
    int s = mkSig("_xp$" + std::to_string(n), w, false);
    xp.temps.insert(s);
    return s;
}
static int xpSigW(int s) { return (size_t)s < sigInfo().size() && sigInfo()[s].second > 0 ? sigInfo()[s].second : 1; }
static E xpX(int w) { return konst(allx(w)); }
static E xpOnes(int w) { return konst(v_not(make(0, 0, w))); }
static E xpZero(int w) { return konst(make(0, 0, w)); }
static E xpIsX(E e) { return bin(Op::CaseEq, std::move(e), konst(allx(1))); }
static E xpIs(E e, int v) { return bin(Op::CaseEq, std::move(e), konst(mk((uint64_t)v, 1))); }
static E xpNot(E e) { return un(Op::LogNot, std::move(e)); }
static E xpUnk1(E e) { return bin(Op::LogAnd, xpNot(xpIs(e, 0)), xpNot(xpIs(e, 1))); }
static E xpRd(const XpUnit& u) { return u.hi < 0 ? ref(u.sig) : slice(ref(u.sig), u.hi, u.lo); }
void xpSet(int sig, E e) { Instr a; a.k = IK::Assign; a.sig = sig; a.e = std::move(e); prog.emit(a); }
void xpWr(const XpUnit& u, E e) {
    Instr a; a.k = IK::Assign; a.sig = u.sig; a.e = std::move(e);
    if (u.hi >= 0) { a.hi = u.hi; a.lo = u.lo; }
    prog.emit(a);
}
size_t xpJmp(size_t target = 0) { Instr j; j.k = IK::Jmp; j.target = target; return prog.emit(j); }

// Statements whose only effect is a message or the end of the run are
// skipped while an unknown region runs: it is not known that they execute.
template <class F> void xpGuard(F&& f) {
    if (!xp.on || xp.guardDepth || xp.clean) { f(); return; }
    size_t jf = emitJmpIfNot(xpNot(xpIs(ref(xp.xm), 1)));
    xp.guardDepth++;
    f();
    xp.guardDepth--;
    xp.guarded.push_back({jf, prog.here()});
    prog.code[jf].target = prog.here();
}
static bool xpMessageTask(std::string_view n) {
    return n.starts_with("$display") || n.starts_with("$write") || n.starts_with("$strobe") ||
           n.starts_with("$monitor") || n.starts_with("$fdisplay") || n.starts_with("$fwrite") ||
           n.starts_with("$fstrobe") || n.starts_with("$fmonitor") || n == "$fatal" || n == "$error" ||
           n == "$warning" || n == "$info" || n == "$finish" || n == "$stop";
}
// Whether a task or function, and every one it calls, only assigns with =
// (no <=, no delay or event control, no fork): then a call of it can run
// speculatively, and its writes are merged with the region's (xpScan).
static bool xpPureSub(const slang::ast::SubroutineSymbol& sub, int depth = 0) {
    using namespace slang::ast;
    struct V : ASTVisitor<V, VisitFlags::Statements | VisitFlags::Expressions> {
        bool ok = true;
        int depth = 0;
        void handle(const AssignmentExpression& a) {
            if (a.isNonBlocking() || a.timingControl) ok = false;
            visitDefault(a);
        }
        void handle(const TimedStatement&) { ok = false; }
        void handle(const WaitStatement&) { ok = false; }
        void handle(const WaitForkStatement&) { ok = false; }
        void handle(const EventTriggerStatement&) { ok = false; }
        void handle(const BlockStatement& b) {
            if (b.blockKind != StatementBlockKind::Sequential) ok = false;
            visitDefault(b);
        }
        void handle(const CallExpression& c) {
            if (const auto* s = std::get_if<const SubroutineSymbol*>(&c.subroutine)) {
                if (depth > 16 || !xpPureSub(**s, depth + 1)) ok = false;
            }
            visitDefault(c);
        }
    } v;
    v.depth = depth;
    sub.getBody().visit(v);
    return v.ok;
}
// An expression statement: a message task is guarded; a call of a user task
// or function that only assigns with = is lowered as it is, and its writes
// are merged (xpScan); any other call makes the regions around it unmergeable.
void xpExprStmt(const slang::ast::Expression& ex) {
    using namespace slang::ast;
    if (xp.on && !xp.clean && ex.kind == ExpressionKind::Call) {
        const auto& call = ex.as<CallExpression>();
        if (call.isSystemCall() && xpMessageTask(call.getSubroutineName())) {
            xpGuard([&] { exprStmt(ex); });
            return;
        }
        if (const auto* s = std::get_if<const SubroutineSymbol*>(&call.subroutine); s && xpPureSub(**s)) {
            exprStmt(ex);
            return;
        }
        size_t a = prog.here();
        exprStmt(ex);
        xp.badRanges.push_back({a, prog.here()});
        return;
    }
    exprStmt(ex);
}

// ---- which nonblocking targets need which treatment ----
// The variable a target expression writes; `simple` is cleared unless the
// target is a whole packed variable or a constant part of one.
bool xpConstInt(const slang::ast::Expression& x, slang::SVInt* out = nullptr) const {
    slang::ConstantValue cv = x.getConstant() ? *x.getConstant() : tryConst(x);
    if (cv.bad() || !cv.isInteger()) return false;
    if (out) *out = cv.integer();
    return true;
}
const slang::ast::Symbol* xpRootOf(const slang::ast::Expression& e0, bool& simple) const {
    using namespace slang::ast;
    const Expression* e = &peel(e0);
    for (;;) {
        switch (e->kind) {
            case ExpressionKind::NamedValue:
                if (!e->type->isIntegral()) simple = false;
                return &e->as<ValueExpressionBase>().symbol;
            case ExpressionKind::HierarchicalValue:
                simple = false;
                return &e->as<ValueExpressionBase>().symbol;
            case ExpressionKind::ElementSelect: {
                const auto& s = e->as<ElementSelectExpression>();
                if (!s.value().type->isIntegral() || !xpConstInt(s.selector())) simple = false;
                e = &peel(s.value());
                break;
            }
            case ExpressionKind::RangeSelect: {
                const auto& s = e->as<RangeSelectExpression>();
                if (!s.value().type->isIntegral() || !xpConstInt(s.left()) || !xpConstInt(s.right())) simple = false;
                e = &peel(s.value());
                break;
            }
            case ExpressionKind::MemberAccess: {
                const auto& s = e->as<MemberAccessExpression>();
                if (!s.value().type->isIntegral()) simple = false;
                e = &peel(s.value());
                break;
            }
            default:
                simple = false;
                return nullptr;
        }
    }
}
struct XpPre {
    std::unordered_set<const slang::ast::Symbol*> direct;
    bool waits = false, unknown = false;
};
using XpCounts = std::unordered_map<const slang::ast::Symbol*, int>;
static void xpAdd(XpCounts& a, const XpCounts& b) { for (auto& [s, n] : b) a[s] = std::min(2, a[s] + n); }
static void xpMax(XpCounts& a, const XpCounts& b) { for (auto& [s, n] : b) a[s] = std::max(a[s], n); }
void xpTargets(const slang::ast::Expression& l0, bool delayed, XpCounts& out, XpPre& pre) {
    using namespace slang::ast;
    const Expression& l = peel(l0);
    if (l.kind == ExpressionKind::Concatenation) {
        for (auto* op : l.as<ConcatenationExpression>().operands()) xpTargets(*op, delayed, out, pre);
        return;
    }
    if (l.kind == ExpressionKind::Streaming) { pre.unknown = true; return; }
    bool simple = true;
    const Symbol* s = xpRootOf(l, simple);
    if (!s) { pre.unknown = true; return; }
    out[s] = std::min(2, out[s] + 1);
    if (!simple || delayed) pre.direct.insert(s);
}
// How many times one pass over s can assign each variable with <=
// (0, 1, or 2 for more).
void xpCount(const slang::ast::Statement& s, XpCounts& out, XpPre& pre) {
    using namespace slang::ast;
    auto loop = [&](const Statement& body) {
        XpCounts c;
        xpCount(body, c, pre);
        for (auto& [sym, n] : c) if (n) out[sym] = 2;
    };
    switch (s.kind) {
        case StatementKind::Empty:
        case StatementKind::VariableDeclaration:
        case StatementKind::Break:
        case StatementKind::Continue:
        case StatementKind::Return:
        case StatementKind::Disable:
            break;
        case StatementKind::List:
            for (auto* st : s.as<StatementList>().list) xpCount(*st, out, pre);
            break;
        case StatementKind::Block: {
            const auto& b = s.as<BlockStatement>();
            if (b.blockKind != StatementBlockKind::Sequential) { pre.unknown = true; break; }
            xpCount(b.body, out, pre);
            break;
        }
        case StatementKind::ExpressionStatement: {
            const Expression& ex = s.as<ExpressionStatement>().expr;
            if (ex.kind == ExpressionKind::Assignment) {
                const auto& a = ex.as<AssignmentExpression>();
                if (a.isNonBlocking()) xpTargets(a.left(), a.timingControl != nullptr, out, pre);
                else if (a.timingControl) pre.waits = true;
            } else if (ex.kind == ExpressionKind::Call && !ex.as<CallExpression>().isSystemCall()) {
                const auto* sub = std::get_if<const SubroutineSymbol*>(&ex.as<CallExpression>().subroutine);
                if (!sub || !xpPureSub(**sub)) pre.unknown = true;   // a task can wait and assign
            }
            break;
        }
        case StatementKind::Conditional: {
            const auto& c = s.as<ConditionalStatement>();
            XpCounts a, b;
            xpCount(c.ifTrue, a, pre);
            if (c.ifFalse) xpCount(*c.ifFalse, b, pre);
            xpMax(a, b);
            xpAdd(out, a);
            break;
        }
        case StatementKind::Case: {
            const auto& c = s.as<CaseStatement>();
            XpCounts a;
            for (auto& item : c.items) { XpCounts b; xpCount(*item.stmt, b, pre); xpMax(a, b); }
            if (c.defaultCase) { XpCounts b; xpCount(*c.defaultCase, b, pre); xpMax(a, b); }
            xpAdd(out, a);
            break;
        }
        case StatementKind::ImmediateAssertion: {
            const auto& c = s.as<ImmediateAssertionStatement>();
            XpCounts a, b;
            if (c.ifTrue) xpCount(*c.ifTrue, a, pre);
            if (c.ifFalse) xpCount(*c.ifFalse, b, pre);
            xpMax(a, b);
            xpAdd(out, a);
            break;
        }
        case StatementKind::ForLoop: loop(s.as<ForLoopStatement>().body); break;
        case StatementKind::RepeatLoop: loop(s.as<RepeatLoopStatement>().body); break;
        case StatementKind::ForeachLoop: loop(s.as<ForeachLoopStatement>().body); break;
        case StatementKind::WhileLoop: loop(s.as<WhileLoopStatement>().body); break;
        case StatementKind::DoWhileLoop: loop(s.as<DoWhileLoopStatement>().body); break;
        case StatementKind::ForeverLoop: loop(s.as<ForeverLoopStatement>().body); break;
        case StatementKind::Timed:
            pre.waits = true;
            xpCount(s.as<TimedStatement>().stmt, out, pre);
            break;
        case StatementKind::Wait:
            pre.waits = true;
            xpCount(s.as<WaitStatement>().stmt, out, pre);
            break;
        default:
            pre.unknown = true;
    }
}

// ---- nonblocking assignments ----
// A nonblocking assignment with no shadow: in an unknown region its target becomes x.
void xpDirect(Instr i) {
    if (!xp.on || xp.clean || i.k != IK::Nba) { prog.emit(i); return; }
    int w = i.bidx && i.pw <= 0 ? 1 : i.pw > 0 ? i.pw : i.hi >= 0 ? i.hi - i.lo + 1
            : i.arr ? i.arr->elemW : (i.sig >= 0 && !isRel(i.sig) ? xpSigW(i.sig) : 0);
    if (w <= 0 || i.dyn >= 0) { prog.emit(i); return; }   // left as it is: the region is refused
    size_t jf = emitJmpIfNot(ref(xp.xm));
    Instr x = i;
    x.e = xpX(w);
    xp.okNba.insert(prog.emit(x));
    size_t js = xpJmp();
    prog.code[jf].target = prog.here();
    xp.okNba.insert(prog.emit(i));
    prog.code[js].target = prog.here();
}
// Emits assignment i, whose target expression is `target`.
void xpEmit(Instr i, const slang::ast::Expression& target) {
    if (xp.on && i.k == IK::Assign && i.sig >= 0 && !i.arr && !target.type->isIntegral()) xp.bad.insert(i.sig);
    if (!xp.on || (i.k != IK::Nba && i.k != IK::Assign)) { prog.emit(i); return; }
    bool simple = true;
    const slang::ast::Symbol* root = xpRootOf(target, simple);
    bool arrOk = i.arr && i.dyn < 0 && !i.arr->dynElem && !i.nbaLater && i.drv < 0 && target.type->isIntegral()
        && (i.k == IK::Assign || (root && xp.once.count(root)));
    // An index with an unknown bit (7.4.6 skips the write). tmerge reads
    // mem[a] = d as a case on a: every element the index may name takes
    // the merge of its present value and d (xmerge: x).
    E idxX;
    if (arrOk && i.arr->dims.size() == 1 && !i.aidx.empty())
        idxX = xpIsX(un(Op::RedXor, i.aidx[0]));
    if (xp.clean) {   // the clean copy: an unknown index goes to the merging copy
        if (idxX) {
            std::vector<size_t> to = xp.cleanSite(&target);
            if (!to.empty()) {
                size_t jx = emitJmpIfNot(un(Op::RedXor, i.aidx[0]));   // 0 or 1: go on
                prog.code[jx].target = prog.here();
                prog.code[jx].xt = to[0];
            }
        }
        prog.emit(i);
        return;
    }
    auto merged = [&](const std::vector<E>& aidx) {
        int w = i.bidx && i.pw <= 0 ? 1 : i.pw > 0 ? i.pw : i.hi >= 0 ? i.hi - i.lo + 1 : i.arr->elemW;
        Instr m = i;
        m.aidx = aidx;
        if (xpropMode() == 2 || i.bidx || i.pw > 0) {
            m.e = xpX(w);
        } else {
            auto rd = node(Op::ArrRead);
            rd->arr = i.arr;
            rd->kids = aidx;
            E oldv = i.hi >= 0 ? slice(rd, i.hi, i.lo) : E(rd);
            m.e = node(Op::Mux, {konst(allx(1)), oldv, i.e});
        }
        return m;
    };
    size_t jidx = (size_t)-1;
    if (idxX) {
        size_t chk = prog.here();
        xp.sites[&target].push_back({chk});
        jidx = emitJmpIfNot(idxX);
        const ArrShape& a = *i.arr;
        auto [l, r] = a.dims[0];
        int k = xpTemp(32);
        auto s32 = [](int64_t v) { return konst(make((uint64_t)v, 0, 32, 0, true)); };
        xpSet(k, s32(0));
        size_t top = prog.here();
        size_t jend = emitJmpIfNot(bin(Op::Lt, ref(k, true), s32(a.size(0))));
        E kidx = l <= r ? bin(Op::Add, s32(l), ref(k, true)) : bin(Op::Sub, s32(l), ref(k, true));
        // May the index name element k? Not when a bit known in both differs.
        E same = un(Op::RedAnd, un(Op::Not, bin(Op::Xor, resize(i.aidx[0], 64, false, false), resize(kidx, 64, true, false))));
        size_t jn = emitJmpIfNot(xpNot(xpIs(same, 0)));
        xp.okArr.insert(prog.emit(merged({kidx})));
        prog.code[jn].target = prog.here();
        xpSet(k, bin(Op::Add, ref(k, true), s32(1)));
        xpJmp(top);
        prog.code[jend].target = prog.here();
        xpSet(k, s32(0));
        size_t jdone = xpJmp();
        prog.code[jidx].target = prog.here();
        jidx = jdone;
    }
    // An element chosen at run time (mem[a] = d, mem[a] <= d). In an unknown
    // region the write still happens, but it stores the merge of the element's
    // present value and the new one: right for one write, pessimistic when
    // several branches write the same element. A nonblocking write merges with
    // the present value only when no other one to that array can be pending.
    if (arrOk) {
        size_t jf = emitJmpIfNot(ref(xp.xm));
        xp.okArr.insert(prog.emit(merged(i.aidx)));
        size_t js = xpJmp();
        prog.code[jf].target = prog.here();
        xp.okArr.insert(prog.emit(i));
        prog.code[js].target = prog.here();
        if (jidx != (size_t)-1) prog.code[jidx].target = prog.here();
        return;
    }
    if (i.k != IK::Nba) { prog.emit(i); return; }
    int cls = XC_DIRECT;
    if (root && simple)
        if (auto it = xp.cls.find(root); it != xp.cls.end()) cls = it->second;
    if (i.nbaLater || i.arr || i.dyn >= 0 || i.bidx || i.pw > 0 || i.sig < 0 || isRel(i.sig) || i.drv >= 0) cls = XC_DIRECT;
    if (cls == XC_DIRECT) { xpDirect(i); return; }
    auto [it, isNew] = xp.shadow.try_emplace(i.sig);
    XpShadow& sh = it->second;
    if (isNew) {
        sh.w = xpSigW(i.sig);
        sh.n = xpTemp(sh.w);
        sh.m = xpTemp(sh.w);
        sh.cls = cls;
        xp.shadowOf[sh.n] = i.sig;
        xp.shadowOf[sh.m] = i.sig;
    }
    Instr n = i, m = i;
    n.k = m.k = IK::Assign;
    n.sig = sh.n;
    m.sig = sh.m;
    m.e = xpOnes(i.hi >= 0 ? i.hi - i.lo + 1 : sh.w);
    if (sh.cls == XC_EAGER) { prog.emit(n); prog.emit(m); return; }
    size_t jf = emitJmpIfNot(ref(xp.xm));
    prog.emit(n);
    prog.emit(m);
    size_t js = xpJmp();
    prog.code[jf].target = prog.here();
    xp.okNba.insert(prog.emit(i));
    prog.code[js].target = prog.here();
}

// ---- break and continue ----
// In the merging copy, a for or foreach loop whose body has a break or a
// continue of its own runs it with flags: break sets brk and continue sets
// cnt instead of jumping, the rest of every statement list after a
// statement that may have set one runs under if (!(brk | cnt)), the body
// under if (!brk), and the loop ends once brk is 1. An unknown condition
// around a break then makes brk x, and the rest of the loop merges with
// not running, as tmerge asks. Both flags are 0 outside the loop.
static bool xpEscapes(const slang::ast::Statement& st) {
    using namespace slang::ast;
    struct V : ASTVisitor<V, VisitFlags::Statements> {
        bool any = false;
        void handle(const BreakStatement&) { any = true; }
        void handle(const ContinueStatement&) { any = true; }
        void handle(const ForLoopStatement&) {}
        void handle(const ForeachLoopStatement&) {}
        void handle(const WhileLoopStatement&) {}
        void handle(const DoWhileLoopStatement&) {}
        void handle(const ForeverLoopStatement&) {}
        void handle(const RepeatLoopStatement&) {}
    } v;
    st.visit(v);
    return v.any;
}
bool xpLoopOk(const slang::ast::Statement& body) { return xp.on && !xp.clean && xpEscapes(body); }
// Lowers the body; returns the jump that leaves the loop when brk is 1 (to be patched to the loop's end).
size_t xpLoopBody(const slang::ast::Statement& body) {
    LoopJumps& lj = loops.back();
    lj.xpBrk = xpTemp(1);
    lj.xpCnt = xpTemp(1);
    size_t jb = emitJmpIfNot(xpNot(xpIs(ref(lj.xpBrk), 1)));
    xpSet(lj.xpCnt, konst(mk(0, 1)));
    int brk = lj.xpBrk;
    xp.key = &body;
    xpChain(1, [&](size_t) { return std::pair<E, E>{nullptr, xpNot(ref(brk))}; },
            [&](size_t k) { if (k == 0) stmt(body); }, "loop body");
    return jb;
}
void xpLoopEnd() {
    xpSet(loops.back().xpBrk, konst(mk(0, 1)));
    xpSet(loops.back().xpCnt, konst(mk(0, 1)));
}
void xpList(std::span<const slang::ast::Statement* const> list, size_t k0) {
    for (size_t k = k0; k < list.size(); k++) {
        stmt(*list[k]);
        if (k + 1 < list.size() && xpEscapes(*list[k])) {
            int brk = loops.back().xpBrk, cnt = loops.back().xpCnt;
            xp.key = list[k];
            xpChain(1, [&](size_t) { return std::pair<E, E>{nullptr, xpNot(bin(Op::Or, ref(brk), ref(cnt)))}; },
                    [&](size_t j) { if (j == 0) xpList(list, k + 1); }, "rest after break / continue");
            return;
        }
    }
}

// ---- a region ----
// What instructions [a, b) assign, or why they cannot run speculatively.
struct XpBits { bool all = false; std::vector<std::pair<int, int>> r; };
// The same for the body of a called function or task: its static
// variables become units of the region; its automatic ones live in its own
// frame and need nothing.
bool xpCallee(const Prog* p, std::map<int, XpBits>& tg, std::string& why, int depth) {
    if (!p) return true;
    if (depth > 16) { why = "a recursive call"; return false; }
    for (const Instr& i : p->code)
        switch (i.k) {
            case IK::Assign:
                if (i.sig >= 0 && isRel(i.sig) && !i.arr && i.dyn < 0) break;   // an automatic variable
                if (i.dyn >= 0 || i.arr || i.drv >= 0 || i.sig < 0 || xp.bad.count(i.sig) || !xpSigW(i.sig)) {
                    why = "a called function or task that writes an array element, a container or a net"; return false;
                }
                if (i.hi < 0 || i.bidx || i.pw > 0) tg[i.sig].all = true;
                else tg[i.sig].r.push_back({i.lo, i.hi});
                break;
            case IK::Jmp: case IK::JmpIfNot: case IK::Ret:
                break;
            case IK::Call:
                if (i.callee == p) { why = "a recursive call"; return false; }
                if (!xpCallee(i.callee, tg, why, depth + 1)) return false;
                break;
            default:
                why = "a called function or task with a statement that cannot run speculatively"; return false;
        }
    return true;
}
bool xpScan(size_t a, size_t b, std::map<int, XpBits>& tg, std::string& why) {
    for (auto& [s, e] : xp.badRanges)
        if (s < b && e > a && s < e) { why = "a task or function call statement"; return false; }
    size_t g = 0;
    for (size_t pc = a; pc < b; pc++) {
        while (g < xp.guarded.size() && xp.guarded[g].second <= pc) g++;
        if (g < xp.guarded.size() && xp.guarded[g].first <= pc) { pc = xp.guarded[g].second - 1; continue; }
        const Instr& i = prog.code[pc];
        switch (i.k) {
            case IK::Assign: {
                if (i.dyn >= 0) { why = "an assignment to a queue, dynamic or associative array element"; return false; }
                if (i.arr) {
                    if (xp.okArr.count(pc)) break;
                    why = "an assignment to an array element chosen at run time"; return false;
                }
                if (i.sig < 0 || isRel(i.sig)) { why = "an assignment to an automatic variable or class property"; return false; }
                if (i.drv >= 0) { why = "a net driver"; return false; }
                if (xp.bad.count(i.sig)) { why = "an assignment to a real, string, handle or container variable"; return false; }
                XpBits& t = tg[i.sig];
                if (i.hi < 0 || i.bidx || i.pw > 0) t.all = true;
                else t.r.push_back({i.lo, i.hi});
                break;
            }
            case IK::Nba:
                if (!xp.okNba.count(pc) && !xp.okArr.count(pc)) { why = "a nonblocking assignment in a form that cannot be merged"; return false; }
                break;
            case IK::Jmp:
            case IK::JmpIfNot:
                if (xp.oolJmp.count(pc)) break;
                if (i.target < a || i.target > b) { why = "a jump out of the branch (break, continue, return or disable)"; return false; }
                break;
            case IK::Call:   // a function or task: what it writes is merged too
                if (!xpCallee(i.callee, tg, why, 0)) return false;
                break;
            case IK::WaitEdge: case IK::WaitDelay: case IK::CycleWait: case IK::WaitFork:
                why = "a delay or event control"; return false;
            case IK::Display: case IK::Strobe: case IK::Monitor: case IK::Finish: case IK::Assert:
                why = "a system task with a side effect"; return false;
            default:
                why = "a statement that cannot run speculatively"; return false;
        }
    }
    return true;
}
static std::vector<XpUnit> xpUnits(const std::map<int, XpBits>& tg) {
    std::vector<XpUnit> out;
    for (auto& [sig, b] : tg) {
        int w = xpSigW(sig);
        bool all = b.all;
        std::vector<std::pair<int, int>> r = b.r;
        if (!all) {
            std::sort(r.begin(), r.end());
            std::vector<std::pair<int, int>> m;
            for (auto& x : r) {
                if (!m.empty() && x.first <= m.back().second + 1) m.back().second = std::max(m.back().second, x.second);
                else m.push_back(x);
            }
            r = std::move(m);
            if (r.size() > 4 || (r.size() == 1 && r[0].first <= 0 && r[0].second >= w - 1)) all = true;
        }
        if (all) out.push_back({sig, -1, 0, w});
        else for (auto& [lo, hi] : r) out.push_back({sig, hi, lo, hi - lo + 1});
    }
    return out;
}
// Schedules the shadow of target q: the parts in `units` that were assigned.
void xpFlush(int q, const XpShadow& sh, const std::vector<XpUnit>& units) {
    for (const XpUnit& u : units) {
        if (u.sig != sh.n) continue;
        XpUnit mu = u;
        mu.sig = sh.m;
        size_t jf = emitJmpIfNot(xpNot(xpIs(un(Op::RedOr, xpRd(mu)), 0)));
        Instr a; a.k = IK::Nba; a.sig = q; a.e = xpRd(u);
        if (u.hi >= 0) { a.hi = u.hi; a.lo = u.lo; }
        prog.emit(a);
        prog.code[jf].target = prog.here();
    }
}

// One region: nCond branches tried in order, then the default branch
// (k == nCond). cond(k) returns {gx, g}: gx is true when it is unknown
// whether branch k is taken, g when it is taken (null: taken whenever gx
// is false). body(k) lowers the branch. T, when given, gets the first
// instruction of each branch. With `total`, weight(k) is how many of the
// `total` possible control values branch k stands for when it is unknown;
// once the unknown branches that ran add up to total, the default branch
// cannot be reached and does not run. False when the branches cannot be
// merged: then an unknown condition counts as false, as without -xprop.
bool xpChain(size_t nCond, const std::function<std::pair<E, E>(size_t)>& cond,
             const std::function<void(size_t)>& body, const char* what, std::vector<size_t>* T = nullptr,
             const std::function<E(size_t)>& weight = nullptr, E total = nullptr) {
    if (xp.clean) {   // the clean copy of an if: an unknown condition goes to the merging copy
        std::vector<size_t> to = xp.cleanSite(xp.key);
        std::vector<size_t> toEnd;
        for (size_t k = 0; k <= nCond; k++) {
            size_t jn = (size_t)-1;
            if (k < nCond) {
                jn = emitJmpIfNot(cond(k).second);
                if (k < to.size()) prog.code[jn].xt = to[k];
            }
            body(k);
            if (k < nCond) {
                toEnd.push_back(xpJmp());
                prog.code[jn].target = prog.here();
            }
        }
        for (size_t j : toEnd) prog.code[j].target = prog.here();
        return true;
    }
    const void* siteKey = xp.key;
    size_t site = xp.sites[siteKey].size();
    xp.sites[siteKey].emplace_back();
    struct Alt { E gx; bool three = false; size_t jpre = 0, N = 0, T = 0, bs = 0, be = 0, jmid = 0, next = 0; };
    std::vector<Alt> R(nCond + 1);
    std::string where = whereStr();
    int t = xpTemp(1);
    std::vector<size_t> toZ;
    size_t jfull = (size_t)-1;
    for (size_t k = 0; k <= nCond; k++) {
        Alt& r = R[k];
        size_t jn = (size_t)-1;
        if (k < nCond) {
            auto [gx, g] = cond(k);
            if (!gx) {   // unknown exactly when g is neither true nor false: one three-way jump
                r.gx = xpIsX(un(Op::RedOr, g));
                r.three = true;
                r.N = prog.here();
                r.jpre = jn = emitJmpIfNot(g);
            } else {
                r.gx = gx;
                size_t jx = emitJmpIfNot(gx);
                r.jpre = xpJmp();
                xp.oolJmp.insert(r.jpre);
                prog.code[jx].target = prog.here();
                r.N = prog.here();
                if (g) jn = emitJmpIfNot(g);
            }
        }
        if (k == nCond && total) {   // in an unknown region: is the default still reachable?
            size_t jf = emitJmpIfNot(ref(t));
            jfull = xpJmp();
            xp.oolJmp.insert(jfull);
            prog.code[jf].target = prog.here();
        }
        r.T = r.bs = prog.here();
        body(k);
        r.be = prog.here();
        toZ.push_back(emitJmpIfNot(ref(t)));
        r.jmid = xpJmp();
        xp.oolJmp.insert(r.jmid);
        if (jn != (size_t)-1) prog.code[jn].target = prog.here();
        r.next = prog.here();
    }
    size_t Z = prog.here();
    for (size_t j : toZ) prog.code[j].target = Z;
    if (T) for (auto& r : R) T->push_back(r.T);

    std::map<int, XpBits> tg;
    std::string why;
    bool ok = true;
    for (auto& r : R)
        if (!xpScan(r.bs, r.be, tg, why)) { ok = false; break; }
    xpReport().push_back({where, what, why, ok});
    if (!ok) {
        for (size_t k = 0; k <= nCond; k++) {
            if (k < nCond && !R[k].three) prog.code[R[k].jpre].target = R[k].N;
            prog.code[R[k].jmid].target = Z;
        }
        if (jfull != (size_t)-1) prog.code[jfull].target = R[nCond].T;
        xp.lastSite = &xp.sites[siteKey][site];
        return false;
    }
    std::vector<XpUnit> units = xpUnits(tg);
    std::vector<int> lazy;   // targets whose shadows this region starts and schedules when it is outermost
    for (auto& [sig, b] : tg)
        if (auto it = xp.shadowOf.find(sig); it != xp.shadowOf.end()) {
            const XpShadow& sh = xp.shadow.at(it->second);
            if (sh.cls == XC_LAZY && sig == sh.n) lazy.push_back(it->second);
        }
    bool tm = xpropMode() == 1;
    int root = xpTemp(1), first = xpTemp(1), which = xpTemp(32);
    int cnt = total ? xpTemp(32) : -1;
    std::vector<E> wt;
    if (total) for (size_t k = 0; k < nCond; k++) wt.push_back(weight(k));
    std::vector<int> snap, acc;
    for (const XpUnit& u : units) {
        snap.push_back(xpTemp(u.w));
        acc.push_back(tm ? xpTemp(u.w) : -1);
    }
    xp.ool.push_back([=, this] {
        auto num = [](size_t k) { return konst(make(k, 0, 32)); };
        std::vector<size_t> toSetup, toMid, toPost;
        for (size_t k = 0; k < nCond; k++) {   // PRE k
            (R[k].three ? prog.code[R[k].jpre].xt : prog.code[R[k].jpre].target) = prog.here();
            size_t jf = emitJmpIfNot(ref(t));
            xpJmp(R[k].T);
            prog.code[jf].target = prog.here();
            xpSet(which, num(k));
            toSetup.push_back(xpJmp());
        }
        for (size_t k = 0; k <= nCond; k++) {   // MID k
            prog.code[R[k].jmid].target = prog.here();
            xpSet(which, num(k));
            toMid.push_back(xpJmp());
        }
        // Entering the region: save what the branches assign.
        for (size_t j : toSetup) prog.code[j].target = prog.here();
        xpSet(root, xpNot(xpIs(ref(xp.xm), 1)));
        {
            size_t jr = emitJmpIfNot(ref(root));
            xpSet(xp.xm, konst(mk(1, 1)));
            for (int q : lazy) {
                const XpShadow& sh = xp.shadow.at(q);
                xpSet(sh.n, ref(q));
                xpSet(sh.m, xpZero(sh.w));
            }
            prog.code[jr].target = prog.here();
        }
        for (size_t u = 0; u < units.size(); u++) xpSet(snap[u], xpRd(units[u]));
        xpSet(first, konst(mk(1, 1)));
        xpSet(t, konst(mk(1, 1)));
        if (total) xpSet(cnt, konst(mk(0, 32)));
        for (size_t k = 0; k < nCond; k++) {
            size_t jf = emitJmpIfNot(bin(Op::CaseEq, ref(which), num(k)));
            xpJmp(R[k].T);
            prog.code[jf].target = prog.here();
        }
        xpJmp(Z);   // not reached
        // A branch has run: fold its results into the merge and undo them.
        for (size_t j : toMid) prog.code[j].target = prog.here();
        if (tm) {
            size_t jf = emitJmpIfNot(ref(first));
            for (size_t u = 0; u < units.size(); u++) xpSet(acc[u], xpRd(units[u]));
            xpSet(first, konst(mk(0, 1)));
            size_t js = xpJmp();
            prog.code[jf].target = prog.here();
            for (size_t u = 0; u < units.size(); u++)
                xpSet(acc[u], mux(konst(allx(1)), ref(acc[u]), xpRd(units[u])));
            prog.code[js].target = prog.here();
        }
        for (size_t u = 0; u < units.size(); u++) xpWr(units[u], ref(snap[u]));
        for (size_t k = 0; k < nCond; k++) {
            size_t jf = emitJmpIfNot(bin(Op::CaseEq, ref(which), num(k)));
            size_t jd = emitJmpIfNot(R[k].gx);   // taken for sure: no later branch runs
            if (total) {   // every possible value is covered: the default cannot be reached
                xpSet(cnt, bin(Op::Add, ref(cnt), wt[k]));
            }
            xpJmp(R[k].next);
            prog.code[jd].target = prog.here();
            toPost.push_back(xpJmp());
            prog.code[jf].target = prog.here();
        }
        if (jfull != (size_t)-1) {
            toPost.push_back(xpJmp());   // the default branch has run
            prog.code[jfull].target = prog.here();
            size_t jf = emitJmpIfNot(bin(Op::CaseEq, ref(cnt), total));
            toPost.push_back(xpJmp());
            prog.code[jf].target = prog.here();
            xpJmp(R[nCond].T);
        }
        // Leaving the region.
        for (size_t j : toPost) prog.code[j].target = prog.here();
        for (size_t u = 0; u < units.size(); u++) xpWr(units[u], tm ? ref(acc[u]) : xpX(units[u].w));
        xpSet(t, konst(mk(0, 1)));
        size_t jr = emitJmpIfNot(ref(root));
        for (int q : lazy) xpFlush(q, xp.shadow.at(q), units);
        xpSet(xp.xm, konst(mk(0, 1)));
        prog.code[jr].target = prog.here();
        xpJmp(Z);
    });
    for (size_t k = 0; k < nCond; k++)
        if (R[k].three) xp.sites[siteKey][site].push_back(R[k].jpre);
    xp.lastSite = &xp.sites[siteKey][site];
    return true;
}

// if / else if / else (12.4), with or without unique / priority.
bool xpIf(const slang::ast::ConditionalStatement& top) {
    using namespace slang::ast;
    if (!xp.on) return false;
    std::vector<const ConditionalStatement*> chain{&top};
    while (chain.back()->ifFalse && chain.back()->ifFalse->kind == StatementKind::Conditional &&
           chain.back()->ifFalse->as<ConditionalStatement>().check == UniquePriorityCheck::None)
        chain.push_back(&chain.back()->ifFalse->as<ConditionalStatement>());
    for (auto* c : chain)
        if (c->conditions.size() != 1 || c->conditions[0].pattern || !c->conditions[0].expr->type->isIntegral()) {
            if (c == &top || top.check != UniquePriorityCheck::None) return false;
            chain.resize((size_t)(std::find(chain.begin(), chain.end(), c) - chain.begin()));
            break;
        }
    size_t n = chain.size();
    bool uniq = top.check == UniquePriorityCheck::Unique || top.check == UniquePriorityCheck::Unique0;
    bool needAll = top.check == UniquePriorityCheck::Unique || top.check == UniquePriorityCheck::Priority;
    const Statement* last = chain.back()->ifFalse;
    if (uniq)   // more than one condition true (12.4.2): checked before any branch runs
        xpGuard([&] {
            std::vector<size_t> toEnd;
            for (size_t k = 0; k + 1 < n; k++) {
                size_t jf = emitJmpIfNot(expr(*chain[k]->conditions[0].expr));
                E more;
                for (size_t m = k + 1; m < n; m++) {
                    E x = expr(*chain[m]->conditions[0].expr);
                    more = more ? bin(Op::LogOr, more, x) : x;
                }
                size_t jo = emitJmpIfNot(more);
                violation(top, checkName(top.check) + " if: more than one condition is true", top.check);
                prog.code[jo].target = prog.here();
                toEnd.push_back(xpJmp());
                prog.code[jf].target = prog.here();
            }
            for (size_t j : toEnd) prog.code[j].target = prog.here();
        });
    xp.key = &top;
    xpChain(n,
        [&](size_t k) {
            E c = expr(*chain[k]->conditions[0].expr);
            return std::pair<E, E>{nullptr, c};
        },
        [&](size_t k) {
            if (k < n) stmt(chain[k]->ifTrue);
            else if (last) stmt(*last);
            else if (needAll)
                xpGuard([&] { violation(top, checkName(top.check) + " if: no condition is true", top.check); });
        }, "if");
    return true;
}

// case / casez / casex (12.5). A selector with no x or z bit is matched
// as always. Otherwise an item may match unless a bit known in both
// differs, and every item that may match before the first that does runs.
bool xpCase(const slang::ast::Statement& s, const slang::ast::CaseStatement& c) {
    using namespace slang::ast;
    if (!xp.on || c.condition == CaseStatementCondition::Inside) return false;
    if (!c.expr.type->isIntegral()) return false;
    bool wz = c.condition == CaseStatementCondition::WildcardJustZ;
    bool wx = c.condition == CaseStatementCondition::WildcardXOrZ;
    for (const auto& item : c.items)
        for (const Expression* x : item.expressions) {
            if (!x->type->isIntegral()) return false;
            if ((wz || wx) && !xpConstInt(*x)) return false;
        }
    if (xp.clean) {   // the clean copy: an unknown selector goes to the merging copy, else the standard case
        std::vector<size_t> to = xp.cleanSite(&c);
        if (!to.empty()) {
            size_t jx = emitJmpIfNot(un(Op::RedXor, expr(c.expr)));   // 0 or 1: go on
            prog.code[jx].target = prog.here();
            prog.code[jx].xt = to[0];
        }
        return false;
    }
    Op cmp = wz ? Op::CaseZEq : wx ? Op::CaseXEq : Op::CaseEq;
    size_t n = c.items.size();
    size_t entry = prog.here();
    std::vector<E> conds;   // the standard match of each item
    for (const auto& item : c.items) {
        E cond;
        for (const Expression* x : item.expressions) {
            E m = bin(cmp, expr(c.expr), expr(*x));
            cond = cond ? bin(Op::LogOr, cond, m) : m;
        }
        conds.push_back(cond);
    }
    bool uniq = c.check == UniquePriorityCheck::Unique || c.check == UniquePriorityCheck::Unique0;
    bool needAll = c.check == UniquePriorityCheck::Unique || c.check == UniquePriorityCheck::Priority;
    int ov = uniq ? caseMayOverlap(c) : 0;
    if (ov > 0 && n > 1)   // more than one item matches (12.5.3): checked before any item runs
        xpGuard([&] {
            E selUnknown;
            if (ov == 1) selUnknown = bin(Op::Ne, sysFnNode(SF_COUNTBITS, {expr(c.expr)}, 12), konst(make(0, 0, 32, 0, true)));
            int hit = xpTemp(32);
            auto num = [](size_t k) { return konst(make(k, 0, 32)); };
            std::vector<size_t> toCheck, toEnd;
            for (size_t k = 0; k + 1 < n; k++) {
                size_t jf = emitJmpIfNot(conds[k]);
                xpSet(hit, num(k));
                toCheck.push_back(xpJmp());
                prog.code[jf].target = prog.here();
            }
            toEnd.push_back(xpJmp());
            for (size_t j : toCheck) prog.code[j].target = prog.here();
            for (size_t m = 1; m < n; m++) {
                E later = bin(Op::LogAnd, bin(Op::Lt, ref(hit), num(m)), conds[m]);
                if (selUnknown) later = bin(Op::LogAnd, selUnknown, later);
                size_t jf = emitJmpIfNot(later);
                violation(s, checkName(c.check) + " case: more than one case item matches", c.check);
                toEnd.push_back(xpJmp());
                prog.code[jf].target = prog.here();
            }
            for (size_t j : toEnd) prog.code[j].target = prog.here();
        });
    // Selector fully known: the standard matches pick the item (out of line).
    size_t jfast = emitJmpIfNot(xpIsX(un(Op::RedXor, expr(c.expr))));
    xp.oolJmp.insert(jfast);
    auto T = std::make_shared<std::vector<size_t>>();
    // A plain case whose labels are distinct known constants: an unknown
    // selector with k unknown bits stands for 2^k values and each label
    // that may match is one of them.
    bool countable = !wz && !wx;
    {
        std::vector<slang::SVInt> seen;
        for (const auto& item : c.items)
            for (const Expression* x : item.expressions) {
                slang::SVInt v;
                if (!xpConstInt(*x, &v) || v.hasUnknown()) { countable = false; continue; }
                for (auto& o : seen)
                    if (o.getBitWidth() != v.getBitWidth() || exactlyEqual(o, v)) countable = false;
                seen.push_back(v);
            }
    }
    auto label = [&](const Expression* x) { return un(Op::RedAnd, un(Op::Not, bin(Op::Xor, expr(c.expr), expr(*x)))); };
    E total;
    if (countable)   // 2^k, or 0 (never reached) when k is 32 or more
        total = bin(Op::Shl, konst(mk(1, 32)), resize(sysFnNode(SF_COUNTBITS, {expr(c.expr)}, 12), 32, false, false));
    xp.key = &c;
    bool ok = xpChain(n,
        [&](size_t k) {
            E g;
            for (const Expression* x : c.items[k].expressions) {
                E m = un(Op::Not, bin(Op::Xor, expr(c.expr), expr(*x)));
                if (wz || wx) {   // wildcard bits of the item match anything
                    slang::SVInt v;
                    xpConstInt(*x, &v);
                    int w = (int)v.getBitWidth();
                    Val mask = make(0, 0, w);
                    bool any = false;
                    for (int b = 0; b < w; b++) {
                        slang::logic_t bit = v[(int32_t)b];
                        if (wx ? bit.isUnknown() : bit.value == slang::logic_t::Z_VALUE) {
                            mask = v_insert(mask, mk(1, 1), b, b);
                            any = true;
                        }
                    }
                    if (any) m = bin(Op::Or, m, konst(mask));
                }
                m = un(Op::RedAnd, m);
                g = g ? bin(Op::LogOr, g, m) : m;
            }
            return std::pair<E, E>{xpIsX(g), g};
        },
        [&](size_t k) {
            if (k < n) stmt(*c.items[k].stmt);
            else if (c.defaultCase) stmt(*c.defaultCase);
            else if (needAll)
                xpGuard([&] { violation(s, checkName(c.check) + " case: no case item matches", c.check); });
        }, "case", T.get(),
        [&](size_t k) {
            E sum;
            for (const Expression* x : c.items[k].expressions) {
                E one = resize(xpIsX(label(x)), 32, false, false);
                sum = sum ? bin(Op::Add, sum, one) : one;
            }
            return sum;
        }, total);
    if (ok) *xp.lastSite = {entry};
    xp.ool.push_back([=, this] {
        if (!ok) { Instr j; j.k = IK::Jmp; j.line = prog.code[jfast].line; j.file = prog.code[jfast].file; prog.code[jfast] = j; }
        prog.code[jfast].target = prog.here();
        for (size_t k = 0; k < n; k++) {
            Instr j; j.k = IK::JmpIfNot; j.e = xpNot(conds[k]); j.target = (*T)[k];
            prog.emit(j);
        }
        xpJmp((*T)[n]);
    });
    return true;
}

// ---- an always procedure ----
// After @(posedge clk or negedge rst_n ...): true when the edge that woke
// the process went from or to x or z and no listed edge certainly happened
// (expr.hpp: SF_XPROP_EDGE). `level` holds the event signals the body reads.
// The kernel keeps each signal's value before its last change and when it
// changed. (Two listed signals changing in one time step but in different
// delta cycles, the first cleanly and the second from or to x, is taken as
// a clean edge.)
E xpAmbiguousEdge(const std::vector<std::pair<int, Edge>>& ev, const std::unordered_set<int>& level) {
    std::vector<E> kids;
    for (auto& [sig, edge] : ev)
        kids.push_back(konst(mk(((uint64_t)sig << 2) | (edge == NEG ? 2 : 0) | (level.count(sig) ? 1 : 0), 64)));
    return sysFnNode(SF_XPROP_EDGE, std::move(kids));
}
// The body of an always, always_comb, always_ff or always_latch.
void xpProc(const slang::ast::Statement& body) {
    using namespace slang::ast;
    const Statement* b = &body;
    std::vector<std::pair<int, Edge>> ev;
    if (b->kind == StatementKind::Timed) {
        const auto& t = b->as<TimedStatement>();
        covLoc(b->sourceRange.start());
        lastLoc() = b->sourceRange.start();
        timing(t.timing);
        if ((t.timing.kind == TimingControlKind::SignalEvent || t.timing.kind == TimingControlKind::EventList) &&
            prog.code.back().k == IK::WaitEdge && !prog.code.back().dynWait) {
            ev = prog.code.back().events;
            for (auto& [sig, edge] : ev)
                if (edge == ANY || sig < 0 || isRel(sig)) { ev.clear(); break; }
        }
        b = &t.stmt;
    }
    XpCounts counts;
    XpPre pre;
    xpCount(*b, counts, pre);
    bool anyEager = false;
    for (auto& [sym, n] : counts) {
        int cls = pre.unknown || pre.direct.count(sym) ? XC_DIRECT
                  : n <= 1 ? XC_LAZY : (pre.waits ? XC_DIRECT : XC_EAGER);
        xp.cls[sym] = cls;
        if (n <= 1 && !pre.unknown && !pre.waits) xp.once.insert(sym);
        anyEager |= cls == XC_EAGER;
    }
    xp.on = true;
    xp.xm = xpTemp(1);
    size_t start = prog.here();
    size_t jinit = anyEager ? xpJmp() : (size_t)-1;
    // The way to the clean copy (below), when there is one.
    size_t lead = anyEager || g_cov ? (size_t)-1 : xpJmp(prog.here() + 1);
    E ambLead;
    if (!ev.empty()) {
        std::unordered_set<int> level;   // event signals the body reads
        if (ev.size() > 1) {
            struct Reads : ASTVisitor<Reads, VisitFlags::Statements | VisitFlags::Expressions> {
                std::unordered_set<const Symbol*> syms;
                void handle(const NamedValueExpression& e) { syms.insert(&e.symbol); }
            } rd;
            b->visit(rd);
            for (const Symbol* sym : rd.syms)
                if (int sg = sigFind(sym); sg >= 0)
                    for (auto& e : ev) if (e.first == sg) level.insert(sg);
        }
        E amb = xpAmbiguousEdge(ev, level);
        ambLead = amb;
        xp.key = &xp;
        xpChain(1, [&](size_t) { return std::pair<E, E>{amb, nullptr}; },
                [&](size_t k) { if (k == 0) stmt(*b); }, "edge");
    } else {
        stmt(*b);
    }
    // Shadows kept for the whole pass are scheduled at its end.
    std::vector<int> eager;
    for (auto& [q, sh] : xp.shadow) if (sh.cls == XC_EAGER) eager.push_back(q);
    std::sort(eager.begin(), eager.end());
    if (!eager.empty()) {
        std::map<int, XpBits> tg;
        for (size_t pc = start; pc < prog.here(); pc++) {
            const Instr& i = prog.code[pc];
            if (i.k != IK::Assign || !xp.shadowOf.count(i.sig)) continue;
            XpBits& t = tg[i.sig];
            if (i.hi < 0) t.all = true; else t.r.push_back({i.lo, i.hi});
        }
        std::vector<XpUnit> units = xpUnits(tg);
        for (int q : eager) xpFlush(q, xp.shadow.at(q), units);
    }
    if (!xp.ool.empty() || jinit != (size_t)-1) {
        size_t over = xpJmp();
        // A region made while another's out-of-line code is emitted cannot happen: all are made by now.
        for (size_t k = 0; k < xp.ool.size(); k++) xp.ool[k]();
        if (jinit != (size_t)-1) {
            prog.code[jinit].target = prog.here();
            for (int q : eager) {
                const XpShadow& sh = xp.shadow.at(q);
                xpSet(sh.n, ref(q));
                xpSet(sh.m, xpZero(sh.w));
            }
            xpJmp(jinit + 1);
        }
        prog.code[over].target = prog.here();
    }
    // The clean copy. While every condition is known, the checks of the
    // merge only cost time: after each branch, before each nonblocking
    // assignment. So a block of plain assignments and branches is lowered a
    // second time as it is without -xprop, except that an unknown condition
    // jumps (Instr::xt) to the same condition in the merging copy above,
    // which finishes the pass. The clean copy runs first; an ambiguous
    // clock edge starts in the merging copy.
    if (lead != (size_t)-1) {
        bool plain = false;
        for (auto& [key, sts] : xp.sites)
            for (auto& st : sts) plain |= !st.empty();
        for (size_t pc = start; pc < prog.here() && plain; pc++) {
            IK k = prog.code[pc].k;
            plain = k == IK::Assign || k == IK::Nba || k == IK::Jmp || k == IK::JmpIfNot || k == IK::Call;
            if (k == IK::Assert) {   // the report of a unique / priority violation
                int ty = asserts().at(prog.code[pc].sig).type;
                plain = ty == AT_UNIQUE || ty == AT_UNIQUE0 || ty == AT_PRIORITY;
            }
        }
        if (plain) {
            size_t jend = xpJmp();
            if (ambLead) {
                Instr j; j.k = IK::JmpIfNot; j.e = ambLead;
                j.line = prog.code[lead].line; j.file = prog.code[lead].file;
                prog.code[lead] = j;
            }
            prog.code[lead].target = prog.here();
            if (ambLead && lead == 1) prog.xpFast = prog.here();
            xp.clean = true;
            stmt(*b);
            xp.clean = false;
            prog.code[jend].target = prog.here();
        }
    }
    // Only this process uses its temporaries: their writes need no kernel.
    for (size_t pc = start; pc < prog.here(); pc++) {
        Instr& i = prog.code[pc];
        if (i.k == IK::Assign && xp.temps.count(i.sig) && !i.arr && !i.bidx && i.pw <= 0 && i.drv < 0 && i.dyn < 0) i.tmp = true;
    }
    xp.on = false;
}
