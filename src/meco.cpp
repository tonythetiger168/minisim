// meco - functional ECO on a gate netlist (src/eco.hpp).
//
//   meco -old old.v -new new.v -lib cells.lib [-o patched.v] [-script eco.tcl]
//   meco -old old.v -rtl new.sv -lib cells.lib ...     (synthesized with msynth first)
//
// old.v is the netlist that is already placed and routed; new.v is the
// revised RTL after synthesis. meco writes the old netlist with the fewest
// cells changed that make it equivalent to new.v, and proves the result.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <unistd.h>

#include "eco.hpp"
#include "version.hpp"

static void usage() {
    std::puts(
        "usage: meco -old OLD.v -new NEW.v -lib CELLS.lib [options]\n"
        "  -old FILE       the implemented gate netlist (repeatable)\n"
        "  -new FILE       the revised design as a gate netlist (repeatable)\n"
        "  -rtl FILE       or the revised RTL (repeatable): msynth makes the netlist first\n"
        "  -msynth PATH    the msynth to run for -rtl (default: the one beside meco, else on PATH)\n"
        "  -synth-out FILE keep the netlist msynth wrote for -rtl\n"
        "  -lib FILE       Liberty cell library (repeatable)\n"
        "  -top NAME       top module of both (default: the one nothing instantiates)\n"
        "  -old-top NAME, -new-top NAME   when the two differ\n"
        "  -tie PORT=0|1   hold an input constant in both (scan enable, test mode)\n"
        "  -o FILE         write the patched netlist\n"
        "  -script FILE    write the edits as Tcl (create_cell, connect_net ...)\n"
        "  -report FILE    write the report to a file as well\n"
        "  -prefix STR     prefix of added cells and nets (default ECO_)\n"
        "  -no-sweep       keep old cells that end up driving nothing\n"
        "  -spare          metal-only: add and remove no cell; build the patch from the cells\n"
        "                  that drive nothing (spare cells) and the ones the change frees\n"
        "  -spare-cells GLOB   -spare, with only the unloaded cells of this instance name as spares\n"
        "  -no-rectify     only copy the changed cones (do not look for the gate to change)\n"
        "  -no-invert      do not reuse an old net with the inverse function through an inverter\n"
        "  -budget N       SAT conflicts per proof (default 100000)\n"
        "  -threads N      worker threads (default: all logical cores; results do not depend on N)\n"
        "  -q              print only the summary line\n"
        "exit status: 0 the patched netlist is proved equivalent, 1 it is not, 2 error,\n             3 proved, but -spare had to add cells");
}

static std::string pickTop(const gnl::Source& src, const std::string& want, const char* which) {
    if (!want.empty()) return want;
    auto t = gnl::tops(src);
    if (t.size() != 1) throw eco::Error(std::string("the ") + which + " netlist has " + std::to_string(t.size()) +
                                        " top modules; name one with -top");
    return t[0];
}

int main(int argc, char** argv) {
    std::vector<std::string> oldF, newF, libF, rtlF;
    std::string top, oldTop, newTop, out, script, report, msynth, synthOut;
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
            else if (a == "-rtl") rtlF.push_back(val());
            else if (a == "-msynth") msynth = val();
            else if (a == "-synth-out") synthOut = val();
            else if (a == "-lib") libF.push_back(val());
            else if (a == "-top") top = val();
            else if (a == "-old-top") oldTop = val();
            else if (a == "-new-top") newTop = val();
            else if (a == "-o") out = val();
            else if (a == "-script") script = val();
            else if (a == "-report") report = val();
            else if (a == "-prefix") opt.prefix = val();
            else if (a == "-no-sweep") opt.sweep = false;
            else if (a == "-spare") opt.spare = true;
            else if (a == "-spare-cells") opt.spare = true, opt.spareGlob = val();
            else if (a == "-budget") opt.budget = std::atol(val().c_str());
            else if (par::threadsArg(argc, argv, i, opt.threads, "meco")) {}
            else if (a == "-no-rectify") opt.rectify = false;
            else if (a == "-no-invert") opt.invert = false;
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
        if (oldF.empty() || (newF.empty() && rtlF.empty()) || libF.empty()) { usage(); return 2; }
        if (!newF.empty() && !rtlF.empty()) throw eco::Error("give the revised design as -new or as -rtl, not both");
        eco::Lib lib;
        for (auto& f : libF) lib.add(f);
        if (opt.threads == 0) opt.threads = par::defaultThreads();
        // Revised RTL: the synthesis tool makes the netlist, with the first
        // library and the top module of the old netlist.
        std::string synthTmp;
        if (!rtlF.empty()) {
            namespace fs = std::filesystem;
            std::string t = !newTop.empty() ? newTop : top;
            if (t.empty()) {
                gnl::Source src;
                for (auto& f : oldF) gnl::read(f, src);
                t = pickTop(src, oldTop, "old");
            }
            if (msynth.empty()) {
                std::error_code ec;
                fs::path self = fs::read_symlink("/proc/self/exe", ec);
                if (ec) self = argv[0];
                fs::path beside = self.parent_path() / "msynth";
                msynth = fs::exists(beside, ec) ? beside.string() : "msynth";
            }
            std::string netl = synthOut;
            if (netl.empty()) {
                synthTmp = (fs::temp_directory_path() / ("meco_" + std::to_string((long)::getpid()) + "_new.v")).string();
                netl = synthTmp;
            }
            auto q = [](const std::string& s) {
                std::string r = "'";
                for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
                return r + "'";
            };
            std::string cmd = q(msynth);
            for (auto& f : rtlF) cmd += " " + q(f);
            cmd += " -top " + q(t) + " -lib " + q(libF[0]) + " -o " + q(netl) + " -q -threads " + std::to_string(opt.threads) +
                   " > " + q(netl + ".log") + " 2>&1";
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                std::string all;
                {
                    std::ifstream log(netl + ".log");
                    all.assign((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
                }
                std::error_code ec;
                if (synthOut.empty()) fs::remove(netl + ".log", ec), fs::remove(netl, ec);
                if (all.size() > 2000) all = all.substr(all.size() - 2000);
                throw eco::Error("msynth failed on the revised RTL (" + msynth + "):\n" + all);
            }
            std::error_code ec;
            if (synthOut.empty()) fs::remove(netl + ".log", ec);
            newF.push_back(netl);
            if (newTop.empty()) newTop = t;
        }
        gnl::Module mo, mn;  // the two netlists are read side by side
        par::parallelFor(2, opt.threads, [&](size_t i) {
            gnl::Source src;
            for (auto& f : i ? newF : oldF) gnl::read(f, src);
            const std::string& t = i ? (newTop.empty() ? top : newTop) : (oldTop.empty() ? top : oldTop);
            (i ? mn : mo) = eco::flatModule(src, pickTop(src, t, i ? "new" : "old"), lib);
        });
        if (!synthTmp.empty()) { std::error_code ec; std::filesystem::remove(synthTmp, ec); }
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
        if (opt.spare) {
            std::snprintf(line, sizeof line, "meco: metal-only: %zu spare cells used, %zu old cells left as spares, %zu cells without a spare\n",
                          r.spares, r.freed, r.unplaced.size());
            rep << line;
            if (!quiet)
                for (auto& u : r.unplaced) rep << "  no spare cell for " << u << "\n";
        }
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
        return !r.verified ? 1 : r.unplaced.empty() ? 0 : 3;
    } catch (std::exception& e) {
        std::fprintf(stderr, "meco: %s\n", e.what());
        return 2;
    }
}
