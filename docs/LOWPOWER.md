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
| `create_power_switch` | `-on_state`, `-on_partial_state`, `-off_state`, `-error_state`, `-ack_port` with `-ack_delay`, the output value rules of 6.23; several switches on a `-resolve parallel` / `one_hot` net |
| `add_power_state` on a supply set | `-supply_expr`, `-logic_expr`, `-simstate`, `-illegal`, `-complete`; predefined `ON` (all functions `FULL_ON`) and `OFF` (9.3.2) |
| `add_power_state` on a domain | `-logic_expr` over power states of other objects and logic nets; an `-illegal` state, two states at once, or no state with `-complete` is reported |
| simstates (9.5, 9.6) | `NORMAL`; `CORRUPT` (and `NOT_NORMAL`): the domain's processes stop, everything they drive is held at `x`; `CORRUPT_ON_ACTIVITY`, `CORRUPT_ON_CHANGE`, `CORRUPT_STATE_ON_ACTIVITY`, `CORRUPT_STATE_ON_CHANGE`: writes by the domain's processes (or its state elements) store `x` |
| power-up (9.6.7) | the holds are released; combinational logic (`assign`, `always_comb`, `always @*`) re-evaluates; flops stay `x` until their next clock or reset edge |
| `set_isolation` (9.8) | on each port connection that crosses the domain boundary: `-applies_to`, `-elements`, `-exclude_elements`, `-source`, `-sink`, `-diff_supply_only`, `-no_isolation`; `-clamp_value 0`, `1`, `Z`, `latch`; one or more `-isolation_signal`s with `-isolation_sense`; `x` when the isolation supply is not `NORMAL` or the control is unknown |
| `set_level_shifter` (9.9) | `x` when its input or output supply (by default the two domains' primary supplies) is not `NORMAL` |
| `set_retention` (9.7) | single control (`{ret high}` / `{ret low}`: saved on entry, held through power-down, shown again at power-up, released when the control falls), or separate save and restore signals (edge or level), `-save_event_condition`, `-restore_event_condition`, `-restore_period_condition`, `-powerdown_period_condition`, `-retention_supply` with `RET_SUP_COR` / `NO_RET_SUP_COR`, `SAV_RES_COR`, `-use_retention_as_primary` |
| `create_pst`, `add_port_state`, `add_pst_state` (legacy) | when a time step settles, the supplies must match a row of every PST |
| `upf_version`, `set_scope`, `set_design_top`, `load_upf`, Tcl `set` and `$var` | yes |

`initial` blocks are never gated, so testbench and model initialization code
keeps running in a powered-off domain (9.2.1). Errors print as
`UPF error: <time>: <text>` and the simulation goes on.

Power-aware runs use the interpreter on one thread: `-compile`, the static
schedules and `-threads` are turned off with `-upf`.

### Not yet

`map_*`, `use_interface_cell`, `define_*_cell`, `set_port_attributes`,
`set_simstate_behavior`, repeaters, composite domains, power models and the
other implementation commands are read and ignored with a note.
Isolation and level shifters sit on port connections between a parent and a
child instance; a strategy on a port of the simulation top itself, or on a
hierarchical reference, has no cell. `-applies_to flop` / `latch` of a
retention strategy are not told apart. Supply ports connected to HDL ports
(VCTs, 4.10), Tcl command substitution `[...]`, `foreach` and procedures are
not supported.

## Static checks: `mlint -upf`

| Rule | What it finds |
|---|---|
| `UPF001` | A UPF name that is not in the design: a domain element, an isolation, retention or switch control signal |
| `UPF002` | A domain whose primary supply set has no power function, or a supply function that names no supply net or port |
| `UPF003` | A port that crosses from a domain that can be off (its power comes from a switch, or a PST / power state turns it off) to one that is on, with no isolation strategy |
| `UPF004` | A port between two supplies that the PST or power states give different voltages, with no level shifter |
| `UPF005` | An isolation strategy on a domain that is never off: redundant isolation |
| `UPF006` | A PST state that names a supply port state never defined with `add_port_state` |

Findings on ports point at the instance's port connection in the HDL;
findings on the power intent point at the UPF line.
