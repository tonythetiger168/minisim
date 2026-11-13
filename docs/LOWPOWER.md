# Low-power simulation: UPF (IEEE 1801-2024)

`msim -upf` simulates a design with its power intent, written in UPF
(IEEE Std 1801-2024, UPF 4.0). `mlint -upf` checks the power intent against
the design without simulating. Clause numbers below are 1801-2024.

```sh
msim tb.sv dut.sv -top tb -upf dut.upf -power_top u_dut     # power-aware simulation
msim tb.sv dut.sv -top tb -upf dut.upf -upf_verbose         # print supply / state changes
mlint tb.sv dut.sv -top tb -upf dut.upf                     # static power-intent checks
```

`-power_top` names the instance the UPF file was written for, as a path from
the simulation top (`u_dut`, `tb/u_dut` or `tb.u_dut`). Without it, msim
uses the UPF's `set_design_top` (a module name is looked up in the design),
and otherwise the simulation top. Everything outside the power top's
domains (normally the testbench) is always on.

## Driving supplies from the testbench

With `-upf`, msim adds the SystemVerilog package `UPF` (11.2.4). The
testbench imports it and turns supplies on and off:

```systemverilog
import UPF::*;
initial begin
  supply_on("VDD", 1.0);        // FULL_ON at 1.0 V
  supply_on("VSS", 0.0);
  ...
  supply_off("VDD_CORE");       // a root supply only: a switch output is driven by its switch
  supply_partial_on("VDD", 0.9);
  void'(set_supply_state("VDD", FULL_ON));
  void'(set_power_state("PD_CORE.primary", "OFF"));
  sv = get_supply_value("VDD"); // upfSupplyTypeT: state and voltage in microvolts
end
```

Supply names are UPF names relative to the power top (`VDD`,
`u_core/VDD_SW`, `PD_CORE.primary.power`); a leading `/` and leading
instance names such as `tb/u_dut/` are accepted too. Every supply starts
`OFF` (9.2.1), so every domain starts corrupted until its supplies are
turned on.

## What is simulated

