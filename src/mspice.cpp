// mspice.cpp - a SPICE circuit simulator for the minisim tool line.
//
//   mspice amp.cir                 run the analyses in the netlist
//   mspice amp.cir -o out          also write out.<analysis>.csv (every point)
//   mspice amp.cir -vcd w.vcd      transient waveforms as a VCD of real values
//   mspice amp.cir -threads 4      worker threads (default: every logical CPU)
//   mspice amp.cir -D rload=2k     override a .param
//
// Netlist: R C L K V I E G F H D M Q X, .model .param .subckt .include
// .global .option .temp .ic .nodeset, analyses .op .dc .ac .tran, outputs
// .print .measure, and .step to repeat everything over parameter values.
// See docs/SPICE.md.
//
// Threads: the runs of a .step, the frequency points of an .ac and the model
// evaluation of large circuits are spread over the workers. Results are
// stored by index and matrices are summed in netlist order, so the output is
// the same bit for bit at any -threads.
#include <chrono>
#include <cinttypes>
#include <iostream>

#include "cpu.hpp"
#include "spice.hpp"
#include "version.hpp"

using namespace spice;

namespace {

std::string num(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6e", v);
    return b;
}

struct Args {
    std::string file, csv, vcd;
    int threads = 0;
    bool quiet = false, stats = false;
    std::map<std::string, double> defines;
    bool flatten = false;
    std::string top;
};

void usage() {
    std::cout << "usage: mspice [options] netlist.cir\n"
                 "  -o <prefix>    write <prefix>.<analysis>.csv with every computed point\n"
                 "  -vcd <file>    write the transient waveforms as a VCD (real variables)\n"
                 "  -threads <n>   worker threads, or all (default: every logical CPU)\n"
                 "  -D name=value  set a .param (overrides the netlist)\n"
                 "  -q             print measurements and errors only\n"
                 "  -stats         print iteration and time step counts\n"
                 "  -flatten       print the elements with flat names and stop (no simulation)\n"
                 "  -top <name>    with -flatten: the subcircuit to flatten (default: the top level)\n"
                 "  -version       print the version\n";
}

std::string stepLabel(const StepPlan& plan, size_t k) {
    std::string s;
    for (size_t i = 0; i < plan.names.size(); i++) s += (i ? " " : "") + plan.names[i] + "=" + num(plan.sets[k][i]);
    return s;
}

void printTable(const Result& r) {
    std::vector<int> cols;
    for (auto& p : r.printed) cols.push_back(r.col(p));
    std::printf("%-14s", r.xname.c_str());
    for (auto& p : r.printed) std::printf(" %-14s", p.c_str());
    std::printf("\n");
    auto row = [&](double xv, const std::function<double(int)>& y) {
        std::printf("%-14s", num(xv).c_str());
        for (int c : cols) std::printf(" %-14s", num(y(c)).c_str());
        std::printf("\n");
    };
    if (r.kind == "tran" && r.tstep > 0 && !r.x.empty()) {
        // SPICE prints a transient on the tstep grid, not at the solver's own points.
        double t0 = r.x.front(), t1 = r.x.back();
        size_t n = (size_t)std::floor((t1 - t0) / r.tstep + 1e-9);
        for (size_t i = 0; i <= n; i++) {
            double t = t0 + i * r.tstep;
            row(t, [&](int c) { return r.at(c, t); });
        }
    } else {
        for (size_t i = 0; i < r.x.size(); i++) row(r.x[i], [&](int c) { return r.y[i][c]; });
    }
}

void report(const RunOutput& out, const Args& a) {
    for (const Result& r : out.results) {
        if (!a.quiet) {
            if (r.kind == "op") {
                std::printf("operating point\n");
                for (size_t i = 0; i < r.cols.size(); i++) std::printf("  %-20s = %s\n", r.cols[i].c_str(), num(r.y[0][i]).c_str());
            } else if (!r.printed.empty()) {
                std::printf("%s analysis\n", r.kind.c_str());
                printTable(r);
            } else {
                std::printf("%s analysis: %zu points\n", r.kind.c_str(), r.x.size());
            }
        }
        if (a.stats)
            std::printf("  %s: %zu Newton iterations, %zu steps accepted, %zu rejected\n", r.kind.c_str(), r.iterations,
                        r.accepted, r.rejected);
        for (auto& [name, v] : r.meas) {
            if (std::isfinite(v)) std::printf("%s = %s\n", name.c_str(), num(v).c_str());
            else std::printf("%s = failed\n", name.c_str());
        }
    }
}

// One CSV per analysis; with .step, the runs follow one another with the
// stepped parameters in the first columns.
void writeCsv(const std::string& prefix, const StepPlan& plan, const std::vector<RunOutput>& runs) {
    std::map<std::string, int> seen;
    for (size_t ai = 0; ai < runs[0].results.size(); ai++) {
        const Result& first = runs[0].results[ai];
        int k = ++seen[first.kind];
        std::string path = prefix + "." + first.kind + (k > 1 ? std::to_string(k) : "") + ".csv";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) throw Error("cannot write " + path);
        for (auto& n : plan.names) std::fprintf(f, "%s,", n.c_str());
        if (!first.xname.empty()) std::fprintf(f, "%s,", first.xname.c_str());
        for (size_t c = 0; c < first.cols.size(); c++) {
            // v(a,b) has a comma of its own.
            bool q = first.cols[c].find(',') != std::string::npos;
            std::fprintf(f, "%s%s%s%s", q ? "\"" : "", first.cols[c].c_str(), q ? "\"" : "", c + 1 < first.cols.size() ? "," : "\n");
        }
        for (size_t s = 0; s < runs.size(); s++) {
            const Result& r = runs[s].results[ai];
            for (size_t i = 0; i < r.x.size(); i++) {
                for (double v : plan.sets[s]) std::fprintf(f, "%.9g,", v);
                if (!r.xname.empty()) std::fprintf(f, "%.12g,", r.x[i]);
                for (size_t c = 0; c < r.y[i].size(); c++) std::fprintf(f, "%.9g%s", r.y[i][c], c + 1 < r.y[i].size() ? "," : "\n");
            }
        }
        std::fclose(f);
    }
}

