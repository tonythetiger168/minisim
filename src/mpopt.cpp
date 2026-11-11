// mpopt.cpp - RTL power optimization on the msim front end: finds clock
// gating (from enables, and sequential), memory gating and operand
// isolation, rewrites the RTL source, and checks the result against the
// original. The engine and the reasoning are in popt.hpp; docs/POWEROPT.md
// is the user guide.
//
//   mpopt design.sv -top soc                 report what can be gated
//   mpopt design.sv -top soc -o out          and write the rewritten RTL to out/
//   mpopt design.sv -top soc -o out -verify  and co-simulate it against the original
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "popt.hpp"
#include "version.hpp"

using namespace lintir;
namespace fs = std::filesystem;

namespace {

void usage() {
    std::cout << "usage: mpopt [options] file.sv ...\n"
                 "  RTL power optimization: clock gating, memory gating, operand isolation\n\n"
                 "  -o <dir>         write the rewritten RTL, " "the clock-gate cell and files.f to <dir>\n"
                 "  -verify          simulate original and rewritten RTL on the same random stimulus\n"
                 "                   and compare every output and register, every cycle (needs -o)\n"
                 "  -verify_only     the same check on what is in <dir> now (after an edit by hand)\n"
                 "  -prove           prove the rewritten RTL equivalent to the original with mequiv (needs -o);\n"
                 "                   sequential gates are outside the proof: -verify covers those\n"
                 "  -prove_effort <n>  SAT conflicts mequiv may spend per compare point\n"
                 "  -power           power before and after, from mpower on the activity of the -verify run\n"
                 "  -lib <file.lib>  Liberty library for -power and -prove (default: mpower's generic technology)\n"
                 "  -cycles <n>      cycles for -verify (default 2000)\n"
                 "  -clock <port>    a clock port of the top, for -verify (default: found from the RTL)\n"
                 "  -reset <port>    an active-high reset port; -reset_n <port>: active-low\n"
                 "  -json <file>     the report as JSON\n"
                 "  -v               also list the registers that were not gated, and why\n"
                 "  -no_cg  -no_seq  -no_iso  -no_split   turn one transformation off\n"
                 "                   (enable gating, sequential gating, operand isolation,\n"
                 "                   splitting an always block whose registers have different enables)\n"
                 "  -min_bits <n>    smallest register group to gate (default 4)\n"
                 "  -seq_min_bits <n>  smallest group for a sequential gate (default 8)\n"
                 "  -seq_depth <n>   pipeline stages a sequential enable may follow (default 4)\n"
                 "  -iso_min_bits <n>  smallest multiplier/divider to isolate (default 8)\n"
                 "  -iso_add <n>     also isolate adders and subtractors of n bits or more\n"
                 "  -iso_plain       isolate with (c ? a : 0) instead of (c !== 1'b0 ? a : 0): for flows that\n"
                 "                   reject !==; an x in c then becomes an x in the result\n"
                 "  -iso_inline      rewrite the expression in place; do not add named isolation signals\n"
                 "  -icg_cell <cell>[:<clk>,<en>,<te>,<gclk>]  instantiate a library clock gate\n"
                 "  -test_enable <signal>  connect the gates' test enable to this signal where a module has it\n"
                 "  -prefix <name>   prefix of everything mpopt adds (default mpopt)\n"
                 "  -threads <n>     worker threads (default: every logical CPU; same result for any n)\n"
                 "  -quiet           print the summary only\n"
                 "  -top M  -f <file>  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
                 "  -version         print the version\n"
                 "  --help-slang     list every front-end option\n";
}

// -f files are read here, so that mpopt knows every argument it was given.
bool expandFile(const std::string& path, std::vector<std::string>& out, int depth = 0) {
    std::ifstream in(path);
    if (!in || depth > 16) return false;
    std::string line;
    while (std::getline(in, line)) {
        size_t c = line.find("//");
        if (c != std::string::npos) line.erase(c);
        if (!line.empty() && line[0] == '#') continue;
        std::istringstream ls(line);
        std::string tok;
        while (ls >> tok) {
            if (tok == "-f" || tok == "-F") {
                std::string sub;
                if (!(ls >> sub)) return false;
                if (fs::path(sub).is_relative() && tok == "-F") sub = (fs::path(path).parent_path() / sub).string();
                if (!expandFile(sub, out, depth + 1)) {
                    std::cerr << "mpopt: cannot read " << sub << "\n";
                    return false;
                }
            } else
                out.push_back(tok);
        }
    }
    return true;
}

std::string selfDir() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.parent_path().string();
}

