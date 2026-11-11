#!/usr/bin/env python3
"""Compare msta's slack at every endpoint with OpenSTA's.

  python3 tests/sta/gen_netlist.py 200000 big.v big.sdc 7
  msta -lib LIB big.v -sdc big.sdc -max_paths 1000000 -json msta.json
  LIB=LIB NET=big.v SDC=big.sdc OUT=osta sta -no_init -exit bench/sta/opensta.tcl
  python3 bench/sta/compare_opensta.py msta.json osta

OpenSTA is run as a separate program; nothing of it is part of minisim.
Exits 1 when an endpoint is missing on either side or differs by more
than the tolerance (ps, default 1).
"""
import json
import re
import sys


def opensta(path):
    d = {}
    pat = re.compile(r"\s*(\S+) \(.*?\)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+) \(")
    for line in open(path):
        m = pat.match(line)
        if m:
            e, s = m.group(1), float(m.group(4))
            if e not in d or s < d[e]:
                d[e] = s
    return d


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    mj = json.load(open(sys.argv[1]))
    tol = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    bad = False
    for typ in ("max", "min"):
        o = opensta(sys.argv[2] + "." + typ)
        m = {}
        for p in mj["paths"]:
            if p["type"] == typ:
                e, s = p["endpoint"], p["slack"]
                if e not in m or s < m[e]:
                    m[e] = s
        common = set(o) & set(m)
        diffs = sorted(abs(o[e] - m[e]) * 1000 for e in common)
        n = len(diffs)
        print(f"{typ}: {n} endpoints in both, {len(set(o) - set(m))} only in OpenSTA, {len(set(m) - set(o))} only in msta")
        if n:
            worst = max(common, key=lambda e: abs(o[e] - m[e]))
            print(f"  slack difference: max {diffs[-1]:.2f} ps, median {diffs[n // 2]:.3f} ps, "
                  f"p99 {diffs[int(n * .99)]:.2f} ps; worst at {worst}: OpenSTA {o[worst]} msta {m[worst]:.4f}")
        if set(o) != set(m) or (n and diffs[-1] > tol):
            bad = True
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
