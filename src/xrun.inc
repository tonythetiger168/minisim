// xrun.inc - the Cadence xrun / irun command line on top of msim (included
// by msim.cpp). Nothing here simulates: the options are translated to msim's
// own, and the run is msim's. msim takes this front end when it is invoked
// as xrun, irun or xmverilog (a link or copy of the binary), or with -xrun
// as the first option:
//
//   xrun -sv tb.sv dut.sv -top tb -incdir inc -define W=8 -access +rwc -l run.log
//   msim -xrun -f run.f -uvm -input run.tcl
//
// What is translated, what is accepted and ignored, and what is refused is
// listed in docs/XRUN.md. Everything else passes through to msim unchanged,
// so msim's own options work in this mode too.
#pragma once
#include "compat.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace xrun {

struct Opts {
    bool on = false;
    std::string tool = "xrun";         // for messages and the default log name
    std::string log;                   // -l / -log; "" with nolog
    bool appendLog = false;
    std::vector<std::string> out;      // the msim command line, argv[0] first
    std::string tclIn;                 // -input file, translated to an msim -do file
};

inline bool ends(const std::string& s, const char* suf) {
    size_t n = std::char_traits<char>::length(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The words of an argument file (-f / -F): blanks separate them, // and #
// start a comment to the end of the line, /* */ is a comment, and quotes
// group words.
inline std::vector<std::string> fileWords(const std::string& path, bool& ok) {
    std::ifstream in(path);
    ok = (bool)in;
    std::vector<std::string> w;
    if (!ok) return w;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string t = ss.str(), cur;
    bool have = false;
    for (size_t i = 0; i < t.size(); i++) {
        char c = t[i];
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') { while (i < t.size() && t[i] != '\n') i++; c = '\n'; }
        else if (c == '#' && !have) { while (i < t.size() && t[i] != '\n') i++; c = '\n'; }
        else if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') {
            size_t e = t.find("*/", i + 2);
            i = e == std::string::npos ? t.size() : e + 1;
            c = ' ';
        } else if (c == '"' || c == '\'') {
            size_t e = t.find(c, i + 1);
            if (e == std::string::npos) e = t.size();
            cur += t.substr(i + 1, e - i - 1);
            have = true;
            i = e;
            continue;
        }
        if (std::isspace((unsigned char)c)) {
            if (have) w.push_back(cur);
            cur.clear();
            have = false;
        } else { cur += c; have = true; }
    }
    if (have) w.push_back(cur);
    // $VAR and ${VAR} in the file, as xrun expands them.
    for (auto& s : w) {
        std::string r;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] != '$' || i + 1 >= s.size()) { r += s[i]; continue; }
            size_t b = i + 1, e;
            bool brace = s[b] == '{';
            if (brace) { e = s.find('}', b); if (e == std::string::npos) { r += s[i]; continue; } b++; }
            else { e = b; while (e < s.size() && (std::isalnum((unsigned char)s[e]) || s[e] == '_')) e++; }
            std::string name = s.substr(b, e - b);
            if (name.empty()) { r += s[i]; continue; }
            const char* v = std::getenv(name.c_str());
            r += v ? v : "";
            i = brace ? e : e - 1;
        }
        s = r;
    }
    return w;
}

// An -input Tcl file in the msim -do language (interactive.inc): commands
// split at ';' and new lines, `run 100 ns` and `run 100ns` kept with their
// unit, `value` as print, `deposit x = 1` as deposit x 1. Waveform and
// database commands have no counterpart here and are dropped with a note.
inline bool tclToDo(const std::string& in, const std::string& outPath, const std::string& tool) {
    std::ifstream f(in);
    if (!f) return false;
    std::ofstream o(outPath);
    std::string line;
    while (std::getline(f, line)) {
        std::stringstream parts(line);
        for (std::string cmd; std::getline(parts, cmd, ';');) {
            std::istringstream is(cmd);
            std::vector<std::string> a;
            for (std::string t; is >> t;) {
                if (t == "=") continue;
                a.push_back(t);
            }
            if (a.empty() || a[0][0] == '#') continue;
            std::string c = a[0];
            if (c == "value") c = "print";
            if (c == "finish") c = "quit";
            if (c == "database" || c == "probe" || c == "dumptcf" || c == "assertion" || c == "set" || c == "source" ||
                c == "simvision") {
                std::cerr << tool << ": the -input command '" << c << "' is not supported; ignored\n";
                continue;
            }
            o << c;
            for (size_t k = 1; k < a.size(); k++) o << ' ' << a[k];
            o << '\n';
        }
    }
    return true;
}