| UPF | msim |
|---|---|
| `create_power_domain` (`-elements`, `-include_scope`, `-exclude_elements`, `-supply`) | each instance belongs to the domain with the closest element above it |
| `create_supply_port`, `create_supply_net` (`-domain`, `-resolve`), `connect_supply_net`, `create_supply_set`, `associate_supply_set`, `set_domain_supply_net` | supply subnets with a state (`OFF`, `FULL_ON`, `PARTIAL_ON`, `UNDETERMINED`) and a voltage (9.2.3) |
| `connect_supply_net` to an HDL port, `-vct`, `create_hdl2upf_vct`, `create_upf2hdl_vct` (4.10, Annex B) | a supply net connected to a one-bit port of the design that is not a UPF supply port: an output port drives the net (its logic value becomes the supply state), an input port is driven by the net (held at the converted state). The twelve predefined tables of Annex B (`SV_LOGIC2UPF`, `UPF2SV_LOGIC`, the `_GNDZERO` and `VHDL_` ones, `SV_TIED_HI`, ...) and tables of the power intent's own. Without `-vct`: `SV_LOGIC2UPF` for an output, `UPF2SV_LOGIC` for an input |
| `find_objects` (6.33) | instances (`-object_type inst`, the default), instances by module (`model`), ports with `-direction`, nets, and the UPF's own `supply_port` / `supply_net`, below a scope; `-transitive`, glob (`*` `?`; brackets are part of a name such as `d[3]`), `-regexp`, `-exact`, `-ignore_case`, `-leaf_only`, `-non_leaf`. The result is a Tcl list of names relative to the current scope, so it feeds `foreach`, `-elements` and the other commands. `set_design_top` places the power top as it is read, so queries see the right instance |
| `create_vcm`, `connect_supply_net -vcm` (4.10, 6.14, 6.30) | value conversion methods. Table VCMs for one-bit (`bit`, `logic`, `std_logic`, `boolean`), real, integer and enum ports, both directions; `-field` converts one field of a packed or unpacked struct port (`a.b` for a nested struct), and a struct input can take one upf2hdl VCM for each field, the other fields staying `x` until their VCM drives them. Every check of 6.30.3 to 6.30.7 (ranges, overlaps, rows for every state or value). hdl2upf: the first row whose range holds the HDL value sets the state, and the voltage when the row gives one (`*`: the HDL value in volts); no row: UNDETERMINED. upf2hdl: the row for the net's state (and voltage) sets the port, `*` the voltage in volts, `Xreal` / `Zreal` the package values; no row is a run-time error. `-vcms` lists are picked from per port by type and direction (4.10.2). A port of type `upfSupplyTypeT` (`supply_net_type`) connects with no VCM. The Annex B tables and the legacy VCTs are VCMs too. Function and model VCMs (6.30.8, 6.30.9): msim checks the function's or module's signature (one input, one output, exactly one side `upfSupplyTypeT`, the HDL side of the port's type), then binds a small module into the simulation top that calls the function, or instantiates the model with `-parameters`, on the port; the HDL side follows the language's own semantics. With `-upf_verbose` the generated SystemVerilog is printed |
| Supply ports by pg_type (4.5.4.6.2, 6.14, 6.15) | a port's pg_type is its `UPF_pg_type` attribute, from `set_port_attributes -pg_type` or the HDL. `connect_supply_net -pg_type` with `-cells`, `-domain` or `-elements` connects the net to every such port of the selected instances below the scope; `connect_supply_set -connect {function pg_type}` does the same for its elements; and each function of a domain's primary supply set goes automatically to the ports of the default pg_type (`power`: `primary_power`, `ground`: `primary_ground`, the bias functions their own names) of the instances in its extent. A port keeps the first of these that reaches it, in that order, after any `-ports` connection |
| `create_upf_library`, `use_upf_library`, `load_upf_library` (5.11, 6.29, 6.36, 6.63) | libraries of VCMs: `lib::name`, `IEEE::name` and `.::name` inside a library; `use_upf_library` makes a library's names visible in its scope and the scopes below, or in the library being created; a simple name found in two visible libraries is an error. A library file runs once however often it is loaded |
| `create_power_switch` | `-on_state`, `-on_partial_state`, `-off_state`, `-error_state`, `-ack_port` with `-ack_delay`, the output value rules of 6.23; several switches on a `-resolve parallel` / `one_hot` net |
| `add_power_state` on a supply set | `-supply_expr`, `-logic_expr`, `-simstate`, `-illegal`, `-complete`; predefined `ON` (all functions `FULL_ON`) and `OFF` (9.3.2) |
| `add_power_state` on a domain | `-logic_expr` over power states of other objects and logic nets; an `-illegal` state, two states at once, or no state with `-complete` is reported |
| simstates (9.5, 9.6) | `NORMAL`; `CORRUPT` (and `NOT_NORMAL`): the domain's processes stop, everything they drive is held at `x`; `CORRUPT_ON_ACTIVITY`, `CORRUPT_ON_CHANGE`, `CORRUPT_STATE_ON_ACTIVITY`, `CORRUPT_STATE_ON_CHANGE`: writes by the domain's processes (or its state elements) store `x` |
| power-up (9.6.7) | the holds are released; combinational logic (`assign`, `always_comb`, `always @*`) re-evaluates; flops stay `x` until their next clock or reset edge |
| `set_isolation` (9.8) | on each port connection that crosses the domain boundary: `-applies_to`, `-applies_to_boundary`, `-elements`, `-exclude_elements`, `-source`, `-sink`, `-diff_supply_only`, `-no_isolation`; `-clamp_value 0`, `1`, `Z`, `latch`; one or more `-isolation_signal`s with `-isolation_sense`; `x` when the isolation supply is not `NORMAL` or the control is unknown |
| `set_level_shifter` (9.9) | `x` when its input or output supply (by default the two domains' primary supplies) is not `NORMAL` |
| `set_retention` (9.7) | single control (`{ret high}` / `{ret low}`: saved on entry, held through power-down, shown again at power-up, released when the control falls), or separate save and restore signals (edge or level), `-save_event_condition`, `-restore_event_condition`, `-restore_period_condition`, `-powerdown_period_condition`, `-retention_supply` with `RET_SUP_COR` / `NO_RET_SUP_COR`, `SAV_RES_COR`, `-use_retention_as_primary`, `-applies_to flop | latch | any` (a latch: `always_latch`, or a variable a level-sensitive `always` leaves unassigned on some path) |
| `set_repeater` (6.52, 9.10) | a buffer on each port connection the strategy applies to (`-applies_to`, `-applies_to_boundary`, `-elements`, `-exclude_elements`, `-source`, `-sink`, `-update`): its output is `x` while `-repeater_supply` is not `NORMAL` |
| `create_logic_net`, `create_logic_port`, `connect_logic_net` (6.13, 6.19, 6.20) | one-bit nets and ports made by the power intent, and their connections to HDL ports (`-reconnect` replaces a constant on an input). The net can be an isolation, retention or switch control |
| `add_state_transition` (6.7, 9.4) | named transitions of a supply set, domain or group (`-from` / `-to`, `-paired`, `-update`): an `-illegal` transition is an error, and with `-complete` so is one that is not named; `UNDEFINED` is passed through; `-upf_verbose` prints each named transition |
| `create_power_state_group` (6.22) | `add_power_state -group`: the legal combinations of other objects' power states; an `-illegal` state, two states at once, or no state with `-complete` is reported |
| `create_composite_domain` (6.17) | a strategy (`set_isolation`, `set_level_shifter`, `set_repeater`, `set_retention`) on the composite domain is a strategy of each subdomain, or of none when a subdomain rejects it; the primary supplies of the subdomains must be equivalent; its power states work like those of a group |
| `set_equivalent` (6.47) | `-nets`: one electrical net, so driving one drives all; `-sets` and `-function_only`: the supplies count as the same in `-source`, `-sink`, `-diff_supply_only` and the composite domain rule |
| `connect_supply_set` (6.15) | `-elements`, `-exclude_elements`, `-transitive`: the named instances follow that supply set instead of their domain's primary supply |
| `set_simstate_behavior` (6.56), `UPF_simstate_behavior`, `UPF_is_power_aware_model` | `DISABLE`: the simulator does not corrupt the instance; `PORT_CORR_ONLY` (hard macros only): its logic is left alone and its outputs are `x` while its supply is not `NORMAL`; by `-models`, `-lib`, `-elements`, `-exclude_elements` |
| `set_design_attributes` (6.44), `set_port_attributes` (6.51) | attributes of models, instances and ports with the precedence of 5.7. Simulated: `-driver_supply` (a primary input of the power top, or an output of an instance with no logic, is `x` while that supply is not `NORMAL`), `-clamp_value` (the clamp of an isolation strategy that gives none), `-is_analog` (no strategy applies), `-is_hard_macro`, `-is_power_aware_model`, `UPF_retention`. The others are kept for the checks |
| `set_retention_elements` (6.54) | a named list that `set_retention -elements` can use; a `required` element that no strategy retains, in a domain that has retention, is an error |
| `add_supply_state` (6.8), `set_partial_on_translation` (6.50) | named states of a supply port, net or supply set function, with `{PARTIAL_ON v}` told apart from `FULL_ON`; `PARTIAL_ON` read as `OFF` or `FULL_ON` |
| `sim_corruption_control` (6.59) | `-type real`, `integer` (a predefined integer type: `int`, `integer`, ...), `seq`, `comb`, `port`, `process`, by `-elements`, `-exclude_elements`, `-domain`, `-model`, `-transitive`: those elements keep their values while their domain is off; a process that drives only such elements keeps running. A retained register stays corrupted, with a warning |
| `sim_assertion_control` (6.58) | `-type suspend`, `reset` (attempts in progress end) or `kill` (never on again), by `-elements` (instances or assertion labels), `-exclude_elements`, `-domain`, `-model`, `-transitive`; active while the assertion's domain (or `-controlling_domain`) is not `NORMAL`, or while `-control_expr` is true. Decided when a time step settles, so supplies the testbench turns on at time 0 do not trigger it |
| `load_simstate_behavior` (6.34) | reads a file of `set_simstate_behavior` commands for a library; any other command, `-lib` in the file, or a model that is not in the design is an error |
| `define_power_model` (6.31), `begin_power_model` / `end_power_model` (6.11, 6.32, legacy), `apply_power_model` (6.9) | a power model's commands run once for each instance it is applied to, with the instance as the current scope; `define_power_model` runs them in a Tcl frame of its own. `-for`, `-update`, `-complete`; `apply_power_model -elements`, or every instance of the model's modules under the current scope; `-supply_map` (an implied `associate_supply_set`), `-port_map` (an implied `connect_supply_net` or `connect_logic_net`), `-parameters`, `-update`. Nested definitions, an instance that does not exist or is not of a model the power model is for, and an unknown parameter are errors. `-implementation` is accepted and not enforced |
| `add_parameter` (6.3) | in a power model, a Tcl variable with the `-default` value or the one `apply_power_model -parameters` gives; `-type buildtime`, `runtime`, `rate` are recorded (power consumption is not modelled) |
| `bind_checker` (6.12) | each command becomes an SV `bind` directive and the design is elaborated with it: `-bind_to` binds into every instance of a module, otherwise into each of `-elements` (or the current scope); `-ports` connects HDL names of that scope, `-parameters` overrides parameters (a value that is not a number is passed as a string). `-upf_verbose` prints the directives. A port that names a UPF object is not supported |
| `sim_replay_control` (6.60) | the selected initial blocks (`-elements` instances, `-exclude_elements`, `-model`, `-domain`, `-transitive`) run again each time their domain (or `-controlling_domain`) goes from not `NORMAL` to `NORMAL` after time 0. An instance named in `-elements` that a `-model` command already covers is an error. Initial blocks are selected by instance, not by label |
| `save_upf` (6.42) | writes, once the power intent is read, every UPF command that ran in the scope (or `-scope`) or below it, after Tcl substitution, with `set_scope` moves relative to that scope. A power model's commands are written as its `define_power_model` and `apply_power_model`. Reading the file back gives the same simulation |
| attributes in the HDL (5.6) | `(* UPF_... = "value" *)` on a module or on one of its ports counts as `set_design_attributes -models` or `set_port_attributes -model -ports` given before the UPF, so a UPF command of the same precedence wins (5.7). Values are checked; Table 8 (attributes that only models, or only instances, can have) is enforced for HDL attributes and UPF commands |
| `create_abstract_power_source` (6.16) | each function of the output supply set is driven like the output of a power switch whose inputs are that function of each input supply set (a function all sets share, such as a common ground, is left alone): `-on_state` (the named input set while its expression of control ports holds), `-off_state` (or off when no on state holds), `-ack_port` with `-ack_delay` (inertial) and `-ack_polarity`. With `-power_switch` (`-update`) the switches drive the nets instead; they must use the source's controls and drive its ack nets. A non-`NORMAL` `-supply_set` is not simulated yet |
| `create_pst`, `add_port_state`, `add_pst_state` (legacy) | when a time step settles, the supplies must match a row of every PST |
| Implementation commands: `map_power_switch` (6.37), `map_repeater_cell` (6.38), `map_retention_cell` (6.39), `map_retention_clamp_cell` (6.40), `name_format` (6.41), `set_correlated` (6.43), `set_variation` (6.57), `use_interface_cell` (6.62), `define_always_on_cell`, `define_diode_clamp`, `define_isolation_cell`, `define_level_shifter_cell`, `define_power_switch_cell`, `define_retention_cell` (7.2 to 7.7) | they choose library cells and names for implementation, so the simulation does not change. Each is read and checked against the errors its clause lists, with file, line and clause: unknown options, missing required options, an empty list, a domain, switch or strategy that does not exist (a strategy of another domain too), an element outside the strategy's elements, more than one isolation or level-shifter strategy in `use_interface_cell`, `-lib_model_name` without `-port_map`, an affix given twice, `-nets` with `-sets`, a `-range` whose low factor is above the high one, different variations on electrically equivalent supplies, `-pin_groups` that repeat a pin, input and output voltage range lists of different lengths, a supply pin a clamp or shifting cell cannot have, a footer switch with `-power_switchable`, save and restore functions on the same pin and edge, and `-hold_check` pins that the save, restore and retention checks need at different values |
| `map_retention_cell -lib_model_name` (6.39) | each mapped register (a variable, or every register an instance's clocked blocks assign) is simulated by the model: its canonical interface is inferred from the always block that assigns it (`UPF_GENERIC_CLOCK` from the clock edge, `UPF_GENERIC_DATA` from the clocked branch, with `if` conditions as `?:`, `UPF_GENERIC_ASYNC_SET_RESET` / `_ASYNC_LOAD` from the edges the `if` chain tests, `UPF_GENERIC_OUTPUT`, `~` to invert), and `-port_map` also takes `save_signal`, `restore_signal`, logic nets and supplies (`retention_supply.f`, `primary_supply.f`, a supply net or port, converted as `connect_supply_net` would). The register follows the model's output; neither corruption nor the strategy's retention applies to it. An unmapped input, a missing module or port, a port mapped twice and a block whose interface cannot be inferred (two clocks, a `case`) are errors |
| `use_interface_cell -force_function` (6.62) | each port the isolation strategy isolates (narrowed by `-elements`, `-exclude_elements` and `-applies_to_clamp`) is simulated by the first of `-lib_cells`, an HDL module: its one input and one output that `-port_map` leaves open are the data input and output, and `-port_map` connects `isolation_signal`, `isolation_signal[i]` (with `~` to invert), `isolation_supply.f`, `input_supply.f` / `output_supply.f` of the level-shifter strategy, supply nets and logic nets. A one-bit model is used once for each bit of a wider port. The strategy's `-isolation_sense` does not apply; the output is still corrupted while the isolation supply is not NORMAL. A cell with no HDL module (`define_isolation_cell` or Liberty only) is simulated as 9.8 describes, with a warning. Errors: a model that clamps (`define_isolation_cell -clamp_cell`) to another value than the strategy, an input left open beside the data input, an output in `-port_map`, a port mapped twice or missing, `~` on anything but the isolation signal, an index past the strategy's signals, a supply reference the strategies do not have |
| `upf_version`, `set_scope`, `set_design_top`, `load_upf` | yes |
| Tcl (5.1) | the tool line's Tcl interpreter (`src/tcl.hpp`): variables and arrays, `[command]` substitution, `expr`, `if`, `foreach`, `for`, `while`, `proc`, `upvar`, `uplevel`, `namespace eval` and `variable`, `regexp` / `regsub`, `list` / `string` commands, `catch`, `error`, `source`, `puts`. A bus index in a name (`gen[0]`, `d[7:0]`) is text, not a command. The first error stops the read, with its file and line |

`initial` blocks are never gated, so testbench and model initialization code
keeps running in a powered-off domain (9.2.1). Errors print as
`UPF error: <time>: <text>` and the simulation goes on.

With `-upf`, the processes power can gate (those in a power domain), those
that run UPF code (isolation and level-shifter cells on port connections,
calls of the UPF package) and those that write a signal the UPF runtime
watches stay coroutines. The rest of the design (the testbench and
always-on logic) is planned as without `-upf`: static combinational and
clocked schedules, partitions and `-threads`. Results do not depend on the
thread count. `MSIM_UPF_SERIAL=1` runs everything on the interpreter on one
thread. The schedule rewrites that move code between processes (merged
drivers, split or inlined blocks, unrolled loops) stay off.

`-compile` works with `-upf`: msim binds the power intent to the design,
writes what it bound to a snapshot next to `simv`, and `simv` reads the UPF
file again with the design's answers and runs the same power-aware runtime.
A process of a power domain sleeps while the domain is off and corrupts its
writes as its simstate says, as in the interpreter; replayed initial blocks
(`sim_replay_control`), HDL ports on supply nets, VCMs, retention and
isolation models, and `-upf_cov` all run in `simv`. A design `-compile`
cannot build for another reason (a concurrent assertion, say) still runs on
the interpreter.

### Forward SAIF (Annex J)

`msim -saif <file> -saif_forward <forward.saif>` writes a backward SAIF file
shaped by a forward one (`-saif_forward` may be given more than once, and
implies `-saif dump.saif`):

- A library forward file (J.4: `(SAIFVERSION "2.0" "lib")`, `LIBRARY`,
  `MODULE`) adds, for every instance of a listed cell, `(INSTANCE "cell" u1
  (PORT ..) (LEAKAGE ..))`: per pin the simple T0/T1/TX/TC followed by the
  state-dependent (`COND` .. `COND_DEFAULT`, split into `(RISE)` and `(FALL)`
  where the directive says `RISE_FALL`), path-dependent (`IOPATH`,
  `IOPATH_DEFAULT`) or both (SDPD) toggle counts (J.3.7 to J.3.9), and per
  leakage directive the T1/T0 time of each condition (J.3.6, J.3.10.4).
  Conditions are priority-encoded. An input toggle counts in the state just
  before it; an output toggle in the state just before the last input toggle,
  which is also its path (J.3.7). Pin directions come from the design.
- An RTL forward file (J.5: `INSTANCE`, `PORT`, `NET` mappings) keeps only the
  listed points, under their mapped names; a vector is written bit by bit. A
  port mapping with a type is a `VIRTUAL_INSTANCE` of that type (J.3.11),
  whose pin is `Q` unless the mapped name ends in `/pin`.

Limits: a toggle whose state has an x or z in a condition pin is not counted
in any item; a pin wider than one bit has no state-dependent data; when RTL
and library files are both given, the RTL mapping is written.

### Not yet

The implementation commands are checked and do not change the simulation,
and a library cell is not checked against a Liberty file.
`map_retention_cell -lib_model_name` and `use_interface_cell -force_function`
with an HDL model are simulated (see the table).
Isolation and level shifters sit on port connections between a parent and a
child instance; a strategy on a port of the simulation top itself, or on a
hierarchical reference, has no cell. A latch inferred from a level-sensitive
`always` is found per process (a variable not assigned on some path from one
wait to the next); one inside a called task or function is not. A supply net driven from a one-bit HDL
port through a table has a state and no voltage.
`create_upf_library` makes no Tcl namespace. `find_objects -object_type process` is not supported (msim does not name
processes), and `mdebug -upf` reads files without a design, so it does not
run `find_objects` on HDL objects. Tcl
has `namespace eval`, `variable`, `upvar`, `uplevel`, `regexp` and `regsub`
(ECMAScript regular expressions), and no file I/O.

## Low-power coverage: `msim -upf_cov <file>`

At the end of the run msim writes a report of the low-power bins and how
often each was hit, with the bins not hit marked:

- each power state of a power domain, a power state group, and a supply set
  that has power states or named transitions (`ON` and `OFF` included), as
  entered (illegal states are not bins);
- each named, legal transition of `add_state_transition`;
- each isolation cell, as the times it began to isolate;
- each retention strategy's saves and restores.

The first line gives the bins hit of all bins. `tests/upf/lpcov.cov` is an
example.

## Static checks: `mlint -upf`

| Rule | What it finds |
|---|---|
| `UPF001` | A UPF name that is not in the design: a domain element, an isolation, retention or switch control signal |
| `UPF002` | A domain whose primary supply set has no power function, or a supply function that names no supply net or port |
| `UPF003` | A port that crosses from a domain that can be off (its power comes from a switch, or a PST / power state turns it off) to one that is on, with no isolation strategy (a port with `UPF_is_isolated` or `UPF_is_analog` is not reported) |
| `UPF004` | A port between two supplies that the PST or power states give different voltages, with no level shifter |
| `UPF005` | An isolation strategy on a domain that is never off: redundant isolation |
| `UPF006` | A PST state that names a supply port state never defined with `add_port_state` |
| `UPF007` | Power intent that breaks a rule of the standard: a composite domain whose subdomains have primary supplies that are not equivalent (6.17), a required retention list element that no strategy retains (6.54) |
| `UPF008` | An isolation control signal in a domain that can be off (and is not on the isolation supply): the enable is lost while the cell must clamp |
| `UPF009` | Output isolation whose `-isolation_supply` is the switched supply of the domain it isolates: the clamp dies with the domain |
| `UPF010` | A retention save or restore signal in a domain that can be off (and not on the retention supply), or a `-retention_supply` that is the domain's own switched supply |
| `UPF011` | A power switch whose control signal is in the domain the switch powers: once off, nothing can turn it on |
| `UPF012` | A level shifter on a crossing whose two sides are on one supply, or always at one voltage (redundant level shifter) |

Findings on ports point at the instance's port connection in the HDL;
findings on the power intent point at the UPF line.
