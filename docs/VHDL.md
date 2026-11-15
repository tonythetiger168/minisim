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
| for / if / case generate, block | generate blocks; a block's generics are localparams with the mapped values and its ports name their actuals; a guarded block has the signal `GUARD`, and a guarded assignment is `if GUARD then ... end if` (11.6); to a guarded signal (`bus`, `register`, 6.4.2.3) it disconnects the driver when GUARD is false, after the disconnection specification's delay (7.4); `s <= null` disconnects too (10.5.2.2); a disconnected `bus` signal resolves with no drivers (`'Z'`), a `register` signal keeps its value |
| a resolved signal with several drivers (tri-state buses) | a wire; every driving process drives its own variable |
| `S'delayed(T)`, `S'stable(T)`, `S'last_event`, `S'transaction` and the other implicit signals (16.2.4) | one helper process per signal keeps the time and value of its last event and transaction; `S'delayed(T)` is a nonblocking assignment delayed by T. A process with a sensitivity list reads `'event` from the value at its last resume |
| function, procedure | `function automatic`; a procedure is a `function automatic void` unless it waits or assigns a signal (then a `task automatic`), so VHDL functions can call it. Ones with unconstrained array parameters or results, or signal parameters, are specialized per call so the widths are known |
| protected type (5.6) | a class; `shared variable` and protected variables hold an object made at elaboration; methods are class methods (a method that needs a per-call specialization is a function taking the object) |
| access type (5.4) | a class handle: `new`, `null`, `.all`, implicit dereference, `deallocate`. A designated record's elements are the class's own members (so a list node may hold the access type); an array is a dynamic array with its left bound and direction |
| incomplete type | a forward `typedef class`, completed by the full declaration |
| unconstrained array port (6.5.6.3) | the parameters `<port>__L` and `<port>__R` hold the index range the actual gives it; an instance whose actual is ascending uses a copy of the module named `<entity>__to_<ports>`. A SystemVerilog parent sets the two parameters (`par #(.d__L(7), .d__R(0)) u (.d(v), ...)`); from SystemVerilog the range is descending |
| entity generic type and subprogram (6.5.6.1) | one copy of the module per set of actuals (`e__g1`, `e__g2`, ...), where the type and subprogram names stand for the actuals |
| `conv(formal) => actual` on an out port (6.5.7.3) | the port drives a signal of its own type, and a process assigns `conv()` of it to the actual; a conversion on the actual of an in port is an expression |
| generic subprogram, subprogram instantiation (4.2.1, 4.4) | each instance is a copy of the subprogram in which the generics' names stand for the actuals (a subprogram generic can be `T'image`) |
| generic package, package instantiation (4.7, 4.9) | in a library, architecture, block, process or subprogram; one SystemVerilog package per instance: generic constants are localparams, generic types and subprograms are names for the actuals, and a package generic (6.5.6.4) names the actual instance |
| an integer assigned to a subtype with a range (5.2.1) | a range check: out of range prints `file:line:@time:(bound check failure)` on stderr and stops the run with a non-zero exit, as GHDL does |
| conditional analysis (24.2) | done on the source text before parsing. `TOOL_TYPE` is `"SIMULATION"`, `VHDL_VERSION` `"2019"`, `TOOL_VENDOR` `"minisim"`, `TOOL_NAME` `"msim"`; `+define+NAME=VALUE` (or `-D`) adds or overrides an identifier |
| mode view port (6.5.2) | one SystemVerilog port per record element in the element's mode (`<port>__<element>`), recursively through element views and, for `view (v) of t`, through each array index (`<port>__<i>__<element>`); inside the module the port is a record signal |
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
`file_name`, `file_path`, `file_line`). VHDL-2019 also brings mode views
(`view`, `'converse`, a view as an element's mode, `view (v) of t` on
arrays of records, 6.5.2), conditional analysis
(`` `if ``, `` `elsif ``, `` `else ``, `` `end ``, `` `warning ``, `` `error ``,
24.2), sequential block statements (10.15), `return x when c else y`,
conditional expressions in initial values, defaults and parentheses
(`constant c : integer := 1 when n > 3 else 2;`, 9.1), empty records,
`'image` of records and of string, bit-vector and integer array types (16.2.2), an
interface list ending in `;`, generic type classes (`type t is (<>)`,
`range <>`, `range <>.<>`, `array (i range <>) of type is private`, 6.5.3)
and the std.env report and assert API (`GetVhdlAssertCount`,
`IsVhdlAssertFailed`, `ClearVhdlAssert`, `SetVhdlAssertEnable`,
`GetVhdlAssertEnable`, `SetVhdlAssertFormat` with `{s}` `{S}` `{r}` `{t}`
`{i}`, `GetVhdlAssertFormat`, `Set/GetVhdlReadSeverity`; 16.5.9), and
std.env's calendar time (`time_record`, `dayofweek`, `epoch`,
`localtime`, `gmtime`, `to_string` of a time record, `+` and `-` on time
records, `time_to_seconds`,
`seconds_to_time`), `getenv`, the call path (`get_call_path`, `to_string` of its
elements, vectors and pointers), directories (`dir_open` and `dir_close` with the `directory` record, its items in name order, `dir_workingdir`,
`dir_itemexists`, `dir_itemisdir`, `dir_itemisfile`, `dir_createdir`,
`dir_deletedir`, `dir_deletefile` as functions and procedures,
`dir_separator`) and the PSL queries (16.5.5 - 16.5.7, 16.5.10). msim
exports small helpers for the time and directory calls, which the
translation reaches through DPI-C.
Attributes: `'event`, `'active`,
`'last_value`, `'last_event`, `'last_active`, `'stable`, `'stable(T)`,
`'quiet`, `'quiet(T)`, `'delayed`, `'delayed(T)`, `'transaction`, `'length`, `'range`, `'reverse_range`, `'left`,
`'right`, `'high`, `'low`, `'ascending`, `'image`, `'value`, `'pos`, `'val`,
`'succ`, `'pred`, `'subtype`, `'element`.