// msim's one table of Cadence options in the shared compatibility layer
// (compat.hpp): the plain renames, and the options accepted with no effect
// here. What needs more than a rename (-f files, -incdir and -define values,
// -top lib.cell:view, the log, -input Tcl, -sdf_cmd_file, ...) stays in
// translate() below.
inline std::span<const compat::Rule> cadenceRules() {
    using compat::Act;
    constexpr auto C = compat::Vendor::Cadence;
    static const compat::Rule r[] = {
        // renames
        {C, "-timescale", Act::Rename, "--timescale", true},
        {C, "-override_timescale", Act::Rename, "--timescale", true},
        {C, "-xmlibdirname", Act::Rename, "-o", true},
        {C, "-nclibdirname", Act::Rename, "-o", true},
        {C, "-c", Act::Rename, "-elab"},                 // stop before the run
        {C, "-elaborate", Act::Rename, "-elab"},
        {C, "-compile", Act::Rename, "-elab"},
        {C, "-notimingchecks", Act::Rename, "+notimingcheck"},
        {C, "-notimingcheck", Act::Rename, "+notimingcheck"},
        {C, "-loadvpi", Act::Rename, "-load", true},
        {C, "-v", Act::Rename, "--libfile", true},       // msim's own -v is its version
        {C, "-y", Act::Rename, "--libdir", true},
        {C, "-endlib", Act::Drop},
        {C, "-makelib", Act::Drop, {}, true, false, "one library here"},
        // accepted, no effect here
        {C, "-64", Act::Drop}, {C, "-sv", Act::Drop}, {C, "-sysv", Act::Drop}, {C, "-linedebug", Act::Drop},
        {C, "-debug", Act::Drop}, {C, "-q", Act::Drop}, {C, "-quiet", Act::Drop}, {C, "-status", Act::Drop},
        {C, "-clean", Act::Drop}, {C, "-exit", Act::Drop}, {C, "-run", Act::Drop}, {C, "-plinowarn", Act::Drop},
        {C, "-disable_sem2009", Act::Drop}, {C, "-relax", Act::Drop}, {C, "-assert", Act::Drop},
        {C, "-messages", Act::Drop}, {C, "-mess", Act::Drop}, {C, "-createdebugdb", Act::Drop},
        {C, "-nowarn_all", Act::Drop}, {C, "-uvmnocdnsextra", Act::Drop}, {C, "-vtimescale", Act::Drop},
        {C, "-mccodegen", Act::Drop}, {C, "-nospecifycheck", Act::Drop}, {C, "-noupdate", Act::Drop},
        {C, "-update", Act::Drop}, {C, "-lineinfo", Act::Drop}, {C, "-unbuffered", Act::Drop},
        {C, "-smartorder", Act::Drop}, {C, "-v200x", Act::Drop}, {C, "-v2001", Act::Drop}, {C, "-sv_ms", Act::Drop},
        {C, "-timedetail", Act::Drop}, {C, "-covoverwrite", Act::Drop}, {C, "-coverage_all", Act::Drop},
        {C, "-abvrecordcoverall", Act::Drop}, {C, "-iprof", Act::Drop},
        {C, "+access+", Act::Drop, {}, false, true}, {C, "+linedebug", Act::Drop, {}, false, true},
        // ... with a value
        {C, "-access", Act::Drop, {}, true}, {C, "-errormax", Act::Drop, {}, true}, {C, "-nowarn", Act::Drop, {}, true},
        {C, "-covfile", Act::Drop, {}, true}, {C, "-covtest", Act::Drop, {}, true},
        {C, "-covworkdir", Act::Drop, {}, true}, {C, "-covscope", Act::Drop, {}, true},
        {C, "-covdut", Act::Drop, {}, true}, {C, "-uvmlinedebug", Act::Drop, {}, true},
        {C, "-xmlibdirpath", Act::Drop, {}, true}, {C, "-nclibdirpath", Act::Drop, {}, true},
        {C, "-cdslib", Act::Drop, {}, true}, {C, "-hdlvar", Act::Drop, {}, true}, {C, "-work", Act::Drop, {}, true},
        {C, "-snapshot", Act::Drop, {}, true}, {C, "-tcl", Act::Drop, {}, true}, {C, "-vlog_ext", Act::Drop, {}, true},
        {C, "-sysv_ext", Act::Drop, {}, true}, {C, "-vhdl_ext", Act::Drop, {}, true},
        {C, "-amsfastspice", Act::Drop, {}, true}, {C, "-licqueue_time", Act::Drop, {}, true},
        {C, "-hostname", Act::Drop, {}, true}, {C, "-layout", Act::Drop, {}, true},
        {C, "-dumpvarsfile", Act::Drop, {}, true},
    };
    return r;
}

