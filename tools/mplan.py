#!/usr/bin/env python3
"""mplan: verification planning and tracking on top of mregress and mcov.

    mplan check  uart.vplan
    mplan report uart.vplan -results regress.d [-cov cov.json] [-o plan.d]
    mplan triage -results regress.d [-plan uart.vplan] [-rules triage.rules]
    mplan trend  plan.d/history.jsonl
    mplan init   -results regress.d [-cov cov.json] -o draft.vplan
    mplan init   -reqs reqs.csv -o draft.vplan
    mplan rank   -results regress.d [uart.vplan] [-by rate] [-keep keep.list]

A verification plan (vPlan) lists the features of a design and, per feature,
what proves it: tests (mregress results), code coverage, covergroups and
assertions (the msim -cov database that mcov merges). ``mplan report`` scores
every feature, records the result in a history file so progress shows over
time, sorts the failures into buckets, and writes ``plan.json`` and a single
``plan.html``.

The plan file. Indentation nests features; ``#`` starts a comment:

    plan uart "UART verification plan"
      owner: tony
      goal: 100                         # default goal of every metric below
      feature tx "Transmit path" weight=2 owner=alice
        doc: spec 3.1, shift register and baud tick
        test   uart_tx_*                # mregress test names
        line   rtl/uart_tx.sv goal=95   # code coverage of the files
        branch rtl/uart_tx.sv goal=90
        toggle tb.dut.u_tx goal=80      # signals in that scope
        cond   rtl/uart_tx.sv           # condition coverage of the files
        fsm    tb.dut.u_tx.state        # an FSM (instance.state variable), or a scope
        cover  tb.cg_tx                 # a covergroup type, or type.item
        assert tb.dut.u_tx              # assertions in that scope
        feature parity "Parity"
          req UART-12 UART-13           # the requirements this feature verifies
          test uart_parity_*
      reqs uart_reqs.csv                # the requirement list (CSV or ReqIF)
      include rx.vplan                  # the features of another plan file

A metric line is ``<kind> <pattern> [goal=PCT] [weight=N]``. In a pattern
``*`` matches any text and ``?`` one character. A hierarchical name also
matches everything below it (``tb.dut.u_tx`` covers ``tb.dut.u_tx.busy``);
a file pattern matches the end of the path (``rtl/uart_tx.sv``) or a
directory (``rtl/``).

    test    tests that pass over tests matched. A test passes when every
            seed of it passed; a known gap (xfail) or a test that did not
            run is not a pass. No matching test scores 0.
    line    lines hit over lines, of the matched files
    branch  branch outcomes seen over outcomes, of the matched files
    toggle  bits toggled both ways over bits, of the matched signals
    cond    condition vectors seen over vectors, of the matched files
    fsm     states reached plus transitions taken, over states plus
            transitions, of the matched FSMs (named instance.variable)
    cover   covergroup items matched, each bins covered over bins, weighed
            by the item's option.weight
    assert  assert / assume directives that passed at least once and never
            failed, plus cover directives that were hit, over directives

A feature's score is the weighted mean of its metrics and child features
(a feature with nothing mapped yet is planned work and counts as 0).
Its status is ``failing`` (a mapped test fails or a mapped assertion
failed, here or below), ``met`` (every metric reached its goal, here and
below), ``empty`` (nothing mapped yet), ``not started`` (score 0), or
``partial``.

Requirements. ``req ID ...`` in a feature says which requirements it
verifies; ``reqs FILE`` in the plan (or ``-reqs FILE``) names the list they
come from. A CSV file (``,`` ``;`` or tab) has a header row with an ``id``
column and, optionally, ``title`` (or ``name`` / ``summary``), ``text`` (or
``description``), ``parent`` and ``priority``. A ReqIF file (``.reqif``, or
a ``.reqifz`` archive, as DOORS, Polarion and Jama export) gives one
requirement per SPEC-OBJECT: the id is its ``ReqIF.ForeignID`` (else its
IDENTIFIER), the title ``ReqIF.Name`` / ``ReqIF.ChapterName``, the text
``ReqIF.Text``, the parent from the specification hierarchy. A requirement
is ``verified`` (every feature that names it is met), ``failing`` (one of
them is failing), ``partial``, ``not started``, or ``uncovered`` (no feature
names it); a requirement with children takes theirs into account. A plan
that names an id the list does not hold gets a warning. ``-trace FILE``
writes the traceability matrix as CSV, ``-fail-on-uncovered`` exits 1 on an
uncovered requirement or an unknown id, and ``init -reqs`` drafts a plan
with one feature per requirement.

Ranking. ``mplan rank`` reads the coverage database of every run (mregress
-cov keeps one per run) and says what each test adds. The coverage points
are mcov's items (``mcov rank`` counts the same ones, per database file). The tests are ordered greedily: first the one with the
most points, then the one that adds the most new points, and so on (``-by
rate``: the most new points per second of run time). The table gives each
test's new points, the coverage so far, its run time, and its unique points
(covered by no other test). Tests that add nothing are listed as redundant;
``-keep FILE`` writes the others, one name a line, in rank order. Failing
tests are left out (``-all`` ranks them too). With a plan, each feature
lists the tests that contribute to its coverage metrics. ``report -rank``
puts the same table in ``plan.html`` and ``plan.json``.

History. ``report`` appends one snapshot per run to ``<o>/history.jsonl``
(``-history FILE``; ``-no-history`` to leave it alone): the time, a label
(``-label``, else the commit from GIT_COMMIT / GITHUB_SHA), every feature's
score and status, the test counts, the coverage totals and the failure
buckets. A second run with the same label replaces that snapshot. The time
is ``-time``, else SOURCE_DATE_EPOCH, else now.

Triage. Every failing run goes into a bucket by its failure signature
(mregress's: the first error line with numbers, paths and hex replaced). A
timeout or a bare exit status says little, so those are split further by
the last line of the run's log. Against the previous snapshot a bucket is
``new`` or ``recurring`` (with the snapshot it was first seen in), buckets
that went away are listed as fixed, and a bucket whose tests also passed on
other seeds is marked flaky. Each bucket names the features it blocks and
their owners. A rules file (``-rules``) annotates buckets, one rule a line:

    /timeout/                 status=known owner=infra bug=INFRA-12
    /output differs at line/  status=waived note="reference model issue"

Gates: ``-gate`` exits 1 unless the whole plan is met, ``-min-score PCT``
exits 1 below that score, ``-fail-on-new`` exits 1 when a bucket is new and
no rule marks it known or waived.

Speed: result directories, coverage databases and failure logs are read in
parallel, ``-threads N`` (or ``-j N``) workers, by default every logical CPU
this process may use. The output does not depend on the worker count.

Exit status: 0, 1 when a gate fails, 2 on bad usage or a bad plan.
"""

import argparse
import bisect
import concurrent.futures as cf
import csv
import datetime
import html
import importlib.machinery
import importlib.util
import json
import os
import re
import shlex
import sys
import time
import zipfile

VERSION = "0.1.0"
KINDS = ("test", "line", "branch", "toggle", "cond", "fsm", "cover", "assert")


def signed(x):
    """+1.2 / -0.4, and +0.0 rather than -0.0."""
    return f"{x + 0.0 if abs(x) >= 0.05 else 0.0:+.1f}"


class PlanError(Exception):
    pass


def tool(name):
    """mregress / mcov as a module: next to this file, installed with or without .py."""
    if name in sys.modules:
        return sys.modules[name]
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, name), os.path.join(here, name + ".py")):
        if os.path.isfile(cand):
            spec = importlib.util.spec_from_loader(name, importlib.machinery.SourceFileLoader(name, cand))
            mod = importlib.util.module_from_spec(spec)
            sys.modules[name] = mod          # worker processes unpickle mcov functions by module name
            sys.path.insert(0, here)
            spec.loader.exec_module(mod)
            return mod
    raise SystemExit(f"mplan: {name} not found next to mplan")


def cpu_count():
    try:
        return len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return os.cpu_count() or 1


# ---------------------------------------------------------------- the plan

class Metric:
    __slots__ = ("kind", "pattern", "goal", "weight", "where", "hit", "total", "score", "note", "bad", "matched")

    def __init__(self, kind, pattern, goal, weight, where):
        self.kind, self.pattern, self.goal, self.weight, self.where = kind, pattern, goal, weight, where
        self.hit = self.total = 0
        self.score = 0.0
        self.note = ""
        self.bad = []          # failing tests / failed assertions
        self.matched = 0

    @property
    def met(self):
        return self.score + 1e-9 >= self.goal


class Feature:
    def __init__(self, ident, title, parent=None, where=""):
        self.id, self.title, self.parent, self.where = ident, title or ident, parent, where
        self.weight = 1.0
        self.goal = None       # default goal of the metrics below (inherited)
        self.owner = None
        self.doc = ""
        self.reqs = []         # requirement ids this feature verifies
        self.req_files = []    # the plan's requirement lists (kept on the root)
        self.metrics, self.children = [], []
        self.score, self.status = 0.0, "empty"

    @property
    def path(self):
        return self.id if self.parent is None or self.parent.parent is None else f"{self.parent.path}.{self.id}"

    def default_goal(self):
        f = self
        while f is not None:
            if f.goal is not None:
                return f.goal
            f = f.parent
        return 100.0

    def owner_of(self):
        f = self
        while f is not None:
            if f.owner:
                return f.owner
            f = f.parent
        return ""

    def walk(self, depth=0):
        yield self, depth
        for c in self.children:
            yield from c.walk(depth + 1)


def _number(text, what, where):
    try:
        v = float(text)
    except ValueError:
        raise PlanError(f"{where}: {what} needs a number, got {text!r}")
    if v < 0:
        raise PlanError(f"{where}: {what} cannot be negative")
    return v


def _words(text, where):
    try:
        return shlex.split(text, comments=True)
    except ValueError as e:
        raise PlanError(f"{where}: {e}")


