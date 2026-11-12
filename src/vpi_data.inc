// Included by vpi.inc inside namespace vpi, before valueObj.
// ---- unpacked structs, members, dynamic arrays, queues, associative
// arrays and class objects (IEEE 1800-2023 37.17, 37.18, 37.21, 37.29) ----

// A member of a struct or union variable (vpiMember, 37.17): the bits it
// takes in the variable's vector, so it reads, writes and forces like a
// part select. Its object type is that of a variable of the member's type.
inline Obj* memberObj(const Obj* parent, const FieldSymbol& f) {
    auto& slot = st().pairObjs[{&f, parent}];
    if (slot) return slot;
    const Type& pt = parent->ty ? parent->ty->getCanonicalType() : f.getParentScope()->asSymbol().as<Type>().getCanonicalType();
    const Type& ft = f.getType().getCanonicalType();
    int off, w;
    if (pt.isUnpackedStruct() || pt.isUnpackedUnion()) std::tie(off, w) = uField(pt, f);
    else { off = (int)f.bitOffset; w = (int)ft.getBitWidth(); }
    int t05 = 0, type = ft.isUnpackedStruct() ? vpiStructVar : ft.isUnpackedUnion() ? vpiUnionVar : varType(ft, false, t05);
    if (!type) return nullptr;
    Obj* o = mkObj(Obj::Bit, type);
    o->parent = parent;
    o->sym = &f;
    o->sig = parent->sig;
    o->off = (parent->kind == Obj::Bit ? parent->off : 0) + off;
    o->w = w;
    o->ty = &ft;
    o->sgn = ft.isSigned();
    slot = o;
    return o;
}
inline bool isStructObj(const Obj* o) {
    if (!o->ty || (o->kind != Obj::Signal && o->kind != Obj::Bit && o->kind != Obj::Word) || o->arr) return false;
    const Type& t = o->ty->getCanonicalType();
    return t.isStruct() || t.kind == SymbolKind::PackedUnionType || t.isUnpackedUnion();
}
inline void members(const Obj* o, std::vector<vpiHandle>& items) {
    for (const auto& f : o->ty->getCanonicalType().as<Scope>().membersOfType<FieldSymbol>())
        if (Obj* m = memberObj(o, f)) items.push_back(H(m));
}

// An element of a dynamic array, queue or associative array: its index or key.
inline const DynObj* dynOf(const Obj* o) {
    auto it = dynStore().find(o->sig);
    return it == dynStore().end() ? nullptr : &it->second;
}
inline Obj* dynElemObj(const Obj* arr, const Val& key) {
    const DynObj* d = dynOf(arr);
    std::string k = d ? (d->strKey ? v_str(key) : std::to_string(d->kind == 2 ? d->key(key).i : v_to_i64(key))) : "";
    auto& slot = st().dynElems[{arr, k}];
    if (!slot) {
        slot = mkObj(Obj::DynElem, arr->type == vpiArrayVar ? arr->t01 : arr->type);
        slot->parent = arr;
        slot->sig = arr->sig;
        slot->cval = key;
        slot->w = arr->w;
        slot->real = arr->real;
        slot->str = arr->str;
        slot->sgn = arr->sgn;
        slot->ty = arr->ty;
        slot->idx = d && !d->strKey && d->kind != 2 ? (int)v_to_i64(key) : -1;
    }
    return slot;
}
inline Val dynRead(const Obj* e) {
    const DynObj* d = dynOf(e->parent);
    if (!d) return make(0, ~0ULL, std::max(1, e->w));
    Val v = d->get(e->cval);
    if (e->str) { v = make(v.v, v.x, 64); v.str = true; }
    return v;
}
inline bool dynWrite(const Obj* e, const Val& v) {
    auto it = dynStore().find(e->parent->sig);
    if (it == dynStore().end()) return false;
    DynObj& d = it->second;
    if (d.kind == 2) d.map[d.key(e->cval)] = v;
    else {
        int64_t k = v_to_i64(e->cval);
        if (e->cval.x || k < 0 || k >= (int64_t)d.vec.size()) return false;
        d.vec[(size_t)k] = v;
    }
    Val c = S.get(e->parent->sig);   // the container's change count: its readers wake up
    S.set(e->parent->sig, make((c.x ? 0 : c.v) + 1, 0, 64));
    return true;
}
inline void dynElems(const Obj* arr, std::vector<vpiHandle>& items) {
    const DynObj* d = dynOf(arr);
    if (!d) return;
    if (d->kind == 2) {
        std::vector<Val> keys;
        for (auto& [k, v] : d->map) keys.push_back(d->keyVal(k));
        for (auto& k : keys) items.push_back(H(dynElemObj(arr, k)));
        return;
    }
    for (size_t i = 0; i < d->vec.size(); i++) items.push_back(H(dynElemObj(arr, make(i, 0, 64))));
}
// A dynamic array, queue or associative array variable of integral, real or
// string elements: a vpiArrayVar whose elements change as the simulation runs.
inline Obj* containerObj(const Symbol* s, const Type& t) {
    auto dm = dynMap().find(s);   // the container's handle signal (lower.hpp)
    int sg = dm == dynMap().end() ? -1 : dm->second;
    if (sg < 0) return nullptr;
    const Type& e = t.getArrayElementType()->getCanonicalType();
    if (!e.isIntegral() && !e.isFloating() && !e.isString()) return nullptr;
    int t05 = 0, et = varType(e, false, t05);
    if (!et) return nullptr;
    Obj* o = mkObj(Obj::Dyn, vpiArrayVar);
    o->t01 = et;   // the elements' type
    o->sig = sg;
    o->w = e.isFloating() || e.isString() ? 64 : (int)e.getBitWidth();
    o->sgn = e.isSigned();
    o->real = e.isFloating();
    o->str = e.isString();
    o->ty = &e;
    o->left = t.kind == SymbolKind::QueueType ? vpiQueueArray : t.isAssociativeArray() ? vpiAssocArray : vpiDynamicArray;
    return o;
}

