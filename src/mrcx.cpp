// mrcx.cpp - parasitic RC extraction: routed DEF + LEF in, SPEF out.
//
//   mrcx -lef tech.lef -lef cells.lef -def routed.def -o design.spef
//   mrcx -lef tech.lef -lef cells.lef -def routed.def -itf process.itf -temp 125 -o slow.spef
//
// The LEF/DEF readers are src/lef.hpp and src/def.hpp (shared with the other
// physical tools); the extraction itself is src/rcx.hpp. This file turns the
// DEF into the engine's layout, runs it, and writes the SPEF and the report.
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

#include "def.hpp"
#include "rcx.hpp"
#include "version.hpp"

namespace {

struct Args {
    std::vector<std::string> lefs;
    std::string def, itf, map, out, report;
    rcx::Options opt;
    bool werror = false, quiet = false, nameMap = true;
};

void usage() {
    std::cout <<
        "mrcx - parasitic RC extraction: routed DEF + LEF in, SPEF out\n"
        "usage: mrcx -lef FILE [-lef FILE ...] -def FILE [-o FILE.spef] [options]\n"
        "  -lef FILE          technology and cell LEF (repeat; technology first)\n"
        "  -def FILE          routed design\n"
        "  -o FILE            SPEF to write (default: <design>.spef)\n"
        "  -itf FILE          process stack: conductor and dielectric thickness, permittivity,\n"
        "                     sheet and via resistance with temperature coefficients\n"
        "                     (without it the LEF layer values are used)\n"
        "  -map FILE          lines of \"LEF-layer process-conductor\" when the names differ\n"
        "  -temp C            temperature for resistance (default: the process file's nominal)\n"
        "  -rscale F          multiply every resistance\n"
        "  -cscale F          multiply every capacitance\n"
        "  -ccscale F         multiply coupling capacitance only\n"
        "  -no_coupling       ground every coupling capacitance (decoupled SPEF)\n"
        "  -cc_factor F       with -no_coupling or below the threshold: grounded value = F x coupling (default 1)\n"
        "  -cc_threshold FF   ground a coupling capacitance smaller than this many fF\n"
        "  -cc_dist UM        how far to look for a neighbour on the same layer (default: 6 pitches)\n"
        "  -max_seg UM        cut wires into pieces no longer than this (default 20; 0: only at junctions)\n"
        "  -no_name_map       write names in full instead of a *NAME_MAP\n"
        "  -report FILE       per-net table: length, resistance, capacitance\n"
        "  -date TEXT         the SPEF *DATE (default: now), for files that compare equal\n"
        "  -threads N         worker threads (default: all logical cores; 1: single thread)\n"
        "  -Werror            exit 1 when a net is open or two nets are shorted\n"
        "  -q                 no summary on stdout\n"
        "  -version           print the version\n";
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

rcx::Rect conv(const phys::Rect& r, double s) {
    auto c = [&](int64_t v) { return static_cast<rcx::i64>(std::llround(static_cast<double>(v) * s)); };
    return {c(r.x0), c(r.y0), c(r.x1), c(r.y1)};
}

struct ViaInfo { int lo = -1, hi = -1, cuts = 1; rcx::Rect rlo, rhi; double res = -1; };

// The DEF and LEF as the engine's layout. Nets are converted in parallel.
rcx::Layout toLayout(const phys::Tech& tech, const phys::Design& d, int threads,
                     std::vector<std::string>& warnings) {
    rcx::Layout L;
    L.design = d.name;
    L.dbu = d.dbu;
    const double ts = static_cast<double>(d.dbu) / tech.dbu;   // LEF units -> DEF units
    std::unordered_map<std::string, int> layerIdx;
    std::vector<const phys::Layer*> routing = tech.routing();
    if (routing.empty()) throw std::runtime_error("the LEF has no ROUTING layer");
    std::vector<rcx::i64> defWidth;
    for (auto* l : routing) {
        rcx::Layer k;
        k.name = l->name;
        k.width = static_cast<double>(l->width) / tech.dbu;
        k.spacing = static_cast<double>(l->spacing) / tech.dbu;
        k.rpersq = l->rPerSq;
        k.areaCap = l->cPerSqDist >= 0 ? l->cPerSqDist * 1000 : -1;
        k.edgeCap = l->edgeCap >= 0 ? l->edgeCap * 1000 : -1;
        k.thickness = l->thickness;
        k.height = l->height;
        layerIdx[l->name] = static_cast<int>(L.layers.size());
        L.layers.push_back(k);
        defWidth.push_back(static_cast<rcx::i64>(std::llround(static_cast<double>(l->width) * ts)));
    }
    auto layerOf = [&](const std::string& n) {
        auto it = layerIdx.find(n);
        return it == layerIdx.end() ? -1 : it->second;
    };

    // Vias: the DEF's own definitions win over the LEF's.
    std::unordered_map<std::string, ViaInfo> vias;
    auto addVia = [&](const phys::ViaDef& v, double s) {
        ViaInfo vi;
        int cuts = 0;
        double cutRes = -1;
        for (auto& sh : v.shapes) {
            int li = layerOf(sh.layer);
            if (li < 0) {
                const phys::Layer* cl = tech.layer(sh.layer);
                if (cl && cl->type == phys::Layer::Cut) { cuts++; if (cl->cutRes >= 0) cutRes = cl->cutRes; }
                continue;
            }
            if (vi.lo < 0 || li < vi.lo) vi.lo = li;
            if (li > vi.hi) vi.hi = li;
        }
        if (vi.lo < 0 || vi.lo == vi.hi) return;
        bool flo = true, fhi = true;
        for (auto& sh : v.shapes) {
            int li = layerOf(sh.layer);
            rcx::Rect r = conv(sh.r, s);
            auto grow = [&](rcx::Rect& a, bool& first) {
                if (first) { a = r; first = false; return; }
                a = {std::min(a.x0, r.x0), std::min(a.y0, r.y0), std::max(a.x1, r.x1), std::max(a.y1, r.y1)};
            };
            if (li == vi.lo) grow(vi.rlo, flo);
            if (li == vi.hi) grow(vi.rhi, fhi);
        }
        vi.cuts = std::max(cuts, 1);
        vi.res = v.res >= 0 ? v.res : (cutRes >= 0 ? cutRes / vi.cuts : -1);
        vias[v.name] = vi;
    };
    for (auto& v : tech.vias) addVia(v, ts);
    for (auto& v : d.vias) addVia(v, 1.0);

    std::unordered_map<std::string, const phys::Component*> comps;
    comps.reserve(d.comps.size() * 2);
    for (auto& c : d.comps) comps[c.name] = &c;
    std::unordered_map<std::string, const phys::IoPin*> ios;
    for (auto& p : d.pins) ios[p.name] = &p;

    const size_t ns = d.nets.size();
    L.nets.resize(ns + d.specialNets.size());
    std::vector<std::vector<std::string>> warn(L.nets.size());
    auto wires = [&](const phys::Net& n, rcx::Net& o, bool special, std::vector<std::string>& w) {
        for (auto& s : n.segs) {
            int li = layerOf(s.layer);
            if (li < 0) { w.push_back("net " + n.name + ": wire on unknown layer " + s.layer); continue; }
            rcx::Wire k;
            k.layer = li;
            k.x0 = s.x0; k.y0 = s.y0; k.x1 = s.x1; k.y1 = s.y1;
            k.width = s.width > 0 ? s.width : defWidth[static_cast<size_t>(li)];
            rcx::i64 dflt = special ? 0 : k.width / 2;
            k.ext0 = s.ext0 >= 0 ? s.ext0 : dflt;
            k.ext1 = s.ext1 >= 0 ? s.ext1 : dflt;
            o.wires.push_back(k);
        }
        for (auto& r : n.rects) {
            int li = layerOf(r.layer);
            if (li >= 0) o.patches.push_back({li, conv(r.r, 1.0)});
        }
    };
    auto dirOf = [](const std::string& s) { return s == "INPUT" ? 'I' : s == "OUTPUT" ? 'O' : 'B'; };
    const size_t blockSize = 256;
    rcx::detail::parBlocks((L.nets.size() + blockSize - 1) / blockSize, threads, [&](size_t b) {
        for (size_t i = b * blockSize; i < std::min(L.nets.size(), (b + 1) * blockSize); i++) {
            rcx::Net& o = L.nets[i];
            auto& w = warn[i];
            if (i >= ns) {   // special nets: power and ground metal
                const phys::Net& n = d.specialNets[i - ns];
                o.name = n.name;
                o.power = true;
                wires(n, o, true, w);
                continue;
            }
            const phys::Net& n = d.nets[i];
            o.name = n.name;
            o.power = n.use == "POWER" || n.use == "GROUND";
            wires(n, o, false, w);
            if (o.power) continue;
            for (auto& v : n.vias) {
                auto it = vias.find(v.via);
                if (it == vias.end()) { w.push_back("net " + n.name + ": unknown via " + v.via); continue; }
                const ViaInfo& vi = it->second;
                rcx::Via k;
                k.lo = vi.lo; k.hi = vi.hi; k.x = v.x; k.y = v.y; k.cuts = vi.cuts; k.res = vi.res;
                k.rlo = {vi.rlo.x0 + v.x, vi.rlo.y0 + v.y, vi.rlo.x1 + v.x, vi.rlo.y1 + v.y};
                k.rhi = {vi.rhi.x0 + v.x, vi.rhi.y0 + v.y, vi.rhi.x1 + v.x, vi.rhi.y1 + v.y};
                o.vias.push_back(k);
            }
            for (auto& c : n.conns) {
                rcx::Pin p;
                if (c.inst == "PIN") {
                    p.pin = c.pin;
                    auto it = ios.find(c.pin);
                    if (it == ios.end()) { w.push_back("net " + n.name + ": no PINS entry for " + c.pin); o.pins.push_back(p); continue; }
                    const phys::IoPin& io = *it->second;
                    p.dir = dirOf(io.dir);
                    p.x = io.x; p.y = io.y;
                    for (auto& s : io.shapes) {
                        int li = layerOf(s.layer);
                        if (li >= 0) p.shapes.push_back({li, conv(s.r, 1.0)});
                    }
                } else {
                    p.inst = c.inst; p.pin = c.pin;
                    auto it = comps.find(c.inst);
                    if (it == comps.end()) { w.push_back("net " + n.name + ": no component " + c.inst); o.pins.push_back(p); continue; }
                    const phys::Component& cm = *it->second;
                    p.cell = cm.macro;
                    p.x = cm.x; p.y = cm.y;
                    const phys::Macro* m = tech.macro(cm.macro);
                    const phys::MacroPin* mp = nullptr;
                    if (m)
                        for (auto& q : m->pins)
                            if (q.name == c.pin) { mp = &q; break; }
                    if (!mp) {
                        w.push_back("net " + n.name + ": no LEF pin " + cm.macro + "/" + c.pin + " for " + c.inst);
                        o.pins.push_back(p);
                        continue;
                    }
                    p.dir = dirOf(mp->dir);
                    rcx::i64 mw = static_cast<rcx::i64>(std::llround(static_cast<double>(m->w) * ts));
                    rcx::i64 mh = static_cast<rcx::i64>(std::llround(static_cast<double>(m->h) * ts));
                    for (auto& s : mp->ports) {
                        int li = layerOf(s.layer);
                        if (li < 0) continue;
                        rcx::Rect r = conv(s.r, ts);
                        phys::Rect t = phys::xform({r.x0, r.y0, r.x1, r.y1}, cm.orient, mw, mh, cm.x, cm.y);
                        p.shapes.push_back({li, {t.x0, t.y0, t.x1, t.y1}});
                    }
                }
                o.pins.push_back(std::move(p));
            }
        }
    });
    // The same message for thousands of nets helps nobody: keep the first of each kind.
    std::map<std::string, int> kinds;
    for (auto& ws : warn)
        for (auto& m : ws) {
            std::string kind = m.substr(m.find(": ") + 2);
            kind = kind.substr(0, kind.find_last_of(' '));
            if (kinds[kind]++ < 5) warnings.push_back(m);
        }
    for (auto& [k, n] : kinds)
        if (n > 5) warnings.push_back("... and " + std::to_string(n - 5) + " more: " + k);
    return L;
}

double secondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    std::string date;
    try {
        for (int i = 1; i < argc; i++) {
            std::string s = argv[i];
            auto val = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(s + " needs a value");
                return argv[++i];
            };
            auto numv = [&]() {
                std::string v = val();
                char* end = nullptr;
                double x = std::strtod(v.c_str(), &end);
                if (end == v.c_str() || *end) throw std::runtime_error(s + ": '" + v + "' is not a number");
                return x;
            };
            if (s == "-h" || s == "-help" || s == "--help") { usage(); return 0; }
            else if (s == "-version" || s == "--version" || s == "-v") {
                std::cout << "mrcx " << MINISIM_VERSION << (std::strlen(MINISIM_GIT) ? " (" MINISIM_GIT ")" : "") << "\n";
                return 0;
            }
            else if (s == "-lef") a.lefs.push_back(val());
            else if (s == "-def") a.def = val();
            else if (s == "-itf") a.itf = val();
            else if (s == "-map") a.map = val();
            else if (s == "-o") a.out = val();
            else if (s == "-report") a.report = val();
            else if (s == "-temp") { a.opt.temp = numv(); a.opt.tempGiven = true; }
            else if (s == "-rscale") a.opt.rscale = numv();
            else if (s == "-cscale") a.opt.cscale = numv();
            else if (s == "-ccscale") a.opt.ccscale = numv();
            else if (s == "-no_coupling") a.opt.coupled = false;
            else if (s == "-cc_factor") a.opt.ccFactor = numv();
            else if (s == "-cc_threshold") a.opt.ccThreshold = numv();
            else if (s == "-cc_dist") a.opt.ccDist = numv();
            else if (s == "-max_seg") a.opt.maxSeg = numv();
            else if (s == "-no_name_map") a.nameMap = false;
            else if (s == "-date") date = val();   // fixed *DATE, for reproducible files
            else if (s == "-threads") a.opt.threads = static_cast<int>(numv());
            else if (s == "-Werror") a.werror = true;
            else if (s == "-q") a.quiet = true;
            else throw std::runtime_error("unknown option " + s + " (see mrcx -help)");
        }
        if (a.lefs.empty() || a.def.empty()) {
            usage();
            return 2;
        }
        if (a.opt.threads < 0) throw std::runtime_error("-threads must be 1 or more");
        int threads = a.opt.threads > 0 ? a.opt.threads : static_cast<int>(std::thread::hardware_concurrency());
        if (threads < 1) threads = 1;
        a.opt.threads = threads;

        auto t0 = std::chrono::steady_clock::now();
        phys::Tech tech;
        phys::Design design;
        // The DEF is the big file: read it while the LEFs are read.
        std::string defErr;
        std::thread defReader;
        if (threads > 1)
            defReader = std::thread([&] {
                try { phys::readDef(a.def, design); } catch (const std::exception& e) { defErr = e.what(); if (defErr.empty()) defErr = "error"; }
            });
        try {
            for (auto& l : a.lefs) phys::readLef(l, tech);
        } catch (...) {
            if (defReader.joinable()) defReader.join();
            throw;
        }
        if (defReader.joinable()) defReader.join();
        else phys::readDef(a.def, design);
        if (!defErr.empty()) throw std::runtime_error(defErr);
        double tRead = secondsSince(t0);

        std::vector<std::string> warnings;
        auto t1 = std::chrono::steady_clock::now();
        rcx::Layout L = toLayout(tech, design, threads, warnings);
        rcx::Stack itf;
        if (!a.itf.empty()) itf = rcx::Stack::parseItf(slurp(a.itf), a.itf);
        std::map<std::string, std::string> map;
        if (!a.map.empty()) {
            std::istringstream in(slurp(a.map));
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream ls(line.substr(0, line.find('#')));
                std::string x, y;
                if (ls >> x >> y) map[x] = y;
            }
        }
        rcx::Stack stack = rcx::makeStack(L, a.itf.empty() ? nullptr : &itf, map);
        rcx::Result R = rcx::extract(L, stack, a.opt);
        double tExtract = secondsSince(t1);

