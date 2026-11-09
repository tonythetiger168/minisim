// msim.cpp - one-step compile, elaborate and simulate, in the style of
// `irun` / `xrun` and `vcs -R`:
//
//   msim tb.sv dut.sv -top tb +define+WIDTH=8 +incdir+inc -f files.f -vcd waves.vcd
//
// Front end options (source files, -f/-F command files, +define+, +incdir+,
// -I, -D, -y/-v libraries, --top, -G) go to slang's own driver, which
// already accepts the VCS spellings. msim adds:
//
//   -top <name>      same as --top (the VCS / irun spelling)
//   -vcd [<file>]    write a VCD waveform (default file dump.vcd)
//   +vcd             same as -vcd dump.vcd
//   -maxtime <n>     stop after simulation time n (default: run until no events or $finish)
//   -sv, -sverilog   accepted and ignored (input is always SystemVerilog)
//   +<plusarg>       any other +arg is a runtime plusarg (kept for $test$plusargs, not yet read)
//   -compile         compiled mode: generate C++ for the design (codegen.hpp),
//                    build it with a C++ compiler into <dir>/simv, and run it.
//                    A design codegen cannot translate runs on the interpreter.
//   -o <dir>         build directory for -compile (default msim.d)
//   -no-static-comb  run continuous assigns and always_comb as ordinary
//                    event-driven processes instead of one levelized schedule
//   -no-static-seq   run NBA-only always @(edge) blocks as ordinary processes
//   -no-partition    apply every NBA on the main thread (no partition tasks)
//   -no-2state       -compile without the 2-state fast path (codegen.hpp fast())
//   -threads <n>     worker threads (default: every hardware thread). Results
//                    are the same for any n; -threads 1 is single-threaded.
//   -cc <compiler>   C++ compiler for -compile (default $CXX, else c++)
//   -O<n>            optimization level for -compile (default -O2)
//
// Exit status: 0 when the simulation ran, 1 on a compile or lowering error,
// 2 on a bad command line.
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/wait.h>

#include "codegen.hpp"
#include "lower.hpp"
#include "slang/driver/Driver.h"

#define MINISIM_NO_MAIN
#include "minisim.cpp"

static bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// slang's driver owns these + prefixes; everything else beginning with + is a plusarg.
static bool slangPlus(std::string_view a) {
    return startsWith(a, "+define+") || startsWith(a, "+incdir+") || startsWith(a, "+libext+");
}

// Single-quote a path for /bin/sh.
static std::string shq(const std::string& s) {
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
}

#ifndef MINISIM_SRC_DIR
#define MINISIM_SRC_DIR "src"
#endif

// How each process runs: on the static combinational schedule (rank and
// level), as a clocked function (seq), or as an ordinary coroutine.
struct Plan {
    std::vector<int> rank, level;
    std::vector<std::vector<int>> ins;
    std::vector<size_t> body;                       // leading Assigns of a comb process
    std::vector<std::vector<int>> outs;             // comb process outputs
    std::vector<char> seq;
    std::vector<std::vector<std::pair<int, Edge>>> ev;
};
static Plan plan(const LowerResult& lowered, bool staticComb, bool staticSeq) {
    size_t n = lowered.procs.size();
    Plan p;
    p.rank.assign(n, -1);
    p.level.assign(n, 0);
    p.ins.assign(n, {});
    p.body.assign(n, 0);
    p.outs.assign(n, {});
    p.seq.assign(n, 0);
    p.ev.assign(n, {});
    if (staticComb) {
        std::vector<CombShape> shapes;
        std::vector<size_t> procOf;
        for (size_t i = 0; i < n; i++) {
            CombShape c;
            if (combShape(lowered.procs[i].prog, c)) { shapes.push_back(c); procOf.push_back(i); }
        }
        std::vector<int> lv;
        std::vector<size_t> order = combSchedule(shapes, &lv);
        for (size_t r = 0; r < order.size(); r++) {
            size_t i = procOf[order[r]];
            p.rank[i] = (int)r;
            p.level[i] = lv[r];
            p.ins[i] = shapes[order[r]].ins;
            p.body[i] = shapes[order[r]].body;
            p.outs[i] = shapes[order[r]].outs;
        }
    }
    if (staticSeq)
        for (size_t i = 0; i < n; i++)
            if (p.rank[i] < 0 && seqShape(lowered.procs[i].prog, p.ev[i])) p.seq[i] = 1;
    return p;
}

