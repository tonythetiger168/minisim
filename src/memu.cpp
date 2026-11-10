// memu.cpp - emulation and FPGA prototyping: puts a gate-level netlist on a
// board of FPGAs.
//
// What an emulator compiler does between synthesis and FPGA place and route:
//
//   clock conversion   Every flop moves onto one free-running clock, emu_clk,
//                      and keeps its value unless its own clock had an edge.
//                      Gated, divided and multiplexed clocks, both edges and
//                      several clock domains then need no clock routing at
//                      all, and there are no hold violations to fix. Each
//                      FPGA carries a copy of the clock generator.
//   memory mapping     Memory cells become FPGA RAM: block RAM for clocked
//                      reads, LUT RAM for small clockless ones, block RAM
//                      read on emu_clk for large clockless ones.
//   partitioning       The cells are split over the board's FPGAs to cut as
//                      few nets as possible within each FPGA's capacity.
//   pin multiplexing   A link with fewer wires than cut nets sends several
//                      nets down each wire in turn.
//   probes             Chosen nets are sampled every event into a trace
//                      buffer the host reads back; memu -trace makes a VCD.
//
// The input is what the tool line's synthesis writes: structural Verilog
// (src/gnetlist.hpp) over a Liberty library (src/liberty.hpp), plus a
// description of the memory cells. The output is one netlist per FPGA for
// FPGA synthesis and place and route, a model of the board, and a testbench
// that runs the original netlist next to the emulation and compares them, so
// the whole flow can be checked in any simulator before hardware exists.
//
// docs/EMULATION.md has the timing model and the file formats.
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>

#include "emu/emit.hpp"
#include "version.hpp"

using namespace emu;

namespace {

const char* kUsage =
    "usage: memu [options] netlist.v ...\n"
    "       memu -trace probes.txt samples.txt -o wave.vcd\n"
    "  -top NAME          top module (default: the one nothing instantiates)\n"
    "  -lib FILE          Liberty library of the netlist's cells (repeatable)\n"
    "  -mem FILE          memory cells: depth, width and the pins of each port (repeatable)\n"
    "  -board FILE        the board: FPGAs, their sizes, and the wires between them\n"
    "  -fpgas N           or a board of N equal FPGAs, every pair wired:\n"
    "    -lut N -ff N -bram-kb N    the size of each (default: unlimited)\n"
    "    -pins N                    wires between each pair (default 100)\n"
    "  -util F            fill each FPGA to at most this fraction (default: the board's, 0.7)\n"
    "  -tdm-max N         most signals one wire may carry in turn (default: the board's, 16)\n"
    "  -clock NAME[:PERIOD_NS[:PHASE_NS]]   a clock input (repeatable; default: found, 10 ns)\n"
    "  -assign GLOB=FPGA  keep the instances whose name matches in one FPGA (repeatable)\n"
    "  -io FPGA           put every port of the design on one FPGA\n"
    "  -probe GLOB        sample the nets whose name matches (repeatable)\n"
    "  -trace-depth N     samples kept per probe (default 1024)\n"
    "  -trigger NET[:N]   stop sampling N events (default: half the depth) after NET is 1\n"
    "  -edge              detect clock edges even where the generator could just say so\n"
    "  -settle N          run N deltas per event instead of the number worked out\n"
    "  -gates-per-lut F   library gates one LUT holds, for the size estimate (default 3)\n"
    "  -lutram-max BITS   largest clockless-read memory kept in LUT RAM (default 4096)\n"
    "  -fmax MHZ          emu_clk frequency for the speed estimate (default 50)\n"
    "  -starts N          partitioning attempts, best one kept (default 16)\n"
    "  -dump-hgr FILE     also write the partitioning problem as an hMETIS hypergraph\n"
    "  -threads N         worker threads (default: every logical CPU; the result is the same)\n"
    "  -o DIR             output directory (default: memu.d)\n"
    "  -q                 print errors only\n"
    "  -xtor-sv           print the path of the transactors (memu_xtor.sv) and exit\n"
    "  -xtor-mvp-sv       the same, with the msim -mvp host side (memu_mvp_host.sv)\n"
    "  -version, -h\n";

// The transactor sources (emu/sv), found by $MINISIM_EMU, next to an installed
// memu, or in the source tree.
std::string xtorDir() {
    if (const char* e = std::getenv("MINISIM_EMU")) return e;
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        auto inst = exe.parent_path().parent_path() / "share" / "minisim" / "emu";
        if (std::filesystem::exists(inst / "sv" / "memu_xtor.sv", ec)) return inst.string();
    }
#ifdef MINISIM_SRC_DIR
    return std::string(MINISIM_SRC_DIR) + "/../emu";
#else
    return "emu";
#endif
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw Error("cannot write " + path);
    f << text;
}

