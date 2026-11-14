// vhdl/ast.hpp - syntax tree of msim's VHDL front end.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lex.hpp"

namespace vhdl {

struct Expr;
struct Stmt;
struct Decl;
struct Subprog;
using EP = std::shared_ptr<Expr>;
using SP = std::shared_ptr<Stmt>;
using DP = std::shared_ptr<Decl>;

enum class EK {
    Name,     // simple name (low / text)
    Sel,      // a.b   (a = prefix, name = suffix; suffix "all" for .all)
    Call,     // a(args): call, index, slice or conversion; resolved later
    Attr,     // a'name or a'name(arg)
    Int, Real, Phys,   // abstract literals; Phys: value in rval/ival, unit in name
    Char,     // 'c' (text)
    Str,      // "..." (text)
    BitStr,   // expanded bit string (text)
    Agg,      // aggregate
    Qual,     // T'(expr) (a = type mark, b = operand)
    Bin, Un,  // operators (op)
    Range,    // a to/downto b
    Open, Others, Null,
    New,      // allocator (a = subtype or qualified expr)
    External, // << signal .a.b : T >> (text = path, name = class)
    Box,      // <>  (unconstrained index)
    SubtypeRange,   // type mark with a range constraint: a = mark, b = Range
    Cond,     // VHDL-2019 conditional expression (9.1): a when b else args[0].actual
};

struct Assoc {
    EP formal;   // may be null (positional)
    EP actual;   // may be Open
};

struct AggElem {
    std::vector<EP> choices;   // empty: positional
    EP value;
};

struct Expr {
    EK k;
    int line = 0;
    std::string name, text, op;   // name: lower-case identifier/attribute/unit; text: as written
    int64_t ival = 0;
    double rval = 0;
    bool downto = false;
    EP a, b;
    std::vector<Assoc> args;
    std::vector<AggElem> agg;
    explicit Expr(EK k, int line = 0) : k(k), line(line) {}
};

struct SubInd {
    EP mark;                    // type mark (Name/Sel), null if none
    EP resolution;              // resolution function, ignored
    std::vector<EP> idx;        // index constraint (ranges or subtype names)
    std::vector<std::vector<EP>> elemIdx;   // VHDL-2008: element constraints, e.g. (open)(7 downto 0)
    EP range;                   // range constraint (Range or Attr)
    bool present() const { return (bool)mark; }
};

struct TypeDef {
    enum K { Enum, Int, Real, Phys, Array, Record, Access, File, Incomplete, Protected, ProtectedBody } k = Incomplete;
    std::vector<std::string> lits;          // enum: lower-case literal names; character literals as "'c'"
    std::vector<std::string> litText;       // as written
    EP range;                               // Int/Real/Phys
    std::vector<EP> index;                  // Array: constraint ranges or (unconstrained) Box/SubtypeRange
    bool unconstrained = false;
    SubInd elem;                            // Array element, Access/File designated type
    std::vector<DP> fields;                 // Record (Decl::Field)
    std::string baseUnit;                   // Phys
    std::vector<std::pair<std::string, EP>> units;   // Phys secondary units: name -> Phys literal
    std::vector<DP> decls;                  // Protected
};

struct Wave {
    EP val;
    EP after;
};

struct Alt {   // case / selected alternatives, conditional branches
    std::vector<EP> choices;   // case: choices; if: [cond]
    EP cond;                   // if/elsif/when condition (null: else)
    std::vector<SP> body;      // sequential/concurrent body
    std::vector<Wave> waves;   // conditional/selected signal assignment
    std::vector<DP> decls;     // generate alternatives
    std::string label;         // generate alternative label
};

enum class SK {
    // sequential
    SigAssign, VarAssign, If, Case, Loop, Next, Exit, Return, Null, Wait, Assert, Report, Call,
    CondSigAssign, SelSigAssign, CondVarAssign, SelVarAssign, Force, Release,
    // concurrent
    Process, ConcAssign, ConcCond, ConcSel, Inst, ForGen, IfGen, CaseGen, Block, ConcAssert, ConcCall,
};

struct Stmt {
    SK k;
    int line = 0, col = 0;
    std::string label;      // as written
    EP target;
    std::vector<Wave> waves;
    EP value;               // variable assignment value, return value, case selector, selected-assignment selector
    std::vector<Alt> alts;  // if / case / conditional / selected / if-generate / case-generate
    bool transport = false, guarded = false, postponed = false, matching = false, forced = false;
    EP reject;
    // loops / generate
    enum LoopK { Plain, While, For } loop = Plain;
    EP cond;                // while condition, next/exit condition, assert condition, wait until
    std::string var;        // loop / for-generate variable (as written)
    EP range;
    std::vector<SP> body;
    std::string loopLabel;  // next/exit target
    // wait
    std::vector<EP> on;
    EP forTime;
    // assert / report
    EP report, severity;
    // call
    EP call;
    // process / block / generate
    std::vector<EP> sens;
    bool sensAll = false;
    bool hasSens = false;
    std::vector<DP> decls;
    // instance
    EP unit;                // component or entity name (Name / Sel)
    std::string unitKind;   // "component", "entity", "configuration" or "" (component)
    std::string arch;       // entity e(arch)
    std::vector<Assoc> genericMap, portMap;
    explicit Stmt(SK k, int line = 0) : k(k), line(line) {}
};

enum class DK {
    Signal, Variable, Constant, File, Type, Subtype, Component, Subprog, Alias, Attribute, AttrSpec,
    Use, Library, Generic, Port, Field, Param, Config, Group, Disconnect, Context, PackageInst, Ignored,
    SubInst,   // function f is new g generic map (...): target = g
    View,      // mode view: type = the record, tdef->fields = element names with their modes
};

struct Decl {
    DK k;
    int line = 0;
    std::vector<std::string> names;   // lower case
    std::vector<std::string> texts;   // as written
    SubInd type;
    EP init;
    std::string mode;          // port/param: in out inout buffer linkage
    std::string sigKind;       // guarded signal (6.4.2.3): "bus" or "register"
    std::string cls;           // param/generic class: signal variable constant file type function procedure package
    bool shared = false;
    std::shared_ptr<TypeDef> tdef;
    std::shared_ptr<Subprog> sub;
    std::vector<DP> generics, ports;   // component
    std::vector<EP> uses;              // use clause names
    EP target;                         // alias target, file name, attribute value
    EP openKind;                       // file open kind
    std::string attrName;              // attribute spec
    std::shared_ptr<struct Unit> cfg;  // configuration specification: its bindings (Unit::binds)
    std::vector<Assoc> genericMap;     // subprogram instantiation
};

struct Subprog {
    bool isFunc = false;
    bool pure = true;
    std::string name, text;   // lower case / as written; operators keep their quotes: "\"+\""
    std::vector<DP> params;   // Decl::Param
    SubInd ret;
    bool hasBody = false;
    std::vector<DP> decls;
    std::vector<SP> body;
    std::vector<DP> generics;   // VHDL-2008 subprogram generics
    int line = 0;
};

struct Unit {
    enum K { Entity, Arch, Package, PackageBody, Config, Context, PackageInst } k = Entity;
    std::string name, text;   // lower / as written
    std::string of, ofText;   // architecture: entity; package body: package; configuration: entity; package instance: uninstantiated package
    std::vector<DP> ctx;      // library and use clauses before the unit
    std::vector<DP> generics, ports, decls;
    std::vector<SP> stmts;
    std::vector<Assoc> genericMap;   // package instance
    std::string library = "work";
    std::string file;
    int line = 0;
    // configuration: architecture chosen for the top
    std::string configArch;
    struct Bind {
        std::string label, comp, entity, arch;
        bool isConfig = false;              // use configuration <entity>
        std::shared_ptr<Unit> sub;          // nested block configuration of the bound architecture
    };
    std::vector<Bind> binds;
};

using UP = std::shared_ptr<Unit>;

}  // namespace vhdl
