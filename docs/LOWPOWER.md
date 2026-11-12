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
| `upf_query_object_properties`, `upf_query_object_type`, `upf_object_in_class`, `upf_query_object_pathname`, `query_upf` (clause 10, 11.1) | the UPF information model, built from the power intent and the design at the first query, which starts phase 5: after it only `set_scope`, `bind_checker`, `find_objects` and `load_upf` may follow (11.1.1). Handles are paths from the root scope (`/u_dut/PD_CORE`, `/u_dut/PD_CORE.ISO_0`, `/u_dut/VDD@upfSupplyNetT` for a net that shares its name with a port), relative to the current scope, `.`, or tool IDs (`#UPFEXTENT1#`, `#UPFSIGSENSE1#`, `#UPFEXPR1#`). Objects: power domains, isolation, level-shifter and retention strategies with their supply set handles and logic references, supply sets and their functions, supply ports and nets with their connections, power switches with their ports and states, port states, power states, state transitions, power state tables, power state groups, composite domains, abstract power sources, repeater strategies, power models with the instances they are applied to, attributes of HDL objects, HDL scopes, ports and nets, extents, signal senses and expressions, each with the static properties of its class (10.6). `query_upf` searches a scope (as `find_objects`, plus `supply_set`) or the extent of a power domain. A test that checks every answer: tests/upf/query.upf |
| `create_vcm`, `connect_supply_net -vcm` (4.10, 6.14, 6.30) | value conversion methods. Table VCMs for one-bit (`bit`, `logic`, `std_logic`, `boolean`), real, integer and enum ports, both directions, with every check of 6.30.3 to 6.30.7 (ranges, overlaps, rows for every state or value). hdl2upf: the first row whose range holds the HDL value sets the state, and the voltage when the row gives one (`*`: the HDL value in volts); no row: UNDETERMINED. upf2hdl: the row for the net's state (and voltage) sets the port, `*` the voltage in volts, `Xreal` / `Zreal` the package values; no row is a run-time error. `-vcms` lists are picked from per port by type and direction (4.10.2). A port of type `upfSupplyTypeT` (`supply_net_type`) connects with no VCM. The Annex B tables and the legacy VCTs are VCMs too. Function and model VCMs (6.30.8, 6.30.9): msim checks the function's or module's signature (one input, one output, exactly one side `upfSupplyTypeT`, the HDL side of the port's type), then binds a small module into the simulation top that calls the function, or instantiates the model with `-parameters`, on the port; the HDL side follows the language's own semantics. With `-upf_verbose` the generated SystemVerilog is printed |
| Supply ports by pg_type (4.5.4.6.2, 6.14, 6.15) | a port's pg_type is its `UPF_pg_type` attribute, from `set_port_attributes -pg_type` or the HDL. `connect_supply_net -pg_type` with `-cells`, `-domain` or `-elements` connects the net to every such port of the selected instances below the scope; `connect_supply_set -connect {function pg_type}` does the same for its elements; and each function of a domain's primary supply set goes automatically to the ports of the default pg_type (`power`: `primary_power`, `ground`: `primary_ground`, the bias functions their own names) of the instances in its extent. A port keeps the first of these that reaches it, in that order, after any `-ports` connection |
| `create_upf_library`, `use_upf_library`, `load_upf_library` (5.11, 6.29, 6.36, 6.63) | libraries of VCMs: `lib::name`, `IEEE::name` and `.::name` inside a library; `use_upf_library` makes a library's names visible in its scope and the scopes below, or in the library being created; a simple name found in two visible libraries is an error. A library file runs once however often it is loaded |
| `create_power_switch` | `-on_state`, `-on_partial_state`, `-off_state`, `-error_state`, `-ack_port` with `-ack_delay`, the output value rules of 6.23; several switches on a `-resolve parallel` / `one_hot` net |
| `add_power_state` on a supply set | `-supply_expr`, `-logic_expr`, `-simstate`, `-illegal`, `-complete`; predefined `ON` (all functions `FULL_ON`) and `OFF` (9.3.2) |
| `add_power_state` on a domain | `-logic_expr` over power states of other objects and logic nets; an `-illegal` state, two states at once, or no state with `-complete` is reported |
| simstates (9.5, 9.6) | `NORMAL`; `CORRUPT` (and `NOT_NORMAL`): the domain's processes stop, everything they drive is held at `x`; `CORRUPT_ON_ACTIVITY`, `CORRUPT_ON_CHANGE`, `CORRUPT_STATE_ON_ACTIVITY`, `CORRUPT_STATE_ON_CHANGE`: writes by the domain's processes (or its state elements) store `x` |
| power-up (9.6.7) | the holds are released; combinational logic (`assign`, `always_comb`, `always @*`) re-evaluates; flops stay `x` until their next clock or reset edge |
| `set_isolation` (9.8) | on each port connection that crosses the domain boundary: `-applies_to`, `-applies_to_boundary`, `-elements`, `-exclude_elements`, `-source`, `-sink`, `-diff_supply_only`, `-no_isolation`; `-clamp_value 0`, `1`, `Z`, `latch`; one or more `-isolation_signal`s with `-isolation_sense`; `x` when the isolation supply is not `NORMAL` or the control is unknown |
| `set_level_shifter` (9.9) | `x` when its input or output supply (by default the two domains' primary supplies) is not `NORMAL` |
| `set_retention` (9.7) | single control (`{ret high}` / `{ret low}`: saved on entry, held through power-down, shown again at power-up, released when the control falls), or separate save and restore signals (edge or level), `-save_event_condition`, `-restore_event_condition`, `-restore_period_condition`, `-powerdown_period_condition`, `-retention_supply` with `RET_SUP_COR` / `NO_RET_SUP_COR`, `SAV_RES_COR`, `-use_retention_as_primary` |
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
| `upf_version`, `set_scope`, `set_design_top`, `load_upf` | yes |
| Tcl (5.1) | the tool line's Tcl interpreter (`src/tcl.hpp`): variables and arrays, `[command]` substitution, `expr`, `if`, `foreach`, `for`, `while`, `proc`, `list` / `string` commands, `catch`, `error`, `source`, `puts`. A bus index in a name (`gen[0]`, `d[7:0]`) is text, not a command. The first error stops the read, with its file and line |

`initial` blocks are never gated, so testbench and model initialization code
keeps running in a powered-off domain (9.2.1). Errors print as
`UPF error: <time>: <text>` and the simulation goes on.

Power-aware runs use the interpreter on one thread: `-compile`, the static
schedules and `-threads` are turned off with `-upf`.

### Not yet

The implementation commands are checked and do not change the simulation:
the model given by `map_retention_cell -lib_model_name` and the function of a
`use_interface_cell -force_function` cell are not simulated in place of the
strategy, and a library cell is not checked against a Liberty file.
Isolation and level shifters sit on port connections between a parent and a
child instance; a strategy on a port of the simulation top itself, or on a
hierarchical reference, has no cell. `-applies_to flop` / `latch` of a
retention strategy are not told apart. A supply net driven from a one-bit HDL
port through a table has a state and no voltage. `-field` of a struct or UDN
is read and checked, and a connection that needs one is an error;
`create_upf_library` makes no Tcl namespace. `find_objects -object_type process` is not supported (msim does not name
processes), and `mdebug -upf` reads files without a design, so it does not
run `find_objects` on HDL objects. Dynamic properties of the information model (`upf_current_state` and the like) are read in simulation through the UPF package (11.2), not by the Tcl queries; cells and retention extent groups are not in the model (msim inserts no cells), and `query_upf -inst_type` other than `all` finds no instances because msim inserts no cells. Tcl
has no `namespace`, `upvar`, `regexp` or file I/O.

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

Findings on ports point at the instance's port connection in the HDL;
findings on the power intent point at the UPF line.
