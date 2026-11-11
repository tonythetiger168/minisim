# mpnr: place and route

`mpnr` takes a gate-level Verilog netlist and LEF, makes a floorplan, places
the cells, routes every net, and writes DEF. It is the back end after
synthesis: its DEF goes to parasitic extraction and from there to timing.

```
mpnr -lef tech.lef -lef cells.lef design.v -def design.def
```

```
mpnr: rand20000: 20000 cells, 20141 nets, 283 ports, 4 threads
floorplan: die 207.48 x 208.60 um, core 196.08 x 197.40 um, 141 rows, utilization 59.9%
placement: wire length (half perimeter) 222177.7 um global, 222527.5 legal, 218357.5 refined (6743 changes); mean move 0.43 um; 24884 fillers; 2.83 s
routing: 17688 nets, wire length 228648.8 um, 170583 vias, 4 iterations, 0 nets unrouted; 1.37 s
check: 17688 nets, 0 open, 0 shorts, 0 spacing violations
mpnr: done in 4.72 s
```

The exit status is 0 when every net is routed and the check is clean, 1 when
nets are left open or the check finds something, 2 on an error in the input.

## Inputs

| Input | Notes |
|---|---|
| Netlist | Structural Verilog as synthesis writes it, read by the shared `src/gnetlist.hpp`: cell and module instances, buses, concatenations, constants, `assign` aliases. Hierarchy is flattened (`inst/cell` names). `-top NAME` picks the top module. |
| LEF (`-lef`, repeatable) | Technology and cells, read by `src/lef.hpp`: routing layers with `DIRECTION`, `PITCH`, `OFFSET`, `WIDTH`, `SPACING`; vias; one core `SITE`; macros with pin shapes. |
| Floorplan DEF (`-floorplan`) | Optional. Its die area, rows, placement and routing blockages, special nets, placed ports and `FIXED` components are kept; everything else is placed by mpnr. Without it the floorplan is made from `-util`, `-aspect` and `-margin`. |

Constants in the netlist (`1'b0`, `1'b1`) get one tie cell each, found in the
LEF by name (`TIE0` / `TIE1`, `LOGIC0` / `LOGIC1`, `TIELO` / `TIEHI`) or named
with `-tielo CELL/PIN` and `-tiehi CELL/PIN`.

## Options

| Option | Meaning |
|---|---|
| `-util F` | Cell area over core area for the automatic floorplan (default 0.6) |
| `-aspect F` | Core height over width (default 1.0) |
| `-margin UM` | Space between the core and the die edge (default four rows) |
| `-min_layer N`, `-max_layer N` | Routing layers used for wires, 1 = the first routing layer in the LEF (default 2 to the top). Pins on layer `min_layer - 1` are reached with a via. |
| `-no_fill` | Do not add filler cells |
| `-no_straps` | No vertical power straps |
| `-strap_layer N` | Routing layer of the straps (the top vertical layer) |
| `-strap_width UM` | Strap width (four wire widths of the layer) |
| `-strap_pitch UM` | Distance between straps of one supply (32 row heights) |
| `-no_cts` | No clock trees |
| `-cts_fanout N` | Most loads on one clock buffer (16) |
| `-cts_buf CELL` | Clock buffer cell (default: by name, a `CLKBUF` before a `BUF`) |
| `-clock NET` | Build a tree on this net (repeat; default: nets on clock pins) |
| `-lib FILE` | Liberty library for timing (repeat) |
| `-sdc FILE` | Constraints; with `-lib`: timing report and timing-driven placement |
| `-no_timing_driven` | With `-lib` and `-sdc`: only report timing |
| `-place_only` | Stop after placement |
| `-def FILE` | The placed and routed design |
| `-netlist FILE` | The final flat netlist, with the tie and filler cells, for LVS and timing |
| `-gds FILE` | The layout as GDSII (see below) |
| `-gds_map FILE` | GDS layer numbers: lines of `LEFLAYER GDSLAYER [DATATYPE]` |
| `-gds_cells FILE` | A GDS with the cells' real layouts, copied into the output |
| `-svg FILE` | A picture of the layout |
| `-report FILE` | The summary as JSON |
| `-check FILE.def` | Only check a routed DEF (from mpnr or any other tool) against the LEF |
| `-threads N` | Worker threads; the default is every logical core |
| `-v` | Progress on stderr |

## What it does

1. **Floorplan.** Core area = cell area / utilization, rows of the core site
   in alternating `N` / `FS` orientation, ports spread along the die edge on
   routing tracks (inputs left and bottom, outputs right and top), and
   `FOLLOWPIN` power rails over the cells' power pins.
2. **Global placement.** Analytic: wire length as a quadratic (bound-to-bound
   net model), solved by preconditioned conjugate gradients; each round the
   solution is spread over the core by recursive area bisection and the cells
   are pulled a little harder towards their spread positions, until the two
   agree.
