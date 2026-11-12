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

**Assertions (clause 39).** Every assertion, assumption and cover,
concurrent or immediate, is an object: `vpiAssert`, `vpiAssume`,
`vpiCover`, `vpiImmediateAssert`, `vpiImmediateAssume`, `vpiImmediateCover`,
from `vpi_iterate(vpiAssertion, NULL)` (all of them) or from an instance or
scope (those in it), with `vpiName`, `vpiFullName`, `vpiFile`, `vpiLineNo`,
`vpiIsDeferred`, `vpiIsFinal`, `vpiScope` and `vpiModule`.
`vpi_register_assertion_cb` (39.4.2) places a callback on one assertion for
`cbAssertionStart`, `cbAssertionSuccess`, `cbAssertionVacuousSuccess`,
`cbAssertionFailure` and `cbAssertionDisabledEvaluation` (each with the
attempt's start time in `s_vpi_attempt_info`), and for the control reasons
`cbAssertionDisable`, `Enable`, `Reset`, `Kill`, `Lock`, `Unlock` and the
pass, fail and vacuous action ones; `vpi_remove_cb` removes it.
`vpi_register_cb` takes the system reasons (39.4.1):
`cbAssertionSysInitialized` (at the start of simulation), `SysOn`, `SysOff`,
`SysKill`, `SysLock`, `SysUnlock`, `SysReset`, `SysEnd` (also at the end of
simulation) and the action ones. `vpi_control` (39.5) takes every assertion
operation of annex M, on one assertion (`vpi_control(vpiAssertionDisable,
h)`) or the system (`vpi_control(vpiAssertionSysOff)`). The system tasks of
20.11 (`$assertoff`, `$assertcontrol`, ...) call the same callbacks.
Stepping (39.5.2): `vpi_control(vpiAssertionEnableStep, h)` on a
concurrent assertion makes every clock step of each of its attempts call
`cbAssertionStepSuccess` (the attempt goes on or matches) or
`cbAssertionStepFailure` (it fails), with the attempt's start time and
`detail.step` giving `stateFrom` and `stateTo`: the engine's states for the
attempt, numbered per assertion in the order they are first seen (two
attempts in the same state have the same number). `vpiAssertionDisableStep`
turns it off. Not supported: `vpiAssertionClockSteps`, the matched
expressions of a step (`matched_expression_count` is 0), and `failExpr` of
a failure is NULL.

**Coverage (clause 40).** With `msim -cov`, `vpi_get(<coverage type>,
instance)` gives the covered items of that instance: `vpiStatementCoverage`
(source lines of its processes that ran), `vpiToggleCoverage` (bits that
rose and fell), `vpiFsmStateCoverage` (FSM states entered) and
`vpiAssertCoverage` (assertions that succeeded, or covers that matched;
counted with or without `-cov`). Items in instances below it are not
included. `vpiCovered`, `vpiCoveredCount` and `vpiCoveredMax` read an
assertion (succeeded, successes, 1), an FSM (states entered, entries, states),
an FSM state, or a net, variable, word or bit (bits toggled both ways, bits
toggled, width). An assertion also gives `vpiAssertAttemptCovered`,
`vpiAssertSuccessCovered`, `vpiAssertFailureCovered`,
`vpiAssertVacuousSuccessCovered`, `vpiAssertDisableCovered` and
`vpiAssertKillCovered`. `vpi_iterate(vpiFsm, instance)` gives the FSMs msim
found (`vpiFsm` objects with `vpiName`, `vpiFullName`, `vpiFile`,
`vpiLineNo`); `vpi_handle(vpiFsmStateExpression, fsm)` the state register;
`vpi_iterate(vpiFsmStates, fsm)` its states (`vpiFsmHandle` objects whose
`vpi_get_value` is the state's value and `vpiName` its name).
`vpi_control(op, type, handle)` takes `vpiCoverageStart`, `vpiCoverageStop`,
`vpiCoverageReset` and `vpiCoverageCheck` (1 when every item of the type is
covered) with an instance, an assertion (for `vpiAssertCoverage`) or NULL
(the whole design); `vpi_control(vpiCoverageSave, type, name)` writes the
coverage database (the `-cov` JSON) to `name` now, and
`vpi_control(vpiCoverageMerge, type, name)` reads such a database and adds
its counts of that type to the current ones: statement hits by source line,
toggled bits by signal name and width, FSM state and transition counts by
instance and register, assertion attempts, successes and failures by
assertion name (items the design does not have are skipped). Not
supported: starting or stopping statement, toggle or FSM coverage for an
instance below the top (it is for the whole design).

**Structs, containers and classes (37.17, 37.18, 37.29).** An unpacked
struct or union variable is a `vpiStructVar` / `vpiUnionVar` (its value is
one vector of its members); `vpi_iterate(vpiMember, ...)` gives the members
of an unpacked or packed struct or union, nested ones too, which read,
write, force and call back like part selects. A dynamic array, queue or
associative array of integral, real or string elements is a `vpiArrayVar`
with `vpiArrayType` (`vpiDynamicArray`, `vpiQueueArray`, `vpiAssocArray`)
and `vpiSize` (the elements it has now); `vpi_handle_by_index` and
`vpi_iterate(vpiReg, ...)` give its elements (entries in key order), which
`vpi_get_value` reads and `vpi_put_value` writes (no delay). A class
variable is a `vpiClassVar`; `vpi_handle(vpiClassObj, var)` gives the
object it refers to (NULL for a null handle, `vpiObjId`), and
`vpi_iterate(vpiVariables, obj)` its properties, base class first, static
ones included, as variables that read and write.

**Class definitions (37.29).** `vpiClassDefn` from a class variable (its
declared class), a class typespec, a method, or a class object (the class
the object was made of, its dynamic type: a `Base` variable holding a
`Child` object gives `Child`, and the object's `vpiVariables` include
`Child`'s properties). A class definition has `vpiName`, `vpiFullName`,
`vpiVirtual` (1 for a `virtual class`), `vpiLineNo`, `vpiFile`; `vpiExtends`
gives an extends object whose `vpiClassTypespec` names the base class;
`vpiMethods` iterates the tasks and functions the class declares (each with
`vpiVirtual`; a `pure virtual` prototype is one too), `vpiDerivedClasses`
the classes that extend it, and `vpiVariables` its static properties.

**Constraints (37.31).** `vpi_iterate(vpiConstraint, class definition)`
gives its constraint blocks, each with `vpiName`, `vpiFullName`,
`vpiLineNo`, `vpiIsConstraintEnabled` (1: `constraint_mode` starts on),
`vpiVirtual` (a `pure constraint`), `vpiAutomatic` (0 for a `static`
constraint) and `vpiClassDefn`. `vpiConstraintItem` iterates the items: an
expression is the expression object itself; `vpiImplication`,
`vpiConstrIf` and `vpiConstrIfElse` have `vpiCondition`, `vpiConstraintExpr`
(the body) and `vpiElseConst`; `vpiConstrForEach` has `vpiVariables` (the
array), `vpiLoopVars` and `vpiConstraintExpr`; `vpiSoftDisable` has
`vpiExpr`; `vpiConstraintOrdering` (solve before) has `vpiSolveBefore` and
`vpiSolveAfter`. `vpiSoft` is 1 on a `soft` expression item and 0 on a
hard one. A `dist` item is a `vpiDistribution` with `vpiExpr` (the value
distributed) and `vpiDistItem` items, each with `vpiDistType`
(`vpiEqualDist` for `:=`, `vpiDivDist` for `:/`), `vpiWeight` and
`vpiValueRange` (the value, or a `vpiValueRange` object with `vpiLeftRange`
and `vpiRightRange`). `vpi_iterate(vpiConstraint, class object)` gives the
object's blocks, its class's own first and then the inherited ones it does
not override; their `vpiIsConstraintEnabled` follows `constraint_mode`
(18.9) and their `vpiParent` is the object. A `unique` constraint has no
object type in annex M, so it is not an item.

**Drivers and loads (37.16, 37.17).** On a net or variable,
`vpiLocalDriver` gives the continuous assignments inside its module whose
left side writes it and the output (or inout) ports of child instances
connected to it; `vpiLocalLoad` gives the continuous assignments that read
it and the input (or inout) ports of child instances it connects to.
`vpiDriver` and `vpiLoad` follow whole-signal port connections up and down
the hierarchy (the simulated net) and give the continuous assignments of
every part of it, plus ports whose connection is only part of an
expression. `vpiPortInst` gives the ports of the instances the signal
connects to. Procedural assignments are neither drivers nor loads, and the
signal's own module ports are not local drivers or loads. These rules were
written from memory of the standard and still need checking against the
text.

**Processes, statements and expressions (37.32 to 37.51).**
`vpi_iterate(vpiProcess, module)` gives the `initial`, `always` (with
`vpiAlwaysType`: `vpiAlways`, `vpiAlwaysComb`, `vpiAlwaysFF`,
`vpiAlwaysLatch`) and `final` blocks, `vpi_iterate(vpiContAssign, module)`
the continuous assignments (`vpiLhs`, `vpiRhs`, `vpiDelay`). From a process,
`vpiStmt` gives its statement. Statements: `vpiBegin` / `vpiNamedBegin` /
`vpiFork` / `vpiNamedFork` (`vpiStmt` iteration, `vpiJoinType`),
`vpiAssignment` (`vpiBlocking`, `vpiLhs`, `vpiRhs`), `vpiIf` / `vpiIfElse`
(`vpiCondition`, `vpiStmt`, `vpiElseStmt`), `vpiCase` (`vpiCaseType`,
`vpiQualifier`, `vpiCaseItem` iteration; an item has `vpiExpr` iteration
and `vpiStmt`), `vpiFor` (`vpiForInitStmt`, `vpiCondition`,
`vpiForIncStmt`, `vpiStmt`), `vpiWhile`, `vpiDoWhile`, `vpiRepeat`,
`vpiForever`, `vpiForeachStmt`, `vpiDelayControl` (`vpiDelay`),
`vpiEventControl` (`vpiCondition` the event expression), `vpiRepeatControl`,
`vpiTaskCall` / `vpiSysTaskCall` / `vpiMethodTaskCall` (`vpiName`,
`vpiArgument` iteration), `vpiWait`, `vpiWaitFork`, `vpiOrderedWait`,
`vpiDisable`, `vpiDisableFork`, `vpiEventStmt`, `vpiReturn`, `vpiBreak`,
`vpiContinue`, `vpiForce` / `vpiRelease` / `vpiAssignStmt` /
`vpiDeassign`, `vpiNullStmt`; an assertion statement is the assertion
object of clause 39. Expressions: a net, variable, word, bit or parameter
is the object itself; otherwise `vpiOperation` (`vpiOpType`, `vpiOperand`
iteration), `vpiConstant` (`vpiConstType`, value), `vpiFuncCall` /
`vpiSysFuncCall` / `vpiMethodFuncCall` (`vpiName`, `vpiFuncType`,
`vpiArgument`), `vpiBitSelect` (`vpiParent`, `vpiIndex`), `vpiPartSelect` /
`vpiIndexedPartSelect` with a variable index. Every one has `vpiLineNo`,
`vpiFile`, `vpiParent`, `vpiModule`, `vpiScope`; an expression has
`vpiSize` and `vpiDecompile` (its source text). `vpi_get_value` reads an
expression only when it is constant.

**Scheduling and more callbacks (38.10, 38.30, 38.35, 38.36).** A delayed
`vpi_put_value` with `vpiReturnEvent` returns a `vpiSchedEvent` handle;
`vpi_get(vpiScheduled, ev)` is 1 until the write lands, and
`vpi_put_value(ev, NULL, NULL, vpiCancelEvent)` cancels it. A
`vpiInertialDelay` write removes the writes still pending on the object, a
`vpiTransportDelay` write those due after it, a `vpiPureTransportDelay`
write none; a delayed write of a bit keeps the other bits as they are when
it lands. `cbForce`, `cbRelease`, `cbAssign` and `cbDeassign` fire on
`force` / `release` / `assign` / `deassign` (and forces through
`vpi_put_value`) of their object, or of any object when `obj` is NULL (then
`obj` of the callback data is the object). `cbNBASynch` runs before the NBA
updates of its time slot, `cbAtEndOfSimTime` at its end, before
`cbReadOnlySynch`. `vpi_get_delays` / `vpi_put_delays` read and change the
rise, fall and turn-off delays of a net declared with a delay (28.16), as
ticks or scaled time, with `mtm_flag` and `append_flag`. `cbStmt` on a
statement handle calls back each time the statement runs, with the
statement as `obj`; msim finds the statement's code by its source line, so
two statements on one line (`#2 n = n + 1;` is a delay control and an
assignment) share the callback.

**Inter-module paths (38.21).** `vpi_handle_multi(vpiInterModPath, out,
in)` takes an output (or inout) port of one instance and an input (or
inout) port of another that are connected to the same net, in either order,
and gives the same path object each time. `vpi_iterate(vpiPort, path)` gives
the two ports. `vpi_get_delays` / `vpi_put_delays` read and write its delay
(1, 2, 3, 6 or 12 values, with `pulsere_flag` pulse limits and
`append_flag`), which is the interconnect delay of the load port from every
source on the net, the same as an SDF `PORT` or `INTERCONNECT` delay
(32.4.4). The load port has to be a net.

**Timing checks (37, 38.10, 38.36.1).** `vpi_iterate(vpiTchk, module)`
gives the timing checks of a module instance's specify blocks
(`vpiTchkType`, `vpiLineNo`, `vpiFile`). `vpiTchkRefTerm` and
`vpiTchkDataTerm` give the terms (`vpiTchkTerm`) with `vpiEdge` (the
`vpiEdge01` ... flags) and `vpiExpr` (the signal), and `vpiTchkNotifier` the
notifier. `vpi_get_delays` / `vpi_put_delays` read and write the limits:
one, or two for `$setuphold`, `$recrem`, `$fullskew`, `$timeskew`,
`$nochange` and `$width` (limit and threshold); the violation report then
shows the new limits. `cbTchkViolation` runs after each violation report, on
one check or (obj NULL) on any, with the check as `obj`. Not yet: the
condition of a term (`&&&`) as an object, and the delayed signals of 31.9.

**Module paths (37, 38.10).** `vpi_iterate(vpiModPath, module)` gives the
module paths of an instance's specify blocks, one object per input / output
pair (`(a, b *> y)` is two), with `vpiModPathHasIfNone`.
`vpiModPathIn` / `vpiModPathOut` (or `vpiPathTerm`) give the terms, each
with `vpiName`, `vpiDirection`, `vpiEdge` (the input's edge) and `vpiExpr`
(the signal). `vpi_get_delays` / `vpi_put_delays` read and write the
path's delays (1, 2, 3, 6 or 12 values, `pulsere_flag` pulse limits,
`append_flag`); a new delay takes effect for the next change. A path also
has `vpiPathType` (`vpiPathFull` for `*>`, `vpiPathParallel` for `=>`),
`vpiPolarity` (`+`, `-` or unknown), `vpiLineNo`, `vpi_handle(vpiCondition)`
(the `if` expression of a state-dependent path) and, for an edge-sensitive
path, `vpi_iterate(vpiModDataPathIn)` (the data source) with
`vpiDataPolarity`.

**Primitives (37, 38.10).** `vpi_iterate(vpiPrimitive, module)` gives the
gate, switch and UDP instances of a module; `vpiGate`, `vpiSwitch` and
`vpiUdp` select one class. Each has `vpiName`, `vpiFullName`, `vpiDefName`
(the primitive), `vpiPrimType` (`vpiAndPrim` ... `vpiPulldownPrim`,
`vpiSeqPrim` / `vpiCombPrim` for a UDP) and `vpiSize` (the terminal count).
`vpi_iterate(vpiPrimTerm, prim)` gives the terminals in order, with
`vpiTermIndex`, `vpiDirection` (outputs first; `inout` for switch
terminals) and `vpiExpr` (the connection). `vpi_get_delays` /
`vpi_put_delays` read and write a gate's rise, fall and turn-off delays (1,
2, 3, 6 or 12 values, `append_flag`); the new delay takes effect for the
next change. The same two routines read and change the delay of a
continuous assignment (`vpiContAssign`) that has one; one without a delay
is an error.

**Error callbacks (38.36.3).** `cbPLIError` runs each time a VPI routine
reports an error (level `vpiError` or above); `cbError` runs after each
run-time error message of the simulation (`$error`, `$fatal`, a failed
assertion, a failed `$cast`). In either, `vpi_chk_error` gives the error;
for `cbError` the message is the first line of the report (file, line and
scope). An error inside the callback does not call it again.

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

**Protected objects (37.3.6).** `vpiIsProtected`, and the access rules for
objects in decryption envelopes: see "VPI and DPI" in `docs/PROTECT.md`.

**SystemVerilog object callbacks (38.36.1).** `cbCreateObj` runs as a class
object is made, with the new object as `obj`; register it with `obj` NULL
for every class or with a class definition for objects of exactly that
class. `cbSizeChange` on a dynamic array, queue or associative array runs
each time its size changes (push, pop, insert, delete, a new key, a new
size); a write to an existing element does not.

**Threads and frames (38.36.1).** A thread is one process: an initial,
always or final procedure, a fork branch, an assertion action. A frame is
one running call of a task or function. In a `calltf`,
`vpi_handle(vpiThread, NULL)` gives the calling thread and
`vpi_handle(vpiFrame, NULL)` its innermost call (NULL outside a task or
function). A thread has `vpiParent` (the thread that forked it),
`vpiFrame` (its innermost call) and `vpi_iterate(vpiThread, thread)` (the
children still running); a frame has `vpiScope` (the task or function),
`vpiParent` (the caller's frame), `vpiThread` and `vpiAutomatic`. Both have
`vpiValid`, 0 once the thread ends or the call returns. `cbStartOfThread`
runs as a thread starts (registered on a thread: as it forks a child),
`cbEndOfThread` as it ends and `cbEnterThread` each time it runs again after
a wait (registered on a thread: that one; obj NULL: every thread);
`cbStartOfFrame` and `cbEndOfFrame` run as a call starts and returns
(obj NULL, a thread, or for `cbEndOfFrame` one frame). The callback's `obj`
is the thread or frame. Threads that started before the first such
callback or handle was asked for are not reported as starting.

**Save, restart and reset (38.36.2).** msim has no save, restart or reset,
so `cbStartOfSave`, `cbEndOfSave`, `cbStartOfRestart`, `cbEndOfRestart`,
`cbStartOfReset` and `cbEndOfReset` register (with a handle) and never run.

## Not yet

The non-static property
declarations of a class definition, containers of structs,
classes or containers, the value of a non-constant expression, drivers and
loads of a bit or part of a signal and of primitive terminals,
instance arrays as objects, the per-source
delay of an inter-module path on a net with several sources,
interactive mode
callbacks, `cbReclaimObj` /
`cbEndOfObject` (msim does not reclaim objects), `cbSignal`,
`cbUnresolvedSystf`, and `vpi_control(vpiReset)`. A call to something not
implemented returns NULL or `vpiUndefined` and sets an error that
`vpi_chk_error` reports.

## Tests

`tests/spec/37-vpi-walk.sv`, `37-vpi-sv-objects.sv`, `37-vpi-statements.sv`, `37-vpi-containers.sv`, `37-vpi-drivers.sv`, `37-vpi-classes.sv`, `37-vpi-constraints.sv`, `37-vpi-constraint-modes.sv`, `39-vpi-assertions.sv`, `39.5-vpi-assert-step.sv`, `40-vpi-coverage.sv`, `40-vpi-coverage-merge.sv`, `38.10-vpi-delays.sv`, `38.35-vpi-sched-events.sv`, `38.36-vpi-cbstmt.sv`, `38.36-vpi-cberror.sv`, `38.36-vpi-tchk.sv`, `38.36-vpi-sv-callbacks.sv`, `38.36-vpi-threads.sv`, `38.10-vpi-modpath.sv`, `38.10-vpi-primitives.sv`, `38.10-vpi-assign-delay.sv`, `38.21-vpi-handle-multi.sv`, `38.16-vpi-value-array.sv`, `38.34-vpi-put-value.sv`,
`38.36-vpi-callbacks.sv` and `38.37-user-systf.sv` with their C files in
`tests/spec/data/vpi/`; `K-vpi-user-h.sv`, `L-vpi-compatibility.sv` and
`M-sv-vpi-user-h.sv` check the headers. When Icarus Verilog is installed, CI builds the same
C files into an Icarus VPI module (`//! icarus-vpi:`) and checks that Icarus
prints the same output.
