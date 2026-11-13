// vhdl/front.hpp - msim's hook for VHDL sources: after slang has parsed the
// SystemVerilog files, the VHDL files are translated (vhdl/sv.hpp) and the
// result is added as one more syntax tree, so the whole mixed design is
// elaborated, lowered and simulated by the one engine.
#pragma once

#include <iostream>

#include "slang/driver/Driver.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "vhdl/vhdl.hpp"

namespace vhdl {

// SystemVerilog modules of the design, for VHDL instances of them.
inline void collectSvModules(slang::driver::Driver& driver, Translator& tr) {
    using namespace slang::syntax;
    for (auto& t : driver.syntaxTrees) {
        auto& root = t->root();
        if (root.kind != SyntaxKind::CompilationUnit) continue;
        for (auto* m : root.as<CompilationUnitSyntax>().members) {
            if (m->kind != SyntaxKind::ModuleDeclaration) continue;
            auto& md = m->as<ModuleDeclarationSyntax>();
            Translator::SvModule sm;
            sm.name = std::string(md.header->name.valueText());
            if (md.header->parameters)
                for (auto* d : md.header->parameters->declarations)
                    if (d->kind == SyntaxKind::ParameterDeclaration)
                        for (auto* dd : d->as<ParameterDeclarationSyntax>().declarators) sm.params.push_back(std::string(dd->name.valueText()));
            for (auto* item : md.members)
                if (item->kind == SyntaxKind::ParameterDeclarationStatement) {
                    auto* pd = item->as<ParameterDeclarationStatementSyntax>().parameter.get();
                    if (pd->kind == SyntaxKind::ParameterDeclaration && pd->as<ParameterDeclarationSyntax>().keyword.kind == slang::parsing::TokenKind::ParameterKeyword)
                        for (auto* dd : pd->as<ParameterDeclarationSyntax>().declarators) sm.params.push_back(std::string(dd->name.valueText()));
                }
            if (md.header->ports) {
                if (md.header->ports->kind == SyntaxKind::AnsiPortList) {
                    for (auto* p : md.header->ports->as<AnsiPortListSyntax>().ports) {
                        if (p->kind == SyntaxKind::ImplicitAnsiPort) sm.ports.push_back(std::string(p->as<ImplicitAnsiPortSyntax>().declarator->name.valueText()));
                        else if (p->kind == SyntaxKind::ExplicitAnsiPort) sm.ports.push_back(std::string(p->as<ExplicitAnsiPortSyntax>().name.valueText()));
                    }
                } else if (md.header->ports->kind == SyntaxKind::NonAnsiPortList) {
                    for (auto* p : md.header->ports->as<NonAnsiPortListSyntax>().ports) {
                        if (p->kind == SyntaxKind::ImplicitNonAnsiPort) {
                            auto* e = p->as<ImplicitNonAnsiPortSyntax>().expr.get();
                            if (e && e->kind == SyntaxKind::PortReference) sm.ports.push_back(std::string(e->as<PortReferenceSyntax>().name.valueText()));
                        } else if (p->kind == SyntaxKind::ExplicitNonAnsiPort)
                            sm.ports.push_back(std::string(p->as<ExplicitNonAnsiPortSyntax>().name.valueText()));
                    }
                }
            }
            tr.svModules[lowerStr(sm.name)] = sm;
        }
    }
}

// Translate `files` and add them to the driver. Returns false (after
// printing the error) on a VHDL error. `dumpSv`: also write the SV text there.
inline bool addVhdl(slang::driver::Driver& driver, const std::vector<std::string>& files, const std::string& dumpSv,
                    const std::string& tool, Translator& tr) {
    try {
        readFiles(tr, files);
        collectSvModules(driver, tr);
        static std::string text;   // the source manager keeps a view of it
        text = tr.run();
        for (auto& w : tr.warnings) std::cerr << tool << ": warning: " << w << "\n";
        if (!dumpSv.empty()) {
            std::ofstream o(dumpSv);
            o << text;
        }
        driver.syntaxTrees.push_back(slang::syntax::SyntaxTree::fromText(text, driver.sourceManager, "vhdl_design.sv", "", driver.createOptionBag()));
        // -top names a VHDL entity case-insensitively
        for (auto& t : driver.options.topModules) {
            std::string e = tr.entityText(lowerStr(t));
            if (!e.empty()) t = e;
        }
    } catch (const Error& e) {
        std::cerr << tool << ": VHDL: " << e.what() << "\n";
        return false;
    }
    return true;
}

}  // namespace vhdl
