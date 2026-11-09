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
- **DPI-C**: call C from SystemVerilog and SystemVerilog from C (clause 35), with `svdpi.h`

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
| `src/codegen.hpp` | Compiled mode (`msim -compile`): each process as generated C++ |
| `src/dpi.inc`, `include/svdpi.h` | DPI-C run time (libffi calls, exports, svdpi.h functions) and the C header |
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
| `-cov [<file>]` | Write line / branch / toggle coverage (default `cov.json`); see `mcov` |
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
| `-cc <cxx>`, `-O<n>` | C++ compiler (default `$CXX`, else `c++`) and its optimization level (default `-O2`) |
| `<file>.c`, `.cpp`, `.o`, `.a` | DPI-C code: msim builds it (with `svdpi.h` on the include path) and loads it |
| `<file>.so`, `-sv_lib <name>`, `-sv_root <dir>`, `-sv_liblist <file>` | DPI-C libraries to load (IEEE 1800-2023 annex J) |
| `-dpiheader <file>` | Write the C prototypes of every DPI import and export |
| `-CFLAGS <s>`, `-LDFLAGS <s>` | Extra flags for building the DPI C files |
| `-version` | Print the version and git commit |
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

### Coverage: `msim -cov` and `mcov`

`msim -cov [file.json]` records line coverage, branch coverage (both outcomes
of every `if`, `case` arm, and loop test), and toggle coverage (every signal
bit seen rising and falling) into `cov.json`. `mcov` merges databases and
reports them; `mregress -cov` does both for a whole regression:

```sh
msim tb.sv -cov run1.json
mcov merge run1.json run2.json -o all.json
mcov report all.json -html cov_html -lcov all.info -min-line 90
mregress tests.list -cov            # regress.d/cov.json + regress.d/cov_html/
```

The LCOV file works with `genhtml` and with editor and CI coverage plugins.
Covergroups (functional coverage, 19) come with the spec work. `-compile`
with `-cov` runs in the interpreter for now.

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