std::string shq(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

struct PortRole {
    enum R { None, Clock, ResetHigh, ResetLow } r = None;
};

// Which ports of a module are clocks and resets, from how the blocks under it use them.
struct Roles {
    const popt::Design& d;
    std::unordered_map<std::string, const popt::Module*> byName;
    std::unordered_map<const InstanceBodySymbol*, std::map<std::string, PortRole::R>> memo;

    explicit Roles(const popt::Design& d) : d(d) {
        for (auto& m : d.mods) byName[m.name] = &m;
    }

    static const Symbol* portOf(const InstanceBodySymbol& body, const Symbol* internal) {
        for (auto p : body.getPortList())
            if (p->kind == SymbolKind::Port && p->as<PortSymbol>().internalSymbol == internal) return p;
        return nullptr;
    }

    const std::map<std::string, PortRole::R>& of(const InstanceBodySymbol& body) {
        auto it = memo.find(&body);
        if (it != memo.end()) return it->second;
        auto& r = memo[&body];
        auto mit = byName.find(std::string(body.getDefinition().name));
        if (mit != byName.end()) {
            // The analyzed body may be another instance of the definition: go by name.
            auto mark = [&](const Symbol* s, PortRole::R role) {
                if (!s) return;
                if (auto here = body.find(s->name); here && portOf(body, here)) r.emplace(std::string(s->name), role);
            };
            for (auto& b : mit->second->blks) {
                if (b.kind != popt::Blk::Seq) continue;
                mark(b.clk, PortRole::Clock);
                mark(b.arst, b.arstHigh ? PortRole::ResetHigh : PortRole::ResetLow);
                if (!b.syncText.empty()) {
                    std::string t = b.syncText;
                    bool neg = false;
                    while (!t.empty() && (t[0] == '!' || t[0] == '~' || t[0] == '(' || t[0] == ' ')) {
                        if (t[0] == '!' || t[0] == '~') neg = !neg;
                        t.erase(0, 1);
                    }
                    while (!t.empty() && (t.back() == ')' || t.back() == ' ')) t.pop_back();
                    if (auto s = body.find(t); s && portOf(body, s))
                        r.emplace(t, neg ? PortRole::ResetLow : PortRole::ResetHigh);
                }
            }
        }
        std::function<void(const Scope&)> rec = [&](const Scope& sc) {
            for (auto& mem : sc.members()) {
                if (mem.kind == SymbolKind::Instance) {
                    auto& inst = mem.as<InstanceSymbol>();
                    auto& sub = of(inst.body);
                    for (auto conn : inst.getPortConnections()) {
                        auto sit = sub.find(std::string(conn->port.name));
                        const Expression* e = conn->getExpression();
                        if (sit == sub.end() || !e) continue;
                        const Expression& u = popt::unwrap(*e);
                        if (u.kind != ExpressionKind::NamedValue) continue;
                        auto& s = u.as<NamedValueExpression>().symbol;
                        if (portOf(body, &s)) memo[&body].emplace(std::string(s.name), sit->second);
                    }
                } else if (mem.kind == SymbolKind::InstanceArray)
                    rec(mem.as<InstanceArraySymbol>());
                else if (mem.kind == SymbolKind::GenerateBlock) {
                    if (!mem.as<GenerateBlockSymbol>().isUninstantiated) rec(mem.as<GenerateBlockSymbol>());
                } else if (mem.kind == SymbolKind::GenerateBlockArray)
                    rec(mem.as<GenerateBlockArraySymbol>());
            }
        };
        rec(body);
        return memo[&body];
    }
};

// The registers of the design by hierarchical name, for the co-simulation to print.
void regPaths(const popt::Design& d, const std::unordered_map<std::string, const popt::Module*>& byName,
              const InstanceBodySymbol& body, const std::string& path, std::vector<std::string>& out, size_t cap) {
    auto mit = byName.find(std::string(body.getDefinition().name));
    if (mit != byName.end()) {
        std::vector<std::string> names;
        for (auto& [s, g] : mit->second->regGroup) {
            auto here = body.find(s->name);
            if (!here || (here->kind != SymbolKind::Variable && here->kind != SymbolKind::Net)) continue;
            if (!here->as<ValueSymbol>().getType().isIntegral()) continue;
            names.push_back(std::string(s->name));
        }
        std::sort(names.begin(), names.end());
        for (auto& n : names)
            if (out.size() < cap) out.push_back(path + "." + n);
    }
    for (auto& mem : body.members())
        if (mem.kind == SymbolKind::Instance && out.size() < cap) {
            auto& inst = mem.as<InstanceSymbol>();
            if (inst.isModule()) regPaths(d, byName, inst.body, path + "." + std::string(inst.name), out, cap);
        }
}

struct VerifyOpts {
    unsigned cycles = 2000;
    std::vector<std::string> clocks, resets, resetsN;
};

// A testbench that drives every input with random values and prints every
// output and register once per cycle. Empty when the top's ports cannot be driven.
std::string makeTb(const popt::Design& d, const InstanceSymbol& top, const VerifyOpts& vo, const std::string& prefix,
                   std::string& why) {
    Roles roles(d);
    auto role = roles.of(top.body);
    for (auto& c : vo.clocks) role[c] = PortRole::Clock;
    for (auto& c : vo.resets) role[c] = PortRole::ResetHigh;
    for (auto& c : vo.resetsN) role[c] = PortRole::ResetLow;
    std::ostringstream decl, conn, drive, rstOn, rstOff, clk;
    std::vector<std::string> outs;
    bool first = true, anyClock = false;
    std::string firstClock;
    for (auto p : top.body.getPortList()) {
        if (p->kind != SymbolKind::Port) { why = "port '" + std::string(p->name) + "' is an interface or a port list"; return ""; }
        auto& ps = p->as<PortSymbol>();
        auto& ty = ps.getType();
        if (!ty.isIntegral()) { why = "port '" + std::string(p->name) + "' is not a bit vector"; return ""; }
        if (ps.direction == ArgumentDirection::InOut) { why = "port '" + std::string(p->name) + "' is an inout"; return ""; }
        unsigned w = ty.getBitWidth();
        std::string n(p->name);
        decl << "  logic " << (w > 1 ? "[" + std::to_string(w - 1) + ":0] " : "") << n << ";\n";
        conn << (first ? "" : ", ") << "." << n << "(" << n << ")";
        first = false;
        if (ps.direction == ArgumentDirection::Out) { outs.push_back(n); continue; }
        auto rit = role.find(n);
        PortRole::R r = rit == role.end() ? PortRole::None : rit->second;
        if (r == PortRole::Clock) {
            clk << "  initial " << n << " = 1'b0;\n  always #5 " << n << " = ~" << n << ";\n";
            if (!anyClock) firstClock = n;
            anyClock = true;
        } else if (r == PortRole::ResetHigh || r == PortRole::ResetLow) {
            rstOn << "      " << n << " = " << (r == PortRole::ResetHigh ? "'1" : "'0") << ";\n";
            rstOff << "      " << n << " = " << (r == PortRole::ResetHigh ? "'0" : "'1") << ";\n";
        } else {
            // One 32-bit draw per statement, so the order of the draws is fixed.
            if (w <= 32) drive << "      " << n << " = rnd();\n";
            else
                for (unsigned i = 0; i < (w + 31) / 32; i++)
                    drive << "      " << n << "[" << std::min(w - 1, i * 32 + 31) << ":" << i * 32 << "] = rnd();\n";
        }
    }
    if (!anyClock) { why = "no clock port was found (name one with -clock)"; return ""; }
    std::unordered_map<std::string, const popt::Module*> byName;
    for (auto& m : d.mods) byName[m.name] = &m;
    std::vector<std::string> regs;
    regPaths(d, byName, top.body, "dut", regs, 4000);
    std::ostringstream tb;
    tb << "// Written by mpopt -verify: random stimulus, one line per cycle.\n"
       << "module " << prefix << "_tb;\n" << decl.str() << "  " << top.body.getDefinition().name << " dut (" << conn.str()
       << ");\n" << clk.str()
       << "  // xorshift32: the same numbers in every simulator and for both designs\n"
       << "  reg [31:0] seed = 32'h2545f491;\n"
       << "  function automatic [31:0] rnd();\n    begin\n      seed = seed ^ (seed << 13);\n"
       << "      seed = seed ^ (seed >> 17);\n      seed = seed ^ (seed << 5);\n      rnd = seed;\n    end\n  endfunction\n"
       << "  task automatic drive();\n    begin\n" << drive.str() << "    end\n  endtask\n"
       << "  task automatic reset_on();\n    begin\n" << rstOn.str() << "    end\n  endtask\n"
       << "  task automatic reset_off();\n    begin\n" << rstOff.str() << "    end\n  endtask\n"
       << "  // +mpopt_vcd=<file>: the design's activity, for mpopt -power\n"
       << "  string vcd_file;\n"
       << "  initial if ($value$plusargs(\"mpopt_vcd=%s\", vcd_file)) begin\n"
       << "    $dumpfile(vcd_file);\n    $dumpvars(0, " << prefix << "_tb.dut);\n  end\n"
       << "  integer i;\n  initial begin\n    reset_on();\n    drive();\n"
       << "    repeat (3) begin @(negedge " << firstClock << "); drive(); end\n    reset_off();\n"
       << "    for (i = 0; i < " << vo.cycles << "; i = i + 1) begin\n      @(negedge " << firstClock << ");\n";
    auto show = [&](const char* tag, const std::vector<std::string>& v) {
        for (size_t at = 0; at < v.size(); at += 16) {
            tb << "      $display(\"" << tag << " %0d";
            for (size_t k = at; k < std::min(v.size(), at + 16); k++) tb << " %h";
            tb << "\", i";
            for (size_t k = at; k < std::min(v.size(), at + 16); k++) tb << ", " << v[k];
            tb << ");\n";
        }
    };
    show("o", outs);
    show("r", regs);
    tb << "      drive();\n"
       << "      if (i == " << vo.cycles / 2 << ") reset_on();\n"
       << "      if (i == " << vo.cycles / 2 + 2 << ") reset_off();\n"
       << "    end\n    $finish;\n  end\nendmodule\n";
    return tb.str();
}

bool writeFile(const fs::path& p, const std::string& text) {
    std::ofstream os(p, std::ios::binary);
    os << text;
    return (bool)os;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "-f" || a == "-F") && i + 1 < argc) {
            if (!expandFile(argv[++i], args)) {
                std::cerr << "mpopt: cannot read " << argv[i] << "\n";
                return 2;
            }
        } else
            args.push_back(a);
    }
    popt::Options opt;
    opt.threads = par::defaultThreads();
    VerifyOpts vo;
    std::vector<std::string> fwd{argv[0]}, simArgs;
    std::string outDir, jsonPath;
    bool verify = false, verifyOnly = false, quiet = false, verbose = false, threadsSet = false;
    bool prove = false, power = false;
    unsigned proveEffort = 0;
    std::string topName;
    std::vector<std::string> libs;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= args.size()) {
                std::cerr << "mpopt: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return args[++i];
        };
        auto num = [&](const char* what) -> unsigned {
            std::string v = next(what);
            char* end = nullptr;
            long n = std::strtol(v.c_str(), &end, 10);
            if (!end || *end || n < 0) {
                std::cerr << "mpopt: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return (unsigned)n;
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-version" || a == "--version") {
            std::cout << "mpopt " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-o") { outDir = next("a directory"); continue; }
        if (a == "-verify") { verify = true; continue; }
        if (a == "-verify_only") { verify = verifyOnly = true; continue; }
        if (a == "-prove") { prove = true; continue; }
        if (a == "-prove_effort") { proveEffort = num("a number of conflicts"); continue; }
        if (a == "-power") { power = verify = true; continue; }
        if (a == "-lib") { libs.push_back(next("a Liberty file")); continue; }
        if (a == "-cycles") { vo.cycles = num("a number of cycles"); continue; }
        if (a == "-clock") { vo.clocks.push_back(next("a port name")); continue; }
        if (a == "-reset") { vo.resets.push_back(next("a port name")); continue; }
        if (a == "-reset_n") { vo.resetsN.push_back(next("a port name")); continue; }
        if (a == "-json") { jsonPath = next("a file name"); continue; }
        if (a == "-v" || a == "-verbose") { verbose = true; continue; }
        if (a == "-quiet" || a == "-q") { quiet = true; continue; }
        if (a == "-no_cg") { opt.cg = false; opt.seq = false; continue; }
        if (a == "-no_seq") { opt.seq = false; continue; }
        if (a == "-no_iso") { opt.iso = false; continue; }
        if (a == "-no_split") { opt.split = false; continue; }
        if (a == "-min_bits") { opt.minBits = num("a number of bits"); continue; }
        if (a == "-seq_min_bits") { opt.seqMinBits = num("a number of bits"); continue; }
        if (a == "-seq_depth") { opt.seqDepth = num("a number of stages"); continue; }
        if (a == "-iso_min_bits") { opt.isoMinBits = num("a number of bits"); continue; }
        if (a == "-iso_add") { opt.isoAddBits = num("a number of bits"); continue; }
        if (a == "-iso_plain") { opt.isoPlain = true; continue; }
        if (a == "-iso_inline") { opt.isoInline = true; continue; }
        if (a == "-prefix") { opt.prefix = next("a name"); continue; }
        if (a == "-test_enable") { opt.testEnable = next("a signal name"); continue; }
        if (a == "-icg_cell") {
            std::string v = next("a cell name");
            size_t c = v.find(':');
            opt.icgCell = v.substr(0, c);
            if (c != std::string::npos) {
                std::vector<std::string> pins;
                std::istringstream ps(v.substr(c + 1));
                std::string p;
                while (std::getline(ps, p, ',')) pins.push_back(p);
                if (pins.size() != 4) {
                    std::cerr << "mpopt: -icg_cell needs four pins: cell:clk,en,te,gclk\n";
                    return 2;
                }
                opt.icgClk = pins[0];
                opt.icgEn = pins[1];
                opt.icgTe = pins[2];
                opt.icgOut = pins[3];
            }
            continue;
        }
        if (a == "-threads" || a == "--threads" || a == "-j") {
            opt.threads = num("a number of threads (1 or more)");
            if (opt.threads < 1) {
                std::cerr << "mpopt: " << a << " needs a number of threads (1 or more)\n";
                return 2;
            }
            threadsSet = true;
            continue;
        }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { topName = next("a module name"); fwd.push_back("--top"); fwd.push_back(topName); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        // Defines and include directories go to msim, mequiv and mpower too, in the
        // one spelling all of them read, with directories made absolute.
        auto incdir = [&](const std::string& dir) {
            std::error_code ec2;
            fs::path abs = fs::absolute(dir, ec2);
            simArgs.push_back("+incdir+" + (ec2 ? dir : abs.string()));
        };
        if (a.rfind("+define+", 0) == 0) simArgs.push_back(a);
        else if (a.rfind("+incdir+", 0) == 0) incdir(a.substr(8));
        else if ((a == "-I" || a == "-D") && i + 1 < args.size()) {
            if (a == "-I") incdir(args[i + 1]);
            else simArgs.push_back("+define+" + args[i + 1]);
            fwd.push_back(a);
            fwd.push_back(args[++i]);
            continue;
        } else if (a.size() > 2 && a.rfind("-I", 0) == 0) incdir(a.substr(2));
        else if (a.size() > 2 && a.rfind("-D", 0) == 0) simArgs.push_back("+define+" + a.substr(2));
        fwd.push_back(a);
    }
    // The front end parses the source files on the same number of threads.
    if (threadsSet) {
        fwd.push_back("--threads");
        fwd.push_back(std::to_string(opt.threads));
    }
    if ((verify || prove) && outDir.empty()) {
        std::cerr << "mpopt: " << (prove ? "-prove" : power ? "-power" : "-verify") << " needs -o <dir>\n";
        return 2;
    }

    auto t0 = std::chrono::steady_clock::now();
    FrontEnd fe;
    if (int rc = fe.run(fwd, "mpopt front end")) return rc < 0 ? 0 : rc;
    auto t1 = std::chrono::steady_clock::now();
    const SourceManager& sm = *fe.comp->getSourceManager();
    popt::Design d = popt::run(*fe.comp, opt);
    auto t2 = std::chrono::steady_clock::now();

    if (!quiet) popt::printReport(std::cout, d, verbose);
    else {
        std::ostringstream all;
        popt::printReport(all, d, false);
        std::string s = all.str();
        size_t at = s.find("mpopt: ");
        std::cout << (at == std::string::npos ? s : s.substr(at));
    }
    if (!jsonPath.empty()) {
        std::ofstream js(jsonPath);
        popt::writeJson(js, d);
        if (!js) {
            std::cerr << "mpopt: cannot write " << jsonPath << "\n";
            return 2;
        }
    }
    if (std::getenv("MPOPT_TIME")) {
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::cerr << "mpopt: front end " << ms(t0, t1) << " ms, analysis " << ms(t1, t2) << " ms (instances "
                  << d.msCollect << ", elaborate " << d.msTouch << ", analyze " << d.msAnalyze << ", totals "
                  << d.msMerge << "), " << opt.threads << " threads\n";
    }
    if (outDir.empty()) return 0;

    // ---- write the rewritten design
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        std::cerr << "mpopt: cannot create " << outDir << "\n";
        return 2;
    }
    struct Emitted {
        std::vector<std::string> origFiles, newFiles;
        std::set<std::string> origDirs;
        bool incChanged = false, hasSeq = false, ok = true;
    };
    // Writes one rewritten design (or, with write off, only names its files).
    auto emit = [&](const popt::Design& dd, const std::string& dir, bool write) {
        Emitted e;
        auto texts = popt::rewrite(sm, dd, opt.threads);
        for (auto& [b, t] : texts)
            if (t.find(opt.prefix + "_st_") != std::string::npos) e.hasSeq = true;
        // Top-level source files in command-line order, then included files that changed.
        std::vector<std::pair<uint32_t, bool>> bufs;   // buffer, is an include
        std::set<uint32_t> have;
        for (auto& tree : fe.driver.syntaxTrees)
            for (BufferID b : tree->getSourceBufferIds()) {
                if (sm.getIncludedFrom(b)) continue;
                if (sm.getFullPath(b).empty()) continue;
                if (have.insert(b.getId()).second) bufs.push_back({b.getId(), false});
            }
        for (auto& [b, t] : texts)
            if (have.insert(b).second) bufs.push_back({b, true});
        std::set<std::string> usedNames;
        for (auto& [b, inc] : bufs) {
            BufferID id(b, ""sv);
            fs::path orig = sm.getFullPath(id);
            std::string base = orig.filename().string();
            // Two inputs of one name: keep both.
            std::string name = base;
            for (int k = 2; !usedNames.insert(name).second; k++)
                name = orig.stem().string() + "_" + std::to_string(k) + orig.extension().string();
            auto it = texts.find(b);
            std::string text;
            if (it != texts.end()) text = it->second;
            else {
                std::string_view sv = sm.getSourceText(id);
                if (!sv.empty() && sv.back() == '\0') sv.remove_suffix(1);
                text = std::string(sv);
            }
            if (write && !writeFile(fs::path(dir) / name, text)) {
                std::cerr << "mpopt: cannot write " << (fs::path(dir) / name).string() << "\n";
                e.ok = false;
                return e;
            }
            if (inc) e.incChanged = true;
            else {
                e.origFiles.push_back(orig.string());
                e.newFiles.push_back((fs::path(dir) / name).string());
                e.origDirs.insert(orig.parent_path().string());
            }
        }
        bool anyIcg = false;
        for (auto& m : dd.mods) anyIcg |= m.usesIcg;
        std::string cells = (fs::path(dir) / (opt.prefix + "_cells.sv")).string();
        if (anyIcg && opt.icgCell.empty()) {
            if (write) writeFile(cells, popt::cellText(opt));
            e.newFiles.push_back(cells);
        }
        if (write) {
            std::ostringstream ff;
            if (e.incChanged) ff << "+incdir+" << dir << "\n";
            for (auto& dname : e.origDirs) ff << "+incdir+" << dname << "\n";
            for (auto& sa : simArgs) ff << sa << "\n";
            for (auto& f : e.newFiles) ff << f << "\n";
            writeFile(fs::path(dir) / "files.f", ff.str());
            std::ofstream js(fs::path(dir) / (opt.prefix + ".json"));
            popt::writeJson(js, dd);
        }
        return e;
    };
    Emitted em = emit(d, outDir, !verifyOnly);
    if (!em.ok) return 2;
    const std::vector<std::string>&origFiles = em.origFiles, &newFiles = em.newFiles;
    const std::set<std::string>& origDirs = em.origDirs;
    const bool incChanged = em.incChanged;
    if (!quiet && !verifyOnly) std::cout << "mpopt: wrote " << newFiles.size() << " files to " << outDir << "\n";
    auto tool = [&](const char* name, const char* env) {
        std::string t = name;
        if (std::string sd = selfDir(); !sd.empty() && fs::exists(fs::path(sd) / name)) t = (fs::path(sd) / name).string();
        if (const char* e = std::getenv(env)) t = e;
        return t;
    };
    auto exitOf = [](int rc) { return rc == -1 ? 127 : WIFEXITED(rc) ? WEXITSTATUS(rc) : 128; };
    if (topName.empty()) {
        auto tops = fe.comp->getRoot().topInstances;
        if (tops.size() == 1) topName = std::string(tops[0]->body.getDefinition().name);
    }
    int result = 0;   // 1: a check failed, 3: the proof was inconclusive

    // ---- prove with mequiv
    if (prove) {
        // A sequential gate is right because of what held one cycle before, which a
        // combinational proof cannot see: the proof is of the rewrite without them.
        std::string dir = outDir;
        const Emitted* pe = &em;
        Emitted noSeq;
        if (em.hasSeq) {
            popt::Options o2 = opt;
            o2.seq = false;
            popt::Design d2 = popt::run(*fe.comp, o2);
            dir = (fs::path(outDir) / "prove").string();
            fs::create_directories(dir, ec);
            noSeq = emit(d2, dir, true);
            if (ec || !noSeq.ok) return 2;
            pe = &noSeq;
        }
        std::string log = (fs::path(outDir) / "prove.log").string();
        std::string c = shq(tool("mequiv", "MPOPT_MEQUIV")) + " -q -threads " + std::to_string(opt.threads);
        if (!topName.empty()) c += " -top " + shq(topName);
        if (proveEffort) c += " -effort " + std::to_string(proveEffort);
        for (auto& l : libs) c += " -lib " + shq(l);
        c += " -report " + shq((fs::path(outDir) / "prove.json").string());
        for (auto& sa : simArgs) c += " " + shq(sa);
        c += " -ref";
        for (auto& f : pe->origFiles) c += " " + shq(f);
        c += " -impl -f " + shq((fs::path(dir) / "files.f").string()) + " > " + shq(log) + " 2>&1";
        auto p0 = std::chrono::steady_clock::now();
        int rc = exitOf(std::system(c.c_str()));
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - p0).count();
        char took[32];
        std::snprintf(took, sizeof took, "%.2f s", sec);
        const char* scope = em.hasSeq ? "the rewrite without sequential gates (in prove/)" : "the rewritten RTL";
        if (rc == 0) {
            std::cout << "mpopt: -prove passed: mequiv proved " << scope << " equivalent to the original (" << took << ")\n";
            if (em.hasSeq)
                std::cout << "mpopt: -prove: sequential gates are outside the proof; -verify checks them in simulation\n";
        } else if (rc == 1) {
            std::cerr << "mpopt: -prove FAILED: mequiv found a difference in " << scope << "; see " << log << "\n";
            result = 1;
        } else if (rc == 3) {
            std::cerr << "mpopt: -prove inconclusive: mequiv ran out of effort on " << scope << " (raise -prove_effort); see "
                      << log << "\n";
            result = 3;
        } else {
            std::cerr << "mpopt: -prove: mequiv could not read the design (exit " << rc << "); see " << log << "\n";
            result = 2;
        }
    }
    if (!verify) return result;

    // ---- co-simulate
    auto tops = fe.comp->getRoot().topInstances;
    if (tops.size() != 1) {
        std::cerr << "mpopt: -verify needs one top module (use -top)\n";
        return 2;
    }
    std::string why;
    std::string tb = makeTb(d, *tops[0], vo, opt.prefix, why);
    if (tb.empty()) {
        std::cerr << "mpopt: -verify cannot drive the top module: " << why << "\n";
        return 2;
    }
    std::string tbPath = (fs::path(outDir) / (opt.prefix + "_tb.sv")).string();
    writeFile(tbPath, tb);
    std::string msim = tool("msim", "MPOPT_MSIM");
    auto command = [&](const std::vector<std::string>& files, bool rewritten, const std::string& log) {
        std::string c = shq(msim);
        if (rewritten && incChanged) c += " " + shq("+incdir+" + outDir);
        if (rewritten)
            for (auto& dname : origDirs) c += " " + shq("+incdir+" + dname);
        for (auto& s : simArgs) c += " " + shq(s);
        for (auto& f : files) c += " " + shq(f);
        c += " " + shq(tbPath) + " -top " + opt.prefix + "_tb";
        if (power) c += " " + shq("+mpopt_vcd=" + (fs::path(outDir) / (rewritten ? "power_opt.vcd" : "power_ref.vcd")).string());
        c += " > " + shq(log) + " 2> " + shq(log + ".err");
        return c;
    };
    std::string refLog = (fs::path(outDir) / "verify_ref.log").string();
    std::string optLog = (fs::path(outDir) / "verify_opt.log").string();
    int rc[2] = {0, 0};
    {
        // The two simulations are independent: run them side by side.
        std::thread a([&] { rc[0] = std::system(command(origFiles, false, refLog).c_str()); });
        if (opt.threads > 1) {
            std::thread b([&] { rc[1] = std::system(command(newFiles, true, optLog).c_str()); });
            b.join();
            a.join();
        } else {
            a.join();
            rc[1] = std::system(command(newFiles, true, optLog).c_str());
        }
    }
    // Only the testbench's own lines are compared: the design's messages
    // (assertions, $display) carry file names, which differ.
    auto own = [](const std::string& log) {
        std::string o;
        std::istringstream in(log);
        std::string line;
        while (std::getline(in, line))
            if (line.size() > 2 && (line[0] == 'o' || line[0] == 'r') && line[1] == ' ' &&
                std::isdigit((unsigned char)line[2]))
                o += line + "\n";
        return o;
    };
    std::string ref = own(readFile(refLog)), got = own(readFile(optLog));
    if (rc[0] != 0 || rc[1] != 0) {
        std::cerr << "mpopt: -verify: msim failed on the " << (rc[0] ? "original" : "rewritten") << " design; see "
                  << (rc[0] ? refLog : optLog) << "\n";
        return 1;
    }
    size_t lines = (size_t)std::count(ref.begin(), ref.end(), '\n');
    if (ref != got || lines < vo.cycles) {
        // Name the first cycle that differs.
        std::istringstream ra(ref), rb(got);
        std::string la, lb;
        size_t n = 0;
        while (true) {
            bool ea = !std::getline(ra, la), eb = !std::getline(rb, lb);
            n++;
            if (ea || eb || la != lb) break;
        }
        std::cerr << "mpopt: -verify FAILED at line " << n << " of the logs\n  original:  " << la
                  << "\n  rewritten: " << lb << "\n  logs: " << refLog << " " << optLog << "\n";
        return 1;
    }
    std::cout << "mpopt: -verify passed: " << vo.cycles << " cycles of random stimulus, every output and register equal ("
              << lines << " lines compared)\n";
    if (!power) return result;

    // ---- power before and after: mpower on the activity of the two runs
    std::string mpower = tool("mpower", "MPOPT_MPOWER");
    auto powerOf = [&](bool rewritten, double& total, double& clock, double& reg, double& comb) {
        std::string tag = rewritten ? "power_opt" : "power_ref";
        std::string json = (fs::path(outDir) / (tag + ".json")).string(), log = (fs::path(outDir) / (tag + ".log")).string();
        std::string c = shq(mpower) + " -q -threads " + std::to_string(opt.threads);
        if (!topName.empty()) c += " -top " + shq(topName);
        for (auto& l : libs) c += " -lib " + shq(l);
        c += " -activity " + shq((fs::path(outDir) / (tag + ".vcd")).string()) + " -strip_path " + opt.prefix + "_tb.dut";
        c += " -json " + shq(json);
        if (rewritten) c += " -f " + shq((fs::path(outDir) / "files.f").string());
        else {
            for (auto& sa : simArgs) c += " " + shq(sa);
            for (auto& f : origFiles) c += " " + shq(f);
        }
        c += " > " + shq(log) + " 2>&1";
        if (exitOf(std::system(c.c_str())) != 0) return false;
        std::string j = readFile(json);
        // "total" of the whole design, and of three groups, in watts.
        auto field = [&](const std::string& obj) {
            size_t at = j.find("\"" + obj + "\": {");
            if (at == std::string::npos) return -1.0;
            size_t end = j.find('}', at), t = j.find("\"total\":", at + obj.size() + 3);
            if (t == std::string::npos || t > end) return -1.0;
            return std::strtod(j.c_str() + t + 8, nullptr);
        };
        total = field("total");
        clock = field("clock_network");
        reg = field("register");
        comb = field("combinational");
        return total >= 0;
    };
    double a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
    bool okA = false, okB = false;
    {
        std::thread ta([&] { okA = powerOf(false, a[0], a[1], a[2], a[3]); });
        if (opt.threads > 1) {
            std::thread tb2([&] { okB = powerOf(true, b[0], b[1], b[2], b[3]); });
            tb2.join();
            ta.join();
        } else {
            ta.join();
            okB = powerOf(true, b[0], b[1], b[2], b[3]);
        }
    }
    if (!okA || !okB) {
        std::cerr << "mpopt: -power: mpower failed on the " << (okA ? "rewritten" : "original") << " design; see "
                  << (fs::path(outDir) / (okA ? "power_opt.log" : "power_ref.log")).string() << "\n";
        return 2;
    }
    auto row = [&](const char* name, double x, double y) {
        char line[160];
        double pct = x > 0 ? (y - x) / x * 100.0 : 0.0;
        std::snprintf(line, sizeof line, "  %-15s %10.2f uW  ->  %10.2f uW  (%+.1f%%)\n", name, x * 1e6, y * 1e6, pct);
        std::cout << line;
    };
    std::cout << "mpopt: -power: before and after, on the " << vo.cycles << " cycles of random stimulus of -verify"
              << (libs.empty() ? " (generic technology)" : "") << "\n";
    row("total", a[0], b[0]);
    if (a[1] >= 0 && b[1] >= 0) row("clock network", a[1], b[1]);
    if (a[2] >= 0 && b[2] >= 0) row("registers", a[2], b[2]);
    if (a[3] >= 0 && b[3] >= 0) row("combinational", a[3], b[3]);
    std::remove((fs::path(outDir) / "power_ref.vcd").string().c_str());
    std::remove((fs::path(outDir) / "power_opt.vcd").string().c_str());
    return result;
}
