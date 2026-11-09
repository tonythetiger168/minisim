#!/usr/bin/env python3
"""mregress: run a regression on msim, VCS, Xcelium, Questa, or Icarus.

    mregress tests.list [-sim msim] [-j 8] [-seeds 3] [-o regress.d]
    mregress -spec tests/spec -sim iverilog

A test list has one test per line, ``name: <simulator arguments>``, plus
optional ``@key=value`` settings on the same line:

    # comment
    %sim msim                   default simulator (the -sim option wins)
    %common +define+FAST        arguments added to every test
    -f more.list                include another list (relative to this one)
    adder: rtl/adder.sv tb/adder_tb.sv -top adder_tb @seeds=4 @timeout=60
    smoke: tb.sv @expect=inline @pass=TEST PASSED

Settings: ``@seeds=N`` (run N seeds), ``@seed=S`` (one fixed seed),
``@timeout=SEC``, ``@expect=inline`` (stdout must equal the ``//> `` lines
of the first .sv file, as in tests/spec) or ``@expect=<file>``,
``@skip=sim[,sim]`` (not run on those simulators; a spec test's
``//! icarus-skip`` line skips iverilog), ``@exit=N`` (expected exit status),
``@xfail=<reason>`` (a known gap: failing is expected, passing is an error;
a spec test's ``//! xfail:`` and ``//! exit:`` lines apply to msim), ``@pass=<regex>`` (a line must match), ``@fail=<regex>`` (no line may match;
added to the built-in error patterns). Text after ``@pass=`` / ``@fail=``
runs to the end of the line.

A run passes when the simulator exits 0, no line matches an error pattern
(UVM_ERROR / UVM_FATAL counts above zero, ``Error-``, ``*E,``, ``** Error``,
``$fatal``), and the @pass / @expect checks hold. Each run gets its own
directory with ``cmd.sh`` and ``sim.log``. Results go to ``results.json``,
``junit.xml`` and ``report.html`` in the output directory, and failures are
grouped by their first error line. Exit status: 0 all passed, 1 some failed,
2 bad usage.
"""

import argparse
import concurrent.futures as cf
import glob
import html
import json
import os
import random
import re
import shlex
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

VERSION = "0.1.0"

# Lines that mean a failure even when the simulator exits 0.
ERROR_PATTERNS = [
    r"^UVM_(ERROR|FATAL)\s*:\s*[1-9]",   # UVM report summary counts
    r"^UVM_FATAL\b",                     # a fatal message itself
    r"^Error-\[",                        # VCS
    r"^\*E,",                            # Xcelium
    r"^\*\* (Error|Fatal)",              # Questa
    r"^(Error|Fatal):",                  # Questa / generic
    r"\$fatal\b",
    r"^msim: .*(error|cannot)",
]


class Test:
    def __init__(self, name, args, opts, base):
        self.name, self.args, self.opts, self.base = name, args, opts, base


def parse_list(path, tests, settings, seen=None):
    seen = seen if seen is not None else set()
    path = os.path.abspath(path)
    if path in seen:
        raise SystemExit(f"mregress: {path} includes itself")
    seen.add(path)
    base = os.path.dirname(path)
    with open(path, encoding="utf-8") as f:
        for no, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith("//"):
                continue
            if line.startswith("%sim "):
                settings.setdefault("sim", line.split(None, 1)[1].strip())
                continue
            if line.startswith("%common "):
                settings.setdefault("common", []).extend(shlex.split(line.split(None, 1)[1]))
                continue
            if line.startswith("-f "):
                parse_list(os.path.join(base, line[3:].strip()), tests, settings, seen)
                continue
            m = re.match(r"^([A-Za-z0-9_.\-/]+)\s*:\s*(.*)$", line)
            if not m:
                raise SystemExit(f"mregress: {path}:{no}: expected 'name: arguments'")
            name, rest = m.group(1), m.group(2)
            opts = {}
            # @pass= / @fail= / @xfail= take the text up to the next of them
            # (or the end of the line), so they can hold spaces.
            while True:
                found = list(re.finditer(r"(?:^|\s)@(pass|fail|xfail)=", rest))
                if not found:
                    break
                mm = found[-1]
                opts[mm.group(1)] = rest[mm.end():].strip()
                rest = rest[: mm.start()]
            args = []
            for tok in shlex.split(rest):
                if tok.startswith("@") and "=" in tok:
                    k, v = tok[1:].split("=", 1)
                    opts[k] = v
                else:
                    args.append(tok)
            tests.append(Test(name, args, opts, base))


