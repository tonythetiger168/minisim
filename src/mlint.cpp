// mlint.cpp - a lint and clock-domain checker for SystemVerilog, on the same
// slang front end as msim.
//
// Two layers:
//   * the front end's own warnings (slang), printed as they are;
//   * the rules below, which need the elaborated design.
//
// Rules
//   L001  a variable is assigned on some paths of an always_comb but not all,
//         so it holds its old value: an unintended latch.
//   ...   and the rules of lintrules.hpp: synthesizability, latches, case,
//         widths, unused signals, connections, clocks, resets, naming, style.
//         mlint -list shows them all; lintcfg.hpp is the table.
//   L002  a blocking assignment to a non-local variable in an always_ff.
//   L003  a non-blocking assignment in an always_comb.
//   L004  one variable or net is assigned by more than one process.
//   CDC001..CDC005, RDC001..RDC004  clock- and reset-domain crossings (cdc.hpp).
//   UPF001..UPF012  with -upf: power intent checks (upf.hpp check()).
#include "upf_hdl.hpp"
#include <unistd.h>

#include <chrono>
#include <iomanip>

#include "slang/analysis/AnalysisManager.h"
#include "slang/diagnostics/AnalysisDiags.h"
#include "slang/util/ThreadPool.h"
#include "cdc.hpp"
#include "cpu.hpp"
#include "lintir.hpp"
#include "lintrules.hpp"
#include "lintformal.hpp"
#include "lintsoc.hpp"
#include "lintsrc.hpp"
#include "lintvhdl.hpp"
#include "vhdl/front.hpp"
#ifdef MINISIM_TIDY
#include "linttidy.hpp"
#endif
#include "parallel.hpp"
#include "upf.hpp"
#include "upf_design.hpp"
#include "version.hpp"

using namespace lintir;

