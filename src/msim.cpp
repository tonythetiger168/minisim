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
//
// Exit status: 0 when the simulation ran, 1 on a compile or lowering error,
// 2 on a bad command line.
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lower.hpp"
#include "slang/driver/Driver.h"

#define MINISIM_NO_MAIN
#include "minisim.cpp"

static bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// slang's driver owns these + prefixes; everything else beginning with + is a plusarg.
static bool slangPlus(std::string_view a) {
    return startsWith(a, "+define+") || startsWith(a, "+incdir+") || startsWith(a, "+libext+");
}

static void usage() {
    std::cerr << "usage: msim [options] <files...>\n"
                 "  -top <name>    top-level module (repeatable)\n"
                 "  -vcd [<file>]  write a VCD waveform (default dump.vcd)\n"
                 "  -maxtime <n>   stop after simulation time n\n"
                 "  +define+N[=V]  +incdir+<dir>  -f <file>  -I <dir>  -D N[=V]\n"
                 "  --help-slang   list every front-end option\n";
}

int main(int argc, char** argv) {
    // Pass 1: take msim's own options out; translate VCS spellings for slang.
    std::vector<std::string> fwd{argv[0]};
    std::vector<std::string> plusargs;
    std::optional<std::string> vcdPath;
    uint64_t maxTime = ~0ULL;
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

    S.vcdPath = vcdPath.value_or("");
    S.timescale = lowered.timescale();
    for (auto& pr : lowered.procs) S.spawn(process(&pr.prog));
    S.run(maxTime);
    std::vector<Task> finals;
    for (auto& pr : lowered.finals) finals.push_back(process(&pr.prog));
    S.runFinal(finals);
    std::cout.flush();
    return 0;
}
