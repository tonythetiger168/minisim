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
//   CDC001..CDC005, RDC001..RDC004  clock- and reset-domain crossings (cdc.hpp).
//   UPF001..UPF006  with -upf: power intent checks (upf.hpp check()).
#include <unistd.h>

#include "cdc.hpp"
#include "lintir.hpp"
#include "upf.hpp"
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

void checkDrivers(const std::map<const Symbol*, std::vector<SourceLocation>>& drivers,
                  const std::set<const Symbol*>& wholeDriven, Report& rep) {
    for (auto& [sym, locs] : drivers) {
        if (locs.size() < 2) continue;
        if (!wholeDriven.count(sym)) continue;   // only part selects: each may own its bits
        rep.add("L004", locs[1],
                "'" + path(*sym) + "' is assigned by " + std::to_string(locs.size()) +
                    " processes; in simulation the last write wins and in synthesis this is a "
                    "multiple-driver error");
    }
}

// ------------------------------------------------------------- UPF (1801)
// The design as UPF sees it: instance paths and signal names as msim names
// them (relative to the top, '.' between levels, generate blocks as
// "gen[1]"), and every port connection of a child instance.
void upfWalk(const Scope& sc, const std::string& path, upf::DesignView& dv, const SourceManager& sm) {
    auto in = [&](const std::string& n) { return path.empty() ? n : path + "." + n; };
    auto loc = [&](SourceLocation l) {
        if (!l) return std::string("?");
        auto f = sm.getFullyOriginalLoc(l);
        return std::string(sm.getFileName(f)) + ":" + std::to_string(sm.getLineNumber(f));
    };
    for (const Symbol& m : sc.members()) {
        if (m.kind == SymbolKind::Variable || m.kind == SymbolKind::Net) dv.signals.insert(in(std::string(m.name)));
        if (m.kind == SymbolKind::Instance) {
            const auto& ch = m.as<InstanceSymbol>();
            std::string p = in(std::string(ch.name));
            dv.instances.insert(p);
            for (const PortConnection* c : ch.getPortConnections()) {
                if (!c || c->port.kind != SymbolKind::Port) continue;
                const auto& ps = c->port.as<PortSymbol>();
                upf::PortSite site;
                site.inst = p;
                site.port = std::string(ps.name);
                site.dir = ps.direction == ArgumentDirection::In ? 0 : ps.direction == ArgumentDirection::Out ? 1 : 2;
                site.width = (int)ps.getType().getBitWidth();
                const Expression* x = c->getExpression();
                site.where = loc(x && x->sourceRange.start() ? x->sourceRange.start() : ch.location);
                if (x) dv.ports.push_back(site);
            }
            upfWalk(ch.body, p, dv, sm);
        } else if (m.kind == SymbolKind::GenerateBlock) {
            const auto& g = m.as<GenerateBlockSymbol>();
            if (g.isUninstantiated) continue;
            std::string p = in(g.getExternalName());
            dv.instances.insert(p);
            upfWalk(g, p, dv, sm);
        } else if (m.kind == SymbolKind::GenerateBlockArray) {
            const auto& ga = m.as<GenerateBlockArraySymbol>();
            std::string an = ga.name.empty() ? std::string("genblk") : std::string(ga.name);
            for (const auto* e : ga.entries) {
                if (e->isUninstantiated) continue;
                std::string p = in(an + "[" + (e->getArrayIndex() ? e->getArrayIndex()->toString() : std::string("?")) + "]");
                dv.instances.insert(p);
                upfWalk(*e, p, dv, sm);
            }
        }
    }
}

