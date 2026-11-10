// drc.hpp - design rule checking: the rule deck and the checks.
//
// A deck is a text file, one statement per line, # starts a comment.
// Lengths are in microns and areas in square microns.
//
//   layer poly 66/20                  a drawn layer: GDS layer/datatype (several allowed)
//   gate = poly & diff                a derived layer
//   rule poly.1a width(poly) < 0.15 "poly width"
//
// Layer expressions
//   a & b   a | b   a - b   a ^ b     AND, OR, A NOT B, XOR (& binds tighter)
//   grow(a, d)  shrink(a, d)          size every side by d
//   interact(a, b)                    polygons of a that overlap or touch b
//   not_interact(a, b)                the other polygons of a
//   inside(a, b)  not_inside(a, b)    polygons of a that are entirely within b, and the rest
//   outside(a, b)                     polygons of a that share no area with b
//   holes(a)                          the holes of a
//   rects(a)  not_rects(a)            polygons of a that are (not) rectangles
//   not_rect(a, w, h)                 polygons of a that are not w by h rectangles
//   extent()                          the bounding box of the layout
//
// Checks (each reports the places where the comparison is true)
//   width(a) < v                      inside distance between facing edges, and across corners
//   space(a) < v                      outside distance, between polygons and in notches
//   space(a, b) < v                   outside distance between a and b
//   enclosure(a, b) < v               a must be inside b with margin v: reports margins
//                                     from 0 up to v and the parts of a outside b
//   overhang(a, b) < v                where a crosses b, a must stick out of b by v
//   area(a) < v     area(a) > v       polygon area
//   density(a, w) < v   ... > v       area of a over area of each w by w window;
//   density(a, w, s) < v              windows step by s (default w); the last
//                                     window in each row and column is moved back to end on the extent
//   <layer expression>                any polygon is a violation
//
// Distances across corners are Euclidean. Results do not depend on the
// number of threads.
#pragma once

#include <cmath>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <sstream>
#include <string>

#include "geom.hpp"
#include "layout.hpp"

namespace drc {

using geom::C;
using geom::Edge;
using geom::Op;
using geom::Rect;
using geom::Region;

struct Expr {
    enum Kind { Name, Bool, Func } kind = Name;
    std::string name;                         // layer name or function name
    Op op = Op::Or;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<double> nums;
};

struct Rule {
    enum Kind { Flag, Width, Space, Enclosure, Overhang, Area, Density } kind = Flag;
    std::string id, desc;
    int line = 0;
    std::unique_ptr<Expr> a, b;
    char cmp = '<';
    double value = 0, window = 0, step = 0;
};

struct Deck {
    struct Drawn { std::string name; std::vector<layout::LayerKey> keys; };
    struct Derived { std::string name; std::unique_ptr<Expr> expr; };
    std::vector<Drawn> drawn;
    std::vector<Derived> derived;
    std::vector<Rule> rules;
};

// ---------------------------------------------------------------------------
// Deck parser
// ---------------------------------------------------------------------------
namespace detail {

struct Tok {
    enum Kind { End, Name, Num, Str, Punct } kind = End;
    std::string text;
    double num = 0;
};

struct Lexer {
    const std::string& s;
    size_t i = 0;
    Tok cur;
    std::string err;
    explicit Lexer(const std::string& line) : s(line) { next(); }
    static bool nameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }
    void next() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        cur = Tok();
        if (i >= s.size() || s[i] == '#') return;
        char c = s[i];
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            size_t j = i;
            while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
            // 66/20 style layer numbers are read by the layer statement itself.
            cur.kind = Tok::Num;
            cur.text = s.substr(i, j - i);
            cur.num = std::atof(cur.text.c_str());
            i = j;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < s.size() && nameChar(s[j])) ++j;
            cur.kind = Tok::Name;
            cur.text = s.substr(i, j - i);
            i = j;
        } else if (c == '"') {
            size_t j = s.find('"', i + 1);
            if (j == std::string::npos) { err = "unterminated string"; i = s.size(); return; }
            cur.kind = Tok::Str;
            cur.text = s.substr(i + 1, j - i - 1);
            i = j + 1;
        } else {
            cur.kind = Tok::Punct;
            cur.text = std::string(1, c);
            ++i;
        }
    }
    bool punct(const char* p) const { return cur.kind == Tok::Punct && cur.text == p; }
};

