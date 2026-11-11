// mpnr design database: the flattened gate-level netlist (read by
// gnetlist.hpp) bound to LEF macros, with the floorplan and placement.
#pragma once
#include "def.hpp"
#include "gnetlist.hpp"
#include <functional>
#include <numeric>
#include <set>

namespace pnr {

using phys::Rect;

struct Pin {
  int inst = -1;        // instance index, or -1 for a design port
  int idx = 0;          // index into the macro's pins, or into DB::ios
  int net = -1;
};

struct Inst {
  std::string name;
  const phys::Macro* m = nullptr;
  int64_t x = 0, y = 0;         // lower left corner, dbu
  std::string orient = "N";
  bool fixed = false, placed = false, filler = false;
  std::vector<int> pins;        // pin ids
};

struct Net {
  std::string name;
  std::vector<int> pins;
  bool routed = false;
};

struct Io {
  std::string name, dir = "INPUT";
  int pin = -1;                 // pin id
  std::string bus;              // "a" for a bit of port a[msb:lsb], else empty
  int bit = -1;
  std::string layer;
  Rect rect;                    // absolute shape once placed
  int64_t x = 0, y = 0;
  std::string orient = "N";
  bool placed = false;
};

struct DB {
  phys::Tech tech;
  std::string top;
  std::vector<Inst> insts;
  std::vector<Net> nets;
  std::vector<Pin> pins;
  std::vector<Io> ios;
  // Floorplan, in tech.dbu.
  Rect die, core;
  std::vector<phys::Row> rows;
  std::vector<phys::Blockage> blockages;
  std::vector<phys::Net> specialNets;
  std::map<std::string, std::pair<int, int>> busRange;   // bus port -> msb, lsb

