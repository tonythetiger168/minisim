# Formal property checking: `mformal`

`mformal` proves or falsifies the assertions of a design for every input
sequence, where a simulation only checks the sequences its testbench drives.
It reads the same SystemVerilog as `msim` (the slang front end and the same
lowering), so a property means the same thing in both tools.

```sh
mformal rtl/*.sv -top fifo                 # every assert, assume and cover in the design
mformal rtl/*.sv -top fifo -depth 40       # look 40 cycles deep (default 20)
mformal rtl/*.sv -top fifo -prop a_count   # one property
mformal rtl/*.sv -top fifo -tb -json r.json
```

```
mformal: clock clk, reset rst (active high, 1 cycle), 53 state bits, 598 gates, depth 20, 8 threads
  assert  top.u.a_count            proven  (induction, k=2)
  assert  top.u.a_push_counts      falsified  at cycle 5  mformal_out/top.u.a_push_counts.vcd
  assert  top.u.a_push_fills       proven  (induction with 4 lemmas, k=2)
  cover   top.u.c_full             covered  at cycle 4  mformal_out/top.u.c_full.vcd
mformal: 4 properties: 2 proven, 1 falsified, 1 covered  [0.12 s]
```

## What is checked

| Directive | Result |
|---|---|
| `assert property`, immediate `assert` (also deferred), `unique` / `priority` checks | **proven** (holds in every reachable state), **falsified** (with a trace), or **bounded** (no counterexample within `-depth`, no proof either) |
| `assume property`, immediate `assume` | a constraint: only input sequences that satisfy it are considered |
| `cover property`, `cover sequence` | **covered** (with a trace), **unreachable** (proven never to happen), or **bounded** |

Supported in properties: boolean expressions, `##n`, `##[m:n]`, `[*m:n]`,
`[->m:n]`, `[=m:n]`, `and`, `or`, `intersect`, `within`, `throughout`,
`first_match`, `|->`, `|=>`, `not`, `if` / `else`, `nexttime`, `always`,
`until`, `until_with`, `accept_on`, `reject_on`, `disable iff`, clocks with
`iff`, default clocking and default disable, named sequences and properties
with arguments, `$past`, `$rose`, `$fell`, `$stable`, `$changed`, `$sampled`,
`$countones`, `$onehot`, `$onehot0`.

Liveness: a property with a strong unbounded obligation (`s_eventually`,
`s_until`, `s_until_with`, a strong sequence with an unbounded repetition,
`always s_eventually`) is also checked for a loop the design can go round
forever without meeting it. Such a counterexample is reported with the cycles
that repeat. Strong operators that end by themselves (`s_nexttime`, a bounded
`strong(...)`) need no loop: on a run that never ends they pass or fail in
bounded time.

Fairness: the same obligation in an `assume` restricts the loops. A loop
counts only if every obligation of every assumption is met on it, so
`assume property (req |-> s_eventually !req)` rules out the requester that
never lets go.

Not decided: a strong obligation that a weak unbounded property could satisfy
in its place (`s_eventually always q`), or under `not` of an unbounded
property, `accept_on` or `reject_on`. The assertion is then reported
`bounded`, with "the safety part is proven; liveness not decided" when no
finite failure exists; such an assumption gives a warning and only its safety
part is used.

Local variables (16.10) are followed when each is assigned once: by its
declaration's initializer, or by a match item in the antecedent of an
implication at the top level of an assertion (`(req, x = d) |-> ##2 q == x`),
or anywhere in the sequence of a cover. The variable becomes a rigid unknown
(state bits that never change): the assertion is proven for every value, a
cover asks for some value. Skipped, with the reason: a variable assigned
twice or inside a repetition, a match item under `or`, `not`, `s_eventually`
or `until`, a subroutine call in a match item, local variables in an
assumption.

`s.triggered` (16.13.6) is the sequence's match in the current tick; the
sequence itself may not have local variables.

Reported as `skipped`, with the reason: multiclock properties, `.matched`,
concurrent assertions inside procedures, `expect`.

## The model

* **One clock.** Flops and assertions on the rising edge of one clock: the
  one the assertions use, or `-clock SIG`. One cycle of the model is one
  rising edge. An asynchronous reset is treated as a synchronous one.
* **Reset.** `-reset SIG` (active high) or `-reset_n SIG` (active low);
  without either, an input named `rst`, `reset`, `rst_n`, `resetn`, ... is
  used. The reset is held for `-reset-cycles` cycles (default 1) from a state
  where every flop is free, then released for good; assertions are checked
  from the first cycle after. `-no-reset` starts from the declared initial
  values (`logic q = 0;`, `initial q = 0;`), which are honoured in both modes.
* **Inputs** are free in every cycle: the top's input ports and every signal
  nothing drives.
* **Two-state.** X and Z read as 0. A property that fails in simulation only
  because of an X does not fail here.
* **Cut.** A process that cannot be modelled (a delay, a wait inside the
  body, another clock or edge, classes, automatic subroutines) is cut: what
  it drives becomes free in every cycle. A proof is still a proof (the model
  only has more behaviour), but a counterexample may be one the real design
  cannot show. The summary counts the cut signals and `-v` lists them.

### Abstraction by option

- `-blackbox NAME` leaves out every instance of the module `NAME` (or the
  one instance with that path): nothing inside is modelled, its outputs are
  free in every cycle, and properties inside it are reported as skipped.
- `-cut SIGNAL` makes one signal a free input in every cycle, whatever
  drives it.

Both make the model an over-approximation: a proof still holds for the real
design, while a failure may use a value the real design cannot produce (the
trace shows the free values; a replay in the simulator will not reproduce
such a failure). `-v` lists what was cut. Typical use: take a multiplier,
a memory or a deep counter out of the cone of a property that does not
depend on its exact value.