3. **Legalization.** Abacus: cells in x order, each to the row segment where it
   moves least; a row is packed by merging clusters at their least-squares
   position. Fixed cells and blockages split rows into segments.
   After global placement the **clock trees** are built (see below), so
   their buffers are legalized with the other cells.
4. **Refinement.** Neighbours swap and cells mirror while the half-perimeter
   wire length drops. Then fillers close the gaps.
5. **Routing.** One track set per layer from the LEF pitch and offset; wires
   run only in a layer's preferred direction; vias sit where tracks cross.
   Each net is grown pin by pin with A* search. Nets negotiate for grid nodes
   (PathFinder): a node another net claims costs extra, more each round, and
   nodes that stay contested build up a history cost. A net claims the nodes
   it uses and those within the end-of-line spacing on the same track, so
   wires and via pads of different nets keep the layer's `SPACING`. Nets still
   contested after a few rounds pull their uncontested neighbours out too.
   A pin on the layer below the first routing layer gets a via from the LEF
   when its pad fits the pin. When none fits, mpnr turns the lower pad a
   quarter or makes it longer, whichever keeps it nearest the pin and gives
   pin and pad the layer's `AREA`, and writes that via (`mpnr_<via>_r`,
   `_l`, `_rl`) to the DEF's `VIAS` section. A piece of wire below its
   layer's `AREA` (a via landing with no wire, a one-step jog, the pads of a
   power via stack) is made as long as the rule needs; the router keeps the
   room for that at every node.
6. **Check.** The DEF that was just written is checked from its shapes alone:
   every net one connected piece (opens), no metal of two nets touching
   (shorts) or closer than `SPACING`. The same check runs on any DEF with
   `-check`.

## Clock trees

Every net on a cell's clock pin (LEF `USE CLOCK`, or a pin named `CK`, `CLK`,
`CP`, `CKN`, `CLKN`, `GCK`) with more loads than `-cts_fanout` (16) gets a
tree; `-clock NET` names the nets instead. The loads are clustered by
position into groups of near-equal size, at most the fan-out each, and each
group gets a buffer at its centre. The buffers are clustered the same way,
level by level, down to one root buffer at the centre of the loads, which the
net's own driver drives. So every load sits behind the same number of
buffers, and the buffers of one level drive near-equal loads.

The buffer is `-cts_buf CELL`, or found in the LEF by name: a `CLKBUF` before
a `BUF`, the widest of its kind. Buffers are named `mpnr_cts_b<n>` and their
nets `mpnr_cts_n<n>`; they are in the DEF and in the `-netlist` output. The
floorplan leaves room for them. `-no_cts` routes clock nets as they are.

```
clock tree: clk: 2500 loads behind 168 CLKBUF_X4 in 3 levels; driver to load 298.4 to 468.3 um of wire
```

The last two numbers are the shortest and longest way from the driver to a
load along the tree (Manhattan distance between pins), a measure of skew
before any timing is known. Trees are balanced by level and by load, not by
delay: there is no wire snaking, no sizing and no timing analysis yet, and
a clock gate's output net gets its own tree, not balanced against the others.

## Timing

With `-lib FILE` (Liberty, repeatable) and `-sdc FILE`, mpnr times the design
with the same engine as `msta` (`src/sta.hpp`; see [STA.md](STA.md) for the
model and the SDC commands it reads). Wires are estimated from the placement:
a net's capacitance from its half-perimeter length, each driver-to-load delay
as the Elmore delay of a direct wire, with the resistance and capacitance per
micron of the two lowest routing layers in use (LEF `RESISTANCE RPERSQ`,
`CAPACITANCE CPERSQDIST`, `EDGECAPACITANCE`).

1. After global placement the design is timed with ideal clocks.
2. Nets on the worst path into an endpoint with less setup slack than a
   tenth of the clock period get a weight of up to 5, and global placement
   runs again with those weights (`-no_timing_driven` skips this).
3. The clock trees are built, and after legalization and refinement the
   design is timed again with the clocks propagated through their trees.

```
timing (global placement): setup WNS -1.039 ns, TNS -7.256 ns, 12 of 695 endpoints violate; hold WNS 0.018 ns, 0 violate
timing-driven placement: 945 nets weighted
timing (placed): setup WNS 1.646 ns, TNS 0.000 ns, 0 of 695 endpoints violate; hold WNS 0.018 ns, 0 violate
  clock clk: arrives at 625 clock pins between 0.120 and 0.127 ns (skew 0.007 ns)
```

The numbers are an estimate before routing. For sign-off, extract the routed
DEF with `mrcx` and run `msta` with the SPEF. mpnr does not size cells, add
buffers on data nets or fix hold violations.

## Power

