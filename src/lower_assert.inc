// lower_assert.inc - #include'd inside struct Lowerer (lower.hpp).
// Immediate and deferred assertions (IEEE 1800-2023 16.3, 16.4), assertion
// control tasks (20.11) and sampled value functions (16.5.1, 16.9.3).
// The runtime side is in sva.hpp and exec.inc.

const slang::ast::Scope* scope = nullptr;   // set by setScope
// The clock of sampled value functions called without one (16.9.3): the
// assertion's clock, or the clock inferred from the procedure (16.14.6).
std::vector<std::pair<int, Edge>> sampleEv;
// `label: assert ...` - slang wraps the statement in a named block.
const slang::ast::Symbol* labelSym = nullptr;
const slang::ast::Statement* labelOf = nullptr;
static const slang::ast::Compilation*& compilation() { static const slang::ast::Compilation* c = nullptr; return c; }

// The hierarchical path of a scope: top.u1, top.u1.blk.
static std::string scopePath(const slang::ast::Scope* sc) {
    using namespace slang::ast;
    if (!sc) return "top";
    const Symbol* s = &sc->asSymbol();
    if (s->kind == SymbolKind::InstanceBody)
        if (const auto* inst = s->as<InstanceBodySymbol>().parentInstance) return inst->getHierarchicalPath();
    return s->getHierarchicalPath();
}
static std::pair<std::string, size_t> fileLine(slang::SourceLocation at) {
    std::string file = "?";
    size_t line = 0;
    if (const auto* sm = srcMgr()) {
        auto loc = sm->getFullyOriginalLoc(at);
        file = std::string(sm->getFileName(loc));
        line = sm->getLineNumber(loc);
    }
    return {file, line};
}

// The instance a scope is in: top.u1 for top.u1.blk.
static std::string instPath(const slang::ast::Scope* sc) {
    using namespace slang::ast;
    while (sc) {
        const Symbol& s = sc->asSymbol();
        if (s.kind == SymbolKind::InstanceBody)
            if (const auto* inst = s.as<InstanceBodySymbol>().parentInstance) return inst->getHierarchicalPath();
        sc = s.getParentScope();
    }
    return "top";
}

// A new entry in asserts(). Its name is the label's path, or one made up
// from the scope and the directive (top.assert_3).
int newAssert(int type, int dir, const slang::ast::Statement& st) {
    AssertInfo a;
    a.type = type;
    a.dir = dir;
    a.inst = instPath(scope);
    int id = (int)asserts().size();
    if (labelSym && labelOf == &st) a.name = labelSym->getHierarchicalPath();
    else a.name = scopePath(scope) + "." + (dir == AD_COVER ? "cover_" : dir == AD_ASSUME ? "assume_" : "assert_") + std::to_string(id);
    asserts().push_back(std::move(a));
    return id;
}

// The default fail action of an assert or assume: $error (16.3), printed as
// 20.10 prints it, with the assertion's name for %m.
void emitAssertError(const slang::ast::Statement& st, const std::string& name) {
    auto [file, line] = fileLine(st.sourceRange.start());
    std::string escFile, escName;
    for (char c : file) { escFile += c; if (c == '%') escFile += '%'; }
    for (char c : name) { escName += c; if (c == '%') escName += '%'; }
    Instr i; i.k = IK::Display;
    i.fmt = "Error: \"" + escFile + "\", " + std::to_string(line) + ": " + escName + ": at time %0t";
    E t = node(Op::Time);
    t->k = mk(1, 64);
    i.args.push_back(t);
    prog.emit(i);
}

static int directiveOf(slang::ast::AssertionKind k) {
    using slang::ast::AssertionKind;
    switch (k) {
        case AssertionKind::Assume: return AD_ASSUME;
        case AssertionKind::CoverProperty:
        case AssertionKind::CoverSequence: return AD_COVER;
        default: return AD_ASSERT;
    }
}
static bool emptyStmt(const slang::ast::Statement* s) { return !s || s->kind == slang::ast::StatementKind::Empty; }

