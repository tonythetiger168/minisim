#include <condition_variable>
#include <mutex>
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
//   -vcd-arrays [N]  also write unpacked array elements (arrays of up to N, default 1024)
//   -vcd-class [N]   also write class objects through handle variables (h.addr), N levels
//   -saif [<file>]   write switching activity as SAIF (default dump.saif): the
//                    time each bit spent at 0, 1 and x and its toggle count,
//                    for power analysis with mpower (docs/POWER.md)
//   -restore <file> [+args]  go back to a $save checkpoint and go on (docs/CHECKPOINT.md)
//   -elab            stop after elaboration, before the run (xrun -elaborate / -c)
//   -parse           stop after parsing: syntax errors only (vlog / vlogan, mstep.inc)
//   -vlog, -vcom, -vopt, -vsim, -vlib, -vmap, -vlogan, -vhdlan, -vcs
//                    the rest is that Siemens / Synopsys step (also when msim
//                    runs under that name): docs/COMPAT.md
//   -xrun, -irun     take the rest of the command line as xrun / irun options
//                    (also when msim runs as xrun, irun or xmverilog): docs/XRUN.md
//   -profile [<file>] time every process and region and write a report
//                    (default simprofile.txt): busiest processes, time per
//                    instance, file and kind. Runs the interpreter on one thread.
//   -saif_forward <file>  a forward SAIF file (IEEE 1801-2024 Annex J.4, J.5)
//                    for -saif: a library one adds state- and path-dependent
//                    activity of its cells, an RTL one keeps only its mapped
//                    points (repeatable)
//   -maxtime <n>     stop after simulation time n: time-precision ticks, or with a unit (3ms, 500ns) (default: run until no events or $finish)
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
//   -partcomp[=...], -fastpartcomp[=j<n>], -partcomp_dir=<dir>, -Mdir=<dir>
//                    VCS partition compile spellings: -compile, -cfiles n, -o dir
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
//   -upf_cov <file>  low-power coverage: power states, transitions, isolation, retention
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
#include <array>
#include <atomic>
#include <functional>
#include <sstream>
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
#include "cov_select.hpp"
#include "lower.hpp"
#include "saif.hpp"
#include "saif_fwd.hpp"
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
#include "upf_design.hpp"
#include "upf_hdl.hpp"
#include "dpi.inc"
#include "vhdl_env.inc"
#include "vpi.inc"
#include "interactive.inc"
#include "vendor_tasks.hpp"
#include "objview.inc"
#include "statview.inc"
#include "xrun.inc"
#include "mstep.inc"
#include "vhdl/front.hpp"

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
    if (!lowered.trans.empty()) return;   // switches resolve their nets from the drivers (S.tranOf is set later)
    for (uint8_t b : S.pathIn) if (b) return;
    size_t ns = S.sigs.size();
    std::vector<std::vector<Instr*>> by(ns);
    std::vector<char> bad(ns, 0);
    for (auto& [s, k] : lowered.netKinds)
        if ((size_t)s < ns) bad[(size_t)s] = 1;   // wand, trireg, ...: not set on S yet
    for (auto& lp : lowered.procs)
        for (auto& i : lp.prog.code)
            if (i.k == IK::Force) return;   // release goes back to the drivers (10.6.2)
    for (auto& sp : lowered.subs)   // also a force in a task or function
        if (sp)
            for (auto& i : sp->code)
                if (i.k == IK::Force) return;
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

// A for loop with a constant trip count in a clocked process (the reset of
// a register file: for (int i = 0; i < 16; i++) rf[i] <= 0) is unrolled:
// each copy reads i as a constant, so an array write gets its fixed
// element and the process needs neither the blocking writes of i nor a
// jump back. It then runs in the cycle domain like any other register
// process (bench/gen_soc.py: 512 such processes were coroutines). The loop
// variable is automatic and local to the loop (12.7.1); it is kept out of
// the waveform, which never shows automatic variables (21.7.1.2).
//
// With comb set, the same is done in combinational processes (always_comb,
// always @*: body; wait; goto 0), before splitCombBlocks: a loop that sets
// one bit per pass (for (int i = 0; i < 32; i++) r[i] = a[31 - i]) becomes
// straight code, which runs without the compare and step of each pass and
// can be split like any other block.
static void unrollSeqLoops(LowerResult& lowered, bool comb = false) {
    std::set<int> lv(loopVarSigs().begin(), loopVarSigs().end());
    if (lv.empty()) return;
    const bool dbg = std::getenv("MSIM_WHYUNROLL") != nullptr;
    auto plainW = [](const Instr& i) {
        return i.sig >= 0 && i.hi < 0 && !i.arr && i.aidx.empty() && !i.bidx && i.drv < 0 && i.dyn < 0 && !i.de && !i.delay &&
               i.dly.empty() && !i.tmp && i.gate < 0 && !i.udp;
    };
    // The value of e when signal `var` holds `v` and nothing else is read.
    auto constEval = [](const E& e, int var, const Val* v, Val& out) {
        bool other = false;
        Reader rd = [&](int s) -> Val {
            if (v && s == var) return *v;
            other = true;
            return S.sigs[(size_t)s].val;
        };
        try { out = eval(e, rd); } catch (...) { return false; }
        return !other;
    };
    std::function<E(const E&, int, const Val&)> sub = [&](const E& e, int var, const Val& v) -> E {
        if (!e) return e;
        if (e->op == Op::Ref && e->sig == var && !e->at && !e->arr) { Val k = v; k.sgn = e->sgn; return konst(k); }
        bool hit = false;
        std::function<bool(const E&)> has = [&](const E& x) {
            if (!x) return false;
            if (x->op == Op::Ref && x->sig == var) return true;
            if (has(x->at)) return true;
            for (auto& k : x->kids) if (has(k)) return true;
            return false;
        };
        hit = has(e);
        if (!hit) return e;
        auto c = std::make_shared<Expr>(*e);
        c->at = sub(c->at, var, v);
        for (auto& k : c->kids) k = sub(k, var, v);
        // Fold what is constant now (rf[i - 1], irq[i] with i known): the
        // generated code then indexes a fixed bit instead of computing one.
        bool pure = !c->at && !c->arr && c->op >= Op::Not && c->op <= Op::Mux && !c->kids.empty();
        for (auto& k : c->kids) pure &= k && k->op == Op::Const;
        // a[i +: w] with i known and in range: a fixed part select.
        if (c->op == Op::DynSlice && !c->at && c->kids.size() == 2 && c->kids[1]->op == Op::Const && !c->kids[1]->k.x && c->kids[0]) {
            const E& a = c->kids[0];
            int aw = a->op == Op::Slice ? a->hi - a->lo + 1 : a->op == Op::Ref && !a->at && !a->arr ? S.sigs[(size_t)a->sig].w : -1;
            int64_t st = v_to_i64(c->kids[1]->k);
            if (c->lo) st = st - c->hi + 1;
            if (aw > 0 && st >= 0 && st + c->hi <= aw) {
                auto sl = node(Op::Slice);
                sl->kids = {a};
                sl->hi = (int)st + c->hi - 1;
                sl->lo = (int)st;
                return sl;
            }
        }
        if (pure) {
            Val r;
            Reader none = [](int s) -> Val { return S.sigs[(size_t)s].val; };
            try { r = eval(c, none); return konst(r); } catch (...) {}
        }
        return c;
    };
    std::set<int> used;
    for (auto& lp : lowered.procs) {
        if (lp.program >= 0 || lp.reactive) continue;
        auto& code = lp.prog.code;
        size_t n = code.size();
        if (n < 3 || !lp.prog.roots.empty() || !lp.prog.spans.empty() || code[n - 1].k != IK::Jmp || code[n - 1].target != 0)
            continue;
        if (comb ? code[n - 2].k != IK::WaitEdge || code[0].k == IK::WaitEdge : code[0].k != IK::WaitEdge) continue;
        for (size_t pc = comb ? 0 : 1; pc + 3 < code.size(); pc++) {
            // i = c; top: if (!(cond)) goto E; body; step: i = f(i); goto top; E:
            const Instr& a = code[pc];
            if (a.k != IK::Assign || !lv.count(a.sig) || !plainW(a)) continue;
            int var = a.sig;
            const Instr& t = code[pc + 1];
            if (t.k != IK::JmpIfNot || t.xt != Instr::NOX || t.target < pc + 4 || t.target >= code.size()) continue;
            size_t top = pc + 1, E = t.target, step = E - 2;
            if (code[E - 1].k != IK::Jmp || code[E - 1].target != top) continue;
            if (code[step].k != IK::Assign || code[step].sig != var || !plainW(code[step])) continue;
            bool ok = true;
            for (size_t b = top + 1; b < step && ok; b++) {
                const Instr& x = code[b];
                if (x.k == IK::Nba) ok = x.sig != var && x.drv < 0 && x.dyn < 0 && !x.nbaLater && !x.de && !x.delay;
                else if (x.k == IK::Assign && comb)   // a blocking write of the block's own (not a driver, not of i)
                    ok = x.sig != var && x.drv < 0 && x.dyn < 0 && x.dsrc < 0 && !x.de && !x.delay && x.dly.empty() && x.gate < 0 && !x.udp;
                else if (x.k == IK::Jmp || x.k == IK::JmpIfNot)
                    ok = x.target > b && x.target <= E && x.xt == Instr::NOX;   // forward, inside or to the end
                else ok = false;
            }
            // Nothing else jumps into the loop.
            for (size_t b = 0; b < code.size() && ok; b++) {
                if (b >= top && b < E) continue;
                const Instr& x = code[b];
                if ((x.k == IK::Jmp || x.k == IK::JmpIfNot) && ((x.target > pc && x.target < E) || (x.xt != Instr::NOX && x.xt > pc && x.xt < E)))
                    ok = false;
            }
            Val v;
            if (!ok || !constEval(a.e, var, nullptr, v)) continue;
            int w = S.sigs[(size_t)var].w;
            v = v_resize(v, w, v.sgn); v.sgn = false;
            std::vector<Instr> out;
            size_t bodyN = step - (top + 1);
            std::vector<size_t> breaks;   // jumps to E, patched at the end
            int iters = 0;
            while (ok) {
                Val c;
                if (!constEval(t.e, var, &v, c) || c.x) { ok = false; break; }
                if (truth(c) != 1) break;
                if (++iters > 64 || out.size() + bodyN > 4096) { ok = false; break; }
                size_t base = out.size();
                for (size_t b = top + 1; b < step; b++) {
                    Instr x = code[b];
                    x.e = sub(x.e, var, v);
                    for (auto& ai : x.aidx) ai = sub(ai, var, v);
                    x.bidx = sub(x.bidx, var, v);   // v[i*8 +: 8] <= ...: the bit index too
                    if (x.k == IK::Jmp || x.k == IK::JmpIfNot) {
                        if (x.target == E) breaks.push_back(out.size());
                        else x.target = base + (x.target - (top + 1));   // the step: the end of this copy
                    }
                    if (x.k == IK::Nba && x.arr && x.arr->base >= 0 && !x.arr->dynElem && !x.aidx.empty()) {   // a fixed element now?
                        std::vector<int64_t> idx;
                        bool fixed = true;
                        for (auto& ai : x.aidx) {
                            Val iv;
                            if (!constEval(ai, -1, nullptr, iv) || iv.x) { fixed = false; break; }
                            idx.push_back(v_to_i64(iv));
                        }
                        if (fixed) {
                            int64_t off = x.arr->offset(idx);
                            if (off < 0) { x = Instr{}; x.k = IK::Jmp; x.target = out.size() + 1; }   // an invalid index writes nothing (7.4.6)
                            else { x.sig = x.arr->base + (int)off; x.arr = nullptr; x.aidx.clear(); }
                        }
                    }
                    out.push_back(std::move(x));
                }
                Val nv;
                if (!constEval(code[step].e, var, &v, nv)) { ok = false; break; }
                v = v_resize(nv, w, nv.sgn); v.sgn = false;
            }
            if (!ok) continue;
            // Splice: [pc, E) becomes `out`; later targets move by the difference.
            size_t start = pc;
            long delta = (long)out.size() - (long)(E - pc);
            for (auto& x : out) if ((x.k == IK::Jmp || x.k == IK::JmpIfNot)) x.target += start;
            for (size_t b : breaks) out[b].target = start + out.size();
            for (size_t b = 0; b < code.size(); b++) {
                if (b >= pc && b < E) continue;
                Instr& x = code[b];
                if ((x.k == IK::Jmp || x.k == IK::JmpIfNot) && x.target >= E) x.target += delta;
                if (x.xt != Instr::NOX && x.xt >= E) x.xt += delta;
            }
            for (auto& x : out) x.line = x.line ? x.line : code[pc].line;
            code.erase(code.begin() + (long)pc, code.begin() + (long)E);
            code.insert(code.begin() + (long)pc, out.begin(), out.end());
            used.insert(var);
            if (dbg) std::cerr << "unrolled " << S.sigs[(size_t)var].name << ": " << iters << " times, " << out.size() << " instructions\n";
            pc += out.size() ? out.size() - 1 : 0;
        }
        // What is left of an unrolled variable here: constant writes (its
        // initial value at the block's entry) that nothing reads.
        std::set<int> mine;
        for (auto& x : code) if (x.k == IK::Assign && used.count(x.sig)) mine.insert(x.sig);
        if (mine.empty()) continue;
        std::function<void(const E&)> rd = [&](const E& e) {
            if (!e) return;
            if (e->op == Op::Ref) mine.erase(e->sig);
            if (e->arr) mine.erase(e->arr->base);
            rd(e->at);
            for (auto& k : e->kids) rd(k);
        };
        for (auto& x : code) {
            rd(x.e); rd(x.bidx); rd(x.de); rd(x.fd); rd(x.self);
            for (auto& a : x.aidx) rd(a);
            for (auto& a : x.args) rd(a);
            if (x.k != IK::Assign && x.sig >= 0) mine.erase(x.sig);
            for (auto& [sg, ed] : x.events) mine.erase(sg);
        }
        for (size_t b = 0; b < code.size(); b++) {
            Instr& x = code[b];
            Val k;
            if (x.k == IK::Assign && mine.count(x.sig) && plainW(x) && constEval(x.e, -1, nullptr, k)) {
                int ln = x.line;
                x = Instr{}; x.k = IK::Jmp; x.target = b + 1; x.line = ln;
            }
        }
    }
    for (int x : used) S.sigs[(size_t)x].hide = true;
}

// A nonblocking write to an element of a small array chosen at run time
// (rf[wa] <= y) becomes one fixed write per element, each behind its
// compare: if (wa == 0) rf[0] <= y; else if (wa == 1) ... An index that
// matches none (x, z or out of range) writes nothing, as before (7.4.6).
// The clocked process then has only fixed targets and joins the cycle domain.
static void fixArrayNbas(LowerResult& lowered) {
    std::function<bool(const E&)> pure = [&](const E& e) {
        if (!e) return true;
        if (e->at) return false;
        if (!(e->op <= Op::Mux || e->op == Op::WildEq || (e->op == Op::ArrRead && e->arr && !e->arr->dynElem))) return false;
        for (auto& k : e->kids) if (!pure(k)) return false;
        return true;
    };
    for (auto& lp : lowered.procs) {
        if (lp.program >= 0 || lp.reactive) continue;
        auto& code = lp.prog.code;
        size_t n = code.size();
        if (n < 3 || !lp.prog.roots.empty() || !lp.prog.spans.empty() || code[0].k != IK::WaitEdge || code[n - 1].k != IK::Jmp ||
            code[n - 1].target != 0)
            continue;
        bool any = false;
        for (auto& x : code) if (x.k == IK::Nba && x.arr) any = true;
        if (!any) continue;
        std::vector<Instr> out;
        std::vector<char> made;          // a jump made here: its target is already a new pc
        std::vector<size_t> at(n + 1);   // old pc -> new pc
        for (size_t pc = 0; pc < n; pc++) {
            at[pc] = out.size();
            made.resize(out.size(), 0);
            const Instr& x = code[pc];
            const ArrShape* a = x.arr.get();
            bool ok = x.k == IK::Nba && a && a->base >= 0 && !a->dynElem && x.aidx.size() == 1 && a->dims.size() == 1 && a->count >= 1 &&
                      a->count <= 32 && x.drv < 0 && x.dyn < 0 && !x.nbaLater && !x.de && !x.delay && x.dly.empty() && !x.bidx &&
                      std::min(a->dims[0].first, a->dims[0].second) >= 0 && pure(x.aidx[0]) && pure(x.e);
            if (!ok) { out.push_back(x); made.push_back(0); continue; }
            auto [l, r] = a->dims[0];
            std::vector<size_t> ends;
            for (int o = 0; o < a->count; o++) {
                int64_t idx = l <= r ? l + o : l - o;
                Instr j; j.k = IK::JmpIfNot; j.line = x.line;
                j.e = node(Op::Eq, {x.aidx[0], konst(make((uint64_t)idx, 0, 64))});
                size_t jp = out.size();
                out.push_back(j);
                Instr w = x;
                w.sig = a->base + o; w.arr = nullptr; w.aidx.clear();
                out.push_back(w);
                Instr e; e.k = IK::Jmp; e.line = x.line;
                ends.push_back(out.size());
                out.push_back(e);
                out[jp].target = out.size();   // relative to out, as new pcs
            }
            for (size_t k : ends) out[k].target = out.size();
            made.resize(out.size(), 1);
        }
        at[n] = out.size();
        for (size_t k = 0; k < out.size(); k++) {
            Instr& x = out[k];
            if ((x.k != IK::Jmp && x.k != IK::JmpIfNot) || made[k]) continue;
            x.target = at[std::min<size_t>(x.target, n)];
            if (x.xt != Instr::NOX) x.xt = at[std::min<size_t>(x.xt, n)];
        }
        code = std::move(out);
    }
}

