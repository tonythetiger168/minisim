// FPGA technology mapping: an and-inverter graph in, a netlist of LUTs and
// flip-flops out, written as nextpnr/Yosys JSON or EDIF.
//
//   Aig      the mapper's flat view of an aig::Design (src/aig.hpp, the graph
//            msynth's elaborator builds): inputs, flops, ANDs, in that order
//   map_luts priority-cut K-LUT mapping: a depth-optimal pass, then area
//            recovery (area flow, then exact local area) that keeps the depth
//   build    LUTs and flops as cells of a device family (generic, ice40,
//            ecp5, xc7)
//
// Cut enumeration runs level by level on the shared pool (src/parallel.hpp). A node's cuts depend
// only on lower levels, so the result is the same for every thread count.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "aig.hpp"
#include "parallel.hpp"

namespace fpga {

using Lit = uint32_t;
inline uint32_t lit_var(Lit l) { return l >> 1; }
inline bool lit_neg(Lit l) { return l & 1; }

struct Flop {
  Lit d = 0;
  Lit clk = 0;
  Lit arst = 0;          // asynchronous reset, active high; literal 0 = none
  bool negedge = false;
  bool rval = false;     // value loaded by arst
  int init = 2;          // power-up value: 0, 1, or 2 = don't care
  Lit en = 1;            // clock enable; literal 1 = always
  Lit srst = 0;          // synchronous reset, active high; literal 0 = none
  bool sval = false;     // value loaded by srst
  std::string name;
};

// An adder on the family's carry chain: {cout, sum} = a + b + cin. The sum
// and carry bits drive variables of the graph, which the LUT mapper sees as
// inputs; a and b are outputs of the mapped logic.
struct Box {
  std::vector<Lit> a, b;        // LSB first, same width
  bool mul = false;             // a multiplier: sum is the product, 2's complement operands where sa, sb
  bool sa = false, sb = false;
  bool cin = false;
  bool binv = false;            // b is an operand inverted (a - b): a chain that can invert inside takes b ^ 1
  std::vector<uint32_t> sum;    // variable driven by each sum bit, 0 = not used
  uint32_t cout = 0;            // variable driven by the carry out, 0 = not used

  template <class F> void for_each_out(F&& f) const {
    for (uint32_t v : sum) if (v) f(v);
    if (cout) f(cout);
  }
  template <class F> void for_each_in(F&& f) const {
    for (Lit l : a) f(l);
    for (Lit l : b) f(l);
  }
};

// A block RAM: one write port, one registered read port. The read data are
// input variables (they change on the read clock, like flop outputs); the
// pins are roots. An address holds 2^abits words.
struct Ram {
  std::string name;
  int width = 0, abits = 0;
  std::vector<Lit> waddr, wdata, raddr;   // LSB first
  Lit wen = 1, ren = 1, wclk = 0, rclk = 0;
  bool wneg = false, rneg = false;        // clocked on the falling edge
  std::vector<uint32_t> q;                // variable of each read data bit, 0 = not used

  template <class F> void for_each_in(F&& f) const {
    for (Lit l : waddr) f(l);
    for (Lit l : wdata) f(l);
    for (Lit l : raddr) f(l);
    f(wen); f(ren); f(wclk); f(rclk);
  }
};

struct Port {
  std::string name;
  bool output = false;
  std::vector<Lit> bits;  // LSB first; an input bit is a positive input literal
  int msb = 0, lsb = 0;   // declared range
};

struct Aig {
  uint32_t nin = 0;                       // variables 1..nin are inputs
  std::vector<Flop> flops;                // variable nin+1+i is flop i's Q
  std::vector<std::array<Lit, 2>> ands;   // variable first_and()+i, fanins are lower variables
  std::vector<Port> ports;
  std::vector<Box> boxes;
  std::vector<Ram> rams;
  std::string top = "top";

  uint32_t first_and() const { return 1 + nin + (uint32_t)flops.size(); }
  uint32_t nvars() const { return first_and() + (uint32_t)ands.size(); }
  bool is_and(uint32_t v) const { return v >= first_and(); }

  // Every literal the mapped logic has to deliver: outputs, flop pins and
  // (with_boxes) the operands of the carry chains.
  template <class F> void for_each_root(F&& f, bool with_boxes = true) const {
    for (auto& p : ports)
      if (p.output) for (Lit l : p.bits) f(l);
    for (auto& fl : flops) { f(fl.d); f(fl.clk); f(fl.arst); f(fl.en); f(fl.srst); }
    if (with_boxes)
      for (auto& b : boxes) b.for_each_in(f);
    for (auto& r : rams) r.for_each_in(f);
  }
};

// Levels with carry chains: inputs and flops are 0, an AND is one above its
// fanins, a box output is one above the box's operands. Returns false and
// the box when a box's operands depend on its own outputs.
inline bool box_levels(const Aig& g, std::vector<uint32_t>& level, int* bad_box = nullptr) {
  const uint32_t nv = g.nvars(), fa = g.first_and();
  level.assign(nv, 0);
  if (g.boxes.empty()) {
    for (uint32_t v = fa; v < nv; ++v)
      level[v] = 1 + std::max(level[lit_var(g.ands[v - fa][0])], level[lit_var(g.ands[v - fa][1])]);
    return true;
  }
  std::vector<int> box_of(fa, -1);
  for (size_t k = 0; k < g.boxes.size(); ++k) g.boxes[k].for_each_out([&](uint32_t v) { box_of[v] = (int)k; });
  std::vector<char> state(nv, 0);
  std::vector<uint32_t> stack;
  for (uint32_t root = 1; root < nv; ++root) {
    if (state[root] == 2) continue;
    stack.push_back(root);
    while (!stack.empty()) {
      uint32_t v = stack.back();
      if (state[v] == 2) { stack.pop_back(); continue; }
      uint32_t lv = 0;
      bool ready = true;
      auto dep = [&](uint32_t u) {
        if (state[u] == 2) { lv = std::max(lv, level[u]); return true; }
        if (state[u] == 1) return false;   // on the stack: a loop
        ready = false;
        stack.push_back(u);
        return true;
      };
      bool ok = true;
      if (v >= fa) ok = dep(lit_var(g.ands[v - fa][0])) && dep(lit_var(g.ands[v - fa][1]));
      else if (box_of[v] >= 0) g.boxes[box_of[v]].for_each_in([&](Lit l) { ok = ok && dep(lit_var(l)); });
      else { state[v] = 2; stack.pop_back(); continue; }
      if (!ok) {
        if (bad_box) {
          *bad_box = -1;
          for (size_t i = stack.size(); i-- > 0 && *bad_box < 0;)
            if (stack[i] < fa) *bad_box = box_of[stack[i]];
        }
        return false;
      }
      if (!ready) { state[v] = 1; continue; }
      level[v] = lv + 1;
      state[v] = 2;
      stack.pop_back();
    }
  }
  return true;
}

// ------------------------------------------------------------ from aig.hpp

// Inverts the flagged flops of a design in place (D, Q and reset value), so a
// flop that must power up 1 becomes one that powers up 0. Run aig::cleanup()
// afterwards: the edits leave the graph unhashed.
inline void invert_flops(aig::Design& d, const std::vector<char>& flag) {
  aig::Graph& g = d.g;
  auto fl = [&](Lit l) -> Lit {
    if (l == aig::NOLIT) return l;
    const aig::Node& n = g.nodes[aig::lnode(l)];
    return (n.k == aig::NK::Dff && (size_t)n.aux < flag.size() && flag[n.aux]) ? l ^ 1 : l;
  };
  for (auto& n : g.nodes) {
    if (n.k == aig::NK::And) { n.a = fl(n.a); n.b = fl(n.b); }
    else if (n.k == aig::NK::Wire) n.a = fl(n.a);
  }
  for (auto& p : d.ports)
    if (p.out) for (Lit& b : p.bits) b = fl(b);
  for (size_t i = 0; i < g.flops.size(); ++i) {
    aig::Flop& f = g.flops[i];
    f.d = fl(f.d); f.clk = fl(f.clk); f.arst = fl(f.arst);
    if (i < flag.size() && flag[i]) { f.d ^= 1; f.rval = !f.rval; }
  }
  g.strash.clear();
}

// What the front half pulls out of the logic before LUT mapping.
struct FrontOptions {
  bool carry = false;          // adders, subtractors and comparators onto carry chains
  int carry_min = 3;           // narrowest operator worth a chain
  bool enable = false;         // d = en ? x : q becomes the flop's clock enable
  bool sync_reset = false;     // d = rst ? v : x becomes the flop's synchronous reset
  bool ce_over_reset = false;  // the family's enable gates its synchronous reset (ice40)
  int sync_reset_min = 3;      // flops one reset signal must control to be used as a reset
  bool ram = false;            // memories with a registered read onto block RAM
  int ram_abits = 0;           // address bits of the deepest configuration of one block
  int ram_min_bits = 512;      // smaller memories stay flops
  bool dsp = false;            // multipliers onto DSP blocks
  int dsp_a = 0, dsp_b = 0;    // widest operands of one block, as it multiplies signed numbers
  bool dsp_unsigned = false;   // the block also multiplies unsigned numbers of that width
  bool dsp_split = true;       // a multiplier too wide for one block: several blocks and adders
  int dsp_min = 10;            // fewest operand bits (both together) worth a block; narrower stay LUTs
};

namespace detail {

// A cleaned design to the flat view. Ports and inputs whose name starts with
// \x01 are from_design's own: flop enables and resets, box operands and outputs.
inline Aig flatten(const aig::Design& d, const std::vector<int>& init, const std::vector<char>& invert,
                   const std::vector<char>& sval, std::map<int, Box> boxes,
                   std::vector<Ram> rams = {}) {
  const aig::Graph& g = d.g;
  Aig out;
  out.top = d.top;
  out.nin = (uint32_t)g.inputNames.size();
  out.flops.resize(g.flops.size());
  // cleanup() lays the graph out as inputs, flops, ANDs; node number = variable
  const uint32_t fa = out.first_and();
  for (uint32_t n = 1; n < g.size(); ++n) {
    const aig::Node& nd = g.nodes[n];
    bool ok = n <= out.nin ? nd.k == aig::NK::Input
            : n < fa ? nd.k == aig::NK::Dff && nd.aux == (int)(n - 1 - out.nin)
            : nd.k == aig::NK::And && aig::lnode(nd.a) < n && aig::lnode(nd.b) < n;
    if (!ok) throw std::runtime_error("fpga: graph is not in inputs, flops, ANDs order");
    if (n >= fa) out.ands.push_back({nd.a, nd.b});
  }
  for (size_t i = 0; i < g.flops.size(); ++i) {
    const aig::Flop& f = g.flops[i];
    Flop& o = out.flops[i];
    o.d = f.d; o.clk = f.clk; o.arst = f.arst == aig::NOLIT ? 0 : f.arst;
    o.negedge = f.negedge; o.rval = f.rval; o.name = f.name;
    o.init = i < init.size() ? init[i] : 2;
    if (i < invert.size() && invert[i] && o.init != 2) o.init ^= 1;
  }
  for (size_t i = 0; i < g.inputNames.size(); ++i) {
    const std::string& nm = g.inputNames[i];
    if (nm.size() < 3 || nm[0] != '\x01') continue;
    size_t pos = 2;
    int k = std::stoi(nm.substr(2), &pos);
    if (nm[1] == 'm') { rams[k].q[std::stoi(nm.substr(3 + pos))] = 1 + (uint32_t)i; continue; }
    Box& b = boxes[k];
    if (nm[2 + pos] == 'c') b.cout = 1 + (uint32_t)i;
    else b.sum[std::stoi(nm.substr(3 + pos))] = 1 + (uint32_t)i;
  }
  for (auto& p : d.ports) {
    if (p.name.empty() || p.name[0] != '\x01') {
      out.ports.push_back(Port{p.name, p.out, p.bits, p.msb, p.lsb});
      continue;
    }
    size_t pos = 0;
    int k = std::stoi(p.name.substr(2), &pos);
    if (p.name[1] == 'm') {
      Ram& r = rams[k];
      switch (p.name[2 + pos]) {
        case 'a': r.waddr = p.bits; break;
        case 'd': r.wdata = p.bits; break;
        case 'r': r.raddr = p.bits; break;
        case 'w': r.wen = p.bits[0]; r.wclk = p.bits[1]; break;
        default: r.ren = p.bits[0]; r.rclk = p.bits[1]; break;
      }
    }
    else if (p.name[1] == 'e') out.flops[k].en = p.bits[0];
    else if (p.name[1] == 'r') { out.flops[k].srst = p.bits[0]; out.flops[k].sval = sval[k]; }
    else if (p.name[2 + pos] == 'a') boxes[k].a = p.bits;
    else boxes[k].b = p.bits;
  }
  for (auto& kv : boxes) out.boxes.push_back(std::move(kv.second));
  out.rams = std::move(rams);
  return out;
}

// x = s ? t : e, as aig::Graph::mux builds it: !(!(s & t) & !(!s & e)).
inline bool is_mux(const aig::Graph& g, Lit x, Lit& s, Lit& t, Lit& e) {
  if (!aig::lisneg(x)) return false;
  const aig::Node& o = g.at(x);
  if (o.k != aig::NK::And || !aig::lisneg(o.a) || !aig::lisneg(o.b)) return false;
  const aig::Node p = g.at(o.a), q = g.at(o.b);
  if (p.k != aig::NK::And || q.k != aig::NK::And) return false;
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j) {
      Lit ps = i ? p.b : p.a, pt = i ? p.a : p.b, qs = j ? q.b : q.a, qe = j ? q.a : q.b;
      if (ps == aig::lneg(qs)) { s = ps; t = pt; e = qe; return true; }
    }
  return false;
}

// x = en ? data : q, through nested muxes: "if (load) q <= d; else if (inc)
// q <= q + 1" holds unless load | inc. The enable logic is new nodes in g,
// shared by the bits of a register. False when no path of x holds q.
inline bool hold_split(aig::Graph& g, Lit x, Lit q, int depth, Lit& en, Lit& data) {
  if (x == q) { en = aig::L0; data = aig::L0; return true; }
  Lit s, t, e;
  if (depth == 0 || !is_mux(g, x, s, t, e)) return false;
  // q ^ c is the mux c ? !q : q; a toggle is logic (often an adder's sum bit), not a hold
  if (t == aig::lneg(q) || e == aig::lneg(q)) return false;
  Lit et = aig::L1, dt = t, ee = aig::L1, de = e;
  const bool ht = hold_split(g, t, q, depth - 1, et, dt);
  const bool he = hold_split(g, e, q, depth - 1, ee, de);
  if (!ht && !he) return false;
  en = g.mux(s, et, ee);
  data = et == aig::L0 ? de : ee == aig::L0 ? dt : g.mux(s, dt, de);
  return true;
}

}  // namespace detail

