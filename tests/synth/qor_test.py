#!/usr/bin/env python3
"""The area steps of msynth show in the netlist of sweep.sv.

  qor_test.py <msynth> <srcdir> <workdir>

Registers nothing reads and registers stuck at their reset value are gone,
registers with a load enable sit on enable flops with no mux in front, and
the 16:1 read select uses 4:1 mux cells. (That the netlist still behaves as
the RTL is the msynth_sweep test.) A second design computes one function
two ways: functional reduction leaves no logic. hold.sv has registers
whose hold case is buried in an if/case tree.
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

# 'x is a don't-care: a select with an 'x arm is the other arm. A register
# without reset whose next state is a constant is that constant.
dc = os.path.join(work, "dc.sv")
open(dc, "w").write("""module dc (input logic clk, s, input logic [3:0] a, output logic [3:0] o, p);
    logic one;
    logic [3:0] t;
    always_comb
        case (s)
            1'b1: t = a;
            default: t = 'x;
        endcase
    always_ff @(posedge clk) one <= 1'b1;
    assign o = t;
    assign p = a & {4{one}};
endmodule
""")
r3 = subprocess.run([msynth, "-top", "dc", "-o", os.path.join(work, "dc_qor.v"), dc],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
check(r3.returncode == 0, "msynth failed on dc.sv")
check("removed 1 unloaded or constant register bits" in r3.stdout, "dc.sv: expected 1 register bit removed")
check("Mapped design 'dc': 0 cells" in r3.stdout, "dc.sv should need no cells:\n" + r3.stdout[:600])

# Hold conditions under the selects of an if/case tree (hold.sv): every
# register sits on an enable flop, and the pass can be switched off.
net4 = os.path.join(work, "hold_qor.v")
r4 = subprocess.run([msynth, "-top", "hold", "-o", net4, os.path.join(src, "hold.sv")],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
check(r4.returncode == 0, "msynth failed on hold.sv")
check("gathered the hold conditions of 24 register bits" in r4.stdout, "hold.sv: expected 24 register bits gathered")
text4 = open(net4).read() if os.path.exists(net4) else ""
hc = {}
for m in re.finditer(r"^\s*([A-Z][A-Z0-9]*_X\d+)\s+\S+\s*\(", text4, re.M):
    hc[m.group(1)] = hc.get(m.group(1), 0) + 1
check(hc.get("DFFE_X1", 0) == 24, "hold.sv: expected 24 enable flops, got %d" % hc.get("DFFE_X1", 0))
check(hc.get("DFF_X1", 0) == 0, "hold.sv: a plain flop is left")
# r and s choose between x and y: one select a bit, none for the hold
check(hc.get("MUX2_X1", 0) <= 16, "hold.sv: expected at most 16 MUX2 cells, got %d" % hc.get("MUX2_X1", 0))
m4 = re.search(r"Mapped design 'hold': \d+ cells, area ([0-9.]+)", r4.stdout)
tcl = os.path.join(work, "hold_off.tcl")
open(tcl, "w").write("set msynth_hold_enables false\nanalyze -format sverilog %s\nelaborate hold\ncompile\n" % os.path.join(src, "hold.sv"))
r5 = subprocess.run([msynth, "-f", tcl], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
m5 = re.search(r"Mapped design 'hold': \d+ cells, area ([0-9.]+)", r5.stdout)
check("gathered the hold" not in r5.stdout, "hold.sv: msynth_hold_enables false did not switch the pass off")
check(bool(m4 and m5) and float(m4.group(1)) < 0.95 * float(m5.group(1)), "hold.sv: the pass should save over 5% area")
if fails:
    print(out)
    print(cells)
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("PASS", cells)
