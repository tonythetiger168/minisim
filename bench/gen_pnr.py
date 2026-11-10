#!/usr/bin/env python3
"""Writes a random gate-level netlist for the mpnr benchmarks and tests.

    python3 bench/gen_pnr.py CELLS [SEED] > design.v

The cells are those of tests/pnr/tiny.lef (or, with --nangate, the Nangate45
names). Each gate input picks a driver that was made shortly before it, so the
netlist has the locality of real logic; one cell in eight is a flop on `clk`.
"""
import random
import sys

args = [a for a in sys.argv[1:] if not a.startswith("--")]
nangate = "--nangate" in sys.argv
n = int(args[0]) if args else 1000
rng = random.Random(int(args[1]) if len(args) > 1 else 1)

if nangate:
    GATES = [("INV_X1", ["A"], "ZN"), ("BUF_X1", ["A"], "Z"), ("NAND2_X1", ["A1", "A2"], "ZN"), ("NOR2_X1", ["A1", "A2"], "ZN"),
             ("AND2_X1", ["A1", "A2"], "ZN"), ("OR2_X1", ["A1", "A2"], "ZN"), ("XOR2_X1", ["A", "B"], "Z"),
             ("NAND3_X1", ["A1", "A2", "A3"], "ZN"), ("AOI21_X1", ["A", "B1", "B2"], "ZN"), ("OAI21_X1", ["A", "B1", "B2"], "ZN"),
             ("MUX2_X1", ["A", "B", "S"], "Z")]
    FLOP = ("DFF_X1", "D", "CK", "Q")
else:
    GATES = [("INV", ["A"], "Y"), ("BUF", ["A"], "Y"), ("NAND2", ["A", "B"], "Y"), ("NOR2", ["A", "B"], "Y"),
             ("AND2", ["A", "B"], "Y"), ("OR2", ["A", "B"], "Y"), ("XOR2", ["A", "B"], "Y"), ("XNOR2", ["A", "B"], "Y"),
             ("NAND3", ["A", "B", "C"], "Y"), ("NOR3", ["A", "B", "C"], "Y"), ("AOI21", ["A1", "A2", "B"], "Y"),
             ("OAI21", ["A1", "A2", "B"], "Y"), ("MUX2", ["A", "B", "S"], "Y")]
    FLOP = ("DFF", "D", "CK", "Q")

nin = max(4, int(n ** 0.5))
nout = max(4, int(n ** 0.5))
drivers = [f"in[{i}]" for i in range(nin)]
lines = []

def pick():
    # Mostly a driver made just before, sometimes one from the same block,
    # rarely one from far away: the locality of real logic.
    r = rng.random()
    mean = 12.0 if r < 0.88 else 150.0 if r < 0.985 else 3000.0
    back = min(len(drivers), 1 + int(rng.expovariate(1 / mean)))
    return drivers[-back]

for i in range(n):
    out = f"n{i}"
    if i % 8 == 7:
        cell, d, ck, q = FLOP
        lines.append(f"  {cell} r{i} (.{d}({pick()}), .{ck}(clk), .{q}({out}));")
    else:
        cell, ins, o = rng.choice(GATES)
        conns = ", ".join(f".{p}({pick()})" for p in ins)
        lines.append(f"  {cell} u{i} ({conns}, .{o}({out}));")
    drivers.append(out)

print(f"// {n} cells, seed {args[1] if len(args) > 1 else 1}. Written by bench/gen_pnr.py.")
print(f"module rand{n} (input clk, input [{nin - 1}:0] in, output [{nout - 1}:0] out);")
print("  wire " + ", ".join(f"n{i}" for i in range(n)) + ";")
print("\n".join(lines))
for i in range(nout):
    print(f"  assign out[{i}] = n{n - 1 - i * max(1, n // (2 * nout))};")
print("endmodule")
