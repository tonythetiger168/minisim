#pragma once
// popt.hpp - the engine of mpopt, the RTL power optimizer.
//
// mpopt reads RTL on the msim front end (slang), finds registers and
// operators that do useless work, and rewrites the source text so that they
// stop. Four transformations:
//
//   CG   enable clock gating. A register that keeps its value unless a
//        condition holds (if (en) q <= d;) gets a clock gate with that
//        condition as its enable. The condition is the OR of the path
//        conditions of every assignment to the register.
//   SCG  sequential (stability) clock gating. A register whose next value
//        is a function of other registers need not load when none of those
//        loaded in the cycle before: the value it would load is the value
//        it has. mpopt adds one flop that remembers "a source loaded" and
//        uses it as the enable. This is the gating a synthesis tool does
//        not find, because the enable is not written in the RTL.
//   MG   memory gating: CG and SCG applied to memories (unpacked arrays).
//        A write port is gated by its write enable; a registered read port
//        only reads again after its address moved or the memory was written.
//   OI   operand isolation. A multiplier or divider whose result is used
//        under a condition gets its operands forced to zero when the
//        condition is false, so that the operator does not toggle.
//
// Why the rewrites are right (docs/POWEROPT.md has the full argument):
//   CG   the block body is kept as written, so when the enable is 0 the
//        body would have assigned nothing. Removing that clock edge changes
//        no register.
//   SCG  with x the source registers and F the block: q' = F(x, q), where
//        the block never reads q except to hold it. If x did not change
//        since the last edge, the same branch is taken and the same values
//        are assigned again: F(x, F(x, q)) = F(x, q). The stability flop
//        resets to 1, so the first edge after reset is always taken.
//   OI   the wrapped operand sits on a path where the condition is true,
//        so (c ? a : 0) is a there.
// Every register keeps its value in every cycle after reset; no state is
// added to or removed from the original design other than the stability
// flops. mpopt -verify co-simulates both versions on random stimulus; the
// proof belongs to mequiv, the equivalence checker.
//
// The rewrite edits the source text in place (comments, formatting and
// everything mpopt does not touch are kept byte for byte), like a designer
// would, so the result can be reviewed with diff.
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lintir.hpp"
#include "parallel.hpp"

#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"

namespace popt {

using namespace slang;
using namespace slang::ast;

// ------------------------------------------------------------------ options

struct Options {
    bool cg = true, seq = true, iso = true, split = true;
    unsigned minBits = 4;      // smallest register group worth a clock gate
    unsigned seqMinBits = 8;   // smallest group worth a gate plus a stability flop
    unsigned seqDepth = 4;     // how many stages a stability enable may chain through
    unsigned isoMinBits = 8;   // smallest operator result worth isolating
    unsigned isoAddBits = 0;   // isolate adders/subtractors at least this wide (0: never)
    bool isoPlain = false;     // write (c ? a : 0) instead of (c !== 1'b0 ? a : 0)
    bool isoInline = false;    // never add a named isolation signal: rewrite the expression only
    unsigned maxEnableTerms = 12;  // an enable with more OR terms than this is not used
    std::string prefix = "mpopt";
    std::string icgCell;       // library cell to instantiate instead of <prefix>_icg
    std::string icgClk = "clk", icgEn = "en", icgTe = "te", icgOut = "gclk";
    std::string testEnable;    // signal tied to the gate's test enable where the module has it
    unsigned threads = 0;
};

// ------------------------------------------------------------- conditions
//
// A path condition: and/or/not over atoms, where an atom is the source text
// of a condition as the designer wrote it. Conditions are compared by key,
// simplified just enough to stay readable, and printed back as SystemVerilog.

struct Cond;
using CP = std::shared_ptr<const Cond>;
struct Cond {
    enum K { T, F, Atom, Not, And, Or } k = T;
    std::string text;      // Atom: source text
    std::vector<CP> kids;
    std::string key;
};

inline CP cTrue() {
    static CP c = [] { auto n = std::make_shared<Cond>(); n->k = Cond::T; n->key = "1"; return n; }();
    return c;
}
inline CP cFalse() {
    static CP c = [] { auto n = std::make_shared<Cond>(); n->k = Cond::F; n->key = "0"; return n; }();
    return c;
}
inline CP cAtom(const std::string& text) {
    auto n = std::make_shared<Cond>();
    n->k = Cond::Atom;
    n->text = text;
    n->key = "(" + text + ")";
    return n;
}
inline std::string compKey(const std::string& k) { return k[0] == '!' ? k.substr(1) : "!" + k; }

CP cAnd(std::vector<CP> in);
CP cOr(std::vector<CP> in);

inline CP cNot(const CP& a) {
    switch (a->k) {
        case Cond::T: return cFalse();
        case Cond::F: return cTrue();
        case Cond::Not: return a->kids[0];
        case Cond::Or: {   // De Morgan, so that "not any earlier case item" is a list of literals
            std::vector<CP> v;
            for (auto& k : a->kids) v.push_back(cNot(k));
            return cAnd(std::move(v));
        }
        default: break;
    }
    auto n = std::make_shared<Cond>();
    n->k = Cond::Not;
    n->kids = {a};
    n->key = "!" + a->key;
    return n;
}

inline CP cMake(Cond::K k, std::vector<CP> kids) {
    auto n = std::make_shared<Cond>();
    n->k = k;
    n->key = k == Cond::And ? "&(" : "|(";
    for (size_t i = 0; i < kids.size(); i++) n->key += (i ? "," : "") + kids[i]->key;
    n->key += ")";
    n->kids = std::move(kids);
    return n;
}

inline CP cAnd(std::vector<CP> in) {
    std::vector<CP> v;
    std::unordered_set<std::string> seen;
    std::function<bool(const CP&)> add = [&](const CP& c) -> bool {
        if (c->k == Cond::T) return true;
        if (c->k == Cond::F) return false;
        if (c->k == Cond::And) {
            for (auto& k : c->kids)
                if (!add(k)) return false;
            return true;
        }
        if (seen.count(compKey(c->key))) return false;
        if (seen.insert(c->key).second) v.push_back(c);
        return true;
    };
    for (auto& c : in)
        if (!add(c)) return cFalse();
    if (v.empty()) return cTrue();
    if (v.size() == 1) return v[0];
    return cMake(Cond::And, std::move(v));
}

// The literals of one OR term (an And, or a single literal).
inline std::vector<CP> litsOf(const CP& c) { return c->k == Cond::And ? c->kids : std::vector<CP>{c}; }

inline CP cOr(std::vector<CP> in) {
    std::vector<CP> v;
    std::unordered_set<std::string> seen;
    std::function<bool(const CP&)> add = [&](const CP& c) -> bool {
        if (c->k == Cond::F) return true;
        if (c->k == Cond::T) return false;
        if (c->k == Cond::Or) {
            for (auto& k : c->kids)
                if (!add(k)) return false;
            return true;
        }
        if (seen.count(compKey(c->key))) return false;
        if (seen.insert(c->key).second) v.push_back(c);
        return true;
    };
    for (auto& c : in)
        if (!add(c)) return cTrue();
    if (v.empty()) return cFalse();
    // Two rules to a fixed point, on terms as sets of literals:
    //   t | u = t | (u without !x)   when x is in t and the rest of t is in u
    //   t | u = t                    when every literal of t is in u
    // They turn "rst | (!rst & en)" into "rst | en" and "(c & a) | (!c & a)" into "a".
    if (v.size() <= 48) {
        std::vector<std::vector<CP>> terms;
        for (auto& c : v) terms.push_back(litsOf(c));
        auto has = [](const std::vector<CP>& t, const std::string& key) {
            for (auto& l : t)
                if (l->key == key) return true;
            return false;
        };
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < terms.size() && !changed; i++)
                for (size_t j = 0; j < terms.size() && !changed; j++) {
                    if (i == j) continue;
                    auto& t = terms[i];
                    auto& u = terms[j];
                    // absorption
                    bool sub = true;
                    for (auto& l : t)
                        if (!has(u, l->key)) { sub = false; break; }
                    if (sub) {
                        terms.erase(terms.begin() + (long)j);
                        changed = true;
                        break;
                    }
                    // literal drop
                    for (size_t x = 0; x < t.size() && !changed; x++) {
                        std::string nk = compKey(t[x]->key);
                        if (!has(u, nk)) continue;
                        bool rest = true;
                        for (size_t y = 0; y < t.size(); y++)
                            if (y != x && !has(u, t[y]->key)) { rest = false; break; }
                        if (!rest) continue;
                        std::vector<CP> nu;
                        for (auto& l : u)
                            if (l->key != nk) nu.push_back(l);
                        if (nu.empty()) return cTrue();
                        u = std::move(nu);
                        changed = true;
                    }
                }
        }
        v.clear();
        for (auto& t : terms) v.push_back(cAnd(t));
    }
    if (v.size() == 1) return v[0];
    return cMake(Cond::Or, std::move(v));
}

inline std::string cText(const CP& c, bool top = true) {
    switch (c->k) {
        case Cond::T: return "1'b1";
        case Cond::F: return "1'b0";
        case Cond::Atom: {
            // A plain name needs no parentheses, nor does a whole enable.
            if (top) return c->text;
            bool plain = !c->text.empty();
            for (char ch : c->text)
                if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == '$')) plain = false;
            return plain ? c->text : "(" + c->text + ")";
        }
        case Cond::Not: return "!" + cText(c->kids[0], false);
        case Cond::And:
        case Cond::Or: {
            std::string s;
            for (size_t i = 0; i < c->kids.size(); i++)
                s += (i ? (c->k == Cond::And ? " && " : " || ") : "") + cText(c->kids[i], false);
            return top ? s : "(" + s + ")";
        }
    }
    return "1'b1";
}
inline size_t cTerms(const CP& c) { return c->k == Cond::Or ? c->kids.size() : 1; }

// ------------------------------------------------------------ source text

// One replacement in a source buffer: [begin, end) becomes text. Insertions
// have begin == end; seq orders insertions at the same place.
struct Edit {
    uint32_t buf = 0;
    size_t begin = 0, end = 0;
    int seq = 0;
    std::string text;
    bool operator==(const Edit& o) const {
        return buf == o.buf && begin == o.begin && end == o.end && text == o.text;
    }
};

// Comments out, runs of white space to one space: a condition copied to
// another line must not carry a "//" with it.
inline std::string normText(std::string_view s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') i++;
            o += ' ';
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i++;
            o += ' ';
            continue;
        }
        if (std::isspace((unsigned char)s[i])) {
            if (!o.empty() && o.back() != ' ') o += ' ';
        } else
            o += s[i];
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    size_t b = 0;
    while (b < o.size() && o[b] == ' ') b++;
    return o.substr(b);
}

struct Span {
    uint32_t buf = 0;
    size_t begin = 0, end = 0;
    bool ok = false;
};

inline Span spanOf(const SourceManager& sm, SourceRange r) {
    Span s;
    if (!r.start() || !r.end()) return s;
    if (sm.isMacroLoc(r.start()) || sm.isMacroLoc(r.end())) return s;
    if (r.start().buffer() != r.end().buffer()) return s;
    s.buf = r.start().buffer().getId();
    s.begin = r.start().offset();
    s.end = r.end().offset();
    s.ok = s.end >= s.begin;
    return s;
}
inline Span spanOf(const SourceManager& sm, const Expression& e) {
    return spanOf(sm, e.syntax ? e.syntax->sourceRange() : e.sourceRange);
}
inline std::string_view rawText(const SourceManager& sm, const Span& s) {
    return sm.getSourceText(BufferID(s.buf, ""sv)).substr(s.begin, s.end - s.begin);
}
inline std::optional<std::string> exprText(const SourceManager& sm, const Expression& e) {
    Span s = spanOf(sm, e);
    if (!s.ok || s.end == s.begin) return std::nullopt;
    std::string_view raw = rawText(sm, s);
    // A conditional-compilation directive inside the expression cannot be moved to one line.
    for (size_t i = raw.find('`'); i != std::string_view::npos; i = raw.find('`', i + 1)) {
        std::string_view r = raw.substr(i + 1);
        for (std::string_view d : {"if", "el", "endif", "define", "undef", "include"})
            if (r.substr(0, d.size()) == d) return std::nullopt;
    }
    return normText(raw);
}

