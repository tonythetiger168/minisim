#!/usr/bin/env python3
"""The msynth shell: constraints, reports, options and errors.

  shell_test.py <msynth> <src dir> <work dir>
"""
import os
import re
import subprocess
import sys

msynth, src, work = sys.argv[1:4]
os.makedirs(work, exist_ok=True)
fails = []


def run(script, args=()):
    path = os.path.join(work, "t.tcl")
    with open(path, "w") as f:
        f.write(script)
    r = subprocess.run([msynth, *args, "-f", path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return r.returncode, r.stdout


def check(name, cond, out=""):
    if not cond:
        fails.append(name)
        print("FAIL:", name)
        print(out)


head = "analyze -format sverilog {%s/counter.sv}\n" % src

# A loose clock is met; the report is in dc_shell's layout.
rc, out = run(head + """
elaborate counter
create_clock -name clk -period 5 [get_ports clk]
set_input_delay 0.5 -clock clk [remove_from_collection [all_inputs] [get_ports clk]]
set_output_delay 0.5 -clock clk [all_outputs]
set_load 0.01 [all_outputs]
compile
report_timing
report_qor
report_constraint -all_violators
report_reference
report_cell
write_sdc %s/c.sdc
""" % work)
check("loose clock", rc == 0 and "slack (MET)" in out and "Startpoint:" in out and "output external delay" in out, out)
check("no violators", "no violated constraints" in out, out)
check("qor", re.search(r"Sequential Cell Count:\s+8\b", out) is not None, out)
check("flop cell", re.search(r"DFFR\s+7\.0000\s+8\b", out) is not None, out)
sdc = open(os.path.join(work, "c.sdc")).read()
check("write_sdc", "create_clock -name clk -period 5" in sdc and "set_output_delay 0.5 -clock clk [get_ports {q[7]}]" in sdc, sdc)

# A clock that cannot be met: reported as violated, and the delay-driven
# mapping is tried (its critical path is no longer than the area one's).
rc, out = run(head + """
elaborate counter
compile
set a [sizeof_collection [get_cells]]
create_clock -period 0.2 [get_ports clk]
compile
report_timing
report_constraint -all_violators
""")
check("tight clock", rc == 0 and "slack (VIOLATED)" in out and "(VIOLATED)" in out.split("report_constraint")[-1], out)

# Parameters: a 16-bit counter has 16 flops; names accept N=>V and N=V.
rc, out = run(head + 'elaborate counter -parameters "W=>16"\ncompile\nputs "flops [llength [all_registers]]"\n', ["-q"])
check("parameters", rc == 0 and "flops 16" in out, out)

# Redirection, Tcl control flow, and a library where cells are excluded.
rpt = os.path.join(work, "area.rpt")
rc, out = run(head + """
elaborate counter
set_dont_use {XOR2 XNOR2 MUX2}
compile
report_area > %s
redirect -append %s {report_reference}
foreach p [get_ports q*] { puts "port $p" }
if {[file exists %s]} { echo "report written" }
""" % (rpt, rpt, rpt), ["-q"])
text = open(rpt).read() if os.path.exists(rpt) else ""
check("redirect", rc == 0 and "Total cell area" in text and "Reference" in text and "report written" in out and "port q" in out, out + text)
check("set_dont_use", "XOR2" not in text and "MUX2" not in text and "NAND2" in text, text)

# Errors: a latch, a missing module, an unknown command; the exit code is 1.
rc, out = run("analyze {%s/latch.sv}\nelaborate latch\n" % src)
check("latch", rc == 1 and "latches are not supported" in out and "latch.sv:3" in out, out)
rc, out = run(head + "elaborate nothere\n")
check("missing top", rc == 1, out)
rc, out = run(head + "elaborate counter\nfrobnicate\ncompile\n")
check("unknown command", rc == 1 and "frobnicate" in out and "Mapped design" in out, out)
rc, out = run(head + "report_area\n")
check("no design", rc == 1 and "compile first" in out, out)

# One step from the command line.
net = os.path.join(work, "one.v")
r = subprocess.run([msynth, os.path.join(src, "counter.sv"), "-top", "counter", "-period", "2", "-o", net, "-threads", "2"],
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
check("one step", r.returncode == 0 and "slack (MET)" in r.stdout and "module counter" in open(net).read(), r.stdout)

if fails:
    sys.exit("%d failed: %s" % (len(fails), ", ".join(fails)))
print("PASS")
