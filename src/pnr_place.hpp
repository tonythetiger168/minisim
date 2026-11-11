// mpnr floorplan and placement.
//
//   floorplan()  die and core from the cell area, rows of the core site,
//                design ports on the die edge
//   globalPlace() analytic placement: a quadratic wire length model (bound to
//                bound nets) solved by conjugate gradients, pulled towards a
//                spread-out copy of itself a little more each round
//   legalize()   Abacus: each cell goes to the row where it moves least, rows
//                are packed by a least-squares cluster pass
//   refine()     swaps and reorders neighbours while wire length drops
//   addFillers() filler cells in the gaps
#pragma once
#include "pnr_db.hpp"
#include "parallel.hpp"
#include <cstdio>

namespace pnr {

struct PlaceOptions {
  double util = 0.6;            // cell area / core area
  double aspect = 1.0;          // core height / width
  double marginUm = -1;         // core to die; -1: four row heights
  int maxIter = 60;
  bool fillers = true;
  bool verbose = false;
};

// ---- floorplan -----------------------------------------------------------

inline int64_t snapDown(int64_t v, int64_t step) { return step > 0 ? v / step * step : v; }
inline int64_t snapUp(int64_t v, int64_t step) { return step > 0 ? (v + step - 1) / step * step : v; }

// Track coordinates of a routing layer across the die: offset + k * pitch.
inline std::vector<int64_t> layerTracks(const phys::Layer& l, const Rect& die) {
  std::vector<int64_t> t;
  int64_t pitch = l.pitch(), lo = l.dir == 'H' ? die.y0 : die.x0, hi = l.dir == 'H' ? die.y1 : die.x1;
  if (pitch <= 0) return t;
  int64_t off = l.hasOffset ? l.offset() : pitch / 2;
  int64_t first = lo + ((off - lo) % pitch + pitch) % pitch;
  // Keep a wire's half width inside the die.
  for (int64_t c = first; c + l.width / 2 <= hi; c += pitch)
    if (c - l.width / 2 >= lo) t.push_back(c);
  return t;
}

inline void makeRows(DB& db) {
  const phys::Site* site = db.tech.coreSite();
  db.rows.clear();
  int n = (int)(db.core.h() / site->h);
  for (int r = 0; r < n; r++) {
    phys::Row row;
    row.name = "ROW_" + std::to_string(r);
    row.site = site->name;
    row.x = db.core.x0; row.y = db.core.y0 + r * site->h;
    row.orient = r % 2 ? "FS" : "N";
    row.nx = db.core.w() / site->w; row.ny = 1; row.stepX = site->w; row.stepY = 0;
    db.rows.push_back(row);
  }
}

// Puts the design ports on the die edge: inputs on the left and bottom,
// outputs on the right and top, evenly spread, each on a routing track of a
// layer that runs away from the edge.
inline void placeIos(DB& db, int minLayer) {
  auto routing = db.tech.routing();
  const phys::Layer* hl = nullptr; const phys::Layer* vl = nullptr;
  for (auto l : routing) {
    if (l->routingLevel < minLayer) continue;
    if (l->dir == 'H' && !hl) hl = l;
    if (l->dir == 'V' && !vl) vl = l;
  }
  if (!hl || !vl) throw std::runtime_error("the LEF needs a horizontal and a vertical routing layer for the design ports");
  std::vector<int> ins, outs;
  for (size_t i = 0; i < db.ios.size(); i++) {
    if (db.ios[i].placed) continue;
    (db.ios[i].dir == "OUTPUT" ? outs : ins).push_back((int)i);
  }
  std::vector<int64_t> ht = layerTracks(*hl, db.die), vt = layerTracks(*vl, db.die);
  // side 0 left, 1 bottom, 2 right, 3 top.
  int64_t depth = 4 * std::max(hl->pitch(), vl->pitch());
  auto rectAt = [&](int side, int64_t c) {
    int64_t hw = (side % 2 == 0 ? hl : vl)->width / 2;
    if (side == 0) return Rect{db.die.x0, c - hw, db.die.x0 + depth, c + hw};
    if (side == 2) return Rect{db.die.x1 - depth, c - hw, db.die.x1, c + hw};
    if (side == 1) return Rect{c - hw, db.die.y0, c + hw, db.die.y0 + depth};
    return Rect{c - hw, db.die.y1 - depth, c + hw, db.die.y1};
  };
  // The tracks of a side that are clear of ports placed before (by a
  // floorplan), with a free track on either side.
  auto freeTracks = [&](int side) {
    const std::vector<int64_t>& tr = side % 2 == 0 ? ht : vt;
    const phys::Layer* l = side % 2 == 0 ? hl : vl;
    std::vector<int64_t> out;
    for (int64_t c : tr) {
      Rect r = rectAt(side, c);
      int64_t g = l->pitch() + l->spacing;
      Rect grown{r.x0 - g, r.y0 - g, r.x1 + g, r.y1 + g};
      bool clash = false;
      for (auto& io : db.ios) if (io.placed && io.layer == l->name && grown.overlaps(io.rect)) { clash = true; break; }
      if (!clash) out.push_back(c);
    }
    return out;
  };
  auto put = [&](const std::vector<int>& list, int sideA, int sideB) {
    std::vector<int64_t> trA = freeTracks(sideA), trB = freeTracks(sideB);
    size_t capA = trA.size() / 2, capB = trB.size() / 2;
    if (list.size() > capA + capB)
      throw std::runtime_error("the die edge is too short for " + std::to_string(list.size()) + " ports; lower -util or place the ports in a floorplan DEF");
    size_t nA = std::min(capA, (list.size() * capA + capA + capB - 1) / std::max<size_t>(1, capA + capB));
    nA = std::min(nA, list.size());
    if (list.size() - nA > capB) nA = list.size() - capB;
    for (int pass = 0; pass < 2; pass++) {
      int side = pass ? sideB : sideA;
      size_t begin = pass ? nA : 0, end = pass ? list.size() : nA, cnt = end - begin;
      const std::vector<int64_t>& tr = pass ? trB : trA;
      for (size_t k = 0; k < cnt; k++) {
        Io& io = db.ios[list[begin + k]];
        io.layer = (side % 2 == 0 ? hl : vl)->name;
        io.rect = rectAt(side, tr[(2 * k + 1) * tr.size() / (2 * cnt)]);
        io.x = (io.rect.x0 + io.rect.x1) / 2; io.y = (io.rect.y0 + io.rect.y1) / 2;
        io.placed = true;
      }
    }
  };
  put(ins, 0, 1);
  put(outs, 2, 3);
}

inline void floorplan(DB& db, const PlaceOptions& opt, int minLayer) {
  const phys::Site* site = db.tech.coreSite();
  if (!site) throw std::runtime_error("the LEF has no SITE");
  double area = 0;
  for (auto& in : db.insts) {
    if (!in.m->isCore()) throw std::runtime_error("instance " + in.name + ": macro " + in.m->name + " is not CLASS CORE; place it in a floorplan DEF (-floorplan)");
    if (in.m->h != site->h) throw std::runtime_error("macro " + in.m->name + " is not one row high");
    area += (double)in.m->w * in.m->h;
  }
  if (opt.util <= 0 || opt.util > 1) throw std::runtime_error("-util must be above 0 and at most 1");
  double coreArea = area / opt.util;
  int64_t h = snapUp((int64_t)std::ceil(std::sqrt(coreArea * opt.aspect)), site->h);
  if (h < site->h) h = site->h;
  int64_t w = snapUp((int64_t)std::ceil(coreArea / (double)h), site->w);
  int64_t widest = 0;
  for (auto& in : db.insts) widest = std::max(widest, in.m->w);
  w = std::max(w, snapUp(widest, site->w));
  int64_t margin = opt.marginUm >= 0 ? (int64_t)std::llround(opt.marginUm * db.tech.dbu) : 4 * site->h;
  int64_t mx = snapUp(margin, site->w), my = snapUp(margin, site->h);
  db.core = {mx, my, mx + w, my + h};
  db.die = {0, 0, w + 2 * mx, h + 2 * my};
  makeRows(db);
  placeIos(db, minLayer);
}

// ---- global placement ----------------------------------------------------

namespace gp {

struct Csr {
  std::vector<int> start, col;
  std::vector<double> val, diag;
};

struct Edge { int a, b; double w; };

// Jacobi-preconditioned conjugate gradients on (D + L) x = b, where the
// off-diagonal entries are -val.
inline void solve(par::Pool& big, const Csr& A, const std::vector<double>& b, std::vector<double>& x, int maxIter, double tol) {
  size_t n = x.size();
  // Waking the workers costs more than a small product; the chunked sums give
  // the same numbers either way.
  par::Pool one(1);
  par::Pool& pool = n < 50000 ? one : big;
  const size_t G = 4096;
  std::vector<double> r(n), z(n), p(n), q(n);
  auto mul = [&](const std::vector<double>& v, std::vector<double>& out) {
    pool.chunks(n, G, [&](size_t lo, size_t hi, size_t) {
      for (size_t i = lo; i < hi; i++) {
        double s = A.diag[i] * v[i];
        for (int k = A.start[i]; k < A.start[i + 1]; k++) s -= A.val[k] * v[A.col[k]];
        out[i] = s;
      }
    });
  };
  auto dot = [&](const std::vector<double>& u, const std::vector<double>& v) {
    return pool.sum(n, G, [&](size_t lo, size_t hi) { double s = 0; for (size_t i = lo; i < hi; i++) s += u[i] * v[i]; return s; });
  };
  mul(x, q);
  for (size_t i = 0; i < n; i++) { r[i] = b[i] - q[i]; z[i] = r[i] / A.diag[i]; p[i] = z[i]; }
  double rz = dot(r, z), bb = dot(b, b);
  if (bb == 0) bb = 1;
  for (int it = 0; it < maxIter; it++) {
    if (dot(r, r) <= tol * tol * bb) break;
    mul(p, q);
    double pq = dot(p, q);
    if (pq <= 0) break;
    double alpha = rz / pq;
    pool.chunks(n, G, [&](size_t lo, size_t hi, size_t) {
      for (size_t i = lo; i < hi; i++) { x[i] += alpha * p[i]; r[i] -= alpha * q[i]; z[i] = r[i] / A.diag[i]; }
    });
    double rz2 = dot(r, z);
    double beta = rz2 / rz;
    rz = rz2;
    pool.chunks(n, G, [&](size_t lo, size_t hi, size_t) { for (size_t i = lo; i < hi; i++) p[i] = z[i] + beta * p[i]; });
  }
}

}  // namespace gp

class Placer {
 public:
  Placer(DB& d, par::Pool& p, const PlaceOptions& o) : db(d), pool(p), opt(o) {}

