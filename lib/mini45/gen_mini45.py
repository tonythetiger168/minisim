#!/usr/bin/env python3
"""Generates the mini45 generic standard cell library.

mini45 is a made-up 45 nm-class library for testing the tool line: synthesis
maps to it, timing and power read it, place and route uses its LEF, and the
gate-level netlists simulate against its Verilog models. Nothing in it comes
from a foundry or from another library. Do not use it for a real chip.

This script writes everything but the Liberty files:

    python3 gen_mini45.py            # rewrite the files next to this script
    python3 gen_mini45.py --check    # exit 1 if a file on disk is out of date

    mini45.v        Verilog models (functional, specify paths for SDF)
    mini45_tech.lef technology LEF (site, 6 metal layers, vias)
    mini45.lef      cell LEF (macros with pin shapes)
    mini45_tb.sv    self-checking bench: Verilog models against the functions
    mini45.sp       transistor netlists, one .subckt per cell
    mini45_models_{tt,ss,ff}.sp   level-1 MOSFET models per corner

The Liberty files (mini45_tt.lib 1.10 V 25 C, mini45_ss.lib 0.95 V 125 C,
mini45_ff.lib 1.25 V -40 C) are measured: characterize.sh runs mchar on the
transistor netlists. What to measure comes from templates this script
writes with

    python3 gen_mini45.py --templates DIR

The templates carry numbers from a small analytic model (logical effort for
delay, simple energy and leakage rules). mchar replaces them, except the
leakage values, the tristate enable and disable arcs and the minimum pulse
widths, which stay analytic (docs/MINI45.md says why).
"""
import itertools
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# ------------------------------------------------------------ technology
SITE_W = 0.19            # um, also the metal2 pitch
ROW_H = 1.40             # um, 10 metal1 tracks
M1_PITCH = 0.14
C_INV = 0.60             # fF, input capacitance of INV_X1
C_PAR = 0.50             # fF, drain capacitance of INV_X1's output
R_UP = 9.0               # kOhm, INV_X1 pull-up
R_DN = 7.0               # kOhm, INV_X1 pull-down
SLEWS = [0.005, 0.01, 0.02, 0.04, 0.08, 0.16, 0.32]          # ns
LOADS_X1 = [0.4, 0.8, 1.6, 3.2, 6.4, 12.8, 25.6]             # fF, scaled by drive
VDD_TT = 1.10

CORNERS = {
    # name: (process, voltage, temperature, delay factor, leakage factor)
    "tt": (1.0, 1.10, 25, 1.00, 1.0),
    "ss": (1.2, 0.95, 125, 1.42, 7.5),
    "ff": (0.8, 1.25, -40, 0.74, 0.35),
}

# ------------------------------------------------------------ cell list
class Cell:
    def __init__(self, name, kind, drive, **kw):
        self.name = name          # INV_X1
        self.kind = kind          # INV
        self.drive = drive
        self.inputs = kw.get("inputs", [])
        self.outputs = kw.get("outputs", {})      # name -> liberty function
        self.vlog = kw.get("vlog", {})            # name -> Verilog expression (default: from the function)
        self.g = kw.get("g", {})                  # logical effort per input
        self.p = kw.get("p", 1.0)                 # parasitic delay, in inverter units
        self.stages = kw.get("stages", 1)         # 2: an inverting core and an output inverter
        self.sense = kw.get("sense", {})          # (input, output) -> positive_unate | negative_unate | non_unate
        self.sites = kw.get("sites", 2)
        self.seq = kw.get("seq")                  # dict for flops, latches, clock gates
        self.tri = kw.get("tri")                  # three_state expression
        self.cls = kw.get("cls", "CORE")
        self.dont_use = kw.get("dont_use", False)
        self.leak = kw.get("leak", 1.0)           # leakage in INV_X1 units, before the drive factor
        self.footprint = kind

CELLS = []

def add(kind, drives, **kw):
    for d in drives:
        k = dict(kw)
        base = k.pop("sites", 2)
        grow = k.pop("grow", 1)
        k["sites"] = base + grow * (int(math.ceil(d / 1.0)) - 1 if d > 1 else 0)
        CELLS.append(Cell("%s_X%d" % (kind, d), kind, d, **k))

def unate(inputs, out, s):
    return {(i, out): s for i in inputs}

NEG, POS, NON = "negative_unate", "positive_unate", "non_unate"

add("INV", [1, 2, 4, 8], inputs=["A"], outputs={"Y": "!A"}, g={"A": 1.0}, p=1.0,
    sense=unate("A", "Y", NEG), sites=2, leak=1.0)
add("BUF", [1, 2, 4, 8], inputs=["A"], outputs={"Y": "A"}, g={"A": 1.0}, p=1.0, stages=2,
    sense=unate("A", "Y", POS), sites=3, leak=1.6)
add("CLKBUF", [1, 2, 4], inputs=["A"], outputs={"Y": "A"}, g={"A": 1.1}, p=1.1, stages=2,
    sense=unate("A", "Y", POS), sites=3, leak=1.7)
add("CLKINV", [1, 2, 4], inputs=["A"], outputs={"Y": "!A"}, g={"A": 1.1}, p=1.1,
    sense=unate("A", "Y", NEG), sites=2, leak=1.1)

def nand(n):
    ins = ["A", "B", "C", "D"][:n]
    add("NAND%d" % n, [1, 2, 4] if n == 2 else [1, 2], inputs=ins,
        outputs={"Y": "!(" + "*".join(ins) + ")"}, g={i: (n + 2) / 3.0 for i in ins}, p=float(n),
        sense=unate(ins, "Y", NEG), sites=n + 1, leak=0.5 + 0.35 * n)
    add("AND%d" % n, [1, 2, 4] if n == 2 else [1, 2], inputs=ins,
        outputs={"Y": "*".join(ins)}, g={i: (n + 2) / 3.0 for i in ins}, p=float(n), stages=2,
        sense=unate(ins, "Y", POS), sites=n + 2, leak=1.3 + 0.35 * n)

def nor(n):
    ins = ["A", "B", "C", "D"][:n]
    add("NOR%d" % n, [1, 2, 4] if n == 2 else [1, 2], inputs=ins,
        outputs={"Y": "!(" + "+".join(ins) + ")"}, g={i: (2 * n + 1) / 3.0 for i in ins}, p=float(n),
        sense=unate(ins, "Y", NEG), sites=n + 1, leak=0.6 + 0.4 * n)
    add("OR%d" % n, [1, 2, 4] if n == 2 else [1, 2], inputs=ins,
        outputs={"Y": "+".join(ins)}, g={i: (2 * n + 1) / 3.0 for i in ins}, p=float(n), stages=2,
        sense=unate(ins, "Y", POS), sites=n + 2, leak=1.4 + 0.4 * n)

for n in (2, 3, 4):
    nand(n)
    nor(n)

add("XOR2", [1, 2], inputs=["A", "B"], outputs={"Y": "A^B"}, g={"A": 2.2, "B": 2.2}, p=3.0, stages=2,
    sense=unate("AB", "Y", NON), sites=6, leak=2.6)
add("XNOR2", [1, 2], inputs=["A", "B"], outputs={"Y": "!(A^B)"}, g={"A": 2.2, "B": 2.2}, p=3.0, stages=2,
    sense=unate("AB", "Y", NON), sites=6, leak=2.6)
add("AOI21", [1, 2], inputs=["A1", "A2", "B"], outputs={"Y": "!((A1*A2)+B)"},
    g={"A1": 2.0, "A2": 2.0, "B": 5 / 3.0}, p=7 / 3.0,
    sense=unate(["A1", "A2", "B"], "Y", NEG), sites=4, leak=1.7)
add("AOI22", [1, 2], inputs=["A1", "A2", "B1", "B2"], outputs={"Y": "!((A1*A2)+(B1*B2))"},
    g={i: 2.0 for i in ("A1", "A2", "B1", "B2")}, p=4.0,
    sense=unate(["A1", "A2", "B1", "B2"], "Y", NEG), sites=5, leak=2.1)
add("AOI211", [1], inputs=["A1", "A2", "B", "C"], outputs={"Y": "!((A1*A2)+B+C)"},
    g={"A1": 8 / 3.0, "A2": 8 / 3.0, "B": 7 / 3.0, "C": 7 / 3.0}, p=10 / 3.0,
    sense=unate(["A1", "A2", "B", "C"], "Y", NEG), sites=5, leak=2.2)
add("OAI21", [1, 2], inputs=["A1", "A2", "B"], outputs={"Y": "!((A1+A2)*B)"},
    g={"A1": 2.0, "A2": 2.0, "B": 4 / 3.0}, p=7 / 3.0,
    sense=unate(["A1", "A2", "B"], "Y", NEG), sites=4, leak=1.6)
