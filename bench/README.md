# bench

Designs and scripts for measuring minisim tools; see the bench-speed skill
(`.claude/skills/bench-speed`) for the method. This page covers the SoC
generator.

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
