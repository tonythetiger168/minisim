# mpv: physical verification

`mpv` checks a layout: design rule checking (DRC) against a rule deck, on
GDSII, OASIS or a routed DEF, and layout versus schematic (LVS): transistor
netlist extraction from GDSII or OASIS, and a routed block against its gate
netlist.

```sh
mpv drc -deck rules.drc chip.gds                 # every rule, one line each
mpv drc -deck rules.drc chip.gds -top core -q    # one cell; only rules with violations
mpv drc -deck rules.drc chip.gds -o drc.txt      # also write the markers
mpv drc -deck rules.drc chip.gds -rdb drc.lyrdb  # markers for KLayout's marker browser
mpv drc -deck rules.drc chip.gds -threads 8
mpv drc -deck rules.drc chip.oas                 # OASIS, told from the file's first bytes
```

## Layout files

A layout is GDSII or OASIS (SEMI P39); `mpv` looks at the first bytes of the
file, not at its name. The OASIS reader takes every record of the standard:
rectangles, polygons and paths with all point-list forms, trapezoids and the
25 compact trapezoids, circles (as 64-sided polygons), texts, placements
with rotation, mirroring and magnification, the eleven repetition forms,
modal variables, relative and absolute coordinates, name tables by reference
(before or after their use, strict mode included) and compressed blocks
(the DEFLATE decoder is built in; no zlib is needed). Properties and
extension records are skipped. A repetition on a regular grid becomes an
array reference; other repetitions are expanded. The reader is tested
against KLayout's reading of the same files (`tests/pv/oasis`).

```
mpv drc: cell bad, 6 shapes on 3 layers, 5 x 5 um, 1 unit = 0.001 um
M.W         1  metal width
M.S         1  metal spacing
M.A         1  metal area
V.SZ        0  via must be 0.17 x 0.17
V.E         1  metal enclosure of via
M.D         1  metal density in 5 um windows
5 violations in 5 of 6 rules
```

Exit status: 0 clean, 1 violations, 2 a usage or input error.

| Option | |
|---|---|
| `-deck <file>` | the rule deck (below) |
| `-top <cell>` | cell to check; default is the one cell nothing references |
| `-o <file>` | results file with the markers |
| `-rdb <file>` | the markers as a KLayout report database (`.lyrdb`): open it in KLayout's marker browser (Tools, Marker Browser) over the layout to step through the violations. One category per rule; each marker is a box, or an edge where the margin is zero, with the measured value |
| `-max <n>` | markers written per rule (default 1000); the counts stay exact |
| `-threads <n>` | worker threads; default is every logical CPU (`$MINISIM_THREADS` overrides) |
| `-q` | print only the rules with violations |

## A routed DEF as the layout

```sh
mpv drc -lef tech.lef -lef cells.lef routed.def                  # rules from the LEF
mpv drc -lef tech.lef -deck rules.drc routed.def -o drc.txt      # your own deck
mpv drc -lef tech.lef routed.def -write-deck tech.drc            # see the rules it made
```

The pins of every placed cell, the wires and vias of the nets and special
nets and the design's pins become shapes; `-obs` adds the cells'
obstructions. LEF layers get GDS numbers by their position in the LEF (the
first layer is `1/0`). Without `-deck` the rules are made from the LEF
technology: width, spacing and minimum area of each routing layer, spacing
of each cut layer, and every cut covered by the metal below and above. The
extent for density rules is the die area. The LEF and DEF readers are the
shared `src/lef.hpp` and `src/def.hpp`.

## Rule deck

One statement per line; `#` starts a comment. Lengths are in microns, areas
in square microns.

```
layer poly 66/20                 # a drawn layer: GDS layer/datatype, several allowed
layer diff 65/20
gate = poly & diff               # a derived layer
rule poly.1a width(poly) < 0.15 "poly width"
rule poly.8  overhang(poly, diff) < 0.13 "poly end cap"
```

A layer must be defined before it is used.

### Layer expressions