add("OAI22", [1, 2], inputs=["A1", "A2", "B1", "B2"], outputs={"Y": "!((A1+A2)*(B1+B2))"},
    g={i: 2.0 for i in ("A1", "A2", "B1", "B2")}, p=4.0,
    sense=unate(["A1", "A2", "B1", "B2"], "Y", NEG), sites=5, leak=2.0)
add("OAI211", [1], inputs=["A1", "A2", "B", "C"], outputs={"Y": "!((A1+A2)*B*C)"},
    g={"A1": 8 / 3.0, "A2": 8 / 3.0, "B": 5 / 3.0, "C": 5 / 3.0}, p=10 / 3.0,
    sense=unate(["A1", "A2", "B", "C"], "Y", NEG), sites=5, leak=2.1)
add("MUX2", [1, 2], inputs=["A", "B", "S"], outputs={"Y": "(A*!S)+(B*S)"},
    vlog={"Y": "(A & ~S) | (B & S) | (A & B)"},
    g={"A": 2.0, "B": 2.0, "S": 2.6}, p=3.5, stages=2,
    sense={("A", "Y"): POS, ("B", "Y"): POS, ("S", "Y"): NON}, sites=7, leak=3.0)
add("MUX4", [1], inputs=["A", "B", "C", "D", "S0", "S1"],
    outputs={"Y": "(A*!S0*!S1)+(B*S0*!S1)+(C*!S0*S1)+(D*S0*S1)"},
    vlog={"Y": "(~S1 & ((A & ~S0) | (B & S0) | (A & B))) | (S1 & ((C & ~S0) | (D & S0) | (C & D))) | "
               "(((A & ~S0) | (B & S0) | (A & B)) & ((C & ~S0) | (D & S0) | (C & D)))"},
    g={"A": 2.0, "B": 2.0, "C": 2.0, "D": 2.0, "S0": 3.6, "S1": 2.6}, p=6.0, stages=2,
    sense={("A", "Y"): POS, ("B", "Y"): POS, ("C", "Y"): POS, ("D", "Y"): POS, ("S0", "Y"): NON, ("S1", "Y"): NON},
    sites=13, leak=6.0)
add("HA", [1], inputs=["A", "B"], outputs={"S": "A^B", "CO": "A*B"}, g={"A": 3.2, "B": 3.2}, p=3.5, stages=2,
    sense={("A", "S"): NON, ("B", "S"): NON, ("A", "CO"): POS, ("B", "CO"): POS}, sites=9, leak=4.0)
add("FA", [1], inputs=["A", "B", "CI"], outputs={"S": "A^B^CI", "CO": "(A*B)+(A*CI)+(B*CI)"},
    g={"A": 4.0, "B": 4.0, "CI": 3.0}, p=5.5, stages=2,
    sense={("A", "S"): NON, ("B", "S"): NON, ("CI", "S"): NON, ("A", "CO"): POS, ("B", "CO"): POS, ("CI", "CO"): POS},
    sites=14, leak=6.5)
add("TBUF", [1, 2], inputs=["A", "EN"], outputs={"Y": "A"}, tri="!EN", g={"A": 1.2, "EN": 1.8}, p=2.5, stages=2,
    sense={("A", "Y"): POS}, sites=5, leak=2.0)

# Sequential cells. q_inv says whether QN exists.
def flop(kind, drives, d_expr, extra, sites, leak, rn=False, sn=False, scan=False):
    ins = ["D"] + extra + ["CK"] + (["RN"] if rn else []) + (["SN"] if sn else [])
    g = {i: 1.2 for i in ins}
    g["CK"] = 1.6
    g["D"] = 1.1
    if "SE" in g:
        g["SE"] = 1.9
    add(kind, drives, inputs=ins, outputs={"Q": "IQ", "QN": "IQN"}, g=g, p=4.0, stages=2, sites=sites, grow=2,
        leak=leak, seq={"type": "ff", "next": d_expr, "clock": "CK", "rn": rn, "sn": sn, "scan": scan,
                        "data": ["D"] + extra})

flop("DFF", [1, 2], "D", [], 17, 7.0)
flop("DFFR", [1, 2], "D", [], 19, 7.8, rn=True)
flop("DFFS", [1, 2], "D", [], 19, 7.8, sn=True)
flop("DFFRS", [1], "D", [], 21, 8.6, rn=True, sn=True)
flop("DFFE", [1], "(E*D)+(!E*IQ)", ["E"], 21, 8.8)
flop("SDFF", [1, 2], "(SE*SI)+(!SE*D)", ["SI", "SE"], 22, 9.2, scan=True)
flop("SDFFR", [1, 2], "(SE*SI)+(!SE*D)", ["SI", "SE"], 24, 10.0, rn=True, scan=True)
flop("SDFFS", [1], "(SE*SI)+(!SE*D)", ["SI", "SE"], 24, 10.0, sn=True, scan=True)
add("DLH", [1, 2], inputs=["D", "E"], outputs={"Q": "IQ"}, g={"D": 1.1, "E": 1.5}, p=3.5, stages=2, sites=10, grow=2,
    leak=4.5, seq={"type": "latch", "enable": "E", "data_in": "D"})
add("DLL", [1], inputs=["D", "EN"], outputs={"Q": "IQ"}, g={"D": 1.1, "EN": 1.5}, p=3.5, stages=2, sites=10, grow=2,
    leak=4.5, seq={"type": "latch", "enable": "!EN", "data_in": "D"})
add("CLKGATE", [1, 2, 4], inputs=["CK", "E", "SE"], outputs={"GCK": "CK*IQ"}, g={"CK": 2.0, "E": 1.1, "SE": 1.1},
    p=3.0, stages=2, sites=14, grow=2, leak=5.5,
    seq={"type": "icg", "enable": "!CK", "data_in": "E+SE"})
# Tie cells and physical-only cells.
CELLS.append(Cell("TIE0", "TIE0", 1, outputs={"Y": "0"}, sites=2, leak=0.3))
CELLS.append(Cell("TIE1", "TIE1", 1, outputs={"Y": "1"}, sites=2, leak=0.3))
for n in (1, 2, 4, 8):
    CELLS.append(Cell("FILL%d" % n, "FILL", 1, sites=n, cls="CORE SPACER", dont_use=True, leak=0.0))

# ------------------------------------------------------------ functions
def py_expr(f):
    """A Liberty function as a Python expression over 0/1 ints."""
    out = []
    for tok in re.findall(r"[A-Za-z_][A-Za-z0-9_]*|[!*+^()]|[01]", f):
        if tok == "!":
            out.append("1-")
        elif tok == "*":
            out.append("&")
        elif tok == "+":
            out.append("|")
        else:
            out.append(tok)
    return "".join(out)

def evalf(f, env):
    return eval(py_expr(f), {"__builtins__": {}}, dict(env)) & 1

def v_expr(f):
    """A Liberty function as a Verilog expression."""
    return f.replace("!", "~").replace("*", " & ").replace("+", " | ").replace("^", " ^ ")

# ------------------------------------------------------------ timing model
def r_drive(c, rising):
    return (R_UP if rising else R_DN) / c.drive

def delay(c, inp, rising, slew, load, corner):
    """Propagation delay in ns of one arc: the slew of inp and the load on
    the output (fF). A two-stage cell has a fixed first stage."""
    k = CORNERS[corner][3]
    r = r_drive(c, rising)
    cpar = C_PAR * c.drive * (c.p if c.stages == 1 else 1.0)
    d = 0.69 * r * (cpar + load) * 1e-3
    if c.stages == 2:
        # the core drives the output inverter's gate
        rc = (R_DN if rising else R_UP) / max(1.0, c.drive / 2.0)
        d += 0.69 * rc * (C_PAR * c.p + C_INV * c.drive * 0.8) * 1e-3
    g = c.g.get(inp, 1.0)
    d += (0.14 + 0.03 * g) * slew                      # a slow input delays the switch point
    d += 0.06 * math.sqrt(slew * r * load * 1e-3)      # and the output fights the input for a while
    return k * d

def trans(c, rising, slew, load, corner):
    k = CORNERS[corner][3]
    r = r_drive(c, rising)
    cpar = C_PAR * c.drive * (c.p if c.stages == 1 else 1.0)
    t = 1.6 * r * (cpar + load) * 1e-3
    t += (0.10 if c.stages == 1 else 0.02) * slew
    return k * t

def loads(c):
    return [round(x * c.drive, 4) for x in LOADS_X1]

def energy(c, inp, rising, slew, load):
    """Internal energy in fJ of one output transition (not counting the load)."""
    v = VDD_TT
    cint = C_PAR * c.drive * c.p * (1.0 if c.stages == 1 else 1.4) + (C_INV * c.drive * 0.8 if c.stages == 2 else 0)
    e = 0.5 * cint * v * v
    e += 0.9 * slew * c.drive * v * (1.0 if rising else 0.8)          # short-circuit current, fJ with slew in ns... scaled
    e += 0.004 * load
    return e

