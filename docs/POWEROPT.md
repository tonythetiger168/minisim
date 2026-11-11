# RTL power optimization: `mpopt`

`mpopt` reads RTL on the same front end as `msim`, finds registers and
operators that do work nobody uses, and rewrites the source so that they
stop. It is the minisim counterpart of a tool such as PowerPro.

```
mpopt rtl/*.sv -top soc                  # report what can be gated
mpopt rtl/*.sv -top soc -o out           # write the rewritten RTL to out/
mpopt rtl/*.sv -top soc -o out -verify   # and co-simulate it against the original
```

The rewrite edits the source text in place. Comments, formatting and every
line `mpopt` does not touch are kept byte for byte, so `diff -r rtl out` shows
exactly what changed and the result can be reviewed like a colleague's patch.

## What it does

| Tag | Transformation | Example |
|---|---|---|
| CG | Clock gating from an enable that is written in the RTL | `if (en) q <= d;` |
| SCG | Sequential clock gating: an enable that is *not* in the RTL | a pipeline stage behind an enabled stage |
| MG | Memory gating: CG and SCG on unpacked arrays | a write port, a registered read port |
| OI | Operand isolation | `if (use) p <= a * b;` |

### CG: enable clock gating

For every register of an `always_ff` or `always @(posedge clk ...)` block,
`mpopt` takes the OR of the path conditions of all assignments that can change
it. If that is not constant true, the register gets a clock gate:

```systemverilog
// before
always_ff @(posedge clk or negedge rst_n)
  if (!rst_n) a_q <= '0;
  else if (in_valid) a_q <= a;

// after
wire mpopt_gclk_L25;
always_ff @(posedge mpopt_gclk_L25 or negedge rst_n)
  if (!rst_n) a_q <= '0;
  else if (in_valid) a_q <= a;
// mpopt: clock gate for a_q
mpopt_icg mpopt_icg_L25 (.clk(clk), .en(in_valid), .te(1'b0), .gclk(mpopt_gclk_L25));
```

Conditions come from `if`, `else`, `case`, `casez`, `casex` and nesting of
them. An asynchronous reset stays on the flop; a synchronous reset becomes
part of the enable (`rst || en`). Registers of one block with different
enables are split into one block per enable (`-no_split` keeps blocks whole
and uses the OR of the enables). A block with a named `begin` is never split.

### SCG: sequential clock gating

A register whose next value is a function of other registers need not load
when none of those loaded in the cycle before: the value it would load is the
value it already has. `mpopt` adds one flop that remembers "a source may have
changed" and gates with it:

```systemverilog
// before: no enable anywhere, but a_q and b_q only load on in_valid
always_ff @(posedge clk or negedge rst_n)
  if (!rst_n) prod_d <= '0;
  else prod_d <= a_q * b_q;

// after
wire mpopt_gclk_L36;
always_ff @(posedge mpopt_gclk_L36 or negedge rst_n)
  if (!rst_n) prod_d <= '0;
  else prod_d <= a_q * b_q;
// mpopt: sequential clock gate for prod_d
reg mpopt_st_L36;
always @(posedge clk or negedge rst_n)
  if (!rst_n) mpopt_st_L36 <= 1'b1;
  else mpopt_st_L36 <= in_valid;
mpopt_icg mpopt_icg_L36 (.clk(clk), .en(mpopt_st_L36), .te(1'b0), .gclk(mpopt_gclk_L36));
```

This is the gating a synthesis tool does not find, because the enable is not
written in the RTL. The enable follows a pipeline for `-seq_depth` stages
(default 4): stage 3 is gated by "stage 2 may have changed", and so on. A
register that has its own enable *and* stable sources gets both
(`en && stable`).

`mpopt` applies it when every signal the block reads (data and conditions,
through combinational logic) comes from registers of the same module on the
same clock, the block does not read the registers it writes, and there is a
reset to start the stability flop from (it starts at 1, so the first edge
after reset is always taken). Inputs of the module, outputs of instances,
function calls and hierarchical names stop it; `-v` says which.

### MG: memory gating

A memory written as `if (we) mem[waddr] <= wdata;` gets one clock gate on
`we`. A registered read `rdata <= mem[raddr];` is gated sequentially: it only
reads again after the address register loaded or the memory was written.

### OI: operand isolation

A multiplier, divider, modulo or power operator (and, with `-iso_add N`, an
adder or subtractor of N bits or more) that sits under a condition gets its
operands forced to zero when the condition is false, so that the operator
does not toggle for results nobody reads:

```systemverilog
// before
assign y = use_mul ? ua * (ub + 12'd3) : {12'd0, ua};
// after
wire [11:0] mpopt_iso_L30_a = use_mul !== 1'b0 ? ua : 1'b0;
wire [23:0] mpopt_iso_L30_b = use_mul !== 1'b0 ? (ub + 12'd3) : 1'b0;
assign y = use_mul ? mpopt_iso_L30_a * mpopt_iso_L30_b : {12'd0, ua};
```

