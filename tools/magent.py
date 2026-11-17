#!/usr/bin/env python3
"""magent: verification agents on the minisim engines, judged by the engines.

A model proposes; msim, mlint, mcov, mdebug and mregress decide. Every agent
keeps a result only when an engine measures it as better, and reports the
measurement, so a run can be scored without reading what the model wrote.

    magent triage   -results regress.d [-root .]                 # explain every failure group
    magent lintfix  rtl/*.sv -top core [-only LAT001,W001] [-regress tests.list] [-apply]
    magent covclose rtl/*.sv -top core -tb tb/tb.sv [-goal line=95,branch=90] [-rounds 5]

Models. -model picks one: claude-opus-5-5 (the default; Anthropic SDK,
ANTHROPIC_API_KEY or `ant auth login`), or any OpenAI-compatible endpoint
with -api openai -base-url URL (DeepSeek, Qwen, vLLM, Ollama; the key from
$MAGENT_API_KEY). The tools the model calls are mmcp's (docs/MCP.md), so an
agent sees exactly what an outside MCP client would.

What each agent measures (all in the JSON report, -o):
  triage    groups explained, cited locations that exist, tests that pass
            again after a suggested fix when -verify is given
  lintfix   findings removed, findings added (must be 0), compiles, regression
            and equivalence (mequiv, when installed) on every accepted patch
  covclose  coverage before / after per metric, tests kept (each must pass and
            add coverage), rounds used

Files are only read and written under the root. lintfix edits a copy and
writes a patch; -apply copies accepted files back. covclose writes new tests
to -o.
"""
import argparse
import difflib
import importlib.machinery
import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile
import time
import urllib.request

VERSION = "0.1.0"
HERE = os.path.dirname(os.path.realpath(__file__))
DEFAULT_MODEL = "claude-opus-5-5"


def load_mmcp():
    for name in ("mmcp.py", "mmcp"):
        p = os.path.join(HERE, name)
        if os.path.isfile(p):
            loader = importlib.machinery.SourceFileLoader("mmcp", p)
            spec = importlib.util.spec_from_loader("mmcp", loader)
            mod = importlib.util.module_from_spec(spec)
            loader.exec_module(mod)
            return mod
    raise SystemExit("magent: mmcp is not installed next to magent")


mmcp = load_mmcp()


# -- models -------------------------------------------------------------------
class Backend:
    """One conversation with a model that can call tools.

    start(system, text, tools); then turn() -> (text, [(id, name, input)]);
    answer([(id, result_text, is_error)]) after running the calls; turn() again.
    An empty call list means the model is done."""

    calls = 0          # model requests made, for the report


class AnthropicBackend(Backend):
    def __init__(self, model, effort="high", max_tokens=16000):
        try:
            import anthropic
        except ImportError:
            raise SystemExit("magent: the Claude backend needs the Anthropic SDK: pip install anthropic")
        self.anthropic = anthropic
        self.client = anthropic.Anthropic()
        self.model, self.effort, self.max_tokens = model, effort, max_tokens

    def start(self, system, text, tools):
        self.system = system
        self.tools = [{"name": t["name"], "description": t["description"], "input_schema": t["inputSchema"]} for t in tools]
        self.messages = [{"role": "user", "content": text}]

    def turn(self):
        while True:
            self.calls += 1
            r = self.client.beta.messages.create(
                model=self.model, max_tokens=self.max_tokens, system=self.system, tools=self.tools,
                messages=self.messages, output_config={"effort": self.effort},
                betas=["server-side-fallback-2026-07-01"], fallbacks="default")
            self.messages.append({"role": "assistant", "content": r.content})
            if r.stop_reason == "pause_turn":
                continue
            if r.stop_reason == "refusal":
                raise RuntimeError("the model declined the request")
            text = "".join(b.text for b in r.content if b.type == "text")
            calls = [(b.id, b.name, b.input) for b in r.content if b.type == "tool_use"]
            return text, calls

    def answer(self, results):
        self.messages.append({"role": "user", "content": [
            {"type": "tool_result", "tool_use_id": i, "content": out, "is_error": bool(err)} for i, out, err in results]})


