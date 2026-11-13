// vpi_iarr.inc - instance arrays through VPI (IEEE 1800-2023 37.11): module,
// interface, program and primitive (gate, switch, UDP) arrays, their
// ranges and elements, and from an element its array and index.
// #included in vpi.inc, inside namespace vpi.

// The outermost array of a (possibly multidimensional) instance array.
inline const InstanceArraySymbol& iarrRoot(const InstanceArraySymbol& a) {
    const InstanceArraySymbol* r = &a;
    while (r->getParentScope() && r->getParentScope()->asSymbol().kind == SymbolKind::InstanceArray)
        r = &r->getParentScope()->asSymbol().as<InstanceArraySymbol>();
    return *r;
}
// The first element that is not itself an array, or null for an empty array.
inline const Symbol* iarrLeaf(const InstanceArraySymbol& a) {
    const Symbol* e = &a;
    while (e && e->kind == SymbolKind::InstanceArray) {
        const auto& x = e->as<InstanceArraySymbol>();
        e = x.elements.empty() ? nullptr : x.elements[0];
    }
    return e;
}
// vpiModuleArray, vpiInterfaceArray, vpiProgramArray, vpiGateArray,
// vpiSwitchArray or vpiUdpArray.
inline int iarrType(const InstanceArraySymbol& a) {
    const Symbol* e = iarrLeaf(a);
    if (e && e->kind == SymbolKind::PrimitiveInstance) {
        int t = primObjType(e->as<PrimitiveInstanceSymbol>());
        return t == vpiUdp ? vpiUdpArray : t == vpiSwitch ? vpiSwitchArray : vpiGateArray;
    }
    if (e && e->kind == SymbolKind::Instance) {
        auto dk = e->as<InstanceSymbol>().getDefinition().definitionKind;
        if (dk == DefinitionKind::Interface) return vpiInterfaceArray;
        if (dk == DefinitionKind::Program) return vpiProgramArray;
    }
    return vpiModuleArray;
}
inline bool isPrimArrType(int t) { return t == vpiGateArray || t == vpiSwitchArray || t == vpiUdpArray; }
// Does an iteration of `want` include an array of type `t`?
inline bool iarrMatches(int want, int t) {
    return want == t || want == vpiInstanceArray || (want == vpiPrimitiveArray && isPrimArrType(t));
}
inline Obj* iarrObj(const InstanceArraySymbol& a) {
    const InstanceArraySymbol& r = iarrRoot(a);
    auto& o = st().symObj[&r];
    if (!o) {
        o = mkObj(Obj::InstArr, iarrType(r));
        o->sym = &r;
    }
    return o;
}
// The object of an element that is not an array.
inline Obj* iarrElemObj(const Symbol& e) {
    if (e.kind == SymbolKind::Instance) return moduleObj(&e.as<InstanceSymbol>());
    if (e.kind == SymbolKind::PrimitiveInstance) return primObj(e.as<PrimitiveInstanceSymbol>());
    return nullptr;
}
// The ranges of each dimension, leftmost first.
inline std::vector<std::pair<int64_t, int64_t>> iarrDims(const InstanceArraySymbol& a) {
    std::vector<std::pair<int64_t, int64_t>> d;
    const Symbol* e = &a;
    while (e && e->kind == SymbolKind::InstanceArray) {
        const auto& x = e->as<InstanceArraySymbol>();
        d.push_back({x.range.left, x.range.right});
        e = x.elements.empty() ? nullptr : x.elements[0];
    }
    return d;
}
// The elements in order, the leftmost index of each dimension first.
inline void iarrElems(const InstanceArraySymbol& a, std::vector<const Symbol*>& out) {
    size_t n = a.elements.size();
    for (size_t k = 0; k < n; k++) {
        const Symbol* e = a.elements[a.range.isDescending() ? n - 1 - k : k];
        if (e->kind == SymbolKind::InstanceArray) iarrElems(e->as<InstanceArraySymbol>(), out);
        else out.push_back(e);
    }
}
// The element (or sub-array) at index `i` of one dimension.
inline const Symbol* iarrAt(const InstanceArraySymbol& a, int64_t i) {
    if (!a.range.containsPoint((int32_t)i)) return nullptr;
    size_t off = (size_t)(i - a.range.lower());
    return off < a.elements.size() ? a.elements[off] : nullptr;
}
// An element's array and its index in the innermost dimension; null if
// the instance is not in an array.
inline const InstanceArraySymbol* iarrOf(const Symbol& e, int64_t* idx) {
    const Scope* p = e.getParentScope();
    if (!p || p->asSymbol().kind != SymbolKind::InstanceArray) return nullptr;
    const auto& a = p->asSymbol().as<InstanceArraySymbol>();
    for (size_t k = 0; k < a.elements.size(); k++)
        if (a.elements[k] == &e && idx) *idx = a.range.lower() + (int64_t)k;
    return &iarrRoot(a);
}
// A module's or primitive's own name: an element of an array is named with
// its indices (23.3.3), "u[2]" or "m[1][0]".
inline std::string iarrElemName(const Symbol& e) {
    std::string idx;
    const Symbol* s = &e;
    while (s->getParentScope() && s->getParentScope()->asSymbol().kind == SymbolKind::InstanceArray) {
        const auto& a = s->getParentScope()->asSymbol().as<InstanceArraySymbol>();
        for (size_t k = 0; k < a.elements.size(); k++)
            if (a.elements[k] == s) idx = "[" + std::to_string(a.range.lower() + (int64_t)k) + "]" + idx;
        s = &a;
    }
    return std::string(s->name) + idx;
}
// The instance arrays of a scope, in declaration order.
inline void scopeArrays(const Scope& sc, int want, std::vector<vpiHandle>& items) {
    bool flat = flatGenerate();
    for (const Symbol& m : sc.members()) {
        if (m.kind == SymbolKind::InstanceArray) {
            Obj* a = iarrObj(m.as<InstanceArraySymbol>());
            if (iarrMatches(want, a->type)) items.push_back(H(a));
        } else if (flat && m.kind == SymbolKind::GenerateBlock) {
            if (!m.as<GenerateBlockSymbol>().isUninstantiated) scopeArrays(m.as<GenerateBlockSymbol>(), want, items);
        } else if (flat && m.kind == SymbolKind::GenerateBlockArray) {
            for (const auto* e : m.as<GenerateBlockArraySymbol>().entries)
                if (!e->isUninstantiated) scopeArrays(*e, want, items);
        }
    }
}
inline bool isArrIter(int type) {
    switch (type) {
        case vpiInstanceArray: case vpiModuleArray: case vpiInterfaceArray: case vpiProgramArray:
        case vpiPrimitiveArray: case vpiGateArray: case vpiSwitchArray: case vpiUdpArray:
            return true;
        default: return false;
    }
}
inline bool iarrIterate(int type, const Obj* o, std::vector<vpiHandle>& items) {
    if (isArrIter(type) && (o->kind == Obj::Module || o->kind == Obj::Scope)) {
        const Scope* sc = scopeOf(o);
        if (!sc) return false;
        scopeArrays(*sc, type, items);
        return true;
    }
    if (o->kind != Obj::InstArr) return false;
    const auto& a = o->sym->as<InstanceArraySymbol>();
    if (type == vpiRange) {   // 37.11 detail 2: leftmost range first
        auto d = iarrDims(a);
        for (size_t k = 0; k < d.size(); k++) items.push_back(H(rangeObj(o, (int)k, d[k].first, d[k].second)));
        return true;
    }
    bool prim = isPrimArrType(o->type);
    bool ok = type == vpiInstance || (prim ? type == vpiPrimitive || type == vpiGate || type == vpiSwitch || type == vpiUdp
                                           : type == vpiModule || type == vpiInterface || type == vpiProgram);
    if (!ok) return true;
    std::vector<const Symbol*> es;
    iarrElems(a, es);
    for (const Symbol* e : es)
        if (Obj* x = iarrElemObj(*e))
            if (type == vpiInstance || type == vpiPrimitive || x->type == type) items.push_back(H(x));
    return true;
}
inline Obj* listOpObj(const Obj* arr);   // vpi_param.inc
inline Obj* iarrHandle(int type, const Obj* o) {
    const auto& a = o->sym->as<InstanceArraySymbol>();
    switch (type) {
        case vpiExpr: return listOpObj(o);   // detail 1: a vpiListOp of the connections
        case vpiLeftRange: return constObj(a.range.left);    // the leftmost dimension (37.11 detail 2)
        case vpiRightRange: return constObj(a.range.right);
        case vpiScope: return scopeOfSym(a);
        case vpiParent:
        case vpiModule:
        case vpiInstance: return moduleObj(instOf(a.getParentScope()));
        default: return nullptr;
    }
}
// From an element: its array (vpiInstanceArray or its specific type) and
// its index (vpiIndex, the innermost dimension).
inline bool elemHandle(int type, const Obj* o, Obj*& out) {
    if (type != vpiIndex && !isArrIter(type)) return false;
    if (!o->sym || (o->kind != Obj::Module && o->kind != Obj::Prim)) return false;
    int64_t idx = 0;
    const InstanceArraySymbol* a = iarrOf(*o->sym, &idx);
    out = nullptr;
    if (!a) return true;
    if (type == vpiIndex) out = constObj(idx);
    else {
        Obj* x = iarrObj(*a);
        if (iarrMatches(type, x->type)) out = x;
    }
    return true;
}
inline int64_t iarrGet(int prop, const Obj* o) {
    const auto& a = o->sym->as<InstanceArraySymbol>();
    switch (prop) {
        case vpiType: return o->type;
        case vpiSize: {
            std::vector<const Symbol*> es;
            iarrElems(a, es);
            return (int64_t)es.size();
        }
        case vpiLineNo: return srcMgr() ? (int64_t)srcMgr()->getLineNumber(a.location) : 0;
        default: return vpiUndefined;
    }
}
// vpi_handle_by_index / vpi_handle_by_multi_index on an array.
inline Obj* iarrByIndex(const Obj* o, int n, const PLI_INT32* idx) {
    const Symbol* s = o->sym;
    for (int k = 0; k < n; k++) {
        if (!s || s->kind != SymbolKind::InstanceArray) return nullptr;
        s = iarrAt(s->as<InstanceArraySymbol>(), idx[k]);
    }
    return s && s->kind != SymbolKind::InstanceArray ? iarrElemObj(*s) : nullptr;
}