def spec_tests(directory):
    tests = []
    for sv in sorted(glob.glob(os.path.join(directory, "*.sv"))):
        name = os.path.splitext(os.path.basename(sv))[0]
        opts = {"expect": "inline"}
        # Markers shared with tests/spec/run_spec.cmake and the spec suite:
        # icarus-skip, and (msim only) xfail: <known gap> and exit: <status>.
        with open(sv, encoding="utf-8", errors="replace") as f:
            for l in f:
                m = re.match(r"//! ([a-z-]+): ?(.*)", l.rstrip("\n"))
                if not m:
                    continue
                if m.group(1) == "icarus-skip":
                    opts["skip"] = "iverilog"
                elif m.group(1) == "xfail":
                    opts["xfail"], opts["xfail-sims"] = m.group(2) or "known gap", "msim"
                elif m.group(1) == "exit":
                    opts["exit"], opts["exit-sims"] = m.group(2), "msim"
        tests.append(Test(name, [os.path.abspath(sv)], opts, os.path.dirname(sv)))
    return tests


def resolve(args, base):
    """Make source paths in a test's arguments absolute (runs use their own dir)."""
    out, take_path = [], False
    for a in args:
        if take_path:
            out.append(a if os.path.isabs(a) else os.path.normpath(os.path.join(base, a)))
            take_path = False
        elif a in ("-f", "-F", "-v", "-y", "-I"):
            out.append(a)
            take_path = True
        elif a.startswith("+incdir+"):
            dirs = a[len("+incdir+"):].split("+")
            out.append("+incdir+" + "+".join(d if os.path.isabs(d) else os.path.join(base, d) for d in dirs if d))
        elif not a.startswith(("-", "+")) and os.path.exists(os.path.join(base, a)):
            out.append(os.path.normpath(os.path.join(base, a)))
        else:
            out.append(a)
    return out


def command(sim, exe, args, seed, rundir):
    """Shell command for one run, per simulator."""
    q = " ".join(shlex.quote(a) for a in args)
    if sim == "msim":
        return f"{shlex.quote(exe or 'msim')} {q} +ntb_random_seed={seed}"
    if sim == "vcs":
        return f"{shlex.quote(exe or 'vcs')} -sverilog -R -l /dev/null {q} +ntb_random_seed={seed}"
    if sim == "xrun":
        return f"{shlex.quote(exe or 'xrun')} -sv -q {q} -svseed {seed}"
    if sim == "questa":
        return f"{shlex.quote(exe or 'qrun')} -sv {q} -sv_seed {seed}"
    if sim == "iverilog":
        comp, run, it = [], [], iter(args)
        for a in it:
            if a == "-top":
                comp += ["-s", next(it)]
            elif a.startswith("+define+"):
                comp += ["-D" + d for d in a[len("+define+"):].split("+") if d]
            elif a.startswith("+incdir+"):
                comp += ["-I" + d for d in a[len("+incdir+"):].split("+") if d]
            elif a.startswith("+"):
                run.append(a)
            elif a in ("-sv", "-sverilog"):
                continue
            else:
                comp.append(a)
        out = os.path.join(rundir, "sim.vvp")
        iv = exe or "iverilog"
        vvp = os.path.join(os.path.dirname(iv), "vvp") if os.path.dirname(iv) else "vvp"
        return (f"{shlex.quote(iv)} -g2012 -o {shlex.quote(out)} "
                + " ".join(shlex.quote(a) for a in comp)
                + f" && {shlex.quote(vvp)} -n {shlex.quote(out)} "
                + " ".join(shlex.quote(a) for a in run) + f" +ntb_random_seed={seed}")
    raise SystemExit(f"mregress: unknown simulator {sim}")


