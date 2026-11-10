// DEF 5.8 reader and writer: floorplan, placement, pins and routed wiring.
//
// Header-only, shares its lexer and geometry types with lef.hpp. Lengths are
// integers in the DEF's own database units (Design::dbu per micron).
//
//   phys::Design d;
//   phys::readDef("top.def", d);
//   phys::writeDef("out.def", d);
//
// Routed wiring is kept as plain segments and vias, not as DEF paths: every
// "( x y ) ( x y )" pair of a path is one Seg, every via name one Via. The
// writer emits one path per segment or via, which every DEF reader accepts.
#pragma once
#include "lef.hpp"
#include <ostream>

namespace phys {

struct Row {
  std::string name, site, orient = "N";
  int64_t x = 0, y = 0, nx = 1, ny = 1, stepX = 0, stepY = 0;
};
struct Tracks {
  char axis = 'X';              // 'X': vertical tracks at x = start + i*step
  int64_t start = 0, count = 0, step = 0;
  std::vector<std::string> layers;
};
struct GCellGrid { char axis = 'X'; int64_t start = 0, count = 0, step = 0; };

struct Component {
  std::string name, macro, orient = "N";
  std::string status;           // PLACED, FIXED, COVER, or empty (UNPLACED)
  std::string source;           // DIST, NETLIST, TIMING, USER, or empty
  int64_t x = 0, y = 0;
};

struct IoPin {
  std::string name, net, dir = "INPUT", use = "SIGNAL";
  std::string layer;            // empty when the pin has no geometry
  Rect rect;                    // around the placement point
  std::string status;           // PLACED, FIXED, COVER, or empty
  std::string orient = "N";
  int64_t x = 0, y = 0;
  // Every shape of the pin (all "+ PORT" and "+ LAYER" entries), already in
  // design coordinates. layer/rect/x/y/orient above describe the first one.
  std::vector<LayerRect> shapes;
  // The first shape in design coordinates.
  Rect shape() const { return xform(rect, orient, 0, 0, x, y); }
};

struct Blockage {
  std::string layer;            // empty for a placement blockage
  Rect r;
};

struct Seg {
  std::string layer;
  int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int64_t width = 0;            // special nets and non-default rules; 0 = the layer's default width
  std::string shape;            // special nets: STRIPE, FOLLOWPIN, RING, ...
  int64_t ext0 = -1, ext1 = -1; // wire extension at each end, -1 = default (half the width)
  bool taper = false;           // TAPER: default width although the net has a rule
};
struct Via { std::string via, layer; int64_t x = 0, y = 0; };

struct NetConn { std::string inst, pin; };   // inst == "PIN" for a design pin

struct Net {
  std::string name, use = "SIGNAL";
  std::vector<NetConn> conns;
  std::string ndr;              // + NONDEFAULTRULE name, or empty
  std::string wiring;           // ROUTED, FIXED, COVER, or empty when unrouted
  std::vector<Seg> segs;
  std::vector<Via> vias;
  std::vector<LayerRect> rects; // RECT patches
};

struct Design {
  std::string name, version = "5.8", divider = "/", busbits = "[]";
  int dbu = 1000;
  Rect die;
  std::vector<std::pair<int64_t, int64_t>> diePoly;   // when DIEAREA is a polygon
  std::vector<Row> rows;
  std::vector<Tracks> tracks;
  std::vector<GCellGrid> gcells;
  std::vector<ViaDef> vias;     // vias the DEF defines itself (generated ones expanded to rects)
  std::map<std::string, Ndr> ndrs;   // NONDEFAULTRULES
  std::vector<Component> comps;
  std::vector<IoPin> pins;
  std::vector<Blockage> blockages;
  std::vector<Net> nets, specialNets;
};

namespace detail {

struct DefParser {
  Lexer lx;
  Design& d;
  DefParser(const std::string& path, Design& des) : lx(path, ";()"), d(des) {}

