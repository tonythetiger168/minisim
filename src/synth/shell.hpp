// synth/shell.hpp - the dc_shell-style synthesis shell behind msynth, as a
// header so another tool can build on it (the FPGA flow adds its own
// commands to the same shell instead of keeping a second one).
//
//   synth::Shell sh;                       // every msynth command registered
//   sh.cmd("report_luts", "report_luts: ...", [&sh](synth::Shell::Words& w) {
//       sh.needNetlist();                   // compiles on demand
//       const synth::Netlist& nl = *sh.nl;
//       ...
//       return std::string();
//   });
//   sh.in.load("run.tcl");                 // or sh.in.parse / sh.in.shell
//
// The interpreter is the shared SDC reader (src/sdc.hpp, on src/tcl.hpp).
// Include this from one translation unit per tool.
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>

#include "slang/driver/Driver.h"

#include "aig_elab.hpp"
#include "gnetlist.hpp"
#include "synth/map.hpp"
#include "synth/sta.hpp"
#include "synth/abc.hpp"
#ifdef MSYNTH_HAVE_SAT
#include "synth/fraig.hpp"
#endif
#include "sdc.hpp"
#include "tcl.hpp"
#include "version.hpp"


// The source tree to look for lib/mini45 in when nothing is installed; the
// build defines it for msynth.
#ifndef MSYNTH_ROOT_DIR
#define MSYNTH_ROOT_DIR ""
#endif

namespace synth {


inline bool glob(const char* p, const char* t) {
    if (!*p) return !*t;
    if (*p == '*') return glob(p + 1, t) || (*t && glob(p, t + 1));
    if (*t && (*p == '?' || *p == *t)) return glob(p + 1, t + 1);
    return false;
}

struct Args {
    std::map<std::string, std::string> opt;
    std::vector<std::string> pos;
    bool has(const std::string& o) const { return opt.count(o) != 0; }
    std::string get(const std::string& o, const std::string& d = "") const {
        auto it = opt.find(o);
        return it == opt.end() ? d : it->second;
    }
};
// Options (words starting with '-' and a letter) take a value when listed in `valued`.
inline Args parseArgs(const std::vector<std::string>& w, std::initializer_list<const char*> valued) {
    Args a;
    for (size_t i = 1; i < w.size(); i++) {
        const std::string& s = w[i];
        if (s.size() > 1 && s[0] == '-' && (isalpha((unsigned char)s[1]) || s[1] == '_')) {
            bool val = false;
            for (const char* v : valued)
                if (s == v) val = true;
            if (val && i + 1 < w.size()) a.opt[s] = w[++i];
            else a.opt[s] = "";
        } else
            a.pos.push_back(s);
    }
    return a;
}

inline std::string verilogExpr(const liberty::Expr& e, const std::map<std::string, std::string>& rename) {
    using K = liberty::Expr;
    switch (e.kind) {
        case K::Const: return e.value ? "1'b1" : "1'b0";
        case K::Var: {
            auto it = rename.find(e.var);
            return it == rename.end() ? e.var : it->second;
        }
        case K::Not: return "~(" + verilogExpr(e.kids[0], rename) + ")";
        case K::And: return "(" + verilogExpr(e.kids[0], rename) + " & " + verilogExpr(e.kids[1], rename) + ")";
        case K::Or: return "(" + verilogExpr(e.kids[0], rename) + " | " + verilogExpr(e.kids[1], rename) + ")";
        case K::Xor: return "(" + verilogExpr(e.kids[0], rename) + " ^ " + verilogExpr(e.kids[1], rename) + ")";
    }
    return "1'b0";
}

// The elaborated design as the shared SDC reader sees it: port bits and
// flops, by name.
struct SdcView : sdc::Design {
    std::vector<std::string> ports, cells, regs, none;
    std::unordered_map<std::string, int> dir;
    std::unordered_set<std::string> seq;
    void build(const aig::Design& d) {
        ports.clear();
        cells.clear();
        dir.clear();
        for (auto& p : d.ports)
            for (size_t i = 0; i < p.bits.size(); i++) {
                ports.push_back(bitName(p.name, p.msb, p.lsb, i, p.bits.size()));
                dir[ports.back()] = p.out ? 1 : 0;
            }
        for (auto& f : d.g.flops) cells.push_back(f.name);
        regs = cells;
    }
    // After compile the cells are the netlist's instances.
    void mapped(const Netlist& nl) {
        cells.clear();
        regs.clear();
        for (auto& i : nl.insts) {
            cells.push_back(i.name);
            if (i.seq) regs.push_back(i.name);
        }
        seq.clear();
        seq.insert(regs.begin(), regs.end());
    }
    const std::vector<std::string>& names(sdc::Kind k) const override {
        return k == sdc::Kind::Port ? ports : k == sdc::Kind::Cell ? cells : none;
    }
    int portDir(const std::string& n) const override {
        auto it = dir.find(n);
        return it == dir.end() ? -1 : it->second;
    }
    sdc::ObjList registers(const sdc::RegQuery& q) const override {
        sdc::ObjList o;
        if (q.cells && q.edgeTriggered)
            for (auto& c : regs) o.push_back({sdc::Kind::Cell, c, true});
        return o;
    }
    std::optional<std::string> attribute(const sdc::Obj& o, const std::string& a) const override {
        if (o.kind == sdc::Kind::Port && a == "direction") return portDir(o.name) == 1 ? "out" : "in";
        if (o.kind == sdc::Kind::Cell && a == "is_sequential") return seq.empty() || seq.count(o.name) ? "true" : "false";
        return std::nullopt;
    }
};

struct Shell {
    // The interpreter is the shared SDC reader (src/sdc.hpp): every SDC
    // command and object query is its own; msynth adds the synthesis commands.
    sdc::Reader in;
    SdcView view;
    size_t digested = 0;   // SDC commands already folded into `sdc`
    std::vector<std::string> files, defines, incdirs;
    std::optional<Design> des;
    std::optional<Netlist> nl;
    std::optional<Timing> tim;
    LibInfo lib;
    std::vector<std::string> libFiles;   // what lib was built from
    Constraints sdc;
    bool quiet = false, interactive = false;
    int errors = 0;

