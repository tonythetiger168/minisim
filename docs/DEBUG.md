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

In the waves, as in a waveform viewer:

| | |
|---|---|
| click | move the cursor |
| shift+click, or `m` | put the marker there; the bar shows the marker and the time from it to the cursor |
| wheel | zoom around the pointer; `fit` shows the whole run |
| `r` | radix of the selected row: hex, unsigned decimal, signed decimal, binary |
| Del | take the selected row out of the waves |
| Ctrl+← / Ctrl+→ | previous / next change of the selected signal |
| find value | the next time the selected signal takes a value (`fe`, `'hfe`, `'d-2`, `'b1x`); shift+Enter for the previous |

The view (rows, their radix, the zoom window, cursor and marker) is kept in
the page address, so a reload shows it again and a copied link (with
`-serve`, or the same `.html` file) opens on the same view.

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

## Block view

`-blocks INST` lists the instances inside INST: each port with the net it
connects to (or the expression, for a constant), and which instance output
feeds which instance input. In the page, selecting an instance in the
hierarchy draws the same as a block diagram: instances are placed left to
right in the order data flows, wires join an output to the inputs it feeds,
and nets that come from the scope's own logic are labelled at the port.
Click a block to go into it, a net name to select the signal.

## Structs and part assignments

A struct, packed or unpacked with integral fields, is one vector in the
waveform. mdebug reads a field as its slice of that vector, so a condition
such as `if (p.lo == 4'd3)` is evaluated, in the tool and in the page. Each
assignment to a field or to a constant bit or part select (`s.f`, `v[3]`,
`v[7:4]`) knows the bits it writes: in one process a later assignment
replaces an earlier one only where it writes the same bits, so every field's
own driver is reported, and a whole assignment (`p = '0;`) that the fields
then overwrite is not.

A read of one field, or of fixed bits, follows only that part. For
`u = p.hi + 4'd1` the trace goes on as `tb.p.hi = 4'h4`, with the time those
bits last changed (not the time another field moved) and only the assignments
that write them. The same holds for `q[3:0]`, in the tool and in the page.

## State machines

    mdebug design.sv -top tb -fsm                      # every state machine
    mdebug design.sv -top tb -vcd run.vcd -fsm tb.u -at 40

`-fsm` lists the state machines of the design, or those under an instance, or
one state register. They come from the extractor that `msim -cov` and mlint
use (`src/fsm_extract.hpp`), so the three tools agree on what a state machine
is. Each one is shown with its states (the reset state marked) and its
transitions with the line that makes each:

    tb.u.state  (ctrl, 2 bits, 4 states)  fsm.sv:8
      states: IDLE=0(reset) RUN=1 WAIT=2 FAIL=3
      IDLE -> RUN  fsm.sv:15  taken 1 time, first @25
      RUN -> FAIL  fsm.sv:16  never taken
      state FAIL never entered
      @40: WAIT  (since 35)  after IDLE@0 RUN@25

With a waveform it also says how often each transition ran and when first,
which never ran, which states were never entered, the state at `-at T` with
the states before it, a transition that ran but is not in the source's list,
and a value that is not a state. In the page, selecting a state register
draws its state diagram: the state at the cursor is marked, transitions that
ran are bright, and an arrow opens its source line.

## Assertions

    msim design.sv -top tb -vcd run.vcd > run.log
    mdebug design.sv -top tb -assert                              # the assertions
    mdebug design.sv -top tb -vcd run.vcd -log run.log -assert    # and why each one failed

`-assert` lists the assert, assume and cover directives of the design, or
those under an instance, or one by name. With `-log` (msim's output, which
has a line `Error: "file", line: name: at time T` per failure) it says which
failed and when. With a waveform as well it explains one failure of each, the
first, or with `-at T` the last at or before T:

    tb.u.a_gnt  assert property  arb.sv:5
      a_gnt: assert property (@(posedge clk) disable iff (rst) req |=> gnt);
      failed 1 time: @55
      failure @55, values sampled before each clock tick:
                       @25       @35       @45       @55
        tb.u.rst         0         0         0         0
        tb.u.req         1         0         1         0
        tb.u.gnt         0         1         0         0
      what drove them:
        tb.u.gnt = 0 @54  (since 35)
          <- arb.sv:4: gnt <= rst ? 1'b0 : req && !hold  [always_ff @posedge clk]

