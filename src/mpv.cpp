// mpv.cpp - physical verification: design rule checking on a GDSII layout.
//
//   mpv drc -deck rules.drc chip.gds [-top CELL] [-o results.txt] [-threads N]
//
// Reads the layout (gds.hpp), flattens the top cell (layout.hpp), runs every
// rule of the deck (drc.hpp), prints one line per rule and writes the
// violation markers to the results file. Exit status: 0 clean, 1 violations,
// 2 a usage or input error.
//
//   mpv lvs -deck tech.lvs chip.gds [-top CELL] [-o extracted.spice]
//
// Extracts the transistor netlist of the layout from the connect, label and
// device statements of the deck and writes it as a SPICE subcircuit.
//
// The checks run on all hardware threads by default (-threads N to choose);
// the report is the same for every thread count.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "lvs.hpp"
#include "version.hpp"

namespace {

int usage(FILE* f) {
    std::fputs(
        "usage: mpv drc -deck <rules> <layout.gds> [options]\n"
        "       mpv lvs -deck <rules> <layout.gds> [-top <cell>] [-o <extracted.spice>] [-threads <n>]\n"
        "  -deck <file>     rule deck (see docs/MPV.md)\n"
        "  -top <cell>      cell to check (default: the one top cell)\n"
        "  -o <file>        write the violation markers here\n"
        "  -max <n>         markers written per rule (default 1000; counts stay exact)\n"
        "  -threads <n>     worker threads (default: every hardware thread)\n"
        "  -q               print only rules with violations and the summary\n"
        "  -version\n", f);
    return f == stderr ? 2 : 0;
}

std::string um(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

int drcMain(int argc, char** argv) {
    std::string deckFile, gdsFile, top, outFile;
    unsigned threads = std::thread::hardware_concurrency();
    if (!threads) threads = 1;
    size_t maxMarkers = 1000;
    bool quiet = false;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](std::string& dst) {
            if (i + 1 >= argc) { std::fprintf(stderr, "mpv: %s needs a value\n", a.c_str()); return false; }
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "-deck") { if (!val(deckFile)) return 2; }
        else if (a == "-top") { if (!val(top)) return 2; }
        else if (a == "-o") { if (!val(outFile)) return 2; }
        else if (a == "-q") quiet = true;
        else if (a == "-threads" || a == "-max") {
            if (!val(v)) return 2;
            char* end = nullptr;
            long n = std::strtol(v.c_str(), &end, 10);
            if (*end || n < (a == "-max" ? 0 : 1)) { std::fprintf(stderr, "mpv: bad %s %s\n", a.c_str(), v.c_str()); return 2; }
            if (a == "-max") maxMarkers = size_t(n); else threads = unsigned(n);
        }
        else if (a == "-h" || a == "-help" || a == "--help") return usage(stdout);
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "mpv: unknown option %s\n", a.c_str()); return 2; }
        else if (gdsFile.empty()) gdsFile = a;
        else { std::fprintf(stderr, "mpv: more than one layout given (%s, %s)\n", gdsFile.c_str(), a.c_str()); return 2; }
    }
    if (deckFile.empty() || gdsFile.empty()) return usage(stderr);

    auto t0 = std::chrono::steady_clock::now();
    std::string err;
    drc::Deck deck;
    if (!drc::readDeck(deckFile, deck, err)) { std::fprintf(stderr, "mpv: %s\n", err.c_str()); return 2; }
    gds::Library lib;
    if (!gds::read(gdsFile, lib, err)) { std::fprintf(stderr, "mpv: %s\n", err.c_str()); return 2; }
    layout::Flat flat;
    if (!layout::flatten(lib, top, flat, err, threads)) { std::fprintf(stderr, "mpv: %s: %s\n", gdsFile.c_str(), err.c_str()); return 2; }

    drc::Engine eng(deck, flat, threads);
    std::vector<drc::RuleResult> res = eng.run();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("mpv drc: cell %s, %zu shapes on %zu layers, %s x %s um, 1 unit = %s um\n", flat.top.c_str(),
                flat.shapes, flat.layers.size(), um((flat.extent.x2 - double(flat.extent.x1)) * flat.dbu).c_str(),
                um((flat.extent.y2 - double(flat.extent.y1)) * flat.dbu).c_str(), um(flat.dbu).c_str());
    if (flat.skipped)
        std::printf("warning: %zu shapes or references have edges that are not at 0 or 90 degrees and were not checked\n",
                    flat.skipped);
    size_t idw = 4;
    for (const drc::Rule& r : deck.rules) idw = std::max(idw, r.id.size());
    size_t total = 0, failed = 0;
    for (size_t i = 0; i < res.size(); ++i) {
        size_t n = res[i].markers.size();
        total += n;
        failed += n != 0;
        if (quiet && !n) continue;
        std::printf("%-*s %8zu  %s\n", int(idw), deck.rules[i].id.c_str(), n, deck.rules[i].desc.c_str());
    }
    std::printf("%zu violation%s in %zu of %zu rules\n", total, total == 1 ? "" : "s", failed, res.size());
    std::fprintf(stderr, "mpv: %.2f s, %u thread%s\n", secs, threads, threads == 1 ? "" : "s");

    if (!outFile.empty()) {
        FILE* f = std::fopen(outFile.c_str(), "w");
        if (!f) { std::fprintf(stderr, "mpv: cannot write %s\n", outFile.c_str()); return 2; }
        // One "rule" line per rule, then one line per marker: the box
        // (x1 y1 x2 y2, microns) and the measured value.
        std::fprintf(f, "mpv-drc 1\ncell %s\n", flat.top.c_str());
        for (size_t i = 0; i < res.size(); ++i) {
            std::fprintf(f, "rule %s %zu \"%s\"\n", deck.rules[i].id.c_str(), res[i].markers.size(), deck.rules[i].desc.c_str());
            size_t n = std::min(maxMarkers, res[i].markers.size());
            for (size_t k = 0; k < n; ++k) {
                const drc::Marker& m = res[i].markers[k];
                std::fprintf(f, "  %s %s %s %s %s\n", um(m.box.x1 * flat.dbu).c_str(), um(m.box.y1 * flat.dbu).c_str(),
                             um(m.box.x2 * flat.dbu).c_str(), um(m.box.y2 * flat.dbu).c_str(), um(m.value).c_str());
            }
        }
        std::fclose(f);
    }
    return total ? 1 : 0;
}

