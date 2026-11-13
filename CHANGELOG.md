# Changelog

All notable changes to minisim are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/) (see `docs/RELEASING.md`).
Spec references are IEEE 1800-2023 clauses.

## [Unreleased]

### Added
- msim VHDL: unconstrained array ports (6.5.6.3). Each port's index range
  comes from the actual, ascending or descending, also through another
  unconstrained port; a SystemVerilog parent sets `<port>__L`/`<port>__R`.
- mvplat: Zbkb, Zbkx, Zabha, Zacas, Zfh (half precision), the CBO.*
  instructions (Zicboz in the device tree) and Svnapot. riscv-tests: 837
  programs (uzbkb, uzbkx, uzabha, uzacas, uzabha_zacas, uzfh, mzicbo and
  ssvnapot added).
- mvplat: Sv48 and Sv57 paging on RV64. `-mmu sv48|sv57` (or the platform's
  `"mmu"`) offers them in the device tree; Linux runs with Sv48. riscv-tests
  run RV64's ui, um, ua and uc under Sv48 too: 675 programs.
- mvplat: the B extension (Zba, Zbb, Zbs) with Zbc and Zicond on RV32 and
  RV64, in `misa` and the device tree; 588 riscv-tests now (uzba, uzbb, uzbc,
  uzbs, uzicond added, `p` and `v`).
- mvplat: virtio-console (VIRTIO 1.2, 5.3). `-vcon` adds one and sends
  `-input` to it; Linux runs its console on `hvc0`. Input is typed once the
  guest has printed and been quiet for 100 ms of simulated time.
- Transaction recording without UVM: Questa's `$create_transaction_stream`,
  `$begin_transaction`, `$add_attribute`, `$end_transaction` and Verdi's
  `$fsdbTrans_begin`, `$fsdbTrans_add_attribute`, `$fsdbTrans_end` write
  the text format of UVM's `uvm_text_tr_database` (`transactions.log`,
  `msim -tr FILE`), which `mdebug -tr` reads. `tests/msim_tr.cmake`.
- mvplat: virtio-net over virtio-mmio (VIRTIO 1.2, 5.1). `-net user` gives a
  built-in gateway at 10.0.2.2 that answers ARP and ping; `-net
  dgram:LOCAL,REMOTE` links platforms (or a host program) over Unix datagram
  sockets; `-net-pcap FILE` records the frames. Linux brings eth0 up and
  pings the gateway. Platform files take `"type": "virtio-net"`.
- mvplat: virtio-blk over virtio-mmio (VIRTIO 1.2, 4.2 and 5.2). `-disk FILE`
  puts a raw image on `/dev/vda` (repeat for more), `-disk-write` lets the
  guest's writes reach the file; platform files take `"type": "virtio-blk"`.
  Linux mounts an ext4 disk and boots with its root file system on it.
  Extension ABI 7.
- `msim -vcd-sva` and `-vcd-power`: assertions and power in the waveform.
  Each assertion and cover gets `.passes` and `.fails` counters; with `-upf`
  each power domain gets `upf.PD.simstate` and each supply net
  `upf.NET.state` and `upf.NET.mV`. `$fsdbDumpSVA` and `"+power"` in
  `$fsdbDumpvars` turn them on too. `docs/DEBUG.md`.
- VPI inter-module paths on a net with several sources each have their own
  delay (`vpi_put_delays` / `vpi_get_delays`, 38.21), and `msim -i` / `-do`
  runs `cbEnterInteractive`, `cbExitInteractive` and
  `cbInteractiveScopeChange` (38.36.3).
- Verdi, VCS and Xcelium waveform tasks: `$fsdbDumpfile`, `$fsdbDumpvars`,
  `$fsdbDumpon` / `off`, `$fsdbDumpMDA`, `$vcdpluson`, `$vcdplusfile`,
  `$vcdplusmemon`, `$shm_open`, `$shm_probe`, `$recordvars` and the rest
  compile on msim and write FST under the same name (the vendor formats are
  closed); msim, mdebug and mlint all accept them, and `msim -i` takes the
  UCLI commands `fsdbDumpfile` / `fsdbDumpvars` / `fsdbDumpoff`.
  `docs/DEBUG.md`, `tests/msim_vendor_dump.cmake`.
- `msim -maxtime` takes a unit: `-maxtime 3ms`, `500ns`, `1.5us` (before,
  only a count of time-precision ticks, so `3ms` stopped after 3 ticks).
- VPI assertion steps list their matched expressions
  (`matched_expression_count`, `matched_exprs`, 39.5.2), and `failExpr` of a
  concurrent assertion with sequence operators is the boolean that did not
  hold (39.4.2). `vpiDecompile` of a net, variable or parameter is its name.
- VPI parameters (37.28): type parameters (`vpiTypeParameter`) in
  `vpi_iterate(vpiParameter, module)`, a parameter's default through
  `vpiExpr`, param assigns (`vpiParamAssign`: the `#(...)` overrides with
  `vpiLhs`, `vpiRhs`, `vpiConnByName`) and def params (`vpiDefParam`).
  An instance array's `vpiExpr` is its connection list (a `vpiListOp`,
  37.11), and net declaration assignments are `vpiContAssign` objects with
  `vpiNetDeclAssign` (37.40). `tests/spec/37.28-vpi-param-assign.sv`.
- mvplat harts are RV32GC / RV64GC: the F and D extensions (IEEE 754 single
  and double, every rounding mode, `fflags`/`frm`/`fcsr`, NaN boxing,
  `mstatus.FS`/`SD`, the compressed FP loads and stores). 430 riscv-tests
  pass (uf and ud added). Linux runs Ubuntu's rv64gc BusyBox; `-input`
  reaches the shell once the kernel's UART driver is up. Extension ABI 5.
- `mlint -cdc-formal`: formal proof that the source of each bus synchronized
  bit by bit changes at most one bit per clock, by exhaustive search of its
  reachable states (slang's evaluator, through assigns, ports, functions and
  `always_comb`). A proved bus is not CDC002; a Gray-coded bus that can change
  more bits is the new rule CDC010, with the step. Proves all eight async
  FIFO pointers of OpenTitan `spi_device` and both of pulp `cdc_fifo_gray`.
- VPI expression values (37.35, 38.15): `vpi_get_value` on an expression
  object gives its value now, not only for a constant expression. The nets
  and variables it reads (memories too) take their current values, so
  operations, variable selects, conditionals, concatenations and calls of
  the design's functions have a value. `tests/spec/37.35-vpi-expr-values.sv`.
- `msim -upf` with VHDL: the VHDL value conversion tables of IEEE 1801-2024
  Annex B (`VHDL_SL2UPF`, `UPF2VHDL_SL`, the `_GNDZERO` ones, `VHDL_TIED_HI`,
  `VHDL_TIED_LO`) connect `std_logic` ports of VHDL entities to supply nets.
- mvplat boots Linux: `-bios`, `-kernel`, `-initrd`, `-append`, `-dtb` as on
  QEMU's virt board, a device tree made from the platform (`-dump-dtb`), and
  OpenSBI's `fw_dynamic` hand-off. OpenSBI 1.7 and Ubuntu 24.04's stock
  riscv64 kernel boot to user space on 1 to 4 harts. Instruction fetch with
  paging on keeps the last page translated, and loads and stores keep the
  last 64 pages each: 25 to 50 MIPS under Linux. Extension ABI 6.
- msim VHDL: protected types (classes), access types (`new`, `null`,
  `.all`, `deallocate`, linked lists through incomplete types, growable
  arrays), generic packages and package instantiation, VHDL-2019
  `std.env` queries, enumeration literals overloaded with functions, and
  procedures as void functions so VHDL functions can call them.
  `tests/vhdl/protected.vhd`, `access.vhd`, `generic_pkg.vhd` (GHDL goldens).
- VPI class definitions (37.31): `vpiVariables` of a class defn gives its
  automatic properties too (before only static ones), as declarations
  with `vpiAutomatic` and `vpiParent` whose value is refused (detail 2);
  `vpiNamedEvent`, `vpiParameter` (with `vpiLocalParam`), `vpiTypedef` and
  `vpiInternalScope` iterate its events, parameters, typedefs and methods.
  `tests/spec/37.31-vpi-class-defn.sv`.
- `mlint` CDC on generated register blocks: a register struct written field
  by field through ports on fields or on concatenations of fields, copied
  whole (`assign hw2reg_regtop = hw2reg`) and passed whole down a port, is
  kept field by field, so one field's crossing no longer spreads to every
  register. A clock behind a scan mux is the functional clock. `-static`
  takes patterns. OpenTitan `usbdev` goes from 162 findings to 12.
- `include/msim_upf.h`: C and C++ (SystemC, TLM) models called through
  DPI-C read the current power state and simstate of a power domain or
  supply set under `msim -upf` (IEEE 1801-2024 5.14).
