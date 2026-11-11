// mpnr detailed router.
//
// The routing grid has one set of tracks per metal layer, in the layer's
// preferred direction (from the LEF pitch and offset). A node is a point on a
// track where a track of the layer below or above crosses it; wires run along
// tracks only, and a via joins two layers where their tracks cross. Cell pins
// one layer below the lowest routing layer are reached with a via dropped on
// the pin shape.
//
// Every net is a tree grown pin by pin with A* search. Nets negotiate for
// nodes (PathFinder): a node another net claims costs extra, the extra grows
// each round, and nodes that stay contested build up a history cost, until no
// two nets claim the same node. A net claims the nodes it uses and the nodes
// on the same track within the end-of-line spacing, so wires and via pads of
// different nets keep their distance.
//
// Nets whose search windows do not overlap are routed at the same time on
// different threads. The grouping depends only on the design, never on the
// thread count, so the result is identical for any -threads value.
#pragma once
#include <unordered_set>
#include "pnr_place.hpp"
#include <array>
#include <queue>
#include <tuple>

namespace pnr {

struct RouteOptions {
  int minLayer = 2;             // lowest routing level used for wires (1 = first routing layer)
  int maxLayer = 0;             // 0: the top layer
  int maxIter = 40;
  bool verbose = false;
};

struct RouteResult {
  int nets = 0, failed = 0, vias = 0, iterations = 0;
  int64_t wireLength = 0;
  std::vector<int64_t> layerLength;
  std::vector<std::string> layerName;
  std::vector<std::string> failedNets;
};

class Router {
 public:
  Router(DB& d, par::Pool& p, const RouteOptions& o) : db(d), pool(p), opt(o) {}

  RouteResult run() {
    buildGrid();
    buildObstacles();
    buildAccess();
    negotiate();
    return summary();
  }

  phys::Design toDef(bool routed) const;

 private:
  struct GLayer {
    const phys::Layer* L = nullptr;
    bool horiz = false;
    std::vector<int64_t> trk, pos;
    int base = 0, nT = 0, nP = 0;
    std::vector<int> upT, upP, dnT, dnP;   // via landing: track for a position, position for a track
    std::vector<int> hlo, hhi;             // claim window along the track, per position
    const phys::ViaDef* viaUp = nullptr;
    int64_t reach = 0;                     // how far metal of a node extends along the track
    int64_t cutW = 0, cutSp = 0;           // the cut of viaUp: its size and the cut layer's SPACING
  };
  struct NetRoute {
    std::vector<int> nodes;                          // unique grid nodes
    std::vector<std::pair<int, int>> edges;          // wire steps and vias
    std::vector<int> pinNode;                        // per pin of the net: its access node, or -1
    std::vector<int> cuts;                           // lower node of each via
    bool ok = false;
  };
  struct Window { int t0[16], t1[16], p0[16], p1[16], off[17]; };

  DB& db;
  par::Pool& pool;
  RouteOptions opt;
  std::vector<GLayer> gl;
  const phys::Layer* accessLayer = nullptr;          // pins here get a via from layer 0
  std::vector<phys::ViaDef> pinVias;                 // the vias a pin may get: the LEF's, and turned or longer copies
  size_t pinViasLef = 0;                             // the first so many are the LEF's own
  std::vector<int32_t> owner;                        // -1 free, -2 blocked, else the only net allowed
  std::vector<uint16_t> claim;
  std::vector<uint16_t> cutClaim;                    // per lower node: vias of nets whose cut is too close to a via here
  std::vector<uint16_t> hist;
  std::vector<int32_t> pinOwner;                     // net whose pin is reached at or next to this node, or -1
  std::vector<int> accStart, accNode;                // per pin: candidate access nodes
  std::vector<NetRoute> routes;
  std::vector<int> order;                            // nets to route
  int64_t viaCost = 0, unit = 0, pinCost = 0;
  int iterations = 0;
  std::vector<uint16_t> variant;                     // per net: which pin its tree starts from

  static int idxOf(const std::vector<int64_t>& v, int64_t c) {
    auto it = std::lower_bound(v.begin(), v.end(), c);
    return it != v.end() && *it == c ? (int)(it - v.begin()) : -1;
  }
  int node(int g, int t, int p) const { return gl[g].base + t * gl[g].nP + p; }
  void decode(int n, int& g, int& t, int& p) const {
    g = 0;
    while (g + 1 < (int)gl.size() && n >= gl[g + 1].base) g++;
    int r = n - gl[g].base;
    t = r / gl[g].nP; p = r % gl[g].nP;
  }
  void xy(int g, int t, int p, int64_t& x, int64_t& y) const {
    if (gl[g].horiz) { x = gl[g].pos[p]; y = gl[g].trk[t]; } else { x = gl[g].trk[t]; y = gl[g].pos[p]; }
  }

  // The via between two layers whose pads stick out least across each layer's
  // tracks (and then least along them).
  const phys::ViaDef* pickVia(const phys::Layer& lo, const phys::Layer& hi) const {
    const phys::ViaDef* best = nullptr;
    int64_t bestKey = INT64_MAX;
    for (auto& v : db.tech.vias) {
      int64_t perp = 0, along = 0;
      bool a = false, b = false;
      for (auto& s : v.shapes) {
        const phys::Layer* l = s.layer == lo.name ? &lo : s.layer == hi.name ? &hi : nullptr;
        if (!l) continue;
        (l == &lo ? a : b) = true;
        int64_t ex = std::max(-s.r.x0, s.r.x1), ey = std::max(-s.r.y0, s.r.y1);
        perp = std::max(perp, (l->dir == 'H' ? ey : ex) - l->width / 2);
        along += l->dir == 'H' ? ex : ey;
      }
      if (!a || !b) continue;
      int64_t key = std::max<int64_t>(perp, 0) * 1000000 + along * 2 + (v.isDefault ? 0 : 1);
      if (key < bestKey) { bestKey = key; best = &v; }
    }
    return best;
  }
  static int64_t padReach(const phys::ViaDef* v, const phys::Layer& l) {
    int64_t r = l.width / 2;
    if (v) for (auto& s : v->shapes) if (s.layer == l.name) r = std::max(r, l.dir == 'H' ? std::max(-s.r.x0, s.r.x1) : std::max(-s.r.y0, s.r.y1));
    return r;
  }

