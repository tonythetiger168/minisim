# VPI in minisim

minisim implements the Verilog Procedural Interface of IEEE 1800-2023
clauses 36-38 with the standard header `include/vpi_user.h`. A VPI
application written for another simulator builds against it unchanged as
long as it uses the parts listed below.

## Loading an application

```
msim top.sv myvpi.c                  # msim builds the C file and loads it
msim top.sv -load ./myvpi.so         # a library built elsewhere
msim top.sv -load ./myvpi.so:boot    # also call the routine `boot` (VCS form)
msim top.sv -loadvpi ./myvpi.so:boot # the same (Xcelium form)
```

Libraries load before elaboration. Each library's `vlog_startup_routines`
table runs in order (36.9.1), so the system tasks and functions it
registers with `vpi_register_systf` can be called from the design. A C
file given on the command line is built into one library with `vpi_user.h`
and `svdpi.h` on the include path; the same library serves DPI-C imports.
With `-dpiheader` the C files are built after elaboration instead, so their
VPI system tasks are not known: build those separately and use `-load`.

A design that uses VPI runs on one thread, in the interpreter (`-compile`
and `-threads` are ignored), because callbacks and `calltf` routines may
read or write any signal at any time.

## What is implemented

**User system tasks and functions (38.37).** Tasks, and functions of type
`vpiIntFunc`, `vpiRealFunc`, `vpiTimeFunc`, `vpiSizedFunc` and
`vpiSizedSignedFunc` (with `sizetf`). `compiletf` runs once per call site
before the simulation; `calltf` runs at each call. A function returns its
value with `vpi_put_value` on the call handle. Arguments: a variable or net
is passed as the object itself; a module name as the module; a string
literal as a string constant; anything else as an expression whose value is
taken when the call runs. `vpi_put_userdata` / `vpi_get_userdata` keep data
per call site.

**Objects (clause 37).** `vpiModule` (top modules from
`vpi_iterate(vpiModule, NULL)`, children through generate blocks and
instance arrays), `vpiNet`, `vpiReg`, `vpiIntegerVar`, `vpiTimeVar`,
`vpiRealVar`, `vpiMemory` (one unpacked dimension) and `vpiMemoryWord`,
`vpiNetBit` / `vpiRegBit`, `vpiParameter`, `vpiPort` (with `vpiDirection`
and `vpiLowConn`), `vpiSysTaskCall` / `vpiSysFuncCall` with `vpiArgument`,
`vpiScope` and `vpiUserSystf`. Properties: `vpiType`, `vpiName`,
`vpiFullName`, `vpiDefName`, `vpiFile`, `vpiLineNo`, `vpiSize`, `vpiSigned`,
`vpiScalar`, `vpiVector`, `vpiTopModule`, `vpiTimeUnit`, `vpiTimePrecision`,
`vpiDirection`, `vpiNetType`, `vpiFuncType`, `vpiConstType`, `vpiLocalParam`,
`vpiArray`. Handles are canonical: the same object always has the same
handle, so `vpi_compare_objects` and `==` agree.

**Values and time (38.15, 38.18, 38.34, 38.35).** `vpi_get_value` and
`vpi_put_value` with `vpiBinStrVal`, `vpiOctStrVal`, `vpiDecStrVal`,
`vpiHexStrVal`, `vpiScalarVal`, `vpiIntVal`, `vpiRealVal`, `vpiStringVal`,
`vpiVectorVal`, `vpiTimeVal`, `vpiObjTypeVal` and `vpiSuppressVal`.
`vpi_put_value` with `vpiNoDelay`, `vpiInertialDelay` (a zero delay acts as
a nonblocking assignment) and `vpiTransportDelay`. `vpi_get_time` in
`vpiSimTime` (ticks) and `vpiScaledRealTime` (the object's time unit).

**Callbacks (38.36).** `cbValueChange` (on a net, variable, memory word or
bit, with the time and value in the requested formats), `cbReadWriteSynch`,
`cbReadOnlySynch`, `cbAfterDelay`, `cbNextSimTime`, `cbAtStartOfSimTime`,
`cbEndOfCompile`, `cbStartOfSimulation`, `cbEndOfSimulation`;
`vpi_remove_cb`, `vpi_get_cb_info`. Declaration initializers take effect
before the simulation starts and are not value changes.

**Other routines.** `vpi_printf`, `vpi_vprintf`, `vpi_mcd_open`,
`vpi_mcd_close`, `vpi_mcd_name`, `vpi_mcd_printf`, `vpi_flush`,
`vpi_chk_error`, `vpi_get_vlog_info`, `vpi_control(vpiFinish)` and
`vpi_control(vpiStop)`, `vpi_free_object` / `vpi_release_handle`.
`vpi_handle_by_name` takes a bit or a part select (`top.u.r[3:0]`, a
`vpiPartSelect`), which reads, writes, forces and releases those bits only.
`vpi_put_value` with `vpiForceFlag` / `vpiReleaseFlag` forces and releases a
net or variable (or one bit of it) like `force` / `release` (10.6.2); a
release returns the value the object then has. `veriuser.h` declares
`tf_dofinish` and `tf_dostop`, and `sv_vpi_user.h` the SystemVerilog object
type numbers, for C code written for other simulators. A DPI context import
may call VPI (35.5.3), as UVM's HDL backdoor does (see `docs/UVM.md`).

**Protected objects (37.3.6).** `vpiIsProtected`, and the access rules for
objects in decryption envelopes: see "VPI and DPI" in `docs/PROTECT.md`.

## Not yet

SystemVerilog-specific objects (classes, structs, interfaces, packages,
dynamic arrays: `sv_vpi_user.h` names their types, but handles to them are
not made), statements and processes, drivers and
loads, `vpiHighConn`, multi-dimensional memories, `vpi_get_delays` / `vpi_put_delays`, `cbForce` /
`cbRelease` / `cbStmt`, and save / restart. A call to something not
implemented returns NULL or `vpiUndefined` and sets an error that
`vpi_chk_error` reports.

## Tests

`tests/spec/37-vpi-walk.sv`, `38.34-vpi-put-value.sv`,
`38.36-vpi-callbacks.sv` and `38.37-user-systf.sv` with their C files in
`tests/spec/data/vpi/`. When Icarus Verilog is installed, CI builds the same
C files into an Icarus VPI module (`//! icarus-vpi:`) and checks that Icarus
prints the same output.
