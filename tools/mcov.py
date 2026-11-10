#!/usr/bin/env python3
"""mcov: merge and report msim code coverage (msim -cov).

    msim tb.sv -cov run1.json                 # one database per run
    mcov merge run1.json run2.json -o all.json
    mcov report all.json                      # summary per file and in total
    mcov report all.json -html cov_html       # annotated sources
    mcov report all.json -lcov all.info       # for genhtml, IDEs, CI services
    mcov report all.json -min-line 90         # exit 1 below 90% line coverage
    mcov report all.json -detail              # every condition vector, FSM state and transition

A database holds line hits, branch outcomes (each if / case arm / loop test
taken true and false), toggle masks (each signal bit seen rising 0->1 and
falling 1->0), condition vectors and FSMs. Merging adds hit counts and ORs
the toggle masks, matched by file path and line, and by signal name.

Condition coverage (as VCS -cm cond): a condition a && b && c, a || b || c,
or the condition of ?: with n terms has n + 1 vectors: the one where no term
decides alone (all 1 for &&, all 0 for ||), and for each term the one where
only that term has the deciding value (0 for &&, 1 for ||).

FSM coverage (as VCS -cm fsm): for each state register msim extracted, the
states entered and the transitions made, out of those its next-state logic
can make. Transitions into a state the register's reset branch assigns are
marked; -fsm-reset-filter leaves them out of the score.

Exit status: 0, or 1 when a -min-* threshold is not met, 2 on bad usage.
"""

import argparse
import html
import json
import os
import re
import sys

VERSION = "0.1.0"


def popcount(v):
    return bin(v).count("1")


if hasattr(int, "bit_count"):           # Python 3.10+
    popcount = int.bit_count


def load_into(db, path):
    """Add one database file into db (as merge() would), without a copy."""
    with open(path, "rb") as f:
        d = json.loads(f.read())
    if d.get("format") != "msim-cov":
        raise SystemExit(f"mcov: {path} is not an msim coverage database")
    files = d["files"]
    lines, branches, toggles = db["lines"], db["branches"], db["toggles"]
    db["tests"] += d.get("tests", [os.path.basename(path)])
    conds = db["conds"]
    for fi, ln, col, op, text, terms, hits in d.get("conditions", ()):
        add_cond(conds, (files[fi], ln, col, text), op, tuple(terms), hits)
    fsms = db["fsms"]
    for m in d.get("fsms", ()):
        add_fsm(fsms, (m["module"], m["inst"], m["name"]),
                {"file": files[m["file"]], "line": m["line"], "width": m["width"],
                 "states": {v: [name, h] for v, name, h in m["states"]},
                 "trans": {(a, b): [h, r] for a, b, h, r in m["trans"]},
                 "extra": {(a, b): h for a, b, h in m.get("extra", ())}})
    get = lines.get
    for fi, ln, hits in d["lines"]:
        k = (files[fi], ln)
        lines[k] = get(k, 0) + hits
    for fi, ln, n, t, fl in d["branches"]:
        k = (files[fi], ln, n)
        old = branches.get(k)
        branches[k] = (old[0] + t, old[1] + fl) if old else (t, fl)
    for name, w, rise, fall in d["toggles"]:
        r, f = int(rise or "0", 16), int(fall or "0", 16)
        old = toggles.get(name)
        toggles[name] = (max(w, old[0]), r | old[1], f | old[2]) if old else (w, r, f)
    return db


def add_cond(conds, key, op, terms, hits):
    old = conds.get(key)
    if old is None or len(old[2]) != len(hits):
        conds[key] = (op, terms, list(hits))
    else:
        h = old[2]
        for i, v in enumerate(hits):
            h[i] += v


def add_fsm(fsms, key, m):
    old = fsms.get(key)
    if old is None:
        fsms[key] = {"file": m["file"], "line": m["line"], "width": m["width"],
                     "states": {v: list(x) for v, x in m["states"].items()},
                     "trans": {t: list(x) for t, x in m["trans"].items()},
                     "extra": dict(m["extra"])}
        return
    for v, (name, h) in m["states"].items():
        st = old["states"].setdefault(v, [name, 0])
        st[1] += h
    for t, (h, r) in m["trans"].items():
        tr = old["trans"].setdefault(t, [0, r])
        tr[0] += h
        tr[1] = tr[1] and r
    for t, h in m["extra"].items():
        old["extra"][t] = old["extra"].get(t, 0) + h
    for t in [t for t in old["extra"] if t in old["trans"]]:   # listed by another run's database
        old["trans"][t][0] += old["extra"].pop(t)


