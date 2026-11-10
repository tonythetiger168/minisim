// mpss.cpp - portable stimulus: reads a model in the Accellera Portable Test
// and Stimulus Standard DSL (PSS), solves scenarios from it, and writes each
// as a UVM sequence (for msim -uvm or any simulator) and as a bare-metal C
// test function.
//
//   mpss model.pss -n 20 -seed 1 -sv tests_pkg.sv -c tests.c
//
// One model gives both outputs: the same solved scenario, with the same
// field values, runs in simulation through UVM agents and on the processor
// as C. See docs/MPSS.md.
//
// Scenario i depends only on the model, -seed and i. The scenarios are
// solved on all cores (-threads N) and written in order, so the output is
// the same for every thread count.
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "cpu.hpp"
#include "pss/gen.hpp"
#include "version.hpp"

namespace {

void usage() {
    std::cout <<
        "usage: mpss <model.pss>... [options]\n"
        "  -top <component>    root component (default: pss_top)\n"
        "  -root <action>      root action (default: <top>::entry, or its only compound action)\n"
        "  -n <count>          scenarios to generate (default: 1)\n"
        "  -seed <n>           random seed (default: 1)\n"
        "  -name <base>        base name of the generated code (default: the root action's name)\n"
        "  -sv <file>          write a package of UVM sequences, one class per scenario\n"
        "  -c <file>           write C test functions, one per scenario\n"
        "  -json <file>        write the solved scenarios as JSON\n"
        "  -print              print the solved scenarios\n"
        "  -check              read and elaborate the model only\n"
        "  -threads <n>        worker threads, or all (default: every logical CPU)\n"
        "  -q                  no summary line\n"
        "  -version, -h\n";
}

struct Result {
    bool ok = false;
    std::string err, text, c, sv, json;
    size_t actions = 0;
    int attempts = 0;
};

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream o(path, std::ios::binary);
    o << text;
    o.close();
    if (!o) {
        std::cerr << "mpss: cannot write " << path << "\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> files;
    std::string top = "pss_top", rootName, base, svOut, cOut, jsonOut;
    long count = 1;
    uint64_t seed = 1;
    int threads = 0;
    bool print = false, check = false, quiet = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mpss: " << a << " needs " << what << "\n";
                exit(2);
            }
            return argv[++i];
        };
        auto number = [&](const char* what) -> long long {
            std::string v = next(what);
            try {
                size_t pos;
                long long n = std::stoll(v, &pos, 0);
                if (pos == v.size()) return n;
            } catch (...) {}
            std::cerr << "mpss: bad " << a << " " << v << "\n";
            exit(2);
        };
        if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
        if (a == "-version" || a == "--version") { std::cout << "mpss " << MINISIM_VERSION << "\n"; return 0; }
        if (a == "-top") top = next("a component name");
        else if (a == "-root") rootName = next("an action name");
        else if (a == "-name") base = next("a name");
        else if (a == "-sv") svOut = next("a file name");
        else if (a == "-c") cOut = next("a file name");
        else if (a == "-json") jsonOut = next("a file name");
        else if (a == "-n") { count = number("a count"); if (count < 1) { std::cerr << "mpss: -n must be at least 1\n"; return 2; } }
        else if (a == "-seed") seed = (uint64_t)number("a number");
        else if (a == "-threads") {
            if (i + 1 < argc && std::string(argv[i + 1]) == "all") { i++; threads = 0; }
            else { threads = (int)number("a count"); if (threads < 1) { std::cerr << "mpss: -threads must be at least 1\n"; return 2; } }
        }
        else if (a == "-print") print = true;
        else if (a == "-check") check = true;
        else if (a == "-q") quiet = true;
        else if (a.size() > 1 && a[0] == '-') { std::cerr << "mpss: unknown option " << a << " (-h lists them)\n"; return 2; }
        else files.push_back(a);
    }
    if (files.empty()) { usage(); return 2; }

    auto t0 = std::chrono::steady_clock::now();
    pss::Model model;
    pss::Design design(model);
    try {
        for (auto& f : files) pss::parseFile(model, f);
        design.elaborate(top, rootName);
    } catch (pss::Error& e) {
        std::cerr << "mpss: " << e.what() << "\n";
        return 1;
    }
    for (auto& w : model.warnings) std::cerr << "mpss: warning: " << w << "\n";
    if (base.empty()) base = pss::ident(design.rootAction->name);
    base = pss::ident(base);
    if (check) {
        if (!quiet)
            std::cout << "mpss: " << model.all.size() << " types, " << design.comps.size() << " component instances, "
                      << design.pools.size() << " pools, root action " << design.rootAction->qname << "\n";
        return 0;
    }

    pss::FuncInfo fi(model);
    int hw = std::max(1, cpuTopo().logical());
    if (threads == 0 || threads > hw) threads = hw;
    threads = (int)std::min<long>(threads, count);

    std::vector<Result> res((size_t)count);
    std::atomic<long> nextJob{0};
    std::atomic<bool> stop{false};
    auto work = [&] {
        for (;;) {
            long i = nextJob.fetch_add(1, std::memory_order_relaxed);
            if (i >= count || stop.load(std::memory_order_relaxed)) return;
            Result& r = res[(size_t)i];
            try {
                auto s = pss::generate(design, seed, (int)i, r.err);
                if (!s) { stop = true; continue; }
                r.actions = s->ais.size();
                r.attempts = s->attempt + 1;
                if (print) r.text = pss::listing(*s);
                if (!jsonOut.empty()) r.json = pss::json(*s);
                if (!cOut.empty()) r.c = pss::cScenario(*s, fi, base);
                if (!svOut.empty()) r.sv = pss::svScenario(*s, fi, base);
                r.ok = true;
            } catch (pss::Error& e) {
                r.err = e.what();
                stop = true;
            }
        }
    };
    // The tool line's shared pool (src/parallel.hpp, namespace par) is not on
    // main yet; this loop moves onto it when it is.
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; t++) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();

    for (long i = 0; i < count; i++)
        if (!res[(size_t)i].ok && !res[(size_t)i].err.empty()) {
            std::cerr << "mpss: scenario " << i << ": " << res[(size_t)i].err << "\n";
            return 1;
        }

    std::string banner = "Generated by mpss " MINISIM_VERSION " from";
    for (auto& f : files) {
        size_t p = f.find_last_of("/\\");
        banner += " " + (p == std::string::npos ? f : f.substr(p + 1));
    }
    banner += ": root " + design.rootAction->qname + ", " + std::to_string(count) + " scenario" + (count == 1 ? "" : "s") +
              ", seed " + std::to_string(seed) + ". Do not edit.";
    size_t actions = 0;
    std::string c, sv, js = "[";
    try {
        if (!cOut.empty()) c = pss::cPrelude(design, fi, base, banner);
        if (!svOut.empty()) sv = pss::svPrelude(design, fi, base, banner);
    } catch (pss::Error& e) {
        std::cerr << "mpss: " << e.what() << "\n";
        return 1;
    }
    for (long i = 0; i < count; i++) {
        Result& r = res[(size_t)i];
        actions += r.actions;
        if (print) std::cout << r.text;
        c += r.c;
        sv += r.sv;
        if (!jsonOut.empty()) js += (i ? ",\n" : "\n") + r.json;
    }
    if (!cOut.empty() && !writeFile(cOut, c + pss::cPostlude(base, (int)count))) return 1;
    if (!svOut.empty() && !writeFile(svOut, sv + pss::svPostlude(base))) return 1;
    if (!jsonOut.empty() && !writeFile(jsonOut, js + "\n]\n")) return 1;
    if (!quiet) {
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cerr << "mpss: " << count << " scenario" << (count == 1 ? "" : "s") << ", " << actions << " actions, "
                  << threads << " thread" << (threads == 1 ? "" : "s") << ", " << sec << " s\n";
    }
    return 0;
}
