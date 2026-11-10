#pragma once
// mlint.cpp - a lint and clock-domain-crossing checker for SystemVerilog,
// on the same slang front end as msim.
//
// Two layers:
//   * the front end's own warnings (slang), printed as they are;
//   * the rules below, which need the elaborated design.
//
// Rules
//   L001  a variable is assigned on some paths of an always_comb but not all,
//         so it holds its old value: an unintended latch.
//   L002  a blocking assignment to a non-local variable in an always_ff.
//   L003  a non-blocking assignment in an always_comb.
//   L004  one variable or net is assigned by more than one process.
//   CDC001  a flop reads a signal from another clock domain with no
//           synchronizer in between.
//   CDC002  a multi-bit bus crosses clock domains through a two-flop
//           synchronizer, which does not keep its bits together.
//
// Clock domains: the clock of every edge-triggered block, canonicalized by a
// union-find over port connections so that one clock is one domain however
// many module boundaries it is passed through.
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <ostream>
#include <vector>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/Type.h"
#include "slang/driver/Driver.h"
#include "slang/text/SourceManager.h"
#include "protect_front.hpp"

using namespace slang;
using namespace slang::ast;

namespace lintir {


// ---------------------------------------------------------------- findings

struct Finding {
    std::string file;
    size_t line = 0, col = 0;
    std::string rule, text;
    bool operator<(const Finding& o) const {
        return std::tie(file, line, col, rule) < std::tie(o.file, o.line, o.col, o.rule);
    }
};

struct Rules {
    std::set<std::string> disabled;
    bool enabled(std::string_view r) const { return !disabled.count(std::string(r)); }
};

struct Report {
    const SourceManager* sm = nullptr;
    const protfront::ProtectedCode* pc = nullptr;   // findings in hidden protected code are only counted
    int hiddenCount = 0;
    Rules rules;
    std::vector<Finding> out;
    std::set<std::pair<std::string, std::string>> seen;  // rule + place, so one cause is reported once
    std::set<std::pair<std::string, SourceLocation>> hiddenSeen;

    // One finding per line: "file:line:col: warning: [RULE] text", sorted.
    void print(std::ostream& os, bool quiet, const char* tool) {
        std::sort(out.begin(), out.end());
        for (const Finding& f : out)
            os << f.file << ":" << f.line << ":" << f.col << ": warning: [" << f.rule << "] "
               << f.text << "\n";
        if (hiddenCount)
            os << tool << ": " << hiddenCount << (hiddenCount == 1 ? " finding" : " findings")
               << " in protected code (details hidden)\n";
        if (quiet) return;
        std::map<std::string, int> per;
        for (const Finding& f : out) per[f.rule]++;
        os << tool << ": " << out.size() << (out.size() == 1 ? " finding" : " findings");
        bool first = true;
        for (auto& [r, n] : per) {
            os << (first ? " (" : ", ") << r << " " << n;
            first = false;
        }
        os << (first ? "" : ")") << "\n";
    }

