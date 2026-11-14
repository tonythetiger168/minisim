// Lint rules that read the SDC file given with -sdc (mlint).
//
// The crossing checks (cdc.hpp) use the SDC for where clocks start and how
// they are related. These rules check the constraints against the design the
// way a lint tool's constraints goal does: every input and output of the top
// has an I/O delay, a clock is defined on a port that clocks something and
// not on a reset, and the file names only objects the design has.
//
// The SDC is read with the same reader (cdc_sdc.hpp, on tcl.hpp). The names
// are the ones a timing tool shows: top ports, instance paths with '/',
// pins as instance/port, nets, and a flop as its signal with _reg.
#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cdc_sdc.hpp"
#include "lintir.hpp"
#include "lintrules.hpp"

namespace lintsdc {
using namespace lintir;
using lintrules::isSignal;

struct View : cdcsdc::Design {
    std::vector<std::string> ports, pins, cells, nets;
    std::unordered_map<std::string, int> dir;

    static void put(std::vector<std::string>& into, const std::string& name, const Type& t) {
        if (t.isIntegral() && t.getBitWidth() > 1 && t.getBitWidth() <= 4096 && t.hasFixedRange()) {
            auto rg = t.getFixedRange();
            for (int i = rg.upper(); i >= rg.lower(); i--) into.push_back(name + "[" + std::to_string(i) + "]");
        } else {
            into.push_back(name);
        }
    }
    void walk(const Scope& sc, const std::string& pre) {
        for (const Symbol& s : sc.members()) {
            if (namePC() && !namePC()->visible(s)) continue;
            if (isSignal(s)) {
                std::string nm = pre + std::string(s.name);
                put(nets, nm, s.as<ValueSymbol>().getType());
                cells.push_back(nm + "_reg");   // any signal may be a flop; a name too many only hides a no-match
                for (const char* p : {"/Q", "/D", "/CK", "/CP", "/RN", "/SN"}) pins.push_back(nm + "_reg" + p);
            } else if (s.kind == SymbolKind::Instance) {
                const auto& ch = s.as<InstanceSymbol>();
                if (!ch.isModule() || ch.body.flags.has(InstanceFlags::Uninstantiated)) continue;
                std::string p = pre + std::string(ch.name);
                cells.push_back(p);
                for (const Symbol* ps : ch.body.getPortList())
                    if (ps->kind == SymbolKind::Port)
                        put(pins, p + "/" + std::string(ps->name), ps->as<PortSymbol>().getType());
                walk(ch.body, p + "/");
            } else if (s.kind == SymbolKind::GenerateBlock) {
                const auto& g = s.as<GenerateBlockSymbol>();
                if (!g.isUninstantiated) walk(g, pre + g.getExternalName() + ".");
            } else if (s.kind == SymbolKind::GenerateBlockArray) {
                const auto& ga = s.as<GenerateBlockArraySymbol>();
                std::string an = ga.name.empty() ? std::string("genblk") : std::string(ga.name);
                for (const auto* e : ga.entries)
                    if (!e->isUninstantiated)
                        walk(*e, pre + an + "[" + (e->getArrayIndex() ? e->getArrayIndex()->toString() : std::string("?")) + "].");
            }
        }
    }
    const std::vector<std::string>& names(cdcsdc::Kind k) const override {
        static const std::vector<std::string> none;
        switch (k) {
            case cdcsdc::Kind::Port: return ports;
            case cdcsdc::Kind::Pin: return pins;
            case cdcsdc::Kind::Cell: return cells;
            case cdcsdc::Kind::Net: return nets;
            default: return none;
        }
    }
    int portDir(const std::string& n) const override {
        auto it = dir.find(n);
        return it == dir.end() ? -1 : it->second;
    }
};

inline std::string base(const std::string& n) { return n.substr(0, n.find('[')); }

inline void run(Compilation& comp, lintrules::Global& g, const std::vector<std::string>& files, Report& rep) {
    if (files.empty()) return;
    bool any = false;
    for (const char* r : {"SDC001", "SDC002", "SDC003", "SDC004", "SDC005"}) any |= rep.rules.enabled(r);
    if (!any) return;

    // The top's ports, in order, and what the design does with each.
    struct Port {
        const PortSymbol* sym;
        int dir;   // 0 in, 1 out, 2 inout
    };
    std::map<std::string, Port> top;
    std::vector<std::string> order;
    View view;
    for (const InstanceSymbol* t : comp.getRoot().topInstances) {
        for (const Symbol* ps : t->body.getPortList()) {
            if (ps->kind != SymbolKind::Port) continue;
            const auto& port = ps->as<PortSymbol>();
            if (!port.internalSymbol || !isSignal(*port.internalSymbol)) continue;
            std::string name(port.name);
            int d = port.direction == ArgumentDirection::In ? 0 : port.direction == ArgumentDirection::Out ? 1 : 2;
            if (top.emplace(name, Port{&port, d}).second) order.push_back(name);
            size_t at = view.ports.size();
            View::put(view.ports, name, port.getType());
            view.dir[name] = d;
            for (size_t i = at; i < view.ports.size(); i++) view.dir[view.ports[i]] = d;
        }
        view.walk(t->body, "");
    }

    cdcsdc::Reader rd;
    rd.design = &view;
    size_t read = 0;
    for (auto& f : files) {
        try {
            rd.load(f);
            read++;
        } catch (const std::exception&) {
            // the crossing checks print the error
        }
    }
    if (!read) return;

    // SDC005: names that match nothing, and commands that are not SDC.
    for (auto& d : rd.diags)
        if (d.code == "no-match" || d.code == "unknown-command")
            rep.addAt("SDC005", d.loc.file, (size_t)std::max(d.loc.line, 1), 1,
                      d.code == "no-match" ? d.text + " in the design; the constraint applies to nothing"
                                           : d.text + "; the constraint is not applied");

    std::unordered_set<const Symbol*> clocks, resets, fed;
    for (auto& fr : g.flops) {
        if (fr.f.clk) clocks.insert(g.find(fr.f.clk));
        if (fr.f.arst) resets.insert(g.find(fr.f.arst));
        if (fr.f.srst) resets.insert(g.find(fr.f.srst));
    }
    for (auto& [t, s] : g.edges)
        if (s) fed.insert(g.find(s));
    auto canon = [&](const Port& p) { return g.find(p.sym->internalSymbol); };

    // What the SDC says about each port, by base name.
    std::set<std::string> clockPorts, inDelay, outDelay, constant, falseFrom, falseTo;
    std::map<std::string, std::string> clockOn;   // port -> clock name
    for (auto& c : rd.sdc.clocks)
        for (auto& o : c.sources)
            if (o.kind == cdcsdc::Kind::Port) {
                clockPorts.insert(base(o.name));
                clockOn.emplace(base(o.name), c.name);
            }
    for (auto& d : rd.sdc.ioDelays)
        for (auto& o : d.ports)
            if (o.kind == cdcsdc::Kind::Port) (d.input ? inDelay : outDelay).insert(base(o.name));
    for (auto& ca : rd.sdc.caseAnalysis)
        for (auto& o : ca.objects)
            if (o.kind == cdcsdc::Kind::Port) constant.insert(base(o.name));
    for (auto& e : rd.sdc.exceptions) {
        if (!e.path.through.empty()) continue;
        if (e.path.to.empty())
            for (auto& o : e.path.from) if (o.kind == cdcsdc::Kind::Port) falseFrom.insert(base(o.name));
        if (e.path.from.empty())
            for (auto& o : e.path.to) if (o.kind == cdcsdc::Kind::Port) falseTo.insert(base(o.name));
    }

    for (const std::string& name : order) {
        const Port& p = top[name];
        const Symbol* c = canon(p);
        bool isClock = clocks.count(c) || clockPorts.count(name);
        SourceLocation loc = p.sym->location;
        if (p.dir != 1 && !isClock && !resets.count(c) && !constant.count(name) && !falseFrom.count(name) &&
            !inDelay.count(name))
            rep.add("SDC001", loc,
                    "input '" + name + "' has no set_input_delay, so no path from it is timed; constrain it, or "
                    "set_false_path -from it if it is static");
        if (p.dir != 0 && !outDelay.count(name) && !falseTo.count(name))
            rep.add("SDC002", loc,
                    "output '" + name + "' has no set_output_delay, so no path to it is timed; constrain it, or "
                    "set_false_path -to it");
        if (clockPorts.count(name) && p.dir != 1) {
            if (resets.count(c) && !clocks.count(c))
                rep.add("SDC004", loc,
                        "'" + name + "' is a reset of the design but a clock (" + clockOn[name] +
                            ") in the SDC; a reset is timed with set_input_delay or a false path");
            else if (!clocks.count(c) && !fed.count(c))
                rep.add("SDC003", loc,
                        "clock " + clockOn[name] + " is defined on '" + name +
                            "', which clocks no flop and feeds no logic in the design");
        }
    }
}

}  // namespace lintsdc