def empty():
    return {"lines": {}, "branches": {}, "toggles": {}, "conds": {}, "fsms": {}, "tests": []}


def load(path):
    return load_into(empty(), path)


def merge(dbs):
    out = empty()
    for db in dbs:
        merge_into(out, db)
    return out


def merge_into(out, db):
    out["tests"] += db["tests"]
    lines, branches, toggles = out["lines"], out["branches"], out["toggles"]
    for k, (op, terms, hits) in db["conds"].items():
        add_cond(out["conds"], k, op, terms, hits)
    for k, m in db["fsms"].items():
        add_fsm(out["fsms"], k, m)
    if not lines and not branches and not toggles:
        lines.update(db["lines"]); branches.update(db["branches"]); toggles.update(db["toggles"])
        return out
    get = lines.get
    for k, h in db["lines"].items():
        lines[k] = get(k, 0) + h
    for k, (t, f) in db["branches"].items():
        old = branches.get(k)
        branches[k] = (old[0] + t, old[1] + f) if old else (t, f)
    for k, (w, r, f) in db["toggles"].items():
        old = toggles.get(k)
        toggles[k] = (max(w, old[0]), r | old[1], f | old[2]) if old else (w, r, f)
    return out


def _load_chunk(paths):
    db = empty()
    for p in paths:
        load_into(db, p)
    return db


def cpu_count():
    """CPUs this process may run on (taskset / cgroup aware), logical threads included."""
    try:
        return len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return os.cpu_count() or 1