  void globalPlace() {
    setup();
    if (mov.empty()) return;
    size_t n = mov.size();
    // Start in the middle of the core.
    for (size_t i = 0; i < n; i++) { cx[i] = (db.core.x0 + db.core.x1) / 2.0; cy[i] = (db.core.y0 + db.core.y1) / 2.0; }
    std::vector<double> ax(n), ay(n);       // anchors: the spread positions
    bool haveAnchor = false;
    double best = 1e300;
    std::vector<double> bestX, bestY;
    for (int it = 0; it < opt.maxIter; it++) {
      // Anchor weight grows each round; the first round has none.
      double alpha = haveAnchor ? 0.02 * std::pow(1.15, it) : 0;
      if (pool.threads() > 1 && n < 400000) {
        // x and y are independent: one thread each.
        static par::Pool px(1), py(1);
        pool.run(2, [&](size_t axis) { solveAxis(axis ? py : px, (int)axis, alpha, axis ? ay : ax); });
      } else {
        for (int axis = 0; axis < 2; axis++) solveAxis(pool, axis, alpha, axis ? ay : ax);
      }
      double lower = hpwlNow(cx, cy);
      ax = cx; ay = cy;
      spread(ax, ay);
      haveAnchor = true;
      double upper = hpwlNow(ax, ay);
      if (opt.verbose) fprintf(stderr, "  place %2d: wire length %.0f solved, %.0f spread\n", it, lower / db.tech.dbu, upper / db.tech.dbu);
      if (upper < best) { best = upper; bestX = ax; bestY = ay; }
      if (it >= 8 && (upper - lower) < 0.06 * upper) break;
    }
    for (size_t i = 0; i < n; i++) {
      Inst& in = db.insts[mov[i]];
      in.x = (int64_t)std::llround(bestX[i] - in.m->w / 2.0);
      in.y = (int64_t)std::llround(bestY[i] - in.m->h / 2.0);
    }
  }

