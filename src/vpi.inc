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
#include "slang/ast/ASTVisitor.h"
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
    enum Kind { Systf, Call, Module, Signal, Word, Bit, Arg, Param, Port, Iter, Callback, Event,
                Scope, Typespec, Member, EnumConst, Const, Range, Modport, Assertion, Fsm, FsmState, SchedEvent,
                Process, ContAssign, Stmt, Expr, ExprStmt, CaseItem, Dyn, DynElem, ClassObj, ClassDefn, Extends, InterModPath, Tchk, TchkTerm, ModPath, PathTerm, Prim, PrimTerm, Constraint, ConstrItem } kind;
    int type = 0;                         // the vpi object type it reports
    int t05 = 0, t01 = 0;                 // what it reports instead to an IEEE 1364 (t05), 1364-1995 / 2001 (t01) application
    bool arr = false;                     // Signal: an unpacked array
    const Type* ty = nullptr;             // Typespec: the type; a variable, parameter or member: its type (array: of an element)
    Val cval;                             // Const, EnumConst: the value
    int64_t left = 0, right = 0;          // Range
    std::vector<int64_t> index;           // Word: its index in each dimension
    int idx = -1;                         // Systf: systf; Arg: argument; Word / Bit: element index; Fsm, FsmState: the FSM
    const VpiSite* site = nullptr;        // Call, Arg
    const Symbol* sym = nullptr;          // Module: InstanceSymbol; Scope: the scope's symbol; Signal / Param / Port / Word: the symbol
    const Obj* parent = nullptr;          // Word, Bit: the memory or vector; Arg: the call
    int sig = -1, w = 0;                  // the signal and its width (Word: element, Bit: the vector's signal)
    int off = 0;                          // Bit: bit offset in the vector (a part select: its lowest bit)
    int lidx = 0;                         // a part select v[idx:lidx]: its right index
    bool sgn = false, real = false, str = false;
    std::vector<vpiHandle> items;         // Iter
    size_t pos = 0;
    mutable int8_t prot = -1;             // vpiIsProtected, once worked out (-1: not yet)
    bool sched = false;                   // SchedEvent: still scheduled (vpiScheduled)
    const void* node = nullptr;           // Stmt: slang Statement; Expr, ExprStmt: Expression; CaseItem: the item group
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
    vpi_assertion_callback_func* assertFn = nullptr;   // an assertion callback (39.4.2)
    int assertId = -1;
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
    std::map<std::pair<const void*, int>, Obj*> codeObjs;   // statement and expression objects (vpi_code.inc)
    std::map<std::pair<const void*, const void*>, Obj*> pairObjs;   // struct members, class properties (vpi_data.inc)
    std::map<std::pair<const Obj*, std::string>, Obj*> dynElems;    // container elements by index or key
    std::unordered_map<int64_t, Obj*> classObjs;                    // class objects by handle
    std::unordered_map<const VpiSite*, Obj*> callObj;
    std::map<std::pair<const Obj*, int>, Obj*> subObj;   // memory words and bits
    std::unordered_map<int, Obj*> sysObj;
    std::unordered_map<const Type*, Obj*> typeObj;
    std::unordered_map<int64_t, Obj*> constObj;
    std::unordered_map<int, Obj*> assertObj;
    std::unordered_map<int, std::vector<Cb*>> assertCbs;   // assertion -> its callbacks (39.4.2)
    std::map<std::pair<int, const void*>, int> stepStates;   // (assertion, engine term) -> state number (39.5.2)
    std::vector<Cb*> assertSys;                            // callbacks on the assertion system (39.4.1)
    bool assertEnded = false;                              // vpiAssertionSysEnd
    // Coverage (40): per assertion its attempts, successes, failures, vacuous
    // successes, disabled and killed attempts; assertions whose counting is
    // stopped; signal -> its symbol; FSM and FSM state objects.
    std::vector<std::array<uint64_t, 6>> assertCov;
    std::vector<char> assertCovOff;
    bool assertCovAllOff = false;
    std::unordered_map<int, const Symbol*> sigSym;
    std::vector<const Prog*> progs;
    std::unordered_map<int, Obj*> fsmObj;
    std::map<std::pair<int, size_t>, Obj*> fsmStateObj;
    const VpiSite* cur = nullptr;         // the call whose calltf / compiletf is running
    std::vector<Cb*> start, endCompile, endSim, rw, ro, next;
    std::vector<Cb*> unresolved;          // cbUnresolvedSystf (38.36.3)
    std::multimap<uint64_t, Cb*> timed;   // cbAfterDelay, cbAtStartOfSimTime, and delayed synch callbacks
    std::unordered_map<int, std::vector<Cb*>> change;   // signal -> its cbValueChange callbacks
    std::vector<std::unique_ptr<Cb>> cbs;
    // A delayed vpi_put_value write: the object, the value, its event handle
    // (vpiReturnEvent) if one was asked for.
    struct Put { const Obj* o; Val v; Obj* ev; };
    std::multimap<uint64_t, Put> puts;
    std::vector<Cb*> holdCbs;             // cbForce, cbRelease, cbAssign, cbDeassign (38.36.1)
    std::map<std::pair<uint32_t, uint32_t>, std::vector<Cb*>> stmtCbs;   // cbStmt: by source file id and line
    std::vector<Cb*> tchkCbs;             // cbTchkViolation (38.36.1)
    std::vector<Cb*> createCbs, sizeCbs;  // cbCreateObj, cbSizeChange (38.36.1)
    std::vector<Cb*> errCbs;              // cbError, cbPLIError (38.36.3)
    bool inErrCb = false;
    std::vector<Cb*> nbaCbs, endSlot;     // cbNBASynch, cbAtEndOfSimTime of this time slot (38.36.2)
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
inline bool markStmt(const Obj* o, Cb* c);   // vpi_code.inc

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
inline void fireErrorCbs(int reason);
inline void onTchk(int id);
inline void onNewObj(int obj);
inline void onSizeChange(int sig);
inline void error(int level, const std::string& msg) {
    auto& s = st();
    s.err = true;
    s.errLevel = level;
    s.errMsg = msg;
    s.errFile.clear();
    s.errLine = 0;
    if (level >= vpiError) fireErrorCbs(cbPLIError);   // 38.36.3: an error in a VPI call
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
// The type an object reports in the mode of the running call (36.12): the
// SystemVerilog types do not exist for an IEEE 1364 application, and
// vpiMemory / vpiMemoryWord only for IEEE 1364-1995 and 1364-2001.
inline int effType(const Obj* o) {
    int m = st().mode;
    if (m <= vpiMode1364v2001 && o->t01) return o->t01;
    if (m <= vpiMode1364v2005 && o->t05) return o->t05;
    return o->type;
}
inline bool flatGenerate() { return st().mode <= vpiMode1364v2001; }
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
        auto dk = inst->getDefinition().definitionKind;
        m = mkObj(Obj::Module, dk == DefinitionKind::Interface ? vpiInterface : dk == DefinitionKind::Program ? vpiProgram : vpiModule);
        m->sym = inst;
    }
    return m;
}
// A scope that is not an instance: a package, generate scope, task,
// function or named block (37.3.x). Null for anything else.
// The return type of a function or of a pure virtual prototype.
inline const Type& subrReturnType(const Symbol& s) {
    if (s.kind == SymbolKind::MethodPrototype) return s.as<MethodPrototypeSymbol>().getReturnType();
    return s.as<SubroutineSymbol>().getReturnType();
}
inline Obj* scopeObj(const Symbol& s) {
    int type = 0;
    switch (s.kind) {
        case SymbolKind::Instance: return moduleObj(&s.as<InstanceSymbol>());
        case SymbolKind::InstanceBody: return moduleObj(s.as<InstanceBodySymbol>().parentInstance);
        case SymbolKind::Package: type = vpiPackage; break;
        case SymbolKind::GenerateBlock:
            if (s.as<GenerateBlockSymbol>().isUninstantiated) return nullptr;
            type = vpiGenScope;
            break;
        case SymbolKind::GenerateBlockArray: type = vpiGenScopeArray; break;
        case SymbolKind::Subroutine: type = s.as<SubroutineSymbol>().subroutineKind == SubroutineKind::Task ? vpiTask : vpiFunction; break;
        case SymbolKind::MethodPrototype:   // a pure virtual method (37.30): a task or function with no body
            if (!(s.as<MethodPrototypeSymbol>().flags & MethodFlags::Pure)) return nullptr;
            type = s.as<MethodPrototypeSymbol>().subroutineKind == SubroutineKind::Task ? vpiTask : vpiFunction;
            break;
        case SymbolKind::StatementBlock:
            if (s.name.empty()) return nullptr;
            type = s.as<StatementBlockSymbol>().blockKind == StatementBlockKind::Sequential ? vpiNamedBegin : vpiNamedFork;
            break;
        default: return nullptr;
    }
    auto& o = st().symObj[&s];
    if (!o) {
        o = mkObj(Obj::Scope, type);
        o->sym = &s;
    }
    return o;
}
// The scope object a symbol is declared in (38.18 vpiScope).
inline Obj* scopeOfSym(const Symbol& s) {
    for (const Scope* sc = s.getParentScope(); sc; sc = sc->asSymbol().getParentScope()) {
        const Symbol& p = sc->asSymbol();
        if (flatGenerate() && (p.kind == SymbolKind::GenerateBlock || p.kind == SymbolKind::GenerateBlockArray)) continue;
        if (Obj* o = scopeObj(p)) return o;
    }
    return nullptr;
}
inline const Scope* scopeOf(const Obj* o) {
    if (!o || !o->sym) return nullptr;
    if (o->kind == Obj::Module) return &o->sym->as<InstanceSymbol>().body;
    if (o->kind == Obj::Scope) return o->sym->isScope() ? &o->sym->as<Scope>() : nullptr;
    return nullptr;
}
inline int sigOfSym(const Symbol* s) {
    auto it = st().lr->sigOf.find(s);
    return it == st().lr->sigOf.end() ? -1 : it->second;
}
// The object type of a variable or net of type `t` (37.16, 37.17), and in
// `t05` what an IEEE 1364 application sees instead.
inline int varType(const Type& t, bool net, int& t05) {
    t05 = 0;
    if (t.isFloating()) {
        bool sh = t.as<FloatingType>().floatKind == FloatingType::ShortReal;
        if (net) { t05 = vpiNet; return sh ? vpiShortRealNet : vpiRealNet; }
        if (sh) t05 = vpiRealVar;
        return sh ? vpiShortRealVar : vpiRealVar;
    }
    if (t.isString()) return vpiStringVar;
    int old = net ? vpiNet : vpiReg;
    if (t.isEnum()) { t05 = old; return net ? vpiEnumNet : vpiEnumVar; }
    if (t.kind == SymbolKind::PackedStructType || t.kind == SymbolKind::UnpackedStructType) { t05 = old; return net ? vpiStructNet : vpiStructVar; }
    if (t.kind == SymbolKind::PackedUnionType || t.kind == SymbolKind::UnpackedUnionType) { t05 = old; return net ? vpiUnionNet : vpiUnionVar; }
    if (t.isPredefinedInteger()) {
        switch (t.as<PredefinedIntegerType>().integerKind) {
            case PredefinedIntegerType::Integer: if (net) t05 = vpiNet; return net ? vpiIntegerNet : vpiIntegerVar;
            case PredefinedIntegerType::Time: if (net) t05 = vpiNet; return net ? vpiTimeNet : vpiTimeVar;
            case PredefinedIntegerType::Int: t05 = old; return net ? vpiIntNet : vpiIntVar;
            case PredefinedIntegerType::ShortInt: t05 = old; return net ? vpiShortIntNet : vpiShortIntVar;
            case PredefinedIntegerType::LongInt: t05 = old; return net ? vpiLongIntNet : vpiLongIntVar;
            case PredefinedIntegerType::Byte: t05 = old; return net ? vpiByteNet : vpiByteVar;
        }
    }
    if (t.kind == SymbolKind::PackedArrayType) {
        const Type* e = &t;
        while (e->kind == SymbolKind::PackedArrayType) e = &e->as<PackedArrayType>().elementType.getCanonicalType();
        if (e->isEnum() || e->kind == SymbolKind::PackedStructType || e->kind == SymbolKind::PackedUnionType) {
            t05 = old;
            return net ? vpiPackedArrayNet : vpiPackedArrayVar;
        }
    }
    if (t.isIntegral()) {
        if (t.isFourState()) return old;
        t05 = old;
        return net ? vpiBitNet : vpiBitVar;
    }
    return 0;
}
inline Obj* valueObj(const Symbol* s);
inline Obj* typespecObj(const Type* t);
#include "vpi_data.inc"
// A variable or net as a VPI object, or null if VPI does not model it.
inline Obj* valueObj(const Symbol* s) {
    if (auto it = st().symObj.find(s); it != st().symObj.end()) return it->second;
    const Type& t = s->as<ValueSymbol>().getType().getCanonicalType();
    bool net = s->kind == SymbolKind::Net;
    Obj* o = nullptr;
    if (t.kind == SymbolKind::DynamicArrayType || t.kind == SymbolKind::QueueType || t.isAssociativeArray()) {
        o = containerObj(s, t);   // 37.18: a dynamic array, queue or associative array
        if (!o) return nullptr;
    } else if (t.isClass()) {
        o = classVarObj(s, t);    // 37.29
        if (!o) return nullptr;
    } else if (t.isUnpackedStruct() || t.isUnpackedUnion()) {   // one vector of its members (7.2)
        int sg = sigOfSym(s);
        if (sg < 0 || uWidth(t) <= 0) return nullptr;
        o = mkObj(Obj::Signal, t.isUnpackedStruct() ? (net ? vpiStructNet : vpiStructVar) : (net ? vpiUnionNet : vpiUnionVar));
        o->sig = sg;
        o->w = uWidth(t);
        o->ty = &t;
    } else if (t.isUnpackedArray()) {
        auto am = arrMap().find(s);
        if (am == arrMap().end() || am->second->dims.empty()) return nullptr;
        const Type* e = &t;
        while (e->isUnpackedArray() && e->getArrayElementType()) e = &e->getArrayElementType()->getCanonicalType();
        int e05 = 0, et = varType(*e, net, e05);
        if (!et || e->isString()) return nullptr;
        o = mkObj(Obj::Signal, net ? vpiNetArray : vpiRegArray);
        o->arr = true;
        // One dimension of reg vectors: a memory for IEEE 1364-1995 / 2001.
        if (!net && am->second->dims.size() == 1 && (e05 ? e05 : et) == vpiReg) o->t01 = vpiMemory;
        o->sig = am->second->base;
        o->w = am->second->elemW;
        o->sgn = am->second->sgn;
        o->real = e->isFloating();
        o->ty = e;
    } else {
        int sg = sigOfSym(s);
        if (sg < 0) return nullptr;
        if (!t.isIntegral() && !t.isFloating() && !t.isString()) return nullptr;
        int t05 = 0, type = varType(t, net, t05);
        if (!type) return nullptr;
        o = mkObj(Obj::Signal, type);
        o->t05 = t05;
        o->sig = sg;
        o->w = t.isFloating() || t.isString() ? 64 : (int)t.getBitWidth();
        o->sgn = t.isSigned();
        o->real = t.isFloating();
        o->str = t.isString();
        o->ty = &t;
    }
    o->sym = s;
    st().symObj[s] = o;
    return o;
}
inline Obj* paramObj(const Symbol* sym) {
    auto& po = st().symObj[sym];
    if (!po) {
        po = mkObj(Obj::Param, vpiParameter);
        po->sym = sym;
        const auto& t = sym->as<ParameterSymbol>().getType();
        po->w = t.isFloating() ? 64 : t.isString() ? 0 : (int)t.getBitWidth();
        po->sgn = t.isSigned();
        po->real = t.isFloating();
        po->str = t.isString();
        po->ty = &t.getCanonicalType();
    }
    return po;
}
inline Obj* portObj(const Symbol* p, int index) {
    auto& po = st().symObj[p];
    if (!po) {
        po = mkObj(Obj::Port, vpiPort);
        po->sym = p;
        po->idx = index;
    }
    return po;
}
// An integer constant object (a range bound, an index).
inline Obj* constObj(int64_t v) {
    auto& c = st().constObj[v];
    if (!c) {
        c = mkObj(Obj::Const, vpiConstant);
        c->w = 32;
        c->sgn = true;
        c->cval = make((uint64_t)v & 0xffffffffULL, 0, 32);
    }
    return c;
}
// Dimension `dim` of an object as a vpiRange (37.22): [left:right].
inline Obj* rangeObj(const Obj* owner, int dim, int64_t l, int64_t r) {
    auto& o = st().subObj[{owner, -1 - dim}];
    if (!o) {
        o = mkObj(Obj::Range, vpiRange);
        o->parent = owner;
        o->left = l;
        o->right = r;
        o->w = (int)((l > r ? l - r : r - l) + 1);
    }
    return o;
}
// The typespec of a type (37.25): canonical per type, so two variables of
// one type give the same handle.
inline Obj* typespecObj(const Type* t) {
    if (!t) return nullptr;
    auto& o = st().typeObj[t];
    if (o) return o;
    const Type& c = t->getCanonicalType();
    int type = 0;
    if (c.isUnpackedArray()) type = vpiArrayTypespec;
    else if (c.isFloating()) type = c.as<FloatingType>().floatKind == FloatingType::ShortReal ? vpiShortRealTypespec : vpiRealTypespec;
    else if (c.isString()) type = vpiStringTypespec;
    else if (c.isEnum()) type = vpiEnumTypespec;
    else if (c.kind == SymbolKind::PackedStructType || c.kind == SymbolKind::UnpackedStructType) type = vpiStructTypespec;
    else if (c.kind == SymbolKind::PackedUnionType || c.kind == SymbolKind::UnpackedUnionType) type = vpiUnionTypespec;
    else if (c.isPredefinedInteger()) {
        switch (c.as<PredefinedIntegerType>().integerKind) {
            case PredefinedIntegerType::Integer: type = vpiIntegerTypespec; break;
            case PredefinedIntegerType::Time: type = vpiTimeTypespec; break;
            case PredefinedIntegerType::Int: type = vpiIntTypespec; break;
            case PredefinedIntegerType::ShortInt: type = vpiShortIntTypespec; break;
            case PredefinedIntegerType::LongInt: type = vpiLongIntTypespec; break;
            case PredefinedIntegerType::Byte: type = vpiByteTypespec; break;
        }
    } else if (c.kind == SymbolKind::PackedArrayType) {
        const Type* e = &c;
        while (e->kind == SymbolKind::PackedArrayType) e = &e->as<PackedArrayType>().elementType.getCanonicalType();
        if (e->isEnum() || e->kind == SymbolKind::PackedStructType || e->kind == SymbolKind::PackedUnionType) type = vpiPackedArrayTypespec;
        else type = e->isFourState() ? vpiLogicTypespec : vpiBitTypespec;
    } else if (c.isIntegral()) type = c.isFourState() ? vpiLogicTypespec : vpiBitTypespec;
    else if (c.isClass()) type = vpiClassTypespec;
    else if (c.isCHandle()) type = vpiChandleTypespec;
    else if (c.isEvent()) type = vpiEventTypespec;
    else if (c.isVoid()) type = vpiVoidTypespec;
    else if (c.isVirtualInterface()) type = vpiInterfaceTypespec;
    else return nullptr;
    o = mkObj(Obj::Typespec, type);
    o->ty = t;
    o->w = c.isIntegral() || c.isFloating() ? (int)c.getBitWidth() : 0;
    o->sgn = c.isIntegral() && c.isSigned();
    o->real = c.isFloating();
    o->str = c.isString();
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
        case Obj::Scope:     // a task, block or generate scope of protected code
        case Obj::Modport:
            p = protectedInstance(instOf(o->sym->getParentScope())) || (hideMsgIf() && hideMsgIf()(o->sym->location));
            break;
        case Obj::Word:
        case Obj::Bit:
        case Obj::Range: p = isProt(o->parent); break;
        case Obj::Call:
        case Obj::Arg: p = o->site && o->site->prot; break;
        case Obj::Assertion: p = asserts()[(size_t)o->idx].prot; break;
        case Obj::Process:
        case Obj::ContAssign:
        case Obj::Stmt:
        case Obj::Expr:
        case Obj::ExprStmt:
        case Obj::CaseItem: {   // code inside protected IP
            const Obj* w = o;
            while (w && w->kind != Obj::Process && w->kind != Obj::ContAssign) w = w->parent;
            p = w && w->sym && protectedInstance(instOf(w->sym->getParentScope()));
            break;
        }
        case Obj::Dyn: p = protSym(o->sym); break;
        case Obj::DynElem: p = isProt(o->parent); break;
        case Obj::Fsm:
        case Obj::FsmState: {   // the state register's declaration
            auto it = st().sigSym.find(fsmDefs()[(size_t)o->idx].sig);
            p = it != st().sigSym.end() && protSym(it->second);
            break;
        }
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
// A range [left:right] of a vector, for bit indices.
inline std::pair<int64_t, int64_t> rangeOf(const Obj* o) {
    if (o->ty && !o->arr && (o->kind == Obj::Signal || o->kind == Obj::Word)) {
        const Type& t = o->ty->getCanonicalType();
        if (t.isIntegral() && !t.isUnpackedArray() && t.hasFixedRange()) {
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
        const Obj* root = vec->kind == Obj::Word ? vec->parent : vec;
        b = mkObj(Obj::Bit, root->sym && root->sym->kind == SymbolKind::Net ? vpiNetBit : vpiRegBit);
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
// One element of an array, by its flat offset.
inline Obj* wordAt(const Obj* mem, int64_t off) {
    auto am = arrMap().find(mem->sym);
    if (am == arrMap().end() || off < 0 || off >= am->second->count) return nullptr;
    auto& b = st().subObj[{mem, (int)off}];
    if (!b) {
        int e05 = 0, et = varType(*mem->ty, mem->type == vpiNetArray, e05);
        b = mkObj(Obj::Word, et);
        b->t05 = e05;
        if (mem->t01) b->t01 = vpiMemoryWord;
        b->parent = mem;
        b->sym = mem->sym;
        b->sig = mem->sig + (int)off;
        b->w = mem->w;
        b->sgn = mem->sgn;
        b->real = mem->real;
        b->ty = mem->ty;
        // Its index in each dimension, the last one in idx.
        int64_t rem = off;
        const auto& dims = am->second->dims;
        b->index.resize(dims.size());
        for (size_t d = dims.size(); d-- > 0;) {
            auto [l, r] = dims[d];
            int64_t n = (l > r ? l - r : r - l) + 1, k = rem % n;
            rem /= n;
            b->index[d] = l <= r ? l + k : l - k;
        }
        b->idx = (int)b->index.back();
    }
    return b;
}
inline Obj* wordObj(const Obj* mem, const std::vector<int64_t>& idx) {
    auto am = arrMap().find(mem->sym);
    if (am == arrMap().end() || idx.size() != am->second->dims.size()) return nullptr;
    return wordAt(mem, am->second->offset(idx));
}
inline Obj* wordObj(const Obj* mem, int64_t i) { return wordObj(mem, std::vector<int64_t>{i}); }
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
// ---- values ----
inline int64_t sigTicks(const p_vpi_time t, const Obj* o);
inline Val readObj(const Obj* o) {
    if (o->kind == Obj::Const || o->kind == Obj::EnumConst || o->kind == Obj::FsmState) return o->cval;
    if (o->kind == Obj::DynElem) return dynRead(o);
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
    if (o->str) { v = make(v.v, v.x, 64); v.str = true; }   // a string variable holds the string's id
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
    if (o->type == vpiIntegerVar || o->type == vpiIntVar || o->kind == Obj::Const) return vpiIntVal;
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
        else if (o->kind == Obj::Word || o->kind == Obj::Bit || o->kind == Obj::Range) return unitExp(o->parent);
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
// cbError and cbPLIError (38.36.3): vpi_chk_error in the callback reports
// the error. A callback that itself fails does not call them again.
inline void fireErrorCbs(int reason) {
    auto& s = st();
    if (s.inErrCb) return;
    s.inErrCb = true;
    std::vector<Cb*> due;
    for (Cb* c : s.errCbs)
        if (c->live && c->d.reason == reason) due.push_back(c);
    bool err = s.err;
    int lvl = s.errLevel;
    std::string msg = s.errMsg;
    for (Cb* c : due) {
        s_cb_data d = c->d;
        s_vpi_time t = c->t;
        d.obj = nullptr;
        d.value = nullptr;
        d.time = nullptr;
        if (c->hasT && t.type != vpiSuppressTime) {
            fillTime(&t, nullptr, S.now);
            d.time = &t;
        }
        s.err = err;   // what the callback's vpi_chk_error sees
        s.errLevel = lvl;
        s.errMsg = msg;
        d.cb_rtn(&d);
    }
    s.err = err;
    s.errLevel = lvl;
    s.errMsg = msg;
    s.inErrCb = false;
}
// A run-time error message of the simulation: its first line, the location
// and scope, without the time.
inline void onRunError(const std::string& fmt) {
    std::string m = fmt.substr(0, fmt.find('\n'));
    if (auto k = m.find(": at time"); k != std::string::npos) m.resize(k);
    std::string clean;
    for (size_t k = 0; k < m.size(); k++) {
        if (m[k] == '%' && k + 1 < m.size() && m[k + 1] == '%') k++;
        clean += m[k];
    }
    auto& s = st();
    s.err = true;
    s.errLevel = vpiError;
    s.errMsg = clean;
    s.errFile.clear();
    s.errLine = 0;
    fireErrorCbs(cbError);
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
// A force, release, procedural assign or deassign of signal `sg` (38.36.1):
// the callbacks on that object (or on any, obj NULL) get the object and,
// unless vpiSuppressVal, its value after the change.
inline void onHold(int sg, bool force, bool on, int hi, int lo) {
    auto& s = st();
    int reason = force ? (on ? cbForce : cbRelease) : (on ? cbAssign : cbDeassign);
    auto list = s.holdCbs;
    for (Cb* c : list) {
        if (!c->live || c->d.reason != reason) continue;
        Obj* o = c->obj;
        if (o) {   // the object, or a part of it, or the whole of which it is a part
            if (o->sig != sg) continue;
            if (o->kind == Obj::Bit && hi >= 0 && (o->off > hi || o->off + o->w - 1 < lo)) continue;
        } else {
            auto it = s.sigSym.find(sg);
            o = it == s.sigSym.end() ? nullptr : valueObj(it->second);
            if (o && o->arr) o = wordAt(o, sg - arrMap().find(o->sym)->second->base);
            if (o && isProt(o)) continue;   // nothing of protected code (IEEE 1735 9.7.1)
        }
        s_cb_data d = c->d;
        s_vpi_time t = c->t;
        s_vpi_value v = c->v;
        d.obj = H(o);
        d.time = c->hasT ? &t : nullptr;
        d.value = c->hasV ? &v : nullptr;
        if (c->hasT && t.type != vpiSuppressTime) fillTime(&t, o, S.now);
        if (c->hasV && o && v.format != vpiSuppressVal) getValue(o, &v);
        d.cb_rtn(&d);
    }
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
        std::vector<State::Put> writes;
        for (auto it = pa; it != pb; ++it) writes.push_back(it->second);
        s.puts.erase(pa, pb);
        for (auto& w : writes) {
            if (w.ev) w.ev->sched = false;
            if (w.o->kind == Obj::Bit)   // the other bits keep the value they have when it lands
                S.set(w.o->sig, v_insert(S.get(w.o->sig), v_resize(w.v, w.o->w, false), w.o->off + w.o->w - 1, w.o->off));
            else S.set(w.o->sig, w.v);
        }
        std::vector<Cb*> nxt;
        for (Cb* c : s.next) (c->at < S.now ? due : nxt).push_back(c);
        s.next.swap(nxt);
        for (Cb* c : due) {
            if (c->d.reason == cbReadWriteSynch) s.rw.push_back(c);
            else if (c->d.reason == cbReadOnlySynch) s.ro.push_back(c);
            else if (c->d.reason == cbAtEndOfSimTime) s.endSlot.push_back(c);
            else if (c->d.reason == cbNBASynch) { s.nbaCbs.push_back(c); S.vpiNbaPend = true; }
            else fire(c);
        }
        if (!s.rw.empty() || !s.ro.empty() || !s.endSlot.empty()) S.rare = true;
        return false;
    }
    if (r == Sim::VR_NBA) {   // cbNBASynch: before the NBA updates of the slot
        S.vpiNbaPend = false;
        if (s.nbaCbs.empty()) return false;
        fireList(s.nbaCbs);
        return true;
    }
    if (r == Sim::VR_RO && !s.endSlot.empty()) {   // cbAtEndOfSimTime, then cbReadOnlySynch
        fireList(s.endSlot);
        S.rare = true;
        return true;
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

// ---- the assertion API (clause 39) ----
// Every assertion, assumption and cover of the design, concurrent or
// immediate, is a VPI object (39.3). Callbacks on one assertion are placed
// with vpi_register_assertion_cb (39.4.2); callbacks on the assertion
// system with vpi_register_cb (39.4.1). The kernel reports what happens
// through assertHook (sva.hpp); vpi_control drives assertion control (39.5).
inline int assertType(const AssertInfo& a) {
    bool imm = !(a.type & AT_CONCURRENT);
    if (a.dir == AD_COVER) return imm ? vpiImmediateCover : vpiCover;
    if (a.dir == AD_ASSUME) return imm ? vpiImmediateAssume : vpiAssume;
    return imm ? vpiImmediateAssert : vpiAssert;
}
inline Obj* assertObj(int id) {
    if (id < 0 || id >= (int)asserts().size() || asserts()[(size_t)id].hidden) return nullptr;
    auto& o = st().assertObj[id];
    if (!o) {
        o = mkObj(Obj::Assertion, assertType(asserts()[(size_t)id]));
        o->idx = id;
        o->sym = static_cast<const Symbol*>(nullptr);
    }
    return o;
}
// The assertions directly in a scope: in the instance (not below it), or in
// a task, block or generate scope.
inline bool assertIn(const AssertInfo& a, const Obj* sc) {
    if (!a.scope || !sc || !sc->sym) return false;
    for (const Scope* s = static_cast<const Scope*>(a.scope); s; s = s->asSymbol().getParentScope()) {
        const Symbol& y = s->asSymbol();
        if (sc->kind == Obj::Module && y.kind == SymbolKind::InstanceBody) return y.as<InstanceBodySymbol>().parentInstance == sc->sym;
        if (&y == sc->sym) return true;
        if (y.kind == SymbolKind::InstanceBody) return false;
    }
    return false;
}
// The reasons an assertion callback may have, and the system ones (39.4).
inline bool assertReason(int r) {
    switch (r) {
        case cbAssertionStart: case cbAssertionSuccess: case cbAssertionFailure: case cbAssertionVacuousSuccess:
        case cbAssertionDisabledEvaluation: case cbAssertionStepSuccess: case cbAssertionStepFailure:
        case cbAssertionLock: case cbAssertionUnlock: case cbAssertionDisable: case cbAssertionEnable:
        case cbAssertionReset: case cbAssertionKill: case cbAssertionEnablePassAction:
        case cbAssertionEnableFailAction: case cbAssertionDisablePassAction: case cbAssertionDisableFailAction:
        case cbAssertionEnableNonvacuousAction: case cbAssertionDisableVacuousAction:
            return true;
        default: return false;
    }
}
inline bool assertSysReason(int r) {
    switch (r) {
        case cbAssertionSysInitialized: case cbAssertionSysOn: case cbAssertionSysOff: case cbAssertionSysKill:
        case cbAssertionSysLock: case cbAssertionSysUnlock: case cbAssertionSysEnd: case cbAssertionSysReset:
        case cbAssertionSysEnablePassAction: case cbAssertionSysEnableFailAction: case cbAssertionSysDisablePassAction:
        case cbAssertionSysDisableFailAction: case cbAssertionSysEnableNonvacuousAction:
        case cbAssertionSysDisableVacuousAction:
            return true;
        default: return false;
    }
}
// The callback reason of an assertion control (Table 20-5), for one
// assertion or for the system.
inline int ctlReason(int ctl, bool sys) {
    switch (ctl) {
        case AC_LOCK: return sys ? cbAssertionSysLock : cbAssertionLock;
        case AC_UNLOCK: return sys ? cbAssertionSysUnlock : cbAssertionUnlock;
        case AC_ON: return sys ? cbAssertionSysOn : cbAssertionEnable;
        case AC_OFF: return sys ? cbAssertionSysOff : cbAssertionDisable;
        case AC_KILL: return sys ? cbAssertionSysKill : cbAssertionKill;
        case AC_PASSON: return sys ? cbAssertionSysEnablePassAction : cbAssertionEnablePassAction;
        case AC_PASSOFF: return sys ? cbAssertionSysDisablePassAction : cbAssertionDisablePassAction;
        case AC_FAILON: return sys ? cbAssertionSysEnableFailAction : cbAssertionEnableFailAction;
        case AC_FAILOFF: return sys ? cbAssertionSysDisableFailAction : cbAssertionDisableFailAction;
        case AC_NONVACON: return sys ? cbAssertionSysEnableNonvacuousAction : cbAssertionEnableNonvacuousAction;
        case AC_VACOFF: return sys ? cbAssertionSysDisableVacuousAction : cbAssertionDisableVacuousAction;
        default: return 0;
    }
}
inline void fireSys(int reason) {
    auto list = st().assertSys;   // a callback may register or remove others
    for (Cb* c : list) {
        if (!c->live || c->d.reason != reason) continue;
        s_cb_data d = c->d;
        s_vpi_time t{vpiSimTime, (PLI_UINT32)(S.now >> 32), (PLI_UINT32)S.now, (double)S.now};
        d.time = &t;
        d.value = nullptr;
        d.cb_rtn(&d);
    }
}
inline void onAssert(int id, int ev, uint64_t start) {
    auto& s = st();
    if (id < 0) {   // the assertion system
        if (ev >= AE_CTL) fireSys(ctlReason(ev - AE_CTL, true));
        return;
    }
    // Assertion coverage (40.5): an attempt, its outcome. Relaxed atomic
    // adds: assertions of two processes may end on two worker threads.
    if ((size_t)id < s.assertCov.size() && !s.assertCovAllOff && !s.assertCovOff[(size_t)id] && ev <= AE_KILLED) {
        int k = ev == AE_START ? 0 : ev == AE_SUCCESS ? 1 : ev == AE_FAILURE ? 2 : ev == AE_VACUOUS ? 3 : ev == AE_DISABLED ? 4 : ev == AE_KILLED ? 5 : -1;
        if (k >= 0) __atomic_fetch_add(&s.assertCov[(size_t)id][(size_t)k], 1, __ATOMIC_RELAXED);
    }
    int reason = 0;
    switch (ev) {
        case AE_START: reason = cbAssertionStart; break;
        case AE_SUCCESS: reason = cbAssertionSuccess; break;
        case AE_VACUOUS: reason = cbAssertionVacuousSuccess; break;
        case AE_FAILURE: reason = cbAssertionFailure; break;
        case AE_DISABLED: reason = cbAssertionDisabledEvaluation; break;
        case AE_STEP_OK: reason = cbAssertionStepSuccess; break;
        case AE_STEP_FAIL: reason = cbAssertionStepFailure; break;
        case AE_RESET: reason = cbAssertionReset; break;
        case AE_KILLED: return;   // an attempt that $assertkill ended: no outcome (39.4.2)
        default: reason = ev >= AE_CTL ? ctlReason(ev - AE_CTL, false) : 0;
    }
    auto it = s.assertCbs.find(id);
    if (!reason || it == s.assertCbs.end()) return;
    auto list = it->second;
    Obj* a = assertObj(id);
    for (Cb* c : list) {
        if (!c->live || c->d.reason != reason) continue;
        s_vpi_time t{vpiSimTime, (PLI_UINT32)(S.now >> 32), (PLI_UINT32)S.now, (double)S.now};
        s_vpi_attempt_info info{};
        info.attemptStartTime = s_vpi_time{vpiSimTime, (PLI_UINT32)(start >> 32), (PLI_UINT32)start, (double)start};
        info.detail.failExpr = nullptr;
        s_vpi_assertion_step_info step{};
        if (ev == AE_STEP_OK || ev == AE_STEP_FAIL) {   // 39.5.2: the states are the engine's, numbered per assertion as first seen
            auto num = [&](const void* n) {
                auto [it2, fresh] = s.stepStates.try_emplace({id, n}, 0);
                if (fresh) {
                    int k = 0;
                    for (auto& [key, v] : s.stepStates)
                        if (key.first == id && key.second != n) k++;
                    it2->second = k;
                }
                return it2->second;
            };
            step.stateFrom = num(assertStep().from);
            step.stateTo = num(assertStep().to);
            info.detail.step = &step;
        }
        bool attempt = ev <= AE_DISABLED || ev == AE_STEP_OK || ev == AE_STEP_FAIL;
        c->assertFn(reason, &t, H(a), attempt ? &info : nullptr, c->d.user_data);
    }
}
// vpi_control for assertions (39.5): on one assertion (a handle follows the
// operation) or on the whole system.
inline PLI_INT32 assertControlOp(PLI_INT32 op, va_list ap) {
    int ctl = 0;
    bool sys = false;
    switch (op) {
        case vpiAssertionLock: ctl = AC_LOCK; break;
        case vpiAssertionUnlock: ctl = AC_UNLOCK; break;
        case vpiAssertionDisable: ctl = AC_OFF; break;
        case vpiAssertionEnable: ctl = AC_ON; break;
        case vpiAssertionKill: ctl = AC_KILL; break;
        case vpiAssertionReset: ctl = -1; break;
        case vpiAssertionDisablePassAction: ctl = AC_PASSOFF; break;
        case vpiAssertionEnablePassAction: ctl = AC_PASSON; break;
        case vpiAssertionDisableFailAction: ctl = AC_FAILOFF; break;
        case vpiAssertionEnableFailAction: ctl = AC_FAILON; break;
        case vpiAssertionDisableVacuousAction: ctl = AC_VACOFF; break;
        case vpiAssertionEnableNonvacuousAction: ctl = AC_NONVACON; break;
        case vpiAssertionSysLock: ctl = AC_LOCK; sys = true; break;
        case vpiAssertionSysUnlock: ctl = AC_UNLOCK; sys = true; break;
        case vpiAssertionSysOn: ctl = AC_ON; sys = true; break;
        case vpiAssertionSysOff: ctl = AC_OFF; sys = true; break;
        case vpiAssertionSysKill: ctl = AC_KILL; sys = true; break;
        case vpiAssertionSysEnd: ctl = -2; sys = true; break;
        case vpiAssertionSysReset: ctl = -1; sys = true; break;
        case vpiAssertionSysEnablePassAction: ctl = AC_PASSON; sys = true; break;
        case vpiAssertionSysDisablePassAction: ctl = AC_PASSOFF; sys = true; break;
        case vpiAssertionSysEnableFailAction: ctl = AC_FAILON; sys = true; break;
        case vpiAssertionSysDisableFailAction: ctl = AC_FAILOFF; sys = true; break;
        case vpiAssertionSysEnableNonvacuousAction: ctl = AC_NONVACON; sys = true; break;
        case vpiAssertionSysDisableVacuousAction: ctl = AC_VACOFF; sys = true; break;
        case vpiAssertionEnableStep:
        case vpiAssertionDisableStep: {   // 39.5.2: report each clock step of the assertion's attempts
            Obj* o = O(va_arg(ap, vpiHandle));
            if (!o || o->kind != Obj::Assertion) {
                error(vpiError, "vpi_control: assertion stepping needs an assertion handle");
                return 0;
            }
            if (denied(o, "vpi_control")) return 0;
            AssertInfo& a = asserts()[(size_t)o->idx];
            if (a.type != AT_CONCURRENT) {   // a concurrent assertion: immediate ones have no steps
                error(vpiError, "vpi_control: only a concurrent assertion has clock steps");
                return 0;
            }
            a.step = op == vpiAssertionEnableStep;
            return 1;
        }
        case vpiAssertionClockSteps:
            error(vpiError, "vpi_control: vpiAssertionClockSteps is not supported");
            return 0;
        default: return -1;   // not an assertion operation
    }
    std::vector<int> ids;
    if (sys) {
        for (size_t k = 0; k < asserts().size(); k++)
            if (!asserts()[k].hidden) ids.push_back((int)k);
    } else {
        Obj* o = O(va_arg(ap, vpiHandle));
        if (!o || o->kind != Obj::Assertion) {
            error(vpiError, "vpi_control: an assertion operation needs an assertion handle");
            return 0;
        }
        ids.push_back(o->idx);
    }
    if (ctl > 0) {
        if (sys) assertControl(ctl, 15, 7, 0, {});
        else assertControl(ctl, 255, 7, 0, {asserts()[(size_t)ids[0]].name});
    } else {
        // Reset: the attempts in flight are dropped and the assertion goes
        // back to its state at the start (enabled, actions on, unlocked).
        // SysEnd: the attempts are dropped and no more start.
        for (int id : ids) {
            AssertInfo& a = asserts()[(size_t)id];
            a.killGen++;
            if (ctl == -1) {
                a.on = a.passOn = a.failOn = a.vacOn = true;
                a.locked = false;
                if (!sys) onAssert(id, AE_RESET, 0);
            } else a.on = false;
        }
        if (sys) fireSys(ctl == -1 ? cbAssertionSysReset : cbAssertionSysEnd);
        if (sys && ctl == -2) st().assertEnded = true;
    }
    return 1;
}

// ---- coverage (40) ----
// What msim collects with -cov: statements (each source line of a process
// counts once), toggles (a bit that rose and fell), FSM states; and, always,
// assertion attempts and outcomes, counted here from the assertion events.
inline void covInit() {
    auto& s = st();
    s.assertCov.assign(asserts().size(), {});
    s.assertCovOff.assign(asserts().size(), 0);
    for (auto& [sym, sig] : s.lr->sigOf) s.sigSym.emplace(sig, sym);
    for (auto& [sym, sh] : arrMap())
        if (sh && sh->base >= 0 && s.lr->sigOf.count(sym))
            for (int k = 0; k < sh->count; k++) s.sigSym.emplace(sh->base + k, sym);
    std::unordered_set<const Prog*> seen;
    std::function<void(const Prog*)> walk = [&](const Prog* p) {
        if (!p || !seen.insert(p).second) return;
        s.progs.push_back(p);
        for (auto& i : p->code) {
            if (i.callee) walk(i.callee);
            for (auto* b : i.branches) walk(b);
        }
    };
    for (auto& p : s.lr->procs) walk(&p.prog);
    for (auto& p : s.lr->finals) walk(&p.prog);
    for (auto& p : s.lr->subs) walk(p.get());
}
// The instance a signal is declared in.
inline const InstanceSymbol* sigInst(int sig) {
    auto it = st().sigSym.find(sig);
    return it == st().sigSym.end() ? nullptr : instOf(it->second->getParentScope());
}
inline Obj* fsmObj(int k) {
    Obj*& o = st().fsmObj[k];
    if (!o) {
        o = mkObj(Obj::Fsm, vpiFsm);
        o->idx = k;
        o->sig = fsmDefs()[(size_t)k].sig;
        o->w = fsmDefs()[(size_t)k].w;
    }
    return o;
}
inline Obj* fsmStateObj(int k, size_t n) {
    Obj*& o = st().fsmStateObj[{k, n}];
    if (!o) {
        const FsmDef& d = fsmDefs()[(size_t)k];
        o = mkObj(Obj::FsmState, vpiFsmHandle);
        o->idx = k;
        o->pos = n;
        o->parent = fsmObj(k);
        o->w = d.w;
        o->cval = make(d.states[n].first, 0, d.w);
    }
    return o;
}
inline uint64_t fsmSeenCount(int k, uint64_t v) {
    if ((size_t)k >= S.fsmSeen.size()) return 0;
    auto it = S.fsmSeen[(size_t)k].find(v);
    return it == S.fsmSeen[(size_t)k].end() ? 0 : it->second;
}
// Bits of signal `sig` (bits [lo, lo+w)) that rose and fell.
inline int toggled(int sig, int lo, int w) {
    if (sig < 0 || (size_t)sig >= S.togRise.size()) return 0;
    const auto& R = S.togRise[(size_t)sig];
    const auto& F = S.togFall[(size_t)sig];
    int n = 0;
    for (int i = lo; i < lo + w; i++) {
        size_t q = (size_t)i / 64;
        if (q < R.size() && q < F.size() && (R[q] >> (i % 64) & 1) && (F[q] >> (i % 64) & 1)) n++;
    }
    return n;
}
// Statement coverage of an instance (null: the whole design): source lines
// of its processes that ran, and all of them.
inline std::pair<int, int> stmtCov(const InstanceSymbol* inst) {
    std::map<std::pair<uint32_t, uint32_t>, bool> lines;
    for (const Prog* p : st().progs) {
        if (inst && p->inst != inst) continue;
        for (auto& i : p->code)
            if (i.line) {
                bool& hit = lines[{i.file, i.line}];
                hit = hit || i.hits;
            }
    }
    int n = 0;
    for (auto& [k, h] : lines) n += h;
    return {n, (int)lines.size()};
}
// Toggle coverage of an instance: bits that rose and fell, and all bits.
inline std::pair<int, int> togCov(const InstanceSymbol* inst) {
    int n = 0, m = 0;
    for (size_t g = 0; g < S.sigs.size(); g++) {
        if (S.sigs[g].hide || (inst && sigInst((int)g) != inst)) continue;
        n += toggled((int)g, 0, S.sigs[g].w);
        m += S.sigs[g].w;
    }
    return {n, m};
}
// FSM state coverage of an instance: states entered, and all states.
inline std::pair<int, int> fsmCov(const InstanceSymbol* inst) {
    int n = 0, m = 0;
    for (size_t k = 0; k < fsmDefs().size(); k++) {
        if (inst && sigInst(fsmDefs()[k].sig) != inst) continue;
        for (auto& [v, name] : fsmDefs()[k].states) { m++; n += fsmSeenCount((int)k, v) > 0; }
    }
    return {n, m};
}
inline bool assertIn(size_t id, const InstanceSymbol* inst) {
    const AssertInfo& a = asserts()[id];
    return !a.hidden && (!inst || instOf(static_cast<const Scope*>(a.scope)) == inst);
}
// Assertion coverage of an instance: assertions that succeeded (a cover
// statement: matched), and all assertions.
inline std::pair<int, int> assertCovOf(const InstanceSymbol* inst) {
    int n = 0, m = 0;
    for (size_t k = 0; k < asserts().size(); k++)
        if (assertIn(k, inst)) { m++; n += k < st().assertCov.size() && st().assertCov[k][1] > 0; }
    return {n, m};
}
inline std::pair<int, int> covOf(int type, const InstanceSymbol* inst) {
    switch (type) {
        case vpiStatementCoverage: return stmtCov(inst);
        case vpiToggleCoverage: return togCov(inst);
        case vpiFsmStateCoverage: return fsmCov(inst);
        default: return assertCovOf(inst);
    }
}
inline bool isCovType(int t) {
    return t == vpiAssertCoverage || t == vpiFsmStateCoverage || t == vpiStatementCoverage || t == vpiToggleCoverage;
}
// vpi_get of a coverage property (40.5.2); -1: not one.
inline int64_t covGet(int prop, const Obj* o) {
    auto& s = st();
    bool cov = prop == vpiCovered || prop == vpiCoveredMax || prop == vpiCoveredCount;
    if (isCovType(prop)) {   // covered items of the type in an instance
        if (o->kind == Obj::Assertion) return prop == vpiAssertCoverage && s.assertCov[(size_t)o->idx][1] > 0;
        if (o->kind != Obj::Module) return 0;
        if (prop != vpiAssertCoverage && !g_cov) return 0;
        return covOf(prop, &o->sym->as<InstanceSymbol>()).first;
    }
    if (o->kind == Obj::Assertion) {
        const auto& c = s.assertCov[(size_t)o->idx];
        switch (prop) {
            case vpiCovered: return c[1] > 0;
            case vpiCoveredCount: return (int64_t)c[1];
            case vpiCoveredMax: return 1;
            case vpiAssertAttemptCovered: return (int64_t)c[0];
            case vpiAssertSuccessCovered: return (int64_t)c[1];
            case vpiAssertFailureCovered: return (int64_t)c[2];
            case vpiAssertVacuousSuccessCovered: return (int64_t)c[3];
            case vpiAssertDisableCovered: return (int64_t)c[4];
            case vpiAssertKillCovered: return (int64_t)c[5];
            default: return -1;
        }
    }
    if (!cov) return -1;
    if (o->kind == Obj::Fsm) {
        const FsmDef& d = fsmDefs()[(size_t)o->idx];
        int64_t n = 0, c = 0;
        for (auto& [v, name] : d.states) { uint64_t h = fsmSeenCount(o->idx, v); n += h > 0; c += (int64_t)h; }
        return prop == vpiCovered ? n : prop == vpiCoveredCount ? c : (int64_t)d.states.size();
    }
    if (o->kind == Obj::FsmState) {
        uint64_t h = fsmSeenCount(o->idx, fsmDefs()[(size_t)o->idx].states[o->pos].first);
        return prop == vpiCovered ? h > 0 : prop == vpiCoveredCount ? (int64_t)h : 1;
    }
    // A net or variable, a word or bit of one: its bits that toggled (both ways).
    if (o->kind == Obj::Signal && o->arr) {
        const ArrShape& sh = *arrMap().find(o->sym)->second;
        int64_t n = 0;
        for (int k = 0; k < sh.count; k++) n += toggled(sh.base + k, 0, sh.elemW);
        return prop == vpiCoveredMax ? (int64_t)sh.count * sh.elemW : n;
    }
    if ((o->kind == Obj::Signal || o->kind == Obj::Word || o->kind == Obj::Bit) && !o->real && !o->str) {
        int n = o->kind == Obj::Bit ? toggled(o->sig, o->off, o->w) : toggled(o->sig, 0, o->w);
        return prop == vpiCoveredMax ? o->w : n;
    }
    return -1;
}
// Zero the counts of a type in an instance (null: everywhere).
inline void covReset(int type, const InstanceSymbol* inst, const Obj* a) {
    auto& s = st();
    if (type == vpiAssertCoverage) {
        for (size_t k = 0; k < s.assertCov.size(); k++)
            if (a ? (int)k == a->idx : assertIn(k, inst)) s.assertCov[k] = {};
    } else if (type == vpiStatementCoverage) {
        for (const Prog* p : s.progs)
            if (!inst || p->inst == inst)
                for (auto& i : p->code) i.hits = i.taken = 0;
    } else if (type == vpiToggleCoverage) {
        for (size_t g = 0; g < S.togRise.size(); g++)
            if (!inst || sigInst((int)g) == inst) {
                std::fill(S.togRise[g].begin(), S.togRise[g].end(), 0);
                if (g < S.togFall.size()) std::fill(S.togFall[g].begin(), S.togFall[g].end(), 0);
            }
    } else {
        for (size_t k = 0; k < fsmDefs().size() && k < S.fsmSeen.size(); k++)
            if (!inst || sigInst(fsmDefs()[k].sig) == inst) { S.fsmSeen[k].clear(); S.fsmTrans[k].clear(); }
    }
}
#include "vpi_covmerge.inc"
// vpi_control for coverage (40.5.1): vpiCoverageStart / Stop / Reset /
// Check with a coverage type and an instance or assertion handle (null: the
// whole design); vpiCoverageSave / Merge with a coverage type and a name.
inline PLI_INT32 covControlOp(PLI_INT32 op, va_list ap) {
    if (op < vpiCoverageStart || op > vpiCoverageSave) return -1;
    auto& s = st();
    int type = va_arg(ap, PLI_INT32);
    if (!isCovType(type)) {
        error(vpiError, "vpi_control: " + std::to_string(type) + " is not a coverage type (40.5.1)");
        return 0;
    }
    if (type != vpiAssertCoverage && !g_cov) {
        error(vpiWarning, "vpi_control: statement, toggle and FSM coverage are collected only with msim -cov");
        return 0;
    }
    if (op == vpiCoverageSave || op == vpiCoverageMerge) {
        const char* name = va_arg(ap, const char*);
        if (!name || !*name) { error(vpiError, "vpi_control: coverage save / merge needs a database name"); return 0; }
        if (op == vpiCoverageMerge) return covMerge(type, name) ? 1 : 0;
        if (!covSaveHook() || !covSaveHook()(name)) { error(vpiError, std::string("vpi_control: cannot write coverage to ") + name); return 0; }
        return 1;
    }
    Obj* o = O(va_arg(ap, vpiHandle));
    if (o && o->kind != Obj::Module && o->kind != Obj::Assertion) {
        error(vpiError, "vpi_control: a coverage operation needs an instance or assertion handle (or null)");
        return 0;
    }
    if (o && o->kind == Obj::Assertion && type != vpiAssertCoverage) {
        error(vpiError, "vpi_control: an assertion handle goes with vpiAssertCoverage");
        return 0;
    }
    if (o && denied(o, "vpi_control")) return 0;
    const InstanceSymbol* inst = o && o->kind == Obj::Module ? &o->sym->as<InstanceSymbol>() : nullptr;
    if (op == vpiCoverageCheck) {   // TRUE when every item of the type is covered
        if (o && o->kind == Obj::Assertion) return s.assertCov[(size_t)o->idx][1] > 0;
        auto [n, m] = covOf(type, inst);
        return n == m ? 1 : 0;
    }
    if (op == vpiCoverageReset) { covReset(type, inst, o && o->kind == Obj::Assertion ? o : nullptr); return 1; }
    bool stop = op == vpiCoverageStop;
    if (type == vpiAssertCoverage) {
        if (!o) s.assertCovAllOff = stop;
        for (size_t k = 0; k < s.assertCovOff.size(); k++)
            if (o && (o->kind == Obj::Assertion ? (int)k == o->idx : assertIn(k, inst))) s.assertCovOff[k] = stop;
            else if (!o) s.assertCovOff[k] = 0;
        return 1;
    }
    // Statement, toggle and FSM counting stops for the whole design: a top
    // instance (or null) only.
    if (inst && inst->getParentScope() != &s.comp->getRoot()) {
        error(vpiWarning, "vpi_control: statement, toggle and FSM coverage start and stop only for the whole design (a top instance or null)");
        return 0;
    }
    int bit = type == vpiStatementCoverage ? 1 : type == vpiToggleCoverage ? 2 : 4;
    g_covStop = stop ? (g_covStop | bit) : (g_covStop & ~bit);
    return 1;
}

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
// cbUnresolvedSystf (38.36.3): each system task or function name in the
// source that is neither built in nor registered, once, with the name in
// cb_data->value (vpiStringVal); the callback may vpi_register_systf it.
struct SysNames : slang::syntax::SyntaxVisitor<SysNames> {
    std::vector<std::string> names;
    void visitToken(slang::parsing::Token t) {
        if (t.kind == slang::parsing::TokenKind::SystemIdentifier) names.emplace_back(t.valueText());
    }
};
inline void fireUnresolved(Compilation& comp) {
    auto& s = st();
    if (s.unresolved.empty()) return;
    SysNames v;
    for (const auto& t : comp.getSyntaxTrees()) t->root().visit(v);
    std::set<std::string> done;
    for (const std::string& n : v.names) {
        if (!done.insert(n).second || vpiSystfNames().count(n) || comp.getSystemSubroutine(n)) continue;
        auto due = s.unresolved;
        for (Cb* c : due) {
            if (!c->live) continue;
            s_cb_data d = c->d;
            s_vpi_value val{};
            val.format = vpiStringVal;
            val.value.str = const_cast<PLI_BYTE8*>(n.c_str());
            d.obj = nullptr;
            d.time = nullptr;
            d.value = &val;
            d.cb_rtn(&d);
        }
    }
}
inline void declare(Compilation& comp) {
    fireUnresolved(comp);
    for (auto& [n, i] : vpiSystfNames()) comp.addSystemSubroutine(std::make_shared<UserSystf>(n, i));
}
inline void setup(Compilation& comp, const LowerResult& lr) {
    auto& s = st();
    s.comp = &comp;
    s.lr = &lr;
    covInit();   // also the process list cbStmt uses
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
    assertHook() = onAssert;
    fireList(s.endCompile);
    fireList(s.start);
    fireSys(cbAssertionSysInitialized);   // 39.4.1
    if (!s.rw.empty() || !s.ro.empty()) S.rare = true;
}
inline void end() {
    if (!st().assertEnded) fireSys(cbAssertionSysEnd);
    fireList(st().endSim);
}
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
            if (!o || (o->kind != Obj::Signal && o->kind != Obj::Word && o->kind != Obj::Bit) || o->arr || o->sig < 0) {
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
        case cbForce:
        case cbRelease:
        case cbAssign:
        case cbDeassign:   // 38.36.1: on one object, or on every one (obj NULL)
            if (c->obj && (vpi::isProt(c->obj) || (c->obj->kind != Obj::Signal && c->obj->kind != Obj::Word && c->obj->kind != Obj::Bit) || c->obj->arr)) {
                s.cbs.pop_back();
                vpi::error(vpiError, "vpi_register_cb: a force / assign callback needs a net, variable, word or bit (not protected)");
                return nullptr;
            }
            s.holdCbs.push_back(c);
            S.vpiHoldCb = vpi::onHold;
            break;
        case cbStmt: {   // 38.36.1: each time the statement runs
            Obj* o = c->obj;
            if (!o || (o->kind != Obj::Stmt && o->kind != Obj::ExprStmt) || vpi::isProt(o)) {
                s.cbs.pop_back();
                vpi::error(vpiError, "vpi_register_cb: cbStmt needs a statement handle (not protected)");
                return nullptr;
            }
            if (!vpi::markStmt(o, c)) {
                s.cbs.pop_back();
                vpi::error(vpiError, "vpi_register_cb: the statement has no code msim runs");
                return nullptr;
            }
            break;
        }
        case cbNBASynch:
        case cbAtEndOfSimTime:   // 38.36.2: in this time slot, or `delay` from now
            if (delay == 0) {
                if (d->reason == cbNBASynch) { s.nbaCbs.push_back(c); S.vpiNbaPend = true; }
                else { s.endSlot.push_back(c); S.rare = true; }
            } else {
                c->at = S.now + delay;
                s.timed.insert({c->at, c});
                vpi::at(c->at);
            }
            break;
        case cbTchkViolation:   // 38.36.1: on one timing check, or (obj NULL) any
            if (c->obj && c->obj->kind != Obj::Tchk) {
                s.cbs.pop_back();
                vpi::error(vpiError, "vpi_register_cb: cbTchkViolation needs a timing check handle or NULL");
                return nullptr;
            }
            s.tchkCbs.push_back(c);
            g_tchkHook = vpi::onTchk;
            break;
        case cbCreateObj:   // 38.36.1: a class object is made (obj NULL: any; a class definition: its objects)
            if (c->obj && c->obj->kind != Obj::ClassDefn) {
                s.cbs.pop_back();
                vpi::error(vpiError, "vpi_register_cb: cbCreateObj needs NULL or a class definition handle");
                return nullptr;
            }
            s.createCbs.push_back(c);
            g_newObjHook = vpi::onNewObj;
            break;
        case cbSizeChange:   // 38.36.1: a dynamic array, queue or associative array changes size
            if (!c->obj || c->obj->kind != Obj::Dyn) {
                s.cbs.pop_back();
                vpi::error(vpiError, "vpi_register_cb: cbSizeChange needs a dynamic array, queue or associative array handle");
                return nullptr;
            }
            s.sizeCbs.push_back(c);
            g_sizeHook = vpi::onSizeChange;
            break;
        case cbStartOfSave:
        case cbEndOfSave:
        case cbStartOfRestart:
        case cbEndOfRestart:
        case cbStartOfReset:
        case cbEndOfReset:   // 38.36.2: msim has no save, restart or reset, so these never fire
            break;
        case cbError:
        case cbPLIError:   // 38.36.3
            s.errCbs.push_back(c);
            if (d->reason == cbError) g_errHook = vpi::onRunError;
            break;
        case cbEndOfCompile: s.endCompile.push_back(c); break;
        case cbUnresolvedSystf: s.unresolved.push_back(c); break;   // runs before elaboration (declare)
        case cbStartOfSimulation: s.start.push_back(c); break;
        case cbEndOfSimulation: s.endSim.push_back(c); break;
        default:
            if (vpi::assertSysReason(d->reason)) { s.assertSys.push_back(c); break; }
            if (vpi::assertReason(d->reason)) {
                s.cbs.pop_back();
                vpi::error(vpiError, "vpi_register_cb: a callback on one assertion is placed with vpi_register_assertion_cb (39.4.2)");
                return nullptr;
            }
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
    if (o->cb->assertFn) {
        auto& l = vpi::st().assertCbs[o->cb->assertId];
        l.erase(std::remove(l.begin(), l.end(), o->cb), l.end());
    } else if (vpi::assertSysReason(o->cb->d.reason)) {
        auto& l = vpi::st().assertSys;
        l.erase(std::remove(l.begin(), l.end(), o->cb), l.end());
    }
    if (o->cb->d.reason == cbTchkViolation) {
        auto& l = vpi::st().tchkCbs;
        l.erase(std::remove(l.begin(), l.end(), o->cb), l.end());
    }
    if (o->cb->d.reason == cbError || o->cb->d.reason == cbPLIError) {
        auto& l = vpi::st().errCbs;
        l.erase(std::remove(l.begin(), l.end(), o->cb), l.end());
    }
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

}   // extern "C"
namespace vpi {
using slang::ast::SymbolKind;
// A constant of the design as a value.
inline bool constVal(const slang::ConstantValue& cv, Val& val) {
    if (cv.isReal() || cv.isShortReal()) {
        double d = cv.isReal() ? (double)cv.real() : (double)cv.shortReal();
        uint64_t b;
        std::memcpy(&b, &d, 8);
        val = make(b, 0, 64);
        val.real = true;
        return true;
    }
    if (cv.isString()) { val = mkstr(cv.str()); return true; }
    if (cv.isInteger()) {
        const slang::SVInt& si = cv.integer();
        int w = (int)si.getBitWidth();
        val = make(0, 0, w);
        if (w > 64) val = maskWidth(val, w);
        for (int i = 0; i < w; i++) {
            slang::logic_t b = si[i];
            ::setBit(val, i, b.value == slang::logic_t::Z_VALUE ? 3 : b.isUnknown() ? 2 : (b.value ? 1 : 0));
        }
        return true;
    }
    return false;
}
// The dimensions of an object, outermost first: the unpacked ones of an
// array, then the packed ones of a vector (37.22).
inline std::vector<std::pair<int64_t, int64_t>> dimsOf(const Obj* o) {
    std::vector<std::pair<int64_t, int64_t>> d;
    const Type* t = nullptr;
    if (o->kind == Obj::Typespec) t = &o->ty->getCanonicalType();
    else if (o->kind == Obj::Signal && o->arr) {
        for (auto [l, r] : arrMap().find(o->sym)->second->dims) d.push_back({l, r});
        return d;
    } else if ((o->kind == Obj::Signal || o->kind == Obj::Word || o->kind == Obj::Param) && o->ty) t = &o->ty->getCanonicalType();
    else if (o->kind == Obj::Port) t = &o->sym->as<PortSymbol>().getType().getCanonicalType();
    if (!t) return d;
    if (t->isUnpackedArray()) {
        if (t->hasFixedRange()) { auto r = t->getFixedRange(); d.push_back({r.left, r.right}); }
        return d;
    }
    if (t->isPredefinedInteger() || t->isEnum() || t->kind == SymbolKind::PackedStructType || t->kind == SymbolKind::PackedUnionType) return d;
    while (t->kind == SymbolKind::PackedArrayType) {
        auto r = t->as<PackedArrayType>().range;
        d.push_back({r.left, r.right});
        t = &t->as<PackedArrayType>().elementType.getCanonicalType();
    }
    return d;
}
// The object a port connection expression names (vpiHighConn, 37.14), or null.
inline Obj* exprObj(const Expression* e) {
    using namespace slang::ast;
    while (e) {
        if (e->kind == ExpressionKind::Conversion) e = &e->as<ConversionExpression>().operand();
        else if (e->kind == ExpressionKind::Assignment) e = &e->as<AssignmentExpression>().left();
        else break;
    }
    if (!e) return nullptr;
    EvalContext ctx(ASTContext(st().comp->getRoot(), LookupLocation::max));
    auto index = [&](const Expression& x, int64_t& out) {
        auto cv = x.eval(ctx);
        if (!cv.isInteger()) return false;
        auto v = cv.integer().as<int64_t>();
        if (!v) return false;
        out = *v;
        return true;
    };
    if (e->kind == ExpressionKind::NamedValue || e->kind == ExpressionKind::HierarchicalValue)
        return valueObj(&e->as<ValueExpressionBase>().symbol);
    if (e->kind == ExpressionKind::ElementSelect) {
        // a[i][j][k]: the indices from the name outward.
        std::vector<int64_t> idx;
        const Expression* x = e;
        while (x->kind == ExpressionKind::ElementSelect) {
            int64_t i;
            if (!index(x->as<ElementSelectExpression>().selector(), i)) return nullptr;
            idx.insert(idx.begin(), i);
            x = &x->as<ElementSelectExpression>().value();
        }
        Obj* base = exprObj(x);
        if (!base || (base->kind != Obj::Signal && base->kind != Obj::Word)) return nullptr;
        size_t used = 0;
        if (base->arr) {
            size_t nd = arrMap().find(base->sym)->second->dims.size();
            if (idx.size() < nd) return nullptr;
            base = wordObj(base, std::vector<int64_t>(idx.begin(), idx.begin() + (long)nd));
            used = nd;
        }
        if (!base || used == idx.size()) return base;
        return used + 1 == idx.size() && !base->real && !base->str ? bitObj(base, idx[used]) : nullptr;
    }
    if (e->kind == ExpressionKind::RangeSelect) {
        const auto& rs = e->as<RangeSelectExpression>();
        Obj* base = exprObj(&rs.value());
        int64_t a, b;
        if (!base || base->arr || rs.getSelectionKind() != RangeSelectionKind::Simple || !index(rs.left(), a) || !index(rs.right(), b)) return nullptr;
        return base->kind == Obj::Signal || base->kind == Obj::Word ? partObj(base, a, b) : nullptr;
    }
    return nullptr;
}
// Does a variable or net object answer an iteration for `type` (37.16, 37.17)?
inline bool varMatches(const Obj* v, int type) {
    bool net = v->sym->kind == SymbolKind::Net;
    int et = effType(v);
    switch (type) {
        case vpiVariables:
            if (net) return false;
            return !v->arr || st().mode >= vpiMode1800v2005;   // 1800: array variables are variables too
        case vpiNet: return net && !v->arr;
        case vpiMemory: return v->arr && v->t01 == vpiMemory;
        case vpiRegArray: return v->arr && !net;
        case vpiNetArray: return v->arr && net;
        default: return et == type;
    }
}
inline bool isVarIter(int type) {
    switch (type) {
        case vpiNet: case vpiReg: case vpiVariables: case vpiIntegerVar: case vpiRealVar: case vpiTimeVar:
        case vpiMemory: case vpiRegArray: case vpiNetArray: case vpiLongIntVar: case vpiShortIntVar: case vpiIntVar:
        case vpiShortRealVar: case vpiByteVar: case vpiStringVar: case vpiEnumVar: case vpiStructVar: case vpiUnionVar:
        case vpiBitVar: case vpiPackedArrayVar: case vpiEnumNet: case vpiIntegerNet: case vpiTimeNet: case vpiStructNet:
        case vpiUnionNet: case vpiPackedArrayNet: case vpiBitNet: case vpiIntNet: case vpiShortIntNet: case vpiLongIntNet:
        case vpiByteNet: case vpiRealNet: case vpiShortRealNet:
            return true;
        default: return false;
    }
}
// The members of a scope. An IEEE 1364-1995 / 2001 application sees the
// contents of generate blocks as part of the module; later standards have
// the generate scopes (37.5, 36.12).
template <typename F> void eachMember(const Scope& sc, F&& f) {
    bool flat = flatGenerate();
    for (const Symbol& m : sc.members()) {
        if (flat && m.kind == SymbolKind::GenerateBlock) {
            if (!m.as<GenerateBlockSymbol>().isUninstantiated) eachMember(m.as<GenerateBlockSymbol>(), f);
        } else if (flat && m.kind == SymbolKind::GenerateBlockArray) {
            for (const auto* e : m.as<GenerateBlockArraySymbol>().entries)
                if (!e->isUninstantiated) eachMember(*e, f);
        } else if (m.kind == SymbolKind::StatementBlock && m.name.empty()) {
            eachMember(m.as<StatementBlockSymbol>(), f);   // an unnamed block is not a scope of its own
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
inline const char* typeName(int type) {
    static const std::map<int, const char*> names = {
        {vpiModule, "vpiModule"}, {vpiNet, "vpiNet"}, {vpiReg, "vpiReg"}, {vpiIntegerVar, "vpiIntegerVar"},
        {vpiRealVar, "vpiRealVar"}, {vpiTimeVar, "vpiTimeVar"}, {vpiMemory, "vpiMemory"},
        {vpiMemoryWord, "vpiMemoryWord"}, {vpiNetBit, "vpiNetBit"}, {vpiRegBit, "vpiRegBit"},
        {vpiParameter, "vpiParameter"}, {vpiPort, "vpiPort"}, {vpiConstant, "vpiConstant"},
        {vpiOperation, "vpiOperation"}, {vpiSysTaskCall, "vpiSysTaskCall"}, {vpiSysFuncCall, "vpiSysFuncCall"},
        {vpiUserSystf, "vpiUserSystf"}, {vpiIterator, "vpiIterator"}, {vpiCallback, "vpiCallback"},
        {vpiPartSelect, "vpiPartSelect"}, {vpiRegArray, "vpiRegArray"}, {vpiNetArray, "vpiNetArray"},
        {vpiRange, "vpiRange"}, {vpiTask, "vpiTask"}, {vpiFunction, "vpiFunction"}, {vpiNamedBegin, "vpiNamedBegin"},
        {vpiNamedFork, "vpiNamedFork"}, {vpiGenScope, "vpiGenScope"}, {vpiGenScopeArray, "vpiGenScopeArray"},
        {vpiPackage, "vpiPackage"}, {vpiInterface, "vpiInterface"}, {vpiProgram, "vpiProgram"}, {vpiModport, "vpiModport"},
        {vpiLongIntVar, "vpiLongIntVar"}, {vpiShortIntVar, "vpiShortIntVar"}, {vpiIntVar, "vpiIntVar"},
        {vpiShortRealVar, "vpiShortRealVar"}, {vpiByteVar, "vpiByteVar"}, {vpiStringVar, "vpiStringVar"},
        {vpiEnumVar, "vpiEnumVar"}, {vpiStructVar, "vpiStructVar"}, {vpiUnionVar, "vpiUnionVar"}, {vpiBitVar, "vpiBitVar"},
        {vpiPackedArrayVar, "vpiPackedArrayVar"}, {vpiEnumNet, "vpiEnumNet"}, {vpiIntegerNet, "vpiIntegerNet"},
        {vpiTimeNet, "vpiTimeNet"}, {vpiStructNet, "vpiStructNet"}, {vpiUnionNet, "vpiUnionNet"},
        {vpiPackedArrayNet, "vpiPackedArrayNet"}, {vpiBitNet, "vpiBitNet"}, {vpiIntNet, "vpiIntNet"},
        {vpiShortIntNet, "vpiShortIntNet"}, {vpiLongIntNet, "vpiLongIntNet"}, {vpiByteNet, "vpiByteNet"},
        {vpiRealNet, "vpiRealNet"}, {vpiShortRealNet, "vpiShortRealNet"},
        {vpiLongIntTypespec, "vpiLongIntTypespec"}, {vpiShortRealTypespec, "vpiShortRealTypespec"},
        {vpiByteTypespec, "vpiByteTypespec"}, {vpiShortIntTypespec, "vpiShortIntTypespec"}, {vpiIntTypespec, "vpiIntTypespec"},
        {vpiClassTypespec, "vpiClassTypespec"}, {vpiStringTypespec, "vpiStringTypespec"},
        {vpiChandleTypespec, "vpiChandleTypespec"}, {vpiEnumTypespec, "vpiEnumTypespec"}, {vpiEnumConst, "vpiEnumConst"},
        {vpiIntegerTypespec, "vpiIntegerTypespec"}, {vpiTimeTypespec, "vpiTimeTypespec"}, {vpiRealTypespec, "vpiRealTypespec"},
        {vpiStructTypespec, "vpiStructTypespec"}, {vpiUnionTypespec, "vpiUnionTypespec"}, {vpiBitTypespec, "vpiBitTypespec"},
        {vpiLogicTypespec, "vpiLogicTypespec"}, {vpiArrayTypespec, "vpiArrayTypespec"}, {vpiVoidTypespec, "vpiVoidTypespec"},
        {vpiTypespecMember, "vpiTypespecMember"}, {vpiPackedArrayTypespec, "vpiPackedArrayTypespec"},
        {vpiEventTypespec, "vpiEventTypespec"}, {vpiInterfaceTypespec, "vpiInterfaceTypespec"},
        {vpiAssert, "vpiAssert"}, {vpiAssume, "vpiAssume"}, {vpiCover, "vpiCover"},
        {vpiImmediateAssert, "vpiImmediateAssert"}, {vpiImmediateAssume, "vpiImmediateAssume"},
        {vpiImmediateCover, "vpiImmediateCover"}, {vpiClassObj, "vpiClassObj"}, {vpiClassVar, "vpiClassVar"},
        {vpiClassDefn, "vpiClassDefn"}, {vpiExtends, "vpiExtends"}, {vpiInterModPath, "vpiInterModPath"}, {vpiTchk, "vpiTchk"}, {vpiTchkTerm, "vpiTchkTerm"}, {vpiModPath, "vpiModPath"}, {vpiPathTerm, "vpiPathTerm"},
        {vpiGate, "vpiGate"}, {vpiSwitch, "vpiSwitch"}, {vpiUdp, "vpiUdp"}, {vpiPrimTerm, "vpiPrimTerm"},
        {vpiConstraint, "vpiConstraint"}, {vpiImplication, "vpiImplication"}, {vpiConstrIf, "vpiConstrIf"},
        {vpiConstrIfElse, "vpiConstrIfElse"}, {vpiConstrForEach, "vpiConstrForEach"}, {vpiSoftDisable, "vpiSoftDisable"},
        {vpiConstraintOrdering, "vpiConstraintOrdering"}};
    auto it = names.find(type);
    return it == names.end() ? nullptr : it->second;
}
#include "vpi_code.inc"
#include "vpi_tchk.inc"
#include "vpi_prim.inc"
#include "vpi_constr.inc"
#include "vpi_svcb.inc"
}   // namespace vpi
extern "C" {

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
    using slang::ast::SymbolKind;
    if (vpi::isCode(o)) return vpi::codeHandle(type, o);   // processes, statements, expressions (37.32-37.51)
    if (type == vpiClassObj && o->type == vpiClassVar) return H(vpi::classObjOf(o));   // 37.29
    if (o->kind == Obj::Constraint || o->kind == Obj::ConstrItem) return H(vpi::constrHandle(type, o));   // 37.31
    if (type == vpiClassDefn || o->kind == Obj::ClassDefn || o->kind == Obj::Extends) return H(vpi::classHandle(type, o));
    if (o->kind == Obj::InterModPath) return nullptr;
    if (o->kind == Obj::Tchk || o->kind == Obj::TchkTerm) return H(vpi::tchkHandle(type, o));   // timing checks
    if (o->kind == Obj::ModPath || o->kind == Obj::PathTerm) return H(vpi::pathHandle(type, o));   // module paths
    if (o->kind == Obj::Prim || o->kind == Obj::PrimTerm) return H(vpi::primHandle(type, o));   // primitives
    if (o->kind == Obj::DynElem) {
        if (type == vpiParent) return H(const_cast<Obj*>(o->parent));
        if (type == vpiIndex) return o->idx >= 0 ? H(vpi::constObj(o->idx)) : nullptr;
        return nullptr;
    }
    if (o->kind == Obj::ClassObj || o->kind == Obj::Dyn) {
        if (type == vpiTypespec && o->kind == Obj::Dyn && o->ty) return H(vpi::typespecObj(o->ty));
        if (type != vpiScope && type != vpiModule && type != vpiInstance && type != vpiParent) return nullptr;
        if (o->kind == Obj::ClassObj) return nullptr;
    }
    if (o->kind == Obj::Fsm || o->kind == Obj::FsmState) {   // 40.5.2
        if (type == vpiFsmStateExpression || type == vpiExpr) {
            auto it = s.sigSym.find(fsmDefs()[(size_t)o->idx].sig);
            return it == s.sigSym.end() ? nullptr : H(vpi::valueObj(it->second));
        }
        if (type == vpiParent && o->kind == Obj::FsmState) return H(const_cast<Obj*>(o->parent));
        if (type == vpiModule || type == vpiScope || type == vpiInstance)
            return H(vpi::moduleObj(vpi::sigInst(fsmDefs()[(size_t)o->idx].sig)));
        return nullptr;
    }
    switch (type) {
        case vpiParent:
            if (o->kind == Obj::Word || o->kind == Obj::Bit || o->kind == Obj::Range) return H(const_cast<Obj*>(o->parent));
            if (o->kind == Obj::Member || o->kind == Obj::EnumConst) return H(const_cast<Obj*>(o->parent));
            return nullptr;
        case vpiScope:
        case vpiModule:
        case vpiInstance: {
            while (o->kind == Obj::Word || o->kind == Obj::Bit) o = const_cast<Obj*>(o->parent);
            if (o->kind == Obj::Assertion) {
                const auto* sc = static_cast<const slang::ast::Scope*>(asserts()[(size_t)o->idx].scope);
                if (!sc) return nullptr;
                Obj* in = vpi::scopeObj(sc->asSymbol());
                if (type == vpiScope || !in) return H(in);
                if (in->kind == Obj::Module) return H(in);
                return H(vpi::moduleObj(vpi::instOf(sc)));
            }
            if (o->kind == Obj::Call || o->kind == Obj::Arg) {
                // The call's own scope is the instance it was written in.
                return H(vpi::moduleObj(static_cast<const slang::ast::InstanceSymbol*>(o->site->scope)));
            }
            if (!o->sym) return nullptr;
            if (type == vpiScope) return H(vpi::scopeOfSym(*o->sym));
            // vpiModule: the module instance around it; vpiInstance: the
            // instance or package around it.
            for (const slang::ast::Scope* sc = o->sym->getParentScope(); sc; sc = sc->asSymbol().getParentScope()) {
                const slang::ast::Symbol& p = sc->asSymbol();
                if (p.kind == SymbolKind::InstanceBody) {
                    Obj* m = vpi::moduleObj(p.as<slang::ast::InstanceBodySymbol>().parentInstance);
                    if (type == vpiInstance || (m && m->type == vpiModule)) return H(m);
                }
                if (p.kind == SymbolKind::Package && type == vpiInstance) return H(vpi::scopeObj(p));
            }
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
        case vpiHighConn:
            if (o->kind == Obj::Port) {
                const auto& p = o->sym->as<slang::ast::PortSymbol>();
                const slang::ast::InstanceSymbol* inst = vpi::instOf(p.getParentScope());
                if (!inst) return nullptr;
                const slang::ast::PortConnection* pc = inst->getPortConnection(p);
                return pc ? H(vpi::exprObj(pc->getExpression())) : nullptr;
            }
            return nullptr;
        case vpiLeftRange:
        case vpiRightRange: {
            if (o->kind == Obj::Range) return H(vpi::constObj(type == vpiLeftRange ? o->left : o->right));
            auto d = vpi::dimsOf(o);
            if (d.empty()) return nullptr;
            return H(vpi::constObj(type == vpiLeftRange ? d[0].first : d[0].second));
        }
        case vpiIndex:
            if (o->kind == Obj::Word || (o->kind == Obj::Bit && o->type != vpiPartSelect)) return H(vpi::constObj(o->idx));
            return nullptr;
        case vpiTypespec:
            if (o->kind == Obj::Signal && o->sym) return H(vpi::typespecObj(&o->sym->as<slang::ast::ValueSymbol>().getType()));
            if (o->kind == Obj::Word || o->kind == Obj::Param || o->kind == Obj::Member) return H(vpi::typespecObj(o->ty));
            if (o->kind == Obj::Port) return H(vpi::typespecObj(&o->sym->as<slang::ast::PortSymbol>().getType()));
            if (o->kind == Obj::Scope && o->type == vpiFunction) return H(vpi::typespecObj(&vpi::subrReturnType(*o->sym)));
            return nullptr;
        case vpiBaseTypespec:
            if (o->kind == Obj::Typespec && o->type == vpiEnumTypespec)
                return H(vpi::typespecObj(&o->ty->getCanonicalType().as<slang::ast::EnumType>().baseType));
            return nullptr;
        case vpiElemTypespec:
            if (o->kind == Obj::Typespec) {
                const slang::ast::Type& c = o->ty->getCanonicalType();
                if (c.isUnpackedArray() && c.getArrayElementType()) return H(vpi::typespecObj(c.getArrayElementType()));
                if (c.kind == SymbolKind::PackedArrayType) return H(vpi::typespecObj(&c.as<slang::ast::PackedArrayType>().elementType));
            }
            return nullptr;
        case vpiTypedefAlias:
            // typedef a b: the typespec of b names a (37.25).
            if (o->kind == Obj::Typespec && o->ty->kind == SymbolKind::TypeAlias) {
                const slang::ast::Type& tgt = o->ty->as<slang::ast::TypeAliasType>().targetType.getType();
                if (tgt.kind == SymbolKind::TypeAlias) return H(vpi::typespecObj(&tgt));
            }
            return nullptr;
        default:
            vpi::error(vpiWarning, "vpi_handle: relation " + std::to_string(type) + " is not supported");
            return nullptr;
    }
}

vpiHandle vpi_iterate(PLI_INT32 type, vpiHandle ref) {
    vpi::clearErr();
    auto& s = vpi::st();
    Obj* o = O(ref);
    using slang::ast::SymbolKind;
    std::vector<vpiHandle> items;
    auto add = [&](Obj* x) { if (x) items.push_back(H(x)); };
    if (!o) {
        switch (type) {
            case vpiModule:
            case vpiInterface:
            case vpiProgram:
            case vpiInstance:
                for (const slang::ast::InstanceSymbol* t : s.comp->getRoot().topInstances) {
                    Obj* m = vpi::moduleObj(t);
                    if (type == vpiInstance || m->type == type) add(m);
                }
                if (type != vpiInstance) break;
                [[fallthrough]];
            case vpiPackage:
                for (const slang::ast::PackageSymbol* p : s.comp->getPackages())
                    if (p->name != "std") add(vpi::scopeObj(*p));
                break;
            case vpiUserSystf:
                for (auto& [i, so] : s.sysObj) add(so);
                std::sort(items.begin(), items.end(), [](vpiHandle a, vpiHandle b) { return O(a)->idx < O(b)->idx; });
                break;
            case vpiCallback:
                for (auto& c : s.cbs) if (c->live) add(c->self);
                break;
            case vpiAssertion:   // every assertion of the design (39.3)
                for (size_t k = 0; k < asserts().size(); k++) add(vpi::assertObj((int)k));
                break;
            default:
                vpi::error(vpiError, "vpi_iterate: null reference handle");
                return nullptr;
        }
        return vpi::iter(std::move(items));
    }
    // A protected call still gives its arguments (38.36); nothing else of a
    // protected object can be iterated.
    if (!(type == vpiArgument && o->kind == Obj::Call) && vpi::denied(o, "vpi_iterate")) return nullptr;
    if (vpi::isCode(o)) return vpi::codeIterate(type, o, items) ? vpi::iter(std::move(items)) : nullptr;
    if (vpi::netConn(type, o, items)) return vpi::iter(std::move(items));
    if (vpi::tchkIterate(type, o, items)) return vpi::iter(std::move(items));   // timing checks
    if (o->kind == Obj::Tchk || o->kind == Obj::TchkTerm) return nullptr;
    if (vpi::pathIterate(type, o, items)) return vpi::iter(std::move(items));   // module paths
    if (o->kind == Obj::ModPath || o->kind == Obj::PathTerm) return nullptr;
    if (vpi::primIterate(type, o, items)) return vpi::iter(std::move(items));   // primitives
    if (o->kind == Obj::Prim || o->kind == Obj::PrimTerm) return nullptr;   // drivers, loads (37.16, 37.17)
    if (vpi::constrIterate(type, o, items)) return vpi::iter(std::move(items));   // constraints (37.31)
    if (o->kind == Obj::Constraint || o->kind == Obj::ConstrItem) return nullptr;
    if (type == vpiMember) {   // the members of a struct or union variable (37.17)
        if (!vpi::isStructObj(o)) return nullptr;
        vpi::members(o, items);
        return vpi::iter(std::move(items));
    }
    if (o->kind == Obj::Dyn) {   // the elements a container has now (37.18)
        if (type != vpiReg && type != vpiVariables && type != vpiElement && type != vpiVarSelect && type != o->t01) return nullptr;
        vpi::dynElems(o, items);
        return vpi::iter(std::move(items));
    }
    if (o->kind == Obj::ClassDefn) return vpi::classIterate(type, o, items) ? vpi::iter(std::move(items)) : nullptr;
    if (o->kind == Obj::InterModPath) return type == vpiPort ? vpi::iter(std::vector<vpiHandle>(o->items)) : nullptr;   // 38.21
    if (o->kind == Obj::Extends) return nullptr;
    if (o->kind == Obj::ClassObj) {   // the properties of a class object (37.29)
        if (type != vpiVariables) return nullptr;
        vpi::classProps(o, *o->ty, items);
        return vpi::iter(std::move(items));
    }
    if (type == vpiArgument) {
        if (o->kind != Obj::Call) return nullptr;
        for (size_t k = 0; k < o->site->args.size(); k++) add(vpi::argObj(o->site, (int)k));
        return vpi::iter(std::move(items));
    }
    if (type == vpiFsm) {   // the FSMs of an instance (40.5.2)
        if (o->kind != Obj::Module) return nullptr;
        for (size_t k = 0; k < fsmDefs().size(); k++)
            if (vpi::sigInst(fsmDefs()[k].sig) == &o->sym->as<slang::ast::InstanceSymbol>()) add(vpi::fsmObj((int)k));
        return vpi::iter(std::move(items));
    }
    if (type == vpiFsmStates) {
        if (o->kind != Obj::Fsm) return nullptr;
        for (size_t n = 0; n < fsmDefs()[(size_t)o->idx].states.size(); n++) add(vpi::fsmStateObj(o->idx, n));
        return vpi::iter(std::move(items));
    }
    if (type == vpiBit) {
        if ((o->kind != Obj::Signal && o->kind != Obj::Word) || o->arr || o->real || o->str) return nullptr;
        auto [l, r] = vpi::rangeOf(o);
        for (int64_t i = l;; i += l >= r ? -1 : 1) {
            add(vpi::bitObj(o, i));
            if (i == r) break;
        }
        return vpi::iter(std::move(items));
    }
    if (o->kind == Obj::Signal && o->arr) {
        // The elements of an array, first to last as declared (37.18, 37.20).
        if (type == vpiMemoryWord || type == vpiReg || type == vpiElement || type == vpiNet || vpi::isVarIter(type)) {
            if (type == vpiMemoryWord && !o->t01) return nullptr;
            int64_t n = arrMap().find(o->sym)->second->count;
            for (int64_t k = 0; k < n; k++) {
                Obj* w = vpi::wordAt(o, k);
                if (type == vpiMemoryWord || type == vpiElement || vpi::effType(w) == type ||
                    (type == vpiReg && o->t01) || (type == vpiNet && o->type == vpiNetArray) || (type == vpiVariables && o->type == vpiRegArray))
                    add(w);
            }
            return vpi::iter(std::move(items));
        }
    }
    if (type == vpiAssertion) {   // the assertions in an instance or scope (39.3)
        if (o->kind != Obj::Module && o->kind != Obj::Scope) return nullptr;
        for (size_t k = 0; k < asserts().size(); k++)
            if (!asserts()[k].hidden && vpi::assertIn(asserts()[k], o)) add(vpi::assertObj((int)k));
        return vpi::iter(std::move(items));
    }
    if (type == vpiRange) {
        auto d = vpi::dimsOf(o);
        for (size_t k = 0; k < d.size(); k++) add(vpi::rangeObj(o, (int)k, d[k].first, d[k].second));
        return vpi::iter(std::move(items));
    }
    if (type == vpiIndex) {
        if (o->kind != Obj::Word) return nullptr;
        for (int64_t i : o->index) add(vpi::constObj(i));
        return vpi::iter(std::move(items));
    }
    if (o->kind == Obj::Typespec) {
        const slang::ast::Type& c = o->ty->getCanonicalType();
        if (type == vpiEnumConst && c.isEnum()) {
            for (const auto& ev : c.as<slang::ast::EnumType>().values()) {
                auto& e = s.symObj[&ev];
                if (!e) {
                    e = vpi::mkObj(Obj::EnumConst, vpiEnumConst);
                    e->sym = &ev;
                    e->parent = o;
                    e->w = (int)c.getBitWidth();
                    e->sgn = c.isSigned();
                    vpi::constVal(ev.getValue(), e->cval);
                }
                add(e);
            }
            return vpi::iter(std::move(items));
        }
        if (type == vpiTypespecMember) {
            const slang::ast::Scope* sc = c.isStruct() || c.isPackedUnion() || c.isUnpackedUnion() ? c.as_if<slang::ast::Scope>() : nullptr;
            if (!sc) return nullptr;
            for (const auto& f : sc->membersOfType<slang::ast::FieldSymbol>()) {
                auto& e = s.symObj[&f];
                if (!e) {
                    e = vpi::mkObj(Obj::Member, vpiTypespecMember);
                    e->sym = &f;
                    e->parent = o;
                    e->ty = &f.getType();
                    e->w = f.getType().isIntegral() || f.getType().isFloating() ? (int)f.getType().getBitWidth() : 0;
                    e->sgn = f.getType().isIntegral() && f.getType().isSigned();
                }
                add(e);
            }
            return vpi::iter(std::move(items));
        }
        return nullptr;
    }
    if (o->kind == Obj::Scope && o->type == vpiGenScopeArray) {
        if (type != vpiGenScope) return nullptr;
        for (const auto* e : o->sym->as<slang::ast::GenerateBlockArraySymbol>().entries) add(vpi::scopeObj(*e));
        return vpi::iter(std::move(items));
    }
    const slang::ast::Scope* sc = vpi::scopeOf(o);
    if (!sc) return nullptr;
    if (vpi::scopeCode(type, *sc, items)) return vpi::iter(std::move(items));   // vpiProcess, vpiContAssign
    if (type == vpiPort) {
        if (o->kind != Obj::Module) return nullptr;
        for (const slang::ast::Symbol* p : o->sym->as<slang::ast::InstanceSymbol>().body.getPortList())
            if (p->kind == SymbolKind::Port) add(vpi::portObj(p, (int)items.size()));
        return vpi::iter(std::move(items));
    }
    bool known = vpi::isVarIter(type);
    switch (type) {
        case vpiModule: case vpiInterface: case vpiProgram: case vpiInstance: case vpiInternalScope: case vpiGenScope:
        case vpiGenScopeArray: case vpiTask: case vpiFunction: case vpiTaskFunc: case vpiNamedBegin: case vpiNamedFork:
        case vpiParameter: case vpiTypedef: case vpiModport:
            known = true;
            break;
        default: break;
    }
    if (!known) {
        vpi::error(vpiWarning, "vpi_iterate: type " + std::to_string(type) + " is not supported");
        return nullptr;
    }
    vpi::eachMember(*sc, [&](const slang::ast::Symbol& m) {
        switch (m.kind) {
            case SymbolKind::Instance: {
                Obj* x = vpi::moduleObj(&m.as<slang::ast::InstanceSymbol>());
                if (x->type == type || type == vpiInstance) add(x);
                break;
            }
            case SymbolKind::GenerateBlock:
            case SymbolKind::GenerateBlockArray:
            case SymbolKind::Subroutine:
            case SymbolKind::StatementBlock: {
                Obj* x = vpi::scopeObj(m);
                if (!x) break;
                if (m.kind == SymbolKind::Subroutine && m.as<slang::ast::SubroutineSymbol>().flags.has(slang::ast::MethodFlags::BuiltIn)) break;
                // 37.3: the internal scopes are the tasks, functions, named
                // blocks and generate scopes; a generate scope array holds
                // its scopes itself.
                if (x->type == type || (type == vpiInternalScope && x->type != vpiGenScopeArray) ||
                    (type == vpiTaskFunc && (x->type == vpiTask || x->type == vpiFunction)))
                    add(x);
                if (type == vpiInternalScope && x->type == vpiGenScopeArray)
                    for (const auto* e : m.as<slang::ast::GenerateBlockArraySymbol>().entries) add(vpi::scopeObj(*e));
                break;
            }
            case SymbolKind::Net:
            case SymbolKind::Variable: {
                if (!vpi::isVarIter(type)) break;
                Obj* v = vpi::valueObj(&m);
                if (v && vpi::varMatches(v, type)) add(v);
                break;
            }
            case SymbolKind::Parameter:
                if (type == vpiParameter) add(vpi::paramObj(&m));
                break;
            case SymbolKind::TypeAlias:
                if (type == vpiTypedef) add(vpi::typespecObj(&m.as<slang::ast::TypeAliasType>()));
                break;
            case SymbolKind::Modport:
                if (type == vpiModport) {
                    auto& x = s.symObj[&m];
                    if (!x) {
                        x = vpi::mkObj(Obj::Modport, vpiModport);
                        x->sym = &m;
                    }
                    add(x);
                }
                break;
            default: break;
        }
    });
    return vpi::iter(std::move(items));
}

vpiHandle vpi_scan(vpiHandle it) {
    vpi::clearErr();
    Obj* o = O(it);
    if (!o || o->kind != Obj::Iter) { vpi::error(vpiError, "vpi_scan: not an iterator"); return nullptr; }
    if (o->pos < o->items.size()) return o->items[o->pos++];
    // The iterator is freed when the scan reaches its end (38.40).
    o->items.clear();
    o->kind = Obj::Event;   // a dead handle
    return nullptr;
}

// 38.21: an object by its name, a full name or one relative to `scope`.
// Each step may carry indices: a generate loop or instance array element,
// and on the last step array elements, a bit or a part select.
vpiHandle vpi_handle_by_name(PLI_BYTE8* name, vpiHandle scope) {
    vpi::clearErr();
    auto& s = vpi::st();
    using slang::ast::SymbolKind;
    if (!name) return nullptr;
    std::string n = name;
    std::vector<std::string> parts;
    {
        std::string cur;
        int depth = 0;
        for (size_t k = 0; k < n.size(); k++) {
            char c = n[k];
            if (c == '[') depth++;
            if (c == ']') depth--;
            bool sep = depth == 0 && (c == '.' || (c == ':' && k + 1 < n.size() && n[k + 1] == ':'));
            if (sep) {
                parts.push_back(cur);
                cur.clear();
                if (c == ':') k++;
            } else cur += c;
        }
        parts.push_back(cur);
    }
    // The indices written after a name: [3], [3][1], [7:4].
    struct Sel { int64_t a = 0, b = 0; bool range = false; };
    auto selsOf = [](const std::string& part) {
        std::vector<Sel> v;
        for (size_t p = part.find('['); p != std::string::npos; p = part.find('[', p + 1)) {
            Sel sl;
            char* end = nullptr;
            sl.a = std::strtoll(part.c_str() + p + 1, &end, 10);
            if (end && *end == ':') { sl.range = true; sl.b = std::strtoll(end + 1, nullptr, 10); }
            v.push_back(sl);
        }
        return v;
    };
    auto top = [&](const std::string& base) -> const slang::ast::Symbol* {
        for (const slang::ast::InstanceSymbol* t : s.comp->getRoot().topInstances)
            if (t->name == base) return t;
        for (const slang::ast::PackageSymbol* p : s.comp->getPackages())
            if (p->name == base) return p;
        return nullptr;
    };
    if (vpi::denied(O(scope), "vpi_handle_by_name")) return nullptr;   // a protected scope (38.20)
    // The name may not find a protected object or instance, even by its
    // disclosed name (38.20; IEEE 1735 9.7.1). What the IP author made
    // visible with a viewport or a right is found by its full name.
    auto shut = [&](const slang::ast::Symbol* y) {
        bool p = false;
        if (y->kind == SymbolKind::Instance) p = protectedInstance(&y->as<slang::ast::InstanceSymbol>());
        else if (y->kind == SymbolKind::GenerateBlock || y->kind == SymbolKind::GenerateBlockArray ||
                 y->kind == SymbolKind::Subroutine || y->kind == SymbolKind::StatementBlock)
            p = protectedInstance(vpi::instOf(y->getParentScope())) || (hideMsgIf() && hideMsgIf()(y->location));
        else p = vpi::protSym(y);
        if (p) vpi::error(vpiError, "vpi_handle_by_name: the name is in protected code");
        return p;
    };
    auto resolve = [&](const slang::ast::Scope* sc) -> vpiHandle {
        const slang::ast::Symbol* sym = nullptr;
        for (size_t k = 0; k < parts.size(); k++) {
            std::string base = parts[k].substr(0, parts[k].find('['));
            auto sels = selsOf(parts[k]);
            bool last = k + 1 == parts.size();
            if (k > 0 && !sc) return nullptr;
            sym = sc ? sc->find(base) : top(base);
            if (!sym && sc) {   // a name declared in an unnamed block (not a scope of its own)
                std::function<const slang::ast::Symbol*(const slang::ast::Scope&)> look = [&](const slang::ast::Scope& in) -> const slang::ast::Symbol* {
                    for (const auto& m : in.members())
                        if (m.kind == SymbolKind::StatementBlock && m.name.empty()) {
                            const auto& b = m.as<slang::ast::StatementBlockSymbol>();
                            if (const auto* f = b.find(base)) return f;
                            if (const auto* f = look(b)) return f;
                        }
                    return nullptr;
                };
                sym = look(*sc);
            }
            if (!sym) return nullptr;
            size_t used = 0;
            while (used < sels.size() && !sels[used].range) {
                const slang::ast::Symbol* nx = nullptr;
                int64_t idx = sels[used].a;
                if (sym->kind == SymbolKind::GenerateBlockArray) {
                    for (const auto* e : sym->as<slang::ast::GenerateBlockArraySymbol>().entries)
                        if (e->getArrayIndex() && e->getArrayIndex()->as<int64_t>() == idx) nx = e;
                } else if (sym->kind == SymbolKind::InstanceArray) {
                    const auto& a = sym->as<slang::ast::InstanceArraySymbol>();
                    int64_t off = a.range.left <= a.range.right ? idx - a.range.left : a.range.left - idx;
                    if (off >= 0 && off < (int64_t)a.elements.size()) nx = a.elements[(size_t)off];
                } else break;   // indices of a variable or net
                if (!nx) return nullptr;
                sym = nx;
                used++;
            }
            if (last && shut(sym)) return nullptr;
            if (!last) {
                if (used != sels.size()) return nullptr;
                if (sym->kind == SymbolKind::Instance) sc = &sym->as<slang::ast::InstanceSymbol>().body;
                else if (sym->kind == SymbolKind::GenerateBlock && sym->as<slang::ast::GenerateBlockSymbol>().isUninstantiated) return nullptr;
                else sc = sym->isScope() ? &sym->as<slang::ast::Scope>() : nullptr;
                continue;
            }
            if (sym->kind == SymbolKind::Parameter) return used == sels.size() ? H(vpi::paramObj(sym)) : nullptr;
            if (sym->kind == SymbolKind::Net || sym->kind == SymbolKind::Variable) {
                Obj* v = vpi::valueObj(sym);
                if (!v) return nullptr;
                if (v->arr && used < sels.size()) {
                    size_t nd = arrMap().find(sym)->second->dims.size();
                    std::vector<int64_t> idx;
                    while (idx.size() < nd && used < sels.size() && !sels[used].range) idx.push_back(sels[used++].a);
                    if (idx.size() != nd) return nullptr;
                    v = vpi::wordObj(v, idx);
                    if (!v) return nullptr;
                }
                if (used == sels.size()) return H(v);
                if (used + 1 != sels.size() || v->real || v->str) return nullptr;
                return H(sels[used].range ? vpi::partObj(v, sels[used].a, sels[used].b) : vpi::bitObj(v, sels[used].a));
            }
            if (used != sels.size()) return nullptr;
            if (sym->kind == SymbolKind::Modport) {
                auto& x = s.symObj[sym];
                if (!x) {
                    x = vpi::mkObj(Obj::Modport, vpiModport);
                    x->sym = sym;
                }
                return H(x);
            }
            if (sym->kind == SymbolKind::TypeAlias) return H(vpi::typespecObj(&sym->as<slang::ast::TypeAliasType>()));
            return H(vpi::scopeObj(*sym));
        }
        return nullptr;
    };
    // An assertion, by its label (39.3). Its label also names a scope (16.3):
    // the name gives the assertion.
    std::string want = name ? name : "";
    std::string pre;
    if (scope) {
        const char* sn = vpi_get_str(vpiFullName, scope);
        pre = sn ? std::string(sn) + "." : "";
    }
    for (size_t k = 0; k < asserts().size(); k++) {
        const AssertInfo& a = asserts()[k];
        if (!a.hidden && (a.name == want || (!pre.empty() && a.name == pre + want))) return H(vpi::assertObj((int)k));
    }
    if (const slang::ast::Scope* sc = vpi::scopeOf(O(scope)))
        if (vpiHandle h = resolve(sc)) return h;
    return resolve(nullptr);   // a full name, also when a scope is given
}

vpiHandle vpi_handle_by_index(vpiHandle h, PLI_INT32 i) {
    vpi::clearErr();
    Obj* o = O(h);
    if (o && o->kind == Obj::Dyn) {   // an element of a dynamic array or queue, or the entry of an associative array
        if (vpi::denied(o, "vpi_handle_by_index")) return nullptr;
        const DynObj* d = vpi::dynOf(o);
        if (!d) return nullptr;
        Val k = make((uint64_t)(int64_t)i, 0, 64);
        if (d->kind == 2) {
            k = make((uint64_t)(int64_t)i, 0, d->keyW);
            k.sgn = d->keySgn;
            if (d->strKey || !d->map.count(d->key(k))) return nullptr;
        } else if (i < 0 || (size_t)i >= d->vec.size()) return nullptr;
        return H(vpi::dynElemObj(o, k));
    }
    if (!o || (o->kind != Obj::Signal && o->kind != Obj::Word)) return nullptr;
    if (vpi::denied(o, "vpi_handle_by_index")) return nullptr;
    if (o->arr) return H(vpi::wordObj(o, i));
    if (o->real || o->str) return nullptr;
    return H(vpi::bitObj(o, i));
}

// 38.20: an element of an array by all its indices; one more index selects
// a bit of the element.
vpiHandle vpi_handle_by_multi_index(vpiHandle h, PLI_INT32 n, PLI_INT32* idx) {
    vpi::clearErr();
    Obj* o = O(h);
    if (!o || !idx || n < 1) { vpi::error(vpiError, "vpi_handle_by_multi_index: no object or indices"); return nullptr; }
    if (vpi::denied(o, "vpi_handle_by_multi_index")) return nullptr;
    if (o->kind != Obj::Signal || !o->arr) return n == 1 ? vpi_handle_by_index(h, idx[0]) : nullptr;
    size_t nd = arrMap().find(o->sym)->second->dims.size();
    if ((size_t)n != nd && (size_t)n != nd + 1) {
        vpi::error(vpiError, "vpi_handle_by_multi_index: the array has " + std::to_string(nd) + " dimensions");
        return nullptr;
    }
    Obj* w = vpi::wordObj(o, std::vector<int64_t>(idx, idx + nd));
    if (!w || (size_t)n == nd) return H(w);
    return w->real ? nullptr : H(vpi::bitObj(w, idx[nd]));
}

PLI_INT32 vpi_get(PLI_INT32 prop, vpiHandle h) {
    vpi::clearErr();
    auto& s = vpi::st();
    Obj* o = O(h);
    using slang::ast::SymbolKind;
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
                (o->kind == Obj::Signal && !o->arr);
    if (prop != vpiType && !(prop == vpiSize && expr) && vpi::denied(o, "vpi_get"))
        return vpiUndefined;
    auto isVar = [&] { return o->kind == Obj::Signal || o->kind == Obj::Word; };
    if (vpi::isCode(o)) return (PLI_INT32)vpi::codeGet(prop, o);
    if (o->kind == Obj::Dyn) {   // 37.18
        const DynObj* d = vpi::dynOf(o);
        switch (prop) {
            case vpiType: return o->type;
            case vpiArrayType: return (PLI_INT32)o->left;
            case vpiSize: return d ? (PLI_INT32)d->size() : 0;
            case vpiLineNo: return o->sym && srcMgr() ? (PLI_INT32)srcMgr()->getLineNumber(o->sym->location) : 0;
            default: return vpiUndefined;
        }
    }
    if (o->kind == Obj::ClassDefn || o->kind == Obj::Extends) return (PLI_INT32)vpi::classGet(prop, o);
    if (o->kind == Obj::InterModPath) return prop == vpiType ? vpiInterModPath : vpiUndefined;
    if (o->kind == Obj::Tchk || o->kind == Obj::TchkTerm) return (PLI_INT32)vpi::tchkGet(prop, o);
    if (o->kind == Obj::ModPath || o->kind == Obj::PathTerm) return (PLI_INT32)vpi::pathGet(prop, o);
    if (o->kind == Obj::Prim || o->kind == Obj::PrimTerm) return (PLI_INT32)vpi::primGet(prop, o);
    if (o->kind == Obj::Constraint || o->kind == Obj::ConstrItem) return (PLI_INT32)vpi::constrGet(prop, o);
    if (prop == vpiVirtual && vpi::isMethod(o))
        return o->sym->kind == SymbolKind::MethodPrototype || o->sym->as<slang::ast::SubroutineSymbol>().isVirtual();
    if (o->kind == Obj::ClassObj) {   // 37.29
        if (prop == vpiType) return vpiClassObj;
        if (prop == vpiObjId) return o->idx;
        return vpiUndefined;
    }
    if (o->kind == Obj::DynElem) {
        if (prop == vpiType) return o->type;
        if (prop == vpiSize) return o->w;
        if (prop == vpiSigned) return o->sgn;
        return vpiUndefined;
    }
    if (o->kind == Obj::SchedEvent) return prop == vpiType ? vpiSchedEvent : prop == vpiScheduled ? o->sched : vpiUndefined;
    if (int64_t c = vpi::covGet(prop, o); c >= 0) return (PLI_INT32)std::min<int64_t>(c, INT32_MAX);   // 40.5.2
    if (o->kind == Obj::Fsm || o->kind == Obj::FsmState) {
        switch (prop) {
            case vpiType: return o->type;
            case vpiSize: return o->w;
            case vpiLineNo: return (PLI_INT32)fsmDefs()[(size_t)o->idx].line;
            default: return vpiUndefined;
        }
    }
    if (o->kind == Obj::Assertion) {   // 39.3
        const AssertInfo& a = asserts()[(size_t)o->idx];
        switch (prop) {
            case vpiType: return o->type;
            case vpiLineNo: return a.line;
            case vpiIsDeferred: return a.type & (AT_OBSERVED | AT_FINAL) ? 1 : 0;
            case vpiIsFinal: return a.type & AT_FINAL ? 1 : 0;
            case vpiIsCoverSequence: return 0;
            case vpiIsClockInferred: return 0;
            default:
                vpi::error(vpiWarning, "vpi_get: property " + std::to_string(prop) + " of an assertion is not supported");
                return vpiUndefined;
        }
    }
    switch (prop) {
        case vpiType: return vpi::effType(o);
        case vpiSize:
            if (o->kind == Obj::Signal && o->arr) return arrMap().find(o->sym)->second->count;
            if (o->kind == Obj::Call) return o->site->func ? o->w : 0;
            if (o->kind == Obj::Port) return (int)o->sym->as<slang::ast::PortSymbol>().getType().getBitWidth();
            if (o->kind == Obj::Arg && o->str) return std::max(8, (int)v_str(vpi::readObj(o)).size() * 8);
            if (o->kind == Obj::Signal && o->str) return (int)v_str(vpi::readObj(o)).size() * 8;
            if (o->kind == Obj::Typespec && o->ty->getCanonicalType().isUnpackedArray()) {
                const auto& c = o->ty->getCanonicalType();
                return c.hasFixedRange() ? (int)c.getFixedRange().width() : 0;
            }
            if (o->kind == Obj::Scope && o->type == vpiGenScopeArray) return (int)o->sym->as<slang::ast::GenerateBlockArraySymbol>().entries.size();
            return o->w;
        case vpiSigned: return o->sgn ? 1 : 0;
        case vpiScalar: return o->w == 1 && !o->real && !o->str && !o->arr && vpi::dimsOf(o).empty() ? 1 : 0;
        case vpiVector: return !o->real && !o->str && !o->arr && (o->w > 1 || !vpi::dimsOf(o).empty()) ? 1 : 0;
        case vpiTopModule: return o->kind == Obj::Module && o->sym->as<slang::ast::InstanceSymbol>().isTopLevel();
        case vpiTop:
            if (o->kind == Obj::Module) return o->sym->as<slang::ast::InstanceSymbol>().isTopLevel();
            return o->kind == Obj::Scope && o->type == vpiPackage ? 1 : 0;
        case vpiLineNo:
        case vpiDefLineNo:
            if (o->kind == Obj::Call) return o->site->line;
            if (prop == vpiDefLineNo && o->kind == Obj::Module && srcMgr())
                return (int)srcMgr()->getLineNumber(o->sym->as<slang::ast::InstanceSymbol>().getDefinition().location);
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
                    case slang::ast::ArgumentDirection::Ref: return vpiRef;
                    default: return vpiNoDirection;
                }
            }
            return vpiUndefined;
        case vpiPortIndex: return o->kind == Obj::Port ? o->idx : vpiUndefined;
        case vpiNetType: {
            const slang::ast::Symbol* sy = o->kind == Obj::Bit ? o->parent->sym : o->sym;
            if (!sy || sy->kind != SymbolKind::Net) return vpiUndefined;
            switch (sy->as<slang::ast::NetSymbol>().netType.netKind) {
                case slang::ast::NetType::WAnd: return vpiWand;
                case slang::ast::NetType::WOr: return vpiWor;
                case slang::ast::NetType::Tri: return vpiTri;
                case slang::ast::NetType::Tri0: return vpiTri0;
                case slang::ast::NetType::Tri1: return vpiTri1;
                case slang::ast::NetType::TriAnd: return vpiTriAnd;
                case slang::ast::NetType::TriOr: return vpiTriOr;
                case slang::ast::NetType::TriReg: return vpiTriReg;
                case slang::ast::NetType::Supply0: return vpiSupply0;
                case slang::ast::NetType::Supply1: return vpiSupply1;
                case slang::ast::NetType::UWire: return vpiUwire;
                case slang::ast::NetType::Interconnect: return vpiInterconnect;
                case slang::ast::NetType::UserDefined: return vpiNettypeNet;
                default: return vpiWire;
            }
        }
        case vpiFuncType:
            if (o->kind == Obj::Call) return o->site->func ? vpiSystfNames()[s.systfs[(size_t)o->site->systf].name].ftype : 0;
            if (o->kind == Obj::Systf) return s.systfs[(size_t)o->idx].d.sysfunctype;
            if (o->kind == Obj::Scope && o->type == vpiFunction) {
                const slang::ast::Type& rt = vpi::subrReturnType(*o->sym).getCanonicalType();
                if (rt.isFloating()) return vpiRealFunc;
                if (rt.isPredefinedInteger()) {
                    auto k = rt.as<slang::ast::PredefinedIntegerType>().integerKind;
                    if (k == slang::ast::PredefinedIntegerType::Integer || k == slang::ast::PredefinedIntegerType::Int) return vpiIntFunc;
                    if (k == slang::ast::PredefinedIntegerType::Time) return vpiTimeFunc;
                }
                if (rt.isIntegral()) return rt.isSigned() ? vpiSizedSignedFunc : vpiSizedFunc;
                return vpiOtherFunc;
            }
            return vpiUndefined;
        case vpiConstType:
            if (o->kind == Obj::Arg && o->type == vpiConstant) {
                const VpiArg& a = o->site->args[(size_t)o->idx];
                if (a.kind == VpiArg::String) return vpiStringConst;
                if (a.real) return vpiRealConst;
                return vpiBinaryConst;
            }
            if (o->kind == Obj::Const) return vpiIntConst;
            if (o->kind == Obj::Param) return o->str ? vpiStringConst : o->real ? vpiRealConst : vpiBinaryConst;
            return vpiUndefined;
        case vpiLocalParam:
            return o->kind == Obj::Param && o->sym->as<slang::ast::ParameterSymbol>().isLocalParam() ? 1 : 0;
        case vpiArray: return o->arr ? 1 : 0;
        case vpiArrayType: return o->arr || (o->kind == Obj::Typespec && o->type == vpiArrayTypespec) ? vpiStaticArray : vpiUndefined;
        case vpiIsMemory: return o->arr && o->t01 == vpiMemory ? 1 : 0;
        case vpiArrayMember: return o->kind == Obj::Word ? 1 : 0;
        case vpiConstantSelect: return o->kind == Obj::Word || o->kind == Obj::Bit ? 1 : 0;
        case vpiPacked:
            if (o->kind == Obj::Typespec) {
                auto k = o->ty->getCanonicalType().kind;
                return k == SymbolKind::PackedStructType || k == SymbolKind::PackedUnionType || k == SymbolKind::PackedArrayType;
            }
            if (isVar() && o->ty && !o->arr) {
                auto k = o->ty->getCanonicalType().kind;
                return k == SymbolKind::PackedStructType || k == SymbolKind::PackedUnionType;
            }
            return 0;
        case vpiTagged:
            if (o->kind == Obj::Typespec || isVar()) {
                const slang::ast::Type& c = o->ty->getCanonicalType();
                return c.isTaggedUnion() ? 1 : 0;
            }
            return 0;
        case vpiAutomatic:
            if (o->kind == Obj::Scope && (o->type == vpiTask || o->type == vpiFunction))
                return o->sym->kind != SymbolKind::Subroutine ||
                       o->sym->as<slang::ast::SubroutineSymbol>().defaultLifetime == slang::ast::VariableLifetime::Automatic;
            if (o->kind == Obj::Signal && o->sym->kind == SymbolKind::Variable)
                return o->sym->as<slang::ast::VariableSymbol>().lifetime == slang::ast::VariableLifetime::Automatic;
            return 0;
        case vpiConstantVariable:
            return o->kind == Obj::Signal && o->sym->kind == SymbolKind::Variable &&
                   o->sym->as<slang::ast::VariableSymbol>().flags.has(slang::ast::VariableFlags::Const);
        case vpiRandType: return isVar() ? vpiNotRand : vpiUndefined;
        case vpiIsRandomized: return 0;
        case vpiVisibility: return vpiPublicVis;
        case vpiStructUnionMember: return 0;
        case vpiCellInstance: return 0;
        case vpiProtected: return 0;
        case vpiExpanded: return o->sym && o->sym->kind == SymbolKind::Net ? 1 : 0;
        case vpiExplicitScalared:
        case vpiExplicitVectored:
        case vpiImplicitDecl: return 0;
        case vpiDefNetType: return o->kind == Obj::Module ? vpiWire : vpiUndefined;
        case vpiUnconnDrive: return o->kind == Obj::Module ? vpiHighZ : vpiUndefined;
        case vpiDefDelayMode: return o->kind == Obj::Module ? vpiDelayModeNone : vpiUndefined;
        case vpiDefDecayTime: return 0;
        case vpiIndex: return o->idx;
        case vpiValid: return 1;
        case vpiAllocScheme: return vpiOtherScheme;
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
    using slang::ast::SymbolKind;
    if (!o) return nullptr;
    if (prop != vpiType && vpi::denied(o, "vpi_get_str")) return nullptr;
    if (vpi::isCode(o) && prop != vpiType) {
        std::string r;
        bool have = false;
        vpi::codeGetStr(prop, o, r, have);
        return have ? vpi::retStr(r) : nullptr;
    }
    if (o->kind == Obj::InterModPath && prop != vpiType) return nullptr;
    if ((o->kind == Obj::Tchk || o->kind == Obj::TchkTerm) && prop != vpiType) {
        std::string r;
        return vpi::tchkGetStr(prop, o, r) ? vpi::retStr(r) : nullptr;
    }
    if ((o->kind == Obj::Constraint || o->kind == Obj::ConstrItem) && prop != vpiType) {
        std::string r;
        return vpi::constrGetStr(prop, o, r) ? vpi::retStr(r) : nullptr;
    }
    if ((o->kind == Obj::Prim || o->kind == Obj::PrimTerm) && prop != vpiType) {
        std::string r;
        return vpi::primGetStr(prop, o, r) ? vpi::retStr(r) : nullptr;
    }
    if ((o->kind == Obj::ModPath || o->kind == Obj::PathTerm) && prop != vpiType) {
        std::string r;
        return vpi::pathGetStr(prop, o, r) ? vpi::retStr(r) : nullptr;
    }
    if ((o->kind == Obj::ClassDefn || o->kind == Obj::Extends) && prop != vpiType) {   // 37.29
        std::string r;
        return vpi::classGetStr(prop, o, r) ? vpi::retStr(r) : nullptr;
    }
    if ((o->kind == Obj::Fsm || o->kind == Obj::FsmState) && prop != vpiType) {   // 40.5.2
        const FsmDef& d = fsmDefs()[(size_t)o->idx];
        std::string name = o->kind == Obj::Fsm ? d.name : d.states[o->pos].second;
        if (prop == vpiName) return vpi::retStr(name);
        if (prop == vpiFullName) return vpi::retStr(o->kind == Obj::Fsm ? d.inst + "." + d.name : d.inst + "." + d.name + "." + name);
        if (prop == vpiFile && d.file < g_covFiles.size()) return vpi::retStr(g_covFiles[d.file]);
        return nullptr;
    }
    if (o->kind == Obj::Assertion && prop != vpiType) {   // 39.3
        const AssertInfo& a = asserts()[(size_t)o->idx];
        if (prop == vpiFullName) return vpi::retStr(a.name);
        if (prop == vpiName) return vpi::retStr(a.name.substr(a.name.rfind('.') + 1));
        if (prop == vpiFile) return vpi::retStr(a.file);
        vpi::error(vpiWarning, "vpi_get_str: property " + std::to_string(prop) + " of an assertion is not supported");
        return nullptr;
    }
    std::function<std::string(const Obj*, bool)> nameOf = [&](const Obj* x, bool full) -> std::string {
        switch (x->kind) {
            case Obj::Call: return s.systfs[(size_t)x->site->systf].name;
            case Obj::Systf: return s.systfs[(size_t)x->idx].name;
            case Obj::Word: {
                std::string b = full ? vpi::fullName(*x->parent->sym) : std::string(x->parent->sym->name);
                for (int64_t i : x->index) b += "[" + std::to_string(i) + "]";
                return b;
            }
            case Obj::DynElem: {
                std::string b = nameOf(x->parent, full);
                const DynObj* d = vpi::dynOf(x->parent);
                if (d && d->strKey) return b + "[\"" + v_str(x->cval) + "\"]";
                return b + "[" + std::to_string(d && d->kind == 2 ? d->key(x->cval).i : v_to_i64(x->cval)) + "]";
            }
            case Obj::ClassObj: return "";
            case Obj::Bit: {
                if (x->sym && x->sym->kind == SymbolKind::Field)   // a struct member (37.17)
                    return full ? nameOf(x->parent, true) + "." + std::string(x->sym->name) : std::string(x->sym->name);
                if (x->sym && x->parent && x->parent->kind == Obj::ClassObj) return std::string(x->sym->name);
                std::string b = nameOf(x->parent, full);
                if (x->type == vpiPartSelect) return b + "[" + std::to_string(x->idx) + ":" + std::to_string(x->lidx) + "]";
                return b + "[" + std::to_string(x->idx) + "]";
            }
            case Obj::Typespec:
                // A typedef's name; a built-in or anonymous type has none.
                if (x->ty->kind == SymbolKind::TypeAlias) return full ? vpi::fullName(*x->ty) : std::string(x->ty->name);
                if (x->ty->kind == SymbolKind::ClassType) return full ? x->ty->getHierarchicalPath() : std::string(x->ty->name);   // 8.3
                return "";
            case Obj::Arg:
            case Obj::Const:
            case Obj::Range: return "";
            case Obj::Scope:
                // An unnamed generate block has the name the standard gives it (27.6).
                if (x->sym->kind == SymbolKind::GenerateBlock && !full) {
                    const auto& g = x->sym->as<slang::ast::GenerateBlockSymbol>();
                    std::string nm = g.getExternalName();
                    if (g.getArrayIndex()) {
                        const slang::ast::Scope* ps = g.getParentScope();
                        nm = std::string(ps ? ps->asSymbol().name : "") + "[" + std::to_string(*g.getArrayIndex()->as<int64_t>()) + "]";
                    }
                    return nm;
                }
                [[fallthrough]];
            default:
                if (!x->sym) return "";
                return full ? vpi::fullName(*x->sym) : std::string(x->sym->name);
        }
    };
    switch (prop) {
        case vpiName: return vpi::retStr(nameOf(o, false));
        case vpiFullName: return vpi::retStr(nameOf(o, true));
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
                if (sc->asSymbol().kind == SymbolKind::CompilationUnit) {
                    lib = sc->asSymbol().as<slang::ast::CompilationUnitSymbol>().sourceLibrary.name;
                    break;
                }
            return vpi::retStr(lib + "." + std::string(cfg.name));
        }
        case vpiFile:
        case vpiDefFile:
            if (o->kind == Obj::Call) return vpi::retStr(o->site->file);
            if (prop == vpiDefFile && o->kind == Obj::Module && srcMgr())
                return vpi::retStr(std::string(srcMgr()->getFileName(o->sym->as<slang::ast::InstanceSymbol>().getDefinition().location)));
            if (o->sym && srcMgr()) return vpi::retStr(std::string(srcMgr()->getFileName(o->sym->location)));
            return nullptr;
        case vpiType: {
            const char* nm = vpi::typeName(vpi::effType(o));
            if (!nm) nm = vpi::codeTypeName(o->type);
            return nm ? vpi::retStr(nm) : nullptr;
        }
        default:
            vpi::error(vpiWarning, "vpi_get_str: property " + std::to_string(prop) + " is not supported");
            return nullptr;
    }
}

vpiHandle vpi_handle_multi(PLI_INT32 type, vpiHandle a, vpiHandle b, ...);

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
        Val val;
        if (!vpi::constVal(o->sym->as<slang::ast::ParameterSymbol>().getValue(), val)) {
            vpi::error(vpiError, "vpi_get_value: parameter value of this type");
            return;
        }
        vpi::getValueOf(o, val, v);
        return;
    }
    if (vpi::isCode(o)) {   // a constant expression (37.35)
        Val val;
        if (o->kind != Obj::Expr || !vpi::codeValue(o, val)) {
            vpi::error(vpiError, "vpi_get_value: only a constant expression has a value here");
            return;
        }
        o->w = val.w;
        o->real = val.real;
        o->str = val.str;
        vpi::getValueOf(o, val, v);
        return;
    }
    if (o->kind == Obj::Iter || o->kind == Obj::Callback || o->kind == Obj::Systf || o->kind == Obj::Module ||
        o->kind == Obj::Scope || o->kind == Obj::Typespec || o->kind == Obj::Member || o->kind == Obj::Range ||
        o->kind == Obj::Modport || o->kind == Obj::Port || (o->kind == Obj::Signal && o->arr) || o->kind == Obj::Dyn ||
        o->kind == Obj::ClassObj || o->kind == Obj::ClassDefn || o->kind == Obj::Extends || o->kind == Obj::InterModPath || o->kind == Obj::Tchk || o->kind == Obj::TchkTerm ||
        o->kind == Obj::ModPath || o->kind == Obj::PathTerm || o->kind == Obj::Prim || o->kind == Obj::PrimTerm ||
        o->kind == Obj::Constraint || o->kind == Obj::ConstrItem) {
        vpi::error(vpiError, "vpi_get_value: the object has no value");
        return;
    }
    vpi::getValue(o, v);
}

vpiHandle vpi_put_value(vpiHandle h, p_vpi_value v, p_vpi_time t, PLI_INT32 flags) {
    vpi::clearErr();
    Obj* o = O(h);
    if ((flags & 0xfff) == vpiCancelEvent) {   // cancel a scheduled write (38.35)
        if (!o || o->kind != Obj::SchedEvent) { vpi::error(vpiError, "vpi_put_value: vpiCancelEvent needs a vpiSchedEvent handle"); return nullptr; }
        auto& ps = vpi::st().puts;
        for (auto it = ps.begin(); it != ps.end(); ++it)
            if (it->second.ev == o) { ps.erase(it); break; }
        o->sched = false;
        return nullptr;
    }
    if (!o || !v) { vpi::error(vpiError, "vpi_put_value: null handle"); return nullptr; }
    // Protected IP cannot be written from outside it (IEEE 1735 9.7.1).
    if (vpi::isProt(o) && !vpi::inOwnCall(o)) { vpi::error(vpiError, "vpi_put_value: the object is protected"); return nullptr; }
    if (o->kind == Obj::DynElem) {   // an element of a container: written now (38.35)
        Val val;
        if ((flags & 0xfff) == vpiForceFlag || (flags & 0xfff) == vpiReleaseFlag) { vpi::error(vpiError, "vpi_put_value: an element of a dynamic array, queue or associative array cannot be forced"); return nullptr; }
        if (!vpi::fromValue(o, v, val)) return nullptr;
        if (!vpi::dynWrite(o, val)) vpi::error(vpiError, "vpi_put_value: the element is no longer in the array");
        return nullptr;
    }
    bool target = (o->kind == Obj::Signal && !o->arr && o->type != vpiClassVar) || o->kind == Obj::Word || o->kind == Obj::Bit ||
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
    int64_t d = mode == vpiNoDelay || mode == 0 ? 0 : vpi::sigTicks(t, o);
    if (mode == vpiNoDelay || mode == 0) {
        vpi::write(o, val);
    } else if (d <= 0) {   // like a nonblocking assignment (38.35)
        if (o->kind == Obj::Bit) S.nba(o->sig, v_resize(val, o->w, false), o->off + o->w - 1, o->off);
        else S.nba(o->sig, val);
    } else {
        uint64_t when = S.now + (uint64_t)d;
        // 38.35: an inertial write removes the writes still pending on the
        // object, a transport write those due after it; a pure transport
        // write removes none.
        auto& ps = vpi::st().puts;
        if (mode == vpiInertialDelay || mode == vpiTransportDelay)
            for (auto it = ps.begin(); it != ps.end();)
                if (it->second.o == o && (mode == vpiInertialDelay || it->first > when)) {
                    if (it->second.ev) it->second.ev->sched = false;
                    it = ps.erase(it);
                } else ++it;
        Obj* ev = nullptr;
        if (flags & vpiReturnEvent) {
            ev = vpi::mkObj(Obj::SchedEvent, vpiSchedEvent);
            ev->sched = true;
            ev->parent = o;
        }
        ps.insert({when, {o, val, ev}});
        vpi::at(when);
        return H(ev);
    }
    return nullptr;
}

void vpi_get_time(vpiHandle h, p_vpi_time t) {
    vpi::clearErr();
    if (!t) return;
    if (t->type != vpiSimTime && t->type != vpiScaledRealTime) t->type = vpiSimTime;
    vpi::fillTime(t, O(h), S.now);
}

// The delays of a net declared with one (28.16: wire #(rise, fall, turn-off)),
// 38.10 and 38.30. Rise, fall and turn-off; with mtm_flag each as min, typ,
// max (all three the same: msim keeps the typical value).
namespace vpi {
inline std::vector<uint64_t>* netDelays(const Obj* o, const char* who) {
    if (!o || o->kind != Obj::Signal || o->arr || !o->sym || o->sym->kind != SymbolKind::Net || o->sig < 0) {
        error(vpiError, std::string(who) + ": delays are supported on nets only");
        return nullptr;
    }
    if (isProt(o)) { error(vpiError, std::string(who) + ": the object is protected"); return nullptr; }
    auto it = S.netDelay.find(o->sig);
    if (it == S.netDelay.end()) {
        error(vpiError, std::string(who) + ": the net has no declared delay");
        return nullptr;
    }
    return &it->second;
}
inline bool delayCount(p_vpi_delay d, const char* who) {
    if (!d || !d->da || d->no_of_delays < 1 || d->no_of_delays > 3 || d->pulsere_flag) {
        error(vpiError, std::string(who) + ": a net has 1, 2 or 3 delays (no pulse limits)");
        return false;
    }
    return true;
}
// The delays of an inter-module path as VPI gives them: 1, 2, 3, 6 or 12
// of the 12 transitions, with pulse limits.
inline bool imDelayCount(p_vpi_delay d, const char* who) {
    if (!d || !d->da) { error(vpiError, std::string(who) + ": no delay structure"); return false; }
    int n = d->no_of_delays;
    if (n != 1 && n != 2 && n != 3 && n != 6 && n != 12) {
        error(vpiError, std::string(who) + ": an inter-module path has 1, 2, 3, 6 or 12 delays");
        return false;
    }
    return true;
}
inline void imGetDelays(const Obj* o, p_vpi_delay d) {
    Delay12 z;
    auto it = S.icDelay.find(o->sig);
    const Delay12& v = it == S.icDelay.end() ? z : it->second;
    int per = (d->mtm_flag ? 3 : 1) * (d->pulsere_flag ? 3 : 1);
    for (int k = 0; k < d->no_of_delays; k++) {
        uint64_t vals[3] = {v.d[k], v.rj[k], v.er[k]};
        for (int q = 0; q < (d->pulsere_flag ? 3 : 1); q++)
            for (int m = 0; m < (d->mtm_flag ? 3 : 1); m++) {
                p_vpi_time t = &d->da[k * per + q * (d->mtm_flag ? 3 : 1) + m];
                t->type = d->time_type;
                fillTime(t, o->items.empty() ? nullptr : O(o->items[1]), vals[q]);
            }
    }
}
inline void imPutDelays(const Obj* o, p_vpi_delay d) {
    int per = (d->mtm_flag ? 3 : 1) * (d->pulsere_flag ? 3 : 1);
    std::vector<uint64_t> dv, rv, ev;
    const Obj* port = O(o->items[1]);
    for (int k = 0; k < d->no_of_delays; k++) {
        auto at = [&](int q) {
            s_vpi_time t = d->da[k * per + q * (d->mtm_flag ? 3 : 1) + (d->mtm_flag ? 1 : 0)];   // typical
            t.type = d->time_type;
            return (uint64_t)std::max<int64_t>(0, sigTicks(&t, port));
        };
        uint64_t x = at(0);
        dv.push_back(x);
        rv.push_back(d->pulsere_flag ? at(1) : rejectOf(x));
        ev.push_back(d->pulsere_flag ? at(2) : x * (uint64_t)std::max(pulseOpts().errorPct, pulseOpts().rejectPct) / 100);
    }
    auto D = expand12(dv), R = expand12(rv), E = expand12(ev);
    Delay12& v = S.icDelay[o->sig];
    for (int k = 0; k < 12; k++) {
        v.d[k] = d->append_flag ? v.d[k] + D[(size_t)k] : D[(size_t)k];
        v.rj[k] = d->append_flag ? v.rj[k] + R[(size_t)k] : R[(size_t)k];
        v.er[k] = d->append_flag ? v.er[k] + E[(size_t)k] : E[(size_t)k];
    }
    if (S.timedOut.size() < S.sigs.size()) S.timedOut.resize(S.sigs.size(), 0);
    if (S.pathIn.size() < S.sigs.size()) S.pathIn.resize(S.sigs.size(), 0);
    if (S.lastChg.size() < S.sigs.size()) S.lastChg.resize(S.sigs.size(), 0);
    S.timedOut[(size_t)o->sig] = 1;
}
}   // namespace vpi
void vpi_get_delays(vpiHandle h, p_vpi_delay d) {
    vpi::clearErr();
    Obj* o = O(h);
    if (o && o->kind == Obj::InterModPath) {   // 38.21
        if (vpi::imDelayCount(d, "vpi_get_delays")) vpi::imGetDelays(o, d);
        return;
    }
    if (o && o->kind == Obj::ModPath) {   // 38.10
        if (vpi::pathDelayCount(d, "vpi_get_delays")) vpi::pathGetDelays(o, d);
        return;
    }
    if (o && (o->kind == Obj::Prim || o->kind == Obj::ContAssign)) {   // 38.10
        if (vpi::primDelayCount(o, d, "vpi_get_delays")) vpi::primGetDelays(o, d);
        return;
    }
    if (o && o->kind == Obj::Tchk) {   // 38.10: the limits
        if (!d || !d->da || d->no_of_delays < 1) { vpi::error(vpiError, "vpi_get_delays: no delay structure"); return; }
        vpi::tchkGetDelays(o, d);
        return;
    }
    auto* nd = vpi::netDelays(o, "vpi_get_delays");
    if (!nd || !vpi::delayCount(d, "vpi_get_delays")) return;
    auto& v = *nd;
    for (int k = 0; k < d->no_of_delays; k++) {
        uint64_t x = (size_t)k < v.size() ? v[(size_t)k] : v.size() == 1 ? v[0] : std::min(v[0], v[1]);   // turn-off: the smaller (28.16)
        for (int m = 0; m < (d->mtm_flag ? 3 : 1); m++) {
            p_vpi_time t = &d->da[k * (d->mtm_flag ? 3 : 1) + m];
            t->type = d->time_type;
            vpi::fillTime(t, o, x);
        }
    }
}
void vpi_put_delays(vpiHandle h, p_vpi_delay d) {
    vpi::clearErr();
    Obj* o = O(h);
    if (o && o->kind == Obj::InterModPath) {   // 38.21
        if (vpi::imDelayCount(d, "vpi_put_delays")) vpi::imPutDelays(o, d);
        return;
    }
    if (o && o->kind == Obj::ModPath) {   // 38.10
        if (vpi::pathDelayCount(d, "vpi_put_delays")) vpi::pathPutDelays(o, d);
        return;
    }
    if (o && (o->kind == Obj::Prim || o->kind == Obj::ContAssign)) {   // 38.10
        if (vpi::primDelayCount(o, d, "vpi_put_delays")) vpi::primPutDelays(o, d);
        return;
    }
    if (o && o->kind == Obj::Tchk) {   // 38.10: the limits
        if (!d || !d->da || d->no_of_delays < 1) { vpi::error(vpiError, "vpi_put_delays: no delay structure"); return; }
        vpi::tchkPutDelays(o, d);
        return;
    }
    auto* nd = vpi::netDelays(o, "vpi_put_delays");
    if (!nd || !vpi::delayCount(d, "vpi_put_delays")) return;
    std::vector<uint64_t> v;
    for (int k = 0; k < d->no_of_delays; k++) {
        p_vpi_time t = &d->da[k * (d->mtm_flag ? 3 : 1) + (d->mtm_flag ? 1 : 0)];   // typical
        s_vpi_time tt = *t;
        tt.type = d->time_type;   // the delays are in time_type
        int64_t x = vpi::sigTicks(&tt, o);
        if (d->append_flag && (size_t)k < nd->size()) x += (int64_t)(*nd)[(size_t)k];
        v.push_back((uint64_t)std::max<int64_t>(0, x));
    }
    *nd = std::move(v);
}

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
inline PLI_INT32 assertControlOp(PLI_INT32 op, va_list ap);
inline PLI_INT32 covControlOp(PLI_INT32 op, va_list ap);
inline PLI_INT32 control(PLI_INT32 op, va_list ap) {
    clearErr();
    if (PLI_INT32 r = assertControlOp(op, ap); r >= 0) return r;
    if (PLI_INT32 r = covControlOp(op, ap); r >= 0) return r;
    if (op == vpiFinish || op == vpiStop) {
        S.finished = true;
        return 1;
    }
    error(vpiWarning, "vpi_control: operation " + std::to_string(op) + " is not supported");
    return 0;
}
// vpi_handle_multi(vpiInterModPath, port1, port2) (38.21): the path from
// an output (or inout) port of one instance to an input (or inout) port of
// another on the same net, in either order. Its delays are the
// interconnect delay of the load port (32.4.4).
inline vpiHandle handleMulti(PLI_INT32 type, vpiHandle a, vpiHandle b, va_list) {
    if (type != vpiInterModPath) {
        error(vpiError, "vpi_handle_multi: only vpiInterModPath is defined");
        return nullptr;
    }
    Obj* x = O(a);
    Obj* y = O(b);
    if (!x || !y || x->kind != Obj::Port || y->kind != Obj::Port) {
        error(vpiError, "vpi_handle_multi: vpiInterModPath needs two port handles");
        return nullptr;
    }
    using slang::ast::ArgumentDirection;
    auto dirOf = [](const Obj* p) { return p->sym->as<PortSymbol>().direction; };
    auto drives = [&](const Obj* p) { return dirOf(p) == ArgumentDirection::Out || dirOf(p) == ArgumentDirection::InOut; };
    auto loads = [&](const Obj* p) { return dirOf(p) == ArgumentDirection::In || dirOf(p) == ArgumentDirection::InOut; };
    if (!(drives(x) && loads(y))) std::swap(x, y);
    if (!(drives(x) && loads(y))) {
        error(vpiError, "vpi_handle_multi: vpiInterModPath needs an output and an input port");
        return nullptr;
    }
    auto netOf = [](const Obj* p) -> const Symbol* {
        const auto& ps = p->sym->as<PortSymbol>();
        const InstanceSymbol* inst = instOf(ps.getParentScope());
        const slang::ast::PortConnection* pc = inst ? inst->getPortConnection(ps) : nullptr;
        return pc ? rootOf(pc->getExpression()) : nullptr;
    };
    const Symbol* n = netOf(x);
    if (!n || n != netOf(y)) {
        error(vpiError, "vpi_handle_multi: the ports are not connected to one net");
        return nullptr;
    }
    const auto& in = y->sym->as<PortSymbol>();
    Obj* lv = in.internalSymbol && ValueSymbol::isKind(in.internalSymbol->kind) ? valueObj(in.internalSymbol) : nullptr;
    if (!lv || lv->sig < 0 || in.internalSymbol->kind != SymbolKind::Net) {
        error(vpiError, "vpi_handle_multi: the input port is not a net (an interconnect delay needs one)");
        return nullptr;
    }
    auto& slot = st().pairObjs[{x->sym, y->sym}];
    if (!slot) {
        slot = mkObj(Obj::InterModPath, vpiInterModPath);
        slot->parent = x;   // the source port
        slot->sym = y->sym; // the load port
        slot->items = {H(x), H(y)};
        slot->sig = lv->sig;
        slot->w = lv->w;
    }
    return H(slot);
}
// The elements a value-array call covers: `num` elements from the one at
// `index`, toward the right bound, going on from the left bound after it.
inline bool arraySpan(const char* who, const Obj* o, PLI_INT32* index, PLI_UINT32 num, const ArrShape*& sh, int64_t& first) {
    if (!o || o->kind != Obj::Signal || !o->arr || o->real) { error(vpiError, std::string(who) + ": the object is not an array of variables"); return false; }
    if (isProt(o)) { error(vpiError, std::string(who) + ": the object is protected"); return false; }
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

// 39.4.2: a callback on one assertion. It is called with the reason, the
// time, the assertion, the attempt (for the start and the outcomes of an
// attempt; null for a change made by assertion control) and the user data.
vpiHandle vpi_register_assertion_cb(vpiHandle assertion, PLI_INT32 reason, vpi_assertion_callback_func* fn, PLI_BYTE8* ud) {
    vpi::clearErr();
    auto& s = vpi::st();
    Obj* a = O(assertion);
    if (!a || a->kind != Obj::Assertion) { vpi::error(vpiError, "vpi_register_assertion_cb: not an assertion handle"); return nullptr; }
    if (!fn) { vpi::error(vpiError, "vpi_register_assertion_cb: no callback routine"); return nullptr; }
    if (!vpi::assertReason(reason)) {
        vpi::error(vpiError, "vpi_register_assertion_cb: reason " + std::to_string(reason) + " is not an assertion callback reason");
        return nullptr;
    }
    if (asserts()[(size_t)a->idx].prot) { vpi::error(vpiError, "vpi_register_assertion_cb: the assertion is protected"); return nullptr; }
    s.cbs.push_back(std::make_unique<vpi::Cb>());
    vpi::Cb* c = s.cbs.back().get();
    c->d.reason = reason;
    c->d.user_data = ud;
    c->assertFn = fn;
    c->assertId = a->idx;
    s.assertCbs[a->idx].push_back(c);
    s.active = true;
    Obj* h = vpi::mkObj(Obj::Callback, vpiCallback);
    h->cb = c;
    c->self = h;
    return H(h);
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
