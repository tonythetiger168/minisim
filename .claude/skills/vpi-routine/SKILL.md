---
name: vpi-routine
description: How to add or complete a VPI routine, object type, property or callback (IEEE 1800-2023 clauses 36 to 41, annexes K to N) - src/vpi.inc, include/vpi_user.h, a tests/spec test with a C file that also runs on Icarus, docs/VPI.md. Use for any VPI / PLI row of the spec table.
---

# Adding a VPI routine

## Where things are

| Part | File |
|---|---|
| Implementation | `src/vpi.inc` |
| Headers, kept identical to the standard's annexes | `include/vpi_user.h` (K), `include/sv_vpi_user.h` (M), `include/veriuser.h` |
| Tests | `tests/spec/<clause>-vpi-<topic>.sv` + C in `tests/spec/data/vpi/` |
| User doc with "What is implemented" and "Not yet" | `docs/VPI.md` |

Clause map: 36 overview and registration, 37 object model diagrams, 38 one
subclause per routine, 39 assertion API, 40 coverage API, 41 data read API.

## Steps

1. **Read the routine's subclause in 38 and the object diagrams in 37** it
   touches: argument meanings, what is returned on error, which object
   types and properties it must accept.
2. **Test first**, as a spec test:

   ```systemverilog
   // IEEE 1800-2023 38.34 vpi_put_value(): <what is checked>.
   //! args: data/vpi/put_value.c
   //! icarus-vpi: data/vpi/put_value.c
   module t; ... initial begin $my_task(sig); ... $finish; end endmodule
   //> expected line
   ```

   msim builds the C file named in `//! args:` and loads it. `//! icarus-vpi:`
   builds the same C into an Icarus VPI module, so Icarus checks the same
   expected output. Use `//! icarus-skip: <reason>` only when Icarus really
   lacks the feature. The C file registers its tasks through
   `vlog_startup_routines` and prints with `vpi_printf`.
3. **Implement in `src/vpi.inc`.** Rules:
   - Anything not supported returns NULL or `vpiUndefined` **and** sets the
     error that `vpi_chk_error` reports. Never return a plausible wrong
     value.
   - Handles: `vpi_release_handle` (and the old `vpi_free_object`) must
     work on everything you hand out; iterators free themselves when
     `vpi_scan` returns NULL. `vpi_compare_objects` is true for two handles
     to the same object.
   - Values: every format the clause lists for the object
     (`vpiBinStrVal`, `vpiHexStrVal`, `vpiIntVal`, `vpiScalarVal`,
     `vpiVectorVal`, `vpiRealVal`, `vpiStringVal`, `vpiTimeVal`,
     `vpiObjTypeVal`), with 4-state x and z, and widths over 64 bits.
   - `vpi_put_value` flags: `vpiNoDelay`, `vpiInertialDelay`,
     `vpiTransportDelay`, `vpiPureTransportDelay`, `vpiForceFlag`,
     `vpiReleaseFlag`; a returned event handle with `vpiReturnEvent`.
   - Callbacks run in the scheduling region clause 4 and 38.36 give them
     (`cbReadWriteSynch`, `cbReadOnlySynch`, `cbNextSimTime`,
     `cbAfterDelay`, `cbValueChange`, start and end of simulation). No
     writes in read-only synch: report the error.
   - A design that uses VPI runs in the interpreter on one thread. Keep it
     so until a routine is proven safe otherwise.
4. **Headers**: constants keep the standard's names and values. Add a
   missing one from the annex; never renumber.
5. **SystemVerilog objects (clause 37 diagrams, annex M)**: adding a new
   object type means the type constant, its properties (`vpiName`,
   `vpiFullName`, `vpiType`, `vpiSize`, ...), the one-to-one relations
   (`vpi_handle`) and the one-to-many relations (`vpi_iterate`) of its
   diagram, and `vpi_handle_by_name` reaching it. Extend
   `37-vpi-walk` or add a walk test for the new type.
6. **Docs**: add the routine or object to "What is implemented" in
   `docs/VPI.md` and remove it from "Not yet". Also fix any statement
   elsewhere that goes stale (for example the 9.7 row in `docs/PROTECT.md`).
7. **Master table**: clauses 36 to 41 are one row each today. When a
   chapter is being worked, split it into per-subclause rows in the
   generator so progress shows (see `spec-gap`, step 5).

```sh
ctest --test-dir build -R 'spec/3[6-9]|spec/4[01]|icarus/3[6-9]' --output-on-failure
```

## Reference behaviour

Icarus Verilog (run as an external program only) is the cross-check for
clauses 36 to 38. For 39 to 41 it has little; derive the expected output
from the clause and say so in the test's header comment.
