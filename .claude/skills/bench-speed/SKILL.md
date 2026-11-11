---
name: bench-speed
description: How to measure minisim speed and compare with Verilator and Icarus - bench/run.sh, bench/tools.sh, OpenTitan Ibex as the reference design, profiling, and how to report numbers. Use for any performance claim, speed-up PR, or "how far are we from Verilator" question.
---

# Benchmarking

The goal for the simulator is Verilator-level speed; for every other tool,
good scaling across cores. A number without a baseline and a checksum is
not a result.

## Build

```sh
CXX=g++ cmake -B build -DMINISIM_WITH_SLANG=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Simulator against Verilator and Icarus

```sh
bench/run.sh                       # lanes.sv
bench/run.sh bench/wide.sv build   # another design; top module = file name
bench/run.sh bench/chain.sv build 200000   # with END_TIME
```

One row each for the msim interpreter, `msim -compile`, `verilator -O3` and
Icarus, with build time and run time in separate columns and the design's
checksum. Tools that are not installed are skipped. **The checksums must
match**; a faster wrong answer is a bug, not a speed-up.

## Tools around the simulator

```sh
bench/tools.sh build 512 2000                       # generated SoC
DESIGN="<files...> -top <tb>" bench/tools.sh build  # a real design
```

Rows: plain, `-vcd`, `-fst`, `-cov`, `-compile` (first and cached build),
coverage merge and report, lint and DFT, a regression.

## Reference design

Use OpenTitan Ibex as the common real design for every tool, PicoRV32 as
the small one. Generated designs (`bench/gen_soc.py`, `gen_wide.py`,
`gen_pnr.py`, `gen_power.py`) show scaling; they do not replace a real one.

## Method

- Same machine, same run, for both sides of a comparison. State the core
  count and the tool versions (`verilator --version`).
- Run three times and report the median; say so when a number is a single run.
- Report build time and run time separately, and both the first and the
  cached `-compile` build.
- Thread scaling: `-threads 1`, physical cores, all hardware threads. The
  output must be identical at every count.
- Profile before optimizing: `perf record -g` where available, otherwise
  `valgrind --tool=callgrind` on a shortened run. Fix the top entry, measure
  again.
- A speed-up PR states before and after on the same design in its
  description, and updates the performance table in `README.md` or the
  tool's doc when the published number changes.
- Add a test for the fast path that compares its output with the slow path.
