// mlint.cpp - a lint and clock-domain checker for SystemVerilog, on the same
// slang front end as msim.
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
#include "lintir.hpp"
#include "version.hpp"

using namespace lintir;

namespace {

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
              << protfront::Options::help() <<
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
    FrontEnd fe;
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
        if (fe.prot.take(argc, argv, i)) continue;
        fwd.push_back(a);
    }

    // Pass 1: the front end, with its own warnings and errors.
    fe.tool = "mlint";
    if (int rc = fe.run(fwd, "mlint front end")) return rc < 0 ? 0 : rc;

    // Pass 2: our rules.
    Report rep;
    rep.sm = fe.comp->getSourceManager();
    if (fe.pc.any()) { rep.pc = &fe.pc; namePC() = &fe.pc; }
    rep.rules = rules;
    Design d;
    d.gather(*fe.comp, rep);

    for (const Block& b : d.blocks) checkBlock(b, rep);
    checkDrivers(d.drivers, rep);
    checkCdc(d.blocks, d.join, rep);

    rep.print(std::cout, quiet, "mlint");
    return werror && !rep.out.empty() ? 1 : 0;
}