    void add(std::string_view rule, SourceLocation loc, std::string text) {
        if (!rules.enabled(rule)) return;
        if (pc && pc->hidden(loc)) {
            if (hiddenSeen.insert({std::string(rule), loc}).second) hiddenCount++;
            return;
        }
        Finding f;
        f.rule = std::string(rule);
        f.text = std::move(text);
        if (sm && loc) {
            auto full = sm->getFullyOriginalLoc(loc);
            f.file = std::string(sm->getFileName(full));
            f.line = sm->getLineNumber(full);
            f.col = sm->getColumnNumber(full);
        }
        if (!seen.insert({f.rule, f.file + ":" + std::to_string(f.line) + ":" + std::to_string(f.col) +
                                      ":" + f.text})
                 .second)
            return;
        out.push_back(std::move(f));
    }
};

// Where a symbol is in the design, for a message: a.b.clk
// Protected code the tool read, if any: names of objects it hides print as
// "<protected>" (IEEE 1735 clause 9).
inline const protfront::ProtectedCode*& namePC() {
    static const protfront::ProtectedCode* pc = nullptr;
    return pc;
}
inline std::string path(const Symbol& s) {
    if (namePC() && !namePC()->visible(s)) return "<protected>";
    std::string p = s.getHierarchicalPath();
    return p.empty() ? std::string(s.name) : p;
}

// -------------------------------------------------------------- union-find

// Signals connected through a port are the same wire. mlint unions them, so a
// clock passed down through three modules is still one domain.
struct Join {
    std::unordered_map<const Symbol*, const Symbol*> up;
    // With path compression: a clock that reaches thousands of instances is
    // one long chain otherwise, and every lookup walked all of it.
    const Symbol* find(const Symbol* s) {
        const Symbol* root = s;
        for (;;) {
            auto it = up.find(root);
            if (it == up.end()) {
                up[root] = root;
                break;
            }
            if (it->second == root) break;
            root = it->second;
        }
        while (s != root) {
            auto it = up.find(s);
            const Symbol* next = it->second;
            it->second = root;
            s = next;
        }
        return root;
    }
    void join(const Symbol* a, const Symbol* b) {
        a = find(a);
        b = find(b);
        if (a != b) up[b] = a;
    }
};

// ------------------------------------------------------- expression walking

// Every value the expression names, read or written.
inline void namesIn(const Expression& e, std::vector<const Symbol*>& out) {
    e.visit(makeVisitor([&](auto&, const NamedValueExpression& n) { out.push_back(&n.symbol); },
                        [&](auto&, const HierarchicalValueExpression& n) { out.push_back(&n.symbol); }));
}

// The single symbol an expression names, if it names exactly one (x, x[3], x.f).
inline const Symbol* oneName(const Expression& e) {
    std::vector<const Symbol*> n;
    namesIn(e, n);
    if (n.empty()) return nullptr;
    for (const Symbol* s : n)
        if (s != n[0]) return nullptr;
    return n[0];
}

// The assignment's target symbol, through bit and field selects.
inline const Symbol* lhsSymbol(const Expression& lhs) {
    std::vector<const Symbol*> n;
    namesIn(lhs, n);
    return n.size() == 1 ? n[0] : (n.empty() ? nullptr : n[0]);
}

// ------------------------------------------------------------ clock domains

struct Clock {
    const Symbol* sig = nullptr;  // canonical clock signal
    bool valid() const { return sig != nullptr; }
};

// The edge-triggered clocks of a timing control. An always_ff has one; a reset
// is an edge too, so the first edge is taken as the clock, as tools do.
inline void clocksOf(const TimingControl& tc, std::vector<const SignalEventControl*>& out) {
    switch (tc.kind) {
        case TimingControlKind::SignalEvent: out.push_back(&tc.as<SignalEventControl>()); break;
        case TimingControlKind::EventList:
            for (const TimingControl* e : tc.as<EventListControl>().events) clocksOf(*e, out);
            break;
        default: break;
    }
}

// --------------------------------------------------------------- the blocks

// One procedural block, with what it assigns and reads.
struct Block {
    const ProceduralBlockSymbol* sym = nullptr;
    bool seq = false;                              // edge triggered
    const Symbol* clk = nullptr;                   // canonical clock, when seq
    const Symbol* rst = nullptr;                   // the other edge, if any
    std::set<const Symbol*> nba, blocking, reads;  // targets and reads
    std::set<const Symbol*> always;                // assigned on every path
    std::set<const Symbol*> locals;                // declared inside the block
    std::set<const Symbol*> whole;                 // assigned as a whole word somewhere (not x[i], x[7:0])
    // src <- the one assignment "dst <= src" of a plain one-statement flop,
    // which is what a synchronizer stage looks like.
    const Symbol* chainFrom = nullptr;
    const Symbol* chainTo = nullptr;
};

// Walks one block's body: targets, reads, and the definitely-assigned set.
struct BodyWalk {
    Block& b;
    Report& rep;

    explicit BodyWalk(Block& blk, Report& r) : b(blk), rep(r) {}

    void expr(const Expression& e, bool lhs = false) {
        std::vector<const Symbol*> n;
        namesIn(e, n);
        if (!lhs)
            for (const Symbol* s : n) b.reads.insert(s);
    }

