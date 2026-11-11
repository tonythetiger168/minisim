#!/usr/bin/env python3
"""mfpga test: map one design for every device family and check the result.

  run_test.py MFPGA MSIM SRC_DIR OUT_DIR DESIGN [seq-depth [reset-input [mfpga-option...]]]

DESIGN is tests/synth/DESIGN.sv (the designs msynth is tested with),
tests/fpga/DESIGN.sv or tests/fpga/DESIGN.aag. Checks:
  1. mfpga -verify: the mapped netlist equals the graph on random patterns.
  2. One thread and four threads write the same bytes.
  3. RTL designs: the design's testbench (DESIGN_tb.sv beside it) prints
     the same lines with msim on the RTL and on the mapped Verilog netlist
     with the cell models of tests/fpga/cells_FAMILY.v.
  4. With yosys on PATH and a seq-depth: the JSON netlist, on the vendor's
     own simulation models, equals the RTL for seq-depth cycles from reset
     (bounded SAT on a miter). This check shares no code with mfpga. With a
     reset-input the proof holds that input at 1 in the first cycle: a flop
     with a synchronous set powers up 1 on ecp5 and 0 in the RTL's proof.
     iCE40 also runs on the vendor's simulation models (Yosys' ice40/cells_sim.v), when installed.
  5. With nextpnr-ice40 / nextpnr-ecp5 on PATH: the netlist places and routes.
"""
import os, shutil, subprocess, sys

mfpga, msim, src, out, design = sys.argv[1:6]
depth = int(sys.argv[6]) if len(sys.argv) > 6 else 0
reset = sys.argv[7] if len(sys.argv) > 7 and sys.argv[7] != "-" else None
extra = sys.argv[8:]
# not an mfpga option: the design, as LUTs, is larger than the HX8K the iCE40 netlist is placed on
skip_ice40_pnr = "--no-ice40-pnr" in extra
extra = [e for e in extra if e != "--no-ice40-pnr"]
os.makedirs(out, exist_ok=True)
rtl = os.path.join(src, "tests", "synth", design + ".sv")
if not os.path.exists(rtl):
    rtl = os.path.join(src, "tests", "fpga", design + ".sv")
aag = os.path.join(src, "tests", "fpga", design + ".aag")
inp = ([rtl, "-top", design] if os.path.exists(rtl) else [aag, "-top", design]) + extra

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit("FAILED: %s\n%s%s" % (" ".join(cmd), r.stdout[-3000:], r.stderr[-3000:]))
    return r.stdout

SIM = {"ice40": "+/ice40/cells_sim.v", "ecp5": "+/ecp5/cells_sim.v", "xc7": "+/xilinx/cells_sim.v", "generic": None}
yosys = shutil.which("yosys") if depth and os.path.exists(rtl) else None
datdir = os.path.join(os.path.dirname(os.path.realpath(shutil.which("yosys"))), "..", "share", "yosys") if shutil.which("yosys") else ""

for fam in ("generic", "ice40", "ecp5", "xc7"):
    base = os.path.join(out, "%s_%s" % (design, fam))
    rep = run([mfpga] + inp + ["-family", fam, "-verify", "-threads", "1", "-o", base + ".json", "-edif", base + ".edf",
                                "-verilog", base + ".v"])
    if "verify   passed" not in rep:
        sys.exit("no verify line:\n" + rep)
    run([mfpga] + inp + ["-family", fam, "-threads", "4", "-q", "-o", base + ".t4.json", "-edif", base + ".t4.edf"])
    for ext in (".json", ".edf"):
        if open(base + ext, "rb").read() != open(base + ".t4" + ext, "rb").read():
            sys.exit("%s%s differs between 1 and 4 threads" % (base, ext))
    edif = open(base + ".edf").read()
    if edif.count("(") != edif.count(")") or not edif.startswith("(edif "):
        sys.exit("malformed EDIF " + base + ".edf")
    if os.path.exists(rtl):
        tb = rtl[:-3] + "_tb.sv"
        cells = [os.path.join(src, "tests", "fpga", "cells_%s.v" % fam)]
        if fam == "ecp5":
            cells.append(os.path.join(src, "tests", "fpga", "cells_ecp5_dsp.v"))
        want = run([msim, rtl, tb, "-top", design + "_tb"])
        got = run([msim, base + ".v"] + cells + [tb, "-top", design + "_tb"])
        if want != got or len(want.splitlines()) < 50:
            sys.exit("%s: the netlist simulates differently from the RTL (%d lines)" % (fam, len(want.splitlines())))
        # iCE40: once more on the vendor's own simulation models, which Yosys ships
        vendor = os.path.join(datdir, "ice40", "cells_sim.v") if datdir and fam == "ice40" else ""
        if os.path.exists(vendor):
            got = run([msim, base + ".v", vendor, tb, "-top", design + "_tb"])
            if want != got:
                sys.exit("ice40: the netlist on the vendor's simulation models differs from the RTL")
    if yosys:
        ys = ["read_verilog -sv " + rtl, "prep -top " + design, "memory_map", "design -stash gold"]
        if SIM[fam]:
            ys.append("read_verilog " + SIM[fam])
        if fam == "ecp5":   # a black box in Yosys 0.33's library
            ys.append("read_verilog " + os.path.join(src, "tests", "fpga", "cells_ecp5_dsp.v"))
        ys += ["read_json %s.json" % base, "hierarchy -top " + design, "proc", "flatten", "memory", "design -stash gate",
               "design -copy-from gold -as gold " + design, "design -copy-from gate -as gate " + design,
               "miter -equiv -flatten -make_outputs gold gate miter", "hierarchy -top miter", "flatten", "opt",
               # an asynchronous reset becomes a synchronous one for the bounded check
               "async2sync",
               "sat -verify -prove trigger 0 -seq %d -set-init-zero %smiter" % (depth, "-set-at 1 in_%s 1 -prove-skip 1 " % reset if reset else "")]
        r = subprocess.run([yosys, "-q", "-p", "; ".join(ys)], capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            sys.exit("FAILED: %s: the yosys proof against the RTL\n%s%s" % (fam, r.stdout[-3000:], r.stderr[-3000:]))
    pnr = {"ice40": ["nextpnr-ice40", "--hx8k", "--package", "ct256", "--pcf-allow-unconstrained", "--asc", base + ".asc"],
           "ecp5": ["nextpnr-ecp5", "--85k", "--package", "CABGA756", "--textcfg", base + ".cfg"]}.get(fam)
    if pnr and shutil.which(pnr[0]) and not (fam == "ice40" and skip_ice40_pnr):
        run(pnr + ["--json", base + ".json", "-q", "--placer", "heap"], timeout=900)
print("ok")