  void buildGrid() {
    auto routing = db.tech.routing();
    int top = opt.maxLayer > 0 ? std::min<int>(opt.maxLayer, (int)routing.size()) : (int)routing.size();
    if (opt.minLayer < 1 || opt.minLayer >= top) throw std::runtime_error("routing needs at least two layers between -min_layer and -max_layer");
    if (top - opt.minLayer + 1 > 15) top = opt.minLayer + 14;
    for (int lv = opt.minLayer; lv <= top; lv++) {
      GLayer g;
      g.L = routing[lv - 1];
      if (!g.L->dir) throw std::runtime_error("layer " + g.L->name + " has no DIRECTION");
      g.horiz = g.L->dir == 'H';
      if (!gl.empty() && gl.back().horiz == g.horiz) throw std::runtime_error("layers " + gl.back().L->name + " and " + g.L->name + " run the same way; mpnr needs alternating directions");
      g.trk = layerTracks(*g.L, db.die);
      if (g.trk.empty()) throw std::runtime_error("layer " + g.L->name + " has no tracks inside the die");
      gl.push_back(g);
    }
    if (opt.minLayer >= 2) {
      accessLayer = routing[opt.minLayer - 2];
      if ((accessLayer->dir == 'H') == gl[0].horiz) throw std::runtime_error("layers " + accessLayer->name + " and " + gl[0].L->name + " run the same way");
    }
    int base = 0;
    for (size_t g = 0; g < gl.size(); g++) {
      GLayer& l = gl[g];
      if (g > 0) l.pos.insert(l.pos.end(), gl[g - 1].trk.begin(), gl[g - 1].trk.end());
      if (g + 1 < gl.size()) l.pos.insert(l.pos.end(), gl[g + 1].trk.begin(), gl[g + 1].trk.end());
      if (g == 0 && accessLayer) { auto t = layerTracks(*accessLayer, db.die); l.pos.insert(l.pos.end(), t.begin(), t.end()); }
      std::sort(l.pos.begin(), l.pos.end());
      l.pos.erase(std::unique(l.pos.begin(), l.pos.end()), l.pos.end());
      l.nT = (int)l.trk.size(); l.nP = (int)l.pos.size();
      l.base = base;
      if ((int64_t)base + (int64_t)l.nT * l.nP > INT32_MAX) throw std::runtime_error("the routing grid is too large");
      base += l.nT * l.nP;
    }
    for (size_t g = 0; g < gl.size(); g++) {
      GLayer& l = gl[g];
      if (g + 1 < gl.size()) {
        l.upT.resize(l.nP); l.upP.resize(l.nT);
        for (int p = 0; p < l.nP; p++) l.upT[p] = idxOf(gl[g + 1].trk, l.pos[p]);
        for (int t = 0; t < l.nT; t++) l.upP[t] = idxOf(gl[g + 1].pos, l.trk[t]);
        l.viaUp = pickVia(*l.L, *gl[g + 1].L);
        if (!l.viaUp) throw std::runtime_error("the LEF has no via between " + l.L->name + " and " + gl[g + 1].L->name);
        for (auto& s : l.viaUp->shapes) {
          const phys::Layer* cl = db.tech.layer(s.layer);
          if (cl && cl->type == phys::Layer::Cut) { l.cutW = std::max(s.r.x1 - s.r.x0, s.r.y1 - s.r.y0); l.cutSp = cl->spacing; }
        }
      }
      if (g > 0) {
        l.dnT.resize(l.nP); l.dnP.resize(l.nT);
        for (int p = 0; p < l.nP; p++) l.dnT[p] = idxOf(gl[g - 1].trk, l.pos[p]);
        for (int t = 0; t < l.nT; t++) l.dnP[t] = idxOf(gl[g - 1].pos, l.trk[t]);
      }
    }
    for (size_t g = 0; g < gl.size(); g++) {
      GLayer& l = gl[g];
      l.reach = l.L->width / 2;
      if (g + 1 < gl.size()) l.reach = std::max(l.reach, padReach(l.viaUp, *l.L));
      if (g > 0) l.reach = std::max(l.reach, padReach(gl[g - 1].viaUp, *l.L));
      if (g == 0 && accessLayer) for (auto& v : db.tech.vias) {
        bool lo = false, hi = false;
        for (auto& s : v.shapes) { lo |= s.layer == accessLayer->name; hi |= s.layer == l.L->name; }
        if (lo && hi && accessVia(v)) l.reach = std::max(l.reach, padReach(&v, *l.L));
      }
      // A short piece of wire is made longer to meet the layer's minimum
      // area, so every node keeps room for that.
      l.reach = std::max(l.reach, minHalf(*l.L));
      // Two nets on one track need their metal a spacing apart.
      int64_t d = 2 * l.reach + l.L->spacing;
      l.hlo.resize(l.nP); l.hhi.resize(l.nP);
      int lo = 0, hi = 0;
      for (int p = 0; p < l.nP; p++) {
        while (l.pos[p] - l.pos[lo] >= d) lo++;
        if (hi < p) hi = p;
        while (hi + 1 < l.nP && l.pos[hi + 1] - l.pos[p] < d) hi++;
        l.hlo[p] = lo; l.hhi[p] = hi;
      }
    }
    buildPinVias();
    owner.assign(base, -1);
    claim.assign(base, 0);
    cutClaim.assign(base, 0);
    hist.assign(base, 0);
    viaCost = 2 * gl[0].L->pitch();
    unit = 4 * viaCost;
    pinCost = viaCost;
  }

  // Half the length a wire of the layer's width needs for the minimum area,
  // rounded up to the manufacturing grid; 0 when the layer has no AREA rule.
  int64_t minHalf(const phys::Layer& l) const {
    if (l.minArea <= 0 || l.width <= 0) return 0;
    int64_t len = (l.minArea + l.width - 1) / l.width, grid = std::max<int64_t>(db.tech.mfgGrid, 1);
    return ((len + 1) / 2 + grid - 1) / grid * grid;
  }

