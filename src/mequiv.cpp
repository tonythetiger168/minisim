// mequiv.cpp - a logic equivalence checker for SystemVerilog, on the same
// slang front end as msim: is this netlist the same circuit as that RTL?
//
//   mequiv -ref rtl/*.sv -impl netlist.v cells.v -top alu
//
// Either side may be RTL, a structural netlist with Verilog cell models, or a
// mix, so the same command checks RTL against a synthesized netlist, one
// netlist against another (before and after an ECO, scan insertion or
// placement optimization), or one RTL revision against the next.
//
// The method is the one sign-off equivalence checkers use: flops and latches
// of the two designs are paired by name and cut, and every primary output and
// every next-state function is then proved equal as plain combinational logic
// (structural hashing, random simulation, SAT). See src/equiv/check.hpp.
//
// Exit status: 0 equivalent, 1 not equivalent (counterexamples printed),
// 2 a design could not be read, 3 inconclusive (a proof ran out of effort).
#include <pthread.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

#include "slang/driver/Driver.h"

#include "equiv/check.hpp"
#include "equiv/elab.hpp"
#include "version.hpp"

using namespace equiv;

namespace {

const char* kHelp =
    "usage: mequiv [options] -ref <files and options> -impl <files and options>\n"
    "\n"
    "Proves that two designs are the same circuit. Each side is a list of\n"
    "SystemVerilog files (RTL, or a netlist plus its cell models) and the usual\n"
    "front-end options: +define+X=1, +incdir+DIR, -f FILE, -v LIBFILE, -y LIBDIR.\n"
    "Options given before -ref apply to both sides.\n"
    "\n"
    "  -top NAME        top module of both sides (default: the only top found)\n"
    "  -reftop NAME     top module of the reference only\n"
    "  -impltop NAME    top module of the implementation only\n"
    "  -match R=I       pair reference signal R with implementation signal I\n"
    "  -matchfile FILE  the same, one 'R I' pair per line\n"
    "  -blackbox MOD    do not look inside module MOD on either side; its pins\n"
    "                   become compare points and free inputs\n"
    "  -threads N       worker threads (default: all logical cores; 1 disables)\n"
    "  -effort N        SAT conflicts allowed per compare point (default 2000000)\n"
    "  -maxfail N       failing points to print in detail (default 10)\n"
    "  -report FILE     write every compare point and its result to FILE (JSON)\n"
    "  -verbose         list unmatched signals and every compare point\n"
    "  -q               print only the verdict\n"
    "  -version         print the version\n"
    "\n"
    "Exit status: 0 equivalent, 1 not equivalent, 2 error, 3 inconclusive.\n";

struct Side {
    const char* label;
    std::vector<std::string> args;
    std::string top;
    slang::driver::Driver driver;
    std::unique_ptr<slang::ast::Compilation> comp;
    const slang::ast::InstanceSymbol* topInst = nullptr;
    Design design;
    std::string error;
    size_t ands = 0;

    // Parse and elaborate with slang. Runs on its own thread.
    void compile() {
        try {
            driver.addStandardArgs();
            std::vector<std::string> a{"mequiv", "--disable-instance-caching", "-Wnone"};
            if (!top.empty()) a.push_back("--top"), a.push_back(top);
            a.insert(a.end(), args.begin(), args.end());
            std::vector<const char*> cargv;
            for (auto& s : a) cargv.push_back(s.c_str());
            if (!driver.parseCommandLine(int(cargv.size()), cargv.data()) || !driver.processOptions() ||
                !driver.parseAllSources()) {
                error = "could not read the sources";
                return;
            }
            comp = driver.createCompilation();
            driver.reportCompilation(*comp, /*quiet*/ true);
            if (!driver.reportDiagnostics(/*quiet*/ true)) {
                error = "the design does not elaborate";
                return;
            }
            auto tops = comp->getRoot().topInstances;
            if (tops.empty()) error = "no top module found";
            else if (tops.size() == 1) topInst = tops[0];
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
};

// Signal evaluation recurses along the logic, so a deep netlist needs a deep
// stack: run it on a thread that has one.
template<typename F>
void onBigStack(F&& f) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, size_t(4) << 30);
    pthread_t th;
    auto thunk = [](void* p) -> void* {
        (*static_cast<F*>(p))();
        return nullptr;
    };
    if (pthread_create(&th, &attr, thunk, &f) != 0) {
        f();  // no such stack to be had: run where we are
        return;
    }
    pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
}

std::string jsonStr(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += c;
    }
    return r + "\"";
}

const char* kindName(Point::Kind k) {
    return k == Point::Output ? "output" : k == Point::Next ? "next-state" : "clock/reset";
}
const char* valueName(int v) { return v == 0 ? "0" : v == 1 ? "1" : v == 2 ? "X" : "?"; }

}  // namespace

