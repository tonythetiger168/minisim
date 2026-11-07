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
- **`$display` / `$strobe` / `$monitor`** on the process IR, including from the slang lowerer. `$display` prints when the statement runs; `$strobe` and `$monitor` print from the postponed region, after NBA. Format is `%b`, `%d`, and literal text.
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
| `--help-slang` | Every front-end option slang accepts |

`$display` output goes to stdout and diagnostics to stderr. The exit status is
0 when the simulation ran, 1 on a compile or lowering error, and 2 on a bad
command line.

slang is pinned to release **v12.1**. Mimalloc is off by default (`-mno-outline` is not a g++ flag); pass `-DSLANG_USE_MIMALLOC=ON` to turn it back on.

## Known limits

- Signals up to 64 bits
- `nmos`, `pmos`, `cmos`, `tran` (and the resistive forms), `bufif` / `notif`, and `trireg` charge storage are an error. Strength is resolved when a net is driven, and a read of the net is 0, 1, X, or Z.
- Instance arrays and interface or modport ports are an error. A hierarchical reference that reads a child's internal signal (`top.u.hidden`) is not lowered
- An `inout` variable is an error. An `inout` net is connected. An `inout` whose two sides differ in width is an error
- Declaration initializers (`logic x = 0;`, `wire y = a & b;`) are lowered. A module variable's initializer runs before every `initial` / `always`
- Tasks and functions have no recursion, no `ref` arguments, no DPI, no class methods, and no cross-module calls. Each subroutine has one static copy of its arguments and locals, so two processes inside a delaying task can overwrite that copy. `&&`, `||`, and `?:` evaluate every arm, so a call on an arm that Verilog would skip still runs.
- System tasks other than `$display` / `$strobe` / `$monitor` / `$finish` are ignored. Format is the kernel's `%b`, `%d`, and literal text
- Vectors assumed declared `[N-1:0]`

## License

MIT
