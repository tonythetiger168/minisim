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
only that term has the deciding value (0 for &&, 1 for ||). A condition with
a term that && or || must be able to skip (it calls a function, or reads
through a class handle) is counted as it runs: vector i is "term i is the
first that decides", and the terms after it show as "-".

Instances: the database also has the lines, decisions and condition vectors
of each module instance by itself. ``mcov report -instances`` prints one row
per instance (its own code, signals and FSMs; not the instances below it),
and with -detail each condition's vectors per instance, so that a vector
only one of two instances of a module reaches shows.

FSM coverage (as VCS -cm fsm): for each state register msim extracted, the
states entered and the transitions made, out of those its next-state logic
can make. Transitions into a state the register's reset branch assigns are
marked; -fsm-reset-filter leaves them out of the score.

It also holds functional coverage: the bins of every covergroup type (the
instances joined by bin name, IEEE 1800-2023 19.11.3) and the statistics of
every assertion (evaluations, passes, failures). Merging adds the counts by
name. A covergroup item's coverage is its bins hit at_least times over its
bins; a type's coverage weighs its items, and the total weighs the types
(19.11, 19.9). ``-min-func`` gates the total, ``-min-assert`` the share of
assert / assume directives that passed at least once and never failed plus
cover directives that were hit.

Exclusions (as an URG exclusion file, -elfile): ``mcov report -elfile x.el``
leaves the listed items out of every number and report. One exclusion per
line, ``kind target -- reason``; # starts a comment; names take * and ?; a
file name also matches as the end of a path:

    file   rtl/dbg_*.sv                -- debug only, not in the product
    line   rtl/ctrl.sv:40-44           -- unreachable: parameter ties it off
    branch rtl/ctrl.sv:52
    cond   rtl/ctrl.sv:60              -- every vector; "cond rtl/ctrl.sv:60 3" only the third row of -detail
    toggle tb.dut.scan_*               -- or tb.dut.bus[31:24] for some bits
    fsm    tb.dut.u.state
    state  tb.dut.u.state FAIL
    trans  tb.dut.u.state RUN->FAIL
    cg     tb.dut.cg_dbg*
    assert tb.dut.a_dbg*
    inst   tb.dut.u_dbg                -- the instance and all below it

Review: ``mcov exclude all.json -o draft.el`` writes one exclusion per item
that is not covered, each with the reason "TODO"; a reviewer keeps the ones
that are justified and writes the reason. ``mcov report -elfile x.el
-el-review`` lists every exclusion with what it removed and flags the ones
that match nothing (stale), that remove items the tests did cover, or that
have no reason; ``-el-strict`` makes any of those exit status 1.

Tests: ``mcov rank run1.json run2.json ...`` takes one database per test and
orders the tests so that each next one adds the most items not yet covered
(lines, branch outcomes, toggle edges, condition vectors, FSM states and
transitions, covergroup bins, assertions). It prints what each test covers,
what only it covers, what it adds in that order and the running total; the
tests that add nothing are redundant for coverage. ``-item file:line`` (or a
signal, FSM, covergroup or assertion name) lists the tests that cover it.

