// FSM extraction for FSM coverage (msim -cov), in the manner of VCS -cm fsm.
//
// A state register is a variable V that one clocked procedure assigns, whole,
// and whose next value is always a constant, V itself, or a next-state
// variable N that one combinational procedure assigns the same way; and
// somewhere that logic branches on V (case (V), or if (V == S)).
//
// The procedure bodies are run symbolically: for every value of V (and for
// "any other value") the set of values V can take next. That gives the
// states (every constant compared with or assigned to V) and the transitions
// S -> T. A constant assigned to V under an if, outside any branch on V, is
// taken as a reset: its transitions are marked so mcov can filter them
// (VCS -cm_fsmresetfilter).
//
// Anything else (a state computed by arithmetic, a one-hot register tested
// bit by bit, a register written by a task or a part select) is not an FSM
// here, and is left to line, branch and toggle coverage.
struct FsmExtractor {
    using Sym = const slang::ast::Symbol*;
    struct AV {   // an abstract next value
        int kind = 3;   // 0 constant, 1 hold (V itself), 2 the variable var, 3 unknown
        uint64_t c = 0;
        Sym var = nullptr;
        bool operator<(const AV& o) const { return std::tie(kind, c, var) < std::tie(o.kind, o.c, o.var); }
    };
    using Set = std::map<AV, bool>;   // value -> only ever assigned by a reset branch
    static void add(Set& s, const AV& v, bool reset) {
        auto [it, fresh] = s.try_emplace(v, reset);
        if (!fresh) it->second = it->second && reset;
    }
    static Set join(Set a, const Set& b) {
        for (auto& [v, f] : b) add(a, v, f);
        return a;
    }
    struct Map {   // per value of V: what the target can be afterwards
        std::map<uint64_t, Set> spec;
        Set any;   // every value of V without an entry in spec
        const Set& lookup(uint64_t k) const {
            auto it = spec.find(k);
            return it == spec.end() ? any : it->second;
        }
    };
    static Map unite(const Map& a, const Map& b) {
        Map r;
        for (auto& [k, s] : a.spec) r.spec[k] = join(s, b.lookup(k));
        for (auto& [k, s] : b.spec)
            if (!a.spec.count(k)) r.spec[k] = join(s, a.any);
        r.any = join(a.any, b.any);
        return r;
    }

