---
name: uvm-row-test
description: How to prove or fix one row of IEEE 1800.2-2020 UVM on msim - a clause-tagged check against the Accellera reference library (uvm-core 2020.3.1) in tests/uvm, finding whether a failure is an msim language bug, and updating docs/UVM.md and the spec table. Use for any UVM row, "UVM does not compile/run on msim", or a class that behaves differently from other simulators.
---

# Proving a UVM row

minisim does not reimplement UVM. The Accellera reference library is the
implementation; a UVM row is done when the library's class for that clause
compiles and behaves correctly **on msim**. So a failing UVM row is almost
always a SystemVerilog gap or bug in msim, and the fix is a `spec-gap` fix.

## Setup

```sh
git clone --depth 1 --branch 2020.3.1 https://github.com/accellera-official/uvm-core ~/uvm-core
CXX=g++ cmake -B build -DMINISIM_WITH_SLANG=ON -DUVM_HOME=$HOME/uvm-core && cmake --build build -j2
ctest --test-dir build -L uvm --output-on-failure
build/msim -uvmhome ~/uvm-core tests/uvm/hello.sv -top top
```

uvm-core is Apache-2.0. It is fetched, never copied into this repository.

## A test

`tests/uvm/<topic>.sv`; every `*.sv` there is picked up by CMake and run by
`tests/uvm/run_uvm.cmake` with `msim -uvmhome ... -top top`.

```systemverilog
// IEEE 1800.2-2020 12.2 uvm_tlm_fifo: <what is checked>.
import uvm_pkg::*;
`include "uvm_macros.svh"
`define CHK(id, c) $display("P %s %0d", id, ((c) ? 1 : 0));
...
  `CHK("12.2.put_get", got == 42)
...
//> P 12.2.put_get 1
```

- The top module is `top`. `//> ` lines must appear in the output in order,
  each as part of a line. `//! args: +UVM_TESTNAME=...` adds plusargs,
  `//! nodpi` builds the library without its C code.
- **One check id per behaviour, prefixed with its clause**:
  `<clause>.<name>`. The spec table counts checks per clause from these
  lines. A check must test behaviour (a value came back, a callback ran, a
  phase order held, an error was reported). `CHK("x", 1)` proves only that
  the code compiled and counts as partial, not done.
- Group checks by chapter into one file per area (base, synchronization,
  TLM, sequences, configuration, registers, recording) so one library
  build serves many checks; each test compiles all of UVM.
- Also assert there is no unexpected `UVM_ERROR` / `UVM_FATAL`: end with the
  report summary counts in the expected lines.

## When a check fails or the class does not compile

1. Reduce to a few lines of plain SystemVerilog with no UVM that show the
   same error or wrong value.
2. That reduction is a spec test: name it after the 1800-2023 clause of the
   construct, add it under `tests/spec/` and fix msim (`spec-gap`).
3. Keep the UVM check as the regression for the row.
4. Never work around it by editing the library or guarding it with a
   define; a patched UVM proves nothing. `+define+UVM_NO_DPI` and the
   library's other documented switches are fine when the test says so.
5. If the reference library itself differs from the standard, record that
   in the row's note; do not count it against msim.

## Rows without a class to instantiate

Macros (annex B), policies, and the DPI layer (`uvm_hdl_*`,
`uvm_re_match`, command line): cover them through the tests that use
them and say which in the table note. Clauses that only define terms are
informative and are not counted.

## Record it

- `docs/UVM.md`: add the test to "What is covered"; trim "Not yet".
- Master table: UVM statuses are computed from the `P <clause>.<name> <0|1>`
  lines, so regenerate after the tests change (see `spec-gap`, step 5).
- Speed matters here: a UVM build is the largest thing msim compiles. If a
  change moves the library's elaboration or run time, measure it
  (`bench-speed`).
