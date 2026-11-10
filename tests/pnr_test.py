#!/usr/bin/env python3
"""mpnr self-test: place and route small netlists on tests/pnr/tiny.lef and
check what comes out.

    python3 tests/pnr_test.py MPNR SOURCE_DIR WORK_DIR [MPV]

With MPV (the physical verification tool) the GDS that mpnr writes is also
run through its DRC.
"""
import json
import os
import re
import struct
import subprocess
import sys

mpnr, src, work = sys.argv[1], sys.argv[2], sys.argv[3]
mpv = sys.argv[4] if len(sys.argv) > 4 else ""
os.makedirs(work, exist_ok=True)
lef = os.path.join(src, "tests/pnr/tiny.lef")
counter = os.path.join(src, "tests/pnr/counter.v")


def run(*args, rc=0):
    p = subprocess.run([mpnr, *args], capture_output=True, text=True)
    if p.returncode != rc:
        sys.exit(f"mpnr {' '.join(args)}: exit {p.returncode}, expected {rc}\n{p.stdout}{p.stderr}")
    return p.stdout + p.stderr


def need(text, pattern):
    if not re.search(pattern, text):
        sys.exit(f"missing /{pattern}/ in:\n{text}")


def out(name):
    return os.path.join(work, name)


# 1. The counter: a hierarchy, bus ports, an alias and constants. Everything
#    routes, and the independent check finds nothing.
log = run("-lef", lef, counter, "-def", out("counter.def"), "-netlist", out("counter.out.v"),
          "-report", out("counter.json"), "-svg", out("counter.svg"), "-threads", "1")
need(log, r"counter: 18 cells")          # 4 x 2 gates + mux + 3 bufs + 4 flops + 2 tie cells
need(log, r"0 nets unrouted")
need(log, r"0 open, 0 shorts, 0 spacing violations")
rep = json.load(open(out("counter.json")))
assert rep["unrouted"] == 0 and rep["opens"] == 0 and rep["shorts"] == 0 and rep["spacing"] == 0, rep
assert rep["cells"] == 18 and rep["fillers"] > 0 and rep["vias"] > 0 and rep["wire_length_um"] > 0, rep
d = open(out("counter.def")).read()
for want in ["DESIGN counter ;", "- ha0/u_s XOR2 + PLACED", "- mpnr_tielo TIE0", "- mpnr_tiehi TIE1", "- q[3] + NET q[3] + DIRECTION OUTPUT",
             "- VDD\n  ( * VDD )", "+ SHAPE FOLLOWPIN", "TRACKS X", "+ ROUTED m", "via12", "FILLER_0"]:
    assert want in d, want
# Every cell sits on a row and a site, inside the core, and no two overlap.
rows = {int(m.group(2)): (int(m.group(1)), int(m.group(3)), int(m.group(4)))
        for m in re.finditer(r"ROW \S+ core (\d+) (\d+) \S+ DO (\d+) BY 1 STEP (\d+) 0", d)}
macros = {m.group(1): round(float(m.group(2)) * 2000) for m in re.finditer(r"MACRO (\S+)\n(?:.*\n)*?\s+SIZE (\S+) BY", open(lef).read())}
spans = {}
for m in re.finditer(r"^- (\S+) (\S+) (?:\+ SOURCE DIST )?\+ PLACED \( (\d+) (\d+) \) (\S+) ;", d, re.M):
    name, mac, x, y = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
    assert y in rows, (name, y)
    x0, n, step = rows[y]
    assert (x - x0) % step == 0 and x0 <= x and x + macros[mac] <= x0 + n * step, name
    spans.setdefault(y, []).append((x, x + macros[mac], name))
for y, s in spans.items():
    s.sort()
    for a, b in zip(s, s[1:]):
        assert a[1] <= b[0], (a, b)
    # With fillers every row is full.
    assert sum(e - b for b, e, _ in s) == rows[y][1] * rows[y][2], y
# The netlist written back keeps the buses, has the tie and filler cells, and
# places and routes again to the same result.
v = open(out("counter.out.v")).read()
for want in ["output [3:0] q;", "TIE0 mpnr_tielo", "FILL", "XOR2 \\ha0/u_s"]:
    assert want in v, want
log2 = run("-lef", lef, out("counter.out.v"), "-def", out("counter2.def"))
need(log2, r"0 open, 0 shorts, 0 spacing violations")

# 2. The result does not depend on the thread count.
for t in ("2", "5"):
    run("-lef", lef, counter, "-def", out(f"counter_t{t}.def"), "-threads", t)
    assert open(out(f"counter_t{t}.def")).read() == d, f"-threads {t} gave a different DEF"

