// lvs.hpp - layout versus schematic: netlist extraction from a layout and
// netlist comparison.
//
// extract() turns the connect, label and device statements of a deck
// (drc.hpp) into a transistor netlist: the polygons of the connected layers
// become nets, every gate polygon becomes a transistor with its width and
// length, and text labels name the nets and make them ports.
//
// compare() decides whether two netlists are the same circuit. Names do not
// have to agree, except the ports. Transistors in parallel are merged first
// (widths add up), source and drain are interchangeable. Matching is by
// partition refinement: nets and devices are coloured by what they connect
// to until nothing changes; where the circuit is symmetric, one pair is
// chosen and the refinement goes on.
#pragma once

#include <cinttypes>
#include <cstdio>
#include <map>
#include <set>

#include "drc.hpp"

namespace lvs {

using geom::C;
using geom::Rect;
using geom::Region;

struct Device {
    std::string model;
    uint32_t d = 0, g = 0, s = 0, b = 0;   // net indices
    double w = 0, l = 0;                   // microns
    int m = 1;                             // merged parallel devices
    std::string name;                      // instance name, or where it is in the layout
};

struct Netlist {
    std::string name;
    std::vector<std::string> nets;
    std::vector<uint32_t> ports;           // net indices, in order
    std::vector<Device> devices;

    uint32_t net(const std::string& n) {
        for (uint32_t i = 0; i < nets.size(); ++i)
            if (nets[i] == n) return i;
        nets.push_back(n);
        return uint32_t(nets.size() - 1);
    }
};

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------
namespace detail {

// Calls f(i, j, len) for every rectangle a[i] and b[j] that share a piece of
// side of length len (the regions must not overlap).
template <class F>
void adjacent(const Region& a, const Region& b, F&& f) {
    struct Side { C at, lo, hi; uint32_t idx; };
    auto join = [&](std::vector<Side>& p, std::vector<Side>& q) {
        auto less = [](const Side& x, const Side& y) { return x.at != y.at ? x.at < y.at : x.lo < y.lo; };
        std::sort(p.begin(), p.end(), less);
        std::sort(q.begin(), q.end(), less);
        size_t j = 0;
        for (const Side& s : p) {
            while (j < q.size() && (q[j].at < s.at || (q[j].at == s.at && q[j].hi <= s.lo))) ++j;
            for (size_t k = j; k < q.size() && q[k].at == s.at && q[k].lo < s.hi; ++k) {
                C len = std::min(s.hi, q[k].hi) - std::max(s.lo, q[k].lo);
                if (len > 0) f(s.idx, q[k].idx, len);
            }
        }
    };
    std::vector<Side> p, q;
    for (int pass = 0; pass < 4; ++pass) {
        p.clear();
        q.clear();
        for (uint32_t i = 0; i < a.rects.size(); ++i) {
            const Rect& r = a.rects[i];
            if (pass == 0) p.push_back({r.x2, r.y1, r.y2, i});
            else if (pass == 1) p.push_back({r.x1, r.y1, r.y2, i});
            else if (pass == 2) p.push_back({r.y2, r.x1, r.x2, i});
            else p.push_back({r.y1, r.x1, r.x2, i});
        }
        for (uint32_t i = 0; i < b.rects.size(); ++i) {
            const Rect& r = b.rects[i];
            if (pass == 0) q.push_back({r.x1, r.y1, r.y2, i});
            else if (pass == 1) q.push_back({r.x2, r.y1, r.y2, i});
            else if (pass == 2) q.push_back({r.y1, r.x1, r.x2, i});
            else q.push_back({r.y2, r.x1, r.x2, i});
        }
        join(p, q);
    }
}

inline std::string at(const Rect& r, double dbu) {
    char b[64];
    std::snprintf(b, sizeof b, "(%.3f, %.3f)", (r.x1 + double(r.x2)) / 2 * dbu, (r.y1 + double(r.y2)) / 2 * dbu);
    return b;
}

}  // namespace detail

// Extracts the netlist of the flattened layout. Problems that make the
// netlist unreliable (a gate without two source/drain regions, one name on
// two nets, two names on one net) are added to `problems`.
inline void extract(const drc::Deck& deck, drc::Engine& eng, const layout::Flat& flat, Netlist& out,
                    std::vector<std::string>& problems) {
    out = Netlist();
    out.name = flat.top;
    // Every layer that takes part, with its polygons numbered into one node space.
    struct Layer { drc::ShapePtr shape; std::vector<uint32_t> id; uint32_t base = 0, count = 0; };
    std::map<std::string, Layer> layers;
    auto use = [&](const std::string& n) { layers.emplace(n, Layer()); };
    for (const auto& c : deck.connects) { use(c.a); use(c.b); }
    for (const auto& l : deck.labels) use(l.layer);
    for (const auto& d : deck.devices) { use(d.gate); use(d.poly); use(d.sd); use(d.bulk); }
    uint32_t nodes = 0;
    for (auto& [name, l] : layers) {
        l.shape = eng.layer(name);
        l.count = uint32_t(geom::components(l.shape->region, l.id));
        l.base = nodes;
        nodes += l.count;
    }
    std::vector<uint32_t> parent(nodes);
    for (uint32_t i = 0; i < nodes; ++i) parent[i] = i;
    auto find = [&](uint32_t v) {
        while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; }
        return v;
    };
    auto unite = [&](uint32_t u, uint32_t v) {
        u = find(u); v = find(v);
        if (u != v) parent[std::max(u, v)] = std::min(u, v);
    };
    for (const auto& c : deck.connects) {
        const Layer &a = layers[c.a], &b = layers[c.b];
        geom::overlaps(a.shape->region, b.shape->region, false,
                       [&](uint32_t i, uint32_t j) { unite(a.base + a.id[i], b.base + b.id[j]); });
    }

