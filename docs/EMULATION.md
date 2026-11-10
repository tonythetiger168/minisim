# Emulation and FPGA prototyping: `memu`

`memu` takes a synthesized design and puts it on a board of FPGAs. It does
the part of an emulator compiler that sits between synthesis and FPGA place
and route:

| Step | What it does |
|---|---|
| Clock conversion | Every flop moves onto one free-running clock, `emu_clk`, and keeps its value unless its own clock had an edge. Gated, divided and multiplexed clocks, both edges and several clock domains need no clock routing, and there are no hold violations between FPGAs. |
| Memory mapping | Memory cells become FPGA RAM: block RAM for clocked reads, LUT RAM for small clockless reads, block RAM read on `emu_clk` for large clockless ones. |
| Partitioning | The cells are split over the FPGAs, within each one's capacity, so that as few signals as possible cross and those that do cross few boards. |
| Routing and pin multiplexing | Each crossing signal takes the shortest way over the board's wires, through other FPGAs where two are not wired directly. A link with fewer wires than signals carries several signals per wire in turn. |
| Probes | Chosen nets are sampled once per event into a trace buffer the host reads back; `memu -trace` turns it into a VCD. |
| Transactors | Synthesizable bus bridges that stop the design's clock while the host answers, so a C model (`msim -mvp`) or a testbench class sits on the other side. |

There is no FPGA in the loop when you run it: the output is Verilog, and
`memu` writes a testbench that runs the original netlist next to the
emulation and compares every output at every event. Any simulator can run it,
so a partition, a memory mapping or a transactor is checked before hardware
exists.

`memu` writes `<top>_fpga<k>.v` (one per FPGA), `<top>_emu_lib.v` (the clock
generator and the memories), `<top>_emu.v` (the board), `<top>_emu.f` (those
three, for a simulator that takes file lists), `<top>_emu_tb.sv`,
`<top>_emu.rpt` and, with probes, `<top>_probes.txt`.

```sh
memu soc.gv -lib cells.lib -mem mems.txt -board board.txt -o out
msim cells.v mems.v soc.gv out/soc_fpga*.v out/soc_emu_lib.v out/soc_emu.v out/soc_emu_tb.sv -top soc_emu_tb
# memu: PASS, 2000 events (2000 with a clock edge), 80856 output bits compared, netlist and emulation agree
```

## Where it sits in the tool line

```
RTL --msynth--> gate netlist + Liberty --memu--> one netlist per FPGA --FPGA synthesis, place and route--> bitstreams
```

`memu` reads structural Verilog (the reader every netlist tool shares,
`src/gnetlist.hpp`) over a Liberty library (`src/liberty.hpp`): which cells
are flops and latches, and which pins are clocks, comes from the library, not
from cell names. It does not synthesize, check equivalence or time anything
itself: those are `msynth`, `mequiv` and `msta`. Each `<top>_fpga<k>.v` it
writes is a netlist of the same library cells plus a few lines of plain
Verilog, which an FPGA flow (`mfpga`, Yosys, a vendor tool) maps given the
cells' Verilog models. The outputs of the tests here pass `yosys synth`.

## Clock conversion

The design's clocks do not exist on the FPGAs. A generator, one copy per
FPGA and all in step, keeps a schedule of every clock's edges and walks from
one edge to the next. One step is an **event**: the clocks that have an edge
at that instant move, the design settles, and the inputs may change.

A flop `always @(posedge clk) q <= d` becomes a flop on `emu_clk` that loads
`d` when its clock rose in this event and holds otherwise. Falling-edge cells
are the same on the other condition; latches and asynchronous set and reset
pins stay as they are. How "its clock rose" is known depends on the design:

- **Strobes.** If every clock input reaches only clock pins, through nothing
  but buffers and inverters, the generator simply says which clocks rise and
  fall (`emu_pos`, `emu_neg`), and edges nothing listens to are skipped. A
  design with one clock and rising-edge flops runs at one `emu_clk` cycle per
  clock cycle.
