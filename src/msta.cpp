// msta.cpp - a static timing analyzer for gate-level netlists, in the style
// of PrimeTime: Liberty libraries, a Verilog netlist, SDC constraints in;
// timing reports and SDF out.
//
//   msta -lib cells.lib netlist.v -top top -sdc top.sdc
//   msta -f run.tcl
//
// Without a script msta links the design, reads the constraints and prints
// check_timing, the worst setup and hold path of every clock, and the
// constraint summary. With a script (Tcl, the same reader the SDC files go
// through) the commands are:
//
//   read_liberty FILE            (also read_lib)
//   read_verilog FILE
//   link_design [TOP]            (also link)
//   read_sdc FILE                and every SDC command directly
//   read_spef FILE [-corner min|typ|max]
//   check_timing [-verbose]
//   report_timing [-delay_type max|min] [-max_paths N] [-group CLOCK]
//                 [-from OBJS] [-through OBJS] [-to OBJS]
//                 [-slack_lesser_than X] [-path full|end] [-input_pins]
//                 [-transition] [-capacitance] [-significant_digits N]
//   report_constraint [-all_violators]
//   report_clocks, report_qor, report_units
//   write_sdf FILE, write_json FILE [-max_paths N]
//   set_threads N
// A report command takes "> FILE" or ">> FILE" at the end.
//
// The exit status is 0 when everything read cleanly, 1 for errors, and with
// -fail_on_violation 2 when a timing or design rule constraint is violated.
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>

#include "sta_report.hpp"
#include "sta_spef.hpp"
#include "version.hpp"

namespace {

struct Shell {
    std::vector<std::unique_ptr<liberty::Library>> libraries;
    gnl::Source source;
    gnl::Netlist netlist;
    std::unique_ptr<sta::Sta> engine;
    std::unique_ptr<sta::SdcDesign> design;
    std::unique_ptr<par::Pool> pool;
    sdc::Reader reader;
    bool presetClearArcs = false;
    size_t seenCommands = 0, seenDiags = 0;
    int threads = 0;
    int errors = 0;
    int violations = 0;
    bool quiet = false;

    std::chrono::steady_clock::time_point mark = std::chrono::steady_clock::now();
    // Seconds since the last call.
    double secs() {
        auto now = std::chrono::steady_clock::now();
        double d = std::chrono::duration<double>(now - mark).count();
        mark = now;
        return d;
    }

    [[noreturn]] void fail(const std::string& m) { throw sdc::Error(m); }

    sta::Sta& sta() {
        if (!engine) fail("no design is linked: use read_liberty, read_verilog and link_design first");
        return *engine;
    }
    // Brings the engine up to date with the constraints read so far.
    sta::Sta& timed() {
        sta::Sta& s = sta();
        if (seenCommands != reader.sdc.commands.size() || !s.timed) {
            seenCommands = reader.sdc.commands.size();
            s.warnings.erase(std::remove_if(s.warnings.begin(), s.warnings.end(),
                                            [](const sta::Warning& w) { return w.code != "no-cell" && w.code != "no-pin" && w.code != "multi-driver"; }),
                             s.warnings.end());
            s.phase("constrain", [&] { s.constrain(reader.sdc); });
        }
        if (!pool || (int)pool->threads() != wantThreads()) pool = std::make_unique<par::Pool>((unsigned)wantThreads());
        s.pool = pool.get();
        s.update();
        return s;
    }
    int wantThreads() const {
        int n = threads > 0 ? threads : (int)par::defaultThreads();
        return n > 0 ? n : 1;
    }