  int64_t ival() { return (int64_t)std::llround(lx.num()); }
  // "( x y )"; '*' repeats the previous value. An optional third value
  // (the wire extension) is read and dropped.
  void point(int64_t& x, int64_t& y, int64_t* ext = nullptr) {
    lx.expect("(");
    std::string a = lx.need(), b = lx.need();
    if (a != "*") x = (int64_t)std::llround(strtod(a.c_str(), nullptr));
    if (b != "*") y = (int64_t)std::llround(strtod(b.c_str(), nullptr));
    if (ext) *ext = -1;
    if (lx.peek() != ")") { std::string e = lx.need(); if (ext) *ext = (int64_t)std::llround(strtod(e.c_str(), nullptr)); }
    lx.expect(")");
  }
  // After a count: "; items... END NAME".
  void endSection(const char* name) { lx.expect(name); }

  // Wiring after ROUTED/FIXED/COVER/NEW, up to the next '+' or ';'.
  void wiring(Net& n, bool special) {
    for (;;) {
      Seg s;
      s.layer = lx.need();
      if (special) s.width = ival();
      else if (!n.ndr.empty()) { auto r = d.ndrs.find(n.ndr); if (r != d.ndrs.end()) { auto l = r->second.find(s.layer); if (l != r->second.end()) s.width = l->second.width; } }
      bool have = false;
      int64_t x = 0, y = 0, ext = -1, pext = -1;
      for (;;) {
        const std::string& p = lx.peek();
        if (p == "(") {
          int64_t px = x, py = y;
          pext = ext;
          point(x, y, &ext);
          if (have && (px != x || py != y)) { Seg q = s; q.x0 = px; q.y0 = py; q.x1 = x; q.y1 = y; q.ext0 = pext; q.ext1 = ext; n.segs.push_back(q); }
          have = true;
        } else if (p == "+" && special) {
          // "+ SHAPE x" may sit inside a special path, before the points.
          lx.need();
          if (lx.accept("SHAPE")) s.shape = lx.need();
          else if (lx.accept("STYLE")) lx.need();
          else { lx.hasPeek = true; lx.peeked = "+"; lx.peekQuoted = false; return; }
        } else if (p == "NEW" || p == ";" || p == "+" || p.empty()) break;
        else if (p == "TAPER") { lx.need(); s.taper = true; s.width = 0; }
        else if (p == "TAPERRULE") {
          lx.need();
          std::string rn = lx.need();
          s.width = 0;
          auto r = d.ndrs.find(rn);
          if (r != d.ndrs.end()) { auto l = r->second.find(s.layer); if (l != r->second.end()) s.width = l->second.width; }
        }
        else if (p == "STYLE" || p == "MASK") { lx.need(); lx.need(); }
        else if (p == "SHAPE") { lx.need(); s.shape = lx.need(); }
        else if (p == "VIRTUAL") { lx.need(); point(x, y); }
        else if (p == "RECT") {
          lx.need();
          if (lx.accept("MASK")) lx.need();
          lx.expect("(");
          int64_t a = ival(), b = ival(), c = ival(), e = ival();
          lx.expect(")");
          Rect r{x + a, y + b, x + c, y + e};
          r.normalize();
          n.rects.push_back({s.layer, r});
        } else {
          // A via name, optionally followed by an orientation or "DO n BY m STEP x y".
          std::string v = lx.need();
          n.vias.push_back({v, s.layer, x, y});
          static const char* orients[] = {"N", "S", "E", "W", "FN", "FS", "FE", "FW"};
          for (auto o : orients) if (lx.peek() == o) { lx.need(); break; }
          if (lx.accept("DO")) { int64_t nx = ival(); lx.expect("BY"); int64_t ny = ival(); lx.expect("STEP"); int64_t sx = ival(), sy = ival();
            for (int64_t i = 0; i < nx; i++) for (int64_t j = 0; j < ny; j++) if (i || j) n.vias.push_back({v, s.layer, x + i * sx, y + j * sy}); }
        }
      }
      if (!lx.accept("NEW")) return;
    }
  }