The table has the signals the assertion reads at its last four clock ticks,
sampled before the edge as a concurrent assertion samples them; an immediate
assertion shows the values at the failure. Below it is the active-driver
trace of each signal. The log's time and the waveform's must be in the same
unit. `-json` and the page's database carry the assertions and their failure
times (`asserts`).

### Assertions in the waveform

    msim design.sv -top tb -fst run.fst -vcd-sva

writes two counters for each assertion, assume and cover: `tb.u.a_gnt.passes`
and `tb.u.a_gnt.fails` (32 bits), how many attempts have passed (a cover:
matched) and failed so far. Each failure is a step in `.fails` at the time it
is reported, so the wave shows where to look and `-active` / `-assert` say
why. `$fsdbDumpSVA` in the testbench (or `"+sva"` in `$fsdbDumpvars`) turns
it on as well. Assertions in protected code are left out. The counters are
written at the end of each time slot; `-compile` runs the interpreter.

## Coverage

    msim design.sv -top tb -cov cov.json
    mdebug design.sv -top tb -cov cov.json -drivers tb.u.next
    mdebug design.sv -top tb -cov cov.json -uncovered tb.u
    mdebug design.sv -top tb -cov cov.json -vcd run.vcd -html debug.html

`-cov` reads msim's coverage database (one run, or several merged with
`mcov merge`). Every driver line then says how often its source line ran:

    fsm.sv:15: next = RUN  [always_comb]  when state == IDLE && go  (line ran 3 times)
    fsm.sv:19: next = IDLE  [always_comb]  when state == FAIL  (line never ran)

so a trace shows at a glance which of a signal's drivers the tests never
reached. The count is the line's: a line with a condition and an assignment
counts when the condition was evaluated. `-uncovered [INST]` lists the
assignments on lines that never ran and the signals with bits that never rose
or never fell, in the design or under one instance. In the page, the line
numbers of lines that ran have a green edge and those that never ran a red
one, with the count as a tooltip, and a selected signal shows its toggle
gaps. Reports and merging stay in `mcov`.

## Low power (UPF)

    msim tb.sv dut.sv -top tb -upf dut.upf -upf_verbose -vcd run.vcd > run.log
    mdebug tb.sv dut.sv -top tb -upf dut.upf -power
    mdebug tb.sv dut.sv -top tb -upf dut.upf -log run.log -vcd run.vcd -power PD_CORE -at 60
    mdebug tb.sv dut.sv -top tb -upf dut.upf -log run.log -vcd run.vcd -active tb.u_dut.u_core.cnt -at 60

`-upf` reads the power intent with msim's own reader, so both see the same
domains (`-power_top INST` as for msim; without it the UPF's
`set_design_top`). `-power [DOMAIN]` lists each power domain: the instances
in it, its primary supply set, and its isolation, level-shifter and
retention strategies with their lines in the UPF file. With `-log`, the
output of a run with `-upf_verbose`, it adds the domain's power states over
time, the state at `-at T`, and the supply or switch change that caused it:

    PD_CORE  dut.upf:7
      elements: tb.u_dut.u_core  (1 instance)
      primary supply: SS_CORE  ground=VSS  power=VDD_CORE
      isolation ISO_0: outputs, clamp 0, on u_core/cnt u_core/q, enabled by iso_en(high)  dut.upf:30
      states: @0 CORRUPT @2 NORMAL @45 CORRUPT @66 NORMAL @90 CORRUPT
      @60: CORRUPT  (since 45)  power switch SW_CORE state off; supply VDD_CORE = OFF

