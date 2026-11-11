# FPGA synthesis: `mfpga`

`mfpga` maps a design to the LUTs and flip-flops of an FPGA family and writes
a netlist that open place-and-route tools read.

```sh
mfpga rtl/*.sv -top core -family ice40 -o core.json
nextpnr-ice40 --hx8k --package ct256 --json core.json --pcf core.pcf --asc core.asc

mfpga rtl/*.sv -top core -family ecp5 -o core.json      # nextpnr-ecp5 --json core.json
mfpga rtl/*.sv -top core -family xc7 -edif core.edf     # Vivado, or nextpnr-xilinx with -o
mfpga design.aig -family ice40 -o design.json           # an AIGER graph instead of RTL
```

It is the FPGA target of the synthesis line, not a second synthesizer: RTL
goes through the elaborator `msynth` uses (`src/aig_elab.hpp`) into the shared
and-inverter graph (`src/aig.hpp`), and `src/fpga.hpp` maps that graph. The
mapping algorithm is the cut mapper of the emulator compiler (`efc`
`cutmap.py`: cut enumeration, depth labels, area flow) in C++, with priority
cuts and an exact-area pass so it scales to large designs.

## Options

| Option | Meaning |
|---|---|
| `-family F` | `generic` (default), `ice40`, `ecp5`, `xc7` |
| `-lut K` | LUT inputs, 2 to 6. Default 4, `xc7` 6 |
| `-o FILE` | Yosys JSON netlist (`nextpnr --json`) |
| `-edif FILE` | EDIF 2 0 0 |
| `-verilog FILE` | structural Verilog of the same cells, for simulation |
| `-top NAME` | top module |
| `+define+M=V`, `-D`, `+incdir+DIR`, `-I`, `-G NAME=VALUE` | macros, include directories, top parameters |
| `-clock NAME` | AIGER input: name of the clock port added for the latches (default `clk`) |
| `-cuts N` | cuts kept per node, 2 to 16 (default 8). More cuts: slightly fewer LUTs, more time |
| `-threads N` | worker threads. Default: every logical core. The output does not depend on N |
| `-nocarry` | adders, subtractors and comparators as LUTs, not carry chains |
| `-nodsp` | multipliers as LUTs, not DSP blocks |
| `-dsp` | `ice40`: multipliers onto `SB_MAC16`. Off by default: only UltraPlus parts (UP5K) have them |
| `-dsp-min N` | fewest operand bits, both operands together, worth a DSP block (default 10) |
| `-noce` | clock enables and synchronous resets stay in the D logic |
| `-verify` | simulate the mapped netlist against the graph on random patterns |
| `-q` | no report |

## Cells written

| Family | LUT | Flip-flops | Other |
|---|---|---|---|
| `generic` | `$lut` (`WIDTH`, `LUT`) | `$_DFF_P_`, `$_DFF_PP0_`, `$_DFFE_PP_`, `$_DFFE_PP0P_`, `$_SDFF_PP0_`, `$_SDFFE_PP0P_`, ... (Yosys gate cells); power-up value as the net's `init` attribute | no carry cells |
| `ice40` | `SB_LUT4` | `SB_DFF` with `E` (enable), `R`/`S` (asynchronous) or `SR`/`SS` (synchronous) reset, and the `SB_DFFN*` falling-edge ones | `SB_CARRY`, one per bit, beside the `SB_LUT4` of the sum; `SB_MAC16` with `-dsp` |
| `ecp5` | `LUT4` | `TRELLIS_FF` (`REGSET`, `SRMODE=ASYNC` or `LSR_OVER_CE`, `CEMUX=CE` with an enable, `CLKMUX=INV` for the falling edge) | `CCU2C`, two bits each; `MULT18X18D` |
| `xc7` | `LUT1` to `LUT6` | `FDRE`, `FDSE`, `FDCE`, `FDPE` with `CE` (`INIT`, `IS_C_INVERTED`) | `CARRY4`, four bits each; `DSP48E1`; `IBUF` and `OBUF` on every port |

Inverters cost nothing: they are folded into LUT tables. An iCE40 flop always
powers up 0, so a flop that must power up 1 is mapped inverted (D, Q and reset
value inverted). In EDIF, constants come from `GND` (pin `G`) and `VCC` (pin
`P`) cells, primitives are referenced from the external library
`hdi_primitives`, and bit-vector parameters are written as `16'h6996`.

## How it maps

1. `aig::cleanup()` gives a hashed graph of two-input ANDs with inverted edges.
2. Every node gets its best K-input cuts, level by level. All nodes of one
   level are computed in parallel; a node only reads lower levels, so the
   result is the same for any thread count.
3. Pass 1 picks the cut with the smallest depth. The depth it finds is the
   minimum for the graph, and later passes keep it.
4. Area recovery: two passes by area flow, then two passes by exact local area
   (how many LUTs the cut really adds to the current cover), within each
   node's required time.
5. Truth tables are computed, inputs a function ignores are removed, and the
   cells of the family are written.

## Carry chains, clock enables and synchronous resets