  // The centre of a pin: an instance pin's first port shape, or the port's own.
  void pinPos(int p, int64_t& x, int64_t& y) const {
    const Pin& pn = pins[p];
    if (pn.inst < 0) { const Io& io = ios[pn.idx]; x = (io.rect.x0 + io.rect.x1) / 2; y = (io.rect.y0 + io.rect.y1) / 2; return; }
    const Inst& in = insts[pn.inst];
    const phys::MacroPin& mp = in.m->pins[pn.idx];
    if (mp.ports.empty()) { x = in.x + in.m->w / 2; y = in.y + in.m->h / 2; return; }
    Rect r = phys::xform(mp.ports[0].r, in.orient, in.m->w, in.m->h, in.x, in.y);
    x = (r.x0 + r.x1) / 2; y = (r.y0 + r.y1) / 2;
  }
  // Half-perimeter wire length of every net with two or more pins, dbu.
  int64_t hpwl() const {
    int64_t sum = 0;
    for (auto& n : nets) {
      if (n.pins.size() < 2) continue;
      int64_t x0 = INT64_MAX, x1 = INT64_MIN, y0 = INT64_MAX, y1 = INT64_MIN;
      for (int p : n.pins) {
        int64_t x, y;
        pinPos(p, x, y);
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
      }
      sum += (x1 - x0) + (y1 - y0);
    }
    return sum;
  }
  double um(int64_t v) const { return (double)v / tech.dbu; }
};

struct NetlistOptions {
  std::string top;              // empty: the one module nothing instantiates
  std::string tieHi, tieLo;     // "CELL/PIN"; empty: found in the LEF by name
};

namespace detail {

// One tie cell drives a constant net.
inline void addTie(DB& db, int net, bool hi, const NetlistOptions& opt) {
  std::string cell, pin;
  const std::string& given = hi ? opt.tieHi : opt.tieLo;
  if (!given.empty()) {
    size_t s = given.find('/');
    cell = given.substr(0, s);
    if (s != std::string::npos) pin = given.substr(s + 1);
  } else {
    static const char* hiNames[] = {"LOGIC1", "TIE1", "TIEHI", "TIEH", "conb"};
    static const char* loNames[] = {"LOGIC0", "TIE0", "TIELO", "TIEL", "conb"};
    for (auto& [name, mac] : db.tech.macros) {
      bool match = false;
      for (auto n : hi ? hiNames : loNames) match |= name.find(n) != std::string::npos;
      if (match) { cell = name; break; }
    }
  }
  const phys::Macro* mac = db.tech.macro(cell);
  if (!mac)
    throw std::runtime_error(std::string("the netlist ties pins to constant ") + (hi ? "1" : "0") + " but the LEF has no tie cell" +
                             (cell.empty() ? "" : " '" + cell + "'") + "; name one with " + (hi ? "-tiehi" : "-tielo") + " CELL/PIN");
  int idx = -1;
  for (size_t i = 0; i < mac->pins.size(); i++) {
    const phys::MacroPin& p = mac->pins[i];
    if (p.use != "SIGNAL") continue;
    if (!pin.empty() ? p.name == pin : (p.dir == "OUTPUT" && (idx < 0 || (hi && (p.name == "HI" || p.name == "Z1")) || (!hi && (p.name == "LO" || p.name == "Z0")))))
      idx = (int)i;
  }
  if (idx < 0) throw std::runtime_error("tie cell " + cell + " has no output pin" + (pin.empty() ? "" : " '" + pin + "'"));
  Inst inst;
  inst.name = hi ? "mpnr_tiehi" : "mpnr_tielo";
  inst.m = mac;
  int pid = (int)db.pins.size();
  db.pins.push_back({(int)db.insts.size(), idx, net});
  inst.pins.push_back(pid);
  db.nets[net].pins.push_back(pid);
  db.insts.push_back(inst);
}

}  // namespace detail

// Read structural Verilog files (gnetlist.hpp), flatten them below the top
// module, and bind every leaf instance to its LEF macro. db.tech must already
// hold the macros.
inline void readNetlist(const std::vector<std::string>& files, DB& db, const NetlistOptions& opt = {}) {
  gnl::Source src;
  for (auto& f : files) gnl::read(f, src);
  std::string top = opt.top;
  if (top.empty()) {
    std::vector<std::string> roots = gnl::tops(src);
    if (roots.size() != 1) {
      std::string msg = roots.empty() ? "no top module found" : "several top modules:";
      for (auto& r : roots) msg += " " + r;
      throw std::runtime_error(msg + "; name one with -top");
    }
    top = roots[0];
  }
  // A bus pin of a macro: the LEF pins "P[k]", most significant first.
  auto busPins = [&](const std::string& cell, const std::string& pin, int) {
    std::vector<std::string> out;
    const phys::Macro* m = db.tech.macro(cell);
    if (!m) return out;
    std::vector<std::pair<int, std::string>> bits;
    for (auto& p : m->pins)
      if (p.name.size() > pin.size() + 2 && p.name.compare(0, pin.size(), pin) == 0 && p.name[pin.size()] == '[' && p.name.back() == ']')
        bits.push_back({atoi(p.name.c_str() + pin.size() + 1), p.name});
    std::sort(bits.rbegin(), bits.rend());
    for (auto& b : bits) out.push_back(b.second);
    return out;
  };
  gnl::Netlist nl = gnl::flatten(src, top, busPins);
  db.top = nl.top;
  if (const gnl::Module* tm = src.module(top))
    for (auto& d : tm->decls) if (d.isPort && d.ranged) db.busRange[d.name] = {d.msb, d.lsb};
  db.nets.resize(nl.nets.size());
  for (size_t i = 0; i < nl.nets.size(); i++) db.nets[i].name = nl.nets[i];
  for (auto& p : nl.ports) {
    if (p.net < 0) continue;
    Io io;
    io.name = p.name;
    io.bus = p.bus; io.bit = p.bit;
    io.dir = p.dir == gnl::Dir::In ? "INPUT" : p.dir == gnl::Dir::Out ? "OUTPUT" : "INOUT";
    io.pin = (int)db.pins.size();
    db.pins.push_back({-1, (int)db.ios.size(), p.net});
    db.nets[p.net].pins.push_back(io.pin);
    db.ios.push_back(io);
  }
  for (auto& fi : nl.insts) {
    const phys::Macro* mac = db.tech.macro(fi.cell);
    if (!mac) throw std::runtime_error("instance " + fi.name + ": '" + fi.cell + "' is neither a module in the netlist nor a macro in the LEF");
    if (mac->isFiller()) continue;                 // fillers of an earlier run are made again
    Inst inst;
    inst.name = fi.name;
    inst.m = mac;
    int id = (int)db.insts.size();
    for (auto& c : fi.conns) {
      if (c.net < 0) continue;
      const phys::MacroPin* mp = mac->pin(c.pin);
      if (!mp) throw std::runtime_error("instance " + fi.name + ": macro " + mac->name + " has no pin '" + c.pin + "'");
      if (mp->use == "POWER" || mp->use == "GROUND") continue;      // rails come from the special nets
      int pid = (int)db.pins.size();
      db.pins.push_back({id, (int)(mp - mac->pins.data()), c.net});
      db.nets[c.net].pins.push_back(pid);
      inst.pins.push_back(pid);
    }
    db.insts.push_back(std::move(inst));
  }
  // Constants: a tie cell each, when anything is connected to them.
  if (nl.const1 >= 0 && !db.nets[nl.const1].pins.empty()) detail::addTie(db, nl.const1, true, opt);
  if (nl.const0 >= 0 && !db.nets[nl.const0].pins.empty()) detail::addTie(db, nl.const0, false, opt);
}

}  // namespace pnr