## Checking against an independent simulator

## Nine-value std_logic

`std_logic` and `std_logic_vector` keep all nine values `U X 0 1 Z W L H -`
(IEEE 1076-2019 16.7): a signal never assigned prints `'U'`, two `'H'`
pull-ups resolve to `'H'`, `'L' and '1'` is `'0'`, `'U' or '0'` is `'U'`,
and `to_x01`, `'image`, `to_string`, `=` and `textio` see every value.

Each `std_logic` object is a four-state SystemVerilog variable (its
four-value view: `L`/`H` read as `0`/`1`, `U`/`W` as `x`, `'-'` as `z`)
plus a companion `<name>__9` that holds the weak/uninitialized bit. The
four-value view is what SystemVerilog, UPF, waveforms and VPI see, so a
mixed design needs no conversion at the boundary: an SV module driving a
VHDL `in` port drives strong values, and a VHDL `out` port read by SV gives
`0`/`1`/`x`/`z`. A resolved signal with weak drivers resolves with
SystemVerilog drive strengths (`weak0`/`weak1`), as 16.7's resolution table.

`msim -vhdl4` turns the companions off: std_logic then runs on the four
values only (`'U'`, `'W'`, `'-'` are `'X'`, `'L'` is `'0'`, `'H'` is `'1'`).
That is about twice as fast in the interpreter (NEORV32's processor test:
55 s with `-vhdl4`, 103 s with nine values, for 300 µs).

Every VHDL test in `tests/vhdl` has a golden output made with GHDL, an
independent open-source VHDL simulator. `ctest -R vhdl` runs msim against the
goldens; when `ghdl` is installed, the `vhdl_ghdl_*` tests also re-make each
golden with GHDL. GHDL is GPL and only ever runs as an external program.

## Limits / Not yet

- std_logic keeps all nine values (see "Nine-value std_logic" above) for
  signals, variables, ports between VHDL units and literals. Record fields,
  arrays of vectors, subprogram parameters and `inout` ports carry only the
  four-value view: `'L'`/`'H'` read as `'0'`/`'1'` and `'U'`/`'W'` as `'X'`
  there. A `case` selector compares the four-value view only.
- A character of a `string(1 to n)` variable that was never assigned is a
  space, where VHDL has NUL (SystemVerilog strings hold no NUL).
- On an `inout` port, the conversions on the formal and the actual must be
  type conversions between arrays of std_logic-based elements (such as
  `std_logic_vector(io) => unsigned(bus)`); a conversion function there is
  not supported. In and out ports take both kinds.
- A package instance declared in an architecture, block, process or
  subprogram is a package of the same name, so two of them in the design
  need distinct names, and its generic actuals cannot name the enclosing
  region's own constants or signals.
- A register signal with several drivers keeps its value when the resolved
  value is all `'Z'` (every driver disconnected, or a connected driver
  driving `'Z'`). A guarded signal must have a scalar or vector `std_logic`
  type to disconnect; a disconnection delay is not checked against later
  ordinary `after` transactions of the same driver.
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
- Range checks cover integer values assigned to a variable or signal of an
  integer subtype (`natural`, `integer range 0 to 15`); an out-of-range
  enumeration or real value, index, or actual in a port or parameter
  association is not checked.
- VHDL-2019: an array mode view needs an array with static bounds; in `get_call_path`
  (16.5.7), the root of a process without a label is named after its
  entity, and a procedure that waits can see frames of another process that
  called subprograms meanwhile; `'image` of arrays whose elements are not integers,
  characters or bits; the read severity is kept but textio reads do not
  report with it; `{i}` in an assert format is the module instance path
  (`:top:u1:`), without the process label; the reflection API (`'reflect`,
  std.reflection).
