# Changelog

All notable changes to minisim are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/) (see `docs/RELEASING.md`).
Spec references are IEEE 1800-2023 clauses.

## [Unreleased]

### Added
- `unique` constraints over dynamic arrays, queues and associative arrays
  (18.5.5), alone or with scalars, under implications and on `m[i]`.
- Rand containers of containers (18.4, 7.5): `rand int m[][]`, queues of
  queues, associative arrays of queues, with constraints on `m.size()`,
  `m[i].size()`, `m[i].sum()` and `m[i][j]` in nested foreach loops. Sizes
  are solved level by level and picked again when the elements have no
  solution; a failed randomize() leaves the arrays unchanged.
- rand associative arrays with string keys (18.4).
- Per-process random generators (18.14.2): a forked process is seeded from
  its parent, so values one process draws do not depend on other processes.
- `mbug`: files regression (mregress `results.json`), lint (mlint) and DFT
  (mdft) failures as issues in GitHub Issues (default), Redmine or Jira, one
  issue per failure signature. A repeat of an open failure becomes a comment,
  and a failure that comes back after its issue was closed gets a new issue
  linked to the old one. Issues have the reproduction command, the log tail,
  attachments (Redmine, Jira), and the Jenkins or GitHub Actions build link.
  `mbug sync-github` copies a repository's GitHub issues into Jira or
  Redmine and keeps open / closed in step (`-two-way` also closes GitHub
  issues resolved on the other side). It has `-dry-run` and a `max_issues`
  cap, and credentials come from the environment only. `ci/Jenkinsfile`
  has an opt-in `BUGS` parameter that runs it. `results.json` now records
  each failure's `signature` and the test lists.
- The constraint solver weights each choice by how many ways the other
  variables can still be completed, so values are close to uniform over the
  joint solution space (18.5.10): `s -> d == 0` gives s = 0, and `x < y`
  is no longer biased toward a large x.
- Containers of containers (7.5, 7.8, 7.10): `int m[][]`, `int q[$][$]`,
  associative arrays of queues, and fixed-size arrays of containers
  (`int f[2][]`); containers of unpacked structs and of vectors wider than
  64 bits.
- `rand join` in randsequence (18.17.5), with a constant bias, and production
  arguments and return values (18.17.7).
- `dist` default weights (`default :/ w`, 18.5.3) and rand associative
  arrays with integral keys (18.4); foreach constraints over associative
  arrays bind the index to each key.
- Function calls in constraints (18.5.12): methods, static and package
  functions, nested calls, in guards, foreach bodies and inline `with`. The
  rand variables a call reads are solved first.
- rand dynamic arrays and queues (18.4): `size()` constraints, `foreach`
  over the elements and array reductions (`sum`, `product`, `and`, `or`,
  `xor`, with or without `with`) in constraints (18.5.8). Sizes are solved
  first, then the elements. Rand arrays and queues of class handles
  randomize each element's object.
- Virtual interfaces (25.9): variables, properties and arguments of
  `virtual ifc` / `virtual ifc.mp` type; signals, events, tasks, functions and
  clocking blocks reached through them.
- `std::semaphore` (15.3), `std::mailbox` (15.4, parameterized, typeless and
  bounded) and `std::process` (9.7: self, status, kill, await, suspend,
  resume).
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

- Constrained randomization (18): rand / randc, constraint blocks (inside,
  dist, unique, implication, if-else, foreach, solve-before, soft, static,
  extern), `randomize()` with `with {...}`, pre/post_randomize,
  rand_mode / constraint_mode, `std::randomize`, randcase, randsequence,
  per-object random generators with srandom / get_randstate / set_randstate.
- `sum()` / `product()` / `and()` / `or()` / `xor()` on fixed-size arrays,
  with an optional `with` clause (7.12.3).
- Classes (8): objects, constructors, static members, `this`, shallow copy,
  inheritance, `super`, virtual and pure virtual methods, abstract and
  parameterized classes, interface classes, typed constructors `D::new`,
  `$cast` on class handles, out-of-block methods, `std::weak_reference`.
- Arrays, dynamic arrays and queues as subroutine arguments (7.7).
- Automatic subroutines get a frame per call (6.21, 13.3.2, 13.4.2): recursion,
  automatic tasks called from several processes at once, and
  `fork automatic int j = i; ... join_none` with one copy per fork. This is
  the base for class methods (8).

### Changed
- msim compiles as IEEE 1800-2023 by default (slang's default was 2017);
  `--std` still selects another version.
- Waveforms nest the instance hierarchy as scopes under the top module
  (`$scope module adder_tb` / `$scope module u` instead of one flat `top`
  scope with `u.sum`).

### Fixed
- `s[i]` on a string read X and could not be written (6.16).
- The constraint solver re-evaluated known subexpressions at every node,
  so a long `sum()` constraint cost O(n^2) per check.
- msim could crash at exit when a process with an automatic frame (a
  forked automatic task, a class method) was still suspended: its frame went
  back to a frame shape that was already freed.
- A static 2-state local of a task, function or block with no initializer
  (`static int c;`) started at x instead of 0 (6.8, 6.21).
- A module-level unpacked array of a 2-state type (`int a[8]`) started at x
  instead of 0 (6.8).
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