class OpenAIBackend(Backend):
    """Chat Completions with function tools: DeepSeek, Qwen, vLLM, Ollama, ..."""

    def __init__(self, model, base_url, key=None, timeout=600):
        self.model, self.url, self.timeout = model, base_url.rstrip("/") + "/chat/completions", timeout
        self.key = key or os.environ.get("MAGENT_API_KEY", "")

    def start(self, system, text, tools):
        self.tools = [{"type": "function", "function": {"name": t["name"], "description": t["description"],
                                                         "parameters": t["inputSchema"]}} for t in tools]
        self.messages = [{"role": "system", "content": system}, {"role": "user", "content": text}]

    def turn(self):
        self.calls += 1
        body = json.dumps({"model": self.model, "messages": self.messages, "tools": self.tools}).encode()
        req = urllib.request.Request(self.url, body, {"Content-Type": "application/json",
                                                      **({"Authorization": "Bearer " + self.key} if self.key else {})})
        with urllib.request.urlopen(req, timeout=self.timeout) as f:
            msg = json.load(f)["choices"][0]["message"]
        self.messages.append({k: v for k, v in msg.items() if k in ("role", "content", "tool_calls")})
        calls = []
        for c in msg.get("tool_calls") or []:
            try:
                args = json.loads(c["function"].get("arguments") or "{}")
            except ValueError:
                args = {"_bad_json": c["function"].get("arguments")}
            calls.append((c["id"], c["function"]["name"], args))
        return msg.get("content") or "", calls

    def answer(self, results):
        for i, out, _err in results:
            self.messages.append({"role": "tool", "tool_call_id": i, "content": out})


def backend_from(a):
    if a.api == "openai":
        if not a.base_url:
            raise SystemExit("magent: -api openai needs -base-url")
        return OpenAIBackend(a.model, a.base_url)
    return AnthropicBackend(a.model, a.effort)


# -- the loop -----------------------------------------------------------------
MAX_RESULT = 60_000


class Session:
    """Runs a model against a set of mmcp tools plus the agent's own tools."""

    def __init__(self, srv, backend, tool_names, local=None, max_steps=40, log=None):
        self.srv, self.backend, self.max_steps, self.log = srv, backend, max_steps, log
        self.local = local or {}
        listed = {t["name"]: t for t in srv.listed()}
        self.tools = [listed[n] for n in tool_names if n in listed] + [
            {"name": n, "description": d, "inputSchema": s} for n, (d, s, _) in self.local.items()]
        self.tool_calls = 0

    def run(self, system, text):
        self.backend.start(system, text, self.tools)
        final = ""
        for _ in range(self.max_steps):
            said, calls = self.backend.turn()
            final = said or final
            if self.log:
                self.log({"model": said[:2000], "calls": [(n, i) for _, n, i in calls]})
            if not calls:
                return final
            results = []
            for cid, name, args in calls:
                self.tool_calls += 1
                out, err = self.call(name, args)
                text_out = json.dumps(out)
                if len(text_out) > MAX_RESULT:
                    text_out = text_out[:MAX_RESULT] + ' ... (cut; use read_file for the rest)'
                results.append((cid, text_out, err))
            self.backend.answer(results)
            if any(n == "finish" for _, n, _ in calls):
                return final
        return final

    def call(self, name, args):
        if name in self.local:
            try:
                return self.local[name][2](args), False
            except (ValueError, KeyError, mmcp.ToolError) as e:
                return {"error": str(e)}, True
        if name not in [t["name"] for t in self.tools]:
            return {"error": f"no tool {name}"}, True
        r = mmcp.result_of(self.srv, name, args)
        return r["structuredContent"], r["isError"]


def obj(props, required=()):
    return {"type": "object", "properties": props, "required": list(required)}


