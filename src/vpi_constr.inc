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
// dist (18.5.3), and the parts of one: a vpiDistribution has vpiExpr (the
// value distributed) and vpiDistItem items; an item has vpiDistType
// (vpiEqualDist :=, vpiDivDist :/), vpiWeight and vpiValueRange (a value,
// or a [lo:hi] range object with vpiLeftRange / vpiRightRange).
inline Obj* distPart(const void* node, int type, const Obj* parent) {
    auto& slot = st().codeObjs[{node, type}];
    if (!slot) {
        slot = mkObj(Obj::ConstrItem, type);
        slot->node = node;
        slot->sym = parent ? parent->sym : nullptr;
        slot->parent = parent;
    }
    return slot;
}
inline bool isDistPart(const Obj* o) {
    return o->kind == Obj::ConstrItem && (o->type == vpiDistribution || o->type == vpiDistItem || o->type == vpiValueRange);
}
inline Obj* constrItemObj(const Constraint& c, const Obj* parent) {
    if (c.kind == ConstraintKind::Expression) {
        const auto& ec = c.as<ExpressionConstraint>();
        const Expression* e = stripConv(&ec.expr);
        Obj* x = e && e->kind == ExpressionKind::Dist ? distPart(e, vpiDistribution, parent) : exprNode(&ec.expr, parent);
        if (x && ec.isSoft) st().softObjs.insert(x);   // vpiSoft (18.5.13)
        return x;
    }
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
// The constraint blocks of a class object (18.9): its class's own, then
// those it inherits and does not override. Their vpiIsConstraintEnabled is
// the object's constraint_mode.
inline Obj* objConstraintObj(const Obj* cobj, const ConstraintBlockSymbol& c) {
    auto& slot = st().pairObjs[{cobj, &c}];
    if (!slot) {
        slot = mkObj(Obj::Constraint, vpiConstraint);
        slot->sym = &c;
        slot->parent = cobj;
        slot->idx = cobj->idx;
    }
    return slot;
}
inline void objConstraints(const Obj* o, std::vector<vpiHandle>& items) {
    std::set<std::string_view> seen;
    for (const Type* t = o->ty; t && t->isClass();) {
        const auto& ct = t->getCanonicalType().as<ClassType>();
        for (const auto& c : ct.membersOfType<ConstraintBlockSymbol>())
            if (seen.insert(c.name).second) items.push_back(H(objConstraintObj(o, c)));
        t = ct.getBaseClass();
    }
}
inline int constrEnabled(const Obj* o) {
    if (!o->parent || o->parent->kind != Obj::ClassObj) return 1;   // a class's block: constraint_mode starts on
    int obj = o->idx;
    const auto& sp = classRt()[(size_t)S.sigs[obj].val.v].rand;
    if (!sp) return 1;
    for (const auto& b : sp->blocks) {
        if (b->name != o->sym->name || b->mode < 0) continue;
        Val v = S.get(b->staticMode ? b->mode : obj + b->mode);
        return b->staticMode ? !(!v.x && v.v == 0) : (!v.x && (v.v & 1));   // a static block holds "off" as 0
    }
    return 1;
}
inline bool constrIterate(int type, const Obj* o, std::vector<vpiHandle>& items) {
    if (o->kind == Obj::ClassObj && type == vpiConstraint) {
        objConstraints(o, items);
        return true;
    }
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
    if (isDistPart(o)) {
        if (o->type == vpiDistribution && type == vpiDistItem)
            for (const auto& di : static_cast<const DistExpression*>(o->node)->items()) items.push_back(H(distPart(&di, vpiDistItem, o)));
        return true;
    }
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
        if (type == vpiParent && o->parent) return const_cast<Obj*>(o->parent);   // an object's block: the object
        if (type == vpiParent || type == vpiClassDefn) {
            const Scope* p = o->sym->getParentScope();
            return p && p->asSymbol().kind == SymbolKind::ClassType ? classDefnObj(&p->asSymbol().as<ClassType>()) : nullptr;
        }
        return nullptr;
    }
    if (type == vpiParent) return const_cast<Obj*>(o->parent);
    if (o->type == vpiDistribution) return type == vpiExpr ? exprNode(&static_cast<const DistExpression*>(o->node)->left(), o) : nullptr;
    if (o->type == vpiDistItem) {
        const auto& di = *static_cast<const DistExpression::DistItem*>(o->node);
        if (type == vpiWeight) return di.weight && di.weight->expr ? exprNode(di.weight->expr, o) : nullptr;
        if (type != vpiValueRange && type != vpiExpr) return nullptr;
        return di.value.kind == ExpressionKind::ValueRange ? distPart(&di.value, vpiValueRange, o) : exprNode(&di.value, o);
    }
    if (o->type == vpiValueRange) {
        const auto& vr = static_cast<const Expression*>(o->node)->as<ValueRangeExpression>();
        if (type == vpiLeftRange) return exprNode(&vr.left(), o);
        if (type == vpiRightRange) return exprNode(&vr.right(), o);
        return nullptr;
    }
    const Constraint& c = *static_cast<const Constraint*>(o->node);
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
            case vpiIsConstraintEnabled: return constrEnabled(o);   // constraint_mode (18.9)
            case vpiVirtual: return (bool)(cb.flags & ConstraintBlockFlags::Pure);
            case vpiAutomatic: return !(cb.flags & ConstraintBlockFlags::Static);
            case vpiLineNo: return srcMgr() ? (int64_t)srcMgr()->getLineNumber(cb.location) : 0;
            default: return vpiUndefined;
        }
    }
    if (o->kind == Obj::ConstrItem && st().softObjs.count(o)) { if (prop == vpiSoft) return 1; }
    else if (prop == vpiSoft) return 0;
    if (o->type == vpiDistItem) {
        const auto& di = *static_cast<const DistExpression::DistItem*>(o->node);
        if (prop == vpiDistType) return di.weight && di.weight->kind == DistExpression::DistWeight::PerRange ? vpiDivDist : vpiEqualDist;
        return vpiUndefined;
    }
    if (isDistPart(o)) {
        if (prop == vpiLineNo && srcMgr()) return (int64_t)srcMgr()->getLineNumber(static_cast<const Expression*>(o->node)->sourceRange.start());
        return vpiUndefined;
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
