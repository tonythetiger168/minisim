#!/usr/bin/env python3
"""Writes tests/pnr/tiny.lef: a small made-up standard cell library for the
mpnr tests. Six routing layers, one site, a handful of gates whose pins are
metal1 bars centred on metal2 tracks.

    python3 tests/pnr/gen_lib.py > tests/pnr/tiny.lef
"""
SITE_W, ROW_H = 0.19, 1.40
LAYERS = [  # name, dir, pitch, width, spacing, r/sq, c/um2, edge c
    ("m1", "HORIZONTAL", 0.14, 0.07, 0.07, 0.38, 7.7e-5, 2.7e-5),
    ("m2", "VERTICAL", 0.19, 0.07, 0.07, 0.25, 4.0e-5, 2.5e-5),
    ("m3", "HORIZONTAL", 0.14, 0.07, 0.07, 0.25, 2.7e-5, 2.5e-5),
    ("m4", "VERTICAL", 0.28, 0.14, 0.14, 0.21, 2.0e-5, 2.4e-5),
    ("m5", "HORIZONTAL", 0.28, 0.14, 0.14, 0.21, 1.6e-5, 2.4e-5),
    ("m6", "VERTICAL", 0.28, 0.14, 0.14, 0.21, 1.3e-5, 2.4e-5),
]
CELLS = [  # name, width in sites, pins (name, direction, track)
    ("INV", 2, [("A", "INPUT", 0), ("Y", "OUTPUT", 1)]),
    ("BUF", 2, [("A", "INPUT", 0), ("Y", "OUTPUT", 1)]),
    ("NAND2", 3, [("A", "INPUT", 0), ("B", "INPUT", 1), ("Y", "OUTPUT", 2)]),
    ("NOR2", 3, [("A", "INPUT", 0), ("B", "INPUT", 1), ("Y", "OUTPUT", 2)]),
    ("AND2", 4, [("A", "INPUT", 0), ("B", "INPUT", 1), ("Y", "OUTPUT", 3)]),
    ("OR2", 4, [("A", "INPUT", 0), ("B", "INPUT", 1), ("Y", "OUTPUT", 3)]),
    ("XOR2", 5, [("A", "INPUT", 0), ("B", "INPUT", 2), ("Y", "OUTPUT", 4)]),
    ("XNOR2", 5, [("A", "INPUT", 0), ("B", "INPUT", 2), ("Y", "OUTPUT", 4)]),
    ("NAND3", 4, [("A", "INPUT", 0), ("B", "INPUT", 1), ("C", "INPUT", 2), ("Y", "OUTPUT", 3)]),
    ("NOR3", 4, [("A", "INPUT", 0), ("B", "INPUT", 1), ("C", "INPUT", 2), ("Y", "OUTPUT", 3)]),
    ("MUX2", 6, [("A", "INPUT", 0), ("B", "INPUT", 1), ("S", "INPUT", 3), ("Y", "OUTPUT", 5)]),
    ("AOI21", 4, [("A1", "INPUT", 0), ("A2", "INPUT", 1), ("B", "INPUT", 2), ("Y", "OUTPUT", 3)]),
    ("OAI21", 4, [("A1", "INPUT", 0), ("A2", "INPUT", 1), ("B", "INPUT", 2), ("Y", "OUTPUT", 3)]),
    ("DFF", 8, [("D", "INPUT", 0), ("CK", "INPUT", 2), ("Q", "OUTPUT", 7)]),
    ("DFFR", 9, [("D", "INPUT", 0), ("CK", "INPUT", 2), ("RN", "INPUT", 4), ("Q", "OUTPUT", 8)]),
    ("TIE0", 2, [("Y", "OUTPUT", 1)]),
    ("TIE1", 2, [("Y", "OUTPUT", 1)]),
]
FILLERS = [("FILL1", 1), ("FILL2", 2), ("FILL4", 4)]

def f(v):
    return ("%.4f" % v).rstrip("0").rstrip(".")

