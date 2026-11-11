# sky130: a real open PDK as the second test library

`lib/mini45` is this project's own generic library and stays the default.
This directory adds cells from the SkyWater 130 nm high-density library
(`sky130_fd_sc_hd`) so every tool can be tried on a library that was
characterized on silicon models, with its quirks: negative delays, pins on
local interconnect, state tables, double-height cells.

| File | What |
|---|---|
| `sky130_hd_subset_tt.lib` | 47 cells of `sky130_fd_sc_hd__tt_025C_1v80.lib` (1.8 V, 25 C), each copied whole |
| `sky130_hd_subset.lef` | the 51 matching macros (the 47 cells and four fill and tap cells) |
| `sky130_hd.tlef` | the technology LEF: li1 and met1 to met5 |
| `sky130.py` | fetches the full files, cuts the subset, checks it |
| `LICENSE` | Apache License 2.0, as shipped with the PDK |

The subset has inverters, buffers, NAND, NOR, AND, OR, XOR, AOI and OAI
gates, multiplexers, a half and a full adder, flops with reset, set and
scan, a latch, a clock gate, tristate drivers, tie, fill, tap, decap and
antenna cells.

## The whole library

    python3 lib/sky130/sky130.py fetch     # 3 corners, merged LEF, tech LEF: 41 MB into lib/sky130/pdk
    python3 lib/sky130/sky130.py --check   # the files kept here are cut from exactly those

`fetch` downloads from pinned commits and checks SHA-256 sums.
`lib/sky130/pdk` is ignored by git; when it is there, the `sky130_full`
test runs on all 428 cells and three corners.

## Where the files come from

- Liberty: <https://github.com/efabless/skywater-pdk-libs-sky130_fd_sc_hd>,
  commit `5a42fd01a0eeb2338340438443c7062e198d97a4`, `timing/`.
- LEF and technology LEF:
  <https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts>, commit
  `bfcc1895a7a1d9ff6e677066825812e549b72421`, `flow/platforms/sky130hd/lef/`.

Copyright 2020 The SkyWater PDK Authors. Licensed under the Apache License,
Version 2.0; see `LICENSE`. The files here are unmodified extracts: whole
cell groups and macros with the library's own header, plus a comment at the
top saying so. They are test data, not a PDK: for a tape-out use the PDK
itself.

## What the tools make of it (2026-11-11)

| Tool | Result |
|---|---|
| `mlibc check` | reads each 12.8 MB corner in 0.3 s. Findings are facts about the library: negative delays (65 tables at tt, 640 at ff), three transition tables that shrink with load, 27 pins with no track crossing, 7 double-height level shifters on the single-height site |
| `mlibc compare` ss against ff | 1341 arcs, slow/fast ratio 3.1, no findings |
| `msynth` | maps to it (`-lib`), 0.3 s with the full library |
| `msta`, `mpower` | read the mapped netlist and report |
| `mpnr` | places it; the router cannot reach pins on li1 yet, so nets stay open |
| `mchar` | not yet: the cells need BSIM4 device models, which `mspice` does not have |