def parse_plan(path, _stack=None):
    """Read a plan file into a Feature tree (the root is the plan itself)."""
    path = os.path.abspath(path)
    stack_files = _stack or []
    if path in stack_files:
        raise PlanError(f"{path}: includes itself")
    try:
        with open(path, encoding="utf-8") as f:
            raw_lines = f.read().splitlines()
    except OSError as e:
        raise PlanError(f"{path}: {e.strerror}")
    root = Feature(os.path.splitext(os.path.basename(path))[0], None, None, path)
    root.goal = None
    stack = [(-1, root)]            # (indent, feature)
    named = False
    for no, raw in enumerate(raw_lines, 1):
        where = f"{path}:{no}"
        if "\t" in raw[: len(raw) - len(raw.lstrip())]:
            raise PlanError(f"{where}: indent with spaces, not tabs")
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip())
        while len(stack) > 1 and indent <= stack[-1][0]:
            stack.pop()
        cur = stack[-1][1]
        # "key: value" attributes of the feature they sit in.
        m = re.match(r"^(owner|doc|goal|weight)\s*:\s*(.*)$", line)
        if m:
            key, val = m.group(1), m.group(2).strip()
            if key in ("owner", "doc"):
                val = re.sub(r"\s+#.*$", "", val) if key == "owner" else val
                if key == "doc" and cur.doc:
                    val = cur.doc + " " + val
                setattr(cur, key, val)
            else:
                setattr(cur, key, _number(re.sub(r"\s+#.*$", "", val), key, where))
            continue
        words = _words(line, where)
        if not words:
            continue
        head, rest = words[0], words[1:]
        opts, args = {}, []
        for w in rest:
            mm = re.match(r"^(goal|weight|owner)=(.*)$", w)
            if mm:
                opts[mm.group(1)] = mm.group(2)
            else:
                args.append(w)
        if head == "plan":
            if named or cur is not root or root.children or root.metrics:
                raise PlanError(f"{where}: 'plan' comes once, first")
            if not args or len(args) > 2:
                raise PlanError(f"{where}: expected: plan NAME [\"title\"]")
            named = True
            root.id, root.title = args[0], args[1] if len(args) > 1 else args[0]
            root.where = where
            stack = [(-1, root)]
            _apply_opts(root, opts, where)
            continue
        if head == "feature":
            if not args or len(args) > 2:
                raise PlanError(f"{where}: expected: feature NAME [\"title\"] [weight=N] [goal=PCT] [owner=WHO]")
            if not re.match(r"^[A-Za-z0-9_\-]+$", args[0]):
                raise PlanError(f"{where}: a feature name is letters, digits, '_' and '-': {args[0]!r}")
            if any(c.id == args[0] for c in cur.children):
                raise PlanError(f"{where}: feature {args[0]!r} is already in {cur.path!r}")
            f = Feature(args[0], args[1] if len(args) > 1 else None, cur, where)
            _apply_opts(f, opts, where)
            cur.children.append(f)
            stack.append((indent, f))
            continue
        if head == "include":
            if len(args) != 1 or opts:
                raise PlanError(f"{where}: expected: include FILE")
            sub = parse_plan(os.path.join(os.path.dirname(path), args[0]), stack_files + [path])
            # A plan with its own name comes in as one feature; a bare list of features is spliced in.
            incoming = [sub] if sub.metrics or getattr(sub, "named", False) else sub.children
            root.req_files += [p for p in sub.req_files if p not in root.req_files]
            for f in incoming:
                if any(c.id == f.id for c in cur.children):
                    raise PlanError(f"{where}: feature {f.id!r} is already in {cur.path!r}")
                f.parent = cur
                cur.children.append(f)
            continue
        if head in ("req", "req:"):
            ids = [i for a_ in args for i in a_.split(",") if i]
            if not ids or opts:
                raise PlanError(f"{where}: expected: req ID [ID ...]")
            cur.reqs += [i for i in ids if i not in cur.reqs]
            continue
        if head == "reqs":
            if len(args) != 1 or opts:
                raise PlanError(f"{where}: expected: reqs FILE")
            rp = os.path.join(os.path.dirname(path), args[0])
            if rp not in root.req_files:
                root.req_files.append(rp)
            continue
        if head in KINDS:
            if len(args) != 1:
                raise PlanError(f"{where}: expected: {head} PATTERN [goal=PCT] [weight=N]")
            if "owner" in opts:
                raise PlanError(f"{where}: owner= belongs on a feature")
            goal = _number(opts["goal"], "goal", where) if "goal" in opts else None
            if goal is not None and goal > 100:
                raise PlanError(f"{where}: a goal is a percentage, at most 100")
            cur.metrics.append(Metric(head, args[0], goal, _number(opts.get("weight", "1"), "weight", where), where))
            continue
        raise PlanError(f"{where}: unknown line {head!r} (expected plan, feature, include, req, reqs, "
                        f"{', '.join(KINDS)}, or owner: / doc: / goal: / weight:)")
    root.named = named
    if _stack is None:      # the whole tree is in: metrics without a goal take the nearest one above
        for f, _ in root.walk():
            for m in f.metrics:
                if m.goal is None:
                    m.goal = f.default_goal()
    return root


def _apply_opts(f, opts, where):
    if "weight" in opts:
        f.weight = _number(opts["weight"], "weight", where)
    if "goal" in opts:
        f.goal = _number(opts["goal"], "goal", where)
        if f.goal > 100:
            raise PlanError(f"{where}: a goal is a percentage, at most 100")
    if "owner" in opts:
        f.owner = opts["owner"]


# ---------------------------------------------------------------- matching

def glob_rx(pattern):
    return "".join(".*" if c == "*" else "." if c == "?" else re.escape(c) for c in pattern)


class Matcher:
    """Match plan patterns against one kind of name. The names are sorted
    once; a pattern is answered from the range of names that start with its
    literal head (the text before its first wildcard), so a plan with
    thousands of patterns does not scan every name for each. Results are
    cached per pattern."""

    def __init__(self, names, mode):
        self.mode = mode              # "name" (exact), "hier" (name or scope above it), "file"
        self.cache = {}
        if mode == "file":
            # A relative pattern matches the end of a path, at a "/": every
            # such tail of every path is a key.
            tails = {}
            for n in names:
                norm = n.replace("\\", "/")
                tails.setdefault(norm, []).append(n)
                p = norm.find("/")
                while p >= 0:
                    if p + 1 < len(norm):
                        tails.setdefault(norm[p + 1:], []).append(n)
                    p = norm.find("/", p + 1)
            self.tails = tails
            self.keys = sorted(tails)
        else:
            self.keys = sorted(names)
            self.known = set(self.keys)

    def _range(self, head):
        """The keys that start with head, as a slice of the sorted keys."""
        keys = self.keys
        lo = bisect.bisect_left(keys, head)
        if not head:
            return lo, len(keys)
        hi = bisect.bisect_left(keys, head[:-1] + chr(ord(head[-1]) + 1), lo)
        return lo, hi

    def match(self, pattern):
        got = self.cache.get(pattern)
        if got is not None:
            return got
        keys = self.keys
        pat = pattern.replace("\\", "/") if self.mode == "file" else pattern
        if self.mode == "file" and pat.endswith("/"):
            pat += "*"
        cut = min((i for i in (pat.find("*"), pat.find("?")) if i >= 0), default=-1)
        if self.mode == "file":
            if cut < 0:
                got = self.tails.get(pat, [])
            else:
                lo, hi = self._range(pat[:cut])
                rx = re.compile(glob_rx(pat) + r"\Z")
                found = set()
                for k in keys[lo:hi]:
                    if rx.match(k):
                        found.update(self.tails[k])
                got = sorted(found)
        elif cut < 0:
            got = [pattern] if pattern in self.known else []
            if self.mode == "hier":       # and everything in that scope
                lo, hi = self._range(pattern + ".")
                got = got + keys[lo:hi]
        else:
            lo, hi = self._range(pat[:cut])
            rx = re.compile(glob_rx(pat) + (r"(?:\..*)?\Z" if self.mode == "hier" else r"\Z"))
            got = [k for k in keys[lo:hi] if rx.match(k)]
        self.cache[pattern] = got
        return got


# ---------------------------------------------------------------- inputs

def _load_results(d):
    path = os.path.join(d, "results.json") if os.path.isdir(d) else d
    try:
        with open(path, "rb") as f:
            data = json.loads(f.read())
    except OSError as e:
        raise SystemExit(f"mplan: {path}: {e.strerror}")
    except ValueError as e:
        raise SystemExit(f"mplan: {path}: not JSON ({e})")
    if "runs" not in data:
        raise SystemExit(f"mplan: {path} is not an mregress results.json")
    base = os.path.dirname(os.path.abspath(path))
    for r in data["runs"]:
        r["_dir"] = base
        r["_sim"] = data.get("simulator", "")
    return data["runs"]


def load_runs(dirs, jobs):
    """Every run of the given mregress output directories, in the order given."""
    if not dirs:
        return []
    if len(dirs) == 1 or jobs <= 1:
        parts = [_load_results(d) for d in dirs]
    else:
        with cf.ThreadPoolExecutor(max_workers=min(jobs, len(dirs))) as pool:
            parts = list(pool.map(_load_results, dirs))
    return [r for part in parts for r in part]


def test_table(runs):
    """test -> {"status", "runs", "pass", "fail", "seconds"}: one verdict per test over its seeds."""
    tests = {}
    for r in runs:
        t = tests.setdefault(r["test"], {"pass": 0, "fail": 0, "xfail": 0, "skip": 0, "seconds": 0.0})
        s = r["status"]
        t["fail" if s in ("fail", "xpass") else s if s in ("pass", "xfail", "skip") else "fail"] += 1
        t["seconds"] += r.get("seconds", 0.0)
    for t in tests.values():
        t["status"] = ("fail" if t["fail"] else "xfail" if t["xfail"] else "pass" if t["pass"] else "skip")
    return tests


class Data:
    """What a plan is scored against: the tests and the coverage database."""

    def __init__(self, runs, db):
        self.runs = runs
        self.tests = test_table(runs)
        self.db = db
        mcov = tool("mcov")
        self.files = {}
        self.toggles = {}
        self.items = {}       # "type.item" -> item
        self.types = {}
        self.asserts = {}
        self.conds = {}       # file -> [vectors hit, vectors]
        self.fsms = {}        # "inst.name" -> (states hit, states, transitions hit, transitions)
        if db is not None:
            per, _, _ = mcov.summarize(db, None)
            self.files = per
            pop = mcov.popcount
            self.toggles = {n: (pop(r & f), w) for n, (w, r, f) in db["toggles"].items()}
            for tname, (_w, _g, items) in db["cgs"].items():
                self.types[tname] = items
                for iname, it in items.items():
                    self.items[f"{tname}.{iname}"] = it
            self.asserts = db["asserts"]
            for (f, _ln, _col, _text), (_op, _terms, hits) in db.get("conds", {}).items():
                c = self.conds.setdefault(f, [0, 0])
                c[0] += sum(1 for h in hits if h > 0)
                c[1] += len(hits)
            for (_mod, inst, name), m in db.get("fsms", {}).items():
                self.fsms[f"{inst}.{name}"] = mcov.fsm_score(m, False)
        self.m = {"test": Matcher(self.tests, "name"), "file": Matcher(self.files, "file"),
                  "toggle": Matcher(self.toggles, "hier"), "cover": Matcher(self.items, "hier"),
                  "assert": Matcher(self.asserts, "hier"), "cond": Matcher(self.conds, "file"),
                  "fsm": Matcher(self.fsms, "hier")}
        self.used = {k: set() for k in ("test", "file", "toggle", "cover", "assert", "fsm")}


