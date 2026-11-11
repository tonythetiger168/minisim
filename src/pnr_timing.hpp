// mpnr timing: static timing analysis of the placed design with the shared
// engine (sta.hpp, the one behind msta), Liberty libraries and SDC
// constraints. Wires are estimated from the placement: each net's
// capacitance from its half-perimeter length, each driver-to-load delay as
// the Elmore delay of a direct wire, with the resistance and capacitance per
// micron of the LEF's routing layers. The slacks give the net weights of
// timing-driven placement.
#pragma once
#include "pnr_db.hpp"
#include "sta.hpp"

namespace pnr {

struct TimingOptions {
  std::vector<std::string> libs;      // Liberty files
  std::string sdc;                    // constraints
  bool driven = true;                 // weight critical nets in a second placement pass
  int minLayer = 2;                   // wires are taken to run on this routing layer and the next
};

struct TimingResult {
  bool constrained = false;           // the SDC defines a clock
  double wns = 0, tns = 0, holdWns = 0, holdTns = 0;      // ns
  int violations = 0, endpoints = 0, holdViolations = 0;
  struct Skew { std::string clock; double early = 0, late = 0; int pins = 0; };
  std::vector<Skew> skew;             // clock arrival at the registers' clock pins
};

class Timing {
public:
  Timing(DB& d, par::Pool& p, const TimingOptions& o) : db(d), pool(p), opt(o) {}
  bool on() const { return !opt.libs.empty() && !opt.sdc.empty(); }

  void loadLibraries() {
    for (auto& f : opt.libs) libs.push_back(std::make_unique<liberty::Library>(liberty::Library::read(f)));
    wireRc();
  }

  // The timing graph of the design as it is now (call again when cells or
  // nets were added), with the constraints applied. With `propagate` the
  // clocks are timed through their networks (the clock trees are built);
  // without, they are ideal unless the SDC says otherwise.
  void build(bool propagate) {
    nl = gnl::Netlist{};
    nl.top = db.top;
    nl.nets.reserve(db.nets.size());
    for (auto& n : db.nets) nl.nets.push_back(n.name);
    for (auto& io : db.ios) {
      gnl::FlatPort p;
      p.name = io.name;
      p.dir = io.dir == "INPUT" ? gnl::Dir::In : io.dir == "OUTPUT" ? gnl::Dir::Out : gnl::Dir::InOut;
      p.net = io.pin >= 0 ? db.pins[io.pin].net : -1;
      p.bus = io.bus; p.bit = io.bit;
      nl.ports.push_back(p);
    }
    staInst.assign(db.insts.size(), -1);
    for (size_t i = 0; i < db.insts.size(); i++) {
      const Inst& in = db.insts[i];
      if (in.filler || in.pins.empty()) continue;
      gnl::FlatInst fi;
      fi.name = in.name;
      fi.cell = in.m->name;
      for (int p : in.pins) fi.conns.push_back({in.m->pins[db.pins[p].idx].name, db.pins[p].net});
      staInst[i] = (int)nl.insts.size();
      nl.insts.push_back(std::move(fi));
    }
    s = std::make_unique<sta::Sta>();
    for (auto& l : libs) s->addLibrary(*l);
    s->pool = &pool;
    s->build(nl);
    design = std::make_unique<sta::SdcDesign>(*s);
    reader = std::make_unique<sdc::Reader>();
    reader->design = design.get();
    reader->load(opt.sdc);
    s->constrain(reader->sdc);
    if (propagate) for (auto& c : s->clocks) c.propagated = true;
    // db pin -> graph node.
    node.assign(db.pins.size(), -1);
    for (size_t p = 0; p < db.pins.size(); p++) {
      const Pin& pn = db.pins[p];
      if (pn.inst < 0) {
        auto it = s->portByName.find(db.ios[pn.idx].name);
        if (it != s->portByName.end()) node[p] = it->second;
        continue;
      }
      int si = staInst[pn.inst];
      if (si < 0) continue;
      const std::string& name = db.insts[pn.inst].m->pins[pn.idx].name;
      for (int k = s->instNodeOff[si]; k < s->instNodeOff[si + 1]; k++)
        if (*s->nodePin[k] == name) { node[p] = k; break; }
    }
  }

