# X-propagation: `msim -xprop`

Standard Verilog is optimistic about an unknown (`x` or `z`) control value:
`if (x)` takes the else branch, a `case` with an `x` selector takes the
default, and a clock going `0 -> x` counts as a rising edge. A design can
pass RTL simulation that way and fail at gate level or in silicon, for
example when a control signal is not reset.

With `-xprop`, an `always` block treats an unknown control as "either
could have happened". Every branch that might be taken runs, and each
variable they assign gets the merge of the results, the way VCS xprop and
Xcelium `-xprop` do.

```sh
msim tb.sv dut.sv -top tb -xprop                 # tmerge
msim tb.sv dut.sv -top tb -xprop=xmerge
msim tb.sv dut.sv -top tb -xprop -xprop-report xprop.log
```

| Option | Meaning |
|---|---|
| `-xprop`, `-xprop=tmerge` | Bits on which all the possible branches agree keep their value; the others become `x`. This is what a real mux or flop could output. |
| `-xprop=xmerge` | Every variable (or part of one) assigned in a possible branch becomes `x`. More pessimistic than hardware. |
| `-xprop=off` | Standard behavior (the default). |
| `-xprop=<file>`, `-xprop-config <file>` | A configuration file: X-propagation on or off per instance, subtree or module, and the merge mode (below). |
| `-xprop-report <file>` | Lists every `if`, `case` and clock edge with its source line, whether it is merged and, if not, why. |

It works the same in the interpreter and with `-compile`, and for every
`-threads` setting: the merge is ordinary code made when the design is
lowered. While no control is unknown the results are those of a run
without `-xprop`.

## Configuration file

As VCS's: statements end with `;`, and `//` or `#` starts a comment.

```
merge = tmerge;                  // or xmerge, or vmerge (off)
tree     {top}        {xpropOn};   // an instance and everything below it
tree     {top.tb_mem} {xpropOff};
instance {top.dut.u_rf} {xpropOff}; // that instance only
module   {sync_fifo cdc_*} {xpropOff}; // every instance of these modules
```

Names are hierarchical instance paths (`instance`, `tree`) or module names
(`module`); `*` and `?` match any text and any one character. Several names
may share one statement. For each `always` block, the rules for its
instance decide: an `instance` rule wins over a `module` rule, which wins
over `tree` rules; among `tree` rules the deepest matching instance wins,
and among equal rules the last one in the file. An instance that no rule
names is on.

## What is merged

```systemverilog
always_comb
  if (sel) y = a; else y = b;        // sel = x: y = merge(a, b)

always_ff @(posedge clk)
  if (en) q <= d;                    // en = x: q <= merge(d, q)

always_comb
  case (op)                          // op = 2'b0x: y = merge(a, b)
    2'b00: y = a;
    2'b01: y = b;
    2'b10: y = c;
    default: y = d;
  endcase
```

With `a = 4'b1010` and `b = 4'b1001`, `merge(a, b)` is `4'b10xx` under
tmerge and `4'bxxxx` under xmerge.

- **`if` / `else if` / `else`**, also `unique`, `unique0` and `priority`.
  A condition is unknown when it has no 1 bit and at least one `x` or `z`
  bit. Every branch up to the first whose condition is certainly true may
  be taken. A missing `else` means "nothing is assigned": the variable
  keeps its value in that branch, so `if (x) q <= d;` gives `merge(d, q)`.
- **`case`, `casez`, `casex`**. A selector without `x` or `z` bits is
  matched as always. Otherwise an item may match unless a bit that is known
  in both selector and label differs (wildcard bits of a `casez` / `casex`
  label match anything). In a plain `case` whose labels are distinct known
  constants, the default is left out when the items that may match already
  cover every value the selector could have: `2'b0x` above merges `a` and
  `b` only.
- **Clock and reset edges** of `always @(posedge clk ...)` and `always_ff`.
  An edge from or to `x` / `z` (`0 -> x`, `x -> 1` for posedge) may or may
  not have happened: the block's assignments are merged with "nothing
  assigned". A signal in the event list that the block also reads is taken
  as an asynchronous set or reset: once its level is known the edge does
  not matter, so `x -> 0` on `negedge rst_n` resets as usual, and `1 -> x`
  merges the reset values with the current ones.