// The action of a deferred assertion as its own process (16.4). Its
// argument expressions are evaluated when the assertion is, so each one is
// moved into `caps` and replaced by a constant slot the kernel fills just
// before the action starts. Only the code before the subroutine's Call is
// argument passing; what follows (output write-back) is left alone.
const Prog* actionProg(const slang::ast::Statement* st, const slang::ast::Statement& as, const std::string& name,
                       std::vector<E>& caps, std::vector<E>& slots) {
    if (!store) unsupported("deferred assertion here");
    Lowerer lw(sigmap);
    lw.bind(subs, mkSig, tempSeq, store);
    lw.tsUnit = tsUnit; lw.tsPrec = tsPrec;
    lw.scope = scope;
    lw.current = current;
    lw.retSig = retSig;
    lw.sampleEv = sampleEv;
    if (st) lw.stmt(*st);
    else lw.emitAssertError(as, name);
    auto cap = [&](E& e) {
        if (!e || e->op == Op::Const) return;
        caps.push_back(e);
        E k = konst(Val{});
        slots.push_back(k);
        e = k;
    };
    for (auto& i : lw.prog.code) {
        if (i.k == IK::Call) break;
        if (i.k == IK::Display || i.k == IK::Strobe || i.k == IK::Monitor) for (auto& x : i.args) cap(x);
        else if (i.k == IK::Assign) cap(i.e);
    }
    store->push_back(std::make_unique<Prog>(std::move(lw.prog)));
    return store->back().get();
}

// assert / assume / cover (expr) action_block, simple or deferred (16.3, 16.4).
void immAssert(const slang::ast::ImmediateAssertionStatement& s) {
    int dir = directiveOf(s.assertionKind);
    int type = !s.isDeferred ? AT_IMMEDIATE : s.isFinal ? AT_FINAL : AT_OBSERVED;
    int id = newAssert(type, dir, s);
    std::string name = asserts()[id].name;
    Instr a;
    a.k = IK::Assert;
    a.sig = id;
    a.e = expr(s.cond);
    if (!s.isDeferred) {
        size_t at = prog.emit(a);
        if (!emptyStmt(s.ifTrue)) stmt(*s.ifTrue);
        size_t j = emitJmp();
        prog.code[at].target = prog.here();
        if (!emptyStmt(s.ifFalse)) stmt(*s.ifFalse);
        else if (dir != AD_COVER) emitAssertError(s, name);
        prog.code[at].hi = prog.here();
        prog.code[j].target = prog.here();
        return;
    }
    a.join = s.isFinal ? 2 : 1;
    std::vector<E> caps, slots;
    const Prog* pass = nullptr;
    const Prog* fail = nullptr;
    if (!emptyStmt(s.ifTrue)) {
        pass = actionProg(s.ifTrue, s, name, caps, slots);
        asserts()[id].passCap = std::move(caps);
        asserts()[id].passSlot = std::move(slots);
    }
    caps.clear();
    slots.clear();
    if (!emptyStmt(s.ifFalse)) fail = actionProg(s.ifFalse, s, name, caps, slots);
    else if (dir != AD_COVER) fail = actionProg(nullptr, s, name, caps, slots);
    asserts()[id].failCap = std::move(caps);
    asserts()[id].failSlot = std::move(slots);
    asserts()[id].pass = pass;
    asserts()[id].fail = fail;
    prog.emit(a);
}

