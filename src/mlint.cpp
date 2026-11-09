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

#include "version.hpp"

using namespace slang;
using namespace slang::ast;

namespace {

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
    Rules rules;
    std::vector<Finding> out;
    std::set<std::pair<std::string, std::string>> seen;  // rule + place, so one cause is reported once

    void add(std::string_view rule, SourceLocation loc, std::string text) {
        if (!rules.enabled(rule)) return;
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
std::string path(const Symbol& s) {
    std::string p = s.getHierarchicalPath();
    return p.empty() ? std::string(s.name) : p;
}

// -------------------------------------------------------------- union-find

// Signals connected through a port are the same wire. mlint unions them, so a
// clock passed down through three modules is still one domain.
struct Join {
    std::unordered_map<const Symbol*, const Symbol*> up;
    const Symbol* find(const Symbol* s) {
        auto it = up.find(s);
        if (it == up.end()) return up[s] = s;
        while (it->second != s) {
            s = it->second;
            it = up.find(s);
            if (it == up.end()) return up[s] = s;
        }
        return s;
    }
    void join(const Symbol* a, const Symbol* b) {
        a = find(a);
        b = find(b);
        if (a != b) up[b] = a;
    }
};

// ------------------------------------------------------- expression walking

// Every value the expression names, read or written.
void namesIn(const Expression& e, std::vector<const Symbol*>& out) {
    e.visit(makeVisitor([&](auto&, const NamedValueExpression& n) { out.push_back(&n.symbol); },
                        [&](auto&, const HierarchicalValueExpression& n) { out.push_back(&n.symbol); }));
}

// The single symbol an expression names, if it names exactly one (x, x[3], x.f).
const Symbol* oneName(const Expression& e) {
    std::vector<const Symbol*> n;
    namesIn(e, n);
    if (n.empty()) return nullptr;
    for (const Symbol* s : n)
        if (s != n[0]) return nullptr;
    return n[0];
}

// The assignment's target symbol, through bit and field selects.
const Symbol* lhsSymbol(const Expression& lhs) {
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
void clocksOf(const TimingControl& tc, std::vector<const SignalEventControl*>& out) {
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
                            a.left().kind == ExpressionKind::HierarchicalValue)
                            def.insert(t);
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

    Gather(Report& r, Join& j, std::vector<Block>& b) : rep(r), join(j), blocks(b) {}

    void handle(const InstanceSymbol& inst) {
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn || conn->port.kind != SymbolKind::Port) continue;
            const Expression* ext = conn->getExpression();
            if (!ext) continue;
            const Expression* outer = ext;
            if (outer->kind == ExpressionKind::Assignment)
                outer = &outer->as<AssignmentExpression>().left();
            if (const Symbol* actual = oneName(*outer)) join.join(&conn->port, actual);
        }
        visitDefault(inst);
    }

    void handle(const ContinuousAssignSymbol& ca) {
        const Expression& e = ca.getAssignment();
        if (e.kind != ExpressionKind::Assignment) return;
        const auto& a = e.as<AssignmentExpression>();
        if (const Symbol* t = lhsSymbol(a.left())) drivers[t].push_back(ca.location);
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
        for (const Symbol* t : targets) drivers[t].push_back(p.location);
        blocks.push_back(std::move(b));
    }
};

// ------------------------------------------------------------------- checks

