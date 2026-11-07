# minisim

A small, readable HDL simulator kernel in C++20, built as a learning project to
understand how tools like VCS work: parse with [slang](https://github.com/MikePopoloski/slang),
lower to a flat process IR, run on an event-driven kernel, dump waveforms for GTKWave.

> **Status: experimental.** It handles a subset of Verilog/SystemVerilog. It is not a VCS replacement.

## Features

- **4-state logic** (0/1/X/Z) up to 64 bits, with Verilog X-propagation rules
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
- **VCD output** (`dump.vcd`, or any file with `msim -vcd`) for GTKWave
- **`msim` command line**: compile and run your own `.sv` files in one step, irun / vcs style

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
| `bench/` | Throughput designs and `run.sh` (msim, msim -compile, Verilator, Icarus) |
| `tests/` | Unit tests for the value library |

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
| `-vcd [<file>]`, `+vcd` | Write a VCD waveform (default `dump.vcd`). No VCD is written without it |
| `-maxtime <n>` | Stop after simulation time `n`. Without it, run until `$finish` or no events remain |
| `-sv`, `-sverilog` | Accepted and ignored |
| `+<arg>` | Any other plusarg is accepted (not yet visible to `$test$plusargs`) |
| `-compile` | Compiled mode: generate C++ for the design, build `<dir>/simv`, run it |
| `-o <dir>` | Build directory for `-compile` (default `msim.d`); `simv` can be rerun with `-maxtime` / `-vcd` |
| `-cc <cxx>`, `-O<n>` | C++ compiler (default `$CXX`, else `c++`) and its optimization level (default `-O2`) |
| `--help-slang` | Every front-end option slang accepts |

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

`$display` output goes to stdout and diagnostics to stderr. The exit status is
0 when the simulation ran, 1 on a compile or lowering error, and 2 on a bad
command line.

slang is pinned to release **v12.1**. Mimalloc is off by default (`-mno-outline` is not a g++ flag); pass `-DSLANG_USE_MIMALLOC=ON` to turn it back on.

## Known limits

- Signals up to 64 bits
- `nmos`, `pmos`, `cmos`, `tran` (and the resistive forms), `bufif` / `notif`, and `trireg` charge storage are an error. Strength is resolved when a net is driven, and a read of the net is 0, 1, X, or Z.
- Instance arrays and interface or modport ports are an error
- An `inout` variable is an error. An `inout` net is connected. An `inout` whose two sides differ in width is an error
- Declaration initializers (`logic x = 0;`, `wire y = a & b;`) are lowered. A module variable's initializer runs before every `initial` / `always`
- Tasks and functions have no recursion, no `ref` arguments, no DPI, no class methods, and no cross-module calls. Each subroutine has one static copy of its arguments and locals, so two processes inside a delaying task can overwrite that copy. `&&`, `||`, and `?:` evaluate every arm, so a call on an arm that Verilog would skip still runs.
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
