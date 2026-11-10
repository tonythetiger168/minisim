// diag.hpp - scan failure diagnosis: from the tester's fail log back to the
// faults that explain it.
//
//   mdft -diagnose fails.log -patterns pats.txt [-lib L] [-top M] netlist.v
//
// The patterns are matpg's (-patterns FILE), the netlist is the scan-inserted
// gate netlist they were made for. The fail log is one failing observation
// per line:
//
//   # comment
//   top <module>              optional, checked against the netlist
//   last <pattern>            optional: the tester stopped after this pattern
//   <pattern> <chain> <cell>  a scan cell that unloaded the wrong value; the
//                             chain counts from 0 in matpg's order, the cell
//                             from 0 at the scan-in end
//   <pattern> po <port>       a primary output that was wrong after capture
//
// Patterns count from 0 in pattern file order (the stuck-at set, then the
// transition set).
//
// Method (effect-cause, one fault at a time): only a fault in the fan-in of
// a failing observe point can explain a failure, so those are the
// candidates. Each is simulated against every pattern with the shared fault
// simulator (faultsim.hpp) and compared with the log bit by bit:
//
//   tfsf  the tester fails and so does the fault        (explained)
//   tfsp  the tester fails, the fault passes            (not explained)
//   tpsf  the tester passes, the fault fails            (mispredicted)
//
// score = tfsf / (tfsf + tfsp + tpsf). The best candidate is reported with
// everything that scores the same; when it leaves failures unexplained, they
// are diagnosed again as a further defect.
//
// To stay fast on a long pattern set, all candidates are simulated only on
// the first 64-pattern block that holds failures; the 256 that score
// best there are then simulated on every pattern.
//
// Threads: candidates are split over the pool; every candidate has its own
// counters, so the report does not depend on the thread count.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../atpg.hpp"
#include "../atpg_model.hpp"
#include "../cpu.hpp"

namespace dft {

struct DiagOpts {
    std::string failLog, patterns, top, se, json, writeLog;
    std::vector<std::string> files, libs, inject;
    std::string fault = "stuck";   // stuck or transition
    int threads = 0, maxDefects = 4, maxSuspects = 10;
    int screenBlocks = 1, screenKeep = 256;    // see Diagnoser::run
    bool quiet = false;
};

struct DiagError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Diagnoser {
    using Pattern = atpg::Pattern;
    const DiagOpts& o;
    atpg::Design ds;
    atpg::Scan scan;
    atpg::Model m;
    std::vector<atpg::Fault> faults;
    std::vector<Pattern> pats;       // the patterns of the chosen fault model
    int firstId = 0;                 // the file's pattern number of pats[0]
    int otherPatterns = 0;           // patterns of the other model in the file
    // An observation is (pattern, observe node).
    std::vector<int> obsNodes;                       // distinct observe nodes
    std::vector<std::vector<std::string>> obsNames;  // what the tester calls each
    std::vector<int> obsOf;                          // node -> index in obsNodes, or -1
    std::vector<std::vector<int>> chainObs;          // chain, cell -> obs index
    std::map<std::string, int> poObs;

    struct Suspect {
        int fault;
        long tfsf = 0, tpsf = 0, tfsp = 0;
        double score() const {
            long d = tfsf + tfsp + tpsf;
            return d ? 100.0 * (double)tfsf / (double)d : 0.0;
        }
    };
    struct Defect {
        long fails = 0;                // failing bits left when this defect was diagnosed
        std::vector<Suspect> suspects; // best first
    };
    std::vector<Defect> defects;
    long totalFails = 0, failingPatterns = 0, unexplained = 0, ignoredOther = 0, ignoredX = 0;
    int candidates = 0, threads = 1;
    double seconds = 0;

