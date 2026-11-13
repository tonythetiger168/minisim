// mstep.inc - the multi-step command lines of Siemens Questa (vlib, vmap,
// vlog, vcom, vsim) and Synopsys VCS (vlogan, vhdlan, vcs, then simv) on top
// of msim (included by msim.cpp, after xrun.inc). Nothing here simulates:
// the analysis steps check a file and record it in a library, and the
// elaboration step runs msim on what the libraries hold.
//
//   vlib work; vlog -sv +incdir+inc dut.sv tb.sv; vcom ctr.vhd
//   vsim -c -do "run -all; quit -f" -sv_seed 5 tb
//
//   vlogan -sverilog -work work dut.sv tb.sv; vhdlan ctr.vhd
//   vcs -debug_access+all tb -o simv; ./simv +ntb_random_seed=5 -l run.log
//
// msim takes this front end when it is invoked under one of these names
// (a link or copy of the binary; minisim ships none), or with -vlog, -vcom,
// -vsim, -vlib, -vmap, -vlogan, -vhdlan or -vcs as the first option.
//
// A library is a directory (./work by default). It holds one text file,
// msim.lib: the analysed sources in analysis order, one per line, each with
// the options it was analysed with (+define+, +incdir+, -y, ...). Analysing a
// file again replaces its line. Library names map to directories through
// modelsim.ini ([Library] name = dir) for the Siemens tools and
// synopsys_sim.setup (NAME : dir, WORK > NAME) for the Synopsys ones; an
// unmapped name is a directory of that name. docs/COMPAT.md lists the
// options.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "compat.hpp"
#include "vhdl/vhdl.hpp"

namespace mstep {

namespace fs = std::filesystem;

enum class Tool { None, Vlib, Vmap, Vlog, Vcom, Vsim, Vlogan, Vhdlan, Vcs, Simv };

struct Opts {
    Tool tool = Tool::None;
    std::string name;                  // the tool's name, for messages
    std::vector<std::string> out;      // the msim command line, argv[0] first
    std::string log;                   // -l: a copy of the output
    bool appendLog = false;
    int exitNow = -1;                  // >= 0: the step is done; exit with this status
};

inline bool snps(Tool t) { return t == Tool::Vlogan || t == Tool::Vhdlan || t == Tool::Vcs || t == Tool::Simv; }

inline std::string self() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string("msim") : p.string();
}
inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
inline std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
inline std::string absPath(const std::string& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    return ec ? p : a.lexically_normal().string();
}
inline std::string shq(const std::string& s) {
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
}
inline std::string expandEnv(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '$' || i + 1 >= s.size()) { r += s[i]; continue; }
        size_t b = i + 1, e;
        bool brace = s[b] == '{' || s[b] == '(';
        if (brace) {
            e = s.find(s[b] == '{' ? '}' : ')', b);
            if (e == std::string::npos) { r += s[i]; continue; }
            b++;
        } else {
            e = b;
            while (e < s.size() && (std::isalnum((unsigned char)s[e]) || s[e] == '_')) e++;
        }
        const char* v = std::getenv(s.substr(b, e - b).c_str());
        r += v ? v : "";
        i = brace ? e : e - 1;
    }
    return r;
}

// ---- library names ----

struct LibMap {
    bool snps = false;
    std::string ini;                              // modelsim.ini or synopsys_sim.setup
    std::map<std::string, std::string> dirs;      // name (upper case for Synopsys) -> directory
    std::string work = "WORK";                    // Synopsys: WORK > NAME
};

inline LibMap readMap(bool synopsys, const std::string& iniGiven = {}) {
    LibMap m;
    m.snps = synopsys;
    if (synopsys) {
        const char* env = std::getenv("SYNOPSYS_SIM_SETUP");
        m.ini = !iniGiven.empty() ? iniGiven : env ? env : "synopsys_sim.setup";
        std::ifstream in(m.ini);
        for (std::string ln; std::getline(in, ln);) {
            if (size_t c = ln.find("--"); c != std::string::npos) ln.resize(c);
            if (size_t c = ln.find('#'); c != std::string::npos) ln.resize(c);
            if (size_t g = ln.find('>'); g != std::string::npos) {
                if (upper(trim(ln.substr(0, g))) == "WORK") m.work = upper(trim(ln.substr(g + 1)));
                continue;
            }
            size_t c = ln.find(':');
            if (c == std::string::npos || ln.find('=') != std::string::npos) continue;
            std::string n = upper(trim(ln.substr(0, c))), d = trim(expandEnv(ln.substr(c + 1)));
            if (!n.empty() && !d.empty() && n.find_first_of(" \t") == std::string::npos) m.dirs[n] = d;
        }
    } else {
        const char* env = std::getenv("MODELSIM");
        m.ini = !iniGiven.empty() ? iniGiven : env ? env : "modelsim.ini";
        std::ifstream in(m.ini);
        bool lib = false;
        for (std::string ln; std::getline(in, ln);) {
            if (size_t c = ln.find(';'); c != std::string::npos) ln.resize(c);
            ln = trim(ln);
            if (ln.empty()) continue;
            if (ln[0] == '[') { lib = lower(ln) == "[library]"; continue; }
            size_t eq = ln.find('=');
            if (!lib || eq == std::string::npos) continue;
            std::string n = trim(ln.substr(0, eq)), d = trim(expandEnv(ln.substr(eq + 1)));
            if (!n.empty() && !d.empty()) m.dirs[n] = d;
        }
    }
    return m;
}

