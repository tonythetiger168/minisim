# Boundary scan and IJTAG in `mdft`

`mdft` covers two test-access standards:

- **IEEE 1149.1 boundary scan**: it wraps a design with a TAP controller and a
  boundary register, writes the BSDL file, checks any BSDL file, and writes
  the device tests a BSDL file allows.
- **IEEE 1687 IJTAG**: it reads ICL, writes the scan network as RTL, and
  retargets PDL (instrument-level `iWrite` / `iRead`) to scans of the whole
  network.

Tests come out as SVF for a tester and as a self-checking SystemVerilog
testbench, so every generated design can be simulated against its own tests:

```sh
mdft -bscan rtl/chip.sv -top chip -tb chip_tb.sv -svf chip.svf
msim rtl/chip.sv chip_bscan.sv chip_tb.sv -top chip_tb      # prints "chip_tb: PASS"
```

The code is in `src/dft/`: `jtag.hpp` (patterns, SVF, testbench, TAP RTL),
`bscan.hpp`, `icl.hpp`, `pdl.hpp`, `lbist.hpp`, `edt.hpp`, `diag.hpp`, `cli.hpp`.

## Boundary scan insertion: `mdft -bscan`

```sh
mdft -bscan rtl/*.sv -top chip [-bscan_out DIR] [-bscan_idcode 149511]
     [-bscan_tristate d=d_o,d_oe,d_i] [-bscan_highz] [-bscan_intest]
     [-bscan_clock PORT] [-bscan_exclude PORT] [-bscan_user NAME] [-bscan_ir N]
```

writes `chip_bscan.sv` and `chip_bscan.bsdl`. The wrapper `chip_bscan` has the
ports of `chip` plus `tck`, `tms`, `tdi`, `tdo`, `trst_n`, and holds:

- the 16-state TAP controller, an instruction register (capture value
  `...01`), the 1-bit BYPASS register and the 32-bit IDCODE register;
- one boundary cell per pad bit. Cell 0 is next to TDO; the first port's most
  significant bit is next to TDI.

| Port | Cell | BSDL |
|---|---|---|
| input | captures the pad; INTEST can drive the core | `BC_1 input` |
| clock (`-bscan_clock`, and every clock mdft finds from the flops) | observe only: nothing is added to the clock path | `BC_4 clock` |
| output | captures the core; EXTEST and CLAMP drive the pad | `BC_1 output2` |
| output with `-bscan_highz` | as above, with one shared enable cell | `BC_1 output3` + `control` |
| `-bscan_tristate PAD=OUT,OE[,IN]` | a bidirectional pad built from the core's out / enable / in ports: enable cell, output cell, input cell (`!OE` for an active-low enable; an enable as wide as OUT gives one enable cell per bit) | `control`, `output3`, `input` |
| inout | observe only; the pad stays connected to the core | `BC_4 observe_only` |
| `-bscan_exclude` | none | port only |

Instructions: EXTEST, SAMPLE and PRELOAD (one opcode), IDCODE, CLAMP, BYPASS
(all ones); HIGHZ when there is a 3-state driver; INTEST with `-bscan_intest`.
`-bscan_user NAME` adds an instruction that selects a register outside the
wrapper (an IJTAG network): the wrapper gets `name_sel` and `name_so`, and the
shared `ijtag_ce`, `ijtag_se`, `ijtag_ue`, `ijtag_si`, `ijtag_rst`; BSDL lists
them as linkage.

With `-tb` or `-svf` the tests below are written for the new wrapper at once.

## BSDL: `mdft -bsdl`

```sh
mdft -bsdl chip.bsdl [-Werror] [-svf chip.svf] [-tb chip_tb.sv]
```

reads a BSDL file (entity, ports, pin map, TAP attributes, instructions,
IDCODE, boundary register) and checks it:

