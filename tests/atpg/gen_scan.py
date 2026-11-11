#!/usr/bin/env python3
"""Random scan netlist of generic cells: gen.py NAME NGATES NFLOPS NPI NPO NCHAINS SEED [muxstyle]"""
import random, sys
name, ng, nf, npi, npo, nch, seed = sys.argv[1], *map(int, sys.argv[2:8])
mux = len(sys.argv) > 8
r = random.Random(seed)
cells = [("INV",1),("BUF",1),("AND2",2),("OR2",2),("NAND2",2),("NOR2",2),("XOR2",2),("XNOR2",2),
         ("AND3",3),("NAND3",3),("OR3",3),("NOR4",4),("MUX2",3),("AOI21",3),("OAI21",3),("AOI22",4),("OAI22",4)]
out = []
srcs = [f"pi[{i}]" for i in range(npi)] + [f"q{i}" for i in range(nf)]
wires = [f"q{i}" for i in range(nf)]
for g in range(ng):
    c, n = r.choice(cells)
    ins = [r.choice(srcs[-60:] if r.random() < 0.7 else srcs) for _ in range(n)]
    pins = ["A","B","S"] if c == "MUX2" else ["A","B","C","D"][:n]
    w = f"n{g}"; wires.append(w)
    out.append(f"  {c} U{g} (" + ", ".join(f".{p}({i})" for p, i in zip(pins, ins)) + f", .Y({w}));")
    srcs.append(w)
chains = [[] for _ in range(nch)]
for i in range(nf): chains[i % nch].append(i)
for ci, ch in enumerate(chains):
    prev = f"scan_in[{ci}]"
    for i in ch:
        d = r.choice(srcs[npi+nf:]) if ng else r.choice(srcs)
        rst = i % 3 == 0
        if mux:
            wires.append(f"sd{i}")
            out.append(f"  MUX2 SM{i} (.A({d}), .B({prev}), .S(scan_en), .Y(sd{i}));")
            out.append(f"  {'DFFR' if rst else 'DFF'} R{i} (.D(sd{i}), .CK(clk), " + (".RN(rst_n), " if rst else "") + f".Q(q{i}));")
        else:
            out.append(f"  {'SDFFR' if rst else 'SDFF'} R{i} (.D({d}), .SI({prev}), .SE(scan_en), .CK(clk), " + (".RN(rst_n), " if rst else "") + f".Q(q{i}));")
        prev = f"q{i}"
    out.append(f"  BUF SO{ci} (.A({prev}), .Y(scan_out[{ci}]));")
for i in range(npo):
    out.append(f"  BUF PO{i} (.A({r.choice(srcs[-max(npo*3,10):])}), .Y(po[{i}]));")
print(f"module {name} (input clk, input rst_n, input scan_en, input [{nch-1}:0] scan_in, input [{npi-1}:0] pi, output [{nch-1}:0] scan_out, output [{npo-1}:0] po);")
for w in wires: print(f"  wire {w};")
print("\n".join(out)); print("endmodule")
