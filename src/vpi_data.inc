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
    if (!o->ty || (o->kind != Obj::Signal && o->kind != Obj::Bit && o->kind != Obj::Word && o->kind != Obj::DynElem) || o->arr) return false;
    const Type& t = o->ty->getCanonicalType();
    return t.isStruct() || t.kind == SymbolKind::PackedUnionType || t.isUnpackedUnion();
}
inline void members(const Obj* o, std::vector<vpiHandle>& items) {
    for (const auto& f : o->ty->getCanonicalType().as<Scope>().membersOfType<FieldSymbol>())
        if (Obj* m = memberObj(o, f)) items.push_back(H(m));
}

// The contents of a container object. A container that is itself an
// element of a container (or of a class object's property) holds the id of
// its contents, read each time: the element may have been replaced.
inline Val dynRead(const Obj* e);
inline bool isContainerType(const Type& t) {
    const Type& c = t.getCanonicalType();
    return c.isQueue() || c.isAssociativeArray() || c.kind == SymbolKind::DynamicArrayType;
}
inline DynObj* dynOfMut(const Obj* o) {
    int id = o->sig;
    if (o->kind == Obj::Dyn && o->parent) {   // an element that is a container
        Val v = dynRead(o);
        if (v.x || !v.v) return nullptr;
        id = (int)v.v;
    }
    auto it = dynStore().find(id);
    return it == dynStore().end() ? nullptr : &it->second;
}
inline const DynObj* dynOf(const Obj* o) { return dynOfMut(o); }
// The id whose change count wakes a container's readers.
inline int dynSig(const Obj* o) {
    if (o->kind == Obj::Dyn && o->parent) {
        Val v = dynRead(o);
        return v.x ? -1 : (int)v.v;
    }
    return o->sig;
}
// The element shape of a container of type t (37.18): integral, real,
// string, class handle, unpacked struct or union, or another container.
inline bool dynShape(Obj* o, const Type& t) {
    const Type* ep = t.getCanonicalType().getArrayElementType();
    if (!ep) return false;
    const Type& e = ep->getCanonicalType();
    int et = 0, w = 0, t05 = 0;
    if (e.isIntegral() || e.isFloating() || e.isString()) {
        et = varType(e, false, t05);
        w = e.isFloating() || e.isString() ? 64 : (int)e.getBitWidth();
    } else if (e.isClass()) { et = vpiClassVar; w = 64; }
    else if (e.isUnpackedStruct() || e.isUnpackedUnion()) { et = e.isUnpackedStruct() ? vpiStructVar : vpiUnionVar; w = uWidth(e); }
    else if (isContainerType(e)) { et = vpiArrayVar; w = 64; }
    if (!et || w <= 0) return false;
    o->t01 = et;   // the elements' type
    o->w = w;
    o->sgn = e.isIntegral() && e.isSigned();
    o->real = e.isFloating();
    o->str = e.isString();
    o->ty = &e;
    o->left = t.getCanonicalType().kind == SymbolKind::QueueType ? vpiQueueArray : t.getCanonicalType().isAssociativeArray() ? vpiAssocArray : vpiDynamicArray;
    return true;
}
// An element of a dynamic array, queue or associative array: its index or
// key. An element that is itself a container is a container object.
inline Obj* dynElemObj(const Obj* arr, const Val& key) {
    const DynObj* d = dynOf(arr);
    std::string k = d ? (d->strKey ? v_str(key) : std::to_string(d->kind == 2 ? d->key(key).i : v_to_i64(key))) : "";
    auto& slot = st().dynElems[{arr, k}];
    if (!slot) {
        bool cont = arr->ty && isContainerType(*arr->ty);
        slot = mkObj(cont ? Obj::Dyn : Obj::DynElem, arr->type == vpiArrayVar ? arr->t01 : arr->type);
        slot->parent = arr;
        slot->sig = arr->sig;
        slot->cval = key;
        slot->idx = d && !d->strKey && d->kind != 2 ? (int)v_to_i64(key) : -1;
        if (cont) {
            if (!dynShape(slot, *arr->ty)) slot->t01 = 0;
            slot->type = vpiArrayVar;
            return slot;
        }
        slot->w = arr->w;
        slot->real = arr->real;
        slot->str = arr->str;
        slot->sgn = arr->sgn;
        slot->ty = arr->ty;
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
    DynObj* dp = dynOfMut(e->parent);
    int cs = dynSig(e->parent);
    if (!dp || cs < 0) return false;
    DynObj& d = *dp;
    if (d.kind == 2) d.map[d.key(e->cval)] = v;
    else {
        int64_t k = v_to_i64(e->cval);
        if (e->cval.x || k < 0 || k >= (int64_t)d.vec.size()) return false;
        d.vec[(size_t)k] = v;
    }
    Val c = S.get(cs);   // the container's change count: its readers wake up
    S.set(cs, make((c.x ? 0 : c.v) + 1, 0, 64));
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
// A dynamic array, queue or associative array variable: a vpiArrayVar
// whose elements change as the simulation runs.
inline Obj* containerObj(const Symbol* s, const Type& t) {
    auto dm = dynMap().find(s);   // the container's handle signal (lower.hpp)
    int sg = dm == dynMap().end() ? -1 : dm->second;
    if (sg < 0) return nullptr;
    Obj* o = mkObj(Obj::Dyn, vpiArrayVar);
    if (!dynShape(o, t)) return nullptr;   // (the object is kept; it is never handed out)
    o->sig = sg;
    return o;
}
// The element (or member of one) a struct member of an element is in.
inline const Obj* dynRoot(const Obj* o) {
    while (o && o->kind == Obj::Bit) o = o->parent;
    return o && o->kind == Obj::DynElem ? o : nullptr;
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
// The class an object was made of: its first signal holds the class id.
inline const Type* dynClass(int base) {
    if (base <= 0 || (size_t)base >= S.sigs.size()) return nullptr;
    int64_t id = (int64_t)S.sigs[(size_t)base].val.v;
    for (const auto& [t, ci] : classMap())
        if (ci && ci->id == id && ci->ty) return ci->ty;
    return nullptr;
}
// The object a class variable refers to now; null for a null handle.
inline Obj* classObjOf(const Obj* var) {
    Val h = var->kind == Obj::DynElem ? dynRead(var) : S.get(var->sig);   // a variable, or an element of a container
    if (h.x || !h.v) return nullptr;
    auto& slot = st().classObjs[(int64_t)h.v];
    if (!slot) {
        slot = mkObj(Obj::ClassObj, vpiClassObj);
        slot->idx = (int)h.v;
    }
    // The object's own class (its dynamic type, 8.20); the variable's class if unknown.
    const Type* dyn = dynClass(slot->idx);
    slot->ty = dyn ? dyn : var->ty;
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
// ---- Class definitions (37.29) ----
inline const ClassType* classTypeOf(const Type* t) {
    if (!t) return nullptr;
    const Type& c = t->getCanonicalType();
    return c.isClass() ? &c.as<ClassType>() : nullptr;
}
inline Obj* classDefnObj(const ClassType* c) {
    if (!c) return nullptr;
    auto& slot = st().symObj[c];
    if (!slot) {
        slot = mkObj(Obj::ClassDefn, vpiClassDefn);
        slot->sym = c;
        slot->ty = c;
    }
    return slot;
}
// The extends clause of a class (8.13): its base class as a typespec.
inline Obj* extendsObj(const ClassType* c) {
    const ClassType* b = c ? classTypeOf(c->getBaseClass()) : nullptr;
    if (!b) return nullptr;
    auto& slot = st().codeObjs[{c, vpiExtends}];
    if (!slot) {
        slot = mkObj(Obj::Extends, vpiExtends);
        slot->sym = c;
        slot->ty = b;
    }
    return slot;
}
inline bool isMethod(const Obj* o) {
    if (o->kind != Obj::Scope || !o->sym || (o->sym->kind != SymbolKind::Subroutine && o->sym->kind != SymbolKind::MethodPrototype)) return false;
    const Scope* p = o->sym->getParentScope();
    return p && p->asSymbol().kind == SymbolKind::ClassType;
}
// vpi_handle on class objects, variables, typespecs, definitions and methods.
inline Obj* classHandle(int type, const Obj* o) {
    if (type == vpiClassDefn) {
        if (o->kind == Obj::ClassObj || o->kind == Obj::Typespec || (o->kind == Obj::Signal && o->type == vpiClassVar))
            return classDefnObj(classTypeOf(o->ty));
        if (o->kind == Obj::Extends) return classDefnObj(classTypeOf(o->ty));
        if (isMethod(o)) return classDefnObj(&o->sym->getParentScope()->asSymbol().as<ClassType>());
        return nullptr;
    }
    if (o->kind == Obj::ClassDefn) {
        if (type == vpiExtends) return extendsObj(static_cast<const ClassType*>(o->ty));
        if (type == vpiTypespec) return typespecObj(o->ty);
        if (type == vpiParent || type == vpiScope || type == vpiInstance) return scopeOfSym(*o->sym);
        return nullptr;
    }
    if (o->kind == Obj::Extends) {
        if (type == vpiClassTypespec || type == vpiTypespec) return typespecObj(o->ty);
        if (type == vpiParent) return classDefnObj(static_cast<const ClassType*>(&o->sym->as<Type>()));
    }
    return nullptr;
}
inline std::vector<const ClassType*> allClasses() {
    std::vector<const ClassType*> v;
    for (const auto& [t, ci] : classMap())
        if (const ClassType* c = classTypeOf(t)) v.push_back(c);
    std::sort(v.begin(), v.end(), [](const ClassType* a, const ClassType* b) {
        if (a->location != b->location) return a->location < b->location;
        return a->name < b->name;
    });
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
inline bool classIterate(int type, const Obj* o, std::vector<vpiHandle>& items) {
    const auto* c = static_cast<const ClassType*>(o->ty);
    if (type == vpiMethods) {   // the tasks and functions the class declares (not built-in ones; pure virtual prototypes too)
        for (const Symbol& m : c->members())
            if ((m.kind == SymbolKind::Subroutine && !(m.as<SubroutineSymbol>().flags & MethodFlags::BuiltIn)) ||
                m.kind == SymbolKind::MethodPrototype) {   // pure virtual: scopeObj skips other prototypes
                if (Obj* x = scopeObj(m)) items.push_back(H(x));
            }
        return true;
    }
    if (type == vpiDerivedClasses) {
        for (const ClassType* d : allClasses())
            if (classTypeOf(d->getBaseClass()) == c) items.push_back(H(classDefnObj(d)));
        return true;
    }
    if (type == vpiVariables) {   // the static properties (the others live in objects)
        for (const auto& p : c->membersOfType<ClassPropertySymbol>())
            if (p.lifetime == VariableLifetime::Static)
                if (Obj* v = valueObj(&p)) items.push_back(H(v));
        return true;
    }
    return false;
}
inline int64_t classGet(int prop, const Obj* o) {
    if (prop == vpiType) return o->type;
    const Symbol* s = o->kind == Obj::ClassDefn ? o->sym : static_cast<const Symbol*>(o->ty);
    if (prop == vpiVirtual && o->kind == Obj::ClassDefn) return s->as<ClassType>().isAbstract;
    if (prop == vpiLineNo) return srcMgr() ? (int64_t)srcMgr()->getLineNumber(o->sym->location) : 0;
    return vpiUndefined;
}
inline bool classGetStr(int prop, const Obj* o, std::string& r) {
    const auto* c = static_cast<const ClassType*>(o->kind == Obj::ClassDefn ? o->sym : o->ty);
    switch (prop) {
        case vpiName: r = std::string(c->name); return true;
        case vpiFullName: r = c->getHierarchicalPath(); return true;
        case vpiFile:
            if (!srcMgr()) return false;
            r = std::string(srcMgr()->getFileName(o->sym->location));
            return true;
        default: return false;
    }
}