int main(int argc, char** argv) {
    Side ref{"reference"}, impl{"implementation"};
    CheckOptions copt;
    ElabOptions eopt;
    std::string reportPath;
    bool verbose = false, quiet = false;
    size_t maxFail = 10;

    int mode = 0;  // 0 common, 1 ref, 2 impl
    auto need = [&](int& i) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << "mequiv: " << argv[i] << " needs a value\n";
            std::exit(2);
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "-help" || a == "--help") { std::cout << kHelp; return 0; }
        if (a == "-version" || a == "--version") {
            std::cout << "mequiv " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-ref") { mode = 1; continue; }
        if (a == "-impl") { mode = 2; continue; }
        if (a == "-top") { ref.top = impl.top = need(i); continue; }
        if (a == "-reftop") { ref.top = need(i); continue; }
        if (a == "-impltop") { impl.top = need(i); continue; }
        if (a == "-threads") { copt.threads = unsigned(std::stoul(need(i))); continue; }
        if (a == "-effort") { copt.effort = std::stoull(need(i)); continue; }
        if (a == "-maxfail") { maxFail = std::stoul(need(i)); continue; }
        if (a == "-report") { reportPath = need(i); continue; }
        if (a == "-blackbox") { eopt.blackboxes.insert(need(i)); continue; }
        if (a == "-verbose") { verbose = true; continue; }
        if (a == "-q") { quiet = true; continue; }
        if (a == "-match") {
            std::string m = need(i);
            auto eq = m.find('=');
            if (eq == std::string::npos) { std::cerr << "mequiv: -match wants REF=IMPL\n"; return 2; }
            copt.matches.push_back({m.substr(0, eq), m.substr(eq + 1)});
            continue;
        }
        if (a == "-matchfile") {
            std::string path = need(i);
            std::ifstream in(path);
            if (!in) { std::cerr << "mequiv: cannot read " << path << "\n"; return 2; }
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream ls(line);
                std::string r, m;
                if (ls >> r >> m && r[0] != '#') copt.matches.push_back({r, m});
            }
            continue;
        }
        if (a == "-sv") continue;  // msim's spelling; everything is SystemVerilog here
        if (mode != 2) ref.args.push_back(a);
        if (mode != 1) impl.args.push_back(a);
    }
    if (mode == 0) { std::cerr << kHelp; return 2; }

    auto t0 = std::chrono::steady_clock::now();
    auto seconds = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    // Read both designs at once.
    if (copt.threads == 1) {
        ref.compile();
        impl.compile();
    } else {
        std::thread t([&] { ref.compile(); });
        impl.compile();
        t.join();
    }
    // A side with several unused modules (a netlist read with its whole cell
    // library) takes the top the other side has.
    for (Side* s : {&ref, &impl}) {
        Side* o = s == &ref ? &impl : &ref;
        if (!s->error.empty() || s->topInst) continue;
        auto tops = s->comp->getRoot().topInstances;
        if (o->error.empty() && o->comp->getRoot().topInstances.size() == 1)
            for (auto* t : tops)
                if (t->name == o->comp->getRoot().topInstances[0]->name) s->topInst = t;
        if (!s->topInst) {
            s->error = "more than one top module; name one with -top (found:";
            for (auto* t : tops) s->error += " " + std::string(t->name);
            s->error += ")";
        }
    }
    for (Side* s : {&ref, &impl})
        if (!s->error.empty()) {
            std::cerr << "mequiv: " << s->label << ": " << s->error << "\n";
            return 2;
        }

    Aig g;
    for (Side* s : {&ref, &impl}) {
        size_t before = g.numAnds();
        onBigStack([&] {
            try {
                Elab e(g, *s->comp, s->design, eopt);
                e.run(*s->topInst);
            } catch (const std::exception& e) {
                s->error = e.what();
            }
        });
        if (!s->error.empty()) {
            std::cerr << "mequiv: " << s->label << ": cannot be turned into logic: " << s->error << "\n";
            return 2;
        }
        s->ands = g.numAnds() - before;
    }
    double tRead = seconds();

    Result res = check(g, ref.design, impl.design, copt);

    auto count = [](const Design& d, bool latch) {
        size_t n = 0;
        for (auto& s : d.states) n += s.latch == latch;
        return n;
    };
    if (!quiet) {
        for (Side* s : {&ref, &impl}) {
            printf("%-15s top '%s': %zu inputs, %zu outputs, %zu flops, %zu latches, %zu AND nodes\n",
                   (std::string(s->label) + ":").c_str(), s->design.top.c_str(), s->design.inputs.size(),
                   s->design.outputs.size(), count(s->design, false), count(s->design, true), s->ands);
            size_t shown = 0;
            for (auto& w : s->design.warnings) {
                if (!verbose && ++shown > 5) {
                    printf("  ... %zu more warnings (-verbose)\n", s->design.warnings.size() - 5);
                    break;
                }
                printf("  warning: %s\n", w.c_str());
            }
        }
        if (verbose)
            for (Side* s : {&ref, &impl})
                for (auto& st : s->design.states)
                    if (st.latch) printf("  %s latch: %s\n", s->label, st.name.c_str());
        printf("Matched:        %zu inputs, %zu outputs, %zu state elements", res.matchedInputs,
               res.matchedOutputs, res.matchedStates);
        if (res.invertedStates) printf(" (%zu stored inverted)", res.invertedStates);
        printf("\n");
        auto list = [&](const char* what, const std::vector<std::string>& v) {
            if (v.empty()) return;
            printf("Unmatched %s: %zu", what, v.size());
            size_t lim = verbose ? v.size() : 5;
            for (size_t i = 0; i < v.size() && i < lim; ++i) printf("%s%s", i ? ", " : " (", v[i].c_str());
            printf("%s)\n", v.size() > lim ? ", ..." : "");
        };
        list("reference inputs", res.refOnlyInputs);
        list("implementation inputs", res.implOnlyInputs);
        list("reference state", res.refOnlyStates);
        list("implementation state", res.implOnlyStates);
        list("reference outputs", res.refOnlyOutputs);
        list("implementation outputs", res.implOnlyOutputs);
        size_t byStruct = 0, bySim = 0, bySat = 0;
        for (auto& p : res.points) {
            std::string how = p.how;
            byStruct += how == "structural", bySim += how == "simulation", bySat += how == "sat";
        }
        printf("Compare points: %zu (%zu settled structurally, %zu by simulation, %zu by SAT)\n",
               res.points.size(), byStruct, bySim, bySat);
        printf("  passed %zu, failed %zu, aborted %zu\n", res.passed, res.failed, res.aborted);

        size_t shown = 0;
        for (auto& p : res.points) {
            if (p.status == Point::Pass) {
                if (verbose) printf("  pass  %-11s %s (%s)\n", kindName(p.kind), p.name.c_str(), p.how);
                continue;
            }
            if (p.status == Point::Abort) {
                printf("  ABORT %-11s %s: no answer within %llu conflicts (raise -effort)\n", kindName(p.kind),
                       p.name.c_str(), static_cast<unsigned long long>(copt.effort));
                continue;
            }
            if (++shown > maxFail) continue;
            printf("  FAIL  %-11s %s", kindName(p.kind), p.name.c_str());
            if (!p.implName.empty()) printf("  (implementation: %s)", p.implName.c_str());
            printf("\n");
            if (!p.note.empty()) { printf("        %s\n", p.note.c_str()); continue; }
            printf("        reference = %s, implementation = %s when", valueName(p.refValue), valueName(p.implValue));
            size_t n = 0;
            for (auto& [name, v] : p.cex) {
                if (++n > 24 && !verbose) { printf(" ... (%zu signals; -verbose shows all)", p.cex.size()); break; }
                printf(" %s=%d", name.c_str(), int(v));
            }
            printf("\n");
        }
        if (shown > maxFail) printf("  ... %zu more failing points (-maxfail)\n", shown - maxFail);
    }

    if (!reportPath.empty()) {
        std::ofstream out(reportPath);
        out << "{\n  \"reference\": " << jsonStr(ref.design.top) << ",\n  \"implementation\": " << jsonStr(impl.design.top)
            << ",\n  \"passed\": " << res.passed << ",\n  \"failed\": " << res.failed << ",\n  \"aborted\": "
            << res.aborted << ",\n  \"equivalent\": " << (res.equivalent() ? "true" : "false") << ",\n  \"points\": [\n";
        for (size_t i = 0; i < res.points.size(); ++i) {
            auto& p = res.points[i];
            out << "    {\"kind\": " << jsonStr(kindName(p.kind)) << ", \"name\": " << jsonStr(p.name)
                << ", \"status\": " << jsonStr(p.status == Point::Pass ? "pass" : p.status == Point::Fail ? "fail" : "abort")
                << ", \"by\": " << jsonStr(p.how);
            if (!p.implName.empty()) out << ", \"impl_name\": " << jsonStr(p.implName);
            if (p.status == Point::Fail) {
                out << ", \"ref\": " << jsonStr(valueName(p.refValue)) << ", \"impl\": " << jsonStr(valueName(p.implValue))
                    << ", \"when\": {";
                for (size_t k = 0; k < p.cex.size(); ++k)
                    out << (k ? ", " : "") << jsonStr(p.cex[k].first) << ": " << int(p.cex[k].second);
                out << "}";
            }
            out << "}" << (i + 1 < res.points.size() ? "," : "") << "\n";
        }
        out << "  ]\n}\n";
    }

    const char* verdict = res.failed || !res.refOnlyOutputs.empty() ? "FAILED: the designs are NOT equivalent"
                        : res.aborted ? "INCONCLUSIVE: some compare points were not decided"
                        : "SUCCEEDED: the designs are equivalent";
    printf("Verification %s\n", verdict);
    if (!quiet) printf("(read %.2fs, verify %.2fs)\n", tRead, seconds() - tRead);
    return res.failed || !res.refOnlyOutputs.empty() ? 1 : res.aborted ? 3 : 0;
}
