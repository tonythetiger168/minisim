// edt_resp.hpp - the expected responses of compressed patterns.
//
//   mdft -edt_encode -scandef chains.json -edt_cubes cubes.txt -edt_patterns out.txt
//        [-edt_xmask] netlist.v [-lib L] [-top M] [-se PORT]
//   mdft -edt ... -edt_responses        the same while writing the hardware
//        [-edt_netlist FILE]            the netlist, when the line also has cell models
//
// With the scan-inserted gate netlist, every encoded pattern is simulated
// (the shared fault simulator's good machine, atpg.hpp): the decompressed
// load and the port inputs give what each scan cell captures, 0, 1 or
// unknown. From that come the compactor's output channels for the unload.
// One unknown cell makes its channel unknown for that cycle; with -edt_xmask
// the compactor can observe a single chain per channel instead, and the mask
// that shows the most known cells is chosen per pattern.
//
// The netlist also says where a chain inverts, which a scan description does
// not: cubes and loads are cell states, the encoder works on shifted bits.
#pragma once
#include "../atpg.hpp"
#include "../atpg_model.hpp"
#include "edt.hpp"

namespace dft {

struct EdtNetlist {
    atpg::Design ds;
    atpg::Scan scan;
    atpg::Model m;
    std::vector<int> traced;                 // plan chain -> chain of `scan`
    std::vector<size_t> base;                // plan chain -> its first scan cell among the model inputs
    std::vector<std::vector<uint8_t>> inInv; // [chain][cell]: scan-in to the cell inverts
    std::vector<std::vector<uint8_t>> outInv;// [chain][cell]: the cell to scan-out inverts
    std::vector<uint8_t> thru;               // per chain: scan-in to scan-out inverts

    EdtNetlist(const EdtPlan& p, const std::vector<std::string>& files, const std::vector<std::string>& libs,
               const std::string& top, const std::string& se) {
        using namespace atpg;
        try {
            ds = readDesign(files, libs, top);
            scan = traceScan(ds, se);
            m = build(ds, scan, 1);
        } catch (const Error&) {
            throw;
        } catch (const std::exception& e) {
            throw Error(e.what());
        }
        if ((int)scan.chains.size() != p.n)
            throw Error("the scan description lists " + std::to_string(p.n) + " chains, the netlist has " + std::to_string(scan.chains.size()));
        traced.assign(p.n, -1);
        base.assign(p.n, 0);
        inInv.resize(p.n), outInv.resize(p.n), thru.assign(p.n, 0);
        size_t at = m.piNets.size();
        for (size_t i = 0; i < scan.chains.size(); i++) {
            const Chain& ch = scan.chains[i];
            int c = -1;
            for (int k = 0; k < p.n; k++)
                if (p.sd.chains[k].scanIn == ch.siName) c = k;
            if (c < 0) throw Error("the netlist has a scan chain from '" + ch.siName + "' that the scan description does not list");
            if (p.sd.chains[c].length != (int)ch.cells.size())
                throw Error("chain " + p.sd.chains[c].name + " has " + std::to_string(ch.cells.size()) + " cells in the netlist, " +
                            std::to_string(p.sd.chains[c].length) + " in the scan description");
            traced[c] = (int)i, base[c] = at;
            at += ch.cells.size();
            size_t len = ch.cells.size();
            inInv[c].resize(len), outInv[c].resize(len);
            bool inv = false;
            for (size_t j = 0; j < len; j++) inInv[c][j] = (uint8_t)(inv ^= ch.cells[j].inv);
            inv = ch.soInv;
            for (size_t j = len; j-- > 0;) {
                outInv[c][j] = (uint8_t)inv;
                inv ^= ch.cells[j].inv;
            }
            thru[c] = (uint8_t)inv;
        }
    }

    // Cubes are cell states; the encoder wants the bits to shift in.
    void toShifted(CubeSet& cs) const {
        for (auto& cube : cs.cubes)
            for (size_t c = 0; c < cube.size(); c++)
                for (size_t j = 0; j < cube[c].size(); j++)
                    if (cube[c][j] != 'x' && inInv[c][j]) cube[c][j] ^= 1;  // '0' <-> '1'
    }