  // The pin vias: each access via of the LEF, then a copy with the lower pad
  // turned a quarter, then copies of both with the lower pad made long enough
  // for the lower layer's minimum area. A pin too small for the LEF's pad
  // gets the copy that suits it; mpnr writes the copies it used to the DEF.
  void buildPinVias() {
    pinVias.clear();
    if (!accessLayer) return;
    for (auto& v : db.tech.vias) {
      bool lo = false, hi = false;
      for (auto& sh : v.shapes) { lo |= sh.layer == accessLayer->name; hi |= sh.layer == gl[0].L->name; }
      if (lo && hi && accessVia(v)) pinVias.push_back(v);
    }
    pinViasLef = pinVias.size();
    auto lower = [&](const phys::ViaDef& v) { Rect r{0, 0, 0, 0}; for (auto& sh : v.shapes) if (sh.layer == accessLayer->name) r = sh.r; return r; };
    auto same = [&](const Rect& r) {
      for (auto& v : pinVias) { Rect q = lower(v); if (q.x0 == r.x0 && q.y0 == r.y0 && q.x1 == r.x1 && q.y1 == r.y1) return true; }
      return false;
    };
    auto add = [&](const phys::ViaDef& from, const Rect& r, const char* tag) {
      if (same(r)) return;
      phys::ViaDef v = from;
      v.name = "mpnr_" + from.name + tag;
      v.isDefault = false;
      bool first = true;
      for (auto it = v.shapes.begin(); it != v.shapes.end();)
        if (it->layer != accessLayer->name) ++it;
        else if (first) { it->r = r; first = false; ++it; }
        else it = v.shapes.erase(it);
      pinVias.push_back(std::move(v));
    };
    int64_t grid = std::max<int64_t>(db.tech.mfgGrid, 1);
    auto longer = [&](Rect r) {
      int64_t w = r.w(), h = r.h();
      if (accessLayer->minArea <= 0 || w * h >= accessLayer->minArea) return r;
      bool alongX = w >= h;
      int64_t half = (((accessLayer->minArea + std::min(w, h) - 1) / std::min(w, h) + 1) / 2 + grid - 1) / grid * grid;
      if (alongX) { r.x0 = std::min(r.x0, -half); r.x1 = std::max(r.x1, half); } else { r.y0 = std::min(r.y0, -half); r.y1 = std::max(r.y1, half); }
      return r;
    };
    for (size_t i = 0; i < pinViasLef; i++) {
      phys::ViaDef v = pinVias[i];
      Rect r = lower(v), t{r.y0, r.x0, r.y1, r.x1};
      add(v, t, "_r");
      add(v, longer(r), "_l");
      add(v, longer(t), "_rl");
    }
  }

  // Vias usable for pin access: the top pad must not be wider than the wire
  // across the tracks of the first routing layer.
  bool accessVia(const phys::ViaDef& v) const {
    const phys::Layer& l = *gl[0].L;
    for (auto& s : v.shapes)
      if (s.layer == l.name) {
        int64_t perp = l.dir == 'H' ? std::max(-s.r.y0, s.r.y1) : std::max(-s.r.x0, s.r.x1);
        if (perp > l.width / 2) return false;
      }
    return true;
  }

  int gridLayer(const std::string& name) const {
    for (size_t g = 0; g < gl.size(); g++) if (gl[g].L->name == name) return (int)g;
    return -1;
  }

  // Marks the nodes whose metal would come within a spacing of `r` on layer g:
  // owner `net` (>= 0) keeps them for that net, -2 blocks them for all.
  void mark(int g, const Rect& r, int net) {
    const GLayer& l = gl[g];
    int64_t along = l.reach + l.L->spacing, perp = l.L->width / 2 + l.L->spacing;
    int64_t tlo = (l.horiz ? r.y0 : r.x0) - perp, thi = (l.horiz ? r.y1 : r.x1) + perp;
    int64_t plo = (l.horiz ? r.x0 : r.y0) - along, phi = (l.horiz ? r.x1 : r.y1) + along;
    int t0 = (int)(std::upper_bound(l.trk.begin(), l.trk.end(), tlo) - l.trk.begin());
    int t1 = (int)(std::lower_bound(l.trk.begin(), l.trk.end(), thi) - l.trk.begin());
    int p0 = (int)(std::upper_bound(l.pos.begin(), l.pos.end(), plo) - l.pos.begin());
    int p1 = (int)(std::lower_bound(l.pos.begin(), l.pos.end(), phi) - l.pos.begin());
    for (int t = t0; t < t1; t++)
      for (int p = p0; p < p1; p++) {
        int32_t& o = owner[node(g, t, p)];
        if (net == -2 || (o != -1 && o != net)) o = -2; else o = net;
      }
  }

  void buildObstacles() {
    // Pin shapes on routing layers keep their surroundings for their own net;
    // everything else on a routing layer blocks.
    std::vector<int> pinNet;
    for (size_t i = 0; i < db.insts.size(); i++) {
      const Inst& in = db.insts[i];
      bool anyRouted = false;
      for (auto& p : in.m->pins) for (auto& s : p.ports) anyRouted |= gridLayer(s.layer) >= 0;
      for (auto& s : in.m->obs) anyRouted |= gridLayer(s.layer) >= 0;
      if (!anyRouted) continue;
      pinNet.assign(in.m->pins.size(), -2);
      for (int pid : in.pins) pinNet[db.pins[pid].idx] = db.pins[pid].net;
      for (size_t k = 0; k < in.m->pins.size(); k++)
        for (auto& s : in.m->pins[k].ports) {
          int g = gridLayer(s.layer);
          if (g >= 0) mark(g, phys::xform(s.r, in.orient, in.m->w, in.m->h, in.x, in.y), pinNet[k]);
        }
      for (auto& s : in.m->obs) {
        int g = gridLayer(s.layer);
        if (g >= 0) mark(g, phys::xform(s.r, in.orient, in.m->w, in.m->h, in.x, in.y), -2);
      }
    }
    for (auto& io : db.ios) {
      int g = gridLayer(io.layer);
      if (g >= 0) mark(g, io.rect, db.pins[io.pin].net);
    }
    for (auto& b : db.blockages) {
      int g = gridLayer(b.layer);
      if (g >= 0) mark(g, b.r, -2);
    }
    for (auto& sn : db.specialNets)
      for (auto& s : sn.segs) {
        int g = gridLayer(s.layer);
        if (g < 0) continue;
        int64_t hw = s.width / 2;
        Rect r{std::min(s.x0, s.x1) - hw, std::min(s.y0, s.y1) - hw, std::max(s.x0, s.x1) + hw, std::max(s.y0, s.y1) + hw};
        mark(g, r, -2);
      }
    // The via stacks under the straps.
    for (auto& sn : db.specialNets)
      for (auto& v : sn.vias) {
        const phys::ViaDef* vd = db.tech.via(v.via);
        if (!vd) continue;
        for (auto& sh : vd->shapes) {
          int g = gridLayer(sh.layer);
          if (g >= 0) mark(g, {sh.r.x0 + v.x, sh.r.y0 + v.y, sh.r.x1 + v.x, sh.r.y1 + v.y}, -2);
        }
      }
  }

  // The grid nodes from which a pin can be reached.
  void pinShapes(int pid, std::vector<phys::LayerRect>& out) const {
    out.clear();
    const Pin& pn = db.pins[pid];
    if (pn.inst < 0) { const Io& io = db.ios[pn.idx]; out.push_back({io.layer, io.rect}); return; }
    const Inst& in = db.insts[pn.inst];
    for (auto& s : in.m->pins[pn.idx].ports) out.push_back({s.layer, phys::xform(s.r, in.orient, in.m->w, in.m->h, in.x, in.y)});
  }

