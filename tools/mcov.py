#!/usr/bin/env python3
"""mcov: merge and report msim code coverage (msim -cov).

    msim tb.sv -cov run1.json                 # one database per run
    mcov merge run1.json run2.json -o all.json
    mcov report all.json                      # summary per file and in total
    mcov report all.json -html cov_html       # annotated sources
    mcov report all.json -lcov all.info       # for genhtml, IDEs, CI services
    mcov report all.json -min-line 90         # exit 1 below 90% line coverage

A database holds line hits, branch outcomes (each if / case arm / loop test
taken true and false), and toggle masks (each signal bit seen rising 0->1 and
falling 1->0). Merging adds hit counts and ORs the toggle masks, matched by
file path and line, and by signal name.

Exit status: 0, or 1 when a -min-* threshold is not met, 2 on bad usage.
"""

import argparse
import html
import json
import os
import re
import sys

VERSION = "0.1.0"


def load(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    if d.get("format") != "msim-cov":
        raise SystemExit(f"mcov: {path} is not an msim coverage database")
    files = d["files"]
    db = {"lines": {}, "branches": {}, "toggles": {}, "tests": d.get("tests", [os.path.basename(path)])}
    for fi, ln, hits in d["lines"]:
        k = (files[fi], ln)
        db["lines"][k] = db["lines"].get(k, 0) + hits
    for fi, ln, n, t, fl in d["branches"]:
        k = (files[fi], ln, n)
        a, b = db["branches"].get(k, (0, 0))
        db["branches"][k] = (a + t, b + fl)
    for name, w, rise, fall in d["toggles"]:
        r, f = int(rise or "0", 16), int(fall or "0", 16)
        if name in db["toggles"]:
            w0, r0, f0 = db["toggles"][name]
            db["toggles"][name] = (max(w, w0), r | r0, f | f0)
        else:
            db["toggles"][name] = (w, r, f)
    return db


def merge(dbs):
    out = {"lines": {}, "branches": {}, "toggles": {}, "tests": []}
    for db in dbs:
        out["tests"] += db["tests"]
        for k, h in db["lines"].items():
            out["lines"][k] = out["lines"].get(k, 0) + h
        for k, (t, f) in db["branches"].items():
            a, b = out["branches"].get(k, (0, 0))
            out["branches"][k] = (a + t, b + f)
        for k, (w, r, f) in db["toggles"].items():
            w0, r0, f0 = out["toggles"].get(k, (w, 0, 0))
            out["toggles"][k] = (max(w, w0), r | r0, f | f0)
    return out


def save(db, path):
    files = sorted({k[0] for k in db["lines"]} | {k[0] for k in db["branches"]})
    idx = {f: i for i, f in enumerate(files)}
    hexw = lambda v, w: format(v, "x").rjust((w + 3) // 4, "0")
    d = {
        "format": "msim-cov", "version": 1, "tool": f"mcov {VERSION}", "tests": db["tests"],
        "files": files,
        "lines": [[idx[f], ln, h] for (f, ln), h in sorted(db["lines"].items())],
        "branches": [[idx[f], ln, n, t, fl] for (f, ln, n), (t, fl) in sorted(db["branches"].items())],
        "toggles": [[name, w, hexw(r, w), hexw(fl, w)] for name, (w, r, fl) in sorted(db["toggles"].items())],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=0)


def pct(a, b):
    return 100.0 * a / b if b else 100.0


def summarize(db, exclude):
    """Per file: (lines hit, lines, branch outcomes hit, outcomes); toggles in total."""
    per = {}
    for (f, ln), h in db["lines"].items():
        if exclude and re.search(exclude, f):
            continue
        s = per.setdefault(f, [0, 0, 0, 0])
        s[1] += 1
        s[0] += h > 0
    for (f, ln, n), (t, fl) in db["branches"].items():
        if exclude and re.search(exclude, f):
            continue
        s = per.setdefault(f, [0, 0, 0, 0])
        s[3] += 2
        s[2] += (t > 0) + (fl > 0)
    tog_hit = tog_all = 0
    for name, (w, r, fl) in db["toggles"].items():
        if exclude and re.search(exclude, name):
            continue
        both = r & fl
        tog_hit += bin(both).count("1")
        tog_all += w
    return per, tog_hit, tog_all


def write_lcov(db, path, exclude):
    with open(path, "w", encoding="utf-8") as f:
        f.write("TN:msim\n")
        files = sorted({k[0] for k in db["lines"]})
        for src in files:
            if exclude and re.search(exclude, src):
                continue
            f.write(f"SF:{src}\n")
            brs = sorted((k, v) for k, v in db["branches"].items() if k[0] == src)
            for (_, ln, n), (t, fl) in brs:
                f.write(f"BRDA:{ln},{n},0,{t}\nBRDA:{ln},{n},1,{fl}\n")
            if brs:
                f.write(f"BRF:{2 * len(brs)}\nBRH:{sum((t > 0) + (fl > 0) for _, (t, fl) in brs)}\n")
            lines = sorted((ln, h) for (s, ln), h in db["lines"].items() if s == src)
            for ln, h in lines:
                f.write(f"DA:{ln},{h}\n")
            f.write(f"LF:{len(lines)}\nLH:{sum(h > 0 for _, h in lines)}\nend_of_record\n")


def write_html(db, outdir, exclude):
    os.makedirs(outdir, exist_ok=True)
    per, th, ta = summarize(db, exclude)
    style = ("body{font:14px system-ui,sans-serif;margin:24px}table{border-collapse:collapse}"
             "td,th{border:1px solid #ccc;padding:3px 8px}pre{margin:0}.hit{background:#e5f4e4}"
             ".miss{background:#fbebeb}.part{background:#f9eeda}.src td{border:none;padding:0 8px;"
             "font:13px ui-monospace,monospace;white-space:pre}.n{color:#888;text-align:right}")
    rows = []
    for i, (src, (lh, lt, bh, bt)) in enumerate(sorted(per.items())):
        page = f"f{i}.html"
        rows.append(f"<tr><td><a href='{page}'>{html.escape(src)}</a></td><td>{pct(lh, lt):.1f}% ({lh}/{lt})</td>"
                    f"<td>{pct(bh, bt):.1f}% ({bh}/{bt})</td></tr>")
        hits = {ln: h for (s, ln), h in db["lines"].items() if s == src}
        br = {}
        for (s, ln, n), (t, fl) in db["branches"].items():
            if s == src:
                br.setdefault(ln, []).append((t, fl))
        try:
            with open(src, encoding="utf-8", errors="replace") as f:
                text = f.read().splitlines()
        except OSError:
            text = [""] * (max(hits) if hits else 0)
        body = []
        for ln, line in enumerate(text, 1):
            cls, note = "", ""
            if ln in hits:
                cls = "hit" if hits[ln] else "miss"
                note = str(hits[ln])
            if ln in br:
                miss = [("T" if not t else "") + ("F" if not fl else "") for t, fl in br[ln]]
                if any(miss) and cls == "hit":
                    cls = "part"
                note += "  " + " ".join(f"[{'T' if t else '-'}{'F' if fl else '-'}]" for t, fl in br[ln])
            body.append(f"<tr class='{cls}'><td class='n'>{ln}</td><td class='n'>{html.escape(note)}</td>"
                        f"<td>{html.escape(line)}</td></tr>")
        with open(os.path.join(outdir, page), "w", encoding="utf-8") as f:
            f.write(f"<!DOCTYPE html><html><head><meta charset='utf-8'><title>{html.escape(src)}</title>"
                    f"<style>{style}</style></head><body><p><a href='index.html'>index</a></p>"
                    f"<h2>{html.escape(src)}</h2><p>Line hits, then branch outcomes per decision: "
                    f"[TF] both seen, [T-] never false, [-F] never true.</p>"
                    f"<table class='src'>{''.join(body)}</table></body></html>")
    togs = []
    for name, (w, r, fl) in sorted(db["toggles"].items()):
        if exclude and re.search(exclude, name):
            continue
        both = r & fl
        n = bin(both).count("1")
        if n < w:
            missing = [str(b) for b in range(w) if not (both >> b) & 1]
            togs.append(f"<tr><td>{html.escape(name)}</td><td>{n}/{w}</td><td>{', '.join(missing[:32])}"
                        f"{' ...' if len(missing) > 32 else ''}</td></tr>")
    lh = sum(v[0] for v in per.values()); lt = sum(v[1] for v in per.values())
    bh = sum(v[2] for v in per.values()); bt = sum(v[3] for v in per.values())
    with open(os.path.join(outdir, "index.html"), "w", encoding="utf-8") as f:
        f.write(f"<!DOCTYPE html><html><head><meta charset='utf-8'><title>mcov</title><style>{style}</style>"
                f"</head><body><h1>Coverage</h1><p>{len(db['tests'])} run(s). Line {pct(lh, lt):.1f}% ({lh}/{lt}), "
                f"branch {pct(bh, bt):.1f}% ({bh}/{bt}), toggle {pct(th, ta):.1f}% ({th}/{ta} bits)</p>"
                f"<table><tr><th>File</th><th>Line</th><th>Branch</th></tr>{''.join(rows)}</table>"
                f"<h2>Signals not fully toggled</h2><table><tr><th>Signal</th><th>Bits toggled both ways</th>"
                f"<th>Missing bits</th></tr>{''.join(togs)}</table></body></html>")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mcov", description="Merge and report msim coverage (see module docstring).")
    ap.add_argument("-version", action="version", version=f"mcov {VERSION}")
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("merge", help="merge databases")
    m.add_argument("dbs", nargs="+")
    m.add_argument("-o", required=True, help="merged database")
    r = sub.add_parser("report", help="print a summary; optionally HTML / LCOV")
    r.add_argument("dbs", nargs="+", help="databases (merged first if several)")
    r.add_argument("-html", metavar="DIR")
    r.add_argument("-lcov", metavar="FILE")
    r.add_argument("-exclude", metavar="REGEX", help="leave out files / signals matching this")
    for k in ("line", "branch", "toggle"):
        r.add_argument(f"-min-{k}", type=float, metavar="PCT", help=f"exit 1 if {k} coverage is below PCT")
    a = ap.parse_args(argv)

    db = merge([load(p) for p in a.dbs])
    if a.cmd == "merge":
        save(db, a.o)
        print(f"mcov: merged {len(a.dbs)} database(s) ({len(db['tests'])} run(s)) into {a.o}")
        return 0
    per, th, ta = summarize(db, a.exclude)
    lh = sum(v[0] for v in per.values()); lt = sum(v[1] for v in per.values())
    bh = sum(v[2] for v in per.values()); bt = sum(v[3] for v in per.values())
    w = max([len(f) for f in per] + [4])
    print(f"{'File':<{w}}  {'Line':>16}  {'Branch':>16}")
    for f, (a1, a2, b1, b2) in sorted(per.items()):
        print(f"{f:<{w}}  {pct(a1, a2):6.1f}% {a1:>4}/{a2:<4}  {pct(b1, b2):6.1f}% {b1:>4}/{b2:<4}")
    print(f"{'Total':<{w}}  {pct(lh, lt):6.1f}% {lh:>4}/{lt:<4}  {pct(bh, bt):6.1f}% {bh:>4}/{bt:<4}")
    print(f"Toggle: {pct(th, ta):.1f}% ({th}/{ta} bits toggled both ways), {len(db['tests'])} run(s)")
    if a.html:
        write_html(db, a.html, a.exclude)
        print(f"HTML: {os.path.join(a.html, 'index.html')}")
    if a.lcov:
        write_lcov(db, a.lcov, a.exclude)
        print(f"LCOV: {a.lcov}")
    rc = 0
    for k, (hit, tot) in {"line": (lh, lt), "branch": (bh, bt), "toggle": (th, ta)}.items():
        lim = getattr(a, f"min_{k}")
        if lim is not None and pct(hit, tot) < lim:
            print(f"mcov: {k} coverage {pct(hit, tot):.1f}% is below {lim}%")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
