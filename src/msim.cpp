#include <csignal>
#include <unistd.h>
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
//   +notimingcheck   turn off the specify block timing checks (31)
//   -sdf [min|typ|max:]<instance>:<file>
//                    annotate an SDF file to an instance before time 0, as
//                    $sdf_annotate(file, instance) would (32); repeatable
//   +mindelays / +typdelays / +maxdelays
//                    pick the min, typ or max of min:typ:max delays (11.11)
//                    and of SDF triples (default typ)
//   -libmap <file>   a library map file (33.3); same as --libmap. -L <lib>
//                    sets the library search order (33.8.1); a config is
//                    chosen with -top <config>
//   +neg_tchk        allow negative $setuphold / $recrem limits (31.9)
//   +pulse_r/<n> +pulse_e/<n>
//                    reject and error pulse limits as n% of each path delay (30.7.2)
//   +pulse_on_event / +pulse_on_detect, +showcancelled / +noshowcancelled
//                    pulse styles of every path output (30.7.4)
//   -fst [<file>]    write an FST waveform (default dump.fst); -vcd x.fst too
//   -saif [<file>]   write switching activity as SAIF (default dump.saif): the
//                    time each bit spent at 0, 1 and x and its toggle count,
//                    for power analysis with mpower (docs/POWER.md)
//   -maxtime <n>     stop after simulation time n (default: run until no events or $finish)
//   -sv, -sverilog   accepted and ignored (input is always SystemVerilog)
//   +<plusarg>       any other +arg is a runtime plusarg ($test$plusargs, $value$plusargs)
//   -seed <n>        seed for $urandom / $urandom_range (also +ntb_random_seed=<n>)
//   -compile         compiled mode: generate C++ for the design (codegen.hpp),
//                    build it with a C++ compiler into <dir>/simv, and run it.
//                    A design codegen cannot translate runs on the interpreter.
//                    With -cov the generated code counts hits too. An
//                    unchanged design reuses the simv already in <dir>; seeds
//                    and plusargs are passed to it at run time.
//   -o <dir>         build directory for -compile (default msim.d)
//   -no-static-comb  run continuous assigns and always_comb as ordinary
//                    event-driven processes instead of one levelized schedule
//   -no-static-seq   run NBA-only always @(edge) blocks as ordinary processes
//   -no-partition    apply every NBA on the main thread (no partition tasks)
//   -no-2state       -compile without the 2-state fast path (codegen.hpp fast())
//   -no-cycle        -compile without the cycle domain (codegen.hpp cycleDomain())
//   -threads <n>     worker threads (default: one per physical P core, at most
//                    8; cpu.hpp). `-threads all` uses every logical CPU, SMT
//                    siblings included. Results are the same for any n.
//   -pin / -no-pin   pin worker thread k to a distinct physical core, SMT
//                    siblings last (default: pin, except under a hypervisor)
//   -cpuinfo         print the detected CPU topology and exit
//   -cc <compiler>   C++ compiler for -compile (default $CXX, else c++)
//   -cfiles <n>      -compile: build the design as up to n C++ files in parallel
//                    (default: one per core, 16 at most, for a large design)
//   -O<n>            optimization level for -compile (default -O2)
//   -keyfile [[<owner>:]<key_keyname>=]<private.pem>
//                    an RSA private key for protected envelopes (`pragma
//                    protect begin_protected: IEEE 1735-2023 version 3, or
//                    IEEE 1800-2023 clause 34). Repeatable. With a name the
//                    key only opens tool blocks / key_blocks of that
//                    key_keyname (and key_keyowner). Encrypt with msimprotect.
//   -keyring <file>  private keys, `<owner> <name> <private.pem>` per line
//   -license <feature>[=<string>]  a licensed feature for licensed rights
//
// Protected code is decrypted in memory only. Diagnostics located in it are
// reported without their text, its signals are left out of the VCD, and
// -compile falls back to the interpreter so no generated C++ holds it.
//   -upf <file>      power-aware simulation with this IEEE 1801 (UPF) power
//                    intent (upf.hpp, upf_rt.inc). Adds package UPF for the
//                    testbench (supply_on, supply_off, ...).
//   -power_top <inst> the instance the UPF file describes, as a path from
//                    the simulation top (u_dut, or tb/u_dut); default: the
//                    UPF's set_design_top, else the simulation top
//   -upf_verbose     print supply, power state and simstate changes
//
// DPI-C (IEEE 1800-2023 clause 35, annex J):
//   <file>.c / .cc / .cpp / .cxx, .o, .a   C or C++ for DPI imports; msim builds
//                    them, with svdpi.h on the include path, into one library
//                    (in -o <dir> if given, else a temporary directory)
//   <file>.so        a DPI library to load
//   -sv_lib <name>   load <name>.so (annex J.3), relative to -sv_root
//   -sv_root <dir>   directory for relative -sv_lib names
//   -sv_liblist <f>  a bootstrap file of library names (J.4)
//   -dpiheader <f>   write the C prototypes of every import and export to f
//   -mvp             add the virtual prototype layer: mvp.sv (mvp_axil_slave,
//                    mvp_irq_slave, mvp_clock) and the runtime its models use.
//                    Give your model C files on the command line as well; the
//                    bundled examples are in <prefix>/share/minisim/mvp/models
//   -uvm             the Accellera UVM library (IEEE 1800.2) in $UVM_HOME:
//                    +incdir+$UVM_HOME/src, uvm_pkg.sv, and UVM's C code
//                    (regular expressions, command line, HDL backdoor over
//                    VPI) built from uvm/uvm_dpi_minisim.cc
//   -uvmhome <dir>   the same with the library in <dir>
//   -uvmnodpi        the same without the C code (+define+UVM_NO_DPI)
//   -CFLAGS <s>      extra flags for compiling the C files
//   -LDFLAGS <s>     extra flags for linking them (e.g. -lm)
//
// VPI (IEEE 1800-2023 clauses 36-38):
//   -load <lib>[:<boot>]     load a VPI library before elaboration and run
//                    its vlog_startup_routines, and <boot> if given (VCS form)
//   -loadvpi <lib>:<boot>    the same (Xcelium form)
//   The C files and .so libraries above are VPI libraries too: a library
//   with vlog_startup_routines has them run. A design that uses VPI runs on
//   one thread in the interpreter.
//
// Exit status: 0 when the simulation ran, 1 on a compile or lowering error,
// 2 on a bad command line.
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_set>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <filesystem>
#include <map>
#include <set>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include "codegen.hpp"
#include "lower.hpp"
#include "saif.hpp"
#include "slang/diagnostics/DiagnosticClient.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/driver/Driver.h"
#include "slang/text/SourceManager.h"
#include "protect_front.hpp"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"

#define MINISIM_NO_MAIN
#include "minisim.cpp"
#include "upf_rt.inc"
#include "dpi.inc"
#include "vpi.inc"

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

#include "version.hpp"

static void version() {
    std::cout << "msim " MINISIM_VERSION;
    if (*MINISIM_GIT) std::cout << " (" MINISIM_GIT ")";
    std::cout << "\nIEEE 1800-2023 SystemVerilog simulator, slang front end\n"
                 "https://github.com/tonythetiger168/minisim\n";
}

// Kernel sources that -compile builds against. $MINISIM_INCLUDE wins. An
// installed msim (bin/msim) finds them in ../share/minisim/include next to
// itself; a build-tree msim uses the source tree it was configured from.
// libfst sources for an FST waveform from simv: $MINISIM_FST_SRC, the
// installed share/minisim/include/fst, or the build tree's fetched copy.
static std::string kernelIncludeDir();
static std::string fstSourceDir() {
    std::error_code ec;
    for (std::string d : {std::getenv("MINISIM_FST_SRC") ? std::string(std::getenv("MINISIM_FST_SRC")) : std::string(),
                          kernelIncludeDir() + "/fst",
#ifdef MINISIM_FST_DIR
                          std::string(MINISIM_FST_DIR),
#endif
                          std::string()})
        if (!d.empty() && std::filesystem::exists(d + "/fstapi.c", ec)) return d;
    return "";
}

static std::string kernelIncludeDir() {
    if (const char* e = std::getenv("MINISIM_INCLUDE")) return e;
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        auto inst = exe.parent_path().parent_path() / "share" / "minisim" / "include";
        if (std::filesystem::exists(inst / "minisim.cpp", ec)) return inst.string();
    }
    return MINISIM_SRC_DIR;
}

// svdpi.h for compiling DPI C code. An installed msim finds it in
// ../include next to itself; a build-tree msim uses the source tree.
static std::string dpiIncludeDir() {
    if (const char* e = std::getenv("MINISIM_DPI_INCLUDE")) return e;
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        auto inst = exe.parent_path().parent_path() / "include";
        if (std::filesystem::exists(inst / "svdpi.h", ec)) return inst.string();
    }
    return std::string(MINISIM_SRC_DIR) + "/../include";
}
// The virtual prototype layer (mvp/): mvp.sv for the design and mvp.c for the
// model library, found next to an installed msim or in the source tree.
static std::string mvpDir() {
    if (const char* e = std::getenv("MINISIM_MVP")) return e;
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        auto inst = exe.parent_path().parent_path() / "share" / "minisim" / "mvp";
        if (std::filesystem::exists(inst / "mvp.c", ec)) return inst.string();
    }
    return std::string(MINISIM_SRC_DIR) + "/../mvp";
}

