# Static timing analysis: `msta`

`msta` times a gate-level netlist against its constraints, the way PrimeTime
does: Liberty libraries, a structural Verilog netlist and SDC in; timing
reports, constraint checks and SDF out. It needs no simulator front end and
builds in the plain CMake build.

```sh
msta -lib cells.lib netlist.v -top top -sdc top.sdc     # the default reports
msta -lib cells.lib netlist.v -sdc top.sdc -spef top.spef   # with parasitics
msta -lib cells.lib netlist.v -sdc top.sdc -sdf top.sdf -json top.json
msta -f run.tcl                                          # a script
```

Without a script, `msta` prints `check_timing`, the worst setup and hold path
of every clock (`-max_paths N` for more), and the constraint summary.

## Scripts

A script is Tcl, read by the same reader as the SDC files (`src/sdc.hpp`), so
SDC commands, variables, `if`, `foreach`, `proc` and collections all work.

```tcl
read_liberty cells.lib
read_verilog netlist.v
link_design top
read_sdc top.sdc                 ;# or the SDC commands directly
check_timing -verbose
report_timing -delay_type max -max_paths 10 -transition -capacitance
report_timing -delay_type min -to [get_pins u_core/r_state_reg*/D]
report_constraint -all_violators > violations.rpt
if {[worst_slack -max] < 0} { puts "setup fails: TNS [total_negative_slack -max]" }
write_sdf top.sdf
```

| Command | What it does |
|---|---|
| `read_liberty FILE` (`read_lib`) | Reads a Liberty library. The first one sets the units of the reports and of the SDC values. |
| `read_verilog FILE` | Reads a structural Verilog netlist. |
| `link_design [TOP]` (`link`) | Flattens the hierarchy below `TOP` and binds cells to the libraries. |
| `read_sdc FILE` | Reads constraints. |
| `read_spef FILE [-corner min\|typ\|max]` (`read_parasitics`) | Back-annotates parasitics: wire capacitance and Elmore wire delays. |
| `check_timing [-verbose]` | Unclocked registers, unconstrained endpoints, inputs with no delay, combinational loops, registers with several clocks, ignored exceptions, cells with no library. Returns the count. |
| `report_timing` (`report_checks`) | Paths. `-delay_type max\|min`, `-max_paths N` (per path group), `-group CLOCK`, `-from`, `-through`, `-to`, `-slack_lesser_than X`, `-path full\|end`, `-input_pins`, `-transition`, `-capacitance`, `-significant_digits N`. |
| `report_constraint [-all_violators]` | WNS, TNS and violation counts per clock for setup and hold, `max_transition` and `max_capacitance` violations. Returns the count. |
| `report_clocks`, `report_qor`, `report_units` | Clock table; logic depth, critical path, slack and cell counts; units. |
| `worst_slack [-max\|-min]`, `total_negative_slack [-max\|-min]` | Numbers for scripts. |
| `write_sdf FILE` | SDF 3.0: `IOPATH` delays as `(min::max)` for rise and fall, `SETUP`, `HOLD`, `RECOVERY`, `REMOVAL`. `msim -sdf` reads it. |
| `write_json FILE [-max_paths N]` | The summary and the worst paths, point by point. |
| `set_threads N` | Worker threads from here on. |

A report command takes `> FILE` or `>> FILE` at the end.

## What is modeled

- **Delay calculation**: NLDM. Cell delays and output slews come from the
  library tables at the input slew and the driven capacitance (pin
  capacitances, `set_load` on ports and nets), with linear interpolation and
  extrapolation. Of several `when` variants of an arc the slowest is used for
  late analysis and the fastest for early. Slews are merged per pin.
- **Parasitics**: `read_spef` (through the shared `src/spef.hpp`) gives each
  annotated net its wire capacitance and each driver to load arc the Elmore
  delay of the RC network, load pin capacitances included; the slew at the
  load is the driver's slew and ln 9 times the wire delay, root sum of
  squares.
- **Clocks**: `create_clock` (waveforms, virtual clocks),
  `create_generated_clock` (`-divide_by`, `-multiply_by`, `-edges`,
  `-edge_shift`, `-invert`, `-duty_cycle`), ideal by default with
  `set_clock_latency` (network and `-source`), `set_clock_transition`,
  `set_clock_uncertainty` (per clock and `-from`/`-to`), and
  `set_propagated_clock`, which times the clock network cell by cell. A
  generated clock of a propagated master starts with the master's arrival at
  its source pin.
- **Checks**: setup, hold, recovery and removal from the library constraint
  tables, on rising and falling clock edges; `set_output_delay` at ports.
  The setup capture edge is the first one after the launch edge, between
  clocks of different periods the closest pair over their common period; the
  hold edge is one capture period earlier.
- **Exceptions**: `set_false_path`, `set_multicycle_path` (`-setup`, `-hold`,
  `-start`, `-end`), `set_max_delay`, `set_min_delay`, with `-from`,
  any number of `-through` and `-to`, on clocks, ports, pins, cells and nets,
  and `-rise_from`/`-fall_from`/`-rise_to`/`-fall_to`. `set_clock_groups`
  cuts the paths between groups.
- **Ports**: `set_input_delay`, `set_output_delay` (`-min`, `-max`, `-rise`,
  `-fall`, `-clock_fall`, `-add_delay`), `set_input_transition`,
  `set_driving_cell`, `set_load`.
