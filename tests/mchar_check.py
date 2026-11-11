#!/usr/bin/env python3
"""Checks mchar (docs/CHAR.md).

    mchar_check.py <mchar> <source dir> <work dir>

1. A cell with a known answer (tests/char/rc.*): delay R C ln 2, transition
   R C ln 4.
2. A few mini45 cells characterized again from the library in the tree: the
   same file for 1 thread and for all, and the numbers the tree has (so the
   checked-in tables are what mchar measures on the checked-in netlists).
3. The physics of the result: more load and a slower input are slower.
"""
import math
import os
import re
import subprocess
import sys

mchar, src, work = sys.argv[1:4]
os.makedirs(work, exist_ok=True)
errors = []

def run(args, ok=(0,)):
    p = subprocess.run([mchar] + args, capture_output=True, text=True)
    if p.returncode not in ok:
        errors.append("mchar %s: exit %d\n%s%s" % (" ".join(args), p.returncode, p.stdout, p.stderr))
    return p

def cells(path):
    """cell -> list of (table name, [numbers]) in file order; plus pin capacitances and pulse widths."""
    text = re.sub(r"/\*.*?\*/", "", open(path).read(), flags=re.S)
    out = {}
    cur = None
    pat = re.compile(r"cell \((\w+)\)|(\w+) \(\w+\) \{[^{}]*?values \((.*?)\);|capacitance : ([-\d.e]+);|(min_pulse_width_\w+) : ([-\d.e]+);", re.S)
    for m in pat.finditer(text):
        if m.group(1):
            cur = out.setdefault(m.group(1), [])
        elif cur is None:
            continue
        elif m.group(2):
            cur.append((m.group(2), [float(x) for x in re.findall(r"-?\d+\.?\d*(?:e-?\d+)?", m.group(3))]))
        elif m.group(4):
            cur.append(("capacitance", [float(m.group(4))]))
        else:
            cur.append((m.group(5), [float(m.group(6))]))
    return out

# 1. the RC inverter
rc = os.path.join(work, "rc.lib")
p = run(["-lib", os.path.join(src, "tests/char/rc.lib"), "-spice", os.path.join(src, "tests/char/rc.sp"),
         "-keep", "caps", "-keep", "leakage", "-o", rc])
if "1 cell without a netlist" not in p.stdout:
    errors.append("rc: expected a note about the cell without a netlist:\n" + p.stdout)
if os.path.exists(rc):
    t = dict(cells(rc)["RCINV"])
    for name, k in (("cell_rise", math.log(2)), ("cell_fall", math.log(2)),
                    ("rise_transition", math.log(4)), ("fall_transition", math.log(4))):
        for i, load in enumerate([10, 20, 10, 20]):
            want = 10e3 * load * 1e-15 * k * 1e9          # ns
            got = t[name][i]
            if abs(got - want) > 0.003 * want:
                errors.append("rc %s[%d]: %.5f ns, expected %.5f" % (name, i, got, want))

# 2. mini45 again, from the tree
lib = os.path.join(src, "lib/mini45")
names = ["INV_X1", "NAND2_X2", "DFFR_X1", "TBUF_X1"]
common = ["-lib", os.path.join(lib, "mini45_tt.lib"), "-spice", os.path.join(lib, "mini45.sp"),
          "-models", os.path.join(lib, "mini45_models_tt.sp"), "-keep", "leakage", "-q"]
for n in names:
    common += ["-cell", n]
one, many = os.path.join(work, "one.lib"), os.path.join(work, "many.lib")
run(common + ["-threads", "1", "-o", one])
run(common + ["-o", many])
if os.path.exists(one) and os.path.exists(many):
    if open(one).read() != open(many).read():
        errors.append("mini45: the output differs between -threads 1 and the default")
    new, old = cells(one), cells(os.path.join(lib, "mini45_tt.lib"))
    for n in names:
        if len(new[n]) != len(old[n]):
            errors.append("%s: %d tables, the tree has %d" % (n, len(new[n]), len(old[n])))
            continue
        for (tn, a), (_, b) in zip(new[n], old[n]):
            for x, y in zip(a, b):
                # 2% or 1 ps: compilers and math libraries round differently
                if abs(x - y) > max(0.02 * abs(y), 0.001):
                    errors.append("%s %s: %.5f, the tree has %.5f (run lib/mini45/characterize.sh?)" % (n, tn, x, y))
                    break
    # 3. shape: 7x7 delay tables rise along both axes
    for n in names:
        for tn, v in new[n]:
            if tn in ("cell_rise", "cell_fall") and len(v) == 49:
                for i in range(7):
                    for j in range(7):
                        if j and v[i * 7 + j] <= v[i * 7 + j - 1]:
                            errors.append("%s %s: not slower with more load at [%d][%d]" % (n, tn, i, j))
                        if i and v[i * 7 + j] <= v[(i - 1) * 7 + j]:
                            errors.append("%s %s: not slower with a slower input at [%d][%d]" % (n, tn, i, j))

# Bad input is an error, not a crash.
p = subprocess.run([mchar, "-lib", "nope.lib", "-spice", os.path.join(lib, "mini45.sp"), "-o", os.path.join(work, "x.lib")],
                   capture_output=True, text=True)
if p.returncode != 2 or "nope.lib" not in p.stderr:
    errors.append("a missing library should exit 2 with a message, got %d: %s" % (p.returncode, p.stderr))

for e in errors[:40]:
    print("FAIL:", e)
print("mchar: %d errors" % len(errors))
sys.exit(1 if errors else 0)