// minisim's build of UVM's C code (uvm/uvm_dpi_minisim.cc), next to an
// installed msim or in the source tree.
static std::string uvmDpiDir() {
    if (const char* e = std::getenv("MINISIM_UVM")) return e;
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        auto inst = exe.parent_path().parent_path() / "share" / "minisim" / "uvm";
        if (std::filesystem::exists(inst / "uvm_dpi_minisim.cc", ec)) return inst.string();
    }
    return std::string(MINISIM_SRC_DIR) + "/../uvm";
}

static bool hasExt(const std::string& a, std::initializer_list<const char*> exts) {
    for (const char* e : exts)
        if (a.size() > strlen(e) && a.compare(a.size() - strlen(e), strlen(e), e) == 0) return true;
    return false;
}
// Annex J: -sv_lib names a library without its extension.
static std::string svLibPath(const std::string& root, std::string name) {
    if (!hasExt(name, {".so"})) name += ".so";
    if (!root.empty() && !name.empty() && name[0] != '/') name = root + "/" + name;
    return name;
}
// Build C files into one shared library. Returns false on a build error.
static bool buildShared(const std::vector<std::string>& files, const std::string& out, const std::string& cflags,
                        const std::string& ldflags) {
    std::string inc = dpiIncludeDir();
    std::vector<std::string> objs;
    bool cxx = false;
    for (auto& f : files) {
        if (hasExt(f, {".o", ".a"})) { objs.push_back(shq(f)); continue; }
        bool isC = hasExt(f, {".c"});
        cxx |= !isC;
        const char* cc = isC ? (std::getenv("CC") ? std::getenv("CC") : "cc") : (std::getenv("CXX") ? std::getenv("CXX") : "c++");
        std::string obj = out + "." + std::to_string(objs.size()) + ".o";
        std::string cmd = shq(cc) + " -c -fPIC -O2 -I" + shq(inc) + " " + cflags + " " + shq(f) + " -o " + shq(obj);
        if (std::system(cmd.c_str()) != 0) { std::cerr << "msim: DPI: C build failed: " << cmd << "\n"; return false; }
        objs.push_back(shq(obj));
    }
    const char* ld = cxx ? (std::getenv("CXX") ? std::getenv("CXX") : "c++") : (std::getenv("CC") ? std::getenv("CC") : "cc");
    std::string cmd = shq(ld) + " -shared -o " + shq(out);
    for (auto& o : objs) cmd += " " + o;
    cmd += " " + ldflags;
    if (std::system(cmd.c_str()) != 0) { std::cerr << "msim: DPI: link failed: " << cmd << "\n"; return false; }
    return true;
}

// How each process runs: on the static combinational schedule (rank and
// level), as a clocked function (seq), or as an ordinary coroutine.
struct Plan {
    std::vector<int> rank, level;
    std::vector<std::vector<int>> ins;
    std::vector<size_t> body;                       // leading Assigns of a comb process
    std::vector<std::vector<int>> outs;             // comb process outputs
    std::vector<char> seq;
    std::vector<std::vector<std::pair<int, Edge>>> ev;
    bool cyclic = false;   // some comb processes feed each other: no single forward sweep
};
// A wire or variable whose drivers are all strong continuous assigns to
// fixed, disjoint bit ranges that cover it (assign w = ..., or assign w[k]
// = ... in a generate loop, one field of a packed struct per assign)
// resolves to those values side by side. Its assigns then write their bits
// directly, without driver resolution: Ibex's ALU drives a 32-bit net bit
// by bit, and resolving 32 drivers on each of 32 writes was a tenth of its
// run time; the drive of its ~1700 single-driver nets was another quarter.
static void mergePartialDrivers(LowerResult& lowered) {
    if (!S.varPathDrv.empty() || !S.loadsOf.empty() || !lowered.netDelays.empty()) return;
    for (auto& t : S.tranOf) if (!t.empty()) return;
    for (uint8_t b : S.pathIn) if (b) return;
    size_t ns = S.sigs.size();
    std::vector<std::vector<Instr*>> by(ns);
    std::vector<char> bad(ns, 0);
    for (auto& [s, k] : lowered.netKinds)
        if ((size_t)s < ns) bad[(size_t)s] = 1;   // wand, trireg, ...: not set on S yet
    for (auto& lp : lowered.procs)
        for (auto& i : lp.prog.code)
            if (i.k == IK::Force) return;   // release goes back to the drivers (10.6.2)
    for (auto& lp : lowered.procs) {
        auto& code = lp.prog.code;
        // Only a continuous assign: Assign, wait, jump back.
        bool contAssign = code.size() == 3 && code[0].k == IK::Assign && code[1].k == IK::WaitEdge;
        for (auto& i : code) {
            if (i.k != IK::Assign || i.drv < 0 || i.sig < 0 || (size_t)i.sig >= ns) continue;
            bool ok = contAssign && i.dyn < 0 && i.gate < 0 && !i.arr && i.aidx.empty() && !i.bidx && !i.de && !i.delay && i.dly.empty() &&
                      i.s0 == STR_STRONG && i.s1 == STR_STRONG;
            if (ok) by[i.sig].push_back(&i); else bad[i.sig] = 1;
        }
    }
    for (size_t s = 0; s < ns; s++) {
        auto& g = S.sigs[s];
        if (bad[s] || by[s].empty() || by[s].size() != g.drv.size() || g.kind != NR_WIRE) continue;
        if (s < S.tranOf.size() && !S.tranOf[s].empty()) continue;
        std::vector<char> used(g.w, 0);
        bool disjoint = true;
        for (Instr* i : by[s])
            for (int b = i->hi < 0 ? 0 : i->lo, h = i->hi < 0 ? g.w - 1 : i->hi; b <= h && disjoint; b++) {
                if (b < 0 || b >= g.w || used[b]) disjoint = false;
                else used[b] = 1;
            }
        // Every bit has its one driver, so no bit resolves to Z (or keeps
        // a variable's initial value) for want of one.
        for (char u : used) disjoint &= u != 0;
        if (!disjoint) continue;
        for (Instr* i : by[s]) i->drv = -1;
        g.drv.clear();
    }
}

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
            if (lowered.procs[i].program < 0 && !lowered.procs[i].reactive && combShape(lowered.procs[i].prog, c)) { shapes.push_back(c); procOf.push_back(i); }
        }
        std::vector<int> lv;
        std::vector<size_t> order = combSchedule(shapes, &lv, &p.cyclic);
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
            if (p.rank[i] < 0 && lowered.procs[i].program < 0 && !lowered.procs[i].reactive && seqShape(lowered.procs[i].prog, p.ev[i])) p.seq[i] = 1;
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
// simv -cov: the counters of each compiled instruction (hits, then taken
// jumps) and the toggle masks per signal, read back into the instructions and
// S so writeCoverage() writes the same database as an interpreted run.
static bool readCompiledCov(const std::string& path, const std::vector<std::pair<const Prog*, size_t>>& slots) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    auto rd = [&](void* p, size_t n) { return std::fread(p, 8, n, f) == n; };
    uint64_t n = 0, ns = 0;
    bool ok = rd(&n, 1) && n >= 2 * slots.size();
    std::vector<uint64_t> cv(ok ? n : 0);
    ok = ok && rd(cv.data(), n) && rd(&ns, 1) && ns == S.sigs.size();
    for (size_t k = 0; ok && k < slots.size(); k++) {
        Instr& in = const_cast<Instr&>(slots[k].first->code[slots[k].second]);
        in.hits += cv[2 * k];
        in.taken += cv[2 * k + 1];
    }
    if (ok) { S.togRise.assign(ns, {}); S.togFall.assign(ns, {}); }
    for (size_t s = 0; ok && s < ns; s++) {
        uint64_t w = 0;
        ok = rd(&w, 1) && w <= ((uint64_t)S.sigs[s].w + 63) / 64;
        if (!ok) break;
        S.togRise[s].resize(w);
        S.togFall[s].resize(w);
        ok = rd(S.togRise[s].data(), w) && rd(S.togFall[s].data(), w);
    }
    std::fclose(f);
    return ok;
}