// --------------------------------------------------------------- the model

using Sym = const Symbol*;
// nullptr in a read set: something mpopt cannot follow (a hierarchical
// name, a function call), so nothing may be concluded from the set.
using SymSet = std::set<Sym>;

struct IsoCand {
    Span a, b;              // the two operands
    bool wrapA = false, wrapB = false, signedA = false, signedB = false;
    bool leafA = false, leafB = false;   // a plain signal that the expression widens: keep its own width
    unsigned widthA = 0, widthB = 0;
    bool namedOk = false;   // operands and condition read module-level signals only
    Span decl;              // where a declaration for this statement can go (continuous assignments)
    std::string indent;
    CP cond;
    std::string op;
    unsigned bits = 0;
    size_t line = 0;
    std::vector<Sym> condReads;
};

struct Nba {
    Span span;              // the whole statement, with its semicolon
    std::vector<Sym> targets;
    CP cond;
    SymSet reads;           // right side, selects on the left, and every condition above
    bool inReset = false;   // in the asynchronous reset branch
    bool inSyncTop = false; // in the true branch of the block's first if
    bool rhsConst = false, self = false;
    std::vector<IsoCand> iso;
};

// A statement of a sequential block and the assignments under it: when a
// block is split, a statement with nothing left under it is removed whole.
struct StmtNode {
    Span span;
    bool inList = false;    // an item of a begin-end: can go without a trace
    size_t first = 0, last = 0;
};

struct Blk {
    const ProceduralBlockSymbol* sym = nullptr;
    const syntax::SyntaxNode* syn = nullptr;
    enum Kind { Seq, Comb, Other } kind = Other;
    std::string bail;       // why a sequential block is left alone
    Span span;              // the whole always block
    Span clkSpan;           // the clock name in the sensitivity list
    Sym clk = nullptr, arst = nullptr;
    bool posedge = true, arstHigh = true;
    std::string syncText;   // the block's synchronous reset condition, if it has one
    SymSet topReads;        // what the first if of a single-edge block reads
    bool hasLabel = false, parentOk = false, multi = false;
    size_t line = 0, col = 0;
    bool sharesLine = false;   // another always block starts on the same line
    std::string file;
    std::string indent;
    std::vector<Nba> nbas;
    std::vector<StmtNode> stmts;
    std::vector<int> groups;
    SymSet reads, writes;   // for combinational and other blocks
    std::vector<IsoCand> iso;  // combinational blocks
};

struct Group {
    int blk = -1;
    std::vector<Sym> regs;
    uint64_t bits = 0;
    bool mem = false;       // holds an unpacked array
    bool bad = false;       // assigned from more than one place
    CP e0;                  // may-change condition from the block's own text
    CP L;                   // the best may-change condition known (e0, or e0 && stable)
    bool srcDone = false, srcUnknown = false;
    std::vector<int> srcs;  // groups this one's next value depends on
    bool hasSt = false;
    std::string stName, stWhy;
    CP stNext;              // what the stability flop loads
    Sym stArst = nullptr;
    bool stArstHigh = true;
    std::string stSync;
    size_t stPos = 0;       // where its logic goes in the file
    std::vector<int> stDeps;   // source groups whose stability flop its own flop reads
    bool planSt = false, stEmit = false;   // this plan: gated with the flop; the flop is written
    int depth = 0;
    bool gated = false;
    CP enable;
    std::string gclk;
    std::string kind;       // CG, SCG, MG
    std::string skip;       // why it is not gated
};

struct Driver {
    enum K { None, Reg, Comb, Unknown } k = None;
    int group = -1;
    SymSet deps;
};

struct Finding {
    std::string kind, file, module, names, enable, note;
    size_t line = 0;
    uint64_t bits = 0;
    unsigned count = 1;     // the same finding in every copy of a generate loop
    bool applied = false;
    std::string key() const { return file + ":" + std::to_string(line) + ":" + kind + ":" + names; }
};

// One module body, analyzed.
struct Module {
    const InstanceBodySymbol* body = nullptr;
    std::string name;
    std::vector<Blk> blks;
    std::vector<Group> groups;
    std::unordered_map<Sym, Driver> drv;
    std::unordered_map<Sym, int> regGroup;
    std::vector<IsoCand> assignIso;   // in continuous assignments
    std::vector<Edit> edits;
    std::vector<Finding> finds;
    uint64_t regBits = 0, gatedBits = 0;
    size_t instances = 0;   // how many instances of this parameterization the design has
    bool usesIcg = false;
    std::unordered_map<Sym, std::pair<bool, std::vector<int>>> srcMemo;
    std::unordered_set<Sym> srcBusy;
};

// ------------------------------------------------------------- expressions

inline bool moduleLevel(const Symbol& s) {
    if (s.kind != SymbolKind::Variable && s.kind != SymbolKind::Net) return false;
    const Scope* sc = s.getParentScope();
    if (!sc) return false;
    auto k = sc->asSymbol().kind;
    return k == SymbolKind::InstanceBody || k == SymbolKind::GenerateBlock;
}

inline bool pureSystemCall(std::string_view n) {
    static const std::unordered_set<std::string_view> ok = {
        "$signed", "$unsigned", "$clog2", "$bits", "$size", "$high", "$low", "$left", "$right",
        "$countones", "$onehot", "$onehot0", "$isunknown", "$countbits"};
    return ok.count(n) > 0;
}

// Every module-level value the expression reads; nullptr for what cannot be followed.
inline void readsOf(const Expression& e, SymSet& out) {
    e.visit(makeVisitor(
        [&](auto&, const NamedValueExpression& n) {
            if (moduleLevel(n.symbol)) out.insert(&n.symbol);
            else if (n.symbol.kind == SymbolKind::Variable || n.symbol.kind == SymbolKind::Net) {
                // A local (a loop variable): a function of the rest, nothing to follow.
                const Scope* sc = n.symbol.getParentScope();
                auto k = sc ? sc->asSymbol().kind : SymbolKind::Unknown;
                if (k != SymbolKind::StatementBlock && k != SymbolKind::Subroutine) out.insert(nullptr);
            }
        },
        [&](auto&, const HierarchicalValueExpression&) { out.insert(nullptr); },
        [&](auto& v, const CallExpression& c) {
            if (!c.isSystemCall() || !pureSystemCall(c.getSubroutineName())) out.insert(nullptr);
            v.visitDefault(c);
        }));
}

inline const Expression& unwrap(const Expression& e) {
    const Expression* p = &e;
    while (p->kind == ExpressionKind::Conversion && p->as<ConversionExpression>().isImplicit())
        p = &p->as<ConversionExpression>().operand();
    return *p;
}

// The variables an assignment writes, and what its selects read. False for
// a left side mpopt does not understand.
inline bool lhsTargets(const Expression& e0, std::vector<Sym>& targets, SymSet& reads) {
    const Expression& e = unwrap(e0);
    switch (e.kind) {
        case ExpressionKind::NamedValue: {
            auto& s = e.as<NamedValueExpression>().symbol;
            targets.push_back(&s);
            return true;
        }
        case ExpressionKind::ElementSelect: {
            auto& s = e.as<ElementSelectExpression>();
            readsOf(s.selector(), reads);
            return lhsTargets(s.value(), targets, reads);
        }
        case ExpressionKind::RangeSelect: {
            auto& s = e.as<RangeSelectExpression>();
            readsOf(s.left(), reads);
            readsOf(s.right(), reads);
            return lhsTargets(s.value(), targets, reads);
        }
        case ExpressionKind::MemberAccess: return lhsTargets(e.as<MemberAccessExpression>().value(), targets, reads);
        case ExpressionKind::Concatenation:
            for (auto op : e.as<ConcatenationExpression>().operands())
                if (!lhsTargets(*op, targets, reads)) return false;
            return true;
        default: return false;
    }
}

inline bool expensiveOp(BinaryOperator op, unsigned bits, const Options& o) {
    switch (op) {
        case BinaryOperator::Multiply:
        case BinaryOperator::Divide:
        case BinaryOperator::Mod:
        case BinaryOperator::Power: return bits >= o.isoMinBits;
        case BinaryOperator::Add:
        case BinaryOperator::Subtract: return o.isoAddBits && bits >= o.isoAddBits;
        default: return false;
    }
}

inline bool hasExpensive(const Expression& e, const Options& o) {
    bool found = false;
    e.visit(makeVisitor([&](auto& v, const BinaryExpression& b) {
        if (expensiveOp(b.op, b.type->isIntegral() ? b.type->getBitWidth() : 0, o)) found = true;
        v.visitDefault(b);
    }));
    return found;
}

// Finds the operators worth isolating in an expression, tracking ?: conditions.
struct IsoVisitor : public ASTVisitor<IsoVisitor, VisitFlags::Expressions> {
    const SourceManager& sm;
    const Options& opt;
    std::vector<CP> stack;
    std::vector<std::vector<Sym>> readStack;
    std::vector<IsoCand>& out;
    bool usable = true;   // false inside something whose condition text is not available

    IsoVisitor(const SourceManager& sm, const Options& opt, CP base, std::vector<Sym> baseReads,
               std::vector<IsoCand>& out) : sm(sm), opt(opt), out(out) {
        stack.push_back(std::move(base));
        readStack.push_back(std::move(baseReads));
    }

    void handle(const ConditionalExpression& e) {
        if (e.conditions.size() != 1 || e.conditions[0].pattern) {
            bool was = usable;
            usable = false;
            visitDefault(e);
            usable = was;
            return;
        }
        const Expression& c = *e.conditions[0].expr;
        c.visit(*this);
        auto t = exprText(sm, c);
        SymSet rs;
        readsOf(c, rs);
        if (!t || rs.count(nullptr)) {
            bool was = usable;
            usable = false;
            e.left().visit(*this);
            e.right().visit(*this);
            usable = was;
            return;
        }
        std::vector<Sym> r = readStack.back();
        r.insert(r.end(), rs.begin(), rs.end());
        readStack.push_back(r);
        stack.push_back(cAnd({stack.back(), cAtom(*t)}));
        e.left().visit(*this);
        stack.pop_back();
        stack.push_back(cAnd({stack.back(), cNot(cAtom(*t))}));
        e.right().visit(*this);
        stack.pop_back();
        readStack.pop_back();
    }