// $asserton, $assertoff, $assertkill, $assertpasson ... and $assertcontrol
// (20.11). False if n is none of them.
bool assertCtlCall(const slang::ast::CallExpression& call, std::string_view n) {
    using namespace slang::ast;
    static const std::pair<std::string_view, std::pair<int, int>> tasks[] = {
        {"$asserton", {AC_ON, 15}}, {"$assertoff", {AC_OFF, 15}}, {"$assertkill", {AC_KILL, 15}},
        {"$assertpasson", {AC_PASSON, 31}}, {"$assertpassoff", {AC_PASSOFF, 31}},
        {"$assertfailon", {AC_FAILON, 31}}, {"$assertfailoff", {AC_FAILOFF, 31}},
        {"$assertnonvacuouson", {AC_NONVACON, 31}}, {"$assertvacuousoff", {AC_VACOFF, 31}},
        {"$assertcontrol", {0, 255}}};
    int ctl = -1, atype = 0;
    for (auto& [name, v] : tasks)
        if (n == name) { ctl = v.first; atype = v.second; }
    if (ctl < 0) return false;
    auto args = call.arguments();
    Instr i;
    i.k = IK::AssertCtl;
    auto argOr = [&](size_t k, int dflt) -> E {
        if (k < args.size() && args[k]->kind != ExpressionKind::EmptyArgument) return resize(expr(*args[k]), 32, true, true);
        return konst(mk((uint64_t)dflt, 32));
    };
    size_t first;   // the levels argument
    if (ctl == 0) {
        if (args.empty()) unsupported("$assertcontrol without a control_type");
        i.args = {argOr(0, 0), argOr(1, 255), argOr(2, 7), argOr(3, 0)};
        first = 3;
    } else {
        i.args = {konst(mk((uint64_t)ctl, 32)), konst(mk((uint64_t)atype, 32)), konst(mk(7, 32)), argOr(0, 0)};
        first = 0;
    }
    for (size_t k = first + 1; k < args.size(); k++) {
        const Expression& x = peel(*args[k]);
        const Symbol* sym = nullptr;
        if (x.kind == ExpressionKind::ArbitrarySymbol) sym = x.as<ArbitrarySymbolExpression>().symbol;
        else if (ValueExpressionBase::isKind(x.kind)) sym = &x.as<ValueExpressionBase>().symbol;
        if (!sym) unsupported("assertion control target that is not a scope or an assertion");
        i.fmt += sym->getHierarchicalPath() + "\n";
    }
    prog.emit(i);
    return true;
}

// ---- sampled values (16.5.1) ----
// Variables that are sampled as their current value: automatic ones.
std::unordered_set<int>& automaticSigs() {
    static std::unordered_set<int> s;
    static size_t built = ~size_t(0);
    if (built != sigmap.size()) {
        s.clear();
        for (auto& [sym, id] : sigmap)
            if (const auto* v = sym->as_if<slang::ast::VariableSymbol>())
                if (v->lifetime == slang::ast::VariableLifetime::Automatic) s.insert(id);
        built = sigmap.size();
    }
    return s;
}
static std::unordered_map<int, int>& shadowMap() { static std::unordered_map<int, int> m; return m; }
static std::unordered_map<int, std::shared_ptr<const ArrShape>>& shadowArrs() {
    static std::unordered_map<int, std::shared_ptr<const ArrShape>> m;
    return m;
}
int shadowOf(int s) {
    auto& m = shadowMap();
    if (auto it = m.find(s); it != m.end()) return it->second;
    if (!mkSig || (size_t)s >= sigInfo().size()) unsupported("sampled value of this signal");
    auto [n, w] = sigInfo()[s];
    int sh = mkSig(n + "$smp", w, false);
    shadowPairs().push_back({s, sh});
    m[s] = sh;
    return sh;
}
// An expression with every static variable read replaced by its sampled value.
E sampledE(const E& e) {
    if (!e) return e;
    if (e->op == Op::Ref) {
        if (isRel(e->sig) || automaticSigs().count(e->sig)) return e;   // automatic: read as it is
        auto c = std::make_shared<Expr>(*e);
        c->sig = shadowOf(e->sig);
        return c;
    }
    if (e->op == Op::DynGet || e->op == Op::DynSize || e->op == Op::DynExists || e->op == Op::DynReduce ||
        e->op == Op::DynSlice)
        return e;   // containers are read as they are now
    auto c = std::make_shared<Expr>(*e);
    if (e->op == Op::ArrRead) {
        auto& sh = shadowArrs()[e->arr->base];
        if (!sh || sh->count != e->arr->count) {
            auto a = std::make_shared<ArrShape>(*e->arr);
            int first = -1;
            for (int k = 0; k < a->count; k++) {
                int s = shadowOf(e->arr->base + k);
                if (first < 0) first = s;
                else if (s != first + k) unsupported("sampled value of this array");
            }
            a->base = first;
            sh = a;
        }
        c->arr = sh;
    }
    for (auto& k : c->kids) k = sampledE(k);
    return c;
}