The active trace uses the same data. A value that is x because its domain
has no power is reported as that, with the cause, and no assignment is
blamed for it; a port with an isolation strategy says so, with the clamp
value and the enable:

    tb.u_dut.u_core.cnt = 8'bxxxxxxxx @60  (since 45)
      power domain PD_CORE is CORRUPT since 45 (power switch SW_CORE state off; supply VDD_CORE = OFF); isolation ISO_0 on this port, clamp 0, enabled by iso_en (high)
      x because the domain has no power: no assignment made this value

### Power in the waveform

    msim tb.sv dut.sv -top tb -upf dut.upf -fst run.fst -vcd-power

adds, under a scope `upf` in the top (`tb.upf`), each power domain's simstate (9.6) and each supply
net's state and voltage:

| Signal | Value |
|---|---|
| `tb.upf.PD_CORE.simstate` | 0 NORMAL, 1 CORRUPT_STATE_ON_CHANGE, 2 CORRUPT_STATE_ON_ACTIVITY, 3 CORRUPT_ON_CHANGE, 4 CORRUPT_ON_ACTIVITY, 5 CORRUPT |
| `tb.upf.VDD_CORE.state` | 0 OFF, 1 FULL_ON, 2 PARTIAL_ON, 3 UNDETERMINED |
| `tb.upf.VDD_CORE.mV` | the voltage in millivolts (0 while OFF) |

so a switch-off is visible next to the signals it corrupts. `"+power"` in
`$fsdbDumpvars` turns it on as well. A supply net is listed by the net that
drives its subnet (a switch output, a supply port).

## Large designs: the served viewer

    mdebug design.sv -top tb -fst run.fst -serve          # http://127.0.0.1:8765/
    mdebug design.sv -top tb -fst run.fst -serve 0        # any free port; the address is printed

The one-file page of `-html` carries its waves inside it, so it has a budget
(2 million value changes over all signals; long waves are cut around
`-at T`). `-serve` has no such budget: mdebug stays running as a web server
on this machine, the page holds the design, and the waves of a signal are
sent the first time the page shows that signal, whole (up to 4 million
changes per signal, around `-at T` beyond that). Everything else in the page
is the same: trace, schematic, state diagrams, coverage marks.

mdebug reads a waveform on demand: at start only the list of signals, then
the changes of a signal the first time a query or the page needs it (one
pass over the file per request; the page of `-html` reads blocks of 2048
signals and keeps only the cut waves). So the memory a session needs grows
with the signals looked at, not with the size of the waveform. Measured on a
4-core machine with `bench/gen_soc.py 1024 20000` (1024 cores, 2178
instances, 23305 signals, a 384 MB FST of 200,000 ns):

| | before | now |
|---|---|---|
| `-script`: `value`, `active`, `changes` on a few signals | out of memory at 13.6 GB | 1.0 s, 113 MB |
| `-serve`: page ready / one signal's waves | out of memory | 0.5 s / 0.05 to 0.08 s, 157 MB |
| `-html` (23305 signals cut to 90 changes each) | out of memory | 13 s, 1.6 GB, 60 MB page |

    python3 bench/gen_soc.py 1024 20000 > soc.sv
    msim soc.sv -top soc_tb -fst soc.fst
    mdebug soc.sv -top soc_tb -fst soc.fst -serve

The server listens on 127.0.0.1 only and answers only requests addressed to
`127.0.0.1` or `localhost`, so another machine, or a web page from another
site, cannot read the design through it. To look from another machine, use
an SSH tunnel (`ssh -L 8765:127.0.0.1:8765 host`). Stop it with Ctrl-C.

## Scripts and other programs

    mdebug design.sv -top tb -vcd run.vcd -script queries.txt
    mdebug design.sv -top tb -vcd run.vcd -script -          # commands on the standard input

`-script` loads the design and the waveform once, then runs one command per
line and answers each with one line of JSON, so a script, a test or an AI
agent can ask many questions of one process. Lines that start with `#` are
skipped. The exit status is 1 when a command failed.

