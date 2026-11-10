// GDSII out for mpnr: the placed and routed design as a GDS library, written
// with the shared gds.hpp. The top cell holds one reference per component,
// the wires as paths, the vias, port shapes and their names. The cells are
// either abstracts made from the LEF (pin shapes, pin names, the outline) or,
// with a cell GDS, the real layouts copied from it.
#pragma once
#include "def.hpp"
#include "gds.hpp"
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace pnr {

struct GdsOptions {
  std::string mapFile;          // "LEFLAYER GDSLAYER [DATATYPE]" per line; empty: LEF order, from 1
  std::string cellsFile;        // GDS with the cells' real layouts; empty: abstracts from the LEF
};

struct GdsLayerMap {
  std::map<std::string, std::pair<int, int>> m;      // LEF layer -> GDS layer, datatype
  std::pair<int, int> outline{0, 0};                 // the name OUTLINE: die and cell boundaries
  bool hasOutline = true;
  const std::pair<int, int>* find(const std::string& n) const { auto it = m.find(n); return it == m.end() ? nullptr : &it->second; }
};

// The default map numbers the LEF's layers 1, 2, ... in file order, datatype
// 0, and puts outlines on GDS layer 0. A map file replaces it entirely: a
// layer it does not name is not written.
inline GdsLayerMap gdsLayerMap(const phys::Tech& tech, const std::string& file) {
  GdsLayerMap map;
  if (file.empty()) {
    int n = 1;
    for (auto& l : tech.layers) map.m[l.name] = {n++, 0};
    return map;
  }
  std::ifstream in(file);
  if (!in) throw std::runtime_error("cannot read layer map " + file);
  map.hasOutline = false;
  std::string line;
  int no = 0;
  while (std::getline(in, line)) {
    no++;
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    for (char& c : line) if (c == '/') c = ' ';
    std::istringstream ls(line);
    std::string name;
    int layer = 0, dt = 0;
    if (!(ls >> name)) continue;
    if (!(ls >> layer) || layer < 0 || layer > 32767) throw std::runtime_error(file + ":" + std::to_string(no) + ": expected LAYERNAME GDSLAYER [DATATYPE]");
    ls >> dt;
    if (name == "OUTLINE") { map.outline = {layer, dt}; map.hasOutline = true; }
    else if (!tech.layer(name)) throw std::runtime_error(file + ":" + std::to_string(no) + ": the LEF has no layer '" + name + "'");
    else map.m[name] = {layer, dt};
  }
  return map;
}

namespace detail {

inline gds::Point gpt(int64_t x, int64_t y) {
  if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) throw std::runtime_error("the design is too large for GDSII's 32-bit coordinates");
  return {(int32_t)x, (int32_t)y};
}
inline gds::Polygon gbox(const std::pair<int, int>& l, const Rect& r) {
  gds::Polygon p;
  p.layer = (int16_t)l.first; p.datatype = (int16_t)l.second;
  p.pts = {gpt(r.x0, r.y0), gpt(r.x1, r.y0), gpt(r.x1, r.y1), gpt(r.x0, r.y1)};
  return p;
}
// A GDS reference transform applied to a point of the cell.
inline void gdsApply(bool mirror, int angle, int64_t px, int64_t py, int64_t& x, int64_t& y) {
  if (mirror) py = -py;
  switch (angle) {
    case 0: x = px; y = py; break;
    case 90: x = -py; y = px; break;
    case 180: x = -px; y = -py; break;
    default: x = py; y = -px; break;
  }
}
// The reference that places a w x h cell the way DEF's orientation does with
// its lower left corner at (x, y): found by trying the eight transforms
// against phys::xform, so the two can never disagree.
inline gds::Ref gdsRef(const std::string& cell, const std::string& orient, int64_t w, int64_t h, int64_t x, int64_t y) {
  const Rect probe{w / 4, h / 8, w / 4 + 1, h / 8 + 1};       // away from every symmetry axis
  Rect want = phys::xform(probe, orient, w, h, 0, 0);
  for (int mirror = 0; mirror < 2; mirror++)
    for (int angle = 0; angle < 360; angle += 90) {
      int64_t ax, ay, bx, by;
      gdsApply(mirror, angle, probe.x0, probe.y0, ax, ay);
      gdsApply(mirror, angle, probe.x1, probe.y1, bx, by);
      int64_t ox = want.x0 - std::min(ax, bx), oy = want.y0 - std::min(ay, by);
      // The same shift must also bring the cell's corner home.
      int64_t cx, cy, dx, dy;
      gdsApply(mirror, angle, 0, 0, cx, cy);
      gdsApply(mirror, angle, w, h, dx, dy);
      Rect box = phys::xform({0, 0, w, h}, orient, w, h, 0, 0);
      if (std::min(cx, dx) + ox != box.x0 || std::min(cy, dy) + oy != box.y0 || std::max(cx, dx) + ox != box.x1 || std::max(cy, dy) + oy != box.y1) continue;
      if (std::max(ax, bx) + ox != want.x1 || std::max(ay, by) + oy != want.y1) continue;
      gds::Ref r;
      r.cell = cell;
      r.mirror = mirror;
      r.angle = angle;
      r.origin = gpt(x + ox, y + oy);
      return r;
    }
  throw std::runtime_error("orientation " + orient + " has no GDS transform");
}

}  // namespace detail