// The directory of library `name` ("" for the work library).
inline std::string libDir(const LibMap& m, std::string name) {
    if (name.empty()) name = m.snps ? m.work : "work";
    std::string key = m.snps ? upper(name) : name;
    if (m.snps && key == "WORK") key = m.work;
    for (int hop = 0; hop < 8; hop++) {   // a Synopsys name may map to another name
        auto it = m.dirs.find(key);
        if (it == m.dirs.end()) break;
        if (!m.snps || it->second.find('/') != std::string::npos || it->second.find('.') != std::string::npos ||
            !m.dirs.count(upper(it->second)))
            return it->second;
        key = upper(it->second);
    }
    return m.snps ? lower(key == "WORK" ? std::string("work") : key) : name;
}

// ---- the library file ----

struct Entry {
    std::string file;
    std::vector<std::string> opts;   // one token each: +define+X, +incdir+/abs, --timescale=1ns/1ps, -uvm, ...
};
inline const char* kLibFile = "msim.lib";

inline std::vector<Entry> readLib(const std::string& dir) {
    std::vector<Entry> v;
    std::ifstream in(dir + "/" + kLibFile);
    for (std::string ln; std::getline(in, ln);) {
        if (ln.empty() || ln[0] == '#') continue;
        Entry e;
        std::stringstream ss(ln);
        std::string f;
        bool first = true;
        while (std::getline(ss, f, '\t')) {
            if (first) e.file = f; else if (!f.empty()) e.opts.push_back(f);
            first = false;
        }
        if (!e.file.empty()) v.push_back(std::move(e));
    }
    return v;
}
inline bool writeLib(const std::string& dir, const std::vector<Entry>& v) {
    std::ofstream out(dir + "/" + kLibFile);
    if (!out) return false;
    out << "# msim library: the analysed sources in analysis order, each with its options (tab separated)\n";
    for (auto& e : v) {
        out << e.file;
        for (auto& o : e.opts) out << '\t' << o;
        out << '\n';
    }
    return (bool)out;
}
inline bool isLib(const std::string& dir) { return fs::exists(dir + "/" + kLibFile); }
inline bool makeLib(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (isLib(dir)) return true;
    return writeLib(dir, {});
}

// ---- argument files ----

inline bool expandF(const std::vector<std::string>& in, std::vector<std::string>& out, const std::string& tool, int depth = 0) {
    for (size_t i = 0; i < in.size(); i++) {
        const std::string& a = in[i];
        if ((a == "-f" || a == "-F" || a == "-file") && i + 1 < in.size()) {
            std::string p = in[++i];
            bool ok;
            auto w = xrun::fileWords(p, ok);
            if (!ok) { std::cerr << tool << ": cannot read " << p << "\n"; return false; }
            if (depth > 20) { std::cerr << tool << ": -f files nest too deeply\n"; return false; }
            fs::path dir = fs::path(p).parent_path();
            if (a == "-F")   // relative paths in it are relative to the file
                for (auto& s : w)
                    if (!s.empty() && s[0] != '-' && s[0] != '+' && s[0] != '/' && !dir.empty() && fs::exists(dir / s))
                        s = (dir / s).string();
            if (!expandF(w, out, tool, depth + 1)) return false;
            continue;
        }
        out.push_back(a);
    }
    return true;
}

// +incdir+a+b with each directory made absolute.
inline std::string absIncdir(const std::string& a) {
    std::string r = "+incdir";
    std::stringstream ss(a.substr(8));
    for (std::string d; std::getline(ss, d, '+');)
        if (!d.empty()) r += "+" + absPath(d);
    return r;
}

