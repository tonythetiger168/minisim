# Changelog

All notable changes to minisim are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/) (see `docs/RELEASING.md`).
Spec references are IEEE 1800-2023 clauses.

## [Unreleased]

### Changed
- License: the repository is Apache-2.0 (`LICENSE`, attribution in `NOTICE`)
  instead of MIT. The MIT grant is not withdrawn: copies obtained under it
  stay under it, and so does the git history. `docs/LICENSING.md` holds the
  open-core policy, the proposed free / paid split per tool, and the rule
  that no GPL or other copyleft code enters the tree.

### Added
- `meco -spare` (and `-spare-cells GLOB`): metal-only ECO. The patch is
  built from the spare cells of the old netlist and the cells the change
  frees, by function (the same cell, or one with inputs tied or shared, or
  the inverse plus a spare inverter); no cell is added or removed, and the
  script holds only net connections. Exit status 3 when a cell had to be
  added after all. See `docs/ECO.md`.
- `mdebug -active`: a read of one struct field or of fixed bits (`p.hi`,
  `q[3:0]`) follows only that part: its value, the time those bits last
  changed, and the assignments that write them. The viewer page does the same.
- `msim -vcd-arrays [N]`: unpacked array elements (memories of up to N
  elements, default 1024) in the VCD or FST, as `mem[3]`. Off by default;
  the default waveform is unchanged. `mdebug -active` follows a memory read
  to the element and the write that filled it.
- `mpnr -lib FILE -sdc FILE`: timing of the placed design with the engine
  behind `msta` (wires estimated from the placement), reported after global
  placement and after legalization with the clocks propagated through
  their trees (setup and hold WNS/TNS, skew per clock), and timing-driven
  placement: nets on slow paths are weighted in a second placement pass
  (`-no_timing_driven` to only report).
- `mpnr` clock tree synthesis: every clock net with more loads than
  `-cts_fanout` (16) gets a tree of buffers, clustered by position level by
  level down to one root buffer, so every flop sits behind the same number
  of buffers. On by default (`-no_cts`); `-cts_buf CELL`, `-clock NET`. The
  buffers are in the DEF and the written netlist, and the run reports the
  shortest and longest driver-to-load wire of each tree.
- `mdebug`: struct fields in the active-driver trace (conditions on a field
  are evaluated; each field and constant part select has its own driver).