// The flat view of a design. init[i] is flop i's power-up value (0, 1,
// 2 = don't care); invert[i] maps the flop inverted (see invert_flops).
// With FrontOptions, clock enables and synchronous resets move from the D
// logic onto flop pins, and the word operators the elaborator recorded
// (aig::Design::ops) are cut out of the graph and become carry-chain boxes.
inline Aig from_design(const aig::Design& in, std::vector<int> init = {}, const std::vector<char>& invert = {},
                       const FrontOptions& fo = {}) {
  aig::Design work;
  const bool inv = std::find(invert.begin(), invert.end(), 1) != invert.end();
  if (inv) { work = in; invert_flops(work, invert); }
  aig::Design d = aig::cleanup(inv ? work : in);

  // ---- block RAM
  // The elaborator leaves a memory in the graph as flops and mux trees and
  // lists its ports (aig::Design::mems). A memory with one whole-word write
  // port, whose every read is registered (q <= mem[addr], with or without an
  // enable), leaves the graph: the read register becomes the RAM's own
  // output register, and the memory's flops, with nothing left to read them,
  // are removed. A read in the same cycle as a write to the same address
  // gives the old word in RTL and anything in the block, so with one clock
  // the write is delayed a cycle and a bypass register returns the word that
  // is still on its way.
  std::vector<Ram> rams;
  bool have_init = inv;
  for (int v : init) have_init |= v != 2;
  if (fo.ram && !have_init && !d.mems.empty()) {
    aig::Graph& g = d.g;
    std::vector<uint32_t> ref(g.size(), 0);
    for (uint32_t n = 1; n < g.size(); ++n) {
      const aig::Node& nd = g.nodes[n];
      if (nd.k == aig::NK::And) { ref[aig::lnode(nd.a)]++; ref[aig::lnode(nd.b)]++; }
      else if (nd.k == aig::NK::Wire && nd.a != aig::NOLIT) ref[aig::lnode(nd.a)]++;
    }
    for (auto& f : g.flops) {
      ref[aig::lnode(f.d)]++; ref[aig::lnode(f.clk)]++;
      if (f.arst != aig::NOLIT) ref[aig::lnode(f.arst)]++;
    }
    for (auto& pt : d.ports) if (pt.out) for (Lit l : pt.bits) ref[aig::lnode(l)]++;
    // which flop registers a literal, and under which enable
    struct Reader { int flop; Lit en; };
    std::unordered_map<Lit, Reader> reader;
    for (size_t i = 0; i < g.flops.size(); ++i) {
      const aig::Flop& f = g.flops[i];
      reader[f.d] = {(int)i, aig::L1};
      Lit sl, t, e;
      if (!detail::is_mux(g, f.d, sl, t, e)) continue;
      if (e == f.q) reader.emplace(t, Reader{(int)i, sl});
      else if (t == f.q) reader.emplace(e, Reader{(int)i, aig::lneg(sl)});
    }
    bool changed = false;
    const size_t nflops = g.flops.size();
    for (const aig::Memory& m : d.mems) {
      if (m.wr.size() != 1 || m.rd.empty() || m.depth < 2) continue;
      const aig::MemPort& w = m.wr[0];
      if (w.lo != 0 || (int)w.data.size() != m.width) continue;
      int abits = 1;
      while ((1 << abits) < m.depth) ++abits;
      if (abits > fo.ram_abits || (long)m.width * m.depth < fo.ram_min_bits) continue;
      // An address is the word position; bits above abits have to be 0 wires.
      auto address = [&](aig::Bits a, bool& ok) {
        while ((int)a.size() > abits && a.back() == aig::L0) a.pop_back();
        ok = ok && (int)a.size() <= abits;
        a.resize(abits, aig::L0);
        return a;
      };
      bool ok = true;
      const aig::Bits wa = address(w.addr, ok);
      struct Read { aig::Bits addr; std::vector<int> flops; Lit en; Lit clk; bool neg; };
      std::vector<Read> reads;
      for (const aig::MemPort& r : m.rd) {
        Read rd;
        rd.addr = address(r.addr, ok);
        ok = ok && (int)r.data.size() == m.width;
        for (size_t i = 0; ok && i < r.data.size(); ++i) {
          auto it = reader.find(r.data[i]);
          ok = it != reader.end() && ref[aig::lnode(r.data[i])] == 1 && it->second.flop < (int)nflops;
          if (!ok) break;
          const aig::Flop& f = g.flops[it->second.flop];
          ok = f.arst == aig::NOLIT && f.var != m.name;
          if (i == 0) { rd.en = it->second.en; rd.clk = f.clk; rd.neg = f.negedge; }
          ok = ok && it->second.en == rd.en && f.clk == rd.clk && f.negedge == rd.neg;
          rd.flops.push_back(it->second.flop);
        }
        if (!ok) break;
        reads.push_back(std::move(rd));
      }
      if (!ok) continue;
      // Rewire, then see that nothing else reads the memory's flops.
      std::vector<std::pair<uint32_t, aig::Node>> undo;
      const size_t inputs0 = g.inputNames.size(), nodes0 = g.nodes.size(), flops0 = g.flops.size(), ports0 = d.ports.size();
      const size_t rams0 = rams.size();
      auto set_wire = [&](uint32_t node, Lit to) {
        undo.push_back({node, g.nodes[node]});
        aig::Node wr;
        wr.k = aig::NK::Wire;
        wr.a = to;
        g.nodes[node] = wr;
      };
      auto reg = [&](Lit dl, Lit en, const std::string& name) {   // q <= en ? dl : q
        aig::Flop f;
        f.clk = w.clk; f.negedge = w.negedge; f.name = name;
        const Lit q = g.dff(f);
        g.flops.back().d = en == aig::L1 ? dl : g.mux(en, dl, q);
        return q;
      };
      auto pseudo = [&](size_t k, char tag, aig::Bits bits) {
        aig::Port pt;
        pt.name = std::string("\x01") + "m" + std::to_string(k) + tag;
        pt.out = true;
        pt.bits = std::move(bits);
        d.ports.push_back(std::move(pt));
      };
      for (size_t ri = 0; ri < reads.size(); ++ri) {
        const Read& rd = reads[ri];
        const size_t k = rams.size();
        Ram ram;
        ram.name = m.name + (reads.size() > 1 ? "_r" + std::to_string(ri) : "");
        ram.width = m.width; ram.abits = abits;
        ram.wneg = w.negedge; ram.rneg = rd.neg;
        ram.q.assign(m.width, 0);
        rams.push_back(std::move(ram));
        const bool same_clk = rd.clk == w.clk && rd.neg == w.negedge;
        aig::Bits a = wa, dat = w.data;
        Lit en = w.en, hit = aig::L0;
        aig::Bits by;
        if (same_clk) {
          const std::string base = m.name + "_ram" + std::to_string(k);
          for (size_t i = 0; i < a.size(); ++i) a[i] = reg(a[i], aig::L1, base + "_wa[" + std::to_string(i) + "]");
          for (size_t i = 0; i < dat.size(); ++i) dat[i] = reg(dat[i], aig::L1, base + "_wd[" + std::to_string(i) + "]");
          en = reg(en, aig::L1, base + "_we");
          hit = reg(g.andl(en, g.eq(a, rd.addr)), rd.en, base + "_hit");
          for (size_t i = 0; i < dat.size(); ++i) by.push_back(reg(dat[i], rd.en, base + "_by[" + std::to_string(i) + "]"));
        }
        pseudo(k, 'a', a);
        pseudo(k, 'd', dat);
        pseudo(k, 'r', rd.addr);
        pseudo(k, 'w', {en, w.clk});
        pseudo(k, 'n', {rd.en, rd.clk});
        for (int i = 0; i < m.width; ++i) {
          Lit q = g.input(std::string("\x01") + "m" + std::to_string(k) + "q" + std::to_string(i));
          if (same_clk) q = g.mux(hit, by[i], q);
          set_wire(aig::lnode(g.flops[rd.flops[i]].q), q);
        }
      }
      bool clean = true;
      {
        const std::vector<uint8_t> live = aig::liveNodes(d);
        for (uint32_t n = 1; n < nodes0 && clean; ++n)
          if (g.nodes[n].k == aig::NK::Dff && live[n] && g.flops[g.nodes[n].aux].var == m.name) clean = false;
      }
      if (!clean) {   // something else reads the words: put everything back
        for (auto it = undo.rbegin(); it != undo.rend(); ++it) g.nodes[it->first] = it->second;
        g.nodes.resize(nodes0);
        g.flops.resize(flops0);
        g.inputNames.resize(inputs0);
        d.ports.resize(ports0);
        rams.resize(rams0);
        g.strash.clear();
        continue;
      }
      for (uint32_t n = 1; n < nodes0; ++n)
        if (g.nodes[n].k == aig::NK::Dff && g.flops[g.nodes[n].aux].var == m.name) {
          aig::Node gone;
          gone.k = aig::NK::Wire;
          gone.a = aig::NOLIT;
          g.nodes[n] = gone;
        }
      changed = true;
    }
    if (changed) {
      g.strash.clear();
      d = aig::cleanup(d);
      init.clear();
    }
  }

  // ---- clock enables and synchronous resets
  std::vector<char> sval(d.g.flops.size(), 0);
  if (fo.enable || fo.sync_reset) {
    struct Pins { Lit en = aig::L1, rst = aig::L0, data = 0; bool val = false; };
    aig::Graph& g = d.g;
    auto peel = [&](size_t i, bool use_rst) {
      const aig::Flop& f = g.flops[i];
      Pins o;
      o.data = f.d;
      int iv = i < init.size() ? init[i] : 2;
      if (iv != 2 && i < invert.size() && invert[i]) iv ^= 1;
      auto try_en = [&] {
        if (!fo.enable) return;
        Lit en, data;
        if (detail::hold_split(g, o.data, f.q, 4, en, data) && en != aig::L1) { o.en = en; o.data = data; }
      };
      auto try_rst = [&] {
        if (!use_rst || !fo.sync_reset || f.arst != aig::NOLIT) return;
        Lit r, data;
        bool v;
        if (!aig::flopSyncReset(g, o.data, r, v, data)) return;
        if (iv != 2 && (iv == 1) != v) return;   // one value serves power-up and reset
        o.rst = r; o.val = v; o.data = data;
      };
      // d = rst ? v : (en ? data : q), or with the enable outermost. The
      // pins have one of the two priorities; the other is a gate away:
      // reset first is en | rst under an enable that gates the reset, and
      // enable first is rst & en under a reset that ignores the enable.
      try_rst();
      const bool rst_first = o.rst != aig::L0;
      try_en();
      if (!rst_first) try_rst();
      if (o.rst != aig::L0 && o.en != aig::L1) {
        if (fo.ce_over_reset && rst_first) o.en = g.orl(o.en, o.rst);
        if (!fo.ce_over_reset && !rst_first) o.rst = g.andl(o.rst, o.en);
      }
      return o;
    };
    std::vector<Pins> pins(g.flops.size());
    std::map<Lit, int> users;
    for (size_t i = 0; i < pins.size(); ++i) {
      pins[i] = peel(i, true);
      if (pins[i].rst != aig::L0) users[pins[i].rst]++;
    }
    // A reset signal has to be shared: any d = x & y looks like a reset of one flop.
    for (size_t i = 0; i < pins.size(); ++i)
      if (pins[i].rst != aig::L0 && users[pins[i].rst] < fo.sync_reset_min) pins[i] = peel(i, false);
    bool any = false;
    for (size_t i = 0; i < pins.size(); ++i) {
      const Pins& o = pins[i];
      if (o.en == aig::L1 && o.rst == aig::L0) continue;
      any = true;
      d.g.flops[i].d = o.data;
      sval[i] = o.val;
      auto pseudo = [&](char tag, Lit l) {
        aig::Port p;
        p.name = std::string("\x01") + tag + std::to_string(i);
        p.out = true;
        p.bits = {l};
        d.ports.push_back(std::move(p));
      };
      if (o.en != aig::L1) pseudo('e', o.en);
      if (o.rst != aig::L0) pseudo('r', o.rst);
    }
    if (any) d = aig::cleanup(d);
  }

  // ---- multipliers
  // A multiplier fits a DSP block by the width its operands really have:
  // the elaborator extends both to the width of the result first.
  struct MulFit { aig::Bits a, b; bool sa = false, sb = false; };
  auto mul_operands = [](const aig::WordOp& op) {
    MulFit m;
    auto trim = [&](aig::Bits v, bool& sg) {
      sg = op.sgn;
      if (sg) while (v.size() > 1 && v.back() == v[v.size() - 2]) v.pop_back();
      else while (v.size() > 1 && v.back() == aig::L0) v.pop_back();
      if (sg && v.size() > 1 && v.back() == aig::L0) { v.pop_back(); sg = false; }   // never negative
      return v;
    };
    m.a = trim(op.a, m.sa);
    m.b = trim(op.b, m.sb);
    if (m.a.size() < m.b.size()) { std::swap(m.a, m.b); std::swap(m.sa, m.sb); }
    return m;
  };
  // Pads m for the block and says whether one block takes it.
  auto mul_fits = [&](MulFit& m) {
    // an unsigned operand on a block that only does signed, or beside a signed one, gets a 0 sign bit
    if (!fo.dsp_unsigned || m.sa != m.sb)
      for (int side = 0; side < 2; ++side) {
        aig::Bits& v = side ? m.b : m.a;
        bool& sg = side ? m.sb : m.sa;
        if (!sg) { v.push_back(aig::L0); sg = true; }
      }
    if (m.a.size() < m.b.size()) { std::swap(m.a, m.b); std::swap(m.sa, m.sb); }
    return (int)m.a.size() <= fo.dsp_a && (int)m.b.size() <= fo.dsp_b;
  };
  // A multiplier too wide for one block is rebuilt as block-sized partial
  // products and adders, both recorded as operators: the products become DSP
  // blocks below and the adders carry chains. Only the top piece of a signed
  // operand is signed; a result bit is the sum of the shifted, extended products.
  if (fo.dsp && fo.dsp_split) {
    const size_t nops = d.ops.size();
    std::vector<char> drop(nops, 0);
    aig::Graph& g = d.g;
    const bool hashing = g.hashing;
    g.hashing = false;   // new nodes only: a node of the old multiplier must not end up inside its replacement
    for (size_t k = 0; k < nops; ++k) {
      if (d.ops[k].kind != aig::WordOp::Mul) continue;
      const aig::WordOp op = d.ops[k];
      MulFit m = mul_operands(op);
      {
        MulFit one = m;
        if (mul_fits(one)) continue;
      }
      if (m.b.size() < 2) continue;
      const size_t n = std::min(op.y.size(), m.a.size() + m.b.size());
      const bool sgn = m.sa || m.sb;
      const bool umode = fo.dsp_unsigned && !sgn;
      struct Piece { aig::Bits bits; bool sg; size_t off; };
      auto pieces = [&](const aig::Bits& v, bool sg, int cap) {
        const size_t c = umode ? cap : cap - 1;   // unsigned bits one block input takes
        std::vector<Piece> r;
        size_t off = 0;
        while (v.size() - off > (sg ? (size_t)cap : c)) {
          r.push_back({aig::Bits(v.begin() + off, v.begin() + off + c), false, off});
          off += c;
        }
        r.push_back({aig::Bits(v.begin() + off, v.end()), sg, off});
        return r;
      };
      const auto pa = pieces(m.a, m.sa, fo.dsp_a), pb = pieces(m.b, m.sb, fo.dsp_b);
      aig::Bits acc(n, aig::L0);
      bool first = true;
      for (auto& x : pa)
        for (auto& y : pb) {
          const size_t s = x.off + y.off;
          if (s >= n) continue;
          const int w = (int)(x.bits.size() + y.bits.size());
          const aig::Bits ex = aig::Graph::extend(x.bits, w, x.sg), ey = aig::Graph::extend(y.bits, w, y.sg);
          aig::Bits pr = g.mul(ex, ey);
          pr.resize(std::min((size_t)w, n - s));   // only the bits the result reads
          d.ops.push_back(aig::WordOp{aig::WordOp::Mul, ex, ey, pr, true});
          const aig::Bits pe = aig::Graph::extend(pr, (int)(n - s), x.sg || y.sg);
          if (first) {
            std::copy(pe.begin(), pe.end(), acc.begin() + s);
            first = false;
            continue;
          }
          const aig::Bits hi(acc.begin() + s, acc.end());
          const aig::Bits sum = g.add(hi, pe, aig::L0);
          d.ops.push_back(aig::WordOp{aig::WordOp::Add, hi, pe, sum, false});
          std::copy(sum.begin(), sum.end(), acc.begin() + s);
        }
      for (size_t i = 0; i < op.y.size(); ++i) {
        const uint32_t node = aig::lnode(op.y[i]);
        if (g.nodes[node].k != aig::NK::And) continue;
        const Lit to = i < n ? acc[i] : sgn ? acc[n - 1] : aig::L0;
        aig::Node wr;
        wr.k = aig::NK::Wire;
        wr.a = to ^ (Lit)aig::lisneg(op.y[i]);
        g.nodes[node] = wr;
      }
      drop[k] = 1;
    }
    g.hashing = hashing;
    if (std::find(drop.begin(), drop.end(), 1) != drop.end()) {
      std::vector<aig::WordOp> keep;
      for (size_t k = 0; k < d.ops.size(); ++k)
        if (k >= nops || !drop[k]) keep.push_back(std::move(d.ops[k]));
      d.ops = std::move(keep);
      // The operator table keeps an operator only with all of its bits alive.
      // A result bit of the old multiplier that was a constant (bit 1 of a
      // square) leaves its replacement unread: such bits leave the new entries.
      std::vector<char> live(g.size(), 0);
      std::vector<uint32_t> stack;
      auto reach = [&](Lit l) {
        if (l == aig::NOLIT) return;
        const uint32_t v = aig::lnode(l);
        if (v < live.size() && !live[v]) { live[v] = 1; stack.push_back(v); }
      };
      for (auto& pt : d.ports) if (pt.out) for (Lit l : pt.bits) reach(l);
      for (auto& fl : g.flops) { reach(fl.d); reach(fl.clk); reach(fl.arst); }
      while (!stack.empty()) {
        const aig::Node& nd = g.nodes[stack.back()];
        stack.pop_back();
        if (nd.k == aig::NK::And) { reach(nd.a); reach(nd.b); }
        else if (nd.k == aig::NK::Wire) reach(nd.a);
      }
      for (size_t k = nops - (size_t)std::count(drop.begin(), drop.end(), 1); k < d.ops.size(); ++k)
        for (Lit& l : d.ops[k].y)
          if (!live[aig::lnode(l)]) l = aig::L0;
      g.strash.clear();
      d = aig::cleanup(d);
    }
  }

  // ---- carry chains
  std::vector<char> sel(d.ops.size(), 0);
  std::map<int, MulFit> fits;
  if (fo.carry)
    for (size_t k = 0; k < d.ops.size(); ++k) {
      const aig::WordOp& op = d.ops[k];
      sel[k] = (op.kind == aig::WordOp::Add || op.kind == aig::WordOp::Sub || op.kind == aig::WordOp::Lt) &&
               op.a.size() == op.b.size() && (int)op.a.size() >= fo.carry_min;
    }
  if (fo.dsp)
    for (size_t k = 0; k < d.ops.size(); ++k) {
      if (d.ops[k].kind != aig::WordOp::Mul) continue;
      MulFit m = mul_operands(d.ops[k]);
      if (!mul_fits(m) || (int)(m.a.size() + m.b.size()) < fo.dsp_min || m.b.size() < 2) continue;
      sel[k] = 1;
      fits[(int)k] = std::move(m);
    }
  for (int round = 0;; ++round) {
    if (round == 16) std::fill(sel.begin(), sel.end(), 0);   // give up on chains rather than loop
    if (std::find(sel.begin(), sel.end(), 1) == sel.end()) return detail::flatten(d, init, invert, sval, {}, rams);
    aig::Design c = d;
    c.g.strash.clear();
    std::vector<char> cut(c.g.size(), 0);
    std::map<int, Box> protos;
    for (size_t k = 0; k < d.ops.size(); ++k) {
      if (!sel[k]) continue;
      const aig::WordOp& op = d.ops[k];
      Box bx;
      aig::Bits a = op.a, b = op.b;
      const bool lt = op.kind == aig::WordOp::Lt;
      const bool mul = op.kind == aig::WordOp::Mul;
      if (mul) {
        const MulFit& m = fits[(int)k];
        a = m.a; b = m.b;
        bx.mul = true; bx.sa = m.sa; bx.sb = m.sb;
      } else if (op.kind != aig::WordOp::Add) {      // a - b = a + ~b + 1; a < b is its missing carry
        for (Lit& l : b) l ^= 1;
        bx.cin = bx.binv = true;
      }
      if (lt && op.sgn) { a.back() ^= 1; b.back() ^= 1; }   // signed: compare with the sign bits flipped
      bx.sum.assign(mul ? std::min(op.y.size(), a.size() + b.size()) : a.size(), 0);
      const std::string base = std::string("\x01") + "b" + std::to_string(k);
      bool any = false;
      // The node behind y becomes a wire from a new input the box will drive.
      auto take = [&](Lit y, const std::string& name, bool inverted) {
        uint32_t n = aig::lnode(y);
        if (n >= cut.size() || cut[n] || c.g.nodes[n].k != aig::NK::And) return;
        Lit x = c.g.input(name);
        aig::Node w;
        w.k = aig::NK::Wire;
        w.a = x ^ (Lit)(aig::lisneg(y) != inverted);
        c.g.nodes[n] = w;
        cut[n] = 1;
        any = true;
      };
      if (lt) take(op.y[0], base + "c", true);   // carry out = !(a < b)
      else
        for (size_t i = 0; i < op.y.size() && i < bx.sum.size(); ++i) take(op.y[i], base + "s" + std::to_string(i), false);
      if (!any) { sel[k] = 0; continue; }
      // result bits past the product: its sign, or 0
      for (size_t i = bx.sum.size(); mul && i < op.y.size(); ++i) {
        uint32_t n = aig::lnode(op.y[i]), top = aig::lnode(op.y[bx.sum.size() - 1]);
        if (n >= cut.size() || cut[n] || c.g.nodes[n].k != aig::NK::And) continue;
        aig::Node w;
        w.k = aig::NK::Wire;
        w.a = bx.sa ? aig::mklit(top, aig::lisneg(op.y[i]) != aig::lisneg(op.y[bx.sum.size() - 1]))
                    : (aig::lisneg(op.y[i]) ? aig::L1 : aig::L0);
        if (bx.sa && !cut[top]) continue;
        c.g.nodes[n] = w;
        cut[n] = 1;
      }
      for (int side = 0; side < 2; ++side) {
        aig::Port p;
        p.name = base + (side ? "b" : "a");
        p.out = true;
        p.bits = side ? b : a;
        c.ports.push_back(std::move(p));
      }
      protos[(int)k] = std::move(bx);
    }
    if (protos.empty()) continue;
    std::vector<int> ids;
    for (auto& kv : protos) ids.push_back(kv.first);
    Aig flat = detail::flatten(aig::cleanup(c), init, invert, sval, std::move(protos), rams);
    // A box nothing reads (the adders inside a comparator that became a
    // chain itself) or one that feeds its own operands is dropped; try again.
    std::vector<char> used(flat.first_and(), 0);
    auto mark = [&](Lit l) { if (lit_var(l) < used.size()) used[lit_var(l)] = 1; };
    for (auto& nd : flat.ands) { mark(nd[0]); mark(nd[1]); }
    flat.for_each_root(mark);
    bool again = false;
    for (size_t j = 0; j < flat.boxes.size(); ++j) {
      Box& b = flat.boxes[j];
      for (uint32_t& v : b.sum) if (v && !used[v]) v = 0;
      if (b.cout && !used[b.cout]) b.cout = 0;
      bool live = b.cout != 0;
      for (uint32_t v : b.sum) live |= v != 0;
      if (!live) { sel[ids[j]] = 0; again = true; }
    }
    if (again) continue;
    std::vector<uint32_t> level;
    int bad = -1;
    if (!box_levels(flat, level, &bad)) {
      if (bad < 0) throw std::runtime_error("fpga: combinational loop");
      sel[ids[bad]] = 0;
      continue;
    }
    return flat;
  }
}