namespace {

// ------------------------------------------------------------------- checks

void checkBlock(const Block& b, Report& rep) {
    const ProceduralBlockSymbol& p = *b.sym;
    bool ff = p.procedureKind == ProceduralBlockKind::AlwaysFF;

    // L002 / L003: the wrong kind of assignment for the block.
    if (ff)
        for (const Symbol* t : b.blocking) {
            if (b.locals.count(t)) continue;
            rep.add("L002", p.location,
                    "blocking assignment to '" + path(*t) +
                        "' in an always_ff; use <= so the result does not depend on evaluation "
                        "order (IEEE 1800-2023 10.4.2)");
        }
    if (p.procedureKind == ProceduralBlockKind::AlwaysComb)
        for (const Symbol* t : b.nba)
            rep.add("L003", p.location,
                    "non-blocking assignment to '" + path(*t) +
                        "' in an always_comb; use = (IEEE 1800-2023 10.4.1)");
}

void checkDrivers(const std::map<const Symbol*, std::vector<SourceLocation>>& drivers,
                  const std::set<const Symbol*>& wholeDriven, Report& rep) {
    for (auto& [sym, locs] : drivers) {
        if (locs.size() < 2) continue;
        if (!wholeDriven.count(sym)) continue;   // only part selects: each may own its bits
        rep.add("L004", locs[1],
                "'" + path(*sym) + "' is assigned by " + std::to_string(locs.size()) +
                    " processes; in simulation the last write wins and in synthesis this is a "
                    "multiple-driver error",
                path(*sym));
    }
}

// --------------------------------------------------------------------- VHDL
// VHDL is linted through its SystemVerilog translation (src/vhdl/sv.hpp).
// Findings in the translator's own runtime package, and rules that only see
// how the translation is written (=== for std_logic '=', a process as
// "always begin ... @(list); end", always rather than always_ff), say
// nothing about the VHDL.
inline bool vhdlNoise(const Finding& f) {
    if (f.file.empty() || f.file == "vhdl_design.sv" || f.file.front() == '<') return true;
    if (!vhdl::isVhdlFile(f.file)) return false;
    static const std::set<std::string> rules = {"STY003", "SYN003", "SYN010", "SEQ003", "TIDY006", "FSM003"};
    return rules.count(f.rule) > 0;
}

// ------------------------------------------------------------- UPF (1801)
// The design as UPF sees it is built by upf::designView (upf_design.hpp).
void checkUpf(Compilation& comp, const std::string& upfPath, std::string powerTop, bool powerTopSet, Report& rep) {
    upf::Model m;
    upf::DesignView dv = upf::designView(comp);
    upf::loadWithDesign(m, upfPath, comp, dv, powerTop, powerTopSet);
    m.addHdlAttrs(upfHdlAttrs(comp));   // (* UPF_... *) in the HDL (5.6)
    m.resolveApplies([&](const std::string& mod) {   // apply_power_model without -elements (6.9)
        std::vector<std::string> r;
        for (auto& [p, md] : dv.models) if (md == mod) r.push_back(p);
        return r;
    });
    for (auto& w : m.warnings) std::cerr << "mlint: UPF: " << w << "\n";
    for (auto& f : upf::check(m, dv)) {
        if (!rep.rules.enabled(f.rule)) continue;
        Finding o;
        o.rule = f.rule;
        o.text = f.text;
        o.col = 1;
        size_t c = f.where.rfind(':');
        o.file = c == std::string::npos ? f.where : f.where.substr(0, c);
        if (c != std::string::npos) o.line = (size_t)std::atoll(f.where.c_str() + c + 1);
        if (!rep.seen.insert({o.rule, f.where + ":" + o.text}).second) continue;
        rep.out.push_back(o);
    }
}

void usage() {
    std::cout << "usage: mlint [options] file.sv ...\n"
                 "  lint and clock-domain checks on the msim front end\n\n"
                 "  -list            list the rules: name, severity, on or off, group, summary\n"
                 "  -explain RULE    what a rule means and what to do about it\n"
                 "  -enable X        turn on a rule, a group (naming, width, synth, ...) or all (repeatable)\n"
                 "  -disable X       turn off a rule or a group (repeatable)\n"
                 "  -only X[,Y]      run these rules or groups and nothing else\n"
                 "  -goal NAME       run one methodology goal (lint_rtl, starc, ...); -list-goals\n"
                 "  -severity R=L    report rule or group R as error, warning or info\n"
                 "  -set NAME=VALUE  a setting, such as name.module='^[a-z][a-z0-9_]*$'\n"
                 "  -config FILE     directives, one per line: enable, disable, only, severity, set, waive\n"
                 "  -waivers FILE    the same format; by habit the file with the waive lines\n"
                 "  -show-waived     also print the waived findings, with the reason\n"
                 "  -json FILE       write the findings (waived ones too) as JSON\n"
                 "  -html FILE       write a self-contained report page: counts, filters, source around each finding\n"
                 "  -abstract-out F  write the top block's abstract (ports, open findings) for the SoC flow\n"
                 "  -abstract FILE   check the top against a block abstract, without the block's RTL\n"
                 "  -bbox PATTERN    treat matching modules as black boxes: nothing inside is reported\n"
                 "  -Werror          exit 1 on any warning (errors always do; info findings never do)\n"
                 "  -quiet           print findings only, no summary\n"
                 "  -stats           print where the time went, on stderr\n"
                 "  -threads N       worker threads, or all (default: every logical CPU); the output\n"
                 "                   does not depend on N\n"
              << cdc::usage() <<
                 "  -top M           the top module    -f <file>  an argument file\n"
                 "  -upf <file>      check IEEE 1801 (UPF) power intent against the design (UPF rules)\n"
                 "  -power_top <i>   the instance the UPF describes (default: set_design_top, else the top)\n"
                 "  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
              << protfront::Options::help() <<
                 "  -version         print the version\n"
                 "  --help-slang     list every front-end option\n\n"
                 "  In the source: // mlint: waive RULE reason   (this line, or the next if alone on its line)\n"
                 "                 // mlint: off RULE ... // mlint: on RULE\n";
}

void listRules(const lintcfg::Config& cfg) {
    auto row = [&](const std::string& id, const std::string& brief) {
        std::string g = lintcfg::groupOf(id);
        std::printf("%-8s %-8s %-4s %-8s %s\n", id.c_str(), lintcfg::sevName(cfg.severity(id)),
                    cfg.enabled(id) ? "on" : "off", g.c_str(), brief.c_str());
    };
    for (auto& r : lintcfg::table()) row(r.id, r.brief);
    for (auto& [r, t] : upf::rules()) row(r, t);
}

int explain(const std::string& id) {
    const lintcfg::RuleInfo* r = lintcfg::findRule(id);
    if (!r) {
        for (auto& [u, t] : upf::rules())
            if (u == id) {
                std::cout << id << ": " << t << "\n";
                return 0;
            }
        std::cerr << "mlint: no rule '" << id << "' (mlint -list)\n";
        return 2;
    }
    std::cout << r->id << " (" << r->group << ", " << lintcfg::sevName(r->sev) << ", " << (r->on ? "on" : "off")
              << " by default)\n  " << r->brief << "\n";
    if (*r->detail) std::cout << "  " << r->detail << "\n";
    return 0;
}

// A finding after the configuration and the waivers have had their say.
struct Final {
    Finding f;
    lintcfg::Sev sev;
    bool waived = false;
    std::string reason;
};

// Enabled rules only; then each finding's severity; then the waivers: the
// comments in the source first, the waiver files after.
std::vector<Final> resolve(std::vector<Finding>& found, const lintcfg::Config& cfg) {
    std::vector<Final> out;
    std::map<std::string, lintcfg::SourceMarks> marks;
    for (Finding& f : found) {
        if (!cfg.enabled(f.rule)) continue;
        Final x;
        x.sev = cfg.severity(f.rule);
        auto [it, fresh] = marks.try_emplace(f.file);
        if (fresh && !f.file.empty()) it->second.scan(f.file);
        x.waived = it->second.waived(f.rule, f.line, x.reason);
        if (!x.waived)
            for (const lintcfg::Waiver& w : cfg.waivers) {
                bool rule = false;
                for (auto& r : w.rules) rule |= lintcfg::Config::names(r, f.rule);
                if (!rule) continue;
                if (!w.file.empty() && !lintcfg::glob(w.file, f.file) && !lintcfg::glob("*/" + w.file, f.file)) continue;
                if (w.line0 && (f.line < w.line0 || f.line > w.line1)) continue;
                if (!w.msg.empty() && !lintcfg::glob(w.msg, f.text)) continue;
                w.hits++;
                x.waived = true;
                x.reason = w.reason.empty() ? w.origin : w.reason + " (" + w.origin + ")";
                break;
            }
        x.f = std::move(f);
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const Final& a, const Final& b) {
        return std::tie(a.f.file, a.f.line, a.f.col, a.f.rule, a.f.text) < std::tie(b.f.file, b.f.line, b.f.col, b.f.rule, b.f.text);
    });
    return out;
}

void printText(std::ostream& os, const std::vector<Final>& fs, const lintcfg::Config& cfg, bool quiet, bool showWaived) {
    std::map<std::string, int> per;
    size_t shown = 0, waived = 0;
    for (const Final& x : fs) {
        if (x.waived) {
            waived++;
            if (showWaived)
                os << x.f.file << ":" << x.f.line << ":" << x.f.col << ": waived: [" << x.f.rule << "] " << x.f.text
                   << " (" << x.reason << ")\n";
            continue;
        }
        os << x.f.file << ":" << x.f.line << ":" << x.f.col << ": " << lintcfg::sevName(x.sev) << ": [" << x.f.rule
           << "] " << x.f.text << "\n";
        per[x.f.rule]++;
        shown++;
    }
    if (quiet) return;
    os << "mlint: " << shown << (shown == 1 ? " finding" : " findings");
    bool first = true;
    for (auto& [r, n] : per) {
        os << (first ? " (" : ", ") << r << " " << n;
        first = false;
    }
    os << (first ? "" : ")");
    if (waived) os << ", " << waived << " waived";
    os << "\n";
    for (const lintcfg::Waiver& w : cfg.waivers)
        if (!w.hits) os << "mlint: " << w.origin << ": this waiver matched nothing; remove it\n";
}

