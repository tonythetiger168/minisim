// vpi_tchk.inc - timing checks through VPI (IEEE 1800-2023 37.? timing
// check diagram, 38.10 delays of a timing check, 38.36.1 cbTchkViolation).
// #included in vpi.inc, inside namespace vpi.

inline Obj* tchkObj(int id) {
    auto& slot = st().codeObjs[{&timingChecks(), id}];
    if (!slot) {
        slot = mkObj(Obj::Tchk, vpiTchk);
        slot->idx = id;
    }
    return slot;
}
inline Obj* tchkTermObj(int id, int k) {
    auto& slot = st().codeObjs[{&timingChecks()[(size_t)id].ev[k], id}];
    if (!slot) {
        slot = mkObj(Obj::TchkTerm, vpiTchkTerm);
        slot->idx = id;
        slot->pos = (size_t)k;
        slot->parent = tchkObj(id);
    }
    return slot;
}
inline int tchkType(const TimingCheck& tc) {
    static const std::map<std::string, int> m = {
        {"$setup", vpiSetup}, {"$hold", vpiHold}, {"$period", vpiPeriod}, {"$width", vpiWidth},
        {"$skew", vpiSkew}, {"$recovery", vpiRecovery}, {"$nochange", vpiNoChange}, {"$setuphold", vpiSetupHold},
        {"$fullskew", vpiFullskew}, {"$recrem", vpiRecrem}, {"$removal", vpiRemoval}, {"$timeskew", vpiTimeskew}};
    auto it = m.find(tc.name);
    return it == m.end() ? 0 : it->second;
}
// The edges of a terminal as vpiEdge flags (vpiEdge01 ...).
inline int tchkEdges(uint8_t e) {
    int r = 0;
    if (e & TE_01) r |= vpiEdge01;
    if (e & TE_10) r |= vpiEdge10;
    if (e & TE_0X) r |= vpiEdge0x;
    if (e & TE_X1) r |= vpiEdgex1;
    if (e & TE_1X) r |= vpiEdge1x;
    if (e & TE_X0) r |= vpiEdgex0;
    return r;
}
inline Obj* tchkModule(const TimingCheck& tc) {
    return O(vpi_handle_by_name(const_cast<PLI_BYTE8*>(tc.inst.c_str()), nullptr));
}
inline Obj* sigObjOf(int sg) {
    if (sg < 0) return nullptr;
    auto it = st().sigSym.find(sg);
    return it == st().sigSym.end() ? nullptr : valueObj(it->second);
}
// A limit in the module's time unit, as the violation report writes it.
inline std::string tchkTime(int64_t t, uint64_t unit) {
    if (unit <= 1 || t % (int64_t)unit == 0) return std::to_string(t / (int64_t)(unit ? unit : 1));
    std::ostringstream o;
    o << (double)t / (double)unit;
    return o.str();
}
// The terms of a check: the reference event, then the data event (if any).
inline int tchkTermIdx(const TimingCheck& tc, int type) {
    if (type == vpiTchkRefTerm) return tc.ref;
    if (type == vpiTchkDataTerm) return tc.nev == 2 ? tc.data : -1;
    return -1;
}
inline bool tchkIterate(int type, const Obj* o, std::vector<vpiHandle>& items) {
    if (o->kind == Obj::Module && type == vpiTchk) {   // the timing checks of a module instance
        std::string me = fullName(*o->sym);
        for (size_t k = 0; k < timingChecks().size(); k++)
            if (timingChecks()[k].inst == me) items.push_back(H(tchkObj((int)k)));
        return true;
    }
    if (o->kind == Obj::Tchk && type == vpiTchkTerm) {
        const TimingCheck& tc = timingChecks()[(size_t)o->idx];
        for (int k = 0; k < tc.nev; k++) items.push_back(H(tchkTermObj(o->idx, k)));
        return true;
    }
    return false;
}
inline Obj* tchkHandle(int type, const Obj* o) {
    if (o->kind == Obj::Tchk) {
        const TimingCheck& tc = timingChecks()[(size_t)o->idx];
        int k = tchkTermIdx(tc, type);
        if (k >= 0) return tchkTermObj(o->idx, k);
        if (type == vpiTchkNotifier) return sigObjOf(tc.notifier);
        if (type == vpiModule || type == vpiScope || type == vpiParent) return tchkModule(tc);
        return nullptr;
    }
    // a term
    const TcEvent& ev = timingChecks()[(size_t)o->idx].ev[o->pos];
    if (type == vpiExpr) return sigObjOf(ev.sig);
    if (type == vpiTchk || type == vpiParent) return const_cast<Obj*>(o->parent);
    return nullptr;
}
inline int64_t tchkGet(int prop, const Obj* o) {
    const TimingCheck& tc = timingChecks()[(size_t)o->idx];
    if (prop == vpiType) return o->type;
    if (prop == vpiLineNo) return tc.line;
    if (o->kind == Obj::Tchk) {
        if (prop == vpiTchkType) return tchkType(tc);
        return vpiUndefined;
    }
    if (prop == vpiEdge) return tchkEdges(tc.ev[o->pos].edges);
    return vpiUndefined;
}
inline bool tchkGetStr(int prop, const Obj* o, std::string& r) {
    const TimingCheck& tc = timingChecks()[(size_t)o->idx];
    if (prop == vpiFile) { r = tc.file; return true; }
    if (prop == vpiName && o->kind == Obj::TchkTerm) { r = tc.ev[o->pos].text; return true; }
    return false;
}
// Delays of a timing check (38.10): its limits, 1 or 2 of them ($setuphold,
// $recrem: setup and hold; $width: limit and threshold; $fullskew: two).
inline int tchkLimits(const TimingCheck& tc) {
    switch (tchkType(tc)) {
        case vpiSetupHold: case vpiRecrem: case vpiFullskew: case vpiTimeskew: case vpiNoChange: case vpiWidth: return 2;
        default: return 1;
    }
}
inline void tchkGetDelays(const Obj* o, p_vpi_delay d) {
    const TimingCheck& tc = timingChecks()[(size_t)o->idx];
    const Obj* m = tchkModule(tc);
    int n = std::min<int>(d->no_of_delays, tchkLimits(tc));
    for (int k = 0; k < d->no_of_delays; k++)
        for (int q = 0; q < (d->mtm_flag ? 3 : 1); q++) {
            p_vpi_time t = &d->da[k * (d->mtm_flag ? 3 : 1) + q];
            t->type = d->time_type;
            int64_t v = k >= n ? 0 : k == 0 ? tc.lim1 : tchkType(tc) == vpiWidth ? tc.thresh : tc.lim2;
            fillTime(t, m, (uint64_t)std::max<int64_t>(0, v));
        }
}
inline void tchkPutDelays(const Obj* o, p_vpi_delay d) {
    TimingCheck& tc = timingChecks()[(size_t)o->idx];
    const Obj* m = tchkModule(tc);
    int n = std::min<int>(d->no_of_delays, tchkLimits(tc));
    for (int k = 0; k < n; k++) {
        s_vpi_time t = d->da[k * (d->mtm_flag ? 3 : 1) + (d->mtm_flag ? 1 : 0)];   // typical
        t.type = d->time_type;
        int64_t v = sigTicks(&t, m);
        int64_t& at = k == 0 ? tc.lim1 : tchkType(tc) == vpiWidth ? tc.thresh : tc.lim2;
        at = d->append_flag ? at + v : v;
    }
    // The report gives the limits as written: now these.
    tc.limText = tchkTime(tc.lim1, tc.unit);
    if (tchkLimits(tc) == 2) tc.limText += ", " + tchkTime(tchkType(tc) == vpiWidth ? tc.thresh : tc.lim2, tc.unit);
}
// cbTchkViolation (38.36.1): after a check reports a violation, on that
// check or (obj NULL) on any.
inline void onTchk(int id) {
    auto& s = st();
    std::vector<Cb*> due;
    for (Cb* c : s.tchkCbs)
        if (c->live && (!c->obj || c->obj->idx == id)) due.push_back(c);
    for (Cb* c : due) {
        s_cb_data d = c->d;
        s_vpi_time t = c->t;
        d.obj = H(tchkObj(id));
        d.value = nullptr;
        d.time = nullptr;
        if (c->hasT && t.type != vpiSuppressTime) {
            fillTime(&t, tchkModule(timingChecks()[(size_t)id]), S.now);
            d.time = &t;
        }
        d.cb_rtn(&d);
    }
}
