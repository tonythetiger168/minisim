# minisim

A small, readable HDL simulator kernel in C++20, built as a learning project to
understand how tools like VCS work: parse with [slang](https://github.com/MikePopoloski/slang),
lower to a flat process IR, run on an event-driven kernel, dump waveforms for GTKWave.

> **Status: experimental.** It handles a subset of Verilog/SystemVerilog. It is not a VCS replacement.

## Features

- **4-state logic** (0/1/X/Z) up to 64 bits, with Verilog X-propagation rules
- **Event-driven kernel**: time wheel, active / inactive (`#0`) / NBA regions
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
| `src/lower.hpp` | slang AST to `Prog` lowering (**not yet compiled against slang**) |
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
cmake -B build -DMINISIM_WITH_SLANG=ON
cmake --build build -j
./build/slang_dump examples/counter.sv
```

## Known limits

- Signals up to 64 bits; no signed arithmetic, division, or strengths
- `lower.hpp` is written against slang's API from memory and needs verification
- Not yet lowered: compound assigns (`+=`), `break`/`continue`, net initializers, tasks/functions, hierarchy, `$display`
- Vectors assumed declared `[N-1:0]`

## License

MIT
