// vhdl/types.hpp - VHDL types and symbols as the translator sees them, and
// how each maps to a SystemVerilog type.
//
// Ranges are kept as SystemVerilog expression text (so a range may depend on
// a generic, which becomes a parameter) plus the folded value when it is a
// constant.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"

namespace vhdl {

struct Type;
using TP = std::shared_ptr<Type>;

struct Rng {
    std::string l, r;   // SystemVerilog text of 'left and 'right
    bool downto = false;
    std::optional<int64_t> lv, rv;
    bool box = false;   // unconstrained (range <>)
    TP idx;             // index type (integer, an enumeration, ...)
    std::optional<int64_t> len;   // constant length of a range whose bounds are not constant
};

struct Type {
    enum K { Logic, Bit, Bool, Char, Int, Real, Time, Enum, Array, Record, String, Line, File, Access, Protected, Unknown } k = Unknown;
    std::string name, text;   // lower / as written ("" for anonymous)
    std::string sv;           // typedef name (Enum, Record)
    std::string home;         // package that declares it ("" module-local, "*" built in)
    // Enum
    std::vector<std::string> lits;     // lower case, character literals as 'c'
    std::vector<std::string> litSv;    // SystemVerilog names of the literals
    // Int / Real / Time / Enum subrange
    std::optional<Rng> range;
    // Array
    std::vector<Rng> dims;
    TP elem;
    std::string flavor;   // "slv", "unsigned", "signed", "bv", "string", "" (other)
    // Record
    std::vector<std::string> fnames, ftext;
    std::vector<TP> ftypes;
    // Access / File: designated type
    TP target;
    TP base;   // the type a subtype constrains
    bool resolved = false;
    bool isX01 = false;
};

inline bool isScalarBit(const TP& t) { return t && (t->k == Type::Logic || t->k == Type::Bit || t->k == Type::Bool); }
inline bool isArray(const TP& t) { return t && t->k == Type::Array; }
inline bool isVec(const TP& t) { return isArray(t) && t->dims.size() == 1 && isScalarBit(t->elem); }
inline bool isLogicVec(const TP& t) { return isVec(t) && t->elem->k == Type::Logic; }
inline bool isNumVec(const TP& t) { return isVec(t) && (t->flavor == "unsigned" || t->flavor == "signed"); }
inline bool isSignedVec(const TP& t) { return isVec(t) && t->flavor == "signed"; }
inline bool isUnconstrained(const TP& t) {
    if (!isArray(t)) return false;
    for (auto& d : t->dims)
        if (d.box) return true;
    return t->elem && isUnconstrained(t->elem);
}
inline bool isIntLike(const TP& t) { return t && (t->k == Type::Int); }
inline bool isFourState(const TP& t) {
    if (!t) return false;
    if (t->k == Type::Logic) return true;
    if (t->k == Type::Array) return isFourState(t->elem);
    if (t->k == Type::Record)
        for (auto& f : t->ftypes)
            if (isFourState(f)) return true;
    return false;
}
// A type SystemVerilog can pack: bit-like scalars, enums, integers and
// arrays/records of those.
inline bool isPackable(const TP& t) {
    if (!t) return false;
    switch (t->k) {
    case Type::Logic: case Type::Bit: case Type::Bool: case Type::Char: case Type::Int: case Type::Enum: return true;
    case Type::Array: return isScalarBit(t->elem) && !isUnconstrained(t);
    case Type::Record:
        for (auto& f : t->ftypes)
            if (!isPackable(f)) return false;
        return true;
    default: return false;
    }
}

inline TP baseOf(TP t) {
    while (t && t->base) t = t->base;
    return t;
}
inline bool sameBase(const TP& a, const TP& b) {
    TP x = baseOf(a), y = baseOf(b);
    if (!x || !y) return false;
    if (x == y) return true;
    if (x->k != y->k) return false;
    if (x->k == Type::Array) return x->flavor == y->flavor && sameBase(x->elem, y->elem) && x->name == y->name;
    if (x->k == Type::Enum || x->k == Type::Record) return x->name == y->name && x->home == y->home;
    return true;
}

struct Sym {
    enum K { Obj, TypeK, Lit, Sub, Comp, Entity, Pkg, Unit, Alias, Lib, Label } k = Obj;
    std::string cls;    // Obj: signal variable constant generic port file loop
    std::string name, text, sv;
    TP type;
    std::string home;   // package that declares it ("" local)
    std::shared_ptr<Decl> decl;
    std::shared_ptr<Subprog> sub;
    std::string mode;
    int lit = -1;
    std::string builtin;   // built-in subprogram id
    EP aliasOf;
    std::string aliasSv;   // alias: SystemVerilog text of the aliased object
    int64_t unitFs = 0;    // physical unit in femtoseconds (time)
    std::shared_ptr<vhdl::Unit> unit;
    bool deferred = false;
    std::string pkgOfSub;   // subprogram declared in this package
    bool member = false;    // a method or variable of a protected type
    TP owner;               // method: its protected type
};

using Scope = std::map<std::string, std::vector<Sym>>;

}  // namespace vhdl
