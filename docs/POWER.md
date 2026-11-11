# Power analysis: `mpower`

`mpower` reports the power of a design from its switching activity: average
power per group, per instance and per cell, peak power over time, and power
per UPF domain. It works on a gate-level netlist with a Liberty library, on
RTL (as an estimate), and on a mix of both.

```sh
# gate level: netlist + library + activity from a simulation
msim cells.v netlist.v tb.sv -top tb -saif run.saif
mpower netlist.v -top dut -lib cells.lib -activity run.saif -strip_path tb.dut

# RTL estimate, with peak power and a power-over-time trace
msim rtl.sv tb.sv -top tb -fst run.fst
mpower rtl.sv -top dut -lib cells.lib -activity run.fst -peak -waveform power.csv

# no simulation at all: a clock period and default toggle rates
mpower netlist.v -top dut -lib cells.lib -clock clk 10
```

`-top` is the design to analyze (the DUT, not the testbench). `-strip_path`
is where that design sits in the activity file (`tb.dut` or `tb/dut`); when it
is left out, mpower looks for the top module's own nets in the file and uses
the scope it finds them under.

## Activity

| Source | How | Notes |
|---|---|---|
| SAIF 2.0 | `msim -saif run.saif`, or any simulator's backward SAIF | per bit: time at 0, at 1, at x, toggle count. Smallest file; no times, so no `-peak` |
| VCD | `msim -vcd run.vcd` | every value change; needed for `-peak` |
| FST | `msim -fst run.fst` | as VCD, much smaller |
| none | `-clock <net> <period ns>` | the clock toggles twice a period; inputs and flops toggle `-default_toggle_rate` times a cycle (0.1) |

`mpower -vcd run.vcd -write_saif run.saif` (no design) converts a waveform to
SAIF. `msim -saif` does the same after a simulation, and with no `-vcd` or
`-fst` it does not keep the waveform.

Nets the activity file does not name get activity by propagation: the
probability and the transition density of each cell output are computed from
its inputs through the cell's Liberty `function`, flops take theirs from
`next_state` and their clock, and a clock gate passes its clock while its
enable is high. So activity from an RTL simulation works on the gate
netlist made from it: ports and registers match by name (the flop
`q_reg[3]` or `q_reg_3_` holds the RTL signal `q[3]`) and the logic in
between is propagated. The report says what share of the nets was annotated
directly.

A toggle is a change between 0 and 1. A change through x counts once the
bit is known again and differs from its last known value. Two changes of one
signal at the same time count as one (no zero-delay glitches).

## What is added up

Per cell, with V the supply voltage, C the load on a net and r its toggle
rate (toggles per second):

- **Switching** `0.5 * C * V^2 * r` for each net, counted at the cell that
  drives it. C is the `capacitance` of every pin on the net plus the wire:
  the net's total capacitance in a `-spef` file (IEEE 1481, as `mrcx` writes
  it), else `-wire_cap <fF>` per fanout, else the library's
  `default_wire_load`, else 0.
- **Internal**, from the `internal_power` tables. For an output pin: the
  energy of a rise and of a fall, looked up by input transition and output
  load, averaged over the groups by how often each is the cause (the toggle
  rate of its `related_pin` times the probability of its `when`), times r/2
  each. For an input pin (the clock pin of a flop): the same from its own
  groups, by its own toggles. Energy unit: `capacitive_load_unit` times
  `voltage_unit` squared.
- **Leakage** each `leakage_power` group's value times the probability of
  its `when`, and `cell_leakage_power` for the rest of the time.

Probabilities assume the pins of a cell are independent. Transition times
come from the library's `rise_transition` / `fall_transition` tables in three
passes from `-input_transition` (0.05 ns) at the inputs; this is not a
timing analysis. `-voltage` overrides `nom_voltage`: dynamic power is scaled
by (V/Vnom)^2 and leakage by V/Vnom, which is a first-order correction; a
library characterized at that voltage is better.