Exit status: 0, or 1 when a -min-* threshold is not met (or -el-strict finds
an exclusion to review), 2 on bad usage.
"""

import argparse
import fnmatch
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
    if d.get("instances"):
        ckeys = [(files[c[0]], c[1], c[2], c[4]) for c in d.get("conditions", ())]
        insts = db["insts"]
        for name, module, lns, brs, cds in d["instances"]:
            it = insts.get(name)
            if it is None:
                it = insts[name] = {"module": module, "lines": {}, "branches": {}, "conds": {}}
            il, ib, ic = it["lines"], it["branches"], it["conds"]
            for fi, ln, hits in lns:
                k = (files[fi], ln)
                il[k] = il.get(k, 0) + hits
            for fi, ln, n, t, fl in brs:
                k = (files[fi], ln, n)
                old = ib.get(k)
                ib[k] = (old[0] + t, old[1] + fl) if old else (t, fl)
            for ci, hits in cds:
                add_inst_cond(ic, ckeys[ci], hits)
    if d.get("covergroups"):
        merge_cgs(db["cgs"], {g[0]: [g[1], g[2], {i[0]: [i[1], i[2], i[3], i[4], i[5], dict(map(tuple, i[6]))]
                                                  for i in g[3]}] for g in d["covergroups"]})
    if d.get("assertions"):
        merge_asserts(db["asserts"], {a[0]: a[1:] for a in d["assertions"]})
    return db


def add_cond(conds, key, op, terms, hits):
    old = conds.get(key)
    if old is None or len(old[2]) != len(hits):
        conds[key] = (op, terms, list(hits))
    else:
        h = old[2]
        for i, v in enumerate(hits):
            h[i] += v


def add_inst_cond(ic, key, hits):
    old = ic.get(key)
    if old is None or len(old) != len(hits):
        ic[key] = list(hits)
    else:
        for i, v in enumerate(hits):
            old[i] += v


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


def merge_cgs(out, cgs):
    """Covergroup types by name: {type: [weight, goal, {item: [kind, weight,
    goal, at_least, bins in total, {bin: hits}]}]}. Hits add; a cross lists
    only the bins that were hit, so its total is kept beside them."""
    for name, (w, goal, items) in cgs.items():
        old = out.get(name)
        if old is None:
            out[name] = [w, goal, {k: v[:5] + [dict(v[5])] for k, v in items.items()}]
            continue
        for iname, it in items.items():
            o = old[2].get(iname)
            if o is None:
                old[2][iname] = it[:5] + [dict(it[5])]
                continue
            o[3] = max(o[3], it[3])
            bins, get = o[5], o[5].get
            for b, h in it[5].items():
                bins[b] = get(b, 0) + h
            o[4] = max(o[4], it[4], len(bins))


def merge_asserts(out, asserts):
    for name, a in asserts.items():
        old = out.get(name)
        out[name] = [a[0], old[1] + a[1], old[2] + a[2], old[3] + a[3]] if old else list(a)


def empty():
    return {"lines": {}, "branches": {}, "toggles": {}, "conds": {}, "fsms": {}, "tests": [], "cgs": {}, "asserts": {},
            "insts": {}}


def load(path):
    return load_into(empty(), path)


def merge(dbs):
    out = empty()
    for db in dbs:
        merge_into(out, db)
    return out


def merge_into(out, db):
    out["tests"] += db["tests"]
    merge_cgs(out["cgs"], db["cgs"])
    merge_asserts(out["asserts"], db["asserts"])
    for name, src in db["insts"].items():
        it = out["insts"].get(name)
        if it is None:
            out["insts"][name] = {"module": src["module"], "lines": dict(src["lines"]), "branches": dict(src["branches"]),
                                  "conds": {k: list(h) for k, h in src["conds"].items()}}
            continue
        il, ib = it["lines"], it["branches"]
        for k, h in src["lines"].items():
            il[k] = il.get(k, 0) + h
        for k, (t, f) in src["branches"].items():
            old = ib.get(k)
            ib[k] = (old[0] + t, old[1] + f) if old else (t, f)
        for k, h in src["conds"].items():
            add_inst_cond(it["conds"], k, h)
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
                   {m["file"] for m in db["fsms"].values()} |
                   {k[0] for it in db["insts"].values() for part in ("lines", "branches") for k in it[part]})
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
                                     for (mod, inst, name), m in sorted(db["fsms"].items())) + "\n],\n")
        cidx = {k: i for i, k in enumerate(sorted(db["conds"]))}
        f.write('"instances": [\n' + rows(
            [name, it["module"], [[idx[fl], ln, h] for (fl, ln), h in sorted(it["lines"].items())],
             [[idx[fl], ln, n, t, fa] for (fl, ln, n), (t, fa) in sorted(it["branches"].items())],
             [[cidx[k], h] for k, h in sorted(it["conds"].items()) if k in cidx]]
            for name, it in sorted(db["insts"].items())) + "\n],\n")
        f.write('"covergroups": [\n' + rows(
            [name, w, goal, [[i, v[0], v[1], v[2], v[3], v[4],
                              [[b, h] for b, h in v[5].items() if h or v[0] != "cross"]] for i, v in items.items()]]
            for name, (w, goal, items) in sorted(db["cgs"].items())) + "\n],\n")
        f.write('"assertions": [\n' + rows([name] + list(a) for name, a in sorted(db["asserts"].items())) + "\n]\n}\n")


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
        tot += sum(1 for h in hits if h >= 0)   # -1: excluded by -elfile
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


def inst_of(name, insts):
    """The instance that declares a hierarchical name: its longest prefix that is an instance."""
    while "." in name:
        name = name.rsplit(".", 1)[0]
        if name in insts:
            return name
    return None


def inst_summary(db, exclude, reset_filter):
    """{instance: [module, line hit, lines, branch hit, outcomes, vector hit,
    vectors, toggle hit, bits, state hit, states, transition hit, transitions]},
    each instance counted by itself (not with the instances below it)."""
    rx = re.compile(exclude) if exclude else None
    insts = db["insts"]
    out = {}
    for name, it in insts.items():
        if rx and rx.search(name):
            continue
        r = [it["module"]] + [0] * 12
        for (f, _), h in it["lines"].items():
            if not (rx and rx.search(f)):
                r[1] += h > 0; r[2] += 1
        for (f, _, _), (t, fl) in it["branches"].items():
            if not (rx and rx.search(f)):
                r[3] += (t > 0) + (fl > 0); r[4] += 2
        for (f, _, _, _), hits in it["conds"].items():
            if not (rx and rx.search(f)):
                r[5] += sum(1 for h in hits if h > 0); r[6] += sum(1 for h in hits if h >= 0)
        out[name] = r
    for sig, (w, rise, fall) in db["toggles"].items():
        if rx and rx.search(sig):
            continue
        r = out.get(inst_of(sig, insts))
        if r:
            r[7] += popcount(rise & fall); r[8] += w
    for (mod, inst, name), m in db["fsms"].items():
        r = out.get(inst)
        if r and not (rx and (rx.search(m["file"]) or rx.search(f"{inst}.{name}"))):
            for i, v in enumerate(fsm_score(m, reset_filter)):
                r[9 + i] += v
    return out


def inst_text(db, exclude, reset_filter):
    summ = inst_summary(db, exclude, reset_filter)
    if not summ:
        return "Instances: none in the database (written by msim 0.1 or older?)"
    w = max([len(n) for n in summ] + [8])
    mw = max([len(r[0]) for r in summ.values()] + [6])
    cell = lambda a, b: f"{pct(a, b):5.1f}% {a:>5}/{b:<5}" if b else f"{'-':>18}"
    out = [f"{'Instance':<{w}}  {'Module':<{mw}}  {'Line':>18}  {'Branch':>18}  {'Condition':>18}  {'Toggle':>18}  "
           f"{'FSM state':>18}  {'FSM transition':>18}"]
    for name, r in sorted(summ.items()):
        out.append(f"{name:<{w}}  {r[0]:<{mw}}  " + "  ".join(cell(r[i], r[i + 1]) for i in range(1, 13, 2)))
    return "\n".join(out)


def cond_rows(op, terms, hits):
    """One (values per term, hits) row per vector of a condition, in report order."""
    n = len(terms)
    if op == "?:":
        return [(("1",), hits[0]), (("0",), hits[1])]
    base, dec = ("0", "1") if op.startswith("||") else ("1", "0")
    rows = [((base,) * n, hits[0])]
    lazy = op.endswith("s")   # short-circuit: the terms after the deciding one did not run
    for i in range(n):
        rows.append((tuple(dec if j == i else "-" if lazy and j > i else base for j in range(n)), hits[i + 1]))
    return rows


def detail_text(db, exclude, reset_filter, uncovered_only, per_inst=False):
    rx = re.compile(exclude) if exclude else None
    out = []
    by_cond = {}
    if per_inst:
        for name, it in db["insts"].items():
            for k, h in it["conds"].items():
                by_cond.setdefault(k, []).append((name, h))
    for (f, ln, col, text), (op, terms, hits) in sorted(db["conds"].items()):
        if rx and rx.search(f):
            continue
        if uncovered_only and all(h != 0 for h in hits):
            continue
        out.append(f"\nCondition {f}:{ln}:{col}  {text}")
        for i, t in enumerate(terms, 1):
            out.append(f"    -{i}- {t}")
        out.append("    " + " ".join(f"-{i}-" for i in range(1, len(terms) + 1)) + "  Hits")
        for vals, h in cond_rows(op, terms, hits):
            out.append("    " + " ".join(f"{v:^{len(str(i)) + 2}}" for i, v in enumerate(vals, 1)) +
                       f"  {'Excluded' if h < 0 else h if h else 'Not covered'}")
        # the same rows per instance: a vector one instance never saw shows as 0
        for name, ih in sorted(by_cond.get((f, ln, col, text), ())):
            if len(ih) == len(hits) and not (uncovered_only and all(h != 0 for h in ih)):
                out.append(f"    {name}: " + " ".join("x" if h < 0 else str(h) for h in ih)
                           + f"  ({sum(1 for h in ih if h > 0)}/{sum(1 for h in ih if h >= 0)})")
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
def item_count(item):
    """(bins covered, bins) of a covergroup item [kind, weight, goal, at_least, total, {bin: hits}]."""
    al = max(1, item[3])
    return sum(1 for h in item[5].values() if h >= al), item[4]


def cg_pct(items):
    """A covergroup type's coverage: its items by weight; an item without bins is left out (19.11)."""
    num = den = 0.0
    for it in items.values():
        c, t = item_count(it)
        if t:
            num += it[1] * c / t
            den += it[1]
    return 100.0 * num / den if den else 100.0