// The clock of a sampled value function: its clocking_event argument, else
// the clock of the context (16.9.3), else the default clocking (14.12).
std::vector<std::pair<int, Edge>> clockArg(const slang::ast::CallExpression& call, size_t idx) {
    using namespace slang::ast;
    std::vector<std::pair<int, Edge>> ev;
    auto args = call.arguments();
    if (idx < args.size() && args[idx]->kind == ExpressionKind::ClockingEvent) {
        collectEvents(args[idx]->as<ClockingEventExpression>().timingControl, ev);
        return ev;
    }
    if (!sampleEv.empty()) return sampleEv;
    if (compilation() && scope)
        if (const Symbol* dc = compilation()->getDefaultClocking(*scope))
            if (const auto* cb = dc->as_if<ClockingBlockSymbol>()) {
                collectEvents(cb->getEvent(), ev);
                return ev;
            }
    unsupported("sampled value function with no clock to use (16.9.3)");
}
// $past(e, n, gate) on clock ev: the signal holding the value n ticks back.
E pastOf(const slang::ast::Expression& x, int n, E gate, const std::vector<std::pair<int, Edge>>& ev) {
    int w = storeW(*x.type);
    if (w <= 0) unsupported("$past of this type");
    auto& clocks = sampleClocks();
    SampleClock* k = nullptr;
    if (!gate)
        for (auto& c : clocks)
            if (c.ev == ev && !c.gate) k = &c;
    if (!k) {
        clocks.push_back({ev, gate, {}});
        k = &clocks.back();
    }
    SampleClock::Line ln;
    ln.src = sampledE(expr(x));
    for (int j = 0; j < n; j++) ln.stages.push_back(mkSig("$past" + std::to_string(*tempSeq) + "_" + std::to_string(j), w, false));
    (*tempSeq)++;
    int last = ln.stages.back();
    k->lines.push_back(std::move(ln));
    return ref(last, x.type->isSigned());
}
// $sampled, $rose, $fell, $stable, $changed, $past (16.9.3). Null if n is
// none of them.
E sampledFn(const slang::ast::CallExpression& call, std::string_view n) {
    using namespace slang::ast;
    bool past = n == "$past";
    if (n != "$sampled" && n != "$rose" && n != "$fell" && n != "$stable" && n != "$changed" && !past) return nullptr;
    auto args = call.arguments();
    if (args.empty()) unsupported("sampled value function without an argument");
    if (!mkSig || !tempSeq) unsupported("sampled value function here");
    const Expression& x = *args[0];
    if (n == "$sampled") return sampledE(expr(x));
    if (past) {
        int k = 1;
        if (args.size() > 1 && args[1]->kind != ExpressionKind::EmptyArgument) k = (int)constInt(*args[1]);
        if (k < 1) unsupported("$past with fewer than 1 tick");
        E gate;
        if (args.size() > 2 && args[2]->kind != ExpressionKind::EmptyArgument) gate = sampledE(expr(*args[2]));
        return pastOf(x, k, gate, clockArg(call, 3));
    }
    E cur = sampledE(expr(x));
    E prev = pastOf(x, 1, nullptr, clockArg(call, 1));
    int w = storeW(*x.type);
    cur = resize(cur, w, false, false);
    if (n == "$stable" || n == "$changed") {
        E st = bin(Op::CaseEq, cur, prev);
        return n == "$stable" ? st : un(Op::LogNot, st);
    }
    auto bit0 = [](E v) { E s = un(Op::Slice, v); s->hi = 0; s->lo = 0; return s; };
    Val want = mk(n == "$rose" ? 1 : 0, 1);
    return bin(Op::LogAnd, bin(Op::CaseEq, bit0(cur), konst(want)),
               un(Op::LogNot, bin(Op::CaseEq, bit0(prev), konst(want))));
}
