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
//   -cc <compiler>   C++ compiler for -compile (default $CXX, else c++)
//   -O<n>            optimization level for -compile (default -O2)
//   -keyfile [<key_keyname>=]<private.pem>
//                    an RSA private key for protected envelopes (`pragma
//                    protect begin_protected, IEEE 1800-2023 clause 34 /
//                    IEEE 1735). Repeatable. With a key name the key only
//                    opens key_blocks of that key_keyname (or whose
//                    key_public_key is this key's). Encrypt with msimprotect.
//
// Protected code is decrypted in memory only. Diagnostics located in it are
// reported without their text, its signals are left out of the VCD, and
// -compile falls back to the interpreter so no generated C++ holds it.
//
// Exit status: 0 when the simulation ran, 1 on a compile or lowering error,
// 2 on a bad command line.
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <filesystem>
#include <map>
#include <set>

#include <sys/wait.h>

#include "codegen.hpp"
#include "lower.hpp"
#include "slang/diagnostics/DiagnosticClient.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/driver/Driver.h"
#include "slang/text/SourceManager.h"
#ifdef MINISIM_HAVE_OPENSSL
#include "protect.hpp"
#endif

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

// -compile: generate, build and run. Returns the exit status, or -1 when the
// design needs the interpreter.
static int runCompiled(const LowerResult& lowered, const std::string& dir, const std::string& cc,
                       const std::string& opt, const std::optional<std::string>& vcdPath, uint64_t maxTime) {
    std::vector<CodegenSig> sigs;
    for (auto& g : S.sigs) sigs.push_back({g.name, g.w, g.net, (int)g.drv.size()});
    std::vector<CodegenProc> procs;
    for (auto& pr : lowered.procs) procs.push_back({&pr.prog, pr.kind});
    std::string src;
    // Not compiled yet: final blocks, and a tick other than 1ns (`timescale).
    const char* why = !lowered.finals.empty() ? "a final block"
                    : lowered.timescale() != "1ns" ? "a `timescale precision other than 1ns" : nullptr;
    if (why) {
        std::cerr << "msim: -compile: " << why << " is not compiled yet; using the interpreter\n";
        return -1;
    }
    try {
        src = codegen(sigs, procs);
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
    std::cout.flush();
    int st = std::system(run.c_str());
    if (st == -1) return 1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

// Protected code: for each file (canonical path), the byte ranges of its
// buffer that came out of decryption envelopes.
struct ProtectedCode {
    std::map<std::string, std::vector<std::pair<size_t, size_t>>> ranges;
    const slang::SourceManager* sm = nullptr;
    bool any() const { return !ranges.empty(); }
    bool contains(slang::SourceLocation loc) const {
        if (!sm || ranges.empty() || !loc.valid() || loc.buffer() == slang::SourceLocation::NoLocation.buffer()) return false;
        loc = sm->getFullyOriginalLoc(loc);
        if (!loc.buffer().valid() || !sm->isFileLoc(loc)) return false;
        auto it = ranges.find(sm->getFullPath(loc.buffer()).string());
        if (it == ranges.end()) return false;
        for (auto [b, e] : it->second)
            if (loc.offset() >= b && loc.offset() < e) return true;
        return false;
    }
};

// Passes diagnostics to slang's text client, except those that point into
// protected code: they are reported by severity and file only.
struct RedactingClient : slang::DiagnosticClient {
    std::shared_ptr<slang::DiagnosticClient> inner;
    const ProtectedCode& pc;
    RedactingClient(std::shared_ptr<slang::DiagnosticClient> in, const ProtectedCode& p) : inner(std::move(in)), pc(p) {}
    bool touches(const slang::ReportedDiagnosticInfo& d) const {
        if (pc.contains(d.location)) return true;
        for (auto l : d.expansionLocs) if (pc.contains(l)) return true;
        for (auto& r : d.ranges) if (pc.contains(r.start())) return true;
        return false;
    }
    void report(const slang::ReportedDiagnostic& d) override {
        bool hide = touches(d);
        for (auto& n : d.notes) hide = hide || touches(n);
        if (!hide) { inner->report(d); return; }
        const char* sev = d.severity == slang::DiagnosticSeverity::Error || d.severity == slang::DiagnosticSeverity::Fatal
                              ? "error" : d.severity == slang::DiagnosticSeverity::Warning ? "warning" : "note";
        std::string file = "protected code";
        if (pc.sm && d.location.valid()) {
            auto loc = pc.sm->getFullyOriginalLoc(d.location);
            if (loc.buffer().valid()) file = std::string(pc.sm->getFileName(loc));
        }
        std::cerr << file << ": " << sev << ": in protected code (details hidden)\n";
    }
};

// Decrypt every source the front end may read that holds an envelope, and
// hand slang the cleartext under the file's own path so it never reads the
// envelope itself. Looks at the source files, plus files with a Verilog
// extension in their directories and in the +incdir+ / -I / -y directories,
// so protected `include files and library cells are found too.
static bool preloadProtected(slang::driver::Driver& driver, const std::vector<std::string>& keySpecs,
                             const std::vector<std::string>& dirs, ProtectedCode& pc) {
    namespace fs = std::filesystem;
    std::set<std::string> files;
    std::set<std::string> searchDirs(dirs.begin(), dirs.end());
    for (auto& p : driver.sourceLoader.getFilePaths()) {
        std::error_code ec;
        auto c = fs::weakly_canonical(p, ec);
        if (ec) continue;
        files.insert(c.string());
        searchDirs.insert(c.parent_path().string());
    }
    static const std::set<std::string> exts{".v", ".sv", ".vh", ".svh", ".vp", ".svp", ".inc", ".svi", ".vg"};
    for (auto& d : searchDirs) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(d, ec)) {
            std::error_code ec2;
            if (!e.is_regular_file(ec2) || !exts.count(e.path().extension().string())) continue;
            auto c = fs::weakly_canonical(e.path(), ec2);
            if (!ec2) files.insert(c.string());
        }
    }
    std::vector<std::pair<std::string, std::string>> found;
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.find("begin_protected") != std::string::npos) found.emplace_back(f, std::move(text));
    }
    if (found.empty()) return true;