// The report's schematic: for each signal a finding names, the processes and
// assigns that drive it (with what they read) and those that read it (with what
// they drive). JSON: {"path": {"d": [node...], "r": [node...]}}, a node being
// {"k": "flop"|"logic"|"assign", "at": "file:line", "clk": name, "io": [[name, path]...]}.
std::string schematicJson(const Design& d, const SourceManager& sm, const std::set<std::string>& sigs) {
    using lintcfg::jsonStr;
    if (sigs.empty()) return "{}";
    std::unordered_map<const Symbol*, std::string> pathOf;
    auto P = [&](const Symbol* s) -> const std::string& {
        auto it = pathOf.find(s);
        if (it != pathOf.end()) return it->second;
        return pathOf[s] = path(*s);
    };
    struct Node {
        const char* k;
        SourceLocation loc;
        const Symbol* clk;
        const std::set<const Symbol*>* outs;
        const std::set<const Symbol*>* ins;
        std::set<const Symbol*> own;   // targets of a block, for blocks
    };
    std::vector<Node> nodes;
    nodes.reserve(d.blocks.size() + d.assigns.size());   // outs points into own
    for (const Block& b : d.blocks) {
        Node n{b.seq ? "flop" : "logic", b.sym->location, b.clk, nullptr, &b.reads, {}};
        for (const Symbol* t : b.nba) if (!b.locals.count(t)) n.own.insert(t);
        for (const Symbol* t : b.blocking) if (!b.locals.count(t)) n.own.insert(t);
        nodes.push_back(std::move(n));
    }
    for (Node& n : nodes) n.outs = &n.own;
    for (const Assign& a : d.assigns) nodes.push_back({"assign", a.loc, nullptr, &a.targets, &a.reads, {}});
    std::map<std::string, std::pair<std::vector<size_t>, std::vector<size_t>>> by;   // path -> drivers, readers
    // Two rounds: the signals the findings name, then their neighbours, so a
    // click in the schematic can walk one step further.
    std::set<std::string> want = sigs;
    for (int round = 0; round < 2; round++) {
    by.clear();
    for (size_t i = 0; i < nodes.size(); i++) {
        for (const Symbol* t : *nodes[i].outs)
            if (want.count(P(t))) {
                auto& v = by[P(t)].first;
                if (v.size() < 6) v.push_back(i);
            }
        for (const Symbol* r : *nodes[i].ins)
            if (want.count(P(r)) && !nodes[i].outs->count(r)) {
                auto& v = by[P(r)].second;
                if (v.size() < 8) v.push_back(i);
            }
        // a flop reads its clock too
        if (const Symbol* c = nodes[i].clk; c && want.count(P(c)) && !nodes[i].ins->count(c)) {
            auto& v = by[P(c)].second;
            if (v.size() < 8) v.push_back(i);
        }
    }
    if (round == 1) break;
    for (auto& [pth, dr] : by)
        for (auto* list : {&dr.first, &dr.second})
            for (size_t i : *list)
                for (const auto* set : {nodes[i].outs, nodes[i].ins})
                    for (const Symbol* x : *set)
                        if (want.size() < sigs.size() + 4000 && lintrules::isSignal(*x)) want.insert(P(x));
    }
    auto names = [&](const std::set<const Symbol*>& set, const Symbol* skip, size_t max) {
        std::vector<std::pair<std::string, std::string>> v;
        for (const Symbol* x : set)
            if (x != skip && lintrules::isSignal(*x)) v.push_back({std::string(x->name), P(x)});
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        std::string o = "[";
        for (size_t i = 0; i < v.size() && i < max; i++)
            o += (i ? "," : "") + std::string("[") + jsonStr(v[i].first) + "," + jsonStr(v[i].second) + "]";
        if (v.size() > max) o += ",[" + jsonStr("+" + std::to_string(v.size() - max) + " more") + ",\"\"]";
        return o + "]";
    };
    auto node = [&](const Node& n, bool driver, const Symbol* self) {
        SourceLocation full = sm.getFullyOriginalLoc(n.loc);
        std::string at = std::string(sm.getFileName(full)) + ":" + std::to_string(sm.getLineNumber(full));
        return "{\"k\":" + jsonStr(n.k) + ",\"at\":" + jsonStr(at) +
               ",\"clk\":" + (n.clk ? jsonStr(std::string(n.clk->name)) : std::string("null")) +
               ",\"io\":" + names(driver ? *n.ins : *n.outs, self, driver ? 8 : 4) + "}";
    };
    std::string o = "{";
    bool first = true;
    for (auto& [pth, dr] : by) {
        // the symbol itself, to leave it out of its own lists
        const Symbol* self = nullptr;
        for (auto& [sym, p] : pathOf)
            if (p == pth) { self = sym; break; }
        std::string port;
        if (self)
            if (const Scope* sc = self->getParentScope(); sc && sc->asSymbol().kind == SymbolKind::InstanceBody)
                for (const Symbol* ps : sc->asSymbol().as<InstanceBodySymbol>().getPortList())
                    if (ps && ps->kind == SymbolKind::Port && ps->as<PortSymbol>().internalSymbol == self)
                        port = ps->as<PortSymbol>().direction == ArgumentDirection::In    ? "in"
                               : ps->as<PortSymbol>().direction == ArgumentDirection::Out ? "out"
                                                                                          : "inout";
        o += (first ? "" : ",") + jsonStr(pth) + ":{\"port\":" + jsonStr(port) + ",\"d\":[";
        first = false;
        for (size_t i = 0; i < dr.first.size(); i++) o += (i ? "," : "") + node(nodes[dr.first[i]], true, self);
        o += "],\"r\":[";
        for (size_t i = 0; i < dr.second.size(); i++) o += (i ? "," : "") + node(nodes[dr.second[i]], false, self);
        o += "]}";
    }
    return o + "}";
}