    // Returns the symbols this statement assigns on every path through it.
    std::set<const Symbol*> walk(const Statement& s) {
        std::set<const Symbol*> def;
        switch (s.kind) {
            case StatementKind::List: {
                for (const Statement* c : s.as<StatementList>().list) {
                    auto d = walk(*c);
                    def.insert(d.begin(), d.end());
                }
                break;
            }
            case StatementKind::Block: {
                const auto& blk = s.as<BlockStatement>();
                if (blk.blockSymbol)
                    for (auto& v : blk.blockSymbol->membersOfType<VariableSymbol>()) b.locals.insert(&v);
                def = walk(blk.body);
                break;
            }
            case StatementKind::ExpressionStatement: {
                const Expression& e = s.as<ExpressionStatement>().expr;
                if (e.kind == ExpressionKind::Assignment) {
                    const auto& a = e.as<AssignmentExpression>();
                    expr(a.right());
                    expr(a.left(), /*lhs*/ true);
                    // A part select reads the rest of the word, and a select
                    // index is itself a read.
                    std::vector<const Symbol*> ln;
                    namesIn(a.left(), ln);
                    const Symbol* t = ln.empty() ? nullptr : ln[0];
                    if (t) {
                        (a.isNonBlocking() ? b.nba : b.blocking).insert(t);
                        // Only a whole-word assignment covers the target.
                        if (a.left().kind == ExpressionKind::NamedValue ||
                            a.left().kind == ExpressionKind::HierarchicalValue) {
                            def.insert(t);
                            b.whole.insert(t);
                        }
                        if (!b.chainTo) {
                            b.chainTo = t;
                            b.chainFrom = oneName(a.right());
                        } else {
                            b.chainTo = b.chainFrom = nullptr;  // more than one assignment
                        }
                    }
                } else {
                    expr(e);
                }
                break;
            }
            case StatementKind::Conditional: {
                const auto& c = s.as<ConditionalStatement>();
                for (auto& cond : c.conditions) expr(*cond.expr);
                auto t = walk(c.ifTrue);
                if (!c.ifFalse) break;  // no else: nothing is assigned on every path
                auto f = walk(*c.ifFalse);
                for (const Symbol* x : t)
                    if (f.count(x)) def.insert(x);
                break;
            }
            case StatementKind::Case: {
                const auto& c = s.as<CaseStatement>();
                expr(c.expr);
                bool first = true;
                std::set<const Symbol*> common;
                for (auto& it : c.items) {
                    for (const Expression* e : it.expressions) expr(*e);
                    auto d = walk(*it.stmt);
                    if (first) {
                        common = d;
                        first = false;
                    } else {
                        std::set<const Symbol*> both;
                        for (const Symbol* x : common)
                            if (d.count(x)) both.insert(x);
                        common = both;
                    }
                }
                if (!c.defaultCase) break;  // no default: a value may be held
                auto d = walk(*c.defaultCase);
                if (first) {
                    def = d;
                } else {
                    for (const Symbol* x : common)
                        if (d.count(x)) def.insert(x);
                }
                break;
            }
            case StatementKind::Timed: {
                const auto& t = s.as<TimedStatement>();
                walkTiming(t.timing);
                def = walk(t.stmt);
                break;
            }
            case StatementKind::VariableDeclaration:
                b.locals.insert(&s.as<VariableDeclStatement>().symbol);
                break;
            default: {
                // Loops, forks, waits: visit for reads and targets, but nothing
                // inside is counted as assigned on every path (a loop may run
                // zero times).
                s.visit(makeVisitor(
                    [&](auto& v, std::derived_from<Statement> auto& inner) { v.visitDefault(inner); },
                    [&](auto&, const Expression& e) {
                        if (e.kind == ExpressionKind::Assignment) {
                            const auto& a = e.as<AssignmentExpression>();
                            expr(a.right());
                            std::vector<const Symbol*> ln;
                            namesIn(a.left(), ln);
                            if (!ln.empty()) (a.isNonBlocking() ? b.nba : b.blocking).insert(ln[0]);
                            if (!ln.empty() && (a.left().kind == ExpressionKind::NamedValue ||
                                                a.left().kind == ExpressionKind::HierarchicalValue))
                                b.whole.insert(ln[0]);
                            b.chainTo = b.chainFrom = nullptr;
                        } else {
                            expr(e);
                        }
                    }));
                break;
            }
        }
        return def;
    }

    void walkTiming(const TimingControl& tc) {
        std::vector<const SignalEventControl*> evs;
        clocksOf(tc, evs);
        for (const SignalEventControl* e : evs)
            if (e->iffCondition) expr(*e->iffCondition);
    }
};

// ------------------------------------------------------------------ gather

struct Gather : ASTVisitor<Gather> {
    Report& rep;
    Join& join;
    std::vector<Block>& blocks;
    // Every assignment of a symbol outside a process, for L004.
    std::map<const Symbol*, std::vector<SourceLocation>> drivers;
    // ...and the symbols some driver writes as a whole word. Drivers of
    // separate bits (assign x[i] = ... in a generate loop) are not a conflict.
    std::set<const Symbol*> wholeDriven;

    Gather(Report& r, Join& j, std::vector<Block>& b) : rep(r), join(j), blocks(b) {}

