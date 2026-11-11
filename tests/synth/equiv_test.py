#!/usr/bin/env python3
"""msynth's netlist is proven equivalent to its RTL by mequiv.

  equiv_test.py <msynth> <mequiv> <srcdir> <workdir> <name> [lib]

<name>.sv is synthesized (top module <name>) onto the default library, or
onto [lib], and mequiv must prove the netlist equivalent to the RTL. Then
one gate of the netlist is swapped for another of the same pins, and mequiv
must report the difference: a proof that cannot fail proves nothing.
"""
import os
import re
import subprocess
import sys

msynth, mequiv, src, work, name = sys.argv[1:6]
lib = sys.argv[6] if len(sys.argv) > 6 else os.path.join(src, "..", "..", "lib", "mini45", "mini45_tt.lib")
tag = name + ("_" + os.path.basename(lib).split(".")[0] if len(sys.argv) > 6 else "")
os.makedirs(work, exist_ok=True)
rtl = os.path.join(src, name + ".sv")
net = os.path.join(work, tag + "_eq.v")
bad = os.path.join(work, tag + "_eq_bad.v")


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return r.returncode, r.stdout


rc, out = run([msynth, "-q", "-top", name, "-lib", lib, "-o", net, rtl])
if rc != 0:
    sys.exit("FAIL: msynth exited with %d\n%s" % (rc, out))


def equiv(impl):
    return run([mequiv, "-ref", rtl, "-impl", impl, "-lib", lib, "-top", name])


rc, out = equiv(net)
if rc != 0:
    sys.exit("FAIL: mequiv does not prove %s equivalent to its RTL\n%s" % (net, out))

# Swap the first two-input gate for its dual (same pins): NAND <-> NOR.
text = open(net).read()
swaps = [("NAND2_X1", "NOR2_X1"), ("NOR2_X1", "NAND2_X1"), ("ND2", "NR2")]
for a, b in swaps:
    m = re.search(r"^  %s " % re.escape(a), text, re.M)
    if m:
        open(bad, "w").write(text[:m.start()] + "  " + b + " " + text[m.end():])
        rc, out = equiv(bad)
        if rc == 0:
            sys.exit("FAIL: mequiv proves a netlist with %s swapped for %s\n%s" % (a, b, out))
        break
print("PASS: %s" % tag)
