# bench

Designs and scripts for measuring minisim tools; see the bench-speed skill
(`.claude/skills/bench-speed`) for the method. This page covers the SoC
generator.

## gen_manycore.py: tiled many-core with shared memory

```sh
python3 bench/gen_manycore.py --help
python3 bench/gen_manycore.py --mesh 8x8 > mc.sv && msim mc.sv -top mc_tb
python3 bench/gen_soc.py --tiles 8x8          # the same, through gen_soc.py
```

The N-core designs of `gen_soc.py` below are N copies that never talk to
each other, which makes them an easy case for a simulator: one module
repeated, and every core busy on every clock. `gen_manycore.py` builds a chip
the way tiled many-cores such as OpenPiton are built (our own RTL, nothing
taken from OpenPiton): an X x Y mesh of tiles, each with

* a RISC-V core: `mcrv` (our own RV32I, `bench/manycore/mc_rv.sv`, the
  default, needs no download), `picorv32`, `femtorv32`, or a mix
  `cva6` (OpenHW CVA6 / Ariane, see below), or a mix (`--core mix` is a
  checkerboard of PicoRV32 and FemtoRV32; a comma list such as
  `--core cva6,mcrv,picorv32` repeats over the tiles),
* 4 KB of private RAM with the program,
* a private L1 data cache (direct-mapped, write-through),
* one slice of the distributed shared L2 (lines homed by address mod N), with
  a limited-pointer directory (`--dir-ptrs`, default 4) that invalidates the
  sharers on a write and broadcasts once a line has more sharers than pointers,
* three routers, one per physical network as in OpenPiton: NoC1 requests,
  NoC2 data, write acks and invalidations, NoC3 invalidation acks
  (single-flit packets, XY routing, round-robin arbitration).

The program is the same in every tile. In each of `--rounds` rounds core i
runs a xorshift loop whose length depends on i (so some cores compute while
others wait at the barrier spinning in their L1), writes its value to a shared
array, joins a barrier (fetch-and-increment, then the last arrival releases a
flag), and mixes in the values of tiles i+1 and i+X. Core 0 then folds every
core's result into a checksum. The generator computes the same checksum in
Python, and the testbench prints PASS only when they agree:

```
mc: tiles=64 mesh=8x8 cores=mcrv done=64/64 cycles=13667 l2_requests=928 checksum=99886ab5 expect=99886ab5 PASS
```

So a run checks the NoC, the coherence protocol and the cores, not only that
nothing hangs. `tests/msim_manycore.cmake` runs three small meshes on msim
with 1 and 4 threads.

| file | contents |
|---|---|
| `manycore/mc_router.sv` | 5-port router, 2-deep input FIFOs |
| `manycore/mc_l1.sv` | L1 data cache, NoC1/NoC3 inject, NoC2 eject |
| `manycore/mc_l2.sv` | L2 slice, directory, invalidation and ack counting |
| `manycore/mc_tile.sv` | core port decode, private RAM, MMIO, the three routers |
| `manycore/mc_rv.sv` | the mcrv core |

Measured (2026-11-17, 4-core VM, `--rounds 2`, mcrv, msim interpreter vs
Verilator 5.054 `-O3`; seconds; every run printed PASS with the same checksum):

| mesh | tiles | cycles | msim interp | Verilator build | Verilator run |
|---|---|---|---|---|---|
| 4x4 | 16 | 11,101 | 4.1 | 22.1 | 0.2 |
| 8x8 | 64 | 13,667 | 28.0 | | |

### CVA6 (Ariane) tiles

```sh
python3 bench/manycore/fetch_cva6.py cva6dir      # pinned CVA6 + cvfpu + hpdcache, config, file list
python3 bench/gen_manycore.py --core cva6 --mesh 4x4 > mc.sv
verilator --binary -Wno-fatal -Wno-lint -Wno-style -Wno-UNOPTFLAT --top-module mc_tb -f cva6dir/cva6.f mc.sv
```