 private:
  DB& db;
  par::Pool& pool;
  const PlaceOptions& opt;
  std::vector<int> mov;                  // movable instance ids
  std::vector<int> varOf;                // instance -> variable, or -1
  std::vector<double> cx, cy;            // cell centres
  // Per pin: variable (or -1 when fixed), offset from the cell centre or the
  // fixed position.
  struct PinRef { int var; double ox, oy; };
  std::vector<PinRef> pref;
  std::vector<int> netStart, netPin;     // nets with 2+ pins, flattened

  void setup() {
    varOf.assign(db.insts.size(), -1);
    for (size_t i = 0; i < db.insts.size(); i++)
      if (!db.insts[i].fixed && !db.insts[i].filler) { varOf[i] = (int)mov.size(); mov.push_back((int)i); }
    cx.assign(mov.size(), 0); cy.assign(mov.size(), 0);
    pref.resize(db.pins.size());
    for (size_t p = 0; p < db.pins.size(); p++) {
      const Pin& pn = db.pins[p];
      if (pn.inst >= 0 && varOf[pn.inst] >= 0) {
        const Inst& in = db.insts[pn.inst];
        const phys::MacroPin& mp = in.m->pins[pn.idx];
        double ox = 0, oy = 0;
        if (!mp.ports.empty()) { const Rect& r = mp.ports[0].r; ox = (r.x0 + r.x1) / 2.0 - in.m->w / 2.0; oy = (r.y0 + r.y1) / 2.0 - in.m->h / 2.0; }
        pref[p] = {varOf[pn.inst], ox, oy};
      } else {
        int64_t x, y;
        db.pinPos((int)p, x, y);
        pref[p] = {-1, (double)x, (double)y};
      }
    }
    netStart.push_back(0);
    for (auto& nt : db.nets) {
      if (nt.pins.size() < 2) continue;
      bool anyMov = false;
      for (int p : nt.pins) anyMov |= pref[p].var >= 0;
      if (!anyMov) continue;
      for (int p : nt.pins) netPin.push_back(p);
      netStart.push_back((int)netPin.size());
    }
  }

