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
        if (automaticSigs().count(e->sig)) return e;
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

// ---- concurrent assertions (16.5 to 16.18) ----
// The terms of one assertion while it is built (sva_engine.hpp).
struct SvaBuild {
    std::vector<E> atoms;
    std::unordered_map<const slang::ast::Expression*, int> atomOf;
    std::vector<std::pair<int, Edge>> clk;
    E clkIff;
};
SvaBuild* svb = nullptr;

// A boolean of the assertion, on sampled values (16.5.1).
int svaAtom(const slang::ast::Expression& x) {
    if (auto it = svb->atomOf.find(&x); it != svb->atomOf.end()) return it->second;
    svb->atoms.push_back(sampledE(expr(x)));
    int id = (int)svb->atoms.size() - 1;
    svb->atomOf[&x] = id;
    return id;
}
int svaAtomE(E e) {
    svb->atoms.push_back(std::move(e));
    return (int)svb->atoms.size() - 1;
}
// A clocking event as signal events and an optional iff condition.
void clockOf(const slang::ast::TimingControl& t, std::vector<std::pair<int, Edge>>& ev, E& iff) {
    using namespace slang::ast;
    if (t.kind == TimingControlKind::SignalEvent) {
        const auto& s = t.as<SignalEventControl>();
        if (s.iffCondition) {
            if (iff) unsupported("a clock with more than one iff");
            iff = expr(*s.iffCondition);
        }
    }
    collectEvents(t, ev);
}
// A clock inside the assertion: the same as the leading clock, or multiclock.
void sameClock(const slang::ast::TimingControl& t) {
    std::vector<std::pair<int, Edge>> ev;
    E iff;
    clockOf(t, ev, iff);
    if (svb->clk.empty()) { svb->clk = ev; svb->clkIff = iff; return; }
    if (ev != svb->clk) unsupported("multiclock assertion (16.13)");
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
static void noLocals(const slang::ast::AssertionInstanceExpression& ai) {
    if (!ai.localVars.empty()) unsupported("local variables in a sequence or property (16.10)");
    if (ai.isRecursiveProperty) unsupported("recursive property (16.12.17)");
}

// A sequence (16.7, 16.9).
const sva::Node* seqTerm(const slang::ast::AssertionExpr& a) {
    using namespace slang::ast;
    auto& st = sva::store();
    switch (a.kind) {
        case AssertionExprKind::Simple: {
            const auto& s = a.as<SimpleAssertionExpr>();
            if (const auto* ai = instanceOf(a)) {
                noLocals(*ai);
                return repeated(seqTerm(ai->body), nullptr, s.repetition);
            }
            if (s.expr.kind == ExpressionKind::Call) {
                const auto& c = s.expr.as<CallExpression>();
                if (c.getSubroutineName() == "triggered" || c.getSubroutineName() == "matched")
                    unsupported("sequence methods triggered and matched (16.13.6)");
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
            if (!s.matchItems.empty()) unsupported("sequence match items (16.10, 16.11)");
            return repeated(seqTerm(s.expr), nullptr, s.repetition);
        }
        case AssertionExprKind::FirstMatch: {
            const auto& f = a.as<FirstMatchAssertionExpr>();
            if (!f.matchItems.empty()) unsupported("sequence match items (16.10, 16.11)");
            return st.fm(seqTerm(f.seq));
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
            sameClock(c.clocking);
            return seqTerm(c.expr);
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
        noLocals(*ai);
        return propTerm(ai->body, strong);
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
            sameClock(c.clocking);
            return propTerm(c.expr, strong);
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

// assert / assume / cover property, cover sequence (16.14).
void concAssert(const slang::ast::ConcurrentAssertionStatement& s) {
    using namespace slang::ast;
    if (s.assertionKind == AssertionKind::Restrict) return;   // for formal tools only (16.14.4)
    if (s.assertionKind == AssertionKind::Expect) unsupported("expect (16.17)");
    SvaBuild b;
    SvaBuild* saved = svb;
    svb = &b;
    auto savedEv = sampleEv;
    int dir = directiveOf(s.assertionKind);
    // The leading clock and disable iff, through named property instances.
    const AssertionExpr* a = &s.propertySpec;
    const Expression* dis = nullptr;
    for (;;) {
        if (const auto* ai = instanceOf(*a); ai && ai->symbol.kind == SymbolKind::Property) {
            noLocals(*ai);
            a = &ai->body;
        } else if (a->kind == AssertionExprKind::Clocking) {
            const auto& c = a->as<ClockingAssertionExpr>();
            if (b.clk.empty()) clockOf(c.clocking, b.clk, b.clkIff);
            else sameClock(c.clocking);
            a = &c.expr;
        } else if (a->kind == AssertionExprKind::DisableIff) {
            if (dis) unsupported("nested disable iff (16.12)");
            dis = &a->as<DisableIffAssertionExpr>().condition;
            a = &a->as<DisableIffAssertionExpr>().expr;
        } else break;
    }
    if (b.clk.empty()) {   // inferred from the procedure, else the default clocking (16.16)
        if (!sampleEv.empty()) b.clk = sampleEv;
        else if (compilation() && scope)
            if (const Symbol* dc = compilation()->getDefaultClocking(*scope))
                if (const auto* cb = dc->as_if<ClockingBlockSymbol>()) clockOf(cb->getEvent(), b.clk, b.clkIff);
    }
    if (b.clk.empty()) unsupported("concurrent assertion with no clock (16.16)");
    if (!dis && compilation() && scope) dis = compilation()->getDefaultDisable(*scope);   // 16.15
    sampleEv = b.clk;   // $past and friends in the assertion use its clock
    ConcAssert c;
    c.id = newAssert(AT_CONCURRENT, dir, s);
    if (s.assertionKind == AssertionKind::CoverSequence) {
        c.coverSeq = true;
        c.root = sva::store().covseq(seqTerm(*a));
    } else
        c.root = propTerm(*a, dir == AD_COVER);
    if (dis) c.disable = expr(*dis);   // current values, not sampled (16.12)
    std::string name = asserts()[c.id].name;
    if (!emptyStmt(s.ifTrue)) c.pass = concAction(s.ifTrue, s, name);
    if (!emptyStmt(s.ifFalse)) c.fail = concAction(s.ifFalse, s, name);
    else if (dir != AD_COVER) c.fail = concAction(nullptr, s, name);
    c.ev = b.clk;
    c.clkIff = b.clkIff;
    c.atoms = std::move(b.atoms);
    if (!staticAssert) {   // in a procedure (16.14.6): queued when the procedure reaches it
        c.procedural = true;
        // Automatic variables are read as they were when the instance was
        // queued (16.14.6.1): each becomes a slot, filled per attempt.
        std::unordered_map<int, E> slotOf;
        std::function<E(const E&)> capture = [&](const E& e) -> E {
            if (!e) return e;
            if (e->op == Op::Ref && automaticSigs().count(e->sig)) {
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
        q.join = 3;
        q.hi = (int)concAsserts().size();
        prog.emit(q);
    }
    concAsserts().push_back(std::move(c));
    sampleEv = savedEv;
    svb = saved;
}