    explicit Diagnoser(const DiagOpts& opts) : o(opts) {
        bool tr = o.fault == "transition";
        if (!tr && o.fault != "stuck") throw DiagError("-diag_fault wants stuck or transition");
        try {
            ds = atpg::readDesign(o.files, o.libs, o.top);
            scan = atpg::traceScan(ds, o.se);
            m = atpg::build(ds, scan, tr ? 2 : 1);
        } catch (const atpg::ModelError& e) {
            throw DiagError(e.what());
        }
        faults = atpg::makeFaults(m, tr);
        atpg::collapse(m.c, faults, tr);
        obsOf.assign(m.c.size(), -1);
        auto obs = [&](int node, const std::string& name) {
            if (node < 0 || !m.c.obs[node]) return -1;
            if (obsOf[node] < 0) {
                obsOf[node] = (int)obsNodes.size();
                obsNodes.push_back(node);
                obsNames.emplace_back();
            }
            obsNames[obsOf[node]].push_back(name);
            return obsOf[node];
        };
        size_t k = 0;
        for (size_t ci = 0; ci < scan.chains.size(); ci++) {
            chainObs.emplace_back();
            for (size_t i = 0; i < scan.chains[ci].cells.size(); i++, k++)
                chainObs.back().push_back(obs(m.capNodes[k], std::to_string(ci) + " " + std::to_string(i)));
        }
        auto outName = atpg::portNames(ds, false);
        for (size_t i = 0; i < m.poNets.size(); i++) poObs[outName[m.poNets[i]]] = obs(m.poNodes[i], "po " + outName[m.poNets[i]]);
        readPatterns(tr);
    }

    // matpg's pattern file ("p stuck|transition <port bits> <scan bits>").
    void readPatterns(bool tr) {
        std::ifstream f(o.patterns);
        if (!f) throw DiagError("cannot open " + o.patterns);
        size_t nPI = m.piNets.size(), nSC = m.scanCells.size();
        std::string line;
        int ln = 0, nStuck = 0, nTrans = 0;
        while (std::getline(f, line)) {
            ln++;
            std::istringstream is(line);
            std::string w, kind, a, b;
            if (!(is >> w)) continue;
            auto bad = [&](const std::string& why) {
                throw DiagError(o.patterns + ":" + std::to_string(ln) + ": " + why);
            };
            if (w == "design") {
                is >> a;
                if (a != ds.top) bad("the patterns are for '" + a + "', the netlist is '" + ds.top + "'");
            } else if (w == "inputs" || w == "scan") {
                size_t n = 0;
                is >> n;
                if (n != (w == "inputs" ? nPI : nSC)) bad("the " + w + " count does not match this netlist");
            } else if (w == "p") {
                is >> kind >> a >> b;
                if (b == "-") b.clear();
                if (a.size() != nPI || b.size() != nSC) bad("a pattern of the wrong length");
                bool t = kind == "transition";
                (t ? nTrans : nStuck)++;
                if (t != tr) continue;
                Pattern p;
                p.reserve(nPI + nSC);
                for (char ch : a + b) {
                    if (ch != '0' && ch != '1') bad("a pattern bit that is not 0 or 1");
                    p.push_back((uint8_t)(ch - '0'));
                }
                pats.push_back(std::move(p));
            }
        }
        // The file numbers the stuck-at set first.
        firstId = tr ? nStuck : 0;
        otherPatterns = tr ? nStuck : nTrans;
        if (pats.empty()) throw DiagError(o.patterns + ": no " + o.fault + " patterns");
    }

    // fails[p] = the observe points (indices into obsNodes) that failed in pattern p.
    std::vector<std::vector<int>> fails;
    int applied = 0;   // patterns the tester ran (of pats)