// Inputs to Sim::buildPartitions, by rank, and every fixed NBA target.
static void partitionInputs(const LowerResult& lowered, const Plan& pl, std::vector<std::vector<int>>& outs,
                            std::vector<std::vector<int>>& ins, std::vector<int>& targets) {
    for (size_t i = 0; i < pl.rank.size(); i++) {
        if (pl.rank[i] < 0) continue;
        if ((size_t)pl.rank[i] >= outs.size()) { outs.resize(pl.rank[i] + 1); ins.resize(pl.rank[i] + 1); }
        outs[pl.rank[i]] = pl.outs[i];
        ins[pl.rank[i]] = pl.ins[i];
    }
    for (size_t i = 0; i < pl.seq.size(); i++)
        if (pl.seq[i]) seqTargets(lowered.procs[i].prog, targets);
}

// -compile: generate, build and run. Returns the exit status, or -1 when the
// design needs the interpreter.
static int runCompiled(const LowerResult& lowered, const std::string& dir, const std::string& cc,
                       const std::string& opt, const std::optional<std::string>& vcdPath, uint64_t maxTime,
                       const Plan& pl, int threads, bool staticPart, bool twoState) {
    std::vector<CodegenSig> sigs;
    for (auto& g : S.sigs) sigs.push_back({g.name, g.w, g.net, (int)g.drv.size()});
    std::vector<CodegenProc> procs;
    for (size_t i = 0; i < lowered.procs.size(); i++) {
        CodegenProc c{&lowered.procs[i].prog, lowered.procs[i].kind, pl.rank[i], pl.ins[i]};
        c.combLevel = pl.level[i];
        c.seq = pl.seq[i];
        c.seqEv = pl.ev[i];
        procs.push_back(std::move(c));
    }
    CodegenParts parts;
    parts.on = staticPart;
    if (staticPart) partitionInputs(lowered, pl, parts.outs, parts.ins, parts.targets);
    std::string src;
    // Not compiled yet: final blocks, and a tick other than 1ns (`timescale).
    const char* why = !lowered.finals.empty() ? "a final block"
                    : !lowered.netDelays.empty() ? "a net delay"
                    : lowered.timescale() != "1ns" ? "a `timescale precision other than 1ns" : nullptr;
    if (why) {
        std::cerr << "msim: -compile: " << why << " is not compiled yet; using the interpreter\n";
        return -1;
    }
    try {
        src = codegen(sigs, procs, parts, twoState);
    } catch (const CodegenUnsupported& ex) {
        std::cerr << "msim: -compile: " << ex.what() << " is not compiled yet; using the interpreter\n";
        return -1;
    }
    std::string inc = MINISIM_SRC_DIR;
    if (const char* e = std::getenv("MINISIM_INCLUDE")) inc = e;
    if (std::system(("mkdir -p " + shq(dir)).c_str()) != 0) { std::cerr << "msim: cannot create " << dir << "\n"; return 1; }
    std::string cpp = dir + "/design.cpp", exe = dir + "/simv";
    {
        std::ofstream f(cpp);
        f << src;
        if (!f) { std::cerr << "msim: cannot write " << cpp << "\n"; return 1; }
    }
    std::string build = shq(cc) + " -std=c++20 " + opt + " -I" + shq(inc) + " " + shq(cpp) + " -o " + shq(exe);
    if (std::system(build.c_str()) != 0) {
        std::cerr << "msim: -compile: C++ build failed: " << build << "\n";
        return 1;
    }
    std::string run = shq(exe);
    if (maxTime != ~0ULL) run += " -maxtime " + std::to_string(maxTime);
    if (vcdPath) run += " -vcd " + shq(*vcdPath);
    run += " -threads " + std::to_string(threads);
    std::cout.flush();
    int st = std::system(run.c_str());
    if (st == -1) return 1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static void usage() {
    std::cerr << "usage: msim [options] <files...>\n"
                 "  -top <name>    top-level module (repeatable)\n"
                 "  -vcd [<file>]  write a VCD waveform (default dump.vcd)\n"
                 "  -maxtime <n>   stop after simulation time n\n"
                 "  -compile       generate C++, build it (<dir>/simv) and run it\n"
                 "  -o <dir>       build directory for -compile (default msim.d)\n"
                 "  -cc <cxx>      C++ compiler for -compile (default $CXX, else c++)\n"
                 "  -threads <n>   worker threads (default: all hardware threads)\n"
                 "  +define+N[=V]  +incdir+<dir>  -f <file>  -I <dir>  -D N[=V]\n"
                 "  --help-slang   list every front-end option\n";
}

int main(int argc, char** argv) {
    // Pass 1: take msim's own options out; translate VCS spellings for slang.
    std::vector<std::string> fwd{argv[0]};
    std::vector<std::string> plusargs;
    std::optional<std::string> vcdPath;
    uint64_t maxTime = ~0ULL;
    bool compiled = false, staticComb = true, staticSeq = true, staticPart = true, twoState = true;
    int threads = defaultThreads();
    std::string buildDir = "msim.d", opt = "-O2";
    std::string cc = std::getenv("CXX") ? std::getenv("CXX") : "c++";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << "msim: " << a << " needs " << what << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        if (a == "-compile") { compiled = true; continue; }
        if (a == "-no-static-comb") { staticComb = false; continue; }
        if (a == "-no-static-seq") { staticSeq = false; continue; }
        if (a == "-no-partition") { staticPart = false; continue; }
        if (a == "-no-2state") { twoState = false; continue; }
        if (a == "-threads") {
            std::string n = next("a thread count");
            try { threads = std::stoi(n); }
            catch (...) { std::cerr << "msim: bad -threads " << n << "\n"; return 2; }
            if (threads < 1) { std::cerr << "msim: -threads must be at least 1\n"; return 2; }
            // More threads than hardware threads only adds waiting: a batch
            // cannot finish until a descheduled worker runs again.
            int hw = (int)std::thread::hardware_concurrency();
            if (hw > 0 && threads > hw) {
                std::cerr << "msim: -threads " << threads << " is more than the " << hw
                          << " hardware threads; using " << hw << "\n";
                threads = hw;
            }
            continue;
        }
        if (a == "-o") { buildDir = next("a directory"); continue; }
        if (a == "-cc") { cc = next("a compiler"); continue; }
        if (a.size() == 3 && a[0] == '-' && a[1] == 'O' && std::isdigit((unsigned char)a[2])) { opt = a; continue; }
        if (a == "+vcd") { vcdPath = "dump.vcd"; continue; }
        if (a == "-vcd") {
            if (i + 1 < argc && std::string_view(argv[i + 1]).ends_with(".vcd")) vcdPath = argv[++i];
            else vcdPath = "dump.vcd";
            continue;
        }
        if (a == "-maxtime") {
            std::string n = next("a time");
            try { maxTime = std::stoull(n); }
            catch (...) { std::cerr << "msim: bad -maxtime " << n << "\n"; return 2; }
            continue;
        }
        if (a.size() > 1 && a[0] == '+' && !slangPlus(a)) { plusargs.push_back(a.substr(1)); continue; }
        fwd.push_back(a);
    }

    // Pass 2: slang parses, elaborates and reports diagnostics.
    slang::driver::Driver driver;
    driver.addStandardArgs();
    std::vector<const char*> cargv;
    for (auto& s : fwd) cargv.push_back(s.c_str());
    if (!driver.parseCommandLine((int)cargv.size(), cargv.data())) return 2;
    for (auto& s : fwd) if (s == "--help") { std::cout << driver.cmdLine.getHelpText("msim front end") << "\n"; return 0; }
    if (!driver.processOptions()) return 2;
    if (!driver.parseAllSources()) return 1;

    auto comp = driver.createCompilation();
    driver.reportCompilation(*comp, /*quiet*/ true);
    if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;

    // Pass 3: lower to the process IR and run on the kernel.
    LowerResult lowered;
    try {
        lowered = lower(*comp,
                        [](const std::string& n, int w, bool net) { return S.sig(n, w, net); },
                        [](int s) { return S.newDriver(s); });
    } catch (const std::exception& ex) {
        std::cerr << "msim: " << ex.what() << "\n";
        return 1;
    }
    if (lowered.procs.empty()) {
        std::cerr << "msim: design has no processes to run\n";
        return 1;
    }

    Plan pl = plan(lowered, staticComb, staticSeq);
    if (compiled) {
        int st = runCompiled(lowered, buildDir, cc, opt, vcdPath, maxTime, pl, threads, staticPart, twoState);
        if (st >= 0) return st;
    }

    S.vcdPath = vcdPath.value_or("");
    S.timescale = lowered.timescale();
    for (auto& [s, d] : lowered.netDelays) S.netDelay[s] = d;
    // A delayed driver is x until its first update arrives (10.3.3, 28.16),
    // and so is a net with a net delay that has drivers.
    {
        std::set<int> touched;
        for (auto& lp : lowered.procs)
            for (auto& i : lp.prog.code)
                if (!i.dly.empty() && i.drv >= 0) {
                    int w = S.sigs[i.sig].w;
                    S.sigs[i.sig].drv[i.drv].v = i.hi >= 0 ? v_insert(allz(w), allx(i.hi - i.lo + 1), i.hi, i.lo) : allx(w);
                    touched.insert(i.sig);
                }
        for (int s : touched) {
            auto& g = S.sigs[s];
            g.val = resolveDrives(g.drv.data(), g.drv.size(), g.w);
        }
        for (auto& [s, d] : lowered.netDelays)
            if (!S.sigs[s].drv.empty()) S.sigs[s].val = allx(S.sigs[s].w);
    }
    S.threads = threads;
    std::vector<int> byRank(lowered.procs.size(), -1);
    for (size_t i = 0; i < pl.rank.size(); i++)
        if (pl.rank[i] >= 0) byRank[pl.rank[i]] = (int)i;
    for (int i : byRank) {
        if (i < 0) continue;
        const Prog* pr = &lowered.procs[i].prog;
        if (pl.body[i] == 1) S.addComb(execComb, pr, pl.ins[i], pl.level[i], combValue, combCommit);
        else S.addComb(execComb, pr, pl.ins[i], pl.level[i]);
    }
    for (size_t i = 0; i < lowered.procs.size(); i++) {
        if (pl.rank[i] >= 0) continue;
        if (pl.seq[i]) S.addSeq(execSeq, &lowered.procs[i].prog, pl.ev[i]);
        else S.spawn(process(&lowered.procs[i].prog));
    }
    if (staticPart) {
        std::vector<std::vector<int>> po, pi;
        std::vector<int> pt;
        partitionInputs(lowered, pl, po, pi, pt);
        S.buildPartitions(po, pi, pt);
    }
    S.run(maxTime);
    std::vector<Task> finals;
    for (auto& pr : lowered.finals) finals.push_back(process(&pr.prog));
    S.runFinal(finals);
    std::cout.flush();
    return S.exitCode;
}
