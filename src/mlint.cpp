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
//   ...   and the rules of lintrules.hpp: synthesizability, latches, case,
//         widths, unused signals, connections, clocks, resets, naming, style.
//         mlint -list shows them all; lintcfg.hpp is the table.
//   L002  a blocking assignment to a non-local variable in an always_ff.
//   L003  a non-blocking assignment in an always_comb.
//   L004  one variable or net is assigned by more than one process.
//   CDC001..CDC005, RDC001..RDC004  clock- and reset-domain crossings (cdc.hpp).
//   UPF001..UPF006  with -upf: power intent checks (upf.hpp check()).
#include <unistd.h>

#include <chrono>

#include "slang/analysis/AnalysisManager.h"
#include "slang/diagnostics/AnalysisDiags.h"
#include "slang/util/ThreadPool.h"
#include "cdc.hpp"
#include "cpu.hpp"
#include "lintir.hpp"
#include "lintrules.hpp"
#include "upf.hpp"
#include "version.hpp"

using namespace lintir;

namespace {

// ------------------------------------------------------------------- checks

void checkBlock(const Block& b, Report& rep) {
    const ProceduralBlockSymbol& p = *b.sym;
    bool ff = p.procedureKind == ProceduralBlockKind::AlwaysFF;

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
                 "  -list            list the rules: name, severity, on or off, group, summary\n"
                 "  -explain RULE    what a rule means and what to do about it\n"
                 "  -enable X        turn on a rule, a group (naming, width, synth, ...) or all (repeatable)\n"
                 "  -disable X       turn off a rule or a group (repeatable)\n"
                 "  -only X[,Y]      run these rules or groups and nothing else\n"
                 "  -severity R=L    report rule or group R as error, warning or info\n"
                 "  -set NAME=VALUE  a setting, such as name.module='^[a-z][a-z0-9_]*$'\n"
                 "  -config FILE     directives, one per line: enable, disable, only, severity, set, waive\n"
                 "  -waivers FILE    the same format; by habit the file with the waive lines\n"
                 "  -show-waived     also print the waived findings, with the reason\n"
                 "  -json FILE       write the findings (waived ones too) as JSON\n"
                 "  -Werror          exit 1 on any warning (errors always do; info findings never do)\n"
                 "  -quiet           print findings only, no summary\n"
                 "  -stats           print where the time went, on stderr\n"
                 "  -threads N       worker threads, or all (default: every logical CPU); the output\n"
                 "                   does not depend on N\n"
              << cdc::usage() <<
                 "  -top M           the top module    -f <file>  an argument file\n"
                 "  -upf <file>      check IEEE 1801 (UPF) power intent against the design (UPF rules)\n"
                 "  -power_top <i>   the instance the UPF describes (default: set_design_top, else the top)\n"
                 "  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
              << protfront::Options::help() <<
                 "  -version         print the version\n"
                 "  --help-slang     list every front-end option\n\n"
                 "  In the source: // mlint: waive RULE reason   (this line, or the next if alone on its line)\n"
                 "                 // mlint: off RULE ... // mlint: on RULE\n";
}

void listRules(const lintcfg::Config& cfg) {
    auto row = [&](const std::string& id, const std::string& brief) {
        std::string g = lintcfg::groupOf(id);
        std::printf("%-8s %-8s %-4s %-8s %s\n", id.c_str(), lintcfg::sevName(cfg.severity(id)),
                    cfg.enabled(id) ? "on" : "off", g.c_str(), brief.c_str());
    };
    for (auto& r : lintcfg::table()) row(r.id, r.brief);
    for (auto& [r, t] : upf::rules()) row(r, t);
}

int explain(const std::string& id) {
    const lintcfg::RuleInfo* r = lintcfg::findRule(id);
    if (!r) {
        for (auto& [u, t] : upf::rules())
            if (u == id) {
                std::cout << id << ": " << t << "\n";
                return 0;
            }
        std::cerr << "mlint: no rule '" << id << "' (mlint -list)\n";
        return 2;
    }
    std::cout << r->id << " (" << r->group << ", " << lintcfg::sevName(r->sev) << ", " << (r->on ? "on" : "off")
              << " by default)\n  " << r->brief << "\n";
    if (*r->detail) std::cout << "  " << r->detail << "\n";
    return 0;
}

// A finding after the configuration and the waivers have had their say.
struct Final {
    Finding f;
    lintcfg::Sev sev;
    bool waived = false;
    std::string reason;
};

// Enabled rules only; then each finding's severity; then the waivers: the
// comments in the source first, the waiver files after.
std::vector<Final> resolve(std::vector<Finding>& found, const lintcfg::Config& cfg) {
    std::vector<Final> out;
    std::map<std::string, lintcfg::SourceMarks> marks;
    for (Finding& f : found) {
        if (!cfg.enabled(f.rule)) continue;
        Final x;
        x.sev = cfg.severity(f.rule);
        auto [it, fresh] = marks.try_emplace(f.file);
        if (fresh && !f.file.empty()) it->second.scan(f.file);
        x.waived = it->second.waived(f.rule, f.line, x.reason);
        if (!x.waived)
            for (const lintcfg::Waiver& w : cfg.waivers) {
                bool rule = false;
                for (auto& r : w.rules) rule |= lintcfg::Config::names(r, f.rule);
                if (!rule) continue;
                if (!w.file.empty() && !lintcfg::glob(w.file, f.file) && !lintcfg::glob("*/" + w.file, f.file)) continue;
                if (w.line0 && (f.line < w.line0 || f.line > w.line1)) continue;
                if (!w.msg.empty() && !lintcfg::glob(w.msg, f.text)) continue;
                w.hits++;
                x.waived = true;
                x.reason = w.reason.empty() ? w.origin : w.reason + " (" + w.origin + ")";
                break;
            }
        x.f = std::move(f);
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const Final& a, const Final& b) {
        return std::tie(a.f.file, a.f.line, a.f.col, a.f.rule, a.f.text) < std::tie(b.f.file, b.f.line, b.f.col, b.f.rule, b.f.text);
    });
    return out;
}

