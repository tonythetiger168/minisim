# High-level synthesis: `mhls`

`mhls` turns an untimed C/C++ function into synthesizable SystemVerilog,
the job Cadence Stratus and Vitis HLS do. It has no dependencies (its own C
front end), and the RTL it writes is checked two ways: against the host C++
compiler, and in `msim`.

```sh
mhls fir.c -top fir -o fir.sv                      # FSM with datapath
mhls fir.c -top fir -pipeline -clock 5             # pipeline, one input per cycle
mhls fir.c -top fir -tb fir_tb.sv -vectors 100     # self-checking testbench
msim fir.sv fir_tb.sv -top fir_tb                  # PASS: fir 100 calls, latency 2..2 cycles
mhls fir.c -top fir -cref fir_ref.cpp              # the same calls for the host compiler
mhls fir.c -top fir -explore 2,5,10,20             # latency and area against the clock period
```

## What it does

1. **Front end** (`src/hls_front.hpp`): lexer, a small preprocessor
   (`#define` without arguments, `#if/#ifdef/#else/#endif`, `-D`), parser.
2. **Lowering** (`src/hls_ir.hpp`) to a control/data-flow graph: every call
   is inlined; `if`/`?:`/`&&`/`||` without loops or jumps become multiplexers
   (if-conversion); loops with a constant trip count unroll on request;
   small arrays become registers, large ones RAMs, `const` ones ROMs.
   Dead code goes, and every operator is narrowed to the bits its readers
   look at, so `uint8_t c = a + b` is an 8-bit adder although C adds as `int`.
   Chains such as `a+b+c+d` are rebalanced into trees.
3. **Scheduling** (`src/mhls.cpp`): each block is cut into clock cycles.
   Operators chain in one cycle while the path fits in `-clock`; an operator
   slower than the clock takes several cycles; `-mul`/`-div` cap how many
   multipliers and dividers work in one cycle; an array has one port, so one
   access per cycle.
4. **Binding**: values that outlive their cycle share registers (left-edge
   algorithm); multipliers and dividers that are never busy together share
   one unit with input multiplexers, as long as that closes no combinational
   loop.
5. **Emission**: an FSM with datapath, or with `-pipeline` a feed-forward
   pipeline that takes a new input every cycle.

## The C subset

| Supported | Not supported |
|---|---|
| `bool`, `char` .. `long long`, `intN_t`/`uintN_t`, `ap_int<N>`/`ap_uint<N>`, `sc_int<N>`/`sc_uint<N>`, N up to 64 | floating point, wider than 64 bits |
| every C operator, casts, `?:`, short-circuit `&&` `\|\|`, `++`/`--`, compound assignment | `sizeof`, string literals |
| `if`, `for`, `while`, `do`, `break`, `continue`, `return` anywhere | `switch`, `goto` |
| fixed-size arrays of any dimension, initializer lists | pointers other than scalar output parameters, dynamic memory |
| functions (inlined), `static` locals and globals (state kept between calls), `const` globals, `enum`, `typedef` | recursion, structs, classes, templates |

`ap_`/`sc_` operands are widened to 64 bits in arithmetic, so a product of
two 20-bit values keeps its 40 bits; the result is cut to the declared width
on assignment. Division by zero gives all ones (remainder: the dividend),
and a shift by the width or more gives 0, in the RTL and in the reference.

Pragmas: `#pragma hls unroll` (before a loop or first in its body),
`#pragma hls pipeline` (in a function), `#pragma hls top`.

## Interfaces

FSM architecture:

| Port | |
|---|---|
| `ap_clk`, `ap_rst` | clock, synchronous active-high reset |
| `ap_start` | pulse to start a call; scalar inputs are sampled in that cycle |
| `ap_done` | one-cycle pulse; outputs are valid then and hold until the next call |
| `ap_idle` | high while waiting for `ap_start` |
| `<arg>` | scalar parameter: input. Pointer/reference parameter: output, or `<arg>_i` and `<arg>_o` when the function also reads it |
| `ap_return` | the return value |
| `<arr>_addr`, `_ce`, `_we`, `_wdata`, `_rdata` | array parameter: a single-port memory with synchronous read (data one cycle after `_ce`) |

Pipeline architecture: `ap_vld_in` marks an input, `ap_vld_out` its result
`L` cycles later (the summary prints `L`), and `ap_ce` low stalls every
stage. A pipeline needs a function that is straight-line after unrolling
(no memories, no data-dependent loops), and a `static` must be read and
updated in the same stage; `mhls` says which rule fails and why.

## Checking the result

`-tb` writes a testbench with random calls and the expected outputs, array
contents and cycle counts from mhls's own reference run of the function.
`-cref` writes a C++ program that includes the source and checks the same
expected values with the host compiler, which checks mhls's reading of C
against a real compiler. `tests/mhls.cmake` does both for every design in
`tests/hls`, and `tests/hls/fuzz.py` does it for random programs:

```sh
tests/hls/fuzz.py --mhls build/mhls --msim build/msim --out /tmp/hlsfuzz --seeds 0:1000
```

## Options

| Option | |
|---|---|
| `-top <f>[,<g>]` | functions to synthesize (default: `#pragma hls top`, else the last function) |
| `-o <file\|dir>` | output file; a directory when there are several tops |
| `-clock <ns>` | clock period used for scheduling (default 10) |
| `-pipeline` | pipeline architecture |
| `-unroll-all`, `-unroll-max <n>` | unroll every constant-trip loop; most iterations (default 4096) |
| `-mul <n>`, `-div <n>` | multipliers / dividers working in one cycle |
| `-no-share` | no register or unit sharing |
| `-max-reg-array <n>` | largest local array kept in registers (default 64) |
| `-delay-scale <x>` | scale the built-in operator delays |
| `-tb <file>`, `-cref <file>`, `-vectors <n>`, `-seed <n>` | verification collateral |
| `-explore <ns,...>` | schedule at each clock period, print states, latency, register bits, area |
| `-report <file>` | the summary as JSON |
| `-threads <n>\|all` | worker threads (default: one per physical core, at most 8); the output is the same for every count |
| `-dump-ir` | print the scheduled graph |

The operator delays are a generic 28 nm-class model and only decide where
cycle boundaries fall; run the RTL through synthesis and timing for real
numbers. Operators that take several cycles are counted in the summary and
need multicycle constraints.

## Not yet

Loop pipelining, dual-port memories, AXI-Stream/handshake interfaces,
`switch`, structs, SystemC modules (`SC_MODULE`, clocked threads), and
operator delays taken from a Liberty library.
