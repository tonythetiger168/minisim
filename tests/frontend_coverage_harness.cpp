#define MINISIM_NO_DEMO
#include "minisim.cpp"
#include "design.hpp"
#include "counter_model.hpp"

GeneratedDesign design;
std::unique_ptr<CounterModel> model;
unsigned budget = 512;
bool hold_reset = false;
std::ofstream trace;

Task adaptive_driver() {
    int clk = design.signals.at("clk"), rst = design.signals.at("rst"), cnt = design.signals.at("cnt");
    S.set(clk, mk(0));
    for (unsigned cycle = 0; cycle < budget && !model->closed(); ++cycle) {
        bool reset = hold_reset || model->next_reset();
        S.set(rst, mk(reset));
        co_await Delay{1};
        if (!model->unchanged(S.get(cnt))) break;
        S.set(clk, mk(1));
        co_await Delay{1}; // Observe the settled NBA value from the preceding time slot.
        Val actual = S.get(cnt);
        trace << cycle << ',' << reset << ',' << (actual.x ? "unknown" : std::to_string(actual.v)) << '\n';
        if (!model->sample(reset, actual)) break;
        S.set(clk, mk(0));
    }
    if (!model->closed() && model->failure.empty()) model->failure = "cycle_budget_exhausted";
    S.finished = true;
}

int main(int argc, char** argv) {
    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--hold-reset") hold_reset = true;
            else if (arg == "--budget" && i + 1 < argc) {
                int value = std::stoi(argv[++i]);
                if (value < 1 || value > 4096) throw std::invalid_argument("budget must be 1..4096");
                budget = (unsigned)value;
            } else throw std::invalid_argument("unknown argument");
        }
        S.collect_coverage = true;
        design = build_generated_design();
        if (S.sigs.at(design.signals.at("clk")).w != 1 || S.sigs.at(design.signals.at("rst")).w != 1)
            throw std::invalid_argument("clock/reset must be scalar");
        model = std::make_unique<CounterModel>(S.sigs.at(design.signals.at("cnt")).w);
        trace.open("stimulus.csv");
        if (!trace) throw std::runtime_error("cannot open stimulus trace");
        for (auto& program : design.programs) S.spawn(exec(&program));
        S.spawn(adaptive_driver()); S.run(2ULL * budget + 1);
        trace.close();
        if (!model->closed() && model->failure.empty()) model->failure = "simulation_ended_incomplete";
        if (model->closed() && (!S.line_coverage.closed() || !S.toggle_coverage.closed()))
            model->failure = "structural_coverage_incomplete";
        std::ofstream out("coverage.json");
        out << "{\"backend\":\"minisim-pyslang-ir\",\"samples\":" << model->samples
            << ",\"passed\":" << (model->closed() ? "true" : "false")
            << ",\"failure\":\"" << model->failure << "\",\"functional\":";
        model->functional.write_json(out);
        out << ",\"line\":"; S.line_coverage.write_json(out);
        out << ",\"toggle\":"; S.toggle_coverage.write_json(out);
        out << ",\"fsm_states\":"; model->fsm.states.write_json(out);
        out << ",\"fsm_arcs\":"; model->fsm.arcs.write_json(out);
        out << ",\"properties\":{\"correct_passed\":" << model->correct.passed
            << ",\"correct_failed\":" << model->correct.failed
            << ",\"wrap_passed\":" << model->wrap.passed
            << ",\"wrap_failed\":" << model->wrap.failed
            << ",\"wrap_vacuous\":" << model->wrap.vacuous
            << ",\"wrap_pending\":" << (model->wrap.has_pending() ? "true" : "false") << "}}\n";
        out.close();
        if (!out || !trace) throw std::runtime_error("cannot write verification evidence");
        if (!model->closed()) {
            std::cerr << model->failure << " sample=" << model->samples << '\n';
            return model->failure == "scoreboard_mismatch" ? 10 : 11;
        }
        std::cout << "coverage closed in " << model->samples << " cycles\n";
    } catch (const std::exception& error) {
        std::cerr << "HARNESS ERROR: " << error.what() << '\n'; return 2;
    }
}