// --------------------------------------------------------------- LUT mapping

constexpr int kMaxK = 6;

struct Cut {
  uint32_t leaf[kMaxK];
  uint32_t sig;
  uint32_t delay;
  float flow;
  uint8_t n;
};

struct Lut {
  uint32_t root;           // variable this LUT computes (positive)
  uint8_t n;
  uint32_t leaf[kMaxK];    // input variables; input i is truth table variable i
  uint64_t tt;
};

struct Mapped {
  std::vector<Lut> luts;   // in topological order
  uint32_t depth = 0;      // LUT levels on the longest path
};

struct MapOptions {
  int K = 4;               // LUT inputs, 2..6
  int cuts = 8;            // cuts kept per node
  int threads = 0;         // 0 = all logical cores
  int flow_passes = 2;
  int exact_passes = 2;
};

namespace detail {

inline uint32_t cut_sig(uint32_t v) { return 1u << (v & 31); }

// Sorted union of two cuts; false if it has more than K leaves.
inline bool cut_merge(const Cut& a, const Cut& b, int K, Cut& o) {
  int i = 0, j = 0, n = 0;
  while (i < a.n || j < b.n) {
    if (n == K) return false;
    if (j == b.n || (i < a.n && a.leaf[i] < b.leaf[j])) o.leaf[n++] = a.leaf[i++];
    else if (i == a.n || b.leaf[j] < a.leaf[i]) o.leaf[n++] = b.leaf[j++];
    else { o.leaf[n++] = a.leaf[i]; ++i; ++j; }
  }
  o.n = (uint8_t)n;
  o.sig = a.sig | b.sig;
  return true;
}

inline bool cut_subset(const Cut& a, const Cut& b) {  // a's leaves all in b
  if (a.n > b.n || (a.sig & ~b.sig)) return false;
  int j = 0;
  for (int i = 0; i < a.n; ++i) {
    while (j < b.n && b.leaf[j] < a.leaf[i]) ++j;
    if (j == b.n || b.leaf[j] != a.leaf[i]) return false;
    ++j;
  }
  return true;
}

const uint64_t kVarTT[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
                            0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};

}  // namespace detail

class LutMapper {
 public:
  LutMapper(const Aig& g, MapOptions o) : g_(g), o_(o) {
    if (o_.K < 2 || o_.K > kMaxK) throw std::runtime_error("LUT size must be 2..6");
    if (o_.cuts < 2) o_.cuts = 2;
    if (o_.cuts > 16) o_.cuts = 16;
    if (o_.threads <= 0) o_.threads = (int)par::defaultThreads();
  }

  Mapped run() {
    par::Pool pool((unsigned)o_.threads);
    const uint32_t nv = g_.nvars(), fa = g_.first_and();
    const int C = o_.cuts;
    cuts_.resize((size_t)g_.ands.size() * C);
    ncuts_.assign(g_.ands.size(), 0);
    best_.resize(g_.ands.size());
    arr_.assign(nv, 0);
    flow_.assign(nv, 0.f);
    req_.assign(nv, UINT32_MAX);
    ref_.assign(nv, 0);
    est_.assign(nv, 1.f);

    // levels and fanout counts
    std::vector<uint32_t> level;
    if (!box_levels(g_, level)) throw std::runtime_error("fpga: a carry chain feeds its own operands");
    uint32_t maxl = 0;
    for (uint32_t v = fa; v < nv; ++v) {
      auto& a = g_.ands[v - fa];
      maxl = std::max(maxl, level[v]);
      ref_[lit_var(a[0])]++; ref_[lit_var(a[1])]++;
    }
    std::vector<uint32_t> box_level(g_.boxes.size(), 0);
    for (size_t k = 0; k < g_.boxes.size(); ++k) {
      g_.boxes[k].for_each_in([&](Lit l) { box_level[k] = std::max(box_level[k], level[lit_var(l)] + 1); });
      maxl = std::max(maxl, box_level[k]);
    }
    g_.for_each_root([&](Lit l) { ref_[lit_var(l)]++; });
    for (uint32_t v = 0; v < nv; ++v) est_[v] = (float)std::max<uint32_t>(1, ref_[v]);
    std::vector<uint32_t> lstart(maxl + 2, 0), order(g_.ands.size());
    for (uint32_t v = fa; v < nv; ++v) lstart[level[v] + 1]++;
    for (uint32_t l = 1; l <= maxl + 1; ++l) lstart[l] += lstart[l - 1];
    {
      std::vector<uint32_t> pos(lstart.begin(), lstart.end() - 1);
      for (uint32_t v = fa; v < nv; ++v) order[pos[level[v]]++] = v;
    }

    // topological order of ANDs and boxes: by level, a level's boxes first
    std::vector<std::vector<uint32_t>> boxes_at(maxl + 1);
    for (size_t k = 0; k < g_.boxes.size(); ++k) boxes_at[box_level[k]].push_back((uint32_t)k);
    topo_.clear();
    for (uint32_t l = 1; l <= maxl; ++l) {
      for (uint32_t k : boxes_at[l]) topo_.push_back(kBox | k);
      for (uint32_t i = lstart[l]; i < lstart[l + 1]; ++i) topo_.push_back(order[i]);
    }

    auto sweep = [&](int mode) {
      for (uint32_t l = 1; l <= maxl; ++l) {
        for (uint32_t k : boxes_at[l]) box_arrival(k);
        uint32_t b = lstart[l], n = lstart[l + 1] - b;
        pool.chunks(n, 64, [&](size_t lo, size_t hi, size_t) {
          for (size_t i = lo; i < hi; ++i) enumerate(order[b + i], mode);
        });
      }
    };
    sweep(0);
    uint32_t depth = cover();
    for (int p = 0; p < o_.flow_passes; ++p) {
      for (uint32_t v = fa; v < nv; ++v)
        est_[v] = std::max(1.f, (est_[v] + 2.f * ref_[v]) / 3.f);
      sweep(1);
      cover(depth);
    }
    for (int p = 0; p < o_.exact_passes; ++p) {
      for (uint32_t it : topo_) {
        if (it & kBox) box_arrival(it & ~kBox);
        else exact(it);
      }
      cover(depth);
    }
    return extract(pool);
  }

