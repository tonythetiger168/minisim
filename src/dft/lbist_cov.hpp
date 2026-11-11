// lbist_cov.hpp - the stuck-at fault coverage of a logic BIST run.
//
//   mdft -lbist_coverage -scandef chains.json [-lbist_patterns N ...] netlist.v [-lib L] [-top M]
//
// The same plan as mdft -lbist (PRPG, phase shifter, seed, pattern count)
// gives the value every scan cell holds before each capture. Those loads are
// graded with the shared fault simulator (faultsim.hpp, atpg.hpp) on the
// gate netlist, under the conditions the BIST wrapper sets up:
//   - the port inputs are 0 and the resets are off;
//   - only the scan cells are observed (they go to the MISR); a fault that
//     shows at a port output alone is not detected.
// The report is matpg's fault classes, the coverage after 64, 128, 256, ...
// patterns (to pick a pattern count), and on request the faults left over,
// which is where test points would go.
#pragma once
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>

#include "../atpg.hpp"
#include "../atpg_model.hpp"
#include "../cpu.hpp"
#include "lbist.hpp"

namespace dft {

struct LbistCovOpts {
    std::vector<std::string> files, libs;
    std::string top, se, json, faultOut;
    double minCoverage = -1;
    int threads = 0;
    bool quiet = false;
};

struct LbistCoverage {
    struct Point {
        int patterns;
        double testCoverage;
    };
    std::vector<Point> curve;
    long n[8] = {0}, total = 0, collapsed = 0;
    int patterns = 0, threads = 1, unknownCaptures = 0;
    double seconds = 0;
    double testCoverage() const {
        long ud = n[atpg::UU] + n[atpg::UR];
        return total - ud ? 100.0 * (double)(n[atpg::DS] + n[atpg::DI]) / (double)(total - ud) : 100.0;
    }
    double faultCoverage() const { return total ? 100.0 * (double)(n[atpg::DS] + n[atpg::DI]) / (double)total : 100.0; }
};

inline int runLbistCoverage(const LbistPlan& p, const LbistCovOpts& o) {
    using namespace atpg;
    auto t0 = std::chrono::steady_clock::now();
    if (o.files.empty()) throw Error("-lbist_coverage needs the scan-inserted gate netlist");
    Design ds;
    Scan scan;
    Model m;
    try {
        ds = readDesign(o.files, o.libs, o.top);
        scan = traceScan(ds, o.se);
        m = build(ds, scan, 1);
    } catch (const ModelError& e) {
        throw Error(e.what());
    }
    // The chains of the scan description, found among the traced ones by
    // their scan-in port.
    std::vector<int> planChain(scan.chains.size(), -1);
    for (size_t i = 0; i < scan.chains.size(); i++) {
        for (int c = 0; c < p.n; c++)
            if (p.sd.chains[c].scanIn == scan.chains[i].siName) planChain[i] = c;
        if (planChain[i] < 0)
            throw Error("the netlist has a scan chain from '" + scan.chains[i].siName + "' that " + o.top + "'s scan description does not list");
        const ScanChain& sc = p.sd.chains[planChain[i]];
        if (sc.length != (int)scan.chains[i].cells.size())
            throw Error("chain " + sc.name + " has " + std::to_string(scan.chains[i].cells.size()) + " cells in the netlist, " +
                        std::to_string(sc.length) + " in the scan description");
    }
    if ((int)scan.chains.size() != p.n)
        throw Error("the scan description lists " + std::to_string(p.n) + " chains, the netlist has " + std::to_string(scan.chains.size()));

    // The MISR sees the scan cells only.
    std::vector<uint8_t> isCap(m.c.size(), 0);
    for (int nd : m.capNodes)
        if (nd >= 0) isCap[nd] = 1;
    for (int nd : m.poNodes)
        if (nd >= 0 && !isCap[nd]) m.c.obs[nd] = 0;
    m.c.finish();

    // Loads to patterns: port inputs 0, then the cells chain by chain. A
    // cell's value is the bit shifted to it, inverted where the chain inverts.
    LbistCoverage r;
    r.patterns = p.patterns;
    size_t nPI = m.piNets.size();
    std::vector<atpg::Pattern> pats((size_t)p.patterns, atpg::Pattern(nPI + m.scanCells.size(), 0));
    {
        auto loads = p.loads(p.patterns);
        for (size_t k = 0; k < pats.size(); k++) {
            size_t at = nPI;
            for (size_t i = 0; i < scan.chains.size(); i++) {
                const std::string& bits = loads[k][planChain[i]];
                bool inv = false;
                for (size_t j = 0; j < scan.chains[i].cells.size(); j++) {
                    inv ^= scan.chains[i].cells[j].inv;
                    pats[k][at++] = (uint8_t)((bits[j] == '1') ^ inv);
                }
            }
        }
    }

    std::vector<Fault> faults = makeFaults(m, false);
    collapse(m.c, faults, false);
    r.threads = o.threads > 0 ? o.threads : std::max(1, cpuTopo().logical());
    Pool pool(r.threads);
    Engine eng(m.c, faults, Options{}, pool);

    // An unknown capture makes the signature unknown: count them on the
    // first block of patterns.
    {
        std::vector<atpg::Pattern> head(pats.begin(), pats.begin() + (long)std::min<size_t>(64, pats.size()));
        std::set<int> bad;
        auto cap = eng.respond(head, m.capNodes);
        for (auto& row : cap)
            for (size_t i = 0; i < row.size(); i++)
                if (row[i] == 2) bad.insert((int)i);
        r.unknownCaptures = (int)bad.size();
    }

    auto tally = [&] {
        std::fill(std::begin(r.n), std::end(r.n), 0);
        r.total = r.collapsed = 0;
        for (size_t i = 0; i < faults.size(); i++) {
            const Fault& f = faults[i];
            uint8_t cls = f.node >= 0 && f.rep != (int)i ? faults[f.rep].cls : f.cls;
            if (f.node < 0 || f.rep == (int)i) r.collapsed++;
            if (cls == NC) cls = ND;
            r.n[cls]++;
            r.total++;
        }
    };
    // Grade in growing steps: the coverage after 64, 128, 256, ... patterns.
    for (size_t done = 0; done < pats.size();) {
        size_t upto = std::min(pats.size(), done ? done * 2 : (size_t)64);
        eng.grade(std::vector<atpg::Pattern>(pats.begin() + (long)done, pats.begin() + (long)upto));
        done = upto;
        tally();
        r.curve.push_back({(int)done, r.testCoverage()});
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    static const char* clsName[] = {"ND", "DS", "DI", "UU", "UR", "AU", "ND"};
    if (!o.faultOut.empty()) {
        std::ofstream f(o.faultOut);
        if (!f) throw Error("cannot write " + o.faultOut);
        for (size_t i = 0; i < faults.size(); i++) {
            const Fault& ft = faults[i];
            uint8_t cls = ft.node >= 0 && ft.rep != (int)i ? faults[ft.rep].cls : ft.cls;
            f << (ft.sa ? "sa1" : "sa0") << "  " << clsName[cls] << "  " << m.sites[ft.site].name << "\n";
        }
    }
    if (!o.quiet) {
        std::cout << "mdft: logic BIST coverage of " << ds.top << ": " << p.patterns << " patterns, " << p.n
                  << (p.n == 1 ? " chain, " : " chains, ") << m.scanCells.size() << " scan cells observed, port inputs at 0\n";
        if (r.unknownCaptures)
            std::cout << "  warning: " << r.unknownCaptures
                      << " scan cells capture an unknown value: the signature is not repeatable until they are bounded\n";
        auto row = [&](const char* name, const char* code, long v) {
            std::cout << "  " << std::left << std::setw(34) << name << std::setw(5) << code << std::right << std::setw(10) << v << "\n";
        };
        row("detected", "DT", r.n[DS] + r.n[DI]);
        row("  by simulation", "DS", r.n[DS]);
        row("  by implication (chain test)", "DI", r.n[DI]);
        row("undetectable (unused)", "UU", r.n[UU]);
        row("untestable in this setup", "AU", r.n[AU]);
        row("not detected", "ND", r.n[ND]);
        row("total faults", "", r.total);
        std::cout << std::fixed << std::setprecision(2) << "  " << std::left << std::setw(39) << "test coverage" << std::right
                  << std::setw(9) << r.testCoverage() << "%\n  " << std::left << std::setw(39) << "fault coverage" << std::right
                  << std::setw(9) << r.faultCoverage() << "%\n  coverage by pattern count:";
        for (const auto& pt : r.curve) std::cout << "  " << pt.patterns << ": " << pt.testCoverage << "%";
        std::cout << "\n  " << r.seconds << " s on " << r.threads << (r.threads == 1 ? " thread\n" : " threads\n");
    }
    if (!o.json.empty()) {
        std::ofstream f(o.json);
        if (!f) throw Error("cannot write " + o.json);
        f << std::fixed << std::setprecision(2) << "{\n  \"design\": \"" << ds.top << "\",\n  \"patterns\": " << p.patterns
          << ",\n  \"total\": " << r.total << ",\n  \"DS\": " << r.n[DS] << ",\n  \"DI\": " << r.n[DI] << ",\n  \"UU\": " << r.n[UU]
          << ",\n  \"AU\": " << r.n[AU] << ",\n  \"ND\": " << r.n[ND] << ",\n  \"unknown_captures\": " << r.unknownCaptures
          << ",\n  \"test_coverage\": " << r.testCoverage() << ",\n  \"fault_coverage\": " << r.faultCoverage() << ",\n  \"curve\": [";
        for (size_t i = 0; i < r.curve.size(); i++)
            f << (i ? ", " : "") << "[" << r.curve[i].patterns << ", " << r.curve[i].testCoverage << "]";
        f << "]\n}\n";
    }
    if (o.minCoverage >= 0 && r.testCoverage() < o.minCoverage) {
        std::cout << "mdft: logic BIST test coverage " << std::fixed << std::setprecision(2) << r.testCoverage() << "% is under "
                  << o.minCoverage << "%\n";
        return 1;
    }
    return 0;
}

}  // namespace dft
