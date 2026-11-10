// geom.hpp - integer polygon engine for the physical verification tools.
//
// A Region is a set of points of the plane bounded by axis-parallel edges,
// held as non-overlapping rectangles in a canonical form: at every x the
// rectangles cover maximal y intervals, and a rectangle runs in x for as long
// as its y interval stays exactly the same. Two regions that cover the same
// points therefore hold the same rectangles in the same order, whatever
// operations (and however many threads) produced them.
//
//   merge(rects)            any rectangles, overlapping or not -> Region
//   boolean(a, b, op)       OR, AND, A NOT B, XOR
//   sized(a, d)             grow (d > 0) or shrink (d < 0) by d on every side
//   boundary(a, vertical)   the boundary edges with their inside direction
//   components(a)           connected polygons
//
// Large inputs are cut into vertical strips that are scanned on separate
// threads and stitched back; the result does not depend on the thread count.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory_resource>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <vector>

namespace geom {

using C = int32_t;

struct Rect {
    C x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool operator==(const Rect& o) const { return x1 == o.x1 && y1 == o.y1 && x2 == o.x2 && y2 == o.y2; }
    bool operator<(const Rect& o) const {
        if (x1 != o.x1) return x1 < o.x1;
        if (y1 != o.y1) return y1 < o.y1;
        if (x2 != o.x2) return x2 < o.x2;
        return y2 < o.y2;
    }
    int64_t area() const { return int64_t(x2 - x1) * int64_t(y2 - y1); }
};

struct Region {
    std::vector<Rect> rects;   // canonical, sorted
    bool empty() const { return rects.empty(); }
    int64_t area() const {
        int64_t a = 0;
        for (const Rect& r : rects) a += r.area();
        return a;
    }
    Rect bbox() const {
        if (rects.empty()) return {};
        Rect b = rects[0];
        for (const Rect& r : rects) {
            b.x1 = std::min(b.x1, r.x1); b.y1 = std::min(b.y1, r.y1);
            b.x2 = std::max(b.x2, r.x2); b.y2 = std::max(b.y2, r.y2);
        }
        return b;
    }
    bool operator==(const Region& o) const { return rects == o.rects; }
};

enum class Op { Or, And, Sub, Xor };

// ---------------------------------------------------------------------------
// Threads. Stand-in for the shared src/parallel.hpp (same call shape): runs
// f(i) for every i in [0, n) on up to `threads` threads, the caller included.
// Calls may nest: all of them share one budget of `threads` running threads,
// so an inner loop uses the threads that the outer loop has finished with.
// ---------------------------------------------------------------------------
inline std::atomic<int>& extraThreads() {
    static std::atomic<int> n{0};
    return n;
}

template <class F>
void parallelFor(size_t n, unsigned threads, F&& f) {
    if (n == 0) return;
    int want = int(std::min<size_t>(n, threads ? threads : 1)) - 1;
    int got = 0;
    std::atomic<int>& busy = extraThreads();
    while (got < want) {
        int cur = busy.load(std::memory_order_relaxed);
        if (cur + 1 >= int(threads)) break;
        if (busy.compare_exchange_weak(cur, cur + 1)) ++got;
    }
    if (got == 0) {
        for (size_t i = 0; i < n; ++i) f(i);
        return;
    }
    std::atomic<size_t> next{0};
    auto run = [&] {
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) f(i);
    };
    std::vector<std::thread> pool;
    pool.reserve(size_t(got));
    for (int t = 0; t < got; ++t)
        pool.emplace_back([&] {
            run();
            busy.fetch_sub(1);   // free for other loops as soon as this one has no more work
        });
    run();
    for (std::thread& t : pool) t.join();
}