 private:
  static constexpr uint32_t kBox = 0x80000000u;   // topo_ entry: a box, not an AND
  bool is_and(uint32_t v) const { return g_.is_and(v); }
  // A carry chain costs one level from its latest operand to every output.
  void box_arrival(uint32_t k) {
    uint32_t d = 0;
    g_.boxes[k].for_each_in([&](Lit l) { d = std::max(d, arr_[lit_var(l)]); });
    g_.boxes[k].for_each_out([&](uint32_t v) { arr_[v] = d + 1; });
  }
  Cut trivial(uint32_t v) const {
    Cut c;
    c.leaf[0] = v; c.n = 1; c.sig = detail::cut_sig(v);
    c.delay = arr_[v]; c.flow = flow_[v];
    return c;
  }
  void eval(Cut& c) const {
    uint32_t d = 0;
    float f = 1.f;
    for (int i = 0; i < c.n; ++i) { d = std::max(d, arr_[c.leaf[i]]); f += flow_[c.leaf[i]]; }
    c.delay = d + 1;
    c.flow = f;
  }

  // mode 0: smallest depth; mode 1: smallest area flow within the required time.
  void enumerate(uint32_t v, int mode) {
    const uint32_t fa = g_.first_and();
    const int K = o_.K, C = o_.cuts;
    const uint32_t a = lit_var(g_.ands[v - fa][0]), b = lit_var(g_.ands[v - fa][1]);
    Cut cand[17 * 17 + 1];
    int nc = 0;
    auto add = [&](const Cut& c) {
      for (int i = 0; i < nc; ++i)
        if (detail::cut_subset(cand[i], c)) return;
      int w = 0;
      for (int i = 0; i < nc; ++i)
        if (!detail::cut_subset(c, cand[i])) cand[w++] = cand[i];
      nc = w;
      cand[nc++] = c;
    };
    Cut ta = trivial(a), tb = trivial(b), m;
    const Cut* ca = is_and(a) ? &cuts_[(size_t)(a - fa) * C] : nullptr;
    const Cut* cb = is_and(b) ? &cuts_[(size_t)(b - fa) * C] : nullptr;
    int na = ca ? ncuts_[a - fa] : 0, nb = cb ? ncuts_[b - fa] : 0;
    for (int i = -1; i < na; ++i) {
      const Cut& x = i < 0 ? ta : ca[i];
      for (int j = -1; j < nb; ++j) {
        const Cut& y = j < 0 ? tb : cb[j];
        if (__builtin_popcount(x.sig | y.sig) > K) continue;
        if (detail::cut_merge(x, y, K, m)) add(m);
      }
    }
    if (mode != 0) add(best_[v - fa]);
    for (int i = 0; i < nc; ++i) eval(cand[i]);
    const uint32_t req = req_[v];
    auto less = [&](const Cut& x, const Cut& y) {
      if (mode == 0) {
        if (x.delay != y.delay) return x.delay < y.delay;
        if (x.flow != y.flow) return x.flow < y.flow;
      } else {
        bool xo = x.delay <= req, yo = y.delay <= req;
        if (xo != yo) return xo;
        if (!xo && x.delay != y.delay) return x.delay < y.delay;
        if (x.flow != y.flow) return x.flow < y.flow;
        if (x.delay != y.delay) return x.delay < y.delay;
      }
      if (x.n != y.n) return x.n < y.n;
      return std::lexicographical_compare(x.leaf, x.leaf + x.n, y.leaf, y.leaf + y.n);
    };
    std::sort(cand, cand + nc, less);
    int keep = std::min(nc, C);
    std::copy(cand, cand + keep, &cuts_[(size_t)(v - fa) * C]);
    ncuts_[v - fa] = (uint8_t)keep;
    best_[v - fa] = cand[0];
    arr_[v] = cand[0].delay;
    flow_[v] = cand[0].flow / est_[v];
  }

  // Marks the LUTs the outputs need, counts references and sets required
  // times for depth D (0 = the depth this cover has). Returns the depth.
  uint32_t cover(uint32_t D = 0) {
    const uint32_t fa = g_.first_and(), nv = g_.nvars();
    std::fill(ref_.begin(), ref_.end(), 0);
    std::fill(req_.begin(), req_.end(), UINT32_MAX);
    uint32_t depth = 0;
    g_.for_each_root([&](Lit l) { depth = std::max(depth, arr_[lit_var(l)]); });
    if (D < depth) D = depth;
    g_.for_each_root([&](Lit l) { ref_[lit_var(l)]++; req_[lit_var(l)] = D; }, false);
    for (size_t t = topo_.size(); t-- > 0;) {
      const uint32_t v = topo_[t];
      if (v & kBox) {
        // operands are needed one level before the earliest required output
        const Box& b = g_.boxes[v & ~kBox];
        uint32_t r = D + 1;
        b.for_each_out([&](uint32_t o) { if (ref_[o]) r = std::min(r, req_[o]); });
        b.for_each_in([&](Lit l) {
          ref_[lit_var(l)]++;
          req_[lit_var(l)] = std::min(req_[lit_var(l)], r ? r - 1 : 0);
        });
        continue;
      }
      if (!ref_[v]) { req_[v] = UINT32_MAX; continue; }
      const Cut& c = best_[v - fa];
      for (int i = 0; i < c.n; ++i) {
        ref_[c.leaf[i]]++;
        req_[c.leaf[i]] = std::min(req_[c.leaf[i]], req_[v] - 1);
      }
    }
    return depth;
  }

  uint32_t cut_ref(const Cut& c) {
    uint32_t area = 1;
    for (int i = 0; i < c.n; ++i) {
      uint32_t u = c.leaf[i];
      if (ref_[u]++ == 0 && is_and(u)) area += cut_ref(best_[u - g_.first_and()]);
    }
    return area;
  }
  uint32_t cut_deref(const Cut& c) {
    uint32_t area = 1;
    for (int i = 0; i < c.n; ++i) {
      uint32_t u = c.leaf[i];
      if (--ref_[u] == 0 && is_and(u)) area += cut_deref(best_[u - g_.first_and()]);
    }
    return area;
  }

  // Exact local area: picks the stored cut that adds the fewest LUTs to the
  // current cover and still meets the required time.
  void exact(uint32_t v) {
    const uint32_t fa = g_.first_and();
    const int C = o_.cuts;
    const bool used = ref_[v] > 0;
    Cut cur = best_[v - fa];
    if (used) cut_deref(cur);
    eval(cur);
    Cut pick = cur;
    uint32_t pick_area = cut_ref(cur);
    cut_deref(cur);
    Cut* cs = &cuts_[(size_t)(v - fa) * C];
    for (int i = 0; i < ncuts_[v - fa]; ++i) {
      Cut c = cs[i];
      eval(c);
      if (c.delay > req_[v] && c.delay > pick.delay) continue;
      uint32_t area = cut_ref(c);
      cut_deref(c);
      bool pick_ok = pick.delay <= req_[v], c_ok = c.delay <= req_[v];
      bool better;
      if (c_ok != pick_ok) better = c_ok;
      else if (!c_ok) better = c.delay < pick.delay;
      else if (area != pick_area) better = area < pick_area;
      else if (c.delay != pick.delay) better = c.delay < pick.delay;
      else better = c.n < pick.n;
      if (better) { pick = c; pick_area = area; }
    }
    best_[v - fa] = pick;
    arr_[v] = pick.delay;
    if (used) cut_ref(pick);
  }

  // Truth tables of the chosen LUTs, inputs the function ignores removed.
  Mapped extract(par::Pool& pool) {
    const uint32_t fa = g_.first_and(), nv = g_.nvars();
    std::vector<uint32_t> roots;
    for (uint32_t v = fa; v < nv; ++v)
      if (ref_[v]) roots.push_back(v);
    std::vector<Lut> luts(roots.size());
    struct Scratch { std::vector<uint64_t> tt; std::vector<uint32_t> stamp, stack; uint32_t now = 0; };
    pool.chunks(roots.size(), 256, [&](size_t lo, size_t hi, size_t) {
     thread_local Scratch s;   // one per worker, reused across chunks
     if (s.tt.size() != nv) { s.tt.assign(nv, 0); s.stamp.assign(nv, 0); s.now = 0; }
     for (size_t i = lo; i < hi; ++i) {
      const uint32_t v = roots[i];
      const Cut& c = best_[v - fa];
      ++s.now;
      for (int k = 0; k < c.n; ++k) { s.tt[c.leaf[k]] = detail::kVarTT[k]; s.stamp[c.leaf[k]] = s.now; }
      s.stack.clear();
      s.stack.push_back(v);
      while (!s.stack.empty()) {
        uint32_t u = s.stack.back();
        if (s.stamp[u] == s.now) { s.stack.pop_back(); continue; }
        Lit l0 = g_.ands[u - fa][0], l1 = g_.ands[u - fa][1];
        uint32_t a = lit_var(l0), b = lit_var(l1);
        if (a == 0) { s.tt[0] = 0; s.stamp[0] = s.now; }
        if (b == 0) { s.tt[0] = 0; s.stamp[0] = s.now; }
        bool ra = s.stamp[a] == s.now, rb = s.stamp[b] == s.now;
        if (ra && rb) {
          s.tt[u] = (s.tt[a] ^ (lit_neg(l0) ? ~0ull : 0)) & (s.tt[b] ^ (lit_neg(l1) ? ~0ull : 0));
          s.stamp[u] = s.now;
          s.stack.pop_back();
        } else {
          if (!ra) s.stack.push_back(a);
          if (!rb) s.stack.push_back(b);
        }
      }
      Lut& L = luts[i];
      L.root = v;
      uint64_t t = s.tt[v];
      int n = 0;
      // keep only the inputs the function depends on, compacting the table
      for (int k = 0; k < c.n; ++k) {
        int sh = 1 << k;
        uint64_t hi = (t & detail::kVarTT[k]) >> sh, lo = t & ~detail::kVarTT[k];
        if (hi == lo) continue;
        L.leaf[n++] = c.leaf[k];
      }
      // rebuild the table over the kept inputs
      uint64_t r = 0;
      int kept[kMaxK], nk = 0;
      for (int k = 0; k < c.n; ++k) {
        int sh = 1 << k;
        if (((t & detail::kVarTT[k]) >> sh) != (t & ~detail::kVarTT[k])) kept[nk++] = k;
      }
      for (int mnt = 0; mnt < (1 << nk); ++mnt) {
        int full = 0;
        for (int k = 0; k < nk; ++k)
          if (mnt >> k & 1) full |= 1 << kept[k];
        if (t >> full & 1) r |= 1ull << mnt;
      }
      // replicate so bits above 2^n repeat the function
      for (int w = 1 << nk; w < 64; w <<= 1) r |= r << w;
      L.n = (uint8_t)n;
      L.tt = r;
     }
    });
    // liveness after input removal, and depth
    Mapped out;
    std::vector<char> live(nv, 0);
    g_.for_each_root([&](Lit l) { live[lit_var(l)] = 1; });
    for (size_t i = luts.size(); i-- > 0;)
      if (live[luts[i].root])
        for (int k = 0; k < luts[i].n; ++k) live[luts[i].leaf[k]] = 1;
    std::vector<uint32_t> lvl(nv, 0);
    std::vector<int> lut_at(nv, -1);
    for (size_t i = 0; i < luts.size(); ++i)
      if (live[luts[i].root]) { lut_at[luts[i].root] = (int)i; out.luts.push_back(luts[i]); }
    for (uint32_t it : topo_) {
      if (it & kBox) {
        uint32_t d = 0;
        g_.boxes[it & ~kBox].for_each_in([&](Lit l) { d = std::max(d, lvl[lit_var(l)]); });
        g_.boxes[it & ~kBox].for_each_out([&](uint32_t v) { lvl[v] = d + 1; out.depth = std::max(out.depth, d + 1); });
      } else if (lut_at[it] >= 0) {
        const Lut& L = luts[lut_at[it]];
        uint32_t d = 0;
        for (int k = 0; k < L.n; ++k) d = std::max(d, lvl[L.leaf[k]]);
        lvl[it] = L.n ? d + 1 : 0;
        out.depth = std::max(out.depth, lvl[it]);
      }
    }
    return out;
  }

  const Aig& g_;
  MapOptions o_;
  std::vector<Cut> cuts_, best_;
  std::vector<uint8_t> ncuts_;
  std::vector<uint32_t> arr_, req_, ref_, topo_;
  std::vector<float> flow_, est_;
};

inline Mapped map_luts(const Aig& g, const MapOptions& o = {}) { return LutMapper(g, o).run(); }

// ------------------------------------------------------------------ netlist

struct Conn {
  std::string port;
  bool output;
  std::vector<int> bits;   // 0 and 1 are the constants, nets start at 2
};
struct Param {
  std::string name, value;  // value: bits MSB first, or text when is_string
  bool is_string;
};
struct Cell {
  std::string name, type;
  std::vector<Param> params;
  std::vector<Conn> conns;
};
struct NetPort {
  std::string name;
  bool output;
  std::vector<int> bits;
  int msb = 0, lsb = 0;
};
struct Netlist {
  std::string top;
  std::vector<NetPort> ports;
  std::vector<Cell> cells;
  std::vector<std::pair<std::string, std::vector<int>>> netnames;
  std::vector<std::string> warnings;
  std::vector<std::pair<int, int>> inits;  // net, power-up value (generic family)
  int nbits = 2;
  size_t luts = 0, ffs = 0, ffs_en = 0, ffs_srst = 0, carry_bits = 0, chains = 0, dsps = 0, brams = 0;
  // where the Aig's signals ended up (for checking)
  std::vector<int> ci_bit;                 // per variable below first_and()
  std::vector<std::array<int, 5>> ff_bits; // D, clock, async reset, enable, sync reset pin nets per flop
  std::vector<std::vector<std::pair<int, Lit>>> ram_pins;  // the same per block RAM
  std::vector<std::vector<std::pair<int, Lit>>> box_pins;  // per box: operand pin nets and the literal each carries
};