S = {"type": "string"}


# -- triage --------------------------------------------------------------------
TRIAGE_SYSTEM = """You triage failing verification runs for a hardware design team. For each failure group
you are given, find the root cause using the tools: read the failing run's log (read_file), look at the
design and testbench source, and when a waveform or design is available ask mdebug_query (drivers,
loads, value, changes, active). Decide whether it is a design bug, a testbench bug, a tool problem, an
environment / setup problem, or a flaky (seed-dependent) failure. Cite the source file and line where
the cause is, quoting the line. Then call submit_triage once per group. Be concrete: name signals,
times and values you saw. If the evidence does not settle it, say so and give your best hypothesis
with confidence "low"."""

CATEGORIES = ["design_bug", "testbench_bug", "tool_bug", "environment", "flaky", "unknown"]


def triage(srv, backend, a, log):
    res_dir = srv.path(a.results)
    with open(os.path.join(res_dir, "results.json"), encoding="utf-8") as f:
        res = json.load(f)
    groups = {}
    for r in res["runs"]:
        if r["status"] in ("fail", "xpass"):
            groups.setdefault(r.get("signature") or r["reason"], []).append(r)
    order = sorted(groups.items(), key=lambda kv: -len(kv[1]))[: a.max_groups]
    answers = {}

    def submit(x):
        sig = x["group"]
        if sig not in groups:
            raise ValueError(f"group must be one of the given signatures, not {sig!r}")
        if x.get("category") not in CATEGORIES:
            raise ValueError(f"category must be one of {CATEGORIES}")
        answers[sig] = x
        return {"recorded": sig, "remaining": [s for s, _ in order if s not in answers]}

    local = {"submit_triage": (
        "Record the diagnosis of one failure group.",
        obj({"group": {"type": "string", "description": "the group's signature, exactly as given"},
             "category": {"type": "string", "enum": CATEGORIES},
             "root_cause": S, "file": S, "line": {"type": "integer"},
             "quote": {"type": "string", "description": "the source line at file:line"},
             "evidence": S, "suggested_fix": S,
             "confidence": {"type": "string", "enum": ["high", "medium", "low"]}},
            ["group", "category", "root_cause", "confidence"]), submit)}
    sess = Session(srv, backend, ["read_file", "mdebug_query", "msim_run", "mregress_run"], local, a.max_steps, log)
    brief = [{"group": sig, "runs": len(runs), "reason": runs[0]["reason"],
              "tests": sorted({r["test"] for r in runs})[:20],
              "seeds": sorted({r["seed"] for r in runs})[:10],
              "log": srv.rel(os.path.join(res_dir, runs[0]["log"])) if runs[0].get("log") else None,
              "command": runs[0].get("cmd")} for sig, runs in order]
    passing = sorted({r["test"] for r in res["runs"] if r["status"] == "pass"})[:50]
    text = ("Failure groups (most runs first):\n" + json.dumps(brief, indent=1) +
            f"\n\nTests that pass in the same regression: {passing}\nDiagnose every group and submit each.")
    t0 = time.time()
    sess.run(TRIAGE_SYSTEM, text)
    rows = []
    for sig, runs in order:
        x = answers.get(sig)
        row = {"group": sig, "runs": len(runs), "diagnosed": x is not None}
        if x:
            row.update(x)
            row["location_valid"] = _location_ok(srv, x)
        rows.append(row)
    n = len(rows)
    metrics = {"groups": n, "diagnosed": sum(r["diagnosed"] for r in rows),
               "locations_cited": sum(1 for r in rows if r.get("file")),
               "locations_valid": sum(1 for r in rows if r.get("location_valid")),
               "seconds": round(time.time() - t0, 1), "model_calls": backend.calls, "tool_calls": sess.tool_calls}
    if a.labels:
        metrics.update(_score_labels(srv, rows, a.labels))
    return {"agent": "triage", "metrics": metrics, "groups": rows}


