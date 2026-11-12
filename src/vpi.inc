// vpi.inc - VPI run time (IEEE 1800-2023 clauses 36-38). #include in msim.cpp
// after dpi.inc. A VPI library is loaded before elaboration, and its
// vlog_startup_routines register user system tasks and functions and
// callbacks. msim is linked with -rdynamic, so the library finds the vpi_*
// functions in msim itself.
//
// Objects are built on demand from the slang AST (modules, nets, variables,
// parameters, ports) and the lowered design (call sites, signals). A handle
// to a design object is canonical: asking twice gives the same pointer, so
// vpi_compare_objects is a pointer compare. Only iterators are freed.
//
// VPI runs the simulation on one thread, in the interpreter: callbacks and
// calltf routines may read or write any signal at any time.

#include <dlfcn.h>

#include <cstdarg>
#include <cmath>

#include "../include/sv_vpi_user.h"
#include "version.hpp"
#include "slang/ast/ASTContext.h"
#include "slang/ast/SystemSubroutine.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/syntax/AllSyntax.h"

namespace vpi {
using namespace slang::ast;

struct Cb;
// One VPI object. `kind` says which fields are used.
struct Obj {
    enum Kind { Systf, Call, Module, Signal, Word, Bit, Arg, Param, Port, Iter, Callback, Event } kind;
    int type = 0;                         // the vpi object type it reports
    int idx = -1;                         // Systf: systf; Arg: argument; Word / Bit: element index
    const VpiSite* site = nullptr;        // Call, Arg
    const Symbol* sym = nullptr;          // Module: InstanceSymbol; Signal / Param / Port / Word: the symbol
    const Obj* parent = nullptr;          // Word, Bit: the memory or vector; Arg: the call
    int sig = -1, w = 0;                  // the signal and its width (Word: element, Bit: the vector's signal)
    int off = 0;                          // Bit: bit offset in the vector (a part select: its lowest bit)
    int lidx = 0;                         // a part select v[idx:lidx]: its right index
    bool sgn = false, real = false, str = false;
    std::vector<vpiHandle> items;         // Iter
    size_t pos = 0;
    mutable int8_t prot = -1;             // vpiIsProtected, once worked out (-1: not yet)
    Cb* cb = nullptr;                     // Callback
};
struct Cb {
    s_cb_data d{};
    s_vpi_time t{};
    s_vpi_value v{};
    bool hasT = false, hasV = false, live = true;
    uint64_t at = 0;                      // timed callbacks: when; cbNextSimTime: when registered
    Obj* obj = nullptr;                   // cbValueChange: the object watched
    Obj* self = nullptr;                  // its handle
};
struct Systf {
    s_vpi_systf_data d{};
    std::string name;
};

struct State {
    bool active = false;                  // a library registered something: VPI is in use
    std::vector<Systf> systfs;
    std::vector<std::unique_ptr<Obj>> objs;
    std::unordered_map<const Symbol*, Obj*> symObj;
    std::unordered_map<const VpiSite*, Obj*> callObj;
    std::map<std::pair<const Obj*, int>, Obj*> subObj;   // memory words and bits
    std::unordered_map<int, Obj*> sysObj;
    const VpiSite* cur = nullptr;         // the call whose calltf / compiletf is running
    std::vector<Cb*> start, endCompile, endSim, rw, ro, next;
    std::multimap<uint64_t, Cb*> timed;   // cbAfterDelay, cbAtStartOfSimTime, and delayed synch callbacks
    std::unordered_map<int, std::vector<Cb*>> change;   // signal -> its cbValueChange callbacks
    std::vector<std::unique_ptr<Cb>> cbs;
    std::multimap<uint64_t, std::pair<int, Val>> puts;   // delayed vpi_put_value writes
    int mode = vpiMode1800v2009;          // the compatibility mode of the running call (36.12)
    std::vector<char> arrBuf;             // vpi_get_value_array without vpiUserAllocFlag
    Compilation* comp = nullptr;
    const LowerResult* lr = nullptr;
    std::vector<std::string> argv;
    std::vector<char*> argvp;
    // Strings and vectors handed to C live until the next call that returns one (38.18).
    std::string strBuf, valBuf;   // vpi_get_str, vpi_get_value
    std::vector<s_vpi_vecval> vecBuf;
    s_vpi_time timeBuf{};
    // The error of the last VPI call (38.4).
    bool err = false;
    int errLevel = 0;
    std::string errMsg, errFile;
    int errLine = 0;
    std::vector<std::FILE*> mcd = std::vector<std::FILE*>(32, nullptr);
    std::vector<std::string> mcdName = std::vector<std::string>(32);
};
inline State& st() { static State s; return s; }

inline vpiHandle H(Obj* o) { return reinterpret_cast<vpiHandle>(o); }
inline Obj* O(vpiHandle h) { return reinterpret_cast<Obj*>(h); }
inline Obj* mkObj(Obj::Kind k, int type) {
    st().objs.push_back(std::make_unique<Obj>());
    Obj* o = st().objs.back().get();
    o->kind = k;
    o->type = type;
    return o;
}
inline void clearErr() { st().err = false; }
inline void error(int level, const std::string& msg) {
    auto& s = st();
    s.err = true;
    s.errLevel = level;
    s.errMsg = msg;
    s.errFile.clear();
    s.errLine = 0;
}
inline char* retStr(std::string s) {
    st().strBuf = std::move(s);
    return st().strBuf.data();
}
inline char* retVal(std::string s) {
    st().valBuf = std::move(s);
    return st().valBuf.data();
}

// ---- design objects ----
inline const InstanceSymbol* instOf(const Scope* sc) {
    for (; sc; sc = sc->asSymbol().getParentScope()) {
        const Symbol& sy = sc->asSymbol();
        if (sy.kind == SymbolKind::InstanceBody) return sy.as<InstanceBodySymbol>().parentInstance;
    }
    return nullptr;
}
inline Obj* moduleObj(const InstanceSymbol* inst) {
    if (!inst) return nullptr;
    auto& m = st().symObj[inst];
    if (!m) {
        m = mkObj(Obj::Module, vpiModule);
        m->sym = inst;
    }
    return m;
}
inline int sigOfSym(const Symbol* s) {
    auto it = st().lr->sigOf.find(s);
    return it == st().lr->sigOf.end() ? -1 : it->second;
}
// A variable or net as a VPI object, or null if VPI does not model it.
inline Obj* valueObj(const Symbol* s) {
    if (auto it = st().symObj.find(s); it != st().symObj.end()) return it->second;
    const Type& t = s->as<ValueSymbol>().getType().getCanonicalType();
    Obj* o = nullptr;
    if (t.isUnpackedArray()) {
        auto am = arrMap().find(s);
        if (am == arrMap().end() || am->second->dims.size() != 1) return nullptr;
        o = mkObj(Obj::Signal, vpiMemory);
        o->sig = am->second->base;
        o->w = am->second->elemW;
        o->sgn = am->second->sgn;
    } else {
        int sg = sigOfSym(s);
        if (sg < 0) return nullptr;
        bool net = s->kind == SymbolKind::Net;
        int type = vpiReg;
        if (net) type = vpiNet;
        else if (t.isFloating()) type = vpiRealVar;
        else if (t.isString()) return nullptr;
        else if (!t.isIntegral()) return nullptr;
        else if (t.isPredefinedInteger() && t.as<PredefinedIntegerType>().integerKind == PredefinedIntegerType::Integer)
            type = vpiIntegerVar;
        else if (t.isPredefinedInteger() && t.as<PredefinedIntegerType>().integerKind == PredefinedIntegerType::Time)
            type = vpiTimeVar;
        o = mkObj(Obj::Signal, type);
        o->sig = sg;
        o->w = t.isFloating() ? 64 : (int)t.getBitWidth();
        o->sgn = t.isSigned();
        o->real = t.isFloating();
    }
    o->sym = s;
    st().symObj[s] = o;
    return o;
}
// ---- protected objects (37.3.6; IEEE 1735-2023 9.7.1) ----
// An object is protected when it represents code in a decryption envelope
// that the IP author's rights and viewports do not make visible, or when it
// sits below an instance of protected code (a path through protected IP is
// opaque). For a protected object only vpiType and vpiIsProtected may be
// read; every other property and relationship is an error, no callback may
// be placed on it, and vpi_handle_by_name does not find it. Two exceptions
// keep a user system task working when the IP itself calls it (model
// fidelity, IEEE 1735 9.2.2): the arguments of a protected call can be
// iterated and their vpiSize read (38.36, 37.57), and while that call's
// calltf or compiletf runs its arguments' values can be read and written.
inline bool protSym(const Symbol* s) {
    if (!s || !hideMsgIf()) return false;
    if (s->kind == SymbolKind::Port) {
        const auto& p = s->as<PortSymbol>();
        if (p.internalSymbol) s = p.internalSymbol;
    }
    if (hideSigIf() && hideSigIf()(*s)) return true;
    // Declared in clear text, but inside a protected instance.
    return !hideMsgIf()(s->location) && protectedInstance(instOf(s->getParentScope()));
}
inline bool isProt(const Obj* o) {
    if (!o) return false;
    if (o->prot >= 0) return o->prot;
    bool p = false;
    switch (o->kind) {
        case Obj::Module: p = protectedInstance(&o->sym->as<InstanceSymbol>()); break;
        case Obj::Signal:
        case Obj::Param:
        case Obj::Port: p = protSym(o->sym); break;
        case Obj::Word:
        case Obj::Bit: p = isProt(o->parent); break;
        case Obj::Call:
        case Obj::Arg: p = o->site && o->site->prot; break;
        default: break;
    }
    o->prot = p;
    return p;
}
// Is `o` an argument (or the result) of the protected call whose calltf or
// compiletf is running? Its value may then be read and written.
inline bool inOwnCall(const Obj* o) {
    const VpiSite* cur = st().cur;
    if (!cur || !cur->prot) return false;
    if (o->kind == Obj::Word || o->kind == Obj::Bit) o = o->parent;
    if (o->kind == Obj::Call || o->kind == Obj::Arg) return o->site == cur;
    for (const VpiArg& a : cur->args)
        if ((a.kind == VpiArg::Signal || a.kind == VpiArg::Scope) && a.sym == o->sym) return true;
    return false;
}
inline bool denied(const Obj* o, const char* fn) {
    if (!isProt(o)) return false;
    error(vpiError, std::string(fn) + ": the object is protected");
    return true;
}

// The full name of a design object (38.1.2 vpiFullName).
inline std::string fullName(const Symbol& s) {
    return s.getHierarchicalPath();
}
// A range [left:right] of a vector signal, for bit indices.
inline std::pair<int64_t, int64_t> rangeOf(const Obj* o) {
    if (o->sym && o->kind == Obj::Signal) {
        const Type& t = o->sym->as<ValueSymbol>().getType().getCanonicalType();
        if (t.isIntegral() && !t.isUnpackedArray()) {
            auto r = t.getFixedRange();
            return {r.left, r.right};
        }
    }
    return {o->w - 1, 0};
}
inline Obj* bitObj(const Obj* vec, int64_t i) {
    auto [l, r] = rangeOf(vec);
    int64_t off = l >= r ? i - r : r - i;
    if (off < 0 || off >= vec->w) return nullptr;
    auto& b = st().subObj[{vec, (int)off}];
    if (!b) {
        b = mkObj(Obj::Bit, vec->type == vpiNet ? vpiNetBit : vpiRegBit);
        b->parent = vec;
        b->sig = vec->sig;
        b->off = (int)off;
        b->idx = (int)i;
        b->w = 1;
    }
    return b;
}
// v[a:b] (a vpiPartSelect, 38.27): bits a down to b of a vector, by its declared range.
inline Obj* partObj(const Obj* vec, int64_t a, int64_t b) {
    auto [l, r] = rangeOf(vec);
    int64_t oa = l >= r ? a - r : r - a, ob = l >= r ? b - r : r - b;
    int64_t lo = std::min(oa, ob), hi = std::max(oa, ob);
    if (lo < 0 || hi >= vec->w) return nullptr;
    if (lo == hi) return bitObj(vec, a);
    auto& b2 = st().subObj[{vec, (int)(lo + ((hi + 1) << 20))}];
    if (!b2) {
        b2 = mkObj(Obj::Bit, vpiPartSelect);
        b2->parent = vec;
        b2->sig = vec->sig;
        b2->off = (int)lo;
        b2->idx = (int)a;
        b2->lidx = (int)b;
        b2->w = (int)(hi - lo + 1);
    }
    return b2;
}
inline Obj* wordObj(const Obj* mem, int64_t i) {
    auto am = arrMap().find(mem->sym);
    if (am == arrMap().end()) return nullptr;
    auto [l, r] = am->second->dims[0];
    int64_t off = l <= r ? i - l : l - i;
    if (off < 0 || off >= am->second->count) return nullptr;
    auto& b = st().subObj[{mem, (int)off}];
    if (!b) {
        b = mkObj(Obj::Word, vpiMemoryWord);
        b->parent = mem;
        b->sym = mem->sym;
        b->sig = mem->sig + (int)off;
        b->w = mem->w;
        b->sgn = mem->sgn;
        b->idx = (int)i;
    }
    return b;
}
inline Obj* callObj(const VpiSite* site) {
    auto& c = st().callObj[site];
    if (!c) {
        c = mkObj(Obj::Call, site->func ? vpiSysFuncCall : vpiSysTaskCall);
        c->site = site;
        c->sig = site->ret;
        c->w = site->retW;
        c->sgn = site->retSgn;
        c->real = site->retReal;
    }
    return c;
}
inline Obj* argObj(const VpiSite* site, int k) {
    const VpiArg& a = site->args[(size_t)k];
    if (a.kind == VpiArg::Signal) return valueObj(static_cast<const Symbol*>(a.sym));
    if (a.kind == VpiArg::Scope) return moduleObj(&static_cast<const Symbol*>(a.sym)->as<InstanceSymbol>());
    Obj* call = callObj(site);
    auto& o = st().subObj[{call, k}];
    if (!o) {
        o = mkObj(Obj::Arg, a.kind == VpiArg::Empty ? vpiOperation : a.vtype ? a.vtype : vpiOperation);
        o->site = site;
        o->parent = call;
        o->idx = k;
        o->sig = a.sig;
        o->w = a.kind == VpiArg::String ? (int)a.text.size() * 8 : a.w;
        o->sgn = a.sgn;
        o->real = a.real;
        o->str = a.str || a.kind == VpiArg::String;
    }
    return o;
}
inline vpiHandle iter(std::vector<vpiHandle> items) {
    if (items.empty()) return nullptr;
    Obj* it = mkObj(Obj::Iter, vpiIterator);
    it->items = std::move(items);
    return H(it);
}
// The members of a module, looking through generate blocks that are
// instantiated (their contents are part of the module's hierarchy here).
template <typename F> void eachMember(const Scope& sc, F&& f) {
    for (const Symbol& m : sc.members()) {
        if (m.kind == SymbolKind::GenerateBlock) {
            if (!m.as<GenerateBlockSymbol>().isUninstantiated) eachMember(m.as<GenerateBlockSymbol>(), f);
        } else if (m.kind == SymbolKind::GenerateBlockArray) {
            for (const auto* e : m.as<GenerateBlockArraySymbol>().entries)
                if (!e->isUninstantiated) eachMember(*e, f);
        } else if (m.kind == SymbolKind::InstanceArray) {
            std::function<void(const InstanceArraySymbol&)> rec = [&](const InstanceArraySymbol& a) {
                for (const Symbol* e : a.elements) {
                    if (e->kind == SymbolKind::InstanceArray) rec(e->as<InstanceArraySymbol>());
                    else f(*e);
                }
            };
            rec(m.as<InstanceArraySymbol>());
        } else {
            f(m);
        }
    }
}

// ---- values ----
inline int64_t sigTicks(const p_vpi_time t, const Obj* o);
inline Val readObj(const Obj* o) {
    if (o->kind == Obj::Arg && o->site->args[(size_t)o->idx].kind == VpiArg::String) return mkstr(o->site->args[(size_t)o->idx].text);
    if (o->kind == Obj::Arg && o->site->args[(size_t)o->idx].kind == VpiArg::Empty) return make(0, ~0ULL, 1);
    if (o->kind == Obj::Bit) {   // a bit or a part select
        Val v = S.get(o->sig);
        return v_slice(v, o->off + o->w - 1, o->off);
    }
    if (o->sig < 0) return make(0, ~0ULL, std::max(1, o->w));
    Val v = S.get(o->sig);
    if (!o->real && !o->str && v.w != o->w) v = maskWidth(v, o->w);
    v.w = o->real || o->str ? 64 : o->w;
    return v;
}
inline bool hasXZ(const Val& v, int w) {
    for (int i = 0; i < w; i++) if (bitAt(v, i) >= 2) return true;
    return false;
}
inline double toDouble(const Obj* o, const Val& v) {
    if (o->real) { double d; uint64_t b = v.v; std::memcpy(&d, &b, 8); return d; }
    if (hasXZ(v, o->w)) return 0;
    double r = 0;
    for (int i = o->w - 1; i >= 0; i--) r = r * 2 + (bitAt(v, i) == 1);
    if (o->sgn && o->w > 0 && bitAt(v, o->w - 1) == 1) r -= std::ldexp(1.0, o->w);
    return r;
}
// Digit c of a radix-2^k string: '0'-'f', or x/X/z/Z for unknown bits (all x: x, some: X).
inline std::string radixStr(const Val& v, int w, int k) {
    std::string r;
    int n = (w + k - 1) / k;
    for (int d = n - 1; d >= 0; d--) {
        int val = 0, nx = 0, nz = 0, cnt = 0;
        for (int b = 0; b < k && d * k + b < w; b++, cnt++) {
            int t = bitAt(v, d * k + b);
            if (t == 1) val |= 1 << b;
            else if (t == 2) nx++;
            else if (t == 3) nz++;
        }
        if (nx + nz == 0) r += "0123456789abcdef"[val];
        else if (nx == cnt) r += 'x';
        else if (nz == cnt) r += 'z';
        else if (nx) r += 'X';
        else r += 'Z';
    }
    return r;
}
inline std::string decStr(const Val& v, int w, bool sgn) {
    int nx = 0, nz = 0;
    for (int i = 0; i < w; i++) { int t = bitAt(v, i); nx += t == 2; nz += t == 3; }
    if (nx || nz) return nx == w ? "x" : nz == w ? "z" : nx ? "X" : "Z";
    std::vector<uint32_t> mag((size_t)(w + 31) / 32, 0);
    for (int i = 0; i < w; i++) if (bitAt(v, i) == 1) mag[(size_t)i / 32] |= 1u << (i % 32);
    bool neg = sgn && w > 0 && bitAt(v, w - 1) == 1;
    if (neg) {   // two's complement magnitude
        uint64_t carry = 1;
        for (size_t i = 0; i < mag.size(); i++) {
            int bits = std::min(32, w - (int)i * 32);
            uint32_t m = bits >= 32 ? ~0u : ((1u << bits) - 1);
            uint64_t x = (uint64_t)(~mag[i] & m) + carry;
            mag[i] = (uint32_t)(x & m);
            carry = x >> bits;
        }
    }
    std::string r;
    for (;;) {
        bool zero = true;
        for (auto m : mag) if (m) zero = false;
        if (zero) break;
        uint64_t rem = 0;
        for (size_t i = mag.size(); i-- > 0;) {
            uint64_t cur = (rem << 32) | mag[i];
            mag[i] = (uint32_t)(cur / 10);
            rem = cur % 10;
        }
        r += (char)('0' + rem);
    }
    if (r.empty()) r = "0";
    if (neg) r += '-';
    return std::string(r.rbegin(), r.rend());
}
inline int defaultFormat(const Obj* o) {
    if (o->real) return vpiRealVal;
    if (o->str) return vpiStringVal;
    if (o->type == vpiIntegerVar) return vpiIntVal;
    if (o->w == 1) return vpiScalarVal;
    return vpiVectorVal;
}
// A string's text as bits, 8 per character, the last character lowest.
inline Val strBits(const std::string& t) {
    int w = std::max(8, (int)t.size() * 8);
    Val r = make(0, 0, w);
    if (w > 64) r = maskWidth(r, w);
    for (size_t k = 0; k < t.size(); k++) {
        unsigned char c = (unsigned char)t[t.size() - 1 - k];
        for (int b = 0; b < 8; b++) setBit(r, (int)k * 8 + b, (c >> b) & 1);
    }
    return r;
}
inline void getValueOf(const Obj* o, Val v, p_vpi_value vp) {
    if (vp->format == vpiObjTypeVal) vp->format = defaultFormat(o);
    std::string text;
    if (o->str) {
        text = v_str(v);
        if (vp->format != vpiStringVal) v = strBits(text);
    }
    int w = o->str ? std::max(8, (int)text.size() * 8) : o->w;
    switch (vp->format) {
        case vpiBinStrVal: vp->value.str = retVal(radixStr(v, o->real ? 64 : w, 1)); break;
        case vpiOctStrVal: vp->value.str = retVal(radixStr(v, w, 3)); break;
        case vpiHexStrVal: vp->value.str = retVal(radixStr(v, w, 4)); break;
        case vpiDecStrVal:
            if (o->real) {
                char b[64];
                std::snprintf(b, sizeof b, "%.0f", toDouble(o, v));
                vp->value.str = retVal(b);
            } else vp->value.str = retVal(decStr(v, w, o->sgn));
            break;
        case vpiScalarVal: {
            int t = bitAt(v, 0);
            vp->value.scalar = t == 0 ? vpi0 : t == 1 ? vpi1 : t == 3 ? vpiZ : vpiX;
            break;
        }
        case vpiIntVal: {
            if (o->real) { vp->value.integer = (PLI_INT32)std::llround(toDouble(o, v)); break; }
            uint32_t r = 0;
            for (int i = 0; i < 32; i++) {
                int t = i < w ? bitAt(v, i) : (o->sgn && w > 0 && bitAt(v, w - 1) == 1 ? 1 : 0);
                if (t == 1) r |= 1u << i;
            }
            vp->value.integer = (PLI_INT32)r;
            break;
        }
        case vpiRealVal: vp->value.real = o->str ? 0 : toDouble(o, v); break;
        case vpiStringVal: vp->value.str = retVal(o->str ? text : bytesToStr(v)); break;
        case vpiVectorVal: {
            int ww = o->real ? 64 : std::max(1, w);
            auto& vb = st().vecBuf;
            vb.assign((size_t)(ww + 31) / 32, s_vpi_vecval{0, 0});
            for (int i = 0; i < ww; i++) {
                int t = bitAt(v, i);
                uint32_t m = 1u << (i % 32);
                auto& e = vb[(size_t)i / 32];
                if (t == 1 || t == 2) e.aval |= m;
                if (t == 2 || t == 3) e.bval |= m;
            }
            vp->value.vector = vb.data();
            break;
        }
        case vpiTimeVal: {
            uint64_t t = v.v & ~v.x;
            st().timeBuf = s_vpi_time{vpiSimTime, (PLI_UINT32)(t >> 32), (PLI_UINT32)t, (double)t};
            vp->value.time = &st().timeBuf;
            break;
        }
        case vpiSuppressVal: break;
        default: error(vpiError, "vpi_get_value: unsupported format " + std::to_string(vp->format));
    }
}
inline void getValue(const Obj* o, p_vpi_value vp) { getValueOf(o, readObj(o), vp); }
// A value from C as a w-bit Val (or a real / string for those objects).
inline bool fromValue(const Obj* o, p_vpi_value vp, Val& out) {
    int w = o->w;
    auto bits = [&](const char* s, int k) {
        Val r = make(0, 0, w);
        int n = (int)std::strlen(s);
        for (int d = 0; d < n; d++) {
            char c = s[n - 1 - d];
            for (int b = 0; b < k; b++) {
                int i = d * k + b;
                if (i >= w) break;
                int t;
                if (c == 'x' || c == 'X') t = 2;
                else if (c == 'z' || c == 'Z' || c == '?') t = 3;
                else {
                    int dv = std::isdigit((unsigned char)c) ? c - '0' : std::tolower((unsigned char)c) - 'a' + 10;
                    t = (dv >> b) & 1;
                }
                setBit(r, i, t);
            }
        }
        return r;
    };
    auto fromInt = [&](int64_t x) {
        Val r = make(0, 0, w);
        for (int i = 0; i < w; i++) setBit(r, i, i < 64 ? (int)((x >> i) & 1) : (x < 0 ? 1 : 0));
        return r;
    };
    auto fromReal = [&](double d) {
        if (o->real) { uint64_t b; std::memcpy(&b, &d, 8); Val r = make(b, 0, 64); r.real = true; return r; }
        return fromInt((int64_t)std::llround(d));
    };
    switch (vp->format) {
        case vpiBinStrVal: out = bits(vp->value.str, 1); break;
        case vpiOctStrVal: out = bits(vp->value.str, 3); break;
        case vpiHexStrVal: out = bits(vp->value.str, 4); break;
        case vpiDecStrVal: {
            const char* s = vp->value.str;
            if (o->real) { out = fromReal(std::strtod(s, nullptr)); break; }
            bool neg = *s == '-';
            if (neg || *s == '+') s++;
            if (*s == 'x' || *s == 'X') { out = bits(std::string((size_t)w, 'x').c_str(), 1); break; }
            if (*s == 'z' || *s == 'Z') { out = bits(std::string((size_t)w, 'z').c_str(), 1); break; }
            std::vector<uint32_t> mag((size_t)(w + 31) / 32 + 1, 0);
            for (; *s; s++) {
                if (!std::isdigit((unsigned char)*s)) continue;
                uint64_t carry = (uint64_t)(*s - '0');
                for (auto& m : mag) {
                    uint64_t x = (uint64_t)m * 10 + carry;
                    m = (uint32_t)x;
                    carry = x >> 32;
                }
            }
            if (neg) {
                uint64_t carry = 1;
                for (auto& m : mag) { uint64_t x = (uint64_t)(uint32_t)~m + carry; m = (uint32_t)x; carry = x >> 32; }
            }
            out = make(0, 0, w);
            for (int i = 0; i < w; i++) setBit(out, i, (mag[(size_t)i / 32] >> (i % 32)) & 1);
            break;
        }
        case vpiScalarVal: {
            int t = vp->value.scalar;
            out = make(0, 0, w);
            setBit(out, 0, t == vpi0 || t == vpiL ? 0 : t == vpi1 || t == vpiH ? 1 : t == vpiZ ? 3 : 2);
            break;
        }
        case vpiIntVal:
            if (o->real) out = fromReal(vp->value.integer);
            else out = fromInt(vp->value.integer);
            break;
        case vpiRealVal: out = fromReal(vp->value.real); break;
        case vpiStringVal: {
            std::string s = vp->value.str ? vp->value.str : "";
            if (o->str) { out = mkstr(s); break; }
            out = make(0, 0, w);
            if (w > 64) out = maskWidth(out, w);
            for (size_t k = 0; k < s.size() && (int)k * 8 < w; k++) {
                unsigned char c = (unsigned char)s[s.size() - 1 - k];
                for (int b = 0; b < 8 && (int)k * 8 + b < w; b++) setBit(out, (int)k * 8 + b, (c >> b) & 1);
            }
            break;
        }
        case vpiVectorVal: {
            out = make(0, 0, w);
            for (int i = 0; i < w; i++) {
                const auto& e = vp->value.vector[i / 32];
                uint32_t m = 1u << (i % 32);
                bool a = e.aval & m, b = e.bval & m;
                setBit(out, i, b ? (a ? 2 : 3) : (a ? 1 : 0));
            }
            break;
        }
        case vpiTimeVal: {
            uint64_t t = ((uint64_t)vp->value.time->high << 32) | vp->value.time->low;
            out = fromInt((int64_t)t);
            break;
        }
        default:
            error(vpiError, "vpi_put_value: unsupported format " + std::to_string(vp->format));
            return false;
    }
    if (!o->real && !o->str && w <= 64) { out.w = w; out.hi.clear(); }
    return true;
}
inline void write(const Obj* o, const Val& v) {
    if (o->kind == Obj::Bit) {
        Val cur = S.get(o->sig);
        S.set(o->sig, v_insert(cur, v_resize(v, o->w, false), o->off + o->w - 1, o->off));
        return;
    }
    S.set(o->sig, v);
}

// ---- time ----
inline int unitExp(const Obj* o) {
    const InstanceSymbol* inst = nullptr;
    if (o) {
        if (o->kind == Obj::Module) inst = &o->sym->as<InstanceSymbol>();
        else if (o->kind == Obj::Call || o->kind == Obj::Arg) inst = static_cast<const InstanceSymbol*>(o->site->scope);
        else if (o->sym) inst = instOf(o->sym->getParentScope());
    }
    if (inst)
        if (auto ts = inst->body.getTimeScale()) return tsExp(ts->base);
    return st().lr->precExp;
}
inline int precExpOf(const Obj* o) {
    if (o && o->kind == Obj::Module)
        if (auto ts = o->sym->as<InstanceSymbol>().body.getTimeScale()) return tsExp(ts->precision);
    return st().lr->precExp;
}
inline void fillTime(p_vpi_time t, const Obj* o, uint64_t ticks) {
    if (!t) return;
    if (t->type == vpiScaledRealTime) t->real = (double)ticks * std::pow(10.0, st().lr->precExp - unitExp(o));
    else if (t->type == vpiSimTime) {
        t->high = (PLI_UINT32)(ticks >> 32);
        t->low = (PLI_UINT32)ticks;
    }
}
// A delay given in a s_vpi_time, in ticks.
inline int64_t sigTicks(const p_vpi_time t, const Obj* o) {
    if (!t) return 0;
    if (t->type == vpiSimTime) return (int64_t)(((uint64_t)t->high << 32) | t->low);
    if (t->type == vpiScaledRealTime) return (int64_t)std::llround(t->real * std::pow(10.0, unitExp(o) - st().lr->precExp));
    return 0;
}

// ---- callbacks ----
inline void fire(Cb* c) {
    if (!c->live) return;
    s_cb_data d = c->d;
    s_vpi_time t = c->t;
    s_vpi_value v = c->v;
    d.time = c->hasT ? &t : nullptr;
    d.value = c->hasV ? &v : nullptr;
    if (c->hasT && t.type != vpiSuppressTime) fillTime(&t, c->obj, S.now);
    if (c->hasV && c->obj && v.format != vpiSuppressVal) getValue(c->obj, &v);
    if (c->obj && c->obj->kind == Obj::Word) d.index = c->obj->idx;
    if (d.reason != cbValueChange) c->live = false;   // the others fire once
    d.cb_rtn(&d);
}
inline void fireList(std::vector<Cb*>& l) {
    std::vector<Cb*> now;
    now.swap(l);
    for (Cb* c : now) fire(c);
}
inline void onChange(int s) {
    auto it = st().change.find(s);
    if (it == st().change.end()) return;
    auto list = it->second;   // a callback may register or remove others
    for (Cb* c : list) if (c->live) fire(c);
}
inline bool onRegion(int r) {
    auto& s = st();
    if (r == Sim::VR_START) {
        auto [a, b] = s.timed.equal_range(S.now);
        std::vector<Cb*> due;
        for (auto it = a; it != b; ++it) due.push_back(it->second);
        s.timed.erase(a, b);
        S.vpiAt.erase(S.now);
        auto [pa, pb] = s.puts.equal_range(S.now);
        std::vector<std::pair<int, Val>> writes;
        for (auto it = pa; it != pb; ++it) writes.push_back(it->second);
        s.puts.erase(pa, pb);
        for (auto& [sg, v] : writes) S.set(sg, v);
        std::vector<Cb*> nxt;
        for (Cb* c : s.next) (c->at < S.now ? due : nxt).push_back(c);
        s.next.swap(nxt);
        for (Cb* c : due) {
            if (c->d.reason == cbReadWriteSynch) s.rw.push_back(c);
            else if (c->d.reason == cbReadOnlySynch) s.ro.push_back(c);
            else fire(c);
        }
        if (!s.rw.empty() || !s.ro.empty()) S.rare = true;
        return false;
    }
    std::vector<Cb*>& l = r == Sim::VR_RW ? s.rw : s.ro;
    if (l.empty()) return false;
    fireList(l);
    if (!s.rw.empty() || !s.ro.empty()) S.rare = true;
    return true;
}
inline void at(uint64_t t) {
    S.vpiAt[t]++;
    S.wheel[t];
}

// ---- the call of a user system task or function ----
inline void callSite(const Instr& ins) {
    auto& s = st();
    const VpiSite* site = ins.vpi;
    const Systf& f = s.systfs[(size_t)site->systf];
    if (!f.d.calltf) return;
    const VpiSite* saved = s.cur;
    s.cur = site;
    f.d.calltf(f.d.user_data);
    s.cur = saved;
}

// The slang side of a user system task or function: any arguments (a
// module name is passed as the instance), and the result type its
// sysfunctype gives (38.37).
class UserSystf : public SystemSubroutine {
public:
    VpiSystfInfo info;
    UserSystf(const std::string& n, const VpiSystfInfo& i)
        : SystemSubroutine(n, i.func ? SubroutineKind::Function : SubroutineKind::Task), info(i) {}
    bool allowEmptyArgument(size_t) const final { return true; }
    const Expression& bindArgument(size_t argIndex, const ASTContext& context, const slang::syntax::ExpressionSyntax& syntax,
                                   const Args& args) const final {
        using slang::syntax::SyntaxKind;
        if (syntax.kind == SyntaxKind::IdentifierName || syntax.kind == SyntaxKind::ScopedName) {
            std::string text;
            for (char c : syntax.toString()) if (!std::isspace((unsigned char)c)) text += c;
            const Symbol* found = context.scope->lookupName(text, context.getLocation());
            if (found && found->kind == SymbolKind::Instance)
                return ArbitrarySymbolExpression::fromSyntax(context.getCompilation(), syntax.as<slang::syntax::NameSyntax>(), context);
        }
        return SystemSubroutine::bindArgument(argIndex, context, syntax, args);
    }
    const Type& checkArguments(const ASTContext& context, const Args&, slang::SourceRange, const Expression*) const final {
        auto& comp = context.getCompilation();
        if (!info.func) return comp.getVoidType();
        switch (info.ftype) {
            case vpiRealFunc: return comp.getRealType();
            case vpiTimeFunc: return comp.getType(64, slang::ast::IntegralFlags::FourState);
            case vpiSizedFunc: return comp.getType((slang::bitwidth_t)info.size, slang::ast::IntegralFlags::FourState);
            case vpiSizedSignedFunc:
                return comp.getType((slang::bitwidth_t)info.size, slang::ast::IntegralFlags::FourState | slang::ast::IntegralFlags::Signed);
            default: return comp.getIntType();
        }
    }
    slang::ConstantValue eval(EvalContext& context, const Args&, slang::SourceRange range,
                              const CallExpression::SystemCallInfo&) const final {
        notConst(context, range);
        return nullptr;
    }
};

// ---- set-up from msim ----
// Load a VPI library and run its bootstrap: vlog_startup_routines, and the
// named routine of -load lib:boot (36.9.1).
inline bool load(const std::string& path, const std::string& boot, std::string& err) {
    void* h = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!h) { err = dlerror(); return false; }
    if (!boot.empty()) {
        void* f = dlsym(h, boot.c_str());
        if (!f) { err = "no routine " + boot + " in " + path; return false; }
        reinterpret_cast<void (*)()>(f)();
    }
    if (void* tab = dlsym(h, "vlog_startup_routines"))
        for (auto** p = reinterpret_cast<void (**)()>(tab); *p; p++) (*p)();
    return true;
}
// Make the registered names known to slang before elaboration.
inline void declare(Compilation& comp) {
    for (auto& [n, i] : vpiSystfNames()) comp.addSystemSubroutine(std::make_shared<UserSystf>(n, i));
}
inline void setup(Compilation& comp, const LowerResult& lr) {
    auto& s = st();
    s.comp = &comp;
    s.lr = &lr;
    vpiCallHook() = callSite;
    S.vpiChange = onChange;
    S.vpiRegion = onRegion;
    // compiletf runs once per call site, before the simulation (38.37.1).
    for (const auto& site : Lowerer::vpiSites()) {
        const Systf& f = s.systfs[(size_t)site->systf];
        if (!f.d.compiletf) continue;
        s.cur = site.get();
        f.d.compiletf(f.d.user_data);
        s.cur = nullptr;
    }
    fireList(s.endCompile);
    fireList(s.start);
    if (!s.rw.empty() || !s.ro.empty()) S.rare = true;
}
inline void end() { fireList(st().endSim); }
}   // namespace vpi

