# Design for test: scan insertion and memory BIST

`mdft` has three jobs. On RTL it checks scan readiness (rules `DFT001` to
`DFT006`, see the README). On a gate netlist it inserts scan chains. For a RAM
it generates a memory BIST controller. This page covers the last two.

## Scan insertion: `mdft -insert_scan`

```sh
mdft -insert_scan design.v -lib cells.lib -o design_scan.v \
     -chains 8 -scandef chains.json -tb flush_tb.v
```

The input is the structural netlist that synthesis writes (read with
`src/gnetlist.hpp`, hierarchy is flattened). The output is still a pure gate
netlist, with three new ports on the top module:

| Port | Direction | Meaning |
|---|---|---|
| `scan_en` | input | high: every scan flop takes its scan input (shift); low: normal operation |
| `scan_in` | input, one bit per chain | head of each chain |
| `scan_out` | output, one bit per chain | tail of each chain |

### Which cells

With `-lib`, cells come from the Liberty library (`src/liberty.hpp`):

- a cell with an `ff` group whose `next_state` is one pin is a flop; its clock
  edge, asynchronous `clear` and `preset` come from the group;
- a cell with a `test_cell` (`signal_type : test_scan_in` /
  `test_scan_enable`) is a scan flop, and it becomes the scan equivalent of
  the plain flop with the same clock edge, set/reset and outputs (the smallest
  one by area);
- buffers, inverters and 2-to-1 muxes are recognised from their functions,
  latches from their `latch` group, clock gates from
  `clock_gating_integrated_cell` and the `clock_gate_*_pin` attributes.

Without `-lib`, msynth's generic cells are assumed (`DFF`, `DFFR`, `DFFS`,
`DFFN`, `MUX2`, `BUF`, `INV`, `LATCH`, `ICG`), plus yosys' `$_DFF_*_` gates and
the sky130 `dfxtp`/`dfrtp`/`dfstp` flops with their `sdf*` scan cells.

`-cellmap FILE` adds cells or overrides either source, one cell per line:

```
flop     MYDFF   D=D CK=CP Q=Q QN=QN R=!CDN scan=MYSDFF
scanflop MYSDFF  D=D SI=SI SE=SE CK=CP Q=Q QN=QN R=!CDN
mux      MYMUX   A=I0 B=I1 SEL=S Y=Z
latch    MYLAT   D=D G=!EN Q=Q
icg      MYICG   CK=CP E=E TE=TE GCK=Q
buf      MYBUF   A=I Y=Z
inv      MYINV   A=I Y=ZN
```

`!` marks an active-low pin (or a falling-edge clock). A name also matches
with a drive-strength suffix (`MYDFF_X2`, `MYDFFD1`), and the scan cell gets
the same suffix.

### What is stitched

A flop is scanned when the tester can control it:

- its clock traces back to a top-level input through buffers, inverters and
  clock gates only (an inverter flips the edge);
- its asynchronous set and reset each trace back to a top-level input, or are
  tied inactive.

Anything else is left alone and reported with the reason (`not scanned:
div_reg: clock not controllable: 'cnt[0]' is driven by flop 'cnt_reg_0_'`).
Enable flops and synchronous-reset flops that are single library cells are
scanned only if the library has a scan equivalent, because a mux in front of
D would not shift while the enable is low. `-exclude 'u_core/dbg_*'` leaves
instances out by name.

Each scanned flop is either swapped for its scan cell (`-style auto`, the
default, when the library has one) or keeps its cell and gets a library mux
`<inst>_scan_mux` in front of D. `-style mux` and `-style cell` force one way.

A clock gate on the way to a scanned flop gets its test-enable pin tied to
`scan_en`, so the clock runs during shift.

### Chains

By default clock domains do not share a chain, and the two edges of one clock
are separate domains, so there are at least as many chains as domains.
`-chains N` asks for more; the extra chains go to the longest domains and
every domain is cut into equal parts. `-max_length L` instead caps the
length. Flops keep their netlist order inside a chain.

`-mix_clocks` lets domains share chains. Falling-edge flops are put first
(a falling-edge flop feeding a rising-edge one is safe; the other order would
lose a bit), and where the clock port changes a lockup latch is inserted: it
is transparent during the inactive half of the launching clock, so the next
domain sees each bit half a cycle late and clock skew cannot shift it twice.
The latch is a library cell (with an inverter if only the other polarity
exists). If the library has no latch, instances of `mdft_lockup` are used and
`-models FILE` writes their model.

### Outputs

- `-o FILE`: the netlist.
- `-scandef FILE`: the chain description that ATPG, logic BIST and
  compression read (JSON): scan-enable port, clock and reset ports with their
  off/active levels, each chain's ports and its cells in order from scan-in to
  scan-out (instance, cell, clock, edge, Q net, `cell` or `mux`), the lockup
  latches, and every unscanned flop with its reason.
- `-tb FILE`: a chain flush testbench. It holds `scan_en` high and the resets
  inactive, shifts a pseudo-random pattern into every chain and checks that it
  leaves `scan_out` exactly `length` clocks later. Run it with the cell models:
  `msim design_scan.v cells.v flush_tb.v -top <top>_scan_tb` prints
  `SCAN FLUSH PASS`.