  void nets(std::vector<Net>& out, bool special) {
    ival(); lx.expect(";");
    while (lx.accept("-")) {
      Net n;
      n.name = lx.need();
      while (lx.peek() == "(") {
        lx.need();
        NetConn c;
        c.inst = lx.need(); c.pin = lx.need();
        if (lx.accept("+")) lx.need();          // + SYNTHESIZED
        lx.expect(")");
        n.conns.push_back(c);
      }
      for (;;) {
        std::string t = lx.need();
        if (t == ";") break;
        if (t != "+") continue;
        t = lx.need();
        if (t == "USE") n.use = lx.need();
        else if (t == "NONDEFAULTRULE") n.ndr = lx.need();
        else if (t == "RECT" && special) {
          std::string l = lx.need();
          Rect r;
          point(r.x0, r.y0); point(r.x1, r.y1);
          r.normalize();
          n.rects.push_back({l, r});
        }
        else if (t == "ROUTED" || t == "FIXED" || t == "COVER") { n.wiring = t; wiring(n, special); }
        // Everything else (SOURCE, WEIGHT, PROPERTY, SHIELDNET, ...) runs to the next '+'.
      }
      out.push_back(std::move(n));
    }
    lx.expect("END"); lx.need();
  }

  void components() {
    ival(); lx.expect(";");
    while (lx.accept("-")) {
      Component c;
      c.name = lx.need(); c.macro = lx.need();
      for (;;) {
        std::string t = lx.need();
        if (t == ";") break;
        if (t != "+") continue;
        t = lx.need();
        if (t == "PLACED" || t == "FIXED" || t == "COVER") { c.status = t; point(c.x, c.y); c.orient = lx.need(); }
        else if (t == "UNPLACED") c.status.clear();
        else if (t == "SOURCE") c.source = lx.need();
      }
      d.comps.push_back(std::move(c));
    }
    lx.expect("END"); lx.need();
  }

  void pins() {
    ival(); lx.expect(";");
    while (lx.accept("-")) {
      IoPin p;
      p.name = lx.need();
      // One "+ PORT" group: its rectangles, then where they are placed.
      std::vector<LayerRect> local;
      int64_t gx = 0, gy = 0;
      std::string gorient = "N";
      auto flush = [&]() {
        for (auto& s : local) p.shapes.push_back({s.layer, xform(s.r, gorient, 0, 0, gx, gy)});
        local.clear(); gx = gy = 0; gorient = "N";
      };
      for (;;) {
        std::string t = lx.need();
        if (t == ";") break;
        if (t != "+") continue;
        t = lx.need();
        if (t == "NET") p.net = lx.need();
        else if (t == "DIRECTION") p.dir = lx.need();
        else if (t == "USE") p.use = lx.need();
        else if (t == "PORT") flush();
        else if (t == "LAYER") {
          std::string l = lx.need();
          while (lx.peek() != "(") lx.need();      // MASK n, SPACING s, DESIGNRULEWIDTH w
          Rect r;
          point(r.x0, r.y0); point(r.x1, r.y1);
          r.normalize();
          if (p.layer.empty()) { p.layer = l; p.rect = r; }
          local.push_back({l, r});
        }
        else if (t == "PLACED" || t == "FIXED" || t == "COVER") {
          point(gx, gy); gorient = lx.need();
          if (p.status.empty()) { p.status = t; p.x = gx; p.y = gy; p.orient = gorient; }
        }
      }
      flush();
      d.pins.push_back(std::move(p));
    }
    lx.expect("END"); lx.need();
  }

  void blockages() {
    ival(); lx.expect(";");
    while (lx.accept("-")) {
      std::string layer;
      for (;;) {
        std::string t = lx.need();
        if (t == ";") break;
        if (t == "LAYER") layer = lx.need();
        else if (t == "PLACEMENT") layer.clear();
        else if (t == "RECT") {
          Rect r;
          point(r.x0, r.y0); point(r.x1, r.y1);
          r.normalize();
          d.blockages.push_back({layer, r});
        }
      }
    }
    lx.expect("END"); lx.need();
  }