enum class Family { Generic, Ice40, Ecp5, Xc7 };

inline Family parse_family(const std::string& s) {
  if (s == "generic") return Family::Generic;
  if (s == "ice40") return Family::Ice40;
  if (s == "ecp5") return Family::Ecp5;
  if (s == "xc7" || s == "xilinx") return Family::Xc7;
  throw std::runtime_error("unknown family '" + s + "' (generic, ice40, ecp5, xc7)");
}
inline int family_lut_size(Family f) { return f == Family::Xc7 ? 6 : 4; }
// What each family's cells offer: all have a clock enable and a synchronous
// reset; the generic cells have no carry.
inline FrontOptions family_front(Family f) {
  FrontOptions o;
  o.carry = f != Family::Generic;
  o.enable = o.sync_reset = true;
  o.ce_over_reset = f == Family::Ice40;
  // MULT18X18D and SB_MAC16 have a signed/unsigned switch per operand;
  // DSP48E1 multiplies 25 x 18 signed. iCE40 has DSPs on UltraPlus parts
  // only, so there the caller turns them on.
  o.dsp = f == Family::Ecp5 || f == Family::Xc7;
  o.dsp_a = f == Family::Xc7 ? 25 : f == Family::Ice40 ? 16 : 18;
  o.dsp_b = f == Family::Ice40 ? 16 : 18;
  o.dsp_unsigned = f != Family::Xc7;
  if (f == Family::Generic) o.dsp_a = o.dsp_b = 0;
  // SB_RAM40_4K goes to 2048 x 2, DP16KD and RAMB18E1 to 16384 x 1
  o.ram = f != Family::Generic;
  o.ram_abits = f == Family::Ice40 ? 11 : 14;
  return o;
}

// ice40 flops power up 0: a flop that must power up 1 is mapped inverted.
inline std::vector<char> family_invert(Family f, const std::vector<int>& init) {
  std::vector<char> inv(init.size(), 0);
  if (f == Family::Ice40)
    for (size_t i = 0; i < init.size(); ++i) inv[i] = init[i] == 1;
  return inv;
}

inline std::string bitstr(uint64_t v, int n) {
  std::string s(n, '0');
  for (int i = 0; i < n; ++i)
    if (v >> i & 1) s[n - 1 - i] = '1';
  return s;
}

class NetlistBuilder {
 public:
  NetlistBuilder(const Aig& g, const Mapped& m, Family f) : g_(g), m_(m), fam_(f) {}

  Netlist run() {
    const uint32_t nv = g_.nvars(), fa = g_.first_and();
    n_.top = g_.top;
    pos_.assign(nv, -1);
    negb_.assign(nv, -1);
    lut_of_.assign(nv, -1);
    pos_[0] = 0;
    const bool pads = fam_ == Family::Xc7;
    for (auto& p : g_.ports) {
      if (p.output) continue;
      NetPort np{p.name, false, {}, p.msb, p.lsb};
      for (Lit l : p.bits) {
        int pad = fresh();
        np.bits.push_back(pad);
        int inner = pad;
        if (pads) {
          inner = fresh();
          add_cell("IBUF", "ibuf_" + std::to_string(pad), {}, {{"I", false, {pad}}, {"O", true, {inner}}});
        }
        pos_[lit_var(l)] = inner;
      }
      n_.netnames.push_back({p.name, np.bits});
      n_.ports.push_back(std::move(np));
    }
    std::vector<char> port_in(nv, 0);
    for (uint32_t v = 1; v <= g_.nin; ++v) port_in[v] = pos_[v] >= 0;
    for (auto& b : g_.boxes) b.for_each_out([&](uint32_t v) { pos_[v] = fresh(); });
    for (auto& r : g_.rams) for (uint32_t v : r.q) if (v) pos_[v] = fresh();
    for (uint32_t v = 1; v <= g_.nin; ++v)
      if (pos_[v] < 0) pos_[v] = 0;  // an input in no port reads as 0
    for (size_t i = 0; i < g_.flops.size(); ++i) pos_[1 + g_.nin + i] = fresh();
    n_.ci_bit.assign(pos_.begin(), pos_.begin() + fa);

    // LUTs whose positive output something reads
    std::vector<char> need(nv, 0);
    g_.for_each_root([&](Lit l) { if (!lit_neg(l)) need[lit_var(l)] = 1; });
    for (size_t i = 0; i < m_.luts.size(); ++i) {
      lut_of_[m_.luts[i].root] = (int)i;
      for (int k = 0; k < m_.luts[i].n; ++k) need[m_.luts[i].leaf[k]] = 1;
    }
    for (auto& L : m_.luts) {
      if (L.n == 0) { pos_[L.root] = (int)(L.tt & 1); continue; }
      if (L.n == 1 && (L.tt & 3) == 2) { pos_[L.root] = pos_[L.leaf[0]]; alias_[L.root] = L.leaf[0]; continue; }
      if (!need[L.root]) continue;
      pos_[L.root] = fresh();
      emit_lut(L, L.tt, pos_[L.root], "lut_" + std::to_string(L.root));
    }

    for (size_t i = 0; i < g_.flops.size(); ++i) emit_ff(i);
    for (size_t k = 0; k < g_.boxes.size(); ++k) emit_box(k);
    for (size_t k = 0; k < g_.rams.size(); ++k) emit_ram(k);

    for (auto& p : g_.ports) {
      if (!p.output) continue;
      NetPort np{p.name, true, {}, p.msb, p.lsb};
      for (Lit l : p.bits) {
        int b = bit_of(l);
        if (pads) {
          int pad = fresh();
          add_cell("OBUF", "obuf_" + std::to_string(pad), {}, {{"I", false, {b}}, {"O", true, {pad}}});
          b = pad;
        } else if (b >= 2 && port_in[lit_var(l)] && !lit_neg(l)) {
          // input wired straight to an output: give the output its own driver
          int o = fresh();
          int in[1] = {b};
          emit_lut_cell(in, 1, 2, o, "buf_" + std::to_string(o));
          b = o;
        }
        np.bits.push_back(b);
      }
      n_.netnames.push_back({p.name, np.bits});
      n_.ports.push_back(std::move(np));
    }
    n_.nbits = next_;
    return std::move(n_);
  }

 private:
  int fresh() { return next_++; }
  void add_cell(std::string type, std::string name, std::vector<Param> params, std::vector<Conn> conns) {
    n_.cells.push_back({std::move(name), std::move(type), std::move(params), std::move(conns)});
  }

  int bit_of(Lit l) {
    uint32_t v = lit_var(l);
    if (v == 0) return lit_neg(l) ? 1 : 0;
    if (!lit_neg(l)) return pos_[v];
    if (pos_[v] == 0 || pos_[v] == 1) {
      if (lut_of_[v] >= 0 || v <= g_.nin) return 1 - pos_[v];
    }
    if (negb_[v] >= 0) return negb_[v];
    auto al = alias_.find(v);
    if (al != alias_.end()) return negb_[v] = bit_of(al->second * 2 + 1);
    int o = fresh();
    if (lut_of_[v] >= 0) {
      const Lut& L = m_.luts[lut_of_[v]];
      emit_lut(L, ~L.tt, o, "lut_" + std::to_string(v) + "_n");
    } else {
      int in[1] = {pos_[v]};
      emit_lut_cell(in, 1, 1, o, "inv_" + std::to_string(v));
    }
    return negb_[v] = o;
  }

  void emit_lut(const Lut& L, uint64_t tt, int out, const std::string& name) {
    int in[kMaxK];
    for (int k = 0; k < L.n; ++k) in[k] = pos_[L.leaf[k]];
    emit_lut_cell(in, L.n, tt, out, name);
  }

  // tt bit i is the output when input k has value (i >> k) & 1.
  void emit_lut_cell(const int* in, int n, uint64_t tt, int out, const std::string& name) {
    ++n_.luts;
    if (n < 6) tt &= (1ull << (1 << n)) - 1;
    for (int w = 1 << n; w < 64; w <<= 1) tt |= tt << w;
    std::vector<Conn> c;
    switch (fam_) {
      case Family::Ice40:
      case Family::Ecp5: {
        static const char* ice[4] = {"I0", "I1", "I2", "I3"};
        static const char* ecp[4] = {"A", "B", "C", "D"};
        const bool i4 = fam_ == Family::Ice40;
        for (int k = 0; k < 4; ++k) c.push_back({i4 ? ice[k] : ecp[k], false, {k < n ? in[k] : 0}});
        c.push_back({i4 ? "O" : "Z", true, {out}});
        add_cell(i4 ? "SB_LUT4" : "LUT4", name, {{i4 ? "LUT_INIT" : "INIT", bitstr(tt, 16), false}}, std::move(c));
        break;
      }
      case Family::Xc7: {
        for (int k = 0; k < n; ++k) c.push_back({"I" + std::to_string(k), false, {in[k]}});
        c.push_back({"O", true, {out}});
        add_cell("LUT" + std::to_string(n), name, {{"INIT", bitstr(tt, 1 << n), false}}, std::move(c));
        break;
      }
      case Family::Generic: {
        std::vector<int> a(in, in + n);
        c.push_back({"A", false, a});
        c.push_back({"Y", true, {out}});
        add_cell("$lut", name, {{"LUT", bitstr(tt, 1 << n), false}, {"WIDTH", bitstr(n, 32), false}}, std::move(c));
        break;
      }
    }
  }

  void emit_ff(size_t i) {
    const Flop& f = g_.flops[i];
    ++n_.ffs;
    const int q = pos_[1 + g_.nin + i];
    const int d = bit_of(f.d), clk = bit_of(f.clk);
    const bool has_r = f.arst != 0, has_e = f.en != 1, has_s = f.srst != 0;
    const int r = has_r ? bit_of(f.arst) : 0;
    const int e = bit_of(f.en), sr = bit_of(f.srst);
    n_.ff_bits.push_back({d, clk, r, e, sr});
    n_.ffs_en += has_e;
    n_.ffs_srst += has_s;
    // "_reg" keeps the cell and its Q net apart from a port of the same name
    std::string name = (f.name.empty() ? "ff_" + std::to_string(i) : f.name) + "_reg";
    while (!used_.insert(name).second) name += "_";
    n_.netnames.push_back({name + "_q", {q}});
    std::vector<Conn> c;
    std::vector<Param> p;
    switch (fam_) {
      case Family::Ice40: {
        // SB_DFF [N] [E] [R|S: asynchronous, SR|SS: synchronous]
        std::string t = f.negedge ? "SB_DFFN" : "SB_DFF";
        c = {{"C", false, {clk}}, {"D", false, {d}}, {"Q", true, {q}}};
        if (has_e) { t += "E"; c.push_back({"E", false, {e}}); }
        if (has_r) { t += f.rval ? "S" : "R"; c.push_back({f.rval ? "S" : "R", false, {r}}); }
        else if (has_s) { t += f.sval ? "SS" : "SR"; c.push_back({f.sval ? "S" : "R", false, {sr}}); }
        if (f.init == 1) n_.warnings.push_back("flop " + name + ": power-up value 1 is not available on ice40");
        add_cell(t, name, {}, std::move(c));
        break;
      }
      case Family::Ecp5: {
        bool set = has_r ? f.rval : has_s ? f.sval : f.init == 1;
        if (has_r && f.init != 2 && (f.init == 1) != f.rval)
          n_.warnings.push_back("flop " + name + ": power-up value differs from its reset value; ecp5 uses the reset value");
        c = {{"CLK", false, {clk}}, {"CE", false, {e}}, {"DI", false, {d}}, {"LSR", false, {has_r ? r : sr}}, {"Q", true, {q}}};
        p = {{"GSR", "DISABLED", true}, {"CEMUX", has_e ? "CE" : "1", true}, {"CLKMUX", f.negedge ? "INV" : "CLK", true},
             {"LSRMUX", "LSR", true}, {"REGSET", set ? "SET" : "RESET", true},
             {"SRMODE", has_r ? "ASYNC" : "LSR_OVER_CE", true}};
        add_cell("TRELLIS_FF", name, std::move(p), std::move(c));
        break;
      }
      case Family::Xc7: {
        std::string t = has_r ? (f.rval ? "FDPE" : "FDCE") : (has_s && f.sval) ? "FDSE" : "FDRE";
        c = {{"C", false, {clk}}, {"CE", false, {e}}, {"D", false, {d}}, {"Q", true, {q}}};
        if (has_r) c.push_back({f.rval ? "PRE" : "CLR", false, {r}});
        else c.push_back({has_s && f.sval ? "S" : "R", false, {sr}});
        p = {{"INIT", f.init == 1 ? "1" : "0", false}};
        if (f.negedge) p.push_back({"IS_C_INVERTED", "1", false});
        add_cell(t, name, std::move(p), std::move(c));
        break;
      }
      case Family::Generic: {
        // Yosys gate-level flops: $_DFF_P_, $_DFFE_PP_, $_DFF_PP0_, $_DFFE_PP0P_,
        // $_SDFF_PP0_, $_SDFFE_PP0P_ (reset over enable)
        const std::string ck = f.negedge ? "N" : "P";
        std::string t;
        c = {{"C", false, {clk}}, {"D", false, {d}}, {"Q", true, {q}}};
        if (has_r || has_s) {
          t = std::string(has_r ? "$_DFF" : "$_SDFF") + (has_e ? "E_" : "_") + ck + "P" +
              ((has_r ? f.rval : f.sval) ? "1" : "0") + (has_e ? "P_" : "_");
          c.push_back({"R", false, {has_r ? r : sr}});
        } else {
          t = has_e ? "$_DFFE_" + ck + "P_" : "$_DFF_" + ck + "_";
        }
        if (has_e) c.push_back({"E", false, {e}});
        if (f.init != 2) n_.inits.push_back({q, f.init});  // Yosys keeps it as the net's init attribute
        add_cell(t, name, {}, std::move(c));
        break;
      }
    }
  }

