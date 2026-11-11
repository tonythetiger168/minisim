---
name: add-tool
description: Checklist for adding a new tool (m<name>) to minisim or extending one - file layout, CMake registration, threads on by default, determinism, shared readers, docs and tests. Use before writing the first file of a tool or a new parser, reader, solver or pool.
---

# Adding or extending a minisim tool

## Before writing code

- **One owner per tool and per shared component. Do not duplicate work.**
  Look for an existing reader before writing one: `src/liberty.hpp`,
  `src/gnetlist.hpp`, `src/sdc.hpp`, `src/tcl.hpp`, `src/lef.hpp`,
  `src/def.hpp`, `src/gds.hpp`, `src/wave_reader.hpp`, `src/aig.hpp`,
  `src/lintir.hpp`, `src/cover.hpp`, `src/upf.hpp`, `src/parallel.hpp`.
  If the project keeps an ownership table (`reports/tool-ownership.md` in
  the project's shared folder), read it. Need a change in someone else's
  component? Send a small PR against that file, do not fork it.
- **Reuse open source where the license allows** (see `third-party-code`).

## Layout

| What | Where |
|---|---|
| Entry point | `src/m<tool>.cpp` (headers next to it, or `src/<tool>/`) |
| Build + tests | `cmake/tools/m<tool>.cmake` - a new file, not a line in `CMakeLists.txt`; every `*.cmake` there is included automatically |
| Test driver | `tests/m<tool>.cmake` or a Python runner, data in `tests/<tool>/` |
| User doc | `docs/<TOOL>.md` |
| Product row | `docs/PRODUCT.md` and the tools table in `README.md` |

Start the cmake file with `include_guard(GLOBAL)`. It may test
`if(MINISIM_WITH_SLANG)`, `if(TARGET msim)` and `MINISIM_WITH_FST`. A tool
that does not need the SystemVerilog front end must build without slang.
Install the binary and its doc (`cmake/tools/matpg.cmake` is a short model).

## Every tool must

1. **Use all cores by default.** Take `-threads N` through
   `par::threadsArg`, default to `par::defaultThreads()`, and run the hot
   loops on `par::parallelFor` / `par::Pool` from `src/parallel.hpp`. No
   private thread pools.
2. **Be deterministic.** The output for `-threads 1` and `-threads N` must be
   byte-identical; add a test that compares them. Reduce per-thread results
   in a fixed order; no iteration over pointer-keyed or unordered containers
   into the output; seeded RNG per work item, not per thread.
3. **Be profiled.** Measure on a real public design (OpenTitan Ibex,
   PicoRV32) before calling it done, and put the numbers in the doc. See
   `bench-speed`.
4. **Exit non-zero on failure** and offer a gate option where it makes sense
   (`-min_coverage`, `-max_violations`), so CI can use it.
5. **Check itself against an independent tool when one exists** (Icarus,
   Verilator, Yosys, ABC, ngspice, OpenSTA, OpenRCX), as an optional test
   that is skipped when the program is not installed. GPL tools are only
   ever run as external programs.

## The doc (`docs/<TOOL>.md`)

One paragraph on what it does, a block of command lines, "What it reads",
"What it writes", options, performance numbers, and an honest
**Limits / Not yet** section. The completion report is built from that
section, so keep it current in the same PR as the code.

## Tests and spec citations

SystemVerilog behaviour is IEEE 1800-2023; tests cite the clause they check.
Register every test with `add_test` so `ctest` runs it in both CI jobs.

## Public repository

This repository is public. Use only public designs in tests and docs, and
never name or describe private IP, VIP or customer repositories.