`-threads N` sets the worker count (default: every core). Files are parsed in
parallel and flops are judged in parallel; the output is identical for any N.

## Memory BIST: `mdft -mbist`

```sh
mdft -mbist -name ram256x32 -words 256 -width 32 -backgrounds all \
     -o ram256x32_bist.v -tb ram256x32_bist_tb.v
```

generates, for one synchronous single-port RAM (`en`, `we`, `addr`, `wdata`,
`rdata`, read data `-latency` clocks after the read, default 1):

- `<name>_bist`: the controller. `bist_start` held high runs the test;
  `bist_done` rises at the end and `bist_fail` is sticky. The first failing
  read is kept in `bist_fail_addr`, `bist_fail_exp`, `bist_fail_got`, and
  `bist_fail_count` counts failing reads up to 255. Dropping `bist_start`
  rearms it. One memory operation is issued per clock (at speed), and the
  expected data travels through a pipeline as deep as the read latency.
- `<name>_bist_collar`: the controller and the muxes that give it the memory
  pins while `bist_busy`; the design's `func_*` pins pass through otherwise.

### Algorithms

`-algo` picks one (`mdft -mbist -list`); `-march` takes any element list.

| Name | Elements | Operations |
|---|---|---|
| `mats+` | `any(w0); up(r0,w1); down(r1,w0)` | 5N |
| `marchx` | `any(w0); up(r0,w1); down(r1,w0); any(r0)` | 6N |
| `marchy` | `any(w0); up(r0,w1,r1); down(r1,w0,r0); any(r0)` | 8N |
| `marchc-` (default) | `any(w0); up(r0,w1); up(r1,w0); down(r0,w1); down(r1,w0); any(r0)` | 10N |
| `marcha` | `any(w0); up(r0,w1,w0,w1); up(r1,w0,w1); down(r1,w0,w1,w0); down(r0,w1,w0)` | 15N |
| `marchb` | `any(w0); up(r0,w1,r1,w0,r0,w1); up(r1,w0,w1); down(r1,w0,w1,w0); down(r0,w1,w0)` | 17N |
| `marchlr` | `any(w0); down(r0,w1); up(r1,w0,r0,w1); up(r1,w0); up(r0,w1,r1,w0); up(r0)` | 14N |
| `marchss` | `any(w0); up(r0,r0,w0,r0,w1); up(r1,r1,w1,r1,w0); down(r0,r0,w0,r0,w1); down(r1,r1,w1,r1,w0); any(r0)` | 22N |

`w0` writes the data background and `w1` its complement. `-backgrounds K`
repeats the algorithm over K backgrounds (`0000`, `0101`, `0011`, `00001111`,
...); `all` uses log2(width)+1 of them, which tells every pair of bits in a
word apart and so covers coupling inside a word.

### Fault coverage

mdft carries a memory fault simulator and prints what the chosen test
detects. It injects every fault of each class into an 8 x 4 memory (the
classes do not depend on the size) and runs the algorithm from both power-up
states; a fault counts only if the test fails from both.

```
mdft: ram_bist: 64 x 8, any(w0); up(r0,w1); up(r1,w0); down(r0,w1); down(r1,w0); any(r0)
  10N operations, 1 background: 642 clocks
  fault coverage (every fault on a 8 x 4 memory):
    stuck-at                                 64 / 64     100.0%
    transition                               64 / 64     100.0%
    inversion coupling, between words      1792 / 1792   100.0%
    inversion coupling, within a word       192 / 192    100.0%
    idempotent coupling, between words     3584 / 3584   100.0%
    idempotent coupling, within a word      192 / 384     50.0%
    state coupling, between words          3584 / 3584   100.0%
    state coupling, within a word           192 / 384     50.0%
    address decoder                          56 / 56     100.0%
    total                                  9720 / 10104   96.2%
```

With `-backgrounds all` the same test reaches 100%. MATS+ finds the stuck-at
and address faults but half of the transition faults, as the literature says.
The faults are simulated on every core (`-threads N`); the numbers do not
depend on N.

### Testbench

`-tb FILE` writes a self-checking testbench: up to sixteen collars, each on a
behavioural RAM with one injected fault (none, stuck-at, transition, the
three coupling kinds between and within words, address decoder). The expected
verdict and first failing address of each come from the fault simulator, so
the test passes only if the generated hardware catches exactly what the
simulator predicts, including the faults a weak algorithm misses.

```sh
msim ram256x32_bist.v ram256x32_bist_tb.v -top ram256x32_bist_tb
# MBIST TB PASS: 16 memories, 15 faulty ones caught, 15362 cycles
```

## Not yet

Scan: test points, wrapper cells, hierarchical (per-block) chains, reordering
from placement. Memory BIST: dual-port and write-mask memories, one
controller shared by several memories, repair (BISR). Compression (EDT),
logic BIST, boundary scan and IJTAG are separate parts of mdft.