int lvsMain(int argc, char** argv) {
    std::string deckFile, gdsFile, top, outFile;
    unsigned threads = std::thread::hardware_concurrency();
    if (!threads) threads = 1;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](std::string& dst) {
            if (i + 1 >= argc) { std::fprintf(stderr, "mpv: %s needs a value\n", a.c_str()); return false; }
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "-deck") { if (!val(deckFile)) return 2; }
        else if (a == "-top") { if (!val(top)) return 2; }
        else if (a == "-o") { if (!val(outFile)) return 2; }
        else if (a == "-threads") {
            if (!val(v)) return 2;
            char* end = nullptr;
            long n = std::strtol(v.c_str(), &end, 10);
            if (*end || n < 1) { std::fprintf(stderr, "mpv: bad -threads %s\n", v.c_str()); return 2; }
            threads = unsigned(n);
        }
        else if (a == "-h" || a == "-help" || a == "--help") return usage(stdout);
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "mpv: unknown option %s\n", a.c_str()); return 2; }
        else if (gdsFile.empty()) gdsFile = a;
        else { std::fprintf(stderr, "mpv: more than one layout given (%s, %s)\n", gdsFile.c_str(), a.c_str()); return 2; }
    }
    if (deckFile.empty() || gdsFile.empty()) return usage(stderr);
    std::string err;
    drc::Deck deck;
    if (!drc::readDeck(deckFile, deck, err)) { std::fprintf(stderr, "mpv: %s\n", err.c_str()); return 2; }
    if (deck.devices.empty() && deck.connects.empty()) {
        std::fprintf(stderr, "mpv: %s has no connect or device statements\n", deckFile.c_str());
        return 2;
    }
    gds::Library lib;
    if (!gds::read(gdsFile, lib, err)) { std::fprintf(stderr, "mpv: %s\n", err.c_str()); return 2; }
    layout::Flat flat;
    if (!layout::flatten(lib, top, flat, err, threads)) { std::fprintf(stderr, "mpv: %s: %s\n", gdsFile.c_str(), err.c_str()); return 2; }
    drc::Engine eng(deck, flat, threads);
    lvs::Netlist net;
    std::vector<std::string> problems;
    lvs::extract(deck, eng, flat, net, problems);
    std::map<std::string, size_t> perModel;
    for (const lvs::Device& d : net.devices) ++perModel[d.model];
    std::printf("mpv lvs: cell %s, %zu devices, %zu nets, %zu ports\n", flat.top.c_str(), net.devices.size(),
                net.nets.size(), net.ports.size());
    for (const auto& [m, n] : perModel) std::printf("  %-32s %zu\n", m.c_str(), n);
    if (flat.skipped)
        std::printf("warning: %zu shapes or references have edges that are not at 0 or 90 degrees and were left out\n", flat.skipped);
    for (const std::string& p : problems) std::printf("error: %s\n", p.c_str());
    if (!outFile.empty()) {
        FILE* f = std::fopen(outFile.c_str(), "w");
        if (!f) { std::fprintf(stderr, "mpv: cannot write %s\n", outFile.c_str()); return 2; }
        std::fputs(lvs::toSpice(net).c_str(), f);
        std::fclose(f);
    }
    return problems.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(stderr);
    std::string cmd = argv[1];
    if (cmd == "-version" || cmd == "--version") {
        std::printf("mpv %s%s%s\n", MINISIM_VERSION, *MINISIM_GIT ? " " : "", MINISIM_GIT);
        return 0;
    }
    if (cmd == "-h" || cmd == "-help" || cmd == "--help") return usage(stdout);
    if (cmd == "drc") return drcMain(argc - 2, argv + 2);
    if (cmd == "lvs") return lvsMain(argc - 2, argv + 2);
    std::fprintf(stderr, "mpv: unknown command %s\n", cmd.c_str());
    return usage(stderr);
}