  double pinCoord(int p, int axis, const std::vector<double>& c) const {
    const PinRef& r = pref[p];
    double o = axis ? r.oy : r.ox;
    return r.var >= 0 ? c[r.var] + o : o;
  }

  double hpwlNow(const std::vector<double>& x, const std::vector<double>& y) const {
    size_t nn = netStart.size() - 1;
    return pool.sum(nn, 2048, [&](size_t lo, size_t hi) {
      double s = 0;
      for (size_t k = lo; k < hi; k++) {
        double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
        for (int i = netStart[k]; i < netStart[k + 1]; i++) {
          double px = pinCoord(netPin[i], 0, x), py = pinCoord(netPin[i], 1, y);
          x0 = std::min(x0, px); x1 = std::max(x1, px); y0 = std::min(y0, py); y1 = std::max(y1, py);
        }
        s += (x1 - x0) + (y1 - y0);
      }
      return s;
    });
  }

  // One axis of the quadratic system with the bound-to-bound net model: each
  // pin is tied to the two outermost pins of its net with weight
  // 2 / ((pins - 1) * distance), which makes the quadratic cost equal the
  // half-perimeter length at the current placement.
  void solveAxis(par::Pool& pool, int axis, double alpha, const std::vector<double>& anchor) {
    std::vector<double>& c = axis ? cy : cx;
    size_t n = c.size(), nn = netStart.size() - 1;
    const double eps = 0.5 * db.tech.coreSite()->w;
    // Edges per net, built in parallel into per-chunk lists, merged in order.
    const size_t G = 1024;
    size_t chunks = (nn + G - 1) / G;
    std::vector<std::vector<gp::Edge>> part(chunks);
    std::vector<std::vector<std::pair<int, double>>> dpart(chunks), bpart(chunks);
    pool.run(chunks, [&](size_t ci) {
      auto& edges = part[ci]; auto& dg = dpart[ci]; auto& rhs = bpart[ci];
      for (size_t k = ci * G; k < std::min(nn, (ci + 1) * G); k++) {
        int s = netStart[k], e = netStart[k + 1], cnt = e - s;
        int lo = s, hi = s;
        double vlo = pinCoord(netPin[s], axis, c), vhi = vlo;
        for (int i = s + 1; i < e; i++) {
          double v = pinCoord(netPin[i], axis, c);
          if (v < vlo) { vlo = v; lo = i; }
          if (v > vhi) { vhi = v; hi = i; }
        }
        if (lo == hi) hi = lo == s ? s + 1 : s;
        auto connect = [&](int i, int j) {
          const PinRef& a = pref[netPin[i]]; const PinRef& b = pref[netPin[j]];
          if (a.var < 0 && b.var < 0) return;
          double pa = pinCoord(netPin[i], axis, c), pb = pinCoord(netPin[j], axis, c);
          double w = 2.0 / ((cnt - 1) * (std::fabs(pa - pb) + eps));
          double oa = axis ? a.oy : a.ox, ob = axis ? b.oy : b.ox;
          // w * (xa + oa - xb - ob)^2
          if (a.var >= 0 && b.var >= 0) {
            if (a.var != b.var) { edges.push_back({a.var, b.var, w}); dg.push_back({a.var, w}); dg.push_back({b.var, w}); }
            rhs.push_back({a.var, w * (ob - oa)}); rhs.push_back({b.var, w * (oa - ob)});
          } else if (a.var >= 0) { dg.push_back({a.var, w}); rhs.push_back({a.var, w * (ob - oa)}); }
          else { dg.push_back({b.var, w}); rhs.push_back({b.var, w * (oa - ob)}); }
        };
        for (int i = s; i < e; i++) {
          if (i != lo) connect(i, lo);
          if (i != hi && i != lo) connect(i, hi);
        }
      }
    });
    gp::Csr A;
    A.diag.assign(n, 0);
    std::vector<double> b(n, 0);
    std::vector<int> deg(n + 1, 0);
    for (auto& edges : part) for (auto& e : edges) { deg[e.a + 1]++; deg[e.b + 1]++; }
    for (size_t i = 0; i < n; i++) deg[i + 1] += deg[i];
    A.start = deg;
    A.col.resize(deg[n]); A.val.resize(deg[n]);
    std::vector<int> fill(deg.begin(), deg.end() - 1);
    for (auto& edges : part)
      for (auto& e : edges) {
        A.col[fill[e.a]] = e.b; A.val[fill[e.a]++] = e.w;
        A.col[fill[e.b]] = e.a; A.val[fill[e.b]++] = e.w;
      }
    for (auto& dg : dpart) for (auto& d : dg) A.diag[d.first] += d.second;
    for (auto& rhs : bpart) for (auto& r : rhs) b[r.first] += r.second;
    for (size_t i = 0; i < n; i++) {
      if (alpha > 0) {
        double w = alpha / (std::fabs(c[i] - anchor[i]) + eps);
        A.diag[i] += w; b[i] += w * anchor[i];
      }
      if (A.diag[i] == 0) { A.diag[i] = 1; b[i] = c[i]; }     // a cell with no nets stays put
    }
    gp::solve(pool, A, b, c, 300, 1e-5);
    double lo = axis ? db.core.y0 : db.core.x0, hi = axis ? db.core.y1 : db.core.x1;
    for (size_t i = 0; i < n; i++) {
      double half = (axis ? db.insts[mov[i]].m->h : db.insts[mov[i]].m->w) / 2.0;
      c[i] = std::min(std::max(c[i], lo + half), hi - half);
    }
  }

