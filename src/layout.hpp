// layout.hpp - a GDS library flattened to rectangles per layer, for the
// physical verification tools.
//
// Every polygon and path of the top cell and of everything it references is
// turned into rectangles in top-cell coordinates. Each cell is decomposed
// once (on all threads) and then placed through its references.
//
// The engine works on axis-parallel geometry. A polygon or path with an edge
// at any other angle, and a reference rotated by something other than a
// multiple of 90 degrees or magnified, is not checked; Flat::skipped counts
// them so that the tools can say so.
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "gds.hpp"
#include "geom.hpp"

namespace layout {

using geom::C;
using geom::Rect;

struct LayerKey {
    int layer = 0, datatype = 0;
    bool operator<(const LayerKey& o) const {
        return layer != o.layer ? layer < o.layer : datatype < o.datatype;
    }
};

struct Label {
    int layer = 0, texttype = 0;
    C x = 0, y = 0;
    std::string text;
};

struct Flat {
    std::string top;
    double dbu = 0.001;
    std::map<LayerKey, std::vector<Rect>> layers;
    std::vector<Label> labels;
    size_t shapes = 0;     // polygons and paths placed
    size_t skipped = 0;    // shapes and references with edges that are not axis-parallel
    Rect extent;           // bounding box of all layers
};

// p' = (a*x + b*y + tx, c*x + d*y + ty), with a, b, c, d in {-1, 0, 1}.
struct Xform {
    int a = 1, b = 0, c = 0, d = 1;
    int64_t tx = 0, ty = 0;
    gds::Point apply(gds::Point p) const {
        return {int32_t(a * int64_t(p.x) + b * int64_t(p.y) + tx), int32_t(c * int64_t(p.x) + d * int64_t(p.y) + ty)};
    }
    Rect apply(const Rect& r) const {
        gds::Point p = apply(gds::Point{r.x1, r.y1}), q = apply(gds::Point{r.x2, r.y2});
        return {std::min(p.x, q.x), std::min(p.y, q.y), std::max(p.x, q.x), std::max(p.y, q.y)};
    }
    // this after inner
    Xform after(const Xform& in) const {
        Xform o;
        o.a = a * in.a + b * in.c; o.b = a * in.b + b * in.d;
        o.c = c * in.a + d * in.c; o.d = c * in.b + d * in.d;
        o.tx = a * in.tx + b * in.ty + tx;
        o.ty = c * in.tx + d * in.ty + ty;
        return o;
    }
};

inline bool refXform(const gds::Ref& r, Xform& x) {
    if (r.mag != 1.0) return false;
    double q = r.angle / 90.0;
    int k = int(q < 0 ? q - 0.5 : q + 0.5);
    if (q != double(k)) return false;
    k = ((k % 4) + 4) % 4;
    static const int cs[4] = {1, 0, -1, 0}, sn[4] = {0, 1, 0, -1};
    int m = r.mirror ? -1 : 1;
    x.a = cs[k]; x.b = -sn[k] * m;
    x.c = sn[k]; x.d = cs[k] * m;
    x.tx = r.origin.x; x.ty = r.origin.y;
    return true;
}

// Rectangles covering an axis-parallel polygon. False when an edge is at
// another angle.
inline bool polygonRects(const std::vector<gds::Point>& p, std::vector<Rect>& out) {
    size_t n = p.size();
    if (n < 3) return true;
    for (size_t i = 0; i < n; ++i) {
        const gds::Point &u = p[i], &v = p[(i + 1) % n];
        if (u.x != v.x && u.y != v.y) return false;
    }
    if (n == 4) {
        C x1 = std::min(p[0].x, p[2].x), x2 = std::max(p[0].x, p[2].x);
        C y1 = std::min(p[0].y, p[2].y), y2 = std::max(p[0].y, p[2].y);
        if (x1 < x2 && y1 < y2) out.push_back({x1, y1, x2, y2});
        return true;
    }
    std::vector<geom::detail::Ev> ev;
    for (size_t i = 0; i < n; ++i) {
        const gds::Point &u = p[i], &v = p[(i + 1) % n];
        if (u.x != v.x || u.y == v.y) continue;
        ev.push_back({u.x, std::min(u.y, v.y), std::max(u.y, v.y), int8_t(v.y < u.y ? 1 : -1), 0});
    }
    geom::detail::scan(ev, geom::Op::Or, out);
    return true;
}

inline bool pathRects(const gds::Path& p, std::vector<Rect>& out) {
    size_t n = p.pts.size();
    if (n < 2 || p.width <= 0) return true;
    for (size_t i = 0; i + 1 < n; ++i)
        if (p.pts[i].x != p.pts[i + 1].x && p.pts[i].y != p.pts[i + 1].y) return false;
    C hw = p.width / 2;
    // Round ends (type 1) are taken as square ends of the same reach.
    C e0 = p.pathtype == 2 || p.pathtype == 1 ? hw : p.pathtype == 4 ? p.bgnextn : 0;
    C e1 = p.pathtype == 2 || p.pathtype == 1 ? hw : p.pathtype == 4 ? p.endextn : 0;
    for (size_t i = 0; i + 1 < n; ++i) {
        gds::Point u = p.pts[i], v = p.pts[i + 1];
        // A bend is covered by extending both segments by half the width.
        C eu = i == 0 ? e0 : hw, ev = i + 2 == n ? e1 : hw;
        if (u.y == v.y) {
            if (u.x <= v.x) out.push_back({u.x - eu, u.y - hw, v.x + ev, u.y + hw});
            else out.push_back({v.x - ev, u.y - hw, u.x + eu, u.y + hw});
        } else {
            if (u.y <= v.y) out.push_back({u.x - hw, u.y - eu, u.x + hw, v.y + ev});
            else out.push_back({u.x - hw, v.y - ev, u.x + hw, u.y + eu});
        }
    }
    return true;
}

inline bool flatten(const gds::Library& lib, const std::string& topName, Flat& out, std::string& err,
                    unsigned threads = 1) {
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < lib.cells.size(); ++i) index[lib.cells[i].name] = i;
    std::string top = topName;
    if (top.empty()) {
        std::vector<std::string> tops = lib.topCells();
        if (tops.empty()) { err = "the layout has no top cell"; return false; }
        if (tops.size() > 1) {
            err = "the layout has " + std::to_string(tops.size()) + " top cells (" + tops[0] + ", " + tops[1] +
                  (tops.size() > 2 ? ", ..." : "") + "); pick one with -top";
            return false;
        }
        top = tops[0];
    }
    if (!index.count(top)) { err = "no cell named " + top; return false; }
    out = Flat();
    out.top = top;
    out.dbu = lib.dbu;