// `always @*` (9.4.2.2) lowers to WaitEdge(reads) ; body ; Jmp 0: it waits
// first, so it is not a combinational shape (body ; WaitEdge ; Jmp 0) and
// stays a coroutine. A body of plain assignments and forward jumps becomes
//     JmpIfNot armed -> W ; body ; W: WaitEdge(reads, armed) ; Jmp 0
// which the static schedule ranks like an always_comb (as Verilator treats
// @*). Unlike always_comb, @* does not run at time 0: its outputs keep their
// values until an input changes. `armed` (2-state, 0) is set by a one-shot
// process that waits once on the same inputs, so the body runs from the
// first change on, as the waiting process would. MSIM_NOATSTAR=1 keeps the
// coroutines.
static void rotateAtStar(LowerResult& lowered) {
    const bool dbg = std::getenv("MSIM_WHYATSTAR") != nullptr;
    size_t done = 0, np = lowered.procs.size();
    for (size_t pi = 0; pi < np; pi++) {
        auto& pr = lowered.procs[pi];
        Prog& q = pr.prog;
        size_t n = q.code.size();
        if (pr.kind != "Always" || !pr.loops || pr.program >= 0 || pr.reactive || n < 3) continue;
        if (!q.roots.empty() || !q.spans.empty()) continue;
        const Instr& w = q.code[0];
        const Instr& j = q.code[n - 1];
        if (w.k != IK::WaitEdge || w.dynWait || j.k != IK::Jmp || j.target != 0) continue;
        std::vector<std::pair<int, Edge>> ev = w.events.empty() ? std::vector<std::pair<int, Edge>>{{w.sig, w.edge}} : w.events;
        bool ok = true;
        for (auto& [sg, ed] : ev) ok &= ed == ANY && sg >= 0;
        for (size_t pc = 1; ok && pc + 1 < n; pc++) {
            const Instr& x = q.code[pc];
            if (x.k == IK::Jmp || x.k == IK::JmpIfNot) {
                ok &= x.target > pc && x.target <= n - 1 && (x.xt == Instr::NOX || (x.xt > pc && x.xt <= n - 1));
            } else if (x.k == IK::Assert) ok &= x.join == 1 || x.join == 2;
            else ok &= x.k == IK::Assign && !x.de && !x.delay && x.dly.empty();
        }
        if (!ok) continue;
        // The wait must cover everything the body reads (the sensitivity of
        // @*, or a complete hand-written list).
        auto rs = combReadSet(q);
        std::set<int> have;
        for (auto& e : ev) have.insert(e.first);
        for (auto& e : rs) ok &= have.count(e.first) > 0;
        if (!ok) { if (dbg) std::cerr << "@* kept: the wait misses a read, line " << q.code[1].line << "\n"; continue; }
        int armed = S.sig("$atstar" + std::to_string(done), 1, false);
        S.sigs[(size_t)armed].val = Val{0, 0, 1, 0, false};
        S.sigs[(size_t)armed].hide = true;
        Instr gate;
        gate.k = IK::JmpIfNot;
        gate.e = ref(armed);
        gate.target = n - 1;
        gate.line = q.code[1].line;
        gate.file = q.code[1].file;
        Instr wait = q.code[0];
        ev.push_back({armed, ANY});
        wait = waitInstr(ev);
        wait.line = q.code[0].line;
        wait.file = q.code[0].file;
        q.code[0] = gate;
        q.code.insert(q.code.end() - 1, wait);
        // The one-shot process: wait for the first change, then arm.
        LoweredProc arm;
        arm.kind = "Always";
        arm.loops = false;
        arm.scope = pr.scope;
        Instr w1 = waitInstr(std::vector<std::pair<int, Edge>>(ev.begin(), ev.end() - 1));
        Instr set;
        set.k = IK::Assign;
        set.sig = armed;
        set.e = konst(Val{1, 0, 1, 0, false});
        arm.prog.code = {w1, set};
        // The one-shot process takes the block's place in the start order,
        // so it waits as early as the block would have (a change at time 0
        // from a later process still wakes it); the block, now ranked, goes last.
        LoweredProc blk = std::move(pr);
        lowered.procs[pi] = std::move(arm);
        lowered.procs.push_back(std::move(blk));
        done++;
    }
    if (dbg) std::cerr << "@* rotated: " << done << "\n";
}

// A clocked block that keeps working values in module variables with
// blocking assignments (PicoRV32's `set_mem_do_rinst = 0; ... if (...)
// set_mem_do_rinst = 1;`) is not a seqShape: its Assigns wake readers, so
// it stays a coroutine. When no other process, final block or wait touches
// such a variable and nothing dumps waveforms, nobody can see the change
// events: its Assigns become temporaries (Instr::tmp, a store without
// notification; the value is still kept in the signal, so a read before the
// first write of an activation sees the last one, as before), and the block
// can join the static clocked schedule. MSIM_NOSEQTMP=1 keeps them.
static void localizeSeqBlocking(LowerResult& lowered) {
    const bool dbg = std::getenv("MSIM_WHYSEQTMP") != nullptr;
    size_t ns = S.sigs.size();
    // Per signal: the process that touches it, -1 none, -2 more than one or something else.
    std::vector<int> owner(ns, -1);
    auto touch = [&](int s, int who) {
        if (s < 0 || (size_t)s >= ns) return;
        int& o = owner[s];
        o = (o == -1 || o == who) ? who : -2;
    };
    auto scanProg = [&](const Prog& top, int who) -> bool {
        std::vector<const Prog*> seen, todo{&top};
        while (!todo.empty()) {
            const Prog* p = todo.back(); todo.pop_back();
            if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
            seen.push_back(p);
            for (const Instr& i : p->code) {
                if (i.k == IK::Dump) return false;
                touch(i.sig, who);
                for (auto& [sg, ed] : i.events) touch(sg, who);
                if (i.arr) for (int k = 0; k < i.arr->count; k++) touch(i.arr->base + k, who);
                if (i.k == IK::Call && i.callee) todo.push_back(i.callee);
                for (const Prog* b : i.branches) todo.push_back(b);
            }
            std::vector<int> rd;
            std::vector<const Prog*> seen2;
            collectProgRefs(*p, rd, seen2);
            for (int r : rd) touch(r, who);
        }
        return true;
    };
    for (size_t pi = 0; pi < lowered.procs.size(); pi++)
        if (!scanProg(lowered.procs[pi].prog, (int)pi)) return;
    for (auto& f : lowered.finals) if (!scanProg(f.prog, -2)) return;
    for (auto& sp : lowered.subs) if (sp) for (const Instr& i : sp->code) if (i.k == IK::Dump) return;
    size_t done = 0, procsDone = 0;
    for (size_t pi = 0; pi < lowered.procs.size(); pi++) {
        auto& pr = lowered.procs[pi];
        Prog& q = pr.prog;
        size_t n = q.code.size();
        if (!pr.loops || pr.program >= 0 || pr.reactive || n < 3) continue;
        if (!q.roots.empty() || !q.spans.empty()) continue;
        if (q.code[0].k != IK::WaitEdge || q.code[n - 1].k != IK::Jmp || q.code[n - 1].target != 0) continue;
        bool edge = !q.code[0].events.empty() || q.code[0].edge != ANY;
        for (auto& [sg, ed] : q.code[0].events) edge &= ed != ANY;
        if (!edge) continue;   // a clocked block only
        std::vector<size_t> cand;
        bool ok = true;
        for (size_t pc = 1; ok && pc + 1 < n; pc++) {
            const Instr& x = q.code[pc];
            if (x.k != IK::Assign || x.tmp) continue;
            ok = x.sig >= 0 && (size_t)x.sig < ns && owner[x.sig] == (int)pi && x.drv < 0 && !x.arr && x.aidx.empty() &&
                 x.dyn < 0 && !x.de && !x.delay && x.dly.empty() && x.gate < 0 && !x.udp;
            cand.push_back(pc);
        }
        if (!ok || cand.empty()) {
            if (dbg && !cand.empty()) std::cerr << "seqtmp kept: line " << q.code[1].line << "\n";
            continue;
        }
        static const long lim = std::getenv("MSIM_SEQTMPMAX") ? std::atol(std::getenv("MSIM_SEQTMPMAX")) : -1;   // debug: bisect
        if (lim >= 0 && (long)procsDone >= lim) continue;
        for (size_t pc : cand) q.code[pc].tmp = true;
        if (dbg) for (size_t pc : cand) std::cerr << "seqtmp: line " << q.code[pc].line << " " << S.sigs[q.code[pc].sig].name << "\n";
        done += cand.size();
        procsDone++;
    }
    if (dbg) std::cerr << "seqtmp: " << done << " assigns in " << procsDone << " clocked blocks\n";
}

// Subroutine calls in a combinational block (always_comb, or an assign whose
// value calls a function) are expanded in place, so the block is straight
// code that the static schedule can rank and split like any other. A static
// subroutine keeps its arguments, result and locals in signals that every
// caller shares; the expanded copy gets its own, when no value ever travels
// between callers through them: here and in every other process each read
// follows a whole write in the same activation.
// A call of a task that does nothing (PicoRV32's `assert in a non-formal
// build calls `task empty_statement; begin end endtask`) becomes a jump to
// the next instruction: a call keeps a clocked block out of the static
// schedule and the cycle domain, where one empty call per block made two
// PicoRV32 blocks per core run as coroutines on every clock edge.
// MSIM_NOEMPTYCALL=1 keeps the calls.
static void dropEmptyCalls(LowerResult& lowered) {
    size_t n = 0;
    auto empty = [](const Prog* c) {
        if (!c) return false;
        for (auto& x : c->code) if (x.k != IK::Ret) return false;
        return c->roots.empty() && c->spans.empty();
    };
    for (auto& pr : lowered.procs)
        for (size_t pc = 0; pc < pr.prog.code.size(); pc++) {
            Instr& i = pr.prog.code[pc];
            if (i.k != IK::Call || !empty(i.callee) || i.lay || !i.init.empty()) continue;
            Instr j;
            j.k = IK::Jmp;
            j.target = pc + 1;
            j.line = i.line;
            j.file = i.file;
            i = std::move(j);
            n++;
        }
    if (std::getenv("MSIM_WHYATSTAR")) std::cerr << "empty task calls dropped: " << n << "\n";
}

