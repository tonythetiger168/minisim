# mlint: lint rules, configuration and waivers

`mlint` checks RTL on the same front end as `msim`, so a design that
simulates needs no extra setup to be linted:

```sh
mlint rtl/*.sv -top soc_top                    # the default rules
mlint -f files.f -top soc_top -Werror          # exit 1 on any warning (CI)
mlint rtl/*.sv -config mlint.cfg -waivers waivers.cfg -json lint.json
mlint -list                                    # every rule: severity, on/off, group
mlint -explain RST003                          # what a rule means, what to do
```

A finding is one line, `file:line:col: severity: [RULE] text`, sorted by file
and line. The last line counts them per rule. Findings can be filed as issues
with `mbug lint`.

## What is checked

Rules are grouped; a group name works wherever a rule name does
(`-disable naming`, `waive width ...`). Each distinct module body (a module
with one set of parameter values) is checked once, however often it is
instantiated. Testbench code is not held to the synthesis rules: a program,
a module with no ports, or a module named in `tb.modules`.

### Latches (`latch`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `L001` | warning | on | A value is not assigned on every path of an always_comb (a latch) |
| `LAT001` | warning | on | A value is not assigned on every path of a combinational always @* (a latch) |
| `LAT002` | info | off | An always_latch: a latch by intent |

### Combinational logic (`comb`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `COMB001` | warning | on | A signal read by a combinational always is missing from its sensitivity list |
| `COMB002` | warning | on | A combinational loop |

### Case statements (`case`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `CASE001` | warning | on | A case in combinational logic has no default and does not cover every value |
| `CASE002` | warning | on | Casex |
| `CASE003` | warning | on | An X or Z in an item of a plain case |
| `CASE004` | warning | on | The same value appears in two items of one case |

### Assignments (`assign`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `L002` | warning | on | A blocking assignment in an always_ff |
| `L003` | warning | on | A non-blocking assignment in an always_comb |
| `SEQ001` | warning | on | A variable has both blocking and non-blocking assignments |
| `SEQ002` | warning | on | A blocking assignment in an edge-triggered always, to a variable read elsewhere |
| `SEQ003` | warning | on | A non-blocking assignment in a combinational always @* |
| `EXP001` | warning | on | A variable is assigned to itself |

### Drivers (`driver`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `L004` | warning | on | One value is assigned by more than one process |

### Widths and signs (`width`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `W001` | warning | on | An assignment truncates its right-hand side |
| `W002` | info | off | An assignment extends a narrower right-hand side |
| `W003` | warning | on | A port is connected to an expression of another width |
| `W004` | warning | on | A comparison of operands of different widths |
| `W005` | warning | on | A signed value is compared with an unsigned one |
| `W006` | info | off | A multi-bit value is used as a condition |
| `W007` | info | off | A logical operator (!, &&, ||) is applied to a multi-bit value |
| `W008` | warning | on | The two branches of ?: have different widths |

### Synthesizability (`synth`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `SYN001` | warning | on | An initial block assigns design signals |
| `SYN002` | warning | on | A delay in design code |
| `SYN003` | warning | on | A system task or function that cannot be synthesized |
| `SYN004` | warning | on | Force, release, or a procedural assign / deassign |
| `SYN005` | warning | on | Fork, wait, or an event trigger in design code |
| `SYN006` | warning | on | A real-typed signal in design code |
| `SYN007` | warning | on | A sensitivity list mixes edges and levels |
| `SYN008` | warning | on | A forever loop, or a loop with a timing control, in an always block |
| `SYN009` | warning | on | == or != against a value with X or Z bits |
| `SYN010` | warning | on | === or !== in design code |
| `SYN011` | info | off | An X is assigned |
| `SYN012` | warning | on | An event control inside the body of an always block |
| `SYN013` | warning | on | A dynamic type (string, queue, dynamic or associative array, class, event) in design code |
| `SYN014` | info | on | Division, modulo or power by a value that is not a constant power of two |
| `SYN015` | warning | on | Defparam |
| `SYN016` | info | on | A tri-state driver on a signal that is not an inout port |
| `SYN017` | warning | on | A hierarchical reference in design code |