- VPI containers of structs, classes and containers (37.18): an element of
  a queue, dynamic or associative array of unpacked structs is a
  `vpiStructVar` whose members read and write the element; one of class
  handles gives its object through `vpiClassObj`; one that is itself a
  container is a `vpiArrayVar` with its own size and elements.
  `tests/spec/37.18-vpi-nested-containers.sv`.
- VPI instance arrays (37.11): `vpi_iterate(vpiModuleArray /
  vpiInterfaceArray / vpiProgramArray / vpiPrimitiveArray /
  vpiInstanceArray, module)`, an array's name, size, ranges and elements,
  `vpi_handle_by_index` / `vpi_handle_by_multi_index` on it, and from an
  element `vpiIndex` and its array. Array elements are now named `u[2]`, and
  `vpi_handle_by_name` picks the right element of a descending array
  (`u[3:1]`). `tests/spec/37.11-vpi-instance-arrays.sv`.
- msim simulates VHDL and mixed VHDL/SystemVerilog designs (`docs/VHDL.md`):
  `.vhd`/`.vhdl` files are translated in memory into SystemVerilog for the
  same engine, so SV can instantiate VHDL entities and VHDL can instantiate
  SV modules. Covers entities, packages, processes, generate, components,
  numeric_std, std_logic_1164, textio, math_real, std.env and common
  VHDL-2008 additions; results are checked against GHDL goldens
  (`tests/vhdl`). `msim -xrun` passes VHDL files on; `vhdl2sv` and
  `msim -vhdl-sv` print the translation.
- `mlint -cdc-html`: the selected crossing is drawn as a schematic: source
  flop, the nets and logic between it and the reader (a glitch source for
  CDC003), the synchronizer stages, and the qualifier of an enable-qualified
  bus.
- `mlint -cdc-sva`: handshake checks are now an alternation check that
  holds on both sides (the side that starts a transfer and the side that
  answers), and an acknowledge read back through logic (an FSM) counts.
  Checked by simulation on pulp common_cells `cdc_2phase`, `cdc_4phase`,
  `cdc_fifo_2phase` and `cdc_fifo_gray`: no assertion fails.
- `mlint`: `NAME018` (Verible dff-name-style: a register written in
  `always_ff` whose name does not match `name.dff`, default
  `(_q|_ff|_reg|_r)[0-9]*$`) and `NAME019` (Verible instance-shadowing: a
  declaration that hides one of the same name in an enclosing scope). Both
  off by default.
- `mlint` reads VHDL (`.vhd`, `.vhdl`), alone or mixed with SystemVerilog,
  through msim's VHDL front end: unused/undriven signals, CDC/RDC, latches,
  sensitivity lists, multiple drivers, reset style, FSMs and formal checks
  run on it, with findings at the VHDL line. Rules that would only describe
  the translation are left out (docs/LINT.md, "VHDL"). The translation now
  puts a `` `line `` before each port and signal declaration. The lint rules
  read `always begin ... @(list); end` (how a VHDL process arrives) as a
  combinational block, and a reset tested with `===`.
- `mlint -goal NAME`: methodology goals like SpyGlass's (`lint_rtl`,
  `lint_synth`, `lint_style`, `starc`, `openmore`, `verible`, `cdc`,
  `formal`, `signoff`; `-list-goals`), and twelve more rules: `CASE005`
  (non-constant case item), `CASE006` (`full_case`/`parallel_case`
  pragmas), `CASE007` (case on a constant), `SYN018` (while loops),
  `SYN019` (tasks in design code), `SYN020` (integer signals), `W010`
  (shift out of every bit), `EXP003` (reduction of one bit), `EXP005`
  (unsigned comparison with one answer), `EXP006` (if on a literal),
  `CLK004` (divided/ripple clocks), and `L005` (a VHDL std_logic driven by
  several processes that do not all release it with 'Z'). docs/LINT.md maps
  the STARC guide's topics to rules. `-list` no longer shows `ABS001`..`ABS004` twice.
- `mlint -html`: a schematic for each finding about a signal: its drivers
  (flops with their clock, logic, assigns, and what they read), the signal,
  and its readers (and what they drive); names link to their own schematic.
  More findings now carry their signal (`UNU001`..`UNU005`, `L004`, `L005`,
  `LAT001`, `SEQ001`, `EXP001`, `CLK001`, `CLK004`), which `-json` and
  `mdebug -lint` use too.

### Changed
- `msim -compile` builds the kernel once into a cached object
  (`$MSIM_KCACHE`, default `~/.cache/msim`) that every `simv` links, instead
  of compiling it into each design: a small design's C++ build goes from
  21 s to 8 s and OpenTitan Ibex from 68 s to 50 s. `MSIM_NOKOBJ=1` turns it
  off.

### Fixed
- msim VHDL: a process's first run sees its input ports' initial values,
  not `Z` (14.7.5.2).
- A continuous assignment to part of a variable (a bit, a slice, a member of
  a packed struct, also through an output port) no longer makes the rest of
  the variable z: procedures keep writing the other bits (6.5).
  `tests/spec/6.5-var-partial-cont-assign.sv`.
- msim VHDL, found running the NEORV32 testbench against GHDL: a
  delayed signal update due at time T is applied before a `wait for` that
  ends at T resumes (1076-2019 14.7.5); positional elements before `others`
  in an array aggregate were dropped (a ROM image read as zeros); a local
  `string(1 to n)` variable starts with n characters; a process with a
  branch slang proved dead (a null slice for one generic value) lost its
  whole body and looped at time 0; `T'(x)` of an unpacked struct with a
  string member failed to lower. `tests/vhdl/sim_order.vhd` (GHDL golden).
- mvplat: a CSRRS/CSRRC on `mip` no longer latches the PLIC's SEIP into the
  software-written bit (privileged ISA 3.1.9). OpenSBI's timer code does such
  a read-modify-write, after which Linux took the same external interrupt
  forever.
- `mlint -cdc-sva`: no held check on a FIFO memory (storage wider than what
  the reader takes), which failed on correct FIFOs such as common_cells
  `cdc_fifo_gray`.
- `mlint`: four more rules after Verible's, off by default: `STY013`
  (explicit-begin: `if`/`else`/loop/`always` bodies without `begin`),
  `STY014` (proper-parameter-declaration: `parameter` outside `#(...)`),
  `STY015` (numeric-format-string-style: `"%h"` without `0x`), `SUS004`
  (plusarg-assignment: `$test$plusargs`).
- UPF language rules of IEEE 1801-2024 clause 5 and 8.3 in `msim -upf` and
  `mlint -upf`: identifier and name-conflict checks, references to objects
  that do not exist, `-update` refinement of strategies and switches,
  conflicting port attributes, `{.}` with `-models`, `-instance` cells that
  are not in the design, and supply ports named like HDL logic ports are
  errors. Expressions gain `<`, `<=`, `>`, `>=`, `.voltage` and `.state`;
  Tcl `catch` sets `errorInfo` and `errorCode`. Strategies are chosen by the
  precedence of 5.7.3 and 5.7.4 (port, instance, domain; then options),
  not by the order they were written; a tie is an error.
- `msim -compile -upf`: power-aware simulation in compiled mode. msim binds
  the power intent, then writes a snapshot that `simv` reads with the UPF
  file; processes of a power domain sleep while it is off and corrupt their
  writes in the generated code. Without `-upf` the generated C++ is
  unchanged. `tests/msim_compile.cmake` runs four UPF designs and the
  low-power coverage report both ways.
- `mlint -upf` rules `UPF008` to `UPF012`: isolation and retention controls
  driven from a domain that can be off, isolation or retention powered by
  the switched supply of the domain itself, a power switch controlled from
  the domain it powers, and redundant level shifters.
- `mlint -cdc-sva`: handshakes. A request whose flop reads a synchronized
  acknowledge back gets an assertion that it changes only once the
  acknowledge has caught up (two- and four-phase). The one-bit stability
  check no longer counts the reset value as a change.
- `msim -upf` plans the logic power cannot gate (testbench, always-on
  logic) as without `-upf`: static schedules, partitions and `-threads`.
  Processes in power domains, UPF cells and writers of UPF controls stay on
  the interpreter. A testbench-heavy power-aware run is about 1.7 times
  faster; `MSIM_UPF_SERIAL=1` restores the old serial mode.
- The Tcl interpreter of the tool line (`src/tcl.hpp`, used by UPF, SDC and
  the tool shells) has `upvar`, `uplevel`, `namespace eval` with `variable`
  and namespace procs, `namespace current / exists / qualifiers / tail /
  which / delete`, `regexp` and `regsub` (ECMAScript syntax), and
  `info level / procs`. `tests/test_tcl.cpp` now runs in ctest.
- `msim -upf_cov <file>`: a low-power coverage report of power states,
  named state transitions, isolation enables and retention saves and
  restores, with the bins not hit marked.
