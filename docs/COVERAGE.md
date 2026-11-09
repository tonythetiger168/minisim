# IEEE 1800-2023 coverage

What minisim simulates, clause by clause, against IEEE Std 1800-2023.
Clause numbers were checked against the 2023 text; most are unchanged from 1800-2017.
A ✅ row has a test in `tests/spec/` named after its clause. Each of those tests
is also checked against Icarus Verilog 13 when `iverilog` is installed.
🟡 works with the limits listed, ❌ is an error at lowering time ("lower: unsupported ..."),
including any system task msim does not know (only the waveform tasks such as `$dumpvars` are skipped),
never a silent wrong answer. If you find a construct that runs but gives the
wrong result, that is a bug: please add a spec test for it.

`mregress -spec tests/spec` (`tools/mregress.py`) runs every spec test and prints a
per-chapter table. A test marked `//! xfail: <reason>` is a known gap (ctest expects
it to fail); the run fails if any other test fails or if an xfail test starts
passing (XPASS). `-sim iverilog` runs the same suite on Icarus.

| Clause | Topic | Status | Notes / test |
|---|---|---|---|
| 4.4–4.9 | Scheduling regions | ✅ | preponed (sampled values), active, inactive (`#0`), NBA, observed, reactive, re-inactive, re-NBA, postponed; once the reactive set starts it runs until it is empty (4.5). Programs, which also run in the reactive set, are not supported yet (24). `4.9.4-nba-strobe`, `16.4-observed-final` |
| 5.7.1 | Integer literals | ✅ | sized, unsized, x/z digits, signed (`'s`), and `'0` `'1` `'x` `'z` in any context. `5.7.1-unbased-unsized` |
| 5.9 | String literals | 🟡 | up to 8 characters (64 bits). `5.9-string-literal` |
| 6.5–6.7 | Nets: wire, tri, strengths | ✅ | resolution by drive strength (PR #15) |
| 6.6.3–6.6.8 | wand/triand, wor/trior, tri0, tri1, trireg, supply0/supply1 | ✅ | wired AND/OR resolution, pull to 0/1 when undriven, trireg holds its last value (starts at x). Not yet: arrays of these nets, `interconnect`. `6.6.3-wired-nets`, `6.6.4-trireg` |
| 6.11 | Integer types | ✅ | int, byte, shortint, longint, integer, bit. `6.11-integer-types` |
| 6.8 | Default values | ✅ | 2-state variables (int, bit, real, string) start at 0 / 0.0 / ""; 4-state at x. `6.8-default-values` |
| 6.12 | real, realtime | ✅ | arithmetic, compare, `**`, conversion both ways (rounded, 6.12.1), real arrays and function results, `$itor` `$rtoi` `$realtobits` `$bitstoreal`, 20.8 math functions. `shortreal` (rounded to single precision, `$shortrealtobits` `$bitstoshortreal`). Not yet: reals in the VCD. `6.12-real`, `6.12.1-real-conversions`, `6.12-shortreal` |
| 6.16 | string type | ✅ | concatenation, replication, compare, all 6.16 methods, string arrays and function arguments, conversion to and from integral types, `case` on strings. `6.16-string`, `6.16-string-methods` |
| 6.18 | typedef | ✅ | for the types listed here |
| 6.19 | Enumerations | ✅ | values, compares, and the methods `first`, `last`, `num`, `next(N)`, `prev(N)`, `name` (6.19.5); `%p` prints the name. `6.19-enum`, `6.19-enum-methods` |
| 6.24.2 | $cast | 🟡 | integral, real and enum destinations: an enum value that is not a member fails (0, or an error as a task). Class handles come with classes (8). `6.24.2-dollar-cast` |
| 6.20 | parameter / localparam | ✅ | incl. overrides at instantiation. `6.20-parameters` |
| 7.2.1, 7.3.1 | Packed structures and unions | ✅ | member read and write, nested members. `7.2.1-packed-struct` |
| 7.2, 7.3 | Unpacked structures and unions | ✅ | members of any type including real and string, nested structs, arrays of structs, default and named patterns, struct function results, `%p`. `7.2-unpacked-struct`, `7.2-unpacked-struct-members`, `7.3-unpacked-union` |
| 7.3.2, 11.9 | Tagged unions | ✅ | unpacked and packed, `void` members, `tagged M v` expressions, the packed layout of 7.3.2 (tag on top, member right-justified), `.member` reads. Not yet: the run-time check that a member read matches the tag. `7.3.2-tagged-unions` |
| 7.4.1 | Packed arrays and ranges | ✅ | any declared range ([7:0], [8:1], [0:7]) and multidimensional packed arrays, element and part selects. `7.4.1-packed-ranges`, `7.4.1-packed-nested-write` |
| 7.4 | Unpacked arrays | 🟡 | fixed-size unpacked arrays of any number of dimensions (up to 2^20 elements), constant or variable index, read and write; out-of-range read gives X (7.4.6); whole-array assignment from `'{...}`, `'{n{...}}`, `'{i: v, default: v}` (also nested, and in a declaration) or another array. Slices `a[1:2]`, whole-array `==` `!=` `===` `!==`, unpacked concatenation as a value. Unpacked-array ports and `assign arr = ...` of a whole array. Not yet: arrays as subroutine arguments. `7.4-unpacked-arrays`, `7.4-array-sensitivity` |
| 7.5 | Dynamic arrays | ✅ | `new[n]`, `new[n](old)`, `size()`, `delete()`, element read and write (a read past the end gives the element default), assignment from `'{...}`, another container, or a fixed-size array. `7.5-dynamic-arrays` |
| 7.8, 7.9 | Associative arrays | 🟡 | integral keys (signed or unsigned, any width up to 64), string keys, `[*]`; `num`/`size`, `exists`, `delete`, `delete(k)`, `first`/`last`/`next`/`prev`, `'{k: v}` patterns, a missing key reads the default. Not yet: class keys, `default:` in a pattern. `7.8-associative-arrays` |
| 7.10 | Queues | ✅ | `push_back`/`push_front`/`pop_back`/`pop_front`/`insert`/`delete(i)`, `$` as the last index, `q[a:b]` slices, concatenation `{q, x, r}`, `{}`, bounded queues `[$:N]`, a write at `q.size()` appends. `7.10-queues`, `7.10.1-queue-slices`, `7.10-string-real-elements` |
| 7.12 | Array methods | ✅ | on dynamic arrays and queues: `reverse`, `sort`, `rsort`, `shuffle`, `sum`, `product`, `and`, `or`, `xor`; locator methods `find`, `find_index`, `find_first(_index)`, `find_last(_index)`, `min`, `max`, `unique`, `unique_index` (7.12.1); a `with` clause on locators, reductions and `sort`/`rsort` (7.12.4). Locators and reductions also work on fixed-size arrays declared `[N]`. Not yet: `item.index`, methods of associative arrays with `with`, `sort` of a fixed-size array, min/max/unique on real or string keys. `7.12-array-methods`, `7.12.1-locator-methods`, `7.12.3-reduction-methods`, `7.12.4-with-clause`, `7.12-fixed-array-methods` |
| 7.5–7.10 | Container limits | 🟡 | elements are integral (up to 64 bits), real, or string; no nested containers, no containers as ports or task arguments, no whole-container compare, no NBA to an element. `-compile` falls back to the interpreter. |
| 8 | Classes | ❌ | blocks UVM |
| 9.2 | initial, always, always_comb/ff/latch | ✅ | |
| 9.2.3 | Final procedures | ✅ | run once after the last time slot, also after `$finish`. `9.2.3-final` |
| 9.3.1 | begin…end | ✅ | |
| 9.3.2 | fork…join / join_any / join_none | ✅ | automatic variables in a fork block get one copy per execution of the fork (`fork automatic int j = i; ... join_none` in a loop). `9.3.2-fork-join`, `9.3.2-fork-automatic` |
| 9.4.1 | Delay control | ✅ | constant, real, time literal, or any integral expression (evaluated when reached; x or z waits 0). `9.4.5-intra-assign-delay` |
| 9.4.2 | Event control | ✅ | posedge / negedge / any change, `or`, `@*`, named events |
| 9.4.3 | wait | ✅ | `9.4.3-wait` |
| 9.4.5 | Intra-assignment delay | ✅ | `a = #d b` and `a <= #d b`. Blocking intra-assignment event controls `a = @(e) b` and `a = repeat (n) @(e) b`. Not yet: the nonblocking form `a <= @(e) b`. `9.4.5-intra-assign-delay`, `9.4.5-repeat-event-assign` |
| 9.6.1 | wait fork | ✅ | `9.6.1-wait-fork` |
| 9.6.2 | disable | 🟡 | an enclosing named block (jumps past it), or a named block that is a whole initial or fork branch, or a named fork, in another process (ends it and its children). Not yet: disabling a task, or a block nested inside another process; a jump out of a block does not end processes it forked. `9.6.2-disable` |
| 9.6.3 | disable fork | ✅ | `9.6.3-disable-fork` |
| 10.3 | Continuous assignment | ✅ | with drive strength; to a whole signal or a constant bit / part select |
| 10.3.1 | Net declaration assignment | ✅ | `10.3.1-net-decl-assign` |
| 10.3.3 | Continuous assignment delays | ✅ | `assign #(r, f, z)`, `wire #d w = a`, net delays `wire #d w;`, gate delays; inertial, rise/fall/turn-off picked per Table 28-9, min:typ:max uses typ. `10.3.3-assign-delay`, `28.16-gate-delays` |
| 10.6 | assign / deassign, force / release | ✅ | on whole variables and nets, constant bit and part selects of nets, and concatenations of these (10.6.2); the expression is re-evaluated whenever its operands change; release hands a net back to its drivers, and a variable keeps its value. Not yet: inside an automatic task. `10.6.1-assign-deassign`, `10.6.2-force-release`, `10.6.2-force-expression`, `10.6.2-force-bits` |
| 10.4 | Blocking / nonblocking | ✅ | whole signal, bit-select, part-select, and a concatenation `{a, b} = v` (also in `assign`). `11.4.12-concatenation-lvalue` |
| 10.5 | Variable declaration assignment | ✅ | runs before every initial/always. `10.5-var-decl-init` |
| 10.11 | Net aliasing | ✅ | `alias a = b = c;` makes one net. `10.11-net-alias` |
| 11.3.6 | Assignment inside an expression | ✅ | `if ((a = b) == 0)`, blocking only |
| 11.4.2 | Increment / decrement | ✅ | also inside expressions (`x = i++ + 1`, `while (j++ < 3)`). `11.4.2-incdec-in-expr` |
| 11.4.3 | Arithmetic incl. unary −/+, ** | ✅ | `11.4.3-arith-unary` |
| 11.4.4–11.4.10 | Relational, equality, logical, bitwise, reduction, shift | ✅ | `11.4.5-equality`, `11.4.9-reduction` |
| 11.4.11 | Conditional `?:` | ✅ | |
| 11.4.12 | Concatenation, replication | ✅ | `11.4.12.1-replication` |
| 11.4.6 | Wildcard equality `==?` `!=?` | ✅ | `11.4.2-incdec-in-expr` |
| 11.4.13 | inside | ✅ | values and `[lo:hi]` ranges. Not yet: unpacked arrays as items, tolerance ranges. `11.4.13-inside` |
| 11.4.14 | Streaming operators | ✅ | `{>>{...}}`, `{<<n{...}}`, `{<<type{...}}` of integral operands and fixed-size arrays, nested streams, a wider target left-aligned, and unpacking `{>>{a, b}} = v` (11.4.14.3). Not yet: queues and dynamic arrays as operands or targets, structs as operands, `with [...]`. `11.4.14-streaming` |
| 11.5.1 | Bit-select with a variable index | ✅ | read and write. `11.5.1-variable-bit-select` |
| 11.5.1 | Indexed part-select `+:` `-:` | ✅ | constant or variable start, read and write; out-of-range bits read x and are not written. `11.5.1-indexed-part-select` |
| 11.6–11.8 | Expression width and sign rules | ✅ | context-determined widths, signed extension |
| 12.4 | if / else; 12.4.2 unique / priority if | ✅ | the unique/priority violation itself is not reported |
| 12.5 | case, casez, casex; 12.5.3 unique case; 12.5.4 case inside | ✅ | `12.5-unique-case`, `12.5.4-case-inside` |
| 12.6 | Pattern matching | ✅ | `if (x matches p)` with `&&&` guards, `case`/`casez`/`casex (x) matches` with `&&&` filters; tagged, variable (`.v`), wildcard (`.*`), constant and structure (`'{.a, 4}`) patterns. Not yet: `matches` in a `?:` expression, real or string constant patterns. `7.3.2-tagged-unions` |
| 12.7 | for, while, do-while, repeat, forever | ✅ | |
| 12.7.3 | foreach | ✅ | fixed-size arrays, left-to-right per dimension, skipped dimensions, break/continue; dynamic arrays and queues by index, associative arrays in key order. `12.7.3-foreach`, `12.7.3-foreach-order`, `12.7.3-foreach-containers` |
| 12.8 | break, continue, return | ✅ | |
| 13 | Tasks and functions | 🟡 | module scope. Automatic tasks, and automatic functions that recurse or fork, run each call in its own frame (13.3.2, 13.4.2): recursion, overlapping calls from several processes. An automatic local starts at its default on every call (6.21). Not yet: `ref`, DPI, arrays or containers as arguments of an automatic task. `13.4-function-automatic`, `6.21-recursion`, `13.3.2-automatic-tasks`, `13.4.2-recursive-task` |
| 14 | Clocking blocks | ✅ | Inputs and inouts sampled with `#1step` (the value before the time step), `#0` (in the observed region), or `#N` (the value N time units before the event), and default skews (14.4, 14.13). Outputs: synchronous drives `cb.v[hi:lo] <= ##n e` in the re-NBA region after the output skew, the next event for a drive between events, last drive wins (14.16, 14.16.2). Targets may be hierarchical, slices, or concatenations; a net target gets its own driver, Z until the first drive. `@(cb)` runs after the inputs are updated, in the observed region (14.10); `@(cb.x)` waits on a sampled value (14.15); `##n` and `##0` (14.11); default clocking (14.12); global clocking and `$global_clock` (14.14); a clocking event with `iff`; clocking blocks in interfaces; `@(cb)` as an assertion clock. Not yet: skews with an edge (`output negedge #1`), clocking blocks in programs (24). `14.3-clocking-blocks`, `14.11-cycle-delay`, `14.13-input-skews`, `14.16-synchronous-drives`, `14.16-output-skews`, `14.9-interface-clocking`, `14.14-global-clocking` |
| 15.5 | Named events | ✅ | `->`, `->>` (also with a delay), `@e`, `.triggered`, hierarchical `@u.e`. `wait_order` with pass and fail actions (15.5.4). Not yet: event aliasing (`e1 = e2`). `15.5-events`, `15.5.3-triggered`, `15.5.4-wait-order` |
| 16.3 | Immediate assertions | ✅ | `assert`, `assume`, `cover` with pass and fail actions; an assert or assume with no `else` calls `$error`, named by its label (`top.a1`). `16.3-immediate-assertions` |
| 16.4 | Deferred assertions | ✅ | `#0` (observed) and `final`, in procedures and as module items (an implied `always_comb`, 16.4.3). Reports wait on a per-process queue; a resume after an event control, or a `disable`, flushes it (16.4.2). Action arguments are evaluated when the assertion runs; observed actions run in the reactive region, final ones in the postponed region. `16.4-deferred-assertions`, `16.4.2-deferred-flush`, `16.4-observed-final` |
| 16.5.1, 16.9.3 | Sampled values, sampled value functions | ✅ | `$sampled`, `$rose`, `$fell`, `$stable`, `$changed`, `$past(e, n, gate, clk)`. The clock is the explicit one, else the procedure's leading event control (16.14.6), else the default clocking. Values at time 0 are the default sampled values; automatic variables are read as they are. Not yet: the global clocking functions (16.9.4). `20.12-sampled-values`, `16.9.3-past` |
| 16.5–16.13 | Sequences and properties | ✅ | Concurrent assertions run on a derivative engine: each attempt is a term that steps on the sampled values at every clock tick, and equal attempts merge. Sequences: `##[m:n]`, `[*]`, `[->]`, `[=]`, `and`, `or`, `intersect`, `within`, `throughout`, `first_match`, sequence instances with arguments. Properties: `not`, `and`, `or`, `implies`, `iff`, `\|->`, `\|=>`, `#-#`, `#=#`, `if`/`else`, `case`, `nexttime`, `s_nexttime`, `always`, `s_always`, `eventually`, `s_eventually`, `until` and its strong and `_with` forms, `accept_on`, `reject_on` and their sync forms, `strong()`, `weak()`. Weak and strong properties still pending at the end of the simulation pass or fail (16.12.2). `disable iff` (16.12) is checked whenever its condition changes. Not yet: local variables and match items (16.10, 16.11), `.triggered` / `.matched` (16.13.6), multiclock properties (16.13), recursive properties. A differential fuzzer against a reference model agrees on 1499 of 1500 random properties (the one difference is the model's own approximation of weak sequences). `16.5-concurrent-assertions`, `16.12-property-operators`, `16.14-cover-disable` |
| 16.14 | assert, assume, cover property, cover sequence | ✅ | Pass and fail actions run in the reactive region (16.14.1); `assume` acts like `assert`; `restrict` is ignored (formal only); `cover sequence` passes once per match end of each attempt (a set, not the multiplicity of paths). Assertion control (20.11) applies, including `$assertkill` of attempts in flight. Not yet: `expect` (16.17). `16.14-cover-disable` |
| 16.14.6 | Concurrent assertions in procedures | ✅ | The clock is inferred from the procedure's leading event control. An instance is queued when the procedure reaches it, flushed if the process resumes again in the same step, and matured in the observed region; it starts at that tick or the next one. Automatic variables are read as they were when queued, in the property and the action block. `16.14.6-procedural-assertions`, `16.14.6-flush` |
| 16.15, 16.16 | Default disable iff, clock inference | ✅ | `default disable iff`; the clock from the assertion, else the procedure, else the default clocking. |
| 18 | Randomization (constraints) | ❌ | classes, `randomize()`, `std::randomize`, randcase, randsequence |
| 18.13 | $urandom, $urandom_range | ✅ | one deterministic stream per run, seeded by `-seed <n>` or `+ntb_random_seed=<n>` (the generator is implementation-defined). `18.13-random-functions` |
| 20.3 | $time, $stime, $realtime | 🟡 | in the calling module's time unit, rounded. `$realtime` is a real. `20.3-time` |
| 20.2 | $finish, $stop | ✅ | `$stop` ends the run like `$finish` |
| 20.4 | $timeformat, $printtimescale | ✅ | `%t` prints in the `$timeformat` units, precision, suffix and width; the default is the simulation precision, width 20. `$printtimescale` with no argument, `$root`, or an instance. `20.4-timeformat`, `20.4-printtimescale` |
| 20.8.1 | $clog2 | ✅ | `20.8-clog2` |
| 20.9 | Bit vector functions | ✅ | `$countones`, `$countbits`, `$onehot`, `$onehot0`, `$isunknown`. `20.9-bit-vector-functions` |
| 20.11 | Assertion control | ✅ | `$asserton`, `$assertoff`, `$assertkill`, `$assertpasson/off`, `$assertfailon/off`, `$assertnonvacuouson`, `$assertvacuousoff`, `$assertcontrol` with control, assertion and directive types, levels, scopes and assertion names, lock and unlock. Not yet: control of unique/priority violation reports. `20.11-assertion-control` |
| 20.10 | Severity tasks | ✅ | `$fatal` / `$error` / `$warning` / `$info` print the severity, file, line, scope and time, then the message; `$fatal` ends the run with exit status 1. `20.10-severity-tasks` |
| 20.14 | $random, $dist_uniform | ✅ | the Annex N algorithm, so a seed gives the same values as other simulators. Not yet: the other `$dist_*` functions. `20.14-distribution` |
| 21.2.1 | $display, $write and b/o/h variants | ✅ | %b %o %h %x %d %c %s %t %m %f %e %g %p, `%0`, widths, `%-8d`, `%.3f`. `%m` is the scope of the call. `21.2.1-display-formats` |
| 21.2.1.6 | %p assignment pattern format | 🟡 | fixed-size arrays (any dimensions), dynamic arrays, queues, associative arrays, packed structs with member names, enums by name, strings in quotes. Not yet: unpacked structs (7.2). `21.2.1.6-assignment-pattern-format` |
| 21.2.2–21.2.3 | $strobe, $monitor | ✅ | |
| 21.3 | File I/O | ✅ | `$fopen` (descriptors and multichannel), `$fclose`, `$fdisplay`/`$fwrite` and b/o/h forms, `$fgets`, `$fgetc`, `$ungetc`, `$feof`, `$fscanf`, `$sscanf` (all conversions, `%*`, widths), `$fseek`, `$ftell`, `$rewind`, `$fflush`. Not yet: `$fstrobe`, `$fmonitor`, `$fread`. `21.3-file-io`, `21.3.1-multichannel` |
| 21.3.3 | $sformatf, $sformat, $swrite | ✅ | the same formats as $display |
| 21.4 | $readmemh, $readmemb | 🟡 | 1-D memories: comments, `@addr`, start/finish, descending loads, the word-count warning. Not yet: multidimensional targets. `21.4-readmem` |
| 21.5 | $writememh, $writememb | ✅ | 1-D memories, optional start/finish. `21.5-writemem` |
| 21.6 | $test$plusargs, $value$plusargs | ✅ | `msim t.sv +N=5`; formats %d %h %o %b %s %f. `21.6-plusargs` |
| 20.6.2 | $bits | ✅ | of any fixed-size expression or type |
| 21.7 | VCD ($dumpfile, $dumpvars) | 🟡 | `msim -vcd` dumps every signal; the tasks are ignored |
| 22.7 | `` `timescale `` | ✅ | per-module unit and precision; an element with none, in a design where others have one, gets 1ns/1ps (3.14.2.3; `--timescale` changes it); one kernel tick is the finest precision in the design, also used as the VCD `$timescale`. `22.7-timescale` |
| 23.3 | Module instances, ports | ✅ | named and positional, parameter overrides; unpacked-array input and output ports, element by element. `7.6-array-ports` |
| 23.6 | Hierarchical names | ✅ | read and write `u.x`, `blk[2].x`. `23.6-hierarchical-names` |
| 25.3 | Interfaces | 🟡 | interface instances with ports and parameters, interface ports (generic `cnt_if c` or `bus_if.src b`), signals, nets with continuous assigns, processes, tasks and functions inside, `bus.send()` from the instantiating module. Not yet: interface arrays, virtual interfaces (needs classes, 25.9). `25.3-interface-ports` |
| 25.5 | Modports | 🟡 | input/output/inout lists and `import` of interface functions. Not yet: modport expressions (25.5.4), `export`. Directions are not enforced. `25.5-modports` |
| 26 | Packages | ✅ | parameters, localparams, typedefs, enums, structs, variables (named `pkg::x` in the VCD), functions and tasks; wildcard and explicit import, `pkg::name`. `26.3-package-import`, `26.2-package-items` |
| 27 | Generate | ✅ | loop, if, and case generate; named and unnamed blocks (`genblkN`); instances and continuous assigns to separate bits inside. `27-generate` |
| 28 | Gate primitives | 🟡 | and/or/xor families, buf/not, pullup/pulldown |
| 30.4 | Module path delays | 🟡 | `(a => b) = d` and `(a *> b) = (r, f)` with constant or specparam delays, to an output net. Every path to one output must have the same delays (minisim does not pick the path by which input changed last). Not yet: 6- and 12-value delays, outputs declared as variables, `$sdf_annotate` (an error), timing checks (`$setup`, `$hold`, ... are accepted and never report). `30.4-path-delays` |

## Next up

In order of how often typical testbenches hit them:

1. ~~Unpacked arrays and memories (7.4), `foreach` (12.7.3), `$readmemh` (21.4)~~ done
2. ~~Generate blocks (27), `+:` / `-:` part-selects (11.5.1)~~ done; values wider than 64 bits next
3. ~~`` `timescale `` (22.7), delay expressions (9.4.1), named events (15.5), disable / wait fork (9.6), final (9.2.3)~~ done
4. ~~`$urandom` (18.13), immediate and deferred assertions (16.3, 16.4), concurrent assertions (16.5–16.16), clocking blocks (14)~~ done; covergroups (19) next
5. ~~Packed structs (7.2.1), `inside` (11.4.13), `++` inside expressions (11.4.2), real (6.12), string (6.16)~~ done
6. ~~Dynamic arrays, queues, associative arrays (7.5, 7.10, 7.8)~~ done; unpacked structs (7.2) next

7. ~~Packages (26), interfaces and modports (25)~~ done

Classes (8) and randomization (18) are what UVM needs next.
