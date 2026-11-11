# mrcx: parasitic RC extraction

`mrcx` reads a routed design (DEF) and its technology and cells (LEF) and
writes the resistance and capacitance of every net as SPEF (IEEE 1481), the
file a timing or power tool reads to see the wires.

```sh
mrcx -lef tech.lef -lef cells.lef -def routed.def -o design.spef
mrcx -lef tech.lef -lef cells.lef -def routed.def -itf process.itf -temp 125 -o slow.spef
```

It builds without slang (`cmake -B build && cmake --build build`), runs on
every logical core by default (`-threads N` to choose, `-threads 1` for one),
and writes the same file for any thread count.

## What comes out

For each signal net: a `*D_NET` with its pins (`*CONN`, with coordinates and
the driving or loading cell), the capacitance to ground of every node and the
coupling capacitance to nodes of other nets (`*CAP`), and the resistors
between nodes (`*RES`). Units are fF and ohm. A coupling capacitor is listed
in both nets, as timing tools expect.

`-report FILE` writes one line per net (length, resistance, capacitance to
ground and to other nets, node count). The summary on stdout gives the totals.

Two checks come for free, since the tool has to work out what touches what:

- **open**: a net whose routing is in several parts that nothing joins;
- **short**: two nets whose metal overlaps on one layer.

Both go to stderr, and `-Werror` makes them exit 1.

## How it works

1. **Connectivity.** Each wire is cut where another wire, a via or a pin of
   its net touches it. The cut points are the nodes. Pins get their geometry
   from the LEF macro, placed and oriented as the DEF says.
2. **Resistance.** A wire piece is sheet resistance x length / width. A via is
   resistance per cut / number of cuts. Both follow temperature:
   `R(T) = R0 (1 + CRT1 (T - Tnom) + CRT2 (T - Tnom)^2)`. Wires longer than
   `-max_seg` (20 um) are cut again, so a long wire is a distributed RC line.
3. **Capacitance**, for every wire piece:
   - to ground: plate and fringe terms to the substrate, less the part of the
     plate that metal on the two layers below hides;
   - to the nearest wire on each side on the same layer, interval by
     interval, so a wire in between shields the ones behind it. A close
     neighbour also takes over part of the fringe field that would have gone
     to the substrate; that part moves from ground to coupling, and the net's
     total stays the same;
   - to each wire of another net it crosses one layer away, and two layers
     away through whatever the layer between leaves open.

   Coupling to power and ground metal (the DEF's SPECIALNETS) counts as
   capacitance to ground.

The formulas are Sakurai's closed-form fits (T. Sakurai, "Closed-form
expressions for interconnection delay, coupling, and crosstalk in VLSIs",
IEEE Trans. Electron Devices 40(1), 1993) on the layer stack: thickness and
height of each conductor and the permittivity of the dielectric between.

## The process file (`-itf`)

The stack comes from an ITF-style file, listed from the top of the chip down.
A conductor sits on the dielectric written after it:

```
TECHNOLOGY = demo2m
GLOBAL_TEMPERATURE = 25
DIELECTRIC top   { THICKNESS = 0.50 ER = 3.9 }
CONDUCTOR metal2 { THICKNESS = 0.20 WMIN = 0.10 SMIN = 0.10 RPSQ = 0.30 CRT1 = 0.003 }
DIELECTRIC ild1  { THICKNESS = 0.35 ER = 2.9 }
CONDUCTOR metal1 { THICKNESS = 0.15 WMIN = 0.10 SMIN = 0.10 RPSQ = 0.40 CRT1 = 0.003 }
DIELECTRIC pmd   { THICKNESS = 0.40 ER = 3.9 }
VIA via1 { FROM = metal1 TO = metal2 AREA = 0.0049 RPV = 6 CRT1 = 0.001 }
```

`ETCH = e` on a conductor says each edge of a wire loses `e` um when it is
made: resistance and capacitance use the width `drawn - 2e` and the spacing
`drawn + 2e`.

`$` starts a comment. Properties the model does not use are skipped. When the
LEF and the process file name a layer differently, `-map FILE` holds lines of
`LEF-layer process-conductor`. `tests/rcx/sky130.itf` is the stack of the open
SkyWater 130 nm process.

Without `-itf`, the LEF's own numbers are used: `RESISTANCE RPERSQ`, the cut
layers' `RESISTANCE`, `CAPACITANCE CPERSQDIST` and `EDGECAPACITANCE` for the
capacitance to ground, `THICKNESS` and `HEIGHT` for coupling.

## Corners

A sign-off run wants the same design at several corners. `-corner NAME` starts
one, and the process file, temperature and scale options that follow belong to
it; those given before the first `-corner` are the defaults of every corner.

```
mrcx -lef tech.lef -lef cells.lef -def routed.def -o design.spef \
     -corner cmax -itf cmax.itf -temp 125 \
     -corner cmin -itf cmin.itf -temp -40 \
     -corner typ  -itf typ.itf
```