// ---- the vpi_user.h functions ----
using vpi::H;
using vpi::O;
using vpi::Obj;

extern "C" {

vpiHandle vpi_register_systf(p_vpi_systf_data d) {
    vpi::clearErr();
    auto& s = vpi::st();
    if (!d || !d->tfname || d->tfname[0] != '$') { vpi::error(vpiError, "vpi_register_systf: the name must start with $"); return nullptr; }
    vpi::Systf f;
    f.d = *d;
    f.name = d->tfname;
    f.d.tfname = nullptr;
    VpiSystfInfo info;
    info.idx = (int)s.systfs.size();
    info.func = d->type == vpiSysFunc;
    info.ftype = info.func ? (d->sysfunctype ? d->sysfunctype : vpiIntFunc) : 0;
    if (info.func && (info.ftype == vpiSizedFunc || info.ftype == vpiSizedSignedFunc))
        info.size = d->sizetf ? d->sizetf(d->user_data) : 32;
    if (info.size <= 0) info.size = 32;
    s.systfs.push_back(f);
    s.systfs.back().d.tfname = s.systfs.back().name.c_str();
    vpiSystfNames()[f.name] = info;
    s.active = true;
    Obj* o = vpi::mkObj(Obj::Systf, vpiUserSystf);
    o->idx = info.idx;
    s.sysObj[info.idx] = o;
    return H(o);
}

void vpi_get_systf_info(vpiHandle h, p_vpi_systf_data d) {
    vpi::clearErr();
    Obj* o = O(h);
    if (!o || o->kind != Obj::Systf || !d) { vpi::error(vpiError, "vpi_get_systf_info: not a vpiUserSystf handle"); return; }
    *d = vpi::st().systfs[(size_t)o->idx].d;
}

vpiHandle vpi_register_cb(p_cb_data d) {
    vpi::clearErr();
    auto& s = vpi::st();
    if (!d || !d->cb_rtn) { vpi::error(vpiError, "vpi_register_cb: no callback routine"); return nullptr; }
    s.cbs.push_back(std::make_unique<vpi::Cb>());
    vpi::Cb* c = s.cbs.back().get();
    c->d = *d;
    if (d->time) { c->t = *d->time; c->hasT = true; }
    if (d->value) { c->v = *d->value; c->hasV = true; }
    c->obj = O(d->obj);
    c->d.time = nullptr;
    c->d.value = nullptr;
    s.active = true;
    uint64_t delay = d->time ? (uint64_t)vpi::sigTicks(d->time, c->obj) : 0;
    switch (d->reason) {
        case cbValueChange: {
            Obj* o = c->obj;
            // No callback on a protected object (38.36.1, IEEE 1735 9.7.1).
            if (vpi::isProt(o)) {
                vpi_printf((PLI_BYTE8*)"VPI error: vpi_register_cb: a callback cannot be placed on a protected object\n");
                vpi::error(vpiError, "vpi_register_cb: the object is protected");
                s.cbs.pop_back();
                return nullptr;
            }
            if (!o || (o->kind != Obj::Signal && o->kind != Obj::Word && o->kind != Obj::Bit) || o->type == vpiMemory || o->sig < 0) {
                vpi::error(vpiError, "vpi_register_cb: cbValueChange needs a net, variable, memory word or bit");
                return nullptr;
            }
            S.sigs[(size_t)o->sig].fl |= Sim::SF_VPI;
            s.change[o->sig].push_back(c);
            break;
        }
        case cbReadWriteSynch:
        case cbReadOnlySynch:
            if (delay == 0) {
                (d->reason == cbReadWriteSynch ? s.rw : s.ro).push_back(c);
                S.rare = true;
            } else {
                c->at = S.now + delay;
                s.timed.insert({c->at, c});
                vpi::at(c->at);
            }
            break;
        case cbAfterDelay:
        case cbAtStartOfSimTime:
            c->at = S.now + delay;
            if (delay == 0 && d->reason == cbAfterDelay) {   // this time slot, after what is queued
                s.rw.push_back(c);
                S.rare = true;
            } else {
                s.timed.insert({c->at, c});
                vpi::at(c->at);
            }
            break;
        case cbNextSimTime:
            c->at = S.now;
            s.next.push_back(c);
            break;
        case cbEndOfCompile: s.endCompile.push_back(c); break;
        case cbStartOfSimulation: s.start.push_back(c); break;
        case cbEndOfSimulation: s.endSim.push_back(c); break;
        default:
            vpi::error(vpiWarning, "vpi_register_cb: reason " + std::to_string(d->reason) + " is not supported");
            return nullptr;
    }
    Obj* h = vpi::mkObj(Obj::Callback, vpiCallback);
    h->cb = c;
    c->self = h;
    return H(h);
}

PLI_INT32 vpi_remove_cb(vpiHandle h) {
    vpi::clearErr();
    Obj* o = O(h);
    if (!o || o->kind != Obj::Callback) { vpi::error(vpiError, "vpi_remove_cb: not a callback handle"); return 0; }
    o->cb->live = false;
    if (o->cb->d.reason == cbValueChange && o->cb->obj) {
        auto& l = vpi::st().change[o->cb->obj->sig];
        l.erase(std::remove(l.begin(), l.end(), o->cb), l.end());
    }
    return 1;
}

void vpi_get_cb_info(vpiHandle h, p_cb_data d) {
    vpi::clearErr();
    Obj* o = O(h);
    if (!o || o->kind != Obj::Callback || !d) { vpi::error(vpiError, "vpi_get_cb_info: not a callback handle"); return; }
    *d = o->cb->d;
    d->time = o->cb->hasT ? &o->cb->t : nullptr;
    d->value = o->cb->hasV ? &o->cb->v : nullptr;
}

vpiHandle vpi_handle(PLI_INT32 type, vpiHandle ref) {
    vpi::clearErr();
    auto& s = vpi::st();
    Obj* o = O(ref);
    if (type == vpiSysTfCall && !o) {
        if (!s.cur) { vpi::error(vpiError, "vpi_handle(vpiSysTfCall, NULL) outside a calltf or compiletf"); return nullptr; }
        return H(vpi::callObj(s.cur));
    }
    if (!o) { vpi::error(vpiError, "vpi_handle: null reference handle"); return nullptr; }
    if (vpi::denied(o, "vpi_handle")) return nullptr;
    switch (type) {
        case vpiScope:
        case vpiModule:
        case vpiParent: {
            if (o->kind == Obj::Word || o->kind == Obj::Bit) {
                if (type == vpiParent) return H(const_cast<Obj*>(o->parent));
                o = const_cast<Obj*>(o->parent);
            }
            if (o->kind == Obj::Call || o->kind == Obj::Arg)
                return H(vpi::moduleObj(static_cast<const slang::ast::InstanceSymbol*>(o->site->scope)));
            if (o->kind == Obj::Module) return H(vpi::moduleObj(vpi::instOf(o->sym->getParentScope())));
            if (o->sym) return H(vpi::moduleObj(vpi::instOf(o->sym->getParentScope())));
            return nullptr;
        }
        case vpiUserSystf:
            if (o->kind == Obj::Call) return H(s.sysObj[o->site->systf]);
            return nullptr;
        case vpiLowConn:
            if (o->kind == Obj::Port) {
                const auto& p = o->sym->as<slang::ast::PortSymbol>();
                if (p.internalSymbol && slang::ast::ValueSymbol::isKind(p.internalSymbol->kind)) return H(vpi::valueObj(p.internalSymbol));
            }
            return nullptr;
        case vpiLeftRange:
        case vpiRightRange:
            return nullptr;
        default:
            vpi::error(vpiWarning, "vpi_handle: relation " + std::to_string(type) + " is not supported");
            return nullptr;
    }
}

vpiHandle vpi_handle_multi(PLI_INT32 type, vpiHandle a, vpiHandle b, ...);

vpiHandle vpi_iterate(PLI_INT32 type, vpiHandle ref) {
    vpi::clearErr();
    auto& s = vpi::st();
    Obj* o = O(ref);
    std::vector<vpiHandle> items;
    if (type == vpiModule && !o) {
        for (const slang::ast::InstanceSymbol* t : s.comp->getRoot().topInstances) items.push_back(H(vpi::moduleObj(t)));
        return vpi::iter(std::move(items));
    }
    if (!o) { vpi::error(vpiError, "vpi_iterate: null reference handle"); return nullptr; }
    // A protected call still gives its arguments (38.36); nothing else of a
    // protected object can be iterated.
    if (!(type == vpiArgument && o->kind == Obj::Call) && vpi::denied(o, "vpi_iterate")) return nullptr;
    if (type == vpiArgument) {
        if (o->kind != Obj::Call) return nullptr;
        for (size_t k = 0; k < o->site->args.size(); k++)
            if (Obj* a = vpi::argObj(o->site, (int)k)) items.push_back(H(a));
        return vpi::iter(std::move(items));
    }
    if (type == vpiBit) {
        if (o->kind != Obj::Signal || o->type == vpiMemory || o->real) return nullptr;
        auto [l, r] = vpi::rangeOf(o);
        for (int64_t i = l;; i += l >= r ? -1 : 1) {
            items.push_back(H(vpi::bitObj(o, i)));
            if (i == r) break;
        }
        return vpi::iter(std::move(items));
    }
    if (type == vpiMemoryWord) {
        if (o->kind != Obj::Signal || o->type != vpiMemory) return nullptr;
        auto am = arrMap().find(o->sym);
        auto [l, r] = am->second->dims[0];
        for (int64_t i = l;; i += l <= r ? 1 : -1) {
            items.push_back(H(vpi::wordObj(o, i)));
            if (i == r) break;
        }
        return vpi::iter(std::move(items));
    }
    if (o->kind != Obj::Module) return nullptr;
    const auto& inst = o->sym->as<slang::ast::InstanceSymbol>();
    if (type == vpiPort) {
        for (const slang::ast::Symbol* p : inst.body.getPortList())
            if (p->kind == slang::ast::SymbolKind::Port) {
                auto& po = s.symObj[p];
                if (!po) {
                    po = vpi::mkObj(Obj::Port, vpiPort);
                    po->sym = p;
                    po->idx = (int)items.size();
                }
                items.push_back(H(po));
            }
        return vpi::iter(std::move(items));
    }
    vpi::eachMember(inst.body, [&](const slang::ast::Symbol& m) {
        switch (type) {
            case vpiModule:
            case vpiInternalScope:
                if (m.kind == slang::ast::SymbolKind::Instance && m.as<slang::ast::InstanceSymbol>().isModule()) items.push_back(H(vpi::moduleObj(&m.as<slang::ast::InstanceSymbol>())));
                break;
            case vpiNet:
            case vpiReg:
            case vpiVariables:
            case vpiIntegerVar:
            case vpiRealVar:
            case vpiTimeVar:
            case vpiMemory:
            case vpiRegArray: {
                if (m.kind != slang::ast::SymbolKind::Net && m.kind != slang::ast::SymbolKind::Variable) break;
                Obj* v = vpi::valueObj(&m);
                if (!v) break;
                bool ok = v->type == type || (type == vpiRegArray && v->type == vpiMemory) ||
                          (type == vpiVariables && (v->type == vpiIntegerVar || v->type == vpiRealVar || v->type == vpiTimeVar || v->type == vpiReg));
                if (ok) items.push_back(H(v));
                break;
            }
            case vpiParameter:
                if (m.kind == slang::ast::SymbolKind::Parameter) {
                    auto& po = s.symObj[&m];
                    if (!po) {
                        po = vpi::mkObj(Obj::Param, vpiParameter);
                        po->sym = &m;
                        const auto& t = m.as<slang::ast::ParameterSymbol>().getType();
                        po->w = t.isFloating() ? 64 : t.isString() ? 0 : (int)t.getBitWidth();
                        po->sgn = t.isSigned();
                        po->real = t.isFloating();
                        po->str = t.isString();
                    }
                    items.push_back(H(po));
                }
                break;
            default: break;
        }
    });
    if (type != vpiModule && type != vpiInternalScope && type != vpiNet && type != vpiReg && type != vpiVariables &&
        type != vpiIntegerVar && type != vpiRealVar && type != vpiTimeVar && type != vpiMemory && type != vpiRegArray &&
        type != vpiParameter)
        vpi::error(vpiWarning, "vpi_iterate: type " + std::to_string(type) + " is not supported");
    return vpi::iter(std::move(items));
}

vpiHandle vpi_scan(vpiHandle it) {
    vpi::clearErr();
    Obj* o = O(it);
    if (!o || o->kind != Obj::Iter) { vpi::error(vpiError, "vpi_scan: not an iterator"); return nullptr; }
    if (o->pos < o->items.size()) return o->items[o->pos++];
    // The iterator is freed when the scan reaches its end (38.36).
    o->items.clear();
    o->kind = Obj::Event;   // a dead handle
    return nullptr;
}

vpiHandle vpi_handle_by_name(PLI_BYTE8* name, vpiHandle scope) {
    vpi::clearErr();
    auto& s = vpi::st();
    if (!name) return nullptr;
    std::string n = name;
    std::vector<std::string> parts;
    {
        std::string cur;
        int depth = 0;
        for (char c : n) {
            if (c == '[') depth++;
            if (c == ']') depth--;
            if (c == '.' && depth == 0) { parts.push_back(cur); cur.clear(); }
            else cur += c;
        }
        parts.push_back(cur);
    }
    const slang::ast::Symbol* sym = nullptr;
    const slang::ast::Scope* sc = nullptr;
    Obj* so = O(scope);
    if (vpi::denied(so, "vpi_handle_by_name")) return nullptr;   // a protected scope (38.20)
    // The name may not find a protected object or instance, even by its
    // disclosed name (38.20; IEEE 1735 9.7.1). What the IP author made
    // visible with a viewport or a right is found by its full name.
    auto shut = [&](const slang::ast::Symbol* y) {
        bool p = false;
        if (y->kind == slang::ast::SymbolKind::Instance) p = protectedInstance(&y->as<slang::ast::InstanceSymbol>());
        else if (y->kind == slang::ast::SymbolKind::GenerateBlock || y->kind == slang::ast::SymbolKind::GenerateBlockArray)
            p = protectedInstance(vpi::instOf(y->getParentScope())) || (hideMsgIf() && hideMsgIf()(y->location));
        else p = vpi::protSym(y);
        if (p) vpi::error(vpiError, "vpi_handle_by_name: the name is in protected code");
        return p;
    };
    if (so && so->kind == Obj::Module) sc = &so->sym->as<slang::ast::InstanceSymbol>().body;
    // A name with an index: the base, then [i] steps.
    auto step = [&](const slang::ast::Scope* in, const std::string& part) -> const slang::ast::Symbol* {
        std::string base = part.substr(0, part.find('['));
        const slang::ast::Symbol* r = in ? in->find(base) : nullptr;
        if (!in) {
            for (const slang::ast::InstanceSymbol* t : s.comp->getRoot().topInstances)
                if (t->name == base) r = t;
        }
        size_t p = part.find('[');
        while (r && p != std::string::npos) {
            size_t q = part.find(']', p);
            int64_t idx = std::strtoll(part.c_str() + p + 1, nullptr, 10);
            const slang::ast::Symbol* nx = nullptr;
            if (r->kind == slang::ast::SymbolKind::GenerateBlockArray) {
                for (const auto* e : r->as<slang::ast::GenerateBlockArraySymbol>().entries)
                    if (e->getArrayIndex() && e->getArrayIndex()->as<int64_t>() == idx) nx = e;
            } else if (r->kind == slang::ast::SymbolKind::InstanceArray) {
                const auto& a = r->as<slang::ast::InstanceArraySymbol>();
                int64_t lo = std::min(a.range.left, a.range.right);
                int64_t off = a.range.left <= a.range.right ? idx - a.range.left : a.range.left - idx;
                (void)lo;
                if (off >= 0 && off < (int64_t)a.elements.size()) nx = a.elements[(size_t)off];
            } else {
                return r;   // a memory word or bit: the caller indexes the object
            }
            r = nx;
            p = q == std::string::npos ? q : part.find('[', q);
        }
        return r;
    };
    for (size_t k = 0; k < parts.size(); k++) {
        if (k > 0 && !sc) return nullptr;
        sym = step(k == 0 ? sc : sc, parts[k]);
        if (!sym && k == 0 && sc) sym = step(nullptr, parts[k]);   // a full name given with a scope
        if (!sym) return nullptr;
        if (sym->kind == slang::ast::SymbolKind::Instance) sc = &sym->as<slang::ast::InstanceSymbol>().body;
        else if (sym->kind == slang::ast::SymbolKind::GenerateBlock) sc = &sym->as<slang::ast::GenerateBlockSymbol>();
        else sc = nullptr;
    }
    if (!sym) return nullptr;
    if (shut(sym)) return nullptr;
    if (sym->kind == slang::ast::SymbolKind::Instance) return H(vpi::moduleObj(&sym->as<slang::ast::InstanceSymbol>()));
    if (sym->kind == slang::ast::SymbolKind::Parameter) {
        auto& po = s.symObj[sym];
        if (!po) {
            po = vpi::mkObj(Obj::Param, vpiParameter);
            po->sym = sym;
            const auto& t = sym->as<slang::ast::ParameterSymbol>().getType();
            po->w = t.isFloating() ? 64 : t.isString() ? 0 : (int)t.getBitWidth();
            po->sgn = t.isSigned();
            po->real = t.isFloating();
            po->str = t.isString();
        }
        return H(po);
    }
    if (sym->kind == slang::ast::SymbolKind::Net || sym->kind == slang::ast::SymbolKind::Variable) {
        Obj* v = vpi::valueObj(sym);
        if (!v) return nullptr;
        const std::string& last = parts.back();
        if (size_t p = last.find('['); p != std::string::npos) {
            char* end = nullptr;
            int64_t idx = std::strtoll(last.c_str() + p + 1, &end, 10);
            if (end && *end == ':' && v->type != vpiMemory)   // v[a:b]
                return H(vpi::partObj(v, idx, std::strtoll(end + 1, nullptr, 10)));
            return H(v->type == vpiMemory ? vpi::wordObj(v, idx) : vpi::bitObj(v, idx));
        }
        return H(v);
    }
    return nullptr;
}

vpiHandle vpi_handle_by_index(vpiHandle h, PLI_INT32 i) {
    vpi::clearErr();
    Obj* o = O(h);
    if (!o || o->kind != Obj::Signal) return nullptr;
    if (vpi::denied(o, "vpi_handle_by_index")) return nullptr;
    if (o->type == vpiMemory) return H(vpi::wordObj(o, i));
    if (o->real) return nullptr;
    return H(vpi::bitObj(o, i));
}

vpiHandle vpi_handle_by_multi_index(vpiHandle h, PLI_INT32 n, PLI_INT32* idx) {
    if (n == 1 && idx) return vpi_handle_by_index(h, idx[0]);
    vpi::error(vpiWarning, "vpi_handle_by_multi_index: only one index is supported");
    return nullptr;
}

PLI_INT32 vpi_get(PLI_INT32 prop, vpiHandle h) {
    vpi::clearErr();
    auto& s = vpi::st();
    Obj* o = O(h);
    if (!o) {
        if (prop == vpiCompatibilityMode) return s.mode;   // 36.12.2
        if (prop == vpiTimePrecision) return s.lr->precExp;
        if (prop == vpiTimeUnit) return s.lr->precExp;
        return vpiUndefined;
    }
    // 37.3.6: of a protected object only vpiType and vpiIsProtected can be
    // read, and vpiSize of an expression (37.57).
    if (prop == vpiIsProtected) return vpi::isProt(o) ? 1 : 0;
    bool expr = o->kind == Obj::Arg || o->kind == Obj::Call || o->kind == Obj::Word || o->kind == Obj::Bit ||
                (o->kind == Obj::Signal && o->type != vpiMemory);
    if (prop != vpiType && !(prop == vpiSize && expr) && vpi::denied(o, "vpi_get"))
        return vpiUndefined;
    switch (prop) {
        case vpiType: return o->type;
        case vpiSize:
            if (o->kind == Obj::Signal && o->type == vpiMemory) return arrMap().find(o->sym)->second->count;
            if (o->kind == Obj::Call) return o->site->func ? o->w : 0;
            if (o->kind == Obj::Port) return (int)o->sym->as<slang::ast::PortSymbol>().getType().getBitWidth();
            if (o->kind == Obj::Arg && o->str) return std::max(8, (int)v_str(vpi::readObj(o)).size() * 8);
            return o->w;
        case vpiSigned: return o->sgn ? 1 : 0;
        case vpiScalar: return o->w == 1 && !o->real ? 1 : 0;
        case vpiVector: return o->w > 1 && !o->real && o->type != vpiMemory ? 1 : 0;
        case vpiTopModule: return o->kind == Obj::Module && o->sym->as<slang::ast::InstanceSymbol>().isTopLevel();
        case vpiLineNo:
            if (o->kind == Obj::Call) return o->site->line;
            if (o->sym && srcMgr()) return (int)srcMgr()->getLineNumber(o->sym->location);
            return 0;
        case vpiTimeUnit: return vpi::unitExp(o);
        case vpiTimePrecision: return vpi::precExpOf(o);
        case vpiDirection:
            if (o->kind == Obj::Port) {
                switch (o->sym->as<slang::ast::PortSymbol>().direction) {
                    case slang::ast::ArgumentDirection::In: return vpiInput;
                    case slang::ast::ArgumentDirection::Out: return vpiOutput;
                    case slang::ast::ArgumentDirection::InOut: return vpiInout;
                    default: return vpiNoDirection;
                }
            }
            return vpiUndefined;
        case vpiPortIndex: return o->kind == Obj::Port ? o->idx : vpiUndefined;
        case vpiNetType: return o->type == vpiNet ? vpiWire : vpiUndefined;
        case vpiFuncType:
            if (o->kind == Obj::Call) return o->site->func ? vpiSystfNames()[s.systfs[(size_t)o->site->systf].name].ftype : 0;
            if (o->kind == Obj::Systf) return s.systfs[(size_t)o->idx].d.sysfunctype;
            return vpiUndefined;
        case vpiConstType:
            if (o->kind == Obj::Arg && o->type == vpiConstant) {
                const VpiArg& a = o->site->args[(size_t)o->idx];
                if (a.kind == VpiArg::String) return vpiStringConst;
                if (a.real) return vpiRealConst;
                return vpiBinaryConst;
            }
            return vpiUndefined;
        case vpiLocalParam:
            return o->kind == Obj::Param && o->sym->as<slang::ast::ParameterSymbol>().isLocalParam() ? 1 : 0;
        case vpiArray: return o->type == vpiMemory ? 1 : 0;
        case vpiAutomatic: return 0;
        case vpiIndex: return o->idx;
        default:
            vpi::error(vpiWarning, "vpi_get: property " + std::to_string(prop) + " is not supported");
            return vpiUndefined;
    }
}

PLI_INT64 vpi_get64(PLI_INT32 prop, vpiHandle h) { return vpi_get(prop, h); }

PLI_BYTE8* vpi_get_str(PLI_INT32 prop, vpiHandle h) {
    vpi::clearErr();
    auto& s = vpi::st();
    Obj* o = O(h);
    if (!o) return nullptr;
    if (prop != vpiType && vpi::denied(o, "vpi_get_str")) return nullptr;
    auto nameOf = [&](bool full) -> std::string {
        switch (o->kind) {
            case Obj::Call: return s.systfs[(size_t)o->site->systf].name;
            case Obj::Systf: return s.systfs[(size_t)o->idx].name;
            case Obj::Word:
            case Obj::Bit: {
                std::string b = full ? vpi::fullName(*o->parent->sym) : std::string(o->parent->sym->name);
                if (o->type == vpiPartSelect) return b + "[" + std::to_string(o->idx) + ":" + std::to_string(o->lidx) + "]";
                return b + "[" + std::to_string(o->idx) + "]";
            }
            case Obj::Arg: return "";
            default:
                if (!o->sym) return "";
                return full ? vpi::fullName(*o->sym) : std::string(o->sym->name);
        }
    };
    switch (prop) {
        case vpiName: return vpi::retStr(nameOf(false));
        case vpiFullName: return vpi::retStr(nameOf(true));
        case vpiDefName:
            if (o->kind == Obj::Module) return vpi::retStr(std::string(o->sym->as<slang::ast::InstanceSymbol>().getDefinition().name));
            return nullptr;
        // 33.7: the library and cell an instance is bound to, and the
        // library.cell of the config that bound it ("" without one).
        case vpiLibrary:
        case vpiCell:
        case vpiConfig: {
            if (o->kind != Obj::Module) return nullptr;
            const auto& inst = o->sym->as<slang::ast::InstanceSymbol>();
            const auto& def = inst.getDefinition();
            if (prop == vpiLibrary) return vpi::retStr(def.sourceLibrary.name);
            if (prop == vpiCell) return vpi::retStr(std::string(def.name));
            if (!inst.resolvedConfig) return vpi::retStr(std::string());
            const auto& cfg = inst.resolvedConfig->useConfig;
            std::string lib = "work";
            for (const slang::ast::Scope* sc = cfg.getParentScope(); sc; sc = sc->asSymbol().getParentScope())
                if (sc->asSymbol().kind == slang::ast::SymbolKind::CompilationUnit) {
                    lib = sc->asSymbol().as<slang::ast::CompilationUnitSymbol>().sourceLibrary.name;
                    break;
                }
            return vpi::retStr(lib + "." + std::string(cfg.name));
        }
        case vpiFile:
            if (o->kind == Obj::Call) return vpi::retStr(o->site->file);
            if (o->sym && srcMgr()) return vpi::retStr(std::string(srcMgr()->getFileName(o->sym->location)));
            return nullptr;
        case vpiType: {
            static const std::map<int, const char*> names = {
                {vpiModule, "vpiModule"}, {vpiNet, "vpiNet"}, {vpiReg, "vpiReg"}, {vpiIntegerVar, "vpiIntegerVar"},
                {vpiRealVar, "vpiRealVar"}, {vpiTimeVar, "vpiTimeVar"}, {vpiMemory, "vpiMemory"},
                {vpiMemoryWord, "vpiMemoryWord"}, {vpiNetBit, "vpiNetBit"}, {vpiRegBit, "vpiRegBit"},
                {vpiParameter, "vpiParameter"}, {vpiPort, "vpiPort"}, {vpiConstant, "vpiConstant"},
                {vpiOperation, "vpiOperation"}, {vpiSysTaskCall, "vpiSysTaskCall"}, {vpiSysFuncCall, "vpiSysFuncCall"},
                {vpiUserSystf, "vpiUserSystf"}, {vpiIterator, "vpiIterator"}, {vpiCallback, "vpiCallback"}};
            auto it = names.find(o->type);
            return it == names.end() ? nullptr : vpi::retStr(it->second);
        }
        default:
            vpi::error(vpiWarning, "vpi_get_str: property " + std::to_string(prop) + " is not supported");
            return nullptr;
    }
}

void vpi_get_value(vpiHandle h, p_vpi_value v) {
    vpi::clearErr();
    Obj* o = O(h);
    if (!o || !v) { vpi::error(vpiError, "vpi_get_value: null handle"); return; }
    if (vpi::isProt(o) && !vpi::inOwnCall(o)) {
        vpi::error(vpiError, "vpi_get_value: the object is protected");
        v->format = vpiSuppressVal;
        return;
    }
    if (o->kind == Obj::Param) {
        const slang::ConstantValue& cv = o->sym->as<slang::ast::ParameterSymbol>().getValue();
        Val val;
        if (cv.isReal()) {
            double d = (double)cv.real();
            uint64_t b;
            std::memcpy(&b, &d, 8);
            val = make(b, 0, 64);
            val.real = true;
        } else if (cv.isString()) {
            val = mkstr(cv.str());
        } else if (cv.isInteger()) {
            const slang::SVInt& si = cv.integer();
            int w = (int)si.getBitWidth();
            val = make(0, 0, w);
            if (w > 64) val = maskWidth(val, w);
            for (int i = 0; i < w; i++) {
                slang::logic_t b = si[i];
                ::setBit(val, i, b.value == slang::logic_t::Z_VALUE ? 3 : b.isUnknown() ? 2 : (b.value ? 1 : 0));
            }
        } else {
            vpi::error(vpiError, "vpi_get_value: parameter value of this type");
            return;
        }
        vpi::getValueOf(o, val, v);
        return;
    }
    if (o->kind == Obj::Iter || o->kind == Obj::Callback || o->kind == Obj::Systf || o->kind == Obj::Module ||
        (o->kind == Obj::Signal && o->type == vpiMemory)) {
        vpi::error(vpiError, "vpi_get_value: the object has no value");
        return;
    }
    vpi::getValue(o, v);
}

vpiHandle vpi_put_value(vpiHandle h, p_vpi_value v, p_vpi_time t, PLI_INT32 flags) {
    vpi::clearErr();
    Obj* o = O(h);
    if (!o || !v) { vpi::error(vpiError, "vpi_put_value: null handle"); return nullptr; }
    // Protected IP cannot be written from outside it (IEEE 1735 9.7.1).
    if (vpi::isProt(o) && !vpi::inOwnCall(o)) { vpi::error(vpiError, "vpi_put_value: the object is protected"); return nullptr; }
    bool target = (o->kind == Obj::Signal && o->type != vpiMemory) || o->kind == Obj::Word || o->kind == Obj::Bit ||
                  (o->kind == Obj::Call && o->site->func);
    if (!target || o->sig < 0) { vpi::error(vpiError, "vpi_put_value: the object cannot be written"); return nullptr; }
    Val val;
    if (!vpi::fromValue(o, v, val)) return nullptr;
    int mode = flags & 0xfff;
    if (o->kind == Obj::Call) { S.set(o->sig, val); return nullptr; }   // the function's result
    if (mode == vpiForceFlag || mode == vpiReleaseFlag) {   // like force / release (10.6.2, 38.35)
        if (o->kind == Obj::Call) { vpi::error(vpiError, "vpi_put_value: a function result cannot be forced"); return nullptr; }
        int hi = -1, lo = 0;
        if (o->kind == Obj::Bit) { lo = o->off; hi = o->off + o->w - 1; val = v_resize(val, o->w, false); }
        if (mode == vpiForceFlag) S.holdOn(o->sig, konst(val), true, hi, lo);
        else {
            S.holdOff(o->sig, true, hi, lo);
            vpi_get_value(h, v);   // the value it has now (38.35)
        }
        return nullptr;
    }
    if (mode == vpiCancelEvent) {
        vpi::error(vpiWarning, "vpi_put_value: cancel is not supported");
        return nullptr;
    }
    int64_t d = mode == vpiNoDelay || mode == 0 ? 0 : vpi::sigTicks(t, o);
    if (mode == vpiNoDelay || mode == 0) {
        vpi::write(o, val);
    } else if (d <= 0) {   // like a nonblocking assignment (38.35)
        if (o->kind == Obj::Bit) S.nba(o->sig, v_resize(val, o->w, false), o->off + o->w - 1, o->off);
        else S.nba(o->sig, val);
    } else {
        uint64_t when = S.now + (uint64_t)d;
        if (o->kind == Obj::Bit) {   // read-modify-write when it lands
            Val cur = S.get(o->sig);
            val = v_insert(cur, v_resize(val, o->w, false), o->off + o->w - 1, o->off);
        }
        vpi::st().puts.insert({when, {o->sig, val}});
        vpi::at(when);
    }
    return nullptr;
}

void vpi_get_time(vpiHandle h, p_vpi_time t) {
    vpi::clearErr();
    if (!t) return;
    if (t->type != vpiSimTime && t->type != vpiScaledRealTime) t->type = vpiSimTime;
    vpi::fillTime(t, O(h), S.now);
}

void vpi_get_delays(vpiHandle, p_vpi_delay) { vpi::error(vpiWarning, "vpi_get_delays is not supported"); }
void vpi_put_delays(vpiHandle, p_vpi_delay) { vpi::error(vpiWarning, "vpi_put_delays is not supported"); }

PLI_INT32 vpi_vprintf(const PLI_BYTE8* fmt, va_list ap) {
    vpi::clearErr();
    va_list cp;
    va_copy(cp, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, cp);
    va_end(cp);
    if (n < 0) return n;
    std::string b((size_t)n + 1, '\0');
    std::vsnprintf(b.data(), b.size(), fmt, ap);
    b.resize((size_t)n);
    std::cout << b;
    return n;
}
PLI_INT32 vpi_printf(const PLI_BYTE8* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vpi_vprintf(fmt, ap);
    va_end(ap);
    return n;
}
PLI_UINT32 vpi_mcd_open(const PLI_BYTE8* file) {
    vpi::clearErr();
    auto& s = vpi::st();
    for (int b = 1; b < 31; b++)
        if (!s.mcd[(size_t)b]) {
            std::FILE* f = std::fopen(file, "w");
            if (!f) return 0;
            s.mcd[(size_t)b] = f;
            s.mcdName[(size_t)b] = file;
            return 1u << b;
        }
    return 0;
}
PLI_UINT32 vpi_mcd_close(PLI_UINT32 mcd) {
    vpi::clearErr();
    auto& s = vpi::st();
    PLI_UINT32 bad = 0;
    for (int b = 1; b < 31; b++)
        if (mcd & (1u << b)) {
            if (s.mcd[(size_t)b]) { std::fclose(s.mcd[(size_t)b]); s.mcd[(size_t)b] = nullptr; }
            else bad |= 1u << b;
        }
    return bad;
}
PLI_BYTE8* vpi_mcd_name(PLI_UINT32 cd) {
    vpi::clearErr();
    auto& s = vpi::st();
    if (cd == 1) return vpi::retStr("stdout");
    for (int b = 1; b < 31; b++)
        if (cd == (1u << b) && s.mcd[(size_t)b]) return vpi::retStr(s.mcdName[(size_t)b]);
    return nullptr;
}
PLI_INT32 vpi_mcd_vprintf(PLI_UINT32 mcd, const PLI_BYTE8* fmt, va_list ap) {
    vpi::clearErr();
    auto& s = vpi::st();
    va_list cp;
    va_copy(cp, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, cp);
    va_end(cp);
    if (n < 0) return n;
    std::string b((size_t)n + 1, '\0');
    std::vsnprintf(b.data(), b.size(), fmt, ap);
    b.resize((size_t)n);
    if (mcd & 1) std::cout << b;
    for (int k = 1; k < 31; k++)
        if ((mcd & (1u << k)) && s.mcd[(size_t)k]) std::fputs(b.c_str(), s.mcd[(size_t)k]);
    return n;
}
PLI_INT32 vpi_mcd_printf(PLI_UINT32 mcd, const PLI_BYTE8* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vpi_mcd_vprintf(mcd, fmt, ap);
    va_end(ap);
    return n;
}
PLI_INT32 vpi_flush(void) { std::cout.flush(); return 0; }
PLI_INT32 vpi_mcd_flush(PLI_UINT32 mcd) {
    if (mcd & 1) std::cout.flush();
    for (int k = 1; k < 31; k++)
        if ((mcd & (1u << k)) && vpi::st().mcd[(size_t)k]) std::fflush(vpi::st().mcd[(size_t)k]);
    return 0;
}

PLI_INT32 vpi_compare_objects(vpiHandle a, vpiHandle b) { vpi::clearErr(); return a == b ? 1 : 0; }

PLI_INT32 vpi_chk_error(p_vpi_error_info e) {
    auto& s = vpi::st();
    if (!s.err) return 0;
    if (e) {
        static char product[] = "minisim";
        e->state = S.now || !s.cur ? vpiRun : vpiCompile;
        e->level = s.errLevel;
        e->message = s.errMsg.data();
        e->product = product;
        e->code = nullptr;
        e->file = nullptr;
        e->line = 0;
    }
    return s.errLevel;
}

PLI_INT32 vpi_free_object(vpiHandle h) {
    vpi::clearErr();
    Obj* o = O(h);
    if (o && o->kind == Obj::Iter) { o->items.clear(); o->kind = Obj::Event; }
    return 1;
}
PLI_INT32 vpi_release_handle(vpiHandle h) { return vpi_free_object(h); }

PLI_INT32 vpi_get_vlog_info(p_vpi_vlog_info info) {
    vpi::clearErr();
    auto& s = vpi::st();
    if (!info) return 0;
    static char product[] = "minisim";
    static char version[] = MINISIM_VERSION;
    s.argvp.clear();
    for (auto& a : s.argv) s.argvp.push_back(a.data());
    s.argvp.push_back(nullptr);
    info->argc = (PLI_INT32)s.argv.size();
    info->argv = s.argvp.data();
    info->product = product;
    info->version = version;
    return 1;
}

PLI_INT32 vpi_get_data(PLI_INT32, PLI_BYTE8*, PLI_INT32) { return 0; }
PLI_INT32 vpi_put_data(PLI_INT32, PLI_BYTE8*, PLI_INT32) { return 0; }

void* vpi_get_userdata(vpiHandle h) {
    vpi::clearErr();
    Obj* o = O(h);
    return o && o->kind == Obj::Call ? o->site->userdata : nullptr;
}
PLI_INT32 vpi_put_userdata(vpiHandle h, void* d) {
    vpi::clearErr();
    Obj* o = O(h);
    if (!o || o->kind != Obj::Call) return 0;
    o->site->userdata = d;
    return 1;
}

}   // extern "C"
namespace vpi {
inline PLI_INT32 control(PLI_INT32 op, va_list) {
    clearErr();
    if (op == vpiFinish || op == vpiStop) {
        S.finished = true;
        return 1;
    }
    error(vpiWarning, "vpi_control: operation " + std::to_string(op) + " is not supported");
    return 0;
}
inline vpiHandle handleMulti(PLI_INT32, vpiHandle, vpiHandle, va_list) {
    error(vpiWarning, "vpi_handle_multi is not supported");
    return nullptr;
}
// The elements a value-array call covers: `num` elements from the one at
// `index`, toward the right bound, going on from the left bound after it.
inline bool arraySpan(const char* who, const Obj* o, PLI_INT32* index, PLI_UINT32 num, const ArrShape*& sh, int64_t& first) {
    if (!o || o->kind != Obj::Signal || o->type != vpiMemory) { error(vpiError, std::string(who) + ": the object is not an array of variables"); return false; }
    sh = arrMap().find(o->sym)->second.get();
    if (!index) { error(vpiError, std::string(who) + ": no index"); return false; }
    auto [l, r] = sh->dims[0];
    first = l <= r ? index[0] - l : l - index[0];
    if (first < 0 || first >= sh->count) { error(vpiError, std::string(who) + ": index " + std::to_string(index[0]) + " is outside the array"); return false; }
    if ((int64_t)num > sh->count) { error(vpiError, std::string(who) + ": more elements asked for than the array has"); return false; }
    return true;
}
}   // namespace vpi
extern "C" {

PLI_INT32 vpi_control(PLI_INT32 op, ...) {
    va_list ap;
    va_start(ap, op);
    PLI_INT32 r = vpi::control(op, ap);
    va_end(ap);
    return r;
}

// 38.16: the values of `num` elements of an array in one call. Each element
// takes one entry of the format's C type; a vector takes ceil(size/32)
// s_vpi_vecval, and the raw formats ceil(size/8) bytes of value (two-state)
// followed, for vpiRawFourStateVal, by as many bytes of the unknown mask.
void vpi_get_value_array(vpiHandle h, p_vpi_arrayvalue av, PLI_INT32* index, PLI_UINT32 num) {
    vpi::clearErr();
    Obj* o = O(h);
    const ArrShape* sh = nullptr;
    int64_t first = 0;
    if (!av) { vpi::error(vpiError, "vpi_get_value_array: no value structure"); return; }
    if (!vpi::arraySpan("vpi_get_value_array", o, index, num, sh, first)) return;
    int w = sh->elemW, nb = (w + 7) / 8, nw = (w + 31) / 32;
    size_t each = 0;
    switch (av->format) {
        case vpiIntVal: each = sizeof(PLI_INT32); break;
        case vpiShortIntVal: each = sizeof(PLI_INT16); break;
        case vpiLongIntVal: each = sizeof(PLI_INT64); break;
        case vpiRealVal: each = sizeof(double); break;
        case vpiShortRealVal: each = sizeof(float); break;
        case vpiTimeVal: each = sizeof(s_vpi_time); break;
        case vpiVectorVal: each = sizeof(s_vpi_vecval) * (size_t)nw; break;
        case vpiRawTwoStateVal: each = (size_t)nb; break;
        case vpiRawFourStateVal: each = (size_t)nb * 2; break;
        default: vpi::error(vpiError, "vpi_get_value_array: unsupported format " + std::to_string(av->format)); return;
    }
    char* out = av->value.rawvals;
    if (!(av->flags & vpiUserAllocFlag)) {   // minisim's buffer, good until the next call
        vpi::st().arrBuf.assign(each * num + 8, 0);
        out = vpi::st().arrBuf.data();
        av->value.rawvals = out;
    } else if (!out) { vpi::error(vpiError, "vpi_get_value_array: vpiUserAllocFlag without a buffer"); return; }
    std::memset(out, 0, each * num);
    for (PLI_UINT32 k = 0; k < num; k++) {
        Val v = S.get(sh->base + (int)((first + k) % sh->count));
        char* e = out + each * k;
        auto bit = [&](int i) { return i < w ? bitAt(v, i) : (sh->sgn && w > 0 && bitAt(v, w - 1) == 1 ? 1 : 0); };
        auto integer = [&](int n) { uint64_t r = 0; for (int i = 0; i < n; i++) if (bit(i) == 1) r |= 1ULL << i; return r; };
        switch (av->format) {
            case vpiIntVal: { PLI_INT32 x = (PLI_INT32)integer(32); std::memcpy(e, &x, sizeof x); break; }
            case vpiShortIntVal: { PLI_INT16 x = (PLI_INT16)integer(16); std::memcpy(e, &x, sizeof x); break; }
            case vpiLongIntVal: { PLI_INT64 x = (PLI_INT64)integer(64); std::memcpy(e, &x, sizeof x); break; }
            case vpiRealVal: { double x = sh->sgn ? (double)(int64_t)integer(64) : (double)integer(64); std::memcpy(e, &x, sizeof x); break; }
            case vpiShortRealVal: { float x = sh->sgn ? (float)(int64_t)integer(64) : (float)integer(64); std::memcpy(e, &x, sizeof x); break; }
            case vpiTimeVal: {
                uint64_t x = integer(64);
                s_vpi_time tm{vpiSimTime, (PLI_UINT32)(x >> 32), (PLI_UINT32)x, (double)x};
                std::memcpy(e, &tm, sizeof tm);
                break;
            }
            case vpiVectorVal:
                for (int i = 0; i < w; i++) {
                    int b = bitAt(v, i);
                    s_vpi_vecval vv;
                    std::memcpy(&vv, e + sizeof vv * (size_t)(i / 32), sizeof vv);
                    if (b == 1 || b == 2) vv.aval |= 1u << (i % 32);
                    if (b == 2 || b == 3) vv.bval |= 1u << (i % 32);
                    std::memcpy(e + sizeof vv * (size_t)(i / 32), &vv, sizeof vv);
                }
                break;
            default:   // raw: value bytes, then (four-state) unknown bytes, lowest byte first
                for (int i = 0; i < w; i++) {
                    int b = bitAt(v, i);
                    if (av->format == vpiRawTwoStateVal) { if (b == 1) e[i / 8] |= (char)(1 << (i % 8)); continue; }
                    if (b == 1 || b == 2) e[i / 8] |= (char)(1 << (i % 8));
                    if (b == 2 || b == 3) e[nb + i / 8] |= (char)(1 << (i % 8));
                }
        }
    }
}

// 38.35: write `num` elements of an array in one call, with no delay. With
// vpiOneValue every element gets the first value. With vpiPropagateOff the
// elements change without waking the processes that wait on them.
void vpi_put_value_array(vpiHandle h, p_vpi_arrayvalue av, PLI_INT32* index, PLI_UINT32 num) {
    vpi::clearErr();
    Obj* o = O(h);
    const ArrShape* sh = nullptr;
    int64_t first = 0;
    if (!av || !av->value.rawvals) { vpi::error(vpiError, "vpi_put_value_array: no values"); return; }
    if (!vpi::arraySpan("vpi_put_value_array", o, index, num, sh, first)) return;
    int w = sh->elemW, nb = (w + 7) / 8, nw = (w + 31) / 32;
    size_t each = 0;
    switch (av->format) {
        case vpiIntVal: each = sizeof(PLI_INT32); break;
        case vpiShortIntVal: each = sizeof(PLI_INT16); break;
        case vpiLongIntVal: each = sizeof(PLI_INT64); break;
        case vpiRealVal: each = sizeof(double); break;
        case vpiShortRealVal: each = sizeof(float); break;
        case vpiTimeVal: each = sizeof(s_vpi_time); break;
        case vpiVectorVal: each = sizeof(s_vpi_vecval) * (size_t)nw; break;
        case vpiRawTwoStateVal: each = (size_t)nb; break;
        case vpiRawFourStateVal: each = (size_t)nb * 2; break;
        default: vpi::error(vpiError, "vpi_put_value_array: unsupported format " + std::to_string(av->format)); return;
    }
    const char* in = av->value.rawvals;
    for (PLI_UINT32 k = 0; k < num; k++) {
        const char* e = in + ((av->flags & vpiOneValue) ? 0 : each * k);
        Val v = make(0, 0, w);
        if (w > 64) v = maskWidth(v, w);
        auto integer = [&](int64_t x, int n) { for (int i = 0; i < w; i++) setBit(v, i, (int)((x >> std::min(i, n - 1)) & 1)); };
        switch (av->format) {
            case vpiIntVal: { PLI_INT32 x; std::memcpy(&x, e, sizeof x); integer(x, 32); break; }
            case vpiShortIntVal: { PLI_INT16 x; std::memcpy(&x, e, sizeof x); integer(x, 16); break; }
            case vpiLongIntVal: { PLI_INT64 x; std::memcpy(&x, e, sizeof x); integer(x, 64); break; }
            case vpiRealVal: { double x; std::memcpy(&x, e, sizeof x); integer((int64_t)std::llround(x), 64); break; }
            case vpiShortRealVal: { float x; std::memcpy(&x, e, sizeof x); integer((int64_t)std::llround(x), 64); break; }
            case vpiTimeVal: { s_vpi_time tm; std::memcpy(&tm, e, sizeof tm); integer((int64_t)(((uint64_t)tm.high << 32) | tm.low), 64); break; }
            case vpiVectorVal:
                for (int i = 0; i < w; i++) {
                    s_vpi_vecval vv;
                    std::memcpy(&vv, e + sizeof vv * (size_t)(i / 32), sizeof vv);
                    int a = (vv.aval >> (i % 32)) & 1, b = (vv.bval >> (i % 32)) & 1;
                    setBit(v, i, b ? (a ? 2 : 3) : a);
                }
                break;
            default:
                for (int i = 0; i < w; i++) {
                    int a = (e[i / 8] >> (i % 8)) & 1;
                    int b = av->format == vpiRawFourStateVal ? (e[nb + i / 8] >> (i % 8)) & 1 : 0;
                    setBit(v, i, b ? (a ? 2 : 3) : a);
                }
        }
        int sg = sh->base + (int)((first + k) % sh->count);
        if (av->flags & vpiPropagateOff) S.sigs[(size_t)sg].val = v;
        else S.set(sg, v);
    }
}

// 39.4.2: callbacks on assertions. The assertion API is not implemented yet.
vpiHandle vpi_register_assertion_cb(vpiHandle, PLI_INT32, vpi_assertion_callback_func*, PLI_BYTE8*) {
    vpi::error(vpiWarning, "vpi_register_assertion_cb is not supported");
    return nullptr;
}

// IEEE 1364 TF routines that VPI code from other simulators still calls (veriuser.h).
int tf_dofinish(void) { return vpi_control(vpiFinish, 0); }
int tf_dostop(void) { return vpi_control(vpiStop, 0); }

// ---- compatibility modes (36.12, annex L) ----
// An application built with VPI_COMPATIBILITY_VERSION_<std> calls these
// variants; each runs the routine in that standard's mode.
namespace vpi {
struct ModeScope {
    int saved;
    explicit ModeScope(int m) : saved(st().mode) { st().mode = m; }
    ~ModeScope() { st().mode = saved; }
};
}   // namespace vpi
vpiHandle vpi_handle_multi(PLI_INT32 type, vpiHandle a, vpiHandle b, ...) {
    va_list ap;
    va_start(ap, b);
    vpiHandle r = vpi::handleMulti(type, a, b, ap);
    va_end(ap);
    return r;
}
#define MSIM_VPI_COMPAT(sfx, mode) \
    PLI_INT32 vpi_compare_objects_##sfx(vpiHandle a, vpiHandle b) { vpi::ModeScope m(mode); return vpi_compare_objects(a, b); } \
    PLI_INT32 vpi_control_##sfx(PLI_INT32 op, ...) { vpi::ModeScope m(mode); va_list ap; va_start(ap, op); PLI_INT32 r = vpi::control(op, ap); va_end(ap); return r; } \
    PLI_INT32 vpi_get_##sfx(PLI_INT32 p, vpiHandle h) { vpi::ModeScope m(mode); return vpi_get(p, h); } \
    PLI_BYTE8* vpi_get_str_##sfx(PLI_INT32 p, vpiHandle h) { vpi::ModeScope m(mode); return vpi_get_str(p, h); } \
    void vpi_get_value_##sfx(vpiHandle h, p_vpi_value v) { vpi::ModeScope m(mode); vpi_get_value(h, v); } \
    vpiHandle vpi_handle_##sfx(PLI_INT32 t, vpiHandle h) { vpi::ModeScope m(mode); return vpi_handle(t, h); } \
    vpiHandle vpi_handle_by_index_##sfx(vpiHandle h, PLI_INT32 i) { vpi::ModeScope m(mode); return vpi_handle_by_index(h, i); } \
    vpiHandle vpi_handle_by_multi_index_##sfx(vpiHandle h, PLI_INT32 n, PLI_INT32* i) { vpi::ModeScope m(mode); return vpi_handle_by_multi_index(h, n, i); } \
    vpiHandle vpi_handle_by_name_##sfx(PLI_BYTE8* n, vpiHandle h) { vpi::ModeScope m(mode); return vpi_handle_by_name(n, h); } \
    vpiHandle vpi_handle_multi_##sfx(PLI_INT32 t, vpiHandle a, vpiHandle b, ...) { vpi::ModeScope m(mode); va_list ap; va_start(ap, b); vpiHandle r = vpi::handleMulti(t, a, b, ap); va_end(ap); return r; } \
    vpiHandle vpi_iterate_##sfx(PLI_INT32 t, vpiHandle h) { vpi::ModeScope m(mode); return vpi_iterate(t, h); } \
    vpiHandle vpi_put_value_##sfx(vpiHandle h, p_vpi_value v, p_vpi_time t, PLI_INT32 f) { vpi::ModeScope m(mode); return vpi_put_value(h, v, t, f); } \
    vpiHandle vpi_register_cb_##sfx(p_cb_data d) { vpi::ModeScope m(mode); return vpi_register_cb(d); } \
    vpiHandle vpi_scan_##sfx(vpiHandle h) { vpi::ModeScope m(mode); return vpi_scan(h); }
MSIM_VPI_COMPAT(1364v1995, vpiMode1364v1995)
MSIM_VPI_COMPAT(1364v2001, vpiMode1364v2001)
MSIM_VPI_COMPAT(1364v2005, vpiMode1364v2005)
MSIM_VPI_COMPAT(1800v2005, vpiMode1800v2005)
MSIM_VPI_COMPAT(1800v2009, vpiMode1800v2009)
#undef MSIM_VPI_COMPAT

}   // extern "C"
