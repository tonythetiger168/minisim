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

### Core verification

Without CMake, on a Linux/WSL environment with Python 3 and a C++20 compiler:

```sh
python3 tests/run_core_tests.py
# Independent comparisons require Icarus Verilog (iverilog + vvp):
python3 tests/compare_mux.py
python3 tests/compare_counter.py
```

The core runner checks expression tests plus scheduler scenarios (NBA/`#0`,
partial writes, multi-event waits, four-state edges, immediate finish and initial
VCD values). It uses temporary build/output directories. The mux comparison
checks 1,024 equal-width two-bit cases against Icarus; the counter comparison
checks the hand-built demo against `examples/counter.sv`. These do not validate
the slang lowerer or establish full SystemVerilog conformance.

See [the development plan](docs/DEVELOPMENT_PLAN.md) for scope and remaining work.

### Python UVM reference test

With the packages in `requirements-verification.txt` and Icarus installed:

```sh
python3 tests/run_pyuvm_counter.py --check-mutant
```

This runs `examples/counter.sv` through cocotb and a pyuvm scoreboard for 96
cycles, including reset, all 16 values and wraparound. The optional mutant check
changes the increment to two in a temporary copy and requires a scoreboard
mismatch in the simulator's XML result. Missing/invalid results are errors.
Minisim itself does not yet expose cocotb's simulator interface; this test is
an independent RTL reference for future integration.

### Coverage-driven development

```sh
python3 tests/run_native_coverage.py
python3 tests/run_verilator_coverage.py
# With the verification Python environment:
python3 tests/run_pyuvm_counter.py --sim verilator --coverage-dir build/coverage/verilator
```

Minisim collects mapped IR line bins and directional bit toggles, with explicit
FSM, functional-bin and clocked-property APIs. The native counter selects stimulus
from uncovered bins and emits JSON. The separate Verilator fixture exercises RTL
line/toggle coverage and real SVA assertions/cover properties, including a mutant
and a no-start test to reject vacuous closure. See [coverage scope and results](docs/COVERAGE.md).
Native Minisim does not yet parse general RTL SVA or automatically extract RTL FSMs.

### Experimental Python RTL frontend

With pyslang installed, a strict AST-based frontend now lowers a small RTL subset
to source-mapped Minisim IR and generated C++:

```sh
python3 tools/pyslang_lower.py examples/counter.sv --output build/frontend-probe
python3 -m unittest discover -s tests -p test_frontend.py -v
python3 tests/run_pyslang_frontend.py
```

The differential runner compiles and executes generated IR for three counter RTL
variants and checks 288 samples against Icarus, including X/Z reset inputs. This
is separate from the unverified C++ `lower.hpp`. Unsupported constructs, including
SVA, are rejected. See [frontend scope and evidence](docs/PYSLANG_FRONTEND.md).

The frontend can also run a coverage-driven counter verification model:

```sh
python3 tests/run_coverage_driven_frontend.py
```

This generates stimulus from live Minisim coverage holes, then replays the exact
recorded stimulus on Icarus and Verilator. Clean 4/8-bit designs close; a +2 mutant
and a stalled reset-only run must fail verification for the intended reason.
See [adaptive frontend evidence and limitations](docs/ADAPTIVE_FRONTEND.md).

### Restricted synthesized VHDL + SystemVerilog path

`python tests/run_mixed_language.py` checks a GHDL-synthesized VHDL counter plus
an SV arithmetic wrapper, flattened by Yosys and executed on Minisim through a
strict netlist frontend. GHDL native simulation and Icarus provide independent
reference checks. This requires GHDL/Yosys and is a translated synthesizable
subset, not a native nine-state VHDL scheduler or arbitrary mixed-language TB.
See [scope and 80-sample evidence](docs/VHDL_MIXED_LANGUAGE.md).

With the slang front end (downloads and builds slang, takes a while):

```sh
cmake -B build -DMINISIM_WITH_SLANG=ON
cmake --build build -j
./build/slang_dump examples/counter.sv
```

## Known limits

An experimental flat-counter VPI host now executes real cocotb and pyuvm tests, including scoreboard rejection of a mutant. The ctypes bridge also has 288 samples matching Icarus. See [PYTHON_BRIDGE.md](docs/PYTHON_BRIDGE.md). This is **not a general cocotb backend for arbitrary RTL**. Dynamis TX transaction integration is documented in [DYNAMIS_INTEGRATION.md](docs/DYNAMIS_INTEGRATION.md).

Kernel execution budgets, coroutine error propagation, and checked delay scheduling are documented in [KERNEL_LIMITS.md](docs/KERNEL_LIMITS.md). The defaults bound yielding processes and IR loops; native C++ loops that never yield still require an external timeout.

- Signals up to 64 bits; no signed arithmetic, division, or strengths
- `lower.hpp` is written against slang's API from memory and needs verification
- Not yet lowered: compound assigns (`+=`), `break`/`continue`, net initializers, tasks/functions, hierarchy, `$display`
- Vectors assumed declared `[N-1:0]`

## License

MIT
