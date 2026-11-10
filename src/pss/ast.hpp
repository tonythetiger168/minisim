// ast.hpp - the syntax tree of a Portable Stimulus (Accellera PSS DSL) model,
// as mpss reads it. See docs/MPSS.md for the supported subset.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pss {

using i128 = __int128;

struct Loc {
    int file = 0, line = 0;
};

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Expr;
using EP = std::shared_ptr<Expr>;

// One step of a reference: name, or name[index].
struct PathElem {
    std::string name;
    EP index;
};

struct Expr {
    enum K { Num, Str, Ref, Unary, Binary, Cond, In, Call } k = Num;
    i128 num = 0;
    std::string s;                           // Str text, operator, or Call name
    std::vector<EP> a;                       // operands / call arguments
    std::vector<std::string> scope;          // Ref: the a::b:: qualifiers
    std::vector<PathElem> path;              // Ref
    std::vector<std::pair<EP, EP>> ranges;   // In: [lo..hi] pairs (hi null for a single value)
    Loc loc;
};

struct TypeRef {
    enum K { Void, Bit, Int, Bool, String, Named } k = Void;
    EP width;                                // Bit / Int: null means the default
    std::vector<std::pair<EP, EP>> dom;      // "in [..]" restriction
    std::string name;                        // Named (possibly a::b)
};

struct Cons;
using CP = std::shared_ptr<Cons>;
struct Cons {
    enum K { Expr_, If, Foreach, Unique } k = Expr_;
    EP e;                                    // Expr_ expression, If condition, Foreach collection
    std::vector<CP> body, els;
    std::string it, idx;                     // Foreach iterator and index names
    std::vector<EP> list;                    // Unique
    Loc loc;
};

struct Act;
using AP = std::shared_ptr<Act>;
struct Act {
    enum K { Seq, Par, Sched, Select, Repeat, Replicate, If, Traverse, Do, Constraint, Bind } k = Seq;
    std::vector<AP> body, els;
    std::vector<EP> weights, guards;         // Select: one per branch (null when absent)
    EP e;                                    // Repeat count, If condition
    std::string idx;                         // Repeat index variable
    PathElem handle;                         // Traverse
    std::string type;                        // Do
    std::vector<CP> with;                    // Traverse / Do inline constraints, Constraint
    std::vector<EP> binds;                   // Bind endpoints
    Loc loc;
};

struct Stmt;
using SP = std::shared_ptr<Stmt>;
struct Stmt {
    enum K { Expr_, Assign, Var, If, Repeat, While, Return, Block, Break, Continue } k = Expr_;
    EP e, lhs;                               // expression / condition / count / initializer; Assign target
    std::string op;                          // Assign operator (=, +=, ...)
    TypeRef type;                            // Var
    std::string name;                        // Var name, Repeat index
    std::vector<SP> body, els;
    Loc loc;
};

struct Exec {
    std::string kind;                        // body, header, declaration, init, post_solve, ...
    std::string lang;                        // "" for a procedural block, else the target language
    std::string text;                        // target template
    std::vector<SP> stmts;
    Loc loc;
};

struct Param {
    TypeRef type;
    std::string name;
};
struct Function {
    std::string name;
    TypeRef ret;
    std::vector<Param> params;
    bool hasBody = false;
    std::vector<SP> body;
    Loc loc;
};

struct Field {
    enum K { Data, Input, Output, Lock, Share, Pool } k = Data;
    bool rand = false;
    TypeRef type;
    std::string name;
    EP dim;                                  // array size, or null
    EP init;
    EP poolSize;                             // Pool
    Loc loc;
};

struct TypeDecl {
    enum K { Struct, Buffer, Stream, State, Resource, Action, Component, Enum } k = Struct;
    std::string name, qname, super;
    bool isAbstract = false, flattened = false;
    std::vector<Field> fields;
    std::vector<CP> cons;
    std::vector<AP> activity;
    std::vector<Exec> execs;
    std::vector<std::pair<std::string, EP>> items;   // Enum
    std::vector<std::pair<std::string, i128>> values; // Enum, evaluated
    std::vector<std::unique_ptr<TypeDecl>> nested;
    TypeDecl* owner = nullptr;               // the component an action is declared in
    int id = 0;
    Loc loc;

    const Field* field(const std::string& n) const {
        for (auto& f : fields)
            if (f.name == n) return &f;
        return nullptr;
    }
    bool isFlow() const { return k == Buffer || k == Stream || k == State; }
};

struct Model {
    std::vector<std::string> files;
    std::vector<std::unique_ptr<TypeDecl>> types;     // top level and package level
    std::vector<TypeDecl*> all;                       // every type, nested included
    std::map<std::string, TypeDecl*> byName;          // qualified and simple names
    std::vector<Function> funcs;
    std::map<std::string, const Function*> funcByName;
    std::map<std::string, EP> consts;                 // package-level constants
    std::map<std::string, TypeRef> typedefs;
    std::vector<std::string> warnings;

    std::string where(Loc l) const {
        return (l.file < (int)files.size() ? files[l.file] : std::string("?")) + ":" + std::to_string(l.line);
    }
    [[noreturn]] void fail(Loc l, const std::string& m) const { throw Error(where(l) + ": " + m); }
};

inline std::string i128str(i128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    unsigned __int128 u = neg ? -(unsigned __int128)v : (unsigned __int128)v;
    std::string s;
    while (u) { s.insert(s.begin(), char('0' + (int)(u % 10))); u /= 10; }
    return neg ? "-" + s : s;
}

} // namespace pss