    void handle(const BinaryExpression& e) {
        unsigned bits = e.type->isIntegral() ? e.type->getBitWidth() : 0;
        CP c = stack.back();
        if (usable && expensiveOp(e.op, bits, opt) && c->k != Cond::T && c->k != Cond::F) {
            IsoCand ic;
            ic.cond = c;
            ic.condReads = readStack.back();
            ic.op = std::string(OpInfo::getText(e.op));
            ic.bits = bits;
            bool named = true;
            for (Sym s : ic.condReads)
                if (!s || !moduleLevel(*s)) named = false;
            auto side = [&](const Expression& x0, Span& sp, bool& wrap, bool& sgn, bool& leaf, unsigned& width) {
                const Expression& x = unwrap(x0);
                sp = spanOf(sm, x);
                SymSet rs;
                readsOf(x, rs);
                bool varies = false;
                for (Sym s : rs) {
                    if (s) varies = true;
                    if (!s || !moduleLevel(*s)) named = false;
                }
                width = x.type->isIntegral() ? (unsigned)x.type->getBitWidth() : 0;
                switch (x.kind) {
                    case ExpressionKind::NamedValue:
                    case ExpressionKind::ElementSelect:
                    case ExpressionKind::RangeSelect:
                    case ExpressionKind::MemberAccess:
                    case ExpressionKind::Concatenation:
                    case ExpressionKind::Replication: leaf = &x != &x0; break;
                    default: leaf = false;
                }
                wrap = sp.ok && sp.end > sp.begin && varies && !x.getConstant() &&
                       x.type->isIntegral() && !hasExpensive(x, opt);
                sgn = x.type->isSigned();
            };
            side(e.left(), ic.a, ic.wrapA, ic.signedA, ic.leafA, ic.widthA);
            side(e.right(), ic.b, ic.wrapB, ic.signedB, ic.leafB, ic.widthB);
            ic.namedOk = named;
            if (ic.wrapA || ic.wrapB) {
                ic.line = sm.getLineNumber(sm.getFullyOriginalLoc(e.sourceRange.start()));
                out.push_back(std::move(ic));
            }
        }
        visitDefault(e);
    }
};

// ---------------------------------------------------------------- gathering

struct Gatherer {
    const SourceManager& sm;
    const Options& opt;
    Module& m;
    const std::unordered_set<const syntax::SyntaxNode*>& xref;  // values named from other modules

    void unknown(Sym s) {
        if (!s) return;
        m.drv[s].k = Driver::Unknown;
    }
    void comb(Sym s, const SymSet& deps) {
        Driver& d = m.drv[s];
        if (d.k == Driver::None || d.k == Driver::Comb) {
            d.k = Driver::Comb;
            d.deps.insert(deps.begin(), deps.end());
        } else
            d.k = Driver::Unknown;
    }

    // ---- sequential blocks

    struct Ctx {
        Blk* b = nullptr;
        std::vector<CP> conds;
        std::vector<Sym> condReads;
        int loop = 0;
        bool inReset = false, inSyncTop = false, comb = false;
    };

    CP path(const Ctx& c) { return cAnd(c.conds); }

    void walkExprStmt(const Statement& s, Ctx& c) {
        const Expression& e = s.as<ExpressionStatement>().expr;
        if (e.kind != ExpressionKind::Assignment) {
            if (!c.comb) c.b->bail = "a statement that is not an assignment";
            return;
        }
        auto& a = e.as<AssignmentExpression>();
        if (c.comb) {
            if (c.loop == 0) {
                IsoVisitor iv(sm, opt, path(c), c.condReads, c.b->iso);
                a.right().visit(iv);
            }
            return;
        }
        if (!a.isNonBlocking() || a.isCompound() || a.timingControl) {
            c.b->bail = a.isNonBlocking() ? "an assignment with a delay" : "a blocking assignment";
            return;
        }
        Nba n;
        if (!lhsTargets(a.left(), n.targets, n.reads)) {
            c.b->bail = "an assignment target that is not a plain variable";
            return;
        }
        for (Sym t : n.targets)
            if (!moduleLevel(*t)) {
                c.b->bail = "an assignment to a local variable";
                return;
            }
        n.span = s.syntax ? spanOf(sm, s.syntax->sourceRange()) : Span{};
        if (!n.span.ok) {
            c.b->bail = "text that comes from a macro";
            return;
        }
        n.cond = path(c);
        readsOf(a.right(), n.reads);
        n.reads.insert(c.condReads.begin(), c.condReads.end());
        n.inReset = c.inReset;
        n.inSyncTop = c.inSyncTop;
        n.rhsConst = a.right().getConstant() != nullptr;
        if (!n.rhsConst) {
            SymSet rs;
            readsOf(a.right(), rs);
            n.rhsConst = rs.empty();
        }
        auto lt = exprText(sm, a.left()), rt = exprText(sm, unwrap(a.right()));
        n.self = lt && rt && *lt == *rt;
        if (opt.iso && c.loop == 0 && !c.inReset) {
            IsoVisitor iv(sm, opt, n.cond, c.condReads, n.iso);
            a.right().visit(iv);
        }
        c.b->nbas.push_back(std::move(n));
    }

    // Pushes one branch condition; in a loop, or without text, the condition is
    // dropped (weakened to true), which is always safe for an enable.
    struct Pushed { size_t reads; };
    Pushed push(Ctx& c, const CP& cond, const SymSet& rs) {
        Pushed p{c.condReads.size()};
        c.conds.push_back(c.loop ? cTrue() : cond);
        c.condReads.insert(c.condReads.end(), rs.begin(), rs.end());
        return p;
    }
    void pop(Ctx& c, Pushed p) {
        c.conds.pop_back();
        c.condReads.resize(p.reads);
    }

    void walk(const Statement& s, Ctx& c) {
        if (!c.b->bail.empty()) return;
        if (c.comb || !s.syntax || s.kind == StatementKind::List || s.kind == StatementKind::Empty) {
            walk1(s, c);
            return;
        }
        size_t at = c.b->stmts.size();
        StmtNode n;
        n.span = spanOf(sm, s.syntax->sourceRange());
        n.inList = s.syntax->parent && syntax::BlockStatementSyntax::isKind(s.syntax->parent->kind);
        n.first = c.b->nbas.size();
        c.b->stmts.push_back(n);
        walk1(s, c);
        c.b->stmts[at].last = c.b->nbas.size();
    }

    void walk1(const Statement& s, Ctx& c) {
        switch (s.kind) {
            case StatementKind::Empty: return;
            case StatementKind::List:
                for (auto st : s.as<StatementList>().list) walk(*st, c);
                return;
            case StatementKind::Block: {
                auto& b = s.as<BlockStatement>();
                if (b.blockKind != StatementBlockKind::Sequential) {
                    if (!c.comb) c.b->bail = "a fork";
                    return;
                }
                if (s.syntax && syntax::BlockStatementSyntax::isKind(s.syntax->kind) &&
                    s.syntax->as<syntax::BlockStatementSyntax>().blockName)
                    c.b->hasLabel = true;
                if (s.syntax && s.syntax->label) c.b->hasLabel = true;
                walk(b.body, c);
                return;
            }
            case StatementKind::VariableDeclaration:
                if (!c.comb) c.b->bail = "a local variable";
                return;
            case StatementKind::ExpressionStatement: walkExprStmt(s, c); return;
            case StatementKind::Conditional: {
                auto& cs = s.as<ConditionalStatement>();
                if (cs.conditions.size() != 1 || cs.conditions[0].pattern) {
                    if (!c.comb) c.b->bail = "a pattern match";
                    return;
                }
                const Expression& ce = *cs.conditions[0].expr;
                SymSet rs;
                readsOf(ce, rs);
                auto t = exprText(sm, ce);
                bool usable = t && !rs.count(nullptr);
                CP atom = usable ? cAtom(*t) : cTrue();
                auto p = push(c, atom, rs);
                walk(cs.ifTrue, c);
                pop(c, p);
                if (cs.ifFalse) {
                    p = push(c, usable ? cNot(atom) : cTrue(), rs);
                    walk(*cs.ifFalse, c);
                    pop(c, p);
                }
                return;
            }
            case StatementKind::Case: {
                auto& cs = s.as<CaseStatement>();
                SymSet rs;
                readsOf(cs.expr, rs);
                auto st = exprText(sm, cs.expr);
                const char* eq = cs.condition == CaseStatementCondition::Normal ? "==" : "==?";
                bool usable = st && !rs.count(nullptr) && cs.condition != CaseStatementCondition::Inside;
                std::vector<CP> before;
                for (auto& g : cs.items) {
                    std::vector<CP> alts;
                    bool ok = usable;
                    for (auto ie : g.expressions) {
                        readsOf(*ie, rs);
                        auto it = exprText(sm, *ie);
                        // casez with an x in an item: ==? would take the x as a wildcard, casez does not.
                        if (cs.condition == CaseStatementCondition::WildcardJustZ && !noX(*ie)) ok = false;
                        if (!it) ok = false;
                        else alts.push_back(cAtom(paren(*st) + " " + eq + " " + paren(*it)));
                    }
                    if (!ok) usable = false;
                    CP match = usable ? cOr(alts) : cTrue();
                    CP here = usable ? cAnd({match, cNot(cOr(before))}) : cTrue();
                    auto p = push(c, here, rs);
                    walk(*g.stmt, c);
                    pop(c, p);
                    before.push_back(match);
                }
                if (cs.defaultCase) {
                    auto p = push(c, usable ? cNot(cOr(before)) : cTrue(), rs);
                    walk(*cs.defaultCase, c);
                    pop(c, p);
                }
                return;
            }
            case StatementKind::ForLoop: {
                auto& f = s.as<ForLoopStatement>();
                SymSet rs;
                for (auto i : f.initializers) readsOf(*i, rs);
                for (auto v : f.loopVars)
                    if (auto i = v->getInitializer()) readsOf(*i, rs);
                if (f.stopExpr) readsOf(*f.stopExpr, rs);
                c.loop++;
                auto p = push(c, cTrue(), rs);
                walk(f.body, c);
                pop(c, p);
                c.loop--;
                return;
            }
            default:
                if (!c.comb) c.b->bail = "a statement mpopt does not rewrite";
                return;
        }
    }

