// vpi_constr.inc - constraint blocks through VPI (IEEE 1800-2023 37.31,
// 37.32): the constraints of a class definition and their items.
// #included in vpi.inc, inside namespace vpi, after vpi_code.inc.

inline Obj* constraintObj(const ConstraintBlockSymbol& c) {
    auto& slot = st().symObj[&c];
    if (!slot) {
        slot = mkObj(Obj::Constraint, vpiConstraint);
        slot->sym = &c;
    }
    return slot;
}
// The items of a constraint list, flattened ({a; {b; c}} is a, b, c).
inline void constrItems(const Constraint& c, std::vector<const Constraint*>& out) {
    if (c.kind == ConstraintKind::List) {
        for (const Constraint* k : c.as<ConstraintList>().list) constrItems(*k, out);
        return;
    }
    if (c.kind != ConstraintKind::Invalid) out.push_back(&c);
}
inline Obj* constrItemObj(const Constraint& c, const Obj* parent) {
    if (c.kind == ConstraintKind::Expression) return exprNode(&c.as<ExpressionConstraint>().expr, parent);
    int type = 0;
    switch (c.kind) {
        case ConstraintKind::Implication: type = vpiImplication; break;
        case ConstraintKind::Conditional: type = c.as<ConditionalConstraint>().elseBody ? vpiConstrIfElse : vpiConstrIf; break;
        case ConstraintKind::Foreach: type = vpiConstrForEach; break;
        case ConstraintKind::DisableSoft: type = vpiSoftDisable; break;
        case ConstraintKind::SolveBefore: type = vpiConstraintOrdering; break;
        default: return nullptr;   // unique {...}: no object in the model
    }
    auto& slot = st().codeObjs[{&c, vpiConstraintItem}];
    if (!slot) {
        slot = mkObj(Obj::ConstrItem, type);
        slot->node = &c;
        slot->sym = parent->sym;
        slot->parent = parent;
    }
    return slot;
}
inline void pushItems(const Constraint& c, const Obj* parent, std::vector<vpiHandle>& items) {
    std::vector<const Constraint*> v;
    constrItems(c, v);
    for (const Constraint* k : v)
        if (Obj* x = constrItemObj(*k, parent)) items.push_back(H(x));
}
inline bool constrIterate(int type, const Obj* o, std::vector<vpiHandle>& items) {
    if (o->kind == Obj::ClassDefn && type == vpiConstraint) {
        for (const auto& c : o->sym->as<ClassType>().membersOfType<ConstraintBlockSymbol>())
            items.push_back(H(constraintObj(c)));
        return true;
    }
    if (o->kind == Obj::Constraint) {
        if (type != vpiConstraintItem) return false;
        const auto& cb = o->sym->as<ConstraintBlockSymbol>();
        if (!(cb.flags & ConstraintBlockFlags::Pure)) pushItems(cb.getConstraints(), o, items);
        return true;
    }
    if (o->kind != Obj::ConstrItem) return false;
    const Constraint& c = *static_cast<const Constraint*>(o->node);
    switch (c.kind) {
        case ConstraintKind::Implication:
            if (type == vpiConstraintExpr) pushItems(c.as<ImplicationConstraint>().body, o, items);
            return true;
        case ConstraintKind::Conditional: {
            const auto& k = c.as<ConditionalConstraint>();
            if (type == vpiConstraintExpr) pushItems(k.ifBody, o, items);
            if (type == vpiElseConst && k.elseBody) pushItems(*k.elseBody, o, items);
            return true;
        }
        case ConstraintKind::Foreach:
            if (type == vpiConstraintExpr) pushItems(c.as<ForeachConstraint>().body, o, items);
            if (type == vpiLoopVars)
                for (const auto& d : c.as<ForeachConstraint>().loopDims)
                    if (d.loopVar)
                        if (Obj* v = valueObj(d.loopVar)) items.push_back(H(v));
            return true;
        case ConstraintKind::SolveBefore: {
            const auto& k = c.as<SolveBeforeConstraint>();
            if (type == vpiSolveBefore)
                for (const Expression* e : k.solve) if (Obj* x = exprNode(e, o)) items.push_back(H(x));
            if (type == vpiSolveAfter)
                for (const Expression* e : k.after) if (Obj* x = exprNode(e, o)) items.push_back(H(x));
            return true;
        }
        default: return true;
    }
}
inline Obj* constrHandle(int type, const Obj* o) {
    if (o->kind == Obj::Constraint) {
        if (type == vpiParent || type == vpiClassDefn) {
            const Scope* p = o->sym->getParentScope();
            return p && p->asSymbol().kind == SymbolKind::ClassType ? classDefnObj(&p->asSymbol().as<ClassType>()) : nullptr;
        }
        return nullptr;
    }
    const Constraint& c = *static_cast<const Constraint*>(o->node);
    if (type == vpiParent) return const_cast<Obj*>(o->parent);
    if (type == vpiCondition) {
        if (c.kind == ConstraintKind::Implication) return exprNode(&c.as<ImplicationConstraint>().predicate, o);
        if (c.kind == ConstraintKind::Conditional) return exprNode(&c.as<ConditionalConstraint>().predicate, o);
    }
    if (type == vpiExpr && c.kind == ConstraintKind::DisableSoft) return exprNode(&c.as<DisableSoftConstraint>().target, o);
    if (type == vpiVariables && c.kind == ConstraintKind::Foreach) return exprNode(&c.as<ForeachConstraint>().arrayRef, o);
    return nullptr;
}
inline int64_t constrGet(int prop, const Obj* o) {
    if (prop == vpiType) return o->type;
    if (o->kind == Obj::Constraint) {
        const auto& cb = o->sym->as<ConstraintBlockSymbol>();
        switch (prop) {
            case vpiIsConstraintEnabled: return 1;   // constraint_mode starts on (18.9)
            case vpiVirtual: return (bool)(cb.flags & ConstraintBlockFlags::Pure);
            case vpiAutomatic: return !(cb.flags & ConstraintBlockFlags::Static);
            case vpiLineNo: return srcMgr() ? (int64_t)srcMgr()->getLineNumber(cb.location) : 0;
            default: return vpiUndefined;
        }
    }
    const Constraint& c = *static_cast<const Constraint*>(o->node);
    if (prop == vpiLineNo && c.syntax && srcMgr()) return (int64_t)srcMgr()->getLineNumber(c.syntax->sourceRange().start());
    return vpiUndefined;
}
inline bool constrGetStr(int prop, const Obj* o, std::string& r) {
    if (o->kind != Obj::Constraint) return false;
    const auto& cb = o->sym->as<ConstraintBlockSymbol>();
    switch (prop) {
        case vpiName: r = std::string(cb.name); return true;
        case vpiFullName: r = cb.getHierarchicalPath(); return true;
        case vpiFile:
            if (!srcMgr()) return false;
            r = std::string(srcMgr()->getFileName(cb.location));
            return true;
        default: return false;
    }
}