| Command | Answer |
|---|---|
| `drivers SIG`, `loads SIG` | `drivers` / `loads`: `file`, `line`, `text`, `kind`, `when`, `target` (and `control` for a load) |
| `value SIG [T]` | `value` (`8'h43`), `bits`, `since`, at T or at the end |
| `changes SIG [T0 [T1]]` | `changes`: `[time, bits]` pairs, at most 10000, then `more` is true |
| `active SIG [T [DEPTH]]`, `trace SIG [DEPTH]`, `blocks INST`, `fsm [NAME] [T]`, `assert [NAME] [T]`, `uncovered [INST]`, `power [DOMAIN] [T]` | `text`: the lines the option of the same name prints |
| `scopes`, `signals [SCOPE]` | the instances; the signals (directly inside SCOPE) |
| `end` | `end` time and `timescale` of the waveform |
| `help`, `quit` | the command list; stop |

    {"cmd":"value tb.u.state 40","ok":true,"signal":"tb.u.state","time":40,"value":"2'h2","bits":"10","since":35}
    {"cmd":"value nope","ok":false,"error":"no signal 'nope' in the design"}

`tools/mdebug_api.py` wraps this for Python (installed under
`share/minisim/python`):

    from mdebug_api import Mdebug
    with Mdebug(["design.sv", "-top", "tb"], wave="run.vcd") as d:
        print(d.value("tb.u.state", 40)["value"])
        for drv in d.drivers("tb.u.state"):
            print(drv["file"], drv["line"], drv["text"])

## Lint findings

    mlint design.sv -top tb -json lint.json
    mdebug design.sv -top tb -lint lint.json -html debug.html     # or -serve

The page gets a Lint list at the top of the right panel (filter by
severity, or show the waived ones), and every line with a finding is marked
in the source: red for an error, amber for a warning, blue for info, with the
findings as the line's tooltip. A click on a finding opens its line and
selects the signal it is about, so its drivers, loads, schematic and waves
are one step away. The signal is the finding's `signal` field (a
hierarchical name) when mlint gives one, else a name quoted in its text
(`'q' is driven but never read`) that is declared in the finding's file,
nearest above its line. A finding in a file that is not part of the design
is listed with no line to open.

In a script, `lint` answers with every finding, its file, line, rule,
severity, signal and whether it is waived.

## Memories

msim leaves unpacked arrays out of the waveform unless asked:

    msim design.sv -top tb -vcd run.vcd -vcd-arrays        # arrays of up to 1024 elements
    msim design.sv -top tb -fst run.fst -vcd-arrays 4096

Each element is then a signal of its own, `tb.mem[3]`. `-active` follows a
read `q = mem[addr]` to the element `addr` selected at that time, and from
there to the write `mem[waddr] <= d` whose index was that element, with the
index and data it had. `-active tb.mem[3]` starts at one element. Without
`-vcd-arrays` the trace stops at the memory and says so.

## Class objects

A waveform holds signals, and a class object is not one: `new` makes it,
handles that change point to it, and it is gone when nothing does. With
`-vcd-class` msim writes what each class handle variable of a module points
to:

    msim tb.sv -vcd run.vcd -vcd-class        # tb.p.addr, tb.p.next
    msim tb.sv -fst run.fst -vcd-class 2      # and tb.p.next.addr

Every property of the handle's declared class (base class first) is a
signal under a scope named after the handle, `tb.p.addr`, that follows the
handle: when `tb.p` moves to another object the views change with it, and
they are `x` while it is null. A handle property such as `tb.p.next` is the
handle value itself; `-vcd-class N` follows it N levels down. Views are
written at the end of each time slot, and are the same for any `-threads`.

mdebug reads them like any signal: `value tb.p.addr 12` and
`changes tb.p.addr` in a script, `-signals` lists them, and in the page,
picking `tb.p` adds a wave row for each of its properties.

