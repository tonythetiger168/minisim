// Layout check on a routed DEF: every net connected (opens), no metal of two
// nets touching (shorts) or closer than the layer's SPACING, and no two cuts
// closer than the cut layer's SPACING even within a net. It works on the
// shapes of the DEF and LEF alone, so it checks what mpnr wrote, not what the
// router believes. Included by pnr_route.hpp.
#pragma once

namespace pnr {

struct CheckResult {
  int nets = 0, opens = 0, shorts = 0, spacing = 0;
  std::vector<std::string> messages;      // the first few of each kind
  std::vector<std::pair<std::string, int>> supplies;   // special net, separate pieces of its wiring
  bool clean() const { return !opens && !shorts && !spacing; }
};

inline void printCheck(std::ostream& o, const CheckResult& c) {
  o << "check: " << c.nets << " nets, " << c.opens << " open, " << c.shorts << " shorts, " << c.spacing << " spacing violations\n";
  for (auto& m : c.messages) o << "  " << m << "\n";
  for (auto& [name, pieces] : c.supplies)
    if (pieces > 1) o << "  note: supply " << name << " is in " << pieces << " separate pieces (rails not tied by straps)\n";
}

inline CheckResult checkDesign(const phys::Tech& tech, const phys::Design& d, par::Pool& pool) {
  struct Shape { Rect r; int net; int layer; int group; bool wire; };
  // net: index into d.nets, d.nets.size() + i for special nets, -1 for an obstruction.
  // group: shapes with the same group >= 0 are one conductor (a pin, a via).
  std::vector<Shape> shapes;
  if (tech.dbu % d.dbu) throw std::runtime_error("the DEF's database units do not divide the LEF's");
  int64_t k = tech.dbu / d.dbu;
  auto sc = [&](Rect r) { return Rect{r.x0 * k, r.y0 * k, r.x1 * k, r.y1 * k}; };
  std::unordered_map<std::string, int> layerIdx;
  for (auto& l : tech.layers) layerIdx[l.name] = l.index;
  auto li = [&](const std::string& n) { auto it = layerIdx.find(n); return it == layerIdx.end() ? -1 : it->second; };
  int groups = 0;

  std::unordered_map<std::string, int> compIdx, ioIdx;
  for (size_t i = 0; i < d.comps.size(); i++) compIdx[d.comps[i].name] = (int)i;
  for (size_t i = 0; i < d.pins.size(); i++) ioIdx[d.pins[i].name] = (int)i;
  std::vector<const phys::Macro*> mac(d.comps.size());
  std::vector<std::vector<int>> pinNet(d.comps.size());
  for (size_t i = 0; i < d.comps.size(); i++) {
    mac[i] = tech.macro(d.comps[i].macro);
    if (!mac[i]) throw std::runtime_error("component " + d.comps[i].name + ": no macro " + d.comps[i].macro + " in the LEF");
    pinNet[i].assign(mac[i]->pins.size(), -1);
  }
  std::vector<int> ioNet(d.pins.size(), -1);
  // Per net: the groups of its pins.
  std::vector<std::vector<int>> netPins(d.nets.size());
  std::vector<std::vector<int>> pinGroup(d.comps.size());
  for (size_t i = 0; i < d.comps.size(); i++) pinGroup[i].assign(mac[i]->pins.size(), -1);
  std::vector<int> ioGroup(d.pins.size(), -1);
  CheckResult res;
  for (size_t ni = 0; ni < d.nets.size(); ni++)
    for (auto& c : d.nets[ni].conns) {
      if (c.inst == "PIN") {
        auto it = ioIdx.find(c.pin);
        if (it == ioIdx.end()) throw std::runtime_error("net " + d.nets[ni].name + ": no pin " + c.pin);
        ioNet[it->second] = (int)ni;
        ioGroup[it->second] = groups;
      } else {
        auto it = compIdx.find(c.inst);
        if (it == compIdx.end()) throw std::runtime_error("net " + d.nets[ni].name + ": no component " + c.inst);
        const phys::MacroPin* mp = mac[it->second]->pin(c.pin);
        if (!mp) throw std::runtime_error("net " + d.nets[ni].name + ": " + c.inst + " has no pin " + c.pin);
        pinNet[it->second][mp - mac[it->second]->pins.data()] = (int)ni;
        pinGroup[it->second][mp - mac[it->second]->pins.data()] = groups;
      }
      netPins[ni].push_back(groups++);
    }
  int nNets = (int)d.nets.size();
  for (size_t si = 0; si < d.specialNets.size(); si++)
    for (auto& c : d.specialNets[si].conns)
      if (c.inst == "*")
        for (size_t i = 0; i < d.comps.size(); i++) { const phys::MacroPin* mp = mac[i]->pin(c.pin); if (mp) pinNet[i][mp - mac[i]->pins.data()] = nNets + (int)si; }

  for (size_t i = 0; i < d.comps.size(); i++) {
    const phys::Component& c = d.comps[i];
    for (size_t p = 0; p < mac[i]->pins.size(); p++)
      for (auto& s : mac[i]->pins[p].ports)
        shapes.push_back({phys::xform(s.r, c.orient, mac[i]->w, mac[i]->h, c.x * k, c.y * k), pinNet[i][p], li(s.layer), pinGroup[i][p], false});
    for (auto& s : mac[i]->obs) shapes.push_back({phys::xform(s.r, c.orient, mac[i]->w, mac[i]->h, c.x * k, c.y * k), -1, li(s.layer), -1, false});
  }
  for (size_t i = 0; i < d.pins.size(); i++)
    for (auto& s : d.pins[i].shapes) shapes.push_back({sc(s.r), ioNet[i], li(s.layer), ioGroup[i], true});
  auto viaDef = [&](const std::string& n) -> const phys::ViaDef* {
    for (auto& v : d.vias) if (v.name == n) return &v;
    return tech.via(n);
  };
  auto wiring = [&](const phys::Net& n, int id, bool special) {
    for (auto& s : n.segs) {
      const phys::Layer* l = tech.layer(s.layer);
      if (!l) continue;
      int64_t w = s.width ? s.width * k : l->width, hw = w / 2;
      int64_t e0 = special ? 0 : (s.ext0 >= 0 ? s.ext0 * k : hw), e1 = special ? 0 : (s.ext1 >= 0 ? s.ext1 * k : hw);
      Rect r;
      int64_t x0 = s.x0 * k, y0 = s.y0 * k, x1 = s.x1 * k, y1 = s.y1 * k;
      if (y0 == y1) { if (x0 > x1) { std::swap(x0, x1); std::swap(e0, e1); } r = {x0 - e0, y0 - hw, x1 + e1, y0 + hw}; }
      else { if (y0 > y1) { std::swap(y0, y1); std::swap(e0, e1); } r = {x0 - hw, y0 - e0, x0 + hw, y1 + e1}; }
      shapes.push_back({r, id, l->index, -1, true});
    }
    for (auto& v : n.vias) {
      const phys::ViaDef* vd = viaDef(v.via);
      if (!vd) throw std::runtime_error("net " + n.name + ": no via " + v.via);
      bool fromDef = vd != tech.via(v.via);
      int g = groups++;
      for (auto& s : vd->shapes) {
        Rect r = fromDef ? sc(s.r) : s.r;
        shapes.push_back({{r.x0 + v.x * k, r.y0 + v.y * k, r.x1 + v.x * k, r.y1 + v.y * k}, id, li(s.layer), g, true});
      }
    }
    for (auto& r : n.rects) shapes.push_back({sc(r.r), id, li(r.layer), -1, true});
  };
  for (size_t i = 0; i < d.nets.size(); i++) wiring(d.nets[i], (int)i, false);
  for (size_t i = 0; i < d.specialNets.size(); i++) wiring(d.specialNets[i], nNets + (int)i, true);
  auto netName = [&](int n) { return n < 0 ? std::string("an obstruction") : n < nNets ? d.nets[n].name : d.specialNets[n - nNets].name; };

  // ---- shorts and spacing ----
  // Per layer the shapes go into a grid of buckets; a pair is looked at in
  // the one bucket that holds the corner of their (grown) overlap, so each
  // pair is seen once. One task per bucket row.
  struct Task { int layer, row; };
  struct LayerGrid { int64_t x0 = 0, y0 = 0, cell = 1; int nx = 0, ny = 0; std::vector<std::vector<int>> wires, cells; };
  std::vector<LayerGrid> grid(tech.layers.size());
  std::vector<Task> tasks;
  Rect all{INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};
  for (auto& sh : shapes) { all.x0 = std::min(all.x0, sh.r.x0); all.y0 = std::min(all.y0, sh.r.y0); all.x1 = std::max(all.x1, sh.r.x1); all.y1 = std::max(all.y1, sh.r.y1); }
  std::vector<std::vector<int>> byLayer(tech.layers.size());
  for (size_t i = 0; i < shapes.size(); i++) if (shapes[i].layer >= 0) byLayer[shapes[i].layer].push_back((int)i);
  for (size_t l = 0; l < tech.layers.size(); l++) {
    const phys::Layer& L = tech.layers[l];
    if ((L.type != phys::Layer::Routing && L.type != phys::Layer::Cut) || byLayer[l].empty()) continue;
    bool anyWire = false;
    for (int i : byLayer[l]) anyWire |= shapes[i].wire;
    if (!anyWire) continue;
    LayerGrid& g = grid[l];
    g.x0 = all.x0 - L.spacing; g.y0 = all.y0 - L.spacing;
    g.cell = std::max<int64_t>(16 * std::max<int64_t>(L.width + L.spacing, 1), 2000);
    g.nx = (int)((all.x1 + L.spacing - g.x0) / g.cell) + 1; g.ny = (int)((all.y1 + L.spacing - g.y0) / g.cell) + 1;
    g.wires.resize((size_t)g.nx * g.ny); g.cells.resize((size_t)g.nx * g.ny);
    for (int i : byLayer[l]) {
      const Shape& sh = shapes[i];
      int cx0 = (int)((sh.r.x0 - L.spacing - g.x0) / g.cell), cx1 = (int)((sh.r.x1 + L.spacing - g.x0) / g.cell);
      int cy0 = (int)((sh.r.y0 - L.spacing - g.y0) / g.cell), cy1 = (int)((sh.r.y1 + L.spacing - g.y0) / g.cell);
      cx0 = std::max(cx0, 0); cy0 = std::max(cy0, 0); cx1 = std::min(cx1, g.nx - 1); cy1 = std::min(cy1, g.ny - 1);
      for (int cy = cy0; cy <= cy1; cy++) for (int cx = cx0; cx <= cx1; cx++) (sh.wire ? g.wires : g.cells)[(size_t)cy * g.nx + cx].push_back(i);
    }
    for (int r = 0; r < g.ny; r++) tasks.push_back({(int)l, r});
  }
  struct Hit { bool isShort; int a, b; std::string msg; };
  std::vector<std::vector<Hit>> hits(tasks.size());
  pool.run(tasks.size(), [&](size_t ti) {
    const phys::Layer& L = tech.layers[tasks[ti].layer];
    const LayerGrid& g = grid[tasks[ti].layer];
    int64_t sp = L.spacing;
    std::vector<Hit>& out = hits[ti];
    auto pair = [&](int ia, int ib, int cx, int cy) {
      const Shape& a = shapes[ia]; const Shape& b = shapes[ib];
      if (a.net == b.net) {
        // Cuts keep their distance within a net too (unless they are one cut drawn twice).
        if (L.type != phys::Layer::Cut || a.net < 0 || a.group == b.group) return;
        if (b.r.x0 < a.r.x1 && a.r.x0 < b.r.x1 && b.r.y0 < a.r.y1 && a.r.y0 < b.r.y1) return;
      }
      // Seen only in the bucket of the lower left corner of the grown overlap.
      int64_t ox = std::max(a.r.x0, b.r.x0) - sp, oy = std::max(a.r.y0, b.r.y0) - sp;
      if (std::max<int64_t>(0, (ox - g.x0) / g.cell) != cx || std::max<int64_t>(0, (oy - g.y0) / g.cell) != cy) return;
      int64_t dx = std::max<int64_t>(0, std::max(b.r.x0 - a.r.x1, a.r.x0 - b.r.x1)), dy = std::max<int64_t>(0, std::max(b.r.y0 - a.r.y1, a.r.y0 - b.r.y1));
      if (dx >= sp || dy >= sp) return;
      if (!dx && !dy) {
        // Metal over an obstruction is a short with it only if they overlap.
        if ((a.net < 0 || b.net < 0) && !(b.r.x0 < a.r.x1 && a.r.x0 < b.r.x1 && b.r.y0 < a.r.y1 && a.r.y0 < b.r.y1)) return;
        out.push_back({true, std::min(a.net, b.net), std::max(a.net, b.net),
                       "short on " + L.name + ": " + netName(a.net) + " and " + netName(b.net) + " at (" + std::to_string((double)std::max(a.r.x0, b.r.x0) / tech.dbu) +
                       ", " + std::to_string((double)std::max(a.r.y0, b.r.y0) / tech.dbu) + ")"});
      } else if (!(dx && dy) || dx * dx + dy * dy < sp * sp) {
        out.push_back({false, std::min(a.net, b.net), std::max(a.net, b.net),
                       "spacing on " + L.name + ": " + netName(a.net) + " and " + netName(b.net) + " are " + std::to_string(std::sqrt((double)(dx * dx + dy * dy)) / tech.dbu) +
                       " um apart at (" + std::to_string((double)std::max(a.r.x0, b.r.x0) / tech.dbu) + ", " + std::to_string((double)std::max(a.r.y0, b.r.y0) / tech.dbu) + ")"});
      }
    };
    int cy = tasks[ti].row;
    for (int cx = 0; cx < g.nx; cx++) {
      const std::vector<int>& w = g.wires[(size_t)cy * g.nx + cx];
      const std::vector<int>& c = g.cells[(size_t)cy * g.nx + cx];
      for (size_t i = 0; i < w.size(); i++) {
        for (size_t j = i + 1; j < w.size(); j++) pair(w[i], w[j], cx, cy);
        for (int k : c) pair(w[i], k, cx, cy);
      }
    }
  });
  {
    std::set<std::tuple<int, int, int>> seenShort;      // layer, net, net
    int shortMsgs = 0, spaceMsgs = 0;
    for (size_t ti = 0; ti < tasks.size(); ti++)
      for (auto& h : hits[ti]) {
        if (h.isShort) {
          if (!seenShort.insert({tasks[ti].layer, h.a, h.b}).second) continue;
          res.shorts++;
          if (shortMsgs++ < 6) res.messages.push_back(h.msg);
        } else {
          res.spacing++;
          if (spaceMsgs++ < 6) res.messages.push_back(h.msg);
        }
      }
  }

  // ---- opens: each net's shapes joined where they touch on a layer ----
  std::vector<std::vector<int>> byNet(d.nets.size());
  for (size_t i = 0; i < shapes.size(); i++) if (shapes[i].net >= 0 && shapes[i].net < nNets && shapes[i].layer >= 0) byNet[shapes[i].net].push_back((int)i);
  std::vector<char> open(d.nets.size(), 0);
  pool.chunks(d.nets.size(), 64, [&](size_t lo, size_t hi, size_t) {
    std::vector<int> parent;
    std::unordered_map<int, int> groupRep;
    for (size_t ni = lo; ni < hi; ni++) {
      if (d.nets[ni].conns.size() < 2) continue;
      std::vector<int>& v = byNet[ni];
      std::sort(v.begin(), v.end(), [&](int a, int b) {
        if (shapes[a].layer != shapes[b].layer) return shapes[a].layer < shapes[b].layer;
        return shapes[a].r.x0 != shapes[b].r.x0 ? shapes[a].r.x0 < shapes[b].r.x0 : a < b;
      });
      parent.resize(v.size());
      std::iota(parent.begin(), parent.end(), 0);
      auto find = [&](int a) { while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
      groupRep.clear();
      for (size_t i = 0; i < v.size(); i++) {
        const Shape& a = shapes[v[i]];
        if (a.group >= 0) { auto [it, fresh] = groupRep.emplace(a.group, (int)i); if (!fresh) parent[find((int)i)] = find(it->second); }
        for (size_t j = i + 1; j < v.size(); j++) {
          const Shape& b = shapes[v[j]];
          if (b.layer != a.layer || b.r.x0 > a.r.x1) break;
          if (b.r.y0 <= a.r.y1 && a.r.y0 <= b.r.y1) parent[find((int)j)] = find((int)i);
        }
      }
      int root = -1;
      for (int g : netPins[ni]) {
        auto it = groupRep.find(g);
        int r = it == groupRep.end() ? -2 : find(it->second);
        if (root == -1) root = r;
        if (r != root || r == -2) { open[ni] = 1; break; }
      }
    }
  });
  for (size_t ni = 0; ni < d.nets.size(); ni++) {
    if (d.nets[ni].conns.size() < 2) continue;
    res.nets++;
    if (open[ni]) { res.opens++; if (res.messages.size() < 16) res.messages.push_back("open: net " + d.nets[ni].name); }
  }
  // ---- supplies: how many separate pieces the wiring of each special net is in ----
  for (size_t si = 0; si < d.specialNets.size(); si++) {
    std::vector<int> v;
    for (size_t i = 0; i < shapes.size(); i++) if (shapes[i].net == nNets + (int)si && shapes[i].wire && shapes[i].layer >= 0) v.push_back((int)i);
    if (v.empty()) continue;
    std::sort(v.begin(), v.end(), [&](int a, int b) {
      if (shapes[a].layer != shapes[b].layer) return shapes[a].layer < shapes[b].layer;
      return shapes[a].r.x0 != shapes[b].r.x0 ? shapes[a].r.x0 < shapes[b].r.x0 : a < b;
    });
    std::vector<int> parent(v.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int a) { while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
    std::unordered_map<int, int> groupRep;
    for (size_t i = 0; i < v.size(); i++) {
      const Shape& a = shapes[v[i]];
      if (a.group >= 0) { auto [it, fresh] = groupRep.emplace(a.group, (int)i); if (!fresh) parent[find((int)i)] = find(it->second); }
      for (size_t j = i + 1; j < v.size(); j++) {
        const Shape& b = shapes[v[j]];
        if (b.layer != a.layer || b.r.x0 > a.r.x1) break;
        if (b.r.y0 <= a.r.y1 && a.r.y0 <= b.r.y1) parent[find((int)j)] = find((int)i);
      }
    }
    int pieces = 0;
    for (size_t i = 0; i < v.size(); i++) pieces += find((int)i) == (int)i;
    res.supplies.push_back({d.specialNets[si].name, pieces});
  }
  return res;
}

}  // namespace pnr
