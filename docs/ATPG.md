# Test pattern generation: `matpg`

`matpg` reads a scan-inserted gate netlist and produces manufacturing test
patterns with a fault coverage report: stuck-at faults, and transition
(delay) faults by launch-on-capture. It also grades existing patterns. It
needs no simulator front end and builds with the core (`cmake -B build`).

```sh
matpg scan.v                               # stuck-at patterns, coverage report
matpg scan.v -fault transition             # launch-on-capture transition patterns
matpg scan.v -fault all -stil p.stil -tb p_tb.v -patterns p.pat -faults p.flt
matpg scan.v -lib cells.lib                # cells from a Liberty library
matpg scan.v -faultsim p.pat               # grade the patterns in p.pat
matpg scan.v -min_coverage 98              # exit 1 under 98% test coverage (CI)
```

Where it sits in the flow: `mdft` checks the RTL for scan-readiness, `msynth`
maps it to gates, scan insertion stitches the chains, and `matpg` generates
and grades the patterns. The Verilog testbench it writes replays the patterns
on the netlist in `msim` or any other simulator.

## What it reads

- A structural Verilog netlist, flat or hierarchical, through the shared
  reader `src/gnetlist.hpp`. Gate primitives (`and`, `nand`, `or`, `nor`,
  `xor`, `xnor`, `buf`, `not`) are read as gates.
- Cell functions from Liberty (`-lib`, repeatable) through `src/liberty.hpp`:
  the `function` of each output, and for a flop `next_state`, `clocked_on`,
  `clear`, `preset` and the scan pins (`signal_type` in `test_cell`, or pins
  named SI and SE). Any combinational cell works, with any number of outputs.
  A cell matpg cannot model (a latch, a three-state output, a cell with no
  function) is kept as a black box whose outputs are unknown.
- Without `-lib`, the generic cells `msynth` writes are known by name: INV,
  BUF, AND2-4, OR2-4, NAND2-4, NOR2-4, XOR2, XNOR2, MUX2, AOI21, OAI21, AOI22,
  OAI22, TIE0, TIE1, and the flops DFF, DFFR, DFFS, DFFRS with their scan
  versions SDFF, SDFFR, SDFFS, SDFFRS.

## Scan structure

matpg finds the chains the way a tester sees them, from the ports: with scan
enable active, each scan flop takes its value from a scan-in port or from the
flop before it. Two styles are traced: a library scan flop, and a plain flop
with a 2:1 mux on D that scan enable selects. Buffers and inverters on the
path are followed, and an inversion (a chain through QN) is carried into the
patterns. Scan enable is found from the scan cells, or by name (`scan_en`,
`test_se`, ...), or given with `-se PORT` (`-se !PORT` when active low).

A flop is left out of scan, and read as unknown, when the tester cannot
reach its clock, cannot hold its asynchronous reset, when it is a
falling-edge flop, or when it is on no chain from a port to a port. `-drc`
lists each one with the reason. During capture the tester holds scan enable
off, the clocks off and the resets inactive; faults those holds make
untestable are reported as ATPG untestable, not as lost patterns.

## Faults and classes

There are two faults on every cell pin and every port. A transition fault is
slow-to-rise (`str`) or slow-to-fall (`stf`); a stuck-at fault is `sa0` or
`sa1`. Equivalent faults are collapsed for the run and expanded again in the
report, so the numbers are for the full list.

| Class | Meaning |
|---|---|
| `DS` | Detected by simulation of a generated pattern |
| `DI` | Detected by implication: on the scan path or a clock, which the chain test exercises |
| `UU` | Undetectable, unused: nothing observes the logic |
| `UR` | Undetectable, redundant: proven that no input can show it |
| `AU` | ATPG untestable: testable only without the tester's holds, or behind unscanned state |
| `ND` | Not detected: the search hit the backtrack limit (`-abort`) |

Test coverage is detected / (total - undetectable); fault coverage is
detected / total. `-faults FILE` writes one line per fault:

```
sa0  DS  U17/A
sa1  UR  u_and/B
str  AU  rst_n
```

## How it works