// The transient result as a VCD with real variables, for GTKWave or Surfer.
void writeVcd(const std::string& path, const Result& r, const std::string& title) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) throw Error("cannot write " + path);
    double tend = r.x.empty() ? 0 : r.x.back();
    // The finest unit that still keeps the end time well inside 64 bits.
    const char* unit = "1 fs";
    double tick = 1e-15;
    for (auto [u, t] : {std::pair<const char*, double>{"1 ps", 1e-12}, {"1 ns", 1e-9}, {"1 us", 1e-6}, {"1 ms", 1e-3}})
        if (tend / tick > 1e15) { unit = u; tick = t; }
    std::fprintf(f, "$comment mspice %s: %s $end\n$timescale %s $end\n$scope module mspice $end\n", MINISIM_VERSION,
                 title.c_str(), unit);
    auto id = [](size_t i) {
        std::string s;
        do { s += (char)('!' + i % 94); i /= 94; } while (i);
        return s;
    };
    for (size_t c = 0; c < r.cols.size(); c++) {
        // v(out) -> out, i(v1) -> i_v1, v(a,b) -> v_a_b
        std::string l = r.cols[c], nm;
        bool plain = l.rfind("v(", 0) == 0 && l.find(',') == std::string::npos;
        std::string body = plain ? l.substr(2, l.size() - 3) : l;
        for (char ch : body) nm += std::isalnum((unsigned char)ch) || ch == '_' || ch == '.' ? ch : '_';
        while (!nm.empty() && nm.back() == '_') nm.pop_back();
        std::fprintf(f, "$var real 64 %s %s $end\n", id(c).c_str(), nm.c_str());
    }
    std::fprintf(f, "$upscope $end\n$enddefinitions $end\n");
    std::vector<double> last(r.cols.size(), NAN);
    int64_t prev = -1;
    for (size_t i = 0; i < r.x.size(); i++) {
        int64_t t = (int64_t)std::llround(r.x[i] / tick);
        if (t == prev) continue;
        bool stamped = false;
        for (size_t c = 0; c < r.cols.size(); c++) {
            if (r.y[i][c] == last[c]) continue;
            if (!stamped) { std::fprintf(f, "#%" PRId64 "\n", t); stamped = true; }
            std::fprintf(f, "r%.16g %s\n", r.y[i][c], id(c).c_str());
            last[c] = r.y[i][c];
        }
        prev = t;
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "mspice: " << s << " needs a value\n"; std::exit(2); }
            return argv[++i];
        };
        auto define = [&](const std::string& d) {
            auto eq = d.find('=');
            if (eq == std::string::npos) { std::cerr << "mspice: -D needs name=value\n"; std::exit(2); }
            try { a.defines[lower(d.substr(0, eq))] = value(lower(d.substr(eq + 1)), nullptr); }
            catch (const Error& e) { std::cerr << "mspice: -D " << d << ": " << e.what() << "\n"; std::exit(2); }
        };
        if (s == "-h" || s == "-help" || s == "--help") { usage(); return 0; }
        if (s == "-version" || s == "--version" || s == "-v") {
            std::cout << "mspice " << MINISIM_VERSION << (MINISIM_GIT[0] ? " (" MINISIM_GIT ")" : "") << "\n";
            return 0;
        }
        if (s == "-o") a.csv = next();
        else if (s == "-vcd") a.vcd = next();
        else if (s == "-q") a.quiet = true;
        else if (s == "-stats") a.stats = true;
        else if (s == "-flatten") a.flatten = true;
        else if (s == "-top") a.top = next();
        else if (s == "-threads") {
            std::string n = next();
            if (n == "all") a.threads = 0;
            else {
                a.threads = std::atoi(n.c_str());
                if (a.threads < 1) { std::cerr << "mspice: -threads must be at least 1, or all\n"; return 2; }
            }
        } else if (s == "-D") define(next());
        else if (s.rfind("-D", 0) == 0) define(s.substr(2));
        else if (s[0] == '-') { std::cerr << "mspice: unknown option " << s << "\n"; usage(); return 2; }
        else if (a.file.empty()) a.file = s;
        else { std::cerr << "mspice: one netlist at a time (" << a.file << " and " << s << ")\n"; return 2; }
    }
    if (a.file.empty()) { usage(); return 2; }
    int hw = std::max(1, cpuTopo().logical());
    int threads = a.threads > 0 ? std::min(a.threads, hw) : (int)par::defaultThreads();

    try {
        auto t0 = std::chrono::steady_clock::now();
        Netlist nl = Reader().read(a.file);
        if (a.flatten) {
            Flat f = flatten(nl, a.top);
            if (!a.top.empty()) {
                std::printf(".ports");
                for (auto& p : f.ports) std::printf(" %s", p.c_str());
                std::printf("\n");
            }
            for (const Primitive& p : f.prims) {
                std::printf("%s", p.path.c_str());
                for (auto& n : p.nodes) std::printf(" %s", n.c_str());
                if (!p.model.empty()) std::printf(" %s", p.model.c_str());
                for (auto& [k, v] : p.params) std::printf(" %s=%s", k.c_str(), num(v).c_str());
                std::printf("\n");
            }
            return 0;
        }
        StepPlan plan = stepPlan(nl, a.defines);
        std::vector<RunOutput> runs(plan.sets.size());
        Pool pool(threads);
        pool.spin(20000);   // about 0.1 ms: one batch per Newton iteration, back to back
        auto overridesOf = [&](size_t k) {
            std::map<std::string, double> o = a.defines;
            for (size_t i = 0; i < plan.names.size(); i++) o[plan.names[i]] = plan.sets[k][i];
            return o;
        };
        if (runs.size() == 1) {
            runs[0] = runNetlist(nl, overridesOf(0), &pool);
        } else {
            // Each run builds its own circuit, so runs share nothing.
            pool.run(runs.size(), [&](size_t k) { runs[k] = runNetlist(nl, overridesOf(k), nullptr); });
        }
        if (!a.quiet) {
            std::printf("mspice %s: %s\n", MINISIM_VERSION, nl.title.c_str());
            std::printf("%zu nodes, %zu elements, %zu unknowns, %d thread%s\n", runs[0].nodes, runs[0].devices,
                        runs[0].unknowns, threads, threads == 1 ? "" : "s");
        }
        std::set<std::string> warned;
        for (auto& r : runs)
            for (auto& w : r.warnings)
                if (warned.insert(w).second) std::cerr << "mspice: warning: " << w << "\n";
        bool failed = false;
        for (size_t k = 0; k < runs.size(); k++) {
            if (runs.size() > 1) std::printf("step %zu of %zu: %s\n", k + 1, runs.size(), stepLabel(plan, k).c_str());
            report(runs[k], a);
            for (auto& r : runs[k].results)
                for (auto& m : r.meas)
                    if (!std::isfinite(m.second)) failed = true;
        }
        if (!a.csv.empty()) writeCsv(a.csv, plan, runs);
        if (!a.vcd.empty()) {
            bool any = false;
            for (size_t k = 0; k < runs.size(); k++)
                for (auto& r : runs[k].results) {
                    if (r.kind != "tran") continue;
                    std::string path = a.vcd;
                    if (runs.size() > 1) {
                        auto dot = path.find_last_of('.');
                        std::string tag = "." + std::to_string(k + 1);
                        path = dot == std::string::npos ? path + tag : path.substr(0, dot) + tag + path.substr(dot);
                    }
                    writeVcd(path, r, nl.title);
                    any = true;
                    break;
                }
            if (!any) std::cerr << "mspice: warning: -vcd needs a .tran analysis; nothing written\n";
        }
        if (a.stats) {
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("elapsed %.3f s\n", s);
        }
        return failed ? 1 : 0;
    } catch (const Error& e) {
        std::cerr << "mspice: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "mspice: internal error: " << e.what() << "\n";
        return 1;
    }
}