struct Parser {
    Lexer& lx;
    std::string& err;
    static int regionArgs(const std::string& f) {
        if (f == "extent") return 0;
        if (f == "grow" || f == "shrink" || f == "holes" || f == "rects" || f == "not_rects" || f == "not_rect") return 1;
        if (f == "interact" || f == "not_interact" || f == "inside" || f == "not_inside" || f == "outside") return 2;
        return -1;
    }
    static int numArgs(const std::string& f) {
        if (f == "grow" || f == "shrink") return 1;
        if (f == "not_rect") return 2;
        return 0;
    }
    std::unique_ptr<Expr> factor() {
        if (lx.punct("(")) {
            lx.next();
            auto e = expr();
            if (!e) return nullptr;
            if (!lx.punct(")")) { err = "expected )"; return nullptr; }
            lx.next();
            return e;
        }
        if (lx.cur.kind != Tok::Name) { err = "expected a layer name"; return nullptr; }
        auto e = std::make_unique<Expr>();
        e->name = lx.cur.text;
        lx.next();
        if (!lx.punct("(")) return e;
        int ra = regionArgs(e->name), na = numArgs(e->name);
        if (ra < 0) { err = "unknown function " + e->name; return nullptr; }
        e->kind = Expr::Func;
        lx.next();
        for (int k = 0; k < ra + na; ++k) {
            if (k && !lx.punct(",")) { err = e->name + " takes " + std::to_string(ra + na) + " arguments"; return nullptr; }
            if (k) lx.next();
            if (k < ra) {
                auto a = expr();
                if (!a) return nullptr;
                e->args.push_back(std::move(a));
            } else {
                if (lx.cur.kind != Tok::Num) { err = "expected a number in " + e->name; return nullptr; }
                e->nums.push_back(lx.cur.num);
                lx.next();
            }
        }
        if (!lx.punct(")")) { err = "expected ) after the arguments of " + e->name; return nullptr; }
        lx.next();
        return e;
    }
    std::unique_ptr<Expr> term() {
        auto l = factor();
        while (l && lx.punct("&")) {
            lx.next();
            auto r = factor();
            if (!r) return nullptr;
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Bool; e->op = Op::And;
            e->args.push_back(std::move(l)); e->args.push_back(std::move(r));
            l = std::move(e);
        }
        return l;
    }
    std::unique_ptr<Expr> expr() {
        auto l = term();
        while (l && (lx.punct("|") || lx.punct("-") || lx.punct("^"))) {
            Op op = lx.punct("|") ? Op::Or : lx.punct("-") ? Op::Sub : Op::Xor;
            lx.next();
            auto r = term();
            if (!r) return nullptr;
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Bool; e->op = op;
            e->args.push_back(std::move(l)); e->args.push_back(std::move(r));
            l = std::move(e);
        }
        return l;
    }
};

inline void names(const Expr& e, std::vector<std::string>& out) {
    if (e.kind == Expr::Name) out.push_back(e.name);
    for (const auto& a : e.args) names(*a, out);
}

}  // namespace detail