    void readFailLog(const std::string& path) {
        std::ifstream f(path);
        if (!f) throw DiagError("cannot open " + path);
        fails.assign(pats.size(), {});
        applied = (int)pats.size();
        std::string line;
        int ln = 0;
        while (std::getline(f, line)) {
            ln++;
            size_t h = line.find('#');
            if (h != std::string::npos) line.erase(h);
            std::istringstream is(line);
            std::string a, b, c;
            if (!(is >> a)) continue;
            auto bad = [&](const std::string& why) {
                throw DiagError(path + ":" + std::to_string(ln) + ": " + why);
            };
            is >> b;
            if (a == "top") {
                if (b != ds.top) bad("the log is for '" + b + "', the netlist is '" + ds.top + "'");
                continue;
            }
            if (a == "patterns") continue;
            long p = 0;
            try {
                size_t used = 0;
                p = std::stol(a == "last" ? b : a, &used);
                if (used != (a == "last" ? b : a).size()) throw 0;
            } catch (...) {
                bad("expected a pattern number");
            }
            p -= firstId;
            if (a == "last") {
                if (p + 1 < applied) applied = (int)std::max<long>(0, p + 1);
                continue;
            }
            if (p < 0 || p >= (long)pats.size()) {
                if (p + firstId >= 0 && p + firstId < (long)pats.size() + otherPatterns) { ignoredOther++; continue; }
                bad("pattern " + a + " is not in " + o.patterns);
            }
            int ob = -1;
            if (b == "po") {
                is >> c;
                auto it = poObs.find(c);
                if (it == poObs.end()) bad("'" + c + "' is not an output port bit");
                ob = it->second;
            } else {
                long ch = -1, cell = -1;
                try {
                    ch = std::stol(b);
                    is >> c;
                    cell = std::stol(c);
                } catch (...) {
                    bad("expected '<pattern> <chain> <cell>' or '<pattern> po <port>'");
                }
                if (ch < 0 || ch >= (long)chainObs.size()) bad("there is no chain " + b);
                if (cell < 0 || cell >= (long)chainObs[ch].size()) bad("chain " + b + " has no cell " + c);
                ob = chainObs[ch][cell];
            }
            if (ob < 0) { ignoredX++; continue; }
            fails[p].push_back(ob);
        }
        for (auto& v : fails) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }
        for (int p = applied; p < (int)fails.size(); p++) fails[p].clear();
    }

    // The patterns packed 64 to a word per input.
    std::vector<std::vector<uint64_t>> packed;
    void pack() {
        packed.assign((pats.size() + 63) / 64, std::vector<uint64_t>(m.c.inputs.size(), 0));
        for (size_t p = 0; p < pats.size(); p++) {
            std::vector<uint64_t>& pk = packed[p / 64];
            for (size_t i = 0; i < pk.size(); i++) pk[i] |= (uint64_t)pats[p][i] << (p % 64);
        }
    }
    const std::vector<uint64_t>& words(size_t b0, uint64_t& valid) const {
        size_t n = std::min<size_t>(64, (size_t)applied - b0);
        valid = n == 64 ? ~0ull : ((1ull << n) - 1);
        return packed[b0 / 64];
    }

    int findFault(const std::string& spec) const {
        std::istringstream is(spec);
        std::string kind, name;
        is >> kind >> name;
        bool tr = o.fault == "transition";
        int sa = -1;
        if (!tr && (kind == "sa0" || kind == "sa1")) sa = kind == "sa1";
        if (tr && (kind == "str" || kind == "stf")) sa = kind == "stf";
        if (sa < 0 || name.empty())
            throw DiagError("-diag_inject wants '" + std::string(tr ? "str|stf" : "sa0|sa1") + " <pin>', not '" + spec + "'");
        for (size_t i = 0; i < faults.size(); i++)
            if (faults[i].sa == sa && m.sites[faults[i].site].name == name) {
                if (faults[i].node < 0) throw DiagError("fault '" + spec + "' is not in the capture model (clock, reset or scan path)");
                return (int)i;
            }
        throw DiagError("no fault site '" + name + "' in " + ds.top);
    }

