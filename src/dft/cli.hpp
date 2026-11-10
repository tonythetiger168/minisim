// cli.hpp - the boundary-scan and IJTAG options of mdft.
//
//   mdft -bscan [options] design.sv -top M     insert boundary scan, write BSDL
//   mdft -bsdl chip.bsdl [-svf F] [-tb F]      check a BSDL file, write its tests
//   mdft -icl net.icl [-pdl t.pdl] [...]       IJTAG: network RTL and retargeted PDL
//
// parse() runs the flows that need no design and returns their exit code; it
// returns -1 when mdft should go on (with the options of this file removed
// from argv). After the front end, a -bscan run finishes in runBscan().
#pragma once
#include <chrono>
#include <thread>

#include "bscan.hpp"
#include "lbist.hpp"
#include "pdl.hpp"

namespace dft {

struct Cli {
    bool bscan = false, lbist = false, werror = false, quiet = false, ijtagTap = false;
    BscanOpts bo;
    LbistOpts lo;
    std::string lbistDump, self;
    std::string bsdlFile, svf, tb, iclTop, iclScanOut, iclRtl, iproc;
    std::vector<std::string> icl, pdl;
    std::vector<char*> rest;
    int threads = 0;

    static void usage(std::ostream& os) {
        os << "boundary scan (IEEE 1149.1):\n"
              "  -bscan                 wrap the top module: TAP, boundary register, BSDL\n"
              "  -bscan_out DIR         where <top>_bscan.sv and <top>_bscan.bsdl go (default .)\n"
              "  -bscan_idcode HEX      the 32-bit device identification code\n"
              "  -bscan_no_idcode       no IDCODE register\n"
              "  -bscan_ir N            instruction register length (default 4)\n"
              "  -bscan_clock PORT      observe-only cell: the clock path is not touched (repeatable;\n"
              "                         clocks that mdft finds itself are treated this way too)\n"
              "  -bscan_exclude PORT    no cell on this port (repeatable)\n"
              "  -bscan_tristate PAD=OUT,OE[,IN]   one bidirectional pad from the core's out, enable\n"
              "                         and in ports (!OE for an active-low enable)\n"
              "  -bscan_highz           every output gets a shared enable cell; adds HIGHZ\n"
              "  -bscan_intest          add INTEST\n"
              "  -bscan_user NAME       an instruction that selects an external register (IJTAG host)\n"
              "  -bsdl FILE             check a BSDL file (rules BSD001..BSD009)\n"
              "logic BIST:\n"
              "  -lbist                 wrap a scan-inserted design with a PRPG, a MISR and a controller\n"
              "  -scandef FILE          the scan chains (JSON from mdft -insert_scan -scandef)\n"
              "  -lbist_out DIR         where <top>_lbist.sv, its testbench, ICL and PDL go (default .)\n"
              "  -lbist_patterns N      patterns to apply (default 1024)\n"
              "  -lbist_prpg N          PRPG width, 8..64 (default 32, more for many chains)\n"
              "  -lbist_misr N          MISR width, 8..64 (default 32)\n"
              "  -lbist_seed HEX        PRPG seed          -lbist_clock PORT  the BIST clock\n"
              "  -lbist_sign            simulate with msim and write the golden signature into the RTL\n"
              "  -lbist_msim PATH       the msim to run (default: next to mdft, then PATH)\n"
              "  -lbist_dump FILE       the pattern loads, one line per chain, scan-in first\n"
              "IJTAG (IEEE 1687):\n"
              "  -icl FILE              read ICL (repeatable)     -icl_top M   the top ICL module\n"
              "  -icl_scan_out PORT     the ScanOutPort to use when the top module has several\n"
              "  -icl_rtl FILE          write the scan network as SystemVerilog\n"
              "  -ijtag_tap             put the network behind a TAP (instruction IJTAG)\n"
              "  -pdl FILE              run PDL and retarget it (repeatable)\n"
              "  -iproc NAME            then call this iProc of the top module\n"
              "test output (with -bscan, -bsdl or -pdl):\n"
              "  -svf FILE              the tests as SVF\n"
              "  -tb FILE               a self-checking SystemVerilog testbench\n";
    }

