// mpnr: place and route. Reads a gate-level Verilog netlist and LEF, makes a
// floorplan, places the cells, routes the nets, and writes DEF.
#include "pnr_route.hpp"
#include "pnr_gds.hpp"
#include "version.hpp"
#include <chrono>
#include <iostream>

using namespace pnr;

static void usage() {
  std::cout <<
    "usage: mpnr -lef FILE... NETLIST.v... -def OUT.def [options]\n"
    "  -lef FILE         technology and cell LEF (repeat for several files)\n"
    "  -top NAME         top module (default: the one nothing instantiates)\n"
    "  -floorplan FILE   DEF with the die, rows, placed ports and fixed cells to keep\n"
    "  -util F           cell area over core area for the automatic floorplan (0.6)\n"
    "  -aspect F         core height over width (1.0)\n"
    "  -margin UM        space between core and die edge (four rows)\n"
    "  -min_layer N      lowest routing layer used for wires, 1 = first (2)\n"
    "  -max_layer N      highest routing layer used (all)\n"
    "  -tiehi CELL/PIN   tie cell for constant 1 (default: found by name)\n"
    "  -tielo CELL/PIN   tie cell for constant 0\n"
    "  -no_fill          do not add filler cells\n"
    "  -no_straps        no vertical power straps over the rows' rails\n"
    "  -strap_layer N    routing layer of the straps (default: the top vertical layer)\n"
    "  -strap_width UM   strap width (four wire widths)\n"
    "  -strap_pitch UM   distance between straps of one supply (32 row heights)\n"
    "  -place_only       stop after placement\n"
    "  -def FILE         write the placed and routed design as DEF\n"
    "  -netlist FILE     write the final flat netlist (with tie and filler cells)\n"
    "  -gds FILE         write the layout as GDSII (cells are abstracts made from the LEF)\n"
    "  -gds_map FILE     GDS layers: lines of LEFLAYER GDSLAYER [DATATYPE] (default: LEF order from 1)\n"
    "  -gds_cells FILE   GDS with the cells' real layouts, copied into the output\n"
    "  -svg FILE         write a picture of the layout\n"
    "  -report FILE      write the summary as JSON\n"
    "  -check FILE.def   only check a routed DEF: opens, shorts, spacing (with -gds: also convert it)\n"
    "  -threads N        worker threads (default: all logical cores; results do not depend on N)\n"
    "  -v                progress on stderr\n"
    "  -version, -help\n";
}

