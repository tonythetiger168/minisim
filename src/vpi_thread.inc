// vpi_thread.inc - threads and frames (IEEE 1800-2023 37, 38.36.1, annex M):
// thread and frame objects, and the callbacks cbStartOfThread,
// cbEndOfThread, cbEnterThread, cbStartOfFrame and cbEndOfFrame.
// #included in vpi.inc, inside namespace vpi.
//
// A thread is one msim process (ProcCtl): an initial, always or final
// procedure, a fork branch, an assertion action. A frame is one running call
// of a task or function in a thread; depth counts the calls below it. A
// handle stays valid (vpiValid) until its thread ends or its call returns;
// msim then stops following the process, so the handle is safe to keep.

inline Obj* thrObj(ProcCtl& c) {
    auto& slot = st().thrObjs[&c];
    if (!slot) {
        slot = mkObj(Obj::Thread, vpiThread);
        slot->thr = &c;
        if (!g_threadHook) g_threadHook = onThread;   // to see it end
    }
    return slot;
}
// The task or function frame `depth` calls deep in c (callee: its body).
inline Obj* frameObj(ProcCtl& c, size_t depth, const Prog* callee) {
    auto& slot = st().frameObjs[{&c, depth}];
    if (!slot) {
        slot = mkObj(Obj::Frame, vpiFrame);
        slot->thr = &c;
        slot->depth = depth;
        slot->node = callee;
        if (!g_threadHook) g_threadHook = onThread;
        if (!g_frameHook) g_frameHook = onFrame;
    }
    return slot;
}
// The calls c is in now, and the body of call `depth`.
inline size_t frameCount(const ProcCtl& c) { return c.rets && !c.ended ? c.rets->size() : 0; }
inline const Prog* frameCallee(const ProcCtl& c, size_t depth) {
    size_t n = frameCount(c);
    if (depth >= n) return nullptr;
    return depth + 1 < n ? (*c.rets)[depth + 1].prog : *c.pp;
}
inline Obj* topFrame(ProcCtl& c) {
    size_t n = frameCount(c);
    return n ? frameObj(c, n - 1, frameCallee(c, n - 1)) : nullptr;
}
// vpi_handle(vpiThread / vpiFrame, NULL): the thread of the running system
// task or function, or its innermost task or function call.
inline Obj* curThreadHandle(int type) {
    auto& s = st();
    if (!s.cur || !g_ctl) {
        error(vpiError, std::string("vpi_handle(") + (type == vpiThread ? "vpiThread" : "vpiFrame") + ", NULL) outside a calltf");
        return nullptr;
    }
    return type == vpiThread ? thrObj(*g_ctl) : topFrame(*g_ctl);
}
inline Obj* thrHandle(int type, Obj* o) {
    if (o->gone) return nullptr;
    ProcCtl& c = *o->thr;
    if (o->kind == Obj::Thread) {
        switch (type) {
            case vpiParent: {   // the thread that forked it
                auto p = c.parent.lock();
                return p ? thrObj(*p) : nullptr;
            }
            case vpiFrame: return topFrame(c);
            default: return nullptr;
        }
    }
    const auto* callee = static_cast<const Prog*>(o->node);
    switch (type) {
        case vpiThread: return thrObj(c);
        case vpiParent: return o->depth ? frameObj(c, o->depth - 1, frameCallee(c, o->depth - 1)) : nullptr;   // its caller
        case vpiScope:   // the task or function it runs
            return callee && callee->sub ? scopeObj(*static_cast<const Symbol*>(callee->sub)) : nullptr;
        default: return nullptr;
    }
}
inline bool thrIterate(int type, Obj* o, std::vector<vpiHandle>& items) {
    if (type != vpiThread || o->kind != Obj::Thread) return false;
    if (o->gone) return true;
    for (auto& k : o->thr->kids)   // the threads it forked that still run
        if (!k->ended) items.push_back(H(thrObj(*k)));
    return true;
}
inline int64_t thrGet(int prop, Obj* o) {
    switch (prop) {
        case vpiType: return o->type;
        case vpiValid: return o->gone ? vpiValidFalse : vpiValidTrue;
        case vpiAutomatic: {
            if (o->kind != Obj::Frame) return vpiUndefined;
            const auto* callee = static_cast<const Prog*>(o->node);
            if (!callee || !callee->sub) return vpiUndefined;
            return static_cast<const slang::ast::SubroutineSymbol*>(callee->sub)->defaultLifetime == slang::ast::VariableLifetime::Automatic;
        }
        default: return vpiUndefined;
    }
}
// The callbacks due for thread c: obj NULL, c itself, or (start) c's parent.
inline bool thrCbOn(const Cb* c, const ProcCtl& t, bool start) {
    if (!c->obj) return true;
    if (c->obj->kind != Obj::Thread || c->obj->gone) return false;
    if (start) return c->obj->thr == t.parent.lock().get();
    return c->obj->thr == &t;
}
inline void onThread(int what, ProcCtl& t) {
    auto& s = st();
    int reason = what == 0 ? cbStartOfThread : what == 1 ? cbEndOfThread : cbEnterThread;
    std::vector<Cb*> due;
    for (Cb* c : s.thrCbs)
        if (c->live && c->d.reason == reason && thrCbOn(c, t, what == 0)) due.push_back(c);
    if (!due.empty()) {
        Obj* h = thrObj(t);
        for (Cb* c : due) fireObjCb(c, h);
    }
    if (what == 1) {   // its handle and its frames' handles are no longer valid
        if (auto it = s.thrObjs.find(&t); it != s.thrObjs.end()) { it->second->gone = true; s.thrObjs.erase(it); }
        for (auto it = s.frameObjs.lower_bound({&t, 0}); it != s.frameObjs.end() && it->first.first == &t;) {
            it->second->gone = true;
            it = s.frameObjs.erase(it);
        }
    }
}
inline void onFrame(int what, ProcCtl& t, size_t depth, const Prog* callee) {
    auto& s = st();
    int reason = what == 0 ? cbStartOfFrame : cbEndOfFrame;
    std::vector<Cb*> due;
    auto fit = s.frameObjs.find({&t, depth});
    if (what == 0 && fit != s.frameObjs.end()) {   // left without a return (disable): that handle is gone
        fit->second->gone = true;
        s.frameObjs.erase(fit);
        fit = s.frameObjs.end();
    }
    for (Cb* c : s.frameCbs) {
        if (!c->live || c->d.reason != reason) continue;
        if (c->obj && c->obj->kind == Obj::Frame) {
            if (fit == s.frameObjs.end() || fit->second != c->obj) continue;
        } else if (!thrCbOn(c, t, false)) continue;
        due.push_back(c);
    }
    if (!due.empty()) {
        Obj* h = frameObj(t, depth, callee);
        for (Cb* c : due) fireObjCb(c, h);
    }
    if (what == 1)
        if (auto it = s.frameObjs.find({&t, depth}); it != s.frameObjs.end()) { it->second->gone = true; s.frameObjs.erase(it); }
}