// Options of the analysis and elaboration steps that mean nothing here.
inline bool quietDrop(const std::string& a) {
    static const std::set<std::string> plain = {
        "-sv", "-sverilog", "+v2k", "-mfcu", "-sfcu", "-lint", "-quiet", "-q", "-nologo", "-incr", "-novopt",
        "-vopt", "-kdb", "-lca", "-87", "-93", "-2002", "-2008", "-2019", "-vhdl2008", "-vhdl93", "-v93", "-smartorder",
        "-nc", "-notice", "-c", "-batch", "-debug_all", "-debug_pp", "-debug", "-sv05compat", "-sv09compat",
        "-sv12compat", "-sv17compat", "-svext", "-full64", "-64", "-32", "-pedanticerrors", "-noincr", "-hazards",
        "-check_synthesis", "-explicit", "-nolibcell", "-novitalcheck"};
    return plain.count(a) || starts(a, "-debug_access") || starts(a, "+acc") || starts(a, "-voptargs") ||
           starts(a, "+lint") || starts(a, "+warn") || starts(a, "+vcs+") || starts(a, "+cover") ||
           starts(a, "-suppress=") || starts(a, "+nowarn") || starts(a, "+error") || starts(a, "-assert=") ||
           starts(a, "-xlrm") || starts(a, "-fsmdebug") || starts(a, "+vpi");
}
// The same, for options followed by a value.
inline bool quietDropValue(const std::string& a) {
    static const std::set<std::string> v = {"-suppress", "-error", "-warning", "-note", "-msgmode", "-displaymsgmode",
                                            "-L", "-Lf", "-t", "-wlf", "-onfinish", "-assert", "-msglimit",
                                            "-timescale_override", "-modelsimini", "-pslfile", "-lint_opts", "-cover",
                                            "-time_res"};
    return v.count(a);
}

// ---- analysis: vlog, vcom, vlogan, vhdlan ----

// Parse-check files with msim -parse: the options, then the files. Output
// goes to stdout and, with a log, to the log too. The exit status.
inline int parseCheck(const std::vector<std::string>& opts, const std::vector<std::string>& files, FILE* log) {
    std::string cmd = shq(self()) + " -parse";
    for (auto& o : opts) cmd += " " + shq(o);
    for (auto& f : files) cmd += " " + shq(f);
    cmd += " 2>&1";
    std::cout.flush();
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return 2;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
        std::fwrite(buf, 1, n, stdout);
        if (log) std::fwrite(buf, 1, n, log);
    }
    std::fflush(stdout);
    int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 2;
}

inline int analyse(const Opts& o, const std::vector<std::string>& args) {
    std::vector<std::string> in;
    if (!expandF(args, in, o.name)) return 2;
    std::string work, ini, logPath;
    bool appendLog = false;
    std::vector<std::string> opts, files;
    auto need = [&](size_t& i, const char* what) -> std::string {
        if (i + 1 >= in.size()) { std::cerr << o.name << ": " << in[i] << " needs " << what << "\n"; std::exit(2); }
        return in[++i];
    };
    for (size_t i = 0; i < in.size(); i++) {
        const std::string& a = in[i];
        if (a == "-work") { work = need(i, "a library"); continue; }
        if (a == "-modelsimini") { ini = need(i, "a file"); continue; }
        if (a == "-l" || a == "-logfile" || a == "-log") { logPath = need(i, "a log file"); continue; }
        if (a == "-append_log") { appendLog = true; continue; }
        if (starts(a, "+define+")) { opts.push_back(a); continue; }
        if (a == "-define" || a == "-D") { opts.push_back("+define+" + need(i, "a macro")); continue; }
        if (starts(a, "+incdir+")) { opts.push_back(absIncdir(a)); continue; }
        if (a == "-incdir" || a == "-I") { opts.push_back("+incdir+" + absPath(need(i, "a directory"))); continue; }
        if (a == "-y") { opts.push_back("--libdir=" + absPath(need(i, "a directory"))); continue; }
        if (a == "-v") { opts.push_back("--libfile=" + absPath(need(i, "a file"))); continue; }
        if (starts(a, "+libext+")) { opts.push_back(a); continue; }
        if (a == "-timescale") { opts.push_back("--timescale=" + need(i, "a time scale")); continue; }
        if (starts(a, "-timescale=")) { opts.push_back("--timescale=" + a.substr(11)); continue; }
        if (a == "-ntb_opts") {
            if (need(i, "a value").find("uvm") != std::string::npos) opts.push_back("-uvm");
            continue;
        }
        if (a == "-uvm") { opts.push_back("-uvm"); continue; }
        if (quietDrop(a)) continue;
        if (quietDropValue(a)) { need(i, "a value"); continue; }
        if (!a.empty() && (a[0] == '-' || a[0] == '+')) {
            std::cerr << o.name << ": ignored option " << a << "\n";
            continue;
        }
        if (!fs::exists(a)) { std::cerr << o.name << ": cannot open " << a << "\n"; return 2; }
        files.push_back(absPath(a));
    }
    if (files.empty()) { std::cerr << o.name << ": no source files\n"; return 2; }
    LibMap m = readMap(snps(o.tool), ini);
    std::string dir = libDir(m, work);
    if (!isLib(dir)) {
        if (snps(o.tool)) {   // vlogan / vhdlan create the library directory
            if (!makeLib(dir)) { std::cerr << o.name << ": cannot create library " << dir << "\n"; return 2; }
        } else {
            std::cerr << o.name << ": library " << dir << " not found; create it with vlib " << dir << "\n";
            return 2;
        }
    }
    std::vector<Entry> lib = readLib(dir);
    std::set<std::string> newFiles(files.begin(), files.end());
    // VHDL refers to what the library already holds: check new VHDL files
    // with the earlier VHDL of the library in front of them.
    std::vector<std::string> check;
    bool anyVhdl = std::any_of(files.begin(), files.end(), [](const std::string& f) { return vhdl::isVhdlFile(f); });
    if (anyVhdl)
        for (auto& e : lib)
            if (vhdl::isVhdlFile(e.file) && !newFiles.count(e.file) && fs::exists(e.file)) check.push_back(e.file);
    check.insert(check.end(), files.begin(), files.end());
    FILE* log = logPath.empty() ? nullptr : std::fopen(logPath.c_str(), appendLog ? "a" : "w");
    int rc = parseCheck(opts, check, log);
    if (rc == 0) {
        lib.erase(std::remove_if(lib.begin(), lib.end(), [&](const Entry& e) { return newFiles.count(e.file) > 0; }),
                  lib.end());
        for (auto& f : files) lib.push_back({f, opts});
        if (!writeLib(dir, lib)) { std::cerr << o.name << ": cannot write " << dir << "/" << kLibFile << "\n"; rc = 2; }
    } else {
        std::string msg = o.name + ": errors; " + std::to_string(files.size()) + " file(s) not added to library " + dir + "\n";
        std::cerr << msg;
        if (log) std::fputs(msg.c_str(), log);
    }
    if (log) std::fclose(log);
    return rc;
}

