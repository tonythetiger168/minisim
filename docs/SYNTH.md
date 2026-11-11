# Logic synthesis: `msynth`

`msynth` turns RTL into a gate netlist of library cells, driven by a Tcl
shell whose commands follow Design Compiler's `dc_shell`. It uses the same
slang front end as `msim`, so a design that simulates also elaborates here.

```sh
msynth -f run.tcl                          # run a script
msynth                                     # interactive: msynth> prompt
msynth rtl/*.sv -top core -period 2 -o core_net.v    # one step
```

## A script

```tcl
set target_library my_cells.lib            ;# Liberty (.lib). None: mini45 (lib/mini45)
analyze -format sverilog {rtl/alu.sv rtl/core.sv}
elaborate core -parameters "XLEN=32"

create_clock -name clk -period 2.0 [get_ports clk]
set_input_delay  0.3 -clock clk [remove_from_collection [all_inputs] [get_ports clk]]
set_output_delay 0.3 -clock clk [all_outputs]
set_load 0.01 [all_outputs]

compile
report_area   > area.rpt
report_timing > timing.rpt
report_qor
write -format verilog -output core_net.v
write_sdc core.sdc
```

## What it does

1. **Elaborate** (`src/aig_elab.hpp`). slang elaborates the design; the
   hierarchy is flattened and every process is run symbolically into an
   and-inverter graph with flops (`src/aig.hpp`). Loops are unrolled,
   functions and tasks inlined, memories become flops and multiplexers.
   Adders are ripple-carry below 8 bits and parallel-prefix (Sklansky) from
   8 bits up; multipliers are carry-save arrays; dividers are restoring.
   Structural hashing shares equal logic as it is built.
2. **Map** (`src/synth/map.hpp`). For every node the 4-input cuts are
   listed with their truth tables, and each is matched against the library
   by function, with any input or the output inverted. Each node keeps its
   best cover in both polarities, by area flow, or by arrival time when the
   area result misses the clock. Flops are chosen by the library's `ff`
   groups; a missing variant (falling edge, preset) is built from another.
3. **Buffer**. Nets with more than `set_max_fanout` loads (12 by default)
   get a buffer tree. Clock nets are left alone.
4. **Time** (`src/synth/sta.hpp`). Arrival times with the library's NLDM
   tables, rise and fall apart, against the clock period, input and output
   delays, output loads and clock uncertainty. Clocks are ideal.

The levels of the graph are mapped in parallel on every core
(`-threads N` or `set_host_options -max_cores N`; `-threads 1` turns it
off). The netlist is the same for any thread count.

## Commands

| Group | Commands |
|---|---|
| Read | `analyze`, `read_verilog`, `read_sverilog`, `read_file`, `elaborate top [-parameters "N=8,W=4"]`, `current_design`, `check_design` |
| Library | `set target_library {a.lib}`, `read_lib`, `set_dont_use`, `report_lib`, `write_lib_models cells.v` |
| Constraints | `read_sdc file.sdc` and every SDC 2.1 command directly, read by the shared reader (`src/sdc.hpp`). The timing estimate uses `create_clock`, `set_input_delay`, `set_output_delay`, `set_load`, `set_clock_uncertainty`, `set_input_transition`, `set_max_fanout`, `set_max_area`; the rest (exceptions, generated clocks, latencies) is checked, kept and written by `write_sdc` |
| Objects | `get_ports`, `get_cells`, `get_clocks`, `all_inputs`, `all_outputs`, `all_clocks`, `all_registers`, with `-filter` and wildcards; collections: `sizeof_collection`, `foreach_in_collection`, `get_object_name`, `add_to_collection`, `remove_from_collection`, `filter_collection` |
| Synthesis | `compile [-map_effort high]`, `compile_ultra` |
| Reports | `report_resources` (word-level operators and memories with their ports, before `compile`), `report_area`, `report_timing [-max_paths N]`, `report_qor`, `report_cell`, `report_reference`, `report_constraint -all_violators` |
| Write | `write -format verilog -output f`, `write_verilog f`, `write_sdc f` |
| Shell | `redirect file {cmd}`, `cmd > file`, `cmd >> file`, `set_app_var`, `set_host_options -max_cores N`, `source`, `help`, `exit`, and Tcl (`set`, `expr`, `if`, `foreach`, `for`, `while`, `proc`, lists, strings, `format`) |

Object queries return Tcl lists of names, so they combine with `foreach`,
`llength` and `lsearch`. Many other dc_shell commands (`link`, `uniquify`,
`set_wire_load_model`, `set_false_path`, ...) are accepted and do nothing,
so existing scripts run. `.db` and `.ddc` are proprietary formats: give the
Liberty source and write Verilog.

## The netlist

One flat module. Ports keep their RTL names and ranges; every connection is
by name, to a scalar net, a port bit, or `1'b0` / `1'b1`. Flops are named
after their RTL variable, `u_core_pc_reg_3_` for bit 3 of `u_core.pc`.

`write_lib_models cells.v` writes Verilog models of the library's cells from
their Liberty functions, so the netlist can be simulated:

```sh
msim core_net.v cells.v tb.sv -top tb
```

## The default library

With no target library, msynth maps to mini45, the tool line's cell library
(`lib/mini45/mini45_tt.lib`, see `docs/MINI45.md`), so that a netlist goes
on to timing, power, place and route and extraction with the same cells.
msynth finds it next to an installed `msynth`
(`share/minisim/lib/mini45`), in the source tree of a build-tree `msynth`,
or in the directory named by `$MINISIM_MINI45`. For simulation use
`lib/mini45/mini45.v`, or `write_lib_models`.

## For other tools: the shared front half

`src/aig.hpp` and `src/aig_elab.hpp` (namespace `aig`) are the RTL-to-graph
half that the equivalence, formal and FPGA tools share. Beside the graph,
`aig::Design` keeps what a mapper with richer cells needs:

- `ops`: every adder, subtractor, multiplier, comparator and shifter with
  its operand and result bits (`aig::WordOp`), for carry chains, DSP blocks
  and datapath cells.
- `mems`: every memory (an unpacked array written through a dynamic index in
  a clocked block) with its write ports (clock, enable, address, data, byte
  lane) and read ports (address, data) (`aig::Memory`). The graph still
  holds the memory as flops and multiplexers; a tool with a RAM drops those
  flops (`Flop::var` is the memory's name) and drives the read data itself.
  Later write ports win over earlier ones on the same word.
- `aig::flopEnable` and `aig::flopSyncReset` find the enable and the
  synchronous reset in front of a flop.

## Limits

- Not synthesized: latches (an incompletely assigned `always_comb` is an
  error that names the variable), tristates and `inout` ports, flops with
  both asynchronous set and reset, real numbers, dynamic types.
- Ignored with a warning: delays, `initial` blocks. Ignored silently:
  system tasks and assertions.
- `unique case`, `priority case` and `(* full_case *)` without a default
  are taken as covering every value.
- One clock is timed; `set_false_path` and `set_multicycle_path` are not
  applied. `msta` is the sign-off timer.
- Library cells with more than 4 inputs or more than one output (adders,
  scan and enable flops) are not used by the mapper.
- The hierarchy is always flattened.

## Tests

`tests/synth`: each design is synthesized and its testbench is simulated
with `msim` on the RTL and on the netlist with random stimulus; both runs
must print the same lines. This is done with the default library and with a
library of only an inverter, a NAND and one flop.
