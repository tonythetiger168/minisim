// meco - functional ECO on a gate netlist (src/eco.hpp).
//
//   meco -old old.v -new new.v -lib cells.lib [-o patched.v] [-script eco.tcl]
//
// old.v is the netlist that is already placed and routed; new.v is the
// revised RTL after synthesis. meco writes the old netlist with the fewest
// cells changed that make it equivalent to new.v, and proves the result.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

#include "eco.hpp"
#include "version.hpp"

static void usage() {
    std::puts(
        "usage: meco -old OLD.v -new NEW.v -lib CELLS.lib [options]\n"
        "  -old FILE       the implemented gate netlist (repeatable)\n"
        "  -new FILE       the revised design as a gate netlist (repeatable)\n"
        "  -lib FILE       Liberty cell library (repeatable)\n"
        "  -top NAME       top module of both (default: the one nothing instantiates)\n"
        "  -old-top NAME, -new-top NAME   when the two differ\n"
        "  -tie PORT=0|1   hold an input constant in both (scan enable, test mode)\n"
        "  -o FILE         write the patched netlist\n"
        "  -script FILE    write the edits as Tcl (create_cell, connect_net ...)\n"
        "  -report FILE    write the report to a file as well\n"
        "  -prefix STR     prefix of added cells and nets (default ECO_)\n"
        "  -no-sweep       keep old cells that end up driving nothing\n"
        "  -no-rectify     only copy the changed cones (do not look for the gate to change)\n"
        "  -budget N       SAT conflicts per proof (default 100000)\n"
        "  -threads N      worker threads (default: all logical cores; results do not depend on N)\n"
        "  -q              print only the summary line\n"
        "exit status: 0 the patched netlist is proved equivalent, 1 it is not, 2 error");
}

static std::string pickTop(const gnl::Source& src, const std::string& want, const char* which) {
    if (!want.empty()) return want;
    auto t = gnl::tops(src);
    if (t.size() != 1) throw eco::Error(std::string("the ") + which + " netlist has " + std::to_string(t.size()) +
                                        " top modules; name one with -top");
    return t[0];
}

int main(int argc, char** argv) {
    std::vector<std::string> oldF, newF, libF;
    std::string top, oldTop, newTop, out, script, report;
    eco::Options opt;
    bool quiet = false;
    try {
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            auto val = [&]() -> std::string {
                if (i + 1 >= argc) throw eco::Error(a + " needs a value");
                return argv[++i];
            };
            if (a == "-old") oldF.push_back(val());
            else if (a == "-new") newF.push_back(val());
            else if (a == "-lib") libF.push_back(val());
            else if (a == "-top") top = val();
            else if (a == "-old-top") oldTop = val();
            else if (a == "-new-top") newTop = val();
            else if (a == "-o") out = val();
            else if (a == "-script") script = val();
            else if (a == "-report") report = val();
            else if (a == "-prefix") opt.prefix = val();
            else if (a == "-no-sweep") opt.sweep = false;
            else if (a == "-budget") opt.budget = std::atol(val().c_str());
            else if (par::threadsArg(argc, argv, i, opt.threads, "meco")) {}
            else if (a == "-no-rectify") opt.rectify = false;
            else if (a == "-q") quiet = true;
            else if (a == "-tie") {
                std::string v = val();
                size_t eq = v.find('=');
                if (eq == std::string::npos || (v.substr(eq + 1) != "0" && v.substr(eq + 1) != "1"))
                    throw eco::Error("-tie wants PORT=0 or PORT=1");
                opt.ties[v.substr(0, eq)] = v[eq + 1] - '0';
            } else if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
            else if (a == "-version" || a == "--version") { std::printf("meco %s\n", MINISIM_VERSION); return 0; }
            else throw eco::Error("unknown option " + a + " (see meco -h)");
        }
        if (oldF.empty() || newF.empty() || libF.empty()) { usage(); return 2; }
        eco::Lib lib;
        for (auto& f : libF) lib.add(f);
        if (opt.threads == 0) opt.threads = par::defaultThreads();
        gnl::Module mo, mn;  // the two netlists are read side by side
        par::parallelFor(2, opt.threads, [&](size_t i) {
            gnl::Source src;
            for (auto& f : i ? newF : oldF) gnl::read(f, src);
            const std::string& t = i ? (newTop.empty() ? top : newTop) : (oldTop.empty() ? top : oldTop);
            (i ? mn : mo) = eco::flatModule(src, pickTop(src, t, i ? "new" : "old"), lib);
        });
        eco::Result r = eco::run(mo, mn, lib, opt);

        std::ostringstream rep;
        if (!quiet) {
            for (auto& w : r.warnings) rep << "warning: " << w << "\n";
            if (!r.failing.empty()) {
                rep << "changed compare points (" << r.failing.size() << "):\n";
                for (auto& f : r.failing) rep << "  " << f << "\n";
            }
            for (auto& c : r.changes) {
                if (c.kind == eco::Change::AddCell) rep << "  + cell " << c.a << " (" << c.b << ")\n";
                else if (c.kind == eco::Change::RemoveCell) rep << "  - cell " << c.a << " (" << c.b << ")\n";
                else if (c.kind == eco::Change::AddPort) rep << "  + " << c.b << " port " << c.a << "\n";
                else if (c.kind == eco::Change::Connect) rep << "  > " << c.b << " <- " << c.a << "\n";
            }
            for (auto& u : r.unverified) rep << "NOT EQUIVALENT after the patch: " << u << "\n";
        }
        char line[512];
        std::snprintf(line, sizeof line,
                      "meco: %zu compare points, %zu changed; patch: +%zu cells (area %.6g), -%zu cells (area %.6g), "
                      "%zu pins rewired, %zu existing nets reused; %s\n",
                      r.points, r.failing.size(), r.added, r.areaAdded, r.removed, r.areaRemoved, r.rewired, r.reused,
                      r.verified ? "patched netlist proved equivalent" : "patched netlist is NOT equivalent");
        rep << line;
        std::cout << rep.str();
        if (!report.empty()) std::ofstream(report) << rep.str();
        if (!out.empty()) {
            std::ofstream os(out);
            if (!os) throw eco::Error("cannot write " + out);
            os << "// patched by meco: +" << r.added << " cells, -" << r.removed << " cells\n";
            gnl::write(r.patched, os);
        }
        if (!script.empty()) {
            std::ofstream os(script);
            if (!os) throw eco::Error("cannot write " + script);
            eco::writeScript(r, os);
        }
        return r.verified ? 0 : 1;
    } catch (std::exception& e) {
        std::fprintf(stderr, "meco: %s\n", e.what());
        return 2;
    }
}
