#!/usr/bin/env python3
"""Writes a random gate netlist over tests/sta/sta_test.lib and its SDC, for
the msta thread-determinism test and for benchmarks.

    gen_netlist.py CELLS OUT.v OUT.sdc [SEED]

The design is a pipeline of register stages with random combinational logic
between them, a small clock tree of buffers, and two clocks.
"""
import random
import sys

n = int(sys.argv[1])
out_v, out_sdc = sys.argv[2], sys.argv[3]
rnd = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 1)
GATES = [("INV_X1", 1), ("BUF_X1", 1), ("NAND2_X1", 2), ("NOR2_X1", 2), ("AND2_X1", 2), ("OR2_X1", 2),
         ("XOR2_X1", 2), ("MUX2_X1", 3)]
PINS = ["A", "B", "S"]
width = max(8, int(n ** 0.5))
depth = 12
stages = max(2, n // (width * (depth + 1)))
lines, wires = [], []
nin = width
prev = ["in[%d]" % i for i in range(nin)]
cell = 0
# Clock tree: one root buffer per clock, a leaf buffer per 16 registers.
leaves = {}
def leaf(clk, k):
    key = (clk, k // 16)
    if key not in leaves:
        w = "%s_l%d" % (clk, key[1])
        wires.append(w)
        lines.append("  BUF_X1 %s_buf%d (.A(%s_root), .Y(%s));" % (clk, key[1], clk, w))
        leaves[key] = w
    return leaves[key]
for c in ("clk", "clk2"):
    wires.append(c + "_root")
    lines.append("  BUF_X1 %s_rootbuf (.A(%s), .Y(%s_root));" % (c, c, c))
regs = 0
for s in range(stages):
    clk = "clk" if s % 3 else "clk2"
    q = []
    for i, d in enumerate(prev):
        w = "q%d_%d" % (s, i)
        wires.append(w)
        typ = "DFFR_X1" if i % 7 == 0 else "DFF_X1"
        extra = ", .RN(rst_n)" if typ == "DFFR_X1" else ""
        lines.append("  %s r%d_%d (.CK(%s), .D(%s)%s, .Q(%s));" % (typ, s, i, leaf(clk, regs), d, extra, w))
        regs += 1
        q.append(w)
    level = q
    for l in range(depth):
        nxt = []
        for i in range(width):
            g, k = rnd.choice(GATES)
            w = "n%d" % cell
            wires.append(w)
            ins = [rnd.choice(level if rnd.random() < 0.8 else q) for _ in range(k)]
            conn = ", ".join(".%s(%s)" % (PINS[j], ins[j]) for j in range(k))
            lines.append("  %s u%d (%s, .Y(%s));" % (g, cell, conn, w))
            cell += 1
            nxt.append(w)
        level = nxt
    prev = level
with open(out_v, "w") as f:
    f.write("module big (input clk, input clk2, input rst_n, input [%d:0] in, output [%d:0] out);\n" % (nin - 1, width - 1))
    for i in range(0, len(wires), 12):
        f.write("  wire " + ", ".join(wires[i:i + 12]) + ";\n")
    f.write("\n".join(lines) + "\n")
    for i, w in enumerate(prev):
        f.write("  BUF_X1 ob%d (.A(%s), .Y(out[%d]));\n" % (i, w, i))
    f.write("endmodule\n")
with open(out_sdc, "w") as f:
    f.write("""create_clock -name clk -period 4 [get_ports clk]
create_clock -name clk2 -period 6 [get_ports clk2]
set_propagated_clock [all_clocks]
set_clock_uncertainty 0.05 [all_clocks]
set_input_delay 0.5 -clock clk [get_ports in*]
set_input_delay 0.5 -clock clk [get_ports rst_n]
set_output_delay 0.5 -clock clk [get_ports out*]
set_load 0.01 [get_ports out*]
set_input_transition 0.1 [get_ports in*]
set_false_path -from [get_ports rst_n]
set_multicycle_path 2 -setup -from [get_clocks clk2] -to [get_clocks clk]
set_multicycle_path 1 -hold -from [get_clocks clk2] -to [get_clocks clk]
""")
