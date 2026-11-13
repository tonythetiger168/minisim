// vpi_param.inc - parameters through VPI (IEEE 1800-2023 37.28): type
// parameters, the default expression of a parameter, param assigns (the
// overrides of an instantiation), def params, and the net declaration
// assignments and connection lists that are expressions built from the
// source (37.11, 37.40).
// #included in vpi.inc, inside namespace vpi.

// An expression of the source bound where it was written (the default of a
// parameter, an override, a connection): bound once.
inline const Expression* boundExpr(const slang::syntax::SyntaxNode* syn, const Scope& sc, LookupLocation loc) {
    // by syntax and scope: each instance body binds its own parameters
    static std::map<std::pair<const slang::syntax::SyntaxNode*, const Scope*>, const Expression*> done;
    if (!syn || !slang::syntax::ExpressionSyntax::isKind(syn->kind)) return nullptr;
    auto it = done.find({syn, &sc});
    if (it != done.end()) return it->second;
    const Expression* e = &Expression::bind(syn->as<slang::syntax::ExpressionSyntax>(), ASTContext(sc, loc));
    if (e->bad()) e = nullptr;
    done[{syn, &sc}] = e;
    return e;
}
inline const Type* boundType(const slang::syntax::SyntaxNode* syn, const Scope& sc, LookupLocation loc) {
    if (!syn || !slang::syntax::DataTypeSyntax::isKind(syn->kind)) return nullptr;
    const Type& t = sc.getCompilation().getType(syn->as<slang::syntax::DataTypeSyntax>(), ASTContext(sc, loc));
    return t.isError() ? nullptr : &t;
}
inline Obj* typeParamObj(const TypeParameterSymbol& p) {
    auto& o = st().symObj[&p];
    if (!o) {
        o = mkObj(Obj::TypeParam, vpiTypeParameter);
        o->sym = &p;
    }
    return o;
}
// A parameter object of either kind.
inline Obj* anyParamObj(const Symbol& s) {
    if (s.kind == SymbolKind::Parameter) return paramObj(&s);
    if (s.kind == SymbolKind::TypeParameter) return typeParamObj(s.as<TypeParameterSymbol>());
    return nullptr;
}
// A parameter's default (37.28 detail 3): an expression, or for a type
// parameter a typespec.
inline Obj* paramDefault(const Obj* o) {
    const Symbol& s = *o->sym;
    const ParameterSymbolBase& b = s.kind == SymbolKind::Parameter ? (const ParameterSymbolBase&)s.as<ParameterSymbol>()
                                                                   : (const ParameterSymbolBase&)s.as<TypeParameterSymbol>();
    const Scope* sc = s.getParentScope();
    if (!sc || !b.defaultValSyntax) return nullptr;
    const slang::syntax::SyntaxNode* syn = b.defaultValSyntax;
    if (syn->kind == slang::syntax::SyntaxKind::Declarator) {   // name = expr
        const auto* init = syn->as<slang::syntax::DeclaratorSyntax>().initializer;
        syn = init ? init->expr.get() : nullptr;
    }
    if (s.kind == SymbolKind::TypeParameter) return typespecObj(boundType(syn, *sc, LookupLocation::before(s)));
    return exprNode(boundExpr(syn, *sc, LookupLocation::before(s)), o);
}