def pin_cap(c, inp):
    return round(C_INV * c.g.get(inp, 1.0) * (c.drive if c.stages == 1 else max(1.0, c.drive / 2.0)), 4)

def leak_nw(c, corner):
    return 2.0 * c.leak * (0.6 + 0.4 * c.drive) * CORNERS[corner][4]

# ------------------------------------------------------------ Liberty writer
def fmt(x):
    return ("%.5f" % x).rstrip("0").rstrip(".") if abs(x) >= 1e-5 or x == 0 else "%.6g" % x

def table(name, tmpl, rows, ind, idx1=None, idx2=None):
    s = "%s%s (%s) {\n" % (ind, name, tmpl)
    if idx1 is not None:
        s += '%s  index_1 ("%s");\n' % (ind, ", ".join(fmt(x) for x in idx1))
    if idx2 is not None:
        s += '%s  index_2 ("%s");\n' % (ind, ", ".join(fmt(x) for x in idx2))
    lines = ['"%s"' % ", ".join("%.5f" % v for v in r) for r in rows]
    s += "%s  values (%s);\n" % (ind, (", \\\n%s          " % ind).join(lines))
    s += "%s}\n" % ind
    return s

def arc_tables(c, inp, out, sense, corner, ind, only=None):
    """cell_rise / cell_fall / transitions for one arc. A negative-unate arc
    rises on a falling input; the table index is the input slew either way."""
    s = ""
    ld = loads(c)
    for rising in (True, False):
        if only is not None and rising != only:
            continue
        d = [[delay(c, inp, rising, sl, l, corner) for l in ld] for sl in SLEWS]
        t = [[trans(c, rising, sl, l, corner) for l in ld] for sl in SLEWS]
        s += table("cell_rise" if rising else "cell_fall", "delay_7x7", d, ind, SLEWS, ld)
        s += table("rise_transition" if rising else "fall_transition", "delay_7x7", t, ind, SLEWS, ld)
    return s

CONS_SLEWS = [0.005, 0.04, 0.32]

def cons_table(name, base, kd, kc, corner, ind):
    """A constraint over (related pin slew, constrained pin slew)."""
    k = CORNERS[corner][3]
    rows = [[k * (base + kc * cs + kd * ds) for ds in CONS_SLEWS] for cs in CONS_SLEWS]
    return table(name, "constraint_3x3", rows, ind)

def state_leakage(c, corner):
    """leakage_power groups per input state, for cells with up to 3 inputs."""
    ins = c.inputs
    if c.seq or not ins or len(ins) > 3 or not c.outputs:
        return []
    base = leak_nw(c, corner)
    out = []
    n = len(ins)
    for bits in itertools.product((0, 1), repeat=n):
        ones = sum(bits)
        # more inputs high: the pull-down conducts and the pull-up stack leaks,
        # which is the leakier state for a NAND-like cell; spread around the mean
        f = 0.7 + 0.6 * ones / n
        when = " * ".join(("%s" if b else "!%s") % i for i, b in zip(ins, bits))
        out.append((when, base * f))
    return out

def lib_cell(c, corner):
    L = []
    w = L.append
    w("  cell (%s) {\n" % c.name)
    w("    area : %s;\n" % fmt(c.sites * SITE_W * ROW_H))
    w('    cell_footprint : "%s";\n' % c.footprint)
    w("    cell_leakage_power : %s;\n" % fmt(leak_nw(c, corner)))
    for when, val in state_leakage(c, corner):
        w('    leakage_power () { when : "%s"; value : %s; }\n' % (when, fmt(val)))
    if c.dont_use:
        w("    dont_use : true;\n    dont_touch : true;\n")
    if c.kind.startswith("TIE"):
        w("    dont_touch : true;\n")
    w("    pg_pin (VDD) { voltage_name : VDD; pg_type : primary_power; }\n")
    w("    pg_pin (VSS) { voltage_name : VSS; pg_type : primary_ground; }\n")
    q = c.seq
    if q and q["type"] == "ff":
        w("    ff (IQ, IQN) {\n")
        w('      clocked_on : "%s";\n      next_state : "%s";\n' % (q["clock"], q["next"]))
        if q["rn"]:
            w('      clear : "!RN";\n')
        if q["sn"]:
            w('      preset : "!SN";\n')
        if q["rn"] and q["sn"]:
            w("      clear_preset_var1 : L;\n      clear_preset_var2 : L;\n")
        w("    }\n")
        if q["scan"]:
            w("    test_cell () {\n")
            w('      ff (IQ, IQN) {\n        clocked_on : "CK";\n        next_state : "D";\n')
            if q["rn"]:
                w('        clear : "!RN";\n')
            if q["sn"]:
                w('        preset : "!SN";\n')
            w("      }\n")
            w("      pin (D) { direction : input; }\n")
            w("      pin (SI) { direction : input; signal_type : test_scan_in; }\n")
            w("      pin (SE) { direction : input; signal_type : test_scan_enable; }\n")
            w("      pin (CK) { direction : input; }\n")
            if q["rn"]:
                w("      pin (RN) { direction : input; }\n")
            if q["sn"]:
                w("      pin (SN) { direction : input; }\n")
            w('      pin (Q) { direction : output; function : "IQ"; signal_type : test_scan_out; }\n')
            w('      pin (QN) { direction : output; function : "IQN"; signal_type : test_scan_out_inverted; }\n')
            w("    }\n")
    elif q and q["type"] in ("latch", "icg"):
        w('    latch (IQ, IQN) {\n      enable : "%s";\n      data_in : "%s";\n    }\n' % (q["enable"], q["data_in"]))
        if q["type"] == "icg":
            w('    clock_gating_integrated_cell : "latch_posedge_precontrol";\n')

    # ---- inputs
    for i in c.inputs:
        w("    pin (%s) {\n      direction : input;\n      capacitance : %s;\n" % (i, fmt(pin_cap(c, i))))
        w("      related_power_pin : VDD;\n      related_ground_pin : VSS;\n")
        if q and q["type"] == "ff":
            if i == "CK":
                w("      clock : true;\n")
                mpw = CORNERS[corner][3] * 0.06
                w("      min_pulse_width_high : %s;\n      min_pulse_width_low : %s;\n" % (fmt(mpw), fmt(mpw)))
            elif i in q["data"]:
                w("      nextstate_type : %s;\n" % {"D": "data", "SI": "scan_in", "SE": "scan_enable", "E": "load"}[i])
                for tt, base, kd, kc in (("setup_rising", 0.035, 0.30, -0.10), ("hold_rising", 0.004, -0.12, 0.22)):
                    w('      timing () {\n        related_pin : "CK";\n        timing_type : %s;\n' % tt)
                    w(cons_table("rise_constraint", base, kd, kc, corner, "        "))
                    w(cons_table("fall_constraint", base * 1.15, kd, kc, corner, "        "))
                    w("      }\n")
            else:   # RN, SN: recovery and removal against the clock
                for tt, base, kd, kc in (("recovery_rising", 0.030, 0.25, -0.08), ("removal_rising", 0.012, -0.10, 0.20)):
                    w('      timing () {\n        related_pin : "CK";\n        timing_type : %s;\n' % tt)
                    w(cons_table("rise_constraint", base, kd, kc, corner, "        "))
                    w("      }\n")
                w("      min_pulse_width_low : %s;\n" % fmt(CORNERS[corner][3] * 0.07))
        elif q and q["type"] == "latch":
            en = i in ("E", "EN")
            if en:
                w("      clock : true;\n")
            else:
                # data must be stable around the closing edge of the enable
                closing = "falling" if q["enable"] == "E" else "rising"
                for tt, base, kd, kc in (("setup_" + closing, 0.030, 0.28, -0.10), ("hold_" + closing, 0.006, -0.10, 0.20)):
                    w('      timing () {\n        related_pin : "%s";\n        timing_type : %s;\n' % (c.inputs[1], tt))
                    w(cons_table("rise_constraint", base, kd, kc, corner, "        "))
                    w(cons_table("fall_constraint", base * 1.15, kd, kc, corner, "        "))
                    w("      }\n")
        elif q and q["type"] == "icg":
            if i == "CK":
                w("      clock : true;\n      clock_gate_clock_pin : true;\n")
            else:
                w("      %s : true;\n" % ("clock_gate_enable_pin" if i == "E" else "clock_gate_test_pin"))
                for tt, base, kd, kc in (("setup_rising", 0.040, 0.30, -0.10), ("hold_rising", 0.002, -0.12, 0.22)):
                    w('      timing () {\n        related_pin : "CK";\n        timing_type : %s;\n' % tt)
                    w(cons_table("rise_constraint", base, kd, kc, corner, "        "))
                    w(cons_table("fall_constraint", base * 1.15, kd, kc, corner, "        "))
                    w("      }\n")
        w("    }\n")

    # ---- outputs
    for o, f in c.outputs.items():
        w("    pin (%s) {\n      direction : output;\n" % o)
        w("      related_power_pin : VDD;\n      related_ground_pin : VSS;\n")
        w('      function : "%s";\n' % f)
        if c.tri:
            w('      three_state : "%s";\n' % c.tri)
        if q and q["type"] == "icg":
            w("      clock_gate_out_pin : true;\n")
        if c.kind.startswith("TIE"):
            w("    }\n")
            continue
        w("      max_capacitance : %s;\n" % fmt(loads(c)[-1]))
        ld = loads(c)

        def power(rel, when=None):
            w('      internal_power () {\n        related_pin : "%s";\n' % rel)
            if when:
                w('        when : "%s";\n' % when)
            for rising in (True, False):
                e = [[energy(c, rel, rising, sl, l) for l in ld] for sl in SLEWS]
                w(table("rise_power" if rising else "fall_power", "power_7x7", e, "        ", SLEWS, ld))
            w("      }\n")

        if q and q["type"] == "ff":
            inv = (o == "QN")
            w('      timing () {\n        related_pin : "CK";\n        timing_type : rising_edge;\n        timing_sense : non_unate;\n')
            w(arc_tables(c, "CK", o, NON, corner, "        "))
            w("      }\n")
            if q["rn"]:   # RN low clears Q, sets QN
                w('      timing () {\n        related_pin : "RN";\n        timing_type : %s;\n        timing_sense : %s;\n'
                  % ("preset" if inv else "clear", NEG if inv else POS))
                w(arc_tables(c, "RN", o, POS, corner, "        ", only=inv))
                w("      }\n")
            if q["sn"]:   # SN low sets Q, clears QN
                w('      timing () {\n        related_pin : "SN";\n        timing_type : %s;\n        timing_sense : %s;\n'
                  % ("clear" if inv else "preset", POS if inv else NEG))
                w(arc_tables(c, "SN", o, NEG, corner, "        ", only=not inv))
                w("      }\n")
            power("CK")
        elif q and q["type"] == "latch":
            en = c.inputs[1]
            w('      timing () {\n        related_pin : "D";\n        timing_sense : positive_unate;\n')
            w(arc_tables(c, "D", o, POS, corner, "        "))
            w("      }\n")
            w('      timing () {\n        related_pin : "%s";\n        timing_type : %s;\n        timing_sense : non_unate;\n'
              % (en, "rising_edge" if en == "E" else "falling_edge"))
            w(arc_tables(c, en, o, NON, corner, "        "))
            w("      }\n")
            power(en)
        elif q and q["type"] == "icg":
            w('      timing () {\n        related_pin : "CK";\n        timing_sense : positive_unate;\n')
            w(arc_tables(c, "CK", o, POS, corner, "        "))
            w("      }\n")
            power("CK")
        else:
            for i in c.inputs:
                s = c.sense.get((i, o))
                if c.tri and i == "EN":
                    for tt in ("three_state_enable", "three_state_disable"):
                        w('      timing () {\n        related_pin : "EN";\n        timing_type : %s;\n        timing_sense : %s;\n'
                          % (tt, NEG if tt.endswith("enable") else POS))
                        w(arc_tables(c, "EN", o, NEG, corner, "        "))
                        w("      }\n")
                    continue
                if s is None:
                    continue
                w('      timing () {\n        related_pin : "%s";\n        timing_sense : %s;\n' % (i, s))
                w(arc_tables(c, i, o, s, corner, "        "))
                w("      }\n")
                power(i)
        w("    }\n")
    w("  }\n")
    return "".join(L)

