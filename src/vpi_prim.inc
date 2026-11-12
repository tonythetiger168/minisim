// vpi_prim.inc - primitives through VPI (IEEE 1800-2023 clause 37
// primitive diagram, 38.10 their delays): gates, switches and UDP
// instances, their terminals, and their delays.
// #included in vpi.inc, inside namespace vpi.

inline int primType(const PrimitiveInstanceSymbol& g) {
    const auto& p = g.primitiveType;
    if (p.primitiveKind == PrimitiveSymbol::UserDefined) return p.isSequential ? vpiSeqPrim : vpiCombPrim;
    static const std::map<std::string_view, int> m = {
        {"and", vpiAndPrim}, {"nand", vpiNandPrim}, {"nor", vpiNorPrim}, {"or", vpiOrPrim}, {"xor", vpiXorPrim},
        {"xnor", vpiXnorPrim}, {"buf", vpiBufPrim}, {"not", vpiNotPrim}, {"bufif0", vpiBufif0Prim},
        {"bufif1", vpiBufif1Prim}, {"notif0", vpiNotif0Prim}, {"notif1", vpiNotif1Prim}, {"nmos", vpiNmosPrim},
        {"pmos", vpiPmosPrim}, {"cmos", vpiCmosPrim}, {"rnmos", vpiRnmosPrim}, {"rpmos", vpiRpmosPrim},
        {"rcmos", vpiRcmosPrim}, {"rtran", vpiRtranPrim}, {"rtranif0", vpiRtranif0Prim}, {"rtranif1", vpiRtranif1Prim},
        {"tran", vpiTranPrim}, {"tranif0", vpiTranif0Prim}, {"tranif1", vpiTranif1Prim}, {"pullup", vpiPullupPrim},
        {"pulldown", vpiPulldownPrim}};
    auto it = m.find(p.name);
    return it == m.end() ? 0 : it->second;
}
// vpiGate, vpiSwitch or vpiUdp.
inline int primObjType(const PrimitiveInstanceSymbol& g) {
    int t = primType(g);
    if (t == vpiSeqPrim || t == vpiCombPrim) return vpiUdp;
    if (t >= vpiNmosPrim && t <= vpiTranif1Prim) return vpiSwitch;
    return vpiGate;
}
// The gateRecs() entry of a primitive instance, or -1.
inline int primRec(const PrimitiveInstanceSymbol& g) {
    std::string inst = Lowerer::instPath(g.getParentScope());
    for (size_t k = 0; k < gateRecs().size(); k++)
        if (gateRecs()[k].name == g.name && gateRecs()[k].inst == inst) return (int)k;
    return -1;
}
inline Obj* primObj(const PrimitiveInstanceSymbol& g) {
    auto& slot = st().symObj[&g];
    if (!slot) {
        slot = mkObj(Obj::Prim, primObjType(g));
        slot->sym = &g;
        slot->idx = primRec(g);
    }
    return slot;
}
inline Obj* primTermObj(const Obj* p, int k) {
    auto& slot = st().codeObjs[{p->sym, 1000 + k}];
    if (!slot) {
        slot = mkObj(Obj::PrimTerm, vpiPrimTerm);
        slot->sym = p->sym;
        slot->pos = (size_t)k;
        slot->parent = p;
    }
    return slot;
}
// A terminal's direction (28.3 to 28.10, 29.3): the outputs come first.
inline int primTermDir(const PrimitiveInstanceSymbol& g, int k) {
    int n = (int)g.getPortConnections().size();
    int t = primType(g);
    if (t == vpiBufPrim || t == vpiNotPrim) return k < n - 1 ? vpiOutput : vpiInput;   // n outputs, 1 input
    if (t == vpiTranPrim || t == vpiRtranPrim) return vpiInout;
    if (t >= vpiTranif0Prim && t <= vpiTranif1Prim) return k < 2 ? vpiInout : vpiInput;
    if (t == vpiRtranif0Prim || t == vpiRtranif1Prim) return k < 2 ? vpiInout : vpiInput;
    return k == 0 ? vpiOutput : vpiInput;
}
inline bool primIterate(int type, const Obj* o, std::vector<vpiHandle>& items) {
    if ((type == vpiPrimitive || type == vpiGate || type == vpiSwitch || type == vpiUdp) &&
        (o->kind == Obj::Module || o->kind == Obj::Scope)) {
        const Scope* sc = scopeOf(o);
        if (!sc) return false;
        for (const Symbol& m : sc->members()) {
            if (m.kind != SymbolKind::PrimitiveInstance) continue;
            Obj* p = primObj(m.as<PrimitiveInstanceSymbol>());
            if (type == vpiPrimitive || p->type == type) items.push_back(H(p));
        }
        return true;
    }
    if (o->kind == Obj::Prim && type == vpiPrimTerm) {
        int n = (int)o->sym->as<PrimitiveInstanceSymbol>().getPortConnections().size();
        for (int k = 0; k < n; k++) items.push_back(H(primTermObj(o, k)));
        return true;
    }
    return false;
}
inline Obj* primHandle(int type, const Obj* o) {
    if (o->kind == Obj::Prim) {
        if (type == vpiModule || type == vpiScope || type == vpiParent) return scopeOfSym(*o->sym);
        return nullptr;
    }
    const auto& g = o->sym->as<PrimitiveInstanceSymbol>();
    if (type == vpiExpr) return exprObj(g.getPortConnections()[o->pos]);
    if (type == vpiParent || type == vpiPrimitive) return const_cast<Obj*>(o->parent);
    return nullptr;
}
inline int64_t primGet(int prop, const Obj* o) {
    const auto& g = o->sym->as<PrimitiveInstanceSymbol>();
    if (prop == vpiType) return o->type;
    if (prop == vpiLineNo) return srcMgr() ? (int64_t)srcMgr()->getLineNumber(g.location) : 0;
    if (o->kind == Obj::Prim) {
        if (prop == vpiPrimType) return primType(g);
        if (prop == vpiSize) return (int64_t)g.getPortConnections().size();
        return vpiUndefined;
    }
    if (prop == vpiDirection) return primTermDir(g, (int)o->pos);
    if (prop == vpiTermIndex) return (int64_t)o->pos;
    return vpiUndefined;
}
inline bool primGetStr(int prop, const Obj* o, std::string& r) {
    const auto& g = o->sym->as<PrimitiveInstanceSymbol>();
    if (o->kind != Obj::Prim) return false;
    if (prop == vpiName) { r = std::string(g.name); return true; }
    if (prop == vpiFullName) { r = Lowerer::instPath(g.getParentScope()) + "." + std::string(g.name); return true; }
    if (prop == vpiDefName) { r = std::string(g.primitiveType.name); return true; }
    if (prop == vpiFile && srcMgr()) { r = std::string(srcMgr()->getFileName(g.location)); return true; }
    return false;
}
// Delays of a primitive (38.10): rise, fall and turn-off, as 1, 2 or 3
// values (or 6 / 12 transitions).
inline bool primDelayCount(const Obj* o, p_vpi_delay d, const char* who) {
    if (!d || !d->da) { error(vpiError, std::string(who) + ": no delay structure"); return false; }
    int n = d->no_of_delays;
    if (n != 1 && n != 2 && n != 3 && n != 6 && n != 12) {
        error(vpiError, std::string(who) + ": a primitive has 1, 2, 3, 6 or 12 delays");
        return false;
    }
    if (o->idx < 0) { error(vpiError, std::string(who) + ": msim keeps no delays for this primitive"); return false; }
    return true;
}
inline void primGetDelays(const Obj* o, p_vpi_delay d) {
    const GateRec& g = gateRecs()[(size_t)o->idx];
    const Obj* m = scopeOfSym(*o->sym);
    // The order of 1 to 3 values is rise, fall, turn-off; the 12 transitions
    // hold them as 0->1, 1->0, 0->z.
    static const int pick3[3] = {T01, T10, T0Z};
    for (int k = 0; k < d->no_of_delays; k++)
        for (int q = 0; q < (d->mtm_flag ? 3 : 1); q++) {
            p_vpi_time t = &d->da[k * (d->mtm_flag ? 3 : 1) + q];
            t->type = d->time_type;
            fillTime(t, m, g.d[d->no_of_delays <= 3 ? pick3[k] : k]);
        }
}
inline void primPutDelays(const Obj* o, p_vpi_delay d) {
    GateRec& g = gateRecs()[(size_t)o->idx];
    const Obj* m = scopeOfSym(*o->sym);
    std::vector<uint64_t> v;
    for (int k = 0; k < d->no_of_delays; k++) {
        s_vpi_time t = d->da[k * (d->mtm_flag ? 3 : 1) + (d->mtm_flag ? 1 : 0)];   // typical
        t.type = d->time_type;
        v.push_back((uint64_t)std::max<int64_t>(0, sigTicks(&t, m)));
    }
    if (d->no_of_delays == 2) {   // two delays: turn-off is the smaller (28.16)
        v.push_back(std::min(v[0], v[1]));
    }
    auto D = expand12(v);
    for (int k = 0; k < 12; k++) g.d[k] = d->append_flag ? g.d[k] + D[(size_t)k] : D[(size_t)k];
    g.annotated = true;
}