    // Returns -1 to continue into mdft, otherwise the exit code.
    int parse(int& argc, char**& argv) {
        bool mine = false;
        rest.push_back(argv[0]);
        self = argv[0];
        try {
            for (int i = 1; i < argc; i++) {
                std::string a = argv[i];
                auto next = [&]() -> std::string {
                    if (i + 1 >= argc) throw Error(a + " needs a value");
                    return argv[++i];
                };
                if (a == "-bscan") bscan = mine = true;
                else if (a == "-bscan_out") bo.outDir = next();
                else if (a == "-bscan_idcode") bo.idcode = (uint32_t)std::stoul(next(), nullptr, 16);
                else if (a == "-bscan_no_idcode") bo.hasId = false;
                else if (a == "-bscan_ir") bo.irLen = std::stoi(next());
                else if (a == "-bscan_clock") bo.clocks.insert(next());
                else if (a == "-bscan_exclude") bo.exclude.insert(next());
                else if (a == "-bscan_highz") bo.highz = true;
                else if (a == "-bscan_intest") bo.intest = true;
                else if (a == "-bscan_user") bo.user.push_back(next());
                else if (a == "-bscan_tristate") {
                    std::string v = next();
                    size_t eq = v.find('=');
                    std::vector<std::string> f = eq == std::string::npos ? std::vector<std::string>{} : detail::splitTop(v.substr(eq + 1));
                    if (f.size() < 2 || f.size() > 3) throw Error("-bscan_tristate wants PAD=OUT,OE[,IN]");
                    TriGroup g;
                    g.pad = v.substr(0, eq), g.out = f[0], g.oe = f[1];
                    if (!g.oe.empty() && g.oe[0] == '!') g.oeLow = true, g.oe.erase(0, 1);
                    if (f.size() == 3) g.in = f[2];
                    bo.tri.push_back(g);
                } else if (a == "-lbist") lbist = mine = true;
                else if (a == "-scandef") lo.scandef = next();
                else if (a == "-lbist_out") lo.outDir = next();
                else if (a == "-lbist_patterns") lo.patterns = std::stoi(next());
                else if (a == "-lbist_prpg") lo.prpg = std::stoi(next());
                else if (a == "-lbist_misr") lo.misr = std::stoi(next());
                else if (a == "-lbist_seed") lo.seed = std::stoull(next(), nullptr, 16);
                else if (a == "-lbist_clock") lo.clock = next();
                else if (a == "-lbist_sign") lo.sign = true;
                else if (a == "-lbist_msim") lo.msim = next();
                else if (a == "-lbist_dump") lbistDump = next();
                else if (a == "-bsdl") bsdlFile = next(), mine = true;
                else if (a == "-icl") icl.push_back(next()), mine = true;
                else if (a == "-icl_top") iclTop = next();
                else if (a == "-icl_scan_out") iclScanOut = next();
                else if (a == "-icl_rtl") iclRtl = next();
                else if (a == "-ijtag_tap") ijtagTap = true;
                else if (a == "-pdl") pdl.push_back(next());
                else if (a == "-iproc") iproc = next();
                else if (a == "-svf") svf = next();
                else if (a == "-tb") tb = next();
                else {
                    if (a == "-Werror") werror = true;
                    if (a == "-quiet" || a == "-q") quiet = true;
                    if (a == "-threads" && i + 1 < argc) {
                        // Accepted for uniformity; these flows are single passes over small inputs.
                        threads = std::atoi(argv[++i]);
                        continue;
                    }
                    rest.push_back(argv[i]);
                }
            }
            if (!mine) {
                if (!pdl.empty() || !iclRtl.empty()) throw Error("-pdl and -icl_rtl need -icl");
                if (!svf.empty() || !tb.empty()) throw Error("-svf and -tb need -bscan, -bsdl or -pdl");
                return -1;
            }
            if (lbist && bscan) throw Error("-lbist and -bscan are separate runs");
            if (lbist && lo.scandef.empty()) throw Error("-lbist needs -scandef FILE");
            if (!bsdlFile.empty()) return runBsdl(bsdlFile, true);
            if (!icl.empty()) return runIjtag();
        } catch (const Error& e) {
            std::cerr << "mdft: " << e.what() << "\n";
            return 2;
        } catch (const std::exception& e) {
            std::cerr << "mdft: bad option value (" << e.what() << ")\n";
            return 2;
        }
        argc = (int)rest.size();
        rest.push_back(nullptr);
        argv = rest.data();
        return -1;
    }

    static std::string stem(const std::string& path) {
        size_t s = path.find_last_of("/\\"), d = path.rfind('.');
        std::string b = path.substr(s == std::string::npos ? 0 : s + 1, d == std::string::npos || (s != std::string::npos && d < s) ? std::string::npos : d - (s == std::string::npos ? 0 : s + 1));
        for (char& c : b)
            if (!std::isalnum((unsigned char)c) && c != '_') c = '_';
        return b;
    }
    void writeTests(const Pattern& p, const TbSpec& spec, const std::string& title, bool trst) {
        if (!svf.empty()) {
            std::ostringstream os;
            writeSvf(os, p, title, trst);
            writeFile(svf, os.str());
        }
        if (!tb.empty()) {
            std::ostringstream os;
            writeTb(os, p, spec, stem(tb));
            writeFile(tb, os.str());
        }
    }

