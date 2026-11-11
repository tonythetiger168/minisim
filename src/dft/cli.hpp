// cli.hpp - the boundary-scan and IJTAG options of mdft.
//
//   mdft -bscan [options] design.sv -top M     insert boundary scan, write BSDL
//   mdft -bsdl chip.bsdl [-svf F] [-tb F]      check a BSDL file, write its tests
//   mdft -icl net.icl [-pdl t.pdl] [...]       IJTAG: network RTL and retargeted PDL
//   mdft -diagnose fails.log -patterns P net.v   failure diagnosis on a gate netlist
//
// parse() runs the flows that need no design and returns their exit code; it
// returns -1 when mdft should go on (with the options of this file removed
// from argv). After the front end, a -bscan run finishes in runBscan().
#pragma once
#include <chrono>
#include <thread>

#include "bscan.hpp"
#include "diag.hpp"
#include <memory>
#include "edt.hpp"
#include "edt_resp.hpp"
#include "lbist.hpp"
#include "lbist_cov.hpp"
#include "pdl.hpp"

namespace dft {

struct Cli {
    bool bscan = false, lbist = false, werror = false, quiet = false, ijtagTap = false;
    BscanOpts bo;
    LbistOpts lo;
    EdtOpts eo;
    bool edt = false, edtEncode = false;
    std::string lbistDump, self;
    std::string bsdlFile, svf, tb, iclTop, iclScanOut, iclRtl, iproc;
    std::vector<std::string> icl, pdl;
    std::vector<char*> rest;
    int threads = 0;
    DiagOpts dg;
    bool diag = false;
    LbistCovOpts lc;
    bool lbistCov = false;

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
              "  -lbist_coverage        grade the BIST patterns on the gate netlist (no wrapper is written):\n"
              "                         needs -scandef, the netlist and -lib / -top / -se as given to matpg\n"
              "  -lbist_faults FILE     with -lbist_coverage: every fault and its class\n"
              "  -lbist_json FILE       with -lbist_coverage: the report as JSON\n"
              "  -lbist_min_coverage P  with -lbist_coverage: exit 1 under P percent test coverage\n"
              "scan compression:\n"
              "  -edt                   add a decompressor and a compactor around the chains of -scandef\n"
              "  -edt_encode            encode cubes only: needs -scandef and -edt_cubes, no design\n"
              "  -edt_channels N        tester channels each way (default: one per ten chains)\n"
              "  -edt_ring N            ring generator width, 8..64 (default 32; 64 above 8 channels)\n"
              "  -edt_cubes FILE        test cubes: '<pattern> <chain> <bits>', scan-in cell first, x = free\n"
              "  -edt_patterns FILE     the compressed patterns and what they load\n"
              "  -edt_fill random|0     the free channel bits (default random)\n"
              "  -edt_xmask             a compactor that can observe one chain per channel (port edt_mask_in)\n"
              "  -edt_responses         with -edt on a gate netlist: expected outputs and <top>_edt_pat_tb.sv\n"
              "                         (-edt_encode does this when the netlist is on the line; -lib, -se as matpg)\n"
              "  -edt_netlist FILE      the gate netlist for the responses, when the line also has cell models\n"
              "  -edt_out DIR           where <top>_edt.sv and its testbench go (default .)\n"
              "  -threads N             encode on N threads (default: every core; the result is the same)\n"
              "failure diagnosis (gate netlist, no SystemVerilog front end):\n"
              "  -diagnose FAILLOG      rank the faults that explain a tester fail log; needs the netlist,\n"
              "                         -patterns, and -lib / -top / -se as given to matpg\n"
              "  -patterns FILE         the patterns that were applied (matpg -patterns)\n"
              "  -diag_fault stuck|transition   the fault model and pattern set (default stuck)\n"
              "  -diag_defects N        diagnose up to N defects (default 4)\n"
              "  -diag_suspects N       list up to N suspects per defect (default 10)\n"
              "  -diag_json FILE        the report as JSON\n"
              "  -diag_inject 'sa0 PIN' simulate this fault instead of reading a log (repeatable)\n"
              "  -diag_log FILE         with -diag_inject: write the fail log it produces\n"
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
                else if (a == "-lbist_coverage") lbistCov = mine = true;
                else if (a == "-lbist_faults") lc.faultOut = next();
                else if (a == "-lbist_json") lc.json = next();
                else if (a == "-lbist_min_coverage") lc.minCoverage = std::stod(next());
                else if (a == "-edt") edt = mine = true;
                else if (a == "-edt_encode") edtEncode = mine = true;
                else if (a == "-edt_channels") eo.channels = std::stoi(next());
                else if (a == "-edt_ring") eo.ring = std::stoi(next());
                else if (a == "-edt_cubes") eo.cubes = next();
                else if (a == "-edt_patterns") eo.patterns = next();
                else if (a == "-edt_fill") eo.fill = next();
                else if (a == "-edt_out") eo.outDir = next();
                else if (a == "-edt_xmask") eo.xmask = true;
                else if (a == "-edt_responses") eo.responses = true;
                else if (a == "-edt_netlist") edtNetlist.push_back(next()), eo.responses = true;
                else if (a == "-diagnose") dg.failLog = next(), diag = mine = true;
                else if (a == "-diag_inject") dg.inject.push_back(next()), diag = mine = true;
                else if (a == "-patterns") dg.patterns = next();
                else if (a == "-diag_fault") dg.fault = next();
                else if (a == "-diag_defects") dg.maxDefects = std::stoi(next());
                else if (a == "-diag_suspects") dg.maxSuspects = std::stoi(next());
                else if (a == "-diag_json") dg.json = next();
                else if (a == "-diag_log") dg.writeLog = next();
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
                        threads = std::atoi(argv[++i]);  // used by the cube encoder
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
            if (diag || lbistCov) {
                // The rest of the line is the gate netlist and how to read it.
                const char* who = diag ? "-diagnose" : "-lbist_coverage";
                std::vector<std::string> files, libs;
                std::string top, se;
                for (size_t k = 1; k < rest.size(); k++) {
                    std::string a = rest[k];
                    auto val = [&]() -> std::string {
                        if (k + 1 >= rest.size()) throw Error(a + " needs a value");
                        return rest[++k];
                    };
                    if (a == "-top") top = val();
                    else if (a == "-lib") libs.push_back(val());
                    else if (a == "-se") se = val();
                    else if (a == "-quiet" || a == "-q" || a == "-Werror") continue;
                    else if (a[0] == '-') throw Error(std::string(who) + " does not take " + a);
                    else files.push_back(a);
                }
                if (diag && (lbistCov || lbist || bscan || edt || edtEncode)) throw Error("-diagnose is a separate run");
                if (diag) {
                    dg.files = files, dg.libs = libs, dg.top = top, dg.se = se;
                    dg.threads = threads, dg.quiet = quiet;
                    return runDiagnose(dg);
                }
                if (lbist || bscan || edt || edtEncode) throw Error("-lbist_coverage is a separate run");
                if (lo.scandef.empty()) throw Error("-lbist_coverage needs -scandef FILE");
                LbistPlan p;
                p.sd = readScanDef(lo.scandef);
                p.plan(lo);
                lc.files = files, lc.libs = libs, lc.top = top, lc.se = se;
                lc.threads = threads, lc.quiet = quiet;
                return runLbistCoverage(p, lc);
            }
            if ((int)lbist + (int)bscan + (int)edt + (int)edtEncode > 1) throw Error("-bscan, -lbist, -edt and -edt_encode are separate runs");
            if ((edt || edtEncode) && lo.scandef.empty()) throw Error("-edt needs -scandef FILE");
            eo.scandef = lo.scandef, eo.threads = threads;
            if (edt || edtEncode) {
                // The gate netlist, for the expected responses (left on the line for -edt).
                for (size_t k = 1; k < rest.size(); k++) {
                    std::string a = rest[k];
                    auto val = [&]() -> std::string {
                        if (k + 1 >= rest.size()) throw Error(a + " needs a value");
                        return rest[++k];
                    };
                    if (a == "-top") nlTop = val();
                    else if (a == "-lib") nlLibs.push_back(val());
                    else if (a == "-se") nlSe = val();
                    else if (a == "-o" || a == "-threads") val();
                    else if (a[0] != '-') nlFiles.push_back(a);
                }
                if (!edtNetlist.empty()) nlFiles = edtNetlist;
                if (edtEncode && !nlFiles.empty()) eo.responses = true;
                if (eo.responses && eo.cubes.empty()) throw Error("-edt_responses needs -edt_cubes FILE");
            }
            if (edtEncode) {
                if (eo.cubes.empty()) throw Error("-edt_encode needs -edt_cubes FILE");
                EdtPlan p;
                p.sd = readScanDef(eo.scandef);
                p.plan(eo);
                runEncode(p, nullptr);
                return 0;
            }
            if (lbist && lo.scandef.empty()) throw Error("-lbist needs -scandef FILE");
            if (!bsdlFile.empty()) return runBsdl(bsdlFile, true);
            if (!icl.empty()) return runIjtag();
        } catch (const Error& e) {
            std::cerr << "mdft: " << e.what() << "\n";
            return 2;
        } catch (const DiagError& e) {
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

    // Encodes -edt_cubes; returns the channel data of the first few patterns for the testbench.
    std::vector<std::string> nlFiles, nlLibs, edtNetlist;
    std::string nlTop, nlSe;
    // What -edt_responses leaves for the pattern testbench.
    std::unique_ptr<EdtNetlist> edtNl;
    CubeSet edtCubes;
    std::vector<EdtEncoder::Result> edtRes;
    std::vector<EdtResponse> edtRs;

    void runEncode(const EdtPlan& p, std::vector<std::vector<std::string>>* tbCh) {
        if (!quiet)
            std::cout << "mdft: scan compression: " << p.n << " chains (longest " << p.len << ") on " << p.c
                      << (p.c == 1 ? " channel, " : " channels, ") << p.r << "-bit ring generator, " << p.T
                      << " shift cycles per pattern\n";
        if (eo.cubes.empty()) return;
        CubeSet cs = readCubes(eo.cubes, p);
        if (eo.responses) {
            edtNl = std::make_unique<EdtNetlist>(p, nlFiles, nlLibs, nlTop, nlSe);
            edtNl->toShifted(cs);
        }
        EdtStats st;
        std::vector<EdtEncoder::Result> res = encodeAll(p, cs, eo, st);
        EdtRespStats rst;
        if (eo.responses) {
            edtRs = edtNl->simulate(p, cs, res, st.threads);
            rst = compactResponses(p, res, edtRs, edtNl->thru, st.threads);
        }
        if (!eo.patterns.empty()) writeEdtPatterns(eo.patterns, p, cs, res, eo.responses ? &edtRs : nullptr);
        if (tbCh)
            for (size_t i = 0; i < res.size() && (int)tbCh->size() < eo.tbPatterns; i++)
                if (res[i].ok) tbCh->push_back(res[i].channels);
        if (!quiet) {
            long cells = 0;
            for (const ScanChain& c : p.sd.chains) cells += c.length;
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "mdft: %d of %d cubes encoded (%d unencodable), %.1f care bits on average, %ld at most\n"
                          "  %ld scan cells from %d channel bits per pattern: %.1fx less tester data\n"
                          "  %.2f s on %d thread%s\n",
                          st.encoded, st.total, st.failed, st.total ? (double)st.care / st.total : 0.0, st.maxCare, cells,
                          p.c * p.T, (double)cells / (p.c * p.T), st.seconds, st.threads, st.threads == 1 ? "" : "s");
            std::cout << buf;
            if (eo.responses) {
                long cells2 = rst.known + rst.unknown;
                std::snprintf(buf, sizeof buf,
                              "  responses: %ld of %ld captured cells are unknown; the compactor shows %.1f%% of the known ones",
                              rst.unknown, cells2, rst.known ? 100.0 * (double)rst.seen / (double)rst.known : 100.0);
                std::cout << buf;
                if (p.xmask) {
                    std::snprintf(buf, sizeof buf, " (%.1f%% without masking; %ld channel masks used)",
                                  rst.known ? 100.0 * (double)rst.seenUnmasked / (double)rst.known : 100.0, rst.masked);
                    std::cout << buf;
                }
                std::cout << "\n";
            }
        }
        if (eo.responses) edtCubes = std::move(cs), edtRes = std::move(res);
    }
    int runEdt(std::vector<Port> ports, const std::string& top) {
        try {
            EdtPlan p;
            p.sd = readScanDef(eo.scandef);
            p.plan(eo);
            std::vector<std::vector<std::string>> tbCh;
            runEncode(p, &tbCh);
            if (tbCh.empty()) {  // no cubes: random channel data exercises the hardware
                std::mt19937_64 rng(0xEDEDEDED);
                for (int k = 0; k < 4; k++) {
                    tbCh.emplace_back(p.c, std::string(p.T, '0'));
                    for (std::string& ch : tbCh.back())
                        for (char& b : ch) b = (rng() & 1) ? '1' : '0';
                }
            }
            EdtResult r = insertEdt(ports, top, eo, p, tbCh);
            if (!quiet) std::cout << "  " << r.rtlPath << "\n  " << r.tbPath << "\n";
            if (eo.responses) {
                std::string pt = writeEdtPatternTb(ports, top, eo, p, *edtNl, edtCubes, edtRes, edtRs);
                if (!quiet) std::cout << "  " << pt << "\n";
            }
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
