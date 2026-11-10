# Changelog

All notable changes to minisim are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/) (see `docs/RELEASING.md`).
Spec references are IEEE 1800-2023 clauses.

## [Unreleased]

### Added
- `mpss`, portable stimulus: reads a model in the Accellera PSS language
  (components, actions, buffers, streams, resources and pools, constraints,
  activities with `parallel` / `schedule` / `select` / `repeat`, exec blocks
  and functions), solves scenarios (field values, resource instances, and
  the actions inferred to feed unbound inputs), and writes each scenario as
  a UVM sequence class (`-sv`, runs under `msim -uvm`) and as a C test
  function (`-c`), plus `-print` and `-json`. Scenarios are solved on all
  cores (`-threads N`) with the same output for every thread count. See
  `docs/MPSS.md` and `examples/pss/dma.pss`.
- `src/interval.hpp`: the interval arithmetic of the `randomize()` solver,
  moved out of `rand.inc` so that `mpss` uses the same rules.
- `mdebug`, source-level debug on the msim front end: `-drivers` and `-loads`
  of a signal with the `if` / `case` conditions on each assignment, `-trace`
  for the fan-in cone across ports, `-active SIG -at T` with `-vcd` for the
  assignment that gave a signal its value at a time and its causes back
  through time, and `-html` for a one-file viewer (hierarchy, clickable
  source, drivers and loads, schematic, waves with values annotated in the
  source). See `docs/DEBUG.md`.
- Protected IP (IEEE 1735-2023 and 1800-2023 clause 34): `msimprotect`
  writes version 3 envelopes (RSA no-padding root key per tool block,
  AES-256-CBC, HMAC-SHA512 signature; `-v1` for clause 34 envelopes), from
  command-line keys, a `-keyring` in the 1735 key exchange format, or a
  recipe in the source, with rights (`-control`). `msim`, `mlint` and `mdft`
  take `-keyfile` / `-keyring` / `-license`, check the signature before
  decrypting, honour the `decryption`, `visibility`, `coverage` and
  `min_version` rights and `viewport` / `interface_viewport` pragmas, and keep
  protected code out of diagnostics, assertion messages, VCD / FST, coverage,
  lint findings and the DFT chain plan. See `docs/PROTECT.md`.
- Datapath library `dw/`: 47 synthesizable, parameterized SystemVerilog
  building blocks named `MW_*` (adders with five carry architectures,
  Booth/Wallace and array multipliers, multiply-accumulate, dividers, square
  root, barrel shifters, leading-zero and priority logic, Gray code, ECC,
  CRC, LFSR, counters, synchronous and dual-clock FIFOs, arbiters). Use with
  `msim -y dw +libext+.sv`; `docs/DATAPATH.md` lists every module.
- Foreach constraints with several loop variables over containers of
  containers, `foreach (q[i, j]) q[i][j] inside {[1:5]};` (18.5.8.1), also
  over a fixed-size array of queues and associative arrays with string keys.
- VPI: `vpi_handle_by_name` of a part select (`top.u.r[3:0]`, a
  `vpiPartSelect`, 38.27) reads, writes, forces and releases just those
  bits, so UVM's `uvm_hdl_*` backdoor works on part-select paths.
- Low-power simulation with UPF (IEEE 1801-2024): `msim -upf file.upf`
  (`-power_top`, `-upf_verbose`) reads power domains, supply ports, nets and
  sets, power switches, isolation, level-shifter and retention strategies,
  power states and legacy PSTs; powered-off logic stops and is corrupted,
  isolation clamps, retention saves and restores, illegal power states and
  PST violations are reported. Package `UPF` (`supply_on`, `supply_off`,
  `set_power_state`, `get_supply_value`, ...) is added for the testbench.
  `mlint -upf` checks the power intent: UPF001 to UPF006. See
  `docs/LOWPOWER.md`.
- `msim -uvm` (and `-uvmhome <dir>`, `-uvmnodpi`): compile the Accellera UVM
  library from `$UVM_HOME` with its C code, so regular expressions, the full
  command line (`+uvm_set_config_*`, `+UVM_VERBOSITY`, ...) and HDL backdoor
  access (`uvm_hdl_*`, register backdoor reads and writes) work.
  `uvm/uvm_dpi_minisim.cc` builds uvm-core's `src/dpi` with a backdoor over
  plain VPI.