    int runBsdl(const std::string& file, bool report) {
        Bsdl d = parseBsdl(readFile(file));
        int n = checkBsdl(d);
        for (const BsdlFinding& f : d.findings) std::cout << file << ": [" << f.rule << "] " << f.text << "\n";
        bool blocking = false;  // BSD008 and BSD009 do not stop test generation
        for (const BsdlFinding& f : d.findings)
            if (f.rule != "BSD008" && f.rule != "BSD009") blocking = true;
        if (report && !quiet) {
            std::cout << "mdft: " << d.entity << ": " << d.irLen << "-bit instruction register, " << d.instrs.size()
                      << " instructions, " << d.blen << " boundary cells, " << n << (n == 1 ? " finding\n" : " findings\n");
        }
        if (!svf.empty() || !tb.empty()) {
            if (blocking) {
                std::cerr << "mdft: " << file << " has errors; no tests written\n";
                return 1;
            }
            TbSpec spec;
            Pattern p = bsdlPatterns(d, spec);
            // Testbench-only drivers carry no port name.
            std::vector<TbPin> pins;
            std::string extra;
            for (const TbPin& pin : spec.pins) {
                if (!pin.name.empty()) pins.push_back(pin);
                else extra += "  " + pin.decl + (pin.init.empty() ? "" : " = " + pin.init) + ";\n";
            }
            spec.pins = pins;
            if (!svf.empty()) {
                std::ostringstream os;
                writeSvf(os, p, "boundary-scan tests for " + d.entity, !d.trst.empty());
                writeFile(svf, os.str());
            }
            if (!tb.empty()) {
                std::ostringstream os;
                writeTb(os, p, spec, stem(tb));
                std::string text = os.str();
                size_t at = text.find("  int errors = 0;\n");
                text.insert(at, extra);
                writeFile(tb, text);
            }
            if (!quiet)
                std::cout << "mdft: " << p.ops.size() << " test steps, " << p.cycles() << " TCK cycles\n";
        }
        return werror && n ? 1 : 0;
    }

    // After the front end: the top module's ports are known.
    int runBscan(std::vector<Port> ports, const std::string& top, const std::set<std::string>& clocks) {
        try {
            bo.top = top;
            for (const std::string& c : clocks) bo.clocks.insert(c);
            BscanResult r = insertBscan(ports, bo);
            if (!quiet)
                std::cout << "mdft: boundary scan for " << top << ": " << r.cells << " cells, " << r.irLen
                          << "-bit instruction register\n  " << r.rtlPath << "\n  " << r.bsdlPath << "\n";
            if (!svf.empty() || !tb.empty()) return runBsdl(r.bsdlPath, false);
            return 0;
        } catch (const Error& e) {
            std::cerr << "mdft: " << e.what() << "\n";
            return 2;
        }
    }

    // Runs `cmd`, returns its output.
    static std::string capture(const std::string& cmd, int& rc) {
        std::string out;
        FILE* f = popen((cmd + " 2>&1").c_str(), "r");
        if (!f) throw Error("cannot run " + cmd);
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        rc = pclose(f);
        return out;
    }
    static std::string quote(const std::string& a) {
        std::string q = "'";
        for (char c : a) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
    }