def score_metric(m, data):
    mcov = tool("mcov")
    kind = m.kind
    if kind == "test":
        names = data.m["test"].match(m.pattern)
        data.used["test"].update(names)
        m.matched = len(names)
        if not names:
            m.hit, m.total, m.note = 0, 1, "no test matches"
        else:
            m.total = len(names)
            m.hit = sum(1 for n in names if data.tests[n]["status"] == "pass")
            m.bad = [n for n in names if data.tests[n]["status"] == "fail"]
            gaps = sum(1 for n in names if data.tests[n]["status"] == "xfail")
            idle = sum(1 for n in names if data.tests[n]["status"] == "skip")
            m.note = ", ".join(x for x in (f"{len(m.bad)} failing" if m.bad else "", f"{gaps} known gap(s)" if gaps else "",
                                           f"{idle} not run" if idle else "") if x)
        m.score = 100.0 * m.hit / m.total
        return
    if data.db is None:
        m.hit, m.total, m.score, m.note = 0, 0, 0.0, "no coverage database"
        return
    if kind in ("line", "branch"):
        names = data.m["file"].match(m.pattern)
        data.used["file"].update(names)
        k = 0 if kind == "line" else 2
        m.hit = sum(data.files[n][k] for n in names)
        m.total = sum(data.files[n][k + 1] for n in names)
        what = "file"
    elif kind == "toggle":
        names = data.m["toggle"].match(m.pattern)
        data.used["toggle"].update(names)
        m.hit = sum(data.toggles[n][0] for n in names)
        m.total = sum(data.toggles[n][1] for n in names)
        what = "signal"
    elif kind == "cond":
        names = data.m["cond"].match(m.pattern)
        m.hit = sum(data.conds[n][0] for n in names)
        m.total = sum(data.conds[n][1] for n in names)
        what = "file with conditions"
    elif kind == "fsm":
        names = data.m["fsm"].match(m.pattern)
        data.used["fsm"].update(names)
        m.hit = sum(data.fsms[n][0] + data.fsms[n][2] for n in names)
        m.total = sum(data.fsms[n][1] + data.fsms[n][3] for n in names)
        what = "FSM"
    elif kind == "cover":
        names = data.m["cover"].match(m.pattern)
        data.used["cover"].update(names)
        num = den = 0.0
        for n in names:
            it = data.items[n]
            c, t = mcov.item_count(it)
            m.hit += c
            m.total += t
            if t:
                num += it[1] * c / t
                den += it[1]
        m.matched = len(names)
        # By item weight, as a covergroup's own coverage is (19.11); weights of 0 fall back to the bins.
        m.score = 100.0 * num / den if den else (100.0 * m.hit / m.total if m.total else 0.0)
        if not names:
            m.note = "no covergroup matches"
        return
    else:   # assert
        names = data.m["assert"].match(m.pattern)
        data.used["assert"].update(names)
        m.total = len(names)
        for n in names:
            akind, _evals, passes, fails = data.asserts[n]
            if fails and akind != "cover":
                m.bad.append(n)
            elif passes:
                m.hit += 1
        idle = m.total - m.hit - len(m.bad)
        m.note = ", ".join(x for x in (f"{len(m.bad)} failed" if m.bad else "", f"{idle} never passed or hit" if idle else "") if x)
        what = "assertion"
    m.matched = len(names)
    if not names:
        m.note = f"no {what} matches"
    m.score = 100.0 * m.hit / m.total if m.total else 0.0


def score_feature(f, data):
    for m in f.metrics:
        score_metric(m, data)
    for c in f.children:
        score_feature(c, data)
    # A feature with nothing mapped yet is planned work: it counts, at 0.
    num = sum(m.weight * m.score for m in f.metrics) + sum(c.weight * c.score for c in f.children)
    den = sum(m.weight for m in f.metrics) + sum(c.weight for c in f.children)
    f.score = num / den if den else 0.0
    kids = [c.status for c in f.children]
    if any(m.bad for m in f.metrics) or "failing" in kids:
        f.status = "failing"
    elif not f.metrics and all(k == "empty" for k in kids):
        f.status = "empty"
    elif all(m.met for m in f.metrics) and all(k == "met" for k in kids):
        f.status = "met"
    elif f.score == 0:
        f.status = "not started"
    else:
        f.status = "partial"


def unmapped(data):
    """What the results hold that no plan line points at."""
    out = {"tests": sorted(set(data.tests) - data.used["test"]),
           "files": sorted(set(data.files) - data.used["file"]),
           "assertions": sorted(set(data.asserts) - data.used["assert"])}
    used_types = {n.rsplit(".", 1)[0] for n in data.used["cover"]}
    out["covergroups"] = sorted(set(data.types) - used_types)
    out["FSMs"] = sorted(set(data.fsms) - data.used["fsm"])
    return out


# ---------------------------------------------------------------- requirements

class Req:
    __slots__ = ("id", "title", "text", "parent", "priority", "source", "children", "features", "score", "status")

    def __init__(self, ident, title="", text="", parent="", priority="", source=""):
        self.id, self.title, self.text, self.parent, self.priority, self.source = ident, title, text, parent, priority, source
        self.children, self.features = [], []
        self.score, self.status = 0.0, "uncovered"


CSV_COLS = {"id": ("id", "req", "req id", "reqid", "req_id", "requirement", "requirement id", "identifier", "key"),
            "title": ("title", "name", "summary", "heading"),
            "text": ("text", "description", "desc", "requirement text", "body"),
            "parent": ("parent", "parent id", "parent_id", "parentid"),
            "priority": ("priority", "prio", "severity")}


def _read_reqs_csv(path):
    with open(path, encoding="utf-8-sig", newline="") as f:
        text = f.read()
    first = text.split("\n", 1)[0]
    delim = max(",;\t", key=first.count)
    rows = list(csv.reader(text.splitlines(), delimiter=delim))
    if not rows:
        raise PlanError(f"{path}: empty requirement list")
    head = [h.strip().lower() for h in rows[0]]
    col = {}
    for key, names in CSV_COLS.items():
        for n in names:
            if n in head:
                col[key] = head.index(n)
                break
    if "id" not in col:
        raise PlanError(f"{path}: no 'id' column in the header ({', '.join(rows[0])})")
    out = []
    for no, row in enumerate(rows[1:], 2):
        def cell(k):
            i = col.get(k)
            return row[i].strip() if i is not None and i < len(row) else ""
        if not cell("id"):
            if any(c.strip() for c in row):
                raise PlanError(f"{path}:{no}: a requirement without an id")
            continue
        out.append(Req(cell("id"), cell("title"), cell("text"), cell("parent"), cell("priority"), f"{path}:{no}"))
    return out


def _local(tag):
    return tag.rsplit("}", 1)[-1]


def _xml_text(el):
    return re.sub(r"\s+", " ", "".join(el.itertext())).strip()


def _read_reqs_reqif(path):
    import xml.etree.ElementTree as ET
    try:
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as z:
                names = sorted(n for n in z.namelist() if n.lower().endswith((".reqif", ".xml")))
                if not names:
                    raise PlanError(f"{path}: no .reqif file in the archive")
                roots = [ET.fromstring(z.read(n)) for n in names]
        else:
            roots = [ET.parse(path).getroot()]
    except ET.ParseError as e:
        raise PlanError(f"{path}: not ReqIF: {e}")
    out = []
    for root in roots:
        # Attribute definitions: IDENTIFIER -> LONG-NAME ("ReqIF.Text", "ReqIF.ForeignID", ...).
        defs = {el.get("IDENTIFIER"): el.get("LONG-NAME") or "" for el in root.iter() if _local(el.tag).startswith("ATTRIBUTE-DEFINITION-")}
        objs = {}
        for so in root.iter():
            if _local(so.tag) != "SPEC-OBJECT":
                continue
            attrs = {}
            for av in so.iter():
                if not _local(av.tag).startswith("ATTRIBUTE-VALUE-"):
                    continue
                ref = next((_xml_text(r) for d in av if _local(d.tag) == "DEFINITION" for r in d), "")
                val = av.get("THE-VALUE")
                if val is None:
                    val = next((_xml_text(v) for v in av if _local(v.tag) == "THE-VALUE"), "")
                attrs[defs.get(ref, ref).lower()] = val.strip()
            pick = lambda *ks: next((attrs[k] for k in ks if attrs.get(k)), "")
            ident = pick("reqif.foreignid", "id", "identifier", "req id") or so.get("IDENTIFIER") or ""
            if not ident:
                continue
            r = Req(ident, pick("reqif.name", "reqif.chaptername", "title", "name") or so.get("LONG-NAME") or "",
                    pick("reqif.text", "text", "description"), "", pick("priority", "reqif.priority"), path)
            objs[so.get("IDENTIFIER")] = r
        seen = set()

        def hier(el, parent):
            for h in el:
                tag = _local(h.tag)
                if tag == "SPEC-HIERARCHY":
                    ref = next((_xml_text(r) for o in h if _local(o.tag) == "OBJECT" for r in o), "")
                    r = objs.get(ref)
                    if r is not None and ref not in seen:
                        seen.add(ref)
                        r.parent = parent
                        out.append(r)
                    hier(h, r.id if r is not None else parent)
                else:
                    hier(h, parent)
        for spec in root.iter():
            if _local(spec.tag) == "SPECIFICATION":
                hier(spec, "")
        out += [r for k, r in objs.items() if k not in seen]      # objects in no specification
    return out


def load_reqs(paths):
    """Requirement lists (CSV or ReqIF) as {id: Req}, in file order."""
    reqs = {}
    for path in paths:
        try:
            with open(path, "rb") as f:
                head = f.read(256).lstrip(b"\xef\xbb\xbf \t\r\n")
        except OSError as e:
            raise PlanError(f"{path}: {e.strerror}")
        got = _read_reqs_reqif(path) if head.startswith((b"<", b"PK")) else _read_reqs_csv(path)
        for r in got:
            if r.id in reqs:
                raise PlanError(f"{r.source}: requirement {r.id!r} is already in {reqs[r.id].source}")
            reqs[r.id] = r
    for r in reqs.values():
        if r.parent and r.parent in reqs and r.parent != r.id:
            reqs[r.parent].children.append(r)
        else:
            r.parent = ""
    return reqs


def trace_reqs(plan, reqs):
    """Link the plan's features to the requirements and give each a score and a status.

    Returns (requirements in order, ids the plan names that no list holds)."""
    known = bool(reqs)
    reqs = dict(reqs)
    unknown = []
    for f, _ in plan.walk():
        for i in f.reqs:
            if i not in reqs:
                if known:
                    unknown.append((i, f.path))
                    continue
                reqs[i] = Req(i, source=f.where)
            reqs[i].features.append(f)
    done = set()

    def settle(r):
        if r.id in done:
            return
        done.add(r.id)
        for c in r.children:
            settle(c)
        parts = [(f.score, {"met": "verified", "empty": "not started"}.get(f.status, f.status)) for f in r.features]
        parts += [(c.score, c.status) for c in r.children]
        sts = [s for _, s in parts]
        r.score = sum(p for p, _ in parts) / len(parts) if parts else 0.0
        if not parts or all(s == "uncovered" for s in sts):
            r.status = "uncovered"
        elif "failing" in sts:
            r.status = "failing"
        elif all(s == "verified" for s in sts):
            r.status = "verified"
        elif r.score == 0:
            r.status = "not started"
        else:
            r.status = "partial"
    order = []

    def visit(r, depth):
        settle(r)
        order.append((r, depth))
        for c in r.children:
            visit(c, depth + 1)
    for r in reqs.values():
        if not r.parent:
            visit(r, 0)
    return order, unknown


REQ_STATES = ("verified", "partial", "not started", "failing", "uncovered")


def req_counts(order):
    """Counts over the requirements that have no children (the ones a feature has to cover)."""
    c = {k: 0 for k in REQ_STATES}
    for r, _ in order:
        if not r.children:
            c[r.status] += 1
    c["total"] = sum(c.values())
    return c


def reqs_json(order, unknown):
    return {"counts": req_counts(order), "unknown": [{"id": i, "feature": p} for i, p in unknown],
            "items": [{"id": r.id, "title": r.title, "text": r.text, "parent": r.parent, "priority": r.priority, "depth": d,
                       "score": round(r.score, 2), "status": r.status, "features": [f.path for f in r.features]} for r, d in order]}


def print_reqs(order, unknown, out=None):
    out = out or sys.stdout
    if not order:
        return
    c = req_counts(order)
    print(f"\nrequirements: {c['verified']}/{c['total']} verified"
          + "".join(f", {c[k]} {k}" for k in REQ_STATES[1:] if c[k]), file=out)
    for r, _ in order:
        if r.status != "verified" and not r.children:
            feats = ", ".join(f.path for f in r.features)
            print(f"  {r.status:<11}  {r.id}" + (f"  {r.title}" if r.title else "") + (f"  ({feats} {r.score:.1f}%)" if feats else ""), file=out)
    for i, p in unknown:
        print(f"  warning: feature {p} names requirement {i!r}, which is in no requirement list", file=out)


