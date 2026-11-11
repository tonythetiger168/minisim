// layout_def.hpp - a routed DEF with its LEF as a flat layout, and a rule
// deck made from the LEF technology, for the physical verification tools.
//
// fromDef() places the pin shapes of every component (and, when asked, the
// obstructions), the routed wires and vias of the nets and special nets, and
// the shapes of the design's pins. LEF layers map to GDS layer numbers by
// their position in the LEF (first layer = 1, datatype 0), the same default
// the place and route tool uses when it writes GDS.
//
// The LEF and DEF readers are src/lef.hpp and src/def.hpp (namespace phys).
#pragma once

#include "def.hpp"
#include "layout.hpp"

namespace layout {

using LayerMap = std::map<std::string, LayerKey>;

inline LayerMap defaultLayerMap(const phys::Tech& tech) {
    LayerMap m;
    for (size_t i = 0; i < tech.layers.size(); ++i) m[tech.layers[i].name] = {int(i) + 1, 0};
    return m;
}

// Where a shape of a routed design comes from.
struct DefShape {
    enum Kind { CompPin, Obs, Wire, SpecialWire, IoPin } kind = Wire;
    uint32_t a = 0;   // CompPin, Obs: component index; Wire, SpecialWire: net index; IoPin: pin index
    uint32_t b = 0;   // CompPin: the pin's index in its macro
};

// Calls f(layer name, rectangle in design units, DefShape) for every shape of
// the placed components' pins (and obstructions, when asked), the routed
// wires and vias of the nets and special nets, and the design's pins.
// Diagonal wires are counted in `skipped`.
template <class F>
bool defShapes(const phys::Tech& tech, const phys::Design& d, bool withObs, size_t& skipped, std::string& err, F&& f) {
    if (tech.dbu != d.dbu) {
        // LEF shapes are in the LEF's units; bring them to the DEF's.
        if (d.dbu % tech.dbu != 0 && tech.dbu % d.dbu != 0) {
            err = "the LEF has " + std::to_string(tech.dbu) + " units per micron and the DEF " + std::to_string(d.dbu);
            return false;
        }
    }
    auto scale = [&](int64_t v) { return v * d.dbu / tech.dbu; };
    auto add = [&](const std::string& layer, phys::Rect r, DefShape src) {
        r.normalize();
        if (r.x0 == r.x1 || r.y0 == r.y1) return;
        f(layer, r, src);
    };
    auto lefRect = [&](phys::Rect r) { return phys::Rect{scale(r.x0), scale(r.y0), scale(r.x1), scale(r.y1)}; };
    for (uint32_t ci = 0; ci < d.comps.size(); ++ci) {
        const phys::Component& c = d.comps[ci];
        const phys::Macro* m = tech.macro(c.macro);
        if (!m) { err = "component " + c.name + ": no macro " + c.macro + " in the LEF"; return false; }
        if (c.status.empty()) continue;   // not placed
        int64_t w = scale(m->w), h = scale(m->h);
        for (uint32_t pi = 0; pi < m->pins.size(); ++pi)
            for (const phys::LayerRect& s : m->pins[pi].ports)
                add(s.layer, phys::xform(lefRect(s.r), c.orient, w, h, c.x, c.y), {DefShape::CompPin, ci, pi});
        if (withObs)
            for (const phys::LayerRect& s : m->obs)
                add(s.layer, phys::xform(lefRect(s.r), c.orient, w, h, c.x, c.y), {DefShape::Obs, ci, 0});
    }
    auto viaDef = [&](const std::string& n, bool& fromLef) -> const phys::ViaDef* {
        for (const phys::ViaDef& v : d.vias)
            if (v.name == n) { fromLef = false; return &v; }
        fromLef = true;
        return tech.via(n);
    };
    auto nets = [&](const std::vector<phys::Net>& ns, DefShape::Kind kind) {
        for (uint32_t ni = 0; ni < ns.size(); ++ni) {
            const phys::Net& n = ns[ni];
            DefShape src{kind, ni, 0};
            for (const phys::Seg& s : n.segs) {
                const phys::Layer* l = tech.layer(s.layer);
                int64_t w = s.width;
                if (!w && !n.ndr.empty() && !s.taper) {
                    auto r = d.ndrs.find(n.ndr);
                    const phys::Ndr* ndr = r != d.ndrs.end() ? &r->second : nullptr;
                    if (!ndr) {
                        auto t = tech.ndrs.find(n.ndr);
                        if (t != tech.ndrs.end()) ndr = &t->second;
                    }
                    if (ndr) {
                        auto li = ndr->find(s.layer);
                        if (li != ndr->end()) w = li->second.width;   // already in the units of the file it came from
                    }
                }
                if (!w && l) w = scale(l->width);
                if (!w) { err = "net " + n.name + ": no width for layer " + s.layer; return false; }
                int64_t hw = w / 2, e0 = s.ext0 < 0 ? hw : s.ext0, e1 = s.ext1 < 0 ? hw : s.ext1;
                if (s.y0 == s.y1) {
                    bool fwd = s.x0 <= s.x1;
                    add(s.layer, {std::min(s.x0, s.x1) - (fwd ? e0 : e1), s.y0 - hw, std::max(s.x0, s.x1) + (fwd ? e1 : e0), s.y0 + hw}, src);
                } else if (s.x0 == s.x1) {
                    bool fwd = s.y0 <= s.y1;
                    add(s.layer, {s.x0 - hw, std::min(s.y0, s.y1) - (fwd ? e0 : e1), s.x0 + hw, std::max(s.y0, s.y1) + (fwd ? e1 : e0)}, src);
                } else {
                    ++skipped;   // a diagonal wire
                }
            }
            for (const phys::Via& v : n.vias) {
                bool fromLef = true;
                const phys::ViaDef* def = viaDef(v.via, fromLef);
                if (!def) { err = "net " + n.name + ": no via " + v.via + " in the LEF or the DEF"; return false; }
                for (const phys::LayerRect& s : def->shapes) {
                    phys::Rect r = fromLef ? lefRect(s.r) : s.r;
                    add(s.layer, {r.x0 + v.x, r.y0 + v.y, r.x1 + v.x, r.y1 + v.y}, src);
                }
            }
            for (const phys::LayerRect& s : n.rects) add(s.layer, s.r, src);
        }
        return true;
    };
    if (!nets(d.nets, DefShape::Wire) || !nets(d.specialNets, DefShape::SpecialWire)) return false;
    for (uint32_t pi = 0; pi < d.pins.size(); ++pi)
        for (const phys::LayerRect& s : d.pins[pi].shapes) add(s.layer, s.r, {DefShape::IoPin, pi, 0});
    return true;
}

inline bool fromDef(const phys::Tech& tech, const phys::Design& d, const LayerMap& map, bool withObs, Flat& out,
                    std::string& err) {
    out = Flat();
    out.top = d.name;
    out.dbu = 1.0 / d.dbu;
    bool ok = defShapes(tech, d, withObs, out.skipped, err, [&](const std::string& layer, const phys::Rect& r, DefShape src) {
        auto it = map.find(layer);
        if (it == map.end()) return;
        out.layers[it->second].push_back({C(r.x0), C(r.y0), C(r.x1), C(r.y1)});
        ++out.shapes;
        if (src.kind == DefShape::IoPin)
            out.labels.push_back({it->second.layer, it->second.datatype, C((r.x0 + r.x1) / 2), C((r.y0 + r.y1) / 2), d.pins[src.a].name});
    });
    if (!ok) return false;
    bool first = true;
    for (const auto& [key, rs] : out.layers)
        for (const Rect& r : rs) {
            if (first) { out.extent = r; first = false; continue; }
            out.extent.x1 = std::min(out.extent.x1, r.x1); out.extent.y1 = std::min(out.extent.y1, r.y1);
            out.extent.x2 = std::max(out.extent.x2, r.x2); out.extent.y2 = std::max(out.extent.y2, r.y2);
        }
    if (d.die.x1 > d.die.x0 && d.die.y1 > d.die.y0) out.extent = {C(d.die.x0), C(d.die.y0), C(d.die.x1), C(d.die.y1)};
    return true;
}

// A rule deck (drc.hpp format) from the LEF technology: width, spacing and
// minimum area of every routing layer; size, spacing and metal cover of
// every cut layer.
inline std::string deckFromLef(const phys::Tech& tech, const LayerMap& map) {
    std::ostringstream o;
    auto um = [&](int64_t v) {
        char b[48];
        std::snprintf(b, sizeof b, "%.6g", double(v) / tech.dbu);
        return std::string(b);
    };
    o << "# Rules from the LEF technology.\n";
    for (const phys::Layer& l : tech.layers) {
        auto it = map.find(l.name);
        if (it == map.end() || (l.type != phys::Layer::Routing && l.type != phys::Layer::Cut)) continue;
        o << "layer " << l.name << " " << it->second.layer << "/" << it->second.datatype << "\n";
    }
    const phys::Layer* below = nullptr;   // the routing layer under the current cut
    for (size_t i = 0; i < tech.layers.size(); ++i) {
        const phys::Layer& l = tech.layers[i];
        if (!map.count(l.name)) continue;
        if (l.type == phys::Layer::Routing) {
            if (l.width) o << "rule " << l.name << ".width width(" << l.name << ") < " << um(l.width) << " \"" << l.name << " width\"\n";
            if (l.spacing) o << "rule " << l.name << ".space space(" << l.name << ") < " << um(l.spacing) << " \"" << l.name << " spacing\"\n";
            if (l.minArea) {
                char b[48];
                std::snprintf(b, sizeof b, "%.6g", double(l.minArea) / tech.dbu / tech.dbu);
                o << "rule " << l.name << ".area area(" << l.name << ") < " << b << " \"" << l.name << " area\"\n";
            }
            below = &l;
        } else if (l.type == phys::Layer::Cut) {
            if (l.spacing) o << "rule " << l.name << ".space space(" << l.name << ") < " << um(l.spacing) << " \"" << l.name << " spacing\"\n";
            const phys::Layer* above = nullptr;
            for (size_t k = i + 1; k < tech.layers.size() && !above; ++k)
                if (tech.layers[k].type == phys::Layer::Routing && map.count(tech.layers[k].name)) above = &tech.layers[k];
            if (below) o << "rule " << l.name << ".below " << l.name << " - " << below->name << " \"" << l.name << " without " << below->name << "\"\n";
            if (above) o << "rule " << l.name << ".above " << l.name << " - " << above->name << " \"" << l.name << " without " << above->name << "\"\n";
        }
    }
    return o.str();
}

}  // namespace layout