- VPI: `vpi_put_value` with `vpiForceFlag` and `vpiReleaseFlag` (38.35);
  `tf_dofinish` / `tf_dostop` (`veriuser.h`) and `sv_vpi_user.h`; VPI calls
  from DPI context imports (35.5.3).
- Specify paths to outputs declared as variables (30.4): the port
  connection takes the path delay.
- Two cells with one name in one library are a warning, and the last one is
  used (33.3.1.1). VPI `vpiLibrary`, `vpiCell` and `vpiConfig` (33.7).
- Pulse filtering at path outputs (30.7): `PATHPULSE$` limits, `+pulse_r/N`
  and `+pulse_e/N`, pulses to x, pulse styles (`pulsestyle_ondetect`,
  `showcancelled` and the `+pulse_on_detect` / `+showcancelled` options).
  Path delays now combine with gate and net delays inside the module by
  taking the larger (30.6).
- SDF: pulse limits (`((d) (r) (e))`, PATHPULSE, PATHPULSEPERCENT, 32.7),
  DEVICE on gates and UDPs, INTERCONNECT from one source of a net with
  several sources (32.4.4, 32.5), and SCOND / CCOND.
- UVM (IEEE 1800.2-2020): the Accellera UVM library (uvm-core 2020.3.1)
  compiles and runs with `+define+UVM_NO_DPI` (see `docs/UVM.md`): run_test,
  the factory and overrides, phases and objections, sequences, sequencers and
  drivers, TLM ports and FIFOs, analysis ports, `uvm_config_db`, field
  macros (print, clone, compare, pack), report catchers. `tests/uvm` runs
  three testbenches in CI.
- Language features that UVM needs: containers of containers
  (`int q[string][$]`, `bit m[C][C]`, foreach over several of their
  dimensions), class-handle, packed-struct and wide associative array keys,
  struct elements and writes to part of an element (`q[i][3:0] = v`,
  `aa[k].f = v`), unpacked structs in classes and automatic variables,
  functions that return queues and associative arrays, static containers in
  functions, `==` / `!=` on whole containers, string indexing (`s[i]`,
  `s[i] = c`, `foreach (s[i])`), `{n{s}}` with a run-time `n`, string
  parameters, `{>>{q}}` of a string queue, `{<<bit{q}} = v` into a dynamic
  array, a stream as the source of a streaming assignment, `inside` on
  strings and handles, `wait (0)`, `%p` of class handles.
- A run-time error (null object access, a crash) names the statement that
  was running; `MSIM_CALLTRACE=1` prints every method and automatic
  subroutine call.

### Fixed
- A cast of a narrow unsigned operand inside a signed expression,
  `int'(u3) % 5`, sign-extended the operand: 6 became -2. The operand is now
  extended with its own signedness, as an assignment to the cast type
  (6.24.1).
- A signed member of an unpacked struct (`int`, `byte`) read as unsigned, so
  `s.f < 0` was false for a negative member (7.2).
- `super.f()` with a comment just before `super` called the override (and
  recursed) instead of the base method.
- An extern method that overrides a pure virtual method, or a virtual
  method without repeating `virtual`, did not get its vtable slot.
- A default argument that calls a method (`trigger(T d = get_default())`)
  called it on the caller's object instead of the callee's.
- A static property read by another static initializer before its own
  initializer ran was x; defaults (null, 0) are now set before any
  initializer.
- `&&`, `||` and `?:` evaluated the operand they skip, so
  `h != null && h.q.exists(k)` failed on a null `h`.
- A fork in a class method lost `this`.
- `wait (cond)` on automatic variables inside a subroutine could be turned
  into an `@*` wait.
- A process still waiting at the end of the simulation could corrupt the
  heap when it was freed.
- Virtual prototypes: `msim -mvp` adds an AXI4-Lite bridge (`mvp_axil_slave`,
  `mvp_irq_slave`, `mvp_clock`) from the design to models written in C, C++ or
  SystemC/TLM-2.0, with a memory map, 32 interrupt lines, timed callbacks and
  a master path for a model that drives the bus itself (`include/mvp.h`,
  `mvp_tlm.h`). A memory and a 16550-style UART are bundled in `mvp/models/`.
  See `docs/VPROTO.md`.