    // Names from labels.
    std::map<uint32_t, std::string> nameOf;        // root -> name
    std::map<std::string, uint32_t> rootOf;
    std::vector<uint32_t> portRoots;
    for (const auto& l : deck.labels) {
        const Layer& lay = layers[l.layer];
        for (const layout::Label& t : flat.labels) {
            bool mine = false;
            for (const layout::LayerKey& k : l.keys) mine |= k.layer == t.layer && k.datatype == t.texttype;
            if (!mine) continue;
            const std::vector<Rect>& rs = lay.shape->region.rects;
            for (size_t i = 0; i < rs.size(); ++i) {
                if (t.x < rs[i].x1 || t.x > rs[i].x2 || t.y < rs[i].y1 || t.y > rs[i].y2) continue;
                uint32_t root = find(lay.base + lay.id[i]);
                auto have = nameOf.find(root);
                if (have != nameOf.end() && have->second != t.text)
                    problems.push_back("short: labels " + have->second + " and " + t.text + " are on the same net");
                else if (have == nameOf.end()) {
                    auto other = rootOf.find(t.text);
                    if (other != rootOf.end() && other->second != root)
                        problems.push_back("open: label " + t.text + " is on two nets that are not connected");
                    else {
                        nameOf[root] = t.text;
                        rootOf[t.text] = root;
                        portRoots.push_back(root);
                    }
                }
                break;
            }
        }
    }

    std::map<uint32_t, uint32_t> netOf;   // root -> net index
    auto net = [&](uint32_t node) {
        uint32_t root = find(node);
        auto it = netOf.find(root);
        if (it != netOf.end()) return it->second;
        auto nm = nameOf.find(root);
        out.nets.push_back(nm != nameOf.end() ? nm->second : "n" + std::to_string(netOf.size() + 1));
        return netOf[root] = uint32_t(out.nets.size() - 1);
    };
    // Ports first, in name order, so that the numbering does not depend on the layout order.
    for (const auto& [name, root] : rootOf) out.ports.push_back(net(root));