Left out: string, real, event, array and queue properties, handles in
automatic variables or only inside other objects (a UVM component tree;
use `msim -i` and `print tb.h.next.id`, or `mdebug -tr` for transactions).
`-compile` runs the interpreter when `-vcd-class` is given.

## Verdi, VCS and Xcelium waveform tasks

A testbench written for Verdi, VCS or Xcelium dumps with their own tasks.
msim compiles them and writes FST: the vendors' formats (FSDB, VPD, SHM)
are their own and closed, so the file of the same name with `.fst` is
written instead, and msim says so once:

    msim: wave.fsdb: FSDB, VPD and SHM are the vendors' own formats; writing wave.fst (open it with mdebug, GTKWave or Surfer)

| Tasks | What msim does |
|---|---|
| `$fsdbDumpfile`, `$vcdplusfile`, `$shm_open`, `$fsdbAutoSwitchDumpfile`, `$recordfile` | name the file (`.fst`); without one, `novas.fst`, `vcdplus.fst` or `waves.fst` |
| `$fsdbDumpvars`, `$fsdbDumpvarsES`, `$fsdbDumpSingle`, `$vcdpluson`, `$shm_probe`, `$recordvars` | as `$dumpvars`: a depth, instances or signals (by name or as strings, `"tb.dut"`); `+fsdbfile+NAME` names the file, `+all` and `+mda` add memories, `+sva` and `+power` add assertions and power states (`-vcd-sva`, `-vcd-power` above), other `+` options are accepted |
| `$fsdbDumpMDA`, `$fsdbDumpMem`, `$vcdplusmemon` | memories too, as `-vcd-arrays` |
| `$fsdbDumpon` / `off`, `$vcdpluson` / `off`, `$recordon` / `off` | as `$dumpon` / `$dumpoff` |
| `$fsdbDumpflush`, `$vcdplusflush` | as `$dumpflush` |
| `$fsdbDumpFinish`, `$vcdplusclose`, `$shm_close`, `$recordclose` | flush and stop |
| `$fsdbDumpSVA` | assertions in the waveform, as `-vcd-sva` |
| `$fsdbSuppress`, `$fsdbLog`, `$vcdplusautoflushon`, ... | accepted, nothing to do |

`msim -i` and `-do` take Verdi's UCLI commands the same way:
`fsdbDumpfile`, `fsdbDumpvars [DEPTH] [SCOPES] [+all]`, `fsdbDumpon`,
`fsdbDumpoff`, `fsdbDumpflush`, `fsdbDumpFinish`, `fsdbDumpMDA`. `-vcd` /
`-fst` on the command line wins over all of them, as over `$dumpvars`.
mdebug, mlint and the other tools compile the same sources.

