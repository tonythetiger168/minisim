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
| `mini45.sp` | transistor netlists: one `.subckt` per cell |
| `mini45_models_tt.sp`, `_ss`, `_ff` | level-1 MOSFET models per corner |
| `gen_mini45.py` | the generator: every file above but the Liberty files comes from it |
| `characterize.sh` | measures the Liberty files from the netlists with `mchar` |

```sh
msim netlist.v lib/mini45/mini45.v -top top       # gate-level simulation
python3 lib/mini45/gen_mini45.py                  # regenerate after editing the generator
lib/mini45/characterize.sh build/mchar            # measure the Liberty files again (minutes)
ctest -R 'mini45|mchar'                           # files up to date, models correct, tables as measured
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

## Where the numbers come from

The timing, power and capacitance numbers are measured. Each cell has a
static CMOS transistor netlist in `mini45.sp` (complex gates, transmission
gate flops and latches, sized by drive strength), and `mchar`
([CHAR.md](CHAR.md)) simulates every arc on `mspice` at each corner's supply
and temperature with that corner's device models:

- delay and transition tables, clock-to-output, reset and set arcs;
- setup, hold, recovery and removal tables (the offset at which the
  clock-to-output delay is 10% longer);
- internal energy per transition and pin capacitance.

The devices are level-1 MOSFETs with made-up parameters (50 nm gate, an
inverter of 0.2 um over 0.3 um), chosen to land near a 45 nm process: a
fanout-of-4 inverter delay of about 20 ps at the typical corner. They are
not a foundry model, and a level-1 device knows nothing of velocity
saturation or subthreshold current, so:

- **Leakage is analytic**, not measured: a per-cell value by input state,
  scaled by corner (a level-1 transistor does not leak when it is off).
- **Tristate disable arcs** (`TBUF`) are analytic too: `mchar` does not
  measure them yet. The enable arcs and the minimum pulse widths are
  measured.

The generator still holds the analytic model (logical effort for delay,
simple energy and leakage rules) these come from; it also writes the
templates that tell `mchar` what to measure.

## Physical view

The site is 0.19 um by 1.40 um (ten metal1 tracks). Metal1, 3 and 5 run
horizontally and metal2, 4 and 6 vertically, with sheet resistance and area
and edge capacitance per layer and a default via per cut layer. Every cell
is a whole number of sites wide and its Liberty `area` is its LEF size. Each
signal pin is a metal1 shape centred where a metal2 track crosses a metal1
track, one metal2 track per pin, so a router can drop a via on any pin.
`VDD` and `VSS` are metal1 rails on the top and bottom edges.
