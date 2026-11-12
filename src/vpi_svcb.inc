// vpi_svcb.inc - SystemVerilog callbacks on objects (IEEE 1800-2023
// 38.36.1, annex M): cbCreateObj when a class object is made, cbSizeChange
// when a dynamic array, queue or associative array changes size.
// #included in vpi.inc, inside namespace vpi.

// The class object whose first signal is `obj`.
inline Obj* classObjById(int obj) {
    auto& slot = st().classObjs[(int64_t)obj];
    if (!slot) {
        slot = mkObj(Obj::ClassObj, vpiClassObj);
        slot->idx = obj;
    }
    if (const Type* t = dynClass(obj)) slot->ty = t;
    return slot;
}
inline void fireObjCb(Cb* c, Obj* obj) {
    s_cb_data d = c->d;
    s_vpi_time t = c->t;
    d.obj = H(obj);
    d.value = nullptr;
    d.time = nullptr;
    if (c->hasT && t.type != vpiSuppressTime) {
        fillTime(&t, nullptr, S.now);
        d.time = &t;
    }
    d.cb_rtn(&d);
}
inline void onNewObj(int obj) {
    auto& s = st();
    std::vector<Cb*> due;
    const Type* cls = nullptr;
    for (Cb* c : s.createCbs) {
        if (!c->live) continue;
        if (c->obj) {   // objects of one class (not of classes derived from it)
            if (!cls) cls = dynClass(obj);
            if (static_cast<const Type*>(c->obj->sym) != cls) continue;
        }
        due.push_back(c);
    }
    for (Cb* c : due) fireObjCb(c, classObjById(obj));
}
inline void onSizeChange(int sig) {
    auto& s = st();
    std::vector<Cb*> due;
    for (Cb* c : s.sizeCbs)
        if (c->live && c->obj->sig == sig) due.push_back(c);
    for (Cb* c : due) fireObjCb(c, c->obj);
}