int main(int argc, char** argv) {
  std::vector<std::string> lefs, netlists;
  std::string defOut, floorplanDef, svgOut, reportOut, netlistOut, checkDef, gdsOut;
  GdsOptions gopt;
  StrapOptions sopt;
  NetlistOptions nopt;
  PlaceOptions popt;
  RouteOptions ropt;
  int threads = 0;
  bool placeOnly = false, verbose = false;
  try {
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      auto val = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
        return argv[++i];
      };
      if (a == "-lef") lefs.push_back(val());
      else if (a == "-top") nopt.top = val();
      else if (a == "-floorplan") floorplanDef = val();
      else if (a == "-util") popt.util = atof(val().c_str());
      else if (a == "-aspect") popt.aspect = atof(val().c_str());
      else if (a == "-margin") popt.marginUm = atof(val().c_str());
      else if (a == "-min_layer") ropt.minLayer = atoi(val().c_str());
      else if (a == "-max_layer") ropt.maxLayer = atoi(val().c_str());
      else if (a == "-tiehi") nopt.tieHi = val();
      else if (a == "-tielo") nopt.tieLo = val();
      else if (a == "-no_fill") popt.fillers = false;
      else if (a == "-no_straps") sopt.on = false;
      else if (a == "-strap_layer") sopt.layer = atoi(val().c_str());
      else if (a == "-strap_width") sopt.widthUm = atof(val().c_str());
      else if (a == "-strap_pitch") sopt.pitchUm = atof(val().c_str());
      else if (a == "-place_only") placeOnly = true;
      else if (a == "-def" || a == "-o") defOut = val();
      else if (a == "-netlist") netlistOut = val();
      else if (a == "-svg") svgOut = val();
      else if (a == "-gds") gdsOut = val();
      else if (a == "-gds_map") gopt.mapFile = val();
      else if (a == "-gds_cells") gopt.cellsFile = val();
      else if (a == "-report") reportOut = val();
      else if (a == "-check") checkDef = val();
      else if (a == "-threads") threads = atoi(val().c_str());
      else if (a == "-v") verbose = true;
      else if (a == "-version" || a == "--version") { std::cout << "mpnr " << MINISIM_VERSION << " (" << MINISIM_GIT << ")" << "\n"; return 0; }
      else if (a == "-help" || a == "--help" || a == "-h") { usage(); return 0; }
      else if (a[0] == '-') throw std::runtime_error("unknown option " + a + " (see mpnr -help)");
      else netlists.push_back(a);
    }
    if (lefs.empty()) throw std::runtime_error("no LEF given (-lef FILE)");
    popt.verbose = ropt.verbose = verbose;
    par::Pool pool(threads);
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    auto secs = [&](clk::time_point a) { return std::chrono::duration<double>(clk::now() - a).count(); };

    DB db;
    for (auto& f : lefs) phys::readLef(f, db.tech);
    if (db.tech.routing().size() < 2) throw std::runtime_error("the LEF defines fewer than two routing layers");

    if (!checkDef.empty()) {
      phys::Design d;
      phys::readDef(checkDef, d);
      CheckResult c = checkDesign(db.tech, d, pool);
      printCheck(std::cout, c);
      if (!gdsOut.empty()) writeGds(db.tech, d, gdsOut, gopt);
      return c.clean() ? 0 : 1;
    }

    if (netlists.empty()) throw std::runtime_error("no netlist given");
    readNetlist(netlists, db, nopt);
    std::cout << "mpnr: " << db.top << ": " << db.insts.size() << " cells, " << db.nets.size() << " nets, " << db.ios.size()
              << " ports, " << pool.threads() << " threads\n";

    auto t1 = clk::now();
    if (!floorplanDef.empty()) loadFloorplan(db, floorplanDef, ropt.minLayer);
    else floorplan(db, popt, ropt.minLayer);
    double cellArea = 0;
    for (auto& in : db.insts) cellArea += db.um(in.m->w) * db.um(in.m->h);
    {
      Placer pl(db, pool, popt);
      pl.globalPlace();
    }
    int64_t gpLen = db.hpwl();
    if (verbose) fprintf(stderr, "  global placement %.2f s\n", secs(t1));
    auto tl = clk::now();
    Legalizer lg(db);
    double moved = lg.run();
    int64_t lgLen = db.hpwl();
    if (verbose) fprintf(stderr, "  legalization %.2f s\n", secs(tl));
    tl = clk::now();
    int swaps = refine(db);
    int64_t dpLen = db.hpwl();
    if (verbose) fprintf(stderr, "  refinement %.2f s\n", secs(tl));
    tl = clk::now();
    int fillers = popt.fillers ? addFillers(db) : 0;
    int straps = 0;
    std::string noStraps;
    if (makeRails(db) && sopt.on) straps = makeStraps(db, sopt, ropt.maxLayer, noStraps);
    if (verbose) fprintf(stderr, "  fillers %.2f s\n", secs(tl));
    double placeSecs = secs(t1);
    printf("floorplan: die %.2f x %.2f um, core %.2f x %.2f um, %zu rows, utilization %.1f%%\n", db.um(db.die.w()), db.um(db.die.h()),
           db.um(db.core.w()), db.um(db.core.h()), db.rows.size(), 100 * cellArea / (db.um(db.core.w()) * db.um(db.core.h())));
    printf("placement: wire length (half perimeter) %.1f um global, %.1f legal, %.1f refined (%d changes); mean move %.2f um; %d fillers; %.2f s\n",
           db.um(gpLen), db.um(lgLen), db.um(dpLen), swaps, db.um((int64_t)moved), fillers, placeSecs);
    if (straps) printf("power: %d straps over the rails\n", straps);
    else if (sopt.on && !noStraps.empty()) printf("power: no straps: %s\n", noStraps.c_str());

    RouteResult rr;
    double routeSecs = 0;
    Router router(db, pool, ropt);
    if (!placeOnly) {
      auto t2 = clk::now();
      rr = router.run();
      routeSecs = secs(t2);
      printf("routing: %d nets, wire length %.1f um, %d vias, %d iterations, %d nets unrouted; %.2f s\n", rr.nets, db.um(rr.wireLength), rr.vias,
             rr.iterations, rr.failed, routeSecs);
      for (auto& n : rr.failedNets) printf("  unrouted: %s\n", n.c_str());
      for (size_t l = 0; l < rr.layerLength.size(); l++)
        if (rr.layerLength[l]) printf("  %-8s %10.1f um\n", rr.layerName[l].c_str(), db.um(rr.layerLength[l]));
    }

    phys::Design out = router.toDef(!placeOnly);
    CheckResult chk;
    if (!placeOnly) {
      auto t3 = clk::now();
      chk = checkDesign(db.tech, out, pool);
      printCheck(std::cout, chk);
      if (verbose) fprintf(stderr, "  check %.2f s\n", secs(t3));
    }
    if (!defOut.empty()) phys::writeDef(defOut, out);
    if (!netlistOut.empty()) writeNetlist(db, netlistOut);
    if (!svgOut.empty()) writeSvg(db, out, svgOut);
    if (!gdsOut.empty()) writeGds(db.tech, out, gdsOut, gopt);
    if (!reportOut.empty()) {
      std::ofstream r(reportOut);
      r << "{\n  \"design\": \"" << db.top << "\",\n  \"cells\": " << db.insts.size() - fillers << ",\n  \"fillers\": " << fillers
        << ",\n  \"nets\": " << db.nets.size() << ",\n  \"die_um\": [" << db.um(db.die.w()) << ", " << db.um(db.die.h()) << "]"
        << ",\n  \"utilization\": " << cellArea / (db.um(db.core.w()) * db.um(db.core.h()))
        << ",\n  \"hpwl_um\": " << db.um(dpLen) << ",\n  \"routed_nets\": " << rr.nets << ",\n  \"wire_length_um\": " << db.um(rr.wireLength)
        << ",\n  \"vias\": " << rr.vias << ",\n  \"unrouted\": " << rr.failed << ",\n  \"opens\": " << chk.opens << ",\n  \"shorts\": " << chk.shorts
        << ",\n  \"spacing\": " << chk.spacing << ",\n  \"threads\": " << pool.threads() << ",\n  \"place_seconds\": " << placeSecs
        << ",\n  \"route_seconds\": " << routeSecs << "\n}\n";
    }
    printf("mpnr: done in %.2f s\n", secs(t0));
    return (placeOnly || (rr.failed == 0 && chk.clean())) ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "mpnr: error: " << e.what() << "\n";
    return 2;
  }
}