This writes `design.cmax.spef`, `design.cmin.spef` and `design.typ.spef` (and a
report per corner with `-report`). The LEF and DEF are read once, every process
file is checked before the first extraction, and each file is byte for byte
what a run with only that corner's options writes. Three corners of the 290k-net
benchmark take 8.6 s on 4 cores against 10.9 s for three runs.

## Options

| Option | Meaning |
|---|---|
| `-lef FILE` | technology and cell LEF, repeatable, technology first |
| `-def FILE` | the routed design |
| `-o FILE` | SPEF to write (default `<design>.spef`) |
| `-itf FILE`, `-map FILE` | process stack, and layer names when they differ |
| `-temp C` | temperature for resistance |
| `-profile` | print the time of each extraction step |
| `-corner NAME` | start a corner; the `-itf`, `-temp`, `-rscale`, `-cscale`, `-ccscale` after it are its own, and it is written to `design.NAME.spef` |
| `-rscale F`, `-cscale F`, `-ccscale F` | scale resistance, all capacitance, coupling only (corners) |
| `-no_coupling` | ground every coupling capacitor (a decoupled SPEF); `-cc_factor F` multiplies it on the way |
| `-cc_threshold FF` | ground coupling capacitors smaller than this |
| `-cc_dist UM` | how far to look for a neighbour (default 6 pitches) |
| `-max_seg UM` | longest wire piece (default 20; 0 cuts only at junctions) |
| `-no_name_map` | full names instead of a `*NAME_MAP` |
| `-report FILE` | per-net table |
| `-date TEXT` | fixed `*DATE`, for files that compare equal |
| `-threads N`, `-j N` | worker threads (default: all logical cores, `$MINISIM_THREADS` overrides) |
| `-Werror` | exit 1 on an open or a short |

## Accuracy

Checked against OpenRCX on the routed `gcd` design in sky130 from the OpenROAD
test suite (`src/rcx/test/gcd.def`, reference `gcd.spefok`), with
`tests/rcx/sky130.itf`. 411 nets, all connected, no shorts:

| | mrcx / OpenRCX, all nets | per net: median | 10% to 90% of nets |
|---|---|---|---|
| Total capacitance | 1.017 | 1.02 | 0.98 to 1.07 |
| Capacitance to ground | 1.010 | 1.02 | 0.95 to 1.13 |
| Coupling capacitance | 1.036 | 1.04 | 0.92 to 1.44 |
| Total resistance | 0.84 | 0.90 | 0.78 to 0.95 |

The one fitted number in the model is how much of the ground fringe a
neighbour takes over (`kShield` in `src/rcx.hpp`); it was set on this design.
The resistance difference is input data, not method. A fit over the 411 nets
reproduces OpenRCX's totals to 1% as 1.33 x mrcx's wire resistance plus a
resistance per via: OpenRCX's rules file for this design carries 1.21 ohm/um
for a 0.14 um met1 wire (0.169 ohm/sq), where the LEF and `sky130.itf` say
0.125 ohm/sq, and its via values differ too. Give mrcx the same sheet and via
resistances, or an `ETCH`, and the two agree.

## Speed

`bench/rcx_gen.py N` writes a routed DEF of N nets. 290,000 nets (840 mm of
wire, 1.7 million nodes, 2.9 million coupling capacitors, a 270 MB SPEF) on a
4-core machine:

| | 1 thread | 4 threads |
|---|---|---|
| Read LEF and DEF | 1.0 s | 1.0 s |
| Extract | 5.0 s | 1.5 s |
| Write SPEF | 1.6 s | 0.6 s |

`-profile` prints where the extraction time goes. With 4 threads: nodes and
resistors 0.22 s, grids 0.05 s, capacitance 0.76 s, collecting each net's
coupling capacitors 0.33 s, totals 0.05 s.

## For other tools

- `src/spef.hpp` is the SPEF reader and writer for every tool:
  `spef::File f = spef::read("design.spef");` gives nets, pins, capacitors and
  resistors with the name map applied and the unit scales in `f.cScale`,
  `f.rScale`, `f.tScale`. Net sections are parsed in parallel.
- `src/rcx.hpp` is the extraction engine on a plain layout description
  (`rcx::Layout`), for a tool that wants parasitics without files (a router
  estimating its own wires).
- `src/lef.hpp` and `src/def.hpp` are the shared LEF and DEF readers.

## Limits

- Wires are axis-parallel; a diagonal wire is skipped with a warning.
- Metal inside cells (pins, obstructions) connects but adds no capacitance.
- No coupling across three or more layers or to wires that pass nearby on
  another layer without overlapping, no via capacitance, no inductance.
- Corners in one run share the read of the design, not the extraction: each corner is extracted again, since its stack changes every capacitance.
- Dielectrics are planar layers: no conformal layers, no width-dependent
  etch or thickness.