// The design as a GDS library. `d` is in its own DEF units; the library is
// written in the LEF's database units.
inline gds::Library toGds(const phys::Tech& tech, const phys::Design& d, const GdsOptions& opt = {}) {
  using namespace detail;
  GdsLayerMap map = gdsLayerMap(tech, opt.mapFile);
  if (tech.dbu % d.dbu) throw std::runtime_error("the DEF's database units do not divide the LEF's");
  int64_t k = tech.dbu / d.dbu;
  gds::Library lib;
  lib.name = d.name.empty() ? "mpnr" : d.name;
  lib.dbu = 1.0 / tech.dbu;
  lib.userUnit = 1.0 / tech.dbu;

  // Cells: the macros the design uses, in name order.
  std::set<std::string> used;
  for (auto& c : d.comps) used.insert(c.macro);
  std::set<std::string> have;
  if (!opt.cellsFile.empty()) {
    gds::Library cells;
    std::string err;
    if (!gds::read(opt.cellsFile, cells, err)) throw std::runtime_error(opt.cellsFile + ": " + err);
    if (std::fabs(cells.dbu - lib.dbu) > 1e-9 * lib.dbu)
      throw std::runtime_error(opt.cellsFile + ": its database unit (" + std::to_string(cells.dbu) + " um) is not the LEF's (" + std::to_string(lib.dbu) + " um)");
    for (auto& c : cells.cells) { have.insert(c.name); lib.cells.push_back(c); }
    for (auto& m : used) if (!have.count(m)) throw std::runtime_error(opt.cellsFile + ": no cell '" + m + "'");
    if (have.count(lib.name)) throw std::runtime_error(opt.cellsFile + ": it already has a cell named like the design, '" + lib.name + "'");
  }
  for (auto& name : used) {
    if (have.count(name)) continue;
    const phys::Macro* m = tech.macro(name);
    if (!m) throw std::runtime_error("no macro " + name + " in the LEF");
    gds::Cell c;
    c.name = name;
    if (map.hasOutline) c.polygons.push_back(gbox(map.outline, {0, 0, m->w, m->h}));
    for (auto& p : m->pins) {
      bool named = false;
      for (auto& s : p.ports) {
        const auto* l = map.find(s.layer);
        if (!l) continue;
        c.polygons.push_back(gbox(*l, s.r));
        if (!named) {
          gds::Text t;
          t.layer = (int16_t)l->first; t.texttype = (int16_t)l->second;
          t.at = gpt((s.r.x0 + s.r.x1) / 2, (s.r.y0 + s.r.y1) / 2);
          t.str = p.name;
          c.texts.push_back(t);
          named = true;
        }
      }
    }
    lib.cells.push_back(std::move(c));
  }

  gds::Cell top;
  top.name = lib.name;
  if (map.hasOutline && d.die.w() > 0) top.polygons.push_back(gbox(map.outline, {d.die.x0 * k, d.die.y0 * k, d.die.x1 * k, d.die.y1 * k}));
  for (auto& c : d.comps) {
    const phys::Macro* m = tech.macro(c.macro);
    if (!m) throw std::runtime_error("component " + c.name + ": no macro " + c.macro + " in the LEF");
    top.refs.push_back(gdsRef(c.macro, c.orient, m->w, m->h, c.x * k, c.y * k));
  }
  for (auto& p : d.pins) {
    bool named = false;
    for (auto& s : p.shapes) {
      const auto* l = map.find(s.layer);
      if (!l) continue;
      Rect r{s.r.x0 * k, s.r.y0 * k, s.r.x1 * k, s.r.y1 * k};
      top.polygons.push_back(gbox(*l, r));
      if (!named) {
        gds::Text t;
        t.layer = (int16_t)l->first; t.texttype = (int16_t)l->second;
        t.at = gpt((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
        t.str = p.name;
        top.texts.push_back(t);
        named = true;
      }
    }
  }
  auto viaDef = [&](const std::string& n, bool& fromDef) -> const phys::ViaDef* {
    for (auto& v : d.vias) if (v.name == n) { fromDef = true; return &v; }
    fromDef = false;
    return tech.via(n);
  };
  auto wiring = [&](const phys::Net& n, bool special) {
    for (auto& s : n.segs) {
      const phys::Layer* L = tech.layer(s.layer);
      const auto* l = map.find(s.layer);
      if (!L || !l) continue;
      int64_t w = s.width ? s.width * k : L->width;
      int64_t e0 = special ? 0 : (s.ext0 >= 0 ? s.ext0 * k : w / 2), e1 = special ? 0 : (s.ext1 >= 0 ? s.ext1 * k : w / 2);
      int64_t x0 = s.x0 * k, y0 = s.y0 * k, x1 = s.x1 * k, y1 = s.y1 * k;
      if (x0 == x1 && y0 == y1) {                       // a dot: GDS paths need two different points
        if (e0 > 0 || e1 > 0) top.polygons.push_back(gbox(*l, {x0 - w / 2, y0 - w / 2, x0 + w / 2, y0 + w / 2}));
        continue;
      }
      gds::Path p;
      p.layer = (int16_t)l->first; p.datatype = (int16_t)l->second;
      p.width = (int32_t)w;
      if (e0 == 0 && e1 == 0) p.pathtype = 0;
      else if (e0 == w / 2 && e1 == w / 2) p.pathtype = 2;
      else { p.pathtype = 4; p.bgnextn = (int32_t)e0; p.endextn = (int32_t)e1; }
      p.pts = {gpt(x0, y0), gpt(x1, y1)};
      top.paths.push_back(std::move(p));
    }
    for (auto& v : n.vias) {
      bool fromDef = false;
      const phys::ViaDef* vd = viaDef(v.via, fromDef);
      if (!vd) throw std::runtime_error("net " + n.name + ": no via " + v.via);
      int64_t vk = fromDef ? k : 1;
      for (auto& s : vd->shapes) {
        const auto* l = map.find(s.layer);
        if (l) top.polygons.push_back(gbox(*l, {s.r.x0 * vk + v.x * k, s.r.y0 * vk + v.y * k, s.r.x1 * vk + v.x * k, s.r.y1 * vk + v.y * k}));
      }
    }
    for (auto& r : n.rects) {
      const auto* l = map.find(r.layer);
      if (l) top.polygons.push_back(gbox(*l, {r.r.x0 * k, r.r.y0 * k, r.r.x1 * k, r.r.y1 * k}));
    }
  };
  for (auto& n : d.specialNets) wiring(n, true);
  for (auto& n : d.nets) wiring(n, false);
  lib.cells.push_back(std::move(top));
  return lib;
}

inline void writeGds(const phys::Tech& tech, const phys::Design& d, const std::string& path, const GdsOptions& opt = {}) {
  gds::Library lib = toGds(tech, d, opt);
  std::string err;
  if (!gds::write(path, lib, err)) throw std::runtime_error(path + ": " + err);
}

}  // namespace pnr