// Translate one argument list (the command line, or an -f file's words) into
// msim's: the rules above first, then the options below.
inline bool translate(const std::vector<std::string>& in0, Opts& o, int depth = 0) {
    const std::vector<std::string> in = compat::translate(compat::Vendor::Cadence, in0, cadenceRules()).args;
    auto& v = o.out;
    for (size_t i = 0; i < in.size(); i++) {
        std::string a = in[i];
        std::string la = lower(a);
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= in.size()) { std::cerr << o.tool << ": " << a << " needs " << what << "\n"; std::exit(2); }
            return in[++i];
        };
        // irun / ncverilog spellings with a + prefix: +nctop+x, +xmseed+5, +access+rwc
        for (const char* pre : {"+xm", "+nc"})
            if (la.rfind(pre, 0) == 0 && la.size() > 3 && la != "+ncinput" && la.find('+', 3) != std::string::npos) {
                size_t p = la.find('+', 3);
                std::string name = "-" + la.substr(3, p - 3), val = a.substr(p + 1);
                std::vector<std::string> sub{name};
                if (!val.empty()) sub.push_back(val);
                if (!translate(sub, o, depth + 1)) return false;
                a.clear();
                break;
            }
        if (a.empty()) continue;
        if (la == "-f" || la == "-file") {
            std::string p = need("a file");
            bool ok;
            auto w = fileWords(p, ok);
            if (!ok) { std::cerr << o.tool << ": cannot read " << p << "\n"; return false; }
            if (depth > 20) { std::cerr << o.tool << ": -f files nest too deeply\n"; return false; }
            if (a == "-F") {   // relative paths in it are relative to the file
                std::filesystem::path dir = std::filesystem::path(p).parent_path();
                for (auto& s : w)
                    if (!s.empty() && s[0] != '-' && s[0] != '+' && s[0] != '/' && !dir.empty() &&
                        std::filesystem::exists(dir / s))
                        s = (dir / s).string();
            }
            if (!translate(w, o, depth + 1)) return false;
            continue;
        }
        if (la == "-incdir") { v.push_back("+incdir+" + need("a directory")); continue; }
        if (la == "-define") { v.push_back("+define+" + need("a macro")); continue; }
        if (la == "-top") {
            std::string t = need("a top module");
            if (size_t d = t.find('.'); d != std::string::npos) t = t.substr(d + 1);   // lib.cell
            if (size_t c = t.find(':'); c != std::string::npos) t = t.substr(0, c);    // cell:view
            v.push_back("-top"); v.push_back(t);
            continue;
        }
        if (la == "-l" || la == "-log") { o.log = need("a log file"); continue; }
        if (la == "-nolog") { o.log.clear(); o.log.push_back('\0'); continue; }
        if (la == "-append_log") { o.appendLog = true; continue; }
        if (la == "-input") { o.tclIn = need("a Tcl file"); continue; }
        if (la == "-gui") { std::cerr << o.tool << ": -gui: no GUI here; open the waveform with mdebug (docs/DEBUG.md)\n"; continue; }
        if (la == "-r" || la == "-r_snapshot") {
            std::cerr << o.tool << ": -R: msim keeps no snapshot between runs; give the sources again\n";
            return false;
        }
        if (la == "-coverage" || la == "-cov") {
            if (i + 1 < in.size() && in[i + 1][0] != '-' && in[i + 1][0] != '+' && !ends(in[i + 1], ".sv") && !ends(in[i + 1], ".v"))
                i++;   // the metrics (all, b:e:t:f:u): msim records every metric
            v.push_back("-cov");
            continue;
        }
        if (la == "-xprop") {
            std::string m = lower(need("F or C"));
            v.push_back(m == "f" ? "-xprop=xmerge" : "-xprop");   // F: forward, pessimistic; C: compute as hardware
            continue;
        }
        if (la == "-svseed" || la == "-seed") {
            std::string s = need("a seed");
            if (lower(s) == "random") {
                std::random_device rd;
                s = std::to_string(rd() & 0x7fffffff);
                std::cerr << o.tool << ": -svseed random: using seed " << s << "\n";
            }
            v.push_back("-seed"); v.push_back(s);
            continue;
        }
        if (la == "-sv_lib" || la == "-sv_root" || la == "-uvmhome" || la == "-dpiheader") {
            v.push_back(la); v.push_back(need("a value"));
            if (la == "-uvmhome" && !std::filesystem::exists(v.back())) {   // CDNS-1.2, CDNS-IEEE: the built-in libraries
                std::cerr << o.tool << ": -uvmhome " << v.back() << ": not a directory; using $UVM_HOME\n";
                v.pop_back(); v.back() = "-uvm";
            }
            continue;
        }
        if (la == "-sdf_cmd_file") {   // SDF command file: COMPILED_SDF_FILE / SDF_FILE and SCOPE pairs
            std::string p = need("a file");
            std::ifstream f(p);
            if (!f) { std::cerr << o.tool << ": cannot read " << p << "\n"; return false; }
            std::stringstream ss;
            ss << f.rdbuf();
            std::string t = ss.str(), file, scope, mtm;
            auto flush = [&] {
                if (file.empty()) return;
                if (ends(file, ".X")) file.resize(file.size() - 2);   // a compiled SDF: its source
                v.push_back("-sdf");
                v.push_back((mtm.empty() ? "" : mtm + ":") + scope + ":" + file);
                file.clear(); scope.clear(); mtm.clear();
            };
            std::istringstream ls(t);
            for (std::string ln; std::getline(ls, ln, ln.find(';') != std::string::npos ? ';' : ',');) {
                size_t eq = ln.find('=');
                if (eq == std::string::npos) continue;
                auto trim = [](std::string s) {
                    size_t b = s.find_first_not_of(" \t\r\n\""), e = s.find_last_not_of(" \t\r\n\"");
                    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
                };
                std::string k = lower(trim(ln.substr(0, eq))), val = trim(ln.substr(eq + 1));
                if (k == "compiled_sdf_file" || k == "sdf_file") { flush(); file = val; }
                else if (k == "scope") scope = val;
                else if (k == "mtm_control") mtm = lower(val) == "minimum" ? "min" : lower(val) == "maximum" ? "max" : "typ";
            }
            flush();
            continue;
        }
        if (ends(la, ".vhd") || ends(la, ".vhdl") || la == "-v93" || la == "-v200x_vhdl") {
            std::cerr << o.tool << ": " << a << ": VHDL is not supported yet\n";
            return false;
        }
        v.push_back(a);
    }
    return true;
}