A Verdi PLI library loaded with `-load` / `-loadvpi` that registers these
names itself keeps them. A second `$fsdbDumpvars` with more scopes after the
dump has started adds none (an FST's signals are fixed when it starts);
msim says so once.

## Tasks, calls and force

A task or function declared in a module drives the signals it assigns, as
`[task name]`. A call drives each output, inout and ref argument from the
arguments that go in, as `[..., call]`, and the arguments that go in drive
the task's formals, so the fan-in cone crosses the call. `force` and the
procedural `assign` are listed as `[..., force]` and `[..., assign]`. A net
declared with a value (`wire n = a + b;`) is a continuous assignment.

## UVM transactions

`-tr FILE` reads the transactions UVM recorded: the text database
`uvm_text_tr_database` writes (`tr_db.log` by default) when
`recording_detail` is on, for sequences and sequence items started on a
sequencer and for every `begin_tr`/`end_tr` in a driver or monitor. Turn it
on in the testbench and run msim as usual:

```systemverilog
uvm_config_db#(uvm_bitstream_t)::set(null, "*", "recording_detail", UVM_FULL);
```

```
msim -uvmhome $UVM_HOME tb.sv -vcd tb.vcd
mdebug tb.sv -vcd tb.vcd -tr tr_db.log -trans
uvm_test_top.e.ag.sqr.s  6 transactions  (0 to 100000)
uvm_test_top.e.ag.d.drive  5 transactions  (10000 to 100000)
uvm_test_top.e.ag.m.observed  5 transactions  (15000 to 100000)
mdebug tb.sv -tr tr_db.log -trans -at 35000
uvm_test_top.e.ag.d.drive at 35000:
  #5 t  @30000 to 40000  a='h22 b='ha sum='h0  (in #0 s on uvm_test_top.e.ag.sqr.s)
uvm_test_top.e.ag.m.observed at 35000:
  #6 t  @35000 to 40000  a='h22 b='ha sum='h2c
```

`-trans STREAM` lists one stream (its full name or a unique tail such as
`ag.d.drive`); `-at T` keeps the transactions in progress at `T`, so the
driver's item and the monitor's view of the same bus cycle sit side by
side. Each line gives the fields the item recorded and the sequence it ran
in. The page draws each stream as a row under the waves, one lane per
transaction in flight; a click on a transaction shows its fields and
parent and moves the cursor to its start. Times are the simulation's, the
same as the waveform's. In `-script`: `trans [STREAM] [T]`.

## Interactive simulation (msim -i)

`msim -i` (also `-ucli`, and `xrun -tcl`) stops before time 0 and reads
commands, like VCS UCLI or the xrun Tcl shell. `msim -do FILE` (and `xrun
-input FILE`) reads them from a file instead; when the commands run out, the
simulation goes on to its end, so a `-do` file can set stops and checks and
leave the rest of the run alone.

The commands run on the tool line's Tcl interpreter (`src/tcl.hpp`), so
`set`, `if`, `foreach`, `while`, `proc`, `expr`, `source`, `puts` and
`[command]` substitution work around them, and a command's result (`get`,
`scope`, `describe`, `senv`) can be used in a script:

```
msim design.sv -i
msim> stop -line 23 -file ucli.sv
stop 1 set on line ucli.sv:23
msim> run
time 12: stop 1: ucli.sv:23  add(3);
msim(stopped)> next
time 12: next: ucli.sv:24  #200 $display("q=%0d sum=%0d", q, sum);
msim(stopped)> step
time 15: step: ucli.sv:20  always #5 clk = ~clk;
msim(stopped)> run 3
time 18: ran to time 18 (next event at 20)
msim> stop -condition {q == 3 && en}
msim> run
time 35: stop 2: {q == 3 && en}
msim> foreach s {clk en} { puts "$s=[get $s -radix dec]" }
msim> print tb.head.next.id
tb.head.next.id = 32'h2
```