def liberty(corner):
    proc, volt, temp, kd, kl = CORNERS[corner]
    name = "mini45_%s" % corner
    s = "/* mini45: a generic standard cell library for testing tools. Made up, not a\n"
    s += "   foundry library. Generated by gen_mini45.py; do not edit. */\n"
    s += "library (%s) {\n" % name
    s += "  technology (cmos);\n  delay_model : table_lookup;\n"
    s += '  comment : "mini45 %s corner";\n' % corner
    s += '  time_unit : "1ns";\n  voltage_unit : "1V";\n  current_unit : "1mA";\n'
    s += '  pulling_resistance_unit : "1kohm";\n  leakage_power_unit : "1nW";\n'
    s += "  capacitive_load_unit (1, ff);\n"
    s += "  slew_lower_threshold_pct_rise : 20;\n  slew_upper_threshold_pct_rise : 80;\n"
    s += "  slew_lower_threshold_pct_fall : 20;\n  slew_upper_threshold_pct_fall : 80;\n"
    s += "  slew_derate_from_library : 1;\n"
    s += "  input_threshold_pct_rise : 50;\n  input_threshold_pct_fall : 50;\n"
    s += "  output_threshold_pct_rise : 50;\n  output_threshold_pct_fall : 50;\n"
    s += "  nom_process : %s;\n  nom_voltage : %s;\n  nom_temperature : %s;\n" % (fmt(proc), fmt(volt), fmt(temp))
    s += "  default_max_transition : %s;\n" % fmt(0.5 * kd)
    s += "  default_fanout_load : 1;\n  default_inout_pin_cap : 1;\n  default_input_pin_cap : 1;\n"
    s += "  default_output_pin_cap : 0;\n  default_cell_leakage_power : 0;\n  default_leakage_power_density : 0;\n"
    s += "  voltage_map (VDD, %s);\n  voltage_map (VSS, 0);\n" % fmt(volt)
    s += "  operating_conditions (%s) {\n    process : %s;\n    voltage : %s;\n    temperature : %s;\n  }\n" % (
        corner, fmt(proc), fmt(volt), fmt(temp))
    s += "  default_operating_conditions : %s;\n" % corner
    s += "  lu_table_template (delay_7x7) {\n    variable_1 : input_net_transition;\n"
    s += "    variable_2 : total_output_net_capacitance;\n"
    s += '    index_1 ("%s");\n    index_2 ("%s");\n  }\n' % (", ".join(fmt(x) for x in SLEWS), ", ".join(fmt(x) for x in LOADS_X1))
    s += "  lu_table_template (constraint_3x3) {\n    variable_1 : related_pin_transition;\n"
    s += "    variable_2 : constrained_pin_transition;\n"
    s += '    index_1 ("%s");\n    index_2 ("%s");\n  }\n' % ((", ".join(fmt(x) for x in CONS_SLEWS),) * 2)
    s += "  power_lut_template (power_7x7) {\n    variable_1 : input_transition_time;\n"
    s += "    variable_2 : total_output_net_capacitance;\n"
    s += '    index_1 ("%s");\n    index_2 ("%s");\n  }\n' % (", ".join(fmt(x) for x in SLEWS), ", ".join(fmt(x) for x in LOADS_X1))
    # Wire load models for synthesis before placement: resistance in kohm
    # and capacitance in fF per unit length, lengths by fanout.
    for wl, slope, lens in (("small", 6.0, [4, 9, 15, 22, 30]), ("medium", 9.0, [6, 14, 24, 35, 48]), ("large", 14.0, [9, 22, 38, 56, 77])):
        s += "  wire_load (%s) {\n    resistance : 0.0004;\n    capacitance : 0.16;\n    area : 0.01;\n    slope : %s;\n" % (wl, fmt(slope))
        for n, l in enumerate(lens, 1):
            s += "    fanout_length (%d, %d);\n" % (n, l)
        s += "  }\n"
    s += "  default_wire_load : small;\n"
    for c in CELLS:
        s += lib_cell(c, corner)
    s += "}\n"
    return s

# ------------------------------------------------------------ Verilog models
UDPS = r"""
// D flip-flop: clear and preset are active high here, clear wins.
primitive mini45_udp_dff (q, d, ck, clr, pre, notifier);
    output q;
    reg q;
    input d, ck, clr, pre, notifier;
    table
    //  d  ck  clr pre ntf : q : q+
        ?   ?   1   ?   ?  : ? : 0;
        ?   ?   0   1   ?  : ? : 1;
        0   r   0   0   ?  : ? : 0;
        1   r   0   0   ?  : ? : 1;
        0   *   0   0   ?  : 0 : 0;     // an edge that is not a clean rise keeps a matching value
        1   *   0   0   ?  : 1 : 1;
        ?   f   0   0   ?  : ? : -;
        ?  (?0) 0   0   ?  : ? : -;
        ?  (1x) 0   0   ?  : ? : -;
        *   ?   0   0   ?  : ? : -;     // data changes away from the edge
        ?   ?  (?0) 0   ?  : ? : -;     // releasing clear or preset keeps the value
        ?   ?   0  (?0) ?  : ? : -;
        ?   ?   ?   ?   *  : ? : x;     // a timing violation
    endtable
endprimitive

// Latch, transparent while en is 1.
primitive mini45_udp_latch (q, d, en, notifier);
    output q;
    reg q;
    input d, en, notifier;
    table
    //  d  en  ntf : q : q+
        0   1   ?  : ? : 0;
        1   1   ?  : ? : 1;
        ?   0   ?  : ? : -;
        0   x   ?  : 0 : 0;
        1   x   ?  : 1 : 1;
        ?   ?   *  : ? : x;
    endtable
endprimitive
"""