// -html: one self-contained page (no server, no network): counts by severity
// and rule, every finding with the source lines around it, a filter box, and
// clicks on a rule or a severity to show only those.
bool writeHtml(const std::string& path, const std::vector<Final>& fs, const std::string& net) {
    std::ofstream os(path);
    if (!os) return false;
    using lintcfg::jsonStr;
    // "</script>" must not appear inside the data.
    auto js = [&](std::string_view t) {
        std::string q = jsonStr(t), o;
        for (size_t i = 0; i < q.size(); i++) {
            if (q[i] == '<' && i + 1 < q.size() && q[i + 1] == '/') o += "<\\";
            else o += q[i];
        }
        return o;
    };
    std::map<std::string, std::vector<std::string>> files;
    auto lines = [&](const std::string& f) -> const std::vector<std::string>& {
        auto it = files.find(f);
        if (it != files.end()) return it->second;
        std::vector<std::string> v;
        std::ifstream in(f);
        for (std::string l; std::getline(in, l);) v.push_back(l);
        return files[f] = std::move(v);
    };
    os << R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>mlint report</title>
<style>
body{font:14px system-ui,sans-serif;margin:0;color:#1d1d1f;background:#fafafa}
header{padding:16px 24px;background:#fff;border-bottom:1px solid #ddd}
h1{font-size:18px;margin:0 0 8px}
.tiles{display:flex;gap:12px;flex-wrap:wrap}
.tile{padding:8px 14px;border:1px solid #ddd;border-radius:8px;background:#fff;cursor:pointer}
.tile b{font-size:20px;display:block}
.tile.on{outline:2px solid #3366cc}
main{display:grid;grid-template-columns:260px minmax(0,1fr);gap:16px;padding:16px 24px}
table{border-collapse:collapse;width:100%;background:#fff}
td,th{padding:4px 8px;border-bottom:1px solid #eee;text-align:left;vertical-align:top}
tr.rule{cursor:pointer}tr.rule.on{background:#e8eefb}
.f{border:1px solid #e3e3e3;border-radius:8px;background:#fff;margin-bottom:10px}
.f .h{padding:6px 10px;display:flex;gap:10px;align-items:baseline;flex-wrap:wrap}
.sev{font-weight:600;text-transform:uppercase;font-size:11px;padding:1px 6px;border-radius:4px}
.error{background:#fde2e1;color:#a4161a}.warning{background:#fff1d6;color:#8a5300}.info{background:#e3effd;color:#1b4f9c}
.waived{background:#eee;color:#666}
.loc{font-family:ui-monospace,monospace;color:#555}
pre{margin:0;padding:6px 10px;background:#f6f6f6;border-top:1px solid #eee;overflow:auto;font-size:12px}
pre .hl{background:#fff3b0;display:block}
#q{padding:6px 10px;width:320px;border:1px solid #ccc;border-radius:6px}
.msg{flex-basis:100%}
.reason{color:#666;font-style:italic}
.sch{border-top:1px solid #eee;padding:8px 10px;overflow:auto}
.sch svg text{font:12px ui-monospace,monospace}
.sch a{cursor:pointer;fill:#3366cc}
.btn{font-size:12px;color:#3366cc;cursor:pointer;border:1px solid #ccd;border-radius:4px;padding:0 6px}
</style></head><body>
<header><h1>mlint report</h1><div class="tiles" id="tiles"></div>
<p><input id="q" placeholder="filter: text, file, rule"> <label><input type="checkbox" id="sw"> show waived</label></p></header>
<main><div><table id="rules"><thead><tr><th>rule</th><th>group</th><th>n</th></tr></thead><tbody></tbody></table></div>
<div id="list"></div></main>
<script>
const F=[)HTML";
    for (size_t i = 0; i < fs.size(); i++) {
        const Final& x = fs[i];
        const auto& L = lines(x.f.file);
        size_t a = x.f.line > 2 ? x.f.line - 2 : 1, z = std::min(L.size(), x.f.line + 2);
        std::string src;
        for (size_t n = a; n <= z && n >= 1 && n <= L.size(); n++)
            src += std::to_string(n) + (n == x.f.line ? "*" : " ") + L[n - 1] + "\n";
        os << (i ? ",\n" : "\n") << "{f:" << js(x.f.file) << ",l:" << x.f.line << ",c:" << x.f.col
           << ",r:" << js(x.f.rule) << ",g:" << js(lintcfg::groupOf(x.f.rule)) << ",s:" << js(lintcfg::sevName(x.sev))
           << ",t:" << js(x.f.text) << ",w:" << (x.waived ? "1" : "0") << ",why:" << js(x.reason) << ",src:" << js(src)
           << ",sig:" << js(x.f.signal) << "}";
    }
    std::string netSafe;
    for (size_t i = 0; i < net.size(); i++) {
        if (net[i] == '<' && i + 1 < net.size() && net[i + 1] == '/') netSafe += "<\\";
        else netSafe += net[i];
    }
    os << "];\nconst N=" << netSafe << R"HTML(;
let sev=null,rule=null;
const esc=s=>s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
function draw(){
  const q=document.getElementById('q').value.toLowerCase(),sw=document.getElementById('sw').checked;
  const vis=F.filter(x=>(sw||!x.w)&&(!sev||(x.w?'waived':x.s)==sev)&&(!rule||x.r==rule)&&
    (!q||(x.t+' '+x.f+' '+x.r).toLowerCase().includes(q)));
  const cnt={error:0,warning:0,info:0,waived:0};F.forEach(x=>cnt[x.w?'waived':x.s]++);
  document.getElementById('tiles').innerHTML=Object.keys(cnt).map(k=>
    `<div class="tile ${sev==k?'on':''}" onclick="sev=sev=='${k}'?null:'${k}';draw()"><b>${cnt[k]}</b><span class="sev ${k}">${k}</span></div>`).join('');
  const per={};F.filter(x=>sw||!x.w).forEach(x=>{per[x.r]=per[x.r]||{g:x.g,n:0};per[x.r].n++});
  document.querySelector('#rules tbody').innerHTML=Object.keys(per).sort().map(r=>
    `<tr class="rule ${rule==r?'on':''}" onclick="rule=rule=='${r}'?null:'${r}';draw()"><td>${r}</td><td>${per[r].g}</td><td>${per[r].n}</td></tr>`).join('');
  document.getElementById('list').innerHTML=(vis.length?'':'<p>No findings.</p>')+vis.map(x=>{
    const src=x.src?'<pre>'+x.src.split('\n').filter(l=>l).map(l=>{const hl=/^\d+\*/.test(l);
      return hl?'<span class="hl">'+esc(l.replace('*',' '))+'</span>':esc(l)+'\n'}).join('')+'</pre>':'';
    const sb=x.sig&&N[x.sig]?`<span class="btn" onclick="sch(this,'${x.sig.replace(/['\\]/g,'\\$&')}')">schematic</span>`:'';
    return `<div class="f"><div class="h"><span class="sev ${x.w?'waived':x.s}">${x.w?'waived':x.s}</span><b>${x.r}</b>
      <span class="loc">${esc(x.f)}:${x.l}:${x.c}</span>${sb}<span class="msg">${esc(x.t)}</span>${x.w?'<span class="reason">'+esc(x.why)+'</span>':''}</div>${src}</div>`}).join('');
}
// The schematic of one signal: what drives it (and from what) on the left, what
// reads it (and what that drives) on the right. Clicking a name that has its
// own schematic moves to it.
function sch(btn,sig){
  const card=btn.closest('.f');let box=card.querySelector('.sch');
  if(!box){box=document.createElement('div');box.className='sch';card.append(box)}
  const n=N[sig];if(!n){box.textContent='no drivers or readers found for '+sig;return}
  const NS='http://www.w3.org/2000/svg',W=200,BH=18,GAP=14,X1=190,XS=X1+W+50,X2=XS+190,XE=X2+W+210;
  const hOf=nd=>Math.max(1,nd.io.length)*BH+30;
  const col=(arr)=>{let y=10;return arr.map(nd=>{const h=hOf(nd),r={nd,y,h};y+=h+GAP;return r})};
  const D=col(n.d),R=col(n.r);
  const H=Math.max(80,...D.map(r=>r.y+r.h),...R.map(r=>r.y+r.h))+10,mid=H/2;
  const s=document.createElementNS(NS,'svg');s.setAttribute('width',XE);s.setAttribute('height',H);
  const add=(t,a,txt)=>{const e=document.createElementNS(NS,t);for(const k in a)e.setAttribute(k,a[k]);if(txt!=null)e.textContent=txt;s.append(e);return e};
  const short=t=>t.length>28?'…'+t.slice(-27):t;
  const label=(x,y,pair,end)=>{const [nm,p]=pair;const e=add('text',end?{x,y,'text-anchor':'end'}:{x,y},short(nm));
    if(p&&N[p]){e.setAttribute('class','');e.style.fill='#3366cc';e.style.cursor='pointer';e.onclick=()=>sch(btn,p)}};
  const block=(r,x,driver)=>{const nd=r.nd;
    add('rect',{x,y:r.y,width:W,height:r.h,rx:nd.k=='flop'?2:nd.k=='assign'?0:12,fill:'#fff',stroke:nd.k=='flop'?'#8a5300':'#555','stroke-width':nd.k=='flop'?2:1});
    add('text',{x:x+8,y:r.y+15,'font-weight':'bold'},(nd.k=='flop'?'flop'+(nd.clk?' @ '+nd.clk:''):nd.k));
    add('text',{x:x+W-8,y:r.y+15,'text-anchor':'end','fill-opacity':.6},short(nd.at.split('/').pop()));
    nd.io.forEach((p,i)=>{const ty=r.y+32+i*BH;
      if(driver){add('line',{x1:x-34,y1:ty-4,x2:x,y2:ty-4,stroke:'#999'});label(x-38,ty,p,true)}
      else{add('line',{x1:x+W,y1:ty-4,x2:x+W+34,y2:ty-4,stroke:'#999'});label(x+W+38,ty,p)}});
    if(driver&&!nd.io.length)add('text',{x:x+8,y:r.y+32,'fill-opacity':.6},'(constant)');};
  D.forEach(r=>{block(r,X1,true);add('path',{d:`M${X1+W} ${r.y+r.h/2} C ${XS-20} ${r.y+r.h/2}, ${XS-20} ${mid}, ${XS} ${mid}`,fill:'none',stroke:'#333'})});
  R.forEach(r=>{block(r,X2,false);add('path',{d:`M${XS+140} ${mid} C ${X2-20} ${mid}, ${X2-20} ${r.y+r.h/2}, ${X2} ${r.y+r.h/2}`,fill:'none',stroke:'#333'})});
  add('rect',{x:XS,y:mid-14,width:140,height:28,rx:14,fill:'#fff3b0',stroke:'#333'});
  add('text',{x:XS+70,y:mid+4,'text-anchor':'middle','font-weight':'bold'},short(sig.split('.').pop()));
  if(!D.length)add('text',{x:X1,y:mid+4,fill:'#a4161a'},n.port=='in'?'input port':'no driver');
  if(!R.length)add('text',{x:X2,y:mid+4,fill:n.port=='out'?'#555':'#a4161a'},n.port=='out'?'output port':'no reader');
  if(D.length&&n.port=='in')add('text',{x:X1,y:10,fill:'#555'},'(also an input port)');
  const head=document.createElement('div');head.className='loc';head.textContent=sig;
  box.replaceChildren(head,s);
}
document.getElementById('q').oninput=draw;document.getElementById('sw').onchange=draw;draw();
</script></body></html>
)HTML";
    return bool(os);
}

bool writeJson(const std::string& path, const std::vector<Final>& fs) {
    std::ofstream os(path);
    if (!os) return false;
    using lintcfg::jsonStr;
    os << "{\n  \"tool\": \"mlint\",\n  \"version\": " << jsonStr(MINISIM_VERSION) << ",\n  \"findings\": [";
    bool first = true;
    std::map<std::string, int> counts;
    for (const Final& x : fs) {
        os << (first ? "\n" : ",\n") << "    {\"file\": " << jsonStr(x.f.file) << ", \"line\": " << x.f.line
           << ", \"col\": " << x.f.col << ", \"rule\": " << jsonStr(x.f.rule) << ", \"group\": "
           << jsonStr(lintcfg::groupOf(x.f.rule)) << ", \"severity\": " << jsonStr(lintcfg::sevName(x.sev))
           << ", \"text\": " << jsonStr(x.f.text) << ", \"waived\": " << (x.waived ? "true" : "false");
        if (!x.f.signal.empty()) os << ", \"signal\": " << jsonStr(x.f.signal);
        if (x.waived) os << ", \"reason\": " << jsonStr(x.reason);
        os << "}";
        first = false;
        counts[x.waived ? "waived" : lintcfg::sevName(x.sev)]++;
    }
    os << (first ? "" : "\n  ") << "],\n  \"summary\": {\"error\": " << counts["error"] << ", \"warning\": "
       << counts["warning"] << ", \"info\": " << counts["info"] << ", \"waived\": " << counts["waived"] << "}\n}\n";
    return bool(os);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> fwd{argv[0]};
    lintcfg::Config cfg;
    bool werror = false, quiet = false, powerTopSet = false, showWaived = false, list = false, stats = false;
    std::string upfPath, powerTop, jsonPath, explainRule;
    std::vector<std::string> bboxPats, abstractIn;  // the SoC flow (lintsoc.hpp)
    std::vector<std::string> vhdlFiles;            // VHDL sources (src/vhdl/)
    std::string abstractOut, htmlPath;
    unsigned threads = par::defaultThreads();
    FrontEnd fe;
    cdc::Options cdcOpt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mlint: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        // NAME=VALUE, split at the first '='.
        auto pair = [&](const std::string& v, const char* what) -> std::pair<std::string, std::string> {
            size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::cerr << "mlint: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return {v.substr(0, eq), v.substr(eq + 1)};
        };
        const std::string origin = "command line";
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-list" || a == "--list") { list = true; continue; }
        if (a == "-goal") { cfg.directive({"goal", next("a goal name")}, origin); continue; }
        if (a == "-list-goals") {
            for (auto& g : lintcfg::goals()) std::cout << std::left << std::setw(12) << g.name << g.what << "\n";
            return 0;
        }
        if (a == "-explain") { explainRule = next("a rule name"); continue; }
        if (a == "-version" || a == "--version" || a == "-v") {
            std::cout << "mlint " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-disable") { cfg.directive({"disable", next("a rule or a group")}, origin); continue; }
        if (a == "-enable") { cfg.directive({"enable", next("a rule or a group")}, origin); continue; }
        if (a == "-only") { cfg.directive({"only", next("rules or groups")}, origin); continue; }
        if (a == "-severity") {
            auto [r, l] = pair(next("RULE=LEVEL"), "RULE=LEVEL");
            cfg.directive({"severity", r, l}, origin);
            continue;
        }
        if (a == "-set") {
            auto [k, v] = pair(next("NAME=VALUE"), "NAME=VALUE");
            cfg.directive({"set", k, v}, origin);
            continue;
        }
        if (a == "-config" || a == "-waivers" || a == "-waiver") { cfg.load(next("a file")); continue; }
        if (a == "-show-waived") { showWaived = true; continue; }
        if (a == "-stats") { stats = true; continue; }
        if (a == "-json") { jsonPath = next("a file"); continue; }
        if (a == "-html") { htmlPath = next("a file"); continue; }
        if (a == "-bbox") { bboxPats.push_back(next("a module name pattern")); continue; }
        if (a == "-abstract") { abstractIn.push_back(next("a block abstract")); continue; }
        if (a == "-abstract-out") { abstractOut = next("a file"); continue; }
        if (a == "-threads") {
            std::string n = next("a count");
            if (n == "all") { threads = par::defaultThreads(); continue; }
            int v = std::atoi(n.c_str());
            if (v < 1) { std::cerr << "mlint: -threads needs a count of 1 or more, or all\n"; return 2; }
            threads = (unsigned)v;
            fwd.push_back("--threads");
            fwd.push_back(n);
            continue;
        }
        if (a == "-Werror") { werror = true; continue; }
        if (cdcOpt.take(a, next)) continue;
        if (a == "-upf") { upfPath = next("a UPF file"); continue; }
        if (a == "-power_top") { powerTop = next("an instance path"); powerTopSet = true; continue; }
        if (a == "-quiet" || a == "-q") { quiet = true; continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        if (fe.prot.take(argc, argv, i)) continue;
        if (a == "-v93" || a == "-v2008" || a == "-vhdl2008" || a == "-v200x" || a == "-vhdl93" || a == "-vhdl87") continue;
        if (a.size() && a[0] != '-' && vhdl::isVhdlFile(a)) { vhdlFiles.push_back(a); continue; }
        fwd.push_back(a);
    }
    lintrules::Params params = lintrules::Params::from(cfg, cfg.errors);
#ifdef MINISIM_TIDY
    linttidy::configure(cfg, cfg.errors);
#endif
    if (!cfg.errors.empty()) {
        for (auto& e : cfg.errors) std::cerr << "mlint: " << e << "\n";
        return 2;
    }
    if (list) { listRules(cfg); return 0; }
    if (!explainRule.empty()) return explain(explainRule);

    // What is off is not computed at all.
    Rules rules;
    for (auto& r : lintcfg::table())
        if (!cfg.enabled(r.id)) rules.disabled.insert(r.id);
    for (auto& [r, t] : upf::rules())
        if (!cfg.enabled(r)) rules.disabled.insert(r);

    // With -upf the testbench may import package UPF (IEEE 1801-2024 11.2.4).
    std::string pkgFile;
    if (!upfPath.empty()) {
        char tmpl[] = "/tmp/mlint_upf_pkgXXXXXX.sv";
        int fd = mkstemps(tmpl, 3);
        std::string pkg = upf::svPackage();
        if (fd < 0 || write(fd, pkg.data(), pkg.size()) != (ssize_t)pkg.size()) {
            std::cerr << "mlint: cannot write the UPF package\n";
            return 2;
        }
        close(fd);
        pkgFile = tmpl;
        fwd.insert(fwd.begin() + 1, pkgFile);
    }
    struct RmPkg { std::string f; ~RmPkg() { if (!f.empty()) std::remove(f.c_str()); } } rmPkg{pkgFile};

    // Block abstracts: the blocks become empty modules the top elaborates against.
    std::vector<lintsoc::BlockFact> blocks;
    std::string stubFile;
    for (auto& f : abstractIn) {
        std::string err;
        if (!lintsoc::readAbstract(f, blocks, err)) {
            std::cerr << "mlint: " << err << "\n";
            return 2;
        }
    }
    if (!blocks.empty()) {
        char tmpl[] = "/tmp/mlint_abstractXXXXXX.sv";
        int fd = mkstemps(tmpl, 3);
        std::string text = lintsoc::stubs(blocks);
        if (fd < 0 || write(fd, text.data(), text.size()) != (ssize_t)text.size()) {
            std::cerr << "mlint: cannot write the block stubs\n";
            return 2;
        }
        close(fd);
        stubFile = tmpl;
        fwd.insert(fwd.begin() + 1, stubFile);
    }
    RmPkg rmStub{stubFile};

    // VHDL (src/vhdl/, the front end msim uses): translated to SystemVerilog
    // with `line directives, so findings point at the VHDL source. slang wants
    // at least one SystemVerilog file; an empty one stands in.
    std::string vhdlStub;
    if (!vhdlFiles.empty()) {
        char tmpl[] = "/tmp/mlint_vhdlXXXXXX.sv";
        int fd = mkstemps(tmpl, 3);
        if (fd < 0) {
            std::cerr << "mlint: cannot write a temporary file\n";
            return 2;
        }
        close(fd);
        vhdlStub = tmpl;
        fwd.push_back(vhdlStub);
        fe.afterParse = [&](driver::Driver& drv) {
            vhdl::Translator tr;
            tr.firstRunWait = false;
            return vhdl::addVhdl(drv, vhdlFiles, "", "mlint", tr);
        };
    }
    RmPkg rmVhdl{vhdlStub};

    // Where the time goes, for -stats.
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    std::vector<std::pair<const char*, double>> phases;
    auto lap = [&](const char* what) {
        auto t1 = Clock::now();
        phases.push_back({what, std::chrono::duration<double>(t1 - t0).count()});
        t0 = t1;
    };

    // Pass 1: the front end, with its own warnings and errors.
    if (int rc = fe.run(fwd, "mlint front end")) return rc < 0 ? 0 : rc;

    // Pass 2: our rules. Two walks of the whole hierarchy on this thread (they
    // elaborate what the front end left for later, which must not race), then
    // everything else in parallel: each task fills its own report, and the
    // reports are merged in task order.
    lap("parse and elaborate");
    Report rep;
    rep.sm = fe.comp->getSourceManager();
    rep.rules = rules;
    if (fe.pc.any()) { rep.pc = &fe.pc; namePC() = &fe.pc; }
    Design d;
    d.gather(*fe.comp, rep);
    lap("design model");
    // L001 from the front end's data-flow analysis, which follows single
    // bits: z[i] assigned for every i in a loop is assigned, w[3:2] is not w.
    // slang-tidy's checks read the same analysis.
    std::unique_ptr<analysis::AnalysisManager> am;
    bool tidy = false;
#ifdef MINISIM_TIDY
    tidy = linttidy::anyEnabled(rep.rules);
#endif
    if (rep.rules.enabled("L001") || tidy) {
        fe.comp->freeze();
        am = std::make_unique<analysis::AnalysisManager>(analysis::AnalysisOptions{}, std::make_shared<ThreadPool>(threads));
        am->analyze(*fe.comp);
        for (auto& dg : am->getDiagnostics()) {
            if (dg.code != diag::InferredLatch || dg.args.empty()) continue;
            const std::string* name = std::get_if<std::string>(&dg.args[0]);
            if (!name) continue;
            rep.add("L001", dg.location,
                    "'" + *name +
                        "' is not assigned on every path of this always_comb, so it holds its old value "
                        "(an unintended latch); add an else or a default");
        }
        fe.comp->unfreeze();
    }
    lap("data-flow analysis (L001)");
    lintrules::Global global(params);
    fe.comp->getRoot().visit(global);
    lintrules::BodyCollector bodies;
    fe.comp->getRoot().visit(bodies);
    // State machines, from the extractor FSM coverage uses (one more walk).
    std::vector<ExtractedFsm> fsms;
    if (rep.rules.enabled("FSM001") || rep.rules.enabled("FSM002") || rep.rules.enabled("FSM003") ||
        rep.rules.enabled("FSM004"))
        fsms = extractFsms(fe.comp->getRoot());
    lap("hierarchy walks");

    bool upfFailed = false;
    const size_t fixed = 4, nb = bodies.out.size();
    std::vector<Report> reps(fixed + nb, rep);
    std::vector<lintrules::BodyOut> outs(nb);
    par::parallelFor(fixed + nb, threads, [&](size_t i) {
        Report& r = reps[i];
        if (i == 0) {
            for (const Block& b : d.blocks) checkBlock(b, r);
            checkDrivers(d.drivers, d.wholeDriven, r);
            for (auto& [sig, n] : d.contention)
                r.add("L005", sig->location,
                      "'" + path(*sig) + "' is a resolved signal driven by " + std::to_string(n) +
                          " processes, and not every one releases it with 'Z': the values fight (X in simulation, contention in hardware)",
                      path(*sig));
        } else if (i == 1) {
            global.analyze(r);
        } else if (i == 2) {
            lintrules::checkDefinitions(*fe.comp, params, r);
            lintrules::checkFsms(fsms, r);
        } else if (i == 3) {
            if (upfPath.empty()) return;
            try {
                checkUpf(*fe.comp, upfPath, powerTop, powerTopSet, r);
            } catch (const std::exception& ex) {
                std::cerr << "mlint: UPF: " << ex.what() << "\n";
                upfFailed = true;
            }
        } else {
            lintrules::BodyRules br(*bodies.out[i - fixed], r, params, outs[i - fixed]);
            br.run();
        }
    });
    if (upfFailed) return 1;
    lintrules::checkUnused(outs, rep);
    // Findings in protected code are counted, never shown (IEEE 1735).
    for (Report& r : reps) rep.hiddenSeen.insert(r.hiddenSeen.begin(), r.hiddenSeen.end());
    size_t hidden = rep.hiddenSeen.size();
    for (Report& r : reps)
        for (Finding& f : r.out)
            if (rep.seen.insert({f.rule, f.file + ":" + std::to_string(f.line) + ":" + std::to_string(f.col) + ":" + f.text}).second)
                rep.out.push_back(std::move(f));

    lap("rules");
    // Clock and reset domain crossings (cdc.hpp): one more walk of the
    // hierarchy, then its own worker threads.
    bool crossings = cdcOpt.report;
    for (auto& r : lintcfg::table())
        crossings |= (!std::strcmp(r.group, "cdc") || !std::strcmp(r.group, "rdc")) && rep.rules.enabled(r.id);
    if (crossings) {
        cdcOpt.threads = (int)threads;
        cdc::check(*fe.comp, rep, cdcOpt);
    }
    lap("clock and reset crossings");
    // Formal-assisted checks (lintformal.hpp).
    {
        lintformal::Options fo;
        fo.maxBits = (unsigned)std::clamp(std::atoi(cfg.param("formal.max_bits", "16").c_str()), 1, 30);
        fo.maxRegBits = (unsigned)std::clamp(std::atoi(cfg.param("formal.reg_bits", "8").c_str()), 1, 12);
        lintformal::run(*fe.comp, rep, fo, threads);
    }
    lap("formal-assisted checks");
    // The source text and the syntax trees (lintsrc.hpp).
    // The translated VHDL is not the user's text: only the semantic rules see it.
    lintsrc::run(*fe.comp, rep, params, threads,
                 [&](std::string_view f) { return (!pkgFile.empty() && f == pkgFile) || f == "vhdl_design.sv"; });
    lap("text and syntax rules");
    // What the VHDL translation erases: the VHDL text itself (lintvhdl.hpp).
    if (!vhdlFiles.empty()) lintvhdl::run(vhdlFiles, rep, threads);
#ifdef MINISIM_TIDY
    if (tidy) linttidy::run(*fe.comp, *am, fe.driver.diagEngine, rep, threads);
    lap("slang-tidy checks");
#endif
    // The SoC flow: connections to abstracted blocks; nothing from inside a black box.
    if (!blocks.empty()) lintsoc::checkTop(*fe.comp, blocks, rep);
    {
        lintsoc::Boxes boxes;
        boxes.add(*fe.comp, bboxPats);
        // The stub file may be named relative to the working directory.
        std::erase_if(rep.out, [&](const Finding& f) {
            if (!vhdlFiles.empty() && vhdlNoise(f)) return true;
            return (!stubFile.empty() && f.file.ends_with(stubFile.substr(stubFile.rfind('/') + 1))) || boxes.inside(f);
        });
    }
    std::vector<Final> fs = resolve(rep.out, cfg);
    if (!abstractOut.empty()) {
        std::vector<lintsoc::BlockFact> out;
        for (auto* top : fe.comp->getRoot().topInstances) {
            lintsoc::BlockFact b = lintsoc::facts(*top);
            for (const Final& x : fs) {
                if (x.waived) continue;
                b.findings[x.f.rule]++;
                b.errors += x.sev == lintcfg::Sev::Error;
                b.warnings += x.sev == lintcfg::Sev::Warning;
            }
            out.push_back(std::move(b));
        }
        if (!lintsoc::writeAbstract(abstractOut, out)) {
            std::cerr << "mlint: cannot write '" << abstractOut << "'\n";
            return 2;
        }
    }
    printText(std::cout, fs, cfg, quiet, showWaived);
    if (hidden)
        std::cout << "mlint: " << hidden << (hidden == 1 ? " finding" : " findings")
                  << " in protected code (details hidden)\n";
    std::string net = "{}";
    if (!htmlPath.empty()) {
        std::set<std::string> sigs;
        for (const Final& x : fs)
            if (!x.f.signal.empty()) sigs.insert(x.f.signal);
        net = schematicJson(d, *fe.comp->getSourceManager(), sigs);
    }
    if (!htmlPath.empty() && !writeHtml(htmlPath, fs, net)) {
        std::cerr << "mlint: cannot write '" << htmlPath << "'\n";
        return 2;
    }
    if (!jsonPath.empty() && !writeJson(jsonPath, fs)) {
        std::cerr << "mlint: cannot write '" << jsonPath << "'\n";
        return 2;
    }
    lap("waivers and report");
    if (stats) {
        double total = 0;
        for (auto& [w, t] : phases) total += t;
        std::fprintf(stderr, "mlint: %zu module bodies, %zu processes, %u threads, %.3f s\n", nb, d.blocks.size(), threads, total);
        for (auto& [w, t] : phases) std::fprintf(stderr, "  %-28s %8.3f s\n", w, t);
    }
    bool errors = false, warnings = false;
    for (const Final& x : fs) {
        if (x.waived) continue;
        errors |= x.sev == lintcfg::Sev::Error;
        warnings |= x.sev == lintcfg::Sev::Warning;
    }
    return errors || (werror && warnings) ? 1 : 0;
}