// ---- param assigns: #(...) of an instantiation ----
struct ParamAsg {
    const InstanceSymbol* inst;
    const Symbol* param;
    const slang::syntax::ExpressionSyntax* expr;
    bool byName;
};
inline std::vector<ParamAsg>& paramAsgs() { static std::vector<ParamAsg> v; return v; }
inline Obj* paramAsgObj(size_t k) {
    auto& o = st().codeObjs[{&paramAsgs()[k], 7001}];
    if (!o) {
        o = mkObj(Obj::ParamAssign, vpiParamAssign);
        o->idx = (int)k;
        o->sym = paramAsgs()[k].param;
    }
    return o;
}
// The overrides of instance `inst`, in the order written.
inline void instParamAsgs(const InstanceSymbol& inst, std::vector<vpiHandle>& items) {
    const auto* hi = inst.getSyntax();
    if (!hi || !hi->parent || hi->parent->kind != slang::syntax::SyntaxKind::HierarchyInstantiation) return;
    const auto* pv = hi->parent->as<slang::syntax::HierarchyInstantiationSyntax>().parameters;
    if (!pv) return;
    std::vector<const Symbol*> ports;
    for (const ParameterSymbolBase* p : inst.body.getParameters())
        if (p->isPortParam()) ports.push_back(&p->symbol);
    size_t pos = 0;
    for (const slang::syntax::ParamAssignmentSyntax* a : pv->parameters) {
        const Symbol* target = nullptr;
        const slang::syntax::ExpressionSyntax* ex = nullptr;
        bool named = a->kind == slang::syntax::SyntaxKind::NamedParamAssignment;
        if (named) {
            const auto& n = a->as<slang::syntax::NamedParamAssignmentSyntax>();
            ex = n.expr;
            for (const ParameterSymbolBase* p : inst.body.getParameters())
                if (p->symbol.name == n.name.valueText()) target = &p->symbol;
        } else {
            ex = a->as<slang::syntax::OrderedParamAssignmentSyntax>().expr;
            if (pos < ports.size()) target = ports[pos];
            pos++;
        }
        if (!target || !ex) continue;   // .P() leaves the default
        auto& v = paramAsgs();
        size_t k = 0;
        while (k < v.size() && !(v[k].inst == &inst && v[k].param == target)) k++;
        if (k == v.size()) v.push_back({&inst, target, ex, named});
        items.push_back(H(paramAsgObj(k)));
    }
}

// ---- the connection list of an instance array (37.11 detail 1) ----
inline Obj* listOpObj(const Obj* arr) {
    auto& o = st().codeObjs[{arr->sym, 7002}];
    if (!o) {
        o = mkObj(Obj::ListOp, vpiOperation);
        o->sym = arr->sym;
        o->parent = arr;
    }
    return o;
}
inline const Symbol* iarrLeaf(const InstanceArraySymbol& a);
// The connections of the array's first element, in port order.
inline void listOperands(const Obj* o, std::vector<vpiHandle>& items) {
    const Symbol* e = iarrLeaf(o->sym->as<InstanceArraySymbol>());
    if (!e) return;
    if (e->kind == SymbolKind::PrimitiveInstance) {
        for (const Expression* x : e->as<PrimitiveInstanceSymbol>().getPortConnections())
            if (Obj* c = exprNode(x, o)) items.push_back(H(c));
        return;
    }
    if (e->kind != SymbolKind::Instance) return;
    const auto& inst = e->as<InstanceSymbol>();
    for (const Symbol* p : inst.body.getPortList()) {
        const PortConnection* pc = p->kind == SymbolKind::Port ? inst.getPortConnection(p->as<PortSymbol>())
                                 : p->kind == SymbolKind::InterfacePort ? inst.getPortConnection(p->as<InterfacePortSymbol>()) : nullptr;
        const Expression* x = pc ? pc->getExpression() : nullptr;
        if (Obj* c = x ? exprNode(x, o) : nullptr) items.push_back(H(c));
    }
}

// ---- net declaration assignments (wire w = expr;, 37.40) ----
inline Obj* netDeclAsgObj(const NetSymbol& n) {
    auto& o = st().codeObjs[{&n, 7003}];
    if (!o) {
        o = mkObj(Obj::ContAssign, vpiContAssign);
        o->sym = &n;
    }
    return o;
}