    // Simulates every encoded pattern; fills pi, load, capture and so.
    std::vector<EdtResponse> simulate(const EdtPlan& p, const CubeSet& cs, const std::vector<EdtEncoder::Result>& res, int threads) {
        using namespace atpg;
        std::vector<EdtResponse> rs(res.size());
        std::vector<size_t> seq;
        for (size_t i = 0; i < res.size(); i++)
            if (res[i].ok) seq.push_back(i);
        size_t nPI = m.piNets.size();
        std::vector<atpg::Pattern> pats(seq.size(), atpg::Pattern(nPI + m.scanCells.size(), 0));
        par::parallelFor(seq.size(), (unsigned)std::max(1, threads), [&](size_t q) {
            size_t i = seq[q];
            EdtResponse& r = rs[i];
            // Port inputs: as given with the cubes; a free one is 0, a held one its value.
            auto pi = cs.pi.find(cs.ids[i]);
            r.pi.assign(nPI, '0');
            for (size_t k = 0; k < nPI; k++) {
                int fixed = m.c.fixed[m.c.inputs[k]];
                bool v = fixed >= 0 ? fixed == 1 : pi != cs.pi.end() && k < pi->second.size() && pi->second[k] == '1';
                r.pi[k] = v ? '1' : '0';
                pats[q][k] = (uint8_t)v;
            }
            r.load = p.loads(res[i].channels);
            for (int c = 0; c < p.n; c++)
                for (size_t j = 0; j < r.load[c].size(); j++) {
                    if (inInv[c][j]) r.load[c][j] ^= 1;
                    pats[q][base[c] + j] = (uint8_t)(r.load[c][j] == '1');
                }
        });
        std::vector<Fault> none;
        Pool pool(1);
        Engine eng(m.c, none, Options{}, pool);
        std::vector<std::vector<uint8_t>> cap = eng.respond(pats, m.capNodes);
        par::parallelFor(seq.size(), (unsigned)std::max(1, threads), [&](size_t q) {
            EdtResponse& r = rs[seq[q]];
            r.capture.resize(p.n), r.so.resize(p.n);
            for (int c = 0; c < p.n; c++) {
                size_t len = r.load[c].size();
                r.capture[c].assign(len, 'x'), r.so[c].assign(len, 'x');
                for (size_t j = 0; j < len; j++) {
                    uint8_t v = cap[q][base[c] - nPI + j];
                    if (v > 1) continue;
                    r.capture[c][j] = (char)('0' + v);
                    r.so[c][len - 1 - j] = (char)('0' + (v ^ outInv[c][j]));
                }
            }
        });
        return rs;
    }
};

// A testbench that runs the compressed patterns on the wrapper around the
// netlist: load through the decompressor, capture, and compare the output
// channels during the next load.
inline std::string writeEdtPatternTb(const std::vector<Port>& ports, const std::string& top, const EdtOpts& o, const EdtPlan& p,
                                     const EdtNetlist& nl, const CubeSet& cs, const std::vector<EdtEncoder::Result>& res,
                                     const std::vector<EdtResponse>& rs) {
    using detail::portBit;
    std::string wrap = top + "_edt", name = wrap + "_pat_tb";
    auto inName = atpg::portNames(nl.ds, true);
    std::set<int> skip(nl.scan.clocks.begin(), nl.scan.clocks.end());
    skip.insert(nl.scan.seNet);
    for (const atpg::Chain& ch : nl.scan.chains) skip.insert(ch.siNet);
    if (p.sd.clocks.empty()) throw Error("the scan description lists no clock");
    std::string clk = p.sd.clocks[0], se = p.sd.scanEn;
    const char* seOn = p.sd.scanEnHigh ? "1'b1" : "1'b0";
    const char* seOff = p.sd.scanEnHigh ? "1'b0" : "1'b1";
    std::ostringstream tb;
    tb << "// " << name << " - runs the compressed patterns (load, capture, compare the unload), written by mdft\n"
       << "`timescale 1ns/1ps\nmodule " << name << ";\n";
    for (const Port& q : ports) {
        if (q.dir == Port::In) tb << "  logic " << q.range() << q.name << " = '0;\n";
        else tb << "  wire " << q.range() << q.name << ";\n";
    }
    tb << "  logic edt_mode = 1, edt_update = 0, edt_mask_in = 0;\n  logic [" << p.c - 1 << ":0] edt_channels_in = '0;\n  wire ["
       << p.c - 1 << ":0] edt_channels_out;\n  int errors = 0, checks = 0, pattern = -1, flog = 0;\n  " << wrap << " dut (.*);\n"
       << "  task automatic cyc(input logic [" << p.c - 1 << ":0] in, input logic [" << p.c - 1 << ":0] exp, input logic ["
       << p.c - 1 << ":0] care, input int sh = -1, input logic mk = 0);\n"
       << "    edt_channels_in = in; edt_mask_in = mk; #4;\n"
       << "    for (int k = 0; k < " << p.c << "; k++) if (care[k]) begin\n      checks++;\n"
       << "      if (edt_channels_out[k] !== exp[k]) begin\n        errors++;\n"
       << "        if (errors <= 20) $display(\"MISMATCH at %0t: pattern %0d channel %0d = %b, expected %b\", $time, pattern, k, "
          "edt_channels_out[k], exp[k]);\n"
       << "        if (flog) $fdisplay(flog, \"%0d ch %0d %0d\", pattern, k, sh);\n"
       << "      end\n    end\n    #1 " << clk << " = 1; #5 " << clk << " = 0;\n  endtask\n  initial begin\n";
    // Held inputs (resets) at their values from the start.
    auto drive = [&](const std::string& pi) {
        for (size_t k = 0; k < nl.m.piNets.size(); k++)
            if (!skip.count(nl.m.piNets[k])) tb << "    " << inName[nl.m.piNets[k]] << " = 1'b" << pi[k] << ";\n";
    };
    std::vector<size_t> seq;
    for (size_t i = 0; i < res.size() && (int)seq.size() < o.tbPatterns; i++)
        if (res[i].ok) seq.push_back(i);
    // +faillog: the mismatches as a fail log for mdft -diagnose -edt_patterns.
    tb << "    if ($test$plusargs(\"faillog\")) begin\n      flog = $fopen(\"" << wrap << ".faillog\", \"w\");\n"
       << "      $fdisplay(flog, \"# <pattern> ch <channel> <unload cycle>\");\n      $fdisplay(flog, \"top " << top << "\");\n    end\n";
    if (!seq.empty()) drive(rs[seq[0]].pi);
    tb << "    " << se << " = " << seOn << ";\n    edt_update = 1; cyc('0, '0, '0); edt_update = 0;\n";
    const std::vector<std::string> zero(p.c, std::string(p.T, '0'));
    for (size_t q = 0; q <= seq.size(); q++) {
        // Load pattern q (or flush after the last), unloading pattern q-1.
        // After the last pattern here: what the next pattern of the file loads, as the expected values assume.
        const std::vector<std::string>* after = &zero;
        if (q == seq.size() && !seq.empty())
            for (size_t i = seq.back() + 1; i < res.size(); i++)
                if (res[i].ok) {
                    after = &res[i].channels;
                    break;
                }
        const std::vector<std::string>& ch = q < seq.size() ? res[seq[q]].channels : *after;
        const EdtResponse* prev = q ? &rs[seq[q - 1]] : nullptr;
        const EdtResponse* cur = q < seq.size() ? &rs[seq[q]] : nullptr;
        if (cur) tb << "    // pattern " << cs.ids[seq[q]] << "\n";
        for (int t = 0; t < p.T; t++) {
            std::string in(p.c, '0'), exp(p.c, '0'), care(p.c, '0');
            for (int c = 0; c < p.c; c++) {
                in[c] = ch[c][t];
                if (prev && prev->out[c][t] != 'x') care[c] = '1', exp[c] = prev->out[c][t];
            }
            int mq = t - (p.T - p.mw);
            bool mk = p.xmask && cur && mq >= 0 && cur->mask[mq] == '1';
            tb << "    cyc(" << vlog(in) << ", " << vlog(exp) << ", " << vlog(care) << ", " << t << (mk ? ", 1'b1" : "") << ");\n";
        }
        if (!cur) break;
        drive(cur->pi);
        tb << "    pattern = " << cs.ids[seq[q]] << "; " << se << " = " << seOff << "; edt_update = 1; cyc('0, '0, '0); edt_update = 0; "
           << se << " = " << seOn << ";\n";
    }
    if (!seq.empty()) tb << "    if (flog) begin\n      $fdisplay(flog, \"last " << cs.ids[seq.back()] << "\");\n      $fclose(flog);\n    end\n";
    tb << "    if (errors == 0 && checks > 0) $display(\"" << name << ": PASS (%0d checks)\", checks);\n"
       << "    else $display(\"" << name << ": FAIL (%0d mismatches)\", errors);\n    $finish;\n  end\nendmodule\n";
    std::string path = o.outDir + "/" + name + ".sv";
    writeFile(path, tb.str());
    return path;
}

}  // namespace dft
