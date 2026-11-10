// lower_clocking.inc - clocking blocks (IEEE 1800-2023 clause 14), included
// inside struct Lowerer. Each clocking block becomes one entry of
// clockings() (clocking.hpp), built the first time anything uses it.

static std::unordered_map<const slang::ast::ClockingBlockSymbol*, int>& clockingIdxMap() {
    static std::unordered_map<const slang::ast::ClockingBlockSymbol*, int> m;
    return m;
}
// The global clocking in effect here (14.14): in this instance, else the
// nearest one above it.
const slang::ast::ClockingBlockSymbol* globalClocking() {
    using namespace slang::ast;
    for (const Scope* sc = scope; sc;) {
        for (const auto& m : sc->members())
            if (const auto* cb = m.as_if<ClockingBlockSymbol>(); cb && cb->isGlobal) return cb;
        sc = sc->asSymbol().getHierarchicalParent();
    }
    unsupported("$global_clock with no global clocking (14.14)");
}
// The scope a checker instance's context is inferred from (17.3, 16.14.7):
// for code in a checker body, the scope the outermost checker is instantiated in.
const slang::ast::Scope* contextScope() {
    using namespace slang::ast;
    const Scope* sc = scope;
    while (sc && sc->asSymbol().kind == SymbolKind::CheckerInstanceBody) {
        const auto* inst = sc->asSymbol().as<CheckerInstanceBodySymbol>().parentInstance;
        if (!inst) break;
        sc = inst->getParentScope();
    }
    return sc;
}
// The default clocking in effect here (14.12): this scope's, else the
// checker's instantiation context's.
const slang::ast::ClockingBlockSymbol* defaultClockingBlock() {
    using namespace slang::ast;
    if (!compilation()) return nullptr;
    for (const Scope* sc : {scope, contextScope()})
        if (sc)
            if (const Symbol* dc = compilation()->getDefaultClocking(*sc))
                if (const auto* cb = dc->as_if<ClockingBlockSymbol>()) return cb;
    return nullptr;
}
// The default disable iff in effect here (16.15), as for defaultClocking.
const slang::ast::Expression* defaultDisable() {
    if (!compilation()) return nullptr;
    for (const slang::ast::Scope* sc : {scope, contextScope()})
        if (sc)
            if (const auto* d = compilation()->getDefaultDisable(*sc)) return d;
    return nullptr;
}
// Is this a call of the system function name (no arguments)?
static bool isSysCall(const slang::ast::Expression& e, std::string_view name) {
    using namespace slang::ast;
    return e.kind == ExpressionKind::Call && e.as<CallExpression>().isSystemCall() &&
           e.as<CallExpression>().getSubroutineName() == name;
}
// $inferred_clock (16.14.7): the default clocking's event.
const slang::ast::ClockingBlockSymbol* inferredClock(const slang::ast::Expression& e) {
    if (!isSysCall(e, "$inferred_clock")) return nullptr;
    if (const auto* cb = defaultClockingBlock()) return cb;
    unsupported("$inferred_clock with no default clocking (16.14.7)");
}
// Is this $global_clock (14.14)?
static bool isGlobalClock(const slang::ast::Expression& e) {
    using namespace slang::ast;
    return e.kind == ExpressionKind::Call && e.as<CallExpression>().isSystemCall() &&
           e.as<CallExpression>().getSubroutineName() == "$global_clock";
}
// The clocking block an event expression names, as in @(cb), or null.
static const slang::ast::ClockingBlockSymbol* clockingRef(const slang::ast::Expression& e) {
    using namespace slang::ast;
    const Symbol* sym = nullptr;
    if (e.kind == ExpressionKind::ArbitrarySymbol) sym = e.as<ArbitrarySymbolExpression>().symbol;
    else if (ValueExpressionBase::isKind(e.kind)) sym = &e.as<ValueExpressionBase>().symbol;
    return sym ? sym->as_if<ClockingBlockSymbol>() : nullptr;
}
// A skew (14.4) as (kind, ticks): kind 0 is #1step, 1 is #0, 2 is #N.
std::pair<int, uint64_t> skewOf(const slang::ast::ClockingSkew& sk, int dflt) {
    using namespace slang::ast;
    if (sk.edge != EdgeKind::None) unsupported("a clocking skew with an edge (14.4)");
    if (!sk.delay) return {dflt, 0};
    if (sk.delay->kind == TimingControlKind::OneStepDelay) return {0, 0};
    if (sk.delay->kind != TimingControlKind::Delay) unsupported("this clocking skew");
    Instr d;
    delayInto(sk.delay->as<DelayControl>().expr, d);
    if (d.de) unsupported("a clocking skew that is not a constant");
    return {d.delay ? 2 : 1, d.delay};
}
// The signal bits a clocking output drives, lowest first (14.5).
void outPieces(const slang::ast::Expression& e, std::vector<ClockingRT::Piece>& out) {
    using namespace slang::ast;
    if (e.kind == ExpressionKind::Concatenation) {
        auto ops = e.as<ConcatenationExpression>().operands();
        for (size_t k = ops.size(); k-- > 0;) outPieces(*ops[k], out);
        return;
    }
    Instr i;
    lhs(e, i);
    if (i.arr || i.bidx || i.dyn >= 0) unsupported("a clocking output with a variable index");
    int w = (int)e.type->getBitWidth();
    ClockingRT::Piece p{i.sig, i.hi >= 0 ? i.lo : 0, w};
    const Expression* root = &e;
    for (;;) {
        if (root->kind == ExpressionKind::ElementSelect) root = &root->as<ElementSelectExpression>().value();
        else if (root->kind == ExpressionKind::RangeSelect) root = &root->as<RangeSelectExpression>().value();
        else if (root->kind == ExpressionKind::MemberAccess) root = &root->as<MemberAccessExpression>().value();
        else if (root->kind == ExpressionKind::Conversion) root = &root->as<ConversionExpression>().operand();
        else break;
    }
    bool net = false;
    if (ValueExpressionBase::isKind(root->kind)) {
        const Symbol* sym = &root->as<ValueExpressionBase>().symbol;
        if (const auto* mp = sym->as_if<ModportPortSymbol>(); mp && mp->internalSymbol) sym = mp->internalSymbol;
        net = sym->kind == SymbolKind::Net;
    }
    if (net && mkSig && (size_t)i.sig < sigInfo().size()) {   // driven through a hidden variable
        p.net = i.sig;
        p.netLo = p.lo;
        p.sig = mkSig(sigInfo()[i.sig].first + "$cbdrv" + std::to_string((*tempSeq)++), w, false);
        p.lo = 0;
    }
    out.push_back(p);
}
int clockingIdx(const slang::ast::ClockingBlockSymbol& cb) {
    using namespace slang::ast;
    if (auto it = clockingIdxMap().find(&cb); it != clockingIdxMap().end()) return it->second;
    auto savedScope = scope;
    auto savedUnit = tsUnit, savedPrec = tsPrec;
    setScope(cb.getParentScope());
    ClockingRT r;
    r.name = cb.getHierarchicalPath();
    clockOf(cb.getEvent(), r.ev, r.iff);
    for (auto& [sg, ed] : r.ev) r.evShadow.push_back(shadowOf(sg));
    r.evSig = sigmap.at(&cb);
    auto [dinK, dinT] = skewOf(cb.getDefaultInputSkew(), 0);
    auto [doutK, doutT] = skewOf(cb.getDefaultOutputSkew(), 1);
    for (const auto& m : cb.members()) {
        const auto* cv = m.as_if<ClockVarSymbol>();
        if (!cv) continue;
        const Expression* init = cv->getInitializer();
        if (!init) unsupported("a clockvar with no signal");
        int sig = sigmap.at(cv);
        bool in = cv->direction == ArgumentDirection::In || cv->direction == ArgumentDirection::InOut;
        bool out = cv->direction == ArgumentDirection::Out || cv->direction == ArgumentDirection::InOut;
        if (in) {
            ClockingRT::In x;
            x.cv = sig;
            auto [k, t] = cv->inputSkew.hasValue() ? skewOf(cv->inputSkew, 0) : std::pair{dinK, dinT};
            x.kind = k;
            x.skew = t;
            E src = expr(*init);
            x.src = k == 0 ? sampledE(src) : src;
            if (k == 2) collectRefs(src, x.refs);
            r.ins.push_back(std::move(x));
        }
        if (out) {
            ClockingRT::Out o;
            o.cv = sig;
            o.w = (int)init->type->getBitWidth();
            auto [k, t] = cv->outputSkew.hasValue() ? skewOf(cv->outputSkew, 1) : std::pair{doutK, doutT};
            if (k == 0) unsupported("#1step as an output skew (14.4)");
            o.skew = t;
            outPieces(*init, o.pieces);
            r.outs.push_back(std::move(o));
        }
    }
    clockings().push_back(std::move(r));
    int idx = (int)clockings().size() - 1;
    clockingIdxMap()[&cb] = idx;
    scope = savedScope;
    tsUnit = savedUnit; tsPrec = savedPrec;
    return idx;
}
// The clocking block a ## uses: the default clocking (14.11, 14.12).
int defaultClocking() {
    using namespace slang::ast;
    if (const auto* cb = defaultClockingBlock()) return clockingIdx(*cb);
    unsupported("## with no default clocking (14.11)");
}
// ##n as a statement prefix (14.11).
void cycleDelay(const slang::ast::CycleDelayControl& c) {
    Instr i;
    i.k = IK::CycleWait;
    i.dkey = defaultClocking();
    i.sig = clockings()[i.dkey].evSig;
    i.e = expr(c.expr);
    prog.emit(i);
}
// A synchronous drive, cb.v[hi:lo] <= [##n] e (14.16).
const slang::ast::ClockVarSymbol* clockvarOf(const slang::ast::Expression& e) {
    using namespace slang::ast;
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
            return e.as<ValueExpressionBase>().symbol.as_if<ClockVarSymbol>();
        case ExpressionKind::ElementSelect: return clockvarOf(e.as<ElementSelectExpression>().value());
        case ExpressionKind::RangeSelect: return clockvarOf(e.as<RangeSelectExpression>().value());
        case ExpressionKind::Conversion: return clockvarOf(e.as<ConversionExpression>().operand());
        default: return nullptr;
    }
}
bool syncDrive(const slang::ast::AssignmentExpression& a) {
    using namespace slang::ast;
    const ClockVarSymbol* cv = clockvarOf(a.left());
    if (!cv) return false;
    if (!a.isNonBlocking()) unsupported("a write to a clockvar other than a synchronous drive (14.16)");
    const auto& cb = cv->getParentScope()->asSymbol().as<ClockingBlockSymbol>();
    int idx = clockingIdx(cb);
    Instr i;
    i.k = IK::CbDrive;
    lhs(a.left(), i);
    if (i.arr) unsupported("a synchronous drive to this target");
    i.dkey = idx;
    i.dres = -1;
    auto& outs = clockings()[idx].outs;
    for (size_t k = 0; k < outs.size(); k++)
        if (outs[k].cv == i.sig) i.dres = (int)k;
    if (i.dres < 0) unsupported("a drive of a clocking input (14.16)");
    i.e = expr(a.right());
    if (const TimingControl* tc = a.timingControl) {
        if (tc->kind != TimingControlKind::CycleDelay) unsupported("an intra-assignment delay in a synchronous drive (14.16)");
        i.de = expr(tc->as<CycleDelayControl>().expr);
    }
    prog.emit(i);
    return true;
}
