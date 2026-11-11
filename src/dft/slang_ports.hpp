// slang_ports.hpp - the ports of the top module, for mdft -bscan. Included
// only by mdft.cpp, after lintir.hpp (which brings in slang).
#pragma once
#include "bscan.hpp"

namespace dft {

inline std::vector<Port> topPorts(slang::ast::Compilation& comp, std::string& topName) {
    using namespace slang::ast;
    auto tops = comp.getRoot().topInstances;
    if (tops.empty()) throw Error("no top module");
    if (tops.size() > 1) throw Error("more than one top module: pick one with -top");
    const InstanceSymbol& inst = *tops[0];
    topName = std::string(inst.getDefinition().name);
    std::vector<Port> out;
    for (const Symbol* s : inst.body.getPortList()) {
        if (!s || s->kind != SymbolKind::Port)
            throw Error("port '" + std::string(s ? s->name : "?") + "' is not a plain port (an interface port?)");
        const auto& ps = s->as<PortSymbol>();
        const Type& t = ps.getType().getCanonicalType();
        if (!t.isIntegral()) throw Error("port '" + std::string(ps.name) + "' is not a bit vector");
        Port p;
        p.name = std::string(ps.name);
        p.dir = ps.direction == ArgumentDirection::In ? Port::In : ps.direction == ArgumentDirection::Out ? Port::Out : Port::InOut;
        int w = (int)t.getBitWidth();
        if (t.kind == SymbolKind::PackedArrayType) {
            auto r = t.getFixedRange();
            p.vec = true, p.msb = r.left, p.lsb = r.right;
            if ((int)r.width() != w) p.msb = w - 1, p.lsb = 0;  // an array of vectors: flat
        } else if (w > 1) p.vec = true, p.msb = w - 1, p.lsb = 0;
        out.push_back(p);
    }
    return out;
}

}  // namespace dft
