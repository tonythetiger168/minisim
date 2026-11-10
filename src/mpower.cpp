// mpower.cpp - power analysis on the same slang front end as msim.
//
//   mpower netlist.v -top dut -lib cells.lib -activity run.saif -strip_path tb.dut
//   mpower rtl.sv -top dut -activity waves.vcd -strip_path tb.dut -peak
//
// This file is the command line and the report; the computation is
// power.hpp (pwr::analyze), which other tools call too. docs/POWER.md is
// the manual.
#include "power.hpp"

using namespace pwr;

namespace {

struct Unit {
    const char* name;
    double scale;
};
Unit pickUnit(double w, const std::string& forced) {
    static const Unit us[] = {{"W", 1}, {"mW", 1e-3}, {"uW", 1e-6}, {"nW", 1e-9}, {"pW", 1e-12}};
    for (auto& u : us)
        if (forced == u.name) return u;
    for (auto& u : us)
        if (w >= u.scale) return u;
    return us[4];
}
std::string num(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.4g", v);
    return b;
}
std::string jnum(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6g", std::isfinite(v) ? v : 0.0);
    return b;
}
std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

void usage() {
    std::cout << "usage: mpower [options] <design files>\n"
                 "  -top <module>          the design to analyze (the DUT, not the testbench)\n"
                 "  -lib <file.lib>        Liberty library with power data; may repeat. Without one a\n"
                 "                         built-in generic technology is used (RTL estimates only)\n"
                 "  -activity <file>       switching activity: .saif, .vcd or .fst (also -saif, -vcd, -fst)\n"
                 "  -strip_path <path>     the design's instance in the activity file: tb.dut\n"
                 "  -clock <net> <ns>      a clock and its period, for nets without activity\n"
                 "  -default_toggle_rate <r>  toggles per clock cycle of an input without activity (0.1)\n"
                 "  -voltage <V>           supply voltage (default: the library's nom_voltage)\n"
                 "  -input_transition <ns> transition time at the primary inputs (0.05)\n"
                 "  -wire_cap <fF>         wire capacitance per fanout (default: the library's wire load, or 0)\n"
                 "  -upf <file>            IEEE 1801 power intent: per-domain power, voltages, switched domains\n"
                 "  -power_top <inst>      the instance the UPF is written for (default: the top)\n"
                 "  -peak                  time-based analysis: peak power (needs .vcd or .fst activity)\n"
                 "  -interval <ns>         the averaging window of -peak (default: one clock period)\n"
                 "  -waveform <file.csv>   power over time, one row per window (implies -peak)\n"
                 "  -hier <n>              power per instance, n levels deep\n"
                 "  -cells <n>             the n cells that draw the most\n"
                 "  -json <file>           the report as JSON\n"
                 "  -write_saif <file>     write the activity as SAIF (with no design: convert only)\n"
                 "  -rtl_ff <cell> -rtl_gate <cell>  the flop and gate RTL is costed with\n"
                 "  -rtl_mem_bits <n>      an unpacked array above n bits is a memory, not flops (4096)\n"
                 "  -unit W|mW|uW|nW|pW    the unit of the tables\n"
                 "  -threads <n>           worker threads (default: every logical core; 1 = serial)\n"
                 "  -f <file>, +define+, +incdir+, -y, -v ...  as msim\n"
                 "  -q                     only the total line\n"
                 "  -version, -h\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::vector<std::string> fwd{"mpower"};
    unsigned threads = par::defaultThreads();   // every logical core; -threads 1 is serial
    auto t_start = std::chrono::steady_clock::now();
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mpower: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-version" || a == "--version") { std::cout << "mpower " << MINISIM_VERSION << "\n"; return 0; }
        if (a == "-top") { o.top = next("a module name"); fwd.push_back("--top"); fwd.push_back(o.top); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        if (a == "-lib") { o.libs.push_back(next("a Liberty file")); continue; }
        if (a == "-activity" || a == "-saif" || a == "-vcd" || a == "-fst") { o.activity = next("an activity file"); continue; }
        if (a == "-strip_path") { o.strip = next("an instance path"); continue; }
        if (a == "-clock") {
            std::string n = next("a net name");
            o.clocks.push_back({n, std::atof(next("a period in ns").c_str())});
            continue;
        }
        if (a == "-default_toggle_rate") { o.toggle = std::atof(next("a rate").c_str()); continue; }
        if (a == "-voltage") { o.voltage = std::atof(next("a voltage").c_str()); continue; }
        if (a == "-input_transition") { o.inSlew = std::atof(next("a time in ns").c_str()); continue; }
        if (a == "-wire_cap") { o.wireCap = std::atof(next("a capacitance in fF").c_str()); continue; }
        if (a == "-upf") { o.upfPath = next("a UPF file"); continue; }
        if (a == "-power_top") { o.powerTop = next("an instance path"); continue; }
        if (a == "-peak") { o.peak = true; continue; }
        if (a == "-interval") { o.interval = std::atof(next("a time in ns").c_str()); o.peak = true; continue; }
        if (a == "-waveform") { o.waveform = next("a file name"); o.peak = true; continue; }
        if (a == "-hier") { o.hier = std::atoi(next("a depth").c_str()); continue; }
        if (a == "-cells") { o.cells = std::atoi(next("a count").c_str()); continue; }
        if (a == "-json") { o.json = next("a file name"); continue; }
        if (a == "-write_saif") { o.writeSaif = next("a file name"); continue; }
        if (a == "-rtl_ff") { o.rtlFf = next("a cell name"); continue; }
        if (a == "-rtl_gate") { o.rtlGate = next("a cell name"); continue; }
        if (a == "-rtl_mem_bits") { o.memBits = std::strtoull(next("a bit count").c_str(), nullptr, 10); continue; }
        if (a == "-unit") { o.unit = next("a unit"); continue; }
        if (par::threadsArg(argc, argv, i, threads, "mpower")) continue;
        if (a == "-q") { o.quiet = true; continue; }
        if (a == "-f") { fwd.push_back("-f"); fwd.push_back(next("a file")); continue; }
        if (a == "-y" || a == "-v") { fwd.push_back(a); fwd.push_back(next("a path")); continue; }
        fwd.push_back(a);
    }

    try {
        if (fwd.size() == 1) {
            // no design: convert the activity
            if (o.writeSaif.empty() || o.activity.empty()) {
                usage();
                return 2;
            }
            saif::Db db;
            saif::load(o.activity, db, threads);
            std::ofstream f(o.writeSaif);
            if (!f) throw std::runtime_error("cannot write " + o.writeSaif);
            saif::write(db, f, "", "mpower", MINISIM_VERSION);
            if (!o.quiet) std::cout << "mpower: wrote " << o.writeSaif << " (" << db.bits.size() << " bits)\n";
            return 0;
        }
        Result R;
        if (int rc = analyze(o, fwd, threads, R)) return rc < 0 ? 0 : rc;
        const P3& total = R.total;
        if (!o.waveform.empty()) {
            std::ofstream f(o.waveform);
            if (!f) throw std::runtime_error("cannot write " + o.waveform);
            f << "time_ns,power_W\n";
            for (size_t k = 0; k < R.wave.size(); k++)
                f << jnum((double)k * R.windowS * 1e9) << "," << jnum(R.wave[k]) << "\n";
        }

        // ---- report
        Unit u = pickUnit(total.total(), o.unit);
        auto col = [&](double w) {
            std::ostringstream s;
            s << std::setw(12) << num(w / u.scale);
            return s.str();
        };
        auto pct = [&](double w) {
            std::ostringstream s;
            s << std::setw(7) << std::fixed << std::setprecision(1) << (total.total() > 0 ? 100 * w / total.total() : 0.0) << "%";
            return s.str();
        };
        auto row = [&](std::string name, const P3& p) {
            if (name.size() > 25) name = "..." + name.substr(name.size() - 22);
            std::cout << std::left << std::setw(26) << name << std::right << col(p.i) << col(p.s) << col(p.l)
                      << col(p.total()) << pct(p.total()) << "\n";
        };
        auto head = [&](const char* what, const char* tail) {
            std::cout << "\n"
                      << std::left << std::setw(26) << what << std::right << std::setw(12) << "Internal" << std::setw(12)
                      << "Switching" << std::setw(12) << "Leakage" << std::setw(12) << "Total" << std::setw(8) << "%"
                      << tail << "\n";
        };
        if (!o.quiet) {
            std::cout << "mpower " << MINISIM_VERSION << "\n";
            std::cout << "design    " << R.design << ": " << R.cells << " cells";
            if (R.anyRtl)
                std::cout << ", RTL estimated as " << R.rtlFlops << " flops (" << R.rtlFf << ") and " << num(R.rtlGates)
                          << " gates (" << R.rtlGate << ")";
            std::cout << ", " << R.nets << " nets\n";
            std::cout << "library   " << R.library;
            if (R.voltage > 0) std::cout << ", " << num(R.voltage) << " V";
            std::cout << "\n";
            if (R.haveActivity)
                std::cout << "activity  " << o.activity << ": " << num(R.seconds * 1e9) << " ns, " << std::fixed
                          << std::setprecision(1) << R.annotated << "% of nets annotated" << std::defaultfloat
                          << (R.activityScope.empty() ? "" : " (under " + R.activityScope + ")") << "\n";
            else
                std::cout << "activity  none: "
                          << (R.clockHz > 0 ? "propagated from -clock" : "no clock given, so leakage only") << "\n";
            if (R.clockHz > 0) std::cout << "clock     " << num(R.clockHz / 1e6) << " MHz (the fastest)\n";
            head("Power group", (std::string("   (") + u.name + ")").c_str());
            for (int g = 0; g < G_COUNT; g++)
                if (R.groups[g].total() > 0 || R.groupCells[g]) row(kGroupName[g], R.groups[g]);
            row("Total", total);
            if (!R.domains.empty()) {
                head("Power domain", "   voltage, time on");
                for (auto& dmn : R.domains) {
                    std::cout << std::left << std::setw(26) << dmn.name << std::right << col(dmn.p.i) << col(dmn.p.s)
                              << col(dmn.p.l) << col(dmn.p.total()) << pct(dmn.p.total()) << "   "
                              << (dmn.volt > 0 ? num(dmn.volt) + " V" : std::string("nominal")) << ", "
                              << (dmn.switched ? num(100 * dmn.on) + "%" : std::string("always")) << "\n";
                }
            }
            if (!R.instances.empty()) {
                head("Instance", "");
                for (auto& r : R.instances) row(r.name, r.p);
            }
            if (!R.topCells.empty()) {
                head("Cell", "");
                for (auto& r : R.topCells) row(r.name + " (" + r.cell + ")", r.p);
            }
            if (R.peak)
                std::cout << "\npeak      " << num(R.peakW / u.scale) << " " << u.name << " at " << num(R.peakT * 1e9)
                          << " ns (" << num(R.windowS * 1e9) << " ns windows, " << R.wave.size() << " of them), "
                          << num(total.total() > 0 ? R.peakW / total.total() : 0) << "x the average\n";
            if (!R.blackBoxes.empty()) {
                size_t n = 0;
                std::string names;
                for (auto& [k, c] : R.blackBoxes) {
                    n += (size_t)c;
                    if (names.size() < 60) names += (names.empty() ? "" : ", ") + k;
                }
                std::cout << "\nnote: " << n << " instances of modules that are in no library and have no source (" << names
                          << (names.size() >= 60 ? ", ..." : "") << ") are not counted\n";
            }
            if (R.anyRtl)
                std::cout << "\nnote: RTL logic is an estimate from operator sizes; synthesize for a number to sign off\n";
            if (R.genericLib)
                std::cout << "note: no -lib given: the built-in generic technology is for comparing designs, not for absolute numbers\n";
            for (auto& n : R.notes) std::cout << "note: " << n << "\n";
            if (R.haveActivity && R.annotated < 20 && R.nets)
                std::cout << "note: few of the nets have activity; check -strip_path (activity names look like '"
                          << R.sampleActivityName << "', design nets like '" << R.sampleNetName << "')\n";
        }
        std::cout << "total power " << num(total.total() / u.scale) << " " << u.name << " (internal "
                  << num(total.i / u.scale) << ", switching " << num(total.s / u.scale) << ", leakage "
                  << num(total.l / u.scale) << ")\n";

        if (!o.json.empty()) {
            std::ofstream f(o.json);
            if (!f) throw std::runtime_error("cannot write " + o.json);
            auto p3 = [&](const P3& p) {
                return "{\"internal\": " + jnum(p.i) + ", \"switching\": " + jnum(p.s) + ", \"leakage\": " + jnum(p.l) +
                       ", \"total\": " + jnum(p.total()) + "}";
            };
            f << "{\n  \"tool\": \"mpower\", \"version\": " << jstr(MINISIM_VERSION) << ", \"unit\": \"W\",\n";
            f << "  \"design\": " << jstr(R.design) << ", \"library\": " << jstr(R.library) << ",\n";
            f << "  \"cells\": " << R.cells << ", \"rtl_flops\": " << R.rtlFlops << ", \"rtl_gates\": " << jnum(R.rtlGates)
              << ", \"nets\": " << R.nets << ",\n";
            f << "  \"activity\": {\"file\": " << jstr(o.activity) << ", \"duration_s\": " << jnum(R.seconds)
              << ", \"annotated_percent\": " << jnum(R.annotated) << ", \"clock_hz\": " << jnum(R.clockHz) << "},\n";
            f << "  \"total\": " << p3(total) << ",\n  \"groups\": {";
            bool first = true;
            for (int g = 0; g < G_COUNT; g++) {
                if (!(R.groups[g].total() > 0 || R.groupCells[g])) continue;
                f << (first ? "" : ",") << "\n    " << jstr(kGroupName[g]) << ": " << p3(R.groups[g]);
                first = false;
            }
            f << "\n  }";
            if (!R.domains.empty()) {
                f << ",\n  \"domains\": {";
                first = true;
                for (auto& dmn : R.domains) {
                    f << (first ? "" : ",") << "\n    " << jstr(dmn.name) << ": {\"power\": " << p3(dmn.p)
                      << ", \"voltage\": " << jnum(dmn.volt) << ", \"on_fraction\": " << jnum(dmn.on)
                      << ", \"switched\": " << (dmn.switched ? "true" : "false") << "}";
                    first = false;
                }
                f << "\n  }";
            }
            if (!R.instances.empty()) {
                f << ",\n  \"instances\": {";
                first = true;
                for (auto& r : R.instances) {
                    f << (first ? "" : ",") << "\n    " << jstr(r.name) << ": " << p3(r.p);
                    first = false;
                }
                f << "\n  }";
            }
            if (!R.topCells.empty()) {
                f << ",\n  \"top_cells\": [";
                first = true;
                for (auto& r : R.topCells) {
                    f << (first ? "" : ",") << "\n    {\"name\": " << jstr(r.name) << ", \"cell\": " << jstr(r.cell)
                      << ", \"power\": " << p3(r.p) << "}";
                    first = false;
                }
                f << "\n  ]";
            }
            if (R.peak)
                f << ",\n  \"peak\": {\"power\": " << jnum(R.peakW) << ", \"time_s\": " << jnum(R.peakT)
                  << ", \"window_s\": " << jnum(R.windowS) << ", \"windows\": " << R.wave.size() << "}";
            f << "\n}\n";
        }
        if (std::getenv("MPOWER_TIME")) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_start).count();
            std::cerr << "mpower: " << ms << " ms, " << threads << " threads\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "mpower: " << e.what() << "\n";
        return 1;
    }
}