  // ---- spreading: recursive bisection by area ----
  // Free area of the core on a grid, as a summed-area table, so a region's
  // capacity is four lookups.
  static const int GR = 256;
  std::vector<double> sat;
  double cellW = 0, cellH = 0;

  void buildCapacity() {
    cellW = (double)db.core.w() / GR; cellH = (double)db.core.h() / GR;
    std::vector<double> freeArea((size_t)GR * GR, cellW * cellH);
    auto block = [&](const Rect& r) {
      int x0 = std::max(0, (int)((r.x0 - db.core.x0) / cellW)), x1 = std::min(GR - 1, (int)((r.x1 - db.core.x0) / cellW));
      int y0 = std::max(0, (int)((r.y0 - db.core.y0) / cellH)), y1 = std::min(GR - 1, (int)((r.y1 - db.core.y0) / cellH));
      for (int gy = y0; gy <= y1; gy++)
        for (int gx = x0; gx <= x1; gx++) {
          double bx0 = db.core.x0 + gx * cellW, by0 = db.core.y0 + gy * cellH;
          double ox = std::min<double>(r.x1, bx0 + cellW) - std::max<double>(r.x0, bx0);
          double oy = std::min<double>(r.y1, by0 + cellH) - std::max<double>(r.y0, by0);
          if (ox > 0 && oy > 0) freeArea[(size_t)gy * GR + gx] = std::max(0.0, freeArea[(size_t)gy * GR + gx] - ox * oy);
        }
    };
    for (auto& in : db.insts) if (in.fixed) block({in.x, in.y, in.x + in.m->w, in.y + in.m->h});
    for (auto& b : db.blockages) if (b.layer.empty()) block(b.r);
    sat.assign((size_t)(GR + 1) * (GR + 1), 0);
    for (int y = 0; y < GR; y++)
      for (int x = 0; x < GR; x++)
        sat[(size_t)(y + 1) * (GR + 1) + x + 1] = freeArea[(size_t)y * GR + x] + sat[(size_t)y * (GR + 1) + x + 1] + sat[(size_t)(y + 1) * (GR + 1) + x] - sat[(size_t)y * (GR + 1) + x];
  }
  // Integral of free area over [core.x0, x] x [core.y0, y], bilinear between grid points.
  double satAt(double x, double y) const {
    double fx = std::min(std::max((x - db.core.x0) / cellW, 0.0), (double)GR), fy = std::min(std::max((y - db.core.y0) / cellH, 0.0), (double)GR);
    int ix = std::min((int)fx, GR - 1), iy = std::min((int)fy, GR - 1);
    double tx = fx - ix, ty = fy - iy;
    auto S = [&](int a, int b) { return sat[(size_t)b * (GR + 1) + a]; };
    return S(ix, iy) * (1 - tx) * (1 - ty) + S(ix + 1, iy) * tx * (1 - ty) + S(ix, iy + 1) * (1 - tx) * ty + S(ix + 1, iy + 1) * tx * ty;
  }
  double capacity(double x0, double y0, double x1, double y1) const { return satAt(x1, y1) - satAt(x0, y1) - satAt(x1, y0) + satAt(x0, y0); }

  struct Region { double x0, y0, x1, y1; };

  struct Part { size_t lo, hi; Region rg; int depth; };