// ---- dispatch ----
inline bool paramIterate(int type, const Obj* o, std::vector<vpiHandle>& items) {
    if (type == vpiParamAssign) {
        if (o->kind == Obj::Module) { instParamAsgs(o->sym->as<InstanceSymbol>(), items); return true; }
        if (o->kind == Obj::InstArr) {
            const Symbol* e = iarrLeaf(o->sym->as<InstanceArraySymbol>());
            if (e && e->kind == SymbolKind::Instance) instParamAsgs(e->as<InstanceSymbol>(), items);
            return true;
        }
        return false;
    }
    if (type == vpiDefParam && (o->kind == Obj::Module || o->kind == Obj::Scope)) {
        const Scope* sc = scopeOf(o);
        if (!sc) return false;
        for (const Symbol& m : sc->members())
            if (m.kind == SymbolKind::DefParam) {
                auto& x = st().symObj[&m];
                if (!x) {
                    x = mkObj(Obj::DefParam, vpiDefParam);
                    x->sym = &m;
                }
                items.push_back(H(x));
            }
        return true;
    }
    if (o->kind == Obj::ListOp) {
        if (type == vpiOperand) listOperands(o, items);
        return true;
    }
    return false;
}
inline bool paramHandle(int type, const Obj* o, Obj*& out) {
    out = nullptr;
    switch (o->kind) {
        case Obj::TypeParam: {
            const auto& p = o->sym->as<TypeParameterSymbol>();
            if (type == vpiTypespec) out = typespecObj(&p.targetType.getType());   // detail 2: as written, aliases kept
            else if (type == vpiExpr) out = paramDefault(o);                      // detail 3: the default typespec
            else if (type == vpiModule || type == vpiInstance) out = moduleObj(instOf(p.getParentScope()));
            else if (type == vpiScope || type == vpiParent) out = scopeOfSym(p);
            return true;
        }
        case Obj::ParamAssign: {
            const ParamAsg& a = paramAsgs()[(size_t)o->idx];
            if (type == vpiLhs) out = anyParamObj(*a.param);   // detail 4: the parameter overridden
            else if (type == vpiRhs) {
                const Scope* sc = a.inst->getParentScope();
                while (sc && sc->asSymbol().kind == SymbolKind::InstanceArray) sc = sc->asSymbol().getParentScope();
                if (a.param->kind == SymbolKind::TypeParameter) out = typespecObj(&a.param->as<TypeParameterSymbol>().targetType.getType());
                else if (sc) out = exprNode(boundExpr(a.expr, *sc, LookupLocation::max), o);
            } else if (type == vpiModule || type == vpiInstance || type == vpiScope || type == vpiParent)
                out = moduleObj(instOf(a.inst->getParentScope()));
            return true;
        }
        case Obj::DefParam: {
            const auto& d = o->sym->as<DefParamSymbol>();
            if (type == vpiLhs) out = d.getTarget() ? anyParamObj(*d.getTarget()) : nullptr;
            else if (type == vpiRhs) out = exprNode(&d.getInitializer(), o);
            else if (type == vpiModule || type == vpiInstance) out = moduleObj(instOf(d.getParentScope()));
            else if (type == vpiScope || type == vpiParent) out = scopeOfSym(d);
            return true;
        }
        case Obj::ListOp:
            if (type == vpiParent) out = const_cast<Obj*>(o->parent);
            return true;
        case Obj::Param:
            if (type == vpiExpr) { out = paramDefault(o); return true; }
            return false;
        default: return false;
    }
}
inline bool paramGet(int prop, const Obj* o, int64_t& r) {
    switch (o->kind) {
        case Obj::TypeParam:
            if (prop == vpiType) r = vpiTypeParameter;
            else if (prop == vpiLocalParam) r = o->sym->as<TypeParameterSymbol>().isLocalParam();
            else if (prop == vpiLineNo) r = srcMgr() ? (int64_t)srcMgr()->getLineNumber(o->sym->location) : 0;
            else r = vpiUndefined;
            return true;
        case Obj::ParamAssign: {
            const ParamAsg& a = paramAsgs()[(size_t)o->idx];
            if (prop == vpiType) r = vpiParamAssign;
            else if (prop == vpiConnByName) r = a.byName;
            else if (prop == vpiLineNo) r = srcMgr() ? (int64_t)srcMgr()->getLineNumber(a.expr->getFirstToken().location()) : 0;
            else r = vpiUndefined;
            return true;
        }
        case Obj::DefParam:
            if (prop == vpiType) r = vpiDefParam;
            else if (prop == vpiLineNo) r = srcMgr() ? (int64_t)srcMgr()->getLineNumber(o->sym->location) : 0;
            else r = vpiUndefined;
            return true;
        case Obj::ListOp:
            if (prop == vpiType) r = vpiOperation;
            else if (prop == vpiOpType) r = vpiListOp;
            else r = vpiUndefined;
            return true;
        default: return false;
    }
}