// ---- vlib, vmap ----

inline int vlib(const Opts& o, const std::vector<std::string>& args) {
    int rc = 0, n = 0;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "-type" || a == "-format" || a == "-locklib" || a == "-unlocklib") {   // library formats: one here
            if (a == "-type" || a == "-format") i++;
            continue;
        }
        if (!a.empty() && a[0] == '-') continue;
        n++;
        if (!makeLib(a)) { std::cerr << o.name << ": cannot create library " << a << "\n"; rc = 2; }
    }
    if (!n && !makeLib("work")) rc = 2;
    return rc;
}

inline int vmap(const Opts& o, const std::vector<std::string>& args) {
    std::string ini = std::getenv("MODELSIM") ? std::getenv("MODELSIM") : "modelsim.ini";
    std::vector<std::string> pos;
    bool del = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-del") { del = true; continue; }
        if (args[i] == "-modelsimini" && i + 1 < args.size()) { ini = args[++i]; continue; }
        if (args[i] == "-c") continue;
        pos.push_back(args[i]);
    }
    LibMap m = readMap(false, ini);
    if (pos.empty()) {   // list the mappings
        for (auto& [n, d] : m.dirs) std::cout << n << " maps to directory " << d << "\n";
        return 0;
    }
    if (pos.size() == 1 && !del) {
        std::cout << pos[0] << " maps to directory " << libDir(m, pos[0]) << "\n";
        return 0;
    }
    if (del ? pos.size() != 1 : pos.size() != 2) { std::cerr << o.name << ": usage: vmap <name> <directory>, vmap -del <name>\n"; return 2; }
    // Rewrite the [Library] section, keeping every other line.
    std::vector<std::string> lines;
    {
        std::ifstream in(ini);
        for (std::string ln; std::getline(in, ln);) lines.push_back(ln);
    }
    std::vector<std::string> out;
    bool inLib = false, done = false, sawLib = false;
    for (auto& ln : lines) {
        std::string t = trim(ln);
        if (!t.empty() && t[0] == '[') {
            if (inLib && !done && !del) { out.push_back(pos[0] + " = " + pos[1]); done = true; }
            inLib = lower(t) == "[library]";
            sawLib |= inLib;
            out.push_back(ln);
            continue;
        }
        if (inLib) {
            size_t eq = t.find('=');
            if (t[0] != ';' && eq != std::string::npos && trim(t.substr(0, eq)) == pos[0]) {
                if (!del && !done) { out.push_back(pos[0] + " = " + pos[1]); done = true; }
                continue;
            }
        }
        out.push_back(ln);
    }
    if (!del && !done) {
        if (!sawLib) out.push_back("[Library]");
        if (inLib || !sawLib) out.push_back(pos[0] + " = " + pos[1]);
        else {   // [Library] was not the last section: add the line right after its header
            for (size_t k = 0; k < out.size(); k++)
                if (lower(trim(out[k])) == "[library]") { out.insert(out.begin() + (long)k + 1, pos[0] + " = " + pos[1]); break; }
        }
    }
    std::ofstream f(ini);
    for (auto& ln : out) f << ln << "\n";
    if (!f) { std::cerr << o.name << ": cannot write " << ini << "\n"; return 2; }
    if (!del) std::cout << "Modifying " << ini << "\n";
    return 0;
}