def verilog():
    s = "// mini45: Verilog models of the generic standard cell library.\n"
    s += "// Functional, with zero-delay specify paths and timing checks that an SDF\n"
    s += "// file can annotate. Generated by gen_mini45.py; do not edit.\n"
    s += "`timescale 1ns/1ps\n`celldefine\n"
    s += UDPS
    for c in CELLS:
        ports = list(c.outputs) + c.inputs
        s += "\nmodule %s (%s);\n" % (c.name, ", ".join(ports))
        for o in c.outputs:
            s += "    output %s;\n" % o
        for i in c.inputs:
            s += "    input %s;\n" % i
        q = c.seq
        if c.cls != "CORE":
            pass
        elif q and q["type"] == "ff":
            s += "    reg notifier;\n    wire iq, dd, clr, pre;\n"
            nxt = q["next"].replace("IQ", "iq")
            s += "    assign dd = %s;\n" % (v_expr(nxt) if nxt != "D" else "D")
            if q["scan"]:
                # as written, SE = x with SI == D would give x
                s += "    // the mux keeps a known value when SE is unknown and both inputs agree\n"
                s = s.replace("    assign dd = (SE & SI) | (~SE & D);\n", "    assign dd = (SE & SI) | (~SE & D) | (SI & D);\n")
            if c.kind == "DFFE":
                s = s.replace("    assign dd = (E & D) | (~E & iq);\n", "    assign dd = (E & D) | (~E & iq) | (D & iq);\n")
            s += "    assign clr = %s;\n" % ("~RN" if q["rn"] else "1'b0")
            s += "    assign pre = %s;\n" % ("~SN" if q["sn"] else "1'b0")
            s += "    mini45_udp_dff u_ff (iq, dd, CK, clr, pre, notifier);\n"
            s += "    buf (Q, iq);\n"
            # with clear and preset both active, both outputs are low (clear_preset_var1/2 : L)
            s += "    assign QN = %s;\n" % ("~iq & ~pre" if (q["rn"] and q["sn"]) else "~iq")
            s += "    specify\n        (posedge CK => (Q +: D)) = (0, 0);\n        (posedge CK => (QN -: D)) = (0, 0);\n"
            if q["rn"]:
                s += "        (negedge RN => (Q +: 1'b0)) = (0, 0);\n        (negedge RN => (QN -: 1'b0)) = (0, 0);\n"
                s += "        $recovery(posedge RN, posedge CK, 0, notifier);\n        $removal(posedge RN, posedge CK, 0, notifier);\n"
                s += "        $width(negedge RN, 0, 0, notifier);\n"
            if q["sn"]:
                s += "        (negedge SN => (Q -: 1'b0)) = (0, 0);\n        (negedge SN => (QN +: 1'b0)) = (0, 0);\n"
                s += "        $recovery(posedge SN, posedge CK, 0, notifier);\n        $removal(posedge SN, posedge CK, 0, notifier);\n"
                s += "        $width(negedge SN, 0, 0, notifier);\n"
            for d in q["data"]:
                s += "        $setuphold(posedge CK, posedge %s, 0, 0, notifier);\n" % d
                s += "        $setuphold(posedge CK, negedge %s, 0, 0, notifier);\n" % d
            s += "        $width(posedge CK, 0, 0, notifier);\n        $width(negedge CK, 0, 0, notifier);\n"
            s += "    endspecify\n"
        elif q and q["type"] == "latch":
            en = c.inputs[1]
            s += "    reg notifier;\n    wire iq, en;\n"
            s += "    assign en = %s;\n" % ("E" if en == "E" else "~EN")
            s += "    mini45_udp_latch u_lat (iq, D, en, notifier);\n    buf (Q, iq);\n"
            edge = "posedge" if en == "E" else "negedge"
            close = "negedge" if en == "E" else "posedge"
            s += "    specify\n        (D => Q) = (0, 0);\n        (%s %s => (Q +: D)) = (0, 0);\n" % (edge, en)
            s += "        $setuphold(%s %s, posedge D, 0, 0, notifier);\n" % (close, en)
            s += "        $setuphold(%s %s, negedge D, 0, 0, notifier);\n" % (close, en)
            s += "    endspecify\n"
        elif q and q["type"] == "icg":
            s += "    reg notifier;\n    wire iq, nck, en;\n"
            s += "    not (nck, CK);\n    or (en, E, SE);\n"
            s += "    mini45_udp_latch u_lat (iq, en, nck, notifier);\n    and (GCK, CK, iq);\n"
            s += "    specify\n        (CK => GCK) = (0, 0);\n"
            for d in ("E", "SE"):
                s += "        $setuphold(posedge CK, posedge %s, 0, 0, notifier);\n" % d
                s += "        $setuphold(posedge CK, negedge %s, 0, 0, notifier);\n" % d
            s += "    endspecify\n"
        elif c.tri:
            s += "    bufif1 (Y, A, EN);\n"
            s += "    specify\n        (A => Y) = (0, 0);\n        (EN => Y) = (0, 0);\n    endspecify\n"
        elif c.kind == "TIE0":
            s += "    supply0 vss;\n    buf (Y, vss);\n"
        elif c.kind == "TIE1":
            s += "    supply1 vdd;\n    buf (Y, vdd);\n"
        else:
            for o, f in c.outputs.items():
                s += "    assign %s = %s;\n" % (o, c.vlog.get(o, v_expr(f)))
            s += "    specify\n"
            for o in c.outputs:
                for i in c.inputs:
                    if (i, o) in c.sense:
                        s += "        (%s => %s) = (0, 0);\n" % (i, o)
            s += "    endspecify\n"
        s += "endmodule\n"
    s += "\n`endcelldefine\n"
    return s

# ------------------------------------------------------------ LEF
M1_W = 0.07

