// msynth.cpp - logic synthesis with a dc_shell-style Tcl shell, on the same
// slang front end as msim.
//
//   msynth -f run.tcl                 run a script
//   msynth                            interactive shell (msynth> prompt)
//   msynth rtl.sv -top top -o net.v   one-step: read, compile, report, write
//
// A script reads like a Design Compiler script:
//
//   set target_library my.lib            ;# Liberty; none: mini45 (lib/mini45)
//   analyze -format sverilog {a.sv b.sv}
//   elaborate top
//   create_clock -period 2.0 [get_ports clk]
//   set_input_delay 0.2 -clock clk [all_inputs]
//   compile
//   report_area
//   report_timing
//   write -format verilog -output net.v
//
// The flow: slang elaborates the RTL; src/synth/elab.hpp turns it into an
// and-inverter graph with flops (hierarchy flattened); src/synth/map.hpp
// maps that onto the library's cells by cut matching, for area and, when a
// clock constraint is not met, for delay; src/synth/sta.hpp times the
// result with the library's NLDM tables. `help` lists the commands.
#include <iostream>
#include <unistd.h>

#include "synth/shell.hpp"

using namespace synth;

namespace {

int usage(int code) {
    std::cout << "msynth " << MINISIM_VERSION << " - logic synthesis\n\n"
                 "  msynth -f script.tcl          run a dc_shell-style script\n"
                 "  msynth                        interactive shell\n"
                 "  msynth rtl.sv ... -top top [-lib cells.lib] [-period ns [-clock port]] [-o net.v] [-models cells.v]\n"
                 "                                one step: read, compile, report, write\n\n"
                 "Options:\n"
                 "  -f file       script to run            -x \"cmds\"   commands to run\n"
                 "  -threads N    threads to use (default: every core; 1 disables)\n"
                 "  -q            only errors and reports\n"
                 "  -version      print the version\n\n"
                 "In the shell, `help` lists the commands.\n";
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> scripts, cmds, srcs;
    std::string top, libf, outf, models, period, clock = "clk";
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "msynth: " << a << " needs a value\n";
                exit(2);
            }
            return argv[++i];
        };
        if (a == "-f") scripts.push_back(val());
        else if (a == "-x") cmds.push_back(val());
        else if (a == "-threads") {
            std::string n = val();
            threadCount() = n == "all" ? 0 : std::max(1, atoi(n.c_str()));
        }
        else if (a == "-top") top = val();
        else if (a == "-lib") libf = val();
        else if (a == "-o") outf = val();
        else if (a == "-models") models = val();
        else if (a == "-period") period = val();
        else if (a == "-clock") clock = val();
        else if (a == "-q") quiet = true;
        else if (a == "-version" || a == "--version") {
            std::cout << "msynth " << MINISIM_VERSION << (std::string(MINISIM_GIT).empty() ? "" : " (" MINISIM_GIT ")") << "\n";
            return 0;
        } else if (a == "-h" || a == "-help" || a == "--help") return usage(0);
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "msynth: unknown option " << a << "\n";
            return usage(2);
        } else srcs.push_back(a);
    }
    Shell sh;
    sh.quiet = quiet;
    // Errors of host commands were printed as they happened; the rest
    // (syntax, unknown commands) are printed here with their place.
    size_t seen = 0;
    auto flush = [&] {
        fflush(stdout);
        for (; seen < sh.in.diags.size(); seen++) {
            const auto& d = sh.in.diags[seen];
            if (sh.shown.count(d.text)) continue;
            std::cerr << (d.sev == tcl::Diag::ErrorSev ? "Error: " : "Warning: ") << d.text << " (" << d.loc.str() << ")\n";
        }
    };
    if (!srcs.empty()) {
        std::string s = "analyze -format sverilog {" + tcl::joinList(srcs) + "}\nelaborate " + top + "\n";
        if (!libf.empty()) s += "read_lib {" + libf + "}\n";
        if (!period.empty()) s += "create_clock -period " + period + " [get_ports " + clock + "]\n";
        s += "compile\nreport_area\nreport_timing\n";
        if (!outf.empty()) s += "write -format verilog -output {" + outf + "}\n";
        if (!models.empty()) s += "write_lib_models {" + models + "}\n";
        cmds.insert(cmds.begin(), s);
    }
    if (!scripts.empty() || !cmds.empty()) {
        for (auto& c : cmds) {
            if (sh.in.exited) break;
            sh.in.parse(c, "<command line>");
            flush();
        }
        for (auto& f : scripts) {
            if (sh.in.exited) break;
            try {
                sh.in.load(f);
            } catch (tcl::Error& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
            flush();
        }
        if (sh.in.exited && sh.in.exitCode) return sh.in.exitCode;
        return sh.in.ok() ? 0 : 1;
    }
    bool tty = isatty(0);
    sh.interactive = true;
    if (tty) std::cout << "msynth " << MINISIM_VERSION << " - logic synthesis. `help` lists the commands, `quit` leaves.\n";
    int rc = sh.in.shell(std::cin, std::cout, tty ? "msynth> " : "");
    return rc ? rc : (tty || sh.in.ok() ? 0 : 1);
}
