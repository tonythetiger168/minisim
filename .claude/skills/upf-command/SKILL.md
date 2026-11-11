---
name: upf-command
description: How to add or complete one UPF command or option for power-aware simulation (IEEE 1801-2024) - reader in src/upf.hpp, runtime in src/upf_rt.inc, a tests/upf trio with golden output, docs/LOWPOWER.md. Use for any UPF / low-power row of the spec table, msim -upf or mlint -upf work.
---

# Adding a UPF command or option

Standard: IEEE 1801-2024 (UPF 4.0). Clause 6 lists the commands, one
subclause each; that subclause number is the row in the spec table.

## Where things are

| Part | File |
|---|---|
| Tcl reader, power model, expressions, static checks, the `UPF` SV package text | `src/upf.hpp` |
| Simulation behaviour (corruption, isolation, retention, switches, supplies) | `src/upf_rt.inc` |
| Command line: `msim -upf f [-power_top inst] [-upf_verbose]`, `mlint -upf` | `src/minisim.cpp`, `src/mlint.cpp` |
| Unit tests of the reader and model | `tests/test_upf.cpp` |
| Simulation tests | `tests/upf/<name>.sv`, `<name>.upf`, `<name>.out`; driver `tests/upf.cmake` |
| User doc and the "Not yet" list | `docs/LOWPOWER.md` |

The shared Tcl interpreter is `src/tcl.hpp`; moving the UPF reader onto it
is how `[...]` substitution, `foreach` and procs arrive. Do not write a
second Tcl.

## Steps

1. **Read the command's subclause**: every option, which are required,
   defaults, and the semantics paragraph. Note what is an error.
2. **Decide the class** and say it in the doc:
   - *simulated*: changes what the simulation does (domains, supplies,
     switches, isolation, level shifters, retention, power states, simstates);
   - *checked*: parsed and validated, used by `mlint -upf`;
   - *implementation only* (`map_*`, `use_interface_cell`, cell
     definitions): parsed, validated, no simulation effect. Read-and-ignore
     with a note is partial; done needs the options validated and a test
     that bad input is reported with file and line.
3. **Test first.** Add `tests/upf/<name>.sv` (a small DUT and a testbench
   that drives the supplies and control signals and prints at each step),
   `<name>.upf`, and the expected `<name>.out`. Add `<name>` to the
   `foreach(t ...)` list in `tests/upf.cmake`. Start each `.upf` with a
   comment naming the clause. Write the expected output from the standard's
   semantics: what is corrupted to x and when, what the clamp value is, when
   the retained value comes back.
4. **Reader**: handle the command in `Model::run` in `src/upf.hpp`; unknown
   options and missing required ones are errors (`file:line: text`), never
   skipped silently. Add the reader cases to `tests/test_upf.cpp`.
5. **Runtime** in `src/upf_rt.inc`. Rules that hold today: a powered-down
   domain's processes sleep and restart on power-up; what the domain drives
   is held at x; `initial` blocks are never gated; isolation and level
   shifters wrap port connections; a UPF run uses the interpreter on one
   thread. Run-time problems print `UPF error: <time>: <text>` and the
   simulation continues.
6. **Static check** when the clause states a rule a tool should enforce:
   add a `UPFnnn` rule to `mlint -upf`, with a bad example in `tests/upf/`.
7. **Docs**: move the item out of "Not yet" in `docs/LOWPOWER.md`, show the
   command in an example, list remaining limits.
8. **Master table**: flip the 1801 row (see `spec-gap`, step 5).

```sh
ctest --test-dir build -R 'upf|test_upf' --output-on-failure
build/msim tests/upf/<name>.sv -upf tests/upf/<name>.upf -upf_verbose
```

## Cross-checks

There is no open-source UPF simulator to compare with. Keep the test small
enough that the expected output can be derived by hand from the clause, and
put that derivation in comments in the `.upf` file.

Copyright: clause numbers and titles only; never commit the standard's text.
