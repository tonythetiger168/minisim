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
vlib work; vcom -2008 rtl.vhd; vlog tb.sv; vsim -c -do "run -all" tb   # Siemens steps (docs/COMPAT.md)
vhdlan rtl.vhd; vlogan -sverilog tb.sv; vcs tb; ./simv                   # Synopsys steps
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
| `entity e(a)`, a configuration specification (`for u : c use entity e(a);`), a configuration declaration (3.4) | another architecture or other bindings are a copy of the module, named `e__a` or `e__cfgN`; a configuration declaration is a module with the configuration's name, so `-top cfg` and `configuration cfg` instances run it. Nested block configurations bind the instances inside the bound architecture |
| package + package body | a package; deferred constants get the body's value |
| process | `always` that runs the body once at time 0, then waits on the sensitivity list (VHDL's equivalent `wait on` at the end). The first run waits one `#0`, so port connections already hold their initial values (14.7.5.2) |
| `process (clk)` + `if rising_edge(clk)`, and the async-reset form | `always @(posedge clk ...)` |
| signal assignment | nonblocking assignment (the next delta); `after` gives the delay |
| `wait for T` | `#T`, then the process resumes on an event triggered in the NBA region (`->>`), so it sees the signal updates due at T as VHDL's simulation cycle does (14.7.5) |
| concurrent, conditional and selected signal assignment | an equivalent process, so a derived signal lags one delta like in VHDL |
| `q <= d when rising_edge(clk);` | `always @(posedge clk)` |
| variable | a static variable of the process |
| component / entity / SV module instance | a module instance; a VHDL component binds to the VHDL entity or SV module with its name, matched case-insensitively |
| for / if / case generate, block | generate blocks |
| a resolved signal with several drivers (tri-state buses) | a wire; every driving process drives its own variable |
| `S'delayed(T)`, `S'stable(T)`, `S'last_event`, `S'transaction` and the other implicit signals (16.2.4) | one helper process per signal keeps the time and value of its last event and transaction; `S'delayed(T)` is a nonblocking assignment delayed by T. A process with a sensitivity list reads `'event` from the value at its last resume |
| function, procedure | `function automatic`; a procedure is a `function automatic void` unless it waits or assigns a signal (then a `task automatic`), so VHDL functions can call it. Ones with unconstrained array parameters or results, or signal parameters, are specialized per call so the widths are known |
| protected type (5.6) | a class; `shared variable` and protected variables hold an object made at elaboration; methods are class methods (a method that needs a per-call specialization is a function taking the object) |
| access type (5.4) | a class handle: `new`, `null`, `.all`, implicit dereference, `deallocate`. A designated record's elements are the class's own members (so a list node may hold the access type); an array is a dynamic array with its left bound and direction |
| incomplete type | a forward `typedef class`, completed by the full declaration |
| unconstrained array port (6.5.6.3) | the parameters `<port>__L` and `<port>__R` hold the index range the actual gives it; an instance whose actual is ascending uses a copy of the module named `<entity>__to_<ports>`. A SystemVerilog parent sets the two parameters (`par #(.d__L(7), .d__R(0)) u (.d(v), ...)`); from SystemVerilog the range is descending |
| entity generic type and subprogram (6.5.6.1) | one copy of the module per set of actuals (`e__g1`, `e__g2`, ...), where the type and subprogram names stand for the actuals |
| generic package, package instantiation (4.7, 4.9) | one SystemVerilog package per instance: generic constants are localparams, generic types and subprograms are names for the actuals |
| `report`, `assert` | a message in GHDL's format (`file:line:col:@time:(report note): text`); severity failure stops the run with a non-zero exit |
| `std.env.stop` / `finish` | `$stop` / `$finish` |
| enumerations, records, arrays | enum, packed or unpacked struct, packed (bit-like elements) or unpacked arrays |
| integer, real, time | int, real, realtime (fs resolution) |

Names keep the spelling of their declaration, so the waveform and hierarchy
show `dut.cnt`, and SystemVerilog code instantiates a VHDL entity by its
declared name.

### Supported language

Design units: entity, architecture, package, package body, configuration, context clauses
(`library`, `use`). Types: enumeration, integer and real ranges, physical
types, constrained and unconstrained arrays (multi-dimensional too), records,
subtypes, `line`/`text`, access types (to scalars, records, arrays and
string), incomplete types, protected types and their bodies. Generic
packages and package instantiation with type, constant and subprogram
generics. Statements: all sequential
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
matching relational operators and `??`, `justify`, and VHDL-2019's
`std.env` tool and source queries (`tool_name`, `vhdl_version`,
`file_name`, `file_path`, `file_line`). Attributes: `'event`, `'active`,
`'last_value`, `'last_event`, `'last_active`, `'stable`, `'stable(T)`,
`'quiet`, `'quiet(T)`, `'delayed`, `'delayed(T)`, `'transaction`, `'length`, `'range`, `'reverse_range`, `'left`,
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
- A character of a `string(1 to n)` variable that was never assigned is a
  space, where VHDL has NUL (SystemVerilog strings hold no NUL).
- Conversion functions in port maps.
- Guarded signals and blocks with ports; package generics (a generic
  package passed to another); generic types and subprograms on subprograms
  (entities and generic packages have them); package instantiation inside a
  declarative region (library-level instances work).
- The actual of an entity's generic subprogram is declared in a package (or
  predefined), and the actual of a generic type is a predefined type or one
  declared in a package. A SystemVerilog parent cannot instantiate an
  entity with generic types.
- Access types: to a bit vector or a multidimensional array; a slice
  through an access value read as a whole array starts at index 0 inside a
  specialized subprogram (`'left` reads 0).
- Transactions (`'active`, `'quiet`, `'transaction`, `'last_active`) are
  seen from assignments in the signal's own architecture; through a port
  only the events of the actual count. The signal attributes other than
  `'event`, `'last_value` and `'stable` need a signal of the architecture,
  a port or a package signal (not one declared in a block), and a constant
  index (`s(3)'delayed`, not `s(i)'delayed` in a generate loop).
- Integer range checks: an out-of-range value is not an error.
- `'image` of a real prints `%g`-style text, not exactly GHDL's.
- VHDL-2019 additions.
