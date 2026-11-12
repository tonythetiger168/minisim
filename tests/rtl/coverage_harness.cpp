#include "Vcoverage_fsm.h"
#include "verilated.h"
#include "verilated_cov.h"
#include <string>
#include <iostream>

int main(int argc, char** argv) {
    VerilatedContext context;
    context.commandArgs(argc, argv);
    Vcoverage_fsm dut(&context);
    // Independent shadow obligation for this fixture's one-cycle implication.
    // This does not read Verilator's internal assertion pending state.
    if (argc > 1 && std::string(argv[1]) == "--end-case") {
        const std::string mode = argc > 2 ? argv[2] : "drained";
        if (mode != "drained" && mode != "undrained" &&
            mode != "no-start" && mode != "reset") return 4;
        bool pending = false;
        unsigned triggers = 0, resolved = 0, failures = 0, cancelled = 0;
        const int cycles = mode == "undrained" ? 7 : 8;
        for (int cycle = 0; cycle < cycles; ++cycle) {
            dut.rst = cycle < 2 || (mode == "reset" && cycle == 7);
            dut.start = cycle == 2 && mode != "no-start";
            dut.clk = 0; dut.eval(); context.timeInc(5);
            // Sample before the rising-edge state update, matching the property.
            if (dut.rst) { cancelled += pending; pending = false; }
            else {
                if (pending) { ++resolved; failures += dut.state != 2; pending = false; }
                if (dut.state == 1 && dut.cnt == 3) { ++triggers; pending = true; }
            }
            std::cout << "SAMPLE cycle=" << cycle << " state=" << unsigned(dut.state)
                      << " cnt=" << unsigned(dut.cnt) << " pending=" << pending << std::endl;
            dut.clk = 1; dut.eval(); context.timeInc(5);
        }
        dut.final();
        std::cout << "OBLIGATION triggers=" << triggers << " resolved=" << resolved
                  << " cancelled=" << cancelled << " pending=" << pending
                  << " failures=" << failures << std::endl;
        if (pending || failures) return 2;
        if (!triggers || !resolved) return 3;
        return 0;
    }
    bool no_start = argc > 1 && std::string(argv[1]) == "--no-start";
    for (int cycle = 0; cycle < 80; ++cycle) {
        dut.rst = cycle < 2;
        dut.start = !no_start && cycle >= 3 && cycle % 16 < 10;
        dut.clk = 0; dut.eval(); context.timeInc(5);
        dut.clk = 1; dut.eval(); context.timeInc(5);
    }
    dut.final();
    context.coveragep()->write("coverage.dat");
}
