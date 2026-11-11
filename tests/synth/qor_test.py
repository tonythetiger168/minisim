#!/usr/bin/env python3
"""The area steps of msynth show in the netlist of sweep.sv.

  qor_test.py <msynth> <srcdir> <workdir>

Registers nothing reads and registers stuck at their reset value are gone,
registers with a load enable sit on enable flops with no mux in front, and
the 16:1 read select uses 4:1 mux cells. (That the netlist still behaves as
the RTL is the msynth_sweep test.) A second design computes one function
two ways: functional reduction leaves no logic.
"""
import os
import re
import subprocess
import sys

msynth, src, work = sys.argv[1:4]
os.makedirs(work, exist_ok=True)
net = os.path.join(work, "sweep_qor.v")
r = subprocess.run([msynth, "-top", "sweep", "-o", net, os.path.join(src, "sweep.sv")],
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
out = r.stdout
fails = []


def check(ok, what):
    if not ok:
        fails.append(what)


check(r.returncode == 0, "msynth failed")
# unread (8) + unread2 (8) + stuck (4) + hold1 (1)
check("removed 21 unloaded or constant register bits" in out, "expected 21 register bits removed")
text = open(net).read() if os.path.exists(net) else ""
cells = {}
for m in re.finditer(r"^\s*([A-Z][A-Z0-9]*_X\d+)\s+\S+\s*\(", text, re.M):
    cells[m.group(1)] = cells.get(m.group(1), 0) + 1
# r (8) and bank (16 x 8): every one loads under an enable
check(cells.get("DFFE_X1", 0) == 136, "expected 136 enable flops, got %d" % cells.get("DFFE_X1", 0))
check(sum(n for c, n in cells.items() if c.startswith("DFF") and not c.startswith("DFFE")) == 0, "a plain flop is left")
# 16:1 x 8 bits = 5 four-input muxes a bit
check(cells.get("MUX4_X1", 0) == 40, "expected 40 MUX4 cells, got %d" % cells.get("MUX4_X1", 0))
check(cells.get("MUX2_X1", 0) == 0, "expected no MUX2 cells, got %d" % cells.get("MUX2_X1", 0))

# Functional reduction: the same function built two ways is found by
# simulation and proven by SAT, here down to a constant output.
dup = os.path.join(work, "dup.sv")
open(dup, "w").write("""module dup (input logic [7:0] a, b, output logic [7:0] o, output logic same);
    logic [7:0] p, q;
    assign p = a ^ b;
    assign q = (a | b) & ~(a & b);
    assign o = p ^ q;
    assign same = (a < b) == !(b <= a);
endmodule
""")
net2 = os.path.join(work, "dup_qor.v")
r2 = subprocess.run([msynth, "-top", "dup", "-o", net2, dup], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
check(r2.returncode == 0, "msynth failed on dup.sv")
check("Information: merged" in r2.stdout, "no nodes merged in dup.sv")
check("Mapped design 'dup': 0 cells" in r2.stdout, "dup.sv should need no cells:\n" + r2.stdout[:600])
if fails:
    print(out)
    print(cells)
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("PASS", cells)