    // Devices.
    for (const auto& def : deck.devices) {
        const Layer &gate = layers[def.gate], &poly = layers[def.poly], &sd = layers[def.sd], &bulk = layers[def.bulk];
        const Region& g = gate.shape->region;
        struct Info {
            Rect box{};
            double area = 0;
            bool seen = false;
            uint32_t polyNode = UINT32_MAX, bulkNode = UINT32_MAX;
            std::map<uint32_t, int64_t> sd;   // source/drain polygon -> shared edge length
        };
        std::vector<Info> info(gate.count);
        for (size_t i = 0; i < g.rects.size(); ++i) {
            Info& f = info[gate.id[i]];
            const Rect& r = g.rects[i];
            if (!f.seen) { f.box = r; f.seen = true; }
            f.box.x1 = std::min(f.box.x1, r.x1); f.box.y1 = std::min(f.box.y1, r.y1);
            f.box.x2 = std::max(f.box.x2, r.x2); f.box.y2 = std::max(f.box.y2, r.y2);
            f.area += double(r.area());
        }
        geom::overlaps(g, poly.shape->region, false, [&](uint32_t i, uint32_t j) { info[gate.id[i]].polyNode = poly.base + poly.id[j]; });
        geom::overlaps(g, bulk.shape->region, false, [&](uint32_t i, uint32_t j) { info[gate.id[i]].bulkNode = bulk.base + bulk.id[j]; });
        detail::adjacent(g, sd.shape->region, [&](uint32_t i, uint32_t j, C len) { info[gate.id[i]].sd[sd.id[j]] += len; });
        for (const Info& f : info) {
            std::string where = def.model + " at " + detail::at(f.box, flat.dbu);
            if (f.sd.size() != 2 || f.polyNode == UINT32_MAX) {
                problems.push_back("device " + where + " has " + std::to_string(f.sd.size()) +
                                   " source/drain regions (a transistor has 2)");
                continue;
            }
            if (f.bulkNode == UINT32_MAX) {
                problems.push_back("device " + where + " is not on a " + def.bulk + " polygon");
                continue;
            }
            Device d;
            d.model = def.model;
            d.name = where;
            auto it = f.sd.begin();
            int64_t len = it->second;
            d.s = net(sd.base + it->first);
            ++it;
            len += it->second;
            d.d = net(sd.base + it->first);
            d.g = net(f.polyNode);
            d.b = net(f.bulkNode);
            d.w = double(len) / 2 * flat.dbu;
            d.l = f.area * flat.dbu * flat.dbu / d.w;
            out.devices.push_back(std::move(d));
        }
    }
}