# Simulator chatter that is not the design's output (for @expect).
NOISE = [r"\$finish called", r"^VCD info:", r"^WARNING: ", r"^msim: "]


def expected_lines(test, args):
    exp = test.opts.get("expect")
    if not exp:
        return None
    if exp == "inline":
        svs = [a for a in args if a.endswith((".sv", ".v"))]
        if not svs:
            return None
        src = svs[0]
        lines = []
        with open(src, encoding="utf-8", errors="replace") as f:
            for l in f:
                l = l.rstrip("\n")
                if l.startswith("//> "):
                    lines.append(l[4:])
                elif l == "//>":
                    lines.append("")
        return lines
    path = exp if os.path.isabs(exp) else os.path.join(test.base, exp)
    with open(path, encoding="utf-8") as f:
        return f.read().splitlines()


def applies(test, key, sim):
    sims = test.opts.get(key + "-sims")
    return key in test.opts and (not sims or sim in sims.split(","))


def check(test, args, rc, out_lines, err_lines, timed_out, sim="msim"):
    """Return (status, reason) for one run, before @xfail is applied."""
    if timed_out:
        return "fail", "timeout"
    # With @expect the expected output is the oracle, so the built-in error
    # patterns (which would flag an expected "Error:" line) are not used.
    fail_pats = [] if "expect" in test.opts else list(ERROR_PATTERNS)
    if "fail" in test.opts:
        fail_pats.append(test.opts["fail"])
    for l in out_lines + err_lines:
        for p in fail_pats:
            if re.search(p, l):
                return "fail", l.strip()
    want = int(test.opts["exit"]) if applies(test, "exit", sim) else 0
    if rc != want:
        last = next((l.strip() for l in reversed(err_lines) if l.strip()), "")
        return "fail", f"exit status {rc}" + (f" (expected {want})" if want else "") + (f": {last}" if last else "")
    if "pass" in test.opts and not any(re.search(test.opts["pass"], l) for l in out_lines):
        return "fail", f"no line matches @pass={test.opts['pass']}"
    exp = expected_lines(test, args)
    if exp is not None:
        got = [l for l in out_lines if not any(re.search(n, l) for n in NOISE)]
        if got != exp:
            for i, (e, g) in enumerate(zip(exp + [None] * len(got), got + [None] * len(exp))):
                if e != g:
                    return "fail", f"output differs at line {i + 1}: expected {e!r}, got {g!r}"
    return "pass", ""


def signature(reason):
    """Group failures: numbers, paths and hex become placeholders."""
    s = re.sub(r"0x[0-9a-fA-F]+", "<hex>", reason)
    s = re.sub(r"(/[^\s:'\"]+)+", "<path>", s)
    s = re.sub(r"\d+", "<n>", s)
    return s[:160]


