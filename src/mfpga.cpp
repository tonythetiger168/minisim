// mfpga: FPGA synthesis. RTL (SystemVerilog, through the same elaborator as
// msynth) or an and-inverter graph (AIGER) in; K-input LUTs and flip-flops of
// a device family out, as a netlist for place and route: Yosys JSON
// (nextpnr --json) or EDIF.
//
//   mfpga design.sv -top core -family ice40 -o core.json
//   nextpnr-ice40 --hx8k --json core.json --asc core.asc
//
// The mapper is src/fpga.hpp, on the shared graph of src/aig.hpp.
#include "slang/ast/Compilation.h"
#include "slang/driver/Driver.h"

#include "aig_elab.hpp"
#include "fpga.hpp"

#include <chrono>
#include <iostream>

static void usage() {
  std::cout <<
    "usage: mfpga [options] file.sv... | design.aag | design.aig\n"
    "  -family F     generic (default), ice40, ecp5, xc7\n"
    "  -lut K        LUT inputs, 2..6 (default: 4, xc7: 6)\n"
    "  -o FILE       write Yosys/nextpnr JSON\n"
    "  -edif FILE    write EDIF 2 0 0\n"
    "  -verilog FILE write structural Verilog (for simulation with the family's cell models)\n"
    "  -top NAME     top module (AIGER: the module name to write, default top)\n"
    "  +define+M=V -D M=V   macros;  +incdir+DIR -I DIR   include directories\n"
    "  -G NAME=VALUE        top-level parameter\n"
    "  -clock NAME   name of the clock input added for AIGER latches (default: clk)\n"
    "  -cuts N       cuts kept per node, 2..16 (default: 8)\n"
    "  -threads N    worker threads (default: every logical core; the output does not depend on N)\n"
    "  -nocarry      adders and comparators as LUTs, not carry chains\n"
    "  -noce         clock enables and synchronous resets stay in the D logic\n"
    "  -verify       simulate the mapped netlist against the input on random patterns\n"
    "  -q            no report\n";
}