def summarize_func(db, exclude=None):
    """Functional coverage: ({type: (percent, bins covered, bins)}, total percent
    over the types by weight), and the assertions: (directives that count as
    covered, directives, [failed], [never passed])."""
    rx = re.compile(exclude) if exclude else None
    per, num, den = {}, 0.0, 0.0
    for name, (w, _goal, items) in db["cgs"].items():
        if rx and rx.search(name):
            continue
        counts = [item_count(it) for it in items.values()]
        p = cg_pct(items)
        per[name] = (p, sum(c for c, _ in counts), sum(t for _, t in counts))
        num += w * p
        den += w
    failed, idle, ok, n = [], [], 0, 0
    for name, (kind, _evals, passes, fails) in db["asserts"].items():
        if rx and rx.search(name):
            continue
        n += 1
        if fails and kind != "cover":
            failed.append(name)
        elif not passes:
            idle.append(name)
        else:
            ok += 1
    return per, (num / den if den else 100.0), (ok, n, sorted(failed), sorted(idle))


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
    cell = lambda h: "<td>Excluded</td>" if h < 0 else f"<td class='{'hit' if h else 'miss'}'>{h if h else 'Not covered'}</td>"
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
                f"{inst_html(db, exclude, reset_filter)}"
                f"<h2>FSMs</h2>{''.join(fsms) or '<p>None found.</p>'}"
                f"<h2>Conditions</h2>{''.join(conds) or '<p>None.</p>'}"
                f"<h2>Signals not fully toggled</h2><table><tr><th>Signal</th><th>Bits toggled both ways</th>"
                f"<th>Missing bits</th></tr>{''.join(togs)}</table>{func_html(db, exclude)}</body></html>")


def inst_html(db, exclude, reset_filter):
    summ = inst_summary(db, exclude, reset_filter)
    if not summ:
        return ""
    def cell(a, b):
        if not b:
            return "<td></td>"
        return f"<td class='{'hit' if a == b else 'part' if a else 'miss'}'>{pct(a, b):.1f}% ({a}/{b})</td>"
    rows = "".join(f"<tr><td>{html.escape(n)}</td><td>{html.escape(r[0])}</td>"
                   + "".join(cell(r[i], r[i + 1]) for i in range(1, 13, 2)) + "</tr>" for n, r in sorted(summ.items()))
    return ("<h2>Instances</h2><table><tr><th>Instance</th><th>Module</th><th>Line</th><th>Branch</th><th>Condition</th>"
            f"<th>Toggle</th><th>FSM states</th><th>FSM transitions</th></tr>{rows}</table>")


