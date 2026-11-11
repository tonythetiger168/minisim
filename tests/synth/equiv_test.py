#!/usr/bin/env python3
"""msynth's netlist is proven equivalent to its RTL by mequiv.

  equiv_test.py <msynth> <mequiv> <srcdir> <workdir> <name> [lib]

<name>.sv is synthesized (top module <name>) onto the default library, or
onto [lib], and mequiv must prove the netlist equivalent to the RTL. Then
a gate of the netlist is swapped for another of the same pins, and mequiv
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

# Swap a two-input gate for its dual (same pins): NAND <-> NOR. One gate can
# sit where its value never reaches an output, so up to eight are tried, one
# at a time; mequiv must report at least one of them.
text = open(net).read()
sites = [(m.start(), m.end(), b) for a, b in [("NAND2_X1", "NOR2_X1"), ("NOR2_X1", "NAND2_X1"), ("ND2", "NR2")]
         for m in re.finditer(r"^  %s " % re.escape(a), text, re.M)]
sites.sort()
found = False
for start, end, b in sites[:8]:
    open(bad, "w").write(text[:start] + "  " + b + " " + text[end:])
    rc, out = equiv(bad)
    if rc != 0:
        found = True
        break
if sites and not found:
    sys.exit("FAIL: mequiv proves netlists with a gate swapped for its dual (%d tried)\n%s" % (min(8, len(sites)), out))
print("PASS: %s" % tag)