- UPF `set_retention -applies_to flop | latch` (IEEE 1801-2024 6.54) selects
  flip-flops or latches: `always_latch` and latches inferred from a
  level-sensitive `always` that leaves a variable unassigned on some path.
  Level-sensitive logic of a domain now re-evaluates at power-up (9.5.3)
  even when no input changed while the domain was off.
- `mvplat`, a virtual platform engine in the manner of Virtualizer, Helium
  and Vista (`docs/VPLAT.md`). It runs RV32IMAC harts in machine and user mode
  with PMP, and all 77 rv32 riscv-tests pass (`tests/mvplat_isa.sh`). The
  board is the built-in `virt32` or a JSON platform file with RAM/ROM, a 16550
  UART, CLINT, PLIC and the test finisher; ELF and HTIF are supported.
  Several harts run on several threads, and the result does not depend on the
  thread count. A spike-style commit log, a decoded-instruction cache (about
  160 MIPS on one hart) and RAM mapped on first touch are included. Its
  headers (`vp/*.hpp`) are installed with the core package, and `vp/ext.hpp`
  is the interface of the licensed extension library: gdb, checkpoints, the
  profile, model libraries and RTL co-simulation.
- `mvplat` runs RV64: RV64IMAC harts (chosen by the program's ELF class),
  the W instructions, LD/SD/LWU, 64-bit atomics, RV64C and Sv39 paging. The
  riscv-tests run grows to 342 programs, RV32 and RV64, all passing. The
  extension ABI is 3.
- `mvplat` supervisor mode and Sv32 paging: delegation (`medeleg`,
  `mideleg`), the supervisor CSRs, SRET, SFENCE.VMA, SUM/MXR/TVM/TW/TSR, a
  TLB per hart, Svade accessed/dirty faults, and a supervisor-mode PLIC
  context per hart. The riscv-tests run grows to 144 programs (rv32si, and
  the `v` environment with paging on), all passing.
- The `minisimCore` package installs `compat.hpp`, so a tool built against the
  installed kernel can register its vendor command-line rules too.
- UPF `use_interface_cell -force_function` (IEEE 1801-2024 6.62) is
  simulated: each isolated port runs through the HDL model of the first
  library cell, wired by `-port_map` to the isolation signal (or its
  inverse) and supplies, in place of the strategy's clamp and sense.
- UPF `map_retention_cell -lib_model_name` (IEEE 1801-2024 6.39) is
  simulated: the mapped register runs as the verification model, wired to
  the clock, data and async set/reset inferred from its always block, the
  strategy's save and restore signals and its supplies.
- `mlint` rule `CDC009` (with `-sdc`): a one-bit signal that can pulse for
  one cycle, crossing from a clock of shorter period into one of longer
  period through a plain synchronizer, can be missed. Periods come from
  `create_clock -period`, and for a generated clock from its master with
  `-divide_by` / `-multiply_by`. A toggle or a held request is not reported.
- Datapath modules `MW_thermdec`, `MW_stack`, `MW_prod_sum`, `MW_div_pipe`,
  `MW_sqrt_pipe`, `MW_log2` and `MW_exp2` (fixed point, under one unit in
  the last place), checked by `tests/dw/more.sv`.
- VPI constraint details (37.31): `vpiSoft` on expression items, `dist`
  items as `vpiDistribution` / `vpiDistItem` / `vpiValueRange` objects with
  `vpiDistType` and `vpiWeight`, and the constraint blocks of a class
  object, whose `vpiIsConstraintEnabled` follows `constraint_mode`. A
  `vpiRefObj` has `vpiName` and `vpiFullName`.
- VPI threads and frames (38.36.1): `vpi_handle(vpiThread / vpiFrame, NULL)`
  in a calltf, thread objects (`vpiParent`, `vpiFrame`, child threads) and
  frame objects (`vpiScope`, `vpiParent`, `vpiThread`, `vpiAutomatic`), both
  with `vpiValid`, and the callbacks `cbStartOfThread`, `cbEndOfThread`,
  `cbEnterThread`, `cbStartOfFrame` and `cbEndOfFrame`. The value of a
  system task argument that is an automatic variable no longer crashes.
- VPI `cbUnresolvedSystf` (38.36.3): runs before elaboration for each
  unknown system task or function name, which the callback may register.
- VPI drivers and loads as 37.16 and 37.46 read: of a bit or part select
  (only what touches those bits), primitive terminals, and the ports of
  the net's own module as local drivers (inputs) and loads (outputs).
- VPI: `force` and procedural `assign` statements in effect are drivers
  (a net: forces only) and loads of what they read (37.16, 37.21).
- VPI `cbEndOfObject` (38.36.1) on a thread or frame runs when it ends;
  `cbReclaimObj` and `cbEndOfObject` on class objects register (msim never
  reclaims class objects).
- VPI assertion control as 39.5 reads: `vpiAssertionEnableStep` /
  `DisableStep` and `vpiAssertionKill` act on one attempt, named by its
  start time (`vpiAssertionClockSteps` accepted); kill no longer disables
  the assertion. System control takes a scope handle (NULL: all);
  `vpiAssertionSysReset` removes the step callbacks, `vpiAssertionSysEnd`
  every assertion callback.
- VPI save, restart and reset (38.6, 38.36.3): `vpi_control(vpiReset)`;
  `cbStartOfSave` / `cbEndOfSave`, `cbStartOfRestart` / `cbEndOfRestart`
  and `cbStartOfReset` / `cbEndOfReset` now run around `$save`,
  `$restart` and `$reset`; `cbSignal` runs at the next time slot after the
  signal. `cbUnresolvedSystf` now also passes the name in `user_data`.
- VPI coverage stop and start on one instance below the top (40.5.1):
  its statement, toggle or FSM counts freeze until `vpiCoverageStart`.
- VPI coverage control as 40.5 reads: `vpi_control` returns the
  `SV_COV_*` codes of the system functions (`vpiCoverageCheck` is 1 when
  coverage is available, not when all is covered; errors are -1), and
  `vpiCovered` of an assertion needs an attempt, a success and no failure.
- VPI `failExpr` on `cbAssertionFailure` (39.4.2): the condition of an
  immediate assertion, or the boolean of a single-expression property.
- UPF information model and Tcl queries (IEEE 1801-2024 clause 10, 11.1):
  `upf_query_object_properties`, `upf_query_object_type`,
  `upf_object_in_class`, `upf_query_object_pathname` and `query_upf`, with
  phase 5 enforced after the first query (docs/LOWPOWER.md).
- The UPF information model also holds abstract power sources, composite
  domains, power state groups, state transitions, repeater strategies, power
  models and attributes (IEEE 1801-2024 10.6.1, 10.6.3).
- `find_objects` follows 1801-2024 6.33 in full: instances, ports and nets
  by default, `-traverse_macros`, `-expand_to_bits`, transitive searches that
  stop at leaf cells and macros, `-leaf_only` / `-non_leaf` for every object
  type, and the glob forms of 6.33.2 (`[a-z]`, `\x`, `{a,b,c}`).

### Added
- Floating point datapath modules, IEEE 754 in any exponent and fraction
  width with subnormals and all five rounding modes: `MW_fp_add`,
  `MW_fp_sub`, `MW_fp_addsub`, `MW_fp_mult`, `MW_fp_div`, `MW_fp_sqrt`,
  `MW_fp_cmp` (with minimumNumber / maximumNumber), `MW_fp_i2flt`,
  `MW_fp_flt2i`, `MW_fp_mac` (fused multiply-add), `MW_fp_recip`,
  `MW_fp_flt2flt` (format conversion) and the shared `MW_fp_round`. `tests/dw_fp_test.py` checks
  them against an exact rational reference (docs/DATAPATH.md).
- `mcov` reads and writes UCIS XML (Accellera UCIS 1.0, its XML interchange
  format): a database named `.xml` is UCIS. Each instance becomes an
  instanceCoverages scope with its statements, branches, conditions,
  toggles, FSMs, assertions and covergroups. It also reads what other tools
  write, such as blocks, case statements with more than two arms and
  SystemVerilog literal state values.
- `mcov` takes other vendors' coverage command lines:
  - URG options with `--compat=synopsys`
  - IMC batch commands with `--compat=cadence`: `-exec`, `-execcmd`, `load`, `merge -out`, `report`, `rank`
  - `vcover merge`, `report` and `ranktest` with `--compat=siemens`
  - the same through a symlink named `urg`, `imc` or `vcover`

  The engine is mcov's own, and it reads mcov's own databases.
  A database that does not exist is now a one-line error instead of a traceback.

### Fixed
- The `SV_TIED_HI`, `SV_TIED_LO`, `VHDL_TIED_HI` and `VHDL_TIED_LO` value
  conversion tables are upf2hdl tables, as in IEEE 1801-2024 Annex B.

### Removed
- `mdft` is now the checks alone. Scan insertion, memory BIST, boundary scan,
  IJTAG, EDT scan compression, logic BIST and failure diagnosis are `mdfti`,
  a paid tool. What stays is what `mdft` always was first: the RTL
  scan-readiness rules DFT001..DFT006 and the flop count per clock domain,
  before a design is changed for test. `src/dft/` and the fault engine
  headers those flows read go with them.
- The implementation and signoff tools leave this repository. `msynth`,
  `mpnr`, `msta`, `mrcx`, `mequiv`, `mformal`, `matpg`, `memu`, `mfpga`,
  `mhls`, `mspice`, `mchar`, `mpv`, `mpower`, `mpopt`, `msdc`, `mlibc`,
  `msimprotect`, `mpss` and the `mvp` virtual prototype layer are a paid
  product now, described in `docs/LICENSING.md`. The free edition is the
  verification flow: `msim`, `mwave`, `mcov`, `mregress`, `mplan`, `mbug`,
  `mlint` with CDC, `mdft`'s scan-readiness checks and `mdebug`.
  - `msim -mvp` keeps its option and reports that it cannot find the runtime
    when the paid layer is not installed beside it.
  - `msim -keyfile` still opens IEEE 1735 envelopes; only `msimprotect`,
    which writes them, has gone.

### Changed
- A for loop with a constant trip count in an always_comb or always @* block
  is unrolled, as loops in clocked blocks already were, so the block runs as
  straight code and can be split per output. On OpenTitan Ibex this saves 8%
  of the instructions executed (`tests/spec/12.7.1-comb-loop-unroll.sv`).
  The unroller also substitutes the loop variable into an indexed
  part-select's base (`v[i*8 +: 8] = ...`).
- The interpreter finds the matching item of a case statement with constant
  labels through a table instead of comparing label by label, and a `unique
  case` whose labels are enum names is no longer treated as possibly
  overlapping. OpenTitan Ibex runs in 15.7 s instead of 20.8 s on one thread
  (`tests/spec/12.5-case-many-items.sv`).
- VPI: a variable of a SystemVerilog type no longer reports `vpiReg` (an
  `int` is a `vpiIntVar`, a `bit` vector a `vpiBitVar`), a
  one-dimensional array reports `vpiRegArray` (`vpiIsMemory` is 1 and
  `vpi_iterate(vpiMemory, ...)` still finds it), `vpiInternalScope` gives
  the generate scopes, tasks, functions and named blocks instead of the
  module instances, and objects in a generate block belong to its
  `vpiGenScope`. Define `VPI_COMPATIBILITY_VERSION_1364v2001` for the old
  view.
- `vpi_user.h`: `vpiFuncType` is 44 and `vpiNetDeclAssign` is 43, as annex K
  says (they were swapped). A VPI library built against the old header
  must be rebuilt.
- `mchar` measures input pin power: `internal_power` groups without a
  `related_pin` get the supply energy of an edge that moves no output, and
  the output arcs on the same pin hold the energy beyond it. The mini45
  flops, latches and clock gates gain these groups on every input, so a
  clock that toggles a register whose output holds now costs power in
  `mpower`.
- License: the repository is Apache-2.0 (`LICENSE`, attribution in `NOTICE`)
  instead of MIT. The MIT grant is not withdrawn: copies obtained under it
  stay under it, and so does the git history. `docs/LICENSING.md` holds the
  open-core policy, the proposed free / paid split per tool, and the rule
  that no GPL or other copyleft code enters the tree.
- Protected IP: the `-license <feature>` option is removed. It let the IP
  user grant a licensed right to themselves; only the license proxy named by
  the envelope decides now. `-license_fail stop|deny` chooses what happens
  when no proxy can be reached.

### Added
- VIP library: `vip/` holds UVM verification IP that runs on `msim -uvm`:
  AXI4, APB4 and a memory model (from SoC-tb-regress-builder) and a new
  AXI4-Lite VIP, each with agents, monitor, coverage, protocol assertions
  and sequences, and self-test benches in `tests/vip` (`ctest -L vip`).
  `tools/mvip.py` runs the 146 VIPs of open-vip-portfolio on msim. See
  `docs/VIP.md`.
- A `tri0` / `tri1` / `wand` / `wor` net connected to an inout port
  (23.3.3.7) read `x` as soon as either side drove it; the port's net is now
  the same net, of the external kind, so the pull and the drivers resolve.
- The implicit sensitivity of a continuous assignment, `always_comb` or `@*`
  no longer holds the arguments, result and locals of the functions it calls
  (9.2.2.2.1): two assignments calling one function that writes its result
  bit by bit woke each other for ever, and the simulation hung at time 0.
- `@(event iff expr)` in procedures (9.4.2.3): the condition was ignored, so
  `@(posedge clk iff rst_n)` woke on the first edge.
- A user function called in the boolean of a concurrent assertion (16.6) is
  run at each clock tick on the sampled values of its arguments; its result
  was garbage before.
- An array declared with an initializer in a non-recursive function,
  `bit [5:0] t[4] = '{...};` (6.21), was an unsupported reference.
- A queue or dynamic array reached through handles in a coverpoint or its
  `iff` guard, `coverpoint tr.txn.data[0] iff (tr.txn.data.size() > 0)`
  (19.5): it was a null object access.
- Constraints on part-selects (18.5): `addr[1:0] == 0; addr[31:12] == 0;`
  with a range on `addr` failed nearly every `randomize()`; the solver now
  narrows a variable's range through a part-select.
- A parameter or localparam of a class read through a handle, `h.N` (8.25).
- VPI: `vpi_get_vlog_info` gives a `-f` file's arguments as the nested
  array 38.36 describes, so `msim -uvm -f files.f` no longer crashes in
  UVM's command line reader.
- UPF `find_objects` (IEEE 1801-2024 6.33): a UPF file can search the
  design for instances, instances of a module, ports by direction, nets and
  its own supply ports and nets (`-transitive`, `-regexp`, `-exact`,
  `-ignore_case`, `-leaf_only`, `-non_leaf`). msim and mlint share the
  design walk and the power top lookup (`src/upf_design.hpp`).
- `msim -cm <metrics>` now selects the code coverage metrics written. It takes
  three notations: VCS `line+branch+cond+tgl+fsm`, Xcelium `b:e:t:f` or
  `all`, and Questa `bcesftx`. `msim -cm_hier <file>` selects the part of
  the design by instance, module or file. It reads VCS `+tree`/`-tree`
  lines and Xcelium covfile `select_coverage` / `deselect_coverage`
  commands. Before this, `-cm` wrote every metric. Covergroups and
  assertions are always written.
- `mlint` clock checks: `CDC007` (a clock read as data) and `CDC008` (a
  clock made by combinational logic of a clock and other signals, which can
  glitch).
- `mlint -sdc`: `set_input_delay` / `set_output_delay -clock` give ports a
  clock domain (virtual clocks included), so an input read without a
  synchronizer, or an output launched on another clock, is a crossing.
  `set_case_analysis 0/1` makes a signal static.
- UPF table VCMs convert a field of a struct port (`create_vcm -field`,
  IEEE 1801-2024 6.30): packed and unpacked structs, nested fields, and one
  upf2hdl VCM per field of a struct input.

- `msim -upf` and `mlint -upf` check the implementation commands of IEEE
  1801-2024 instead of ignoring them: `map_power_switch`, `map_repeater_cell`,
  `map_retention_cell`, `map_retention_clamp_cell`, `name_format`,
  `set_correlated`, `set_variation`, `use_interface_cell` (6.37 to 6.41, 6.43,
  6.57, 6.62) and the `define_*_cell` library commands (clause 7). Each error
  their clauses list is reported with file, line and clause; the simulation is
  unchanged.
- UPF (IEEE 1801-2024): `set_repeater` and the simulation of repeaters
  (6.52, 9.10); `create_logic_net`, `create_logic_port` and
  `connect_logic_net` (6.13, 6.19, 6.20); `add_state_transition` with
  transition detection (6.7, 9.4); `create_power_state_group` (6.22);
  `-applies_to_boundary` on isolation and level-shifter strategies.
- UPF (IEEE 1801-2024): `create_composite_domain` (6.17), `set_equivalent`
  (6.47), `connect_supply_set` (6.15), `set_simstate_behavior` (6.56),
  `set_design_attributes` (6.44), `set_port_attributes` (6.51),
  `set_retention_elements` (6.54), `set_partial_on_translation` (6.50) and
  `add_supply_state` with `{net_state voltage}` (6.8) are applied instead of
  read and ignored; `mlint -upf` rule `UPF007`. See `docs/LOWPOWER.md`.
- UPF (IEEE 1801-2024): `sim_corruption_control` (6.59),
  `sim_assertion_control` (6.58) and `load_simstate_behavior` (6.34).
- UPF (IEEE 1801-2024): power models: `define_power_model` (6.31),
  `begin_power_model` / `end_power_model` (6.11, 6.32), `apply_power_model`
  (6.9) with `-supply_map`, `-port_map` and `-parameters`, and
  `add_parameter` (6.3).
- UPF (IEEE 1801-2024): `bind_checker` (6.12) instantiates checker modules
  in the design through SV `bind` directives; `sim_replay_control` (6.60)
  runs initial blocks again when their domain powers up; `save_upf` (6.42)
  writes the power intent of a scope.
- UPF (IEEE 1801-2024): `(* UPF_... *)` attributes in the HDL (5.6) are read
  by `msim -upf` and `mlint -upf`, below UPF commands in precedence (5.7);
  the Table 8 model-only and instance-only rules are errors.
- UPF (IEEE 1801-2024): `create_abstract_power_source` (6.16) switches a
  supply set between input supply sets, with ack ports.
- UPF (IEEE 1801-2024): value conversion methods (`create_vcm`, 6.30) with
  `connect_supply_net -vcm`: tables for one-bit, real, integer and enum HDL
  ports, VCM lists, and ports of type `upfSupplyTypeT` without conversion
  (4.10); UPF libraries (`create_upf_library`, `use_upf_library`,
  `load_upf_library`; 5.11, 6.29, 6.36, 6.63).
- UPF (IEEE 1801-2024): function and model VCMs (6.30.8, 6.30.9) are
  simulated: the function is called, or the model instantiated, on the port
  through a module bound into the simulation top.
- UPF (IEEE 1801-2024): supply ports connected by pg_type:
  `connect_supply_net -pg_type` with `-cells`, `-domain`, `-elements` (6.14),
  `connect_supply_set -connect` (6.15), and the automatic connection of a
  domain's primary supply set (4.5.4.6.2).
- Forward SAIF (IEEE 1801-2024 Annex J): `msim -saif_forward <file>`. A
  library forward file (J.4) adds state-dependent, path-dependent and SDPD
  toggle counts and state-dependent leakage times for each instance of its
  cells (J.3.6 to J.3.10); an RTL forward file (J.5) writes only its mapped
  ports and nets, typed ports as virtual instances (J.3.11). The SAIF reader
  takes `(INSTANCE "design" path ..)` and `VIRTUAL_INSTANCE`.
- `mlint -cdc-html FILE`: a self-contained page to browse the clock domains,
  the crossings (filter, the path of each through its synchronizer) and the
  reset synchronizers.
- `mlint -cdc-abstract FILE` and `-cdc-model FILE`: a block's CDC model (the
  clock of each port, and which inputs it synchronizes) is written from its
  RTL and used in the SoC run in place of the RTL, so a block can be a black
  box or skipped for speed with the same findings at its ports.