namespace detail {

struct Ev {
    C x, y1, y2;
    int8_t da, db;
};

struct Cnt {
    int32_t a = 0, b = 0;
    bool operator==(const Cnt& o) const { return a == o.a && b == o.b; }
};

inline bool pred(Op op, Cnt c) {
    switch (op) {
    case Op::Or: return c.a || c.b;
    case Op::And: return c.a && c.b;
    case Op::Sub: return c.a && !c.b;
    case Op::Xor: return (c.a != 0) != (c.b != 0);
    }
    return false;
}

// Scanline over events sorted by x. The status maps y to the two winding
// counts of the segment that starts there; `open` holds the output intervals
// that are currently covered, with the x where each began.
inline void scan(std::vector<Ev>& ev, Op op, std::vector<Rect>& out) {
    std::sort(ev.begin(), ev.end(), [](const Ev& p, const Ev& q) {
        if (p.x != q.x) return p.x < q.x;
        return p.y1 < q.y1;
    });
    std::pmr::unsynchronized_pool_resource pool;
    std::pmr::map<C, Cnt> st(&pool);
    st.emplace(INT32_MIN, Cnt{});
    struct Open { C y2, x; };
    std::pmr::map<C, Open> open(&pool);
    std::vector<std::pair<C, C>> touched, fresh;
    std::vector<std::pair<C, Open>> old;
    auto split = [&](C y) {
        auto it = st.lower_bound(y);
        if (it != st.end() && it->first == y) return it;
        return st.emplace_hint(it, y, std::prev(it)->second);
    };
    size_t i = 0, n = ev.size();
    while (i < n) {
        C x = ev[i].x;
        touched.clear();
        for (; i < n && ev[i].x == x; ++i) {
            const Ev& e = ev[i];
            if (e.y1 >= e.y2) continue;
            auto a = split(e.y1), b = split(e.y2);
            for (; a != b; ++a) { a->second.a += e.da; a->second.b += e.db; }
            if (!touched.empty() && e.y1 <= touched.back().second)
                touched.back().second = std::max(touched.back().second, e.y2);
            else
                touched.push_back({e.y1, e.y2});
        }
        for (auto [lo, hi] : touched) {
            // Drop break points that no longer separate different counts.
            for (auto it = st.lower_bound(lo); it != st.end() && it->first <= hi;) {
                if (it != st.begin() && std::prev(it)->second == it->second) it = st.erase(it);
                else ++it;
            }
            // The open intervals that meet [lo, hi], and the hull E of both.
            old.clear();
            C elo = lo, ehi = hi;
            auto oi = open.upper_bound(lo);
            if (oi != open.begin() && std::prev(oi)->second.y2 >= lo) --oi;
            auto first = oi;
            for (; oi != open.end() && oi->first <= hi; ++oi) {
                old.push_back(*oi);
                elo = std::min(elo, oi->first);
                ehi = std::max(ehi, oi->second.y2);
            }
            // The intervals that are covered now, inside E.
            fresh.clear();
            auto si = st.upper_bound(elo);
            --si;
            bool in = false;
            C start = 0;
            for (; si != st.end() && si->first < ehi; ++si) {
                bool p = pred(op, si->second);
                C y = std::max(si->first, elo);
                if (p && !in) { start = y; in = true; }
                else if (!p && in) { fresh.push_back({start, y}); in = false; }
            }
            if (in) fresh.push_back({start, ehi});
            // Keep the intervals that did not change; close and open the rest.
            bool same = old.size() == fresh.size();
            for (size_t k = 0; same && k < old.size(); ++k)
                same = old[k].first == fresh[k].first && old[k].second.y2 == fresh[k].second;
            if (same) continue;
            open.erase(first, oi);
            size_t a = 0, b = 0;
            while (a < old.size() || b < fresh.size()) {
                if (a < old.size() && b < fresh.size() && old[a].first == fresh[b].first &&
                    old[a].second.y2 == fresh[b].second) {
                    open.emplace(old[a].first, old[a].second);
                    ++a; ++b;
                } else if (b == fresh.size() || (a < old.size() && old[a].first <= fresh[b].first)) {
                    if (x > old[a].second.x) out.push_back({old[a].second.x, old[a].first, x, old[a].second.y2});
                    ++a;
                } else {
                    open.emplace(fresh[b].first, Open{fresh[b].second, x});
                    ++b;
                }
            }
        }
    }
}

inline void addEvents(std::vector<Ev>& ev, const std::vector<Rect>& rs, bool second, C c0, C c1) {
    for (const Rect& r : rs) {
        if (r.x1 >= c1 || r.x2 <= c0 || r.x1 >= r.x2 || r.y1 >= r.y2) continue;
        int8_t a = second ? 0 : 1, b = second ? 1 : 0;
        ev.push_back({std::max(r.x1, c0), r.y1, r.y2, a, b});
        ev.push_back({std::min(r.x2, c1), r.y1, r.y2, int8_t(-a), int8_t(-b)});
    }
}

constexpr size_t kStripMin = 40000;   // rectangles below which one scan is faster

inline Region run(const std::vector<Rect>& a, const std::vector<Rect>& b, Op op, unsigned threads) {
    Region out;
    size_t n = a.size() + b.size();
    const C lo = INT32_MIN, hi = INT32_MAX;
    if (threads <= 1 || n < kStripMin) {
        std::vector<Ev> ev;
        ev.reserve(2 * n);
        addEvents(ev, a, false, lo, hi);
        addEvents(ev, b, true, lo, hi);
        scan(ev, op, out.rects);
        std::sort(out.rects.begin(), out.rects.end());
        return out;
    }
    // Cut at quantiles of the left sides, scan each strip, stitch.
    std::vector<C> xs;
    xs.reserve(n / 16 + 2);
    for (size_t i = 0; i < a.size(); i += 16) xs.push_back(a[i].x1);
    for (size_t i = 0; i < b.size(); i += 16) xs.push_back(b[i].x1);
    std::sort(xs.begin(), xs.end());
    size_t strips = std::min<size_t>(size_t(threads) * 4, n / (kStripMin / 4));
    std::vector<C> cuts;
    for (size_t s = 1; s < strips; ++s) {
        C c = xs[s * xs.size() / strips];
        if (cuts.empty() || c > cuts.back()) cuts.push_back(c);
    }
    size_t ns = cuts.size() + 1;
    std::vector<std::vector<Rect>> part(ns);
    parallelFor(ns, threads, [&](size_t s) {
        C c0 = s ? cuts[s - 1] : lo, c1 = s < cuts.size() ? cuts[s] : hi;
        std::vector<Ev> ev;
        addEvents(ev, a, false, c0, c1);
        addEvents(ev, b, true, c0, c1);
        scan(ev, op, part[s]);
    });
    // A rectangle that ends at a cut continues in the rectangle that starts
    // there with the same y interval.
    std::vector<Rect>& r = out.rects;
    std::vector<size_t> base(ns + 1, 0);
    for (size_t s = 0; s < ns; ++s) base[s + 1] = base[s] + part[s].size();
    r.reserve(base[ns]);
    for (auto& p : part) r.insert(r.end(), p.begin(), p.end());
    std::vector<char> dead(r.size(), 0);
    std::unordered_map<uint64_t, size_t> ends;
    auto key = [](const Rect& q) { return (uint64_t(uint32_t(q.y1)) << 32) | uint32_t(q.y2); };
    std::vector<size_t> carry, nextCarry;
    for (size_t s = 0; s + 1 < ns; ++s) {
        C cut = cuts[s];
        ends.clear();
        for (size_t i = base[s]; i < base[s + 1]; ++i)
            if (!dead[i] && r[i].x2 == cut) ends[key(r[i])] = i;
        for (size_t i : carry)
            if (r[i].x2 == cut) ends[key(r[i])] = i;
        nextCarry.clear();
        for (size_t i = base[s + 1]; i < base[s + 2]; ++i) {
            if (r[i].x1 != cut) continue;
            auto it = ends.find(key(r[i]));
            if (it == ends.end()) continue;
            r[it->second].x2 = r[i].x2;
            dead[i] = 1;
            nextCarry.push_back(it->second);
        }
        carry.swap(nextCarry);
    }
    size_t w = 0;
    for (size_t i = 0; i < r.size(); ++i)
        if (!dead[i]) r[w++] = r[i];
    r.resize(w);
    std::sort(r.begin(), r.end());
    return out;
}

}  // namespace detail