Groups: `clock_network` (clock nets, and the buffers, inverters and clock
gates that drive clock pins), `register` (flops, with their clock pins),
`sequential` (latches), `combinational`, `memory`, `primary_inputs` (the
switching of nets the design's inputs drive). A module that is in no library
and has no source is a black box: it is counted and listed, not costed.

## RTL estimate

RTL has no cells, so mpower costs it with two cells of the library: each
register bit as a flop (`-rtl_ff`, default the smallest plain flop) on the
clock in its `always_ff`, and each block of logic as a number of reference
gates (`-rtl_gate`, default the smallest NAND2) estimated from its operators:
adders, multipliers, comparators, shifters, muxes for `?:`, `if` and `case`,
mux trees for variable indexes. A `for` loop counts once per iteration.
Logic nothing reads is dropped, as synthesis would. An unpacked array of
more than `-rtl_mem_bits` (4096) bits written under a clock is a memory, not
flops: its leakage and the clocking of about one word are counted. Without
`-lib` a built-in generic 1.0 V technology is used.

Because each flop is costed from the activity of its own clock net, a
register behind a clock gate pays only for the gated clock's toggles. That
makes the estimate useful for what it is meant for: comparing two versions
of the RTL, or finding which block is worth gating.

Measured on Ibex (lowRISC, RV32IMC, the msim benchmark): 2,361 flops and
about 53,000 gates estimated, against roughly 25,000 to 40,000 gate
equivalents for synthesized Ibex configurations. Treat absolute RTL numbers
as good to about a factor of two, and synthesize for a number to sign off.

## Peak power

`-peak` bins the energy of every toggle (its net's switching energy and the
internal energy of the pins on the net) into windows of `-interval <ns>`
(default one period of the fastest clock) and adds leakage. It prints the
highest window and its time; `-waveform file.csv` writes every window
(`time_ns,power_W`). Activity that was propagated has no times and is spread
evenly. The windows average to the reported total, apart from toggles at the
very last time of the waveform, which fall in no window.

## UPF

`-upf file.upf` (IEEE 1801, the reader `msim -upf` uses) adds a table per
power domain:

- instances are assigned to domains as in simulation (`create_power_domain
  -elements`, nearest enclosing element);
- a domain's voltage is the highest voltage its primary supply has in
  `add_power_state` / `add_port_state`, and its cells are computed at it;
- a domain behind `create_power_switch` is on for the share of time its
  `-on_state` expression is true in the activity file. Its leakage is counted
  for that share only, and so is the internal power of inputs that keep
  toggling while it is off (its clock). Outputs do not toggle while it is
  off in a power-aware simulation, so their power is already in the activity.

`-power_top` names the instance the UPF was written for when it is not the
top. Isolation, level-shifter and retention cells that the UPF implies but the
netlist does not yet contain are not added.

## Options

```
-top <module>            the design to analyze
-lib <file.lib>          Liberty library; may repeat (the first one that has a cell wins)
-activity <file>         .saif, .vcd or .fst (also -saif, -vcd, -fst)
-strip_path <path>       the design's instance in the activity file
-clock <net> <ns>        a clock and its period, for nets without activity
-default_toggle_rate <r> toggles per clock cycle where nothing else is known (0.1)
-voltage <V>   -input_transition <ns>   -wire_cap <fF per fanout>
-spef <file>            extracted wire capacitance (nets not in it keep the estimate)
-upf <file>  -power_top <inst>
-peak  -interval <ns>  -waveform <file.csv>
-hier <n>                power per instance, n levels deep
-cells <n>               the n cells that draw the most
-json <file>             the whole report, in watts
-write_saif <file>       the activity as SAIF
-rtl_ff <cell>  -rtl_gate <cell>  -rtl_mem_bits <n>
-unit W|mW|uW|nW|pW      -q (only the total line)
-threads <n>             default: every logical core; the report is the same for any n
-f, +define+, +incdir+, -y, -v   as msim
```

The last line of the output is always
`total power X uW (internal .., switching .., leakage ..)`.

## Speed

mpower uses every logical core by default (`-threads 1` is serial) and gives
the same report, digit for digit, on any thread count: the waveform is read
in parallel, each signal is summed and binned on its own, each cell is
computed on its own, and sums are taken in a fixed order. Measured on a
4-vCPU cloud VM:

| Design | Activity | 1 thread | 4 threads |
|---|---|---|---|
| Ibex RTL, 118,745 cycles | FST, 27 MB, 40.6M value changes, `-peak` with 118,746 windows | 9.5 s | 3.2 s |
| 200,000 cells, gate level (`bench/gen_power.py`) | VCD, 8 MB, `-peak` | 1.5 s | 1.5 s |
| 1,000,000 cells, gate level | VCD, 41 MB, `-peak` | 8.8 s | 8.0 s |

On Ibex the time is the waveform (reading and summing it scales with
threads). On the gate netlists about 70% is the slang front end parsing and
elaborating the netlist on one thread, which the other tools share; the
power computation itself is 0.4 s for a million cells on one thread and
0.15 s on four.

## For other tools

`src/power.hpp` is the computation: `pwr::analyze(options, slang-args,
threads, result)` returns totals, groups, domains, instances and the power
trace in watts. `src/saif.hpp` is the SAIF reader and writer and the per-bit
activity (`saif::load` takes SAIF, VCD or FST; `saif::Db::find("tb.dut.q[3]")`
gives T0, T1, TX and the toggle count). Waveforms come through
`src/wave_reader.hpp` and cells through `src/liberty.hpp`.

## Not yet

- SDC clocks (`create_clock`) for the clock network and vectorless rates.
- Glitch power (SAIF `IG`), state-dependent path-dependent SAIF, and
  per-arc exact evaluation from the waveform instead of probabilities.
- Isolation, level-shifter and retention cells implied by UPF.
- `msim` does not dump unpacked arrays, so their bits are never annotated.