def tech_lef():
    s = "# mini45 technology LEF: a made-up 45 nm-class stack for testing tools.\n"
    s += "# Generated by gen_mini45.py; do not edit.\n"
    s += 'VERSION 5.8 ;\nBUSBITCHARS "[]" ;\nDIVIDERCHAR "/" ;\n\n'
    s += "UNITS\n  TIME NANOSECONDS 1 ;\n  CAPACITANCE PICOFARADS 1 ;\n  RESISTANCE OHMS 1 ;\n  DATABASE MICRONS 2000 ;\nEND UNITS\n\n"
    s += "MANUFACTURINGGRID 0.005 ;\n\n"
    s += "SITE core\n  CLASS CORE ;\n  SYMMETRY Y ;\n  SIZE %.2f BY %.2f ;\nEND core\n\n" % (SITE_W, ROW_H)
    s += "LAYER poly\n  TYPE MASTERSLICE ;\nEND poly\n\n"
    # name, direction, pitch, width, spacing, ohm/sq, pF/um^2, pF/um edge, thickness, cut below (width, spacing, ohm)
    layers = [
        ("metal1", "HORIZONTAL", 0.14, 0.07, 0.065, 0.38, 7.7e-5, 2.7e-5, 0.13, ("contact", 0.065, 0.075, 10.0)),
        ("metal2", "VERTICAL", 0.19, 0.07, 0.07, 0.25, 4.0e-5, 2.5e-5, 0.14, ("via1", 0.065, 0.075, 5.0)),
        ("metal3", "HORIZONTAL", 0.14, 0.07, 0.07, 0.25, 2.7e-5, 2.5e-5, 0.14, ("via2", 0.07, 0.085, 5.0)),
        ("metal4", "VERTICAL", 0.28, 0.14, 0.14, 0.21, 2.0e-5, 2.3e-5, 0.28, ("via3", 0.07, 0.085, 5.0)),
        ("metal5", "HORIZONTAL", 0.28, 0.14, 0.14, 0.21, 1.6e-5, 2.3e-5, 0.28, ("via4", 0.14, 0.16, 3.0)),
        ("metal6", "VERTICAL", 0.57, 0.28, 0.28, 0.08, 1.2e-5, 2.0e-5, 0.60, ("via5", 0.14, 0.16, 3.0)),
    ]
    height = 0.37
    for name, d, pitch, width, sp, rs, ca, ce, th, cut in layers:
        cn, cw, cs, cr = cut
        if cn != "contact":
            s += "LAYER %s\n  TYPE CUT ;\n  WIDTH %.3f ;\n  SPACING %.3f ;\n  RESISTANCE %.1f ;\nEND %s\n\n" % (cn, cw, cs, cr, cn)
        off = 0.0 if d == "HORIZONTAL" else pitch / 2.0
        s += "LAYER %s\n  TYPE ROUTING ;\n  DIRECTION %s ;\n  PITCH %.3f ;\n  OFFSET %.3f ;\n  WIDTH %.3f ;\n  SPACING %.3f ;\n" % (
            name, d, pitch, off, width, sp)
        s += "  AREA %.4f ;\n" % (width * width * 3)
        s += "  RESISTANCE RPERSQ %.3f ;\n  CAPACITANCE CPERSQDIST %.3e ;\n  EDGECAPACITANCE %.3e ;\n" % (rs, ca, ce)
        s += "  THICKNESS %.2f ;\n  HEIGHT %.2f ;\nEND %s\n\n" % (th, height, name)
        height += th + 0.12
    s += "LAYER OVERLAP\n  TYPE OVERLAP ;\nEND OVERLAP\n\n"
    for n in range(1, 6):
        lo, hi = layers[n - 1], layers[n]
        cn, cw = hi[9][0], hi[9][1]
        e1, e2 = max(lo[3], cw + 0.04) / 2.0, max(hi[3], cw + 0.04) / 2.0
        h = cw / 2.0
        s += "VIA via%d_0 DEFAULT\n  RESISTANCE %.1f ;\n" % (n, hi[9][3])
        # the lower metal extends along its own direction
        if lo[1] == "HORIZONTAL":
            s += "  LAYER %s ;\n    RECT %.3f %.3f %.3f %.3f ;\n" % (lo[0], -e1 - 0.03, -lo[3] / 2 if lo[3] >= cw else -h, e1 + 0.03, lo[3] / 2 if lo[3] >= cw else h)
        else:
            s += "  LAYER %s ;\n    RECT %.3f %.3f %.3f %.3f ;\n" % (lo[0], -lo[3] / 2 if lo[3] >= cw else -h, -e1 - 0.03, lo[3] / 2 if lo[3] >= cw else h, e1 + 0.03)
        s += "  LAYER %s ;\n    RECT %.4f %.4f %.4f %.4f ;\n" % (cn, -h, -h, h, h)
        if hi[1] == "HORIZONTAL":
            s += "  LAYER %s ;\n    RECT %.3f %.3f %.3f %.3f ;\n" % (hi[0], -e2 - 0.03, -hi[3] / 2 if hi[3] >= cw else -h, e2 + 0.03, hi[3] / 2 if hi[3] >= cw else h)
        else:
            s += "  LAYER %s ;\n    RECT %.3f %.3f %.3f %.3f ;\n" % (hi[0], -hi[3] / 2 if hi[3] >= cw else -h, -e2 - 0.03, hi[3] / 2 if hi[3] >= cw else h, e2 + 0.03)
        s += "END via%d_0\n\n" % n
    s += "END LIBRARY\n"
    return s

def cell_lef():
    s = "# mini45 cell LEF. Generated by gen_mini45.py; do not edit.\n"
    s += 'VERSION 5.8 ;\nBUSBITCHARS "[]" ;\nDIVIDERCHAR "/" ;\n\n'
    rail = 0.085
    for c in CELLS:
        w = c.sites * SITE_W
        s += "MACRO %s\n  CLASS %s ;\n  ORIGIN 0 0 ;\n  FOREIGN %s 0 0 ;\n  SIZE %.2f BY %.2f ;\n  SYMMETRY X Y ;\n  SITE core ;\n" % (
            c.name, c.cls, c.name, w, ROW_H)
        pins = [(i, "INPUT") for i in c.inputs] + [(o, "OUTPUT") for o in c.outputs]
        if pins and len(pins) > c.sites:
            raise SystemExit("%s: %d pins do not fit %d sites" % (c.name, len(pins), c.sites))
        # One metal2 track per pin, spread over the cell, on metal1 tracks 3..7
        # in a zigzag so neighbours keep their spacing. Each shape is a
        # metal1 bar centred on a metal1 track, covering its metal2 track.
        used = []
        for n, (p, d) in enumerate(pins):
            col = int(round((n + 0.5) * c.sites / len(pins) - 0.5))
            while col in used:
                col += 1
            used.append(col)
            x = SITE_W / 2 + col * SITE_W
            y = M1_PITCH * (3 + (n % 3) * 2)
            s += "  PIN %s\n    DIRECTION %s%s ;\n    USE %s ;\n" % (
                p, d, " TRISTATE" if (c.tri and d == "OUTPUT") else "",
                "CLOCK" if p == "CK" and c.seq else "SIGNAL")
            s += "    PORT\n      LAYER metal1 ;\n        RECT %.3f %.3f %.3f %.3f ;\n    END\n  END %s\n" % (
                x - M1_W / 2, y - 0.07, x + M1_W / 2, y + 0.07, p)
        for p, use, y in (("VDD", "POWER", ROW_H), ("VSS", "GROUND", 0.0)):
            s += "  PIN %s\n    DIRECTION INOUT ;\n    USE %s ;\n    SHAPE ABUTMENT ;\n" % (p, use)
            s += "    PORT\n      LAYER metal1 ;\n        RECT 0.000 %.3f %.3f %.3f ;\n    END\n  END %s\n" % (y - rail, w, y + rail, p)
        s += "END %s\n\n" % c.name
    s += "END LIBRARY\n"
    return s