CVA6 is not copied into minisim; the script clones it (Solderpad 0.51,
hpdcache Solderpad 2.1, cvfpu's T-Head divider Apache-2.0) and derives a
config from `cv32a6_imac_sv32`: RV32IMAC with its own I-cache and
write-through D-cache, where only the private RAM is cacheable (so every
shared access goes through the tile's coherent L1) and the
fetch-and-increment alias and MMIO are non-idempotent. `bench/manycore/mc_cva6.sv`
turns its AXI4 port into the tile port. With CVA6 the program gets FENCEs
around each barrier (RVWMO), so CVA6 cannot share a mesh with FemtoRV32,
which does not decode FENCE.

On Verilator 5.054 1x1, 2x2, 4x4 and a 3x3 mix of CVA6, mcrv and PicoRV32
print PASS. msim (since #690 fixed the reversed range at
`core/cva6_mmu/cva6_mmu.sv:405`) prints the same checksums: 1x1 in 6 s and
2x2 in 42 s on the interpreter (`msim -f cva6dir/cva6.f mc.sv -top mc_tb`).
Keep the MMU in the config: with `MmuPresent = 0` CVA6 checks
non-idempotence against the previous access's address, issued the
fetch-and-increment load speculatively and then replayed it, and the
barrier hung.

`msim -compile` gives the same result on 4x4 but its build took 9 min 40 s,
against 4 s for the interpreter: this design is the next compiled-mode case.

## gen_soc.py: SoC of N cores

```sh
python3 bench/gen_soc.py --help
python3 bench/gen_soc.py [--core synthetic|ibex|picorv32|femtorv32] N [CYCLES]
```

`synthetic` (the default) is our own generated pipeline core. The other three
are real, unmodified open-source RISC-V cores. Each gets 1 KB of RAM with a
program that runs a xorshift32 loop from a per-core seed. After CYCLES clocks
the testbench prints `cores_ran=N/N checksum=… loops=…`. If msim and Verilator
print the same line, both ran the same programs to the same cycle.

None of the three cores is shipped with minisim. Only permissive licenses
are used. Fetch the sources yourself:

| core | license | source |
|---|---|---|
| ibex | Apache-2.0 | https://github.com/lowRISC/ibex: the file list and options of `/mnt/project-files/opentitan-ibex/run_msim.sh`, with `-top rvsoc_tb` |
| picorv32 | ISC | https://raw.githubusercontent.com/YosysHQ/picorv32/main/picorv32.v |
| femtorv32 | BSD-3-Clause | https://raw.githubusercontent.com/BrunoLevy/learn-fpga/master/FemtoRV/RTL/PROCESSOR/femtorv32_quark.v (pass `-DBENCH`) |

```sh
python3 bench/gen_soc.py --core picorv32 16 20000 > soc.sv
msim -compile picorv32.v soc.sv -top rvsoc_tb
verilator --binary -O3 --top-module rvsoc_tb picorv32.v soc.sv && obj_dir/Vrvsoc_tb
```

FemtoRV32 needs `-DBENCH`. Without it, its shift counter and `x0` start
as `x`. Verilator's 2-state simulation hides that, but in a 4-state
simulator the core waits forever.

### The cores compared

| | Ibex | PicoRV32 | FemtoRV32 (Quark) |
|---|---|---|---|
| license | Apache-2.0 (lowRISC) | ISC (YosysHQ) | BSD-3-Clause (Bruno Levy) |
| language, size | SystemVerilog, 29k lines with its prims | Verilog-2005, one file, 3k lines | Verilog, one file, 420 lines |
| configuration here | RV32E, no M | RV32I, counters off | RV32I |
| architecture | 2-stage pipeline | multi-cycle state machine | multi-cycle state machine, shifts 1 bit per clock |
| measured CPI on the program | 1.3 | 5.6 | 6.7 |
| msynth area (cell units) | not read yet (include dirs, latch clock gate) | 16.1k | 12.2k |
| pros | real chip core (OpenTitan); exercises SystemVerilog the most; fastest per clock | plain Verilog that every simulator and msynth read; widely used and mature | smallest RTL; fastest real core to build |
| cons | biggest; slowest to build and to simulate per core | about 4x more clocks per instruction than Ibex | teaching core with little industry use; needs `-DBENCH` |

CPI is clocks per instruction on the generated program, computed from the
loop counts as cores × CYCLES / (loops × 11 instructions). msim and Verilator
give the same loop counts.

### Recommended use

| core | use it for |
|---|---|
| synthetic | quick smoke tests and scaling runs of every tool: cores, waveforms, coverage, lint, DFT |
| ibex | realism and SystemVerilog coverage: the closest to a real SoC |
| picorv32 | cross-tool compatibility: Icarus, Verilator, msynth and other simulators all read it |
| femtorv32 | scale and speed limits with a real CPU: the most cores per build |

### Measured (2026-11-12, 4-core VM, 16 GB, Verilator 5.020, 20000 cycles)

Build and run times are in seconds. Each run is the median of 3. The msim
build includes one run.

| design | msim interp | msim -compile build | msim simv run | Verilator build | Verilator run | same output |
|---|---|---|---|---|---|---|
| femtorv32 × 16 | 3.0 | 68 | 0.24 | 5.2 | 0.04 | yes |
| picorv32 × 16 | 10.6 | 138 | 2.9 | 10.0 | 0.04 | yes |
| ibex × 4 | 17.3 | 212 | 8.1 | 7.0 | 0.04 | yes |
| ibex × 16 | 74 | out of memory (4 C++ files of 7 MB, 3.6 GB each) | - | 17.1 | 0.13 | Verilator vs interp: yes |

msim generates code for every instance, so its C++ grows with the core
count. Verilator shares one copy of the code per module. That gap, and the
slow simv on the multi-core real designs, are the next msim speed work.