  // With `later`, the two halves of a large part near the top of the tree
  // are handed back instead of being split here, so spread() can give them
  // to the pool.
  void bisect(std::vector<int>& ids, size_t lo, size_t hi, Region rg, std::vector<double>& x, std::vector<double>& y, int depth, std::vector<Part>* later = nullptr) {
    size_t cnt = hi - lo;
    if (cnt == 0) return;
    if (cnt == 1) {
      int i = ids[lo];
      const Inst& in = db.insts[mov[i]];
      // Stay where the solver put the cell if that is inside the region.
      x[i] = std::min(std::max(x[i], std::min(rg.x0 + in.m->w / 2.0, (rg.x0 + rg.x1) / 2)), std::max(rg.x1 - in.m->w / 2.0, (rg.x0 + rg.x1) / 2));
      y[i] = std::min(std::max(y[i], std::min(rg.y0 + in.m->h / 2.0, (rg.y0 + rg.y1) / 2)), std::max(rg.y1 - in.m->h / 2.0, (rg.y0 + rg.y1) / 2));
      return;
    }
    bool vertCut = (rg.x1 - rg.x0) >= (rg.y1 - rg.y0);       // cut the longer side
    std::vector<double>& key = vertCut ? x : y;
    std::sort(ids.begin() + lo, ids.begin() + hi, [&](int a, int b) { return key[a] != key[b] ? key[a] < key[b] : a < b; });
    double total = 0;
    for (size_t k = lo; k < hi; k++) total += area[ids[k]];
    double half = 0;
    size_t mid = lo;
    while (mid < hi - 1 && half + area[ids[mid]] <= total / 2) half += area[ids[mid++]];
    if (mid == lo) { half = area[ids[lo]]; mid = lo + 1; }
    double frac = half / total;
    // The cut line that gives the left part its share of the free area.
    double a = vertCut ? rg.x0 : rg.y0, b = vertCut ? rg.x1 : rg.y1;
    double cap = capacity(rg.x0, rg.y0, rg.x1, rg.y1);
    double cut = a + (b - a) * frac;
    if (cap > 0) {
      double l = a, h = b;
      for (int k = 0; k < 30; k++) {
        double m = (l + h) / 2;
        double c = vertCut ? capacity(rg.x0, rg.y0, m, rg.y1) : capacity(rg.x0, rg.y0, rg.x1, m);
        if (c < frac * cap) l = m; else h = m;
      }
      cut = (l + h) / 2;
    }
    Region r1 = rg, r2 = rg;
    if (vertCut) { r1.x1 = cut; r2.x0 = cut; } else { r1.y1 = cut; r2.y0 = cut; }
    if (later && depth < forkDepth && cnt > 2000) {
      later->push_back({lo, mid, r1, depth + 1});
      later->push_back({mid, hi, r2, depth + 1});
    } else {
      bisect(ids, lo, mid, r1, x, y, depth + 1);
      bisect(ids, mid, hi, r2, x, y, depth + 1);
    }
  }

  std::vector<double> area;
  int forkDepth = 0;

  void spread(std::vector<double>& x, std::vector<double>& y) {
    if (sat.empty()) {
      buildCapacity();
      area.resize(mov.size());
      for (size_t i = 0; i < mov.size(); i++) area[i] = (double)db.insts[mov[i]].m->w * db.insts[mov[i]].m->h;
      forkDepth = 0;
      while ((1 << forkDepth) < (int)pool.threads()) forkDepth++;
    }
    std::vector<int> ids(mov.size());
    std::iota(ids.begin(), ids.end(), 0);
    // Level by level near the top, each level's parts side by side on the pool.
    std::vector<Part> parts{{0, ids.size(), {(double)db.core.x0, (double)db.core.y0, (double)db.core.x1, (double)db.core.y1}, 0}};
    while (!parts.empty()) {
      std::vector<std::vector<Part>> made(parts.size());
      pool.run(parts.size(), [&](size_t i) { bisect(ids, parts[i].lo, parts[i].hi, parts[i].rg, x, y, parts[i].depth, &made[i]); });
      parts.clear();
      for (auto& m : made) parts.insert(parts.end(), m.begin(), m.end());
    }
  }
};

// ---- legalization (Abacus) -------------------------------------------------

class Legalizer {
 public:
  explicit Legalizer(DB& d) : db(d) {}

  // Returns the mean displacement in dbu.
  double run() {
    const phys::Site* site = db.tech.coreSite();
    siteW = site->w;
    buildSubRows();
    std::vector<int> order;
    for (size_t i = 0; i < db.insts.size(); i++) if (!db.insts[i].fixed && !db.insts[i].filler) order.push_back((int)i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return db.insts[a].x != db.insts[b].x ? db.insts[a].x < db.insts[b].x : a < b; });
    double moved = 0;
    for (int id : order) {
      Inst& in = db.insts[id];
      int r0 = (int)std::min<int64_t>(std::max<int64_t>((in.y - db.core.y0 + rowH / 2) / rowH, 0), (int64_t)rowSubs.size() - 1);
      double best = 1e300;
      int bestSub = -1;
      for (int d = 0; d < (int)rowSubs.size(); d++) {
        // Rows d away are at least (d - 1) row heights off: stop when that alone costs more.
        if (d > 0 && (double)(d - 1) * rowH >= best) break;
        for (int sgn = -1; sgn <= 1; sgn += 2) {
          if (d == 0 && sgn > 0) continue;
          int r = r0 + sgn * d;
          if (r < 0 || r >= (int)rowSubs.size()) continue;
          double dy = std::fabs((double)(rowY[r] - in.y));
          if (dy >= best) continue;
          for (int s : rowSubs[r]) {
            double c = trial(subs[s], in);
            if (c < 0) continue;
            if (c + dy < best) { best = c + dy; bestSub = s; }
          }
        }
      }
      if (bestSub < 0) throw std::runtime_error("no room to place " + in.name + "; lower -util");
      commit(subs[bestSub], id);
    }
    for (auto& s : subs) {
      for (auto& c : s.clusters) {
        int64_t x = c.x;
        for (int id : c.cells) {
          Inst& in = db.insts[id];
          moved += std::fabs((double)(x - in.x)) + std::fabs((double)(s.y - in.y));
          in.x = x; in.y = s.y; in.orient = s.orient; in.placed = true;
          x += in.m->w;
        }
      }
    }
    return order.empty() ? 0 : moved / order.size();
  }

