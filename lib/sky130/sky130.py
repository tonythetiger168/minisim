#!/usr/bin/env python3
"""The SkyWater sky130 high-density cell library as a second test library.

    sky130.py fetch [DIR]     download the full Liberty and LEF files (pinned
                              commits, checked by SHA-256) into DIR
                              (default: lib/sky130/pdk, which git ignores)
    sky130.py subset DIR      cut the cells in CELLS out of the files in DIR
                              and write the small files this directory keeps
    sky130.py --check DIR     exit 1 if the kept files are not what `subset`
                              would write from DIR

The files are the work of the SkyWater PDK authors under Apache-2.0 (see
LICENSE and README.md here). Nothing is generated or changed: `subset` copies
whole cell groups and macros, byte for byte, and says so in a header.
"""
import hashlib
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
LIBS = "https://raw.githubusercontent.com/efabless/skywater-pdk-libs-sky130_fd_sc_hd/5a42fd01a0eeb2338340438443c7062e198d97a4/"
ORFS = "https://raw.githubusercontent.com/The-OpenROAD-Project/OpenROAD-flow-scripts/bfcc1895a7a1d9ff6e677066825812e549b72421/flow/platforms/sky130hd/"

# name in DIR -> (url, sha256)
FILES = {
    "sky130_fd_sc_hd__tt_025C_1v80.lib": (LIBS + "timing/sky130_fd_sc_hd__tt_025C_1v80.lib", "8e78e14442062dba34d414fca6490b2f6b96038d4510d1438ca44fee31487135"),
    "sky130_fd_sc_hd__ss_100C_1v60.lib": (LIBS + "timing/sky130_fd_sc_hd__ss_100C_1v60.lib", "9b24f0db3967ac67b4cae1f74bb480fd47922d18d0ed577e3c121739b412c361"),
    "sky130_fd_sc_hd__ff_n40C_1v95.lib": (LIBS + "timing/sky130_fd_sc_hd__ff_n40C_1v95.lib", "fb61d91c55a7f85b1989e8149d040b13ba5e7f5478f5bf0c3c16c5da30720139"),
    "sky130_fd_sc_hd_merged.lef": (ORFS + "lef/sky130_fd_sc_hd_merged.lef", "44180aaa0068b1fcb0789f128b090304c01ad48f511d3f7afa66c90b3e176ec5"),
    "sky130_fd_sc_hd.tlef": (ORFS + "lef/sky130_fd_sc_hd.tlef", "8e99b4e8b016db0713029ebcae6b2cc2aedd9c2c49682e2f23521dd0b1a2085e"),
}

# The cells the subset keeps: enough for synthesis, timing, DFT and
# place-and-route tests, a few kB each.
CELLS = """
inv_1 inv_2 inv_4 buf_1 buf_2 buf_4 clkbuf_1 clkbuf_4 clkinv_1
nand2_1 nand2_2 nand3_1 nand4_1 nor2_1 nor2_2 nor3_1 nor4_1
and2_1 and3_1 or2_1 or3_1 xor2_1 xnor2_1
a21oi_1 a22oi_1 o21ai_1 o22ai_1 a21o_1 o21a_1 mux2_1 mux4_1 ha_1 fa_1
dfxtp_1 dfxtp_2 dfrtp_1 dfstp_1 dfbbp_1 sdfxtp_1 sdfrtp_1 dlxtp_1 dlclkp_1
ebufn_1 einvp_1 conb_1 fill_1 fill_2 fill_4 decap_4 tapvpwrvgnd_1 diode_2
""".split()
PREFIX = "sky130_fd_sc_hd__"

HEADER = """A subset of %s from the SkyWater sky130 PDK
(Copyright 2020 The SkyWater PDK Authors, Apache-2.0; see LICENSE and README.md
in this directory). Cut by lib/sky130/sky130.py: %d of the library's cells,
each copied whole and unchanged, with the library's own header. Not for
tape-out: use the PDK."""


def fetch(dest):
    os.makedirs(dest, exist_ok=True)
    for name, (url, sha) in FILES.items():
        path = os.path.join(dest, name)
        if not (os.path.exists(path) and digest(path) == sha):
            print("fetching", name)
            urllib.request.urlretrieve(url, path)
        if digest(path) != sha:
            sys.exit("sky130: %s is not the pinned file (SHA-256 %s)" % (name, digest(path)))
    print("sky130: %d files in %s" % (len(FILES), dest))


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def cut_liberty(text, keep):
    """The library header, the cell groups in `keep`, the closing brace."""
    starts = [(m.start(), m.group(1)) for m in re.finditer(r'^    cell \("([^"]+)"\) \{', text, re.M)]
    end = text.rstrip().rfind("}")
    out = [text[:starts[0][0]]]
    found = []
    for i, (pos, name) in enumerate(starts):
        if name in keep:
            out.append(text[pos:starts[i + 1][0] if i + 1 < len(starts) else end])
            found.append(name)
    out.append("}\n")
    return "".join(out), found


def cut_lef(text, keep):
    first = re.search(r"^MACRO ", text, re.M).start()
    out = [text[:first]]
    found = []
    for m in re.finditer(r"^MACRO (\S+)\n.*?^END \1\n", text, re.M | re.S):
        if m.group(1) in keep:
            out.append(m.group(0) + "\n")
            found.append(m.group(1))
    out.append("END LIBRARY\n")
    return "".join(out), found


def subset(src):
    """file name here -> contents."""
    keep = {PREFIX + c for c in CELLS}
    out = {}
    lib = open(os.path.join(src, "sky130_fd_sc_hd__tt_025C_1v80.lib")).read()
    body, found = cut_liberty(lib, keep)
    inlib = set(found)
    note = HEADER % ("sky130_fd_sc_hd__tt_025C_1v80.lib", len(found))
    out["sky130_hd_subset_tt.lib"] = "/*\n" + note + "\n*/\n" + body
    lef = open(os.path.join(src, "sky130_fd_sc_hd_merged.lef")).read()
    body, found = cut_lef(lef, keep)
    # Fill and tap cells are physical only: they have a macro and no timing.
    missing = keep - (inlib & set(found)) - {c for c in set(found) - inlib if re.search("__(fill|tap|decap)", c)}
    if missing:
        sys.exit("sky130: not in both the Liberty and the LEF file: " + " ".join(sorted(missing)))
    note = HEADER % ("sky130_fd_sc_hd_merged.lef", len(found))
    out["sky130_hd_subset.lef"] = "".join("# " + l + "\n" for l in note.split("\n")) + body
    out["sky130_hd.tlef"] = open(os.path.join(src, "sky130_fd_sc_hd.tlef")).read()
    return out


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return
    cmd = args[0]
    d = args[1] if len(args) > 1 else os.path.join(HERE, "pdk")
    if cmd == "fetch":
        fetch(d)
    elif cmd == "subset":
        for name, text in subset(d).items():
            with open(os.path.join(HERE, name), "w") as f:
                f.write(text)
            print("wrote %s (%d kB)" % (name, len(text) // 1024))
    elif cmd == "--check":
        bad = [n for n, text in subset(d).items()
               if not os.path.exists(os.path.join(HERE, n)) or open(os.path.join(HERE, n)).read() != text]
        if bad:
            sys.exit("sky130: out of date: " + " ".join(bad))
        print("sky130: the subset matches")
    else:
        sys.exit("sky130.py: unknown command " + cmd)


if __name__ == "__main__":
    main()
