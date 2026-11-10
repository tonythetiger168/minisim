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
   is inlined; `if`/`switch`/`?:`/`&&`/`||` without loops become multiplexers
   (if-conversion), including early returns such as `if (x < lo) return lo;`; loops with a constant trip count unroll on request;
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
| `if`, `switch` (with fall-through), `for`, `while`, `do`, `break`, `continue`, `return` anywhere | `goto` |
| fixed-size arrays of any dimension, initializer lists | pointers other than scalar output parameters, dynamic memory |
| functions (inlined), `static` locals and globals (state kept between calls), `const` globals, `enum`, `typedef` | recursion, classes, templates |
| structs: nested, in arrays, with array members and bit-fields; copied, passed by value, pointer or reference, and returned | unions, pointer members, member functions, structs defined inside a function |

`ap_`/`sc_` operands are widened to 64 bits in arithmetic, so a product of
two 20-bit values keeps its 40 bits; the result is cut to the declared width
on assignment. Division by zero gives all ones (remainder: the dividend),
and a shift by the width or more gives 0, in the RTL and in the reference.

A struct is taken apart: each member is its own register, memory or port,
named `<variable>__<member>` (`s.px.r` is `s__px__r`), so only the members
that are used cost anything. An array of structs is one memory per member.
A returned struct comes out as `ap_return__<member>`. A struct with an
array member is passed by pointer or reference, not by value.

Pragmas: `#pragma hls unroll` (before a loop or first in its body),
`#pragma hls pipeline` (in a function: the whole function is a pipeline;
first in a loop body: that loop is pipelined), `#pragma hls top`.

## Pipelined loops

A loop normally runs one iteration at a time, each taking as many states as
its body needs: two for a body that reads a memory (address, then data),
more when the logic does not fit in one clock period. A pipelined loop
starts a new iteration every cycle while the earlier ones are still in
their later stages, so `n` iterations of an `L`-stage body take `n + L - 1`
cycles instead of `n * L`.

```c
for (int i = 0; i < n; i++) {
#pragma hls pipeline
  int32_t y = (in[i] * gain) >> 6;
  out[i] = y;
  if (y > peak) peak = y;
}
```

In `tests/hls/looppipe.c` this halves the latency of three loops (for
example 129 to 67 cycles for a 63-element sum) at the same area.

The pragma insists: when the loop cannot run one iteration per cycle,
`mhls` stops and says which rule fails. `-pipeline-loops` tries every loop
instead, leaves the others as they are, and the summary lists what happened
to each. The rules:

- the body is straight-line code after if-conversion and unrolling of inner
  loops (no `break`, `continue`, `return`, or inner loop that stays a loop);
- each memory is accessed at most once per iteration (one port), so
  `a[i] = a[i] + 1` does not qualify but `b[i] = a[i] + 1` does;
- a value carried from one iteration to the next (an accumulator, the loop
  counter) is updated within one clock period of being read, so not
  through a memory read (`p = next[p]`) or a multi-cycle operator;
- the loop condition is known in the first stage, so it does not depend on
  memory data (`while (s[n] != 0)`).

A pipelined loop is one FSM state; the summary and the generated testbench
count its `n + L - 1` cycles.

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
| `<arg>__<member>` | one port (or memory interface) per member of a struct parameter, following the rules above |
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
| `-pipeline-loops` | pipeline every loop that allows it (see Pipelined loops) |
| `-mul <n>`, `-div <n>` | multipliers / dividers working in one cycle |
| `-no-share` | no register or unit sharing |
| `-max-reg-array <n>` | largest local array kept in registers (default 64) |
| `-delay-scale <x>` | scale the built-in operator delays |
| `-tb <file>`, `-cref <file>`, `-vectors <n>`, `-seed <n>` | verification collateral |
| `-explore <ns,...>` | schedule at each clock period, print states, latency, register bits, area |
| `-report <file>` | the summary as JSON |
| `-threads <n>\|all` | worker threads (default: every hardware thread, hyper-threads included); the output is the same for every count |
| `-dump-ir` | print the scheduled graph |

The operator delays are a generic 28 nm-class model and only decide where
cycle boundaries fall; run the RTL through synthesis and timing for real
numbers. Operators that take several cycles are counted in the summary and
need multicycle constraints.

## Not yet

Dual-port memories, pipelined loops that start an iteration every second
cycle, AXI-Stream/handshake interfaces,
unions, SystemC modules (`SC_MODULE`, clocked threads), and
operator delays taken from a Liberty library.