    void link(const std::string& topArg) {
        std::string top = topArg;
        if (top.empty()) {
            auto t = gnl::tops(source);
            if (t.empty()) fail("link_design: no netlist has been read");
            if (t.size() > 1) {
                std::string all;
                for (auto& x : t) all += " " + x;
                fail("link_design: more than one top module, name one of:" + all);
            }
            top = t[0];
        }
        double readT = secs();
        engine = std::make_unique<sta::Sta>();
        engine->presetClearArcs = presetClearArcs;
        engine->phases.push_back({"read", readT});
        for (auto& l : libraries) engine->addLibrary(*l);
        auto busPins = [this](const std::string& cell, const std::string& pin, int) {
            std::vector<std::string> r;
            auto it = engine->cellByName.find(cell);
            if (it != engine->cellByName.end())
                for (auto& p : it->second->pins)
                    if (p.bus == pin) r.push_back(p.name);
            return r;
        };
        auto pinOrder = [this](const std::string& cell) {
            std::vector<std::string> r;
            auto it = engine->cellByName.find(cell);
            if (it != engine->cellByName.end())
                for (auto& p : it->second->pins) r.push_back(p.name);
            return r;
        };
        try {
            netlist = gnl::flatten(source, top, busPins, pinOrder);
        } catch (gnl::Error& e) {
            engine.reset();
            fail(e.what());
        }
        for (auto& w : netlist.warnings) std::cerr << "Warning: " << w << "\n";
        engine->phases.push_back({"flatten", secs()});
        if (!pool || (int)pool->threads() != wantThreads()) pool = std::make_unique<par::Pool>((unsigned)wantThreads());
        engine->pool = pool.get();
        engine->build(netlist);
        engine->phases.push_back({"build graph", secs()});
        design = std::make_unique<sta::SdcDesign>(*engine);
        reader.design = design.get();
        seenCommands = (size_t)-1;
    }

    // Options of a host command: flags and values, and what is left over.
    struct Args {
        std::map<std::string, std::string> opt;
        std::vector<std::string> pos;
        std::string redirect;
        bool append = false;
        bool has(const std::string& k) const { return opt.count(k) != 0; }
        std::string get(const std::string& k, const std::string& d = "") const {
            auto it = opt.find(k);
            return it == opt.end() ? d : it->second;
        }
    };
    Args args(const std::vector<std::string>& w, const std::set<std::string>& valued, const std::set<std::string>& flags) {
        Args a;
        for (size_t i = 1; i < w.size(); i++) {
            const std::string& x = w[i];
            if ((x == ">" || x == ">>") && i + 1 < w.size()) {
                a.redirect = w[++i];
                a.append = x == ">>";
            } else if (valued.count(x)) {
                if (i + 1 >= w.size()) fail(w[0] + ": " + x + " needs a value");
                a.opt[x] = w[++i];
            } else if (flags.count(x)) {
                a.opt[x] = "1";
            } else if (!x.empty() && x[0] == '-' && x.size() > 1 && !isdigit((unsigned char)x[1])) {
                fail(w[0] + ": unknown option " + x);
            } else {
                a.pos.push_back(x);
            }
        }
        return a;
    }
    // Runs a report into the console or the file the command redirects to.
    template <class F>
    void emit(const Args& a, F&& f) {
        if (a.redirect.empty()) {
            f(std::cout);
            std::cout.flush();
            return;
        }
        std::ofstream out(a.redirect, a.append ? std::ios::app : std::ios::trunc);
        if (!out) fail("cannot write " + a.redirect);
        f(out);
    }

    void objects(const std::string& word, std::unordered_set<int>& nodes, std::unordered_set<int>& clks) {
        sta::Sta& s = sta();
        for (auto& o : reader.objects(word)) {
            if (o.kind == sdc::Kind::Clock) {
                int k = s.clockIdx(o.name);
                if (k >= 0) clks.insert(k);
            } else {
                std::vector<int> ns;
                s.nodesOf(o, ns);
                nodes.insert(ns.begin(), ns.end());
            }
        }
    }