    // After the front end: wrap the scan-inserted top module with logic BIST.
    int runLbist(std::vector<Port> ports, const std::string& top) {
        try {
            std::string zero = std::to_string(lo.misr) + "'h0";
            LbistResult r = insertLbist(ports, top, lo, zero);
            const LbistPlan& p = r.plan;
            if (!quiet) {
                std::cout << "mdft: logic BIST for " << top << ": " << p.n << (p.n == 1 ? " chain" : " chains") << ", longest "
                          << p.len << ", " << p.patterns << " patterns, " << p.cycles() << " clock cycles\n"
                          << "  PRPG " << p.pw << " bits, MISR " << p.mw << " bits, BIST clock " << p.clock << "\n";
                for (const std::string& u : p.sd.unscanned)
                    std::cout << "  warning: " << u << " is not scanned; if it reaches a scan cell the signature is unknown\n";
            }
            if (!lbistDump.empty()) {
                std::ostringstream os;
                auto loads = p.loads(p.patterns);
                for (size_t k = 0; k < loads.size(); k++)
                    for (int c = 0; c < p.n; c++) os << k << " " << p.sd.chains[c].name << " " << loads[k][c] << "\n";
                writeFile(lbistDump, os.str());
            }
            if (lo.sign) {
                std::string msim = lo.msim;
                if (msim.empty()) {
                    size_t slash = self.find_last_of('/');
                    std::string beside = slash == std::string::npos ? "" : self.substr(0, slash + 1) + "msim";
                    msim = !beside.empty() && std::ifstream(beside).good() ? beside : "msim";
                }
                std::string cmd = quote(msim);
                for (size_t i = 1; i < rest.size() && rest[i]; i++) {
                    std::string a = rest[i];
                    if (a == "-Werror" || a == "-quiet" || a == "-q") continue;
                    if (a == "-top" || a == "--top" || a == "-disable") { i++; continue; }
                    cmd += " " + quote(a);
                }
                cmd += " " + quote(r.rtlPath) + " " + quote(r.tbPath) + " -top " + r.wrap + "_tb";
                int rc = 0;
                std::string out = capture(cmd, rc);
                size_t at = out.find("LBIST signature = ");
                if (at == std::string::npos) {
                    std::cerr << "mdft: -lbist_sign: the simulation printed no signature\n  " << cmd << "\n" << out;
                    return 1;
                }
                std::string sig = out.substr(at + 18, out.find('\n', at) - at - 18);
                if (sig.find_first_of("xXzZ") != std::string::npos) {
                    std::cerr << "mdft: the signature is unknown (" << sig << "): an X source reaches the scan chains\n"
                              << "  (an unscanned flop, a memory, an undriven input, or a chain shorter than the JSON says)\n";
                    return 1;
                }
                r = insertLbist(ports, top, lo, std::to_string(lo.misr) + "'h" + sig);
                if (!quiet) std::cout << "  golden signature " << sig << " (simulated with " << msim << ")\n";
            } else if (!quiet) std::cout << "  no golden signature yet: run with -lbist_sign, or simulate " << r.tbPath << "\n";
            if (!quiet) std::cout << "  " << r.rtlPath << "\n  " << r.tbPath << "\n  " << r.iclPath << "\n  " << r.pdlPath << "\n";
            return 0;
        } catch (const Error& e) {
            std::cerr << "mdft: " << e.what() << "\n";
            return 2;
        }
    }

    int runIjtag() {
        icl::Network net;
        for (const std::string& f : icl) net.lib.read(readFile(f), f);
        net.elaborate(iclTop, iclScanOut);
        std::string base = icl::Network::ident(net.top) + "_ijtag";
        // Signals PDL may touch must exist before the RTL is written, so the
        // PDL runs first and the RTL after.
        Retargeter rt(net, ijtagTap);
        for (const std::string& f : pdl) rt.run(readFile(f), f);
        if (!iproc.empty()) {
            std::vector<std::string> w{"iCall", iproc};
            rt.tcl.file = "-iproc", rt.tcl.line = 0;
            rt.iCall(w, 1);
        }
        rt.finish();
        if (!iclRtl.empty()) {
            std::ostringstream os;
            net.writeRtl(os, base);
            if (ijtagTap) {
                os << "\n";
                net.writeTapTop(os, base + "_top", base, 0x1687001);
            }
            writeFile(iclRtl, os.str());
        }
        TbSpec spec;
        spec.dut = ijtagTap ? base + "_top" : base;
        spec.tap = ijtagTap, spec.trst = true;
        spec.tdi = ijtagTap ? "tdi" : "si", spec.tdo = ijtagTap ? "tdo" : "so";
        for (size_t i = 0; i < rt.rtlPins; i++) {
            const icl::FPin& p = net.pins[i];
            std::string n = icl::Network::ident(p.name);
            spec.pins.push_back({"logic [" + std::to_string(p.width - 1) + ":0] " + n, n, "'0"});
        }
        for (const icl::FObs& o : net.obs) {
            std::string n = icl::Network::ident(o.name);
            spec.pins.push_back({"wire [" + std::to_string(o.bits.size() - 1) + ":0] " + n, n, ""});
        }
        writeTests(rt.pat, spec, "IJTAG tests for " + net.top, true);
        if (!quiet) {
            size_t bits = 0;
            for (const icl::FReg& r : net.regs) bits += r.width;
            std::cout << "mdft: IJTAG network " << net.top << ": " << net.regs.size() << " scan registers (" << bits
                      << " bits), " << net.muxes.size() << " scan muxes, " << net.root->kids.size() << " top instances\n";
            std::vector<int> path = net.activePath();
            size_t len = 0;
            for (int r : path) len += net.regs[r].width;
            if (!pdl.empty() || !iproc.empty())
                std::cout << "mdft: " << rt.applies << " iApply, " << rt.scans << " scans, " << rt.scanBits << " scan bits, "
                          << rt.pat.cycles() << " TCK cycles\n";
            else {
                net.resetState();
                path = net.activePath(), len = 0;
                for (int r : path) len += net.regs[r].width;
                std::cout << "mdft: scan path after reset: " << len << " bits\n";
            }
        }
        return 0;
    }
};

}  // namespace dft
