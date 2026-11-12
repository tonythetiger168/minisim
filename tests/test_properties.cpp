#include "coverage.hpp"
#include <stdexcept>
void check(bool ok) { if (!ok) throw std::runtime_error("coverage API regression"); }
int main() {
    using T = PropertyTruth;
    ClockedProperty vacuous(false);
    vacuous.sample(T::False, T::True);
    check(vacuous.vacuous == 1 && !vacuous.nonvacuous_pass() && !vacuous.cover_hit());
    ClockedProperty overlap(false);
    overlap.sample(T::True, T::True);
    check(overlap.nonvacuous_pass() && overlap.cover_hit());
    overlap.sample(T::True, T::Unknown);
    check(overlap.failed == 1 && !overlap.nonvacuous_pass());
    ClockedProperty delayed(true);
    delayed.sample(T::True, T::False);
    check(delayed.has_pending() && !delayed.cover_hit());
    delayed.sample(T::False, T::True);
    check(delayed.nonvacuous_pass() && !delayed.has_pending());
    delayed.sample(T::True, T::True);
    delayed.sample(T::False, T::False, true); // disable aborts pending obligation
    check(!delayed.has_pending() && delayed.failed == 0 && delayed.disabled == 1);
    // A resolved hit must not close an assertion with a newer pending obligation.
    ClockedProperty consecutive(true);
    consecutive.sample(T::True, T::False);
    consecutive.sample(T::True, T::True);
    check(consecutive.attempts == 2 && consecutive.passed == 1);
    check(consecutive.cover_hit() && consecutive.has_pending() && !consecutive.nonvacuous_pass());
    consecutive.sample(T::False, T::False);
    check(consecutive.failed == 1 && !consecutive.has_pending() && !consecutive.nonvacuous_pass());

    ClockedProperty failure(true);
    failure.sample(T::True, T::True);
    failure.sample(T::False, T::False);
    check(failure.failed == 1 && !failure.nonvacuous_pass());
    FsmCoverage f;
    f.state("IDLE"); f.state("RUN"); f.arc("IDLE", "RUN");
    f.sample("IDLE"); f.sample("RUN");
    check(f.states.closed() && f.arcs.closed());
    bool rejected = false;
    try { f.sample("IDLE"); } catch (const std::out_of_range&) { rejected = true; }
    check(rejected);
    f.reset_history(); f.sample("IDLE");
    CoverageGroup g;
    g.add("hit"); g.add("hole"); g.hit("hit");
    check(!g.closed() && g.missing() == std::vector<std::string>{"hole"});
}