  void buildAccess() {
    size_t np = db.pins.size();
    std::vector<std::vector<int>> acc(np);
    pool.chunks(np, 512, [&](size_t lo, size_t hi, size_t) {
      std::vector<phys::LayerRect> shapes;
      for (size_t pid = lo; pid < hi; pid++) {
        int net = db.pins[pid].net;
        if (net < 0 || db.nets[net].pins.size() < 2) continue;
        pinShapes((int)pid, shapes);
        std::vector<int>& a = acc[pid];
        std::vector<int> loose;
        for (auto& s : shapes) {
          int g = gridLayer(s.layer);
          bool below = g < 0 && accessLayer && s.layer == accessLayer->name;
          if (g < 0 && !below) continue;
          if (below) g = 0;
          const GLayer& l = gl[g];
          const Rect& r = s.r;
          int t0 = (int)(std::lower_bound(l.trk.begin(), l.trk.end(), l.horiz ? r.y0 : r.x0) - l.trk.begin());
          int t1 = (int)(std::upper_bound(l.trk.begin(), l.trk.end(), l.horiz ? r.y1 : r.x1) - l.trk.begin());
          int p0 = (int)(std::lower_bound(l.pos.begin(), l.pos.end(), l.horiz ? r.x0 : r.y0) - l.pos.begin());
          int p1 = (int)(std::upper_bound(l.pos.begin(), l.pos.end(), l.horiz ? r.x1 : r.y1) - l.pos.begin());
          for (int t = t0; t < t1; t++)
            for (int p = p0; p < p1; p++) {
              int n = node(g, t, p);
              if (owner[n] != -1 && owner[n] != net) continue;
              if (!below) { a.push_back(n); continue; }
              // A via pad that fits inside the pin shape is preferred.
              int64_t x, y;
              xy(g, t, p, x, y);
              (fitsVia(r, x, y) ? a : loose).push_back(n);
            }
        }
        if (a.empty()) a = loose;
        std::sort(a.begin(), a.end());
        a.erase(std::unique(a.begin(), a.end()), a.end());
      }
    });
    accStart.assign(np + 1, 0);
    for (size_t i = 0; i < np; i++) accStart[i + 1] = accStart[i] + (int)acc[i].size();
    accNode.resize(accStart[np]);
    for (size_t i = 0; i < np; i++) std::copy(acc[i].begin(), acc[i].end(), accNode.begin() + accStart[i]);
    // Wires of other nets are steered away from where a pin is reached, and
    // from the nodes its via pad needs clear.
    pinOwner.assign(owner.size(), -1);
    for (size_t i = 0; i < np; i++)
      for (int n : acc[i]) {
        int g, t, p;
        decode(n, g, t, p);
        const GLayer& l = gl[g];
        int b = l.base + t * l.nP;
        for (int q = l.hlo[p]; q <= l.hhi[p]; q++) if (pinOwner[b + q] == -1) pinOwner[b + q] = db.pins[i].net;
      }
  }

  // The pin via whose lower pad fits inside `r` at (x, y); or else the one
  // that, with the pin shape, has the lower layer's minimum area and sticks
  // out least.
  const phys::ViaDef* pinVia(const Rect& r, int64_t x, int64_t y, bool* fits = nullptr) const {
    const phys::ViaDef* best = nullptr;
    int64_t bestOut = INT64_MAX;
    for (auto& v : pinVias) {
      int64_t out = 0;
      for (auto& s : v.shapes) {
        if (s.layer != accessLayer->name) continue;
        Rect q{x + s.r.x0, y + s.r.y0, x + s.r.x1, y + s.r.y1};
        out += std::max<int64_t>(0, r.x0 - q.x0) + std::max<int64_t>(0, q.x1 - r.x1) +
               std::max<int64_t>(0, r.y0 - q.y0) + std::max<int64_t>(0, q.y1 - r.y1);
        if (out && accessLayer->minArea > 0) {
          int64_t ow = std::min(r.x1, q.x1) - std::max(r.x0, q.x0), oh = std::min(r.y1, q.y1) - std::max(r.y0, q.y0);
          int64_t area = r.w() * r.h() + q.w() * q.h() - (ow > 0 && oh > 0 ? ow * oh : 0);
          if (area < accessLayer->minArea) out += INT64_C(1) << 40;
        }
      }
      if (out < bestOut) { bestOut = out; best = &v; }
    }
    if (fits) *fits = best && bestOut == 0;
    return best;
  }
  bool fitsVia(const Rect& r, int64_t x, int64_t y) const { bool f = false; pinVia(r, x, y, &f); return f; }

  // ---- claims ----
  void addClaims(const NetRoute& r, int delta) {
    int g, t, p;
    for (int n : r.nodes) {
      decode(n, g, t, p);
      const GLayer& l = gl[g];
      int b = l.base + t * l.nP;
      for (int q = l.hlo[p]; q <= l.hhi[p]; q++) claim[b + q] = (uint16_t)(claim[b + q] + delta);
    }
    for (int c : r.cuts) cutNear(c, [&](int n) { cutClaim[n] = (uint16_t)(cutClaim[n] + delta); });
  }

  // ---- search ----
  struct Scratch {
    std::vector<int64_t> dist;
    std::vector<int32_t> from, stamp, tstamp;
    int32_t cur = 0;
    std::unordered_set<int> cuts;          // the net being routed: lower nodes too close to one of its vias
    std::vector<int> banned;               // lower nodes no via may use in this search
    void reset(size_t n) {
      if (dist.size() < n) { dist.resize(n); from.resize(n); stamp.assign(n, 0); tstamp.assign(n, 0); cur = 0; }
      if (++cur == INT32_MAX) { std::fill(stamp.begin(), stamp.end(), 0); std::fill(tstamp.begin(), tstamp.end(), 0); cur = 1; }
    }
  };
  struct Box { int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
    void add(int64_t x, int64_t y) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    void grow(int64_t m) { x0 -= m; y0 -= m; x1 += m; y1 += m; }
    bool has(int64_t x, int64_t y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  };

