# Standard benchmarks on mvplat

`run.sh` builds CoreMark, Dhrystone and 17 Embench-IoT programs for a bare
RV64 hart and times them on mvplat, and on Spike and QEMU
(`qemu-system-riscv64 -machine virt`) when they are on the `PATH` (or given as
`SPIKE=`, `QEMU=`):

    bench/mvplat/run.sh build/mvplat /tmp/bench-mvplat     # RUNS=3 by default

`ARCH=aarch64` builds the same programs for an AArch64 processor
(`MARCH`, default `armv8-a`) and runs them on mvplat's arm board and on QEMU
(`qemu-system-aarch64 -machine virt -cpu max`): they print on the PL011 and
leave through PSCI `SYSTEM_OFF` over HVC, which both machines answer
(`link-a64.ld`, `tests/vplat/a64/crt0.S`). On a 2-core host, 2026-11-28,
with mvplat's decoded-page interpreter (no AArch64 translator yet): CoreMark
7.86 s against QEMU's 2.19 s, Dhrystone 5.33 s against 0.90 s, the Embench
programs 0.6 to 2.0 s against 0.07 to 0.43 s, every result correct.

The benchmarks are fetched at pinned commits into the work directory and are
never kept here:

| Benchmark | Source | Licence |
| --- | --- | --- |
| CoreMark | github.com/eembc/coremark | Apache-2.0, with EEMBC's rules for the CoreMark name |
| Dhrystone | riscv-tests `benchmarks/dhrystone` | BSD-3-Clause |
| Embench-IoT | github.com/embench/embench-iot | GPL-3.0 (each program its own) |

They run as guest programs only; no part of them is linked into minisim.
What is minisim's here is the port: `rt.c` (console, exit, cycle counter and
the few C library functions the programs call), `inc/` (the headers for them),
`coremark/core_portme.*`, `embench/boardsupport.*`, `dhrystone_rt.c` and
`link.ld` and `link-a64.ld` (data on its own pages, so translating simulators are not slowed by
stores near code).

Each program is built twice: printing on the UART and leaving through the
SiFive test finisher (mvplat, QEMU), and silent, leaving through HTIF's
`tohost` (mvplat, Spike). Times are the host's wall clock, best of `RUNS`; a
non-zero exit (a wrong CoreMark CRC, a failed Embench check) is marked `!`.
Results go to `WORKDIR/results.tsv`. CoreMark is a valid run (at least 10 s of
simulated time at mvplat's 100 MHz) with the default 4000 iterations; the
numbers it prints are in simulated time, not a CoreMark score of the host.
