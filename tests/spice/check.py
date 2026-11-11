#!/usr/bin/env python3
"""Check an mspice test netlist.

    check.py <mspice> <netlist.cir> [<ngspice>]

A netlist carries its own expectations in comment lines:

    *> name value rtol [mspice-only]   .measure `name` must equal value within rtol
                                       (with .step, name@2 is the second run's value)
    *> error "text"                    mspice must fail and print this text
    *> ngspice rtol                    tolerance against ngspice (default 0.01)
    *> no-ngspice                      do not compare this file against ngspice

Three checks:
  1. the values above, which are worked out by hand in each file;
  2. the same output, byte for byte, with -threads 1 and with every CPU;
  3. when ngspice is given, every .measure against ngspice. ngspice in batch
     mode runs one analysis per deck, so the file is split per analysis.
"""
import math
import os
import re
import subprocess
import sys
import tempfile

MEAS = re.compile(r"^(\w+)\s*=\s*([-+0-9.eE]+|failed)\b")


def measures(text):
    # With .step a name repeats: the k-th value is also `name@k`.
    out, seen = {}, {}
    for line in text.splitlines():
        m = MEAS.match(line.strip())
        if m:
            name = m.group(1).lower()
            v = math.nan if m.group(2) == "failed" else float(m.group(2))
            seen[name] = seen.get(name, 0) + 1
            out.setdefault(name, v)
            out[f"{name}@{seen[name]}"] = v
    return out


def close(a, b, rtol):
    return abs(a - b) <= rtol * max(abs(a), abs(b)) + 1e-15


def main():
    mspice, path = sys.argv[1], sys.argv[2]
    ngspice = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None
    lines = open(path).read().splitlines()
    expect, skip, want_error, ng_rtol, use_ng = [], set(), None, 0.01, True
    for l in lines:
        if not l.startswith("*>"):
            continue
        w = l[2:].split()
        if w[0] == "error":
            want_error = l.split('"')[1]
        elif w[0] == "ngspice":
            ng_rtol = float(w[1])
        elif w[0] == "no-ngspice":
            use_ng = False
        else:
            expect.append((w[0].lower(), float(w[1]), float(w[2])))
            if "mspice-only" in w:
                skip.add(w[0].lower())
    bad = []

    def run(*extra):
        return subprocess.run([mspice, path, "-q", *extra], capture_output=True, text=True)

    r = run()
    if want_error is not None:
        if r.returncode == 0 or want_error not in r.stderr:
            bad.append(f"expected failure with {want_error!r}, got exit {r.returncode}: {r.stderr.strip()}")
        return finish(path, bad)
    if r.returncode != 0:
        bad.append(f"mspice exited {r.returncode}: {r.stderr.strip()}")
        return finish(path, bad)
    got = measures(r.stdout)
    for name, val, rtol in expect:
        if name not in got:
            bad.append(f"{name}: not measured")
        elif not close(got[name], val, rtol):
            bad.append(f"{name}: mspice {got[name]:.6e}, expected {val:.6e} (rtol {rtol})")

    # Same answer at any thread count, including the CSV.
    with tempfile.TemporaryDirectory() as d:
        outs = []
        for t in ("1", "3", "all"):
            p = run("-threads", t, "-o", os.path.join(d, "t" + t))
            csv = "".join(open(os.path.join(d, f)).read() for f in sorted(os.listdir(d)) if f.startswith("t" + t + "."))
            outs.append((p.stdout, csv))
        if outs[0] != outs[1] or outs[0] != outs[2]:
            bad.append("output differs between -threads 1, 3 and all")

    if ngspice and use_ng:
        word = lambda l: (l.lower().split() or [""])[0]   # ".op" is not ".option"
        kinds = [k for k in ("op", "dc", "ac", "tran") if any(word(l) == "." + k for l in lines)]
        for kind in kinds:
            deck = []
            for l in lines:
                lo = l.lower()
                if lo.startswith(".print") or lo.startswith(".end") and not lo.startswith(".ends"):
                    continue
                if any(word(l) == "." + k for k in kinds if k != kind):
                    continue
                if lo.startswith(".meas") and lo.split()[1] != kind:
                    continue
                deck.append(l)
            deck += [".save all", ".end"]
            with tempfile.NamedTemporaryFile("w", suffix=".cir", delete=False, dir=os.path.dirname(os.path.abspath(path))) as f:
                f.write("\n".join(deck) + "\n")
            try:
                n = subprocess.run([ngspice, "-b", f.name], capture_output=True, text=True)
            finally:
                os.unlink(f.name)
            ref = measures(n.stdout)
            for name, val in ref.items():
                if name in skip or name not in got or "@" in name:
                    continue
                if not close(got[name], val, ng_rtol):
                    bad.append(f"{name}: mspice {got[name]:.6e}, ngspice {val:.6e} (rtol {ng_rtol})")
            for name in got:
                if "@" not in name and name not in ref and name not in skip and any(
                        l.lower().startswith(".meas") and l.lower().split()[1] == kind and l.lower().split()[2] == name for l in lines):
                    bad.append(f"{name}: ngspice gave no value")
    return finish(path, bad)


def finish(path, bad):
    for b in bad:
        print(f"{os.path.basename(path)}: {b}")
    if not bad:
        print(f"{os.path.basename(path)}: ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