  bool makeWindow(const Box& b, Window& w) const {
    int off = 0;
    for (size_t g = 0; g < gl.size(); g++) {
      const GLayer& l = gl[g];
      int64_t tlo = l.horiz ? b.y0 : b.x0, thi = l.horiz ? b.y1 : b.x1, plo = l.horiz ? b.x0 : b.y0, phi = l.horiz ? b.x1 : b.y1;
      w.t0[g] = (int)(std::lower_bound(l.trk.begin(), l.trk.end(), tlo) - l.trk.begin());
      w.t1[g] = (int)(std::upper_bound(l.trk.begin(), l.trk.end(), thi) - l.trk.begin()) - 1;
      w.p0[g] = (int)(std::lower_bound(l.pos.begin(), l.pos.end(), plo) - l.pos.begin());
      w.p1[g] = (int)(std::upper_bound(l.pos.begin(), l.pos.end(), phi) - l.pos.begin()) - 1;
      w.off[g] = off;
      int64_t cnt = (int64_t)std::max(0, w.t1[g] - w.t0[g] + 1) * std::max(0, w.p1[g] - w.p0[g] + 1);
      if (off + cnt > INT32_MAX / 2) return false;
      off += (int)cnt;
    }
    w.off[gl.size()] = off;
    return true;
  }
  int local(const Window& w, int g, int t, int p) const {
    if (t < w.t0[g] || t > w.t1[g] || p < w.p0[g] || p > w.p1[g]) return -1;
    return w.off[g] + (t - w.t0[g]) * (w.p1[g] - w.p0[g] + 1) + (p - w.p0[g]);
  }
  void unlocal(const Window& w, int lid, int& g, int& t, int& p) const {
    g = 0;
    while (lid >= w.off[g + 1]) g++;
    int r = lid - w.off[g], np = w.p1[g] - w.p0[g] + 1;
    t = w.t0[g] + r / np; p = w.p0[g] + r % np;
  }

  // Two cuts of the layer above grid layer g, at (t, p) and (t2, p2) of g:
  // closer than the cut layer's SPACING?
  bool cutClash(int g, int t, int p, int t2, int p2) const {
    const GLayer& l = gl[g];
    int64_t a = std::llabs(l.trk[t] - l.trk[t2]) - l.cutW, b = std::llabs(l.pos[p] - l.pos[p2]) - l.cutW;
    if (a < 0 && b < 0) return false;                  // the same cut
    a = std::max<int64_t>(a, 0); b = std::max<int64_t>(b, 0);
    return a < l.cutSp && b < l.cutSp && (!a || !b || a * a + b * b < l.cutSp * l.cutSp);
  }
  // Calls fn(node) for the lower nodes where a via would be too close to
  // the cut of a via up from `lo`.
  template <class F> void cutNear(int lo, F&& fn) const {
    int g, t, p;
    decode(lo, g, t, p);
    const GLayer& l = gl[g];
    if (!l.cutSp) return;
    int64_t need = l.cutW + l.cutSp;
    int t0 = t, t1 = t, p0 = p, p1 = p;
    while (t0 > 0 && l.trk[t] - l.trk[t0 - 1] < need) t0--;
    while (t1 + 1 < l.nT && l.trk[t1 + 1] - l.trk[t] < need) t1++;
    while (p0 > 0 && l.pos[p] - l.pos[p0 - 1] < need) p0--;
    while (p1 + 1 < l.nP && l.pos[p1 + 1] - l.pos[p] < need) p1++;
    for (int tt = t0; tt <= t1; tt++)
      for (int pp = p0; pp <= p1; pp++)
        if (cutClash(g, t, p, tt, pp)) fn(node(g, tt, pp));
  }
  // A via from the node (g, t, p) at local id `lid` towards layer gd, when the
  // path came out of gd a step or two back along this track: are the two cuts
  // too close? (Other shapes of the same mistake are caught by pathCutClash.)
  bool jogBlocked(int g, int p, int gd, int lid, const Window& w, const Scratch& sc) const {
    const GLayer& c = gl[std::min(g, gd)];
    if (!c.cutSp) return false;
    const GLayer& l = gl[g];
    int at = lid;
    for (int k = 0; k < 4; k++) {
      int prev = sc.from[at];
      if (prev < 0) return false;
      if (prev < w.off[g] || prev >= w.off[g + 1]) return false;      // came in by a via here: the same cut or another layer
      int pp = p + (prev - lid);
      if (std::llabs(l.pos[p] - l.pos[pp]) - c.cutW >= c.cutSp) return false;
      int f = sc.from[prev];
      if (f >= 0 && f >= w.off[gd] && f < w.off[gd + 1]) return true;
      at = prev;
    }
    return false;
  }

  // The lower node of a via of the path that is too close to an earlier via
  // of the same path, or -1.
  int pathCutClash(const std::vector<int>& path) const {
    struct V { int g, t, p, n; };
    std::vector<V> vs;
    int g, t, p;
    for (size_t i = 0; i + 1 < path.size(); i++) {
      int lo = std::min(path[i], path[i + 1]), hi = std::max(path[i], path[i + 1]);
      decode(lo, g, t, p);
      if (g + 1 >= (int)gl.size() || hi < gl[g + 1].base) continue;
      for (size_t k = vs.size(); k-- > 0 && vs.size() - k <= 6;)
        if (vs[k].g == g && gl[g].cutSp && cutClash(g, t, p, vs[k].t, vs[k].p)) return lo;
      vs.push_back({g, t, p, lo});
    }
    return -1;
  }