    // An operand of a comparison mpopt writes: parentheses unless it is one token.
    static std::string paren(const std::string& t) {
        for (char ch : t)
            if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == '\'' || ch == '$' || ch == '.' || ch == ':'))
                return "(" + t + ")";
        return t;
    }

    // True for a constant with no x bit.
    static bool noX(const Expression& e) {
        const Expression& u = unwrap(e);
        const ConstantValue* cv = u.getConstant();
        SVInt v;
        if (cv && cv->isInteger()) v = cv->integer();
        else if (u.kind == ExpressionKind::IntegerLiteral) v = u.as<IntegerLiteral>().getValue();
        else if (u.kind == ExpressionKind::NamedValue) {
            auto& sym = u.as<NamedValueExpression>().symbol;
            const ConstantValue* sv = sym.kind == SymbolKind::Parameter ? &sym.as<ParameterSymbol>().getValue()
                                      : sym.kind == SymbolKind::EnumValue ? &sym.as<EnumValueSymbol>().getValue()
                                      : nullptr;
            if (!sv || !sv->isInteger()) return false;
            v = sv->integer();
        } else
            return false;
        if (!v.hasUnknown()) return true;
        for (bitwidth_t i = 0; i < v.getBitWidth(); i++)
            if (exactlyEqual(v[(int32_t)i], logic_t::x)) return false;
        return true;
    }

    static const Statement& peel(const Statement& s) {
        const Statement* p = &s;
        for (;;) {
            if (p->kind == StatementKind::Block) p = &p->as<BlockStatement>().body;
            else if (p->kind == StatementKind::List && p->as<StatementList>().list.size() == 1)
                p = p->as<StatementList>().list[0];
            else return *p;
        }
    }

    void gatherSeq(Blk& b, const TimedStatement& ts) {
        std::vector<const SignalEventControl*> evs;
        lintir::clocksOf(ts.timing, evs);
        if (evs.empty() || evs.size() > 2) { b.bail = "more than two edges in the sensitivity list"; return; }
        for (auto ev : evs) {
            if (ev->iffCondition) { b.bail = "an iff in the sensitivity list"; return; }
            if (ev->expr.kind != ExpressionKind::NamedValue ||
                !moduleLevel(ev->expr.as<NamedValueExpression>().symbol)) {
                b.bail = "a clock or reset that is not a plain signal";
                return;
            }
        }
        const Statement& top = peel(ts.stmt);
        const SignalEventControl* ck = evs[0];
        Ctx c;
        c.b = &b;
        if (evs.size() == 2) {
            if (top.kind != StatementKind::Conditional || top.as<ConditionalStatement>().conditions.size() != 1) {
                b.bail = "two edges but no reset test";
                return;
            }
            auto& cs = top.as<ConditionalStatement>();
            SymSet rs;
            readsOf(*cs.conditions[0].expr, rs);
            Sym s0 = &evs[0]->expr.as<NamedValueExpression>().symbol;
            Sym s1 = &evs[1]->expr.as<NamedValueExpression>().symbol;
            bool r0 = rs.count(s0), r1 = rs.count(s1);
            if (r0 == r1) { b.bail = "cannot tell the clock from the reset"; return; }
            const SignalEventControl* rst = r0 ? evs[0] : evs[1];
            ck = r0 ? evs[1] : evs[0];
            b.arst = &rst->expr.as<NamedValueExpression>().symbol;
            b.arstHigh = rst->edge == EdgeKind::PosEdge;
            b.clk = &ck->expr.as<NamedValueExpression>().symbol;
            // The reset branch is outside the enable; the rest starts from "true".
            if (s_hasLabel(ts.stmt)) b.hasLabel = true;
            c.inReset = true;
            auto p = push(c, cTrue(), rs);
            walk(cs.ifTrue, c);
            pop(c, p);
            c.inReset = false;
            if (cs.ifFalse) walk(*cs.ifFalse, c);
        } else {
            b.clk = &ck->expr.as<NamedValueExpression>().symbol;
            if (top.kind == StatementKind::Conditional && top.as<ConditionalStatement>().conditions.size() == 1 &&
                !top.as<ConditionalStatement>().conditions[0].pattern) {
                // Walk by hand so the first branch can be recognized as a synchronous reset.
                auto& cs = top.as<ConditionalStatement>();
                if (s_hasLabel(ts.stmt)) b.hasLabel = true;
                const Expression& ce = *cs.conditions[0].expr;
                SymSet rs;
                readsOf(ce, rs);
                auto t = exprText(sm, ce);
                bool usable = t && !rs.count(nullptr);
                CP atom = usable ? cAtom(*t) : cTrue();
                // What this condition reads is kept apart: when it is a
                // synchronous reset, the stability flop handles it by name.
                b.topReads = rs;
                size_t first = b.nbas.size();
                c.inSyncTop = true;
                auto p = push(c, atom, {});
                walk(cs.ifTrue, c);
                pop(c, p);
                c.inSyncTop = false;
                size_t last = b.nbas.size();
                if (cs.ifFalse) {
                    p = push(c, usable ? cNot(atom) : cTrue(), {});
                    walk(*cs.ifFalse, c);
                    pop(c, p);
                }
                bool allConst = last > first && usable && rs.size() == 1 && cs.ifFalse;
                for (size_t i = first; i < last && allConst; i++) allConst = b.nbas[i].rhsConst;
                if (allConst) b.syncText = *t;
            } else
                walk(ts.stmt, c);
        }
        if (ck->edge != EdgeKind::PosEdge && ck->edge != EdgeKind::NegEdge) b.bail = "a clock with no edge";
        b.posedge = ck->edge == EdgeKind::PosEdge;
        b.clkSpan = spanOf(sm, ck->expr);
        if (!b.clkSpan.ok && b.bail.empty()) b.bail = "text that comes from a macro";
    }

    static bool s_hasLabel(const Statement& s) {
        const Statement* p = &s;
        while (p->kind == StatementKind::Block) {
            if (p->syntax && syntax::BlockStatementSyntax::isKind(p->syntax->kind) &&
                p->syntax->as<syntax::BlockStatementSyntax>().blockName)
                return true;
            p = &p->as<BlockStatement>().body;
        }
        return false;
    }

    // Reads and writes of a block mpopt does not gate (always_comb, initial, ...).
    void gatherLoose(Blk& b, const Statement& body) {
        body.visit(makeVisitor(
            [&](auto& v, const AssignmentExpression& a) {
                std::vector<Sym> t;
                SymSet r;
                if (!lhsTargets(a.left(), t, r)) {
                    std::vector<const Symbol*> ns;
                    lintir::namesIn(a.left(), ns);
                    for (auto n : ns) b.writes.insert(nullptr), unknown(n);
                }
                for (Sym s : t)
                    if (moduleLevel(*s)) b.writes.insert(s);
                b.reads.insert(r.begin(), r.end());
                readsOf(a.right(), b.reads);
                v.visitDefault(a);
            },
            [&](auto&, const NamedValueExpression& n) {
                if (moduleLevel(n.symbol)) b.reads.insert(&n.symbol);
            },
            [&](auto&, const HierarchicalValueExpression&) { b.reads.insert(nullptr); },
            [&](auto& v, const CallExpression& c) {
                if (!c.isSystemCall() || !pureSystemCall(c.getSubroutineName())) b.reads.insert(nullptr);
                v.visitDefault(c);
            }));
    }

    void block(const ProceduralBlockSymbol& p) {
        Blk b;
        b.sym = &p;
        b.syn = p.getSyntax();
        if (b.syn) {
            b.span = spanOf(sm, b.syn->sourceRange());
            if (b.syn->parent) {
                auto pk = b.syn->parent->kind;
                b.parentOk = pk == syntax::SyntaxKind::ModuleDeclaration || pk == syntax::SyntaxKind::GenerateBlock ||
                             pk == syntax::SyntaxKind::GenerateRegion;
            }
        }
        // Where the block is written: for one that a macro makes, where the macro is used.
        SourceLocation at = sm.getFullyExpandedLoc(p.location);
        b.line = sm.getLineNumber(at);
        b.col = sm.getColumnNumber(at);
        b.file = std::string(sm.getFileName(at));
        const Statement& body = p.getBody();
        bool edge = false, implicit = false;
        if (body.kind == StatementKind::Timed) {
            auto& tc = body.as<TimedStatement>().timing;
            std::vector<const SignalEventControl*> evs;
            lintir::clocksOf(tc, evs);
            edge = !evs.empty();
            for (auto e : evs)
                if (e->edge != EdgeKind::PosEdge && e->edge != EdgeKind::NegEdge) edge = false;
            implicit = tc.kind == TimingControlKind::ImplicitEvent;
        }
        if ((p.procedureKind == ProceduralBlockKind::AlwaysFF || p.procedureKind == ProceduralBlockKind::Always) && edge) {
            b.kind = Blk::Seq;
            gatherSeq(b, body.as<TimedStatement>());
            if (!b.span.ok && b.bail.empty()) b.bail = "text that comes from a macro";
            if (!b.parentOk && b.bail.empty()) b.bail = "a block that is the only item of a generate construct";
            // Whatever happened, the dataflow needs every target of the block.
            gatherLoose(b, body);
            // The first token's line start gives the indentation.
            if (b.span.ok) {
                auto text = sm.getSourceText(BufferID(b.span.buf, ""sv));
                size_t ls = b.span.begin;
                while (ls > 0 && text[ls - 1] != '\n') ls--;
                std::string ind(text.substr(ls, b.span.begin - ls));
                bool blank = true;
                for (char ch : ind)
                    if (ch != ' ' && ch != '\t') blank = false;
                b.indent = blank ? ind : "";
            }
        } else if (p.procedureKind == ProceduralBlockKind::AlwaysComb ||
                   p.procedureKind == ProceduralBlockKind::AlwaysLatch ||
                   (p.procedureKind == ProceduralBlockKind::Always && implicit)) {
            b.kind = Blk::Comb;
            const Statement& st = body.kind == StatementKind::Timed ? body.as<TimedStatement>().stmt : body;
            gatherLoose(b, st);
            if (opt.iso && p.procedureKind != ProceduralBlockKind::AlwaysLatch) {
                Ctx c;
                c.b = &b;
                c.comb = true;
                walk(st, c);
                // A condition whose signals the block itself assigns may have
                // changed by the time the operator is reached.
                std::vector<IsoCand> keep;
                for (auto& ic : b.iso) {
                    bool ok = true;
                    for (Sym s : ic.condReads)
                        if (!s || b.writes.count(s)) ok = false;
                    if (ok) keep.push_back(std::move(ic));
                }
                b.iso = std::move(keep);
            }
        } else {
            b.kind = Blk::Other;
            gatherLoose(b, body);
        }
        m.blks.push_back(std::move(b));
    }

    void scope(const Scope& sc) {
        for (auto& mem : sc.members()) {
            switch (mem.kind) {
                case SymbolKind::ProceduralBlock: block(mem.as<ProceduralBlockSymbol>()); break;
                case SymbolKind::ContinuousAssign: {
                    const Expression& e = mem.as<ContinuousAssignSymbol>().getAssignment();
                    if (e.kind != ExpressionKind::Assignment) break;
                    auto& a = e.as<AssignmentExpression>();
                    std::vector<Sym> t;
                    SymSet r;
                    if (!lhsTargets(a.left(), t, r)) {
                        std::vector<const Symbol*> ns;
                        lintir::namesIn(a.left(), ns);
                        for (auto n : ns) unknown(n);
                        break;
                    }
                    readsOf(a.right(), r);
                    for (Sym s : t) comb(s, r);
                    if (opt.iso) {
                        size_t n0 = m.assignIso.size();
                        IsoVisitor iv(sm, opt, cTrue(), {}, m.assignIso);
                        a.right().visit(iv);
                        // A declaration can go in front of the assign statement.
                        const syntax::SyntaxNode* st = mem.getSyntax();
                        while (st && st->kind != syntax::SyntaxKind::ContinuousAssign) st = st->parent;
                        Span sp;
                        if (st) sp = spanOf(sm, st->sourceRange());
                        if (sp.ok) {
                            auto text = sm.getSourceText(BufferID(sp.buf, ""sv));
                            size_t ls = sp.begin;
                            while (ls > 0 && text[ls - 1] != '\n') ls--;
                            std::string ind(text.substr(ls, sp.begin - ls));
                            for (char ch : ind)
                                if (ch != ' ' && ch != '\t') ind.clear();
                            for (size_t i = n0; i < m.assignIso.size(); i++) {
                                m.assignIso[i].decl = sp;
                                m.assignIso[i].indent = ind;
                            }
                        }
                    }
                    break;
                }
                case SymbolKind::Net: {
                    auto& n = mem.as<NetSymbol>();
                    if (auto init = n.getInitializer()) {
                        SymSet r;
                        readsOf(*init, r);
                        comb(&n, r);
                    }
                    break;
                }
                case SymbolKind::Port: {
                    auto& p = mem.as<PortSymbol>();
                    if (p.direction != ArgumentDirection::Out && p.internalSymbol) unknown(p.internalSymbol);
                    break;
                }
                case SymbolKind::MultiPort:
                case SymbolKind::InterfacePort: break;
                case SymbolKind::Instance: instance(mem.as<InstanceSymbol>()); break;
                case SymbolKind::InstanceArray: instArray(mem.as<InstanceArraySymbol>()); break;
                case SymbolKind::GenerateBlock:
                    if (!mem.as<GenerateBlockSymbol>().isUninstantiated) scope(mem.as<GenerateBlockSymbol>());
                    break;
                case SymbolKind::GenerateBlockArray:
                    for (auto e : mem.as<GenerateBlockArraySymbol>().entries)
                        if (!e->isUninstantiated) scope(*e);
                    break;
                case SymbolKind::Subroutine: {
                    // A task that assigns module variables: those are out of mpopt's hands.
                    auto& sub = mem.as<SubroutineSymbol>();
                    sub.getBody().visit(makeVisitor([&](auto& v, const AssignmentExpression& a) {
                        std::vector<const Symbol*> ns;
                        lintir::namesIn(a.left(), ns);
                        for (auto n : ns)
                            if (moduleLevel(*n)) unknown(n);
                        v.visitDefault(a);
                    }));
                    break;
                }
                case SymbolKind::Variable:
                    if (mem.getSyntax() && xref.count(mem.getSyntax())) unknown(&mem);
                    break;
                default: break;
            }
        }
    }

    void instArray(const InstanceArraySymbol& a) {
        for (auto e : a.elements) {
            if (e->kind == SymbolKind::Instance) instance(e->as<InstanceSymbol>());
            else if (e->kind == SymbolKind::InstanceArray) instArray(e->as<InstanceArraySymbol>());
        }
    }

    void instance(const InstanceSymbol& inst) {
        for (auto conn : inst.getPortConnections()) {
            const Expression* e = conn->getExpression();
            if (!e) continue;
            bool in = conn->port.kind == SymbolKind::Port &&
                      conn->port.as<PortSymbol>().direction == ArgumentDirection::In;
            if (in) continue;
            std::vector<const Symbol*> ns;
            lintir::namesIn(*e, ns);
            for (auto n : ns) unknown(n);
        }
    }
};