# 3. -check reads a DEF back and finds faults put into it.
need(run("-lef", lef, "-check", out("counter.def")), r"0 open, 0 shorts, 0 spacing")
nets = d[d.index("\nNETS "):]
blocks = re.findall(r"\n- (\S+)\n(.*?) ;\n", nets, re.S)
routed = [(n, b) for n, b in blocks if "+ ROUTED" in b]
# (a) Drop one wire of a net: an open.
name, body = next((n, b) for n, b in routed if re.search(r"\n    NEW m\d \( \d+ \d+ \) \( [\d*]+ [\d*]+ \)", b))
cut = re.sub(r"\n    NEW m\d \( \d+ \d+ \) \( [\d*]+ [\d*]+ \)", "", body, count=1)
open(out("open.def"), "w").write(d.replace(body, cut))
log = run("-lef", lef, "-check", out("open.def"), rc=1)
need(log, r"1 open, 0 shorts")
need(log, "open: net " + re.escape(name))
# (b) Give one net the wires of another as well: a short.
(n1, b1), (n2, b2) = routed[0], routed[1]
wires = b2[b2.index("+ ROUTED") + len("+ ROUTED"):]
open(out("short.def"), "w").write(d.replace(b1, b1 + "\n    NEW" + wires))
log = run("-lef", lef, "-check", out("short.def"), rc=1)
need(log, r"[1-9]\d* shorts")
need(log, "short on m")
# (c) A wire one manufacturing grid step from a track: a spacing violation with its neighbour.
m = re.search(r"NEW (m3) \( (\d+) (\d+) \) \( (\d+) \* \)", b1) or re.search(r"ROUTED (m3) \( (\d+) (\d+) \) \( (\d+) \* \)", b1)
if m:
    y = int(m.group(3)) + 200          # 0.10 um above: 0.03 um from the wire, less than the 0.07 um spacing
    other = b2 + f"\n    NEW m3 ( {m.group(2)} {y} ) ( {m.group(4)} * )"
    open(out("space.def"), "w").write(d.replace(b2, other))
    log = run("-lef", lef, "-check", out("space.def"), rc=1)
    need(log, r"[1-9]\d* spacing violations")

# 4. A floorplan DEF: the die, a fixed cell, a hand-placed port and a blockage are kept.
log = run("-lef", lef, counter, "-floorplan", os.path.join(src, "tests/pnr/floorplan.def"), "-def", out("fp.def"))
need(log, r"die 30\.40 x 16\.80 um")
need(log, r"0 open, 0 shorts, 0 spacing violations")
f = open(out("fp.def")).read()
assert "- r0 DFFR + FIXED ( 22800 11200 ) N ;" in f
assert re.search(r"- clk \+ NET clk \+ DIRECTION INPUT \+ USE SIGNAL\s+\+ LAYER m3 \( 0 -70 \) \( 1520 70 \)\s+\+ PLACED \( 0 16940 \) N ;", f), "clk port moved"
for m in re.finditer(r"^- (\S+) (\S+) (?:\+ SOURCE DIST )?\+ PLACED \( (\d+) (\d+) \) \S+ ;", f, re.M):
    x, y, w = int(m.group(3)), int(m.group(4)), macros[m.group(2)]
    assert not (x < 34200 and x + w > 30400 and 11200 <= y < 22400), f"{m.group(1)} is inside the blockage"

# 5. Options and errors.
log = run("-lef", lef, counter, "-util", "0.3", "-aspect", "0.5", "-max_layer", "4", "-def", out("opt.def"))
need(log, r"utilization (2\d|30)\.\d%")
need(log, r"0 open, 0 shorts, 0 spacing violations")
assert " m5 " not in open(out("opt.def")).read().split("\nNETS ")[1]
need(run("-lef", lef, counter, "-place_only", "-def", out("placed.def")), r"placement:")
assert "+ ROUTED" not in open(out("placed.def")).read().split("\nNETS ")[1]
open(out("bad.v"), "w").write("module bad (input a, output y);\n  NOSUCH u (.A(a), .Y(y));\nendmodule\n")
need(run("-lef", lef, out("bad.v"), rc=2), r"'NOSUCH' is neither a module in the netlist nor a macro in the LEF")
open(out("badpin.v"), "w").write("module bad (input a, output y);\n  INV u (.A(a), .Q(y));\nendmodule\n")
need(run("-lef", lef, out("badpin.v"), rc=2), r"macro INV has no pin 'Q'")
need(run(counter, rc=2), r"no LEF given")
need(run("-version"), r"mpnr \d+\.\d+")

# 6. A few thousand random gates (bench/gen_pnr.py): clean, and identical on 1 and 4 threads.
gen = subprocess.run([sys.executable, os.path.join(src, "bench/gen_pnr.py"), "3000", "7"], capture_output=True, text=True, check=True)
open(out("rand.v"), "w").write(gen.stdout)
log = run("-lef", lef, out("rand.v"), "-def", out("rand1.def"), "-threads", "1")
need(log, r"rand3000: 3000 cells")
need(log, r"0 nets unrouted")
need(log, r"0 open, 0 shorts, 0 spacing violations")
run("-lef", lef, out("rand.v"), "-def", out("rand4.def"), "-threads", "4")
assert open(out("rand1.def")).read() == open(out("rand4.def")).read(), "random design: -threads 4 differs from -threads 1"