inline bool parseDeck(std::istream& in, Deck& deck, std::string& err) {
    using namespace detail;
    std::string line;
    int no = 0;
    std::unordered_map<std::string, bool> known;
    auto fail = [&](const std::string& m) {
        err = "line " + std::to_string(no) + ": " + m;
        return false;
    };
    auto checkNames = [&](const Expr& e, std::string& bad) {
        std::vector<std::string> ns;
        names(e, ns);
        for (const std::string& n : ns)
            if (!known.count(n)) { bad = n; return false; }
        return true;
    };
    while (std::getline(in, line)) {
        ++no;
        Lexer lx(line);
        if (!lx.err.empty()) return fail(lx.err);
        if (lx.cur.kind == Tok::End) continue;
        if (lx.cur.kind != Tok::Name) return fail("expected layer, rule, or a name");
        std::string head = lx.cur.text;
        std::string bad;
        if (head == "layer") {
            // layer NAME L/D [L/D ...]: read straight from the text.
            std::istringstream ss(line.substr(0, line.find('#')));
            std::string w, name;
            ss >> w >> name;
            if (name.empty()) return fail("layer needs a name");
            if (known.count(name)) return fail("layer " + name + " is defined twice");
            Deck::Drawn d;
            d.name = name;
            while (ss >> w) {
                layout::LayerKey k;
                char* end = nullptr;
                k.layer = int(std::strtol(w.c_str(), &end, 10));
                if (end == w.c_str()) return fail("bad layer number " + w);
                if (*end == '/') {
                    const char* q = end + 1;
                    k.datatype = int(std::strtol(q, &end, 10));
                    if (end == q) return fail("bad datatype in " + w);
                }
                if (*end) return fail("bad layer number " + w);
                d.keys.push_back(k);
            }
            if (d.keys.empty()) return fail("layer " + name + " needs a GDS layer number, as in 66/20");
            known[name] = true;
            deck.drawn.push_back(std::move(d));
            continue;
        }
        Parser ps{lx, err};
        if (head == "rule") {
            // The id is the next word, whatever characters it has.
            size_t p = lx.i;
            while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
            size_t q = p;
            while (q < line.size() && !std::isspace(static_cast<unsigned char>(line[q]))) ++q;
            if (q == p) return fail("rule needs a name");
            Rule r;
            r.id = line.substr(p, q - p);
            r.line = no;
            lx.i = q;
            lx.next();
            static const std::pair<const char*, Rule::Kind> checks[] = {
                {"width", Rule::Width}, {"space", Rule::Space}, {"enclosure", Rule::Enclosure},
                {"overhang", Rule::Overhang}, {"area", Rule::Area}, {"density", Rule::Density}};
            bool isCheck = false;
            if (lx.cur.kind == Tok::Name) {
                size_t save = lx.i;
                while (save < line.size() && std::isspace(static_cast<unsigned char>(line[save]))) ++save;
                for (const auto& c : checks)
                    if (lx.cur.text == c.first && save < line.size() && line[save] == '(') { r.kind = c.second; isCheck = true; }
            }
            if (isCheck) {
                std::string fn = lx.cur.text;
                lx.next();   // name
                lx.next();   // (
                r.a = ps.expr();
                if (!r.a) return fail(err);
                if (r.kind == Rule::Enclosure || r.kind == Rule::Overhang || (r.kind == Rule::Space && lx.punct(","))) {
                    if (!lx.punct(",")) return fail(fn + " takes two layers");
                    lx.next();
                    r.b = ps.expr();
                    if (!r.b) return fail(err);
                }
                if (r.kind == Rule::Density) {
                    if (!lx.punct(",")) return fail("density needs a window size, as in density(met1, 100)");
                    lx.next();
                    if (lx.cur.kind != Tok::Num || lx.cur.num <= 0) return fail("bad density window");
                    r.window = r.step = lx.cur.num;
                    lx.next();
                    if (lx.punct(",")) {
                        lx.next();
                        if (lx.cur.kind != Tok::Num || lx.cur.num <= 0) return fail("bad density step");
                        r.step = lx.cur.num;
                        lx.next();
                    }
                }
                if (!lx.punct(")")) return fail("expected ) in " + fn);
                lx.next();
                if (!lx.punct("<") && !lx.punct(">")) return fail("expected < or > after " + fn + "(...)");
                r.cmp = lx.cur.text[0];
                if (r.cmp == '>' && r.kind != Rule::Area && r.kind != Rule::Density)
                    return fail(fn + " only takes <");
                lx.next();
                if (lx.cur.kind != Tok::Num) return fail("expected a number after " + std::string(1, r.cmp));
                r.value = lx.cur.num;
                lx.next();
            } else {
                r.a = ps.expr();
                if (!r.a) return fail(err);
            }
            if (lx.cur.kind == Tok::Str) { r.desc = lx.cur.text; lx.next(); }
            if (!lx.err.empty()) return fail(lx.err);
            if (lx.cur.kind != Tok::End) return fail("unexpected '" + lx.cur.text + "'");
            if (!checkNames(*r.a, bad) || (r.b && !checkNames(*r.b, bad))) return fail("layer " + bad + " is not defined");
            for (const Rule& o : deck.rules)
                if (o.id == r.id) return fail("rule " + r.id + " is defined twice (line " + std::to_string(o.line) + ")");
            deck.rules.push_back(std::move(r));
            continue;
        }
        // NAME = expression
        lx.next();
        if (!lx.punct("=")) return fail("expected '=' after " + head);
        lx.next();
        if (known.count(head)) return fail("layer " + head + " is defined twice");
        if (Parser::regionArgs(head) >= 0) return fail(head + " is a function name");
        auto e = ps.expr();
        if (!e) return fail(err);
        if (lx.cur.kind != Tok::End) return fail("unexpected '" + lx.cur.text + "'");
        if (!checkNames(*e, bad)) return fail("layer " + bad + " is not defined");
        known[head] = true;
        deck.derived.push_back({head, std::move(e)});
    }
    return true;
}