  // One memory on the family's block RAMs: as many side by side as the word
  // is wide, in the widest configuration that is deep enough.
  void emit_ram(size_t k) {
    const Ram& R = g_.rams[k];
    n_.ram_pins.emplace_back();
    auto pin = [&](Lit l) { int b = bit_of(l); n_.ram_pins.back().push_back({b, l}); return b; };
    std::vector<int> wa, wd, ra;
    for (Lit l : R.waddr) wa.push_back(pin(l));
    for (Lit l : R.wdata) wd.push_back(pin(l));
    for (Lit l : R.raddr) ra.push_back(pin(l));
    const int wen = pin(R.wen), ren = pin(R.ren), wclk = pin(R.wclk), rclk = pin(R.rclk);
    auto q = [&](int i) { return i < R.width && R.q[i] ? pos_[R.q[i]] : fresh(); };
    auto padded = [](std::vector<int> v, size_t w) { v.resize(w, 0); return v; };
    std::string base = R.name;
    for (char& ch : base) if (ch == '/' || ch == '.') ch = '_';
    switch (fam_) {
      case Family::Ice40: {
        // SB_RAM40_4K: 256 x 16, 512 x 8, 1024 x 4 or 2048 x 2; the narrow modes use these data bits
        const int mode = R.abits <= 8 ? 0 : R.abits - 8;
        static const std::vector<int> lanes[4] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                                                  {0, 2, 4, 6, 8, 10, 12, 14}, {1, 5, 9, 13}, {3, 11}};
        const std::vector<int>& lane = lanes[mode];
        const int w = (int)lane.size();
        const std::string type = std::string("SB_RAM40_4K") + (R.rneg || R.wneg ? "N" : "") + (R.rneg ? "R" : "") + (R.wneg ? "W" : "");
        for (int at = 0, j = 0; at < R.width; at += w, ++j) {
          std::vector<int> di(16, 0), dout(16);
          for (int b = 0; b < 16; ++b) dout[b] = fresh();
          for (int b = 0; b < w && at + b < R.width; ++b) { di[lane[b]] = wd[at + b]; dout[lane[b]] = q(at + b); }
          std::string name = base + "_ram" + std::to_string(j);
          while (!used_.insert(name).second) name += "_";
          ++n_.brams;
          add_cell(type, name, {{"READ_MODE", bitstr(mode, 32), false}, {"WRITE_MODE", bitstr(mode, 32), false}},
                   {{"RDATA", true, dout}, {R.rneg ? "RCLKN" : "RCLK", false, {rclk}}, {"RCLKE", false, {ren}}, {"RE", false, {1}},
                    {"RADDR", false, padded(ra, 11)}, {R.wneg ? "WCLKN" : "WCLK", false, {wclk}}, {"WCLKE", false, {wen}},
                    {"WE", false, {1}}, {"WADDR", false, padded(wa, 11)}, {"MASK", false, std::vector<int>(16, 0)},
                    {"WDATA", false, di}});
        }
        break;
      }
      case Family::Ecp5: {
        // DP16KD: port A writes, port B reads. 1024 x 18 down to 16384 x 1; the address sits at
        // the top of AD[13:0], and in the 18 and 9 bit modes the low bits of port A are byte enables.
        const int w = R.abits <= 10 ? 18 : R.abits == 11 ? 9 : R.abits == 12 ? 4 : R.abits == 13 ? 2 : 1;
        const int shift = w == 18 ? 4 : w == 9 ? 3 : w == 4 ? 2 : w == 2 ? 1 : 0;
        const int deep = 14 - shift;   // address bits of this mode
        for (int at = 0, j = 0; at < R.width; at += w, ++j) {
          std::vector<Conn> c;
          auto bus = [&](const char* pre, const std::vector<int>& v, bool out) {
            for (size_t i = 0; i < v.size(); ++i) c.push_back({pre + std::to_string(i), out, {v[i]}});
          };
          std::vector<int> ada(14, 0), adb(14, 0), dia(18, 0), dib(18, 0), doa(18), dob(18);
          for (int i = 0; i < deep; ++i) {
            ada[shift + i] = i < (int)wa.size() ? wa[i] : 0;
            adb[shift + i] = i < (int)ra.size() ? ra[i] : 0;
          }
          if (w == 18) ada[0] = ada[1] = 1;
          if (w == 9) ada[0] = 1;
          for (int b = 0; b < 18; ++b) {
            doa[b] = fresh();
            dob[b] = b < w && at + b < R.width ? q(at + b) : fresh();
            if (b < w && at + b < R.width) dia[b] = wd[at + b];
          }
          bus("DIA", dia, false); bus("ADA", ada, false);
          c.push_back({"CEA", false, {1}}); c.push_back({"OCEA", false, {1}}); c.push_back({"CLKA", false, {wclk}});
          c.push_back({"WEA", false, {wen}}); c.push_back({"RSTA", false, {0}});
          bus("CSA", {0, 0, 0}, false); bus("DOA", doa, true);
          bus("DIB", dib, false); bus("ADB", adb, false);
          c.push_back({"CEB", false, {ren}}); c.push_back({"OCEB", false, {1}}); c.push_back({"CLKB", false, {rclk}});
          c.push_back({"WEB", false, {0}}); c.push_back({"RSTB", false, {0}});
          bus("CSB", {0, 0, 0}, false); bus("DOB", dob, true);
          std::string name = base + "_ram" + std::to_string(j);
          while (!used_.insert(name).second) name += "_";
          ++n_.brams;
          add_cell("DP16KD", name,
                   {{"DATA_WIDTH_A", bitstr(w, 32), false}, {"DATA_WIDTH_B", bitstr(w, 32), false},
                    {"REGMODE_A", "NOREG", true}, {"REGMODE_B", "NOREG", true}, {"RESETMODE", "SYNC", true},
                    {"ASYNC_RESET_RELEASE", "SYNC", true}, {"CSDECODE_A", "0b000", true}, {"CSDECODE_B", "0b000", true},
                    {"WRITEMODE_A", "NORMAL", true}, {"WRITEMODE_B", "NORMAL", true},
                    {"CLKAMUX", R.wneg ? "INV" : "CLKA", true}, {"CLKBMUX", R.rneg ? "INV" : "CLKB", true},
                    {"GSR", "DISABLED", true}},
                   std::move(c));
        }
        break;
      }
      case Family::Xc7: {
        // RAMB18E1 as a true dual-port RAM: port A reads, port B writes. 1024 x 18 down to
        // 16384 x 1; the address sits at the top of ADDR[13:0], and bits 16 and 17 of an 18 bit
        // word (bit 8 of a 9 bit word) are the parity bits.
        const int w = R.abits <= 10 ? 18 : R.abits == 11 ? 9 : R.abits == 12 ? 4 : R.abits == 13 ? 2 : 1;
        const int shift = w == 18 ? 4 : w == 9 ? 3 : w == 4 ? 2 : w == 2 ? 1 : 0;
        const int nd = w == 18 ? 16 : w == 9 ? 8 : w;   // data bits; the rest of a word is parity
        for (int at = 0, j = 0; at < R.width; at += w, ++j) {
          std::vector<int> ada(14, 1), adb(14, 1), di(16, 0), dip(2, 0), doa(16), dopa(2), dob(16), dopb(2);
          for (int i = 0; i < 14 - shift; ++i) {
            ada[shift + i] = i < (int)ra.size() ? ra[i] : 0;
            adb[shift + i] = i < (int)wa.size() ? wa[i] : 0;
          }
          for (int b = 0; b < 16; ++b) { doa[b] = fresh(); dob[b] = fresh(); }
          for (int b = 0; b < 2; ++b) { dopa[b] = fresh(); dopb[b] = fresh(); }
          for (int b = 0; b < w && at + b < R.width; ++b) {
            if (b < nd) { di[b] = wd[at + b]; doa[b] = q(at + b); }
            else { dip[b - nd] = wd[at + b]; dopa[b - nd] = q(at + b); }
          }
          std::string name = base + "_ram" + std::to_string(j);
          while (!used_.insert(name).second) name += "_";
          ++n_.brams;
          const std::vector<int> z16(16, 0), z2(2, 0);
          add_cell("RAMB18E1", name,
                   {{"RAM_MODE", "TDP", true}, {"READ_WIDTH_A", bitstr(w, 32), false}, {"READ_WIDTH_B", bitstr(0, 32), false},
                    {"WRITE_WIDTH_A", bitstr(0, 32), false}, {"WRITE_WIDTH_B", bitstr(w, 32), false},
                    {"DOA_REG", bitstr(0, 32), false}, {"DOB_REG", bitstr(0, 32), false},
                    {"WRITE_MODE_A", "READ_FIRST", true}, {"WRITE_MODE_B", "READ_FIRST", true},
                    {"IS_CLKARDCLK_INVERTED", R.rneg ? "1" : "0", false}, {"IS_CLKBWRCLK_INVERTED", R.wneg ? "1" : "0", false},
                    {"SIM_DEVICE", "7SERIES", true}},
                   {{"CLKARDCLK", false, {rclk}}, {"ENARDEN", false, {ren}}, {"REGCEAREGCE", false, {1}},
                    {"RSTRAMARSTRAM", false, {0}}, {"RSTREGARSTREG", false, {0}}, {"WEA", false, z2},
                    {"ADDRARDADDR", false, ada}, {"DIADI", false, z16}, {"DIPADIP", false, z2},
                    {"DOADO", true, doa}, {"DOPADOP", true, dopa},
                    {"CLKBWRCLK", false, {wclk}}, {"ENBWREN", false, {wen}}, {"REGCEB", false, {1}},
                    {"RSTRAMB", false, {0}}, {"RSTREGB", false, {0}}, {"WEBWE", false, std::vector<int>(4, 1)},
                    {"ADDRBWRADDR", false, adb}, {"DIBDI", false, di}, {"DIPBDIP", false, dip},
                    {"DOBDO", true, dob}, {"DOPBDOP", true, dopb}});
        }
        break;
      }
      default:
        throw std::runtime_error("fpga: no block RAM cell for this family");
    }
  }

  // One multiplier on the family's DSP block.
  void emit_mul(size_t k) {
    const Box& B = g_.boxes[k];
    ++n_.dsps;
    n_.box_pins.emplace_back();
    // operands padded to the block's width: by the sign, or by 0
    auto operand = [&](const std::vector<Lit>& v, bool sg, size_t w) {
      std::vector<int> r;
      for (Lit l : v) { r.push_back(bit_of(l)); n_.box_pins.back().push_back({r.back(), l}); }
      const int ext = sg ? r.back() : 0;
      while (r.size() < w) r.push_back(ext);
      return r;
    };
    auto product = [&](size_t w) {
      std::vector<int> r;
      for (size_t i = 0; i < w; ++i) r.push_back(i < B.sum.size() && B.sum[i] ? pos_[B.sum[i]] : fresh());
      return r;
    };
    const std::string name = "mul" + std::to_string(k);
    auto num = [](unsigned v, int w) { return bitstr(v, w); };
    switch (fam_) {
      case Family::Ice40: {
        std::vector<Param> p;
        for (const char* z : {"NEG_TRIGGER", "C_REG", "A_REG", "B_REG", "D_REG", "TOP_8x8_MULT_REG", "BOT_8x8_MULT_REG",
                              "PIPELINE_16x16_MULT_REG1", "PIPELINE_16x16_MULT_REG2", "TOPADDSUB_UPPERINPUT",
                              "BOTADDSUB_UPPERINPUT", "MODE_8x8"})
          p.push_back({z, "0", false});
        for (const char* z : {"TOPADDSUB_LOWERINPUT", "TOPADDSUB_CARRYSELECT", "BOTADDSUB_LOWERINPUT", "BOTADDSUB_CARRYSELECT"})
          p.push_back({z, "00", false});
        p.push_back({"TOPOUTPUT_SELECT", "11", false});
        p.push_back({"BOTOUTPUT_SELECT", "11", false});
        p.push_back({"A_SIGNED", B.sa ? "1" : "0", false});
        p.push_back({"B_SIGNED", B.sb ? "1" : "0", false});
        add_cell("SB_MAC16", name, std::move(p),
                 {{"A", false, operand(B.a, B.sa, 16)}, {"B", false, operand(B.b, B.sb, 16)}, {"O", true, product(32)}});
        break;
      }
      case Family::Ecp5: {
        std::vector<Conn> c;
        auto bus = [&](const char* pre, const std::vector<int>& v, bool out) {
          for (size_t i = 0; i < v.size(); ++i) c.push_back({pre + std::to_string(i), out, {v[i]}});
        };
        bus("A", operand(B.a, B.sa, 18), false);
        bus("B", operand(B.b, B.sb, 18), false);
        bus("C", std::vector<int>(18, 0), false);
        c.push_back({"SIGNEDA", false, {B.sa ? 1 : 0}});
        c.push_back({"SIGNEDB", false, {B.sb ? 1 : 0}});
        c.push_back({"SOURCEA", false, {0}});
        c.push_back({"SOURCEB", false, {0}});
        bus("P", product(36), true);
        add_cell("MULT18X18D", name,
                 {{"REG_INPUTA_CLK", "NONE", true}, {"REG_INPUTB_CLK", "NONE", true}, {"REG_INPUTC_CLK", "NONE", true},
                  {"REG_PIPELINE_CLK", "NONE", true}, {"REG_OUTPUT_CLK", "NONE", true}, {"SOURCEB_MODE", "B_SHIFT", true},
                  {"MULT_BYPASS", "DISABLED", true}, {"RESETMODE", "SYNC", true}, {"GSR", "DISABLED", true}},
                 std::move(c));
        break;
      }
      case Family::Xc7: {
        std::vector<Param> p;
        for (const char* z : {"ACASCREG", "ADREG", "ALUMODEREG", "AREG", "BCASCREG", "BREG", "CARRYINREG", "CARRYINSELREG",
                              "CREG", "DREG", "INMODEREG", "MREG", "OPMODEREG", "PREG"})
          p.push_back({z, num(0, 32), false});
        p.push_back({"A_INPUT", "DIRECT", true});
        p.push_back({"B_INPUT", "DIRECT", true});
        p.push_back({"USE_MULT", "MULTIPLY", true});
        p.push_back({"USE_SIMD", "ONE48", true});
        p.push_back({"USE_DPORT", "FALSE", true});
        const std::vector<int> z48(48, 0);
        auto zeros = [](size_t w) { return std::vector<int>(w, 0); };
        std::vector<int> opmode = {1, 0, 1, 0, 0, 0, 0};   // 7'b0000101: P = M
        add_cell("DSP48E1", name, std::move(p),
                 {{"A", false, operand(B.a, true, 30)}, {"B", false, operand(B.b, true, 18)}, {"C", false, zeros(48)},
                  {"D", false, zeros(25)}, {"CARRYIN", false, {0}}, {"INMODE", false, zeros(5)}, {"ALUMODE", false, zeros(4)},
                  {"OPMODE", false, opmode}, {"CARRYINSEL", false, zeros(3)}, {"ACIN", false, zeros(30)},
                  {"BCIN", false, zeros(18)}, {"PCIN", false, zeros(48)}, {"P", true, product(48)}});
        break;
      }
      case Family::Generic:
        throw std::runtime_error("fpga: the generic family has no DSP cells");
    }
  }

  // The carry chain of one box.
  void emit_box(size_t k) {
    const Box& B = g_.boxes[k];
    if (B.mul) { emit_mul(k); return; }
    // ecp5 and xc7 chains have a LUT per bit: it takes b as it is and inverts
    // inside. ice40's SB_CARRY has none, so a subtraction needs b inverted outside.
    const bool inv = B.binv && fam_ != Family::Ice40;
    std::vector<int> A, Bb, S;   // operand nets and sum nets (-1: not used)
    n_.box_pins.emplace_back();
    for (Lit l : B.a) { A.push_back(bit_of(l)); n_.box_pins.back().push_back({A.back(), l}); }
    for (Lit l : B.b) { Bb.push_back(bit_of(l ^ (Lit)inv)); n_.box_pins.back().push_back({Bb.back(), l ^ (Lit)inv}); }
    const int pad = inv ? 1 : 0;   // an operand bit that is 0 past the top
    for (uint32_t v : B.sum) S.push_back(v ? pos_[v] : -1);
    int cout = B.cout ? pos_[B.cout] : -1;
    const std::string base = "add" + std::to_string(k) + "_";
    if (fam_ == Family::Ecp5 && cout >= 0) {
      // the carry leaves the chain as the sum of one more bit: 0 + 0 + carry
      A.push_back(0); Bb.push_back(pad); S.push_back(cout);
      cout = -1;
    }
    size_t w = A.size();
    if (cout < 0) while (w > 0 && S[w - 1] < 0) --w;   // bits above the last used sum
    n_.carry_bits += w;
    ++n_.chains;
    int c = B.cin ? 1 : 0;
    switch (fam_) {
      case Family::Ice40:
        for (size_t i = 0; i < w; ++i) {
          const std::string id = base + std::to_string(i);
          if (S[i] >= 0) {
            // the sum LUT shares a logic cell with its SB_CARRY: I1, I2 are the operands, I3 the carry in
            uint64_t x = (detail::kVarTT[1] ^ detail::kVarTT[2] ^ detail::kVarTT[3]) & 0xffff;
            ++n_.luts;
            add_cell("SB_LUT4", id + "_sum", {{"LUT_INIT", bitstr(x, 16), false}},
                     {{"I0", false, {0}}, {"I1", false, {A[i]}}, {"I2", false, {Bb[i]}}, {"I3", false, {c}}, {"O", true, {S[i]}}});
          }
          const bool last = i + 1 == w;
          if (last && cout < 0) break;
          int co = last ? cout : fresh();
          add_cell("SB_CARRY", id + "_carry", {},
                   {{"CI", false, {c}}, {"I0", false, {A[i]}}, {"I1", false, {Bb[i]}}, {"CO", true, {co}}});
          c = co;
        }
        break;
      case Family::Ecp5:
        for (size_t i = 0; i < w; i += 2) {
          const bool two = i + 1 < w;
          int co = fresh();
          add_cell("CCU2C", base + std::to_string(i),
                   // with C = 0 and D = 1 INIT[11:8] is the propagate LUT of A, B (xor, or xnor to
                   // subtract) and INIT[3:0] the generate LUT (A)
                   {{"INIT0", inv ? "1001100110101010" : "1001011010101010", false},
                    {"INIT1", inv ? "1001100110101010" : "1001011010101010", false},
                    {"INJECT1_0", "NO", true}, {"INJECT1_1", "NO", true}},
                   {{"CIN", false, {c}},
                    {"A0", false, {A[i]}}, {"B0", false, {Bb[i]}}, {"C0", false, {0}}, {"D0", false, {1}},
                    {"A1", false, {two ? A[i + 1] : 0}}, {"B1", false, {two ? Bb[i + 1] : pad}}, {"C1", false, {0}}, {"D1", false, {1}},
                    {"S0", true, {S[i] >= 0 ? S[i] : fresh()}}, {"S1", true, {two && S[i + 1] >= 0 ? S[i + 1] : fresh()}},
                    {"COUT", true, {co}}});
          c = co;
        }
        break;
      case Family::Xc7:
        for (size_t i = 0; i < w; i += 4) {
          std::vector<int> di(4, 0), sel(4, 0), o(4), co(4);
          for (size_t j = 0; j < 4; ++j) {
            o[j] = i + j < w && S[i + j] >= 0 ? S[i + j] : fresh();
            co[j] = i + j + 1 == w && cout >= 0 ? cout : fresh();
            if (i + j >= w) continue;
            di[j] = A[i + j];
            sel[j] = fresh();   // propagate: a ^ b
            int in[2] = {A[i + j], Bb[i + j]};
            emit_lut_cell(in, 2, inv ? 9 : 6, sel[j], base + std::to_string(i + j) + "_p");
          }
          add_cell("CARRY4", base + std::to_string(i), {},
                   {{"CI", false, {i ? c : 0}}, {"CYINIT", false, {i ? 0 : c}}, {"DI", false, di}, {"S", false, sel},
                    {"O", true, o}, {"CO", true, co}});
          c = co[3];
        }
        break;
      case Family::Generic:
        throw std::runtime_error("fpga: the generic family has no carry cells");
    }
  }

  const Aig& g_;
  const Mapped& m_;
  Family fam_;
  Netlist n_;
  std::vector<int> pos_, negb_, lut_of_;
  std::map<uint32_t, uint32_t> alias_;
  std::set<std::string> used_;
  int next_ = 2;
};

