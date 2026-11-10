# Timing constraints: `msdc`, `src/sdc.hpp`, `src/tcl.hpp`

`msdc` reads a design and its SDC file and reports what is wrong or missing in
the constraints, before synthesis or static timing spend hours on them.

```sh
msdc rtl/*.sv -top soc -sdc soc.sdc              # the report, then a summary
msdc rtl/*.sv -top soc -sdc soc.sdc -Werror      # exit 1 on any finding (for CI)
msdc -sdc soc.sdc                                # the file alone, with no design
msdc -list                                       # the rules
```

Options: `-sdc <file>` (repeatable, read in order), `-top`, `-f`, `+define+`,
`+incdir+` as in `msim`; `-threads N` (default: every logical core, `1` turns
threading off, the report is the same for any N); `-disable RULE`; `-Werror`;
`-quiet`; `-json <file>`; `-stats` (design size and the time of each step).
Errors in the file (SDC001, SDC002 and a few others marked "error") make the
exit code 1; warnings do so only with `-Werror`.

## How the design is seen

The design is elaborated on the same front end as `msim` and `mlint` and
reduced to registers and the combinational logic between them. Names follow
the synthesis tools, so an SDC file written for synthesis resolves on RTL:

| Object | Name |
|---|---|
| port | `clk`, `data[3]`; `data` names every bit |
| instance | `u_core/u_alu`; inside a generate block `gen[2].u_x` |
| register | `u_core/acc_reg`, bits `u_core/acc_reg[7]` |
| register pins | `u_core/acc_reg[7]/CK`, `/D`, `/Q` (`CP`, `CLK`, `QN` are accepted) |
| instance pin | `u_core/clk`, `u_core/din[3]` |
| net | `u_core/sum`, `u_core/sum[3]` |

A signal is followed as a whole: the bits of one vector are not told apart, so
an array of clocks (`clk[1:0]`) is one clock net. Latches are logic, not
registers. Clocks propagate through logic and module boundaries, stop at a
register, and stop where another clock is defined without `-add`.

## Rules

| Rule | What it reports |
|---|---|
| SDC001 | a syntax error, a bad value or an unknown option |
| SDC002 | a command that is not an SDC command |
| SDC003 | an object query that matches nothing in the design |
| SDC004 | a clock that is named but not defined |
| SDC010 | a clock name defined twice |
| SDC011 | two clocks on one source without `-add` |
| SDC012 | a bad period or waveform |
| SDC013 | a generated clock with no usable master, or no divide / multiply / edges |
| SDC014 | a clock that clocks nothing and is used by no constraint |
| SDC015 | registers that no clock reaches |
| SDC016 | registers reached by several clocks that are not declared exclusive (a clock mux) |
| SDC018 | clocks with no `set_clock_uncertainty` |
| SDC020 | an input port with no `set_input_delay` |
| SDC021 | an output port with no `set_output_delay` |
| SDC022 | an I/O delay larger than the clock period |
| SDC023 | an I/O delay whose `-min` is above its `-max` |
| SDC024 | an I/O delay on the wrong kind of port (direction, or a clock source) |
| SDC025 | an I/O delay with no `-clock` |
| SDC027 | an I/O delay that replaces an earlier one (no `-add_delay`) |
| SDC030 | an exception whose `-from`, `-through` or `-to` names nothing, or that has none |
| SDC031 | a `-from` that is not a startpoint (clock, input port, register) |
| SDC032 | a `-to` that is not an endpoint (clock, output port, register) |
| SDC033 | an exception between points with no path in the design |
| SDC034 | a setup multicycle path with no matching hold multicycle |
| SDC035 | an exception that repeats another, or is overridden by a false path |
| SDC037 | a value that cannot be meant (multiplier below 1, min delay above max delay) |
| SDC040 | paths between two clocks with no declared relationship |
| SDC041 | clocks declared asynchronous although one derives from the other and paths exist |
| SDC042 | a malformed `set_clock_groups` |
| SDC050 | conflicting or bad `set_case_analysis` |
| SDC060 | input ports with no driving cell, drive or input transition |
| SDC061 | output ports with no load |

SDC040 is the check that needs the design: for every register (and every
output port with an output delay) it finds the registers and input ports that
feed it, and the clocks on both sides. A pair of different clocks is accepted
when one derives from the other, when a `set_clock_groups` separates them, or
when a `set_false_path` or `set_max_delay` covers the clocks or the very
registers. What is left is reported once per clock pair with the number of
paths and one example. `mlint`'s CDC rules ask a different question (is there
a synchronizer); msdc asks whether the constraints say what the timing tools
should do with the crossing.

`msta check_timing` reports what the timing graph of a gate-level netlist
leaves unconstrained. msdc works on RTL, before synthesis, and on the
constraints themselves.

## Speed

Measured on a 4-core VM (`-stats` prints the split):

| Design | Size | SDC | Time |
|---|---|---|---|
| Ibex (`ibex_top`) | 1,934 register bits, 13,313 pins | 10 commands | 0.08 s |
| generated, 3,000 blocks on 16 clocks | 294,000 register bits, 1.2 million pins | 6,021 commands, 6,000 exceptions | 2.0 s |

The second design took more than ten minutes before three changes: name
lookups go through sorted indexes (a pattern with a literal start is a range
lookup, `-hierarchical` uses an index by leaf name), `-filter` is parsed once
and applied before objects are copied, and the union-find that joins a net
across module boundaries compresses its paths (which also speeds up `mlint`
and `mdft` on nets with thousands of loads). The searches that walk the design
(SDC033, SDC040, port fan-out) and the index sorts run on all cores; on this
design they are about a quarter of the run, so more threads gain little here
and matter more as the logic between registers gets deeper. No open-source
constraint checker was at hand to compare against.

## The readers, for other tools

`src/tcl.hpp` is the one Tcl interpreter of the tool line (`tcl::Interp`):
variables and arrays, `[command]`, braces and quotes, `expr`, `if`, `foreach`,
`for`, `while`, `proc`, list and string commands, `format`, `catch`, `source`,
`exit`. A tool adds its commands and runs a script or a prompt:

```cpp
tcl::Interp t;
t.addCommand("report_timing", [&](const std::vector<std::string>& w) { ...; return std::string(); });
t.onPuts = [](const std::string& s) { std::cout << s << "\n"; };
t.load("run.tcl");                        // or t.parse(text, "name")
int rc = t.shell(std::cin, std::cout, "msta> ");
for (auto& d : t.diags) ...               // every error, with file and line
```

`src/sdc.hpp` is the SDC 2.1 reader (`sdc::Reader`, a `tcl::Interp` with the
SDC commands). The design is seen through `sdc::Design`; implement `names()`
for ports, pins, cells and nets, and optionally `portDir`, `registers`,
`related` (`-of_objects`) and `attribute` (`-filter`).

```cpp
sdc::Reader r;
r.design = &myDesign;                     // optional: without it patterns stay unresolved
r.load("top.sdc");
for (auto& c : r.sdc.clocks) ...          // also ioDelays, clockGroups, exceptions,
                                          // uncertainties, latencies, caseAnalysis
for (auto* c : r.sdc.of("set_load")) c->num("value"), c->objs("objects"), c->loc;
```

Every command is kept in `r.sdc.commands` with its options parsed, its
objects resolved and typed (`sdc::Obj{kind, name}`), and its file and line.
Not supported: `current_instance` to a sub-instance (names are read from the
top), and `-filter` on attributes the design does not provide (reported once,
the filter then keeps every object).
