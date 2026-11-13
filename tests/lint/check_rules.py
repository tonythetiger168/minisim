#!/usr/bin/env python3
"""mlint rule tests.

    check_rules.py <mlint> <tests/lint dir>

Each tests/lint/rules/*.sv (and *.vhd, with -- for //) says how to run mlint on it ("//! args: ...") and
marks every line that must get a finding ("//~ RULE [RULE ...]", or
"//~v RULE" on the line above it). The run
must report exactly the marked (line, rule) pairs: a missing one is a rule
that stopped working, an extra one is a false positive. The same file is run
with -threads 1 and -threads 4, and the two outputs must be identical.
"""
import json
import os
import re
import subprocess
import sys
import tempfile

mlint, here = sys.argv[1], sys.argv[2]
FINDING = re.compile(r"^(.*?):(\d+):(\d+): (warning|error|info|waived): \[([A-Z]+\d+)\] (.*)$")
failed = 0


def run(args, cwd=None):
    p = subprocess.run([mlint] + args, capture_output=True, text=True, cwd=cwd)
    return p.returncode, p.stdout, p.stderr


def fail(what):
    global failed
    failed += 1
    print("FAIL:", what)


def findings(out):
    return [m.groups() for m in (FINDING.match(l) for l in out.splitlines()) if m]


rules_dir = os.path.join(here, "rules")
tested = set()
# The TIDY rules are a build option (MINISIM_WITH_SLANG_TIDY).
have_tidy = "TIDY001" in run(["-list"])[1]
for name in sorted(os.listdir(rules_dir)):
    if not name.endswith((".sv", ".vhd")):
        continue
    if name == "tidy.sv" and not have_tidy:
        continue
    path = os.path.join(rules_dir, name)
    text = open(path).read()
    args = [x.replace("$DIR", here) for x in re.search(r"^(?://|--)! args: (.*)$", text, re.M).group(1).split()]
    want = []
    for n, line in enumerate(text.splitlines(), 1):
        m = re.search(r"(?://|--)~(v?) (.*)$", line)
        if m:  # "//~v RULE" marks the line below, for rules about the end of a line
            want += [(n + (m.group(1) == "v"), r) for r in m.group(2).split()]
    rc, out, err = run([path] + args + ["-threads", "1"])
    if rc not in (0, 1) or "error:" in err:
        fail(f"{name}: mlint exited {rc}\n{out}{err}")
        continue
    got = sorted((int(f[1]), f[4]) for f in findings(out))
    tested |= {r for _, r in got}
    if got != sorted(want):
        missing = sorted(set(want) - set(got))
        extra = sorted(set(got) - set(want))
        fail(f"{name}: missing {missing}, unexpected {extra}\n{out}")
    rc4, out4, _ = run([path] + args + ["-threads", "4"])
    if out4 != out or rc4 != rc:
        fail(f"{name}: -threads 4 differs from -threads 1")

# Every rule in the table is exercised by some file (CDC has its own test).
rc, out, _ = run(["-list"])
listed = [l.split()[0] for l in out.splitlines() if l.strip()]
untested = [r for r in listed if r not in tested and not r.startswith(("CDC", "RDC", "UPF"))]
if untested:
    fail(f"rules with no test: {untested}")
for l in out.splitlines():
    if len(l.split(None, 4)) < 5:
        fail(f"-list: malformed line {l!r}")

# ------------------------------------------------------- configuration, waivers
cfgdir = os.path.join(here, "config")
dut = os.path.join(cfgdir, "dut.sv")

rc, out, _ = run([dut, "-top", "dut"])
base = findings(out)
if rc != 0 or sorted(f[4] for f in base) != ["SYN002", "UNU002", "UNU004", "UNU004", "W001"]:
    fail(f"config/dut.sv baseline:\n{out}")
if "waived" in out:
    fail("baseline mentions waivers")

# Comments in the source: this line, the next line, a region, and a rule list.
rc, out, _ = run([os.path.join(cfgdir, "inline.sv"), "-top", "inl", "-show-waived"])
fs = findings(out)
live = sorted((int(f[1]), f[4]) for f in fs if f[3] != "waived")
waived = sorted((int(f[1]), f[4]) for f in fs if f[3] == "waived")
if live != [(13, "W001")] or waived != [(5, "W001"), (7, "W001"), (9, "W001"), (10, "W001"), (15, "W001"), (15, "W004")]:
    fail(f"inline waivers: live {live}, waived {waived}\n{out}")
if "intended truncation" not in out or "1 finding (W001 1), 6 waived" not in out:
    fail(f"inline waivers: reason or summary missing\n{out}")

# A waiver file: by rule and file, by message, by line; one that matches nothing.
rc, out, _ = run([dut, "-top", "dut", "-waivers", os.path.join(cfgdir, "waivers.cfg"), "-Werror"])
if rc != 0 or "0 findings, 5 waived" not in out or "waivers.cfg:5: this waiver matched nothing" not in out:
    fail(f"waiver file: rc={rc}\n{out}")

# A configuration file: enable a group that is off, set its pattern, raise a
# severity (an error fails the run without -Werror), turn a rule off.
rc, out, _ = run([dut, "-top", "dut", "-config", os.path.join(cfgdir, "mlint.cfg")])
fs = findings(out)
if rc != 1 or not any(f[3] == "error" and f[4] == "W001" for f in fs):
    fail(f"config: W001 should be an error and exit 1 (rc={rc})\n{out}")
if any(f[4] == "UNU002" for f in fs) or not any(f[4] == "NAME002" and "'Tmp'" in f[5] for f in fs):
    fail(f"config: disable / enable / set did not take\n{out}")

# The same from the command line, and -only.
rc, out, _ = run([dut, "-top", "dut", "-only", "W001", "-severity", "W001=info", "-Werror"])
fs = findings(out)
if rc != 0 or [(f[3], f[4]) for f in fs] != [("info", "W001")]:
    fail(f"-only / -severity: rc={rc}\n{out}")
rc, out, _ = run([dut, "-top", "dut", "-Werror", "-quiet"])
if rc != 1 or "findings" in out:
    fail(f"-Werror -quiet: rc={rc}\n{out}")
rc, out, _ = run([dut, "-top", "dut", "-disable", "width,unused,synth", "-Werror"])
if rc != 0 or "0 findings" not in out:
    fail(f"-disable groups: rc={rc}\n{out}")

# Mistakes are reported, not ignored.
for bad in (["-enable", "NOPE1"], ["-severity", "W001=loud"], ["-set", "name.modul=x"], ["-set", "name.module=("]):
    rc, out, err = run([dut, "-top", "dut"] + bad)
    if rc != 2 or "mlint:" not in err:
        fail(f"{bad}: expected exit 2 and a message, got {rc}\n{err}")

# JSON carries everything, waived findings too.
with tempfile.TemporaryDirectory() as tmp:
    js = os.path.join(tmp, "out.json")
    run([os.path.join(cfgdir, "inline.sv"), "-top", "inl", "-json", js])
    j = json.load(open(js))
    if j["summary"] != {"error": 0, "warning": 1, "info": 0, "waived": 6} or len(j["findings"]) != 7:
        fail(f"json: {j['summary']}")
    if not all({"file", "line", "col", "rule", "group", "severity", "text", "waived"} <= set(f) for f in j["findings"]):
        fail("json: a finding lacks a field")

rc, out, _ = run(["-explain", "RST003"])
if rc != 0 or "reset" not in out:
    fail(f"-explain: {out}")

if failed:
    sys.exit(1)
print(f"mlint rules: ok ({len(tested)} rules exercised)")