// ---- elaboration: the libraries' sources as one msim command line ----

// Appends the sources of the libraries in `libs`, in order, each once, with
// their options (each option once) in front of the files.
inline bool collect(const LibMap& m, const std::vector<std::string>& libs, std::vector<std::string>& out,
                    const std::string& tool) {
    std::set<std::string> seenLib, seenFile, seenOpt;
    std::vector<std::string> opts, files;
    for (auto& l : libs) {
        std::string dir = absPath(libDir(m, l));
        if (!seenLib.insert(dir).second) continue;
        if (!isLib(dir)) { std::cerr << tool << ": library " << l << " (" << dir << ") not found\n"; return false; }
        for (auto& e : readLib(dir)) {
            if (!seenFile.insert(e.file).second) continue;
            for (auto& op : e.opts)
                if (seenOpt.insert(op).second) opts.push_back(op);
            files.push_back(e.file);
        }
    }
    if (files.empty()) { std::cerr << tool << ": the libraries hold no sources; analyse some first\n"; return false; }
    out.insert(out.end(), opts.begin(), opts.end());
    out.insert(out.end(), files.begin(), files.end());
    return true;
}

// A design unit name: lib.top, top, or top(arch) / lib.top:view.
inline void splitTop(std::string t, std::string& lib, std::string& top) {
    if (size_t p = t.find_first_of("(:"); p != std::string::npos) t.resize(p);
    size_t d = t.find('.');
    lib = d == std::string::npos ? std::string() : t.substr(0, d);
    top = d == std::string::npos ? t : t.substr(d + 1);
}

// A -do script made only of run -all and quit needs no Tcl: msim runs to the end.
inline bool justRun(const std::string& script) {
    std::string s = script;
    for (char& c : s)
        if (c == '\n' || c == ';') c = '\n';
    std::stringstream ss(s);
    for (std::string ln; std::getline(ss, ln);) {
        ln = trim(ln);
        if (ln.empty() || ln[0] == '#') continue;
        std::stringstream ws(ln);
        std::vector<std::string> w;
        for (std::string x; ws >> x;) w.push_back(x);
        bool ok = (w[0] == "run" && (w.size() == 1 || (w.size() == 2 && (w[1] == "-all" || w[1] == "-a")))) ||
                  ((w[0] == "quit" || w[0] == "exit" || w[0] == "finish") &&
                   (w.size() == 1 || (w.size() == 2 && (w[1] == "-f" || w[1] == "-force" || w[1] == "-sim"))));
        if (!ok) return false;
    }
    return true;
}

inline std::string seedValue(const std::string& s, const std::string& tool) {
    if (lower(s) != "random") return s;
    std::random_device rd;
    std::string r = std::to_string(rd() & 0x7fffffff);
    std::cerr << tool << ": seed random: using seed " << r << "\n";
    return r;
}

// Writes a Tcl script for msim -do; returns its path ("" on failure).
inline std::string doFile(const std::string& script, const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string p = dir + "/run.do";
    std::ofstream f(p);
    std::string s = script;
    for (char& c : s)
        if (c == ';') c = '\n';
    f << s << "\n";
    return f ? p : std::string();
}

