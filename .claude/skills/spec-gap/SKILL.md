---
name: spec-gap
description: The loop for closing one gap against the standards (IEEE 1800-2023 SystemVerilog first; also the entry point for 1800.2 UVM, 1801 UPF, 1735) - pick a row, write a clause-cited test in tests/spec, make it pass, update docs/COVERAGE.md and the master spec table. Use for any "match the spec", "100% spec", missing-construct or wrong-result work in msim.
---

# Closing a spec gap

Goal: every normative row of the four standards done, each proven by a test
that cites its clause. One row, one test, one small PR.

## 1. Pick the row

Sources, in this order:

1. **Wrong answers first.** Rows marked **WRONG** or **BUG** in the master
   spec table (msim runs the construct and prints the wrong thing). A silent
   wrong answer is worse than a missing feature.
2. Tests marked `//! xfail:` - `python3 tools/mregress.py -spec tests/spec`
   lists them with a per-chapter table.
3. ❌ then 🟡 rows of `docs/COVERAGE.md` and of the master table
   (`reports/master-spec-table-<date>.md` in the project's shared folder,
   when the session has one). Prefer rows that block something bigger (a UVM
   class that does not compile, a public design that does not elaborate).

Each row has an owner thread in the table. Work your own rows; for someone
else's, tell the coordinator instead of fixing it in parallel.

## 2. Write the test before the fix

`tests/spec/<clause>-<topic>.sv`, for example `10.5-var-decl-init.sv`. Every
`*.sv` there is picked up by CMake; no list to edit.

```systemverilog
// IEEE 1800-2023 10.5 Variable declaration assignment: <one line on the rule>.
module t;
  ...
  initial begin $display("v=%0d", v); $finish; end
endmodule
//> v=5
```

- First line: the standard, the clause number and its title, then the rule
  being checked in your own words. Clause numbers are from the 2023 text.
- `//> text` is one expected stdout line (`//>` alone is an empty line).
- `//! args: ...` extra msim arguments, `//! exit: N` expected exit code,
  `//! icarus-skip: <reason>` when Icarus 13 cannot run it,
  `//! xfail: <what is missing>` for a gap you are recording but not fixing
  (the test must then fail; an xfail that passes is an error).
- Small and single-purpose: one clause, a handful of displays. Cover the
  edge the clause actually specifies (x/z, signedness, widths over 64,
  zero-size, out of range), not only the happy path.
- The expected output comes from the standard's rule. Confirm it on Icarus
  (`ctest -R icarus/<name>`) and, when they are installed, Verilator or a
  commercial simulator. If the simulators disagree, the standard decides;
  say so in a comment.

## 3. Fix

```sh
CXX=g++ cmake -B build -DMINISIM_WITH_SLANG=ON && cmake --build build -j2
ctest --test-dir build -R 'spec/<clause>' --output-on-failure
ctest --test-dir build -L spec            # the whole suite before pushing
```

- Never a silent wrong answer: what is not implemented is an error at
  lowering time ("lower: unsupported ...").
- The interpreter and `-compile` must agree. If the construct can reach
  compiled code, run the test with `-compile` too; if it cannot be compiled
  yet, it must fall back to the interpreter, not miscompile.
- No regressions in `-threads N`: results equal `-threads 1`.

## 4. Record it, in the same PR

- `docs/COVERAGE.md`: flip the row (❌/🟡 to ✅) and name the test. Keep the
  remaining limits in the row when it stays 🟡.
- Remove the `//! xfail` line of a test that now passes.
- `CHANGELOG.md` when users would notice.

## 5. Refresh the master table

The table is generated; never hand-edit the `.md`. In the project's shared
folder: edit the status dictionaries in `reports/master-spec-table-gen.py`
(done / partial with note / missing with note), then

```sh
python3 reports/master-spec-table-gen.py reports/master-spec-table-<date>.md
```

Counting rule: done 1, partial 0.5, missing 0, informative rows left out. A
row is done only when a passing test names its clause.

## The other standards

| Standard | Tests live in | Skill |
|---|---|---|
| IEEE 1800-2023 clauses 36-41 (VPI) | `tests/spec/3x-*.sv` + `tests/spec/data/vpi/*.c` | `vpi-routine` |
| IEEE 1800.2-2020 UVM | `tests/uvm/*.sv` against uvm-core | `uvm-row-test` |
| IEEE 1801-2024 UPF | `tests/upf/<name>.{sv,upf,out}` | `upf-command` |
| IEEE 1735-2023 IP encryption | `tests/msim_protect.cmake`, `tests/test_protect.cpp`; table in `docs/PROTECT.md` | this loop, same rules |

## Copyright

The standards are copyrighted. Cite clause numbers and titles; describe the
rule in your own words. Never commit the PDFs, pasted paragraphs, figures,
or the non-redistributable test vectors. Use only public designs.