#ifdef MINISIM_HAVE_OPENSSL
    try {
        protect::Keyring ring;
        for (auto& k : keySpecs) ring.add(k);
        for (auto& [path, text] : found) {
            std::string shown = fs::proximate(path).string();
            protect::Decrypted d = protect::decryptSource(text, ring, shown);
            if (d.envelopes == 0) continue;   // "begin_protected" only in a comment
            driver.sourceManager.assignText(path, d.text);
            OPENSSL_cleanse(d.text.data(), d.text.size());
            pc.ranges[path] = std::move(d.ranges);
        }
    } catch (const protect::Error& ex) {
        std::cerr << "msim: " << ex.what() << "\n";
        return false;
    }
    pc.sm = &driver.sourceManager;
    return true;
#else
    (void)keySpecs;
    std::cerr << "msim: " << found.front().first << " has a protected envelope, and this msim was built without OpenSSL\n";
    return false;
#endif
}

static void usage() {
    std::cerr << "usage: msim [options] <files...>\n"
                 "  -top <name>    top-level module (repeatable)\n"
                 "  -vcd [<file>]  write a VCD waveform (default dump.vcd)\n"
                 "  -maxtime <n>   stop after simulation time n\n"
                 "  -compile       generate C++, build it (<dir>/simv) and run it\n"
                 "  -o <dir>       build directory for -compile (default msim.d)\n"
                 "  -cc <cxx>      C++ compiler for -compile (default $CXX, else c++)\n"
                 "  -keyfile [<keyname>=]<private.pem>  key for protected envelopes (repeatable)\n"
                 "  +define+N[=V]  +incdir+<dir>  -f <file>  -I <dir>  -D N[=V]\n"
                 "  --help-slang   list every front-end option\n";
}

int main(int argc, char** argv) {
    // Pass 1: take msim's own options out; translate VCS spellings for slang.
    std::vector<std::string> fwd{argv[0]};
    std::vector<std::string> plusargs;
    std::optional<std::string> vcdPath;
    uint64_t maxTime = ~0ULL;
    bool compiled = false;
    std::string buildDir = "msim.d", opt = "-O2";
    std::string cc = std::getenv("CXX") ? std::getenv("CXX") : "c++";
    std::vector<std::string> keySpecs, searchDirs;
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
        if (a == "-o") { buildDir = next("a directory"); continue; }
        if (a == "-cc") { cc = next("a compiler"); continue; }
        if (a.size() == 3 && a[0] == '-' && a[1] == 'O' && std::isdigit((unsigned char)a[2])) { opt = a; continue; }
        if (a == "-keyfile") { keySpecs.push_back(next("a private key file")); continue; }
        // Directories that may hold protected `include files or library cells.
        if (startsWith(a, "+incdir+")) {
            for (size_t p = 8; p < a.size();) {
                size_t e = a.find('+', p);
                if (e == std::string::npos) e = a.size();
                if (e > p) searchDirs.push_back(a.substr(p, e - p));
                p = e + 1;
            }
        } else if ((a == "-I" || a == "-y" || a == "--libdir" || a == "--include-directory") && i + 1 < argc) {
            searchDirs.push_back(argv[i + 1]);
        } else if (a.size() > 2 && (startsWith(a, "-I") || startsWith(a, "-y"))) {
            searchDirs.push_back(a.substr(2));
        }
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

    // Protected envelopes (34): decrypt before slang reads the files, and
    // route diagnostics through the redacting client.
    ProtectedCode pc;
    if (!preloadProtected(driver, keySpecs, searchDirs, pc)) return 1;
    if (pc.any()) {
        driver.diagEngine.removeClient(driver.textDiagClient);
        driver.diagEngine.addClient(std::make_shared<RedactingClient>(driver.textDiagClient, pc));
        hideSigIf() = [&pc](const slang::ast::Symbol& s) { return pc.contains(s.location); };
    }
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
        // The message may name something declared in protected code.
        if (pc.any()) std::cerr << "msim: the design (which has protected code) uses a construct msim cannot simulate yet; details hidden\n";
        else std::cerr << "msim: " << ex.what() << "\n";
        return 1;
    }
    if (lowered.procs.empty()) {
        std::cerr << "msim: design has no processes to run\n";
        return 1;
    }

    for (int id : hiddenSigs()) S.sigs[(size_t)id].hide = true;

    // Generated C++ would hold the protected design in the clear.
    if (compiled && pc.any()) {
        std::cerr << "msim: -compile: the design has protected code; using the interpreter\n";
        compiled = false;
    }
    if (compiled) {
        int st = runCompiled(lowered, buildDir, cc, opt, vcdPath, maxTime);
        if (st >= 0) return st;
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