static void inlineCombCalls(LowerResult& lowered) {
    const bool dbg = std::getenv("MSIM_WHYINLINE") != nullptr;
    auto plainAssign = [](const Instr& i) {
        return i.k == IK::Assign && i.sig >= 0 && !i.arr && i.aidx.empty() && !i.de && !i.delay && i.dly.empty() && i.drv < 0 &&
               i.dyn < 0 && i.dsrc < 0 && i.dres < 0 && i.dkey < 0 && !i.tmp;
    };
    auto plainCall = [](const Instr& i) {
        return i.k == IK::Call && i.callee && !i.lay && !i.self && i.vslot == -1 && i.init.empty() && i.dinit.empty();
    };
    // A body that is only assignments, forward branches, deferred assertions and such calls.
    std::function<bool(const Prog&, int)> okBody = [&](const Prog& c, int depth) {
        if (depth > 16 || !c.roots.empty() || !c.spans.empty()) return false;   // spans: a disable finds it by its own code (9.6.2)
        for (size_t pc = 0; pc < c.code.size(); pc++) {
            const Instr& i = c.code[pc];
            if (i.k == IK::Jmp || i.k == IK::JmpIfNot) {
                if (i.target <= pc || i.xt != Instr::NOX) return false;
            } else if (i.k == IK::Ret) {
            } else if (i.k == IK::Assert) {
                if (i.join != 1 && i.join != 2) return false;
            } else if (i.k == IK::Call) {
                if (!plainCall(i) || !okBody(*i.callee, depth + 1)) return false;
            } else if (!plainAssign(i)) return false;
        }
        return true;
    };
    std::function<void(const E&, std::set<int>&)> refs = [&](const E& e, std::set<int>& out) {
        if (!e) return;
        if (e->sig >= 0) out.insert(e->sig);
        if (e->arr) out.insert(e->arr->base);
        refs(e->at, out);
        for (auto& k : e->kids) refs(k, out);
    };
    auto instrRefs = [&](const Instr& i, std::set<int>& out) {
        refs(i.e, out); refs(i.bidx, out); refs(i.de, out); refs(i.fd, out); refs(i.self, out);
        for (auto& a : i.args) refs(a, out);
        for (auto& a : i.aidx) refs(a, out);
        for (auto& a : i.init) refs(a.second, out);
        if (i.k == IK::Assert) {
            const AssertInfo& a = asserts().at(i.sig);
            for (auto* cs : {&a.passCap, &a.failCap, &a.passSlot, &a.failSlot})
                for (auto& c : *cs) refs(c, out);
        }
    };
    // Every candidate a body may read before it has written it whole
    // (`def`: written so far; a wait forgets them all). Returns false for a
    // body it cannot follow: its candidates are then all in `bad`.
    std::set<int> cand, bad;
    // def: the bits written so far of each signal (all ones: the whole of it).
    using Def = std::map<int, uint64_t>;
    auto full = [&](int x) -> uint64_t { int w = x >= 0 && (size_t)x < S.sigs.size() ? S.sigs[(size_t)x].w : 0; return w > 0 && w < 64 ? (1ULL << w) - 1 : ~0ULL; };
    auto isDef = [&](const Def& d, int x) { auto it = d.find(x); return it != d.end() && (it->second & full(x)) == full(x); };
    auto both = [](const Def& a, const Def& b) { Def r; for (auto& [x, m] : a) { auto it = b.find(x); if (it != b.end() && (m & it->second)) r[x] = m & it->second; } return r; };
    const char* curWhy = "";
    auto mark = [&](int x, const char* why) { if (bad.insert(x).second && dbg && x >= 0 && (size_t)x < S.sigs.size()) std::cerr << "    shared: " << S.sigs[(size_t)x].name << " (" << why << ", " << curWhy << ")\n"; };
    std::function<void(const Prog&, Def&, int)> flow = [&](const Prog& q, Def& def, int depth) {
        size_t n = q.code.size();
        std::vector<char> has(n + 1, 0), back(n + 1, 0);
        std::vector<Def> in(n + 1);
        for (size_t pc = 0; pc < n; pc++)
            if ((q.code[pc].k == IK::Jmp || q.code[pc].k == IK::JmpIfNot) && q.code[pc].target <= pc) back[std::min(q.code[pc].target, n)] = 1;
        auto meet = [&](size_t t, const Def& d) {
            t = std::min(t, n);
            if (!has[t]) { in[t] = d; has[t] = 1; return; }
            in[t] = both(in[t], d);
        };
        in[0] = def; has[0] = 1;
        Def out;   // at the exits
        bool exits = false;
        auto leave = [&](const Def& d) {
            if (!exits) { out = d; exits = true; return; }
            out = both(out, d);
        };
        for (size_t pc = 0; pc < n; pc++) {
            if (back[pc]) { in[pc].clear(); has[pc] = 1; }
            if (!has[pc]) continue;   // not reached (the code after a return)
            Def d = std::move(in[pc]);
            const Instr& i = q.code[pc];
            std::set<int> rd;
            instrRefs(i, rd);
            bool known = true, falls = true;
            switch (i.k) {
                case IK::Assign:
                    if (i.sig >= 0 && (i.bidx || !plainAssign(i) || (i.hi >= 0 && S.sigs[(size_t)i.sig].w > 64))) rd.insert(i.sig);   // keeps the other bits
                    break;
                case IK::Jmp: falls = false; break;
                case IK::JmpIfNot: case IK::Display: case IK::Finish: case IK::Assert: case IK::Ret: break;
                case IK::WaitDelay: break;
                case IK::WaitEdge:
                    if (i.events.empty()) { if (cand.count(i.sig)) mark(i.sig, "waited on"); }
                    for (auto& [sg, ed] : i.events) if (cand.count(sg)) mark(sg, "waited on");
                    break;
                case IK::Call: break;
                default: known = false; break;
            }
            for (int x : rd) if (cand.count(x) && !isDef(d, x)) mark(x, "read before written");
            if (!known) {   // anything else: whatever it names is out
                if (i.sig >= 0 && cand.count(i.sig)) mark(i.sig, "other use");
                for (int x : rd) if (cand.count(x)) mark(x, "other use");
                for (int x : {i.dsrc, i.dres, i.dkey, i.dyn}) if (cand.count(x)) mark(x, "other use");
                for (const Prog* b : i.branches) { Def e; flow(*b, e, depth + 1); }
                d.clear();
            }
            if (i.k == IK::Assign && plainAssign(i) && !i.bidx) {
                if (i.hi < 0) d[i.sig] = ~0ULL;
                else if (S.sigs[(size_t)i.sig].w <= 64) d[i.sig] |= (i.hi - i.lo >= 63 ? ~0ULL : ((1ULL << (i.hi - i.lo + 1)) - 1)) << i.lo;
            }
            if (i.k == IK::WaitDelay || i.k == IK::WaitEdge) d.clear();
            if (i.k == IK::Call) {
                if (i.callee && depth < 16 && !i.lay) flow(*i.callee, d, depth + 1);
                else {
                    if (i.callee) {
                        std::vector<int> sg; std::vector<const Prog*> seen;
                        collectProgRefs(*i.callee, sg, seen);
                        for (int x : sg) if (cand.count(x)) mark(x, "other use");
                        std::unordered_set<int> wr; seen.clear();
                        collectProgWrites(*i.callee, wr, seen);
                        for (int x : wr) if (cand.count(x)) mark(x, "other use");
                    }
                    d.clear();
                }
            }
            if (i.k == IK::Ret) { leave(d); continue; }
            if (i.k == IK::Jmp || i.k == IK::JmpIfNot) { if (i.target > pc) meet(i.target, d); }
            if (i.k == IK::JmpIfNot && i.xt != Instr::NOX && i.xt > pc) meet(i.xt, d);
            if (falls) meet(pc + 1, d);
        }
        if (has[n]) leave(in[n]);
        def = exits ? out : Def{};
    };
    // Does a prog (or one it calls or forks) name a candidate at all?
    auto touches = [&](const Prog& q) {
        std::vector<const Prog*> seen;
        std::function<bool(const Prog&)> t = [&](const Prog& r) {
            if (std::find(seen.begin(), seen.end(), &r) != seen.end()) return false;
            seen.push_back(&r);
            for (auto& i : r.code) {
                std::set<int> rd;
                instrRefs(i, rd);
                if (i.k != IK::Assert && i.sig >= 0) rd.insert(i.sig);
                for (auto& [sg, ed] : i.events) rd.insert(sg);
                for (int x : {i.dsrc, i.dres, i.dkey, i.dyn}) if (x >= 0) rd.insert(x);
                for (int x : rd) if (cand.count(x)) return true;
                if (i.callee && t(*i.callee)) return true;
                for (const Prog* b : i.branches) if (t(*b)) return true;
            }
            return false;
        };
        return t(q);
    };
    int serial = 0;
    for (size_t pi = 0; pi < lowered.procs.size(); pi++) {
        LoweredProc& lp = lowered.procs[pi];
        if (lp.program >= 0 || lp.reactive) continue;
        const Prog& p = lp.prog;
        size_t n = p.code.size();
        if (n < 3 || !p.roots.empty() || !p.spans.empty() || p.code[n - 1].k != IK::Jmp || p.code[n - 1].target != 0 || p.code[n - 2].k != IK::WaitEdge) continue;
        bool calls = false, ok = true;
        for (size_t pc = 0; pc + 2 < n && ok; pc++) {
            const Instr& i = p.code[pc];
            if (i.k == IK::Call) { calls = true; ok = plainCall(i) && okBody(*i.callee, 0); }
            else if (i.k == IK::Jmp || i.k == IK::JmpIfNot) ok = i.target > pc && i.target <= n - 2 && i.xt == Instr::NOX;
            else if (i.k == IK::Assert) ok = i.join == 1 || i.join == 2;
            else ok = i.k == IK::Assign;
        }
        if (!calls || !ok) continue;
        // The signals of the subroutines: everything their bodies name.
        cand.clear(); bad.clear();
        {
            std::vector<const Prog*> seen;
            std::function<void(const Prog&)> names = [&](const Prog& c) {
                if (std::find(seen.begin(), seen.end(), &c) != seen.end()) return;
                seen.push_back(&c);
                for (auto& i : c.code) {
                    if (i.k == IK::Assign && i.sig >= 0) cand.insert(i.sig);
                    refs(i.e, cand); refs(i.bidx, cand);
                    if (i.k == IK::Call) names(*i.callee);
                }
            };
            for (size_t pc = 0; pc + 2 < n; pc++) if (p.code[pc].k == IK::Call) names(*p.code[pc].callee);
        }
        // The expanded body.
        std::vector<Instr> code = p.code;
        bool big = false;
        for (size_t pos = 0; pos < code.size() && !big;) {
            if (code[pos].k != IK::Call) { pos++; continue; }
            const Prog& c = *code[pos].callee;
            size_t L = c.code.size();
            std::vector<Instr> mid;
            for (size_t k = 0; k < L; k++) {
                Instr i = c.code[k];
                if (i.k == IK::Ret) { Instr j; j.k = IK::Jmp; j.file = i.file; j.line = i.line; j.target = pos + L; i = j; }
                else if (i.k == IK::Jmp || i.k == IK::JmpIfNot) i.target = pos + std::min(i.target, L);
                i.hits = i.taken = 0;
                mid.push_back(std::move(i));
            }
            for (auto& i : code)
                if ((i.k == IK::Jmp || i.k == IK::JmpIfNot) && i.target > pos) i.target += L - 1;
            code.erase(code.begin() + (long)pos);
            code.insert(code.begin() + (long)pos, mid.begin(), mid.end());
            code.back().target = 0;
            big = code.size() > 20000;
        }
        if (big) continue;
        // Which candidates are scratch: never waited on, written only by
        // plain blocking assignments, and written before read everywhere.
        for (int x : cand)
            if (x < 0 || (size_t)x >= S.sigs.size() || S.sigs[(size_t)x].net || !S.sigs[(size_t)x].drv.empty()) mark(x, "other use");
        {
            Prog q;
            q.code = code;
            curWhy = "this block";
            Def d;
            flow(q, d, 0);
        }
        auto other = [&](const Prog& q) {
            if (!touches(q)) return;
            curWhy = "another process";
            Def d;
            flow(q, d, 0);
        };
        for (size_t pj = 0; pj < lowered.procs.size(); pj++) if (pj != pi) other(lowered.procs[pj].prog);
        for (auto& f : lowered.finals) other(f.prog);
        for (auto& a : asserts()) {
            std::set<int> rd;
            for (auto* cs : {&a.passCap, &a.failCap, &a.passSlot, &a.failSlot})
                for (auto& c : *cs) refs(c, rd);
            for (int x : rd) if (cand.count(x)) mark(x, "other use");
        }
        std::map<int, int> clone;
        for (int x : cand) {
            if (bad.count(x)) continue;
            bool used = false;
            for (auto& i : code) {
                std::set<int> rd;
                instrRefs(i, rd);
                if (i.k == IK::Assign) rd.insert(i.sig);
                if (rd.count(x)) { used = true; break; }
            }
            if (!used) continue;
            // Named as a temporary of the lowering (_tf...), without the
            // characters that mark array elements and internal signals.
            std::string nm = S.sigs[(size_t)x].name;
            nm = nm.substr(nm.rfind('.') == std::string::npos ? 0 : nm.rfind('.') + 1);
            for (char& ch : nm) if (!std::isalnum((unsigned char)ch)) ch = '_';
            int w = S.sigs[(size_t)x].w;
            clone[x] = S.sig("_tfc" + std::to_string(serial++) + "_" + nm, w, false);
            S.sigs[(size_t)clone[x]].hide = true;
        }
        std::function<E(const E&)> sub = [&](const E& e) -> E {
            if (!e) return e;
            std::set<int> rd;
            refs(e, rd);
            bool hit = false;
            for (int x : rd) hit |= clone.count(x) > 0;
            if (!hit) return e;
            auto c = std::make_shared<Expr>(*e);
            if (c->sig >= 0 && !c->arr) { auto it = clone.find(c->sig); if (it != clone.end()) c->sig = it->second; }
            c->at = sub(c->at);
            for (auto& k : c->kids) k = sub(k);
            return c;
        };
        for (auto& i : code) {
            if (i.k == IK::Assign) { auto it = clone.find(i.sig); if (it != clone.end()) i.sig = it->second; }
            i.e = sub(i.e); i.bidx = sub(i.bidx);
            for (auto& a : i.args) a = sub(a);
        }
        Prog trial = lp.prog;
        trial.code = code;
        CombShape sh;
        bool shaped = combShape(trial, sh);
        if (dbg) {
            std::cerr << "inline " << lp.kind << " line " << p.code[0].line << ": " << n << " -> " << code.size() << " instructions, " << clone.size()
                      << " private of " << cand.size() << (shaped ? "" : " (still not a static block)") << "\n";
        }
        lp.prog.code = std::move(code);
    }
}


// An always_comb with if / case that writes several signals, as one slice per
// group of outputs: the block's control flow with only the assignments to
// that group, minus the branches that no longer choose anything, waiting on
// just the signals the slice reads. A controller that sets thirty outputs in
// one case statement otherwise makes everything it reads feed everything it
// writes; on Ibex that tied 106 blocks into one cycle of whole-block
// dependencies, and every change in it re-ran the whole controller.
// Outputs are grouped when one's slice reads the other (an intermediate
// value inside the block), so a slice only reads final values from outside
// its group. The block's deferred assertions form a group of their own.
static void splitCombBlocks(LowerResult& lowered) {
    size_t np = lowered.procs.size();
    // Temporaries of the lowering (_tf<n>: a unique case's "which item
    // matched") that only one process names: a slice that reads one carries
    // the assignments to it, so they do not tie the outputs together.
    std::vector<int> users(S.sigs.size(), 0);
    {
        std::function<void(const E&, std::set<int>&)> all = [&](const E& e, std::set<int>& out) {
            if (!e) return;
            if (e->sig >= 0) out.insert(e->sig);
            for (auto& k : e->kids) all(k, out);
        };
        std::vector<const Prog*> seen;
        std::function<void(const Prog*, std::set<int>&)> walk = [&](const Prog* p, std::set<int>& out) {
            if (!p || std::find(seen.begin(), seen.end(), p) != seen.end()) return;
            seen.push_back(p);
            for (auto& in : p->code) {
                if (in.sig >= 0) out.insert(in.sig);
                all(in.e, out); all(in.bidx, out); all(in.de, out);
                for (auto& a : in.args) all(a, out);
                for (auto& a : in.aidx) all(a, out);
                for (auto& [sg, ed] : in.events) out.insert(sg);
                walk(in.callee, out);
                for (auto* b : in.branches) walk(b, out);
            }
        };
        for (auto& lp : lowered.procs) {
            std::set<int> m;
            seen.clear();
            walk(&lp.prog, m);
            for (int x : m) if ((size_t)x < users.size()) users[(size_t)x]++;
        }
    }
    auto isTemp = [&](int sg) {
        return sg >= 0 && (size_t)sg < users.size() && users[(size_t)sg] == 1 && S.sigs[(size_t)sg].name.rfind("_tf", 0) == 0;
    };
    for (size_t pi = 0; pi < np; pi++) {
        if (lowered.procs[pi].program >= 0 || lowered.procs[pi].reactive) continue;
        CombShape sh;
        {
            const Prog& p = lowered.procs[pi].prog;
            if (!combShape(p, sh) || !sh.branches || sh.outs.size() + (sh.asserts ? 1 : 0) < 2) continue;
        }
        const Prog orig = lowered.procs[pi].prog;
        const auto& code = orig.code;
        size_t n = code.size(), end = n - 2;   // code[end] is the wait
        bool ok = true;
        const char* why = "";
        struct Rep { const char*& w; bool& ok; const LoweredProc& lp; size_t no; ~Rep() { if (std::getenv("MSIM_WHYSPLIT")) std::cerr << "split " << lp.kind << " line " << (lp.prog.code.empty() ? 0 : lp.prog.code[0].line) << " outs " << no << ": " << (*w ? w : "split") << "\n"; } } rep{why, ok, lowered.procs[pi], sh.outs.size()};
        std::set<int> refAll;
        std::function<void(const E&, std::set<int>&)> refs = [&](const E& e, std::set<int>& out) {
            if (!e) return;
            if (e->arr) { ok = false; why = "arr"; }   // an element chosen at run time: no fixed set of signals
            if (e->op == Op::Ref && e->sig >= 0) out.insert(e->sig);
            for (auto& k : e->kids) refs(k, out);
        };
        for (size_t pc = 0; pc < end; pc++) {
            const Instr& i = code[pc];
            if ((i.k == IK::Jmp || i.k == IK::JmpIfNot) && i.target <= pc) { ok = false; why = "loop"; }   // a loop
            if (i.tmp || i.xt != Instr::NOX) { ok = false; why = "-xprop code"; }   // shared temporaries, out-of-line jumps
            if (i.bidx) { ok = false; why = "variable bit select"; }
            if (i.k == IK::Assert) {
                const AssertInfo& a = asserts().at(i.sig);
                for (auto* cs : {&a.passCap, &a.failCap})
                    for (auto& c : *cs) refs(c, refAll);
            }
            refs(i.e, refAll);
        }
        const Instr& w = code[end];
        std::vector<std::pair<int, Edge>> ev = w.events;
        if (ev.size() <= 1) ev = {{w.sig, w.edge}};
        for (auto& [sg, ed] : ev) if (!refAll.count(sg)) { ok = false; if (!*why) why = "waits on a signal the body does not name"; }
        if (!ok) continue;
        // Output index of each instruction: its signal's, or the assertions' (last).
        size_t no = sh.outs.size() + (sh.asserts ? 1 : 0);
        auto outOf = [&](const Instr& i) -> int {
            if (i.k == IK::Assert) return (int)sh.outs.size();
            if (i.k != IK::Assign || isTemp(i.sig)) return -1;
            for (size_t k = 0; k < sh.outs.size(); k++) if (sh.outs[k] == i.sig) return (int)k;
            return -1;
        };
        std::vector<int> grp(no);
        for (size_t k = 0; k < no; k++) grp[k] = (int)k;
        std::function<int(int)> find = [&](int x) { while (grp[x] != x) x = grp[x] = grp[grp[x]]; return x; };
        // The slice of one group: nxt[pc] is the first instruction it keeps
        // at or after pc (end: none). A branch is kept when its two ways differ.
        std::vector<size_t> nxt(n);
        std::vector<char> keep(n);
        auto slice = [&](int g) {
            std::set<int> temps;   // the temporaries the slice reads: their assignments stay too
            for (;;) {
                nxt[end] = end;
                for (size_t pc = end; pc-- > 0;) {
                    const Instr& i = code[pc];
                    keep[pc] = 0;
                    if (i.k == IK::Jmp) nxt[pc] = nxt[i.target];
                    else if (i.k == IK::JmpIfNot) {
                        size_t a = nxt[pc + 1], b = nxt[i.target];
                        if (a == b) nxt[pc] = a;
                        else { nxt[pc] = pc; keep[pc] = 1; }
                    } else if ((outOf(i) >= 0 && find(outOf(i)) == g) || (i.k == IK::Assign && temps.count(i.sig))) { nxt[pc] = pc; keep[pc] = 1; }
                    else nxt[pc] = nxt[pc + 1];
                }
                size_t before = temps.size();
                std::set<int> rd;
                for (size_t pc = 0; pc < end; pc++) if (keep[pc]) refs(code[pc].e, rd);
                for (int x : rd) if (isTemp(x)) temps.insert(x);
                if (temps.size() == before) break;
            }
        };
        for (bool again = true; again;) {
            again = false;
            for (size_t k = 0; k < no && !again; k++) {
                if (find((int)k) != (int)k || (k < sh.outs.size() && isTemp(sh.outs[k]))) continue;
                slice((int)k);
                std::set<int> rd;
                for (size_t pc = 0; pc < end; pc++) {
                    if (!keep[pc]) continue;
                    refs(code[pc].e, rd);
                    if (code[pc].k == IK::Assign && (code[pc].hi >= 0 || code[pc].bidx)) rd.insert(code[pc].sig);   // a part write keeps the other bits
                }
                for (size_t j = 0; j < sh.outs.size(); j++)
                    if (rd.count(sh.outs[j]) && !isTemp(sh.outs[j]) && find((int)j) != (int)k) { grp[find((int)j)] = (int)k; again = true; }
            }
        }
        std::vector<int> roots;
        for (size_t k = 0; k < no; k++)
            if (find((int)k) == (int)k && !(k < sh.outs.size() && isTemp(sh.outs[k]))) roots.push_back((int)k);
        if (roots.size() < 2) { why = "one group"; continue; }
        std::vector<Prog> progs;
        for (int g : roots) {
            slice(g);
            Prog q;
            q.pd = orig.pd; q.pstate = orig.pstate;
            std::vector<size_t> at(n, 0);       // old pc -> new index
            std::vector<std::pair<size_t, size_t>> fix;   // new index of a jump, old target pc
            std::set<int> rd;
            for (size_t pc = 0; pc < end; pc++) {
                if (!keep[pc]) continue;
                at[pc] = q.code.size();
                Instr i = code[pc];
                refs(i.e, rd);
                if (i.k == IK::Assert) {
                    const AssertInfo& a = asserts().at(i.sig);
                    for (auto* cs : {&a.passCap, &a.failCap})
                        for (auto& c : *cs) refs(c, rd);
                }
                q.code.push_back(i);
                if (i.k == IK::JmpIfNot) fix.push_back({q.code.size() - 1, nxt[i.target]});
                // Falls through to the next kept instruction, or jumps to where it went.
                size_t to = nxt[pc + 1], nextKept = pc + 1;
                while (nextKept < end && !keep[nextKept]) nextKept++;
                if (to != nextKept) {
                    Instr j; j.k = IK::Jmp; j.file = i.file; j.line = i.line;
                    q.code.push_back(j);
                    fix.push_back({q.code.size() - 1, to});
                }
            }
            size_t first = nxt[0];
            if (first != end) {
                size_t fk = 0;
                while (fk < end && !keep[fk]) fk++;
                if (first != fk) { ok = false; why = "entry"; break; }   // cannot happen: the first kept instruction is nxt[0]
            }
            at[end] = q.code.size();
            for (auto& [ni, old] : fix) q.code[ni].target = at[old];
            Instr wt = w;
            wt.events.clear();
            for (auto& e : ev) if (rd.count(e.first)) wt.events.push_back(e);
            if (wt.events.empty()) { ok = false; why = "a slice waits on nothing"; break; }   // reads nothing it waits on: leave the block whole
            if (wt.events.size() == 1) { wt.sig = wt.events[0].first; wt.edge = wt.events[0].second; wt.events.clear(); }
            q.code.push_back(wt);
            Instr back = code[n - 1];
            q.code.push_back(back);
            progs.push_back(std::move(q));
        }
        if (!ok) continue;
        for (auto& q : progs) {
            CombShape c;
            if (!combShape(q, c)) { ok = false; why = "slice shape"; break; }
        }
        if (!ok) continue;
        LoweredProc tmpl = lowered.procs[pi];
        lowered.procs[pi].prog = std::move(progs[0]);
        for (size_t k = 1; k < progs.size(); k++) {
            LoweredProc lp = tmpl;
            lp.prog = std::move(progs[k]);
            lowered.procs.push_back(std::move(lp));
        }
    }
}

