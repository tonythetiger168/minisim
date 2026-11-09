# Changelog

All notable changes to minisim are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/) (see `docs/RELEASING.md`).
Spec references are IEEE 1800-2023 clauses.

## [Unreleased]

### Added
- `mregress`, a regression runner: test lists, seeds, parallel runs, msim /
  VCS / Xcelium / Questa / Icarus backends, pass/fail checks (exit status,
  UVM and simulator error lines, `@pass`, `@expect`), failure grouping,
  `-rerun`, `results.json` / `junit.xml` / `report.html`, `@xfail` known gaps
  (XPASS is an error) and `@exit`, and `-spec` to run the spec suite on any
  of them, honouring `//! xfail:` / `//! exit:` and printing a per-chapter
  pass table.

## [0.1.0] - unreleased

First tagged release: everything merged up to spec round 5.

### Added
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
