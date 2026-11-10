# mequiv: logic equivalence checking

`mequiv` proves that two designs are the same circuit. It answers the question
synthesis, scan insertion, an ECO or a hand edit always raises: is the netlist
still the RTL?

```sh
mequiv -ref rtl/*.sv -impl netlist.v cells.v -top alu
```

```
reference:      top 'alu': 12 inputs, 5 outputs, 4 flops, 0 latches, 172 AND nodes
implementation: top 'alu': 12 inputs, 5 outputs, 4 flops, 0 latches, 83 AND nodes
Matched:        12 inputs, 5 outputs, 4 state elements
Compare points: 13 (8 settled structurally, 0 by simulation, 5 by SAT)
  passed 13, failed 0, aborted 0
Verification SUCCEEDED: the designs are equivalent
```

When they differ, each failing point comes with input values that show it:

```
  FAIL  next-state  y[2]  (implementation: y_reg_2_.Q)
        reference = 0, implementation = 1 when a[0]=1 a[1]=0 a[2]=0 b[0]=1 b[1]=1 b[2]=0 op[0]=0 op[1]=1 rst_n=1 y[2]=0
Verification FAILED: the designs are NOT equivalent
```

Exit status: 0 equivalent, 1 not equivalent, 2 a design could not be read,
3 inconclusive (a proof ran out of effort).

## What can be compared

Each side is a list of SystemVerilog files with the usual front-end options
(`+define+`, `+incdir+`, `-f`, `-v`, `-y`). Options before `-ref` apply to
both sides. A side may be:

- RTL (anything `msim` elaborates that is synthesizable: always_ff / always_comb
  / always_latch and their Verilog forms, functions and tasks, loops with
  constant bounds, generate, packed and unpacked arrays and structs, memories,
  signed arithmetic, multiply, divide, shifts, casez/casex, `unique`/`priority`);
- a structural netlist plus Verilog models of its cells (continuous assigns,
  gate primitives, combinational UDPs, behavioural flops and latches);
- a mix of the two.

So the same command checks RTL against a netlist, a netlist against a netlist
(before and after scan insertion, an ECO, or optimization), and RTL against
RTL (a refactor that must not change behaviour).

A netlist read together with its whole cell library has many unused modules;
`mequiv` takes the top that the other side has, or the one `-top` names.

## How it decides

1. **Gates.** Both designs are flattened into one and-inverter graph. Always
   blocks are executed symbolically: an `if` or `case` becomes a mux on its
   condition, loops are unrolled, functions inlined. Logic the two sides build
   the same way becomes the very same nodes.
2. **State.** Every bit assigned in an edge-triggered block is a flop: its
   output is a free variable, its next-state function is what the block
   computes (asynchronous set/reset included, as written), and its trigger is
   the clock and async controls. A bit that a level-sensitive block does not
   assign on every path is a latch; that is decided by proof, so a full `case`
   without a `default` is plain logic.
3. **Matching.** Inputs, outputs and state elements are paired by name: exact,
   then normalized (`u_core.cnt[3]`, `u_core_cnt_reg_3_` and
   `\u_core/cnt_reg[3]` are one name), then by the cell instance that holds
   the state, then by the net on the flop's output pin, which also finds
   registers that synthesis stored inverted. `-match REF=IMPL` and
   `-matchfile` add pairs by hand.
4. **Proof.** Each matched pair of state elements is cut, which leaves a
   combinational question per compare point: every output, every next-state
   function, every trigger. A point is settled structurally (same node), by
   random simulation (a difference found), or by SAT with a conflict budget
   (`-effort`).

This is the standard sign-off method. It is sound: "equivalent" means that
from equal state the two designs produce equal outputs and move to equal
state, forever. It is incomplete where synthesis changed the state itself
(retiming, register merging or duplication, FSM re-encoding): those show up as
unmatched or failing points, never as a wrong "equivalent". Initial values
are not compared.

### Don't-cares

An X in the reference (an `'x` assignment, an out-of-range select, a divide by
zero, an undriven net, no arm of a `unique case` matching) means the
implementation may do anything there, which is the freedom synthesis is
given. X in the implementation is not forgiven: put the design with the
don't-cares on the `-ref` side.

### Black boxes

`-blackbox MOD` stops at module MOD on both sides (a RAM macro, an analog
block). Its output pins become free inputs and its input pins become compare
points, so the logic around it is still checked.

## Speed and threads

Reading the two designs runs on two threads; SAT runs on all logical cores
(`-threads N`, `-threads 1` to disable). Every compare point has its own
solver and its effort is counted in conflicts, not seconds, so the verdict,
the counterexamples and the report are the same on any machine and any
thread count.

## Options

| Option | Meaning |
|---|---|
| `-ref ...` / `-impl ...` | the two sides: files and front-end options |
| `-top NAME` | top module of both sides (`-reftop`, `-impltop` for one) |
| `-match R=I`, `-matchfile FILE` | pair a reference signal with an implementation signal |
| `-blackbox MOD` | do not look inside MOD |
| `-threads N` | worker threads (default: all logical cores) |
| `-effort N` | SAT conflicts allowed per compare point (default 2000000) |
| `-maxfail N` | failing points printed in detail (default 10) |
| `-report FILE` | every compare point and its result, as JSON |
| `-verbose` | unmatched signals, latches, and every compare point |
| `-q` | only the verdict |

### Datapaths

Two adders or multipliers built differently share no internal structure, and
SAT alone does not prove wide multipliers equal. So points that survive a
quick SAT pass go through SAT sweeping: internal nodes that simulate alike
are proved equal one at a time, smallest first, and merged, after which the
outputs usually fall out. For `a * b` mequiv also builds the product with the
rows in the other order and uses the fact that both are the same number, so a
netlist that adds partial products row by row is proved whichever operand it
walks (a 16x16 multiplier in under a second). A netlist with a different
multiplier architecture (Wallace tree, Booth) can still exhaust `-effort` and
report those outputs as aborted, which is inconclusive, never a pass.

## Not yet

- Liberty (`.lib`) cell functions read directly, sequential UDPs.
- Clock-gating cells treated as enables; retimed or merged registers.
- Tri-state buses and inout ports.

## The pieces

`src/equiv/` is header-only and usable as a library (namespace `equiv`):
`aig.hpp` (the graph), `bv.hpp` (bit vectors with X), `sat.hpp` and `cnf.hpp`
(the solver and the encoding), `elab.hpp` (a slang compilation to a `Design`),
`check.hpp` (`equiv::check(aig, ref, impl, options)` returns every compare
point with its status and counterexample).