inline Region merge(const std::vector<Rect>& rects, unsigned threads = 1) {
    static const std::vector<Rect> none;
    return detail::run(rects, none, Op::Or, threads);
}

inline Region boolean(const Region& a, const Region& b, Op op, unsigned threads = 1) {
    return detail::run(a.rects, b.rects, op, threads);
}

// Grows (d > 0) or shrinks (d < 0) the region by |d| on every side.
inline Region sized(const Region& a, C d, unsigned threads = 1) {
    if (d == 0 || a.empty()) return a;
    if (d > 0) {
        std::vector<Rect> g;
        g.reserve(a.rects.size());
        for (const Rect& r : a.rects) g.push_back({r.x1 - d, r.y1 - d, r.x2 + d, r.y2 + d});
        return merge(g, threads);
    }
    // Shrinking is growing the outside: take the complement inside a frame,
    // grow it, and subtract it from the region.
    Rect b = a.bbox();
    C m = -d;
    Region frame;
    frame.rects.push_back({b.x1 - 2 * m, b.y1 - 2 * m, b.x2 + 2 * m, b.y2 + 2 * m});
    Region outside = sized(boolean(frame, a, Op::Sub, threads), m, threads);
    return boolean(a, outside, Op::Sub, threads);
}

inline Region transposed(const Region& a, unsigned threads = 1) {
    std::vector<Rect> t;
    t.reserve(a.rects.size());
    for (const Rect& r : a.rects) t.push_back({r.y1, r.x1, r.y2, r.x2});
    return merge(t, threads);
}