Every row gets its VDD and VSS rail from the cells' supply pins (`SHAPE
FOLLOWPIN` special wires). Vertical straps (`SHAPE STRIPE`) on the top
vertical routing layer then tie the rails of each supply together: one strap
per supply every `-strap_pitch`, spread evenly over the core, with a stack of
single-cut vias down to each of its rails. The router treats straps and
stacks as obstructions. Straps are left out, with a note, when the top
vertical layer is one the cells' pins are reached from (fewer than three
layers above the rails) unless `-strap_layer` names it, and when a floorplan
DEF brings its own special nets. The check reports a supply whose wiring is
in more than one piece.

## GDS out

`-gds out.gds` writes the layout through the shared `src/gds.hpp`. The top
cell, named after the design, holds one reference per component, the wires as
paths (the same widths and end extensions as in the DEF), the vias and port
shapes as rectangles, a text label with each port's name on its shape, and
the die outline. The cells are abstracts made from the LEF: pin shapes, a
label with the pin's name on each pin, and the cell outline; obstructions are
left out. With `-gds_cells cells.gds` the cells are copied from that file
instead (it must hold every cell the design uses, in the LEF's database
unit).

Without a map, LEF layers are numbered by their position in the LEF (the
first layer is 1), datatype 0, and outlines go to GDS layer 0; `mpv` uses the
same numbers for the rules it makes from a LEF, so this checks the GDS:

```
mpnr -lef tech.lef -lef cells.lef design.v -def out.def -gds out.gds
mpv drc -lef tech.lef -lef cells.lef -write-deck rules.deck out.def
mpv drc -deck rules.deck out.gds
```

A map file replaces the default entirely: one `LEFLAYER GDSLAYER [DATATYPE]`
(or `GDSLAYER/DATATYPE`) per line, `#` comments, and `OUTLINE` for the
outlines. Layers it does not name are not written. `-check routed.def -gds
out.gds` converts an existing DEF. The time stamps in the file are fixed, so it is
the same for any thread count.

## Threads

Placement solves x and y at the same time and splits vector work into chunks;
routing runs nets whose search windows do not overlap at the same time; the
check runs one grid row per task. Chunks and net groups depend only on the
design, so the DEF is byte-identical for any `-threads` value (the self-test
compares 1, 2, 4 and 5).

## Speed

Random netlists from `bench/gen_pnr.py` on `tests/pnr/tiny.lef` (six metal
layers), utilization 0.6, 4 cores of a cloud VM. Every run ends with 0 open,
0 shorts and 0 spacing violations.

| Cells | Nets routed | Place | Route | Total, 4 threads | Total, 1 thread |
|---|---|---|---|---|---|
| 5,000 | 4,426 | 0.5 s | 0.4 s | 0.9 s | 1.4 s |
| 20,000 | 17,688 | 2.8 s | 1.5 s | 4.9 s | 8.3 s |
| 50,000 | 44,052 | 8.2 s | 3.3 s | 12.9 s | 23.3 s |
| 100,000 | 88,093 | 17.6 s | 6.3 s | 26.7 s | 48.7 s |

The same flow routes 5,000 Nangate45 cells clean on the real Nangate45 LEF
(ten metal layers) in 1.9 s.

## Limits

- Design rules are those the grid enforces: width, spacing, via pads, track
  alignment, cut spacing (between nets and inside a net) and minimum area
  of the wiring. Antenna and density rules are not handled, and `-check`
  does not measure area; run a DRC tool on the result (`mpv drc -lef`).
- Standard cells one row high. Larger macros can be fixed in a floorplan DEF
  (their pins and obstructions are honoured) but mpnr does not place them.
- Clock trees are balanced by level and load, not by delay, and are routed
  as ordinary nets. Timing drives placement only: no timing-driven routing,
  no sizing, no buffering of data nets, no hold fixing.
- No global-routing stage: a design too dense for its layers (above about
  85% utilization on three routing layers) leaves nets unrouted and takes
  long to say so.
- Power: rails and vertical straps with single-cut via stacks; no rings, no
  horizontal straps, no check of current or voltage drop.

## Files

| File | Role |
|---|---|
| `src/mpnr.cpp` | Command line and flow |
| `src/pnr_db.hpp` | Netlist bound to LEF macros, floorplan and placement state |
| `src/pnr_place.hpp` | Floorplan, global placement, legalization, refinement, fillers |
| `src/pnr_route.hpp` | Routing grid, pin access, A* search, negotiation |
| `src/pnr_out.hpp` | Rails and straps, DEF, netlist and SVG output, floorplan DEF input |
| `src/pnr_gds.hpp` | GDS out: layer map, cell abstracts, the top cell |
| `src/pnr_check.hpp` | Opens, shorts and spacing on a routed DEF |
| `src/lef.hpp`, `src/def.hpp` | LEF and DEF readers, DEF writer (shared) |
| `tests/pnr/`, `tests/pnr_test.py` | Test library, netlists and the self-test |
| `bench/gen_pnr.py` | Random netlists of any size |
