# minisim product line

minisim is heading toward a complete IEEE 1800-2023 SystemVerilog
verification flow that is free, scriptable, and runs the same command lines
as VCS and Xcelium. This page lists the products, how they fit together, and
the roadmap to 1.0. Release mechanics are in [`RELEASING.md`](RELEASING.md).

## Products

| Product | What it is | Status |
|---|---|---|
| **msim** | Simulator: slang front end, event-driven 4-state kernel, interpreter by default, `-compile` to native C++, multi-core, VCD. irun / vcs -R style command line | Shipping in 0.1 |
| **msimprotect** | IEEE 1735 IP protection: encrypt source with `msimprotect`, simulate it with `msim -keyfile` | PR #29, planned for 0.2 |
| **nocgen** | UVM testbench and memory-profile generator for AXI NoC meshes ([SoC-tb-regress-builder](https://github.com/tonythetiger168/SoC-tb-regress-builder)) | Separate repo; runs on msim once classes and UVM land |
| **mregress** | Regression runner: test lists, seeds, parallel jobs on msim / VCS / Xcelium / Questa / Icarus, failure grouping, JUnit and HTML reports | Shipping in 0.2 |
| **mbug** | Bug reports: each distinct mregress / mlint / mdft failure becomes one issue in GitHub Issues, Redmine or Jira (deduplicated by signature, a comment per repeat), with repro command, logs and the CI build link; syncs GitHub issues into Jira / Redmine | Shipping in 0.2 |
| **mlint** | Lint, clock-domain (CDC) and reset-domain (RDC) checks on the elaborated design: latches, assignment kind, multiple drivers, missing synchronizers, multi-bit crossings, glitches, reconvergence, reset crossings (`docs/CDC.md`) | Shipping next |
| **mvp** | Virtual prototypes: an AXI4-Lite bridge from the design to C, C++ or SystemC/TLM-2.0 models, with a memory map, interrupt lines and timed callbacks; a memory and a UART bundled | Shipping next |
| **mdft** | Design-for-test: scan-readiness checks (gated clocks, logic-driven resets, latches, mixed edges, combinational loops) and a per-domain flop count for chain planning | Shipping next |
| **mformal** | Formal property checking: proves or falsifies `assert`, constrains with `assume`, reaches `cover`, for every input sequence; bounded model checking and k-induction on CaDiCaL, counterexample waveforms and replay testbenches, one job per property on every core (`docs/FORMAL.md`) | Shipping next |
| **mdft** | Design-for-test: scan-readiness checks (gated clocks, logic-driven resets, latches, mixed edges, combinational loops) and a per-domain flop count for chain planning; scan insertion on gate netlists (`-insert_scan`) and March memory BIST generation with fault coverage (`-mbist`) | Shipping next |
| **mequiv** | Logic equivalence checking: RTL vs netlist, netlist vs netlist, RTL vs RTL; name-based matching of flops and latches, SAT proof of every output and next-state function on all cores, counterexamples (`docs/EQUIV.md`) | Shipping next |
| **mdebug** | Source-level debug: drivers and loads with their conditions, fan-in cones, the active driver at a time in a waveform traced back through time, and a one-file viewer (hierarchy, source, schematic, waves) | Shipping next |
| **mcov** | Coverage merged across a regression: code coverage (line, branch, toggle), covergroups and assertion statistics; text / HTML / LCOV reports and gates | Shipping in 0.2 |
| **mplan** | Verification planning and tracking: a plan maps features to tests, coverage and assertions; scores per feature, history and trend, failure triage (new / recurring / flaky / fixed), HTML report and CI gates | Shipping next |
| **mrcx** | Parasitic extraction: routed DEF + LEF in, SPEF out. Distributed RC per net with coupling capacitance, process stack and temperature, open and short checks, multi-core | Shipping next |
| **mwave** | Waveforms for GTKWave / Surfer: hierarchical VCD and compressed FST (shipping in 0.2); `$dumpfile` / `$dumpvars` scoping and transaction views next | Partly shipping in 0.2 |

All of them install from one package (`minisim_x.y.z_amd64.deb` /
`.tar.gz`) and share one version number.

## Editions

- **minisim (open source, MIT)**: every product above. This is the product.
- An enterprise edition (support contract, long-term-support branches,
  validated platform builds, encrypted-IP key management) is an option once
  1.0 is out; nothing is held back from the open-source build before then.

## Roadmap

| Release | Language and features | Product work |
|---|---|---|
| **0.1** | Everything to spec round 5: arrays, generate, events, fork/join, packed structs, real, string, dynamic arrays / queues / associative arrays; `-compile`, multi-core, 2-state | Versioning, packages, release workflow |
| **0.2** | Round 6: packages, interfaces, modports. Fix every wrong-answer row in the spec gap table (severity tasks, net delays, wand/wor, alias, `%p`, `$writememh`). `$test$plusargs` / `$value$plusargs` | msimprotect, mregress, FST waveforms, mcov code coverage |
| **0.3** | Classes, inheritance, virtual methods, parameterized classes; constrained randomization and `std::randomize` | mwave `$dumpvars` scoping, mlint |
| **0.4** | SVA (immediate and concurrent assertions), covergroups, clocking blocks, unpacked structs | mcov |
| **0.5** | DPI-C, UVM 1.2 / IEEE 1800.2 base classes running unmodified | Native Windows build, nocgen testbenches on msim |
| **1.0** | Per-clause suite covers all of IEEE 1800-2023 and passes; faster than Verilator on `bench/`; stable command line | LTS branch |

Speed and compliance are tracked in their own work streams; the roadmap only
schedules them into releases.

## Positioning

| | minisim | Commercial (VCS, Xcelium, Questa, UniVista UVS) | Verilator | Icarus |
|---|---|---|---|---|
| Price | Free, MIT | Per-seat licenses | Free | Free |
| SystemVerilog testbench (classes, randomization, UVM) | Roadmap 0.3 to 0.5 | Yes | Partial (UVM support in progress) | Partial |
| 4-state, event-driven semantics | Yes | Yes | 2-state; `--timing` for delays | Yes |
| VCS / irun command line | Yes (`msim`) | Native to each | No | No |
| Interpreter (no C++ build step) | Yes, default | Yes | No | Yes |
| Compiled, multi-core | `-compile`, threads by default | Yes | Yes | No |

Where minisim differs: a free 4-state, event-driven simulator that takes VCS
command lines, starts instantly in interpreter mode, and compiles to native
code when speed matters, with every supported feature tied to an IEEE
1800-2023 clause test that is cross-checked against a second simulator.