| Rule | What it finds |
|---|---|
| `BSD001` | A mandatory attribute is missing or malformed |
| `BSD002` | Instruction register: capture value not ending in `01`, opcode of the wrong length, one opcode for two instructions, a mandatory instruction missing, BYPASS without all ones |
| `BSD003` | IDCODE register: not 32 bits, bit 0 not 1, register without instruction or the reverse |
| `BSD004` | Boundary register: a cell number missing or outside `BOUNDARY_LENGTH`, a malformed cell |
| `BSD005` | A cell on a port that does not exist, an index out of range, a cell function that contradicts the port direction, a cell on a linkage port |
| `BSD006` | A 3-state cell with no control cell, a control reference that is not a control cell, a bad disable value or result |
| `BSD007` | A TAP port is missing or is not a single bit of the right direction |
| `BSD008` | A port with no boundary cell |
| `BSD009` | The pin map lacks a port, or has the wrong number of pins for it |

The tests written from a BSDL file:

1. the instruction register captures its `INSTRUCTION_CAPTURE` value;
2. IDCODE is selected after reset and by its instruction, and reads the code;
3. BYPASS is one bit and captures 0;
4. the boundary register is exactly `BOUNDARY_LENGTH` long;
5. SAMPLE captures two opposite patterns driven on every input pin;
6. PRELOAD then EXTEST puts two opposite patterns on every output pin;
7. each control cell turns its drivers off (the pin reads `z`), and the pad
   then works as an input;
8. HIGHZ floats every 3-state pin; CLAMP holds the preloaded values; both
   select BYPASS.

Pin drives and pin checks are in the testbench; SVF has no portable form for
them, so they appear there as comments.

## IJTAG: `mdft -icl`

```sh
mdft -icl net.icl [-icl_top Top] [-icl_rtl net.sv] [-ijtag_tap]
     [-pdl test.pdl] [-iproc all] [-svf t.svf] [-tb net_tb.sv]
```

**ICL** supported: `Module`, `ScanInPort`, `ScanOutPort`, `DataInPort`,
`DataOutPort`, the control ports, `ScanRegister` (`ScanInSource`,
`CaptureSource`, `ResetValue`, `RefEnum`), `ScanMux`, `DataMux`,
`LogicSignal`, `Instance` (`InputPort`, `Parameter`), `Alias`, `Enum`,
`Parameter`, `LocalParameter`, with `$param` expressions in ranges.
`ScanInterface`, `AccessLink`, `Attribute` and `NameSpace` are read and
ignored. `DataRegister` (addressable registers) is not supported.

**RTL** (`-icl_rtl`): one flat module `<Top>_ijtag` with `tck, si, so, sel,
ce, se, ue, rst`, an input for each top-level `DataInPort` and for each
instrument output (`sens_done`), and an output for each top-level
`DataOutPort` and each instrument input (`sens_en`). A register shifts,
captures and updates only while it is on the active scan path. `-ijtag_tap`
adds `<Top>_ijtag_top`: the network behind a TAP, selected by the instruction
IJTAG (`0010`).

**PDL** is Tcl with the i-commands: `iProcsForModule`, `iProc`, `iCall`,
`iWrite`, `iRead`, `iApply`, `iReset`, `iRunLoop`, `iNote`, `iPrefix`. The
Tcl subset: `set`, `expr`, `if`, `for`, `foreach`, `while`, `incr`, `proc`,
`return`, `break`, `continue`, `puts`, `list`, `lindex`, `llength`,
`lappend`, `format`, `string`. `name[3]` and `name[7:0]` are bit selects.
`iScan` is not supported.

