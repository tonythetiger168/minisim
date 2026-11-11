// LEF 5.8 reader: technology (layers, vias, sites) and cell macros.
//
// Header-only, no dependencies. Shared by the physical tools (mpnr place and
// route, parasitic extraction). Lengths are stored as integers in database
// units: Tech::dbu per micron, taken from "UNITS DATABASE MICRONS" (1000 when
// the file has none). Electrical values keep the LEF units: ohm per square,
// pF per square micron, pF per micron, ohm per cut.
//
//   phys::Tech tech;
//   phys::readLef("tech.lef", tech);     // may be called for several files
//   phys::readLef("cells.lef", tech);
//   const phys::Macro* m = tech.macro("NAND2_X1");
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace phys {

struct Rect {
  int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int64_t w() const { return x1 - x0; }
  int64_t h() const { return y1 - y0; }
  bool overlaps(const Rect& o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
  bool contains(int64_t x, int64_t y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  void normalize() { if (x0 > x1) std::swap(x0, x1); if (y0 > y1) std::swap(y0, y1); }
};
struct LayerRect { std::string layer; Rect r; };

struct Layer {
  enum Type { Routing, Cut, Masterslice, Overlap, Other };
  std::string name;
  Type type = Other;
  char dir = 0;                 // 'H' or 'V' for routing layers, 0 when not given
  int64_t pitchX = 0, pitchY = 0, offsetX = 0, offsetY = 0;
  bool hasOffset = false;       // OFFSET given (0 is a real value then)
  int64_t width = 0, spacing = 0, minArea = 0;   // minArea in dbu^2
  // Electrical values in LEF units; -1 when the LEF does not give one.
  double rPerSq = -1;           // routing: ohm per square
  double cPerSqDist = -1;       // routing: pF per square micron
  double edgeCap = -1;          // routing: pF per micron of edge
  double cutRes = -1;           // cut: ohm per cut
  double thickness = -1, height = -1;   // microns
  int index = -1;               // position in Tech::layers
  int routingLevel = 0;         // 1 for the first routing layer, 0 for others
  int64_t pitch() const { return dir == 'H' ? pitchY : pitchX; }
  int64_t offset() const { return dir == 'H' ? offsetY : offsetX; }
};

struct ViaDef {
  std::string name;
  bool isDefault = false;
  std::vector<LayerRect> shapes;   // around the via origin
  double res = -1;                 // ohm, -1 when the via has no RESISTANCE
};

struct Site { std::string name, cls; int64_t w = 0, h = 0; };

struct MacroPin {
  std::string name;
  std::string dir = "INPUT";       // INPUT, OUTPUT, INOUT, FEEDTHRU
  std::string use = "SIGNAL";      // SIGNAL, POWER, GROUND, CLOCK, ANALOG
  std::string shape;
  std::vector<LayerRect> ports;    // from the macro's lower left corner
};

struct Macro {
  std::string name, cls, site;
  int64_t w = 0, h = 0;
  bool symX = false, symY = false, symR90 = false;
  std::vector<MacroPin> pins;
  std::vector<LayerRect> obs;
  const MacroPin* pin(const std::string& n) const {
    for (auto& p : pins) if (p.name == n) return &p;
    return nullptr;
  }
  bool isCore() const { return cls.rfind("CORE", 0) == 0; }
  bool isFiller() const { return cls.find("SPACER") != std::string::npos; }
};

// A non-default routing rule: wire width and spacing per layer.
struct NdrLayer { int64_t width = 0, spacing = 0; };
using Ndr = std::map<std::string, NdrLayer>;

struct Tech {
  int dbu = 1000;
  bool dbuSet = false;
  int64_t mfgGrid = 0;
  std::vector<Layer> layers;                       // in file order (bottom up)
  std::vector<ViaDef> vias;
  std::map<std::string, Site> sites;
  std::map<std::string, Macro> macros;
  std::map<std::string, Ndr> ndrs;                 // NONDEFAULTRULE name -> layers

  const Layer* layer(const std::string& n) const {
    for (auto& l : layers) if (l.name == n) return &l;
    return nullptr;
  }
  // The routing layers, bottom up.
  std::vector<const Layer*> routing() const {
    std::vector<const Layer*> r;
    for (auto& l : layers) if (l.type == Layer::Routing) r.push_back(&l);
    return r;
  }
  const Macro* macro(const std::string& n) const {
    auto it = macros.find(n);
    return it == macros.end() ? nullptr : &it->second;
  }
  const ViaDef* via(const std::string& n) const {
    for (auto& v : vias) if (v.name == n) return &v;
    return nullptr;
  }
  // The cut layer between two adjacent routing layers, or null.
  const Layer* cutBetween(const Layer& lo, const Layer& hi) const {
    for (int i = lo.index + 1; i < hi.index; i++)
      if (layers[i].type == Layer::Cut) return &layers[i];
    return nullptr;
  }
  // A via joining routing layer `lo` to the next routing layer up: the first
  // DEFAULT via with a shape on both, else the first via of any kind.
  const ViaDef* viaBetween(const std::string& lo, const std::string& hi) const {
    const ViaDef* any = nullptr;
    for (auto& v : vias) {
      bool a = false, b = false;
      for (auto& s : v.shapes) { a |= s.layer == lo; b |= s.layer == hi; }
      if (!a || !b) continue;
      if (v.isDefault) return &v;
      if (!any) any = &v;
    }
    return any;
  }
  const Site* coreSite() const {
    for (auto& [n, s] : sites) if (s.cls == "CORE") return &s;
    return sites.empty() ? nullptr : &sites.begin()->second;
  }
};

// Place a macro-local rectangle in the design: the macro (size w by h) sits
// with its lower left corner at (x, y) after the DEF orientation is applied.
inline Rect xform(Rect r, const std::string& orient, int64_t w, int64_t h, int64_t x, int64_t y) {
  Rect o = r;
  if (orient == "N") {}
  else if (orient == "S") o = {w - r.x1, h - r.y1, w - r.x0, h - r.y0};
  else if (orient == "FN") o = {w - r.x1, r.y0, w - r.x0, r.y1};
  else if (orient == "FS") o = {r.x0, h - r.y1, r.x1, h - r.y0};
  else if (orient == "W") o = {h - r.y1, r.x0, h - r.y0, r.x1};
  else if (orient == "E") o = {r.y0, w - r.x1, r.y1, w - r.x0};
  else if (orient == "FW") o = {r.y0, r.x0, r.y1, r.x1};
  else if (orient == "FE") o = {h - r.y1, w - r.x1, h - r.y0, w - r.x0};
  return {o.x0 + x, o.y0 + y, o.x1 + x, o.y1 + y};
}

namespace detail {

// Token stream for LEF and DEF: blank-separated words, '#' comments, quoted
// strings as one token, and the given punctuation characters as tokens of
// their own.
struct Lexer {
  std::string file, text, punct;
  size_t pos = 0;
  int line = 1;
  std::string peeked;
  bool hasPeek = false, peekQuoted = false, quoted = false;

  Lexer(const std::string& path, const char* punctChars) : file(path), punct(punctChars) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(path + ": cannot open");
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  [[noreturn]] void fail(const std::string& msg) const {
    throw std::runtime_error(file + ":" + std::to_string(line) + ": " + msg);
  }
  bool raw(std::string& t) {
    quoted = false;
    for (;;) {
      while (pos < text.size() && isspace((unsigned char)text[pos])) { if (text[pos] == '\n') line++; pos++; }
      if (pos < text.size() && text[pos] == '#') { while (pos < text.size() && text[pos] != '\n') pos++; continue; }
      break;
    }
    if (pos >= text.size()) return false;
    char c = text[pos];
    if (c == '"') {
      size_t e = pos + 1;
      while (e < text.size() && text[e] != '"') { if (text[e] == '\n') line++; if (text[e] == '\\') e++; e++; }
      t = text.substr(pos + 1, e - pos - 1);
      pos = std::min(e + 1, text.size());
      quoted = true;
      return true;
    }
    if (punct.find(c) != std::string::npos) { t = std::string(1, c); pos++; return true; }
    size_t e = pos;
    while (e < text.size() && !isspace((unsigned char)text[e]) && punct.find(text[e]) == std::string::npos) {
      if (text[e] == '\\' && e + 1 < text.size()) e++;   // an escaped character stays in the name
      e++;
    }
    t = text.substr(pos, e - pos);
    pos = e;
    return true;
  }
  bool next(std::string& t) {
    if (hasPeek) { t = peeked; hasPeek = false; quoted = peekQuoted; return true; }
    return raw(t);
  }
  std::string need() {
    std::string t;
    if (!next(t)) fail("unexpected end of file");
    return t;
  }
  const std::string& peek() {
    if (!hasPeek) { if (!raw(peeked)) peeked.clear(); hasPeek = true; peekQuoted = quoted; }
    return peeked;
  }
  bool accept(const char* s) {
    if (peek() == s && !peekQuoted) { hasPeek = false; return true; }
    return false;
  }
  void expect(const char* s) {
    std::string t = need();
    if (t != s) fail(std::string("expected '") + s + "' but found '" + t + "'");
  }
  double num() {
    std::string t = need();
    char* end = nullptr;
    double v = strtod(t.c_str(), &end);
    if (end == t.c_str() || *end) fail("expected a number but found '" + t + "'");
    return v;
  }
  bool isNum(const std::string& t) const {
    if (t.empty()) return false;
    char* end = nullptr;
    strtod(t.c_str(), &end);
    return end != t.c_str() && !*end;
  }
  void skipStmt() { std::string t; while (next(t)) if (t == ";" && !quoted) return; }
  // Skip a block that closes with "END <name>".
  void skipBlock(const std::string& name) {
    std::string t;
    while (next(t))
      if (t == "END" && !quoted && peek() == name) { hasPeek = false; return; }
    fail("missing END " + name);
  }
};

// A rectilinear polygon as rectangles (horizontal slabs).
inline std::vector<Rect> polygonRects(const std::vector<std::pair<int64_t, int64_t>>& p) {
  std::vector<Rect> out;
  std::vector<int64_t> ys;
  for (auto& q : p) ys.push_back(q.second);
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
  size_t n = p.size();
  for (size_t s = 0; s + 1 < ys.size(); s++) {
    int64_t ya = ys[s], yb = ys[s + 1];
    std::vector<int64_t> xs;
    for (size_t i = 0; i < n; i++) {
      auto a = p[i], b = p[(i + 1) % n];
      if (a.first != b.first) continue;                 // vertical edges only
      int64_t lo = std::min(a.second, b.second), hi = std::max(a.second, b.second);
      if (lo <= ya && hi >= yb) xs.push_back(a.first);
    }
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i + 1 < xs.size(); i += 2)
      if (xs[i] < xs[i + 1]) out.push_back({xs[i], ya, xs[i + 1], yb});
  }
  return out;
}

struct LefParser {
  Lexer lx;
  Tech& tech;
  LefParser(const std::string& path, Tech& t) : lx(path, ";"), tech(t) {}

  int64_t du(double microns) const { return (int64_t)std::llround(microns * tech.dbu); }
  int64_t len() { return du(lx.num()); }

  // LAYER / RECT / POLYGON statements up to the END of a PORT, OBS or VIA.
  // Returns at END (consumed); `ox, oy` shift the shapes (the macro ORIGIN).
  void geometry(std::vector<LayerRect>& out, int64_t ox, int64_t oy, double* res = nullptr) {
    std::string layer, t;
    for (;;) {
      t = lx.need();
      if (t == "END") return;
      if (t == "LAYER") { layer = lx.need(); lx.skipStmt(); }
      else if (t == "RECT") {
        if (lx.accept("MASK")) lx.need();
        Rect r;
        r.x0 = len() + ox; r.y0 = len() + oy; r.x1 = len() + ox; r.y1 = len() + oy;
        r.normalize();
        out.push_back({layer, r});
        lx.skipStmt();
      } else if (t == "POLYGON") {
        if (lx.accept("MASK")) lx.need();
        std::vector<std::pair<int64_t, int64_t>> pts;
        while (lx.peek() != ";" && lx.isNum(lx.peek())) {
          int64_t x = len() + ox, y = len() + oy;
          pts.push_back({x, y});
        }
        lx.skipStmt();
        for (auto& r : polygonRects(pts)) out.push_back({layer, r});
      } else if (t == "RESISTANCE" && res) { *res = lx.num(); lx.skipStmt(); }
      else lx.skipStmt();
    }
  }

  void layer() {
    Layer l;
    l.name = lx.need();
    std::string t;
    for (;;) {
      t = lx.need();
      if (t == "END") { lx.need(); break; }
      if (t == "TYPE") {
        std::string v = lx.need();
        l.type = v == "ROUTING" ? Layer::Routing : v == "CUT" ? Layer::Cut
               : v == "MASTERSLICE" ? Layer::Masterslice : v == "OVERLAP" ? Layer::Overlap : Layer::Other;
      } else if (t == "DIRECTION") { l.dir = lx.need() == "HORIZONTAL" ? 'H' : 'V'; }
      else if (t == "PITCH") { l.pitchX = len(); l.pitchY = lx.peek() == ";" ? l.pitchX : len(); }
      else if (t == "OFFSET") { l.hasOffset = true; l.offsetX = len(); l.offsetY = lx.peek() == ";" ? l.offsetX : len(); }
      else if (t == "WIDTH") { l.width = len(); }
      else if (t == "AREA") { l.minArea = (int64_t)std::llround(lx.num() * tech.dbu * tech.dbu); }
      else if (t == "SPACING") { int64_t s = len(); if (!l.spacing || lx.peek() == ";") l.spacing = s; }   // a plain SPACING wins over ranged ones
      else if (t == "SPACINGTABLE") {
        // PARALLELRUNLENGTH l0 l1 ... WIDTH w s0 s1 ... : the first row's first spacing.
        while (lx.peek() != ";" && lx.peek() != "WIDTH") lx.need();
        if (lx.accept("WIDTH")) { lx.num(); int64_t s = len(); if (!l.spacing) l.spacing = s; }
      }
      else if (t == "RESISTANCE") {
        if (lx.accept("RPERSQ")) l.rPerSq = lx.num(); else l.cutRes = lx.num();
      }
      else if (t == "CAPACITANCE") { if (lx.accept("CPERSQDIST")) l.cPerSqDist = lx.num(); }
      else if (t == "EDGECAPACITANCE") { l.edgeCap = lx.num(); }
      else if (t == "THICKNESS") { l.thickness = lx.num(); }
      else if (t == "HEIGHT") { l.height = lx.num(); }
      else if (t == "PROPERTY") {}
      lx.skipStmt();
    }
    for (auto& e : tech.layers)
      if (e.name == l.name) { l.index = e.index; l.routingLevel = e.routingLevel; e = l; return; }
    l.index = (int)tech.layers.size();
    if (l.type == Layer::Routing) {
      int n = 0;
      for (auto& e : tech.layers) n += e.type == Layer::Routing;
      l.routingLevel = n + 1;
    }
    tech.layers.push_back(l);
  }

  void via() {
    ViaDef v;
    v.name = lx.need();
    if (lx.accept("DEFAULT")) v.isDefault = true;
    if (lx.accept("GENERATED")) {}
    geometry(v.shapes, 0, 0, &v.res);
    lx.need();
    for (auto& e : tech.vias) if (e.name == v.name) { e = v; return; }
    tech.vias.push_back(v);
  }

  void site() {
    Site s;
    s.name = lx.need();
    std::string t;
    for (;;) {
      t = lx.need();
      if (t == "END") { lx.need(); break; }
      if (t == "CLASS") s.cls = lx.need();
      else if (t == "SIZE") { s.w = len(); lx.expect("BY"); s.h = len(); }
      lx.skipStmt();
    }
    tech.sites[s.name] = s;
  }

  void macro() {
    Macro m;
    m.name = lx.need();
    int64_t ox = 0, oy = 0;
    std::string t;
    for (;;) {
      t = lx.need();
      if (t == "END") { lx.need(); break; }
      if (t == "CLASS") {
        while (lx.peek() != ";") { if (!m.cls.empty()) m.cls += ' '; m.cls += lx.need(); }
        lx.skipStmt();
      } else if (t == "ORIGIN") { ox = len(); oy = len(); lx.skipStmt(); }
      else if (t == "SIZE") { m.w = len(); lx.expect("BY"); m.h = len(); lx.skipStmt(); }
      else if (t == "SITE") { m.site = lx.need(); lx.skipStmt(); }
      else if (t == "SYMMETRY") {
        while (lx.peek() != ";") { std::string s = lx.need(); m.symX |= s == "X"; m.symY |= s == "Y"; m.symR90 |= s == "R90"; }
        lx.skipStmt();
      } else if (t == "PIN") {
        MacroPin p;
        p.name = lx.need();
        for (;;) {
          t = lx.need();
          if (t == "END") { lx.need(); break; }
          if (t == "DIRECTION") { p.dir = lx.need(); lx.skipStmt(); }
          else if (t == "USE") { p.use = lx.need(); lx.skipStmt(); }
          else if (t == "SHAPE") { p.shape = lx.need(); lx.skipStmt(); }
          else if (t == "PORT") {
            if (lx.accept("CLASS")) lx.skipStmt();
            geometry(p.ports, ox, oy);
          }
          else lx.skipStmt();
        }
        m.pins.push_back(std::move(p));
      } else if (t == "OBS") { geometry(m.obs, ox, oy); }
      else lx.skipStmt();
    }
    tech.macros[m.name] = std::move(m);
  }

  void ndr() {
    std::string name = lx.need(), t;
    Ndr& r = tech.ndrs[name];
    for (;;) {
      t = lx.need();
      if (t == "END") { lx.need(); break; }
      if (t == "LAYER") {
        std::string l = lx.need();
        for (;;) {
          t = lx.need();
          if (t == "END") { lx.need(); break; }
          if (t == "WIDTH") r[l].width = len();
          else if (t == "SPACING") r[l].spacing = len();
          lx.skipStmt();
        }
      } else if (t == "VIA") { ViaDef v; v.name = lx.need(); if (lx.accept("DEFAULT")) {} geometry(v.shapes, 0, 0, &v.res); lx.need(); tech.vias.push_back(v); }
      else lx.skipStmt();
    }
  }

  void run() {
    std::string t;
    while (lx.next(t)) {
      if (t == "UNITS") {
        for (;;) {
          t = lx.need();
          if (t == "END") { lx.need(); break; }
          if (t == "DATABASE") {
            lx.expect("MICRONS");
            int v = (int)lx.num();
            if (tech.dbuSet && v != tech.dbu) lx.fail("DATABASE MICRONS differs from an earlier LEF file");
            tech.dbu = v; tech.dbuSet = true;
          }
          lx.skipStmt();
        }
      } else if (t == "MANUFACTURINGGRID") { tech.mfgGrid = len(); lx.skipStmt(); }
      else if (t == "LAYER") layer();
      else if (t == "VIA") via();
      else if (t == "SITE") site();
      else if (t == "MACRO") macro();
      else if (t == "VIARULE") lx.skipBlock(lx.need());
      else if (t == "NONDEFAULTRULE") ndr();
      else if (t == "SPACING" || t == "PROPERTYDEFINITIONS") lx.skipBlock(t);
      else if (t == "BEGINEXT") { while (lx.next(t) && t != "ENDEXT") {} }
      else if (t == "END") { lx.need(); break; }   // END LIBRARY
      else lx.skipStmt();
    }
  }
};

}  // namespace detail

// Read a LEF file into `tech`, adding to what earlier files defined. Throws
// std::runtime_error("file:line: message") on a syntax error.
inline void readLef(const std::string& path, Tech& tech) {
  detail::LefParser p(path, tech);
  p.run();
}

}  // namespace phys