The isolated operands are named signals, declared in front of the statement.
A power tool reads their activity from a waveform like any other signal (an
operand hidden inside an expression has none), and the operator keeps the
size it had: a 12-bit `ua` stays a 12-bit operand of the multiplier, where a
`?:` written into the expression would have been widened to the 24 bits of
the result. An operand that is itself an expression gets the width it is
computed in (`ub + 12'd3` above: 24 bits, with its carry). Named signals are
used in `always_ff` blocks and continuous assignments whose operands and
condition read module-level signals. In `always_comb`, where a value may be a
step of a blocking computation, and with `-iso_inline` everywhere, the
expression is rewritten in place: `((c) !== 1'b0 ? a : 1'b0)`, inside a
concatenation when `a` is a plain signal so that it keeps its width.

The condition is the path condition of the operator: `if`/`case` branches in
`always_ff` and `always_comb`, and `?:` in any expression. The operand keeps
its width and signedness (`$signed(1'b0)` for a signed operand). The
comparison with `!== 1'b0` makes the rewritten RTL simulate exactly like the
original when the condition is x (an `else` or `default` taken on an x);
`-iso_plain` writes `(c ? a : 1'b0)` for flows that reject `!==`, which is
x-pessimistic in simulation and the same in hardware.

## Why the rewrites are right

- **CG.** The body of the block is not changed. When the enable is 0, no
  assignment of the block would have run, so removing that clock edge changes
  no register. When the enable is x the gated clock goes 0 to x, the block
  runs, and its own conditions decide, as before.
- **SCG.** Write the block as q' = F(x, q), with x the registers it reads.
  F never reads q except to hold it. If x did not change since the last edge,
  the same branches are taken and the same values are assigned again:
  F(x, F(x, q)) = F(x, q), so the edge can be skipped. x cannot change unless
  one of its own clock gates was open (or the asynchronous reset fired, which
  sets the stability flop to 1), and that is what the stability flop records.
- **OI.** The wrapped operand is only evaluated on a path where the condition
  is true, so `(c !== 1'b0 ? a : 0)` is `a` there.

In every case each register of the original design holds the same value in
every cycle. `mpopt` adds stability flops and clock gates and removes nothing.

## Checking the result

`-verify` writes a testbench (`<dir>/mpopt_tb.sv`) that drives every input of
the top module with the same pseudo-random values, toggles the clock, applies
reset at the start and once more in the middle, and prints every output and
every register once per cycle. It runs `msim` on the original and on the
rewritten RTL (side by side) and compares the two logs; any difference is an
error with the first differing cycle. Clock and reset ports are found from
the RTL (`-clock`, `-reset`, `-reset_n` name them by hand); `-cycles N` sets
the length (default 2000). `-verify_only` repeats the check on what is in the
output directory now, for example after an edit by hand.

Co-simulation is a check, not a proof. `-prove` hands the proof to `mequiv`,
the equivalence checker: it runs `mequiv -ref <the original> -impl -f
<dir>/files.f` and reports proved (exit 0), a difference (exit 1) or out of
effort (exit 3, see `-prove_effort`), with the log in `<dir>/prove.log` and
every compare point in `<dir>/prove.json`. `mequiv` reads a flop on a gated
clock as a flop with a load enable, which makes CG, MG on write ports and OI
combinational compare points. Two limits, both stated in the output:

- SCG adds stability flops and is right because of what held one cycle
  earlier, an inductive argument from reset that `mequiv` does not have yet.
  When the rewrite has sequential gates, `-prove` writes the rewrite without
  them to `<dir>/prove/` and proves that one; the sequential gates
  themselves are covered by `-verify`.
- The miter of two wide multipliers whose operands differ by the isolation
  is hard for SAT: with isolated multipliers of 24 bits the proof can end as
  out of effort. `-no_iso -prove` proves the gating alone.
- `mequiv` does not yet read a gate on a clock that is already gated (a
  design with its own top-level clock gate, such as Ibex) or a gate whose
  enable is a constant as an enable flop, and reports those registers as
  different. Until it does, `-verify` is the check for such designs.

## Measuring the result

`-power` measures what the rewrite saved. It runs the `-verify` simulations
with waveforms on, gives each waveform to `mpower` with its own design, and
prints the two results:

```
mpopt: -power: before and after, on the 2000 cycles of random stimulus of -verify (generic technology)
  total               192.49 uW  ->      148.44 uW  (-22.9%)
  clock network        36.45 uW  ->       21.01 uW  (-42.4%)
  registers            80.68 uW  ->       48.77 uW  (-39.6%)
  combinational        72.98 uW  ->       75.56 uW  (+3.5%)
```

On Ibex (`ibex_top`, 62 gates on 90% of the register bits) the same run over
5000 cycles gives 1325 uW before and 801 uW after, 40% lower: the clock
network and the registers each drop by about 80%.

`-lib file.lib` gives `mpower` a Liberty library; without one it uses its
generic technology, which is good for comparing two versions of a design and
not for absolute numbers. The reports are `<dir>/power_ref.json` and
`<dir>/power_opt.json`. Two things to keep in mind. The stimulus is random:
every enable is high about half the time, which is busier than most real
workloads, so the saving on a real workload is usually larger; for that
number, simulate both designs with your own testbench and run `mpower` on
each. And the combinational row can go up a little: the gates, the enable
logic and the isolation logic are new logic. The total is what counts.