    // The variables an assignment's left side writes. whole: `v = ...`;
    // otherwise a select, a member or a concatenation.
    static void lhsRoots(const slang::ast::Expression& e0, bool whole, std::vector<std::pair<Sym, bool>>& out) {
        using namespace slang::ast;
        const Expression& e = Lowerer::peel(e0);
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
                out.push_back({&e.as<ValueExpressionBase>().symbol, whole && e.kind == ExpressionKind::NamedValue});
                break;
            case ExpressionKind::ElementSelect: lhsRoots(e.as<ElementSelectExpression>().value(), false, out); break;
            case ExpressionKind::RangeSelect: lhsRoots(e.as<RangeSelectExpression>().value(), false, out); break;
            case ExpressionKind::MemberAccess: lhsRoots(e.as<MemberAccessExpression>().value(), false, out); break;
            case ExpressionKind::Concatenation:
                for (auto* op : e.as<ConcatenationExpression>().operands()) lhsRoots(*op, false, out);
                break;
            case ExpressionKind::Streaming:
                for (auto& st : e.as<StreamingConcatenationExpression>().streams()) lhsRoots(*st.operand, false, out);
                break;
            default: break;
        }
    }
    struct Writes : slang::ast::ASTVisitor<Writes, slang::ast::VisitFlags::Statements | slang::ast::VisitFlags::Expressions> {
        std::vector<std::pair<Sym, bool>> roots;
        void handle(const slang::ast::AssignmentExpression& a) {
            lhsRoots(a.left(), true, roots);
            visitDefault(a);
        }
    };
    template <typename Node>
    static bool writes(const Node& n, Sym t) {
        Writes w;
        n.visit(w);
        for (auto& [s, whole] : w.roots)
            if (s == t) return true;
        return false;
    }

    struct Block {
        const slang::ast::ProceduralBlockSymbol* p;
        bool clocked;
    };
    struct WriteInfo {
        std::set<int> blocks;
        bool partial = false, other = false;   // a part of it is written; written outside a procedure
    };
    std::vector<Block> blocks;
    std::unordered_map<Sym, WriteInfo> written;
    std::vector<Sym> order;   // candidates, in source order

    static bool edgeControl(const slang::ast::TimingControl& t) {
        using namespace slang::ast;
        if (t.kind == TimingControlKind::SignalEvent) return t.as<SignalEventControl>().edge != EdgeKind::None;
        if (t.kind == TimingControlKind::EventList) {
            for (auto* ev : t.as<EventListControl>().events)
                if (!edgeControl(*ev)) return false;
            return true;
        }
        return false;
    }
    struct Collect : slang::ast::ASTVisitor<Collect> {
        FsmExtractor& x;
        explicit Collect(FsmExtractor& fx) : x(fx) {}
        void handle(const slang::ast::InstanceSymbol& inst) {
            if (!enterInstance(inst)) return;
            visitDefault(inst);
        }
        void handle(const slang::ast::GenerateBlockSymbol& g) {
            if (!g.isUninstantiated) visitDefault(g);
        }
        void handle(const slang::ast::ClassType&) {}
        void handle(const slang::ast::CovergroupType&) {}
        void handle(const slang::ast::SubroutineSymbol&) {}
        void handle(const slang::ast::ContinuousAssignSymbol& ca) {
            Writes w;
            ca.getAssignment().visit(w);
            for (auto& [s, whole] : w.roots) x.written[s].other = true;
        }
        void handle(const slang::ast::ProceduralBlockSymbol& p) {
            using namespace slang::ast;
            if (p.isFromAssertion) return;
            Writes w;
            p.getBody().visit(w);
            if (p.procedureKind == ProceduralBlockKind::Initial || p.procedureKind == ProceduralBlockKind::Final)
                return;   // a testbench preset or a variable initializer does not make or break an FSM
            bool clocked = false;
            if (p.procedureKind == ProceduralBlockKind::Always || p.procedureKind == ProceduralBlockKind::AlwaysFF)
                if (p.getBody().kind == StatementKind::Timed) clocked = edgeControl(p.getBody().as<TimedStatement>().timing);
            int id = (int)x.blocks.size();
            x.blocks.push_back({&p, clocked});
            for (auto& [s, whole] : w.roots) {
                auto [it, fresh] = x.written.try_emplace(s);
                if (fresh) x.order.push_back(s);
                it->second.blocks.insert(id);
                if (!whole) it->second.partial = true;
            }
        }
    };

    // Runs a procedure body for the target T (V, or its next-state variable).
    struct Interp {
        Sym T, V;
        int w;
        const slang::ast::Scope* scope;
        bool resets;   // T is V in its clocked procedure: a constant under an if is a reset
        std::map<uint64_t, std::string>& names;
        int ifDepth = 0, splitDepth = 0;
        bool split = false, bad = false;

        static Sym symOf(const slang::ast::Expression& e) {
            const auto& x = Lowerer::peel(e);
            return x.kind == slang::ast::ExpressionKind::NamedValue ? &x.as<slang::ast::NamedValueExpression>().symbol : nullptr;
        }
        std::optional<uint64_t> constOf(const slang::ast::Expression& e) {
            using namespace slang::ast;
            const Expression& x = Lowerer::peel(e);
            if (Sym s = symOf(x); s && s->kind != SymbolKind::EnumValue && s->kind != SymbolKind::Parameter) return std::nullopt;
            slang::ConstantValue cv;
            if (const auto* c = x.getConstant()) cv = *c;
            else {
                EvalContext ec(ASTContext(*scope, LookupLocation::max));
                cv = x.eval(ec);
            }
            if (!cv.isInteger() || cv.integer().hasUnknown()) return std::nullopt;
            slang::SVInt v = cv.integer().resize((slang::bitwidth_t)w);
            v.setSigned(false);
            auto u = v.as<uint64_t>();
            if (!u) return std::nullopt;
            if (Sym s = symOf(x)) names.try_emplace(*u, std::string(s->name));
            return *u;
        }
        Set rhs(const slang::ast::Expression& e) {
            using namespace slang::ast;
            const Expression& x = Lowerer::peel(e);
            if (auto c = constOf(x)) return {{AV{0, *c, nullptr}, false}};
            if (Sym s = symOf(x)) {
                if (s == V) return {{AV{1, 0, nullptr}, false}};
                if (T == V && s->kind == SymbolKind::Variable) return {{AV{2, 0, s}, false}};
            }
            if (x.kind == ExpressionKind::ConditionalOp) {
                const auto& c = x.as<ConditionalExpression>();
                return join(rhs(c.left()), rhs(c.right()));
            }
            return {{AV{}, false}};
        }
        void assign(Map& m, Set v) {
            if (resets && ifDepth > 0 && splitDepth == 0)
                for (auto& [av, f] : v) f = av.kind == 0;
            for (auto& [k, s] : m.spec) s = v;
            m.any = std::move(v);
        }
        // c is V == K or V != K.
        bool stateCmp(const slang::ast::Expression& c, uint64_t& k, bool& neq) {
            using namespace slang::ast;
            const Expression& x = Lowerer::peel(c);
            if (x.kind != ExpressionKind::BinaryOp) return false;
            const auto& b = x.as<BinaryExpression>();
            bool eq = b.op == BinaryOperator::Equality || b.op == BinaryOperator::CaseEquality;
            neq = b.op == BinaryOperator::Inequality || b.op == BinaryOperator::CaseInequality;
            if (!eq && !neq) return false;
            const Expression* other = symOf(b.left()) == V ? &b.right() : symOf(b.right()) == V ? &b.left() : nullptr;
            if (!other) return false;
            auto cv = constOf(*other);
            if (!cv) return false;
            k = *cv;
            return true;
        }
        Map run(const slang::ast::Statement& s, Map in) {
            using namespace slang::ast;
            switch (s.kind) {
                case StatementKind::Empty: return in;
                case StatementKind::List:
                    for (auto* st : s.as<StatementList>().list) in = run(*st, std::move(in));
                    return in;
                case StatementKind::Block:
                    if (s.as<BlockStatement>().blockKind != StatementBlockKind::Sequential) break;
                    return run(s.as<BlockStatement>().body, std::move(in));
                case StatementKind::Timed: return run(s.as<TimedStatement>().stmt, std::move(in));
                case StatementKind::ExpressionStatement: {
                    const Expression& e = s.as<ExpressionStatement>().expr;
                    if (e.kind == ExpressionKind::Assignment) {
                        const auto& a = e.as<AssignmentExpression>();
                        if (symOf(a.left()) == T && Lowerer::peel(a.left()).kind == ExpressionKind::NamedValue) {
                            assign(in, a.isCompound() || writes(a.right(), T) ? Set{{AV{}, false}} : rhs(a.right()));
                            return in;
                        }
                    }
                    break;
                }
                case StatementKind::Conditional: {
                    const auto& c = s.as<ConditionalStatement>();
                    if (c.conditions.size() != 1 || c.conditions[0].pattern) break;
                    if (writes(*c.conditions[0].expr, T)) break;
                    uint64_t k = 0;
                    bool neq = false;
                    if (stateCmp(*c.conditions[0].expr, k, neq)) {
                        split = true;
                        in.spec.try_emplace(k, in.lookup(k));
                        splitDepth++;
                        Map t = run(c.ifTrue, in), f = c.ifFalse ? run(*c.ifFalse, in) : in;
                        splitDepth--;
                        if (neq) std::swap(t, f);
                        f.spec[k] = t.spec[k];
                        return f;
                    }
                    ifDepth++;
                    Map t = run(c.ifTrue, in);
                    Map f = c.ifFalse ? run(*c.ifFalse, in) : in;
                    ifDepth--;
                    return unite(t, f);
                }
                case StatementKind::Case: {
                    const auto& c = s.as<CaseStatement>();
                    if (writes(c.expr, T)) break;
                    if (symOf(c.expr) != V || c.condition == CaseStatementCondition::Inside) {
                        ifDepth++;
                        Map out = c.defaultCase ? run(*c.defaultCase, in) : in;
                        for (auto& item : c.items) out = unite(out, run(*item.stmt, in));
                        ifDepth--;
                        return out;
                    }
                    split = true;
                    std::set<uint64_t> covered;
                    Map out;
                    splitDepth++;
                    for (auto& item : c.items) {
                        std::vector<uint64_t> ks;
                        for (const Expression* x : item.expressions) {
                            auto k = constOf(*x);
                            if (!k) { bad = true; splitDepth--; return in; }   // a wildcard or computed label
                            ks.push_back(*k);
                        }
                        Map in2 = in;
                        for (uint64_t k : ks) in2.spec.try_emplace(k, in.lookup(k));
                        Map r = run(*item.stmt, std::move(in2));
                        for (uint64_t k : ks)
                            if (covered.insert(k).second) out.spec[k] = r.spec[k];   // the first matching item runs
                    }
                    Map d = c.defaultCase ? run(*c.defaultCase, in) : in;
                    splitDepth--;
                    for (auto& [k, set] : d.spec)
                        if (!covered.count(k)) out.spec[k] = set;
                    out.any = d.any;
                    return out;
                }
                case StatementKind::ForLoop: return unite(in, run(s.as<ForLoopStatement>().body, in));
                case StatementKind::WhileLoop: return unite(in, run(s.as<WhileLoopStatement>().body, in));
                case StatementKind::DoWhileLoop: return unite(in, run(s.as<DoWhileLoopStatement>().body, in));
                case StatementKind::RepeatLoop: return unite(in, run(s.as<RepeatLoopStatement>().body, in));
                case StatementKind::ForeachLoop: return unite(in, run(s.as<ForeachLoopStatement>().body, in));
                case StatementKind::ForeverLoop: return unite(in, run(s.as<ForeverLoopStatement>().body, in));
                default: break;
            }
            if (writes(s, T)) assign(in, Set{{AV{}, false}});   // written in a way not followed here
            return in;
        }
    };

    static bool known(const Set& s) {
        for (auto& [v, f] : s)
            if (v.kind >= 2) return false;
        return true;
    }

    void run(const slang::ast::RootSymbol& root, const std::unordered_map<Sym, int>& sigmap,
             const slang::SourceManager* sm) {
        using namespace slang::ast;
        Collect col(*this);
        root.visit(col);
        for (Sym vs : order) {
            const WriteInfo& wi = written[vs];
            if (wi.partial || wi.other || wi.blocks.size() != 1 || !blocks[*wi.blocks.begin()].clocked) continue;
            if (vs->kind != SymbolKind::Variable) continue;
            const Type& ty = vs->as<VariableSymbol>().getType();
            if (!ty.isIntegral() || ty.getBitWidth() < 1 || ty.getBitWidth() > 64) continue;
            auto sit = sigmap.find(vs);
            if (sit == sigmap.end()) continue;
            int w = (int)ty.getBitWidth();
            std::map<uint64_t, std::string> names;
            const ProceduralBlockSymbol& pb = *blocks[*wi.blocks.begin()].p;
            Interp iv{vs, vs, w, pb.getParentScope(), true, names};
            Map start;
            start.any = {{AV{1, 0, nullptr}, false}};
            Map mv = iv.run(pb.getBody(), start);
            if (iv.bad) continue;
            // The next-state variable, if V is loaded from one.
            Sym nx = nullptr;
            bool ok = true;
            auto scan = [&](const Set& s) {
                for (auto& [v, f] : s) {
                    if (v.kind == 3) ok = false;
                    if (v.kind == 2) { if (nx && nx != v.var) ok = false; nx = v.var; }
                }
            };
            scan(mv.any);
            for (auto& [k, s] : mv.spec) scan(s);
            if (!ok) continue;
            bool split = iv.split;
            Map fin = mv;
            if (nx) {
                auto wn = written.find(nx);
                if (wn == written.end() || wn->second.partial || wn->second.other || wn->second.blocks.size() != 1) continue;
                const Block& nb = blocks[*wn->second.blocks.begin()];
                if (nb.clocked) continue;
                Interp in{nx, vs, w, nb.p->getParentScope(), false, names};
                Map mn = in.run(nb.p->getBody(), start);
                if (in.bad) continue;
                split = split || in.split;
                auto subst = [&](const Set& sv, const Set& sn) {
                    Set r;
                    for (auto& [v, f] : sv) {
                        if (v.kind != 2) { add(r, v, f); continue; }
                        for (auto& [n, fn] : sn) add(r, n, fn);
                    }
                    return r;
                };
                fin = Map{};
                for (auto& [k, s] : mv.spec) fin.spec[k] = subst(s, mn.lookup(k));
                for (auto& [k, s] : mn.spec)
                    if (!mv.spec.count(k)) fin.spec[k] = subst(mv.any, s);
                fin.any = subst(mv.any, mn.any);
            }
            if (!split || !known(fin.any)) continue;
            for (auto& [k, s] : fin.spec) ok = ok && known(s);
            if (!ok) continue;
            std::set<uint64_t> states;
            for (auto& [k, s] : fin.spec) {
                states.insert(k);
                for (auto& [v, f] : s) if (v.kind == 0) states.insert(v.c);
            }
            for (auto& [v, f] : fin.any) if (v.kind == 0) states.insert(v.c);
            if (states.size() < 2 || states.size() > 1024) continue;
            if (ty.getCanonicalType().kind == SymbolKind::EnumType)
                for (const auto& ev : ty.getCanonicalType().as<EnumType>().values()) {
                    const auto& cv = ev.getValue();
                    if (!cv.isInteger() || cv.integer().hasUnknown()) continue;
                    slang::SVInt v = cv.integer().resize((slang::bitwidth_t)w);
                    v.setSigned(false);
                    if (auto u = v.as<uint64_t>()) names[*u] = std::string(ev.name);
                }
            FsmDef d;
            d.name = std::string(vs->name);
            std::string path = vs->getHierarchicalPath();
            d.inst = path.size() > d.name.size() + 1 ? path.substr(0, path.size() - d.name.size() - 1) : path;
            if (const auto* body = vs->getParentScope()->getContainingInstance()) d.module = std::string(body->getDefinition().name);
            if (sm && vs->location.valid()) {
                auto ol = sm->getFullyOriginalLoc(vs->location);
                d.file = covFileId(std::string(sm->getFullPath(ol.buffer()).string()));
                d.line = (uint32_t)sm->getLineNumber(ol);
            }
            d.sig = sit->second;
            d.w = w;
            for (uint64_t st : states) {
                auto nm = names.find(st);
                char b[40];
                std::snprintf(b, sizeof b, "%d'h%llx", w, (unsigned long long)st);
                d.states.push_back({st, nm == names.end() ? std::string(b) : nm->second});
            }
            for (uint64_t st : states)
                for (auto& [v, f] : fin.lookup(st))
                    if (v.kind == 0 && v.c != st) d.trans.push_back({st, v.c, f});
            fsmDefs().push_back(std::move(d));
        }
    }
};
