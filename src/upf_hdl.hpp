// upf_hdl.hpp - UPF attributes written in the HDL (IEEE 1801-2024 5.6):
// (* UPF_is_hard_macro = "TRUE" *) on a module, (* UPF_clamp_value = "1" *)
// on a port. Shared by msim and mlint.
#pragma once
#include <set>
#include <string>
#include <vector>

#include <slang/ast/Compilation.h>
#include <slang/ast/symbols/AttributeSymbol.h>
#include <slang/ast/symbols/BlockSymbols.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/text/SourceManager.h>
#include <slang/numeric/ConstantValue.h>
#include <functional>

#include "upf.hpp"

inline std::vector<upf::HdlAttr> upfHdlAttrs(slang::ast::Compilation& comp) {
    using namespace slang::ast;
    std::vector<upf::HdlAttr> out;
    std::set<const DefinitionSymbol*> seen;
    const auto* sm = comp.getSourceManager();
    auto where = [&](const Symbol& s) {
        if (!sm || !s.location) return std::string("?");
        auto l = sm->getFullyOriginalLoc(s.location);
        return std::string(sm->getFileName(l)) + ":" + std::to_string(sm->getLineNumber(l));
    };
    auto take = [&](std::span<const AttributeSymbol* const> as, const std::string& model, const std::string& port, const Symbol& at) {
        for (const AttributeSymbol* a : as) {
            std::string n(a->name);
            if (n.rfind("UPF_", 0) != 0) continue;
            const slang::ConstantValue& cv = a->getValue();
            std::string v;
            if (cv.isString()) v = cv.str();
            else if (cv.isInteger()) {
                // a string literal is an integer of 8-bit characters (5.9 of 1800)
                std::string t = cv.convertToStr().str();
                bool text = !t.empty() && !cv.integer().hasUnknown();
                for (char ch : t) if (!std::isprint((unsigned char)ch)) text = false;
                v = text ? t : cv.integer().toString(slang::LiteralBase::Decimal, false);
            } else v = cv.toString();
            out.push_back({model, port, n, v, where(at)});
        }
    };
    std::function<void(const InstanceSymbol&)> walk = [&](const InstanceSymbol& in) {
        const DefinitionSymbol& d = in.getDefinition();
        if (seen.insert(&d).second) {
            std::string model(d.name);
            take(comp.getAttributes(d), model, "", d);
            for (const Symbol* p : in.body.getPortList()) {
                if (p->kind != SymbolKind::Port) continue;
                const auto& ps = p->as<PortSymbol>();
                take(comp.getAttributes(ps), model, std::string(ps.name), ps);
                if (ps.internalSymbol && comp.getAttributes(ps).empty())
                    take(comp.getAttributes(*ps.internalSymbol), model, std::string(ps.name), ps);
            }
        }
        for (auto& m : in.body.members()) {
            if (m.kind == SymbolKind::Instance) walk(m.as<InstanceSymbol>());
            else if (m.kind == SymbolKind::GenerateBlock || m.kind == SymbolKind::GenerateBlockArray) {
                std::function<void(const Scope&)> gen = [&](const Scope& sc) {
                    for (auto& g : sc.members()) {
                        if (g.kind == SymbolKind::Instance) walk(g.as<InstanceSymbol>());
                        else if (g.kind == SymbolKind::GenerateBlock) gen(g.as<GenerateBlockSymbol>());
                        else if (g.kind == SymbolKind::GenerateBlockArray)
                            for (auto* e : g.as<GenerateBlockArraySymbol>().entries) gen(*e);
                    }
                };
                if (m.kind == SymbolKind::GenerateBlock) gen(m.as<GenerateBlockSymbol>());
                else for (auto* e : m.as<GenerateBlockArraySymbol>().entries) gen(*e);
            }
        }
    };
    for (auto* t : comp.getRoot().topInstances) walk(*t);
    return out;
}