void checkBlock(const Block& b, Report& rep) {
    const ProceduralBlockSymbol& p = *b.sym;
    bool comb = p.procedureKind == ProceduralBlockKind::AlwaysComb ||
                p.procedureKind == ProceduralBlockKind::AlwaysLatch;
    bool ff = p.procedureKind == ProceduralBlockKind::AlwaysFF;

    // L001: assigned somewhere but not on every path, in an always_comb.
    if (p.procedureKind == ProceduralBlockKind::AlwaysComb || (b.seq == false && !comb && !ff)) {
        bool isComb = p.procedureKind == ProceduralBlockKind::AlwaysComb;
        if (isComb) {
            for (const Symbol* t : b.blocking) {
                if (b.locals.count(t) || b.always.count(t)) continue;
                rep.add("L001", p.location,
                        "'" + path(*t) +
                            "' is not assigned on every path of this always_comb, so it holds its "
                            "old value (an unintended latch); add an else or a default");
            }
        }
    }

    // L002 / L003: the wrong kind of assignment for the block.
    if (ff)
        for (const Symbol* t : b.blocking) {
            if (b.locals.count(t)) continue;
            rep.add("L002", p.location,
                    "blocking assignment to '" + path(*t) +
                        "' in an always_ff; use <= so the result does not depend on evaluation "
                        "order (IEEE 1800-2023 10.4.2)");
        }
    if (p.procedureKind == ProceduralBlockKind::AlwaysComb)
        for (const Symbol* t : b.nba)
            rep.add("L003", p.location,
                    "non-blocking assignment to '" + path(*t) +
                        "' in an always_comb; use = (IEEE 1800-2023 10.4.1)");
}

void checkDrivers(const std::map<const Symbol*, std::vector<SourceLocation>>& drivers, Report& rep) {
    for (auto& [sym, locs] : drivers) {
        if (locs.size() < 2) continue;
        rep.add("L004", locs[1],
                "'" + path(*sym) + "' is assigned by " + std::to_string(locs.size()) +
                    " processes; in simulation the last write wins and in synthesis this is a "
                    "multiple-driver error");
    }
}

// Clock-domain crossings. For every sequential block, the signals it reads
// that a flop in another domain drives must come through a synchronizer: two
// plain flop stages in the reading domain before any logic uses the value.
void checkCdc(std::vector<Block>& blocks, Join& join, Report& rep) {
    auto dom = [&](const Symbol* clk) { return clk ? join.find(clk) : nullptr; };

    // Which domain drives each signal, and whether more than one does.
    std::unordered_map<const Symbol*, const Symbol*> driverDom;
    std::unordered_set<const Symbol*> multi;
    for (Block& b : blocks) {
        if (!b.seq) continue;
        const Symbol* d = dom(b.clk);
        if (!d) continue;
        for (const Symbol* t : b.nba) {
            const Symbol* c = join.find(t);
            auto it = driverDom.find(c);
            if (it == driverDom.end())
                driverDom[c] = d;
            else if (it->second != d)
                multi.insert(c);
        }
    }

    // A flop whose whole body is "dst <= src" is one stage of a synchronizer.
    auto isStage = [](const Block& b) {
        return b.seq && b.chainTo && b.chainFrom && b.nba.size() == 1 && b.blocking.empty() &&
               !b.rst;
    };
    // The stages that read a given signal, per domain, so a chain can be
    // followed forward: rq -> s1 -> s2.
    std::map<std::pair<const Symbol*, const Symbol*>, const Block*> nextStage;
    for (Block& b : blocks)
        if (isStage(b)) nextStage[{join.find(b.chainFrom), dom(b.clk)}] = &b;

    // How many plain stages follow this one, itself counted.
    std::function<int(const Block&, const Symbol*, int)> chainLen =
        [&](const Block& b, const Symbol* d, int guard) -> int {
        if (guard > 8) return guard;
        auto it = nextStage.find({join.find(b.chainTo), d});
        if (it == nextStage.end()) return 1;
        return 1 + chainLen(*it->second, d, guard + 1);
    };

    for (Block& b : blocks) {
        if (!b.seq) continue;
        const Symbol* d = dom(b.clk);
        if (!d) continue;
        const Symbol* rstDom = b.rst ? join.find(b.rst) : nullptr;
        for (const Symbol* r : b.reads) {
            const Symbol* c = join.find(r);
            if (c == join.find(b.clk) || (rstDom && c == rstDom)) continue;
            auto src = driverDom.find(c);
            if (src == driverDom.end() || src->second == d) continue;  // same domain, or not a flop
            if (multi.count(c)) continue;  // two domains drive it: L004 first

            int width = 1;
            if (auto* v = r->as_if<ValueSymbol>()) width = (int)v->getType().getBitWidth();
            // Stages in front of the crossing: this block, if it is a plain
            // stage, plus the stages that read it.
            int n = isStage(b) && join.find(b.chainFrom) == c ? chainLen(b, d, 0) : 0;
            std::string where = "'" + path(*r) + "' (clocked by " + path(*src->second) +
                                ") is read by logic clocked by " + path(*b.clk);
            if (n == 0)
                rep.add("CDC001", b.sym->location,
                        where + " with no synchronizer; two flops in the reading domain are the "
                                "minimum for a one-bit signal");
            else if (n == 1)
                rep.add("CDC001", b.sym->location,
                        where + " through one flop only; a single stage still settles too late, "
                                "so use two");
            else if (width > 1)
                rep.add("CDC002", b.sym->location,
                        where + " through " + std::to_string(n) + " flops, but it is " +
                            std::to_string(width) +
                            " bits wide: the bits settle in different cycles, so use a handshake, "
                            "a FIFO, or Gray coding");
        }
    }
}