int fpgaIndex(const Board& b, const std::string& s) {
    for (int i = 0; i < b.k(); i++)
        if (b.fpgas[i].name == s) return i;
    char* e = nullptr;
    long v = std::strtol(s.c_str(), &e, 10);
    if (e && !*e && !s.empty() && v >= 0 && v < b.k()) return (int)v;
    throw Error("no FPGA named " + s + " on the board");
}

// The clock events of one repeat of the schedule, per clock: how many
// emulation events one cycle of that clock costs.
std::vector<double> eventsPerCycle(const Design& d, long unitPs) {
    std::vector<double> r(d.clocks.size(), 0);
    std::vector<int> act;
    for (size_t i = 0; i < d.clocks.size(); i++)
        if (d.clocks[i].livePos || d.clocks[i].liveNeg) act.push_back((int)i);
    if (act.empty()) return r;
    // Simulate the generator until every clock has done 64 cycles.
    std::vector<long> rem(d.clocks.size()), cycles(d.clocks.size(), 0);
    std::vector<char> val(d.clocks.size(), 0);
    for (int i : act) {
        auto& c = d.clocks[i];
        bool both = c.livePos && c.liveNeg;
        rem[i] = (both || c.livePos ? c.phasePs + c.periodPs / 2 : c.phasePs + c.periodPs) / unitPs;
    }
    long events = 0;
    for (int guard = 0; guard < 2000000; guard++) {
        long mn = rem[act[0]];
        for (int i : act) mn = std::min(mn, rem[i]);
        events++;
        bool allDone = true;
        for (int i : act) {
            auto& c = d.clocks[i];
            if (rem[i] == mn) {
                bool both = c.livePos && c.liveNeg;
                if (both) {
                    val[i] = !val[i];
                    rem[i] = c.periodPs / 2 / unitPs;
                    if (!val[i]) cycles[i]++;
                } else {
                    rem[i] = c.periodPs / unitPs;
                    cycles[i]++;
                }
            } else {
                rem[i] -= mn;
            }
            if (cycles[i] < 64) allDone = false;
        }
        if (allDone) break;
    }
    for (int i : act)
        if (cycles[i]) r[i] = (double)events / cycles[i];
    return r;
}

// ------------------------------------------------------------- trace to VCD