### Unused and undriven signals (`unused`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `UNU001` | warning | on | A signal is declared but never used |
| `UNU002` | warning | on | A signal is driven but never read |
| `UNU003` | warning | on | A signal is read but never driven |
| `UNU004` | info | on | An input port is never read |
| `UNU005` | warning | on | An output port is never driven |

### Connections (`connect`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `CON001` | warning | on | An input port of an instance is left unconnected |
| `CON002` | info | off | An output port of an instance is left unconnected |
| `CON003` | warning | on | Ports connected by position |
| `CON004` | warning | on | An implicit net |

### Clocks (`clock`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `CLK001` | warning | on | A clock is used as data |
| `CLK002` | info | off | A flop on the falling edge of its clock |
| `CLK003` | warning | on | One signal is used both as a clock and as a reset |

### Resets (`reset`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `RST001` | warning | on | One reset is used with both polarities |
| `RST002` | warning | on | One reset is used both asynchronously and synchronously |
| `RST003` | warning | on | A flop in an asynchronous-reset block is not reset |
| `RST004` | info | off | A flop with no reset |
| `RST005` | warning | on | The reset's edge does not match the level its block tests |
| `RST006` | warning | on | An asynchronous reset to a value that is not constant |
| `RST007` | warning | on | An asynchronous block that does not start with if (reset) |

### Naming (`naming`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `NAME001` | warning | off | A module name does not match name.module |
| `NAME002` | warning | off | A signal name does not match name.signal |
| `NAME003` | warning | off | A parameter name does not match name.param |
| `NAME004` | warning | off | A clock's name does not match name.clock |
| `NAME005` | warning | off | A reset's name does not match name.reset |
| `NAME006` | warning | off | An active-low reset's name does not match name.active_low |
| `NAME007` | warning | off | An instance name does not match name.instance |
| `NAME008` | warning | off | A port name does not match name.port_in / name.port_out |
| `NAME009` | warning | on | Two names in one scope differ only in case |
| `NAME010` | warning | off | A name is longer than name.max_length |
| `NAME011` | info | on | A generate block has no label |
| `NAME012` | warning | off | A name is a VHDL keyword |
| `NAME013` | warning | off | A typedef name does not match name.typedef |

### Style (`style`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `STY001` | info | on | More than one module in a file |
| `STY002` | info | off | A file's name is not its module's name |
| `STY003` | info | off | A plain always where always_ff / always_comb / always_latch says more |
| `FUNC001` | info | on | A function reads or writes a signal that is not one of its arguments |

### slang-tidy checks (`tidy`)