- `mlint -cdc-sva FILE`: writes a module of assertions for the synchronized
  crossings (a one-bit signal holds for two reading edges, a Gray or
  multi-bit bus changes one bit at a time, enable-qualified data is stable
  when its control arrives), to simulate with the design.
- `mlint` CDC/RDC: a vector written bit by bit, or a struct written field by
  field, is tracked as separate signals (`top.r[1]`, `top.ctl.req`), so two
  synchronizers kept in one vector, or a struct with fields in different
  domains, are no longer mixed up.
- `mlint` reset tree checks: `RDC005` (a reset synchronizer of one flop) and
  `RDC006` (an asynchronous reset made by combinational logic of several
  flops). `-cdc-report` lists the reset synchronizers, their length and how
  many flops each resets.
- `mlint` `CDC004` follows synchronized signals through flops: signals
  synchronized separately that meet again up to `-cdc-depth N` flops later
  (default 2) are reported, once per set of signals.
- `mlint -sdc file.sdc`: the CDC and RDC checks take the clocks and how they
  are related from the timing constraints (`src/cdc_sdc.hpp`, the SDC
  commands that set a domain, on the shared Tcl interpreter): generated
  clocks are synchronous to their master, `set_clock_groups -asynchronous`
  and a false path between two clocks make a pair a crossing, exclusive
  groups are not checked, any other pair is synchronous. New rule `CDC006`:
  a flop's clock is not a clock in the SDC file. `-cdc-report` lists the SDC
  clocks of every domain (`docs/CDC.md`).