// A boundary edge at coordinate `at`, running from lo to hi along the other
// axis. inside = +1 when the region lies on the increasing side of the edge.
struct Edge {
    C at, lo, hi;
    int8_t inside;
};

// The vertical boundary edges (at = x, lo..hi = y range), or with
// vertical = false the horizontal ones (at = y, lo..hi = x range). Sorted by
// (at, lo); collinear pieces are joined.
inline std::vector<Edge> boundary(const Region& a, bool vertical) {
    struct S { C at, lo, hi; int8_t d; };
    std::vector<S> s;
    s.reserve(2 * a.rects.size());
    for (const Rect& r : a.rects) {
        if (vertical) { s.push_back({r.x1, r.y1, r.y2, 1}); s.push_back({r.x2, r.y1, r.y2, -1}); }
        else { s.push_back({r.y1, r.x1, r.x2, 1}); s.push_back({r.y2, r.x1, r.x2, -1}); }
    }
    std::sort(s.begin(), s.end(), [](const S& p, const S& q) { return p.at != q.at ? p.at < q.at : p.lo < q.lo; });
    std::vector<Edge> out;
    std::vector<std::pair<C, int>> pts;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        pts.clear();
        for (; j < s.size() && s[j].at == s[i].at; ++j) {
            pts.push_back({s[j].lo, s[j].d});
            pts.push_back({s[j].hi, -s[j].d});
        }
        std::sort(pts.begin(), pts.end());
        int cur = 0;
        size_t firstAt = out.size();
        C prev = 0;
        for (size_t k = 0; k < pts.size();) {
            C y = pts[k].first;
            if (cur != 0 && y > prev) {
                int8_t side = int8_t(cur > 0 ? 1 : -1);
                if (out.size() > firstAt && out.back().hi == prev && out.back().inside == side) out.back().hi = y;
                else out.push_back({s[i].at, prev, y, side});
            }
            for (; k < pts.size() && pts[k].first == y; ++k) cur += pts[k].second;
            prev = y;
        }
        i = j;
    }
    return out;
}