// ----------------------------------------------------------------- analysis

inline uint64_t bitsOf(Sym s) {
    if (s->kind != SymbolKind::Variable && s->kind != SymbolKind::Net) return 0;
    return s->as<ValueSymbol>().getType().getBitstreamWidth();
}

// Registers of each sequential block, grouped by their may-change condition.
inline void formGroups(Module& m, const Options& opt) {
    std::unordered_map<Sym, std::set<int>> writers;   // sequential blocks per register
    for (size_t bi = 0; bi < m.blks.size(); bi++) {
        Blk& b = m.blks[bi];
        if (b.kind != Blk::Seq) continue;
        for (Sym s : b.writes)
            if (s) writers[s].insert((int)bi);
    }
    for (size_t bi = 0; bi < m.blks.size(); bi++) {
        Blk& b = m.blks[bi];
        if (b.kind != Blk::Seq) continue;
        // Registers in source order; a concatenation on the left ties its targets together.
        std::vector<Sym> order;
        std::unordered_map<Sym, Sym> up;
        std::function<Sym(Sym)> find = [&](Sym s) {
            while (up[s] != s) s = up[s] = up[up[s]];
            return s;
        };
        auto note = [&](Sym s) {
            if (!up.count(s)) {
                up[s] = s;
                order.push_back(s);
            }
        };
        if (b.bail.empty()) {
            for (auto& n : b.nbas) {
                for (Sym t : n.targets) note(t);
                for (size_t i = 1; i < n.targets.size(); i++) up[find(n.targets[i])] = find(n.targets[0]);
            }
        } else {
            for (Sym s : b.writes)
                if (s) note(s);
            // One group: nothing is known about a block that was not walked.
            std::sort(order.begin(), order.end(), [](Sym a, Sym c) { return a->location < c->location; });
            for (size_t i = 1; i < order.size(); i++) up[find(order[i])] = find(order[0]);
        }
        // Per register: the OR of the conditions of the assignments that can change it.
        std::unordered_map<Sym, std::vector<CP>> conds;
        if (b.bail.empty())
            for (auto& n : b.nbas) {
                if (n.inReset || n.self) continue;
                for (Sym t : n.targets) conds[find(t)].push_back(n.cond);
            }
        std::map<std::string, int> byKey;
        std::unordered_map<Sym, int> rootGroup;
        for (Sym s : order) {
            Sym r = find(s);
            if (!rootGroup.count(r)) {
                CP e = b.bail.empty() ? cOr(conds[r]) : cTrue();
                auto it = byKey.find(e->key);
                if (it == byKey.end()) {
                    Group g;
                    g.blk = (int)bi;
                    g.e0 = g.L = e;
                    it = byKey.emplace(e->key, (int)m.groups.size()).first;
                    b.groups.push_back((int)m.groups.size());
                    m.groups.push_back(std::move(g));
                }
                rootGroup[r] = it->second;
            }
            Group& g = m.groups[(size_t)rootGroup[r]];
            g.regs.push_back(s);
            // An array written one element per block (a generate loop): each block has its share.
            size_t nw = std::max<size_t>(1, writers[s].size());
            g.bits += bitsOf(s) / nw;
            if (nw > 1) g.bad = true;
            else if (s->as<ValueSymbol>().getType().isUnpackedArray()) g.mem = true;
            if (!m.regGroup.count(s)) m.regBits += bitsOf(s);
            m.regGroup[s] = rootGroup[r];
        }
        if (!b.bail.empty())
            for (int gi : b.groups) m.groups[(size_t)gi].bad = true;
    }
    // Drivers: a register is driven by its group, unless something else drives it too.
    for (auto& [s, gi] : m.regGroup) {
        Driver& d = m.drv[s];
        if (d.k == Driver::None) {
            d.k = Driver::Reg;
            d.group = gi;
        } else {
            d.k = Driver::Unknown;
            m.groups[(size_t)gi].bad = true;
        }
    }
    for (auto& b : m.blks) {
        if (b.kind == Blk::Comb) {
            for (Sym s : b.writes) {
                if (!s) continue;
                Driver& d = m.drv[s];
                if (d.k == Driver::None || d.k == Driver::Comb) {
                    d.k = Driver::Comb;
                    d.deps.insert(b.reads.begin(), b.reads.end());
                    d.deps.erase(s);   // a temporary read back in its own block
                } else {
                    if (d.k == Driver::Reg) m.groups[(size_t)d.group].bad = true;
                    d.k = Driver::Unknown;
                }
            }
        } else if (b.kind == Blk::Other) {
            for (Sym s : b.writes) {
                if (!s) continue;
                Driver& d = m.drv[s];
                if (d.k == Driver::Reg) m.groups[(size_t)d.group].bad = true;
                d.k = Driver::Unknown;
            }
        }
    }
    (void)opt;
}

// The register groups a value depends on, through combinational logic.
// first == false: it depends on something that cannot be followed.
inline const std::pair<bool, std::vector<int>>& sourcesOf(Module& m, Sym s) {
    auto it = m.srcMemo.find(s);
    if (it != m.srcMemo.end()) return it->second;
    static const std::pair<bool, std::vector<int>> bad{false, {}};
    if (!s) return bad;
    if (!m.srcBusy.insert(s).second) return bad;   // a combinational loop
    std::pair<bool, std::vector<int>> r{true, {}};
    auto dit = m.drv.find(s);
    Driver::K k = dit == m.drv.end() ? Driver::None : dit->second.k;
    if (k == Driver::Unknown) r.first = false;
    else if (k == Driver::Reg) r.second.push_back(dit->second.group);
    else if (k == Driver::Comb) {
        std::set<int> all;
        for (Sym d : dit->second.deps) {
            auto& sub = sourcesOf(m, d);
            if (!sub.first) { r.first = false; break; }
            all.insert(sub.second.begin(), sub.second.end());
            if (all.size() > 64) { r.first = false; break; }
        }
        if (r.first) r.second.assign(all.begin(), all.end());
    } else if (s->kind == SymbolKind::Net || s->kind == SymbolKind::Variable) {
        // Nothing drives it here: a constant (or x) for the whole run.
    }
    m.srcBusy.erase(s);
    return m.srcMemo[s] = std::move(r);
}

inline std::string lineName(const Module& m, const Blk& b, const Group& g, const char* what, const Options& opt) {
    std::string n = opt.prefix + "_" + what + "_L" + std::to_string(b.line);
    if (b.sharesLine) n += "c" + std::to_string(b.col);
    if (b.groups.size() > 1) {
        size_t k = 0;
        while (k < b.groups.size() && &m.groups[(size_t)b.groups[k]] != &g) k++;
        n += "_" + std::to_string(k);
    }
    return n;
}

// Sequential (stability) enables.
inline void findStability(Module& m, const Options& opt) {
    if (!opt.seq) return;
    auto sources = [&](Group& g) {
        if (g.srcDone) return;
        g.srcDone = true;
        Blk& b = m.blks[(size_t)g.blk];
        std::set<int> all;
        std::unordered_set<Sym> mine(g.regs.begin(), g.regs.end());
        for (auto& n : b.nbas) {
            bool hit = false;
            for (Sym t : n.targets)
                if (mine.count(t)) hit = true;
            if (!hit || n.inReset) continue;
            for (Sym r : n.reads) {
                auto& s = sourcesOf(m, r);
                if (!s.first) { g.srcUnknown = true; return; }
                all.insert(s.second.begin(), s.second.end());
            }
        }
        if (b.syncText.empty())
            for (Sym r : b.topReads) {
                auto& s = sourcesOf(m, r);
                if (!s.first) { g.srcUnknown = true; return; }
                all.insert(s.second.begin(), s.second.end());
            }
        g.srcs.assign(all.begin(), all.end());
    };
    for (unsigned round = 0; round < opt.seqDepth; round++) {
        bool any = false;
        for (size_t gi = 0; gi < m.groups.size(); gi++) {
            Group& g = m.groups[gi];
            Blk& b = m.blks[(size_t)g.blk];
            if (g.hasSt || g.bad || !b.bail.empty() || b.multi) continue;
            if (g.bits < opt.seqMinBits) { g.stWhy = "too few bits"; continue; }
            if (g.e0->k == Cond::F) continue;
            sources(g);
            if (g.srcUnknown) { g.stWhy = "its inputs are not all registers of this module"; continue; }
            if (g.srcs.empty()) { g.stWhy = "it loads constants"; continue; }
            std::string why;
            std::vector<CP> ors;
            Sym arst = b.arst;
            bool arstHigh = b.arstHigh;
            std::string sync = b.syncText;
            int depth = 0;
            size_t pos = b.span.end;
            std::vector<int> deps;
            for (int si : g.srcs) {
                Group& sg = m.groups[(size_t)si];
                Blk& sb = m.blks[(size_t)sg.blk];
                if (si == (int)gi) { why = "it reads its own value"; break; }
                if (sg.bad || !sb.bail.empty()) { why = "a source register is not understood"; break; }
                if (sb.clk != b.clk || sb.posedge != b.posedge) { why = "a source register is on another clock"; break; }
                if (!sb.syn || !b.syn || sb.syn->parent != b.syn->parent || sb.span.buf != b.span.buf || sb.multi) {
                    why = "a source register is in another scope";
                    break;
                }
                if (sg.L->k == Cond::T) { why = "a source register loads every cycle"; break; }
                if (sb.arst) {
                    if (arst && (arst != sb.arst || arstHigh != sb.arstHigh)) { why = "two asynchronous resets"; break; }
                    arst = sb.arst;
                    arstHigh = sb.arstHigh;
                }
                if (sync.empty()) sync = sb.syncText;
                ors.push_back(sg.L);
                depth = std::max(depth, sg.depth);
                pos = std::max(pos, sb.span.end);
                if (sg.hasSt) {
                    pos = std::max(pos, sg.stPos);
                    deps.push_back(si);
                }
            }
            // The block's own synchronous reset is not a source: the gate opens on it by name.
            if (!b.syncText.empty()) sync = b.syncText;
            else if (arst) sync.clear();
            if (why.empty() && !arst && sync.empty()) why = "no reset to start the stability flop from";
            if (why.empty() && depth + 1 > (int)opt.seqDepth) why = "the enable chain is too deep";
            if (!sync.empty()) ors.insert(ors.begin(), cAtom(sync));
            CP next = cOr(ors);
            if (why.empty() && (next->k == Cond::T || cTerms(next) > opt.maxEnableTerms))
                why = "the sources load in too many cases";
            if (!why.empty()) {
                // A source that has no enable yet may get one in a later round.
                g.stWhy = why;
                continue;
            }
            g.hasSt = true;
            g.stWhy.clear();
            g.stName = lineName(m, b, g, "st", opt);
            g.stNext = next;
            g.stArst = arst;
            g.stArstHigh = arstHigh;
            g.stSync = sync;
            g.stPos = pos;
            g.stDeps = deps;
            g.depth = depth + 1;
            CP st = cAtom(g.stName);
            if (!sync.empty()) st = cOr({st, cAtom(sync)});
            g.L = cAnd({g.e0, st});
            any = true;
        }
        if (!any) break;
    }
}