static int g_cfiles = 0;   // -cfiles: 0 = automatic
static int runCompiled(const LowerResult& lowered, const std::string& dir, const std::string& cc,
                       const std::string& opt, const std::optional<std::string>& vcdPath, uint64_t maxTime,
                       const Plan& pl, int threads, bool staticPart, bool twoState, bool cycle,
                       uint64_t seed, const std::string& covPath) {
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
    bool combAssert = false;   // a deferred report is queued by one thread
    for (size_t i = 0; i < lowered.procs.size(); i++)
        if (pl.rank[i] >= 0)
            for (auto& in : lowered.procs[i].prog.code) combAssert |= in.k == IK::Assert;
    parts.on = staticPart && !combAssert && !pl.cyclic;
    cycle = cycle && !pl.cyclic;   // the domain evaluates every comb statement once, in rank order
    if (staticPart) partitionInputs(lowered, pl, parts.outs, parts.ins, parts.targets);
    std::string src;
    std::vector<std::pair<const Prog*, size_t>> covSlots;
    // Not compiled yet: final blocks, and a tick other than 1ns (`timescale).
    const char* why = !lowered.finals.empty() ? "a final block"
                    : !lowered.netDelays.empty() ? "a net delay"
                    : !lowered.netKinds.empty() ? "a wand/wor/tri0/tri1/trireg/supply net"
                    : lowered.timescale() != "1ns" ? "a `timescale precision other than 1ns"
                    : std::any_of(asserts().begin(), asserts().end(), [](const AssertInfo& a) {
                          return a.hidden || (a.type & (AT_CONCURRENT | AT_FINAL | AT_EXPECT));
                      }) ? "a concurrent, final or expect assertion"
                    : !clockings().empty() ? "a clocking block"
                    : !cgDefs().empty() ? "a covergroup"
                    : !shadowPairs().empty() || !sampleClocks().empty() ? "a sampled value function" : nullptr;
    if (why) {
        std::cerr << "msim: -compile: " << why << " is not compiled yet; using the interpreter\n";
        return -1;
    }
    try {
        // With a VCD every signal must live in S.sigs, so no private signals.
        // A large design is built as several files at once, one per core (16 at most).
        src = codegen(sigs, procs, parts, twoState, cycle, !vcdPath, g_cov ? &covSlots : nullptr,
                      g_cfiles ? -g_cfiles : std::min(16, maxThreads()));
    } catch (const CodegenUnsupported& ex) {
        std::cerr << "msim: -compile: " << ex.what() << " is not compiled yet; using the interpreter\n";
        return -1;
    }
    std::string inc = kernelIncludeDir();
    if (std::system(("mkdir -p " + shq(dir)).c_str()) != 0) { std::cerr << "msim: cannot create " << dir << "\n"; return 1; }
    std::string cpp = dir + "/design.cpp", exe = dir + "/simv";
    // One build at a time per directory: parallel runs of one test (seeds in
    // mregress) share the build, the first one compiles and the rest reuse it.
    int lockFd = ::open((dir + "/.build.lock").c_str(), O_CREAT | O_RDWR, 0644);
    if (lockFd >= 0) ::flock(lockFd, LOCK_EX);
    struct Unlock { int fd; ~Unlock() { if (fd >= 0) ::close(fd); } } unlock{lockFd};
    std::string build = shq(cc) + " -std=c++20 " + opt + " -I" + shq(inc) + " " + shq(cpp) + " -o " + shq(exe);
    std::string fstC, fstL;   // FST: extra compile and link arguments
    // An FST waveform: build simv with libfst too. Its C sources are compiled
    // once into <dir>/fst/*.o with $CC (default cc).
    if (vcdPath && vcdPath->ends_with(".fst")) {
        std::string fd = fstSourceDir();
        if (fd.empty()) {
            std::cerr << "msim: -compile: no libfst sources for FST output; using the interpreter\n";
            return -1;
        }
        std::string ccC = std::getenv("CC") ? std::getenv("CC") : "cc";
        std::string od = dir + "/fst-par", objs;   // built with the background writer thread
        if (std::system(("mkdir -p " + shq(od)).c_str()) != 0) return 1;
        for (const char* f : {"fstapi", "fastlz", "lz4"}) {
            std::string o = od + "/" + f + ".o";
            objs += " " + shq(o);
            if (std::filesystem::exists(o)) continue;
            std::string c = shq(ccC) + " -O2 -c -D_GNU_SOURCE -DHAVE_FSEEKO -DHAVE_REALPATH"
                            " -DHAVE_LIBPTHREAD -DFST_WRITER_PARALLEL " +
                            shq(fd + "/" + f + ".c") + " -o " + shq(o);
            if (std::system(c.c_str()) != 0) { std::cerr << "msim: -compile: libfst build failed: " << c << "\n"; return 1; }
        }
        build += " -DMINISIM_FST -DMINISIM_FST_PARALLEL -I" + shq(fd) + objs + " -lz -pthread";
        fstC = " -DMINISIM_FST -DMINISIM_FST_PARALLEL -I" + shq(fd);
        fstL = objs + " -lz -pthread";
    }
    // Build cache: the same generated source, compiler command and kernel
    // sources as the simv already in `dir` reuse it. A rebuild of a large
    // design takes minutes; a regression runs the same design many times.
    auto slurp = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    std::string stamp = build + "\n";
    for (const char* k : {"minisim.cpp", "exec.inc", "expr.hpp", "prog.hpp", "cpu.hpp"}) {
        std::error_code ec;
        auto p = std::filesystem::path(inc) / k;
        auto t = std::filesystem::last_write_time(p, ec);
        stamp += std::string(k) + " " + std::to_string(ec ? 0 : std::filesystem::file_size(p, ec)) + " " +
                 std::to_string(ec ? 0 : (long long)t.time_since_epoch().count()) + "\n";
    }
    std::error_code ec;
    // Several files (codegen split a large design): each compiles on its own
    // core, and a file the same as last time keeps its object.
    std::vector<std::pair<std::string, std::string>> files;
    for (size_t at = src.rfind(kFileMark, 0) == 0 ? 0 : std::string::npos; at != std::string::npos;) {
        size_t nl = src.find('\n', at), next = src.find(std::string("\n") + kFileMark, nl);
        std::string name = src.substr(at + std::strlen(kFileMark), nl - at - std::strlen(kFileMark));
        files.push_back({name, src.substr(nl + 1, next == std::string::npos ? std::string::npos : next + 1 - (nl + 1))});
        at = next == std::string::npos ? next : next + 1;
    }
    if (!files.empty()) {
        std::string cflags = " -std=c++20 " + opt + " -I" + shq(inc) + fstC;
        stamp = cflags + fstL + "\n" + stamp;
        bool sameFlags = slurp(dir + "/simv.stamp") == stamp;
        bool hdrSame = slurp(dir + "/design.hpp") == files[0].second;
        std::filesystem::remove(dir + "/simv.stamp", ec);
        std::vector<std::string> jobs, objs;
        for (auto& [name, text] : files) {
            std::string path = dir + "/" + name;
            bool same = slurp(path) == text;
            if (!same) {
                std::ofstream f(path, std::ios::binary);
                f << text;
                if (!f) { std::cerr << "msim: cannot write " << path << "\n"; return 1; }
            }
            if (!name.ends_with(".cpp")) continue;
            std::string obj = path.substr(0, path.size() - 4) + ".o";
            objs.push_back(obj);
            if (same && hdrSame && sameFlags && std::filesystem::exists(obj, ec)) continue;
            std::filesystem::remove(obj, ec);
            jobs.push_back(shq(cc) + cflags + " -c " + shq(path) + " -o " + shq(obj));
        }
        // Remove parts left from an earlier, larger split.
        for (int k = (int)files.size() - 2;; k++) {
            std::string old = dir + "/part" + std::to_string(k);
            if (!std::filesystem::exists(old + ".cpp", ec)) break;
            std::filesystem::remove(old + ".cpp", ec);
            std::filesystem::remove(old + ".o", ec);
        }
        std::atomic<size_t> next{0};
        std::atomic<bool> failed{false};
        std::vector<std::thread> pool;
        for (int t = 0; t < std::min<int>(maxThreads(), (int)jobs.size()); t++)
            pool.emplace_back([&] {
                for (size_t j; (j = next++) < jobs.size();)
                    if (std::system(jobs[j].c_str()) != 0) { failed = true; std::cerr << "msim: -compile: C++ build failed: " << jobs[j] << "\n"; }
            });
        for (auto& t : pool) t.join();
        if (failed) return 1;
        if (!jobs.empty() || !std::filesystem::exists(exe, ec)) {
            std::string link = shq(cc) + " " + opt;
            for (auto& o : objs) link += " " + shq(o);
            link += fstL + " -o " + shq(exe);
            if (std::system(link.c_str()) != 0) { std::cerr << "msim: -compile: link failed: " << link << "\n"; return 1; }
        }
        std::ofstream(dir + "/simv.stamp") << stamp;
    }
    bool fresh = !files.empty() || (std::filesystem::exists(exe, ec) && slurp(dir + "/simv.stamp") == stamp && slurp(cpp) == src);
    if (!fresh) {
        std::filesystem::remove(dir + "/simv.stamp", ec);
        {
            std::ofstream f(cpp);
            f << src;
            if (!f) { std::cerr << "msim: cannot write " << cpp << "\n"; return 1; }
        }
        if (std::system(build.c_str()) != 0) {
            std::cerr << "msim: -compile: C++ build failed: " << build << "\n";
            return 1;
        }
        std::ofstream(dir + "/simv.stamp") << stamp;
    }
    if (lockFd >= 0) ::flock(lockFd, LOCK_UN);
    std::string run = shq(exe);
    if (maxTime != ~0ULL) run += " -maxtime " + std::to_string(maxTime);
    if (vcdPath) run += " -vcd " + shq(*vcdPath) + " -vcdtop " + shq(S.vcdTop);
    run += " -threads " + std::to_string(threads);
    if (g_pin >= 0) run += g_pin ? " -pin" : " -no-pin";
    // Run-time inputs go to simv, so one build serves every seed and plusarg.
    if (seed) run += " -seed " + std::to_string(seed);
    for (auto& a : plusArgs()) run += " " + shq("+" + a);
    std::string covBin = covPath + ".bin";   // per run: parallel runs share the build directory
    if (g_cov) { std::filesystem::remove(covBin); std::filesystem::remove(covBin + ".fcov"); run += " -cov " + shq(covBin); }
    std::cout.flush();
    int st = std::system(run.c_str());
    if (st == -1) return 1;
    if (g_cov && !readCompiledCov(covBin, covSlots)) {
        std::cerr << "msim: -compile: simv wrote no coverage (" << covBin << ")\n";
        return 1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

// Coverage database (msim -cov): JSON with line hits, branch outcomes, and
// toggle masks. mcov merges databases and writes reports.
static std::string jstr(const std::string& x) {
    std::string r = "\"";
    for (char c : x) {
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += c;
    }
    return r + "\"";
}
static void collectProgs(const Prog* p, std::vector<const Prog*>& all, std::unordered_set<const Prog*>& seen) {
    if (!p || !seen.insert(p).second) return;
    all.push_back(p);
    for (auto& i : p->code) {
        if (i.callee) collectProgs(i.callee, all, seen);
        for (auto* b : i.branches) collectProgs(b, all, seen);
    }
}
static bool writeCoverage(const std::string& path, const LowerResult& lw, const std::string& top) {
    std::vector<const Prog*> progs;
    std::unordered_set<const Prog*> seen;
    for (auto& p : lw.procs) collectProgs(&p.prog, progs, seen);
    for (auto& p : lw.finals) collectProgs(&p.prog, progs, seen);
    for (auto& p : lw.subs) collectProgs(p.get(), progs, seen);
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> lines;            // (file, line) -> hits
    std::map<std::pair<uint32_t, uint32_t>, std::vector<std::pair<uint64_t, uint64_t>>> branches;
    for (const Prog* p : progs)
        for (auto& i : p->code) {
            if (!i.line) continue;
            auto key = std::make_pair(i.file, i.line);
            auto& h = lines[key];
            h = std::max(h, i.hits);
            // A JmpIfNot is a two-way decision: true = fell through, false = jumped.
            if (i.k == IK::JmpIfNot) branches[key].push_back({i.hits - i.taken, i.taken});
        }
    std::ofstream f(path);
    if (!f) return false;
    f << "{\n \"format\": \"msim-cov\", \"version\": 1, \"tool\": \"msim " MINISIM_VERSION "\", \"top\": " << jstr(top)
      << ",\n \"files\": [";
    for (size_t k = 0; k < g_covFiles.size(); k++) f << (k ? ", " : "") << jstr(g_covFiles[k]);
    f << "],\n \"lines\": [";
    bool first = true;
    for (auto& [k, h] : lines) {
        f << (first ? "\n  " : ",\n  ") << "[" << k.first << ", " << k.second << ", " << h << "]";
        first = false;
    }
    f << "\n ],\n \"branches\": [";
    first = true;
    for (auto& [k, v] : branches)
        for (size_t n = 0; n < v.size(); n++) {
            f << (first ? "\n  " : ",\n  ") << "[" << k.first << ", " << k.second << ", " << n << ", " << v[n].first << ", " << v[n].second << "]";
            first = false;
        }
    f << "\n ],\n \"toggles\": [";
    first = true;
    auto hex = [](const std::vector<uint64_t>& m, int w) {
        // MSB-first hex of the low w bits.
        std::string r;
        int digits = (w + 3) / 4;
        for (int d = digits - 1; d >= 0; d--) {
            int bit = d * 4;
            uint64_t word = (size_t)bit / 64 < m.size() ? m[(size_t)bit / 64] : 0;
            r += "0123456789abcdef"[(word >> (bit % 64)) & 0xF];
        }
        return r;
    };
    for (size_t sidx = 0; sidx < S.sigs.size(); sidx++) {
        const auto& g = S.sigs[sidx];
        if (g.hide) continue;
        static const std::vector<uint64_t> none;
        const auto& R = sidx < S.togRise.size() ? S.togRise[sidx] : none;
        const auto& F = sidx < S.togFall.size() ? S.togFall[sidx] : none;
        f << (first ? "\n  " : ",\n  ") << "[" << jstr(top + "." + g.name) << ", " << g.w << ", \"" << hex(R, g.w) << "\", \"" << hex(F, g.w) << "\"]";
        first = false;
    }
    f << "\n ],\n";
    // Functional coverage: covergroup bins and assertion statistics. A
    // compiled run's simv wrote them next to its counters.
    std::ifstream fc(path + ".bin.fcov");
    if (fc) f << fc.rdbuf();
    else writeFuncCov(f);
    fc.close();
    std::filesystem::remove(path + ".bin.fcov");
    f << "\n}\n";
    return (bool)f;
}

// The tops msim picks when -top is not given and slang's own choice does
// not cover the design: every config that no other config binds with
// `use ... :config` (33.5.4); failing that, when slang found no top, every
// module or program in a library (33.3) that nothing instantiates.
static std::vector<std::string> implicitTops(slang::driver::Driver& driver, slang::ast::Compilation& comp) {
    using namespace slang::syntax;
    std::vector<std::pair<std::string, std::string>> cfgs;   // (bare name, name for --top)
    std::set<std::string> bound;
    auto scan = [&](const std::shared_ptr<SyntaxTree>& tree) {
        auto visit = [&](const SyntaxNode& m) {
            if (m.kind != SyntaxKind::ConfigDeclaration) return;
            const auto& c = m.as<ConfigDeclarationSyntax>();
            std::string name(c.name.valueText());
            const auto* lib = tree->getSourceLibrary();
            std::string q = lib && !lib->isDefault ? lib->name + "." + name : name;
            cfgs.push_back({name, q + ":config"});
            for (auto* r : c.rules) {
                std::string t = r->toString();
                for (size_t p = t.find(":config"); p != std::string::npos; p = t.find(":config", p + 1)) {
                    size_t e = p;
                    while (e > 0 && std::isspace((unsigned char)t[e - 1])) e--;
                    size_t b = e;
                    while (b > 0 && (std::isalnum((unsigned char)t[b - 1]) || t[b - 1] == '_' || t[b - 1] == '$')) b--;
                    bound.insert(t.substr(b, e - b));
                }
            }
        };
        const SyntaxNode& root = tree->root();
        if (root.kind == SyntaxKind::CompilationUnit)
            for (auto* m : root.as<CompilationUnitSyntax>().members) visit(*m);
        else if (root.kind == SyntaxKind::LibraryMap)
            for (auto* m : root.as<LibraryMapSyntax>().members) visit(*m);
    };
    for (auto& t : driver.syntaxTrees) scan(t);
    for (auto& t : driver.sourceLoader.getLibraryMaps()) scan(t);
    std::vector<std::string> tops;
    for (auto& [name, q] : cfgs)
        if (!bound.count(name)) tops.push_back(q);
    if (tops.size() > 1) {
        std::cerr << "msim: several configs could be the top:";
        for (auto& t : tops) std::cerr << " " << t.substr(0, t.size() - 7);
        std::cerr << "; choose one with -top <config>\n";
        std::exit(2);
    }
    if (!tops.empty() || !comp.getRoot().topInstances.empty()) return tops;
    struct Inst : SyntaxVisitor<Inst> {
        std::set<std::string> names;
        void handle(const HierarchyInstantiationSyntax& h) {
            names.insert(std::string(h.type.valueText()));
            visitDefault(h);
        }
    } inst;
    for (auto& t : driver.syntaxTrees) t->root().visit(inst);
    std::set<std::string> seen;
    for (const auto* sym : comp.getDefinitions()) {
        using slang::ast::DefinitionKind;
        if (sym->kind != slang::ast::SymbolKind::Definition) continue;
        const auto& def = sym->as<slang::ast::DefinitionSymbol>();
        if (def.sourceLibrary.isDefault || inst.names.count(std::string(def.name))) continue;
        if (def.definitionKind != DefinitionKind::Module && def.definitionKind != DefinitionKind::Program) continue;
        if (!seen.insert(std::string(def.name)).second) continue;   // the first library that has it (33.6.1)
        tops.push_back(def.sourceLibrary.name + "." + std::string(def.name));
    }
    return tops;
}

static void usage() {
    std::cerr << "usage: msim [options] <files...>\n"
                 "  -top <name>    top-level module (repeatable)\n"
                 "  -vcd [<file>]  write a VCD waveform (default dump.vcd); a .fst name writes FST\n"
                 "  -saif [<file>] write switching activity as SAIF (default dump.saif), for mpower\n"
                 "  +notimingcheck turn off timing checks ($setup, $hold, ...)\n"
                 "  -sdf [min|typ|max:]<inst>:<file>  annotate an SDF file before time 0\n"
                 "  +mindelays / +typdelays / +maxdelays  pick from min:typ:max delays\n"
                 "  -libmap <file>  library map (33.3); -L <lib> library search order\n"
                 "  +neg_tchk      allow negative $setuphold / $recrem limits, with delayed signals\n"
                 "  +pulse_r/<n> +pulse_e/<n>  pulse reject / error limits as n% of path delays\n"
                 "  +pulse_on_event / +pulse_on_detect, +showcancelled / +noshowcancelled  pulse styles\n"
                 "  -fst [<file>]  write an FST waveform (default dump.fst)\n"
                 "  -cov [<file>]  write line / branch / toggle coverage (default cov.json); see mcov\n"
                 "  -maxtime <n>   stop after simulation time n\n"
                 "  -compile       generate C++, build it (<dir>/simv) and run it; works with -cov,\n"
                 "                 and reuses <dir>/simv while the design is unchanged\n"
                 "  -o <dir>       build directory for -compile (default msim.d)\n"
                 "  -cc <cxx>      C++ compiler for -compile (default $CXX, else c++)\n"
                 "  -cfiles <n>    -compile: build in up to n C++ files at once (default: one per core, 16 at most)\n"
                 "  -threads <n>   worker threads, or all (default: one per physical P core, max 8)\n"
                 "  -pin, -no-pin  pin worker threads to cores (default: yes, not under a VM)\n"
                 "  -cpuinfo       print the detected cores / SMT / P+E topology\n"
              << protfront::Options::help() <<
                 "  -xprop[=tmerge|xmerge]  X-propagation: an unknown if / case condition or clock edge\n"
                 "                 merges the branches instead of picking one (docs/XPROP.md)\n"
                 "  -xprop-report <file>  list every if / case / edge and whether it is merged\n"
                 "  -upf <file>    power-aware simulation with IEEE 1801 (UPF) power intent\n"
                 "  -power_top <i> the instance the UPF describes (default: set_design_top, else the top)\n"
                 "  -upf_verbose   print supply, power state and simstate changes\n"
                 "  <file>.c       C or C++ for DPI-C imports; also -sv_lib / -sv_root / -dpiheader\n"
                 "  -mvp           add the virtual prototype layer (mvp_axil_slave and its runtime)\n"
                 "  -uvm           compile the Accellera UVM library from $UVM_HOME, with its C code\n"
                 "                 (-uvmhome <dir>: from <dir>; -uvmnodpi: without C, +define+UVM_NO_DPI)\n"
                 "  -load <lib>    load a VPI library before elaboration\n"
                 "  +define+N[=V]  +incdir+<dir>  -f <file>  -I <dir>  -D N[=V]\n"
                 "  -version       print the msim version\n"
                 "  --help-slang   list every front-end option\n";
}

int main(int argc, char** argv) {
    std::atexit([] { g_framesDead = true; });   // before the kernel's waiting processes are destroyed
    for (int sg : {SIGSEGV, SIGABRT})
        std::signal(sg, [](int) {   // say where, then stop
            std::string w = "internal error: crash" + runLocStr() + "\n";
            (void)!write(2, w.data(), w.size());
            _exit(139);
        });
    // Pass 1: take msim's own options out; translate VCS spellings for slang.
    std::vector<std::string> fwd{argv[0]};
    std::vector<std::string> plusargs;
    uint64_t seed = 0;
    std::optional<std::string> vcdPath;
    std::string saifPath;   // -saif: the waveform is summed into a SAIF file after the run
    bool saifTmp = false;   // ...and the waveform was only written for that
    std::string covPath = "cov.json";
    uint64_t maxTime = ~0ULL;
    bool compiled = false, staticComb = true, staticSeq = true, staticPart = true, twoState = true, cycle = true;
    int threads = defaultThreads();
    bool threadsGiven = false;
    std::string buildDir = "msim.d", opt = "-O2";
    std::string cc = std::getenv("CXX") ? std::getenv("CXX") : "c++";
    protfront::Options prot;
    std::string upfPath, powerTop;
    std::string xpropReport;
    bool powerTopSet = false, upfVerbose = false;
    std::vector<std::string> svLibs, cFiles, soFiles;
    std::vector<std::pair<std::string, std::string>> vpiLibs;   // -load lib:boot
    std::string svRoot, dpiHeader, cflags, ldflags;
    std::string uvmHome;    // -uvm / -uvmhome: a uvm-core checkout
    bool uvmWanted = false, uvmNoDpi = false;
    bool buildDirGiven = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << "msim: " << a << " needs " << what << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-version" || a == "--version" || a == "-v") { version(); return 0; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        if (a == "-compile") { compiled = true; continue; }
        if (a == "-no-static-comb") { staticComb = false; continue; }
        if (a == "-no-static-seq") { staticSeq = false; continue; }
        if (a == "-no-partition") { staticPart = false; continue; }
        if (a == "-no-2state") { twoState = false; continue; }
        if (a == "-no-cycle") { cycle = false; continue; }
        if (a == "-xprop" || a == "-xprop=tmerge") { Lowerer::xpropMode() = 1; continue; }
        if (a == "-xprop=xmerge") { Lowerer::xpropMode() = 2; continue; }
        if (a == "-xprop=off" || a == "-xprop=vmerge") { Lowerer::xpropMode() = 0; continue; }
        if (a.rfind("-xprop=", 0) == 0) { std::cerr << "msim: bad " << a << " (tmerge, xmerge or off)\n"; return 2; }
        if (a == "-xprop-report") { xpropReport = next("a file name"); continue; }
        if (a == "-no-pin") { g_pin = 0; continue; }
        if (a == "-pin") { g_pin = 1; continue; }
        if (a == "-cpuinfo") {
            const CpuTopo& t = cpuTopo();
            std::cout << t.describe() << "\ndefault -threads " << defaultThreads() << "\nplacement:";
            for (int c : t.order) std::cout << ' ' << c;
            std::cout << "\n";
            return 0;
        }
        if (a == "-threads") {
            threadsGiven = true;
            std::string n = next("a thread count");
            if (n == "all") { threads = maxThreads(); continue; }
            try { threads = std::stoi(n); }
            catch (...) { std::cerr << "msim: bad -threads " << n << "\n"; return 2; }
            if (threads < 1) { std::cerr << "msim: -threads must be at least 1\n"; return 2; }
            // More threads than hardware threads only adds waiting: a batch
            // cannot finish until a descheduled worker runs again.
            int hw = maxThreads();
            if (hw > 0 && threads > hw) {
                std::cerr << "msim: -threads " << threads << " is more than the " << hw
                          << " hardware threads; using " << hw << "\n";
                threads = hw;
            }
            continue;
        }
        if (a == "-o") { buildDir = next("a directory"); buildDirGiven = true; continue; }
        if (a == "-upf") { upfPath = next("a UPF file"); continue; }
        if (a == "-power_top") { powerTop = next("an instance path"); powerTopSet = true; continue; }
        if (a == "-upf_verbose" || a == "+upf_verbose") { upfVerbose = true; continue; }
        if (a == "-cc") { cc = next("a compiler"); continue; }
        if (a == "-cfiles") { g_cfiles = std::max(1, std::atoi(next("a file count").c_str())); continue; }
        if (a.size() == 3 && a[0] == '-' && a[1] == 'O' && std::isdigit((unsigned char)a[2])) { opt = a; continue; }
        if (prot.take(argc, argv, i)) continue;
        if (a == "+vcd") { vcdPath = "dump.vcd"; continue; }
        if (a == "+notimingcheck" || a == "+notimingchecks") { timingChecksOn() = false; continue; }
        if (a == "+mindelays" || a == "+typdelays" || a == "+maxdelays") {
            mtmSelect() = a[1] == 'm' && a[2] == 'i' ? 0 : a[1] == 't' ? 1 : 2;
            fwd.push_back("-T");
            fwd.push_back(mtmSelect() == 0 ? "min" : mtmSelect() == 1 ? "typ" : "max");
            continue;
        }
        if (a == "-sdf") {   // VCS: -sdf min|typ|max:instance:file
            std::string v = next("[min|typ|max:]instance:file");
            SdfRequest r;
            r.where = "-sdf " + v + ": ";
            r.cli = true;
            auto c1 = v.find(':');
            if (c1 != std::string::npos) {
                std::string m = v.substr(0, c1);
                if (m == "min" || m == "typ" || m == "max") {
                    r.mtm = m == "min" ? "MINIMUM" : m == "typ" ? "TYPICAL" : "MAXIMUM";
                    v = v.substr(c1 + 1);
                }
            }
            auto c2 = v.find(':');
            if (c2 == std::string::npos) { std::cerr << "msim: -sdf needs instance:file\n"; return 2; }
            r.scope = v.substr(0, c2);
            r.file = v.substr(c2 + 1);
            sdfRequests().push_back(r);
            continue;
        }
        if (a == "-libmap") { fwd.push_back("--libmap"); fwd.push_back(next("a library map file")); continue; }
        if (a == "+neg_tchk") { negTimingChecks() = true; continue; }
        if (a.rfind("+pulse_r/", 0) == 0 || a.rfind("+pulse_e/", 0) == 0) {   // 30.7.2
            std::string n = a.substr(9);
            char* end = nullptr;
            long p = std::strtol(n.c_str(), &end, 10);
            if (n.empty() || *end || p < 0 || p > 100) { std::cerr << "msim: " << a << ": the percentage is an integer from 0 to 100\n"; return 2; }
            (a[7] == 'r' ? pulseOpts().rejectPct : pulseOpts().errorPct) = (int)p;
            continue;
        }
        if (a == "+pulse_on_event" || a == "+pulse_on_detect") { pulseOpts().detect = a == "+pulse_on_detect"; continue; }   // 30.7.4.1
        if (a == "+showcancelled" || a == "+show_cancelled") { pulseOpts().showCancelled = 1; continue; }                   // 30.7.4.2
        if (a == "+noshowcancelled" || a == "+noshow_cancelled") { pulseOpts().showCancelled = 0; continue; }
        if (a == "+fst") { vcdPath = "dump.fst"; continue; }
        // Switching activity for power analysis (mpower): SAIF, T0/T1/TX/TC per bit.
        if (a == "-saif") {
            std::string_view nx = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();
            saifPath = nx.ends_with(".saif") ? argv[++i] : "dump.saif";
            continue;
        }
        if (a == "-cov") {
            g_cov = true;
            if (i + 1 < argc && std::string_view(argv[i + 1]).ends_with(".json")) covPath = argv[++i];
            continue;
        }
        // -vcd x.fst writes FST too: the file name picks the format.
        if (a == "-vcd" || a == "-fst") {
            std::string_view nx = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();
            if (nx.ends_with(".vcd") || nx.ends_with(".fst")) vcdPath = argv[++i];
            else vcdPath = a == "-fst" ? "dump.fst" : "dump.vcd";
            continue;
        }
        if (a == "-maxtime") {
            std::string n = next("a time");
            try { maxTime = std::stoull(n); }
            catch (...) { std::cerr << "msim: bad -maxtime " << n << "\n"; return 2; }
            continue;
        }
        if (a == "-seed") {
            std::string n = next("a seed");
            try { seed = std::stoull(n); }
            catch (...) { std::cerr << "msim: bad -seed " << n << "\n"; return 2; }
            continue;
        }
        if (startsWith(a, "+ntb_random_seed=")) {
            try { seed = std::stoull(a.substr(17)); }
            catch (...) { std::cerr << "msim: bad " << a << "\n"; return 2; }
            continue;
        }
        if (a.size() > 1 && a[0] == '+' && !slangPlus(a)) { plusargs.push_back(a.substr(1)); continue; }
        if (a == "-sv_lib") { svLibs.push_back(next("a library name")); continue; }
        if (a == "-sv_root") { svRoot = next("a directory"); continue; }
        if (a == "-sv_liblist") {   // J.4: "#!SV_LIBRARIES" then one library per line
            std::string f = next("a bootstrap file");
            std::ifstream in(f);
            if (!in) { std::cerr << "msim: cannot read " << f << "\n"; return 2; }
            std::string line;
            while (std::getline(in, line)) {
                size_t b = line.find_first_not_of(" \t\r"), e = line.find_last_not_of(" \t\r");
                if (b == std::string::npos || line[b] == '#') continue;
                svLibs.push_back(line.substr(b, e - b + 1));
            }
            continue;
        }
        if (a == "-dpiheader") { dpiHeader = next("a file name"); continue; }
        if (a == "-load" || a == "-loadvpi") {   // VPI (36.9.1)
            std::string l = next("a VPI library");
            size_t c = l.rfind(':');
            std::string lib = c == std::string::npos ? l : l.substr(0, c), boot = c == std::string::npos ? "" : l.substr(c + 1);
            if (lib.find('/') == std::string::npos) lib = "./" + lib;
            vpiLibs.push_back({lib, boot});
            continue;
        }
        if (a == "-mvp") {   // the virtual prototype layer: mvp.sv and its runtime
            std::string d = mvpDir();
            std::error_code ec;
            if (!std::filesystem::exists(d + "/mvp.c", ec)) {
                std::cerr << "msim: -mvp cannot find the mvp runtime in " << d
                          << " (set $MINISIM_MVP)\n";
                return 2;
            }
            fwd.push_back(d + "/sv/mvp.sv");
            cFiles.push_back(d + "/mvp.c");
            continue;
        }
        if (a == "-uvm") { uvmHome = uvmHome.empty() && std::getenv("UVM_HOME") ? std::getenv("UVM_HOME") : uvmHome; uvmWanted = true; continue; }
        if (a == "-uvmhome") { uvmHome = next("a uvm-core directory"); uvmWanted = true; continue; }
        if (a == "-uvmnodpi") { uvmNoDpi = uvmWanted = true; continue; }
        if (a == "-CFLAGS") { cflags += " " + next("flags"); continue; }
        if (a == "-LDFLAGS") { ldflags += " " + next("flags"); continue; }
        if (a.size() && a[0] != '-' && hasExt(a, {".c", ".cc", ".cpp", ".cxx", ".C", ".o", ".a"})) { cFiles.push_back(a); continue; }
        if (a.size() && a[0] != '-' && hasExt(a, {".so"})) { soFiles.push_back(a); continue; }
        fwd.push_back(a);
    }
    if (uvmWanted) {   // -uvm: the package first, then its C code (regex, command line, HDL backdoor)
        if (uvmHome.empty() && std::getenv("UVM_HOME")) uvmHome = std::getenv("UVM_HOME");
        if (uvmHome.empty()) { std::cerr << "msim: -uvm needs $UVM_HOME or -uvmhome <dir>\n"; return 2; }
        std::string uvmSrc = uvmHome + "/src";
        std::error_code ec0;
        if (!std::filesystem::exists(uvmSrc + "/uvm_pkg.sv", ec0)) {
            std::cerr << "msim: no " << uvmSrc << "/uvm_pkg.sv (is " << uvmHome << " a uvm-core checkout?)\n";
            return 2;
        }
        fwd.insert(fwd.begin() + 1, {"+incdir+" + uvmSrc, uvmSrc + "/uvm_pkg.sv"});
        if (uvmNoDpi) fwd.push_back("+define+UVM_NO_DPI");
        else {
            std::string d = uvmDpiDir();
            std::error_code ec;
            if (!std::filesystem::exists(d + "/uvm_dpi_minisim.cc", ec)) {
                std::cerr << "msim: -uvm cannot find uvm_dpi_minisim.cc in " << d << " (set $MINISIM_UVM, or use -uvmnodpi)\n";
                return 2;
            }
            cFiles.push_back(d + "/uvm_dpi_minisim.cc");
            cflags += " -I" + shq(uvmSrc + "/dpi") + " -w";
        }
    }
    for (auto& l : svLibs) soFiles.push_back(svLibPath(svRoot, l));
    plusArgs() = plusargs;
    // -upf: package UPF (IEEE 1801-2024 11.2.4) goes in as one more source.
    std::string upfPkgFile;
    if (!upfPath.empty()) {
        char tmpl[] = "/tmp/msim_upf_pkgXXXXXX.sv";
        int fd = mkstemps(tmpl, 3);
        if (fd < 0) { std::cerr << "msim: cannot write the UPF package\n"; return 1; }
        std::string pkg = upf::svPackage();
        if (write(fd, pkg.data(), pkg.size()) != (ssize_t)pkg.size()) { std::cerr << "msim: cannot write the UPF package\n"; return 1; }
        close(fd);
        upfPkgFile = tmpl;
        fwd.insert(fwd.begin() + 1, upfPkgFile);
    }
    struct RmPkg { std::string f; ~RmPkg() { if (!f.empty()) std::remove(f.c_str()); } } rmPkg{upfPkgFile};
    // The language is IEEE 1800-2023 unless --std says otherwise (slang's
    // default is 1800-2017, which rejects e.g. weak_reference, 8.30).
    if (!saifPath.empty() && !vcdPath) {
#ifdef MINISIM_FST
        vcdPath = saifPath + ".tmp.fst";
#else
        vcdPath = saifPath + ".tmp.vcd";
#endif
        saifTmp = true;
    }
    // After the run: every value change of the waveform, summed per bit.
    auto writeSaif = [&]() -> bool {
        if (saifPath.empty()) return true;
        S.closeWaves();
        try {
            saif::Db db;
            saif::load(*vcdPath, db, par::defaultThreads());
            std::ofstream f(saifPath);
            if (!f) throw saif::Error("cannot write " + saifPath);
            saif::write(db, f, S.vcdTop, "msim", MINISIM_VERSION);
        } catch (const std::exception& e) {
            std::cerr << "msim: -saif: " << e.what() << "\n";
            return false;
        }
        if (saifTmp) std::remove(vcdPath->c_str());
        return true;
    };
    if (std::none_of(fwd.begin(), fwd.end(), [](const std::string& s) { return startsWith(s, "--std"); }))
        fwd.push_back("--std=1800-2023");
    if (seed) {   // the $urandom stream (18.13); $random keeps its own seed (Annex N)
        uint64_t st = (seed + 1) * 0x9E3779B97F4A7C15ull;
        urandomState() = st ? st : 1;
    }

    // Pass 2: slang parses, elaborates and reports diagnostics.
    slang::driver::Driver driver;
    driver.addStandardArgs();
    std::vector<const char*> cargv;
    for (auto& s : fwd) cargv.push_back(s.c_str());
    if (!driver.parseCommandLine((int)cargv.size(), cargv.data())) return 2;
    for (auto& s : fwd) if (s == "--help") { std::cout << driver.cmdLine.getHelpText("msim front end") << "\n"; return 0; }
    // Like VCS and Xcelium, a name may be used before its declaration
    // (--allow-use-before-declare=false turns this off).
    if (auto& f = driver.options.compilationFlags[slang::ast::CompilationFlags::AllowUseBeforeDeclare]; !f)
        f = true;
    if (!driver.processOptions()) return 2;
    // 33.3.1.1: two cells with one name in one library are a warning, and
    // the last one encountered is the one used (slang keeps the later
    // definition first in its list). -Werror=duplicate-definition keeps
    // the error.
    {
        bool userSet = false;
        for (auto& s : fwd) userSet |= s.find("duplicate-definition") != std::string::npos;
        if (!userSet) driver.diagEngine.setSeverity(slang::diag::DuplicateDefinition, slang::DiagnosticSeverity::Warning);
    }

    // Protected envelopes (34): decrypt before slang reads the files, and
    // route diagnostics through the redacting client.
    protfront::ProtectedCode pc;
    if (!protfront::preload(driver, prot, pc, "msim")) return 1;
    if (pc.any()) {
        protfront::redact(driver, pc);
        hideSigIf() = [&pc](const slang::ast::Symbol& s) { return !pc.visible(s); };
        hideLocIf() = [&pc](slang::SourceLocation l) { return !pc.coverable(l); };
        hideMsgIf() = [&pc](slang::SourceLocation l) { return pc.hidden(l); };
    }
    if (!driver.parseAllSources()) return 1;
    // A design element with no `timescale or timeunit, in a design where
    // others have one, gets 1ns/1ps instead of an error (--timescale sets
    // another default). A design with no timescale at all keeps 1ns/1ns.
    if (!driver.options.timeScale) {
        for (auto b : driver.sourceManager.getAllBuffers()) {
            std::string_view t = driver.sourceManager.getSourceText(b);
            if (t.find("`timescale") != t.npos || t.find("timeunit") != t.npos ||
                t.find("timeprecision") != t.npos) {
                driver.options.timeScale = "1ns/1ps";
                break;
            }
        }
    }

    // VPI (36.9.1): libraries load before elaboration, so the system tasks
    // and functions they register are known to it. The C files are built now
    // for that, unless -dpiheader asks for prototypes they may include.
    std::string userSo, earlyDir;
    bool earlyTmp = false;
    if (!cFiles.empty() && dpiHeader.empty()) {
        earlyDir = buildDir;
        if (!buildDirGiven) {
            std::string t = (std::filesystem::temp_directory_path() / "msim-dpi-XXXXXX").string();
            if (!mkdtemp(t.data())) { std::cerr << "msim: cannot create a temporary directory\n"; return 1; }
            earlyDir = t;
            earlyTmp = true;
        }
        if (std::system(("mkdir -p " + shq(earlyDir)).c_str()) != 0) {
            std::cerr << "msim: cannot create " << earlyDir << "\n";
            return 1;
        }
        userSo = std::filesystem::absolute(earlyDir + "/dpi_user.so").string();
        if (!buildShared(cFiles, userSo, cflags, ldflags)) return 1;
    }
    vpi::st().argv.assign(argv, argv + argc);
    {
        std::vector<std::pair<std::string, std::string>> all = vpiLibs;
        if (!userSo.empty()) all.push_back({userSo, ""});
        for (auto& s : soFiles) all.push_back({s.find('/') == std::string::npos ? "./" + s : s, ""});
        for (auto& [lib, boot] : all) {
            std::string err;
            if (!vpi::load(lib, boot, err)) { std::cerr << "msim: VPI: " << err << "\n"; return 1; }
        }
    }

    auto comp = driver.createCompilation();
    // Without -top: a config on the command line names the top (33.5.4),
    // and with library maps a module in any library that nothing
    // instantiates is a top (23.3.1, 33.6.1); slang only looks in work.
    if (driver.options.topModules.empty()) {
        std::vector<std::string> tops = implicitTops(driver, *comp);
        if (!tops.empty()) {
            driver.options.topModules = tops;
            comp = driver.createCompilation();
        }
    }
    if (vpi::st().active) vpi::declare(*comp);
    driver.reportCompilation(*comp, /*quiet*/ true);
    if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;

    // The VCD's root scope is the top module (several tops: "top").
    if (auto tops = comp->getRoot().topInstances; tops.size() == 1) S.vcdTop = std::string(tops[0]->name);

    // Pass 3: lower to the process IR and run on the kernel.
    LowerResult lowered;
    try {
        lowered = lower(*comp,
                        [](const std::string& n, int w, bool net) { return S.sig(n, w, net); },
                        [](int s) { return S.newDriver(s); });
    } catch (const std::exception& ex) {
        // The message may name something declared in protected code.
        if (pc.any()) std::cerr << "msim: the design (which has protected code) uses a construct msim cannot simulate yet; details hidden\n";
        else std::cerr << "msim: " << ex.what() << "\n";
        return 1;
    }
    timeFmt().tickExp = timeFmt().units = lowered.precExp;   // %t defaults (20.4.3)
    if (!xpropReport.empty()) {   // -xprop-report: what is merged, what is not and why
        std::ofstream f(xpropReport);
        if (!f) { std::cerr << "msim: cannot write " << xpropReport << "\n"; return 1; }
        size_t ok = 0;
        for (auto& r : Lowerer::xpReport()) ok += r.ok;
        f << "xprop " << (Lowerer::xpropMode() == 2 ? "xmerge" : Lowerer::xpropMode() == 1 ? "tmerge" : "off") << ": "
          << ok << " of " << Lowerer::xpReport().size() << " if / case / edge constructs merged\n";
        for (auto& r : Lowerer::xpReport())
            f << r.where << r.what << (r.ok ? ": merged" : ": not merged, it holds " + r.why) << "\n";
    }
    if (!upfPath.empty()) {   // power-aware simulation (IEEE 1801-2024 9)
        auto& rt = upfrt::rt();
        rt.verbose = upfVerbose;
        try {
            rt.m.load(upfPath, "");
            std::string top = powerTopSet ? powerTop : rt.m.designTop;
            for (char& c : top) if (c == '/') c = '.';
            while (!top.empty() && top[0] == '.') top.erase(0, 1);
            // a path may start with the simulation top's own name (tb.u_dut)
            if (!S.vcdTop.empty() && (top == S.vcdTop || top.starts_with(S.vcdTop + "."))) top = top.substr(std::min(top.size(), S.vcdTop.size() + 1));
            // set_design_top may name the module the UPF was written for
            if (!powerTopSet && !top.empty() && top.find('.') == std::string::npos) {
                std::string found;
                std::function<void(const slang::ast::InstanceSymbol&, const std::string&)> walk =
                    [&](const slang::ast::InstanceSymbol& in, const std::string& path) {
                        if (!found.empty()) return;
                        if (in.getDefinition().name == top && !path.empty()) { found = path; return; }
                        for (auto& m : in.body.members())
                            if (m.kind == slang::ast::SymbolKind::Instance) {
                                auto& ch = m.as<slang::ast::InstanceSymbol>();
                                walk(ch, path.empty() ? std::string(ch.name) : path + "." + std::string(ch.name));
                            }
                    };
                for (auto* t : comp->getRoot().topInstances) {
                    if (t->getDefinition().name == top) { found = "."; break; }
                    walk(*t, "");
                }
                if (found == ".") top.clear();
                else if (!found.empty()) top = found;
            }
            rt.m.top = top;
            rt.bind(lowered);
        } catch (const std::exception& ex) {
            std::cerr << "msim: UPF: " << ex.what() << "\n";
            return 1;
        }
        // Power gating needs every process on the interpreter, one thread.
        staticComb = staticSeq = staticPart = false;
        threads = 1;
        if (compiled) { std::cerr << "msim: -compile: power-aware simulation is not compiled yet; using the interpreter\n"; compiled = false; }
    }
    if (usesForce()) staticPart = false;   // a forced signal must see every write (10.6)
    if (lowered.procs.empty()) {
        // Nothing to schedule: the simulation ends at time 0 (4.4.1).
        std::cerr << "msim: note: design has no processes; simulation ends at time 0\n";
        return 0;
    }

    for (int id : hiddenSigs()) S.sigs[(size_t)id].hide = true;

    // DPI-C (35): write the header, build the C code and the export stubs,
    // load the libraries, and resolve the imports.
    if (!lowered.dpi.empty() || !cFiles.empty() || !soFiles.empty()) {
        dpi::collectExports(lowered);
        if (!dpiHeader.empty()) {
            std::ofstream h(dpiHeader);
            h << dpi::header(lowered.dpi);
            if (!h) { std::cerr << "msim: cannot write " << dpiHeader << "\n"; return 1; }
        }
        std::vector<std::string> libs;
        bool needDir = !dpi::st().exports.empty() || !cFiles.empty();
        // Without -o the C build goes to a private temporary directory, so
        // runs in the same directory do not share (or race on) one library.
        std::string dpiDir = buildDir;
        bool tmpDir = false;
        if (needDir && !buildDirGiven) {
            std::string t = (std::filesystem::temp_directory_path() / "msim-dpi-XXXXXX").string();
            if (!mkdtemp(t.data())) { std::cerr << "msim: cannot create a temporary directory\n"; return 1; }
            dpiDir = t;
            tmpDir = true;
        }
        const std::string& buildDir = dpiDir;
        if (needDir && std::system(("mkdir -p " + shq(buildDir)).c_str()) != 0) {
            std::cerr << "msim: cannot create " << buildDir << "\n";
            return 1;
        }
        if (!dpi::st().exports.empty()) {   // C calls these, so they load first
            std::string c = buildDir + "/dpi_exports.c", so = buildDir + "/dpi_exports.so";
            std::ofstream f(c);
            f << dpi::exportStubs(dpi::st().exports);
            f.close();
            if (!buildShared({c}, so, "", "")) return 1;
            libs.push_back(std::filesystem::absolute(so).string());
        }
        if (!userSo.empty()) {
            libs.push_back(userSo);
        } else if (!cFiles.empty()) {
            std::string so = buildDir + "/dpi_user.so";
            if (!buildShared(cFiles, so, cflags, ldflags)) return 1;
            libs.push_back(std::filesystem::absolute(so).string());
        }
        for (auto& s : soFiles) libs.push_back(s.find('/') == std::string::npos ? "./" + s : s);
        std::string err;
        bool ok = dpi::setup(lowered, libs, err);
        if (tmpDir) { std::error_code ec; std::filesystem::remove_all(dpiDir, ec); }   // the libraries stay mapped
        if (!ok) { std::cerr << "msim: DPI: " << err << "\n"; return 1; }
        compiled = false;   // -compile does not run DPI calls yet
    }
    if (compiled && (!pathRecs().empty() || !sdfRequests().empty() || sdfAtRunTime())) {
        std::cerr << "msim: -compile: specify path delays and SDF annotation are not compiled yet; using the interpreter\n";
        compiled = false;
    }
    if (earlyTmp) { std::error_code ec; std::filesystem::remove_all(earlyDir, ec); }   // the library stays mapped

    // A context import may call VPI (35.5.3), as UVM's HDL backdoor does:
    // VPI's object model is then set up as for a VPI library.
    for (const auto& f : lowered.dpi)
        if (f->context) vpi::st().active = true;
    // VPI: callbacks and calltf routines may touch any signal at any time,
    // so the design runs on one thread in the interpreter, every process
    // scheduled the plain way.
    if (vpi::st().active) {
        if (compiled) std::cerr << "msim: -compile: VPI is not compiled yet; using the interpreter\n";
        compiled = false;
        staticComb = staticSeq = staticPart = false;
        threads = 1;
    }
    // SDF DEVICE may give gates new delays (32.4.1): their drivers then
    // run the plain way.
    if ((!sdfRequests().empty() || sdfAtRunTime()) && !gateRecs().empty()) staticComb = staticSeq = false;
    // Paths, SDF and gate delays (30-32) act on drivers; tran and net kinds
    // resolve them: those designs keep every driver.
    if (staticComb && !vpi::st().active && !g_upf && pathRecs().empty() && sdfRequests().empty() && !sdfAtRunTime() &&
        gateRecs().empty())
        mergePartialDrivers(lowered);   // -no-static-comb keeps them, as the reference
    Plan pl = plan(lowered, staticComb, staticSeq);
    // Generated C++ would hold the protected design in the clear.
    if (compiled && pc.any()) {
        std::cerr << "msim: -compile: the design has protected code; using the interpreter\n";
        compiled = false;
    }
    if (compiled && timingChecksOn() && !timingChecks().empty()) {
        std::cerr << "msim: -compile: timing checks are not compiled yet; using the interpreter\n";
        compiled = false;
    }
    bool anyReactive = false;
    for (auto& lp : lowered.procs) anyReactive |= lp.reactive;
    if (compiled && (lowered.programs || anyReactive)) {
        std::cerr << "msim: -compile: programs and checkers are not compiled yet; using the interpreter\n";
        compiled = false;
    }
    if (compiled) {
        int st = runCompiled(lowered, buildDir, cc, opt, vcdPath, maxTime, pl, threads, staticPart, twoState, cycle,
                             seed, covPath);
        if (st >= 0) {
            if (g_cov && !writeCoverage(covPath, lowered, S.vcdTop)) {
                std::cerr << "msim: cannot write " << covPath << "\n";
                return 1;
            }
            if (!writeSaif()) return 1;
            return st;
        }
    }

    S.vcdPath = vcdPath.value_or("");
    S.timescale = lowered.timescale();
    for (auto& [s, d] : lowered.netDelays) S.netDelay[s] = d;
    for (auto& t : lowered.trans) S.addTran(t.a, t.b, t.resist);
    for (auto& [s, k] : lowered.netKinds) {   // tri0 starts at 0, supply1 at 1, trireg at x (6.6)
        auto& g = S.sigs[s];
        g.kind = k;
        g.val = resolveNet(k, g.drv.data(), g.drv.size(), g.w, allx(g.w));
    }
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
    // An FST file is compressed on libfst's own writer thread: leave it a
    // core, unless -threads was given.
    if (!threadsGiven && threads > 1 && vcdPath && vcdPath->ends_with(".fst")) threads--;
    S.threads = threads;
    std::vector<int> byRank(lowered.procs.size(), -1);
    for (size_t i = 0; i < pl.rank.size(); i++)
        if (pl.rank[i] >= 0) byRank[pl.rank[i]] = (int)i;
    for (int i : byRank) {
        if (i < 0) continue;
        const Prog* pr = &lowered.procs[i].prog;
        for (auto& in : pr->code)
            if (in.k == IK::Assert && !pr->combCtl) pr->combCtl = std::make_shared<ProcCtl>();
        if (pl.body[i] == 1 && pr->code[0].k == IK::Assign) S.addComb(execComb, pr, pl.ins[i], pl.level[i], combValue, combCommit);
        else S.addComb(execComb, pr, pl.ins[i], pl.level[i]);
    }
    for (size_t i = 0; i < lowered.procs.size(); i++) {
        if (pl.rank[i] >= 0) continue;
        if (vpi::st().active && lowered.procs[i].kind == "init") {
            // Declaration initializers take effect before the simulation
            // starts, so they are not value changes VPI callbacks see (6.8).
            Task t = process(&lowered.procs[i].prog);
            S.all.push_back(t.h);
            t.h.resume();
            continue;
        }
        if (pl.seq[i]) S.addSeq(execSeq, &lowered.procs[i].prog, pl.ev[i]);
        else if (lowered.procs[i].program >= 0 || lowered.procs[i].reactive)
            S.spawn(programProc(&lowered.procs[i].prog, lowered.procs[i].program, lowered.procs[i].kind == "Initial"));
        else S.spawn(process(&lowered.procs[i].prog));
    }
    // Sampled values (16.5.1) and $past clocks. Shadows read every signal at
    // the start of a slot, which the static partitions do not keep current.
    S.freeVars = lowered.freeVars;   // free checker variables start random (17.7.2)
    for (auto [s, every] : S.freeVars) S.sigs[s].val = S.freeValue(s);
    S.shadows = shadowPairs();
    S.sclocks = sampleClocks();
    for (size_t c = 0; c < S.sclocks.size(); c++) S.spawn(sampleClockProc((int)c));
    if (upfrt::rt().on) upfrt::rt().start();
    startConcAsserts();
    startClockings();
    startCovergroups();
    if (startSdf()) return 1;   // SDF timing first, so the checks start with the annotated limits
    startTimingChecks();
    if (!pathRecs().empty() || !S.icDelay.empty() || !S.shadowOf.empty() || sdfAtRunTime()) staticPart = false;   // path inputs' change times (30.5.3)
    if (!S.shadows.empty() || !asserts().empty() || !clockings().empty() || pl.cyclic) staticPart = false;
    if (vpi::st().active) vpi::setup(*comp, lowered);   // compiletf, cbEndOfCompile, cbStartOfSimulation
    if (staticPart) {
        std::vector<std::vector<int>> po, pi;
        std::vector<int> pt;
        partitionInputs(lowered, pl, po, pi, pt);
        S.buildPartitions(po, pi, pt);
    }
    S.run(maxTime);
    std::vector<Task> finals = concEnd();   // strong obligations still pending fail (16.12.21)
    for (auto& pr : lowered.finals) finals.push_back(process(&pr.prog));
    S.runFinal(finals);
    if (vpi::st().active) vpi::end();   // cbEndOfSimulation
    std::cout.flush();
    if (g_cov && !writeCoverage(covPath, lowered, S.vcdTop)) {
        std::cerr << "msim: cannot write " << covPath << "\n";
        return 1;
    }
    if (!writeSaif()) return 1;
    return S.exitCode;
}