[slang-tidy](https://github.com/MikePopoloski/slang/tree/master/tools/tidy)
is the lint tool that ships with slang, mlint's front end (MIT license). Its
checks are built into mlint and report as mlint rules, so severities,
waivers, `-json` and `-threads` apply to them. Only the checks mlint has no
rule of its own for are here (its `NoCaseX` is `CASE003`, `AlwaysFFBlocking`
is `L002`, and so on). The name in brackets is the slang-tidy check.

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `TIDY001` | warning | on | A register is assigned in the reset branch only (`OnlyAssignedOnReset`) |
| `TIDY002` | warning | on | An always_ff with a reset assigns a register outside the if / else of the reset (`AlwaysFFAssignmentOutsideConditional`) |
| `TIDY003` | warning | on | A for loop in front of the reset test of an always_ff (`LoopBeforeResetCheck`) |
| `TIDY004` | warning | on | Some bits of a signal are never driven (`UndrivenRange`) |
| `TIDY005` | warning | on | Fields of one struct are assigned in different processes (`StructFieldsInSameProcess`) |
| `TIDY006` | warning | on | A signed cast used as an index (`CastSignedIndex`) |
| `TIDY007` | warning | off | A signal in a sensitivity list is not used in the block (`UnusedSensitiveSignal`) |
| `TIDY008` | warning | off | 'd or '? literals (not accepted by some FPGA tools) (`XilinxDoNotCareValues`) |
| `TIDY009` | warning | off | A .* port connection (`NoDotStarInPortConnection`) |
| `TIDY010` | warning | off | A .name port connection with no expression (`NoDotVarInPortConnection`) |
| `TIDY011` | warning | off | A non-ANSI port declaration (`OnlyANSIPortDecl`) |
| `TIDY012` | warning | off | Generate / endgenerate keywords or a genvar declared outside its loop (`NoLegacyGenerate`) |
| `TIDY013` | warning | off | An always_comb whose begin has no label (`AlwaysCombBlockNamed`) |
| `TIDY014` | warning | off | An enum declared without a typedef (`TypedefEnums`) |
| `TIDY015` | warning | off | A struct or union declared without a typedef (`TypedefStructUnion`) |
| `TIDY016` | warning | off | An enum type name does not match tidy.enum (`EnumName`) |
| `TIDY017` | warning | off | A struct type name does not match tidy.struct (`StructName`) |
| `TIDY018` | warning | off | A union type name does not match tidy.union (`UnionName`) |
| `TIDY019` | warning | off | A covergroup name does not match tidy.covergroup (`CovergroupName`) |
| `TIDY020` | warning | off | A coverpoint name does not match tidy.coverpoint (`CoverpointName`) |
| `TIDY021` | warning | off | A cross name does not match tidy.cross (`CrossName`) |

Settings (`-set`, or `set` in a configuration file): `tidy.clock` (default
`clk_i`) and `tidy.reset` (`rst_ni`) name the clock and the reset the
reset checks look for, `tidy.reset_active_high` (`false`); `tidy.enum`,
`tidy.struct`, `tidy.union`, `tidy.covergroup`, `tidy.coverpoint` and
`tidy.cross` are the name patterns. Build without them with
`-DMINISIM_WITH_SLANG_TIDY=OFF`.

### Source text and syntax (`format`, `suspicious`, and more of `style`)

These read the source files and the syntax tree, so they also see code that
is never instantiated. The set follows Verible's lint rules, which is where
most teams know them from; they are written on slang's syntax tree (no
Verible code is used) and `mlint -explain RULE` names the Verible rule each
one answers to. All are off by default: turn on a group (`-enable format`)
or single rules.

| Rule | Severity | Default | What it finds | Verible rule |
|---|---|---|---|---|
| `FMT001` | info | off | A line longer than `fmt.line_length` (100). A line that is one long word is let through | line-length |
| `FMT002` | info | off | A tab character | no-tabs |
| `FMT003` | info | off | Trailing whitespace | no-trailing-spaces |
| `FMT004` | info | off | The file does not end with a newline | posix-eof |
| `SUS001` | info | off | A stray semicolon: `x = 1;;`, `endfunction;` | forbid-consecutive-null-statements |
| `STY004` | info | off | `` `endif `` with no comment | endif-comment |
| `STY005` | info | off | A package in a file not named after it (`foo_pkg` may live in `foo.sv`) | package-filename |
| `STY006` | info | off | A function or task with no lifetime, outside classes and `automatic` modules | explicit-function-lifetime, explicit-task-lifetime |
| `STY007` | info | off | A parameter with no type | explicit-parameter-storage-type |
| `STY008` | info | off | A packed dimension that counts up (`[0:7]`) | packed-dimensions-range-ordering |
| `STY009` | info | off | An unpacked dimension that counts down (`[3:0]`) | unpacked-dimensions-range-ordering |
| `NAME014` | warning | off | A macro name does not match `name.macro` | macro-name-style |
| `W009` | info | off | A sized binary literal with fewer digits than its width (`8'b101`) | undersized-binary-literal |

Verible's suspicious-semicolon (`if (c);`) is slang's own `-Wempty-body`
warning, which mlint already prints, and its always-comb, case-missing-default,
generate-label and name-style rules are `SYN`, `CASE`, `STY` and `NAME`
rules above.

### State machines (`fsm`)

Found by the extractor FSM coverage uses (`msim -cov`), so lint and coverage
agree on what a state machine is: a register that one clocked block loads
with constants, itself or a next-state variable, with logic that branches on
it. One-hot registers tested bit by bit are followed too.

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `FSM001` | warning | on | A state that no transition and no reset leads to |
| `FSM002` | warning | on | A state with no way out (left only by reset) |
| `FSM003` | warning | on | The case on a state register has no default and does not use every encoding (not for `unique`, `priority` or `full_case`) |
| `FSM004` | warning | on | A state register with no reset state |