// ---- class objects (37.29) ----
// A class variable holds a handle: the first signal of the object (0 is
// null). The object's properties are signals at fixed offsets from it.
inline Obj* classVarObj(const Symbol* s, const Type& t) {
    int sg = sigOfSym(s);
    if (sg < 0) return nullptr;
    Obj* o = mkObj(Obj::Signal, vpiClassVar);
    o->sig = sg;
    o->w = 64;
    o->ty = &t;
    return o;
}
// The object a class variable refers to now; null for a null handle.
inline Obj* classObjOf(const Obj* var) {
    Val h = S.get(var->sig);
    if (h.x || !h.v) return nullptr;
    auto& slot = st().classObjs[(int64_t)h.v];
    if (!slot) {
        slot = mkObj(Obj::ClassObj, vpiClassObj);
        slot->idx = (int)h.v;
        slot->ty = var->ty;   // the variable's class; a derived object shows the properties of this class
    }
    return slot;
}
// A non-static property of an object: a variable at the object's base
// signal plus the property's offset.
inline Obj* propObj(const Obj* cobj, const Symbol& p) {
    auto pm = propMap().find(&p);
    if (pm == propMap().end() || pm->second.kind != 0) return nullptr;
    auto& slot = st().pairObjs[{&p, cobj}];
    if (slot) return slot;
    const Type& t = p.as<ValueSymbol>().getType().getCanonicalType();
    if (!t.isIntegral() && !t.isFloating() && !t.isString() && !t.isClass()) return nullptr;
    int t05 = 0, type = t.isClass() ? vpiClassVar : varType(t, false, t05);
    if (!type) return nullptr;
    Obj* o = mkObj(Obj::Signal, type);
    o->t05 = t05;
    o->sig = cobj->idx + pm->second.off;
    o->w = t.isFloating() || t.isString() || t.isClass() ? 64 : (int)t.getBitWidth();
    o->sgn = t.isSigned();
    o->real = t.isFloating();
    o->str = t.isString();
    o->ty = &t;
    o->sym = &p;
    o->parent = cobj;
    slot = o;
    return o;
}
// The properties of a class object, its base classes' first (8.13).
inline void classProps(const Obj* cobj, const Type& ct, std::vector<vpiHandle>& items) {
    const auto& c = ct.getCanonicalType().as<ClassType>();
    if (const Type* b = c.getBaseClass()) classProps(cobj, *b, items);
    for (const auto& p : c.membersOfType<ClassPropertySymbol>()) {
        if (p.lifetime == VariableLifetime::Static) {
            if (Obj* v = valueObj(&p)) items.push_back(H(v));
        } else if (Obj* v = propObj(cobj, p)) items.push_back(H(v));
    }
}