// The netlist as a SPICE subcircuit (M cards: drain gate source body).
inline std::string toSpice(const Netlist& n) {
    std::string s = ".subckt " + n.name;
    for (uint32_t p : n.ports) s += " " + n.nets[p];
    s += "\n";
    char buf[96];
    for (size_t i = 0; i < n.devices.size(); ++i) {
        const Device& d = n.devices[i];
        s += "M" + std::to_string(i) + " " + n.nets[d.d] + " " + n.nets[d.g] + " " + n.nets[d.s] + " " + n.nets[d.b] + " " + d.model;
        std::snprintf(buf, sizeof buf, " w=%.6g l=%.6g", d.w, d.l);
        s += buf;
        if (d.m != 1) s += " m=" + std::to_string(d.m);
        s += "\n";
    }
    s += ".ends\n";
    return s;
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

struct CompareOptions {
    bool mergeParallel = true;
    bool portsByName = true;
    double tolerance = 0.01;      // relative difference allowed in W and L
};

struct CompareResult {
    bool match = false;
    size_t devices[2] = {0, 0}, nets[2] = {0, 0};   // after merging; [0] layout, [1] schematic
    size_t matchedDevices = 0, matchedNets = 0;
    std::vector<std::string> messages;              // what differs
    std::vector<std::pair<uint32_t, uint32_t>> netPairs;   // layout net, schematic net
};

namespace detail {

inline uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    return h ^ (h >> 33);
}
inline uint64_t hashStr(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// Devices with the same model, length, gate, body and the same pair of
// source/drain nets become one device with the widths added.
inline void mergeParallel(Netlist& n) {
    std::map<std::tuple<std::string, int64_t, uint32_t, uint32_t, uint32_t, uint32_t>, size_t> seen;
    std::vector<Device> out;
    for (Device& d : n.devices) {
        auto key = std::make_tuple(d.model, int64_t(std::llround(d.l * 1e4)), d.g, d.b, std::min(d.s, d.d), std::max(d.s, d.d));
        auto it = seen.find(key);
        if (it == seen.end()) {
            seen[key] = out.size();
            out.push_back(d);
        } else {
            out[it->second].w += d.w;
            out[it->second].m += d.m;
        }
    }
    n.devices.swap(out);
}

struct Graph {
    const Netlist* n;
    std::vector<uint64_t> dev, net;                       // colours
    std::vector<std::vector<std::pair<uint32_t, int>>> uses;   // per net: (device, terminal class)
    std::vector<char> used;
};

}  // namespace detail

inline CompareResult compare(Netlist layout, Netlist schematic, const CompareOptions& opt = {}) {
    using namespace detail;
    CompareResult res;
    if (opt.mergeParallel) { mergeParallel(layout); mergeParallel(schematic); }
    Graph g[2];
    g[0].n = &layout;
    g[1].n = &schematic;
    const char* side[2] = {"layout", "schematic"};

    for (int k = 0; k < 2; ++k) {
        const Netlist& n = *g[k].n;
        g[k].uses.assign(n.nets.size(), {});
        g[k].used.assign(n.nets.size(), 0);
        for (uint32_t i = 0; i < n.devices.size(); ++i) {
            const Device& d = n.devices[i];
            g[k].uses[d.g].push_back({i, 1});
            g[k].uses[d.b].push_back({i, 2});
            g[k].uses[d.s].push_back({i, 3});
            g[k].uses[d.d].push_back({i, 3});
            g[k].used[d.g] = g[k].used[d.b] = g[k].used[d.s] = g[k].used[d.d] = 1;
        }
        for (uint32_t p : n.ports) g[k].used[p] = 1;
        res.devices[k] = n.devices.size();
        for (char u : g[k].used) res.nets[k] += u;
    }

    // Ports must agree by name.
    if (opt.portsByName) {
        std::set<std::string> names[2];
        for (int k = 0; k < 2; ++k)
            for (uint32_t p : g[k].n->ports) names[k].insert(g[k].n->nets[p]);
        for (int k = 0; k < 2; ++k)
            for (const std::string& s : names[k])
                if (!names[1 - k].count(s))
                    res.messages.push_back("port " + s + " is in the " + side[k] + " but not in the " + side[1 - k]);
    }

    auto run = [&](bool withSize, std::vector<std::string>& msgs, bool record) {
        for (int k = 0; k < 2; ++k) {
            const Netlist& n = *g[k].n;
            g[k].dev.resize(n.devices.size());
            g[k].net.assign(n.nets.size(), 7);
            for (size_t i = 0; i < n.devices.size(); ++i) {
                uint64_t h = hashStr(n.devices[i].model);
                if (withSize) {
                    // Sizes within the tolerance fall in the same bucket almost always;
                    // the exact comparison is made after matching.
                    h = mix(h, uint64_t(std::llround(std::log(n.devices[i].w) / std::log1p(4 * opt.tolerance))));
                    h = mix(h, uint64_t(std::llround(std::log(n.devices[i].l) / std::log1p(4 * opt.tolerance))));
                }
                g[k].dev[i] = h;
            }
            if (opt.portsByName)
                for (uint32_t p : n.ports) g[k].net[p] = mix(11, hashStr(n.nets[p]));
        }
        auto refine = [&] {
            size_t classes = 0;
            for (int iter = 0; iter < 1000; ++iter) {
                std::set<uint64_t> all;
                for (int k = 0; k < 2; ++k) {
                    const Netlist& n = *g[k].n;
                    std::vector<uint64_t> nd(n.devices.size()), nn(n.nets.size());
                    for (size_t i = 0; i < n.devices.size(); ++i) {
                        const Device& d = n.devices[i];
                        uint64_t a = g[k].net[d.s], b = g[k].net[d.d];
                        uint64_t h = mix(g[k].dev[i], g[k].net[d.g]);
                        h = mix(h, g[k].net[d.b]);
                        h = mix(h, std::min(a, b));
                        nd[i] = mix(h, std::max(a, b));
                    }
                    std::vector<uint64_t> tmp;
                    for (size_t i = 0; i < n.nets.size(); ++i) {
                        tmp.clear();
                        for (auto [dv, cls] : g[k].uses[i]) tmp.push_back(mix(g[k].dev[dv], uint64_t(cls)));
                        std::sort(tmp.begin(), tmp.end());
                        uint64_t h = g[k].net[i];
                        for (uint64_t t : tmp) h = mix(h, t);
                        nn[i] = h;
                    }
                    g[k].dev.swap(nd);
                    g[k].net.swap(nn);
                    for (uint64_t c : g[k].dev) all.insert(c);
                    for (size_t i = 0; i < n.nets.size(); ++i)
                        if (g[k].used[i]) all.insert(g[k].net[i]);
                }
                if (all.size() == classes) break;
                classes = all.size();
            }
        };
        // Colour classes: colour -> members on each side.
        struct Class { std::vector<uint32_t> m[2]; };
        bool ok = true;
        std::map<uint64_t, Class> devs, nets;
        for (int round = 0; round < 100000; ++round) {
            refine();
            devs.clear();
            nets.clear();
            for (int k = 0; k < 2; ++k) {
                for (uint32_t i = 0; i < g[k].dev.size(); ++i) devs[g[k].dev[i]].m[k].push_back(i);
                for (uint32_t i = 0; i < g[k].net.size(); ++i)
                    if (g[k].used[i]) nets[g[k].net[i]].m[k].push_back(i);
            }
            ok = true;
            for (auto& [c, cl] : devs) ok &= cl.m[0].size() == cl.m[1].size();
            for (auto& [c, cl] : nets) ok &= cl.m[0].size() == cl.m[1].size();
            if (!ok) break;
            // A symmetric class: fix one pair and refine again. The smallest
            // class first, nets before devices, for a stable choice.
            Class* pick = nullptr;
            bool isNet = false;
            uint64_t colour = 0;
            for (auto& [c, cl] : nets)
                if (cl.m[0].size() > 1 && (!pick || cl.m[0].size() < pick->m[0].size())) { pick = &cl; isNet = true; colour = c; }
            if (!pick)
                for (auto& [c, cl] : devs)
                    if (cl.m[0].size() > 1 && (!pick || cl.m[0].size() < pick->m[0].size())) { pick = &cl; colour = c; }
            if (!pick) break;
            uint64_t fresh = mix(colour, 0xabcdef12345ull + uint64_t(round));
            for (int k = 0; k < 2; ++k) (isNet ? g[k].net : g[k].dev)[pick->m[k][0]] = fresh;
        }
        if (record) {
            res.matchedDevices = res.matchedNets = 0;
            res.netPairs.clear();
            for (auto& [c, cl] : devs)
                if (cl.m[0].size() == 1 && cl.m[1].size() == 1) ++res.matchedDevices;
            for (auto& [c, cl] : nets)
                if (cl.m[0].size() == 1 && cl.m[1].size() == 1) {
                    ++res.matchedNets;
                    res.netPairs.push_back({cl.m[0][0], cl.m[1][0]});
                }
        }
        if (ok) {
            // Sizes of the matched devices.
            for (auto& [c, cl] : devs) {
                if (cl.m[0].size() != 1) continue;
                const Device &a = layout.devices[cl.m[0][0]], &b = schematic.devices[cl.m[1][0]];
                auto differs = [&](double x, double y) { return std::fabs(x - y) > opt.tolerance * std::max(x, y); };
                if (differs(a.w, b.w) || differs(a.l, b.l)) {
                    char buf[200];
                    std::snprintf(buf, sizeof buf, "size: %s is w=%.4g l=%.4g in the layout, %s is w=%.4g l=%.4g in the schematic",
                                  a.name.c_str(), a.w, a.l, b.name.c_str(), b.w, b.l);
                    msgs.push_back(buf);
                    ok = false;
                }
            }
            return ok;
        }
        // What does not pair up, a few of each.
        size_t shown = 0;
        for (auto& [c, cl] : devs) {
            if (cl.m[0].size() == cl.m[1].size()) continue;
            for (int k = 0; k < 2; ++k)
                for (size_t i = cl.m[1 - k].size(); i < cl.m[k].size() && shown < 20; ++i, ++shown) {
                    const Device& d = g[k].n->devices[cl.m[k][i]];
                    msgs.push_back(std::string("unmatched device in the ") + side[k] + ": " + d.name + " " + d.model + " (d=" +
                                   g[k].n->nets[d.d] + " g=" + g[k].n->nets[d.g] + " s=" + g[k].n->nets[d.s] + " b=" + g[k].n->nets[d.b] + ")");
                }
        }
        shown = 0;
        for (auto& [c, cl] : nets) {
            if (cl.m[0].size() == cl.m[1].size()) continue;
            for (int k = 0; k < 2; ++k)
                for (size_t i = cl.m[1 - k].size(); i < cl.m[k].size() && shown < 20; ++i, ++shown)
                    msgs.push_back(std::string("unmatched net in the ") + side[k] + ": " + g[k].n->nets[cl.m[k][i]] + " (" +
                                   std::to_string(g[k].uses[cl.m[k][i]].size()) + " connections)");
        }
        return false;
    };

    char buf[160];
    if (res.devices[0] != res.devices[1]) {
        std::snprintf(buf, sizeof buf, "the layout has %zu devices, the schematic %zu", res.devices[0], res.devices[1]);
        res.messages.push_back(buf);
    }
    if (res.nets[0] != res.nets[1]) {
        std::snprintf(buf, sizeof buf, "the layout has %zu nets, the schematic %zu", res.nets[0], res.nets[1]);
        res.messages.push_back(buf);
    }
    std::vector<std::string> msgs;
    bool ok = run(true, msgs, true);
    if (!ok) {
        // Again without sizes in the colours: this separates a different
        // circuit from the same circuit with different sizes.
        msgs.clear();
        ok = run(false, msgs, true);
    }
    res.messages.insert(res.messages.end(), msgs.begin(), msgs.end());
    res.match = ok && res.messages.empty();
    return res;
}

}  // namespace lvs