def write_trace(order, path):
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["id", "title", "parent", "priority", "status", "score", "features", "owners", "tests"])
        for r, _ in order:
            owners = sorted({f.owner_of() for f in r.features if f.owner_of()})
            tests = sorted({m.pattern for f in r.features for g, _ in f.walk() for m in g.metrics if m.kind == "test"})
            w.writerow([r.id, r.title, r.parent, r.priority, r.status, f"{r.score:.1f}", " ".join(f.path for f in r.features),
                        " ".join(owners), " ".join(tests)])


# ---------------------------------------------------------------- per-test coverage and ranking

# The coverage items and their keys are mcov's (mcov.test_items, the same ones
# mcov rank counts); mplan adds the test names and seeds from the regression,
# the order by run time, and the features each test serves.

def _owner(k):
    """The plan-facing owner of an mcov item key: (kind, name) as the matchers know them."""
    c = k[0]
    if c in ("l", "b"):
        return ("file", k[1])
    if c == "t":
        return ("toggle", k[1])
    if c == "c":
        return ("cond", k[1])
    if c in ("s", "x"):
        return ("fsm", f"{k[2]}.{k[3]}")
    if c == "g":
        return ("cover", f"{k[1]}.{k[2]}")
    return ("assert", k[1])


class Points:
    """Every test's coverage as one bit mask over mcov's items (all seeds of a test together)."""

    def __init__(self, runs, jobs, include_failing=False):
        mcov = tool("mcov")
        tests = test_table(runs)
        self.skipped = sorted(n for n, t in tests.items() if t["status"] != "pass") if not include_failing else []
        want = [r for r in runs if (include_failing or tests[r["test"]]["status"] == "pass") and r.get("log")]
        paths = [os.path.join(r["_dir"], os.path.dirname(r["log"]), "cov.json") for r in want]
        todo = [p for p in paths if os.path.exists(p)]
        loaded = None
        if jobs > 1 and len(todo) > 1:
            try:
                with cf.ProcessPoolExecutor(max_workers=min(jobs, len(todo))) as pool:
                    loaded = list(pool.map(mcov._rank_load, todo, chunksize=max(1, len(todo) // (4 * jobs))))
            except SystemExit:
                raise
            except Exception:           # no fork / not picklable here: read them one by one
                loaded = None
        if loaded is None:
            loaded = [mcov._rank_load(p) for p in todo]
        # Number the items in run order, so the result does not depend on the worker count.
        index = {}
        for _, all_, _ in loaded:
            for k in all_:
                if k not in index:
                    index[k] = len(index)
        self.index, self.total = index, len(index)
        hit_of = {p: hit for p, _, hit in loaded}
        nb = (len(index) + 7) // 8
        self.mask, self.seconds = {}, {}
        for r, p in zip(want, paths):
            name = r["test"]
            self.seconds[name] = self.seconds.get(name, 0.0) + r.get("seconds", 0.0)
            hit = hit_of.get(p)
            if hit is None:
                self.mask.setdefault(name, None)
                continue
            buf = bytearray(nb)
            for k in hit:
                i = index[k]
                buf[i >> 3] |= 1 << (i & 7)
            m = int.from_bytes(buf, "little")
            self.mask[name] = m if self.mask.get(name) is None else self.mask[name] | m
        self.no_data = sorted(n for n, m in self.mask.items() if m is None)
        for n in self.no_data:
            self.mask[n] = 0
        self._owners = None

    def owner_masks(self):
        """(kind, name) -> the bits of that file / signal / FSM / covergroup item / assertion."""
        if self._owners is None:
            bits = {}
            for k, i in self.index.items():
                bits.setdefault(_owner(k), []).append(i)
            nb = (self.total + 7) // 8
            self._owners = {}
            for o, ii in bits.items():
                buf = bytearray(nb)
                for i in ii:
                    buf[i >> 3] |= 1 << (i & 7)
                self._owners[o] = int.from_bytes(buf, "little")
        return self._owners


def rank_tests(pts, by="points"):
    """Greedy order of the tests by what each adds. Returns the ranking as a dict."""
    pop = tool("mcov").popcount
    names = sorted(n for n in pts.mask if n not in pts.no_data)
    once = multi = 0
    for n in names:
        multi |= once & pts.mask[n]
        once |= pts.mask[n]
    only = once & ~multi
    covered, order, left = 0, [], set(names)

    def worth(n, gain):
        return gain / max(pts.seconds.get(n, 0.0), 1e-3) if by == "rate" else gain
    # Lazy greedy: a test's gain only shrinks as others are picked, so a stale best that is still best is the best.
    cand = sorted(((worth(n, pop(pts.mask[n])), n) for n in names), key=lambda x: (-x[0], x[1]))
    while cand:
        best = None
        for i, (w, n) in enumerate(cand):
            if best is not None and w <= best[0]:
                break
            gain = pop(pts.mask[n] & ~covered)
            w = worth(n, gain)
            cand[i] = (w, n)
            if gain and (best is None or w > best[0] or (w == best[0] and n < best[1])):
                best = (w, n, gain)
        if best is None:
            break
        _, n, gain = best
        covered |= pts.mask[n]
        left.discard(n)
        order.append({"test": n, "new": gain, "points": pop(pts.mask[n]), "unique": pop(pts.mask[n] & only),
                      "seconds": round(pts.seconds.get(n, 0.0), 3), "covered": pop(covered),
                      "cumulative": round(100.0 * pop(covered) / pts.total, 2) if pts.total else 0.0})
        cand = sorted(((w, m) for w, m in cand if m != n), key=lambda x: (-x[0], x[1]))
    return {"by": by, "points": pts.total, "covered": pop(covered),
            "coverage": round(100.0 * pop(covered) / pts.total, 2) if pts.total else 0.0,
            "seconds": round(sum(pts.seconds.get(n, 0.0) for n in names), 3),
            "seconds_ranked": round(sum(o["seconds"] for o in order), 3),
            "order": order, "redundant": sorted(left), "no_data": pts.no_data, "not_ranked": pts.skipped}


METRIC_OWNER = {"line": ("file", "file"), "branch": ("file", "file"), "cond": ("cond", "cond"), "toggle": ("toggle", "toggle"),
                "fsm": ("fsm", "fsm"), "cover": ("cover", "cover"), "assert": ("assert", "assert")}


def attribute(plan, data, pts, top=10):
    """Per feature, the tests that cover items of its own coverage metrics: {path: {"tests": N, "top": [[test, items], ...]}}."""
    pop = tool("mcov").popcount
    owners = pts.owner_masks()
    out = {}
    for f, _ in plan.walk():
        fm = 0
        for m in f.metrics:
            if m.kind in METRIC_OWNER:
                matcher, kind = METRIC_OWNER[m.kind]
                for n in data.m[matcher].match(m.pattern):
                    fm |= owners.get((kind, n), 0)
        if not fm:
            continue
        rows = sorted(([n, pop(mk & fm)] for n, mk in pts.mask.items() if mk & fm), key=lambda r: (-r[1], r[0]))
        out[f.path] = {"tests": len(rows), "top": rows[:top]}
    return out


def print_rank(rk, attr=None, limit=0, out=None):
    out = out or sys.stdout
    rows = [("Rank", "Test", "New", "Coverage", "Seconds", "Unique")]
    for i, o in enumerate(rk["order"][: limit or None], 1):
        rows.append((str(i), o["test"], str(o["new"]), f"{o['cumulative']:.1f}%", f"{o['seconds']:.1f}", str(o["unique"])))
    ws = [max(len(r[i]) for r in rows) for i in range(6)]
    for r in rows:
        print(f"{r[0]:>{ws[0]}}  {r[1]:<{ws[1]}}  {r[2]:>{ws[2]}}  {r[3]:>{ws[3]}}  {r[4]:>{ws[4]}}  {r[5]:>{ws[5]}}".rstrip(), file=out)
    if limit and len(rk["order"]) > limit:
        print(f"  ... {len(rk['order']) - limit} more", file=out)
    print(f"\nmplan: {len(rk['order'])} of {len(rk['order']) + len(rk['redundant'])} tests reach {rk['coverage']:.1f}% "
          f"({rk['covered']}/{rk['points']} points) in {rk['seconds_ranked']:.1f}s of {rk['seconds']:.1f}s", file=out)
    for label, key in (("redundant (add nothing)", "redundant"), ("no coverage data", "no_data"), ("not ranked (not passing)", "not_ranked")):
        if rk[key]:
            print(f"{label}: {', '.join(rk[key][:20])}{' ...' if len(rk[key]) > 20 else ''}", file=out)
    if attr:
        print("\ntests behind each feature's coverage:", file=out)
        for path, a in attr.items():
            print(f"  {path}: " + ", ".join(f"{n} ({c})" for n, c in a["top"][:5]) + (f" and {a['tests'] - 5} more" if a["tests"] > 5 else ""), file=out)


# ---------------------------------------------------------------- triage

GENERIC = re.compile(r"^(timeout$|exit status )")
LOG_NOISE = re.compile(r"^\s*$|^---- stderr ----$|\$finish called|^VCD info:")


def _log_tail(args):
    base, rel = args
    try:
        with open(os.path.join(base, rel), "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 4096))
            text = f.read().decode("utf-8", errors="replace")
    except OSError:
        return ""
    for line in reversed(text.splitlines()):
        if not LOG_NOISE.search(line):
            return line.strip()
    return ""


def load_rules(path):
    rules = []
    if not path:
        return rules
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise SystemExit(f"mplan: {path}: {e.strerror}")
    for no, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^/(.*)/(?:\s+(.*))?$", line)
        if not m:
            raise SystemExit(f"mplan: {path}:{no}: expected /regex/ key=value ...")
        try:
            rx = re.compile(m.group(1))
        except re.error as e:
            raise SystemExit(f"mplan: {path}:{no}: bad regex: {e}")
        ann = {}
        for w in shlex.split(m.group(2) or ""):
            k, eq, v = w.partition("=")
            if not eq or k not in ("status", "owner", "bug", "note"):
                raise SystemExit(f"mplan: {path}:{no}: expected status= / owner= / bug= / note=, got {w!r}")
            ann[k] = v
        if ann.get("status", "known") not in ("known", "waived", "open"):
            raise SystemExit(f"mplan: {path}:{no}: status is known, waived or open")
        rules.append((rx, ann))
    return rules


def triage(runs, tests, plan, history, rules, jobs, matcher=None):
    """Failure buckets of this regression, set against the snapshots before it."""
    signature = tool("mregress").signature
    failing = [r for r in runs if r["status"] in ("fail", "xpass")]
    # A timeout or a bare exit status: the last log line tells them apart.
    need = [(r["_dir"], r["log"]) for r in failing if GENERIC.match(r.get("reason", "")) and r.get("log")]
    tails = {}
    if need:
        uniq = sorted(set(need))
        if jobs > 1 and len(uniq) > 1:
            with cf.ThreadPoolExecutor(max_workers=min(jobs, len(uniq))) as pool:
                tails = dict(zip(uniq, pool.map(_log_tail, uniq)))
        else:
            tails = {k: _log_tail(k) for k in uniq}
    buckets = {}
    for r in failing:
        sig = r.get("signature") or signature(r.get("reason", ""))
        if GENERIC.match(r.get("reason", "")):
            tail = tails.get((r["_dir"], r.get("log", "")), "")
            if tail and signature(tail) != sig:
                sig = f"{sig} | {signature(tail)}"
        b = buckets.setdefault(sig, {"signature": sig, "runs": 0, "tests": set(), "seeds": [], "example": None})
        b["runs"] += 1
        b["tests"].add(r["test"])
        b["seeds"].append((r["test"], r["seed"]))
        if b["example"] is None or (r["test"], r["seed"]) < (b["example"]["test"], b["example"]["seed"]):
            b["example"] = r
    # Which features each failing test blocks.
    blocks = {}
    if plan is not None:
        matcher = matcher or Matcher(tests, "name")
        for f, _ in plan.walk():
            for m in f.metrics:
                if m.kind == "test":
                    for n in matcher.match(m.pattern):
                        blocks.setdefault(n, []).append(f)
    prev = history[-1] if history else None
    prev_buckets = prev.get("buckets", {}) if prev else {}
    out = []
    for sig, b in buckets.items():
        names = sorted(b["tests"])
        feats, owners = [], []
        for n in names:
            for f in blocks.get(n, []):
                if f.path not in feats:
                    feats.append(f.path)
                o = f.owner_of()
                if o and o not in owners:
                    owners.append(o)
        ann = {}
        for rx, a in rules:
            if rx.search(sig):
                ann = a
                break
        first, age = None, 0
        for snap in reversed(history):
            if sig in snap.get("buckets", {}):
                first, age = snap.get("label") or snap.get("time"), age + 1
            else:
                break
        ex = b["example"]
        out.append({
            "signature": sig, "runs": b["runs"], "tests": names,
            "seeds": sorted(s for _, s in b["seeds"])[:16],
            "class": "new" if (prev is None or sig not in prev_buckets) else "recurring",
            "first_seen": first, "age": age,
            "flaky": all(tests[n]["pass"] > 0 for n in names),
            "features": sorted(feats), "owners": ([ann["owner"]] if ann.get("owner") else sorted(owners)),
            "status": ann.get("status", "known") if ann else "open",
            "bug": ann.get("bug", ""), "note": ann.get("note", ""),
            "reason": ex.get("reason", ""), "log": os.path.join(ex["_dir"], ex["log"]) if ex.get("log") else "",
            "cmd": ex.get("cmd", ""),
        })
    out.sort(key=lambda b: (b["class"] != "new", -b["runs"], b["signature"]))
    now_failing = {n for n, t in tests.items() if t["status"] == "fail"}
    prev_failing = set(prev.get("failing", [])) if prev else set()
    return {
        "buckets": out,
        "fixed": sorted(s for s in prev_buckets if s not in buckets),
        "newly_failing": sorted(now_failing - prev_failing) if prev else [],
        "fixed_tests": sorted(n for n in prev_failing - now_failing if n in tests and tests[n]["status"] == "pass"),
        "compared_to": (prev.get("label") or prev.get("time")) if prev else None,
    }


# ---------------------------------------------------------------- history

def read_history(path):
    snaps = []
    if not path or not os.path.exists(path):
        return snaps
    with open(path, encoding="utf-8") as f:
        for no, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                snaps.append(json.loads(line))
            except ValueError:
                raise SystemExit(f"mplan: {path}:{no}: not a snapshot (bad JSON)")
    return snaps


def write_history(path, snaps):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        for s in snaps:
            f.write(json.dumps(s, sort_keys=True, separators=(",", ":")) + "\n")
    os.replace(tmp, path)


def snapshot_time(arg):
    if arg:
        try:
            t = datetime.datetime.fromisoformat(arg.replace("Z", "+00:00"))
        except ValueError:
            raise SystemExit(f"mplan: -time needs an ISO date and time, got {arg!r}")
        if t.tzinfo is None:
            t = t.replace(tzinfo=datetime.timezone.utc)
    elif os.environ.get("SOURCE_DATE_EPOCH"):
        t = datetime.datetime.fromtimestamp(int(os.environ["SOURCE_DATE_EPOCH"]), datetime.timezone.utc)
    else:
        t = datetime.datetime.fromtimestamp(int(time.time()), datetime.timezone.utc)
    return t.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def default_label():
    for k in ("GIT_COMMIT", "GITHUB_SHA"):
        if os.environ.get(k):
            return os.environ[k][:10]
    return ""


def coverage_totals(data):
    if data.db is None:
        return {}
    mcov = tool("mcov")
    lh = sum(v[0] for v in data.files.values()); lt = sum(v[1] for v in data.files.values())
    bh = sum(v[2] for v in data.files.values()); bt = sum(v[3] for v in data.files.values())
    th = sum(v[0] for v in data.toggles.values()); ta = sum(v[1] for v in data.toggles.values())
    cgs, func, (aok, an, _f, _i) = mcov.summarize_func(data.db)
    out = {"line": round(mcov.pct(lh, lt), 2), "branch": round(mcov.pct(bh, bt), 2), "toggle": round(mcov.pct(th, ta), 2)}
    if data.conds:
        out["cond"] = round(mcov.pct(sum(c[0] for c in data.conds.values()), sum(c[1] for c in data.conds.values())), 2)
    if data.fsms:
        out["fsm"] = round(mcov.pct(sum(v[0] + v[2] for v in data.fsms.values()), sum(v[1] + v[3] for v in data.fsms.values())), 2)
    if cgs:
        out["func"] = round(func, 2)
    if an:
        out["assert"] = round(mcov.pct(aok, an), 2)
    return out


def make_snapshot(plan, data, tri, when, label, trace=None):
    counts = {"pass": 0, "fail": 0, "xfail": 0, "skip": 0}
    for t in data.tests.values():
        counts[t["status"]] += 1
    extra = {"reqs": req_counts(trace[0])} if trace and trace[0] else {}
    return {
        **extra,
        "time": when, "label": label or when, "plan": plan.id, "score": round(plan.score, 2), "status": plan.status,
        "features": {f.path: [round(f.score, 2), f.status] for f, d in plan.walk() if d > 0},
        "tests": counts, "runs": len(data.runs), "coverage": coverage_totals(data),
        "failing": sorted(n for n, t in data.tests.items() if t["status"] == "fail"),
        "buckets": {b["signature"]: b["runs"] for b in tri["buckets"]},
    }


# ---------------------------------------------------------------- reports

def plan_json(plan, data, tri, snap, history, trace=None, rank=None):
    def feat(f):
        return {"id": f.id, "path": f.path, "title": f.title, "owner": f.owner_of(), "doc": f.doc, "weight": f.weight,
                "reqs": f.reqs,
                "score": round(f.score, 2), "status": f.status, "where": f.where,
                "metrics": [{"kind": m.kind, "pattern": m.pattern, "goal": m.goal, "weight": m.weight,
                             "score": round(m.score, 2), "hit": m.hit, "total": m.total, "matched": m.matched,
                             "met": m.met, "note": m.note, "bad": m.bad} for m in f.metrics],
                "features": [feat(c) for c in f.children]}
    return {"format": "mplan", "version": 1, "tool": f"mplan {VERSION}", "snapshot": snap, "plan": feat(plan),
            "requirements": reqs_json(*trace) if trace and trace[0] else None,
            "ranking": {**rank[0], "features": rank[1]} if rank else None,
            "triage": tri, "unmapped": unmapped(data), "history": [
                {"time": s.get("time"), "label": s.get("label"), "score": s.get("score"), "tests": s.get("tests"),
                 "coverage": s.get("coverage")} for s in history]}


def print_plan(plan, data, tri, prev, out=None, trace=None):
    out = out or sys.stdout
    rows = []
    for f, depth in plan.walk():
        rows.append(("  " * depth + f.id, f"{f.score:6.1f}%", f.status, f.owner_of(), ""))
        for m in f.metrics:
            detail = f"{m.hit}/{m.total}" + (f"  goal {m.goal:g}%" if m.goal != 100 else "") + (f"  ({m.note})" if m.note else "")
            rows.append(("  " * (depth + 1) + f"{m.kind} {m.pattern}", f"{m.score:6.1f}%", "met" if m.met else "below goal", "", detail))
    w0 = max(len(r[0]) for r in rows)
    w2 = max(len(r[2]) for r in rows)
    w3 = max([len(r[3]) for r in rows] + [5])
    print(f"{'Feature / metric':<{w0}}  {'Score':>7}  {'Status':<{w2}}  {'Owner':<{w3}}", file=out)
    for a, b, c, d, e in rows:
        print(f"{a:<{w0}}  {b:>7}  {c:<{w2}}  {d:<{w3}}  {e}".rstrip(), file=out)
    c = {"pass": 0, "fail": 0, "xfail": 0, "skip": 0}
    for t in data.tests.values():
        c[t["status"]] += 1
    feats = [f for f, d in plan.walk() if d > 0]
    met = sum(f.status == "met" for f in feats)
    line = f"\nmplan: {plan.id}: {plan.score:.1f}% ({plan.status}), {met}/{len(feats)} features met"
    if prev:
        line += f", {signed(plan.score - prev.get('score', 0))} since {prev.get('label') or prev.get('time')}"
    print(line, file=out)
    print(f"tests: {c['pass']} pass, {c['fail']} fail" + (f", {c['xfail']} known gaps" if c["xfail"] else "")
          + (f", {c['skip']} not run" if c["skip"] else "") + f" ({len(data.runs)} runs)", file=out)
    cov = coverage_totals(data)
    if cov:
        print("coverage: " + ", ".join(f"{k} {v:.1f}%" for k, v in cov.items()), file=out)
    if prev:
        before = prev.get("features", {})
        worse = [(f.path, before[f.path][0], f.score) for f in feats if f.path in before and f.score < before[f.path][0] - 0.005]
        for p, a, b in worse:
            print(f"  regressed: {p} {a:.1f}% -> {b:.1f}%", file=out)
    um = unmapped(data)
    parts = [f"{len(v)} {k}" for k, v in um.items() if v]
    if parts:
        print("not in the plan: " + ", ".join(parts), file=out)
    if trace:
        print_reqs(*trace, out)
    print_triage(tri, out)


def print_triage(tri, out=None):
    out = out or sys.stdout
    bs = tri["buckets"]
    if not bs and not tri["fixed"]:
        return
    new = sum(b["class"] == "new" for b in bs)
    print(f"\nfailure buckets: {len(bs)} ({new} new" + (f", {len(tri['fixed'])} fixed" if tri["fixed"] else "")
          + (f"; compared to {tri['compared_to']}" if tri["compared_to"] else "") + ")", file=out)
    for b in bs:
        tags = [b["class"]] + (["flaky"] if b["flaky"] else []) + ([b["status"]] if b["status"] != "open" else [])
        if b["class"] == "recurring" and b["first_seen"]:
            tags.append(f"since {b['first_seen']}")
        print(f"  {b['runs']:4d}  [{', '.join(tags)}]  {b['signature']}", file=out)
        names = b["tests"]
        print(f"        tests: {', '.join(names[:8])}{' ...' if len(names) > 8 else ''}", file=out)
        extra = [f"features: {', '.join(b['features'])}" if b["features"] else "",
                 f"owner: {', '.join(b['owners'])}" if b["owners"] else "", f"bug: {b['bug']}" if b["bug"] else "",
                 f"note: {b['note']}" if b["note"] else ""]
        if any(extra):
            print("        " + "; ".join(x for x in extra if x), file=out)
    for s in tri["fixed"]:
        print(f"  fixed: {s}", file=out)
    if tri["newly_failing"]:
        print(f"  newly failing tests: {', '.join(tri['newly_failing'][:12])}{' ...' if len(tri['newly_failing']) > 12 else ''}", file=out)


def write_md(plan, path):
    rows = ["| Feature | Score | Status | Owner |", "|---|---|---|---|"]
    for f, depth in plan.walk():
        rows.append(f"| {'&nbsp;&nbsp;' * depth}{f.title} | {f.score:.1f}% | {f.status} | {f.owner_of()} |")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(rows) + "\n")


CSS = """
.viz-root{color-scheme:light;--surface:#fcfcfb;--surface-2:#f3f2ef;--text:#0b0b0b;--text-2:#52514e;--muted:#8a8985;
--grid:#e4e3df;--series-1:#2a78d6;--series-2:#eb6834;--good:#0ca30c;--warning:#fab219;--serious:#ec835a;--critical:#d03b3b;--idle:#b9b8b2}
@media (prefers-color-scheme:dark){.viz-root{color-scheme:dark;--surface:#1a1a19;--surface-2:#262624;--text:#f0efec;
--text-2:#c3c2b7;--muted:#8a8985;--grid:#3a3a37;--series-1:#3987e5;--series-2:#d95926;--idle:#5c5b57}}
body{margin:0}
.viz-root{font:14px system-ui,sans-serif;background:var(--surface);color:var(--text);padding:24px;min-height:100vh;box-sizing:border-box}
h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:28px 0 8px}
.sub{color:var(--text-2);margin:0 0 16px}
.tiles{display:flex;flex-wrap:wrap;gap:12px}
.tile{background:var(--surface-2);border-radius:8px;padding:12px 16px;min-width:130px}
.tile b{display:block;font-size:24px;font-weight:600}.tile span{color:var(--text-2);font-size:12px}
table{border-collapse:collapse;width:100%}
td,th{border-bottom:1px solid var(--grid);padding:5px 8px;text-align:left;vertical-align:top}
th{color:var(--text-2);font-weight:500;font-size:12px}
td.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.bar{display:inline-block;width:120px;height:8px;background:var(--grid);border-radius:4px;overflow:hidden;vertical-align:middle;position:relative}
.bar i{display:block;height:100%;background:var(--series-1);border-radius:4px}
.bar u{position:absolute;top:0;bottom:0;width:2px;background:var(--text)}
.st{white-space:nowrap}.st::before{content:"";display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px;background:var(--idle)}
.st.met::before{background:var(--good)}.st.failing::before{background:var(--critical);border-radius:1px}
.st.partial::before{background:var(--warning);border-radius:50% 50% 0 0}.st.below::before{background:var(--serious);border-radius:50% 50% 0 0}
tr.metric td{color:var(--text-2);font-size:13px;border-bottom:none;padding-top:2px;padding-bottom:2px}
tr.feat td{font-weight:500}
code{font:12px ui-monospace,monospace}
.tag{display:inline-block;border:1px solid var(--grid);border-radius:10px;padding:0 7px;font-size:12px;margin-right:4px;color:var(--text-2)}
.tag.new{border-color:var(--critical);color:var(--text)}
details{margin:6px 0}summary{cursor:pointer;color:var(--text-2)}
.chart{position:relative;max-width:860px}
.legend{display:flex;gap:16px;color:var(--text-2);font-size:12px;margin:4px 0}
.legend i{display:inline-block;width:14px;height:2px;vertical-align:middle;margin-right:5px}
#tip{position:absolute;pointer-events:none;background:var(--surface-2);border:1px solid var(--grid);border-radius:6px;
padding:6px 9px;font-size:12px;display:none;white-space:nowrap}
svg text{fill:var(--text-2);font-size:11px}
"""

CHART_JS = """
(function(){var d=JSON.parse(document.getElementById('trend-data').textContent),svg=document.getElementById('trend'),
tip=document.getElementById('tip'),cur=document.getElementById('cursor');if(!svg)return;
svg.addEventListener('mousemove',function(e){var r=svg.getBoundingClientRect(),x=(e.clientX-r.left)/r.width*d.w,best=0,bd=1e9;
for(var i=0;i<d.x.length;i++){var k=Math.abs(d.x[i]-x);if(k<bd){bd=k;best=i}}
cur.setAttribute('x1',d.x[best]);cur.setAttribute('x2',d.x[best]);cur.style.display='';
var s=d.s[best];tip.innerHTML='<b>'+s.label+'</b><br>'+s.time+'<br>Plan score '+s.score+'%'+(s.pass==null?'':'<br>Tests passing '+s.pass+'%');
tip.style.display='block';var left=d.x[best]/d.w*r.width+12;if(left+tip.offsetWidth>r.width)left-=tip.offsetWidth+24;
tip.style.left=left+'px';tip.style.top='24px'});
svg.addEventListener('mouseleave',function(){tip.style.display='none';cur.style.display='none'})})();
"""


def trend_svg(history):
    """Plan score and test pass rate over the snapshots: one percent axis."""
    esc = html.escape
    pts = []
    for s in history:
        t = s.get("tests") or {}
        n = sum(t.get(k, 0) for k in ("pass", "fail", "xfail"))
        pts.append((s.get("label") or s.get("time", ""), s.get("time", ""), s.get("score", 0.0),
                    round(100.0 * t.get("pass", 0) / n, 1) if n else None))
    if len(pts) < 2:
        return "<p class='sub'>The trend chart appears with the second snapshot.</p>"
    W, H, L, R, T, B = 860, 240, 40, 90, 12, 28
    xs = [L + (W - L - R) * i / (len(pts) - 1) for i in range(len(pts))]
    y = lambda v: T + (H - T - B) * (1 - v / 100.0)
    out = [f"<div class='chart'><div class='legend'><span><i style='background:var(--series-1)'></i>Plan score</span>"
           f"<span><i style='background:var(--series-2)'></i>Tests passing</span></div>"
           f"<svg id='trend' viewBox='0 0 {W} {H}' role='img' aria-label='Plan score and tests passing per snapshot'>"]
    for g in (0, 25, 50, 75, 100):
        out.append(f"<line x1='{L}' x2='{W - R}' y1='{y(g):.1f}' y2='{y(g):.1f}' stroke='var(--grid)'/>"
                   f"<text x='{L - 6}' y='{y(g) + 4:.1f}' text-anchor='end'>{g}%</text>")
    step = max(1, len(pts) // 8)
    for i in range(0, len(pts), step):
        out.append(f"<text x='{xs[i]:.1f}' y='{H - 8}' text-anchor='middle'>{esc(str(pts[i][0])[:12])}</text>")
    for idx, color, name in ((3, "var(--series-2)", "Tests"), (2, "var(--series-1)", "Plan")):
        seq = [(xs[i], y(p[idx])) for i, p in enumerate(pts) if p[idx] is not None]
        if not seq:
            continue
        out.append(f"<polyline fill='none' stroke='{color}' stroke-width='2' stroke-linejoin='round' points='"
                   + " ".join(f"{a:.1f},{b:.1f}" for a, b in seq) + "'/>")
        a, b = seq[-1]
        out.append(f"<circle cx='{a:.1f}' cy='{b:.1f}' r='4' fill='{color}' stroke='var(--surface)' stroke-width='2'/>")
    # Direct labels at the line ends, nudged apart when they would collide.
    ends = sorted(((y(pts[-1][i]), n, pts[-1][i]) for i, n in ((2, "Plan"), (3, "Tests")) if pts[-1][i] is not None))
    if len(ends) == 2 and ends[1][0] - ends[0][0] < 14:
        mid = (ends[0][0] + ends[1][0]) / 2
        ends = [(mid - 7, ends[0][1], ends[0][2]), (mid + 7, ends[1][1], ends[1][2])]
    for yy, n, v in ends:
        out.append(f"<text x='{W - R + 10}' y='{yy + 4:.1f}'>{n} {v:.1f}%</text>")
    out.append(f"<line id='cursor' y1='{T}' y2='{H - B}' stroke='var(--muted)' style='display:none'/></svg><div id='tip'></div></div>")
    data = {"w": W, "x": [round(x, 1) for x in xs],
            "s": [{"label": esc(str(p[0])), "time": esc(str(p[1])), "score": round(p[2], 1), "pass": p[3]} for p in pts]}
    out.append(f"<script type='application/json' id='trend-data'>{json.dumps(data).replace('</', '<\\/')}</script>")
    rows = "".join(f"<tr><td>{esc(str(p[0]))}</td><td>{esc(str(p[1]))}</td><td class='num'>{p[2]:.1f}%</td>"
                   f"<td class='num'>{'' if p[3] is None else f'{p[3]:.1f}%'}</td></tr>" for p in pts)
    out.append(f"<details><summary>Snapshots as a table</summary><table><tr><th>Label</th><th>Time</th><th>Plan score</th>"
               f"<th>Tests passing</th></tr>{rows}</table></details>")
    return "".join(out)


def write_html(plan, data, tri, snap, history, path, trace=None, rank=None):
    esc = html.escape
    feats = [f for f, d in plan.walk() if d > 0]
    met = sum(f.status == "met" for f in feats)
    c = snap["tests"]
    cov = snap["coverage"]
    prev = history[-2] if len(history) > 1 else None
    delta = f"{signed(plan.score - prev.get('score', 0))} since {esc(str(prev.get('label') or prev.get('time')))}" if prev else "first snapshot"
    tiles = [(f"{plan.score:.1f}%", f"plan score ({delta})"), (f"{met}/{len(feats)}", "features met"),
             (f"{c['pass']}/{c['pass'] + c['fail'] + c['xfail']}", "tests passing"),
             (str(len(tri["buckets"])), f"failure buckets ({sum(b['class'] == 'new' for b in tri['buckets'])} new)")]
    if trace and trace[0]:
        rc = req_counts(trace[0])
        tiles.append((f"{rc['verified']}/{rc['total']}", f"requirements verified ({rc['uncovered']} uncovered)"))
    tiles += [(f"{v:.1f}%", {"func": "functional coverage", "assert": "assertions passed or hit", "cond": "condition coverage", "fsm": "FSM coverage"}.get(k, f"{k} coverage"))
              for k, v in cov.items()]
    doc = [f"<!DOCTYPE html><html><head><meta charset='utf-8'><title>mplan: {esc(plan.title)}</title><style>{CSS}</style></head>"
           f"<body><div class='viz-root'><h1>{esc(plan.title)}</h1><p class='sub'>Snapshot {esc(snap['label'])}, {esc(snap['time'])}. "
           f"Plan status: <span class='st {plan.status.split()[0]}'>{plan.status}</span></p><div class='tiles'>"
           + "".join(f"<div class='tile'><b>{a}</b><span>{esc(b)}</span></div>" for a, b in tiles) + "</div>"]
    doc.append("<h2>Progress</h2>" + trend_svg(history))

    def bar(score, goal=None):
        g = f"<u style='left:calc({goal:.0f}% - 1px)' title='goal {goal:g}%'></u>" if goal is not None and goal < 100 else ""
        return f"<span class='bar'><i style='width:{max(0.0, min(100.0, score)):.1f}%'></i>{g}</span>"

    rows = []
    for f, depth in plan.walk():
        pad = f"style='padding-left:{8 + 18 * depth}px'"
        title = esc(f.title) + (f" <code>{esc(f.path)}</code>" if depth and f.title != f.id else "")
        rows.append(f"<tr class='feat'><td {pad}>{title}{'<br><span class=sub>' + esc(f.doc) + '</span>' if f.doc else ''}"
                    f"{'<br><span class=sub>verifies ' + esc(', '.join(f.reqs)) + '</span>' if f.reqs else ''}</td>"
                    f"<td class='num'>{f.score:.1f}%</td><td>{bar(f.score)}</td>"
                    f"<td><span class='st {f.status.split()[0]}'>{f.status}</span></td><td>{esc(f.owner_of())}</td><td></td></tr>")
        for m in f.metrics:
            pad = f"style='padding-left:{8 + 18 * (depth + 1)}px'"
            bad = ""
            if m.bad:
                bad = "<br>" + esc(", ".join(m.bad[:10])) + (" ..." if len(m.bad) > 10 else "")
            state = "met" if m.met else "failing" if m.bad else "below"
            rows.append(f"<tr class='metric'><td {pad}>{m.kind} <code>{esc(m.pattern)}</code></td>"
                        f"<td class='num'>{m.score:.1f}%</td><td>{bar(m.score, m.goal)}</td>"
                        f"<td><span class='st {state}'>{'met' if m.met else 'failed' if m.bad else 'below goal'}</span></td><td></td>"
                        f"<td>{m.hit}/{m.total}{', goal ' + format(m.goal, 'g') + '%' if m.goal != 100 else ''}"
                        f"{'; ' + esc(m.note) if m.note else ''}{bad}</td></tr>")
    doc.append("<h2>Features</h2><table><tr><th>Feature / metric</th><th>Score</th><th></th><th>Status</th><th>Owner</th>"
               "<th>Detail</th></tr>" + "".join(rows) + "</table>")

    if trace and trace[0]:
        order, unknown = trace
        rows = []
        for r, depth in order:
            pad = f"style='padding-left:{8 + 18 * depth}px'"
            cls = {"verified": "met", "uncovered": "empty", "not started": "not"}.get(r.status, r.status)
            rows.append(f"<tr><td {pad}><code>{esc(r.id)}</code></td><td>{esc(r.title)}"
                        f"{'<br><span class=sub>' + esc(r.text[:300]) + '</span>' if r.text else ''}</td>"
                        f"<td>{esc(r.priority)}</td><td class='num'>{r.score:.1f}%</td><td>{bar(r.score)}</td>"
                        f"<td><span class='st {cls}'>{r.status}</span></td>"
                        f"<td>{', '.join('<code>' + esc(f.path) + '</code>' for f in r.features)}</td></tr>")
        doc.append("<h2>Requirements</h2><table><tr><th>Id</th><th>Requirement</th><th>Priority</th><th>Score</th><th></th>"
                   "<th>Status</th><th>Verified by</th></tr>" + "".join(rows) + "</table>")
        if unknown:
            doc.append("<p class='sub'>In no requirement list: " + ", ".join(f"<code>{esc(i)}</code> (feature {esc(p)})" for i, p in unknown) + "</p>")

    doc.append("<h2>Failure triage</h2>")
    if tri["compared_to"]:
        doc.append(f"<p class='sub'>Compared to snapshot {esc(str(tri['compared_to']))}.</p>")
    if tri["buckets"]:
        rows = []
        for b in tri["buckets"]:
            tags = f"<span class='tag {b['class']}'>{b['class']}</span>"
            if b["flaky"]:
                tags += "<span class='tag'>flaky</span>"
            if b["status"] != "open":
                tags += f"<span class='tag'>{b['status']}</span>"
            since = f"<br><span class='sub'>since {esc(str(b['first_seen']))} ({b['age']} snapshot(s))</span>" if b["class"] == "recurring" and b["first_seen"] else ""
            names = esc(", ".join(b["tests"][:12])) + (" ..." if len(b["tests"]) > 12 else "")
            who = esc(", ".join(b["owners"])) + (f"<br>{esc(b['bug'])}" if b["bug"] else "")
            rows.append(f"<tr><td class='num'>{b['runs']}</td><td>{tags}{since}</td><td><code>{esc(b['signature'])}</code>"
                        f"{'<br>' + esc(b['note']) if b['note'] else ''}<details><summary>reproduce</summary><code>{esc(b['cmd'])}</code>"
                        f"<br>log: <code>{esc(b['log'])}</code></details></td><td>{names}</td>"
                        f"<td>{esc(', '.join(b['features']))}</td><td>{who}</td></tr>")
        doc.append("<table><tr><th>Runs</th><th>Class</th><th>Signature</th><th>Tests</th><th>Features blocked</th><th>Owner</th></tr>"
                   + "".join(rows) + "</table>")
    else:
        doc.append("<p>No failing runs.</p>")
    for title, items in (("Buckets fixed since the last snapshot", tri["fixed"]), ("Newly failing tests", tri["newly_failing"]),
                         ("Tests fixed since the last snapshot", tri["fixed_tests"])):
        if items:
            doc.append(f"<details open><summary>{title} ({len(items)})</summary><p>" + "<br>".join(f"<code>{esc(i)}</code>" for i in items[:200]) + "</p></details>")

    if rank:
        rk, attr = rank
        rows = "".join(f"<tr><td class='num'>{i}</td><td><code>{esc(o['test'])}</code></td><td class='num'>{o['new']}</td>"
                       f"<td class='num'>{o['cumulative']:.1f}%</td><td>{bar(o['cumulative'])}</td><td class='num'>{o['seconds']:.1f}</td>"
                       f"<td class='num'>{o['unique']}</td></tr>" for i, o in enumerate(rk["order"][:500], 1))
        doc.append(f"<h2>Tests by what they add</h2><p class='sub'>{len(rk['order'])} of {len(rk['order']) + len(rk['redundant'])} tests reach "
                   f"{rk['coverage']:.1f}% of {rk['points']} coverage points in {rk['seconds_ranked']:.1f}s of {rk['seconds']:.1f}s "
                   f"(ordered by {'new points per second' if rk['by'] == 'rate' else 'new points'}).</p>"
                   "<table><tr><th>Rank</th><th>Test</th><th>New points</th><th>Coverage so far</th><th></th><th>Seconds</th>"
                   "<th>Unique points</th></tr>" + rows + "</table>")
        for title, items in (("Redundant tests (add nothing)", rk["redundant"]), ("Tests without coverage data", rk["no_data"])):
            if items:
                doc.append(f"<details><summary>{title} ({len(items)})</summary><p>" + "<br>".join(f"<code>{esc(i)}</code>" for i in items[:500]) + "</p></details>")
        if attr:
            doc.append("<details><summary>Tests behind each feature's coverage</summary><table><tr><th>Feature</th><th>Tests</th><th>Most points</th></tr>"
                       + "".join(f"<tr><td><code>{esc(p)}</code></td><td class='num'>{a['tests']}</td><td>"
                                 + ", ".join(f"<code>{esc(n)}</code> ({c})" for n, c in a["top"][:5]) + "</td></tr>" for p, a in attr.items())
                       + "</table></details>")

    um = unmapped(data)
    if any(um.values()):
        doc.append("<h2>Not in the plan</h2><p class='sub'>Results that no plan line points at.</p>")
        for k, v in um.items():
            if v:
                doc.append(f"<details><summary>{len(v)} {k}</summary><p>" + "<br>".join(f"<code>{esc(i)}</code>" for i in v[:500])
                           + ("<br>..." if len(v) > 500 else "") + "</p></details>")
    doc.append(f"<script>{CHART_JS}</script></div></body></html>")
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(doc))


# ---------------------------------------------------------------- commands

def find_cov(dirs, covs):
    if covs:
        return covs
    return [p for p in (os.path.join(d, "cov.json") for d in dirs if os.path.isdir(d)) if os.path.exists(p)]


def load_data(a):
    jobs = a.threads or cpu_count()
    runs = load_runs(a.results, jobs)
    covs = find_cov(a.results, a.cov)
    for c in covs:
        if not os.path.exists(c):
            raise SystemExit(f"mplan: {c}: no such coverage database")
    db = tool("mcov").load_all(covs, jobs) if covs else None
    return Data(runs, db), jobs


def cmd_check(a):
    plan = parse_plan(a.plan)
    feats = [f for f, d in plan.walk() if d > 0]
    nm = sum(len(f.metrics) for f, _ in plan.walk())
    empty = [f.path for f in feats if not f.metrics and not f.children]
    print(f"mplan: {a.plan}: plan {plan.id!r}, {len(feats)} feature(s), {nm} metric(s)")
    for p in empty:
        print(f"  nothing mapped yet: {p}")
    order, unknown = trace_reqs(plan, load_reqs(plan.req_files + a.reqs))
    if order:
        leaves = [r for r, _ in order if not r.children]
        bare = [r for r in leaves if not r.features]
        print(f"  {len(leaves)} requirement(s), {len(leaves) - len(bare)} named by a feature")
        for r in bare:
            print(f"  no feature verifies: {r.id}" + (f"  {r.title}" if r.title else ""))
        for i, p in unknown:
            print(f"  warning: feature {p} names requirement {i!r}, which is in no requirement list")
    return 0


def cmd_report(a):
    plan = parse_plan(a.plan)
    data, jobs = load_data(a)
    if not a.results and data.db is None:
        raise SystemExit("mplan: report needs -results DIR and / or -cov DB")
    score_feature(plan, data)
    trace = trace_reqs(plan, load_reqs(plan.req_files + a.reqs))
    outdir = os.path.abspath(a.o)
    hpath = a.history or os.path.join(outdir, "history.jsonl")
    history = read_history(hpath)
    when, label = snapshot_time(a.time), a.label or default_label()
    # The same label again replaces that snapshot: compare with the ones before it.
    if history and label and history[-1].get("label") == label:
        history.pop()
    tri = triage(data.runs, data.tests, plan, history, load_rules(a.rules), jobs, data.m["test"])
    snap = make_snapshot(plan, data, tri, when, label, trace)
    rank = None
    if a.rank:
        pts = Points(data.runs, jobs)
        rank = (rank_tests(pts, a.by), attribute(plan, data, pts))
    prev = history[-1] if history else None
    history.append(snap)
    os.makedirs(outdir, exist_ok=True)
    if not a.no_history:
        write_history(hpath, history)
    with open(os.path.join(outdir, "plan.json"), "w", encoding="utf-8") as f:
        # No indent: that keeps the C encoder, several times faster on a large plan.
        f.write(json.dumps(plan_json(plan, data, tri, snap, history, trace, rank), sort_keys=True) + "\n")
    write_html(plan, data, tri, snap, history, os.path.join(outdir, "plan.html"), trace, rank)
    if a.md:
        write_md(plan, a.md)
    if a.trace:
        write_trace(trace[0], a.trace)
    if not a.q:
        print_plan(plan, data, tri, prev, trace=trace)
        if rank:
            print()
            print_rank(rank[0], limit=10)
    else:
        print(f"mplan: {plan.id}: {plan.score:.1f}% ({plan.status})")
    print(f"report: {os.path.relpath(os.path.join(outdir, 'plan.html'))}")
    rc = 0
    if a.gate and plan.status != "met":
        print(f"mplan: gate: the plan is {plan.status}, not met")
        rc = 1
    if a.min_score is not None and plan.score + 1e-9 < a.min_score:
        print(f"mplan: gate: plan score {plan.score:.1f}% is below {a.min_score:g}%")
        rc = 1
    if a.fail_on_new:
        fresh = [b for b in tri["buckets"] if b["class"] == "new" and b["status"] == "open"]
        if fresh:
            print(f"mplan: gate: {len(fresh)} new failure bucket(s)")
            rc = 1
    if a.fail_on_uncovered:
        bare = [r.id for r, _ in trace[0] if r.status == "uncovered" and not r.children]
        if bare or trace[1]:
            print(f"mplan: gate: {len(bare)} uncovered requirement(s)" + (f", {len(trace[1])} unknown id(s)" if trace[1] else ""))
            rc = 1
    return rc


def cmd_triage(a):
    if not a.results:
        raise SystemExit("mplan: triage needs -results DIR")
    plan = parse_plan(a.plan) if a.plan else None
    jobs = a.threads or cpu_count()
    runs = load_runs(a.results, jobs)
    tests = test_table(runs)
    tri = triage(runs, tests, plan, read_history(a.history), load_rules(a.rules), jobs)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(tri, f, indent=1, sort_keys=True)
    if not tri["buckets"] and not tri["fixed"]:
        print("mplan: no failing runs")
    print_triage(tri)
    fresh = [b for b in tri["buckets"] if b["class"] == "new" and b["status"] == "open"]
    return 1 if a.fail_on_new and fresh else 0


def cmd_rank(a):
    if not a.results:
        raise SystemExit("mplan: rank needs -results DIR")
    plan = parse_plan(a.plan) if a.plan else None
    data, jobs = load_data(a)
    pts = Points(data.runs, jobs, a.all)
    if not pts.total:
        raise SystemExit("mplan: rank: no per-run coverage (run mregress with -cov)")
    rk = rank_tests(pts, a.by)
    attr = attribute(plan, data, pts) if plan is not None and data.db is not None else None
    print_rank(rk, attr, a.n or 0)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"format": "mplan-rank", "version": 1, **rk, "features": attr}, f, indent=1, sort_keys=True)
            f.write("\n")
    if a.keep:
        with open(a.keep, "w", encoding="utf-8") as f:
            f.write("".join(o["test"] + "\n" for o in rk["order"]))
    return 0


def cmd_trend(a):
    snaps = read_history(a.history)
    if not snaps:
        raise SystemExit(f"mplan: {a.history}: no snapshots")
    if a.n:
        snaps = snaps[-a.n:]
    covk = [k for k in ("line", "branch", "toggle", "cond", "fsm", "func", "assert") if any(k in (s.get("coverage") or {}) for s in snaps)]
    has_reqs = any(s.get("reqs") for s in snaps)
    head = ["Time", "Label", "Score", "Change", "Tests", "Buckets"] + (["Reqs"] if has_reqs else []) + [k.capitalize() for k in covk]
    rows, last = [], None
    for s in snaps:
        if a.feature:
            got = s.get("features", {}).get(a.feature)
            score = got[0] if got else None
        else:
            score = s.get("score")
        t = s.get("tests") or {}
        n = sum(t.get(k, 0) for k in ("pass", "fail", "xfail"))
        rows.append([s.get("time", ""), str(s.get("label", ""))[:16], "-" if score is None else f"{score:.1f}%",
                     "" if score is None or last is None else signed(score - last), f"{t.get('pass', 0)}/{n}",
                     str(len(s.get("buckets", {})))]
                    + ([f"{s['reqs']['verified']}/{s['reqs']['total']}" if s.get("reqs") else "-"] if has_reqs else [])
                    + [f"{s['coverage'][k]:.1f}%" if k in (s.get("coverage") or {}) else "-" for k in covk])
        if score is not None:
            last = score
    ws = [max(len(h), *(len(r[i]) for r in rows)) for i, h in enumerate(head)]
    print("  ".join(h.ljust(w) for h, w in zip(head, ws)).rstrip())
    for r in rows:
        print("  ".join(c.ljust(w) for c, w in zip(r, ws)).rstrip())
    return 0


def cmd_init(a):
    """A starting plan from what a regression holds: tests by name prefix, coverage by scope."""
    data, _ = load_data(a)
    reqs = load_reqs(a.reqs)
    if not data.tests and data.db is None and not reqs:
        raise SystemExit("mplan: init needs -results DIR, -cov DB and / or -reqs FILE")
    groups = {}
    for n in sorted(data.tests):
        key = re.split(r"[_.\-/]", n, maxsplit=1)[0] or n
        groups.setdefault(key, []).append(n)
    name = a.name or "design"
    out = [f"# Draft plan written by mplan init: regroup, rename and add the features that have no test yet.",
           f"plan {name} \"{name} verification plan\"", "  goal: 100"]
    if reqs:
        # One feature per requirement, nested like the requirements; the tests and coverage go in by hand.
        out += [f"  reqs {os.path.relpath(p, os.path.dirname(os.path.abspath(a.o)) if a.o else os.getcwd())}" for p in a.reqs]
        used = set()

        def emit(r, depth):
            ident = re.sub(r"[^A-Za-z0-9_\-]", "_", r.id)
            while (depth, ident) in used:
                ident += "_"
            used.add((depth, ident))
            pad = "  " * (depth + 1)
            out.append(f"{pad}feature {ident}" + (f" {json.dumps(r.title, ensure_ascii=False)}" if r.title else ""))
            if not r.children:
                out.append(f"{pad}  req {r.id}")
            for c in r.children:
                emit(c, depth + 1)
        for r in reqs.values():
            if not r.parent:
                emit(r, 0)
    for key, names in groups.items():
        ident = re.sub(r"[^A-Za-z0-9_\-]", "_", key)
        out.append(f"  feature {ident}")
        out.append(f"    test {key}*" if len(names) > 1 or names[0] != key else f"    test {key}")
    if data.db is not None:
        if data.fsms:
            out.append("  feature fsm \"State machines\"")
            out += [f"    fsm {n}" for n in sorted(data.fsms)]
        if data.types:
            out.append("  feature functional \"Functional coverage\"")
            out += [f"    cover {t}" for t in sorted(data.types)]
        if data.asserts:
            out.append("  feature assertions \"Assertions\"")
            scopes = sorted({n.rsplit(".", 1)[0] if "." in n else n for n in data.asserts})
            out += [f"    assert {s}" for s in scopes]
        if data.files:
            out.append("  feature code \"Code coverage\"")
            common = os.path.commonpath(list(data.files)) if len(data.files) > 1 else os.path.dirname(next(iter(data.files)))
            for fpath in sorted(data.files):
                rel = os.path.relpath(fpath, common) if common else fpath
                out.append(f"    line {rel}")
                out.append(f"    branch {rel}")
                if fpath in data.conds:
                    out.append(f"    cond {rel}")
    text = "\n".join(out) + "\n"
    if a.o:
        with open(a.o, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"mplan: wrote {a.o}")
    else:
        sys.stdout.write(text)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mplan", description="Verification planning and tracking (see the module docstring).")
    ap.add_argument("-version", action="version", version=f"mplan {VERSION}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def inputs(p):
        p.add_argument("-results", action="append", default=[], metavar="DIR",
                       help="an mregress output directory (or its results.json); repeat for several")
        p.add_argument("-cov", action="append", default=[], metavar="DB",
                       help="a coverage database (default: cov.json in each -results directory); repeat for several")
        p.add_argument("-threads", "-j", type=int, default=0, dest="threads",
                       help="workers for reading the inputs (default: every logical CPU this process may use)")

    def reqopt(p):
        p.add_argument("-reqs", action="append", default=[], metavar="FILE",
                       help="a requirement list, CSV or ReqIF (.reqif / .reqifz); repeat for several")

    c = sub.add_parser("check", help="read a plan and report mistakes")
    c.add_argument("plan")
    reqopt(c)
    r = sub.add_parser("report", help="score a plan, record a snapshot, write plan.json and plan.html")
    r.add_argument("plan")
    inputs(r)
    r.add_argument("-o", default="plan.d", help="output directory (default plan.d)")
    r.add_argument("-history", metavar="FILE", help="snapshot history (default <o>/history.jsonl)")
    r.add_argument("-no-history", action="store_true", help="do not record this run in the history")
    r.add_argument("-label", help="name of this snapshot (default: GIT_COMMIT / GITHUB_SHA, else the time)")
    r.add_argument("-time", help="time of this snapshot, ISO 8601 (default: SOURCE_DATE_EPOCH, else now)")
    r.add_argument("-rules", metavar="FILE", help="triage rules: /regex/ status=known|waived owner=.. bug=.. note=..")
    r.add_argument("-md", metavar="FILE", help="also write the feature table as Markdown")
    r.add_argument("-rank", action="store_true", help="also rank the tests by the coverage each adds (see mplan rank)")
    r.add_argument("-by", choices=("points", "rate"), default="points", help="-rank order: new points, or new points per second")
    reqopt(r)
    r.add_argument("-trace", metavar="FILE", help="also write the requirement traceability matrix as CSV")
    r.add_argument("-fail-on-uncovered", action="store_true",
                   help="exit 1 when a requirement has no feature, or the plan names an id that no list holds")
    r.add_argument("-gate", action="store_true", help="exit 1 unless the whole plan is met")
    r.add_argument("-min-score", type=float, metavar="PCT", help="exit 1 if the plan score is below PCT")
    r.add_argument("-fail-on-new", action="store_true", help="exit 1 on a new failure bucket that no rule marks known or waived")
    r.add_argument("-q", action="store_true", help="print only the summary line")
    t = sub.add_parser("triage", help="sort the failures of a regression into buckets")
    inputs(t)
    t.add_argument("-plan", help="a plan, to name the features and owners each bucket blocks")
    t.add_argument("-history", metavar="FILE", help="snapshot history to compare with (new / recurring / fixed)")
    t.add_argument("-rules", metavar="FILE", help="triage rules")
    t.add_argument("-json", metavar="FILE", help="also write the buckets as JSON")
    t.add_argument("-fail-on-new", action="store_true", help="exit 1 on a new bucket that no rule marks known or waived")
    k = sub.add_parser("rank", help="rank the tests by the coverage each adds; list the redundant ones")
    k.add_argument("plan", nargs="?", help="a plan, to list the tests behind each feature's coverage")
    inputs(k)
    k.add_argument("-by", choices=("points", "rate"), default="points", help="order by new points (default) or new points per second")
    k.add_argument("-all", action="store_true", help="rank failing tests too")
    k.add_argument("-n", type=int, help="print only the first N ranks")
    k.add_argument("-json", metavar="FILE", help="also write the ranking as JSON")
    k.add_argument("-keep", metavar="FILE", help="write the tests that add coverage, one name a line, in rank order")
    d = sub.add_parser("trend", help="print the history as a table")
    d.add_argument("history")
    d.add_argument("-n", type=int, help="only the last N snapshots")
    d.add_argument("-feature", metavar="PATH", help="the score of one feature (e.g. tx.parity) instead of the plan's")
    i = sub.add_parser("init", help="write a draft plan from a regression's tests and coverage")
    inputs(i)
    reqopt(i)
    i.add_argument("-name", help="plan name (default: design)")
    i.add_argument("-o", metavar="FILE", help="write the plan here (default: standard output)")
    a = ap.parse_args(argv)
    try:
        return {"check": cmd_check, "report": cmd_report, "triage": cmd_triage, "trend": cmd_trend, "init": cmd_init,
                "rank": cmd_rank}[a.cmd](a)
    except PlanError as e:
        print(f"mplan: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