  void vias() {
    ival(); lx.expect(";");
    while (lx.accept("-")) {
      ViaDef v;
      v.name = lx.need();
      // A generated via: "+ VIARULE r + CUTSIZE + LAYERS + CUTSPACING + ENCLOSURE [+ ROWCOL + ORIGIN + OFFSETS]".
      bool gen = false;
      int64_t cw = 0, ch = 0, sx = 0, sy = 0, be[2] = {0, 0}, te[2] = {0, 0}, rows = 1, cols = 1, ox = 0, oy = 0, off[4] = {0, 0, 0, 0};
      std::string lb, lc, lt;
      for (;;) {
        std::string t = lx.need();
        if (t == ";") break;
        if (t != "+") continue;
        t = lx.need();
        if (t == "VIARULE") { gen = true; lx.need(); }
        else if (t == "CUTSIZE") { cw = ival(); ch = ival(); }
        else if (t == "LAYERS") { lb = lx.need(); lc = lx.need(); lt = lx.need(); }
        else if (t == "CUTSPACING") { sx = ival(); sy = ival(); }
        else if (t == "ENCLOSURE") { be[0] = ival(); be[1] = ival(); te[0] = ival(); te[1] = ival(); }
        else if (t == "ROWCOL") { rows = ival(); cols = ival(); }
        else if (t == "ORIGIN") { ox = ival(); oy = ival(); }
        else if (t == "OFFSETS") { for (auto& f : off) f = ival(); }
        else if (t == "RECT") {
          std::string l = lx.need();
          if (lx.accept("+")) { lx.need(); lx.need(); }   // + MASK n
          Rect r;
          point(r.x0, r.y0); point(r.x1, r.y1);
          r.normalize();
          v.shapes.push_back({l, r});
        }
      }
      if (gen) {
        int64_t tw = cols * cw + (cols - 1) * sx, th = rows * ch + (rows - 1) * sy;
        int64_t x0 = ox - tw / 2, y0 = oy - th / 2;
        v.shapes.push_back({lb, {x0 - be[0] + off[0], y0 - be[1] + off[1], x0 + tw + be[0] + off[0], y0 + th + be[1] + off[1]}});
        for (int64_t r = 0; r < rows; r++)
          for (int64_t c = 0; c < cols; c++) {
            int64_t cx = x0 + c * (cw + sx), cy = y0 + r * (ch + sy);
            v.shapes.push_back({lc, {cx, cy, cx + cw, cy + ch}});
          }
        v.shapes.push_back({lt, {x0 - te[0] + off[2], y0 - te[1] + off[3], x0 + tw + te[0] + off[2], y0 + th + te[1] + off[3]}});
      }
      d.vias.push_back(std::move(v));
    }
    lx.expect("END"); lx.need();
  }

  void ndrs() {
    ival(); lx.expect(";");
    while (lx.accept("-")) {
      Ndr& r = d.ndrs[lx.need()];
      std::string layer;
      for (;;) {
        std::string t = lx.need();
        if (t == ";") break;
        if (t == "LAYER") layer = lx.need();
        else if (t == "WIDTH" && !layer.empty()) r[layer].width = ival();
        else if (t == "SPACING" && !layer.empty()) r[layer].spacing = ival();
      }
    }
    lx.expect("END"); lx.need();
  }