- SDF back-annotation (32): `$sdf_annotate` and `msim -sdf` read SDF 3.0
  files and annotate specify paths (IOPATH, COND, CONDELSE, DEVICE),
  interconnect delays (PORT, NETDELAY, INTERCONNECT), timing check limits
  (Table 32-2) and specparams (LABEL), with ABSOLUTE / INCREMENT, min:typ:max
  selection, scale factors, a config file and a log file. `+mindelays`,
  `+typdelays` and `+maxdelays` pick from min:typ:max (11.11).
- Specify path delays pick the delay of the paths whose input changed last
  and whose condition holds (30.5.3), so paths to one output may differ;
  6- and 12-value path delays (30.5.1, 30.5.2).
- Configurations and libraries (33): `msim -libmap`, configs as the top
  (with or without `-top`), library tops, `-L`, and `%l` / `%L`.
- Spec tests can pass msim arguments with a `//! args:` line.
- `%p` of containers of unpacked structs, unions, enums and packed structs
  (21.2.1.6), at any nesting depth.
- Timing checks (31): all twelve checks, edge-control specifiers, notifiers,
  `&&&` conditions and vector terminals; `+notimingcheck` turns them off.
  Negative `$setuphold` / `$recrem` limits with delayed signals under
  `+neg_tchk` (31.9).
- Checkers (17): formals and defaults, `$inferred_clock`, outputs, assertions,
  `always_ff` on sampled values, checker variables and free variables, all
  in the reactive region set. Before this, checker instances were ignored.
- Programs (24): program code runs in the reactive region set, programs end
  with an implicit `$finish`, `$exit`, nested and anonymous programs.
- Gate-level switches (28): `bufif0/1`, `notif0/1` with L/H strength
  resolution, `nmos`/`pmos`/`cmos` and their resistive forms, and the
  bidirectional `tran`/`rtran`/`tranif0/1`/`rtranif0/1`.
- User-defined primitives (29): combinational, level- and edge-sensitive.
- Arrays of instances and of interfaces (23.3.3, 25.3), including
  multidimensional instance arrays and per-element port slicing.
- System tasks and functions (20, 21): the other `$dist_*` functions (Annex N),
  `$system`, the array query functions on every kind of dimension,
  `$typename`, and the VCD tasks (`$dumpfile`, `$dumpvars` with a level and
  scope selection, `$dumpoff` / `$dumpon` / `$dumpall` / `$dumpflush` /
  `$dumplimit`). A design's `$dumpvars` now writes its dump file without
  `-vcd`.
- `$fstrobe`, `$fmonitor` (several at once, cancelled by `$fclose`) and
  `$fread` (21.3.2, 21.3.4.4).
- VPI (clauses 36-38): `vpi_user.h`, VPI libraries loaded before
  elaboration (`-load`, `-loadvpi`, or C files on the command line), user
  system tasks and functions, the module / net / variable / memory /
  parameter / port object model, `vpi_get_value` and `vpi_put_value` in all
  the common formats, and callbacks. The tests match Icarus 13 VPI output.
- DPI-C (clause 35): `import "DPI-C"` functions and tasks (`pure`, `context`,
  C names), `export "DPI-C"` functions and tasks, every argument type
  including packed vectors and open arrays, context scopes, exported tasks
  that wait on time, and the 35.9 disable protocol. `include/svdpi.h` is
  installed. msim builds C/C++ files given on its command line, loads
  libraries with `-sv_lib` / `-sv_root` / `-sv_liblist` (annex J), and writes
  prototypes with `-dpiheader`. msim now links libffi.
- `chandle` variables (6.14).
- `repeat (n)` with a count known only at run time (12.7.2).
- randomize() arguments that are arrays or containers (18.11), and whole
  fixed-size array properties through a handle: `h.a = '{...}` and `%p`
  of `h.a` (8.4).
- Fixed-size arrays of containers (`int q[4][$]`) as class properties,
  rand properties and automatic variables (7.4, 18.4), with `%p`, foreach,
  object copies and access through a handle.
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
