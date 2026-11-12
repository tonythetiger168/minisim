// lower_cover.inc - covergroups (IEEE 1800-2023 clause 19), included inside
// struct Lowerer. A covergroup type becomes a CgDef (cover.hpp) the first
// time it is used; `new`, the predefined methods and $get_coverage become
// SF_CG nodes that the kernel runs (cover_rt.inc).

static std::unordered_map<const slang::ast::CovergroupType*, int>& cgIdxMap() {
    static std::unordered_map<const slang::ast::CovergroupType*, int> m;
    return m;
}
int cgItemSig = -1;   // the `item` of a bin's with clause (19.5.1.1)

// A bins value or range bound, as written: conversions to the coverpoint
// type are peeled so that the bound keeps its own value (19.5.7), and it is
// read as 64 bits, sign-extended when signed.
E cgBound(const slang::ast::Expression& x0, bool& sg) {
    using namespace slang::ast;
    const Expression* x = &x0;
    while (x->kind == ExpressionKind::Conversion && x->as<ConversionExpression>().operand().type->isIntegral())
        x = &x->as<ConversionExpression>().operand();
    if (!x->type->isIntegral()) unsupported("a coverage bin value that is not integral (real coverpoints, 19.5.1)");
    sg = x->type->isSigned();
    if (x->type->getBitWidth() > 64) unsupported("a coverage bin value wider than 64 bits");
    return resize(expr(*x), 64, sg, sg);
}
CgItem cgItem(const slang::ast::Expression& x) {
    using namespace slang::ast;
    CgItem it;
    if (x.kind == ExpressionKind::ValueRange) {
        const auto& r = x.as<ValueRangeExpression>();
        if (r.rangeKind != ValueRangeKind::Simple) unsupported("a tolerance range in a bin (real coverpoints)");
        it.range = true;
        auto bound = [&](const Expression& b, bool& unb, bool& sg) -> E {
            const Expression* p = &b;
            while (p->kind == ExpressionKind::Conversion) p = &p->as<ConversionExpression>().operand();
            if (p->kind == ExpressionKind::UnboundedLiteral) { unb = true; return nullptr; }
            return cgBound(b, sg);
        };
        it.lo = bound(r.left(), it.loMin, it.loSgn);
        it.hi = bound(r.right(), it.hiMax, it.hiSgn);
        return it;
    }
    it.lo = cgBound(x, it.loSgn);
    return it;
}
// A bins value or range of a real coverpoint (19.5.1): reals, and
// tolerance ranges.
CgItem cgItemReal(const slang::ast::Expression& x) {
    using namespace slang::ast;
    CgItem it;
    if (x.kind == ExpressionKind::ValueRange) {
        const auto& r = x.as<ValueRangeExpression>();
        it.range = true;
        it.tol = r.rangeKind == ValueRangeKind::AbsoluteTolerance ? 1 : r.rangeKind == ValueRangeKind::RelativeTolerance ? 2 : 0;
        auto bound = [&](const Expression& b, bool& unb) -> E {
            const Expression* p = &b;
            while (p->kind == ExpressionKind::Conversion) p = &p->as<ConversionExpression>().operand();
            if (p->kind == ExpressionKind::UnboundedLiteral) { unb = true; return nullptr; }
            return realOf(b);
        };
        it.lo = bound(r.left(), it.loMin);
        it.hi = bound(r.right(), it.hiMax);
        return it;
    }
    it.lo = realOf(x);
    return it;
}
static int cgOptId(std::string_view n, bool type) {
    if (type) {
        if (n == "weight") return CO_TYPE_WEIGHT;
        if (n == "goal") return CO_TYPE_GOAL;
        if (n == "strobe") return CO_TYPE_STROBE;
        if (n == "merge_instances") return CO_TYPE_MERGE;
        if (n == "distribute_first") return CO_TYPE_DISTRIBUTE_FIRST;
        if (n == "comment") return CO_TYPE_COMMENT;
        return -1;
    }
    if (n == "weight") return CO_WEIGHT;
    if (n == "goal") return CO_GOAL;
    if (n == "at_least") return CO_AT_LEAST;
    if (n == "auto_bin_max") return CO_AUTO_BIN_MAX;
    if (n == "per_instance") return CO_PER_INSTANCE;
    if (n == "get_inst_coverage") return CO_GET_INST_COVERAGE;
    if (n == "cross_num_print_missing") return CO_CROSS_NUM_PRINT_MISSING;
    if (n == "detect_overlap") return CO_DETECT_OVERLAP;
    if (n == "name") return CO_NAME;
    if (n == "comment") return CO_COMMENT;
    return -1;
}
void cgOpts(std::span<const slang::ast::CoverageOptionSetter> in, std::vector<CgOptSet>& out, E* realInterval = nullptr) {
    using namespace slang::ast;
    for (const auto& o : in) {
        if (o.isTypeOption() && o.getName() == "real_interval") {   // a real (19.7.1)
            const Expression& e = o.getExpression();
            const Expression& v = e.kind == ExpressionKind::Assignment ? e.as<AssignmentExpression>().right() : e;
            if (realInterval) *realInterval = realOf(v);
            continue;
        }
        int id = cgOptId(o.getName(), o.isTypeOption());
        if (id < 0) unsupported("this coverage option");
        if (id == CO_NAME || id == CO_COMMENT || id == CO_TYPE_COMMENT) continue;   // strings: reports only
        const Expression& e = o.getExpression();
        const Expression& v = e.kind == ExpressionKind::Assignment ? e.as<AssignmentExpression>().right() : e;
        out.push_back({id, resize(expr(v), 64, v.type->isSigned(), true)});
    }
}
std::shared_ptr<CgSel> cgSel(const slang::ast::BinsSelectExpr& b, const slang::ast::CoverCrossSymbol& x) {
    using namespace slang::ast;
    auto s = std::make_shared<CgSel>();
    switch (b.kind) {
        case BinsSelectExprKind::Condition: {
            const auto& c = b.as<ConditionBinsSelectExpr>();
            const Symbol* cp = &c.target;
            if (cp->kind == SymbolKind::CoverageBin) {
                s->bin = std::string(cp->name);
                cp = &cp->getParentScope()->asSymbol();
            }
            for (size_t k = 0; k < x.targets.size(); k++)
                if (x.targets[k] == cp) s->cp = (int)k;
            if (s->cp < 0) unsupported("binsof of a coverpoint not in the cross");
            bool real = x.targets[s->cp]->getType().isFloating();
            for (const Expression* e : c.intersects) s->isect.push_back(real ? cgItemReal(*e) : cgItem(*e));
            return s;
        }
        case BinsSelectExprKind::Unary:
            s->kind = 1;
            s->kids.push_back(cgSel(b.as<UnaryBinsSelectExpr>().expr, x));
            return s;
        case BinsSelectExprKind::Binary: {
            const auto& bb = b.as<BinaryBinsSelectExpr>();
            s->kind = bb.op == BinaryBinsSelectExpr::And ? 2 : 3;
            s->kids.push_back(cgSel(bb.left, x));
            s->kids.push_back(cgSel(bb.right, x));
            return s;
        }
        default: unsupported("this cross bin select expression (19.6.1)");
    }
}
// The definition of a covergroup type, built on first use.
int cgDefOf(const slang::ast::CovergroupType& t) {
    using namespace slang::ast;
    if (auto it = cgIdxMap().find(&t); it != cgIdxMap().end()) return it->second;
    if (t.getBaseGroup()) unsupported("a covergroup that extends another (19.4.1)");
    auto savedScope = scope;
    auto savedUnit = tsUnit, savedPrec = tsPrec;
    setScope(t.getParentScope());
    CgDef d;
    // A covergroup in a class (19.4): its expressions read the properties
    // of the object it was created for, through `this`, with no frame.
    ClassInfo* savedCls = curClass;
    auto savedFrames = std::move(frames);
    frames.clear();
    const Symbol& ps = t.getParentScope()->asSymbol();
    if (ps.kind == SymbolKind::ClassType) {
        curClass = classInfo(ps.as<ClassType>());
        d.inClass = true;
    }
    d.name = t.getParentScope()->asSymbol().getHierarchicalPath() + "." + std::string(t.name);
    d.prot = hideLocIf() && hideLocIf()(t.location);   // the coverage right (1735)
    auto hidden = [&](const Symbol& s, const Type& ty) {
        int w = storeW(ty);
        if (w <= 0) unsupported("a covergroup argument of this type");
        int id = mkSig(d.name + "$" + std::string(s.name), w, false);
        sigmap[&s] = id;
        return id;
    };
    for (const FormalArgumentSymbol* a : t.getArguments()) {
        d.argSigs.push_back(hidden(*a, a->getType()));
        d.argRef.push_back(a->direction == ArgumentDirection::Ref);
    }
    for (const auto& m : t.members())
        if (const auto* a = m.as_if<FormalArgumentSymbol>(); a && (a->flags & VariableFlags::CoverageSampleFormal))
            d.sampleSigs.push_back(hidden(*a, a->getType()));
    if (const TimingControl* ev = t.getCoverageEvent()) {
        if (ev->kind != TimingControlKind::SignalEvent && ev->kind != TimingControlKind::EventList)
            unsupported("this covergroup sampling event (block events, 19.3)");
        clockOf(*ev, d.ev, d.evIff);
    }
    const auto& body = t.getBody();
    cgOpts(body.options, d.opts, &d.realInterval);
    std::unordered_map<const CoverpointSymbol*, int> cpIdx;
    for (const auto& m : body.members()) {
        const auto* cp = m.as_if<CoverpointSymbol>();
        if (!cp) continue;
        CgCpDef c;
        c.name = std::string(cp->name);
        const Type& ty = cp->getType().getCanonicalType();
        const Expression& ce = cp->getCoverageExpr();
        // What lowering emits for an expression (a container's slot through a
        // handle: Assign tmp = Addr(h)) runs before it at each sample.
        // Anything else (a function or method call) is lowered into a program
        // of its own, run to its end before the expression is read.
        Prog outer;
        auto beginPre = [&]() { outer = std::move(prog); prog = Prog{}; return (size_t)0; };
        auto takePre = [&](size_t, std::vector<std::pair<int, E>>& out, std::shared_ptr<Prog>& code) {
            bool simple = true;
            for (const Instr& i : prog.code)
                if (i.k != IK::Assign || i.hi >= 0 || i.bidx || i.sig < 0) simple = false;
            if (simple) for (const Instr& i : prog.code) out.push_back({i.sig, i.e});
            else code = std::make_shared<Prog>(std::move(prog));
            prog = std::move(outer);
        };
        size_t at0 = beginPre();
        if (ty.isFloating()) {   // a real coverpoint (19.5)
            c.isReal = true;
            c.w = 64;
            c.expr = realOf(ce);
        } else {
            if (!ty.isIntegral()) unsupported("a coverpoint of this type (19.5)");
            c.w = (int)ty.getBitWidth();
            if (c.w > 64) unsupported("a coverpoint wider than 64 bits");
            c.sgn = ty.isSigned();
            c.expr = resize(expr(ce), c.w, ce.type->isSigned(), c.sgn);
        }
        takePre(at0, c.pre, c.preProg);
        if (const Expression* iff = cp->getIffExpr()) {
            size_t at1 = beginPre();
            c.iff = expr(*iff);
            takePre(at1, c.preIff, c.preIffProg);
        }
        const Expression* pe = &ce;   // the coverpoint expression under its conversion to the coverpoint type
        while (pe->kind == ExpressionKind::Conversion) pe = &pe->as<ConversionExpression>().operand();
        const Type& et = ty.isEnum() ? ty : pe->type->getCanonicalType();
        if (!c.isReal && et.isEnum() && (int)et.getBitWidth() == c.w) {
            c.isEnum = true;
            for (const auto& ev : et.as<EnumType>().values()) {
                const auto& cv = ev.getValue();
                if (!cv.isInteger() || cv.integer().hasUnknown()) continue;
                auto v = cv.integer().as<int64_t>();
                c.enumVals.push_back({std::string(ev.name), v ? (uint64_t)*v : 0});
            }
        }
        cgOpts(cp->options, c.opts, &c.realInterval);
        for (const auto& bm : cp->members()) {
            const auto* b = bm.as_if<CoverageBinSymbol>();
            if (!b) continue;
            CgBinDef bd;
            bd.name = std::string(b->name);
            bd.kind = b->binsKind == CoverageBinSymbol::IgnoreBins ? 1 : b->binsKind == CoverageBinSymbol::IllegalBins ? 2 : 0;
            bd.array = b->isArray;
            if (const Expression* n = b->getNumberOfBinsExpr()) bd.count = resize(expr(*n), 64, n->type->isSigned(), true);
            bd.wildcard = b->isWildcard;
            bd.dflt = b->isDefault;
            bd.dfltSeq = b->isDefaultSequence;
            if (const Expression* iff = b->getIffExpr()) bd.iff = expr(*iff);
            if (b->getSetCoverageExpr()) unsupported("a bin set expression (19.5.1.2)");
            for (const Expression* v : b->getValues()) bd.items.push_back(c.isReal ? cgItemReal(*v) : cgItem(*v));
            if (c.isReal) {   // only value bins (19.5)
                if (!b->getTransList().empty()) unsupported("a transition bin of a real coverpoint (19.5)");
                if (b->isWildcard || b->getWithExpr()) unsupported("a wildcard or with bin of a real coverpoint (19.5)");
                if (b->isDefault && b->isArray) unsupported("an array of default bins of a real coverpoint (19.5)");
            }
            for (const auto& ts : b->getTransList()) {
                std::vector<CgStep> seq;
                for (const auto& r : ts) {
                    CgStep st;
                    for (const Expression* v : r.items) st.items.push_back(cgItem(*v));
                    st.rep = r.repeatKind == CoverageBinSymbol::TransRangeList::Consecutive ? 1
                           : r.repeatKind == CoverageBinSymbol::TransRangeList::GoTo ? 2
                           : r.repeatKind == CoverageBinSymbol::TransRangeList::Nonconsecutive ? 3 : 0;
                    if (r.repeatFrom) st.from = resize(expr(*r.repeatFrom), 64, false, false);
                    if (r.repeatTo) st.to = resize(expr(*r.repeatTo), 64, false, false);
                    seq.push_back(std::move(st));
                }
                bd.trans.push_back(std::move(seq));
            }
            if (const Expression* w = b->getWithExpr()) {
                bd.itemSig = mkSig(d.name + "." + c.name + "$item", c.w, false);
                cgItemSig = bd.itemSig;
                bd.with = expr(*w);
                cgItemSig = -1;
                if (bd.items.empty()) bd.allValues = true;   // bins b[] = cp with (...)
            }
            c.bins.push_back(std::move(bd));
        }
        if (c.isReal && std::none_of(c.bins.begin(), c.bins.end(), [](const CgBinDef& b) { return b.kind == 0; }))
            unsupported("a real coverpoint without bins: there are no automatic bins for reals (19.5.3)");
        cpIdx[cp] = (int)d.cps.size();
        d.cps.push_back(std::move(c));
    }
    for (const auto& m : body.members()) {
        const auto* x = m.as_if<CoverCrossSymbol>();
        if (!x) continue;
        CgCrossDef cd;
        cd.name = std::string(x->name);
        for (const CoverpointSymbol* cp : x->targets) {
            auto it = cpIdx.find(cp);
            if (it == cpIdx.end()) unsupported("a cross of this coverpoint");
            cd.cps.push_back(it->second);
        }
        if (const Expression* iff = x->getIffExpr()) cd.iff = expr(*iff);
        cgOpts(x->options, cd.opts);
        std::function<void(const Scope&)> bins = [&](const Scope& sc) {
            for (const auto& bm : sc.members()) {
                if (const auto* b = bm.as_if<CoverageBinSymbol>()) {
                    CgCrossBinDef xb;
                    xb.name = std::string(b->name);
                    xb.kind = b->binsKind == CoverageBinSymbol::IgnoreBins ? 1 : b->binsKind == CoverageBinSymbol::IllegalBins ? 2 : 0;
                    const BinsSelectExpr* sel = b->getCrossSelectExpr();
                    if (!sel) unsupported("this cross bin");
                    xb.sel = cgSel(*sel, *x);
                    cd.bins.push_back(std::move(xb));
                } else if (const auto* body = bm.as_if<CoverCrossBodySymbol>())
                    bins(*body);
            }
        };
        bins(*x);
        d.crosses.push_back(std::move(cd));
    }
    cgDefs().push_back(std::move(d));
    int idx = (int)cgDefs().size() - 1;
    cgIdxMap()[&t] = idx;
    curClass = savedCls;
    frames = std::move(savedFrames);
    scope = savedScope;
    tsUnit = savedUnit; tsPrec = savedPrec;
    return idx;
}
// new(args) of a covergroup (19.3): the instance's handle. A ref argument
// passes the signal it names; the others pass their values.
E cgNew(const slang::ast::Expression& e, std::span<const slang::ast::Expression* const> args) {
    NoCondCov off(condCovOff);
    using namespace slang::ast;
    const auto& t = e.type->getCanonicalType().as<CovergroupType>();
    int d = cgDefOf(t);
    std::vector<E> ks{konst(mk((uint64_t)d, 32))};
    auto formals = t.getArguments();
    for (size_t k = 0; k < formals.size(); k++) {
        if (k >= args.size()) unsupported("a covergroup new() without every argument");
        const Expression* a = args[k];
        if (cgDefs()[d].argRef[k]) {
            const Expression* p = a;
            while (p->kind == ExpressionKind::Conversion) p = &p->as<ConversionExpression>().operand();
            int s = ValueExpressionBase::isKind(p->kind) ? sigFind(&p->as<ValueExpressionBase>().symbol) : -1;
            if (s < 0 || isRel(s)) unsupported("a covergroup ref argument that is not a static variable");
            ks.push_back(konst(mk((uint64_t)s, 32)));
        } else
            ks.push_back(resize(expr(*a), sigInfo()[cgDefs()[d].argSigs[k]].second, a->type->isSigned(), false));
    }
    if (cgDefs()[d].inClass) {
        if (!curClass) unsupported("new() of a class's covergroup outside its class");
        ks.push_back(node(Op::This));
    }
    return sysFnNode(SF_CG, std::move(ks), CG_NEW);
}
// A predefined covergroup, coverpoint or cross method (19.8), or null.
E cgCall(const slang::ast::CallExpression& call) {
    using namespace slang::ast;
    const auto* const* sp = std::get_if<const SubroutineSymbol*>(&call.subroutine);
    if (!sp || !*sp) return nullptr;
    const SubroutineSymbol& sub = **sp;
    const Symbol& owner = sub.getParentScope()->asSymbol();
    if (owner.kind != SymbolKind::CovergroupBody && owner.kind != SymbolKind::Coverpoint &&
        owner.kind != SymbolKind::CoverCross)
        return nullptr;
    std::string_view n = sub.name;
    const Expression* self = call.thisClass();
    // The covergroup type, the item (coverpoint or cross) if any, and the handle.
    const Symbol* g = &owner;
    while (g->kind != SymbolKind::CovergroupType) g = &g->getParentScope()->asSymbol();
    int d = cgDefOf(g->as<CovergroupType>());
    int item = -1;
    if (owner.kind != SymbolKind::CovergroupBody) {
        const CgDef& def = cgDefs()[d];
        for (size_t k = 0; k < def.cps.size(); k++)
            if (def.cps[k].name == owner.name && owner.kind == SymbolKind::Coverpoint) item = (int)k;
        for (size_t k = 0; k < def.crosses.size(); k++)
            if (def.crosses[k].name == owner.name && owner.kind == SymbolKind::CoverCross) item = (int)(def.cps.size() + k);
        if (item < 0) unsupported("a method of this coverage item");
        if (self && self->kind == ExpressionKind::MemberAccess) self = &self->as<MemberAccessExpression>().value();
        else self = nullptr;
    }
    E h = self ? expr(*self) : konst(mk(0, 32));
    E dk = konst(mk((uint64_t)d, 32));
    if (n == "sample") {
        if (item >= 0) return nullptr;
        std::vector<E> ks{h};
        auto args = call.arguments();
        const auto& ss = cgDefs()[d].sampleSigs;
        for (size_t k = 0; k < ss.size() && k < args.size(); k++)
            ks.push_back(resize(expr(*args[k]), sigInfo()[ss[k]].second, args[k]->type->isSigned(), false));
        return sysFnNode(SF_CG, std::move(ks), CG_SAMPLE);
    }
    if (n == "get_coverage" || n == "get_inst_coverage") {
        bool inst = n == "get_inst_coverage";
        if (inst && !self) unsupported("get_inst_coverage without an instance");
        auto args = call.arguments();
        if (args.size() == 2 && args[0]->kind != ExpressionKind::IntegerLiteral) {
            // get_coverage(covered, total): the counts are written first (19.8).
            for (int which = 0; which < 2; which++) {
                const Expression* a = args[which];
                if (a->kind == ExpressionKind::Assignment) a = &a->as<AssignmentExpression>().left();
                Instr i;
                i.k = IK::Assign;
                lhs(*a, i);
                i.e = resize(sysFnNode(SF_CG, {inst ? h : dk, konst(mk((uint64_t)(int64_t)item, 32)),
                                               konst(mk(inst ? 1 : 0, 32)), konst(mk((uint64_t)which, 32))}, CG_COUNT),
                             (int)a->type->getBitWidth(), false, true);
                prog.emit(i);
            }
        }
        if (item < 0) return sysFnNode(SF_CG, {inst ? h : dk}, inst ? CG_INSTCOV : CG_GETCOV);
        return sysFnNode(SF_CG, {inst ? h : dk, konst(mk((uint64_t)item, 32))}, inst ? CG_ITEMINST : CG_ITEMCOV);
    }
    if (n == "start" || n == "stop") return sysFnNode(SF_CG, {h}, n == "start" ? CG_START : CG_STOP);
    if (n == "set_inst_name") return sysFnNode(SF_CG, {h}, CG_SETNAME);
    return nullptr;
}
