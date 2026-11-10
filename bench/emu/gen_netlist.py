#!/usr/bin/env python3
"""A large gate netlist over tests/sta/sta_test.lib, for timing memu.

  gen_netlist.py BLOCKS [SEED] > big.gv

Each block is a 32-bit register stage with about 150 gates of random logic in
front of it. A block reads mostly its own registers and those of its
neighbours, the way the modules of a real design do, so there is a good
partition to find. One clock, one reset, 32 inputs, 32 outputs.
"""
import random
import sys

blocks = int(sys.argv[1]) if len(sys.argv) > 1 else 100
rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
W = 32
GATES = ["NAND2_X1", "NOR2_X1", "AND2_X1", "OR2_X1", "XOR2_X1"]
out = []
w = out.append
w("module big (clk, rst_n, din, dout);")
w("  input clk, rst_n;")
w(f"  input [{W-1}:0] din;")
w(f"  output [{W-1}:0] dout;")
for b in range(blocks):
    w(f"  wire [{W-1}:0] r{b};")
n = 0
for b in range(blocks):
    # What this block may read: itself, two neighbours, now and then a far block.
    def src():
        x = rnd.random()
        if b == 0 and x < 0.3:
            return f"din[{rnd.randrange(W)}]"
        if x < 0.6:
            o = b
        elif x < 0.995:
            o = (b + rnd.choice([-2, -1, 1, 2])) % blocks
        else:
            o = rnd.randrange(blocks)
        return f"r{o}[{rnd.randrange(W)}]"
    local = []
    for g in range(150):
        a = src() if not local or rnd.random() < 0.5 else rnd.choice(local[-20:])
        c = src() if not local or rnd.random() < 0.5 else rnd.choice(local[-20:])
        y = f"g{n}"
        w(f"  wire {y};")
        if rnd.random() < 0.1:
            s = src()
            w(f"  MUX2_X1 u{n} (.A({a}), .B({c}), .S({s}), .Y({y}));")
        else:
            w(f"  {rnd.choice(GATES)} u{n} (.A({a}), .B({c}), .Y({y}));")
        local.append(y)
        n += 1
    for i in range(W):
        d = local[-1 - i]
        if i % 4 == 0:
            w(f"  DFFR_X1 f{b}_{i} (.CK(clk), .RN(rst_n), .D({d}), .Q(r{b}[{i}]));")
        else:
            w(f"  DFF_X1 f{b}_{i} (.CK(clk), .D({d}), .Q(r{b}[{i}]));")
for i in range(W):
    w(f"  BUF_X1 o{i} (.A(r{blocks-1}[{i}]), .Y(dout[{i}]));")
w("endmodule")
sys.stdout.write("\n".join(out) + "\n")