// Under -upf, a process stays a coroutine on the interpreter when power can
// gate it (it is in a power domain) or it runs UPF code (an isolation or
// level-shifter cell on a port connection, the UPF package): the rest of the
// design is planned, and runs on several threads, as without -upf.
// A process that writes a signal the UPF runtime watches (a supply or power
// state control, a logic expression's operand) stays a coroutine too, so the
// power states see its value in the same order as without planning.
static bool upfPinned(const Prog& q) {
    if (!g_upf) return false;
    if (q.pd >= 0) return true;
    static const std::set<int> watched = [] {
        std::set<int> w;
        for (auto& [sg, e] : upfrt::rt().watch) w.insert(sg);
        return w;
    }();
    for (auto& i : q.code)
        if ((i.k == IK::Assign || i.k == IK::Nba) && i.sig >= 0 && watched.count(i.sig)) return true;
    std::function<bool(const E&)> has = [&](const E& e) -> bool {
        if (!e) return false;
        if (e->op == Op::SysFn && e->hi == SF_UPF) return true;
        if (has(e->at)) return true;
        for (auto& k : e->kids) if (has(k)) return true;
        return false;
    };
    for (auto& i : q.code) {
        if (has(i.e) || has(i.bidx) || has(i.de) || has(i.fd) || has(i.self)) return true;
        for (auto& a : i.args) if (has(a)) return true;
        for (auto& a : i.aidx) if (has(a)) return true;
        if (i.k == IK::Call || i.k == IK::Fork) return true;   // the callee may run UPF code
    }
    return false;
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
            if (lowered.procs[i].program < 0 && !lowered.procs[i].reactive && !upfPinned(lowered.procs[i].prog) && combShape(lowered.procs[i].prog, c)) { shapes.push_back(c); procOf.push_back(i); }
        }
        std::vector<int> lv;
        // MSIM_WHYCYCLE=1: the blocks that feed each other (they keep the
        // design out of the partitions and the cycle domain).
        std::vector<int> cyc;
        if (std::getenv("MSIM_WHYCYCLE")) g_combCycles = &cyc;
        std::vector<size_t> order = combSchedule(shapes, &lv, &p.cyclic);
        g_combCycles = nullptr;
        for (size_t a = 0; a < cyc.size(); a++) {
            if (cyc[a] < 0) continue;
            std::string o, in;
            for (int x : shapes[a].outs) o += S.sigs[x].name + " ";
            for (int x : shapes[a].ins) in += S.sigs[x].name + " ";
            std::cerr << "cycle " << cyc[a] << ": " << lowered.procs[procOf[a]].kind << " writes " << o << "| reads " << in << "\n";
        }
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
            if (p.rank[i] < 0 && lowered.procs[i].program < 0 && !lowered.procs[i].reactive && !upfPinned(lowered.procs[i].prog) && seqShape(lowered.procs[i].prog, p.ev[i])) p.seq[i] = 1;
    // MSIM_WHYCORO=1: the processes left as coroutines, with the kinds of
    // instruction that are neither an assign nor a jump.
    if (std::getenv("MSIM_WHYCORO"))
        for (size_t i = 0; i < n; i++) {
            if (p.rank[i] >= 0 || p.seq[i]) continue;
            const Prog& q = lowered.procs[i].prog;
            std::string w;
            int waits = 0, blk = 0, nba = 0, arr = 0, back = 0;
            std::set<int> ks;
            for (size_t pc = 0; pc < q.code.size(); pc++) {
                const Instr& x = q.code[pc];
                if (x.k == IK::WaitEdge) waits++;
                else if (x.k == IK::Assign) { blk++; if (x.arr || !x.aidx.empty() || x.bidx) arr++;
                    if (x.sig >= 0 && (size_t)x.sig < S.sigs.size()) w += " blk(" + S.sigs[(size_t)x.sig].name + ")";
                    if (x.arr) w += " arrW(" + S.sigs[x.arr->base].name + " n=" + std::to_string(x.arr->count) + ")";
                    else if (x.bidx) w += " bidx(" + S.sigs[x.sig].name + ")";
                    if (x.dyn >= 0) w += " dyn"; if (x.de || x.delay || !x.dly.empty()) w += " dly"; }
                else if (x.k == IK::Assert) w += " assert.join" + std::to_string(x.join);
                else if (x.k == IK::Call) w += " call(n=" + std::to_string(x.callee ? x.callee->code.size() : 0) + ")";
                else if (x.k == IK::Nba) { nba++; if (x.arr || !x.aidx.empty() || x.bidx) arr++; }
                else if (x.k == IK::Jmp || x.k == IK::JmpIfNot) { if (x.target <= pc && !(x.k == IK::Jmp && pc + 1 == q.code.size())) back++; }
                else ks.insert((int)x.k);
            }
            for (int k : ks) w += " ik" + std::to_string(k);
            std::cerr << "coro " << lowered.procs[i].kind << " n=" << q.code.size() << " waits=" << waits << " blk=" << blk << " nba=" << nba
                      << " arr=" << arr << " back=" << back << " roots=" << q.roots.size() << w << " : " << (q.code.size() > 2 && q.code[q.code.size() - 2].k == IK::WaitEdge ? S.sigs[q.code[q.code.size() - 2].sig].name : std::string("?")) << "\n";
        }
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
    // condition counters, then per FSM its states and transitions
    uint64_t nc = 0, nfsm = 0;
    ok = ok && rd(&nc, 1) && nc == g_condHits.size();
    for (size_t k = 0; ok && k < nc; k++) {
        uint64_t c = 0;
        ok = rd(&c, 1);
        g_condHits[k] += c;
    }
    ok = ok && rd(&nfsm, 1) && nfsm == S.fsmSigs.size();
    for (size_t k = 0; ok && k < nfsm; k++) {
        uint64_t a = 0, b = 0, r[3];
        ok = rd(&a, 1);
        for (uint64_t j = 0; ok && j < a; j++)
            if ((ok = rd(r, 2))) S.fsmSeen[k][r[0]] += r[1];
        ok = ok && rd(&b, 1);
        for (uint64_t j = 0; ok && j < b; j++)
            if ((ok = rd(r, 3))) S.fsmTrans[k][{r[0], r[1]}] += r[2];
    }
    std::fclose(f);
    return ok;
}

static int g_cfiles = 0;   // -cfiles: 0 = automatic
// msim -compile: the kernel (minisim.cpp and what it includes) compiled
// once into an object that every simv links. A generated file defines
// MINISIM_CLIENT and only declares the kernel's large functions (MSIM_KDEF),
// so a design's C++ build no longer compiles them again: a small design
// went from 21 s to 8 s. The object is kept in $MSIM_KCACHE (default
// $XDG_CACHE_HOME/msim or ~/.cache/msim), one per compiler, flags and
// kernel sources; parallel builds wait on its lock. Empty on failure.
// MSIM_NOKOBJ=1 compiles the kernel into the design as before.
static bool useKernelObject() { return !std::getenv("MSIM_NOKOBJ"); }
static std::string kernelObject(const std::string& cc, const std::string& cflags, const std::string& inc, const std::string& dir) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const std::string& t) { for (unsigned char c : t) { h ^= c; h *= 1099511628211ULL; } h ^= 0xff; h *= 1099511628211ULL; };
    mix(cc);
    mix(cflags);
    std::error_code ec;
    std::vector<std::string> srcs;
    for (auto it = std::filesystem::recursive_directory_iterator(inc, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string e = it->path().extension().string();
        if (e == ".hpp" || e == ".inc" || e == ".h" || e == ".cpp" || e == ".c") srcs.push_back(it->path().string());
    }
    std::sort(srcs.begin(), srcs.end());
    for (auto& f : srcs) {
        auto t = std::filesystem::last_write_time(f, ec);
        mix(f + " " + std::to_string(std::filesystem::file_size(f, ec)) + " " + std::to_string((long long)t.time_since_epoch().count()));
    }
    std::string cache;
    if (const char* k = std::getenv("MSIM_KCACHE"); k && *k) cache = k;
    else if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) cache = std::string(x) + "/msim";
    else if (const char* home = std::getenv("HOME"); home && *home) cache = std::string(home) + "/.cache/msim";
    std::filesystem::create_directories(cache, ec);
    if (cache.empty() || ec) { cache = dir; ec.clear(); }
    char hx[17];
    std::snprintf(hx, sizeof hx, "%016llx", (unsigned long long)h);
    std::string obj = cache + "/kernel-" + hx + ".o";
    if (std::filesystem::exists(obj, ec)) return obj;
    int fd = ::open((obj + ".lock").c_str(), O_CREAT | O_RDWR, 0644);
    if (fd >= 0) ::flock(fd, LOCK_EX);
    struct Unlock { int fd; ~Unlock() { if (fd >= 0) ::close(fd); } } unlock{fd};
    if (std::filesystem::exists(obj, ec)) return obj;   // built while this one waited
    std::string src = cache + "/kernel-" + hx + ".cpp", tmp = obj + ".tmp" + std::to_string(::getpid());
    {
        std::ofstream f(src);
        f << "// The msim -compile kernel object (msim.cpp kernelObject).\n"
             "#define MINISIM_NO_MAIN\n#define MINISIM_KERNEL_TU\n#include \"minisim.cpp\"\n";
        if (!f) return "";
    }
    std::string cmd = shq(cc) + cflags + " -c " + shq(src) + " -o " + shq(tmp);
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "msim: -compile: kernel build failed: " << cmd << "\n";
        std::filesystem::remove(tmp, ec);
        return "";
    }
    std::filesystem::rename(tmp, obj, ec);
    if (ec) return "";
    return obj;
}