The elaborator records every `+`, `-` and `<` of the RTL with its operand and
result bits (`aig::Design::ops`, shared with `msynth`). For a family with
carry cells, `mfpga` cuts each one of three bits or more out of the graph and
writes the family's chain instead; `-`, `<`, `<=`, `>` and `>=` are `a + ~b + 1`
and its carry out, signed compares flip the two sign bits. The LUT mapper sees
a chain as a box: its operands are roots like outputs, its sum bits arrive one
level after the latest operand. ECP5 and 7-series chains invert `b` inside for
a subtraction; iCE40's `SB_CARRY` cannot, so there `~b` costs a LUT per bit
unless the driving LUT absorbs it. A chain nothing reads, or one in a
combinational loop with its own operands, goes back to LUTs.

A register that holds its value (`if (en) q <= d`, also through nested
`if`/`else`) gets the condition on the flop's enable pin, and a reset shared
by three or more flops (`if (rst) q <= 0`) on its synchronous set/reset pin,
so neither costs a LUT input. Reset over enable and enable over reset are
both recognized; where the family's flop has the other priority (iCE40's
enable gates its reset) one shared gate fixes it.

A 64-bit registered adder with a compare, placed and routed by nextpnr:

| | LUTs | Fmax |
|---|---|---|
| iCE40 HX8K, carry chains | 129 (64 are the chain's sum LUTs) | 74 MHz |
| iCE40 HX8K, `-nocarry` | 430 | 112 MHz |
| iCE40 HX8K, Yosys `synth_ice40` | 310 logic cells with the flops | 80 MHz |
| ECP5-85F, carry chains | 1 + 66 `CCU2C` | 179 MHz |
| ECP5-85F, `-nocarry` | 430 | 125 MHz |

A chain is always the smaller choice. On iCE40 a long ripple is slower than
the LUT tree, as it is for Yosys; `-nocarry` trades area for that speed.

## DSP blocks

Every `*` is in the same operator table. The elaborator extends both operands
to the width of the result, so `mfpga` first finds the width each really has
(leading zeros, or copies of the sign), and a multiplier that fits one block
becomes that block, unregistered: `MULT18X18D` (18 x 18, signed or unsigned
per operand), `DSP48E1` (25 x 18 signed, so 24 x 17 unsigned), `SB_MAC16`
(16 x 16). Result bits above the product are its sign or 0; unused product
bits are left open. `tests/fpga/mul.sv` (8x8, signed 9x6, 11x8 truncated, a
multiply-accumulate and a 28x28) takes 4 blocks and 1,758 LUT4, all of them
the 28x28; without blocks it is 2,647.

## Speed

A 128x128 multiplier (166,252 AND nodes) on a 4-core machine, LUT6: 32,344
LUTs, depth 18. Mapping takes 0.95 s on one thread, 0.54 s on two and 0.37 s
on four, with byte-identical output. On a 32x32 multiplier the LUT count is
2,854 at LUT4 (ABC through Yosys `abc -lut 4`: 2,841) and 2,019 at LUT6
(ABC: 2,113).

## Checks

`tests/fpga/run_test.py` maps the designs `msynth` is tested with, and a
datapath with synchronous reset, enable, add/subtract and signed and unsigned
compares (`tests/fpga/dp.sv`), and multipliers (`mul.sv`, `mulp.sv`), for
every family, and for each one:

- `-verify` simulates the netlist against the graph on random patterns;
- one and four threads must write the same bytes;
- the design's testbench is run with `msim` on the RTL and on the mapped
  Verilog netlist (`-verilog`, with the cell models in `tests/fpga/`), and
  both must print the same lines;
- with Yosys installed, the JSON netlist on the vendor's own simulation
  models is proven equal to the RTL by a bounded SAT check that shares no code
  with `mfpga` (the counter, the datapath and the small multipliers; Yosys
  0.33 cannot read the other designs). This is what checks the carry cells and flop pins against
  the vendors' models;
- with nextpnr installed, the iCE40 and ECP5 netlists are placed and routed.

CI installs Yosys and nextpnr.

## Limits

- A multiplier wider than one DSP block stays LUTs: it is not split over
  several blocks yet. The blocks' input, pipeline and output registers and
  their adders (multiply-accumulate) are not used. `MULT18X18D` is a black
  box in Yosys 0.33, so its proof runs on the model in
  `tests/fpga/cells_ecp5_dsp.v`; `DSP48E1` is proven on Yosys' own model, and
  `SB_MAC16` was (by hand, with place and route on a UP5K), since the CI
  tests place iCE40 netlists on an HX8K, which has no DSPs.
- Memories become flops, not block RAM: that needs the memory table the
  elaborator will record.
- Chains are not split or sped up (carry-select, carry-lookahead) by timing,
  and an adder is not merged with a mux or an AND in front of it into the
  chain's LUTs.
- AIGER input has no word operators, so it maps to LUTs and flops only.
- The `xc7` netlist is checked by simulation and by Yosys equivalence on its
  JSON form. The EDIF itself has not been through a Vivado run.
- A flop's power-up value is known for AIGER input only; the elaborator does
  not record `initial` values.
