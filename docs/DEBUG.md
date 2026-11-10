# mdebug: source-level debug

`mdebug` answers the questions a waveform alone does not: who drives this
signal, who reads it, and which assignment gave it the value it has at this
time. It runs the same front end as `msim`, so it takes the same files and
options.

```sh
msim tb.sv dut.sv -top tb -vcd waves.vcd                 # a run to debug (or -fst waves.fst)
mdebug tb.sv dut.sv -top tb -drivers tb.u0.y             # who assigns it
mdebug tb.sv dut.sv -top tb -loads tb.y0                 # who reads it
mdebug tb.sv dut.sv -top tb -trace tb.y0 -depth 4        # the fan-in cone
mdebug tb.sv dut.sv -top tb -vcd waves.vcd -active tb.sum -at 70
mdebug tb.sv dut.sv -top tb -vcd waves.vcd -html debug.html
```

A signal is a hierarchical path (`tb.u0.y`). A tail that names one signal is
enough (`u0.y`). Every instance has its own signals: `tb.u0.r` and `tb.u1.r`
are answered separately.

`-vcd` takes a VCD or an FST (`msim -fst`); `-fst` and `-wave` are the same
option.

## Drivers and loads

A driver is any assignment that can give the signal a value: a continuous
`assign`, a procedural assignment, or a port connection. Each is printed with
its place, the process it is in, and the conditions on the way to it:

```
drivers of tb.u0.y: 2
  alu.sv:19: y <= '0  [always_ff @(posedge clk or posedge rst)]  when rst
  alu.sv:20: y <= r  [always_ff @(posedge clk or posedge rst)]  when !(rst) && en
```

`-loads` lists the same records read the other way: every assignment whose
right side, condition or select index names the signal. A load through a
condition is marked `(control)`.

`-trace SIG -depth N` prints the fan-in cone: the drivers, then the drivers of
what they read, N levels down, across module ports.

## The active driver

With a waveform, `-active SIG -at T` finds the assignment that gave the signal
its value at time T, then follows the cause back through time:

```
tb.sum = 8'h20 @70  (since 45)
  <- alu.sv:29: sum = y0 + y1  [assign]
    tb.y0 = 8'h11 @45
      <- alu.sv:27: alu u0 (.clk, .rst, .en, .op, .a, .b, .y(y0));  [port]
        tb.u0.y = 8'h11 @45
          <- alu.sv:20: y <= r  [always_ff @(posedge clk or posedge rst)]  when !(rst) && en
             with rst=0, en=1 @44
            tb.u0.r = 8'h11 @44  (since 42)
              <- alu.sv:15: r = a ^ b  [always_comb]  when op: default
```

How it decides:

- The signal's last change at or before T is found in the waveform.
- Each driver's `if` and `case` conditions are evaluated against the
  waveform. A flop's conditions are sampled just before the time its output
  changed; combinational logic is sampled at that time.
- A driver whose right side does not evaluate to the value seen is dropped
  when another one does.
- Combinational logic is followed through the inputs that changed at the same
  time; the others are listed as unchanged. A flop is followed through its
  data input at the sample time.

`<-` marks a driver whose conditions hold. `??` marks one mdebug could not
decide (a condition it cannot evaluate, or a signal that is not in the
waveform). `-depth N` sets how many steps back (default 3); without `-at` the
end of the waveform is used.

## The viewer

`-html debug.html` writes one self-contained page (no server, no network):

- the instance hierarchy and the signals of the selected instance;
- the source, with every signal clickable; the lines that drive the selected
  signal are highlighted, the lines that read it are tinted;
- the drivers and loads of the selected signal, each a jump to its line;
- a schematic of the selected signal: inputs, the assignments that drive it,
  the assignments that read it and what they drive. Clicking a signal moves
  the view there, so a cone is walked one step at a time;
- with `-vcd`: the active driver at the cursor, traced back four steps, in
  the page itself. Clicking a line of the trace moves to that signal and
  that time; the driver that ran is outlined in the driver list;
- with `-vcd`: waveforms, a time cursor, steps to the previous and next change,
  and the value of every signal at the cursor written next to its name in the
  source.

`-json file` writes the database the page reads (instances, signals, drivers
with their conditions, sources, waves).

## Speed and threads

mdebug uses every logical core by default (hyper-threads included):
`-threads N` sets the count, `-threads 1` turns it off. The front end parses
source files in parallel, and the waveform is read in parallel: the VCD is
mapped, cut at time stamps into one piece per thread, and each piece is
scanned without copying a value. The answers and the page are the same for
any thread count.

A 388 MB VCD (11,657 signals, `bench/gen_soc.py 512 2000`) on a 4-core
machine, for `-active` end to end:

| | seconds |
|---|---|
| first version (stream parser, one thread) | 9.5 |
| `-threads 1` | 2.4 |
| `-threads 2` | 1.3 |
| `-threads 4` | 0.9 |

An FST is decoded in time slices, one libfst reader per thread: a 19 MB FST
of the same run (14.7 million changes) reads in about 3 s on one thread and 1.1 s
on four.

The page from `-html` holds a budget of two million value changes. When the
waveform has more, each signal keeps the changes around `-at T` (or its first
ones), and mdebug says so.

## Limits

- In the page, a condition that reads an `x` or `z` value is undecided
  (`??`); the command line evaluates all four states.
- Unpacked arrays, structs and class objects are not in msim's VCD, so the
  active-driver trace stops at them with `(not in the waveform)`.
- Assignments made through task or function output arguments, and `force`,
  are not listed as drivers.
- Conditions inside loops, `fork` blocks and pattern matches are shown as
  `...` and are not evaluated.
- `-html` embeds at most 20000 value changes per signal (`-max-changes N`),
  and two million in all.