// vsim [options] [lib.]top ...: msim on the libraries' sources.
inline bool vsim(Opts& o, const std::vector<std::string>& args) {
    std::vector<std::string> in;
    if (!expandF(args, in, o.name)) return false;
    std::vector<std::string> libs, tops, rest;
    std::string ini, workLib, script;
    bool batch = false, doGiven = false, compiled = false, dirGiven = false;
    auto need = [&](size_t& i, const char* what) -> std::string {
        if (i + 1 >= in.size()) { std::cerr << o.name << ": " << in[i] << " needs " << what << "\n"; std::exit(2); }
        return in[++i];
    };
    for (size_t i = 0; i < in.size(); i++) {
        const std::string& a = in[i];
        if (a == "-c" || a == "-batch") { batch = true; continue; }
        if (a == "-gui" || a == "-i") { std::cerr << o.name << ": " << a << ": no GUI here; open the waveform with mdebug (docs/DEBUG.md)\n"; continue; }
        if (a == "-do") {
            std::string d = need(i, "a command or file");
            if (fs::is_regular_file(d)) {
                std::ifstream f(d);
                std::stringstream ss;
                ss << f.rdbuf();
                d = ss.str();
            }
            script += d + "\n";
            doGiven = true;
            continue;
        }
        if (a == "-L" || a == "-Lf") { libs.push_back(need(i, "a library")); continue; }
        if (a == "-lib" || a == "-work") { workLib = need(i, "a library"); continue; }
        if (a == "-modelsimini") { ini = need(i, "a file"); continue; }
        if (a == "-l" || a == "-logfile") { o.log = need(i, "a log file"); continue; }
        if (a == "-sv_seed" || a == "-seed") { rest.push_back("-seed"); rest.push_back(seedValue(need(i, "a seed"), o.name)); continue; }
        if (a == "-sv_lib" || a == "-sv_root" || a == "-sv_liblist") { rest.push_back(a); rest.push_back(need(i, "a value")); continue; }
        if (a == "-pli") { rest.push_back("-load"); rest.push_back(need(i, "a library")); continue; }
        if ((starts(a, "-g") || starts(a, "-G")) && a.size() > 2 && a.find('=') != std::string::npos) {
            rest.push_back("-G");
            rest.push_back(a.substr(2));
            continue;
        }
        if (a == "-coverage") { rest.push_back("-cov"); continue; }
        if (a == "-compile") { compiled = true; rest.push_back(a); continue; }
        if (a == "-o" || starts(a, "-partcomp_dir=") || starts(a, "-Mdir=")) dirGiven = true;
        if (quietDrop(a)) continue;
        if (quietDropValue(a)) { need(i, "a value"); continue; }
        if (!a.empty() && (a[0] == '-' || a[0] == '+')) {   // msim's own options and plusargs
            rest.push_back(a);
            if (a == "-o" || a == "-cc" || a == "-threads" || a == "-vcd" || a == "-fst" || a == "-maxtime")
                if (i + 1 < in.size()) rest.push_back(in[++i]);
            continue;
        }
        tops.push_back(a);
    }
    if (tops.empty()) { std::cerr << o.name << ": no design unit to simulate\n"; return false; }
    LibMap m = readMap(false, ini);
    std::vector<std::string> order = libs;   // -L libraries first, then those of the tops
    for (auto& t : tops) {
        std::string l, n;
        splitTop(t, l, n);
        order.push_back(l.empty() ? workLib : l);
    }
    if (!collect(m, order, o.out, o.name)) return false;
    for (auto& t : tops) {
        std::string l, n;
        splitTop(t, l, n);
        o.out.push_back("-top");
        o.out.push_back(n);
    }
    std::string lib0 = absPath(libDir(m, workLib));
    std::string first;
    {
        std::string l;
        splitTop(tops[0], l, first);
        if (!l.empty()) lib0 = absPath(libDir(m, l));
    }
    if (compiled && !dirGiven) { o.out.push_back("-o"); o.out.push_back(lib0 + "/_msim/" + first); }
    o.out.insert(o.out.end(), rest.begin(), rest.end());
    if (doGiven && !justRun(script)) {
        std::string p = doFile(script, lib0 + "/_msim/" + first);
        if (p.empty()) { std::cerr << o.name << ": cannot write the -do script\n"; return false; }
        o.out.push_back("-do");
        o.out.push_back(p);
    } else if (batch && !doGiven) {
        o.out.push_back("-i");   // vsim -c with no -do: commands from the terminal
    }
    return true;
}

// ---- vcs and simv ----

inline bool hdlFile(const std::string& a) {
    static const char* ext[] = {".v", ".sv", ".svh", ".vh", ".vp", ".svp", ".vlib", ".vo", ".vg", ".vhd", ".vhdl", ".vho"};
    std::string l = lower(a);
    for (const char* e : ext)
        if (l.size() > strlen(e) && l.compare(l.size() - strlen(e), strlen(e), e) == 0) return true;
    return false;
}