int main(int argc, char** argv) {
  std::string json, edif, vlog, top, clock = "clk", family = "generic";
  std::vector<std::string> files, slang_args;
  fpga::MapOptions mo;
  int K = 0;
  bool verify = false, quiet = false, carry = true, ce = true;
  try {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto val = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
        return argv[++i];
      };
      if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
      else if (a == "-family") family = val();
      else if (a == "-lut") K = std::stoi(val());
      else if (a == "-o" || a == "-json") json = val();
      else if (a == "-edif") edif = val();
      else if (a == "-verilog") vlog = val();
      else if (a == "-top") top = val();
      else if (a == "-clock") clock = val();
      else if (a == "-cuts") mo.cuts = std::stoi(val());
      else if (a == "-threads") mo.threads = std::stoi(val());
      else if (a == "-verify") verify = true;
      else if (a == "-nocarry") carry = false;
      else if (a == "-noce") ce = false;
      else if (a == "-q") quiet = true;
      else if (a == "-D" || a == "-I" || a == "-G") { slang_args.push_back(a); slang_args.push_back(val()); }
      else if (a.rfind("+define+", 0) == 0) slang_args.push_back("-D" + a.substr(8));
      else if (a.rfind("+incdir+", 0) == 0) { slang_args.push_back("-I"); slang_args.push_back(a.substr(8)); }
      else if (!a.empty() && a[0] == '-') throw std::runtime_error("unknown option " + a);
      else files.push_back(a);
    }
    if (files.empty()) { usage(); return 2; }
    auto ends = [](const std::string& s, const char* e) { size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; };
    const bool aiger = ends(files[0], ".aig") || ends(files[0], ".aag");
    if (aiger && files.size() > 1) throw std::runtime_error("more than one AIGER file");
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    fpga::Family fam = fpga::parse_family(family);
    mo.K = K ? K : fpga::family_lut_size(fam);
    if (fam != fpga::Family::Generic && mo.K > fpga::family_lut_size(fam))
      throw std::runtime_error(family + " has " + std::to_string(fpga::family_lut_size(fam)) + "-input LUTs");
    aig::Design des;
    std::vector<int> init;
    if (aiger) {
      fpga::AigerDesign ad = fpga::read_aiger_file(files[0], clock);
      des = std::move(ad.design);
      init = std::move(ad.init);
      des.top = top.empty() ? "top" : top;
    } else {
      slang::driver::Driver drv;
      drv.addStandardArgs();
      std::vector<std::string> a{"mfpga"};
      for (auto& f : files) a.push_back(f);
      if (!top.empty()) { a.push_back("--top"); a.push_back(top); }
      a.push_back("-DSYNTHESIS");
      for (auto& x : slang_args) a.push_back(x);
      a.push_back("-Wnone");
      std::vector<const char*> av;
      for (auto& x : a) av.push_back(x.c_str());
      if (!drv.parseCommandLine((int)av.size(), av.data()) || !drv.processOptions() || !drv.parseAllSources())
        throw std::runtime_error("cannot read the design");
      auto comp = drv.createCompilation();
      drv.reportCompilation(*comp, true);
      if (!drv.reportDiagnostics(true)) throw std::runtime_error("the design has errors");
      des = aig::elaborate(*comp);
      for (auto& w : des.warnings) std::cerr << "mfpga: warning: " << w << "\n";
    }
    size_t raw_ands = 0;
    for (auto& nd : des.g.nodes) raw_ands += nd.k == aig::NK::And;
    fpga::FrontOptions fo = fpga::family_front(fam);
    fo.carry = fo.carry && carry;
    fo.enable = fo.enable && ce;
    fo.sync_reset = fo.sync_reset && ce;
    fpga::Aig g = fpga::from_design(des, init, fpga::family_invert(fam, init), fo);
    auto t1 = clk::now();
    fpga::Mapped m = fpga::map_luts(g, mo);
    auto t2 = clk::now();
    fpga::Netlist n = fpga::build_netlist(g, m, fam);
    for (auto& w : n.warnings) std::cerr << "mfpga: warning: " << w << "\n";
    if (verify) {
      std::string err = fpga::check_netlist(g, n);
      if (!err.empty()) { std::cerr << "mfpga: verify FAILED: " << err << "\n"; return 1; }
    }
    auto put = [](const std::string& path, const std::string& text) {
      std::ofstream f(path, std::ios::binary);
      if (!f || !(f << text)) throw std::runtime_error("cannot write " + path);
    };
    if (!json.empty()) put(json, fpga::write_json(n));
    if (!edif.empty()) put(edif, fpga::write_edif(n));
    if (!vlog.empty()) put(vlog, fpga::write_verilog(n));
    auto t3 = clk::now();
    if (!quiet) {
      auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      size_t hist[fpga::kMaxK + 1] = {};
      uint32_t box_outs = 0;   // chain outputs are inputs of the graph, not of the design
      for (auto& bx : g.boxes) bx.for_each_out([&](uint32_t) { ++box_outs; });
      for (auto& l : m.luts) hist[l.n]++;
      printf("design   %s: %u inputs, %zu flops, %zu ANDs (%zu after cleanup)\n", g.top.c_str(),
             g.nin - box_outs, g.flops.size(), raw_ands, g.ands.size());
      printf("mapped   %s LUT%d: %zu LUTs, %zu flops, depth %u\n", family.c_str(), mo.K, n.luts, n.ffs, m.depth);
      if (n.carry_bits || n.ffs_en || n.ffs_srst)
        printf("         %zu carry chains (%zu bits), %zu flops with clock enable, %zu with synchronous reset\n",
               g.boxes.size(), n.carry_bits, n.ffs_en, n.ffs_srst);
      printf("by size ");
      for (int k = 1; k <= mo.K; ++k) printf(" LUT%d %zu", k, hist[k]);
      printf("\n");
      if (verify) printf("verify   passed\n");
      printf("time     read %.1f ms, map %.1f ms, write %.1f ms\n", ms(t0, t1), ms(t1, t2), ms(t2, t3));
    }
  } catch (const std::exception& e) {
    std::cerr << "mfpga: " << e.what() << "\n";
    return 2;
  }
  return 0;
}