    std::function<void(const std::string&)> sink = [](const std::string& s) { fputs(s.c_str(), stdout); };
    std::set<std::string> shown;   // errors already printed

    void out(const std::string& s) { sink(s); }
    std::string var(const std::string& n, const std::string& d = "") { return in.get(n).value_or(d); }
    void outf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        sink(buf);
    }

    std::string findFile(const std::string& f) {
        if (std::filesystem::exists(f)) return f;
        for (auto& d : tcl::splitList(var("search_path"))) {
            std::string p = d + "/" + f;
            if (std::filesystem::exists(p)) return p;
        }
        return f;
    }

    // ---------------------------------------------------------- library
    // mini45 (typical corner), the default target: $MINISIM_MINI45, else next
    // to an installed msynth, else in the source tree this msynth was built from.
    static std::string defaultLib() {
        namespace fs = std::filesystem;
        const char* file = "mini45_tt.lib";
        std::error_code ec;
        if (const char* e = std::getenv("MINISIM_MINI45")) return (fs::path(e) / file).string();
        auto exe = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            auto inst = exe.parent_path().parent_path() / "share" / "minisim" / "lib" / "mini45" / file;
            if (fs::exists(inst, ec)) return inst.string();
        }
        auto src = fs::path(MSYNTH_ROOT_DIR) / "lib" / "mini45" / file;
        if (fs::exists(src, ec)) return src.string();
        throw tcl::Error("the default library mini45_tt.lib was not found; set target_library, or MINISIM_MINI45 to the directory that holds it");
    }
    void loadLibs(const std::vector<std::string>& paths) {
        lib.libs.clear();
        for (auto& p : paths) {
            if (p == "<generic>") {
                try {
                    lib.libs.push_back(std::make_shared<liberty::Library>(liberty::load(defaultLib())));
                } catch (liberty::Error& e) {
                    throw tcl::Error(e.what());
                }
                continue;
            }
            if (p.size() > 3 && p.compare(p.size() - 3, 3, ".db") == 0)
                throw tcl::Error(p + ": .db is a proprietary binary format; give the Liberty source (.lib)");
            try {
                lib.libs.push_back(std::make_shared<liberty::Library>(liberty::load(findFile(p))));
            } catch (liberty::Error& e) {
                throw tcl::Error(e.what());
            }
        }
        lib.build();
        libFiles = paths;
        nl.reset();
        tim.reset();
    }
    // The library the script asked for: read_lib, else $target_library, else the generic one.
    void needLib() {
        std::vector<std::string> want;
        for (auto& f : tcl::splitList(var("target_library")))
            if (f != "*" && !f.empty()) want.push_back(f);
        if (want.empty() && !lib.libs.empty() && libFiles != std::vector<std::string>{"<generic>"}) return;   // read_lib
        if (want.empty()) want.push_back("<generic>");
        if (want != libFiles || lib.libs.empty()) {
            loadLibs(want);
            if (want[0] == "<generic>") out("Information: no target library set; using the mini45 library (mini45_tt).\n");
        }
    }

    // ---------------------------------------------------------- ports
    std::vector<std::string> flatten(const std::vector<std::string>& pos, size_t from = 0) {
        std::vector<std::string> r;
        for (size_t i = from; i < pos.size(); i++)
            for (auto& e : tcl::splitList(pos[i])) r.push_back(e);
        return r;
    }

    // ---------------------------------------------------------- elaborate
    void elaborateTop(const std::string& top, const std::vector<std::string>& params) {
        if (files.empty()) throw tcl::Error("no source files; use analyze or read_verilog first");
        slang::driver::Driver drv;
        drv.addStandardArgs();
        std::vector<std::string> a{"msynth"};
        for (auto& f : files) a.push_back(f);
        if (!top.empty()) { a.push_back("--top"); a.push_back(top); }
        a.push_back("-DSYNTHESIS");
        for (auto& d : defines) a.push_back("-D" + d);
        for (auto& d : incdirs) { a.push_back("-I"); a.push_back(d); }
        for (auto& p : params) { a.push_back("-G"); a.push_back(p); }
        a.push_back("-Wnone");
        std::vector<const char*> argv;
        for (auto& s : a) argv.push_back(s.c_str());
        if (!drv.parseCommandLine((int)argv.size(), argv.data()) || !drv.processOptions() || !drv.parseAllSources())
            throw tcl::Error("cannot read the design");
        auto comp = drv.createCompilation();
        drv.reportCompilation(*comp, true);
        if (!drv.reportDiagnostics(true)) throw tcl::Error("the design has errors");
        auto t0 = std::chrono::steady_clock::now();
        try {
            des = aig::elaborate(*comp);
        } catch (ElabError& e) {
            throw tcl::Error(e.what());
        }
        // Registers nothing reads, and registers stuck at their reset value.
        size_t swept = 0;
        des = aig::sweep(std::move(*des), &swept);
        if (swept && !quiet) outf("Information: removed %zu unloaded or constant register bits.\n", swept);
        if (var("msynth_hold_enables", "true") != "false") {
            size_t held = 0;
            // the mapper's enable flops have no asynchronous reset
            des = aig::holdEnables(std::move(*des), &held, false);
            if (held && !quiet) outf("Information: gathered the hold conditions of %zu register bits into enables.\n", held);
            // a register that now only holds is a constant
            if (held) des = aig::sweep(std::move(*des), nullptr);
        }
#ifdef MSYNTH_HAVE_SAT
        if (var("msynth_fraig", "true") != "false") {
            FraigStats fs;
            des = fraig(*des, &fs);
            if (fs.merged && !quiet)
                outf("Information: merged %zu nodes that compute an earlier node's function (%zu refuted, %zu undecided).\n",
                     fs.merged, fs.refuted, fs.undecided);
        }
#endif
        nl.reset();
        tim.reset();
        view.build(*des);
        in.design = &view;
        in.designChanged();
        digested = (size_t)-1;
        for (auto& w : des->warnings) out("Warning: " + w + "\n");
        size_t ands = 0;
        for (auto& n : des->g.nodes) ands += n.k == NK::And;
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!quiet)
            outf("Elaborated design '%s': %zu ports, %zu flops, %zu AND nodes (%.2fs).\n", des->top.c_str(),
                 des->ports.size(), des->g.flops.size(), ands, dt);
        in.set("current_design", des->top);
    }

    // ---------------------------------------------------------- compile
    void compile(bool forceDelay, bool highEffort = false) {
        if (!des) throw tcl::Error("no design; run elaborate first");
        needLib();
        digest();
        auto t0 = std::chrono::steady_clock::now();
        try {
            Netlist a = Mapper(*des, lib, false).run();
            mergeMux4(a, lib);
            bufferFanout(a, lib, sdc.maxFanout);
            Timing ta = analyze(a, sdc);
            // High effort: ABC restructures the logic, and the result is kept
            // when it maps smaller without losing slack.
            std::string useAbc = var("msynth_abc", "auto");
            if (useAbc == "true" || (useAbc != "false" && highEffort)) {
                std::string prog = abcProgram();
                if (prog.empty()) {
                    if (useAbc == "true") out("Warning: msynth_abc is true but no abc program was found (MSYNTH_ABC, or abc, berkeley-abc, yosys-abc on PATH).\n");
                } else {
                    try {
                        Design r = abcRestructure(*des, prog, var("msynth_abc_script", abcDefaultScript()));
                        Netlist c = Mapper(r, lib, false).run();
                        mergeMux4(c, lib);
                        bufferFanout(c, lib, sdc.maxFanout);
                        Timing tc = analyze(c, sdc);
                        bool keep = c.area() < a.area() - 1e-9 && (!ta.constrained || tc.wns >= std::min(ta.wns, 0.0) - 1e-9);
                        if (!quiet)
                            outf("Information: ABC restructuring: area %.2f against %.2f, %s.\n", c.area(), a.area(), keep ? "kept" : "not kept");
                        if (keep) {
                            a = std::move(c);
                            ta = std::move(tc);
                        }
                    } catch (AbcError& e) {
                        out(std::string("Warning: ABC restructuring skipped: ") + e.what() + "\n");
                    }
                }
            }
            bool viol = ta.constrained && ta.wns < 0;
            // The delay-driven mapping is for a clock that is not met (or, at
            // high effort, to see whether it gives the same slack smaller);
            // without constraints the smallest netlist stands.
            if (viol || (forceDelay && ta.constrained)) {
                Netlist b = Mapper(*des, lib, true).run();
                bufferFanout(b, lib, sdc.maxFanout);
                Timing tb = analyze(b, sdc);
                bool pickB = tb.wns > ta.wns + 1e-9 || (tb.wns >= ta.wns - 1e-9 && b.area() < a.area());
                if (pickB) {
                    a = std::move(b);
                    ta = std::move(tb);
                }
            }
            nl = std::move(a);
            tim = std::move(ta);
            view.mapped(*nl);
            in.designChanged();
        } catch (MapError& e) {
            throw tcl::Error(e.what());
        }
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!quiet) {
            outf("Mapped design '%s': %zu cells, area %.2f", nl->top.c_str(), nl->insts.size(), nl->area());
            if (tim->constrained) outf(", worst slack %.4f%s", tim->ends.empty() ? 0.0 : tim->ends[0].slack, tim->wns < 0 ? " (VIOLATED)" : "");
            else outf(", critical path %.4f", tim->critical);
            outf(" (%.2fs, %d threads).\n", dt, threads());
        }
    }
    // What msynth's timing estimate uses of the constraints read so far:
    // the clocks on ports, max I/O delays, port loads, clock uncertainty,
    // input transition, max area and max fanout. A port object without a
    // design behind it (constraints given before elaborate) is a pattern.
    std::vector<std::string> portsOf(const sdc::ObjList& objs) {
        std::vector<std::string> r;
        for (auto& o : objs) {
            if (o.kind != sdc::Kind::Port) continue;
            if (o.resolved) { r.push_back(o.name); continue; }
            for (auto& p : view.ports)
                if (tcl::globMatch(o.name.c_str(), p.c_str(), true)) r.push_back(p);
        }
        return r;
    }
    bool digest() {
        const sdc::Constraints& c = in.sdc;
        if (digested == c.commands.size()) return false;
        digested = c.commands.size();
        Constraints n;
        std::set<std::string> said;
        auto note = [&](const std::string& m) {
            if (noted.insert(m).second) out("Information: " + m + "\n");
        };
        for (auto& ck : c.clocks) {
            if (ck.generated) { note("generated clock '" + ck.name + "' is not used by msynth's timing estimate."); continue; }
            Constraints::Clock k;
            k.name = ck.name;
            k.period = ck.period;
            k.ports = portsOf(ck.sources);
            bool replaced = false;
            for (auto& o : n.clocks)
                if (o.name == k.name) { o = k; replaced = true; }
            if (!replaced) n.clocks.push_back(k);
        }
        if (n.clocks.size() > 1)
            note("msynth times every flop against the first clock ('" + n.clocks[0].name + "'); the other clocks only mark their ports as clocks.");
        for (auto& d : c.ioDelays) {
            if (d.min && !d.max) continue;   // hold side: not part of a setup estimate
            for (auto& p : portsOf(d.ports)) (d.input ? n.inputDelay : n.outputDelay)[p] = d.value;
        }
        for (auto* cmd : c.of("set_load"))
            for (auto& p : portsOf(cmd->objs("objects"))) n.load[p] = cmd->num("value");
        for (auto& u : c.uncertainties)
            if (!u.hold || u.setup) n.uncertainty = u.value;
        for (auto* cmd : c.of("set_input_transition")) n.inputSlew = cmd->num("transition");
        for (auto* cmd : c.of("set_max_area")) n.maxArea = cmd->num("value");
        for (auto* cmd : c.of("set_max_fanout")) n.maxFanout = std::max(2, (int)cmd->num("value"));
        if (!c.exceptions.empty()) note("timing exceptions (false paths, multicycle paths, max/min delays) are kept for write_sdc but not used by msynth's timing estimate.");
        sdc = std::move(n);
        return true;
    }
    std::set<std::string> noted;
    void needNetlist() {
        if (!nl) throw tcl::Error("the design is not mapped; run compile first");
        if (digest() || !tim) tim = analyze(*nl, sdc);
    }

    // ---------------------------------------------------------- reports
    void header(const char* what) {
        out("\n****************************************\n");
        outf("Report : %s\nDesign : %s\nVersion: msynth %s\n", what, nl ? nl->top.c_str() : des ? des->top.c_str() : "", MINISIM_VERSION);
        out("****************************************\n\n");
    }
    void reportArea() {
        needNetlist();
        header("area");
        size_t nports = 0, comb = 0, seq = 0, bufinv = 0;
        double ca = 0, sa = 0, ba = 0;
        for (auto& p : nl->ports) nports += p.nets.size();
        for (auto& i : nl->insts) {
            if (i.seq) { seq++; sa += i.cell->area; }
            else {
                comb++;
                ca += i.cell->area;
                size_t ins = 0;
                for (auto& p : i.cell->pins) ins += p.dir == liberty::Dir::In;
                if (ins == 1) { bufinv++; ba += i.cell->area; }
            }
        }
        std::set<const liberty::Cell*> refs;
        for (auto& i : nl->insts) refs.insert(i.cell);
        std::string libs;
        for (auto& l : lib.libs) libs += (libs.empty() ? "" : " ") + l->name;
        outf("Library(s) Used:\n\n    %s\n\n", libs.c_str());
        outf("Number of ports:               %10zu\n", nports);
        outf("Number of nets:                %10zu\n", nl->nets.size() - 2);
        outf("Number of cells:               %10zu\n", nl->insts.size());
        outf("Number of combinational cells: %10zu\n", comb);
        outf("Number of sequential cells:    %10zu\n", seq);
        outf("Number of buf/inv:             %10zu\n", bufinv);
        outf("Number of references:          %10zu\n\n", refs.size());
        outf("Combinational area:        %14.4f\n", ca);
        outf("Buf/Inv area:              %14.4f\n", ba);
        outf("Noncombinational area:     %14.4f\n", sa);
        outf("Total cell area:           %14.4f\n", ca + sa);
    }
    void reportReference() {
        needNetlist();
        header("reference");
        std::map<std::string, std::pair<const liberty::Cell*, int>> refs;
        for (auto& i : nl->insts) {
            auto& r = refs[i.cell->name];
            r.first = i.cell;
            r.second++;
        }
        outf("%-24s %12s %8s %14s\n", "Reference", "Unit Area", "Count", "Total Area");
        out(std::string(62, '-') + "\n");
        double tot = 0;
        for (auto& [n, r] : refs) {
            outf("%-24s %12.4f %8d %14.4f\n", n.c_str(), r.first->area, r.second, r.first->area * r.second);
            tot += r.first->area * r.second;
        }
        out(std::string(62, '-') + "\n");
        outf("Total %zu references %39.4f\n", refs.size(), tot);
    }
    void reportCell() {
        needNetlist();
        header("cell");
        outf("%-32s %-20s %10s\n", "Cell", "Reference", "Area");
        out(std::string(64, '-') + "\n");
        for (auto& i : nl->insts) outf("%-32s %-20s %10.4f\n", i.name.c_str(), i.cell->name.c_str(), i.cell->area);
        out(std::string(64, '-') + "\n");
        outf("Total %zu cells %47.4f\n", nl->insts.size(), nl->area());
    }
    int levels(const Timing::End& e) {
        int n = 0, net = e.net, edge = e.edge;
        while (true) {
            const auto& t = tim->nets[net];
            if (t.fromInst < 0 || nl->insts[t.fromInst].seq || t.fromPin[edge] < 0) break;
            n++;
            int ip = t.fromPin[edge], ie = t.fromEdge[edge];
            net = nl->insts[t.fromInst].pins[ip];
            edge = ie;
        }
        return n;
    }
    void reportTiming(int maxPaths) {
        needNetlist();
        header("timing\n        -path full\n        -delay max");
        if (tim->ends.empty()) { out("No paths.\n"); return; }
        for (int k = 0; k < maxPaths && k < (int)tim->ends.size(); k++) {
            out(pathReport(*nl, *tim, sdc, tim->ends[k]));
            out("\n");
        }
    }
    void reportQor() {
        needNetlist();
        header("qor");
        size_t comb = 0, seq = 0;
        for (auto& i : nl->insts) (i.seq ? seq : comb)++;
        out("  Timing Path Group '" + (sdc.clock() ? sdc.clock()->name : std::string("(none)")) + "'\n");
        out("  -----------------------------------\n");
        if (!tim->ends.empty()) {
            outf("  Levels of Logic:        %10d\n", levels(tim->ends[0]));
            outf("  Critical Path Length:   %10.4f\n", tim->ends[0].arrival);
            if (tim->constrained) {
                outf("  Critical Path Slack:    %10.4f\n", tim->ends[0].slack);
                outf("  Critical Path Clk Period:%9.4f\n", sdc.clock()->period);
                outf("  Total Negative Slack:   %10.4f\n", tim->tns);
                outf("  No. of Violating Paths: %10d\n", tim->violations);
            }
        }
        out("\n  Cell Count\n  -----------------------------------\n");
        outf("  Leaf Cell Count:        %10zu\n", nl->insts.size());
        outf("  Combinational Cell Count:%9zu\n", comb);
        outf("  Sequential Cell Count:  %10zu\n", seq);
        out("\n  Area\n  -----------------------------------\n");
        outf("  Design Area:            %10.4f\n", nl->area());
    }
    void reportConstraint() {
        needNetlist();
        header("constraint\n        -all_violators");
        if (!tim->constrained) { out("The design has no clock constraint.\n"); return; }
        if (!tim->violations) { out("This design has no violated constraints.\n"); return; }
        outf("   max_delay/setup ('%s' group)\n\n", sdc.clock()->name.c_str());
        outf("   %-40s %10s %10s %10s\n", "Endpoint", "Required", "Arrival", "Slack");
        out("   " + std::string(73, '-') + "\n");
        for (auto& e : tim->ends)
            if (e.slack < 0)
                outf("   %-40s %10.4f %10.4f %10.4f  (VIOLATED)\n", e.name.c_str(), e.required, e.arrival, e.slack);
    }

    // ---------------------------------------------------------- writers
    // The mapped netlist as a gnl::Module, so that the shared structural
    // netlist writer (src/gnetlist.hpp) is the only one in the tree.
    gnl::Module toModule() const {
        gnl::Module m;
        m.name = nl->top;
        std::set<std::string> portNames;
        for (auto& p : nl->ports) {
            portNames.insert(p.name);
            m.portOrder.push_back(p.name);
            gnl::Decl& d = m.decl(p.name);
            d.isPort = true;
            d.dir = p.out ? gnl::Dir::Out : gnl::Dir::In;
            d.ranged = p.nets.size() > 1 || p.msb != 0 || p.lsb != 0;
            d.msb = p.msb;
            d.lsb = p.lsb;
        }
        auto ref = [&](const std::string& name) {
            gnl::Ref r;
            if (name == "1'b0" || name == "1'b1") {
                r.kind = gnl::Ref::Const;
                r.bits = {(uint8_t)(name[3] - '0')};
                return r;
            }
            size_t br = name.rfind('[');
            if (br != std::string::npos && name.back() == ']' && portNames.count(name.substr(0, br))) {
                r.kind = gnl::Ref::Bit;
                r.name = name.substr(0, br);
                r.msb = r.lsb = std::atoi(name.c_str() + br + 1);
                return r;
            }
            r.kind = gnl::Ref::Net;
            r.name = name;
            return r;
        };
        std::vector<uint8_t> used(nl->nets.size(), 0);
        for (auto& i : nl->insts)
            for (int n : i.pins)
                if (n >= 0) used[n] = 1;
        for (auto& [bn, id] : nl->assigns) used[id] = 1;
        for (size_t n = 2; n < nl->nets.size(); n++)
            if (used[n] && ref(nl->nets[n]).kind == gnl::Ref::Net && !portNames.count(nl->nets[n])) m.decl(nl->nets[n]);
        for (auto& i : nl->insts) {
            gnl::Inst gi;
            gi.cell = i.cell->name;
            gi.name = i.name;
            for (size_t p = 0; p < i.pins.size(); p++) {
                // an unused output is written as .QN() so that simulators do not warn
                if (i.pins[p] < 0 && i.cell->pins[p].dir != liberty::Dir::Out) continue;
                gnl::Conn c;
                c.port = i.cell->pins[p].name;
                if (i.pins[p] >= 0) c.refs.push_back(ref(nl->nets[i.pins[p]]));
                gi.conns.push_back(std::move(c));
            }
            m.insts.push_back(std::move(gi));
        }
        for (auto& [bn, id] : nl->assigns) m.assigns.push_back({{ref(bn)}, {ref(nl->nets[id])}});
        return m;
    }
    void writeVerilog(const std::string& path) {
        needNetlist();
        std::ofstream f(path);
        if (!f) throw tcl::Error("cannot write " + path);
        f << "// Generated by msynth " << MINISIM_VERSION << "\n// Design: " << nl->top << "\n\n";
        gnl::write(toModule(), f);
    }

    // Verilog simulation models of the library's cells, from their Liberty functions.
    void writeLibModels(const std::string& path) {
        needLib();
        std::ofstream f(path);
        if (!f) throw tcl::Error("cannot write " + path);
        f << "// Cell models generated by msynth " << MINISIM_VERSION << " from the Liberty functions.\n\n";
        for (auto& l : lib.libs)
            for (auto& c : l->cells) {
                std::vector<const liberty::Pin*> ins, outs;
                bool ok = true;
                for (auto& p : c.pins) {
                    if (p.dir == liberty::Dir::In) ins.push_back(&p);
                    else if (p.dir == liberty::Dir::Out) outs.push_back(&p);
                    else ok = false;
                    if (!p.bus.empty()) ok = false;
                }
                if (!ok || outs.empty() || !c.latches.empty() || c.ffs.size() > 1) continue;
                std::ostringstream m;
                try {
                    m << "module " << c.name << " (";
                    bool first = true;
                    for (auto* p : ins) { m << (first ? "" : ", ") << p->name; first = false; }
                    for (auto* p : outs) { m << (first ? "" : ", ") << p->name; first = false; }
                    m << ");\n";
                    for (auto* p : ins) m << "  input " << p->name << ";\n";
                    for (auto* p : outs) m << "  output " << p->name << ";\n";
                    std::map<std::string, std::string> ren;
                    if (!c.ffs.empty()) {
                        const auto& ff = c.ffs[0];
                        ren[ff.iq] = "IQ__";
                        ren[ff.iqn] = "(~IQ__)";
                        std::string ckv, v;
                        bool ckNeg, neg;
                        if (!LibInfo::simpleVar(ff.clockedOn, ckv, ckNeg)) continue;
                        m << "  reg IQ__;\n  always @(" << (ckNeg ? "negedge " : "posedge ") << ckv;
                        std::string body;
                        for (int k = 0; k < 2; k++) {
                            const std::string& fn = k == 0 ? ff.clear : ff.preset;
                            if (fn.empty()) continue;
                            if (!LibInfo::simpleVar(fn, v, neg)) throw liberty::Error("async");
                            m << " or " << (neg ? "negedge " : "posedge ") << v;
                            body += std::string("    ") + (body.empty() ? "" : "else ") + "if (" + (neg ? "!" : "") + v + ") IQ__ <= 1'b" + (k == 0 ? "0" : "1") + ";\n";
                        }
                        m << ")\n" << body << "    " << (body.empty() ? "" : "else ") << "IQ__ <= "
                          << verilogExpr(liberty::parseFunction(ff.nextState), ren) << ";\n";
                    }
                    for (auto* p : outs) {
                        if (!p->hasFunction() || !p->threeState.empty()) throw liberty::Error("no function");
                        m << "  assign " << p->name << " = " << verilogExpr(p->functionExpr(), ren) << ";\n";
                    }
                    m << "endmodule\n\n";
                } catch (liberty::Error&) {
                    continue;   // a cell this simple model cannot describe
                }
                f << m.str();
            }
    }

    // The constraints as read, one command per line, objects by name.
    void writeSdc(const std::string& path) {
        std::ofstream f(path);
        if (!f) throw tcl::Error("cannot write " + path);
        f << "# Written by msynth " << MINISIM_VERSION << "\n";
        auto objs = [](const sdc::ObjList& l) {
            std::map<sdc::Kind, std::vector<std::string>> by;
            for (auto& o : l) by[o.kind].push_back(o.name);
            std::string r;
            for (auto& [k, names] : by) {
                const char* get = k == sdc::Kind::Port ? "get_ports" : k == sdc::Kind::Pin ? "get_pins" : k == sdc::Kind::Cell ? "get_cells"
                                : k == sdc::Kind::Net ? "get_nets" : k == sdc::Kind::Clock ? "get_clocks" : "get_lib_cells";
                r += std::string(r.empty() ? "" : " ") + "[" + get + " {" + tcl::joinList(names) + "}]";
            }
            return by.size() > 1 ? "[list " + r + "]" : r;
        };
        for (auto& c : in.sdc.commands) {
            f << c.name;
            for (auto& a : c.args) {
                bool opt = !a.opt.empty() && a.opt[0] == '-';
                if (opt) f << " " << a.opt;
                if (!a.objs.empty()) {
                    // -clock takes a clock by name; everything else a collection
                    if (opt && a.objs.size() == 1 && a.objs[0].kind == sdc::Kind::Clock) f << " " << a.objs[0].name;
                    else f << " " << objs(a.objs);
                }
                else if (!opt || !a.s.empty()) f << " " << tcl::joinList({a.s});
            }
            f << "\n";
        }
    }

    // ---------------------------------------------------------- commands
    using Words = const std::vector<std::string>;
    std::map<std::string, std::string> helpText;

    void cmd(const std::string& name, const std::string& help, std::function<std::string(Words&)> fn) {
        helpText[name] = help;
        // `command ... > file` and `>> file`, as dc_shell has it. An error is
        // printed where it happens; the interpreter also keeps it in diags.
        in.addCommand(name, [this, fn](Words& w) -> std::string {
            try {
                for (size_t i = 1; i + 1 < w.size(); i++)
                    if (w[i] == ">" || w[i] == ">>") {
                        std::ofstream f(w[i + 1], w[i] == ">>" ? std::ios::app : std::ios::trunc);
                        if (!f) throw tcl::Error("cannot write " + w[i + 1]);
                        auto saved = sink;
                        sink = [&f](const std::string& s) { f << s; };
                        struct Back { Shell& sh; std::function<void(const std::string&)> s; ~Back() { sh.sink = s; } } back{*this, saved};
                        return fn(std::vector<std::string>(w.begin(), w.begin() + i));
                    }
                return fn(w);
            } catch (tcl::Error& e) {
                errors++;
                if (!interactive) {
                    fflush(stdout);
                    std::cerr << "Error: " << e.what() << "\n";
                    shown.insert(e.what());
                }
                throw;
            }
        });
    }

    double num(const std::string& s, const char* what) {
        char* end = nullptr;
        double d = strtod(s.c_str(), &end);
        if (s.empty() || !end || *end) throw tcl::Error(std::string("Error: ") + what + ": expected a number, got '" + s + "'");
        return d;
    }

    Shell() {
        in.onPuts = [this](const std::string& s) { out(s + "\n"); };
        in.set("target_library", "");
        in.set("link_library", "");
        in.set("search_path", ".");
        in.set("synthetic_library", "");
        in.set("sh_product_version", MINISIM_VERSION);

        auto readFiles = [this](Words& w) -> std::string {
            Args a = parseArgs(w, {"-format", "-f", "-define", "-work", "-library", "-top", "-autoread", "-rtl"});
            for (auto& d : tcl::splitList(a.get("-define"))) defines.push_back(d);
            auto fs = flatten(a.pos);
            if (fs.empty()) throw tcl::Error(w[0] + ": no files");
            for (auto& f : fs) {
                std::string p = findFile(f);
                if (!std::filesystem::exists(p)) throw tcl::Error("cannot open file '" + f + "'");
                if (std::find(files.begin(), files.end(), p) == files.end()) files.push_back(p);
                if (!quiet) out("Loading source file '" + p + "'\n");
            }
            return "1";
        };
        cmd("analyze", "analyze [-format verilog|sverilog] [-define {A B=1}] {files}: read RTL source files", readFiles);
        cmd("read_verilog", "read_verilog {files}: read RTL source files (same as analyze)", readFiles);
        cmd("read_sverilog", "read_sverilog {files}: read RTL source files (same as analyze)", readFiles);
        cmd("read_file", "read_file [-format f] {files}: read RTL source files (same as analyze)", readFiles);
        cmd("elaborate", "elaborate top [-parameters \"N=8,W=4\"]: build the design from the analyzed files", [this](Words& w) {
            Args a = parseArgs(w, {"-parameters", "-architecture", "-work", "-library"});
            std::vector<std::string> params;
            std::string ps = a.get("-parameters");
            for (char& c : ps)
                if (c == ',') c = ' ';
            for (auto p : tcl::splitList(ps)) {
                size_t arrow = p.find("=>");
                if (arrow != std::string::npos) p = p.substr(0, arrow) + "=" + p.substr(arrow + 2);
                params.push_back(p);
            }
            elaborateTop(a.pos.empty() ? "" : a.pos[0], params);
            return std::string("1");
        });
        cmd("current_design", "current_design [name]: the design being worked on", [this](Words& w) {
            if (w.size() > 1 && des && w[1] != des->top) throw tcl::Error("design '" + w[1] + "' is not loaded (msynth flattens to the elaborated top)");
            return des ? des->top : std::string();
        });
        cmd("read_lib", "read_lib {files.lib}: read Liberty libraries as the target library", [this](Words& w) {
            Args a = parseArgs(w, {});
            loadLibs(flatten(a.pos));
            in.onPuts = [this](const std::string& s) { out(s + "\n"); };
        in.set("target_library", "");
            size_t cells = 0;
            for (auto& l : lib.libs) cells += l->cells.size();
            if (!quiet) outf("Read %zu cells: %zu combinational and %zu flip-flop cells are usable for mapping.\n", cells, lib.gates.size(), lib.flops.size());
            return std::string("1");
        });
        in.hostCommands["read_liberty"] = in.hostCommands["read_lib"];
        cmd("set_dont_use", "set_dont_use {cells}: keep the mapper from using these library cells (patterns allowed)", [this](Words& w) {
            Args a = parseArgs(w, {});
            needLib();
            for (auto& pat : flatten(a.pos)) {
                std::string p = pat;
                if (size_t s = p.rfind('/'); s != std::string::npos) p = p.substr(s + 1);
                for (auto& l : lib.libs)
                    for (auto& c : l->cells)
                        if (glob(p.c_str(), c.name.c_str())) lib.dontUse.insert(c.name);
            }
            lib.build();
            return std::string("1");
        });

        // Object queries (get_ports, all_inputs, ...) and the SDC constraints
        // are the reader's own commands.
        cmd("read_sdc", "read_sdc file: read constraints (SDC); every SDC command can also be typed directly", [this](Words& w) {
            Args a = parseArgs(w, {"-version"});
            if (a.pos.size() != 1) throw tcl::Error("read_sdc: a file is expected");
            std::string f = findFile(a.pos[0]);
            if (!std::filesystem::exists(f)) throw tcl::Error("cannot open file '" + a.pos[0] + "'");
            in.load(f);
            return std::string("1");
        });
        // Accepted so that existing scripts run; they do not change msynth's result.
        for (const char* n : {"link", "uniquify", "ungroup", "set_fix_multiple_port_nets", "set_dont_touch", "set_dont_touch_network",
                              "set_flatten",
                              "set_structure", "define_design_lib", "change_names", "set_svf", "check_library",
                              "set_critical_range",
                              "set_cost_priority", "define_name_rules",
                              "remove_unconnected_ports", "set_clock_gating_style", "suppress_message",
                              "set_auto_disable_drc_nets", "set_boundary_optimization", "saif_map", "set_leakage_optimization",
                              "set_dynamic_optimization", "set_register_merging", "set_optimize_registers"}) {
            in.addCommand(n, [](Words&) { return std::string("1"); });
        }
        cmd("set_app_var", "set_app_var name value: set a shell variable", [this](Words& w) {
            if (w.size() != 3) throw tcl::Error("set_app_var name value");
            in.set(w[1], w[2]);
            return w[2];
        });
        cmd("set_host_options", "set_host_options -max_cores N: threads to use (default: every core)", [](Words& w) {
            Args a = parseArgs(w, {"-max_cores", "-num_processes"});
            if (a.has("-max_cores")) threadCount() = std::max(1, atoi(a.get("-max_cores").c_str()));
            return std::string("1");
        });

        // ---- synthesis
        auto doCompile = [this](Words& w) {
            Args a = parseArgs(w, {"-map_effort", "-area_effort", "-power_effort"});
            bool high = a.get("-map_effort") == "high" || w[0] == "compile_ultra";
            compile(a.has("-timing_high_effort_script") || high, high);
            return std::string("1");
        };
        cmd("compile", "compile [-map_effort low|medium|high]: optimize and map the design onto the target library", doCompile);
        cmd("compile_ultra", "compile_ultra: compile at high effort (ABC restructuring when an abc program is found; the delay-driven mapping always tried)", doCompile);
        cmd("check_design", "check_design: summary and warnings of the elaborated design", [this](Words&) {
            if (!des) throw tcl::Error("no design; run elaborate first");
            for (auto& x : des->warnings) out("Warning: " + x + "\n");
            size_t ands = 0;
            for (auto& n : des->g.nodes) ands += n.k == NK::And;
            outf("Design '%s': %zu ports, %zu flops, %zu AND nodes, %zu warnings.\n", des->top.c_str(), des->ports.size(), des->g.flops.size(), ands, des->warnings.size());
            return std::string("1");
        });

        cmd("report_resources", "report_resources: the word-level operators of the design (adders, multipliers, comparators, shifters) flops with an enable, and memories with their ports", [this](Words&) {
            if (!des) throw tcl::Error("no design; run elaborate first");
            header("resources");
            static const char* names[] = {"add", "sub", "mult", "less-than", "equal", "shift-left", "shift-right", "arith-shift-right"};
            std::map<std::pair<int, size_t>, int> count;
            for (auto& op : des->ops) count[{(int)op.kind, op.a.size()}]++;
            outf("%-20s %8s %8s\n", "Operator", "Width", "Count");
            out(std::string(38, '-') + "\n");
            for (auto& [k, n] : count) outf("%-20s %8zu %8d\n", names[k.first], k.second, n);
            size_t en = 0;
            for (auto& f : des->g.flops) {
                Lit e, d;
                en += flopEnable(des->g, f, e, d);
            }
            outf("\nFlip-flops: %zu, of which %zu load under an enable.\n", des->g.flops.size(), en);
            if (!des->mems.empty()) {
                outf("\n%-28s %8s %8s %8s %8s\n", "Memory", "Depth", "Width", "Write", "Read");
                out(std::string(64, '-') + "\n");
                for (auto& m : des->mems) outf("%-28s %8d %8d %8zu %8zu\n", m.name.c_str(), m.depth, m.width, m.wr.size(), m.rd.size());
            }
            return std::string();
        });

        // ---- reports
        cmd("report_area", "report_area: cell counts and area", [this](Words&) { reportArea(); return std::string(); });
        cmd("report_timing", "report_timing [-max_paths N]: the worst paths", [this](Words& w) {
            Args a = parseArgs(w, {"-max_paths", "-nworst", "-delay", "-path", "-significant_digits", "-from", "-to", "-through", "-delay_type"});
            reportTiming(std::max(1, atoi(a.get("-max_paths", "1").c_str())));
            return std::string();
        });
        cmd("report_qor", "report_qor: timing, cell count and area summary", [this](Words&) { reportQor(); return std::string(); });
        cmd("report_cell", "report_cell: every cell instance", [this](Words&) { reportCell(); return std::string(); });
        cmd("report_reference", "report_reference: library cells used and how many of each", [this](Words&) { reportReference(); return std::string(); });
        cmd("report_constraint", "report_constraint [-all_violators]: endpoints that miss timing", [this](Words&) { reportConstraint(); return std::string(); });
        cmd("report_lib", "report_lib: the cells of the target library usable for mapping", [this](Words&) {
            needLib();
            outf("%-20s %10s %8s  %s\n", "Cell", "Area", "Inputs", "Function");
            for (auto& gt : lib.gates)
                outf("%-20s %10.4f %8zu  %s\n", gt.cell->name.c_str(), gt.area, gt.in.size(), gt.cell->pins[gt.out].function.c_str());
            for (auto& fc : lib.flops)
                outf("%-20s %10.4f %8s  flip-flop%s%s%s\n", fc.cell->name.c_str(), fc.area, "", fc.negClk ? ", falling edge" : "",
                     fc.clr >= 0 ? ", clear" : "", fc.pre >= 0 ? ", preset" : "");
            return std::string();
        });

        // ---- output
        auto doWrite = [this](Words& w) {
            Args a = parseArgs(w, {"-format", "-f", "-output", "-o"});
            std::string fmt = a.get("-format", a.get("-f", "verilog"));
            std::string path = a.get("-output", a.get("-o", a.pos.empty() ? "" : a.pos[0]));
            if (path.empty()) throw tcl::Error(w[0] + ": -output file is required");
            if (fmt != "verilog") throw tcl::Error("write: format '" + fmt + "' is not supported (verilog is; .ddc is proprietary)");
            writeVerilog(path);
            if (!quiet) out("Writing verilog file '" + path + "'.\n");
            return std::string("1");
        };
        cmd("write", "write -format verilog -output file: the mapped netlist", doWrite);
        cmd("write_file", "write_file -format verilog -output file: the mapped netlist", doWrite);
        cmd("write_verilog", "write_verilog file: the mapped netlist", doWrite);
        cmd("write_sdc", "write_sdc file: the constraints", [this](Words& w) {
            Args a = parseArgs(w, {"-version"});
            if (a.pos.empty()) throw tcl::Error("write_sdc: a file is expected");
            writeSdc(a.pos[0]);
            return std::string("1");
        });
        cmd("write_lib_models", "write_lib_models file: Verilog simulation models of the target library's cells", [this](Words& w) {
            if (w.size() < 2) throw tcl::Error("write_lib_models: a file is expected");
            writeLibModels(w[1]);
            return std::string("1");
        });

        // ---- shell
        cmd("redirect", "redirect [-append] file {command}: send a command's output to a file", [this](Words& w) {
            Args a = parseArgs(w, {"-variable"});
            if (a.pos.size() != 2) throw tcl::Error("redirect file {command}");
            std::ofstream f(a.pos[0], a.has("-append") ? std::ios::app : std::ios::trunc);
            if (!f) throw tcl::Error("cannot write " + a.pos[0]);
            auto saved = sink;
            sink = [&f](const std::string& s) { f << s; };
            struct Back { Shell& sh; std::function<void(const std::string&)> s; ~Back() { sh.sink = s; } } back{*this, saved};
            in.parse(a.pos[1], in.where().file, in.where().line);
            return std::string();
        });
        cmd("getenv", "getenv NAME: an environment variable", [](Words& w) {
            const char* v = w.size() > 1 ? getenv(w[1].c_str()) : nullptr;
            return std::string(v ? v : "");
        });
        cmd("file", "file exists|mkdir|tail|dirname|rootname path", [](Words& w) -> std::string {
            if (w.size() < 3) throw tcl::Error("file option path");
            std::filesystem::path p(w[2]);
            if (w[1] == "exists") return std::filesystem::exists(p) ? "1" : "0";
            if (w[1] == "mkdir") { std::filesystem::create_directories(p); return ""; }
            if (w[1] == "tail") return p.filename().string();
            if (w[1] == "dirname") return p.parent_path().empty() ? "." : p.parent_path().string();
            if (w[1] == "rootname") return (p.parent_path() / p.stem()).string();
            if (w[1] == "extension") return p.extension().string();
            throw tcl::Error("file: unsupported option '" + w[1] + "'");
        });
        cmd("date", "date: the current time", [](Words&) {
            time_t t = time(nullptr);
            std::string s = ctime(&t);
            if (!s.empty() && s.back() == '\n') s.pop_back();
            return s;
        });
        cmd("help", "help [command]: the commands", [this](Words& w) {
            if (w.size() > 1) {
                auto it = helpText.find(w[1]);
                out(it == helpText.end() ? "No help for '" + w[1] + "'.\n" : it->second + "\n");
                return std::string();
            }
            for (auto& [n, h] : helpText) out("  " + h + "\n");
            out("Tcl: set, expr, if, for, foreach, while, proc, source, list commands, string, format, puts, echo.\n");
            return std::string();
        });
    }
};

}  // namespace synth