    void install() {
        reader.onPuts = [](const std::string& s) { std::cout << s << "\n"; };
        auto readLib = [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {}, {});
            if (a.pos.size() != 1) fail(w[0] + " FILE");
            try {
                libraries.push_back(std::make_unique<liberty::Library>(liberty::load(a.pos[0])));
            } catch (liberty::Error& e) {
                fail(e.what());
            }
            return "";
        };
        reader.addCommand("read_liberty", readLib);
        reader.addCommand("read_lib", readLib);
        reader.addCommand("read_verilog", [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {}, {});
            if (a.pos.empty()) fail("read_verilog FILE");
            try {
                for (auto& f : a.pos) gnl::read(f, source, wantThreads());
            } catch (gnl::Error& e) {
                fail(e.what());
            }
            return "";
        });
        auto linkCmd = [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {}, {});
            link(a.pos.empty() ? "" : a.pos[0]);
            return netlist.top;
        };
        reader.addCommand("link_design", linkCmd);
        reader.addCommand("link", linkCmd);
        reader.addCommand("read_sdc", [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {}, {"-echo"});
            if (a.pos.size() != 1) fail("read_sdc FILE");
            sta();
            reader.load(a.pos[0]);
            return "";
        });
        auto readSpef = [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {"-corner", "-path"}, {"-quiet", "-keep_capacitive_coupling"});
            if (a.pos.size() != 1) fail(w[0] + " FILE");
            sta::Sta& s = sta();
            if (!pool || (int)pool->threads() != wantThreads()) pool = std::make_unique<par::Pool>((unsigned)wantThreads());
            s.pool = pool.get();
            std::string cn = a.get("-corner", "typ");
            int corner = cn == "min" ? 0 : cn == "max" ? 2 : 1;
            sta::SpefResult r;
            try {
                spef::File f = spef::read(a.pos[0], wantThreads(), corner);
                r = sta::annotateSpef(s, f);
            } catch (std::exception& e) {
                fail(e.what());
            }
            if (!a.has("-quiet")) {
                std::cout << "read_spef: " << r.annotated << " of " << s.nl->nets.size() << " nets annotated";
                if (r.unknown) std::cout << ", " << r.unknown << " SPEF nets are not in the design";
                if (r.noDriver) std::cout << ", " << r.noDriver << " with no driver in their RC network (capacitance only)";
                if (r.loops) std::cout << ", " << r.loops << " with resistor loops (timed along a tree)";
                std::cout << "\n";
            }
            return std::to_string(r.annotated);
        };
        reader.addCommand("read_spef", readSpef);
        reader.addCommand("read_parasitics", readSpef);
        reader.addCommand("set_threads", [this](const std::vector<std::string>& w) -> std::string {
            if (w.size() != 2) fail("set_threads N");
            threads = std::atoi(w[1].c_str());
            return "";
        });
        reader.addCommand("update_timing", [this](const std::vector<std::string>&) -> std::string {
            timed();
            return "";
        });
        reader.addCommand("check_timing", [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {}, {"-verbose"});
            sta::Reporter r(timed());
            int n = 0;
            emit(a, [&](std::ostream& os) { n = r.checkTiming(a.has("-verbose"), os); });
            return std::to_string(n);
        });
        auto reportTiming = [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {"-delay_type", "-path_delay", "-max_paths", "-nworst", "-group", "-path_group", "-from", "-to", "-through",
                              "-slack_lesser_than", "-path", "-path_type", "-format", "-significant_digits", "-digits", "-group_count",
                              "-endpoint_count", "-unique_paths_to_endpoint", "-group_path_count", "-endpoint_path_count"},
                          {"-input_pins", "-transition", "-capacitance", "-nets", "-nosplit", "-unconstrained"});
            sta::Sta& s = timed();
            sta::ReportOpts o;
            std::string dt = a.get("-delay_type", a.get("-path_delay", "max"));
            if (dt == "max" || dt == "setup") o.late = true;
            else if (dt == "min" || dt == "hold") o.late = false;
            else fail("report_timing: -delay_type is max or min");
            o.maxPaths = std::atoi(a.get("-max_paths", a.get("-group_count", a.get("-group_path_count", "1"))).c_str());
            if (o.maxPaths < 1) o.maxPaths = 1;
            o.digits = std::atoi(a.get("-significant_digits", a.get("-digits", "3")).c_str());
            if (o.digits < 0 || o.digits > 9) o.digits = 3;
            o.group = a.get("-group", a.get("-path_group"));
            if (a.has("-slack_lesser_than")) o.slackLess = std::atof(a.get("-slack_lesser_than").c_str()) * s.timeUnit;
            std::string path = a.get("-path", a.get("-path_type", a.get("-format", "full")));
            o.endOnly = path == "end" || path == "end_slack_only" || path == "summary";
            o.inputPins = a.has("-input_pins") || path == "full_clock_expanded";
            o.showTrans = a.has("-transition");
            o.showCap = a.has("-capacitance");
            if (a.has("-from")) { o.hasFrom = true; objects(a.get("-from"), o.from, o.fromClks); }
            if (a.has("-to")) { o.hasTo = true; objects(a.get("-to"), o.to, o.toClks); }
            if (a.has("-through")) {
                std::unordered_set<int> none;
                o.hasThrough = true;
                objects(a.get("-through"), o.through, none);
            }
            sta::Reporter r(s);
            emit(a, [&](std::ostream& os) { r.reportTiming(o, os); });
            return "";
        };
        reader.addCommand("report_timing", reportTiming);
        reader.addCommand("report_checks", reportTiming);
        reader.addCommand("report_constraint", [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {"-significant_digits"}, {"-all_violators", "-max_delay", "-min_delay", "-max_transition",
                                                      "-max_capacitance", "-nosplit", "-verbose"});
            sta::Reporter r(timed());
            int n = 0;
            emit(a, [&](std::ostream& os) {
                n = r.reportConstraint(a.has("-all_violators") || a.has("-verbose"),
                                       std::atoi(a.get("-significant_digits", "3").c_str()), os);
            });
            violations = n;
            return std::to_string(n);
        });
        reader.addCommand("report_clocks", [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {}, {});
            sta::Reporter r(timed());
            emit(a, [&](std::ostream& os) { r.reportClocks(os); });
            return "";
        });
        reader.addCommand("report_clock", reader.hostCommands["report_clocks"]);
        reader.addCommand("report_qor", [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {}, {});
            sta::Reporter r(timed());
            emit(a, [&](std::ostream& os) { r.reportQor(os); });
            return "";
        });
        reader.addCommand("report_units", [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {}, {});
            sta::Sta& s = sta();
            emit(a, [&](std::ostream& os) {
                os << "  time         " << s.timeUnit << " ns\n  capacitance  " << s.capUnit << " pF\n";
            });
            return "";
        });
        reader.addCommand("write_sdf", [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {"-version", "-divider"}, {"-no_edge", "-include"});
            if (a.pos.size() != 1) fail("write_sdf FILE");
            sta::Reporter r(timed());
            std::ofstream out(a.pos[0]);
            if (!out) fail("cannot write " + a.pos[0]);
            r.writeSdf(out, MINISIM_VERSION);
            return "";
        });
        reader.addCommand("write_json", [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {"-max_paths"}, {});
            if (a.pos.size() != 1) fail("write_json FILE");
            sta::Reporter r(timed());
            std::ofstream out(a.pos[0]);
            if (!out) fail("cannot write " + a.pos[0]);
            r.writeJson(out, std::atoi(a.get("-max_paths", "10").c_str()));
            return "";
        });
        // Slack queries for scripts: the worst slack and the total negative slack.
        auto slack = [this](const std::vector<std::string>& w) -> std::string {
            Args a = args(w, {}, {"-max", "-min"});
            sta::Sta& s = timed();
            sta::Sta::Summary sm = s.summary(!a.has("-min"));
            char b[64];
            std::snprintf(b, sizeof b, "%.6g", (w[0] == "total_negative_slack" ? sm.tns : sm.wns) / s.timeUnit);
            return b;
        };
        reader.addCommand("worst_slack", slack);
        reader.addCommand("total_negative_slack", slack);
        auto quit = [this](const std::vector<std::string>&) -> std::string { throw Quit{}; };
        reader.addCommand("exit", quit);
        reader.addCommand("quit", quit);
    }
    struct Quit {};

    // Prints the diagnostics the reader has gathered since the last call.
    void flushDiags() {
        for (; seenDiags < reader.diags.size(); seenDiags++) {
            auto& d = reader.diags[seenDiags];
            bool err = d.sev == sdc::Diag::ErrorSev;
            if (err) errors++;
            if (err || !quiet)
                std::cerr << (err ? "Error: " : "Warning: ") << d.loc.str() << ": " << d.text << " (" << d.code << ")\n";
        }
    }
    void run(const std::string& text, const std::string& file) {
        reader.parse(text, file);
        flushDiags();
    }
};