    // The fail log the given faults would produce, each acting alone.
    void inject(const std::vector<std::string>& specs) {
        std::vector<int> fl;
        for (auto& s : specs) fl.push_back(findFault(s));
        fails.assign(pats.size(), {});
        applied = (int)pats.size();
        atpg::Sim sim(m.c);
        atpg::Sim::Work w;
        w.init(m.c.size());
        std::vector<std::pair<int, uint64_t>> at;
        pack();
        for (size_t b0 = 0; b0 < pats.size(); b0 += 64) {
            uint64_t valid;
            sim.good(words(b0, valid));
            for (int fi : fl) {
                sim.detect(w, faults[fi], &at);
                for (auto& [node, mask] : at)
                    for (uint64_t x = mask & valid; x; x &= x - 1) fails[b0 + (size_t)__builtin_ctzll(x)].push_back(obsOf[node]);
            }
        }
        for (auto& v : fails) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }
    }

    void writeFailLog(const std::string& path) const {
        std::ofstream f(path);
        if (!f) throw DiagError("cannot write " + path);
        f << "# fail log: <pattern> <chain> <cell> | <pattern> po <port>\ntop " << ds.top << "\npatterns " << o.patterns << "\n";
        for (size_t p = 0; p < fails.size(); p++)
            for (int ob : fails[p])
                for (auto& n : obsNames[ob]) f << p + (size_t)firstId << " " << n << "\n";
    }

    std::string faultName(int i) const {
        const atpg::Fault& f = faults[i];
        bool tr = o.fault == "transition";
        return std::string(tr ? (f.sa ? "stf " : "str ") : (f.sa ? "sa1 " : "sa0 ")) + m.sites[f.site].name;
    }

    void run() {
        auto t0 = std::chrono::steady_clock::now();
        threads = o.threads > 0 ? o.threads : std::max(1, cpuTopo().logical());
        atpg::Pool pool(threads);
        // One simulator per thread: a pass over many blocks runs a block
        // per thread, a pass over a few splits the candidates instead.
        struct Ctx {
            atpg::Sim sim;
            atpg::Sim::Work work;
            std::vector<std::pair<int, uint64_t>> at;
            std::vector<uint64_t> failMask, origMask;
            std::vector<long> tfsf, tpsf;
            long removed = 0;
            explicit Ctx(const atpg::Circuit& c) : sim(c) { work.init(c.size()); }
        };
        std::vector<Ctx> ctx;
        for (int t = 0; t < threads; t++) {
            ctx.emplace_back(m.c);
            ctx.back().failMask.assign(obsNodes.size(), 0);
            ctx.back().origMask.assign(obsNodes.size(), 0);
        }
        if (packed.empty()) pack();
        std::vector<std::vector<int>> left;   // not yet explained
        auto blockEnd = [&](size_t b0) { return std::min<size_t>(b0 + 64, (size_t)applied); };
        // The good machine and the fail masks of one block, in ctx[t].
        auto load = [&](int t, size_t b0, uint64_t& valid) {
            Ctx& c = ctx[t];
            c.sim.good(words(b0, valid));
            std::fill(c.failMask.begin(), c.failMask.end(), 0);
            std::fill(c.origMask.begin(), c.origMask.end(), 0);
            for (size_t p = b0; p < blockEnd(b0); p++) {
                for (int ob : left[p]) c.failMask[ob] |= 1ull << (p - b0);
                for (int ob : fails[p]) c.origMask[ob] |= 1ull << (p - b0);
            }
        };

        for (auto& v : fails) {
            totalFails += (long)v.size();
            failingPatterns += !v.empty();
        }
        left = fails;
        long leftCount = totalFails;

        while (leftCount > 0 && (int)defects.size() < o.maxDefects) {
            // Screening works on the first blocks that still hold failures.
            std::vector<size_t> first, later;
            for (size_t b0 = 0; b0 < (size_t)applied; b0 += 64) {
                bool any = false;
                for (size_t p = b0; p < std::min<size_t>(b0 + 64, (size_t)applied); p++) any |= !left[p].empty();
                (any && (int)first.size() < o.screenBlocks ? first : later).push_back(b0);
            }
            // Candidates: one fault that explains a failing pattern lies in
            // the fan-in of every point that fails in it. Where no node is
            // common to all (two defects in one pattern), any fan-in counts.
            std::vector<uint8_t> cone(m.c.size(), 0);
            std::vector<int> stack, seen(m.c.size(), -1), hits(m.c.size(), 0), touched;
            int tag = 0;
            for (size_t b0 : first)
                for (size_t p = b0; p < std::min<size_t>(b0 + 64, (size_t)applied); p++) {
                    if (left[p].empty()) continue;
                    touched.clear();
                    for (int ob : left[p]) {
                        stack.assign(1, obsNodes[ob]);
                        seen[obsNodes[ob]] = tag;
                        while (!stack.empty()) {
                            int n = stack.back();
                            stack.pop_back();
                            if (!hits[n]++) touched.push_back(n);
                            const int* a = m.c.in(n);
                            for (int k = 0; k < m.c.fiN[n]; k++)
                                if (seen[a[k]] != tag) { seen[a[k]] = tag; stack.push_back(a[k]); }
                        }
                        tag++;
                    }
                    int need = (int)left[p].size();
                    bool common = false;
                    for (int n : touched) common |= hits[n] == need;
                    for (int n : touched) {
                        if (!common || hits[n] == need) cone[n] = 1;
                        hits[n] = 0;
                    }
                }
            std::vector<Suspect> cand;
            for (size_t i = 0; i < faults.size(); i++)
                if (faults[i].node >= 0 && faults[i].rep == (int)i && cone[faults[i].node]) cand.push_back({(int)i});
            if (defects.empty()) candidates = (int)cand.size();

            auto pass = [&](const std::vector<size_t>& blocks) {
                // A bit an earlier defect explained is no misprediction,
                // hence origMask for tpsf.
                if (blocks.size() < 2 * (size_t)threads) {
                    for (size_t b0 : blocks) {
                        uint64_t valid;
                        load(0, b0, valid);
                        pool.each((int)cand.size(), 64, [&](int t, int i) {
                            Suspect& s = cand[i];
                            if (!ctx[0].sim.detect(ctx[t].work, faults[s.fault], &ctx[t].at)) return;
                            for (auto& [node, mask] : ctx[t].at) {
                                uint64_t d = mask & valid;
                                s.tfsf += __builtin_popcountll(d & ctx[0].failMask[obsOf[node]]);
                                s.tpsf += __builtin_popcountll(d & ~ctx[0].origMask[obsOf[node]]);
                            }
                        });
                    }
                    return;
                }
                for (Ctx& c : ctx) {
                    c.tfsf.assign(cand.size(), 0);
                    c.tpsf.assign(cand.size(), 0);
                }
                pool.each((int)blocks.size(), 1, [&](int t, int bi) {
                    Ctx& c = ctx[t];
                    uint64_t valid;
                    load(t, blocks[bi], valid);
                    for (size_t i = 0; i < cand.size(); i++) {
                        if (!c.sim.detect(c.work, faults[cand[i].fault], &c.at)) continue;
                        for (auto& [node, mask] : c.at) {
                            uint64_t d = mask & valid;
                            c.tfsf[i] += __builtin_popcountll(d & c.failMask[obsOf[node]]);
                            c.tpsf[i] += __builtin_popcountll(d & ~c.origMask[obsOf[node]]);
                        }
                    }
                });
                for (Ctx& c : ctx)
                    for (size_t i = 0; i < cand.size(); i++) {
                        cand[i].tfsf += c.tfsf[i];
                        cand[i].tpsf += c.tpsf[i];
                    }
            };
            auto better = [](const Suspect& a, const Suspect& b) {
                // The order of score(), in whole numbers.
                long la = a.tfsf * (b.tfsf + b.tfsp + b.tpsf), lb = b.tfsf * (a.tfsf + a.tfsp + a.tpsf);
                if (la != lb) return la > lb;
                if (a.tfsf != b.tfsf) return a.tfsf > b.tfsf;
                return a.fault < b.fault;
            };
            // Screen: a candidate that explains nothing in the first failing
            // blocks is out, and only the best of the rest see every pattern.
            pass(first);
            cand.erase(std::remove_if(cand.begin(), cand.end(), [](const Suspect& s) { return s.tfsf == 0; }), cand.end());
            long firstFails = 0;
            for (size_t b0 : first)
                for (size_t p = b0; p < std::min<size_t>(b0 + 64, (size_t)applied); p++) firstFails += (long)left[p].size();
            for (Suspect& s : cand) s.tfsp = firstFails - s.tfsf;
            std::sort(cand.begin(), cand.end(), better);
            if ((int)cand.size() > o.screenKeep) cand.resize((size_t)o.screenKeep);
            pass(later);
            if (cand.empty()) break;
            for (Suspect& s : cand) s.tfsp = leftCount - s.tfsf;
            std::sort(cand.begin(), cand.end(), better);
            Defect d;
            d.fails = leftCount;
            for (size_t i = 0; i < cand.size() && (int)i < o.maxSuspects; i++) d.suspects.push_back(cand[i]);
            // What the best candidate explains leaves the log.
            int best = cand[0].fault;
            std::vector<size_t> todo;
            for (size_t b0 = 0; b0 < (size_t)applied; b0 += 64)
                for (size_t p = b0; p < blockEnd(b0); p++)
                    if (!left[p].empty()) { todo.push_back(b0); break; }
            for (Ctx& c : ctx) c.removed = 0;
            pool.each((int)todo.size(), 1, [&](int t, int bi) {
                Ctx& c = ctx[t];
                size_t b0 = todo[bi];
                uint64_t valid;
                c.sim.good(words(b0, valid));
                c.sim.detect(c.work, faults[best], &c.at);
                for (auto& [node, mask] : c.at)
                    for (uint64_t x = mask & valid; x; x &= x - 1) {
                        auto& v = left[b0 + (size_t)__builtin_ctzll(x)];
                        auto it = std::lower_bound(v.begin(), v.end(), obsOf[node]);
                        if (it != v.end() && *it == obsOf[node]) { v.erase(it); c.removed++; }
                    }
            });
            long removed = 0;
            for (Ctx& c : ctx) removed += c.removed;
            defects.push_back(std::move(d));
            leftCount -= removed;
            if (!removed) break;
        }
        unexplained = leftCount;
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    // The faults of one equivalence class, the representative first.
    std::vector<int> classOf(int rep) const {
        std::vector<int> v{rep};
        for (size_t i = 0; i < faults.size(); i++)
            if ((int)i != rep && faults[i].node >= 0 && faults[i].rep == rep) v.push_back((int)i);
        return v;
    }

    void report(std::ostream& os) const {
        os << "mdft: diagnosis of " << ds.top << ": " << totalFails << " failing bit" << (totalFails == 1 ? "" : "s")
           << " in " << failingPatterns << " of " << applied << " " << o.fault << " patterns, " << candidates
           << " candidate faults\n";
        if (ignoredOther)
            os << "  " << ignoredOther << " failures are in "
               << (o.fault == "stuck" ? "transition" : "stuck-at") << " patterns: run again with -diag_fault "
               << (o.fault == "stuck" ? "transition" : "stuck") << " for those\n";
        if (ignoredX) os << "  " << ignoredX << " failures are at points the model does not observe: ignored\n";
        if (!totalFails) {
            os << "  nothing failed: nothing to diagnose\n";
            return;
        }
        for (size_t di = 0; di < defects.size(); di++) {
            const Defect& d = defects[di];
            const Suspect& b = d.suspects[0];
            os << "  defect " << di + 1 << ": explains " << b.tfsf << " of " << d.fails << " failing bits\n"
               << "    rank   score   tfsf   tfsp   tpsf  fault\n";
            int rank = 0;
            for (size_t i = 0; i < d.suspects.size(); i++) {
                const Suspect& s = d.suspects[i];
                if (i == 0 || s.tfsf != d.suspects[i - 1].tfsf || s.tpsf != d.suspects[i - 1].tpsf) rank = (int)i + 1;
                std::vector<int> cls = classOf(s.fault);
                os << "    " << std::setw(4) << rank << std::setw(7) << std::fixed << std::setprecision(1) << s.score()
                   << "%" << std::setw(7) << s.tfsf << std::setw(7) << s.tfsp << std::setw(7) << s.tpsf << "  "
                   << faultName(cls[0]) << "\n";
                for (size_t k = 1; k < cls.size(); k++)
                    os << std::string(41, ' ') << "= " << faultName(cls[k]) << "\n";
            }
        }
        if (unexplained)
            os << "  " << unexplained << " failing bits are not explained by "
               << (defects.size() == 1 ? "this fault" : "these faults") << "\n";
        os << "  " << std::fixed << std::setprecision(2) << seconds << " s on " << threads
           << (threads == 1 ? " thread\n" : " threads\n");
    }

    void writeJson(const std::string& path) const {
        std::ofstream f(path);
        if (!f) throw DiagError("cannot write " + path);
        auto q = [](const std::string& s) {
            std::string r = "\"";
            for (char c : s) {
                if (c == '"' || c == '\\') r += '\\';
                r += c;
            }
            return r + "\"";
        };
        f << "{\n  \"design\": " << q(ds.top) << ",\n  \"fault\": " << q(o.fault) << ",\n  \"patterns\": " << applied
          << ",\n  \"failing_patterns\": " << failingPatterns << ",\n  \"failing_bits\": " << totalFails
          << ",\n  \"candidates\": " << candidates << ",\n  \"unexplained\": " << unexplained << ",\n  \"defects\": [";
        for (size_t di = 0; di < defects.size(); di++) {
            f << (di ? "," : "") << "\n    {\"failing_bits\": " << defects[di].fails << ", \"suspects\": [";
            for (size_t i = 0; i < defects[di].suspects.size(); i++) {
                const Suspect& s = defects[di].suspects[i];
                f << (i ? "," : "") << "\n      {\"score\": " << std::fixed << std::setprecision(2) << s.score()
                  << ", \"tfsf\": " << s.tfsf << ", \"tfsp\": " << s.tfsp << ", \"tpsf\": " << s.tpsf << ", \"faults\": [";
                std::vector<int> cls = classOf(s.fault);
                for (size_t k = 0; k < cls.size(); k++) f << (k ? ", " : "") << q(faultName(cls[k]));
                f << "]}";
            }
            f << "\n    ]}";
        }
        f << "\n  ]\n}\n";
    }
};

// Exit code: 0 diagnosed (or nothing failed), 1 failures left unexplained.
inline int runDiagnose(const DiagOpts& o) {
    if (o.files.empty()) throw DiagError("-diagnose needs the gate netlist the patterns were made for");
    if (o.patterns.empty()) throw DiagError("-diagnose needs -patterns FILE (from matpg -patterns)");
    if (o.failLog.empty() && o.inject.empty()) throw DiagError("-diagnose needs a fail log");
    Diagnoser d(o);
    if (!o.inject.empty()) {
        d.inject(o.inject);
        if (!o.writeLog.empty()) d.writeFailLog(o.writeLog);
        if (o.failLog.empty() && !o.writeLog.empty()) {
            long n = 0;
            for (auto& v : d.fails) n += (long)v.size();
            if (!o.quiet) std::cout << "mdft: " << o.writeLog << ": " << n << " failing bits\n";
            return 0;
        }
    } else {
        d.readFailLog(o.failLog);
    }
    d.run();
    if (!o.quiet) d.report(std::cout);
    if (!o.json.empty()) d.writeJson(o.json);
    return d.unexplained ? 1 : 0;
}

}  // namespace dft