- `msim -vcd-class [N]`: class objects in the VCD or FST. Each property of
  the object a module's class handle variable points to is a signal
  `tb.p.addr` that follows the handle (x while it is null), with handle
  properties followed N levels. `mdebug` reads them back: script `value` and
  `changes`, `-signals`, and the page adds them when the handle is picked.
- `mdebug` reads a waveform on demand: the signal list first, then each
  signal's changes when a query or the page first needs it (the `-html`
  page in blocks of 2048 signals). On a 1024-core generated SoC (23305
  signals, 384 MB FST) a script query takes 1 s and 113 MB, `-serve` 157 MB,
  `-html` 1.6 GB, where every one of them ran out of memory before.
  `wave::Reader::lazy`, `fetch()` and `release()` give other tools the same.
- `mdebug -tr FILE -trans [STREAM]`: UVM transactions from the recording
  uvm-core writes (`tr_db.log`): streams, each item's begin, end, fields and
  parent sequence, those in progress at `-at T`, and stream rows in the
  viewer's waves.
- `msim -i` and `msim -do FILE`: interactive simulation like VCS UCLI:
  `run [T]`, `step`, stops on a signal change, edge or value, `print` of
  signals and class objects (properties, inherited ones, paths through
  handles), `force`, `release`, `deposit` and `signals`.
- `mdebug` waves: a marker with the time to the cursor (shift+click or `m`),
  a radix per row (`r`: hex, decimal, signed, binary), find value
  (next/previous time the selected signal takes it), Del to remove a row,
  Ctrl+arrows to step changes; the view lives in the page address, so a
  reload or a copied link opens on it.
- `msim -i`: `drivers` / `loads` of a signal, `call` of a task or function
  (static or automatic, with its return value and outputs), `save` /
  `restore` checkpoints by replay of the run and its commands (also read by
  `msim -restore`), and `restart` from time 0.
- `msim -i` / `-do` is a UCLI on the shared Tcl interpreter: `set`, `if`,
  `foreach`, `proc`, `source` around the commands; stops at a line
  (`stop -line N -file F`), at a time, on a condition, with `-command`,
  `-continue` and `-once`, and `-delete/-disable/-enable`; `step` and `next`
  one statement at a time; `run -posedge/-negedge/-change SIG`, `run -slot N`
  (was `step N`), `run T -absolute`; `get -radix`, `change`, `scope`, `show`,
  `describe`, `search`, `where`, `listing`, `dump` / `probe`, `senv`, `alias`.
  `xrun -input` runs its Tcl file as it is (no translation), and `xrun -tcl`
  is `-i`.
- `mdebug -lint FILE`: mlint's findings (`mlint -json`) in the page: a list
  with a severity filter, marks on the source lines, and a click that opens
  the line and selects the signal the finding names; script command `lint`.
- `mdebug -serve [PORT]`: the viewer from a web server on 127.0.0.1; a
  signal's waves are sent when the page first shows it, so large waveforms
  are not cut to the one-file page's budget.
- `mdebug -upf FILE -power [DOMAIN]`: the power domains, their supplies and
  isolation, level-shifter and retention strategies; with the log of
  `msim -upf_verbose`, power states over time. The active trace says when a
  value is x because its domain is off and names the isolation on a port.
- `mdebug -cov FILE`: with msim's coverage database, driver lines say how
  often their line ran, `-uncovered [INST]` lists assignments that never ran
  and signals that never toggled, and the viewer page marks covered lines.
- `mdebug -assert [NAME]`: the design's assertions; with `-log LOG` and a
  waveform, each failure with the values the assertion sampled at the clock
  ticks before it and the driver trace of each signal.
- `mdebug -script FILE` (`-` for the standard input): one command per line
  (drivers, loads, value, changes, active, trace, blocks, fsm, scopes,
  signals, end), one line of JSON per answer, from one process that loads the
  design and waveform once. `tools/mdebug_api.py` wraps it for Python.
- `mdebug -fsm [NAME]`: the design's state machines (shared extractor), their
  states and transitions with source lines; with a waveform, which transitions
  ran, states never entered, values that are not states, and the state at
  `-at T`. The viewer page draws the state diagram of a selected state register.
- `msupport`: an AI support agent per product. Each answers from its
  product's documentation (BM25 search over the manuals), can run a short
  example on the product's tool in a sandbox, and files a ticket when the
  documentation does not answer; command line, terminal chat and a local web
  service. Product files for the nine free tools in `support/products/`.
  Not online yet. See `docs/SUPPORT.md`.