 private:
  struct Cluster { double e = 0, q = 0; int64_t w = 0, x = 0; std::vector<int> cells; };
  struct SubRow { int64_t x0, x1, y; std::string orient; int64_t used = 0; std::vector<Cluster> clusters; };
  DB& db;
  int64_t siteW = 1, rowH = 1;
  std::vector<SubRow> subs;
  std::vector<std::vector<int>> rowSubs;
  std::vector<int64_t> rowY;

  void buildSubRows() {
    rowH = db.tech.coreSite()->h;
    std::vector<Rect> obst;
    for (auto& in : db.insts) if (in.fixed) obst.push_back({in.x, in.y, in.x + in.m->w, in.y + in.m->h});
    for (auto& b : db.blockages) if (b.layer.empty()) obst.push_back(b.r);
    rowSubs.resize(db.rows.size());
    for (size_t r = 0; r < db.rows.size(); r++) {
      const phys::Row& row = db.rows[r];
      rowY.push_back(row.y);
      int64_t x0 = row.x, x1 = row.x + row.nx * row.stepX;
      std::vector<std::pair<int64_t, int64_t>> cuts;
      for (auto& o : obst) if (o.y0 < row.y + rowH && o.y1 > row.y && o.x0 < x1 && o.x1 > x0) cuts.push_back({o.x0, o.x1});
      std::sort(cuts.begin(), cuts.end());
      int64_t at = x0;
      auto add = [&](int64_t a, int64_t b) {
        a = row.x + snapUp(a - row.x, siteW); b = row.x + snapDown(b - row.x, siteW);
        if (b - a >= siteW) { rowSubs[r].push_back((int)subs.size()); subs.push_back({a, b, row.y, row.orient, 0, {}}); }
      };
      for (auto& c : cuts) { if (c.first > at) add(at, c.first); at = std::max(at, c.second); }
      if (at < x1) add(at, x1);
    }
  }

  int64_t snapSite(const SubRow& s, double x, int64_t w) const {
    int64_t v = s.x0 + (int64_t)std::llround((x - s.x0) / (double)siteW) * siteW;
    return std::min(std::max(v, s.x0), s.x1 - w);
  }

  // Where the cell would land if appended to the sub-row: |dx|, or -1 when
  // the sub-row is full. Clusters are merged on paper only.
  double trial(const SubRow& s, const Inst& in) const {
    int64_t w = in.m->w;
    if (s.used + w > s.x1 - s.x0) return -1;
    double e = 1, q = (double)in.x;
    int64_t cw = w;
    int k = (int)s.clusters.size() - 1;
    int64_t x = snapSite(s, q / e, cw);
    while (k >= 0 && s.clusters[k].x + s.clusters[k].w > x) {
      const Cluster& p = s.clusters[k];
      // merged cluster: q' = qp + q - e * wp
      q = p.q + q - e * p.w;
      e += p.e;
      cw += p.w;
      x = snapSite(s, q / e, cw);
      k--;
    }
    return std::fabs((double)(x + cw - w - in.x));
  }

  void commit(SubRow& s, int id) {
    const Inst& in = db.insts[id];
    Cluster c;
    c.e = 1; c.q = (double)in.x; c.w = in.m->w; c.cells.push_back(id);
    c.x = snapSite(s, c.q / c.e, c.w);
    while (!s.clusters.empty() && s.clusters.back().x + s.clusters.back().w > c.x) {
      Cluster p = std::move(s.clusters.back());
      s.clusters.pop_back();
      p.q = p.q + c.q - c.e * p.w;
      p.e += c.e;
      p.w += c.w;
      p.cells.insert(p.cells.end(), c.cells.begin(), c.cells.end());
      p.x = snapSite(s, p.q / p.e, p.w);
      c = std::move(p);
    }
    s.clusters.push_back(std::move(c));
    s.used += in.m->w;
  }
};

// ---- detailed placement ----------------------------------------------------