- **Edge detection.** Otherwise (a gated clock, a divided clock, a clock that
  is also read as data) the clock is a real signal and each flop's clock net
  is compared with its previous value. An event then takes several **deltas**:
  one for the clocks to move, more for derived clocks to follow, and more for
  the logic after the inputs change. `memu` works the numbers out from the
  depth of the clock network and prints them; `-settle N` overrides them.

A delta is one `emu_clk` cycle unless a combinational path crosses a
multiplexed link or a large clockless-read memory, in which case it is as
many cycles as the slowest such path needs. The report says what one cycle of
each clock costs:

```
clocks: edge detection
  clk: period 10 ns, also read as data or gated; one cycle is 8 emu_clk cycles, 6.25 MHz at 50 MHz emu_clk
timing: 2 deltas after a clock event, 1 after the inputs change, 1 emu_clk cycle per delta: 4 emu_clk cycles per event
```

Clocks are found (inputs that reach a clock pin) or named:
`-clock NAME[:PERIOD_NS[:PHASE_NS]]`. Periods and phases set the order of the
edges of unrelated clocks, exactly: two clocks of 10 ns and 14 ns have their
edges interleaved the way a simulator would have them.

## The wrapper

`<top>_emu` is the board: the FPGAs wired as the board file says, with the
design's ports (minus the clocks) and

| Port | |
|---|---|
| `emu_clk`, `emu_rst` | the only clock, and a synchronous reset of the emulation itself |
| `emu_ready` | high in the last cycle of an event: sample the outputs and change the inputs at that edge |
| `emu_run` | low holds the design at the end of an event; its time does not pass |
| `emu_pos`, `emu_neg` | per clock: the edges of the event that starts at this edge |
| `emu_next_pos`, `emu_next_neg`, `emu_next_time` | the edges and the time the next event will have, whether or not `emu_run` lets it start |
| `emu_time` | the design's time, in the unit the report prints |

## Memories

A memory cell is not in the Liberty library in any usable way, so `-mem FILE`
describes each one:

```
memory SRAM16X8 depth=16 width=8 [init=file.hex]
port clk=CLK addr=A din=D dout=Q we=WE cs=CS     # a read/write port
memory RF16X8 depth=16 width=8
port clk=WCLK addr=WA din=D we=WE                # a write port
port addr=RA dout=Q                              # a clockless read port
```

Pins: `clk addr din dout we cs re wmask`; a `!` before a pin name makes it
active low. A port with a clock reads on that clock (block RAM); a read port
without one is LUT RAM up to `-lutram-max` bits (default 4096) and above that
a block RAM read on `emu_clk`, which costs a cycle per delta.

## The board

```
board quad
util 0.7            # fill each FPGA to at most this fraction
tdm_max 16          # most signals one wire may carry in turn
fpga A lut=200000 ff=400000 bram_kb=10000
fpga B lut=200000 ff=400000 bram_kb=10000
link A B pins=240   # wires between two FPGAs, both directions together
```

or `-fpgas N -lut N -ff N -bram-kb N -pins N` for N equal FPGAs with every
pair wired. `-assign GLOB=FPGA` pins instances to an FPGA, `-io FPGA` puts
every port of the design on one.

Size is an estimate, since the real number comes from FPGA synthesis: a LUT
per `-gates-per-lut` library gates (default 3), a flop per flop, the bits of
each block RAM.

## Partitioning

The netlist is a hypergraph: a vertex per cell and per port bit, an edge per
net. The cost of a partition is, for every net, the number of board hops
from its driver's FPGA to each FPGA that reads it, so on a board that is not
fully wired the partitioner prefers neighbours. The method is multilevel:
cluster cells that share small nets until the graph is small, split the small
graph several ways and keep the best, then un-cluster level by level and at
each level move cells between FPGAs while that lowers the cost
(Fiduccia-Mattheyses passes that also accept a run of bad moves if what
follows makes up for it). Capacity is a limit, not a goal: nothing asks for
equal halves.

`-starts N` (default 16) runs N such attempts from different seeds and keeps
the best. That is what the threads are for.

## Probes and waveforms