// Applies edits that lie inside [base, base+text.size()) of a buffer to a copy of that text.
inline std::string applyLocal(std::string_view text, size_t base, std::vector<Edit> es) {
    std::stable_sort(es.begin(), es.end(), [](const Edit& a, const Edit& b) {
        if (a.begin != b.begin) return a.begin < b.begin;
        if ((a.end - a.begin) != (b.end - b.begin)) return (a.end - a.begin) > (b.end - b.begin);
        return a.seq < b.seq;
    });
    std::string out;
    size_t at = base;
    for (auto& e : es) {
        if (e.begin < at) continue;   // inside a replaced statement
        out.append(text.substr(at - base, e.begin - at));
        out += e.text;
        at = e.end;
    }
    out.append(text.substr(at - base));
    return out;
}

// The isolating condition is compared with !== 1'b0 so that an x in it lets
// the operand through, as the original does when it takes an else or a
// default on an x: the rewritten RTL then simulates the same, x for x.
// The edits of one isolated operator. With names, each operand becomes a
// signal declared in decl (a power tool then sees its activity, and the
// operator keeps the operand's own width); without, the expression is
// rewritten in place.
inline void isoEdits(const SourceManager& sm, const IsoCand& ic, std::vector<Edit>& es, const Options& opt,
                     const std::string& nameA = "", const std::string& nameB = "", std::string* decl = nullptr,
                     const std::string& ind = "") {
    std::string c = cText(ic.cond, false) + (opt.isoPlain ? "" : " !== 1'b0");
    auto wrap = [&](const Span& sp, bool sgn, bool leaf, unsigned width, const std::string& name) {
        Edit e;
        e.buf = sp.buf;
        e.begin = sp.begin;
        e.end = sp.end;
        std::string iso = "(" + c + " ? " + std::string(rawText(sm, sp)) + (sgn ? " : $signed(1'b0))" : " : 1'b0)");
        if (decl && !name.empty() && width) {
            *decl += "wire " + std::string(sgn ? "signed " : "") + "[" + std::to_string(width - 1) + ":0] " + name + " = " +
                     iso.substr(1, iso.size() - 2) + ";\n" + ind;
            e.text = name;
        } else if (leaf)
            // A concatenation is as wide as its operand: the operator is not widened by the ?:.
            e.text = sgn ? "$signed({" + iso + "})" : "{" + iso + "}";
        else
            e.text = iso;
        es.push_back(std::move(e));
    };
    if (ic.wrapA) wrap(ic.a, ic.signedA, ic.leafA, ic.widthA, nameA);
    if (ic.wrapB) wrap(ic.b, ic.signedB, ic.leafB, ic.widthB, nameB);
}

inline std::string regNames(const Group& g) {
    std::string s;
    for (size_t i = 0; i < g.regs.size(); i++) {
        if (i == 4) { s += ", ... (" + std::to_string(g.regs.size()) + " in all)"; break; }
        s += (i ? ", " : "") + std::string(g.regs[i]->name);
    }
    return s;
}

