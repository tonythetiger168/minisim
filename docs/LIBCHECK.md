# Cell library checks: `mlibc`

`mlibc` reads Liberty (`.lib`) files through the shared reader
(`src/liberty.hpp`) and LEF through `src/lef.hpp`, and looks for the
mistakes that make a cell library unusable downstream. It builds with the
core tools (no slang).

```sh
mlibc check lib/mini45/mini45_tt.lib -lef lib/mini45/mini45_tech.lef -lef lib/mini45/mini45.lef
mlibc check slow.lib typ.lib fast.lib -Werror        # several corners in one run
mlibc report lib.lib                                 # one line per cell
mlibc report lib.lib -cell NAND2_X1                  # pins, arcs and table ranges of one cell
mlibc compare slow.lib fast.lib                      # the slow corner must be slower
```

A finding is one line, `file: cell: [RULE] what: detail`. The exit status is
0, or 1 with `-Werror` when anything was found, and 2 for a file that cannot
be read. `-disable RULE` leaves a rule out and `-list` prints the rules.

Files are read on separate threads and cells are checked in parallel, on all
hardware threads by default; `-threads N` sets the count and `-threads 1`
turns it off. Findings are collected per cell and printed in file and cell
order, so the output is identical for any thread count.

## Rules

| Rule | Finding |
| --- | --- |
| `LIB001` | an output pin has no function |
| `LIB002` | timing_sense disagrees with the pin's function |
| `LIB003` | the function depends on an input that has no timing arc to the output |
| `LIB004` | a combinational arc from an input the function does not depend on |
| `LIB005` | a delay or transition table decreases as the load grows |
| `LIB006` | a negative delay or transition |
| `LIB007` | an input pin has no capacitance |
| `LIB008` | a sequential cell's data pin has no setup or no hold arc, or its clock pin is not marked |
| `LIB009` | a cell has no area |
| `LIB010` | an output pin has no max_capacitance |
| `LIB011` | a function names something that is not a pin or a state variable |
| `LIB012` | leakage_power states overlap, leave an input state out, or there is no cell_leakage_power |
| `LIB013` | a stronger drive of the same cell is slower or smaller than a weaker one |
| `LIB014` | a table index is not strictly increasing |
| `LIB015` | an output pin has no timing arc |
| `LEF001` | a cell has no LEF macro |
| `LEF002` | the Liberty area is not the LEF macro's size |
| `LEF003` | a pin is in the Liberty cell or the LEF macro but not in both, or its direction differs |
| `LEF004` | the macro is not a whole number of sites wide, or not one site tall |
| `LEF005` | a signal pin has no shape on a routing track crossing |
| `CMP001` | a cell or pin is in one corner and not in the other |
| `CMP002` | the slow corner is faster than the fast corner |
| `CMP003` | area, function or pin capacitance differs between corners |

Notes on a few of them:

- `LIB002` works out from the `function` whether the output rises, falls or
  does both when one input rises, and compares that with `timing_sense`. It
  is checked for a pin with one unconditional arc; arcs split by `when` are
  left alone, since each may be unate where the whole function is not.
- `LIB005` looks along the load axis only. A delay may fall as the input
  slew grows; it may not fall as the load grows.
- `LIB012` accepts a cell that gives leakage for some states and a
  `cell_leakage_power` for the rest. Two groups that match the same input
  state are a finding.
- `LIB013` compares cells named `NAME_X<n>`: a larger `n` must not be
  smaller in area, nor slower at the weaker cell's largest load.
- `LEF005` takes the first horizontal and the first vertical routing layer
  as the track grid, and wants each signal pin to have a shape that covers a
  crossing of the two, so a router can reach it with a via.

`compare` matches cells, pins and arcs by name between two corners and
reports a cell or pin missing from one (`CMP001`), an arc that is faster in
the slow file (`CMP002`), and an area or function that differs (`CMP003`).
Its last line gives the mean slow-to-fast delay ratio.