        auto t2 = std::chrono::steady_clock::now();
        if (a.out.empty()) a.out = L.design + ".spef";
        std::FILE* f = std::fopen(a.out.c_str(), "wb");
        if (!f) throw std::runtime_error("cannot write " + a.out);
        std::vector<char> big(1 << 20);
        std::setvbuf(f, big.data(), _IOFBF, big.size());
        rcx::SpefOptions so;
        so.version = MINISIM_VERSION;
        so.nameMap = a.nameMap;
        so.threads = threads;
        std::time_t now = std::time(nullptr);
        char stamp[64];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
        so.date = date.empty() ? stamp : date;
        rcx::writeSpef(f, L, R, so);
        if (std::fclose(f) != 0) throw std::runtime_error("error writing " + a.out);
        double tWrite = secondsSince(t2);

        // Totals.
        size_t nets = 0, nodes = 0, res = 0, ccs = 0, opens = 0, unrouted = 0;
        double len = 0, cg = 0, cc = 0;
        for (size_t i = 0; i < L.nets.size(); i++) {
            if (L.nets[i].power) continue;
            const rcx::NetOut& n = R.nets[i];
            nets++; nodes += n.nodes.size(); res += n.res.size(); ccs += n.cc.size();
            len += n.wirelen; cg += n.cgTotal; cc += n.ccTotal;
            if (n.opens) {
                if (L.nets[i].wires.empty() && L.nets[i].vias.empty()) unrouted++;
                else opens++;
            }
        }
        for (auto& w : R.warnings) warnings.push_back(w);
        for (auto& w : warnings) std::cerr << "mrcx: warning: " << w << "\n";
        int shown = 0;
        for (size_t i = 0; i < L.nets.size(); i++) {
            if (L.nets[i].power || !R.nets[i].opens || (L.nets[i].wires.empty() && L.nets[i].vias.empty())) continue;
            if (shown++ < 10)
                std::cerr << "mrcx: open: net " << L.nets[i].name << " is in " << R.nets[i].opens + 1
                          << " parts that the routing does not join\n";
        }
        if (shown > 10) std::cerr << "mrcx: ... and " << shown - 10 << " more open nets\n";
        shown = 0;
        for (auto& s : R.shorts)
            if (shown++ < 10)
                std::cerr << "mrcx: short: nets " << L.nets[static_cast<size_t>(s.netA)].name << " and "
                          << L.nets[static_cast<size_t>(s.netB)].name << " overlap on "
                          << L.layers[static_cast<size_t>(s.layer)].name << " at ("
                          << static_cast<double>(s.where.x0) / L.dbu << ", " << static_cast<double>(s.where.y0) / L.dbu << ")\n";
        if (shown > 10) std::cerr << "mrcx: ... and " << shown - 10 << " more shorts\n";

