#!/usr/bin/env python3
"""Writes a random gate-level netlist for the mpnr benchmarks and tests.

    python3 bench/gen_pnr.py CELLS [SEED] > design.v

The cells are those of tests/pnr/tiny.lef (or, with --mini45, of lib/mini45;
with --nangate, the Nangate45 names; with --sky130, the SkyWater 130 nm hd cells). Each gate input picks a driver that was made shortly before it, so the
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

if "--sky130" in sys.argv:                 # the SkyWater 130 nm high-density cells (lib/sky130)
    P = "sky130_fd_sc_hd__"
    GATES = [(P + "inv_1", ["A"], "Y"), (P + "buf_1", ["A"], "X"), (P + "nand2_1", ["A", "B"], "Y"), (P + "nor2_1", ["A", "B"], "Y"),
             (P + "and2_1", ["A", "B"], "X"), (P + "or2_1", ["A", "B"], "X"), (P + "xor2_1", ["A", "B"], "X"), (P + "xnor2_1", ["A", "B"], "Y"),
             (P + "nand3_1", ["A", "B", "C"], "Y"), (P + "nor3_1", ["A", "B", "C"], "Y"), (P + "a21oi_1", ["A1", "A2", "B1"], "Y"),
             (P + "o21ai_1", ["A1", "A2", "B1"], "Y"), (P + "mux2_1", ["A0", "A1", "S"], "X")]
    FLOP = (P + "dfxtp_1", "D", "CLK", "Q")

if "--mini45" in sys.argv:                 # lib/mini45: the same cells and pins, with a drive strength
    GATES = [(g + "_X1", ins, o) for g, ins, o in GATES]
    FLOP = (FLOP[0] + "_X1",) + FLOP[1:]

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