int traceToVcd(const std::string& mapFile, const std::string& dumpFile, const std::string& out) {
    struct P {
        int fpga, bit;
        std::string name;
    };
    std::vector<P> probes;
    long unit = 1;
    int nf = 1;
    std::string top = "top";
    {
        std::istringstream in(readFile(mapFile));
        std::string ln;
        while (std::getline(in, ln)) {
            std::istringstream ls(ln);
            std::string kw;
            ls >> kw;
            if (kw == "top") ls >> top;
            else if (kw == "unit_ps") ls >> unit;
            else if (kw == "fpgas") ls >> nf;
            else if (kw == "probe") {
                P p;
                ls >> p.fpga >> p.bit;
                std::getline(ls, p.name);
                while (!p.name.empty() && p.name[0] == ' ') p.name.erase(0, 1);
                probes.push_back(p);
            }
        }
    }
    if (probes.empty()) throw Error(mapFile + ": no probes");
    std::ostringstream o;
    o << "$version memu " << MINISIM_VERSION << " $end\n$timescale 1ps $end\n";
    // Scopes from the '/' in the net names.
    auto idOf = [](size_t i) {
        std::string s;
        do {
            s += (char)('!' + i % 94);
            i /= 94;
        } while (i);
        return s;
    };
    std::vector<size_t> order(probes.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return probes[a].name < probes[b].name; });
    std::vector<std::string> open{top};
    o << "$scope module " << top << " $end\n";
    for (size_t i : order) {
        std::vector<std::string> path{top};
        std::string leaf, part;
        std::stringstream ss(probes[i].name);
        std::vector<std::string> parts;
        while (std::getline(ss, part, '/')) parts.push_back(part);
        leaf = parts.back();
        for (size_t x = 0; x + 1 < parts.size(); x++) path.push_back(parts[x]);
        size_t common = 0;
        while (common < open.size() && common < path.size() && open[common] == path[common]) common++;
        while (open.size() > common) {
            o << "$upscope $end\n";
            open.pop_back();
        }
        while (open.size() < path.size()) {
            o << "$scope module " << path[open.size()] << " $end\n";
            open.push_back(path[open.size()]);
        }
        for (auto& c : leaf)
            if (c == ' ') c = '_';
        o << "$var wire 1 " << idOf(i) << " " << leaf << " $end\n";
    }
    while (!open.empty()) {
        o << "$upscope $end\n";
        open.pop_back();
    }
    o << "$enddefinitions $end\n";
    std::istringstream in(readFile(dumpFile));
    std::string ln;
    std::vector<char> lastV(probes.size(), '?');
    long samples = 0;
    while (std::getline(in, ln)) {
        std::istringstream ls(ln);
        std::string t;
        if (!(ls >> t)) continue;
        std::vector<std::string> f(nf);
        for (int x = 0; x < nf; x++)
            if (!(ls >> f[x])) throw Error(dumpFile + ": a sample has fewer than " + std::to_string(nf) + " FPGA fields");
        unsigned long long tv = std::strtoull(t.c_str(), nullptr, 16);
        bool stamped = false;
        for (size_t i = 0; i < probes.size(); i++) {
            auto& hex = f[probes[i].fpga];
            int nib = probes[i].bit / 4;
            char v = 'x';
            if (nib < (int)hex.size()) {
                char c = hex[hex.size() - 1 - nib];
                int dgt = isdigit((unsigned char)c) ? c - '0' : isxdigit((unsigned char)c) ? tolower(c) - 'a' + 10 : -1;
                if (dgt >= 0) v = (dgt >> (probes[i].bit % 4)) & 1 ? '1' : '0';
            }
            if (v == lastV[i]) continue;
            if (!stamped) o << "#" << tv * (unsigned long long)unit << "\n";
            stamped = true;
            o << v << idOf(i) << "\n";
            lastV[i] = v;
        }
        samples++;
    }
    writeFile(out, o.str());
    std::cout << "memu: " << samples << " samples of " << probes.size() << " probes -> " << out << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> netlists, libs, memFiles, clockSpecs, probePats, assigns;
    std::string top, boardFile, outDir = "memu.d", ioFpga, trigger, hgrFile;
    int nFpgas = 0, pins = 100, starts = 16, traceDepth = 1024, tdmMax = -1;
    double lut = 0, ff = 0, bramKb = 0, util = -1, fmax = 50;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    bool quiet = false;
    Options opt;
    std::vector<std::string> trace;
    bool traceMode = false, outGiven = false;

    try {
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            auto next = [&](const char* what) -> std::string {
                if (i + 1 >= argc) throw Error(a + " needs " + what);
                return argv[++i];
            };
            if (a == "-h" || a == "-help" || a == "--help") {
                std::cout << kUsage;
                return 0;
            } else if (a == "-version" || a == "--version") {
                std::cout << "memu " << MINISIM_VERSION << "\n";
                return 0;
            } else if (a == "-xtor-sv" || a == "-xtor-mvp-sv") {
                std::string dir = xtorDir();
                if (!std::filesystem::exists(dir + "/sv/memu_xtor.sv"))
                    throw Error("the transactors are not in " + dir + " (set $MINISIM_EMU)");
                std::cout << dir << "/sv/memu_xtor.sv";
                if (a == "-xtor-mvp-sv") std::cout << " " << dir << "/sv/memu_mvp_host.sv";
                std::cout << "\n";
                return 0;
            } else if (a == "-top") top = next("a module name");
            else if (a == "-lib") libs.push_back(next("a file"));
            else if (a == "-mem") memFiles.push_back(next("a file"));
            else if (a == "-board") boardFile = next("a file");
            else if (a == "-fpgas") nFpgas = std::atoi(next("a number").c_str());
            else if (a == "-lut") lut = std::atof(next("a number").c_str());
            else if (a == "-ff") ff = std::atof(next("a number").c_str());
            else if (a == "-bram-kb") bramKb = std::atof(next("a number").c_str());
            else if (a == "-pins") pins = std::atoi(next("a number").c_str());
            else if (a == "-util") util = std::atof(next("a fraction").c_str());
            else if (a == "-tdm-max") tdmMax = std::atoi(next("a number").c_str());
            else if (a == "-clock") clockSpecs.push_back(next("name[:period_ns[:phase_ns]]"));
            else if (a == "-assign") assigns.push_back(next("GLOB=FPGA"));
            else if (a == "-io") ioFpga = next("an FPGA");
            else if (a == "-probe") probePats.push_back(next("a net name pattern"));
            else if (a == "-trace-depth") traceDepth = std::atoi(next("a number").c_str());
            else if (a == "-trigger") trigger = next("a net name");
            else if (a == "-edge") opt.forceEdge = true;
            else if (a == "-settle") opt.settle = std::atoi(next("a number").c_str());
            else if (a == "-gates-per-lut") opt.gatesPerLut = std::atof(next("a number").c_str());
            else if (a == "-lutram-max") opt.lutramMaxBits = std::atol(next("a number").c_str());
            else if (a == "-fmax") fmax = std::atof(next("MHz").c_str());
            else if (a == "-starts") starts = std::max(1, std::atoi(next("a number").c_str()));
            else if (a == "-threads" || a == "-j") threads = (unsigned)std::max(1, std::atoi(next("a number").c_str()));
            else if (a == "-o") outDir = next("a path"), outGiven = true;
            else if (a == "-q") quiet = true;
            else if (a == "-dump-hgr") hgrFile = next("a file");
            else if (a == "-trace") {
                traceMode = true;
                trace.push_back(next("the probe map"));
                trace.push_back(next("the sample file"));
            } else if (!a.empty() && a[0] == '-') throw Error("unknown option " + a + " (memu -h lists them)");
            else netlists.push_back(a);
        }
        if (traceMode) return traceToVcd(trace[0], trace[1], outGiven ? outDir : "memu.vcd");
        if (netlists.empty()) {
            std::cerr << kUsage;
            return 2;
        }
        if (opt.gatesPerLut <= 0) throw Error("-gates-per-lut is above 0");
        if (traceDepth < 2 || traceDepth > (1 << 24)) throw Error("-trace-depth is from 2 to 16777216");

        auto t0 = std::chrono::steady_clock::now();
        auto since = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        };

        Board board = !boardFile.empty() ? Board::load(boardFile)
                                         : Board::uniform(std::max(1, nFpgas), lut, ff, bramKb, std::max(1, pins));
        if (util > 0) board.util = std::min(1.0, util);
        if (tdmMax > 0) board.tdmMax = tdmMax;
        if (board.k() > 64) throw Error("at most 64 FPGAs on a board");

        Design d;
        d.opt = opt;
        d.load(netlists, libs, memFiles, top);
        double tLoad = since();
        d.findClocks(clockSpecs);
        d.computeLevels();

        // Instances and ports the user placed.
        std::vector<int> fixed(d.ni() + d.nl.ports.size(), -1);
        for (auto& a : assigns) {
            auto eq = a.rfind('=');
            if (eq == std::string::npos) throw Error("-assign needs GLOB=FPGA");
            std::string pat = a.substr(0, eq);
            int f = fpgaIndex(board, a.substr(eq + 1));
            size_t hits = 0;
            for (size_t i = 0; i < d.ni(); i++)
                if (globMatch(pat.c_str(), d.nl.insts[i].name.c_str())) {
                    fixed[i] = f;
                    hits++;
                }
            if (!hits) d.warnings.push_back("-assign " + pat + " matches no instance");
        }
        if (!ioFpga.empty()) {
            int f = fpgaIndex(board, ioFpga);
            for (size_t p = 0; p < d.nl.ports.size(); p++) fixed[d.ni() + p] = f;
        }

        if (!hgrFile.empty()) {
            // The partitioning problem in hMETIS format, for comparing with other partitioners.
            HGraph g = buildGraph(d, fixed);
            std::ostringstream h;
            h << g.ne() << " " << g.nv << " 11\n";
            for (int e = 0; e < g.ne(); e++) {
                h << g.netW[e];
                for (int x = g.netOff[e]; x < g.netOff[e + 1]; x++) h << " " << g.netPins[x] + 1;
                h << "\n";
            }
            for (int v = 0; v < g.nv; v++) h << std::lround(g.w[v][0] * opt.gatesPerLut) << "\n";
            writeFile(hgrFile, h.str());
        }
        Plan pl = makePlan(d, board, fixed, starts, threads);
        double tPart = since();

        Emitter em(d, board, pl);
        em.traceDepth = traceDepth;
        em.setProbes(probePats, d.warnings);
        if (!trigger.empty()) {
            if (em.probes.empty()) throw Error("-trigger needs -probe: there is nothing to stop sampling");
            auto c = trigger.rfind(':');
            std::string name = trigger;
            em.triggerPost = (1 << em.traceAw()) / 2;
            if (c != std::string::npos && c + 1 < trigger.size() && isdigit((unsigned char)trigger[c + 1])) {
                name = trigger.substr(0, c);
                em.triggerPost = std::atoi(trigger.c_str() + c + 1);
            }
            for (size_t n = 0; n < d.nn() && em.triggerNet < 0; n++)
                if (d.nl.nets[n] == name && d.drv[n] != -1 && d.clockOfNet[n] < 0) em.triggerNet = (int)n;
            if (em.triggerNet < 0) throw Error("-trigger " + name + ": no net of that name");
            em.triggerName = name;
            em.triggerFpga = pl.netPart(d, em.triggerNet);
            em.triggerPost = std::min(em.triggerPost, (1 << em.traceAw()) - 1);
        }

        ::mkdir(outDir.c_str(), 0777);
        std::string base = outDir + "/" + em.top;
        int k = board.k();
        // One file per FPGA, each written by its own thread.
        parallelFor((size_t)k + 3, threads, [&](size_t x) {
            if ((int)x < k) writeFile(base + "_fpga" + std::to_string(x) + ".v", em.fpga((int)x));
            else if ((int)x == k) writeFile(base + "_emu_lib.v", em.library());
            else if ((int)x == k + 1) writeFile(base + "_emu.v", em.wrapper());
            else writeFile(base + "_emu_tb.sv", em.testbench());
        });
        std::string flist;
        for (int f = 0; f < k; f++) flist += em.top + "_fpga" + std::to_string(f) + ".v\n";
        flist += em.top + "_emu_lib.v\n" + em.top + "_emu.v\n";
        writeFile(base + "_emu.f", flist);
        if (!em.probes.empty()) writeFile(base + "_probes.txt", em.probeMap());

        // The report.
        std::ostringstream r;
        size_t nFF = 0, nLatch = 0, nMem = 0, nComb = 0;
        for (size_t i = 0; i < d.ni(); i++) {
            Kind kd = d.kind((int)i);
            (kd == Kind::FF ? nFF : kd == Kind::Latch ? nLatch : kd == Kind::Mem ? nMem : nComb)++;
        }
        r << "memu " << MINISIM_VERSION << ": " << em.top << " on " << board.name << " (" << k << " FPGA"
          << (k > 1 ? "s" : "") << ")\n"
          << "design: " << d.ni() << " cells (" << nComb << " gates, " << nFF << " flops, " << nLatch << " latches, "
          << nMem << " memories), " << d.nn() << " nets\n";
        r << "clocks: " << (d.strobe ? "strobes (every clock only reaches clock pins)" : "edge detection")
          << (opt.forceEdge ? " (-edge)" : "") << "\n";
        auto perCycle = eventsPerCycle(d, em.unitPs);
        int K = em.N() * pl.B;
        for (size_t i = 0; i < d.clocks.size(); i++) {
            auto& c = d.clocks[i];
            r << "  " << c.name << ": period " << c.periodPs / 1000.0 << " ns";
            if (c.phasePs) r << ", phase " << c.phasePs / 1000.0 << " ns";
            r << (c.pure ? "" : ", also read as data or gated");
            if (!c.livePos && !c.liveNeg) r << ", unused";
            else if (d.strobe) r << ", " << (c.livePos && c.liveNeg ? "both edges" : c.livePos ? "rising edge" : "falling edge");
            if (perCycle[i] > 0)
                r << "; one cycle is " << perCycle[i] * K << " emu_clk cycle" << (perCycle[i] * K == 1 ? "" : "s") << ", " << fmax / (perCycle[i] * K) << " MHz at "
                  << fmax << " MHz emu_clk";
            r << "\n";
        }
        r << "timing: " << d.dc << " delta" << (d.dc == 1 ? "" : "s") << " after a clock event, " << d.da
          << " after the inputs change, " << pl.B << " emu_clk cycle" << (pl.B == 1 ? "" : "s") << " per delta: "
          << K << " emu_clk cycle" << (K == 1 ? "" : "s") << " per event\n";
        r << "time: emu_time counts in units of " << em.unitPs << " ps\n";
        for (auto& m : d.mems) {
            if (!d.roleOf.count(m.cell)) continue;
            size_t cnt = 0;
            int ri = d.roleOf.at(m.cell);
            for (size_t i = 0; i < d.ni(); i++) cnt += d.role[i] == ri;
            r << "memory " << m.cell << ": " << m.depth << " x " << m.width << ", " << cnt << " instance" << (cnt == 1 ? "" : "s")
              << ", " << (!m.asyncRead() ? "block RAM" : m.lagRead ? "block RAM read on emu_clk" : "LUT RAM") << "\n";
        }
        if (k > 1)
            r << "partition: cut cost " << (long)pl.stats.cost << " (best of " << pl.starts << " starts, seed "
              << pl.stats.seed << ")\n";
        for (int f = 0; f < k; f++) {
            auto& fp = board.fpgas[f];
            auto pct = [&](double used, double cap) {
                std::ostringstream s;
                if (cap > 0) s << " (" << (long)std::lround(100 * used / cap) << "%)";
                return s.str();
            };
            r << "  " << fp.name << ": " << em.instsOf[f].size() << " cells, " << (long)std::ceil(pl.load[f][0]) << " LUTs"
              << pct(pl.load[f][0], fp.lut) << ", " << (long)pl.load[f][1] << " flops" << pct(pl.load[f][1], fp.ff) << ", "
              << (long)std::ceil(pl.load[f][2] / 1024) << " kb block RAM" << pct(pl.load[f][2] / 1024, fp.bramKb) << ", "
              << em.piOf[f].size() + em.poOf[f].size() << " design pins";
            if (!em.probesOf[f].empty()) r << ", " << em.probesOf[f].size() << " probes";
            r << "\n";
        }
        for (auto& l : pl.links) {
            if (l.ab.empty() && l.ba.empty()) continue;
            r << "  link " << board.fpgas[l.a].name << "-" << board.fpgas[l.b].name << ": " << l.ab.size() << " + "
              << l.ba.size() << " signals on " << l.pinsAB + l.pinsBA << " of " << l.pins << " wires";
            if (l.ratioAB > 1 || l.ratioBA > 1) r << ", multiplexed " << std::max(l.ratioAB, l.ratioBA) << ":1";
            r << "\n";
        }
        if (!em.probes.empty()) {
            r << "probes: " << em.probes.size() << " nets, " << (1 << em.traceAw()) << " samples each";
            if (em.triggerNet >= 0) r << ", stops " << em.triggerPost << " events after " << em.triggerName << " is 1";
            r << "\n";
        }
        for (auto& w : d.warnings) r << "warning: " << w << "\n";
        r << "wrote " << outDir << "/: " << em.top << "_fpga0" << (k > 1 ? ".." + std::to_string(k - 1) : "") << ".v, "
          << em.top << "_emu_lib.v, " << em.top << "_emu.v (the board), " << em.top << "_emu_tb.sv (netlist against emulation)"
          << (em.probes.empty() ? "" : ", " + em.top + "_probes.txt") << "\n";
        writeFile(base + "_emu.rpt", r.str());
        if (!quiet) {
            std::cout << r.str();
            char line[200];
            std::snprintf(line, sizeof line,
                          "memu: %u thread%s: read %.2f s, clocks %.2f s, graph %.2f s, partition %.2f s, route %.2f s, "
                          "write %.2f s\n",
                          threads, threads == 1 ? "" : "s", tLoad, tPart - tLoad - pl.tGraph - pl.tPart - pl.tRoute,
                          pl.tGraph, pl.tPart, pl.tRoute, since() - tPart);
            std::cout << line;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "memu: " << e.what() << "\n";
        return 2;
    }
}