```sh
memu soc.gv ... -probe 'u_core.pc*' -probe irq -trace-depth 4096 [-trigger NET[:N]]
```

Each FPGA samples its probed nets into a ring buffer at the end of every
event; FPGA 0 keeps the time of each sample. With `-trigger`, sampling stops
N events after the net is 1, so the buffer holds what led up to it. The host
reads the buffers through the `emu_dbg_*` ports (the testbench does, with
`+trace=FILE`), and

```sh
memu -trace out/soc_probes.txt samples.txt -o wave.vcd
```

writes a VCD in the design's own time.

## Transactors

`emu/sv/memu_xtor.sv` (`memu -xtor-sv` prints its path) has two synthesizable
bridges that go on the board beside the design. Both use the same idea: at
`emu_ready` they look at what the next event is, and when it is an edge of
their clock and they need the host, they hold `emu_run` low until the host
has answered. The design sees an answer that took no time.

**The design is the master: `memu_axil_slave_xtor`.** It answers an AXI4-Lite
master port of the design cycle for cycle like `mvp_axil_slave`, the
simulation model of a virtual prototype (docs/VPROTO.md), and passes each
access to the host as a request. `memu_mvp_host` (`memu -xtor-mvp-sv`) is the
host end in simulation: it calls the same C models, gives them the design's
time, and drives the interrupt lines.

```sh
msim -mvp $(memu -xtor-mvp-sv) cells.v out/soc_fpga*.v out/soc_emu_lib.v out/soc_emu.v soc_tb.sv ram.c uart.c -top soc_tb
```

`tests/emu/xsoc_tb.sv` runs one design both ways, simulated with
`mvp_axil_slave` and emulated on two FPGAs with the transactor, and both print

```
xsoc: done after 177 cycles, sum fff83aac, waited 99 for the interrupt
```

**The host is the master: `memu_axil_master_xtor`.** It takes one command at
a time (read, write, or let N cycles pass) and the design's clock runs only
while there is one, so a test sees the same cycle counts on every run.
`memu_host_if` is the host end: an interface with `read`, `write` and `idle`
tasks, which a UVM driver holds as a `virtual memu_host_if`:

```systemverilog
class reg_driver extends uvm_driver #(reg_item);
  virtual memu_host_if #(8, 32) bus;
  task run_phase(uvm_phase phase);
    forever begin
      seq_item_port.get_next_item(req);
      if (req.write) bus.write(req.addr, req.data, req.resp);
      else           bus.read(req.addr, req.data, req.resp);
      seq_item_port.item_done();
    end
  endtask
endclass
```

The transactors also run in plain simulation (`emu_ready` and `emu_next_pos`
tied high, `memu_sim_clock` making the design's clock), so the same test
drives the RTL and the emulation; `tests/emu/xreg_tb.sv` does, and the two
logs are compared byte for byte.

On a real board the request and response ports go to whatever link the host
has (UART, JTAG, PCIe). Nothing depends on how long the link takes.

## Threads and speed

`memu` uses every logical CPU by default (`-threads N` to choose) for the
partitioning attempts. The result does not depend on the number of threads:
the attempts are independent, and the best one wins by (overflow, cost,
lowest seed).

On a generated netlist of 910,000 cells split over 4 FPGAs
(`bench/emu/run.sh`), on 4 cores: reading 4.7 s, partitioning 9.7 s (36 s on
one thread), writing 0.3 s. Against KaHyPar (k-way, connectivity objective)
on the same hypergraphs the cut is 7% larger on a 100-block netlist and 35%
larger on a 500-block one, in less time; KaHyPar balances the parts, which
`memu` does not need to.

## What it does not do yet

- The partitioner does not look at timing: a combinational path may cross
  between FPGAs more often than it must, which raises the cycles per delta on
  multiplexed links.
- Tristate buses between FPGAs, and designs whose clocks form a combinational
  loop (`-settle N` gives those a fixed number of deltas).
- Memory cells with more than one write clock per port, and FPGA vendor
  constraints (pin locations, the `emu_clk` source): those belong to the
  board support files of the FPGA flow.
