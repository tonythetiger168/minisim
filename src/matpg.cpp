// matpg.cpp - automatic test pattern generation and fault simulation on a
// gate-level netlist (the output of msynth, after scan insertion).
//
//   matpg scan.v                         stuck-at patterns and a coverage report
//   matpg scan.v -fault transition       launch-on-capture transition patterns
//   matpg scan.v -fault all -stil p.stil -tb p_tb.v -lib cells.lib
//   matpg scan.v -faultsim p.pat         grade existing patterns
//
// The flow is the classic full-scan one: trace the scan chains from the
// ports, cut the scan flops open, build the fault list on the cell pins,
// run random patterns and then PODEM, fault-simulate every pattern against
// every remaining fault, compact, and write the patterns out as STIL (IEEE
// 1450), as a self-checking Verilog testbench that shifts them through the
// chains, and as a plain text file that -faultsim reads back.
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

#include "atpg.hpp"
#include "atpg_model.hpp"
#include "version.hpp"

using namespace atpg;

namespace {

struct Args {
    std::vector<std::string> files;
    std::string top, se, stil, tb, patOut, faultOut, faultsim, json, cubeOut;
    std::vector<std::string> libs;
    std::string fault = "stuck";
    Options opt;
    unsigned threads = par::defaultThreads();
    double minCoverage = -1;
    bool quiet = false, drc = false;
};

void usage() {
    std::cout <<
        "usage: matpg [options] netlist.v ...\n"
        "  test pattern generation and fault simulation on a scan netlist\n\n"
        "  -top M            the top module (default: the one nothing instantiates)\n"
        "  -lib FILE         a Liberty library with the cells' functions (repeatable;\n"
        "                    the generic cells msynth writes are always known)\n"
        "  -fault KIND       stuck (default), transition, or all\n"
        "  -se PORT          the scan-enable port, !PORT if active low (default: found\n"
        "                    from the scan cells)\n"
        "  -abort N          PODEM backtrack limit per fault (default 10)\n"
        "  -random N         most random patterns to try first (default 1024, 0 for none)\n"
        "  -seed N           seed of the random fill (default 1)\n"
        "  -no_compact       keep every pattern (skip reverse-order compaction)\n"
        "  -threads N        worker threads (default: every logical core; the result\n"
        "                    is the same for any N)\n"
        "  -stil FILE        write the patterns as STIL (IEEE 1450)\n"
        "  -tb FILE          write a self-checking Verilog testbench\n"
        "  -patterns FILE    write the patterns as text (what -faultsim reads)\n"
        "  -cubes FILE       write the tests before random fill, x for a free bit, for\n"
        "                    test compression (mdft -edt_encode)\n"
        "  -faults FILE      write the fault list with each fault's class\n"
        "  -json FILE        write the summary as JSON\n"
        "  -faultsim FILE    do not generate: grade the patterns in FILE\n"
        "  -drc              print why each unscanned flop is not scanned\n"
        "  -min_coverage P   exit 1 if the test coverage is under P percent\n"
        "  -quiet            print the summary only\n"
        "  -version          print the version\n";
}

struct Summary {
    std::string kind;
    long total = 0, collapsed = 0;
    long n[8] = {0};
    int patterns = 0;
    Stats st;
    double seconds = 0;
    double testCoverage() const {
        long ud = n[UU] + n[UR];
        return total - ud ? 100.0 * (n[DS] + n[DI]) / (double)(total - ud) : 100.0;
    }
    double faultCoverage() const { return total ? 100.0 * (n[DS] + n[DI]) / (double)total : 100.0; }
    double effectiveness() const {
        return total ? 100.0 * (n[DS] + n[DI] + n[UU] + n[UR] + n[AU]) / (double)total : 100.0;
    }
};

const char* clsName(uint8_t c) {
    static const char* n[] = {"NC", "DS", "DI", "UU", "UR", "AU", "ND"};
    return n[c];
}

void printSummary(const Summary& s, std::ostream& os) {
    auto row = [&](const char* name, const char* code, long v) {
        os << "  " << std::left << std::setw(34) << name << std::setw(5) << code << std::right
           << std::setw(10) << v << "\n";
    };
    os << "\n" << s.kind << " fault summary (uncollapsed)\n";
    os << "  -------------------------------------------------\n";
    row("detected", "DT", s.n[DS] + s.n[DI]);
    row("  by simulation", "DS", s.n[DS]);
    row("  by implication (chain test)", "DI", s.n[DI]);
    row("undetectable", "UD", s.n[UU] + s.n[UR]);
    row("  unused", "UU", s.n[UU]);
    row("  redundant", "UR", s.n[UR]);
    row("ATPG untestable", "AU", s.n[AU]);
    row("not detected (aborted)", "ND", s.n[ND]);
    os << "  -------------------------------------------------\n";
    row("total faults", "", s.total);
    row("collapsed faults", "", s.collapsed);
    os << std::fixed << std::setprecision(2);
    os << "  " << std::left << std::setw(39) << "test coverage" << std::right << std::setw(9)
       << s.testCoverage() << "%\n";
    os << "  " << std::left << std::setw(39) << "fault coverage" << std::right << std::setw(9)
       << s.faultCoverage() << "%\n";
    os << "  " << std::left << std::setw(39) << "ATPG effectiveness" << std::right << std::setw(9)
       << s.effectiveness() << "%\n";
    row("patterns", "", s.patterns);
    os << "  -------------------------------------------------\n";
}

// The tester's view of the design: which input bit is where in a cycle.
struct Tester {
    const Design& nl;
    const Scan& scan;
    const Model& m;
    std::vector<int> inNets, ckNets, outNets;   // testbench vector layouts
    std::map<int, int> inIdx, ckIdx, outIdx;
    std::vector<int> piPos;                     // model input -> in_v index or -1 (a clock)
    std::vector<std::vector<int>> cellPos;      // chain -> scanCells index of each cell
    size_t L = 0;

