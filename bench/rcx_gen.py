#!/usr/bin/env python3
"""Write a routed DEF of any size for timing mrcx (uses tests/rcx/demo.lef).

  python3 bench/rcx_gen.py 200000 > big.def
  mrcx -lef tests/rcx/demo.lef -def big.def -o big.spef

Every net is a metal1 wire on its own stretch of a track, a via, and a metal2
wire up to three tracks long, so each has neighbours on both layers and
crossings, and no two nets touch. The layout is the same for a given size.
"""
import random
import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
rnd = random.Random(1)
pitch = 200
cols = max(64, int((n * 14) ** 0.5))      # tracks across; about 14 columns per net
rows = n * 14 // cols + 8
out = []
count = 0
for r in range(rows):
    x = rnd.randrange(0, 6)
    y = r * pitch
    while count < n:
        length = rnd.randrange(4, 22)
        if x + length >= cols:
            break
        # A column this row may use for its via: one in four, by row.
        c = x + rnd.randrange(0, length)
        c += (r - c) % 4
        if c > x + length:
            c -= 4
        up = rnd.randrange(1, 4)
        s = "  - n%d + USE SIGNAL\n    + ROUTED metal1 ( %d %d ) ( %d * )" % (count, x * pitch, y, (x + length) * pitch)
        if c >= x:
            s += "\n      NEW metal2 ( %d %d ) ( * %d ) via1_0\n      NEW metal2 ( %d %d ) via1_0" % (
                c * pitch, y, (r + up) * pitch, c * pitch, y)
        out.append(s + " ;")
        count += 1
        x += length + rnd.randrange(1, 4)
print("VERSION 5.8 ;\nDIVIDERCHAR \"/\" ;\nBUSBITCHARS \"[]\" ;\nDESIGN bench ;\nUNITS DISTANCE MICRONS 1000 ;")
print("DIEAREA ( 0 0 ) ( %d %d ) ;" % (cols * pitch, (rows + 4) * pitch))
print("NETS %d ;" % len(out))
print("\n".join(out))
print("END NETS\nEND DESIGN")