  // Wire capacitances and delays from where the cells are now.
  void annotate() {
    s->spefCap.assign(nl.nets.size(), -1.0);
    s->wireDelay.assign(s->edges.size(), 0.0f);
    pool.chunks(db.nets.size(), 512, [&](size_t lo, size_t hi, size_t) {
      for (size_t ni = lo; ni < hi; ni++) {
        const Net& n = db.nets[ni];
        if (n.pins.size() < 2) { s->spefCap[ni] = 0; continue; }
        int64_t x0 = INT64_MAX, x1 = INT64_MIN, y0 = INT64_MAX, y1 = INT64_MIN, dx = 0, dy = 0;
        int drv = -1;
        for (int p : n.pins) {
          int64_t x, y;
          db.pinPos(p, x, y);
          x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
          if (drv < 0 && node[p] >= 0 && s->nodes[node[p]].driver) { drv = p; dx = x; dy = y; }
        }
        // A tree over many pins is longer than the half perimeter.
        double cnt = (double)n.pins.size();
        double len = db.um((x1 - x0) + (y1 - y0)) * (cnt <= 3 ? 1.0 : std::max(1.0, 0.6 * std::sqrt(cnt - 1)));
        s->spefCap[ni] = len * cUm;
        if (drv < 0) continue;
        for (int p : n.pins) {
          if (p == drv || node[p] < 0) continue;
          int64_t x, y;
          db.pinPos(p, x, y);
          double d = db.um(std::llabs(x - dx) + std::llabs(y - dy));
          double delay = rUm * d * (cUm * d / 2 + s->nodes[node[p]].cap) * 1e-3;      // ohm * pF = 1e-3 ns
          for (int k = s->finOff[node[p]]; k < s->finOff[node[p] + 1]; k++) {
            sta::Edge& e = s->edges[s->finIdx[k]];
            if (e.kind == sta::NetEdge && e.from == node[drv]) s->wireDelay[s->finIdx[k]] = (float)delay;
          }
        }
      }
    });
    s->timed = false;
  }

  TimingResult run() {
    s->update();
    TimingResult r;
    r.constrained = !s->clocks.empty();
    auto late = s->summary(true), early = s->summary(false);
    r.wns = late.total ? late.wns : 0; r.tns = late.tns; r.violations = late.violations; r.endpoints = late.total;
    r.holdWns = early.total ? early.wns : 0; r.holdTns = early.tns; r.holdViolations = early.violations;
    std::vector<TimingResult::Skew> sk(s->clocks.size());
    std::vector<std::unordered_set<int>> seen(s->clocks.size());
    for (auto& pe : s->ends) {
      if (pe.kind != sta::EndKind::Setup || pe.capClk < 0 || pe.clkNode < 0) continue;
      if (!seen[pe.capClk].insert(pe.clkNode).second) continue;
      TimingResult::Skew& k = sk[pe.capClk];
      if (!k.pins++) k.early = k.late = pe.capArr;
      k.early = std::min(k.early, pe.capArr); k.late = std::max(k.late, pe.capArr);
    }
    for (size_t c = 0; c < sk.size(); c++) if (sk[c].pins) { sk[c].clock = s->clocks[c].name; r.skew.push_back(sk[c]); }
    return r;
  }

  // A weight per net of the design, 1 or more: nets on the worst path into
  // an endpoint with little or no setup slack weigh more, up to `top`.
  std::vector<double> netWeights(double top = 5.0) {
    s->update();
    std::vector<double> w(db.nets.size(), 1.0);
    double period = 0;
    for (auto& c : s->clocks) period = std::max(period, c.period);
    double wns = 0;
    for (auto& pe : s->ends) if (sta::isLate(pe.kind)) wns = std::min(wns, pe.slack);
    double margin = 0.1 * period;                 // slack below this counts
    if (margin - wns <= 0) return w;
    for (auto& pe : s->ends) {
      if (!sta::isLate(pe.kind) || pe.slack >= margin || pe.tag < 0) continue;
      double crit = (margin - pe.slack) / (margin - wns);
      double wt = 1.0 + (top - 1.0) * crit * crit;
      for (auto& st : s->trace(pe.node, pe.tag, sta::LATE, pe.rf)) {
        int net = s->nodes[st.node].net;
        if (net >= 0 && net < (int)w.size()) w[net] = std::max(w[net], wt);
      }
    }
    return w;
  }

  double resPerUm() const { return rUm; }
  double capPerUm() const { return cUm; }
  size_t warnings() const { return s ? s->warnings.size() : 0; }

private:
  DB& db;
  par::Pool& pool;
  TimingOptions opt;
  std::vector<std::unique_ptr<liberty::Library>> libs;
  gnl::Netlist nl;
  std::unique_ptr<sta::Sta> s;
  std::unique_ptr<sta::SdcDesign> design;
  std::unique_ptr<sdc::Reader> reader;
  std::vector<int> staInst, node;
  double rUm = 0, cUm = 0;            // ohm and pF per micron of wire

  // The mean of the two routing layers most wires run on.
  void wireRc() {
    auto routing = db.tech.routing();
    int n = 0;
    for (int lv = opt.minLayer; lv <= opt.minLayer + 1 && lv <= (int)routing.size(); lv++) {
      const phys::Layer& l = *routing[lv - 1];
      double w = db.um(l.width);
      if (w <= 0) continue;
      rUm += l.rPerSq > 0 ? l.rPerSq / w : 0;
      cUm += (l.cPerSqDist > 0 ? l.cPerSqDist * w : 0) + (l.edgeCap > 0 ? 2 * l.edgeCap : 0);
      n++;
    }
    if (n) { rUm /= n; cUm /= n; }
  }
};

}  // namespace pnr