out = []
w = out.append
w("# A made-up 45 nm style library for the mpnr tests. Written by gen_lib.py.")
w('VERSION 5.8 ;\nBUSBITCHARS "[]" ;\nDIVIDERCHAR "/" ;')
w("UNITS\n  DATABASE MICRONS 2000 ;\nEND UNITS\nMANUFACTURINGGRID 0.005 ;")
for i, (name, d, pitch, width, sp, r, c, ec) in enumerate(LAYERS):
    w(f"LAYER {name}\n  TYPE ROUTING ;\n  DIRECTION {d} ;\n  PITCH {f(pitch)} ;\n  OFFSET 0.095 0.07 ;\n  WIDTH {f(width)} ;\n"
      f"  SPACING {f(sp)} ;\n  RESISTANCE RPERSQ {r} ;\n  CAPACITANCE CPERSQDIST {c} ;\n  EDGECAPACITANCE {ec} ;\n"
      f"  THICKNESS {f(0.13 + 0.03 * i)} ;\n  HEIGHT {f(0.37 + 0.3 * i)} ;\nEND {name}")
    if i + 1 < len(LAYERS):
        cut = width
        w(f"LAYER v{i + 1}\n  TYPE CUT ;\n  WIDTH {f(cut)} ;\n  SPACING {f(cut + 0.01)} ;\n  RESISTANCE {5 - 0.5 * i} ;\nEND v{i + 1}")
for i in range(len(LAYERS) - 1):
    lo, hi = LAYERS[i], LAYERS[i + 1]
    cut = lo[3] / 2
    def enc(layer):   # the pad is longer along the layer's own direction
        return (cut + 0.035, cut) if layer[1] == "HORIZONTAL" else (cut, cut + 0.035)
    (bx, by), (tx, ty) = enc(lo), enc(hi)
    w(f"VIA via{i + 1}{i + 2} DEFAULT\n  LAYER {lo[0]} ;\n    RECT {f(-bx)} {f(-by)} {f(bx)} {f(by)} ;\n"
      f"  LAYER v{i + 1} ;\n    RECT {f(-cut)} {f(-cut)} {f(cut)} {f(cut)} ;\n"
      f"  LAYER {hi[0]} ;\n    RECT {f(-tx)} {f(-ty)} {f(tx)} {f(ty)} ;\nEND via{i + 1}{i + 2}")
# The cell pins are vertical metal1 bars, so the via onto them is tall on metal1.
w("VIA via12_pin DEFAULT\n  LAYER m1 ;\n    RECT -0.035 -0.07 0.035 0.07 ;\n  LAYER v1 ;\n    RECT -0.035 -0.035 0.035 0.035 ;\n"
  "  LAYER m2 ;\n    RECT -0.035 -0.07 0.035 0.07 ;\nEND via12_pin")
w(f"SITE core\n  SYMMETRY Y ;\n  CLASS CORE ;\n  SIZE {f(SITE_W)} BY {f(ROW_H)} ;\nEND core")

def rails(width):
    return (f"  PIN VDD\n    DIRECTION INOUT ;\n    USE POWER ;\n    SHAPE ABUTMENT ;\n    PORT\n      LAYER m1 ;\n"
            f"        RECT 0 {f(ROW_H - 0.085)} {f(width)} {f(ROW_H + 0.085)} ;\n    END\n  END VDD\n"
            f"  PIN VSS\n    DIRECTION INOUT ;\n    USE GROUND ;\n    SHAPE ABUTMENT ;\n    PORT\n      LAYER m1 ;\n"
            f"        RECT 0 -0.085 {f(width)} 0.085 ;\n    END\n  END VSS")

for name, sites, pins in CELLS:
    width = sites * SITE_W
    w(f"MACRO {name}\n  CLASS CORE ;\n  ORIGIN 0 0 ;\n  SIZE {f(width)} BY {f(ROW_H)} ;\n  SYMMETRY X Y ;\n  SITE core ;")
    for pn, d, track in pins:
        x = 0.095 + SITE_W * track
        w(f"  PIN {pn}\n    DIRECTION {d} ;\n    USE SIGNAL ;\n    PORT\n      LAYER m1 ;\n"
          f"        RECT {f(x - 0.035)} 0.28 {f(x + 0.035)} 1.12 ;\n    END\n  END {pn}")
    w(rails(width))
    w(f"END {name}")
for name, sites in FILLERS:
    width = sites * SITE_W
    w(f"MACRO {name}\n  CLASS CORE SPACER ;\n  ORIGIN 0 0 ;\n  SIZE {f(width)} BY {f(ROW_H)} ;\n  SYMMETRY X Y ;\n  SITE core ;")
    w(rails(width))
    w(f"END {name}")
w("END LIBRARY")
print("\n".join(out))
