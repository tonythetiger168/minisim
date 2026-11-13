// lower.hpp - lower slang's elaborated AST into Sim signals + Prog processes.
// Built against slang v12.1 (the tag FetchContent pins in CMakeLists.txt).
//
// Supported subset (a parent may instantiate a child; ports connect):
//   statements : begin/end, if/else, case/casez/casex, blocking &
//                non-blocking assign to a variable, a[const], a[hi:lo],
//                compound assign (+= -= *= /= %= &= |= ^= <<= >>= <<<= >>>=),
//                @(posedge/negedge/any sig [or ...]), @*, always_comb,
//                for/while/do/forever/repeat(const), break/continue,
//                #<literal>, $display / $strobe / $monitor / $finish, assign
//                (optional drive strength), buf/not/and/nand/or/nor/xor/xnor
//                and pullup/pulldown (same strength spec), module-level task
//                and function calls (input, output, inout)
//   expressions: literals, variable refs, unary/binary ops, ?:, {concat},
//                a[expr], a[hi:lo], a function call
// Selects use the declared range ([7:0], [8:1], [0:7], [3:0][7:0]); see packedSel.
// A ConversionExpression is lowered at the conversion's bit width so a
// context-determined operator (a = 4'hF + 4'h1 with a 8 bits wide) is not
// evaluated at the operands' self-determined width. Widening sign-extends
// when the operand is signed in that context (both operands signed, or an
// assignment of a signed expression). $signed / $unsigned retag the bits.
// A module instance connects each port with a continuous assign. An input
// expression drives the child's formal. An output's external expression is an
// assignment whose right-hand side is an EmptyArgument (the formal), the same
// shape as a subroutine output write-back. An inout net is both directions.
// Automatic tasks, class methods, and automatic functions that recurse or
// fork run each call in a frame (expr.hpp: Frame); their variables are
// relative ids. Everything else is a static signal.
// Not yet:  ref arguments, DPI, class methods,
//           hierarchical calls, instance arrays, interfaces, hierarchical
//           references into a child. Arguments are one static copy per subroutine.
//
// Usage:
//   LowerResult r = lower(compilation,
//                         [&](const std::string& n, int w, bool net){ return S.sig(n, w, net); },
//                         [&](int sig){ return S.newDriver(sig); });
//   for (auto& pr : r.procs) S.spawn(exec(&pr.prog));
#pragma once
#include <cmath>
#include <deque>
#include <fstream>
#include <sstream>
#include <set>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "prog.hpp"
#include "sva.hpp"
#include "sva_engine.hpp"
#include "clocking.hpp"
#include "cover.hpp"
#include "tchk.hpp"
#include "specify.hpp"
#include "sdf.hpp"
#include "fsm_extract.hpp"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/text/SourceManager.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/SemanticFacts.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/SpecifySymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/text/SourceManager.h"

// Fixed-size unpacked arrays (7.4), keyed by the array's symbol. SigCollector
// fills it; Lowerer reads it. lower() clears it first, since symbols from an
// earlier compilation may share addresses with this one's.
using ArrMap = std::unordered_map<const slang::ast::Symbol*, std::shared_ptr<ArrShape>>;
inline ArrMap& arrMap() { static ArrMap m; return m; }
constexpr int64_t kMaxArrayElems = 1 << 20;
// The design's source manager, for file names and line numbers in messages.
inline const slang::SourceManager*& srcMgr() { static const slang::SourceManager* m = nullptr; return m; }
// Net types that resolve differently from wire (6.6): signal -> kind.
// alias a = b = c; (10.11): every net in an alias maps to one of them, which
// is the only one that gets a signal.
inline std::unordered_map<const slang::ast::Symbol*, int>& interconnectW() {
    static std::unordered_map<const slang::ast::Symbol*, int> m;
    return m;
}
inline std::unordered_map<const slang::ast::Symbol*, const slang::ast::Symbol*>& aliasOf() {
    static std::unordered_map<const slang::ast::Symbol*, const slang::ast::Symbol*> m;
    return m;
}
// Events that an assignment or initializer may make an alias of another
// (6.17): symbol -> a 64-bit slot holding (signal id + 1) of the event it names now.
inline std::unordered_map<const slang::ast::Symbol*, int>& eventPtr() {
    static std::unordered_map<const slang::ast::Symbol*, int> m;
    return m;
}
inline std::unordered_set<const slang::ast::Symbol*>& assignedEvents() {
    static std::unordered_set<const slang::ast::Symbol*> m;
    return m;
}
// Annex D: the targets of $scope calls (D.11) in the order first seen; the
// interactive scope is a signal holding 0 (the first top-level module) or
// 1 + an index here. annexDUsed: a task that -compile leaves to the interpreter.
inline std::vector<const slang::ast::Symbol*>& scopeTargets() { static std::vector<const slang::ast::Symbol*> v; return v; }
inline const slang::ast::Symbol*& scopeDefault() { static const slang::ast::Symbol* s = nullptr; return s; }
inline int& curScopeSig() { static int s = -1; return s; }
inline bool& annexDUsed() { static bool f = false; return f; }
// Annex E: the optional compiler directives in force where a module is
// declared. slang reads and drops them, so msim finds them in the source
// text before the module: `delay_mode_distributed / _path / _unit / _zero,
// `default_decay_time, `default_trireg_strength; `resetall clears them
// (22.3). A directive in another file than the module is not seen.
struct DelayDirectives {
    enum Mode : uint8_t { NONE, DISTRIBUTED, PATH, UNIT, ZERO };
    Mode mode = NONE;
    bool decaySet = false, decayInfinite = false;
    double decay = 0;   // in the module's time units
    int triregStrength = -1;
};
inline std::unordered_map<uint32_t, std::vector<std::pair<size_t, DelayDirectives>>>& directiveMarks() {
    static std::unordered_map<uint32_t, std::vector<std::pair<size_t, DelayDirectives>>> m;
    return m;
}
// The directives of the module that holds scope `sc`.
inline DelayDirectives delayDirectives(const slang::ast::Scope* sc) {
    using namespace slang::ast;
    const Symbol* s = sc ? &sc->asSymbol() : nullptr;
    while (s && s->kind != SymbolKind::InstanceBody) s = s->getParentScope() ? &s->getParentScope()->asSymbol() : nullptr;
    if (!s || !srcMgr()) return {};
    const DefinitionSymbol& def = s->as<InstanceBodySymbol>().getDefinition();
    slang::SourceLocation loc = srcMgr()->getFullyOriginalLoc(def.location);
    if (!loc.valid()) return {};
    auto [it, fresh] = directiveMarks().try_emplace(loc.buffer().getId());
    auto& marks = it->second;
    if (fresh) {   // every directive of the file, once: where it ends and the state after it
        std::string_view t = srcMgr()->getSourceText(loc.buffer());
        DelayDirectives cur;
        marks.push_back({0, cur});
        size_t p = 0, n = t.size();
        auto word = [&](size_t& q) {
            while (q < n && (t[q] == ' ' || t[q] == '\t')) q++;
            size_t b = q;
            while (q < n && !isspace((unsigned char)t[q]) && t[q] != '/') q++;
            return std::string(t.substr(b, q - b));
        };
        while (p < n) {
            char c = t[p];
            if (c == '/' && p + 1 < n && t[p + 1] == '/') { while (p < n && t[p] != '\n') p++; continue; }
            if (c == '/' && p + 1 < n && t[p + 1] == '*') { size_t e = t.find("*/", p + 2); p = e == std::string_view::npos ? n : e + 2; continue; }
            if (c == '"') { p++; while (p < n && t[p] != '"' && t[p] != '\n') p += t[p] == '\\' ? 2 : 1; p++; continue; }
            if (c != '`') { p++; continue; }
            size_t q = p + 1;
            while (q < n && (isalnum((unsigned char)t[q]) || t[q] == '_')) q++;
            std::string_view d = t.substr(p + 1, q - p - 1);
            bool hit = true;
            if (d == "delay_mode_distributed") cur.mode = DelayDirectives::DISTRIBUTED;
            else if (d == "delay_mode_path") cur.mode = DelayDirectives::PATH;
            else if (d == "delay_mode_unit") cur.mode = DelayDirectives::UNIT;
            else if (d == "delay_mode_zero") cur.mode = DelayDirectives::ZERO;
            else if (d == "resetall") cur = DelayDirectives{};
            else if (d == "default_decay_time") {
                std::string w = word(q);
                cur.decaySet = true;
                cur.decayInfinite = w == "infinite";
                cur.decay = cur.decayInfinite ? 0 : std::strtod(w.c_str(), nullptr);
            } else if (d == "default_trireg_strength") cur.triregStrength = (int)std::strtol(word(q).c_str(), nullptr, 10);
            else hit = false;
            if (hit) marks.push_back({q, cur});
            p = q;
        }
    }
    size_t off = loc.offset();
    auto m = std::upper_bound(marks.begin(), marks.end(), off, [](size_t o, const auto& e) { return o < e.first; });
    return std::prev(m)->second;
}
// `default_trireg_strength n (E.3) as a charge strength: 1 small, 2 or 3
// medium, 4 and above large.
inline uint8_t chargeLevel(int n) { return n <= 1 ? STR_SMALL : n < 4 ? STR_MEDIUM : STR_LARGE; }
inline bool& usesForce() { static bool f = false; return f; }   // some force / assign (10.6): no partitions
inline std::vector<std::pair<int, uint8_t>>& netKinds() { static std::vector<std::pair<int, uint8_t>> v; return v; }
// Fixed-size arrays of containers (int f[2][]): one container per element,
// their ids in a row; the shape's base is the first.
inline std::unordered_map<const slang::ast::Symbol*, std::shared_ptr<ArrShape>>& dynArrMap() {
    static std::unordered_map<const slang::ast::Symbol*, std::shared_ptr<ArrShape>> m;
    return m;
}
// Dynamic arrays, queues, and associative arrays: symbol -> handle signal.
inline std::unordered_map<const slang::ast::Symbol*, int>& dynMap() {
    static std::unordered_map<const slang::ast::Symbol*, int> m;
    return m;
}

// Automatic variables that live in a frame (expr.hpp): symbol -> frame
// shape and slot. An unpacked array's shape has its first slot as base; a
// container is one slot whose empty shape is in the layout's dyn list.
struct LocalSlot { FrameLayout* lay = nullptr; int off = 0; };
inline std::unordered_map<const slang::ast::Symbol*, LocalSlot>& localMap() {
    static std::unordered_map<const slang::ast::Symbol*, LocalSlot> m;
    return m;
}
inline std::unordered_map<const slang::ast::Symbol*, std::pair<FrameLayout*, std::shared_ptr<ArrShape>>>& localArr() {
    static std::unordered_map<const slang::ast::Symbol*, std::pair<FrameLayout*, std::shared_ptr<ArrShape>>> m;
    return m;
}
inline std::unordered_map<const slang::ast::Symbol*, LocalSlot>& localDyn() {
    static std::unordered_map<const slang::ast::Symbol*, LocalSlot> m;
    return m;
}
// `ref` arguments (13.5.2): the formal's slot holds the actual's signal id.
// kind as PropSlot (0 value, 1 unpacked array, 2 container), with the shape.
struct RefFormal { int kind = 0; std::shared_ptr<ArrShape> arr; DynObj dyn; };
inline std::unordered_map<const slang::ast::Symbol*, RefFormal>& refFormals() {
    static std::unordered_map<const slang::ast::Symbol*, RefFormal> m;
    return m;
}
// Frame shapes outlive the result that owns them: processes still
// suspended when the program exits are destroyed with the kernel, after
// main's LowerResult, and each of their frames goes back to its shape's
// pool. So the shapes are never freed.
struct LayoutList : std::vector<std::unique_ptr<FrameLayout>> {
    LayoutList() = default;
    LayoutList(LayoutList&&) = default;
    LayoutList& operator=(LayoutList&&) = default;
    ~LayoutList() { for (auto& p : *this) (void)p.release(); }
};
// Owner of every frame shape made by this lowering (LowerResult::layouts).
inline std::vector<std::unique_ptr<FrameLayout>>*& layoutStore() {
    static std::vector<std::unique_ptr<FrameLayout>>* v = nullptr;
    return v;
}
inline FrameLayout* newLayout(std::string name) {
    if (!layoutStore()) throw std::runtime_error("lower: unsupported automatic variables here");
    layoutStore()->push_back(std::make_unique<FrameLayout>());
    layoutStore()->back()->name = std::move(name);
    return layoutStore()->back().get();
}

// Specparams an SDF LABEL (32.4.3) gives a new value, in the module's time
// unit. Every expression that reads one is computed with that value.
inline std::unordered_map<const slang::ast::Symbol*, double>& labelled() {
    static std::unordered_map<const slang::ast::Symbol*, double> m;
    return m;
}

// Time (22.7, 3.14.3): one kernel tick is the simulation precision, the finest
// time precision of any module, as a power of ten of a second (-9 = 1ns).
inline int& simPrecExp() { static int p = -9; return p; }
inline int tsExp(const slang::TimeScaleValue& v) {
    int e = 0;
    switch (v.unit) {
        case slang::TimeUnit::Seconds: e = 0; break;
        case slang::TimeUnit::Milliseconds: e = -3; break;
        case slang::TimeUnit::Microseconds: e = -6; break;
        case slang::TimeUnit::Nanoseconds: e = -9; break;
        case slang::TimeUnit::Picoseconds: e = -12; break;
        case slang::TimeUnit::Femtoseconds: e = -15; break;
    }
    if (v.magnitude == slang::TimeScaleMagnitude::Ten) e += 1;
    if (v.magnitude == slang::TimeScaleMagnitude::Hundred) e += 2;
    return e;
}
// Named blocks (9.6.2): an id for each block some `disable` names from outside
// the block, and the blocks that are a whole process (Prog::roots).
struct BlockIds {
    std::unordered_map<const slang::ast::Symbol*, int> id;
    std::unordered_set<int> roots;
    std::unordered_set<const slang::ast::Symbol*> targets;   // every block and task some disable names
    int of(const slang::ast::Symbol* s) {
        auto [it, fresh] = id.try_emplace(s, (int)id.size());
        return it->second;
    }
};
inline BlockIds& blockIds() { static BlockIds b; return b; }
// Bits a value of this type takes in a signal: a string is a 64-bit id (6.16).
// Unpacked structs and unions (7.2, 7.3) are kept as one vector: the
// members in declared order, the first at the top, each in its own storage
// (a real as its 64 bits, a string as its 64-bit pool id). A union's members
// all start at bit 0. -1: a member that cannot be laid out this way.
inline int uWidth(const slang::ast::Type& t0);
// A string is a 64-bit pool id; a shortreal is kept as a double, rounded to
// single precision whenever a shortreal value is made (6.12); a chandle is a
// 64-bit pointer (6.14).
inline bool isHandleType(const slang::ast::Type& t);
inline int storeW(const slang::ast::Type& t0) {
    const auto& t = t0.getCanonicalType();
    if (t.kind == slang::ast::SymbolKind::CovergroupType) return 32;   // a covergroup handle (19.3)
    if (t.isUntypedType()) return 64;   // a typeless mailbox's message (15.4.9): the value as it is
    if (t.isUnpackedStruct() || t.isUnpackedUnion()) return uWidth(t);
    return t.isString() || t.isFloating() || t.isCHandle() || isHandleType(t) ? 64 : (int)t.getBitWidth();
}
// Tag bits of a tagged union (7.3.2): enough to number its members. The tag
// is at the top; a member's value is right-justified below it.
inline int tagBitsOf(const slang::ast::Type& t0) {
    using namespace slang::ast;
    const auto& t = t0.getCanonicalType();
    if (t.kind == SymbolKind::PackedUnionType) return (int)t.as<PackedUnionType>().tagBits;
    size_t n = t.as<UnpackedUnionType>().fields.size();
    int b = 1;
    while (((size_t)1 << b) < n) b++;
    return b;
}
inline bool isTaggedUnion(const slang::ast::Type& t0) {
    using namespace slang::ast;
    const auto& t = t0.getCanonicalType();
    if (t.kind == SymbolKind::PackedUnionType) return t.as<PackedUnionType>().isTagged;
    if (t.kind == SymbolKind::UnpackedUnionType) return t.as<UnpackedUnionType>().isTagged;
    return false;
}
inline int tagIndexOf(const slang::ast::Type& t0, const slang::ast::Symbol& f) {
    using namespace slang::ast;
    int k = 0;
    for (const auto& g : t0.getCanonicalType().as<Scope>().membersOfType<FieldSymbol>()) {
        if (&g == &f) return k;
        k++;
    }
    return -1;
}
inline int uWidth(const slang::ast::Type& t0) {
    using namespace slang::ast;
    const auto& t = t0.getCanonicalType();
    if (isTaggedUnion(t) && t.isUnpackedUnion()) {
        int w = 0;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
            if (f.getType().isVoid()) continue;
            int fw = uWidth(f.getType());
            if (fw <= 0) return -1;
            w = std::max(w, fw);
        }
        return w + tagBitsOf(t);
    }
    if (t.isUnpackedStruct() || t.isUnpackedUnion()) {
        int w = 0;
        bool u = t.isUnpackedUnion();
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
            int fw = uWidth(f.getType());
            if (fw <= 0) return -1;
            w = u ? std::max(w, fw) : w + fw;
        }
        return w;
    }
    if (t.kind == SymbolKind::FixedSizeUnpackedArrayType) {   // a struct member that is an array: its elements, left one highest
        const auto& a = t.as<FixedSizeUnpackedArrayType>();
        int ew = uWidth(a.elementType);
        if (ew <= 0 || (int64_t)ew * (int64_t)a.range.width() > (1 << 24)) return -1;
        return ew * (int)a.range.width();
    }
    if (t.isUnpackedArray() || t.isEvent() || t.isVirtualInterface()) return -1;
    return storeW(t);   // a class handle or chandle member is a 64-bit handle
}
// The bits a value of type t takes inside an unpacked struct or array (uWidth),
// or its storage width (storeW).
inline int bitsOf(const slang::ast::Type& t) {
    return t.getCanonicalType().kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType ? uWidth(t) : storeW(t);
}
// Offset and width of field f inside unpacked struct or union t.
inline std::pair<int, int> uField(const slang::ast::Type& t0, const slang::ast::FieldSymbol& f) {
    using namespace slang::ast;
    const auto& t = t0.getCanonicalType();
    int fw = uWidth(f.getType());
    if (t.isUnpackedUnion()) return {0, fw};
    int after = 0;
    bool seen = false;
    for (const auto& g : t.as<Scope>().membersOfType<FieldSymbol>()) {
        if (seen) after += uWidth(g.getType());
        if (&g == &f) seen = true;
    }
    return {after, fw};
}
inline Val structDefaultVal(const slang::ast::Type& t0);
// A class handle (8), chandle, or null: a 64-bit signal holding an object's
// first signal id (0 is null).
inline bool isHandleType(const slang::ast::Type& t) {
    return t.isClass() || t.isNull() || t.isCHandle() || t.isVirtualInterface();   // a virtual interface is an instance number (25.9)
}
// Virtual interfaces (25.9): interface instances are numbered from 1 as
// they are referenced, and each member reached through a virtual interface
// gets a column of vifTab() (filled in once every signal exists).
struct VifInfo {
    std::unordered_map<const slang::ast::InstanceSymbol*, int> inst;
    std::vector<const slang::ast::InstanceSymbol*> insts{nullptr};
    std::unordered_map<const slang::ast::Symbol*, int> member;
    std::vector<const slang::ast::Symbol*> members;
};
inline VifInfo& vifInfo() { static VifInfo v; return v; }
inline int vifInstance(const slang::ast::InstanceSymbol& i) {
    auto& v = vifInfo();
    auto [it, fresh] = v.inst.emplace(&i, (int)v.insts.size());
    if (fresh) v.insts.push_back(&i);
    return it->second;
}
inline int vifMember(const slang::ast::Symbol& m) {
    auto& v = vifInfo();
    auto [it, fresh] = v.member.emplace(&m, (int)v.members.size());
    if (fresh) v.members.push_back(&m);
    return it->second;
}
// Class methods are always automatic (8.6); other subroutines when declared
// so or inside an automatic module, program or interface (13.3.2).
inline bool isClassMethod(const slang::ast::SubroutineSymbol& s) {
    const slang::ast::Scope* p = s.getParentScope();
    return p && p->asSymbol().kind == slang::ast::SymbolKind::ClassType;
}
inline bool isAutomatic(const slang::ast::SubroutineSymbol& s) {
    return s.defaultLifetime == slang::ast::VariableLifetime::Automatic || isClassMethod(s);
}
inline uint64_t pow10u(int n) { uint64_t r = 1; while (n-- > 0) r *= 10; return r; }

// name, width, and whether the signal is a net. A net starts at Z.
using MkSig = std::function<int(const std::string&, int, bool)>;

// Protected code (34): when set, signals whose declaration this accepts are
// listed in hiddenSigs() so the caller can keep them out of the waveform.
inline std::function<bool(const slang::ast::Symbol&)>& hideSigIf() {
    static std::function<bool(const slang::ast::Symbol&)> f;
    return f;
}
inline std::vector<int>& hiddenSigs() { static std::vector<int> v; return v; }
// The automatic variables declared by for statements (12.7.1) in a static
// process: local to their loop, so msim may unroll the loop (unrollSeqLoops).
inline std::vector<int>& loopVarSigs() { static std::vector<int> v; return v; }
// Per-instance coverage: the g_covInsts id of an instance, named by its path
// below the top ("" = a top).
inline uint32_t covInstIdOf(const slang::ast::InstanceSymbol& inst) {
    std::string p = inst.getHierarchicalPath();
    size_t dot = p.find('.');
    return covInstId(dot == std::string::npos ? std::string() : p.substr(dot + 1), std::string(inst.getDefinition().name));
}
// Protected code whose lines may not show in coverage (-cov): statements
// there get no line, so no line or branch coverage is recorded for them.
inline std::function<bool(slang::SourceLocation)>& hideLocIf() {
    static std::function<bool(slang::SourceLocation)> f;
    return f;
}
// Protected code the tool's own messages may not describe (IEEE 1735 9.2.1):
// a default assertion failure there names neither the line nor the label.
inline std::function<bool(slang::SourceLocation)>& hideMsgIf() {
    static std::function<bool(slang::SourceLocation)> f;
    return f;
}
// Is this instance inside protected IP (IEEE 1735 9.2)? True when its own
// definition, or the definition of an instance above it, is protected code
// the rights do not make visible: a path into or through protected IP is
// opaque.
inline const slang::ast::InstanceSymbol* instanceAround(const slang::ast::Scope* sc) {
    for (; sc; sc = sc->asSymbol().getParentScope()) {
        const slang::ast::Symbol& sy = sc->asSymbol();
        if (sy.kind == slang::ast::SymbolKind::InstanceBody)
            return sy.as<slang::ast::InstanceBodySymbol>().parentInstance;
    }
    return nullptr;
}
inline bool protectedInstance(const slang::ast::InstanceSymbol* inst) {
    if (!hideMsgIf()) return false;
    for (; inst; inst = instanceAround(inst->getParentScope()))
        if (hideMsgIf()(inst->getDefinition().location)) return true;
    return false;
}
inline void noteHidden(const slang::ast::Symbol& s, int first, int count) {
    if (hideSigIf() && hideSigIf()(s))
        for (int i = 0; i < count; i++) hiddenSigs().push_back(first + i);
}

// scope: the instance the process is in, as a signal name prefix without
// the final '.' ("" = the top). A port connection's process is in the
// parent; port names the child's port it carries ("u_core.out"), with
// portDir 0 input, 1 output, 2 inout (UPF boundaries, upf_rt.inc).
// program: the index of the program instance (24) the process belongs to,
// or -1. A program's initial procedures and continuous assigns run in the
// reactive region set (24.3.1).
// reactive: a process of a checker (17.7.3), which also runs in the
// reactive region set but takes no part in ending the run.
struct LoweredProc {
    Prog prog; bool loops; std::string kind;
    std::string scope, port;
    int portDir = -1;
    int program = -1;
    bool reactive = false;
};

// One module-level task or function. Arguments and the function result are
// signals (the formal and slang's returnValVar), shared by every call.
// `prog` is owned by LowerResult::subs so a Call's pointer stays valid after
// lower() returns.
struct ClassInfo;
struct Subr {
    Prog* prog = nullptr;
    ClassInfo* cls = nullptr;     // a constructor of this class (8.7): base constructor, initializers, body
    FrameLayout* lay = nullptr;   // automatic (13.3.2): arguments, result and locals are frame slots
    const slang::ast::SubroutineSymbol* sym = nullptr;
    int retSig = -1;
    int retW = 1;
    bool retSgn = false;
    bool retDyn = false;          // returns a queue, dynamic or associative array: retSig is its frame slot
    struct Arg {
        int sig = -1;
        slang::ast::ArgumentDirection dir = slang::ast::ArgumentDirection::In;
        int w = 1;
        bool sgn = false;
        // An unpacked array or container argument (7.7), copied element by
        // element (sig is -1).
        const slang::ast::FormalArgumentSymbol* arr = nullptr;
    };
    std::vector<Arg> args;
    std::vector<Subr*> calls;
};

// DPI imports by symbol (35); filled before any process is lowered.
inline std::unordered_map<const slang::ast::SubroutineSymbol*, DpiFunc*>& dpiMap() {
    static std::unordered_map<const slang::ast::SubroutineSymbol*, DpiFunc*> m;
    return m;
}
// The hierarchical name of the instance, generate block or package a
// subroutine is declared in: its DPI scope (35.5.3).
inline std::string dpiScopeName(const slang::ast::Symbol& sub) {
    using namespace slang::ast;
    const Scope* sc = sub.getParentScope();
    if (!sc) return "";
    const Symbol& s = sc->asSymbol();
    if (s.kind == SymbolKind::InstanceBody) {
        const auto* inst = s.as<InstanceBodySymbol>().parentInstance;
        return inst ? inst->getHierarchicalPath() : std::string(s.name);
    }
    return s.getHierarchicalPath();
}
// How a value of type t0 goes to C (35.5.6, H.7). w and sgn describe the
// SV value. An enum is passed as its base type.
inline DpiType dpiType(const slang::ast::Type& t0, int& w, bool& sgn) {
    using namespace slang::ast;
    const Type& t = t0.getCanonicalType();
    if (t.kind == SymbolKind::EnumType) return dpiType(t.as<EnumType>().baseType, w, sgn);
    sgn = t.isSigned();
    if (t.isVoid()) { w = 0; return DT_VOID; }
    if (t.isString()) { w = 64; return DT_STRING; }
    if (t.isCHandle()) { w = 64; return DT_CHANDLE; }
    if (t.isFloating()) {   // a shortreal is held as a double, like a real (6.12)
        bool sr = t.getBitWidth() == 32;
        w = 64;
        return sr ? DT_SHORTREAL : DT_REAL;
    }
    if (!t.isIntegral()) throw std::runtime_error("lower: unsupported DPI argument type " + t0.toString());
    w = (int)t.getBitWidth();
    if (t.kind == SymbolKind::PredefinedIntegerType) {
        switch (t.as<PredefinedIntegerType>().integerKind) {
            case PredefinedIntegerType::Byte: return DT_BYTE;
            case PredefinedIntegerType::ShortInt: return DT_SHORT;
            case PredefinedIntegerType::Int: return DT_INT;
            case PredefinedIntegerType::LongInt: return DT_LONG;
            default: return DT_LOGICVEC;   // integer, time: 4-state packed
        }
    }
    if (t.kind == SymbolKind::ScalarType) return t.isFourState() ? DT_LOGIC : DT_BIT;
    return t.isFourState() ? DT_LOGICVEC : DT_BITVEC;
}
// ---------- classes (8) ----------
// An object is a block of signals laid out like a frame: slot 0 holds its
// class id, then the base class's properties, then its own. A non-static
// property is a slot offset; static ones are ordinary signals ("C::x").
struct ClassInfo {
    const slang::ast::ClassType* ty = nullptr;
    int id = -1;
    ClassInfo* base = nullptr;
    FrameLayout* lay = nullptr;
    Subr* ctor = nullptr;   // the constructor, written or implicit (8.7)
    // A built-in class of the std package and the slots of its hidden
    // state: 1 weak_reference (8.30: the referent), 2 semaphore (15.3: keys,
    // change count), 3 mailbox (15.4: queue, bound, change count),
    // 4 process (9.7: process number).
    int builtin = 0;
    int bslot = -1;
    int bslot2 = -1, bslot3 = -1;
};
inline int builtinClassKind(const slang::ast::ClassType& ct) {
    const slang::ast::Scope* sc = ct.genericClass ? ct.genericClass->getParentScope() : ct.getParentScope();
    if (!sc || sc->asSymbol().kind != slang::ast::SymbolKind::Package || sc->asSymbol().name != "std") return 0;
    std::string_view n = ct.genericClass ? ct.genericClass->name : ct.name;
    if (n == "weak_reference") return 1;
    if (n == "semaphore") return 2;
    if (n == "mailbox") return 3;
    if (n == "process") return 4;
    throw std::runtime_error("lower: unsupported built-in class std::" + std::string(n));
}
inline std::unordered_map<const slang::ast::Type*, ClassInfo*>& classMap() {
    static std::unordered_map<const slang::ast::Type*, ClassInfo*> m;
    return m;
}
inline std::vector<std::unique_ptr<ClassInfo>>& classStore() { static std::vector<std::unique_ptr<ClassInfo>> v; return v; }
// A non-static property's slot: kind 0 a value (or an event: two slots),
// 1 an unpacked array (arr->base is its first slot), 2 a container.
struct PropSlot { int kind = 0; int off = 0; std::shared_ptr<ArrShape> arr; };
inline std::unordered_map<const slang::ast::Symbol*, PropSlot>& propMap() {
    static std::unordered_map<const slang::ast::Symbol*, PropSlot> m;
    return m;
}
// A virtual method's index in the virtual method table (8.20).
inline std::unordered_map<const slang::ast::SubroutineSymbol*, int>& vslotMap() {
    static std::unordered_map<const slang::ast::SubroutineSymbol*, int> m;
    return m;
}
// rand_mode / constraint_mode slots (18.8, 18.9) of rand properties and constraint blocks.
inline std::unordered_map<const slang::ast::Symbol*, int>& randModeMap() {
    static std::unordered_map<const slang::ast::Symbol*, int> m;
    return m;
}
inline std::unordered_map<const slang::ast::Symbol*, int>& consModeMap() {
    static std::unordered_map<const slang::ast::Symbol*, int> m;
    return m;
}
inline std::unordered_map<const slang::ast::Symbol*, int>& staticConsMode() {
    static std::unordered_map<const slang::ast::Symbol*, int> m;
    return m;
}
// An interface class method's global number k (8.26); a call through an
// interface handle has vslot -2 - k.
inline std::unordered_map<const slang::ast::SubroutineSymbol*, int>& ifaceSlotMap() {
    static std::unordered_map<const slang::ast::SubroutineSymbol*, int> m;
    return m;
}
inline ClassInfo* classInfo(const slang::ast::Type& t) {
    auto it = classMap().find(&t.getCanonicalType());
    if (it == classMap().end()) throw std::runtime_error("lower: unsupported class type " + t.toString());
    return it->second;
}

struct LowerResult {
    std::vector<std::unique_ptr<DpiFunc>> dpi;   // DPI imports and exports (35)
    std::vector<std::string> scopes;             // every instance and package, for svGetScopeFromName
    std::vector<std::string> protScopes;         // those of `scopes` inside protected IP (IEEE 1735 9.7.2.3)
    std::unordered_map<const slang::ast::Symbol*, int> sigOf;   // variable / net -> signal (VPI, 37)
    std::vector<LoweredProc> procs;
    std::vector<std::unique_ptr<Prog>> subs;
    std::vector<LoweredProc> finals;  // final blocks (9.2.3), run once at the end
    int programs = 0;                 // program instances (24.3)
    LayoutList layouts;   // frame shapes of automatic subroutines and fork branches
    std::vector<std::pair<int, std::vector<uint64_t>>> netDelays;   // wire #d w (28.16): signal, ticks
    // trireg nets with a charge strength other than medium or a charge decay
    // time (28.16.2, Annex E): signal, strength level, decay ticks, has a decay.
    struct Trireg { int sig; uint8_t charge; uint64_t decay; bool decays; };
    std::vector<Trireg> trireg;
    std::vector<std::pair<int, uint8_t>> netKinds;                   // wand, wor, tri0, ... (6.6): signal, NetResolve
    // Free checker variables (17.7) with no initializer: each starts random;
    // an active one (never assigned, not const) takes a new random value
    // in every time step (17.7.2). Signal, every step.
    std::vector<std::pair<int, bool>> freeVars;
    std::vector<std::unique_ptr<UdpTable>> udps;   // tables of the user-defined primitives (29)
    struct TranDecl { int a, b; bool resist; int aLo = 0, bLo = 0, w = -1; };
    std::vector<TranDecl> trans;      // tran / tranif switches (28.8): the two nets, resistive; w >= 0: only bits [aLo, aLo+w) of a and [bLo, bLo+w) of b
    int precExp = -9;                 // one kernel tick = 10^precExp s
    // The tick as a VCD $timescale ("1ns", "100ps").
    std::string timescale() const {
        static const char* u[] = {"s", "ms", "us", "ns", "ps", "fs"};
        int e = precExp, m = 1;
        while (e % 3) { e--; m *= 10; }  // e.g. -10 = 100ps
        int k = -e / 3;
        if (k < 0) k = 0;
        if (k > 5) k = 5;
        return std::to_string(m) + u[k];
    }
};

// A parent asked for a width. fill sign-extends when widening. tag is the
// signedness after the resize. Propagated ops set fill == tag. An assignment
// sets fill from the expression and tag from the destination type.
struct ExprCtx {
    int w = -1;
    bool fill = false;
    bool tag = false;
    bool live = false;
};

struct Lowerer {
    std::unordered_map<const slang::ast::Symbol*, int>& sigmap;
    Prog prog;
    std::vector<size_t> implicitWaits;  // WaitEdge slots for @*, filled by finishImplicit()
    // Set while lowering a compound assignment. slang's right-hand side is the
    // binary operator, and its left operand is an LValueReference: the target's
    // type, and no symbol. The read is the assignment's left expression.
    const slang::ast::Expression* lvalue = nullptr;
    // Innermost loop is back(). break and continue emit Jmp; the pc is filled
    // in when that loop's end (break) or continuation (continue) is known.
    struct LoopJumps {
        std::vector<size_t> breaks;
        std::vector<size_t> continues;
        int xpBrk = -1, xpCnt = -1;   // -xprop: break / continue set these flags instead of jumping
    };
    std::vector<LoopJumps> loops;
    // Named sequential blocks around the statement being lowered, each with
    // the jumps a `disable` of it makes to its end.
    std::vector<std::pair<const slang::ast::Symbol*, std::vector<size_t>>> named;
    // Named blocks being lowered that have a fork in them (9.6.2:
    // disabling one ends what it forked).
    std::unordered_set<const slang::ast::Symbol*> forkyBlocks;
    static bool hasFork(const slang::ast::Statement& st) {
        using namespace slang::ast;
        struct F : ASTVisitor<F, VisitFlags::Statements> {
            bool any = false;
            void handle(const BlockStatement& b) {
                if (b.blockKind != StatementBlockKind::Sequential) any = true;
                visitDefault(b);
            }
        } f;
        st.visit(f);
        return f.any;
    }
    // A statement that is one named begin...end block.
    static const slang::ast::Symbol* namedSeq(const slang::ast::Statement& st) {
        using namespace slang::ast;
        if (st.kind != StatementKind::Block) return nullptr;
        const auto& b = st.as<BlockStatement>();
        if (b.blockKind != StatementBlockKind::Sequential || !b.blockSymbol || b.blockSymbol->name.empty()) return nullptr;
        return b.blockSymbol;
    }
    static int rootId(const slang::ast::Symbol* s) {
        int id = blockIds().of(s);
        blockIds().roots.insert(id);
        return id;
    }
    // The event signal an event expression names (e, sub.e).
    int eventSig(const slang::ast::Expression& x) {
        using namespace slang::ast;
        if (vifAccess(x)) {   // v.e, v.x (25.9): an event is two signals
            const auto& m = x.as<MemberAccessExpression>();
            bool two = m.member.kind == slang::ast::SymbolKind::ClockingBlock ||
                       (m.member.isValue() && m.member.as<slang::ast::ValueSymbol>().getType().isEvent());
            return vifSlot(m, two ? 2 : 1);
        }
        if (classMember(x)) {   // h.e: its two slots (count, last trigger) through the handle
            const auto& m = x.as<MemberAccessExpression>();
            const PropSlot* ps = memberSlot(m);
            if (!ps) return sigAt(&m.member);
            return indSlot(exprH(m.value()), ps->off, 2);
        }
        if (!ValueExpressionBase::isKind(x.kind)) unsupported("event expression");
        const Symbol* sym = &x.as<ValueExpressionBase>().symbol;
        if (auto p = eventPtr().find(sym); p != eventPtr().end()) {   // maybe an alias (6.17): through its slot
            int first = -1;
            for (int k = 0; k < 2; k++) {
                int tmp = fresh(64);
                Instr i; i.k = IK::Assign; i.sig = tmp;
                i.e = k ? bin(Op::Add, ref(p->second), konst(mk(1, 64))) : ref(p->second);
                prog.emit(i);
                int id = indThrough(tmp);
                if (!k) first = id;
            }
            return first;
        }
        int s = sigFind(sym);
        if (s < 0) unsupported("event expression");
        return s;
    }
    // The (signal id + 1) of the event an expression names now: e, an alias, or null.
    E eventAddr(const slang::ast::Expression& x0) {
        using namespace slang::ast;
        const Expression& x = peel(x0);
        if (x.kind == ExpressionKind::NullLiteral) {   // a fresh event that is never triggered
            int c = fresh(32);
            if (fresh(64) != c + 1) unsupported("event signals that are not consecutive");
            auto n = node(Op::AddrOf); n->sig = c;
            return n;
        }
        if (ValueExpressionBase::isKind(x.kind))
            if (auto p = eventPtr().find(&x.as<ValueExpressionBase>().symbol); p != eventPtr().end())
                return ref(p->second);
        if (classMember(x)) unsupported("assignment of an event property");
        auto n = node(Op::AddrOf); n->sig = eventSig(x);
        return n;
    }
    std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* subs = nullptr;
    MkSig mkSig;
    int* tempSeq = nullptr;
    // The enclosing module's time unit and precision (powers of ten).
    int tsUnit = -9, tsPrec = -9;
    void setScope(const slang::ast::Scope* sc) {
        if (!sc) return;
        scope = sc;
        if (auto ts = sc->getTimeScale()) { tsUnit = tsExp(ts->base); tsPrec = tsExp(ts->precision); }
    }
    uint64_t ticksPerUnit() const { return pow10u(tsUnit - simPrecExp()); }
    // A delay in this module's units, as kernel ticks: rounded to the module's
    // precision first (22.7), then scaled to the simulation precision.
    uint64_t ticksOf(double units) const {
        double inPrec = units * (double)pow10u(tsUnit - tsPrec);
        double r = inPrec < 0 ? 0 : std::floor(inPrec + 0.5);
        return (uint64_t)r * pow10u(tsPrec - simPrecExp());
    }
    // #d: a constant becomes Instr::delay; anything else is an expression
    // evaluated when the statement runs (9.4.1). x or z waits zero time.
    // The rise / fall / turn-off delays of a continuous assign, gate or net
    // (10.3.3, 28.16), in ticks. They must be constant.
    std::vector<uint64_t> delays3(const slang::ast::TimingControl* tc) {
        using namespace slang::ast;
        std::vector<uint64_t> out;
        if (!tc) return out;
        auto one = [&](const Expression& e) {
            Instr t;
            delayInto(e, t);
            if (t.de) unsupported("a delay on a continuous assignment, gate or net that is not constant");
            out.push_back(t.delay);
        };
        if (tc->kind == TimingControlKind::Delay) one(tc->as<DelayControl>().expr);
        else if (tc->kind == TimingControlKind::Delay3) {
            const auto& d3 = tc->as<Delay3Control>();
            one(d3.expr1);
            if (d3.expr2) one(*d3.expr2);
            if (d3.expr3) one(*d3.expr3);
        } else unsupported("this kind of delay");
        // Annex E: `delay_mode_zero drops the delay, `delay_mode_unit makes
        // each nonzero one a unit of the module's precision, and
        // `delay_mode_path leaves only the module path delays.
        auto mode = delayDirectives(scope).mode;
        if (mode == DelayDirectives::ZERO || mode == DelayDirectives::PATH) out.clear();
        else if (mode == DelayDirectives::UNIT)
            for (auto& v : out) if (v) v = pow10u(tsPrec - simPrecExp());
        bool any = false;
        for (auto v : out) any |= v != 0;
        if (!any) out.clear();   // #0 is no delay
        return out;
    }
    // $sdf_annotate ( sdf_file [, module_instance [, config_file [, log_file
    // [, mtm_spec [, scale_factors [, scale_type ]]]]]] ) (32.9): each
    // argument's text, and whether it is known before the run (a literal,
    // the instance, or left out). The scope defaults to the caller's instance.
    static void sdfArgs(const slang::ast::CallExpression& call, std::string (&fields)[7], bool (&known)[7]) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (args.empty() || args.size() > 7) unsupported("$sdf_annotate arguments");
        const auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine);
        fields[1] = sci && sci->scope ? instPath(sci->scope.get()) : std::string("top");
        for (size_t k = 0; k < 7; k++) {
            known[k] = true;
            if (k >= args.size() || peel(*args[k]).kind == ExpressionKind::EmptyArgument) continue;
            const Expression& a = peel(*args[k]);
            if (k == 1) {
                const Symbol* sym = a.kind == ExpressionKind::ArbitrarySymbol ? a.as<ArbitrarySymbolExpression>().symbol.get() : nullptr;
                if (!sym || sym->kind != SymbolKind::Instance) unsupported("a $sdf_annotate scope that is not an instance");
                fields[1] = sym->getHierarchicalPath();
            } else if (a.kind == ExpressionKind::StringLiteral) fields[k] = std::string(a.as<StringLiteral>().getValue());
            else known[k] = false;
        }
    }
    static const slang::ast::Expression& mtmOf(const slang::ast::MinTypMaxExpression& m) {
        return mtmSelect() == 0 ? m.min() : mtmSelect() == 2 ? m.max() : m.typ();
    }
    // Does e call a task or function of the design (not a system one)?
    static bool hasUserCall(const slang::ast::Expression& e) {
        using namespace slang::ast;
        struct V : ASTVisitor<V, VisitFlags::Expressions> {
            bool hit = false;
            void handle(const CallExpression& c) {
                if (!c.isSystemCall()) hit = true;
                visitDefault(c);
            }
        } v;
        e.visit(v);
        return v.hit;
    }
    // Does e read a specparam that an SDF LABEL changed?
    static bool readsLabel(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (labelled().empty()) return false;
        struct V : ASTVisitor<V, VisitFlags::Expressions> {
            bool hit = false;
            void handle(const NamedValueExpression& n) { hit |= labelled().count(&n.symbol) != 0; }
        } v;
        e.visit(v);
        return v.hit;
    }
    // A constant expression that reads a LABEL'd specparam, computed with
    // its new value (32.4.3).
    std::optional<double> labelFold(const slang::ast::Expression& e) {
        if (!readsLabel(e)) return std::nullopt;
        E v = realOf(e);
        Val r = eval(v, [](int) -> Val { unsupported("a specparam expression that reads a signal"); });
        return v_real(r);
    }
    void delayInto(const slang::ast::Expression& d, Instr& i) {
        using namespace slang::ast;
        if (peel(d).kind == ExpressionKind::MinTypMax) {   // #(1:2:3): the typical value unless +mindelays / +maxdelays (11.11)
            delayInto(mtmOf(peel(d).as<MinTypMaxExpression>()), i);
            return;
        }
        if (auto lv = labelFold(d)) { i.delay = ticksOf(*lv); return; }
        const Expression& x = peel(d);
        if (x.kind == ExpressionKind::RealLiteral) { i.delay = ticksOf(x.as<RealLiteral>().getValue()); return; }
        if (x.kind == ExpressionKind::TimeLiteral) { i.delay = ticksOf(x.as<TimeLiteral>().getValue()); return; }
        if (const auto* cv = d.getConstant()) {
            if (cv->isReal()) { i.delay = ticksOf(cv->real()); return; }
            if (cv->isInteger() && !cv->integer().hasUnknown()) {
                auto v = cv->integer().as<int64_t>();
                if (v) { i.delay = ticksOf((double)*v); return; }
            }
        }
        if (x.kind == ExpressionKind::IntegerLiteral && !x.as<IntegerLiteral>().getValue().hasUnknown()) {
            i.delay = ticksOf((double)*x.as<IntegerLiteral>().getValue().as<uint64_t>());
            return;
        }
        if (d.type->isFloating()) {   // #(hb) with a real variable: scaled to ticks, rounded (10.3.3, 11.11)
            // rounded to the module's precision first, as a constant delay is
            E inPrec = rtoi(bin(Op::FMul, realOf(d), realK((double)pow10u(tsUnit - tsPrec))), 64, false, false);
            i.de = bin(Op::Mul, inPrec, konst(mk(pow10u(tsPrec - simPrecExp()), 64)));
            return;
        }
        if (!d.type->isIntegral()) unsupported("non-integral delay expression");
        E v = resize(expr(d), 64, false, false);
        i.de = bin(Op::Mul, v, konst(mk(ticksPerUnit(), 64)));
    }
    int retSig = -1;     // function result signal while lowering a function body
    Subr* current = nullptr;
    // Set while lowering an output/inout write-back. slang's actual is an
    // assignment whose right-hand side is an EmptyArgument (the formal),
    // possibly wrapped in a conversion to the actual's type.
    struct OutSlot { int sig; int w; bool sgn; };
    const OutSlot* outSlot = nullptr;

    explicit Lowerer(std::unordered_map<const slang::ast::Symbol*, int>& m) : sigmap(m) {}

    // ---------- frames (automatic variables) ----------
    // The frame shapes around the code being lowered, outermost first: an
    // automatic subroutine's, then a fork block's, then a branch's. Empty in
    // static code, where every variable is a signal.
    std::vector<FrameLayout*> frames;
    ClassInfo* curClass = nullptr;   // the class whose method is being lowered
    E defaultSelf;                   // while lowering a method's default argument: the object it is called on
    // Containers reached through a handle (indirect ids): their shapes.
    mutable std::unordered_map<int, DynObj> indShape;
    static bool isInd(int s) { return isRel(s) && relMode(s) >= kRelIndSig; }
    // The indirect id through the slot `slot` names (a static signal or a frame slot).
    static int indThrough(int slot) {
        if (!isRel(slot)) return relSig(kRelIndSig, slot);
        if (relMode(slot) > 4) unsupported("reference through a slot more than 4 frames up");
        return relSig(kRelIndFrame + relMode(slot), relOff(slot));
    }
    int thisRel(int off) const {
        if (!curClass) unsupported("class property outside a method of its class");
        return relSig(kRelThis, off);
    }
    int relIn(const FrameLayout* L, int off) const {
        for (size_t k = frames.size(); k-- > 0;)
            if (frames[k] == L) {
                int depth = (int)(frames.size() - 1 - k);
                if (depth >= kRelThis) unsupported("fork nested more than 7 deep in an automatic context");
                return relSig(depth, off);
            }
        unsupported("automatic variable used outside its frame");
    }
    FrameLayout* layOf(int rel) const {
        int depth = relMode(rel);
        if (depth >= (int)frames.size()) unsupported("automatic variable outside a frame");
        return frames[frames.size() - 1 - depth];
    }
    // The slot that holds a ref formal's address (a frame slot or a signal).
    int refSlot(const slang::ast::Symbol* sym) const {
        if (auto it = localMap().find(sym); it != localMap().end()) return relIn(it->second.lay, it->second.off);
        auto it = sigmap.find(sym);
        if (it == sigmap.end()) unsupported("ref argument with no slot");
        return it->second;
    }
    int sigFind(const slang::ast::Symbol* sym) const {
        if (auto ov = symOverride.find(sym); ov != symOverride.end()) return ov->second;
        if (cgItemSig >= 0 && sym->kind == slang::ast::SymbolKind::Iterator && sym->name == "item") return cgItemSig;
        if (!cgIterSigs.empty() && sym->kind == slang::ast::SymbolKind::Iterator)   // a cross with filter's coverpoint (19.6.1.2)
            if (auto it = cgIterSigs.find(std::string(sym->name)); it != cgIterSigs.end()) return it->second;
        if (auto rf = refFormals().find(sym); rf != refFormals().end())
            return rf->second.kind == 0 ? indThrough(refSlot(sym)) : -1;
        if (auto it = localMap().find(sym); it != localMap().end()) return relIn(it->second.lay, it->second.off);
        if (auto it = propMap().find(sym); it != propMap().end() && it->second.kind == 0) return thisRel(it->second.off);
        auto it = sigmap.find(sym);
        return it == sigmap.end() ? -1 : it->second;
    }
    int sigAt(const slang::ast::Symbol* sym) const {
        int s = sigFind(sym);
        if (s < 0) unsupported("reference to a variable with no signal");
        return s;
    }
    std::shared_ptr<const ArrShape> arrFind(const slang::ast::Symbol* sym) const {
        if (auto rf = refFormals().find(sym); rf != refFormals().end()) {
            if (rf->second.kind != 1) return nullptr;
            auto a = std::make_shared<ArrShape>(*rf->second.arr);
            a->base = indThrough(refSlot(sym));
            return a;
        }
        if (auto it = propMap().find(sym); it != propMap().end() && it->second.kind == 1) {
            auto a = std::make_shared<ArrShape>(*it->second.arr);
            a->base = thisRel(it->second.arr->base);
            return a;
        }
        if (auto it = localArr().find(sym); it != localArr().end()) {
            auto a = std::make_shared<ArrShape>(*it->second.second);
            a->base = relIn(it->second.first, it->second.second->base);
            return a;
        }
        auto it = arrMap().find(sym);
        return it == arrMap().end() ? nullptr : it->second;
    }
    int dynFind(const slang::ast::Symbol* sym) const {
        if (auto rf = refFormals().find(sym); rf != refFormals().end()) {
            if (rf->second.kind != 2) return -1;
            int d = indThrough(refSlot(sym));
            indShape[d] = rf->second.dyn;
            return d;
        }
        if (auto it = propMap().find(sym); it != propMap().end() && it->second.kind == 2) return thisRel(it->second.off);
        if (auto it = localDyn().find(sym); it != localDyn().end()) return relIn(it->second.lay, it->second.off);
        auto it = dynMap().find(sym);
        return it == dynMap().end() ? -1 : it->second;
    }
    // The empty shape of container d (a signal, or a frame slot).
    const DynObj& dynShape(int d) const {
        if (!isRel(d)) return dynStore()[d];
        if (isInd(d)) {
            auto it = indShape.find(d);
            if (it == indShape.end()) unsupported("container through a handle");
            return it->second;
        }
        const FrameLayout* L = relMode(d) == kRelThis ? (curClass ? curClass->lay : nullptr) : layOf(d);
        if (!L) unsupported("container slot");
        for (auto& [off, o] : L->dyn)
            if (off == relOff(d)) return o;
        unsupported("container slot");
    }
    // A variable as a frame slot, by its type (6.21): 4-state starts X,
    // 2-state 0, string "", real 0.0, a container empty.
    static Val initOf(const slang::ast::Type& t0) {
        const auto& t = t0.getCanonicalType();
        if (t.isFloating()) return mkreal(0);
        if (t.isString()) return mkstr("");
        int w = storeW(t);
        if (w <= 0) unsupported("automatic variable of this type");
        if (!t.isFourState()) return make(0, 0, w);
        return w > 64 ? allx(w) : Val{};
    }
    static DynObj dynShapeOf(const slang::ast::Type& t0);
    static void elemInto(DynObj& o, const slang::ast::Type& et);
    // Slots for a variable of type t in frame (or object) shape L.
    static PropSlot declSlot(const slang::ast::Type& t0, FrameLayout* L) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        PropSlot ps;
        if (isDynType(t)) {
            ps.kind = 2;
            ps.off = L->add(64, make(0, 0, 64));
            L->dyn.push_back({ps.off, dynShapeOf(t)});
            return ps;
        }
        if (t.isEvent()) {
            ps.off = L->add(32, make(0, 0, 32));
            L->add(64, make(0, 0, 64));
            return ps;
        }
        if (t.isUnpackedArray()) {
            auto shape = std::make_shared<ArrShape>();
            const Type* et = &t;
            int64_t total = 1;
            while (et->isUnpackedArray() && !isDynType(*et)) {
                if (et->kind != SymbolKind::FixedSizeUnpackedArrayType) unsupported("automatic array of this kind");
                const auto& ft = et->as<FixedSizeUnpackedArrayType>();
                shape->dims.push_back({ft.range.left, ft.range.right});
                total *= (int64_t)ft.range.width();
                if (total > (1 << 16)) unsupported("automatic array with more than 65536 elements");
                et = &ft.elementType.getCanonicalType();
            }
            if (isDynType(*et)) {   // int f[2][$] (7.4): one container slot per element
                shape->count = (int)total;
                shape->elemW = 64;
                shape->dynElem = std::make_shared<DynObj>(dynShapeOf(*et));
                for (int64_t k = 0; k < total; k++) {
                    int off = L->add(64, make(0, 0, 64));
                    if (!k) shape->base = off;
                    else if (off != shape->base + (int)k) unsupported("array of containers not laid out in a row");
                    L->dyn.push_back({off, *shape->dynElem});
                }
                ps.kind = 1;
                ps.arr = shape;
                return ps;
            }
            Val iv = initOf(*et);
            shape->count = (int)total;
            shape->elemW = storeW(*et);
            shape->sgn = et->isSigned();
            shape->twoState = !et->isFourState();
            for (int64_t k = 0; k < total; k++) {
                int off = L->add(shape->elemW, iv);
                if (!k) shape->base = off;
            }
            ps.kind = 1;
            ps.arr = shape;
            return ps;
        }
        if (t.isUnpackedStruct() || t.isUnpackedUnion()) {   // one vector, as for a static one (7.2)
            int w = uWidth(t);
            if (w <= 0) unsupported("unpacked struct with an array or event member as an automatic variable or class property");
            ps.off = L->add(w, structDefaultVal(t));
            return ps;
        }
        ps.off = L->add(storeW(t), initOf(t));
        return ps;
    }
    void declLocal(const slang::ast::VariableSymbol& v, FrameLayout* L) {
        if (localMap().count(&v) || localArr().count(&v) || localDyn().count(&v)) return;
        PropSlot ps = declSlot(v.getType(), L);
        if (ps.kind == 2) localDyn()[&v] = {L, ps.off};
        else if (ps.kind == 1) localArr()[&v] = {L, ps.arr};
        else localMap()[&v] = {L, ps.off};
    }
    // Every automatic variable that `st` declares, outside nested fork
    // branches (which get their own frames), as a slot of frame L.
    void declLocals(const slang::ast::Statement& st, FrameLayout* L);
    // A loop variable made by the lowerer (foreach): a static signal, or in
    // a frame a slot of the innermost frame.
    void bindVar(const slang::ast::Symbol* sym, int sig) {
        if (isRel(sig)) localMap()[sym] = {frames.back(), relOff(sig)};
        else sigmap[sym] = sig;
    }

#include "lower_assert.inc"
#include "lower_clocking.inc"
#include "lower_cover.inc"
#include "lower_xprop.inc"

    // Owns the Prog of each fork branch, so Fork's pointers outlive lower().
    std::vector<std::unique_ptr<Prog>>* store = nullptr;
    void bind(std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* s,
              MkSig mk, int* seq, std::vector<std::unique_ptr<Prog>>* st = nullptr) {
        subs = s;
        mkSig = std::move(mk);
        tempSeq = seq;
        store = st;
    }

    int freshStatic(int w) {
        if (!mkSig) unsupported("temporary");
        int n = tempSeq ? (*tempSeq)++ : 0;
        return mkSig("_tf" + std::to_string(n), w, false);
    }
    int fresh(int w) {
        if (!frames.empty()) return relSig(0, frames.back()->add(w, w > 64 ? allx(w) : Val{}));
        if (!mkSig) unsupported("temporary");
        int n = tempSeq ? (*tempSeq)++ : 0;
        return mkSig("_tf" + std::to_string(n), w, false);
    }

    // The statement or construct being lowered, so an error can say where.
    static slang::SourceLocation& lastLoc() { static slang::SourceLocation l; return l; }
    static std::string whereStr() {
        const auto* sm = srcMgr();
        slang::SourceLocation l = lastLoc();
        if (!sm || !l.valid()) return "";
        auto ol = sm->getFullyOriginalLoc(l);
        return std::string(sm->getFileName(ol)) + ":" + std::to_string(sm->getLineNumber(ol)) + ": ";
    }
    // Subroutines not lowered, with the reason a run-time call reports.
    static std::unordered_map<const slang::ast::SubroutineSymbol*, std::string>& skippedSubs() {
        static std::unordered_map<const slang::ast::SubroutineSymbol*, std::string> m;
        return m;
    }
    // Makes a signal driven by an event expression; set while lowering a design.
    static std::function<int(const slang::ast::Expression&)>& eventExprHook() {
        static std::function<int(const slang::ast::Expression&)> h;
        return h;
    }
    [[noreturn]] static void unsupported(const char* what) {
        throw std::runtime_error(whereStr() + "lower: unsupported " + what);
    }

    size_t emitJmpIfNot(E c) {
        Instr j;
        j.k = IK::JmpIfNot;
        j.e = std::move(c);
        return prog.emit(j);
    }
    // A random number in [0, total) for weighted choices (18.16, 18.17.1): two
    // $urandom draws, so the 64-bit remainder is close to uniform.
    E weightedPick(E total) {
        E hi = resize(sysFnNode(SF_URANDOM, {}), 64, false, false);
        E lo = resize(sysFnNode(SF_URANDOM, {}), 64, false, false);
        E r = bin(Op::Or, bin(Op::Shl, hi, konst(mk(32, 32))), lo);
        return bin(Op::Mod, r, total);
    }
    // randcase (18.16): weights summed, one draw, the branch whose share
    // holds it. All weights zero: no branch.
    void randCase(const slang::ast::RandCaseStatement& rc) {
        std::vector<int> w;
        E total;
        for (const auto& it : rc.items) {
            int t = fresh(64);
            Instr i; i.k = IK::Assign; i.sig = t;
            i.e = resize(expr(*it.expr), 64, it.expr->type->isSigned(), false);
            prog.emit(i);
            w.push_back(t);
            total = total ? bin(Op::Add, total, ref(t)) : ref(t);
        }
        if (!total) return;
        int r = fresh(64);
        Instr d; d.k = IK::Assign; d.sig = r; d.e = weightedPick(total);
        prog.emit(d);
        std::vector<size_t> ends;
        for (size_t k = 0; k < rc.items.size(); k++) {
            size_t skip = emitJmpIfNot(bin(Op::Lt, ref(r), ref(w[k])));
            stmt(*rc.items[k].stmt);
            ends.push_back(emitJmp());
            prog.code[skip].target = prog.here();
            Instr sub; sub.k = IK::Assign; sub.sig = r; sub.e = bin(Op::Sub, ref(r), ref(w[k]));
            prog.emit(sub);
        }
        patch(ends, prog.here());
    }
    // randsequence (18.17): each production is a subroutine with no frame, so
    // productions can recurse; return in a code block ends its production,
    // break ends the whole sequence (a flag every caller checks).
    int rsBreak = -1;
    std::unordered_map<const slang::ast::RandSeqProductionSymbol*, Prog*> rsProds;
    // Arguments and values (18.17.7): each production's arguments and return
    // value live in signals of their own; each use of a production with a
    // value keeps it in a temporary. A production saves its own signals on a
    // stack around every call it makes, so recursion keeps them.
    struct RsSig { std::vector<int> args; int ret = -1; };
    std::unordered_map<const slang::ast::RandSeqProductionSymbol*, RsSig> rsSigs;
    std::unordered_map<const void*, int> rsItemVal;   // a ProdItem -> its value's temporary
    std::unordered_map<const slang::ast::Symbol*, std::vector<int>> rsVals;   // slang's variable for a production's values, in the rule being lowered
    std::vector<int> rsState;   // the signals the production being lowered saves around calls
    int rsStack = -1;
    void randSequence(const slang::ast::RandSequenceStatement& rs) {
        using namespace slang::ast;
        if (!rs.firstProduction) return;
        if (!store) unsupported("randsequence here");
        int savedBreak = rsBreak;
        auto savedProds = rsProds;
        rsBreak = fresh(1);
        auto savedSigs = rsSigs;
        int savedStack = rsStack;
        bool any = false;
        for (const RandSeqProductionSymbol* p : rs.productions) {
            store->push_back(std::make_unique<Prog>());
            rsProds[p] = store->back().get();
            RsSig& rsg = rsSigs[p];
            for (const FormalArgumentSymbol* a : p->arguments) {
                if (a->direction != ArgumentDirection::In) unsupported("randsequence production argument that is not input");
                int w = storeW(a->getType());
                needWidth(w);
                int sg = fresh(w);
                rsg.args.push_back(sg);
                symOverride[a] = sg;
                any = true;
            }
            if (!p->getReturnType().isVoid()) {
                int w = storeW(p->getReturnType());
                needWidth(w);
                rsg.ret = fresh(w);
                any = true;
            }
        }
        if (any) { DynObj st; st.kind = 1; rsStack = dynTemp(std::move(st)); }
        for (const RandSeqProductionSymbol* p : rs.productions) {
            Prog body;
            std::swap(prog, body);
            auto savedLoops = std::move(loops);
            loops.clear();
            auto savedNamed = std::move(named);
            named.clear();
            int savedRet = retSig;
            retSig = rsSigs[p].ret;   // return in a code block: this production's value
            auto savedState = std::move(rsState);
            rsState = rsSigs[p].args;
            rsProduction(*p);
            rsState = std::move(savedState);
            retSig = savedRet;
            loops = std::move(savedLoops);
            named = std::move(savedNamed);
            std::swap(prog, body);
            *rsProds[p] = std::move(body);
        }
        Instr clr; clr.k = IK::Assign; clr.sig = rsBreak; clr.e = konst(mk(0, 1));
        prog.emit(clr);
        rsCall(rs.firstProduction, false);
        rsBreak = savedBreak;
        rsProds = std::move(savedProds);
        rsSigs = std::move(savedSigs);
        rsStack = savedStack;
    }
    // Call production p; inside a production, return at once after a break.
    void rsCall(const slang::ast::RandSeqProductionSymbol* p, bool inProd,
                const slang::ast::RandSeqProductionSymbol::ProdItem* item = nullptr) {
        const RsSig& callee = rsSigs[p];
        // The arguments, all evaluated before any is set (the callee may be this production).
        std::vector<int> av;
        if (item) {
            if (item->args.size() > callee.args.size()) unsupported("randsequence production argument count");
            for (size_t k = 0; k < item->args.size(); k++) {
                int t = fresh(storeW(*item->args[k]->type) > 0 ? storeW(*item->args[k]->type) : 1);
                Instr i; i.k = IK::Assign; i.sig = t; i.e = expr(*item->args[k]);
                prog.emit(i);
                av.push_back(t);
            }
        }
        bool save = inProd && !rsState.empty();
        if (save)
            for (int sg : rsState) { Instr i = dynCmd(rsStack, D_PUSH_BACK); i.e = ref(sg); prog.emit(i); }
        for (size_t k = 0; k < av.size(); k++) {
            Instr i; i.k = IK::Assign; i.sig = callee.args[k]; i.e = ref(av[k]);
            prog.emit(i);
        }
        Instr c; c.k = IK::Call; c.callee = rsProds.at(p);
        prog.emit(c);
        int got = -1;
        if (callee.ret >= 0 && item && rsItemVal.count(item)) {
            got = fresh(storeW(p->getReturnType()));
            Instr i; i.k = IK::Assign; i.sig = got; i.e = ref(callee.ret);
            prog.emit(i);
        }
        if (save)
            for (size_t k = rsState.size(); k-- > 0;) { Instr i = dynCmd(rsStack, D_POP_BACK); i.dres = rsState[k]; prog.emit(i); }
        if (got >= 0) { Instr i; i.k = IK::Assign; i.sig = rsItemVal.at(item); i.e = ref(got); prog.emit(i); }
        if (!inProd) return;
        size_t go = emitJmpIfNot(ref(rsBreak));
        Instr r; r.k = IK::Ret; prog.emit(r);
        prog.code[go].target = prog.here();
    }
    void rsItem(const slang::ast::RandSeqProductionSymbol::ProdItem& it) {
        if (!it.target) unsupported("randsequence production");
        rsCall(it.target, true, &it);
    }
    void rsProduction(const slang::ast::RandSeqProductionSymbol& p) {
        using namespace slang::ast;
        using RS = RandSeqProductionSymbol;
        auto rules = p.getRules();
        if (rules.empty()) return;
        // Pick a rule by weight (default 1) when there is more than one (18.17.1).
        std::vector<size_t> starts, ends;
        int pick = -1;
        std::vector<int> w;
        if (rules.size() > 1) {
            E total;
            for (const auto& r : rules) {
                int t = fresh(64);
                Instr i; i.k = IK::Assign; i.sig = t;
                i.e = r.weightExpr ? resize(expr(*r.weightExpr), 64, r.weightExpr->type->isSigned(), false) : konst(mk(1, 64));
                prog.emit(i);
                w.push_back(t);
                total = total ? bin(Op::Add, total, ref(t)) : ref(t);
            }
            pick = fresh(64);
            Instr d; d.k = IK::Assign; d.sig = pick; d.e = weightedPick(total);
            prog.emit(d);
        }
        // Each use of a production with a value: a temporary, named in the
        // rule's code as the production (or production[i], from 1).
        std::vector<std::unordered_map<const RS*, std::vector<int>>> ruleVals(rules.size());
        for (size_t k = 0; k < rules.size(); k++) {
            std::function<void(const RS::ProdItem&)> use = [&](const RS::ProdItem& it) {
                if (!it.target || rsSigs[it.target].ret < 0) return;
                int t = fresh(storeW(it.target->getReturnType()));
                rsItemVal[&it] = t;
                ruleVals[k][it.target].push_back(t);
                rsState.push_back(t);
            };
            for (const RS::ProdBase* pb : rules[k].prods) {
                switch (pb->kind) {
                    case RS::ProdKind::Item: use(pb->as<RS::ProdItem>()); break;
                    case RS::ProdKind::IfElse: {
                        const auto& ie = pb->as<RS::IfElseProd>();
                        use(ie.ifItem);
                        if (ie.elseItem) use(*ie.elseItem);
                        break;
                    }
                    case RS::ProdKind::Repeat: use(pb->as<RS::RepeatProd>().item); break;
                    case RS::ProdKind::Case: {
                        const auto& cp = pb->as<RS::CaseProd>();
                        for (const auto& ci : cp.items) use(ci.item);
                        if (cp.defaultItem) use(*cp.defaultItem);
                        break;
                    }
                    default: break;
                }
            }
        }
        for (size_t k = 0; k < rules.size(); k++) {
            const auto& r = rules[k];
            // slang declares, in the rule's block, a variable per production
            // with a value: an int, or int[1:n] when it is used n times.
            auto savedVals = std::move(rsVals);
            rsVals.clear();
            for (const auto& m : r.ruleBlock->members()) {
                if (m.kind != SymbolKind::Variable || !m.as<VariableSymbol>().flags.has(VariableFlags::CompilerGenerated)) continue;
                for (auto& [ps, ts] : ruleVals[k])
                    if (ps->name == m.name) {
                        if (m.as<VariableSymbol>().getType().isUnpackedArray()) rsVals[&m] = ts;
                        else symOverride[&m] = ts.back();
                    }
            }
            size_t skip = 0;
            if (pick >= 0) skip = emitJmpIfNot(bin(Op::Lt, ref(pick), ref(w[k])));
            if (r.isRandJoin) { rsJoin(r); }
            else for (const RS::ProdBase* pb : r.prods) rsProd(pb);
            if (r.codeBlock) {
                if (const Statement* st = r.codeBlock->block->tryGetStatement()) stmt(*st);
            }
            for (const auto& m : r.ruleBlock->members()) symOverride.erase(&m);
            rsVals = std::move(savedVals);
            if (pick >= 0) {
                ends.push_back(emitJmp());
                prog.code[skip].target = prog.here();
                Instr sub; sub.k = IK::Assign; sub.sig = pick; sub.e = bin(Op::Sub, ref(pick), ref(w[k]));
                prog.emit(sub);
            }
        }
        patch(ends, prog.here());
    }
    // rand join (18.17.5): the items of the listed productions, interleaved
    // at random, each production's own items in order. Each step picks a
    // production with items left, weighted by how many it has left raised
    // to 2N-1 (N the join's bias, 0.5 by default: every one alike).
    void rsJoin(const slang::ast::RandSeqProductionSymbol::Rule& jr) {
        using namespace slang::ast;
        using RS = RandSeqProductionSymbol;
        double bias = 0.5;
        if (jr.randJoinExpr) {
            slang::ConstantValue cv = tryConst(*jr.randJoinExpr);
            if (cv.isReal()) bias = double(cv.real());
            else if (cv.isShortReal()) bias = double(cv.shortReal());
            else if (cv.isInteger()) bias = (double)cv.integer().as<int64_t>().value_or(0);
            else unsupported("rand join with a bias that is not constant");
        }
        bias = std::clamp(bias, 0.0, 1.0);
        auto k64 = [](uint64_t v) { return konst(mk(v, 64)); };
        struct Part { std::span<const RS::Rule> rules; int rule = -1, cnt = 0; E len; size_t maxLen = 0; };
        std::vector<Part> parts;
        for (const RS::ProdBase* pb : jr.prods) {
            if (pb->kind != RS::ProdKind::Item) unsupported("rand join of something other than a production");
            const auto& it = pb->as<RS::ProdItem>();
            if (!it.target || !it.args.empty()) unsupported("rand join of a production with arguments");
            Part pt;
            pt.rules = it.target->getRules();
            for (const auto& r : pt.rules)
                if (r.isRandJoin) unsupported("rand join of a production that is itself a rand join");
            if (pt.rules.size() > 1) {   // its rule, by weight, first (18.17.1)
                std::vector<int> w;
                E total;
                for (const auto& r : pt.rules) {
                    int t = fresh(64);
                    Instr i; i.k = IK::Assign; i.sig = t;
                    i.e = r.weightExpr ? resize(expr(*r.weightExpr), 64, r.weightExpr->type->isSigned(), false) : k64(1);
                    prog.emit(i);
                    w.push_back(t);
                    total = total ? bin(Op::Add, total, ref(t)) : ref(t);
                }
                int pick = fresh(64);
                Instr d; d.k = IK::Assign; d.sig = pick; d.e = weightedPick(total);
                prog.emit(d);
                E sel = k64(pt.rules.size() - 1), cum;
                std::vector<E> cums;
                for (size_t k = 0; k < w.size(); k++) { cum = cum ? bin(Op::Add, cum, ref(w[k])) : ref(w[k]); cums.push_back(cum); }
                for (size_t k = w.size() - 1; k-- > 0;) sel = mux(bin(Op::Lt, ref(pick), cums[k]), k64(k), sel);
                pt.rule = fresh(64);
                Instr a; a.k = IK::Assign; a.sig = pt.rule; a.e = sel;
                prog.emit(a);
            }
            // Its steps: the rule's items, then its code block.
            E len;
            for (size_t k = pt.rules.size(); k-- > 0;) {
                size_t n = pt.rules[k].prods.size() + (pt.rules[k].codeBlock ? 1 : 0);
                pt.maxLen = std::max(pt.maxLen, n);
                len = len ? mux(bin(Op::Eq, ref(pt.rule), k64(k)), k64(n), len) : k64(n);
            }
            pt.len = len;
            pt.cnt = fresh(64);
            Instr z; z.k = IK::Assign; z.sig = pt.cnt; z.e = k64(0);
            prog.emit(z);
            parts.push_back(std::move(pt));
        }
        size_t top = prog.here();
        std::vector<int> wts;
        E total;
        for (auto& pt : parts) {
            E rem = bin(Op::Sub, pt.len, ref(pt.cnt));
            E wt;
            if (bias == 0.5) wt = k64(1);
            else {   // round(1e6 * rem^(2N-1)), at least 1
                wt = k64(1);
                for (size_t r = pt.maxLen; r >= 1; r--) {
                    double x = std::pow((double)r, 2 * bias - 1) * 1e6;
                    wt = mux(bin(Op::Eq, rem, k64(r)), k64(std::max<uint64_t>(1, (uint64_t)std::llround(x))), wt);
                }
            }
            int t = fresh(64);
            Instr i; i.k = IK::Assign; i.sig = t; i.e = mux(bin(Op::Eq, rem, k64(0)), k64(0), wt);
            prog.emit(i);
            wts.push_back(t);
            total = total ? bin(Op::Add, total, ref(t)) : ref(t);
        }
        size_t done = emitJmpIfNot(bin(Op::Ne, total, k64(0)));
        int pick = fresh(64);
        { Instr d; d.k = IK::Assign; d.sig = pick; d.e = weightedPick(total); prog.emit(d); }
        std::vector<size_t> next;
        for (size_t p = 0; p < parts.size(); p++) {
            Part& pt = parts[p];
            size_t skip = emitJmpIfNot(bin(Op::Lt, ref(pick), ref(wts[p])));
            Instr inc; inc.k = IK::Assign; inc.sig = pt.cnt; inc.e = bin(Op::Add, ref(pt.cnt), k64(1));
            std::vector<size_t> out;
            for (size_t k = 0; k < pt.rules.size(); k++) {
                const auto& r = pt.rules[k];
                size_t n = r.prods.size() + (r.codeBlock ? 1 : 0);
                for (size_t st = 0; st < n; st++) {
                    E here = bin(Op::Eq, ref(pt.cnt), k64(st));
                    if (pt.rule >= 0) here = bin(Op::LogAnd, bin(Op::Eq, ref(pt.rule), k64(k)), here);
                    size_t no = emitJmpIfNot(here);
                    prog.emit(inc);   // first, so a step that ends the join early is counted
                    if (st < r.prods.size()) rsProd(r.prods[st]);
                    else if (const Statement* s = r.codeBlock->block->tryGetStatement()) stmt(*s);
                    out.push_back(emitJmp());
                    prog.code[no].target = prog.here();
                }
            }
            patch(out, prog.here());
            next.push_back(emitJmp());
            prog.code[skip].target = prog.here();
            Instr sub; sub.k = IK::Assign; sub.sig = pick; sub.e = bin(Op::Sub, ref(pick), ref(wts[p]));
            prog.emit(sub);
        }
        patch(next, top);
        Instr j; j.k = IK::Jmp; j.target = top; prog.emit(j);
        prog.code[done].target = prog.here();
    }
    // One item of a randsequence rule (18.17).
    void rsProd(const slang::ast::RandSeqProductionSymbol::ProdBase* pb) {
        using namespace slang::ast;
        using RS = RandSeqProductionSymbol;
        switch (pb->kind) {
            case RS::ProdKind::Item: rsItem(pb->as<RS::ProdItem>()); break;
            case RS::ProdKind::CodeBlock: {
                const Statement* st = pb->as<RS::CodeBlockProd>().block->tryGetStatement();
                if (st) stmt(*st);
                break;
            }
            case RS::ProdKind::IfElse: {
                const auto& ie = pb->as<RS::IfElseProd>();
                size_t no = emitJmpIfNot(expr(*ie.expr));
                rsItem(ie.ifItem);
                size_t end = emitJmp();
                prog.code[no].target = prog.here();
                if (ie.elseItem) rsItem(*ie.elseItem);
                prog.code[end].target = prog.here();
                break;
            }
            case RS::ProdKind::Repeat: {
                const auto& rp = pb->as<RS::RepeatProd>();
                int n = fresh(64);
                Instr i; i.k = IK::Assign; i.sig = n;
                i.e = resize(expr(*rp.expr), 64, rp.expr->type->isSigned(), false);
                prog.emit(i);
                size_t top = prog.here();
                size_t done = emitJmpIfNot(bin(Op::Gt, ref(n), konst(mk(0, 64))));
                rsItem(rp.item);
                Instr dec; dec.k = IK::Assign; dec.sig = n; dec.e = bin(Op::Sub, ref(n), konst(mk(1, 64)));
                prog.emit(dec);
                Instr j; j.k = IK::Jmp; j.target = top; prog.emit(j);
                prog.code[done].target = prog.here();
                break;
            }
            case RS::ProdKind::Case: {
                const auto& cp = pb->as<RS::CaseProd>();
                int v = fresh(64);
                Instr i; i.k = IK::Assign; i.sig = v;
                i.e = resize(expr(*cp.expr), 64, cp.expr->type->isSigned(), false);
                prog.emit(i);
                std::vector<size_t> out;
                for (const auto& ci : cp.items) {
                    E any;
                    for (const Expression* x : ci.expressions) {
                        E eq = bin(Op::CaseEq, ref(v), resize(expr(*x), 64, x->type->isSigned(), false));
                        any = any ? bin(Op::LogOr, any, eq) : eq;
                    }
                    size_t no = emitJmpIfNot(any);
                    rsItem(ci.item);
                    out.push_back(emitJmp());
                    prog.code[no].target = prog.here();
                }
                if (cp.defaultItem) rsItem(*cp.defaultItem);
                patch(out, prog.here());
                break;
            }
        }
    }
    size_t emitJmp() {
        Instr j;
        j.k = IK::Jmp;
        return prog.emit(j);
    }
    void patch(std::vector<size_t>& sites, size_t target) {
        for (size_t pc : sites) prog.code[pc].target = target;
        sites.clear();
    }

    // Constant integer from a literal or a slang-folded constant expression.
    static int64_t constInt(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (e.kind == ExpressionKind::IntegerLiteral)
            return *e.as<IntegerLiteral>().getValue().as<int64_t>();
        if (e.kind == ExpressionKind::Conversion)
            return constInt(e.as<ConversionExpression>().operand());
        if (const auto* cv = e.getConstant(); cv && cv->isInteger()) {
            if (auto v = cv->integer().as<int64_t>()) return *v;
        }
        unsupported("non-constant select bound");
    }

    static void needWidth(int w) {
        if (w <= 0) unsupported("non-positive width");
    }

    // Put selfTag on a constant or a ref. Other ops take their signedness
    // from eval, which reads the tags on their operands.
    static E tagExpr(E e, bool selfTag) {
        if (!e) return e;
        if (e->op == Op::Const) {
            if (e->k.sgn == selfTag) return e;
            Val k = e->k;
            k.sgn = selfTag;
            return konst(k);
        }
        if (e->op == Op::Ref) return e->sgn == selfTag ? e : ref(e->sig, selfTag);
        // A signed field of a struct (s.offset) is a slice, which reads as unsigned.
        if (e->op == Op::Slice && selfTag) { int w = e->hi - e->lo + 1; return resize(std::move(e), w, false, true); }
        return e;
    }

    // Honor ctx when a parent asked for a width. Otherwise keep selfTag.
    E sized(E e, int srcW, bool selfTag, ExprCtx ctx) {
        if (!ctx.live) return tagExpr(e, selfTag);
        needWidth(ctx.w);
        if (ctx.w == srcW && ctx.tag == selfTag) return tagExpr(e, selfTag);
        return resize(std::move(e), ctx.w, ctx.fill && ctx.w > srcW, ctx.tag);
    }

    static bool isShift(slang::ast::BinaryOperator op) {
        using slang::ast::BinaryOperator;
        switch (op) {
            case BinaryOperator::LogicalShiftLeft:
            case BinaryOperator::LogicalShiftRight:
            case BinaryOperator::ArithmeticShiftLeft:
            case BinaryOperator::ArithmeticShiftRight:
                return true;
            default:
                return false;
        }
    }

    static bool widthPropagates(slang::ast::BinaryOperator op) {
        using slang::ast::BinaryOperator;
        switch (op) {
            case BinaryOperator::Add:
            case BinaryOperator::Subtract:
            case BinaryOperator::Multiply:
            case BinaryOperator::BinaryAnd:
            case BinaryOperator::BinaryOr:
            case BinaryOperator::BinaryXor:
            case BinaryOperator::BinaryXnor:
                return true;
            default:
                return false;
        }
    }

    static bool isDivMod(slang::ast::BinaryOperator op) {
        using slang::ast::BinaryOperator;
        return op == BinaryOperator::Divide || op == BinaryOperator::Mod;
    }

    static const slang::ast::Expression& peel(const slang::ast::Expression& e) {
        using namespace slang::ast;
        const Expression* p = &e;
        while (p->kind == ExpressionKind::Conversion)
            p = &p->as<ConversionExpression>().operand();
        return *p;
    }

    // Copy inputs in, call, copy outputs back. A function result is snapshotted
    // into a fresh signal: f(a)+f(b) would otherwise read the same return signal
    // twice and see only the second call.
    // super.f() (8.15) calls the base class's body, never a derived override.
    static bool superCall(const slang::ast::CallExpression& call) {
        if (!call.syntax) return false;
        // the first token (its leading comments and spaces are trivia, not text)
        return call.syntax->getFirstToken().rawText() == "super";
    }
    // Package UPF (IEEE 1801-2024 11.2.4), added by msim -upf: the functions
    // that change or read supplies run in the power-aware runtime
    // (upf_rt.inc) as SF_UPF with lo = the UpfOp.
    static int upfOp(const slang::ast::SubroutineSymbol& sym) {
        const auto* sc = sym.getParentScope();
        if (!sc || sc->asSymbol().kind != slang::ast::SymbolKind::Package || sc->asSymbol().name != "UPF") return -1;
        std::string_view n = sym.name;
        if (n == "supply_on") return UPF_SUPPLY_ON;
        if (n == "supply_off") return UPF_SUPPLY_OFF;
        if (n == "supply_partial_on") return UPF_SUPPLY_PARTIAL_ON;
        if (n == "set_supply_state") return UPF_SET_SUPPLY_STATE;
        if (n == "set_power_state") return UPF_SET_POWER_STATE;
        if (n == "get_supply_value") return UPF_GET_SUPPLY_VALUE;
        return -1;
    }
    E upfCall(const slang::ast::CallExpression& call, const slang::ast::SubroutineSymbol& sym, bool wantValue) {
        int op = upfOp(sym);
        if (op < 0) return nullptr;
        auto args = call.arguments();
        std::vector<E> k;
        if (args.empty()) unsupported("UPF function without arguments");
        k.push_back(exprStr(*args[0]));
        if (op == UPF_SUPPLY_ON || op == UPF_SUPPLY_PARTIAL_ON)
            k.push_back(args.size() > 1 ? realOf(*args[1]) : konst(mkreal(1.0)));
        else if (op == UPF_SET_SUPPLY_STATE) {
            if (args.size() < 2) unsupported("set_supply_state without a state");
            k.push_back(resize(expr(*args[1]), 32, false, false));
        } else if (op == UPF_SET_POWER_STATE) {
            if (args.size() < 2) unsupported("set_power_state without a state");
            k.push_back(exprStr(*args[1]));
        }
        E r = sysFnNode(SF_UPF, k, op);
        if (wantValue) return r;
        Instr i; i.k = IK::Assign; i.sig = fresh(op == UPF_GET_SUPPLY_VALUE ? 64 : 1); i.e = r;
        prog.emit(i);
        return nullptr;
    }
    // A task or function of an interface called through a virtual
    // interface (25.9): the same subroutine of whichever instance v names.
    E vifCall(const slang::ast::CallExpression& call, const slang::ast::SubroutineSymbol& sym, bool wantValue) {
        using namespace slang::ast;
        const Expression& v = *call.thisClass();
        const auto& vt = v.type->getCanonicalType().as<VirtualInterfaceType>();
        const DefinitionSymbol* def = &vt.iface.getDefinition();
        int h = fresh(64);
        { Instr i; i.k = IK::Assign; i.sig = h; i.e = exprH(v); prog.emit(i); }
        int res = -1;
        int rw = 0;
        if (wantValue) {
            rw = storeW(sym.getReturnType());
            if (rw <= 0) unsupported("return type of a function called through a virtual interface");
            res = fresh(rw);
        }
        std::vector<size_t> ends;
        auto& vi = vifInfo();
        for (size_t k = 1; k < vi.insts.size(); k++) {
            const InstanceSymbol& inst = *vi.insts[k];
            if (&inst.getDefinition() != def) continue;
            const Symbol* f = inst.body.find(sym.name);
            if (!f || f->kind != SymbolKind::Subroutine) continue;
            size_t no = emitJmpIfNot(bin(Op::Eq, ref(h), konst(make((uint64_t)k, 0, 64))));
            vifTarget = &f->as<SubroutineSymbol>();
            E r = emitUserCall(call, wantValue);
            if (wantValue) { Instr i; i.k = IK::Assign; i.sig = res; i.e = resize(r, rw, sym.getReturnType().isSigned(), false); prog.emit(i); }
            ends.push_back(emitJmp());
            prog.code[no].target = prog.code.size();
        }
        // No instance matched: a null virtual interface (25.9).
        { Instr i; i.k = IK::Assign; i.sig = fresh(64); i.e = node(Op::Ref); i.e->sig = -1; i.e->at = konst(make(0, 0, 64)); prog.emit(i); }
        patch(ends, prog.code.size());
        return wantValue ? ref(res, sym.getReturnType().isSigned()) : nullptr;
    }
    const slang::ast::SubroutineSymbol* vifTarget = nullptr;
    E emitUserCall(const slang::ast::CallExpression& call, bool wantValue) {
        using namespace slang::ast;
        if (call.isSystemCall()) unsupported("system call");
        const auto* sym = std::get<const SubroutineSymbol*>(call.subroutine);
        bool viaVif = false;
        if (vifTarget) { sym = vifTarget; vifTarget = nullptr; viaVif = true; }
        else if (call.thisClass() && call.thisClass()->type->isVirtualInterface()) return vifCall(call, *sym, wantValue);
        if (!sym || !subs) unsupported("subroutine call");
        if (sym->flags.has(MethodFlags::DPIImport)) return emitDpiCall(call, *sym, wantValue);
        if (sym->flags.has(MethodFlags::Randomize)) return randomizeCall(call);
        if (E r = upfCall(call, *sym, wantValue); r || upfOp(*sym) >= 0) return r;
        if (!sym->getSyntax() && isClassMethod(*sym)) {   // built-in methods of every class (18.8, 18.9, 18.13.3)
            if (sym->name == "rand_mode" || sym->name == "constraint_mode") {
                Instr i; i.k = IK::RandMode;
                i.e = call.thisClass() ? exprH(*call.thisClass()) : node(Op::This);
                i.de = expr(*call.arguments()[0]);
                i.hi = sym->name == "constraint_mode";
                prog.emit(i);
                return konst(mk(0, 32));
            }
            if (sym->name == "srandom" || sym->name == "get_randstate" || sym->name == "set_randstate") {   // 18.13.3, 18.15
                E self = call.thisClass() ? exprH(*call.thisClass()) : node(Op::This);
                std::vector<E> k{self};
                if (sym->name == "srandom") k.push_back(resize(expr(*call.arguments()[0]), 64, true, false));
                if (sym->name == "set_randstate") k.push_back(exprStr(*call.arguments()[0]));
                E r = sysFnNode(sym->name == "srandom" ? SF_OBJ_SRANDOM : sym->name == "get_randstate" ? SF_OBJ_GETSTATE : SF_OBJ_SETSTATE, k);
                if (wantValue) return r;
                Instr i; i.k = IK::Assign; i.sig = fresh(sym->name == "get_randstate" ? 64 : 32); i.e = r;
                prog.emit(i);
                return nullptr;
            }
        }
        if (sym->flags.has(MethodFlags::InterfaceExtern))   // extern task of an interface: the module that exports it (25.7)
            if (const auto* pr = sym->getPrototype(); pr && pr->getFirstExternImpl()) {
                if (pr->getFirstExternImpl()->getNextImpl()) unsupported("extern forkjoin task with more than one export");
                sym = pr->getFirstExternImpl()->impl;
            }
        auto it = subs->find(sym);
        if (it == subs->end()) {
            auto sk = skippedSubs().find(sym);
            unsupported(sk != skippedSubs().end() ? sk->second.c_str() : "subroutine call");
        }
        Subr* sub = it->second;
        if (current) current->calls.push_back(sub);

        auto args = call.arguments();
        if (args.size() != sub->args.size()) unsupported("subroutine argument count");
        if (isClassMethod(*sym)) {   // 8.6, 8.10, 8.20
            bool isStatic = sym->flags.has(MethodFlags::Static);
            E self;
            if (!isStatic && call.thisClass()) self = exprH(*call.thisClass());
            else if (!isStatic && defaultSelf) self = defaultSelf;   // in a default argument: the callee's object
            if (!isStatic && !call.thisClass() && !curClass && !defaultSelf) unsupported("method call without an object");
            int vslot = -1;
            if (!isStatic && sym->isVirtual() && !superCall(call)) {
                auto vs = vslotMap().find(sym);
                if (vs != vslotMap().end()) vslot = vs->second;
                auto is = ifaceSlotMap().find(sym);
                if (is != ifaceSlotMap().end()) vslot = -2 - is->second;
            }
            return emitAutoCall(*sub, args, wantValue, self, isStatic, vslot);
        }
        if (call.thisClass() && !viaVif) unsupported("method call on a non-class");
        if (sub->lay) return emitAutoCall(*sub, args, wantValue);

        // f(1, g(2)) where g calls f: the call in a later argument would
        // overwrite the formals set before it, so then every value goes
        // through a temporary first and the formals are set after.
        bool staged = false;
        for (size_t n = 1; n < args.size() && !staged; n++) staged = hasUserCall(*args[n]);
        std::vector<std::pair<int, int>> stagedSet;   // (formal, temporary)
        for (size_t n = 0; n < args.size(); n++) {
            const auto& formal = sub->args[n];
            const Expression& a = *args[n];
            if (staged && !formal.arr && formal.dir == ArgumentDirection::In && a.kind != ExpressionKind::EmptyArgument) {
                int tmp = fresh(formal.w);
                Instr i; i.k = IK::Assign; i.sig = tmp; i.e = expr(a);
                prog.emit(i);
                stagedSet.push_back({formal.sig, tmp});
                continue;
            }
            if (formal.dir == ArgumentDirection::Ref) {
                Instr i; i.k = IK::Assign; i.sig = formal.sig; i.e = addrOf(a);
                prog.emit(i);
                continue;
            }
            if (formal.arr) {   // 7.7: copy the whole array in
                if (formal.dir == ArgumentDirection::Out) continue;
                const Expression& src = formal.dir == ArgumentDirection::InOut ? a.as<AssignmentExpression>().left() : a;
                copyArrayArg(*formal.arr, src);
                continue;
            }
            if (formal.dir == ArgumentDirection::In) {
                if (a.kind == ExpressionKind::EmptyArgument) unsupported("empty argument");
                Instr i;
                i.k = IK::Assign;
                i.sig = formal.sig;
                i.e = expr(a);
                prog.emit(i);
            } else if (formal.dir == ArgumentDirection::InOut) {
                if (a.kind != ExpressionKind::Assignment) unsupported("inout argument");
                Instr i;
                i.k = IK::Assign;
                i.sig = formal.sig;
                i.e = expr(a.as<AssignmentExpression>().left());
                prog.emit(i);
            } else if (a.kind != ExpressionKind::Assignment &&
                       a.kind != ExpressionKind::EmptyArgument) {
                unsupported("output argument");
            }
        }

        for (auto [f, t] : stagedSet) { Instr i; i.k = IK::Assign; i.sig = f; i.e = ref(t); prog.emit(i); }
        Instr c;
        c.k = IK::Call;
        c.callee = sub->prog;
        prog.emit(c);

        for (size_t n = 0; n < args.size(); n++) {
            const auto& formal = sub->args[n];
            if (formal.dir != ArgumentDirection::Out && formal.dir != ArgumentDirection::InOut)
                continue;
            const Expression& a = *args[n];
            if (a.kind == ExpressionKind::EmptyArgument) continue;
            if (a.kind != ExpressionKind::Assignment) unsupported("output argument");
            const auto& as = a.as<AssignmentExpression>();
            if (formal.arr) {
                int src = -1;
                if (isDynType(formal.arr->getType())) src = dynFind(formal.arr);
                else if (auto fs = arrFind(formal.arr)) src = fs->base;
                if (src < 0) unsupported("array output argument");
                copyArrayOut(*formal.arr, as.left(), src);
                continue;
            }
            Instr i;
            i.k = IK::Assign;
            lhs(as.left(), i);
            OutSlot slot{formal.sig, formal.w, formal.sgn};
            const OutSlot* saved = outSlot;
            outSlot = &slot;
            i.e = expr(as.right());
            outSlot = saved;
            prog.emit(i);
        }

        if (!wantValue) return {};
        if (sub->retSig < 0) unsupported("subroutine call has no return value");
        int tmp = fresh(sub->retW);
        Instr i;
        i.k = IK::Assign;
        i.sig = tmp;
        i.e = ref(sub->retSig, sub->retSgn);
        prog.emit(i);
        return ref(tmp, sub->retSgn);
    }

    // A DPI import call (35): inputs are copied into the formals' signals,
    // IK::Dpi calls C, and outputs are copied back, as for an SV subroutine.
    // An unpacked array argument is passed in place: the call records the
    // actual array (a fixed array or a container) instead of a signal.
    E emitDpiCall(const slang::ast::CallExpression& call, const slang::ast::SubroutineSymbol& sym, bool wantValue) {
        using namespace slang::ast;
        auto fit = dpiMap().find(&sym);
        if (fit == dpiMap().end()) unsupported("DPI import");
        const DpiFunc* f = fit->second;
        auto args = call.arguments();
        if (args.size() != f->args.size()) unsupported("DPI argument count");
        Instr c;
        c.k = IK::Dpi;
        c.dpi = f;
        c.dact.resize(args.size());
        for (size_t n = 0; n < args.size(); n++) {
            const DpiArg& formal = f->args[n];
            const Expression& a = *args[n];
            if (formal.uarr) {
                const Expression* x = &a;
                if (formal.dir != 0) {
                    if (a.kind != ExpressionKind::Assignment) unsupported("DPI array argument");
                    x = &a.as<AssignmentExpression>().left();
                }
                x = &peel(*x);
                if (!ValueExpressionBase::isKind(x->kind)) unsupported("DPI array argument that is not a variable");
                const Symbol* as = &x->as<ValueExpressionBase>().symbol;
                if (auto it = arrMap().find(as); it != arrMap().end()) c.dact[n].arr = it->second;
                else if (int d = dynSig(*x); d >= 0) c.dact[n].dyn = d;
                else unsupported("DPI array argument of this kind");
                continue;
            }
            if (formal.dir == 0) {
                if (a.kind == ExpressionKind::EmptyArgument) unsupported("empty argument");
                Instr i;
                i.k = IK::Assign;
                i.sig = formal.sig;
                i.e = expr(a);
                if (formal.t == DT_REAL || formal.t == DT_SHORTREAL) i.e = realOf(a);
                prog.emit(i);
            } else if (formal.dir == 2) {
                if (a.kind != ExpressionKind::Assignment) unsupported("inout argument");
                Instr i;
                i.k = IK::Assign;
                i.sig = formal.sig;
                const Expression& l = a.as<AssignmentExpression>().left();
                i.e = (formal.t == DT_REAL || formal.t == DT_SHORTREAL) ? realOf(l) : expr(l);
                prog.emit(i);
            } else if (a.kind != ExpressionKind::Assignment && a.kind != ExpressionKind::EmptyArgument) {
                unsupported("output argument");
            }
        }
        if (srcMgr()) {
            auto loc = call.sourceRange.start();
            c.fmt = std::string(srcMgr()->getFileName(loc));
            c.dline = (int)srcMgr()->getLineNumber(loc);
            // A call from protected IP: svGetCallerInfo may not give the
            // place in the IP author's source (IEEE 1735 9.7.2.3).
            if (hideMsgIf() && hideMsgIf()(loc)) {
                c.fmt = "<protected>";
                c.dline = 0;
            }
        }
        prog.emit(c);
        for (size_t n = 0; n < args.size(); n++) {
            const DpiArg& formal = f->args[n];
            if (formal.dir == 0 || formal.uarr) continue;
            const Expression& a = *args[n];
            if (a.kind == ExpressionKind::EmptyArgument) continue;
            const auto& as = a.as<AssignmentExpression>();
            Instr i;
            i.k = IK::Assign;
            lhs(as.left(), i);
            OutSlot slot{formal.sig, formal.w, formal.sgn};
            const OutSlot* saved = outSlot;
            outSlot = &slot;
            i.e = expr(as.right());
            outSlot = saved;
            prog.emit(i);
        }
        if (!wantValue) return {};
        if (f->retSig < 0) unsupported("DPI call has no return value");
        int tmp = fresh(f->retW);
        Instr i;
        i.k = IK::Assign;
        i.sig = tmp;
        i.e = ref(f->retSig, f->retSgn);
        prog.emit(i);
        return ref(tmp, f->retSgn);
    }

    // A call of a user system task or function registered through VPI
    // (38.37). A variable or net argument is passed as itself (the C side
    // reads and writes it); any other expression is evaluated into a
    // temporary just before the call. A function's result is whatever the
    // calltf puts on the call handle (vpi_put_value), read right after it.
    static std::vector<std::unique_ptr<VpiSite>>& vpiSites() {
        static std::vector<std::unique_ptr<VpiSite>> v;
        return v;
    }
    E emitVpiCall(const slang::ast::CallExpression& call, const VpiSystfInfo& info, bool wantValue) {
        using namespace slang::ast;
        auto site = std::make_unique<VpiSite>();
        site->systf = info.idx;
        site->func = info.func;
        for (const Expression* a0 : call.arguments()) {
            VpiArg va;
            const Expression& a = *a0;
            if (a.kind == ExpressionKind::EmptyArgument) { site->args.push_back(va); continue; }
            if (a.kind == ExpressionKind::ArbitrarySymbol) {
                const Symbol* sym = a.as<ArbitrarySymbolExpression>().symbol;
                if (sym && sym->kind == SymbolKind::Instance) {
                    va.kind = VpiArg::Scope;
                    va.sym = sym;
                    site->args.push_back(va);
                    continue;
                }
                unsupported("this argument of a VPI system task");
            }
            va.vtype = a.getConstant() || a.kind == ExpressionKind::StringLiteral ? 7 : 39;   // vpiConstant, vpiOperation
            if (a.kind == ExpressionKind::StringLiteral) {
                va.kind = VpiArg::String;
                va.text = std::string(a.as<StringLiteral>().getValue());
                site->args.push_back(va);
                continue;
            }
            const Type& t = *a.type;
            if (ValueExpressionBase::isKind(a.kind) && t.isIntegral()) {
                const Symbol* sym = &a.as<ValueExpressionBase>().symbol;
                if (auto it = sigmap.find(sym); it != sigmap.end() && !symOverride.count(sym) && !localMap().count(sym)) {
                    va.kind = VpiArg::Signal;
                    va.sig = it->second;
                    va.w = (int)t.getBitWidth();
                    va.sgn = t.isSigned();
                    va.sym = sym;
                    site->args.push_back(va);
                    continue;
                }
            }
            // A whole fixed-size array: the array object (37.19), for
            // vpi_handle_by_index, vpi_iterate and the value-array calls.
            if (ValueExpressionBase::isKind(a.kind) && t.isUnpackedArray()) {
                const Symbol* sym = &a.as<ValueExpressionBase>().symbol;
                if (auto it = arrMap().find(sym); it != arrMap().end() && !symOverride.count(sym) && !localMap().count(sym)) {
                    va.kind = VpiArg::Signal;
                    va.sig = it->second->base;
                    va.w = it->second->elemW;
                    va.sgn = it->second->sgn;
                    va.sym = sym;
                    site->args.push_back(va);
                    continue;
                }
            }
            va.kind = VpiArg::Value;
            Instr i;
            i.k = IK::Assign;
            if (t.isFloating()) {
                va.real = true;
                va.w = 64;
                i.e = realOf(a);
            } else if (t.isString()) {
                va.str = true;
                va.w = 64;
                i.e = expr(a);
            } else if (t.isIntegral()) {
                va.w = (int)t.getBitWidth();
                va.sgn = t.isSigned();
                i.e = expr(a);
            } else {
                unsupported("an argument of this type to a VPI system task");
            }
            va.sig = i.sig = fresh(va.w);
            prog.emit(i);
            site->args.push_back(va);
        }
        if (info.func) {
            site->retReal = info.ftype == 2;   // vpiRealFunc
            site->retW = site->retReal || info.ftype == 3 ? 64 : info.ftype == 1 ? 32 : info.size;
            site->retSgn = info.ftype == 1 || info.ftype == 5;
            site->ret = fresh(site->retW);
        }
        if (srcMgr()) {
            auto loc = call.sourceRange.start();
            site->file = std::string(srcMgr()->getFileName(loc));
            site->prot = hideMsgIf() && hideMsgIf()(loc);
            site->line = (int)srcMgr()->getLineNumber(loc);
        }
        for (const Scope* sc = scope; sc; sc = sc->asSymbol().getParentScope()) {
            const Symbol& sy = sc->asSymbol();
            if (sy.kind == SymbolKind::InstanceBody) {
                site->scope = sy.as<InstanceBodySymbol>().parentInstance;
                break;
            }
        }
        Instr c;
        c.k = IK::Vpi;
        c.vpi = site.get();
        prog.emit(c);
        int ret = site->ret, retW = site->retW;
        bool sgn = site->retSgn;
        vpiSites().push_back(std::move(site));
        if (!wantValue) return {};
        int tmp = fresh(retW);
        Instr i;
        i.k = IK::Assign;
        i.sig = tmp;
        i.e = ref(ret, sgn);
        prog.emit(i);
        return ref(tmp, sgn);
    }

    // A call of an automatic subroutine (13.3.2): its arguments go straight
    // into the new frame; outputs and the result are copied out of the frame
    // it returned from (relative mode kRelRet) right after the call.
    E emitAutoCall(const Subr& sub, std::span<const slang::ast::Expression* const> args, bool wantValue,
                   E self = nullptr, bool noSelf = false, int vslot = -1) {
        using namespace slang::ast;
        Instr c;
        c.k = IK::Call;
        c.callee = sub.prog;
        c.lay = sub.lay;
        c.self = std::move(self);
        c.noSelf = noSelf;
        c.vslot = vslot;
        for (size_t n = 0; n < args.size(); n++) {
            const auto& formal = sub.args[n];
            const Expression& a = *args[n];
            if (formal.dir == ArgumentDirection::Ref) { c.init.push_back({formal.sig, addrOf(a)}); continue; }
            if (formal.arr) {   // 7.7: the whole array, copied into the new frame
                if (formal.dir == ArgumentDirection::Out) continue;
                const Expression& src = formal.dir == ArgumentDirection::InOut ? a.as<AssignmentExpression>().left() : a;
                if (isDynType(formal.arr->getType())) {
                    int d = dynSig(src);
                    if (d < 0) {   // build it in a temporary first
                        d = dynTemp(dynShapeOf(formal.arr->getType()));
                        assignContainer(d, src);
                    }
                    c.dinit.push_back({formal.sig, d});
                } else {
                    std::vector<E> el;
                    arrayElems(*src.type, src, el);
                    for (size_t k = 0; k < el.size(); k++) c.init.push_back({formal.sig + (int)k, el[k]});
                }
                continue;
            }
            if (formal.dir == ArgumentDirection::In) {
                if (a.kind == ExpressionKind::EmptyArgument) unsupported("empty argument");
                // A default argument (13.5.3) is evaluated for the callee: a
                // method it calls without an object runs on the callee's object.
                const auto* fs = sub.sym && n < sub.sym->getArguments().size() ? sub.sym->getArguments()[n] : nullptr;
                if (fs && fs->getDefaultValue() == &a && isClassMethod(*sub.sym) && !noSelf) {
                    E saved = defaultSelf;
                    defaultSelf = c.self ? c.self : node(Op::This);
                    c.init.push_back({formal.sig, expr(a)});
                    defaultSelf = saved;
                    continue;
                }
                // A string literal put into a typeless mailbox (15.4.9) goes
                // in as a string, so a get into a string variable reads it.
                if (fs && fs->getType().isUntypedType() && peel(a).kind == ExpressionKind::StringLiteral) {
                    c.init.push_back({formal.sig, konst(mkstr(std::string(peel(a).as<StringLiteral>().getValue())))});
                    continue;
                }
                c.init.push_back({formal.sig, expr(a)});
            } else if (formal.dir == ArgumentDirection::InOut) {
                if (a.kind != ExpressionKind::Assignment) unsupported("inout argument");
                c.init.push_back({formal.sig, expr(a.as<AssignmentExpression>().left())});
            } else if (a.kind != ExpressionKind::Assignment && a.kind != ExpressionKind::EmptyArgument) {
                unsupported("output argument");
            }
        }
        prog.emit(c);
        for (size_t n = 0; n < args.size(); n++) {
            const auto& formal = sub.args[n];
            if (formal.dir != ArgumentDirection::Out && formal.dir != ArgumentDirection::InOut) continue;
            const Expression& a = *args[n];
            if (a.kind == ExpressionKind::EmptyArgument) continue;
            const auto& as = a.as<AssignmentExpression>();
            if (formal.arr) { copyArrayOut(*formal.arr, as.left(), relSig(kRelRet, formal.sig)); continue; }
            Instr i;
            i.k = IK::Assign;
            lhs(as.left(), i);
            OutSlot slot{relSig(kRelRet, formal.sig), formal.w, formal.sgn};
            const OutSlot* saved = outSlot;
            outSlot = &slot;
            i.e = expr(as.right());
            outSlot = saved;
            prog.emit(i);
        }
        if (!wantValue) return {};
        if (sub.retSig < 0) unsupported("subroutine call has no return value");
        int tmp = fresh(sub.retW);
        Instr i;
        i.k = IK::Assign;
        i.sig = tmp;
        i.e = ref(relSig(kRelRet, sub.retSig), sub.retSgn);
        prog.emit(i);
        return ref(tmp, sub.retSgn);
    }

    // ---------- class handles (8) ----------
    // The value of a handle-typed expression: an object's first signal id, 0 for null.
    static bool isThisSym(const slang::ast::Symbol& s) { return s.name == "this" && s.kind == slang::ast::SymbolKind::Variable; }
    E exprH(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (E l = lazyExpr(e, ExprCtx{})) return l;
        switch (e.kind) {
            case ExpressionKind::NullLiteral: return konst(make(0, 0, 64));
            case ExpressionKind::Conversion: {
                const Expression& op = e.as<ConversionExpression>().operand();
                if (isHandleType(*op.type)) return exprH(op);
                if (op.type->isIntegral() && e.type->isCHandle()) return resize(expr(op), 64, false, false);
                unsupported("conversion to a class handle");
            }
            case ExpressionKind::NamedValue: {
                const Symbol& sym = e.as<NamedValueExpression>().symbol;
                if (isThisSym(sym)) {
                    if (!curClass) unsupported("`this` outside a method");
                    return node(Op::This);
                }
                int sg = sigFind(&sym);
                if (sg < 0) unsupported("class handle with no signal");
                return ref(sg);
            }
            case ExpressionKind::NewClass: return newObject(e.as<NewClassExpression>());
            case ExpressionKind::ArbitrarySymbol: {   // an interface instance as a virtual interface value (25.9)
                const Symbol* sym = e.as<ArbitrarySymbolExpression>().symbol;
                if (sym->kind == SymbolKind::InterfacePort) {
                    auto [conn, mp] = sym->as<InterfacePortSymbol>().getConnection();
                    sym = conn;
                }
                if (!sym || sym->kind != SymbolKind::Instance) unsupported("virtual interface value of this kind");
                return konst(make((uint64_t)vifInstance(sym->as<InstanceSymbol>()), 0, 64));
            }
            case ExpressionKind::CopyClass: {   // new h (8.12): a shallow copy
                int tmp = fresh(64);
                Instr i; i.k = IK::New; i.sig = tmp; i.e = exprH(e.as<CopyClassExpression>().sourceExpr());
                prog.emit(i);
                return ref(tmp);
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                return mux(un(Op::LogNot, un(Op::LogNot, condOf(c))), exprH(c.left()), exprH(c.right()));
            }
            case ExpressionKind::Assignment: unsupported("assignment of a handle inside an expression");
            default: break;
        }
        return expr2(e, ExprCtx{});
    }
    // The slot of property `member` in the object h names, as an indirect
    // id (expr.hpp mode 10/11). `n` consecutive ids for n consecutive slots.
    int indSlot(E h, int off, int n = 1) {
        int first = -1;
        for (int k = 0; k < n; k++) {
            int tmp = fresh(64);
            auto a = node(Op::Addr, {h});
            a->sig = off + k;
            Instr i; i.k = IK::Assign; i.sig = tmp; i.e = a;
            prog.emit(i);
            int id = indThrough(tmp);
            if (!k) first = id;
        }
        return first;
    }
    // The signal id of a ref argument's actual (13.5.2): a variable, an
    // array or container, or a property through a handle.
    E addrOf(const slang::ast::Expression& a0) {
        using namespace slang::ast;
        const Expression& a = outArg(a0);
        auto at = [](int sig) { auto n = node(Op::AddrOf); n->sig = sig; return n; };
        if (vifAccess(a)) return vifAddr(a.as<MemberAccessExpression>());   // ref to v.x (25.9)
        if (classMember(a)) {
            const auto& m = a.as<MemberAccessExpression>();
            const PropSlot* ps = memberSlot(m);
            if (!ps) {
                if (int d = dynFind(&m.member); d >= 0) return at(d);
                if (auto ar = arrFind(&m.member)) return at(ar->base);
                return at(sigAt(&m.member));
            }
            auto n = node(Op::Addr, {exprH(m.value())});
            n->sig = ps->kind == 1 ? ps->arr->base : ps->off;
            return n;
        }
        if (a.kind == ExpressionKind::NamedValue || a.kind == ExpressionKind::HierarchicalValue) {
            const Symbol* sym = &a.as<ValueExpressionBase>().symbol;
            if (int d = dynFind(sym); d >= 0) return at(d);
            if (auto ar = arrFind(sym)) return at(ar->base);
            return at(sigAt(sym));
        }
        unsupported("ref argument that is not a whole variable or property");
    }
    // A ref formal's kind and shape, from its type.
    static RefFormal refFormalOf(const slang::ast::Type& t0) {
        const auto& t = t0.getCanonicalType();
        RefFormal r;
        if (isDynType(t)) { r.kind = 2; r.dyn = dynShapeOf(t); return r; }
        if (t.isUnpackedArray()) {
            FrameLayout tmp;
            PropSlot ps = declSlot(t, &tmp);
            r.kind = 1;
            r.arr = ps.arr;
            r.arr->base = 0;
            return r;
        }
        if (t.isEvent()) unsupported("ref event argument");
        return r;
    }
    // h.member where h is a class handle: the property's PropSlot, or null
    // for a static property (an ordinary signal).
    static const PropSlot* memberSlot(const slang::ast::MemberAccessExpression& m) {
        auto it = propMap().find(&m.member);
        if (it != propMap().end()) return &it->second;
        if (m.member.kind == slang::ast::SymbolKind::ClassProperty) {
            if (m.member.as<slang::ast::ClassPropertySymbol>().lifetime == slang::ast::VariableLifetime::Static) return nullptr;
            unsupported("class property with no slot");
        }
        unsupported("class member access of this kind");
    }
    static bool classMember(const slang::ast::Expression& e) {
        if (e.kind != slang::ast::ExpressionKind::MemberAccess) return false;
        const auto& t = *e.as<slang::ast::MemberAccessExpression>().value().type;
        return isHandleType(t) && !t.isVirtualInterface();
    }
    // The virtual interface v of v.x or v.cb.x (25.9), or null.
    static const slang::ast::Expression* vifBase(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (e.kind != ExpressionKind::MemberAccess) return nullptr;
        const Expression& v = e.as<MemberAccessExpression>().value();
        if (v.type->isVirtualInterface()) return &v;
        if (v.kind == ExpressionKind::MemberAccess && v.as<MemberAccessExpression>().member.kind == SymbolKind::ClockingBlock &&
            v.as<MemberAccessExpression>().value().type->isVirtualInterface())
            return &v.as<MemberAccessExpression>().value();
        return nullptr;
    }
    static bool vifAccess(const slang::ast::Expression& e) { return vifBase(e) != nullptr; }
    // The interface instances a virtual interface of this type can name.
    static std::vector<std::pair<int, const slang::ast::InstanceSymbol*>> vifInstances(const slang::ast::Type& t) {
        using namespace slang::ast;
        const DefinitionSymbol* def = &t.getCanonicalType().as<VirtualInterfaceType>().iface.getDefinition();
        std::vector<std::pair<int, const InstanceSymbol*>> r;
        auto& vi = vifInfo();
        for (size_t k = 1; k < vi.insts.size(); k++)
            if (&vi.insts[k]->getDefinition() == def) r.push_back({(int)k, vi.insts[k]});
        return r;
    }
    // The clocking block of instance `inst` that cb (of the interface's
    // definition) stands for, built if nothing built it yet (14).
    const slang::ast::ClockingBlockSymbol* vifClocking(const slang::ast::InstanceSymbol& inst, const slang::ast::ClockingBlockSymbol& cb) {
        using namespace slang::ast;
        const Symbol* f = inst.body.find(cb.name);
        if (!f || f->kind != SymbolKind::ClockingBlock) return nullptr;
        const auto& icb = f->as<ClockingBlockSymbol>();
        auto savedFrames = frames;
        frames.clear();
        clockingIdx(icb);
        frames = savedFrames;
        return &icb;
    }
    // 1 + the signal id of v.x now (0 for a null v).
    E vifAddr(const slang::ast::MemberAccessExpression& m, bool elems = false) {
        using namespace slang::ast;
        const Expression* v = vifBase(m);
        const Symbol& mem = m.member;
        if (mem.kind == SymbolKind::ClockingBlock || mem.kind == SymbolKind::ClockVar) {
            const auto& cb = mem.kind == SymbolKind::ClockingBlock ? mem.as<ClockingBlockSymbol>()
                                                                     : mem.getParentScope()->asSymbol().as<ClockingBlockSymbol>();
            for (auto& [k, inst] : vifInstances(*v->type)) vifClocking(*inst, cb);
        } else {
            if (!mem.isValue()) unsupported("this member through a virtual interface");
            const Type& mt = mem.as<ValueSymbol>().getType().getCanonicalType();
            if ((mt.isUnpackedArray() && !elems) || isDynType(mt)) unsupported("an array member through a virtual interface");
        }
        auto n = node(Op::VifSig, {exprH(*v)});
        n->hi = vifMember(mem);
        return n;
    }
    // v.cb.x <= e (14.16) through a virtual interface: the drive of
    // whichever instance v names.
    bool vifSyncDrive(const slang::ast::AssignmentExpression& a) {
        using namespace slang::ast;
        const Expression* lp = &a.left();   // v.cb.a[i]: the clockvar under the indices
        while (lp->kind == ExpressionKind::ElementSelect &&
               lp->as<ElementSelectExpression>().value().type->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType)
            lp = &lp->as<ElementSelectExpression>().value();
        const Expression& l = *lp;
        if (!vifAccess(l) || l.as<MemberAccessExpression>().member.kind != SymbolKind::ClockVar) return false;
        if (!a.isNonBlocking()) unsupported("a write to a clockvar other than a synchronous drive (14.16)");
        const auto& m = l.as<MemberAccessExpression>();
        const auto& cb = m.member.getParentScope()->asSymbol().as<ClockingBlockSymbol>();
        const Expression* v = vifBase(l);
        int h = fresh(64);
        { Instr i; i.k = IK::Assign; i.sig = h; i.e = exprH(*v); prog.emit(i); }
        std::vector<size_t> ends;
        for (auto& [k, inst] : vifInstances(*v->type)) {
            const ClockingBlockSymbol* icb = vifClocking(*inst, cb);
            if (!icb) continue;
            const Symbol* cv = icb->find(m.member.name);
            if (!cv) continue;
            size_t no = emitJmpIfNot(bin(Op::Eq, ref(h), konst(make((uint64_t)k, 0, 64))));
            if (auto ca = arrMap().find(cv); ca != arrMap().end()) {   // an array clockvar: element by element
                cbArrayDrive(clockingIdx(*icb), ca->second->base, *ca->second, a);
                ends.push_back(emitJmp());
                prog.code[no].target = prog.code.size();
                continue;
            }
            if (&l != &a.left()) unsupported("a synchronous drive of part of a clockvar through a virtual interface (14.16)");
            Instr i;
            i.k = IK::CbDrive;
            i.sig = sigmap.at(cv);
            i.dkey = clockingIdx(*icb);
            i.dres = -1;
            auto& outs = clockings()[i.dkey].outs;
            for (size_t q = 0; q < outs.size(); q++)
                if (outs[q].cv == i.sig) i.dres = (int)q;
            if (i.dres < 0) unsupported("a drive of a clocking input (14.16)");
            i.e = expr(a.right());
            if (const TimingControl* tc = a.timingControl) {
                if (tc->kind != TimingControlKind::CycleDelay) unsupported("an intra-assignment delay in a synchronous drive (14.16)");
                i.de = expr(tc->as<CycleDelayControl>().expr);
            }
            prog.emit(i);
            ends.push_back(emitJmp());
            prog.code[no].target = prog.code.size();
        }
        patch(ends, prog.code.size());
        return true;
    }
    // v.x as an indirect id (expr.hpp mode 10/11); n consecutive ids.
    int vifSlot(const slang::ast::MemberAccessExpression& m, int n = 1, bool elems = false) {
        int first = -1;
        for (int k = 0; k < n; k++) {
            int tmp = fresh(64);
            Instr i; i.k = IK::Assign; i.sig = tmp;
            i.e = k ? bin(Op::Add, vifAddr(m, elems), konst(make((uint64_t)k, 0, 64))) : vifAddr(m, elems);
            prog.emit(i);
            if (!k) first = indThrough(tmp);
        }
        return first;
    }
    // The shape of a fixed-size array of integral elements, or null.
    static std::shared_ptr<ArrShape> fixedShape(const slang::ast::Type& t0) {
        using namespace slang::ast;
        const Type* t = &t0.getCanonicalType();
        if (t->kind != SymbolKind::FixedSizeUnpackedArrayType) return nullptr;
        auto a = std::make_shared<ArrShape>();
        int64_t total = 1;
        while (t->kind == SymbolKind::FixedSizeUnpackedArrayType) {
            const auto& ft = t->as<FixedSizeUnpackedArrayType>();
            a->dims.push_back({ft.range.left, ft.range.right});
            total *= (int64_t)ft.range.width();
            t = &ft.elementType.getCanonicalType();
        }
        if (!t->isIntegral() || total > 4096) return nullptr;
        a->count = (int)total;
        a->elemW = (int)t->getBitWidth();
        a->sgn = t->isSigned();
        a->twoState = !t->isFourState();
        return a;
    }
    // v.a or v.cb.a (25.9, 14.3): a fixed-size array member through a virtual
    // interface, as one indirect id per element; null for anything else.
    std::shared_ptr<const ArrShape> vifArr(const slang::ast::Expression& x) {
        using namespace slang::ast;
        if (!vifAccess(x)) return nullptr;
        const auto& m = x.as<MemberAccessExpression>();
        if (!m.member.isValue()) return nullptr;
        auto a = fixedShape(m.member.as<ValueSymbol>().getType());
        if (!a) return nullptr;
        if (a->count > 256) unsupported("an array of more than 256 elements through a virtual interface");
        a->base = vifSlot(m, a->count, true);
        return a;
    }
    // new / new(args) (8.7): allocate, then run the constructor on it.
    E newObject(const slang::ast::NewClassExpression& n) {
        using namespace slang::ast;
        if (n.isSuperClass) unsupported("super.new outside a constructor");
        ClassInfo* ci = classInfo(*n.type);
        if (ci->ty->isAbstract) unsupported("new of an abstract class");
        int tmp = fresh(64);
        Instr i; i.k = IK::New; i.sig = tmp; i.hi = ci->id;
        prog.emit(i);
        callCtor(*ci, n.constructorCall(), ref(tmp));
        return ref(tmp);
    }
    // Run ci's constructor on object `self` (null: this object, from a
    // derived constructor) with the arguments of call (none if null).
    void callCtor(const ClassInfo& ci, const slang::ast::Expression* call, E self) {
        using namespace slang::ast;
        if (!ci.ctor) unsupported("class with no constructor");
        std::vector<const Expression*> args;
        if (call) {
            if (call->kind != ExpressionKind::Call) unsupported("constructor call");
            for (auto* a : call->as<CallExpression>().arguments()) args.push_back(a);
        }
        // An implied super.new() (8.17) passes no arguments: the base
        // constructor's defaults apply.
        if (ci.ctor->sym && args.size() < ci.ctor->args.size()) {
            auto formals = ci.ctor->sym->getArguments();
            for (size_t k = args.size(); k < formals.size(); k++) {
                const Expression* d = formals[k]->getDefaultValue();
                if (!d) break;
                args.push_back(d);
            }
        }
        if (args.size() != ci.ctor->args.size()) unsupported("constructor argument count");
        emitAutoCall(*ci.ctor, args, false, std::move(self));
    }
    // The body of a method of a built-in class: weak_reference (8.30),
    // semaphore (15.3), mailbox (15.4), process (9.7).
    void builtinMethod(const ClassInfo& ci, const Subr& sub) {
        using namespace slang::ast;
        std::string_view n = sub.sym ? sub.sym->name : "new";
        auto arg = [&](size_t k) { return ref(relSig(0, sub.args.at(k).sig), sub.args.at(k).sgn); };
        auto set = [&](int sig, E e) { Instr i; i.k = IK::Assign; i.sig = sig; i.e = std::move(e); prog.emit(i); };
        auto ret = [&](E e) { set(relSig(0, sub.retSig), std::move(e)); };
        auto slot = [&](int off, bool sgn = false) { return ref(thisRel(off), sgn); };
        auto bump = [&](int off) { set(thisRel(off), bin(Op::Add, slot(off), konst(mk(1, 32)))); };
        // while (cond) @(change count);
        auto waitWhile = [&](const std::function<E()>& cond, int chg) {
            size_t top = prog.code.size();
            size_t go = emitJmpIfNot(cond());
            Instr w; w.k = IK::WaitEdge; w.sig = thisRel(chg); w.edge = ANY;
            prog.emit(w);
            prog.code[emitJmp()].target = top;
            prog.code[go].target = prog.code.size();
        };
        // if (cond) { then(); result 1 } else result 0
        auto tryDo = [&](E cond, const std::function<void()>& then) {
            ret(konst(mk(0, 32)));
            size_t no = emitJmpIfNot(std::move(cond));
            then();
            ret(konst(mk(1, 32)));
            prog.code[no].target = prog.code.size();
        };
        switch (ci.builtin) {
            case 1: {
                Instr i; i.k = IK::Assign;
                if (n == "new") { i.sig = thisRel(ci.bslot); i.e = arg(0); }
                else if (n == "get") { i.sig = relSig(0, sub.retSig); i.e = ref(thisRel(ci.bslot)); }
                else if (n == "clear") { i.sig = thisRel(ci.bslot); i.e = konst(make(0, 0, 64)); }
                else if (n == "get_id") { i.sig = relSig(0, sub.retSig); i.e = arg(0); }   // the handle: stable, nonzero, 0 for null
                else unsupported("built-in method");
                prog.emit(i);
                return;
            }
            case 2: {   // semaphore: keys, change count
                int keys = ci.bslot, chg = ci.bslot2;
                auto enough = [&]() { return bin(Op::Ge, slot(keys, true), arg(0)); };
                if (n == "new") set(thisRel(keys), arg(0));
                else if (n == "put") { set(thisRel(keys), bin(Op::Add, slot(keys, true), arg(0))); bump(chg); }
                else if (n == "get") {
                    waitWhile([&]() { return bin(Op::Lt, slot(keys, true), arg(0)); }, chg);
                    set(thisRel(keys), bin(Op::Sub, slot(keys, true), arg(0)));
                    bump(chg);
                } else if (n == "try_get")
                    tryDo(enough(), [&]() { set(thisRel(keys), bin(Op::Sub, slot(keys, true), arg(0))); bump(chg); });
                return;   // the methods every class has (randomize, ...) are handled at the call
            }
            case 3: {   // mailbox: queue, bound, change count
                int q = thisRel(ci.bslot), bound = ci.bslot2, chg = ci.bslot3;
                auto size = [&]() { auto x = node(Op::DynSize); x->sig = q; return x; };
                auto full = [&]() {
                    return bin(Op::LogAnd, bin(Op::Gt, slot(bound, true), konst(mk(0, 32))),
                               bin(Op::Ge, size(), slot(bound, true)));
                };
                auto empty = [&]() { return bin(Op::Eq, size(), konst(mk(0, 32))); };
                auto front = [&]() { auto x = node(Op::DynGet, {konst(mk(0, 32))}); x->sig = q; return x; };
                auto msg = [&]() {   // the ref formal `message`
                    const auto& f = *sub.sym->getArguments()[0];
                    if (isDynType(f.getType()) || f.getType().isUnpackedArray()) unsupported("a mailbox of arrays");
                    return sigAt(&f);
                };
                auto push = [&]() { Instr i = dynCmd(q, D_PUSH_BACK); i.e = arg(0); prog.emit(i); bump(chg); };
                auto pop = [&]() {
                    set(msg(), front());
                    Instr i = dynCmd(q, D_DELETE_AT); i.args = {konst(mk(0, 32))}; prog.emit(i);
                    bump(chg);
                };
                if (n == "new") set(thisRel(bound), arg(0));
                else if (n == "num") ret(size());
                else if (n == "put") { waitWhile(full, chg); push(); }
                else if (n == "try_put") tryDo(node(Op::LogNot, {full()}), push);
                else if (n == "get") { waitWhile(empty, chg); pop(); }
                else if (n == "try_get") tryDo(node(Op::LogNot, {empty()}), pop);
                else if (n == "peek") { waitWhile(empty, chg); set(msg(), front()); }
                else if (n == "try_peek") tryDo(node(Op::LogNot, {empty()}), [&]() { set(msg(), front()); });
                return;
            }
            case 4: {   // process
                static const std::pair<const char*, int> ops[] = {{"self", PO_SELF}, {"status", PO_STATUS},
                    {"kill", PO_KILL}, {"await", PO_AWAIT}, {"suspend", PO_SUSPEND}, {"resume", PO_RESUME}};
                for (auto& [s, op] : ops)
                    if (n == s) {
                        Instr i; i.k = IK::ProcOp; i.hi = op; i.lo = ci.id; i.dkey = ci.bslot;
                        if (op != PO_SELF) i.e = node(Op::This);
                        if (op == PO_SELF || op == PO_STATUS) i.sig = relSig(0, sub.retSig);
                        prog.emit(i);
                    }
                return;   // srandom / get_randstate / set_randstate: handled at the call (18.13.3)
            }
        }
        unsupported("built-in method");
    }
    // A constructor (8.7, 8.17): the base class's constructor (super.new,
    // written as the first statement or implied), then this class's property
    // initializers, then the rest of the body.
    const slang::ast::Expression* superNewDone = nullptr;
    void ctorBody(ClassInfo& ci, const slang::ast::Statement* body) {
        using namespace slang::ast;
        const Expression* explicitSuper = nullptr;
        if (body) {
            const Statement* st = body;
            while (true) {
                if (st->kind == StatementKind::Block) { st = &st->as<BlockStatement>().body; continue; }
                if (st->kind == StatementKind::List) {
                    const Statement* first = nullptr;
                    for (auto* x : st->as<StatementList>().list)
                        if (x->kind != StatementKind::VariableDeclaration) { first = x; break; }
                    if (!first) break;
                    st = first;
                    continue;
                }
                break;
            }
            if (st->kind == StatementKind::ExpressionStatement) {
                const Expression& x = st->as<ExpressionStatement>().expr;
                if (x.kind == ExpressionKind::NewClass && x.as<NewClassExpression>().isSuperClass) explicitSuper = &x;
            }
        }
        if (ci.base) {
            const Expression* call = nullptr;
            if (explicitSuper) call = explicitSuper->as<NewClassExpression>().constructorCall();
            else if (const Expression* bc = ci.ty->getBaseConstructorCall()) {
                if (bc->kind == ExpressionKind::NewClass) call = bc->as<NewClassExpression>().constructorCall();
                else if (bc->kind == ExpressionKind::Call) call = bc;
            }
            callCtor(*ci.base, call, nullptr);
        }
        for (const auto& m : ci.ty->members()) {
            if (m.kind != SymbolKind::ClassProperty) continue;
            const auto& p = m.as<ClassPropertySymbol>();
            if (p.lifetime == VariableLifetime::Static) continue;
            const Expression* init = p.getInitializer();
            if (!init) continue;
            const PropSlot& ps = propMap().at(&p);
            if (ps.kind == 2) { assignContainer(thisRel(ps.off), *init); continue; }
            if (ps.kind == 1) { assignWholeArray(p, p.getType(), *init, false); continue; }
            Instr i; i.k = IK::Assign; i.sig = thisRel(ps.off);
            i.e = isHandleType(p.getType()) ? exprH(*init) : expr(*init);
            prog.emit(i);
        }
        superNewDone = explicitSuper;
        if (body) stmt(*body);
        superNewDone = nullptr;
    }

    // ---------- expressions ----------
    // ctx.live: produce a value of that width. An outer conversion wins over
    // the self-determined widening slang inserts underneath it, so
    // `cnt <= cnt + 1` with cnt 4 bits is a 4-bit add, not a 32-bit add sliced
    // down. Add/sub/mul stay congruent mod 2^w, signed or not, once operands
    // are extended to that width. Sign-extension is not congruent that way
    // for shifts or compares, so those extend before the operator.
    // real (6.12): a real-typed expression is lowered by exprReal. Its
    // fallback lowers the same expression as plain bits once (rawNext).
    bool rawNext = false;
    static E realK(double d) { return konst(mkreal(d)); }
    E realOf(const slang::ast::Expression& e) {   // any expression, as a real
        if (e.type->isFloating()) return exprReal(e);
        return un(Op::IToR, expr(e));
    }
    E fcmp(int kind, E a, E b) { auto n = node(Op::FCmp, {std::move(a), std::move(b)}); n->hi = kind; return n; }
    E rtoi(E a, int w, bool sgn, bool trunc) {
        auto n = node(Op::RToI, {std::move(a)});
        n->hi = w; n->sgn = sgn; n->lo = trunc ? 1 : 0;
        return n;
    }
    static int mathId(std::string_view n) {
        static const std::pair<const char*, int> t[] = {
            {"$ln", M_LN}, {"$log10", M_LOG10}, {"$exp", M_EXP}, {"$sqrt", M_SQRT}, {"$pow", M_POW},
            {"$floor", M_FLOOR}, {"$ceil", M_CEIL}, {"$sin", M_SIN}, {"$cos", M_COS}, {"$tan", M_TAN},
            {"$asin", M_ASIN}, {"$acos", M_ACOS}, {"$atan", M_ATAN}, {"$atan2", M_ATAN2},
            {"$hypot", M_HYPOT}, {"$sinh", M_SINH}, {"$cosh", M_COSH}, {"$tanh", M_TANH},
            {"$asinh", M_ASINH}, {"$acosh", M_ACOSH}, {"$atanh", M_ATANH}};
        for (auto& [s, id] : t) if (n == s) return id;
        return -1;
    }
    E exprReal(const slang::ast::Expression& e) {
        if (e.type->getBitWidth() == 32) {   // shortreal (6.12): computed as a real, then rounded
            E r = exprReal64(e);
            if (r->op == Op::FMath && r->hi == M_F32) return r;
            auto m = node(Op::FMath, {r});
            m->hi = M_F32;
            return m;
        }
        return exprReal64(e);
    }
    E exprReal64(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (const auto* ps = paramMember(e); ps && ps->kind == SymbolKind::Parameter) {   // h.P (8.25)
            const slang::ConstantValue& cv = ps->as<ParameterSymbol>().getValue();
            if (cv.isReal()) return realK(cv.real());
            if (cv.isShortReal()) return realK(cv.shortReal());
        }
        switch (e.kind) {
            case ExpressionKind::RealLiteral: return realK(e.as<RealLiteral>().getValue());
            case ExpressionKind::TimeLiteral: return realK(e.as<TimeLiteral>().getValue());
            case ExpressionKind::Conversion: {
                const Expression& op = e.as<ConversionExpression>().operand();
                return realOf(op);
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                Op op;
                switch (b.op) {
                    case BinaryOperator::Add: op = Op::FAdd; break;
                    case BinaryOperator::Subtract: op = Op::FSub; break;
                    case BinaryOperator::Multiply: op = Op::FMul; break;
                    case BinaryOperator::Divide: op = Op::FDiv; break;
                    case BinaryOperator::Power: op = Op::FPow; break;
                    default: unsupported("operator on real operands");
                }
                return bin(op, realOf(b.left()), realOf(b.right()));
            }
            case ExpressionKind::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                if (u.op == UnaryOperator::Minus) return un(Op::FNeg, realOf(u.operand()));
                if (u.op == UnaryOperator::Plus) return realOf(u.operand());
                break;
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (E l = lazyExpr(e, ExprCtx{})) return l;   // an arm with a call: only the chosen one runs (11.4.11)
                return mux(condOf(c), realOf(c.left()), realOf(c.right()));
            }
            case ExpressionKind::Call: {
                const auto& call = e.as<CallExpression>();
                if (!call.isSystemCall()) {
                    if (E r = cgCall(call)) return r;   // get_coverage() is real (19.8)
                    break;
                }
                std::string_view n = call.getSubroutineName();
                auto args = call.arguments();
                if (auto vi = vpiSystfNames().find(std::string(n)); vi != vpiSystfNames().end() && vi->second.ftype == 2)
                    return un(Op::AsReal, emitVpiCall(call, vi->second, true));   // a VPI real function (38.37)
                if (n == "$get_coverage") return sysFnNode(SF_CG, {}, CG_GLOBAL);   // 19.9
                if (isStrMethodCall(call)) return strMethod(call, strMethodId(n));   // s.atoreal()
                if (n == "$realtime") { E t = node(Op::RealTime); t->k = mk(ticksPerUnit(), 64); return t; }
                if (n == "$itor" && args.size() == 1) return un(Op::IToR, expr(*args[0]));
                if (n == "$bitstoreal" && args.size() == 1) return un(Op::AsReal, expr(*args[0]));
                if (n == "$bitstoshortreal" && args.size() == 1) return sysFnNode(SF_BITS2SR, {expr(*args[0])});
                if (int id = mathId(n); id >= 0) {
                    std::vector<E> ks;
                    for (auto* a : args) ks.push_back(realOf(*a));
                    auto m = node(Op::FMath, std::move(ks));
                    m->hi = id;
                    return m;
                }
                break;
            }
            default: break;
        }
        rawNext = true;   // a real variable, array element, function result...
        return un(Op::AsReal, expr(e));
    }

    // string (6.16): a string-typed expression is lowered by exprStr.
    E strOf(const slang::ast::Expression& e) {   // a string, or an integral value as one
        if (e.type->isString()) return exprStr(e);
        const auto& p = peel(e);   // a literal keeps its text, whatever its length
        if (p.kind == slang::ast::ExpressionKind::StringLiteral)
            return konst(mkstr(std::string(p.as<slang::ast::StringLiteral>().getValue())));
        if (E j = streamStrings(p)) return j;
        if (p.kind == slang::ast::ExpressionKind::Concatenation) {   // {"ab", "cd"} given to a string
            bool lits = true;
            for (auto* op : p.as<slang::ast::ConcatenationExpression>().operands())
                if (peel(*op).kind != slang::ast::ExpressionKind::StringLiteral && !op->type->isString()) lits = false;
            if (lits) {
                std::vector<E> parts;
                for (auto* op : p.as<slang::ast::ConcatenationExpression>().operands()) parts.push_back(strOf(*op));
                return node(Op::StrCat, std::move(parts));
            }
        }
        return un(Op::IntToStr, expr(e));
    }
    // {>>{s, q}} of strings and string containers (11.4.14): their text, joined.
    E streamStrings(const slang::ast::Expression& p) {
        using namespace slang::ast;
        if (p.kind != ExpressionKind::Streaming) return nullptr;
        const auto& sc = p.as<StreamingConcatenationExpression>();
        if (!sc.isFixedSize() && false) return nullptr;
        if (sc.getSliceSize() != 0) return nullptr;
        std::vector<E> parts;
        for (auto& st : sc.streams()) {
            const Expression& op = peel(*st.operand);
            if (st.withExpr) return nullptr;
            if (op.type->isString()) { parts.push_back(exprStr(op)); continue; }
            if (isDynType(*op.type) && op.type->getArrayElementType() && op.type->getArrayElementType()->isString()) {
                int d = dynSig(op);
                if (d < 0) return nullptr;
                auto j = node(Op::DynStrJoin);
                j->sig = d;
                parts.push_back(j);
                continue;
            }
            return nullptr;
        }
        return node(Op::StrCat, std::move(parts));
    }
    static int strMethodId(std::string_view n) {
        static const std::pair<const char*, int> t[] = {
            {"len", S_LEN}, {"getc", S_GETC}, {"toupper", S_TOUPPER}, {"tolower", S_TOLOWER},
            {"compare", S_COMPARE}, {"icompare", S_ICOMPARE}, {"substr", S_SUBSTR}, {"atoi", S_ATOI},
            {"atohex", S_ATOHEX}, {"atooct", S_ATOOCT}, {"atobin", S_ATOBIN}, {"atoreal", S_ATOREAL},
            {"putc", S_PUTC}, {"itoa", S_ITOA}, {"hextoa", S_HEXTOA}, {"octtoa", S_OCTTOA},
            {"bintoa", S_BINTOA}, {"realtoa", S_REALTOA}};
        for (auto& [s, id] : t) if (n == s) return id;
        return -1;
    }
    // s.len(), s.substr(i, j), ...: kid 0 is the string, then the arguments.
    // ---- array query functions (20.7) ----
    static bool isArrayQuery(std::string_view n) {
        return n == "$left" || n == "$right" || n == "$low" || n == "$high" || n == "$increment" || n == "$size" ||
               n == "$dimensions" || n == "$unpacked_dimensions";
    }
    // The type as $typename writes it (20.6.1): typedefs resolved, no
    // default signing, ranges as decimal numbers.
    static std::string typeName(const slang::ast::Type& t) {
        std::string s = t.getCanonicalType().toString();
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {   // "logic [3:0]" -> "logic[3:0]"
            if (s[i] == ' ' && i + 1 < s.size() && s[i + 1] == '[') continue;
            out += s[i];
        }
        return out;
    }
    struct QDim { int kind; int64_t l, r; };   // kind: 0 fixed, 1 dynamic array or queue, 2 associative
    static void queryDims(const slang::ast::Type& t0, std::vector<QDim>& dims, int& unpacked, int64_t& assocMax) {
        using namespace slang::ast;
        const Type* t = &t0.getCanonicalType();
        unpacked = 0;
        assocMax = 0;
        for (;;) {
            if (t->kind == SymbolKind::FixedSizeUnpackedArrayType) {
                const auto& f = t->as<FixedSizeUnpackedArrayType>();
                dims.push_back({0, f.range.left, f.range.right});
                t = &f.elementType.getCanonicalType();
            } else if (t->kind == SymbolKind::DynamicArrayType) {
                dims.push_back({1, 0, 0});
                t = &t->as<DynamicArrayType>().elementType.getCanonicalType();
            } else if (t->kind == SymbolKind::QueueType) {
                dims.push_back({1, 0, 0});
                t = &t->as<QueueType>().elementType.getCanonicalType();
            } else if (t->kind == SymbolKind::AssociativeArrayType) {
                const auto& a = t->as<AssociativeArrayType>();
                if (a.indexType && a.indexType->isIntegral()) {
                    int kw = (int)a.indexType->getBitWidth();
                    bool ks = a.indexType->isSigned();
                    assocMax = kw >= 64 ? INT64_MAX : ks ? ((int64_t)1 << (kw - 1)) - 1 : ((int64_t)1 << kw) - 1;
                } else assocMax = -1;
                dims.push_back({2, 0, 0});
                t = &a.elementType.getCanonicalType();
            } else break;
            unpacked++;
        }
        if (t->kind == SymbolKind::EnumType) t = &t->as<EnumType>().baseType.getCanonicalType();
        while (t->kind == SymbolKind::PackedArrayType) {
            const auto& p = t->as<PackedArrayType>();
            dims.push_back({0, p.range.left, p.range.right});
            t = &p.elementType.getCanonicalType();
            if (t->kind == SymbolKind::EnumType) t = &t->as<EnumType>().baseType.getCanonicalType();
        }
        if (t->kind == SymbolKind::PredefinedIntegerType || t->kind == SymbolKind::PackedStructType || t->kind == SymbolKind::PackedUnionType)
            dims.push_back({0, (int64_t)t->getBitWidth() - 1, 0});   // int is [31:0] (20.7)
    }
    E arrayQuery(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (args.empty()) unsupported("array query without an argument");
        auto num = [](int64_t v) { Val r = make((uint64_t)v, 0, 32); r.sgn = true; return konst(r); };
        E xv = konst(xfill(32, true));
        std::vector<QDim> dims;
        int unpacked = 0;
        int64_t assocMax = 0;
        const Type& t = *args[0]->type;
        queryDims(t, dims, unpacked, assocMax);
        if (n == "$dimensions") {
            if (dims.empty() && (t.isString() || t.isIntegral())) return num(t.isString() ? 1 : 1);
            return num((int64_t)dims.size());
        }
        if (n == "$unpacked_dimensions") return num(unpacked);
        if (dims.empty() && t.isIntegral() && t.getBitWidth() == 1) dims.push_back({0, 0, 0});   // a scalar
        int64_t d = 1;
        if (args.size() > 1) {
            const auto* cv = args[1]->getConstant();
            bool known = (cv && cv->isInteger() && !cv->integer().hasUnknown()) || args[1]->kind == ExpressionKind::IntegerLiteral;
            if (!known) {   // a dimension known only at run time: pick among the answers
                E dv = resize(expr(*args[1]), 64, args[1]->type->isSigned(), true);
                E r = xv;
                for (size_t k = dims.size(); k-- > 0;) {
                    if (dims[k].kind != 0 && k != 0) continue;
                    Val kk = make(k + 1, 0, 64); kk.sgn = true;
                    r = mux(bin(Op::Eq, dv, konst(kk)), queryDim(args, n, dims[k], (int64_t)k + 1, assocMax), r);
                }
                return r;
            }
            d = constInt(*args[1]);
        }
        if (d < 1 || d > (int64_t)dims.size()) return xv;
        return queryDim(args, n, dims[(size_t)d - 1], d, assocMax);
    }
    E queryDim(std::span<const slang::ast::Expression* const> args, std::string_view n, const QDim& q, int64_t d,
               int64_t assocMax) {
        auto num = [](int64_t v) { Val r = make((uint64_t)v, 0, 32); r.sgn = true; return konst(r); };
        E xv = konst(xfill(32, true));
        if (q.kind == 0) {
            int64_t l = q.l, r = q.r, inc = l >= r ? 1 : -1;
            int64_t lo = std::min(l, r), hi = std::max(l, r);
            if (n == "$left") return num(l);
            if (n == "$right") return num(r);
            if (n == "$low") return num(lo);
            if (n == "$high") return num(hi);
            if (n == "$increment") return num(inc);
            return num(hi - lo + 1);
        }
        int h = d == 1 ? dynSig(*args[0]) : -1;
        if (h < 0) unsupported("array query of a dynamic dimension that is not a variable's first dimension");
        E size = dynSize(h);
        Val one = make(1, 0, 32); one.sgn = true;
        if (n == "$increment") return num(-1);
        if (n == "$size") return size;
        if (q.kind == 1) {   // dynamic array or queue: [0 : size-1]
            if (n == "$left" || n == "$low") return num(0);
            return bin(Op::Sub, size, konst(one));
        }
        if (n == "$left") return num(0);   // associative (20.7)
        if (n == "$right") return assocMax < 0 ? xv : num(assocMax);
        auto b = node(Op::SysFn);
        b->hi = SF_ASSOC_BOUND;
        b->lo = n == "$high" ? 1 : 0;
        b->sig = h;
        return b;
    }

    E strMethod(const slang::ast::CallExpression& call, int id) {
        auto args = call.arguments();
        std::vector<E> ks;
        ks.push_back(exprStr(*args[0]));
        for (size_t a = 1; a < args.size(); a++) {
            if (id == S_COMPARE || id == S_ICOMPARE) ks.push_back(strOf(*args[a]));
            else if (id == S_REALTOA) ks.push_back(realOf(*args[a]));
            else ks.push_back(expr(*args[a]));
        }
        auto m = node(Op::StrMethod, std::move(ks));
        m->hi = id;
        return m;
    }
    bool isStrMethodCall(const slang::ast::CallExpression& call) {
        return call.isSystemCall() && !call.arguments().empty() && call.arguments()[0]->type->isString() &&
               strMethodId(call.getSubroutineName()) >= 0 && call.getSubroutineName()[0] != '$';
    }
    E exprStr(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (const auto* ps = paramMember(e)) return constSym(*ps, e, ExprCtx{});   // h.P (8.25)
        switch (e.kind) {
            case ExpressionKind::StringLiteral:
                return konst(mkstr(std::string(e.as<StringLiteral>().getValue())));
            case ExpressionKind::Conversion: return strOf(e.as<ConversionExpression>().operand());
            case ExpressionKind::Concatenation: {
                std::vector<E> parts;
                for (auto* op : e.as<ConcatenationExpression>().operands()) parts.push_back(strOf(*op));
                return node(Op::StrCat, std::move(parts));
            }
            case ExpressionKind::Replication: {
                const auto& r = e.as<ReplicationExpression>();
                auto n = node(Op::StrRep, {strOf(r.concat())});
                if (const auto* cv = r.count().getConstant(); cv && cv->isInteger()) n->hi = (int)constInt(r.count());
                else n->kids.push_back(expr(r.count()));   // {n{s}} with n a variable (11.4.12.2)
                return n;
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (E l = lazyExpr(e, ExprCtx{})) return l;   // an arm with a call: only the chosen one runs (11.4.11)
                return mux(condOf(c), strOf(c.left()), strOf(c.right()));
            }
            case ExpressionKind::Call: {
                const auto& call = e.as<CallExpression>();
                if (!call.isSystemCall()) break;
                std::string_view n = call.getSubroutineName();
                if (int ix = iterIndexSig(call); ix >= 0) return un(Op::AsStr, ref(ix));   // item.index of a string key
                if (n == "$sformatf" || n == "$psprintf") {
                    auto args = call.arguments();
                    auto f = node(Op::SFormat);
                    hierName = scopeName(call); libName = libCell(call);
                    formatInto(args, 'd', f->fmt, f->kids);
                    return f;
                }
                if (isStrMethodCall(call)) return strMethod(call, strMethodId(n));
                if (E r = enumMethod(call)) return r;   // e.name()
                break;
            }
            default: break;
        }
        rawNext = true;   // a string variable, array element, function result...
        return un(Op::AsStr, expr(e));
    }

    // Does x call a user subroutine or make an object? Such an operand of
    // ?:, && or || runs only when it is selected (11.4.7, 11.4.11), so it
    // is lowered with jumps instead of as one expression.
    static bool sideEffects(const slang::ast::Expression& x) {
        using namespace slang::ast;
        struct V : ASTVisitor<V, VisitFlags::Expressions> {
            bool found = false;
            // A user subroutine, or a container method that changes the container
            // (q.pop_front() is a system call to slang): it must run only in the
            // arm that is chosen, after the condition (11.4.11).
            void handle(const CallExpression& c) {
                auto n = c.getSubroutineName();
                if (!c.isSystemCall() || n == "pop_front" || n == "pop_back") found = true;
                else visitDefault(c);
            }
            void handle(const AssignmentExpression&) { found = true; }   // c ? i = 1 : 0
            void handle(const UnaryExpression& u) {   // c ? i++ : i
                if (OpInfo::isLValue(u.op)) found = true; else visitDefault(u);
            }
            void handle(const NewClassExpression&) { found = true; }
            void handle(const CopyClassExpression&) { found = true; }
        } v;
        x.visit(v);
        return v.found;
    }
    // The value of e as a new temporary, by its type.
    E asType(const slang::ast::Type& t, int sig, bool sgn) {
        if (t.isFloating()) return un(Op::AsReal, ref(sig));
        if (t.isString()) return un(Op::AsStr, ref(sig));
        return ref(sig, sgn);
    }
    E lowerAs(const slang::ast::Expression& x, const slang::ast::Type& t, ExprCtx ctx) {
        if (isHandleType(t)) return exprH(x);
        if (t.isFloating()) return realOf(x);
        if (t.isString()) return exprStr(x);
        return expr(x, ctx);
    }
    // ---------- condition coverage (msim -cov) ----------
    // On while a procedure, a continuous assignment or a subroutine body is
    // lowered; off inside constraints, assertions and covergroups, whose
    // expressions other passes take apart.
    bool condCov = false;
    int condCovOff = 0;
    struct NoCondCov { int& n; explicit NoCondCov(int& x) : n(x) { n++; } ~NoCondCov() { n--; } };
    // Evaluating e reads signals and nothing else: no call, no handle that
    // may be null, no container. Every term of a covered condition is
    // evaluated, also the ones && and || would skip.
    static bool condPure(const E& e) {
        if (!e || e->at) return false;
        switch (e->op) {
            case Op::Const: case Op::Ref: case Op::Not: case Op::And: case Op::Or: case Op::Xor: case Op::Add: case Op::Sub:
            case Op::Mul: case Op::Div: case Op::Mod: case Op::Eq: case Op::Ne: case Op::CaseEq: case Op::CaseZEq:
            case Op::CaseXEq: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Shl: case Op::Shr: case Op::Asr:
            case Op::Resize: case Op::LogNot: case Op::LogAnd: case Op::LogOr: case Op::RedAnd: case Op::RedOr:
            case Op::RedXor: case Op::Slice: case Op::Index: case Op::Concat: case Op::Mux: case Op::WildEq:
            case Op::CondCov: case Op::ArrRead:
                break;
            default: return false;
        }
        for (auto& k : e->kids) if (!condPure(k)) return false;
        return true;
    }
    static std::string condText(const slang::ast::Expression& x) {
        if (!x.syntax) return "?";
        std::string t = x.syntax->toString(), r;
        bool sp = false;
        for (char c : t) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = !r.empty(); continue; }
            if (sp) r += ' ';
            sp = false;
            r += c;
        }
        return r;
    }
    // e is a && b, a || b, or the condition of ?: . The covered form of it,
    // or null when it is not covered: coverage is off here, it is constant,
    // or a term has a side effect or is not an integral value.
    E condCovExpr(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (!g_cov || !condCov || condCovOff > 0 || !covSM()) return nullptr;
        const Expression& top = peel(e);
        bool andOp = top.kind == ExpressionKind::BinaryOp && top.as<BinaryExpression>().op == BinaryOperator::LogicalAnd;
        bool orOp = top.kind == ExpressionKind::BinaryOp && top.as<BinaryExpression>().op == BinaryOperator::LogicalOr;
        // -cm_cond allops: a & b and a | b of one-bit unsigned operands are conditions too
        auto bit1 = [](const Expression& x) {
            return x.type->isIntegral() && !x.type->isSigned() && x.type->getBitWidth() == 1;
        };
        BinaryOperator bop = andOp ? BinaryOperator::LogicalAnd : BinaryOperator::LogicalOr;
        bool bitwise = false;
        if (g_condAllops && top.kind == ExpressionKind::BinaryOp && !andOp && !orOp) {
            const auto& b = top.as<BinaryExpression>();
            if ((b.op == BinaryOperator::BinaryAnd || b.op == BinaryOperator::BinaryOr) && bit1(top) && bit1(b.left()) &&
                bit1(b.right())) {
                bitwise = true;
                bop = b.op;
                (b.op == BinaryOperator::BinaryAnd ? andOp : orOp) = true;
            }
        }
        if (&top != &e && (andOp || orOp)) return nullptr;   // c of c ? a : b is itself a && b: covered as that
        if (e.getConstant()) return nullptr;
        std::vector<const Expression*> terms;
        // (a && f()) && b: the inner && is lowered as jumps (lazyExpr), so it stays one term
        auto flat = [&](auto&& self, const Expression& x) -> void {
            if (x.kind == ExpressionKind::BinaryOp && (andOp || orOp) && x.as<BinaryExpression>().op == bop &&
                (!bitwise || (bit1(x.as<BinaryExpression>().left()) && bit1(x.as<BinaryExpression>().right()))) &&
                !sideEffects(x.as<BinaryExpression>().right())) {
                self(self, x.as<BinaryExpression>().left());
                self(self, x.as<BinaryExpression>().right());
            } else terms.push_back(&x);
        };
        flat(flat, top);
        if (terms.size() > 64) return nullptr;
        for (const Expression* t : terms)
            if (!t->type->isIntegral() || isHandleType(*t->type) || t->getConstant()) return nullptr;
        std::vector<E> kids;
        bool pure = true;
        for (const Expression* t : terms) {
            kids.push_back(expr(*t));
            pure = pure && condPure(kids.back());
        }
        // A term that is not safe to evaluate when && or || would skip it:
        // the terms in order, and the vectors of condCovLazyEnd.
        bool lazy = !pure && terms.size() > 1;
        if (bitwise && lazy) return nullptr;   // & and | evaluate every operand: one with a side effect is not ours to repeat
        bool full = g_condFull && !lazy && terms.size() > 1 && terms.size() <= 8;
        std::vector<std::string> texts;
        for (const Expression* t : terms) texts.push_back(condText(*t));
        auto n = node(Op::CondCov, std::move(kids));
        n->hi = (int)condDef(e, orOp, lazy, std::move(texts), full, bitwise);
        n->lo = (orOp ? 1 : 0) | (lazy ? 2 : 0) | (full ? 4 : 0);
        return n;
    }
    // Registers a condition; returns the index of its first counter.
    size_t condDef(const slang::ast::Expression& e, bool orOp, bool lazy, std::vector<std::string> terms, bool full = false,
                   bool bitwise = false) {
        CondDef d;
        auto loc = e.sourceRange.start();
        if (loc.valid()) {
            auto ol = covSM()->getFullyOriginalLoc(loc);
            d.file = covFileId(std::string(covSM()->getFullPath(ol.buffer()).string()));
            d.line = (uint32_t)covSM()->getLineNumber(ol);
            d.col = (uint32_t)covSM()->getColumnNumber(ol);
        }
        d.inst = g_covInst;
        d.isOr = orOp;
        d.lazy = lazy;
        d.full = full;
        d.bitwise = bitwise;
        d.text = condText(e);
        d.terms = std::move(terms);
        d.base = g_condHits.size();
        g_condHits.resize(d.base + (full ? (size_t)1 << d.terms.size() : d.terms.size() + 1), 0);
        condDefs().push_back(std::move(d));
        return condDefs().back().base;
    }
    // A procedural checker instance (17.3): the process reaching it queues
    // the checker's static assertions, concurrent ones as procedural
    // concurrent assertions (16.14.6) and deferred ones as procedural
    // deferred assertions (16.4), with the procedure's clock and automatic
    // values. Those of checkers it instantiates statically go the same way.
    // Everything else in the checker runs on its own (ProcCollector).
    void procCheckerAsserts(const slang::ast::Scope& body) {
        using namespace slang::ast;
        const Scope* savedScope = scope;
        scope = &body;
        for (const Symbol& m : body.members()) {
            if (m.kind == SymbolKind::ProceduralBlock && m.as<ProceduralBlockSymbol>().isFromAssertion)
                stmt(m.as<ProceduralBlockSymbol>().getBody());
            else if (m.kind == SymbolKind::CheckerInstance)
                procCheckerAsserts(m.as<CheckerInstanceSymbol>().body);
            else if (m.kind == SymbolKind::GenerateBlock && !m.as<GenerateBlockSymbol>().isUninstantiated)
                procCheckerAsserts(m.as<GenerateBlockSymbol>());
            else if (m.kind == SymbolKind::GenerateBlockArray)
                for (const auto* e : m.as<GenerateBlockArraySymbol>().entries)
                    if (!e->isUninstantiated) procCheckerAsserts(*e);
        }
        scope = savedScope;
    }
    // The condition of c ? a : b, or of x matches p &&& g ? a : b (12.6.2):
    // each part in turn, pattern variables bound as they are matched.
    E condOf(const slang::ast::ConditionalExpression& c) {
        if (c.conditions.size() == 1 && !c.conditions[0].pattern) {
            if (E k = condCovExpr(*c.conditions[0].expr)) return k;
            return expr(*c.conditions[0].expr);
        }
        E out;
        for (auto& cd : c.conditions) {
            E x = cd.pattern ? matchExpr(*cd.expr, *cd.pattern, slang::ast::CaseStatementCondition::Normal)
                             : un(Op::LogNot, un(Op::LogNot, expr(*cd.expr)));
            out = out ? bin(Op::LogAnd, out, x) : x;
        }
        return out;
    }
    // h.P, a class parameter or localparam read through a handle (8.25), or
    // an enum value named that way: the constant, whatever h holds.
    static const slang::ast::Symbol* paramMember(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (e.kind != ExpressionKind::MemberAccess) return nullptr;
        const Symbol& m = e.as<MemberAccessExpression>().member;
        return m.kind == SymbolKind::Parameter || m.kind == SymbolKind::EnumValue ? &m : nullptr;
    }
    E lazyExpr(const slang::ast::Expression& e, ExprCtx ctx) {
        using namespace slang::ast;
        if (e.kind == ExpressionKind::ConditionalOp) {
            const auto& c = e.as<ConditionalExpression>();
            if (!sideEffects(c.left()) && !sideEffects(c.right())) return nullptr;
            int w = isHandleType(*e.type) || e.type->isFloating() || e.type->isString() ? 64
                  : ctx.live ? ctx.w : (int)e.type->getBitWidth();
            needWidth(w);
            bool sg = ctx.live ? ctx.tag : e.type->isSigned();
            int tmp = fresh(w);
            Instr j; j.k = IK::JmpIfNot; j.e = condOf(c);
            size_t jf = prog.emit(j);
            Instr a; a.k = IK::Assign; a.sig = tmp; a.e = lowerAs(c.left(), *e.type, ctx); prog.emit(a);
            size_t je = emitJmp();
            prog.code[jf].target = prog.here();
            Instr b; b.k = IK::Assign; b.sig = tmp; b.e = lowerAs(c.right(), *e.type, ctx); prog.emit(b);
            prog.code[je].target = prog.here();
            return asType(*e.type, tmp, sg);
        }
        if (e.kind == ExpressionKind::BinaryOp) {
            const auto& b = e.as<BinaryExpression>();
            bool andOp = b.op == BinaryOperator::LogicalAnd, orOp = b.op == BinaryOperator::LogicalOr;
            if ((!andOp && !orOp) || !sideEffects(b.right())) return nullptr;
            auto truthE = [&](const Expression& x) {
                E v = isHandleType(*x.type) ? exprH(x) : x.type->isFloating() ? realOf(x) : expr(x);
                return un(Op::LogNot, un(Op::LogNot, v));
            };
            int tmp = fresh(1);
            Instr l; l.k = IK::Assign; l.sig = tmp; l.e = truthE(b.left()); prog.emit(l);
            // Condition coverage: the left term is kept, and after the right
            // one ran (or was skipped) the pair is counted as a lazy
            // condition of (left, result), which has the same vectors.
            bool cover = g_cov && condCov && condCovOff == 0 && covSM() && b.left().type->isIntegral() &&
                         b.right().type->isIntegral() && !isHandleType(*b.left().type) && !isHandleType(*b.right().type);
            int left = -1;
            if (cover) {
                left = fresh(1);
                Instr s; s.k = IK::Assign; s.sig = left; s.e = ref(tmp); prog.emit(s);
            }
            Instr j; j.k = IK::JmpIfNot; j.e = andOp ? ref(tmp) : un(Op::LogNot, ref(tmp));
            size_t jf = prog.emit(j);
            Instr r; r.k = IK::Assign; r.sig = tmp; r.e = bin(andOp ? Op::LogAnd : Op::LogOr, ref(tmp), truthE(b.right()));
            prog.emit(r);
            prog.code[jf].target = prog.here();
            if (cover) {
                auto n = node(Op::CondCov, {ref(left), ref(tmp)});
                n->hi = (int)condDef(e, orOp, true, {condText(b.left()), condText(b.right())});
                n->lo = (orOp ? 1 : 0) | 2;
                Instr c; c.k = IK::Assign; c.sig = tmp; c.e = n; prog.emit(c);
            }
            return sized(ref(tmp), 1, false, ctx);
        }
        return nullptr;
    }
    // A modport port with an expression (.P(r[3:0]), 25.5.4): that expression.
    static const slang::ast::Expression* modportConn(const slang::ast::Expression& e) {
        using namespace slang::ast;
        if (e.kind != ExpressionKind::NamedValue && e.kind != ExpressionKind::HierarchicalValue) return nullptr;
        const Symbol& s = e.as<ValueExpressionBase>().symbol;
        if (s.kind != SymbolKind::ModportPort) return nullptr;
        return s.as<ModportPortSymbol>().explicitConnection;
    }
    E expr(const slang::ast::Expression& e, ExprCtx ctx = {}) {
        using namespace slang::ast;
        if (const Expression* c = modportConn(e)) return expr(*c, ctx);
        if (!exprOverride.empty())
            if (auto ov = exprOverride.find(&e); ov != exprOverride.end())
                return sized(ref(ov->second, e.type->isSigned()), (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
        if (e.kind == ExpressionKind::NamedValue && !labelled().empty())   // a LABEL'd specparam (32.4.3)
            if (auto lv = labelled().find(&e.as<NamedValueExpression>().symbol); lv != labelled().end()) {
                if (e.type->isFloating()) return konst(mkreal(lv->second));
                int w = (int)e.type->getBitWidth();
                Val k = make((uint64_t)std::llround(lv->second), 0, w, 0, e.type->isSigned());
                return sized(konst(k), w, e.type->isSigned(), ctx);
            }
        if (const auto* ps = paramMember(e); ps && !e.type->isFloating()) return constSym(*ps, e, ctx);   // h.P (8.25)
        if (!rawNext)
            if (E l = lazyExpr(e, ctx)) return l;
        bool raw = rawNext;
        rawNext = false;
        if (isHandleType(*e.type)) return exprH(e);
        if (!raw && e.type->isFloating()) return exprReal(e);
        if (!raw && e.type->isString()) return exprStr(e);
        switch (e.kind) {
            case ExpressionKind::IntegerLiteral:
                return svLit(e.as<IntegerLiteral>().getValue(), e.type->isSigned(), ctx);
            case ExpressionKind::NullLiteral: {     // a chandle's null (6.14)
                Val z = make(0, 0, ctx.live && ctx.w > 0 ? ctx.w : 64);
                return konst(z);
            }
            case ExpressionKind::UnbasedUnsizedIntegerLiteral: {   // '0 '1 'x 'z fill their context (5.7.1)
                auto b = e.as<UnbasedUnsizedIntegerLiteral>().getLiteralValue();
                int w = ctx.w > 0 ? ctx.w : (int)e.type->getBitWidth();
                if (w <= 0) w = 1;
                needWidth(w);
                int bit = b.value == slang::logic_t::Z_VALUE ? 3 : b.isUnknown() ? 2 : b.value ? 1 : 0;
                Val v;
                if (w <= 64) {
                    uint64_t m = mask(w);
                    v = make(bit == 1 ? m : 0, bit >= 2 ? m : 0, w, bit == 3 ? m : 0, false);
                } else {
                    v = make(0, 0, w, 0, false);
                    for (int k = 0; k < w; k++) setBit(v, k, bit);
                    v = finish(v);
                }
                return sized(konst(v), w, false, ctx);
            }
            case ExpressionKind::StringLiteral: {   // "abc" is 8 bits per char (5.9)
                const auto& sv = e.as<StringLiteral>().getIntValue().integer();
                return svLit(sv, false, ctx);
            }
            case ExpressionKind::Replication: {     // {n{a}} (11.4.12.1)
                const auto& r = e.as<ReplicationExpression>();
                int64_t n = constInt(r.count());
                if (n < 0) unsupported("replication count");
                if (n == 0) unsupported("zero replication");
                E one = expr(r.concat());
                std::vector<E> parts((size_t)n, one);
                return sized(concat(parts), (int)e.type->getBitWidth(), false, ctx);
            }
            default: break;
        }
        return expr2(e, ctx);
    }

    // A constant from slang's 4-state integer, at its own width and sign.
    E svLit(const slang::SVInt& sv, bool self, ExprCtx ctx) {
        int w = (int)sv.getBitWidth();
        if (w <= 0) unsupported("constant");
        E lit;
        if (w <= 64) {
            if (sv.hasUnknown()) {   // x / z / ? bits
                uint64_t v = 0, x = 0, z = 0;
                for (int b = 0; b < w && b < 64; b++) {
                    auto bit = sv[b];
                    if (bit.isUnknown()) {
                        x |= 1ULL << b;
                        if (bit.value == slang::logic_t::Z_VALUE) z |= 1ULL << b;
                    } else if (bit.value) {
                        v |= 1ULL << b;
                    }
                }
                lit = konst(make(v, x, w, z, self));
            } else {
                lit = konst(make(sv.getRawPtr()[0], 0, w, 0, self));
            }
            return sized(lit, w, self, ctx);
        }
        // Wider than one limb. A known value is little-endian words. An
        // unknown value is walked a bit at a time: slang stores Z as a 1 in
        // the value word and a 1 in the unknown word, X as a 0 and a 1.
        Val bits = make(0, 0, w, 0, self);
        if (!sv.hasUnknown()) {
            bits = wordsToVal(w, self, sv.getRawPtr(), (w + 63) / 64);
        } else {
            for (int b = 0; b < w; b++) {
                auto bit = sv[b];
                if (bit.isUnknown())
                    setBit(bits, b, bit.value == slang::logic_t::Z_VALUE ? 3 : 2);
                else if (bit.value)
                    setBit(bits, b, 1);
            }
            bits = finish(bits);
            bits.sgn = self;
        }
        return sized(konst(bits), w, self, ctx);
    }

    // The symbol a select chain (a[i][j], a.f[k]) starts from, or null.
    static const slang::ast::Symbol* selectBase(const slang::ast::Expression& e0) {
        using namespace slang::ast;
        const Expression* e = &e0;
        for (;;) {
            if (e->kind == ExpressionKind::ElementSelect) e = &e->as<ElementSelectExpression>().value();
            else if (e->kind == ExpressionKind::RangeSelect) e = &e->as<RangeSelectExpression>().value();
            else if (e->kind == ExpressionKind::MemberAccess) e = &e->as<MemberAccessExpression>().value();
            else if (e->kind == ExpressionKind::Conversion) e = &e->as<ConversionExpression>().operand();
            else break;
        }
        if (e->kind == ExpressionKind::NamedValue || e->kind == ExpressionKind::HierarchicalValue)
            return &e->as<ValueExpressionBase>().symbol;
        return nullptr;
    }
    // Evaluates a constant expression that slang did not fold, such as an
    // element of a parameter array or a constant function call (11.2.1,
    // 13.4.3). Returns a bad value when the expression is not constant.
    slang::ConstantValue tryConst(const slang::ast::Expression& e, const slang::ast::Symbol* anchor = nullptr) const {
        using namespace slang::ast;
        if (const auto* c = e.getConstant()) return *c;
        if (anchor && anchor->getParentScope()) {
            EvalContext ec(*anchor);
            return e.eval(ec);
        }
        if (!scope) return {};
        EvalContext ec(ASTContext(*scope, LookupLocation::max));
        return e.eval(ec);
    }
    // A constant unpacked array value, one entry per element, leftmost first.
    void constElems(const slang::ast::Type& t0, const slang::ConstantValue& cv, std::vector<E>& out) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        if (t.kind == SymbolKind::FixedSizeUnpackedArrayType) {
            const Type& et = t.as<FixedSizeUnpackedArrayType>().elementType;
            if (!cv.isUnpacked()) unsupported("constant array value");
            for (const auto& el : cv.elements()) constElems(et, el, out);
            return;
        }
        if (cv.isInteger()) {
            out.push_back(resize(svLit(cv.integer(), t.isSigned(), {}), (int)t.getBitWidth(),
                                 cv.integer().isSigned(), t.isSigned()));
            return;
        }
        if (cv.isReal()) { out.push_back(konst(mkreal(cv.real()))); return; }
        unsupported("constant array element of this kind");
    }

    // P[i][j:k] where P is a parameter unpacked array and every index is
    // constant: the selected value. Null when this is not such a select.
    E paramSelect(const slang::ast::Expression& e, ExprCtx ctx) {
        using namespace slang::ast;
        const Symbol* b = selectBase(e);
        if (!b || b->kind != SymbolKind::Parameter || !b->as<ParameterSymbol>().getType().isUnpackedArray()) return nullptr;
        auto cv = tryConst(e, b);
        if (!cv || !cv.isInteger()) unsupported("non-constant select of a parameter unpacked array");
        return svLit(cv.integer(), e.type->isSigned(), ctx);
    }
    // a[i][j] on an unpacked array: the array and one index per dimension.
    // Fewer indices than dimensions (a slice) is not supported yet.
    struct ArrSel {
        std::shared_ptr<const ArrShape> a;
        std::vector<const slang::ast::Expression*> idx;
        bool constant = true;
    };
    std::optional<ArrSel> arraySel(const slang::ast::Expression& e, bool partial = false) {
        using namespace slang::ast;
        std::vector<const Expression*> idx;
        const Expression* cur = &e;
        while (cur->kind == ExpressionKind::ElementSelect &&
               cur->as<ElementSelectExpression>().value().type->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType) {
            idx.push_back(&cur->as<ElementSelectExpression>().selector());
            cur = &cur->as<ElementSelectExpression>().value();
        }
        if (idx.empty()) return std::nullopt;
        std::shared_ptr<const ArrShape> shape;
        if (auto va = vifArr(*cur)) shape = va;   // v.a[i], v.cb.a[i] (25.9)
        else if (classMember(*cur)) {   // h.a[i] (8.4): the array's first slot through the handle
            const auto& m = cur->as<MemberAccessExpression>();
            const PropSlot* ps = memberSlot(m);
            if (!ps) shape = arrFind(&m.member);
            else {
                if (ps->kind != 1) unsupported("array select on this property");
                auto a = std::make_shared<ArrShape>(*ps->arr);
                a->base = indSlot(exprH(m.value()), ps->arr->base);
                shape = a;
            }
        } else {
            if (cur->kind == ExpressionKind::MemberAccess || cur->kind == ExpressionKind::ElementSelect)
                return std::nullopt;   // s.arr[i]: an array inside an unpacked struct is bits of it (7.2)
            if (cur->kind != ExpressionKind::NamedValue && cur->kind != ExpressionKind::HierarchicalValue)
                unsupported("array select on a non-variable");
            shape = arrFind(&cur->as<ValueExpressionBase>().symbol);   // m[i] or top.u.m[i] (23.6)
        }
        if (!shape && ValueExpressionBase::isKind(cur->kind) && sigFind(&cur->as<ValueExpressionBase>().symbol) >= 0)
            return std::nullopt;   // an array held packed in one signal (a function result): bits of it
        if (!shape) unsupported("array select");
        ArrSel s;
        s.a = shape;
        s.idx.assign(idx.rbegin(), idx.rend());
        if (s.idx.size() > s.a->dims.size() || (!partial && s.idx.size() != s.a->dims.size()))
            unsupported("array slice (fewer indices than dimensions)");
        for (auto* x : s.idx)
            if (!(x->kind == ExpressionKind::IntegerLiteral || x->getConstant())) s.constant = false;
        return s;
    }
    // m[i] of a multidimensional array (7.4.5): the index lists of the
    // elements of that sub-array, in row-major order. The given indices come
    // first; the rest run over the remaining dimensions, left to right.
    std::vector<std::vector<E>> subArrayIdx(const ArrSel& s) {
        std::vector<E> pre;
        for (auto* x : s.idx) pre.push_back(expr(*x));
        size_t fixed = s.idx.size(), nd = s.a->dims.size();
        int64_t cnt = 1;
        for (size_t d = fixed; d < nd; d++) { auto [l, r] = s.a->dims[d]; cnt *= (l > r ? l - r : r - l) + 1; }
        std::vector<std::vector<E>> out;
        for (int64_t k = 0; k < cnt; k++) {
            std::vector<E> idx = pre;
            idx.resize(nd);
            int64_t rest = k;
            for (size_t d = nd; d-- > fixed;) {
                auto [l, r] = s.a->dims[d];
                int64_t n = (l > r ? l - r : r - l) + 1, o = rest % n;
                rest /= n;
                idx[d] = konst(make((uint64_t)(l <= r ? l + o : l - o), 0, 32, 0, true));
            }
            out.push_back(std::move(idx));
        }
        return out;
    }
    std::optional<ArrSel> subArraySel(const slang::ast::Expression& x) {
        using namespace slang::ast;
        if (x.kind != ExpressionKind::ElementSelect || !x.type->isUnpackedArray() ||
            x.type->getCanonicalType().kind != SymbolKind::FixedSizeUnpackedArrayType)
            return std::nullopt;
        auto s = arraySel(x, true);
        if (!s || s->idx.size() >= s->a->dims.size() || s->a->dynElem) return std::nullopt;
        return s;
    }
    // The element values of an unpacked-array value of type t, flattened in
    // row-major order (7.4, 10.9.1): an assignment pattern ('{a, b},
    // '{n{a}}, '{0: a, default: b}), or another fixed-size array.
    void arrayElems(const slang::ast::Type& t0, const slang::ast::Expression& x0, std::vector<E>& out) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        if (t.kind != SymbolKind::FixedSizeUnpackedArrayType) {
            if (t.isUnpackedArray()) unsupported("a container inside a fixed-size array");
            if (!t.isIntegral() && !t.isFloating() && !t.isString()) unsupported("array of this element type");
            int w = (int)t.getBitWidth();
            const Expression& x = peel(x0);
            E v = expr(x0);
            if (t.isIntegral() && x.type->isIntegral()) v = resize(expr(x), w, x.type->isSigned(), t.isSigned());
            out.push_back(v);
            return;
        }
        const auto& ft = t.as<FixedSizeUnpackedArrayType>();
        const Type& et = ft.elementType;
        int64_t n = (int64_t)ft.range.width();
        const Expression& x = peel(x0);
        if (auto sa = subArraySel(x)) {   // m[i]: a sub-array of a multidimensional array (7.4.5)
            for (auto& idx : subArrayIdx(*sa)) {
                auto rd = node(Op::ArrRead, std::move(idx));
                rd->arr = sa->a;
                out.push_back(rd);
            }
            return;
        }
        if (isDynType(*x.type)) {   // fa = q (7.6): elements left to right; the size is checked by the caller
            const Type& set = x.type->getArrayElementType()->getCanonicalType();
            const Type& ec = et.getCanonicalType();
            if (ec.isUnpackedArray() || !(ec.isIntegral() || ec.isFloating() || ec.isString()) || !set.isMatching(ec))
                unsupported("fixed-size array from a container of another element type");
            int d = dynSig(x);
            if (d < 0) unsupported("array value");
            for (int64_t k = 0; k < n; k++) {
                auto g = node(Op::DynGet, {konst(make((uint64_t)k, 0, 32, 0, true))});
                g->sig = d;
                out.push_back(g);
            }
            return;
        }
        switch (x.kind) {
            case ExpressionKind::SimpleAssignmentPattern:
                for (auto* el : x.as<SimpleAssignmentPatternExpression>().elements()) arrayElems(et, *el, out);
                return;
            case ExpressionKind::ReplicatedAssignmentPattern: {
                const auto& r = x.as<ReplicatedAssignmentPatternExpression>();
                int64_t c = constInt(r.count());
                for (int64_t k = 0; k < c; k++)
                    for (auto* el : r.elements()) arrayElems(et, *el, out);
                return;
            }
            case ExpressionKind::StructuredAssignmentPattern: {
                const auto& sp = x.as<StructuredAssignmentPatternExpression>();
                if (!sp.typeSetters.empty()) unsupported("type: key in an array pattern");
                for (int64_t k = 0; k < n; k++) {
                    int64_t idx = ft.range.left >= ft.range.right ? ft.range.left - k : ft.range.left + k;
                    const Expression* v = nullptr;
                    for (const auto& is : sp.indexSetters)
                        if (constInt(*is.index) == idx) v = is.expr;
                    if (v) { arrayElems(et, *v, out); continue; }
                    if (!sp.defaultSetter) unsupported("array pattern without a value for every index");
                    fillElems(et, *sp.defaultSetter, out);
                }
                return;
            }
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {
                auto a = arrFind(&x.as<ValueExpressionBase>().symbol);
                if (!a) {   // a parameter array (6.20.1)
                    auto cv = tryConst(x, &x.as<ValueExpressionBase>().symbol);
                    if (!cv) unsupported("array value");
                    constElems(t, cv, out);
                    return;
                }
                for (int k = 0; k < a->count; k++) out.push_back(ref(a->base + k, a->sgn));
                return;
            }
            case ExpressionKind::Concatenation: {   // {a, 4, 5} into an unpacked array (10.10)
                for (auto* op : x.as<ConcatenationExpression>().operands()) {
                    if (op->type->isUnpackedArray()) arrayElems(*op->type, *op, out);
                    else arrayElems(et, *op, out);
                }
                return;
            }
            case ExpressionKind::RangeSelect: {   // a[1:2], a[i +: 2] with a constant start (7.4.6)
                const auto& rs = x.as<RangeSelectExpression>();
                const Expression& base = peel(rs.value());
                if (int d = isDynType(*base.type) ? dynSig(base) : -1; d >= 0) {   // d[1:4] with constant bounds: elements 1 to 4
                    E first;   // the lowest index, and how many
                    int64_t cnt;
                    if (rs.getSelectionKind() == RangeSelectionKind::Simple) {
                        int64_t l = constInt(rs.left()), r = constInt(rs.right());
                        first = konst(make((uint64_t)std::min(l, r), 0, 32, 0, true));
                        cnt = std::abs(r - l) + 1;
                    } else {   // d[i +: w], d[i -: w]: i may vary, w is constant
                        cnt = constInt(rs.right());
                        first = resize(expr(rs.left()), 32, rs.left().type->isSigned(), true);
                        if (rs.getSelectionKind() == RangeSelectionKind::IndexedDown)
                            first = bin(Op::Sub, first, konst(make((uint64_t)(cnt - 1), 0, 32, 0, true)));
                        int tmp = fresh(32);
                        Instr h; h.k = IK::Assign; h.sig = tmp; h.e = first;
                        prog.emit(h);
                        first = ref(tmp, true);
                    }
                    for (int64_t k = 0; k < cnt; k++) {
                        auto g = node(Op::DynGet, {k ? bin(Op::Add, first, konst(make((uint64_t)k, 0, 32, 0, true))) : first});
                        g->sig = d;
                        out.push_back(g);
                    }
                    return;
                }
                if (base.kind != ExpressionKind::NamedValue && base.kind != ExpressionKind::HierarchicalValue)
                    unsupported("slice of an array expression");
                auto it = arrMap().find(&base.as<ValueExpressionBase>().symbol);
                if (it == arrMap().end() || it->second->dims.size() != 1) unsupported("slice of a multidimensional array");
                const ArrShape& A = *it->second;
                int64_t l, r;
                if (rs.getSelectionKind() == RangeSelectionKind::Simple) { l = constInt(rs.left()); r = constInt(rs.right()); }
                else {
                    int64_t st = constInt(rs.left()), wd = constInt(rs.right());
                    bool up = rs.getSelectionKind() == RangeSelectionKind::IndexedUp;
                    auto [dl, dr] = A.dims[0];
                    bool asc = dl <= dr;   // a[i +: w] runs from i upward in index value
                    int64_t lo = up ? st : st - wd + 1, hi = up ? st + wd - 1 : st;
                    l = asc ? lo : hi; r = asc ? hi : lo;
                }
                int64_t step = l <= r ? 1 : -1;
                for (int64_t k = l;; k += step) {
                    int64_t off = A.offset({k});
                    if (off < 0) unsupported("array slice outside the array");
                    out.push_back(ref(A.base + (int)off, A.sgn));
                    if (k == r) break;
                }
                return;
            }
            case ExpressionKind::MemberAccess:
                if (classMember(x) || vifAccess(x)) {   // h.a (8.4), v.a (25.9): each element read through the handle
                    const auto& m = x.as<MemberAccessExpression>();
                    const PropSlot* ps = vifAccess(x) ? nullptr : memberSlot(m);
                    std::shared_ptr<const ArrShape> A;
                    if (vifAccess(x)) A = vifArr(x);
                    else if (!ps) A = arrFind(&m.member);
                    else {
                        if (ps->kind != 1 || ps->arr->dynElem) unsupported("array value of this property");
                        auto c = std::make_shared<ArrShape>(*ps->arr);
                        c->base = indSlot(exprH(m.value()), ps->arr->base);
                        A = c;
                    }
                    if (!A) unsupported("array value of this property");
                    for (int k = 0; k < A->count; k++) {
                        std::vector<int64_t> idx(A->dims.size());
                        int64_t rest = k;
                        for (size_t dd = A->dims.size(); dd-- > 0;) {
                            auto [l, r] = A->dims[dd];
                            int64_t nn = (l > r ? l - r : r - l) + 1;
                            int64_t o = rest % nn;
                            rest /= nn;
                            idx[dd] = l <= r ? l + o : l - o;
                        }
                        auto rd = node(Op::ArrRead);
                        rd->arr = A;
                        for (int64_t i : idx) rd->kids.push_back(konst(make((uint64_t)i, 0, 32, 0, true)));
                        out.push_back(rd);
                    }
                    return;
                }
                [[fallthrough]];
            default: {   // a constant function call or a parameter array element
                auto cv = tryConst(x, selectBase(x));
                if (!cv && uWidth(t) > 0) {   // a value held packed (a struct member, a container element): its slices
                    const Type* leaf = &t;
                    while (leaf->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType)
                        leaf = &leaf->getCanonicalType().as<FixedSizeUnpackedArrayType>().elementType;
                    int lw = uWidth(*leaf);
                    if (lw > 0 && (leaf->isIntegral() || leaf->isFloating() || leaf->isString())) {
                        E pv = expr(x);
                        for (int top = uWidth(t); top > 0; top -= lw) {
                            auto sl = node(Op::Slice, {pv});
                            sl->hi = top - 1;
                            sl->lo = top - lw;
                            out.push_back(leaf->isIntegral() ? resize(sl, lw, false, leaf->isSigned()) : E(sl));
                        }
                        return;
                    }
                }
                if (!cv) unsupported("fixed-size array value of this kind");
                constElems(t, cv, out);
                return;
            }
        }
    }
    // default: v fills every element below type t (10.9.1), unless v is
    // itself an array value of type t.
    void fillElems(const slang::ast::Type& t0, const slang::ast::Expression& v, std::vector<E>& out) {
        const auto& t = t0.getCanonicalType();
        if (t.kind != slang::ast::SymbolKind::FixedSizeUnpackedArrayType || v.type->isUnpackedArray()) {
            arrayElems(t, v, out);
            return;
        }
        const auto& ft = t.as<slang::ast::FixedSizeUnpackedArrayType>();
        for (int64_t k = 0; k < (int64_t)ft.range.width(); k++) fillElems(ft.elementType, v, out);
    }
    // a = <array value>; for a whole fixed-size array. The values are taken
    // before any element is written, so a = '{a[1], a[0]} swaps.
    bool assignWholeArray(const slang::ast::Expression& l0, const slang::ast::Expression& rhs, bool nba) {
        using namespace slang::ast;
        const Expression& l = peel(l0);
        if (auto sa = subArraySel(l)) {   // m[i] = ... (7.4.5, 7.6): every element of the sub-array
            std::vector<E> vals;
            arrayElems(*l.type, rhs, vals);
            auto idxs = subArrayIdx(*sa);
            if (vals.size() != idxs.size()) unsupported("array value with the wrong number of elements");
            for (auto& v : vals) {   // every value first: m[1] = m[0] then m[0] = m[1] reads before writing
                if (v->op == Op::Const && !nba) continue;
                int tmp = fresh(sa->a->elemW);
                Instr h; h.k = IK::Assign; h.sig = tmp; h.e = v;
                prog.emit(h);
                v = ref(tmp, sa->a->sgn);
            }
            for (size_t k = 0; k < idxs.size(); k++) {
                Instr i; i.k = nba ? IK::Nba : IK::Assign;
                i.sig = sa->a->base; i.arr = sa->a; i.e = vals[k];
                i.aidx = std::move(idxs[k]);
                xpDirect(i);
            }
            return true;
        }
        if (classMember(l) && l.type->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType) {   // h.a = '{...} (8.4)
            const auto& m = l.as<MemberAccessExpression>();
            const PropSlot* ps = memberSlot(m);
            if (!ps) return assignWholeArray(m.member, *l.type, rhs, nba);
            if (ps->kind != 1 || ps->arr->dynElem) return false;
            std::vector<E> vals;
            arrayElems(*l.type, rhs, vals);
            const ArrShape& P = *ps->arr;
            if ((int)vals.size() != P.count) unsupported("array value with the wrong number of elements");
            for (auto& v : vals) {   // every value first: h.a = '{h.a[1], h.a[0]} swaps
                if (v->op == Op::Const && !nba) continue;
                int tmp = fresh(P.elemW);
                Instr h; h.k = IK::Assign; h.sig = tmp; h.e = v;
                prog.emit(h);
                v = ref(tmp, P.sgn);
            }
            auto A = std::make_shared<ArrShape>(P);
            A->base = indSlot(exprH(m.value()), P.base);
            for (int k = 0; k < P.count; k++) {
                std::vector<int64_t> idx(P.dims.size());
                int64_t rest = k;
                for (size_t dd = P.dims.size(); dd-- > 0;) {
                    auto [lo, ro] = P.dims[dd];
                    int64_t nn = (lo > ro ? lo - ro : ro - lo) + 1;
                    int64_t o = rest % nn;
                    rest /= nn;
                    idx[dd] = lo <= ro ? lo + o : lo - o;
                }
                Instr i; i.k = nba ? IK::Nba : IK::Assign;
                i.sig = A->base; i.arr = A; i.e = vals[(size_t)k];
                for (int64_t x : idx) i.aidx.push_back(konst(make((uint64_t)x, 0, 32, 0, true)));
                xpDirect(i);
            }
            return true;
        }
        if ((l.kind == ExpressionKind::NamedValue || l.kind == ExpressionKind::HierarchicalValue) &&
            assignWholeArray(l.as<ValueExpressionBase>().symbol, *l.type, rhs, nba))
            return true;
        // A fixed-size array held packed (a struct member, a container
        // element, a function result): one write of all its bits.
        if (l.type->getCanonicalType().kind != SymbolKind::FixedSizeUnpackedArrayType || uWidth(*l.type) <= 0) return false;
        if (ValueExpressionBase::isKind(l.kind) && arrFind(&l.as<ValueExpressionBase>().symbol)) return false;
        Instr i; i.k = nba ? IK::Nba : IK::Assign;
        i.e = packedValue(*l.type, rhs);
        lhs(l, i);
        xpDirect(i);
        return true;
    }
    // The bits of a fixed-size array value as it is held packed (uWidth),
    // the left element highest.
    E packedValue(const slang::ast::Type& t, const slang::ast::Expression& x) {
        std::vector<E> vals;
        arrayElems(t, x, vals);
        return vals.size() == 1 ? vals[0] : node(Op::Concat, std::move(vals));
    }
    bool assignWholeArray(const slang::ast::Symbol& sym, const slang::ast::Type& lt, const slang::ast::Expression& rhs, bool nba) {
        auto ap = arrFind(&sym);
        if (!ap) return false;
        const ArrShape& A = *ap;
        size_t skip = 0;   // the jump past the write when a container source has the wrong size
        bool sized = false;
        if (const auto& r = peel(rhs); isDynType(*r.type)) {
            // fa = q (7.6): an error, and no write, unless q has as many elements
            int d = dynSig(r);
            if (d < 0) return false;
            int ok = castOk >= 0 ? castOk : fresh(1);
            Instr o; o.k = IK::Assign; o.sig = ok;
            o.e = bin(Op::Eq, resize(dynSize(d), 32, false, false), konst(make((uint64_t)A.count, 0, 32, 0, false)));
            prog.emit(o);
            if (castOk < 0) {
                Instr j; j.k = IK::JmpIfNot; j.e = un(Op::LogNot, ref(ok));
                size_t jo = prog.emit(j);
                Instr e; e.k = IK::Display;
                e.fmt = "Error: assignment to a fixed-size array of " + std::to_string(A.count) +
                        " elements from a container of another size";
                prog.emit(e);
                prog.code[jo].target = prog.here();
            }
            Instr j; j.k = IK::JmpIfNot; j.e = ref(ok);
            skip = prog.emit(j);
            sized = true;
        }
        std::vector<E> vals;
        arrayElems(lt, rhs, vals);
        if ((int)vals.size() != A.count) unsupported("array value with the wrong number of elements");
        bool direct = nba || A.count == 1;
        if (!direct) {
            direct = true;
            for (auto& v : vals) if (v->op != Op::Const) { direct = false; break; }
        }
        if (!direct) {
            for (auto& v : vals) {
                int tmp = fresh(A.elemW);
                Instr h; h.k = IK::Assign; h.sig = tmp; h.e = v;
                prog.emit(h);
                v = ref(tmp, A.sgn);
            }
        }
        for (int k = 0; k < A.count; k++) {
            Instr i; i.k = nba ? IK::Nba : IK::Assign; i.sig = A.base + k; i.e = vals[k];
            xpDirect(i);
        }
        if (sized) prog.code[skip].target = prog.here();
        return true;
    }
    int castOk = -1;   // set by $cast: the flag a container-to-array size check writes, instead of an error
    int64_t constOffset(const ArrSel& s) {
        std::vector<int64_t> v;
        for (auto* x : s.idx) v.push_back(constInt(*x));
        return s.a->offset(v);
    }
    E arrayRead(const ArrSel& s) {
        if (s.constant) {
            int64_t off = constOffset(s);
            if (off >= 0 && !isInd(s.a->base)) return ref(s.a->base + (int)off, s.a->sgn);
        }
        auto n = node(Op::ArrRead);
        n->arr = s.a;
        for (auto* x : s.idx) n->kids.push_back(expr(*x));
        return n;
    }

    // ---------- dynamic arrays, queues, associative arrays (7.5, 7.8, 7.10) ----------
    // A container is a hidden 64-bit handle signal whose value counts its
    // changes; the elements live in dynStore()[handle].
    int dollarQ = -1;   // the queue whose `$` (last index) the selector being lowered uses
    int dynSig(const slang::ast::Expression& e0) {
        using namespace slang::ast;
        const Expression& e = peel(e0);
        if (classMember(e)) {   // h.q (8.4): the container slot through the handle
            const auto& m = e.as<MemberAccessExpression>();
            const PropSlot* ps = memberSlot(m);
            if (!ps) return dynFind(&m.member);
            if (ps->kind != 2) return -1;
            const ClassInfo* ci = classInfo(*m.value().type);
            const DynObj* shape = nullptr;
            for (auto& [off, o] : ci->lay->dyn) if (off == ps->off) shape = &o;
            if (!shape) unsupported("container property shape");
            int d = indSlot(exprH(m.value()), ps->off);
            indShape[d] = *shape;
            return d;
        }
        if (e.kind == ExpressionKind::Call && isDynType(*e.type) && !e.as<CallExpression>().isSystemCall()) {
            // f() returning a container: copy it out of the frame it returned from
            const auto& call = e.as<CallExpression>();
            const auto* sym = std::get<const SubroutineSymbol*>(call.subroutine);
            if (!subs || !sym) return -1;
            auto it = subs->find(sym);
            if (it == subs->end() || !it->second->retDyn) return -1;
            emitUserCall(call, false);
            int t = dynTemp(dynShapeOf(*e.type));
            Instr i = dynCmd(t, D_COPY); i.dsrc = relSig(kRelRet, it->second->retSig); prog.emit(i);
            return t;
        }
        if (e.kind == ExpressionKind::Call && isDynType(*e.type) && e.as<CallExpression>().isSystemCall()) {
            // qq.pop_front() / pop_back() of a queue of queues (7.10.2.6): the
            // element is copied out, then removed
            const auto& call = e.as<CallExpression>();
            auto n = call.getSubroutineName();
            if ((n != "pop_front" && n != "pop_back") || call.arguments().empty()) return -1;
            int outer = dynSig(*call.arguments()[0]);
            if (outer < 0 || !dynShape(outer).elem) return -1;
            int t = dynTemp(dynShapeOf(*e.type));
            { Instr c = dynCmd(t, D_DELETE); prog.emit(c); }   // empty when the queue is
            Instr j; j.k = IK::JmpIfNot;
            j.e = bin(Op::Ne, dynSize(outer), konst(mk(0, 32)));
            size_t at = prog.emit(j);
            E key = n == "pop_front" ? konst(mk(0, 64)) : bin(Op::Sub, resize(dynSize(outer), 64, false, false), konst(mk(1, 64)));
            auto r = node(Op::DynElemRef, {key});
            r->sig = outer;
            int el = elemRef(outer, r);
            Instr c = dynCmd(t, D_COPY); c.dsrc = el; prog.emit(c);
            prog.code[at].target = prog.here();
            prog.emit(dynCmd(outer, n == "pop_front" ? D_POP_FRONT : D_POP_BACK));   // warns when it is empty
            return t;
        }
        if (e.kind == ExpressionKind::ElementSelect && isDynType(*e.type)) {   // m[i] of a container of containers
            const auto& sel = e.as<ElementSelectExpression>();
            // f[i][j] of a fixed-size array of containers: its element's id
            std::vector<const Expression*> idx;
            const Expression* b = &e;
            while (b->kind == ExpressionKind::ElementSelect) {
                idx.insert(idx.begin(), &b->as<ElementSelectExpression>().selector());
                b = &peel(b->as<ElementSelectExpression>().value());
            }
            if (auto f = contArr(*b); f && idx.size() == f->dims.size()) {
                std::vector<E> ks;
                for (auto* x : idx) ks.push_back(expr(*x));
                auto n = node(Op::ArrAddr, std::move(ks));
                n->arr = f;
                return bindCont(std::move(n), *f->dynElem);
            }
            int outer = dynSig(sel.value());
            if (outer < 0 || !dynShape(outer).elem) return -1;
            auto n = node(Op::DynElemRef, {dynKey(outer, sel.selector())});
            n->sig = outer;
            return elemRef(outer, n);
        }
        if (e.kind != ExpressionKind::NamedValue && e.kind != ExpressionKind::HierarchicalValue) return -1;
        return dynFind(&e.as<ValueExpressionBase>().symbol);
    }
    // A fixed-size array of containers (int f[2][$]): static, automatic, a
    // property of this object or one through a handle; its base resolved.
    std::shared_ptr<const ArrShape> contArr(const slang::ast::Expression& b0) {
        using namespace slang::ast;
        const Expression& b = peel(b0);
        std::shared_ptr<const ArrShape> a;
        if (classMember(b)) {
            const auto& m = b.as<MemberAccessExpression>();
            const PropSlot* ps = memberSlot(m);
            if (!ps) a = arrFind(&m.member);
            else if (ps->kind == 1 && ps->arr->dynElem) {
                auto c = std::make_shared<ArrShape>(*ps->arr);
                c->base = indSlot(exprH(m.value()), ps->arr->base);
                a = c;
            }
        } else if (ValueExpressionBase::isKind(b.kind)) {
            const Symbol* sym = &b.as<ValueExpressionBase>().symbol;
            a = arrFind(sym);   // an automatic one first: a local may also have a static twin
            if (!(a && a->dynElem))
                if (auto f = dynArrMap().find(sym); f != dynArrMap().end()) return f->second;
        }
        return a && a->dynElem ? a : nullptr;
    }
    // The indirect id of the container that n (an ArrAddr or DynElemRef,
    // giving id + 1) names: read into a temporary now, or in a constraint
    // kept on the item and resolved at solve time.
    int bindCont(E n, const DynObj& shape) {
        int tmp = fresh(64);
        int d = indThrough(tmp);
        indShape[d] = shape;
        if (consDepth) { consRefs.push_back({d, std::move(n)}); return d; }
        Instr i; i.k = IK::Assign; i.sig = tmp; i.e = std::move(n);
        prog.emit(i);
        return d;
    }
    // The indirect id of the element container that `ref` (a DynElemRef of
    // outer) names, read into a temporary now.
    int elemRef(int outer, E ref0) {
        DynObj shape = *dynShape(outer).elem;
        int tmp = fresh(64);
        if (consDepth) {   // in a constraint: resolved at solve time (RandItem::refs)
            int d = indThrough(tmp);
            indShape[d] = std::move(shape);
            consRefs.push_back({d, std::move(ref0)});
            return d;
        }
        Instr i; i.k = IK::Assign; i.sig = tmp; i.e = std::move(ref0);
        prog.emit(i);
        int d = indThrough(tmp);
        indShape[d] = std::move(shape);
        return d;
    }
    // push_back / push_front / insert of x into t, a container of containers:
    // a new empty element, then x assigned to it.
    void pushContainer(int t, int cmd, const slang::ast::Expression& x, const slang::ast::Expression* at = nullptr) {
        Instr i = dynCmd(t, cmd);
        E key;
        if (cmd == D_INSERT) { i.args = {dynKey(t, *at)}; key = i.args[0]; }
        else if (cmd == D_PUSH_FRONT) key = konst(mk(0, 32));
        prog.emit(i);
        if (!key) key = bin(Op::Sub, resize(dynSize(t), 64, false, false), konst(mk(1, 64)));
        auto n = node(Op::DynElemRef, {key});
        n->sig = t;
        assignContainer(elemRef(t, n), x);
    }
    // How many container levels a type or a shape has (int[][] is 2).
    static int dynDepth(const slang::ast::Type& t) {
        return isDynType(t) ? 1 + dynDepth(*t.getCanonicalType().getArrayElementType()) : 0;
    }
    static int dynDepth(const DynObj& o) { return 1 + (o.elem ? dynDepth(*o.elem) : 0); }
    static bool isDynType(const slang::ast::Type& t0) {
        const auto& t = t0.getCanonicalType();
        return t.isQueue() || t.isAssociativeArray() ||
               t.kind == slang::ast::SymbolKind::DynamicArrayType;
    }
    // An index or key of container d, with `$` meaning its last index.
    E dynKey(int d, const slang::ast::Expression& x) {
        if (dynShape(d).strKey) return strOf(x);
        int saved = dollarQ;
        dollarQ = d;
        E k = expr(x);
        dollarQ = saved;
        return k;
    }
    E dynGet(int d, const slang::ast::Expression& key) {
        auto n = node(Op::DynGet, {dynKey(d, key)});
        n->sig = d;
        return n;
    }
    E dynSize(int d) {
        auto n = node(Op::DynSize);
        n->sig = d;
        return n;
    }
    Instr dynCmd(int d, int cmd) {
        Instr i; i.k = IK::Dyn; i.sig = d; i.dcmd = cmd;
        return i;
    }
    // A scratch container shaped like d, to build a new value in before
    // copying it over, so q = {q[1:$], q[0]} reads the old q throughout.
    int dynTemp(int d) {
        DynObj o = dynShape(d);
        o.vec.clear(); o.map.clear();
        return dynTemp(std::move(o));
    }
    int dynTemp(DynObj o) {
        int t = fresh(64);
        if (isRel(t)) frames.back()->dyn.push_back({relOff(t), o});
        else dynStore()[t] = o;
        return t;
    }
    static const slang::ast::Expression& outArg(const slang::ast::Expression& a) {
        using namespace slang::ast;
        const Expression* x = &peel(a);
        if (x->kind == ExpressionKind::Assignment) x = &peel(x->as<AssignmentExpression>().left());
        return *x;
    }
    // Append the value of `x` (an element, a container, or a queue slice) to t.
    void dynAppend(int t, const slang::ast::Expression& x0) {
        using namespace slang::ast;
        const Expression& x = peel(x0);
        if (const DynObj& ts = dynShape(t); ts.elem && dynDepth(*x.type) == dynDepth(*ts.elem)) {   // {qq, q}: q is one element
            pushContainer(t, D_PUSH_BACK, x);
            return;
        }
        if (int s = dynSig(x); s >= 0) {
            Instr i = dynCmd(t, D_APPEND); i.dsrc = s; prog.emit(i); return;
        }
        if (x.kind == ExpressionKind::RangeSelect) {
            const auto& r = x.as<RangeSelectExpression>();
            if (int s = dynSig(r.value()); s >= 0) {
                if (r.getSelectionKind() != RangeSelectionKind::Simple) unsupported("+: / -: slice of a queue");
                Instr i = dynCmd(t, D_APPEND_SLICE); i.dsrc = s;
                i.args = {dynKey(s, r.left()), dynKey(s, r.right())};
                prog.emit(i);
                return;
            }
        }
        if (x.kind == ExpressionKind::NamedValue && x.type->isUnpackedArray()) {
            auto a = arrFind(&x.as<NamedValueExpression>().symbol);
            if (!a || a->dims.size() != 1) unsupported("array in a container value");
            for (int k = 0; k < a->count; k++) {
                Instr i = dynCmd(t, D_PUSH_BACK); i.e = ref(a->base + k, a->sgn); prog.emit(i);
            }
            return;
        }
        if (x.kind != ExpressionKind::NamedValue && isFixedArr(x) && x.type->getCanonicalType()
                .as<FixedSizeUnpackedArrayType>().elementType.isIntegral()) {   // s.a, m[i]: its elements, left first
            std::vector<E> vals;
            arrayElems(*x.type, x, vals);
            for (auto& v : vals) { Instr i = dynCmd(t, D_PUSH_BACK); i.e = v; prog.emit(i); }
            return;
        }
        if (x.kind == ExpressionKind::Concatenation && isDynType(*x.type)) {
            for (auto* op : x.as<ConcatenationExpression>().operands()) dynAppend(t, *op);
            return;
        }
        if (x.kind == ExpressionKind::Call && isDynType(*x.type)) {   // r = q.find(x) with (...)
            const auto& c = x.as<CallExpression>();
            if (c.isSystemCall() && isLocator(c.getSubroutineName())) {
                Instr i = dynCmd(t, D_APPEND); i.dsrc = locatorCall(c); prog.emit(i);
                return;
            }
        }
        if (x.type->isUnpackedArray()) unsupported("container value");
        Instr i = dynCmd(t, D_PUSH_BACK); i.e = expr(x); prog.emit(i);
    }
    // An array argument (7.7): the formal gets a copy of the actual.
    void copyArrayArg(const slang::ast::FormalArgumentSymbol& f, const slang::ast::Expression& a) {
        if (isDynType(f.getType())) {
            int d = dynFind(&f);
            if (d < 0) unsupported("container argument");
            assignContainer(d, a);
            return;
        }
        if (!assignWholeArray(f, f.getType(), a, false)) unsupported("array argument");
    }
    // An output array argument (7.7): the actual gets a copy of the formal.
    // src: the formal's container signal, or its first element.
    void copyArrayOut(const slang::ast::FormalArgumentSymbol& f, const slang::ast::Expression& target0, int src) {
        using namespace slang::ast;
        const Expression& target = peel(target0);
        if (isDynType(f.getType())) {
            int t = dynSig(target);
            if (t < 0) unsupported("container output argument into this target");
            Instr i = dynCmd(t, D_COPY); i.dsrc = src; prog.emit(i);
            return;
        }
        if (!target.type->isUnpackedArray() || isDynType(*target.type) || target.kind != ExpressionKind::NamedValue)
            unsupported("array output argument into this target");
        auto ts = arrFind(&target.as<ValueExpressionBase>().symbol);
        std::vector<E> el;
        arrayElems(*target.type, target, el);
        if (!ts || (size_t)ts->count != el.size()) unsupported("array output argument of another shape");
        bool sg = target.type->getArrayElementType()->isSigned();
        for (int k = 0; k < ts->count; k++) {
            Instr i; i.k = IK::Assign; i.sig = ts->base + k;
            i.e = ref(src + k, sg);
            prog.emit(i);
        }
    }
    // dst = rhs for a whole container: new[n](src), '{...}, {a, q, b}, {},
    // another container, a fixed-size array, or a queue slice.
    void assignContainer(int dst, const slang::ast::Expression& rhs0) {
        using namespace slang::ast;
        const Expression& rhs = peel(rhs0);
        if (rhs.kind == ExpressionKind::Streaming) {   // q = {>>8{v}} (11.4.14.3): unpack into elements
            const auto& sc = rhs.as<StreamingConcatenationExpression>();
            if (dynShape(dst).kind == 2) unsupported("streaming into an associative array");
            Instr i = dynCmd(dst, D_UNPACK);
            if (streamIsDyn(sc)) i.e = streamDyn(sc);
            else { int w = 0; i.e = streamBits(sc, w); }
            prog.emit(i);
            return;
        }
        if (rhs.kind == ExpressionKind::NewArray) {   // 7.5.1
            const auto& na = rhs.as<NewArrayExpression>();
            Instr i = dynCmd(dst, D_NEW);
            i.args = {expr(na.sizeExpr())};
            if (const Expression* init = na.initExpr()) {
                i.dsrc = dynSig(*init);
                if (i.dsrc < 0) {   // new[n]('{...}): build the source first
                    i.dsrc = dynTemp(dst);
                    prog.emit(dynCmd(i.dsrc, D_DELETE));
                    assignInto(i.dsrc, *init);
                }
            }
            prog.emit(i);
            return;
        }
        if (int s = dynSig(rhs); s >= 0) {
            if (s == dst) return;
            Instr i = dynCmd(dst, D_COPY); i.dsrc = s; prog.emit(i); return;
        }
        int t = dynTemp(dst);
        prog.emit(dynCmd(t, D_DELETE));
        assignInto(t, rhs);
        Instr i = dynCmd(dst, D_COPY); i.dsrc = t; prog.emit(i);
        if (rhs.kind == slang::ast::ExpressionKind::StructuredAssignmentPattern &&
            rhs.as<slang::ast::StructuredAssignmentPatternExpression>().defaultSetter)
            prog.emit(dynCmd(t, D_DEFAULT));   // the temporary is shared: its default goes back to the type's
    }
    // Build rhs into the empty container t.
    void assignInto(int t, const slang::ast::Expression& rhs0) {
        using namespace slang::ast;
        const Expression& rhs = peel(rhs0);
        switch (rhs.kind) {
            case ExpressionKind::SimpleAssignmentPattern:
            case ExpressionKind::ReplicatedAssignmentPattern: {
                if (dynShape(t).kind == 2) unsupported("positional pattern for an associative array");
                const auto& ap = static_cast<const AssignmentPatternExpressionBase&>(rhs);
                int64_t n = rhs.kind == ExpressionKind::ReplicatedAssignmentPattern
                                ? constInt(rhs.as<ReplicatedAssignmentPatternExpression>().count()) : 1;
                for (int64_t k = 0; k < n; k++)
                    for (auto* el : ap.elements()) {
                        if (dynShape(t).elem) { pushContainer(t, D_PUSH_BACK, *el); continue; }   // '{'{1, 2}, '{3}}
                        Instr i = dynCmd(t, D_PUSH_BACK); i.e = expr(*el); prog.emit(i);
                    }
                return;
            }
            case ExpressionKind::StructuredAssignmentPattern: {   // '{key: v, default: d} (10.9.2)
                const auto& sp = rhs.as<StructuredAssignmentPatternExpression>();
                if (dynShape(t).kind != 2) unsupported("keyed pattern for this container");
                if (!sp.memberSetters.empty() || !sp.typeSetters.empty()) unsupported("pattern member setter");
                if (sp.defaultSetter) {   // 7.9.11: what a missing element reads as from now on
                    if (dynShape(t).elem) unsupported("default: in a pattern for an associative array of containers");
                    Instr i = dynCmd(t, D_DEFAULT); i.e = expr(*sp.defaultSetter); prog.emit(i);
                }
                for (auto& is : sp.indexSetters) {
                    Instr i = dynCmd(t, D_SET); i.args = {dynKey(t, *is.index)}; i.e = expr(*is.expr); prog.emit(i);
                }
                return;
            }
            case ExpressionKind::EmptyArgument: return;
            default: dynAppend(t, rhs);
        }
    }
    static bool isReduction(std::string_view n) {
        return n == "sum" || n == "product" || n == "and" || n == "or" || n == "xor";
    }
    static bool isLocator(std::string_view n) {
        return n == "find" || n == "find_index" || n == "find_first" || n == "find_first_index" ||
               n == "find_last" || n == "find_last_index" || n == "min" || n == "max" ||
               n == "unique" || n == "unique_index";
    }
    // A fixed-size array, numbered [0:N-1], for an array method: a scratch
    // queue holding a copy of its elements.
    static bool isFixedArr(const slang::ast::Expression& x) {
        return x.type->getCanonicalType().kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType;
    }
    // A container value: its id, or for q.find with (...) and other
    // container-valued calls a scratch container holding it; -1 if neither.
    bool isDynCall(const slang::ast::Expression& x) {
        const auto& p = peel(x);
        return stmtDepth && p.kind == slang::ast::ExpressionKind::Call && isDynType(*p.type);
    }
    int dynValue(const slang::ast::Expression& x) {
        int d = dynSig(x);
        if (d >= 0 || !isDynCall(x)) return d;
        d = dynTemp(dynShapeOf(*x.type));
        prog.emit(dynCmd(d, D_DELETE));
        assignInto(d, x);
        return d;
    }
    int fixedAsQueue(const slang::ast::Expression& x) {
        using namespace slang::ast;
        const Type& at = x.type->getCanonicalType();
        if (!isFixedArr(x)) unsupported("array method of this array");
        if (!stmtDepth) unsupported("array method of a fixed-size array outside a procedural statement");
        auto r = at.as<FixedSizeUnpackedArrayType>().range;
        if (r.left != 0 || r.right < 0) unsupported("array method of an array not declared [N] or [0:N-1]");
        int q = keyQueue(*at.getArrayElementType());
        dynAppend(q, x);
        lastFixedQueue = q;
        return q;
    }
    int lastFixedQueue = -1;   // the scratch queue fixedAsQueue made last
    // An element of container d for an array method: the with expression
    // evaluated with the iterator set to element ix, or the element itself.
    // An array method's iteration (7.12): container d, the with expression
    // and its iterator; idxSig holds item.index, from the queue keys when the
    // array was an associative one.
    struct ElemKey {
        int d; const slang::ast::Expression* withE; const slang::ast::ValueSymbol* it; int itSig;
        int idxSig = -1, keys = -1;
    };
    std::unordered_map<const slang::ast::Symbol*, int> iterIndex;   // iterator -> its item.index
    int iterIndexSig(const slang::ast::CallExpression& call) {
        if (call.getSubroutineName() != "index" || call.arguments().empty()) return -1;
        const auto& a = peel(*call.arguments()[0]);
        if (a.kind != slang::ast::ExpressionKind::NamedValue) return -1;
        auto it = iterIndex.find(&a.as<slang::ast::NamedValueExpression>().symbol);
        return it == iterIndex.end() ? -1 : it->second;
    }
    // Set up the iterator of a with clause: its value, and its index.
    void bindIterator(ElemKey& ek, int kw) {
        if (!ek.it) return;
        int w = storeW(ek.it->getType());
        if (w <= 0) throw std::runtime_error(whereStr() + "lower: unsupported locator iterator of type " + ek.it->getType().toString());
        needWidth(w);
        ek.itSig = fresh(w);
        sigmap[ek.it] = ek.itSig;
        if (kw <= 0) return;
        needWidth(kw);
        ek.idxSig = fresh(kw);
        iterIndex[ek.it] = ek.idxSig;
    }
    // The elements of associative array d in key order as a queue, and its
    // keys as a second one (array methods of 7.12 on an associative array).
    std::pair<int, int> assocAsQueues(int d, const slang::ast::Type& at0) {
        using namespace slang::ast;
        const Type& at = at0.getCanonicalType();
        if (at.kind != SymbolKind::AssociativeArrayType) unsupported("array method of this associative array");
        const auto& aat = at.as<AssociativeArrayType>();
        if (!aat.indexType) unsupported("array method of an associative array with a wildcard index");
        int vals = keyQueue(aat.elementType), keys = keyQueue(*aat.indexType);
        bool sk = dynShape(d).strKey, sg = dynShape(d).keySgn;
        int v = fresh(sk ? 64 : dynShape(d).keyW), ok = fresh(32);
        Instr first = dynCmd(d, D_FIRST); first.dkey = v; first.dres = ok;
        prog.emit(first);
        size_t top = prog.here();
        Instr j; j.k = IK::JmpIfNot; j.e = ref(ok);
        size_t jf = prog.emit(j);
        auto key = [&]() -> E { return sk ? un(Op::AsStr, ref(v)) : ref(v, sg); };
        Instr pk = dynCmd(keys, D_PUSH_BACK); pk.e = key(); prog.emit(pk);
        auto g = node(Op::DynGet, {key()});
        g->sig = d;
        Instr pv = dynCmd(vals, D_PUSH_BACK); pv.e = g; prog.emit(pv);
        Instr nx = dynCmd(d, D_NEXT); nx.dkey = v; nx.dres = ok;
        prog.emit(nx);
        Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
        prog.code[jf].target = prog.here();
        return {vals, keys};
    }
    E elemAt(int d, int ix) {
        auto g = node(Op::DynGet, {ref(ix, true)});
        g->sig = d;
        return g;
    }
    E elemKey(const ElemKey& k, int ix) {
        if (!k.withE) return elemAt(k.d, ix);
        Instr a; a.k = IK::Assign; a.sig = k.itSig; a.e = elemAt(k.d, ix);
        prog.emit(a);
        if (k.idxSig >= 0) {
            Instr x; x.k = IK::Assign; x.sig = k.idxSig; x.e = k.keys >= 0 ? elemAt(k.keys, ix) : ref(ix, true);
            prog.emit(x);
        }
        return expr(*k.withE);
    }
    // An empty scratch queue whose elements have type t.
    int keyQueue(const slang::ast::Type& t) {
        int q = fresh(64);
        DynObj o;
        o.kind = 1;
        elemInto(o, t);
        if (isRel(q)) frames.back()->dyn.push_back({relOff(q), o});
        else dynStore()[q] = o;
        prog.emit(dynCmd(q, D_DELETE));
        return q;
    }
    // for (ix = 0; ix < d.size(); ix++) body, or from the end when back is
    // set. body may add jumps to the end of the loop to exits.
    template <class F>
    void dynLoop(int d, bool back, F body) {
        int ix = fresh(32);
        Val one = mk(1, 32); one.sgn = true;
        Val zero = mk(0, 32); zero.sgn = true;
        Instr i0; i0.k = IK::Assign; i0.sig = ix;
        i0.e = back ? bin(Op::Sub, resize(dynSize(d), 32, false, true), konst(one)) : konst(zero);
        prog.emit(i0);
        size_t top = prog.here();
        Instr j; j.k = IK::JmpIfNot;
        j.e = back ? bin(Op::Ge, ref(ix, true), konst(zero))
                   : bin(Op::Lt, ref(ix, true), resize(dynSize(d), 32, false, true));
        std::vector<size_t> exits{prog.emit(j)};
        body(ix, exits);
        Instr st; st.k = IK::Assign; st.sig = ix;
        st.e = bin(back ? Op::Sub : Op::Add, ref(ix, true), konst(one));
        prog.emit(st);
        Instr bk; bk.k = IK::Jmp; bk.target = top; prog.emit(bk);
        for (size_t x : exits) prog.code[x].target = prog.here();
    }
    // q.find(x) with (...), q.min(), q.unique_index() ... (7.12.1): the
    // result goes into a scratch queue, which is returned.
    int locatorCall(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        std::string_view n = call.getSubroutineName();
        auto args = call.arguments();
        if (!stmtDepth) unsupported("locator method outside a procedural statement");
        int d = dynValue(*args[0]);
        if (d < 0) d = fixedAsQueue(*args[0]);
        int keys = -1, ixw = 32;
        if (dynShape(d).kind == 2) {   // an associative array: its elements and keys in key order
            ixw = dynShape(d).strKey ? 64 : dynShape(d).keyW;
            std::tie(d, keys) = assocAsQueues(d, *args[0]->type);
        }
        const Expression* withE = nullptr;
        const ValueSymbol* itSym = nullptr;
        if (auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine))
            std::tie(withE, itSym) = sci->getIteratorInfo();
        ElemKey ek{d, withE, itSym, -1};
        ek.keys = keys;
        bindIterator(ek, ixw);
        bool index = n.ends_with("_index");
        const Type& et = call.type->getCanonicalType().getArrayElementType()->getCanonicalType();
        int res = keyQueue(et);
        auto push = [&](int ix) {
            Instr p = dynCmd(res, D_PUSH_BACK);
            p.e = !index ? elemAt(d, ix) : keys >= 0 ? elemAt(keys, ix) : ref(ix, true);
            prog.emit(p);
        };
        if (n.starts_with("find")) {
            if (!withE) unsupported("find without a with clause");
            bool first = n.starts_with("find_first"), last = n.starts_with("find_last");
            dynLoop(d, last, [&](int ix, std::vector<size_t>& exits) {
                Instr j; j.k = IK::JmpIfNot; j.e = elemKey(ek, ix);
                size_t jf = prog.emit(j);
                push(ix);
                if (first || last) { Instr x; x.k = IK::Jmp; exits.push_back(prog.emit(x)); }
                prog.code[jf].target = prog.here();
            });
            return res;
        }
        const Type& kt = (withE ? *withE->type : *args[0]->type->getArrayElementType()).getCanonicalType();
        // A string or real key is compared as one (7.12.1); its 64-bit value is held as is.
        int ksk = kt.isString() ? 1 : kt.isFloating() ? 2 : 0;
        if (!ksk && !kt.isIntegral()) unsupported("min / max / unique on an aggregate key");
        int kw = ksk ? 64 : (int)kt.getBitWidth();
        needWidth(kw);
        bool ks = !ksk && kt.isSigned();
        auto keyOf = [&](int ix) -> E {
            if (ksk == 1 && withE) {
                Instr a; a.k = IK::Assign; a.sig = ek.itSig; a.e = elemAt(ek.d, ix); prog.emit(a);
                return strOf(*withE);
            }
            if (ksk == 2 && withE) {
                Instr a; a.k = IK::Assign; a.sig = ek.itSig; a.e = elemAt(ek.d, ix); prog.emit(a);
                auto bits = node(Op::Resize, {realOf(*withE)});   // its 64 bits, as $realtobits
                bits->hi = 64;
                return bits;
            }
            E k = elemKey(ek, ix);
            return ksk ? k : resize(std::move(k), kw, ks, ks);
        };
        auto cmpKey = [&](int kind, E a, E b) -> E {   // kind as FCmp: 0 <, 2 >, 4 ==
            if (ksk == 1) {   // a held key is a string id: AsStr makes it a string again
                auto c = node(Op::StrCmp, {un(Op::AsStr, std::move(a)), un(Op::AsStr, std::move(b))});
                c->hi = kind;
                return c;
            }
            if (ksk == 2) return fcmp(kind, un(Op::AsReal, std::move(a)), un(Op::AsReal, std::move(b)));
            if (kind == 0) return bin(Op::Lt, std::move(a), std::move(b));
            if (kind == 2) return bin(Op::Gt, std::move(a), std::move(b));
            return bin(Op::CaseEq, std::move(a), std::move(b));
        };
        if (n == "min" || n == "max") {
            int best = fresh(32), bk = fresh(kw);
            Val m1 = mk(-1, 32); m1.sgn = true; m1.v = mask(32);
            Instr b0; b0.k = IK::Assign; b0.sig = best; b0.e = konst(m1); prog.emit(b0);
            dynLoop(d, false, [&](int ix, std::vector<size_t>&) {
                int cur = fresh(kw);
                Instr c; c.k = IK::Assign; c.sig = cur; c.e = keyOf(ix);
                prog.emit(c);
                Val zero = mk(0, 32); zero.sgn = true;
                E better = cmpKey(n == "min" ? 0 : 2, ref(cur, ks), ref(bk, ks));
                Instr j; j.k = IK::JmpIfNot;
                j.e = bin(Op::LogOr, bin(Op::Lt, ref(best, true), konst(zero)), better);
                size_t jf = prog.emit(j);
                Instr s1; s1.k = IK::Assign; s1.sig = bk; s1.e = ref(cur, ks); prog.emit(s1);
                Instr s2; s2.k = IK::Assign; s2.sig = best; s2.e = ref(ix, true); prog.emit(s2);
                prog.code[jf].target = prog.here();
            });
            Val zero = mk(0, 32); zero.sgn = true;
            Instr j; j.k = IK::JmpIfNot; j.e = bin(Op::Ge, ref(best, true), konst(zero));
            size_t jf = prog.emit(j);
            push(best);
            prog.code[jf].target = prog.here();
            return res;
        }
        if (n == "unique" || n == "unique_index") {
            // element ix is kept when no earlier element has an equal key
            int cur = fresh(kw), dup = fresh(1);
            dynLoop(d, false, [&](int ix, std::vector<size_t>&) {
                Instr c; c.k = IK::Assign; c.sig = cur; c.e = keyOf(ix); prog.emit(c);
                Instr z; z.k = IK::Assign; z.sig = dup; z.e = konst(mk(0, 1)); prog.emit(z);
                int jx = fresh(32);
                Val zero = mk(0, 32); zero.sgn = true;
                Val one = mk(1, 32); one.sgn = true;
                Instr j0; j0.k = IK::Assign; j0.sig = jx; j0.e = konst(zero); prog.emit(j0);
                size_t top = prog.here();
                Instr jt; jt.k = IK::JmpIfNot; jt.e = bin(Op::Lt, ref(jx, true), ref(ix, true));
                size_t jf = prog.emit(jt);
                E eq = cmpKey(4, keyOf(jx), ref(cur, ks));
                Instr sd; sd.k = IK::Assign; sd.sig = dup;
                sd.e = bin(Op::LogOr, ref(dup, false), eq);
                prog.emit(sd);
                Instr inc; inc.k = IK::Assign; inc.sig = jx; inc.e = bin(Op::Add, ref(jx, true), konst(one)); prog.emit(inc);
                Instr bk; bk.k = IK::Jmp; bk.target = top; prog.emit(bk);
                prog.code[jf].target = prog.here();
                Instr k; k.k = IK::JmpIfNot; k.e = un(Op::LogNot, ref(dup, false));
                size_t kf = prog.emit(k);
                push(ix);
                prog.code[kf].target = prog.here();
            });
            return res;
        }
        unsupported("locator method");
    }

    // Built-in methods of a container (7.5.2, 7.8, 7.9, 7.10.2, 7.12): as a
    // value, or as a statement when wantValue is false.
    E dynMethod(const slang::ast::CallExpression& call, bool wantValue) {
        using namespace slang::ast;
        std::string_view n = call.getSubroutineName();
        auto args = call.arguments();
        int d = dynValue(*args[0]);
        if (d < 0) d = fixedAsQueue(*args[0]);
        const DynObj& o = dynShape(d);
        const Expression* withE = nullptr;
        const ValueSymbol* itSym = nullptr;
        if (auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine))
            std::tie(withE, itSym) = sci->getIteratorInfo();
        if (withE && consDepth) {   // in a constraint: sum() with (...) and the like, folded at solve time (18.5.8.2)
            static const std::pair<const char*, int> red[] = {{"sum", 0}, {"product", 1}, {"and", 2}, {"or", 3}, {"xor", 4}};
            for (auto& [s, id] : red)
                if (n == s) {
                    if (!call.type->isIntegral()) unsupported("reduction with a non-integral with clause");
                    int ph = fresh(itSym ? std::max(1, storeW(itSym->getType())) : 64);
                    if (itSym) symOverride[itSym] = ph;
                    E body = expr(*withE);
                    if (itSym) symOverride.erase(itSym);
                    auto x = node(Op::DynReduce, {resize(body, (int)call.type->getBitWidth(), withE->type->isSigned(), false)});
                    x->sig = d;
                    x->hi = id;
                    x->lo = ph;
                    x->k = mk(0, (int)call.type->getBitWidth());
                    x->k.sgn = call.type->isSigned();
                    return x;
                }
        }
        if (withE || isLocator(n)) {
            if (!stmtDepth) unsupported("array method with a `with` clause outside a procedural statement");
            int keys = -1, kw = 32;
            if (o.kind == 2) {   // an associative array: its elements and keys in key order
                if (n == "sort" || n == "rsort") unsupported("sort of an associative array");
                kw = o.strKey ? 64 : o.keyW;
                std::tie(d, keys) = assocAsQueues(d, *args[0]->type);
            }
            ElemKey ek{d, withE, itSym, -1};
            ek.keys = keys;
            bindIterator(ek, kw);
            static const std::pair<const char*, Op> redw[] = {
                {"sum", Op::Add}, {"product", Op::Mul}, {"and", Op::And}, {"or", Op::Or}, {"xor", Op::Xor}};
            for (auto& [s, op] : redw)
                if (n == s) {   // 7.12.3: the result has the type of the with expression
                    int w = (int)call.type->getBitWidth();
                    bool sg = call.type->isSigned();
                    if (!call.type->isIntegral()) unsupported("reduction with a non-integral with clause");
                    int acc = fresh(w);
                    Val init = n == "product" ? mk(1, w) : n == "and" ? v_not(mk(0, w)) : mk(0, w);
                    Instr a0; a0.k = IK::Assign; a0.sig = acc; a0.e = konst(init); prog.emit(a0);
                    dynLoop(d, false, [&](int ix, std::vector<size_t>&) {
                        Instr a; a.k = IK::Assign; a.sig = acc;
                        a.e = bin(op, ref(acc, sg), resize(elemKey(ek, ix), w, withE ? withE->type->isSigned() : sg, sg));
                        prog.emit(a);
                    });
                    return ref(acc, sg);
                }
            if (n == "sort" || n == "rsort") {   // 7.12.2: order by the with expression
                if (wantValue) unsupported("sort used as a value");
                int kc = keyQueue(*withE->type);
                dynLoop(d, false, [&](int ix, std::vector<size_t>&) {
                    Instr p = dynCmd(kc, D_PUSH_BACK); p.e = elemKey(ek, ix); prog.emit(p);
                });
                Instr i = dynCmd(d, n == "sort" ? D_SORT_BY : D_RSORT_BY);
                i.dsrc = kc;
                prog.emit(i);
                return nullptr;
            }
            if (isLocator(n)) unsupported("locator method here (assign its result to a queue)");
            unsupported("array method with a `with` clause");
        }
        if (n == "size" || n == "num" || n == "$size") return dynSize(d);
        if (n == "exists") {
            auto x = node(Op::DynExists, {dynKey(d, *args[1])});
            x->sig = d;
            return x;
        }
        static const std::pair<const char*, int> red[] = {{"sum", 0}, {"product", 1}, {"and", 2}, {"or", 3}, {"xor", 4}};
        for (auto& [s, id] : red)
            if (n == s) {
                if (o.dflt.real || o.dflt.str) unsupported("reduction of a real or string array");
                auto x = node(Op::DynReduce);
                x->sig = d;
                x->hi = id;
                x->k = mk(0, (int)call.type->getBitWidth());
                x->k.sgn = call.type->isSigned();
                return x;
            }
        if (n == "pop_front" || n == "pop_back") {
            if (wantValue && !stmtDepth) unsupported("pop outside a procedural statement");
            Instr i = dynCmd(d, n == "pop_front" ? D_POP_FRONT : D_POP_BACK);
            int w = storeW(*call.type);
            needWidth(w);
            i.dres = fresh(w);
            prog.emit(i);
            return ref(i.dres, call.type->isSigned());
        }
        if (n == "first" || n == "last" || n == "next" || n == "prev") {   // 7.9.4-7.9.7
            if (wantValue && !stmtDepth) unsupported("first/next outside a procedural statement");
            Instr i = dynCmd(d, n == "first" ? D_FIRST : n == "last" ? D_LAST : n == "next" ? D_NEXT : D_PREV);
            Instr k; k.k = IK::Assign;
            lhs(outArg(*args[1]), k);
            if (k.hi >= 0 || k.bidx || k.arr || k.dyn >= 0) unsupported("first/next into a part of a variable");
            i.dkey = k.sig;
            i.dres = fresh(32);
            prog.emit(i);
            return ref(i.dres, true);
        }
        if (wantValue) unsupported("container method used as a value");
        static const std::pair<const char*, int> cmds[] = {{"push_back", D_PUSH_BACK}, {"push_front", D_PUSH_FRONT},
            {"insert", D_INSERT}, {"reverse", D_REVERSE}, {"sort", D_SORT}, {"rsort", D_RSORT}, {"shuffle", D_SHUFFLE}};
        Instr i;
        if (n == "delete") i = dynCmd(d, args.size() > 1 ? D_DELETE_AT : D_DELETE);
        else {
            bool found = false;
            for (auto& [s, c] : cmds) if (n == s) { i = dynCmd(d, c); found = true; }
            if (!found) unsupported("container method");
        }
        if (i.dcmd == D_DELETE_AT) i.args = {dynKey(d, *args[1])};
        if (dynShape(d).elem && (i.dcmd == D_PUSH_BACK || i.dcmd == D_PUSH_FRONT || i.dcmd == D_INSERT)) {
            pushContainer(d, i.dcmd, i.dcmd == D_INSERT ? *args[2] : *args[1], i.dcmd == D_INSERT ? args[1] : nullptr);
            return nullptr;
        }
        if (i.dcmd == D_PUSH_BACK || i.dcmd == D_PUSH_FRONT) i.e = expr(*args[1]);
        if (i.dcmd == D_INSERT) { i.args = {dynKey(d, *args[1])}; i.e = expr(*args[2]); }
        prog.emit(i);
        return nullptr;
    }

    // A select of a packed vector (11.5.1, 7.4.1), as a bit range of the
    // signal. Indices are in declared numbering: [7:0], [8:1], or [0:7] all
    // work, and an element of a packed array (logic [3:0][7:0] x; x[1]) is
    // its whole element. Either the start bit is a constant (cbit) or it is
    // computed at run time (vbit); a part outside the vector reads x.
    struct PSel { enum Kind { Index, Up, Down, Range }; bool known = true; int64_t cbit = 0; E vbit; int bits = 1; };
    PSel packedSel(const slang::ast::Type& vt, const slang::ast::Expression* idx,
                   const slang::ast::Expression* idx2, int64_t count, PSel::Kind kind) {
        using namespace slang::ast;
        int W = vt.getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType ? uWidth(vt) : (int)vt.getBitWidth();
        int64_t left = W - 1, right = 0;
        if (vt.hasFixedRange()) { auto fr = vt.getFixedRange(); left = fr.left; right = fr.right; }
        bool desc = left >= right;
        int64_t n = (desc ? left - right : right - left) + 1;
        int ew = n > 0 ? (int)(W / n) : 1;
        PSel ps;
        auto isConst = [](const Expression* x) { return x->kind == ExpressionKind::IntegerLiteral || x->getConstant(); };
        // first declared index covered by the select, lowest position first
        auto startPos = [&](int64_t i) -> int64_t {
            switch (kind) {
                case PSel::Index: return desc ? i - right : right - i;
                case PSel::Up:    return desc ? i - right : right - i - count + 1;
                case PSel::Down:  return desc ? i - count + 1 - right : right - i;
                case PSel::Range: return 0;
            }
            return 0;
        };
        if (kind == PSel::Range) {   // a[x:y], both constant
            int64_t x = constInt(*idx), y = constInt(*idx2);
            int64_t lo = desc ? std::min(x, y) - right : right - std::max(x, y);
            ps.cbit = lo * ew;
            ps.bits = (int)((x > y ? x - y : y - x) + 1) * ew;
            return ps;
        }
        ps.bits = (int)count * ew;
        if (isConst(idx)) { ps.cbit = startPos(constInt(*idx)) * ew; return ps; }
        ps.known = false;
        E ix = resize(expr(*idx), 64, idx->type->isSigned(), true);
        auto K = [](int64_t v) { return konst(make((uint64_t)v, 0, 64, 0, true)); };
        E pos;
        switch (kind) {
            case PSel::Index: pos = desc ? bin(Op::Sub, ix, K(right)) : bin(Op::Sub, K(right), ix); break;
            case PSel::Up:    pos = desc ? bin(Op::Sub, ix, K(right)) : bin(Op::Sub, K(right - count + 1), ix); break;
            case PSel::Down:  pos = desc ? bin(Op::Sub, ix, K(count - 1 + right)) : bin(Op::Sub, K(right), ix); break;
            default: break;
        }
        ps.vbit = ew == 1 ? pos : bin(Op::Mul, pos, K(ew));
        return ps;
    }
    PSel rangePSel(const slang::ast::RangeSelectExpression& r) {
        using namespace slang::ast;
        const Type& vt = *r.value().type;
        if (r.getSelectionKind() == RangeSelectionKind::Simple)
            return packedSel(vt, &r.left(), &r.right(), 0, PSel::Range);
        bool down = r.getSelectionKind() == RangeSelectionKind::IndexedDown;
        return packedSel(vt, &r.left(), nullptr, constInt(r.right()), down ? PSel::Down : PSel::Up);
    }
    // s.f on a packed struct or union (7.2.1, 7.3.1): a constant slice.
    PSel memberPSel(const slang::ast::MemberAccessExpression& m) {
        using namespace slang::ast;
        if (m.member.kind != SymbolKind::Field) unsupported("member access");
        const auto& f = m.member.as<FieldSymbol>();
        const Type& vt = m.value().type->getCanonicalType();
        if (vt.isUnpackedStruct() || vt.isUnpackedUnion()) {
            auto [off, w] = uField(vt, f);
            if (w <= 0 || uWidth(vt) <= 0) unsupported("member of an unpacked struct that holds an array, class or event");
            PSel ps;
            ps.cbit = off;
            ps.bits = w;
            return ps;
        }
        if (!vt.isIntegral()) unsupported("member of a class");
        PSel ps;
        ps.cbit = (int64_t)f.bitOffset;
        ps.bits = (int)f.getType().getBitWidth();
        return ps;
    }
    E readPSel(E base, const PSel& ps) {
        // A constant select is a slice even when the bits sit above bit 63.
        // v_slice reads a bit past the vector as X.
        if (ps.known && ps.cbit >= 0 && ps.bits > 0)
            return slice(std::move(base), (int)(ps.cbit + ps.bits - 1), (int)ps.cbit);
        E start = ps.known ? konst(make((uint64_t)ps.cbit, 0, 64, 0, true)) : ps.vbit;
        auto n = node(Op::DynSlice, {std::move(base), start});
        n->hi = ps.bits;
        n->lo = 0;
        return n;
    }
    // The same select as an assignment target. When the target already has a
    // select (p[3][7:4] = ...), this one is inside it: offsets add up.
    void writePSel(Instr& i, const PSel& ps) {
        bool inner = i.hi >= 0 || i.bidx;
        if (inner) {
            auto K = [](int64_t v) { return konst(make((uint64_t)v, 0, 64, 0, true)); };
            if (!i.bidx && ps.known) {
                if (ps.cbit < 0 || ps.cbit + ps.bits > i.hi - i.lo + 1) unsupported("constant select outside the element");
                i.lo += (int)ps.cbit;
                i.hi = i.lo + ps.bits - 1;
                return;
            }
            E outer = ps.known ? K(ps.cbit) : ps.vbit;
            E base = i.bidx ? i.bidx : K(i.lo);
            i.bidx = bin(Op::Add, base, outer);
            i.pw = ps.bits;
            i.pdown = false;
            i.hi = 0;
            i.lo = 0;
            return;
        }
        if (ps.known) {
            if (ps.cbit < 0) unsupported("constant select outside the vector");
            i.lo = (int)ps.cbit;
            i.hi = (int)(ps.cbit + ps.bits - 1);
            return;
        }
        i.bidx = ps.vbit;
        i.pw = ps.bits;
        i.pdown = false;
        i.hi = 0;
        i.lo = 0;
    }

    // A whole fixed-size unpacked array as one value, laid out as in an
    // unpacked struct (uWidth): the left element highest. It is how such an
    // array goes into a container element or a struct member.
    E packedArray(const slang::ast::Symbol* sym, const slang::ast::Type& t) {
        if (t.getCanonicalType().kind != slang::ast::SymbolKind::FixedSizeUnpackedArrayType || uWidth(t) <= 0) return nullptr;
        auto a = arrFind(sym);
        if (!a || a->dynElem || isInd(a->base)) return nullptr;
        std::vector<E> parts;
        for (int k = 0; k < a->count; k++) parts.push_back(ref(a->base + k));
        return parts.size() == 1 ? parts[0] : node(Op::Concat, std::move(parts));
    }
    // A parameter, localparam, or enum value (6.20, 6.19.3) is its constant.
    E constSym(const slang::ast::Symbol& sym, const slang::ast::Expression& e, ExprCtx ctx) {
        using namespace slang::ast;
        const slang::ConstantValue* cv = nullptr;
        if (sym.kind == SymbolKind::Specparam) {   // 30.3; an SDF LABEL may have changed it (32.4.3)
            if (auto lv = labelled().find(&sym); lv != labelled().end()) {
                if (e.type->isFloating()) return konst(mkreal(lv->second));
                int w = (int)e.type->getBitWidth();
                return sized(konst(make((uint64_t)std::llround(lv->second), 0, w, 0, e.type->isSigned())), w, e.type->isSigned(), ctx);
            }
            cv = &sym.as<SpecparamSymbol>().getValue();
            if (cv->isReal()) return konst(mkreal(cv->real()));
            if (cv->isShortReal()) return konst(mkreal(cv->shortReal()));
        }
        if (cv) {}
        else if (sym.kind == SymbolKind::Parameter) cv = &sym.as<ParameterSymbol>().getValue();
        else if (sym.kind == SymbolKind::EnumValue) cv = &sym.as<EnumValueSymbol>().getValue();
        else if (const auto* c = e.getConstant()) cv = c;
        if (cv && cv->isString()) return konst(mkstr(cv->str()));   // a string parameter
        if (cv && (cv->isReal() || cv->isShortReal())) {   // a real parameter: its 64 bits, which AsReal reads (6.12)
            double d = cv->isReal() ? (double)cv->real() : (double)(float)cv->shortReal();
            uint64_t bits;
            std::memcpy(&bits, &d, 8);
            return konst(mk(bits, 64));
        }
        if (!cv || !cv->isInteger()) unsupported("reference to non-signal symbol");
        return svLit(cv->integer(), e.type->isSigned(), ctx);
    }

    E expr2(const slang::ast::Expression& e, ExprCtx ctx) {
        using namespace slang::ast;
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {   // x, or u.x / blk[2].x (23.6)
                const Symbol* sym = &e.as<ValueExpressionBase>().symbol;
                if (auto cv = constVals.find(sym); cv != constVals.end())   // an unrolled foreach index (18.5.8)
                    return sized(konst(cv->second), cv->second.w, cv->second.sgn, ctx);
                if (auto xb = exprBind.find(sym); xb != exprBind.end())   // `item` of a reduction's with clause
                    return sized(xb->second, (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
                if (dynFind(sym) >= 0) unsupported("a whole dynamic array, queue, or associative array as a value");
                int sg = sigFind(sym);
                if (sg < 0)
                    if (E p = packedArray(sym, *e.type)) return p;   // a whole fixed-size array as one value
                if (sg < 0) return constSym(*sym, e, ctx);
                int src = (int)e.type->getBitWidth();
                bool self = e.type->isSigned();
                return sized(ref(sg, self), src, self, ctx);
            }
            case ExpressionKind::LValueReference: {
                if (!lvalue) unsupported("lvalue reference");
                return expr(*lvalue, ctx);
            }
            case ExpressionKind::TaggedUnion:
                return sized(taggedValue(e.as<TaggedUnionExpression>()), storeW(*e.type), false, ctx);
            case ExpressionKind::Streaming: {
                const auto& sc = e.as<StreamingConcatenationExpression>();
                int w = 0;
                E v = streamBits(sc, w);
                return sized(v, w, false, ctx);
            }
            case ExpressionKind::Conversion: {
                // 4'hF inside an 8-bit add is a 4-bit literal under a conversion
                // to 8. cnt+1 is a 32-bit add under a conversion to 4. Either
                // way the operand is lowered at the width that applies here.
                // Propagated (11.8.2): the context type's signedness replaces
                // the operand's, then the value is extended. Implicit assignment
                // (11.8.3): extend with the operand's signedness. An outer width
                // still wins, so a truncating assign stays narrow.
                const auto& c = e.as<ConversionExpression>();
                // To a 2-state type (bit, int, byte) from a 4-state one: x and
                // z bits become 0 (6.11.2, 10.7). The conversion is lowered as
                // usual, then its x and z bits are cleared.
                if (to2State != &e && c.type->isIntegral() && !c.type->isFourState() &&
                    c.operand().type->isIntegral() && c.operand().type->isFourState()) {
                    const Expression* saved = to2State;
                    to2State = &e;
                    E v = expr(e, ctx);
                    to2State = saved;
                    if (v->op == Op::Const) {
                        Val k = v->k;
                        for (int i = 0; i < limbCount(k.w); i++) setLimb(k, i, limbV(k, i) & ~limbX(k, i), 0, 0);
                        return konst(k);
                    }
                    return sysFnNode(SF_TO_2STATE, {std::move(v)});
                }
                if (peel(c.operand()).kind == ExpressionKind::Streaming && c.type->isIntegral()) {
                    // a stream into a wider integral type is left-aligned,
                    // padded with zeros on the right (11.4.14)
                    int w = (int)c.type->getBitWidth();
                    needWidth(w);
                    return sized(streamTo(peel(c.operand()).as<StreamingConcatenationExpression>(), w),
                                 w, c.type->isSigned(), ctx);
                }
                if (c.operand().type->isString() && c.type->isIntegral()) {   // the text as bytes (6.16)
                    int w = (int)c.type->getBitWidth();
                    needWidth(w);
                    auto n = node(Op::StrToInt, {exprStr(c.operand())});
                    n->hi = w;
                    return sized(n, w, c.type->isSigned(), ctx);
                }
                if (c.operand().type->isFloating() && c.type->isIntegral()) {   // 6.12.1: rounded
                    int w = (int)c.type->getBitWidth();
                    needWidth(w);
                    return sized(rtoi(exprReal(c.operand()), w, c.type->isSigned(), false), w, c.type->isSigned(), ctx);
                }
                int dst = (int)c.type->getBitWidth();
                if (c.conversionKind == ConversionKind::Explicit && c.type->isIntegral() &&
                    c.operand().type->isIntegral() && ctx.live && (int)c.operand().type->getBitWidth() < dst) {
                    // A cast (6.24.1) is an assignment to the cast type: the
                    // operand is extended to that width with its own
                    // signedness first, whatever the context wants after.
                    // int'(u3) in a signed 32-bit sum is 0..7, not -4..3.
                    needWidth(dst);
                    E v = expr(c.operand(), ExprCtx{dst, c.operand().type->isSigned(), c.type->isSigned(), true});
                    return sized(std::move(v), dst, c.type->isSigned(), ctx);
                }
                if (c.conversionKind == ConversionKind::Explicit && c.type->isIntegral() &&
                    c.operand().type->isIntegral() &&
                    ((int)c.operand().type->getBitWidth() > dst ||
                     ((int)c.operand().type->getBitWidth() == dst && ctx.live && ctx.w > dst))) {
                    // A narrowing cast (6.24.1): the operand is worked out at its
                    // own width, 16'((a + 32'd4) >> 1) keeps the carry, and cut to
                    // the cast's; a wider context extends only that result, so
                    // 16'(a + k) < 32 compares 16 bits.
                    needWidth(dst);
                    int ow = (int)c.operand().type->getBitWidth();
                    bool os = c.operand().type->isSigned();
                    E v = resize(expr(c.operand(), ExprCtx{ow, os, os, true}), dst, false, c.type->isSigned());
                    return sized(std::move(v), dst, c.type->isSigned(), ctx);
                }
                bool fill, tag;
                if (ctx.live && ctx.w > 0) {
                    dst = ctx.w;
                    fill = ctx.fill;
                    tag = ctx.tag;
                } else if (c.conversionKind == ConversionKind::Propagated) {
                    fill = tag = c.type->isSigned();
                } else {
                    fill = c.operand().type->isSigned();
                    tag = c.type->isSigned();
                }
                needWidth(dst);
                return expr(c.operand(), ExprCtx{dst, fill, tag, true});
            }
            case ExpressionKind::MinTypMax:   // (1:2:3) is its typical value, or min / max (11.11)
                return expr(mtmOf(e.as<MinTypMaxExpression>()), ctx);
            case ExpressionKind::UnboundedLiteral: {   // q[$]: the last index (7.10.1)
                if (dollarQ < 0) unsupported("$ outside a queue index");
                Val one = mk(1, 32); one.sgn = true;
                return sized(bin(Op::Sub, dynSize(dollarQ), konst(one)), 32, true, ctx);
            }
            case ExpressionKind::EmptyArgument: {
                if (!outSlot) unsupported("empty argument");
                return sized(ref(outSlot->sig, outSlot->sgn), outSlot->w, outSlot->sgn, ctx);
            }
            case ExpressionKind::NewCovergroup:   // new(...) of a covergroup (19.3)
                return cgNew(e, e.as<NewCovergroupExpression>().arguments);
            case ExpressionKind::Call: {
                const auto& call = e.as<CallExpression>();
                if (!call.isSystemCall()) {
                    if (E r = cgCall(call)) return r;   // c.sample(), c.get_coverage(), ... (19.8)
                    E v;
                    if (consDepth) {   // in a constraint: its own program, run at solve time (18.5.12)
                        Prog saved = std::move(prog);
                        prog = Prog{};
                        v = emitUserCall(call, true);
                        // The result into a temporary of its own: the callee's frame is gone by then.
                        int w = storeW(*e.type);
                        int t = fresh(w);
                        Instr i; i.k = IK::Assign; i.sig = t; i.e = v;
                        prog.emit(i);
                        v = ref(t, e.type->isSigned());
                        consFns.push_back(std::make_shared<Prog>(std::move(prog)));
                        prog = std::move(saved);
                    } else v = emitUserCall(call, true);
                    int w = bitsOf(*e.type);
                    needWidth(w);
                    return sized(std::move(v), w, e.type->isSigned(), ctx);
                }
                std::string_view n = call.getSubroutineName();
                if (auto vi = vpiSystfNames().find(std::string(n)); vi != vpiSystfNames().end()) {   // VPI (38.37)
                    if (vi->second.func && vi->second.ftype == 2) unsupported("a VPI real function as an integral value");
                    return sized(emitVpiCall(call, vi->second, true), (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
                }
                if (n == "randomize") return sized(randomizeCall(call), 32, true, ctx);
                if (n == "rand_mode" || n == "constraint_mode") return sized(randModeCall(call, true), 32, true, ctx);
                if ((n == "sum" || n == "product" || n == "and" || n == "or" || n == "xor") && !call.arguments().empty() &&
                    call.arguments()[0]->type->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType)
                    return sized(fixedReduce(call, n), (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
                // $time / $stime / $realtime (20.3.1): the current time. minisim
                // has one time unit, so $realtime is the same integer.
                if (n == "$time" || n == "$stime") {
                    // In the calling module's time unit (20.3.1), rounded.
                    int w = n == "$stime" ? 32 : 64;
                    E t = node(Op::Time);
                    t->k = mk(ticksPerUnit(), 64);
                    if (w != 64) t = resize(std::move(t), w, false, false);
                    return sized(std::move(t), w, false, ctx);
                }
                if (isArrayQuery(n)) return sized(arrayQuery(call, n), 32, true, ctx);
                if (n == "$typename" && call.arguments().size() == 1) {   // 20.6.1
                    if (ctx.live && !e.type->isString()) unsupported("$typename used as a number");
                    return konst(mkstr(typeName(*call.arguments()[0]->type)));
                }
                if (!call.arguments().empty() &&
                    (dynSig(*call.arguments()[0]) >= 0 || isDynCall(*call.arguments()[0]) ||
                     (isFixedArr(*call.arguments()[0]) && isReduction(n)))) {   // q.size(), aa.exists(k), a.sum() ...
                    int w = storeW(*e.type);
                    needWidth(w);
                    return sized(dynMethod(call, true), w, e.type->isSigned(), ctx);
                }
                if (isStrMethodCall(call)) {   // s.len(), s.getc(i), s.compare(t), s.atoi() ...
                    int w = (int)e.type->getBitWidth();
                    needWidth(w);
                    return sized(strMethod(call, strMethodId(n)), w, e.type->isSigned(), ctx);
                }
                if (int ix = iterIndexSig(call); ix >= 0)   // item.index in a with clause (7.12.4)
                    return sized(ref(ix, e.type->isSigned()), storeW(*e.type), e.type->isSigned(), ctx);
                if ((n == "$rtoi" || n == "$realtobits") && call.arguments().size() == 1) {
                    E r = realOf(*call.arguments()[0]);
                    int w = (int)e.type->getBitWidth();
                    needWidth(w);
                    if (n == "$rtoi") return sized(rtoi(r, w, e.type->isSigned(), true), w, e.type->isSigned(), ctx);
                    auto bits = node(Op::Resize, {r});   // the same 64 bits, as an integer
                    bits->hi = 64;
                    return sized(bits, 64, false, ctx);
                }
                // e.triggered (15.5.3): e was triggered in this time slot.
                if ((n == "triggered" || n == "matched") && call.arguments().size() == 1 &&
                    peel(*call.arguments()[0]).kind == ExpressionKind::AssertionInstance)   // a sequence's end point (16.13.6)
                    return sized(seqEndPoint(*call.arguments()[0], n == "matched"), 1, false, ctx);
                if (n == "triggered" && call.arguments().size() == 1) {
                    int s = eventSig(*call.arguments()[0]);
                    E t = node(Op::Time);
                    t->k = mk(1, 64);   // raw ticks
                    E v = bin(Op::Eq, ref(s + 1), bin(Op::Add, t, konst(mk(1, 64))));
                    return sized(std::move(v), 1, false, ctx);
                }
                if (n == "$bits" && call.arguments().size() == 1 && call.arguments()[0]->type->isFixedSize()) {   // 20.6.2
                    Val b = mk(call.arguments()[0]->type->getBitstreamWidth(), 32);
                    b.sgn = true;
                    return sized(konst(b), 32, true, ctx);
                }
                if (n == "$cast" && call.arguments().size() == 2 && isHandleType(*outArg(*call.arguments()[0]).type))
                    return sized(castClass(call, true), 1, false, ctx);
                if (E r = sysFn(call, n)) return sized(r, (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
                // $signed / $unsigned keep the bits and change the tag.
                if (n != "$signed" && n != "$unsigned") throw std::runtime_error(whereStr() + "lower: unsupported system function " + std::string(n));
                auto args = call.arguments();
                if (args.size() != 1 || args[0]->kind == ExpressionKind::EmptyArgument)
                    unsupported("system function");
                int aw = (int)args[0]->type->getBitWidth();
                needWidth(aw);
                bool toS = n == "$signed";
                E arg = expr(*args[0]);
                E cast = resize(std::move(arg), aw, false, toS);
                return sized(cast, aw, toS, ctx);
            }
            case ExpressionKind::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                if (u.op == UnaryOperator::Preincrement || u.op == UnaryOperator::Postincrement ||
                    u.op == UnaryOperator::Predecrement || u.op == UnaryOperator::Postdecrement) {
                    // ++ / -- inside an expression (11.4.2): the update runs
                    // just before the statement, and the expression reads a
                    // copy (the old value for i++, the new one for ++i).
                    if (!stmtDepth) unsupported("++ / -- outside a procedural statement");
                    int w = (int)u.operand().type->getBitWidth();
                    needWidth(w);
                    bool post = u.op == UnaryOperator::Postincrement || u.op == UnaryOperator::Postdecrement;
                    int tmp = fresh(w);
                    Instr save; save.k = IK::Assign; save.sig = tmp; save.e = expr(u.operand());
                    if (post) prog.emit(save);
                    exprStmt(e);
                    if (!post) { save.e = expr(u.operand()); prog.emit(save); }
                    return sized(ref(tmp, u.operand().type->isSigned()), w, u.operand().type->isSigned(), ctx);
                }
                // ~a, -a, +a are context-determined (11.6.1): the operand is
                // extended to the context width before the operator.
                bool bw = u.op == UnaryOperator::BitwiseNot || u.op == UnaryOperator::Minus ||
                          u.op == UnaryOperator::Plus;
                E a;
                if (bw) {
                    int w = ctx.live && ctx.w > 0 ? ctx.w : (int)e.type->getBitWidth();
                    needWidth(w);
                    bool sg = e.type->isSigned();
                    a = expr(u.operand(), ExprCtx{w, sg, sg, true});
                } else {
                    a = expr(u.operand());
                }
                E out;
                switch (u.op) {
                    case UnaryOperator::BitwiseNot: out = un(Op::Not, a); break;
                    case UnaryOperator::Plus: out = a; break;
                    case UnaryOperator::Minus: {   // 0 - a at a's width and sign
                        Val zero = mk(0, ctx.live && ctx.w > 0 ? ctx.w : (int)e.type->getBitWidth());
                        zero.sgn = e.type->isSigned();
                        out = bin(Op::Sub, konst(zero), a);
                        break;
                    }
                    case UnaryOperator::BitwiseNand: out = un(Op::LogNot, un(Op::RedAnd, a)); break;
                    case UnaryOperator::BitwiseNor: out = un(Op::LogNot, un(Op::RedOr, a)); break;
                    case UnaryOperator::BitwiseXnor: out = un(Op::Not, un(Op::RedXor, a)); break;
                    case UnaryOperator::LogicalNot: out = un(Op::LogNot, a); break;
                    case UnaryOperator::BitwiseAnd: out = un(Op::RedAnd, a); break;
                    case UnaryOperator::BitwiseOr: out = un(Op::RedOr, a); break;
                    case UnaryOperator::BitwiseXor: out = un(Op::RedXor, a); break;
                    default: unsupported("unary operator");
                }
                if (!bw) return sized(out, (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
                // Already at the context width; a cast such as unsigned'(-a) can
                // still change the sign the parent sees (6.24.1).
                if (ctx.live && ctx.w > 0 && ctx.tag != e.type->isSigned()) {
                    int w = ctx.w;
                    return resize(std::move(out), w, false, ctx.tag);
                }
                return out;
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                if (isHandleType(*b.left().type) || isHandleType(*b.right().type)) {   // handle ==, != (8.4)
                    bool eq = b.op == BinaryOperator::Equality || b.op == BinaryOperator::CaseEquality;
                    bool ne = b.op == BinaryOperator::Inequality || b.op == BinaryOperator::CaseInequality;
                    if (!eq && !ne) unsupported("this operator on class handles");
                    return sized(bin(eq ? Op::Eq : Op::Ne, exprH(b.left()), exprH(b.right())), 1, false, ctx);
                }
                if ((isDynType(*b.left().type) || isDynType(*b.right().type)) &&
                    (b.op == BinaryOperator::Equality || b.op == BinaryOperator::Inequality ||
                     b.op == BinaryOperator::CaseEquality || b.op == BinaryOperator::CaseInequality)) {
                    // Whole containers (7.6, 11.4.5): same size, and every element pair equal.
                    auto side = [&](const Expression& x, const Expression& other) {
                        int d = dynSig(x);
                        if (d >= 0) return d;
                        d = dynTemp(dynShapeOf(isDynType(*x.type) ? *x.type : *other.type));
                        assignContainer(d, x);
                        return d;
                    };
                    int da = side(b.left(), b.right()), db = side(b.right(), b.left());
                    auto n = node(Op::DynEq);
                    n->sig = da;
                    n->hi = db;
                    n->lo = b.op == BinaryOperator::CaseEquality || b.op == BinaryOperator::CaseInequality;
                    E acc = n;
                    if (b.op == BinaryOperator::Inequality || b.op == BinaryOperator::CaseInequality) acc = un(Op::LogNot, acc);
                    return sized(acc, 1, false, ctx);
                }
                if (b.left().type->isUnpackedArray() &&
                    (b.op == BinaryOperator::Equality || b.op == BinaryOperator::Inequality ||
                     b.op == BinaryOperator::CaseEquality || b.op == BinaryOperator::CaseInequality)) {
                    // Whole fixed-size arrays (7.6): every element pair compares
                    // equal, as == does on the elements (x if none differ but
                    // some compare x), or === for case equality.
                    std::vector<E> l, r;
                    arrayElems(*b.left().type, b.left(), l);
                    arrayElems(*b.right().type, b.right(), r);
                    if (l.size() != r.size()) unsupported("compare of arrays of different sizes");
                    bool cs = b.op == BinaryOperator::CaseEquality || b.op == BinaryOperator::CaseInequality;
                    E acc;
                    for (size_t k = 0; k < l.size(); k++) {
                        E m = bin(cs ? Op::CaseEq : Op::Eq, l[k], r[k]);
                        acc = acc ? bin(Op::LogAnd, acc, m) : m;
                    }
                    if (!acc) acc = konst(mk(1, 1));
                    if (b.op == BinaryOperator::Inequality || b.op == BinaryOperator::CaseInequality) acc = un(Op::LogNot, acc);
                    return sized(acc, 1, false, ctx);
                }
                if (b.left().type->isString() || b.right().type->isString()) {   // 6.16: string compare
                    int kind = -1;
                    switch (b.op) {
                        case BinaryOperator::LessThan: kind = 0; break;
                        case BinaryOperator::LessThanEqual: kind = 1; break;
                        case BinaryOperator::GreaterThan: kind = 2; break;
                        case BinaryOperator::GreaterThanEqual: kind = 3; break;
                        case BinaryOperator::Equality: case BinaryOperator::CaseEquality: kind = 4; break;
                        case BinaryOperator::Inequality: case BinaryOperator::CaseInequality: kind = 5; break;
                        default: unsupported("operator on string operands");
                    }
                    auto n = node(Op::StrCmp, {strOf(b.left()), strOf(b.right())});
                    n->hi = kind;
                    return sized(n, 1, false, ctx);
                }
                if (b.left().type->isFloating() || b.right().type->isFloating()) {
                    // A comparison or logical operator on reals: a 1-bit result.
                    auto truthOf = [&](const Expression& x) {
                        return x.type->isFloating() ? fcmp(5, exprReal(x), realK(0)) : expr(x);
                    };
                    E out;
                    switch (b.op) {
                        case BinaryOperator::LessThan: out = fcmp(0, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::LessThanEqual: out = fcmp(1, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::GreaterThan: out = fcmp(2, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::GreaterThanEqual: out = fcmp(3, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::Equality: out = fcmp(4, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::Inequality: out = fcmp(5, realOf(b.left()), realOf(b.right())); break;
                        case BinaryOperator::LogicalAnd: out = bin(Op::LogAnd, truthOf(b.left()), truthOf(b.right())); break;
                        case BinaryOperator::LogicalOr: out = bin(Op::LogOr, truthOf(b.left()), truthOf(b.right())); break;
                        default: unsupported("operator on real operands");
                    }
                    return sized(out, 1, false, ctx);
                }
                if (b.op == BinaryOperator::LogicalAnd || b.op == BinaryOperator::LogicalOr ||
                    (g_condAllops && (b.op == BinaryOperator::BinaryAnd || b.op == BinaryOperator::BinaryOr)))
                    if (E cc = condCovExpr(e)) return sized(cc, 1, false, ctx);
                bool sizeBoth = widthPropagates(b.op) || isDivMod(b.op);
                // a ** b sizes like a shift: the result is a's width, b is
                // self-determined (11.6.1).
                bool shift = isShift(b.op) || b.op == BinaryOperator::Power;
                E l, r;
                int w = 0;
                if (sizeBoth || shift) {
                    // Shifts size only the left operand. The count stays
                    // self-determined and is always treated as unsigned.
                    // A narrower context shrinks an unsized literal (`cnt + 1`
                    // is 4 bits, not 32). A sized operand that is already wider
                    // stays wide; the result is truncated afterwards
                    // (`logic [3:0] a; logic [7:0] b; a = a / b`).
                    int opW = (int)e.type->getBitWidth();
                    w = opW;
                    if (ctx.live && ctx.w > 0 && ctx.w != opW) {
                        if (ctx.w > opW) {
                            w = ctx.w;
                        } else {
                            auto eff = e.getEffectiveWidth();
                            if (!eff || (int)*eff <= ctx.w) w = ctx.w;
                        }
                    }
                    needWidth(w);
                    bool sg = e.type->isSigned();
                    ExprCtx op{w, sg, sg, true};
                    l = expr(b.left(), op);
                    r = shift ? expr(b.right()) : expr(b.right(), op);
                } else {
                    l = expr(b.left());
                    r = expr(b.right());
                }
                Op kop;
                switch (b.op) {
                    case BinaryOperator::Add: kop = Op::Add; break;
                    case BinaryOperator::Subtract: kop = Op::Sub; break;
                    case BinaryOperator::Multiply: kop = Op::Mul; break;
                    case BinaryOperator::Divide: kop = Op::Div; break;
                    case BinaryOperator::Mod: kop = Op::Mod; break;
                    case BinaryOperator::BinaryAnd: kop = Op::And; break;
                    case BinaryOperator::BinaryOr: kop = Op::Or; break;
                    case BinaryOperator::BinaryXor: kop = Op::Xor; break;
                    case BinaryOperator::Equality: kop = Op::Eq; break;
                    case BinaryOperator::Inequality: kop = Op::Ne; break;
                    case BinaryOperator::CaseEquality: kop = Op::CaseEq; break;
                    case BinaryOperator::LessThan: kop = Op::Lt; break;
                    case BinaryOperator::LessThanEqual: kop = Op::Le; break;
                    case BinaryOperator::GreaterThan: kop = Op::Gt; break;
                    case BinaryOperator::GreaterThanEqual: kop = Op::Ge; break;
                    case BinaryOperator::LogicalShiftLeft:
                    case BinaryOperator::ArithmeticShiftLeft: kop = Op::Shl; break;
                    case BinaryOperator::LogicalShiftRight: kop = Op::Shr; break;
                    case BinaryOperator::ArithmeticShiftRight:
                        kop = e.type->isSigned() ? Op::Asr : Op::Shr;
                        break;
                    case BinaryOperator::LogicalAnd: kop = Op::LogAnd; break;
                    case BinaryOperator::LogicalOr: kop = Op::LogOr; break;
                    case BinaryOperator::Power: kop = Op::Pow; break;
                    case BinaryOperator::CaseInequality: kop = Op::CaseEq; break;
                    case BinaryOperator::WildcardEquality: kop = Op::WildEq; break;
                    case BinaryOperator::WildcardInequality: kop = Op::WildEq; break;
                    case BinaryOperator::BinaryXnor: kop = Op::Xor; break;
                    case BinaryOperator::LogicalImplication: kop = Op::LogOr; l = un(Op::LogNot, l); break;
                    case BinaryOperator::LogicalEquivalence: kop = Op::Eq; l = un(Op::RedOr, l); r = un(Op::RedOr, r); break;
                    default: unsupported("binary operator");
                }
                E out = bin(kop, l, r);
                if (b.op == BinaryOperator::CaseInequality || b.op == BinaryOperator::WildcardInequality)
                    out = un(Op::LogNot, out);
                if (b.op == BinaryOperator::BinaryXnor) out = un(Op::Not, out);
                if (sizeBoth || shift) {
                    if (ctx.live && ctx.w > 0 && w != ctx.w)
                        return resize(std::move(out), ctx.w, false, ctx.tag);
                    return out;
                }
                return sized(out, (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                int w = ctx.live && ctx.w > 0 ? ctx.w : e.type->isIntegral() ? (int)e.type->getBitWidth() : storeW(*e.type);
                needWidth(w);
                bool sg = e.type->isSigned();
                ExprCtx arm{w, sg, sg, true};
                E cond = condOf(c);
                return mux(cond, expr(c.left(), arm), expr(c.right(), arm));
            }
            case ExpressionKind::Concatenation: {
                std::vector<E> parts;
                for (auto* op : e.as<ConcatenationExpression>().operands()) {
                    // {0{x}} inside a concatenation is ignored (11.4.12.1).
                    if (op->kind == ExpressionKind::Replication && constInt(op->as<ReplicationExpression>().count()) == 0)
                        continue;
                    parts.push_back(expr(*op));
                }
                if (parts.empty()) unsupported("concatenation of only zero replications");
                return sized(concat(parts), (int)e.type->getBitWidth(), false, ctx);
            }
            case ExpressionKind::ElementSelect: {   // a[i]
                if (E k = paramSelect(e, ctx)) return k;
                if (const auto& sv = e.as<ElementSelectExpression>().value();
                    ValueExpressionBase::isKind(sv.kind) && rsVals.count(&sv.as<ValueExpressionBase>().symbol)) {   // P[i] (18.17.7)
                    const auto& ts = rsVals.at(&sv.as<ValueExpressionBase>().symbol);
                    auto cv = tryConst(e.as<ElementSelectExpression>().selector());
                    if (!cv.isInteger()) unsupported("a production value with an index that is not constant");
                    int64_t n = cv.integer().as<int64_t>().value_or(0);
                    if (n < 1 || n > (int64_t)ts.size()) unsupported("production value index out of range");
                    return sized(ref(ts[(size_t)n - 1], e.type->isSigned()), storeW(*e.type), e.type->isSigned(), ctx);
                }
                if (int d = dynValue(e.as<ElementSelectExpression>().value()); d >= 0) {   // q[i], aa[key], a.max()[0]
                    int w = bitsOf(*e.type);
                    needWidth(w);
                    return sized(dynGet(d, e.as<ElementSelectExpression>().selector()), w, e.type->isSigned(), ctx);
                }
                if (auto as = arraySel(e))
                    return sized(arrayRead(*as), as->a->elemW, as->a->sgn, ctx);
                const auto& sel = e.as<ElementSelectExpression>();
                if (sel.value().type->isString()) {   // s[i]: its byte i, 0 when out of range (6.16)
                    auto m = node(Op::StrMethod, {exprStr(sel.value()), expr(sel.selector())});
                    m->hi = S_GETC;
                    return sized(std::move(m), 8, false, ctx);
                }
                PSel ps = packedSel(*sel.value().type, &sel.selector(), nullptr, 1, PSel::Index);
                return sized(readPSel(expr(sel.value()), ps), sel.value().type->isUnpackedArray() ? bitsOf(*e.type) : (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
            }
            case ExpressionKind::RangeSelect: {     // a[hi:lo], a[i +: w], a[i -: w]
                if (E k = paramSelect(e, ctx)) return k;
                const auto& r = e.as<RangeSelectExpression>();
                PSel ps = rangePSel(r);
                return sized(readPSel(expr(r.value()), ps), (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
            }
            case ExpressionKind::Assignment: {      // (a = b) inside an expression (11.3.6)
                if (!stmtDepth) unsupported("assignment inside an expression here");
                const auto& a = e.as<AssignmentExpression>();
                if (a.isNonBlocking() || a.timingControl) unsupported("assignment inside an expression");
                exprStmt(e);
                return expr(a.left(), ctx);
            }
            case ExpressionKind::Inside: {          // x inside {a, [lo:hi]} (11.4.13)
                const auto& in = e.as<InsideExpression>();
                E acc;
                for (const Expression* it : in.rangeList()) {
                    E m = insideItem(in.left(), *it);
                    acc = acc ? bin(Op::LogOr, acc, m) : m;
                }
                return sized(acc ? acc : konst(mk(0, 1)), 1, false, ctx);
            }
            case ExpressionKind::MemberAccess: {    // s.f
                const auto& m = e.as<MemberAccessExpression>();
                if (vifAccess(e)) {   // v.x, v.cb.x (25.9)
                    if (m.member.kind == SymbolKind::Subroutine) unsupported("a task or function call through a virtual interface");
                    E r = ref(-1, e.type->isSigned());
                    r->at = vifAddr(m);
                    if (isHandleType(*e.type)) return r;
                    return sized(r, storeW(*e.type), e.type->isSigned(), ctx);
                }
                if (isHandleType(*m.value().type)) {   // h.x (8.4): read through the handle when evaluated
                    // h.P, a parameter or enum value of the class (8.25): its constant
                    if (m.member.kind == SymbolKind::Parameter || m.member.kind == SymbolKind::EnumValue)
                        return constSym(m.member, e, ctx);
                    const PropSlot* ps = memberSlot(m);
                    E r;
                    if (!ps) {
                        int sg = sigFind(&m.member);
                        if (sg < 0) unsupported("static class property with no signal");
                        r = ref(sg, e.type->isSigned());
                    } else {
                        if (ps->kind != 0) unsupported("whole array or container property as a value");
                        r = ref(ps->off, e.type->isSigned());
                        r->at = exprH(m.value());
                    }
                    if (isHandleType(*e.type)) return r;
                    int src = storeW(*e.type);
                    return sized(r, src, e.type->isSigned(), ctx);
                }
                if (stmtDepth && isTaggedUnion(*m.value().type) && m.member.kind == SymbolKind::Field) {
                    // 7.3.2: reading a member other than the one the tag names is a run-time error
                    const Type& ut = m.value().type->getCanonicalType();
                    int uw = ut.isUnpackedUnion() ? uWidth(ut) : (int)ut.getBitWidth();
                    int tb = tagBitsOf(ut);
                    if (tb > 0 && uw > 0) {
                        Instr j; j.k = IK::JmpIfNot;
                        j.e = un(Op::LogNot, bin(Op::CaseEq, slice(expr(m.value()), uw - 1, uw - tb),
                                                 konst(mk((uint64_t)tagIndexOf(ut, m.member), tb))));
                        size_t jf = prog.emit(j);
                        Instr d; d.k = IK::Display;
                        std::string at = whereStr();
                        if (auto sl = at.rfind('/'); sl != std::string::npos) at = at.substr(sl + 1);
                        d.fmt = escPct("Error: " + at + "read of tagged union member '" + std::string(m.member.name) +
                                       "', which is not the member the tag names");
                        prog.emit(d);
                        prog.code[jf].target = prog.here();
                    }
                }
                                return sized(readPSel(expr(m.value()), memberPSel(m)), bitsOf(*e.type), e.type->isSigned(), ctx);
            }
            case ExpressionKind::SimpleAssignmentPattern:
            case ExpressionKind::StructuredAssignmentPattern:
            case ExpressionKind::ReplicatedAssignmentPattern: {   // '{a, b}, '{f: x}, '{n{a}} (10.9)
                if (e.type->isUnpackedStruct() || e.type->isUnpackedUnion()) return sized(structValue(e), storeW(*e.type), false, ctx);
                if (e.type->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType && uWidth(*e.type) > 0)
                    return sized(arrayBitsValue(e), uWidth(*e.type), false, ctx);   // s.arr = '{...} (7.2)
                if (!e.type->isIntegral()) unsupported("assignment pattern of an unpacked type");
                const auto& ap = static_cast<const AssignmentPatternExpressionBase&>(e);
                std::vector<E> parts;
                for (auto* el : ap.elements()) parts.push_back(expr(*el));
                if (e.kind == ExpressionKind::ReplicatedAssignmentPattern) {
                    int64_t n = constInt(e.as<ReplicatedAssignmentPatternExpression>().count());
                    std::vector<E> rep;
                    for (int64_t k = 0; k < n; k++) rep.insert(rep.end(), parts.begin(), parts.end());
                    parts = std::move(rep);
                }
                return sized(concat(parts), (int)e.type->getBitWidth(), e.type->isSigned(), ctx);
            }
            default:
                throw std::runtime_error("lower: unsupported expression kind " + std::string(toString(e.kind)));
        }
    }

    // Fill i.sig (+ i.hi/i.lo for partial writes) from an assignment target.
    void lhs(const slang::ast::Expression& e, Instr& i) {
        using namespace slang::ast;
        if (const Expression* c = modportConn(e)) { lhs(*c, i); return; }   // a modport expression (25.5.4)
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {
                if (dynFind(&e.as<ValueExpressionBase>().symbol) >= 0) unsupported("container assignment here");
                int sg = sigFind(&e.as<ValueExpressionBase>().symbol);
                if (sg < 0) unsupported("assignment to a non-signal");
                i.sig = sg;
                return;
            }
            case ExpressionKind::ElementSelect: {
                if (int d = dynSig(e.as<ElementSelectExpression>().value()); d >= 0) {   // q[i] = ..., aa[k] = ...
                    i.sig = d;
                    i.dyn = d;
                    i.args = {dynKey(d, e.as<ElementSelectExpression>().selector())};
                    return;
                }
                if (auto as = arraySel(e)) {   // mem[i] = ...
                    int64_t off = as->constant ? constOffset(*as) : -1;
                    if (off >= 0 && !isInd(as->a->base)) { i.sig = as->a->base + (int)off; return; }
                    i.sig = as->a->base;
                    i.arr = as->a;
                    for (auto* x : as->idx) i.aidx.push_back(expr(*x));
                    return;
                }
                const auto& sel = e.as<ElementSelectExpression>();
                lhs(sel.value(), i);
                writePSel(i, packedSel(*sel.value().type, &sel.selector(), nullptr, 1, PSel::Index));
                return;
            }
            case ExpressionKind::RangeSelect: {   // v[7:4] = ..., v[i +: w] = ... (11.5.1)
                const auto& r = e.as<RangeSelectExpression>();
                if (dynSig(r.value()) >= 0) unsupported("write to a queue slice");
                lhs(r.value(), i);
                writePSel(i, rangePSel(r));
                return;
            }
            case ExpressionKind::MemberAccess: {   // s.f = ...
                const auto& m = e.as<MemberAccessExpression>();
                if (vifAccess(e)) { i.sig = vifSlot(m); return; }   // v.x = ... (25.9)
                if (isHandleType(*m.value().type)) {   // h.x = ... (8.4)
                    const PropSlot* ps = memberSlot(m);
                    if (!ps) { i.sig = sigAt(&m.member); return; }
                    if (ps->kind != 0) unsupported("assignment to a whole array or container property here");
                    i.sig = indSlot(exprH(m.value()), ps->off);
                    return;
                }
                lhs(m.value(), i);
                writePSel(i, memberPSel(m));
                return;
            }
            case ExpressionKind::Conversion:
                lhs(e.as<ConversionExpression>().operand(), i);
                return;
            default: unsupported("assignment target");
        }
    }

    // ---------- statements ----------
    // Collect (signal, edge) pairs from a SignalEvent or EventList control.
    // BothEdges is approximated as ANY. (@* is handled in timing()/finishImplicit().)
    void collectEvents(const slang::ast::TimingControl& t, std::vector<std::pair<int, Edge>>& ev) {
        using namespace slang::ast;
        if (t.kind == TimingControlKind::SignalEvent) {
            const auto& s = t.as<SignalEventControl>();
            if (isGlobalClock(s.expr)) {   // @($global_clock): the global clocking's event (14.14)
                if (s.edge != EdgeKind::None || s.iffCondition) unsupported("an edge or iff on $global_clock");
                collectEvents(globalClocking()->getEvent(), ev);
                return;
            }
            if (const auto* cb = inferredClock(s.expr)) {   // @($inferred_clock) (16.14.7)
                if (s.edge != EdgeKind::None || s.iffCondition) unsupported("an edge or iff on $inferred_clock");
                collectEvents(cb->getEvent(), ev);
                return;
            }
            if (const auto* cb = clockingRef(s.expr)) {   // @(cb): the clocking block event (14.10)
                if (s.edge != EdgeKind::None || s.iffCondition) unsupported("an edge or iff on a clocking block event");
                ev.push_back({clockings()[clockingIdx(*cb)].evSig, ANY});
                return;
            }
            if (peel(s.expr).kind == ExpressionKind::AssertionInstance) {   // @(seq): its end point (9.4.2.4)
                if (s.edge != EdgeKind::None || s.iffCondition) unsupported("an edge or iff on a sequence event");
                ev.push_back({seqEventSig(s.expr), ANY});
                return;
            }
            int sig;
            if (classMember(s.expr) || vifAccess(s.expr)) sig = eventSig(s.expr);   // @(h.e), @(h.x), @(v.x)
            else if (ValueExpressionBase::isKind(s.expr.kind) && eventPtr().count(&s.expr.as<ValueExpressionBase>().symbol))
                sig = eventSig(s.expr);   // an event that may be an alias (6.17)
            else if (ValueExpressionBase::isKind(s.expr.kind) && !modportConn(s.expr)) {
                sig = sigAt(&s.expr.as<ValueExpressionBase>().symbol);
            } else {
                // @(posedge clk[i]), @(a & b): wait on a signal that a
                // continuous assignment keeps equal to the expression; an
                // edge is that value's least significant bit (9.4.2).
                if (!eventExprHook()) unsupported("event on non-signal");
                sig = eventExprHook()(s.expr);
            }
            Edge e = s.edge == EdgeKind::PosEdge ? POS : s.edge == EdgeKind::NegEdge ? NEG : ANY;
            ev.push_back({sig, e});
        } else if (t.kind == TimingControlKind::EventList) {
            for (auto* sub : t.as<EventListControl>().events) collectEvents(*sub, ev);
        } else {
            unsupported("timing control");
        }
    }

    void timing(const slang::ast::TimingControl& t) {
        using namespace slang::ast;
        switch (t.kind) {
            case TimingControlKind::Delay: {
                const auto& d = t.as<DelayControl>();
                Instr i; i.k = IK::WaitDelay;
                delayInto(d.expr, i);
                prog.emit(i);
                break;
            }
            case TimingControlKind::CycleDelay:   // ##n (14.11)
                cycleDelay(t.as<CycleDelayControl>());
                break;
            case TimingControlKind::ImplicitEvent: {   // @*  -> patched once the whole block is lowered
                Instr i; i.k = IK::WaitEdge;
                implicitWaits.push_back(prog.emit(i));
                break;
            }
            default: {  // SignalEvent or EventList: @(posedge a or negedge b ...)
                // @(ev iff c) (9.4.2.3): the event counts only when c is true
                // as it happens, so wait again otherwise.
                const Expression* iff = nullptr;
                if (t.kind == TimingControlKind::SignalEvent) iff = t.as<SignalEventControl>().iffCondition;
                else if (t.kind == TimingControlKind::EventList)
                    for (auto* sub : t.as<EventListControl>().events)
                        if (sub->kind == TimingControlKind::SignalEvent && sub->as<SignalEventControl>().iffCondition)
                            unsupported("iff in an event list");
                Instr i; i.k = IK::WaitEdge;
                collectEvents(t, i.events);
                i.sig = i.events[0].first;
                i.edge = i.events[0].second;
                size_t top = prog.emit(i);
                if (iff) {
                    Instr j; j.k = IK::JmpIfNot; j.e = expr(*iff); j.target = top;
                    prog.emit(j);
                }
            }
        }
    }

    // Each string-literal argument is format text; any other argument
    // fills the next specifier, or prints with the default if none is
    // waiting ($display(a, b) prints both in decimal).
    // The hierarchical name %m prints (21.2.1.1): the scope of the call, such
    // as top.u1 or top.u1.blk.
    static std::string scopeName(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        const auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine);
        if (!sci || !sci->scope) return "top";
        const Symbol* s = &sci->scope->asSymbol();
        if (s->kind == SymbolKind::InstanceBody) {
            const auto* inst = s->as<InstanceBodySymbol>().parentInstance;
            if (inst) return inst->getHierarchicalPath();
        }
        return s->getHierarchicalPath();
    }
    std::string hierName = "top";   // what %m prints for the call being lowered
    std::string libName = "work.top";   // what %l prints (33.7)
    // The library.cell bound to the instance containing the call (33.7).
    static std::string libCell(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        const auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine);
        for (const Scope* sc = sci && sci->scope ? sci->scope.get() : nullptr; sc; ) {
            const Symbol& s = sc->asSymbol();
            if (s.kind == SymbolKind::InstanceBody) {
                const auto& def = s.as<InstanceBodySymbol>().getDefinition();
                return def.sourceLibrary.name + "." + std::string(def.name);
            }
            sc = s.getParentScope();
        }
        return "work.top";
    }
    // %m (21.2.1.1) and %l (33.7) are known when lowering: put them in the text.
    static std::string substM(const std::string& f, const std::string& name, const std::string& lib = "") {
        std::string esc, escL, r;
        for (char c : lib) { escL += c; if (c == '%') escL += '%'; }
        for (char c : name) { esc += c; if (c == '%') esc += '%'; }
        for (size_t p = 0; p < f.size(); p++) {
            if (f[p] != '%') { r += f[p]; continue; }
            if (p + 1 < f.size() && f[p + 1] == '%') { r += "%%"; p++; continue; }
            size_t q = p + 1;
            while (q < f.size() && (isdigit((unsigned char)f[q]) || f[q] == '.' || f[q] == '-')) q++;
            if (q < f.size() && (f[q] == 'm' || f[q] == 'M')) { r += esc; p = q; continue; }
            if (q < f.size() && (f[q] == 'l' || f[q] == 'L') && !lib.empty()) { r += escL; p = q; continue; }
            r += f[p];
        }
        return r;
    }
    // A value read as text with format letter c (a string for %s, a real for
    // %f %e %g, else a 64-bit integer), converted for a write to type dst.
    E readAs(E v, char c, const slang::ast::Type& dst, const char* what) {
        bool txt = c == 's', rl = c == 'f' || c == 'e' || c == 'g';
        int w = (int)dst.getBitWidth();
        if (dst.isString()) {
            if (!txt) unsupported((std::string(what) + " into a string with a number format").c_str());
            return v;
        }
        if (dst.isFloating()) return rl ? v : txt ? v : un(Op::IToR, v);
        if (txt) { v = node(Op::StrToInt, {v}); v->hi = w; return v; }
        if (rl) return rtoi(v, w, dst.isSigned(), false);
        // %d reads a signed number; %h, %o, %b digits are zero-extended (21.3.4.3)
        return resize(v, w, c == 'd', dst.isSigned());
    }
    // $fscanf / $sscanf (21.3.4): one node reads everything; then each output
    // that a conversion reached is written. Returns the count.
    E scanCall(const slang::ast::CallExpression& call, bool file) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (args.size() < 2) unsupported("$fscanf / $sscanf arguments");
        if (!stmtDepth) unsupported("$fscanf / $sscanf outside a procedural statement");
        const Expression& f = peel(*args[1]);
        if (f.kind != ExpressionKind::StringLiteral) unsupported("$fscanf / $sscanf format that is not a literal");
        std::string fs(f.as<StringLiteral>().getValue());
        std::vector<char> letters;   // one per conversion that is stored
        for (size_t i = 0; i < fs.size(); i++) {
            if (fs[i] != '%' || i + 1 >= fs.size()) continue;
            i++;
            if (fs[i] == '%') continue;
            bool skip = fs[i] == '*';
            if (skip) i++;
            while (i < fs.size() && isdigit((unsigned char)fs[i])) i++;
            if (i >= fs.size()) break;
            char c = (char)tolower((unsigned char)fs[i]);
            if (std::string_view("dhxobcsfegtum").find(c) == std::string_view::npos)
                unsupported(("$fscanf / $sscanf format %" + std::string(1, fs[i])).c_str());
            if (!skip) letters.push_back(c);
        }
        if (letters.size() != args.size() - 2) unsupported("$fscanf / $sscanf with a different number of formats and outputs");
        E src = file ? expr(*args[0]) : strOf(*args[0]);
        int cnt = fresh(32);
        Instr t; t.k = IK::Assign; t.sig = cnt; t.e = sysFnNode(SF_SCAN, {src, konst(mkstr(fs))}, file ? 1 : 0);
        prog.emit(t);
        for (size_t k = 0; k < letters.size(); k++) {
            if (letters[k] == 'm') continue;
            Instr j; j.k = IK::JmpIfNot;
            j.e = bin(Op::Gt, ref(cnt, true), konst(make(k, 0, 32, 0, true)));
            size_t jf = prog.emit(j);
            const Expression& dst = outArg(*args[k + 2]);
            Instr w; w.k = IK::Assign;
            lhs(dst, w);
            w.e = readAs(sysFnNode(SF_SCAN_GET, {konst(mk(k, 32))}), letters[k], *dst.type, "$fscanf / $sscanf");
            prog.emit(w);
            prog.code[jf].target = prog.here();
        }
        return ref(cnt, true);
    }
    // $fdisplay / $fwrite and their b / o / h forms (21.3.2).
    E fileWriteCall(const slang::ast::CallExpression& call, std::string_view n) {
        auto args = call.arguments();
        if (args.empty()) unsupported("$fdisplay / $fwrite without a descriptor");
        char dflt = 'd';
        std::string_view base = n;
        if (base.ends_with('b') || base.ends_with('o') || base.ends_with('h')) { dflt = base.back(); base.remove_suffix(1); }
        auto f = node(Op::SFormat);
        hierName = scopeName(call); libName = libCell(call);
        formatInto(args.subspan(1), dflt, f->fmt, f->kids);
        return sysFnNode(SF_FWRITE, {expr(*args[0]), f}, base == "$fdisplay" ? 1 : 0);
    }
    // $q_initialize, $q_add, $q_remove, $q_full, $q_exam (20.15): the kernel
    // keeps the queues; the status (and the other outputs) are written back.
    E queueCall(const slang::ast::CallExpression& call, std::string_view n) {
        auto args = call.arguments();
        if (!stmtDepth) unsupported("a $q_ call outside a procedural statement");
        auto in = [&](size_t k) { return resize(expr(*args[k]), 32, args[k]->type->isSigned(), true); };
        auto out = [&](size_t k, E v) {
            const auto& dst = outArg(*args[k]);
            Instr w; w.k = IK::Assign;
            lhs(dst, w);
            w.e = resize(v, (int)dst.type->getBitWidth(), true, dst.type->isSigned());
            prog.emit(w);
        };
        auto run = [&](int ctl, std::vector<E> kids, int w) {
            int tmp = fresh(w);
            Instr h; h.k = IK::Assign; h.sig = tmp; h.e = sysFnNode(SF_QUEUE, std::move(kids), ctl);
            prog.emit(h);
            return tmp;
        };
        auto last = [&](int ctl) { Val one = mk(0, 32); return resize(sysFnNode(SF_QUEUE, {konst(one)}, ctl), 32, true, true); };
        if (n == "$q_initialize" && args.size() == 4) { out(3, ref(run(0, {in(0), in(1), in(2)}, 32), true)); return konst(mk(0, 1)); }
        if (n == "$q_add" && args.size() == 4) { out(3, ref(run(1, {in(0), in(1), in(2)}, 32), true)); return konst(mk(0, 1)); }
        if (n == "$q_remove" && args.size() == 4) {
            int st = run(2, {in(0)}, 32);
            Instr j; j.k = IK::JmpIfNot; j.e = bin(Op::Eq, ref(st), konst(mk(0, 32)));   // the outputs change only on success
            size_t jf = prog.emit(j);
            out(1, last(3));
            out(2, last(4));
            prog.code[jf].target = prog.here();
            out(3, ref(st, true));
            return konst(mk(0, 1));
        }
        if (n == "$q_full" && args.size() == 2) {
            int r = run(5, {in(0)}, 64);
            out(1, slice(ref(r), 31, 0));
            return resize(slice(ref(r), 63, 32), 32, false, true);
        }
        if (n == "$q_exam" && args.size() == 4) {
            int st = run(6, {in(0), in(1), konst(mk(ticksPerUnit(), 64))}, 32);
            out(2, last(7));
            out(3, ref(st, true));
            return konst(mk(0, 1));
        }
        unsupported("this $q_ call");
    }
    // $async$and$array and the other PLA tasks (20.16). The personality
    // memory has one word per output term and one bit per input term, both
    // counted from the left. A sync task evaluates once, where it is called;
    // an async one also starts a process that evaluates again whenever an
    // input term or a memory word changes.
    bool plaCall(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        bool async = n.starts_with("$async$");
        if (!async && !n.starts_with("$sync$")) return false;
        std::string_view rest = n.substr(async ? 7 : 6);
        int logic = -1;
        for (auto [name, id] : {std::pair<std::string_view, int>{"and$", 0}, {"or$", 1}, {"nand$", 2}, {"nor$", 3}})
            if (rest.starts_with(name)) { logic = id; rest.remove_prefix(name.size()); break; }
        if (logic < 0 || (rest != "array" && rest != "plane")) return false;
        bool plane = rest == "plane";
        auto args = call.arguments();
        if (args.size() != 3) unsupported("PLA task arguments");
        const Expression& m = peel(*args[0]);
        if (!ValueExpressionBase::isKind(m.kind)) unsupported("PLA personality that is not a memory");
        auto shape = arrFind(&m.as<ValueExpressionBase>().symbol);
        if (!shape || shape->dims.size() != 1) unsupported("PLA personality that is not a one-dimensional memory");
        int nin = shape->elemW, nout = shape->count;
        const Expression& dst = outArg(*args[2]);
        if ((int)args[1]->type->getBitWidth() != nin || (int)dst.type->getBitWidth() != nout)
            unsupported("PLA terms that do not match the personality memory");
        bool isAnd = logic == 0 || logic == 2, inv = logic >= 2;
        auto build = [&](Lowerer& L) {
            E in = L.expr(*args[1]);
            std::vector<E> outs;
            for (int k = 0; k < nout; k++) {
                E word = ref(shape->base + k);
                E terms;
                if (!plane) {   // 1: take the input, 0: leave it out
                    terms = isAnd ? bin(Op::Or, in, un(Op::Not, word)) : bin(Op::And, in, word);
                } else {        // 1 the input, 0 its complement, x the worst case, z / ? no part
                    std::vector<E> bits;
                    E neutral = konst(mk(isAnd ? 1 : 0, 1));
                    for (int b = nin - 1; b >= 0; b--) {
                        E mb = slice(word, b, b), ib = slice(in, b, b);
                        Val z1 = make(0, 1, 1, 1), x1 = make(0, 1, 1, 0);
                        E t = mux(bin(Op::CaseEq, mb, konst(mk(1, 1))), ib,
                                  mux(bin(Op::CaseEq, mb, konst(mk(0, 1))), un(Op::Not, ib),
                                      mux(bin(Op::CaseEq, mb, konst(z1)), neutral, konst(x1))));
                        bits.push_back(t);
                    }
                    terms = bits.size() == 1 ? bits[0] : concat(bits);
                }
                E o = un(isAnd ? Op::RedAnd : Op::RedOr, terms);
                outs.push_back(inv ? un(Op::Not, o) : o);
            }
            E all = outs.size() == 1 ? outs[0] : concat(outs);
            if (dst.kind == ExpressionKind::Concatenation) {   // {b1, b2, b3}: one write per part
                int tmp = L.fresh(nout);
                Instr t; t.k = IK::Assign; t.sig = tmp; t.e = all;
                L.prog.emit(t);
                std::vector<std::tuple<const Expression*, int, int>> parts;
                concatParts(dst, nout - 1, parts);
                for (auto& [x, hi, lo] : parts) {
                    Instr w; w.k = IK::Assign;
                    L.lhs(*x, w);
                    w.e = slice(ref(tmp), hi, lo);
                    L.prog.emit(w);
                }
                return;
            }
            Instr w; w.k = IK::Assign;
            L.lhs(dst, w);
            w.e = all;
            L.prog.emit(w);
        };
        build(*this);
        if (!async) return true;
        if (!frames.empty() || !store) unsupported("an asynchronous PLA task in an automatic context");
        Lowerer lw(sigmap);
        lw.bind(subs, mkSig, tempSeq, store);
        lw.setScope(scope);
        std::vector<std::pair<int, Edge>> ev;
        {   // wait first: the call itself did the first evaluation
            Lowerer probe(sigmap);
            probe.bind(subs, mkSig, tempSeq, store);
            probe.setScope(scope);
            build(probe);
            ev = combReadSet(probe.prog);
        }
        if (ev.empty()) return true;
        lw.prog.emit(waitInstr(ev));
        build(lw);
        Instr j; j.k = IK::Jmp; j.target = 0; lw.prog.emit(j);
        store->push_back(std::make_unique<Prog>(std::move(lw.prog)));
        Instr f; f.k = IK::Fork; f.join = 2;
        f.branches.push_back(store->back().get());
        f.blays.push_back(nullptr);
        prog.emit(f);
        return true;
    }
    // $fgets(str, fd) (21.3.4): the line is written only if one was read.
    E fgetsCall(const slang::ast::CallExpression& call) {
        auto args = call.arguments();
        if (args.size() != 2) unsupported("$fgets arguments");
        if (!stmtDepth) unsupported("$fgets outside a procedural statement");
        int cnt = fresh(32);
        Instr t; t.k = IK::Assign; t.sig = cnt; t.e = sysFnNode(SF_FGETS, {expr(*args[1])});
        prog.emit(t);
        Instr j; j.k = IK::JmpIfNot; j.e = ref(cnt);
        size_t jf = prog.emit(j);
        const auto& dst = outArg(*args[0]);
        Instr w; w.k = IK::Assign;
        lhs(dst, w);
        w.e = readAs(sysFnNode(SF_SCAN_TEXT, {}), 's', *dst.type, "$fgets");
        prog.emit(w);
        prog.code[jf].target = prog.here();
        return ref(cnt, true);
    }
    // $fread (21.3.4.4): binary, big-endian, (w + 7) / 8 bytes per value.
    // Into a variable: written when any byte was read. Into a memory: an
    // instruction loads elements from start (default: the lowest address)
    // upward, at most count of them, until the file ends. The result is the
    // number of bytes read.
    E freadCall(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (args.size() < 2) unsupported("$fread arguments");
        if (!stmtDepth) unsupported("$fread outside a procedural statement");
        const Expression& dst = outArg(*args[0]);
        const Type& t = dst.type->getCanonicalType();
        int cnt = fresh(32);
        if (t.isUnpackedArray()) {
            const Symbol* msym = nullptr;
            const Expression& m = peel(dst);
            if (m.kind == ExpressionKind::NamedValue) msym = &m.as<NamedValueExpression>().symbol;
            auto it = msym ? arrMap().find(msym) : arrMap().end();
            if (it == arrMap().end()) unsupported("$fread into this array");
            if (it->second->dims.size() != 1) unsupported("$fread into a multidimensional array");
            const Type& el = t.getArrayElementType()->getCanonicalType();
            if (!el.isIntegral()) unsupported("$fread into an array of non-integral elements");
            Instr i; i.k = IK::ReadMem; i.arr = it->second; i.pw = 1;   // pw 1: $fread
            i.dres = cnt;
            i.args.push_back(expr(*args[1]));
            for (size_t a = 2; a < 4; a++)
                i.args.push_back(a < args.size() && args[a]->kind != ExpressionKind::EmptyArgument ? expr(*args[a]) : E());
            prog.emit(i);
            return ref(cnt, true);
        }
        if (!t.isIntegral()) unsupported("$fread into a non-integral variable (unpacked structs not yet)");
        int w = (int)t.getBitWidth();
        Instr r; r.k = IK::Assign; r.sig = cnt;
        r.e = sysFnNode(SF_FREAD, {expr(*args[1]), konst(mk((w + 7) / 8, 32))});
        prog.emit(r);
        Instr j; j.k = IK::JmpIfNot; j.e = ref(cnt);
        size_t jf = prog.emit(j);
        Instr wr; wr.k = IK::Assign;
        lhs(dst, wr);
        wr.e = sysFnNode(SF_FREAD_GET, {}, w);
        prog.emit(wr);
        prog.code[jf].target = prog.here();
        return ref(cnt, true);
    }
    // Enumeration methods (6.19.5): first, last, num, next(N), prev(N), name.
    // Each is a chain of compares against the declared values. A value that
    // is not a member gives the type's default for next / prev and "" for name.
    E enumMethod(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (!call.isSystemCall() || args.empty() || call.getSubroutineName()[0] == '$') return nullptr;
        const Type& t = args[0]->type->getCanonicalType();
        if (t.kind != SymbolKind::EnumType) return nullptr;
        std::string_view n = call.getSubroutineName();
        std::vector<const EnumValueSymbol*> vals;
        for (const auto& ev : t.as<EnumType>().values()) vals.push_back(&ev);
        int w = (int)t.getBitWidth();
        bool sg = t.isSigned();
        auto lit = [&](size_t k) { return resize(svLit(vals[k]->getValue().integer(), sg, {}), w, sg, sg); };
        if (vals.empty()) unsupported("method of an empty enum");
        if (n == "first") return lit(0);
        if (n == "last") return lit(vals.size() - 1);
        if (n == "num") return konst(make(vals.size(), 0, 32, 0, true));
        E v = expr(*args[0]);
        if (n == "name") {
            E r = konst(mkstr(""));
            for (size_t k = vals.size(); k-- > 0;)
                r = mux(bin(Op::CaseEq, v, lit(k)), konst(mkstr(std::string(vals[k]->name))), r);
            return r;
        }
        if (n == "next" || n == "prev") {
            int64_t step = 1;
            if (args.size() > 1) {
                if (!args[1]->getConstant() && args[1]->kind != ExpressionKind::IntegerLiteral)
                    unsupported("enum next(N) / prev(N) with N not a constant");
                step = constInt(*args[1]);
            }
            int64_t cnt = (int64_t)vals.size();
            int64_t sh = ((n == "next" ? step : -step) % cnt + cnt) % cnt;
            Val d = t.isFourState() ? xfill(w, sg) : make(0, 0, w, 0, sg);
            E r = konst(d);
            for (size_t k = vals.size(); k-- > 0;)
                r = mux(bin(Op::CaseEq, v, lit(k)), lit((size_t)(((int64_t)k + sh) % cnt)), r);
            return r;
        }
        return nullptr;
    }
    // $cast(dst, src) (6.24.2) for integral, real and enum types: the cast
    // is valid unless dst is an enum and src is not one of its values. A valid
    // cast writes dst and gives 1; an invalid one leaves dst alone and gives 0,
    // and as a task it is a run-time error.
    bool asTask = false;   // the system call being lowered is a statement
    bool voidCast = false; // ... written as void'(f(...)): still the function form
    E castCall(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (args.size() != 2) unsupported("$cast arguments");
        if (!stmtDepth) unsupported("$cast outside a procedural statement");
        bool task = asTask;
        const Expression& dst = outArg(*args[0]);
        const Expression& src = *args[1];
        const Type& dt = dst.type->getCanonicalType();
        const Type& st = src.type->getCanonicalType();
        if (dt.isClass() || st.isClass()) unsupported("$cast of these types");
        if (!(dt.isIntegral() || dt.isFloating()) || !(st.isIntegral() || st.isFloating())) {
            // Strings, unpacked structs and arrays (6.24.2): the cast is the
            // assignment dst = type(dst)'(src), which is always valid when
            // the types are cast compatible; dst gets the converted value.
            if (!dt.isCastCompatible(st)) unsupported("$cast of these types");
            static thread_local std::deque<ConversionExpression> convs;
            static thread_local std::deque<AssignmentExpression> asgs;
            Expression* rhs = const_cast<Expression*>(&src);
            if (!dt.isMatching(st))
                rhs = &convs.emplace_back(*dst.type, ConversionKind::Explicit, *rhs, src.sourceRange);
            auto& a = asgs.emplace_back(std::nullopt, false, *dst.type, const_cast<Expression&>(dst), *rhs, nullptr,
                                        call.sourceRange);
            int ok = fresh(1);
            Instr o; o.k = IK::Assign; o.sig = ok; o.e = konst(mk(1, 1));
            prog.emit(o);
            int saved = castOk;
            castOk = ok;
            exprStmt(a);
            castOk = saved;
            if (task) {   // as a task, a failed cast is a run-time error (6.24.2)
                Instr j; j.k = IK::JmpIfNot; j.e = un(Op::LogNot, ref(ok));
                size_t jo = prog.emit(j);
                Instr d; d.k = IK::Display; d.fmt = "Error: $cast failed: the source has the wrong number of elements";
                prog.emit(d);
                prog.code[jo].target = prog.here();
            }
            return resize(ref(ok), 32, false, true);
        }
        int w = (int)dt.getBitWidth();
        E v;
        if (dt.isFloating()) v = realOf(src);
        else if (st.isFloating()) v = rtoi(realOf(src), w, dt.isSigned(), false);
        else v = resize(expr(src), w, st.isSigned(), dt.isSigned());
        int tmp = fresh(dt.isFloating() ? 64 : w);
        Instr h; h.k = IK::Assign; h.sig = tmp; h.e = v;
        prog.emit(h);
        E val = ref(tmp, dt.isSigned());
        E okE = konst(mk(1, 1));
        if (dt.kind == SymbolKind::EnumType) {
            okE = nullptr;
            for (const auto& ev : dt.as<EnumType>().values()) {
                E m = bin(Op::CaseEq, val, resize(svLit(ev.getValue().integer(), dt.isSigned(), {}), w, dt.isSigned(), dt.isSigned()));
                okE = okE ? bin(Op::LogOr, okE, m) : m;
            }
            if (!okE) okE = konst(mk(0, 1));
        }
        int ok = fresh(1);
        Instr o; o.k = IK::Assign; o.sig = ok; o.e = okE;
        prog.emit(o);
        Instr j; j.k = IK::JmpIfNot; j.e = ref(ok);
        size_t jf = prog.emit(j);
        Instr wr; wr.k = IK::Assign;
        lhs(dst, wr);
        wr.e = dt.isFloating() ? un(Op::AsReal, ref(tmp)) : val;
        prog.emit(wr);
        if (task) {
            size_t js = emitJmp();
            prog.code[jf].target = prog.here();
            Instr d; d.k = IK::Display;
            d.fmt = "Error: $cast failed: the value is not a valid " + escPct(std::string(dt.name.empty() ? "enum" : dt.name)) + " value";
            prog.emit(d);
            prog.code[js].target = prog.here();
        } else {
            prog.code[jf].target = prog.here();
        }
        return resize(ref(ok), 32, false, true);
    }
    // One item of an inside list (11.4.13), also a case inside label
    // (12.5.4): [lo:hi] is a range, anything else matches with ==?.
    E insideItem(const slang::ast::Expression& l, const slang::ast::Expression& it) {
        using namespace slang::ast;
        auto pair = [&](const Expression& x, const Expression& y) {
            int w = std::max((int)x.type->getBitWidth(), (int)y.type->getBitWidth());
            needWidth(w);
            bool sg = x.type->isSigned() && y.type->isSigned();
            ExprCtx c{w, sg, sg, true};
            return std::pair<E, E>{expr(x, c), expr(y, c)};
        };
        auto isReal = [](const Expression& x) { return x.type->isFloating(); };
        if (it.kind == ExpressionKind::ValueRange) {
            const auto& vr = it.as<ValueRangeExpression>();
            if (vr.rangeKind == ValueRangeKind::Simple) {
                if (isReal(l) || isReal(vr.left()) || isReal(vr.right()))
                    return bin(Op::LogAnd, fcmp(3, realOf(l), realOf(vr.left())), fcmp(1, realOf(l), realOf(vr.right())));
                auto [l1, lo] = pair(l, vr.left());
                auto [l2, hi] = pair(l, vr.right());
                return bin(Op::LogAnd, bin(Op::Ge, l1, lo), bin(Op::Le, l2, hi));
            }
            // [A +/- B] is [A-B : A+B]; [A +%- B] is [A - A*B/100.0 : A + A*B/100.0]
            // (11.4.13), compared as reals.
            E a = realOf(vr.left()), b = realOf(vr.right());
            if (vr.rangeKind == ValueRangeKind::RelativeTolerance) {
                Val h = mkreal(100.0);
                b = bin(Op::FDiv, bin(Op::FMul, a, b), konst(h));
            }
            E x = realOf(l);
            return bin(Op::LogAnd, fcmp(3, x, bin(Op::FSub, a, b)), fcmp(1, x, bin(Op::FAdd, a, b)));
        }
        if (it.type->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType) {
            // an unpacked array as an item: any of its elements (11.4.13)
            std::vector<E> els;
            arrayElems(*it.type, it, els);
            const Type& et = *it.type->getCanonicalType().getArrayElementType();
            if (!et.isIntegral() || !l.type->isIntegral()) unsupported("inside an array of non-integral elements");
            int w = std::max((int)l.type->getBitWidth(), (int)et.getBitWidth());
            bool sg = l.type->isSigned() && et.isSigned();
            ExprCtx c{w, sg, sg, true};
            E lv = expr(l, c);
            E acc;
            for (auto& el : els) {
                E m = bin(Op::WildEq, lv, resize(el, w, et.isSigned(), sg));
                acc = acc ? bin(Op::LogOr, acc, m) : m;
            }
            return acc ? acc : konst(mk(0, 1));
        }
        if (int d = dynSig(it); d >= 0) {   // a queue or dynamic array as an item
            if (dynShape(d).kind == 2) unsupported("an associative array inside an inside list");
            if (!l.type->isIntegral()) unsupported("inside a container with a non-integral value");
            auto n = node(Op::DynHas, {expr(l)});
            n->sig = d;
            return n;
        }
        if (isReal(l) || isReal(it)) return fcmp(4, realOf(l), realOf(it));
        if (l.type->isString() || it.type->isString()) {   // strings compare by text (11.4.13)
            auto c = node(Op::StrCmp, {strOf(l), strOf(it)});
            c->hi = 4;
            return c;
        }
        if (isHandleType(*l.type) && isHandleType(*it.type)) return bin(Op::Eq, exprH(l), exprH(it));
        if (!it.type->isIntegral() || !l.type->isIntegral()) unsupported("inside on a non-integral item");
        auto [a, b] = pair(l, it);
        return bin(Op::WildEq, a, b);
    }
    // The storage bits of a value of type t (see uWidth): a real as its 64
    // bits, a string as its pool id, a struct as its members.
    E storageOf(const slang::ast::Type& t0, const slang::ast::Expression& x) {
        const auto& t = t0.getCanonicalType();
        if (t.isFloating()) { auto b = node(Op::Resize, {realOf(x)}); b->hi = 64; return b; }
        if (t.isString()) return resize(strOf(x), 64, false, false);
        int w = bitsOf(t);
        if (t.isIntegral()) return resize(expr(x), w, x.type->isSigned(), false);
        return resize(expr(x), w, false, false);
    }
    // '{a, b} or '{n{a}} for an array held as bits of an unpacked struct (7.2): the left element highest.
    E arrayBitsValue(const slang::ast::Expression& e) {
        using namespace slang::ast;
        const auto& a = e.type->getCanonicalType().as<FixedSizeUnpackedArrayType>();
        const auto& ap = static_cast<const AssignmentPatternExpressionBase&>(e);
        int64_t reps = e.kind == ExpressionKind::ReplicatedAssignmentPattern
                           ? constInt(e.as<ReplicatedAssignmentPatternExpression>().count()) : 1;
        std::vector<E> parts;
        for (int64_t r = 0; r < reps; r++)
            for (auto* el : ap.elements()) parts.push_back(storageOf(a.elementType, *el));
        if ((int64_t)parts.size() != (int64_t)a.range.width()) unsupported("array pattern with the wrong number of elements");
        return parts.size() == 1 ? parts[0] : node(Op::Concat, std::move(parts));
    }
    // '{a, b, c} or '{f: v, default: d} for an unpacked struct (10.9.2): slang
    // gives one element per member, in member order.
    E structValue(const slang::ast::Expression& e) {
        using namespace slang::ast;
        const Type& t = e.type->getCanonicalType();
        if (t.isUnpackedUnion()) unsupported("assignment pattern of an unpacked union");
        if (e.kind == ExpressionKind::ReplicatedAssignmentPattern) unsupported("replicated pattern of a struct");
        const auto& ap = static_cast<const AssignmentPatternExpressionBase&>(e);
        std::vector<const FieldSymbol*> fs;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) fs.push_back(&f);
        auto els = ap.elements();
        if (els.size() != fs.size()) unsupported("struct pattern");
        std::vector<E> parts;
        for (size_t k = 0; k < fs.size(); k++) parts.push_back(storageOf(fs[k]->getType(), *els[k]));
        return parts.size() == 1 ? parts[0] : node(Op::Concat, std::move(parts));
    }
    // The default value of an unpacked struct (6.8): x for 4-state members,
    // 0 for 2-state members and reals, "" for strings.
    E structDefault(const slang::ast::Type& t0) {
        using namespace slang::ast;
        const auto& t = t0.getCanonicalType();
        if (t.kind == SymbolKind::FixedSizeUnpackedArrayType) {   // an array member: each element's default
            const auto& a = t.as<FixedSizeUnpackedArrayType>();
            const Type& et = a.elementType.getCanonicalType();
            E one;
            if (et.isUnpackedStruct() || et.isUnpackedUnion() || et.kind == SymbolKind::FixedSizeUnpackedArrayType) one = structDefault(et);
            else if (et.isIntegral() && et.isFourState()) one = konst(xfill(uWidth(et)));
            else if (et.isString()) one = konst(make(mkstr("").v, 0, 64, 0));
            else one = konst(make(0, 0, uWidth(et), 0));
            std::vector<E> parts((size_t)a.range.width(), one);
            return parts.size() == 1 ? parts[0] : node(Op::Concat, std::move(parts));
        }
        std::vector<E> parts;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
            const Type& ft = f.getType().getCanonicalType();
            int w = uWidth(ft);
            if (ft.isUnpackedStruct() || ft.kind == SymbolKind::FixedSizeUnpackedArrayType) parts.push_back(structDefault(ft));
            else if (ft.isIntegral() && ft.isFourState()) parts.push_back(konst(xfill(w)));
            else if (ft.isString()) parts.push_back(konst(make(mkstr("").v, 0, 64, 0)));
            else parts.push_back(konst(make(0, 0, w, 0)));
            if (t.isUnpackedUnion()) break;
        }
        if (t.isUnpackedUnion()) {
            int w = uWidth(t);
            return resize(parts[0], w, false, false);
        }
        return parts.size() == 1 ? parts[0] : node(Op::Concat, std::move(parts));
    }
    // $clog2, $countbits and friends (20.8, 20.9), $random / $dist_uniform
    // (20.14), $urandom / $urandom_range (18.13). Null if n is none of them.
    E sysFnNode(int id, std::vector<E> kids, int ctl = 0) {
        auto n = node(Op::SysFn, std::move(kids));
        n->hi = id;
        n->lo = ctl;
        return n;
    }
    // The control bits of $countbits: each argument is a 1-bit constant.
    int countCtl(std::span<const slang::ast::Expression* const> ctl) {
        using namespace slang::ast;
        int m = 0;
        for (const Expression* c : ctl) {
            const Expression& x = peel(*c);
            if (x.kind != ExpressionKind::IntegerLiteral) unsupported("$countbits control that is not a literal");
            auto bit = x.as<IntegerLiteral>().getValue()[0];
            m |= bit.value == slang::logic_t::Z_VALUE ? 8 : bit.isUnknown() ? 4 : bit.value ? 2 : 1;
        }
        return m;
    }
    // f(seed, ...) where seed is inout: one node gives {new seed, value}; the
    // seed is written back just before the statement, the value read after.
    E seededCall(int id, const slang::ast::Expression& seedArg, std::vector<E> more) {
        if (!stmtDepth) unsupported("$random(seed) outside a procedural statement");
        const slang::ast::Expression& sd = outArg(seedArg);
        std::vector<E> kids{expr(sd)};
        for (auto& m : more) kids.push_back(m);
        int tmp = fresh(64);
        Instr h; h.k = IK::Assign; h.sig = tmp; h.e = sysFnNode(id, kids);
        prog.emit(h);
        Instr w; w.k = IK::Assign;
        lhs(sd, w);
        int sw = (int)sd.type->getBitWidth();
        w.e = resize(slice(ref(tmp), 63, 32), sw, false, sd.type->isSigned());
        prog.emit(w);
        return resize(slice(ref(tmp), 31, 0), 32, false, true);
    }
    // ---- Annex D: the interactive scope (D.11) and what lists it ----
    int scopeSig() {
        if (curScopeSig() < 0) curScopeSig() = freshStatic(32);
        return curScopeSig();
    }
    static const slang::ast::Scope* scopeOf(const slang::ast::Symbol* s) {
        using namespace slang::ast;
        if (!s) return nullptr;
        if (s->kind == SymbolKind::Instance) return &s->as<InstanceSymbol>().body;
        return s->isScope() ? &s->as<Scope>() : nullptr;
    }
    static std::string scopeKind(const slang::ast::Symbol& s) {
        using namespace slang::ast;
        switch (s.kind) {
            case SymbolKind::Instance: {
                auto& d = s.as<InstanceSymbol>().getDefinition();
                std::string k = d.definitionKind == DefinitionKind::Interface ? "interface"
                              : d.definitionKind == DefinitionKind::Program   ? "program" : "module";
                return k + " " + s.getHierarchicalPath() + " (" + std::string(d.name) + ")";
            }
            case SymbolKind::Subroutine:
                return std::string(s.as<SubroutineSymbol>().subroutineKind == SubroutineKind::Task ? "task " : "function ") +
                       s.getHierarchicalPath();
            case SymbolKind::StatementBlock: return "named block " + s.getHierarchicalPath();
            case SymbolKind::GenerateBlock: return "generate block " + s.getHierarchicalPath();
            default: return "";
        }
    }
    static void listScopes(const slang::ast::Scope& sc, bool deep, std::string& out) {
        using namespace slang::ast;
        for (auto& m : sc.members()) {
            if (m.kind == SymbolKind::GenerateBlockArray) {
                for (auto* b : m.as<GenerateBlockArraySymbol>().entries)
                    if (!b->isUninstantiated) { out += scopeKind(*b) + "\n"; if (deep) listScopes(*b, deep, out); }
                continue;
            }
            if (m.kind == SymbolKind::GenerateBlock && m.as<GenerateBlockSymbol>().isUninstantiated) continue;
            if (m.name.empty() && m.kind != SymbolKind::Instance) continue;
            std::string k = scopeKind(m);
            if (k.empty()) continue;
            out += k + "\n";
            if (deep)
                if (const Scope* in = scopeOf(&m)) listScopes(*in, deep, out);
        }
    }
    // $showvars of one variable or net: its name, type and value; a net adds its strength.
    void showVar(const slang::ast::Symbol& v) {
        using namespace slang::ast;
        auto it = sigmap.find(&v);
        if (it == sigmap.end() || !v.isValue()) return;
        const Type& t = v.as<ValueSymbol>().getType();
        if (!t.isIntegral() && !t.isFloating()) return;
        Instr d; d.k = IK::Display;
        bool net = v.kind == SymbolKind::Net;
        std::string ty = net ? std::string(v.as<NetSymbol>().netType.name) + (t.getBitWidth() > 1 ? " " + t.toString().substr(t.toString().find('[')) : "")
                             : t.toString();
        d.fmt = escPct(v.getHierarchicalPath() + " (" + ty + ") = ") + (t.isFloating() ? "%g" : "%b") + (net ? " %v" : "");
        auto r = ref(it->second, t.isSigned());
        if (t.isFloating()) r = node(Op::AsReal, {r});
        d.args.push_back(r);
        if (net) d.args.push_back(ref(it->second));
        prog.emit(d);
    }
    void showVars(const slang::ast::Symbol* s) {
        if (const slang::ast::Scope* sc = scopeOf(s))
            for (auto& m : sc->members())
                if (m.kind == slang::ast::SymbolKind::Variable || m.kind == slang::ast::SymbolKind::Net) showVar(m);
    }
    void emitText(const std::string& text) {
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            Instr d; d.k = IK::Display; d.fmt = escPct(text.substr(p, e - p));
            prog.emit(d);
            p = e + 1;
        }
    }
    static std::string sourceOf(const slang::ast::Symbol& s0) {
        using namespace slang::ast;
        const Symbol* s = &s0;
        const slang::syntax::SyntaxNode* syn = s->kind == SymbolKind::Instance
            ? s->as<InstanceSymbol>().getDefinition().getSyntax() : s->getSyntax();
        if (!syn || !srcMgr()) return "";
        auto r = syn->sourceRange();
        auto a = srcMgr()->getFullyOriginalLoc(r.start()), b = srcMgr()->getFullyOriginalLoc(r.end());
        if (a.buffer() != b.buffer()) return "";
        std::string_view t = srcMgr()->getSourceText(a.buffer());
        if (b.offset() > t.size() || a.offset() > b.offset()) return "";
        return std::string(t.substr(a.offset(), b.offset() - a.offset()));
    }
    // Run `body` for the interactive scope: one branch per $scope target.
    template <class F> void forCurScope(F body) {
        auto& tg = scopeTargets();
        if (tg.empty()) { body(scopeDefault()); return; }
        int cs = scopeSig();
        std::vector<size_t> ends;
        for (size_t k = 0; k < tg.size(); k++) {
            Instr j; j.k = IK::JmpIfNot;
            j.e = bin(Op::Eq, ref(cs), konst(make(k + 1, 0, 32, 0)));
            size_t jf = prog.emit(j);
            body(tg[k]);
            Instr x; x.k = IK::Jmp;
            ends.push_back(prog.emit(x));
            prog.code[jf].target = prog.here();
        }
        body(scopeDefault());
        for (size_t e : ends) prog.code[e].target = prog.here();
    }
    void lowerScopeTask(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        auto args = call.arguments();
        auto symArg = [&](size_t k) -> const Symbol* {
            const Expression& x = *args[k];
            if (x.kind == ExpressionKind::ArbitrarySymbol) return x.as<ArbitrarySymbolExpression>().symbol;
            return x.getSymbolReference();
        };
        if (n == "$scope") {   // D.11: set the interactive scope
            const Symbol* t = symArg(0);
            auto& tg = scopeTargets();
            auto it = std::find(tg.begin(), tg.end(), t);
            if (it == tg.end()) unsupported("$scope target");
            Instr i; i.k = IK::Assign; i.sig = scopeSig();
            i.e = konst(make((uint64_t)(it - tg.begin()) + 1, 0, 32, 0));
            prog.emit(i);
            return;
        }
        if (n == "$showvars") {   // D.13
            if (args.empty()) { forCurScope([&](const Symbol* s) { showVars(s); }); return; }
            for (auto* a : args)
                if (const Symbol* v = a->getSymbolReference()) showVar(*v);
            return;
        }
        if (n == "$list") {   // D.6: the source of the scope
            if (!args.empty()) { emitText(sourceOf(*symArg(0))); return; }
            forCurScope([&](const Symbol* s) { emitText(sourceOf(*s)); });
            return;
        }
        // $showscopes [(n)] (D.12): the scopes in the interactive scope; n != 0, every level below it.
        E deep = args.empty() ? nullptr : expr(*args[0]);
        forCurScope([&](const Symbol* s) {
            const Scope* sc = scopeOf(s);
            if (!sc) return;
            std::string flat, all;
            listScopes(*sc, false, flat);
            if (!deep) { emitText(flat); return; }
            listScopes(*sc, true, all);
            Instr j; j.k = IK::JmpIfNot; j.e = deep;
            size_t jf = prog.emit(j);
            emitText(all);
            Instr x; x.k = IK::Jmp;
            size_t je = prog.emit(x);
            prog.code[jf].target = prog.here();
            emitText(flat);
            prog.code[je].target = prog.here();
        });
    }
    // $countdrivers (D.2): bit 0 the result; the counts go to the output
    // arguments (forced, 0/1/x drivers, 0, 1, x).
    E countDrivers(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        auto args = call.arguments();
        Instr t; lhs(peel(*args[0]), t);
        if (t.arr || t.dyn >= 0) unsupported("$countdrivers of this net");
        E bit = t.bidx ? t.bidx : konst(make(t.hi >= 0 ? (uint64_t)t.lo : 0, 0, 32, 0));
        E c = sysFnNode(SF_ANNEXD, {konst(make((uint64_t)t.sig, 0, 32, 0)), bit}, AD_COUNT);
        if (args.size() == 1) return c;
        if (!stmtDepth) unsupported("$countdrivers with output arguments outside a procedural statement");
        int r = fresh(1);
        Instr a; a.k = IK::Assign; a.sig = r; a.e = c;
        prog.emit(a);
        for (size_t k = 1; k < args.size(); k++) {
            const Expression& dst = outArg(*args[k]);
            if (dst.kind == ExpressionKind::EmptyArgument) continue;
            Instr w; w.k = IK::Assign;
            lhs(dst, w);
            w.e = resize(sysFnNode(SF_ANNEXD, {konst(make(k - 1, 0, 32, 0))}, AD_COUNT_GET), (int)dst.type->getBitWidth(), false, false);
            prog.emit(w);
        }
        return ref(r);
    }
    E annexD(const slang::ast::CallExpression& call, std::string_view n) {
        static const std::pair<const char*, int> ops[] = {
            {"$log", AD_LOG}, {"$nolog", AD_NOLOG}, {"$key", AD_KEY}, {"$nokey", AD_NOKEY}, {"$input", AD_INPUT},
            {"$reset", AD_RESET}, {"$reset_count", AD_RESET_COUNT}, {"$reset_value", AD_RESET_VALUE},
            {"$save", AD_SAVE}, {"$incsave", AD_INCSAVE}, {"$restart", AD_RESTART}};
        auto args = call.arguments();
        for (auto& [nm, op] : ops) {
            if (n != nm) continue;
            annexDUsed() = true;
            std::vector<E> k;
            for (auto* a : args) {
                if (a->kind == slang::ast::ExpressionKind::EmptyArgument) break;
                k.push_back(a->type->isString() || a->kind == slang::ast::ExpressionKind::StringLiteral ? strOf(*a) : expr(*a));
            }
            return sysFnNode(SF_ANNEXD, std::move(k), op);
        }
        if (n == "$countdrivers") { annexDUsed() = true; return countDrivers(call); }
        if (n == "$getpattern" && args.size() == 1) return expr(*args[0]);   // D.3: the memory word itself
        if (n == "$scale" && args.size() == 1 && args[0]->kind == slang::ast::ExpressionKind::ArbitrarySymbol) {   // D.10
            // One time unit of the named module in units of this one, rounded.
            const auto* sym = args[0]->as<slang::ast::ArbitrarySymbolExpression>().symbol.get();
            int u = tsUnit;
            if (sym->kind == slang::ast::SymbolKind::Instance)
                if (auto ts = sym->as<slang::ast::InstanceSymbol>().body.getTimeScale()) u = tsExp(ts->base);
            double f = std::pow(10.0, u - tsUnit);
            Val v = make((uint64_t)(int64_t)std::llround(f), 0, 32, 0, true);
            return konst(v);
        }
        return nullptr;
    }
    // The coverage access functions (20.13, 40.3.2). The hierarchy argument
    // becomes a list of instance paths below the top ("" is the top; "*" is
    // $root, every top), separated by '|', or "\x01" when it names nothing:
    // an instance as a hierarchical reference, or a module definition name as
    // a string, which stands for every instance of that module.
    E coverageApi(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        auto args = call.arguments();
        int op = n == "$coverage_control" ? 0 : n == "$coverage_get_max" ? 1 : n == "$coverage_get" ? 2
               : n == "$coverage_save" ? 3 : n == "$coverage_merge" ? 4 : -1;
        if (op < 0) unsupported(("system function " + std::string(n)).c_str());
        size_t nInts = op == 0 ? 3 : op <= 2 ? 2 : 1;
        if (args.size() != nInts + 1) unsupported(("the arguments of " + std::string(n)).c_str());
        std::vector<E> kids;
        for (size_t k = 0; k < nInts; k++) kids.push_back(expr(*args[k]));
        const Expression& last = *args[nInts];
        if (op >= 3) { kids.push_back(strOf(last)); return sysFnNode(SF_COVAPI, std::move(kids), op); }
        auto rel = [](const Symbol& s) {
            std::string p = s.getHierarchicalPath();
            size_t d = p.find('.');
            return d == std::string::npos ? std::string() : p.substr(d + 1);
        };
        std::string targets;
        const Symbol* sym = last.kind == ExpressionKind::ArbitrarySymbol ? last.as<ArbitrarySymbolExpression>().symbol.get() : nullptr;
        if (sym && sym->kind == SymbolKind::Root) targets = "*";
        else if (sym && sym->kind == SymbolKind::Instance) targets = rel(*sym);
        else if (last.type->isString() || last.kind == ExpressionKind::StringLiteral || peel(last).kind == ExpressionKind::StringLiteral) {
            std::string def;
            if (peel(last).kind == ExpressionKind::StringLiteral) def = std::string(peel(last).as<StringLiteral>().getValue());
            else if (const auto* cv = last.getConstant()) def = cv->isString() ? cv->str() : cv->convertToStr().str();
            else unsupported(("a module name that is not a constant in " + std::string(n)).c_str());
            const auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine);
            if (!sci || !sci->scope) unsupported("$coverage_* outside a scope");
            const RootSymbol& root = sci->scope->getCompilation().getRoot();
            bool any = false;
            std::function<void(const Scope&)> walk = [&](const Scope& sc) {
                for (const Symbol& m : sc.members()) {
                    if (m.kind == SymbolKind::Instance) {
                        const auto& in = m.as<InstanceSymbol>();
                        if (in.getDefinition().name == def) { targets += (any ? "|" : "") + rel(in); any = true; }
                        walk(in.body);
                    } else if (m.kind == SymbolKind::InstanceArray) walk(m.as<InstanceArraySymbol>());
                    else if (m.kind == SymbolKind::GenerateBlock && !m.as<GenerateBlockSymbol>().isUninstantiated) walk(m.as<GenerateBlockSymbol>());
                    else if (m.kind == SymbolKind::GenerateBlockArray) walk(m.as<GenerateBlockArraySymbol>());
                }
            };
            for (const InstanceSymbol* top : root.topInstances) {
                if (top->getDefinition().name == def) { targets += (any ? "|" : "") + std::string(); any = true; }
                walk(top->body);
            }
            if (!any) targets = "\x01";
        } else unsupported(("the hierarchy argument of " + std::string(n)).c_str());
        kids.push_back(konst(mkstr(targets)));
        return sysFnNode(SF_COVAPI, std::move(kids), op);
    }
    E sysFn(const slang::ast::CallExpression& call, std::string_view n) {
        if (E r = annexD(call, n)) return r;   // Annex D optional tasks and functions
        if (n == "$get_coverage") return sysFnNode(SF_CG, {}, CG_GLOBAL);   // 19.9
        if (n.starts_with("$coverage_")) return coverageApi(call, n);   // 20.13
        if (E r = sampledFn(call, n)) return r;   // $past, $rose, ... (16.9.3)
        if (E r = enumMethod(call)) return r;
        auto args = call.arguments();
        auto a = [&](size_t i) { return expr(*args[i]); };
        if (n == "$shortrealtobits" && args.size() == 1) return sysFnNode(SF_SR2BITS, {realOf(*args[0])});
        if (n == "$clog2" && args.size() == 1) return sysFnNode(SF_CLOG2, {a(0)});
        if (n == "$countones" && args.size() == 1) return sysFnNode(SF_COUNTBITS, {a(0)}, 2);
        if (n == "$countbits" && args.size() >= 2) return sysFnNode(SF_COUNTBITS, {a(0)}, countCtl(args.subspan(1)));
        if ((n == "$onehot" || n == "$onehot0") && args.size() == 1) {
            E c = sysFnNode(SF_COUNTBITS, {a(0)}, 2);
            return bin(n == "$onehot" ? Op::Eq : Op::Le, c, konst(make(1, 0, 32, 0, true)));
        }
        if (n == "$isunknown" && args.size() == 1)
            return bin(Op::Ne, sysFnNode(SF_COUNTBITS, {a(0)}, 12), konst(make(0, 0, 32, 0, true)));
        if (n == "$random") {
            if (args.empty()) return sysFnNode(SF_RANDOM, {});
            return seededCall(SF_RANDOM_SEED, *args[0], {});
        }
        if (n == "$dist_uniform" && args.size() == 3)
            return seededCall(SF_DIST_UNIFORM, *args[0], {resize(a(1), 32, args[1]->type->isSigned(), true),
                                                          resize(a(2), 32, args[2]->type->isSigned(), true)});
        {   // $dist_normal and the rest of 20.14 / Annex N
            static const std::pair<const char*, int> dists[] = {
                {"$dist_normal", SF_DIST_NORMAL}, {"$dist_exponential", SF_DIST_EXPONENTIAL},
                {"$dist_poisson", SF_DIST_POISSON}, {"$dist_chi_square", SF_DIST_CHI_SQUARE},
                {"$dist_t", SF_DIST_T}, {"$dist_erlang", SF_DIST_ERLANG}};
            for (auto& [dn, id] : dists)
                if (n == dn && !args.empty()) {
                    std::vector<E> more;
                    for (size_t i = 1; i < args.size(); i++) more.push_back(resize(a(i), 32, args[i]->type->isSigned(), true));
                    return seededCall(id, *args[0], more);
                }
        }
        if (n == "$system") return args.empty() ? sysFnNode(SF_SYSTEM, {}) : sysFnNode(SF_SYSTEM, {strOf(*args[0])});
        if (n.starts_with("$q_")) return queueCall(call, n);   // 20.15
        if (n == "$urandom") return args.empty() ? sysFnNode(SF_URANDOM, {}) : sysFnNode(SF_URANDOM_SEED, {a(0)});
        if (n == "$urandom_range" && !args.empty()) {
            std::vector<E> k{a(0)};
            if (args.size() > 1) k.push_back(a(1));
            return sysFnNode(SF_URANDOM_RANGE, k);
        }
        if (n == "$fopen" && !args.empty() && args.size() <= 2) {
            std::vector<E> k{strOf(*args[0])};
            if (args.size() == 2) k.push_back(strOf(*args[1]));
            return sysFnNode(SF_FOPEN, k, args.size() == 2 ? 1 : 0);
        }
        if (n == "$fclose" && args.size() == 1) return sysFnNode(SF_FCLOSE, {a(0)});
        if (n == "$fflush") return args.empty() ? sysFnNode(SF_FFLUSH, {}) : sysFnNode(SF_FFLUSH, {a(0)});
        if (n == "$fgetc" && args.size() == 1) return sysFnNode(SF_FGETC, {a(0)});
        if (n == "$ungetc" && args.size() == 2) return sysFnNode(SF_UNGETC, {a(0), a(1)});
        if (n == "$feof" && args.size() == 1) return sysFnNode(SF_FEOF, {a(0)});
        if (n == "$ftell" && args.size() == 1) return sysFnNode(SF_FTELL, {a(0)});
        if (n == "$fseek" && args.size() == 3) return sysFnNode(SF_FSEEK, {a(0), a(1), a(2)});
        if (n == "$rewind" && args.size() == 1) return sysFnNode(SF_FSEEK, {a(0)});
        if (n == "$fgets") return fgetsCall(call);
        if (n == "$fread") return freadCall(call);
        if (n == "$fscanf") return scanCall(call, true);
        if (n == "$sscanf") return scanCall(call, false);
        if (n == "$fdisplay" || n == "$fdisplayb" || n == "$fdisplayo" || n == "$fdisplayh" ||
            n == "$fwrite" || n == "$fwriteb" || n == "$fwriteo" || n == "$fwriteh")
            return fileWriteCall(call, n);
        if (n == "$cast") return castCall(call);
        if (n == "$timeformat") {
            std::vector<E> k;
            for (size_t i = 0; i < args.size() && i < 4; i++) k.push_back(i == 2 ? strOf(*args[i]) : expr(*args[i]));
            return sysFnNode(SF_TIMEFORMAT, k);
        }
        if (n == "$test$plusargs" && args.size() == 1) return sysFnNode(SF_PLUS_TEST, {strOf(*args[0])});
        if (n == "$value$plusargs" && args.size() == 2) {
            // "N=%d": the prefix is the text before the %, the letter says how
            // to read the rest. The variable is written only on a match.
            const auto& f = peel(*args[0]);
            if (f.kind != slang::ast::ExpressionKind::StringLiteral) unsupported("$value$plusargs format that is not a literal");
            std::string fs(f.as<slang::ast::StringLiteral>().getValue());
            size_t pc = fs.find('%');
            if (pc == std::string::npos) unsupported("$value$plusargs format without a specifier");
            size_t q = pc + 1;
            while (q < fs.size() && isdigit((unsigned char)fs[q])) q++;
            if (q >= fs.size()) unsupported("$value$plusargs format");
            char c = (char)tolower((unsigned char)fs[q]);
            if (std::string_view("dhxobsfeg").find(c) == std::string_view::npos) unsupported("$value$plusargs format letter");
            if (!stmtDepth) unsupported("$value$plusargs outside a procedural statement");
            E pre = konst(mkstr(fs.substr(0, pc)));
            int found = fresh(32);
            Instr t; t.k = IK::Assign; t.sig = found; t.e = sysFnNode(SF_PLUS_TEST, {pre});
            prog.emit(t);
            Instr j; j.k = IK::JmpIfNot; j.e = ref(found);
            size_t jf = prog.emit(j);
            const slang::ast::Expression& dst = outArg(*args[1]);
            Instr w; w.k = IK::Assign;
            lhs(dst, w);
            E v = readAs(sysFnNode(SF_PLUS_VALUE, {pre}, c), c, *dst.type, "$value$plusargs");
            w.e = v;
            prog.emit(w);
            prog.code[jf].target = prog.here();
            return ref(found, true);
        }
        return nullptr;
    }
    // %p (21.2.1.6): the format text and arguments that print value v of
    // type t as an assignment pattern. Integral values print in decimal (as
    // unformatted), enums by name, strings in quotes, packed structs and
    // arrays as '{...}. A fixed-size array is expanded here, element by
    // element; a dynamic container is printed at run time by %Q.
    static std::string escPct(const std::string& s) {
        std::string r;
        for (char c : s) { r += c; if (c == '%') r += '%'; }
        return r;
    }
    void pfmtSingular(const slang::ast::Type& t0, E v, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        if (t.isString()) { fmt += "\"%s\""; out.push_back(v); return; }
        if (t.isFloating()) { fmt += "%R"; out.push_back(v); return; }
        if (t.kind == SymbolKind::EnumType) {
            // The name of a valid value, else the value as its base type.
            auto fb = node(Op::SFormat, {v});
            fb->fmt = "%0d";
            E name = fb;
            std::vector<const EnumValueSymbol*> vals;
            for (const auto& ev : t.as<EnumType>().values()) vals.push_back(&ev);
            for (size_t k = vals.size(); k-- > 0;) {
                const auto& cv = vals[k]->getValue();
                if (!cv.isInteger()) continue;
                E eq = bin(Op::CaseEq, v, svLit(cv.integer(), t.isSigned(), {}));
                name = mux(eq, konst(mkstr(std::string(vals[k]->name))), name);
            }
            fmt += "%s";
            out.push_back(name);
            return;
        }
        if (isTaggedUnion(t)) { pfmtTagged(t, v, fmt, out); return; }
        if (t.kind == SymbolKind::PackedStructType || t.kind == SymbolKind::PackedUnionType) {
            fmt += "'{";
            bool first = true;
            for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
                if (!first) fmt += ", ";
                first = false;
                fmt += escPct(std::string(f.name)) + ":";
                int w = (int)f.getType().getBitWidth();
                E part = slice(v, (int)f.bitOffset + w - 1, (int)f.bitOffset);
                if (f.getType().isSigned()) part = resize(part, w, false, true);
                pfmtSingular(f.getType(), part, fmt, out);
                if (t.kind == SymbolKind::PackedUnionType) break;   // only the first member
            }
            fmt += "}";
            return;
        }
        if (t.isIntegral()) { fmt += "%0d"; out.push_back(v); return; }
        if (isHandleType(t) || t.isEvent() || t.isVirtualInterface()) {   // a handle: null, or @ and its object id
            auto at = node(Op::SFormat, {v});
            at->fmt = "@%0d";
            fmt += "%s";
            out.push_back(mux(bin(Op::Eq, v, konst(make(0, 0, 64))), konst(mkstr("null")), at));
            return;
        }
        unsupported("%p of this type");
    }
    // A tagged union prints tag:value of the member its tag names (21.2.1.6),
    // as '{M:v}; a void member prints as '{M}.
    void pfmtTagged(const slang::ast::Type& t, E v, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        bool up = t.isUnpackedUnion();
        int w = up ? uWidth(t) : (int)t.getBitWidth();
        int tb = tagBitsOf(t);
        E r = konst(mkstr("'{?}"));
        std::vector<const FieldSymbol*> fs;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) fs.push_back(&f);
        for (size_t k = fs.size(); k-- > 0;) {
            const FieldSymbol& f = *fs[k];
            const Type& mt = f.getType();
            std::string sub = "'{" + escPct(std::string(f.name));
            std::vector<E> subArgs;
            if (!mt.isVoid()) {
                int fw = up ? uWidth(mt) : (int)mt.getBitWidth();
                sub += ":";
                E part = slice(v, fw - 1, 0);
                if (up) pfmtBits(mt, part, sub, subArgs);
                else pfmtSingular(mt, mt.isSigned() ? resize(part, fw, false, true) : part, sub, subArgs);
            }
            sub += "}";
            auto sf = node(Op::SFormat, std::move(subArgs));
            sf->fmt = sub;
            E cond = tb > 0 ? bin(Op::CaseEq, slice(v, w - 1, w - tb), konst(mk((uint64_t)k, tb))) : konst(mk(1, 1));
            r = mux(cond, sf, r);
        }
        fmt += "%s";
        out.push_back(r);
    }
    // An unpacked struct prints with member names; a union, its first member (21.2.1.6).
    void pfmtStruct(const slang::ast::Type& t0, E v, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        if (isTaggedUnion(t)) { pfmtTagged(t, v, fmt, out); return; }
        fmt += "'{";
        bool first = true;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
            if (!first) fmt += ", ";
            first = false;
            fmt += escPct(std::string(f.name)) + ":";
            auto [off, w] = uField(t, f);
            E part = slice(v, off + w - 1, off);
            pfmtBits(f.getType(), part, fmt, out);
            if (t.isUnpackedUnion()) break;
        }
        fmt += "}";
    }
    // %p of container h whose innermost elements have type ec: %Q with d, s
    // or R; other elements (enums, structs) %QF with a temporary that holds
    // each element in turn and the format that prints it.
    void pfmtCont(const slang::ast::Type& ec, E h, std::string& fmt, std::vector<E>& out) {
        fmt += "%Q";
        if (ec.isString()) { fmt += 's'; out.push_back(h); return; }
        if (ec.isFloating()) { fmt += 'R'; out.push_back(h); return; }
        if (ec.isIntegral() && ec.kind != slang::ast::SymbolKind::EnumType && !ec.isStruct()) { fmt += 'd'; out.push_back(h); return; }
        int w = bitsOf(ec);
        if (w <= 0) unsupported("%p of a container of this element type");
        int tmp = fresh(w);
        std::string sub;
        std::vector<E> subArgs;
        if (ec.isUnpackedStruct() || ec.isUnpackedUnion()) pfmtStruct(ec, ref(tmp), sub, subArgs);
        else if (ec.getCanonicalType().kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType) pfmtBits(ec, ref(tmp), sub, subArgs);
        else pfmtSingular(ec, ref(tmp, ec.isSigned()), sub, subArgs);
        auto sf = node(Op::SFormat, std::move(subArgs));
        sf->fmt = sub;
        fmt += 'F';
        out.push_back(h);
        out.push_back(ref(tmp));
        out.push_back(sf);
    }
    // A value held as the bits of an unpacked struct member (uWidth).
    void pfmtBits(const slang::ast::Type& t0, E part, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        const Type& ft = t0.getCanonicalType();
        int w = uWidth(ft);
        if (ft.kind == SymbolKind::FixedSizeUnpackedArrayType) {   // '{e0, e1, ...}, the left element first
            const auto& a = ft.as<FixedSizeUnpackedArrayType>();
            int ew = uWidth(a.elementType), n = (int)a.range.width();
            fmt += "'{";
            for (int k = 0; k < n; k++) {
                if (k) fmt += ", ";
                int hi = w - 1 - k * ew;
                pfmtBits(a.elementType, slice(part, hi, hi - ew + 1), fmt, out);
            }
            fmt += "}";
        }
        else if (ft.isUnpackedStruct() || ft.isUnpackedUnion()) pfmtStruct(ft, part, fmt, out);
        else if (ft.isString()) pfmtSingular(ft, un(Op::AsStr, part), fmt, out);
        else if (ft.isFloating()) pfmtSingular(ft, un(Op::AsReal, part), fmt, out);
        else pfmtSingular(ft, ft.isSigned() ? resize(part, w, false, true) : part, fmt, out);
    }
    void pfmt(const slang::ast::Expression& a, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        const Expression& x = peel(a);
        if (int d = dynValue(x); d >= 0) {
            const Type* et = x.type->getCanonicalType().getArrayElementType();
            while (isDynType(*et)) et = et->getCanonicalType().getArrayElementType();   // a container of containers: the innermost elements
            pfmtCont(et->getCanonicalType(), ref(d), fmt, out);
            return;
        }
        if (auto ca = x.type->isUnpackedArray() ? contArr(x) : nullptr) {   // int f[2][$]: each element container by %Q
            const Type* et = x.type;
            for (size_t k = 0; k < ca->dims.size(); k++) et = et->getCanonicalType().getArrayElementType();
            std::vector<int64_t> idx(ca->dims.size());
            std::function<void(size_t)> walk = [&](size_t dim) {
                auto [l, r] = ca->dims[dim];
                int64_t n = (l > r ? l - r : r - l) + 1;
                fmt += "'{";
                for (int64_t k = 0; k < n; k++) {
                    if (k) fmt += ", ";
                    idx[dim] = l <= r ? l + k : l - k;
                    if (dim + 1 < ca->dims.size()) { walk(dim + 1); continue; }
                    std::vector<E> ks;
                    for (int64_t i : idx) ks.push_back(konst(make((uint64_t)i, 0, 32, 0, true)));
                    auto addr = node(Op::ArrAddr, std::move(ks));
                    addr->arr = ca;
                    const Type* it = et;
                    while (isDynType(*it)) it = it->getCanonicalType().getArrayElementType();
                    pfmtCont(it->getCanonicalType(), ref(bindCont(std::move(addr), *ca->dynElem)), fmt, out);
                }
                fmt += "}";
            };
            walk(0);
            return;
        }
        if (x.type->isUnpackedArray()) {
            const Symbol* sym = (x.kind == ExpressionKind::NamedValue || x.kind == ExpressionKind::HierarchicalValue)
                                    ? &x.as<ValueExpressionBase>().symbol : nullptr;
            auto ap = sym ? arrFind(sym) : nullptr;
            if (!ap) {   // h.a and other array expressions: their elements, flattened (7.4)
                std::vector<E> el;
                arrayElems(*x.type, x, el);
                std::vector<int64_t> dims;
                const Type* et = x.type;
                while (et->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType) {
                    dims.push_back((int64_t)et->getCanonicalType().as<FixedSizeUnpackedArrayType>().range.width());
                    et = et->getCanonicalType().getArrayElementType();
                }
                size_t flat = 0;
                std::function<void(size_t)> walk = [&](size_t dim) {
                    fmt += "'{";
                    for (int64_t k = 0; k < dims[dim]; k++) {
                        if (k) fmt += ", ";
                        if (dim + 1 < dims.size()) walk(dim + 1);
                        else if (flat < el.size()) {
                            E v = el[flat++];
                            if (v->op == Op::Ref || v->op == Op::ArrRead) {   // an element's raw bits
                                if (et->isFloating()) v = un(Op::AsReal, v);
                                else if (et->isString()) v = un(Op::AsStr, v);
                            }
                            pfmtSingular(*et, v, fmt, out);
                        }
                    }
                    fmt += "}";
                };
                walk(0);
                return;
            }
            const ArrShape& A = *ap;
            const Type* et = x.type;
            for (size_t k = 0; k < A.dims.size(); k++) et = et->getCanonicalType().getArrayElementType();
            int flat = 0;
            std::function<void(size_t)> walk = [&](size_t dim) {
                auto [l, r] = A.dims[dim];
                int64_t n = (l > r ? l - r : r - l) + 1;
                fmt += "'{";
                for (int64_t k = 0; k < n; k++) {
                    if (k) fmt += ", ";
                    if (dim + 1 < A.dims.size()) walk(dim + 1);
                    else if (et->isUnpackedStruct() || et->isUnpackedUnion()) pfmtStruct(*et, ref(A.base + flat++), fmt, out);
                    else if (et->isString()) pfmtSingular(*et, un(Op::AsStr, ref(A.base + flat++)), fmt, out);
                    else if (et->isFloating()) pfmtSingular(*et, un(Op::AsReal, ref(A.base + flat++)), fmt, out);
                    else pfmtSingular(*et, ref(A.base + flat++, A.sgn), fmt, out);
                }
                fmt += "}";
            };
            walk(0);
            return;
        }
        if (x.type->isUnpackedStruct() || x.type->isUnpackedUnion()) { pfmtStruct(*x.type, expr(x), fmt, out); return; }
        pfmtSingular(*x.type, expr(x), fmt, out);
    }
    template <typename Args>
    void formatInto(const Args& argv, char dflt, std::string& fmt, std::vector<E>& out) {
        using namespace slang::ast;
        // The specifiers still waiting for an argument: where each one sits
        // in fmt, its length, and its letter. A %p is replaced by its
        // expansion once its argument is known.
        struct Spec { size_t pos, len; char c; };
        std::deque<Spec> pending;
        auto addSpecs = [&](const std::string& f, size_t at) {
            for (size_t p = 0; p < f.size(); p++) {
                if (f[p] != '%') continue;
                if (p + 1 < f.size() && f[p + 1] == '%') { p++; continue; }
                size_t q = p + 1;
                while (q < f.size() && (isdigit((unsigned char)f[q]) || f[q] == '.' || f[q] == '-')) q++;
                if (q < f.size() && f[q] != 'm' && f[q] != 'M')
                    pending.push_back({at + p, q - p + 1, (char)tolower((unsigned char)f[q])});
            }
        };
        for (const Expression* a : argv) {
            if (a->kind == ExpressionKind::EmptyArgument) { fmt += ' '; continue; }
            const Expression& pe = peel(*a);
            if (pe.kind == ExpressionKind::StringLiteral && pending.empty()) {
                std::string f = substM(std::string(pe.as<StringLiteral>().getValue()), hierName, libName);
                size_t at = fmt.size();
                fmt += f;
                addSpecs(f, at);
                continue;
            }
            if (!pending.empty()) {
                Spec sp = pending.front();
                pending.pop_front();
                if (sp.c == 't') {   // %t (20.4.3): the value is in this module's units
                    const Expression& x = peel(*a);
                    E v = expr(*a);
                    if (x.type->isFloating()) v = bin(Op::FMul, realOf(*a), konst(mkreal((double)ticksPerUnit())));
                    else if (ticksPerUnit() != 1) v = bin(Op::Mul, resize(v, 64, x.type->isSigned(), false), konst(mk(ticksPerUnit(), 64)));
                    out.push_back(v);
                    continue;
                }
                if (sp.c == 's' && pe.kind == ExpressionKind::StringLiteral) {   // a literal of any length (5.9)
                    out.push_back(strOf(*a));
                    continue;
                }
                if (sp.c == 'p') {
                    std::string x;
                    pfmt(*a, x, out);
                    fmt.replace(sp.pos, sp.len, x);
                    for (auto& o : pending) o.pos += x.size() - sp.len;
                    continue;
                }
            } else { fmt += '%'; fmt += a->type->isFloating() ? 'R' : a->type->isString() ? 's' : dflt; }
            out.push_back(expr(*a));
        }
    }

    // $fatal / $error / $warning / $info (20.10): a header line with the
    // severity, the file and line of the call, its scope, and the time, then
    // the user's message (formatted like $display) if there is one. $fatal
    // then ends the run like $finish, and msim exits with status 1.
    void severity(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        auto args = call.arguments();
        if (n == "$fatal" && !args.empty()) {
            const Expression& a0 = peel(*args[0]);
            if (a0.kind != ExpressionKind::StringLiteral) args = args.subspan(1);   // finish_number
        }
        std::string file = "?";
        size_t line = 0;
        if (const auto* sm = srcMgr()) {
            auto loc = sm->getFullyOriginalLoc(call.sourceRange.start());
            file = std::string(sm->getFileName(loc));
            line = sm->getLineNumber(loc);
        }
        const char* sev = n == "$fatal" ? "Fatal" : n == "$error" ? "Error" : n == "$warning" ? "Warning" : "Info";
        Instr i; i.k = IK::Display;
        hierName = scopeName(call); libName = libCell(call);
        std::string escFile;   // a '%' in a file name prints as itself
        for (char c : file) { escFile += c; if (c == '%') escFile += '%'; }
        i.fmt = substM(std::string(sev) + ": \"" + escFile + "\", " + std::to_string(line) + ": %m: at time %0t", hierName, libName);
        E t = node(Op::Time);
        t->k = mk(1, 64);   // %t takes kernel ticks
        i.args.push_back(t);
        if (!args.empty()) {
            i.fmt += "\n";
            formatInto(args, 'd', i.fmt, i.args);
        }
        prog.emit(i);
        if (n == "$fatal") {
            Instr f; f.k = IK::Finish; f.hi = 1;   // exit status 1
            prog.emit(f);
        }
    }

    // $display / $strobe / $monitor use the kernel format (a string of %b / %d /
    // literal text, arguments evaluated when the line is printed). $finish and
    // $stop stop the sim. Anything else is still ignored.
    // ---------- constrained randomization (18) ----------
    std::unordered_map<const slang::ast::Symbol*, Val> constVals;     // foreach indices of a constraint, unrolled
    std::unordered_map<const slang::ast::Symbol*, int> symOverride;   // with {...}: the caller's properties, copied
    const slang::ast::Expression* to2State = nullptr;   // the conversion being lowered without its x/z clearing
    // with {...}: an element of the caller's array or container, by an index
    // the caller knows (addrs[i]), read into a signal before the call.
    std::unordered_map<const slang::ast::Expression*, int> exprOverride;
    using BeforeList = std::vector<std::pair<std::vector<E>, std::vector<E>>>;
    std::unordered_map<const slang::ast::Symbol*, E> exprBind;
    // a.sum(), a.sum() with (expr) ... of a fixed-size array (7.12.3): the
    // elements folded, at the result's width.
    E fixedReduce(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        const Expression& arr = *call.arguments()[0];
        std::vector<E> el;
        arrayElems(*arr.type, arr, el);
        auto [iter, iv] = std::get<CallExpression::SystemCallInfo>(call.subroutine).getIteratorInfo();
        const auto& et = arr.type->getArrayElementType()->getCanonicalType();
        int w = (int)call.type->getBitWidth();
        bool sg = call.type->isSigned();
        needWidth(w);
        Op op = n == "sum" ? Op::Add : n == "product" ? Op::Mul : n == "and" ? Op::And : n == "or" ? Op::Or : Op::Xor;
        E acc;
        for (E x : el) {
            E t;
            if (iter) {
                exprBind[iv] = x;
                t = expr(*iter, ExprCtx{w, sg, sg, true});
                exprBind.erase(iv);
            } else t = resize(x, w, et.isSigned(), sg);
            acc = acc ? bin(op, acc, t) : t;
        }
        if (!acc) return konst(make(n == "product" ? 1 : n == "and" ? ~0ULL : 0, 0, w, 0, sg));
        return acc;
    }
    // The values of x: one, or every element of a fixed-size array.
    void randElems(const slang::ast::Expression& x, std::vector<E>& out) {
        if (x.type->isUnpackedArray()) {
            if (isDynType(*x.type)) unsupported("a dynamic array or queue in solve-before");
            arrayElems(*x.type, x, out);
        } else out.push_back(expr(x));
    }
    // The constraints of c (18.5), each guarded by `guard` (implication and
    // if-else bodies), as items.
    int consDepth = 0;   // > 0 while lowering constraints
    std::vector<std::shared_ptr<Prog>> consFns;   // function calls lowered in the current constraint (18.5.12)
    std::vector<std::pair<int, E>> consRefs;      // m[i] containers named in the current constraint
    void constraintItems(const slang::ast::Constraint& c, E guard, std::vector<RandItem>& items, BeforeList& before) {
        NoCondCov off(condCovOff);
        consDepth++;
        struct Out { int& d; ~Out() { d--; } } out{consDepth};
        size_t f0 = consFns.size(), i0 = items.size(), r0 = consRefs.size();
        constraintItems1(c, guard, items, before);
        // Containers named here go with every item this constraint made.
        if (consRefs.size() > r0) {
            for (size_t k = i0; k < items.size(); k++)
                items[k].refs.insert(items[k].refs.end(), consRefs.begin() + (long)r0, consRefs.end());
            consRefs.resize(r0);
        }
        // Calls lowered here (in a guard or an expression) go with every item
        // this constraint made, after those of its parts.
        if (consFns.size() > f0) {
            std::function<void(RandItem&)> add = [&](RandItem& it) {
                it.fns.insert(it.fns.end(), consFns.begin() + (long)f0, consFns.end());
                for (auto& b : it.body) add(b);
            };
            for (size_t k = i0; k < items.size(); k++) add(items[k]);
            consFns.resize(f0);
        }
    }
    void constraintItems1(const slang::ast::Constraint& c, E guard, std::vector<RandItem>& items, BeforeList& before) {
        using namespace slang::ast;
        size_t code = prog.code.size();
        auto andG = [&](E p) { return guard ? bin(Op::LogAnd, guard, p) : p; };
        switch (c.kind) {
            case ConstraintKind::List:
                for (const Constraint* x : c.as<ConstraintList>().list) constraintItems(*x, guard, items, before);
                break;
            case ConstraintKind::Expression: {
                const auto& ec = c.as<ExpressionConstraint>();
                RandItem it;
                it.soft = ec.isSoft;
                const Expression& x = ec.expr;
                if (x.kind == ExpressionKind::Dist) {   // 18.5.3
                    const auto& d = x.as<DistExpression>();
                    it.isDist = true;
                    if (auto* dw = d.defaultWeight()) it.distDefault = dw->expr ? expr(*dw->expr) : konst(mk(1, 32));
                    it.e = expr(d.left());
                    it.guard = guard;
                    for (const auto& di : d.items()) {
                        RandDist rd;
                        if (di.value.kind == ExpressionKind::ValueRange) {
                            const auto& vr = di.value.as<ValueRangeExpression>();
                            if (vr.rangeKind != ValueRangeKind::Simple) unsupported("dist range with a tolerance");
                            rd.lo = expr(vr.left());
                            rd.hi = expr(vr.right());
                        } else rd.lo = expr(di.value);
                        rd.weight = di.weight && di.weight->expr ? expr(*di.weight->expr) : konst(mk(1, 32));
                        rd.perRange = di.weight && di.weight->kind == DistExpression::DistWeight::PerRange;
                        it.dist.push_back(std::move(rd));
                    }
                } else {
                    E e = expr(x);
                    it.e = guard ? bin(Op::LogOr, un(Op::LogNot, guard), e) : e;
                }
                items.push_back(std::move(it));
                break;
            }
            case ConstraintKind::Implication: {   // 18.5.6
                const auto& ic = c.as<ImplicationConstraint>();
                constraintItems(ic.body, andG(expr(ic.predicate)), items, before);
                break;
            }
            case ConstraintKind::Conditional: {   // 18.5.7
                const auto& cc = c.as<ConditionalConstraint>();
                E p = expr(cc.predicate);
                constraintItems(cc.ifBody, andG(p), items, before);
                if (cc.elseBody) constraintItems(*cc.elseBody, andG(un(Op::LogNot, p)), items, before);
                break;
            }
            case ConstraintKind::Uniqueness: {   // 18.5.5
                std::vector<E> el;
                std::vector<int> conts;
                for (const Expression* x : c.as<UniquenessConstraint>().items) {
                    if (isDynType(*x->type)) {   // unique {q}: q's elements, known at solve time
                        if (dynDepth(*x->type) > 1) unsupported("a container of containers in unique");
                        int d = dynSig(*x);
                        if (d < 0) unsupported("this container in unique");
                        conts.push_back(d);
                    } else randElems(*x, el);
                }
                if (!conts.empty()) {
                    RandItem it;
                    it.uniqConts = std::move(conts);
                    it.uniqElems = std::move(el);
                    it.guard = guard;
                    items.push_back(std::move(it));
                    break;
                }
                for (size_t a = 0; a < el.size(); a++)
                    for (size_t b = a + 1; b < el.size(); b++) {
                        RandItem it;
                        E ne = bin(Op::Ne, el[a], el[b]);
                        it.e = guard ? bin(Op::LogOr, un(Op::LogNot, guard), ne) : ne;
                        items.push_back(std::move(it));
                    }
                break;
            }
            case ConstraintKind::SolveBefore: {   // 18.5.9
                const auto& sb = c.as<SolveBeforeConstraint>();
                std::vector<E> a, b;
                for (const Expression* x : sb.solve) randElems(*x, a);
                for (const Expression* x : sb.after) randElems(*x, b);
                before.push_back({std::move(a), std::move(b)});
                break;
            }
            case ConstraintKind::Foreach: {   // 18.5.8.1: unrolled over a fixed-size array
                const auto& fc = c.as<ForeachConstraint>();
                if (fc.loopDims.size() == 1 && !fc.loopDims[0].range) {   // over a dynamic array or queue: expanded at solve time
                    int d = dynSig(fc.arrayRef);
                    if (d < 0) unsupported("foreach constraint over this array");
                    RandItem loop;
                    loop.loopCont = d;
                    loop.loopVar = fresh(32);
                    const Symbol* lv = fc.loopDims[0].loopVar;
                    if (lv) symOverride[lv] = loop.loopVar;
                    constraintItems(fc.body, guard, loop.body, before);
                    if (lv) symOverride.erase(lv);
                    items.push_back(std::move(loop));
                    break;
                }
                size_t nd = fc.loopDims.size(), nf = 0;
                while (nf < nd && fc.loopDims[nf].range) nf++;
                if (nf < nd) {   // foreach (q[i, j]) over containers of containers, maybe inside a fixed-size array
                    for (size_t k = nf; k < nd; k++)
                        if (fc.loopDims[k].range) unsupported("foreach constraint over a fixed-size array inside a container");
                    // One loop per container level; the inner container is
                    // the element the outer loop variable names, at solve time.
                    std::function<void(size_t, int, std::vector<RandItem>&)> nest = [&](size_t k, int cont, std::vector<RandItem>& out) {
                        if (k == nd) { constraintItems(fc.body, guard, out, before); return; }
                        RandItem loop;
                        loop.loopCont = cont;
                        loop.loopVar = fresh(32);
                        const Symbol* lv = fc.loopDims[k].loopVar;
                        if (lv) symOverride[lv] = loop.loopVar;
                        if (k + 1 < nd) {
                            if (!dynShape(cont).elem) unsupported("foreach constraint: more dimensions than the container has");
                            size_t r0 = consRefs.size();
                            auto n = node(Op::DynElemRef, {ref(loop.loopVar, true)});
                            n->sig = cont;
                            int inner = elemRef(cont, n);
                            loop.refs.insert(loop.refs.end(), consRefs.begin() + (long)r0, consRefs.end());
                            consRefs.resize(r0);
                            nest(k + 1, inner, loop.body);
                        } else constraintItems(fc.body, guard, loop.body, before);
                        if (lv) symOverride.erase(lv);
                        out.push_back(std::move(loop));
                    };
                    if (nf == 0) {
                        int d = dynSig(fc.arrayRef);
                        if (d < 0) unsupported("foreach constraint over this array");
                        nest(0, d, items);
                        break;
                    }
                    auto fa = contArr(fc.arrayRef);   // int f[2][$]: each container of the fixed-size part
                    if (!fa || fa->dims.size() != nf) unsupported("foreach constraint over this array");
                    std::vector<int32_t> idx(nf);
                    std::function<void(size_t)> fix = [&](size_t k) {
                        if (k == nf) {
                            std::vector<E> ks;
                            for (int32_t v : idx) { Val cv = mk((uint64_t)(int64_t)v, 32); cv.sgn = true; ks.push_back(konst(cv)); }
                            auto n = node(Op::ArrAddr, std::move(ks));
                            n->arr = fa;
                            size_t r0 = consRefs.size();
                            int c = bindCont(std::move(n), *fa->dynElem);
                            std::vector<std::pair<int, E>> refs(consRefs.begin() + (long)r0, consRefs.end());
                            consRefs.resize(r0);
                            size_t i0 = items.size();
                            nest(nf, c, items);
                            for (size_t q = i0; q < items.size(); q++) items[q].refs.insert(items[q].refs.end(), refs.begin(), refs.end());
                            return;
                        }
                        const slang::ConstantRange& r = *fc.loopDims[k].range;
                        const Symbol* lv = fc.loopDims[k].loopVar;
                        int step = r.left <= r.right ? 1 : -1;
                        for (int32_t v = r.left;; v += step) {
                            idx[k] = v;
                            if (lv) { Val cv = mk((uint64_t)(int64_t)v, 32); cv.sgn = true; constVals[lv] = cv; }
                            fix(k + 1);
                            if (v == r.right) break;
                        }
                        if (lv) constVals.erase(lv);
                    };
                    fix(0);
                    break;
                }
                std::vector<std::pair<const Symbol*, slang::ConstantRange>> dims;
                for (const auto& d : fc.loopDims) {
                    if (d.loopVar) dims.push_back({d.loopVar, *d.range});
                }
                std::function<void(size_t)> loop = [&](size_t k) {
                    if (k == dims.size()) { constraintItems(fc.body, guard, items, before); return; }
                    const slang::ConstantRange& r = dims[k].second;
                    int step = r.left <= r.right ? 1 : -1;
                    for (int32_t v = r.left;; v += step) {
                        Val cv = mk((uint64_t)(int64_t)v, 32);
                        cv.sgn = true;
                        constVals[dims[k].first] = cv;
                        loop(k + 1);
                        if (v == r.right) break;
                    }
                    constVals.erase(dims[k].first);
                };
                loop(0);
                break;
            }
            case ConstraintKind::DisableSoft: {   // 18.5.13.2
                RandItem it;
                it.disableSoft = true;
                it.e = expr(c.as<DisableSoftConstraint>().target);
                items.push_back(std::move(it));
                break;
            }
            default: unsupported("constraint of this kind");
        }
        if (prog.code.size() != code) unsupported("a function call in a constraint");
    }
    // A constraint block, lowered once in the context of its class (18.5).
    std::shared_ptr<RandBlock> randBlock(const slang::ast::ConstraintBlockSymbol& cb) {
        auto b = std::make_shared<RandBlock>();
        b->name = std::string(cb.name);
        if (auto it = consModeMap().find(&cb); it != consModeMap().end()) b->mode = it->second;
        else if (cb.flags.has(slang::ast::ConstraintBlockFlags::Static)) { b->mode = staticModeSig(cb); b->staticMode = true; }
        constraintItems(cb.getConstraints(), nullptr, b->items, b->before);
        return b;
    }
    // A static constraint block's constraint_mode: one signal for every object (18.5.10).
    int staticModeSig(const slang::ast::ConstraintBlockSymbol& cb) {
        auto& m = staticConsMode();
        if (auto it = m.find(&cb); it != m.end()) return it->second;
        if (!mkSig) unsupported("static constraint block");
        int s = mkSig(std::string(cb.getParentScope()->asSymbol().name) + "::" + std::string(cb.name) + "$mode", 1, false);
        m[&cb] = s;
        return s;
    }
    // obj.randomize(), randomize() in a method, std::randomize(a, b) (18.6, 18.7, 18.11, 18.12).
    E randomizeCall(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        auto args = call.arguments();
        auto rc = std::make_shared<RandCall>();
        Instr i;
        i.k = IK::Randomize;
        size_t first = 0;
        ClassInfo* wc = nullptr;
        bool isStd = !call.isSystemCall();
        if (!isStd && !args.empty() && args[0]->kind != ExpressionKind::Assignment && isHandleType(*args[0]->type) &&
            args[0]->type->isClass()) {
            wc = classInfo(*args[0]->type);
            i.e = exprH(*args[0]);
            first = 1;
        } else if (!isStd && curClass) {
            wc = curClass;
            i.e = node(Op::This);
        }
        const CallExpression::RandomizeCallInfo* info = nullptr;
        if (call.isSystemCall())
            info = std::get_if<CallExpression::RandomizeCallInfo>(&std::get<1>(call.subroutine).extraInfo);
        ClassInfo* savedCls = curClass;
        auto savedOv = symOverride;
        if (wc) {
            // The caller's own properties named in with {...} are copied
            // first: inside the call `this` is the object being randomized.
            if (info && info->inlineConstraints && savedCls) {
                auto inObjScope = [&](const Symbol* sym) {
                    const Scope* sc = sym->getParentScope();
                    for (ClassInfo* c = wc; c; c = c->base)
                        if (sc == static_cast<const Scope*>(c->ty)) return true;
                    return false;
                };
                // addrs[i] with addrs the caller's array or container and i
                // known to the caller (a local, a loop variable, a scalar
                // property): read the element now, into a signal.
                struct Sel : ASTVisitor<Sel, VisitFlags::Expressions> {
                    std::vector<const ElementSelectExpression*> sels;
                    void handle(const ElementSelectExpression& x) { sels.push_back(&x); visitDefault(x); }
                } sv;
                info->inlineConstraints->visit(sv);
                std::unordered_set<const Expression*> covered;   // the array names read this way
                for (const ElementSelectExpression* x : sv.sels) {
                    if (x->value().kind != ExpressionKind::NamedValue) continue;
                    const Symbol* arr = &x->value().as<NamedValueExpression>().symbol;
                    auto pm = propMap().find(arr);
                    if (pm == propMap().end() || pm->second.kind == 0 || inObjScope(arr)) continue;
                    const Type& et = x->type->getCanonicalType();
                    if (!et.isIntegral() || isHandleType(*x->type)) continue;
                    struct Refs : ASTVisitor<Refs, VisitFlags::Expressions> {
                        std::vector<const Symbol*> syms;
                        void handle(const NamedValueExpression& n) { syms.push_back(&n.symbol); }
                    } rv;
                    x->selector().visit(rv);
                    bool callerOnly = true;
                    for (const Symbol* r : rv.syms) {
                        if (inObjScope(r)) { callerOnly = false; break; }
                        // a constraint's own foreach iterator has no signal yet
                        if (r->kind == SymbolKind::Iterator && sigFind(r) < 0) { callerOnly = false; break; }
                        auto rp = propMap().find(r);
                        if (rp != propMap().end() && rp->second.kind != 0) { callerOnly = false; break; }
                    }
                    if (!callerOnly) continue;
                    int t = fresh(storeW(*x->type));
                    Instr a; a.k = IK::Assign; a.sig = t;
                    a.e = expr(*x);
                    prog.emit(a);
                    exprOverride[x] = t;
                    covered.insert(&x->value());
                }
                struct V : ASTVisitor<V, VisitFlags::Expressions> {
                    const std::unordered_set<const Expression*>* skip;
                    std::vector<const Symbol*> found;
                    void handle(const NamedValueExpression& n) { if (!skip->count(&n)) found.push_back(&n.symbol); }
                } v;
                v.skip = &covered;
                info->inlineConstraints->visit(v);
                for (const Symbol* sym : v.found) {
                    auto pm = propMap().find(sym);
                    if (pm == propMap().end() || symOverride.count(sym)) continue;
                    const Scope* sc = sym->getParentScope();
                    bool inObj = false;
                    for (ClassInfo* c = wc; c; c = c->base)
                        if (sc == static_cast<const Scope*>(c->ty)) inObj = true;
                    if (inObj) continue;
                    if (pm->second.kind != 0) unsupported("an array or container of the caller in with {...}");
                    int t = fresh(storeW(sym->as<ValueSymbol>().getType()));
                    Instr a; a.k = IK::Assign; a.sig = t;
                    a.e = ref(thisRel(pm->second.off));
                    prog.emit(a);
                    symOverride[sym] = t;
                }
            }
            curClass = wc;
        }
        for (size_t k = first; k < args.size(); k++) {
            const Expression& a = outArg(*args[k]);
            if (a.kind == ExpressionKind::NullLiteral) { rc->argsGiven = true; continue; }   // randomize(null): check only
            if (a.type->isUnpackedArray()) {   // a container or a fixed-size array (18.11): every element
                rc->argsGiven = true;
                const Type* et = a.type;
                std::vector<int> conts;
                std::vector<E> addrs;
                if (isDynType(*a.type)) {
                    int d = dynSig(a);
                    if (d < 0) unsupported("randomize() argument: this container");
                    conts.push_back(d);
                } else if (auto ca = contArr(a)) {   // int q[4][$]: each queue
                    while (!isDynType(*et)) et = et->getCanonicalType().getArrayElementType();
                    for (int k = 0; k < ca->count; k++) {
                        std::vector<E> ks;
                        int64_t rest = k;
                        std::vector<int64_t> idx(ca->dims.size());
                        for (size_t dd = ca->dims.size(); dd-- > 0;) {
                            auto [l, r] = ca->dims[dd];
                            int64_t n = (l > r ? l - r : r - l) + 1;
                            int64_t o = rest % n;
                            rest /= n;
                            idx[dd] = l <= r ? l + o : l - o;
                        }
                        for (int64_t i : idx) ks.push_back(konst(make((uint64_t)i, 0, 32, 0, true)));
                        auto n = node(Op::ArrAddr, std::move(ks));
                        n->arr = ca;
                        addrs.push_back(std::move(n));
                    }
                } else {   // int a[4]: its elements
                    std::vector<E> el;
                    arrayElems(*a.type, a, el);
                    for (E& r : el) {
                        if (r->op != Op::Ref) unsupported("randomize() argument: an array element that is not a variable");
                        rc->args.push_back(r);
                    }
                    continue;
                }
                int depth = 0;
                while (isDynType(*et)) { depth++; et = et->getCanonicalType().getArrayElementType(); }
                const Type& ec = et->getCanonicalType();
                if (!ec.isIntegral() || ec.isUnpackedStruct() || ec.getBitWidth() > 64) unsupported("randomize() argument: a container of this element type");
                for (int d : conts) rc->contArgs.push_back({d, (int)ec.getBitWidth(), ec.isSigned(), depth, nullptr});
                for (E& n : addrs) rc->contArgs.push_back({-1, (int)ec.getBitWidth(), ec.isSigned(), depth, std::move(n)});
                continue;
            }
            if (!a.type->isIntegral()) unsupported("randomize() argument of this type");
            E r = expr(a);
            if (r->op != Op::Ref) unsupported("randomize() argument that is not a variable");
            rc->args.push_back(r);
            rc->argsGiven = true;
        }
        if (info && info->inlineConstraints) constraintItems(*info->inlineConstraints, nullptr, rc->items, rc->before);
        curClass = savedCls;
        symOverride = std::move(savedOv);
        exprOverride.clear();
        i.sig = fresh(32);
        i.rc = rc;
        prog.emit(i);
        return ref(i.sig, true);
    }
    // x.rand_mode(v), obj.rand_mode(v), c.constraint_mode(v), and their
    // function forms that read the mode (18.8, 18.9).
    E randModeCall(const slang::ast::CallExpression& call, bool wantValue) {
        using namespace slang::ast;
        bool cons = call.getSubroutineName() == "constraint_mode";
        auto args = call.arguments();
        if (args.empty()) unsupported("rand_mode / constraint_mode without an object");
        const Expression& t = *args[0];
        E val = args.size() > 1 ? expr(*args[1]) : nullptr;
        auto staticOf = [&](const Symbol& m) -> int {
            if (!cons || m.kind != SymbolKind::ConstraintBlock || !m.as<ConstraintBlockSymbol>().flags.has(ConstraintBlockFlags::Static)) return -1;
            return staticModeSig(m.as<ConstraintBlockSymbol>());
        };
        auto modeOf = [&](const Symbol& m) -> int {
            if (cons) {
                auto it = consModeMap().find(&m);
                if (it == consModeMap().end()) unsupported("constraint_mode of this kind");
                return it->second;
            }
            auto it = randModeMap().find(&m);
            if (it == randModeMap().end()) unsupported("rand_mode of a variable that is not rand");
            return it->second;
        };
        int slot = -1;
        const Symbol* named = t.kind == ExpressionKind::MemberAccess ? &t.as<MemberAccessExpression>().member
                            : t.kind == ExpressionKind::NamedValue ? &t.as<NamedValueExpression>().symbol
                            : t.kind == ExpressionKind::ArbitrarySymbol ? t.as<ArbitrarySymbolExpression>().symbol.get() : nullptr;
        if (int ss = named ? staticOf(*named) : -1; ss >= 0) {
            if (!val) return resize(un(Op::LogNot, bin(Op::CaseEq, ref(ss), konst(mk(0, 1)))), 32, false, true);
            slot = ss;
        } else if (t.kind == ExpressionKind::MemberAccess && (t.as<MemberAccessExpression>().member.kind == SymbolKind::ClassProperty ||
                                                       t.as<MemberAccessExpression>().member.kind == SymbolKind::ConstraintBlock)) {
            const auto& m = t.as<MemberAccessExpression>();
            int off = modeOf(m.member);
            if (!val) { E r = ref(off); r->at = exprH(m.value()); return resize(r, 32, false, true); }
            slot = indSlot(exprH(m.value()), off);
        } else if ((t.kind == ExpressionKind::NamedValue && (randModeMap().count(&t.as<NamedValueExpression>().symbol) ||
                                                             consModeMap().count(&t.as<NamedValueExpression>().symbol)))) {
            int off = modeOf(t.as<NamedValueExpression>().symbol);
            if (!val) return resize(ref(thisRel(off)), 32, false, true);
            slot = thisRel(off);
        } else if (t.kind == ExpressionKind::ArbitrarySymbol && consModeMap().count(t.as<ArbitrarySymbolExpression>().symbol)) {
            int off = modeOf(*t.as<ArbitrarySymbolExpression>().symbol);
            if (!val) return resize(ref(thisRel(off)), 32, false, true);
            slot = thisRel(off);
        } else if (isHandleType(*t.type) && t.type->isClass()) {
            if (!val) unsupported("rand_mode() / constraint_mode() of a whole object as a value");
            Instr i; i.k = IK::RandMode; i.e = exprH(t); i.de = val; i.hi = cons;
            prog.emit(i);
            return konst(mk(0, 32));
        } else unsupported("rand_mode / constraint_mode of this kind");
        Instr i; i.k = IK::Assign; i.sig = slot;
        i.e = un(Op::RedOr, val);   // any nonzero value turns it on
        prog.emit(i);
        (void)wantValue;
        return konst(mk(0, 32));
    }
    // $cast(dst, src) of class handles (8.16): dst = src when src is null
    // or an object of dst's class (or one derived from it). As a task, a
    // failed cast is an error and dst is unchanged.
    E castClass(const slang::ast::CallExpression& call, bool wantValue) {
        using namespace slang::ast;
        const Expression& dst = outArg(*call.arguments()[0]);
        const Expression& src = *call.arguments()[1];
        if (!dst.type->isClass()) unsupported("$cast to a non-class handle");
        int h = fresh(64), ok = fresh(1);
        Instr a; a.k = IK::Assign; a.sig = h; a.e = exprH(src); prog.emit(a);
        auto t = node(Op::IsA, {ref(h)});
        t->hi = classInfo(*dst.type)->id;
        Instr b; b.k = IK::Assign; b.sig = ok; b.e = t; prog.emit(b);
        Instr j; j.k = IK::JmpIfNot; j.e = ref(ok);
        size_t jf = prog.emit(j);
        Instr w; w.k = IK::Assign; lhs(dst, w); w.e = ref(h); prog.emit(w);
        if (!wantValue) {
            size_t js = emitJmp();
            prog.code[jf].target = prog.here();
            Instr d; d.k = IK::Display; d.fmt = "Error: $cast failed: the object is not a " + dst.type->toString();
            prog.emit(d);
            prog.code[js].target = prog.here();
            return {};
        }
        prog.code[jf].target = prog.here();
        return ref(ok);
    }
    void sysCall(const slang::ast::CallExpression& call) {
        using namespace slang::ast;
        std::string_view n = call.getSubroutineName();
        if (auto vi = vpiSystfNames().find(std::string(n)); vi != vpiSystfNames().end()) {   // VPI (38.37)
            emitVpiCall(call, vi->second, false);
            return;
        }
        if (n == "randomize") { randomizeCall(call); return; }
        if (n == "rand_mode" || n == "constraint_mode") { randModeCall(call, false); return; }
        if (n == "$cast" && call.arguments().size() == 2 && isHandleType(*outArg(*call.arguments()[0]).type)) {
            castClass(call, voidCast);
            return;
        }
        if (n == "$finish" || n == "$stop") {
            Instr i; i.k = IK::Finish; prog.emit(i); return;
        }
        if (n == "$exit") { Instr i; i.k = IK::Exit; prog.emit(i); return; }
        if (n == "$fatal" || n == "$error" || n == "$warning" || n == "$info") { severity(call, n); return; }
        if (assertCtlCall(call, n)) return;   // $asserton, $assertcontrol, ... (20.11)
        if (!call.arguments().empty() && isFixedArr(*call.arguments()[0]) &&
            (n == "sort" || n == "rsort" || n == "reverse" || n == "shuffle")) {   // 7.12.2 on a fixed-size array
            const auto& x = peel(*call.arguments()[0]);
            auto a = x.kind == slang::ast::ExpressionKind::NamedValue
                         ? arrFind(&x.as<slang::ast::NamedValueExpression>().symbol) : nullptr;
            if (!a || a->dims.size() != 1 || a->dynElem) {
                // A struct member, a row of a bigger array, ...: sort a copy,
                // then write the whole array back in one go (7.12.2).
                const Type& at = x.type->getCanonicalType();
                const auto& ft = at.as<slang::ast::FixedSizeUnpackedArrayType>();
                const Type& et = ft.elementType.getCanonicalType();
                if (!et.isIntegral() || uWidth(at) <= 0) unsupported("ordering method of this fixed-size array");
                dynMethod(call, false);
                int q = lastFixedQueue, n = (int)ft.range.width(), ew = (int)et.getBitWidth();
                std::vector<E> parts;
                for (int k = 0; k < n; k++) {
                    auto g = node(Op::DynGet, {konst(make((uint64_t)k, 0, 32, 0, true))});
                    g->sig = q;
                    parts.push_back(resize(std::move(g), ew, et.isSigned(), et.isSigned()));
                }
                // The values are read from the scratch queue, not from the
                // array, so the writes can go one by one.
                if (auto sa = subArraySel(x)) {   // m[i].sort(): the elements of the row
                    auto idxs = subArrayIdx(*sa);
                    if (idxs.size() != parts.size()) unsupported("ordering method of this fixed-size array");
                    for (size_t k = 0; k < idxs.size(); k++) {
                        Instr i; i.k = IK::Assign;
                        i.sig = sa->a->base; i.arr = sa->a; i.e = parts[k];
                        i.aidx = std::move(idxs[k]);
                        prog.emit(i);
                    }
                    return;
                }
                if (classMember(x)) unsupported("ordering method of an array property");
                Instr i; i.k = IK::Assign;   // held packed (a struct member): all its bits at once
                i.e = parts.size() == 1 ? parts[0] : node(Op::Concat, std::move(parts));
                lhs(x, i);
                prog.emit(i);
                return;
            }
            dynMethod(call, false);   // on a scratch queue holding a copy; then the elements go back
            int q = lastFixedQueue;
            for (int k = 0; k < a->count; k++) {
                Instr w; w.k = IK::Assign; w.sig = a->base + k;
                auto g = node(Op::DynGet, {konst(make((uint64_t)k, 0, 32, 0, true))});
                g->sig = q;
                w.e = resize(std::move(g), a->elemW, a->sgn, a->sgn);
                prog.emit(w);
            }
            return;
        }
        if (!call.arguments().empty() && dynSig(*call.arguments()[0]) >= 0) {   // q.push_back(x), aa.delete(k) ...
            dynMethod(call, false);
            return;
        }
        if (isStrMethodCall(call)) {   // s.putc(i, c), s.itoa(v), ...: s gets the new string
            int id = strMethodId(n);
            if (id < S_PUTC) return;   // a value method used as a statement: no effect
            Instr i; i.k = IK::Assign;
            lhs(*call.arguments()[0], i);
            i.e = strMethod(call, id);
            prog.emit(i);
            return;
        }
        if (n == "$sformat" || n == "$swrite") {   // $sformat(s, fmt, ...) (21.3.3)
            auto args = call.arguments();
            if (args.empty()) unsupported("$sformat");
            const Expression* dst = args[0];
            if (dst->kind == ExpressionKind::Assignment) dst = &dst->as<AssignmentExpression>().left();
            Instr i; i.k = IK::Assign;
            lhs(*dst, i);
            auto f = node(Op::SFormat);
            hierName = scopeName(call); libName = libCell(call);
            formatInto(args.subspan(1), 'd', f->fmt, f->kids);
            i.e = dst->type->isString() ? f : E(node(Op::StrToInt, {f}));
            if (!dst->type->isString()) i.e->hi = (int)dst->type->getBitWidth();
            prog.emit(i);
            return;
        }
        if (n == "$readmemh" || n == "$readmemb" || n == "$writememh" || n == "$writememb") {   // 21.4, 21.5
            auto args = call.arguments();
            if (args.size() < 2) unsupported("$readmem arguments");
            const Expression& fn = peel(*args[0]);
            // The file name: a literal, a constant such as a string parameter,
            // or an expression evaluated when the task runs.
            std::optional<std::string> fname;
            if (fn.kind == ExpressionKind::StringLiteral) fname = std::string(fn.as<StringLiteral>().getValue());
            else if (const slang::ConstantValue* cv = fn.kind == ExpressionKind::NamedValue &&
                                                     fn.as<NamedValueExpression>().symbol.kind == SymbolKind::Parameter
                                                 ? &fn.as<NamedValueExpression>().symbol.as<ParameterSymbol>().getValue()
                                                 : args[0]->getConstant();
                     cv && !cv->bad()) {
                if (cv->isString()) fname = cv->str();
                else if (cv->isInteger()) {   // a string literal held in an integral parameter
                    auto sv = cv->integer();
                    std::string t;
                    for (int b = (int)sv.getBitWidth() - 8; b >= 0; b -= 8) {
                        char ch = (char)sv.slice(b + 7, b).as<uint8_t>().value_or(0);
                        if (ch) t += ch;
                    }
                    fname = t;
                }
            }
            // slang binds the memory as an output argument: an assignment to it.
            const Expression* mp = &peel(*args[1]);
            if (mp->kind == ExpressionKind::Assignment) mp = &peel(mp->as<AssignmentExpression>().left());
            // A partly indexed memory (mem[1], mem[1][2:5]) names the lesser
            // dimensioned array left over (21.4): the indexes go in aidx, and
            // a slice of the last dimension given adds its left and right
            // bounds there too (pdown set).
            std::vector<E> sub;
            bool slice = false;
            if (mp->kind == ExpressionKind::RangeSelect) {
                auto& rs = mp->as<RangeSelectExpression>();
                E l = expr(rs.left()), r = expr(rs.right());
                if (rs.getSelectionKind() != RangeSelectionKind::Simple) {
                    // [b +: w] covers b .. b+w-1 in the dimension's direction; [b -: w] b-w+1 .. b
                    auto& vt = rs.value().type->getCanonicalType();
                    bool asc = vt.kind == SymbolKind::FixedSizeUnpackedArrayType &&
                               vt.as<FixedSizeUnpackedArrayType>().range.left <
                                   vt.as<FixedSizeUnpackedArrayType>().range.right;
                    auto w = rs.right().getConstant();
                    if (!w || !w->isInteger()) unsupported("$readmem target slice width");
                    int64_t wn = w->integer().as<int64_t>().value_or(1);
                    E b = resize(l, 64, rs.left().type->isSigned(), true);
                    E d = konst(make((uint64_t)(wn - 1), 0, 64, 0, true));
                    E lo = rs.getSelectionKind() == RangeSelectionKind::IndexedUp ? b : bin(Op::Sub, b, d);
                    E hi = bin(Op::Add, lo, d);
                    l = asc ? lo : hi; r = asc ? hi : lo;
                }
                sub.push_back(l); sub.push_back(r);
                slice = true;
                mp = &peel(rs.value());
            }
            std::vector<E> idx;
            while (mp->kind == ExpressionKind::ElementSelect) {
                auto& es = mp->as<ElementSelectExpression>();
                if (!es.value().type->getCanonicalType().isUnpackedArray()) break;
                idx.insert(idx.begin(), expr(es.selector()));
                mp = &peel(es.value());
            }
            const Expression& mem = *mp;
            const Symbol* msym = nullptr;
            if (mem.kind == ExpressionKind::NamedValue) msym = &mem.as<NamedValueExpression>().symbol;
            else if (mem.kind == ExpressionKind::ArbitrarySymbol) msym = mem.as<ArbitrarySymbolExpression>().symbol;
            if (!msym) unsupported("$readmem target that is not a memory");
            auto ap = arrFind(msym);
            if (!ap) unsupported("$readmem target that is not an unpacked array");
            if (ap->dynElem) unsupported("$readmem into an array of containers");
            if (idx.size() >= ap->dims.size()) unsupported("$readmem target that is a single word");
            Instr i; i.k = n[1] == 'r' ? IK::ReadMem : IK::WriteMem;
            i.aidx = idx;
            for (auto& x : sub) i.aidx.push_back(x);
            i.pdown = slice;
            if (fname) i.fmt = *fname;
            else i.e = strOf(*args[0]);
            i.arr = ap;
            i.hex = n.ends_with('h');
            for (size_t a = 2; a < args.size() && a < 4; a++) i.args.push_back(expr(*args[a]));
            prog.emit(i);
            return;
        }
        if (n == "$sreadmemh" || n == "$sreadmemb") {   // D.14: $readmem with the data in strings
            auto args = call.arguments();
            const Expression* mp = &peel(*args[0]);
            if (mp->kind == ExpressionKind::Assignment) mp = &peel(mp->as<AssignmentExpression>().left());
            const Symbol* msym = ValueExpressionBase::isKind(mp->kind) ? &mp->as<ValueExpressionBase>().symbol : nullptr;
            auto ap = msym ? arrFind(msym) : nullptr;
            if (!ap || ap->dynElem) unsupported("$sreadmem target that is not a whole fixed-size memory");
            Instr i; i.k = IK::ReadMem; i.pw = 2;
            i.fmt = std::string(n) + " string";
            i.arr = ap;
            i.hex = n.ends_with('h');
            for (size_t a = 1; a < args.size(); a++) i.args.push_back(a < 3 ? expr(*args[a]) : strOf(*args[a]));
            prog.emit(i);
            return;
        }
        if (n == "$scope" || n == "$showvars" || n == "$showscopes" || n == "$list") {   // D.6, D.11-D.13
            lowerScopeTask(call, n);
            return;
        }
        if (n == "$sdf_annotate") {   // 32.9
            // With every argument known now, the annotation is made before
            // time 0, so the timing checks start with the annotated limits
            // and LABELs reach the specparams; otherwise when the call runs.
            auto args = call.arguments();
            std::string fields[7];
            bool known[7];
            sdfArgs(call, fields, known);
            std::vector<E> kids;
            bool all = true;
            for (size_t k = 0; k < 7; k++) {
                if (known[k]) { kids.push_back(konst(mkstr(fields[k]))); continue; }
                all = false;
                kids.push_back(strOf(*args[k]));
            }
            if (all) {
                SdfRequest r{fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], whereStr()};
                sdfRequests().push_back(r);
                return;
            }
            sdfAtRunTime() = true;
            Instr i; i.k = IK::Assign; i.sig = fresh(32); i.e = sysFnNode(SF_SDF, std::move(kids));
            prog.emit(i);
            return;
        }
        if (n == "$printtimescale") {   // 20.4.2
            auto expStr = [](int e) {
                static const char* u[] = {"s", "ms", "us", "ns", "ps", "fs"};
                int m = 1;
                while (((e % 3) + 3) % 3) { e--; m *= 10; }
                int k = -e / 3;
                return std::to_string(m) + (k >= 0 && k <= 5 ? u[k] : "?");
            };
            auto args = call.arguments();
            std::string name = scopeName(call);
            int u = tsUnit, pr = tsPrec;
            if (!args.empty()) {
                const Expression& x = peel(*args[0]);
                const Symbol* sym = x.kind == ExpressionKind::ArbitrarySymbol ? x.as<ArbitrarySymbolExpression>().symbol.get() : nullptr;
                if (sym && sym->kind == SymbolKind::Root) { name = "$root"; u = pr = simPrecExp(); }
                else if (sym && sym->kind == SymbolKind::Instance) {
                    name = sym->getHierarchicalPath();
                    auto ts = sym->as<InstanceSymbol>().body.getTimeScale();
                    if (ts) { u = tsExp(ts->base); pr = tsExp(ts->precision); }
                } else unsupported("$printtimescale of this argument");
            }
            Instr i; i.k = IK::Display;
            i.fmt = escPct("Time scale of (" + name + ") is " + expStr(u) + " / " + expStr(pr));
            prog.emit(i);
            return;
        }
        if (plaCall(call, n)) return;   // 20.16
        if (n == "$stacktrace") {   // 20.17: the scope of the call (there is no standard format)
            Instr i; i.k = IK::Display;
            i.fmt = escPct("$stacktrace: " + scopeName(call));
            prog.emit(i);
            return;
        }
        asTask = !voidCast;
        E rt = sysFn(call, n);
        asTask = false;
        if (E r = rt) {   // a system function called as a task: its effect only
            Instr i; i.k = IK::Assign; i.sig = fresh(64); i.e = resize(r, 64, false, false);
            prog.emit(i);
            return;
        }
        // $display / $write and their b / o / h variants (21.2.1). The suffix
        // only changes the default format of an argument with no specifier.
        IK k;
        bool nonl = false;
        char dflt = 'd';
        std::string_view base = n;
        if (base.size() > 1 && (base.ends_with('b') || base.ends_with('o') || base.ends_with('h')) &&
            (base.starts_with("$display") || base.starts_with("$write") || base.starts_with("$strobe") ||
             base.starts_with("$monitor") || base.starts_with("$fstrobe") || base.starts_with("$fmonitor")) &&
            base != "$monitoron") {
            dflt = base.back();
            base.remove_suffix(1);
        }
        if (base == "$display") k = IK::Display;
        else if (base == "$write") { k = IK::Display; nonl = true; }
        else if (base == "$strobe") k = IK::Strobe;
        else if (base == "$monitor") k = IK::Monitor;
        else if (base == "$fstrobe" || base == "$fmonitor") k = base == "$fstrobe" ? IK::Strobe : IK::Monitor;
        else {
            // Waveform tasks only change what msim -vcd writes, so skipping
            // them changes no result. Any other task would be a silent wrong
            // answer if it were skipped, so it is an error.
            if (n == "$dumpfile" || n == "$dumpvars" || n == "$dumpon" || n == "$dumpoff" || n == "$dumpall" ||
                n == "$dumpflush" || n == "$dumplimit") {
                lowerDump(call, n);
                return;
            }
            if (n == "$dumpports" || n == "$dumpportsoff" || n == "$dumpportson" || n == "$dumpportsall" ||
                n == "$dumpportsflush" || n == "$dumpportslimit") {
                lowerDumpPorts(call, n);
                return;
            }
            static const std::set<std::string_view> harmless = {
                "$set_coverage_db_name", "$load_coverage_db", "$fsdbDumpfile", "$fsdbDumpvars", "$fsdbDumpon", "$fsdbDumpoff",
                "$vcdpluson", "$vcdplusoff", "$vcdplusfile", "$shm_open", "$shm_probe", "$recordvars"};
            if (!harmless.count(n)) unsupported(("system task " + std::string(n)).c_str());
            std::cerr << "lower: ignoring system call " << n << "\n";
            return;
        }
        Instr i; i.k = k; i.nonl = nonl;
        hierName = scopeName(call); libName = libCell(call);
        auto fargs = call.arguments();
        if (base[1] == 'f') {   // $fstrobe / $fmonitor (21.3.2): the first argument is the descriptor
            if (fargs.empty()) unsupported("$fstrobe / $fmonitor without a descriptor");
            i.fd = expr(*fargs[0]);
            fargs = fargs.subspan(1);
        }
        formatInto(fargs, dflt, i.fmt, i.args);
        prog.emit(i);
    }

    // VCD tasks (21.7.1). $dumpvars scope and variable arguments become
    // signal-name prefixes: msim names a signal by its path below the top
    // module ("u1.x"), so top.u1 is "u1." and the top itself is "".
    void lowerDump(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        Instr i;
        i.k = IK::Dump;
        auto args = call.arguments();
        if (n == "$dumpfile") {
            i.dcmd = DC_FILE;
            if (!args.empty()) i.args.push_back(strOf(*args[0]));
        } else if (n == "$dumpvars") {
            i.dcmd = DC_VARS;
            if (!args.empty()) i.args.push_back(expr(*args[0]));
            for (size_t a = 1; a < args.size(); a++) {
                const Expression* x = args[a];
                const Symbol* sym = nullptr;
                if (x->kind == ExpressionKind::ArbitrarySymbol) sym = x->as<ArbitrarySymbolExpression>().symbol.get();
                else if (x->kind == ExpressionKind::NamedValue || x->kind == ExpressionKind::HierarchicalValue)
                    sym = &x->as<ValueExpressionBase>().symbol;
                if (!sym) unsupported("$dumpvars argument");
                std::string path = sym->getHierarchicalPath();
                size_t d = path.find('.');
                std::string rel = path.find("::") != std::string::npos ? path   // a package variable: "pkg::x"
                                : d == std::string::npos ? std::string() : path.substr(d + 1);
                bool var = sym->kind != SymbolKind::Instance;
                if (!var && !rel.empty()) rel.push_back('.');
                i.dsel.push_back({rel, var});
            }
        } else {
            i.dcmd = n == "$dumpon" ? DC_ON : n == "$dumpoff" ? DC_OFF : n == "$dumpall" ? DC_ALL
                   : n == "$dumpflush" ? DC_FLUSH : DC_LIMIT;
            if (i.dcmd == DC_LIMIT && !args.empty()) i.args.push_back(expr(*args[0]));
        }
        prog.emit(i);
    }

    // Extended VCD tasks (21.7.3). $dumpports names module instances; each
    // of their ports becomes one dsel entry "dir \t scope \t port \t msb \t
    // lsb \t signal" (the signal named as msim names it, below the top).
    void lowerDumpPorts(const slang::ast::CallExpression& call, std::string_view n) {
        using namespace slang::ast;
        Instr i;
        i.k = IK::Dump;
        auto args = call.arguments();
        auto isEmpty = [](const Expression& x) { return x.kind == ExpressionKind::EmptyArgument; };
        if (n != "$dumpports") {
            i.dcmd = n == "$dumpportsoff" ? DC_POFF : n == "$dumpportson" ? DC_PON : n == "$dumpportsall" ? DC_PALL
                   : n == "$dumpportsflush" ? DC_PFLUSH : DC_PLIMIT;
            size_t a = 0;
            if (i.dcmd == DC_PLIMIT) {
                if (args.empty() || isEmpty(*args[0])) unsupported("$dumpportslimit without a size");
                i.args.push_back(expr(*args[a++]));
            }
            if (a < args.size() && !isEmpty(*args[a])) i.args.push_back(strOf(*args[a]));
            prog.emit(i);
            return;
        }
        i.dcmd = DC_PORTS;
        std::vector<const InstanceBodySymbol*> bodies;
        const Expression* file = nullptr;
        for (size_t a = 0; a < args.size(); a++) {
            const Expression* x = args[a];
            if (isEmpty(*x)) continue;
            const Symbol* sym = nullptr;
            if (x->kind == ExpressionKind::ArbitrarySymbol) sym = x->as<ArbitrarySymbolExpression>().symbol.get();
            if (sym && sym->kind == SymbolKind::Instance) bodies.push_back(&sym->as<InstanceSymbol>().body);
            else if (a + 1 == args.size()) file = x;
            else unsupported("$dumpports argument that is not a module instance");
        }
        if (bodies.empty()) {
            // No scope: the module the call is in.
            const auto* sci = std::get_if<CallExpression::SystemCallInfo>(&call.subroutine);
            const Scope* sc = sci ? sci->scope.get() : nullptr;
            const InstanceBodySymbol* body = sc ? sc->getContainingInstance() : nullptr;
            if (!body) unsupported("$dumpports outside a module");
            bodies.push_back(body);
        }
        if (file) i.args.push_back(strOf(*file));
        for (const InstanceBodySymbol* body : bodies) {
            std::string scopePath = body->parentInstance ? body->parentInstance->getHierarchicalPath() : std::string(body->name);
            for (const Symbol* ps : body->getPortList()) {
                if (ps->kind != SymbolKind::Port) continue;   // interface ports have no value
                const auto& port = ps->as<PortSymbol>();
                if (!port.internalSymbol) continue;
                const Type& t = port.getType().getCanonicalType();
                if (!t.isIntegral()) continue;
                std::string path = port.internalSymbol->getHierarchicalPath();
                size_t d = path.find('.');
                std::string rel = d == std::string::npos ? path : path.substr(d + 1);
                if (auto it = sigmap.find(port.internalSymbol); it != sigmap.end()) rel = "#" + std::to_string(it->second);
                int w = (int)t.getBitWidth();
                int msb = w - 1, lsb = 0;
                if (t.hasFixedRange()) { auto r = t.getFixedRange(); msb = r.left; lsb = r.right; }
                char dir = port.direction == ArgumentDirection::In ? 'i' : port.direction == ArgumentDirection::Out ? 'o' : 'b';
                std::string enc(1, dir);
                enc += '\t' + scopePath + '\t' + std::string(port.name) + '\t' + std::to_string(msb) + '\t' + std::to_string(lsb) + '\t' + rel;
                i.dsel.push_back({enc, w > 1 || t.hasFixedRange() && !t.isScalar()});
            }
        }
        prog.emit(i);
    }

    // {<<s{...}} (11.4.14.2): the stream cut into s-bit slices from the
    // right, in reverse order, so the rightmost slice comes first and the
    // leftmost one, which may be shorter, comes last.
    static E reverseSlices(E v, int w, int s) {
        if (s <= 0 || s >= w) return v;
        std::vector<E> parts;
        for (int lo = 0; lo < w; lo += s) parts.push_back(slice(v, std::min(lo + s, w) - 1, lo));
        return concat(std::move(parts));
    }
    // The inverse, for unpacking (11.4.14.3): the first s bits of the stream
    // are the rightmost slice of the targets.
    static E unreverseSlices(E v, int w, int s) {
        if (s <= 0 || s >= w) return v;
        std::vector<E> parts;   // slices of the targets, rightmost first
        int hi = w - 1;
        for (int lo = 0; lo < w; lo += s) {
            int len = std::min(lo + s, w) - lo;
            parts.push_back(slice(v, hi, hi - len + 1));
            hi -= len;
        }
        std::reverse(parts.begin(), parts.end());
        return concat(std::move(parts));
    }
    // An unpacked struct whose members are all integral (or fixed-size
    // arrays or structs of such): its value bits are its stream (11.4.14.1),
    // members in order, the first on top.
    static bool streamStruct(const slang::ast::Type& t0) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        if (t.isIntegral()) return true;
        if (t.kind == SymbolKind::FixedSizeUnpackedArrayType) return streamStruct(t.as<FixedSizeUnpackedArrayType>().elementType);
        if (!t.isUnpackedStruct()) return false;
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>())
            if (!streamStruct(f.getType())) return false;
        return uWidth(t) > 0;
    }
    // The bits of a streaming concatenation of integral operands, and fixed
    // unpacked arrays of them (elements left to right).
    E streamBits(const slang::ast::StreamingConcatenationExpression& sc, int& w) {
        using namespace slang::ast;
        std::vector<E> parts;
        w = 0;
        for (auto& st : sc.streams()) {
            const Expression& op = peel(st.withExpr ? *st.withExpr : *st.operand);   // a with [...] (11.4.14.4): the selection
            int pw = 0;
            if (op.kind == ExpressionKind::Streaming) {
                parts.push_back(streamBits(op.as<StreamingConcatenationExpression>(), pw));
            } else if (op.type->isIntegral()) {
                pw = (int)op.type->getBitWidth();
                parts.push_back(expr(op));
            } else if (isFixedArr(op) && op.kind == ExpressionKind::NamedValue) {
                auto it = arrMap().find(&op.as<NamedValueExpression>().symbol);
                if (it == arrMap().end() || it->second->dims.size() != 1 ||
                    !op.type->getArrayElementType()->isIntegral())
                    unsupported("this array in a streaming concatenation");
                int ew = (int)op.type->getArrayElementType()->getBitWidth();
                auto r = op.type->getCanonicalType().as<FixedSizeUnpackedArrayType>().range;
                (void)r;
                for (int k = 0; k < it->second->count; k++) {   // foreach order: left index first
                    parts.push_back(ref(it->second->base + k));
                    pw += ew;
                }
            } else if (op.type->isUnpackedStruct() && streamStruct(*op.type)) {
                pw = uWidth(*op.type);
                parts.push_back(expr(op));
            } else if (isFixedArr(op) && op.type->getArrayElementType()->isIntegral()) {   // a[1:2]: its elements, left first
                std::vector<E> el;
                arrayElems(*op.type, op, el);
                int ew = (int)op.type->getArrayElementType()->getBitWidth();
                for (auto& e : el) { parts.push_back(resize(e, ew, false, false)); pw += ew; }
            } else {
                unsupported("this operand in a streaming concatenation");
            }
            w += pw;
        }
        needWidth(w);
        E v = parts.size() == 1 ? parts[0] : concat(std::move(parts));
        return reverseSlices(v, w, (int)sc.getSliceSize());
    }
    // Whether a stream has a container operand, so its width is known only
    // at run time.
    bool streamIsDyn(const slang::ast::StreamingConcatenationExpression& sc) {
        using namespace slang::ast;
        for (auto& st : sc.streams()) {
            const Expression& op = peel(st.withExpr ? *st.withExpr : *st.operand);
            if (op.kind == ExpressionKind::Streaming && streamIsDyn(op.as<StreamingConcatenationExpression>())) return true;
            if (st.withExpr && isDynType(*op.type)) return true;
            if (dynSig(op) >= 0) return true;
        }
        return false;
    }
    // q with [a:b] (11.4.14.4) whose bounds vary: a scratch container holding the slice.
    // Items past the end read as the missing-element value.
    int withSlice(const slang::ast::Expression& op) {
        using namespace slang::ast;
        int d = dynTemp(dynShapeOf(*op.type));
        prog.emit(dynCmd(d, D_DELETE));
        const auto* rs = op.kind == ExpressionKind::RangeSelect ? &op.as<RangeSelectExpression>() : nullptr;
        int s = rs ? dynSig(rs->value()) : -1;
        if (s < 0) { dynAppend(d, op); return d; }
        auto k32 = [](int64_t v) { return konst(make((uint64_t)v, 0, 32, 0, true)); };
        E l = resize(expr(rs->left()), 32, rs->left().type->isSigned(), true);
        E r = resize(expr(rs->right()), 32, rs->right().type->isSigned(), true);
        E lo, hi;
        switch (rs->getSelectionKind()) {
            case RangeSelectionKind::Simple: lo = l; hi = r; break;
            case RangeSelectionKind::IndexedUp: lo = l; hi = bin(Op::Sub, bin(Op::Add, l, r), k32(1)); break;
            default: lo = bin(Op::Add, bin(Op::Sub, l, r), k32(1)); hi = l; break;
        }
        int iv = fresh(32), end = fresh(32);
        { Instr h; h.k = IK::Assign; h.sig = iv; h.e = lo; prog.emit(h); }
        { Instr h; h.k = IK::Assign; h.sig = end; h.e = hi; prog.emit(h); }
        size_t top = prog.here();
        size_t out = emitJmpIfNot(bin(Op::Le, ref(iv, true), ref(end, true)));
        auto g = node(Op::DynGet, {ref(iv, true)});
        g->sig = s;
        Instr pb = dynCmd(d, D_PUSH_BACK); pb.e = g; prog.emit(pb);
        { Instr h; h.k = IK::Assign; h.sig = iv; h.e = bin(Op::Add, ref(iv, true), k32(1)); prog.emit(h); }
        size_t j = emitJmp();
        prog.code[j].target = top;
        prog.code[out].target = prog.here();
        return d;
    }
    // A stream with containers in it, as a StreamPack of its natural width.
    E streamDyn(const slang::ast::StreamingConcatenationExpression& sc) {
        using namespace slang::ast;
        std::vector<E> kids;
        for (auto& st : sc.streams()) {
            const Expression& op = peel(st.withExpr ? *st.withExpr : *st.operand);
            if (op.kind == ExpressionKind::Streaming) {
                const auto& in = op.as<StreamingConcatenationExpression>();
                if (streamIsDyn(in)) kids.push_back(streamDyn(in));
                else { int w = 0; kids.push_back(streamBits(in, w)); }
            } else if (int d = st.withExpr && isDynType(*op.type) ? withSlice(op) : dynSig(op); d >= 0) {
                const DynObj& o = dynShape(d);
                if (o.dflt.real || o.dflt.str || !op.type->getArrayElementType() ||
                    !op.type->getArrayElementType()->isIntegral())
                    unsupported("a container of non-integral elements in a streaming concatenation");
                auto n = node(Op::DynBits);
                n->sig = d;
                kids.push_back(n);
            } else if (op.type->isIntegral() || (op.type->isUnpackedStruct() && streamStruct(*op.type))) {
                kids.push_back(expr(op));
            } else {
                unsupported("this operand in a streaming concatenation with a container");
            }
        }
        auto n = node(Op::StreamPack, std::move(kids));
        n->hi = -1;
        n->lo = (int)sc.getSliceSize();
        return n;
    }
    E streamTo(const slang::ast::StreamingConcatenationExpression& sc, int w) {
        if (streamIsDyn(sc)) {
            E n = streamDyn(sc);
            n->hi = w;
            return n;
        }
        int sw = 0;
        E v = streamBits(sc, sw);
        if (w == sw) return v;
        if (w < sw) return slice(v, sw - 1, sw - w);
        return concat({v, konst(mk(0, w - sw))});
    }

    static std::string checkName(slang::ast::UniquePriorityCheck k) {
        using slang::ast::UniquePriorityCheck;
        return k == UniquePriorityCheck::Unique ? "unique" : k == UniquePriorityCheck::Unique0 ? "unique0" : "priority";
    }
    // A unique / priority violation report (12.4.2, 12.5.3), as a warning.
    // A unique / unique0 / priority violation (12.4.2.1): checked now,
    // reported in the Observed region unless the process reaches a flush
    // point first (it resumes from a wait, or an always_comb re-runs), so a
    // zero-delay glitch is not reported. $assertoff and friends control it
    // as an assertion of type 32 / 64 / 128 (20.11). The report is a warning.
    void violation(const slang::ast::Statement& st, const std::string& what, slang::ast::UniquePriorityCheck chk) {
        using slang::ast::UniquePriorityCheck;
        int type = chk == UniquePriorityCheck::Unique ? AT_UNIQUE : chk == UniquePriorityCheck::Unique0 ? AT_UNIQUE0 : AT_PRIORITY;
        if (!store) {   // nowhere to keep the report's process: print at once
            prog.emit(violationMsg(st, what));
            return;
        }
        int id = newAssert(type, AD_ASSERT, st);
        Lowerer lw(sigmap);
        lw.bind(subs, mkSig, tempSeq, store);
        lw.prog.emit(lw.violationMsg(st, what));
        std::vector<E> caps, slots;
        for (auto& x : lw.prog.code[0].args) {   // the time, taken when the check fails
            caps.push_back(x);
            E k = konst(Val{});
            slots.push_back(k);
            x = k;
        }
        store->push_back(std::make_unique<Prog>(std::move(lw.prog)));
        asserts()[id].fail = store->back().get();
        asserts()[id].failCap = std::move(caps);
        asserts()[id].failSlot = std::move(slots);
        Instr a; a.k = IK::Assert; a.sig = id; a.e = konst(mk(0, 1)); a.join = 1;
        prog.emit(a);
    }
    Instr violationMsg(const slang::ast::Statement& st, const std::string& what) {
        std::string file = "?";
        size_t line = 0;
        if (const auto* sm = srcMgr()) {
            auto loc = sm->getFullyOriginalLoc(st.sourceRange.start());
            file = std::string(sm->getFileName(loc));
            line = sm->getLineNumber(loc);
        }
        std::string esc;
        for (char ch : file) { esc += ch; if (ch == '%') esc += '%'; }
        Instr i; i.k = IK::Display;
        i.fmt = "Warning: \"" + esc + "\", " + std::to_string(line) + ": " + what + " at time %0t";
        E t = node(Op::Time); t->k = mk(1, 64);
        i.args.push_back(t);
        return i;
    }
    // Whether two items of a unique case can match the same selector value:
    // 0 never, 1 only when the selector has x or z bits, 2 maybe. Only
    // constant labels are checked here; anything else is checked at run time.
    // A unique / unique0 case with many items that may overlap. Checking
    // "no later item matches" right after each match costs O(n^2) code (for
    // Ibex's CSR decoder that made a 1.3 MB C++ function under -compile). So
    // the first matching item k is recorded in `hit`, one shared pass looks
    // for a later match (and reports once), then `hit` picks the statement.
    // The items' conditions are evaluated in the same order as before.
    void uniqueCaseLinear(const slang::ast::Statement& s, const slang::ast::CaseStatement& c,
                          const std::vector<E>& conds, const E& selUnknown) {
        size_t n = c.items.size();
        int hit = fresh(32);
        auto num = [](size_t k) { return konst(make(k, 0, 32)); };
        std::vector<size_t> toCheck, toEnd;
        for (size_t k = 0; k < n; k++) {
            size_t jf = emitJmpIfNot(conds[k]);
            Instr a; a.k = IK::Assign; a.sig = hit; a.e = num(k); prog.emit(a);
            Instr j; j.k = IK::Jmp; toCheck.push_back(prog.emit(j));
            prog.code[jf].target = prog.here();
        }
        // No item matched.
        bool needAll = c.check == slang::ast::UniquePriorityCheck::Unique;
        if (c.defaultCase) stmt(*c.defaultCase);
        else if (needAll) violation(s, checkName(c.check) + " case: no case item matches", c.check);
        { Instr j; j.k = IK::Jmp; toEnd.push_back(prog.emit(j)); }
        // A later item that also matches.
        for (size_t k : toCheck) prog.code[k].target = prog.here();
        std::vector<size_t> toPick;
        for (size_t m = 1; m < n; m++) {
            E later = bin(Op::LogAnd, bin(Op::Lt, ref(hit), num(m)), conds[m]);
            if (selUnknown) later = bin(Op::LogAnd, selUnknown, later);
            size_t jf = emitJmpIfNot(later);
            violation(s, checkName(c.check) + " case: more than one case item matches", c.check);
            Instr j; j.k = IK::Jmp; toPick.push_back(prog.emit(j));
            prog.code[jf].target = prog.here();
        }
        for (size_t k : toPick) prog.code[k].target = prog.here();
        // Run the matched item.
        for (size_t k = 0; k < n; k++) {
            size_t jf = emitJmpIfNot(bin(Op::Eq, ref(hit), num(k)));
            stmt(*c.items[k].stmt);
            Instr j; j.k = IK::Jmp; toEnd.push_back(prog.emit(j));
            prog.code[jf].target = prog.here();
        }
        for (size_t k : toEnd) prog.code[k].target = prog.here();
    }
    // An expression that reads signals and computes, with no side effect and
    // nothing that depends on how often it runs: a case selector that may be
    // evaluated once instead of once per label.
    static bool pureSel(const E& e) {
        if (!e) return true;
        switch (e->op) {
            case Op::Const: case Op::Ref: case Op::Not: case Op::And: case Op::Or: case Op::Xor:
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Eq: case Op::Ne: case Op::CaseEq:
            case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Shl: case Op::Shr: case Op::Asr:
            case Op::Resize: case Op::LogNot: case Op::LogAnd: case Op::LogOr: case Op::RedAnd: case Op::RedOr:
            case Op::RedXor: case Op::Slice: case Op::Index: case Op::Concat: case Op::Mux: case Op::ArrRead:
                break;
            default: return false;
        }
        if (!pureSel(e->at)) return false;
        for (const auto& k : e->kids) if (!pureSel(k)) return false;
        return true;
    }
    inline static int caseIds = 0;   // the last Instr::caseId given out
    static int caseMayOverlap(const slang::ast::CaseStatement& c) {
        using namespace slang::ast;
        using slang::logic_t;
        if (c.condition == CaseStatementCondition::Inside) return 2;
        bool z = c.condition == CaseStatementCondition::WildcardJustZ;
        bool xz = c.condition == CaseStatementCondition::WildcardXOrZ;
        auto wild = [&](logic_t b) { return (z && b == logic_t::z) || (xz && b.isUnknown()); };
        std::vector<slang::SVInt> seen;
        for (const auto& item : c.items)
            for (const Expression* x : item.expressions) {
                const auto* cv = x->getConstant();
                if (!cv || !cv->isInteger()) return 2;
                const slang::SVInt& v = cv->integer();
                if (!z && !xz && v.hasUnknown()) return 2;
                for (const auto& o : seen) {
                    if (o.getBitWidth() != v.getBitWidth()) return 2;
                    bool disjoint = false;   // some bit is concrete in both and differs
                    for (uint32_t b = 0; b < v.getBitWidth() && !disjoint; b++) {
                        logic_t p = v[(int32_t)b], q = o[(int32_t)b];
                        if (wild(p) || wild(q)) continue;
                        if (p.isUnknown() || q.isUnknown()) { if (!(p == q)) disjoint = true; continue; }
                        if (p.value != q.value) disjoint = true;
                    }
                    if (!disjoint) return 2;
                }
                seen.push_back(v);
            }
        return z || xz ? 1 : 0;
    }
    // unique if / unique0 if / priority if (12.4.2): the chain of else-ifs is
    // one statement. unique and unique0 also check that no later condition
    // is true; unique and priority report when none is true and there is no
    // final else.
    void checkedIf(const slang::ast::ConditionalStatement& top) {
        using namespace slang::ast;
        std::vector<const ConditionalStatement*> chain{&top};
        while (chain.back()->ifFalse && chain.back()->ifFalse->kind == StatementKind::Conditional &&
               chain.back()->ifFalse->as<ConditionalStatement>().check == UniquePriorityCheck::None)
            chain.push_back(&chain.back()->ifFalse->as<ConditionalStatement>());
        for (auto* c : chain)
            if (c->conditions.size() != 1 || c->conditions[0].pattern) unsupported("unique or priority if with a pattern");
        bool uniq = top.check == UniquePriorityCheck::Unique || top.check == UniquePriorityCheck::Unique0;
        bool needAll = top.check == UniquePriorityCheck::Unique || top.check == UniquePriorityCheck::Priority;
        std::vector<size_t> toEnd;
        for (size_t k = 0; k < chain.size(); k++) {
            Instr j; j.k = IK::JmpIfNot; j.e = expr(*chain[k]->conditions[0].expr);
            size_t jf = prog.emit(j);
            if (uniq && k + 1 < chain.size()) {
                E more;
                for (size_t m = k + 1; m < chain.size(); m++) {
                    E x = expr(*chain[m]->conditions[0].expr);
                    more = more ? bin(Op::LogOr, more, x) : x;
                }
                Instr o; o.k = IK::JmpIfNot; o.e = more;
                size_t jo = prog.emit(o);
                violation(top, checkName(top.check) + " if: more than one condition is true", top.check);
                prog.code[jo].target = prog.here();
            }
            stmt(chain[k]->ifTrue);
            Instr skip; skip.k = IK::Jmp;
            toEnd.push_back(prog.emit(skip));
            prog.code[jf].target = prog.here();
        }
        if (const Statement* last = chain.back()->ifFalse) stmt(*last);
        else if (needAll) violation(top, checkName(top.check) + " if: no condition is true", top.check);
        for (size_t k : toEnd) prog.code[k].target = prog.here();
    }

    // x matches p (12.6): x is held in a temporary, so pattern variables
    // bound along the way read a stable value.
    E matchExpr(const slang::ast::Expression& x, const slang::ast::Pattern& p,
                slang::ast::CaseStatementCondition cc) {
        int w = storeW(*x.type);
        needWidth(w);
        int v = fresh(w);
        Instr h; h.k = IK::Assign; h.sig = v; h.e = storageOf(*x.type, x); prog.emit(h);
        return matchP(p, ref(v), *x.type, w, cc);
    }
    // Whether v (w bits, the storage of a value of type t) matches p. A
    // pattern variable is bound here, whether or not the match succeeds.
    E matchP(const slang::ast::Pattern& p, E v, const slang::ast::Type& t0, int w,
             slang::ast::CaseStatementCondition cc) {
        using namespace slang::ast;
        const Type& t = t0.getCanonicalType();
        Op eqOp = cc == CaseStatementCondition::Normal ? Op::CaseEq
                  : cc == CaseStatementCondition::WildcardJustZ ? Op::CaseZEq : Op::CaseXEq;
        switch (p.kind) {
            case PatternKind::Wildcard: return konst(mk(1, 1));
            case PatternKind::Variable: {
                const auto& var = p.as<VariablePattern>().variable;
                int sig = fresh(w);
                sigmap[&var] = sig;
                Instr a; a.k = IK::Assign; a.sig = sig; a.e = v; prog.emit(a);
                return konst(mk(1, 1));
            }
            case PatternKind::Constant: {
                const Expression& ce = p.as<ConstantPattern>().expr;
                if (t.isFloating()) return fcmp(4, un(Op::AsReal, v), realOf(ce));   // 12.6: a real compares as ==
                if (t.isString()) {
                    auto n = node(Op::StrCmp, {un(Op::AsStr, v), strOf(ce)});
                    n->hi = 4;
                    return n;
                }
                return bin(eqOp, v, resize(storageOf(t, ce), w, ce.type->isSigned(), false));
            }
            case PatternKind::Tagged: {
                const auto& tp = p.as<TaggedPattern>();
                if (!isTaggedUnion(t)) unsupported("tagged pattern");
                int tb = tagBitsOf(t);
                int idx = tagIndexOf(t, tp.member);
                E cond = tb > 0 ? bin(Op::CaseEq, slice(v, w - 1, w - tb), konst(mk((uint64_t)idx, tb)))
                                : konst(mk(1, 1));
                if (tp.valuePattern) {
                    const Type& mt = tp.member.getType();
                    int fw = t.isUnpackedUnion() ? uWidth(mt) : (int)mt.getBitWidth();
                    if (fw > 0) cond = bin(Op::LogAnd, cond, matchP(*tp.valuePattern, slice(v, fw - 1, 0), mt, fw, cc));
                }
                return cond;
            }
            case PatternKind::Structure: {
                const auto& sp = p.as<StructurePattern>();
                E cond = konst(mk(1, 1));
                for (auto& fp : sp.patterns) {
                    const Type& ft = fp.field->getType();
                    int off, fw;
                    if (t.isUnpackedStruct()) std::tie(off, fw) = uField(t, *fp.field);
                    else { off = (int)fp.field->bitOffset; fw = (int)ft.getBitWidth(); }
                    if (fw <= 0) unsupported("structure pattern member");
                    cond = bin(Op::LogAnd, cond, matchP(*fp.pattern, slice(v, off + fw - 1, off), ft, fw, cc));
                }
                return cond;
            }
            default: unsupported("pattern");
        }
    }
    // tagged M v (11.9): the tag on top, the member value right-justified.
    E taggedValue(const slang::ast::TaggedUnionExpression& te) {
        using namespace slang::ast;
        const Type& t = te.type->getCanonicalType();
        int w = t.isUnpackedUnion() ? uWidth(t) : (int)t.getBitWidth();
        needWidth(w);
        int tb = tagBitsOf(t);
        int idx = tagIndexOf(t, te.member);
        const Type& mt = te.member.as<FieldSymbol>().getType();
        int fw = 0;
        E val;
        if (te.valueExpr && !mt.isVoid()) {
            fw = t.isUnpackedUnion() ? uWidth(mt) : (int)mt.getBitWidth();
            val = t.isUnpackedUnion() ? storageOf(mt, *te.valueExpr)
                                      : resize(expr(*te.valueExpr), fw, te.valueExpr->type->isSigned(), false);
        }
        std::vector<E> parts;
        if (tb > 0) parts.push_back(konst(mk((uint64_t)idx, tb)));
        if (w - tb - fw > 0) parts.push_back(konst(mk(0, w - tb - fw)));
        if (val) parts.push_back(val);
        return parts.size() == 1 ? parts[0] : concat(std::move(parts));
    }

    // The parts of a concatenation assignment target, first (most
    // significant) first, with their offsets in the whole (11.4.12, 10.4).
    static void concatParts(const slang::ast::Expression& e, int hi,
                            std::vector<std::tuple<const slang::ast::Expression*, int, int>>& out) {
        using namespace slang::ast;
        const Expression& x = peel(e);
        if (x.kind != ExpressionKind::Concatenation) {
            int w = (int)x.type->getBitWidth();
            needWidth(w);
            out.push_back({&x, hi, hi - w + 1});
            return;
        }
        for (auto* op : x.as<ConcatenationExpression>().operands()) {
            int w = (int)op->type->getBitWidth();
            concatParts(*op, hi, out);
            hi -= w;
        }
    }

    // {a, b} = v; {a, b} <= v; {a, b} = #d v: v is evaluated once, into a
    // temporary as wide as the target, and each part takes its slice.
    void concatAssign(const slang::ast::AssignmentExpression& a) {
        using namespace slang::ast;
        int w = (int)a.left().type->getBitWidth();
        needWidth(w);
        std::vector<std::tuple<const Expression*, int, int>> parts;
        const Expression* tgt = &a.left();
        if (const Expression* c = modportConn(peel(*tgt))) tgt = c;   // .W({a, b}) (25.5.4)
        concatParts(*tgt, w - 1, parts);
        int tmp = fresh(w);
        Instr hold; hold.k = IK::Assign; hold.sig = tmp;
        const Expression* saved = lvalue;
        if (a.isCompound()) lvalue = &a.left();
        hold.e = resize(expr(a.right()), w, false, false);
        lvalue = saved;
        prog.emit(hold);
        Instr d;   // the delay of a <= #d v
        bool later = false;
        if (const TimingControl* tc = a.timingControl) {
            if (tc->kind == TimingControlKind::Delay && a.isNonBlocking()) {
                delayInto(tc->as<DelayControl>().expr, d);
                later = true;
            } else if (a.isNonBlocking()) {
                unsupported("nonblocking assignment with an intra-assignment event control");
            } else if (tc->kind == TimingControlKind::RepeatedEvent) {
                unsupported("repeat event control on a concatenation assignment");
            } else {
                timing(*tc);
            }
        }
        for (auto& [x, hi, lo] : parts) {
            Instr i;
            i.k = a.isNonBlocking() ? IK::Nba : IK::Assign;
            lhs(*x, i);
            if (i.dyn >= 0 && a.isNonBlocking()) unsupported("nonblocking write to a container element");
            i.e = slice(ref(tmp), hi, lo);
            if (later) { i.delay = d.delay; i.de = d.de; i.nbaLater = true; }
            xpEmit(i, *x);
        }
    }

    // {>>{a, b}} = v; {<<8{a, b}} = v (11.4.14.3): the leftmost bits of v,
    // slices reversed for <<, are spread over the targets, first one on top.
    // {>>{a, q with [0 +: n], b}} = src (11.4.14.3, 11.4.14.4) with
    // containers or with [...] ranges among the targets: the stream is held
    // whole and the targets take their fields from the left in turn, each
    // range evaluated just before its array is unpacked. A container
    // without a range takes what the fixed-size targets after it leave; one
    // with a range grows to hold it. Too short a stream is an error.
    void streamAssignDyn(const slang::ast::AssignmentExpression& a, const slang::ast::StreamingConcatenationExpression& sc) {
        using namespace slang::ast;
        if (a.isNonBlocking()) unsupported("nonblocking streaming assignment to a container");
        auto k32 = [](int64_t v) { return konst(make((uint64_t)v, 0, 32, 0, true)); };
        // The source and its width.
        const Expression& r = peel(a.right());
        E src, W;
        if (r.kind == ExpressionKind::Streaming) {
            const auto& rs = r.as<StreamingConcatenationExpression>();
            if (!streamIsDyn(rs)) { int rw = 0; src = streamBits(rs, rw); W = k32(rw); }
            else { src = streamDyn(rs); W = nullptr; }
        } else if (int d = dynSig(r); d >= 0) {
            if (!r.type->getArrayElementType() || !r.type->getArrayElementType()->isIntegral())
                unsupported("streaming from a container of non-integral elements");
            auto bits = node(Op::DynBits);
            bits->sig = d;
            auto n = node(Op::StreamPack, {bits});
            n->hi = -1; n->lo = 0;
            src = n;
        } else if (r.type->isIntegral()) {
            src = expr(r);
            W = k32((int64_t)r.type->getBitWidth());
        } else unsupported("this source of a streaming assignment");
        if (int sl = (int)sc.getSliceSize(); sl > 0) {   // {<<s{...}} = src: the stream in s-bit slices, reversed
            auto n = node(Op::StreamPack, {src});
            n->hi = -1; n->lo = sl;
            src = n;
        }
        DynObj hold;
        hold.kind = 1;
        hold.dflt = make(0, 0, 1, 0);
        int t = dynTemp(std::move(hold));
        prog.emit(dynCmd(t, D_DELETE));
        Instr pb = dynCmd(t, D_PUSH_BACK); pb.e = src; prog.emit(pb);
        E held = node(Op::DynGet, {k32(0)});
        held->sig = t;
        int wsig = fresh(32);
        {
            Instr h; h.k = IK::Assign; h.sig = wsig;
            if (W) h.e = W;
            else {   // the held value's width, known only now
                h.e = sysFnNode(SF_VALUE_WIDTH, {held});
            }
            prog.emit(h);
        }
        int pos = fresh(32);   // bits taken so far, from the left
        { Instr h; h.k = IK::Assign; h.sig = pos; h.e = k32(0); prog.emit(h); }
        std::vector<size_t> short_;   // jumps to the too-short error
        auto take = [&](int w) -> E {   // the next w bits; checks there are that many
            short_.push_back(emitJmpIfNot(bin(Op::Le, bin(Op::Add, ref(pos, true), k32(w)), ref(wsig, true))));
            auto f = node(Op::DynSlice, {held, bin(Op::Sub, bin(Op::Sub, ref(wsig, true), k32(1)), ref(pos, true))});
            f->hi = w; f->lo = 1;
            return f;
        };
        auto advance = [&](int w) {
            Instr h; h.k = IK::Assign; h.sig = pos; h.e = bin(Op::Add, ref(pos, true), k32(w)); prog.emit(h);
        };
        // A loop over iv from lo to hi (inclusive, either direction), body(iv).
        auto loop = [&](E lo, E hi, bool down, const std::function<void(E)>& body) {
            int iv = fresh(32), end = fresh(32);
            { Instr h; h.k = IK::Assign; h.sig = iv; h.e = lo; prog.emit(h); }
            { Instr h; h.k = IK::Assign; h.sig = end; h.e = hi; prog.emit(h); }
            size_t top = prog.here();
            size_t out = emitJmpIfNot(down ? bin(Op::Ge, ref(iv, true), ref(end, true)) : bin(Op::Le, ref(iv, true), ref(end, true)));
            body(ref(iv, true));
            { Instr h; h.k = IK::Assign; h.sig = iv; h.e = down ? bin(Op::Sub, ref(iv, true), k32(1)) : bin(Op::Add, ref(iv, true), k32(1)); prog.emit(h); }
            size_t j = emitJmp();
            prog.code[j].target = top;
            prog.code[out].target = prog.here();
        };
        // The index range of a with [...]: first, last, and whether it counts down.
        auto range = [&](const Expression& wx, E& lo, E& hi) {
            if (wx.kind == ExpressionKind::ElementSelect) {
                lo = hi = resize(expr(wx.as<ElementSelectExpression>().selector()), 32, true, true);
                return;
            }
            const auto& rs = wx.as<RangeSelectExpression>();
            E l = resize(expr(rs.left()), 32, rs.left().type->isSigned(), true);
            E rr = resize(expr(rs.right()), 32, rs.right().type->isSigned(), true);
            switch (rs.getSelectionKind()) {
                case RangeSelectionKind::Simple: lo = l; hi = rr; break;
                case RangeSelectionKind::IndexedUp: lo = l; hi = bin(Op::Sub, bin(Op::Add, l, rr), k32(1)); break;
                default: lo = bin(Op::Add, bin(Op::Sub, l, rr), k32(1)); hi = l; break;
            }
            int a0 = fresh(32), a1 = fresh(32);   // fixed now, before the elements are written
            { Instr h; h.k = IK::Assign; h.sig = a0; h.e = lo; prog.emit(h); }
            { Instr h; h.k = IK::Assign; h.sig = a1; h.e = hi; prog.emit(h); }
            lo = ref(a0, true); hi = ref(a1, true);
        };
        const auto& streams = sc.streams();
        for (size_t si = 0; si < streams.size(); si++) {
            const auto& st = streams[si];
            const Expression& op = peel(*st.operand);
            if (int d = dynSig(op); d >= 0) {
                const Type* et = op.type->getArrayElementType();
                if (dynShape(d).kind == 2 || !et || !et->isIntegral()) unsupported("this container in a streaming assignment");
                int ew = (int)et->getBitWidth();
                E lo, hi;
                if (st.withExpr) {   // grows to hold the range (11.4.14.4)
                    range(*st.withExpr, lo, hi);
                    E need = bin(Op::Add, hi, k32(1));
                    size_t big = emitJmpIfNot(bin(Op::Gt, need, resize(dynSize(d), 32, false, true)));
                    Instr g = dynCmd(d, D_NEW); g.args = {need}; g.dsrc = d; prog.emit(g);
                    prog.code[big].target = prog.here();
                } else {   // greedy: whatever the fixed-size targets after it leave
                    int after = 0;
                    for (size_t sj = si + 1; sj < streams.size(); sj++) {
                        const Expression& o2 = peel(*streams[sj].operand);
                        if (!streams[sj].withExpr && o2.type->isIntegral()) after += (int)o2.type->getBitWidth();
                        else if (!streams[sj].withExpr && o2.type->isUnpackedStruct() && streamStruct(*o2.type)) after += uWidth(*o2.type);
                        else if (!streams[sj].withExpr && isFixedArr(o2)) after += (int)o2.type->getBitstreamWidth();
                    }
                    int n = fresh(32);
                    Instr h; h.k = IK::Assign; h.sig = n;
                    h.e = bin(Op::Div, bin(Op::Sub, bin(Op::Sub, ref(wsig, true), ref(pos, true)), k32(after)), k32(ew));
                    prog.emit(h);
                    Instr g = dynCmd(d, D_NEW); g.args = {ref(n, true)}; prog.emit(g);
                    lo = k32(0);
                    hi = bin(Op::Sub, ref(n, true), k32(1));
                }
                loop(lo, hi, false, [&](E iv) {
                    Instr w = dynCmd(d, D_SET); w.args = {iv}; w.e = resize(take(ew), ew, false, et->isSigned()); prog.emit(w);
                    advance(ew);
                });
                continue;
            }
            if ((op.type->isIntegral() || (op.type->isUnpackedStruct() && streamStruct(*op.type))) && !st.withExpr) {
                int w = op.type->isIntegral() ? (int)op.type->getBitWidth() : uWidth(*op.type);
                Instr i; i.k = IK::Assign;
                lhs(op, i);
                i.e = take(w);
                xpEmit(i, op);
                advance(w);
                continue;
            }
            if (isFixedArr(op) && op.type->getArrayElementType()->isIntegral() &&
                (op.kind == ExpressionKind::NamedValue || op.kind == ExpressionKind::HierarchicalValue)) {
                auto A = arrFind(&op.as<ValueExpressionBase>().symbol);
                if (!A || A->dims.size() != 1) unsupported("this array in a streaming assignment");
                int ew = (int)op.type->getArrayElementType()->getBitWidth();
                auto [l, rr] = A->dims[0];
                E lo = k32(l), hi = k32(rr);
                bool down = l > rr;
                if (st.withExpr) { range(*st.withExpr, lo, hi); down = false; }   // index order as written
                auto body = [&](E iv) {
                    Instr i; i.k = IK::Assign; i.sig = A->base; i.arr = A; i.aidx = {iv}; i.e = take(ew);
                    xpDirect(i);
                    advance(ew);
                };
                if (st.withExpr) {   // a range that runs either way
                    size_t dn = emitJmpIfNot(bin(Op::Le, lo, hi));
                    loop(lo, hi, false, body);
                    size_t j = emitJmp();
                    prog.code[dn].target = prog.here();
                    loop(lo, hi, true, body);
                    prog.code[j].target = prog.here();
                } else loop(lo, hi, down, body);
                continue;
            }
            unsupported("this target of a streaming assignment");
        }
        size_t done = emitJmp();
        for (size_t j : short_) prog.code[j].target = prog.here();
        Instr e; e.k = IK::Display; e.fmt = "Error: the stream is too short for the targets of a streaming assignment (11.4.14.3)";
        prog.emit(e);
        prog.code[done].target = prog.here();
    }
    void streamAssign(const slang::ast::AssignmentExpression& a) {
        using namespace slang::ast;
        if (a.timingControl || a.isCompound()) unsupported("delayed or compound streaming assignment");
        const auto& sc = peel(a.left()).as<StreamingConcatenationExpression>();
        if (sc.streams().size() == 1 && !sc.streams()[0].withExpr) {   // {<<bit{q}} = v: q gets every element of the stream
            const Expression& op = peel(*sc.streams()[0].operand);
            const Expression& r = peel(a.right());
            if (int d = dynSig(op); d >= 0 && r.type->isIntegral()) {
                if (dynShape(d).kind == 2) unsupported("streaming into an associative array");
                int rw = (int)r.type->getBitWidth();
                needWidth(rw);
                Instr i = dynCmd(d, D_UNPACK);
                i.e = unreverseSlices(expr(r), rw, (int)sc.getSliceSize());
                prog.emit(i);
                return;
            }
        }
        for (auto& st : sc.streams()) {   // a container or a with [...] target: the stream is cut at run time
            const Expression& op = peel(*st.operand);
            if (st.withExpr || dynSig(op) >= 0 || (op.type->isUnpackedArray() && !op.type->isIntegral())) {
                streamAssignDyn(a, sc);
                return;
            }
        }
        std::vector<std::pair<const Expression*, int>> tg;   // target, width
        int sw = 0;
        for (auto& st : sc.streams()) {
            const Expression& op = peel(*st.operand);
            bool ss = op.type->isUnpackedStruct() && streamStruct(*op.type);
            if (!op.type->isIntegral() && !ss) unsupported("this target of a streaming assignment");
            int w = ss ? uWidth(*op.type) : (int)op.type->getBitWidth();
            tg.push_back({&op, w});
            sw += w;
        }
        needWidth(sw);
        const Expression& r = peel(a.right());
        int rw = (int)r.type->getBitWidth();
        E v;
        if (r.kind == ExpressionKind::Streaming && !streamIsDyn(r.as<StreamingConcatenationExpression>())) {
            v = streamBits(r.as<StreamingConcatenationExpression>(), rw);   // {>>{a}} = {<<8{a}}
            if (rw < sw) unsupported("streaming assignment from a narrower stream");
        } else {
            if (!r.type->isIntegral() || rw < sw) unsupported("streaming assignment from a narrower or non-integral value");
            v = expr(r);
        }
        if (rw > sw) v = slice(v, rw - 1, rw - sw);
        int tmp = fresh(sw);
        Instr hold; hold.k = IK::Assign; hold.sig = tmp; hold.e = unreverseSlices(v, sw, (int)sc.getSliceSize());
        prog.emit(hold);
        int hi = sw - 1;
        for (auto& [x, w] : tg) {
            Instr i;
            i.k = a.isNonBlocking() ? IK::Nba : IK::Assign;
            lhs(*x, i);
            i.e = slice(ref(tmp), hi, hi - w + 1);
            xpEmit(i, *x);
            hi -= w;
        }
    }

    // Assignments, ++/--, and system calls used as statements.
    void exprStmt(const slang::ast::Expression& ex) {
        using namespace slang::ast;
        if (ex.kind == ExpressionKind::Assignment) {
            const auto& a = ex.as<AssignmentExpression>();
            if (syncDrive(a) || vifSyncDrive(a)) return;   // cb.v <= e, v.cb.v <= e (14.16, 25.9)
            if (const Expression& l = peel(a.left()); a.left().type->isEvent() && ValueExpressionBase::isKind(l.kind) &&
                eventPtr().count(&l.as<ValueExpressionBase>().symbol)) {   // e2 = e1: e2 names e1's event from now on (6.17)
                auto p = eventPtr().find(&l.as<ValueExpressionBase>().symbol);
                if (a.isNonBlocking() || a.timingControl) unsupported("this event assignment");
                Instr i; i.k = IK::Assign; i.sig = p->second; i.e = eventAddr(a.right());
                prog.emit(i);
                return;
            }
            if (const Expression& l = peel(a.left()); l.kind == ExpressionKind::ElementSelect &&
                l.as<ElementSelectExpression>().value().type->isString()) {   // s[i] = c (6.16): s.putc(i, c)
                if (a.isNonBlocking() || a.timingControl || a.isCompound()) unsupported("this write to a string character");
                const auto& sel = l.as<ElementSelectExpression>();
                Instr i; i.k = IK::Assign;
                lhs(sel.value(), i);
                auto m = node(Op::StrMethod, {exprStr(sel.value()), expr(sel.selector()), expr(a.right())});
                m->hi = S_PUTC;
                i.e = m;
                prog.emit(i);
                return;
            }
            if (int d = dynSig(a.left()); d >= 0) {   // q = '{1, 2}; d = new[8]; aa = other;
                if (a.isNonBlocking() || a.timingControl || a.isCompound()) unsupported("this container assignment");
                assignContainer(d, a.right());
                return;
            }
            if (a.left().type->isUnpackedArray() && !a.timingControl && !a.isCompound() &&
                assignWholeArray(a.left(), a.right(), a.isNonBlocking()))
                return;
            if (const Expression* mc = modportConn(peel(a.left()));
                peel(mc ? *mc : a.left()).kind == ExpressionKind::Concatenation) {
                concatAssign(a);
                return;
            }
            if (peel(a.left()).kind == ExpressionKind::Streaming) {
                streamAssign(a);
                return;
            }
            Instr i;
            i.k = a.isNonBlocking() ? IK::Nba : IK::Assign;
            lhs(a.left(), i);
            if (i.dyn >= 0 && (a.isNonBlocking() || a.timingControl))
                unsupported("nonblocking or delayed write to a container element");
            const Expression* saved = lvalue;
            if (a.isCompound()) lvalue = &a.left();
            i.e = expr(a.right());
            lvalue = saved;
            if (const TimingControl* tc = a.timingControl; tc && tc->kind != TimingControlKind::Delay) {
                // a = @(e) b; a = repeat (n) @(e) b (9.4.5): b is taken now and
                // written after the event (n events for repeat).
                if (a.isNonBlocking()) {   // a <= @(e) b: a forked process waits and then schedules the update
                    if (!store) unsupported("nonblocking assignment with an intra-assignment event control here");
                    int w = storeW(*a.left().type);
                    needWidth(w);
                    Instr f; f.k = IK::Fork; f.join = 2;
                    FrameLayout* flay = newLayout("nba event");
                    int slot = flay->add(w, w > 64 ? allx(w) : Val{});
                    f.init.push_back({slot, resize(i.e, w, a.right().type->isSigned(), false)});
                    int cslot = -1;
                    const TimingControl* ev = tc;
                    if (tc->kind == TimingControlKind::RepeatedEvent) {
                        const auto& re = tc->as<RepeatedEventControl>();
                        cslot = flay->add(32, Val{});
                        f.init.push_back({cslot, resize(expr(re.expr), 32, re.expr.type->isSigned(), true)});
                        ev = &re.event;
                    }
                    f.lay = flay;
                    Lowerer lw(sigmap);
                    lw.bind(subs, mkSig, tempSeq, store);
                    lw.tsUnit = tsUnit; lw.tsPrec = tsPrec;
                    lw.current = current;
                    lw.retSig = retSig;
                    lw.scope = scope;
                    lw.sampleEv = sampleEv;
                    lw.curClass = curClass;
                    FrameLayout* bl = newLayout("fork branch");
                    lw.frames = frames;
                    lw.frames.push_back(flay);
                    lw.frames.push_back(bl);
                    f.blays.push_back(bl);
                    if (cslot >= 0) {
                        int cnt = lw.relIn(flay, cslot);
                        size_t top = lw.prog.here();
                        Instr j; j.k = IK::JmpIfNot; j.e = bin(Op::Gt, ref(cnt, true), konst(make(0, 0, 32, 0, true)));
                        size_t jf = lw.prog.emit(j);
                        lw.timing(*ev);
                        Instr dec; dec.k = IK::Assign; dec.sig = cnt; dec.e = bin(Op::Sub, ref(cnt, true), konst(make(1, 0, 32, 0, true)));
                        lw.prog.emit(dec);
                        Instr back; back.k = IK::Jmp; back.target = top; lw.prog.emit(back);
                        lw.prog.code[jf].target = lw.prog.here();
                    } else {
                        lw.timing(*ev);
                    }
                    Instr n; n.k = IK::Nba;
                    lw.lhs(a.left(), n);
                    n.e = ref(lw.relIn(flay, slot), a.left().type->isSigned());
                    lw.prog.emit(n);
                    lw.finishImplicit();
                    store->push_back(std::make_unique<Prog>(std::move(lw.prog)));
                    f.branches.push_back(store->back().get());
                    prog.emit(f);
                    return;
                }
                int w = (int)a.left().type->getBitWidth();
                needWidth(w);
                int tmp = fresh(w);
                Instr hold; hold.k = IK::Assign; hold.sig = tmp; hold.e = i.e;
                prog.emit(hold);
                if (tc->kind == TimingControlKind::RepeatedEvent) {
                    const auto& re = tc->as<RepeatedEventControl>();
                    int cnt = fresh(32);
                    Instr c0; c0.k = IK::Assign; c0.sig = cnt; c0.e = resize(expr(re.expr), 32, re.expr.type->isSigned(), true);
                    prog.emit(c0);
                    size_t top = prog.here();
                    Instr j; j.k = IK::JmpIfNot; j.e = bin(Op::Gt, ref(cnt, true), konst(make(0, 0, 32, 0, true)));
                    size_t jf = prog.emit(j);
                    timing(re.event);
                    Instr dec; dec.k = IK::Assign; dec.sig = cnt; dec.e = bin(Op::Sub, ref(cnt, true), konst(make(1, 0, 32, 0, true)));
                    prog.emit(dec);
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    prog.code[jf].target = prog.here();
                } else {
                    timing(*tc);
                }
                i.e = ref(tmp, a.left().type->isSigned());
                prog.emit(i);
                return;
            }
            if (const TimingControl* tc = a.timingControl) {   // a = #d b; a <= #d b (9.4.5)
                Instr d;
                delayInto(tc->as<DelayControl>().expr, d);
                if (a.isNonBlocking()) {   // the update is scheduled d later; the process goes on
                    i.delay = d.delay;
                    i.de = d.de;
                    i.nbaLater = true;
                    xpEmit(i, a.left());
                    return;
                }
                // Blocking: the value is taken now, written after the wait.
                int w = (int)a.left().type->getBitWidth();
                needWidth(w);
                int tmp = fresh(w);
                Instr hold; hold.k = IK::Assign; hold.sig = tmp; hold.e = i.e;
                prog.emit(hold);
                d.k = IK::WaitDelay;
                prog.emit(d);
                i.e = ref(tmp);
                prog.emit(i);
                return;
            }
            xpEmit(i, a.left());
        } else if (ex.kind == ExpressionKind::UnaryOp) {   // i++ / ++i / i-- / --i
            const auto& u = ex.as<UnaryExpression>();
            bool inc = u.op == UnaryOperator::Preincrement || u.op == UnaryOperator::Postincrement;
            bool dec = u.op == UnaryOperator::Predecrement || u.op == UnaryOperator::Postdecrement;
            if (!inc && !dec) unsupported("unary operator as statement");
            Instr i; i.k = IK::Assign;
            lhs(u.operand(), i);
            i.e = bin(inc ? Op::Add : Op::Sub, expr(u.operand()), konst(mk(1, 32)));
            prog.emit(i);
        } else if (ex.kind == ExpressionKind::Call) {
            const auto& call = ex.as<CallExpression>();
            if (call.isSystemCall()) sysCall(call);
            else if (E r = cgCall(call)) {   // c.sample(), c.start(), ... (19.8)
                Instr i; i.k = IK::Assign; i.sig = fresh(64); i.e = std::move(r);
                prog.emit(i);
            } else emitUserCall(call, false);
        } else if (ex.kind == ExpressionKind::NewClass && ex.as<NewClassExpression>().isSuperClass) {
            if (&ex != superNewDone) unsupported("super.new that is not the first statement of a constructor");
        } else {
            unsupported("expression statement");
        }
    }

    // @* waits on every signal the block reads (known only after lowering).
    void finishImplicit() {
        if (implicitWaits.empty()) return;
        auto rs = readSet(prog);
        if (rs.empty()) unsupported("@* in a block that reads no signals");
        for (size_t pc : implicitWaits) prog.code[pc] = waitInstr(rs);
    }

    // foreach over a dynamic array or queue: index 0 up to size() - 1, read
    // again each pass. Over an associative array: first() then next().
    // foreach over a container (12.7.3): one loop per dimension; a dimension
    // past the first walks the inner container of a container of containers.
    void foreachDyn(const slang::ast::ForeachLoopStatement& f, int d) {
        if (f.loopDims.size() == 1 && !f.loopDims[0].loopVar) { stmt(f.body); return; }
        loops.push_back({});
        foreachDynDim(f, d, 0);
        patch(loops.back().breaks, prog.here());
        loops.pop_back();
    }
    // The dimensions of a fixed-size array inside a container element
    // (int d[][3]): each loop variable from the left bound to the right.
    void foreachFixedDims(const slang::ast::ForeachLoopStatement& f, size_t k, const slang::ast::Type& t) {
        if (k == f.loopDims.size()) { stmt(f.body); return; }
        const auto& a = t.getCanonicalType().as<slang::ast::FixedSizeUnpackedArrayType>();
        const auto* var = f.loopDims[k].loopVar;
        int64_t l = a.range.left, r = a.range.right, step = l <= r ? 1 : -1;
        int v = fresh(32);
        if (var) bindVar(var, v);
        Instr init; init.k = IK::Assign; init.sig = v; init.e = konst(make((uint64_t)l, 0, 32, 0, true));
        prog.emit(init);
        size_t top = prog.here();
        Instr j; j.k = IK::JmpIfNot;
        j.e = bin(step > 0 ? Op::Le : Op::Ge, ref(v, true), konst(make((uint64_t)r, 0, 32, 0, true)));
        size_t jf = prog.emit(j);
        auto saved = std::move(loops.back().continues);
        loops.back().continues.clear();
        const auto& next = a.elementType;
        if (k + 1 < f.loopDims.size()) {
            if (next.getCanonicalType().kind != slang::ast::SymbolKind::FixedSizeUnpackedArrayType)
                unsupported("foreach over more dimensions than the array has");
            foreachFixedDims(f, k + 1, next);
        } else stmt(f.body);
        patch(loops.back().continues, prog.here());
        loops.back().continues = std::move(saved);
        Instr st; st.k = IK::Assign; st.sig = v;
        st.e = bin(Op::Add, ref(v, true), konst(make((uint64_t)step, 0, 32, 0, true)));
        prog.emit(st);
        Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
        prog.code[jf].target = prog.here();
    }
    // Dimension k of a foreach over container d; a container of containers
    // (7.5, 7.10) loops over each element's own elements next.
    void foreachDynDim(const slang::ast::ForeachLoopStatement& f, int d, size_t k) {
        const auto* var = f.loopDims[k].loopVar;
        const DynObj o = dynShape(d);
        bool last = k + 1 == f.loopDims.size();
        const slang::ast::Type* et = nullptr;   // the element type, when it is a fixed-size array the loop goes on into
        if (!last && !o.elem) {
            et = f.arrayRef.type;
            for (size_t j = 0; j <= k && et; j++) et = et->getArrayElementType();
            if (!et || et->getCanonicalType().kind != slang::ast::SymbolKind::FixedSizeUnpackedArrayType)
                unsupported("foreach over more dimensions than the container has");
        }
        auto body = [&](E key) {
            if (last) {
                stmt(f.body);
                return;
            }
            if (et) { foreachFixedDims(f, k + 1, *et); return; }
            auto n = node(Op::DynElemRef, {key});
            n->sig = d;
            foreachDynDim(f, elemRef(d, n), k + 1);
        };
        // continue in the body goes to the step of the innermost loop only
        auto saved = std::move(loops.back().continues);
        loops.back().continues.clear();
        if (o.kind != 2) {
            int v = fresh(32);
            if (var) bindVar(var, v);
            Instr init; init.k = IK::Assign; init.sig = v; init.e = konst(make(0, 0, 32, 0, true));
            prog.emit(init);
            size_t top = prog.here();
            Instr j; j.k = IK::JmpIfNot; j.e = bin(Op::Lt, ref(v, true), dynSize(d));
            size_t jf = prog.emit(j);
            body(ref(v, true));
            if (last) patch(loops.back().continues, prog.here());
            Instr st; st.k = IK::Assign; st.sig = v;
            st.e = bin(Op::Add, ref(v, true), konst(make(1, 0, 32, 0, true)));
            prog.emit(st);
            Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
            prog.code[jf].target = prog.here();
        } else {
            int v = fresh(o.strKey ? 64 : o.keyW);
            if (var) bindVar(var, v);
            int ok = fresh(32);
            Instr first = dynCmd(d, D_FIRST); first.dkey = v; first.dres = ok;
            prog.emit(first);
            size_t top = prog.here();
            Instr j; j.k = IK::JmpIfNot; j.e = ref(ok);
            size_t jf = prog.emit(j);
            body(ref(v, !o.strKey && o.keySgn));
            if (last) patch(loops.back().continues, prog.here());
            Instr nx = dynCmd(d, D_NEXT); nx.dkey = v; nx.dres = ok;
            prog.emit(nx);
            Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
            prog.code[jf].target = prog.here();
        }
        loops.back().continues = std::move(saved);
    }

    int stmtDepth = 0;   // > 0 while lowering a procedural statement
    // Source of a location, for coverage (set by lower() when -cov is on).
    static const slang::SourceManager*& covSM() { static const slang::SourceManager* sm = nullptr; return sm; }
    static void covLoc(slang::SourceLocation loc) {
        auto* sm = covSM();
        if (!sm || !loc.valid()) return;
        if (hideLocIf() && hideLocIf()(loc)) { g_emitLoc = {}; return; }
        auto ol = sm->getFullyOriginalLoc(loc);
        g_emitLoc = {covFileId(std::string(sm->getFullPath(ol.buffer()).string())), (uint32_t)sm->getLineNumber(ol)};
    }
    void stmt(const slang::ast::Statement& s) {
        struct Depth { int& d; Depth(int& x) : d(x) { d++; } ~Depth() { d--; } } depth(stmtDepth);
        if (g_cov && !prog.inst) prog.inst = instanceAround(scope);   // statement coverage per instance (40)
        // Each statement's instructions carry its line; a block's own
        // instructions keep the line of the statement around them.
        SrcLoc savedLoc = g_emitLoc;
        struct RestoreLoc { SrcLoc& l; SrcLoc v; ~RestoreLoc() { g_emitLoc = v; } } restoreLoc{g_emitLoc, savedLoc};
        if (s.kind != slang::ast::StatementKind::List && s.kind != slang::ast::StatementKind::Block) {
            covLoc(s.sourceRange.start());
            lastLoc() = s.sourceRange.start();
        }
        using namespace slang::ast;
        switch (s.kind) {
            case StatementKind::Empty: break;
            case StatementKind::List:
                if (!loops.empty() && loops.back().xpBrk >= 0) { xpList(s.as<StatementList>().list, 0); break; }   // -xprop
                for (auto* st : s.as<StatementList>().list) stmt(*st);
                break;
            case StatementKind::Block: {
                const auto& b = s.as<BlockStatement>();
                if (b.blockKind == StatementBlockKind::Sequential) {
                    if (!b.blockSymbol || b.blockSymbol->name.empty()) { stmt(b.body); break; }
                    size_t k = named.size();
                    named.push_back({b.blockSymbol, {}});
                    if (blockIds().targets.count(b.blockSymbol) && hasFork(b.body)) forkyBlocks.insert(b.blockSymbol);
                    labelSym = b.blockSymbol;   // label: assert ... names the assertion
                    labelOf = &b.body;
                    size_t s0 = prog.here();
                    stmt(b.body);
                    if (blockIds().targets.count(b.blockSymbol))   // another process may disable it (9.6.2)
                    { prog.spans.push_back({blockIds().of(b.blockSymbol), s0, prog.here()}); anySpans() = true; }
                    patch(named[k].second, prog.here());   // disable blk: jump past it
                    named.pop_back();
                    break;
                }
                // fork ... join / join_any / join_none (9.3.2): each statement
                // of the block is its own process. Fork starts them all, then
                // waits for all, for the first, or not at all.
                if (!store) unsupported("fork here");
                Instr f; f.k = IK::Fork;
                f.join = b.blockKind == StatementBlockKind::JoinAll ? 0
                       : b.blockKind == StatementBlockKind::JoinAny ? 1 : 2;
                // Automatic variables declared in the fork block itself get
                // one frame per execution of the fork, shared by its branches
                // and set before they start (9.3.2: fork automatic int j = i;).
                FrameLayout* flay = nullptr;
                if (b.body.kind == StatementKind::List)
                    for (const Statement* st : b.body.as<StatementList>().list) {
                        if (st->kind != StatementKind::VariableDeclaration) continue;
                        const auto& v = st->as<VariableDeclStatement>().symbol;
                        if (v.lifetime != VariableLifetime::Automatic) continue;
                        const Type& vt = v.getType().getCanonicalType();
                        if (vt.isUnpackedArray() || vt.isEvent()) unsupported("automatic array, container or event declared in a fork block");
                        if (!flay) flay = newLayout("fork");
                        declLocal(v, flay);
                        const Expression* init = v.getInitializer();
                        f.init.push_back({localMap().at(&v).off, init ? expr(*init) : konst(initOf(v.getType()))});
                    }
                f.lay = flay;
                auto branch = [&](const Statement& st) {
                    Lowerer lw(sigmap);
                    lw.bind(subs, mkSig, tempSeq, store);
                    lw.tsUnit = tsUnit; lw.tsPrec = tsPrec;
                    lw.current = current;
                    lw.retSig = retSig;
                    lw.scope = scope;
                    lw.sampleEv = sampleEv;
                    lw.curClass = curClass;   // a fork in a method: `this` is the method's object
                    // Each branch has a frame for its own automatic variables
                    // (and temporaries), unless it is plain static code.
                    FrameLayout* bl = newLayout("fork branch");
                    lw.frames = frames;
                    if (flay) lw.frames.push_back(flay);
                    lw.frames.push_back(bl);
                    lw.declLocals(st, bl);
                    bool framed = !frames.empty() || flay || !bl->w.empty();
                    if (!framed) lw.frames.clear();
                    f.blays.push_back(framed ? bl : nullptr);
                    lw.stmt(st);
                    lw.finishImplicit();
                    // disable <named fork> ends every branch; disable <named
                    // branch block> ends that branch.
                    if (b.blockSymbol && !b.blockSymbol->name.empty()) lw.prog.roots.push_back(rootId(b.blockSymbol));
                    if (const auto* nb = namedSeq(st)) lw.prog.roots.push_back(rootId(nb));
                    store->push_back(std::make_unique<Prog>(std::move(lw.prog)));
                    f.branches.push_back(store->back().get());
                };
                if (b.body.kind == StatementKind::List) {
                    for (const Statement* st : b.body.as<StatementList>().list) {
                        // A static declaration in a fork block belongs to the parent.
                        if (st->kind == StatementKind::VariableDeclaration) {
                            if (st->as<VariableDeclStatement>().symbol.lifetime != VariableLifetime::Automatic) stmt(*st);
                            continue;
                        }
                        branch(*st);
                    }
                } else if (b.body.kind != StatementKind::Empty) {
                    branch(b.body);
                }
                prog.emit(f);
                break;
            }
            case StatementKind::Wait: {   // wait (cond) stmt (9.4.3)
                // top: if (cond) goto body; @(signals in cond); goto top
                const auto& w = s.as<WaitStatement>();
                size_t top = prog.here();
                Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                size_t jf = prog.emit(j);
                size_t jbody = emitJmp();
                prog.code[jf].target = prog.here();
                if (hasRel(prog.code[jf].e)) {   // automatic variables: resolved when the wait starts
                    Instr w2; w2.k = IK::WaitEdge; w2.dynWait = true; w2.e = prog.code[jf].e;
                    prog.emit(w2);
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    prog.code[jbody].target = prog.here();
                    stmt(w.stmt);
                    break;
                }
                std::vector<int> refs;
                collectRefs(prog.code[jf].e, refs);
                if (refs.empty()) refs.push_back(freshStatic(1));   // wait(0): a signal nothing writes, so forever
                std::vector<std::pair<int, Edge>> rs;
                for (int r : refs) rs.push_back({r, ANY});
                prog.emit(waitInstr(rs));
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                prog.code[jbody].target = prog.here();
                stmt(w.stmt);
                break;
            }
            case StatementKind::Timed: {
                const auto& t = s.as<TimedStatement>();
                timing(t.timing);
                stmt(t.stmt);
                break;
            }
            case StatementKind::Conditional: {
                // if (a &&& b matches p &&& c) (12.6.2): each condition in
                // turn; a pattern binds its variables for the ones after it.
                const auto& c = s.as<ConditionalStatement>();
                if (xp.on && xpIf(c)) break;   // -xprop
                if (c.check != UniquePriorityCheck::None) { checkedIf(c); break; }
                std::vector<size_t> jfs;
                for (auto& cd : c.conditions) {
                    Instr j; j.k = IK::JmpIfNot;
                    j.e = cd.pattern ? matchExpr(*cd.expr, *cd.pattern, CaseStatementCondition::Normal)
                                     : expr(*cd.expr);
                    jfs.push_back(prog.emit(j));
                }
                stmt(c.ifTrue);
                if (c.ifFalse) {
                    Instr skip; skip.k = IK::Jmp;
                    size_t js = prog.emit(skip);
                    for (size_t jf : jfs) prog.code[jf].target = prog.here();
                    stmt(*c.ifFalse);
                    prog.code[js].target = prog.here();
                } else {
                    for (size_t jf : jfs) prog.code[jf].target = prog.here();
                }
                break;
            }
            case StatementKind::PatternCase: {   // case (x) matches (12.6.1)
                const auto& c = s.as<PatternCaseStatement>();
                int w = storeW(*c.expr.type);
                needWidth(w);
                int v = fresh(w);
                Instr h; h.k = IK::Assign; h.sig = v; h.e = storageOf(*c.expr.type, c.expr); prog.emit(h);
                std::vector<size_t> toEnd;
                for (auto& it : c.items) {
                    std::vector<size_t> next;
                    Instr j; j.k = IK::JmpIfNot; j.e = matchP(*it.pattern, ref(v), *c.expr.type, w, c.condition);
                    next.push_back(prog.emit(j));
                    if (it.filter) {
                        Instr g; g.k = IK::JmpIfNot; g.e = expr(*it.filter);
                        next.push_back(prog.emit(g));
                    }
                    stmt(*it.stmt);
                    Instr skip; skip.k = IK::Jmp;
                    toEnd.push_back(prog.emit(skip));
                    for (size_t x : next) prog.code[x].target = prog.here();
                }
                if (c.defaultCase) stmt(*c.defaultCase);
                for (size_t x : toEnd) prog.code[x].target = prog.here();
                break;
            }
            case StatementKind::ExpressionStatement:
                // void'($cast(d, s)) is the function form (8.16): a failed cast is not an error.
                voidCast = s.syntax && s.syntax->kind == slang::syntax::SyntaxKind::VoidCastedCallStatement;
                xpExprStmt(s.as<ExpressionStatement>().expr);
                voidCast = false;
                break;
            case StatementKind::VariableDeclaration: {   // int x = 5;  inside a block
                const auto& vd = s.as<VariableDeclStatement>();
                const Expression* init = vd.symbol.getInitializer();
                if (localDyn().count(&vd.symbol)) {   // an automatic container starts empty at each entry
                    int d = dynFind(&vd.symbol);
                    prog.emit(dynCmd(d, D_DELETE));
                    if (init) assignContainer(d, *init);
                    break;
                }
                if (localArr().count(&vd.symbol)) {
                    if (init) { assignWholeArray(vd.symbol, vd.symbol.getType(), *init, false); break; }
                    auto a = arrFind(&vd.symbol);
                    const Type* et = &vd.symbol.getType().getCanonicalType();
                    while (et->isUnpackedArray()) et = &et->getArrayElementType()->getCanonicalType();
                    Val iv = initOf(*et);
                    for (int k = 0; k < a->count; k++) { Instr i; i.k = IK::Assign; i.sig = a->base + k; i.e = konst(iv); prog.emit(i); }
                    break;
                }
                // An array with an initializer in a function that keeps static
                // signals: assigned at each entry, as an automatic one is.
                if (init && vd.symbol.getType().getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType &&
                    !(current && vd.symbol.lifetime == VariableLifetime::Static) && arrFind(&vd.symbol) &&
                    assignWholeArray(vd.symbol, vd.symbol.getType(), *init, false))
                    break;
                if (localMap().count(&vd.symbol)) {   // an automatic variable starts over at each entry (6.21)
                    if (vd.symbol.getType().isEvent()) break;
                    Instr i; i.k = IK::Assign; i.sig = sigAt(&vd.symbol);
                    i.e = init ? expr(*init) : konst(initOf(vd.symbol.getType()));
                    prog.emit(i);
                    break;
                }
                if (auto it = dynMap().find(&vd.symbol); it != dynMap().end()) {
                    if (vd.symbol.lifetime == slang::ast::VariableLifetime::Static && current) {
                        // keeps its contents from call to call (6.21)
                        if (init) unsupported("static container with an initializer in a task or function");
                        break;
                    }
                    prog.emit(dynCmd(it->second, D_DELETE));   // a block entry starts it empty
                    if (init) assignContainer(it->second, *init);
                    break;
                }
                if (!init) {
                    // An automatic scalar starts over at its default on every
                    // entry (6.21), also in a function that keeps static signals.
                    const Type& vt = vd.symbol.getType().getCanonicalType();
                    if (vd.symbol.lifetime == VariableLifetime::Automatic && current && !vt.isUnpackedArray() &&
                        !vt.isEvent() && (vt.isIntegral() || vt.isFloating() || vt.isString())) {
                        Instr i; i.k = IK::Assign; i.sig = sigAt(&vd.symbol); i.e = konst(initOf(vt));
                        prog.emit(i);
                    }
                    break;
                }
                // A static local is initialized once (6.21), not on every
                // entry: on the first call of a task or function, or the
                // first pass of an always or of a loop body, then left alone.
                // The flag starts as X, so the first entry fails the "already
                // done" test and takes the init.
                bool once = vd.symbol.lifetime == slang::ast::VariableLifetime::Static;
                auto initVar = [&] {   // a fixed-size array takes its value element by element
                    if (arrFind(&vd.symbol) && assignWholeArray(vd.symbol, vd.symbol.getType(), *init, false)) return;
                    Instr i; i.k = IK::Assign; i.sig = sigAt(&vd.symbol); i.e = expr(*init);
                    prog.emit(i);
                };
                if (!once) {
                    initVar();
                    break;
                }
                int flag = freshStatic(1);
                Instr j; j.k = IK::JmpIfNot; j.e = ref(flag);
                size_t jf = prog.emit(j);
                size_t js = emitJmp();
                prog.code[jf].target = prog.here();
                Instr set; set.k = IK::Assign; set.sig = flag; set.e = konst(mk(1, 1));
                prog.emit(set);
                initVar();
                prog.code[js].target = prog.here();
                break;
            }
            case StatementKind::Return: {
                const auto& r = s.as<ReturnStatement>();
                if (r.expr && current && current->retDyn) assignContainer(retSig, *r.expr);
                else if (r.expr) {
                    if (retSig < 0) unsupported("return value");
                    Instr i;
                    i.k = IK::Assign;
                    i.sig = retSig;
                    i.e = r.expr->type->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType
                        ? packedValue(*r.expr->type, *r.expr) : expr(*r.expr);
                    prog.emit(i);
                }
                if (current && current->sym) blockEventSample(current->sym, false);   // @@(end f) (19.3)
                Instr ret;
                ret.k = IK::Ret;
                prog.emit(ret);
                break;
            }
            case StatementKind::RandCase: { randCase(s.as<RandCaseStatement>()); break; }
            case StatementKind::RandSequence: { randSequence(s.as<RandSequenceStatement>()); break; }
            case StatementKind::Break: {
                if (loops.empty() && rsBreak >= 0) {   // break in a randsequence code block: end the whole sequence (18.17.6)
                    Instr i; i.k = IK::Assign; i.sig = rsBreak; i.e = konst(mk(1, 1));
                    prog.emit(i);
                    Instr r; r.k = IK::Ret; prog.emit(r);
                    break;
                }
                if (loops.empty()) unsupported("break outside a loop");
                if (loops.back().xpBrk >= 0) { xpSet(loops.back().xpBrk, konst(mk(1, 1))); break; }   // -xprop
                loops.back().breaks.push_back(emitJmp());
                break;
            }
            case StatementKind::Continue: {
                if (loops.empty()) unsupported("continue outside a loop");
                if (loops.back().xpCnt >= 0) { xpSet(loops.back().xpCnt, konst(mk(1, 1))); break; }   // -xprop
                loops.back().continues.push_back(emitJmp());
                break;
            }
            case StatementKind::ForLoop: {
                const auto& f = s.as<ForLoopStatement>();
                for (const VariableSymbol* v : f.loopVars)
                    if (v->lifetime == VariableLifetime::Automatic && !localMap().count(v))
                        if (int sg = sigFind(v); sg >= 0) loopVarSigs().push_back(sg);
                for (const VariableSymbol* v : f.loopVars)
                    if (const Expression* init = v->getInitializer()) {
                        Instr i; i.k = IK::Assign; i.sig = sigAt(v); i.e = expr(*init);
                        prog.emit(i);
                    }
                for (const Expression* e : f.initializers) exprStmt(*e);
                size_t top = prog.here(), jf = (size_t)-1;
                if (f.stopExpr) { Instr j; j.k = IK::JmpIfNot; j.e = expr(*f.stopExpr); jf = prog.emit(j); }
                loops.push_back({});
                if (f.stopExpr && xpLoopOk(f.body)) {   // -xprop: break / continue become flags
                    size_t jb = xpLoopBody(f.body);
                    patch(loops.back().continues, prog.here());
                    for (const Expression* e : f.steps) exprStmt(*e);
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    prog.code[jf].target = prog.here();
                    prog.code[jb].target = prog.here();
                    xpLoopEnd();
                    loops.pop_back();
                    break;
                }
                stmt(f.body);
                // continue lands on the step. The step then falls into the
                // back-edge, which checks the condition again.
                patch(loops.back().continues, prog.here());
                for (const Expression* e : f.steps) exprStmt(*e);
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                if (jf != (size_t)-1) prog.code[jf].target = prog.here();
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::ForeachLoop: {   // foreach (a[i, j]) (12.7.3)
                // One counting loop per named dimension, outermost first, each
                // from its left bound to its right bound. break leaves the whole
                // foreach; continue steps the innermost loop.
                const auto& f = s.as<ForeachLoopStatement>();
                if (int d = dynSig(f.arrayRef); d >= 0) { foreachDyn(f, d); break; }
                if (f.arrayRef.type->isString()) {   // foreach (s[i]): 0 .. s.len()-1
                    const ValueSymbol* lvar = nullptr;
                    for (const auto& d : f.loopDims) if (d.loopVar) lvar = d.loopVar;
                    if (!lvar) { break; }
                    int v = fresh(32);
                    bindVar(lvar, v);
                    Instr init; init.k = IK::Assign; init.sig = v; init.e = konst(make(0, 0, 32, 0, true));
                    prog.emit(init);
                    size_t top = prog.here();
                    auto len = node(Op::StrMethod, {exprStr(f.arrayRef)});
                    len->hi = S_LEN;
                    Instr j; j.k = IK::JmpIfNot;
                    j.e = bin(Op::Lt, ref(v, true), resize(len, 32, false, true));
                    size_t jf = prog.emit(j);
                    loops.push_back({});
                    stmt(f.body);
                    patch(loops.back().continues, prog.here());
                    Instr inc; inc.k = IK::Assign; inc.sig = v; inc.e = bin(Op::Add, ref(v, true), konst(make(1, 0, 32, 0, true)));
                    prog.emit(inc);
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    prog.code[jf].target = prog.here();
                    patch(loops.back().breaks, prog.here());
                    loops.pop_back();
                    break;
                }
                struct Lvl { int var; int64_t right; bool up; size_t top, jf; };
                std::vector<Lvl> lv;
                // A fixed-size array of containers (int f[2][]): its fixed
                // dimensions here, then each element container's own.
                std::shared_ptr<const ArrShape> da = contArr(f.arrayRef);
                size_t nfixed = da ? da->dims.size() : f.loopDims.size();
                for (size_t dk = 0; dk < nfixed && dk < f.loopDims.size(); dk++) {
                    const auto& d = f.loopDims[dk];
                    if (!d.loopVar && !(da && f.loopDims.size() > nfixed)) continue;
                    if (!d.range) unsupported("foreach over a dynamic dimension");
                    int v = xpLoopTemp(&d, 32);
                    if (d.loopVar) bindVar(d.loopVar, v);
                    Instr init; init.k = IK::Assign; init.sig = v;
                    init.e = konst(make((uint64_t)d.range->left, 0, 32, 0, true));
                    prog.emit(init);
                    Lvl L{v, d.range->right, d.range->left <= d.range->right, prog.here(), 0};
                    Instr j; j.k = IK::JmpIfNot;
                    j.e = bin(L.up ? Op::Le : Op::Ge, ref(v, true), konst(make((uint64_t)L.right, 0, 32, 0, true)));
                    L.jf = prog.emit(j);
                    lv.push_back(L);
                }
                loops.push_back({});
                if (da && f.loopDims.size() > nfixed) {
                    std::vector<E> ks;
                    for (auto& L : lv) ks.push_back(ref(L.var, true));
                    auto n = node(Op::ArrAddr, std::move(ks));
                    n->arr = da;
                    foreachDynDim(f, bindCont(std::move(n), *da->dynElem), nfixed);
                } else if (xpLoopOk(f.body)) {   // -xprop: break / continue become flags
                    size_t jb = xpLoopBody(f.body);
                    patch(loops.back().continues, prog.here());
                    for (size_t k = lv.size(); k-- > 0;) {
                        const Lvl& L = lv[k];
                        Instr st; st.k = IK::Assign; st.sig = L.var;
                        st.e = bin(L.up ? Op::Add : Op::Sub, ref(L.var, true), konst(make(1, 0, 32, 0, true)));
                        prog.emit(st);
                        Instr back; back.k = IK::Jmp; back.target = L.top; prog.emit(back);
                        prog.code[L.jf].target = prog.here();
                    }
                    prog.code[jb].target = prog.here();
                    xpLoopEnd();
                    loops.pop_back();
                    break;
                } else stmt(f.body);
                patch(loops.back().continues, prog.here());
                for (size_t k = lv.size(); k-- > 0;) {
                    const Lvl& L = lv[k];
                    Instr st; st.k = IK::Assign; st.sig = L.var;
                    st.e = bin(L.up ? Op::Add : Op::Sub, ref(L.var, true), konst(make(1, 0, 32, 0, true)));
                    prog.emit(st);
                    Instr back; back.k = IK::Jmp; back.target = L.top; prog.emit(back);
                    // The inner loop's init sits after the outer test, so each
                    // outer pass starts the inner loop at its left bound again.
                    prog.code[L.jf].target = prog.here();
                }
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::WhileLoop: {
                const auto& w = s.as<WhileLoopStatement>();
                if (xpCondLoopOk(w.body)) {   // -xprop: break / continue become flags
                    loops.push_back({});
                    xpLoopStart();
                    size_t top = prog.here();
                    size_t jp = xpLoopGuard(&w.cond);
                    Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                    size_t jf = prog.emit(j);
                    size_t a = prog.here();
                    size_t jb = xpLoopBody(w.body);
                    size_t b = prog.here();
                    patch(loops.back().continues, prog.here());
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    xpLoopPoison(a, b, {jp});
                    prog.code[jf].target = prog.here();
                    prog.code[jb].target = prog.here();
                    xpLoopEnd();
                    loops.pop_back();
                    break;
                }
                size_t top = prog.here();
                Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                size_t jf = prog.emit(j);
                loops.push_back({});
                stmt(w.body);
                patch(loops.back().continues, prog.here());
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                prog.code[jf].target = prog.here();
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::DoWhileLoop: {
                const auto& w = s.as<DoWhileLoopStatement>();
                if (xpCondLoopOk(w.body)) {   // -xprop: break / continue become flags
                    loops.push_back({});
                    xpLoopStart();
                    size_t top = prog.here();
                    size_t a = prog.here();
                    size_t jb = xpLoopBody(w.body);
                    size_t b = prog.here();
                    patch(loops.back().continues, prog.here());
                    size_t jp = xpLoopGuard(&w.cond);
                    Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                    size_t jf = prog.emit(j);
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    xpLoopPoison(a, b, {jp});
                    prog.code[jf].target = prog.here();
                    prog.code[jb].target = prog.here();
                    xpLoopEnd();
                    loops.pop_back();
                    break;
                }
                size_t top = prog.here();
                loops.push_back({});
                stmt(w.body);
                // continue tests the condition before another trip through the body.
                patch(loops.back().continues, prog.here());
                Instr j; j.k = IK::JmpIfNot; j.e = expr(w.cond);
                size_t jf = prog.emit(j);
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                prog.code[jf].target = prog.here();
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::ForeverLoop: {
                if (const auto& body = s.as<ForeverLoopStatement>().body; xpCondLoopOk(body)) {   // -xprop
                    loops.push_back({});
                    xpLoopStart();
                    size_t top = prog.here();
                    size_t jp = xpLoopGuard(nullptr);
                    size_t a = prog.here();
                    size_t jb = xpLoopBody(body);
                    size_t b = prog.here();
                    patch(loops.back().continues, prog.here());
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    xpLoopPoison(a, b, {jp});
                    prog.code[jb].target = prog.here();
                    xpLoopEnd();
                    loops.pop_back();
                    break;
                }
                size_t top = prog.here();
                loops.push_back({});
                stmt(s.as<ForeverLoopStatement>().body);
                patch(loops.back().continues, prog.here());
                Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::RepeatLoop: {   // a small constant count is unrolled
                const auto& r = s.as<RepeatLoopStatement>();
                const auto* cv = r.count.getConstant();
                bool small = (r.count.kind == ExpressionKind::IntegerLiteral || (cv && cv->isInteger())) &&
                             constInt(r.count) <= 1024;
                // -xprop: break / continue become flags, in the counter form; the
                // clean copy uses the same form and counter (xpLoopTemp).
                bool xpr = xpLoopOk(r.body), xpc = xp.on && xpEscapes(r.body);
                if (!small || xpc) {   // a counter (12.7.2): x, z or a negative count runs the body zero times
                    int c = xpLoopTemp(&r, 64);
                    Instr init; init.k = IK::Assign; init.sig = c;
                    init.e = resize(expr(r.count), 64, r.count.type->isSigned(), true);
                    prog.emit(init);
                    size_t top = prog.here();
                    Val zero = make(0, 0, 64); zero.sgn = true;
                    Val one = make(1, 0, 64); one.sgn = true;
                    Instr j; j.k = IK::JmpIfNot; j.e = bin(Op::Gt, ref(c, true), konst(zero));
                    size_t jf = prog.emit(j);
                    loops.push_back({});
                    size_t jb = (size_t)-1;
                    if (xpr) jb = xpLoopBody(r.body);
                    else stmt(r.body);
                    patch(loops.back().continues, prog.here());
                    Instr dec; dec.k = IK::Assign; dec.sig = c; dec.e = bin(Op::Sub, ref(c, true), konst(one));
                    prog.emit(dec);
                    Instr back; back.k = IK::Jmp; back.target = top; prog.emit(back);
                    prog.code[jf].target = prog.here();
                    if (xpr) { prog.code[jb].target = prog.here(); xpLoopEnd(); }
                    patch(loops.back().breaks, prog.here());
                    loops.pop_back();
                    break;
                }
                int64_t n = constInt(r.count);
                loops.push_back({});
                for (int64_t k = 0; k < n; k++) {
                    stmt(r.body);
                    // The next copy is the next iteration. After the last copy,
                    // continue leaves the loop.
                    patch(loops.back().continues, prog.here());
                }
                patch(loops.back().breaks, prog.here());
                loops.pop_back();
                break;
            }
            case StatementKind::Case: {
                const auto& c = s.as<CaseStatement>();
                if (xp.on && xpCase(s, c)) break;   // -xprop
                Op cmp = c.condition == CaseStatementCondition::Normal        ? Op::CaseEq
                         : c.condition == CaseStatementCondition::WildcardJustZ ? Op::CaseZEq
                                                                                : Op::CaseXEq;
                std::vector<E> conds;
                // A selector without side effects is lowered once and shared by
                // every compare, so the interpreter can find the item through a
                // table (Instr::caseId) instead of comparing label by label.
                E sel1 = c.condition == CaseStatementCondition::Normal ? expr(c.expr) : nullptr;
                if (sel1 && !pureSel(sel1)) sel1 = nullptr;
                for (const auto& item : c.items) {
                    E cond;   // OR of (selector === item_expr) over this item's labels
                    for (const Expression* x : item.expressions) {
                        E m = c.condition == CaseStatementCondition::Inside ? insideItem(c.expr, *x)
                                                                             : bin(cmp, sel1 ? sel1 : expr(c.expr), expr(*x));
                        cond = cond ? bin(Op::LogOr, cond, m) : m;
                    }
                    conds.push_back(cond);
                }
                // unique / unique0 (12.5.3): after item k matches, a later
                // item that also matches is a violation. Constant labels that
                // cannot overlap need no check at run time.
                bool uniq = c.check == UniquePriorityCheck::Unique || c.check == UniquePriorityCheck::Unique0;
                bool needAll = c.check == UniquePriorityCheck::Unique || c.check == UniquePriorityCheck::Priority;
                int ov = uniq ? caseMayOverlap(c) : 0;
                if (ov == 2 && sel1) {   // labels slang did not fold, such as enum names: compare the lowered constants
                    E sel;
                    std::vector<uint64_t> keys;
                    bool known = true;
                    for (const E& cd : conds) known = known && caseLabels(cd, sel, keys);
                    std::sort(keys.begin(), keys.end());
                    if (known && std::adjacent_find(keys.begin(), keys.end()) == keys.end()) ov = 0;
                }
                bool mayOverlap = ov > 0;
                E selUnknown;   // casez / casex labels that never overlap can still
                                // both match a selector with z (or x) bits
                if (ov == 1)
                    selUnknown = bin(Op::Ne, sysFnNode(SF_COUNTBITS, {expr(c.expr)}, 12), konst(make(0, 0, 32, 0, true)));
                std::vector<size_t> toEnd;
                if (mayOverlap && c.items.size() > 8) {   // many items: a check of linear size
                    uniqueCaseLinear(s, c, conds, selUnknown);
                    break;
                }
                bool table = sel1 && !mayOverlap && c.items.size() >= 4 && !g_cov;
                if (table) caseIds++;
                for (size_t k = 0; k < c.items.size(); k++) {
                    Instr j; j.k = IK::JmpIfNot; j.e = conds[k];
                    if (table) { j.caseId = caseIds; j.caseItem = (int)k + 1; }
                    size_t jf = prog.emit(j);
                    if (mayOverlap && k + 1 < c.items.size()) {
                        E more;
                        for (size_t m = k + 1; m < c.items.size(); m++) more = more ? bin(Op::LogOr, more, conds[m]) : conds[m];
                        if (selUnknown) more = bin(Op::LogAnd, selUnknown, more);
                        Instr o; o.k = IK::JmpIfNot; o.e = more;
                        size_t jo = prog.emit(o);
                        violation(s, checkName(c.check) + " case: more than one case item matches", c.check);
                        prog.code[jo].target = prog.here();
                    }
                    stmt(*c.items[k].stmt);
                    Instr skip; skip.k = IK::Jmp;
                    toEnd.push_back(prog.emit(skip));
                    prog.code[jf].target = prog.here();
                }
                if (c.defaultCase) stmt(*c.defaultCase);
                else if (needAll) violation(s, checkName(c.check) + " case: no case item matches", c.check);
                for (size_t k : toEnd) prog.code[k].target = prog.here();
                break;
            }
            case StatementKind::WaitOrder: {   // wait_order (a, b, c) pass; else fail (15.5.4)
                // For each event in turn, wait for it or any later one. A
                // later one triggering first is out of order and fails.
                const auto& w = s.as<WaitOrderStatement>();
                std::vector<int> ev;
                for (auto* x : w.events) ev.push_back(eventSig(*x));
                std::vector<size_t> toFail;
                for (size_t k = 0; k < ev.size(); k++) {
                    std::vector<int> snap;
                    std::vector<std::pair<int, Edge>> on;
                    for (size_t j = k; j < ev.size(); j++) {
                        int sn = fresh(32);
                        Instr c; c.k = IK::Assign; c.sig = sn; c.e = ref(ev[j]); prog.emit(c);
                        snap.push_back(sn);
                        on.push_back({ev[j], ANY});
                    }
                    size_t top = prog.here();
                    prog.emit(waitInstr(on));
                    for (size_t j = k + 1; j < ev.size(); j++) {
                        Instr f; f.k = IK::JmpIfNot; f.e = bin(Op::CaseEq, ref(ev[j]), ref(snap[j - k]));
                        toFail.push_back(prog.emit(f));
                    }
                    Instr again; again.k = IK::JmpIfNot; again.e = un(Op::LogNot, bin(Op::CaseEq, ref(ev[k]), ref(snap[0])));
                    again.target = top;
                    prog.emit(again);
                }
                if (w.ifTrue) stmt(*w.ifTrue);
                Instr skip; skip.k = IK::Jmp;
                size_t js = prog.emit(skip);
                for (size_t f : toFail) prog.code[f].target = prog.here();
                if (w.ifFalse) stmt(*w.ifFalse);
                else {   // no else: the failure is an error (15.5.4)
                    std::string file = "?";
                    size_t line = 0;
                    if (const auto* sm = srcMgr()) {
                        auto loc = sm->getFullyOriginalLoc(s.sourceRange.start());
                        file = std::string(sm->getFileName(loc));
                        line = sm->getLineNumber(loc);
                    }
                    Instr i; i.k = IK::Display;
                    i.fmt = "Error: \"" + file + "\", " + std::to_string(line) +
                            ": wait_order: events triggered out of order at time %0t";
                    E t = node(Op::Time); t->k = mk(1, 64);
                    i.args.push_back(t);
                    prog.emit(i);
                }
                prog.code[js].target = prog.here();
                break;
            }
            case StatementKind::EventTrigger: {   // -> e, ->> e (15.5.1)
                const auto& t = s.as<EventTriggerStatement>();
                int sig = eventSig(t.target);
                Instr cnt; cnt.k = t.isNonBlocking ? IK::Nba : IK::Assign;
                cnt.sig = sig;
                cnt.e = bin(Op::Add, ref(sig), konst(mk(1, 32)));
                Instr at = cnt;
                at.sig = sig + 1;
                E now = node(Op::Time);
                now->k = mk(1, 64);
                at.e = bin(Op::Add, now, konst(mk(1, 64)));
                if (t.timing) {
                    if (!t.isNonBlocking || t.timing->kind != TimingControlKind::Delay)
                        unsupported("event trigger with an event control");
                    Instr d;
                    delayInto(t.timing->as<DelayControl>().expr, d);
                    for (Instr* x : {&cnt, &at}) { x->delay = d.delay; x->de = d.de; x->nbaLater = true; }
                    // The trigger time is when it fires: now + d.
                    E fire = node(Op::Time); fire->k = mk(1, 64);
                    at.e = bin(Op::Add, bin(Op::Add, fire, d.de ? d.de : konst(mk(d.delay, 64))), konst(mk(1, 64)));
                }
                prog.emit(at);
                prog.emit(cnt);
                break;
            }
            case StatementKind::ProceduralAssign:     // assign / force (10.6)
            case StatementKind::ProceduralDeassign: { // deassign / release
                bool assign = s.kind == StatementKind::ProceduralAssign;
                const Expression* tgt;
                const Expression* val = nullptr;
                bool force;
                if (assign) {
                    const auto& pa = s.as<ProceduralAssignStatement>();
                    force = pa.isForce;
                    const Expression& a = pa.assignment;
                    if (a.kind != ExpressionKind::Assignment) unsupported("force / assign");
                    tgt = &a.as<AssignmentExpression>().left();
                    val = &a.as<AssignmentExpression>().right();
                } else {
                    const auto& pd = s.as<ProceduralDeassignStatement>();
                    force = pd.isRelease;
                    tgt = &pd.lvalue;
                }
                // force {a, w[3]} = v: each part takes its slice of v (10.6.2).
                std::vector<std::tuple<const Expression*, int, int>> parts;
                int tw = (int)tgt->type->getBitWidth();
                if (peel(*tgt).kind == ExpressionKind::Concatenation) concatParts(*tgt, tw - 1, parts);
                else parts.push_back({tgt, -1, -1});
                E whole;
                if (val) {
                    whole = expr(*val);
                    if (parts.size() > 1) {   // evaluated as one value, sliced per part
                        needWidth(tw);
                        whole = resize(whole, tw, false, false);
                    }
                }
                for (auto& [x, phi, plo] : parts) {
                    Instr i;
                    lhs(*x, i);
                    if (i.arr || i.bidx || i.dyn >= 0 || i.drv >= 0)
                        unsupported("force / assign of a variable bit, part, or array element");
                    if (i.hi >= 0 && !force) unsupported("assign / deassign of a bit or part (only force can, 10.6.2)");
                    // A constant bit or part: pw is its width and dkey its low bit.
                    i.pw = i.hi >= 0 ? i.hi - i.lo + 1 : 0;
                    i.dkey = i.hi >= 0 ? i.lo : -1;
                    i.k = assign ? IK::Force : IK::Release;
                    i.hi = force ? 1 : 0;
                    i.lo = 0;
                    if (whole) i.e = phi >= 0 ? slice(whole, phi, plo) : whole;
                    usesForce() = true;
                    prog.emit(i);
                }
                break;
            }
            case StatementKind::Disable: {   // disable blk (9.6.2)
                const auto& d = s.as<DisableStatement>();
                if (d.target.kind != ExpressionKind::ArbitrarySymbol) unsupported("disable target");
                const Symbol* sym = d.target.as<ArbitrarySymbolExpression>().symbol;
                if (sym->kind != SymbolKind::StatementBlock && sym->kind != SymbolKind::Subroutine) unsupported("disable target");
                // Inside the block, in a process's own code, with nothing
                // forked in it: no other process can be inside it, so a jump
                // past it is the whole disable.
                for (size_t k = named.size(); k-- > 0;)
                    if (named[k].first == sym && !current && !forkyBlocks.count(sym)) { named[k].second.push_back(emitJmp()); return; }
                // A block or task some process is running: a process it is
                // the whole of ends; any other process inside it resumes
                // after it, this one included (a task disabling itself).
                Instr i; i.k = IK::Disable;
                i.sig = blockIds().of(sym);
                prog.emit(i);
                break;
            }
            case StatementKind::WaitFork: { Instr i; i.k = IK::WaitFork; prog.emit(i); break; }
            case StatementKind::ProceduralChecker:   // 17.3: its static assertions, queued here
                for (const Symbol* inst : s.as<ProceduralCheckerStatement>().instances)
                    if (inst->kind == SymbolKind::CheckerInstance) procCheckerAsserts(inst->as<CheckerInstanceSymbol>().body);
                break;
            case StatementKind::ImmediateAssertion:
                xpGuard([&] { immAssert(s.as<ImmediateAssertionStatement>()); });
                break;
            case StatementKind::ConcurrentAssertion:
{
                NoCondCov off(condCovOff);
                concAssert(s.as<ConcurrentAssertionStatement>());
                break;
            }
            case StatementKind::DisableFork: { Instr i; i.k = IK::DisableFork; prog.emit(i); break; }
            default:
                throw std::runtime_error("lower: unsupported statement kind " + std::string(toString(s.kind)));
        }
    }
};

// Skip bodies slang built only for name lookup. Arrays and interfaces are
// later limits, so they stay a hard error instead of a silent wrong netlist.
// The default value of an unpacked struct or union, as one vector (7.2).
inline Val structDefaultVal(const slang::ast::Type& t0) {
    using namespace slang::ast;
    const auto& t = t0.getCanonicalType();
    int W = uWidth(t);
    Val r = make(0, 0, W);
    if (t.isUnpackedUnion()) {
        for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
            const Type& ft = f.getType().getCanonicalType();
            if (ft.isIntegral() && ft.isFourState()) r = xfill(W);
            break;
        }
        return r;
    }
    if (t.kind == SymbolKind::FixedSizeUnpackedArrayType) {   // each element's default, left element highest
        const Type& et = t.as<FixedSizeUnpackedArrayType>().elementType.getCanonicalType();
        int ew = uWidth(et);
        Val p = et.isUnpackedStruct() || et.isUnpackedUnion() || et.kind == SymbolKind::FixedSizeUnpackedArrayType ? structDefaultVal(et)
              : et.isIntegral() && et.isFourState() ? xfill(ew) : make(0, 0, ew);
        for (int top = W - ew; top >= 0; top -= ew) copyBits(r, top, p, 0, ew);
        return finish(r);
    }
    int top = W;
    for (const auto& f : t.as<Scope>().membersOfType<FieldSymbol>()) {
        const Type& ft = f.getType().getCanonicalType();
        int w = uWidth(ft);
        top -= w;
        Val p;
        if (ft.isUnpackedStruct() || ft.isUnpackedUnion() || ft.kind == SymbolKind::FixedSizeUnpackedArrayType) p = structDefaultVal(ft);
        else if (ft.isIntegral() && ft.isFourState()) p = xfill(w);
        else p = make(0, 0, w);
        copyBits(r, top, p, 0, w);
    }
    return finish(r);
}
// The empty shape of a container type (7.5, 7.8, 7.10).
inline DynObj Lowerer::dynShapeOf(const slang::ast::Type& t0) {
    using namespace slang::ast;
    const Type& t = t0.getCanonicalType();
    DynObj o;
    const Type* et = nullptr;
    if (t.isQueue()) {
        o.kind = 1;
        const auto& q = t.as<QueueType>();
        et = &q.elementType;
        if (q.maxBound) o.bound = (int64_t)q.maxBound;
    } else if (t.isAssociativeArray()) {
        o.kind = 2;
        const auto& a = t.as<AssociativeArrayType>();
        et = &a.elementType;
        if (a.indexType) {
            const Type& kt = a.indexType->getCanonicalType();
            if (kt.isString()) o.strKey = true;
            else if (kt.isIntegral() && kt.getBitWidth() <= 64) {
                o.keyW = (int)kt.getBitWidth();
                o.keySgn = kt.isSigned();
            } else if (kt.isIntegral()) { o.wideKey = true; o.keyW = (int)kt.getBitWidth(); o.keySgn = false; } else if (isHandleType(kt)) { o.keyW = 64; o.keySgn = false; }   // class key: by handle
            else throw std::runtime_error("lower: unsupported associative array index type " + kt.toString());
        } else { o.keyW = 64; o.keySgn = false; }   // [*]: integral keys, unsigned order
    } else {
        et = &t.as<DynamicArrayType>().elementType;
    }
    elemInto(o, *et);
    return o;
}
inline void Lowerer::elemInto(DynObj& o, const slang::ast::Type& et) {
    const auto& e = et.getCanonicalType();
    if (e.isUntypedType()) { o.dflt = Val{}; return; }   // a typeless mailbox (15.4.9): values as they are
    if (isDynType(e)) {   // a container of containers (7.5, 7.10): each element names a container
        o.dflt = make(0, 0, 64);
        o.elem = std::make_shared<DynObj>(dynShapeOf(e));
        return;
    }
    if (e.isFloating()) o.dflt = mkreal(0);
    else if (e.isString()) o.dflt = mkstr("");
    else if (isHandleType(e)) o.dflt = make(0, 0, 64);   // a class handle starts null
    else if (e.isIntegral()) {
        int w = (int)e.getBitWidth();
        o.dflt = e.isFourState() ? allx(w) : make(0, 0, w);
        o.dflt.sgn = e.isSigned();
    } else if ((e.isUnpackedStruct() || e.isUnpackedUnion() || e.kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType) &&
               uWidth(e) > 0) {
        o.dflt = structDefaultVal(e);   // members' (elements') defaults, packed as in a struct
    } else unsupported("container element type");
}

inline void Lowerer::declLocals(const slang::ast::Statement& st, FrameLayout* L) {
    using namespace slang::ast;
    struct W : ASTVisitor<W, VisitFlags::Statements> {
        Lowerer& lw;
        FrameLayout* L;
        W(Lowerer& l, FrameLayout* f) : lw(l), L(f) {}
        void handle(const BlockStatement& b) {
            if (b.blockKind == StatementBlockKind::Sequential) visitDefault(b);
            // fork: its declarations and branches get their own frames
        }
        void handle(const VariableDeclStatement& v) {
            if (v.symbol.lifetime == VariableLifetime::Automatic) lw.declLocal(v.symbol, L);
        }
        void handle(const ForLoopStatement& f) {
            for (const VariableSymbol* v : f.loopVars)
                if (v->lifetime == VariableLifetime::Automatic) lw.declLocal(*v, L);
            visitDefault(f);
        }
    } w(*this, L);
    st.visit(w);
}

// An instance's name; an element of an instance array (23.3.3) is named with
// its indices, "u[2]" or "m[1][0]", like its hierarchical path.
inline std::string instName(const slang::ast::InstanceSymbol& inst) {
    if (inst.arrayPath.empty()) return std::string(inst.name);
    std::string_view name;
    for (const slang::ast::Scope* sc = inst.getParentScope();
         sc && sc->asSymbol().kind == slang::ast::SymbolKind::InstanceArray; sc = sc->asSymbol().getParentScope())
        name = sc->asSymbol().name;
    slang::SmallVector<slang::ConstantRange, 4> dims;
    inst.getArrayDimensions(dims);
    std::string r(name);
    for (size_t d = 0; d < dims.size() && d < inst.arrayPath.size(); d++)
        r += "[" + std::to_string((int64_t)inst.arrayPath[d] + dims[d].lower()) + "]";
    return r;
}
// Where an element sits in its instance array, counted from the left-most
// index of each dimension, row-major (23.3.3.5), and how many there are.
inline std::pair<int, int> instArrayPos(const slang::ast::InstanceSymbol& inst) {
    slang::SmallVector<slang::ConstantRange, 4> dims;
    inst.getArrayDimensions(dims);
    int pos = 0, n = 1;
    for (size_t d = 0; d < dims.size() && d < inst.arrayPath.size(); d++) {
        int idx = (int)inst.arrayPath[d] + dims[d].lower();
        int p = dims[d].isDescending() ? dims[d].left - idx : idx - dims[d].left;
        pos = pos * (int)dims[d].width() + p;
        n *= (int)dims[d].width();
    }
    return {pos, n};
}

inline bool enterInstance(const slang::ast::InstanceSymbol& inst) {
    using namespace slang::ast;
    if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return false;
    // An interface instance (25.3) is lowered like a module instance: its
    // signals are named "bus.data", and its processes and tasks run as usual.
    // A program (24.3) is lowered like a module; its processes are marked
    // and run in the reactive region set.
    if (!inst.isTopLevel() && !inst.isModule() && !inst.isInterface() &&
        inst.getDefinition().definitionKind != slang::ast::DefinitionKind::Program)
        Lowerer::unsupported("non-module instance");
    return true;
}

// A ref port (23.3.3.3) is another name for the variable it connects to, so
// a write on either side is seen at once on the other. A dynamic array, queue
// or associative array port (7.5-7.9) is joined the same way: the child sees
// the parent's container itself. Returns the internal variable when the
// port's actual is a whole variable that the two can share.
inline const slang::ast::Symbol* sharedPortVar(const slang::ast::PortSymbol& ps, const slang::ast::Symbol& actual) {
    using namespace slang::ast;
    const Symbol* in = ps.internalSymbol;
    if (!in || actual.kind != SymbolKind::Variable) return nullptr;
    // slang makes an input port of container type a net; it holds a container all the same.
    bool dyn = Lowerer::isDynType(ps.getType());
    if (in->kind != SymbolKind::Variable && !(dyn && in->kind == SymbolKind::Net)) return nullptr;
    if (ps.direction != ArgumentDirection::Ref && !dyn) return nullptr;
    if (!in->as<ValueSymbol>().getType().isMatching(actual.as<VariableSymbol>().getType())) return nullptr;
    return in;
}

// Ports whose internal and external connections are both nets of one
// user-defined nettype are merged into a single simulated net (23.3.3.7.2),
// whatever the direction: its resolution function sees the drivers of both
// sides. Returns the internal net when they can be merged.
inline const slang::ast::Symbol* nettypeCollapses(const slang::ast::PortSymbol& ps, const slang::ast::Symbol& actual) {
    using namespace slang::ast;
    const Symbol* in = ps.internalSymbol;
    if (!in || in->kind != SymbolKind::Net || actual.kind != SymbolKind::Net) return nullptr;
    const NetType& a = in->as<NetSymbol>().netType;
    const NetType& b = actual.as<NetSymbol>().netType;
    if (a.netKind != NetType::UserDefined || b.netKind != NetType::UserDefined) return nullptr;
    if (&a != &b && (a.getResolutionFunction() != b.getResolutionFunction() ||
                     !a.getDataType().isMatching(b.getDataType())))
        return nullptr;
    return in;
}

// An inout port whose actual is a whole net of the same width and kind is
// the same net as its internal net (23.3.3.1). Returns the internal net when
// the two can share one signal.
inline const slang::ast::Symbol* inoutCollapses(const slang::ast::PortSymbol& ps, const slang::ast::Symbol& actual) {
    using namespace slang::ast;
    if (ps.direction != ArgumentDirection::InOut || !ps.internalSymbol) return nullptr;
    if (ps.internalSymbol->kind != SymbolKind::Net || actual.kind != SymbolKind::Net) return nullptr;
    const auto& in = ps.internalSymbol->as<NetSymbol>();
    const auto& out = actual.as<NetSymbol>();
    auto wireish = [](const NetSymbol& n) { return n.netType.netKind == NetType::Wire || n.netType.netKind == NetType::Tri; };
    auto simple = [](const NetSymbol& n) {
        auto k = n.netType.netKind;
        return n.getType().isIntegral() && !n.getDelay() && k != NetType::UserDefined && k != NetType::Interconnect &&
               k != NetType::TriReg && k != NetType::Unknown;
    };
    if (!simple(in) || !simple(out) || in.getType().getBitWidth() != out.getType().getBitWidth()) return nullptr;
    // One net, of the kind that wins (23.3.3.7): a wire or tri side takes the
    // other side's kind (an inout tri port on a tri1 bus is a tri1 net).
    if (!wireish(in) && !wireish(out) && in.netType.netKind != out.netType.netKind) return nullptr;
    return &in;
}

// Pass 1: register every variable / net as a simulator signal.
// Nested signals are named from the instance path (`u.y`) so two children
// with the same local name stay distinct. The top module keeps bare names.
struct SigCollector : slang::ast::ASTVisitor<SigCollector> {
    MkSig mkSig;
    std::unordered_map<const slang::ast::Symbol*, int>& sigmap;
    std::string prefix;
    SigCollector(MkSig f, std::unordered_map<const slang::ast::Symbol*, int>& m)
        : mkSig(std::move(f)), sigmap(m) {}
    std::vector<const slang::ast::Symbol*> aliased;   // nets that share another net's signal
    template <typename T> void reg(const T& s, bool net) {
        if (auto a = aliasOf().find(&s); a != aliasOf().end() && a->second != &s) { aliased.push_back(&s); return; }
        if (Lowerer::isDynType(s.getType())) { regDyn(s); return; }
        if (s.getType().isUnpackedArray()) {
            const slang::ast::Type* t = &s.getType().getCanonicalType();
            while (t->kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType) t = &t->getArrayElementType()->getCanonicalType();
            if (Lowerer::isDynType(*t)) { regDynArray(s, *t); return; }
            regArray(s, net);
            return;
        }
        if (s.getType().isEvent()) {
            // A named event (15.5) is two signals: a trigger count, which an
            // @e waits on, and the time of the last trigger + 1, which .triggered reads.
            int id = mkSig(prefix + std::string(s.name), 32, false);
            if (mkSig(prefix + std::string(s.name) + "$trig", 64, false) != id + 1)
                Lowerer::unsupported("event signals that are not consecutive");
            sigmap[&s] = id;
            noteHidden(s, id, 2);
            if (assignedEvents().count(&s)) {
                int ptr = mkSig(prefix + std::string(s.name) + "$ptr", 64, false);
                eventPtr()[&s] = ptr;
                noteHidden(s, ptr, 1);
            }
            return;
        }
        int w = storeW(s.getType());
        if (w <= 0)
            if (auto it = interconnectW().find(&s); it != interconnectW().end()) w = it->second;
        if (w <= 0) unsupported_width(s.name);
        sigmap[&s] = mkSig(prefix + std::string(s.name), w, net);
        noteHidden(s, sigmap[&s], 1);
    }
    // A dynamic array, queue, or associative array: a hidden handle signal
    // (its name has '$') and its store entry.
    template <typename T> void regDynArray(const T& s, const slang::ast::Type& et) {
        using namespace slang::ast;
        auto shape = std::make_shared<ArrShape>();
        int64_t total = 1;
        for (const Type* t = &s.getType().getCanonicalType(); t->kind == SymbolKind::FixedSizeUnpackedArrayType;
             t = &t->getArrayElementType()->getCanonicalType()) {
            const auto& ft = t->as<FixedSizeUnpackedArrayType>();
            shape->dims.push_back({ft.range.left, ft.range.right});
            total *= (int64_t)ft.range.width();
        }
        if (total > 4096) throw std::runtime_error("lower: unsupported array of more than 4096 containers: " + std::string(s.name));
        DynObj o = Lowerer::dynShapeOf(et);
        for (int64_t k = 0; k < total; k++) {
            int id = mkSig(prefix + std::string(s.name) + "[" + std::to_string(k) + "]$", 64, false);
            if (k == 0) shape->base = id;
            else if (id != shape->base + (int)k) throw std::runtime_error("lower: array of containers not laid out in a row: " + std::string(s.name));
            dynStore()[id] = o;
        }
        shape->count = (int)total;
        shape->elemW = 64;
        shape->dynElem = std::make_shared<DynObj>(o);
        dynArrMap()[&s] = shape;
    }
    template <typename T> void regDyn(const T& s) {
        using namespace slang::ast;
        DynObj o = Lowerer::dynShapeOf(s.getType());
        int id = mkSig(prefix + std::string(s.name) + "$", 64, false);
        dynStore()[id] = o;
        dynMap()[&s] = id;
    }
    // One signal per element, named with its indices ("m[3]", "g[1][0]"),
    // in row-major order with each dimension running left to right.
    template <typename T> void regArray(const T& s, bool net) {
        using namespace slang::ast;
        auto shape = std::make_shared<ArrShape>();
        const Type* t = &s.getType().getCanonicalType();
        int64_t total = 1;
        while (t->isUnpackedArray()) {
            if (t->kind != SymbolKind::FixedSizeUnpackedArrayType)
                throw std::runtime_error("lower: unsupported dynamic, associative, or queue array: " + std::string(s.name));
            const auto& ft = t->as<FixedSizeUnpackedArrayType>();
            shape->dims.push_back({ft.range.left, ft.range.right});
            total *= (int64_t)ft.range.width();
            if (total > kMaxArrayElems)
                throw std::runtime_error("lower: unsupported array with more than 2^20 elements: " + std::string(s.name));
            t = &ft.elementType.getCanonicalType();
        }
        int w = storeW(*t);
        if (w <= 0) unsupported_width(s.name);
        shape->count = (int)total;
        shape->elemW = w;
        shape->sgn = t->isSigned();
        shape->twoState = !t->isFourState();
        std::string base = prefix + std::string(s.name);
        std::vector<int64_t> idx(shape->dims.size());
        for (int64_t n = 0; n < total; n++) {
            int64_t rem = n;
            for (size_t d = shape->dims.size(); d-- > 0;) {
                int64_t sz = shape->size(d), o = rem % sz;
                rem /= sz;
                auto [l, r] = shape->dims[d];
                idx[d] = l <= r ? l + o : l - o;
            }
            std::string name = base;
            for (int64_t i : idx) name += "[" + std::to_string(i) + "]";
            int id = mkSig(name, w, net);
            if (n == 0) shape->base = id;
            else if (id != shape->base + n) Lowerer::unsupported("array elements that are not consecutive signals");
        }
        arrMap()[&s] = shape;
        noteHidden(s, shape->base, shape->count);
    }
    static void unsupported_width(std::string_view n) {
        throw std::runtime_error("lower: signal has a non-positive width: " + std::string(n));
    }
    // Inside a class method, automatic variables live in frames: only a
    // static local gets a signal. A class's static properties are signals
    // named "C::x"; the others are object slots (ClassInfo).
    int inMethod = 0;
    void handle(const slang::ast::VariableSymbol& v) {
        if (inMethod && v.lifetime == slang::ast::VariableLifetime::Automatic) return;
        reg(v, false);
    }
    void handle(const slang::ast::FormalArgumentSymbol& v) {
        // A DPI import's array formal has no signal: the call passes the actual array (35.5.6.1).
        using namespace slang::ast;
        const auto& t = v.getType().getCanonicalType();
        if (t.kind == SymbolKind::DPIOpenArrayType) return;
        if (t.isUnpackedArray()) {
            const Scope* sc = v.getParentScope();
            if (sc && sc->asSymbol().kind == SymbolKind::Subroutine &&
                sc->asSymbol().as<SubroutineSymbol>().flags.has(MethodFlags::DPIImport))
                return;
        }
        if (!inMethod && v.direction != ArgumentDirection::Ref) reg(v, false);   // a ref formal gets an address slot later
    }
    void handle(const slang::ast::ClassPropertySymbol& p) {
        if (p.lifetime == slang::ast::VariableLifetime::Static) reg(p, false);
    }
    int subrDepth = 0;
    void handle(const slang::ast::SubroutineSymbol& sub) {
        bool m = isClassMethod(sub);
        inMethod += m;
        // Every signal made in here belongs to the subroutine (see subrLocalSigs).
        MkSig saved = mkSig;
        if (subrDepth++ == 0)
            mkSig = [saved](const std::string& n, int w, bool net) {
                int id = saved(n, w, net);
                subrLocalSigs().insert(id);
                return id;
            };
        visitDefault(sub);
        if (--subrDepth == 0) mkSig = saved;
        inMethod -= m;
    }
    // extern task t in an interface (25.7): the module task that exports it
    // is not a member of any scope; its variables are registered here.
    void handle(const slang::ast::MethodPrototypeSymbol& p) {
        visitDefault(p);
        for (auto* x = p.getFirstExternImpl(); x; x = x->getNextImpl()) x->impl->visit(*this);
    }
    void handle(const slang::ast::ClassType& c) {
        std::string saved = prefix;
        prefix += std::string(c.name) + "::";
        visitDefault(c);
        prefix = std::move(saved);
    }
    // A clocking block (14): its event is a pair of signals like a named
    // event's, and its clockvars are named cb.x.
    void handle(const slang::ast::CovergroupType&) {}   // built when used (lower_cover.inc)
    void handle(const slang::ast::ClockingBlockSymbol& cb) {
        std::string name = prefix + std::string(cb.name.empty() ? "$cb" : cb.name);
        int id = mkSig(name, 32, false);
        if (mkSig(name + "$trig", 64, false) != id + 1) Lowerer::unsupported("event signals that are not consecutive");
        sigmap[&cb] = id;
        std::string saved = prefix;
        prefix = name + ".";
        visitDefault(cb);
        prefix = std::move(saved);
    }
    // A modport port (25.5) is another name for a signal of its interface.
    // They are aliased after every signal is registered.
    std::vector<const slang::ast::ModportPortSymbol*> modportPorts;
    void handle(const slang::ast::ModportPortSymbol& m) { modportPorts.push_back(&m); }
    // A package's variables are named "pkg::x" (26.3), apart from any module's x.
    void handle(const slang::ast::PackageSymbol& p) {
        std::string saved = prefix;
        prefix = std::string(p.name) + "::";
        visitDefault(p);
        prefix = std::move(saved);
    }
    std::vector<std::pair<const slang::ast::Symbol*, uint8_t>> aliasedKinds;
    void finishAliases() {
        for (const auto* s : aliased) {
            const slang::ast::Symbol* root = aliasOf().at(s);
            bool any = false;
            if (auto it = sigmap.find(root); it != sigmap.end()) { sigmap[s] = it->second; any = true; }
            if (auto it = arrMap().find(root); it != arrMap().end()) { arrMap()[s] = it->second; any = true; }
            if (auto it = dynMap().find(root); it != dynMap().end()) { dynMap()[s] = it->second; any = true; }
            if (auto it = dynArrMap().find(root); it != dynArrMap().end()) { dynArrMap()[s] = it->second; any = true; }
            if (!any) Lowerer::unsupported("alias of a net that has no signal");
        }
        for (auto [s, k] : aliasedKinds)
            if (auto it = sigmap.find(s); it != sigmap.end()) netKinds().push_back({it->second, k});
    }
    void aliasModports() {
        for (const auto* m : modportPorts) {
            const slang::ast::Symbol* in = m->internalSymbol;
            if (m->explicitConnection) {   // .P(r) names r; .P(r[3:0]) and others are lowered where used (25.5.4)
                const auto& c = Lowerer::peel(*m->explicitConnection);
                in = slang::ast::ValueExpressionBase::isKind(c.kind) ? &c.as<slang::ast::ValueExpressionBase>().symbol : nullptr;
            }
            if (!in) continue;
            if (auto it = sigmap.find(in); it != sigmap.end()) sigmap[m] = it->second;
            if (auto it = arrMap().find(in); it != arrMap().end()) arrMap()[m] = it->second;
            if (auto it = dynMap().find(in); it != dynMap().end()) dynMap()[m] = it->second;
        }
    }
    void handle(const slang::ast::NetSymbol& n) {
        reg(n, true);
        using NK = slang::ast::NetType::NetKind;
        uint8_t k = NR_WIRE;
        switch (n.netType.netKind) {
            case NK::WAnd: case NK::TriAnd: k = NR_WAND; break;
            case NK::WOr: case NK::TriOr: k = NR_WOR; break;
            case NK::Tri0: k = NR_TRI0; break;
            case NK::Tri1: k = NR_TRI1; break;
            case NK::TriReg: k = NR_TRIREG; break;
            case NK::Supply0: k = NR_SUPPLY0; break;
            case NK::Supply1: k = NR_SUPPLY1; break;
            case NK::Interconnect: break;   // resolves as a wire (6.6.8)
            default: break;
        }
        if (k == NR_WIRE) return;
        // A net collapsed into another (an inout port, alias): its type is the
        // simulated net's when the other side is a wire (23.3.3.7), once the
        // alias has its signal.
        if (auto a = aliasOf().find(&n); a != aliasOf().end() && a->second != &n) {
            if (!n.getType().isUnpackedArray()) aliasedKinds.push_back({&n, k});
            return;
        }
        if (n.getType().isUnpackedArray()) {   // wor w [0:1]: each element resolves on its own (6.6.3)
            auto a = arrMap().find(&n);
            if (a == arrMap().end() || a->second->dynElem) Lowerer::unsupported("this array of wand/wor/tri0/tri1/trireg/supply nets");
            for (int e = 0; e < a->second->count; e++) netKinds().push_back({a->second->base + e, k});
            return;
        }
        netKinds().push_back({sigmap.at(&n), k});
    }
    // A checker instance (17.3): its variables and output formals are named
    // like an instance's, "chk.v". Its input formals are not signals: slang
    // puts each actual in place of the formal.
    void handle(const slang::ast::CheckerInstanceSymbol& ci) {
        std::string saved = prefix;
        prefix += std::string(ci.name) + ".";
        visitDefault(ci);
        prefix = std::move(saved);
    }
    void handle(const slang::ast::InstanceSymbol& inst) {
        if (!enterInstance(inst)) return;
        std::string saved = prefix;
        if (!inst.isTopLevel()) {
            std::string nm = instName(inst);
            if (nm.empty()) Lowerer::unsupported("unnamed instance");
            prefix += nm;
            prefix.push_back('.');
        }
        visitDefault(inst);
        prefix = std::move(saved);
    }
    // Generate blocks (27): a signal inside one is named with its block,
    // "gen[2].x" for a loop iteration and "genblk1.x" for an unnamed block
    // (27.6). A block that was not selected is skipped.
    void handle(const slang::ast::GenerateBlockSymbol& g) {
        if (g.isUninstantiated) return;
        std::string saved = prefix;
        const auto* parent = g.getParentScope() ? &g.getParentScope()->asSymbol() : nullptr;
        if (parent && parent->kind == slang::ast::SymbolKind::GenerateBlockArray) {
            std::string an = parent->name.empty() ? std::string("genblk") : std::string(parent->name);
            prefix += an + "[" + (g.getArrayIndex() ? g.getArrayIndex()->toString() : std::string("?")) + "].";
        } else {
            prefix += g.getExternalName() + ".";
        }
        visitDefault(g);
        prefix = std::move(saved);
    }
};

// Pass 2: lower every procedural block and connect instance ports.
struct ProcCollector : slang::ast::ASTVisitor<ProcCollector> {
    std::unordered_map<const slang::ast::Symbol*, int>& sigmap;
    LowerResult& out;
    std::function<int(int)> newDriver;
    std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* subs = nullptr;
    MkSig mkSig;
    int* tempSeq = nullptr;
    ProcCollector(std::unordered_map<const slang::ast::Symbol*, int>& m, LowerResult& o,
                  std::function<int(int)> nd,
                  std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>* s,
                  MkSig mk, int* seq)
        : sigmap(m), out(o), newDriver(std::move(nd)), subs(s), mkSig(std::move(mk)), tempSeq(seq) {
        Lowerer::eventExprHook() = [this](const slang::ast::Expression& x) { return eventSignal(x); };
    }
    ~ProcCollector() { Lowerer::eventExprHook() = nullptr; }

    // One signal per distinct event expression, driven by a continuous
    // assignment of that expression.
    std::unordered_map<const slang::ast::Expression*, int> evSigs;
    int eventSignal(const slang::ast::Expression& x) {
        if (auto it = evSigs.find(&x); it != evSigs.end()) return it->second;
        int w = (int)x.type->getBitWidth();
        if (w <= 0 || !x.type->isIntegral()) Lowerer::unsupported("event on an expression of this type");
        int sg = mkSig("__event" + std::to_string((*tempSeq)++), w, false);
        // Start equal to the expression, before any procedure waits, so
        // variables that start with a value give no event at time 0.
        if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
        {
            Instr i0; i0.k = IK::Assign; i0.sig = sg;
            i0.e = init->expr(x);
            init->prog.emit(i0);
        }
        Lowerer lw(sigmap);
        prep(lw);
        Instr i; i.k = IK::Assign;
        i.sig = sg;
        i.e = lw.expr(x);
        if (!lw.prog.code.empty()) Lowerer::unsupported("event expression with a call");
        finishAssign(lw, i);
        evSigs[&x] = sg;
        return sg;
    }

    void prep(Lowerer& lw) { lw.bind(subs, mkSig, tempSeq, &out.subs); }

    // Nets of a user-defined nettype with a resolution function (6.6.7):
    // each continuous driver writes its own variable, and one process calls
    // the function with all of them whenever one changes (and once at time 0).
    struct UserNet {
        const slang::ast::NetSymbol* net = nullptr;
        const slang::ast::SubroutineSymbol* fn = nullptr;
        int w = 0;
        std::vector<int> drivers;
        std::shared_ptr<const ArrShape> arr;   // a net of an unpacked array type: its elements (held packed by the drivers)
    };
    std::map<int, UserNet> userNets;
    bool userNetsFound = false;
    UserNet* userNet(int sig) {
        using namespace slang::ast;
        if (!userNetsFound) {
            userNetsFound = true;
            for (auto& [sym, id] : sigmap) {
                if (sym->kind != SymbolKind::Net) continue;
                const auto& n = sym->as<NetSymbol>();
                if (n.netType.netKind != NetType::UserDefined) continue;
                if (const SubroutineSymbol* fn = n.netType.getResolutionFunction())
                    userNets[id] = UserNet{&n, fn, storeW(n.getType()), {}, nullptr};
            }
            for (auto& [sym, a] : arrMap()) {   // nettype of an unpacked array type (6.6.7)
                if (sym->kind != SymbolKind::Net || a->dynElem) continue;
                const auto& n = sym->as<NetSymbol>();
                if (n.netType.netKind != NetType::UserDefined) continue;
                const SubroutineSymbol* fn = n.netType.getResolutionFunction();
                int w = uWidth(n.getType());
                if (fn && w > 0) userNets[a->base] = UserNet{&n, fn, w, {}, a};
            }
        }
        auto it = userNets.find(sig);
        return it == userNets.end() ? nullptr : &it->second;
    }
    void finishUserNets() {
        using namespace slang::ast;
        userNet(-1);
        for (auto& [sig, un] : userNets) {
            Lowerer L(sigmap);
            prep(L);
            L.setScope(un.net->getParentScope());
            auto si = subs->find(un.fn);
            if (si == subs->end()) Lowerer::unsupported("resolution function");
            const Subr& sub = *si->second;
            if (sub.args.size() != 1 || !sub.args[0].arr || !Lowerer::isDynType(sub.args[0].arr->getType()))
                Lowerer::unsupported("resolution function that does not take one dynamic array");
            int d = L.dynTemp(Lowerer::dynShapeOf(sub.args[0].arr->getType()));
            L.prog.emit(L.dynCmd(d, D_DELETE));
            for (int dv : un.drivers) { Instr p = L.dynCmd(d, D_PUSH_BACK); p.e = ref(dv); L.prog.emit(p); }
            Instr c; c.k = IK::Call; c.callee = sub.prog;
            E ret;
            if (sub.lay) {
                c.lay = sub.lay; c.noSelf = true;
                c.dinit.push_back({sub.args[0].sig, d});
                L.prog.emit(c);
                ret = ref(relSig(kRelRet, sub.retSig), sub.retSgn);
            } else {
                int fd = L.dynFind(sub.args[0].arr);
                if (fd < 0) Lowerer::unsupported("resolution function argument");
                Instr cp = L.dynCmd(fd, D_COPY); cp.dsrc = d; L.prog.emit(cp);
                L.prog.emit(c);
                ret = ref(sub.retSig, sub.retSgn);
            }
            if (sub.retSig < 0) Lowerer::unsupported("resolution function with no result");
            if (un.arr) {   // each element from its bits of the result, left element highest
                int ew = un.w / un.arr->count;
                int rt = L.fresh(un.w);
                Instr t; t.k = IK::Assign; t.sig = rt; t.e = ret;
                L.prog.emit(t);
                for (int k = 0; k < un.arr->count; k++) {
                    int top = un.w - k * ew;
                    auto sl = node(Op::Slice, {ref(rt)});
                    sl->hi = top - 1;
                    sl->lo = top - ew;
                    Instr a; a.k = IK::Assign; a.sig = un.arr->base + k; a.e = sl; a.drv = newDriver(a.sig);
                    L.prog.emit(a);
                }
            } else {
                Instr a; a.k = IK::Assign; a.sig = sig; a.e = ret; a.drv = newDriver(sig);
                L.prog.emit(a);
            }
            std::vector<std::pair<int, Edge>> ev;
            for (int dv : un.drivers) ev.push_back({dv, ANY});
            bool loops = !ev.empty();
            if (loops) {
                L.prog.emit(waitInstr(ev));
                Instr j; j.k = IK::Jmp; j.target = 0; L.prog.emit(j);
            }
            out.procs.push_back({std::move(L.prog), loops, "resolve"});
        }
    }

    // One driver, then wait on every signal the right-hand side reads.
    // Several assigns to one wire each own a driver slot and resolve.
    void finishAssign(Lowerer& lw, Instr i) {
        // assign v[3] = ... / assign v[7:4] = ... (10.3): this driver drives
        // only those bits; the rest of its slot stays Z, so other drivers of
        // the other bits resolve with it.
        if (i.arr || i.bidx) Lowerer::unsupported("continuous assign to a variable index");
        if (auto un = userNet(i.sig)) {   // a driver of a net with a resolution function (6.6.7): its own variable
            if (i.hi >= 0) Lowerer::unsupported("continuous assign to part of a net of a user-defined nettype");
            i.sig = mkSig("$" + std::to_string(i.sig) + "$drv" + std::to_string(un->drivers.size()), un->w, false);
            un->drivers.push_back(i.sig);
            i.s0 = i.s1 = STR_STRONG;
            i.dly.clear();
            lw.prog.emit(i);
            auto rs = combReadSet(lw.prog);
            if (!rs.empty()) {
                lw.prog.emit(waitInstr(rs));
                Instr j; j.k = IK::Jmp; j.target = 0; lw.prog.emit(j);
            }
            out.procs.push_back({std::move(lw.prog), !rs.empty(), "assign"});
            return;
        }
        i.drv = newDriver(i.sig);
        if (linkFormal >= 0 && i.hi < 0) outLinks().push_back({linkFormal, i.sig, i.drv});
        lw.prog.emit(i);
        auto rs = combReadSet(lw.prog);
        bool loops = !rs.empty();
        if (loops) {
            lw.prog.emit(waitInstr(rs));
            Instr j; j.k = IK::Jmp; j.target = 0; lw.prog.emit(j);
        }
        out.procs.push_back({std::move(lw.prog), loops, "assign", curScope, curPort, curPortDir,
                             program, checkerDepth > 0});
    }

    // Module-level `logic x = v;` (IEEE 1800-2023 10.5): a static variable's
    // declaration initializer runs before any initial or always procedure
    // starts. All of them go into one process that lower() places first, so
    // it is the first thing to run at time 0. Locals of a block, task or
    // function are not module members and are handled by Lowerer.
    std::unique_ptr<Lowerer> init;
    std::unique_ptr<Lowerer> init0;   // default values (no initializer): run before every initializer
    void handle(const slang::ast::VariableSymbol& v) {
        const slang::ast::Expression* e = v.getInitializer();
        if (v.getType().isEvent()) {   // both counters start at 0: not triggered
            if (!sigmap.count(&v)) return;   // an automatic one lives in a frame
            if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
            for (int k = 0; k < 2; k++) {
                Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v) + k;
                i.e = konst(mk(0, k ? 64 : 32));
                init->prog.emit(i);
            }
            if (auto p = eventPtr().find(&v); p != eventPtr().end()) {   // names itself, or its initializer's event
                Instr i; i.k = IK::Assign; i.sig = p->second;
                if (e) { init->setScope(v.getParentScope()); i.e = init->eventAddr(*e); }
                else { auto n = node(Op::AddrOf); n->sig = sigmap.at(&v); i.e = n; }
                init->prog.emit(i);
            } else if (e) Lowerer::unsupported("event initializer");
            return;
        }
        const slang::ast::Scope* sc = v.getParentScope();
        if (!sc) return;
        auto pk = sc->asSymbol().kind;
        if (pk != slang::ast::SymbolKind::InstanceBody && pk != slang::ast::SymbolKind::GenerateBlock &&
            pk != slang::ast::SymbolKind::Package && pk != slang::ast::SymbolKind::CompilationUnit &&
            pk != slang::ast::SymbolKind::CheckerInstanceBody) {   // package (26.2) and checker (17.7) variables too
            // A static local of a task, function or block with no
            // initializer: a 2-state one starts at 0 like a module variable
            // (6.8, 6.21). One with an initializer takes it on the first call.
            if (v.lifetime == slang::ast::VariableLifetime::Static && !e && (sigmap.count(&v) || arrMap().count(&v)) && !dynMap().count(&v))
                initStatic(v);
            return;
        }
        if (v.flags.has(slang::ast::VariableFlags::CheckerFreeVariable) && !e) { freeVar(v); return; }
        initStatic(v);
    }
    // `output logic q = 5` (23.2.2.3): slang keeps the initializer on the
    // port; it initializes the port's variable like a declaration's.
    void handle(const slang::ast::PortSymbol& ps) {
        using namespace slang::ast;
        if (ps.direction != ArgumentDirection::Out || !ps.internalSymbol ||
            ps.internalSymbol->kind != SymbolKind::Variable)
            return;
        const Expression* e = ps.getInitializer();
        if (!e) return;
        const auto& v = ps.internalSymbol->as<VariableSymbol>();
        if (!sigmap.count(&v) || arrMap().count(&v) || dynMap().count(&v)) Lowerer::unsupported("this output port initializer");
        if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
        init->setScope(ps.getParentScope());
        Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v);
        i.e = resize(init->expr(*e), storeW(v.getType()), e->type->isSigned(), v.getType().isSigned());
        init->prog.emit(i);
    }
    // A class's static property (8.9) starts like a module variable.
    void handle(const slang::ast::ClassPropertySymbol& p) {
        if (p.lifetime == slang::ast::VariableLifetime::Static) initStatic(p);
    }
    void initStatic(const slang::ast::VariableSymbol& v) {
        const slang::ast::Expression* e = v.getInitializer();
        if (auto it = dynMap().find(&v); it != dynMap().end()) {   // int q[$] = '{1, 2};
            if (!e) return;
            if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
            init->assignContainer(it->second, *e);
            return;
        }
        if (e && arrMap().count(&v)) {   // int a[3] = '{1, 2, 3};
            if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
            init->assignWholeArray(v, v.getType(), *e, false);
            return;
        }
        if (auto ar = arrMap().find(&v); ar != arrMap().end()) {   // int a[3]: 2-state elements start at 0 (6.8)
            const auto& s = *ar->second;
            if (!s.twoState) return;
            if (!init0) { init0 = std::make_unique<Lowerer>(sigmap); prep(*init0); }
            if (s.count <= 64) {
                for (int k = 0; k < s.count; k++) {
                    Instr i; i.k = IK::Assign; i.sig = s.base + k;
                    i.e = konst(make(0, 0, s.elemW, 0, s.sgn));
                    init0->prog.emit(i);
                }
                return;
            }
            // A big array (bit [7:0] mem [0:1<<20]): a loop over a flat view,
            // not one instruction per element, which took gigabytes.
            auto flat = std::make_shared<ArrShape>();
            flat->base = s.base; flat->count = s.count; flat->elemW = s.elemW;
            flat->sgn = s.sgn; flat->twoState = true;
            flat->dims = {{0, (int64_t)s.count - 1}};
            Prog& p = init0->prog;
            int c = init0->fresh(32);
            Instr i0; i0.k = IK::Assign; i0.sig = c; i0.e = konst(make(0, 0, 32, 0, true));
            p.emit(i0);
            size_t top = p.here();
            Instr t; t.k = IK::JmpIfNot; t.e = bin(Op::Lt, ref(c, true), konst(make((uint64_t)s.count, 0, 32, 0, true)));
            size_t test = p.emit(t);
            Instr a; a.k = IK::Assign; a.sig = s.base; a.arr = flat; a.aidx = {ref(c, true)};
            a.e = konst(make(0, 0, s.elemW, 0, s.sgn));
            p.emit(a);
            Instr inc; inc.k = IK::Assign; inc.sig = c; inc.e = bin(Op::Add, ref(c, true), konst(make(1, 0, 32, 0, true)));
            p.emit(inc);
            Instr j; j.k = IK::Jmp; j.target = top;
            p.emit(j);
            p.code[test].target = p.here();
            return;
        }
        const auto& t = v.getType();
        if (!e && (t.isUnpackedStruct() || t.isUnpackedUnion())) {   // 7.2: each member's default
            if (!init0) { init0 = std::make_unique<Lowerer>(sigmap); prep(*init0); }
            Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v);
            i.e = init0->structDefault(t);
            init0->prog.emit(i);
            return;
        }
        // 6.8: with no initializer, a 2-state variable (int, bit, real) starts at 0.
        // A chandle (6.14) or a class handle (8.4) starts as null.
        bool zero = !t.isFourState() && (t.isIntegral() || t.isFloating() || t.isString() || t.isCHandle() || isHandleType(t));
        if (!e && !zero) return;
        if (zero) {   // defaults go first, so no initializer (which may call any function) sees an unset variable
            if (!init0) { init0 = std::make_unique<Lowerer>(sigmap); prep(*init0); }
            Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v);
            i.e = konst(mk(0, storeW(t)));
            init0->prog.emit(i);
            if (!e) return;
        }
        if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
        Instr i; i.k = IK::Assign; i.sig = sigmap.at(&v);
        i.e = init->expr(*e);
        init->prog.emit(i);
    }
    // Module path delays (30.4, 30.5): each input-to-output pair of a path
    // is a PathRec. When an output with paths changes, the kernel picks the
    // delay from the paths whose input changed last and whose condition
    // holds (30.5.3), so paths to one output may have different delays and
    // SDF IOPATH entries (32.4.1) annotate each pair.
    static std::string termText(const slang::ast::Expression& e) {
        if (e.syntax) return sdf::squeeze(e.syntax->toString());
        const slang::ast::Symbol* s = e.getSymbolReference();
        return s ? std::string(s->name) : std::string();
    }
    void handle(const slang::ast::TimingPathSymbol& tp) {
        using namespace slang::ast;
        Lowerer d(sigmap);
        prep(d);
        d.setScope(tp.getParentScope());
        std::vector<uint64_t> dl;
        auto ds = tp.getDelays();
        if (ds.size() != 1 && ds.size() != 2 && ds.size() != 3 && ds.size() != 6 && ds.size() != 12)
            Lowerer::unsupported("a path delay with this many values");
        for (const Expression* e : ds) {
            Instr t;
            d.delayInto(*e, t);
            if (t.de) Lowerer::unsupported("a path delay that is not constant");
            dl.push_back(t.delay);
        }
        switch (delayDirectives(tp.getParentScope()).mode) {   // Annex E
            case DelayDirectives::DISTRIBUTED: return;   // only the distributed delays count
            case DelayDirectives::ZERO: for (auto& v : dl) v = 0; break;
            case DelayDirectives::UNIT: for (auto& v : dl) if (v) v = pow10u(d.tsPrec - simPrecExp()); break;
            default: break;
        }
        std::vector<uint64_t> d12 = expand12(dl);
        auto sigOf = [&](const Expression* x, const char* what) {
            const Expression& p = Lowerer::peel(*x);
            const Symbol* sym = p.getSymbolReference();
            if (p.kind == ExpressionKind::ElementSelect || p.kind == ExpressionKind::RangeSelect) {
                const Expression& v = p.kind == ExpressionKind::ElementSelect ? p.as<ElementSelectExpression>().value()
                                                                              : p.as<RangeSelectExpression>().value();
                sym = Lowerer::peel(v).getSymbolReference();
            }
            auto it = sym ? sigmap.find(sym) : sigmap.end();
            if (it == sigmap.end()) Lowerer::unsupported(what);
            return std::pair<int, const Symbol*>(it->second, sym);
        };
        PathRec base;
        base.inst = Lowerer::instPath(tp.getParentScope());
        base.unit = d.ticksPerUnit();
        base.sym = &tp;
        if (const Expression* c = tp.getConditionExpr()) {
            base.hasCond = true;
            base.cond = termText(*c);
            base.condE = d.expr(*c);
        } else if (tp.isStateDependent) base.ifnone = true;
        base.edge = tp.edgeIdentifier == EdgeKind::PosEdge ? 1 : tp.edgeIdentifier == EdgeKind::NegEdge ? 2
                  : tp.edgeIdentifier == EdgeKind::BothEdges ? 3 : 0;
        std::copy(d12.begin(), d12.end(), base.d);
        auto ins = tp.getInputs(), outs = tp.getOutputs();
        // Pulse limits (30.7.1): PATHPULSE$in$out names the first input and
        // output of this declaration and covers all its paths; PATHPULSE$
        // covers every path of the module the others do not name. Without
        // either, the +pulse_r / +pulse_e percentages of the delay (30.7.2).
        for (int k = 0; k < 12; k++) { base.rj[k] = rejectOf(base.d[k]); base.er[k] = errorOf(base.d[k]); }
        {
            const SpecparamSymbol *mine = nullptr, *all = nullptr;
            const Symbol* in0 = ins.empty() ? nullptr : sigOf(ins[0], "a path from this input").second;
            const Symbol* out0 = outs.empty() ? nullptr : sigOf(outs[0], "a path to this output").second;
            // slang marks only the two-value form as a path pulse; with one
            // value PATHPULSE$a$y is a plain specparam, named the same way.
            std::string in0n = ins.empty() ? "" : std::string(in0->name), out0n = outs.empty() ? "" : std::string(out0->name);
            for (const Symbol& m : tp.getParentScope()->members()) {
                if (m.kind != SymbolKind::Specparam) continue;
                const auto& sp = m.as<SpecparamSymbol>();
                std::string_view nm = sp.name;
                if (!nm.starts_with("PATHPULSE$")) continue;
                if (sp.isPathPulse && (sp.getPathSource() || sp.getPathDest())) {
                    if (sp.getPathSource() == in0 && sp.getPathDest() == out0) mine = &sp;
                    continue;
                }
                std::string rest(nm.substr(10));
                if (rest.empty()) all = &sp;
                else if (rest == in0n + "$" + out0n) mine = &sp;
            }
            if (const SpecparamSymbol* sp = mine ? mine : all) {
                auto ticks = [&](const slang::ConstantValue& cv) -> uint64_t {
                    double v = cv.isReal() ? (double)cv.real() : cv.isInteger() && !cv.integer().hasUnknown()
                                   ? (double)*cv.integer().as<int64_t>() : 0.0;
                    if (v < 0) v = 0;
                    return (uint64_t)std::llround(v * (double)d.ticksPerUnit());
                };
                uint64_t r = ticks(sp->isPathPulse ? sp->getPulseRejectLimit() : sp->getValue());
                uint64_t e = r;
                if (sp->isPathPulse && !sp->getPulseErrorLimit().bad()) e = ticks(sp->getPulseErrorLimit());
                for (int k = 0; k < 12; k++) { base.rj[k] = r; base.er[k] = std::max(e, r); }
            }
        }
        for (const Expression* o : outs) {
            auto [dst, osym] = sigOf(o, "a path to this output");
            // A variable output (output reg q): the module reads q at once,
            // and the path delay is on its port connection (30.4).
            bool var = osym->kind != SymbolKind::Net;
            // ch.q of an interface port (25.6): a modport port, or a member of an interface instance
            const Symbol* osrc = osym;
            if (osrc->kind == SymbolKind::ModportPort) osrc = nullptr;
            bool ifaceTerm = !osrc;
            if (osrc) {
                const Scope* ps = osrc->getParentScope();
                const Symbol* body = ps ? &ps->asSymbol() : nullptr;
                ifaceTerm = body && body->kind == SymbolKind::InstanceBody &&
                            body->as<InstanceBodySymbol>().getDefinition().definitionKind == DefinitionKind::Interface;
            }
            for (const Expression* i : ins) {
                PathRec r = base;
                r.in = termText(*i);
                r.out = termText(*o);
                r.src = sigOf(i, "a path from this input").first;
                r.dst = dst;
                r.dstVar = var;
                r.dstIface = var && ifaceTerm;
                pathRecs().push_back(std::move(r));
            }
        }
    }
    // pulsestyle_onevent / pulsestyle_ondetect / showcancelled /
    // noshowcancelled (30.7.4): the style of each path output named.
    void handle(const slang::ast::PulseStyleSymbol& ps) {
        using namespace slang::ast;
        for (const Expression* t : ps.getTerminals()) {
            const Expression& p = Lowerer::peel(*t);
            const Symbol* sym = p.getSymbolReference();
            if (p.kind == ExpressionKind::ElementSelect) sym = Lowerer::peel(p.as<ElementSelectExpression>().value()).getSymbolReference();
            else if (p.kind == ExpressionKind::RangeSelect) sym = Lowerer::peel(p.as<RangeSelectExpression>().value()).getSymbolReference();
            auto it = sym ? sigmap.find(sym) : sigmap.end();
            if (it == sigmap.end()) continue;
            uint8_t& st = pulseStyles()[it->second];
            switch (ps.pulseStyleKind) {
                case PulseStyleKind::OnEvent: st &= (uint8_t)~PS_DETECT; break;
                case PulseStyleKind::OnDetect: st |= PS_DETECT; break;
                case PulseStyleKind::ShowCancelled: st |= PS_SHOWCANCELLED; break;
                case PulseStyleKind::NoShowCancelled: st &= (uint8_t)~PS_SHOWCANCELLED; break;
            }
        }
    }
    // A timing check (31): its events, limits in ticks and notifier, for
    // the kernel's check process (tchk.inc).
    void handle(const slang::ast::SystemTimingCheckSymbol& sc) {
        using namespace slang::ast;
        using K = SystemTimingCheckKind;
        Lowerer::lastLoc() = sc.location;
        Lowerer lw(sigmap);
        prep(lw);
        lw.setScope(sc.getParentScope());
        TimingCheck t;
        static const std::pair<K, int> kinds[] = {
            {K::Setup, TC_SETUP}, {K::Hold, TC_HOLD}, {K::SetupHold, TC_SETUPHOLD},
            {K::Recovery, TC_RECOVERY}, {K::Removal, TC_REMOVAL}, {K::RecRem, TC_RECREM},
            {K::Skew, TC_SKEW}, {K::TimeSkew, TC_TIMESKEW}, {K::FullSkew, TC_FULLSKEW},
            {K::Period, TC_PERIOD}, {K::Width, TC_WIDTH}, {K::NoChange, TC_NOCHANGE}};
        bool known = false;
        for (auto [k, v] : kinds) if (k == sc.timingCheckKind) { t.kind = v; known = true; }
        if (!known) Lowerer::unsupported("this timing check");
        const auto* syn = sc.getSyntax() ? &sc.getSyntax()->as<slang::syntax::SystemTimingCheckSyntax>() : nullptr;
        if (!syn) Lowerer::unsupported("a timing check with no syntax");
        t.name = std::string(syn->name.valueText());
        auto args = sc.getArguments();
        auto text = [&](size_t k) {
            if (k >= syn->args.size()) return std::string();
            std::string x = syn->args[k]->toString();
            size_t a = x.find_first_not_of(" \t\r\n"), b = x.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
        };
        t.nev = (t.kind == TC_PERIOD || t.kind == TC_WIDTH) ? 1 : 2;
        if (t.kind == TC_SETUP) { t.data = 0; t.ref = 1; }
        if (t.nev == 1) t.data = 0;
        for (int k = 0; k < t.nev; k++) {
            const auto& a = args[(size_t)k];
            TcEvent& ev = t.ev[k];
            if (!a.expr) Lowerer::unsupported("a timing check with an empty event");
            ev.e = lw.expr(*a.expr);
            ev.text = text((size_t)k);
            ev.port = termText(*a.expr);
            if (a.condition) ev.condText = termText(*a.condition);
            if (a.condition) {
                ev.cond = lw.expr(*a.condition);
                const Expression& c = Lowerer::peel(*a.condition);
                if (c.kind == ExpressionKind::BinaryOp) {
                    auto op = c.as<BinaryExpression>().op;
                    ev.nondet = op == BinaryOperator::Equality || op == BinaryOperator::Inequality;
                }
            }
            if (!a.edgeDescriptors.empty()) {
                auto lv = [](char c) { return c == '0' ? 0 : c == '1' ? 1 : 2; };
                static const uint8_t bit[3][3] = {{0, TE_01, TE_0X}, {TE_10, 0, TE_1X}, {TE_X0, TE_X1, 0}};
                for (auto d : a.edgeDescriptors) ev.edges |= bit[lv(d[0])][lv(d[1])];
            } else if (a.edge == EdgeKind::PosEdge) ev.edges = TE_POS;
            else if (a.edge == EdgeKind::NegEdge) ev.edges = TE_NEG;
            else if (a.edge == EdgeKind::BothEdges) ev.edges = TE_ALL;
            std::vector<int> sigs;
            collectRefs(ev.e, sigs);
            if (sigs.size() == 1) ev.sig = sigs[0];
            for (int sg : sigs) {
                bool have = false;
                for (auto& w : t.watch) have |= w.first == sg;
                if (!have) t.watch.push_back({sg, ANY});
            }
        }
        if ((t.kind == TC_PERIOD || t.kind == TC_WIDTH || t.kind == TC_NOCHANGE) && !t.ev[0].edges)
            Lowerer::unsupported("a $period, $width or $nochange reference event without an edge");
        // The limits: constants in the module's time unit.
        auto ticks = [&](size_t k) -> int64_t {
            if (k >= args.size() || !args[k].expr) return 0;
            if (auto lv = lw.labelFold(*args[k].expr)) return (int64_t)std::llround(*lv * (double)lw.ticksPerUnit());
            ASTContext ctx(*sc.getParentScope(), LookupLocation::max);
            slang::ConstantValue cv = ctx.tryEval(*args[k].expr);
            double u;
            if (cv.isInteger() && !cv.integer().hasUnknown()) u = (double)*cv.integer().as<int64_t>();
            else if (cv.isReal()) u = cv.real();
            else Lowerer::unsupported("a timing check limit that is not constant");
            return (int64_t)std::llround(u * (double)lw.ticksPerUnit());
        };
        auto flag = [&](size_t k) {
            if (k >= args.size() || !args[k].expr) return false;
            ASTContext ctx(*sc.getParentScope(), LookupLocation::max);
            slang::ConstantValue cv = ctx.tryEval(*args[k].expr);
            return cv.isTrue();
        };
        size_t first = (size_t)t.nev, nlim = 1, nIdx;
        if (t.kind == TC_SETUPHOLD || t.kind == TC_RECREM || t.kind == TC_FULLSKEW || t.kind == TC_NOCHANGE ||
            t.kind == TC_WIDTH)
            nlim = 2;
        t.lim1 = ticks(first);
        if (nlim == 2) {
            if (t.kind == TC_WIDTH) t.thresh = ticks(first + 1);
            else t.lim2 = ticks(first + 1);
        }
        nIdx = first + nlim;
        for (size_t k = first; k < first + nlim && k < syn->args.size(); k++) {
            std::string x = text(k);
            if (x.empty()) continue;
            if (!t.limText.empty()) t.limText += ", ";
            t.limText += x;
        }
        if (nIdx < args.size() && args[nIdx].expr) {
            const Expression& n = Lowerer::peel(*args[nIdx].expr);
            const Symbol* sym = n.getSymbolReference();
            auto it = sym ? sigmap.find(sym) : sigmap.end();
            if (it == sigmap.end()) Lowerer::unsupported("this timing check notifier");
            t.notifier = it->second;
        }
        if (t.kind == TC_TIMESKEW || t.kind == TC_FULLSKEW) {
            t.eventBased = flag(nIdx + 1);
            t.remainActive = flag(nIdx + 2);
        }
        if (t.kind == TC_SETUPHOLD || t.kind == TC_RECREM) {
            // 31.9: timestamp and timecheck conditions, delayed reference and data.
            if (nIdx + 1 < args.size() && args[nIdx + 1].expr) {
                t.tsCond = lw.expr(*args[nIdx + 1].expr);
                t.tsText = termText(*args[nIdx + 1].expr);
            }
            if (nIdx + 2 < args.size() && args[nIdx + 2].expr) {
                t.tcCond = lw.expr(*args[nIdx + 2].expr);
                t.tcText = termText(*args[nIdx + 2].expr);
            }
            for (int k = 0; k < 2; k++) {
                size_t ix = nIdx + 3 + (size_t)k;
                if (ix >= args.size() || !args[ix].expr) continue;
                const Expression* dx = args[ix].expr;   // bound as an output: an assignment to it
                if (dx->kind == ExpressionKind::Assignment) dx = &dx->as<AssignmentExpression>().left();
                const Expression& x = Lowerer::peel(*dx);
                const Symbol* sym = x.getSymbolReference();
                if (x.kind != ExpressionKind::NamedValue || !sym || !sigmap.count(sym))
                    Lowerer::unsupported("a delayed signal that is not a whole net or variable");
                t.ev[k].delayed = sigmap.at(sym);
            }
        }
        t.inst = Lowerer::instPath(sc.getParentScope());
        if (const auto* sm = srcMgr()) {
            auto ol = sm->getFullyOriginalLoc(sc.location);
            t.file = std::string(sm->getFileName(ol));
            t.line = (int)sm->getLineNumber(ol);
        }
        t.unit = lw.ticksPerUnit();
        timingChecks().push_back(std::move(t));
    }
    // A path delay to a net that also has a net delay is not modeled (30.6).
    void finishPaths() {}   // a net delay on a path output combines with the path delay (30.6)
    // `wire y = a & b;` (IEEE 1800-2023 10.3.1) is a continuous assignment to y.
    // `wire #d w;` is a net delay: every change of w's resolved value takes
    // d to arrive (28.16). `wire #d w = a;` is instead a delayed continuous
    // assignment (10.3.3), and the net itself has no delay.
    void handle(const slang::ast::NetSymbol& n) {
        const slang::ast::Expression* e = n.getInitializer();
        std::vector<uint64_t> dl;
        bool trireg = n.netType.netKind == slang::ast::NetType::TriReg && !e;
        std::optional<uint64_t> decay;   // a trireg's charge decay time (28.16.2), ticks
        if (n.getDelay() || trireg) {
            Lowerer d(sigmap);
            prep(d);
            d.setScope(n.getParentScope());
            const auto* tc = n.getDelay();
            if (trireg && tc && tc->kind == slang::ast::TimingControlKind::Delay3 && tc->as<slang::ast::Delay3Control>().expr3) {
                // The third delay of a trireg is its charge decay time, not a turn-off delay.
                const auto& d3 = tc->as<slang::ast::Delay3Control>();
                Instr t;
                d.delayInto(*d3.expr3, t);
                if (t.de) Lowerer::unsupported("a trireg charge decay time that is not constant");
                decay = t.delay;
                std::vector<uint64_t> rf = d.delays3(n.getDelay());
                if (rf.size() == 3) rf.pop_back();
                if (std::all_of(rf.begin(), rf.end(), [](uint64_t v) { return v == 0; })) rf.clear();
                dl = rf;
            } else if (tc) dl = d.delays3(tc);
            if (trireg && !decay) {   // `default_decay_time (E.2)
                DelayDirectives dd = delayDirectives(n.getParentScope());
                if (dd.decaySet && !dd.decayInfinite) decay = d.ticksOf(dd.decay);
            }
            if (trireg) {
                auto cs = n.getChargeStrength();
                uint8_t lvl = STR_MEDIUM;
                if (cs) lvl = *cs == slang::ast::ChargeStrength::Small ? STR_SMALL : *cs == slang::ast::ChargeStrength::Large ? STR_LARGE : STR_MEDIUM;
                else if (int ts = delayDirectives(n.getParentScope()).triregStrength; ts >= 0) lvl = chargeLevel(ts);
                if (decay || lvl != STR_MEDIUM) out.trireg.push_back({sigmap.at(&n), lvl, decay ? *decay : 0, (bool)decay});
            }
        }
        if (!e) {
            if (!dl.empty()) out.netDelays.push_back({sigmap.at(&n), dl});
            return;
        }
        Lowerer lw(sigmap);
        prep(lw);
        lw.setScope(n.getParentScope());
        Instr i; i.k = IK::Assign; i.sig = sigmap.at(&n); i.e = lw.expr(*e);
        i.dly = dl;
        finishAssign(lw, i);
    }
    // Moves the initializer process to the front of `out.procs`.
    void placeInit() {
        if (init) out.procs.insert(out.procs.begin(), {std::move(init->prog), false, "init"});
        if (init0) out.procs.insert(out.procs.begin(), {std::move(init0->prog), false, "init"});
        init.reset();
        init0.reset();
    }

    static uint8_t strengthOf(slang::ast::DriveStrength s) {
        using slang::ast::DriveStrength;
        switch (s) {
            case DriveStrength::Supply: return STR_SUPPLY;
            case DriveStrength::Strong: return STR_STRONG;
            case DriveStrength::Pull: return STR_PULL;
            case DriveStrength::Weak: return STR_WEAK;
            case DriveStrength::HighZ: return STR_HIZ;
        }
        return STR_STRONG;
    }

    // slang's pair is (0-strength, 1-strength). A missing side keeps `fallback`.
    static void applyStrength(Instr& i, std::optional<slang::ast::DriveStrength> ds0,
                              std::optional<slang::ast::DriveStrength> ds1,
                              uint8_t fb0, uint8_t fb1) {
        i.s0 = ds0 ? strengthOf(*ds0) : fb0;
        i.s1 = ds1 ? strengthOf(*ds1) : fb1;
    }

    const std::vector<uint64_t>* gateDelay = nullptr;   // the delays of the gate being lowered
    int curGate = -1;                                    // its gateRecs() entry
    int linkFormal = -1;                                 // the output port being connected (outLinks)
    void driveOut(const slang::ast::Expression& out, E rhs, uint8_t s0, uint8_t s1) {
        Lowerer lw(sigmap);
        prep(lw);
        Instr i;
        i.k = IK::Assign;
        i.s0 = s0;
        i.s1 = s1;
        if (gateDelay) i.dly = *gateDelay;
        // A gate output port is sometimes an assignment whose left side is the net.
        const slang::ast::Expression* tgt = &out;
        if (tgt->kind == slang::ast::ExpressionKind::Assignment)
            tgt = &tgt->as<slang::ast::AssignmentExpression>().left();
        lw.lhs(*tgt, i);
        if (curGate >= 0) {
            i.gate = curGate;
            auto& o = gateRecs()[(size_t)curGate].outs;
            if (i.sig >= 0 && std::find(o.begin(), o.end(), i.sig) == o.end()) o.push_back(i.sig);
        }
        i.e = rhs;
        finishAssign(lw, i);
    }

    void handle(const slang::ast::ContinuousAssignSymbol& ca) {
        using namespace slang::ast;
        const auto& a = ca.getAssignment().as<AssignmentExpression>();
        Lowerer::covLoc(ca.location);
        Lowerer::lastLoc() = ca.location;
        Lowerer lw(sigmap);
        prep(lw);
        lw.condCov = true;
        auto [ds0, ds1] = ca.getDriveStrength();
        if (a.left().type->getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType) {
            // assign arr = other_arr / '{...}: one assign per element (10.3, 7.6)
            const Expression& l = Lowerer::peel(a.left());
            if (l.kind != ExpressionKind::NamedValue && l.kind != ExpressionKind::HierarchicalValue)
                Lowerer::unsupported("continuous assign to part of an unpacked array");
            auto it = arrMap().find(&l.as<ValueExpressionBase>().symbol);
            if (it == arrMap().end()) Lowerer::unsupported("continuous assign to this array");
            std::vector<E> els;
            lw.setScope(ca.getParentScope());
            lw.arrayElems(*a.left().type, a.right(), els);
            if ((int)els.size() != it->second->count || !lw.prog.code.empty())
                Lowerer::unsupported("continuous assign of an unpacked array from this value");
            if (auto un = userNet(it->second->base); un && un->arr) {   // a driver of an array nettype net (6.6.7): one packed value
                Instr i; i.k = IK::Assign; i.sig = it->second->base;
                i.e = els.size() == 1 ? els[0] : node(Op::Concat, std::move(els));
                finishAssign(lw, i);
                g_emitLoc = {};
                return;
            }
            for (int k = 0; k < it->second->count; k++) {
                Lowerer L(sigmap);
                prep(L);
                Instr i; i.k = IK::Assign;
                applyStrength(i, ds0, ds1, STR_STRONG, STR_STRONG);
                i.sig = it->second->base + k;
                i.e = els[(size_t)k];
                L.setScope(ca.getParentScope());
                i.dly = L.delays3(ca.getDelay());
                assignGate(ca, i);
                finishAssign(L, i);
            }
            g_emitLoc = {};
            return;
        }
        if (Lowerer::peel(a.left()).kind == ExpressionKind::Concatenation) {
            // assign {c, s} = x + y: one driver per part, each driving its
            // slice of the right-hand side.
            int w = (int)a.left().type->getBitWidth();
            std::vector<std::tuple<const Expression*, int, int>> parts;
            Lowerer::concatParts(a.left(), w - 1, parts);
            bool first = true;
            for (auto& [x, hi, lo] : parts) {
                Lowerer pw(sigmap);
                if (!first) prep(pw);
                Lowerer& L = first ? lw : pw;
                first = false;
                Instr i; i.k = IK::Assign;
                applyStrength(i, ds0, ds1, STR_STRONG, STR_STRONG);
                L.lhs(*x, i);
                i.e = slice(resize(L.expr(a.right()), w, false, false), hi, lo);
                L.setScope(ca.getParentScope());
                i.dly = L.delays3(ca.getDelay());
                assignGate(ca, i);
                finishAssign(L, i);
            }
            g_emitLoc = {};
            return;
        }
        Instr i; i.k = IK::Assign;
        applyStrength(i, ds0, ds1, STR_STRONG, STR_STRONG);
        lw.lhs(a.left(), i);
        i.e = lw.expr(a.right());
        lw.setScope(ca.getParentScope());
        i.dly = lw.delays3(ca.getDelay());
        assignGate(ca, i);
        finishAssign(lw, i);
        g_emitLoc = {};
    }
    // A delayed continuous assignment drives through a gateRecs() entry, so
    // vpi_put_delays can change its delay at run time (38.10).
    void assignGate(const slang::ast::ContinuousAssignSymbol& ca, Instr& i) {
        if (i.dly.empty()) return;
        auto [it, fresh] = assignGates().try_emplace(&ca, (int)gateRecs().size());
        if (fresh) {
            GateRec gr;
            gr.inst = Lowerer::instPath(ca.getParentScope());
            gr.name = "$assign";
            std::vector<uint64_t> d3 = i.dly;
            if (d3.size() > 3) d3.resize(3);
            std::vector<uint64_t> d12 = expand12(d3);
            std::copy(d12.begin(), d12.end(), gr.d);
            gateRecs().push_back(std::move(gr));
        }
        i.gate = it->second;
    }

    // buf/not/and/nand/or/nor/xor/xnor and pullup/pulldown drive the same way
    // as a continuous assign. Other primitives (mos, tran, bufif) do not.
    void handle(const slang::ast::PrimitiveInstanceSymbol& g) {
        using namespace slang::ast;
        std::vector<uint64_t> gdly;
        {
            Lowerer dl(sigmap);
            prep(dl);
            dl.setScope(g.getParentScope());
            gdly = dl.delays3(g.getDelay());
        }
        gateDelay = &gdly;
        struct Reset { const std::vector<uint64_t>*& p; int& g; ~Reset() { p = nullptr; g = -1; } } reset{gateDelay, curGate};
        {
            GateRec gr;
            gr.inst = Lowerer::instPath(g.getParentScope());
            gr.name = std::string(g.name);
            std::vector<uint64_t> d3 = gdly;
            if (d3.size() > 3) d3.resize(3);
            std::vector<uint64_t> d12 = expand12(d3);
            std::copy(d12.begin(), d12.end(), gr.d);
            curGate = (int)gateRecs().size();
            gateRecs().push_back(std::move(gr));
        }
        std::string_view name = g.primitiveType.name;
        auto ports = g.getPortConnections();
        auto [ds0, ds1] = g.getDriveStrength();
        if (g.primitiveType.primitiveKind == PrimitiveSymbol::UserDefined) {
            lowerUdp(g, ports, ds0 ? strengthOf(*ds0) : STR_STRONG, ds1 ? strengthOf(*ds1) : STR_STRONG);
            return;
        }
        uint8_t fb0 = STR_STRONG, fb1 = STR_STRONG;
        bool pull = name == "pullup" || name == "pulldown";
        if (name == "pullup") { fb0 = STR_HIZ; fb1 = STR_PULL; }
        else if (name == "pulldown") { fb0 = STR_PULL; fb1 = STR_HIZ; }
        uint8_t s0 = ds0 ? strengthOf(*ds0) : fb0;
        uint8_t s1 = ds1 ? strengthOf(*ds1) : fb1;

        bool ninput = name == "and" || name == "nand" || name == "or" || name == "nor" ||
                      name == "xor" || name == "xnor";
        bool noutput = name == "buf" || name == "not";
        bool tri = name == "bufif0" || name == "bufif1" || name == "notif0" || name == "notif1";
        bool mos = name == "nmos" || name == "pmos" || name == "rnmos" || name == "rpmos";
        bool cmos = name == "cmos" || name == "rcmos";
        if (name.find("tran") != std::string_view::npos) { lowerTran(name, ports); return; }
        if (!pull && !ninput && !noutput && !tri && !mos && !cmos)
            throw std::runtime_error("lower: unsupported gate " + std::string(name));
        if (tri || mos || cmos) { lowerSwitchGate(name, ports, s0, s1); return; }

        if (pull) {
            if (ports.size() != 1 || !ports[0]) Lowerer::unsupported("pull gate port");
            int w = (int)ports[0]->type->getBitWidth();
            if (w <= 0) Lowerer::unsupported("gate width");
            driveOut(*ports[0], konst(name == "pullup" ? v_ones(w) : make(0, 0, w)), s0, s1);
            return;
        }

        Lowerer lw(sigmap);
        prep(lw);
        if (noutput) {
            if (ports.size() < 2 || !ports.back()) Lowerer::unsupported("gate ports");
            E in = lw.expr(*ports.back());
            // not(not) is a buf. Z becomes X, and 0 and 1 pass through.
            E rhs = name == "not" ? un(Op::Not, in) : un(Op::Not, un(Op::Not, in));
            for (size_t i = 0; i + 1 < ports.size(); i++) {
                if (!ports[i]) Lowerer::unsupported("gate ports");
                driveOut(*ports[i], rhs, s0, s1);
            }
            return;
        }

        if (ports.size() < 2 || !ports[0]) Lowerer::unsupported("gate ports");
        Op op = (name == "and" || name == "nand") ? Op::And
              : (name == "or" || name == "nor") ? Op::Or : Op::Xor;
        if (!ports[1]) Lowerer::unsupported("gate ports");
        E acc = lw.expr(*ports[1]);
        for (size_t i = 2; i < ports.size(); i++) {
            if (!ports[i]) Lowerer::unsupported("gate ports");
            acc = bin(op, acc, lw.expr(*ports[i]));
        }
        if (name == "nand" || name == "nor" || name == "xnor") acc = un(Op::Not, acc);
        driveOut(*ports[0], acc, s0, s1);
    }

    // Three-state gates and MOS switches (28.5, 28.7): out = data (inverted
    // for notif) while the control enables it and Z while it disables it. An
    // X or Z control gives L or H (28.12.1): the value might be the data or
    // Z. Each gate is two drivers, one with only its 0 strength and one with
    // only its 1 strength, so an X on the first is L and on the second is H,
    // and the net resolves them by strength like any other driver. A MOS
    // switch passes Z. cmos is an nmos and a pmos on one output. The r forms
    // are resistive (28.13.1): minisim drives them at pull, the strength a
    // strong data gets through them.
    template <class Ports>
    void lowerSwitchGate(std::string_view name, const Ports& ports, uint8_t s0, uint8_t s1) {
        size_t need = name.ends_with("cmos") ? 4 : 3;
        if (ports.size() != need) Lowerer::unsupported("gate ports");
        for (auto* p : ports) if (!p) Lowerer::unsupported("gate ports");
        Lowerer lw(sigmap);
        prep(lw);
        int w = (int)ports[0]->type->getBitWidth();
        if (w != 1) Lowerer::unsupported("a three-state gate or switch wider than one bit");
        bool resist = name[0] == 'r';
        if (resist) {
            s0 = std::min<uint8_t>(s0, STR_PULL);
            s1 = std::min<uint8_t>(s1, STR_PULL);
        }
        auto bit = [&](const slang::ast::Expression& x) { return resize(lw.expr(x), 1, false, false); };
        auto is = [](E v, Val k) { return node(Op::CaseEq, {v, konst(k)}); };
        auto mux = [](E c, E a, E b) { return node(Op::Mux, {c, a, b}); };
        E d = bit(*ports[1]);
        E Z = konst(allz(1)), X = konst(allx(1));
        auto drive = [&](E ctl, bool activeHigh, bool invert, bool passZ) {
            E val = invert ? un(Op::Not, d) : passZ ? d : un(Op::Not, un(Op::Not, d));
            E on = is(ctl, make(activeHigh ? 1 : 0, 0, 1)), off = is(ctl, make(activeHigh ? 0 : 1, 0, 1));
            for (int part = 0; part < 2; part++) {   // 0: the 0-strength driver, 1: the 1-strength driver
                // With an unknown control this driver may drive unless the value is the other level or Z.
                E unk = mux(is(val, make(part ? 0 : 1, 0, 1)), Z, mux(is(val, allz(1)), Z, X));
                driveOut(*ports[0], mux(on, val, mux(off, Z, unk)), part ? STR_HIZ : s0, part ? s1 : STR_HIZ);
            }
        };
        if (name.starts_with("bufif") || name.starts_with("notif")) {
            drive(bit(*ports[2]), name.back() == '1', name[0] == 'n', false);
            return;
        }
        std::string_view base = resist ? name.substr(1) : name;
        if (base == "nmos" || base == "pmos") {
            drive(bit(*ports[2]), base == "nmos", false, true);
            return;
        }
        drive(bit(*ports[2]), true, false, true);    // cmos: the n control
        drive(bit(*ports[3]), false, false, true);   // and the p control
    }

    // A user-defined primitive instance (29). One process evaluates the table
    // whenever an input changes and keeps the UDP's state in a signal of its
    // own; a driver (with the instance's strength and delay) copies the state
    // to the output net.
    template <class Ports>
    void lowerUdp(const slang::ast::PrimitiveInstanceSymbol& g, const Ports& ports, uint8_t s0, uint8_t s1) {
        using namespace slang::ast;
        const PrimitiveSymbol& p = g.primitiveType;
        auto tab = std::make_unique<UdpTable>();
        tab->nin = (int)p.ports.size() - 1;
        tab->seq = p.isSequential;
        for (const auto& e : p.table) {
            UdpRow r;
            r.state = e.state;
            r.out = e.output;
            for (size_t k = 0; k < e.inputs.size(); k++) {
                char c = e.inputs[k];
                if (c == '(') {
                    if (k + 3 >= e.inputs.size()) Lowerer::unsupported("UDP table edge");
                    r.in.push_back(std::string(e.inputs.substr(k, 4)));
                    k += 3;
                } else r.in.push_back(std::string(1, c));
                const std::string& f = r.in.back();
                if (f[0] == '(' || f == "r" || f == "f" || f == "p" || f == "n" || f == "*") {
                    r.edge = (int)r.in.size() - 1;
                    tab->edges = true;
                }
            }
            if ((int)r.in.size() != tab->nin) Lowerer::unsupported("UDP table row");
            tab->rows.push_back(std::move(r));
        }
        if (ports.size() != p.ports.size()) Lowerer::unsupported("UDP instance ports");
        for (auto* x : ports) if (!x) Lowerer::unsupported("UDP instance ports");
        Lowerer lw(sigmap);
        prep(lw);
        int state = lw.fresh(1), last = lw.fresh(2 * tab->nin);
        Instr init; init.k = IK::Assign; init.sig = state;
        int iv = 2;   // x unless the UDP has an initial statement (29.7)
        if (p.initVal && p.initVal->isInteger()) {
            const auto& ci = p.initVal->integer();
            iv = ci.hasUnknown() ? 2 : (int)(ci.as<uint64_t>().value_or(0) & 1);
        }
        init.e = konst(iv == 2 ? allx(1) : make((uint64_t)iv, 0, 1));
        lw.prog.emit(init);
        Instr li; li.k = IK::Assign; li.sig = last; li.e = konst(allx(2 * tab->nin));   // inputs start at x
        lw.prog.emit(li);
        size_t top = lw.prog.here();
        Instr u; u.k = IK::Udp; u.udp = tab.get(); u.sig = state; u.dres = last;
        for (int k = 0; k < tab->nin; k++) u.args.push_back(resize(lw.expr(*ports[(size_t)k + 1]), 1, false, false));
        lw.prog.emit(u);
        auto rs = readSet(lw.prog);
        rs.erase(std::remove_if(rs.begin(), rs.end(), [&](const std::pair<int, Edge>& r) { return r.first == state || r.first == last; }), rs.end());
        bool loops = !rs.empty();
        if (loops) {
            lw.prog.emit(waitInstr(rs));
            Instr j; j.k = IK::Jmp; j.target = top; lw.prog.emit(j);
        }
        out.procs.push_back({std::move(lw.prog), loops, "udp"});
        out.udps.push_back(std::move(tab));
        driveOut(*ports[0], ref(state), s0, s1);
    }

    // tran, rtran, tranif0/1, rtranif0/1 (28.8): the two terminals become one
    // switch the kernel resolves with the drivers of both nets. A process
    // feeds it the control's value whenever the control changes.
    template <class Ports>
    void lowerTran(std::string_view name, const Ports& ports) {
        using namespace slang::ast;
        bool ctl = name.find("tranif") != std::string_view::npos;
        if (ports.size() != (ctl ? 3u : 2u)) Lowerer::unsupported("tran ports");
        int t[2];
        for (int k = 0; k < 2; k++) {
            if (!ports[k]) Lowerer::unsupported("tran ports");
            const Expression* x = ports[k];
            if (x->kind == ExpressionKind::Assignment) x = &x->as<AssignmentExpression>().left();
            x = &Lowerer::peel(*x);
            const Symbol* sym = ValueExpressionBase::isKind(x->kind) ? &x->as<ValueExpressionBase>().symbol : nullptr;
            auto it = sym ? sigmap.find(sym) : sigmap.end();
            if (!sym || sym->kind != SymbolKind::Net || it == sigmap.end())
                Lowerer::unsupported("a tran terminal that is not a whole net");
            t[k] = it->second;
        }
        if (ports[0]->type->getBitWidth() != ports[1]->type->getBitWidth()) Lowerer::unsupported("tran terminals of different widths");
        int id = (int)out.trans.size();
        out.trans.push_back({t[0], t[1], name[0] == 'r'});
        Lowerer lw(sigmap);
        prep(lw);
        Instr i;
        i.k = IK::Tran;
        i.sig = id;
        if (ctl) {
            if (!ports[2]) Lowerer::unsupported("tran ports");
            i.e = resize(lw.expr(*ports[2]), 1, false, false);
            i.pdown = name.back() == '0';
        }
        lw.prog.emit(i);
        bool loops = false;
        if (ctl) {
            auto rs = readSet(lw.prog);
            if (!rs.empty()) {
                lw.prog.emit(waitInstr(rs));
                Instr j; j.k = IK::Jmp; j.target = 0; lw.prog.emit(j);
                loops = true;
            }
        }
        out.procs.push_back({std::move(lw.prog), loops, "tran"});
    }

    // The child's formal signal. A null port (no internal connection) is skipped.
    struct Formal { int sig; int w; bool sgn; };
    // An inout port on a constant bit or part select of a net (`.d0(dat[0])`),
    // or on a whole net it does not collapse into (another net type): a switch between the internal net and those bits, so each side resolves
    // the other's drivers (23.3.3.1) and neither holds the other's last value.
    bool inoutTran(const slang::ast::PortSymbol& ps, const slang::ast::Expression& act0, const Formal& f) {
        using namespace slang::ast;
        const Expression& act = Lowerer::peel(act0);
        if (act.kind != ExpressionKind::ElementSelect && act.kind != ExpressionKind::RangeSelect &&
            act.kind != ExpressionKind::NamedValue && act.kind != ExpressionKind::HierarchicalValue)
            return false;
        auto simple = [](const Symbol& s) {
            if (s.kind != SymbolKind::Net) return false;
            const auto& n = s.as<NetSymbol>();
            auto k = n.netType.netKind;
            return n.getType().isIntegral() && !n.getDelay() && k != NetType::UserDefined && k != NetType::Interconnect &&
                   k != NetType::TriReg && k != NetType::Unknown;
        };
        if (!ps.internalSymbol || !simple(*ps.internalSymbol)) return false;
        const Expression* root = &act;
        while (root->kind == ExpressionKind::ElementSelect || root->kind == ExpressionKind::RangeSelect)
            root = root->kind == ExpressionKind::ElementSelect ? &root->as<ElementSelectExpression>().value()
                                                               : &root->as<RangeSelectExpression>().value();
        if (root->kind != ExpressionKind::NamedValue && root->kind != ExpressionKind::HierarchicalValue) return false;
        if (!simple(root->as<ValueExpressionBase>().symbol)) return false;
        if ((int)act.type->getBitWidth() != f.w) return false;
        Lowerer lw(sigmap);
        prep(lw);
        Instr i;
        lw.lhs(act, i);
        if (i.arr || i.bidx || i.dyn >= 0 || !lw.prog.code.empty()) return false;   // a variable index
        int id = (int)out.trans.size();
        out.trans.push_back({f.sig, i.sig, false, 0, i.hi >= 0 ? i.lo : 0, f.w});
        Instr t;
        t.k = IK::Tran;
        t.sig = id;
        lw.prog.emit(t);
        out.procs.push_back({std::move(lw.prog), false, "tran"});
        return true;
    }
    std::optional<Formal> portFormal(const slang::ast::PortSymbol& ps) {
        using namespace slang::ast;
        const Symbol* sym = ps.internalSymbol;
        if (!sym) {
            const Expression* in = ps.getInternalExpr();
            if (!in) return std::nullopt;
            const Expression& peeled = Lowerer::peel(*in);
            if (peeled.kind != ExpressionKind::NamedValue)
                Lowerer::unsupported("port internal connection");
            sym = &peeled.as<NamedValueExpression>().symbol;
        }
        auto it = sigmap.find(sym);
        if (it == sigmap.end())
            throw std::runtime_error("lower: port '" + std::string(ps.name) + "' is not a signal");
        int w = (int)ps.getType().getBitWidth();
        if (w <= 0 && (ps.getType().isUnpackedStruct() || ps.getType().isUnpackedUnion()))
            w = storeW(ps.getType());   // an unpacked struct port: one vector, as for a variable (7.2)
        if (w <= 0) Lowerer::unsupported("port width");
        return Formal{it->second, w, ps.getType().isSigned()};
    }

    // An unpacked array port (23.3.3.4 via 7.6): element by element, each
    // a continuous assignment, leftmost element to leftmost element.
    void connectArrayPort(const slang::ast::PortSymbol& ps, const slang::ast::Expression& ext) {
        using namespace slang::ast;
        const Symbol* sym = ps.internalSymbol;
        if (!sym) Lowerer::unsupported("array port with an internal expression");
        auto fit = arrMap().find(sym);
        if (fit == arrMap().end()) throw std::runtime_error("lower: array port '" + std::string(ps.name) + "' is not an array");
        const ArrShape& f = *fit->second;
        if (ps.direction == ArgumentDirection::In) {
            Lowerer lw(sigmap);
            prep(lw);
            std::vector<E> els;
            lw.arrayElems(ps.getType(), ext, els);
            if ((int)els.size() != f.count) Lowerer::unsupported("array port connection of another size");
            if (!lw.prog.code.empty()) Lowerer::unsupported("array port connection that needs statements");
            for (int k = 0; k < f.count; k++) {
                Lowerer L(sigmap);
                prep(L);
                Instr i; i.k = IK::Assign; i.sig = f.base + k; i.e = els[(size_t)k];
                finishAssign(L, i);
            }
            return;
        }
        if (ps.direction != ArgumentDirection::Out) Lowerer::unsupported("inout or ref array port");
        if (ext.kind != ExpressionKind::Assignment) Lowerer::unsupported("output port connection");
        const Expression& act = Lowerer::peel(ext.as<AssignmentExpression>().left());
        if (act.kind != ExpressionKind::NamedValue && act.kind != ExpressionKind::HierarchicalValue)
            Lowerer::unsupported("array output port connected to an expression");
        auto ait = arrMap().find(&act.as<ValueExpressionBase>().symbol);
        if (ait == arrMap().end() || ait->second->count != f.count) Lowerer::unsupported("array output port connection");
        for (int k = 0; k < f.count; k++) {
            Lowerer L(sigmap);
            prep(L);
            Instr i; i.k = IK::Assign; i.sig = ait->second->base + k; i.e = ref(f.base + k, f.sgn);
            finishAssign(L, i);
        }
    }

    void driveSig(int sig, const slang::ast::Expression& rhs) {
        Lowerer lw(sigmap);
        prep(lw);
        Instr i; i.k = IK::Assign; i.sig = sig; i.e = lw.expr(rhs);
        finishAssign(lw, i);
    }

    // Drive the parent's actual from the child's formal. `ext` is the
    // assignment slang built for an output or inout; its right-hand side is
    // an EmptyArgument standing in for that formal.
    void driveActual(const slang::ast::AssignmentExpression& ext, const Formal& formal) {
        Lowerer lw(sigmap);
        prep(lw);
        Lowerer::OutSlot slot{formal.sig, formal.w, formal.sgn};
        lw.outSlot = &slot;
        if (Lowerer::peel(ext.left()).kind == slang::ast::ExpressionKind::Concatenation) {
            // .q({a, b}): one driver per part, each its slice of the port (23.3.2).
            int w = (int)ext.left().type->getBitWidth();
            std::vector<std::tuple<const slang::ast::Expression*, int, int>> parts;
            Lowerer::concatParts(ext.left(), w - 1, parts);
            bool first = true;
            for (auto& [x, hi, lo] : parts) {
                Lowerer pw(sigmap);
                if (!first) { prep(pw); pw.outSlot = &slot; }
                Lowerer& L = first ? lw : pw;
                first = false;
                Instr i; i.k = IK::Assign;
                L.lhs(*x, i);
                i.e = slice(resize(L.expr(ext.right()), w, false, false), hi, lo);
                finishAssign(L, i);
            }
            return;
        }
        Instr i; i.k = IK::Assign;
        lw.lhs(ext.left(), i);
        i.e = lw.expr(ext.right());
        finishAssign(lw, i);
    }

    // A port of an element of a multidimensional instance array (23.3.3.5).
    // slang slices a packed actual N times the port width for each element,
    // but for two or more dimensions of different sizes it picks the wrong
    // part (it scales by the previous dimension's size). Here the part is
    // taken again from the whole actual: the left-most element (row-major,
    // left-most index of each dimension first) gets the most significant
    // bits. Returns false to let the normal path connect slang's expression.
    bool connectArrayElem(const slang::ast::InstanceSymbol& inst, const slang::ast::PortSymbol& ps,
                          const slang::ast::Expression& ext, const Formal& formal) {
        using namespace slang::ast;
        if (inst.arrayPath.size() < 2) return false;
        bool in = ps.direction == ArgumentDirection::In;
        if (!in && ps.direction != ArgumentDirection::Out) return false;
        if (!in && ext.kind != ExpressionKind::Assignment) return false;
        const Expression& act = in ? Lowerer::peel(ext) : ext.as<AssignmentExpression>().left();
        if (act.kind != ExpressionKind::RangeSelect) return false;
        const Expression& whole = act.as<RangeSelectExpression>().value();
        auto [pos, n] = instArrayPos(inst);
        int W = (int)whole.type->getBitWidth();
        if (whole.type->isUnpackedArray() || W != n * formal.w) return false;
        int hi = W - 1 - pos * formal.w, lo = hi - formal.w + 1;
        Lowerer lw(sigmap);
        prep(lw);
        Instr i; i.k = IK::Assign;
        if (in) {
            i.sig = formal.sig;
            i.e = slice(lw.expr(whole), hi, lo);
        } else {
            lw.lhs(whole, i);
            if (i.arr || i.bidx || i.dyn >= 0 || i.sig < 0) return false;
            int base = i.hi >= 0 ? i.lo : 0;
            i.lo = base + lo;
            i.hi = base + hi;
            i.e = ref(formal.sig, formal.sgn);
        }
        finishAssign(lw, i);
        return true;
    }

    void connectInstance(const slang::ast::InstanceSymbol& inst, const std::string& self) {
        using namespace slang::ast;
        if (inst.isTopLevel()) return;
        struct PortTag {
            ProcCollector& pc;
            ~PortTag() { pc.curPort.clear(); pc.curPortDir = -1; }
        } tag{*this};
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (conn && conn->port.kind == SymbolKind::Port) {
                const auto& ps0 = conn->port.as<PortSymbol>();
                curPort = self + "." + std::string(ps0.name);
                curPortDir = ps0.direction == ArgumentDirection::In ? 0 : ps0.direction == ArgumentDirection::Out ? 1 : 2;
            }
            if (conn && conn->port.kind == SymbolKind::InterfacePort) continue;   // b.x names the instance's own signal (25.3)
            if (!conn || conn->port.kind != SymbolKind::Port)
                Lowerer::unsupported("port kind");
            const auto& ps = conn->port.as<PortSymbol>();
            const Expression* ext = conn->getExpression();
            if (!ext && !ps.isNullPort && ps.direction == ArgumentDirection::In && ps.isNetPort() &&
                inst.getDefinition().unconnectedDrive != UnconnectedDrive::None) {
                // `unconnected_drive pull1 / pull0 (22.9): an unconnected input is pulled.
                if (auto formal = portFormal(ps)) {
                    bool one = inst.getDefinition().unconnectedDrive == UnconnectedDrive::Pull1;
                    std::vector<E> bits((size_t)formal->w, konst(mk(one ? 1 : 0, 1)));
                    Lowerer lw(sigmap);
                    prep(lw);
                    Instr i; i.k = IK::Assign; i.sig = formal->sig;
                    i.s0 = i.s1 = STR_PULL;
                    i.e = formal->w == 1 ? bits[0] : concat(bits);
                    finishAssign(lw, i);
                }
                continue;
            }
            if (!ext || ps.isNullPort || Lowerer::peel(*ext).kind == ExpressionKind::EmptyArgument) continue;   // u(a, ): left open (23.3.2)
            Lowerer::lastLoc() = ext->sourceRange.start().valid() ? ext->sourceRange.start() : inst.location;
            // A ref port or a container port joined to a whole variable shares
            // that variable's signal (see sharedPortVar).
            if (ps.internalSymbol && aliasOf().count(ps.internalSymbol) && aliasOf().at(ps.internalSymbol) != ps.internalSymbol &&
                (ps.internalSymbol->kind != SymbolKind::Net || Lowerer::isDynType(ps.getType()) ||
                 ps.internalSymbol->as<NetSymbol>().netType.netKind == NetType::UserDefined))
                continue;
            if (ps.direction == ArgumentDirection::Ref) Lowerer::unsupported("ref port connected to anything but a whole variable");
            if (ps.internalSymbol && Lowerer::isDynType(ps.getType()))
                Lowerer::unsupported("container port connected to anything but a whole container variable");
            if (ps.getType().getCanonicalType().kind == SymbolKind::FixedSizeUnpackedArrayType) {
                connectArrayPort(ps, *ext);
                continue;
            }
            auto formal = portFormal(ps);
            if (!formal) continue;
            if (connectArrayElem(inst, ps, *ext, *formal)) continue;
            // The links of a port to a whole net, for INTERCONNECT (32.4.4).
            auto inLink = [&](const Expression& x) {
                const Symbol* sym = Lowerer::peel(x).getSymbolReference();
                if (!sym || Lowerer::peel(x).kind == ExpressionKind::ElementSelect || Lowerer::peel(x).kind == ExpressionKind::RangeSelect) return;
                auto it = sigmap.find(sym);
                if (it != sigmap.end() && (int)x.type->getBitWidth() == formal->w) inLinks()[formal->sig] = it->second;
            };
            if (ps.direction == ArgumentDirection::In) {
                inLink(*ext);
                driveSig(formal->sig, *ext);
                continue;
            }
            if (ps.direction != ArgumentDirection::Out && ps.direction != ArgumentDirection::InOut)
                Lowerer::unsupported("ref port");
            // An open output with an initializer (`output logic q = 5`): slang
            // gives the initializer as the connection; nothing is connected.
            if (ext->kind != ExpressionKind::Assignment && ps.direction == ArgumentDirection::Out &&
                ext == ps.getInitializer())
                continue;
            if (ext->kind != ExpressionKind::Assignment)
                Lowerer::unsupported("output port connection");
            const auto& asg = ext->as<AssignmentExpression>();
            if (ps.direction == ArgumentDirection::InOut) {   // collapsed onto the net it connects to: nothing to drive
                const Expression& act = Lowerer::peel(asg.left());
                if (ValueExpressionBase::isKind(act.kind) && act.kind != ExpressionKind::ElementSelect &&
                    inoutCollapses(ps, act.as<ValueExpressionBase>().symbol) && aliasOf().count(ps.internalSymbol))
                    continue;
            }
            if (ps.direction == ArgumentDirection::InOut && inoutTran(ps, asg.left(), *formal)) continue;
            if (Lowerer::peel(asg.left()).kind != ExpressionKind::Concatenation) linkFormal = formal->sig;
            driveActual(asg, *formal);
            linkFormal = -1;
            if (ps.direction != ArgumentDirection::InOut) continue;
            inLink(asg.left());
            if (!ps.isNetPort()) Lowerer::unsupported("inout port that is not a net");
            if ((int)asg.left().type->getBitWidth() != formal->w)
                Lowerer::unsupported("inout width mismatch");
            driveSig(formal->sig, asg.left());
        }
    }

    // The instance being visited, named like signals (see the signal
    // naming visitor): "" for the top, "u_core", "gen[1].u".
    std::string curScope, curPort;
    int curPortDir = -1;
    static std::string inScope(const std::string& sc, const std::string& n) { return sc.empty() ? n : sc + "." + n; }
    // A checker instance (17.3): its body is lowered like an instance's,
    // with its processes in the reactive region set (17.7.3). Each output
    // formal is continuously assigned to its actual in the reactive region.
    int checkerDepth = 0;
    std::unordered_set<const slang::ast::Symbol*> freeAssigned;
    bool checkerAssumes = false;
    int procCheckerDepth = 0;   // inside a checker instantiated in procedural code (17.3)
    // A free checker variable (17.7): random from the start, unless it has
    // an initializer; an active one gets a new random value every time step.
    void freeVar(const slang::ast::VariableSymbol& v) {
        using namespace slang::ast;
        int w = storeW(v.getType());
        if (w > 64 || !v.getType().isIntegral()) Lowerer::unsupported("a free checker variable wider than 64 bits or not integral");
        bool isConst = v.flags.has(VariableFlags::Const);
        bool active = !isConst && !freeAssigned.count(&v);
        // With assume statements, active ones change only at the assume
        // set's clock, solved there (17.7.2), not in every time step.
        out.freeVars.push_back({sigmap.at(&v), active && !checkerAssumes});
    }
    void handle(const slang::ast::CheckerInstanceSymbol& ci) {
        using namespace slang::ast;
        // In procedural code (17.3) its static assertions are lowered where the
        // process reaches the instance (Lowerer::procCheckerAsserts); the rest
        // runs as for a static instance.
        bool procedural = ci.body.isProcedural;
        if (procedural) procCheckerDepth++;
        struct Leave { int& d; bool on; ~Leave() { if (on) d--; } } leave{procCheckerDepth, procedural};
        // Which variables the checker assigns, and whether it has assume
        // statements (17.7.2): an active free variable is one it never assigns.
        struct Scan : ASTVisitor<Scan, VisitFlags::Statements | VisitFlags::Expressions> {
            std::unordered_set<const Symbol*> assigned;
            bool assumes = false;
            void handle(const AssignmentExpression& a) {
                const Expression* l = &a.left();
                for (;;) {
                    if (l->kind == ExpressionKind::ElementSelect) l = &l->as<ElementSelectExpression>().value();
                    else if (l->kind == ExpressionKind::RangeSelect) l = &l->as<RangeSelectExpression>().value();
                    else if (l->kind == ExpressionKind::MemberAccess) l = &l->as<MemberAccessExpression>().value();
                    else break;
                }
                if (const Symbol* sym = l->getSymbolReference()) assigned.insert(sym);
                visitDefault(a);
            }
            void handle(const ConcurrentAssertionStatement& c) {
                if (c.assertionKind == AssertionKind::Assume) assumes = true;
                visitDefault(c);
            }
        } scan;
        ci.body.visit(scan);
        std::swap(freeAssigned, scan.assigned);
        bool savedAssumes = checkerAssumes;
        checkerAssumes = scan.assumes;
        // The active free variables of this checker (17.7.2): its assume set
        // solves for them. They are read as they are, also in assertions.
        Lowerer::FreeSolve fs;
        if (scan.assumes)
            for (const auto& v : ci.body.membersOfType<VariableSymbol>())
                if (v.flags.has(VariableFlags::CheckerFreeVariable) && !v.flags.has(VariableFlags::Const) &&
                    !freeAssigned.count(&v) && sigmap.count(&v)) {
                    fs.vars.push_back(sigmap.at(&v));
                    Lowerer::svaLiveSigs().insert(sigmap.at(&v));
                }
        fs.active = !fs.vars.empty();
        std::swap(Lowerer::freeSolve(), fs);
        checkerDepth++;
        visitDefault(ci);
        checkerDepth--;
        std::swap(Lowerer::freeSolve(), fs);
        checkerAssumes = savedAssumes;
        for (const auto& c : ci.getPortConnections()) {
            if (c.formal.kind != SymbolKind::FormalArgument) continue;
            const auto& f = c.formal.as<FormalArgumentSymbol>();
            if (f.direction != ArgumentDirection::Out) continue;
            int fs = sigmap.at(&f);
            int w = storeW(f.getType());
            if (const Expression* ie = c.getOutputInitialExpr()) {   // output int ctr = 0 (17.2)
                if (!init) { init = std::make_unique<Lowerer>(sigmap); prep(*init); }
                Instr i; i.k = IK::Assign; i.sig = fs;
                i.e = resize(init->expr(*ie), w, f.getType().isSigned(), ie->type->isSigned());
                init->prog.emit(i);
            }
            const auto* ap = std::get_if<const Expression*>(&c.actual);
            if (!ap || !*ap) continue;
            const Expression* a = *ap;
            if (a->kind == ExpressionKind::Assignment) a = &a->as<AssignmentExpression>().left();
            Lowerer lw(sigmap);
            prep(lw);
            lw.setScope(ci.getParentScope());
            Instr i; i.k = IK::Assign;
            lw.lhs(*a, i);
            int tw = storeW(*a->type);
            i.e = resize(ref(fs, f.getType().isSigned()), tw, f.getType().isSigned(), f.getType().isSigned());
            finishAssign(lw, i);
            out.procs.back().reactive = true;
        }
    }
    int program = -1;   // the program instance being visited (24.3), or -1
    void handle(const slang::ast::InstanceSymbol& inst) {
        if (!enterInstance(inst)) return;
        noteInstance(inst);
        std::string savedScope = curScope;
        std::string self = inst.isTopLevel() ? curScope : inScope(curScope, std::string(inst.name));
        connectInstance(inst, self);
        curScope = self;
        uint32_t savedInst = g_covInst;
        if (g_cov) g_covInst = covInstIdOf(inst);
        int saved = program;
        if (inst.getDefinition().definitionKind == slang::ast::DefinitionKind::Program) program = out.programs++;
        visitDefault(inst);
        program = saved;
        g_covInst = savedInst;
        curScope = std::move(savedScope);
    }
    // What SDF annotation (32) looks up: each instance's cell, and the
    // formal signal of each of its ports.
    void noteInstance(const slang::ast::InstanceSymbol& inst) {
        using namespace slang::ast;
        std::string path = inst.getHierarchicalPath();
        instCells()[path] = std::string(inst.getDefinition().name);
        for (const Symbol* p : inst.body.getPortList()) {
            if (p->kind != SymbolKind::Port) continue;
            const auto& ps = p->as<PortSymbol>();
            if (ps.isNullPort || !ps.internalSymbol) continue;
            auto it = sigmap.find(ps.internalSymbol);
            if (it == sigmap.end()) continue;
            PortRec r;
            r.sig = it->second;
            r.dir = ps.direction == ArgumentDirection::In ? 0 : ps.direction == ArgumentDirection::Out ? 1 : 2;
            r.net = ps.internalSymbol->kind == SymbolKind::Net;
            portRecs()[path + "." + std::string(ps.name)] = r;
        }
    }
    void handle(const slang::ast::GenerateBlockSymbol& g) {
        if (g.isUninstantiated) return;
        std::string saved = curScope;
        const auto* parent = g.getParentScope() ? &g.getParentScope()->asSymbol() : nullptr;
        if (parent && parent->kind == slang::ast::SymbolKind::GenerateBlockArray) {
            std::string an = parent->name.empty() ? std::string("genblk") : std::string(parent->name);
            curScope = inScope(curScope, an + "[" + (g.getArrayIndex() ? g.getArrayIndex()->toString() : std::string("?")) + "]");
        } else {
            curScope = inScope(curScope, g.getExternalName());
        }
        visitDefault(g);
        curScope = std::move(saved);
    }
    void handle(const slang::ast::CovergroupType&) {}   // built when used (lower_cover.inc)
    void handle(const slang::ast::ClockingBlockSymbol& cb) {   // 14: runs on its clocking event
        Lowerer lw(sigmap);
        prep(lw);
        lw.setScope(cb.getParentScope());
        lw.clockingIdx(cb);
    }
    // The clock a procedure infers for its sampled value functions and
    // assertions (16.14.6): an event control at its start.
    static const slang::ast::TimingControl* leadingEvent(const slang::ast::Statement& st) {
        using namespace slang::ast;
        if (st.kind == StatementKind::Timed) {
            const auto& t = st.as<TimedStatement>().timing;
            if (t.kind == TimingControlKind::SignalEvent || t.kind == TimingControlKind::EventList) return &t;
            return nullptr;
        }
        if (st.kind == StatementKind::Block && st.as<BlockStatement>().blockKind == StatementBlockKind::Sequential)
            return leadingEvent(st.as<BlockStatement>().body);
        if (st.kind == StatementKind::List && !st.as<StatementList>().list.empty())
            return leadingEvent(*st.as<StatementList>().list[0]);
        return nullptr;
    }
    // An assertion written as a module item (16.4.3, 16.14.5). slang wraps it
    // in a procedure marked isFromAssertion.
    void assertionItem(const slang::ast::ProceduralBlockSymbol& p) {
        using namespace slang::ast;
        const Statement* st = &p.getBody();
        if (st->kind == StatementKind::Block && st->as<BlockStatement>().blockKind == StatementBlockKind::Sequential)
            st = &st->as<BlockStatement>().body;
        Lowerer lw(sigmap);
        prep(lw);
        lw.setScope(p.getParentScope());
        if (st->kind == StatementKind::ConcurrentAssertion) {   // runs on its clock, not as a process
            lw.staticAssert = true;
            lw.stmt(p.getBody());
            return;
        }
        if (st->kind != StatementKind::ImmediateAssertion) Lowerer::unsupported("this assertion item");
        // A static deferred assertion acts as if in an always_comb (16.4.3).
        lw.stmt(p.getBody());
        auto rs = readSet(lw.prog);
        bool loops = !rs.empty();
        if (loops) {
            lw.prog.emit(waitInstr(rs));
            Instr j; j.k = IK::Jmp; j.target = 0; lw.prog.emit(j);
        }
        out.procs.push_back({std::move(lw.prog), loops, "always_comb", curScope});
    }
    // -xprop configuration file: whether the instance around p is on.
    static bool xpropHere(const slang::ast::Symbol& p) {
        if (Lowerer::xpRules().empty()) return true;
        const auto* inst = instanceAround(p.getParentScope());
        if (!inst) return true;
        return Lowerer::xpConfigOn(inst->getHierarchicalPath(), inst->getDefinition().name);
    }
    void handle(const slang::ast::ProceduralBlockSymbol& p) {
        using slang::ast::ProceduralBlockKind;
        if (p.isFromAssertion) {
            if (procCheckerDepth == 0) assertionItem(p);   // else queued by the procedure (17.3)
            return;
        }
        // A final block (9.2.3) runs once when simulation ends. Lowering it as
        // a process would run it at time 0 and, looping, forever.
        Lowerer lw(sigmap);
        prep(lw);
        lw.condCov = true;
        lw.setScope(p.getParentScope());
        if (p.procedureKind != ProceduralBlockKind::Initial && p.procedureKind != ProceduralBlockKind::Final)
            if (const auto* t = leadingEvent(p.getBody())) {
                try { E iff; lw.clockOf(*t, lw.sampleEv, iff); } catch (const std::runtime_error&) { lw.sampleEv.clear(); }
            }
        int below = checkerDepth > 0 && p.procedureKind == ProceduralBlockKind::AlwaysFF ? lw.fresh(1) : -1;
        if (Lowerer::xpropMode() && checkerDepth == 0 && program < 0 && lw.frames.empty() &&
            p.procedureKind != ProceduralBlockKind::Initial && p.procedureKind != ProceduralBlockKind::Final &&
            xpropHere(p))
            lw.xpProc(p.getBody());   // -xprop
        else
            lw.stmt(p.getBody());
        lw.finishImplicit();
        // 17.5: in a checker's always_ff, every expression but the event
        // control reads sampled values. Signals made while lowering the body
        // (temporaries) are read as they are.
        if (below >= 0) lw.sampleProg(below);
        if (p.procedureKind == ProceduralBlockKind::Initial)
            if (const auto* nb = Lowerer::namedSeq(p.getBody())) lw.prog.roots.push_back(Lowerer::rootId(nb));
        // A final block (9.2.3) runs once, after the last time slot. slang
        // already rejects delays and event waits in it.
        if (p.procedureKind == ProceduralBlockKind::Final) {
            out.finals.push_back({std::move(lw.prog), false, "final"});
            return;
        }
        bool loops = p.procedureKind != ProceduralBlockKind::Initial;
        if (p.procedureKind == ProceduralBlockKind::AlwaysComb ||
            p.procedureKind == ProceduralBlockKind::AlwaysLatch) {
            // runs once at t=0, then re-runs whenever any signal it reads,
            // and does not itself write, changes (9.2.2.2.1)
            auto rs = combReadSet(lw.prog);
            if (rs.empty()) loops = false;
            else lw.prog.emit(waitInstr(rs));
        }
        if (loops) { Instr j; j.k = IK::Jmp; j.target = 0; lw.prog.emit(j); }  // repeat forever
        if (loops && p.procedureKind == ProceduralBlockKind::Always && !lw.prog.code.empty() &&
            lw.prog.code[0].k == IK::WaitEdge)
            lw.prog.alwaysTop = true;
        out.procs.push_back({std::move(lw.prog), loops, std::string(slang::ast::toString(p.procedureKind)),
                             curScope, {}, -1,
                             p.procedureKind == ProceduralBlockKind::Initial ? program : -1, checkerDepth > 0});
    }
};


// @* lowered inside a subroutine is a WaitEdge with no events yet. Patch it
// after every callee body is installed, so the wait sees those reads too.
inline void patchImplicit(Prog& prog) {
    std::vector<size_t> slots;
    for (size_t pc = 0; pc < prog.code.size(); pc++) {
        const Instr& i = prog.code[pc];
        if (i.k == IK::WaitEdge && i.events.empty() && i.sig < 0 && !i.dynWait) slots.push_back(pc);
    }
    if (slots.empty()) return;
    auto rs = readSet(prog);
    if (rs.empty()) Lowerer::unsupported("@* in a block that reads no signals");
    for (size_t pc : slots) prog.code[pc] = waitInstr(rs);
}

// The automatic subroutines that need a frame per call: tasks (they can
// wait, so two calls can be live at once), class methods, and functions
// that recurse or fork. Any other automatic function runs one call at a
// time and to completion, so it keeps static signals: that is the same
// behavior (its locals are set again on every call) and -compile handles it.
inline std::unordered_set<const slang::ast::SubroutineSymbol*>
framedSubroutines(const std::vector<const slang::ast::SubroutineSymbol*>& all) {
    using namespace slang::ast;
    struct Scan : ASTVisitor<Scan, VisitFlags::Statements | VisitFlags::Expressions> {
        std::vector<const SubroutineSymbol*> callees;
        bool forks = false;
        void handle(const CallExpression& c) {
            if (!c.isSystemCall())
                if (auto* s = std::get<const SubroutineSymbol*>(c.subroutine)) callees.push_back(s);
            visitDefault(c);
        }
        void handle(const BlockStatement& b) {
            if (b.blockKind != StatementBlockKind::Sequential) forks = true;
            visitDefault(b);
        }
    };
    std::unordered_map<const SubroutineSymbol*, std::vector<const SubroutineSymbol*>> calls;
    std::unordered_set<const SubroutineSymbol*> out;
    for (auto* s : all) {
        Scan sc;
        s->getBody().visit(sc);
        calls[s] = std::move(sc.callees);
        if (Lowerer::isDynType(s->getReturnType())) { out.insert(s); continue; }   // the result is a frame container
        if (!isAutomatic(*s)) continue;
        if (s->subroutineKind == SubroutineKind::Task || isClassMethod(*s) || sc.forks) out.insert(s);
    }
    // Automatic functions on a call cycle (13.4.2).
    for (auto* s : all) {
        if (!isAutomatic(*s) || out.count(s)) continue;
        std::vector<const SubroutineSymbol*> todo(calls[s].begin(), calls[s].end());
        std::unordered_set<const SubroutineSymbol*> seen;
        while (!todo.empty()) {
            auto* c = todo.back();
            todo.pop_back();
            if (c == s) { out.insert(s); break; }
            if (!seen.insert(c).second) continue;
            for (auto* d : calls[c]) todo.push_back(d);
        }
    }
    return out;
}

inline void rejectRecursive(const std::vector<Subr*>& subs) {
    enum class Color { White, Gray, Black };
    std::unordered_map<const Subr*, Color> color;
    for (Subr* s : subs) color[s] = Color::White;
    auto dfs = [&](auto& self, Subr* s) -> void {
        color[s] = Color::Gray;
        for (Subr* c : s->calls) {
            if (c->lay) continue;   // an automatic subroutine may recurse (13.4.2)
            if (color[c] == Color::Gray) Lowerer::unsupported("recursive static subroutine");
            if (color[c] == Color::White) self(self, c);
        }
        color[s] = Color::Black;
    };
    for (Subr* s : subs)
        if (color[s] == Color::White) dfs(dfs, s);
}

// Object shape, constructor and virtual method table of every class,
// base classes first (8.13). Called after every subroutine has its Subr.
inline ClassInfo* buildClass(const slang::ast::ClassType& ct,
                             std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>& subMap,
                             LowerResult& out, std::vector<std::unique_ptr<Subr>>& owned, std::vector<Subr*>& order) {
    using namespace slang::ast;
    if (auto it = classMap().find(&ct); it != classMap().end()) return it->second;
    auto method = [&](const Symbol& m) -> const SubroutineSymbol* {
        if (m.kind == SymbolKind::Subroutine) return &m.as<SubroutineSymbol>();
        if (m.kind == SymbolKind::MethodPrototype) return m.as<MethodPrototypeSymbol>().getSubroutine();
        return nullptr;
    };
    if (ct.isInterface) {   // 8.26: a type to cast to and call through; no objects of its own
        classStore().push_back(std::make_unique<ClassInfo>());
        ClassInfo* ii = classStore().back().get();
        classMap()[&ct] = ii;
        ii->ty = &ct;
        ii->id = (int)classRt().size();
        classRt().push_back({});
        classRt().back().name = std::string(ct.name);
        for (const Type* t : ct.getImplementedInterfaces())
            classRt()[(size_t)ii->id].ifs.push_back(buildClass(t->getCanonicalType().as<ClassType>(), subMap, out, owned, order)->id);
        for (const auto& m : ct.members())
            if (const SubroutineSymbol* sub = method(m); sub && !ifaceSlotMap().count(sub))
                ifaceSlotMap().emplace(sub, (int)ifaceSlotMap().size());
        return ii;
    }
    ClassInfo* base = nullptr;
    if (const Type* bt = ct.getBaseClass()) {
        if (!bt->getCanonicalType().isClass()) Lowerer::unsupported("base class of this kind");
        base = buildClass(bt->getCanonicalType().as<ClassType>(), subMap, out, owned, order);
    }
    std::vector<ClassInfo*> ifaces;
    for (const Type* t : ct.getImplementedInterfaces())
        ifaces.push_back(buildClass(t->getCanonicalType().as<ClassType>(), subMap, out, owned, order));
    classStore().push_back(std::make_unique<ClassInfo>());
    ClassInfo* ci = classStore().back().get();
    classMap()[&ct] = ci;
    ci->ty = &ct;
    ci->base = base;
    ci->id = (int)classRt().size();
    classRt().push_back({});
    classRt().back().name = std::string(ct.name);
    classRt().back().base = base ? base->id : -1;
    ci->lay = newLayout("class " + std::string(ct.name));
    if (base) {
        *ci->lay = *base->lay;
        ci->lay->pool.clear();
        ci->lay->name = "class " + std::string(ct.name);
        ci->lay->init[0] = make((uint64_t)ci->id, 0, 32);
        classRt().back().vt = classRt()[(size_t)base->id].vt;
        classRt().back().imap = classRt()[(size_t)base->id].imap;
    } else {
        ci->lay->add(32, make((uint64_t)ci->id, 0, 32));
    }
    classRt().back().lay = ci->lay;
    switch (ci->builtin = builtinClassKind(ct)) {
        case 1: case 4: ci->bslot = ci->lay->add(64, make(0, 0, 64)); break;
        case 2:
            ci->bslot = ci->lay->add(32, make(0, 0, 32, 0, true));
            ci->bslot2 = ci->lay->add(32, make(0, 0, 32));
            break;
        case 3: {
            const Type& t = ct.find("T")->as<TypeParameterSymbol>().targetType.getType();
            DynObj o;
            o.kind = 1;
            Lowerer::elemInto(o, t);
            ci->bslot = ci->lay->add(64, make(0, 0, 64));
            ci->lay->dyn.push_back({ci->bslot, std::move(o)});
            ci->bslot2 = ci->lay->add(32, make(0, 0, 32, 0, true));
            ci->bslot3 = ci->lay->add(32, make(0, 0, 32));
            break;
        }
        default: break;
    }
    for (const auto& m : ct.members()) {
        if (m.kind == SymbolKind::ClassProperty) {
            const auto& p = m.as<ClassPropertySymbol>();
            if (p.lifetime == VariableLifetime::Static) continue;
            propMap()[&p] = Lowerer::declSlot(p.getType(), ci->lay);
            // An embedded covergroup's handle is null in a shallow copy (8.12, 19.4).
            if (const Type& pt = p.getType().getCanonicalType(); pt.kind == SymbolKind::CovergroupType &&
                pt.as<CovergroupType>().getParentScope() == static_cast<const Scope*>(&ct))
                ci->lay->noCopy.push_back(propMap()[&p].off);
            if (p.randMode != RandMode::None) randModeMap()[&p] = ci->lay->add(1, mk(1));
            continue;
        }
        if (m.kind == SymbolKind::ConstraintBlock) {
            if (!m.as<ConstraintBlockSymbol>().flags.has(ConstraintBlockFlags::Static))
                consModeMap()[&m] = ci->lay->add(1, mk(1));
            continue;
        }
        const SubroutineSymbol* sub = method(m);
        if (!sub) continue;
        if (sub->flags.has(MethodFlags::Constructor)) {
            auto it = subMap.find(sub);
            if (it == subMap.end()) Lowerer::unsupported("constructor");
            ci->ctor = it->second;
            ci->ctor->cls = ci;
            continue;
        }
        if (sub->flags.has(MethodFlags::Static)) continue;
        int slot = -1;
        if (const SubroutineSymbol* ov = sub->getOverride()) {
            auto vs = vslotMap().find(ov);
            if (vs != vslotMap().end()) slot = vs->second;
        }
        // An override of a virtual method a base class declares, also through
        // extern prototypes (8.20, 8.24): the nearest base method of that name.
        for (ClassInfo* b = base; slot < 0 && b; b = b->base) {
            const Symbol* f = b->ty->find(sub->name);
            if (!f || f->getParentScope() != b->ty) continue;
            if (const SubroutineSymbol* bs = method(*f)) {
                auto vs = vslotMap().find(bs);
                if (vs != vslotMap().end()) slot = vs->second;
                break;
            }
        }
        bool isVirt = sub->isVirtual() || slot >= 0 ||
                      (m.kind == SymbolKind::MethodPrototype && m.as<MethodPrototypeSymbol>().flags.has(MethodFlags::Virtual));
        if (!isVirt) continue;
        auto& vt = classRt()[(size_t)ci->id].vt;
        if (slot < 0) { slot = (int)vt.size(); vt.push_back({nullptr, nullptr}); }
        vslotMap()[sub] = slot;
        if (!sub->flags.has(MethodFlags::Pure)) {
            auto it = subMap.find(sub);
            if (it == subMap.end()) throw std::runtime_error("lower: unsupported virtual method " + std::string(ct.name) + "::" + std::string(sub->name) + (Lowerer::skippedSubs().count(sub) ? " (" + Lowerer::skippedSubs()[sub] + ")" : ""));
            vt[(size_t)slot] = {it->second->prog, it->second->lay};
        }
    }
    // Interface methods (8.26): each maps to the vt slot of the method of
    // the same name in this class or a base class.
    for (ClassInfo* ii : ifaces) {
        auto& cr = classRt()[(size_t)ci->id];
        cr.ifs.push_back(ii->id);
        for (const auto& m : ii->ty->members()) {
            const SubroutineSymbol* isub = method(m);
            if (!isub) continue;
            int k = ifaceSlotMap().at(isub);
            const SubroutineSymbol* impl = nullptr;
            for (const ClassInfo* c = ci; c && !impl; c = c->base)
                if (const Symbol* f = c->ty->find(isub->name)) impl = method(*f);
            if (!impl) continue;   // a virtual class may leave it to a subclass
            int slot = -1;
            if (auto vs = vslotMap().find(impl); vs != vslotMap().end()) slot = vs->second;
            else {
                auto it = subMap.find(impl);
                if (it == subMap.end() || impl->flags.has(MethodFlags::Pure)) continue;
                slot = (int)cr.vt.size();
                cr.vt.push_back({it->second->prog, it->second->lay});
            }
            if (cr.imap.size() <= (size_t)k) cr.imap.resize((size_t)k + 1, -1);
            cr.imap[(size_t)k] = slot;
        }
    }
    if (!ci->ctor) {   // no `new`: an implicit constructor with no arguments (8.7)
        auto sub = std::make_unique<Subr>();
        out.subs.push_back(std::make_unique<Prog>());
        sub->prog = out.subs.back().get();
        sub->lay = newLayout("new of " + std::string(ct.name));
        sub->cls = ci;
        ci->ctor = sub.get();
        order.push_back(sub.get());
        owned.push_back(std::move(sub));
    }
    return ci;
}
inline void buildClasses(const std::vector<const slang::ast::ClassType*>& cs,
                         std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*>& subMap,
                         LowerResult& out, std::vector<std::unique_ptr<Subr>>& owned, std::vector<Subr*>& order) {
    for (const auto* c : cs) buildClass(*c, subMap, out, owned, order);
}
// Default sampled values (16.5.1): at time 0, right after the declaration
// initializers ran, every shadow takes its signal's value and every $past
// stage the sampled value of its expression.
inline void sampledInit(LowerResult& out) {
    if (shadowPairs().empty() && sampleClocks().empty()) return;
    Prog p;
    for (auto [src, sh] : shadowPairs()) {
        Instr i; i.k = IK::Assign; i.sig = sh; i.e = ref(src);
        p.emit(i);
    }
    for (auto& c : sampleClocks())
        for (auto& ln : c.lines)
            for (int st : ln.stages) {
                Instr i; i.k = IK::Assign; i.sig = st; i.e = ln.src;
                p.emit(i);
            }
    size_t at = 0;
    for (size_t k = 0; k < out.procs.size(); k++)
        if (out.procs[k].kind == "init") at = k + 1;
    out.procs.insert(out.procs.begin() + (ptrdiff_t)at, {std::move(p), false, "sampled"});
}

// SDF LABELs (32.4.3) before lowering: the $sdf_annotate calls whose
// arguments are known and msim -sdf are read here only for their LABEL
// entries, which set labelled() so that the specparams' new values are
// used wherever they are read. The rest of each file is annotated before
// time 0 (sdf_annotate.inc).
inline void sdfLabels(slang::ast::Compilation& comp) {
    using namespace slang::ast;
    labelled().clear();
    labelledNames().clear();
    std::vector<SdfRequest> reqs;
    for (auto& r : sdfRequests()) if (r.cli) reqs.push_back(r);
    std::map<std::string, const InstanceSymbol*> insts;
    struct Scan : ASTVisitor<Scan, VisitFlags::Statements | VisitFlags::Expressions> {
        std::vector<SdfRequest>* reqs;
        std::map<std::string, const InstanceSymbol*>* insts;
        void handle(const InstanceSymbol& i) {
            if (i.body.flags.has(InstanceFlags::Uninstantiated)) return;
            (*insts)[i.getHierarchicalPath()] = &i;
            visitDefault(i);
        }
        void handle(const CallExpression& c) {
            if (c.isSystemCall() && c.getSubroutineName() == "$sdf_annotate") {
                std::string f[7];
                bool known[7];
                try { Lowerer::sdfArgs(c, f, known); } catch (const std::exception&) { return; }
                bool all = true;
                for (bool k : known) all &= k;
                if (all) reqs->push_back({f[0], f[1], f[2], f[3], f[4], f[5], f[6], ""});
            }
            visitDefault(c);
        }
    } scan;
    scan.reqs = &reqs;
    scan.insts = &insts;
    comp.getRoot().visit(scan);
    std::map<std::string, double> vals;   // current value of each LABEL'd specparam, in its module's unit
    std::map<std::string, const SpecparamSymbol*> syms;
    for (auto& r : reqs) {
        sdf::File f;
        sdf::Error err;
        if (!sdf::read(r.file, f, err)) continue;   // reported when annotating
        sdf::Sel sel;
        sel.mtm = mtmSelect();
        std::vector<std::string> warn;
        if (!r.config.empty()) sdf::readConfig(sel, r.config, mtmSelect(), warn);
        if (!r.mtm.empty()) sdf::setMtm(sel, r.mtm, mtmSelect());
        if (!r.scale.empty()) sdf::setScale(sel, r.scale);
        if (!r.scaleType.empty()) sdf::setScaleType(sel, r.scaleType);
        for (const sdf::Op& op : f.ops) {
            if (op.kind != sdf::LABEL || op.vals.size() != 1) continue;
            std::vector<const InstanceSymbol*> where;
            if (op.wild) {
                for (auto& [p, i] : insts)
                    if ((p == r.scope || p.rfind(r.scope + ".", 0) == 0) && i->getDefinition().name == op.cellType)
                        where.push_back(i);
            } else {
                std::string p = r.scope;
                for (auto& n : sdf::splitPath(op.inst)) p += "." + n;
                if (auto it = insts.find(p); it != insts.end()) where.push_back(it->second);
            }
            auto v = sdf::pick(op.vals[0].d, sel);
            if (!v) continue;
            for (const InstanceSymbol* i : where) {
                const Symbol* sym = i->body.find(sdf::unescape(op.label));
                if (!sym || sym->kind != SymbolKind::Specparam) continue;
                const auto& sp = sym->as<SpecparamSymbol>();
                int unit = -9;
                if (auto ts = i->body.getTimeScale()) unit = tsExp(ts->base);
                double units = *v * f.timescale / std::pow(10.0, unit);
                std::string name = i->getHierarchicalPath() + "." + std::string(sp.name);
                if (!vals.count(name)) {
                    const auto& cv = sp.getValue();
                    vals[name] = cv.isReal() ? (double)cv.real() : cv.isInteger() && !cv.integer().hasUnknown()
                                 ? (double)*cv.integer().as<int64_t>() : 0.0;
                }
                vals[name] = op.incr ? vals[name] + units : units;
                syms[name] = &sp;
            }
        }
    }
    for (auto& [name, v] : vals) {
        labelled()[syms[name]] = v;
        labelledNames()[name] = v;
    }
}

inline LowerResult lower(slang::ast::Compilation& comp, MkSig mkSig,
                         std::function<int(int)> newDriver) {
    std::unordered_map<const slang::ast::Symbol*, int> sigmap;
    LowerResult out;
    asserts().clear();
    concAsserts().clear();
    timingChecks().clear();
    sva::store().clear();
    shadowPairs().clear();
    sampleClocks().clear();
    clockings().clear();
    Lowerer::clockingIdxMap().clear();
    cgDefs().clear();
    cgBlockDefs().clear();
    Lowerer::blockEventIds().clear();
    cgInsts().clear();
    Lowerer::cgIdxMap().clear();
    Lowerer::seqTrackers().clear();
    Lowerer::svaLocalSigs().clear();
    Lowerer::svaLiveSigs().clear();
    sigInfo().clear();
    Lowerer::vpiSites().clear();
    Lowerer::shadowMap().clear();
    Lowerer::shadowArrs().clear();
    Lowerer::xpReport().clear();
    Lowerer::compilation() = &comp;
    pathRecs().clear();
    portRecs().clear();
    instCells().clear();
    gateRecs().clear();
    assignGates().clear();
    pulseStyles().clear();
    outLinks().clear();
    inLinks().clear();
    std::erase_if(sdfRequests(), [](const SdfRequest& r) { return !r.cli; });
    sdfAtRunTime() = false;
    sdfLabels(comp);
    {   // remember every signal's name and width, for sampled-value shadows (16.5.1)
        MkSig inner = std::move(mkSig);
        mkSig = [inner](const std::string& n, int w, bool net) {
            int id = inner(n, w, net);
            if ((size_t)id >= sigInfo().size()) sigInfo().resize(id + 1);
            sigInfo()[id] = {n, w};
            return id;
        };
    }
    arrMap().clear();
    localMap().clear();
    localArr().clear();
    localDyn().clear();
    refFormals().clear();
    classMap().clear();
    classStore().clear();
    propMap().clear();
    vslotMap().clear();
    ifaceSlotMap().clear();
    randModeMap().clear();
    consModeMap().clear();
    staticConsMode().clear();
    classRt().clear();
    layoutStore() = &out.layouts;
    netKinds().clear();
    subrLocalSigs().clear();
    usesForce() = false;
    srcMgr() = comp.getSourceManager();
    dynMap().clear();
    dynStore().clear();
    hiddenSigs().clear();
    loopVarSigs().clear();
    dynPending().clear();
    dynPendingAt().clear();
    blockIds() = BlockIds{};
    anySpans() = false;
    {   // The blocks and tasks a disable names (9.6.2), before any code is lowered.
        struct DisTargets : slang::ast::ASTVisitor<DisTargets, slang::ast::VisitFlags::Symbols | slang::ast::VisitFlags::Statements> {
            void handle(const slang::ast::DisableStatement& d) {
                if (d.target.kind == slang::ast::ExpressionKind::ArbitrarySymbol)
                    blockIds().targets.insert(d.target.as<slang::ast::ArbitrarySymbolExpression>().symbol);
            }
        } dt;
        comp.getRoot().visit(dt);
    }
    Lowerer::covSM() = comp.getSourceManager();   // statement locations, for coverage and run-time messages
    condDefs().clear();
    g_covInsts.assign(1, {});
    g_covInst = 0;
    g_condHits.clear();
    fsmDefs().clear();
    {   // alias statements (10.11), before any signal is made
        aliasOf().clear();
        interconnectW().clear();
        struct AliasFinder : slang::ast::ASTVisitor<AliasFinder> {
            std::unordered_map<const slang::ast::Symbol*, const slang::ast::Symbol*> up;
            const slang::ast::Symbol* root(const slang::ast::Symbol* s) {
                while (up.count(s) && up[s] != s) s = up[s];
                return s;
            }
            void handle(const slang::ast::InstanceSymbol& inst) {
                using namespace slang::ast;
                if (!enterInstance(inst)) return;
                // An interconnect net (6.6.8) takes its width from the ports
                // it connects: the widest of them.
                for (const PortConnection* c : inst.getPortConnections()) {
                    if (!c || c->port.kind != SymbolKind::Port) continue;
                    const Expression* x = c->getExpression();
                    if (!x) continue;
                    const Expression* y = &Lowerer::peel(*x);
                    if (y->kind == ExpressionKind::Assignment) y = &Lowerer::peel(y->as<AssignmentExpression>().left());
                    if (y->kind != ExpressionKind::NamedValue && y->kind != ExpressionKind::HierarchicalValue) continue;
                    const Symbol& ns = y->as<ValueExpressionBase>().symbol;
                    // An inout port joined to a whole net is that net (23.3.3.1):
                    // one signal, as with alias. Two continuous assignments, one
                    // each way, would hold the last driven value for ever.
                    const Symbol* in = inoutCollapses(c->port.as<PortSymbol>(), ns);
                    if (!in) in = sharedPortVar(c->port.as<PortSymbol>(), ns);
                    if (!in) in = nettypeCollapses(c->port.as<PortSymbol>(), ns);
                    if (in) {
                        if (!up.count(&ns)) up[&ns] = &ns;
                        if (!up.count(in)) up[in] = in;
                        const Symbol* ra = root(&ns), *rb = root(in);
                        if (ra != rb) up[rb] = ra;
                    }
                    if (ns.kind != SymbolKind::Net || ns.as<NetSymbol>().netType.netKind != NetType::Interconnect) continue;
                    int w = (int)c->port.as<PortSymbol>().getType().getBitWidth();
                    int& cur = interconnectW()[&ns];
                    cur = std::max(cur, w);
                }
                visitDefault(inst);
            }
            void handle(const slang::ast::NetAliasSymbol& a) {
                using namespace slang::ast;
                const Symbol* first = nullptr;
                for (const Expression* e : a.getNetReferences()) {
                    const Expression& x = Lowerer::peel(*e);
                    if (x.kind != ExpressionKind::NamedValue && x.kind != ExpressionKind::HierarchicalValue)
                        Lowerer::unsupported("alias of part of a net");
                    const Symbol* s = &x.as<ValueExpressionBase>().symbol;
                    if (!up.count(s)) up[s] = s;
                    if (!first) { first = s; continue; }
                    const Symbol* ra = root(first), *rb = root(s);
                    if (ra != rb) up[rb] = ra;
                }
            }
        } af;
        comp.getRoot().visit(af);
        assignedEvents().clear();
        eventPtr().clear();
        scopeTargets().clear();
        directiveMarks().clear();
        curScopeSig() = -1;
        annexDUsed() = false;
        scopeDefault() = comp.getRoot().topInstances.empty() ? nullptr : comp.getRoot().topInstances[0];
        struct EventAssignFinder : slang::ast::ASTVisitor<EventAssignFinder,
                                       slang::ast::VisitFlags::Statements | slang::ast::VisitFlags::Expressions> {
            void handle(const slang::ast::AssignmentExpression& a) {
                using namespace slang::ast;
                if (a.left().type->isEvent()) {
                    const Expression& l = Lowerer::peel(a.left());
                    if (ValueExpressionBase::isKind(l.kind)) assignedEvents().insert(&l.as<ValueExpressionBase>().symbol);
                }
                visitDefault(a);
            }
            void handle(const slang::ast::VariableSymbol& v) {
                if (v.getType().isEvent() && v.getInitializer()) assignedEvents().insert(&v);
                visitDefault(v);
            }
            void handle(const slang::ast::CallExpression& c) {   // $scope targets (D.11), known before any $showvars is lowered
                using namespace slang::ast;
                if (c.isSystemCall() && c.getSubroutineName() == "$scope" && c.arguments().size() == 1) {
                    const Expression& x = *c.arguments()[0];
                    if (x.kind == ExpressionKind::ArbitrarySymbol) {
                        const Symbol* t = x.as<ArbitrarySymbolExpression>().symbol;
                        auto& v = scopeTargets();
                        if (std::find(v.begin(), v.end(), t) == v.end()) v.push_back(t);
                    }
                }
                visitDefault(c);
            }
        } eaf;
        comp.getRoot().visit(eaf);
        for (auto& [s, p] : af.up) aliasOf()[s] = af.root(s);
    }
    SigCollector sc(mkSig, sigmap);
    comp.getRoot().visit(sc);
    sc.finishAliases();
    sc.aliasModports();
    out.netKinds = netKinds();

    struct Finder : slang::ast::ASTVisitor<Finder> {
        std::vector<const slang::ast::SubroutineSymbol*> found;
        int prec = 1000;
        void handle(const slang::ast::InstanceSymbol& inst) {
            if (!enterInstance(inst)) return;
            if (auto ts = inst.body.getTimeScale()) prec = std::min(prec, tsExp(ts->precision));
            visitDefault(inst);
        }
        void handle(const slang::ast::SubroutineSymbol& s) { found.push_back(&s); }
        std::vector<const slang::ast::ClassType*> classes;
        void handle(const slang::ast::ClassType& c) { classes.push_back(&c); visitDefault(c); }
        void handle(const slang::ast::CovergroupType&) {}   // its methods are built in (19.8)
        void handle(const slang::ast::GenerateBlockSymbol& g) {
            if (!g.isUninstantiated) visitDefault(g);
        }
    };
    Finder finder;
    comp.getRoot().visit(finder);
    // Built-in classes of the std package that are used (8.30).
    for (const auto& m : comp.getStdPackage().members())
        if (m.kind == slang::ast::SymbolKind::GenericClassDef && (m.name == "weak_reference" || m.name == "mailbox"))
            for (const auto& spec : m.as<slang::ast::GenericClassDefSymbol>().specializations())
                spec.visit(finder);
        else if (m.kind == slang::ast::SymbolKind::ClassType && (m.name == "semaphore" || m.name == "process"))
            m.visit(finder);
    // A task an interface declares extern and a module exports (25.7): the
    // module's body, which is not a member of any scope.
    for (size_t k = 0, n = finder.found.size(); k < n; k++)
        if (finder.found[k]->flags.has(slang::ast::MethodFlags::InterfaceExtern))
            if (const auto* pr = finder.found[k]->getPrototype())
                for (auto* x = pr->getFirstExternImpl(); x; x = x->getNextImpl())
                    if (std::find(finder.found.begin(), finder.found.end(), x->impl.get()) == finder.found.end())
                        finder.found.push_back(x->impl.get());
    // Every interface instance gets its virtual interface number (25.9).
    {
        struct IfInst : slang::ast::ASTVisitor<IfInst> {
            void handle(const slang::ast::InstanceSymbol& i) {
                if (i.isInterface()) vifInstance(i);
                visitDefault(i);
            }
            void handle(const slang::ast::GenerateBlockSymbol& g) { if (!g.isUninstantiated) visitDefault(g); }
        } ifi;
        comp.getRoot().visit(ifi);
    }
    // The simulation precision is the finest precision of any module (3.14.3).
    simPrecExp() = finder.prec == 1000 ? -9 : finder.prec;
    out.precExp = simPrecExp();

    std::unordered_map<const slang::ast::SubroutineSymbol*, Subr*> subMap;
    std::vector<std::unique_ptr<Subr>> owned;
    std::vector<Subr*> order;
    dpiMap().clear();
    auto dpiArgs = [&](const slang::ast::SubroutineSymbol& sym, DpiFunc& f) {
        using namespace slang::ast;
        for (const FormalArgumentSymbol* formal : sym.getArguments()) {
            DpiArg a;
            a.name = std::string(formal->name);
            a.dir = formal->direction == ArgumentDirection::Out ? 1 : formal->direction == ArgumentDirection::InOut ? 2 : 0;
            const Type* t = &formal->getType().getCanonicalType();
            while (t->kind == SymbolKind::FixedSizeUnpackedArrayType || t->kind == SymbolKind::DPIOpenArrayType) {
                if (t->kind == SymbolKind::DPIOpenArrayType) {
                    const auto& o = t->as<DPIOpenArrayType>();
                    if (o.isPacked) Lowerer::unsupported("DPI packed open array argument (bit [])");
                    a.open = true;
                    a.udims.push_back({0, 0});
                    a.uopen.push_back(true);
                    t = &o.elementType.getCanonicalType();
                } else {
                    const auto& ft = t->as<FixedSizeUnpackedArrayType>();
                    a.udims.push_back({ft.range.left, ft.range.right});
                    a.uopen.push_back(false);
                    t = &ft.elementType.getCanonicalType();
                }
                a.uarr = true;
            }
            if (t->isUnpackedStruct()) Lowerer::unsupported("DPI unpacked struct argument");
            a.t = dpiType(*t, a.w, a.sgn);
            if (!a.uarr) {
                auto it = sigmap.find(formal);
                if (it == sigmap.end()) Lowerer::unsupported("DPI argument");
                a.sig = it->second;
            } else if (f.isExport) Lowerer::unsupported("unpacked array argument of a DPI export");
            f.args.push_back(std::move(a));
        }
    };
    Lowerer setup(sigmap);
    auto framed = framedSubroutines(finder.found);
    for (const slang::ast::SubroutineSymbol* sym : finder.found) {
        if (!sym->flags.has(slang::ast::MethodFlags::DPIImport) || dpiMap().count(sym)) continue;
        using namespace slang::ast;
        auto f = std::make_unique<DpiFunc>();
        f->svName = std::string(sym->name);
        f->cName = f->svName;
        if (auto* syn = sym->getSyntax(); syn && syn->kind == slang::syntax::SyntaxKind::DPIImport) {
            auto cid = syn->as<slang::syntax::DPIImportSyntax>().c_identifier.valueText();
            if (!cid.empty()) f->cName = std::string(cid);
        }
        f->scope = dpiScopeName(*sym);
        f->task = sym->subroutineKind == SubroutineKind::Task;
        f->context = sym->flags.has(MethodFlags::DPIContext);
        f->pure = sym->flags.has(MethodFlags::Pure);
        dpiArgs(*sym, *f);
        if (!f->task) {
            f->ret = dpiType(sym->getReturnType(), f->retW, f->retSgn);
            if (f->ret != DT_VOID) f->retSig = mkSig("_dpi_ret$" + f->cName, f->retW, false);
        }
        dpiMap()[sym] = f.get();
        out.dpi.push_back(std::move(f));
    }
    for (const slang::ast::SubroutineSymbol* sym : finder.found) {
        if (subMap.count(sym) || sym->flags.has(slang::ast::MethodFlags::DPIImport)) continue;
        bool packedRet = sym->returnValVar && sym->getReturnType().getCanonicalType().kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType &&
                         uWidth(sym->getReturnType()) > 0;   // a fixed-size array result, held packed in one signal
        if (packedRet) arrMap().erase(sym->returnValVar);
        if (!packedRet && sym->returnValVar && sym->getReturnType().isUnpackedArray() && !Lowerer::isDynType(sym->getReturnType())) {
            // Returning an unpacked array is only supported when every call
            // is constant (13.4.3) and slang evaluates it; a call that needs
            // the function at run time reports this then.
            Lowerer::skippedSubs()[sym] = "function returning an unpacked array";
            continue;
        }
        auto sub = std::make_unique<Subr>();
        out.subs.push_back(std::make_unique<Prog>());
        sub->prog = out.subs.back().get();
        sub->sym = sym;
        if (framed.count(sym)) {
            // Arguments, the result, and every automatic local are slots of
            // one frame per call (13.3.2), so the subroutine can recurse and
            // run in several processes at once.
            using namespace slang::ast;
            sub->lay = newLayout(std::string(sym->name));
            if (sym->returnValVar || !sym->getReturnType().isVoid()) {   // (pure and built-in methods have no returnValVar)
                // The result is slot 0 and the arguments follow, so every
                // override of a virtual method has them at the same slots.
                if (Lowerer::isDynType(sym->getReturnType())) {   // a container result: a container slot
                    if (sym->returnValVar) { setup.declLocal(*sym->returnValVar, sub->lay); sub->retSig = localDyn().at(sym->returnValVar).off; }
                    else sub->retSig = Lowerer::declSlot(sym->getReturnType(), sub->lay).off;
                    sub->retDyn = true;
                    sub->retW = 64;
                } else if (packedRet) {
                    int w = uWidth(sym->getReturnType());
                    int off = sub->lay->add(w, structDefaultVal(sym->getReturnType()));
                    localMap()[sym->returnValVar] = {sub->lay, off};
                    sub->retSig = off;
                    sub->retW = w;
                } else {
                if (sym->returnValVar) setup.declLocal(*sym->returnValVar, sub->lay);
                else sub->lay->add(storeW(sym->getReturnType()), Lowerer::initOf(sym->getReturnType()));
                sub->retSig = sym->returnValVar ? localMap().at(sym->returnValVar).off : 0;
                sub->retW = storeW(sym->getReturnType());
                }
                if (sub->retW <= 0) Lowerer::unsupported("function return width");
                sub->retSgn = sym->getReturnType().isSigned();
            }
            for (const FormalArgumentSymbol* formal : sym->getArguments()) {
                if (formal->direction == ArgumentDirection::Ref) {   // a slot for the actual's signal id
                    refFormals()[formal] = Lowerer::refFormalOf(formal->getType());
                    int off = sub->lay->add(64, make(0, 0, 64));
                    localMap()[formal] = {sub->lay, off};
                    sub->args.push_back({off, formal->direction, 64, false});
                    continue;
                }
                if (Lowerer::isDynType(formal->getType()) || formal->getType().isUnpackedArray()) {
                    // 7.7: a frame array or container, copied in at the call
                    setup.declLocal(*formal, sub->lay);
                    Subr::Arg ar{-1, formal->direction, 0, false};
                    if (auto la = localArr().find(formal); la != localArr().end()) ar.sig = la->second.second->base;
                    else ar.sig = localDyn().at(formal).off;
                    ar.arr = formal;
                    sub->args.push_back(ar);
                    continue;
                }
                setup.declLocal(*formal, sub->lay);
                int w = storeW(formal->getType());
                if (w <= 0) Lowerer::unsupported("subroutine argument width");
                sub->args.push_back({localMap().at(formal).off, formal->direction, w, formal->getType().isSigned()});
            }
            setup.declLocals(sym->getBody(), sub->lay);
            subMap[sym] = sub.get();
            order.push_back(sub.get());
            owned.push_back(std::move(sub));
            continue;
        }
        if (packedRet && !sigmap.count(sym->returnValVar))
            sigmap[sym->returnValVar] = mkSig(std::string(sym->name) + "$ret", uWidth(sym->getReturnType()), false);
        if (sym->returnValVar) {
            auto it = sigmap.find(sym->returnValVar);
            if (it == sigmap.end()) Lowerer::unsupported("function return");
            sub->retSig = it->second;
            sub->retW = bitsOf(sym->getReturnType());
            if (sub->retW <= 0) Lowerer::unsupported("function return width");
            sub->retSgn = sym->getReturnType().isSigned();
        }
        for (const slang::ast::FormalArgumentSymbol* formal : sym->getArguments()) {
            if (formal->direction == slang::ast::ArgumentDirection::Ref) {   // its signal holds the actual's id
                refFormals()[formal] = Lowerer::refFormalOf(formal->getType());
                int sg = mkSig(std::string(sym->name) + "." + std::string(formal->name) + "$ref", 64, false);
                sigmap[formal] = sg;
                sub->args.push_back({sg, formal->direction, 64, false});
                continue;
            }
            if (Lowerer::isDynType(formal->getType()) || formal->getType().isUnpackedArray()) {
                Subr::Arg ar{-1, formal->direction, 0, false};
                ar.arr = formal;
                sub->args.push_back(ar);
                continue;
            }
            auto it = sigmap.find(formal);
            if (it == sigmap.end()) Lowerer::unsupported("subroutine argument");
            int w = storeW(formal->getType());
            if (w <= 0) Lowerer::unsupported("subroutine argument width");
            sub->args.push_back({it->second, formal->direction, w, formal->getType().isSigned()});
        }
        subMap[sym] = sub.get();
        order.push_back(sub.get());
        owned.push_back(std::move(sub));
    }

    // Classes (8): object shapes, constructors, virtual method tables.
    buildClasses(finder.classes, subMap, out, owned, order);
    if (!classStore().empty()) {
        g_nullSig = mkSig("$null", 64, false);
    }

    Lowerer::scanBlockEvents(comp.getRoot());
    int tempSeq = 0;
    for (Subr* sub : order) {
        if (sub->sym && sub->sym->flags.has(slang::ast::MethodFlags::DPIImport)) continue;
        if (sub->sym && sub->sym->flags.has(slang::ast::MethodFlags::Pure)) continue;
        if (sub->cls) {   // a constructor, written or implicit
            Lowerer lw(sigmap);
            lw.bind(&subMap, mkSig, &tempSeq, &out.subs);
            if (sub->sym) lw.setScope(sub->sym);
            lw.frames = {sub->lay};
            lw.curClass = sub->cls;
            lw.current = sub;
            if (sub->cls->builtin) lw.builtinMethod(*sub->cls, *sub);
            else lw.ctorBody(*sub->cls, sub->sym ? &sub->sym->getBody() : nullptr);
            *sub->prog = std::move(lw.prog);
            sub->prog->sub = sub->sym;
            continue;
        }
        g_covInst = 0;
        if (g_cov)
            if (const auto* body = sub->sym->getParentScope()->getContainingInstance())
                if (body->parentInstance) g_covInst = covInstIdOf(*body->parentInstance);
        Lowerer lw(sigmap);
        lw.bind(&subMap, mkSig, &tempSeq, &out.subs);
        lw.setScope(sub->sym);
        lw.retSig = sub->retSig;
        lw.condCov = true;
        if (sub->lay) { lw.frames = {sub->lay}; lw.retSig = sub->retSig >= 0 ? relSig(0, sub->retSig) : -1; }
        if (isClassMethod(*sub->sym)) lw.curClass = classInfo(sub->sym->getParentScope()->asSymbol().as<slang::ast::ClassType>());
        lw.current = sub;
        if (lw.curClass && lw.curClass->builtin) lw.builtinMethod(*lw.curClass, *sub);
        else {
            lw.blockEventSample(sub->sym, true);   // @@(begin f) / @@(end f) (19.3)
            lw.stmt(sub->sym->getBody());
            lw.blockEventSample(sub->sym, false);
        }
        if (blockIds().targets.count(sub->sym))   // a task some disable names (9.6.2)
        { lw.prog.spans.push_back({blockIds().of(sub->sym), 0, SIZE_MAX}); anySpans() = true; }
        *sub->prog = std::move(lw.prog);
        sub->prog->sub = sub->sym;   // VPI frames (38.36.1)
        if (!sub->lay) {   // its static variables: callers are not sensitive to them
            struct Walk {
                const std::unordered_map<const slang::ast::Symbol*, int>& sm;
                std::vector<int>& out;
                void scope(const slang::ast::Scope& sc) {
                    for (const auto& m : sc.members()) {
                        if (m.kind == slang::ast::SymbolKind::Variable || m.kind == slang::ast::SymbolKind::FormalArgument)
                            if (auto it = sm.find(&m); it != sm.end()) out.push_back(it->second);
                        if (m.isScope()) scope(m.as<slang::ast::Scope>());
                    }
                }
            } w{sigmap, sub->prog->locals};
            w.scope(*sub->sym);
            if (sub->retSig >= 0) sub->prog->locals.push_back(sub->retSig);
        }
    }
    g_covInst = 0;
    for (Subr* sub : order) patchImplicit(*sub->prog);
    for (const auto& ex : comp.getDPIExports()) {   // export "DPI-C" (35.5.4)
        using namespace slang::ast;
        auto it = subMap.find(ex.subroutine);
        if (it == subMap.end()) Lowerer::unsupported("DPI export of this subroutine");
        Subr* sub = it->second;
        if (sub->lay) Lowerer::unsupported("DPI export of an automatic task, or of a function that recurses");
        auto f = std::make_unique<DpiFunc>();
        f->isExport = true;
        f->svName = std::string(ex.subroutine->name);
        f->cName = ex.cIdentifier;
        f->scope = dpiScopeName(*ex.subroutine);
        f->task = ex.subroutine->subroutineKind == SubroutineKind::Task;
        f->prog = sub->prog;
        dpiArgs(*ex.subroutine, *f);
        if (!f->task) {
            f->ret = dpiType(ex.subroutine->getReturnType(), f->retW, f->retSgn);
            f->retSig = sub->retSig;
        }
        out.dpi.push_back(std::move(f));
    }
    {   // DPI scope names (35.5.3): every instance and package
        struct Scopes : slang::ast::ASTVisitor<Scopes> {
            std::vector<std::string>* out;
            std::vector<std::string>* prot;
            void handle(const slang::ast::InstanceSymbol& inst) {
                if (!enterInstance(inst)) return;
                out->push_back(inst.getHierarchicalPath());
                if (protectedInstance(&inst)) prot->push_back(out->back());
                visitDefault(inst);
            }
            void handle(const slang::ast::PackageSymbol& p) {
                out->push_back(std::string(p.name));
                if (hideMsgIf() && hideMsgIf()(p.location)) prot->push_back(out->back());
            }
            void handle(const slang::ast::GenerateBlockSymbol& g) {
                if (g.isUninstantiated) return;
                out->push_back(g.getHierarchicalPath());
                if (protectedInstance(instanceAround(g.getParentScope()))) prot->push_back(out->back());
                visitDefault(g);
            }
        } sv;
        sv.out = &out.scopes;
        sv.prot = &out.protScopes;
        comp.getRoot().visit(sv);
    }

    // Randomization (18): each class's rand variables, constraint blocks
    // (an override by name replaces the base block), pre/post_randomize.
    {
        using namespace slang::ast;
        std::unordered_map<const ConstraintBlockSymbol*, std::shared_ptr<RandBlock>> blockOf;
        for (auto& up : classStore()) {
            ClassInfo* ci = up.get();
            if (ci->ty->isInterface || ci->builtin || !ci->lay) continue;
            std::vector<ClassInfo*> chain;
            for (ClassInfo* c = ci; c; c = c->base) chain.insert(chain.begin(), c);
            auto sp = std::make_shared<RandSpec>();
            std::unordered_map<std::string, size_t> byName;
            for (ClassInfo* c : chain) {
                Lowerer lw(sigmap);
                lw.bind(&subMap, mkSig, &tempSeq, &out.subs);
                lw.curClass = c;
                auto enumBlock = std::make_shared<RandBlock>();
                for (const auto& m : c->ty->members()) {
                    if (m.kind == SymbolKind::ClassProperty) {
                        const auto& p = m.as<ClassPropertySymbol>();
                        if (p.randMode == RandMode::None) continue;
                        if (p.lifetime == VariableLifetime::Static) Lowerer::unsupported("static rand property");
                        const PropSlot& ps = propMap().at(&p);
                        const Type& t = p.getType().getCanonicalType();
                        RandVar rv;
                        rv.name = std::string(p.name);
                        rv.randc = p.randMode == RandMode::RandC;
                        rv.mode = randModeMap().at(&p);
                        const Type* et = &t;
                        if (ps.kind == 1 && !ps.arr->dynElem) {
                            rv.off = ps.arr->base; rv.count = ps.arr->count;
                            while (et->isUnpackedArray()) et = &et->getArrayElementType()->getCanonicalType();
                        } else if (ps.kind == 0) rv.off = ps.off;
                        else {   // a dynamic array or queue (18.4): its size and elements
                            const Type* dt = &t;   // int q[4][$]: each of the 4 queues
                            if (ps.kind == 1) {
                                while (!Lowerer::isDynType(*dt)) dt = &dt->getArrayElementType()->getCanonicalType();
                                rv.off = ps.arr->base; rv.count = ps.arr->count;
                            } else rv.off = ps.off;
                            if (dt->isAssociativeArray()) {   // its values (18.4); the keys stay
                                const auto* ix = dt->as<AssociativeArrayType>().indexType;
                                if (!ix || !(ix->getCanonicalType().isIntegral() || ix->getCanonicalType().isString()))
                                    Lowerer::unsupported("rand associative array with a wildcard or class index");
                            }
                            rv.dyn = true;
                            et = &dt->getArrayElementType()->getCanonicalType();
                            while (Lowerer::isDynType(*et)) {   // int m[][] (7.5, 18.4): every level
                                if (et->isAssociativeArray()) {
                                    const auto* ix = et->as<AssociativeArrayType>().indexType;
                                    if (!ix || !(ix->getCanonicalType().isIntegral() || ix->getCanonicalType().isString()))
                                        Lowerer::unsupported("rand associative array with a wildcard or class index");
                                }
                                rv.depth++;
                                et = &et->getArrayElementType()->getCanonicalType();
                            }
                            if (et->isUnpackedArray()) Lowerer::unsupported("rand container of fixed-size arrays");
                        }
                        if (isHandleType(*et)) {
                            rv.obj = true; rv.w = 64;   // a dynamic array or queue of them too: each object
                        }
                        else if (et->isIntegral() && !et->isUnpackedStruct()) {
                            rv.w = (int)et->getBitWidth(); rv.sgn = et->isSigned();
                            if (rv.w > 64 && rv.randc) Lowerer::unsupported("randc variable wider than 64 bits");
                        } else Lowerer::unsupported("rand property of this type");
                        if (et->isEnum() && rv.dyn) for (int k0 = 0; k0 < rv.count; k0++) {   // each element: one of the named values
                            RandItem loop;
                            loop.loopCont = lw.thisRel(rv.off + k0);
                            loop.loopVar = lw.fresh(32);
                            // Containers of containers: a loop per level, over m[i] then m[i][j].
                            std::vector<RandItem> outer;
                            for (int lv = 1; lv < rv.depth; lv++) {
                                RandItem in;
                                in.loopCont = Lowerer::indThrough(lw.fresh(64));
                                in.loopVar = lw.fresh(32);
                                auto r = node(Op::DynElemRef, {ref(loop.loopVar, true)});
                                r->sig = loop.loopCont;
                                in.refs.push_back({in.loopCont, r});
                                outer.push_back(std::move(loop));
                                loop = std::move(in);
                            }
                            E any;
                            for (const auto& ev : et->as<EnumType>().values()) {
                                const auto& cv = ev.getValue();
                                if (!cv.isInteger()) continue;
                                auto u = cv.integer().as<uint64_t>();
                                auto g = node(Op::DynGet, {ref(loop.loopVar, true)});
                                g->sig = loop.loopCont;
                                E eq = bin(Op::Eq, g, konst(mk(u ? *u : 0, rv.w)));
                                any = any ? bin(Op::LogOr, any, eq) : eq;
                            }
                            if (any) {
                                RandItem it; it.e = any; loop.body.push_back(std::move(it));
                                while (!outer.empty()) {
                                    RandItem up = std::move(outer.back());
                                    outer.pop_back();
                                    up.body.push_back(std::move(loop));
                                    loop = std::move(up);
                                }
                                enumBlock->items.push_back(std::move(loop));
                            }
                        } else if (et->isEnum()) {   // an enum takes only its named values (18.4)
                            for (int k = 0; k < rv.count; k++) {
                                E any;
                                for (const auto& ev : et->as<EnumType>().values()) {
                                    const auto& cv = ev.getValue();
                                    if (!cv.isInteger()) continue;
                                    auto u = cv.integer().as<uint64_t>();
                                    E eq = bin(Op::Eq, ref(lw.thisRel(rv.off + k), false), konst(mk(u ? *u : 0, rv.w)));
                                    any = any ? bin(Op::LogOr, any, eq) : eq;
                                }
                                if (any) { RandItem it; it.e = any; enumBlock->items.push_back(std::move(it)); }
                            }
                        }
                        sp->vars.push_back(std::move(rv));
                        continue;
                    }
                    if (m.kind == SymbolKind::ConstraintBlock) {
                        const auto& cb = m.as<ConstraintBlockSymbol>();
                        auto& b = blockOf[&cb];
                        if (!b) b = lw.randBlock(cb);
                        if (auto f = byName.find(b->name); f != byName.end()) sp->blocks[f->second] = b;
                        else { byName[b->name] = sp->blocks.size(); sp->blocks.push_back(b); }
                        continue;
                    }
                    // (slang adds a built-in pre/post_randomize, with no body, to a class that declares none)
                    // An extern declaration is a prototype: its body is the out-of-block definition (8.24).
                    const SubroutineSymbol* rsub = m.kind == SymbolKind::Subroutine ? &m.as<SubroutineSymbol>()
                        : m.kind == SymbolKind::MethodPrototype ? m.as<MethodPrototypeSymbol>().getSubroutine() : nullptr;
                    if (rsub && m.getSyntax() && (m.name == "pre_randomize" || m.name == "post_randomize")) {
                        auto it = subMap.find(rsub);
                        if (it == subMap.end()) continue;
                        if (m.name == "pre_randomize") { sp->pre = it->second->prog; sp->preLay = it->second->lay; }
                        else { sp->post = it->second->prog; sp->postLay = it->second->lay; }
                    }
                }
                if (!enumBlock->items.empty()) sp->blocks.push_back(enumBlock);
            }
            classRt()[(size_t)ci->id].rand = sp;
        }
    }

    ProcCollector pc(sigmap, out, std::move(newDriver), &subMap, mkSig, &tempSeq);
    comp.getRoot().visit(pc);
    pc.finishPaths();
    pc.finishUserNets();
    pc.placeInit();
    sampledInit(out);
    rejectRecursive(order);
    // Virtual interfaces (25.9): each referenced instance's signal for each
    // member reached through a virtual interface.
    {
        using namespace slang::ast;
        auto& vi = vifInfo();
        auto bodyDef = [](const Symbol& m) -> const DefinitionSymbol* {
            const Scope* sc = m.getParentScope();
            while (sc && sc->asSymbol().kind != SymbolKind::InstanceBody) sc = sc->asSymbol().getParentScope();
            return sc ? &sc->asSymbol().as<InstanceBodySymbol>().getDefinition() : nullptr;
        };
        auto& tab = vifTab();
        tab.assign(vi.insts.size(), {});
        for (size_t k = 1; k < vi.insts.size(); k++) {
            const InstanceSymbol& inst = *vi.insts[k];
            tab[k].assign(vi.members.size(), -1);
            for (size_t j = 0; j < vi.members.size(); j++) {
                const Symbol* m = vi.members[j];
                if (bodyDef(*m) != &inst.getDefinition()) continue;
                if (m->kind == SymbolKind::ModportPort)
                    if (const Symbol* in = m->as<ModportPortSymbol>().internalSymbol) m = in;
                std::vector<std::string_view> path;   // cb.x: [cb, x]
                for (const Symbol* q = m; q && q->kind != SymbolKind::InstanceBody; q = &q->getParentScope()->asSymbol())
                    path.insert(path.begin(), q->name);
                const Symbol* f = &inst.body;
                for (auto nm : path) {
                    if (!f || !f->isScope()) { f = nullptr; break; }
                    f = f->as<Scope>().find(nm);
                }
                if (f && f->kind == SymbolKind::Port)
                    if (const Symbol* in = f->as<PortSymbol>().internalSymbol) f = in;
                if (!f) continue;
                if (auto it = sigmap.find(f); it != sigmap.end()) tab[k][j] = it->second;
                else if (auto a = arrMap().find(f); a != arrMap().end()) tab[k][j] = a->second->base;   // its first element
            }
        }
    }
    if (g_cov) {   // FSM coverage: the state registers and their transitions
        const slang::SourceManager* sm = comp.getSourceManager();
        for (ExtractedFsm& x : extractFsms(comp.getRoot())) {
            auto sit = sigmap.find(x.var);
            if (sit == sigmap.end()) continue;
            FsmDef d;
            d.module = std::move(x.module);
            d.inst = std::move(x.inst);
            d.name = std::move(x.name);
            if (sm && x.var->location.valid()) {
                auto ol = sm->getFullyOriginalLoc(x.var->location);
                d.file = covFileId(std::string(sm->getFullPath(ol.buffer()).string()));
                d.line = (uint32_t)sm->getLineNumber(ol);
            }
            d.sig = sit->second;
            d.w = x.w;
            d.onehot = x.onehot;
            d.states = std::move(x.states);
            for (auto& t : x.trans) d.trans.push_back({t.from, t.to, t.reset});
            fsmDefs().push_back(std::move(d));
        }
    }
    out.sigOf = sigmap;
    return out;
}
