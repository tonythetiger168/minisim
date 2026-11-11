#!/usr/bin/env python3
"""check.py <mspice> <netlist> <expected> [mspice options]: the output must equal the expected file."""
import subprocess
import sys

out = subprocess.run([sys.argv[1], sys.argv[2]] + sys.argv[4:], capture_output=True, text=True)
want = open(sys.argv[3]).read()
if out.returncode != 0 or out.stdout != want:
    sys.stdout.write(out.stdout + out.stderr)
    sys.exit("FAIL: output differs from " + sys.argv[3])
print("PASS")