void checkUpf(Compilation& comp, const std::string& upfPath, std::string powerTop, bool powerTopSet, Report& rep) {
    upf::Model m;
    m.load(upfPath, "");
    upf::DesignView dv;
    auto tops = comp.getRoot().topInstances;
    for (auto* t : tops) upfWalk(t->body, "", dv, *comp.getSourceManager());
    std::string top = powerTopSet ? powerTop : m.designTop;
    for (char& c : top) if (c == '/') c = '.';
    while (!top.empty() && top[0] == '.') top.erase(0, 1);
    if (tops.size() == 1 && (top == tops[0]->name || top.starts_with(std::string(tops[0]->name) + ".")))
        top = top.substr(std::min(top.size(), tops[0]->name.size() + 1));
    if (!powerTopSet && !top.empty() && top.find('.') == std::string::npos) {   // a module name
        std::string found;
        std::function<void(const InstanceSymbol&, const std::string&)> walk = [&](const InstanceSymbol& in, const std::string& path) {
            if (!found.empty()) return;
            if (in.getDefinition().name == top && !path.empty()) { found = path; return; }
            for (auto& mm : in.body.members())
                if (mm.kind == SymbolKind::Instance) {
                    auto& ch = mm.as<InstanceSymbol>();
                    walk(ch, path.empty() ? std::string(ch.name) : path + "." + std::string(ch.name));
                }
        };
        for (auto* t : tops) {
            if (t->getDefinition().name == top) { found = "."; break; }
            walk(*t, "");
        }
        top = found == "." ? "" : found.empty() ? top : found;
    }
    m.top = top;
    for (auto& w : m.warnings) std::cerr << "mlint: UPF: " << w << "\n";
    for (auto& f : upf::check(m, dv)) {
        if (!rep.rules.enabled(f.rule)) continue;
        Finding o;
        o.rule = f.rule;
        o.text = f.text;
        o.col = 1;
        size_t c = f.where.rfind(':');
        o.file = c == std::string::npos ? f.where : f.where.substr(0, c);
        if (c != std::string::npos) o.line = (size_t)std::atoll(f.where.c_str() + c + 1);
        if (!rep.seen.insert({o.rule, f.where + ":" + o.text}).second) continue;
        rep.out.push_back(o);
    }
}

void usage() {
    std::cout << "usage: mlint [options] file.sv ...\n"
                 "  lint and clock-domain checks on the msim front end\n\n"
                 "  -disable RULE  turn one rule off (repeatable)\n"
                 "  -list          list the rules\n"
                 "  -Werror        exit non-zero on any finding (the default is errors only)\n"
                 "  -quiet         print findings only, no summary\n"
              << cdc::usage()
              << "  -top M         the top module    -f <file>  an argument file\n"
              << protfront::Options::help() <<
                 "  -upf <file>    check IEEE 1801 (UPF) power intent against the design (UPF rules)\n"
                 "  -power_top <i> the instance the UPF describes (default: set_design_top, else the top)\n"
                 "  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
                 "  -version       print the version\n"
                 "  --help-slang   list every front-end option\n";
}

void listRules() {
    std::cout << "L001    a value is not assigned on every path of an always_comb (a latch)\n"
                 "L002    a blocking assignment in an always_ff\n"
                 "L003    a non-blocking assignment in an always_comb\n"
                 "L004    one value is assigned by more than one process\n"
              << cdc::rules();
    for (auto& [r, t] : upf::rules()) std::cout << r << "  " << t << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    FrontEnd fe;
    std::vector<std::string> fwd{argv[0]};
    Rules rules;
    bool werror = false, quiet = false, powerTopSet = false;
    std::string upfPath, powerTop;
    cdc::Options cdcOpt;
    cdcOpt.defaults(rules.disabled);
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
        if (cdcOpt.take(a, next, rules.disabled, fwd)) continue;
        if (a == "-upf") { upfPath = next("a UPF file"); continue; }
        if (a == "-power_top") { powerTop = next("an instance path"); powerTopSet = true; continue; }
        if (a == "-quiet" || a == "-q") { quiet = true; continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        if (fe.prot.take(argc, argv, i)) continue;
        fwd.push_back(a);
    }

    // With -upf the testbench may import package UPF (IEEE 1801-2024 11.2.4).
    std::string pkgFile;
    if (!upfPath.empty()) {
        char tmpl[] = "/tmp/mlint_upf_pkgXXXXXX.sv";
        int fd = mkstemps(tmpl, 3);
        std::string pkg = upf::svPackage();
        if (fd < 0 || write(fd, pkg.data(), pkg.size()) != (ssize_t)pkg.size()) {
            std::cerr << "mlint: cannot write the UPF package\n";
            return 2;
        }
        close(fd);
        pkgFile = tmpl;
        fwd.insert(fwd.begin() + 1, pkgFile);
    }
    struct RmPkg { std::string f; ~RmPkg() { if (!f.empty()) std::remove(f.c_str()); } } rmPkg{pkgFile};

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
    checkDrivers(d.drivers, d.wholeDriven, rep);
    cdc::check(*fe.comp, rep, cdcOpt);
    if (!upfPath.empty()) {
        try {
            checkUpf(*fe.comp, upfPath, powerTop, powerTopSet, rep);
        } catch (const std::exception& ex) {
            std::cerr << "mlint: UPF: " << ex.what() << "\n";
            return 1;
        }
    }

    rep.print(std::cout, quiet, "mlint");
    return werror && !rep.out.empty() ? 1 : 0;
}