- Value conversion tables (IEEE 1801-2024 4.10, 6.18, 6.28, Annex B):
  `connect_supply_net` joins a supply net and an HDL port. An output port
  (a regulator model) drives the net; an input port (a macro's power pin) is
  driven by it. The twelve predefined tables and `create_hdl2upf_vct` /
  `create_upf2hdl_vct`, chosen with `-vct` (`src/upf_vct.hpp`).
- UPF is Tcl (IEEE 1801-2024 5.1): `msim -upf` and `mlint -upf` read power
  intent with the tool line's Tcl interpreter (`src/tcl.hpp`), so `[command]`
  substitution, `foreach`, `for`, `while`, `if`, `expr`, `proc`, lists,
  `catch` and `puts` work in a UPF file. An error is reported with the file
  and line of the command, inside procs and loops too.
- `meco -spare` (and `-spare-cells GLOB`): metal-only ECO. The patch is
  built from the spare cells of the old netlist and the cells the change
  frees, by function (the same cell, or one with inputs tied or shared, or
  the inverse plus a spare inverter); no cell is added or removed, and the
  script holds only net connections. Exit status 3 when a cell had to be
  added after all. See `docs/ECO.md`.
- Protected IP and VPI / DPI (IEEE 1800-2023 37.3.6, IEEE 1735-2023 9.7):
  `vpiIsProtected`, and for a protected object only `vpiType` (and `vpiSize`
  of an expression) can be read. Handles, iteration, names, values,
  callbacks and `vpi_handle_by_name` are errors; the IP's own system task
  calls keep working. `svGetNameFromScope` and `svGetCallerInfo` give
  nothing for protected code. See "VPI and DPI" in `docs/PROTECT.md`.
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
- `mpnr` on the SkyWater 130 nm cells (`lib/sky130`): pins on li1 that sit
  off the met1/met2 crossings are reached by vias placed along the pin, with
  a short wire to the nearest crossing; via pads wider than the wires keep
  their spacing to other nets' pads, to cell shapes on the next track and
  (met4 under a met5 via) to wires on the next track; a minimum area longer
  than a track step is met by lengthening the piece; notches between a
  net's own pads are filled with `RECT` patches; pin vias of one net keep
  their cut spacing. Room is kept for pins with few ways in before routing
  starts, and contested nets are taken out together, so routing settles in
  fewer rounds. 10,000 sky130 cells route with 0 open, 0 shorts, 0 spacing
  violations and 0 violations in `mpv drc -lef`. `bench/gen_pnr.py --sky130`
  writes netlists over the library's cells.
- `mpnr` on `lib/mini45`: a pin too small for the LEF's via pad gets a via
  made for it (lower pad turned or lengthened, written to the DEF's `VIAS`),
  short pieces of wire and the pads of power via stacks are lengthened to
  the layer's `AREA`, and a layer's `OFFSET 0` is taken as given (it used to
  mean half a pitch). `bench/gen_pnr.py --mini45` writes netlists over the
  library's cells; the `mpnr` test places, routes and DRC-checks one.
- `mplan`: `cond` and `fsm` plan metrics (condition vectors of files, states
  and transitions of FSMs), in the totals, the history and `mplan init`.
- `mregress -farm`: runs go to a compute farm instead of the local machine:
  `lsf` (bsub -K), `slurm` (srun), `sge` (qsub -sync y), `ssh:HOST[*N],...`
  or `cmd:TEMPLATE` for any other launcher. `-farm-opts`, `-farm-cpus`
  (cores per job and msim -threads), `-farm-retries` (a job that never ran
  is submitted again), `-farm-wait`, `-farm-dry`. The time limit runs on the
  farm host, so queue time does not count; results come back through the
  shared run directory.
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
- `mformal`: SVA local variables (assigned once, in an antecedent or a
  cover's sequence) and `s.triggered` are checked instead of skipped.
- `mformal -blackbox NAME` and `-cut SIGNAL`: leave a module or instance
  out of the model, or make a signal a free input.
- `mformal -connect FILE`: connectivity checking. Each line
  `destination, source [, condition]` is proven, and a condition that can
  never hold is flagged.
- `mformal -app toggle`: unreachable-coverage analysis. Reports every
  signal bit that can never be 0 or never be 1 after the reset.
- `memu` is timing-aware between FPGAs: on a link with too few wires the
  signals on the longest combinational paths keep a wire of their own and the
  rest share what is left (40% fewer `emu_clk` cycles per event on a
  910,000-cell netlist), and every partitioning attempt is routed and timed
  so the fastest one is kept, not the smallest cut. `-partitioner CMD` lets
  another partitioner compete (`tools/memu_mtkahypar.py` for Mt-KaHyPar). The
  per-FPGA netlists now go through `mfpga` (the test maps a three-FPGA split
  to iCE40 and checks it against the original netlist).
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
- RTL to logic (`src/aig_elab.hpp`, used by `msynth`, `mequiv`, `mformal`,
  `mfpga`, `meco`): a `case` whose items cannot match together (`unique
  case`, `parallel_case`, distinct constant labels) is built without
  priority among the items. PicoRV32 on mini45 with `msynth`: 17534 to
  17377 (high effort 16997), critical path 1.11 to 0.99; the ALU test
  design: 4734 to 4449.
- `msynth`: a register's hold condition is gathered out of its if/case
  tree into one enable, and the held leaves leave the data logic
  (`set msynth_hold_enables false` skips it). PicoRV32 on mini45: 18096 to
  17534, with high effort 17577 to 17166.
- `msynth` high effort (`compile -map_effort high`, `compile_ultra`):
  the logic is restructured by ABC, run as a separate program (`abc`,
  `berkeley-abc` or `yosys-abc` on `PATH`, or `$MSYNTH_ABC`), and the
  smaller of the two mapped results is kept. Without constraints the
  smallest netlist now stands (high effort used to pick the faster one).
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
- `mhls -lib cells.lib`: operator delays, clock-to-Q and setup from a
  Liberty library (reference cells at fanout four, operators as gate
  counts fitted to msynth and msta results), so the schedule follows the
  library and its corner. `tests/hls/calibrate.sh` measures the fit.
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
- `-xprop`: `disable` of a named block from inside it is merged: it sets
  a flag (and the break flags of the loops between) instead of jumping, and
  the rest of the block runs under the flag, so an unknown condition around
  it merges the rest with not running it.
- `-xprop`: blocking writes to an element of an array of 64 elements or
  fewer chosen at run time merge exactly: the branch saves, puts back and
  merges the array's elements like variables, so two branches that write
  the same element no longer merge with its old value.
- `-xprop`: `case ... inside` is merged: an item may match when its `==?`
  or range compare is unknown. The merging code of a combinational block
  now writes without change events and publishes each final value once, so
  blocks that read each other no longer wake each other forever (the
  CV32E40P decoder and controller hung at time 0). CV32E40P runs with all
  753 `if` / `case` / edge constructs merged and the same result.
- `-xprop`: a write to an element of a multidimensional array whose indexes
  have unknown bits (`m[r][c] <= d`) merges into every element the indexes
  may name. Before, only one-dimensional arrays did, and the write was
  skipped.
- `-xprop`: a branch that calls a task or function with a nonblocking
  assignment is merged. The task's `<=` target becomes `x` when the branch
  only might be taken (each process calls its own copy of the task that
  tests its unknown flag); before, the whole block kept the standard
  behavior.
- `-xprop`: `break` and `continue` in `while`, `do ... while`, `repeat` and
  `forever` loops are merged; once a break or the condition of an unbounded
  loop is unknown, what the loop assigns becomes x. Fixed: a `foreach` or
  `repeat` loop whose body met an unknown condition in the clean copy
  continued with a stale loop counter.
- `-xprop`: a configuration file (`-xprop=<file>` or `-xprop-config <file>`,
  VCS style: `tree`, `instance`, `module`, `merge`) turns X-propagation on or
  off per instance, subtree or module. Branches that call tasks or functions
  are merged (their writes too); `break` and `continue` in `for` / `foreach`
  loops are merged; an array write whose index has an unknown bit merges
  into every element the index may name.
- `-xprop`: the clean copy uses the standard `case`, and clocked blocks skip
  the ambiguous-edge test when nothing went from or to x. Ibex with
  `-xprop` now costs about 12% more than without (was 30%).
- `-xprop`: a block of plain assignments and branches gets a clean copy
  that runs while every condition is known and hands over to the merging
  copy at the first unknown one. Ibex with `-xprop`: 36 s to 28 s in the
  interpreter (24.5 s without).
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
- VPI headers complete (annexes K, L, M): `vpi_user.h` has every constant of
  annex K, `sv_vpi_user.h` every object type, relation, property, operator,
  callback reason and the assertion and coverage declarations of annex M,
  and the new `vpi_compatibility.h` (annex L, 36.12) binds an application
  built with `VPI_COMPATIBILITY_VERSION_<standard>` to that standard's
  variants of the routines; `vpi_get(vpiCompatibilityMode, NULL)` returns
  the caller's mode. New routines `vpi_get_value_array` and
  `vpi_put_value_array` (38.16, 38.35). A whole array can be an argument of
  a user system task. See `docs/VPI.md`.
- VPI object model of SystemVerilog (clause 37): packages, interfaces with
  modports and programs as instances (`vpi_iterate(vpiInstance, NULL)`);
  generate scopes, tasks, functions and named blocks as internal scopes;
  variables and nets report their SystemVerilog type (`vpiBitVar`,
  `vpiIntVar`, `vpiEnumVar`, `vpiStructVar`, `vpiStringVar`, ...) and have a
  typespec with enum constants, struct members and typedef names; arrays of
  any number of dimensions with ranges, `vpi_handle_by_multi_index` and
  names like `top.m[1][2][3]`; `vpiHighConn`; `pkg::name` in
  `vpi_handle_by_name`. An application built for IEEE 1364
  (`vpi_compatibility.h`) keeps that standard's view. See `docs/VPI.md`.
- VPI assertion API (clause 39): assertions are objects found by
  `vpi_iterate(vpiAssertion, ...)`; `vpi_register_assertion_cb` reports each
  attempt's start, success, vacuous success, failure and disabled
  evaluation, and assertion control; the system callbacks
  (`cbAssertionSysInitialized`, `cbAssertionSysOff`, ...) and every
  assertion operation of `vpi_control` work. See `docs/VPI.md`.
- VPI `cbCreateObj` and `cbSizeChange` (38.36.1); the save, restart and
  reset callback reasons register (msim has no save, so they never run).
  `tests/spec/38.36-vpi-sv-callbacks.sv`.
- VPI constraints (37.31): `vpi_iterate(vpiConstraint, class)`, the items of
  each block (expressions, implication, if / if-else, foreach, disable soft,
  solve before), and pure virtual prototypes in `vpiMethods`.
  `tests/spec/37-vpi-constraints.sv`.
- VPI module path properties: `vpiPathType`, `vpiPolarity`,
  `vpiDataPolarity`, `vpiCondition` and `vpiModDataPathIn` (37, 38.10).
- VPI delays of continuous assignments (38.10): `vpi_get_delays` /
  `vpi_put_delays` on a `vpiContAssign` with a delay.
  `tests/spec/38.10-vpi-assign-delay.sv`.
- VPI primitives (37, 38.10): `vpi_iterate(vpiPrimitive / vpiGate /
  vpiSwitch / vpiUdp, module)`, `vpiPrimType`, `vpiPrimTerm` terminals with
  direction and connection, and gate delays through `vpi_get_delays` /
  `vpi_put_delays`. `tests/spec/38.10-vpi-primitives.sv`.
- VPI module paths (38.10): `vpi_iterate(vpiModPath, module)`, their input
  and output terms, and their delays through `vpi_get_delays` /
  `vpi_put_delays`. `tests/spec/38.10-vpi-modpath.sv`.
- VPI timing checks: `vpi_iterate(vpiTchk, module)`, their terms, notifier
  and limits (`vpi_get_delays` / `vpi_put_delays`), and `cbTchkViolation`
  (38.10, 38.36.1). `tests/spec/38.36-vpi-tchk.sv`.
- VPI assertion stepping (39.5.2): `vpiAssertionEnableStep` /
  `vpiAssertionDisableStep` with `cbAssertionStepSuccess` /
  `cbAssertionStepFailure` for each clock step of each attempt.
  `tests/spec/39.5-vpi-assert-step.sv`.
- VPI `vpi_control(vpiCoverageMerge, type, name)` (40.5.1): adds the
  statement, toggle, FSM or assertion counts of a saved msim coverage
  database to the current run. `tests/spec/40-vpi-coverage-merge.sv`.
- VPI `vpi_handle_multi(vpiInterModPath, out, in)` (38.21): the path
  between two instance ports on one net, its ports, and its interconnect
  delay through `vpi_get_delays` / `vpi_put_delays`.
  `tests/spec/38.21-vpi-handle-multi.sv`.
- VPI `cbError` and `cbPLIError` (38.36.3): called for run-time error
  messages of the simulation and for errors of VPI calls, with
  `vpi_chk_error` giving the error. `tests/spec/38.36-vpi-cberror.sv`.
- VPI class definitions (37.29): `vpiClassDefn`, `vpiExtends`,
  `vpiMethods`, `vpiDerivedClasses` and static `vpiVariables`; a class
  object now reports the class it was made of, and its properties include
  that class's own. `tests/spec/37-vpi-classes.sv`.
- VPI drivers and loads (37.16, 37.17): `vpiDriver`, `vpiLoad`,
  `vpiLocalDriver`, `vpiLocalLoad` and `vpiPortInst` on nets and variables
  give continuous assignments and instance ports, following whole-signal
  port connections through the hierarchy. `tests/spec/37-vpi-drivers.sv`.
- VPI structs, containers and classes (37.17, 37.18, 37.29): unpacked
  struct and union variables with `vpiMember` (also of packed ones),
  dynamic arrays, queues and associative arrays with their elements, class
  variables, `vpiClassObj` and its properties.
- VPI `cbStmt` (38.36.1): a callback each time a statement runs.
- VPI processes, statements and expressions (37.32 to 37.51):
  `vpi_iterate(vpiProcess / vpiContAssign, module)`, the statement tree
  (blocks, assignments, if, case and case items, loops, delay and event
  controls, task calls, wait, disable, force / release, ...) and
  expressions (`vpiOperation`, `vpiConstant`, function calls, selects) with
  `vpiOpType`, `vpiConstType`, `vpiDecompile`, `vpiLineNo`.
- VPI scheduling (38.35, 38.36): `vpiReturnEvent` gives a `vpiSchedEvent`
  handle that `vpiCancelEvent` cancels (`vpiScheduled`); inertial,
  transport and pure transport writes remove pending writes as the standard
  says; `cbForce`, `cbRelease`, `cbAssign`, `cbDeassign`, `cbNBASynch` and
  `cbAtEndOfSimTime`; `vpi_get_delays` / `vpi_put_delays` on nets with a
  declared delay (38.10, 38.30).
- VPI coverage API (clause 40): `vpi_get` of `vpiStatementCoverage`,
  `vpiToggleCoverage`, `vpiFsmStateCoverage` and `vpiAssertCoverage` on an
  instance, `vpiCovered` / `vpiCoveredCount` / `vpiCoveredMax` and the
  assertion counts on assertions, FSMs, FSM states, nets and bits,
  `vpi_iterate(vpiFsm)` and `vpiFsmStates`, and `vpi_control` with
  `vpiCoverageStart`, `Stop`, `Reset`, `Check` and `Save` (`msim -cov`).
  `vpi_handle_by_name` finds an assertion by its label.
- VPI: `vpi_handle_by_name` of a part select (`top.u.r[3:0]`, a
  `vpiPartSelect`, 38.27) reads, writes, forces and releases just those
  bits, so UVM's `uvm_hdl_*` backdoor works on part-select paths.
- `mcov exclude -formal`: what `mformal -json` proves unreachable (toggle
  bits today, other targets that name the exclusion they prove) becomes
  exclusions with that reason, for the items the tests did not cover.
- Condition coverage modes (as VCS `-cm_cond`): `full` records every
  combination of the term values (up to 8 terms), `allops` also covers `&`
  and `|` of one-bit operands; interpreter and `-compile`.
- Per-instance coverage: `msim -cov` also writes each module instance's own
  line hits, branch outcomes and condition vectors (interpreter and
  `-compile`); `mcov report -instances` prints a row per instance with line,
  branch, condition, toggle and FSM coverage, `-detail -instances` the
  vectors of each condition per instance, the HTML report an Instances table,
  and `inst <path>` excludes an instance and everything below it.
- `mcov` exclusion files and test ranking: `mcov report -elfile` leaves the
  listed lines, branches, condition vectors, toggle bits, FSM states and
  transitions, covergroups and assertions out of every number, each with its
  reason; `mcov exclude` drafts the file from what is not covered, and
  `-el-review` / `-el-strict` flag exclusions that are stale, that remove
  covered items or that have no reason. `mcov rank` orders one database per
  test by the coverage each adds, shows what only one test covers and which
  tests add nothing, and `-item` lists the tests that cover an item.
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
- `mpss`: address spaces (`addr_reg_pkg`). A model declares a
  `transparent_addr_space_c<>` with regions, and actions and buffers claim
  memory from it with `transparent_addr_claim_s<>`; claims that are alive at
  the same time never overlap, so `examples/pss/dma.pss` now checks the
  copied data. Also `constraint default`, and `x.f()` calls and region
  variables in `exec init`.
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
- UVM (IEEE 1800.2-2020): three tests cover the classes that compiled but
  had no check of their own: `tests/uvm/base_extras.sv` (`uvm_time`,
  `uvm_heartbeat`, `uvm_sequence_library`, report macros,
  `uvm_callback_iter`, `uvm_run_test_callback`, `uvm_root`),
  `reg_extras.sv` (`uvm_reg_file`, `uvm_reg_indirect_data`, `uvm_reg_fifo`,
  `uvm_vreg`, `uvm_vreg_field`, `uvm_mem_mam`, block lookups) and
  `reg_sequences.sv` (the built-in register and memory test sequences of
  Annex E, each also run against a faulty DUT). Two faults they found are
  fixed: `c ? i++ : i` and `c ? (x = 1) : 0` ran the assignment in the arm
  not picked (11.4.11), and a `randc` property named in `randomize(v)` lost
  its cycle (18.11).
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
- `mdft -diagnose -edt_patterns`: diagnosis from compressed-pattern fail logs
  (failing output channel bits), with the candidates' failures folded through
  the compactor and the masks; the pattern testbench writes such a log with
  `+faillog`.
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
- `mspice`: behavioral sources (`B` elements, `E`/`G` with `vol=`, `cur=` or
  `value=`): any expression of node voltages, branch currents and `time`,
  compiled once and differentiated exactly for Newton's method and AC;
  switches (`S`, `W`) in the SPICE3 form with hysteresis and in a smooth
  form (`vswitch`, `iswitch`); `-raw` and `-raw-ascii` write SPICE raw files
  that ngspice and waveform viewers load.
- `mspice`: `.noise` (thermal, shot and flicker noise of every device at an
  output and referred to the input, totals and largest contributors), `.tf`
  (DC gain, input and output resistance), `.sens` (DC sensitivity to every
  resistor and source) and `.four` (harmonics and total harmonic distortion).
  Noise and sensitivities use the adjoint circuit: one transposed solve
  instead of one solve per device. On a 3,600-node mesh with 7,140 noise
  sources, a 401-point noise analysis takes 2.0 s on one thread and 0.8 s on
  four; ngspice 42 takes 6.9 s and gives the same total.
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
- Protected IP: the IEEE 1735-2023 clause 8 license system. `msim`, `mlint`
  and `mdft` ask the IP author's license proxy over TLS for the value of a
  `license(...)` or `license_string(...)` call in a right (`src/licproxy.hpp`).
  See "Licenses" in `docs/PROTECT.md`.
- Protected IP, IEEE 1735-2023 5.2, 9.3, 9.4, 9.9 and Annex A checked rule
  by rule (in `test_protect`): strings without `\` or control
  characters, lower-case keywords (`` `pragma protect BEGIN `` is an error
  instead of leaving the source in the clear), key names and rights that
  differ only in case, end-of-file characters inside an envelope, strict
  BASE64_TEXT, what tool and common blocks may hold, and viewport lists with
  `.` or `:` scoped names. Clear modules instantiated in or bound into
  protected IP are hidden from waveforms, coverage and lint output.
- `mplan`: requirement traceability. `req ID ...` in a feature and a
  requirement list (`reqs FILE` in the plan or `-reqs FILE`), read from CSV
  or ReqIF (`.reqif` / `.reqifz`, with the hierarchy). Every requirement is
  verified, partial, failing, not started or uncovered; the report, the
  HTML page, `plan.json`, the history and `mplan trend` show it, `-trace`
  writes the matrix as CSV, `-fail-on-uncovered` gates on it, and
  `mplan init -reqs` drafts a plan from the list.
- `mplan rank`: per-test coverage and test ranking from the per-run
  coverage databases of `mregress -cov`. A greedy order by new coverage
  points (`-by rate`: per second of run time), each test's unique points,
  the redundant tests, `-keep FILE` for the short list with the same
  coverage, and with a plan the tests behind each feature's coverage.
  `mplan report -rank` adds it to `plan.html` and `plan.json`.
- `mplan serve` and `mplan push`: a multi-user server and web dashboard.
  CI pushes each report; the server keeps the history per project and shows
  every plan (score, status, tests, requirements, trend) and one page per
  plan where people set the owner, status, bug id and note of each failure
  bucket. Tokens with read / write / admin roles, an audit log, a JSON API,
  and `mplan report -rules URL` to take the annotations as triage rules.
- `src/compat.hpp` and `docs/COMPAT.md`: one shared layer that lets every tool
  accept Synopsys, Cadence or Siemens command lines (`--compat=cadence`, or the
  name the tool was started as). It renames or drops options from a per-tool
  rule table and prints messages in the chosen vendor's layout. The engine
  stays the same; nothing is implemented twice. Unit test `test_compat`.
- `src/saif.hpp` reads state- and path-dependent SAIF: the `COND` / `IOPATH`
  / `RISE` / `FALL` toggle counts of a port go to `Db::paths`, the `LEAKAGE`
  state times of an instance to `Db::states`. Unit test `test_saif`.

### Fixed
- A function that writes part-selects of its result or of a local in a
  loop, called from two continuous assignments or always_comb blocks, hung
  msim at time 0: each caller was sensitive to the function's own variables,
  so the callers woke each other forever. Callers are now sensitive only to
  their operands (10.3.2, 9.2.2.2.1). Five open-ip-portfolio IPs that timed
  out after 600 s now pass in 2 to 101 s (AES_GCM 27.5 s; Icarus 30 s).
- `c ? q.pop_front() : d` (and `pop_back`) runs the pop only in the chosen
  arm, after the condition (11.4.11). It used to pop first, so a condition
  on `q.size()` popping the last element gave `d` and lost the element.
  Found by the SPI VIP's slave model.
- `x inside {q}` in a constraint, where `q` is a queue or dynamic array (a
  caller's variable in `randomize() with`, or a class property), no longer
  makes randomize() fail: the solver narrows x to the container's elements
  (11.4.13, 18.5.4). Found by the CCIX VIP's data sequence.
- A port of an unpacked struct type is one vector, as a variable of that
  type is; it was "unsupported port width".
- msim memory on big VIP benches: a 2-state unpacked array with more than
  64 elements gets its 0 defaults (6.8) from a loop instead of one
  instruction per element (a 1 MiB `bit [7:0] mem[]` cost about 1 GB), and
  an assertion action block (16.14.3) is freed when it finishes instead of
  at the end of the run (a pass action on every clock grew without bound).
  A UFS VIP test went from 7.5 GB to 2.3 GB; SD and eMMC tests were killed
  for memory before.
- `randomize()` no longer fails when a variable solved first is tied to one
  whose values have holes (`s == d` with `d inside {512, 1024, 4096}` and
  `solve s before d`, or `data.size() == len` with `len` in a set, since
  array sizes are solved first). The value search now narrows the other
  variables at each step (18.5, 18.4).
- msim: four faults found by re-measuring the spec table with uvm-core.
  `void'($cast(d, s))` no longer reports an error when the cast fails; only
  the task form does (8.16). `'{default: v}` on an associative array sets
  the value a missing element reads as (7.9.11); this lets
  `uvm_sequence_library` compile. A hierarchical name can end in an element
  of an unpacked array in an expression (`top.u.m[2]`, 23.6).
  `pre_randomize` / `post_randomize` declared `extern` are called (18.6.2);
  before, `uvm_reg::randomize()` left the fields' desired values unchanged.
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
- `mlint`: eight more rules after Verible's: `NAME015`..`NAME017` (interface
  and constraint names, parameters named for what they disable), `SUS002`
  (`void'(x.randomize())`), `SUS003` (`$psprintf`, `$random`, `$dist_*`),
  `STY010`..`STY012` (string line continuations, a semicolon after a UVM
  macro, `disable` of something other than an enclosing block).
- `mlint` formal-assisted checks (`FML001`..`FML003`, `src/lintformal.hpp`):
  dead `if`/`case` branches, indexes and part selects that can fall out of
  range, and registers that never leave their reset value, each proved by
  enumerating the values of the signals involved through single-driver nets
  and the reachable values of small registers. Multithreaded; settings
  `formal.max_bits` and `formal.reg_bits`.
- `mlint` SoC flow (`src/lintsoc.hpp`): `-abstract-out` writes a block
  abstract (ports and what the block does with them, parameters, open
  findings); `-abstract` checks the top against it without the block's RTL
  (rules `ABS001`..`ABS004`); `-bbox` makes modules black boxes.
- `mlint -html FILE`: a self-contained report page with counts by severity
  and rule, the source around each finding, and filters.
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
- `mfpga`: block RAM on `xc7` too (`RAMB18E1`, 1024 x 18 to 16384 x 1,
  parity bits used as data).
- `mfpga`: small memories and memories read without a register map to LUT
  RAM on `xc7` (`RAM32X1D`, `RAM64X1D`) and ECP5 (`TRELLIS_DPR16X4`).
  `-nolutram` keeps flops.

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