- **Blocking and nonblocking assignments**, to whole variables, constant
  part selects and struct members, nested to any depth, in loops, with
  temporaries and function calls in the expressions.
- **Task and function calls** in a branch, when the task or function (and
  every one it calls) assigns only with `=`: the static variables it writes
  are merged like the block's own.
- **`break` and `continue`** in a `for` or `foreach` loop. Under an unknown
  condition the rest of the loop merges with "not run". This is
  pessimistic: each unknown step merges with the state before it, so
  values that every path agrees on can still come out `x`.
- **Array elements chosen at run time** (`mem[addr] = d`,
  `mem[addr] <= d`). In a possible branch the element takes the merge of
  its present value and the new one. An index with an unknown bit, inside
  a branch or not, names every element it may match (a bit known in both
  must agree), and each takes that merge (`x` under xmerge); without
  `-xprop` such a write is skipped (7.4.6). This needs a one-dimensional
  array.

Only `always`, `always_comb`, `always_ff` and `always_latch` are affected
(with the tasks and functions they call). `initial` and `final` blocks,
classes, programs, checkers
and continuous assignments keep the standard behavior (the `?:` operator
already merges on an unknown condition, 11.4.11).

In a branch that only might be taken, `$display` and the other message
tasks, `$error` and the other severity tasks, `$finish`, `$stop` and
immediate assertions do not run, and `unique` / `priority` violations are
not reported.

## What is not merged

A construct that cannot run speculatively keeps the standard behavior
(unknown counts as false), together with the constructs around it. The
report names each one:

```
xprop tmerge: 13 of 16 if / case / edge constructs merged
dut.sv:27: loop body: merged
dut.sv:57: if: not merged, it holds a jump out of the branch (break, continue, return or disable)
dut.sv:61: if: not merged, it holds a task or function call statement
dut.sv:44: case: merged
```

- a delay, event control, `wait` or `fork` in a branch;
- a call of a task or function that has a nonblocking assignment, a delay,
  an event control or a `fork`, or that writes an array element, a
  container or a net;
- `break` or `continue` out of a `while`, `do ... while`, `forever` or
  `repeat` loop, and `disable` of a block outside the branch;
- a blocking assignment to a queue, dynamic or associative array, a real, a
  string or a class property;
- `case ... inside`, `case ... matches`, `if ... matches`, and `casez` /
  `casex` with a label that is not constant.

Some assignments are merged pessimistically: in a possible branch they
make their target `x` instead of merging values.

- a nonblocking assignment to a bit or part chosen at run time
  (`q[i] <= d`), or with a delay (`q <= #1 d`);
- a nonblocking assignment to an array element (`mem[addr] <= d`) when one
  pass over the block can assign that array more than once;
- a nonblocking assignment to a variable that one pass over the block can
  assign more than once, when the block also has a delay or an event
  control past the first;
- every nonblocking assignment of a block that calls a task that is not
  merged (above), or has a `fork`.

An array element merges with its present value, which is exact for one
write such as `if (we) mem[addr] <= d;`. When several possible branches
write the same element, or a branch reads back an element it wrote, the
result has `x` where the present value differs, even if every branch
agrees. An unknown index of a multidimensional array keeps the standard
behavior (the write is skipped).

Known limit: two signals of one event list that change in the same time
step but in different delta cycles, the first with a clean edge and the
second from or to `x`, are taken as one clean edge.

## Cost

While every condition is known, `-xprop` should cost close to nothing. So
a block made of plain assignments, `if` and `case` is lowered twice: a
clean copy, as without `-xprop`, and a merging copy. The clean copy runs
first. When an `if` condition or a `case` selector is unknown, a jump with
a third target for an unknown value moves to the same place in the merging
copy, which finishes the pass. An ambiguous clock edge starts in the
merging copy; a clocked function skips that test in a time step where no
signal has changed from or to x. A `case` in the clean copy is the standard
one, after one test of its selector. A block that assigns a variable with `<=` more than once in
a pass, or that holds other statements (tasks, messages, delays), has only
the merging copy, which tests after each branch whether a merge is running.

The temporaries of the merge are stored without going through the
scheduler, so clocked blocks still run as parallel clocked functions and
combinational blocks stay in the static schedule. On Ibex (lowRISC,
118 745 cycles) the interpreter takes 25 s with `-xprop` and 22.5 s
without.
