#!/usr/bin/env python3
"""Check mspice's raw files.

    check.py <mspice> <netlist.cir>

Runs the netlist with -raw (binary) and with -raw-ascii, reads both files
back and checks that they hold the same plots, that the binary values are
the ASCII values, and a few numbers of the RC circuit in rc.cir.
"""
import math
import struct
import subprocess
import sys
import tempfile


def read(path):
    data = open(path, "rb").read()
    plots, pos = [], 0
    while pos < len(data):
        head = {}
        names = []
        while True:
            end = data.index(b"\n", pos)
            line = data[pos:end].decode()
            pos = end + 1
            if line in ("Binary:", "Values:"):
                break
            if line == "Variables:":
                for _ in range(int(head["No. Variables"])):
                    end = data.index(b"\n", pos)
                    names.append(data[pos:end].decode().split("\t")[2])
                    pos = end + 1
                continue
            key, _, value = line.partition(":")
            head[key] = value.strip()
        nv, npts = int(head["No. Variables"]), int(head["No. Points"])
        rows = []
        if line == "Binary:":
            for _ in range(npts):
                rows.append(list(struct.unpack("<%dd" % nv, data[pos:pos + 8 * nv])))
                pos += 8 * nv
        else:
            for i in range(npts):
                row = []
                for v in range(nv):
                    end = data.index(b"\n", pos)
                    f = data[pos:end].decode().split()
                    if v == 0:
                        assert int(f[0]) == i, "point number"
                    row.append(float(f[-1]))
                    pos = end + 1
                pos = data.index(b"\n", pos) + 1   # the blank line after a point
                rows.append(row)
        plots.append((head["Plotname"], head["Flags"], names, rows))
    return plots


def main():
    mspice, deck = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([mspice, deck, "-q", "-raw", d + "/b.raw"], check=True)
        subprocess.run([mspice, deck, "-q", "-raw-ascii", d + "/a.raw"], check=True)
        b, a = read(d + "/b.raw"), read(d + "/a.raw")
    bad = []
    if [p[:3] for p in a] != [p[:3] for p in b]:
        bad.append("the binary and the ASCII file hold different plots")
    for pa, pb in zip(a, b):
        if pa[3] != pb[3]:
            bad.append(pa[0] + ": binary and ASCII values differ")
    names = [p[0] for p in b]
    if names != ["Operating Point", "Transient Analysis", "AC Analysis"]:
        bad.append("plots: %r" % names)
    else:
        op, tran, ac = b
        if op[2] != ["v(in)", "v(out)", "i(v1)"] or len(op[3]) != 1:
            bad.append("operating point: %r" % (op[2],))
        if tran[2][0] != "time" or abs(tran[3][-1][0] - 2e-3) > 1e-12:
            bad.append("transient: no time axis to 2 ms")
        else:
            out = tran[2].index("v(out)")
            want = 1 - math.exp(-2)   # two time constants
            if abs(tran[3][-1][out] - want) > 2e-3:
                bad.append("transient: v(out) ends at %g, not %g" % (tran[3][-1][out], want))
        if ac[2] != ["frequency", "vdb(out)", "vp(out)"]:
            bad.append("ac: %r" % (ac[2],))
        else:
            # At 10 Hz, well below the 159 Hz corner: 0 dB within 0.02.
            if abs(ac[3][0][0] - 10) > 1e-9 or abs(ac[3][0][1]) > 0.02:
                bad.append("ac: first point %r" % (ac[3][0],))
    for m in bad:
        print("FAIL:", m)
    sys.exit(1 if bad else 0)


main()