// Decides what to gate and writes the edits.
inline void plan(const SourceManager& sm, Module& m, const Options& opt, bool allowSeq) {
    m.edits.clear();
    m.finds.clear();
    m.gatedBits = 0;
    m.usesIcg = false;
    int seq = 0;
    auto fileOf = [&](uint32_t buf) { return std::string(sm.getFileName(SourceLocation(BufferID(buf, ""sv), 0))); };
    auto teText = [&]() -> std::string {
        if (opt.testEnable.empty()) return "1'b0";
        if (m.body->find(opt.testEnable)) return opt.testEnable;
        return "1'b0";
    };
    auto icgInst = [&](const std::string& ind, const std::string& name, const std::string& clk,
                       const std::string& en, const std::string& gclk) {
        std::string cell = opt.icgCell.empty() ? opt.prefix + "_icg" : opt.icgCell;
        return ind + cell + " " + name + " (." + opt.icgClk + "(" + clk + "), ." + opt.icgEn + "(" + en + "), ." +
               opt.icgTe + "(" + teText() + "), ." + opt.icgOut + "(" + gclk + "));\n";
    };

    std::vector<std::vector<int>> gates(m.blks.size());
    std::vector<char> wholes(m.blks.size());
    std::vector<std::vector<Edit>> isos(m.blks.size());
    std::vector<std::string> isoDecls(m.blks.size());
    std::unordered_map<std::string, int> isoNames;
    auto isoName = [&](const IsoCand& ic, const char* side) {
        std::string n = opt.prefix + "_iso_L" + std::to_string(ic.line);
        int k = isoNames[n + side]++;
        return n + (k ? "_" + std::to_string(k) : "") + side;
    };
    for (size_t bi = 0; bi < m.blks.size(); bi++) {
        Blk& b = m.blks[bi];
        if (b.kind != Blk::Seq) continue;
        std::vector<int>& gate = gates[bi];
        for (int gi : b.groups) {
            Group& g = m.groups[(size_t)gi];
            g.gated = false;
            g.planSt = g.stEmit = false;
            g.skip.clear();
            bool useSt = allowSeq && g.hasSt;
            CP en = useSt ? g.L : g.e0;
            g.kind = g.mem ? "MG" : useSt ? "SCG" : "CG";
            if (useSt && cTerms(en) > opt.maxEnableTerms) {   // fall back to the written enable
                useSt = false;
                en = g.e0;
                if (!g.mem) g.kind = "CG";
            }
            if (!opt.cg) g.skip = "clock gating is off";
            else if (!b.bail.empty()) g.skip = "the block has " + b.bail;
            else if (!b.posedge) g.skip = "the clock is a falling edge";
            else if (en->k == Cond::F) g.skip = "it is only assigned in reset";
            else if (en->k == Cond::T)
                g.skip = "it loads every cycle" + (g.stWhy.empty() ? std::string() : " and " + g.stWhy);
            else if (!useSt && g.bits < opt.minBits) g.skip = "too few bits";
            else if (cTerms(en) > opt.maxEnableTerms) g.skip = "the enable has too many terms";
            if (g.skip.empty()) {
                g.enable = en;
                g.planSt = useSt;
                g.gclk = lineName(m, b, g, "gclk", opt);
                gate.push_back(gi);
            }
        }
        // Isolation edits inside this block, by statement.
        std::vector<Edit>& iso = isos[bi];
        if (opt.iso && b.bail.empty())
            for (auto& n : b.nbas)
                for (auto& ic : n.iso) {
                    if (ic.namedOk && !opt.isoInline)
                        isoEdits(sm, ic, iso, opt, isoName(ic, "_a"), isoName(ic, "_b"), &isoDecls[bi], b.indent);
                    else
                        isoEdits(sm, ic, iso, opt);
                    Finding f;
                    f.kind = "OI";
                    f.file = fileOf(b.span.buf);
                    f.line = ic.line;
                    f.module = m.name;
                    f.names = "operator " + ic.op;
                    f.bits = ic.bits;
                    f.enable = cText(ic.cond);
                    f.applied = true;
                    m.finds.push_back(std::move(f));
                }
        bool whole = gate.size() == 1;   // one gate covers every assignment outside reset
        if (whole)
            for (int gi : b.groups)
                if (gi != gate[0] && m.groups[(size_t)gi].e0->k != Cond::F) whole = false;
        if (!gate.empty() && !whole && (!opt.split || b.hasLabel)) {
            // Cannot split: gate the block as a whole if one enable covers it.
            std::vector<CP> all;
            uint64_t bits = 0;
            bool ok = true;
            for (int gi : b.groups) {
                Group& g = m.groups[(size_t)gi];
                if (g.e0->k == Cond::F) continue;
                all.push_back(g.e0);
                bits += g.bits;
            }
            CP en = cOr(all);
            std::string why = !opt.split ? "splitting is off" : "the block has a named begin";
            for (int gi : gate) m.groups[(size_t)gi].skip = why + ", and the block's registers have different enables";
            gate.clear();
            if (ok && en->k != Cond::T && en->k != Cond::F && bits >= opt.minBits && cTerms(en) <= opt.maxEnableTerms) {
                // Fold every group into the first as one gate.
                Group& g0 = m.groups[(size_t)b.groups[0]];
                for (int gi : b.groups) m.groups[(size_t)gi].skip = "gated with the whole block";
                g0.skip.clear();
                g0.enable = en;
                g0.kind = "CG";
                g0.gclk = opt.prefix + "_gclk_L" + std::to_string(b.line) + (b.sharesLine ? "c" + std::to_string(b.col) : "");
                g0.planSt = false;
                gate.push_back(b.groups[0]);
                whole = true;
            }
        }
        // Findings for every group, gated or not.
        for (int gi : b.groups) {
            Group& g = m.groups[(size_t)gi];
            bool on = std::find(gate.begin(), gate.end(), gi) != gate.end();
            Finding f;
            f.kind = g.kind;
            f.file = b.file;
            f.line = b.line;
            f.module = m.name;
            f.names = regNames(g);
            f.bits = g.bits;
            f.applied = on;
            if (on) {
                f.enable = cText(g.enable);
                if (whole && gi == gate[0] && b.groups.size() > 1) {
                    f.bits = 0;
                    f.names.clear();
                    for (int gj : b.groups) {
                        if (m.groups[(size_t)gj].e0->k == Cond::F) continue;
                        f.bits += m.groups[(size_t)gj].bits;
                        f.names += (f.names.empty() ? "" : ", ") + regNames(m.groups[(size_t)gj]);
                    }
                }
                if (g.planSt)
                    f.note = "stable unless " + cText(g.stNext) + " held in the cycle before";
                m.gatedBits += f.bits;
            } else {
                if (g.skip == "gated with the whole block") continue;
                f.note = g.skip;
            }
            m.finds.push_back(std::move(f));
        }
        wholes[bi] = whole;
    }
    // A stability flop is written when its group is gated with it, or when a
    // flop that is written reads it.
    for (auto& g : m.groups) g.stEmit = g.planSt;
    for (bool more = true; more;) {
        more = false;
        for (auto& g : m.groups)
            if (g.stEmit)
                for (int d : g.stDeps)
                    if (!m.groups[(size_t)d].stEmit) m.groups[(size_t)d].stEmit = more = true;
    }
    auto stLogic = [&](const Blk& b, const Group& g, const std::string& ind) {
        std::string logic = ind + "reg " + g.stName + ";\n";
        std::string edge = std::string(b.posedge ? "posedge " : "negedge ") + std::string(b.clk->name);
        if (g.stArst) {
            std::string rn(g.stArst->name);
            logic += ind + "always @(" + edge + " or " + (g.stArstHigh ? "posedge " : "negedge ") + rn + ")\n";
            logic += ind + "  if (" + (g.stArstHigh ? "" : "!") + rn + ") " + g.stName + " <= 1'b1;\n";
            logic += ind + "  else " + g.stName + " <= " + cText(g.stNext) + ";\n";
        } else
            logic += ind + "always @(" + edge + ") " + g.stName + " <= " + cText(g.stNext) + ";\n";
        return logic;
    };
    for (size_t bi = 0; bi < m.blks.size(); bi++) {
        Blk& b = m.blks[bi];
        if (b.kind != Blk::Seq) continue;
        std::string clkName = b.clk ? std::string(b.clk->name) : "";
        std::vector<int>& gate = gates[bi];
        std::vector<Edit>& iso = isos[bi];
        bool whole = wholes[bi];
        // Flops other groups need, of groups that are not gated themselves.
        for (int gi : b.groups) {
            Group& g = m.groups[(size_t)gi];
            if (!g.stEmit || g.planSt) continue;
            Edit e;
            e.buf = b.span.buf;
            e.begin = e.end = g.stPos;
            e.seq = 1000000 + (int)gi * 4 + g.depth * 100000;
            e.text = "\n" + b.indent + "// " + opt.prefix + ": " + regNames(g) + " can only change after this is 1\n" +
                     stLogic(b, g, b.indent);
            while (!e.text.empty() && e.text.back() == '\n') e.text.pop_back();
            m.edits.push_back(std::move(e));
        }
        if (gate.empty()) {
            // Only isolation, if any: edit in place.
            for (auto& e : iso) { e.seq = seq++; m.edits.push_back(e); }
            if (!isoDecls[bi].empty()) {
                Edit e;
                e.buf = b.span.buf;
                e.begin = e.end = b.span.begin;
                e.seq = seq++;
                e.text = isoDecls[bi];
                m.edits.push_back(std::move(e));
            }
            continue;
        }
        m.usesIcg = true;
        std::string_view text = rawText(sm, b.span);
        std::string ind = b.indent;
        std::string decls = isoDecls[bi], after;
        auto render = [&](int keep, bool gatedClock, const std::string& gclk) {
            // keep >= 0: only that group's assignments; -1: everything; -2: all but the gated groups.
            std::vector<Edit> es = iso;
            if (gatedClock) {
                Edit e;
                e.begin = b.clkSpan.begin;
                e.end = b.clkSpan.end;
                e.text = gclk;
                es.push_back(e);
            }
            if (keep != -1) {
                std::vector<char> kept(b.nbas.size());
                for (size_t i = 0; i < b.nbas.size(); i++) {
                    int ng = m.regGroup[b.nbas[i].targets[0]];
                    bool isGated = std::find(gate.begin(), gate.end(), ng) != gate.end();
                    kept[i] = keep >= 0 ? ng == keep : !isGated;
                }
                for (size_t si = 0; si < b.stmts.size(); si++) {
                    const StmtNode& sn = b.stmts[si];
                    if (sn.first == sn.last || !sn.span.ok) continue;
                    bool any = false;
                    for (size_t i = sn.first; i < sn.last; i++) any |= (bool)kept[i];
                    if (any) continue;
                    Edit e;
                    e.begin = sn.span.begin;
                    e.end = sn.span.end;
                    if (sn.inList) {
                        // Take the line with it when nothing else is on it.
                        size_t lb = e.begin, le = e.end, base = b.span.begin;
                        while (lb > base && (text[lb - 1 - base] == ' ' || text[lb - 1 - base] == '\t')) lb--;
                        while (le - base < text.size() && (text[le - base] == ' ' || text[le - base] == '\t')) le++;
                        if (lb > base && text[lb - 1 - base] == '\n' && le - base < text.size() && text[le - base] == '\n') {
                            e.begin = lb;
                            e.end = le + 1;
                        }
                    } else
                        e.text = "begin end";
                    es.push_back(e);
                }
            }
            return applyLocal(text, b.span.begin, std::move(es));
        };
        std::string body;
        for (int gi : gate) {
            Group& g = m.groups[(size_t)gi];
            decls += "wire " + g.gclk + ";\n" + ind;
            std::string logic;
            bool useSt = g.planSt;
            logic += ind + "// " + opt.prefix + ": " + (useSt ? "sequential " : "") + "clock gate for " + regNames(g) + "\n";
            if (useSt) logic += stLogic(b, g, ind);
            std::string inst = opt.prefix + "_icg" + g.gclk.substr(opt.prefix.size() + 5);
            logic += icgInst(ind, inst, clkName, cText(g.enable), g.gclk);
            if (useSt && g.stPos > b.span.end) {
                Edit e;
                e.buf = b.span.buf;
                e.begin = e.end = g.stPos;
                e.seq = 1000000 + (int)gi * 4 + g.depth * 100000;   // sources' logic first
                e.text = "\n" + logic;
                while (!e.text.empty() && e.text.back() == '\n') e.text.pop_back();
                m.edits.push_back(std::move(e));
            } else
                after += logic;
        }
        if (whole) body = render(-1, true, m.groups[(size_t)gate[0]].gclk);
        else {
            bool rest = false;
            for (int gi : b.groups)
                if (std::find(gate.begin(), gate.end(), gi) == gate.end()) rest = true;
            if (rest) body = render(-2, false, "");
            for (int gi : gate) {
                if (!body.empty()) body += "\n" + ind;
                body += render(gi, true, m.groups[(size_t)gi].gclk);
            }
        }
        Edit e;
        e.buf = b.span.buf;
        e.begin = b.span.begin;
        e.end = b.span.end;
        e.seq = seq++;
        while (!after.empty() && after.back() == '\n') after.pop_back();
        e.text = decls + body + (after.empty() ? "" : "\n" + after);
        m.edits.push_back(std::move(e));
    }
    // Isolation outside sequential blocks.
    auto loose = [&](const std::vector<IsoCand>& v) {
        for (auto& ic : v) {
            std::vector<Edit> es;
            if (ic.namedOk && ic.decl.ok && !opt.isoInline) {
                Edit d;
                d.buf = ic.decl.buf;
                d.begin = d.end = ic.decl.begin;
                isoEdits(sm, ic, es, opt, isoName(ic, "_a"), isoName(ic, "_b"), &d.text, ic.indent);
                d.seq = seq++;
                m.edits.push_back(std::move(d));
            } else
                isoEdits(sm, ic, es, opt);
            for (auto& e : es) { e.seq = seq++; m.edits.push_back(e); }
            Finding f;
            f.kind = "OI";
            f.file = fileOf(ic.wrapA ? ic.a.buf : ic.b.buf);
            f.line = ic.line;
            f.module = m.name;
            f.names = "operator " + ic.op;
            f.bits = ic.bits;
            f.enable = cText(ic.cond);
            f.applied = true;
            m.finds.push_back(std::move(f));
        }
    };
    if (opt.iso) {
        loose(m.assignIso);
        for (auto& b : m.blks)
            if (b.kind == Blk::Comb) loose(b.iso);
    }
    std::stable_sort(m.edits.begin(), m.edits.end(), [](const Edit& a, const Edit& b) {
        return std::tie(a.buf, a.begin, a.seq) < std::tie(b.buf, b.begin, b.seq);
    });
    // The copies of a generate loop report once, with a count.
    std::vector<Finding> merged;
    std::unordered_map<std::string, size_t> at;
    for (auto& f : m.finds) {
        std::string k = f.key() + ":" + f.enable + ":" + f.note + (f.applied ? ":1" : ":0");
        auto it = at.find(k);
        if (it == at.end()) {
            at.emplace(k, merged.size());
            merged.push_back(std::move(f));
        } else if (f.kind == "OI")
            ;   // one operator in the text
        else
            merged[it->second].count++;
    }
    m.finds = std::move(merged);
}

inline void analyze(const SourceManager& sm, Module& m, const Options& opt,
                    const std::unordered_set<const syntax::SyntaxNode*>& xref) {
    Gatherer g{sm, opt, m, xref};
    g.scope(*m.body);
    // A block inside a generate loop is one piece of text and many blocks.
    std::unordered_map<const syntax::SyntaxNode*, int> seen;
    for (auto& b : m.blks)
        if (b.syn) seen[b.syn]++;
    std::unordered_set<const syntax::SyntaxNode*> first;
    std::vector<Blk> keep;
    for (auto& b : m.blks) {
        if (b.syn && seen[b.syn] > 1) b.multi = true;
        keep.push_back(std::move(b));
    }
    m.blks = std::move(keep);
    {
        std::map<std::pair<uint32_t, size_t>, std::set<const syntax::SyntaxNode*>> onLine;
        for (auto& b : m.blks)
            if (b.kind == Blk::Seq && b.span.ok) onLine[{b.span.buf, b.line}].insert(b.syn);
        for (auto& b : m.blks)
            if (b.kind == Blk::Seq && b.span.ok && onLine[{b.span.buf, b.line}].size() > 1) b.sharesLine = true;
    }
    formGroups(m, opt);
    findStability(m, opt);
    plan(sm, m, opt, true);
    // One piece of text, one edit: the copies of a generate loop must agree.
    std::vector<Edit> out;
    for (auto& e : m.edits) {
        if (!out.empty() && out.back().buf == e.buf && out.back().begin == e.begin && out.back().end == e.end &&
            e.end > e.begin) {
            if (out.back().text == e.text) continue;
            out.back().text.clear();   // marks a disagreement
            continue;
        }
        out.push_back(e);
    }
    std::vector<Edit> ok;
    for (auto& e : out)
        if (!(e.end > e.begin && e.text.empty())) ok.push_back(std::move(e));
    m.edits = std::move(ok);
}

// ------------------------------------------------------------------ design

struct Totals {
    uint64_t regBits = 0, cgBits = 0, scgBits = 0, mgBits = 0;
    size_t cg = 0, scg = 0, mg = 0, oi = 0, modules = 0, instances = 0;
};

struct Design {
    Totals tot;                       // over every instance of the design
    double msCollect = 0, msTouch = 0, msAnalyze = 0, msMerge = 0;
    std::vector<Module> mods;         // one per analyzed definition, in a fixed order
    std::vector<std::vector<Module>> variants;
    std::unordered_set<const syntax::SyntaxNode*> xref;
};

inline std::string paramSig(const InstanceBodySymbol& body) {
    std::string s;
    for (auto p : body.getParameters()) {
        s += std::string(p->symbol.name) + "=";
        if (p->symbol.kind == SymbolKind::Parameter) s += p->symbol.as<ParameterSymbol>().getValue().toString();
        else if (p->symbol.kind == SymbolKind::TypeParameter)
            s += p->symbol.as<TypeParameterSymbol>().targetType.getType().toString();
        s += ";";
    }
    return s;
}

// Forces lazy elaboration of one body on the calling thread, so that the
// parallel passes afterwards only read; also records values named from
// outside their module and assigned there.
struct Toucher : public ASTVisitor<Toucher, VisitFlags::AllGood> {
    std::unordered_set<const syntax::SyntaxNode*>& xref;
    explicit Toucher(std::unordered_set<const syntax::SyntaxNode*>& x) : xref(x) {}
    void handle(const InstanceSymbol& i) {
        for (auto c : i.getPortConnections())
            if (auto e = c->getExpression()) e->visit(*this);
    }
    void handle(const AssignmentExpression& a) {
        a.left().visit(makeVisitor([&](auto&, const HierarchicalValueExpression& h) {
            if (h.symbol.getSyntax()) xref.insert(h.symbol.getSyntax());
        }));
        visitDefault(a);
    }
};

