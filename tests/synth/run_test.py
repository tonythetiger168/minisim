#!/usr/bin/env python3
"""Synthesizes one design with msynth and checks the netlist against the RTL.

  run_test.py <msynth> <msim> <src dir> <work dir> <name> [lib]

<name>.sv is the design (top module <name>) and <name>_tb.sv its testbench,
which prints the outputs for random stimulus. The testbench is simulated
with msim twice, on the RTL and on the netlist with the library's cell
models that msynth generates, and both runs must print the same lines.
With [lib], that Liberty file is the target library instead of the
built-in generic one.
"""
import os
import subprocess
import sys

msynth, msim, src, work, name = sys.argv[1:6]
lib = sys.argv[6] if len(sys.argv) > 6 else None
os.makedirs(work, exist_ok=True)
rtl = os.path.join(src, name + ".sv")
tb = os.path.join(src, name + "_tb.sv")
net = os.path.join(work, name + "_net.v")
cells = os.path.join(work, name + "_cells.v")

script = []
if lib:
    script.append("set target_library {%s}" % lib)
script += [
    "analyze -format sverilog {%s}" % rtl,
    "elaborate %s" % name,
    "compile",
    "report_area",
    "report_timing",
    "write -format verilog -output {%s}" % net,
    "write_lib_models {%s}" % cells,
]
tcl = os.path.join(work, name + ".tcl")
with open(tcl, "w") as f:
    f.write("\n".join(script) + "\n")


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout)
        sys.exit("FAIL: %s exited with %d" % (" ".join(cmd), r.returncode))
    return r.stdout


for threads in ("1", "4"):
    out = run([msynth, "-threads", threads, "-f", tcl])
    with open(net) as f:
        text = f.read()
    if threads == "1":
        first = text
    elif text != first:
        sys.exit("FAIL: the netlist differs between -threads 1 and -threads 4")
print(out)
want = run([msim, rtl, tb, "-top", name + "_tb"])
got = run([msim, net, cells, tb, "-top", name + "_tb"])
wl, gl = want.splitlines(), got.splitlines()
if len(wl) < 100:
    print(want)
    sys.exit("FAIL: the RTL run printed only %d lines" % len(wl))
if wl != gl:
    for i, (a, b) in enumerate(zip(wl, gl)):
        if a != b:
            print("line %d: RTL '%s' netlist '%s'" % (i + 1, a, b))
            break
    sys.exit("FAIL: the netlist does not behave like the RTL (%d vs %d lines)" % (len(wl), len(gl)))
print("PASS: %d lines match" % len(wl))
