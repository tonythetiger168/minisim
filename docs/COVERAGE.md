# IEEE 1800-2023 coverage

What minisim simulates, clause by clause, against IEEE Std 1800-2023.
Clause numbers were checked against the 2023 text; most are unchanged from 1800-2017.
A ✅ row has a test in `tests/spec/` named after its clause. Each of those tests
is also checked against Icarus Verilog when `iverilog` is installed.
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
| 6.12 | real, shortreal, realtime | ❌ | |
| 6.16 | string type | ❌ | |
| 6.18 | typedef | ✅ | for the types listed here |
| 6.19 | Enumerations | 🟡 | values and compares; no `.name()` / `.next()`. `6.19-enum` |
| 6.20 | parameter / localparam | ✅ | incl. overrides at instantiation. `6.20-parameters` |
| 7.2 | Structures | ❌ | packed and unpacked |
| 7.4 | Packed / unpacked arrays | 🟡 | packed vectors up to 64 bits. Unpacked arrays are an error |
| 7.5–7.10 | Dynamic, associative arrays, queues | ❌ | |
| 8 | Classes | ❌ | blocks UVM |
| 9.2 | initial, always, always_comb/ff/latch | ✅ | |
| 9.2.3 | Final procedures | ❌ | an error |
| 9.3.1 | begin…end | ✅ | |
| 9.3.2 | fork…join / join_any / join_none | ✅ | `9.3.2-fork-join` |
| 9.4.1 | Delay control | 🟡 | integer literal delays only; no `timescale` units |
| 9.4.2 | Event control | 🟡 | posedge / negedge / any change, `or`, `@*`. No named events (`->`) |
| 9.4.3 | wait | ✅ | `9.4.3-wait` |
| 9.4.5 | Intra-assignment delay | ✅ | `a <= #2 b` |
| 9.6 | disable, wait fork | ❌ | |
| 10.3 | Continuous assignment | ✅ | with drive strength |
| 10.3.1 | Net declaration assignment | ✅ | `10.3.1-net-decl-assign` |
| 10.4 | Blocking / nonblocking | ✅ | whole signal, bit-select, part-select |
| 10.5 | Variable declaration assignment | ✅ | runs before every initial/always. `10.5-var-decl-init` |
| 11.4.2 | Increment / decrement | 🟡 | as statements only (`i++;`), not inside expressions |
| 11.4.3 | Arithmetic incl. unary −/+, ** | ✅ | `11.4.3-arith-unary` |
| 11.4.4–11.4.10 | Relational, equality, logical, bitwise, reduction, shift | ✅ | `11.4.5-equality`, `11.4.9-reduction` |
| 11.4.11 | Conditional `?:` | ✅ | |
| 11.4.12 | Concatenation, replication | ✅ | `11.4.12.1-replication` |
| 11.4.13 | inside | ❌ | |
| 11.5.1 | Indexed part-select `+:` `-:` | ❌ | |
| 11.6–11.8 | Expression width and sign rules | ✅ | context-determined widths, signed extension |
| 12.4 | if / else; 12.4.2 unique / priority if | ✅ | the unique/priority violation itself is not reported |
| 12.5 | case, casez, casex; 12.5.3 unique case | ✅ | `12.5-unique-case` |
| 12.7 | for, while, do-while, repeat, forever | ✅ | |
| 12.7.3 | foreach | ❌ | needs unpacked arrays |
| 12.8 | break, continue, return | ✅ | |
| 13 | Tasks and functions | 🟡 | module scope; no recursion, no `ref`, no DPI. `13.4-function-automatic` |
| 16 | Assertions | ❌ | immediate and concurrent |
| 18 | Randomization | ❌ | incl. `$urandom` |
| 20.3 | $time, $stime, $realtime | 🟡 | one time unit. `20.3-time` |
| 20.2 | $finish, $stop | ✅ | `$stop` ends the run like `$finish` |
| 21.2.1 | $display, $write and b/o/h variants | ✅ | %b %o %h %x %d %c %s %t %m, `%0`, widths. `21.2.1-display-formats` |
| 21.2.2–21.2.3 | $strobe, $monitor | ✅ | |
| 21.3 | File I/O ($fopen, $readmemh, ...) | ❌ | currently ignored with a warning |
| 21.7 | VCD ($dumpfile, $dumpvars) | 🟡 | `msim -vcd` dumps every signal; the tasks are ignored |
| 23.3 | Module instances, ports | ✅ | named and positional, parameter overrides |
| 25 | Interfaces, modports | ❌ | |
| 27 | Generate | ❌ | |
| 28 | Gate primitives | 🟡 | and/or/xor families, buf/not, pullup/pulldown |

## Next up

In order of how often typical testbenches hit them:

1. Unpacked arrays and memories (7.4), then `foreach` (12.7.3) and `$readmemh` (21.4)
2. Generate blocks (27) and `+:` / `-:` part-selects (11.5.1)
3. `` `timescale `` (22.7, 20.4) and delay expressions (9.4.1)
4. Named events (15.5), `$urandom` (18.13), immediate assertions (16.3)
5. Packed structs (7.2.1), `inside` (11.4.13), `++` inside expressions (11.4.2)
6. Values wider than 64 bits

Classes (8), randomization (18), and interfaces (25) are what UVM needs.
