// test_pv.cpp - the polygon engine (geom.hpp), the GDSII reader and writer
// (gds.hpp), flattening (layout.hpp) and the design rule checks (drc.hpp).
//
//   test_pv              runs the checks
//   test_pv -gen <dir>   only writes <dir>/pv_bad.gds, a layout with known
//                        violations, for the mpv command line test
#include <cassert>
#include <cstdio>
#include <random>
#include <sstream>

#include "drc.hpp"

using geom::C;
using geom::Op;
using geom::Rect;
using geom::Region;

namespace {

const int N = 48;   // raster size for the brute-force reference

struct Grid {
    char c[N][N] = {};
    void add(const Rect& r) {
        for (int x = r.x1; x < r.x2; ++x)
            for (int y = r.y1; y < r.y2; ++y)
                if (x >= 0 && y >= 0 && x < N && y < N) c[x][y] = 1;
    }
    bool operator==(const Grid& o) const { return std::memcmp(c, o.c, sizeof c) == 0; }
};

Grid raster(const Region& r) {
    Grid g;
    for (const Rect& q : r.rects) {
        for (int x = q.x1; x < q.x2; ++x)
            for (int y = q.y1; y < q.y2; ++y) {
                assert(x >= 0 && y >= 0 && x < N && y < N);
                assert(!g.c[x][y]);   // canonical rectangles never overlap
                g.c[x][y] = 1;
            }
    }
    return g;
}

std::vector<Rect> randomRects(std::mt19937& rng, int n, int span, int maxSize) {
    std::vector<Rect> v;
    for (int i = 0; i < n; ++i) {
        int x = int(rng() % span), y = int(rng() % span);
        int w = 1 + int(rng() % maxSize), h = 1 + int(rng() % maxSize);
        v.push_back({x, y, std::min(span, x + w), std::min(span, y + h)});
    }
    return v;
}

// Canonical form: no two rectangles could be joined in x, and at every x the
// y intervals are maximal.
void checkCanonical(const Region& r) {
    for (size_t i = 0; i < r.rects.size(); ++i) {
        const Rect& p = r.rects[i];
        assert(p.x1 < p.x2 && p.y1 < p.y2);
        if (i) assert(r.rects[i - 1] < p);
        for (size_t j = 0; j < r.rects.size(); ++j) {
            const Rect& q = r.rects[j];
            if (i == j) continue;
            assert(!(p.x2 == q.x1 && p.y1 == q.y1 && p.y2 == q.y2));
            bool xOverlap = std::max(p.x1, q.x1) < std::min(p.x2, q.x2);
            assert(!(xOverlap && (p.y2 == q.y1 || q.y2 == p.y1)));
        }
    }
}

void testBooleans() {
    std::mt19937 rng(1);
    for (int iter = 0; iter < 300; ++iter) {
        std::vector<Rect> ra = randomRects(rng, 1 + int(rng() % 12), N, 14), rb = randomRects(rng, 1 + int(rng() % 12), N, 14);
        Region a = geom::merge(ra), b = geom::merge(rb);
        Grid ga, gb;
        for (const Rect& r : ra) ga.add(r);
        for (const Rect& r : rb) gb.add(r);
        assert(raster(a) == ga && raster(b) == gb);
        checkCanonical(a);
        for (Op op : {Op::Or, Op::And, Op::Sub, Op::Xor}) {
            Region c = geom::boolean(a, b, op);
            checkCanonical(c);
            Grid want;
            for (int x = 0; x < N; ++x)
                for (int y = 0; y < N; ++y) {
                    bool p = ga.c[x][y], q = gb.c[x][y];
                    want.c[x][y] = op == Op::Or ? p || q : op == Op::And ? p && q : op == Op::Sub ? p && !q : p != q;
                }
            assert(raster(c) == want);
        }
        // Same points, same rectangles: the order of the input does not matter.
        std::vector<Rect> shuffled = ra;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        assert(geom::merge(shuffled) == a);
        // Area and transposition.
        int64_t cells = 0;
        for (int x = 0; x < N; ++x)
            for (int y = 0; y < N; ++y) cells += ga.c[x][y];
        assert(a.area() == cells);
        assert(geom::transposed(geom::transposed(a)) == a);
    }
}

void testSizing() {
    std::mt19937 rng(2);
    for (int iter = 0; iter < 100; ++iter) {
        std::vector<Rect> ra = randomRects(rng, 1 + int(rng() % 8), N - 16, 12);
        for (Rect& r : ra) { r.x1 += 8; r.x2 += 8; r.y1 += 8; r.y2 += 8; }
        Region a = geom::merge(ra);
        Grid ga = raster(a);
        for (int d = 1; d <= 3; ++d) {
            Grid grow, shrink;
            for (int x = 0; x < N; ++x)
                for (int y = 0; y < N; ++y) {
                    bool any = false, all = true;
                    for (int i = -d; i <= d; ++i)
                        for (int j = -d; j <= d; ++j) {
                            // The cell (x, y) is the unit square [x, x+1] x [y, y+1]; a
                            // square of half-side d around any of its points reaches
                            // the cells within d.
                            int u = x + i, v = y + j;
                            bool in = u >= 0 && v >= 0 && u < N && v < N && ga.c[u][v];
                            any |= in;
                            all &= in;
                        }
                    grow.c[x][y] = any;
                    shrink.c[x][y] = all;
                }
            assert(raster(geom::sized(a, d)) == grow);
            assert(raster(geom::sized(a, -d)) == shrink);
        }
    }
}

void testThreads() {
    // Large enough to be cut into strips: every thread count gives the same rectangles.
    std::mt19937 rng(3);
    std::vector<Rect> ra = randomRects(rng, 60000, 30000, 300), rb = randomRects(rng, 60000, 30000, 300);
    Region a1 = geom::merge(ra, 1), b1 = geom::merge(rb, 1);
    for (unsigned t : {2u, 8u}) {
        assert(geom::merge(ra, t) == a1);
        for (Op op : {Op::Or, Op::And, Op::Sub, Op::Xor}) assert(geom::boolean(a1, b1, op, t) == geom::boolean(a1, b1, op, 1));
    }
    // A | B from the merged halves equals merging everything at once.
    std::vector<Rect> all = ra;
    all.insert(all.end(), rb.begin(), rb.end());
    assert(geom::boolean(a1, b1, Op::Or, 4) == geom::merge(all, 4));
}

void testTopology() {
    // A frame (one polygon with a hole), and a separate square that touches it only at a corner.
    Region frame = geom::boolean(geom::merge({{0, 0, 30, 30}}), geom::merge({{10, 10, 20, 20}}), Op::Sub);
    Region both = geom::boolean(frame, geom::merge({{30, 30, 40, 40}}), Op::Or);
    std::vector<uint32_t> id;
    assert(geom::components(frame, id) == 1);
    assert(geom::components(both, id) == 2);
    std::vector<geom::Edge> v = geom::boundary(frame, true), h = geom::boundary(frame, false);
    assert(v.size() == 4 && h.size() == 4);
    int64_t len = 0;
    for (const geom::Edge& e : v) len += e.hi - e.lo;
    for (const geom::Edge& e : h) len += e.hi - e.lo;
    assert(len == 4 * 30 + 4 * 10);
    // Left side of the frame: the region is to the right of it.
    assert(v[0].at == 0 && v[0].lo == 0 && v[0].hi == 30 && v[0].inside == 1);
    // Overlap and touch pairs.
    Region a = geom::merge({{0, 0, 10, 10}, {20, 0, 30, 10}}), b = geom::merge({{10, 0, 15, 10}, {25, 5, 40, 20}});
    int over = 0, touch = 0;
    geom::overlaps(a, b, false, [&](uint32_t, uint32_t) { ++over; });
    geom::overlaps(a, b, true, [&](uint32_t, uint32_t) { ++touch; });
    assert(over == 1 && touch == 2);
}

void testGds() {
    gds::Library lib;
    lib.name = "T";
    lib.cells.resize(2);
    gds::Cell& leaf = lib.cells[0];
    leaf.name = "leaf";
    leaf.polygons.push_back({5, 2, {{0, 0}, {100, 0}, {100, 50}, {40, 50}, {40, 120}, {0, 120}}});
    gds::Path p;
    p.layer = 6; p.width = 20; p.pathtype = 2;
    p.pts = {{0, 0}, {200, 0}, {200, 300}};
    leaf.paths.push_back(p);
    leaf.texts.push_back({5, 9, {10, 20}, "VDD"});
    gds::Cell& top = lib.cells[1];
    top.name = "top";
    gds::Ref r;
    r.cell = "leaf"; r.origin = {1000, -2000}; r.mirror = true; r.angle = 90;
    top.refs.push_back(r);
    gds::Ref arr;
    arr.cell = "leaf"; arr.origin = {0, 0}; arr.cols = 3; arr.rows = 2; arr.colStep = {500, 0}; arr.rowStep = {0, 700};
    top.refs.push_back(arr);

    std::string bytes = gds::serialize(lib), err;
    gds::Library back;
    assert(gds::parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), back, err));
    assert(back.name == "T" && back.cells.size() == 2);
    assert(std::fabs(back.dbu - 0.001) < 1e-12 && std::fabs(back.userUnit - 0.001) < 1e-12);
    assert(gds::serialize(back) == bytes);
    const gds::Cell& l2 = back.cells[0];
    assert(l2.polygons[0].pts.size() == 6 && l2.polygons[0].layer == 5 && l2.polygons[0].datatype == 2);
    assert(l2.paths[0].width == 20 && l2.paths[0].pathtype == 2 && l2.paths[0].pts.size() == 3);
    assert(l2.texts[0].str == "VDD" && l2.texts[0].texttype == 9);
    const gds::Ref& a2 = back.cells[1].refs[1];
    assert(a2.cols == 3 && a2.rows == 2 && a2.colStep.x == 500 && a2.rowStep.y == 700);
    assert(back.cells[1].refs[0].mirror && back.cells[1].refs[0].angle == 90.0);
    assert(back.topCells() == std::vector<std::string>{"top"});
    // A truncated stream is an error, not a crash.
    gds::Library bad;
    assert(!gds::parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size() - 7, bad, err));

    // Flatten: 7 placements of the L polygon (area 100*50 + 40*70) and of the path.
    layout::Flat flat;
    assert(layout::flatten(back, "", flat, err, 2));
    assert(flat.top == "top" && flat.shapes == 14 && flat.skipped == 0 && flat.labels.size() == 7);
    Region l5 = geom::merge(flat.layers[{5, 2}]);
    assert(l5.area() == 7 * (100 * 50 + 40 * 70));
    // The path: 200 + 300 long, 20 wide, half-width extensions, the bend counted once.
    Region l6 = geom::merge(flat.layers[{6, 0}]);
    std::vector<uint32_t> id;
    assert(geom::components(l6, id) == 7);
    assert(l6.area() == 7 * ((200 + 20) * 20 + 300 * 20));
    // The mirrored and rotated copy: x' = y... mirror then rotate 90: (x, y) -> (y, x) + origin.
    bool found = false;
    for (const layout::Label& t : flat.labels) found |= t.x == 1000 + 20 && t.y == -2000 + 10;
    assert(found);
}