def _location_ok(srv, x):
    """The cited file exists under the root and the line is in it; the quote matches when given."""
    if not x.get("file"):
        return False
    try:
        p = srv.path(x["file"])
        with open(p, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except (mmcp.ToolError, OSError):
        return False
    ln = x.get("line")
    if not isinstance(ln, int) or not 1 <= ln <= len(lines):
        return False
    q = (x.get("quote") or "").strip()
    if q:
        near = " ".join(lines[max(0, ln - 3): ln + 2])
        return _norm(q) in _norm(near)
    return True


def _norm(s):
    return re.sub(r"\s+", " ", s).strip()


def _score_labels(srv, rows, path):
    """Labels: {signature or test name: {"category": ..., "file": ..., "line": N}}; a hit is the
    right category and a cited line within 3 of the labelled one."""
    with open(srv.path(path), encoding="utf-8") as f:
        labels = json.load(f)
    cat = loc = n = 0
    for r in rows:
        lab = labels.get(r["group"])
        if lab is None:
            continue
        n += 1
        cat += r.get("category") == lab.get("category")
        if lab.get("file") and r.get("file") and os.path.basename(r["file"]) == os.path.basename(lab["file"]) \
                and isinstance(r.get("line"), int) and abs(r["line"] - lab.get("line", -99)) <= 3:
            loc += 1
    return {"labelled": n, "category_correct": cat, "location_correct": loc}


# -- lint fix ------------------------------------------------------------------
LINTFIX_SYSTEM = """You fix lint findings in SystemVerilog / Verilog RTL without changing what the design does,
unless a finding is a real bug (a latch, a width truncation, a multiply-driven net) where the
intent is clear from the code. Read the source with read_file, then call edit_file with exact
old / new text (old must occur once in the file). Change only what the findings need: no renaming,
reformatting or unrelated clean-up. Each edit is checked by re-running mlint, compiling with msim
and, when available, a regression and an equivalence check; you get the result back and may try
again. Call finish when you are done or when a finding should be waived instead (say why)."""


def _key(f):
    return (f.get("rule"), f.get("signal") or f.get("text"), os.path.basename(f.get("file") or ""))


def lintfix(srv, backend, a, log):
    files = [srv.rel(srv.path(f)) for f in a.files]
    work = srv.workdir("lintfix", a.out)
    copy = os.path.join(work, "tree")
    _copy_tree(srv.root, copy)
    csrv = mmcp.Server(copy, srv.bins, (), srv.timeout)
    lint_args = {"files": files, "top": a.top, "max_findings": 500}
    if a.only:
        lint_args["only"] = a.only
    if a.lint_args:
        lint_args["args"] = a.lint_args
    before = mmcp.result_of(csrv, "mlint_check", lint_args)["structuredContent"]
    if not before.get("ok"):
        return {"agent": "lintfix", "error": "mlint did not run", "detail": before}
    base_findings = [f for f in before["findings"] if f.get("severity") in ("error", "warning")]
    base_keys = {_key(f) for f in base_findings}
    base_reg = None
    if a.regress:
        base_reg = mmcp.result_of(csrv, "mregress_run", {"lists": [a.regress], "out": os.path.join(work, "reg0")})["structuredContent"]
    originals = {f: _read(os.path.join(copy, f)) for f in files}
    state = {"accepted": [], "rejected": [], "done": False, "waived": []}

    def check():
        """Every engine's verdict on the copy as it is now."""
        lint = mmcp.result_of(csrv, "mlint_check", lint_args)["structuredContent"]
        now = [f for f in lint.get("findings", []) if f.get("severity") in ("error", "warning")]
        keys = {_key(f) for f in now}
        v = {"removed": len(base_keys - keys), "added": sorted(map(list, keys - base_keys)),
             "remaining": len(keys & base_keys)}
        sim = mmcp.result_of(csrv, "msim_run", {"files": files, "top": a.top, "args": ["-elab"]})["structuredContent"]
        v["compiles"] = sim.get("status") != "compile_error"
        if not v["compiles"]:
            v["compile_errors"] = sim.get("errors")
        if a.regress:
            reg = mmcp.result_of(csrv, "mregress_run", {"lists": [a.regress], "out": os.path.join(work, "reg")})["structuredContent"]
            v["regression"] = {"passed": reg.get("passed"), "failed": reg.get("failed")}
            v["regression_ok"] = reg.get("ok") and reg.get("failed", 1) <= (base_reg or {}).get("failed", 0)
        if srv.find("mequiv") and not a.no_equiv:
            ref = [os.path.join(work, "orig", f) for f in files]
            for f in files:
                os.makedirs(os.path.dirname(os.path.join(work, "orig", f)), exist_ok=True)
                with open(os.path.join(work, "orig", f), "w", encoding="utf-8") as fh:
                    fh.write(originals[f])
            rc, out, err, _, _ = csrv.run([srv.find("mequiv"), "-ref"] + ref + ["-impl"] + [os.path.join(copy, f) for f in files]
                                          + ["-top", a.top], a.timeout)
            v["equivalent"] = {0: True, 1: False}.get(rc)       # None: could not decide
            if rc == 1:
                v["equiv_fails"] = [l.strip() for l in (out + err).splitlines() if l.strip().startswith("FAIL")][:10]
        v["ok"] = (v["compiles"] and not v["added"] and v["removed"] > 0 and v.get("regression_ok", True) is not False)
        return v

    def edit(x):
        rel = x["file"]
        if rel not in files:
            raise ValueError(f"file must be one of {files}")
        p = os.path.join(copy, rel)
        with open(p, encoding="utf-8") as fh:
            text = fh.read()
        if text.count(x["old"]) != 1:
            raise ValueError(f"old text occurs {text.count(x['old'])} times in {rel}; give a unique, exact piece")
        new = text.replace(x["old"], x["new"], 1)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(new)
        v = check()
        functional = bool(x.get("functional_change"))
        if v["ok"] and v.get("equivalent") is False and not functional:
            v["ok"] = False
            v["why"] = "the edit changes behaviour (mequiv) but was not marked functional_change"
        if not v["ok"]:
            with open(p, "w", encoding="utf-8") as fh:      # undo
                fh.write(text)
            state["rejected"].append({"file": rel, "reason": x.get("reason"), "verdict": v})
            return {"accepted": False, "verdict": v}
        state["accepted"].append({"file": rel, "reason": x.get("reason"), "functional_change": functional, "verdict": v})
        return {"accepted": True, "verdict": v}

    def finish(x):
        state["done"] = True
        state["waived"] = x.get("waive") or []
        return {"ok": True}

    local = {
        "edit_file": ("Replace one exact piece of text in one of the design files; the edit is checked and undone if "
                      "it adds a finding, breaks compilation, fails the regression, or (unless functional_change) "
                      "changes behaviour.",
                      obj({"file": S, "old": S, "new": S, "reason": S,
                           "functional_change": {"type": "boolean", "description": "true when the finding is a real bug and the fix changes behaviour on purpose"}},
                          ["file", "old", "new", "reason"]), edit),
        "finish": ("Stop. List findings that should be waived instead of fixed, with why.",
                   obj({"waive": {"type": "array", "items": obj({"rule": S, "signal": S, "why": S})}}), finish)}
    sess = Session(csrv, backend, ["read_file", "mlint_check", "mdebug_query"], local, a.max_steps, log)
    findings_txt = json.dumps([{k: f.get(k) for k in ("file", "line", "rule", "severity", "text", "signal")} for f in base_findings[:150]], indent=1)
    t0 = time.time()
    sess.run(LINTFIX_SYSTEM, f"Design files: {files}, top {a.top}.\nFindings to fix:\n{findings_txt}")
    final = check()
    patch = []
    for f in files:
        new = _read(os.path.join(copy, f))
        if new != originals[f]:
            patch += difflib.unified_diff(originals[f].splitlines(True), new.splitlines(True), "a/" + f, "b/" + f)
    patch_path = os.path.join(work, "lintfix.patch")
    with open(patch_path, "w", encoding="utf-8") as fh:
        fh.writelines(patch)
    applied = []
    if a.apply and patch:
        for f in files:
            src = os.path.join(copy, f)
            if _read(src) != originals[f]:
                shutil.copyfile(src, srv.path(f))
                applied.append(f)
    metrics = {"findings_before": len(base_keys), "removed": final["removed"], "added": len(final["added"]),
               "remaining": final["remaining"], "compiles": final["compiles"],
               "regression_ok": final.get("regression_ok"), "equivalent": final.get("equivalent"),
               "edits_accepted": len(state["accepted"]), "edits_rejected": len(state["rejected"]),
               "functional_changes": sum(1 for e in state["accepted"] if e["functional_change"]),
               "seconds": round(time.time() - t0, 1), "model_calls": backend.calls, "tool_calls": sess.tool_calls}
    return {"agent": "lintfix", "metrics": metrics, "patch": srv.rel(patch_path), "applied": applied,
            "accepted": state["accepted"], "rejected": state["rejected"], "waive": state["waived"]}


def _read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def _copy_tree(src, dst):
    skip = {".git", ".mmcp", "build", "node_modules", "__pycache__"}
    shutil.copytree(src, dst, ignore=lambda d, names: [n for n in names if n in skip or n.startswith("build")
                                                       or n.endswith((".vcd", ".fst", ".fsdb"))],
                    dirs_exist_ok=True)


# -- coverage closure -----------------------------------------------------------
COVCLOSE_SYSTEM = """You close code coverage on a hardware design by writing new directed tests. You get the
design, an existing testbench, and what is not covered yet (mcov_report with uncovered=true). Write a
new, complete, self-checking SystemVerilog testbench module with add_test: it must instantiate the
design the same way the existing testbench does, drive the inputs that reach the uncovered lines,
branches, conditions and FSM transitions, check what it can, print TEST PASSED, and call $finish.
Each test is simulated with coverage; it is kept only when it passes and adds coverage, and you get
the new totals back. Aim for the uncovered items, not for repeating what is covered. Call finish when
the goal is met or nothing more can be reached (say what is unreachable and why)."""


def covclose(srv, backend, a, log):
    files = [srv.rel(srv.path(f)) for f in a.files]
    tb = srv.rel(srv.path(a.tb))
    work = srv.workdir("covclose", a.out)
    goal = {}
    for g in (a.goal or "line=100,branch=100").split(","):
        k, _, v = g.partition("=")
        goal[k.strip()] = float(v)
    base = mmcp.result_of(srv, "msim_run", {"files": files + [tb], "top": a.tb_top, "coverage": True,
                                            "args": a.sim_args, "out": os.path.relpath(os.path.join(work, "base"), srv.root)})["structuredContent"]
    if not base.get("passed") or not base.get("coverage_db"):
        return {"agent": "covclose", "error": "the existing testbench does not pass with coverage", "detail": base}
    dbs = [base["coverage_db"]]
    excl = a.exclude or _tb_exclude(tb)

    def cov():
        return mmcp.result_of(srv, "mcov_report", {"dbs": dbs, "exclude": excl})["structuredContent"]["totals"]

    start = cov()
    state = {"kept": [], "dropped": [], "done": False, "unreachable": None, "n": 0, "totals": start}

    def met(t):
        return all(t.get(k, {}).get("pct", 0) >= v for k, v in goal.items())

    def add_test(x):
        state["n"] += 1
        if state["n"] > a.rounds:
            return {"error": f"the limit of {a.rounds} tests is reached; call finish"}
        name = re.sub(r"\W", "_", x.get("name") or f"cov_test_{state['n']}")
        path = os.path.join(work, "tests", name + ".sv")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(x["source"])
        top = x.get("top") or _first_module(x["source"])
        r = mmcp.result_of(srv, "msim_run", {"files": files + [srv.rel(path)], "top": top, "coverage": True,
                                             "args": a.sim_args, "seed": x.get("seed"),
                                             "out": os.path.relpath(os.path.join(work, "runs", name), srv.root)})["structuredContent"]
        if not r.get("passed"):
            os.remove(path)
            state["dropped"].append({"name": name, "why": r.get("status"), "errors": r.get("errors")})
            return {"kept": False, "status": r.get("status"), "errors": r.get("errors"), "log_tail": r.get("log_tail")}
        before = state["totals"]
        dbs.append(r["coverage_db"])
        after = cov()
        gain = {k: round(after[k]["pct"] - before[k]["pct"], 2) for k in SCORED if k in after}
        if not any(after[k].get("hit", 0) > before[k].get("hit", 0) for k in SCORED if k in after):
            dbs.pop()
            os.remove(path)
            state["dropped"].append({"name": name, "why": "no new coverage"})
            return {"kept": False, "why": "passed but covered nothing new", "totals": _pcts(before)}
        state["totals"] = after
        state["kept"].append({"name": name, "file": srv.rel(path), "top": top, "gain": gain})
        return {"kept": True, "gain": gain, "totals": _pcts(after), "goal_met": met(after)}

    def finish(x):
        state["done"] = True
        state["unreachable"] = x.get("unreachable")
        return {"ok": True}

    def uncovered(_x):
        return mmcp.result_of(srv, "mcov_report", {"dbs": dbs, "exclude": excl, "uncovered": True, "max_lines": 300})["structuredContent"]

    local = {
        "add_test": ("Write a new testbench (a whole module) and simulate it with coverage; kept only if it passes and "
                     "adds coverage.", obj({"name": S, "source": S, "top": {"type": "string", "description": "its module name"},
                                            "seed": {"type": "integer"}}, ["source"]), add_test),
        "uncovered_now": ("What is still not covered, with everything kept so far merged in.", obj({}), uncovered),
        "finish": ("Stop. Say which uncovered items cannot be reached and why.",
                   obj({"unreachable": {"type": "array", "items": obj({"item": S, "why": S})}}), finish)}
    sess = Session(srv, backend, ["read_file", "msim_run"], local, a.max_steps, log)
    t0 = time.time()
    if not met(start):
        text = (f"Design files: {files}. Existing testbench: {tb} (top {a.tb_top}).\nGoal: {goal}.\n"
                f"Coverage now: {json.dumps(_pcts(start))}\nNot covered:\n" + json.dumps(uncovered({}), indent=1)[:40000])
        sess.run(COVCLOSE_SYSTEM, text)
    end = state["totals"]
    merged = os.path.join(work, "merged_cov.json")
    mc = mmcp._mcov_module(srv)
    mc.save(mc.load_all([srv.path(d) for d in dbs]), merged)
    metrics = {"before": _pcts(start), "after": _pcts(end), "goal": goal, "goal_met": met(end),
               "tests_kept": len(state["kept"]), "tests_dropped": len(state["dropped"]),
               "seconds": round(time.time() - t0, 1), "model_calls": backend.calls, "tool_calls": sess.tool_calls}
    return {"agent": "covclose", "metrics": metrics, "kept": state["kept"], "dropped": state["dropped"],
            "unreachable": state["unreachable"], "coverage_db": srv.rel(merged)}


# Metrics counted by source file, so a new testbench top does not count as new
# coverage. Toggles are named by hierarchy (tb_new.u.q differs from tb.u.q) and
# are left out of covclose's scoring.
SCORED = ("line", "branch", "condition", "fsm_state", "fsm_transition", "functional", "assertion")


def _pcts(t):
    return {k: v["pct"] for k, v in t.items() if k in SCORED and isinstance(v, dict) and "pct" in v and v.get("total", 1)}


def _first_module(src):
    m = re.search(r"^\s*module\s+(\w+)", src, re.M)
    if not m:
        raise ValueError("no module in the test source")
    return m.group(1)


def _tb_exclude(tb):
    return re.escape(os.path.basename(tb)) + "$|/tests/[^/]+\\.sv$"


# -- main -----------------------------------------------------------------------
def main(argv=None, backend=None):
    ap = argparse.ArgumentParser(prog="magent", description="verification agents judged by the minisim engines")
    ap.add_argument("-version", action="version", version=f"magent {VERSION}")
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-root", default=".")
    common.add_argument("-bin", action="append", default=[])
    common.add_argument("-plugin", action="append", default=[])
    common.add_argument("-model", default=DEFAULT_MODEL)
    common.add_argument("-api", choices=["anthropic", "openai"], default="anthropic")
    common.add_argument("-base-url", dest="base_url")
    common.add_argument("-effort", default="high", choices=["low", "medium", "high", "xhigh", "max"])
    common.add_argument("-max-steps", dest="max_steps", type=int, default=40, help="model turns at most")
    common.add_argument("-timeout", type=float, default=600)
    common.add_argument("-o", dest="report", help="write the JSON report here (default: print it)")
    common.add_argument("-out", help="work directory under the root")
    common.add_argument("-log", help="append each model turn as a JSON line")
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("triage", parents=[common], help="explain each failure group of a regression")
    t.add_argument("-results", required=True, help="mregress output directory")
    t.add_argument("-max-groups", dest="max_groups", type=int, default=10)
    t.add_argument("-labels", help="JSON of known causes, to score the run")
    l = sub.add_parser("lintfix", parents=[common], help="fix lint findings, checked by the engines")
    l.add_argument("files", nargs="+")
    l.add_argument("-top", required=True)
    l.add_argument("-only", help="rules or groups to fix (mlint -only)")
    l.add_argument("-lint-args", dest="lint_args", nargs="*", default=[])
    l.add_argument("-regress", help="a test list that must still pass after every edit")
    l.add_argument("-no-equiv", dest="no_equiv", action="store_true", help="skip the mequiv check")
    l.add_argument("-apply", action="store_true", help="copy the accepted edits back to the design files")
    c = sub.add_parser("covclose", parents=[common], help="write tests until coverage reaches a goal")
    c.add_argument("files", nargs="+", help="design files")
    c.add_argument("-tb", required=True, help="the existing testbench")
    c.add_argument("-tb-top", dest="tb_top", default="tb")
    c.add_argument("-goal", help="metric=percent,... (default line=100,branch=100)")
    c.add_argument("-rounds", type=int, default=6, help="tests the model may write")
    c.add_argument("-exclude", help="regex of files left out of the score (default: the testbenches)")
    c.add_argument("-sim-args", dest="sim_args", nargs="*", default=[])
    a = ap.parse_args(argv)
    srv = mmcp.Server(a.root, a.bin, a.plugin, a.timeout)
    backend = backend or backend_from(a)
    logf = open(a.log, "a", encoding="utf-8") if a.log else None
    log = (lambda rec: (logf.write(json.dumps(rec, default=str) + "\n"), logf.flush())) if logf else None
    try:
        r = {"triage": triage, "lintfix": lintfix, "covclose": covclose}[a.cmd](srv, backend, a, log)
    except mmcp.ToolError as e:
        print(f"magent: {e}", file=sys.stderr)
        return 2
    r["model"] = getattr(backend, "model", type(backend).__name__)
    text = json.dumps(r, indent=1, default=str)
    if a.report:
        with open(a.report, "w", encoding="utf-8") as f:
            f.write(text + "\n")
        print(json.dumps(r.get("metrics", r), default=str))
    else:
        print(text)
    return 1 if r.get("error") else 0


if __name__ == "__main__":
    sys.exit(main())