// The argv msim runs with, when this front end is wanted.
// Also on --compat=cadence anywhere on the command line (compat.hpp), or a
// link to msim under one of these names.
inline bool wanted(int argc, char** argv, Opts& o) {
    static const compat::Alias names[] = {{"xrun", compat::Vendor::Cadence}, {"irun", compat::Vendor::Cadence},
                                           {"xmverilog", compat::Vendor::Cadence}, {"ncverilog", compat::Vendor::Cadence}};
    std::string me = compat::basename(argv[0]);
    bool byOpt = argc > 1 && (std::string(argv[1]) == "-xrun" || std::string(argv[1]) == "-irun");
    std::vector<std::string> in(argv + (byOpt ? 2 : 1), argv + argc);
    bool cadence = compat::detect(argv[0], in, names) == compat::Vendor::Cadence;
    if (!cadence && !byOpt) return false;
    o.on = true;
    bool byName = std::find_if(std::begin(names), std::end(names), [&](const compat::Alias& a) { return a.argv0 == me; }) !=
                  std::end(names);
    o.tool = byOpt ? std::string(argv[1]).substr(1) : byName ? me : std::string("xrun");
    o.log = o.tool + ".log";
    o.out.push_back(argv[0]);
    if (!translate(in, o)) std::exit(2);
    if (o.log.size() == 1 && o.log[0] == '\0') o.log.clear();
    return true;
}

// Copy everything msim writes on stdout and stderr to the log file too, as
// xrun does: a child runs the simulation, this process copies its output.
// Returns in the child; the parent exits with the child's status.
inline void teeLog(const Opts& o) {
    if (o.log.empty()) return;
    std::cout.flush();
    std::cerr.flush();
    int fd[2];
    if (pipe(fd) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return; }
    if (pid == 0) {   // the simulation
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[0]);
        close(fd[1]);
        return;
    }
    close(fd[1]);
    FILE* lf = std::fopen(o.log.c_str(), o.appendLog ? "a" : "w");
    if (!lf) std::cerr << o.tool << ": cannot write " << o.log << "\n";
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd[0], buf, sizeof buf);
        if (n <= 0) break;
        (void)!write(1, buf, (size_t)n);
        if (lf) std::fwrite(buf, 1, (size_t)n, lf);
    }
    if (lf) std::fclose(lf);
    int st = 0;
    waitpid(pid, &st, 0);
    std::_Exit(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0));
}

}   // namespace xrun