def func_html(db, exclude):
    """Covergroups (items with their uncovered bins) and assertions, for index.html."""
    per, total, (ok, n, failed, idle) = summarize_func(db, exclude)
    if not per and not n:
        return ""
    esc = html.escape
    rows = []
    for name in sorted(per):
        p, c, t = per[name]
        rows.append(f"<tr class='{'hit' if c == t else 'part'}'><td><b>{esc(name)}</b></td><td>{p:.1f}% ({c}/{t})</td><td></td></tr>")
        for iname, it in db["cgs"][name][2].items():
            ic, itot = item_count(it)
            al = max(1, it[3])
            holes = [b for b, h in it[5].items() if h < al]
            miss = ", ".join(holes[:24]) + (" ..." if len(holes) > 24 else "")
            if it[0] == "cross" and ic < itot and not holes:
                miss = f"{itot - ic} cross bins never hit"
            rows.append(f"<tr class='{'hit' if ic == itot else 'miss' if not ic else 'part'}'><td>&nbsp;&nbsp;{esc(iname)}"
                        f" ({it[0]})</td><td>{pct(ic, itot):.1f}% ({ic}/{itot})</td><td>{esc(miss)}</td></tr>")
    arows = []
    for name, (kind, evals, passes, fails) in sorted(db["asserts"].items()):
        if name in failed or name in idle:
            state = "failed" if name in failed else "never passed" if kind != "cover" else "never hit"
            arows.append(f"<tr class='{'miss' if name in failed else 'part'}'><td>{esc(name)}</td><td>{kind}</td>"
                         f"<td>{state}</td><td>{evals}</td><td>{passes}</td><td>{fails}</td></tr>")
    out = ""
    if per:
        out += (f"<h2>Functional coverage: {total:.1f}%</h2><table><tr><th>Covergroup / item</th><th>Coverage</th>"
                f"<th>Bins not covered</th></tr>{''.join(rows)}</table>")
    if n:
        out += (f"<h2>Assertions: {ok}/{n} passed or hit</h2><table><tr><th>Assertion</th><th>Kind</th><th>State</th>"
                f"<th>Evaluations</th><th>Passes</th><th>Failures</th></tr>{''.join(arows)}</table>")
    return out


# ---- exclusions ------------------------------------------------------------

EL_KINDS = ("file", "line", "branch", "cond", "toggle", "fsm", "state", "trans", "cg", "assert", "inst")


def read_elfile(path):
    """[{kind, target, arg, reason, where}] of an exclusion file."""
    out = []
    try:
        f = open(path, encoding="utf-8")
    except OSError as e:
        raise SystemExit(f"mcov: {e}")
    with f:
        for no, raw in enumerate(f, 1):
            text = raw.split("#", 1)[0].strip()
            if not text:
                continue
            body, _, reason = text.partition("--")
            w = body.split()
            if w[0] not in EL_KINDS or len(w) < 2 or len(w) > 3:
                raise SystemExit(f"mcov: {path}:{no}: expected '<kind> <target> [-- reason]', kind one of {', '.join(EL_KINDS)}")
            out.append({"kind": w[0], "target": w[1], "arg": w[2] if len(w) == 3 else "", "reason": reason.strip(),
                        "where": f"{path}:{no}", "n": 0, "covered": 0})
    return out


def _file_match(path, pat):
    return fnmatch.fnmatchcase(path, pat) or fnmatch.fnmatchcase(path, "*/" + pat)


def _file_lines(target, where):
    """'a.sv:10-12' -> ('a.sv', 10, 12)."""
    f, _, rng = target.rpartition(":")
    lo, _, hi = rng.partition("-")
    if not f or not lo.isdigit() or (hi and not hi.isdigit()):
        raise SystemExit(f"mcov: {where}: expected <file>:<line> or <file>:<line>-<line>, not '{target}'")
    return f, int(lo), int(hi or lo)


