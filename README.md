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
- **Resolved nets**: several `assign`s to one wire resolve per bit (Z yields, conflicts give X)
- **`case` / `casez` / `casex`**, part-selects, bit-selects, `for` / `while` / `repeat`
- **VCD output** (`dump.vcd`) for GTKWave

## Layout

| File | Role |
|---|---|
| `src/expr.hpp` | 4-state `Val`, operators, expression tree and evaluator |
| `src/prog.hpp` | Process IR: a flat instruction list per `always`/`initial` |
| `src/exec.inc` | Coroutine that interprets a `Prog` |
| `src/minisim.cpp` | Kernel (scheduler, nets, VCD) and a hand-built counter demo |
| `src/lower.hpp` | slang AST to `Prog` (flat module; a nested instance is an error) |
| `src/slang_dump.cpp` | Small slang front-end probe: lists variables, nets, processes |
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

slang is pinned to release **v12.1**. Mimalloc is off by default (`-mno-outline` is not a g++ flag); pass `-DSLANG_USE_MIMALLOC=ON` to turn it back on.

## Known limits

- Signals up to 64 bits; no strengths
- Instantiating a module is an error until port connections are implemented
- Not yet lowered: `break`/`continue`, net initializers, tasks/functions
- System tasks other than `$display` / `$strobe` / `$monitor` / `$finish` are ignored. Format is the kernel's `%b`, `%d`, and literal text
- Vectors assumed declared `[N-1:0]`

## License

MIT
