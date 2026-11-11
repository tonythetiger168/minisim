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
| `-verify` | simulate the mapped netlist against the graph on random patterns |
| `-q` | no report |

## Cells written

| Family | LUT | Flip-flops | Other |
|---|---|---|---|
| `generic` | `$lut` (`WIDTH`, `LUT`) | `$_DFF_P_`, `$_DFF_N_`, `$_DFF_PP0_`, ... (Yosys gate cells); power-up value as the net's `init` attribute | |
| `ice40` | `SB_LUT4` | `SB_DFF`, `SB_DFFR`, `SB_DFFS` and the `SB_DFFN*` falling-edge ones | |
| `ecp5` | `LUT4` | `TRELLIS_FF` (`REGSET`, `SRMODE=ASYNC` for an asynchronous reset, `CLKMUX=INV` for the falling edge) | |
| `xc7` | `LUT1` to `LUT6` | `FDRE`, `FDCE`, `FDPE` (`INIT`, `IS_C_INVERTED`) | `IBUF` and `OBUF` on every port |

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

## Speed

A 128x128 multiplier (166,252 AND nodes) on a 4-core machine, LUT6: 32,344
LUTs, depth 18. Mapping takes 0.95 s on one thread, 0.54 s on two and 0.37 s
on four, with byte-identical output. On a 32x32 multiplier the LUT count is
2,854 at LUT4 (ABC through Yosys `abc -lut 4`: 2,841) and 2,019 at LUT6
(ABC: 2,113).

## Checks

`tests/fpga/run_test.py` maps the designs `msynth` is tested with for every
family, and for each one:

- `-verify` simulates the netlist against the graph on random patterns;
- one and four threads must write the same bytes;
- the design's testbench is run with `msim` on the RTL and on the mapped
  Verilog netlist (`-verilog`, with the cell models in `tests/fpga/`), and
  both must print the same lines;
- with Yosys installed, the JSON netlist on the vendor's own simulation
  models is proven equal to the RTL by a bounded SAT check that shares no code
  with `mfpga` (the counter; Yosys 0.33 cannot read the other designs);
- with nextpnr installed, the iCE40 and ECP5 netlists are placed and routed.

CI installs Yosys and nextpnr.

## Limits

- LUTs and flip-flops only. Adders become LUTs, not carry chains; multipliers
  become LUTs, not DSP blocks; memories become flops, not block RAM. These need
  the word-level operator table the elaborator will record, and come next.
- No clock enable or synchronous reset pins yet: both are in the D logic.
- The `xc7` netlist is checked by simulation and by Yosys equivalence on its
  JSON form. The EDIF itself has not been through a Vivado run.
- A flop's power-up value is known for AIGER input only; the elaborator does
  not record `initial` values.