inline Netlist build_netlist(const Aig& g, const Mapped& m, Family f) { return NetlistBuilder(g, m, f).run(); }

// -------------------------------------------------------------- JSON writer

inline std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (char ch : s) {
    if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
    else if ((unsigned char)ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
    else o += ch;
  }
  return o + "\"";
}

inline void json_bits(std::string& o, const std::vector<int>& bits) {
  o += '[';
  for (size_t i = 0; i < bits.size(); ++i) {
    if (i) o += ',';
    if (bits[i] < 2) o += bits[i] ? "\"1\"" : "\"0\"";
    else o += std::to_string(bits[i]);
  }
  o += ']';
}

// Yosys JSON netlist, the format nextpnr reads (--json).
inline std::string write_json(const Netlist& n, const std::string& creator = "mfpga") {
  std::string o;
  o.reserve(n.cells.size() * 400 + 1024);
  o += "{\n \"creator\": " + json_str(creator) + ",\n \"modules\": {\n  " + json_str(n.top) + ": {\n";
  o += "   \"attributes\": {\"top\": \"00000000000000000000000000000001\"},\n   \"ports\": {";
  for (size_t i = 0; i < n.ports.size(); ++i) {
    auto& p = n.ports[i];
    o += i ? ",\n    " : "\n    ";
    o += json_str(p.name) + ": {\"direction\": \"" + (p.output ? "output" : "input") + "\", \"bits\": ";
    json_bits(o, p.bits);
    if (std::min(p.msb, p.lsb) != 0) o += ", \"offset\": " + std::to_string(std::min(p.msb, p.lsb));
    if (p.msb < p.lsb) o += ", \"upto\": 1";
    o += '}';
  }
  o += "\n   },\n   \"cells\": {";
  for (size_t i = 0; i < n.cells.size(); ++i) {
    auto& c = n.cells[i];
    o += i ? ",\n    " : "\n    ";
    o += json_str(c.name) + ": {\"hide_name\": 0, \"type\": " + json_str(c.type) + ", \"parameters\": {";
    for (size_t k = 0; k < c.params.size(); ++k) {
      auto& p = c.params[k];
      if (k) o += ", ";
      // Yosys marks a text value made only of 0/1/x/z with a trailing space.
      bool amb = p.is_string && p.value.find_first_not_of("01xz") == std::string::npos;
      o += json_str(p.name) + ": " + json_str(amb ? p.value + " " : p.value);
    }
    o += "}, \"attributes\": {}, \"port_directions\": {";
    for (size_t k = 0; k < c.conns.size(); ++k) {
      if (k) o += ", ";
      o += json_str(c.conns[k].port) + ": \"" + (c.conns[k].output ? "output" : "input") + "\"";
    }
    o += "}, \"connections\": {";
    for (size_t k = 0; k < c.conns.size(); ++k) {
      if (k) o += ", ";
      o += json_str(c.conns[k].port) + ": ";
      json_bits(o, c.conns[k].bits);
    }
    o += "}}";
  }
  o += "\n   },\n   \"netnames\": {";
  bool first = true;
  std::map<std::string, bool> seen;
  for (auto& nn : n.netnames) {
    if (seen[nn.first]) continue;
    seen[nn.first] = true;
    o += first ? "\n    " : ",\n    ";
    first = false;
    o += json_str(nn.first) + ": {\"hide_name\": 0, \"bits\": ";
    json_bits(o, nn.second);
    o += ", \"attributes\": {";
    if (nn.second.size() == 1)
      for (auto& in : n.inits)
        if (in.first == nn.second[0]) o += std::string("\"init\": \"") + (in.second ? "1" : "0") + "\"";
    o += "}}";
  }
  o += "\n   }\n  }\n }\n}\n";
  return o;
}

// -------------------------------------------------------------- EDIF writer

inline std::string edif_id(const std::string& s) {
  std::string o;
  for (char ch : s) o += (isalnum((unsigned char)ch) || ch == '_') ? ch : '_';
  if (o.empty() || !isalpha((unsigned char)o[0])) o = "n_" + o;
  return o;
}
inline std::string edif_name(const std::string& s) {
  std::string id = edif_id(s);
  return id == s ? id : "(rename " + id + " \"" + s + "\")";
}

// EDIF 2 0 0 netlist. Constants are driven by GND (pin G) and VCC (pin P)
// cells; bit-vector parameters are written as Verilog-style hex ("16'h6996").
inline std::string write_edif(const Netlist& n, const std::string& lib = "hdi_primitives") {
  std::ostringstream o;
  std::string top = edif_id(n.top);
  o << "(edif " << top << "\n (edifVersion 2 0 0)\n (edifLevel 0)\n (keywordMap (keywordLevel 0))\n"
    << " (status (written (timeStamp 1970 1 1 0 0 0) (program \"mfpga\")))\n"
    << " (external " << lib << "\n  (edifLevel 0)\n  (technology (numberDefinition))\n";
  std::map<std::string, std::vector<std::pair<std::string, bool>>> types;
  for (auto& c : n.cells) {
    auto& t = types[c.type];
    if (t.empty())
      for (auto& cn : c.conns)
        for (size_t b = 0; b < cn.bits.size(); ++b)
          t.push_back({cn.bits.size() > 1 ? cn.port + "[" + std::to_string(b) + "]" : cn.port, cn.output});
  }
  bool use0 = false, use1 = false;
  auto scan = [&](const std::vector<int>& bits) { for (int b : bits) { use0 |= b == 0; use1 |= b == 1; } };
  for (auto& c : n.cells) for (auto& cn : c.conns) scan(cn.bits);
  for (auto& p : n.ports) scan(p.bits);
  if (use0) types["GND"] = {{"G", true}};
  if (use1) types["VCC"] = {{"P", true}};
  for (auto& [name, ports] : types) {
    o << "  (cell " << edif_name(name) << " (cellType GENERIC)\n   (view netlist (viewType NETLIST)\n    (interface";
    for (auto& p : ports) o << "\n     (port " << edif_name(p.first) << " (direction " << (p.second ? "OUTPUT" : "INPUT") << "))";
    o << ")))\n";
  }
  o << " )\n (library work\n  (edifLevel 0)\n  (technology (numberDefinition))\n  (cell " << edif_name(n.top)
    << " (cellType GENERIC)\n   (view netlist (viewType NETLIST)\n    (interface";
  for (auto& p : n.ports) {
    std::string dir = p.output ? "OUTPUT" : "INPUT";
    if (p.bits.size() == 1) o << "\n     (port " << edif_name(p.name) << " (direction " << dir << "))";
    else
      o << "\n     (port (array (rename " << edif_id(p.name) << " \"" << p.name << "[" << p.msb << ":" << p.lsb << "]\") " << p.bits.size() << ") (direction " << dir << "))";
  }
  o << ")\n    (contents";
  std::vector<std::string> joined(n.nbits);
  if (use0) { o << "\n     (instance GND (viewRef netlist (cellRef GND (libraryRef " + lib + "))))"; joined[0] += " (portRef G (instanceRef GND))"; }
  if (use1) { o << "\n     (instance VCC (viewRef netlist (cellRef VCC (libraryRef " + lib + "))))"; joined[1] += " (portRef P (instanceRef VCC))"; }
  for (auto& p : n.ports)
    for (size_t b = 0; b < p.bits.size(); ++b) {
      if (p.bits.size() == 1) joined[p.bits[b]] += " (portRef " + edif_id(p.name) + ")";
      else joined[p.bits[b]] += " (portRef (member " + edif_id(p.name) + " " + std::to_string(p.bits.size() - 1 - b) + "))";
    }
  for (auto& c : n.cells) {
    std::string inst = edif_id(c.name);
    o << "\n     (instance " << edif_name(c.name) << " (viewRef netlist (cellRef " << edif_id(c.type) << " (libraryRef " + lib + ")))";
    for (auto& p : c.params) {
      o << "\n      (property " << edif_id(p.name) << " (string \"";
      if (p.is_string) o << p.value;
      else {
        size_t w = p.value.size();
        o << w << "'h";
        std::string hex;
        for (size_t e = w; e > 0;) {
          int d = 0;
          size_t s = e >= 4 ? e - 4 : 0;
          for (size_t k = s; k < e; ++k) d = d * 2 + (p.value[k] == '1');
          hex.insert(hex.begin(), "0123456789ABCDEF"[d]);
          e = s;
        }
        o << hex;
      }
      o << "\"))";
    }
    o << ")";
    for (auto& cn : c.conns)
      for (size_t b = 0; b < cn.bits.size(); ++b) {
        std::string pn = cn.bits.size() > 1 ? cn.port + "[" + std::to_string(b) + "]" : cn.port;
        joined[cn.bits[b]] += " (portRef " + edif_id(pn) + " (instanceRef " + inst + "))";
      }
  }
  for (int b = 0; b < n.nbits; ++b)
    if (!joined[b].empty()) o << "\n     (net n" << b << " (joined" << joined[b] << "))";
  o << "))))\n (design " << top << " (cellRef " << top << " (libraryRef work))))\n";
  return o.str();
}

// ----------------------------------------------------------- Verilog writer

inline std::string verilog_id(const std::string& s) {
  bool simple = !s.empty() && (isalpha((unsigned char)s[0]) || s[0] == '_');
  for (char ch : s) simple &= isalnum((unsigned char)ch) || ch == '_';
  return simple ? s : "\\" + s + " ";
}