  // A* from the given source nodes to any of the target nodes, inside the
  // window. Appends the path (target first, source last) to `path`.
  bool search(int net, const std::vector<int>& sources, const std::vector<int>& targets, const Window& w, int64_t pres, int64_t hw, Scratch& sc,
              std::vector<int>& path) const {
    sc.reset((size_t)w.off[gl.size()]);
    Box tb;
    int gmin = 99, gmax = -1;
    int g, t, p;
    bool anyTarget = false;
    for (int n : targets) {
      decode(n, g, t, p);
      int lid = local(w, g, t, p);
      if (lid < 0) continue;
      sc.tstamp[lid] = sc.cur;
      int64_t x, y;
      xy(g, t, p, x, y);
      tb.add(x, y);
      gmin = std::min(gmin, g); gmax = std::max(gmax, g);
      anyTarget = true;
    }
    if (!anyTarget) return false;
    auto heur = [&](int gg, int tt, int pp) {
      int64_t x, y;
      xy(gg, tt, pp, x, y);
      int64_t dx = x < tb.x0 ? tb.x0 - x : x > tb.x1 ? x - tb.x1 : 0, dy = y < tb.y0 ? tb.y0 - y : y > tb.y1 ? y - tb.y1 : 0;
      int dl = gg < gmin ? gmin - gg : gg > gmax ? gg - gmax : 0;
      // On the targets' own layer but off their track: up and down again.
      if (!dl && gmin == gmax && (gl[gg].horiz ? dy : dx)) dl = 2;
      return dx + dy + dl * viaCost;
    };
    using Item = std::pair<int64_t, int32_t>;      // f, local id
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    for (int n : sources) {
      decode(n, g, t, p);
      int lid = local(w, g, t, p);
      if (lid < 0) continue;
      if (sc.stamp[lid] == sc.cur) continue;
      sc.stamp[lid] = sc.cur; sc.dist[lid] = 0; sc.from[lid] = -1;
      { int64_t h = heur(g, t, p); heap.push({(h << 24) | std::min<int64_t>(h, 0xffffff), lid}); }
    }
    while (!heap.empty()) {
      auto [key, lid] = heap.top();
      heap.pop();
      unlocal(w, lid, g, t, p);
      int64_t d = sc.dist[lid];
      if ((key >> 24) > d + heur(g, t, p) * hw / 8) continue;        // stale
      if (sc.tstamp[lid] == sc.cur) {
        for (int at = lid; at >= 0; at = sc.from[at]) {
          int a, b, c;
          unlocal(w, at, a, b, c);
          path.push_back(node(a, b, c));
        }
        return true;
      }
      const GLayer& l = gl[g];
      auto relax = [&](int g2, int t2, int p2, int64_t step, int cutLow = -1) {
        int l2 = local(w, g2, t2, p2);
        if (l2 < 0) return;
        int n2 = node(g2, t2, p2);
        int32_t o = owner[n2];
        if (o != -1 && o != net) return;
        int32_t po = pinOwner[n2];
        int64_t nd = d + step + (claim[n2] ? pres : 0) + (int64_t)hist[n2] * unit / 4 + (po != -1 && po != net ? pinCost : 0);
        if (sc.stamp[l2] == sc.cur && sc.dist[l2] <= nd) return;
        if (cutLow >= 0 && ((!sc.cuts.empty() && sc.cuts.count(cutLow)) || (!sc.banned.empty() && std::find(sc.banned.begin(), sc.banned.end(), cutLow) != sc.banned.end()))) return;
        sc.stamp[l2] = sc.cur; sc.dist[l2] = nd; sc.from[l2] = lid;
        int64_t h = heur(g2, t2, p2);
        heap.push({((nd + h * hw / 8) << 24) | std::min<int64_t>(h, 0xffffff), l2});
      };
      if (p > 0) relax(g, t, p - 1, l.pos[p] - l.pos[p - 1]);
      if (p + 1 < l.nP) relax(g, t, p + 1, l.pos[p + 1] - l.pos[p]);
      if (g + 1 < (int)gl.size() && l.upT[p] >= 0 && !jogBlocked(g, p, g + 1, lid, w, sc)) { int lo = node(g, t, p); relax(g + 1, l.upT[p], l.upP[t], viaCost + (cutClaim[lo] ? pres : 0), lo); }
      if (g > 0 && l.dnT[p] >= 0 && !jogBlocked(g, p, g - 1, lid, w, sc)) { int lo = node(g - 1, l.dnT[p], l.dnP[t]); relax(g - 1, l.dnT[p], l.dnP[t], viaCost + (cutClaim[lo] ? pres : 0), lo); }
    }
    return false;
  }

  // The order in which a net's pins join the tree, with the pin each one is
  // closest to (Prim's algorithm on pin positions; a sweep for huge nets).
  void pinOrder(const Net& nt, int start, std::vector<std::pair<int, int>>& out) const {
    size_t n = nt.pins.size();
    std::vector<int64_t> px(n), py(n);
    for (size_t i = 0; i < n; i++) db.pinPos(nt.pins[i], px[i], py[i]);
    out.clear();
    if (n > 3000) {
      std::vector<int> idx(n);
      std::iota(idx.begin(), idx.end(), 0);
      // Snake through horizontal bands so consecutive pins are close.
      int64_t band = std::max<int64_t>(1, db.tech.coreSite() ? 8 * db.tech.coreSite()->h : 1);
      std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        int64_t ba = py[a] / band, bb = py[b] / band;
        if (ba != bb) return ba < bb;
        if (px[a] != px[b]) return (ba & 1) ? px[a] > px[b] : px[a] < px[b];
        return a < b;
      });
      out.push_back({idx[0], -1});
      for (size_t i = 1; i < n; i++) out.push_back({idx[i], idx[i - 1]});
      return;
    }
    std::vector<int64_t> best(n, INT64_MAX);
    std::vector<int> par(n, -1);
    std::vector<char> in(n, 0);
    int cur = start % (int)n;
    in[cur] = 1;
    out.push_back({cur, -1});
    for (size_t k = 1; k < n; k++) {
      int nxt = -1;
      for (size_t i = 0; i < n; i++) {
        if (in[i]) continue;
        int64_t d = std::llabs(px[i] - px[cur]) + std::llabs(py[i] - py[cur]);
        if (d < best[i]) { best[i] = d; par[i] = cur; }
        if (nxt < 0 || best[i] < best[nxt]) nxt = (int)i;
      }
      in[nxt] = 1;
      out.push_back({nxt, par[nxt]});
      cur = nxt;
    }
  }

  Box netBox(int net) const {
    Box b;
    for (int pid : db.nets[net].pins) {
      for (int k = accStart[pid]; k < accStart[pid + 1]; k++) {
        int g, t, p;
        int64_t x, y;
        decode(accNode[k], g, t, p);
        xy(g, t, p, x, y);
        b.add(x, y);
      }
    }
    return b;
  }
  int64_t margin(int iter) const { return (int64_t)(6 + 4 * std::min(iter, 12)) * gl[0].L->pitch() * 2; }
  int64_t haloReach() const {
    int64_t m = 0;
    for (auto& l : gl) m = std::max({m, 2 * l.reach + l.L->spacing, l.cutW + l.cutSp});
    return m;
  }