| Command | What it does |
|---|---|
| `run [T [unit]] [-absolute]` | run until a stop, for `T` (`100`, `100ns`, `100 ns`), to time `T`, or to the end (`run`, `run -all`) |
| `run -posedge SIG`, `-negedge SIG`, `-change SIG` | run until the signal rises, falls or changes |
| `run -slot [N]` | run `N` time slots (one by default) |
| `step`, `next` (`run -step`, `run -next`) | run to the next statement of any process, or of the process stopped in (a task call is stepped over) |
| `stop SIG [posedge\|negedge\|== VALUE]`, `stop -posedge/-negedge/-change SIG`, `stop -create -object SIG` | stop when the signal changes, rises, falls, or takes the value |
| `stop -line N [-file F]` | stop before the statement at the line runs (in any process, and in tasks and functions); the file defaults to the current statement's |
| `stop -absolute T`, `stop -relative T`, `stop -time T` | stop at a time |
| `stop -condition {EXPR}` | stop when the condition becomes true; on a signal or line stop, stop only while it holds. Signals are read by name, numbers may be sized (`4'hc`); an `x` makes it false |
| `... -command {CMDS}`, `-continue`, `-once` | run commands at the stop; go on instead of stopping; delete the stop after it hits |
| `stop`, `stop -show` | list the stops |
| `stop -delete/-disable/-enable N ... \| all`, `delete N` | remove, turn off, turn on |
| `get SIG ... [-radix hex\|dec\|sdec\|bin\|oct]` | the value as the command's result, for scripts |
| `print NAME ...`, `value NAME` | a signal, a class handle (class name, object id and every property, inherited ones included) or a path through handles such as `tb.head.next.id` |
| `force SIG VALUE`, `release SIG` | as the `force` and `release` statements |
| `deposit SIG VALUE`, `change SIG VALUE`, `deposit SIG = VALUE` | set the value once; the design's next assignment overwrites it |
| `scope [PATH]`, `scope -up`, `scope -set PATH` | the current scope: names are looked up there first, then from the top |
| `show [-scopes\|-signals\|-ports] [-value] [SCOPE]` | the instances, generate blocks, signals or ports of a scope |
| `describe NAME ...` | kind, type, width, parameter value and where it is declared |
| `search PATTERN`, `signals [TEXT]` | the signals whose path matches the glob, or contains the text |
| `where`, `stack` | the statement stopped at (or last run) and its process |
| `listing [N]`, `listing -file F -line L` | the source around the current statement |
| `dump -file F`, `dump -add NAMES [-depth N]`, `probe -create NAMES`, `dump -off/-on/-flush/-close` | write a VCD from the next time slot: scopes (everything below, `-depth` levels) and signals, as `$dumpvars` |
| `drivers SIG ...`, `loads SIG ...` | the statements of the simulation that write or read the signal: file, line, source and process (an `assign`, a port, an `always`, a task) |
| `call TASK(ARGS)`, `call TASK ARG ...` | run a task or function of the design (static or automatic) from the current scope; prints what it returns and its `output` arguments. While stopped at a line it runs when the simulation goes on |
| `save FILE` (`checkpoint FILE`) | write a checkpoint: as `$save`, by replay; it records the commands given so far |
| `restore FILE` | go back to a `save` checkpoint: msim starts again, runs the saved command line and commands quietly up to it, and reads the next command after `restore` (from the same `-do` file, or the standard input) |
| `restart` | start the simulation again from time 0, reading the next command after `restart` |
| `senv [time\|scope\|top\|timescale\|file\|line\|stopped]` | the state of the simulation |
| `alias NAME COMMAND ...` | a new command |
| `time`, `help`, `quit`, `exit`, `finish` | `quit` ends the simulation now; final blocks still run |

Values are decimal or sized literals (`8'h40`, `4'b10x1`). Names can leave
out the top module (`cnt` for `tb.cnt`). `-i` and `-do` use the
interpreter, so they turn off `-compile`, and they run on one thread so that
a stop can pause a process in the middle. Lines that start with `#` are
comments.

`msim -restore FILE` also takes a checkpoint written by `save`; the commands
then come from the standard input. A restore costs a run up to the
checkpoint (docs/CHECKPOINT.md), and a command typed into a pipe after
`restore` is lost (a file or a terminal goes on where it was).

## Limits

- In the page, a condition that reads an `x` or `z` value is undecided
  (`??`); the command line evaluates all four states.
- Class objects are in msim's waveform only through module handle
  variables (`-vcd-class`, above), and they have no drivers, so the
  active-driver trace stops at a property with `(not in the waveform)`.
  Memories need `-vcd-arrays`
  (above); the page's own trace does not follow memory elements yet, the
  command line does.
- A task's formal arguments are not in the waveform, so the active-driver
  trace shows an assignment inside a task that depends on one as undecided
  (`??`); `-trace` follows the argument to every call.
- `release` is not a driver: after it the trace goes back to the ordinary
  drivers by value.
- Conditions inside loops, `fork` blocks and pattern matches are shown as
  `...` and are not evaluated.
- `-html` embeds at most 20000 value changes per signal (`-max-changes N`),
  and two million in all.