// vcs: one step (vcs files...) or the elaboration step after vlogan /
// vhdlan (vcs top). Either way it checks the design now, as vcs does, and
// writes simv: a script that runs msim with the recorded command line. -R
// runs it at once.
inline int vcs(Opts& o, const std::vector<std::string>& args) {
    std::vector<std::string> in;
    if (!expandF(args, in, o.name)) return 2;
    std::string simv = "simv", logPath;
    bool run = false, compiled = false, dirGiven = false;
    std::vector<std::string> rec, runArgs, tops;
    bool oneStep = false;
    for (auto& a : in)
        if (!a.empty() && a[0] != '-' && a[0] != '+' && (hdlFile(a) || fs::is_regular_file(a))) oneStep = true;
    auto need = [&](size_t& i, const char* what) -> std::string {
        if (i + 1 >= in.size()) { std::cerr << o.name << ": " << in[i] << " needs " << what << "\n"; std::exit(2); }
        return in[++i];
    };
    std::string ini;
    for (size_t i = 0; i < in.size(); i++) {
        const std::string& a = in[i];
        if (a == "-o") { simv = need(i, "a name"); continue; }
        if (a == "-R") { run = true; continue; }
        if (a == "-l") { logPath = need(i, "a log file"); continue; }
        if (a == "-ntb_opts") {
            if (need(i, "a value").find("uvm") != std::string::npos) rec.push_back("-uvm");
            continue;
        }
        if (starts(a, "-timescale=")) { rec.push_back("--timescale=" + a.substr(11)); continue; }
        if (starts(a, "+incdir+")) { rec.push_back(absIncdir(a)); continue; }
        if (a == "-y") { rec.push_back("--libdir=" + absPath(need(i, "a directory"))); continue; }
        if (a == "-v") { rec.push_back("--libfile=" + absPath(need(i, "a file"))); continue; }
        if (a == "-top") { rec.push_back("-top"); rec.push_back(need(i, "a module")); continue; }
        if (a == "-gui" || a == "-verdi") { std::cerr << o.name << ": " << a << ": no GUI here; open the waveform with mdebug (docs/DEBUG.md)\n"; continue; }
        if (a == "-compile" || starts(a, "-partcomp") || starts(a, "-fastpartcomp")) {
            compiled = true;
            if (starts(a, "-partcomp_dir=")) dirGiven = true;
            rec.push_back(a);
            continue;
        }
        if (starts(a, "-Mdir=")) { dirGiven = true; rec.push_back(a); continue; }
        if (a == "-setup" || a == "-synopsys_sim_setup") { ini = need(i, "a file"); continue; }
        if (starts(a, "-j") && a.size() > 2 && std::isdigit((unsigned char)a[2])) continue;   // parallel compile: msim's own
        if (a == "-Mupdate" || a == "-Mlib" || starts(a, "-Mlib=")) continue;
        if (quietDrop(a)) continue;
        if (a == "-assert" || a == "-error" || a == "-suppress" || a == "-msglimit") { need(i, "a value"); continue; }
        if (!a.empty() && (a[0] == '-' || a[0] == '+')) {   // msim's own options (msim takes the VCS spellings) and plusargs
            rec.push_back(a);
            if (a == "-cm" || a == "-cm_hier" || a == "-cm_cond" || a == "-cm_dir" || a == "-cm_name" || a == "-sv_lib" ||
                a == "-sv_root" || a == "-load" || a == "-cc" || a == "-threads" || a == "-sdf" || a == "-xprop")
                if (i + 1 < in.size()) rec.push_back(in[++i]);
            continue;
        }
        if (oneStep) {
            if (!fs::exists(a)) { std::cerr << o.name << ": cannot open " << a << "\n"; return 2; }
            rec.push_back(absPath(a));
        } else
            tops.push_back(a);
    }
    std::string daidir = absPath(simv + ".daidir");
    std::vector<std::string> cmd;
    if (!oneStep) {
        if (tops.empty()) { std::cerr << o.name << ": no source files and no design unit\n"; return 2; }
        LibMap m = readMap(true, ini);
        // VCS finds design units in every library the setup file maps: the
        // others first, then those of the tops.
        std::vector<std::string> libs;
        for (auto& [n, d] : m.dirs)
            if (n != "WORK" && n != m.work && isLib(d)) libs.push_back(n);
        for (auto& t : tops) {
            std::string l, n;
            splitTop(t, l, n);
            libs.push_back(l);
        }
        if (!collect(m, libs, cmd, o.name)) return 2;
        for (auto& t : tops) {
            std::string l, n;
            splitTop(t, l, n);
            cmd.push_back("-top");
            cmd.push_back(n);
        }
        std::string l0, n0;
        splitTop(tops[0], l0, n0);
        if (compiled && !dirGiven) { cmd.push_back("-o"); cmd.push_back(absPath(libDir(m, l0)) + "/_msim/" + n0); }
    } else if (compiled && !dirGiven) {
        cmd.push_back("-o");
        cmd.push_back(daidir + "/msim.d");
    }
    cmd.insert(cmd.end(), rec.begin(), rec.end());
    // Check the design now, as vcs does: msim -elab.
    {
        FILE* log = logPath.empty() ? nullptr : std::fopen(logPath.c_str(), "w");
        std::vector<std::string> opts = cmd;
        std::string c = shq(self()) + " -elab";
        for (auto& x : opts) c += " " + shq(x);
        c += " 2>&1";
        std::cout.flush();
        FILE* p = popen(c.c_str(), "r");
        char buf[8192];
        size_t n;
        std::string text;
        while (p && (n = std::fread(buf, 1, sizeof buf, p)) > 0) text.append(buf, n);
        int st = p ? pclose(p) : 2 << 8;
        // msim -elab says "elaborated, not run": the elaboration step says nothing on success.
        std::stringstream ss(text);
        for (std::string ln; std::getline(ss, ln);) {
            if (ln.find("not run") != std::string::npos) continue;
            std::cout << ln << "\n";
            if (log) std::fprintf(log, "%s\n", ln.c_str());
        }
        if (log) std::fclose(log);
        int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
        if (rc != 0) { std::cerr << o.name << ": errors; " << simv << " not written\n"; return rc; }
    }
    std::error_code ec;
    fs::create_directories(daidir, ec);
    {
        std::ofstream a(daidir + "/msim.args");
        for (auto& x : cmd) a << x << "\n";
        std::ofstream s(simv);
        s << "#!/bin/sh\n# simv written by msim's vcs front end: msim runs the design recorded in " << simv
          << ".daidir/msim.args\nexec " << shq(self()) << " -simv " << shq(daidir + "/msim.args") << " \"$@\"\n";
        if (!a || !s) { std::cerr << o.name << ": cannot write " << simv << "\n"; return 2; }
    }
    fs::permissions(simv, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    if (!run) return 0;
    // -R: run simv now, with the plusargs of the vcs command line.
    std::string c = shq(absPath(simv));
    for (auto& x : rec)
        if (!x.empty() && x[0] == '+' && !starts(x, "+define+") && !starts(x, "+incdir+") && !starts(x, "+libext+"))
            c += " " + shq(x);
    if (!logPath.empty()) c += " -l " + shq(logPath) + " -append_log";
    std::cout.flush();
    int st = std::system(c.c_str());
    return WIFEXITED(st) ? WEXITSTATUS(st) : 2;
}

// simv <runtime options>: msim on the recorded command line.
inline bool simvRun(Opts& o, const std::string& argsFile, const std::vector<std::string>& args) {
    std::ifstream f(argsFile);
    if (!f) { std::cerr << "simv: cannot read " << argsFile << "\n"; return false; }
    for (std::string ln; std::getline(f, ln);) o.out.push_back(ln);
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "-l" && i + 1 < args.size()) { o.log = args[++i]; continue; }
        if (a == "-append_log") { o.appendLog = true; continue; }
        if (a == "-ucli") { o.out.push_back("-i"); continue; }
        if (a == "-do" && i + 1 < args.size()) { o.out.push_back("-do"); o.out.push_back(args[++i]); continue; }
        if (a == "-gui" || a == "-verdi") { std::cerr << "simv: " << a << ": no GUI here; open the waveform with mdebug (docs/DEBUG.md)\n"; continue; }
        if (a == "-q" || a == "+vcs+lic+wait" || starts(a, "+fsdb")) continue;
        if (starts(a, "+vcs+finish+")) { o.out.push_back("-maxtime"); o.out.push_back(a.substr(12)); continue; }
        if (starts(a, "+ntb_random_seed=") && lower(a.substr(17)) == "random") {   // a fresh seed each run
            o.out.push_back("-seed");
            o.out.push_back(seedValue("random", "simv"));
            continue;
        }
        o.out.push_back(a);   // plusargs, +ntb_random_seed=, -cm ..., msim's own options
    }
    return true;
}

