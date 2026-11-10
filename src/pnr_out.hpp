// mpnr outputs: the design as DEF (phys::Design), the flat netlist, an SVG
// picture, and the floorplan DEF reader. Included by pnr_route.hpp.
#pragma once

namespace pnr {

// Power rails along every row, taken from the POWER and GROUND pin shapes of
// the cells (SHAPE FOLLOWPIN special wires on the pins' layer).
inline void makeRails(DB& db) {
  if (!db.specialNets.empty()) return;
  const phys::Macro* ref = nullptr;
  for (auto& in : db.insts) if (in.m->isCore()) { ref = in.m; break; }
  if (!ref) return;
  std::map<std::string, phys::Net> nets;
  for (auto& p : ref->pins) {
    if (p.use != "POWER" && p.use != "GROUND") continue;
    // The widest shape is the rail.
    const phys::LayerRect* rail = nullptr;
    for (auto& s : p.ports) if (!rail || s.r.w() > rail->r.w()) rail = &s;
    if (!rail || rail->r.w() < ref->w) continue;
    phys::Net& n = nets[p.name];
    n.name = p.name; n.use = p.use; n.wiring = "ROUTED";
    n.conns.push_back({"*", p.name});
    std::set<int64_t> seen;
    for (auto& row : db.rows) {
      Rect r = phys::xform(rail->r, row.orient, ref->w, ref->h, row.x, row.y);
      int64_t y = (r.y0 + r.y1) / 2;
      if (!seen.insert(y).second) continue;
      phys::Seg s;
      s.layer = rail->layer; s.width = r.h(); s.shape = "FOLLOWPIN";
      s.x0 = row.x; s.x1 = row.x + row.nx * row.stepX; s.y0 = s.y1 = y;
      n.segs.push_back(s);
    }
  }
  for (auto& [name, n] : nets) db.specialNets.push_back(n);
}

inline phys::Design Router::toDef(bool routed) const {
  phys::Design d;
  d.name = db.top;
  d.dbu = db.tech.dbu;
  d.die = db.die;
  d.rows = db.rows;
  for (auto l : db.tech.routing()) {
    std::vector<int64_t> t = layerTracks(*l, db.die);
    if (t.empty()) continue;
    phys::Tracks k;
    k.axis = l->dir == 'H' ? 'Y' : 'X';
    k.start = t[0]; k.count = (int64_t)t.size(); k.step = l->pitch();
    k.layers.push_back(l->name);
    d.tracks.push_back(k);
  }
  for (auto& in : db.insts) {
    phys::Component c;
    c.name = in.name; c.macro = in.m->name; c.x = in.x; c.y = in.y; c.orient = in.orient;
    c.status = in.fixed ? "FIXED" : "PLACED";
    if (in.filler) c.source = "DIST";
    d.comps.push_back(c);
  }
  for (auto& io : db.ios) {
    phys::IoPin p;
    p.name = io.name; p.net = db.nets[db.pins[io.pin].net].name; p.dir = io.dir;
    p.layer = io.layer; p.x = io.x; p.y = io.y; p.status = "PLACED";
    p.rect = {io.rect.x0 - io.x, io.rect.y0 - io.y, io.rect.x1 - io.x, io.rect.y1 - io.y};
    p.shapes.push_back({io.layer, io.rect});
    d.pins.push_back(p);
  }
  d.blockages = db.blockages;
  d.specialNets = db.specialNets;
  std::vector<phys::LayerRect> shapes;
  for (size_t ni = 0; ni < db.nets.size(); ni++) {
    const Net& nt = db.nets[ni];
    if (nt.pins.empty()) continue;
    phys::Net n;
    n.name = nt.name;
    for (int pid : nt.pins) {
      const Pin& pn = db.pins[pid];
      if (pn.inst < 0) n.conns.push_back({"PIN", db.ios[pn.idx].name});
      else n.conns.push_back({db.insts[pn.inst].name, db.insts[pn.inst].m->pins[pn.idx].name});
    }
    if (routed && ni < routes.size() && routes[ni].ok) {
      const NetRoute& r = routes[ni];
      n.wiring = "ROUTED";
      // Wire steps along one track, merged into runs.
      std::vector<std::array<int, 4>> steps;        // layer, track, from, to
      for (auto& e : r.edges) {
        int g1, t1, p1, g2, t2, p2;
        decode(e.first, g1, t1, p1); decode(e.second, g2, t2, p2);
        if (g1 == g2) { steps.push_back({g1, t1, std::min(p1, p2), std::max(p1, p2)}); continue; }
        int lo = g1 < g2 ? g1 : g2;
        int64_t x, y;
        xy(g1, t1, p1, x, y);
        n.vias.push_back({gl[lo].viaUp->name, gl[lo].L->name, x, y});
      }
      std::sort(steps.begin(), steps.end());
      for (size_t i = 0; i < steps.size();) {
        size_t j = i;
        int end = steps[i][3];
        while (j + 1 < steps.size() && steps[j + 1][0] == steps[i][0] && steps[j + 1][1] == steps[i][1] && steps[j + 1][2] <= end) { j++; end = std::max(end, steps[j][3]); }
        const GLayer& l = gl[steps[i][0]];
        phys::Seg s;
        s.layer = l.L->name;
        int64_t c = l.trk[steps[i][1]], a = l.pos[steps[i][2]], b = l.pos[end];
        if (l.horiz) { s.x0 = a; s.x1 = b; s.y0 = s.y1 = c; } else { s.y0 = a; s.y1 = b; s.x0 = s.x1 = c; }
        n.segs.push_back(s);
        i = j + 1;
      }
      // A via down to each pin that sits on the layer below the first routing layer.
      if (accessLayer)
        for (size_t k = 0; k < nt.pins.size(); k++) {
          int an = r.pinNode[k];
          if (an < 0) continue;
          int g, t, p;
          int64_t x, y;
          decode(an, g, t, p);
          if (g != 0) continue;
          xy(g, t, p, x, y);
          pinShapes(nt.pins[k], shapes);
          const Rect* on = nullptr;
          bool direct = false;
          for (auto& s : shapes) {
            if (!s.r.contains(x, y)) continue;
            if (s.layer == gl[0].L->name) direct = true;
            else if (s.layer == accessLayer->name && (!on || fitsVia(s.r, x, y))) on = &s.r;
          }
          if (direct || !on) continue;
          const phys::ViaDef* v = pinVia(*on, x, y);
          if (v) n.vias.push_back({v->name, accessLayer->name, x, y});
        }
    }
    d.nets.push_back(std::move(n));
  }
  return d;
}

// A floorplan DEF gives the die, rows, blockages, special nets, placed ports
// and FIXED cells; everything else is placed by mpnr.
inline void loadFloorplan(DB& db, const std::string& path, int minLayer) {
  phys::Design f;
  phys::readDef(path, f);
  if (db.tech.dbu % f.dbu) throw std::runtime_error(path + ": UNITS DISTANCE MICRONS " + std::to_string(f.dbu) + " does not divide the LEF's " + std::to_string(db.tech.dbu));
  int64_t k = db.tech.dbu / f.dbu;
  auto sc = [&](Rect r) { return Rect{r.x0 * k, r.y0 * k, r.x1 * k, r.y1 * k}; };
  db.die = sc(f.die);
  if (db.die.w() <= 0 || db.die.h() <= 0) throw std::runtime_error(path + ": no DIEAREA");
  const phys::Site* site = db.tech.coreSite();
  if (!site) throw std::runtime_error("the LEF has no SITE");
  if (f.rows.empty()) {
    int64_t mx = snapUp(4 * site->h, site->w), my = 4 * site->h;
    db.core = {db.die.x0 + mx, db.die.y0 + my, db.die.x0 + mx + snapDown(db.die.w() - 2 * mx, site->w), db.die.y0 + my + snapDown(db.die.h() - 2 * my, site->h)};
    makeRows(db);
  } else {
    db.core = {INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};
    for (auto r : f.rows) {
      r.x *= k; r.y *= k; r.stepX *= k; r.stepY *= k;
      if (r.ny != 1) throw std::runtime_error(path + ": row " + r.name + " is not horizontal");
      if (r.stepX == 0) r.stepX = site->w;
      db.rows.push_back(r);
      db.core.x0 = std::min(db.core.x0, r.x); db.core.y0 = std::min(db.core.y0, r.y);
      db.core.x1 = std::max(db.core.x1, r.x + r.nx * r.stepX); db.core.y1 = std::max(db.core.y1, r.y + site->h);
    }
    std::sort(db.rows.begin(), db.rows.end(), [](const phys::Row& a, const phys::Row& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
  }
  for (auto b : f.blockages) { b.r = sc(b.r); db.blockages.push_back(b); }
  for (auto n : f.specialNets) {
    for (auto& s : n.segs) { s.x0 *= k; s.y0 *= k; s.x1 *= k; s.y1 *= k; s.width *= k; }
    for (auto& v : n.vias) { v.x *= k; v.y *= k; }
    for (auto& r : n.rects) r.r = sc(r.r);
    db.specialNets.push_back(n);
  }
  std::unordered_map<std::string, int> ioByName, instByName;
  for (size_t i = 0; i < db.ios.size(); i++) ioByName[db.ios[i].name] = (int)i;
  for (size_t i = 0; i < db.insts.size(); i++) instByName[db.insts[i].name] = (int)i;
  for (auto& p : f.pins) {
    auto it = ioByName.find(p.name);
    if (it == ioByName.end() || p.layer.empty() || p.status.empty()) continue;
    Io& io = db.ios[it->second];
    io.layer = p.layer; io.rect = sc(p.shape()); io.x = p.x * k; io.y = p.y * k; io.orient = p.orient; io.placed = true;
  }
  for (auto& c : f.comps) {
    auto it = instByName.find(c.name);
    if (it == instByName.end() || (c.status != "FIXED" && c.status != "COVER")) continue;
    Inst& in = db.insts[it->second];
    in.x = c.x * k; in.y = c.y * k; in.orient = c.orient; in.fixed = in.placed = true;
  }
  placeIos(db, minLayer);
}

// The final flat netlist: the input cells plus the tie and filler cells.
// Ports keep their bus declarations.
inline void writeNetlist(const DB& db, const std::string& path) {
  gnl::Module m;
  m.name = db.top;
  std::vector<gnl::Ref> netRef(db.nets.size());
  std::vector<char> haveRef(db.nets.size(), 0);
  auto portRef = [](const Io& io) {
    gnl::Ref r;
    if (io.bus.empty()) { r.kind = gnl::Ref::Net; r.name = io.name; }
    else { r.kind = gnl::Ref::Bit; r.name = io.bus; r.msb = r.lsb = io.bit; }
    return r;
  };
  std::set<std::string> declared;
  for (auto& io : db.ios) {
    const std::string& pn = io.bus.empty() ? io.name : io.bus;
    if (declared.insert(pn).second) {
      m.portOrder.push_back(pn);
      gnl::Decl& d = m.decl(pn);
      d.isPort = true;
      d.dir = io.dir == "INPUT" ? gnl::Dir::In : io.dir == "OUTPUT" ? gnl::Dir::Out : gnl::Dir::InOut;
      auto br = db.busRange.find(pn);
      if (!io.bus.empty() && br != db.busRange.end()) { d.ranged = true; d.msb = br->second.first; d.lsb = br->second.second; }
    }
    // A net is written as the first port on it; further ports are aliases.
    int n = db.pins[io.pin].net;
    if (!haveRef[n]) { netRef[n] = portRef(io); haveRef[n] = 1; }
    else if (io.dir == "INPUT") m.assigns.push_back({{netRef[n]}, {portRef(io)}});
    else m.assigns.push_back({{portRef(io)}, {netRef[n]}});
  }
  for (size_t n = 0; n < db.nets.size(); n++) {
    if (haveRef[n] || db.nets[n].pins.empty()) continue;
    std::string name = db.nets[n].name;
    while (declared.count(name)) name += "_n";
    declared.insert(name);
    m.decl(name);
    netRef[n].kind = gnl::Ref::Net; netRef[n].name = name;
    haveRef[n] = 1;
  }
  for (auto& in : db.insts) {
    gnl::Inst gi;
    gi.cell = in.m->name; gi.name = in.name;
    for (int pid : in.pins) {
      gnl::Conn c;
      c.port = in.m->pins[db.pins[pid].idx].name;
      c.refs.push_back(netRef[db.pins[pid].net]);
      gi.conns.push_back(c);
    }
    m.insts.push_back(gi);
  }
  std::ofstream o(path);
  if (!o) throw std::runtime_error(path + ": cannot write");
  o << "// " << db.top << ": flat netlist after place and route. Written by mpnr.\n";
  gnl::write(m, o);
}

// A picture of the layout: cells, pins, and the wires of each layer.
inline void writeSvg(const DB& db, const phys::Design& d, const std::string& path) {
  std::ofstream o(path);
  if (!o) throw std::runtime_error(path + ": cannot write");
  double s = 1400.0 / std::max<int64_t>(1, std::max(db.die.w(), db.die.h()));
  double H = db.die.h() * s;
  auto X = [&](int64_t x) { return (x - db.die.x0) * s; };
  auto Y = [&](int64_t y) { return H - (y - db.die.y0) * s; };
  auto rect = [&](const Rect& r, const char* style) {
    o << "<rect x=\"" << X(r.x0) << "\" y=\"" << Y(r.y1) << "\" width=\"" << r.w() * s << "\" height=\"" << r.h() * s << "\" " << style << "/>\n";
  };
  static const char* colors[] = {"#3b6fd9", "#d9443b", "#2f9e5b", "#c48a12", "#8b46c9", "#1aa3a3", "#d94c9a", "#6b7a12", "#555", "#a0522d"};
  o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << db.die.w() * s << "\" height=\"" << H << "\" viewBox=\"0 0 " << db.die.w() * s << " " << H << "\">\n";
  rect(db.die, "fill=\"#fafafa\" stroke=\"#222\" stroke-width=\"1\"");
  rect(db.core, "fill=\"none\" stroke=\"#bbb\" stroke-width=\"0.5\" stroke-dasharray=\"4 3\"");
  o << "<g fill=\"#e4e7ee\" stroke=\"#8a93a6\" stroke-width=\"0.3\">\n";
  for (auto& in : db.insts) if (!in.filler) rect({in.x, in.y, in.x + in.m->w, in.y + in.m->h}, "");
  o << "</g>\n<g fill=\"#f4f4f4\" stroke=\"#ccc\" stroke-width=\"0.2\">\n";
  for (auto& in : db.insts) if (in.filler) rect({in.x, in.y, in.x + in.m->w, in.y + in.m->h}, "");
  o << "</g>\n<g fill=\"#222\">\n";
  for (auto& io : db.ios) rect(io.rect, "");
  o << "</g>\n";
  auto routing = db.tech.routing();
  for (size_t li = 0; li < routing.size(); li++) {
    const phys::Layer* l = routing[li];
    o << "<g stroke=\"" << colors[li % 10] << "\" stroke-opacity=\"0.75\" stroke-linecap=\"square\" fill=\"none\">\n";
    for (auto& n : d.nets)
      for (auto& sg : n.segs)
        if (sg.layer == l->name)
          o << "<line x1=\"" << X(sg.x0) << "\" y1=\"" << Y(sg.y0) << "\" x2=\"" << X(sg.x1) << "\" y2=\"" << Y(sg.y1) << "\" stroke-width=\"" << std::max(0.4, l->width * s) << "\"/>\n";
    o << "</g>\n";
  }
  o << "<g fill=\"#111\" fill-opacity=\"0.6\">\n";
  int64_t vs = routing[0]->width;
  for (auto& n : d.nets) for (auto& v : n.vias) rect({v.x - vs / 2, v.y - vs / 2, v.x + vs / 2, v.y + vs / 2}, "");
  o << "</g>\n</svg>\n";
}

}  // namespace pnr
