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
6. **Check.** The DEF that was just written is checked from its shapes alone:
   every net one connected piece (opens), no metal of two nets touching
   (shorts) or closer than `SPACING`. The same check runs on any DEF with
   `-check`.

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
  alignment, and cut spacing (between nets and inside a net). Minimum area,
  antenna and density rules are not checked; run a DRC tool on the result.
- Standard cells one row high. Larger macros can be fixed in a floorplan DEF
  (their pins and obstructions are honoured) but mpnr does not place them.
- The clock is routed as an ordinary net: no clock tree, no buffering, no
  timing-driven placement or routing yet.
- No global-routing stage: a design too dense for its layers (above about
  85% utilization on three routing layers) leaves nets unrouted and takes
  long to say so.
- Power: rails only, no straps or rings.

## Files

| File | Role |
|---|---|
| `src/mpnr.cpp` | Command line and flow |
| `src/pnr_db.hpp` | Netlist bound to LEF macros, floorplan and placement state |
| `src/pnr_place.hpp` | Floorplan, global placement, legalization, refinement, fillers |
| `src/pnr_route.hpp` | Routing grid, pin access, A* search, negotiation |
| `src/pnr_out.hpp` | DEF, netlist and SVG output, floorplan DEF input |
| `src/pnr_gds.hpp` | GDS out: layer map, cell abstracts, the top cell |
| `src/pnr_check.hpp` | Opens, shorts and spacing on a routed DEF |
| `src/lef.hpp`, `src/def.hpp` | LEF and DEF readers, DEF writer (shared) |
| `tests/pnr/`, `tests/pnr_test.py` | Test library, netlists and the self-test |
| `bench/gen_pnr.py` | Random netlists of any size |