std::string tclWord(const std::string& s) {
    std::string r = "{";
    return r + s + "}";
}

void usage() {
    std::cout <<
        "usage: msta [options] [netlist.v ...] [script.tcl]\n"
        "  -lib FILE            a Liberty library (repeat for more)\n"
        "  -top NAME            the top module (default: the one nothing instantiates)\n"
        "  -sdc FILE            constraints (repeat for more)\n"
        "  -spef FILE           parasitics: wire capacitance and Elmore wire delay\n"
        "  -f FILE              run a Tcl script instead of the default reports\n"
        "  -e COMMAND           run one command after the design is read\n"
        "  -max_paths N         paths per clock in the default reports (1)\n"
        "  -sdf FILE            write the delays and timing checks as SDF\n"
        "  -json FILE           write the summary and the worst paths as JSON\n"
        "  -threads N           worker threads (default: every logical core; 1: none)\n"
        "  -preset_clear_arcs   time paths through asynchronous set/reset to output arcs\n"
        "  -fail_on_violation   exit 2 when a constraint is violated\n"
        "  -q                   no warnings from the constraint reader\n"
        "  -version, -h\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> libs, netlists, sdcs, scripts, cmds, spefs;
    std::string top, sdf, json;
    int maxPaths = 1;
    bool failOnViolation = false, stats = false;
    Shell sh;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "msta: " << a << " needs a value\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "-lib" || a == "-liberty") libs.push_back(val());
        else if (a == "-top") top = val();
        else if (a == "-sdc") sdcs.push_back(val());
        else if (a == "-spef") spefs.push_back(val());
        else if (a == "-f" || a == "-file") scripts.push_back(val());
        else if (a == "-e") cmds.push_back(val());
        else if (a == "-max_paths") maxPaths = std::atoi(val().c_str());
        else if (a == "-sdf") sdf = val();
        else if (a == "-json") json = val();
        else if (a == "-threads") sh.threads = std::atoi(val().c_str());
        else if (a == "-preset_clear_arcs") sh.presetClearArcs = true;
        else if (a == "-fail_on_violation") failOnViolation = true;
        else if (a == "-stats") stats = true;
        else if (a == "-q") sh.quiet = true;
        else if (a == "-version" || a == "--version") { std::cout << "msta " << MINISIM_VERSION << "\n"; return 0; }
        else if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') { std::cerr << "msta: unknown option " << a << "\n"; return 1; }
        else if (a.size() > 4 && (a.substr(a.size() - 4) == ".tcl")) scripts.push_back(a);
        else if (a.size() > 4 && (a.substr(a.size() - 4) == ".lib")) libs.push_back(a);
        else if (a.size() > 4 && (a.substr(a.size() - 4) == ".sdc")) sdcs.push_back(a);
        else netlists.push_back(a);
    }
    if (libs.empty() && netlists.empty() && scripts.empty()) {
        usage();
        return 1;
    }
    sh.install();
    auto t0 = std::chrono::steady_clock::now();
    int rc = 0;
    try {
        std::string pre;
        for (auto& l : libs) pre += "read_liberty " + tclWord(l) + "\n";
        for (auto& n : netlists) pre += "read_verilog " + tclWord(n) + "\n";
        if (!netlists.empty()) pre += "link_design " + (top.empty() ? "" : tclWord(top)) + "\n";
        if (!pre.empty()) sh.run(pre, "<command line>");
        if (sh.errors) return 1;
        for (auto& f : sdcs) sh.run("read_sdc " + tclWord(f) + "\n", "<command line>");
        for (auto& f : spefs) sh.run("read_spef " + tclWord(f) + "\n", "<command line>");
        if (scripts.empty() && cmds.empty() && sh.engine) {
            std::string mp = std::to_string(maxPaths < 1 ? 1 : maxPaths);
            sh.run("check_timing\nputs {}\nreport_timing -delay_type max -max_paths " + mp +
                   "\nreport_timing -delay_type min -max_paths " + mp + "\nreport_constraint\n", "<default>");
        }
        for (auto& f : scripts) sh.run("source " + tclWord(f) + "\n", "<command line>");
        for (auto& c : cmds) sh.run(c + "\n", "<-e>");
        if (!sdf.empty()) sh.run("write_sdf " + tclWord(sdf) + "\n", "<command line>");
        if (!json.empty()) sh.run("write_json " + tclWord(json) + " -max_paths " + std::to_string(std::max(maxPaths, 10)) + "\n", "<command line>");
        if (failOnViolation && sh.engine) {
            sta::Sta& s = sh.timed();
            if (s.summary(true).violations || s.summary(false).violations || !s.transViol.empty() || !s.capViol.empty()) rc = 2;
        }
    } catch (Shell::Quit&) {
        sh.flushDiags();
    } catch (std::exception& e) {
        sh.flushDiags();
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    if (stats && sh.engine) {
        auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        sta::Sta& s = *sh.engine;
        size_t ntags = 0;
        for (auto& tv : s.tags) ntags += tv.size();
        std::cerr << "msta: " << s.nl->insts.size() << " cells, " << s.nodes.size() << " pins, " << s.edges.size() << " arcs, "
                  << s.levels.size() << " levels, " << ntags << " tags, " << s.ends.size() << " path ends, "
                  << sh.wantThreads() << " threads, " << dt << " s\n";
        for (auto& [what, t] : s.phases) std::cerr << "  " << what << ": " << t << " s\n";
    }
    if (sh.errors) return 1;
    return rc;
}
