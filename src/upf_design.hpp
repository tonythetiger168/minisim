#pragma once
// upf_design.hpp - the elaborated design as UPF sees it, from slang.
//
// Shared by msim -upf and mlint -upf: instance paths, signal names and port
// connections as msim names them (relative to the top, '.' between levels,
// generate blocks as "gen[1]"), the module of each instance and the ports
// of each instance (find_objects, IEEE 1801-2024 6.33), and the power top a
// UPF file was written for (-power_top or set_design_top).
#include <functional>
#include <string>

#include "slang/ast/Compilation.h"
#include "slang/ast/Scope.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/text/SourceManager.h"
#include "upf.hpp"

namespace upf {

inline int portDirOf(slang::ast::ArgumentDirection d) {
    using slang::ast::ArgumentDirection;
    return d == ArgumentDirection::In ? 0 : d == ArgumentDirection::Out ? 1 : 2;
}

inline void walkDesign(const slang::ast::Scope& sc, const std::string& path, DesignView& dv, const slang::SourceManager& sm) {
    using namespace slang::ast;
    auto in = [&](const std::string& n) { return path.empty() ? n : path + "." + n; };
    auto loc = [&](slang::SourceLocation l) {
        if (!l) return std::string("?");
        auto f = sm.getFullyOriginalLoc(l);
        return std::string(sm.getFileName(f)) + ":" + std::to_string(sm.getLineNumber(f));
    };
    for (const Symbol& m : sc.members()) {
        if (m.kind == SymbolKind::Variable || m.kind == SymbolKind::Net) {
            dv.signals.insert(in(std::string(m.name)));
            dv.width[in(std::string(m.name))] = (int)m.as<ValueSymbol>().getType().getBitWidth();
        }
        if (m.kind == SymbolKind::Instance) {
            const auto& ch = m.as<InstanceSymbol>();
            std::string p = in(std::string(ch.name));
            dv.instances.insert(p);
            dv.model[p] = std::string(ch.getDefinition().name);
            auto& own = dv.portsOf[p];
            for (const Symbol* ps : ch.body.getPortList())
                if (ps && ps->kind == SymbolKind::Port)
                    own.push_back({std::string(ps->name), portDirOf(ps->as<PortSymbol>().direction)});
            for (const PortConnection* c : ch.getPortConnections()) {
                if (!c || c->port.kind != SymbolKind::Port) continue;
                const auto& ps = c->port.as<PortSymbol>();
                PortSite site;
                site.inst = p;
                site.port = std::string(ps.name);
                site.dir = portDirOf(ps.direction);
                site.width = (int)ps.getType().getBitWidth();
                const Expression* x = c->getExpression();
                site.where = loc(x && x->sourceRange.start() ? x->sourceRange.start() : ch.location);
                if (x) dv.ports.push_back(site);
            }
            walkDesign(ch.body, p, dv, sm);
        } else if (m.kind == SymbolKind::GenerateBlock) {
            const auto& g = m.as<GenerateBlockSymbol>();
            if (g.isUninstantiated) continue;
            std::string p = in(g.getExternalName());
            dv.instances.insert(p);
            walkDesign(g, p, dv, sm);
        } else if (m.kind == SymbolKind::GenerateBlockArray) {
            const auto& ga = m.as<GenerateBlockArraySymbol>();
            std::string an = ga.name.empty() ? std::string("genblk") : std::string(ga.name);
            for (const auto* e : ga.entries) {
                if (e->isUninstantiated) continue;
                std::string p = in(an + "[" + (e->getArrayIndex() ? e->getArrayIndex()->toString() : std::string("?")) + "]");
                dv.instances.insert(p);
                walkDesign(*e, p, dv, sm);
            }
        }
    }
}

// Every top instance; the top's own ports and module are those of path "".
inline DesignView designView(slang::ast::Compilation& comp) {
    using namespace slang::ast;
    DesignView dv;
    for (auto* t : comp.getRoot().topInstances) {
        if (!dv.model.count("")) {
            dv.model[""] = std::string(t->getDefinition().name);
            for (const Symbol* ps : t->body.getPortList())
                if (ps && ps->kind == SymbolKind::Port)
                    dv.portsOf[""].push_back({std::string(ps->name), portDirOf(ps->as<PortSymbol>().direction)});
        }
        walkDesign(t->body, "", dv, *comp.getSourceManager());
    }
    return dv;
}

// The simulator path of the power top: -power_top (when given) or the
// design top of the UPF, an instance path ("tb/u_dut", "tb.u_dut", "u_dut")
// or a module name (set_design_top dut). "" is the simulation top.
inline std::string powerTopPath(slang::ast::Compilation& comp, std::string top, bool explicitPath) {
    using namespace slang::ast;
    auto tops = comp.getRoot().topInstances;
    for (char& c : top) if (c == '/') c = '.';
    while (!top.empty() && top[0] == '.') top.erase(0, 1);
    // a path may start with the simulation top's own name (tb.u_dut)
    if (!tops.empty()) {
        std::string tn(tops[0]->name);
        if (top == tn || top.starts_with(tn + ".")) top = top.substr(std::min(top.size(), tn.size() + 1));
    }
    if (explicitPath || top.empty() || top.find('.') != std::string::npos) return top;
    // a module name: the first instance of it
    std::string found;
    std::function<void(const InstanceSymbol&, const std::string&)> walk = [&](const InstanceSymbol& in, const std::string& path) {
        if (!found.empty()) return;
        if (in.getDefinition().name == top && !path.empty()) { found = path; return; }
        for (auto& m : in.body.members())
            if (m.kind == SymbolKind::Instance) {
                auto& ch = m.as<InstanceSymbol>();
                walk(ch, path.empty() ? std::string(ch.name) : path + "." + std::string(ch.name));
            }
    };
    for (auto* t : tops) {
        if (t->getDefinition().name == top) return "";
        walk(*t, "");
    }
    return found.empty() ? top : found;
}

// Reads a UPF file against the design: find_objects sees the design, and
// set_design_top places the power top as soon as it is read.
inline void loadWithDesign(Model& m, const std::string& path, slang::ast::Compilation& comp, const DesignView& dv,
                           const std::string& powerTop, bool powerTopSet) {
    m.dv = &dv;
    m.topFixed = powerTopSet;
    m.resolveTop = [&comp](const std::string& t) { return powerTopPath(comp, t, false); };
    std::string start = powerTopSet ? powerTopPath(comp, powerTop, true) : "";
    m.load(path, start);
    if (!powerTopSet) m.top = powerTopPath(comp, m.designTop, false);
    m.dv = nullptr;
    m.resolveTop = nullptr;
}

}   // namespace upf