struct Variant {
    std::string sig;
    const InstanceBodySymbol* body = nullptr;
    size_t count = 0;
};

inline void collectBodies(const InstanceSymbol& inst,
                          std::vector<const DefinitionSymbol*>& order,
                          std::unordered_map<const DefinitionSymbol*, std::vector<Variant>>& defs,
                          std::unordered_set<const InstanceBodySymbol*>& seen) {
    const InstanceBodySymbol& body = inst.body;
    if (!seen.insert(&body).second) return;
    auto& def = body.getDefinition();
    auto& v = defs[&def];
    if (v.empty()) order.push_back(&def);
    std::string sig = paramSig(body);
    bool have = false;
    for (auto& e : v)
        if (e.sig == sig) { have = true; e.count++; }
    if (!have) v.push_back({sig, &body, 1});
    std::function<void(const Scope&)> rec = [&](const Scope& sc) {
        for (auto& mem : sc.members()) {
            if (mem.kind == SymbolKind::Instance) collectBodies(mem.as<InstanceSymbol>(), order, defs, seen);
            else if (mem.kind == SymbolKind::InstanceArray) rec(mem.as<InstanceArraySymbol>());
            else if (mem.kind == SymbolKind::GenerateBlock) {
                if (!mem.as<GenerateBlockSymbol>().isUninstantiated) rec(mem.as<GenerateBlockSymbol>());
            } else if (mem.kind == SymbolKind::GenerateBlockArray)
                rec(mem.as<GenerateBlockArraySymbol>());
        }
    };
    rec(body);
}

inline Design run(Compilation& comp, const Options& opt) {
    const SourceManager& sm = *comp.getSourceManager();
    auto t0 = std::chrono::steady_clock::now();
    Design d;
    std::vector<const DefinitionSymbol*> order;
    std::unordered_map<const DefinitionSymbol*, std::vector<Variant>> defs;
    std::unordered_set<const InstanceBodySymbol*> seen;
    for (auto top : comp.getRoot().topInstances) collectBodies(*top, order, defs, seen);
    // Up to 8 parameterizations per module are compared; more than that and
    // only the rewrites that do not depend on the parameters are kept.
    struct Job { size_t def, var; const InstanceBodySymbol* body; };
    std::vector<Job> jobs;
    d.variants.resize(order.size());
    for (size_t i = 0; i < order.size(); i++) {
        auto& v = defs[order[i]];
        size_t n = std::min<size_t>(v.size(), 8);
        d.variants[i].resize(n);
        // The widest parameterizations first: they decide what is worth a gate.
        for (size_t k = n; k < v.size(); k++) v[n - 1].count += v[k].count;
        for (size_t k = 0; k < n; k++) jobs.push_back({i, k, v[k].body});
    }
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    auto t1 = now();
    Toucher t(d.xref);
    for (auto& j : jobs) j.body->visit(t);
    auto t2 = now();
    unsigned threads = opt.threads ? opt.threads : par::defaultThreads();
    par::parallelFor(jobs.size(), threads, [&](size_t ji) {
        Module& m = d.variants[jobs[ji].def][jobs[ji].var];
        m.body = jobs[ji].body;
        m.instances = defs[order[jobs[ji].def]][jobs[ji].var].count;
        m.name = std::string(m.body->getDefinition().name);
        analyze(sm, m, opt, d.xref);
    });
    auto t3 = now();
    d.msCollect = ms(t0, t1);
    d.msTouch = ms(t1, t2);
    d.msAnalyze = ms(t2, t3);
    for (size_t i = 0; i < order.size(); i++) {
        auto& vs = d.variants[i];
        bool same = defs[order[i]].size() <= 8;
        for (size_t k = 1; k < vs.size() && same; k++) same = vs[k].edits == vs[0].edits;
        // The widest parameterization decides what is worth a gate.
        size_t rep = 0;
        for (size_t k = 1; k < vs.size(); k++)
            if (vs[k].regBits > vs[rep].regBits) rep = k;
        if (!same) {
            // The parameterizations disagree. Enable gating and isolation are
            // true of the text alone, so they hold for every one of them;
            // sequential gating is only kept when all of them agree on it.
            bool hadSeq = false;
            for (auto& v : vs)
                for (auto& f : v.finds) hadSeq |= f.applied && f.kind != "CG" && f.kind != "OI" && f.note.size();
            for (auto& v : vs) plan(sm, v, opt, false);
            if (hadSeq) {
                Finding f;
                f.kind = "NOTE";
                f.module = vs[rep].name;
                f.note = "its parameterizations differ: sequential gating is not applied to this module";
                vs[rep].finds.push_back(std::move(f));
            }
        }
        // Design totals: every instance counts, at its own width.
        std::unordered_map<std::string, const Finding*> on;
        for (auto& f : vs[rep].finds)
            if (f.applied) on[f.key()] = &f;
        for (auto& v : vs) {
            d.tot.regBits += v.regBits * v.instances;
            d.tot.instances += v.instances;
            for (auto& f : v.finds) {
                // A wider or narrower copy of a gated group is gated too.
                auto it = on.find(f.key());
                if (it == on.end()) continue;
                uint64_t bits = f.bits * f.count * v.instances;
                size_t n = f.count * v.instances;
                const std::string& k = it->second->kind;
                if (k == "CG") { d.tot.cg += n; d.tot.cgBits += bits; }
                else if (k == "SCG") { d.tot.scg += n; d.tot.scgBits += bits; }
                else if (k == "MG") { d.tot.mg += n; d.tot.mgBits += bits; }
                else if (k == "OI") d.tot.oi += v.instances;
            }
        }
        d.tot.modules++;
        Module keep = std::move(vs[rep]);
        d.mods.push_back(std::move(keep));
    }
    d.variants.clear();
    d.msMerge = ms(t3, now());
    return d;
}

// ------------------------------------------------------------------ output

inline std::string cellText(const Options& opt) {
    std::string n = opt.prefix + "_icg";
    return "// " + n + ": the clock gate mpopt inserts. A latch that is open while the\n"
           "// clock is low, and an AND: gclk follows clk while en (or te, the scan test\n"
           "// enable) was 1 before the rising edge. Map it to your library's integrated\n"
           "// clock-gating cell, or run mpopt with -icg_cell to instantiate that cell.\n"
           "module " + n + " (input wire clk, input wire en, input wire te, output wire gclk);\n"
           "  reg en_l;\n"
           "  always @(*) if (!clk) en_l = en | te;\n"
           "  assign gclk = clk & en_l;\n"
           "endmodule\n";
}

// The rewritten text of every buffer that has edits: buffer id -> text.
inline std::map<uint32_t, std::string> rewrite(const SourceManager& sm, const Design& d, unsigned threads) {
    std::map<uint32_t, std::vector<Edit>> per;
    for (auto& m : d.mods)
        for (auto& e : m.edits) per[e.buf].push_back(e);
    std::vector<uint32_t> bufs;
    for (auto& [b, v] : per) bufs.push_back(b);
    std::vector<std::string> texts(bufs.size());
    par::parallelFor(bufs.size(), threads, [&](size_t i) {
        auto es = per[bufs[i]];
        // Two modules of one file: keep each module's order, drop exact repeats.
        std::stable_sort(es.begin(), es.end(), [](const Edit& a, const Edit& b) {
            return std::tie(a.begin, a.seq) < std::tie(b.begin, b.seq);
        });
        std::vector<Edit> u;
        for (auto& e : es)
            if (u.empty() || !(u.back() == e)) u.push_back(e);
        std::string_view text = sm.getSourceText(BufferID(bufs[i], ""sv));
        if (!text.empty() && text.back() == '\0') text.remove_suffix(1);
        std::string out;
        size_t at = 0;
        for (auto& e : u) {
            if (e.begin < at) continue;
            out.append(text.substr(at, e.begin - at));
            out += e.text;
            at = e.end;
        }
        out.append(text.substr(at));
        texts[i] = std::move(out);
    });
    std::map<uint32_t, std::string> r;
    for (size_t i = 0; i < bufs.size(); i++) r[bufs[i]] = std::move(texts[i]);
    return r;
}

inline std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

inline std::vector<const Finding*> sortedFindings(const Design& d) {
    std::vector<const Finding*> v;
    for (auto& m : d.mods)
        for (auto& f : m.finds) v.push_back(&f);
    std::stable_sort(v.begin(), v.end(), [](const Finding* a, const Finding* b) {
        return std::tie(a->file, a->line, a->kind, a->names) < std::tie(b->file, b->line, b->kind, b->names);
    });
    return v;
}

inline void printReport(std::ostream& os, const Design& d, bool verbose) {
    for (const Finding* f : sortedFindings(d)) {
        if (!f->applied && !verbose) continue;
        os << f->file << ":" << f->line << ": [" << f->kind << "] " << f->module;
        if (!f->names.empty()) os << ": " << f->names;
        if (f->bits) {
            os << " (" << f->bits << (f->bits == 1 ? " bit" : " bits");
            if (f->count > 1) os << " x " << f->count;
            os << ")";
        }
        if (f->applied) os << (f->kind == "OI" ? " isolated by " : " gated by ") << f->enable;
        else if (f->kind != "NOTE") os << " not gated";
        if (!f->note.empty()) os << (f->kind == "NOTE" ? ": " : "; ") << f->note;
        os << "\n";
    }
    const Totals& t = d.tot;
    uint64_t g = t.cgBits + t.scgBits + t.mgBits;
    os << "mpopt: " << t.modules << (t.modules == 1 ? " module, " : " modules, ") << t.instances
       << (t.instances == 1 ? " instance, " : " instances, ") << t.regBits << " register bits, " << g << " gated";
    if (t.regBits) os << " (" << (g * 100 / t.regBits) << "%)";
    os << "\n";
    os << "  clock gates from enables:    " << t.cg << " (" << t.cgBits << " bits)\n";
    os << "  sequential clock gates:      " << t.scg << " (" << t.scgBits << " bits)\n";
    os << "  memory gates:                " << t.mg << " (" << t.mgBits << " bits)\n";
    os << "  operators isolated:          " << t.oi << "\n";
}

inline void writeJson(std::ostream& os, const Design& d) {
    const Totals& t = d.tot;
    os << "{\n  \"modules\": " << t.modules << ",\n  \"instances\": " << t.instances << ",\n  \"register_bits\": " << t.regBits
       << ",\n  \"gated_bits\": " << (t.cgBits + t.scgBits + t.mgBits)
       << ",\n  \"clock_gates\": {\"count\": " << t.cg << ", \"bits\": " << t.cgBits << "}"
       << ",\n  \"sequential_clock_gates\": {\"count\": " << t.scg << ", \"bits\": " << t.scgBits << "}"
       << ",\n  \"memory_gates\": {\"count\": " << t.mg << ", \"bits\": " << t.mgBits << "}"
       << ",\n  \"isolated_operators\": " << t.oi << ",\n  \"findings\": [";
    bool first = true;
    for (const Finding* f : sortedFindings(d)) {
        os << (first ? "\n" : ",\n") << "    {\"kind\": " << jsonStr(f->kind) << ", \"file\": " << jsonStr(f->file)
           << ", \"line\": " << f->line << ", \"module\": " << jsonStr(f->module) << ", \"names\": "
           << jsonStr(f->names) << ", \"bits\": " << f->bits << ", \"count\": " << f->count << ", \"applied\": " << (f->applied ? "true" : "false")
           << ", \"enable\": " << jsonStr(f->enable) << ", \"note\": " << jsonStr(f->note) << "}";
        first = false;
    }
    os << "\n  ]\n}\n";
}

}  // namespace popt