def apply_exclusions(db, rules):
    """Take the excluded items out of db. Each rule gets n (items removed)
    and covered (how many of those the tests had covered)."""
    for r in rules:
        kind, tg, arg, where = r["kind"], r["target"], r["arg"], r["where"]
        n = cov = 0
        if kind in ("file", "line", "branch", "cond"):
            if kind == "file":
                pat, lo, hi = tg, 0, 1 << 60
            else:
                pat, lo, hi = _file_lines(tg, where)
            fm = {}
            ok = lambda f, ln: lo <= ln <= hi and (fm[f] if f in fm else fm.setdefault(f, _file_match(f, pat)))
            for it in db["insts"].values():      # the per-instance copies; counted once, below
                if kind in ("file", "line"):
                    for k in [k for k in it["lines"] if ok(k[0], k[1])]:
                        del it["lines"][k]
                if kind in ("file", "branch"):
                    for k in [k for k in it["branches"] if ok(k[0], k[1])]:
                        del it["branches"][k]
                if kind in ("file", "cond"):
                    for k in [k for k in it["conds"] if ok(k[0], k[1])]:
                        if arg.isdigit():
                            if 0 < int(arg) <= len(it["conds"][k]):
                                it["conds"][k][int(arg) - 1] = -1
                        else:
                            del it["conds"][k]
            if kind in ("file", "line"):
                for k in [k for k in db["lines"] if ok(k[0], k[1])]:
                    n += 1; cov += db["lines"].pop(k) > 0
            if kind in ("file", "branch"):
                for k in [k for k in db["branches"] if ok(k[0], k[1])]:
                    t, fl = db["branches"].pop(k)
                    n += 2; cov += (t > 0) + (fl > 0)
            if kind in ("file", "cond"):
                if arg and not arg.isdigit():
                    raise SystemExit(f"mcov: {where}: the vector of a cond exclusion is a row number, not '{arg}'")
                for k in [k for k in db["conds"] if ok(k[0], k[1])]:
                    hits = db["conds"][k][2]
                    if arg:
                        i = int(arg) - 1
                        if 0 <= i < len(hits) and hits[i] >= 0:
                            n += 1; cov += hits[i] > 0
                            hits[i] = -1
                    else:
                        live = [h for h in hits if h >= 0]
                        n += len(live); cov += sum(1 for h in live if h > 0)
                        del db["conds"][k]
            if kind == "file":
                for k in [k for k, m in db["fsms"].items() if ok(m["file"], m["line"])]:
                    m = db["fsms"].pop(k)
                    n += len(m["states"]) + len(m["trans"])
                    cov += sum(1 for _, h in m["states"].values() if h > 0) + sum(1 for h, _ in m["trans"].values() if h > 0)
        elif kind == "toggle":
            mt = re.fullmatch(r"(.*)\[(\d+)(?::(\d+))?\]", tg)
            pat, bits = tg, None
            if mt and not any(fnmatch.fnmatchcase(name, tg) for name in db["toggles"]):
                a, b = int(mt.group(2)), int(mt.group(3) or mt.group(2))
                pat, bits = mt.group(1), ((1 << (max(a, b) + 1)) - 1) & ~((1 << min(a, b)) - 1)
            for name in [x for x in db["toggles"] if fnmatch.fnmatchcase(x, pat)]:
                w, rise, fall = db["toggles"][name]
                if bits is None:
                    n += w; cov += popcount(rise & fall)
                    del db["toggles"][name]
                else:
                    m = bits & ((1 << w) - 1)
                    k = popcount(m)
                    if not k:
                        continue
                    n += k; cov += popcount(rise & fall & m)
                    # the bits left are packed down so that width stays the number of bits counted
                    keep = [i for i in range(w) if not (m >> i) & 1]
                    pack = lambda v: sum(((v >> b) & 1) << i for i, b in enumerate(keep))
                    if keep:
                        db["toggles"][name] = (len(keep), pack(rise), pack(fall))
                    else:
                        del db["toggles"][name]
        elif kind in ("fsm", "state", "trans"):
            for key in [k for k in db["fsms"] if fnmatch.fnmatchcase(f"{k[1]}.{k[2]}", tg)]:
                m = db["fsms"][key]
                val = {name: v for v, (name, _) in m["states"].items()}
                if kind == "fsm":
                    n += len(m["states"]) + len(m["trans"])
                    cov += sum(1 for _, h in m["states"].values() if h > 0) + sum(1 for h, _ in m["trans"].values() if h > 0)
                    del db["fsms"][key]
                elif kind == "state":
                    for v in [v for name, v in val.items() if fnmatch.fnmatchcase(name, arg)]:
                        n += 1; cov += m["states"].pop(v)[1] > 0
                        for t in [t for t in m["trans"] if v in t]:
                            n += 1; cov += m["trans"].pop(t)[0] > 0
                        for t in [t for t in m["extra"] if v in t]:
                            del m["extra"][t]
                else:
                    a, sep, b = arg.partition("->")
                    if not sep:
                        raise SystemExit(f"mcov: {where}: a trans exclusion is '<fsm> <FROM>-><TO>'")
                    name = {v: nm for nm, v in val.items()}
                    for t in [t for t in m["trans"] if fnmatch.fnmatchcase(name.get(t[0], ""), a)
                              and fnmatch.fnmatchcase(name.get(t[1], ""), b)]:
                        n += 1; cov += m["trans"].pop(t)[0] > 0
        elif kind == "inst":
            # An instance and all below it: its lines, decisions and vectors
            # leave the totals unless another instance of the module keeps them.
            gone = [x for x in db["insts"] if fnmatch.fnmatchcase(x, tg) or fnmatch.fnmatchcase(x, tg + ".*")]
            dead = [db["insts"].pop(x) for x in gone]
            lines, branches, conds = db["lines"], db["branches"], db["conds"]
            keep_l, keep_c = {}, {}
            for it in db["insts"].values():
                for k, h in it["lines"].items():
                    keep_l[k] = keep_l.get(k, 0) + h
                for k in it["conds"]:
                    keep_c[k] = True
            for it in dead:
                for k, h in it["lines"].items():
                    if k in lines and k not in keep_l:
                        n += 1; cov += lines.pop(k) > 0
                    elif k in lines:
                        lines[k] = keep_l[k]
                for k in it["branches"]:
                    if k in branches:
                        t, fl = branches.pop(k)
                        n += 2; cov += (t > 0) + (fl > 0)
                for k, ih in it["conds"].items():
                    if k not in conds:
                        continue
                    if k not in keep_c:
                        live = [h for h in conds.pop(k)[2] if h >= 0]
                        n += len(live); cov += sum(1 for h in live if h > 0)
                    elif len(ih) == len(conds[k][2]):
                        hits = conds[k][2]
                        for i, h in enumerate(ih):
                            if hits[i] >= 0 and h > 0:
                                hits[i] -= h
            pre = tuple(g + "." for g in gone)
            for sig in [x for x in db["toggles"] if x.startswith(pre)] if pre else ():
                if inst_of(sig, set(gone) | set(db["insts"])) in gone:
                    w, rise, fall = db["toggles"].pop(sig)
                    n += w; cov += popcount(rise & fall)
            for key in [k for k in db["fsms"] if k[1] in gone]:
                m = db["fsms"].pop(key)
                n += len(m["states"]) + len(m["trans"])
                cov += sum(1 for _, h in m["states"].values() if h > 0) + sum(1 for h, _ in m["trans"].values() if h > 0)
        elif kind == "cg":
            for name in [x for x in db["cgs"] if fnmatch.fnmatchcase(x, tg)]:
                for it in db["cgs"].pop(name)[2].values():
                    c, t = item_count(it)
                    n += t; cov += c
        elif kind == "assert":
            for name in [x for x in db["asserts"] if fnmatch.fnmatchcase(x, tg)]:
                a = db["asserts"].pop(name)
                n += 1; cov += bool(a[2]) and not (a[3] and a[0] != "cover")
        r["n"] += n
        r["covered"] += cov
    return rules


def el_issue(r):
    """What a reviewer should look at in an applied exclusion, or ''."""
    if not r["n"]:
        return "matches nothing (stale?)"
    if r["covered"]:
        return f"{r['covered']} of its {r['n']} item(s) are covered by the tests"
    if not r["reason"] or r["reason"].upper().startswith("TODO"):
        return "no reason given"
    return ""


def el_review_text(rules):
    out = ["Exclusions:"]
    for r in rules:
        tgt = r["target"] + (" " + r["arg"] if r["arg"] else "")
        issue = el_issue(r)
        out.append(f"  {r['where']}: {r['kind']} {tgt}: {r['n']} item(s)"
                   + (f"  -- {r['reason']}" if r["reason"] else "") + (f"  ** {issue}" if issue else ""))
    return "\n".join(out)


