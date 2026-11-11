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

**Objects (clause 37).** Instances: `vpiModule`, `vpiInterface` (with
`vpiModport`), `vpiProgram` and `vpiPackage`; `vpi_iterate(vpiInstance, NULL)`
gives the top instances and the packages, `vpi_iterate(vpiModule, NULL)` the
top modules. Scopes inside an instance (`vpiInternalScope`): `vpiGenScope`
(and `vpiGenScopeArray` for a generate loop), `vpiTask`, `vpiFunction`,
`vpiNamedBegin`, `vpiNamedFork`. Nets and variables with their
SystemVerilog types: `vpiNet`, `vpiReg` (`vpiLogicVar`), `vpiBitVar`,
`vpiIntegerVar`, `vpiIntVar`, `vpiShortIntVar`, `vpiLongIntVar`,
`vpiByteVar`, `vpiTimeVar`, `vpiRealVar`, `vpiShortRealVar`, `vpiStringVar`,
`vpiEnumVar`, `vpiStructVar` and `vpiUnionVar` (packed), `vpiPackedArrayVar`
and the matching net types; `vpi_iterate(vpiVariables / vpiNet, scope)`
gives them all. Arrays of any number of unpacked dimensions (`vpiRegArray` /
`vpiArrayVar`, `vpiNetArray`) with a `vpiRange` per dimension, elements by
`vpi_handle_by_index`, `vpi_handle_by_multi_index`, by name (`top.m[1][2]`,
also one bit `top.m[1][2][3]`) or by iteration. `vpiNetBit` / `vpiRegBit`,
`vpiPartSelect`, `vpiParameter`, `vpiPort` (with `vpiDirection`,
`vpiLowConn` and `vpiHighConn`), `vpiRange` with `vpiLeftRange` /
`vpiRightRange` constants, `vpiSysTaskCall` / `vpiSysFuncCall` with
`vpiArgument`, `vpiUserSystf`, and `vpi_iterate(vpiCallback, NULL)`.
Typespecs (`vpiTypespec` of a variable, parameter, port, function or
member; `vpi_iterate(vpiTypedef, scope)`): the integer, real, string, logic
and bit typespecs, `vpiEnumTypespec` with its `vpiEnumConst` values and
`vpiBaseTypespec`, `vpiStructTypespec` / `vpiUnionTypespec` with
`vpiTypespecMember`, `vpiArrayTypespec` and `vpiPackedArrayTypespec` with
`vpiElemTypespec`; a typedef's typespec has its name. Relations `vpiScope`,
`vpiModule`, `vpiInstance`, `vpiParent`, `vpiIndex`. Properties: `vpiType`,
`vpiName`, `vpiFullName`, `vpiDefName`, `vpiFile`, `vpiLineNo`,
`vpiDefFile`, `vpiDefLineNo`, `vpiSize`, `vpiSigned`, `vpiScalar`,
`vpiVector`, `vpiTopModule`, `vpiTop`, `vpiTimeUnit`, `vpiTimePrecision`,
`vpiDirection`, `vpiNetType` (wire, wand, wor, tri, tri0, tri1, trireg,
supply0, supply1, uwire), `vpiFuncType`, `vpiConstType`, `vpiLocalParam`,
`vpiArray`, `vpiArrayType`, `vpiIsMemory`, `vpiArrayMember`, `vpiPacked`,
`vpiTagged`, `vpiAutomatic`, `vpiConstantVariable`, `vpiRandType`.
Handles are canonical: the same object always has the same handle, so
`vpi_compare_objects` and `==` agree. `vpi_handle_by_name` takes full
names, names relative to a scope, `pkg::name`, generate scopes
(`top.gen[1].v`) and instance array elements.

**Compatibility modes (36.12).** An application built for IEEE 1364 sees
that standard's model: SystemVerilog variable types report as `vpiReg`
(`bit [3:0]`, `int`, an enum, a packed struct) or their net as `vpiNet`,
and for IEEE 1364-1995 / 2001 a one-dimensional array of `reg` is a
`vpiMemory` of `vpiMemoryWord`s and the contents of generate blocks belong
to the module. `vpi_iterate(vpiMemory, ...)` and `vpiMemoryWord` work in
every mode.

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

**Array values (38.16, 38.35).** `vpi_get_value_array` and
`vpi_put_value_array` on a memory, from a start index, in `vpiIntVal`,
`vpiShortIntVal`, `vpiLongIntVal`, `vpiRealVal`, `vpiShortRealVal`,
`vpiTimeVal`, `vpiVectorVal`, `vpiRawTwoStateVal` and `vpiRawFourStateVal`
(ceil(size/8) value bytes per element, then as many unknown-mask bytes),
with `vpiUserAllocFlag`, `vpiOneValue` and `vpiPropagateOff`. A whole array
can be an argument of a user system task.

**Headers (annexes K, L, M).** `vpi_user.h` and `sv_vpi_user.h` define every
constant, structure and routine of annexes K and M with the standard's
numbers. `vpi_compatibility.h` (annex L, included by `vpi_user.h`): an
application that defines `VPI_COMPATIBILITY_VERSION_1364v1995`, `_1364v2001`,
`_1364v2005`, `_1800v2005` or `_1800v2009` before the include is bound to
that standard's variants of the routines (`vpi_get_1364v2005`, ...), and
`vpi_get(vpiCompatibilityMode, NULL)` returns the mode of the calling
application (36.12.2). The default mode is `vpiMode1800v2009`, the newest
the standard defines. The object model is the same in every mode so far.

**Other routines.** `vpi_printf`, `vpi_vprintf`, `vpi_mcd_open`,
`vpi_mcd_close`, `vpi_mcd_name`, `vpi_mcd_printf`, `vpi_flush`,
`vpi_chk_error`, `vpi_get_vlog_info`, `vpi_control(vpiFinish)` and
`vpi_control(vpiStop)`, `vpi_free_object` / `vpi_release_handle`.
`vpi_handle_by_name` takes a bit or a part select (`top.u.r[3:0]`, a
`vpiPartSelect`), which reads, writes, forces and releases those bits only.
`vpi_put_value` with `vpiForceFlag` / `vpiReleaseFlag` forces and releases a
net or variable (or one bit of it) like `force` / `release` (10.6.2); a
release returns the value the object then has. `veriuser.h` declares
`tf_dofinish` and `tf_dostop` for C code written for other simulators. A DPI context import
may call VPI (35.5.3), as UVM's HDL backdoor does (see `docs/UVM.md`).

## Not yet

Class objects, unpacked structs, dynamic arrays, queues and associative
arrays, statements, processes and expressions, drivers and loads,
instance arrays as objects, `vpi_get_delays` / `vpi_put_delays`, `cbForce` /
`cbRelease` / `cbStmt`, and save / restart. A call to something not
implemented returns NULL or `vpiUndefined` and sets an error that
`vpi_chk_error` reports.

## Tests

`tests/spec/37-vpi-walk.sv`, `37-vpi-sv-objects.sv`, `38.16-vpi-value-array.sv`, `38.34-vpi-put-value.sv`,
`38.36-vpi-callbacks.sv` and `38.37-user-systf.sv` with their C files in
`tests/spec/data/vpi/`; `K-vpi-user-h.sv`, `L-vpi-compatibility.sv` and
`M-sv-vpi-user-h.sv` check the headers. When Icarus Verilog is installed, CI builds the same
C files into an Icarus VPI module (`//! icarus-vpi:`) and checks that Icarus
prints the same output.