- `mpnr` on `lib/mini45`: a pin too small for the LEF's via pad gets a via
  made for it (lower pad turned or lengthened, written to the DEF's `VIAS`),
  short pieces of wire and the pads of power via stacks are lengthened to
  the layer's `AREA`, and a layer's `OFFSET 0` is taken as given (it used to
  mean half a pitch). `bench/gen_pnr.py --mini45` writes netlists over the
  library's cells; the `mpnr` test places, routes and DRC-checks one.
- `mplan`: `cond` and `fsm` plan metrics (condition vectors of files, states
  and transitions of FSMs), in the totals, the history and `mplan init`.
- `mdebug -blocks INST`, and a block diagram in the viewer: the instances
  inside a scope, their port connections and which instance feeds which.
- `mdebug`: tasks and functions in a module, call output arguments, `force`,
  procedural `assign` and net declaration assignments are drivers and loads;
  the fan-in cone follows a task argument to its calls.
- `memu`, emulation and FPGA prototyping: puts a gate-level netlist
  (structural Verilog over a Liberty library) on a board of FPGAs. Clock
  conversion onto one free-running clock (strobes for clean clocks, edge
  detection for gated, divided and data-read clocks, any number of unrelated
  clocks), memory mapping to block RAM and LUT RAM, multilevel partitioning
  under each FPGA's capacity on a board file, routing through FPGAs and
  time-division multiplexing where wires are short, probes with a trigger
  and `memu -trace` to a VCD, and a generated testbench that runs the
  netlist against the emulation. Transactors (`emu/sv`): an AXI4-Lite slave
  that carries the design's accesses to `msim -mvp` models and an AXI4-Lite
  master driven by a testbench class through a virtual interface, both
  stopping the design's clock while the host answers. Multi-threaded with
  thread-independent results. See `docs/EMULATION.md`.
- `mformal -aiger FILE` writes the model as AIGER 1.9 (bad-state
  properties for assertions and covers, invariant constraints for
  assumptions), so other model checkers can run on it or cross-check a
  verdict.
- `mformal`: fairness. A strong obligation in an `assume`
  (`assume property (req |-> s_eventually !req)`) restricts the loops a
  liveness counterexample may use.
- `mformal` is faster on larger designs: PDR lifts each state to the bits
  that matter, and a short induction with a small PDR budget runs before the
  deep search (8-way arbiter liveness 24 s to 4 s, a failure 300 cycles deep
  6.6 s to 1.7 s, one thread). It solves through `src/sat.hpp` and takes its
  threads from `src/parallel.hpp`. Benchmark designs are in `bench/formal`.
- `mformal`: a PDR (IC3) engine proves what k-induction cannot and finds
  failures deeper than `-depth`; liveness (`s_eventually`, `s_until`,
  `strong()`, `always s_eventually`) is proven or falsified with a loop
  counterexample.
- `mformal`: formal property checking on the msim front end. Proves or
  falsifies concurrent and immediate `assert`, takes `assume` as input
  constraints, reaches `cover` or proves it unreachable. Bounded model
  checking and k-induction (simple path, proven assertions as lemmas) on
  CaDiCaL; a VCD of the whole design for every counterexample and cover
  trace, `-tb` for a testbench that replays it in msim, `-json`; one job per
  property on every core (`-threads N`), same result for any thread count.
  See `docs/FORMAL.md`.
- `mplan`: verification planning and tracking. A plan file maps features to
  tests, code coverage, covergroups and assertions; `mplan report` scores it
  against mregress results and the coverage database, keeps a history of
  snapshots (trend chart, regressions per feature), sorts failures into
  buckets (new / recurring / flaky / fixed, features and owners blocked,
  rules for known and waived failures) and writes `plan.json` and
  `plan.html`. Also `mplan check`, `triage`, `trend`, `init`, the gates
  `-gate` / `-min-score` / `-fail-on-new`, and `mregress -plan`. See
  `docs/MPLAN.md`.
- `mlint` clock- and reset-domain crossing checks, rebuilt on a signal graph
  of the elaborated design: `CDC001` to `CDC005` (no synchronizer, multi-bit
  bus, logic in front of a synchronizer, reconvergence, a signal synchronized
  twice) and `RDC001` to `RDC004` (a reset from another clock domain, one
  reset used raw in several domains, a flop feeding a flop with another reset
  or with none). Synchronizers in one block, with a reset, as shift registers
  or as per-bit synchronizer cells, enable-qualified buses and Gray-coded
  pointers are recognized; clock gates stay in their clock's domain and bits
  of a clock bus are separate clocks. New options: `-cdc-report`,
  `-clock-group`, `-static`. See `docs/CDC.md`.
  `-clock-group`, `-static`, `-enable`, `-threads`. See `docs/CDC.md`.
- `mpopt`: RTL power optimization. Finds clock gating from enables (CG),
  sequential clock gating from registers whose sources did not load in the
  cycle before (SCG), memory gating (MG) and operand isolation of multipliers
  and dividers (OI); rewrites the RTL source in place (`-o dir`, with the
  `mpopt_icg` clock-gate model or a library cell via `-icg_cell`), and
  `-verify` co-simulates original and rewritten RTL in msim on random
  stimulus, comparing every output and register every cycle. Multithreaded
  (`-threads N`), same result for any thread count. See `docs/POWEROPT.md`.
- Shared headers for the tools: `src/parallel.hpp` (default thread count,
  the `-threads N` option, `par::parallelFor`, and `par::Pool` with `run`,
  `chunks` and an order-stable `sum`) and `src/wave_reader.hpp`
  (`wave::Reader`: a parallel, zero-copy VCD reader and an FST reader).
- `meco`, a functional ECO tool: given the implemented gate netlist and a
  netlist of the revised design, it writes the smallest patch it can find
  (patched netlist plus a Tcl edit script) and proves the result equivalent.
  It looks for the gate or pin to change before copying any logic, reuses old
  nets proved to have the same function, handles added, removed and replaced
  registers, scan flops (`-tie se=0`) and black boxes, and gives the same
  patch for any `-threads`. See `docs/ECO.md`.
- `mequiv`, a logic equivalence checker (RTL against netlist, netlist against
  netlist, RTL against RTL): both designs become one and-inverter graph,
  flops and latches are matched by name (or by the net on their output, also
  when stored inverted) and cut, and every output, next-state function and
  clock/reset is proved equal by structural hashing, random simulation and
  SAT on all cores, with a counterexample for each failing point. X in the
  reference is a don't-care. `-blackbox`, `-match`, `-effort`, `-threads`,
  `-report`. See `docs/EQUIV.md`.
- `mpnr`, place and route for gate-level netlists: an automatic floorplan or
  one from a DEF (`-floorplan`), analytic global placement, Abacus
  legalization, refinement and filler cells, a negotiated-congestion detailed
  router on the LEF track grid, and DEF out with a built-in check for opens,
  shorts and spacing (`-check FILE.def` checks any routed DEF). Multithreaded
  with identical results for any `-threads`. `-gds` writes the layout as GDSII
  (LEF cell abstracts or real cells from `-gds_cells`, layer map with
  `-gds_map`). Vertical power straps with via stacks tie the rows' rails
  together (`-no_straps`, `-strap_layer`, `-strap_width`, `-strap_pitch`).
  See `docs/PNR.md`.
- `mdebug`, source-level debug on the msim front end: `-drivers` and `-loads`
  of a signal with the `if` / `case` conditions on each assignment, `-trace`
  for the fan-in cone across ports, `-active SIG -at T` with `-vcd` for the
  assignment that gave a signal its value at a time and its causes back
  through time, and `-html` for a one-file viewer (hierarchy, clickable
  source, drivers and loads, schematic, waves with values annotated in the
  source). `-vcd` reads VCD and FST; the page traces the active driver at the
  cursor itself. See `docs/DEBUG.md`.
- `mrcx -corner NAME`: several extraction corners in one run. The options after a
  `-corner` (`-itf`, `-temp`, `-rscale`, `-cscale`, `-ccscale`) are that corner's; the
  design is read once and each corner goes to `design.NAME.spef`.
- `mrcx`, parasitic RC extraction: a routed DEF and its LEF in, SPEF (IEEE
  1481) out. Per net a distributed RC network with capacitance to ground and
  coupling capacitance to same-layer neighbours (who also take over part of
  the ground fringe) and to crossings one and two layers away, from an
  ITF-style process stack (`-itf`, `-map`, `-temp`) or the LEF values; scale
  factors, decoupled output, per-net report, open and short checks
  (`-Werror`). Multi-core by default (`-threads N`), same file for any thread
  count. `ETCH` on a conductor gives the width as made. `src/spef.hpp` is the
  SPEF reader and writer for the other tools.
  See `docs/RCX.md`.
- `lib/sky130`: cells of the open SkyWater 130 nm library (Apache-2.0) as a
  second test library: a 47-cell Liberty subset with its LEF and technology
  LEF, `sky130.py fetch` for the whole library at three corners (pinned
  commits, SHA-256 checked), and a `sky130` test that runs `mlibc`, `msynth`,
  `msta` and `mpower` on it. `mlibc` no longer expects a LEF pin for a
  Liberty `internal` pin, and `mlibc report` fits long cell names.
- `mchar`, cell library characterization on `mspice`: from a Liberty
  template and one transistor `.subckt` per cell it measures delay and
  transition tables (combinational, clock-to-output, set and reset arcs,
  tristate enable), setup, hold, recovery and removal tables and minimum
  pulse widths by bisection, internal energy,
  pin capacitance and leakage, and writes Liberty with the shared writer.
  The stimulus for each arc is found from the Liberty functions and `ff` /
  `latch` groups. Every table point runs as its own task on all logical
  CPUs (`-threads N`) with byte-identical output. `lib/mini45` gains
  transistor netlists (`mini45.sp`) and device models per corner, and its
  timing, power and capacitance tables are now measured, not analytic
  (leakage and tristate disable arcs stay analytic). See
  `docs/CHAR.md`.
- `mlibc`, cell library checks and reports on the shared Liberty and LEF
  readers: `mlibc check` (23 rules: function against timing arcs and
  `timing_sense`, tables that shrink with load, missing setup or hold,
  leakage states, drive strengths out of order, and Liberty against LEF),
  `mlibc report` and `mlibc compare slow.lib fast.lib`. Multithreaded, with
  the same output for any `-threads`. See `docs/LIBCHECK.md`.
- `lib/mini45`: a generic 45 nm-class standard cell library for the tool
  line, written from scratch (not a foundry library): 88 cells (inverters,
  buffers, NAND/NOR/AND/OR up to 4 inputs, XOR, AOI/OAI, muxes, adders,
  flops with reset, set, enable and scan, latches, clock gates, tristate, tie
  and filler cells, several drive strengths) as Liberty at three corners
  (`mini45_tt.lib`, `_ss`, `_ff`: NLDM 7x7 delay tables, constraints,
  internal power, state-dependent leakage, wire loads), Verilog models
  (`mini45.v`), and technology and cell LEF. See `docs/MINI45.md`.
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
- Logic synthesis: `msynth`, a dc_shell-style Tcl shell (`analyze`,
  `elaborate`, `create_clock`, `compile`, `report_timing`, `report_area`,
  `report_qor`, `write -format verilog` ...) on the slang front end. RTL is
  flattened into an and-inverter graph with flops (`src/aig.hpp`,
  `src/aig_elab.hpp`, shared with the equivalence, formal and FPGA tools),
  mapped onto any Liberty library by cut matching for area or delay,
  high-fanout nets are buffered, and the result is timed with the library's
  NLDM tables. The default library is mini45. Mapping runs on every
  core (`-threads N`) with the same netlist for any thread count. See
  `docs/SYNTH.md`.
- `aig::Design::mems`: the shared RTL-to-AIG front half lists every memory
  with its write ports (clock, enable, address, data, byte lane) and read
  ports, for block RAM inference; `msynth`'s `report_resources` shows them.
- `msynth` area: registers nothing reads or that are stuck at their reset
  value are removed (`aig::sweep`), nested selects fold into one
  (`aig::Graph::mux`), load enables use the library's enable flops, and
  wide selects use 4:1 mux cells; logic that computes the same function
  twice is merged by simulation and SAT (`src/synth/fraig.hpp`); `'x` in a
  select is a don't-care, and registers without reset that only hold a
  constant are removed. PicoRV32 on mini45: 30669 to 18098 (Yosys with
  ABC: 15458, in 7 times the run time); the ALU test design: 6536 to 4682.
  See `docs/SYNTH.md`.
- `mhls`: high-level synthesis, C/C++ to SystemVerilog. A C subset (integer
  types up to 64 bits including `ap_int`/`ap_uint`, arrays, inlined
  functions, all control flow but `goto`) is scheduled into clock cycles
  under `-clock`, `-mul` and `-div`, with operator chaining, multi-cycle
  operators, register and multiplier/divider sharing, and comes out as an
  FSM with datapath or (`-pipeline`) a pipeline taking one input per cycle.
  `-tb` writes a self-checking testbench for `msim`, `-cref` checks the same
  calls with the host C++ compiler, `-explore` sweeps the clock period.
  See `docs/HLS.md`.
- `mhls`: structs. Nested structs, arrays of structs, array members and
  bit-fields; structs copied, passed by value, pointer or reference, and
  returned. Each member becomes its own register, memory or port.
- `mhls`: pipelined loops. `#pragma hls pipeline` first in a loop body, or
  `-pipeline-loops` for every loop that allows it, starts a new iteration
  every cycle: `n` iterations of an `L`-stage body take `n + L - 1` cycles
  instead of `n * L`. A loop that cannot be pipelined is refused with the
  rule it breaks.
- `mhls`: pipelined loops at slower rates. A loop that cannot start an
  iteration every cycle (a memory updated in place, a pointer chase, an exit
  that depends on memory data) is pipelined at one iteration every 2 or more
  cycles when that beats running it unpipelined; `#pragma hls pipeline` now
  also works first in a `do` body.
- `mhls`: a variable that only feeds itself (a counter or checksum nothing
  reads) is removed with its register and operators.
- `mhls -handshake`: also writes `<top>_hs`, the block behind
  `in_valid`/`in_ready` and `out_valid`/`out_ready` ports (FSM and pipeline);
  the generated testbench drives it with a sink that refuses now and then.
- `mhls`: stream arguments. A `hls::stream<T>&` parameter becomes a
  `tdata`/`tvalid`/`tready` port; `read()` and `write(x)` are transfers the
  state machine waits for. `include/mhls_stream.h` runs the same source on
  the host; the testbench source and sink hesitate on every other call. A
  pipelined loop over streams moves one element per cycle and stalls as a
  whole.
- `mhls -clang`: C++ classes and templates. The installed clang (run as a
  program) reads the source; mhls reduces its syntax tree to plain functions
  over structs: member functions, constructors, operators, class and
  function templates, default arguments, overloads, static members, scoped
  enums, namespaces, and whatever the design uses from its headers
  (`std::min`, `std::max` included). `-emit-c` shows the reduction.
- `mhls -clang`: inheritance. A base class is the first member of the
  derived struct; base constructors, inherited and hidden members, several
  bases, template bases and objects passed as their base all work. Classes
  whose only constructor is the implicit one now run their members'
  constructors. Virtual functions in a hierarchy are refused.
- Functional coverage in the coverage database: `msim -cov` records the bins
  of every covergroup type (19.11) and the statistics of every assertion
  (16.3); `mcov` merges and reports them (`-min-func`, `-min-assert`).
- Datapath library `dw/`: 47 synthesizable, parameterized SystemVerilog
  building blocks named `MW_*` (adders with five carry architectures,
  Booth/Wallace and array multipliers, multiply-accumulate, dividers, square
  root, barrel shifters, leading-zero and priority logic, Gray code, ECC,
  CRC, LFSR, counters, synchronous and dual-clock FIFOs, arbiters). Use with
  `msim -y dw +libext+.sv`; `docs/DATAPATH.md` lists every module.
- The `matpg -tb` testbench writes a fail log with `+faillog` (pattern, chain
  and cell of every wrong bit) for diagnosis; the fault simulator queues
  events by level and is up to twice as fast on deep logic; the pattern file
  reader and writer are in `src/atpg_model.hpp` for other tools.
- `matpg -cubes FILE` writes each test before the random fill (x for a free
  bit) for test compression, and the fault simulator can report where each
  pattern fails (`Sim::detect` with a third argument) for diagnosis.
- `matpg`: test pattern generation and fault simulation on a scan-inserted
  gate netlist. Stuck-at faults and launch-on-capture transition faults on
  every cell pin; scan chains traced from the ports (library scan flops or a
  mux on D); random patterns, PODEM, a 64-pattern parallel fault simulator
  and reverse-order compaction; fault classes (DS, DI, UU, UR, AU, ND) with
  test and fault coverage; patterns as STIL (IEEE 1450), as a self-checking
  Verilog testbench and as text; `-faultsim` grades existing patterns;
  `-lib` takes cell functions from Liberty; `-threads N` with the same
  result for any N. The fault simulator is `src/faultsim.hpp` for the other
  test tools. See `docs/ATPG.md`.
- `-xprop` is faster: an `if` evaluates its condition once, and clocked
  blocks with merged branches run in parallel again. Ibex with `-xprop`:
  42 s to 26 s in the interpreter, 9.8 s to 7 s with `-compile`.
- `-xprop`: an assignment to an array element chosen at run time
  (`if (we) mem[addr] <= d;`) is merged with the element's present value
  instead of blocking the merge or giving `x`.
- X-propagation: `msim -xprop` (`-xprop=tmerge`, `-xprop=xmerge`,
  `-xprop-report <file>`). In `always` blocks an unknown `if` condition,
  `case` selector or clock / reset edge no longer picks one branch: every
  branch that may be taken runs and the assigned variables get the merge of
  the results, for blocking and nonblocking assignments. Same results in
  the interpreter, with `-compile` and for every `-threads` count. See
  `docs/XPROP.md`.
- Foreach constraints with several loop variables over containers of
  containers, `foreach (q[i, j]) q[i][j] inside {[1:5]};` (18.5.8.1), also
  over a fixed-size array of queues and associative arrays with string keys.
- VPI: `vpi_handle_by_name` of a part select (`top.u.r[3:0]`, a
  `vpiPartSelect`, 38.27) reads, writes, forces and releases just those
  bits, so UVM's `uvm_hdl_*` backdoor works on part-select paths.
- Condition and FSM coverage (as VCS `-cm cond+fsm`): `msim -cov` (or
  `-cm <metrics>`) also records, for every `&&` / `||` / `?:` condition, the
  vectors in which each term decides the result, and, for every state
  register it extracts (two-process, one-process and `if (state == S)`
  styles; enum, parameter and literal states), the states entered and the
  transitions made out of those the next-state logic allows. Works in the
  interpreter and with `-compile`, with any `-threads`, with the same
  database. `mcov report` adds `Condition:` and `FSM:` lines, `-detail`,
  `-uncovered`, `-fsm-reset-filter`, `-min-cond`, `-min-fsm-state`,
  `-min-fsm-trans`, and FSM and condition sections in the HTML report;
  `mcov merge` merges both. One-hot FSMs tested bit by bit
  (`case (1'b1)` on `state[K]`, `if (state[K])`) are extracted, and
  conditions with a term that calls a function or reads through a class
  handle are covered with short-circuit vectors.
- `mdft -insert_scan`: scan insertion on a gate netlist. Flops are found
  through the Liberty library (`-lib`: ff groups, `test_cell` scan pins, clock
  gates) or a cell map, checked for a controllable clock and asynchronous
  set/reset, and stitched into balanced mux-D chains: scan cell swap or a
  library mux before D, one clock domain per chain or `-mix_clocks` with
  lockup latches, clock-gate test enables tied to `scan_en`. Writes the
  netlist, a JSON chain description for ATPG (`-scandef`) and a chain flush
  testbench (`-tb`). Multithreaded, same output for any `-threads N`.
- `mdft -mbist`: memory BIST generation for a synchronous single-port RAM:
  an at-speed March controller and a collar, eight built-in algorithms
  (MATS+, March X, Y, C-, A, B, LR, SS) or `-march`, data backgrounds, read
  latency 0 to 16, first-fail address and data. A built-in memory fault
  simulator reports stuck-at, transition, coupling and address-decoder
  coverage, and `-tb` writes a testbench of fault-injecting RAM models whose
  expected results come from that simulator. See `docs/DFT.md`.
- `mpss`, portable stimulus: reads a model in the Accellera PSS language
  (components, actions, buffers, streams, resources and pools, constraints,
  activities with `parallel` / `schedule` / `select` / `repeat`, exec blocks
  and functions), solves scenarios (field values, resource instances, and
  the actions inferred to feed unbound inputs), and writes each scenario as
  a UVM sequence class (`-sv`, runs under `msim -uvm`) and as a C test
  function (`-c`), plus `-print` and `-json`. Scenarios are solved on all
  cores (`-threads N`) with the same output for every thread count. See
  `docs/MPSS.md` and `examples/pss/dma.pss`.
- `src/interval.hpp`: the interval arithmetic and bounds propagation of the
  `randomize()` solver, moved out of `rand.inc` so that `mpss` uses the same
  rules.
- Low-power simulation with UPF (IEEE 1801-2024): `msim -upf file.upf`
  (`-power_top`, `-upf_verbose`) reads power domains, supply ports, nets and
  sets, power switches, isolation, level-shifter and retention strategies,
  power states and legacy PSTs; powered-off logic stops and is corrupted,
  isolation clamps, retention saves and restores, illegal power states and
  PST violations are reported. Package `UPF` (`supply_on`, `supply_off`,
  `set_power_state`, `get_supply_value`, ...) is added for the testbench.
  `mlint -upf` checks the power intent: UPF001 to UPF006. See
  `docs/LOWPOWER.md`.
- `mpv drc -svrf rules.svrf` reads rule files in the SVRF style that foundry
  decks use (`LAYER`, `LAYER MAP`, `AND`/`OR`/`NOT`/`XOR`, `INTERACT`,
  `INSIDE`, `OUTSIDE`, `SIZE`, `HOLES`, `RECTANGLE`, rule blocks with `INT`,
  `EXT`, `ENC`, `AREA`, `DENSITY`, `#IFDEF`, `INCLUDE`) and translates them
  to the native deck (`-write-deck`). Options it does not apply are listed
  in warnings; statements it cannot run are errors with their line. Decks
  can now name every datatype of a layer (`layer m1 68/*`).
- `mpv drc -rdb drc.lyrdb` writes the violation markers as a KLayout report
  database, to step through them in KLayout's marker browser.
- `mpv drc`: `enclosure(a, b, opposite) < v` passes a contact or via that
  has the margin on two opposite sides (the foundry rule "on one of two
  adjacent sides"). The SKY130 test deck gains li.5, licon.5c, m1.5,
  via.5a and m2.5 (40 rules).
- `mpv` reads OASIS layouts (SEMI P39) as well as GDSII, told apart by the
  file's first bytes: every record, repetition and point-list form, compact
  trapezoids, name tables, strict mode and compressed blocks (built-in
  DEFLATE). `src/oasis.hpp`; `oasis::readLayout` reads either format into a
  `gds::Library`. Checked against KLayout on random files and SKY130 cells.
- Fixed: the GDSII writer (`gds::write`) wrote STRANS as an integer record;
  it is a bit array, and KLayout refused files with mirrored references.
- `mpv lvs -lef tech.lef routed.def -netlist block.v`: LVS of a placed and
  routed block against its gate netlist. The nets come from the geometry
  (cell pins from the LEF, wires, vias and block pins from the DEF), not from
  the DEF's connection lists; opens, shorts, missing or different cells and
  pins are reported, and `-o` writes the extracted netlist as Verilog.
- `mpv drc -lef tech.lef routed.def`: design rule checking of a routed DEF,
  with the rules taken from the LEF technology when no `-deck` is given
  (`-write-deck` shows them).
- `mpv lvs`: extracts the transistor netlist of a layout (nets from `connect`
  statements, names and ports from `label`, MOS devices with W and L from
  `device mos`) and writes it as SPICE. `src/lvs.hpp` also holds the netlist
  comparison (partition refinement, parallel merging, source/drain
  exchange). `tests/pv/sky130.lvs` is the SKY130 deck.
- `mpv drc`: design rule checking on GDSII layouts. A rule deck defines drawn
  and derived layers (`&`, `|`, `-`, `^`, `grow`, `shrink`, `interact`,
  `inside`, `outside`, `holes`, ...) and width, spacing, enclosure, overhang,
  area and density checks; violations are written as markers. Multithreaded
  by default (`-threads N`) with the same report for every thread count. A
  35-rule SKY130 deck and `sky130_fd_sc_hd` cells are the tests. `src/gds.hpp`
  is the shared GDSII reader and writer. See `docs/MPV.md`.
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
- `mdft -bscan`: IEEE 1149.1 boundary-scan insertion (TAP controller,
  instruction, BYPASS, IDCODE and boundary registers, 2-state, 3-state and
  bidirectional pads, EXTEST / SAMPLE / PRELOAD / CLAMP / HIGHZ / INTEST)
  and the BSDL file; `mdft -bsdl` checks a BSDL file (rules BSD001..009) and
  writes its device tests as SVF and a self-checking testbench
  (`docs/JTAG.md`).
- `mdft -icl`: IEEE 1687 IJTAG. Reads ICL, writes the scan network as RTL
  (optionally behind a TAP), and retargets PDL (`iWrite`, `iRead`, `iApply`,
  `iCall`, ...) through SIBs and scan muxes to SVF and a testbench.
- `mdft -lbist`: logic BIST for a scan-inserted design (PRPG on a primitive
  polynomial, phase shifter, space compactor, MISR, controller and wrapper),
  the golden signature by simulation (`-lbist_sign`), a check for X sources,
  and ICL / PDL to run it over IJTAG.
- `mdft -edt`: scan test compression. A ring-generator decompressor and an
  XOR compactor around the scan chains, and an encoder that solves test
  cubes into channel data over GF(2), in parallel and deterministically.
  With the gate netlist it simulates each compressed pattern and writes the
  expected output channels and a load / capture / compare testbench;
  `-edt_xmask` adds compactor X masking (one chain per channel observed,
  mask chosen per pattern) so unknown captures do not hide known ones.
- `mdft -lbist_coverage`: stuck-at fault coverage of the logic BIST patterns
  on the gate netlist (port inputs at 0, scan cells observed), with the
  coverage after 64, 128, 256, ... patterns, a fault list, JSON and a
  `-lbist_min_coverage` gate. Uses the shared fault simulator.
- `mdft -diagnose`: scan failure diagnosis. Reads a tester fail log (pattern,
  chain, cell or output) with matpg's patterns and the gate netlist, and ranks
  the stuck-at or transition faults that explain it, with equivalent faults
  and a second, third... defect when one does not explain everything. Uses
  the shared fault simulator; multithreaded with the same report on any
  thread count. See docs/JTAG.md.
- `mspice`: process corners and statistics. `.lib 'file' section` reads a
  section of a library file, `.alter` blocks rerun the deck with lines
  replaced, and `.step monte N` (or `sweep monte=N`) runs Monte Carlo samples
  in parallel with `gauss`, `agauss`, `unif` and `aunif`: a top-level
  parameter varies globally, a subcircuit parameter per instance. Samples
  depend only on the seed and the sample number; mean, sigma, minimum and
  maximum of every `.measure` are printed.
- `mspice`: BSIM4 transistors (`.model ... level=14` or `54`). The model
  equations are the UC Berkeley BSIM Research Group's BSIM4 4.8.3 code,
  unmodified under the Educational Community License 2.0
  (`third_party/bsim4`, `THIRD_PARTY_NOTICES.md`), behind an adapter to
  mspice's solver: DC, AC and transient, gate/body/source-drain resistance
  networks and the NQS model, evaluated on all threads with identical
  results. Checked against ngspice (same equations, within 1 to 2%); a
  4,000-transistor chain runs in 3.2 s on one thread and 2.1 s on four
  against 5.8 s. Build option `MINISIM_WITH_BSIM4`.
- `mspice`: unknowns fixed by a voltage source to ground are eliminated
  first, which keeps the pivots of a long chain of gain stages usable.

### Fixed
- An inout port connected to a whole net is now that net (23.3.3.1). It was
  two continuous assignments, one each way, so the net kept the last value
  driven after every driver had gone to z.
- `src/sdc.hpp`: a reader for Synopsys Design Constraints (SDC 2.1) shared by
  the constraint, timing and synthesis tools. A small Tcl (variables, `expr`,
  `[...]`, `if`/`foreach`/`for`/`while`, `proc`, lists, `source`) fills a typed
  model (clocks, generated clocks, I/O delays, clock groups, exceptions,
  uncertainty, latency, case analysis) and keeps every command with its file
  and line. Object queries (`get_ports`, `get_pins`, `get_cells`, `get_nets`,
  `get_clocks`, `all_inputs`, `all_registers`, `-hierarchical`, `-regexp`,
  `-filter`, `-of_objects`) resolve through `sdc::Design`, which a tool
  implements for its own netlist; a host can add its own commands.
- `src/tcl.hpp`: the one Tcl interpreter of the tool line (`tcl::Interp`),
  split out of the SDC reader. A tool adds its own commands (`addCommand`),
  runs scripts (`load`, `parse`) or a prompt (`shell`), and gets every error
  as a diagnostic with file and line instead of a stop. `sdc::Reader` is now
  an `Interp` with the SDC commands added.
- `msdc`: timing constraint checks. Reads the design (same front end as
  `msim`) and its SDC file and reports what is wrong or missing: 32 rules
  from syntax (SDC001) through clocks, I/O delays and exceptions to paths
  between clocks with no declared relationship (SDC040). Multithreaded
  (`-threads N`, same report for any N), `-json`, `-Werror`, `-stats`. See
  `docs/CONSTRAINTS.md`.
- SDC reader: object lookups use sorted name indexes and `-filter` is parsed
  once, so large designs with thousands of queries read in seconds.
- `mlint` grows from 6 rules to about 80, in groups: latches and
  combinational logic (`L001` now bit-accurate, `LAT001`, incomplete
  sensitivity lists, combinational loops across modules), case statements,
  assignment style, widths and signs (`W001`..`W008`), synthesizability
  (`SYN001`..`SYN017`), unused and undriven signals, connections, clocks and
  resets (`CLK`, `RST`), naming (`NAME001`..`NAME013`, by pattern) and style.
  Every rule has a severity and an on/off default; `-config`, `-enable`,
  `-disable`, `-only`, `-severity`, `-set` configure them, `-list` and
  `-explain` describe them. Waivers: a waiver file (`-waivers`, by rule,
  file, line and message, stale ones reported) and comments in the source
  (`// mlint: waive RULE reason`, `mlint: off / on`, `translate_off`).
  `-json` report, `-show-waived`, `-stats`. Rules run on every logical CPU
  (`-threads N`), with the same output for any count. See `docs/LINT.md`.
- `msta`: static timing analysis of gate-level netlists in the style of
  PrimeTime. Liberty (NLDM) delay calculation, ideal and propagated clocks,
  generated clocks, setup / hold / recovery / removal and output delay
  checks, false paths, multicycle paths, max and min delay, clock groups,
  `max_transition` and `max_capacitance`; `check_timing`, `report_timing`,
  `report_constraint`, `report_clocks`, `report_qor`, SDF and JSON output;
  Tcl scripts on the shared SDC reader. Multithreaded, with the same results
  at any thread count. See `docs/STA.md`.
- `src/liberty.hpp`, `src/gnetlist.hpp`: a Liberty reader and a gate-level
  Verilog netlist reader / flattener / writer, shared by the timing,
  synthesis, power, equivalence and physical tools.
- `mlint` state machine rules on the extractor FSM coverage uses: `FSM001` a
  state never entered, `FSM002` a state with no way out, `FSM003` a state
  case with no default that leaves encodings unused, `FSM004` no reset state.
- `mpower`, power analysis on the msim front end: average power by group,
  instance and cell from SAIF, VCD or FST activity and Liberty
  `internal_power` / `leakage_power` tables; activity propagation for nets
  the file does not name (RTL activity on a gate netlist, or `-clock` with no
  activity); an RTL estimate (register bits as the library's flop, logic as
  reference gates); `-peak` with `-waveform` for power over time; `-upf` for
  power per domain with domain voltages and switched domains; `-spef` for
  extracted wire capacitance; `-json`.
  Multithreaded, with the same report on any thread count. `msim -saif`
  writes SAIF from a simulation, and `mpower -write_saif` converts a
  waveform. `src/saif.hpp` (SAIF reader and writer) and `src/power.hpp` (the
  computation) are shared headers. See `docs/POWER.md`.
- FPGA synthesis: `mfpga` maps RTL or AIGER to K-input LUTs and flip-flops
  (iCE40, ECP5, Xilinx 7-series, generic cells) on the shared graph of
  `src/aig.hpp`, and writes Yosys JSON for nextpnr, EDIF or Verilog.
  Depth-optimal priority-cut mapping with area recovery; multithreaded, with
  the same output for any `-threads N`. See `docs/FPGA.md`.
- `mfpga`: adders, subtractors and comparators map to the family's carry
  chain (`SB_CARRY`, `CCU2C`, `CARRY4`), and clock enables and synchronous
  resets to flop pins. `-nocarry` and `-noce` turn each off.
- `mfpga`: a multiplier that fits one DSP block maps to it (`MULT18X18D`,
  `DSP48E1`, and `SB_MAC16` with `-dsp`). `-nodsp` keeps LUTs.
- `mlint` runs slang-tidy's checks (the lint tool that ships with slang, MIT)
  as rules `TIDY001`..`TIDY021`, with mlint's severities, waivers, JSON and
  threads; the checks mlint already had a rule for are left out. See
  `docs/LINT.md` and `THIRD_PARTY_NOTICES.md`.
- `mlint` has 13 text and syntax rules that follow Verible's lint rules
  (`FMT001`..`FMT004`, `SUS001`, `STY004`..`STY009`, `NAME014`, `W009`):
  line length, tabs, trailing spaces, stray semicolons, `` `endif ``
  comments, package file names, explicit lifetimes and parameter types,
  range direction, macro names, short binary literals. Off by default; one
  task per file, same output at any `-threads`.
- `mspice`, a SPICE circuit simulator: `.op`, `.dc`, `.ac` and `.tran` on
  netlists with R, C, L, coupled inductors, independent and controlled
  sources (E, G, F, H), diodes, level-1 MOSFETs and Gummel-Poon bipolar
  transistors; `.model`, `.param` with expressions, nested `.subckt`,
  `.include`, `.ic`, `.nodeset`, `.option`, `.print`, `.measure` and `.step`;
  CSV (`-o`) and VCD (`-vcd`) output. Sparse LU with pivot reuse, latent
  device bypass, and threads for `.step` runs, AC points and model
  evaluation, with identical output at any `-threads`. Every test netlist is
  checked against hand-worked values and against ngspice. See
  `docs/SPICE.md`.
- `mspice`: MOSFET evaluation and matrix assembly run in parallel (a
  2,000-inverter chain goes from 1.14 s to 0.70 s on four threads, with the
  same output bit for bit); `parmin` now defaults to 1024. The reader gains
  `spice::flatten()`, a flat list of primitive elements for LVS and cell
  characterization, printed by `mspice -flatten [-top name]`.
- `mspice`: `.option chgtol=` (needed for accurate edges on femtofarad
  nodes) and `spice::Reader::readText()` for netlists held in memory.
  mspice now runs on the shared thread pool of `src/parallel.hpp`, which
  gains `Pool::spin()` for short batches that come one after the other.
- `mfpga`: a multiplier wider than one DSP block is split into block-sized
  partial products summed on carry chains (32x32 on ECP5: 4 blocks, no LUTs).
- `mfpga`: memories with registered reads map to block RAM on iCE40
  (`SB_RAM40_4K`) and ECP5 (`DP16KD`), with a bypass that keeps the RTL's
  read-during-write result. `-noram` keeps flops.

### Fixed
- `mformal` could report a liveness assertion proven when it fails:
  `s_eventually` of a sequence longer than one cycle (`s_eventually (a ##1 a)`
  with `a` alternating), and could have reported a loop for obligations that
  are met and renewed. Liveness now follows single obligations round the
  loop; what it does not decide (`s_eventually always q`) is reported
  `bounded`, not proven.
- `mlint`, `mdft`: a net connected to thousands of instances no longer makes
  the clock-domain union-find quadratic (path compression).
- A concurrent assertion read the current value, not the sampled one, of a
  bit select or indexed part select with a variable index (`r[k]`,
  `r[k -: 2]`, 16.5.1).
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
- `msta`: the hold check between clocks of different periods used a capture
  edge that was too early and reported too much slack. It now takes the
  tightest hold relation over the common period. Asynchronous set / reset to
  output arcs are no longer timed as paths by default
  (`-preset_clear_arcs` turns them on). Both were found by comparing with
  OpenSTA, which now agrees at every endpoint of a million-cell design to
  0.1 ps; see "Compared with OpenSTA" in `docs/STA.md` and `bench/sta/`.

### Changed
- `mlint` prints each finding's severity (`error`, `warning` or `info`) and
  names signals by their name in the module rather than by full path, so a
  module instantiated many times reports once. `-Werror` fails on warnings
  and errors, not on `info` findings. Modules that nothing instantiates are
  no longer linted as if they were part of the design.
- `mlint` and `mdft`: the structure that joins signals across ports now
  compresses paths. A clock reaching 4000 instances made every lookup walk
  4000 links: 13 s for a 120,000-line design, now 1.3 s.
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