1. The scan flops are cut open: their outputs become inputs the tester
   loads, their captured values become outputs it reads. For transition
   faults the logic is unrolled over two clock cycles: the first launches
   the transition, the second captures it, and the inputs are held.
2. Random patterns are fault-simulated while they still pay.
3. PODEM (with SCOAP-guided backtrace and an X-path check) targets each
   remaining fault. The unassigned bits of a test are filled at random 64
   ways, all 64 are fault-simulated against every remaining fault at once,
   and the fill that detects most is kept. Faults the search gave up on get
   a second pass at ten times the limit.
4. Reverse-order fault simulation drops the patterns that no longer detect
   anything first.

The fault simulator is parallel-pattern single-fault-propagation, 64
patterns to the machine word, three-valued. It is a header other tools use:
`src/faultsim.hpp` (`atpg::Circuit`, `atpg::Fault`, `atpg::Sim`), with
`src/atpg_model.hpp` to build the model and the fault list from a netlist,
and `src/atpg.hpp` for PODEM and the flow.

Every test PODEM returns is checked by the fault simulator before it is
kept; a disagreement is reported and the pattern dropped. `tests/test_faultsim.cpp`
checks both against brute force on random circuits, and the `matpg_sim` test
replays generated patterns in `msim` on the good netlist (they pass) and on
a netlist with a detected fault wired in (they fail).

## Outputs

- `-stil FILE`: IEEE 1450 STIL with the signals, the scan chains
  (`ScanStructures`), a `load_unload` procedure and a `capture` procedure.
  A pattern's response is unloaded while the next pattern is loaded.
- `-tb FILE`: a self-checking Verilog testbench. One cycle drives the
  inputs, compares the outputs, then pulses the clocks. It starts with a
  chain test (0011 through every chain) and prints `ATPG PASS` or the
  mismatching cycles.
  Run with `+faillog` it also writes `<top>.faillog` (define `MATPG_FAILLOG`
  as a quoted string for another name): `top <name>`, then one line per
  wrong bit. `<pattern> <chain> <cell>` is a scan cell that captured the
  wrong value (cell 0 nearest scan-in) and `<pattern> po <port>` a port
  that was wrong at capture; patterns and chains are numbered as in
  `-cubes`. This is what a diagnosis tool reads.
- `-patterns FILE`: the patterns as text, one line each (input bits, then
  scan cell bits in chain order); `-faultsim` reads it back.
- `-cubes FILE`: every test before the random fill, for test compression
  (an EDT encoder needs to know which bits it is free to choose). A line is
  `<pattern> <chain> <bits>`: the pattern's number from 0 in pattern file
  order, the chain's number from 0 in the order matpg lists the chains, and
  the value each cell of that chain is loaded with, the cell nearest scan-in
  first, `x` for a bit ATPG did not need. `<pattern> pi <bits>` gives the
  port inputs the same way. A random pattern has no free bits. A header
  line `chains <port> <port> ...` names the scan-in port of chain 0, 1, ...
- `-json FILE`: the summary, for dashboards and CI.

## Threads

matpg uses every logical core by default; `-threads N` sets the count and
`-threads 1` turns threading off. Fault simulation is split over the faults,
and PODEM works on a fixed-size batch of target faults at a time, so the
patterns are the same for any thread count.

## Numbers

PicoRV32 (public RISC-V core) synthesized to the generic cells, 6,729 gates
and 1,601 flops on 8 scan chains, 67,064 faults, on a 4-core 2.3 GHz Xeon:

| Run | Test coverage | Patterns | 1 thread | 4 threads |
|---|---|---|---|---|
| stuck-at | 99.73% | 637 | 1.2 s | 0.6 s |
| transition (launch-on-capture) | 93.37% | 1,196 | 19.4 s | 8.1 s |

## Limits

- Full scan with one capture clock pulse on all clocks (two for transition).
  No launch-on-shift, no multiple capture cycles, no clock-domain masking:
  paths between clock domains are captured as if the clocks were aligned.
- Falling-edge flops and latches are left out of scan (no lockup latches).
- No path-delay, bridging or cell-aware fault models, and no test
  compression in this tool.
- Proving a fault redundant is PODEM's weak side: hard cases stop at the
  backtrack limit and are reported `ND`. Raise `-abort` to spend more.
