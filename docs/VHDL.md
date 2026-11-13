# VHDL and mixed-language simulation

msim simulates VHDL, and designs that mix VHDL and SystemVerilog in either
direction, with the same engine it uses for SystemVerilog. VHDL design units
are translated into SystemVerilog in memory (`src/vhdl/`), handed to the slang
front end together with the SystemVerilog files, then elaborated, lowered and
run on the msim kernel. Waveforms, coverage, `-compile`, VPI, `-i` and every
other msim feature see one design.

```
msim counter.vhd tb.vhd                         # VHDL only; the top is found like for SV
msim counter.vhd tb.sv -top tb                  # an SV testbench around a VHDL entity
msim adder.sv tb.vhd -top tb                    # a VHDL testbench around an SV module
msim -xrun -v200x rtl.vhd tb.sv                 # the xrun front end passes VHDL files on
msim a.vhd -vhdl-sv a.sv                        # also write the SystemVerilog msim simulates
vhdl2sv a.vhd b.vhd > design.sv                 # the same translation, as a separate program
mlint a.vhd b.vhd -top top                      # lint the VHDL (docs/LINT.md, "VHDL")
```

Files ending in `.vhd`, `.vhdl`, `.vho` or `.vht` are VHDL. They are read in
command-line order (the analysis order). `-v93`, `-v2008`, `-vhdl2008` are
accepted and ignored: the front end reads VHDL-93 and the VHDL-2008 additions
listed below. `-top` names a VHDL entity case-insensitively.

## How VHDL maps onto the engine

| VHDL | msim |
|---|---|
| entity + architecture (the last analyzed one) | a module with the entity's name; generics are parameters |
| package + package body | a package; deferred constants get the body's value |
| process | `always` that runs the body once at time 0, then waits on the sensitivity list (VHDL's equivalent `wait on` at the end) |
| `process (clk)` + `if rising_edge(clk)`, and the async-reset form | `always @(posedge clk ...)` |
| signal assignment | nonblocking assignment (the next delta); `after` gives the delay |
| concurrent, conditional and selected signal assignment | an equivalent process, so a derived signal lags one delta like in VHDL |
| `q <= d when rising_edge(clk);` | `always @(posedge clk)` |
| variable | a static variable of the process |
| component / entity / SV module instance | a module instance; a VHDL component binds to the VHDL entity or SV module with its name, matched case-insensitively |
| for / if / case generate, block | generate blocks |
| a resolved signal with several drivers (tri-state buses) | a wire; every driving process drives its own variable |
| function, procedure | `function automatic` / `task automatic`; ones with unconstrained array parameters or results, or signal parameters, are specialized per call so the widths are known |
| `report`, `assert` | a message in GHDL's format (`file:line:col:@time:(report note): text`); severity failure stops the run with a non-zero exit |
| `std.env.stop` / `finish` | `$stop` / `$finish` |
| enumerations, records, arrays | enum, packed or unpacked struct, packed (bit-like elements) or unpacked arrays |
| integer, real, time | int, real, realtime (fs resolution) |

Names keep the spelling of their declaration, so the waveform and hierarchy
show `dut.cnt`, and SystemVerilog code instantiates a VHDL entity by its
declared name.

### Supported language

Design units: entity, architecture, package, package body, context clauses
(`library`, `use`). Types: enumeration, integer and real ranges, physical
types, constrained and unconstrained arrays (multi-dimensional too), records,
subtypes, `line`/`text`, access to string. Statements: all sequential
statements (if, case, case?, loops with labels and `next`/`exit` to outer
loops, `wait on/until/for` and combinations, signal and variable assignment
including VHDL-2008 conditional and selected forms, force/release), process
with sensitivity list or `process (all)`, concurrent assignments, assertions
and procedure calls, component and entity instances (named, positional and
partial port association, `open`), generate statements, blocks.

Libraries: `std.standard`, `std.textio` (read/write of the standard types,
readline/writeline, files), `std.env`, `ieee.std_logic_1164`,
`ieee.numeric_std`, `ieee.numeric_bit`, `ieee.math_real` (including
`uniform`), `ieee.std_logic_unsigned` / `std_logic_signed` /
`std_logic_arith` (the Synopsys packages), and VHDL-2008's `to_string`,
`to_hstring`, `to_ostring`, `minimum`, `maximum`, reduction operators,
matching relational operators and `??`. Attributes: `'event`,
`'last_value`, `'stable`, `'length`, `'range`, `'reverse_range`, `'left`,
`'right`, `'high`, `'low`, `'ascending`, `'image`, `'value`, `'pos`, `'val`,
`'succ`, `'pred`, `'subtype`, `'element`.

## Checking against an independent simulator

Every VHDL test in `tests/vhdl` has a golden output made with GHDL, an
independent open-source VHDL simulator. `ctest -R vhdl` runs msim against the
goldens; when `ghdl` is installed, the `vhdl_ghdl_*` tests also re-make each
golden with GHDL. GHDL is GPL and only ever runs as an external program.

## Limits / Not yet

- std_logic's nine values run on SystemVerilog's four: `'U'`, `'W'`, `'-'`
  become `'X'`, `'L'` becomes `'0'`, `'H'` becomes `'1'`. `std_logic'image`
  and `to_string` therefore print `X` where GHDL prints `U` for a signal
  never assigned.
- Configurations (choosing an architecture other than the last analyzed),
  configuration instances, and conversion functions in port maps.
- Unconstrained ports (an entity port without a range).
- Generic types, generic subprograms, generic packages and package
  instantiation (VHDL-2008), protected types, guarded signals and blocks
  with ports.
- `'delayed`, `'transaction`, `'last_event`, `'stable(T)`, `'quiet(T)`.
- Integer range checks: an out-of-range value is not an error.
- `'image` of a real prints `%g`-style text, not exactly GHDL's.
- VHDL-2019 additions.