### Clock-domain crossings (`cdc`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `CDC001` | warning | on | A clock-domain crossing with no synchronizer, or only one flop |
| `CDC002` | warning | on | A multi-bit bus crossing through a synchronizer, not Gray coded |
| `CDC003` | warning | on | Combinational logic in front of a synchronizer (a glitch can be captured) |
| `CDC004` | warning | on | Separately synchronized signals of one domain meet again (reconvergence) |
| `CDC005` | warning | on | One signal synchronized more than once into the same domain |
| `CDC006` | warning | on | With `-sdc`: a flop's clock is not a clock in the SDC file |
| `CDC007` | warning | on | A clock read as data by logic |
| `CDC008` | warning | on | A clock made by combinational logic of a clock and other signals (glitch) |

### Reset-domain crossings (`rdc`)

| Rule | Severity | Default | What it finds |
|---|---|---|---|
| `RDC001` | warning | on | An asynchronous reset generated in another clock domain |
| `RDC002` | warning | on | One asynchronous reset used directly in several clock domains |
| `RDC003` | warning | on | A flop feeds a flop with a different asynchronous reset, in one clock domain |
| `RDC004` | warning | off | A reset flop feeds a flop with no asynchronous reset |
| `RDC005` | warning | on | A reset synchronizer of one flop |
| `RDC006` | warning | on | An asynchronous reset made by combinational logic of several flops |

How domains and synchronizers are recognized, and the options `-cdc-report`,
`-clock-group` and `-static`, are in [CDC.md](CDC.md).

The UPF rules (`UPF001`..`UPF006`, with `-upf`) are in `docs/LOWPOWER.md`.
Design-for-test rules (gated clocks, logic-driven resets, divided clocks,
both clock edges) are `mdft`'s.

Notes on a few rules:

- **L001** uses the front end's data-flow analysis, so it follows single
  bits: `z[i]` assigned for every `i` in a loop is assigned, and
  `w[3:2]` assigned under an `if` is reported as `w[3:2]`.
- **COMB002** follows whole-word dependencies through continuous
  assignments, combinational blocks and port connections, across module
  boundaries. A value assigned earlier in the same block stands for what it
  was made from (`y = a; y = y + 1;` is not a loop), bit-select chains such
  as a carry chain are not reported, and logic that a parameter switches off
  (`x & ~(FALL_THROUGH & push)` with `FALL_THROUGH = 0`) is not followed.
- **W001** counts the bits a value needs: `cnt <= cnt + 1` and `y = 4'd9`
  are fine, `y4 = 300` and `y4 = a8` are not.
- **UNU001..UNU005** add up all the bodies of a module, so a signal used in
  one parameterization is used. A signal whose only use is in a generate
  branch that no instance selects is reported. Names matching
  `unused.ignore` (by default `unused_*`, `*_unused`, `nc_*`, `*_nc`) are
  not reported.
- **CLK001** does not report a plain buffer (`assign clk_o = clk_i;`).
  Clock gates and muxes are reported: waive them in the cell that holds them.
- **RST003** leaves memories (unpacked arrays) alone.
- **CASE001** accepts a case that lists every value of its selector or every
  member of its enum, `unique` / `priority`, and `(* full_case *)`.

## Configuration

A configuration file has one directive per line; `#` starts a comment.

```
enable  naming                  # a rule, a group, or all
disable W002 STY001
only    width,latch             # everything else off
severity W001 error             # error | warning | info
severity naming info
set name.module  ^[a-z][a-z0-9_]*$
set tb.modules   "tb_* *_bfm"
waive W001 -file "rtl/legacy/*" -reason "frozen IP"
```

Pass it with `-config FILE` (repeatable; later lines win). The same things
can be said on the command line: `-enable X`, `-disable X`, `-only X,Y`,
`-severity RULE=LEVEL`, `-set NAME=VALUE`. A rule that is off is not
computed. A mistake in a configuration (an unknown rule, a bad pattern) is
an error, exit status 2.

Severities: an `error` finding makes mlint exit 1; a `warning` does with
`-Werror`; an `info` finding never does.