  // Routes one net from scratch. The net's old claims must already be gone.
  bool routeNet(int net, int iter, int64_t pres, Scratch& sc, NetRoute& out) const {
    const Net& nt = db.nets[net];
    out = NetRoute{};
    out.pinNode.assign(nt.pins.size(), -1);
    if (sc.cuts.bucket_count() > 512) sc.cuts = {}; else sc.cuts.clear();      // clear() walks every bucket
    for (int pid : nt.pins) if (accStart[pid] == accStart[pid + 1]) return false;      // a pin nothing can reach
    std::vector<std::pair<int, int>> ord;
    pinOrder(nt, variant[net], ord);
    Box limit = netBox(net);
    limit.grow(margin(iter));
    struct Piece { size_t begin, end; Box box; };
    std::vector<Piece> pieces;                      // tree nodes by the connection that added them
    std::vector<int> sources, targets, path;
    std::vector<char> seen;
    int64_t m = margin(iter);
    for (size_t k = 1; k < ord.size(); k++) {
      int pi = ord[k].first, par = ord[k].second;
      int pid = nt.pins[pi];
      targets.assign(accNode.begin() + accStart[pid], accNode.begin() + accStart[pid + 1]);
      // Window: the target pin, the pin it hangs from, and some room.
      Box wb;
      int g, t, p;
      int64_t x, y;
      for (int n : targets) { decode(n, g, t, p); xy(g, t, p, x, y); wb.add(x, y); }
      sources.clear();
      if (k == 1) {
        int p0 = nt.pins[ord[0].first];
        sources.assign(accNode.begin() + accStart[p0], accNode.begin() + accStart[p0 + 1]);
        for (int n : sources) { decode(n, g, t, p); xy(g, t, p, x, y); wb.add(x, y); }
      } else {
        decode(out.pinNode[par], g, t, p);
        xy(g, t, p, x, y);
        wb.add(x, y);
      }
      wb.grow(m);
      wb.x0 = std::max(wb.x0, limit.x0); wb.y0 = std::max(wb.y0, limit.y0); wb.x1 = std::min(wb.x1, limit.x1); wb.y1 = std::min(wb.y1, limit.y1);
      if (k > 1)
        for (auto& pc : pieces) {
          if (pc.box.x1 < wb.x0 || pc.box.x0 > wb.x1 || pc.box.y1 < wb.y0 || pc.box.y0 > wb.y1) continue;
          for (size_t i = pc.begin; i < pc.end; i++) sources.push_back(out.nodes[i]);
        }
      Window w;
      if (!makeWindow(wb, w)) return false;
      // The first rounds lean on the distance estimate (10/8 of it): far fewer
      // nodes searched for slightly longer wires. Later rounds search exactly.
      // The search keeps one way into each node, so now and then the path it
      // returns has two of its own vias too close; that via is banned and the
      // search run again.
      sc.banned.clear();
      for (int tries = 0;; tries++) {
        path.clear();
        if (!search(net, sources, targets, w, pres, iter < 2 ? 10 : 8, sc, path)) return false;
        int bad = pathCutClash(path);
        if (bad < 0) break;
        if (tries == 8) return false;
        sc.banned.push_back(bad);
      }
      // path: target ... source.
      out.pinNode[pi] = path.front();
      if (k == 1) out.pinNode[ord[0].first] = path.back();
      Piece pc;
      pc.begin = out.nodes.size();
      // The source end is already in the tree (except on the first connection).
      size_t fresh = k == 1 ? path.size() : path.size() - 1;
      for (size_t i = 0; i < fresh; i++) {
        out.nodes.push_back(path[i]);
        decode(path[i], g, t, p);
        xy(g, t, p, x, y);
        pc.box.add(x, y);
      }
      for (size_t i = 0; i + 1 < path.size(); i++) {
        out.edges.push_back({path[i], path[i + 1]});
        // Wire steps stay inside a layer's node range; a via crosses it.
        int lo = std::min(path[i], path[i + 1]), hi = std::max(path[i], path[i + 1]);
        int lg = 0;
        while (lg + 1 < (int)gl.size() && lo >= gl[lg + 1].base) lg++;
        if (lg + 1 < (int)gl.size() && hi >= gl[lg + 1].base) { out.cuts.push_back(lo); cutNear(lo, [&](int n) { sc.cuts.insert(n); }); }
      }
      pc.end = out.nodes.size();
      if (pc.end > pc.begin) pieces.push_back(pc);
    }
    // A pin whose access node another pin's path already crossed shares it.
    std::sort(out.nodes.begin(), out.nodes.end());
    out.nodes.erase(std::unique(out.nodes.begin(), out.nodes.end()), out.nodes.end());
    std::sort(out.cuts.begin(), out.cuts.end());
    out.cuts.erase(std::unique(out.cuts.begin(), out.cuts.end()), out.cuts.end());
    out.ok = true;
    return true;
  }

  bool conflicted(int net) {
    NetRoute& r = routes[net];
    if (!r.ok) return false;
    addClaims(r, -1);
    bool bad = false;
    for (int n : r.nodes) if (claim[n]) { bad = true; if (hist[n] < 60000) hist[n]++; }
    for (int n : r.cuts) if (cutClaim[n]) { bad = true; if (hist[n] < 60000) hist[n]++; }
    addClaims(r, +1);
    return bad;
  }

  // The nodes of a routed net that another net also claims.
  std::vector<int> stuckNodes(int net) {
    std::vector<int> out;
    NetRoute& r = routes[net];
    if (!r.ok) return out;
    addClaims(r, -1);
    for (int n : r.nodes) if (claim[n]) out.push_back(n);
    for (int n : r.cuts) if (cutClaim[n]) out.push_back(n);
    addClaims(r, +1);
    return out;
  }

