// lower_assert.inc - #include'd inside struct Lowerer (lower.hpp).
// Immediate and deferred assertions (IEEE 1800-2023 16.3, 16.4), assertion
// control tasks (20.11) and sampled value functions (16.5.1, 16.9.3).
// The runtime side is in sva.hpp and exec.inc.

const slang::ast::Scope* scope = nullptr;   // set by setScope
bool staticAssert = false;   // lowering a module-level assertion (16.14.5), not procedural code
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
    a.prot = hideLocIf() && hideLocIf()(st.sourceRange.start());   // the coverage right (1735)
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
    bool hidden = hideMsgIf() && hideMsgIf()(st.sourceRange.start());
    std::string escFile, escName;
    for (char c : file) { escFile += c; if (c == '%') escFile += '%'; }
    for (char c : hidden ? instPath(scope) + ".<protected assertion>" : name) { escName += c; if (c == '%') escName += '%'; }
    Instr i; i.k = IK::Display;
    i.fmt = "Error: \"" + escFile + "\", " + (hidden ? std::string("protected code") : std::to_string(line)) + ": " +
            escName + ": at time %0t";
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
// The hidden signals of assertion local variables (16.10): read as they are.
static std::unordered_set<int>& svaLocalSigs() { static std::unordered_set<int> s; return s; }
// The signals of sequence end points (16.13.6): read as they are, too.
static std::unordered_set<int>& svaLiveSigs() { static std::unordered_set<int> s; return s; }
// The checker whose body is being lowered, when it has assume statements and
// active free variables (17.7.2): those variables, and the assume set of
// each clock (its first assumption, which holds the solve).
struct FreeSolve {
    bool active = false;
    std::vector<int> vars;
    std::vector<std::pair<std::vector<std::pair<int, Edge>>, int>> groups;
};
static FreeSolve& freeSolve() { static FreeSolve f; return f; }
// Signals from this id on are read as they are (temporaries; see sampleProg).
static int& sampleBelow() { static int n = INT_MAX; return n; }
E sampledE(const E& e) {
    if (!e) return e;
    if (e->op == Op::Const) return e;
    if (e->op == Op::Ref) {
        if (isRel(e->sig) || automaticSigs().count(e->sig) || svaLocalSigs().count(e->sig) || svaLiveSigs().count(e->sig) ||
            e->sig >= sampleBelow())
            return e;   // automatic, local, or an end point: read as it is
        auto c = std::make_shared<Expr>(*e);
        c->sig = shadowOf(e->sig);
        return c;
    }
    // DynSlice is not a container: v[i] and v[i +: w] with a variable i read the sampled v and i.
    if (e->op == Op::DynGet || e->op == Op::DynSize || e->op == Op::DynExists || e->op == Op::DynReduce ||
        e->op == Op::DynElemRef)
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

// Every expression of prog but its event controls reads sampled values
// (a checker's always_ff, 17.5); signals from `below` on are read as they are.
void sampleProg(int below) {
    int saved = sampleBelow();
    sampleBelow() = below;
    for (auto& i : prog.code) {
        if (i.k == IK::WaitEdge) continue;
        i.e = sampledE(i.e);
        for (auto& a : i.args) a = sampledE(a);
        for (auto& a : i.aidx) a = sampledE(a);
        i.bidx = sampledE(i.bidx);
    }
    sampleBelow() = saved;
}

// The clock of a sampled value function: its clocking_event argument, else
// the clock of the context (16.9.3), else the default clocking (14.12).
std::vector<std::pair<int, Edge>> clockArg(const slang::ast::CallExpression& call, size_t idx) {
    using namespace slang::ast;
    std::vector<std::pair<int, Edge>> ev;
    auto args = call.arguments();
    if (idx < args.size() && args[idx]->kind == ExpressionKind::ClockingEvent) {
        E iff;
        clockOf(args[idx]->as<ClockingEventExpression>().timingControl, ev, iff);
        return ev;
    }
    if (!sampleEv.empty()) return sampleEv;
    if (const auto* cb = defaultClockingBlock()) {
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

// ---- concurrent assertions (16.5 to 16.18) ----
// The terms of one assertion while it is built (sva_engine.hpp).
struct SvaBuild {
    std::vector<E> atoms;
    std::vector<char> atomLocal;     // the atom reads a local variable
    std::vector<int> localSigs;      // local variables (16.10), as hidden signals
    std::vector<std::vector<ConcAssert::Item>> acts;   // match items and initializers
    std::vector<int> matchFlags;     // .matched flags read (16.13.6)
    std::vector<std::pair<std::vector<std::pair<int, Edge>>, E>> more;   // the other clocks (16.13)
    std::unordered_map<const slang::ast::Expression*, int> atomOf;
    std::vector<std::pair<int, Edge>> clk;
    E clkIff;
};
SvaBuild* svb = nullptr;

// A boolean of the assertion, on sampled values (16.5.1).
static bool readsLocal(const E& e) {
    if (!e) return false;
    if (e->op == Op::Ref && svaLocalSigs().count(e->sig)) return true;
    for (const auto& k : e->kids)
        if (readsLocal(k)) return true;
    return false;
}
int svaAtom(const slang::ast::Expression& x) {
    if (auto it = svb->atomOf.find(&x); it != svb->atomOf.end()) return it->second;
    int id = svaAtomE(sampledE(expr(x)));
    svb->atomOf[&x] = id;
    return id;
}
int svaAtomE(E e) {
    svb->atomLocal.push_back(readsLocal(e));
    svb->atoms.push_back(std::move(e));
    return (int)svb->atoms.size() - 1;
}
// The local variables of a sequence or property instance (16.10): a hidden
// signal each, and the act that initializes them when the instance starts
// (declaration initializers and local input arguments, 16.8.2); -1: none.
int instLocals(const slang::ast::AssertionInstanceExpression& ai) {
    using namespace slang::ast;
    if (ai.isRecursiveProperty) unsupported("recursive property (16.12.17)");
    if (ai.localVars.empty()) return -1;
    std::vector<ConcAssert::Item> init;
    for (const LocalAssertionVarSymbol* lv : ai.localVars) {
        const Type& t = lv->getType();
        if (!t.isIntegral()) unsupported("a local variable that is not integral (16.10)");
        int w = (int)t.getBitWidth();
        if (!mkSig) unsupported("local variable here");
        int sg = mkSig(scopePath(scope) + ".$lv_" + std::string(lv->name) + std::to_string(tempSeq ? (*tempSeq)++ : 0), w, false);
        sigmap[lv] = sg;
        svaLocalSigs().insert(sg);
        int k = (int)svb->localSigs.size();
        svb->localSigs.push_back(sg);
        const Expression* from = nullptr;
        if (lv->formalPort) {
            if (lv->formalPort->direction && *lv->formalPort->direction != ArgumentDirection::In)
                unsupported("a local variable argument that is output or inout (16.8.2)");
            for (auto& [port, actual] : ai.arguments)
                if (port == lv->formalPort)
                    if (auto* x = std::get_if<const Expression*>(&actual)) from = *x;
        } else if (const DeclaredType* dt = lv->getDeclaredType())
            from = dt->getInitializer();
        if (from) init.push_back({k, resize(sampledE(expr(*from)), w, from->type->isSigned(), false)});
    }
    if (init.empty()) return -1;
    svb->acts.push_back(std::move(init));
    return (int)svb->acts.size() - 1;
}
// A subroutine call in a match item (16.11) as a process. Its arguments are
// evaluated at the match, on sampled values and the local variables' values
// then: each becomes a constant slot, as in actionProg.
const Prog* callProg(const slang::ast::CallExpression& call, std::vector<E>& caps, std::vector<E>& slots) {
    if (!store) unsupported("a subroutine call in a match item here");
    Lowerer lw(sigmap);
    lw.bind(subs, mkSig, tempSeq, store);
    lw.tsUnit = tsUnit; lw.tsPrec = tsPrec;
    lw.scope = scope;
    lw.current = current;
    lw.sampleEv = sampleEv;
    if (call.isSystemCall()) lw.sysCall(call);
    else lw.emitUserCall(call, false);
    lw.finishImplicit();
    auto cap = [&](E& e) {
        if (!e || e->op == Op::Const) return;
        caps.push_back(sampledE(e));
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
// Match items (16.10): assignments, ++ and -- of local variables, as an act; -1: none.
int matchAct(std::span<const slang::ast::Expression* const> items) {
    using namespace slang::ast;
    if (items.empty()) return -1;
    std::vector<ConcAssert::Item> act;
    auto localOf = [&](const Expression& x) -> int {
        const Expression& t = peel(x);
        if (t.kind == ExpressionKind::NamedValue)
            if (auto it = sigmap.find(&t.as<NamedValueExpression>().symbol); it != sigmap.end())
                for (size_t k = 0; k < svb->localSigs.size(); k++)
                    if (svb->localSigs[k] == it->second) return (int)k;
        unsupported("a match item that assigns something other than a local variable (16.10)");
    };
    for (const Expression* it : items) {
        if (it->kind == ExpressionKind::Assignment) {
            const auto& a = it->as<AssignmentExpression>();
            int k = localOf(a.left());
            int w = (int)a.left().type->getBitWidth();
            const Expression* saved = lvalue;
            if (a.isCompound()) lvalue = &a.left();
            E r = expr(a.right());
            lvalue = saved;
            act.push_back({k, resize(sampledE(r), w, a.right().type->isSigned(), false)});
        } else if (it->kind == ExpressionKind::UnaryOp) {
            const auto& u = it->as<UnaryExpression>();
            bool inc = u.op == UnaryOperator::Preincrement || u.op == UnaryOperator::Postincrement;
            bool dec = u.op == UnaryOperator::Predecrement || u.op == UnaryOperator::Postdecrement;
            if (!inc && !dec) unsupported("this match item (16.10)");
            int k = localOf(u.operand());
            int w = (int)u.operand().type->getBitWidth();
            act.push_back({k, resize(bin(inc ? Op::Add : Op::Sub, expr(u.operand()), konst(mk(1, 32))), w, false, false)});
        } else if (it->kind == ExpressionKind::Call) {
            ConcAssert::Item ci;
            ci.call = callProg(it->as<CallExpression>(), ci.caps, ci.slots);
            act.push_back(std::move(ci));
        } else
            unsupported("this match item (16.10)");
    }
    svb->acts.push_back(std::move(act));
    return (int)svb->acts.size() - 1;
}
// A clocking event as signal events and an optional iff condition.
void clockOf(const slang::ast::TimingControl& t, std::vector<std::pair<int, Edge>>& ev, E& iff) {
    using namespace slang::ast;
    if (t.kind == TimingControlKind::SignalEvent) {
        const auto& s = t.as<SignalEventControl>();
        const auto* cb = isGlobalClock(s.expr) ? globalClocking() : clockingRef(s.expr);
        if (!cb) cb = inferredClock(s.expr);
        if (cb) {
            clockOf(cb->getEvent(), ev, iff);   // @(cb) or @($global_clock) as a clock (14.10, 14.14, 16.16)
            return;
        }
        if (s.iffCondition) {
            if (iff) unsupported("a clock with more than one iff");
            iff = expr(*s.iffCondition);
        }
    }
    collectEvents(t, ev);
}
// A clock inside the assertion (16.13): its index, 0 for the leading clock;
// a different one is a clock of a multiclock assertion.
int clockIdx(const slang::ast::TimingControl& t) {
    std::vector<std::pair<int, Edge>> ev;
    E iff;
    clockOf(t, ev, iff);
    if (svb->clk.empty()) { svb->clk = ev; svb->clkIff = iff; return 0; }
    if (ev == svb->clk && !iff && !svb->clkIff) return 0;
    for (size_t k = 0; k < svb->more.size(); k++)
        if (svb->more[k].first == ev && !iff && !svb->more[k].second) return (int)k + 1;
    if (svb->more.size() >= 31) unsupported("an assertion with more than 32 clocks");
    svb->more.push_back({ev, iff});
    return (int)svb->more.size();
}
// t's subterm under clock ci.
template <class F> const sva::Node* underClock(int ci, F f) {
    auto& st = sva::store();
    int saved = st.clk;
    st.clk = ci;
    const sva::Node* r = f();
    st.clk = saved;
    return r;
}
static int rangeHi(const std::optional<uint32_t>& m) { return m ? (int)*m : -1; }

const sva::Node* repeated(const sva::Node* base, const slang::ast::Expression* boolX,
                          const std::optional<slang::ast::SequenceRepetition>& rp) {
    using slang::ast::SequenceRepetition;
    auto& st = sva::store();
    if (!rp) return base;
    int lo = (int)rp->range.min, hi = rangeHi(rp->range.max);
    switch (rp->kind) {
        case SequenceRepetition::Consecutive: return st.rep(base, lo, hi);
        case SequenceRepetition::GoTo:
        case SequenceRepetition::Nonconsecutive: {
            if (!boolX) unsupported("goto or nonconsecutive repetition of a sequence");
            int a = svaAtom(*boolX);
            return rp->kind == SequenceRepetition::GoTo ? st.gotoRep(a, lo, hi) : st.nonconsec(a, lo, hi);
        }
    }
    return base;
}
static const slang::ast::AssertionInstanceExpression* instanceOf(const slang::ast::AssertionExpr& a) {
    using namespace slang::ast;
    if (a.kind != AssertionExprKind::Simple) return nullptr;
    const Expression& x = a.as<SimpleAssertionExpr>().expr;
    if (x.kind != ExpressionKind::AssertionInstance) return nullptr;
    return &x.as<AssertionInstanceExpression>();
}

// A sequence (16.7, 16.9).
const sva::Node* seqTerm(const slang::ast::AssertionExpr& a) {
    using namespace slang::ast;
    auto& st = sva::store();
    switch (a.kind) {
        case AssertionExprKind::Simple: {
            const auto& s = a.as<SimpleAssertionExpr>();
            if (const auto* ai = instanceOf(a)) {
                int init = instLocals(*ai);
                return repeated(st.init(init, seqTerm(ai->body)), nullptr, s.repetition);
            }
            return repeated(st.boolean(svaAtom(s.expr)), &s.expr, s.repetition);
        }
        case AssertionExprKind::SequenceConcat: {
            const sva::Node* r = nullptr;
            for (auto& el : a.as<SequenceConcatExpr>().elements) {
                const sva::Node* s = seqTerm(*el.sequence);
                int lo = (int)el.delay.min, hi = rangeHi(el.delay.max);
                if (!r && lo == 0 && hi == 0) r = s;
                else r = st.delay(r, lo, hi, s);
            }
            return r ? r : st.empty();
        }
        case AssertionExprKind::SequenceWithMatch: {
            const auto& s = a.as<SequenceWithMatchExpr>();
            const sva::Node* r = seqTerm(s.expr);
            return repeated(st.match(r, matchAct(s.matchItems)), nullptr, s.repetition);
        }
        case AssertionExprKind::FirstMatch: {
            const auto& f = a.as<FirstMatchAssertionExpr>();
            const sva::Node* r = st.fm(seqTerm(f.seq));
            return st.match(r, matchAct(f.matchItems));
        }
        case AssertionExprKind::Binary: {
            const auto& b = a.as<BinaryAssertionExpr>();
            switch (b.op) {
                case BinaryAssertionOperator::And: return st.seqAnd(seqTerm(b.left), seqTerm(b.right));
                case BinaryAssertionOperator::Or: return st.sor({seqTerm(b.left), seqTerm(b.right)});
                case BinaryAssertionOperator::Intersect: return st.sand(seqTerm(b.left), seqTerm(b.right));
                case BinaryAssertionOperator::Within: return st.within(seqTerm(b.left), seqTerm(b.right));
                case BinaryAssertionOperator::Throughout: {
                    if (b.left.kind != AssertionExprKind::Simple) unsupported("throughout of a sequence");
                    return st.sand(st.rep(seqTerm(b.left), 0, -1), seqTerm(b.right));
                }
                default: unsupported("this property operator where a sequence is needed");
            }
        }
        case AssertionExprKind::Clocking: {
            const auto& c = a.as<ClockingAssertionExpr>();
            return underClock(clockIdx(c.clocking), [&] { return seqTerm(c.expr); });
        }
        default: unsupported("this sequence expression");
    }
}
// Whether an assertion expression is a sequence, as opposed to a property.
bool isSeq(const slang::ast::AssertionExpr& a) {
    using namespace slang::ast;
    switch (a.kind) {
        case AssertionExprKind::Simple:
            if (const auto* ai = instanceOf(a)) return ai->symbol.kind != SymbolKind::Property && isSeq(ai->body);
            return true;
        case AssertionExprKind::SequenceConcat:
        case AssertionExprKind::SequenceWithMatch:
        case AssertionExprKind::FirstMatch: return true;
        case AssertionExprKind::Binary: {
            const auto& b = a.as<BinaryAssertionExpr>();
            switch (b.op) {
                case BinaryAssertionOperator::Intersect:
                case BinaryAssertionOperator::Within:
                case BinaryAssertionOperator::Throughout: return true;
                case BinaryAssertionOperator::And:
                case BinaryAssertionOperator::Or: return isSeq(b.left) && isSeq(b.right);
                default: return false;
            }
        }
        case AssertionExprKind::Clocking: return isSeq(a.as<ClockingAssertionExpr>().expr);
        default: return false;
    }
}
// A property (16.12). `strong`: how a bare sequence is read (16.12.2).
const sva::Node* propTerm(const slang::ast::AssertionExpr& a, bool strong) {
    using namespace slang::ast;
    auto& st = sva::store();
    if (const auto* ai = instanceOf(a); ai && ai->symbol.kind == SymbolKind::Property) {
        int init = instLocals(*ai);
        return st.init(init, propTerm(ai->body, strong));
    }
    if (isSeq(a)) return st.pseq(seqTerm(a), strong);
    switch (a.kind) {
        case AssertionExprKind::Unary: {
            const auto& u = a.as<UnaryAssertionExpr>();
            int lo = u.range ? (int)u.range->min : 0, hi = u.range ? rangeHi(u.range->max) : -1;
            switch (u.op) {
                case UnaryAssertionOperator::Not: return st.pnot(propTerm(u.expr, strong));
                case UnaryAssertionOperator::NextTime:
                case UnaryAssertionOperator::SNextTime:
                    return st.next(propTerm(u.expr, strong), u.range ? (int)u.range->min : 1,
                                   u.op == UnaryAssertionOperator::SNextTime);
                case UnaryAssertionOperator::Always:
                case UnaryAssertionOperator::SAlways:
                    return st.ranged(sva::ALWAYS, propTerm(u.expr, strong), lo, hi, u.op == UnaryAssertionOperator::SAlways);
                case UnaryAssertionOperator::Eventually:
                case UnaryAssertionOperator::SEventually:
                    return st.ranged(sva::EVENT, propTerm(u.expr, strong), lo, hi, u.op == UnaryAssertionOperator::SEventually);
            }
            break;
        }
        case AssertionExprKind::Binary: {
            const auto& b = a.as<BinaryAssertionExpr>();
            auto P = [&](const AssertionExpr& x) { return propTerm(x, strong); };
            switch (b.op) {
                case BinaryAssertionOperator::And: return st.pand({P(b.left), P(b.right)});
                case BinaryAssertionOperator::Or: return st.por({P(b.left), P(b.right)});
                case BinaryAssertionOperator::Iff: {
                    const sva::Node* l = P(b.left), *r = P(b.right);
                    return st.por({st.pand({l, r}), st.pand({st.pnot(l), st.pnot(r)})});
                }
                case BinaryAssertionOperator::Implies: return st.por({st.pnot(P(b.left)), P(b.right)});
                case BinaryAssertionOperator::Until: return st.until(P(b.left), P(b.right), false, false);
                case BinaryAssertionOperator::SUntil: return st.until(P(b.left), P(b.right), true, false);
                case BinaryAssertionOperator::UntilWith: return st.until(P(b.left), P(b.right), false, true);
                case BinaryAssertionOperator::SUntilWith: return st.until(P(b.left), P(b.right), true, true);
                case BinaryAssertionOperator::OverlappedImplication:
                    return st.impl(seqTerm(b.left), P(b.right), false);
                case BinaryAssertionOperator::NonOverlappedImplication:
                    return st.impl(seqTerm(b.left), P(b.right), true);
                case BinaryAssertionOperator::OverlappedFollowedBy:      // s #-# p = not (s |-> not p)
                    return st.pnot(st.impl(seqTerm(b.left), st.pnot(P(b.right)), false));
                case BinaryAssertionOperator::NonOverlappedFollowedBy:   // s #=# p = not (s |=> not p)
                    return st.pnot(st.impl(seqTerm(b.left), st.pnot(P(b.right)), true));
                default: break;
            }
            break;
        }
        case AssertionExprKind::Clocking: {
            const auto& c = a.as<ClockingAssertionExpr>();
            return underClock(clockIdx(c.clocking), [&] { return propTerm(c.expr, strong); });
        }
        case AssertionExprKind::StrongWeak: {
            const auto& s = a.as<StrongWeakAssertionExpr>();
            return st.pseq(seqTerm(s.expr), s.strength == StrongWeakAssertionExpr::Strong);
        }
        case AssertionExprKind::Abort: {
            const auto& ab = a.as<AbortAssertionExpr>();
            return st.abort(ab.action == AbortAssertionExpr::Accept ? sva::ACCEPT : sva::REJECT, svaAtom(ab.condition),
                            propTerm(ab.expr, strong));
        }
        case AssertionExprKind::Conditional: {
            const auto& c = a.as<ConditionalAssertionExpr>();
            return st.pif(svaAtom(c.condition), propTerm(c.ifExpr, strong),
                          c.elseExpr ? propTerm(*c.elseExpr, strong) : nullptr);
        }
        case AssertionExprKind::Case: {   // 16.12.16: the first item whose expression matches
            const auto& c = a.as<CaseAssertionExpr>();
            E sel = sampledE(expr(c.expr));
            const sva::Node* r = c.defaultCase ? propTerm(*c.defaultCase, strong) : nullptr;
            for (size_t i = c.items.size(); i-- > 0;) {
                E cond;
                for (const Expression* x : c.items[i].expressions) {
                    E eq = bin(Op::CaseEq, sel, sampledE(expr(*x)));
                    cond = cond ? bin(Op::LogOr, cond, eq) : eq;
                }
                r = st.pif(svaAtomE(cond), propTerm(*c.items[i].body, strong), r);
            }
            return r ? r : st.ptrue();
        }
        case AssertionExprKind::DisableIff: unsupported("disable iff inside a property (16.12)");
        default: break;
    }
    unsupported("this property expression");
}

// The action block of a concurrent assertion as a process: `st`, or $error
// when it is null. It runs in the reactive region (16.14.1).
const Prog* concAction(const slang::ast::Statement* st, const slang::ast::Statement& as, const std::string& name) {
    if (!store) unsupported("concurrent assertion here");
    Lowerer lw(sigmap);
    lw.bind(subs, mkSig, tempSeq, store);
    lw.tsUnit = tsUnit; lw.tsPrec = tsPrec;
    lw.scope = scope;
    lw.current = current;
    lw.sampleEv = sampleEv;
    if (st) lw.stmt(*st);
    else lw.emitAssertError(as, name);
    lw.finishImplicit();
    store->push_back(std::make_unique<Prog>(std::move(lw.prog)));
    return store->back().get();
}

// The end point tracker of a sequence instance (16.13.6, 9.4.2.4): its first
// signal counts the ticks where the sequence matches (@(s) waits on it), the
// next holds the time + 1 of the last one (.triggered compares it with now).
// It runs like a cover sequence, starting an attempt at every tick of the
// sequence's clock: its own, else the clock of the assertion reading it,
// else the default clocking.
static std::map<const void*, int>& seqTrackers() { static std::map<const void*, int> m; return m; }   // -> concAsserts index
int seqTracker(const slang::ast::AssertionInstanceExpression& ai) {
    using namespace slang::ast;
    if (ai.symbol.kind != SymbolKind::Sequence) unsupported("an end point of something other than a sequence");
    // Instances without arguments share one tracker.
    const void* key = ai.arguments.empty() ? (const void*)&ai.symbol : (const void*)&ai;
    if (auto it = seqTrackers().find(key); it != seqTrackers().end()) return it->second;
    if (!mkSig) unsupported("a sequence end point here");
    std::string base = ai.symbol.getHierarchicalPath() + "$end" + std::to_string(tempSeq ? (*tempSeq)++ : 0);
    int sg = mkSig(base, 32, false);
    if (mkSig(base + "$t", 64, false) != sg + 1) unsupported("sequence end point signals that are not consecutive");
    svaLiveSigs().insert(sg);
    svaLiveSigs().insert(sg + 1);
    SvaBuild b;
    SvaBuild* saved = svb;
    const std::vector<char>* savedLocal = sva::store().localAtoms;
    auto savedEv = sampleEv;
    const AssertionExpr* body = &ai.body;
    if (body->kind == AssertionExprKind::Clocking) {
        clockOf(body->as<ClockingAssertionExpr>().clocking, b.clk, b.clkIff);
        body = &body->as<ClockingAssertionExpr>().expr;
    } else if (saved && !saved->clk.empty()) {
        b.clk = saved->clk;
        b.clkIff = saved->clkIff;
    } else if (const auto* cb = defaultClockingBlock())
        clockOf(cb->getEvent(), b.clk, b.clkIff);
    if (b.clk.empty()) unsupported("an end point of a sequence with no clock (16.13.6)");
    svb = &b;
    sva::store().localAtoms = &b.atomLocal;
    int savedClk = sva::store().clk;
    sva::store().clk = 0;
    sampleEv = b.clk;
    ConcAssert c;
    AssertInfo info;
    info.name = base;
    info.inst = instPath(scope);
    info.type = AT_CONCURRENT;
    info.dir = AD_COVER;
    info.hidden = true;
    c.id = (int)asserts().size();
    asserts().push_back(std::move(info));
    c.coverSeq = true;
    int init = instLocals(ai);
    c.root = sva::store().covseq(sva::store().init(init, seqTerm(*body)));
    c.ev = b.clk;
    c.clkIff = b.clkIff;
    c.atoms = std::move(b.atoms);
    c.localSig = std::move(b.localSigs);
    c.acts = std::move(b.acts);
    c.endSig = sg;
    c.more = std::move(b.more);
    svb = saved;
    sva::store().localAtoms = savedLocal;
    sva::store().clk = savedClk;
    sampleEv = savedEv;
    int k = (int)concAsserts().size();
    concAsserts().push_back(std::move(c));
    seqTrackers()[key] = k;
    return k;
}
// s.triggered (true in a time step where s matched), and s.matched: in an
// assertion, true when s matched since the assertion's last tick (its clock
// may differ, 16.13.6); elsewhere the same as triggered.
E seqEndPoint(const slang::ast::Expression& arg, bool matched) {
    using namespace slang::ast;
    const Expression& x = peel(arg);
    if (x.kind != ExpressionKind::AssertionInstance) unsupported("triggered or matched of this expression");
    int k = seqTracker(x.as<AssertionInstanceExpression>());
    if (matched && svb) {
        if (!mkSig) unsupported("matched here");
        int f = mkSig(asserts()[concAsserts()[k].id].name + "$m" + std::to_string(tempSeq ? (*tempSeq)++ : 0), 1, false);
        svaLiveSigs().insert(f);
        concAsserts()[k].matchFlags.push_back(f);
        svb->matchFlags.push_back(f);
        return bin(Op::CaseEq, ref(f), konst(mk(1, 1)));
    }
    int s = concAsserts()[k].endSig;
    E t = node(Op::Time);
    t->k = mk(1, 64);   // raw ticks
    return bin(Op::CaseEq, ref(s + 1), bin(Op::Add, t, konst(mk(1, 64))));
}
// @(s) for a sequence instance s (9.4.2.4): its end point count.
int seqEventSig(const slang::ast::Expression& x) {
    return concAsserts()[seqTracker(peel(x).as<slang::ast::AssertionInstanceExpression>())].endSig;
}

// assert / assume / cover property, cover sequence (16.14).
// The leading clock written inside a sequence or property operand, if any:
// `cover property (s)` with s's own @(posedge clk) is clocked by it (16.16).
const slang::ast::TimingControl* leadingClock(const slang::ast::AssertionExpr& a) {
    using namespace slang::ast;
    if (const auto* ai = instanceOf(a)) return leadingClock(ai->body);
    switch (a.kind) {
        case AssertionExprKind::Clocking: return &a.as<ClockingAssertionExpr>().clocking;
        case AssertionExprKind::SequenceConcat: {
            const auto& c = a.as<SequenceConcatExpr>();
            return c.elements.empty() ? nullptr : leadingClock(*c.elements[0].sequence);
        }
        case AssertionExprKind::SequenceWithMatch: return leadingClock(a.as<SequenceWithMatchExpr>().expr);
        case AssertionExprKind::FirstMatch: return leadingClock(a.as<FirstMatchAssertionExpr>().seq);
        case AssertionExprKind::Unary: return leadingClock(a.as<UnaryAssertionExpr>().expr);
        case AssertionExprKind::Binary: return leadingClock(a.as<BinaryAssertionExpr>().left);
        default: return nullptr;
    }
}
void concAssert(const slang::ast::ConcurrentAssertionStatement& s) {
    using namespace slang::ast;
    if (s.assertionKind == AssertionKind::Restrict) return;   // for formal tools only (16.14.4)
    bool expect = s.assertionKind == AssertionKind::Expect;
    if (expect && staticAssert) unsupported("expect outside a procedure (16.17)");
    SvaBuild b;
    SvaBuild* saved = svb;
    svb = &b;
    auto savedEv = sampleEv;
    int dir = directiveOf(s.assertionKind);
    // The leading clock and disable iff, through named property instances.
    const AssertionExpr* a = &s.propertySpec;
    const Expression* dis = nullptr;
    std::vector<int> inits;   // the local variables of the leading property instances
    sva::store().localAtoms = &b.atomLocal;
    sva::store().clk = 0;
    struct Reset { ~Reset() { sva::store().localAtoms = nullptr; sva::store().clk = 0; } } reset;
    for (;;) {
        if (const auto* ai = instanceOf(*a); ai && ai->symbol.kind == SymbolKind::Property) {
            inits.push_back(instLocals(*ai));
            a = &ai->body;
        } else if (a->kind == AssertionExprKind::Clocking) {
            const auto& c = a->as<ClockingAssertionExpr>();
            if (b.clk.empty()) clockOf(c.clocking, b.clk, b.clkIff);
            else sva::store().clk = clockIdx(c.clocking);
            a = &c.expr;
        } else if (a->kind == AssertionExprKind::DisableIff) {
            if (dis) unsupported("nested disable iff (16.12)");
            dis = &a->as<DisableIffAssertionExpr>().condition;
            a = &a->as<DisableIffAssertionExpr>().expr;
        } else break;
    }
    if (b.clk.empty())
        if (const auto* tc = leadingClock(*a)) clockOf(*tc, b.clk, b.clkIff);
    if (b.clk.empty()) {   // inferred from the procedure, else the default clocking (16.16)
        if (!sampleEv.empty()) b.clk = sampleEv;
        else if (const auto* cb = defaultClockingBlock()) clockOf(cb->getEvent(), b.clk, b.clkIff);
    }
    if (b.clk.empty()) unsupported("concurrent assertion with no clock (16.16)");
    if (dis && isSysCall(*dis, "$inferred_disable")) dis = defaultDisable();   // 16.14.7
    if (!dis) dis = defaultDisable();   // 16.15
    sampleEv = b.clk;   // $past and friends in the assertion use its clock
    ConcAssert c;
    c.id = newAssert(AT_CONCURRENT, dir, s);
    if (s.assertionKind == AssertionKind::CoverSequence) {
        c.coverSeq = true;
        c.root = sva::store().covseq(seqTerm(*a));
    } else
        c.root = propTerm(*a, dir == AD_COVER);
    for (size_t k = inits.size(); k-- > 0;) c.root = sva::store().init(inits[k], c.root);
    sva::store().localAtoms = nullptr;
    if (dis) c.disable = expr(*dis);   // current values, not sampled (16.12)
    std::string name = asserts()[c.id].name;
    c.expect = expect;
    if (expect) {}   // the action block runs in the process, after the verdict
    else if (!emptyStmt(s.ifTrue)) c.pass = concAction(s.ifTrue, s, name);
    if (expect) {}
    else if (!emptyStmt(s.ifFalse)) c.fail = concAction(s.ifFalse, s, name);
    else if (dir != AD_COVER) c.fail = concAction(nullptr, s, name);
    c.ev = b.clk;
    c.clkIff = b.clkIff;
    c.atoms = std::move(b.atoms);
    c.localSig = std::move(b.localSigs);
    c.acts = std::move(b.acts);
    c.clearFlags = std::move(b.matchFlags);
    c.more = std::move(b.more);
    if (!c.more.empty() && !staticAssert) unsupported("a multiclock assertion in a procedure (16.14.6)");
    if (!staticAssert) {   // in a procedure (16.14.6): queued when the procedure reaches it
        c.procedural = true;
        // Automatic variables are read as they were when the instance was
        // queued (16.14.6.1): each becomes a slot, filled per attempt.
        std::unordered_map<int, E> slotOf;
        std::function<E(const E&)> capture = [&](const E& e) -> E {
            if (!e) return e;
            if (e->op == Op::Ref && (isRel(e->sig) || automaticSigs().count(e->sig))) {
                auto [it, fresh] = slotOf.try_emplace(e->sig);
                if (fresh) {
                    it->second = konst(Val{});
                    c.capSig.push_back(e->sig);
                    c.capSlot.push_back(it->second);
                }
                return it->second;
            }
            bool same = true;
            std::vector<E> kids;
            for (auto& k : e->kids) { kids.push_back(capture(k)); same = same && kids.back() == k; }
            if (same) return e;
            auto n = std::make_shared<Expr>(*e);
            n->kids = std::move(kids);
            return n;
        };
        for (auto& at : c.atoms) at = capture(at);
        for (auto& act : c.acts)
            for (auto& item : act) {
                item.rhs = capture(item.rhs);
                for (auto& x : item.caps) x = capture(x);
            }
        for (const Prog* act : {c.pass, c.fail})   // the action blocks see them too
            if (act)
                for (auto& i : const_cast<Prog*>(act)->code) {
                    i.e = capture(i.e);
                    i.bidx = capture(i.bidx);
                    i.de = capture(i.de);
                    for (auto& x : i.args) x = capture(x);
                    for (auto& x : i.aidx) x = capture(x);
                }
        Instr q;
        q.k = IK::Assert;
        q.sig = c.id;
        q.join = expect ? 4 : 3;
        q.hi = (int)concAsserts().size();
        size_t at = prog.emit(q);
        if (expect) {   // pass: on; fail: target; disabled or killed: lo (after both)
            if (!emptyStmt(s.ifTrue)) stmt(*s.ifTrue);
            size_t j = emitJmp();
            prog.code[at].target = prog.here();
            if (!emptyStmt(s.ifFalse)) stmt(*s.ifFalse);
            else emitAssertError(s, name);
            prog.code[j].target = prog.here();
            prog.code[at].lo = (int)prog.here();
        }
    }
    if (s.assertionKind == AssertionKind::Assume && freeSolve().active && staticAssert) {
        // An assumption of a checker with active free variables (17.7.2):
        // its check in this time step joins the assume set of its clock.
        E ok = assumeNow(*a);
        if (ok && dis) ok = bin(Op::LogOr, un(Op::LogNot, un(Op::LogNot, expr(*dis))), ok);
        auto& fs = freeSolve();
        int g = -1;
        for (auto& [ev, idx] : fs.groups)
            if (ev == c.ev && !c.clkIff) g = idx;
        if (g < 0) {
            g = (int)concAsserts().size();
            fs.groups.push_back({c.ev, g});
            c.solveVars = fs.vars;
            c.solveOk = ok ? ok : konst(mk(1, 1));
        } else if (ok) {
            concAsserts()[g].solveOk = bin(Op::LogAnd, concAsserts()[g].solveOk, ok);
        }
    }
    concAsserts().push_back(std::move(c));
    sampleEv = savedEv;
    svb = saved;
}
// What assumption a requires in the current time step (17.7.2), on sampled
// values (active free variables read as they are): a boolean, an
// overlapping implication of booleans, and and / or / not of such checks.
// Null when it has no such check (a temporal property: not constrained).
E assumeNow(const slang::ast::AssertionExpr& a) {
    using namespace slang::ast;
    auto boolOf = [&](const AssertionExpr& x) -> E {
        if (x.kind != AssertionExprKind::Simple) return nullptr;
        const auto& se = x.as<SimpleAssertionExpr>();
        if (se.repetition) return nullptr;
        if (se.expr.kind == ExpressionKind::AssertionInstance) return nullptr;
        if (!se.expr.type->isIntegral() && !se.expr.type->isFloating()) return nullptr;
        return un(Op::LogNot, un(Op::LogNot, sampledE(expr(se.expr))));
    };
    if (const auto* ai = instanceOf(a); ai && ai->symbol.kind == SymbolKind::Property) return nullptr;
    switch (a.kind) {
        case AssertionExprKind::Simple: return boolOf(a);
        case AssertionExprKind::Unary: {
            const auto& u = a.as<UnaryAssertionExpr>();
            if (u.op != UnaryAssertionOperator::Not) return nullptr;
            E b = boolOf(u.expr);
            return b ? un(Op::LogNot, b) : nullptr;
        }
        case AssertionExprKind::Binary: {
            const auto& b = a.as<BinaryAssertionExpr>();
            if (b.op == BinaryAssertionOperator::OverlappedImplication) {
                E l = boolOf(b.left);
                E r = assumeNow(b.right);
                return l && r ? bin(Op::LogOr, un(Op::LogNot, l), r) : nullptr;
            }
            if (b.op == BinaryAssertionOperator::NonOverlappedImplication && !sampleEv.empty() &&
                b.left.kind == AssertionExprKind::Simple && !b.left.as<SimpleAssertionExpr>().repetition &&
                boolOf(b.left)) {   // l |=> r: r now if l held at the previous tick
                const Expression& lx = b.left.as<SimpleAssertionExpr>().expr;
                E r = assumeNow(b.right);
                if (!r) return nullptr;
                E was = un(Op::LogNot, un(Op::LogNot, pastOf(lx, 1, nullptr, sampleEv)));
                return bin(Op::LogOr, un(Op::LogNot, was), r);
            }
            if (b.op == BinaryAssertionOperator::And || b.op == BinaryAssertionOperator::Or) {
                E l = assumeNow(b.left), r = assumeNow(b.right);
                if (b.op == BinaryAssertionOperator::And) return l && r ? bin(Op::LogAnd, l, r) : l ? l : r;
                return l && r ? bin(Op::LogOr, l, r) : nullptr;
            }
            return nullptr;
        }
        default: return nullptr;
    }
}