        if (!a.report.empty()) {
            std::ofstream rp(a.report);
            if (!rp) throw std::runtime_error("cannot write " + a.report);
            rp << "# mrcx report for " << L.design << " (" << stack.name << " stack, " << R.temp << " C)\n";
            rp << "# layer  wire length um\n";
            for (size_t l = 0; l < L.layers.size(); l++) rp << "#   " << L.layers[l].name << "  " << R.layerLen[l] << "\n";
            rp << "# net  length_um  res_ohm  cap_ground_fF  cap_coupling_fF  cap_total_fF  nodes  open_parts\n";
            char line[256];
            for (size_t i = 0; i < L.nets.size(); i++) {
                if (L.nets[i].power) continue;
                const rcx::NetOut& n = R.nets[i];
                std::snprintf(line, sizeof line, "  %.3f  %.4g  %.6g  %.6g  %.6g  %zu  %d\n", n.wirelen, n.rTotal,
                              n.cgTotal, n.ccTotal, n.cgTotal + n.ccTotal, n.nodes.size(), n.opens);
                rp << L.nets[i].name << line;
            }
        }
        if (!a.quiet) {
            std::printf("mrcx: %s: %zu nets, %.1f um of wire, %zu nodes, %zu resistors, %zu coupling capacitors\n",
                        L.design.c_str(), nets, len, nodes, res, ccs / 2);
            std::printf("mrcx: capacitance %.4g fF to ground, %.4g fF between nets (%s stack, %g C)\n", cg, cc / 2,
                        stack.name.c_str(), R.temp);
            if (opens || unrouted || !R.shorts.empty())
                std::printf("mrcx: %zu open nets, %zu unrouted nets, %zu shorts\n", opens, unrouted, R.shorts.size());
            std::printf("mrcx: wrote %s (read %.2f s, extract %.2f s, write %.2f s, %d threads)\n", a.out.c_str(),
                        tRead, tExtract, tWrite, threads);
        }
        if (a.werror && (opens || !R.shorts.empty())) return 1;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "mrcx: error: " << e.what() << "\n";
        return 2;
    }
}