    Tester(const Design& n, const Scan& s, const Model& mm) : nl(n), scan(s), m(mm) {
        std::map<int, int> isClk;
        for (int c : scan.clocks) isClk[c] = 1;
        for (int net : m.piNets) {
            if (isClk.count(net)) { ckIdx[net] = (int)ckNets.size(); ckNets.push_back(net); piPos.push_back(-1); }
            else { inIdx[net] = (int)inNets.size(); inNets.push_back(net); piPos.push_back(inIdx[net]); }
        }
        for (int net : m.poNets)
            if (!outIdx.count(net)) { outIdx[net] = (int)outNets.size(); outNets.push_back(net); }
        int k = 0;
        for (const Chain& ch : scan.chains) {
            std::vector<int> pos;
            for (size_t i = 0; i < ch.cells.size(); i++) pos.push_back(k++);
            L = std::max(L, pos.size());
            cellPos.push_back(pos);
        }
    }
    // The bit to drive on scan-in so that cell i of the chain ends up at v,
    // and the bit seen on scan-out when cell i holds v.
    static bool inPar(const Chain& ch, size_t i) {
        bool p = false;
        for (size_t k = 0; k <= i; k++) p ^= ch.cells[k].inv;
        return p;
    }
    static bool outPar(const Chain& ch, size_t i) {
        bool p = ch.soInv;
        for (size_t k = i + 1; k < ch.cells.size(); k++) p ^= ch.cells[k].inv;
        return p;
    }
    // How the testbench connects one port bit.
    std::string bit(const Port& p) const {
        if (p.net < 0) return p.input ? "1'b0" : "";
        if (!p.input) return "out_v[" + std::to_string(outIdx.at(p.net)) + "]";
        if (ckIdx.count(p.net)) return "ck_v[" + std::to_string(ckIdx.at(p.net)) + "]";
        if (inIdx.count(p.net)) return "in_v[" + std::to_string(inIdx.at(p.net)) + "]";
        return p.net == nl.c1 ? "1'b1" : "1'b0";
    }
};

std::string lit(const std::string& bits) {   // index 0 is the lsb
    std::string s(bits.rbegin(), bits.rend());
    return std::to_string(bits.size()) + "'b" + s;
}

void writeTb(const Tester& t, const std::vector<PatSet>& sets, std::ostream& os) {
    const Design& nl = t.nl;
    size_t NI = t.inNets.size(), NK = t.ckNets.size(), NO = t.outNets.size();
    std::string top = nl.top;
    os << "// Self-checking scan test of " << top << ", written by matpg " << MINISIM_VERSION << ".\n"
       << "// Each cycle drives the inputs, compares the outputs, then pulses the clocks.\n"
       << "// +faillog writes one line per wrong bit to " << top << ".faillog, or to the file\n"
       << "// MATPG_FAILLOG is defined as (see docs/ATPG.md).\n"
       << "`ifndef MATPG_FAILLOG\n`define MATPG_FAILLOG \"" << top << ".faillog\"\n`endif\n"
       << "`timescale 1ns/1ps\nmodule " << gnl::vname(top + "_atpg_tb") << ";\n";
    if (NI) os << "  reg [" << NI - 1 << ":0] in_v;\n";
    if (NK) os << "  reg [" << NK - 1 << ":0] ck_v;\n";
    if (NO) os << "  wire [" << NO - 1 << ":0] out_v;\n";
    os << "  integer errors, cycles, flog, b;\n  " << gnl::vname(top) << " dut (";
    // A bus is connected once, as the concatenation of its bits (msb first,
    // the order the flattener lists them).
    for (size_t i = 0; i < nl.ports.size();) {
        const Port& p = nl.ports[i];
        size_t j = i + 1;
        if (!p.bus.empty())
            while (j < nl.ports.size() && nl.ports[j].bus == p.bus) j++;
        os << (i ? "," : "") << "\n    ." << gnl::vname(p.bus.empty() ? p.name : p.bus) << "({";
        for (size_t k = i; k < j; k++) os << (k > i ? ", " : "") << t.bit(nl.ports[k]);
        os << "})";
        i = j;
    }
    os << ");\n";
    os << "  task c;\n";
    if (NI) os << "    input [" << NI - 1 << ":0] i;\n";
    os << "    input pulse;\n";
    if (NO)
        os << "    input [" << NO - 1 << ":0] e;\n    input [" << NO - 1 << ":0] m;\n"
           << "    input integer pt;\n    input integer sh;\n    reg [" << NO - 1 << ":0] d;\n";
    os << "    begin\n";
    if (NI) os << "      in_v = i;\n";
    os << "      #40; cycles = cycles + 1;\n";
    if (NO)
        os << "      if (((out_v ^ e) & m) !== " << NO << "'d0) begin\n"
           << "        errors = errors + 1;\n"
           << "        if (errors <= 10) $display(\"MISMATCH cycle %0d: got %b expected %b mask %b\", cycles, out_v, e, m);\n"
           << "        d = (out_v ^ e) & m;\n"
           << "        if (flog != 0 && pt >= 0)\n"
           << "          for (b = 0; b < " << NO << "; b = b + 1)\n"
           << "            if (d[b] !== 1'b0) faillog(pt, sh, b);\n"
           << "      end\n";
    os << "      #10;";
    if (NK) os << " if (pulse) ck_v = {" << NK << "{1'b1}};";
    os << "\n      #30;";
    if (NK) os << " ck_v = " << NK << "'d0;";
    os << "\n      #20;\n    end\n  endtask\n";

    // +faillog: one line per wrong bit, for diagnosis. A bit unloaded
    // from a chain is "<pattern> <chain> <cell>" (cell 0 nearest scan-in), a
    // port compared at capture is "<pattern> po <port>".
    if (NO) {
        auto outName = portNames(nl, false);
        auto str = [](const std::string& n) {
            std::string r;
            for (char ch : n) {
                if (ch == '\\' || ch == '"') r += '\\';
                r += ch;
            }
            return r;
        };
        os << "  task faillog;\n    input integer pt;\n    input integer sh;\n    input integer ob;\n    begin\n"
           << "      if (sh < 0)\n        case (ob)\n";
        for (size_t i = 0; i < NO; i++)
            os << "          " << i << ": $fdisplay(flog, \"%0d po %s\", pt, \"" << str(outName[t.outNets[i]]) << "\");\n";
        os << "          default: ;\n        endcase\n      else\n        case (ob)\n";
        for (size_t ci = 0; ci < t.scan.chains.size(); ci++)
            os << "          " << t.outIdx.at(t.scan.chains[ci].soNet) << ": $fdisplay(flog, \"%0d " << ci
               << " %0d\", pt, " << t.scan.chains[ci].cells.size() - 1 << " - sh);\n";
        os << "          default: ;\n        endcase\n    end\n  endtask\n";
    }

    std::string idle(NI, '0');
    for (size_t i = 0; i < t.m.piNets.size(); i++) {
        auto h = t.scan.hold.find(t.m.piNets[i]);
        if (h != t.scan.hold.end() && t.piPos[i] >= 0) idle[t.piPos[i]] = (char)('0' + h->second);
    }
    long cycles = 0;
    std::ostringstream body;
    // pt and sh say what the cycle compares: pattern pt's response, at
    // capture (sh < 0) or in shift cycle sh of its unload.
    auto cyc = [&](const std::string& in, bool pulse, const std::string& e, const std::string& mk, int pt = -1,
                   int sh = -1) {
        body << "    c(";
        if (NI) body << lit(in) << ", ";
        body << (pulse && NK ? "1'b1" : "1'b0");
        if (NO) body << ", " << lit(e) << ", " << lit(mk) << ", " << pt << ", " << sh;
        body << ");\n";
        cycles++;
    };
    auto base = [&](const Pattern& p) {
        std::string in = idle;
        for (size_t i = 0; i < t.m.piNets.size(); i++)
            if (t.piPos[i] >= 0) in[t.piPos[i]] = (char)('0' + p[i]);
        return in;
    };
    size_t nPI = t.m.piNets.size();
    std::vector<uint8_t> prev;   // what the chains hold to unload, 2 = do not compare
    // L shift cycles: load `load` (per scan cell, empty = zeros), unload prev.
    int prevId = -1;             // the pattern prev is the response of (-1: the chain test)
    auto shift = [&](std::string in, const std::vector<uint8_t>& load) {
        if (t.scan.chains.empty()) return;
        in[t.inIdx.at(t.scan.seNet)] = (char)('0' + t.scan.seActive);
        for (size_t cy = 0; cy < t.L; cy++) {
            std::string e(NO, '0'), mk(NO, '0');
            for (size_t ci = 0; ci < t.scan.chains.size(); ci++) {
                const Chain& ch = t.scan.chains[ci];
                size_t k = ch.cells.size(), pad = t.L - k;
                char bit = '0';
                if (cy >= pad && !load.empty()) {
                    size_t cell = k - 1 - (cy - pad);
                    bit = (char)('0' + (load[t.cellPos[ci][cell]] ^ (uint8_t)Tester::inPar(ch, cell)));
                }
                in[t.inIdx.at(ch.siNet)] = bit;
                if (cy < k && !prev.empty()) {
                    size_t cell = k - 1 - cy;
                    uint8_t v = prev[t.cellPos[ci][cell]];
                    if (v != 2) {
                        e[t.outIdx.at(ch.soNet)] = (char)('0' + (v ^ (uint8_t)Tester::outPar(ch, cell)));
                        mk[t.outIdx.at(ch.soNet)] = '1';
                    }
                }
            }
            cyc(in, true, e, mk, prevId, (int)cy);
        }
    };
    int total = 0;
    std::string lastBase = idle;
    if (!t.scan.chains.empty()) {
        // The chain test: 0011 through every chain, no capture.
        std::vector<uint8_t> st(t.m.scanCells.size());
        for (size_t i = 0; i < st.size(); i++) st[i] = (i >> 1) & 1;
        body << "    // chain test\n";
        shift(idle, st);
        prev = st;
    }
    for (const PatSet& ps : sets)
        for (size_t p = 0; p < ps.pats.size(); p++) {
            const Pattern& pat = ps.pats[p];
            body << "    // " << (ps.transition ? "transition" : "stuck-at") << " pattern " << p << "\n";
            std::string in = base(pat);
            std::vector<uint8_t> load(pat.begin() + (long)nPI, pat.end());
            shift(in, load);
            std::string e(NO, '0'), mk(NO, '0');
            for (size_t k = 0; k < t.m.poNets.size(); k++) {
                uint8_t v = ps.po[p][k];
                int idx = t.outIdx.at(t.m.poNets[k]);
                if (v != 2) { e[idx] = (char)('0' + v); mk[idx] = '1'; }
            }
            if (ps.transition) cyc(in, true, std::string(NO, '0'), std::string(NO, '0'));
            cyc(in, true, e, mk, total, -1);
            prev = ps.cap[p];
            prevId = total;
            lastBase = in;
            total++;
        }
    if (!prev.empty()) {
        body << "    // unload the last response\n";
        shift(lastBase, {});
    }
    os << "  initial begin\n    errors = 0; cycles = 0; flog = 0;\n"
       << "    if ($test$plusargs(\"faillog\")) begin\n      flog = $fopen(`MATPG_FAILLOG, \"w\");\n"
       << "      $fdisplay(flog, \"top " << top << "\");\n    end\n";
    if (NK) os << "    ck_v = " << NK << "'d0;\n";
    if (NI) os << "    in_v = " << lit(idle) << ";\n";
    os << body.str();
    os << "    if (errors == 0) $display(\"ATPG PASS: " << total << " patterns, %0d cycles\", cycles);\n"
       << "    else $display(\"ATPG FAIL: %0d mismatching cycles of %0d\", errors, cycles);\n"
       << "    if (flog != 0) $fclose(flog);\n    $finish;\n  end\nendmodule\n";
}

void writeStil(const Tester& t, const std::vector<PatSet>& sets, std::ostream& os) {
    const Design& nl = t.nl;
    auto inName = portNames(nl, true), outName = portNames(nl, false);
    auto q = [](const std::string& s) { return "\"" + s + "\""; };
    std::time_t now = std::time(nullptr);
    char date[64];
    std::strftime(date, sizeof date, "%a %b %d %H:%M:%S %Y", std::gmtime(&now));
    os << "STIL 1.0 { Design 2005; }\nHeader {\n  Title \"matpg " << MINISIM_VERSION << " patterns for "
       << nl.top << "\";\n  Date \"" << date << "\";\n  Source \"matpg\";\n}\n";
    os << "Signals {\n";
    std::map<int, int> si, so;
    for (const Chain& ch : t.scan.chains) { si[ch.siNet] = 1; so[ch.soNet] = 1; }
    for (int n : t.m.piNets) os << "  " << q(inName[n]) << " In" << (si.count(n) ? " { ScanIn; }" : ";") << "\n";
    for (int n : t.outNets) os << "  " << q(outName[n]) << " Out" << (so.count(n) ? " { ScanOut; }" : ";") << "\n";
    os << "}\nSignalGroups {\n";
    auto group = [&](const char* name, const std::vector<std::string>& sig) {
        os << "  " << q(name) << " = '";
        for (size_t i = 0; i < sig.size(); i++) os << (i ? " + " : "") << q(sig[i]);
        os << "';\n";
    };
    std::vector<std::string> pis, pos, clks;
    for (int n : t.m.piNets) pis.push_back(inName[n]);
    for (int n : t.outNets) pos.push_back(outName[n]);
    for (int n : t.ckNets) clks.push_back(inName[n]);
    group("_pi", pis);
    if (!pos.empty()) group("_po", pos);
    if (!clks.empty()) group("_clk", clks);
    os << "}\n";
    if (!t.scan.chains.empty()) {
        os << "ScanStructures {\n";
        for (size_t ci = 0; ci < t.scan.chains.size(); ci++) {
            const Chain& ch = t.scan.chains[ci];
            os << "  ScanChain " << q("chain" + std::to_string(ci + 1)) << " {\n    ScanLength "
               << ch.cells.size() << ";\n    ScanIn " << q(ch.siName) << ";\n    ScanOut " << q(ch.soName)
               << ";\n    ScanInversion " << (Tester::inPar(ch, ch.cells.size() - 1) ^ ch.soInv ? 1 : 0)
               << ";\n    ScanEnable " << (t.scan.seActive ? "" : "!") << q(t.scan.seName)
               << ";\n    ScanMasterClock " << q(inName[ch.clock]) << ";\n    ScanCells";
            for (const ScanCell& sc : ch.cells) os << "\n      " << (sc.inv ? "! " : "") << q(nl.cells[sc.cell].name);
            os << ";\n  }\n";
        }
        os << "}\n";
    }
    os << "Timing {\n  WaveformTable \"_default_WFT_\" {\n    Period '100ns';\n    Waveforms {\n"
       << "      \"_pi\" { 01 { '0ns' D/U; } }\n";
    if (!clks.empty()) os << "      \"_clk\" { 0P { '0ns' D; '50ns' D/U; '80ns' D; } }\n";
    if (!pos.empty()) os << "      \"_po\" { LHX { '0ns' X; '40ns' L/H/X; } }\n";
    os << "    }\n  }\n}\n";
    os << "PatternBurst \"_burst_\" { PatList { \"_pattern_\" { } } }\n"
       << "PatternExec { PatternBurst \"_burst_\"; }\n";
    // The held values, for the procedures.
    std::string idle(t.m.piNets.size(), '0');
    for (size_t i = 0; i < idle.size(); i++) {
        auto h = t.scan.hold.find(t.m.piNets[i]);
        if (h != t.scan.hold.end()) idle[i] = (char)('0' + h->second);
    }
    os << "Procedures {\n";
    if (!t.scan.chains.empty()) {
        os << "  \"load_unload\" {\n    W \"_default_WFT_\";\n    C { \"_pi\"=" << idle << "; ";
        if (!pos.empty()) os << "\"_po\"=" << std::string(pos.size(), 'X') << "; ";
        os << q(t.scan.seName) << "=" << t.scan.seActive << "; }\n    Shift { V { ";
        for (const Chain& ch : t.scan.chains) os << q(ch.siName) << "=#; " << q(ch.soName) << "=#; ";
        os << "\"_clk\"=" << std::string(clks.size(), 'P') << "; } }\n  }\n";
    }
    os << "  \"capture\" {\n    W \"_default_WFT_\";\n    V { \"_pi\"=#; ";
    if (!pos.empty()) os << "\"_po\"=#; ";
    os << "}\n  }\n}\n";
    os << "Pattern \"_pattern_\" {\n  W \"_default_WFT_\";\n";
    std::vector<uint8_t> prev;
    size_t nPI = t.m.piNets.size();
    auto load = [&](const std::vector<uint8_t>& st) {
        if (t.scan.chains.empty()) return;
        os << "  Call \"load_unload\" {";
        for (size_t ci = 0; ci < t.scan.chains.size(); ci++) {
            const Chain& ch = t.scan.chains[ci];
            size_t k = ch.cells.size();
            if (!st.empty()) {
                os << "\n    " << q(ch.siName) << "=";
                for (size_t cy = 0; cy < k; cy++) {
                    size_t cell = k - 1 - cy;
                    os << (char)('0' + (st[t.cellPos[ci][cell]] ^ (uint8_t)Tester::inPar(ch, cell)));
                }
                os << ";";
            }
            if (!prev.empty()) {
                os << "\n    " << q(ch.soName) << "=";
                for (size_t cy = 0; cy < k; cy++) {
                    size_t cell = k - 1 - cy;
                    uint8_t v = prev[t.cellPos[ci][cell]];
                    os << (v == 2 ? 'X' : (v ^ (uint8_t)Tester::outPar(ch, cell)) ? 'H' : 'L');
                }
                os << ";";
            }
        }
        os << " }\n";
    };
    auto capture = [&](const Pattern& p, const std::vector<uint8_t>* po) {
        os << "  Call \"capture\" { \"_pi\"=";
        for (size_t i = 0; i < nPI; i++) os << (t.piPos[i] < 0 ? 'P' : (char)('0' + p[i]));
        os << "; ";
        if (!pos.empty()) {
            std::string e(pos.size(), 'X');
            if (po)
                for (size_t k = 0; k < t.m.poNets.size(); k++)
                    if ((*po)[k] != 2) e[t.outIdx.at(t.m.poNets[k])] = (*po)[k] ? 'H' : 'L';
            os << "\"_po\"=" << e << "; ";
        }
        os << "}\n";
    };
    if (!t.scan.chains.empty()) {
        std::vector<uint8_t> st(t.m.scanCells.size());
        for (size_t i = 0; i < st.size(); i++) st[i] = (i >> 1) & 1;
        os << "  \"chain_test\":\n";
        load(st);
        prev = st;
    }
    int id = 0;
    for (const PatSet& ps : sets)
        for (size_t p = 0; p < ps.pats.size(); p++) {
            os << "  \"pattern " << id++ << "\": Ann {* " << (ps.transition ? "transition" : "stuck-at") << " *}\n";
            load(std::vector<uint8_t>(ps.pats[p].begin() + (long)nPI, ps.pats[p].end()));
            if (ps.transition) capture(ps.pats[p], nullptr);
            capture(ps.pats[p], &ps.po[p]);
            prev = ps.cap[p];
        }
    if (!prev.empty()) {
        os << "  \"end\":\n";
        load({});
    }
    os << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "matpg: " << s << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (s == "-h" || s == "--help" || s == "-help") { usage(); return 0; }
        if (s == "-version" || s == "--version") {
            std::cout << "matpg " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (s == "-top") a.top = next("a module name");
        else if (s == "-fault") a.fault = next("stuck, transition or all");
        else if (s == "-se") a.se = next("a port name");
        else if (s == "-abort") a.opt.abortLimit = std::stol(next("a number"));
        else if (s == "-random") a.opt.randomMax = std::stoi(next("a number"));
        else if (s == "-seed") a.opt.seed = std::stoull(next("a number"));
        else if (s == "-no_compact") a.opt.compact = false;
        else if (par::threadsArg(argc, argv, i, a.threads, "matpg")) continue;
        else if (s == "-stil") a.stil = next("a file");
        else if (s == "-tb") a.tb = next("a file");
        else if (s == "-lib") a.libs.push_back(next("a file"));
        else if (s == "-patterns") a.patOut = next("a file");
        else if (s == "-cubes") a.cubeOut = next("a file");
        else if (s == "-faults") a.faultOut = next("a file");
        else if (s == "-json") a.json = next("a file");
        else if (s == "-faultsim") a.faultsim = next("a file");
        else if (s == "-min_coverage") a.minCoverage = std::stod(next("a percentage"));
        else if (s == "-drc") a.drc = true;
        else if (s == "-quiet" || s == "-q") a.quiet = true;
        else if (s == "-f") {
            std::ifstream f(next("a file"));
            std::string w;
            while (f >> w) a.files.push_back(w);
        } else if (s[0] == '-') {
            std::cerr << "matpg: unknown option " << s << " (see -h)\n";
            return 2;
        } else a.files.push_back(s);
    }
    if (a.fault != "stuck" && a.fault != "transition" && a.fault != "all") {
        std::cerr << "matpg: -fault takes stuck, transition or all\n";
        return 2;
    }
    try {
        if (a.files.empty()) { usage(); return 2; }
        auto t0 = std::chrono::steady_clock::now();
        auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        Design nl = readDesign(a.files, a.libs, a.top);
        Scan scan = traceScan(nl, a.se);
        unsigned threads = a.threads;
        Pool pool(threads);
        if (!a.quiet) {
            int comb = 0;
            for (const Cell& c : nl.cells) comb += !c.def->flop;
            for (auto& w : nl.warnings) std::cout << "  warning: " << w << "\n";
            std::cout << "matpg: " << nl.top << ": " << comb << " gates, " << scan.flops << " flops, "
                      << scan.scanned << " on " << scan.chains.size()
                      << (scan.chains.size() == 1 ? " scan chain" : " scan chains") << ", " << threads
                      << (threads == 1 ? " thread\n" : " threads\n");
            auto inName = portNames(nl, true);
            for (size_t i = 0; i < scan.chains.size(); i++) {
                const Chain& ch = scan.chains[i];
                std::cout << "  chain " << i + 1 << ": " << ch.siName << " -> " << ch.soName << ", "
                          << ch.cells.size() << " cells, clock " << inName[ch.clock] << "\n";
            }
            if (scan.flops > scan.scanned) {
                std::cout << "  " << scan.flops - scan.scanned
                          << " flops are not scanned; the tester sees them as unknown"
                          << (a.drc ? ":" : " (-drc lists them)") << "\n";
                if (a.drc)
                    for (auto& n : scan.notes) std::cout << "    " << n << "\n";
            }
        }

        std::vector<PatSet> sets;
        std::vector<Summary> sums;
        std::ofstream faultFile;
        if (!a.faultOut.empty()) {
            faultFile.open(a.faultOut);
            if (!faultFile) throw ModelError("cannot write " + a.faultOut);
        }
        Model first = build(nl, scan, 1);
        if (!a.quiet && (first.loops || first.floating || first.blackboxes))
            std::cout << "  " << first.loops << " combinational loops cut, " << first.floating
                      << " undriven nets, " << first.blackboxes << " cells without a function: read as unknown\n";
        std::vector<PatSet> given;
        if (!a.faultsim.empty()) given = readPatterns(a.faultsim, nl, first);

        for (int tr = 0; tr < 2; tr++) {
            if (tr == 0 && a.fault == "transition") continue;
            if (tr == 1 && a.fault == "stuck") continue;
            double start = secs();
            Model two;
            if (tr) two = build(nl, scan, 2);
            const Model& m = tr ? two : first;
            std::vector<Fault> faults = makeFaults(m, tr);
            collapse(m.c, faults, tr);
            Options opt = a.opt;
            opt.proveRedundant = !tr;
            Engine eng(m.c, faults, opt, pool);
            PatSet ps;
            ps.transition = tr;
            if (!a.faultsim.empty()) {
                ps.pats = given[tr].pats;
                eng.grade(ps.pats);
            } else {
                ps.pats = eng.generate();
                ps.cubes = eng.cubes;
            }
            ps.po = eng.respond(ps.pats, m.poNodes);
            ps.cap = eng.respond(ps.pats, m.capNodes);

            Summary s;
            s.kind = tr ? "transition" : "stuck-at";
            s.st = eng.st;
            s.patterns = (int)ps.pats.size();
            for (size_t i = 0; i < faults.size(); i++) {
                Fault& f = faults[i];
                if (f.node >= 0) {
                    if (f.rep == (int)i) s.collapsed++;
                    else f.cls = faults[f.rep].cls;
                } else {
                    s.collapsed++;
                }
                // Graded but never detected: not detected.
                if (f.cls == NC) f.cls = ND;
                s.n[f.cls]++;
                s.total++;
            }
            if (faultFile.is_open())
                for (const Fault& f : faults)
                    faultFile << (tr ? (f.sa ? "stf" : "str") : (f.sa ? "sa1" : "sa0")) << "  "
                              << clsName(f.cls) << "  " << m.sites[f.site].name << "\n";
            s.seconds = secs() - start;
            printSummary(s, std::cout);
            if (!a.quiet) {
                std::cout << "  " << s.st.randomPatterns << " random + " << s.st.podemPatterns
                          << " PODEM patterns, " << s.st.beforeCompaction << " before compaction; "
                          << s.st.podemCalls << " PODEM runs, " << s.st.aborted << " aborted; "
                          << std::fixed << std::setprecision(2) << s.seconds << " s\n";
            }
            if (s.st.mismatches)
                std::cout << "  warning: " << s.st.mismatches
                          << " generated tests did not simulate as detecting; they were dropped\n";
            sums.push_back(s);
            sets.push_back(std::move(ps));
        }

        Tester tester(nl, scan, first);
        if (!a.tb.empty()) {
            std::ofstream f(a.tb);
            if (!f) throw ModelError("cannot write " + a.tb);
            writeTb(tester, sets, f);
        }
        if (!a.stil.empty()) {
            std::ofstream f(a.stil);
            if (!f) throw ModelError("cannot write " + a.stil);
            writeStil(tester, sets, f);
        }
        if (!a.patOut.empty()) {
            std::ofstream f(a.patOut);
            if (!f) throw ModelError("cannot write " + a.patOut);
            writePatterns(nl, first, sets, f);
        }
        if (!a.cubeOut.empty()) {
            // "<pattern> <chain> <bits>": the load of one chain, the cell
            // nearest scan-in first; "<pattern> pi <bits>": the port inputs
            // in the order of the pattern file. Patterns count from 0 in
            // pattern file order, chains from 0 in the order printed.
            std::ofstream f(a.cubeOut);
            if (!f) throw ModelError("cannot write " + a.cubeOut);
            f << "# matpg test cubes for " << nl.top << ": x is a bit left free\n";
            // Which chain a number means, for a reader with its own chain list.
            f << "chains";
            for (const Chain& ch : scan.chains) f << " " << ch.siName;
            f << "\n";
            size_t nPI = first.piNets.size();
            int id = 0;
            const char* ch = "01x";
            for (const PatSet& ps : sets)
                for (const Pattern& cu : ps.cubes.empty() ? ps.pats : ps.cubes) {
                    f << id << " pi ";
                    for (size_t i = 0; i < nPI; i++)
                        f << (first.c.fixed[first.c.inputs[i]] >= 0 ? ch[first.c.fixed[first.c.inputs[i]]] : ch[cu[i]]);
                    f << "\n";
                    size_t k = nPI;
                    for (size_t ci = 0; ci < scan.chains.size(); ci++) {
                        f << id << " " << ci << " ";
                        for (size_t i = 0; i < scan.chains[ci].cells.size(); i++) f << ch[cu[k++]];
                        f << "\n";
                    }
                    id++;
                }
        }
        if (!a.json.empty()) {
            std::ofstream f(a.json);
            if (!f) throw ModelError("cannot write " + a.json);
            f << "{\n  \"design\": \"" << nl.top << "\",\n  \"flops\": " << scan.flops
              << ",\n  \"scan_flops\": " << scan.scanned << ",\n  \"chains\": " << scan.chains.size()
              << ",\n  \"runs\": [";
            for (size_t i = 0; i < sums.size(); i++) {
                const Summary& s = sums[i];
                f << (i ? "," : "") << "\n    {\"fault\": \"" << s.kind << "\", \"total\": " << s.total
                  << ", \"collapsed\": " << s.collapsed << ", \"DS\": " << s.n[DS] << ", \"DI\": " << s.n[DI]
                  << ", \"UU\": " << s.n[UU] << ", \"UR\": " << s.n[UR] << ", \"AU\": " << s.n[AU]
                  << ", \"ND\": " << s.n[ND] << ", \"patterns\": " << s.patterns << std::fixed
                  << std::setprecision(2) << ", \"test_coverage\": " << s.testCoverage()
                  << ", \"fault_coverage\": " << s.faultCoverage() << ", \"seconds\": " << s.seconds << "}";
            }
            f << "\n  ]\n}\n";
        }
        if (a.minCoverage >= 0)
            for (const Summary& s : sums)
                if (s.testCoverage() < a.minCoverage) {
                    std::cout << "matpg: " << s.kind << " test coverage " << std::fixed << std::setprecision(2)
                              << s.testCoverage() << "% is under " << a.minCoverage << "%\n";
                    return 1;
                }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "matpg: " << e.what() << "\n";
        return 2;
    }
}