void printText(std::ostream& os, const std::vector<Final>& fs, const lintcfg::Config& cfg, bool quiet, bool showWaived) {
    std::map<std::string, int> per;
    size_t shown = 0, waived = 0;
    for (const Final& x : fs) {
        if (x.waived) {
            waived++;
            if (showWaived)
                os << x.f.file << ":" << x.f.line << ":" << x.f.col << ": waived: [" << x.f.rule << "] " << x.f.text
                   << " (" << x.reason << ")\n";
            continue;
        }
        os << x.f.file << ":" << x.f.line << ":" << x.f.col << ": " << lintcfg::sevName(x.sev) << ": [" << x.f.rule
           << "] " << x.f.text << "\n";
        per[x.f.rule]++;
        shown++;
    }
    if (quiet) return;
    os << "mlint: " << shown << (shown == 1 ? " finding" : " findings");
    bool first = true;
    for (auto& [r, n] : per) {
        os << (first ? " (" : ", ") << r << " " << n;
        first = false;
    }
    os << (first ? "" : ")");
    if (waived) os << ", " << waived << " waived";
    os << "\n";
    for (const lintcfg::Waiver& w : cfg.waivers)
        if (!w.hits) os << "mlint: " << w.origin << ": this waiver matched nothing; remove it\n";
}