def load_all(paths, jobs=0):
    """Load and merge databases; with many files, in parallel worker processes.
    JSON decoding is CPU bound, so each worker merges its own share and the
    partial results are merged here, in input order (sums and ORs commute)."""
    jobs = jobs or cpu_count()
    jobs = max(1, min(jobs, len(paths) // 4))
    if jobs <= 1:
        return _load_chunk(paths)
    import concurrent.futures as cf
    size = (len(paths) + jobs - 1) // jobs
    chunks = [paths[i:i + size] for i in range(0, len(paths), size)]
    try:
        with cf.ProcessPoolExecutor(max_workers=len(chunks)) as pool:
            parts = list(pool.map(_load_chunk, chunks))
    except SystemExit:
        raise
    except Exception:   # no worker processes here (or they cannot import mcov): load serially
        return _load_chunk(paths)
    out = empty()
    for part in parts:
        merge_into(out, part)
    return out


def save(db, path):
    files = sorted({k[0] for k in db["lines"]} | {k[0] for k in db["branches"]} | {k[0] for k in db["conds"]} |
                   {m["file"] for m in db["fsms"].values()})
    idx = {f: i for i, f in enumerate(files)}
    hexw = lambda v, w: format(v, "x").rjust((w + 3) // 4, "0")
    # One entry per line, like msim writes it. Built with json.dumps per row:
    # json.dump(indent=...) uses the pure-Python encoder, which was most of
    # the time of a large merge.
    dumps = json.dumps
    rows = lambda items: ",\n".join(dumps(r) for r in items)
    head = {"format": "msim-cov", "version": 1, "tool": f"mcov {VERSION}"}
    with open(path, "w", encoding="utf-8") as f:
        f.write("{\n" + "".join(f"{dumps(k)}: {dumps(v)},\n" for k, v in head.items()))
        f.write(f'"tests": {dumps(db["tests"])},\n"files": {dumps(files)},\n')
        f.write('"lines": [\n' + rows([idx[fl], ln, h] for (fl, ln), h in sorted(db["lines"].items())) + "\n],\n")
        f.write('"branches": [\n' + rows([idx[fl], ln, n, t, fa] for (fl, ln, n), (t, fa)
                                         in sorted(db["branches"].items())) + "\n],\n")
        f.write('"toggles": [\n' + rows([name, w, hexw(r, w), hexw(fa, w)] for name, (w, r, fa)
                                        in sorted(db["toggles"].items())) + "\n],\n")
        f.write('"conditions": [\n' + rows([idx[fl], ln, col, op, text, list(terms), hits]
                                           for (fl, ln, col, text), (op, terms, hits) in sorted(db["conds"].items()))
                + "\n],\n")
        f.write('"fsms": [\n' + rows({"module": mod, "inst": inst, "name": name, "file": idx[m["file"]],
                                      "line": m["line"], "width": m["width"],
                                      "states": [[v, n, h] for v, (n, h) in sorted(m["states"].items())],
                                      "trans": [[a, b, h, r] for (a, b), (h, r) in sorted(m["trans"].items())],
                                      "extra": [[a, b, h] for (a, b), h in sorted(m["extra"].items())]}
                                     for (mod, inst, name), m in sorted(db["fsms"].items())) + "\n]\n}\n")


def by_file(db):
    """file -> ({line: hits}, {line: [(n, taken, not taken)]}), one pass over the database."""
    out = {}
    for (f, ln), h in db["lines"].items():
        out.setdefault(f, ({}, {}))[0][ln] = h
    for (f, ln, n), tf in db["branches"].items():
        out.setdefault(f, ({}, {}))[1].setdefault(ln, []).append((n,) + tuple(tf))
    return out


def pct(a, b):
    return 100.0 * a / b if b else 100.0


def summarize(db, exclude, files=None):
    """Per file: (lines hit, lines, branch outcomes hit, outcomes); toggles in total."""
    files = files if files is not None else by_file(db)
    rx = re.compile(exclude) if exclude else None
    per = {}
    for f, (hits, br) in files.items():
        if rx and rx.search(f):
            continue
        s = [sum(1 for h in hits.values() if h > 0), len(hits), 0, 0]
        for arms in br.values():
            for _, t, fl in arms:
                s[3] += 2
                s[2] += (t > 0) + (fl > 0)
        per[f] = s
    tog_hit = tog_all = 0
    for name, (w, r, fl) in db["toggles"].items():
        if rx and rx.search(name):
            continue
        tog_hit += popcount(r & fl)
        tog_all += w
    return per, tog_hit, tog_all


def cond_summary(db, exclude):
    """(vectors hit, vectors, conditions) of the conditions not excluded."""
    rx = re.compile(exclude) if exclude else None
    hit = tot = n = 0
    for (f, _, _, _), (_, _, hits) in db["conds"].items():
        if rx and rx.search(f):
            continue
        n += 1
        tot += len(hits)
        hit += sum(1 for h in hits if h > 0)
    return hit, tot, n


def fsm_score(m, reset_filter):
    """(states hit, states, transitions hit, transitions) of one FSM."""
    sh = sum(1 for _, h in m["states"].values() if h > 0)
    tr = [h for h, r in m["trans"].values() if not (reset_filter and r)]
    return sh, len(m["states"]), sum(1 for h in tr if h > 0), len(tr)


def fsm_summary(db, exclude, reset_filter):
    rx = re.compile(exclude) if exclude else None
    tot = [0, 0, 0, 0]
    n = 0
    for (mod, inst, name), m in db["fsms"].items():
        if rx and (rx.search(m["file"]) or rx.search(f"{inst}.{name}")):
            continue
        n += 1
        for i, v in enumerate(fsm_score(m, reset_filter)):
            tot[i] += v
    return tot, n


def cond_rows(op, terms, hits):
    """One (values per term, hits) row per vector of a condition, in report order."""
    n = len(terms)
    if op == "?:":
        return [(("1",), hits[0]), (("0",), hits[1])]
    base, dec = ("0", "1") if op == "||" else ("1", "0")
    rows = [((base,) * n, hits[0])]
    for i in range(n):
        rows.append((tuple(dec if j == i else base for j in range(n)), hits[i + 1]))
    return rows


def detail_text(db, exclude, reset_filter, uncovered_only):
    rx = re.compile(exclude) if exclude else None
    out = []
    for (f, ln, col, text), (op, terms, hits) in sorted(db["conds"].items()):
        if rx and rx.search(f):
            continue
        if uncovered_only and all(h > 0 for h in hits):
            continue
        out.append(f"\nCondition {f}:{ln}:{col}  {text}")
        for i, t in enumerate(terms, 1):
            out.append(f"    -{i}- {t}")
        out.append("    " + " ".join(f"-{i}-" for i in range(1, len(terms) + 1)) + "  Hits")
        for vals, h in cond_rows(op, terms, hits):
            out.append("    " + " ".join(f"{v:^{len(str(i)) + 2}}" for i, v in enumerate(vals, 1)) +
                       f"  {h if h else 'Not covered'}")
    for (mod, inst, name), m in sorted(db["fsms"].items()):
        if rx and (rx.search(m["file"]) or rx.search(f"{inst}.{name}")):
            continue
        sh, st, th, tt = fsm_score(m, reset_filter)
        if uncovered_only and sh == st and th == tt and not m["extra"]:
            continue
        nm = lambda v: m["states"][v][0] if v in m["states"] else f"{m['width']}'h{v:x}"
        out.append(f"\nFSM {inst}.{name}  (module {mod}, {m['file']}:{m['line']})  "
                   f"states {sh}/{st}, transitions {th}/{tt}")
        for v, (n, h) in sorted(m["states"].items()):
            if not uncovered_only or not h:
                out.append(f"    state {n:<20} {h if h else 'Not covered'}")
        for (a, b), (h, r) in sorted(m["trans"].items()):
            if reset_filter and r:
                continue
            if not uncovered_only or not h:
                out.append(f"    {nm(a) + ' -> ' + nm(b):<26} {h if h else 'Not covered'}{'  (reset)' if r else ''}")
        for (a, b), h in sorted(m["extra"].items()):
            out.append(f"    {nm(a) + ' -> ' + nm(b):<26} {h}  (not in the next-state logic)")
    return "\n".join(out)


def write_lcov(db, path, exclude, files=None):
    files = files if files is not None else by_file(db)
    rx = re.compile(exclude) if exclude else None
    out = ["TN:msim\n"]
    for src in sorted(f for f, (hits, _) in files.items() if hits):
        if rx and rx.search(src):
            continue
        hits, br = files[src]
        out.append(f"SF:{src}\n")
        brs = sorted((ln, n, t, fl) for ln, arms in br.items() for n, t, fl in arms)
        for ln, n, t, fl in brs:
            out.append(f"BRDA:{ln},{n},0,{t}\nBRDA:{ln},{n},1,{fl}\n")
        if brs:
            out.append(f"BRF:{2 * len(brs)}\nBRH:{sum((t > 0) + (fl > 0) for _, _, t, fl in brs)}\n")
        lines = sorted(hits.items())
        out.extend(f"DA:{ln},{h}\n" for ln, h in lines)
        out.append(f"LF:{len(lines)}\nLH:{sum(h > 0 for _, h in lines)}\nend_of_record\n")
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(out))


def write_html(db, outdir, exclude, files=None, reset_filter=False):
    os.makedirs(outdir, exist_ok=True)
    files = files if files is not None else by_file(db)
    per, th, ta = summarize(db, exclude, files)
    style = ("body{font:14px system-ui,sans-serif;margin:24px}table{border-collapse:collapse}"
             "td,th{border:1px solid #ccc;padding:3px 8px}pre{margin:0}.hit{background:#e5f4e4}"
             ".miss{background:#fbebeb}.part{background:#f9eeda}.src td{border:none;padding:0 8px;"
             "font:13px ui-monospace,monospace;white-space:pre}.n{color:#888;text-align:right}")
    rows = []
    for i, (src, (lh, lt, bh, bt)) in enumerate(sorted(per.items())):
        page = f"f{i}.html"
        rows.append(f"<tr><td><a href='{page}'>{html.escape(src)}</a></td><td>{pct(lh, lt):.1f}% ({lh}/{lt})</td>"
                    f"<td>{pct(bh, bt):.1f}% ({bh}/{bt})</td></tr>")
        hits, arms = files[src]
        br = {ln: [(t, fl) for _, t, fl in a] for ln, a in arms.items()}
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
    rx = re.compile(exclude) if exclude else None
    for name, (w, r, fl) in sorted(db["toggles"].items()):
        if rx and rx.search(name):
            continue
        both = r & fl
        n = popcount(both)
        if n < w:
            missing = [str(b) for b in range(w) if not (both >> b) & 1]
            togs.append(f"<tr><td>{html.escape(name)}</td><td>{n}/{w}</td><td>{', '.join(missing[:32])}"
                        f"{' ...' if len(missing) > 32 else ''}</td></tr>")
    lh = sum(v[0] for v in per.values()); lt = sum(v[1] for v in per.values())
    bh = sum(v[2] for v in per.values()); bt = sum(v[3] for v in per.values())
    esc = html.escape
    cell = lambda h: f"<td class='{'hit' if h else 'miss'}'>{h if h else 'Not covered'}</td>"
    ch, ct, cn = cond_summary(db, exclude)
    (sh, st, xh, xt), fn = fsm_summary(db, exclude, reset_filter)
    conds = []
    for (src, ln, col, text), (op, terms, hits) in sorted(db["conds"].items()):
        if rx and rx.search(src):
            continue
        head = "".join(f"<th>{esc(t)}</th>" for t in terms)
        body = "".join("<tr>" + "".join(f"<td>{v}</td>" for v in vals) + cell(h) + "</tr>"
                       for vals, h in cond_rows(op, terms, hits))
        conds.append(f"<h3>{esc(src)}:{ln} &nbsp; <code>{esc(text)}</code></h3>"
                     f"<table><tr>{head}<th>Hits</th></tr>{body}</table>")
    fsms = []
    for (mod, inst, name), m in sorted(db["fsms"].items()):
        if rx and (rx.search(m["file"]) or rx.search(f"{inst}.{name}")):
            continue
        nm = lambda v: m["states"][v][0] if v in m["states"] else f"{m['width']}'h{v:x}"
        a, b, c, d = fsm_score(m, reset_filter)
        srow = "".join(f"<tr><td>{esc(n)}</td><td>{m['width']}'h{v:x}</td>{cell(h)}</tr>"
                       for v, (n, h) in sorted(m["states"].items()))
        trow = "".join(f"<tr><td>{esc(nm(x))} &rarr; {esc(nm(y))}</td><td>{'reset' if r else ''}</td>{cell(h)}</tr>"
                       for (x, y), (h, r) in sorted(m["trans"].items()) if not (reset_filter and r))
        trow += "".join(f"<tr class='part'><td>{esc(nm(x))} &rarr; {esc(nm(y))}</td><td>not in the next-state logic</td>"
                        f"<td>{h}</td></tr>" for (x, y), h in sorted(m["extra"].items()))
        fsms.append(f"<h3>{esc(inst)}.{esc(name)} &nbsp; (module {esc(mod)}, {esc(m['file'])}:{m['line']})</h3>"
                    f"<p>States {a}/{b}, transitions {c}/{d}</p>"
                    f"<table><tr><th>State</th><th>Value</th><th>Hits</th></tr>{srow}</table><br>"
                    f"<table><tr><th>Transition</th><th></th><th>Hits</th></tr>{trow}</table>")
    with open(os.path.join(outdir, "index.html"), "w", encoding="utf-8") as f:
        f.write(f"<!DOCTYPE html><html><head><meta charset='utf-8'><title>mcov</title><style>{style}</style>"
                f"</head><body><h1>Coverage</h1><p>{len(db['tests'])} run(s). Line {pct(lh, lt):.1f}% ({lh}/{lt}), "
                f"branch {pct(bh, bt):.1f}% ({bh}/{bt}), toggle {pct(th, ta):.1f}% ({th}/{ta} bits), "
                f"condition {pct(ch, ct):.1f}% ({ch}/{ct} vectors), FSM states {pct(sh, st):.1f}% ({sh}/{st}), "
                f"FSM transitions {pct(xh, xt):.1f}% ({xh}/{xt})</p>"
                f"<table><tr><th>File</th><th>Line</th><th>Branch</th></tr>{''.join(rows)}</table>"
                f"<h2>FSMs</h2>{''.join(fsms) or '<p>None found.</p>'}"
                f"<h2>Conditions</h2>{''.join(conds) or '<p>None.</p>'}"
                f"<h2>Signals not fully toggled</h2><table><tr><th>Signal</th><th>Bits toggled both ways</th>"
                f"<th>Missing bits</th></tr>{''.join(togs)}</table></body></html>")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mcov", description="Merge and report msim coverage (see module docstring).")
    ap.add_argument("-version", action="version", version=f"mcov {VERSION}")
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("merge", help="merge databases")
    m.add_argument("dbs", nargs="+")
    m.add_argument("-o", required=True, help="merged database")
    m.add_argument("-j", type=int, default=0, help="worker processes for loading (default: available CPUs)")
    r = sub.add_parser("report", help="print a summary; optionally HTML / LCOV")
    r.add_argument("dbs", nargs="+", help="databases (merged first if several)")
    r.add_argument("-html", metavar="DIR")
    r.add_argument("-lcov", metavar="FILE")
    r.add_argument("-exclude", metavar="REGEX", help="leave out files / signals matching this")
    r.add_argument("-j", type=int, default=0, help="worker processes for loading (default: available CPUs)")
    r.add_argument("-detail", action="store_true", help="list every condition vector, FSM state and transition")
    r.add_argument("-uncovered", action="store_true", help="as -detail, only what is not covered")
    r.add_argument("-fsm-reset-filter", action="store_true",
                   help="leave transitions into reset states out of FSM coverage")
    for k in ("line", "branch", "toggle", "cond", "fsm-state", "fsm-trans"):
        r.add_argument(f"-min-{k}", type=float, metavar="PCT", help=f"exit 1 if {k} coverage is below PCT")
    a = ap.parse_args(argv)

    db = load_all(a.dbs, a.j)
    if a.cmd == "merge":
        save(db, a.o)
        print(f"mcov: merged {len(a.dbs)} database(s) ({len(db['tests'])} run(s)) into {a.o}")
        return 0
    files = by_file(db)
    per, th, ta = summarize(db, a.exclude, files)
    lh = sum(v[0] for v in per.values()); lt = sum(v[1] for v in per.values())
    bh = sum(v[2] for v in per.values()); bt = sum(v[3] for v in per.values())
    w = max([len(f) for f in per] + [4])
    print(f"{'File':<{w}}  {'Line':>16}  {'Branch':>16}")
    for f, (a1, a2, b1, b2) in sorted(per.items()):
        print(f"{f:<{w}}  {pct(a1, a2):6.1f}% {a1:>4}/{a2:<4}  {pct(b1, b2):6.1f}% {b1:>4}/{b2:<4}")
    print(f"{'Total':<{w}}  {pct(lh, lt):6.1f}% {lh:>4}/{lt:<4}  {pct(bh, bt):6.1f}% {bh:>4}/{bt:<4}")
    print(f"Toggle: {pct(th, ta):.1f}% ({th}/{ta} bits toggled both ways), {len(db['tests'])} run(s)")
    ch, ct, cn = cond_summary(db, a.exclude)
    (sh, st, xh, xt), fn = fsm_summary(db, a.exclude, a.fsm_reset_filter)
    print(f"Condition: {pct(ch, ct):.1f}% ({ch}/{ct} vectors of {cn} condition(s))")
    print(f"FSM: states {pct(sh, st):.1f}% ({sh}/{st}), transitions {pct(xh, xt):.1f}% ({xh}/{xt}) of {fn} FSM(s)")
    if a.detail or a.uncovered:
        text = detail_text(db, a.exclude, a.fsm_reset_filter, a.uncovered)
        if text:
            print(text)
    if a.html:
        write_html(db, a.html, a.exclude, files, a.fsm_reset_filter)
        print(f"HTML: {os.path.join(a.html, 'index.html')}")
    if a.lcov:
        write_lcov(db, a.lcov, a.exclude, files)
        print(f"LCOV: {a.lcov}")
    rc = 0
    for k, (hit, tot) in {"line": (lh, lt), "branch": (bh, bt), "toggle": (th, ta), "cond": (ch, ct),
                          "fsm-state": (sh, st), "fsm-trans": (xh, xt)}.items():
        lim = getattr(a, "min_" + k.replace("-", "_"))
        if lim is not None and pct(hit, tot) < lim:
            print(f"mcov: {k} coverage {pct(hit, tot):.1f}% is below {lim}%")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