def draft_exclusions(db, reset_filter=False):
    """Exclusion file text: one exclusion per item that is not covered."""
    out = ["# mcov exclusion draft: every item below is not covered. Keep an exclusion only if the",
           "# item cannot or need not be covered, and replace TODO with the reason.", ""]
    todo = "  -- TODO"
    runs = {}
    for (f, ln), h in sorted(db["lines"].items()):
        if not h:
            runs.setdefault(f, []).append(ln)
    for f, lns in runs.items():
        i = 0
        while i < len(lns):
            j = i
            while j + 1 < len(lns) and lns[j + 1] == lns[j] + 1:
                j += 1
            out.append(f"line   {f}:{lns[i]}" + (f"-{lns[j]}" if j > i else "") + todo)
            i = j + 1
    for (f, ln) in sorted({(f, ln) for (f, ln, _), (t, fl) in db["branches"].items() if not (t and fl)}):
        out.append(f"branch {f}:{ln}{todo}")
    for (f, ln, col, text), (op, terms, hits) in sorted(db["conds"].items()):
        miss = [i for i, h in enumerate(hits, 1) if h == 0]
        if len(miss) == len(hits):
            out.append(f"cond   {f}:{ln}{todo}   # {text}")
        else:
            out += [f"cond   {f}:{ln} {i}{todo}   # {text}" for i in miss]
    for name, (w, r, fl) in sorted(db["toggles"].items()):
        miss = [i for i in range(w) if not (r & fl) >> i & 1]
        if len(miss) == w:
            out.append(f"toggle {name}{todo}")
            continue
        i = 0
        while i < len(miss):
            j = i
            while j + 1 < len(miss) and miss[j + 1] == miss[j] + 1:
                j += 1
            out.append(f"toggle {name}[{miss[j]}:{miss[i]}]{todo}" if j > i else f"toggle {name}[{miss[i]}]{todo}")
            i = j + 1
    for (mod, inst, name), m in sorted(db["fsms"].items()):
        nm = lambda v: m["states"][v][0]
        dead = {v for v, (_, h) in m["states"].items() if not h}
        for v in sorted(dead):
            out.append(f"state  {inst}.{name} {nm(v)}{todo}")
        for (a, b), (h, r) in sorted(m["trans"].items()):
            if not h and a not in dead and b not in dead and not (reset_filter and r):
                out.append(f"trans  {inst}.{name} {nm(a)}->{nm(b)}{todo}")
    for name, a in sorted(db["asserts"].items()):
        if not a[2]:
            out.append(f"assert {name}{todo}")
    return "\n".join(out) + "\n"


# ---- test ranking ----------------------------------------------------------

def test_items(db):
    """(every item of the database, the ones this run covered), as keys that
    are the same in every run of the same design."""
    all_, hit = [], []
    def add(k, h):
        all_.append(k)
        if h:
            hit.append(k)
    for k, h in db["lines"].items():
        add(("l",) + k, h > 0)
    for k, (t, f) in db["branches"].items():
        add(("b",) + k + (1,), t > 0)
        add(("b",) + k + (0,), f > 0)
    for name, (w, r, f) in db["toggles"].items():
        for i in range(w):
            add(("t", name, i, 1), (r >> i) & 1)
            add(("t", name, i, 0), (f >> i) & 1)
    for k, (_, _, hits) in db["conds"].items():
        for i, h in enumerate(hits):
            add(("c",) + k + (i,), h > 0)
    for k, m in db["fsms"].items():
        for v, (_, h) in m["states"].items():
            add(("s",) + k + (v,), h > 0)
        for t, (h, _) in m["trans"].items():
            add(("x",) + k + t, h > 0)
    for g, (_, _, items) in db["cgs"].items():
        for i, it in items.items():
            al = max(1, it[3])
            for b, h in it[5].items():
                add(("g", g, i, b), h >= al)
    for name, a in db["asserts"].items():
        add(("a", name), bool(a[2]) and not (a[3] and a[0] != "cover"))
    return all_, hit


def _rank_load(path):
    db = load(path)
    return (path,) + test_items(db)


def item_label(k):
    c = k[0]
    if c == "l": return f"{k[1]}:{k[2]}"
    if c == "b": return f"{k[1]}:{k[2]} branch {k[3]} {'taken' if k[4] else 'not taken'}"
    if c == "t": return f"{k[1]}[{k[2]}] {'rise' if k[3] else 'fall'}"
    if c == "c": return f"{k[1]}:{k[2]}:{k[3]} vector {k[5] + 1}  {k[4]}"
    if c == "s": return f"{k[2]}.{k[3]} state {k[4]}"
    if c == "x": return f"{k[2]}.{k[3]} transition {k[4]}->{k[5]}"
    if c == "g": return f"{k[1]}.{k[2]} bin {k[3]}"
    return k[1]