  void negotiate() {
    routes.assign(db.nets.size(), NetRoute{});
    variant.assign(db.nets.size(), 0);
    for (size_t i = 0; i < db.nets.size(); i++) if (db.nets[i].pins.size() >= 2) order.push_back((int)i);
    // Short nets first.
    std::vector<Box> box(db.nets.size());
    pool.chunks(order.size(), 256, [&](size_t lo, size_t hi, size_t) { for (size_t i = lo; i < hi; i++) box[order[i]] = netBox(order[i]); });
    auto size = [&](int n) { return box[n].x1 >= box[n].x0 ? (box[n].x1 - box[n].x0) + (box[n].y1 - box[n].y0) : 0; };
    std::sort(order.begin(), order.end(), [&](int a, int b) { int64_t sa = size(a), sb = size(b); return sa != sb ? sa < sb : a < b; });

    std::vector<int> todo = order;
    size_t bestLeft = SIZE_MAX;
    int bestIter = 0;
    std::vector<char> failed(db.nets.size(), 0);
    int64_t pres = unit * 2;
    // Coarse map for grouping nets that cannot touch each other.
    int64_t cell = 16 * gl[0].L->pitch();
    int cw = (int)(db.die.w() / cell) + 1, ch = (int)(db.die.h() / cell) + 1;
    std::vector<int> mapStamp((size_t)cw * ch, 0);
    int stampNo = 0;
    int64_t halo = haloReach();
    std::vector<Scratch> scratch(pool.threads() + 1);
    std::mutex scratchLock;
    std::vector<int> freeScratch;
    for (int i = 0; i <= (int)pool.threads(); i++) freeScratch.push_back(i);

    for (int iter = 0; iter < opt.maxIter && !todo.empty(); iter++) {
      iterations = iter + 1;
      int64_t m = margin(iter) + halo;
      std::vector<char> done(todo.size(), 0);
      size_t first = 0, left = todo.size();
      std::vector<int> batch;
      int nBatches = 0;
      while (left) {
        nBatches++;
        batch.clear();
        stampNo++;
        // Take nets, in order, whose windows touch none already taken.
        size_t scanned = 0;
        for (size_t i = first; i < todo.size() && batch.size() < 4096 && scanned < 20000; i++) {
          if (done[i]) continue;
          scanned++;
          const Box& b = box[todo[i]];
          bool empty = b.x1 < b.x0;
          int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
          if (!empty) {
            x0 = (int)std::max<int64_t>(0, (b.x0 - m - db.die.x0) / cell); x1 = (int)std::min<int64_t>(cw - 1, (b.x1 + m - db.die.x0) / cell);
            y0 = (int)std::max<int64_t>(0, (b.y0 - m - db.die.y0) / cell); y1 = (int)std::min<int64_t>(ch - 1, (b.y1 + m - db.die.y0) / cell);
          }
          bool clash = false;
          for (int y = y0; y <= y1 && !clash; y++)
            for (int x = x0; x <= x1; x++) if (mapStamp[(size_t)y * cw + x] == stampNo) { clash = true; break; }
          if (clash) continue;
          for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) mapStamp[(size_t)y * cw + x] = stampNo;
          batch.push_back((int)i);
          done[i] = 1;
        }
        while (first < todo.size() && done[first]) first++;
        left -= batch.size();
        pool.run(batch.size(), [&](size_t bi) {
          int si;
          { std::lock_guard<std::mutex> g(scratchLock); si = freeScratch.back(); freeScratch.pop_back(); }
          int net = todo[batch[bi]];
          NetRoute& r = routes[net];
          if (r.ok) addClaims(r, -1);
          NetRoute nr;
          bool ok = routeNet(net, iter, pres, scratch[si], nr);
          r = std::move(nr);
          failed[net] = !ok;
          if (ok) addClaims(r, +1); else r = NetRoute{};
          { std::lock_guard<std::mutex> g(scratchLock); freeScratch.push_back(si); }
        });
      }
      // Who still shares nodes?
      std::vector<int> next;
      for (int net : order) if (failed[net] || conflicted(net)) next.push_back(net);
      size_t contested = next.size();
      if (opt.verbose) fprintf(stderr, "  route %2d: %zu nets routed in %d groups, %zu to redo\n", iter, todo.size(), nBatches, next.size());
      // A net that stays contested is boxed in by nets that are not: take
      // those out too, and let the contested ones choose first.
      if (iter >= 2 && !next.empty() && next.size() <= 64) {
        std::vector<Box> boxes;
        int64_t grow = 4 * gl[0].L->pitch();
        for (int net : next) {
          if (failed[net]) continue;
          Box b;
          for (int n : stuckNodes(net)) { int g, t, p; int64_t x, y; decode(n, g, t, p); xy(g, t, p, x, y); b.add(x, y); }
          if (b.x1 < b.x0) continue;
          b.grow(grow);
          boxes.push_back(b);
        }
        std::vector<char> inNext(db.nets.size(), 0);
        for (int net : next) inNext[net] = 1;
        std::vector<char> near(order.size(), 0);
        pool.chunks(order.size(), 256, [&](size_t lo, size_t hi, size_t) {
          for (size_t i = lo; i < hi; i++) {
            int net = order[i];
            if (inNext[net] || !routes[net].ok) continue;
            const Box& nb = box[net];
            bool maybe = false;
            for (auto& b : boxes) if (!(nb.x1 < b.x0 || nb.x0 > b.x1 || nb.y1 < b.y0 || nb.y0 > b.y1)) { maybe = true; break; }
            if (!maybe && db.nets[net].pins.size() < 16) continue;
            for (int n : routes[net].nodes) {
              int g, t, p;
              int64_t x, y;
              decode(n, g, t, p);
              xy(g, t, p, x, y);
              for (auto& b : boxes) if (b.has(x, y)) { near[i] = 1; break; }
              if (near[i]) break;
            }
          }
        });
        for (size_t i = 0; i < order.size(); i++)
          if (near[i]) { int net = order[i]; addClaims(routes[net], -1); routes[net] = NetRoute{}; next.push_back(net); }
      }
      // Stop when the contested nets have not become fewer for a while.
      if (contested < bestLeft) { bestLeft = contested; bestIter = iter; }
      // A net builds its tree pin by pin, and its own earlier vias can shut
      // the clean way into a later pin: start it from another pin.
      if (iter - bestIter >= 2) for (size_t i = 0; i < contested; i++) variant[next[i]]++;
      todo.swap(next);
      if (iter - bestIter >= 8) break;
      pres = std::min<int64_t>(pres * 2, unit * 4096);
    }
    // Whatever still clashes is taken out, so the result has opens, not shorts.
    for (auto it = todo.rbegin(); it != todo.rend(); ++it) {
      int net = *it;
      if (failed[net]) continue;
      if (conflicted(net)) { addClaims(routes[net], -1); routes[net] = NetRoute{}; failed[net] = 1; }
    }
    for (int net : order) db.nets[net].routed = routes[net].ok;
  }

  RouteResult summary() const {
    RouteResult r;
    r.iterations = iterations;
    r.layerLength.assign(gl.size(), 0);
    for (auto& l : gl) r.layerName.push_back(l.L->name);
    for (int net : order) {
      const NetRoute& nr = routes[net];
      r.nets++;
      if (!nr.ok) { r.failed++; if (r.failedNets.size() < 20) r.failedNets.push_back(db.nets[net].name); continue; }
      for (auto& e : nr.edges) {
        int g1, t1, p1, g2, t2, p2;
        decode(e.first, g1, t1, p1); decode(e.second, g2, t2, p2);
        if (g1 != g2) { r.vias++; continue; }
        int64_t len = std::llabs(gl[g1].pos[p1] - gl[g1].pos[p2]);
        r.layerLength[g1] += len; r.wireLength += len;
      }
      if (accessLayer)
        for (size_t k = 0; k < nr.pinNode.size(); k++) {
          const Pin& pn = db.pins[db.nets[net].pins[k]];
          if (pn.inst >= 0) r.vias++;
        }
    }
    return r;
  }

  friend struct RouterOut;
};

}  // namespace pnr

#include "pnr_out.hpp"
#include "pnr_check.hpp"