// Connected polygons: id[i] is the polygon of rects[i], numbered in order of
// each polygon's first rectangle. Rectangles that share part of a side are
// connected; touching at a corner does not connect.
inline size_t components(const Region& a, std::vector<uint32_t>& id) {
    size_t n = a.rects.size();
    std::vector<uint32_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](uint32_t v) {
        while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; }
        return v;
    };
    // Canonical rectangles only ever abut left-to-right.
    std::vector<uint32_t> byRight(n);
    std::iota(byRight.begin(), byRight.end(), 0u);
    std::sort(byRight.begin(), byRight.end(), [&](uint32_t p, uint32_t q) {
        const Rect &rp = a.rects[p], &rq = a.rects[q];
        return rp.x2 != rq.x2 ? rp.x2 < rq.x2 : rp.y1 < rq.y1;
    });
    size_t li = 0;
    for (size_t k = 0; k < n;) {
        C x = a.rects[byRight[k]].x2;
        size_t ke = k;
        while (ke < n && a.rects[byRight[ke]].x2 == x) ++ke;
        while (li < n && a.rects[li].x1 < x) ++li;
        size_t le = li;
        while (le < n && a.rects[le].x1 == x) ++le;
        size_t p = k, q = li;
        while (p < ke && q < le) {
            const Rect &rp = a.rects[byRight[p]], &rq = a.rects[q];
            if (std::max(rp.y1, rq.y1) < std::min(rp.y2, rq.y2)) {
                uint32_t u = find(byRight[p]), v = find(uint32_t(q));
                if (u != v) parent[std::max(u, v)] = std::min(u, v);
            }
            if (rp.y2 < rq.y2) ++p; else ++q;
        }
        k = ke;
    }
    id.assign(n, 0);
    std::vector<uint32_t> label(n, UINT32_MAX);
    uint32_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t r = find(uint32_t(i));
        if (label[r] == UINT32_MAX) label[r] = count++;
        id[i] = label[r];
    }
    return count;
}

// Calls f(i, j) for every pair a.rects[i], b.rects[j] whose interiors overlap
// (or, with touch = true, that also share part of a side or a corner).
template <class F>
void overlaps(const Region& a, const Region& b, bool touch, F&& f) {
    // Sweep over x; the rectangles of one canonical region that are alive at
    // one x have disjoint y intervals, so each side is a map by y1.
    struct Item { C x; uint32_t idx; bool isB, start; };
    std::vector<Item> ev;
    ev.reserve(2 * (a.rects.size() + b.rects.size()));
    for (uint32_t i = 0; i < a.rects.size(); ++i) {
        ev.push_back({a.rects[i].x1, i, false, true});
        ev.push_back({a.rects[i].x2, i, false, false});
    }
    for (uint32_t i = 0; i < b.rects.size(); ++i) {
        ev.push_back({b.rects[i].x1, i, true, true});
        ev.push_back({b.rects[i].x2, i, true, false});
    }
    // Without touch, ends come first at the same x; with touch, starts do.
    std::sort(ev.begin(), ev.end(), [&](const Item& p, const Item& q) {
        if (p.x != q.x) return p.x < q.x;
        if (p.start != q.start) return touch ? p.start : !p.start;
        if (p.isB != q.isB) return !p.isB;
        return p.idx < q.idx;
    });
    std::multimap<C, uint32_t> act[2];
    for (const Item& e : ev) {
        const Rect& r = (e.isB ? b : a).rects[e.idx];
        auto& mine = act[e.isB];
        if (!e.start) {
            auto range = mine.equal_range(r.y1);
            for (auto it = range.first; it != range.second; ++it)
                if (it->second == e.idx) { mine.erase(it); break; }
            continue;
        }
        const auto& other = act[!e.isB];
        const std::vector<Rect>& orects = (e.isB ? a : b).rects;
        // Rectangles of one region can touch along x at the same moment, so
        // walk back while the previous ones still reach r.y1.
        auto it = other.lower_bound(r.y1);
        for (int miss = 0; it != other.begin() && miss < 2;) {
            auto pv = std::prev(it);
            C y2 = orects[pv->second].y2;
            if (touch ? y2 >= r.y1 : y2 > r.y1) miss = 0; else ++miss;
            it = pv;
        }
        for (; it != other.end() && (touch ? it->first <= r.y2 : it->first < r.y2); ++it) {
            C y2 = orects[it->second].y2;
            if (touch ? y2 < r.y1 : y2 <= r.y1) continue;
            if (e.isB) f(it->second, e.idx); else f(e.idx, it->second);
        }
        mine.emplace(r.y1, e.idx);
    }
}

}  // namespace geom
