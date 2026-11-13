// dpi.inc - DPI-C run time (IEEE 1800-2023 clause 35, annex H). #include in
// msim.cpp after minisim.cpp. It loads the C libraries, calls imported
// functions through libffi, runs exported SV subroutines when C calls them,
// and implements the svdpi.h functions (msim is linked with -rdynamic, so a
// loaded library finds them in msim itself).
//
// An import function runs on the simulator's stack. An import task runs on
// its own stack (a ucontext fiber): when it calls an exported task that
// waits on time, the fiber is parked, the calling process waits, and the
// fiber resumes when the exported task ends (35.5.2).
#include <cstdarg>
#include <cstring>
#include <dlfcn.h>
#include <ffi.h>
#include <sys/mman.h>
#include <ucontext.h>

#include "../include/svdpi.h"

namespace dpi {

// An unpacked array argument (35.5.6.1, H.7.6) as C sees it: the elements
// in C layout, row-major, and in each dimension the lowest index first.
// dims are the formal's view: an open dimension has the actual's range, a
// sized one the formal's.
struct OpenArr {
    char* data = nullptr;
    size_t elemSize = 0, count = 0;
    DpiType t = DT_INT;
    int w = 32;
    std::vector<std::pair<int64_t, int64_t>> dims;   // unpacked, [left, right]
    // Byte offset of an element, or -1 when an index is outside its range.
    int64_t offset(const int* idx) const {
        int64_t off = 0;
        for (size_t d = 0; d < dims.size(); d++) {
            auto [l, r] = dims[d];
            int64_t n = (l > r ? l - r : r - l) + 1;
            int64_t o = idx[d] - std::min(l, r);
            if (o < 0 || o >= n) return -1;
            off = off * n + o;
        }
        return off * (int64_t)elemSize;
    }
};

struct Scope {
    std::string name;
    bool prot = false;   // inside protected IP: its name is not given out (IEEE 1735 9.7.2.3)
    std::map<void*, void*> user;
};

// One import's resolved C function and its libffi call interface.
struct Import {
    void (*fn)() = nullptr;
    ffi_cif cif;
    std::vector<ffi_type*> types;
    ffi_type* ret = &ffi_type_void;
};

struct Fiber {
    ucontext_t ctx{}, back{};
    char* stack = nullptr;
    size_t size = 0;
    const Instr* ins = nullptr;
    bool done = false;
    shared_ptr<JoinState> wait;      // an exported task this fiber waits for
    shared_ptr<ProcCtl> owner;       // the process that called the import task
    bool disabled = false;           // that process was disabled (35.9)
    bool acked = false;              // and the C code called svAckDisabledState
};

struct State {
    std::vector<void*> libs;
    std::map<std::string, std::unique_ptr<Scope>> scopes;
    std::vector<const DpiFunc*> exports;                    // by stub id
    std::map<std::string, std::vector<const DpiFunc*>> exportsByName;
    Scope* scope = nullptr;          // the current scope (svSetScope / the import's context)
    const Instr* call = nullptr;     // the import call running now
    Fiber* fiber = nullptr;          // the import task fiber running now
    std::vector<Fiber*> freeFibers;
};
inline State& st() { static State s; return s; }

inline Scope* scopeOf(const std::string& name) {
    auto& m = st().scopes;
    auto it = m.find(name);
    if (it != m.end()) return it->second.get();
    auto s = std::make_unique<Scope>();
    s->name = name;
    Scope* r = s.get();
    m[name] = std::move(s);
    return r;
}

[[noreturn]] inline void fatal(const std::string& msg) {
    std::cout.flush();
    std::cerr << "msim: DPI: " << msg << "\n";
    std::exit(1);
}

// ---- values between SV and C ----
inline int nwords(int w) { return (w + 31) / 32; }
inline size_t elemBytes(DpiType t, int w) {
    switch (t) {
        case DT_BYTE: case DT_BIT: case DT_LOGIC: return 1;
        case DT_SHORT: return 2;
        case DT_INT: case DT_SHORTREAL: return 4;
        case DT_LONG: case DT_REAL: case DT_CHANDLE: case DT_STRING: return 8;
        case DT_BITVEC: return 4 * (size_t)nwords(w);
        case DT_LOGICVEC: return 8 * (size_t)nwords(w);
        default: return 0;
    }
}
inline uint64_t bits64(const Val& v) { return v.v & ~v.x; }
// Bit i of v as svLogic: 0, 1, z = 2, x = 3.
inline uint8_t logicAt(const Val& v, int i) {
    int b = bitAt(v, i);
    return b == 0 ? sv_0 : b == 1 ? sv_1 : b == 3 ? sv_z : sv_x;
}

// Write an SV value into C storage at p (C layout of type t).
// strs keeps the text of strings alive for the call.
inline void toC(void* p, DpiType t, int w, const Val& v, std::vector<std::string>& strs) {
    switch (t) {
        case DT_BYTE: { int8_t x = (int8_t)bits64(v); memcpy(p, &x, 1); break; }
        case DT_SHORT: { int16_t x = (int16_t)bits64(v); memcpy(p, &x, 2); break; }
        case DT_INT: { int32_t x = (int32_t)bits64(v); memcpy(p, &x, 4); break; }
        case DT_LONG: { int64_t x = (int64_t)bits64(v); memcpy(p, &x, 8); break; }
        case DT_REAL: { double d = v.real ? v_real(v) : (double)bits64(v); if (!v.real) memcpy(&d, &v.v, 8); memcpy(p, &d, 8); break; }
        case DT_SHORTREAL: { double d; memcpy(&d, &v.v, 8); float f = (float)d; memcpy(p, &f, 4); break; }
        case DT_CHANDLE: { void* x = (void*)(uintptr_t)bits64(v); memcpy(p, &x, 8); break; }
        case DT_STRING: {
            Val s = v;
            s.str = true;
            strs.push_back(v_str(s));
            // reserve() above keeps earlier entries in place
            const char* c = strs.back().c_str();
            memcpy(p, &c, sizeof c);
            break;
        }
        case DT_BIT: { uint8_t b = bitAt(v, 0) == 1; memcpy(p, &b, 1); break; }
        case DT_LOGIC: { uint8_t b = logicAt(v, 0); memcpy(p, &b, 1); break; }
        case DT_BITVEC: {
            auto* o = (uint32_t*)p;
            for (int k = 0; k < nwords(w); k++) {
                uint64_t l = limbV(v, k / 2) & ~limbX(v, k / 2);
                o[k] = (uint32_t)(l >> (32 * (k % 2)));
            }
            int r = w % 32;
            if (r) o[nwords(w) - 1] &= (1u << r) - 1;
            break;
        }
        case DT_LOGICVEC: {
            auto* o = (svLogicVecVal*)p;
            for (int k = 0; k < nwords(w); k++) {
                int sh = 32 * (k % 2);
                uint64_t lv = limbV(v, k / 2), lx = limbX(v, k / 2), lz = limbZ(v, k / 2);
                uint32_t a = (uint32_t)(lv >> sh), x = (uint32_t)(lx >> sh), z = (uint32_t)(lz >> sh);
                o[k].aval = (a & ~x) | (x & ~z);   // x: 1/1, z: 0/1
                o[k].bval = x;
            }
            int r = w % 32;
            if (r) { o[nwords(w) - 1].aval &= (1u << r) - 1; o[nwords(w) - 1].bval &= (1u << r) - 1; }
            break;
        }
        default: break;
    }
}

// Read C storage at p back into an SV value of width w.
inline Val fromC(const void* p, DpiType t, int w, bool sgn) {
    Val r;
    switch (t) {
        case DT_BYTE: { int8_t x; memcpy(&x, p, 1); r = make((uint64_t)(int64_t)x, 0, w); break; }
        case DT_SHORT: { int16_t x; memcpy(&x, p, 2); r = make((uint64_t)(int64_t)x, 0, w); break; }
        case DT_INT: { int32_t x; memcpy(&x, p, 4); r = make((uint64_t)(int64_t)x, 0, w); break; }
        case DT_LONG: { int64_t x; memcpy(&x, p, 8); r = make((uint64_t)x, 0, w); break; }
        case DT_REAL: { double d; memcpy(&d, p, 8); return mkreal(d); }
        case DT_SHORTREAL: { float f; memcpy(&f, p, 4); return mkreal((double)f); }
        case DT_CHANDLE: { void* x; memcpy(&x, p, 8); r = make((uint64_t)(uintptr_t)x, 0, 64); break; }
        case DT_STRING: { const char* c; memcpy(&c, p, sizeof c); return mkstr(c ? c : ""); }
        case DT_BIT: { uint8_t b; memcpy(&b, p, 1); r = make(b & 1, 0, 1); break; }
        case DT_LOGIC: {
            uint8_t b; memcpy(&b, p, 1);
            r = b == sv_0 ? make(0, 0, 1) : b == sv_1 ? make(1, 0, 1) : b == sv_z ? make(0, 1, 1, 1) : make(0, 1, 1);
            break;
        }
        case DT_BITVEC: {
            auto* in = (const uint32_t*)p;
            r = make(0, 0, w);
            for (int k = 0; k < nwords(w); k++) {
                int l = k / 2, sh = 32 * (k % 2);
                setLimb(r, l, limbV(r, l) | ((uint64_t)in[k] << sh), 0, 0);
            }
            r = finish(r);
            break;
        }
        case DT_LOGICVEC: {
            auto* in = (const svLogicVecVal*)p;
            r = make(0, 0, w);
            for (int k = 0; k < nwords(w); k++) {
                int l = k / 2, sh = 32 * (k % 2);
                uint32_t a = in[k].aval, b = in[k].bval;
                uint64_t v = (uint64_t)(a & ~b) << sh, x = (uint64_t)b << sh, z = (uint64_t)(b & ~a) << sh;
                setLimb(r, l, limbV(r, l) | v, limbX(r, l) | x, limbZ(r, l) | z);
            }
            r = finish(r);
            break;
        }
        default: r = make(0, 0, w > 0 ? w : 1); break;
    }
    r.sgn = sgn;
    return r;
}

inline ffi_type* ffiOf(DpiType t) {
    switch (t) {
        case DT_VOID: return &ffi_type_void;
        case DT_BYTE: return &ffi_type_sint8;
        case DT_SHORT: return &ffi_type_sint16;
        case DT_INT: return &ffi_type_sint32;
        case DT_LONG: return &ffi_type_sint64;
        case DT_REAL: return &ffi_type_double;
        case DT_SHORTREAL: return &ffi_type_float;
        case DT_BIT: case DT_LOGIC: return &ffi_type_uint8;
        default: return &ffi_type_pointer;
    }
}

// ---- an unpacked array argument: SV array <-> C buffer ----
struct ArrBuf {
    OpenArr oa;
    std::vector<char> bytes;
};
// C element k of the buffer is actual element map[k] (a flat offset in the
// actual's own order: row-major, each dimension from its left bound).
inline void arrIn(ArrBuf& b, const DpiArg& f, const DpiActual& a, std::vector<std::string>& strs,
                  std::vector<size_t>& map) {
    int w = f.popen ? a.pw : f.w;
    b.oa.t = f.t;
    b.oa.w = w;
    b.oa.elemSize = elemBytes(f.t, w);
    std::vector<Val> vals;
    std::vector<std::pair<int64_t, int64_t>> act;
    if (a.arr) {
        act = a.arr->dims;
        for (int k = 0; k < a.arr->count; k++) vals.push_back(S.get(a.arr->base + k));
    } else {
        vals = dynStore()[a.dyn].vec;
        act = {{0, (int64_t)vals.size() - 1}};
    }
    b.oa.dims.clear();
    for (size_t d = 0; d < act.size(); d++) {
        bool open = d >= f.uopen.size() || f.uopen[d];
        b.oa.dims.push_back(open ? act[d] : f.udims[d]);
    }
    b.oa.count = vals.size();
    map.assign(vals.size(), 0);
    for (size_t c = 0; c < vals.size(); c++) {   // C order -> the actual's order
        size_t rem = c, flat = 0, mul = 1;
        for (size_t d = b.oa.dims.size(); d-- > 0;) {
            auto [l, r] = b.oa.dims[d];
            size_t n = (size_t)((l > r ? l - r : r - l) + 1);
            size_t cd = rem % n;
            rem /= n;
            size_t o = l > r ? n - 1 - cd : cd;   // offset from the left bound
            flat += o * mul;
            mul *= n;
        }
        map[c] = flat < vals.size() ? flat : 0;
    }
    b.bytes.assign(b.oa.count * b.oa.elemSize + 8, 0);
    b.oa.data = b.bytes.data();
    if (f.t == DT_STRING) strs.reserve(strs.size() + vals.size());
    for (size_t k = 0; k < vals.size(); k++) toC(b.oa.data + k * b.oa.elemSize, f.t, w, vals[map[k]], strs);
}
inline void arrOut(const ArrBuf& b, const DpiArg& f, const DpiActual& a, const std::vector<size_t>& map) {
    for (size_t k = 0; k < b.oa.count; k++) {
        Val v = fromC(b.oa.data + k * b.oa.elemSize, f.t, b.oa.w, f.sgn);
        if (a.arr) {
            if (a.arr->twoState) { v.x = 0; v.z = 0; for (auto& h : v.hi) { h.x = h.z = 0; } }
            S.set(a.arr->base + (int)map[k], v);
        } else {
            auto& d = dynStore()[a.dyn];
            if (map[k] < d.vec.size()) d.vec[map[k]] = v;
        }
    }
    if (!a.arr) dynBump(a.dyn);
}

// ---- calling an import ----
// Runs the whole call: formals -> C, the C function, C -> formals.
inline void invoke(const Instr& ins) {
    const DpiFunc& f = *ins.dpi;
    auto* imp = (Import*)f.rt;
    size_t n = f.args.size();
    // Per-argument storage: the value (or the value an output pointer points
    // at), the pointer passed for by-reference arguments, and array buffers.
    struct Slot { alignas(16) unsigned char val[16]; void* ptr; std::vector<unsigned char> vec; };
    std::vector<Slot> slots(n);
    std::vector<ArrBuf> arrs(n);
    std::vector<std::vector<size_t>> maps(n);
    std::vector<void*> argv(n);
    std::vector<std::string> strs;
    strs.reserve(2 * n + 1);
    for (size_t k = 0; k < n; k++) {
        const DpiArg& a = f.args[k];
        Slot& s = slots[k];
        memset(s.val, 0, sizeof s.val);
        if (a.uarr) {
            arrIn(arrs[k], a, ins.dact[k], strs, maps[k]);
            s.ptr = a.open ? (void*)&arrs[k].oa : (void*)arrs[k].oa.data;
            argv[k] = &s.ptr;
            continue;
        }
        Val v = S.get(a.sig);
        if (a.t == DT_STRUCT) {   // a C struct, passed by pointer whatever the direction
            s.vec.assign(std::max<size_t>(a.cSize, 1), 0);
            for (const DpiField& fl : a.fields) {
                Val fv = v_slice(v, fl.lo + fl.w - 1, fl.lo);
                if (fl.t == DT_REAL || fl.t == DT_SHORTREAL) fv.real = true;
                toC(s.vec.data() + fl.off, fl.t, fl.w, fv, strs);
            }
            s.ptr = s.vec.data();
            argv[k] = &s.ptr;
            continue;
        }
        if (a.t == DT_REAL || a.t == DT_SHORTREAL) { v.real = true; }
        bool vec = a.t == DT_BITVEC || a.t == DT_LOGICVEC;
        if (vec) {
            s.vec.assign(elemBytes(a.t, a.w), 0);
            toC(s.vec.data(), a.t, a.w, v, strs);
            s.ptr = s.vec.data();
            argv[k] = &s.ptr;
        } else {
            toC(s.val, a.t, a.w, v, strs);
            if (a.dir == 0) argv[k] = s.val;
            else { s.ptr = s.val; argv[k] = &s.ptr; }
        }
    }
    alignas(16) unsigned char ret[16] = {0};
    // svGetScope / svGetCallerInfo see this call; restore the caller's afterwards.
    Scope* savedScope = st().scope;
    const Instr* savedCall = st().call;
    st().scope = scopeOf(f.scope);
    st().call = &ins;
    ffi_call(&imp->cif, imp->fn, ret, argv.data());
    st().scope = savedScope;
    st().call = savedCall;
    for (size_t k = 0; k < n; k++) {
        const DpiArg& a = f.args[k];
        if (a.dir == 0) continue;
        if (a.uarr) { arrOut(arrs[k], a, ins.dact[k], maps[k]); continue; }
        if (a.t == DT_STRUCT) {
            Val v = S.get(a.sig);
            for (const DpiField& fl : a.fields) {
                Val fv = fromC(slots[k].vec.data() + fl.off, fl.t, fl.w, fl.sgn);
                fv.real = false;
                v = v_insert(v, v_resize(fv, fl.w, false), fl.lo + fl.w - 1, fl.lo);
            }
            S.set(a.sig, v);
            continue;
        }
        const void* p = (a.t == DT_BITVEC || a.t == DT_LOGICVEC) ? (const void*)slots[k].vec.data() : (const void*)slots[k].val;
        S.set(a.sig, fromC(p, a.t, a.w, a.sgn));
    }
    if (f.retSig >= 0) {
        Val r;
        // libffi widens a small integer result to a full ffi_arg.
        if (f.ret == DT_BYTE || f.ret == DT_SHORT || f.ret == DT_INT || f.ret == DT_BIT || f.ret == DT_LOGIC) {
            ffi_arg x; memcpy(&x, ret, sizeof x);
            int64_t sx = f.ret == DT_BYTE ? (int8_t)x : f.ret == DT_SHORT ? (int16_t)x : f.ret == DT_INT ? (int32_t)x : (int64_t)(uint8_t)x;
            unsigned char tmp[8];
            if (f.ret == DT_BIT || f.ret == DT_LOGIC) { tmp[0] = (uint8_t)sx; }
            else if (f.ret == DT_BYTE) { int8_t y = (int8_t)sx; memcpy(tmp, &y, 1); }
            else if (f.ret == DT_SHORT) { int16_t y = (int16_t)sx; memcpy(tmp, &y, 2); }
            else { int32_t y = (int32_t)sx; memcpy(tmp, &y, 4); }
            r = fromC(tmp, f.ret, f.retW, f.retSgn);
        } else r = fromC(ret, f.ret, f.retW, f.retSgn);
        S.set(f.retSig, r);
    }
}

// ---- fibers for import tasks ----
inline void fiberMain(unsigned lo, unsigned hi) {
    Fiber* fb = (Fiber*)(((uintptr_t)hi << 32) | (uintptr_t)lo);
    invoke(*fb->ins);
    fb->done = true;
    swapcontext(&fb->ctx, &fb->back);
}
inline Fiber* newFiber(const Instr& ins, const shared_ptr<ProcCtl>& owner) {
    Fiber* fb;
    if (!st().freeFibers.empty()) { fb = st().freeFibers.back(); st().freeFibers.pop_back(); }
    else {
        fb = new Fiber;
        fb->size = 1 << 20;   // 1 MiB for the C code of one import task
        void* m = mmap(nullptr, fb->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (m == MAP_FAILED) fatal("cannot allocate a stack for an import task");
        fb->stack = (char*)m;
    }
    fb->ins = &ins;
    fb->done = false;
    fb->disabled = fb->acked = false;
    fb->owner = owner;
    fb->wait.reset();
    getcontext(&fb->ctx);
    fb->ctx.uc_stack.ss_sp = fb->stack;
    fb->ctx.uc_stack.ss_size = fb->size;
    fb->ctx.uc_link = nullptr;
    uintptr_t p = (uintptr_t)fb;
    makecontext(&fb->ctx, (void (*)())fiberMain, 2, (unsigned)(p & 0xffffffffu), (unsigned)(p >> 32));
    return fb;
}
// Run fb until it ends or parks on an exported task.
inline shared_ptr<JoinState> enter(Fiber* fb, void*& out) {
    Fiber* prev = st().fiber;
    Scope* sc = st().scope;
    const Instr* cl = st().call;
    st().fiber = fb;
    swapcontext(&fb->back, &fb->ctx);
    st().fiber = prev;
    st().scope = sc;
    st().call = cl;
    if (fb->done) {
        if (fb->disabled && !fb->acked)
            std::cerr << "msim: DPI: warning: import task \"" << fb->ins->dpi->cName
                      << "\" was disabled but did not call svAckDisabledState (35.9)\n";
        fb->owner.reset();
        st().freeFibers.push_back(fb);
        out = nullptr;
        return nullptr;
    }
    out = fb;
    return fb->wait;
}
inline shared_ptr<JoinState> call(const Instr& ins, void*& fib, const shared_ptr<ProcCtl>& ctl) {
    if (!ins.dpi->task) { invoke(ins); return nullptr; }
    return enter(newFiber(ins, ctl), fib);
}
// The process of a waiting import task was disabled: every exported task it
// is in returns 1, and the C code runs to its end (35.9).
inline void kill(void* fib) {
    Fiber* fb = (Fiber*)fib;
    fb->disabled = true;
    void* out = nullptr;
    while (enter(fb, out)) {}
}
inline shared_ptr<JoinState> resume(void* fib) {
    void* out = nullptr;
    auto r = enter((Fiber*)fib, out);
    return r;
}

// ---- calling an export from C ----
inline const DpiFunc* exportFor(int id) {
    const DpiFunc* f0 = st().exports.at((size_t)id);
    auto& all = st().exportsByName[f0->cName];
    if (all.size() == 1) return all[0];
    if (!st().scope) fatal("export \"" + f0->cName + "\" called with no scope set (35.5.3: use svSetScope)");
    for (const DpiFunc* f : all)
        if (f->scope == st().scope->name) return f;
    fatal("export \"" + f0->cName + "\" is not declared in scope " + st().scope->name);
}

}  // namespace dpi

// Called by the generated export stubs: args[k] points at argument k as the
// C function received it, ret at the C result.
extern "C" int msim_dpi_export(int id, void** args, void* ret) {
    using namespace dpi;
    const DpiFunc& f = *exportFor(id);
    Fiber* fb = st().fiber;
    if (fb && fb->disabled) {   // 35.9: a disabled import task's exported tasks return at once
        if (f.task && ret) { int one = 1; memcpy(ret, &one, sizeof one); }
        return 0;
    }
    std::vector<std::string> strs;
    for (size_t k = 0; k < f.args.size(); k++) {
        const DpiArg& a = f.args[k];
        if (a.dir == 1) continue;
        if (a.sarr) {   // a sized array: C passes its first element's address
            const char* base = *(const char* const*)args[k];
            size_t eb = elemBytes(a.t, a.w);
            for (int e = 0; e < a.sarr->count; e++) {
                Val v = fromC(base + (size_t)e * eb, a.t, a.w, a.sgn);
                if (a.sarr->twoState) { v.x = 0; v.z = 0; for (auto& h : v.hi) { h.x = h.z = 0; } }
                S.set(a.sarr->base + e, v);
            }
            continue;
        }
        bool vec = a.t == DT_BITVEC || a.t == DT_LOGICVEC;
        const void* p = args[k];
        if (vec || a.dir != 0) p = *(void* const*)args[k];   // by pointer
        S.set(a.sig, fromC(p, a.t, a.w, a.sgn));
    }
    auto ctl = make_shared<ProcCtl>();
    auto js = make_shared<JoinState>();
    js->left = 1;
    ctl->join = js;
    if (fb && fb->owner) {   // disabling the calling process ends this task too
        ctl->parent = fb->owner;
        fb->owner->kids.push_back(ctl);
    }
    Task t = startProc(f.prog, ctl);
    Scope* sc = st().scope;
    const Instr* cl = st().call;
    t.h.resume();
    if (!ctl->ended) {   // the exported task waits on time (35.5.2)
        if (!f.task || !fb)
            fatal("exported task \"" + f.cName + "\" waited on time, but it was not called from an import task");
        S.all.push_back(t.h);
        fb->wait = js;
        swapcontext(&fb->ctx, &fb->back);   // back in fiber once the task ended
    } else
        t.h.destroy();
    st().scope = sc;
    st().call = cl;
    std::vector<std::string> keep;
    for (size_t k = 0; k < f.args.size(); k++) {
        const DpiArg& a = f.args[k];
        if (a.dir == 0) continue;
        void* p = *(void**)args[k];
        if (a.sarr) {
            size_t eb = elemBytes(a.t, a.w);
            for (int e = 0; e < a.sarr->count; e++) {
                Val v = S.get(a.sarr->base + e);
                if (a.t == DT_REAL || a.t == DT_SHORTREAL) v.real = true;
                char* q = (char*)p + (size_t)e * eb;
                if (a.t == DT_STRING) {
                    Val s = v; s.str = true;
                    const char* c = strPool().text[s.v < strPool().text.size() ? s.v : 0].c_str();
                    memcpy(q, &c, sizeof c);
                } else toC(q, a.t, a.w, v, keep);
            }
            continue;
        }
        Val v = S.get(a.sig);
        if (a.t == DT_REAL || a.t == DT_SHORTREAL) v.real = true;
        if (a.t == DT_STRING) {   // the text must outlive the call: keep it in the string pool
            Val s = v; s.str = true;
            const char* c = strPool().text[s.v < strPool().text.size() ? s.v : 0].c_str();
            memcpy(p, &c, sizeof c);
            continue;
        }
        toC(p, a.t, a.w, v, keep);
    }
    if (f.retSig >= 0 && ret) {
        Val v = S.get(f.retSig);
        if (f.ret == DT_REAL || f.ret == DT_SHORTREAL) v.real = true;
        if (f.ret == DT_STRING) {
            Val s = v; s.str = true;
            const char* c = strPool().text[s.v < strPool().text.size() ? s.v : 0].c_str();
            memcpy(ret, &c, sizeof c);
        } else toC(ret, f.ret, f.retW, v, keep);
    }
    if (f.task && ret) { int z = ctl->killed || (fb && fb->disabled) ? 1 : 0; memcpy(ret, &z, sizeof z); }
    return 0;
}

// ---- svdpi.h ----
extern "C" {
const char* svDpiVersion(void) { return "1800-2023"; }

svBit svGetBitselBit(const svBitVecVal* s, int i) { return (s[i / 32] >> (i % 32)) & 1; }
svLogic svGetBitselLogic(const svLogicVecVal* s, int i) {
    uint32_t a = (s[i / 32].aval >> (i % 32)) & 1, b = (s[i / 32].bval >> (i % 32)) & 1;
    return (svLogic)(b << 1 | a);   // 0/0 0, 1/0 1, 0/1 z(2), 1/1 x(3)
}
void svPutBitselBit(svBitVecVal* d, int i, svBit s) {
    uint32_t m = 1u << (i % 32);
    d[i / 32] = (s & 1) ? (d[i / 32] | m) : (d[i / 32] & ~m);
}
void svPutBitselLogic(svLogicVecVal* d, int i, svLogic s) {
    uint32_t m = 1u << (i % 32);
    uint32_t a = s == sv_1 || s == sv_x, b = s == sv_z || s == sv_x;
    d[i / 32].aval = a ? (d[i / 32].aval | m) : (d[i / 32].aval & ~m);
    d[i / 32].bval = b ? (d[i / 32].bval | m) : (d[i / 32].bval & ~m);
}
void svGetPartselBit(svBitVecVal* d, const svBitVecVal* s, int i, int w) {
    for (int k = 0; k < (w + 31) / 32; k++) d[k] = 0;
    for (int k = 0; k < w; k++) svPutBitselBit(d, k, svGetBitselBit(s, i + k));
}
void svGetPartselLogic(svLogicVecVal* d, const svLogicVecVal* s, int i, int w) {
    for (int k = 0; k < (w + 31) / 32; k++) d[k].aval = d[k].bval = 0;
    for (int k = 0; k < w; k++) svPutBitselLogic(d, k, svGetBitselLogic(s, i + k));
}
void svPutPartselBit(svBitVecVal* d, const svBitVecVal s, int i, int w) {
    for (int k = 0; k < w && k < 32; k++) svPutBitselBit(d, i + k, (s >> k) & 1);
}
void svPutPartselLogic(svLogicVecVal* d, const svLogicVecVal s, int i, int w) {
    for (int k = 0; k < w && k < 32; k++) svPutBitselLogic(d, i + k, svGetBitselLogic(&s, k));
}

static const dpi::OpenArr* oa(const svOpenArrayHandle h) { return (const dpi::OpenArr*)h; }
// Dimension 0 is the element's packed range [w-1:0]; 1.. are unpacked.
static std::pair<int64_t, int64_t> dimOf(const svOpenArrayHandle h, int d) {
    if (d == 0) return {oa(h)->w - 1, 0};
    if (d < 1 || d > (int)oa(h)->dims.size()) return {0, 0};
    return oa(h)->dims[(size_t)d - 1];
}
int svLeft(const svOpenArrayHandle h, int d) { return (int)dimOf(h, d).first; }
int svRight(const svOpenArrayHandle h, int d) { return (int)dimOf(h, d).second; }
int svLow(const svOpenArrayHandle h, int d) { auto p = dimOf(h, d); return (int)std::min(p.first, p.second); }
int svHigh(const svOpenArrayHandle h, int d) { auto p = dimOf(h, d); return (int)std::max(p.first, p.second); }
int svIncrement(const svOpenArrayHandle h, int d) { auto p = dimOf(h, d); return p.first >= p.second ? 1 : -1; }
int svSize(const svOpenArrayHandle h, int d) { auto p = dimOf(h, d); return (int)(std::max(p.first, p.second) - std::min(p.first, p.second) + 1); }
int svDimensions(const svOpenArrayHandle h) { return (int)oa(h)->dims.size(); }
void* svGetArrayPtr(const svOpenArrayHandle h) { return oa(h)->data; }
int svSizeOfArray(const svOpenArrayHandle h) { return (int)(oa(h)->count * oa(h)->elemSize); }

static void* elemPtr(const svOpenArrayHandle h, const int* idx) {
    int64_t off = oa(h)->offset(idx);
    return off < 0 ? nullptr : oa(h)->data + off;
}
static void* elemPtrV(const svOpenArrayHandle h, int i1, va_list ap) {
    int idx[16] = {i1};
    size_t n = oa(h)->dims.size();
    for (size_t k = 1; k < n && k < 16; k++) idx[k] = va_arg(ap, int);
    return elemPtr(h, idx);
}
void* svGetArrElemPtr(const svOpenArrayHandle h, int indx1, ...) {
    va_list ap; va_start(ap, indx1);
    void* p = elemPtrV(h, indx1, ap);
    va_end(ap);
    return p;
}
void* svGetArrElemPtr1(const svOpenArrayHandle h, int i1) { int x[1] = {i1}; return elemPtr(h, x); }
void* svGetArrElemPtr2(const svOpenArrayHandle h, int i1, int i2) { int x[2] = {i1, i2}; return elemPtr(h, x); }
void* svGetArrElemPtr3(const svOpenArrayHandle h, int i1, int i2, int i3) { int x[3] = {i1, i2, i3}; return elemPtr(h, x); }

// Element copies: a bit vector element is nwords words, a logic one nwords pairs.
static void putVec(const svOpenArrayHandle h, const void* s, void* p, bool logic) {
    if (!p) return;
    int n = dpi::nwords(oa(h)->w);
    if (oa(h)->t == DT_BIT || oa(h)->t == DT_LOGIC) {   // scalar elements
        uint8_t b = logic ? svGetBitselLogic((const svLogicVecVal*)s, 0) : svGetBitselBit((const svBitVecVal*)s, 0);
        *(uint8_t*)p = b;
        return;
    }
    memcpy(p, s, (size_t)n * (logic ? 8 : 4));
}
static void getVec(void* d, const svOpenArrayHandle h, const void* p, bool logic) {
    if (!p) return;
    int n = dpi::nwords(oa(h)->w);
    if (oa(h)->t == DT_BIT || oa(h)->t == DT_LOGIC) {
        uint8_t b = *(const uint8_t*)p;
        if (logic) { ((svLogicVecVal*)d)->aval = 0; ((svLogicVecVal*)d)->bval = 0; svPutBitselLogic((svLogicVecVal*)d, 0, b); }
        else *(svBitVecVal*)d = b & 1;
        return;
    }
    memcpy(d, p, (size_t)n * (logic ? 8 : 4));
}
#define MSIM_VA(h, i1, body) va_list ap; va_start(ap, i1); void* p = elemPtrV(h, i1, ap); va_end(ap); body
void svPutBitArrElemVecVal(const svOpenArrayHandle d, const svBitVecVal* s, int i1, ...) { MSIM_VA(d, i1, putVec(d, s, p, false);) }
void svPutBitArrElem1VecVal(const svOpenArrayHandle d, const svBitVecVal* s, int i1) { putVec(d, s, svGetArrElemPtr1(d, i1), false); }
void svPutBitArrElem2VecVal(const svOpenArrayHandle d, const svBitVecVal* s, int i1, int i2) { putVec(d, s, svGetArrElemPtr2(d, i1, i2), false); }
void svPutBitArrElem3VecVal(const svOpenArrayHandle d, const svBitVecVal* s, int i1, int i2, int i3) { putVec(d, s, svGetArrElemPtr3(d, i1, i2, i3), false); }
void svPutLogicArrElemVecVal(const svOpenArrayHandle d, const svLogicVecVal* s, int i1, ...) { MSIM_VA(d, i1, putVec(d, s, p, true);) }
void svPutLogicArrElem1VecVal(const svOpenArrayHandle d, const svLogicVecVal* s, int i1) { putVec(d, s, svGetArrElemPtr1(d, i1), true); }
void svPutLogicArrElem2VecVal(const svOpenArrayHandle d, const svLogicVecVal* s, int i1, int i2) { putVec(d, s, svGetArrElemPtr2(d, i1, i2), true); }
void svPutLogicArrElem3VecVal(const svOpenArrayHandle d, const svLogicVecVal* s, int i1, int i2, int i3) { putVec(d, s, svGetArrElemPtr3(d, i1, i2, i3), true); }
void svGetBitArrElemVecVal(svBitVecVal* d, const svOpenArrayHandle s, int i1, ...) { MSIM_VA(s, i1, getVec(d, s, p, false);) }
void svGetBitArrElem1VecVal(svBitVecVal* d, const svOpenArrayHandle s, int i1) { getVec(d, s, svGetArrElemPtr1(s, i1), false); }
void svGetBitArrElem2VecVal(svBitVecVal* d, const svOpenArrayHandle s, int i1, int i2) { getVec(d, s, svGetArrElemPtr2(s, i1, i2), false); }
void svGetBitArrElem3VecVal(svBitVecVal* d, const svOpenArrayHandle s, int i1, int i2, int i3) { getVec(d, s, svGetArrElemPtr3(s, i1, i2, i3), false); }
void svGetLogicArrElemVecVal(svLogicVecVal* d, const svOpenArrayHandle s, int i1, ...) { MSIM_VA(s, i1, getVec(d, s, p, true);) }
void svGetLogicArrElem1VecVal(svLogicVecVal* d, const svOpenArrayHandle s, int i1) { getVec(d, s, svGetArrElemPtr1(s, i1), true); }
void svGetLogicArrElem2VecVal(svLogicVecVal* d, const svOpenArrayHandle s, int i1, int i2) { getVec(d, s, svGetArrElemPtr2(s, i1, i2), true); }
void svGetLogicArrElem3VecVal(svLogicVecVal* d, const svOpenArrayHandle s, int i1, int i2, int i3) { getVec(d, s, svGetArrElemPtr3(s, i1, i2, i3), true); }

static svBit getBit(const svOpenArrayHandle s, void* p) { svBitVecVal v = 0; getVec(&v, s, p, false); return v & 1; }
static svLogic getLogic(const svOpenArrayHandle s, void* p) { svLogicVecVal v{0, 0}; getVec(&v, s, p, true); return svGetBitselLogic(&v, 0); }
static void putBit(const svOpenArrayHandle d, svBit b, void* p) { svBitVecVal v = b & 1; putVec(d, &v, p, false); }
static void putLogic(const svOpenArrayHandle d, svLogic b, void* p) { svLogicVecVal v{0, 0}; svPutBitselLogic(&v, 0, b); putVec(d, &v, p, true); }
svBit svGetBitArrElem(const svOpenArrayHandle s, int i1, ...) { MSIM_VA(s, i1, return getBit(s, p);) }
svBit svGetBitArrElem1(const svOpenArrayHandle s, int i1) { return getBit(s, svGetArrElemPtr1(s, i1)); }
svBit svGetBitArrElem2(const svOpenArrayHandle s, int i1, int i2) { return getBit(s, svGetArrElemPtr2(s, i1, i2)); }
svBit svGetBitArrElem3(const svOpenArrayHandle s, int i1, int i2, int i3) { return getBit(s, svGetArrElemPtr3(s, i1, i2, i3)); }
svLogic svGetLogicArrElem(const svOpenArrayHandle s, int i1, ...) { MSIM_VA(s, i1, return getLogic(s, p);) }
svLogic svGetLogicArrElem1(const svOpenArrayHandle s, int i1) { return getLogic(s, svGetArrElemPtr1(s, i1)); }
svLogic svGetLogicArrElem2(const svOpenArrayHandle s, int i1, int i2) { return getLogic(s, svGetArrElemPtr2(s, i1, i2)); }
svLogic svGetLogicArrElem3(const svOpenArrayHandle s, int i1, int i2, int i3) { return getLogic(s, svGetArrElemPtr3(s, i1, i2, i3)); }
void svPutBitArrElem(const svOpenArrayHandle d, svBit v, int i1, ...) { MSIM_VA(d, i1, putBit(d, v, p);) }
void svPutBitArrElem1(const svOpenArrayHandle d, svBit v, int i1) { putBit(d, v, svGetArrElemPtr1(d, i1)); }
void svPutBitArrElem2(const svOpenArrayHandle d, svBit v, int i1, int i2) { putBit(d, v, svGetArrElemPtr2(d, i1, i2)); }
void svPutBitArrElem3(const svOpenArrayHandle d, svBit v, int i1, int i2, int i3) { putBit(d, v, svGetArrElemPtr3(d, i1, i2, i3)); }
void svPutLogicArrElem(const svOpenArrayHandle d, svLogic v, int i1, ...) { MSIM_VA(d, i1, putLogic(d, v, p);) }
void svPutLogicArrElem1(const svOpenArrayHandle d, svLogic v, int i1) { putLogic(d, v, svGetArrElemPtr1(d, i1)); }
void svPutLogicArrElem2(const svOpenArrayHandle d, svLogic v, int i1, int i2) { putLogic(d, v, svGetArrElemPtr2(d, i1, i2)); }
void svPutLogicArrElem3(const svOpenArrayHandle d, svLogic v, int i1, int i2, int i3) { putLogic(d, v, svGetArrElemPtr3(d, i1, i2, i3)); }
#undef MSIM_VA

svScope svGetScope(void) { return dpi::st().scope; }
svScope svSetScope(const svScope scope) {
    svScope old = dpi::st().scope;
    dpi::st().scope = (dpi::Scope*)scope;
    return old;
}
// A scope inside protected IP gives a fixed, meaningless name (IEEE 1735
// 9.7.2.3). svGetScopeFromName still finds it by its disclosed name, so an
// import can set up its context and keep user data there.
const char* svGetNameFromScope(const svScope s) {
    if (!s) return nullptr;
    const dpi::Scope* sc = (const dpi::Scope*)s;
    return sc->prot ? "<protected>" : sc->name.c_str();
}
svScope svGetScopeFromName(const char* name) {
    if (!name) return nullptr;
    auto& m = dpi::st().scopes;
    auto it = m.find(name);
    return it == m.end() ? nullptr : it->second.get();
}
int svPutUserData(const svScope scope, void* key, void* data) {
    if (!scope) return -1;
    ((dpi::Scope*)scope)->user[key] = data;
    return 0;
}
void* svGetUserData(const svScope scope, void* key) {
    if (!scope) return nullptr;
    auto& u = ((dpi::Scope*)scope)->user;
    auto it = u.find(key);
    return it == u.end() ? nullptr : it->second;
}
int svGetCallerInfo(const char** fileName, int* lineNumber) {
    const Instr* c = dpi::st().call;
    if (!c) return 0;
    if (fileName) *fileName = c->fmt.c_str();
    if (lineNumber) *lineNumber = c->dline;
    return 1;
}
int svIsDisabledState(void) { return dpi::st().fiber && dpi::st().fiber->disabled ? 1 : 0; }
void svAckDisabledState(void) { if (dpi::st().fiber) dpi::st().fiber->acked = true; }
}  // extern "C"

namespace dpi {

// ---- set-up: libraries, symbols, export stubs ----
inline std::string cType(DpiType t, bool ptr, bool isConst, bool sgn = true) {
    std::string b;
    // byte unsigned is unsigned char, and so on (H.7.4)
    if (!sgn && (t == DT_BYTE || t == DT_SHORT || t == DT_INT || t == DT_LONG)) {
        b = t == DT_BYTE ? "unsigned char" : t == DT_SHORT ? "unsigned short" : t == DT_INT ? "unsigned int" : "unsigned long long";
        return ptr ? std::string(isConst ? "const " : "") + b + "*" : b;
    }
    switch (t) {
        case DT_VOID: return "void";
        case DT_BYTE: b = "char"; break;
        case DT_SHORT: b = "short"; break;
        case DT_INT: b = "int"; break;
        case DT_LONG: b = "long long"; break;
        case DT_REAL: b = "double"; break;
        case DT_SHORTREAL: b = "float"; break;
        case DT_CHANDLE: b = "void*"; break;
        case DT_STRING: b = "const char*"; break;
        case DT_BIT: b = "svBit"; break;
        case DT_LOGIC: b = "svLogic"; break;
        case DT_BITVEC: return std::string(isConst ? "const " : "") + "svBitVecVal*";
        case DT_LOGICVEC: return std::string(isConst ? "const " : "") + "svLogicVecVal*";
        case DT_STRUCT: b = "void"; break;
    }
    if (!ptr) return b;
    return std::string(isConst ? "const " : "") + b + "*";
}
// The C prototype of an import or export (35.5.6): what -dpiheader writes
// and what an export stub defines.
inline std::string cArgType(const DpiArg& a) {
    if (a.t == DT_STRUCT) return std::string(a.dir == 0 ? "const " : "") + a.cName + "*";
    if (a.uarr && a.open) return a.dir == 0 ? "const svOpenArrayHandle" : "svOpenArrayHandle";
    if (a.uarr) return a.t == DT_STRING ? "const char**" : cType(a.t, true, a.dir == 0, a.sgn);
    if (a.dir == 0) return cType(a.t, false, true, a.sgn);
    return cType(a.t, true, false, a.sgn);
}
inline std::string cRetType(const DpiFunc& f) { return f.task ? "int" : cType(f.ret, false, true, f.retSgn); }
inline std::string prototype(const DpiFunc& f) {
    std::string s = cRetType(f) + " " + f.cName + "(";
    for (size_t k = 0; k < f.args.size(); k++) {
        if (k) s += ", ";
        s += cArgType(f.args[k]) + " " + (f.args[k].name.empty() ? "a" + std::to_string(k) : f.args[k].name);
    }
    if (f.args.empty()) s += "void";
    return s + ")";
}
inline std::string header(const std::vector<std::unique_ptr<DpiFunc>>& all) {
    std::string h = "/* DPI-C prototypes written by msim -dpiheader (IEEE 1800-2023 35.5.6). */\n"
                    "#ifndef MSIM_DPI_HEADER\n#define MSIM_DPI_HEADER\n#include \"svdpi.h\"\n"
                    "#ifdef __cplusplus\nextern \"C\" {\n#endif\n";
    std::set<std::string> seen;
    for (int pass = 0; pass < 2; pass++) {
        h += pass ? "\n/* exports: SystemVerilog subroutines C can call */\n" : "\n/* imports: C functions SystemVerilog calls */\n";
        for (auto& f : all)
            if (f->isExport == (pass == 1) && seen.insert(f->cName).second) h += "extern " + prototype(*f) + ";\n";
    }
    return h + "\n#ifdef __cplusplus\n}\n#endif\n#endif\n";
}
inline std::string exportStubs(const std::vector<const DpiFunc*>& ex) {
    std::string s = "#include \"svdpi.h\"\nextern int msim_dpi_export(int id, void** args, void* ret);\n";
    for (size_t id = 0; id < ex.size(); id++) {
        const DpiFunc& f = *ex[id];
        s += prototype(f) + " {\n    void* args[" + std::to_string(f.args.size() + 1) + "] = {";
        for (size_t k = 0; k < f.args.size(); k++)
            s += "(void*)&" + (f.args[k].name.empty() ? "a" + std::to_string(k) : f.args[k].name) + ", ";
        s += "0};\n";
        std::string rt = cRetType(f);
        if (rt == "void") s += "    msim_dpi_export(" + std::to_string(id) + ", args, 0);\n}\n";
        else s += "    " + rt + " msim_ret_ = 0;\n    msim_dpi_export(" + std::to_string(id) + ", args, (void*)&msim_ret_);\n    return msim_ret_;\n}\n";
    }
    return s;
}

// One stub per C name: the same export in several instances is one C
// function, and the scope (svSetScope) picks the instance at run time.
inline void collectExports(const LowerResult& lr) {
    st().exports.clear();
    st().exportsByName.clear();
    for (auto& f : lr.dpi) {
        if (!f->isExport) continue;
        auto& v = st().exportsByName[f->cName];
        if (v.empty()) st().exports.push_back(f.get());
        v.push_back(f.get());
    }
}

// Load the libraries and resolve every import. A symbol is looked up in the
// libraries in order, then in msim and the C library (so an import of a libc
// function such as abs needs no library).
inline bool setup(const LowerResult& lr, const std::vector<std::string>& libs, std::string& err) {
    for (auto& sc : lr.scopes) scopeOf(sc);
    for (auto& sc : lr.protScopes) scopeOf(sc)->prot = true;
    for (auto& path : libs) {
        void* h = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (!h) { err = std::string("cannot load ") + path + ": " + dlerror(); return false; }
        st().libs.push_back(h);
    }
    for (auto& fp : lr.dpi) {
        DpiFunc& f = *fp;
        scopeOf(f.scope);
        if (f.isExport) continue;
        auto imp = new Import;
        void* sym = nullptr;
        for (void* h : st().libs) if ((sym = dlsym(h, f.cName.c_str()))) break;
        if (!sym) sym = dlsym(RTLD_DEFAULT, f.cName.c_str());
        if (!sym) {
            err = "import \"" + f.svName + "\": no C function \"" + f.cName + "\" in " +
                  (libs.empty() ? std::string("msim or the C library (give the C file, or -sv_lib)") : "the -sv_lib libraries or the C library");
            return false;
        }
        imp->fn = (void (*)())sym;
        for (auto& a : f.args)
            imp->types.push_back(a.uarr || a.dir != 0 || a.t == DT_BITVEC || a.t == DT_LOGICVEC || a.t == DT_STRING ||
                                         a.t == DT_CHANDLE ? &ffi_type_pointer : ffiOf(a.t));
        imp->ret = f.task ? &ffi_type_sint32 : ffiOf(f.ret);
        if (ffi_prep_cif(&imp->cif, FFI_DEFAULT_ABI, (unsigned)imp->types.size(), imp->ret, imp->types.data()) != FFI_OK) {
            err = "import \"" + f.svName + "\": cannot build its call";
            return false;
        }
        f.rt = imp;
    }
    dpiHook().call = call;
    dpiHook().resume = resume;
    dpiHook().kill = kill;
    return true;
}

}  // namespace dpi
