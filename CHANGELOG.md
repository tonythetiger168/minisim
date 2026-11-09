# Changelog

All notable changes to minisim are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/) (see `docs/RELEASING.md`).
Spec references are IEEE 1800-2023 clauses.

## [Unreleased]

### Added
- DPI-C (clause 35): `import "DPI-C"` functions and tasks (`pure`, `context`,
  C names), `export "DPI-C"` functions and tasks, every argument type
  including packed vectors and open arrays, context scopes, exported tasks
  that wait on time, and the 35.9 disable protocol. `include/svdpi.h` is
  installed. msim builds C/C++ files given on its command line, loads
  libraries with `-sv_lib` / `-sv_root` / `-sv_liblist` (annex J), and writes
  prototypes with `-dpiheader`. msim now links libffi.
- `chandle` variables (6.14).
- `repeat (n)` with a count known only at run time (12.7.2).
- `mregress`, a regression runner: test lists, seeds, parallel runs, msim /
  VCS / Xcelium / Questa / Icarus backends, pass/fail checks (exit status,
  UVM and simulator error lines, `@pass`, `@expect`), failure grouping,
  `-rerun`, `results.json` / `junit.xml` / `report.html`, `@xfail` known gaps
  (XPASS is an error) and `@exit`, and `-spec` to run the spec suite on any
  of them, honouring `//! xfail:` / `//! exit:` and printing a per-chapter
  pass table.
- FST waveforms: `msim -fst [file]`, `+fst`, or `-vcd file.fst`, in the
  interpreter and in `-compile` (libfst from GTKWave, MIT, fetched at build;
  `-DMINISIM_WITH_FST=OFF` builds VCD only).
- Code coverage: `msim -cov` writes line, branch and toggle coverage; `mcov`
  merges databases and reports text, HTML and LCOV, with `-min-line` /
  `-min-branch` / `-min-toggle` gates; `mregress -cov` merges a whole
  regression.

### Changed
- Waveforms nest the instance hierarchy as scopes under the top module
  (`$scope module adder_tb` / `$scope module u` instead of one flat `top`
  scope with `u.sum`).

### Fixed
- Signals inside generate blocks (`blk[2].x`) were left out of the waveform as
  if they were array elements.
- CI recipes: `ci/Jenkinsfile`, a declarative Jenkins pipeline (JUnit, HTML
  reports, LCOV coverage, `-rerun` on failure), and `ci/README.md` on the
  files mregress and mcov write for a CI job.

## [0.1.0] - unreleased

First tagged release: everything merged up to spec round 5.

### Added
- `mdft`, design-for-test checks and a scan chain plan: `DFT001` a gated or
  unreachable clock, `DFT002` an asynchronous reset built from logic, `DFT003`
  a latch, `DFT004` both edges of one clock, `DFT005` a combinational loop,
  `DFT006` a ripple or divided clock. The summary counts the flops per clock
  domain.
- `mlint`, lint and clock-domain checks on the msim front end: `L001` latches
  in `always_comb`, `L002` blocking assignment in `always_ff`, `L003`
  non-blocking in `always_comb`, `L004` multiple drivers, `CDC001` a crossing
  with no synchronizer or one flop, `CDC002` a multi-bit bus through a two-flop
  synchronizer. `-disable RULE`, `-Werror`, `-list`.
- Release engineering: a version number (`msim -version`), `cmake --install`
  rules, `cpack` packages (`.tar.gz`, `.deb`), a tag-triggered release
  workflow, this changelog, `docs/RELEASING.md` and `docs/PRODUCT.md`.
- An installed `msim -compile` finds the kernel sources in
  `share/minisim/include` next to its `bin/` directory.
- `msim`, an irun / vcs -R style compile-and-simulate command: `-top`,
  `+define+`, `+incdir+`, `-f`, `-vcd`, `-maxtime` (#17).
- Compiled mode, `msim -compile`: each process as generated C++ (#20), with a
  2-state fast path (#28).
- Static levelized combinational schedule (#25); multi-core clocked processes
  and partitions, deterministic for any thread count, at most 8 threads by
  default with work stealing (#27, #33).
- Drive-strength net resolution, gates, pullup / pulldown (#15).
- Spec suite `tests/spec/` with Icarus Verilog 13 cross-check (#18, #24).
- Spec rounds: unpacked arrays, `foreach`, `$readmemh/b` (#21); generate,
  hierarchical names, `+:` / `-:`, any packed range (#22); timescale, delay
  expressions, named events, `disable fork` / `wait fork`, `final` (#23);
  packed structs, `inside`, `++` in expressions, `real`, `string` (#24);
  dynamic arrays, queues, associative arrays (#26).
- Vectors wider than 64 bits (#31).
- `-O2` by default when no build type is set (#19).