# 6b. Power straps: on by default, they tie every rail of a supply together.
log = run("-lef", lef, counter, "-def", out("straps.def"))
need(log, r"power: 2 straps over the rails")
need(log, r"0 open, 0 shorts, 0 spacing violations")
assert "separate pieces" not in log, log
special = open(out("straps.def")).read().split("\nSPECIALNETS ")[1]
assert special.count("+ SHAPE STRIPE") == 2 and "via56" in special and "via12" in special, special
log = run("-lef", lef, counter, "-no_straps", "-def", out("nostraps.def"))
need(log, r"note: supply VDD is in 3 separate pieces")
assert "STRIPE" not in open(out("nostraps.def")).read()
need(run("-lef", lef, counter, "-max_layer", "3"), r"power: no straps: the top vertical layer, m2, is needed for pin access")
log = run("-lef", lef, counter, "-strap_layer", "4", "-strap_pitch", "3", "-strap_width", "0.2", "-def", out("straps4.def"))
need(log, r"power: 4 straps over the rails")
need(log, r"0 open, 0 shorts, 0 spacing violations")
assert open(out("straps4.def")).read().count("NEW m4 400 + SHAPE STRIPE") + open(out("straps4.def")).read().count("ROUTED m4 400 + SHAPE STRIPE") == 4


# 6c. The project's own cell library, lib/mini45: pins smaller than the LEF's
# via pad get a via made for them (written to the DEF's VIAS), tracks follow
# OFFSET 0, and short pieces of wire are made long enough for the AREA rule.
m45 = ["-lef", os.path.join(src, "lib/mini45/mini45_tech.lef"), "-lef", os.path.join(src, "lib/mini45/mini45.lef")]
gen = subprocess.run([sys.executable, os.path.join(src, "bench/gen_pnr.py"), "2000", "3", "--mini45"], capture_output=True, text=True, check=True)
open(out("m45.v"), "w").write(gen.stdout)
log = run(*m45, out("m45.v"), "-def", out("m45.def"), "-threads", "1")
need(log, r"0 nets unrouted")
need(log, r"0 open, 0 shorts, 0 spacing violations")
run(*m45, out("m45.v"), "-def", out("m45_4.def"), "-threads", "4")
text = open(out("m45.def")).read()
assert text == open(out("m45_4.def")).read(), "mini45: -threads 4 differs from -threads 1"
# With only the LEF's via1_0, whose metal1 pad is wider than the pins are apart,
# the pins get mpnr's own via.
tech = open(os.path.join(src, "lib/mini45/mini45_tech.lef")).read()
open(out("m45_tech0.lef"), "w").write(re.sub(r"VIA via1_1 .*?END via1_1\n", "", tech, flags=re.S))
m45_0 = ["-lef", out("m45_tech0.lef"), "-lef", m45[3]]
need(run(*m45_0, out("m45.v"), "-def", out("m45_0.def")), r"0 open, 0 shorts, 0 spacing violations")
need(open(out("m45_0.def")).read(), r"VIAS 1 ;\n- mpnr_via1_0_rl?\n")
need(text, r"TRACKS Y 280 DO \d+ STEP 280 LAYER metal1 ;")
need(run(*m45, "-check", out("m45.def")), r"0 open, 0 shorts, 0 spacing violations")
if mpv:
    # DRC with the rules of the LEF. The one rule left is the area of the
    # cells' own unconnected pins, which the library draws below its AREA.
    for lefs, name in ((m45, "m45.def"), (m45_0, "m45_0.def")):
        p = subprocess.run([mpv, "drc", *lefs, out(name), "-q"], capture_output=True, text=True)
        bad = [l for l in (p.stdout + p.stderr).splitlines() if re.match(r"\S+ +\d+ +\S", l) and not l.startswith("metal1.area")]
        assert not bad and "violations in" in p.stdout + p.stderr, "mini45: mpv drc finds\n" + p.stdout + p.stderr

if mpv:
    # DRC on the GDS with the rules made from the LEF: clean, and it sees the
    # cells' pins under the vias (a wrong orientation would leave vias bare).
    def pv(*args):
        p = subprocess.run([mpv, *args], capture_output=True, text=True)
        return p.stdout + p.stderr
    pv("drc", "-lef", lef, "-write-deck", out("deck.txt"), out("g.def"))
    log = pv("drc", "-deck", out("deck.txt"), out("g1.gds"))
    need(log, r"\n0 violations in 0 of \d+ rules")
    need(log, r"v1\.below +0 ")
    need(pv("drc", "-deck", out("deck.txt"), out("rand.gds")), r"\n0 violations in 0 of \d+ rules")
print("pnr_test: ok")
