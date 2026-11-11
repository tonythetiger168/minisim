# mchar: cell library characterization

`mchar` measures a standard cell library from its transistor netlists and
writes the Liberty file, in the manner of PrimeLib, SiliconSmart or
Liberate. It simulates on `mspice` ([SPICE.md](SPICE.md)) and reads and
writes Liberty with the shared reader (`src/liberty.hpp`), so what it
writes is what the timing, power and synthesis tools read.

```
mchar -lib template.lib -spice cells.sp -models corner.sp -o out.lib
mchar ... -cell DFF_X1 -cell 'NAND*'      only these cells; the others keep the template's numbers
mchar ... -keep leakage                   leave a kind of number as the template has it
mchar ... -threads 8                      worker threads; the default is every logical CPU
```

The exit status is 0 when every table was measured, 1 when some could not be
(each is reported and keeps the template's values), 2 for bad input.

## Inputs

**The template** is a Liberty file that says what to measure: the cells,
their pins and `function`s, `ff` and `latch` groups, the `timing` and
`internal_power` groups with their table templates and indexes. The numbers
in its tables are placeholders. A characterized library is a valid template,
so `mchar -lib lib.lib -o lib.lib` measures a library again. Units, slew and
delay thresholds, the supply voltage and the temperature come from the
template (`operating_conditions`, else `nom_voltage` and `nom_temperature`).

**The netlists** are one `.subckt` per cell, named as the cell, with the
cell's input pins, then its output pins, in the template's order, then `vdd`
and `vss`. `-models` files are included in front (device models, one set per
corner). A cell without a subcircuit keeps the template's numbers.

## What is measured

| Liberty | How |
| --- | --- |
| `cell_rise`, `cell_fall`, `rise_transition`, `fall_transition` of `combinational`, `rising_edge`, `falling_edge`, `preset` and `clear` arcs | one transient per table point: the related pin ramps with the index slew into the cell loaded with the index capacitance; delay between the input and output thresholds, transition between the slew thresholds |
| `rise_constraint`, `fall_constraint` of `setup_*`, `hold_*`, `recovery_*`, `removal_*` | a bisection per table point on the offset between the pin's edge and the clock's: the smallest offset at which the cell still captures the right value and its clock-to-output delay is no more than 10% longer (`-degrade`) than with the pin far away (`-range`, 1 ns) |
| `rise_power`, `fall_power` of `internal_power` | from the same transient as the delay: the energy drawn from the supply during the event, less leakage, less the energy delivered to the loads of the outputs that rose; shared equally by the outputs that move together and have a group for that pin |
| `rise_power`, `fall_power` of an input pin's `internal_power` (no `related_pin`, a table over the input slew) | an edge on the pin in a state where no output moves and the stored state stays (the clock with the data equal to the output; an input masked by another): the energy drawn from the supply, less leakage. The charge on the pin itself comes from its driver and is not counted. One of the two edges can be negative, when the edge pushes charge back into the supply; the pair sums to the real loss. An output arc's table on the same related pin then holds what the event costs beyond this, because a power tool adds the two |
| `cell_rise`, `cell_fall` and the transitions of a `three_state_enable` arc | the output starts undriven, with the other level left on its load by an earlier step of the stimulus, and the enable pin's edge makes the cell drive it; either edge of the pin is tried |
| `min_pulse_width_high`, `min_pulse_width_low` of a pin | bisection on the width of a pulse (between the input thresholds, edges of four times the smallest table slew): the narrowest one after which the cell ends where a wide pulse leaves it, with the output no more than 10% later; never less than the edge time, where the pulse just reaches the rail |
| `capacitance` (and `rise_capacitance`, `fall_capacitance` where the template has them) | the charge a ramp on the pin delivers, over the supply voltage; the mean of rising and falling |
| `leakage_power`, `cell_leakage_power` | the supply current at rest, averaged over the input states each `when` allows |

A `non_unate` arc is measured for both edges of the related pin and each
table point takes the slower one. Outputs other than the one measured carry
the smallest load of the table.

Kept from the template, and counted in the summary: `three_state_disable`
arcs, `min_pulse_width` timing groups and other timing types,
timing groups with several related pins, and everything that is not a
number mchar measures (areas, functions, wire loads, `max_capacitance`).

## Finding the stimulus

Nothing but the Liberty description tells mchar how to exercise a cell. It
models the cell from `function`, `three_state`, `ff` (`clocked_on`,
`next_state`, `clear`, `preset`) and `latch` (`enable`, `data_in`), and
searches breadth-first over single-pin changes, from every input low, for
the shortest sequence that ends in the wanted event:

- for a delay arc, a state where the related pin's edge moves the output the
  right way (and the arc's `when` holds);
- for a constraint, a state where the clock edge captures something that
  depends on whether the constrained pin changed first, preferring one where
  meeting the constraint shows as an output edge to time;
- for a tristate enable arc, a state where the output is undriven and still
  holds the level opposite to the one the cell is about to drive;
- for a pulse width, a state where a pulse on the pin leaves an output
  changed;
- for input pin power, a state where the pin's edge changes no output and
  not the stored state.

The search runs on both power-up values of the state at once and accepts a
sequence only when both give the same result, so the first steps of a
sequence initialize the flop or latch. Each step waits `-settle` (0.5 ns)
before the next, and a loaded output is given longer until it is at rest.
For an arc that can be exercised in several states (the other inputs of an
AOI, say) the first state found is used.

## Accuracy and speed

- Each simulation runs with `reltol=1e-4 chgtol=1e-17 trtol=1`: the default
  charge tolerance is far above the charge on a femtofarad node. A window
  that ends before the output has settled is run again, three times longer.
- `tests/char/rc.sp` is a cell with a known answer (an ideal inverter behind
  10 kohm): mchar gives R C ln 2 and R C ln 4 to 0.1%.
- Every table point is an independent task. They run on every logical CPU
  (`-threads N`, `-threads 1` for none) and are stored by index, so the
  output is byte for byte the same on any thread count; the test checks it.
- mini45 (88 cells, 412 delay tables, 150 constraint tables, 388 power
  tables: 23,000 measurements) takes about 3 minutes per corner on a 4-CPU
  machine.

## Not there yet

- Tristate disable arcs (they need a resistive load and a threshold the
  template does not give), minimum period.
- State-dependent tables that the template does not ask for with `when`
  (the worst case over side inputs); CCS and ECSM current-source models;
  on-chip variation (LVF) tables.
- Noise and signal-integrity views.