def run_one(job, sim, exe, outdir, timeout_default, common, cov=False):
    test, seed = job
    if sim in test.opts.get("skip", "").split(","):
        return {"test": test.name, "seed": seed, "status": "skip", "reason": f"@skip={test.opts['skip']}",
                "seconds": 0.0, "log": "", "cmd": ""}
    args = resolve(common + test.args, test.base)
    rundir = os.path.join(outdir, "runs", f"{test.name.replace('/', '_')}_s{seed}")
    os.makedirs(rundir, exist_ok=True)
    if cov:
        args = args + ["-cov", os.path.join(rundir, "cov.json")]
    cmd = command(sim, exe, args, seed, rundir)
    with open(os.path.join(rundir, "cmd.sh"), "w") as f:
        f.write(f"#!/bin/sh\ncd {shlex.quote(test.base)}\n{cmd}\n")
    timeout = float(test.opts.get("timeout", timeout_default))
    start = time.time()
    timed_out = False
    # Run in the test list's directory, like the spec suite, so relative
    # data files ($readmemh) resolve.
    # stdout and stderr are kept apart: @expect compares stdout only (the
    # design's output), while error patterns are searched in both.
    p = subprocess.Popen(cmd, shell=True, cwd=test.base, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, start_new_session=True)
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(p.pid, 9)
        except OSError:
            pass
        out, err = p.communicate()
    secs = time.time() - start
    text = out.decode("utf-8", errors="replace")
    etext = err.decode("utf-8", errors="replace")
    with open(os.path.join(rundir, "sim.log"), "w", encoding="utf-8") as f:
        f.write(text)
        if etext:
            f.write("\n---- stderr ----\n" + etext)
    status, reason = check(test, args, p.returncode, text.splitlines(), etext.splitlines(), timed_out, sim)
    # @xfail: a known gap. Failing is expected; passing is reported (xpass)
    # and fails the regression, so the marker gets removed.
    if applies(test, "xfail", sim):
        status, reason = ("xfail", test.opts["xfail"]) if status == "fail" else ("xpass", "passes: remove @xfail / //! xfail")
    return {"test": test.name, "seed": seed, "status": status, "reason": reason,
            "seconds": round(secs, 3), "log": os.path.relpath(os.path.join(rundir, "sim.log"), outdir),
            "cmd": cmd}


def write_junit(results, path, sim):
    suite = ET.Element("testsuite", name=f"mregress.{sim}", tests=str(len(results)),
                       failures=str(sum(r["status"] in ("fail", "xpass") for r in results)),
                       skipped=str(sum(r["status"] in ("skip", "xfail") for r in results)),
                       time=f"{sum(r['seconds'] for r in results):.3f}")
    for r in results:
        tc = ET.SubElement(suite, "testcase", classname=r["test"], name=f"seed{r['seed']}",
                           time=f"{r['seconds']:.3f}")
        if r["status"] in ("skip", "xfail"):
            ET.SubElement(tc, "skipped", message=r["reason"])
        elif r["status"] != "pass":
            ET.SubElement(tc, "failure", message=r["reason"]).text = f"log: {r['log']}\ncmd: {r['cmd']}"
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def write_html(results, groups, path, sim):
    npass = sum(r["status"] == "pass" for r in results)
    rows = "\n".join(
        f"<tr class='{r['status']}'><td>{html.escape(r['test'])}</td><td>{r['seed']}</td>"
        f"<td>{r['status'].upper()}</td><td>{r['seconds']:.2f}</td>"
        f"<td><a href='{html.escape(r['log'])}'>log</a></td><td>{html.escape(r['reason'])}</td></tr>"
        for r in sorted(results, key=lambda r: (r["status"] == "pass", r["test"], r["seed"])))
    grows = "\n".join(
        f"<tr><td>{len(v)}</td><td>{html.escape(k)}</td>"
        f"<td>{html.escape(', '.join(sorted({r['test'] for r in v})))}</td></tr>"
        for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1])))
    doc = f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>mregress {sim}</title>