void usage() {
    std::cout << "usage: mlint [options] file.sv ...\n"
                 "  lint and clock-domain checks on the msim front end\n\n"
                 "  -disable RULE  turn one rule off (repeatable)\n"
                 "  -list          list the rules\n"
                 "  -Werror        exit non-zero on any finding (the default is errors only)\n"
                 "  -quiet         print findings only, no summary\n"
                 "  -top M         the top module    -f <file>  an argument file\n"
                 "  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
                 "  -version       print the version\n"
                 "  --help-slang   list every front-end option\n";
}

void listRules() {
    std::cout << "L001    a value is not assigned on every path of an always_comb (a latch)\n"
                 "L002    a blocking assignment in an always_ff\n"
                 "L003    a non-blocking assignment in an always_comb\n"
                 "L004    one value is assigned by more than one process\n"
                 "CDC001  a clock-domain crossing with no synchronizer, or only one flop\n"
                 "CDC002  a multi-bit bus crossing through a two-flop synchronizer\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> fwd{argv[0]};
    Rules rules;
    bool werror = false, quiet = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mlint: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-list" || a == "--list") { listRules(); return 0; }
        if (a == "-version" || a == "--version" || a == "-v") {
            std::cout << "mlint " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-disable") { rules.disabled.insert(next("a rule name")); continue; }
        if (a == "-Werror") { werror = true; continue; }
        if (a == "-quiet" || a == "-q") { quiet = true; continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        fwd.push_back(a);
    }

    driver::Driver driver;
    driver.addStandardArgs();
    std::vector<const char*> cargv;
    for (auto& s : fwd) cargv.push_back(s.c_str());
    if (!driver.parseCommandLine((int)cargv.size(), cargv.data())) return 2;
    for (auto& s : fwd)
        if (s == "--help") {
            std::cout << driver.cmdLine.getHelpText("mlint front end") << "\n";
            return 0;
        }
    if (!driver.processOptions()) return 2;
    if (!driver.parseAllSources()) return 1;

    auto comp = driver.createCompilation();
    // Pass 1: the front end's own warnings and errors.
    driver.reportCompilation(*comp, /*quiet*/ true);
    if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;

    // Pass 2: our rules.
    Report rep;
    rep.sm = comp->getSourceManager();
    rep.rules = rules;
    Join join;
    std::vector<Block> blocks;
    Gather g(rep, join, blocks);
    comp->getRoot().visit(g);

    for (const Block& b : blocks) checkBlock(b, rep);
    checkDrivers(g.drivers, rep);
    checkCdc(blocks, join, rep);

    std::sort(rep.out.begin(), rep.out.end());
    for (const Finding& f : rep.out)
        std::cout << f.file << ":" << f.line << ":" << f.col << ": warning: [" << f.rule << "] "
                  << f.text << "\n";
    if (!quiet) {
        std::map<std::string, int> per;
        for (const Finding& f : rep.out) per[f.rule]++;
        std::cout << "mlint: " << rep.out.size() << (rep.out.size() == 1 ? " finding" : " findings");
        bool first = true;
        for (auto& [r, n] : per) {
            std::cout << (first ? " (" : ", ") << r << " " << n;
            first = false;
        }
        std::cout << (first ? "" : ")") << "\n";
    }
    return werror && !rep.out.empty() ? 1 : 0;
}