## How it works

1. Each process is executed symbolically once. A combinational process gives
   its outputs as functions of what it reads, a clocked one the next value of
   each flop. Values are vectors of literals of an and-inverter graph.
2. Each assertion becomes a monitor automaton. Its states are the terms an
   evaluation attempt can become (the derivatives `msim` computes at run
   time, here for every valuation of the property's boolean expressions),
   with one latch per term.
3. Per property, the cone of influence is unrolled into the SAT solver
   CaDiCaL. Three engines run in turn:
   * bounded model checking from the reset state finds counterexamples and
     cover traces up to `-depth`;
   * k-induction with simple-path constraints proves; what it cannot prove
     alone is tried again with the assertions already proven as lemmas;
   * PDR (IC3) takes what is left. It builds the invariant the proof needs,
     clause by clause, or finds a failure deeper than `-depth` (the trace
     then comes from a bounded check that deep). `-no-pdr` turns it off.
   The first pass is short (induction over at most 10 cycles, a small PDR
   budget), because PDR often proves at once what induction needs many
   cycles for; what is still open gets induction up to `-depth` and PDR's
   whole budget.
   Liveness is reduced to safety first. A property is a conjunction of
   obligations (the conjuncts of the terms its attempts are at), and a step
   turns an obligation into the conjuncts of its derivative. A free input
   saves the state once and picks one open obligation; the property fails
   when that state comes back with the picked obligation open again, down a
   chain of obligations each held open by a strong unbounded operator, and no
   obligation of an assumption that was open at the save still is.
4. Properties are independent jobs on `-threads N` threads (default: every
   logical core). The output is the same for any N. The solver stops at
   `-conflicts N` conflicts per call (a deterministic limit, not a
   timeout) and the property is then `inconclusive` or `bounded`.

## Traces

A falsified assertion or a covered cover writes
`mformal_out/<property>.vcd` (`-o DIR`): every signal of the design in every
cycle, the clock included, for any waveform viewer. Values are shown as the
rising edge samples them.

`-tb` also writes `<property>_tb.sv`, a testbench that drives the same
inputs into the design, so the failure can be debugged in the simulator:

```sh
msim rtl/*.sv mformal_out/top.u.a_push_counts_tb.sv -top mformal_replay
```

## Apps

`-app toggle` is unreachable-coverage analysis. For every bit of every
signal of the design (not the top-level inputs or the clock) it asks two
questions: can the bit be 0, and can it be 1, in some cycle after the reset?
Only the targets that are proven unreachable are listed (`-v` lists all):

```
  toggle  top.u.dead==1     unreachable  (induction, k=8)
  toggle  top.u.even[0]==1  unreachable  (induction, k=2)
mformal: toggle: 30 targets on 15 signal bits: 27 reachable, 3 unreachable
```

Such a bit is stuck: toggle coverage on it can never close in simulation, so
it is either dead logic or a coverage hole to waive. Assumptions apply, so a
bit that only toggles outside the constraints is reported too. Signals wider
than 4096 bits are skipped (`-toggle-max N`), and so are variables declared
inside a procedure (loop counters). A target that is neither hit nor proven
unreachable with a short budget is listed as bounded and counted as "not
decided" (a bit of a wide counter, say); `-toggle-deep` spends the whole
`-depth` and PDR budget on each of those. With `-Werror` an unreachable
target makes the exit status 1.

### Connectivity

`-connect FILE` proves a connectivity specification, the usual check for pad
multiplexers and top-level wiring. Each line is one connection:

```
# destination, source [, condition]
pad,       scan,       test_en
pad,       uart_tx,    !test_en && mode == 2'd1
dbg,       core.pc[7:4]
```

A connection is proven as "in every cycle after the reset, whenever the
condition holds, destination == source". Names are signals, with or without
the top's name, and may select bits (`sig[3]`, `sig[7:4]`); both sides must
have the same width. A condition is terms joined by `&&`, each `name`,
`!name`, `name == number` or `name != number`. A wrong connection is
falsified with a trace like any assertion. For each condition there is also
a cover, `<name>.enabled`: when the condition can never hold (so the
connection is only proven vacuously) it is listed as unreachable.
Assumptions in the design apply. An error in the file is a usage error.

## Other model checkers (AIGER)

`mformal ... -aiger model.aig` writes the model and stops without checking
(`.aig` is the binary format, any other name the ASCII one). The file is
AIGER 1.9: one bad-state property per assertion and per cover, in the order
of the report (`b0`, `b1`, ... in the symbol table carry the names; a cover
is "covered" when its bad state is reachable), and one invariant constraint
per assumption. Liveness is already turned into safety, and the reset
sequence is part of the latches, so the checker needs no extra setup. Any
checker that reads AIGER 1.9 with constraints can be used (ABC, rIC3 and
others); they run as separate programs. Use `-only NAME` to export a single
property for a checker that handles one at a time.

`tests/formal/aiger_check.py` reads the file back with its own small reader,
visits every reachable state and compares with mformal's verdicts.

## Exit status

0: nothing falsified. 1: an assertion was falsified (with `-Werror`, also an
unreachable cover or toggle target, an inconclusive or a skipped property). 2: a usage or
front-end error.

## Testing

`tests/mformal.cmake` checks the verdict of every property in
`tests/formal/`, replays the counterexamples in `msim`, and compares the
output across thread counts. `tests/formal/fuzz.py` generates random designs
and checks `mformal` against `msim`: every counterexample must fail in the
simulator at the same time, and nothing proven may fail under random stimulus.