| Setting | Default | Used by |
|---|---|---|
| `name.module` | `^[a-z][a-z0-9_]*$` | `NAME001` |
| `name.signal` | `^[a-z][a-z0-9_]*$` | `NAME002` |
| `name.param` | `^[A-Z][A-Za-z0-9_]*$` | `NAME003` |
| `name.clock` | `clk\|clock\|ck` (any case) | `NAME004` |
| `name.reset` | `rst\|reset\|clr\|clear` (any case) | `NAME005`, and which signal a synchronous `if` treats as a reset (`RST001`, `RST002`, `RST004`) |
| `name.active_low` | `(_n\|_ni\|_n_i\|_b\|_an\|n)$` | `NAME006` |
| `name.instance` | `^(u\|i\|g)_` | `NAME007` |
| `name.port_in`, `name.port_out` | `_i$`, `_o$` | `NAME008` |
| `name.typedef` | `_(t\|e\|s\|u)$` | `NAME013` |
| `name.max_length` | `40` | `NAME010` |
| `unused.ignore` | `^_?unused\|_unused$\|^_?nc_\|_nc$` (any case) | `UNU001`..`UNU005` |
| `name.macro` | `^[A-Z_][A-Z0-9_]*$` | `NAME014` |
| `fmt.line_length` | `100` | `FMT001` |
| `tb.modules` | empty | module names (globs) to treat as testbench code |

Patterns are regular expressions (ECMAScript), matched anywhere in the name
unless anchored.

## Waivers

A waived finding is not printed and does not fail the run; it is counted
(`3 findings (...), 2 waived`), shown with `-show-waived`, and always in the
JSON report with its reason.

In the source, next to the code:

```systemverilog
assign y = a;            // mlint: waive W001 only the low nibble is used
// mlint: waive W001,W004      (alone on its line: applies to the next line)
assign z = b;
// mlint: off SYN002
...                      // a region; "mlint: off" with no rule covers all rules
// mlint: on SYN002
// synthesis translate_off     (also "pragma" and "synopsys"): synth rules off
...
// synthesis translate_on
```

In a waiver file (`-waivers FILE`, the same format as a configuration file):

```
waive RULE[,RULE...] [-file GLOB] [-line N[-M]] [-msg GLOB] [-reason TEXT]
waive CLK001 -file "*/prim_clock_gating.sv" -reason "the clock gate cell"
waive unused -msg "*'dbg_*" -reason "debug taps"
```

`-file` matches the path as printed, or its tail (`-file dut.sv` matches
`rtl/dut.sv`); `-msg` matches the finding's text; `*` and `?` are wildcards.
A waiver that matches nothing is reported, so stale waivers do not pile up.

## Reports

`-json FILE` writes every finding, waived ones included:

```json
{"tool": "mlint", "version": "0.2.0",
 "findings": [{"file": "rtl/a.sv", "line": 12, "col": 5, "rule": "W001", "group": "width",
               "severity": "warning", "text": "...", "waived": false}],
 "summary": {"error": 0, "warning": 1, "info": 0, "waived": 0}}
```

## Speed

mlint uses every logical CPU by default; `-threads N` sets the count
(`-threads 1` for one). The output is the same for any count: every module
body is a task with its own report, and the reports are merged in a fixed
order and sorted. `-stats` prints where the time went.

Measured on a 4-CPU cloud machine (2026-11-10): Ibex (30 module bodies)
lints in 0.1 s, almost all of it parsing and elaboration; a generated design
of 4000 distinct modules (120,000 lines, 12,000 processes) takes 1.3 s on
one thread and 1.0 s on four, of which the rules themselves are 0.16 s and
0.08 s. The same design took 13 s before the signal-joining structure got
path compression, which `mdft` shares.

## Adding a rule

1. A row in the table in `src/lintcfg.hpp` (name, group, severity, on or
   off, one line, and the `-explain` text).
2. The check: in `BodyRules` (`src/lintrules.hpp`) if it looks at one module
   body, in `Global` if it needs signals joined across the hierarchy.
3. A marked line in a file under `tests/lint/rules/` (`//~ RULE`); the test
   fails for a rule that has none, and for any finding that is not marked.
