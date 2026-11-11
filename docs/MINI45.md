# mini45: the generic cell library (`lib/mini45`)

mini45 is a made-up 45 nm-class standard cell library for testing the tool
line end to end: synthesis maps to it, timing and power read it, place and
route uses its LEF, and gate-level netlists simulate against its Verilog
models. It was written from scratch. Nothing in it comes from a foundry or
from another library, and it must not be used for a real chip.

| File | What |
| --- | --- |
| `mini45_tt.lib` | Liberty, typical corner: 1.10 V, 25 C |
| `mini45_ss.lib` | slow corner: 0.95 V, 125 C |
| `mini45_ff.lib` | fast corner: 1.25 V, -40 C |
| `mini45.v` | Verilog models: functional, with specify paths and timing checks for SDF |
| `mini45_tech.lef` | technology LEF: site, six metal layers, vias |
| `mini45.lef` | cell LEF: one macro per cell with pin shapes |
| `mini45_tb.sv` | self-checking bench of the Verilog models |
| `gen_mini45.py` | the generator: every file above comes from it |

```sh
msim netlist.v lib/mini45/mini45.v -top top       # gate-level simulation
python3 lib/mini45/gen_mini45.py                  # regenerate after editing the generator
ctest -R mini45                                   # files up to date, models correct
```

## Cells

Names are `<function>_X<drive>`. Outputs are `Y` (or `Q`, `QN`, `S`, `CO`,
`GCK`); inputs are `A B C D`, or `A1 A2 B1 B2` where the grouping matters.

| Cells | Function |
| --- | --- |
| `INV_X1/2/4/8`, `BUF_X1/2/4/8` | inverter, buffer |
| `CLKBUF_X1/2/4`, `CLKINV_X1/2/4` | clock tree buffer and inverter |
| `NAND2_X1/2/4`, `NAND3_X1/2`, `NAND4_X1/2`, and the same for `NOR`, `AND`, `OR` | 2 to 4 input gates |
| `XOR2_X1/2`, `XNOR2_X1/2` | exclusive or |
| `AOI21 AOI22 OAI21 OAI22 _X1/2`, `AOI211_X1`, `OAI211_X1` | and-or-invert, or-and-invert |
| `MUX2_X1/2`, `MUX4_X1` | multiplexers (`S`, or `S0 S1`) |
| `HA_X1`, `FA_X1` | half and full adder (`S`, `CO`) |
| `TBUF_X1/2` | tristate buffer, drives when `EN` = 1 |
| `DFF_X1/2` | D flip-flop, rising `CK`, outputs `Q` and `QN` |
| `DFFR_X1/2`, `DFFS_X1/2`, `DFFRS_X1` | with asynchronous active-low reset `RN`, set `SN`, or both |
| `DFFE_X1` | with enable `E` |
| `SDFF_X1/2`, `SDFFR_X1/2`, `SDFFS_X1` | scan flops: `SI`, `SE` |
| `DLH_X1/2`, `DLL_X1` | latch, transparent when `E` = 1 or `EN` = 0 |
| `CLKGATE_X1/2/4` | integrated clock gate: `CK`, `E`, test enable `SE`, output `GCK` |
| `TIE0`, `TIE1` | constant 0 and 1 |
| `FILL1/2/4/8` | filler cells (`dont_use`, LEF class `CORE SPACER`) |

## What the Liberty files hold

Units are ns, fF, V, kohm, nW. Each corner has:

- NLDM delay and transition tables, 7 input slews by 7 loads, for every
  input-to-output arc, with the load axis scaled to the cell's drive.
- Setup, hold, recovery and removal tables (3x3) and minimum pulse widths
  for the sequential cells; `ff`, `latch` and `test_cell` groups; clock gate
  attributes.
- `internal_power` tables per arc and `leakage_power` per input state.
- `function`, `three_state`, `max_capacitance`, pin capacitances,
  `pg_pin`s, three `wire_load` models and `operating_conditions`.

The numbers come from a small analytic model in the generator: logical
effort for delay (a pull-up and pull-down resistance per unit drive, a
parasitic term per cell, a slew term), switched internal capacitance plus a
short-circuit term for energy, and a per-cell leakage scaled by corner. That
gives tables with the right shape and ordering: a stronger drive is faster
and bigger, a stack is slower, the slow corner is slower and the hot one
leaks more. They are not measured from transistors; the characterizer will
replace them with simulated values.

## Physical view

The site is 0.19 um by 1.40 um (ten metal1 tracks). Metal1, 3 and 5 run
horizontally and metal2, 4 and 6 vertically, with sheet resistance and area
and edge capacitance per layer and a default via per cut layer. Every cell
is a whole number of sites wide and its Liberty `area` is its LEF size. Each
signal pin is a metal1 shape centred where a metal2 track crosses a metal1
track, one metal2 track per pin, so a router can drop a via on any pin.
Each pin is 0.07 um by 0.21 um, the metal1 minimum area, so a pin no net
connects is still legal. `VDD` and `VSS` are metal1 rails on the top and
bottom edges. Metal1 to metal2 has two default vias: `via1_0` with a
horizontal metal1 pad for wires, and `via1_1` with a vertical pad the shape
of a pin, so vias on the pins of neighbouring metal2 tracks keep their
spacing.