- **Design rules**: `max_transition` and `max_capacitance` from the library
  pins, `default_max_transition`, and `set_max_transition` /
  `set_max_capacitance`.
- **Constants**: pins tied to `1'b0` / `1'b1`, `set_case_analysis` and
  `set_logic_zero` / `set_logic_one` are propagated through the cell
  functions. A constant pin has no arcs, an arc that the constants on the
  other inputs block is removed, and one they make unate (an XOR with one
  input constant) keeps only that direction.
- `set_disable_timing` on cells (`-from`, `-to`), library cells, pins and ports.

## Threads

Delay calculation, arrival propagation and the checks run level by level on
every logical core by default. `-threads N` sets the number of threads and
`-threads 1` turns them off. The results are the same with any number: the
test suite compares the reports, the SDF and the JSON of a 20,000 cell design
at 1, 2 and 8 threads.

## Compared with OpenSTA

msta is checked against [OpenSTA](https://github.com/parallaxsw/OpenSTA)
3.2.0, run as a separate program (it is GPL; none of its code is in minisim).
The designs are random netlists from `tests/sta/gen_netlist.py` on the mini45
typical library: two propagated clocks of periods 4 and 6 with paths between
them, clock uncertainty, input and output delays, a false path and multicycle
paths. `bench/sta/opensta.tcl` runs OpenSTA and
`bench/sta/compare_opensta.py` compares the slack at every endpoint.

| Cells | Endpoints | Setup slack, largest difference | Hold slack, largest difference |
|---|---|---|---|
| 20,000 | 1,551 | 0.05 ps | 0.05 ps |
| 200,000 | 15,645 | 0.06 ps | 0.06 ps |
| 1,000,000 | 77,000 | 0.10 ps | 0.09 ps |

Every endpoint is in both reports, and the differences are the rounding of
OpenSTA's four-digit report. Time from start to the worst setup and hold
slack (read the library, netlist and constraints, then a full update), on a
4-core Xeon at 2.1 GHz:

| Cells | OpenSTA, 1 thread | OpenSTA, 4 threads | msta, 1 thread | msta, 4 threads | Peak memory (OpenSTA / msta) |
|---|---|---|---|---|---|
| 20,000 | 0.56 s | 0.45 s | 0.15 s | 0.12 s | 77 / 49 MB |
| 200,000 | 6.5 s | 4.9 s | 1.4 s | 1.2 s | 471 / 431 MB |
| 1,000,000 | 38.3 s | 29.6 s | 7.1 s | 6.0 s | 2.2 / 2.1 GB |

At a million cells msta's analysis itself (delays, arrivals, checks) is 0.5 s
on 4 threads; reading and flattening the netlist and building the graph are
the other 5 s. Writing a path for each of the 77,000 endpoints (`-max_paths`
with `-json`) adds 14 s and is the next thing to speed up.

The comparison found two differences, both fixed:

- The hold check between clocks of different periods used the capture edge
  one period before the setup one. It now takes, over the common period, the
  tightest of the two hold checks of every setup relation (the launch edge
  against the capture edge before, and the next launch edge against the
  capture edge).
- Asynchronous set and reset to output arcs (`timing_type` `preset` and
  `clear`) were timed as paths and their slew was merged into the output's.
  They are now off by default, as in PrimeTime and OpenSTA;
  `-preset_clear_arcs` turns them on. Recovery and removal checks are not
  affected.

## Limits

These are not done yet, and each is either reported or documented here:

- With SPEF the driver is still timed into the total capacitance (no
  effective capacitance), coupling capacitors count once to ground (no
  crosstalk delay), and an RC network with loops is timed along a tree.
  Without SPEF nets are ideal apart from `set_load`; there are no wire load
  models.
- Latches are timed as registers at the edge of their timing arcs: a latch's
  `D` is an endpoint and its `D` to `Q` arc is not a path (no time borrowing).
- On-chip variation: no `set_timing_derate`, no clock reconvergence pessimism
  removal. One library corner per run.
- `report_timing` shows the single worst path into each endpoint; `-nworst`
  above 1 and path-based analysis are not there. `-from` and `-through`
  therefore select among those worst paths.
- A `set_max_delay` path into a register with no clock is not timed.
- Path exceptions that name pins in `-from` or `-through` use one of 256
  internal state bits per stage; past that an exception is ignored with a
  warning.
- CCS and ECSM tables, crosstalk, and minimum pulse width / period checks.

## Files

| File | |
|---|---|
| `src/liberty.hpp` | Liberty reader (shared with the synthesis, power and equivalence tools) |
| `src/gnetlist.hpp` | Gate netlist reader, flattener and writer (shared) |
| `src/sdc.hpp` | SDC / Tcl reader (shared) |
| `src/sta.hpp` | The engine: graph, delay calculation, arrivals, checks |
| `src/sta_report.hpp` | Reports, SDF and JSON |
| `src/sta_spef.hpp` | SPEF back-annotation (the reader is the shared `src/spef.hpp`) |
| `src/parallel.hpp` | The thread pool shared by the tools |
| `src/msta.cpp` | The command line and the script commands |
| `tests/sta/`, `tests/msta.cmake` | A small library with linear tables and designs whose expected numbers are worked out by hand |
