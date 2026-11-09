# IEEE 1800-2023 coverage

What minisim simulates, clause by clause, against IEEE Std 1800-2023.
Clause numbers were checked against the 2023 text; most are unchanged from 1800-2017.
A ✅ row has a test in `tests/spec/` named after its clause. Each of those tests
is also checked against Icarus Verilog 13 when `iverilog` is installed.
🟡 works with the limits listed, ❌ is an error at lowering time ("lower: unsupported ..."),
never a silent wrong answer. If you find a construct that runs but gives the
wrong result, that is a bug: please add a spec test for it.

| Clause | Topic | Status | Notes / test |
|---|---|---|---|
| 4.4–4.9 | Scheduling regions | ✅ | active, inactive (`#0`), NBA, postponed. `4.9.4-nba-strobe` |
| 5.7.1 | Integer literals | ✅ | sized, unsized, x/z digits, signed (`'s`) |
| 5.9 | String literals | 🟡 | up to 8 characters (64 bits). `5.9-string-literal` |
| 6.5–6.7 | Nets: wire, tri, strengths | ✅ | resolution by drive strength (PR #15) |
| 6.11 | Integer types | ✅ | int, byte, shortint, longint, integer, bit. `6.11-integer-types` |
| 6.8 | Default values | ✅ | 2-state variables (int, bit, real, string) start at 0 / 0.0 / ""; 4-state at x. `6.8-default-values` |
| 6.12 | real, realtime | ✅ | arithmetic, compare, `**`, conversion both ways (rounded, 6.12.1), real arrays and function results, `$itor` `$rtoi` `$realtobits` `$bitstoreal`, 20.8 math functions. Not yet: `shortreal` (an error), reals in the VCD. `6.12-real`, `6.12.1-real-conversions` |
| 6.16 | string type | ✅ | concatenation, replication, compare, all 6.16 methods, string arrays and function arguments, conversion to and from integral types, `case` on strings. `6.16-string`, `6.16-string-methods` |
| 6.18 | typedef | ✅ | for the types listed here |
| 6.19 | Enumerations | 🟡 | values and compares; no `.name()` / `.next()`. `6.19-enum` |
| 6.20 | parameter / localparam | ✅ | incl. overrides at instantiation. `6.20-parameters` |
| 7.2.1, 7.3.1 | Packed structures and unions | ✅ | member read and write, nested members. `7.2.1-packed-struct` |
| 7.2 | Unpacked structures | ❌ | |
| 7.4.1 | Packed arrays and ranges | ✅ | any declared range ([7:0], [8:1], [0:7]) and multidimensional packed arrays, element and part selects. `7.4.1-packed-ranges`, `7.4.1-packed-nested-write` |
| 7.4 | Unpacked arrays | 🟡 | fixed-size unpacked arrays of any number of dimensions (up to 2^20 elements), constant or variable index, read and write; out-of-range read gives X (7.4.6). Not yet: slices, whole-array assignment or compare, arrays as ports or arguments. `7.4-unpacked-arrays`, `7.4-array-sensitivity` |
| 7.5 | Dynamic arrays | ✅ | `new[n]`, `new[n](old)`, `size()`, `delete()`, element read and write (a read past the end gives the element default), assignment from `'{...}`, another container, or a fixed-size array. `7.5-dynamic-arrays` |
| 7.8, 7.9 | Associative arrays | 🟡 | integral keys (signed or unsigned, any width up to 64), string keys, `[*]`; `num`/`size`, `exists`, `delete`, `delete(k)`, `first`/`last`/`next`/`prev`, `'{k: v}` patterns, a missing key reads the default. Not yet: class keys, `default:` in a pattern. `7.8-associative-arrays` |
| 7.10 | Queues | ✅ | `push_back`/`push_front`/`pop_back`/`pop_front`/`insert`/`delete(i)`, `$` as the last index, `q[a:b]` slices, concatenation `{q, x, r}`, `{}`, bounded queues `[$:N]`, a write at `q.size()` appends. `7.10-queues`, `7.10.1-queue-slices`, `7.10-string-real-elements` |
| 7.12 | Array methods | 🟡 | on dynamic arrays and queues: `reverse`, `sort`, `rsort`, `shuffle`, `sum`, `product`, `and`, `or`, `xor`. Not yet: a `with` clause, locator methods (`find`, `min`, `unique`, ...). `7.12-array-methods`, `7.12.3-reduction-methods` |
| 7.5–7.10 | Container limits | 🟡 | elements are integral (up to 64 bits), real, or string; no nested containers, no containers as ports or task arguments, no `%p`, no whole-container compare, no NBA to an element. `-compile` falls back to the interpreter. |
| 8 | Classes | ❌ | blocks UVM |
| 9.2 | initial, always, always_comb/ff/latch | ✅ | |
| 9.2.3 | Final procedures | ✅ | run once after the last time slot, also after `$finish`. `9.2.3-final` |
| 9.3.1 | begin…end | ✅ | |
| 9.3.2 | fork…join / join_any / join_none | ✅ | `9.3.2-fork-join` |
| 9.4.1 | Delay control | ✅ | constant, real, time literal, or any integral expression (evaluated when reached; x or z waits 0). `9.4.5-intra-assign-delay` |
| 9.4.2 | Event control | ✅ | posedge / negedge / any change, `or`, `@*`, named events |
| 9.4.3 | wait | ✅ | `9.4.3-wait` |
| 9.4.5 | Intra-assignment delay | ✅ | `a = #d b` and `a <= #d b`. Intra-assignment event controls (`a = @e b`) are an error. `9.4.5-intra-assign-delay` |
| 9.6.1 | wait fork | ✅ | `9.6.1-wait-fork` |
| 9.6.2 | disable | 🟡 | an enclosing named block (jumps past it), or a named block that is a whole initial or fork branch, or a named fork, in another process (ends it and its children). Not yet: disabling a task, or a block nested inside another process; a jump out of a block does not end processes it forked. `9.6.2-disable` |
| 9.6.3 | disable fork | ✅ | `9.6.3-disable-fork` |
| 10.3 | Continuous assignment | ✅ | with drive strength; to a whole signal or a constant bit / part select |
| 10.3.1 | Net declaration assignment | ✅ | `10.3.1-net-decl-assign` |
| 10.4 | Blocking / nonblocking | ✅ | whole signal, bit-select, part-select |
| 10.5 | Variable declaration assignment | ✅ | runs before every initial/always. `10.5-var-decl-init` |
| 11.3.6 | Assignment inside an expression | ✅ | `if ((a = b) == 0)`, blocking only |
| 11.4.2 | Increment / decrement | ✅ | also inside expressions (`x = i++ + 1`, `while (j++ < 3)`). `11.4.2-incdec-in-expr` |
| 11.4.3 | Arithmetic incl. unary −/+, ** | ✅ | `11.4.3-arith-unary` |
| 11.4.4–11.4.10 | Relational, equality, logical, bitwise, reduction, shift | ✅ | `11.4.5-equality`, `11.4.9-reduction` |
| 11.4.11 | Conditional `?:` | ✅ | |
| 11.4.12 | Concatenation, replication | ✅ | `11.4.12.1-replication` |
| 11.4.6 | Wildcard equality `==?` `!=?` | ✅ | `11.4.2-incdec-in-expr` |
| 11.4.13 | inside | ✅ | values and `[lo:hi]` ranges. Not yet: unpacked arrays as items, tolerance ranges. `11.4.13-inside` |
| 11.5.1 | Bit-select with a variable index | ✅ | read and write. `11.5.1-variable-bit-select` |
| 11.5.1 | Indexed part-select `+:` `-:` | ✅ | constant or variable start, read and write; out-of-range bits read x and are not written. `11.5.1-indexed-part-select` |
| 11.6–11.8 | Expression width and sign rules | ✅ | context-determined widths, signed extension |
| 12.4 | if / else; 12.4.2 unique / priority if | ✅ | the unique/priority violation itself is not reported |
| 12.5 | case, casez, casex; 12.5.3 unique case | ✅ | `12.5-unique-case` |
| 12.7 | for, while, do-while, repeat, forever | ✅ | |
| 12.7.3 | foreach | ✅ | fixed-size arrays, left-to-right per dimension, skipped dimensions, break/continue; dynamic arrays and queues by index, associative arrays in key order. `12.7.3-foreach`, `12.7.3-foreach-order`, `12.7.3-foreach-containers` |
| 12.8 | break, continue, return | ✅ | |
| 13 | Tasks and functions | 🟡 | module scope; no recursion, no `ref`, no DPI. `13.4-function-automatic` |
| 15.5 | Named events | ✅ | `->`, `->>` (also with a delay), `@e`, `.triggered`, hierarchical `@u.e`. Not yet: event aliasing (`e1 = e2`), `wait_order`. `15.5-events`, `15.5.3-triggered` |
| 16 | Assertions | ❌ | immediate and concurrent |
| 18 | Randomization | ❌ | incl. `$urandom` |
| 20.3 | $time, $stime, $realtime | 🟡 | in the calling module's time unit, rounded. `$realtime` is a real. `20.3-time` |
| 20.2 | $finish, $stop | ✅ | `$stop` ends the run like `$finish` |
| 21.2.1 | $display, $write and b/o/h variants | ✅ | %b %o %h %x %d %c %s %t %m %f %e %g, `%0`, widths, `%-8d`, `%.3f`. `21.2.1-display-formats` |
| 21.2.2–21.2.3 | $strobe, $monitor | ✅ | |
| 21.3 | File I/O ($fopen, $fwrite, ...) | ❌ | ignored with a warning |
| 21.3.3 | $sformatf, $sformat, $swrite | ✅ | the same formats as $display |
| 21.4 | $readmemh, $readmemb | 🟡 | 1-D memories: comments, `@addr`, start/finish, descending loads, the word-count warning. Not yet: multidimensional targets. `21.4-readmem` |
| 21.7 | VCD ($dumpfile, $dumpvars) | 🟡 | `msim -vcd` dumps every signal; the tasks are ignored |
| 22.7 | `` `timescale `` | ✅ | per-module unit and precision; one kernel tick is the finest precision in the design, also used as the VCD `$timescale`. `22.7-timescale` |
| 23.3 | Module instances, ports | ✅ | named and positional, parameter overrides |
| 23.6 | Hierarchical names | ✅ | read and write `u.x`, `blk[2].x`. `23.6-hierarchical-names` |
| 25 | Interfaces, modports | ❌ | |
| 27 | Generate | ✅ | loop, if, and case generate; named and unnamed blocks (`genblkN`); instances and continuous assigns to separate bits inside. `27-generate` |
| 28 | Gate primitives | 🟡 | and/or/xor families, buf/not, pullup/pulldown |

## Next up

In order of how often typical testbenches hit them:

1. ~~Unpacked arrays and memories (7.4), `foreach` (12.7.3), `$readmemh` (21.4)~~ done
2. ~~Generate blocks (27), `+:` / `-:` part-selects (11.5.1)~~ done; values wider than 64 bits next
3. ~~`` `timescale `` (22.7), delay expressions (9.4.1), named events (15.5), disable / wait fork (9.6), final (9.2.3)~~ done
4. `$urandom` (18.13), immediate assertions (16.3)
5. ~~Packed structs (7.2.1), `inside` (11.4.13), `++` inside expressions (11.4.2), real (6.12), string (6.16)~~ done
6. ~~Dynamic arrays, queues, associative arrays (7.5, 7.10, 7.8)~~ done; unpacked structs (7.2) next

Classes (8), randomization (18), and interfaces (25) are what UVM needs.
