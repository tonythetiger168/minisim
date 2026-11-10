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
`bscan.hpp`, `icl.hpp`, `pdl.hpp`, `cli.hpp`.

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