// Swaps neighbouring cells in a row and mirrors cells left to right while the
// half-perimeter wire length drops. Returns the number of changes kept.
inline int refine(DB& db, int passes = 3) {
  // Rows of movable cells in x order.
  std::map<int64_t, std::vector<int>> rows;
  for (size_t i = 0; i < db.insts.size(); i++) if (!db.insts[i].filler) rows[db.insts[i].y].push_back((int)i);
  for (auto& [y, v] : rows) std::sort(v.begin(), v.end(), [&](int a, int b) { return db.insts[a].x < db.insts[b].x; });
  auto netLen = [&](int n) {
    const Net& nt = db.nets[n];
    if (nt.pins.size() < 2) return (int64_t)0;
    int64_t x0 = INT64_MAX, x1 = INT64_MIN, y0 = INT64_MAX, y1 = INT64_MIN;
    for (int p : nt.pins) {
      int64_t x, y;
      db.pinPos(p, x, y);
      x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
    return (x1 - x0) + (y1 - y0);
  };
  std::vector<int> touched;
  auto cost = [&](int a, int b) {
    touched.clear();
    for (int id : {a, b}) if (id >= 0) for (int p : db.insts[id].pins) if (db.nets[db.pins[p].net].pins.size() <= 64) touched.push_back(db.pins[p].net);
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    int64_t s = 0;
    for (int n : touched) s += netLen(n);
    return s;
  };
  auto mirror = [](const std::string& o) { return o == "N" ? std::string("FN") : o == "FN" ? std::string("N") : o == "FS" ? std::string("S") : std::string("FS"); };
  int kept = 0;
  for (int pass = 0; pass < passes; pass++) {
    int changed = 0;
    for (auto& [y, v] : rows) {
      for (size_t k = 0; k < v.size(); k++) {
        Inst& a = db.insts[v[k]];
        if (a.fixed) continue;
        // Mirror about the y axis (pins stay on tracks: the width is a whole number of sites).
        if (a.m->symY) {
          int64_t before = cost(v[k], -1);
          std::string keep = a.orient;
          a.orient = mirror(a.orient);
          if (cost(v[k], -1) < before) changed++; else a.orient = keep;
        }
        if (k + 1 >= v.size()) continue;
        Inst& b = db.insts[v[k + 1]];
        if (b.fixed) continue;
        // Across a gap only equal widths swap: the gap may be a blockage or a fixed cell.
        if (a.x + a.m->w != b.x && a.m->w != b.m->w) continue;
        int64_t before = cost(v[k], v[k + 1]);
        int64_t ax = a.x, bx = b.x;
        // a | gap | b  ->  b | gap | a, keeping the outer edges.
        b.x = ax; a.x = bx + b.m->w - a.m->w;
        if (cost(v[k], v[k + 1]) < before) { std::swap(v[k], v[k + 1]); changed++; }
        else { a.x = ax; b.x = bx; }
      }
    }
    kept += changed;
    if (!changed) break;
  }
  return kept;
}

// ---- fillers ---------------------------------------------------------------

inline int addFillers(DB& db) {
  std::vector<const phys::Macro*> fill;
  const phys::Site* site = db.tech.coreSite();
  for (auto& [n, m] : db.tech.macros)
    if (m.isFiller() && m.h == site->h && m.w % site->w == 0 && m.site == site->name) {
      bool signal = false;
      for (auto& p : m.pins) signal |= p.use == "SIGNAL";
      if (!signal) fill.push_back(&m);
    }
  if (fill.empty()) return 0;
  std::sort(fill.begin(), fill.end(), [](auto a, auto b) { return a->w != b->w ? a->w > b->w : a->name < b->name; });
  int count = 0;
  for (auto& row : db.rows) {
    std::vector<std::pair<int64_t, int64_t>> occ;
    int64_t x0 = row.x, x1 = row.x + row.nx * row.stepX;
    for (auto& in : db.insts) if (in.y < row.y + site->h && in.y + in.m->h > row.y) occ.push_back({in.x, in.x + in.m->w});
    for (auto& b : db.blockages) if (b.layer.empty() && b.r.y0 < row.y + site->h && b.r.y1 > row.y) occ.push_back({snapDown(b.r.x0 - row.x, site->w) + row.x, snapUp(b.r.x1 - row.x, site->w) + row.x});
    std::sort(occ.begin(), occ.end());
    occ.push_back({x1, x1});
    int64_t at = x0;
    for (auto& o : occ) {
      int64_t gap = o.first - at;
      while (gap > 0) {
        const phys::Macro* f = nullptr;
        for (auto m : fill) if (m->w <= gap) { f = m; break; }
        if (!f) break;
        Inst in;
        in.name = "FILLER_" + std::to_string(count++);
        in.m = f; in.x = at; in.y = row.y; in.orient = row.orient; in.placed = true; in.filler = true;
        db.insts.push_back(in);
        at += f->w; gap -= f->w;
      }
      at = std::max(at, o.second);
    }
  }
  return count;
}

}  // namespace pnr
