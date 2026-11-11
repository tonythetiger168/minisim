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
   Dead code goes (also a variable that only feeds itself), and every operator is narrowed to the bits its readers
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
| functions (inlined), `static` locals and globals (state kept between calls), `const` globals, `enum`, `typedef` | recursion |
| with `-clang`: classes, member functions, constructors, operators, templates, namespaces (see C++ through Clang) | inheritance, virtual dispatch through a base, lambdas |
| `hls::stream<T>&` parameters with `read()` and `write(x)` (see Streams) | `empty()`, `full()`, non-blocking access, local streams |
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

## C++ through Clang

`mhls -clang` has the installed `clang` read the source (an external
program; nothing of LLVM is built into mhls) and takes its syntax tree. In
that tree every template is instantiated, every overload is resolved and
every implicit conversion is spelled out, so C++ reduces to what the rest of
mhls already synthesizes: plain functions over structs.

```sh
mhls fir.cpp -clang -top fir4 -o fir4.sv -tb fir4_tb.sv -cref fir4_ref.cpp
mhls fir.cpp -clang -emit-c fir.c      # see the reduction
```

| C++ | becomes |
|---|---|
| a class or struct with member functions | a struct, and one function per member function that is used, taking the object as `self` |
| a class or function template | one struct or function per instantiation (`Fir<4>` is `Fir_4`) |
| a constructor, member initializers | a function run where the object is declared; an array of objects runs it per element |
| `a + b`, `v[i]`, `f(x)` on objects | calls to `operator+`, `operator[]`, `operator()` |
| a temporary object, an object returned by a call, `c ? a : b` on objects | a variable declared before the statement |
| a member function that returns `*this` (`operator+=`, `++`, a setter that chains) | a void function; the call's value is the object |
| default arguments, overloads, `static` members, `enum class`, namespaces, `constexpr`, `auto` | the value, the function or the constant they stand for |
| `const T&` to a number | the number |

Only what the design uses is written. That covers the classes, templates and
inline functions of the headers it includes, and of the standard library
where they are this plain (`std::min`, `std::max`). `-I` and `-D` go to
clang, and `__MHLS__` is defined while it reads. With `-top`, only the
functions named and what they use are looked at, so the file may hold more
than mhls can synthesize. Errors and warnings from the rest of mhls name the
lines of the C++ file. `clang` is found on the `PATH`; set
`MHLS_CLANG` to use another one.

Refused, with the line: base classes, pointer or reference members and
variables, functions that return a non-constant reference to anything but
their own object, floating point,
lambdas, range-based `for`, `new`, exceptions, a call to a function clang
shows no body for. The C limits apply as well (no recursion, fixed-size
arrays). `ap_int<N>` and `ap_uint<N>` are not available with `-clang` yet;
use `intN_t` types or bit-fields. `#pragma hls` lines are read from the file
given to mhls, not from its headers.

Without `-clang` nothing changes: the built-in C front end reads the file
and clang is not needed.

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

`mhls` picks the fastest rate the loop allows. Most loops can start an
iteration every cycle. One that reads and writes the same memory, follows a
pointer it has just read, or ends on a value read from memory cannot, but
it can start one every second (or third) cycle, which still beats running
the iterations one after the other when the body is longer than that:
`n` iterations then take `(n - 1) * K + L` cycles. In
`tests/hls/looppipe2.c` a histogram (`h[b] = h[b] + 1`) runs at one
iteration every 2 cycles instead of 3.

The pragma insists: when no rate is faster than the unpipelined loop,
`mhls` stops and says which rule fails. `-pipeline-loops` tries every loop
instead, leaves the others as they are, and the summary lists what happened
to each. The rules, for one iteration every `K` cycles:

- the body is straight-line code after if-conversion and unrolling of inner
  loops (no `break`, `continue`, `return`, or inner loop that stays a loop);
- the accesses to one memory share its port: they are in different stages,
  all within `K` stages of each other, so at one per cycle `b[i] = a[i] + 1`
  qualifies and `a[i] = a[i] + 1` needs every second cycle;
- a value carried from one iteration to the next (an accumulator, the loop
  counter) is updated within `K` clock periods of being read, and not
  through a multi-cycle operator;
- the loop condition is known within the first `K` stages.

A pipelined loop is one FSM state; the summary and the generated testbench
count its cycles.

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

### Streams

A parameter of type `hls::stream<T>&` (T a scalar type) is a valid/ready
port instead of a memory. Each `read()` takes one element, each `write(x)`
sends one, in program order, and the state machine waits in that state
while the source has nothing to offer or the sink is not ready. A stream is
read or written, not both.

| Port | |
|---|---|
| `<s>_tdata`, `<s>_tvalid` | the element and its valid; inputs for a stream that is read, outputs for one that is written |
| `<s>_tready` | the other direction: mhls is ready to take, or the sink is ready |

```c
#include "mhls_stream.h"
void scale(hls::stream<uint8_t>& in, hls::stream<uint16_t>& out, uint8_t gain, uint8_t n) {
  for (int i = 0; i < n; i++) out.write(in.read() * gain);
}
```

`include/mhls_stream.h` defines `hls::stream` for compiling the same source
on the host; `mhls` ignores the include. A transfer takes a state of its
own and a read delivers its data in the next one, so the loop above moves
one element every two cycles. Pipelined (`#pragma hls pipeline` or
`-pipeline-loops`) it moves one element per cycle: the stages of a
pipelined loop all stop in a cycle where one of them cannot make its
transfer. Two accesses to the same stream in one iteration follow the
memory rule (one iteration every 2 cycles). The pipeline architecture
(`-pipeline`) does not take streams. `-tb` adds a source and a sink that hesitate on every other
call and checks the elements, their order and their number; `-cref` checks
the same on the host.

`-handshake` adds a second module `<top>_hs` around the block with
`in_valid`/`in_ready` for the scalar inputs and `out_valid`/`out_ready` for
the results, in place of the `ap_` control ports. A transfer happens in a
cycle where valid and ready are both high. Around an FSM the result waits
with `out_valid` high until it is taken, and the next call is accepted
after that; around a pipeline a result that is not taken stalls every
stage, and `in_ready` goes low. Memory ports pass through unchanged. `-tb`
then drives the wrapper, with a sink that refuses now and then.

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
| `-handshake` | also write `<top>_hs`, the block behind ready/valid ports |
| `-max-reg-array <n>` | largest local array kept in registers (default 64) |
| `-delay-scale <x>` | scale the built-in operator delays |
| `-clang` | read the source with clang: C++ classes and templates (see C++ through Clang) |
| `-emit-c <file>` | with `-clang`: write the plain C the source reduces to |
| `-I<dir>`, `-D<name>[=<v>]` | include path (clang only) and macro definition |
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

Dual-port memories, non-blocking stream access,
unions, SystemC modules (`SC_MODULE`, clocked threads), and
operator delays taken from a Liberty library.
