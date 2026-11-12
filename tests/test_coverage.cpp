#define MINISIM_NO_DEMO
#include "minisim.cpp"
#include "coverage.hpp"
#include <stdexcept>

CoverageGroup coverage;
int clk_id, rst_id, count_id;
int samples = 0;
FsmCoverage fsm;
ClockedProperty count_correct(false), wrap_property(true);
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Unlike the scoreboard model below, the design executes through the real IR/NBA path.
Prog counter_program() {
    Prog p;
    p.emit(waitInstr({{clk_id, POS}}));
    Instr branch; branch.k = IK::JmpIfNot; branch.e = ref(rst_id); branch.target = 4;
    p.emit(branch);
    Instr reset; reset.k = IK::Nba; reset.sig = count_id; reset.e = konst(mk(0, 4));
    p.emit(reset);
    Instr jump; jump.k = IK::Jmp; jump.target = 5; p.emit(jump);
    Instr increment; increment.k = IK::Nba; increment.sig = count_id;
    increment.e = bin(Op::Add, ref(count_id), konst(mk(1, 4))); p.emit(increment);
    jump.target = 0; p.emit(jump);
    p.code[1].line_bin = "line:counter_sv:4";
    p.code[2].line_bin = "line:counter_sv:4";
    p.code[4].line_bin = "line:counter_sv:5";
    return p;
}

Task coverage_driver() {
    int expected = 0;
    S.set(clk_id, mk(0));
    for (int cycle = 0; cycle < 64 && !coverage.closed(); ++cycle) {
        // Choose stimulus from live holes, rather than replaying a fixed vector list.
        bool reset = cycle == 0 ||
            (!coverage.covered("reset_zero") && expected == 0) ||
            (!coverage.covered("reset_nonzero") && expected != 0 &&
             coverage.covered("wrap"));
        S.set(rst_id, mk(reset));
        co_await Delay{1};
        if (cycle) require(S.get(count_id).v == (uint64_t)expected,
                           "count changed without clock edge");
        S.set(clk_id, mk(1));
        co_await Delay{1}; // resumes after the prior slot's NBA has settled
        int previous = expected;
        expected = reset ? 0 : (expected + 1) % 16;
        Val actual = S.get(count_id);
        require(actual.x == 0 && actual.v == (uint64_t)expected,
                "counter scoreboard mismatch");
        count_correct.sample(PropertyTruth::True,
            actual.x == 0 && actual.v == (uint64_t)expected ?
            PropertyTruth::True : PropertyTruth::False);
        wrap_property.sample(actual.v == 15 ? PropertyTruth::True : PropertyTruth::False,
            actual.v == 0 ? PropertyTruth::True : PropertyTruth::False, reset);
        fsm.sample(actual.v == 0 ? "ZERO" : "ACTIVE");
        coverage.hit("count:" + std::to_string(actual.v));
        if (cycle && reset) coverage.hit(previous ? "reset_nonzero" : "reset_zero");
        if (!reset) coverage.hit("increment");
        if (!reset && previous == 15) coverage.hit("wrap");
        ++samples;
        S.set(clk_id, mk(0));
    }
    require(coverage.closed(), "coverage stalled or cycle budget exhausted");
    S.finished = true;
}

int main(int argc, char** argv) {
    // API guards: no vacuous success, and unexpected samples cannot add bins.
    require(!coverage.closed(), "empty coverage group claimed closed");
    coverage.add("reset_zero"); coverage.add("reset_nonzero");
    coverage.add("increment"); coverage.add("wrap");
    for (int n = 0; n < 16; ++n) coverage.add("count:" + std::to_string(n));
    require(coverage.missing().size() == 20, "initial denominator incorrect");
    bool rejected = false;
    try { coverage.hit("typo"); } catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "unknown sample silently accepted");
    S.collect_coverage = true; // enable before registering signals
    fsm.state("ZERO"); fsm.state("ACTIVE");
    fsm.arc("ZERO", "ZERO"); fsm.arc("ZERO", "ACTIVE");
    fsm.arc("ACTIVE", "ACTIVE"); fsm.arc("ACTIVE", "ZERO");
    clk_id = S.sig("clk", 1); rst_id = S.sig("rst", 1); count_id = S.sig("cnt", 4);
    Prog p = counter_program();
    S.spawn(exec(&p)); S.spawn(coverage_driver()); S.run(200);
    require(coverage.closed() && samples <= 64, "coverage did not close");
    require(S.line_coverage.closed(), "mapped line coverage incomplete");
    require(S.toggle_coverage.closed(), "known-bit directional toggle coverage incomplete");
    require(fsm.states.closed() && fsm.arcs.closed(), "FSM coverage incomplete");
    require(count_correct.nonvacuous_pass(), "counter assertion failed or vacuous");
    require(wrap_property.nonvacuous_pass() && wrap_property.cover_hit(),
            "wrap property failed, pending or not covered");
    coverage.write_json(std::cout);
    std::cout << "coverage-driven Minisim counter: " << samples << " cycles, 20/20 bins\n";
    if (argc == 3 && std::string(argv[1]) == "--report") {
        std::ofstream out(argv[2]);
        require(bool(out), "cannot open coverage report");
        out << "{\"backend\":\"minisim-hand-built-ir\",\"cycles\":" << samples
            << ",\"line\":"; S.line_coverage.write_json(out);
        out << ",\"toggle\":"; S.toggle_coverage.write_json(out);
        out << ",\"fsm_states\":"; fsm.states.write_json(out);
        out << ",\"fsm_arcs\":"; fsm.arcs.write_json(out);
        out << ",\"functional\":"; coverage.write_json(out);
        out << ",\"properties\":{\"count_correct_passed\":" << count_correct.passed
            << ",\"count_correct_failed\":" << count_correct.failed
            << ",\"wrap_passed\":" << wrap_property.passed
            << ",\"wrap_failed\":" << wrap_property.failed
            << ",\"wrap_vacuous\":" << wrap_property.vacuous
            << ",\"wrap_disabled\":" << wrap_property.disabled
            << ",\"wrap_pending\":" << (wrap_property.has_pending() ? "true" : "false")
            << "}}\n";
        require(bool(out), "cannot write coverage report");
    }
}