| | |
|---|---|
| `a & b`, `a \| b`, `a - b`, `a ^ b` | AND, OR, A NOT B, XOR. `&` binds tighter; use parentheses otherwise |
| `grow(a, d)`, `shrink(a, d)` | move every edge out or in by `d` |
| `interact(a, b)`, `not_interact(a, b)` | the polygons of `a` that overlap or touch `b`, and the others |
| `inside(a, b)`, `not_inside(a, b)` | the polygons of `a` entirely within `b`, and the others |
| `outside(a, b)` | the polygons of `a` that share no area with `b` |
| `holes(a)` | the holes of `a` |
| `rects(a)`, `not_rects(a)` | the polygons of `a` that are (not) rectangles |
| `not_rect(a, w, h)` | the polygons of `a` that are not `w` by `h` rectangles (either way round) |
| `extent()` | the bounding box of the layout |

### Checks

A rule reports the places where its comparison is true.

| | |
|---|---|
| `width(a) < v` | inside distance between facing edges, and across inside corners |
| `space(a) < v` | outside distance between polygons, in notches, and corner to corner |
| `space(a, b) < v` | outside distance between `a` and `b` |
| `enclosure(a, b) < v` | `a` must be inside `b` with margin `v`. Reports margins from 0 up to `v`, and any part of `a` outside `b` |
| `enclosure(a, b, opposite) < v` | the same margins, but a polygon of `a` passes when it has margin `v` on two opposite sides (left and right, or bottom and top). This is the foundry rule "enclosure on one of two adjacent sides", as for contacts and vias with a line-end enclosure. Parts of `a` outside `b` are always reported |
| `overhang(a, b) < v` | where `a` crosses `b`, `a` must continue past `b` by `v` (a poly end cap, diffusion past a gate) |
| `area(a) < v`, `area(a) > v` | polygon area |
| `density(a, w) < v`, `... > v` | area of `a` over the area of each `w` by `w` window of the extent. `density(a, w, s)` steps the windows by `s`; the last window of each row and column is moved back to end on the extent |
| *layer expression* | every polygon of the result is a violation: `rule li.5 licon - li "licon without li"` |

Distances across corners are Euclidean. A nearer edge hides the edges behind
it, so a check never reports through another shape.

## Results file

```
mpv-drc 1
cell bad
rule M.W 1 "metal width"
  0 0.42 2 0.52 0.1
rule M.S 1 "metal spacing"
  0 0.3 2 0.42 0.12
```

One `rule` line per rule (name, count, description), then one line per marker:
the box `x1 y1 x2 y2` in microns and the measured value (a distance, an area,
or a density). For a distance the box is the gap between the two edges or
corners. Markers are sorted, so two runs can be compared with `diff`.

## LVS: netlist extraction

```sh
mpv lvs -deck tests/pv/sky130.lvs tests/pv/sky130/sky130_fd_sc_hd__inv_1.gds -o inv.spice
```

```
.subckt sky130_fd_sc_hd__inv_1 A VGND VNB VPB VPWR Y
M0 Y A VGND VNB sky130_fd_pr__nfet_01v8 w=0.65 l=0.15
M1 Y A VPWR VPB sky130_fd_pr__pfet_01v8_hvt w=1 l=0.15
.ends
```

The deck says what conducts, what names a net, and what a transistor is:

```
connect licon li                 # one net where the two overlap
connect li met1 by mcon          # ... where mcon overlaps both
label li 67/5 67/16              # text on these GDS layers names the li net under it
device mos sky130_fd_pr__nfet_01v8 gate=ngate poly=poly sd=nsd bulk=pwell
```

Every polygon of the `gate` layer is one transistor: its gate terminal is the
`poly` polygon over it, source and drain are the two `sd` polygons beside it,
the body is the `bulk` polygon under it. The width is half the length of the
gate's sides shared with source and drain; the length is the gate area over
the width. Labelled nets are the ports. A gate without exactly two
source/drain regions, two labels on one net (a short) and one label on two
nets (an open) are reported and give exit status 1.

