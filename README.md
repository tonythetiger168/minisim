# minisim

A small, readable HDL simulator kernel in C++20, built as a learning project to
understand how tools like VCS work: parse with [slang](https://github.com/MikePopoloski/slang),
lower to a flat process IR, run on an event-driven kernel, dump waveforms for GTKWave.

> **Status: experimental.** It handles a subset of Verilog/SystemVerilog. It is not a VCS replacement.

## Features

- **4-state logic** (0/1/X/Z), including packed vectors wider than 64 bits, with Verilog X-propagation rules
- **Signed arithmetic**: `signed` values, `$signed` / `$unsigned`, signed compare, add, subtract, multiply, divide, remainder, and arithmetic right shift (`>>>`)
- **Compound assignment**: `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=` `<<<=` `>>>=`, same result as `a = a op b`
- **Event-driven kernel**: time wheel, active / inactive (`#0`) / NBA / postponed regions
- **`$display` / `$write` / `$strobe` / `$monitor`** (and the `b`/`o`/`h` variants). `$display` prints when the statement runs; `$strobe` and `$monitor` print from the postponed region, after NBA. Formats: `%b %o %h %x %d %c %s %t %m`, `%0` and explicit widths. `$time` / `$stime` / `$realtime`
- **`fork` / `join` / `join_any` / `join_none`**, `wait (expr)`
- **Unpacked arrays and memories**: any number of dimensions, variable indices, `foreach`, `$readmemh` / `$readmemb`
- **Generate** (loop / if / case), hierarchical names (`u.x`, `blk[2].x`), `+:` / `-:`, any packed range (`[8:1]`, `[0:7]`, `[3:0][7:0]`)
- **Constants**: `parameter`, `localparam`, enum values; unary `-`/`+`, `**`, `!==`, `~&` `~|` `~^`, replication
- **Coroutine processes**: `initial` / `always` as C++20 coroutines (`co_await Delay`, `co_await Wait`)
- **Multi-event waits**: `@(posedge clk or negedge rst_n)`, `@*`, `always_comb`
- **Resolved nets**: several drivers on a `wire` or `tri` resolve per bit by drive strength. A stronger 0 or 1 wins. Equal strengths on opposite values resolve to X. A driven X stays X, including when its 0-strength and 1-strength differ. Z yields. `assign (strong1, weak0)` sets the strength, and so do `buf`, `not`, `and`, `nand`, `or`, `nor`, `xor`, `xnor`, `pullup`, and `pulldown`. An undriven net stays Z.
- **`case` / `casez` / `casex`**, part-selects, bit-selects, `for` / `while` / `do` / `forever` / `repeat`, `break` / `continue`
- **Tasks and functions** at module scope, called from procedural code in that module. A `function` used in an expression returns a value (`return` or an assignment to the function name). A `task` is a statement. `input` is copied in. `output` and `inout` are copied back when the call returns. A `#delay` inside a task suspends that process.
- **Module instances.** A parent instantiates a child. Named (`.a(x)`) and positional port connections carry an `input` into the child and an `output` back to the parent. The output may be a continuous assign or a procedural write. An `inout` net is connected in both directions. A grandchild uses the same connection. A `#(.W(8))` override changes the port width. Child signals are named on the instance path (`u.y`).
- **Waveforms**: VCD (`msim -vcd`) or compressed FST (`msim -fst`), with the instance hierarchy as scopes, for GTKWave or Surfer
- **`msim` command line**: compile and run your own `.sv` files in one step, irun / vcs style
- **UVM**: the Accellera UVM library (IEEE 1800.2-2020, uvm-core 2020.3.1) compiles and runs agent testbenches, with its C code (regular expressions, command line, HDL backdoor) via `msim -uvm`; see [`docs/UVM.md`](docs/UVM.md)
- **DPI-C**: call C from SystemVerilog and SystemVerilog from C (clause 35), with `svdpi.h`
- **Emulation and FPGA prototyping**: `memu` puts a gate-level netlist on a board of FPGAs (clock conversion, memory mapping, partitioning, pin multiplexing, probes, transactors to `msim -mvp` models and to testbench classes) and writes a testbench that checks the emulation against the netlist; see [docs/EMULATION.md](docs/EMULATION.md)
- **Virtual prototypes**: `msim -mvp` gives the design an AXI4-Lite port onto C, C++ or SystemC/TLM models, with a memory map, interrupts and timed callbacks; see [docs/VPROTO.md](docs/VPROTO.md)
- **VPI**: user system tasks and functions, the design's object model, values and callbacks from C (clauses 36-38), with `vpi_user.h`; see [docs/VPI.md](docs/VPI.md)

## Layout

| File | Role |
|---|---|
| `src/expr.hpp` | 4-state `Val`, operators, expression tree and evaluator |
| `src/prog.hpp` | Process IR: a flat instruction list per `always`/`initial` |
| `src/exec.inc` | Coroutine that interprets a `Prog` |
| `src/minisim.cpp` | Kernel (scheduler, nets, VCD) and a hand-built counter demo |
| `src/lower.hpp` | slang AST to `Prog`, including module instances and port connections |
| `src/slang_dump.cpp` | Small slang front-end probe: lists variables, nets, processes |
| `src/msim.cpp` | `msim` command line: slang driver, lower, run, optional VCD |
| `src/parallel.hpp` | Shared by the tools: default thread count, `-threads N`, `par::parallelFor`, `par::Pool` |
| `src/wave_reader.hpp` | Shared by the tools: `wave::Reader`, VCD (parallel, mapped) and FST into memory |
| `src/protect.hpp`, `src/msimprotect.cpp` | Protected envelopes (1800-2023 clause 34 / IEEE 1735): encrypt and decrypt with OpenSSL |
| `src/codegen.hpp` | Compiled mode (`msim -compile`): each process as generated C++ |
| `src/dpi.inc`, `include/svdpi.h` | DPI-C run time (libffi calls, exports, svdpi.h functions) and the C header |
| `src/vpi.inc`, `include/vpi_user.h` | VPI run time (user system tasks, objects, values, callbacks) and the C header |
| `src/lef.hpp`, `src/def.hpp` | LEF 5.8 and DEF 5.8 readers and a DEF writer (namespace `phys`), shared by the physical tools |
| `src/mrcx.cpp`, `src/rcx.hpp` | Parasitic RC extraction (`mrcx`): routed DEF + LEF in, SPEF out |
| `src/spef.hpp` | SPEF (IEEE 1481) reader and writer, shared by extraction, timing and power |
| `bench/` | Throughput designs (`gen_wide.py` writes `wide.sv`) and `run.sh` (msim, msim -compile, Verilator, Icarus) |
| `tests/` | Unit tests for the value library |

## Install

Release packages for Linux x86_64 (`.deb` and `.tar.gz`) are on the
[Releases](https://github.com/tonythetiger168/minisim/releases) page; see
[`docs/RELEASING.md`](docs/RELEASING.md) for installing and for how releases
are made, [`CHANGELOG.md`](CHANGELOG.md) for what changed, and
[`docs/PRODUCT.md`](docs/PRODUCT.md) for the product line and roadmap.
`msim -version` prints the installed version.

## Build and run

```sh
cmake -B build
cmake --build build -j
./build/minisim          # runs the demo, writes dump.vcd
ctest --test-dir build   # unit tests
gtkwave dump.vcd
```

With the slang front end (downloads and builds slang, takes a while):

```sh
CXX=g++ cmake -B build -DMINISIM_WITH_SLANG=ON
cmake --build build -j
./build/slang_dump examples/counter.sv
```

### Simulating your own design: `msim`

The slang build also makes `msim`, a one-step compile, elaborate, and simulate
command in the style of `irun` / `xrun` or `vcs -R`:

```sh
./build/msim examples/cli/adder.sv examples/cli/adder_tb.sv -top adder_tb +define+WIDTH=8 -vcd adder.vcd
```

| Option | Meaning |
|---|---|
| `<files>` , `-f <file>` | Source files, or a command file listing them |
| `-top <name>` | Top-level module (repeatable). Without it, slang picks uninstantiated modules |
| `+define+N[=V]`, `-D N[=V]` | Preprocessor define |
| `+incdir+<dir>`, `-I <dir>` | Include directory |
| `-vcd [<file>]`, `+vcd` | Write a VCD waveform (default `dump.vcd`). No waveform is written without it or `-fst` |
| `-cov [<file>]` | Write line / branch / toggle / condition / FSM coverage (default `cov.json`); see `mcov`. `-cm line+cond+fsm+tgl` (as VCS) does the same |
| `-fst [<file>]`, `+fst` | Write an FST waveform (default `dump.fst`); `-vcd x.fst` does the same. About 7x smaller than VCD and faster to write; GTKWave and Surfer open it |
| `-maxtime <n>` | Stop after simulation time `n`. Without it, run until `$finish` or no events remain |
| `-sv`, `-sverilog` | Accepted and ignored |
| `+<arg>` | Any other plusarg is accepted (not yet visible to `$test$plusargs`) |
| `-compile` | Compiled mode: generate C++ for the design, build `<dir>/simv`, run it |
| `-o <dir>` | Build directory for `-compile` (default `msim.d`); `simv` can be rerun with `-maxtime` / `-vcd` |
| `-no-static-comb` | Run assigns and `always_comb` as ordinary event-driven processes |
| `-threads <n>` | Worker threads (default: hardware threads, at most 8; capped at the hardware threads) |
| `-no-static-seq`, `-no-partition` | Turn off clocked-process functions / partition tasks |
| `-no-2state` | `-compile` without the 2-state fast path |
| `-no-cycle` | `-compile` without the cycle domain (Verilator-style eval of clocked + comb logic) |
| `-cc <cxx>`, `-O<n>` | C++ compiler (default `$CXX`, else `c++`) and its optimization level (default `-O2`) |
| `-xprop[=tmerge\|xmerge]`, `-xprop-report <file>` | X-propagation: an unknown `if` / `case` condition or clock edge merges the branches instead of picking one; see [`docs/XPROP.md`](docs/XPROP.md) |
| `-upf <file>` | Power-aware simulation with this UPF (IEEE 1801) power intent; see [`docs/LOWPOWER.md`](docs/LOWPOWER.md) |
| `-power_top <inst>`, `-upf_verbose` | The instance the UPF describes; print supply, power state and simstate changes |
| `<file>.c`, `.cpp`, `.o`, `.a` | DPI-C code: msim builds it (with `svdpi.h` on the include path) and loads it |
| `<file>.so`, `-sv_lib <name>`, `-sv_root <dir>`, `-sv_liblist <file>` | DPI-C libraries to load (IEEE 1800-2023 annex J) |
| `-dpiheader <file>` | Write the C prototypes of every DPI import and export |
| `-load <lib>[:<boot>]`, `-loadvpi <lib>:<boot>` | Load a VPI library before elaboration; its `vlog_startup_routines` run (the C files and `.so` files above are VPI libraries too) |
| `-CFLAGS <s>`, `-LDFLAGS <s>` | Extra flags for building the DPI C files |
| `-version` | Print the version and git commit |
| `-keyfile [[<owner>:]<keyname>=]<priv.pem>` | RSA private key for protected envelopes (repeatable); `-keyring <file>` reads several, `-license <feature>` grants a licensed right; see below |
| `--help-slang` | Every front-end option slang accepts |

### C code: DPI-C

`import "DPI-C"` and `export "DPI-C"` work as in IEEE 1800-2023 clause 35.
Give the C files on the command line, or a prebuilt library with `-sv_lib`:

```sh
./build/msim tb.sv model.c                     # builds model.c, then runs
./build/msim tb.sv -sv_lib libmodel            # loads libmodel.so
./build/msim tb.sv model.c -dpiheader dpi.h    # also writes the C prototypes
```

C code includes `svdpi.h` (installed in `include/`, and in `include/` of the
source tree). Import tasks may call exported tasks that wait on simulation
time. A DPI call runs on the interpreter: `-compile` falls back to it.

### Regressions: `mregress`

`mregress` (installed next to `msim`; in the source tree `tools/mregress.py`)
runs a test list on msim, VCS, Xcelium, Questa, or Icarus, in parallel, with
seeds, and writes `results.json`, `junit.xml` (for CI) and `report.html`, with
failures grouped by their first error line:

```sh
mregress tests.list -sim msim -j 8 -seeds 3     # or -sim vcs / xrun / questa / iverilog
mregress tests.list -rerun                      # repeat only the failures
mregress -spec tests/spec -sim iverilog         # the spec suite on another simulator
```

A test list line is `name: <simulator arguments>` with optional settings
(`@seeds=N`, `@timeout=SEC`, `@pass=<regex>`, `@fail=<regex>`,
`@expect=inline` for `//> ` expected output, `@skip=<sim>`, `@exit=N`,
`@xfail=<reason>` for a known gap); `%common` and
`%sim` set shared arguments and the default simulator, and `-f` includes
another list. A run fails on a non-zero exit, a UVM_ERROR / UVM_FATAL count
above zero, a simulator error line (`Error-[`, `*E,`, `** Error`), `$fatal`,
or a failed `@pass` / `@expect` check. An `@xfail` test that fails counts as
a known gap; one that passes (XPASS) fails the regression so the marker gets
removed. With `-spec`, a test's `//! xfail: <reason>` and `//! exit: <n>`
lines mean the same on msim, and a per-chapter pass table is printed
(`-md FILE` also writes it as Markdown). See `tests/regress/smoke.list` and
`mregress -h`.

### Bug reports: `mbug`

`mbug` (in the source tree `tools/mbug.py`) files each distinct failure as
one issue in **GitHub Issues** (the default), **Redmine** or **Jira**. A
failure that is already open gets a comment for the new occurrence instead
of a duplicate. A failure whose issue was closed gets a new issue that links
to the old one. Each issue has the failing tests and seeds, a reproduction
command, the log tail and a link to the CI build. Redmine and Jira also get
`sim.log` and `cmd.sh` attached.

```sh
export GITHUB_TOKEN=...                                          # issues: write
mregress tests.list -o regress || mbug regress regress/results.json -project you/repo
mlint -top top rtl/*.sv | mbug lint - -project you/repo          # one issue per rule
mdft -top top rtl/*.sv > dft.log; mbug lint dft.log -tool mdft -project you/repo
mbug regress regress/results.json -tracker jira -project VER     # JIRA_URL, JIRA_USER, JIRA_TOKEN
mbug regress regress/results.json -tracker redmine -project soc  # REDMINE_URL, REDMINE_KEY
mbug sync-github -repo you/repo -tracker jira -project VER       # GitHub issues -> Jira, kept in step
mbug regress regress/results.json -project you/repo -dry-run     # show the plan only
```

Credentials are read only from the environment, and a `-config` file that
contains a token is refused. See [`ci/README.md`](ci/README.md#bug-reports)
and `mbug -h`.

### Coverage: `msim -cov` and `mcov`

`msim -cov [file.json]` records line coverage, branch coverage (both outcomes
of every `if`, `case` arm, and loop test), toggle coverage (every signal
bit seen rising and falling), condition coverage and FSM coverage into `cov.json`. `mcov` merges databases and
reports them; `mregress -cov` does both for a whole regression:

```sh
msim tb.sv -cov run1.json
mcov merge run1.json run2.json -o all.json
mcov report all.json -html cov_html -lcov all.info -min-line 90
mregress tests.list -cov            # regress.d/cov.json + regress.d/cov_html/
```

The LCOV file works with `genhtml` and with editor and CI coverage plugins.
`-cov` works in the interpreter and with `-compile`, with any `-threads`;
the database is the same in each.

**Condition coverage** (as VCS `-cm cond`). A condition `a && b && c`,
`a || b || c`, or the condition of `?:`, with n terms has n + 1 vectors: the
one where no term decides alone (all 1 for `&&`, all 0 for `||`) and, for each
term, the one where only that term has the deciding value. A vector with an
x term counts for none. Conditions in procedures, continuous assignments,
functions and tasks are covered; not those in constraints, assertions and
covergroups, constant ones, or ones whose terms call a function or read
through a class handle or a container (every term is evaluated to tell the
vectors apart, so a term must be safe to evaluate).

**FSM coverage** (as VCS `-cm fsm`). msim finds the state registers itself: a
variable that one clocked procedure assigns, whose next value is always a
constant (literal, parameter or enum name), itself, or a next-state variable
assigned that way by one combinational procedure, where that logic branches
on the register with `case (state)` or `if (state == S)`. The states are the
constants compared with or assigned to the register; the transitions are the
ones the next-state logic can make. A constant assigned under an `if` outside
any branch on the state is taken as the reset, and transitions that only the
reset makes are marked `(reset)`; `mcov report -fsm-reset-filter` leaves them
out (VCS `-cm_fsmresetfilter`). A change the logic does not show (a forced
value, say) is listed as "not in the next-state logic". Not extracted: one-hot
registers tested bit by bit, states computed by arithmetic, registers written
through a part select or by a task.

```sh
mcov report all.json -detail              # every vector, state and transition
mcov report all.json -uncovered           # only what is missing
mcov report all.json -min-cond 80 -min-fsm-state 100 -min-fsm-trans 90
```

```
Condition tb.sv:30:15  a && b
    -1- a
    -2- b
    -1- -2-  Hits
     1   1   3
     0   1   Not covered
     1   0   Not covered

FSM tb.u.state  (module ctrl, tb.sv:8)  states 3/4, transitions 3/6
    state FAIL                 Not covered
    RUN -> IDLE                Not covered  (reset)
    RUN -> FAIL                Not covered
```

The database also holds functional coverage: the bins of every covergroup
type (19) and the evaluations, passes and failures of every assertion (16).
`mcov report` prints both, lists uncovered bins and failed or idle
assertions in the HTML, and gates them with `-min-func` and `-min-assert`.

### Verification planning: `mplan`

`mplan` (in the source tree `tools/mplan.py`) tracks a verification plan
against the regressions: which features are proven, by which tests,
coverage and assertions, how that moves over time, and what the failures
are. A plan is a small indented text file:

```
plan uart "UART verification plan"
  owner: tony
  feature tx "Transmit path" weight=2 owner=alice
    test   uart_tx_*                # mregress tests
    line   rtl/uart_tx.sv goal=95   # code coverage
    cover  tb.cg_tx                 # a covergroup type, or type.item
    assert tb.dut.u_tx              # the assertions in that scope
    feature parity "Parity"
      test uart_parity_*
```

```sh
mregress tests.list -cov -plan uart.vplan   # regress, then score the plan into regress.d/plan/
mplan report uart.vplan -results regress.d -label $GIT_COMMIT -rules triage.rules -gate
mplan triage -results regress.d -plan uart.vplan   # only the failure buckets
mplan trend  plan.d/history.jsonl                  # score, tests and coverage per snapshot
mplan init   -results regress.d -o draft.vplan     # a starting plan from an existing regression
```

`mplan report` scores every feature (weighted, with a goal per metric),
writes `plan.json` and one self-contained `plan.html` (feature tree, trend
chart, failure triage, results that no plan line points at), and appends a
snapshot to `history.jsonl`. Failures are bucketed by signature and marked
new, recurring (since which snapshot), flaky or fixed, with the features and
owners each bucket blocks; a rules file marks buckets known or waived and
attaches a bug id. `-gate`, `-min-score` and `-fail-on-new` turn it into a
CI gate. Full reference: [`docs/MPLAN.md`](docs/MPLAN.md) and `mplan -h`.

### Protected IP: `msimprotect` and `msim -keyfile`

minisim reads and writes IEEE 1735-2023 version 3 envelopes, the format
commercial simulators exchange encrypted IP in: a random root key, RSA
encrypted for each tool's key (a tool block, with the IP author's rights for
that tool), AES-256-CBC for the source, and an HMAC-SHA512 signature that is
checked before anything is decrypted. The older IEEE 1800-2023 clause 34
envelopes work too (`msimprotect -v1`). The crypto is OpenSSL's libcrypto.
Mark the region to hide and encrypt it:

```sh
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out acme.pem
openssl pkey -in acme.pem -pubout -out acme_pub.pem
./build/msimprotect -key acme_pub.pem -keyowner Acme -keyname acme-1 \
                    -control visibility=interface examples/protect/secret_core.sv -o core.svp
./build/msim  -keyfile Acme:acme-1=acme.pem core.svp examples/protect/core_tb.sv -vcd waves.vcd
./build/mlint -keyfile Acme:acme-1=acme.pem core.svp examples/protect/core_tb.sv
```

msim, mlint and mdft keep the protected code out of diagnostics, waveforms,
coverage, lint findings and the DFT chain plan, except what the rights
(`visibility`, `coverage`) and `viewport` pragmas allow.
[`docs/PROTECT.md`](docs/PROTECT.md) has the format, recipes, keyrings
(`-keyring`, IEEE 1735 key exchange format), the rights, and a clause by
clause status.

### Compiled mode

By default `msim` interprets: each process is a flat instruction list that a
coroutine walks. `msim -compile` translates every process into C++ instead
(one coroutine per process, `goto` for jumps, inline 4-state operators for
expressions), compiles it against the same kernel, and runs it. Output is the
same as the interpreter's (`ctest -R msim_compile` checks this); a design that
uses something `codegen.hpp` does not translate yet falls back to the
interpreter with a note on stderr. Times from `bench/run.sh` (lanes with END_TIME=10000000; 4-core cloud
VM, g++ 13, seconds of run time, build time not included):

| Design | msim | msim -compile | Verilator 5.020 -O3 | Icarus 13.0 |
|---|---|---|---|---|
| `bench/lanes.sv` (16 LFSR lanes, 1M cycles) | 8.13 | 3.11 | 0.29 | 29.2 |
| `bench/chain.sv` (8-deep assign chain, 1M cycles) | 1.35 | 0.74 | 0.20 | 7.82 |
| `bench/counter.sv` (one counter, 1M cycles) | 0.22 | 0.14 | 0.19 | 0.31 |

The C++ build adds 2 to 4 seconds per design.

### Static combinational schedule

Continuous assigns, gates, and straight-line `always_comb` / `always @*`
blocks (only assignments, waiting on any change of their inputs) do not run
as event-driven processes. msim sorts them into one topological order at
startup, the levelized scheduling Verilator and VCS use: when a signal
changes, the dependent blocks run in that order, once each, as plain
functions. Blocks on a combinational loop stay event-driven. Both the
interpreter and `-compile` use the schedule; `-no-static-comb` turns it off.

### Multi-core

msim uses up to 8 threads by default, fewer on a machine with fewer hardware
threads (`-threads N` to choose, `-threads 1` for one). More is rarely faster:
on a 20-core / 28-thread i7-14700, `bench/wide.sv` was fastest at 4 to 12
threads, and at 24 or more one descheduled thread held up every batch. Idle
workers, and the main thread waiting for a batch, spin briefly and then sleep.
Two kinds of work run in parallel:

- **Clocked processes.** An `always @(posedge clk)` block that only makes
  nonblocking assignments runs as a function that writes its NBAs into its
  own buffer. All of them triggered in one time slot run as one parallel
  batch, and their NBAs join the queue in process order.
- **Partitions**, like Verilator's macro-tasks. Signals written by
  combinational logic or by those NBAs are grouped with all the combinational
  logic that reads them, so the design splits into islands that share no
  written signal. In the NBA region each island applies its NBAs and
  re-evaluates its logic as one task. An island goes through the ordinary
  serial path when something outside could watch it mid-region (a VCD, a
  `$monitor`, a process waiting on one of its signals).

Results are committed in a fixed order, so the output is the same for every
thread count (`ctest -R msim_threads` checks it). Small batches stay on one
thread. `-no-static-seq` and `-no-partition` turn the two parts off.

### Cycle domain (`-compile`)

Like Verilator's `eval()`. The largest group of clocked processes that wait
on the same events (usually every `always @(posedge clk)`) runs as one
function. Their NBAs go into fixed slots, and in the NBA region the slots are
written and every static combinational process is re-evaluated once, in rank
order. Writes skip `set()` when nothing else can see the signal (no process
waits on it, no VCD, no `$monitor`), and a signal that only the domain touches
lives in a plain C++ variable instead of the kernel's signal table. The time
wheel also no longer allocates per time step. `-no-cycle` turns the domain off.

Signals the testbench only initializes and `$display`s can be private too:
those writes and reads are compiled to use the variable. When every signal
the domain reads is known (no X or Z), a second copy of the domain without
the per-expression X checks runs instead, and the rare 4-state code is kept
out of the hot path.

On a 4-core VM, run seconds, 1 thread (bench/run.sh):

| design | before (main) | cycle domain | Verilator -O3 |
|---|---|---|---|
| lanes | 0.34 | 0.07 | 0.09 |
| chain | 0.35 | 0.08 | 0.28 |
| counter | 0.16 | 0.08 | 0.28 |
| wide | 1.2 (4 threads) | 0.18 | 0.10 |

When every signal the domain writes is private, the domain is split into
islands: processes and continuous assignments that share no signal it writes.
With at least 32 islands, groups of them run on separate threads, each
computing its NBAs, writing them and re-evaluating its own combinational logic
with no locks. When nothing else can run before the NBA region (no `#0` and no
other combinational process pending), that is one parallel pass per clock edge.

| wide (run seconds) | 1 thread | 2 threads | 4 threads |
|---|---|---|---|
| msim -compile | 0.13 | 0.07 | 0.08 |
| Verilator -O3 `--threads N` | 0.10 | 0.14 | slower |

### 2-state fast path (`-compile`)

Most of the time no signal holds X or Z. For each expression, `-compile`
emits a check that every signal it reads is fully known, and in that case
plain 64-bit integer arithmetic; otherwise the 4-state operators. Division,
modulo, `**`, and a variable bit index keep the 4-state code, because they
can make X from known inputs. `-no-2state` turns the fast path off, and
`ctest -R msim_fuzz` compares it with the interpreter on random expressions
(`tests/fuzz/gen_expr.py`) with X and Z inputs.

`$display` output goes to stdout and diagnostics to stderr. The exit status is
0 when the simulation ran, 1 on a compile or lowering error, and 2 on a bad
command line.

slang is pinned to release **v12.1**. Mimalloc is off by default (`-mno-outline` is not a g++ flag); pass `-DSLANG_USE_MIMALLOC=ON` to turn it back on.

### Static timing analysis: `msta`

`msta` times a gate-level netlist against SDC constraints with Liberty
libraries, PrimeTime style, and builds without the slang front end:

```sh
msta -lib cells.lib netlist.v -top top -sdc top.sdc      # check_timing, worst setup and hold paths, summary
msta -lib cells.lib netlist.v -sdc top.sdc -sdf top.sdf  # delays and timing checks for msim -sdf
msta -f run.tcl                                          # report_timing, report_constraint, ... in a Tcl script
```

It runs on every core by default (`-threads N`), with the same results at any
thread count. Commands, what is modeled and the current limits are in
[docs/STA.md](docs/STA.md).

### Datapath library: `dw/`

`dw/` is a library of synthesizable, parameterized building blocks
(`MW_add`, `MW_mult`, `MW_div`, `MW_shifter`, `MW_fifo_sync`,
`MW_fifo_async`, `MW_crc`, `MW_ecc` and 39 more), each with selectable
architectures where that matters (ripple, lookahead, Kogge-Stone, Brent-Kung
and carry-select adders; array and Booth/Wallace multipliers):

```sh
msim tb.sv rtl/*.sv -y dw +libext+.sv -top tb
```

`docs/DATAPATH.md` lists every module and its ports.

### Timing constraint checks: `msdc`

`msdc` reads the design and its SDC file and reports what is wrong or missing
in the constraints: syntax and names that match nothing, clocks that clock
nothing, registers no clock reaches, ports with no I/O delay, exceptions that
are not startpoint-to-endpoint or have no path in the design, a setup
multicycle with no hold, and paths between clocks whose relationship nobody
declared. 32 rules, multithreaded, same report for any `-threads N`.

```sh
msdc rtl/*.sv -top soc -sdc soc.sdc         # the report, then a summary
msdc rtl/*.sv -top soc -sdc soc.sdc -Werror # exit 1 on any finding (for CI)
msdc -sdc soc.sdc                           # the file alone
msdc -list                                  # the rules
```

The SDC reader (`src/sdc.hpp`) and the Tcl interpreter under it
(`src/tcl.hpp`) are shared with the timing and synthesis tools. See
[docs/CONSTRAINTS.md](docs/CONSTRAINTS.md).

### Lint and clock-domain checks: `mlint`

`mlint` runs the same front end as `msim` and then checks the elaborated
design, so it needs no separate build setup:

```sh
mlint rtl/*.sv -top soc_top                 # warnings, then the rules below
mlint rtl/*.sv -top soc_top -Werror         # exit 1 on any finding (for CI)
mlint rtl/*.sv -disable L004 -disable CDC002
mlint rtl/*.sv -top soc_top -cdc-report     # clock domains, resets, every crossing
mlint -list                                 # the rules
```

| Rule | What it finds |
|---|---|
| `L001` | A value is assigned on some paths of an `always_comb` but not all, so it holds its old value: an unintended latch (an `if` with no `else`, a `case` with no `default`) |
| `L002` | A blocking assignment to a non-local variable in an `always_ff` |
| `L003` | A non-blocking assignment in an `always_comb` |
| `L004` | One value is assigned by more than one process |
| `CDC001` | A flop depends on another clock domain with no synchronizer, or through one flop only |
| `CDC002` | A multi-bit bus crosses domains through a synchronizer and is not Gray coded |
| `CDC003` | Combinational logic in front of a synchronizer: a glitch can be captured |
| `CDC004` | Reconvergence: separately synchronized signals of one domain meet again |
| `CDC005` | One signal is synchronized more than once into the same domain |
| `RDC001` | An asynchronous reset generated in another clock domain |
| `RDC002` | One asynchronous reset used directly in several clock domains |
| `RDC003` | A flop feeds a flop with a different asynchronous reset, in one clock domain |
| `RDC004` | A reset flop feeds a flop with no asynchronous reset (off by default: `-enable RDC004`) |

Clock and reset domains are found from the RTL: the clock of every
edge-triggered block, followed through ports, buffers and clock gates, so one
clock passed down through several modules stays one domain. Multi-flop
synchronizers (also shift registers and synchronizer cells), enable-qualified
buses and Gray-coded pointers are recognized as safe. `-cdc-report` lists
every domain and crossing, `-clock-group a,b` declares related clocks,
`-static sig` waives a quasi-static signal, and `-threads N` sets the worker
threads (default: every logical CPU). See [docs/CDC.md](docs/CDC.md).
Findings print as `file:line:col: warning: [RULE] text`, sorted by file and
line.

### Source-level debug: `mdebug`

`mdebug` lists the drivers and loads of a signal with the conditions on each
assignment, prints fan-in cones, and with a VCD finds the assignment that
gave a signal its value at a time and what caused it:

```sh
mdebug tb.sv dut.sv -top tb -drivers tb.u0.y
mdebug tb.sv dut.sv -top tb -vcd waves.vcd -active tb.sum -at 70
mdebug tb.sv dut.sv -top tb -vcd waves.vcd -html debug.html   # one-file viewer
```

The viewer page has the hierarchy, clickable source, drivers and loads, a
schematic of the selected signal, and waves with the values annotated in the
source. See [`docs/DEBUG.md`](docs/DEBUG.md).

### Low-power simulation: UPF (IEEE 1801)

`msim -upf dut.upf` simulates with the design's power intent: power domains,
supply nets and power switches, corruption of powered-off logic, isolation,
level shifters, retention, power states and PSTs. The testbench drives
supplies with `supply_on` / `supply_off` from package `UPF`, which msim
adds. `mlint -upf dut.upf` checks the power intent statically (missing
isolation or level shifters, names not in the design, ...). See
[`docs/LOWPOWER.md`](docs/LOWPOWER.md).

```sh
msim tb.sv dut.sv -top tb -upf dut.upf -power_top u_dut [-upf_verbose]
mlint tb.sv dut.sv -top tb -upf dut.upf
```

### Formal property checking: `mformal`

`mformal` proves or falsifies the design's assertions for every input
sequence, on the same front end and lowering as `msim`. Assertions are
proven or falsified, assumptions constrain the inputs, covers are reached or
proven unreachable. A failure comes with a VCD of the whole design and, with
`-tb`, a testbench that replays it in `msim`.

```sh
mformal rtl/*.sv -top fifo                  # every assert / assume / cover
mformal rtl/*.sv -top fifo -depth 40 -tb    # deeper, with replay testbenches
```

```
  assert  top.u.a_count        proven  (induction, k=2)
  assert  top.u.a_push_counts  falsified  at cycle 5  mformal_out/top.u.a_push_counts.vcd
  cover   top.u.c_full         covered  at cycle 4  mformal_out/top.u.c_full.vcd
```

Properties are checked in parallel on every core (`-threads N`), with the
same result for any thread count. See `docs/FORMAL.md` for the model (clock,
reset, two-state values, what is cut) and the supported property operators.

### Equivalence checking: `mequiv`

`mequiv` proves that a netlist is the same circuit as the RTL it came from
(or as another netlist, or one RTL revision as the next), the way Formality
and Conformal do: flops are matched by name and cut, and every output and
next-state function is proved equal. A failing point comes with the input
values that show the difference.

```sh
mequiv -ref rtl/*.sv -impl netlist.v cells.v -top alu
```

See [docs/EQUIV.md](docs/EQUIV.md).

### Design for test: `mdft`

`mdft` checks that a design can be scan-tested, and prints the flop count per
clock domain, which is what a chain plan is built from:

```sh
mdft rtl/*.sv -top soc_top
mdft rtl/*.sv -top soc_top -Werror      # exit 1 on any finding (for CI)
mdft -list
```

| Rule | What it finds |
|---|---|
| `DFT001` | A gated or unreachable clock: the tester cannot drive the flop in shift mode |
| `DFT002` | An asynchronous set or reset built from logic, which scan shift cannot hold inactive |
| `DFT003` | A latch: not scannable without a transparency control |
| `DFT004` | Both edges of one clock, so the two halves need separate chains with a lockup latch |
| `DFT005` | A combinational loop |
| `DFT006` | A ripple or divided clock (a flop drives the clock) |

```
mdft: 5 flops in 3 clock domains
  t.clk: 3 flops (2 rising, 1 falling)
```

On a gate netlist `mdft -insert_scan` stitches the flops into scan chains
(scan cells from the Liberty library or a mux in front of D, balanced chains
per clock domain, lockup latches, clock gates held open) and writes the
netlist, a chain description for ATPG and a chain flush testbench.
`mdft -mbist` generates a March memory BIST controller and collar for a RAM
and reports the fault coverage of the chosen algorithm. See
[docs/DFT.md](docs/DFT.md).

```sh
mdft -insert_scan design.v -lib cells.lib -o design_scan.v -chains 8 -scandef chains.json -tb flush_tb.v
mdft -mbist -name ram256x32 -words 256 -width 32 -backgrounds all -o ram_bist.v -tb ram_bist_tb.v
```

`mlint` and `mdft` share one model of the elaborated design
(`src/lintir.hpp`): the blocks with what each assigns, reads and assigns on
every path, and the union-find that makes one clock one domain across module
boundaries.

### Emulation and FPGA prototyping: `memu`

`memu` is the step between synthesis and FPGA place and route that makes a
design fit a board of several FPGAs and run there at all: every flop moves
onto one free-running clock, memories become FPGA RAM, the cells are split
over the FPGAs, and signals share wires where the board has too few.

```sh
memu soc.gv -lib cells.lib -mem mems.txt -fpgas 4 -lut 200000 -pins 240 -probe 'u_core.pc*' -o out
msim cells.v mems.v soc.gv out/soc_fpga*.v out/soc_emu_lib.v out/soc_emu.v out/soc_emu_tb.sv -top soc_emu_tb
```
```
memu: PASS, 2000 events (2000 with a clock edge), 80856 output bits compared, netlist and emulation agree
```

The second command needs no hardware: it runs the netlist `memu` was given
next to the emulation it wrote, on the same random inputs, and compares every
output at every clock event. Transactors (`memu -xtor-sv`) connect the
emulated design to the C models of a virtual prototype or to a testbench
class, with the design's clock stopped while the host answers. It uses every
logical CPU by default (`-threads N`), with the same result on any number.
[docs/EMULATION.md](docs/EMULATION.md) has the timing model, the file formats
and the transactors.
### Parasitic extraction: `mrcx`

`mrcx` reads a routed DEF and its LEF and writes every net's resistance and
capacitance, coupling included, as SPEF for timing and power analysis:

```sh
mrcx -lef tech.lef -lef cells.lef -def routed.def -o design.spef
mrcx -lef tech.lef -lef cells.lef -def routed.def -itf process.itf -temp 125 -o slow.spef
```

```
mrcx: gcd: 411 nets, 22243.4 um of wire, 6986 nodes, 6688 resistors, 5134 coupling capacitors
mrcx: capacitance 2029 fF to ground, 409.5 fF between nets (sky130 stack, 25 C)
```

It runs on every core (`-threads N`), writes the same file for any thread
count, and reports open and shorted nets. On the OpenROAD `gcd` test design
its total net capacitance is within 2% of OpenRCX's, and the ground and
coupling parts within 1% and 4%. See
[`docs/RCX.md`](docs/RCX.md).

### Virtual prototypes: `-mvp`

A memory, a peripheral, or a whole subsystem can be a program instead of RTL,
so software and design meet before the RTL is finished. The design drives an
AXI4-Lite port and the model answers in C or C++:

```sh
msim -mvp soc_tb.sv my_model.c -top soc_tb
```

`-mvp` adds `mvp.sv` (`mvp_axil_slave`, `mvp_irq_slave`, `mvp_clock`) and the
runtime that keeps the memory map, the interrupt lines and the models' time.
A model is ordinary C against [`include/mvp.h`](include/mvp.h):
`mvp_add_device` to join the map, `mvp_after_ps` to take time, `mvp_set_irq`
to interrupt, `mvp_bus_read` / `mvp_bus_write` to be a master. A TLM-2.0
target goes behind the same bridge with `mvp_tlm.h`. A memory and a 16550-style
UART are bundled in `mvp/models/`. See [docs/VPROTO.md](docs/VPROTO.md).

## Known limits

- `nmos`, `pmos`, `cmos`, `tran` (and the resistive forms), `bufif` / `notif`, and `trireg` charge storage are an error. Strength is resolved when a net is driven, and a read of the net is 0, 1, X, or Z.
- Instance arrays and interface or modport ports are an error
- An `inout` variable is an error. An `inout` net is connected. An `inout` whose two sides differ in width is an error
- Declaration initializers (`logic x = 0;`, `wire y = a & b;`) are lowered. A module variable's initializer runs before every `initial` / `always`
- Tasks and functions have no recursion, no `ref` arguments, no class methods, and no cross-module calls. Each subroutine has one static copy of its arguments and locals, so two processes inside a delaying task can overwrite that copy. `&&`, `||`, and `?:` evaluate every arm, so a call on an arm that Verilog would skip still runs.
- System tasks other than the display family, `$finish`, and `$stop` are ignored with a warning
- Unpacked arrays: fixed size only, at most 2^20 elements; no slices or whole-array operations. Array elements are left out of the VCD. See `docs/COVERAGE.md` for the full list

## Spec coverage and tests

[`docs/COVERAGE.md`](docs/COVERAGE.md) maps IEEE 1800-2023 clauses to what minisim
supports. `tests/spec/` holds one small test per clause, named after it
(`9.3.2-fork-join.sv`). The expected output is in the file as `//> ` comments.
`ctest` runs each one on `msim` (`spec/...`), and on Icarus Verilog too when
`iverilog` is installed (`icarus/...`), so an expectation is checked against
a second simulator. CI uses Icarus Verilog 13.0, built from its `v13_0` tag; a
test Icarus 13 cannot run says why on a `//! icarus-skip:` line. Locally, point
cmake at another build with `-DIVERILOG=... -DVVP=...`. To add one, drop a `.sv` file in `tests/spec/` and re-run cmake.

## License

MIT