**Retargeting**: targets are named at instrument level (`iWrite en 1` inside
an `iProc` of the instrument's module, called as `iCall sens.measure`); mdft
traces each one to the scan register bits that drive or capture it. Each
`iApply` then issues capture-shift-update scans until every pending write and
read is done: scan muxes on the way to a target (SIBs) are opened one level
per scan, and registers already on the path are written in the same scan.
SIBs are left open afterwards. The testbench also acts as the instruments: it
drives what an `iRead` expects on the instrument's outputs and checks every
instrument input after each `iApply`.

```
mdft: IJTAG network Top: 5 scan registers (19 bits), 3 scan muxes, 5 top instances
mdft: 8 iApply, 13 scans, 145 scan bits, 232 TCK cycles
```

A network of 3000 SIBs with 3000 32-bit registers (99,000 scan bits) and
1001 `iApply` retargets to 99 million scan bits of SVF in about 5 s on one
core. These flows are single passes over small inputs; `-threads` is accepted
and has nothing to parallelize here.

## Logic BIST: `mdft -lbist`

```sh
mdft -lbist design_scan.v cells.v -top chip -scandef chains.json -lbist_sign
     [-lbist_patterns 1024] [-lbist_prpg 32] [-lbist_misr 32] [-lbist_seed HEX]
     [-lbist_clock PORT] [-lbist_out DIR] [-lbist_dump loads.txt]
```

takes a scan-inserted design and its chain description (the JSON that
`mdft -insert_scan -scandef` writes: scan enable, clocks, resets, and each
chain's `scan_in`, `scan_out` and `length`) and writes a STUMPS logic BIST:

- `chip_lbist_ctrl`: a PRPG (an LFSR on a primitive polynomial), a phase
  shifter (one XOR of three PRPG bits per chain, a distinct set each), a space
  compactor and a MISR, and the controller. It fills the chains, then for each
  pattern captures for one clock and shifts the response into the MISR while
  the next pattern shifts in: `(patterns + 1) * longest chain + patterns + 1`
  clocks in all.
- `chip_lbist`: the design with the controller around it, and five more
  ports: `lbist_start` (a level: hold it until done), `lbist_rst_n`,
  `lbist_busy`, `lbist_done`, `lbist_pass`, `lbist_signature`. While
  `lbist_start` is high every clock of the scan description is the BIST
  clock, the resets are held off, the scan enable and scan inputs come from
  the controller, and every other input is held at 0. With `lbist_start` low
  the design is untouched, so tester scan still works.
- `chip_lbist_tb.sv`: runs the BIST once and prints the signature.
- `chip_lbist.icl` and `chip_lbist.pdl`: IJTAG access (a register with start,
  done, pass and the signature, and an `iProc run`), for `mdft -icl`.

`-lbist_sign` simulates the wrapper with `msim` and writes the signature into
the RTL as the `GOLDEN` parameter. A signature with unknown bits is an error:
an X source (an unscanned flop, a memory, an undriven input) reaches a scan
cell, and it has to be scanned or blocked first.

The polynomials are found at run time, not tabulated: a candidate with the
fewest terms is kept when x has order 2^n - 1 modulo it, which makes it
primitive (2^n - 1 is factored with Pollard's rho). Any width from 8 to 64
works; the same options always give the same hardware.

`-lbist_dump` writes what each pattern loads into each chain (`pattern chain
bits`, scan-in cell first), the input a fault simulator needs to grade the
BIST. Fault coverage and test points are not reported yet: they wait for the
fault simulator of the ATPG work, which this will call instead of bringing a
second one. Primary outputs are not observed during BIST.

### Fault coverage: `mdft -lbist_coverage`

How much of the design do the pseudo-random patterns test? `-lbist_coverage`
takes the same options as `-lbist` (so the PRPG, seed and pattern count are
the same), works out what every scan cell holds before each capture, and
grades those patterns on the scan-inserted gate netlist with the shared
fault simulator:

```
mdft -lbist_coverage -scandef chains.json -lbist_patterns 4096 chip_scan.v -lib cells.lib -top chip
```

```
mdft: logic BIST coverage of big: 4096 patterns, 16 chains, 6000 scan cells observed, port inputs at 0
  ...
  total faults                               494544
  test coverage                              91.61%
  fault coverage                             72.61%
  coverage by pattern count:  64: 71.80%  128: 77.18%  256: 81.75%  512: 85.15%  1024: 87.81%  2048: 89.90%  4096: 91.61%
  2.09 s on 4 threads
```

It grades under the conditions the wrapper creates: port inputs are 0,
resets are off, and only scan cells are observed, so logic that only drives
port outputs counts as not observed. The curve shows where more patterns
stop paying. `-lbist_faults FILE` lists every fault with its class (`ND`
faults are where test points would help), `-lbist_json FILE` writes the
numbers, and `-lbist_min_coverage P` fails the run under P percent. A scan
cell that captures an unknown value is reported, because it makes the
signature unrepeatable. That run (60,000 gates, 6,000 flops) takes 5.4 s on
one thread. The run uses every core (`-threads N`) and gives the
same result for any thread count.

The grade is checked in the test suite against the real wrapper: faults
graded detected change the simulated signature, faults graded not detected
do not.

## Scan compression: `mdft -edt`

```sh
mdft -edt design_scan.v cells.v -top chip -scandef chains.json [-edt_channels N] [-edt_ring N]
     [-edt_cubes cubes.txt -edt_patterns patterns.txt] [-edt_fill random|0] [-threads N]
mdft -edt_encode -scandef chains.json -edt_cubes cubes.txt -edt_patterns patterns.txt
```

A few tester channels feed all the scan chains, so the tester stores and
shifts far less data. `-edt` reads the same chain description as `-lbist` and
writes `chip_edt.sv`:

- `chip_edt_decomp`: a ring generator (an LFSR on a primitive polynomial that
  the input channels are XORed into at evenly spaced bits every shift cycle)
  and a phase shifter (a distinct XOR of three generator bits per chain);
- `chip_edt_comp`: an XOR compactor, chain i to output channel i mod N;
- `chip_edt`: the design with both, and `edt_mode`, `edt_update`,
  `edt_channels_in`, `edt_channels_out`. With `edt_mode` low the scan pins
  work as before (bypass).

One pattern is one clock with `edt_update` high (the generator clears; this
is the capture cycle), then `longest chain + init` shift clocks, where the
init cycles (`ring width / channels`) let the generator reach any state
before the bits that stay in the chains are produced.

**Encoding.** A test cube is a pattern in which ATPG fixed only the cells it
needs (`x` elsewhere). Every scan cell is a linear function over GF(2) of the
channel bits, so a cube is one linear equation per care bit; mdft solves it by
Gaussian elimination and fills the free channel bits at random (or with 0).
Each result is decompressed again and compared with its cube before it is
written. A cube with too many care bits, or too many in the same few shift
cycles (the decompressor gains only one free bit per channel per cycle), is
reported as `unencodable`, for ATPG to split.

Cube file: `<pattern> <chain> <bits>`, the scan-in cell first, `x` for free;
chains not listed are all free. Pattern file: `<pattern> in <channel> <bits>`
(first shifted first) and `<pattern> load <chain> <bits>` (the full pattern
the chains receive, which is what a fault simulator grades).

Cubes are encoded in parallel on every core (`-threads N` to choose), and the
output is identical for any thread count. 2000 cubes for 400 chains of 250
cells on 16 channels (100,000 cells, 378 care bits on average, 3000 at most,
24.6x less tester data): 6.7 s on one thread, 1.6 s on four.

`chip_edt_tb.sv` plays the first encoded patterns (or random channel data)
through the wrapper in shift mode and checks every output channel bit: a
chain in shift mode is a shift register, so what leaves it is what the
decompressor put in `length` cycles before. That checks the decompressor, the
compactor, the wiring and the chain lengths against the encoder's model.

Not there yet: X masking in the compactor (an unknown response bit spoils its
output channel for that cycle), expected compacted responses (they need the
good-machine responses from fault simulation), and reading the ATPG tool's
own pattern format directly.

`matpg -cubes FILE` writes cubes the encoder reads as they are: it numbers
the chains in its own order and names their scan-in ports in a `chains` line,
which the encoder uses to find each chain in the scan description, and its
`<pattern> pi <bits>` lines (the port inputs) are copied to the compressed
pattern file.

## Failure diagnosis: `mdft -diagnose`

When a part fails scan test, the tester's fail log says which scan cells and
outputs came back wrong in which patterns. Diagnosis works back from that to
the faults that would produce exactly those failures.

```
matpg chip_scan.v -lib cells.lib -top chip -patterns pats.txt
mdft -diagnose fails.log -patterns pats.txt chip_scan.v -lib cells.lib -top chip
```

It reads the gate netlist the way `matpg` does (`-lib`, `-top`, `-se`) and
uses the same fault simulator (`src/faultsim.hpp`), so a fault means the same
thing in both tools. No SystemVerilog front end is involved.

The fail log has one failing observation per line:

```
# comment
top chip              optional, checked against the netlist
last 412              optional: the tester stopped after pattern 412
17 3 250              pattern 17, chain 3, cell 250 unloaded the wrong value
17 po done            pattern 17, output port bit `done` was wrong
```

`matpg`'s testbench writes this format when it is run with `+faillog`
(`<top>.faillog`), so a simulated faulty netlist can be diagnosed the same way
as a tester log:

```
matpg chip_scan.v -top chip -patterns pats.txt -tb chip_tb.v
msim chip_tb.v chip_faulty.v cells.v +faillog
mdft -diagnose chip.faillog -patterns pats.txt chip_scan.v -top chip
```

Patterns count from 0 in pattern file order (the stuck-at set first, then the
transition set). Chains count from 0 in the order `matpg` prints them. Cells
count from 0 at the scan-in end, and a failure refers to the value the cell
captured, so inversions along the chain are already taken out.

The report ranks the suspects:

```
mdft: diagnosis of chip: 2856 failing bits in 1396 of 4677 stuck patterns, 75 candidate faults
  defect 1: explains 2738 of 2856 failing bits
    rank   score   tfsf   tfsp   tpsf  fault
       1   95.9%   2738    118      0  sa0 U6609/A
                                         = sa0 U6609/B
                                         = sa1 U6609/Y
       2   95.4%   2738    118     14  sa0 U4040/Y
  defect 2: explains 118 of 118 failing bits
       1  100.0%    118      0      0  sa1 U14941/A
```

- `tfsf`: bits that fail on the tester and with this fault (explained).
- `tfsp`: bits that fail on the tester but not with this fault.
- `tpsf`: bits that pass on the tester but would fail with this fault.
- `score` is `tfsf / (tfsf + tfsp + tpsf)`. Lines with `=` are faults no test
  can tell apart from the one above (the same equivalence class).

When the best suspect leaves failures unexplained, what it explains is taken
out of the log and the rest is diagnosed as a further defect, up to
`-diag_defects` (default 4). The exit code is 1 when failures remain that no
reported fault explains, 2 on bad input.

| Option | Meaning |
| --- | --- |
| `-diagnose FILE` | the fail log |
| `-patterns FILE` | the patterns that were applied (`matpg -patterns`) |
| `-diag_fault stuck\|transition` | fault model and pattern set (default `stuck`) |
| `-diag_defects N` | diagnose up to N defects (default 4) |
| `-diag_suspects N` | list up to N suspects per defect (default 10) |
| `-diag_json FILE` | the report as JSON |
| `-diag_inject 'sa0 PIN'` | simulate this fault instead of reading a log (repeatable); `str`/`stf` with `-diag_fault transition` |
| `-diag_log FILE` | with `-diag_inject`: write the fail log and stop |
| `-threads N` | default every core; the report is the same for any N |

How it works: only a fault in the fan-in of every failing point of a pattern
can explain that pattern alone, which gives the candidates. All of them are
simulated on the first 64 patterns that hold failures; the 256 that score
best there are simulated on every pattern. Blocks of 64 patterns run in
parallel, one fault simulator per thread.

Limits: one fault at a time is simulated, so two defects whose effects meet
at the same observation point in the same pattern are only partly explained.
`-diag_inject` with several faults makes the union of their separate logs,
not the log of a part with both. Stuck-at diagnosis uses the stuck-at
patterns and transition diagnosis the transition patterns; failures in the
other set are counted and left out. Bridges, opens and chain (shift)
failures are not modeled yet. Logs from compressed patterns need the
compactor mapped back to cells first, which `-edt` does not do yet.

Speed: a 60,000-gate, 6,000-flop netlist with 4,677 patterns and two
injected defects diagnoses in 2.3 s on one thread and 0.6 s on four.