// ---- entry ----

// Whether this front end is wanted. For the analysis steps, vlib, vmap and
// vcs, the step runs here and o.exitNow is its status. For vsim and simv,
// o.out is the msim command line to run (with o.log to copy the output to).
inline bool wanted(int argc, char** argv, Opts& o) {
    static const std::pair<const char*, Tool> names[] = {
        {"vlib", Tool::Vlib}, {"vmap", Tool::Vmap}, {"vlog", Tool::Vlog}, {"vcom", Tool::Vcom}, {"vsim", Tool::Vsim},
        {"vlogan", Tool::Vlogan}, {"vhdlan", Tool::Vhdlan}, {"vcs", Tool::Vcs}};
    std::string me = compat::basename(argv[0]);
    int skip = 1;
    for (auto& [n, t] : names) {
        if (me == n) { o.tool = t; o.name = n; }
        if (argc > 1 && std::string(argv[1]) == std::string("-") + n) { o.tool = t; o.name = n; skip = 2; }
    }
    if (argc > 2 && std::string(argv[1]) == "-simv") { o.tool = Tool::Simv; o.name = "simv"; skip = 3; }
    if (o.tool == Tool::None) return false;
    std::vector<std::string> args(argv + skip, argv + argc);
    // Licence and 32/64-bit options of every tool (compat.hpp).
    args = compat::translate(snps(o.tool) ? compat::Vendor::Synopsys : compat::Vendor::Siemens, args, {}).args;
    o.out.push_back(argv[0]);
    switch (o.tool) {
        case Tool::Vlib: o.exitNow = vlib(o, args); break;
        case Tool::Vmap: o.exitNow = vmap(o, args); break;
        case Tool::Vlog: case Tool::Vcom: case Tool::Vlogan: case Tool::Vhdlan: o.exitNow = analyse(o, args); break;
        case Tool::Vcs: o.exitNow = vcs(o, args); break;
        case Tool::Vsim: if (!vsim(o, args)) o.exitNow = 2; break;
        case Tool::Simv: if (!simvRun(o, argv[2], args)) o.exitNow = 2; break;
        default: break;
    }
    return true;
}

}   // namespace mstep