<style>body{{font:14px system-ui,sans-serif;margin:24px}}table{{border-collapse:collapse}}
td,th{{border:1px solid #ccc;padding:4px 8px;text-align:left}}tr.fail td{{background:#fbebeb}}
tr.pass td{{background:#e5f4e4}}tr.skip td{{background:#f0efec}}tr.xfail td{{background:#f9eeda}}tr.xpass td{{background:#fbebeb}}</style></head><body>
<h1>mregress: {npass} / {len(results)} passed ({sim})</h1>
<h2>Failure groups</h2><table><tr><th>Runs</th><th>Signature</th><th>Tests</th></tr>{grows}</table>
<h2>Runs</h2><table><tr><th>Test</th><th>Seed</th><th>Status</th><th>Seconds</th><th>Log</th><th>Reason</th></tr>
{rows}</table></body></html>"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(doc)


def print_chapters(runs, md=None):
    """Per IEEE chapter (the test name's leading number): pass, known gaps, unexpected."""
    by = {}
    for r in runs:
        m = re.match(r"(\d+)", r["test"])
        ch = int(m.group(1)) if m else -1
        c = by.setdefault(ch, {"pass": 0, "xfail": 0, "skip": 0, "bad": []})
        if r["status"] == "pass":
            c["pass"] += 1
        elif r["status"] in ("xfail", "skip"):
            c[r["status"]] += 1
        else:
            c["bad"].append(r["test"] + (" (xfail passes)" if r["status"] == "xpass" else ""))
    rows = ["| Chapter | Pass | Known gaps | Skipped | Unexpected | Pass rate |", "|---|---|---|---|---|---|"]
    for ch in sorted(by):
        c = by[ch]
        xp = sum(1 for b in c["bad"] if b.endswith("(xfail passes)"))
        n = c["pass"] + c["xfail"] + len(c["bad"])
        rate = 100.0 * (c["pass"] + xp) / n if n else 100.0
        rows.append(f"| {ch if ch >= 0 else '-'} | {c['pass'] + xp} | {c['xfail']} | {c['skip']} | {', '.join(c['bad'])} | {rate:.0f}% |")
    text = "\n".join(rows)
    print("\n" + text)
    if md:
        with open(md, "w", encoding="utf-8") as f:
            f.write(text + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mregress", description="Run a regression (see the module docstring).")
    ap.add_argument("lists", nargs="*", help="test list files")
    ap.add_argument("-spec", metavar="DIR", help="run every DIR/*.sv as a test with @expect=inline")
    ap.add_argument("-sim", help="msim (default), vcs, xrun, questa, or iverilog")
    ap.add_argument("-exe", help="simulator executable (default: found on PATH)")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 1, help="parallel runs (default: CPU count)")
    ap.add_argument("-seeds", type=int, help="seeds per test (overrides @seeds)")
    ap.add_argument("-seed", type=int, help="first seed (default: random); seeds are seed, seed+1, ...")
    ap.add_argument("-o", default="regress.d", help="output directory (default regress.d)")
    ap.add_argument("-timeout", type=float, default=300, help="seconds per run (default 300)")
    ap.add_argument("-filter", help="only tests whose name matches this regex")
    ap.add_argument("-rerun", action="store_true", help="rerun the failures recorded in <o>/results.json")
    ap.add_argument("-q", action="store_true", help="print only the summary")
    ap.add_argument("-chapters", action="store_true",
                    help="print a pass table per chapter (leading number of the test name; on by default with -spec)")
    ap.add_argument("-md", metavar="FILE", help="also write the chapter table as Markdown")
    ap.add_argument("-cov", action="store_true",
                    help="msim only: collect coverage per run and merge it into <o>/cov.json (see mcov)")
    ap.add_argument("-version", action="version", version=f"mregress {VERSION}")
    a = ap.parse_args(argv)

    tests, settings = [], {}
    for l in a.lists:
        parse_list(l, tests, settings)
    if a.spec:
        tests += spec_tests(a.spec)
    if a.filter:
        tests = [t for t in tests if re.search(a.filter, t.name)]
    if not tests:
        ap.error("no tests (give a test list or -spec DIR)")
    sim = a.sim or settings.get("sim", "msim")
    if sim not in ("msim", "vcs", "xrun", "questa", "iverilog"):
        ap.error(f"unknown simulator {sim}")
    exe = os.path.abspath(a.exe) if a.exe and os.sep in a.exe else a.exe
    tool = exe or {"msim": "msim", "vcs": "vcs", "xrun": "xrun", "questa": "qrun", "iverilog": "iverilog"}[sim]
    if not shutil.which(tool):
        ap.error(f"{tool} not found (put it on PATH or pass -exe)")

    outdir = os.path.abspath(a.o)
    jobs, previous = [], []
    if a.rerun:
        with open(os.path.join(outdir, "results.json"), encoding="utf-8") as f:
            previous = json.load(f)["runs"]
        failed = {(r["test"], r["seed"]) for r in previous if r["status"] in ("fail", "xpass")}
        byname = {t.name: t for t in tests}
        jobs = [(byname[n], s) for n, s in sorted(failed) if n in byname]
    else:
        first = a.seed if a.seed is not None else random.randrange(1, 2**31)
        for t in tests:
            if "seed" in t.opts and a.seeds is None:
                jobs.append((t, int(t.opts["seed"])))
                continue
            n = a.seeds if a.seeds is not None else int(t.opts.get("seeds", 1))
            jobs += [(t, first + i) for i in range(n)]
    os.makedirs(outdir, exist_ok=True)

    results, start = [], time.time()
    with cf.ThreadPoolExecutor(max_workers=max(1, a.j)) as pool:
        if a.cov and sim != "msim":
            ap.error("-cov needs -sim msim")
        futs = [pool.submit(run_one, j, sim, exe, outdir, a.timeout, settings.get("common", []), a.cov) for j in jobs]
        for fut in cf.as_completed(futs):
            r = fut.result()
            results.append(r)
            if not a.q:
                tag = r["status"].upper()
                extra = f"  {r['reason']}" if r["status"] in ("fail", "xpass", "xfail") else ""
                print(f"{tag}  {r['test']}  seed={r['seed']}  {r['seconds']:.2f}s{extra}", flush=True)

    # A rerun replaces only the runs it repeated; the rest of the record stays.
    redone = {(r["test"], r["seed"]) for r in results}
    results += [r for r in previous if (r["test"], r["seed"]) not in redone]
    groups = {}
    for r in results:
        if r["status"] in ("fail", "xpass"):
            groups.setdefault(signature(r["reason"]), []).append(r)
    npass = sum(r["status"] == "pass" for r in results)
    nskip = sum(r["status"] == "skip" for r in results)
    nxfail = sum(r["status"] == "xfail" for r in results)
    nfail = len(results) - npass - nskip - nxfail
    summary = {"simulator": sim, "passed": npass, "failed": nfail, "skipped": nskip, "xfail": nxfail,
               "seconds": round(time.time() - start, 3), "runs": sorted(results, key=lambda r: (r["test"], r["seed"]))}
    with open(os.path.join(outdir, "results.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    write_junit(summary["runs"], os.path.join(outdir, "junit.xml"), sim)
    write_html(summary["runs"], groups, os.path.join(outdir, "report.html"), sim)

    if a.cov:
        dbs = sorted(glob.glob(os.path.join(outdir, "runs", "*", "cov.json")))
        here = os.path.dirname(os.path.abspath(__file__))
        mcov = next((c for c in (os.path.join(here, "mcov"), os.path.join(here, "mcov.py")) if os.path.exists(c)), None)
        if dbs and mcov:
            merged = os.path.join(outdir, "cov.json")
            subprocess.run([sys.executable, mcov, "merge", *dbs, "-o", merged], stdout=subprocess.DEVNULL, check=False)
            subprocess.run([sys.executable, mcov, "report", merged, "-html", os.path.join(outdir, "cov_html")], check=False)
    if a.spec or a.chapters:
        print_chapters(summary["runs"], a.md)
    print(f"\nmregress: {npass} passed, {nfail} failed, {nskip} skipped" + (f", {nxfail} known gaps (xfail)" if nxfail else "")
          + f" on {sim} in {summary['seconds']:.1f}s"
          f"  ({os.path.relpath(outdir)}/report.html)")
    for sig, rs in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        names = sorted({r["test"] for r in rs})
        print(f"  {len(rs):4d}  {sig}\n        {', '.join(names[:8])}{' ...' if len(names) > 8 else ''}")
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