def test_layout(db):
    """The items of a run as a layout and a hit mask: the same items as
    test_items, but a signal's toggle edges are one segment of 2 * width bits
    (rise bits, then fall bits) instead of a tuple per bit. Returns (singles,
    toggles, mask, nbits): singles are the keys of one-bit items, toggles
    (name, width) pairs; the mask has the singles first (padded to a byte),
    then each toggle segment padded to a byte."""
    singles, hit = test_items_no_toggles(db)
    nb = (len(singles) + 7) // 8
    buf = bytearray(nb)
    for i in hit:
        buf[i >> 3] |= 1 << (i & 7)
    parts = [bytes(buf)]
    toggles = []
    nbits = len(singles)
    for name, (w, r, f) in db["toggles"].items():
        toggles.append((name, w))
        nbits += 2 * w
        parts.append((r | (f << w)).to_bytes((2 * w + 7) // 8, "little"))
    return singles, toggles, int.from_bytes(b"".join(parts), "little"), nbits


def test_items_no_toggles(db):
    """(keys, indexes of the covered ones) of every item of test_items but the toggles."""
    keys, hit = [], []
    def add(k, h):
        if h:
            hit.append(len(keys))
        keys.append(k)
    for k, h in db["lines"].items():
        add(("l",) + k, h > 0)
    for k, (t, f) in db["branches"].items():
        add(("b",) + k + (1,), t > 0)
        add(("b",) + k + (0,), f > 0)
    for k, (_, _, hits) in db["conds"].items():
        for i, h in enumerate(hits):
            add(("c",) + k + (i,), h > 0)
    for k, m in db["fsms"].items():
        for v, (_, h) in m["states"].items():
            add(("s",) + k + (v,), h > 0)
        for t, (h, _) in m["trans"].items():
            add(("x",) + k + t, h > 0)
    for g, (_, _, items) in db["cgs"].items():
        for i, it in items.items():
            al = max(1, it[3])
            for b, h in it[5].items():
                add(("g", g, i, b), h >= al)
    for name, a in db["asserts"].items():
        add(("a", name), bool(a[2]) and not (a[3] and a[0] != "cover"))
    return keys, hit


def _layout_digest(singles, toggles):
    import hashlib
    h = hashlib.blake2b(digest_size=16)
    h.update(repr(singles).encode())
    h.update(repr(toggles).encode())
    return h.digest()


def _rank_mask(path):
    """In a worker: (digest of the run's layout, its hit mask, its item count).
    The layout itself stays in the worker; runs of one design share it."""
    singles, toggles, mask, nbits = test_layout(load(path))
    return _layout_digest(singles, toggles), mask, nbits


def _remap(mask, src, dst):
    """A mask over layout src as a mask over layout dst (dst has every item of src)."""
    s_singles, s_togs = src
    d_singles, d_togs, d_index, d_togoff = dst
    out = bytearray((d_togoff[-1] + 7) // 8 if d_togoff else (len(d_singles) + 7) // 8)
    low = mask & ((1 << len(s_singles)) - 1)
    i = 0
    while low:
        if low & 1:
            j = d_index[s_singles[i]]
            out[j >> 3] |= 1 << (j & 7)
        low >>= 1
        i += 1
    m = int.from_bytes(out, "little")
    off = (len(s_singles) + 7) // 8 * 8
    for name, w in s_togs:
        seg = (mask >> off) & ((1 << (2 * w)) - 1)
        if seg:
            m |= seg << d_togoff[d_index[("T", name, w)]]
        off += (2 * w + 7) // 8 * 8
    return m


def rank_tests(paths, jobs=0):
    """Greedy order of the tests by what each adds. Returns (rows, total
    items): rows are (path, covered, unique, added, cumulative), in rank
    order. Each worker loads its runs and builds their hit masks against the
    run's own layout; the masks of runs with another layout (another design
    version) are remapped onto the union once per layout. The greedy pass is
    lazy: what a test adds only shrinks as tests are taken."""
    import heapq
    jobs = max(1, min(jobs or cpu_count(), len(paths)))
    got = None
    if jobs > 1 and len(paths) > 1:
        import concurrent.futures as cf
        try:
            with cf.ProcessPoolExecutor(max_workers=jobs) as pool:
                got = list(pool.map(_rank_mask, paths, chunksize=max(1, len(paths) // (jobs * 4))))
        except SystemExit:
            raise
        except Exception:
            got = None
    if got is None:
        got = [_rank_mask(p) for p in paths]
    digests = list(dict.fromkeys(g[0] for g in got))
    if len(digests) == 1:
        masks = [g[1] for g in got]
        total = got[0][2]
    else:
        # Union of the layouts, in input order, then each run's mask moved onto it.
        layouts = {}
        for p, g in zip(paths, got):
            if g[0] not in layouts:
                sg, tg, _, _ = test_layout(load(p))
                layouts[g[0]] = (sg, tg)
        d_singles, d_index = [], {}
        for sg, _ in layouts.values():
            for k in sg:
                if k not in d_index:
                    d_index[k] = len(d_singles)
                    d_singles.append(k)
        d_togoff = []
        off = (len(d_singles) + 7) // 8 * 8
        total = len(d_singles)
        for _, tg in layouts.values():
            for name, w in tg:
                k = ("T", name, w)
                if k not in d_index:
                    d_index[k] = len(d_togoff)
                    d_togoff.append(off)
                    off += (2 * w + 7) // 8 * 8
                    total += 2 * w
        d_togoff.append(off)
        dst = (d_singles, None, d_index, d_togoff)
        masks = [_remap(g[1], layouts[g[0]], dst) for g in got]
    once = twice = 0
    for m in masks:
        twice |= once & m
        once |= m
    only = once & ~twice
    cnt = [popcount(m) for m in masks]
    heap = [(-c, i) for i, c in enumerate(cnt)]
    heapq.heapify(heap)
    cum, rows = 0, []
    while heap:
        _, i = heapq.heappop(heap)
        gain = popcount(masks[i] & ~cum)
        if heap and (-gain, i) > heap[0]:   # stale: someone else may add more now
            heapq.heappush(heap, (-gain, i))
            continue
        cum |= masks[i]
        rows.append((paths[i], cnt[i], popcount(masks[i] & only), gain, popcount(cum)))
    return rows, total


def item_tests(paths, item):
    """[(label, [tests that cover it])] of the items whose label is item or starts with it."""
    loaded = [test_items(load(p)) for p in paths]
    index = {}
    for all_, _ in loaded:
        for k in all_:
            index.setdefault(k, len(index))
    hits = [set(hit) for _, hit in loaded]
    out = []
    for k in index:
        lab = item_label(k)
        if lab == item or lab.startswith(item + " ") or lab.startswith(item + "[") or lab.startswith(item + ":"):
            out.append((lab, [p for p, h in zip(paths, hits) if k in h]))
    return out


def rank_main(a):
    if a.item:
        found = item_tests(a.dbs, a.item)
        for lab, who in found:
            print(f"{lab}: " + (", ".join(who) if who else "no test"))
        if not found:
            print(f"mcov: no coverage item matches '{a.item}'", file=sys.stderr)
            return 1
        return 0
    rows, total = rank_tests(a.dbs, a.j)
    w = max([len(r[0]) for r in rows] + [4])
    print(f"{'Rank':>4}  {'Test':<{w}}  {'Covers':>8}  {'Only it':>8}  {'Adds':>8}  {'Total':>16}")
    for n, (p, c, u, g, cum) in enumerate(rows, 1):
        print(f"{n:>4}  {p:<{w}}  {c:>8}  {u:>8}  {g:>8}  {cum:>8} {pct(cum, total):6.1f}%")
    need = sum(1 for r in rows if r[3])
    print(f"{len(rows)} test(s), {total} item(s); the first {need} reach the total, {len(rows) - need} add nothing")
    if a.o:
        with open(a.o, "w", encoding="utf-8") as f:
            f.write("".join(r[0] + "\n" for r in rows if r[3]))
        print(f"Tests that add coverage, in rank order: {a.o}")
    return 0


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
    r.add_argument("-instances", action="store_true",
                   help="coverage of each module instance; with -detail, each condition's vectors per instance")
    r.add_argument("-elfile", metavar="FILE", action="append", default=[], help="exclusion file (may be repeated)")
    r.add_argument("-el-review", action="store_true", help="list every exclusion, what it removes and what to review")
    r.add_argument("-el-strict", action="store_true",
                   help="exit 1 if an exclusion matches nothing, removes covered items or has no reason")
    for k in ("line", "branch", "toggle", "cond", "fsm-state", "fsm-trans", "func", "assert"):
        r.add_argument(f"-min-{k}", type=float, metavar="PCT", help=f"exit 1 if {k} coverage is below PCT")
    x = sub.add_parser("exclude", help="write an exclusion file draft: one exclusion per item not covered")
    x.add_argument("dbs", nargs="+", help="databases (merged first if several)")
    x.add_argument("-o", required=True, help="exclusion file to write")
    x.add_argument("-elfile", metavar="FILE", action="append", default=[], help="exclusions already agreed (left out of the draft)")
    x.add_argument("-fsm-reset-filter", action="store_true", help="leave out transitions into reset states")
    x.add_argument("-j", type=int, default=0, help="worker processes for loading (default: available CPUs)")
    k = sub.add_parser("rank", help="order tests by the coverage each adds; one database per test")
    k.add_argument("dbs", nargs="+", help="one database per test")
    k.add_argument("-o", metavar="FILE", help="write the tests that add coverage, in rank order")
    k.add_argument("-item", metavar="NAME", help="list the tests that cover this item (file:line, signal, FSM, covergroup, assertion)")
    k.add_argument("-j", type=int, default=0, help="worker processes for loading (default: available CPUs)")
    a = ap.parse_args(argv)

    if a.cmd == "rank":
        return rank_main(a)
    db = load_all(a.dbs, a.j)
    rules = []
    for path in getattr(a, "elfile", ()):
        rules += apply_exclusions(db, read_elfile(path))
    if a.cmd == "exclude":
        text = draft_exclusions(db, a.fsm_reset_filter)
        with open(a.o, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"mcov: {sum(1 for l in text.splitlines() if l and l[0] != '#')} exclusion(s) to review in {a.o}")
        return 0
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
    if a.instances:
        print(inst_text(db, a.exclude, a.fsm_reset_filter))
    if rules:
        print(f"Excluded: {sum(r['n'] for r in rules)} item(s) by {len(rules)} exclusion(s)")
        if a.el_review:
            print(el_review_text(rules))
    if a.detail or a.uncovered:
        text = detail_text(db, a.exclude, a.fsm_reset_filter, a.uncovered, a.instances)
        if text:
            print(text)
    cgs, ftotal, (aok, an, afailed, aidle) = summarize_func(db, a.exclude)
    if cgs:
        gw = max(len(g) for g in cgs)
        for g, (p, c, t) in sorted(cgs.items()):
            print(f"{g:<{gw}}  {p:6.1f}% {c:>4}/{t:<4} bins")
        print(f"Functional: {ftotal:.1f}% ({len(cgs)} covergroup type(s))")
    if an:
        print(f"Assertions: {pct(aok, an):.1f}% ({aok}/{an} passed or hit"
              + (f", {len(afailed)} failed" if afailed else "") + (f", {len(aidle)} never passed or hit" if aidle else "") + ")")
        for name in afailed:
            print(f"  failed: {name}")
    if a.html:
        write_html(db, a.html, a.exclude, files, a.fsm_reset_filter)
        print(f"HTML: {os.path.join(a.html, 'index.html')}")
    if a.lcov:
        write_lcov(db, a.lcov, a.exclude, files)
        print(f"LCOV: {a.lcov}")
    rc = 0
    issues = [r for r in rules if el_issue(r)]
    if a.el_strict and issues:
        if not a.el_review:
            for r in issues:
                print(f"mcov: {r['where']}: {r['kind']} {r['target']}: {el_issue(r)}")
        print(f"mcov: {len(issues)} exclusion(s) need review")
        rc = 1
    for k, (hit, tot) in {"line": (lh, lt), "branch": (bh, bt), "toggle": (th, ta), "cond": (ch, ct),
                          "fsm-state": (sh, st), "fsm-trans": (xh, xt),
                          "func": (ftotal, 100.0), "assert": (aok, an)}.items():
        lim = getattr(a, "min_" + k.replace("-", "_"))
        if lim is not None and pct(hit, tot) < lim:
            print(f"mcov: {k} coverage {pct(hit, tot):.1f}% is below {lim}%")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