Besides that,
`ctest -R mpopt_fuzz` runs random designs through `mpopt -verify`
(`tests/popt/fuzz_popt.py`; 3000 designs with 2000 sequential gates passed
before the first release), and a broken rewrite is shown to be caught
(`tests/mpopt.cmake`).

## Output

```
rtl/pipe.sv:25: [CG] pipe: a_q, b_q (32 bits) gated by in_valid
rtl/pipe.sv:36: [SCG] pipe: prod_d (32 bits) gated by mpopt_st_L36; stable unless in_valid held in the cycle before
rtl/pipe.sv:60: [MG] pipe: mem (256 bits) gated by we
mpopt: 1 module, 1 instance, 405 register bits, 404 gated (99%)
  clock gates from enables:    4 (68 bits)
  sequential clock gates:      3 (80 bits)
  memory gates:                1 (256 bits)
  operators isolated:          0
```

`-v` adds the registers that were not gated and why. `-json file` writes the
same as JSON. The totals count every instance of the design at its own width;
the lines show each module once (`x N` for the copies of a generate loop).

With `-o dir`:

| File | What |
|---|---|
| `dir/<name>.sv` | every source file, rewritten or copied; changed include files too |
| `dir/mpopt_cells.sv` | `mpopt_icg`, the clock gate: a latch that is open while the clock is low, and an AND |
| `dir/files.f` | the file list to compile instead of the original |
| `dir/mpopt.json` | the report |

Map `mpopt_icg` to your library's integrated clock-gating cell, or let `mpopt`
instantiate the cell directly: `-icg_cell CKLNQD1:CP,E,TE,Q` (cell, then the
clock, enable, test-enable and output pins). `-test_enable scan_en` connects
the gates' test enable to that signal in every module that has it (otherwise
it is tied to 0).

Power numbers come from `mpower` (the power analysis tool, in development):
simulate the original and the rewritten RTL with the same testbench, and
compare the two `mpower` reports. A register on a gated clock is costed from
that clock's activity.

## Options

| Option | Meaning |
|---|---|
| `-o <dir>` | write the rewritten RTL |
| `-verify`, `-verify_only`, `-cycles <n>` | co-simulate against the original |
| `-prove`, `-prove_effort <n>` | prove the rewrite with `mequiv` |
| `-power`, `-lib <file.lib>` | power before and after, from `mpower` |
| `-clock <port>`, `-reset <port>`, `-reset_n <port>` | clock and reset ports for `-verify` |
| `-json <file>`, `-v`, `-quiet` | report |
| `-no_cg`, `-no_seq`, `-no_iso`, `-no_split` | turn one transformation off |
| `-min_bits <n>` | smallest register group to gate (default 4) |
| `-seq_min_bits <n>` | smallest group for a sequential gate (default 8) |
| `-seq_depth <n>` | stages a sequential enable follows (default 4) |
| `-iso_min_bits <n>`, `-iso_add <n>`, `-iso_plain`, `-iso_inline` | operand isolation |
| `-icg_cell <cell>[:clk,en,te,gclk]`, `-test_enable <signal>`, `-prefix <name>` | what is inserted |
| `-threads <n>` | worker threads (default: every logical CPU) |

Modules are analyzed in parallel and files are rewritten in parallel; the
report and the files are the same for any `-threads`. A module with several
parameterizations is rewritten once: enable gating and isolation hold for all
of them by construction, and sequential gating is kept only when all
parameterizations agree on it.

## What is left alone

`mpopt` does not touch a block it does not fully understand: blocking
assignments or local variables in a clocked block, falling-edge clocks, text
that comes from a macro, a clock or reset that is not a plain signal, tasks
and system tasks in the block. `-v` lists each one with the reason.

## Speed

| Design | Size | Result | Time |
|---|---|---|---|
| Ibex (`ibex_top`) | 20 modules, 37 instances | 62 clock gates on 90% of 2089 register bits | 0.07 s |
| Ibex with `-verify -cycles 5000` | | every output and register equal | 3.4 s |
| Synthetic, 4000 modules | 717,000 lines | 21,000 gates | 5.4 s on one thread, 3.8 s on four |

Of the 3.8 s, 2.4 s is the front end (parsing and elaboration, shared with
every minisim tool) and 0.65 s is `mpopt`'s own analysis (1.8 s on one thread).

## Not yet

- Sequential gating across module boundaries (today the sources must be
  registers of the same module, so a pipeline split over modules is not seen).
- Observability gating: not loading a register whose value will not be read.
  It changes register values in cycles nobody observes, so it waits for the
  sequential equivalence proof in `mequiv`.
- Chip-enable gating of instantiated RAM macros (inferred memories are done).
- A proof of the sequential gates (needs induction with an invariant in `mequiv`).
- Ranking candidates by measured activity, and `-power` on a user testbench.
- Falling-edge registers and latch-based designs.