  void run() {
    std::string t;
    while (lx.next(t)) {
      if (t == "VERSION") { d.version = lx.need(); lx.skipStmt(); }
      else if (t == "DIVIDERCHAR") { d.divider = lx.need(); lx.skipStmt(); }
      else if (t == "BUSBITCHARS") { d.busbits = lx.need(); lx.skipStmt(); }
      else if (t == "DESIGN") { d.name = lx.need(); lx.skipStmt(); }
      else if (t == "UNITS") { lx.expect("DISTANCE"); lx.expect("MICRONS"); d.dbu = (int)ival(); lx.skipStmt(); }
      else if (t == "DIEAREA") {
        while (lx.peek() == "(") { int64_t x = 0, y = 0; point(x, y); d.diePoly.push_back({x, y}); }
        lx.skipStmt();
        if (d.diePoly.empty()) lx.fail("DIEAREA has no points");
        d.die = {d.diePoly[0].first, d.diePoly[0].second, d.diePoly[0].first, d.diePoly[0].second};
        for (auto& q : d.diePoly) {
          d.die.x0 = std::min(d.die.x0, q.first); d.die.x1 = std::max(d.die.x1, q.first);
          d.die.y0 = std::min(d.die.y0, q.second); d.die.y1 = std::max(d.die.y1, q.second);
        }
        if (d.diePoly.size() == 2) d.diePoly.clear();
      }
      else if (t == "ROW") {
        Row r;
        r.name = lx.need(); r.site = lx.need(); r.x = ival(); r.y = ival(); r.orient = lx.need();
        if (lx.accept("DO")) { r.nx = ival(); lx.expect("BY"); r.ny = ival(); if (lx.accept("STEP")) { r.stepX = ival(); r.stepY = ival(); } }
        lx.skipStmt();
        d.rows.push_back(r);
      }
      else if (t == "TRACKS") {
        Tracks k;
        k.axis = lx.need() == "X" ? 'X' : 'Y';
        k.start = ival(); lx.expect("DO"); k.count = ival(); lx.expect("STEP"); k.step = ival();
        if (lx.accept("MASK")) { lx.need(); lx.accept("SAMEMASK"); }
        if (lx.accept("LAYER")) while (lx.peek() != ";") k.layers.push_back(lx.need());
        lx.skipStmt();
        d.tracks.push_back(k);
      }
      else if (t == "GCELLGRID") {
        GCellGrid g;
        g.axis = lx.need() == "X" ? 'X' : 'Y';
        g.start = ival(); lx.expect("DO"); g.count = ival(); lx.expect("STEP"); g.step = ival();
        lx.skipStmt();
        d.gcells.push_back(g);
      }
      else if (t == "VIAS") vias();
      else if (t == "COMPONENTS") components();
      else if (t == "PINS") pins();
      else if (t == "BLOCKAGES") blockages();
      else if (t == "SPECIALNETS") nets(d.specialNets, true);
      else if (t == "NETS") nets(d.nets, false);
      else if (t == "NONDEFAULTRULES") ndrs();
      else if (t == "PROPERTYDEFINITIONS" || t == "REGIONS" || t == "PINPROPERTIES" ||
               t == "SLOTS" || t == "FILLS" || t == "SCANCHAINS" || t == "GROUPS" || t == "STYLES")
        lx.skipBlock(t);
      else if (t == "BEGINEXT") { while (lx.next(t) && t != "ENDEXT") {} }
      else if (t == "END") { lx.need(); break; }   // END DESIGN
      else lx.skipStmt();
    }
  }
};

}  // namespace detail

// Read a DEF file. Throws std::runtime_error("file:line: message").
inline void readDef(const std::string& path, Design& d) {
  detail::DefParser p(path, d);
  p.run();
}

inline void writeDef(std::ostream& o, const Design& d) {
  auto pt = [&](int64_t x, int64_t y) { o << "( " << x << ' ' << y << " )"; };
  o << "VERSION " << d.version << " ;\nDIVIDERCHAR \"" << d.divider << "\" ;\nBUSBITCHARS \"" << d.busbits << "\" ;\n";
  o << "DESIGN " << d.name << " ;\nUNITS DISTANCE MICRONS " << d.dbu << " ;\n";
  o << "DIEAREA ";
  if (d.diePoly.empty()) { pt(d.die.x0, d.die.y0); o << ' '; pt(d.die.x1, d.die.y1); }
  else for (auto& q : d.diePoly) { pt(q.first, q.second); o << ' '; }
  o << " ;\n";
  for (auto& r : d.rows)
    o << "ROW " << r.name << ' ' << r.site << ' ' << r.x << ' ' << r.y << ' ' << r.orient << " DO " << r.nx << " BY " << r.ny
      << " STEP " << r.stepX << ' ' << r.stepY << " ;\n";
  for (auto& k : d.tracks) {
    o << "TRACKS " << k.axis << ' ' << k.start << " DO " << k.count << " STEP " << k.step;
    if (!k.layers.empty()) { o << " LAYER"; for (auto& l : k.layers) o << ' ' << l; }
    o << " ;\n";
  }
  for (auto& g : d.gcells) o << "GCELLGRID " << g.axis << ' ' << g.start << " DO " << g.count << " STEP " << g.step << " ;\n";
  if (!d.ndrs.empty()) {
    o << "NONDEFAULTRULES " << d.ndrs.size() << " ;\n";
    for (auto& [name, r] : d.ndrs) {
      o << "- " << name;
      for (auto& [l, v] : r) { o << "\n  + LAYER " << l << " WIDTH " << v.width; if (v.spacing) o << " SPACING " << v.spacing; }
      o << " ;\n";
    }
    o << "END NONDEFAULTRULES\n";
  }
  if (!d.vias.empty()) {
    o << "VIAS " << d.vias.size() << " ;\n";
    for (auto& v : d.vias) {
      o << "- " << v.name;
      for (auto& s : v.shapes) { o << "\n  + RECT " << s.layer << ' '; pt(s.r.x0, s.r.y0); o << ' '; pt(s.r.x1, s.r.y1); }
      o << " ;\n";
    }
    o << "END VIAS\n";
  }
  o << "COMPONENTS " << d.comps.size() << " ;\n";
  for (auto& c : d.comps) {
    o << "- " << c.name << ' ' << c.macro;
    if (!c.source.empty()) o << " + SOURCE " << c.source;
    if (!c.status.empty()) { o << " + " << c.status << ' '; pt(c.x, c.y); o << ' ' << c.orient; }
    o << " ;\n";
  }
  o << "END COMPONENTS\n";
  o << "PINS " << d.pins.size() << " ;\n";
  for (auto& p : d.pins) {
    o << "- " << p.name << " + NET " << (p.net.empty() ? p.name : p.net) << " + DIRECTION " << p.dir << " + USE " << p.use;
    if (!p.layer.empty()) { o << "\n  + LAYER " << p.layer << ' '; pt(p.rect.x0, p.rect.y0); o << ' '; pt(p.rect.x1, p.rect.y1); }
    if (!p.status.empty()) { o << "\n  + " << p.status << ' '; pt(p.x, p.y); o << ' ' << p.orient; }
    o << " ;\n";
  }
  o << "END PINS\n";
  if (!d.blockages.empty()) {
    o << "BLOCKAGES " << d.blockages.size() << " ;\n";
    for (auto& b : d.blockages) {
      o << "- " << (b.layer.empty() ? std::string("PLACEMENT") : "LAYER " + b.layer) << " RECT ";
      pt(b.r.x0, b.r.y0); o << ' '; pt(b.r.x1, b.r.y1); o << " ;\n";
    }
    o << "END BLOCKAGES\n";
  }
  auto nets = [&](const std::vector<Net>& ns, const char* section, bool special) {
    if (ns.empty() && special) return;
    o << section << ' ' << ns.size() << " ;\n";
    for (auto& n : ns) {
      o << "- " << n.name;
      int col = 0;
      for (auto& c : n.conns) { o << (col++ % 4 == 0 ? "\n  " : " ") << "( " << c.inst << ' ' << c.pin << " )"; }
      if (n.use != "SIGNAL" || special) o << "\n  + USE " << n.use;
      if (!n.ndr.empty()) o << "\n  + NONDEFAULTRULE " << n.ndr;
      bool first = true;
      auto lead = [&]() { o << "\n  " << (first ? "+ " + (n.wiring.empty() ? std::string("ROUTED") : n.wiring) : std::string("  NEW")) << ' '; first = false; };
      for (auto& s : n.segs) {
        lead();
        o << s.layer << ' ';
        if (special) { o << s.width << ' '; if (!s.shape.empty()) o << "+ SHAPE " << s.shape << ' '; }
        if (s.taper) o << "TAPER ";
        o << "( " << s.x0 << ' ' << s.y0;
        if (s.ext0 >= 0) o << ' ' << s.ext0;
        o << " ) ( " << (s.x1 == s.x0 ? std::string("*") : std::to_string(s.x1)) << ' '
          << (s.y1 == s.y0 ? std::string("*") : std::to_string(s.y1));
        if (s.ext1 >= 0) o << ' ' << s.ext1;
        o << " )";
      }
      for (auto& v : n.vias) {
        lead();
        o << v.layer << ' ';
        if (special) o << "0 ";
        pt(v.x, v.y); o << ' ' << v.via;
      }
      for (auto& r : n.rects) {
        lead();
        o << r.layer << ' ';
        if (special) o << "0 ";
        pt(r.r.x0, r.r.y0); o << " RECT ( 0 0 " << r.r.x1 - r.r.x0 << ' ' << r.r.y1 - r.r.y0 << " )";
      }
      o << " ;\n";
    }
    o << "END " << section << '\n';
  };
  nets(d.specialNets, "SPECIALNETS", true);
  nets(d.nets, "NETS", false);
  o << "END DESIGN\n";
}

inline void writeDef(const std::string& path, const Design& d) {
  std::ofstream o(path);
  if (!o) throw std::runtime_error(path + ": cannot write");
  writeDef(o, d);
}

}  // namespace phys