static int runCompiled(const LowerResult& lowered, const std::string& dir, const std::string& cc,
                       const std::string& opt, const std::optional<std::string>& vcdPath, uint64_t maxTime,
                       const Plan& pl, int threads, bool staticPart, bool twoState, bool cycle,
                       uint64_t seed, const std::string& covPath) {
    // -upf: the power-aware runtime runs in simv too (codegen.hpp Codegen::upf);
    // the runtime sees every signal, so no private ones.
    const bool upf = upfrt::rt().on;
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
    // The domain evaluates every comb statement once, in rank order. Not with
    // -upf: every write goes through Sim::set, which keeps the UPF holds.
    cycle = cycle && !pl.cyclic && !upf;
    if (staticPart) partitionInputs(lowered, pl, parts.outs, parts.ins, parts.targets);
    std::string src;
    std::vector<std::pair<const Prog*, size_t>> covSlots;
    // Not compiled yet: final blocks, and a tick other than 1ns (`timescale).
    const char* why = !lowered.finals.empty() ? "a final block"
                    : !lowered.netDelays.empty() ? "a net delay"
                    : !lowered.netKinds.empty() ? "a wand/wor/tri0/tri1/trireg/supply net"
                    : std::any_of(asserts().begin(), asserts().end(), [](const AssertInfo& a) {
                          return a.hidden || (a.type & (AT_CONCURRENT | AT_FINAL | AT_EXPECT));
                      }) ? "a concurrent, final or expect assertion"
                    : !clockings().empty() ? "a clocking block"
                    : !cgDefs().empty() ? "a covergroup"
                    : annexDUsed() ? "an Annex D system task"
                    : !shadowPairs().empty() || !sampleClocks().empty() ? "a sampled value function" : nullptr;
    if (why) {
        std::cerr << "msim: -compile: " << why << " is not compiled yet; using the interpreter\n";
        return -1;
    }
    try {
        // With a VCD every signal must live in S.sigs, so no private signals.
        // A large design is built as several files at once, one per core (16 at most).
        src = codegen(sigs, procs, parts, twoState, cycle, !vcdPath && !upf, g_cov ? &covSlots : nullptr,
                      g_cfiles ? -g_cfiles : std::min(16, maxThreads()), lowered.precExp, lowered.timescale(), upf,
                      useKernelObject());
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
    // The kernel object: built (once, cached) while the design's files compile.
    std::string kobj;
    std::thread kthread;
    if (useKernelObject())
        kthread = std::thread([&] { kobj = kernelObject(cc, " -std=c++20 " + opt + " -I" + shq(inc) + fstC, inc, dir); });
    auto kernelReady = [&]() -> bool {
        if (kthread.joinable()) kthread.join();
        return !useKernelObject() || !kobj.empty();
    };
    struct KJoin { std::thread& t; ~KJoin() { if (t.joinable()) t.join(); } } kjoin{kthread};
    std::string stamp = build + "\n";
    std::vector<const char*> kfiles = {"minisim.cpp", "exec.inc", "expr.hpp", "prog.hpp", "cpu.hpp"};
    if (upf) kfiles.insert(kfiles.end(), {"upf_rt.inc", "upf.hpp", "upf_impl.inc", "upf_vct.hpp", "tcl.hpp"});
    for (const char* k : kfiles) {
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
        // Signal numbers resolved at link time (codegen.hpp relocText): absolute
        // symbols as displacements and immediates, so no PIE.
        const bool reloc = files[0].second.find("#define MSIM_RELOC 1") != std::string::npos;
        if (reloc) cflags += " -fno-pie";
        stamp = cflags + fstL + "\n" + stamp;
        bool sameFlags = slurp(dir + "/simv.stamp") == stamp;
        bool hdrSame = slurp(dir + "/design.hpp") == files[0].second;
        std::filesystem::remove(dir + "/simv.stamp", ec);
        std::vector<std::string> jobs, objs;
        std::vector<double> jobMB;   // the compiler's estimated memory for each job
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
            jobMB.push_back(300 + 600.0 * (text.size() + files[0].second.size()) / 1e6 * (cflags.find("-O0") == std::string::npos ? 1 : 0.5));
        }
        // Remove parts left from an earlier, larger split.
        for (int k = (int)files.size() - 2;; k++) {
            std::string old = dir + "/part" + std::to_string(k);
            if (!std::filesystem::exists(old + ".cpp", ec)) break;
            std::filesystem::remove(old + ".cpp", ec);
            std::filesystem::remove(old + ".o", ec);
        }
        // The compiler takes about 0.6 GB per MB of a large generated file:
        // four of them at once ran a 16 GB machine out of memory (16 Ibex
        // cores). Jobs start while their estimates fit in the memory free
        // now; one always runs.
        double budget = 0;
        {
            std::ifstream mi("/proc/meminfo");
            std::string k;
            double kb;
            std::string unit;
            while (mi >> k >> kb >> unit)
                if (k == "MemAvailable:") { budget = kb / 1024 * 0.85; break; }
        }
        std::atomic<size_t> next{0};
        std::atomic<bool> failed{false};
        std::vector<std::thread> pool;
        std::mutex memMu;
        std::condition_variable memCv;
        double inUse = 0;
        int running = 0;
        for (int t = 0; t < std::min<int>(maxThreads(), (int)jobs.size()); t++)
            pool.emplace_back([&] {
                for (size_t j; (j = next++) < jobs.size();) {
                    {
                        std::unique_lock<std::mutex> lk(memMu);
                        memCv.wait(lk, [&] { return budget <= 0 || running == 0 || inUse + jobMB[j] <= budget; });
                        inUse += jobMB[j];
                        running++;
                    }
                    if (std::system(jobs[j].c_str()) != 0) { failed = true; std::cerr << "msim: -compile: C++ build failed: " << jobs[j] << "\n"; }
                    {
                        std::lock_guard<std::mutex> lk(memMu);
                        inUse -= jobMB[j];
                        running--;
                    }
                    memCv.notify_all();
                }
            });
        for (auto& t : pool) t.join();
        if (failed) return 1;
        if (!kernelReady()) return 1;   // a new kernel object relinks (simv.kobj)
        if (!jobs.empty() || !std::filesystem::exists(exe, ec) || slurp(dir + "/simv.kobj") != kobj) {
            std::string link = shq(cc) + " " + opt + (reloc ? " -no-pie" : "");
            for (auto& o : objs) link += " " + shq(o);
            if (!kobj.empty()) link += " " + shq(kobj);
            link += fstL + " -o " + shq(exe);
            if (std::system(link.c_str()) != 0) { std::cerr << "msim: -compile: link failed: " << link << "\n"; return 1; }
            std::ofstream(dir + "/simv.kobj") << kobj;
        }
        std::ofstream(dir + "/simv.stamp") << stamp;
    }
    if (files.empty()) {   // one file: compiled and linked with the kernel object in one command
        if (!kernelReady()) return 1;
        if (!kobj.empty()) { build += " " + shq(kobj); stamp += "kernel " + kobj + "\n"; }
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
    if (vcdPath && S.dumpArrays) run += " -vcd-arrays " + std::to_string(S.dumpArrays);
    run += " -threads " + std::to_string(threads);
    if (g_pin >= 0) run += g_pin ? " -pin" : " -no-pin";
    // Run-time inputs go to simv, so one build serves every seed and plusarg.
    if (seed) run += " -seed " + std::to_string(seed);
    for (auto& a : plusArgs()) run += " " + shq("+" + a);
    // -upf: the bound power intent, per run (parallel runs share the build);
    // MSIM_KEEP_SNAP keeps it, to run simv by hand.
    std::string snap = dir + "/upf.snap." + std::to_string(::getpid());
    struct RmSnap { std::string p; ~RmSnap() { if (!p.empty() && !std::getenv("MSIM_KEEP_SNAP")) std::filesystem::remove(p); } } rmSnap{upf ? snap : ""};
    if (upf) {
        std::vector<const Prog*> progs;
        for (auto& lp : lowered.procs) progs.push_back(&lp.prog);
        if (!upfrt::rt().writeSnapshot(snap, progs)) { std::cerr << "msim: cannot write " << snap << "\n"; return 1; }
        run += " -upf_snap " + shq(snap);
    }
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
// Coverage access from SystemVerilog (20.13, 40.3.2): $coverage_control,
// $coverage_get_max, $coverage_get, $coverage_save and $coverage_merge.
// Statement, toggle and FSM state coverage exist only with msim -cov;
// assertion coverage (an assertion or cover with at least one success)
// always does. A hierarchy is a list of instance paths below the top.
namespace covapi {
enum { START = 0, STOP = 1, RESET = 2, CHECK = 3, MODULE = 10, HIER = 11,
       ASSERTION = 20, FSM_STATE = 21, STATEMENT = 22, TOGGLE = 23,
       OVERFLOW = -2, ERROR = -1, NOCOV = 0, OK = 1, PARTIAL = 2 };
struct Stmt { std::string scope; uint32_t file, line; std::vector<uint64_t*> hits; };
const LowerResult* lw = nullptr;
std::vector<Stmt> stmts;
bool built = false;
// Stopped items keep the values they had at the stop; START puts them back
// (what ran while stopped is not counted), queries read them meanwhile.
std::map<uint64_t*, uint64_t> stopStmt;
std::map<int, std::pair<std::vector<uint64_t>, std::vector<uint64_t>>> stopTog;
std::map<int, std::array<uint64_t, 3>> stopAssert;
std::map<size_t, std::map<uint64_t, uint64_t>> stopFsm;

void build() {
    if (built) return;
    built = true;
    std::map<std::tuple<std::string, uint32_t, uint32_t>, size_t> at;
    std::function<void(const Prog*, const std::string&, std::unordered_set<const Prog*>&)> walk =
        [&](const Prog* p, const std::string& sc, std::unordered_set<const Prog*>& seen) {
            if (!p || !seen.insert(p).second) return;
            for (auto& i : p->code) {
                if (i.line) {
                    auto key = std::make_tuple(sc, i.file, i.line);
                    auto it = at.find(key);
                    if (it == at.end()) { it = at.emplace(key, stmts.size()).first; stmts.push_back({sc, i.file, i.line, {}}); }
                    stmts[it->second].hits.push_back(const_cast<uint64_t*>(&i.hits));
                }
                for (auto* b : i.branches) walk(b, sc, seen);
            }
        };
    for (auto& p : lw->procs) { std::unordered_set<const Prog*> seen; walk(&p.prog, p.scope, seen); }
    for (auto& p : lw->finals) { std::unordered_set<const Prog*> seen; walk(&p.prog, p.scope, seen); }
}
// "top.u1.x" -> "u1": the instance below the top of a signal or assertion.
std::string instOfSig(const std::string& n) {
    int br = 0;
    size_t cut = std::string::npos;
    for (size_t k = 0; k < n.size(); k++) {
        if (n[k] == '[') br++;
        else if (n[k] == ']') br--;
        else if (n[k] == '.' && !br) cut = k;
    }
    return cut == std::string::npos ? std::string() : n.substr(0, cut);
}
std::string belowTop(const std::string& inst) {
    size_t d = inst.find('.');
    return d == std::string::npos ? std::string() : inst.substr(d + 1);
}
struct Sel {
    std::vector<std::string> paths;
    bool all = false, hier = false, bad = false;
    bool has(const std::string& sc) const {
        if (all) return hier || sc.empty();
        for (const auto& p : paths) {
            if (sc == p) return true;
            if (hier && (p.empty() || (sc.size() > p.size() && sc.compare(0, p.size(), p) == 0 && sc[p.size()] == '.'))) return true;
        }
        return false;
    }
};
Sel selOf(int64_t scopeDef, const std::string& t) {
    Sel s;
    if (scopeDef != MODULE && scopeDef != HIER) { s.bad = true; return s; }
    s.hier = scopeDef == HIER;
    if (t == "\x01") { s.bad = true; return s; }
    if (t == "*") { s.all = true; return s; }
    size_t b = 0;
    for (;;) {
        size_t e = t.find('|', b);
        s.paths.push_back(t.substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return s;
}
bool available(int64_t type) {
    if (type == ASSERTION) return true;
    if (type == STATEMENT || type == TOGGLE || type == FSM_STATE) return g_cov;
    return false;
}
bool knownType(int64_t type) { return type >= ASSERTION && type <= TOGGLE; }
// Every coverable item of the type in the selection: f(covered) per item.
// op: 0 count, 1 stop, 2 start, 3 reset.
int64_t visit(int64_t type, const Sel& sel, int op, int64_t* covered) {
    int64_t n = 0;
    if (covered) *covered = 0;
    if (type == STATEMENT) {
        build();
        for (auto& st : stmts) {
            if (!sel.has(st.scope)) continue;
            n++;
            uint64_t h = 0;
            for (uint64_t* c : st.hits) {
                auto sit = stopStmt.find(c);
                uint64_t v = sit != stopStmt.end() ? sit->second : *c;
                h = std::max(h, v);
                if (op == 1 && sit == stopStmt.end()) stopStmt[c] = *c;
                if (op == 2 && sit != stopStmt.end()) { *c = sit->second; stopStmt.erase(sit); }
                if (op == 3) { *c = 0; if (sit != stopStmt.end()) sit->second = 0; }
            }
            if (covered && h) ++*covered;
        }
    } else if (type == TOGGLE) {
        for (size_t s = 0; s < S.sigs.size(); s++) {
            const auto& g = S.sigs[s];
            if (g.hide || g.name.find('$') != std::string::npos || !sel.has(instOfSig(g.name))) continue;
            n += g.w;
            if (S.togRise.size() < S.sigs.size()) { S.togRise.resize(S.sigs.size()); S.togFall.resize(S.sigs.size()); }
            auto& R = S.togRise[s];
            auto& F = S.togFall[s];
            auto st = stopTog.find((int)s);
            if (op == 1 && st == stopTog.end()) st = stopTog.emplace((int)s, std::make_pair(R, F)).first;
            else if (op == 2 && st != stopTog.end()) { R = st->second.first; F = st->second.second; stopTog.erase(st); st = stopTog.end(); }
            else if (op == 3) {
                std::fill(R.begin(), R.end(), 0); std::fill(F.begin(), F.end(), 0);
                if (st != stopTog.end()) { st->second.first.assign(st->second.first.size(), 0); st->second.second.assign(st->second.second.size(), 0); }
            }
            if (covered) {
                const auto& r = st != stopTog.end() ? st->second.first : R;
                const auto& f = st != stopTog.end() ? st->second.second : F;
                for (size_t k = 0; k < r.size() && k < f.size(); k++) *covered += __builtin_popcountll(r[k] & f[k]);
            }
        }
    } else if (type == ASSERTION) {
        for (size_t id = 0; id < asserts().size(); id++) {
            AssertInfo& a = asserts()[id];
            if (a.hidden || a.prot || !sel.has(belowTop(a.inst))) continue;
            n++;
            auto st = stopAssert.find((int)id);
            if (op == 1 && st == stopAssert.end()) st = stopAssert.emplace((int)id, std::array<uint64_t, 3>{a.evals, a.passes, a.fails}).first;
            else if (op == 2 && st != stopAssert.end()) { a.evals = st->second[0]; a.passes = st->second[1]; a.fails = st->second[2]; stopAssert.erase(st); st = stopAssert.end(); }
            else if (op == 3) { a.evals = a.passes = a.fails = 0; if (st != stopAssert.end()) st->second = {0, 0, 0}; }
            if (covered && (st != stopAssert.end() ? st->second[1] : a.passes)) ++*covered;
        }
    } else if (type == FSM_STATE) {
        for (size_t k = 0; k < fsmDefs().size(); k++) {
            const FsmDef& d = fsmDefs()[k];
            if (!sel.has(belowTop(d.inst)) && !sel.has(d.inst)) continue;
            n += (int64_t)d.states.size();
            if (S.fsmSeen.size() <= k) continue;
            auto& seen = S.fsmSeen[k];
            auto st = stopFsm.find(k);
            if (op == 1 && st == stopFsm.end()) st = stopFsm.emplace(k, seen).first;
            else if (op == 2 && st != stopFsm.end()) { seen = st->second; stopFsm.erase(st); st = stopFsm.end(); }
            else if (op == 3) { seen.clear(); if (st != stopFsm.end()) st->second.clear(); }
            if (covered) {
                const auto& m = st != stopFsm.end() ? st->second : seen;
                for (auto& [v, nm] : d.states) if (m.count(v)) ++*covered;
            }
        }
    }
    return n;
}
// Put stopped items back as they were at the stop, before the database is written.
void restoreStopped() {
    for (auto& [c, v] : stopStmt) *c = v;
    for (auto& [s, rf] : stopTog) if ((size_t)s < S.togRise.size()) { S.togRise[(size_t)s] = rf.first; S.togFall[(size_t)s] = rf.second; }
    for (auto& [id, a] : stopAssert) { asserts()[(size_t)id].evals = a[0]; asserts()[(size_t)id].passes = a[1]; asserts()[(size_t)id].fails = a[2]; }
    for (auto& [k, m] : stopFsm) if (k < S.fsmSeen.size()) S.fsmSeen[k] = m;
}
Val ret(int64_t v) {
    if (v > INT32_MAX) v = OVERFLOW;
    return make((uint64_t)v, 0, 32, 0, true);
}
std::string dbPath(const std::string& name) { return name + ".mcovsave"; }
Val hook(int op, const std::vector<Val>& a) {
    auto num = [&](size_t k) -> int64_t { return k < a.size() && !a[k].x ? v_to_i64(a[k]) : -1000; };
    auto str = [&](size_t k) { if (k >= a.size()) return std::string(); Val v = a[k]; v.str = true; return v_str(v); };
    if (op == 0) {   // $coverage_control
        int64_t ctl = num(0), type = num(1);
        Sel sel = selOf(num(2), str(3));
        if (sel.bad || ctl < START || ctl > CHECK || !knownType(type)) return ret(ERROR);
        if (!available(type)) return ret(ctl == STOP || ctl == RESET ? OK : NOCOV);
        int64_t n = visit(type, sel, ctl == STOP ? 1 : ctl == START ? 2 : ctl == RESET ? 3 : 0, nullptr);
        if ((ctl == CHECK || ctl == START) && n == 0) return ret(NOCOV);
        return ret(OK);
    }
    if (op == 1 || op == 2) {   // $coverage_get_max / $coverage_get
        int64_t type = num(0);
        Sel sel = selOf(num(1), str(2));
        if (sel.bad || !knownType(type)) return ret(ERROR);
        if (!available(type)) return ret(NOCOV);
        int64_t cov = 0;
        int64_t n = visit(type, sel, 0, &cov);
        return ret(op == 1 ? n : cov);
    }
    int64_t type = num(0);
    std::string name = str(1);
    if (!knownType(type) || name.empty()) return ret(ERROR);
    Sel everything;
    everything.all = everything.hier = true;
    if (op == 3) {   // $coverage_save: the counts of every item of the type
        if (!available(type)) return ret(NOCOV);
        std::ofstream f(dbPath(name));
        if (!f) return ret(ERROR);
        f << "msim-covsave 1 " << type << "\n";
        if (type == STATEMENT) {
            build();
            for (auto& st : stmts) {
                uint64_t h = 0;
                for (uint64_t* c : st.hits) { auto it = stopStmt.find(c); h = std::max(h, it != stopStmt.end() ? it->second : *c); }
                f << "S " << (st.scope.empty() ? "." : st.scope) << " " << (st.file < g_covFiles.size() ? g_covFiles[st.file] : "?") << " " << st.line << " " << h << "\n";
            }
        } else if (type == TOGGLE) {
            for (size_t s = 0; s < S.sigs.size() && s < S.togRise.size(); s++) {
                auto st = stopTog.find((int)s);
                const auto& r = st != stopTog.end() ? st->second.first : S.togRise[s];
                const auto& fl = st != stopTog.end() ? st->second.second : S.togFall[s];
                if (r.empty()) continue;
                f << "T " << S.sigs[s].name;
                for (size_t k = 0; k < r.size(); k++) f << " " << r[k] << " " << (k < fl.size() ? fl[k] : 0);
                f << "\n";
            }
        } else if (type == ASSERTION) {
            for (size_t id = 0; id < asserts().size(); id++) {
                const AssertInfo& as = asserts()[id];
                if (as.hidden || as.prot) continue;
                auto st = stopAssert.find((int)id);
                f << "A " << as.name << " " << (st != stopAssert.end() ? st->second[1] : as.passes) << "\n";
            }
        } else {
            for (size_t k = 0; k < fsmDefs().size() && k < S.fsmSeen.size(); k++)
                for (auto& [v, c] : S.fsmSeen[k]) f << "F " << fsmDefs()[k].inst << "." << fsmDefs()[k].name << " " << v << " " << c << "\n";
        }
        return ret(f ? OK : ERROR);
    }
    // $coverage_merge: add the saved counts to this run's
    std::ifstream f(dbPath(name));
    std::string magic;
    int ver = 0;
    int64_t savedType = 0;
    if (!f || !(f >> magic >> ver >> savedType) || magic != "msim-covsave") return ret(ERROR);
    if (savedType != type) return ret(NOCOV);
    if (!available(type)) return ret(NOCOV);
    std::string kind, key;
    if (type == STATEMENT) {
        build();
        std::string sc, file;
        uint32_t line;
        uint64_t h;
        while (f >> kind >> sc >> file >> line >> h) {
            if (sc == ".") sc.clear();
            for (auto& st : stmts)
                if (st.scope == sc && st.line == line && st.file < g_covFiles.size() && g_covFiles[st.file] == file && !st.hits.empty())
                    *st.hits[0] += h;
        }
    } else if (type == TOGGLE) {
        std::unordered_map<std::string, size_t> byName;
        for (size_t s = 0; s < S.sigs.size(); s++) byName.emplace(S.sigs[s].name, s);
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            std::istringstream ls(line);
            if (!(ls >> kind >> key)) continue;
            auto it = byName.find(key);
            if (it == byName.end()) continue;
            size_t s = it->second;
            if (S.togRise.size() < S.sigs.size()) { S.togRise.resize(S.sigs.size()); S.togFall.resize(S.sigs.size()); }
            auto& R = S.togRise[s];
            auto& F = S.togFall[s];
            size_t words = (size_t)(S.sigs[s].w + 63) / 64;
            if (R.size() < words) { R.resize(words, 0); F.resize(words, 0); }
            uint64_t r, fl;
            for (size_t k = 0; k < words && (ls >> r >> fl); k++) { R[k] |= r; F[k] |= fl; }
        }
    } else if (type == ASSERTION) {
        uint64_t p;
        while (f >> kind >> key >> p)
            for (auto& as : asserts()) if (as.name == key) as.passes += p;
    } else {
        uint64_t v, c;
        while (f >> kind >> key >> v >> c)
            for (size_t k = 0; k < fsmDefs().size() && k < S.fsmSeen.size(); k++)
                if (fsmDefs()[k].inst + "." + fsmDefs()[k].name == key) S.fsmSeen[k][v] += c;
    }
    return ret(OK);
}
}  // namespace covapi

// -cm / -cm_hier: the code coverage written (src/cov_select.hpp).
static covsel::Selection g_covSel;

static bool writeCoverage(const std::string& path, const LowerResult& lw, const std::string& top) {
    covapi::restoreStopped();   // a coverage_control STOP still in force (40.3.2.1)
    std::vector<const Prog*> progs;
    std::unordered_set<const Prog*> seen;
    for (auto& p : lw.procs) collectProgs(&p.prog, progs, seen);
    for (auto& p : lw.finals) collectProgs(&p.prog, progs, seen);
    for (auto& p : lw.subs) collectProgs(p.get(), progs, seen);
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> lines;            // (file, line) -> hits
    std::map<std::pair<uint32_t, uint32_t>, std::vector<std::pair<uint64_t, uint64_t>>> branches;
    // The same per module instance (Prog::covInst): its own line hits, and
    // which of the decisions of a line are its own (n as in "branches").
    struct InstCov {
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> lines;
        std::vector<std::array<uint64_t, 5>> branches;       // file, line, n, true, false
        std::map<size_t, std::vector<uint64_t>> conds;       // index in "conditions" -> hits
    };
    std::vector<InstCov> insts(g_covInsts.size());
    // Selection (-cm, -cm_hier): the instance path and module of a coverage
    // instance, and whether a metric of its code in a file is written.
    auto instPath = [&](uint32_t k) {
        if (!k || k >= g_covInsts.size()) return std::string();
        return g_covInsts[k].path.empty() ? top : top + "." + g_covInsts[k].path;
    };
    const bool selAll = g_covSel.metrics == covsel::ALL && g_covSel.rules.empty();
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, bool> selCache;   // (instance, file, metric)
    auto sel = [&](uint32_t m, uint32_t inst, uint32_t file) {
        if (selAll) return true;
        auto [it, fresh] = selCache.try_emplace({inst, file, m}, false);
        if (fresh)
            it->second = g_covSel.selected(m, instPath(inst), inst < g_covInsts.size() ? g_covInsts[inst].module : "",
                                           file < g_covFiles.size() ? g_covFiles[file] : "");
        return it->second;
    };
    for (const Prog* p : progs) {
        InstCov* ic = p->covInst && p->covInst < insts.size() ? &insts[p->covInst] : nullptr;
        for (auto& i : p->code) {
            if (!i.line) continue;
            auto key = std::make_pair(i.file, i.line);
            if (sel(covsel::LINE, p->covInst, i.file)) {
                auto& h = lines[key];
                h = std::max(h, i.hits);
                if (ic) {
                    auto& ih = ic->lines[key];
                    ih = std::max(ih, i.hits);
                }
            }
            // A JmpIfNot is a two-way decision: true = fell through, false = jumped.
            // n counts every decision of the line, so that it is the same for any selection.
            if (i.k == IK::JmpIfNot) {
                auto& v = branches[key];
                bool on = sel(covsel::BRANCH, p->covInst, i.file);
                if (ic && on) ic->branches.push_back({i.file, i.line, v.size(), i.hits - i.taken, i.taken});
                v.push_back({on ? i.hits - i.taken : UINT64_MAX, i.taken});
            }
        }
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
            if (v[n].first == UINT64_MAX) continue;   // not selected
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
    // A signal's instance: the longest prefix of its name that is one.
    std::unordered_map<std::string, uint32_t> instByPath;
    if (!selAll)
        for (uint32_t k = 1; k < g_covInsts.size(); k++) instByPath.emplace(instPath(k), k);
    auto sigSelected = [&](const std::string& full) {
        if (selAll) return true;
        std::string_view n = full;
        while (true) {
            size_t d = n.rfind('.');
            if (d == std::string_view::npos) break;
            n = n.substr(0, d);
            if (auto it = instByPath.find(std::string(n)); it != instByPath.end()) return sel(covsel::TGL, it->second, 0);
        }
        return g_covSel.selected(covsel::TGL, n, "");
    };
    for (size_t sidx = 0; sidx < S.sigs.size(); sidx++) {
        const auto& g = S.sigs[sidx];
        if (g.hide) continue;
        if (!sigSelected(top + "." + g.name)) continue;
        static const std::vector<uint64_t> none;
        const auto& R = sidx < S.togRise.size() ? S.togRise[sidx] : none;
        const auto& F = sidx < S.togFall.size() ? S.togFall[sidx] : none;
        f << (first ? "\n  " : ",\n  ") << "[" << jstr(top + "." + g.name) << ", " << g.w << ", \"" << hex(R, g.w) << "\", \"" << hex(F, g.w) << "\"]";
        first = false;
    }
    // Conditions (a && b, a || b, the condition of ?:): per source position,
    // the hits of each vector, summed over the instances of the module.
    struct CondSum { const CondDef* d; std::vector<uint64_t> hits; size_t index = 0; };
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, std::string>, CondSum> conds;
    for (auto& d : condDefs()) {
        if (!sel(covsel::COND, d.inst, d.file)) continue;
        auto& c = conds[{d.file, d.line, d.col, d.text}];
        if (!c.d) { c.d = &d; c.hits.assign(d.counters(), 0); }
        if (c.d->counters() != d.counters()) continue;
        for (size_t k = 0; k < d.counters(); k++) c.hits[k] += g_condHits[d.base + k];
    }
    f << "\n ],\n \"conditions\": [";
    first = true;
    for (auto& [key, c] : conds) {
        f << (first ? "\n  " : ",\n  ") << "[" << c.d->file << ", " << c.d->line << ", " << c.d->col << ", "
          << (c.d->terms.size() == 1 ? std::string("\"?:\"")
              : "\"" + std::string(c.d->bitwise ? (c.d->isOr ? "|" : "&") : (c.d->isOr ? "||" : "&&")) +
                    (c.d->lazy ? "s" : c.d->full ? "f" : "") + "\"")
          << ", " << jstr(c.d->text) << ", [";
        for (size_t k = 0; k < c.d->terms.size(); k++) f << (k ? ", " : "") << jstr(c.d->terms[k]);
        f << "], [";
        for (size_t k = 0; k < c.hits.size(); k++) f << (k ? ", " : "") << c.hits[k];
        f << "]]";
        first = false;
    }
    // FSMs: per state register, its states and transitions with their hits;
    // "extra" lists changes seen that the next-state logic does not show.
    S.fsmFinish();
    f << "\n ],\n \"fsms\": [";
    first = true;
    for (size_t k = 0; k < fsmDefs().size() && k < S.fsmSeen.size(); k++) {
        const FsmDef& d = fsmDefs()[k];
        if (!selAll && !g_covSel.selected(covsel::FSM, d.inst, d.module, d.file < g_covFiles.size() ? g_covFiles[d.file] : ""))
            continue;
        const auto& seen = S.fsmSeen[k];
        auto trans = S.fsmTrans[k];
        f << (first ? "\n  " : ",\n  ") << "{\"module\": " << jstr(d.module) << ", \"inst\": " << jstr(d.inst) << ", \"name\": "
          << jstr(d.name) << ", \"file\": " << d.file << ", \"line\": " << d.line << ", \"width\": " << d.w << ",\n   \"states\": [";
        for (size_t j = 0; j < d.states.size(); j++) {
            auto it = seen.find(d.states[j].first);
            f << (j ? ", " : "") << "[" << d.states[j].first << ", " << jstr(d.states[j].second) << ", "
              << (it == seen.end() ? 0 : it->second) << "]";
        }
        f << "],\n   \"trans\": [";
        for (size_t j = 0; j < d.trans.size(); j++) {
            auto it = trans.find({d.trans[j].from, d.trans[j].to});
            f << (j ? ", " : "") << "[" << d.trans[j].from << ", " << d.trans[j].to << ", " << (it == trans.end() ? 0 : it->second)
              << ", " << (d.trans[j].reset ? 1 : 0) << "]";
            if (it != trans.end()) trans.erase(it);
        }
        f << "],\n   \"extra\": [";
        bool f2 = true;
        for (auto& [t, c] : trans) {
            f << (f2 ? "" : ", ") << "[" << t.first << ", " << t.second << ", " << c << "]";
            f2 = false;
        }
        f << "]}";
        first = false;
    }
    // Per instance: [path, module, lines [file, line, hits], branches [file,
    // line, n, true, false], conditions [index in "conditions", hits]].
    {
        size_t ci = 0;
        for (auto& [key, c] : conds) c.index = ci++;
        for (auto& d : condDefs()) {
            if (!d.inst || d.inst >= insts.size() || !sel(covsel::COND, d.inst, d.file)) continue;
            const CondSum& c = conds[{d.file, d.line, d.col, d.text}];
            if (d.counters() != c.d->counters()) continue;
            auto& h = insts[d.inst].conds[c.index];
            h.resize(d.counters(), 0);
            for (size_t k = 0; k < d.counters(); k++) h[k] += g_condHits[d.base + k];
        }
    }
    f << "\n ],\n \"instances\": [";
    first = true;
    for (size_t k = 1; k < insts.size(); k++) {
        const InstCov& ic = insts[k];
        f << (first ? "\n  " : ",\n  ") << "[" << jstr(g_covInsts[k].path.empty() ? top : top + "." + g_covInsts[k].path) << ", "
          << jstr(g_covInsts[k].module) << ", [";
        bool f2 = true;
        for (auto& [key, h] : ic.lines) {
            f << (f2 ? "" : ", ") << "[" << key.first << ", " << key.second << ", " << h << "]";
            f2 = false;
        }
        f << "], [";
        for (size_t j = 0; j < ic.branches.size(); j++) {
            auto& b = ic.branches[j];
            f << (j ? ", " : "") << "[" << b[0] << ", " << b[1] << ", " << b[2] << ", " << b[3] << ", " << b[4] << "]";
        }
        f << "], [";
        f2 = true;
        for (auto& [ci, h] : ic.conds) {
            f << (f2 ? "" : ", ") << "[" << ci << ", [";
            for (size_t j = 0; j < h.size(); j++) f << (j ? ", " : "") << h[j];
            f << "]]";
            f2 = false;
        }
        f << "]]";
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

// -saif with -saif_forward (IEEE 1801-2024 Annex J): an RTL forward file
// keeps only its mapped points (J.5); a library one adds the state- and
// path-dependent activity of each instance of its cells (J.3.6 to J.3.10, J.4).
static void writeForwardSaif(const saif::Db& db, const wave::Reader& wr, const std::vector<std::string>& files,
                             std::ostream& f) {
    saif::Forward lib, rtl;
    lib.lib = true;
    for (auto& p : files) {
        saif::Forward fw = saif::readForward(p);
        if (fw.lib) {
            for (auto& [n, c] : fw.cells) lib.cells[n] = std::move(c);
            lib.path = p;
        } else {
            if (rtl.path.empty()) rtl.path = p;
            for (auto& in : fw.insts) rtl.insts.push_back(std::move(in));
        }
    }
    if (!rtl.path.empty()) {
        if (!lib.cells.empty())
            std::cerr << "msim: -saif_forward: the RTL mapping of " << rtl.path
                      << " is written; library cells are not mapped points, so " << lib.path << " is not used\n";
        std::vector<std::string> warn;
        saif::rtlBackward(db, rtl, f, S.vcdTop, "msim", MINISIM_VERSION, warn);
        for (auto& w : warn) std::cerr << "msim: -saif_forward: " << w << "\n";
        return;
    }
    std::vector<saif::CellInst> insts;
    for (auto& [path, mod] : instCells()) {
        auto it = lib.cells.find(mod);
        if (it == lib.cells.end()) continue;
        saif::CellInst ci;
        ci.path = path;
        ci.cell = &it->second;
        std::string pre = path + ".";
        for (auto pi = portRecs().lower_bound(pre); pi != portRecs().end() && pi->first.compare(0, pre.size(), pre) == 0; ++pi) {
            std::string pin = pi->first.substr(pre.size());
            if (pin.find('.') == std::string::npos) ci.output[pin] = pi->second.dir == 1;
        }
        insts.push_back(std::move(ci));
    }
    std::vector<saif::CellActivity> acts(insts.size());
    uint64_t end = std::max<uint64_t>(wr.end, db.start);
    par::parallelFor(insts.size(), par::defaultThreads(), [&](size_t i) {
        acts[i] = saif::cellActivity(insts[i], wr, db.start, end);
    });
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < insts.size(); i++) at[insts[i].path] = i;
    saif::WriteHooks h;
    h.cell = [&](const std::string& p) {
        auto it = at.find(p);
        return it == at.end() ? std::string() : insts[it->second].cell->name;
    };
    h.isPort = [&](const std::string& p, const std::string& net) {
        auto it = at.find(p);
        if (it == at.end()) return false;
        size_t br = net.find('[');
        return insts[it->second].output.count(br == std::string::npos ? net : net.substr(0, br)) > 0;
    };
    h.pin = [&](const std::string& p, const std::string& pin, std::ostream& os, const std::string& ind) {
        size_t i = at.at(p);
        auto& ports = insts[i].cell->ports;
        auto pd = std::find_if(ports.begin(), ports.end(), [&](auto& x) { return x.first == pin; });
        if (pd != ports.end()) saif::writePortDirectives(os, pd->second, acts[i].ports.at(pin), ind);
    };
    h.extra = [&](const std::string& p, std::ostream& os, const std::string& ind) {
        auto it = at.find(p);
        if (it != at.end()) saif::writeLeakage(os, *insts[it->second].cell, acts[it->second], ind);
    };
    saif::write(db, f, S.vcdTop, "msim", MINISIM_VERSION, &h);
}

static void usage() {
    std::cerr << "usage: msim [options] <files...>\n"
                 "  -top <name>    top-level module (repeatable)\n"
                 "  -vcd [<file>]  write a VCD waveform (default dump.vcd); a .fst name writes FST\n"
                 "  -saif [<file>] write switching activity as SAIF (default dump.saif), for mpower\n"
                 "  -restore <file> go back to a $save checkpoint (first option; also -r)\n"
                 "  -xrun ...      the rest is an xrun / irun command line (also when run as xrun or irun)\n"
                 "  -elab          stop after elaboration, before the run\n"
                 "  -profile [<file>] write where simulation time goes (default simprofile.txt)\n"
                 "  -saif_forward <file>  forward SAIF for -saif: library (SDPD) or RTL mapping\n"
                 "  +notimingcheck turn off timing checks ($setup, $hold, ...)\n"
                 "  -sdf [min|typ|max:]<inst>:<file>  annotate an SDF file before time 0\n"
                 "  +mindelays / +typdelays / +maxdelays  pick from min:typ:max delays\n"
                 "  -libmap <file>  library map (33.3); -L <lib> library search order\n"
                 "  +neg_tchk      allow negative $setuphold / $recrem limits, with delayed signals\n"
                 "  +pulse_r/<n> +pulse_e/<n>  pulse reject / error limits as n% of path delays\n"
                 "  +pulse_on_event / +pulse_on_detect, +showcancelled / +noshowcancelled  pulse styles\n"
                 "  -fst [<file>]  write an FST waveform (default dump.fst)\n"
                 "  -vcd-arrays [N]  also write unpacked array elements (memories) of up to N\n"
                 "                 elements each (default 1024) to the VCD or FST, as mem[3]\n"
                 "  -vcd-class [N]  also write the properties of the object each class handle\n"
                 "                 variable points to, as h.addr (x while h is null); handle\n"
                 "                 properties are followed N levels (default 1)\n"
                 "  -tr FILE       where $begin_transaction / $fsdbTrans_begin record their\n"
                 "                 transactions (default transactions.log; read with mdebug -tr)\n"
                 "  -vcd-sva       also write how many times each assertion and cover has\n"
                 "                 passed and failed, as a.passes and a.fails\n"
                 "  -vcd-power     with -upf: also write each power domain's simstate and each\n"
                 "                 supply net's state and voltage, as upf.PD.simstate, upf.VDD.mV\n"
                 "  -cov [<file>]  write line / branch / toggle / condition / FSM coverage (default cov.json);\n"
                 "                 see mcov\n"
                 "  -cm <metrics>  the same, with only these code coverage metrics: line+branch+cond+tgl+fsm\n"
                 "                 (as VCS), b:e:t:f or all (as Xcelium -coverage), bcesftx (as Questa\n"
                 "                 +cover); covergroups and assertions are always written\n"
                 "  -cm_hier <f>   the part of the design whose code coverage is written: +tree/-tree\n"
                 "                 <inst> [depth], +module/-module, +file/-file (as VCS -cm_hier), or\n"
                 "                 select_coverage / deselect_coverage commands (as an Xcelium covfile)\n"
                 "  -cm_cond <m>   condition coverage modes, joined by +: full (every combination of the\n"
                 "                 term values, up to 8 terms), allops (also & and | of one-bit operands)\n"
                 "  -maxtime <n>   stop after simulation time n (ticks, or with a unit: 3ms)\n"
                 "  -i             interactive: run, step, stop on a signal, print (signals and class objects),\n"
                 "                 force, release, deposit; commands on the standard input (help lists them)\n"
                 "  -do <file>     the same, with the commands from a file; then the run goes on to its end\n"
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
                 "  -xprop=<file>, -xprop-config <file>  X-propagation set per instance or module\n"
                 "                 (tree / instance / module {names} {xpropOn|xpropOff}; merge = ...)\n"
                 "  -xprop-report <file>  list every if / case / edge and whether it is merged\n"
                 "  -upf <file>    power-aware simulation with IEEE 1801 (UPF) power intent\n"
                 "  -power_top <i> the instance the UPF describes (default: set_design_top, else the top)\n"
                 "  -upf_verbose   print supply, power state and simstate changes\n"
                 "  -upf_cov <file>  low-power coverage report: power states, transitions, isolation, retention\n"
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
    annexStartup(argc, argv);   // $reset / $restart run msim again (Annex D)
    std::atexit([] { g_framesDead = true; });   // before the kernel's waiting processes are destroyed
    for (int sg : {SIGSEGV, SIGABRT})
        std::signal(sg, [](int) {   // say where, then stop
            std::string w = "internal error: crash" + runLocStr() + "\n";
            (void)!write(2, w.data(), w.size());
            _exit(139);
        });
    // Siemens vlib / vmap / vlog / vcom / vsim and Synopsys vlogan / vhdlan /
    // vcs / simv (mstep.inc): the analysis steps run there; vsim and simv
    // become an msim command line on the libraries' sources.
    mstep::Opts mo;
    std::vector<char*> margv;
    if (mstep::wanted(argc, argv, mo)) {
        if (mo.exitNow >= 0) return mo.exitNow;
        for (auto& s : mo.out) margv.push_back(s.data());
        margv.push_back(nullptr);
        argc = (int)margv.size() - 1;
        argv = margv.data();
        if (!mo.log.empty()) {
            xrun::Opts lo;
            lo.tool = mo.name;
            lo.log = mo.log;
            lo.appendLog = mo.appendLog;
            xrun::teeLog(lo);
        }
    }
    // xrun / irun command line (xrun.inc): translated to msim's options.
    xrun::Opts xo;
    std::vector<char*> xargv;
    if (xrun::wanted(argc, argv, xo)) {
        for (auto& s : xo.out) xargv.push_back(s.data());
        if (!xo.tclIn.empty()) {   // -input run.tcl: msim -do runs Tcl with the xrun commands (interactive.inc)
            if (!std::ifstream(xo.tclIn)) { std::cerr << xo.tool << ": cannot read " << xo.tclIn << "\n"; return 2; }
            static std::string doOpt = "-do";
            xargv.push_back(doOpt.data());
            xargv.push_back(xo.tclIn.data());
        }
        xargv.push_back(nullptr);
        argc = (int)xargv.size() - 1;
        argv = xargv.data();
        xrun::teeLog(xo);
    }
    // Pass 1: take msim's own options out; translate VCS spellings for slang.
    std::vector<std::string> fwd{argv[0]};
    std::vector<std::string> plusargs;
    uint64_t seed = 0;
    std::optional<std::string> vcdPath;
    int vcdClass = 0;   // -vcd-class [N]
    bool vcdSva = false, vcdPower = false;   // -vcd-sva, -vcd-power
    std::string saifPath;   // -saif: the waveform is summed into a SAIF file after the run
    bool saifTmp = false;   // ...and the waveform was only written for that
    std::vector<std::string> saifForward;   // -saif_forward (IEEE 1801-2024 Annex J)
    std::string covPath = "cov.json";
    uint64_t maxTime = ~0ULL;
    double maxTimeVal = 0;   // -maxtime with a unit: converted once the time precision is known
    std::optional<int> maxTimeExp;
    bool interactiveMode = false;   // -i / -do FILE
    std::string doPath;
    bool elabOnly = false;   // -elab: stop after elaboration (xrun -elaborate)
    bool parseOnly = false;  // -parse: stop after parsing (the analysis steps of mstep.inc)
    bool compiled = false, staticComb = true, staticSeq = true, staticPart = true, twoState = true, cycle = true;
    int threads = defaultThreads();
    bool threadsGiven = false;
    std::string buildDir = "msim.d", opt = "-O2";
    std::string cc = std::getenv("CXX") ? std::getenv("CXX") : "c++";
    protfront::Options prot;
    std::string upfPath, powerTop;
    std::string xpropReport;
    bool powerTopSet = false, upfVerbose = false;
    std::string upfCov;   // -upf_cov: the low-power coverage report
    std::vector<std::string> svLibs, cFiles, soFiles;
    std::vector<std::string> vhdlFiles;
    std::string vhdlSv;
    bool vhdl4 = false;   // -vhdl4: VHDL std_logic with four values (faster; see vhdl::Translator::nine)
    std::vector<std::pair<std::string, std::string>> vpiLibs;   // -load lib:boot
    std::string svRoot, dpiHeader, cflags, ldflags;
    std::string uvmHome;    // -uvm / -uvmhome: a uvm-core checkout
    bool uvmWanted = false, uvmNoDpi = false;
    bool buildDirGiven = false;
    bool partcompNoted = false;
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
        if (a == "-elab") { elabOnly = true; continue; }
        if (a == "-parse") { parseOnly = true; continue; }
        if (a == "-i" || a == "-ucli") { interactiveMode = true; continue; }
        if (a == "-do") { doPath = next("a command file"); interactiveMode = true; continue; }
        if (a == "-no-static-comb") { staticComb = false; continue; }
        if (a == "-no-static-seq") { staticSeq = false; continue; }
        if (a == "-no-partition") { staticPart = false; continue; }
        if (a == "-no-2state") { twoState = false; continue; }
        if (a == "-no-cycle") { cycle = false; continue; }
        if (a == "-xprop" || a == "-xprop=tmerge") { Lowerer::xpropMode() = 1; continue; }
        if (a == "-xprop=xmerge") { Lowerer::xpropMode() = 2; continue; }
        if (a == "-xprop=off" || a == "-xprop=vmerge") { Lowerer::xpropMode() = 0; continue; }
        if (a.rfind("-xprop=", 0) == 0 || a == "-xprop-config") {   // a configuration file (docs/XPROP.md)
            std::string cfg = a == "-xprop-config" ? next("a file name") : a.substr(7);
            if (a != "-xprop-config" && !std::ifstream(cfg)) { std::cerr << "msim: bad " << a << " (tmerge, xmerge, off or a configuration file)\n"; return 2; }
            std::string err = Lowerer::xpReadConfig(cfg);
            if (!err.empty()) { std::cerr << "msim: -xprop configuration " << cfg << ": " << err << "\n"; return 2; }
            continue;
        }
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
        if (a == "-upf_cov") { upfCov = next("a coverage report file"); continue; }
        if (a == "-cc") { cc = next("a compiler"); continue; }
        // VCS partition compile (docs/COMPAT.md): compiled mode. msim chooses
        // its own parts, the generated C++ files; an unchanged one is not
        // rebuilt, and the build directory keeps them between runs.
        if (a == "-partcomp" || startsWith(a, "-partcomp=") || a == "-fastpartcomp" || startsWith(a, "-fastpartcomp=")) {
            compiled = true;
            if (startsWith(a, "-fastpartcomp=j") && std::isdigit((unsigned char)a[15]))
                g_cfiles = std::max(1, std::atoi(a.c_str() + 15));
            if (!partcompNoted) {
                std::cerr << "msim: " << a << ": compiled mode; msim chooses its own parts, and an unchanged part is not rebuilt\n";
                partcompNoted = true;
            }
            continue;
        }
        if (startsWith(a, "-partcomp_dir=") || startsWith(a, "-Mdir=")) {
            buildDir = a.substr(a.find('=') + 1);
            buildDirGiven = true;
            continue;
        }
        if (a == "-partcomp_sharedlib" || a == "-pcmakeprof") continue;   // msim's parts are always linked into one simv
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
        if (a == "-saif_forward" && i + 1 < argc) {
            saifForward.push_back(argv[++i]);
            continue;
        }
        if (a == "-profile") {   // profile.hpp
            prof::P().on = true;
            if (i + 1 < argc && std::string_view(argv[i + 1]).ends_with(".txt")) prof::P().path = argv[++i];
            continue;
        }
        if (a == "-cov") {
            g_cov = true;
            if (i + 1 < argc && std::string_view(argv[i + 1]).ends_with(".json")) covPath = argv[++i];
            continue;
        }
        // -cm <metrics>: the code coverage metrics written; VCS line+cond+fsm+tgl+branch,
        // Xcelium b:e:t:f:u:a or all, Questa bcesftx (src/cov_select.hpp). Covergroups
        // and assertions are always written.
        if ((a == "-cm" && i + 1 < argc) || a.starts_with("-cm=")) {
            g_cov = true;
            std::string err;
            std::string_view v = a == "-cm" ? std::string_view(argv[++i]) : a.substr(4);
            if (!covsel::parseMetrics(v, g_covSel.metrics, err)) {
                std::cerr << "msim: -cm: " << err << "\n";
                return 2;
            }
            continue;
        }
        // -cm_hier <file>: the part of the design whose code coverage is written: VCS
        // +tree/-tree/+module/-module/+file/-file lines or Xcelium covfile commands.
        if (a == "-cm_hier" && i + 1 < argc) {
            std::string err;
            if (!g_covSel.read(argv[++i], err)) {
                std::cerr << "msim: -cm_hier: " << err << "\n";
                return 2;
            }
            for (auto& n : g_covSel.notes) std::cerr << "msim: " << n << "\n";
            g_covSel.notes.clear();
            if (g_covSel.allops) g_condAllops = true;
            continue;
        }
        if (a == "-cm_cond" && i + 1 < argc) {   // VCS: -cm_cond full+allops (std / basic: the default)
            std::string_view v = argv[++i];
            while (!v.empty()) {
                size_t p = v.find('+');
                std::string_view w = v.substr(0, p);
                if (w == "full") g_condFull = true;
                else if (w == "allops") g_condAllops = true;
                else if (w != "std" && w != "basic") {
                    std::cerr << "msim: -cm_cond: unknown mode '" << w << "' (full, allops, std)\n";
                    return 2;
                }
                v = p == std::string_view::npos ? std::string_view() : v.substr(p + 1);
            }
            continue;
        }
        // -vcd-class [N]: class objects through handle variables, N levels deep (default 1).
        if (a == "-vcd-class" || a == "-fst-class") {
            vcdClass = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                vcdClass = std::max(1, std::atoi(argv[++i]));
            continue;
        }
        // -tr FILE: where $begin_transaction / $fsdbTrans_begin record (default transactions.log).
        if (a == "-tr" && i + 1 < argc) { trrec::path() = argv[++i]; setenv("MSIM_TR", argv[i], 1); continue; }
        // -vcd-sva / -vcd-power: assertion counts, power states and supplies as signals (statview.inc).
        if (a == "-vcd-sva" || a == "-fst-sva") { vcdSva = true; continue; }
        if (a == "-vcd-power" || a == "-fst-power") { vcdPower = true; continue; }
        // -vcd-arrays [N]: memories of up to N elements (default 1024) in the waveform.
        if (a == "-vcd-arrays" || a == "-fst-arrays") {
            S.dumpArrays = 1024;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                S.dumpArrays = std::strtoull(argv[++i], nullptr, 10);
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
            // a number of time-precision ticks, or with a unit: 3ms, 500ns, 1.5us
            try {
                size_t used = 0;
                double val = std::stod(n, &used);
                std::string u = n.substr(used);
                static const std::pair<const char*, int> units[] = {{"fs", -15}, {"ps", -12}, {"ns", -9}, {"us", -6}, {"ms", -3}, {"s", 0}};
                if (u.empty()) maxTime = std::stoull(n);
                else {
                    auto it = std::find_if(std::begin(units), std::end(units), [&](auto& x) { return u == x.first; });
                    if (it == std::end(units) || val < 0) throw std::invalid_argument(n);
                    maxTimeVal = val;
                    maxTimeExp = it->second;
                }
            }
            catch (...) { std::cerr << "msim: bad -maxtime " << n << " (a number, or with fs, ps, ns, us, ms or s)\n"; return 2; }
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
        // VHDL sources (docs/VHDL.md): translated for the same engine after slang reads the SV files
        if (a.size() && a[0] != '-' && vhdl::isVhdlFile(a)) { vhdlFiles.push_back(a); continue; }
        if (a == "-vhdl-sv") { vhdlSv = next("a file name"); continue; }
        if (a == "-vhdl4") { vhdl4 = true; continue; }
        if (a == "-v93" || a == "-v2008" || a == "-vhdl2008" || a == "-v200x" || a == "-vhdl93" || a == "-vhdl87") continue;
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
    // VHDL only: slang wants at least one SystemVerilog source; an empty one stands in
    std::string vhdlStub;
    if (!vhdlFiles.empty()) {
        char tmpl[] = "/tmp/msim_vhdlXXXXXX.sv";
        int fd = mkstemps(tmpl, 3);
        if (fd < 0) { std::cerr << "msim: cannot write a temporary file\n"; return 1; }
        close(fd);
        vhdlStub = tmpl;
        fwd.push_back(vhdlStub);
    }
    struct RmStub { std::string f; ~RmStub() { if (!f.empty()) std::remove(f.c_str()); } } rmStub{vhdlStub};
    // The language is IEEE 1800-2023 unless --std says otherwise (slang's
    // default is 1800-2017, which rejects e.g. weak_reference, 8.30).
    if (!saifForward.empty() && saifPath.empty()) saifPath = "dump.saif";   // -saif_forward implies -saif
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
            wave::Reader wr;
            saif::load(*vcdPath, db, par::defaultThreads(), saifForward.empty() ? nullptr : &wr);
            std::ofstream f(saifPath);
            if (!f) throw saif::Error("cannot write " + saifPath);
            if (saifForward.empty()) {
                saif::write(db, f, S.vcdTop, "msim", MINISIM_VERSION);
            } else {
                writeForwardSaif(db, wr, saifForward, f);
            }
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
    if (!vhdlFiles.empty()) {
        vhdl::Translator vtr;
        vtr.nine = !vhdl4;
        if (!vhdl::addVhdl(driver, vhdlFiles, vhdlSv, "msim", vtr)) return 1;
    }
    if (parseOnly) return driver.reportParseDiags() ? 0 : 1;
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
    vendor::declare(*comp);   // $fsdbDumpvars, $vcdpluson, $shm_probe, ... (after VPI: a real PLI library wins)
    driver.reportCompilation(*comp, /*quiet*/ true);
    if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;

    // The VCD's root scope is the top module (several tops: "top").
    if (auto tops = comp->getRoot().topInstances; tops.size() == 1) S.vcdTop = std::string(tops[0]->name);

    // -upf: the power intent is read now; its bind_checker commands (IEEE
    // 1801-2024 6.12) become bind directives, and the design is elaborated
    // again with them.
    if (!upfPath.empty()) {
        auto& rt = upfrt::rt();
        try {
            upf::DesignView dv = upf::designView(*comp);
            upf::loadWithDesign(rt.m, upfPath, *comp, dv, powerTop, powerTopSet);
            rt.lrec.attrs = upfHdlAttrs(*comp);
            rt.m.addHdlAttrs(rt.lrec.attrs);   // (* UPF_... *) in the HDL (5.6)
            rt.lrec.path = upfPath;   // msim -compile: simv reads the file again (Rt::readSnapshot)
            rt.lrec.topFixed = powerTopSet;
            rt.lrec.dv = dv;
            // the simulation top: a checker module nothing instantiates yet is a top too
            std::string simTop = S.vcdTop;
            {
                std::set<std::string> checkers;
                for (auto& b : rt.m.bindCheckers) checkers.insert(b.module);
                for (auto& [n, v] : rt.m.vcms)   // and so is a model VCM's module (6.30.9)
                    if (v.kind == upf::Vcm::Model) checkers.insert(v.model.substr(v.model.rfind('.') == std::string::npos ? 0 : v.model.rfind('.') + 1));
                for (auto& rm : rt.m.retModels)   // and a retention model (6.39)
                    checkers.insert(rm.model.substr(rm.model.rfind('.') == std::string::npos ? 0 : rm.model.rfind('.') + 1));
                for (auto& im : rt.m.ifcModels)   // and an isolation model (6.62)
                    checkers.insert(im.model.substr(im.model.rfind('.') == std::string::npos ? 0 : im.model.rfind('.') + 1));
                std::vector<std::string> real;
                for (auto* t : comp->getRoot().topInstances)
                    if (!checkers.count(std::string(t->getDefinition().name))) real.push_back(std::string(t->name));
                if (real.size() == 1) simTop = real[0];
            }
            // function and model VCMs (6.30.8, 6.30.9) are placed in the design too
            std::string vcmText = rt.m.connectVcm.empty() ? std::string() : rt.vcmSource(*comp, simTop);
            vcmText += rt.m.retModels.empty() ? std::string() : rt.retModelSource(*comp, simTop);   // map_retention_cell -lib_model_name (6.39)
            vcmText += rt.m.ifcModels.empty() ? std::string() : rt.ifcModelSource(*comp, simTop);   // use_interface_cell -force_function (6.62)
            if (!rt.m.bindCheckers.empty() || !vcmText.empty()) {
                static std::string bindText;
                bindText = (rt.m.bindCheckers.empty() ? std::string() : rt.m.bindSource(simTop)) + vcmText;
                if (upfVerbose) std::cout << "UPF: " << bindText;
                driver.syntaxTrees.push_back(slang::syntax::SyntaxTree::fromText(bindText, driver.sourceManager, "upf_bind_checker.sv", "",
                                                                                 driver.createOptionBag()));
                comp = driver.createCompilation();
                if (vpi::st().active) vpi::declare(*comp);
                vendor::declare(*comp);
                driver.reportCompilation(*comp, /*quiet*/ true);
                if (!driver.reportDiagnostics(/*quiet*/ true)) return 1;
                if (auto tops = comp->getRoot().topInstances; tops.size() == 1) S.vcdTop = std::string(tops[0]->name);
            }
        } catch (const std::exception& ex) {
            std::cerr << "msim: UPF: " << ex.what() << "\n";
            return 1;
        }
    }

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
    if (maxTimeExp) maxTime = (uint64_t)std::llround(maxTimeVal * std::pow(10.0, *maxTimeExp - lowered.precExp));
    if (elabOnly) { std::cout << "msim: elaborated " << lowered.procs.size() << " processes; not run (-elab)\n"; return 0; }
    if (g_cov) {   // FSM coverage: the kernel records the state registers' values
        std::vector<int> watch;
        std::vector<char> hot;
        for (auto& d : fsmDefs()) { watch.push_back(d.sig); hot.push_back(d.onehot); }
        S.fsmWatch(watch, hot);
    }
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
        rt.covPath = upfCov;
        try {
            rt.m.resolveApplies([](const std::string& mod) {   // apply_power_model without -elements (6.9)
                std::vector<std::string> r;
                std::string pre = S.vcdTop.empty() ? "" : S.vcdTop + ".";
                for (auto& [full, m] : instCells())
                    if (m == mod && full.compare(0, pre.size(), pre) == 0) r.push_back(full.substr(pre.size()));
                upfrt::rt().lrec.applies[mod] = r;
                return r;
            });
            rt.bind(lowered);
        } catch (const std::exception& ex) {
            std::cerr << "msim: UPF: " << ex.what() << "\n";
            return 1;
        }
        // Power gating keeps the processes it can gate, and those that run
        // UPF code, on the interpreter (upfPinned); the rest is planned and
        // runs on several threads. MSIM_UPF_SERIAL=1 runs it all serially.
        if (std::getenv("MSIM_UPF_SERIAL")) {
            staticComb = staticSeq = staticPart = false;
            threads = 1;
        }
        // -compile: simv runs the same runtime from a snapshot of it (upf_rt.inc).
        if (std::string gap = rt.simvGap(); compiled && !gap.empty()) {
            std::cerr << "msim: -compile: " << gap << " in the power intent is not compiled yet; using the interpreter\n";
            compiled = false;
        }
    }
    if (usesForce()) staticPart = false;   // a forced signal must see every write (10.6)
    if (lowered.procs.empty()) {
        // Nothing to schedule: the simulation ends at time 0 (4.4.1).
        std::cerr << "msim: note: design has no processes; simulation ends at time 0\n";
        return 0;
    }

    for (int id : hiddenSigs()) S.sigs[(size_t)id].hide = true;
    // With protected IP in the design, the lowering's temporaries (_tf...)
    // stay out of the waveform: one may hold a value computed inside the IP
    // (an argument of the IP's own system task call).
    if (hideSigIf())
        for (auto& g : S.sigs) {
            size_t dot = g.name.rfind('.');
            if (g.name.compare(dot == std::string::npos ? 0 : dot + 1, 3, "_tf") == 0) g.hide = true;
        }

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
    if (compiled && interactiveMode) {
        std::cerr << "msim: -compile: the interactive mode is not compiled yet; using the interpreter\n";
        compiled = false;
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
    // $dumpports (21.7.4.3) tells which side of a port drives it: every
    // continuous driver keeps its slot, and its instance is recorded.
    bool dumpPorts = false;
    {
        auto scan = [&](const Prog& p) {
            for (const Instr& i : p.code) if (i.k == IK::Dump && i.dcmd == DC_PORTS) dumpPorts = true;
        };
        for (auto& lp : lowered.procs) scan(lp.prog);
        for (auto& sp : lowered.subs) if (sp) scan(*sp);
        if (dumpPorts)
            for (auto& lp : lowered.procs)
                for (const Instr& i : lp.prog.code) {
                    if (i.k != IK::Assign || i.drv < 0 || i.sig < 0 || (size_t)i.sig >= S.sigs.size()) continue;
                    auto& v = S.drvScope[i.sig];
                    if (v.size() <= (size_t)i.drv) v.resize((size_t)i.drv + 1);
                    v[(size_t)i.drv] = lp.scope;
                }
    }
    if (staticComb && !vpi::st().active && !g_upf && pathRecs().empty() && sdfRequests().empty() && !sdfAtRunTime() &&
        gateRecs().empty() && !dumpPorts)
        mergePartialDrivers(lowered);
    if (!vpi::st().active && !g_upf && !g_cov && !std::getenv("MSIM_NOEMPTYCALL")) dropEmptyCalls(lowered);
    if (staticComb && !vpi::st().active && !g_upf && !g_cov && !std::getenv("MSIM_NOATSTAR")) rotateAtStar(lowered);
    if (staticComb && !vpi::st().active && !g_upf && !g_cov && !std::getenv("MSIM_NOINLINE")) inlineCombCalls(lowered);
    if (staticComb && !vpi::st().active && !g_upf && !g_cov && !Lowerer::xpropMode() && !std::getenv("MSIM_NOUNROLL"))
        unrollSeqLoops(lowered, true);
    if (staticComb && !vpi::st().active && !g_upf && !g_cov) splitCombBlocks(lowered);
    if (staticSeq && !vpi::st().active && !g_upf && !g_cov && !Lowerer::xpropMode() && !std::getenv("MSIM_NOUNROLL")) unrollSeqLoops(lowered);
    if (staticSeq && !vpi::st().active && !g_upf && !g_cov && !Lowerer::xpropMode() && !std::getenv("MSIM_NOARRFIX")) fixArrayNbas(lowered);
    if (staticSeq && !vpi::st().active && !g_upf && !g_cov && !Lowerer::xpropMode() && !vcdPath && !std::getenv("MSIM_NOSEQTMP")) localizeSeqBlocking(lowered);   // -cov counts each branch once   // -no-static-comb keeps them, as the reference
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
    if (compiled && prof::P().on) {
        std::cerr << "msim: -compile: -profile times the interpreter; using it\n";
        compiled = false;
    }
    bool anyReactive = false;
    for (auto& lp : lowered.procs) anyReactive |= lp.reactive;
    if (compiled && (lowered.programs || anyReactive)) {
        std::cerr << "msim: -compile: programs and checkers are not compiled yet; using the interpreter\n";
        compiled = false;
    }
    if (compiled && vcdClass) {
        std::cerr << "msim: -compile: -vcd-class needs the interpreter; using it\n";
        compiled = false;
    }
    if (g_vendorWave() & 1) vcdSva = true;   // $fsdbDumpSVA, "+sva"
    if (g_vendorWave() & 2) vcdPower = true;   // $fsdbDumpvars(..., "+power")
    if (compiled && (vcdSva || vcdPower)) {
        std::cerr << "msim: -compile: " << (vcdSva ? "assertions" : "power states") << " in the waveform need the interpreter; using it\n";
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
    for (auto& t : lowered.trireg) {   // 28.16.2, Annex E
        if (t.charge != STR_MEDIUM) S.charge[t.sig] = t.charge;
        if (t.decays) S.decay[t.sig] = Sim::Decay{t.decay, 0, true};
    }
    for (auto& t : lowered.trans) S.addTran(t.a, t.b, t.resist, t.aLo, t.bLo, t.w);
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
    // msim -profile: one thread, so that the times add up, and one entry per process.
    auto profEnt = [&](const LoweredProc& lp) {
        std::string loc;
        for (const Instr& in : lp.prog.code)
            if (in.line) {
                loc = (in.file < g_covFiles.size() ? g_covFiles[in.file] : std::string("?")) + ":" + std::to_string(in.line);
                break;
            }

        // The kinds as they are written in the source.
        static const std::map<std::string, std::string> kinds = {
            {"Always", "always"}, {"AlwaysFF", "always_ff"}, {"AlwaysComb", "always_comb"},
            {"AlwaysLatch", "always_latch"}, {"Initial", "initial"}, {"Final", "final"},
            {"init", "initializers"}, {"assign", "assign"}};
        auto k = kinds.find(lp.kind);
        std::string top = S.vcdTop.empty() ? std::string("top") : S.vcdTop;
        if (!lp.port.empty()) {   // a port connection: counted in the instance whose port it is
            size_t dot = lp.port.rfind('.');
            return prof::P().add("port " + lp.port.substr(dot + 1), top + "." + lp.port.substr(0, dot), "");
        }
        return prof::P().add(k == kinds.end() ? lp.kind : k->second, lp.scope.empty() ? top : top + "." + lp.scope, loc);
    };
    if (prof::P().on) { S.threads = 1; staticPart = false; prof::P().t0 = std::chrono::steady_clock::now(); }
    if (interactiveMode) S.threads = 1;   // a stop at a line pauses inside a process: one thread
    std::vector<int> byRank(lowered.procs.size(), -1);
    for (size_t i = 0; i < pl.rank.size(); i++)
        if (pl.rank[i] >= 0) byRank[pl.rank[i]] = (int)i;
    for (int i : byRank) {
        if (i < 0) continue;
        const Prog* pr = &lowered.procs[i].prog;
        for (auto& in : pr->code)
            if (in.k == IK::Assert && !pr->combCtl) pr->combCtl = std::make_shared<ProcCtl>();
        if (prof::P().on) prof::P().combEnt.push_back(profEnt(lowered.procs[i]));
        if (pl.body[i] == 1 && pr->code[0].k == IK::Assign) S.addComb(execComb, pr, pl.ins[i], pl.level[i], combValue, combCommit);
        else S.addComb(execComb, pr, pl.ins[i], pl.level[i]);
    }
    installCovergroupHooks();   // the init process below may run now, and may create covergroups
    for (size_t i = 0; i < lowered.procs.size(); i++) {
        // A process run as a function (static comb or clocked) still takes
        // its generator seed in start order (18.14.2), so every later
        // process draws the same $urandom values as with -no-static-comb
        // -no-static-seq: which blocks the static schedule takes over never
        // changes a test's random stimulus.
        if (pl.rank[i] >= 0 || pl.seq[i]) { ProcCtl unused; seedProc(unused); }
        if (pl.rank[i] >= 0) continue;
        if (lowered.procs[i].kind == "init") {
            // Declaration initializers take effect before the simulation
            // starts (6.8): not value changes VPI callbacks see, and not
            // edges a clocked process registered below could wake on
            // (logic clk = 0 is no negedge).
            Task t = process(&lowered.procs[i].prog);
            S.all.push_back(t.h);
            t.h.resume();
            continue;
        }
        int pe = prof::P().on ? profEnt(lowered.procs[i]) : -1;
        if (pl.seq[i]) {
            if (pe >= 0) prof::P().seqEnt.push_back(pe);
            S.addSeq(execSeq, &lowered.procs[i].prog, pl.ev[i]);
            continue;
        }
        Task t = lowered.procs[i].program >= 0 || lowered.procs[i].reactive
                     ? programProc(&lowered.procs[i].prog, lowered.procs[i].program, lowered.procs[i].kind == "Initial")
                     : process(&lowered.procs[i].prog);
        if (pe >= 0) prof::P().byHandle[t.h.address()] = pe;
        S.spawn(t);
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
    if (vpi::st().active && g_cov) {   // vpi_control(vpiCoverageSave, ...) writes the database now (40.5.1)
        static const LowerResult* lw = &lowered;
        covSaveHook() = [](const std::string& path) { return writeCoverage(path, *lw, S.vcdTop); };
    }
    if (vpi::st().active) vpi::setup(*comp, lowered);   // compiletf, cbEndOfCompile, cbStartOfSimulation
    if (staticPart) {
        std::vector<std::vector<int>> po, pi;
        std::vector<int> pt;
        partitionInputs(lowered, pl, po, pi, pt);
        S.buildPartitions(po, pi, pt);
    }
    covapi::lw = &lowered;
    g_covApiHook = covapi::hook;   // 20.13
    if (vcdClass && comp) objview::build(*comp, vcdClass);   // before the waveform header
    if (vcdSva) statview::sva();
    if (vcdPower) statview::power();
        if (interactiveMode) {   // msim -i: commands drive the run (interactive.inc)
        interactive::Session ses;
        ses.comp = comp.get();
        ses.maxTime = maxTime;
        ses.lr = &lowered;
        ses.doPath = doPath;
        if (doPath.empty()) { ses.prompt = isatty(0); ses.loop(std::cin); }
        else {
            std::ifstream df(doPath);
            if (!df) { std::cerr << "msim: cannot read " << doPath << "\n"; return 2; }
            ses.loop(df);
        }
    } else S.run(maxTime);
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
    if (upfrt::rt().on && !upfrt::rt().covPath.empty() && !upfrt::rt().writeCoverage()) {
        std::cerr << "msim: cannot write " << upfrt::rt().covPath << "\n";
        return 1;
    }
    if (prof::P().on && !prof::P().write(S.now, S.timescale)) {
        std::cerr << "msim: cannot write " << prof::P().path << "\n";
        return 1;
    }
    return S.exitCode;
}