    // Each cell's own shapes as rectangles, once.
    struct Local {
        std::map<LayerKey, std::vector<Rect>> layers;
        size_t shapes = 0, skipped = 0;
    };
    std::vector<Local> local(lib.cells.size());
    geom::parallelFor(lib.cells.size(), threads, [&](size_t i) {
        const gds::Cell& c = lib.cells[i];
        Local& l = local[i];
        for (const gds::Polygon& p : c.polygons) {
            if (polygonRects(p.pts, l.layers[{p.layer, p.datatype}])) ++l.shapes; else ++l.skipped;
        }
        for (const gds::Path& p : c.paths) {
            if (pathRects(p, l.layers[{p.layer, p.datatype}])) ++l.shapes; else ++l.skipped;
        }
    });

    std::vector<char> onPath(lib.cells.size(), 0);
    bool ok = true;
    auto place = [&](auto&& self, size_t ci, const Xform& x) -> void {
        if (!ok) return;
        if (onPath[ci]) { err = "cell " + lib.cells[ci].name + " references itself"; ok = false; return; }
        onPath[ci] = 1;
        const Local& l = local[ci];
        out.shapes += l.shapes;
        out.skipped += l.skipped;
        for (const auto& [key, rs] : l.layers) {
            std::vector<Rect>& dst = out.layers[key];
            for (const Rect& r : rs) dst.push_back(x.apply(r));
        }
        for (const gds::Text& t : lib.cells[ci].texts) {
            gds::Point p = x.apply(t.at);
            out.labels.push_back({t.layer, t.texttype, p.x, p.y, t.str});
        }
        for (const gds::Ref& r : lib.cells[ci].refs) {
            auto it = index.find(r.cell);
            if (it == index.end()) { err = "cell " + r.cell + " is referenced but not defined"; ok = false; break; }
            Xform rx;
            if (!refXform(r, rx)) { ++out.skipped; continue; }
            // The array steps are in the parent's coordinates and are not transformed.
            for (int row = 0; row < std::max(1, r.rows); ++row)
                for (int col = 0; col < std::max(1, r.cols); ++col) {
                    Xform k = rx;
                    k.tx += int64_t(col) * r.colStep.x + int64_t(row) * r.rowStep.x;
                    k.ty += int64_t(col) * r.colStep.y + int64_t(row) * r.rowStep.y;
                    self(self, it->second, x.after(k));
                }
        }
        onPath[ci] = 0;
    };
    place(place, index[top], Xform());
    if (!ok) return false;
    bool first = true;
    for (const auto& [key, rs] : out.layers)
        for (const Rect& r : rs) {
            if (first) { out.extent = r; first = false; continue; }
            out.extent.x1 = std::min(out.extent.x1, r.x1); out.extent.y1 = std::min(out.extent.y1, r.y1);
            out.extent.x2 = std::max(out.extent.x2, r.x2); out.extent.y2 = std::max(out.extent.y2, r.y2);
        }
    return true;
}

}  // namespace layout
