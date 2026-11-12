// Reusable verification model for an unsigned +1 synchronous-reset counter.
// It observes DUT values; it never implements or replaces the DUT.
#pragma once
#include "coverage.hpp"
#include "expr.hpp"
#include <stdexcept>

class CounterModel {
    uint64_t expected_value = 0;
    bool initialized = false;
    uint64_t maximum;
public:
    CoverageGroup functional;
    FsmCoverage fsm;
    ClockedProperty correct{false}, wrap{true};
    unsigned samples = 0;
    std::string failure;

    explicit CounterModel(int width) {
        // Enumerated value bins and ZERO/ACTIVE self arcs are bounded to this range.
        if (width < 2 || width > 8) throw std::invalid_argument("counter model width must be 2..8");
        maximum = (1ULL << width) - 1;
        for (uint64_t n = 0; n <= maximum; ++n) functional.add("count:" + std::to_string(n));
        for (const auto* name : {"reset_zero", "reset_nonzero", "increment", "wrap"})
            functional.add(name);
        fsm.state("ZERO"); fsm.state("ACTIVE");
        for (const auto* from : {"ZERO", "ACTIVE"})
            for (const auto* to : {"ZERO", "ACTIVE"}) fsm.arc(from, to);
    }
    bool next_reset() const {
        return !initialized ||
            (!functional.covered("reset_zero") && expected_value == 0) ||
            (!functional.covered("reset_nonzero") && expected_value != 0 && functional.covered("wrap"));
    }
    bool unchanged(Val actual) {
        if (initialized && (actual.x || actual.v != expected_value)) {
            failure = "changed_without_clock"; return false;
        }
        return true;
    }
    bool sample(bool reset, Val actual) {
        uint64_t previous = expected_value;
        expected_value = reset ? 0 : (previous + 1) & maximum;
        ++samples;
        bool matches = actual.x == 0 && actual.v == expected_value;
        correct.sample(PropertyTruth::True, matches ? PropertyTruth::True : PropertyTruth::False);
        if (!matches) { failure = "scoreboard_mismatch"; return false; }
        // Incorrect samples are never credited toward functional closure.
        functional.hit("count:" + std::to_string(actual.v));
        if (initialized && reset) functional.hit(previous ? "reset_nonzero" : "reset_zero");
        if (!reset) functional.hit("increment");
        if (!reset && previous == maximum) functional.hit("wrap");
        wrap.sample(actual.v == maximum ? PropertyTruth::True : PropertyTruth::False,
                    actual.v == 0 ? PropertyTruth::True : PropertyTruth::False, reset);
        fsm.sample(actual.v == 0 ? "ZERO" : "ACTIVE");
        initialized = true;
        return true;
    }
    bool closed() const {
        return failure.empty() && functional.closed() && fsm.states.closed() && fsm.arcs.closed()
            && correct.nonvacuous_pass() && wrap.nonvacuous_pass();
    }
};