inline bool readDeck(const std::string& file, Deck& deck, std::string& err) {
    std::ifstream in(file);
    if (!in) { err = "cannot open " + file; return false; }
    if (!parseDeck(in, deck, err)) { err = file + ": " + err; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

// A violation: the box between the two edges or corners (or the bounding box
// of a polygon or density window), and the measured value in microns, square
// microns, or as a ratio.
struct Marker {
    Rect box;
    double value = 0;
    bool operator<(const Marker& o) const {
        if (!(box == o.box)) return box < o.box;
        return value < o.value;
    }
    bool operator==(const Marker& o) const { return box == o.box && value == o.value; }
};

namespace detail {

struct Seg { C at, lo, hi; };
struct EdgePair { C a, b, lo, hi; };

inline std::vector<Seg> side(const std::vector<Edge>& es, int inside) {
    std::vector<Seg> out;
    for (const Edge& e : es)
        if (e.inside == inside) out.push_back({e.at, e.lo, e.hi});
    return out;
}

// The parts of g that no segment of a at the same coordinate covers. Both sorted by (at, lo).
inline std::vector<Seg> minus(const std::vector<Seg>& g, const std::vector<Seg>& a) {
    std::vector<Seg> out;
    size_t j = 0;
    for (const Seg& s : g) {
        while (j < a.size() && (a[j].at < s.at || (a[j].at == s.at && a[j].hi <= s.lo))) ++j;
        C lo = s.lo;
        for (size_t k = j; k < a.size() && a[k].at == s.at && a[k].lo < s.hi; ++k) {
            if (a[k].lo > lo) out.push_back({s.at, lo, a[k].lo});
            lo = std::max(lo, a[k].hi);
        }
        if (lo < s.hi) out.push_back({s.at, lo, s.hi});
    }
    return out;
}

// From every `from` edge, looks in direction dir (+1: increasing coordinate)
// for the nearest `to` edge and reports it when it is closer than dist. A
// `block` edge, or a nearer `from` edge, hides what lies behind it. With
// zero = true a distance of 0 is reported as well.
inline void sweep(const std::vector<Seg>& from, const std::vector<Seg>& to, const std::vector<Seg>& block,
                  C dist, bool zero, int dir, unsigned threads, std::vector<EdgePair>& out) {
    struct E { C at; uint8_t order, kind; C lo, hi; };
    enum { From, To, Block };
    std::vector<E> ev;
    ev.reserve(from.size() + to.size() + block.size());
    for (const Seg& s : from) ev.push_back({C(s.at * dir), uint8_t(zero ? 1 : 2), From, s.lo, s.hi});
    for (const Seg& s : to) ev.push_back({C(s.at * dir), uint8_t(zero ? 2 : 0), To, s.lo, s.hi});
    for (const Seg& s : block) ev.push_back({C(s.at * dir), uint8_t(zero ? 0 : 1), Block, s.lo, s.hi});
    std::sort(ev.begin(), ev.end(), [](const E& p, const E& q) {
        if (p.at != q.at) return p.at < q.at;
        if (p.order != q.order) return p.order < q.order;
        return p.lo < q.lo;
    });
    // One strip: the `from` edges in [c0, c1) are reported; the events up to
    // dist beyond c1 are needed to find and to hide their partners.
    auto strip = [&](size_t first, size_t ownEnd, size_t last, std::vector<EdgePair>& res) {
        struct P { C hi, at; bool own; };
        std::pmr::unsynchronized_pool_resource pool;
        std::pmr::map<C, P> pend(&pool);
        auto cut = [&](C lo, C hi, auto&& fn) {
            auto it = pend.upper_bound(lo);
            if (it != pend.begin() && std::prev(it)->second.hi > lo) --it;
            while (it != pend.end() && it->first < hi) {
                C plo = it->first;
                P p = it->second;
                C olo = std::max(plo, lo), ohi = std::min(p.hi, hi);
                it = pend.erase(it);
                if (plo < olo) pend.emplace(plo, P{olo, p.at, p.own});
                if (ohi < p.hi) it = std::next(pend.emplace(ohi, P{p.hi, p.at, p.own}).first);
                fn(olo, ohi, p);
            }
        };
        for (size_t i = first; i < last; ++i) {
            const E& e = ev[i];
            if (e.kind == To) {
                cut(e.lo, e.hi, [&](C lo, C hi, const P& p) {
                    int64_t d = int64_t(e.at) - p.at;
                    if (p.own && d < dist && (d > 0 || zero)) res.push_back({C(p.at * dir), C(e.at * dir), lo, hi});
                });
            } else {
                cut(e.lo, e.hi, [](C, C, const P&) {});
                if (e.kind == From) pend.emplace(e.lo, P{e.hi, e.at, i < ownEnd});
            }
        }
    };
    const size_t kMin = 50000;
    size_t strips = threads > 1 ? std::min<size_t>(size_t(threads) * 4, ev.size() / kMin) : 1;
    if (strips <= 1) {
        strip(0, ev.size(), ev.size(), out);
        return;
    }
    // Strip boundaries fall between different coordinates.
    std::vector<size_t> start{0};
    for (size_t s = 1; s < strips; ++s) {
        size_t i = s * ev.size() / strips;
        while (i < ev.size() && i > 0 && ev[i].at == ev[i - 1].at) ++i;
        if (i > start.back() && i < ev.size()) start.push_back(i);
    }
    start.push_back(ev.size());
    std::vector<std::vector<EdgePair>> part(start.size() - 1);
    geom::parallelFor(part.size(), threads, [&](size_t s) {
        size_t last = start[s + 1];
        if (last < ev.size()) {
            int64_t reach = int64_t(ev[last].at) + dist;
            last = size_t(std::upper_bound(ev.begin() + long(last), ev.end(), reach,
                                           [](int64_t v, const E& e) { return v < e.at; }) - ev.begin());
        }
        strip(start[s], start[s + 1], last, part[s]);
    });
    for (auto& p : part) out.insert(out.end(), p.begin(), p.end());
}

// A corner of the boundary. (qx, qy) is the quadrant the corner opens to:
// for a convex corner the outside quadrant opposite the polygon, for a
// concave corner the inside quadrant opposite the notch.
struct Corner {
    C x, y;
    int8_t qx, qy;
    bool convex;
};

inline std::vector<Corner> corners(const std::vector<Edge>& v, const std::vector<Edge>& h) {
    // Walk the boundary with the region on the left: a vertical edge with the
    // region to its right runs down, a horizontal edge with the region above
    // runs right. Every corner is the end of one edge and the start of
    // another; the two directions give the kind of corner.
    struct End {
        uint64_t key;   // (x, y), ordered
        int8_t dx, dy;
        bool in;
    };
    auto key = [](C x, C y) { return (uint64_t(uint32_t(x) ^ 0x80000000u) << 32) | (uint32_t(y) ^ 0x80000000u); };
    std::vector<End> ends;
    ends.reserve(2 * (v.size() + h.size()));
    for (const Edge& e : v) {
        if (e.inside > 0) { ends.push_back({key(e.at, e.hi), 0, -1, false}); ends.push_back({key(e.at, e.lo), 0, -1, true}); }
        else { ends.push_back({key(e.at, e.lo), 0, 1, false}); ends.push_back({key(e.at, e.hi), 0, 1, true}); }
    }
    for (const Edge& e : h) {
        if (e.inside > 0) { ends.push_back({key(e.lo, e.at), 1, 0, false}); ends.push_back({key(e.hi, e.at), 1, 0, true}); }
        else { ends.push_back({key(e.hi, e.at), -1, 0, false}); ends.push_back({key(e.lo, e.at), -1, 0, true}); }
    }
    std::sort(ends.begin(), ends.end(), [](const End& p, const End& q) {
        if (p.key != q.key) return p.key < q.key;
        if (p.in != q.in) return p.in;
        if (p.dx != q.dx) return p.dx < q.dx;
        return p.dy < q.dy;
    });
    std::vector<Corner> out;
    out.reserve(ends.size() / 2);
    for (size_t i = 0; i < ends.size();) {
        size_t j = i;
        while (j < ends.size() && ends[j].key == ends[i].key) ++j;
        C x = C(uint32_t(ends[i].key >> 32) ^ 0x80000000u), y = C(uint32_t(ends[i].key) ^ 0x80000000u);
        for (size_t a = i; a < j; ++a) {
            if (!ends[a].in) continue;
            // With two polygons meeting at a point, each incoming edge takes
            // the outgoing edge that turns left.
            const End* pick = nullptr;
            for (size_t b = i; b < j; ++b) {
                if (ends[b].in) continue;
                int cross = ends[a].dx * ends[b].dy - ends[a].dy * ends[b].dx;
                if (!pick || cross > 0) pick = &ends[b];
            }
            if (!pick) continue;
            int cross = ends[a].dx * pick->dy - ends[a].dy * pick->dx;
            if (cross == 0) continue;
            out.push_back({x, y, int8_t(ends[a].dx - pick->dx), int8_t(ends[a].dy - pick->dy), cross > 0});
        }
        i = j;
    }
    return out;
}

// Pairs of facing corners (one of a, one of b) closer than dist, measured
// straight across. Only corners of the given kind are used.
inline void cornerPairs(const std::vector<Corner>& a, const std::vector<Corner>& b, bool convex, C dist,
                        double dbu, std::vector<Marker>& out) {
    if (dist <= 0) return;
    struct B { uint64_t key; C x, y; int8_t qy; };
    auto cell = [&](C v) { return int64_t(std::floor(double(v) / dist)); };
    auto key = [](int64_t cx, int64_t cy) { return (uint64_t(uint32_t(cx)) << 32) | uint32_t(cy); };
    std::vector<B> grid;
    for (const Corner& c : b)
        if (c.convex == convex && c.qx < 0) grid.push_back({key(cell(c.x), cell(c.y)), c.x, c.y, c.qy});
    std::sort(grid.begin(), grid.end(), [](const B& p, const B& q) {
        if (p.key != q.key) return p.key < q.key;
        if (p.x != q.x) return p.x < q.x;
        return p.y < q.y;
    });
    int64_t d2 = int64_t(dist) * dist;
    for (const Corner& c : a) {
        if (c.convex != convex || c.qx <= 0) continue;
        int64_t cx = cell(c.x), cy = cell(c.y);
        for (int64_t ix = cx; ix <= cx + 1; ++ix)
            for (int64_t iy = cy; iy != cy + 2 * c.qy; iy += c.qy) {
                uint64_t k = key(ix, iy);
                auto it = std::lower_bound(grid.begin(), grid.end(), k, [](const B& p, uint64_t v) { return p.key < v; });
                for (; it != grid.end() && it->key == k; ++it) {
                    if (it->qy != -c.qy) continue;
                    int64_t dx = int64_t(it->x) - c.x, dy = (int64_t(it->y) - c.y) * c.qy;
                    if (dx <= 0 || dy <= 0 || dx * dx + dy * dy >= d2) continue;
                    Marker m;
                    m.box = {c.x, std::min(c.y, it->y), it->x, std::max(c.y, it->y)};
                    m.value = std::sqrt(double(dx * dx + dy * dy)) * dbu;
                    out.push_back(m);
                }
            }
    }
}

}  // namespace detail

// A layer with the boundary edges and corners that the checks need, worked
// out once however many rules use the layer.
class Shape {
public:
    Region region;
    Shape() = default;
    explicit Shape(Region r) : region(std::move(r)) {}
    const std::vector<Edge>& edges(bool vertical) const {
        std::call_once(vertical ? onceV_ : onceH_, [&] { (vertical ? v_ : h_) = geom::boundary(region, vertical); });
        return vertical ? v_ : h_;
    }
    const std::vector<detail::Corner>& corners() const {
        std::call_once(onceC_, [&] { c_ = detail::corners(edges(true), edges(false)); });
        return c_;
    }

private:
    mutable std::once_flag onceV_, onceH_, onceC_;
    mutable std::vector<Edge> v_, h_;
    mutable std::vector<detail::Corner> c_;
};

using ShapePtr = std::shared_ptr<const Shape>;

struct RuleResult {
    std::vector<Marker> markers;   // sorted
};

class Engine {
public:
    Engine(const Deck& deck, const layout::Flat& flat, unsigned threads)
        : deck_(deck), flat_(flat), threads_(threads ? threads : 1) {
        for (size_t i = 0; i < deck.drawn.size(); ++i) slots_[deck.drawn[i].name] = std::make_unique<Slot>(true, i);
        for (size_t i = 0; i < deck.derived.size(); ++i) slots_[deck.derived[i].name] = std::make_unique<Slot>(false, i);
    }

    // Runs every rule; results are in deck order.
    std::vector<RuleResult> run() {
        std::vector<RuleResult> res(deck_.rules.size());
        geom::parallelFor(deck_.rules.size(), threads_, [&](size_t i) {
            check(deck_.rules[i], res[i].markers);
            std::sort(res[i].markers.begin(), res[i].markers.end());
            res[i].markers.erase(std::unique(res[i].markers.begin(), res[i].markers.end()), res[i].markers.end());
        });
        return res;
    }

    ShapePtr layer(const std::string& name) {
        Slot& s = *slots_.at(name);
        std::call_once(s.once, [&] {
            if (s.drawn) {
                std::vector<Rect> all;
                for (const layout::LayerKey& k : deck_.drawn[s.index].keys) {
                    auto it = flat_.layers.find(k);
                    if (it != flat_.layers.end()) all.insert(all.end(), it->second.begin(), it->second.end());
                }
                s.shape = make(geom::merge(all, threads_));
            } else {
                s.shape = eval(*deck_.derived[s.index].expr);
            }
        });
        return s.shape;
    }

    ShapePtr eval(const Expr& e) {
        if (e.kind == Expr::Name) return layer(e.name);
        if (e.kind == Expr::Bool) {
            ShapePtr a = eval(*e.args[0]), b = eval(*e.args[1]);
            return make(geom::boolean(a->region, b->region, e.op, threads_));
        }
        const std::string& f = e.name;
        if (f == "extent") {
            Region r;
            if (flat_.extent.x1 < flat_.extent.x2 && flat_.extent.y1 < flat_.extent.y2) r.rects.push_back(flat_.extent);
            return make(std::move(r));
        }
        ShapePtr sa = eval(*e.args[0]);
        const Region& a = sa->region;
        if (f == "grow") return make(geom::sized(a, units(e.nums[0]), threads_));
        if (f == "shrink") return make(geom::sized(a, -units(e.nums[0]), threads_));
        if (f == "holes") return make(holes(a));
        std::vector<uint32_t> id;
        size_t n = geom::components(a, id);
        std::vector<char> hit(n, 0);
        bool keepHit = true;
        if (f == "rects" || f == "not_rects" || f == "not_rect") {
            std::vector<uint32_t> cnt(n, 0);
            for (uint32_t c : id) ++cnt[c];
            C w = 0, h = 0;
            if (f == "not_rect") { w = units(e.nums[0]); h = units(e.nums[1]); }
            for (size_t i = 0; i < id.size(); ++i) {
                const Rect& r = a.rects[i];
                bool ok = cnt[id[i]] == 1;
                if (ok && f == "not_rect") {
                    C rw = r.x2 - r.x1, rh = r.y2 - r.y1;
                    ok = (rw == w && rh == h) || (rw == h && rh == w);
                }
                if (ok) hit[id[i]] = 1;
            }
            keepHit = f == "rects";
        } else {
            ShapePtr sb = eval(*e.args[1]);
            const Region& b = sb->region;
            if (f == "interact" || f == "not_interact") {
                geom::overlaps(a, b, true, [&](uint32_t i, uint32_t) { hit[id[i]] = 1; });
                keepHit = f == "interact";
            } else if (f == "outside") {
                geom::overlaps(a, b, false, [&](uint32_t i, uint32_t) { hit[id[i]] = 1; });
                keepHit = false;
            } else {   // inside, not_inside: a polygon with a part outside b is not inside
                Region rest = geom::boolean(a, b, Op::Sub, threads_);
                geom::overlaps(a, rest, false, [&](uint32_t i, uint32_t) { hit[id[i]] = 1; });
                keepHit = f == "not_inside";
            }
        }
        Region out;
        for (size_t i = 0; i < id.size(); ++i)
            if (bool(hit[id[i]]) == keepHit) out.rects.push_back(a.rects[i]);
        return make(std::move(out));
    }

    C units(double microns) const { return C(std::llround(microns / flat_.dbu)); }

private:
    struct Slot {
        bool drawn;
        size_t index;
        std::once_flag once;
        ShapePtr shape;
        Slot(bool d, size_t i) : drawn(d), index(i) {}
    };

    static ShapePtr make(Region r) { return std::make_shared<Shape>(std::move(r)); }

    Region holes(const Region& a) {
        if (a.empty()) return {};
        Rect b = a.bbox();
        Region frame;
        frame.rects.push_back({b.x1 - 1, b.y1 - 1, b.x2 + 1, b.y2 + 1});
        Region rest = geom::boolean(frame, a, Op::Sub, threads_);
        std::vector<uint32_t> id;
        geom::components(rest, id);
        // The polygon of the first rectangle starts at the frame's left side: the outside.
        Region out;
        for (size_t i = 0; i < id.size(); ++i)
            if (id[i] != 0) out.rects.push_back(rest.rects[i]);
        return out;
    }

    void polygons(const Region& r, std::vector<Marker>& out, double lo, double hi) {
        // One marker per polygon whose area (square microns) is in (lo, hi).
        std::vector<uint32_t> id;
        size_t n = geom::components(r, id);
        std::vector<Rect> box(n);
        std::vector<double> area(n, 0);
        std::vector<char> seen(n, 0);
        for (size_t i = 0; i < id.size(); ++i) {
            const Rect& q = r.rects[i];
            Rect& b = box[id[i]];
            if (!seen[id[i]]) { b = q; seen[id[i]] = 1; }
            else { b.x1 = std::min(b.x1, q.x1); b.y1 = std::min(b.y1, q.y1); b.x2 = std::max(b.x2, q.x2); b.y2 = std::max(b.y2, q.y2); }
            area[id[i]] += double(q.area());
        }
        for (size_t c = 0; c < n; ++c) {
            double um2 = area[c] * flat_.dbu * flat_.dbu;
            if (um2 > lo && um2 < hi) out.push_back({box[c], um2});
        }
    }

    // Runs the four passes of an edge check (two axes, and for one-sided
    // checks two directions) and turns the pairs into markers.
    template <class Lists>
    void edgeCheck(C dist, bool zero, bool bothDirs, Lists&& lists, std::vector<Marker>& out) {
        std::vector<Marker> part[4];
        geom::parallelFor(bothDirs ? 4 : 2, threads_, [&](size_t k) {
            bool vertical = (k & 1) == 0;
            int dir = k < 2 ? 1 : -1;
            std::vector<detail::Seg> from, to, block;
            lists(vertical, dir, from, to, block);
            std::vector<detail::EdgePair> pairs;
            detail::sweep(from, to, block, dist, zero, dir, threads_, pairs);
            for (const detail::EdgePair& p : pairs) {
                C a = std::min(p.a, p.b), b = std::max(p.a, p.b);
                Marker m;
                m.box = vertical ? Rect{a, p.lo, b, p.hi} : Rect{p.lo, a, p.hi, b};
                m.value = double(b - a) * flat_.dbu;
                part[k].push_back(m);
            }
        });
        for (auto& p : part) out.insert(out.end(), p.begin(), p.end());
    }

    void check(const Rule& r, std::vector<Marker>& out) {
        using namespace detail;
        ShapePtr a = eval(*r.a);
        ShapePtr b = r.b ? eval(*r.b) : nullptr;
        C d = units(r.value);
        switch (r.kind) {
        case Rule::Flag:
            polygons(a->region, out, -1, INFINITY);
            break;
        case Rule::Area:
            if (r.cmp == '<') polygons(a->region, out, -1, r.value); else polygons(a->region, out, r.value, INFINITY);
            break;
        case Rule::Width:
        case Rule::Space: {
            if (r.kind == Rule::Space && b) {
                // Looking up and right from a to b, and from b to a.
                for (int swap = 0; swap < 2; ++swap) {
                    const Shape &f = swap ? *b : *a, &t = swap ? *a : *b;
                    edgeCheck(d, false, false, [&](bool vertical, int, auto& from, auto& to, auto& block) {
                        from = side(f.edges(vertical), -1);
                        to = side(t.edges(vertical), 1);
                        block = side(t.edges(vertical), -1);
                    }, out);
                    cornerPairs(f.corners(), t.corners(), true, d, flat_.dbu, out);
                }
                break;
            }
            int inside = r.kind == Rule::Width ? 1 : -1;
            edgeCheck(d, false, false, [&](bool vertical, int, auto& from, auto& to, auto&) {
                from = side(a->edges(vertical), inside);
                to = side(a->edges(vertical), -inside);
            }, out);
            cornerPairs(a->corners(), a->corners(), r.kind == Rule::Space, d, flat_.dbu, out);
            break;
        }
        case Rule::Enclosure: {
            edgeCheck(d, true, true, [&](bool vertical, int dir, auto& from, auto& to, auto& block) {
                from = side(a->edges(vertical), -dir);
                to = side(b->edges(vertical), -dir);
                block = side(b->edges(vertical), dir);
            }, out);
            Region rest = geom::boolean(a->region, b->region, Op::Sub, threads_);
            std::vector<Marker> outside;
            polygons(rest, outside, -1, INFINITY);
            for (Marker& m : outside) { m.value = 0; out.push_back(m); }
            break;
        }
        case Rule::Overhang: {
            Shape g(geom::boolean(a->region, b->region, Op::And, threads_));
            edgeCheck(d, false, true, [&](bool vertical, int dir, auto& from, auto& to, auto& block) {
                to = side(a->edges(vertical), -dir);
                from = minus(side(g.edges(vertical), -dir), to);
                block = side(a->edges(vertical), dir);
            }, out);
            break;
        }
        case Rule::Density:
            density(a->region, r, out);
            break;
        }
    }

    void density(const Region& a, const Rule& r, std::vector<Marker>& out) {
        Rect e = flat_.extent;
        int64_t W = e.x2 - int64_t(e.x1), H = e.y2 - int64_t(e.y1);
        if (W <= 0 || H <= 0) return;
        int64_t win = units(r.window), step = units(r.step);
        if (win <= 0 || step <= 0) return;
        auto test = [&](const Rect& box, double area) {
            double ratio = area / double(box.area());
            if (r.cmp == '<' ? ratio < r.value : ratio > r.value) out.push_back({box, ratio});
        };
        // Window origins along one axis; the last one is moved back to end on the extent.
        auto origins = [&](int64_t lo, int64_t len) {
            std::vector<int64_t> o;
            if (len <= win) { o.push_back(lo); return o; }
            for (int64_t p = lo; p + win <= lo + len; p += step) o.push_back(p);
            if (o.back() + win < lo + len) o.push_back(lo + len - win);
            return o;
        };
        std::vector<int64_t> xs = origins(e.x1, W), ys = origins(e.y1, H);
        int64_t ww = std::min(win, W), wh = std::min(win, H);
        // Area per window: each rectangle adds its overlap to the windows it meets.
        std::vector<double> sum(xs.size() * ys.size(), 0.0);
        auto range = [&](const std::vector<int64_t>& o, int64_t w, int64_t lo, int64_t hi, size_t& first, size_t& last) {
            // windows [o[i], o[i]+w) that overlap [lo, hi)
            first = size_t(std::upper_bound(o.begin(), o.end(), lo - w) - o.begin());
            last = size_t(std::lower_bound(o.begin(), o.end(), hi) - o.begin());
        };
        for (const Rect& q : a.rects) {
            size_t x0, x1, y0, y1;
            range(xs, ww, q.x1, q.x2, x0, x1);
            range(ys, wh, q.y1, q.y2, y0, y1);
            for (size_t i = x0; i < x1; ++i) {
                int64_t ox = std::min<int64_t>(q.x2, xs[i] + ww) - std::max<int64_t>(q.x1, xs[i]);
                if (ox <= 0) continue;
                for (size_t j = y0; j < y1; ++j) {
                    int64_t oy = std::min<int64_t>(q.y2, ys[j] + wh) - std::max<int64_t>(q.y1, ys[j]);
                    if (oy > 0) sum[i * ys.size() + j] += double(ox) * double(oy);
                }
            }
        }
        for (size_t i = 0; i < xs.size(); ++i)
            for (size_t j = 0; j < ys.size(); ++j)
                test({C(xs[i]), C(ys[j]), C(xs[i] + ww), C(ys[j] + wh)}, sum[i * ys.size() + j]);
    }

    const Deck& deck_;
    const layout::Flat& flat_;
    unsigned threads_;
    std::unordered_map<std::string, std::unique_ptr<Slot>> slots_;
};

}  // namespace drc
