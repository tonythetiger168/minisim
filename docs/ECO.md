# meco: functional ECO

`meco` answers the question a late RTL fix raises: the design is already
placed and routed, the RTL changed a little, what is the smallest edit to the
existing netlist that makes it the new design? It does the job Conformal ECO
does. It takes the implemented netlist and a netlist of the revised design
(a fresh synthesis of the new RTL), and writes a patched netlist and an edit
script that keep everything of the old netlist that still fits.

```sh
meco -old impl.v -new resynth.v -lib cells.lib -o patched.v -script eco.tcl
```

```
changed compare points (14):
  par
  q_reg[3].next
  ...
  > sx3/B <- ECO_c[2]
  > cy3/B <- ECO_c[2]
  - cell cy2 (AND2_X1)
  + cell ECO_cy2 (OR2_X1)
  > ECO_cy2/A <- a[2]
  > ECO_cy2/B <- c[1]
  > ECO_cy2/Y <- ECO_c[2]
meco: 81 compare points, 14 changed; patch: +1 cells (area 2.5), -1 cells (area 2.5), 2 pins rewired, 2 existing nets reused; patched netlist proved equivalent
```

One carry gate changed, deep inside the logic of fourteen outputs and
registers. The patch is that one gate, not fourteen cones.

Exit status: 0 the patched netlist is proved equivalent to the new design,
1 it is not (a proof failed or ran out of effort; the report names the
points), 2 a file could not be read, 3 proved, but `-spare` could not do it
without adding a cell.

## Options

| Option | Meaning |
|---|---|
| `-old FILE` | The implemented gate netlist (repeatable) |
| `-new FILE` | The revised design as a gate netlist (repeatable) |
| `-lib FILE` | Liberty cell library (repeatable) |
| `-top NAME` | Top module of both; `-old-top` / `-new-top` when they differ |
| `-tie PORT=0\|1` | Hold an input constant in both designs (scan enable, test mode) |
| `-o FILE` | Write the patched netlist |
| `-script FILE` | Write the edits as Tcl |
| `-report FILE` | Write the report to a file as well |
| `-prefix STR` | Prefix of added cells and nets (default `ECO_`) |
| `-no-sweep` | Keep old cells that end up driving nothing |
| `-spare` | Metal-only: add and remove no cell (see below) |
| `-spare-cells GLOB` | `-spare`, with only the unloaded cells of this instance name as spare cells |
| `-no-rectify` | Only copy the changed cones; do not look for the gate to change |
| `-budget N` | SAT conflicts per proof (default 100000) |
| `-threads N` | Worker threads. Default: all logical cores. The result does not depend on N |
| `-q` | Print only the summary line |

## What is compared

Both netlists are flattened and read against the library. A cell is one of
three kinds, decided by its Liberty description:

- combinational: every output has a `function`;
- state: one `ff` or `latch` group. Its next-state (or data), clock (or
  enable), clear and preset are compare points, each a function of the
  cell's pins, so a scan flop, an enable flop and a plain flop compare by
  what they do;
- anything else (a memory, a macro, a cell without a function) is a black
  box: its outputs are free inputs, its input pins are compare points, and
  the instance must be the same cell on both sides.

Compare points are matched by name: output ports, registers and black boxes.
That is what a re-synthesis of slightly changed RTL gives. A register only in
the new design is added, one only in the old design is removed, and one whose
cell cannot do the new job (a reset was added, say) is replaced under its old
name.

`-tie` is how a scan-inserted netlist is compared with a netlist that has no
scan: with scan enable held at 0 the scan flop is the plain flop.

## How the patch is found

1. Both designs become one and-inverter graph (the equivalence checker's),
   so identical logic is already the same node. Every compare point is
   simulated on random patterns and then proved or refuted by SAT.
2. For the changed points, meco first looks for the place to fix. For each
   net of the new design that the old design has no equal of, it tries to put
   it in the old netlist: in place of an old net, or on one input pin of an
   old gate. A candidate is taken only when proved: every load of the edited
   net must then compute something the new design also computes. This is
   what finds a changed gate deep in a cone, or a changed pin.
3. Whatever still differs gets its cone copied from the new design, down to
   the first old nets with the same function (proved, not guessed). Those
   nets are reused, so a copy stops as early as it can.
4. Old cells that no longer drive anything are removed (cells that drove
   nothing before, such as spares, are left alone).
5. The patched netlist is read back from scratch and every compare point is
   proved against the new design. The exit status is that proof.

Proofs are independent of each other and run on all cores; results are
combined in a fixed order, so the patch is the same for any `-threads`.

## Metal-only: `-spare`

After the base layers are made, an ECO can change wires but not cells.
`-spare` builds the patch from cells that are already there:

- the spare cells: cells of the old netlist that drive nothing (all of them,
  or those `-spare-cells 'spare_*'` names);
- the cells the change itself frees. They are not removed: their inputs are
  tied low and they are spare cells from then on.

Each gate of the patch is moved onto a free cell that can compute it: the
same cell, or another one with inputs tied or sharing a net (an OR2 from a
MUX2 with `S` and `B` together, an inverter from a NAND2), or the inverse
function with a spare cell as inverter behind it. Among the cells that fit,
the one with the fewest inputs is taken.

```
  > cy2/A <- 1'b0
  > cy2/B <- 1'b0
  > spare_m0/A <- c[1]
  > spare_m0/B <- a[2]
  > spare_m0/S <- a[2]
  > spare_m0/Y <- ECO_c[2]
meco: 81 compare points, 14 changed; patch: +0 cells (area 0), -0 cells (area 0), 8 pins rewired, 2 existing nets reused; patched netlist proved equivalent
meco: metal-only: 1 spare cells used, 1 old cells left as spares, 0 cells without a spare
```

The script then holds only `connect_net` and `disconnect_net`. A gate no
free cell can stand in for is added as a normal cell, the report lists it
(`no spare cell for ...`) and the exit status is 3. Registers are in that
group for now: a register of the patch is not moved onto a spare flop.
Spare cells are chosen by function, not by where they sit on the die.

## The edit script

```tcl
create_net {ECO_c[2]}
create_cell {ECO_cy2} {OR2_X1}
disconnect_net {c[2]} {sx3/B}
connect_net {ECO_c[2]} {sx3/B}
...
remove_cell {cy2}
```

The commands are the ones place-and-route tools take for an ECO, so the
script is the input for placing the new cells and rerouting the changed nets.

## Speed

`bench/eco/gen.py STAGES BITS old.v new.v` writes a pipelined adder and a
copy with three gates changed, re-rendered under other net names.

| Cells | Patch | Time (8 threads) |
|---|---|---|
| 115 thousand | 3 cells added, 3 removed | 1.3 s |
| 1.15 million | 3 cells added, 3 removed | 18 s |

## Tests

`tests/meco.cmake` runs the cases in `tests/eco` (scan against no scan, a
deep one-gate change, added, removed and replaced registers, a hierarchical
netlist, a memory, spare cells) and checks the patch, the proof, that a second run on the
patched netlist finds nothing to change, and that one thread and four give
the same files. `tests/eco/fuzz.py` makes random netlists and random edits,
and also simulates each patched netlist against the new one in `msim`.

## Limits

- `-new` is a gate netlist. RTL as the revised design waits for the
  synthesis tool; today, synthesize the new RTL first.
- A hierarchical old netlist is patched and written back flat.
- An old net is reused when it has the same function, not when it has the
  inverse.
- Registers are matched by name only.
- `-spare` places gates but not registers on spare cells, and does not
  look at placement when it picks one.