# ------------------------------------------------------------ test bench
def testbench():
    """Every combinational model against its function's truth table, and
    directed checks of the sequential cells."""
    s = "// Generated by gen_mini45.py; do not edit. Checks mini45.v: each\n"
    s += "// combinational cell against the truth table of its Liberty function, and\n"
    s += "// the flops, latches and clock gate with directed stimulus.\n"
    s += "`timescale 1ns/1ps\nmodule mini45_tb;\n    integer errors = 0;\n    integer cells = 0;\n"
    body = ""
    n = 0
    for c in CELLS:
        if c.seq or c.cls != "CORE":
            continue
        n += 1
        ins = c.inputs
        outs = list(c.outputs)
        if ins:
            s += "    reg [%d:0] i%d;\n" % (len(ins) - 1, n)
        s += "    wire [%d:0] o%d;\n" % (len(outs) - 1, n)
        conns = [".%s(o%d[%d])" % (o, n, k) for k, o in enumerate(outs)] + [".%s(i%d[%d])" % (i, n, k) for k, i in enumerate(ins)]
        s += "    %s u%d (%s);\n" % (c.name, n, ", ".join(conns))
        body += "        cells = cells + 1;\n"
        for bits in itertools.product((0, 1), repeat=len(ins)):
            env = dict(zip(ins, bits))
            want = ""
            for o in reversed(outs):
                if c.tri and evalf(c.tri, env):
                    want += "z"
                else:
                    want += str(evalf(c.outputs[o], env))
            if ins:
                body += "        i%d = %d'b%s; #1;\n" % (n, len(ins), "".join(str(b) for b in reversed(bits)))
            else:
                body += "        #1;\n"
            body += "        if (o%d !== %d'b%s) begin errors = errors + 1; $display(\"%s %s: got %%b\", o%d); end\n" % (
                n, len(outs), want, c.name, "".join(str(b) for b in bits), n)
    # X handling of the muxes: equal data inputs give a known output
    s += "    reg mx_a, mx_s;\n    wire mx_y;\n    MUX2_X1 u_mx (.Y(mx_y), .A(mx_a), .B(mx_a), .S(mx_s));\n"
    body += "        mx_a = 1; mx_s = 1'bx; #1;\n"
    body += "        if (mx_y !== 1'b1) begin errors = errors + 1; $display(\"MUX2 with S=x: %b\", mx_y); end\n"
    # sequential cells
    s += "    reg ck = 0, d = 0, rn = 1, sn = 1, se = 0, si = 0, e = 1;\n"
    seq = [c for c in CELLS if c.seq]
    for k, c in enumerate(seq):
        q = c.seq
        m = {"D": "d", "CK": "ck", "RN": "rn", "SN": "sn", "SE": "se", "SI": "si", "E": "e", "EN": "e"}
        outs = list(c.outputs)
        s += "    wire %s;\n" % ", ".join("s%d_%s" % (k, o.lower()) for o in outs)
        conns = [".%s(s%d_%s)" % (o, k, o.lower()) for o in outs] + [".%s(%s)" % (i, m[i]) for i in c.inputs]
        s += "    %s us%d (%s);\n" % (c.name, k, ", ".join(conns))
    s += "    task chk(input got, input want, input [8*24-1:0] what);\n"
    s += "        if (got !== want) begin errors = errors + 1; $display(\"%0s: got %b, want %b at %0t\", what, got, want, $time); end\n"
    s += "    endtask\n"
    s += "    task tick; begin #2 ck = 1; #2 ck = 0; end endtask\n"
    body += "        // ---- flops: capture, hold, scan, enable, asynchronous clear and preset\n"
    body += "        d = 1; se = 0; si = 0; e = 1; rn = 1; sn = 1; tick;\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "ff":
            body += "        cells = cells + 1; chk(s%d_q, 1'b1, \"%s capture 1\"); chk(s%d_qn, 1'b0, \"%s QN\");\n" % (k, c.name, k, c.name)
    body += "        d = 0; tick;\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "ff":
            body += "        chk(s%d_q, 1'b0, \"%s capture 0\"); chk(s%d_qn, 1'b1, \"%s QN\");\n" % (k, c.name, k, c.name)
    body += "        d = 1; #1;   // no clock edge: nothing moves\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "ff":
            body += "        chk(s%d_q, 1'b0, \"%s holds\");\n" % (k, c.name)
    body += "        d = 0; se = 1; si = 1; tick;   // scan shifts SI in\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "ff":
            body += "        chk(s%d_q, 1'b%d, \"%s scan\");\n" % (k, 1 if c.seq["scan"] else 0, c.name)
    body += "        se = 0; si = 0; d = 1; e = 0; tick;   // enable low keeps the value\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "ff":
            keep = 1 if c.seq["scan"] else 0
            body += "        chk(s%d_q, 1'b%d, \"%s enable\");\n" % (k, keep if c.kind == "DFFE" else 1, c.name)
    body += "        e = 1; d = 1; tick; rn = 0; #1;   // clear without a clock\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "ff":
            body += "        chk(s%d_q, 1'b%d, \"%s clear\"); chk(s%d_qn, 1'b%d, \"%s clear QN\");\n" % (
                k, 0 if c.seq["rn"] else 1, c.name, k, 1 if c.seq["rn"] else 0, c.name)
    body += "        tick;   // clear holds through a clock\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "ff":
            body += "        chk(s%d_q, 1'b%d, \"%s clear held\");\n" % (k, 0 if c.seq["rn"] else 1, c.name)
    body += "        rn = 1; d = 0; tick; sn = 0; #1;   // preset\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "ff":
            body += "        chk(s%d_q, 1'b%d, \"%s preset\");\n" % (k, 1 if c.seq["sn"] else 0, c.name)
    body += "        rn = 0; #1;   // both: Q and QN low where the cell has both pins\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "ff" and c.seq["rn"] and c.seq["sn"]:
            body += "        chk(s%d_q, 1'b0, \"%s clear+preset\"); chk(s%d_qn, 1'b0, \"%s clear+preset QN\");\n" % (k, c.name, k, c.name)
    body += "        rn = 1; sn = 1; d = 0; tick;\n"
    body += "        // ---- latches: transparent, then closed\n"
    body += "        ck = 0; e = 1; d = 1; #1;\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "latch":
            hi = c.inputs[1] == "E"
            body += "        cells = cells + 1;\n"
            if hi:
                body += "        chk(s%d_q, 1'b1, \"%s open\");\n" % (k, c.name)
    body += "        e = 0; #1; d = 0; #1;\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "latch":
            hi = c.inputs[1] == "E"
            body += "        chk(s%d_q, 1'b%d, \"%s after e=0\");\n" % (k, 1 if hi else 0, c.name)
    body += "        e = 1; #1; d = 1; #1;\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "latch":
            hi = c.inputs[1] == "E"
            body += "        chk(s%d_q, 1'b%d, \"%s after e=1\");\n" % (k, 1 if hi else 0, c.name)
    body += "        // ---- clock gates: enable sampled while the clock is low, no glitch\n"
    body += "        ck = 0; se = 0; e = 1; #1; ck = 1; #1;\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "icg":
            body += "        cells = cells + 1; chk(s%d_gck, 1'b1, \"%s enabled\");\n" % (k, c.name)
    body += "        e = 0; #1;   // dropping the enable while the clock is high does not cut the pulse\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "icg":
            body += "        chk(s%d_gck, 1'b1, \"%s no glitch\");\n" % (k, c.name)
    body += "        ck = 0; #1; ck = 1; #1;\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "icg":
            body += "        chk(s%d_gck, 1'b0, \"%s gated\");\n" % (k, c.name)
    body += "        e = 1; #1;   // raising it while the clock is high does not start one\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "icg":
            body += "        chk(s%d_gck, 1'b0, \"%s no late pulse\");\n" % (k, c.name)
    body += "        ck = 0; e = 0; se = 1; #1; ck = 1; #1;   // the test pin enables too\n"
    for k, c in enumerate(seq):
        if c.seq["type"] == "icg":
            body += "        chk(s%d_gck, 1'b1, \"%s test enable\");\n" % (k, c.name)
    s += "    initial begin\n" + body
    s += "        $display(\"mini45: %0d cells checked, %0d errors\", cells, errors);\n        $finish;\n    end\nendmodule\n"
    return s

# ------------------------------------------------------------ transistors
# Transistor netlists for the characterizer (mchar on mspice). Static CMOS
# with level-1 MOSFETs; the devices are as made up as the rest of mini45.
L_GATE = 0.05            # um
WN = 0.20                # um, INV_X1 pull-down
WP = 0.30                # um, INV_X1 pull-up
DIFF = 0.10              # um, source/drain length

MODELS = {
    # corner: (nmos vto, nmos kp, pmos vto, pmos kp)
    # A level-1 device's mobility falls as T^-1.5, far more than a short
    # channel's current does, so kp leans the other way at the hot and cold
    # corners: slow over fast comes out near 2.5, not 5.
    "tt": (0.35, 105e-6, -0.35, 54e-6),
    "ss": (0.37, 136e-6, -0.37, 70e-6),
    "ff": (0.33, 84e-6, -0.33, 43e-6),
}

def spice_models(corner):
    vtn, kpn, vtp, kpp = MODELS[corner]
    _, vdd, temp, _, _ = CORNERS[corner]
    common = "tox=2n gamma=0.3 phi=0.7 lambda=0.1 cgso=0.15n cgdo=0.15n cj=1.2m mj=0.5 cjsw=0.25n mjsw=0.33 pb=0.8"
    return ("* mini45 %s corner: device models and conditions for the cell netlists.\n"
            "* Made up, not a foundry model. Generated by gen_mini45.py; do not edit.\n"
            "* Supply and temperature come from the Liberty corner: %g V, %g C.\n"
            ".model nch nmos level=1 vto=%g kp=%g %s\n"
            ".model pch pmos level=1 vto=%g kp=%g %s\n"
            % (corner, vdd, temp, vtn, kpn, common, vtp, kpp, common))

class Sub:
    """One .subckt: static CMOS gates, transmission gates, tristate inverters."""
    def __init__(self, name, pins):
        self.name, self.pins, self.lines, self.k, self.m = name, pins, [], 0, 0

    def net(self):
        self.k += 1
        return "n%d" % self.k

    def mos(self, kind, d, g, s, w):
        self.m += 1
        w = max(w, 0.12)
        self.lines.append("m%d %s %s %s %s %s w=%su l=%su ad=%sp as=%sp pd=%su ps=%su"
                          % (self.m, d, g, s, "vss" if kind == "n" else "vdd", "nch" if kind == "n" else "pch",
                             fmt(w), fmt(L_GATE), fmt(w * DIFF), fmt(w * DIFF),
                             fmt(2 * (w + DIFF)), fmt(2 * (w + DIFF))))

    def network(self, kind, e, top, bot, w):
        """e is a net name, ('s', ...) in series or ('p', ...) in parallel."""
        if isinstance(e, str):
            self.mos(kind, top, e, bot, w)
        elif e[0] == "s":
            kids = e[1:]
            nodes = [top] + [self.net() for _ in kids[1:]] + [bot]
            for i, kid in enumerate(kids):
                self.network(kind, kid, nodes[i], nodes[i + 1], w * len(kids))
        else:
            for kid in e[1:]:
                self.network(kind, kid, top, bot, w)

    @staticmethod
    def dual(e):
        if isinstance(e, str):
            return e
        return ("p" if e[0] == "s" else "s",) + tuple(Sub.dual(k) for k in e[1:])

    def gate(self, out, pd, size=1.0):
        """out = !(pd): pd is the pull-down network, the pull-up is its dual."""
        self.network("n", pd, out, "vss", WN * size)
        self.network("p", Sub.dual(pd), "vdd", out, WP * size)
        return out

    def inv(self, out, a, size=1.0):
        return self.gate(out, a, size)

    def tgate(self, a, b, gn, gp, size=1.0):
        self.mos("n", a, gn, b, WN * size)
        self.mos("p", a, gp, b, WP * size)

    def tinv(self, out, a, en, enb, size=1.0):
        """Tristate inverter: drives out = !a while en is high (enb low)."""
        x, y = self.net(), self.net()
        self.mos("n", out, en, x, 2 * WN * size)
        self.mos("n", x, a, "vss", 2 * WN * size)
        self.mos("p", out, enb, y, 2 * WP * size)
        self.mos("p", y, a, "vdd", 2 * WP * size)

    def text(self):
        return (".subckt %s %s vdd vss\n" % (self.name.lower(), " ".join(p.lower() for p in self.pins))
                + "".join(l + "\n" for l in self.lines) + ".ends\n")

