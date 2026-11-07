# Minisim verification improvement follow-up — 2026-11-07

Tracking proposal, not an implementation or closure claim. Local AI-EDA work is on codex/minisim-four-state-mux and includes unsubmitted changes; those changes are not silently included in this PR.

## P1: RTL temporal-property end-of-run acceptance

Local review found that the coverage FSM harness ends after its scheduled cycles without an independently checked pending-obligation count. Native C++ property API guards do not establish closure for RTL assert/cover.

- [ ] Add a last-cycle antecedent fixture for the one-cycle RUN-to-DONE obligation.
- [ ] Stop introducing antecedents, drain the known obligation, and assert no pending work at completion.
- [ ] Reject a wrong consequent and an undrained run; exercise reset disable and vacuous traffic.
- [ ] Require nonempty assertion/cover denominators and at least one nonvacuous antecedent.
- [ ] Record actual tool, seed, source SHA, assertion/cover outcomes and limitations. Distinguish RTL SVA from the C++ property API and a full parser.

Acceptance: clean and drained fixture passes; wrong-consequent, pending-at-end and empty/vacuous acceptance cases are rejected for the intended reason. Existing core PASS is insufficient.

## P1: reproducible VPI build/run lifecycle

Local helper work centralizes manifest, runtime identity, result validation and source/binary hashes. It is not fully migrated or hermetic.

- [ ] Inventory remaining duplicate runners; migrate incrementally without changing their test scope.
- [ ] Define explicit build reuse/invalidations for RTL, generated header, host, core, test/library content, compiler and package identity.
- [ ] Reject changed inputs before simulation and failed/partial builds; do not reuse stale binaries to bypass provenance.
- [ ] Require positive testcase counts and reject malformed/contradictory XML, including expected-failure reason mismatches.

Acceptance: one representative real cocotb run plus intentional stale-input/build-failure/result-validation negatives for each migrated entry; no repeated runner copies.

## P2: coverage-driven random regression reporting

- [ ] Preserve fixed seeds, exact metric denominators and source identity before merging runs.
- [ ] Report new bins per seed and plateau; add directed reachable-hole tests rather than unbounded random repetition.
- [ ] Retain unreachable candidates and tool-attribution gaps until separately justified; no automatic waivers or zero-denominator closure.
- [ ] Keep functional bins, RTL line/toggle/branch/FSM/SVA and external-simulator results separate.
- [ ] Reject empty/error samples and demonstrate an actual DUT mutant is caught.

Acceptance applies only to explicitly measured scopes. Historical portfolio frontend unsupported results must not be rewritten after later frontend improvements; external Verilator/Icarus PASS does not imply Minisim PASS.

No RTL or new test execution is delivered in this documentation PR. Follow-up implementation PRs must attach reproducible evidence and reference this checklist.