// Structural Verilog of the cells, for simulation with the vendor's models.
inline std::string write_verilog(const Netlist& n) {
  std::string o = "module " + verilog_id(n.top) + "(";
  for (size_t i = 0; i < n.ports.size(); ++i) o += (i ? ", " : "") + verilog_id(n.ports[i].name);
  o += ");\n";
  auto bit = [](int b) { return b < 2 ? std::string(b ? "1'b1" : "1'b0") : "n" + std::to_string(b); };
  for (auto& p : n.ports) {
    o += p.output ? "  output " : "  input ";
    if (p.bits.size() > 1 || p.msb != p.lsb) o += "[" + std::to_string(p.msb) + ":" + std::to_string(p.lsb) + "] ";
    o += verilog_id(p.name) + ";\n";
  }
  for (int b = 2; b < n.nbits; ++b) o += "  wire n" + std::to_string(b) + ";\n";
  for (auto& p : n.ports)
    for (size_t k = 0; k < p.bits.size(); ++k) {
      std::string ref = verilog_id(p.name);
      if (p.bits.size() > 1 || p.msb != p.lsb)
        ref += "[" + std::to_string(p.msb >= p.lsb ? p.lsb + (int)k : p.lsb - (int)k) + "]";
      if (p.output) o += "  assign " + ref + " = " + bit(p.bits[k]) + ";\n";
      else o += "  assign " + bit(p.bits[k]) + " = " + ref + ";\n";
    }
  for (auto& c : n.cells) {
    o += "  " + verilog_id(c.type);
    if (!c.params.empty()) {
      o += " #(";
      for (size_t k = 0; k < c.params.size(); ++k) {
        auto& p = c.params[k];
        o += (k ? ", ." : ".") + p.name + "(";
        o += p.is_string ? "\"" + p.value + "\"" : std::to_string(p.value.size()) + "'b" + p.value;
        o += ")";
      }
      o += ")";
    }
    o += " " + verilog_id(c.name) + " (";
    for (size_t k = 0; k < c.conns.size(); ++k) {
      auto& cn = c.conns[k];
      o += (k ? ", ." : ".") + cn.port + "(";
      if (cn.bits.size() == 1) o += bit(cn.bits[0]);
      else {
        o += "{";
        for (size_t b = cn.bits.size(); b-- > 0;) o += bit(cn.bits[b]) + (b ? ", " : "");
        o += "}";
      }
      o += ")";
    }
    o += ");\n";
  }
  return o + "endmodule\n";
}

// ------------------------------------------------------------- AIGER reader

// Reads AIGER (ASCII "aag" or binary "aig") into an aig::Design. Latches become flops on one
// clock input named `clock`. Symbols like "a[3]" are grouped into buses.
struct AigerDesign {
  aig::Design design;
  std::vector<int> init;   // power-up value per flop: 0, 1, 2 = don't care
};

inline AigerDesign read_aiger(std::istream& is, const std::string& clock = "clk") {
  std::string magic;
  uint32_t M, I, L, O, A;
  std::string header;
  std::getline(is, header);
  {
    std::istringstream hs(header);
    if (!(hs >> magic >> M >> I >> L >> O >> A) || (magic != "aag" && magic != "aig"))
      throw std::runtime_error("not an AIGER file");
    uint32_t extra;
    while (hs >> extra)
      if (extra) throw std::runtime_error("AIGER 1.9 bad/constraint/justice/fairness sections are not supported");
  }
  const bool bin = magic == "aig";
  std::vector<Lit> in_l(I), latch_l(L), next(L), out(O);
  std::vector<int> init(L, 0);
  std::vector<std::array<Lit, 3>> ands(A);
  auto line = [&]() { std::string s; if (!std::getline(is, s)) throw std::runtime_error("AIGER: unexpected end of file"); return s; };
  for (uint32_t i = 0; i < I; ++i) in_l[i] = bin ? 2 * (i + 1) : (Lit)std::stoul(line());
  for (uint32_t i = 0; i < L; ++i) {
    std::istringstream ls(line());
    if (bin) latch_l[i] = 2 * (I + i + 1); else ls >> latch_l[i];
    ls >> next[i];
    uint32_t r;
    if (ls >> r) init[i] = r == 0 ? 0 : r == 1 ? 1 : 2;
  }
  for (uint32_t i = 0; i < O; ++i) out[i] = (Lit)std::stoul(line());
  if (bin) {
    auto rd = [&]() {
      uint32_t x = 0; int sh = 0; int ch;
      while ((ch = is.get()) != EOF) { x |= (uint32_t)(ch & 0x7f) << sh; if (!(ch & 0x80)) return x; sh += 7; }
      throw std::runtime_error("AIGER: unexpected end of file");
    };
    for (uint32_t i = 0; i < A; ++i) {
      Lit lhs = 2 * (I + L + i + 1);
      uint32_t d0 = rd(), d1 = rd();
      ands[i] = {lhs, lhs - d0, lhs - d0 - d1};
    }
  } else {
    for (uint32_t i = 0; i < A; ++i) { std::istringstream ls(line()); ls >> ands[i][0] >> ands[i][1] >> ands[i][2]; }
  }
  std::vector<std::string> in_n(I), latch_n(L), out_n(O);
  for (std::string s; std::getline(is, s);) {
    if (s.empty()) continue;
    if (s[0] == 'c') break;
    size_t sp = s.find(' ');
    if (sp == std::string::npos || sp < 2) continue;
    uint32_t idx = (uint32_t)std::stoul(s.substr(1, sp - 1));
    std::string nm = s.substr(sp + 1);
    if (s[0] == 'i' && idx < I) in_n[idx] = nm;
    else if (s[0] == 'l' && idx < L) latch_n[idx] = nm;
    else if (s[0] == 'o' && idx < O) out_n[idx] = nm;
  }

  AigerDesign res;
  aig::Design& D = res.design;
  aig::Graph& g = D.g;
  std::vector<Lit> m(M + 1, aig::NOLIT);  // file variable -> literal
  m[0] = 0;
  auto chk = [&](Lit l) { if (lit_var(l) > M) throw std::runtime_error("AIGER: literal out of range"); return l; };
  std::vector<Lit> in_lit(I);
  for (uint32_t i = 0; i < I; ++i) {
    if (in_n[i].empty()) in_n[i] = "i" + std::to_string(i);
    m[lit_var(chk(in_l[i]))] = in_lit[i] = g.input(in_n[i]);
  }
  const Lit clk = L ? g.input(clock) : 0;
  for (uint32_t i = 0; i < L; ++i) {
    aig::Flop f;
    f.clk = clk;
    f.name = latch_n[i];
    m[lit_var(chk(latch_l[i]))] = g.dff(f);
  }
  res.init = init;
  // ANDs in dependency order (the ASCII format allows any order)
  std::vector<uint32_t> def(M + 1, UINT32_MAX);
  for (uint32_t i = 0; i < A; ++i) { chk(ands[i][0]); chk(ands[i][1]); chk(ands[i][2]); def[lit_var(ands[i][0])] = i; }
  std::vector<uint32_t> stack;
  std::vector<char> state(A, 0);
  for (uint32_t i = 0; i < A; ++i) {
    if (state[i] == 2) continue;
    stack.push_back(i);
    while (!stack.empty()) {
      uint32_t a = stack.back();
      if (state[a] == 2) { stack.pop_back(); continue; }
      bool ready = true;
      for (int k = 1; k <= 2; ++k) {
        uint32_t v = lit_var(ands[a][k]);
        if (m[v] != aig::NOLIT) continue;
        if (def[v] == UINT32_MAX) throw std::runtime_error("AIGER: undefined literal " + std::to_string(ands[a][k]));
        if (state[def[v]] == 1) throw std::runtime_error("AIGER: combinational loop");
        ready = false;
        stack.push_back(def[v]);
      }
      if (!ready) { state[a] = 1; continue; }
      auto tr = [&](Lit l) { return m[lit_var(l)] ^ (l & 1); };
      m[lit_var(ands[a][0])] = g.andl(tr(ands[a][1]), tr(ands[a][2]));
      state[a] = 2;
      stack.pop_back();
    }
  }
  auto tr = [&](Lit l) {
    if (m[lit_var(chk(l))] == aig::NOLIT) throw std::runtime_error("AIGER: undefined literal " + std::to_string(l));
    return m[lit_var(l)] ^ (l & 1);
  };
  for (uint32_t i = 0; i < L; ++i) g.flops[i].d = tr(next[i]);
  // ports: "name[idx]" symbols with a complete index range become one bus
  auto group = [&](const std::vector<std::string>& names, const char* dflt, bool output, auto&& lit_at) {
    std::map<std::string, std::map<uint32_t, Lit>> buses;
    std::vector<std::string> order;
    std::vector<std::pair<std::string, Lit>> scalars;
    for (size_t i = 0; i < names.size(); ++i) {
      std::string nm = names[i].empty() ? dflt + std::to_string(i) : names[i];
      size_t lb = nm.rfind('[');
      if (lb != std::string::npos && lb > 0 && nm.back() == ']' && lb + 2 <= nm.size() - 1 &&
          nm.find_first_not_of("0123456789", lb + 1) == nm.size() - 1) {
        std::string base = nm.substr(0, lb);
        if (!buses.count(base)) order.push_back(base);
        buses[base][(uint32_t)std::stoul(nm.substr(lb + 1))] = lit_at(i);
      } else {
        order.push_back("\x01" + std::to_string(scalars.size()));
        scalars.push_back({nm, lit_at(i)});
      }
    }
    auto add = [&](const std::string& name, aig::Bits bits) {
      aig::Port p;
      p.name = name; p.out = output; p.msb = (int)bits.size() - 1; p.lsb = 0; p.bits = std::move(bits);
      D.ports.push_back(std::move(p));
    };
    for (auto& nm : order) {
      if (nm[0] == '\x01') {
        auto& s = scalars[std::stoul(nm.substr(1))];
        add(s.first, {s.second});
        continue;
      }
      auto& b = buses[nm];
      if (b.rbegin()->first == b.size() - 1) {
        aig::Bits bits;
        for (auto& kv : b) bits.push_back(kv.second);
        add(nm, std::move(bits));
      } else {
        for (auto& kv : b) add(nm + "[" + std::to_string(kv.first) + "]", {kv.second});
      }
    }
  };
  group(in_n, "i", false, [&](size_t i) { return in_lit[i]; });
  if (L) {
    aig::Port p;
    p.name = clock; p.bits = {clk};
    D.ports.push_back(std::move(p));
  }
  group(out_n, "o", true, [&](size_t i) { return tr(out[i]); });
  return res;
}

inline AigerDesign read_aiger_file(const std::string& path, const std::string& clock = "clk") {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  return read_aiger(f, clock);
}

// ----------------------------------------------------------------- checking

// Simulates the Aig and the netlist's LUTs on `rounds` x 64 random patterns
// (inputs and flop outputs free) and compares every output bit and every flop
// D, clock and reset pin. Returns "" or a description of the first mismatch.
inline std::string check_netlist(const Aig& g, const Netlist& n, int rounds = 16, uint64_t seed = 1) {
  const uint32_t nv = g.nvars(), fa = g.first_and();
  std::vector<uint64_t> a(nv), b(n.nbits);
  uint64_t s = seed * 0x9E3779B97F4A7C15ull + 1;
  auto rnd = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
  for (int r = 0; r < rounds; ++r) {
    a[0] = 0;
    for (uint32_t v = 1; v < fa; ++v) a[v] = rnd();
    auto val = [&](Lit l) { return a[lit_var(l)] ^ (lit_neg(l) ? ~0ull : 0); };
    for (uint32_t v = fa; v < nv; ++v) a[v] = val(g.ands[v - fa][0]) & val(g.ands[v - fa][1]);
    std::fill(b.begin(), b.end(), 0);
    b[1] = ~0ull;
    // pads first: a port bit carries the input's value, internal copies follow from the cells
    for (auto& p : g.ports) {
      if (p.output) continue;
      for (auto& np : n.ports)
        if (!np.output && np.name == p.name)
          for (size_t k = 0; k < p.bits.size(); ++k) b[np.bits[k]] = a[lit_var(p.bits[k])];
    }
    for (uint32_t v = 1 + g.nin; v < fa; ++v) b[n.ci_bit[v]] = a[v];
    for (auto& bx : g.boxes) bx.for_each_out([&](uint32_t v) { b[n.ci_bit[v]] = a[v]; });   // chains are cut points
    for (auto& rm : g.rams) for (uint32_t v : rm.q) if (v) b[n.ci_bit[v]] = a[v];
    for (auto& c : n.cells) {
      const std::string& t = c.type;
      if (t == "IBUF" || t == "OBUF") { b[c.conns[1].bits[0]] = b[c.conns[0].bits[0]]; continue; }
      const bool lut = t == "SB_LUT4" || t == "LUT4" || t == "$lut" || (t.size() == 4 && t.compare(0, 3, "LUT") == 0);
      if (!lut) continue;
      if (c.name.compare(0, 3, "add") == 0 && c.name.size() > 4 && c.name.compare(c.name.size() - 4, 4, "_sum") == 0)
        continue;   // the sum LUT of a carry chain: its output is a cut point
      std::vector<int> in;
      int out = -1;
      for (auto& cn : c.conns) {
        if (cn.output) out = cn.bits[0];
        else in.insert(in.end(), cn.bits.begin(), cn.bits.end());
      }
      const std::string* init = nullptr;
      for (auto& p : c.params)
        if (p.name == "LUT_INIT" || p.name == "INIT" || p.name == "LUT") init = &p.value;
      uint64_t o = 0;
      for (size_t mnt = 0; mnt < init->size(); ++mnt) {
        if ((*init)[init->size() - 1 - mnt] != '1') continue;
        uint64_t hit = ~0ull;
        for (size_t k = 0; k < in.size(); ++k) hit &= (mnt >> k & 1) ? b[in[k]] : ~b[in[k]];
        o |= hit;
      }
      b[out] = o;
    }
    for (auto& p : g.ports) {
      if (!p.output) continue;
      for (auto& np : n.ports)
        if (np.output && np.name == p.name)
          for (size_t k = 0; k < p.bits.size(); ++k)
            if (b[np.bits[k]] != val(p.bits[k])) return "output " + p.name + " bit " + std::to_string(k) + " differs";
    }
    for (size_t i = 0; i < g.flops.size(); ++i) {
      const Flop& f = g.flops[i];
      if (b[n.ff_bits[i][0]] != val(f.d)) return "flop " + std::to_string(i) + " D differs";
      if (b[n.ff_bits[i][1]] != val(f.clk)) return "flop " + std::to_string(i) + " clock differs";
      if (b[n.ff_bits[i][2]] != val(f.arst)) return "flop " + std::to_string(i) + " reset differs";
      if (b[n.ff_bits[i][3]] != val(f.en)) return "flop " + std::to_string(i) + " enable differs";
      if (b[n.ff_bits[i][4]] != val(f.srst)) return "flop " + std::to_string(i) + " synchronous reset differs";
    }
    for (size_t k = 0; k < g.boxes.size(); ++k) {
      bool ok = true;
      for (auto& pn : n.box_pins[k]) ok = ok && b[pn.first] == val(pn.second);
      if (!ok) return "carry chain " + std::to_string(k) + " operand differs";
    }
    for (size_t k = 0; k < g.rams.size(); ++k)
      for (auto& pn : n.ram_pins[k])
        if (b[pn.first] != val(pn.second)) return "block RAM " + g.rams[k].name + ": a pin differs";
  }
  return "";
}

}  // namespace fpga