// --- DRC -------------------------------------------------------------------

struct Shapes {
    layout::Flat flat;
    void add(int layer, Rect r) {
        flat.layers[{layer, 0}].push_back(r);
        if (flat.shapes++ == 0) flat.extent = r;
        flat.extent.x1 = std::min(flat.extent.x1, r.x1); flat.extent.y1 = std::min(flat.extent.y1, r.y1);
        flat.extent.x2 = std::max(flat.extent.x2, r.x2); flat.extent.y2 = std::max(flat.extent.y2, r.y2);
    }
};

std::vector<drc::RuleResult> runDeck(const std::string& text, const layout::Flat& flat, unsigned threads = 2) {
    drc::Deck deck;
    std::istringstream in(text);
    std::string err;
    if (!drc::parseDeck(in, deck, err)) { std::fprintf(stderr, "deck: %s\n", err.c_str()); assert(false); }
    drc::Engine eng(deck, flat, threads);
    return eng.run();
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

void testChecks() {
    const char* layers = "layer a 1\nlayer b 2\n";
    {   // width: a 100-wide bar against 0.15, a 150-wide bar is fine
        Shapes s;
        s.add(1, {0, 0, 100, 1000});
        s.add(1, {500, 0, 650, 1000});
        auto r = runDeck(std::string(layers) + "rule w width(a) < 0.15\nrule w2 width(a) < 0.1\n", s.flat);
        assert(r[0].markers.size() == 1 && near(r[0].markers[0].value, 0.1));
        assert((r[0].markers[0].box == Rect{0, 0, 100, 1000}));
        assert(r[1].markers.empty());
    }
    {   // width across a corner: two squares overlapping in a 20 x 20 neck
        Shapes s;
        s.add(1, {0, 0, 200, 200});
        s.add(1, {180, 180, 380, 380});
        auto r = runDeck(std::string(layers) + "rule w width(a) < 0.1\n", s.flat);
        assert(r[0].markers.size() == 1 && near(r[0].markers[0].value, std::sqrt(800.0) * 0.001));
    }
    {   // spacing: side by side, a notch, and corner to corner
        Shapes s;
        s.add(1, {0, 0, 200, 200});
        s.add(1, {300, 50, 500, 400});                       // 100 from the first: violation over y 50..200
        s.add(1, {1000, 0, 1600, 200});                      // U shape with a 120 notch
        s.add(1, {1000, 200, 1200, 600});
        s.add(1, {1320, 200, 1600, 600});
        s.add(1, {3000, 0, 3200, 200});                      // diagonal neighbours, 60 and 80 apart: 100
        s.add(1, {3260, 280, 3500, 500});
        auto r = runDeck(std::string(layers) + "rule s space(a) < 0.14\nrule s2 space(a) < 0.1\n", s.flat);
        assert(r[0].markers.size() == 3);
        assert((r[0].markers[0].box == Rect{200, 50, 300, 200}) && near(r[0].markers[0].value, 0.1));
        assert((r[0].markers[1].box == Rect{1200, 200, 1320, 600}) && near(r[0].markers[1].value, 0.12));
        assert((r[0].markers[2].box == Rect{3200, 200, 3260, 280}) && near(r[0].markers[2].value, 0.1));
        assert(r[1].markers.empty());
    }
    {   // spacing between two layers; shapes of the same layer are not compared
        Shapes s;
        s.add(1, {0, 0, 200, 200});
        s.add(1, {250, 0, 450, 200});
        s.add(2, {0, 260, 200, 400});
        s.add(2, {520, 230, 700, 400});   // corner to corner with the second a: 70, 30 -> 76.2
        auto r = runDeck(std::string(layers) + "rule s space(a, b) < 0.08\n", s.flat);
        // The second a is also 50, 60 from the corner of the first b: 78.1.
        assert(r[0].markers.size() == 3);
        assert((r[0].markers[0].box == Rect{0, 200, 200, 260}) && near(r[0].markers[0].value, 0.06));
        assert(near(r[0].markers[1].value, std::sqrt(50.0 * 50 + 60 * 60) * 0.001));
        assert(near(r[0].markers[2].value, std::sqrt(70.0 * 70 + 30 * 30) * 0.001));
    }
    {   // enclosure: enough margin, too little on one side, flush with an edge, and partly outside
        Shapes s;
        s.add(2, {0, 0, 1000, 1000});
        s.add(1, {100, 100, 270, 270});      // fine
        s.add(1, {30, 500, 200, 670});       // 30 from the left edge
        s.add(1, {500, 0, 670, 170});        // flush with the bottom edge
        s.add(1, {900, 800, 1070, 970});     // sticks out on the right
        auto r = runDeck(std::string(layers) + "rule e enclosure(a, b) < 0.04\n", s.flat);
        // The last one is also 30 from the top edge, over the part that is inside b.
        assert(r[0].markers.size() == 4);
        assert((r[0].markers[0].box == Rect{0, 500, 30, 670}) && near(r[0].markers[0].value, 0.03));
        assert((r[0].markers[1].box == Rect{500, 0, 670, 0}) && near(r[0].markers[1].value, 0));
        assert((r[0].markers[2].box == Rect{900, 970, 1000, 1000}) && near(r[0].markers[2].value, 0.03));
        assert((r[0].markers[3].box == Rect{1000, 800, 1070, 970}) && near(r[0].markers[3].value, 0));
    }
    {   // overhang: a gate whose end cap is 100 on one end and 130 on the other
        Shapes s;
        s.add(2, {0, 0, 1000, 500});         // diff
        s.add(1, {300, -100, 450, 630});     // poly
        auto r = runDeck(std::string(layers) + "rule cap overhang(a, b) < 0.13\nrule sd overhang(b, a) < 0.25\n"
                                               "rule sd2 overhang(b, a) < 0.4\n", s.flat);
        assert(r[0].markers.size() == 1 && (r[0].markers[0].box == Rect{300, -100, 450, 0}));
        assert(r[1].markers.empty());
        assert(r[2].markers.size() == 1 && (r[2].markers[0].box == Rect{0, 0, 300, 500}) && near(r[2].markers[0].value, 0.3));
    }
    {   // area, polygon selection, holes, exact rectangles
        Shapes s;
        s.add(1, {0, 0, 100, 100});           // 0.01
        s.add(1, {200, 0, 500, 300});         // 0.09, touches b
        s.add(1, {200, 300, 300, 400});       // same polygon: 0.1 in all
        s.add(1, {1000, 0, 1170, 170});       // inside b
        s.add(2, {500, 0, 600, 100});
        s.add(2, {900, -100, 1300, 300});
        s.add(2, {2000, 0, 2300, 300});       // a ring: a hole of 100 x 100
        s.add(1, {2100, 100, 2200, 200});
        std::string d = std::string(layers) +
            "ring = b - a\n"
            "rule small area(a) < 0.05\n"
            "rule big area(a) > 0.09\n"
            "rule touch interact(a, b)\n"
            "rule apart not_interact(a, b)\n"
            "rule in inside(a, b)\n"
            "rule out outside(a, b)\n"
            "rule hole holes(ring)\n"
            "rule shape not_rect(a, 0.17, 0.17)\n"
            "rule notrect not_rects(a)\n"
            "rule grown grow(a, 0.05) & b - a\n"
            "rule gone shrink(a, 0.06)\n";
        auto r = runDeck(d, s.flat);
        assert(r[0].markers.size() == 3);     // the two 100 x 100 squares and the 170 x 170 one
        assert(r[1].markers.size() == 1 && near(r[1].markers[0].value, 0.1));
        assert(r[2].markers.size() == 3);     // the L (touches), the square inside b, the one in the ring
        assert(r[3].markers.size() == 1);
        assert(r[4].markers.size() == 2);
        assert(r[5].markers.size() == 2);     // the first square and the L (touching is not overlapping)
        // b - a has two holes: where the contact was, and the middle of the ring.
        assert(r[6].markers.size() == 2 && (r[6].markers[0].box == Rect{1000, 0, 1170, 170}) &&
               (r[6].markers[1].box == Rect{2100, 100, 2200, 200}));
        assert(r[7].markers.size() == 3);
        assert(r[8].markers.size() == 1);
        assert(r[9].markers.size() == 3);     // next to the L, around the contact, around the hole
        assert(r[10].markers.size() == 2 && (r[10].markers[0].box == Rect{260, 60, 440, 240}));
    }
    {   // density: a 400 x 200 extent in 100 x 100 windows; one window is full, one half full
        Shapes s;
        s.add(2, {0, 0, 400, 200});
        s.add(1, {0, 0, 100, 100});
        s.add(1, {300, 100, 350, 200});
        auto r = runDeck(std::string(layers) + "rule lo density(a, 0.1) < 0.2\nrule hi density(a, 0.1) > 0.8\n"
                                               "rule all density(a, 0.4) < 0.2\nrule odd density(a, 0.15, 0.15) > 0.2\n", s.flat);
        assert(r[0].markers.size() == 6);
        assert(r[1].markers.size() == 1 && near(r[1].markers[0].value, 1.0));
        // The window is larger than the extent in y: one window, 15000 / 80000.
        assert(r[2].markers.size() == 1 && near(r[2].markers[0].value, 15000.0 / 80000.0));
        // 150 windows at x = 0, 150 and (moved back) 250; y = 0 and (moved back) 50.
        assert(r[3].markers.size() == 3 && near(r[3].markers[0].value, 10000.0 / 22500.0) &&
               (r[3].markers[2].box == Rect{250, 50, 400, 200}) && near(r[3].markers[2].value, 5000.0 / 22500.0));
    }
    {   // deck errors name the line
        drc::Deck deck;
        std::string err;
        std::istringstream a("layer m 1\nrule r width(n) < 0.1\n");
        assert(!drc::parseDeck(a, deck, err) && err == "line 2: layer n is not defined");
        std::istringstream b("layer m 1\nrule r width(m) > 0.1\n");
        drc::Deck d2;
        assert(!drc::parseDeck(b, d2, err) && err == "line 2: width only takes <");
        std::istringstream c("layer m 1\nrule r width(m) < 0.1\nrule r space(m) < 0.1\n");
        drc::Deck d3;
        assert(!drc::parseDeck(c, d3, err) && err == "line 3: rule r is defined twice (line 2)");
    }
}

void testCheckThreads() {
    // Random wires: every thread count reports the same markers.
    std::mt19937 rng(7);
    Shapes s;
    for (int i = 0; i < 60000; ++i) {
        int x = int(rng() % 300000), y = int(rng() % 300000);
        bool horiz = rng() & 1;
        int len = 200 + int(rng() % 3000), w = 140 + int(rng() % 3) * 20;
        s.add(1 + int(rng() % 2), horiz ? Rect{x, y, x + len, y + w} : Rect{x, y, x + w, y + len});
    }
    const char* deck = "layer a 1\nlayer b 2\nc = a & b\n"
                       "rule w width(a - b) < 0.1\nrule s space(a) < 0.2\nrule ab space(a, b) < 0.15\n"
                       "rule e enclosure(c, a) < 0.05\nrule o overhang(a, b) < 0.3\nrule ar area(c) < 0.03\n"
                       "rule d density(a, 20, 10) > 0.1\nrule g grow(c, 0.1) - a\n";
    auto one = runDeck(deck, s.flat, 1);
    size_t total = 0;
    for (const auto& r : one) { assert(!r.markers.empty()); total += r.markers.size(); }
    for (unsigned t : {2u, 8u}) {
        auto many = runDeck(deck, s.flat, t);
        for (size_t i = 0; i < one.size(); ++i) assert(many[i].markers == one[i].markers);
    }
    std::printf("  %zu markers, the same on 1, 2 and 8 threads\n", total);
}

// A small layout on the layers of tests/pv/mini.drc with one violation for most rules.
void writeBad(const std::string& dir) {
    gds::Library lib;
    lib.cells.resize(2);
    gds::Cell& via = lib.cells[0];
    via.name = "via";
    via.polygons.push_back({2, 0, {{0, 0}, {170, 0}, {170, 170}, {0, 170}}});
    gds::Cell& top = lib.cells[1];
    top.name = "bad";
    auto box = [&](int layer, int x1, int y1, int x2, int y2) {
        top.polygons.push_back({int16_t(layer), 0, {{x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}}});
    };
    box(1, 0, 0, 2000, 300);          // metal
    box(1, 0, 420, 2000, 520);        // 100 wide (min 140), 120 from the first (min 140)
    box(3, 0, 0, 5000, 5000);         // boundary for the density check
    gds::Ref r;
    r.cell = "via";
    r.origin = {100, 60};
    top.refs.push_back(r);            // fine: 60 inside the metal
    r.origin = {1000, 20};
    top.refs.push_back(r);            // 20 from the metal edge (min 30)
    gds::Path p;
    p.layer = 1; p.width = 200; p.pts = {{3000, 3000}, {3400, 3000}};
    top.paths.push_back(p);           // 0.08 square microns (min 0.083)
    std::string err;
    if (!gds::write(dir + "/pv_bad.gds", lib, err)) { std::fprintf(stderr, "%s\n", err.c_str()); std::exit(1); }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "-gen") {
        writeBad(argv[2]);
        return 0;
    }
    testBooleans();
    testSizing();
    testThreads();
    testTopology();
    testGds();
    testChecks();
    testCheckThreads();
    std::puts("test_pv: ok");
    return 0;
}