def cell_netlist(c):
    pins = list(c.inputs) + list(c.outputs)
    x = Sub(c.name, pins)
    d = float(c.drive)
    pre = max(1.0, d / 3.0)              # the stage in front of an output inverter
    k = c.kind
    ins = c.inputs
    S = lambda *a: ("s",) + a
    P = lambda *a: ("p",) + a
    if k in ("INV", "CLKINV"):
        x.inv("Y", "A", d)
    elif k in ("BUF", "CLKBUF"):
        x.inv("Y", x.inv(x.net(), "A", pre), d)
    elif k.startswith("NAND"):
        x.gate("Y", S(*ins), d)
    elif k.startswith("NOR"):
        x.gate("Y", P(*ins), d)
    elif k.startswith("AND"):
        x.inv("Y", x.gate(x.net(), S(*ins), pre), d)
    elif k.startswith("OR"):
        x.inv("Y", x.gate(x.net(), P(*ins), pre), d)
    elif k in ("XOR2", "XNOR2"):
        an, bn = x.inv(x.net(), "A"), x.inv(x.net(), "B")
        core = P(S("A", bn), S(an, "B")) if k == "XOR2" else P(S("A", "B"), S(an, bn))
        x.inv("Y", x.gate(x.net(), core, pre), d)
    elif k == "AOI21":
        x.gate("Y", P(S("A1", "A2"), "B"), d)
    elif k == "AOI22":
        x.gate("Y", P(S("A1", "A2"), S("B1", "B2")), d)
    elif k == "AOI211":
        x.gate("Y", P(S("A1", "A2"), "B", "C"), d)
    elif k == "OAI21":
        x.gate("Y", S(P("A1", "A2"), "B"), d)
    elif k == "OAI22":
        x.gate("Y", S(P("A1", "A2"), P("B1", "B2")), d)
    elif k == "OAI211":
        x.gate("Y", S(P("A1", "A2"), "B", "C"), d)
    elif k == "MUX2":
        sn = x.inv(x.net(), "S")
        x.inv("Y", x.gate(x.net(), P(S("A", sn), S("B", "S")), pre), d)
    elif k == "MUX4":
        s0n, s1n = x.inv(x.net(), "S0"), x.inv(x.net(), "S1")
        lo = x.gate(x.net(), P(S("A", s0n), S("B", "S0")))
        hi = x.gate(x.net(), P(S("C", s0n), S("D", "S0")))
        y = x.gate(x.net(), P(S(lo, s1n), S(hi, "S1")))
        x.inv("Y", x.inv(x.net(), y, pre), d)
    elif k == "HA":
        an, bn = x.inv(x.net(), "A"), x.inv(x.net(), "B")
        x.inv("S", x.gate(x.net(), P(S("A", bn), S(an, "B")), pre), d)
        x.inv("CO", x.gate(x.net(), S("A", "B"), pre), d)
    elif k == "FA":
        con = x.gate(x.net(), P(S("A", "B"), S("CI", P("A", "B"))))
        sn = x.gate(x.net(), P(S("A", "B", "CI"), S(con, P("A", "B", "CI"))))
        x.inv("CO", con, d)
        x.inv("S", sn, d)
    elif k == "TBUF":
        enb = x.inv(x.net(), "EN")
        pg = x.gate(x.net(), S("A", "EN"), pre)
        ng = x.gate(x.net(), P("A", enb), pre)
        x.mos("p", "Y", pg, "vdd", WP * d)
        x.mos("n", "Y", ng, "vss", WN * d)
    elif c.seq and c.seq["type"] == "ff":
        q = c.seq
        din = "D"
        if q["scan"]:
            sen = x.inv(x.net(), "SE")
            din = x.inv(x.net(), x.gate(x.net(), P(S("SI", "SE"), S("D", sen))))
        s2 = x.net()
        if "E" in ins:
            en = x.inv(x.net(), "E")
            din = x.inv(x.net(), x.gate(x.net(), P(S("D", "E"), S(s2, en))))
        ckn = x.inv(x.net(), "CK")
        ckb = x.inv(x.net(), ckn)
        m1, m2, s1 = x.net(), x.net(), x.net()
        r = x.inv(x.net(), "RN") if q["rn"] else None
        s = x.inv(x.net(), "SN") if q["sn"] else None
        # Master, open while CK is low: m1 = D, m2 = !D.
        x.tgate(din, m1, ckn, ckb)
        if r and s:
            x.gate(m2, S(P(m1, s), "RN"))
        elif r:
            x.gate(m2, S(m1, "RN"))
        elif s:
            x.gate(m2, P(m1, s))
        else:
            x.inv(m2, m1)
        x.tinv(m1, m2, ckb, ckn, 0.5)
        # Slave, open while CK is high: s1 = !D, s2 = D.
        x.tgate(m2, s1, ckb, ckn)
        if r and s:
            x.gate(s2, P(S(s1, "SN"), r))
        elif r:
            x.gate(s2, P(s1, r))
        elif s:
            x.gate(s2, S(s1, "SN"))
        else:
            x.inv(s2, s1)
        x.tinv(s1, s2, ckn, ckb, 0.5)
        x.inv("Q", s1, d)
        x.inv("QN", s2, d)
    elif c.seq and c.seq["type"] == "latch":
        e = "E" if "E" in ins else "EN"
        a = x.inv(x.net(), e)
        b = x.inv(x.net(), a)
        on, onb = (b, a) if e == "E" else (a, b)      # on is high while the latch is open
        l1, l2 = x.net(), x.net()
        x.tgate("D", l1, on, onb)
        x.inv(l2, l1)
        x.tinv(l1, l2, onb, on, 0.5)
        x.inv("Q", l2, d)
    elif c.seq and c.seq["type"] == "icg":
        ckn = x.inv(x.net(), "CK")
        ckb = x.inv(x.net(), ckn)
        l1, l2 = x.net(), x.net()
        x.tgate(x.gate(x.net(), P("E", "SE")), l1, ckn, ckb)
        x.inv(l2, l1)
        x.tinv(l1, l2, ckb, ckn, 0.5)
        x.inv("GCK", x.gate(x.net(), S("CK", l2), pre), d)
    elif k == "TIE1":
        g = x.net()
        x.mos("n", g, g, "vss", WN)
        x.mos("p", "Y", g, "vdd", WP)
    elif k == "TIE0":
        g = x.net()
        x.mos("p", g, g, "vdd", WP)
        x.mos("n", "Y", g, "vss", WN)
    return x.text()

def spice():
    s = ("* mini45 transistor netlists: one .subckt per cell, pins in the order of the\n"
         "* Liberty cell (inputs, then outputs), then vdd and vss. Models nch and pch come\n"
         "* from mini45_models_<corner>.sp. Generated by gen_mini45.py; do not edit.\n")
    for c in CELLS:
        s += "\n" + cell_netlist(c)
    return s

# ------------------------------------------------------------ main
def main():
    files = {
        "mini45.v": verilog(),
        "mini45_tech.lef": tech_lef(),
        "mini45.lef": cell_lef(),
        "mini45_tb.sv": testbench(),
        "mini45.sp": spice(),
    }
    for corner in CORNERS:
        files["mini45_models_%s.sp" % corner] = spice_models(corner)
    if "--templates" in sys.argv:
        # The analytic Liberty files: what to measure, for characterize.sh.
        out = sys.argv[sys.argv.index("--templates") + 1]
        os.makedirs(out, exist_ok=True)
        for corner in CORNERS:
            with open(os.path.join(out, "mini45_%s.lib" % corner), "w") as f:
                f.write(liberty(corner))
        print("mini45: %d template libraries in %s" % (len(CORNERS), out))
        return
    check = "--check" in sys.argv
    stale = 0
    for name, text in sorted(files.items()):
        path = os.path.join(HERE, name)
        if check:
            old = open(path).read() if os.path.exists(path) else None
            if old != text:
                print("out of date: %s (run gen_mini45.py)" % name)
                stale = 1
        else:
            with open(path, "w") as f:
                f.write(text)
    if not check:
        print("mini45: %d cells, %d files" % (len(CELLS), len(files)))
    sys.exit(stale)

if __name__ == "__main__":
    main()