    void handle(const InstanceSymbol& inst) {
        // A module nobody instantiates is elaborated only so that it can be
        // checked; it is not part of the design.
        if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return;
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn || conn->port.kind != SymbolKind::Port) continue;
            const Expression* ext = conn->getExpression();
            if (!ext) continue;
            const Expression* outer = ext;
            if (outer->kind == ExpressionKind::Assignment)
                outer = &outer->as<AssignmentExpression>().left();
            if (const Symbol* actual = oneName(*outer)) {
                join.join(&conn->port, actual);
                // Inside the instance, processes name the port's internal
                // net or variable, not the port: join that too, or every
                // module's clk_i was a clock domain of its own.
                if (const Symbol* in = conn->port.as<PortSymbol>().internalSymbol) join.join(actual, in);
            }
        }
        visitDefault(inst);
    }

    // The branch of a generate if / case that elaboration did not pick (and a
    // loop body for no iteration) is not part of the design: walking it made
    // false findings, such as a port "assigned by 2 processes" when the two
    // assigns sit in opposite branches.
    void handle(const GenerateBlockSymbol& g) {
        if (g.isUninstantiated) return;
        visitDefault(g);
    }

    void handle(const ContinuousAssignSymbol& ca) {
        const Expression& e = ca.getAssignment();
        if (e.kind != ExpressionKind::Assignment) return;
        const auto& a = e.as<AssignmentExpression>();
        if (const Symbol* t = lhsSymbol(a.left())) {
            drivers[t].push_back(ca.location);
            if (a.left().kind == ExpressionKind::NamedValue || a.left().kind == ExpressionKind::HierarchicalValue)
                wholeDriven.insert(t);
        }
    }

    void handle(const ProceduralBlockSymbol& p) {
        Block b;
        b.sym = &p;
        const Statement& body = p.getBody();
        BodyWalk w(b, rep);
        // always_ff / always @(posedge clk): the timing control is the body's
        // first statement.
        const Statement* inner = &body;
        if (body.kind == StatementKind::Timed) {
            const auto& t = body.as<TimedStatement>();
            std::vector<const SignalEventControl*> evs;
            clocksOf(t.timing, evs);
            std::vector<const SignalEventControl*> edges;
            for (const SignalEventControl* e : evs)
                if (e->edge != EdgeKind::None) edges.push_back(e);
            if (!edges.empty()) {
                b.seq = true;
                b.clk = oneName(edges[0]->expr);
                if (edges.size() > 1) b.rst = oneName(edges[1]->expr);
            }
            inner = &t.stmt;
            w.walkTiming(t.timing);
        }
        b.always = w.walk(*inner);
        // One process is one driver, even when it assigns the target twice.
        std::set<const Symbol*> targets = b.nba;
        for (const Symbol* t : b.blocking)
            if (!b.locals.count(t)) targets.insert(t);
        for (const Symbol* t : targets) {
            drivers[t].push_back(p.location);
            if (b.whole.count(t)) wholeDriven.insert(t);
        }
        blocks.push_back(std::move(b));
    }
};


// The front end the checkers share: msim's option spellings (-top, -sv,
// +define+, -f) on slang's driver, then parse and elaborate. Returns null when
// the design does not elaborate (the diagnostics are already printed).
struct FrontEnd {
    driver::Driver driver;
    std::unique_ptr<Compilation> comp;
    protfront::Options prot;        // -keyfile / -keyring / -license, filled by the tool
    protfront::ProtectedCode pc;
    const char* tool = "mlint";

    // `own` holds the options the tool itself took out of the command line.
    int run(const std::vector<std::string>& fwd, const char* helpTitle) {
        driver.addStandardArgs();
        std::vector<const char*> cargv;
        for (auto& s : fwd) cargv.push_back(s.c_str());
        if (!driver.parseCommandLine((int)cargv.size(), cargv.data())) return 2;
        for (auto& s : fwd)
            if (s == "--help") {
                std::cout << driver.cmdLine.getHelpText(helpTitle) << "\n";
                return -1;  // handled, nothing to check
            }
        if (!driver.processOptions()) return 2;
        if (!protfront::preload(driver, prot, pc, tool)) return 1;
        protfront::redact(driver, pc);
        if (!driver.parseAllSources()) return 1;
        comp = driver.createCompilation();
        driver.reportCompilation(*comp, /*quiet*/ true);
        if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;
        return 0;
    }
};

// Elaborate, then walk: the blocks, the drivers, and the port union-find.
struct Design {
    Join join;
    std::vector<Block> blocks;
    std::map<const Symbol*, std::vector<SourceLocation>> drivers;
    std::set<const Symbol*> wholeDriven;

    void gather(Compilation& comp, Report& rep) {
        Gather g(rep, join, blocks);
        comp.getRoot().visit(g);
        drivers = std::move(g.drivers);
        wholeDriven = std::move(g.wholeDriven);
    }
};

}  // namespace lintir