The comparison engine (`lvs::compare` in `src/lvs.hpp`) matches two netlists
by partition refinement: ports by name, everything else by structure, with
parallel transistors merged and source and drain interchangeable. It reports
unmatched devices and nets and size differences. The command line option to
compare against a SPICE schematic comes with the shared SPICE reader.

## LVS of a routed block

```sh
mpv lvs -lef tech.lef -lef cells.lef routed.def -netlist block.v
mpv lvs -lef tech.lef routed.def -o extracted.v          # extraction only
```

```
mpv lvs: block counter, 49 instances, 150 pins, 27 nets, 9 ports
note: net VDD of the DEF is 3 separate nets in the layout
open: net s[0] is in 2 pieces in the layout: [ha0/u_s/Y] [m0/A]
MISMATCH: 49 instances, 21 nets and 9 ports compared, 1 opens, 0 shorts
```

Here the cells are the devices. The layout's nets are found from the
geometry alone: the pin shapes of the placed cells (from the LEF), the wires
and vias of the DEF and the shapes of the block's pins. Shapes of one metal
layer that overlap or share part of a side are one net, a cut joins the
metal under and over it, and all shapes of one cell pin are one node. The
net names and connection lists in the DEF are not trusted; they only name
the extracted nets in messages.

The netlist is structural Verilog (read by `src/gnetlist.hpp`, hierarchy
flattened to the leaf cells; `-top` picks the module, by default the one
named like the DEF design). Instances and ports pair by name, as place and
route keeps them. Then:

- a net of the netlist whose pins are on several nets of the layout is an
  **open**, listed with its pieces;
- a net of the layout that holds pins of several nets of the netlist is a
  **short**;
- an instance on one side only, a different cell, a pin the cell does not
  have, and a pin the netlist leaves open that is wired in the layout are
  reported too.

Supply pins are compared only when the netlist connects them. Cells with
nothing but supply pins (fillers, taps) need not be in the netlist. A net of
the DEF that is several nets in the layout and is not compared (supply rails
that nothing ties together) is a note, as are pieces of wire that reach no
pin. Exit status: 0 match, 1 mismatch, 2 error. `-o` writes the extracted
netlist as Verilog, supply pins included.

## SKY130 example

`tests/pv/sky130.drc` holds 35 SKY130 rules from diffusion to metal 2. The
`sky130_fd_sc_hd` standard cells in `tests/pv/sky130/` pass it:

```sh
mpv drc -deck tests/pv/sky130.drc tests/pv/sky130/sky130_fd_sc_hd__dfxtp_1.gds
```

## Threads

The rules of a deck run in parallel, and a large layer operation is cut into
vertical strips that run in parallel and are stitched back. Layers are held
in a canonical form, so the report is identical for every thread count.

## Limits

- The hierarchy is flattened before checking.
- Geometry must be axis-parallel. Polygons and paths with other angles, and
  cell references that are magnified or rotated by other than a multiple of
  90 degrees, are counted in a warning and not checked.
- Round path ends are taken as square.

## Source

| File | |
|---|---|
| `src/gds.hpp` | GDSII reader and writer (namespace `gds`), shared with the other physical tools |
| `src/oasis.hpp` | OASIS reader into the same library (namespace `oasis`); `oasis::readLayout` takes either format |
| `src/geom.hpp` | regions of rectangles: booleans, sizing, boundary edges, connected polygons |
| `src/layout.hpp` | a GDS library flattened to rectangles per layer |
| `src/layout_def.hpp` | a routed DEF with its LEF as a flat layout; a deck from the LEF technology |
| `src/drc.hpp` | the deck parser and the checks |
| `src/lvs.hpp` | netlist extraction and netlist comparison |
| `src/lvs_block.hpp` | block LVS: nets of a routed DEF from its geometry, comparison with a gate netlist |
| `src/mpv.cpp` | the command line |