bool writeJson(const std::string& path, const std::vector<Final>& fs) {
    std::ofstream os(path);
    if (!os) return false;
    using lintcfg::jsonStr;
    os << "{\n  \"tool\": \"mlint\",\n  \"version\": " << jsonStr(MINISIM_VERSION) << ",\n  \"findings\": [";
    bool first = true;
    std::map<std::string, int> counts;
    for (const Final& x : fs) {
        os << (first ? "\n" : ",\n") << "    {\"file\": " << jsonStr(x.f.file) << ", \"line\": " << x.f.line
           << ", \"col\": " << x.f.col << ", \"rule\": " << jsonStr(x.f.rule) << ", \"group\": "
           << jsonStr(lintcfg::groupOf(x.f.rule)) << ", \"severity\": " << jsonStr(lintcfg::sevName(x.sev))
           << ", \"text\": " << jsonStr(x.f.text) << ", \"waived\": " << (x.waived ? "true" : "false");
        if (x.waived) os << ", \"reason\": " << jsonStr(x.reason);
        os << "}";
        first = false;
        counts[x.waived ? "waived" : lintcfg::sevName(x.sev)]++;
    }
    os << (first ? "" : "\n  ") << "],\n  \"summary\": {\"error\": " << counts["error"] << ", \"warning\": "
       << counts["warning"] << ", \"info\": " << counts["info"] << ", \"waived\": " << counts["waived"] << "}\n}\n";
    return bool(os);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> fwd{argv[0]};
    lintcfg::Config cfg;
    bool werror = false, quiet = false, powerTopSet = false, showWaived = false, list = false, stats = false;
    std::string upfPath, powerTop, jsonPath, explainRule;
    unsigned threads = (unsigned)std::max<size_t>(1, cpuTopo().cpus.size());
    FrontEnd fe;
    cdc::Options cdcOpt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mlint: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        // NAME=VALUE, split at the first '='.
        auto pair = [&](const std::string& v, const char* what) -> std::pair<std::string, std::string> {
            size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::cerr << "mlint: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return {v.substr(0, eq), v.substr(eq + 1)};
        };
        const std::string origin = "command line";
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-list" || a == "--list") { list = true; continue; }
        if (a == "-explain") { explainRule = next("a rule name"); continue; }
        if (a == "-version" || a == "--version" || a == "-v") {
            std::cout << "mlint " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-disable") { cfg.directive({"disable", next("a rule or a group")}, origin); continue; }
        if (a == "-enable") { cfg.directive({"enable", next("a rule or a group")}, origin); continue; }
        if (a == "-only") { cfg.directive({"only", next("rules or groups")}, origin); continue; }
        if (a == "-severity") {
            auto [r, l] = pair(next("RULE=LEVEL"), "RULE=LEVEL");
            cfg.directive({"severity", r, l}, origin);
            continue;
        }
        if (a == "-set") {
            auto [k, v] = pair(next("NAME=VALUE"), "NAME=VALUE");
            cfg.directive({"set", k, v}, origin);
            continue;
        }
        if (a == "-config" || a == "-waivers" || a == "-waiver") { cfg.load(next("a file")); continue; }
        if (a == "-show-waived") { showWaived = true; continue; }
        if (a == "-stats") { stats = true; continue; }
        if (a == "-json") { jsonPath = next("a file"); continue; }
        if (a == "-threads") {
            std::string n = next("a count");
            if (n == "all") { threads = (unsigned)std::max<size_t>(1, cpuTopo().cpus.size()); continue; }
            int v = std::atoi(n.c_str());
            if (v < 1) { std::cerr << "mlint: -threads needs a count of 1 or more, or all\n"; return 2; }
            threads = (unsigned)v;
            fwd.push_back("--threads");
            fwd.push_back(n);
            continue;
        }
        if (a == "-Werror") { werror = true; continue; }
        if (cdcOpt.take(a, next)) continue;
        if (a == "-upf") { upfPath = next("a UPF file"); continue; }
        if (a == "-power_top") { powerTop = next("an instance path"); powerTopSet = true; continue; }
        if (a == "-quiet" || a == "-q") { quiet = true; continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        if (fe.prot.take(argc, argv, i)) continue;
        fwd.push_back(a);
    }
    lintrules::Params params = lintrules::Params::from(cfg, cfg.errors);
    if (!cfg.errors.empty()) {
        for (auto& e : cfg.errors) std::cerr << "mlint: " << e << "\n";
        return 2;
    }
    if (list) { listRules(cfg); return 0; }
    if (!explainRule.empty()) return explain(explainRule);

    // What is off is not computed at all.
    Rules rules;
    for (auto& r : lintcfg::table())
        if (!cfg.enabled(r.id)) rules.disabled.insert(r.id);
    for (auto& [r, t] : upf::rules())
        if (!cfg.enabled(r)) rules.disabled.insert(r);

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

    // Where the time goes, for -stats.
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    std::vector<std::pair<const char*, double>> phases;
    auto lap = [&](const char* what) {
        auto t1 = Clock::now();
        phases.push_back({what, std::chrono::duration<double>(t1 - t0).count()});
        t0 = t1;
    };

    // Pass 1: the front end, with its own warnings and errors.
    if (int rc = fe.run(fwd, "mlint front end")) return rc < 0 ? 0 : rc;

    // Pass 2: our rules. Two walks of the whole hierarchy on this thread (they
    // elaborate what the front end left for later, which must not race), then
    // everything else in parallel: each task fills its own report, and the
    // reports are merged in task order.
    lap("parse and elaborate");
    Report rep;
    rep.sm = fe.comp->getSourceManager();
    rep.rules = rules;
    if (fe.pc.any()) { rep.pc = &fe.pc; namePC() = &fe.pc; }
    Design d;
    d.gather(*fe.comp, rep);
    lap("design model");
    // L001 from the front end's data-flow analysis, which follows single
    // bits: z[i] assigned for every i in a loop is assigned, w[3:2] is not w.
    if (rep.rules.enabled("L001")) {
        fe.comp->freeze();
        analysis::AnalysisManager am({}, std::make_shared<ThreadPool>(threads));
        am.analyze(*fe.comp);
        for (auto& dg : am.getDiagnostics()) {
            if (dg.code != diag::InferredLatch || dg.args.empty()) continue;
            const std::string* name = std::get_if<std::string>(&dg.args[0]);
            if (!name) continue;
            rep.add("L001", dg.location,
                    "'" + *name +
                        "' is not assigned on every path of this always_comb, so it holds its old value "
                        "(an unintended latch); add an else or a default");
        }
        fe.comp->unfreeze();
    }
    lap("data-flow analysis (L001)");
    lintrules::Global global(params);
    fe.comp->getRoot().visit(global);
    lintrules::BodyCollector bodies;
    fe.comp->getRoot().visit(bodies);
    lap("hierarchy walks");

    bool upfFailed = false;
    const size_t fixed = 4, nb = bodies.out.size();
    std::vector<Report> reps(fixed + nb, rep);
    std::vector<lintrules::BodyOut> outs(nb);
    lintrules::parallelFor(fixed + nb, threads, [&](size_t i) {
        Report& r = reps[i];
        if (i == 0) {
            for (const Block& b : d.blocks) checkBlock(b, r);
            checkDrivers(d.drivers, d.wholeDriven, r);
        } else if (i == 1) {
            global.analyze(r);
        } else if (i == 2) {
            lintrules::checkDefinitions(*fe.comp, params, r);
        } else if (i == 3) {
            if (upfPath.empty()) return;
            try {
                checkUpf(*fe.comp, upfPath, powerTop, powerTopSet, r);
            } catch (const std::exception& ex) {
                std::cerr << "mlint: UPF: " << ex.what() << "\n";
                upfFailed = true;
            }
        } else {
            lintrules::BodyRules br(*bodies.out[i - fixed], r, params, outs[i - fixed]);
            br.run();
        }
    });
    if (upfFailed) return 1;
    lintrules::checkUnused(outs, rep);
    // Findings in protected code are counted, never shown (IEEE 1735).
    for (Report& r : reps) rep.hiddenSeen.insert(r.hiddenSeen.begin(), r.hiddenSeen.end());
    size_t hidden = rep.hiddenSeen.size();
    for (Report& r : reps)
        for (Finding& f : r.out)
            if (rep.seen.insert({f.rule, f.file + ":" + std::to_string(f.line) + ":" + std::to_string(f.col) + ":" + f.text}).second)
                rep.out.push_back(std::move(f));

    lap("rules");
    // Clock and reset domain crossings (cdc.hpp): one more walk of the
    // hierarchy, then its own worker threads.
    bool crossings = cdcOpt.report;
    for (auto& r : lintcfg::table())
        crossings |= (!std::strcmp(r.group, "cdc") || !std::strcmp(r.group, "rdc")) && rep.rules.enabled(r.id);
    if (crossings) {
        cdcOpt.threads = (int)threads;
        cdc::check(*fe.comp, rep, cdcOpt);
    }
    lap("clock and reset crossings");
    std::vector<Final> fs = resolve(rep.out, cfg);
    printText(std::cout, fs, cfg, quiet, showWaived);
    if (hidden)
        std::cout << "mlint: " << hidden << (hidden == 1 ? " finding" : " findings")
                  << " in protected code (details hidden)\n";
    if (!jsonPath.empty() && !writeJson(jsonPath, fs)) {
        std::cerr << "mlint: cannot write '" << jsonPath << "'\n";
        return 2;
    }
    lap("waivers and report");
    if (stats) {
        double total = 0;
        for (auto& [w, t] : phases) total += t;
        std::fprintf(stderr, "mlint: %zu module bodies, %zu processes, %u threads, %.3f s\n", nb, d.blocks.size(), threads, total);
        for (auto& [w, t] : phases) std::fprintf(stderr, "  %-28s %8.3f s\n", w, t);
    }
    bool errors = false, warnings = false;
    for (const Final& x : fs) {
        if (x.waived) continue;
        errors |= x.sev == lintcfg::Sev::Error;
        warnings |= x.sev == lintcfg::Sev::Warning;
    }
    return errors || (werror && warnings) ? 1 : 0;
}
