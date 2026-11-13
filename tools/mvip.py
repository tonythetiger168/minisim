#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mvip.py --portfolio DIR [VIP ...] [-j N] [--csv FILE] [--md FILE]

Run the UVM tests of a VIP portfolio checkout on msim.

The portfolio is a checkout of open-vip-portfolio (vips/*.zip plus
tools/run_uvm_tests.py). Its own runner knows each VIP's file list, top
module, tests and pass rule; this script takes all of that from it and only
replaces the simulator: where it builds a Verilator binary, mvip runs

    msim -uvm --single-unit <+incdir+...> <files> -top <top> +UVM_TESTNAME=<test>

(one compilation unit, as Verilator compiles them: the VIPs rely on a macro
or a $unit class of an earlier file being visible in the later ones).

Two kinds of test, as in the portfolio:
  checked   VIPs with a register model or a template-built agent: the test
            must finish with no UVM_ERROR / UVM_FATAL, no UVM_WARNING from
            the VIP's own code, and print RAL_PASS / SB_PASS checks=<n>.
  smoke     every other VIP: all its .sv files, the module that calls
            run_test() as top, the first test derived from its base test.
            PASS: runs past time 0 with no error. WARN: no error, but the
            VIP's own checkers warned.
Results: one line per test, then a summary; --csv / --md write the table.
Env: MSIM (default: msim on PATH), UVM_HOME (uvm-core checkout).
"""
import argparse, concurrent.futures as cf, csv, os, re, shutil, subprocess, sys, time, types, zipfile


def load_portfolio(root, work):
    """The portfolio's runner as a module, with its VIPs unpacked to work/vips."""
    vips = os.path.join(work, "vips")
    if not os.path.isdir(vips):
        os.makedirs(vips)
        for z in sorted(os.listdir(os.path.join(root, "vips"))):
            if z.endswith(".zip"):
                zipfile.ZipFile(os.path.join(root, "vips", z)).extractall(vips)
    os.environ["VIPS_DIR"] = vips
    # elab_scan imports pyslang for its own lint pass; only its file order is used here
    if "pyslang" not in sys.modules:
        try:
            import pyslang  # noqa: F401
        except ImportError:
            m = types.ModuleType("pyslang")
            m.driver = None
            sub = types.ModuleType("pyslang.pyslang")
            sub.DiagnosticEngine = sub.DiagnosticSeverity = None
            m.pyslang = sub
            sys.modules["pyslang"] = m
            sys.modules["pyslang.pyslang"] = sub
    sys.path.insert(0, os.path.join(root, "tools"))
    import run_uvm_tests as r
    return r, vips


def run_one(r, vips, work, msim, vip, timeout):
    checked = vip in r.SUITES
    suite = r.SUITES[vip] if checked else r.smoke_suite(vip)
    vdir = os.path.join(vips, vip)
    rows = []
    if not suite["tests"]:
        return [(vip, "", "smoke", "NONE", 0, "no UVM test", 0.0)]
    for test in suite["tests"]:
        cmd = [msim, "-uvm", "--single-unit"] + ["+incdir+" + os.path.join(vdir, d) for d in suite["incs"]] \
            + [os.path.join(vdir, f) for f in suite["files"]] \
            + ["-top", suite.get("top", "top_tb"), "+UVM_TESTNAME=" + test]
        log = os.path.join(work, "logs", "%s.%s.log" % (vip, test))
        os.makedirs(os.path.dirname(log), exist_ok=True)
        t0 = time.time()
        rc, out = r.run_to_file(cmd, vdir, timeout, log, max_log=64 << 20)
        dt = time.time() - t0
        # msim's own errors: an unsupported construct or a compile error
        tool = [l for l in out.splitlines() if l.startswith(("msim:", "internal error")) or ": error:" in l]
        if tool and "UVM_FATAL :" not in out:
            rows.append((vip, test, "checked" if checked else "smoke", "ERROR", 0, tool[0][:300], dt))
            continue
        if checked:
            st, n, detail = r.classify(rc, out)
        else:
            st, n, detail = r.classify_smoke(rc, out)
            if st == "PASS" and re.search(r"\$finish .*\btime 0\b", out):
                st, detail = "FAIL", "finished at time 0: the test drove no traffic"
        rows.append((vip, test, "checked" if checked else "smoke", st, n, detail, dt))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--portfolio", required=True, help="open-vip-portfolio checkout")
    ap.add_argument("--work", default="mvip_work", help="scratch directory (unpacked VIPs, logs)")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--timeout", type=int, default=600, help="seconds per test")
    ap.add_argument("--csv")
    ap.add_argument("--md")
    ap.add_argument("--expect", help="CSV of an earlier run: exit non-zero only if a test that passed there no longer does")
    ap.add_argument("vips", nargs="*")
    a = ap.parse_args()
    msim = os.environ.get("MSIM") or shutil.which("msim")
    if not msim:
        sys.exit("mvip: msim not found (set MSIM)")
    msim = os.path.abspath(msim)
    work = os.path.abspath(a.work)
    r, vips = load_portfolio(os.path.abspath(a.portfolio), work)
    names = a.vips or sorted(d for d in os.listdir(vips) if os.path.isdir(os.path.join(vips, d)))
    rows = []
    with cf.ThreadPoolExecutor(a.j) as ex:
        futs = {ex.submit(run_one, r, vips, work, msim, v, a.timeout): v for v in names}
        for f in cf.as_completed(futs):
            for row in f.result():
                rows.append(row)
                print("%-5s %s/%s (%s, %.1fs)%s" % (row[3], row[0], row[1], row[2], row[6],
                                                   " -- " + row[5] if row[5] else ""), flush=True)
    rows.sort()
    count = {}
    for row in rows:
        count[row[3]] = count.get(row[3], 0) + 1
    print("mvip: %d tests: %s" % (len(rows), ", ".join("%s %d" % kv for kv in sorted(count.items()))))
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["vip", "test", "kind", "result", "checks", "detail"])
            w.writerows(row[:6] for row in rows)
    if a.md:
        with open(a.md, "w") as f:
            f.write("| VIP | Test | Kind | Result | Checks | First problem |\n|---|---|---|---|---|---|\n")
            for v, t, k, st, n, d, _ in rows:
                f.write("| %s | `%s` | %s | %s | %s | %s |\n" % (v, t, k, st, n or "", d.replace("|", "\\|")))
    if a.expect:
        was = {(x["vip"], x["test"]): x["result"] for x in csv.DictReader(open(a.expect))}
        worse = [row for row in rows if was.get((row[0], row[1])) in ("PASS", "WARN")
                 and row[3] not in ("PASS", "WARN")]
        for row in worse:
            print("REGRESSION %s/%s: %s -- %s" % (row[0], row[1], row[3], row[5]))
        sys.exit(1 if worse else 0)
    sys.exit(0 if all(row[3] in ("PASS", "WARN", "NONE") for row in rows) else 1)


if __name__ == "__main__":
    main()
