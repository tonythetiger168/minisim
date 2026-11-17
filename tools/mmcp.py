#!/usr/bin/env python3
"""mmcp: the minisim engines as Model Context Protocol (MCP) tools.

Any agent that speaks MCP (Claude Code, Claude Desktop, Cursor, an agent on
DeepSeek, Qwen or a local model through an MCP client) can drive msim, mlint,
mcov, mdebug, mregress and mplan through this server. Every tool answers with
one JSON object, so the agent reads results instead of scraping logs.

    mmcp                               # serve MCP on stdin / stdout (the usual way)
    mmcp -root ~/proj                  # agents may only touch files under ~/proj
    mmcp -list                         # the tools this installation offers, as JSON
    mmcp -call msim_run '{"files": ["tb.sv"], "top": "tb"}'    # one call, no agent

Claude Code:  claude mcp add minisim -- mmcp -root .
Other clients: command "mmcp", arguments ["-root", "<project>"].

Files. Every path an agent passes is relative to the root (-root, default the
current directory) and must stay inside it; a path that leaves it is refused.
Outputs a tool writes go under <root>/.mmcp/<tool>-<n>/ unless the call names
them. Commands run without a shell, one argument per list item.

Plug-ins. Python files in <prefix>/share/minisim/mcp/, in the directories of
$MINISIM_MCP_PLUGINS (separated by the path separator) and in -plugin DIR are
loaded at start. Each defines register(server) and adds tools with
server.tool(name, description, schema, handler); handler(server, args) returns
a dict. Add-on tool packages ship their MCP tools this way. Such a tool runs
its own program, which checks its own licence: when the licence is missing
the call returns isError with the program's message, and nothing else breaks.

Protocol: MCP over stdio, JSON-RPC 2.0, one message per line (versions
2025-06-18, 2025-03-26 and 2024-11-05). Only the Python standard library.
"""
import argparse
import glob
import importlib.machinery
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

VERSION = "0.1.0"
PROTOCOLS = ["2025-06-18", "2025-03-26", "2024-11-05"]
HERE = os.path.dirname(os.path.realpath(__file__))
LOG_TAIL = 60          # lines of a log returned with a result
MAX_TEXT = 200_000     # characters of any one text field

# Lines that mean a failure even when the simulator exits 0 (as mregress).
ERROR_RX = re.compile(r"^(?:UVM_(?:ERROR|FATAL)\s*:\s*[1-9]|UVM_FATAL\b|Error-\[|\*E,|\*\* (?:Error|Fatal)|(?:Error|Fatal):|.*\$fatal\b|msim: .*(?:error|cannot)|.*:\d+:\d+: error:)")
LOC_RX = re.compile(r"(?P<file>[^\s:]+\.(?:sv|svh|v|vh|vhd|vhdl)):(?P<line>\d+)(?::(?P<col>\d+))?")


class ToolError(Exception):
    """A call that cannot run: a bad argument, a missing program or file."""


class Server:
    def __init__(self, root, bins=(), plugins=(), timeout=600):
        self.root = os.path.realpath(root)
        self.timeout = timeout
        self.tools = {}
        self.order = []
        self.lock = threading.Lock()
        self.counter = 0
        prefix = os.path.dirname(HERE)          # <prefix>/bin/mmcp -> <prefix>
        self.bins = [os.path.realpath(b) for b in bins] + [HERE]
        home = os.environ.get("MINISIM_HOME")
        if home:
            self.bins.append(os.path.join(home, "bin"))
        dirs = list(plugins) + [d for d in os.environ.get("MINISIM_MCP_PLUGINS", "").split(os.pathsep) if d]
        dirs.append(os.path.join(prefix, "share", "minisim", "mcp"))
        if home:
            dirs.append(os.path.join(home, "share", "minisim", "mcp"))
        self.plugin_errors = []
        self.plugins = []
        _register_builtin(self)
        seen = set()
        for d in dirs:
            d = os.path.realpath(d)
            if d in seen or not os.path.isdir(d):
                continue
            seen.add(d)
            for f in sorted(glob.glob(os.path.join(d, "*.py"))):
                self._load_plugin(f)

    # -- registry -----------------------------------------------------------
    def tool(self, name, description, schema, handler, needs=()):
        """Add a tool. needs: programs it runs; the tool is listed only when all are found."""
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", name):
            raise ValueError(f"bad tool name {name!r}")
        if name not in self.tools:
            self.order.append(name)
        self.tools[name] = {"name": name, "description": description, "inputSchema": schema,
                            "handler": handler, "needs": list(needs)}

    def available(self, name):
        t = self.tools[name]
        return all(self.find(p) for p in t["needs"])

    def listed(self):
        return [{"name": n, "description": self.tools[n]["description"], "inputSchema": self.tools[n]["inputSchema"]}
                for n in self.order if self.available(n)]

    def _load_plugin(self, path):
        try:
            name = "mmcp_plugin_" + re.sub(r"\W", "_", os.path.splitext(os.path.basename(path))[0])
            spec = importlib.util.spec_from_file_location(name, path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            mod.register(self)
            self.plugins.append(path)
        except Exception as e:  # a broken plug-in must not take the free tools down
            self.plugin_errors.append(f"{path}: {type(e).__name__}: {e}")

    # -- helpers for tools and plug-ins --------------------------------------
    def find(self, prog):
        """The path of a minisim program (bin dir, MINISIM_HOME, then PATH), or None."""
        for d in self.bins:
            for cand in (prog, prog + ".py"):
                p = os.path.join(d, cand)
                if os.path.isfile(p) and os.access(p, os.R_OK) and (cand.endswith(".py") or os.access(p, os.X_OK)):
                    return p
        return shutil.which(prog)

    def need(self, prog):
        p = self.find(prog)
        if not p:
            raise ToolError(f"{prog} is not installed (looked in {', '.join(self.bins)} and PATH)")
        return p

    def path(self, p, must_exist=True):
        """An agent's path, resolved under the root; refuses one that leaves it."""
        if not isinstance(p, str) or not p:
            raise ToolError(f"bad path {p!r}")
        full = os.path.realpath(os.path.join(self.root, os.path.expanduser(p)))
        if full != self.root and not full.startswith(self.root + os.sep):
            raise ToolError(f"path {p!r} is outside the root {self.root}")
        if must_exist and not os.path.exists(full):
            raise ToolError(f"no such file: {p}")
        return full

    def rel(self, p):
        return os.path.relpath(p, self.root) if p else p

    def shown(self, p):
        """A path as an agent should see it: relative when it is under the root."""
        if not p or not os.path.isabs(p):
            return p
        r = os.path.realpath(p)
        return os.path.relpath(r, self.root) if r == self.root or r.startswith(self.root + os.sep) else p

    def workdir(self, tag, out=None):
        if out:
            d = self.path(out, must_exist=False)
        else:
            with self.lock:
                self.counter += 1
                n = self.counter
            base = os.path.join(self.root, ".mmcp")
            d = os.path.join(base, f"{tag}-{os.getpid()}-{n}")
        os.makedirs(d, exist_ok=True)
        return d

    def run(self, argv, timeout=None, cwd=None, stdin=None):
        """Run a program; (exit status, stdout, stderr, timed out, seconds)."""
        t0 = time.time()
        timeout = timeout or self.timeout
        argv = launch(argv)
        try:
            p = subprocess.run(argv, cwd=cwd or self.root, input=stdin, capture_output=True, text=True,
                               timeout=timeout, errors="replace")
            return p.returncode, p.stdout, p.stderr, False, time.time() - t0
        except subprocess.TimeoutExpired as e:
            out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            err = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
            return -1, out, err, True, time.time() - t0
        except OSError as e:
            raise ToolError(f"cannot run {argv[0]}: {e}")

    def args_list(self, a, key):
        v = a.get(key, [])
        if isinstance(v, str):
            v = [v]
        if not isinstance(v, list) or not all(isinstance(x, (str, int, float)) for x in v):
            raise ToolError(f"{key} must be a list of strings")
        return [str(x) for x in v]

    def call(self, name, args):
        if name not in self.tools:
            raise ToolError(f"no tool {name!r}")
        if not isinstance(args, dict):
            raise ToolError("arguments must be an object")
        t = self.tools[name]
        for k in t["inputSchema"].get("required", []):
            if k not in args:
                raise ToolError(f"{name}: missing argument {k!r}")
        return t["handler"](self, args)


def launch(argv):
    """A tool from the source tree (tools/x.py) runs under this Python."""
    return [sys.executable] + list(argv) if argv and argv[0].endswith(".py") else list(argv)


# -- results ------------------------------------------------------------------
def tail(text, n=LOG_TAIL):
    lines = text.splitlines()
    return "\n".join(lines[-n:])


def clip(text):
    return text if len(text) <= MAX_TEXT else text[:MAX_TEXT] + f"\n... ({len(text) - MAX_TEXT} more characters)"


def error_lines(text, limit=50):
    out = []
    for line in text.splitlines():
        if ERROR_RX.match(line):
            e = {"text": line.strip()[:500]}
            m = LOC_RX.search(line)
            if m:
                e["file"], e["line"] = m.group("file"), int(m.group("line"))
            out.append(e)
            if len(out) >= limit:
                break
    return out


def write_log(d, name, out, err):
    p = os.path.join(d, name)
    with open(p, "w", encoding="utf-8") as f:
        f.write(out)
        if err:
            f.write(("\n" if out and not out.endswith("\n") else "") + err)
    return p


# -- the free tools -----------------------------------------------------------
S = {"type": "string"}
SL = {"type": "array", "items": {"type": "string"}}
FILES = {"type": "array", "items": {"type": "string"}, "description": "source files (.sv, .v, .vhd), relative to the root"}
TOP = {"type": "string", "description": "top module"}
EXTRA = {"type": "array", "items": {"type": "string"}, "description": "more command-line arguments, one per item (+define+X, -f list.f, +incdir+dir, ...)"}
TIMEOUT = {"type": "number", "description": "seconds before the run is stopped (default 600)"}


def sources(srv, a):
    files = [srv.path(f) for f in srv.args_list(a, "files")]
    argv = files
    if a.get("top"):
        argv += ["-top", str(a["top"])]
    return argv


def t_info(srv, a):
    progs = ["msim", "mlint", "mdebug", "mcov", "mregress", "mplan"]
    found = {p: srv.rel(srv.find(p)) if srv.find(p) and srv.find(p).startswith(srv.root) else srv.find(p) for p in progs}
    ver = None
    if found["msim"]:
        rc, out, err, _, _ = srv.run([srv.find("msim"), "-version"], timeout=30)
        ver = (out or err).strip().splitlines()[0] if (out or err).strip() else None
    return {"server": f"mmcp {VERSION}", "root": srv.root, "msim_version": ver, "programs": found,
            "tools": [t["name"] for t in srv.listed()], "plugins": srv.plugins, "plugin_errors": srv.plugin_errors}


def t_msim(srv, a):
    msim = srv.need("msim")
    d = srv.workdir("msim", a.get("out"))
    res = os.path.join(d, "result.json")
    argv = [msim] + sources(srv, a) + ["-result", res]
    if a.get("uvm"):
        argv += ["-uvm"]
    if a.get("seed") is not None:
        argv += ["-seed", str(int(a["seed"]))]
    if a.get("maxtime"):
        argv += ["-maxtime", str(a["maxtime"])]
    cov = wave = None
    if a.get("coverage"):
        cov = os.path.join(d, "cov.json")
        argv += ["-cov", cov]
    if a.get("wave"):
        kind = str(a["wave"]).lower()
        if kind not in ("vcd", "fst"):
            raise ToolError("wave must be vcd or fst")
        wave = os.path.join(d, "dump." + kind)
        argv += ["-vcd", wave]
    for k, v in (a.get("defines") or {}).items():
        argv.append(f"+define+{k}" + ("" if v in (None, "", True) else f"={v}"))
    argv += srv.args_list(a, "args") + ["+" + p.lstrip("+") for p in srv.args_list(a, "plusargs")]
    rc, out, err, timed_out, secs = srv.run(argv, a.get("timeout"))
    log = write_log(d, "sim.log", out, err)
    try:
        with open(res, encoding="utf-8") as f:
            verdict = json.load(f)
    except (OSError, ValueError):
        verdict = None
    errs = error_lines(out + "\n" + err)
    status = "timeout" if timed_out else (verdict or {}).get("status", "pass" if rc == 0 else "error")
    if status == "pass" and errs:
        status = "error"
    r = {"status": status, "passed": status == "pass", "exit_status": rc, "seconds": round(secs, 3),
         "errors": errs, "verdict": verdict, "log": srv.rel(log), "log_tail": clip(tail(out + err))}
    if cov:
        r["coverage_db"] = srv.rel(cov) if os.path.exists(cov) else None
    if wave:
        r["wave"] = srv.rel(wave) if os.path.exists(wave) else None
    r["command"] = [srv.shown(x) for x in argv]
    return r


def t_mlint(srv, a):
    mlint = srv.need("mlint")
    d = srv.workdir("mlint", a.get("out"))
    js = os.path.join(d, "lint.json")
    argv = [mlint] + sources(srv, a) + ["-json", js]
    if a.get("only"):
        argv += ["-only", str(a["only"])]
    for w in srv.args_list(a, "waivers"):
        argv += ["-waivers", srv.path(w)]
    argv += srv.args_list(a, "args")
    rc, out, err, timed_out, secs = srv.run(argv, a.get("timeout"))
    try:
        with open(js, encoding="utf-8") as f:
            rep = json.load(f)
    except (OSError, ValueError):
        return {"ok": False, "exit_status": rc, "timed_out": timed_out, "errors": error_lines(out + err),
                "log_tail": clip(tail(out + err))}
    findings = rep.get("findings", [])
    for f in findings:
        if f.get("file"):
            f["file"] = srv.shown(f["file"])
    if not a.get("include_waived"):
        findings = [f for f in findings if not f.get("waived")]
    sev = a.get("severity")
    if sev:
        keep = {"error": {"error"}, "warning": {"error", "warning"}}.get(sev, {"error", "warning", "info"})
        findings = [f for f in findings if f.get("severity") in keep]
    limit = int(a.get("max_findings", 200))
    by_rule = {}
    for f in findings:
        by_rule[f.get("rule")] = by_rule.get(f.get("rule"), 0) + 1
    return {"ok": True, "exit_status": rc, "seconds": round(secs, 3), "summary": rep.get("summary"),
            "by_rule": dict(sorted(by_rule.items(), key=lambda kv: -kv[1])), "count": len(findings),
            "findings": findings[:limit], "truncated": len(findings) > limit, "report": srv.rel(js)}


def _mcov_module(srv):
    p = srv.need("mcov")
    loader = importlib.machinery.SourceFileLoader("mmcp_mcov", p)
    spec = importlib.util.spec_from_loader("mmcp_mcov", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def t_mcov(srv, a):
    mc = _mcov_module(srv)
    dbs = [srv.path(p) for p in srv.args_list(a, "dbs")]
    if not dbs:
        raise ToolError("dbs: give at least one coverage database (msim -cov)")
    db = mc.load_all(dbs) if len(dbs) > 1 else mc.load(dbs[0])
    rules = []
    for e in srv.args_list(a, "elfile"):
        rules += mc.read_elfile(srv.path(e))
    if rules:
        mc.apply_exclusions(db, rules)
    ex = a.get("exclude")
    rf = bool(a.get("fsm_reset_filter"))
    per, th, ta = mc.summarize(db, ex)
    lh = sum(s[0] for s in per.values()); lt = sum(s[1] for s in per.values())
    bh = sum(s[2] for s in per.values()); bt = sum(s[3] for s in per.values())
    ch, ct, cn = mc.cond_summary(db, ex)
    (sh, st, trh, trt), fn = mc.fsm_summary(db, ex, rf)
    cgs, func, (aok, an, afail, aidle) = mc.summarize_func(db, ex)
    r2 = lambda x: round(x, 2)
    totals = {"line": {"pct": r2(mc.pct(lh, lt)), "hit": lh, "total": lt},
              "branch": {"pct": r2(mc.pct(bh, bt)), "hit": bh, "total": bt},
              "toggle": {"pct": r2(mc.pct(th, ta)), "hit": th, "total": ta},
              "condition": {"pct": r2(mc.pct(ch, ct)), "hit": ch, "total": ct, "conditions": cn},
              "fsm_state": {"pct": r2(mc.pct(sh, st)), "hit": sh, "total": st, "fsms": fn},
              "fsm_transition": {"pct": r2(mc.pct(trh, trt)), "hit": trh, "total": trt},
              "functional": {"pct": r2(func), "covergroups": len(cgs)},
              "assertion": {"pct": r2(mc.pct(aok, an)), "covered": aok, "total": an, "failed": afail, "never_passed": aidle}}
    files = [{"file": srv.shown(f), "line_pct": r2(mc.pct(s[0], s[1])), "lines": [s[0], s[1]],
              "branch_pct": r2(mc.pct(s[2], s[3])), "branches": [s[2], s[3]]} for f, s in sorted(per.items())]
    r = {"totals": totals, "files": files,
         "covergroups": [{"name": n, "pct": r2(p), "bins": [c, t]} for n, (p, c, t) in sorted(cgs.items())]}
    gates, failed = {}, []
    for k, v in (a.get("min") or {}).items():
        key = {"func": "functional", "cond": "condition", "fsm": "fsm_state", "assert": "assertion"}.get(k, k)
        if key not in totals:
            raise ToolError(f"min: unknown metric {k!r}")
        gates[key] = float(v)
        if totals[key]["pct"] < float(v):
            failed.append(key)
    if gates:
        r["gate"] = {"goals": gates, "failed": failed, "passed": not failed}
    if a.get("uncovered"):
        text = mc.detail_text(db, ex, rf, True)
        if not isinstance(text, str):
            text = "\n".join(text)
        lines = text.splitlines()
        limit = int(a.get("max_lines", 400))
        r["uncovered"] = lines[:limit]
        r["uncovered_truncated"] = len(lines) > limit
        # uncovered source lines per file, the first thing a closure agent needs
        miss, arms = {}, []
        for f, (hits, br) in mc.by_file(db).items():
            if ex and re.search(ex, f):
                continue
            m = sorted(int(l) for l, h in hits.items() if h == 0)
            if m:
                miss[srv.shown(f)] = m[:200]
            for ln, outcomes in sorted(br.items(), key=lambda kv: int(kv[0])):
                for n, t, fl in outcomes:
                    never = [w for w, h in (("true", t), ("false", fl)) if h == 0]
                    if never:
                        arms.append({"file": srv.shown(f), "line": int(ln), "decision": n, "never": never})
        r["uncovered_lines"] = miss
        # decisions (if / case arm / loop test) with an outcome never taken
        r["uncovered_branches"] = arms[:limit]
    return r


def t_mdebug(srv, a):
    mdebug = srv.need("mdebug")
    cmds = srv.args_list(a, "commands")
    if not cmds:
        raise ToolError("commands: give at least one mdebug -script command (help lists them)")
    argv = [mdebug] + sources(srv, a)
    if a.get("wave"):
        argv += ["-vcd", srv.path(a["wave"])]
    if a.get("cov"):
        argv += ["-cov", srv.path(a["cov"])]
    if a.get("lint"):
        argv += ["-lint", srv.path(a["lint"])]
    argv += srv.args_list(a, "args") + ["-script", "-"]
    text = "\n".join(c.replace("\n", " ") for c in cmds) + "\nquit\n"
    rc, out, err, timed_out, secs = srv.run(argv, a.get("timeout"), stdin=text)
    answers = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                answers.append(json.loads(line))
            except ValueError:
                pass
    r = {"exit_status": rc, "timed_out": timed_out, "seconds": round(secs, 3), "answers": answers}
    if len(answers) < len(cmds):
        r["errors"] = error_lines(err) or [{"text": l} for l in err.strip().splitlines()[-10:]]
    return r


def t_mregress(srv, a):
    mreg = srv.need("mregress")
    d = srv.workdir("mregress", a.get("out"))
    argv = [mreg] + [srv.path(p) for p in srv.args_list(a, "lists")]
    if a.get("spec"):
        argv += ["-spec", srv.path(a["spec"])]
    if len(argv) == 1:
        raise ToolError("give lists (test list files) or spec (a directory of spec tests)")
    sim = a.get("sim", "msim")
    argv += ["-sim", str(sim), "-o", d, "-q"]
    if sim == "msim" and srv.find("msim"):
        argv += ["-exe", srv.find("msim")]
    if a.get("filter"):
        argv += ["-filter", str(a["filter"])]
    for k, flag in (("jobs", "-j"), ("seeds", "-seeds")):
        if a.get(k):
            argv += [flag, str(int(a[k]))]
    if a.get("coverage"):
        argv += ["-cov"]
    if a.get("rerun"):
        argv += ["-rerun"]
    argv += srv.args_list(a, "args")
    env_path = os.pathsep.join([os.path.dirname(srv.find("msim"))] if srv.find("msim") else [])
    rc, out, err, timed_out, secs = _run_env(srv, argv, a.get("timeout") or 3600, env_path)
    try:
        with open(os.path.join(d, "results.json"), encoding="utf-8") as f:
            res = json.load(f)
    except (OSError, ValueError):
        return {"ok": False, "exit_status": rc, "timed_out": timed_out, "log_tail": clip(tail(out + err))}
    groups = {}
    for run in res["runs"]:
        if run["status"] in ("fail", "xpass"):
            g = groups.setdefault(run.get("signature") or run["reason"], {"reason": run["reason"], "runs": []})
            g["runs"].append({"test": run["test"], "seed": run["seed"],
                              "log": srv.rel(os.path.join(d, run["log"])) if run.get("log") else None})
    limit = int(a.get("max_groups", 50))
    fail_groups = sorted(groups.values(), key=lambda g: -len(g["runs"]))
    return {"ok": True, "exit_status": rc, "passed": res["passed"], "failed": res["failed"], "skipped": res["skipped"],
            "xfail": res.get("xfail", 0), "seconds": res["seconds"], "out": srv.rel(d),
            "results": srv.rel(os.path.join(d, "results.json")),
            "failure_groups": fail_groups[:limit], "groups_truncated": len(fail_groups) > limit}


def _run_env(srv, argv, timeout, extra_path):
    env = dict(os.environ)
    if extra_path:
        env["PATH"] = extra_path + os.pathsep + env.get("PATH", "")
    t0 = time.time()
    try:
        p = subprocess.run(launch(argv), cwd=srv.root, capture_output=True, text=True, timeout=timeout, env=env, errors="replace")
        return p.returncode, p.stdout, p.stderr, False, time.time() - t0
    except subprocess.TimeoutExpired as e:
        return -1, str(e.stdout or ""), str(e.stderr or ""), True, time.time() - t0


def t_mplan(srv, a):
    mplan = srv.need("mplan")
    mode = a.get("mode", "report")
    if mode not in ("report", "check", "triage"):
        raise ToolError("mode must be report, check or triage")
    d = srv.workdir("mplan", a.get("out"))
    argv = [mplan, mode]
    if mode in ("report", "check"):
        if not a.get("plan"):
            raise ToolError("plan: the .vplan file")
        argv.append(srv.path(a["plan"]))
    elif a.get("plan"):
        argv += ["-plan", srv.path(a["plan"])]
    if mode in ("report", "triage"):
        if not a.get("results"):
            raise ToolError("results: the mregress output directory")
        argv += ["-results", srv.path(a["results"])]
    if mode == "report":
        for c in srv.args_list(a, "cov"):
            argv += ["-cov", srv.path(c)]
        argv += ["-o", d]
    tri = os.path.join(d, "triage.json")
    if mode == "triage":
        argv += ["-json", tri]
    argv += srv.args_list(a, "args")
    rc, out, err, timed_out, secs = srv.run(argv, a.get("timeout"))
    r = {"exit_status": rc, "seconds": round(secs, 3)}
    if mode == "report":
        try:
            with open(os.path.join(d, "plan.json"), encoding="utf-8") as f:
                pj = json.load(f)
        except (OSError, ValueError):
            pj = None
        if pj:
            def slim(feat, depth):
                x = {"id": feat["id"], "title": feat["title"], "score": feat["score"], "status": feat["status"],
                     "metrics": [{k: m[k] for k in ("kind", "pattern", "goal", "score", "hit", "total", "met")}
                                 for m in feat["metrics"] if not m["met"]] if depth else []}
                if feat["features"]:
                    x["features"] = [slim(c, depth + 1) for c in feat["features"]]
                return x
            r.update({"plan": slim(pj["plan"], 0), "triage": pj.get("triage"), "unmapped": pj.get("unmapped"),
                      "plan_json": srv.rel(os.path.join(d, "plan.json")), "plan_html": srv.rel(os.path.join(d, "plan.html"))})
    elif mode == "triage":
        try:
            with open(tri, encoding="utf-8") as f:
                r["buckets"] = json.load(f)
        except (OSError, ValueError):
            pass
    r["text"] = clip(tail(out + err, 200))
    return r


def t_read(srv, a):
    p = srv.path(a["path"])
    if os.path.isdir(p):
        names = sorted(os.listdir(p))
        return {"path": srv.rel(p), "dir": True, "entries": names[:1000], "truncated": len(names) > 1000}
    start = max(1, int(a.get("start", 1)))
    count = min(int(a.get("count", 400)), 5000)
    with open(p, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    if a.get("grep"):
        rx = re.compile(a["grep"])
        hits = [(i + 1, l.rstrip("\n")) for i, l in enumerate(lines) if rx.search(l)]
        return {"path": srv.rel(p), "lines": len(lines), "matches": [{"line": n, "text": t[:500]} for n, t in hits[:count]],
                "truncated": len(hits) > count}
    sel = lines[start - 1:start - 1 + count]
    return {"path": srv.rel(p), "lines": len(lines), "start": start, "text": clip("".join(sel)),
            "more": start - 1 + count < len(lines)}


def _register_builtin(srv):
    srv.tool("minisim_info",
             "Which minisim programs and MCP tools this server has, the msim version and the root directory. Call first.",
             {"type": "object", "properties": {}}, t_info)
    srv.tool("msim_run",
             "Compile and simulate SystemVerilog / Verilog / VHDL with msim (IEEE 1800-2023, UVM). Returns the verdict "
             "(pass, compile_error, error, fatal, uvm_error, timeout), the error lines with file and line, UVM counts, the log "
             "tail, and the coverage database / waveform when asked for.",
             {"type": "object", "required": ["files"], "properties": {
                 "files": FILES, "top": TOP, "uvm": {"type": "boolean", "description": "compile the UVM library in"},
                 "seed": {"type": "integer"}, "maxtime": {"type": "string", "description": "stop at this time, e.g. 1ms"},
                 "coverage": {"type": "boolean", "description": "write a code + functional coverage database (for mcov_report)"},
                 "wave": {"type": "string", "enum": ["vcd", "fst"], "description": "dump a waveform (for mdebug_query)"},
                 "defines": {"type": "object", "description": "macro -> value (+define+)"},
                 "plusargs": {"type": "array", "items": S, "description": "run-time plusargs, e.g. UVM_TESTNAME=smoke"},
                 "args": EXTRA, "timeout": TIMEOUT, "out": {"type": "string", "description": "output directory"}}},
             t_msim, needs=["msim"])
    srv.tool("mlint_check",
             "Lint RTL with mlint (style, synthesis, clock/reset, CDC/RDC, FSM, formal rules; SpyGlass-like). Returns every "
             "finding with file, line, rule, severity, message and signal, counted by rule.",
             {"type": "object", "required": ["files"], "properties": {
                 "files": FILES, "top": TOP,
                 "only": {"type": "string", "description": "rules or groups, comma separated (e.g. cdc,rdc or W001)"},
                 "waivers": {"type": "array", "items": S, "description": "waiver / config files"},
                 "severity": {"type": "string", "enum": ["error", "warning", "info"], "description": "the least severity returned"},
                 "include_waived": {"type": "boolean"}, "max_findings": {"type": "integer", "description": "default 200"},
                 "args": EXTRA, "timeout": TIMEOUT, "out": S}},
             t_mlint, needs=["mlint"])
    srv.tool("mcov_report",
             "Coverage from msim coverage databases (merged when several): line, branch, toggle, condition, FSM, functional "
             "(covergroups) and assertion totals, per file, and with uncovered=true the items not covered and the uncovered "
             "source lines. min gates metrics, e.g. {\"line\": 90}.",
             {"type": "object", "required": ["dbs"], "properties": {
                 "dbs": {"type": "array", "items": S, "description": "msim -cov databases"},
                 "exclude": {"type": "string", "description": "regex of files / signals to leave out"},
                 "elfile": {"type": "array", "items": S, "description": "exclusion files"},
                 "uncovered": {"type": "boolean"}, "max_lines": {"type": "integer"},
                 "fsm_reset_filter": {"type": "boolean"},
                 "min": {"type": "object", "description": "metric -> least percent (line, branch, toggle, condition, fsm_state, functional, assertion)"}}},
             t_mcov, needs=["mcov"])
    srv.tool("mdebug_query",
             "Ask mdebug (Verdi-like) questions about a design and its waveform in one process: drivers SIG, loads SIG, value SIG [T], "
             "changes SIG [T0 [T1]], active SIG [T [DEPTH]], trace SIG [DEPTH], blocks INST, fsm [NAME] [T], assert [NAME] [T], "
             "uncovered [INST], power [DOMAIN] [T], scopes, signals [SCOPE], end, help. One JSON answer per command.",
             {"type": "object", "required": ["files", "commands"], "properties": {
                 "files": FILES, "top": TOP, "wave": {"type": "string", "description": "VCD or FST from msim_run"},
                 "cov": {"type": "string", "description": "coverage database"}, "lint": {"type": "string", "description": "mlint JSON report"},
                 "commands": {"type": "array", "items": S}, "args": EXTRA, "timeout": TIMEOUT}},
             t_mdebug, needs=["mdebug"])
    srv.tool("mregress_run",
             "Run a regression with mregress (test lists or a spec-test directory; msim, or VCS / Xcelium / Questa / Icarus). Returns "
             "pass / fail counts and the failures grouped by their first error line, with each run's log.",
             {"type": "object", "properties": {
                 "lists": {"type": "array", "items": S, "description": "test list files"},
                 "spec": {"type": "string", "description": "a directory of self-checking spec tests"},
                 "sim": {"type": "string", "description": "msim (default), vcs, xrun, questa, iverilog"},
                 "jobs": {"type": "integer"}, "seeds": {"type": "integer"}, "coverage": {"type": "boolean"},
                 "filter": {"type": "string", "description": "only tests whose name matches this regex"},
                 "rerun": {"type": "boolean", "description": "rerun only the failures recorded in out"},
                 "max_groups": {"type": "integer"}, "args": EXTRA,
                 "timeout": {"type": "number", "description": "seconds for the whole regression (default 3600)"}, "out": S}},
             t_mregress, needs=["mregress"])
    srv.tool("mplan_report",
             "Verification plan tracking with mplan: mode report scores a .vplan against mregress results and coverage "
             "(features below goal and their unmet metrics), mode check validates a plan, mode triage sorts failures into buckets.",
             {"type": "object", "properties": {
                 "mode": {"type": "string", "enum": ["report", "check", "triage"]},
                 "plan": S, "results": {"type": "string", "description": "mregress output directory"},
                 "cov": {"type": "array", "items": S}, "args": EXTRA, "timeout": TIMEOUT, "out": S}},
             t_mplan, needs=["mplan"])
    srv.tool("read_file",
             "Read part of a text file (a log, a report, a source) under the root, by line range or regex grep; a directory lists its entries.",
             {"type": "object", "required": ["path"], "properties": {
                 "path": S, "start": {"type": "integer"}, "count": {"type": "integer"},
                 "grep": {"type": "string", "description": "regex; return matching lines with numbers"}}},
             t_read)


# -- MCP over stdio -----------------------------------------------------------
def result_of(srv, name, args):
    try:
        out = srv.call(name, args)
        is_err = bool(out.get("error")) if isinstance(out, dict) else False
    except ToolError as e:
        out, is_err = {"error": str(e)}, True
    except Exception as e:  # report, keep serving
        out, is_err = {"error": f"{type(e).__name__}: {e}"}, True
    return {"content": [{"type": "text", "text": json.dumps(out, indent=1)}], "structuredContent": out, "isError": is_err}


def handle(srv, msg):
    """One JSON-RPC message in; the reply, or None for a notification."""
    mid = msg.get("id")
    method = msg.get("method")
    p = msg.get("params") or {}
    if mid is None:
        return None
    ok = lambda r: {"jsonrpc": "2.0", "id": mid, "result": r}
    if method == "initialize":
        want = p.get("protocolVersion")
        return ok({"protocolVersion": want if want in PROTOCOLS else PROTOCOLS[0],
                   "capabilities": {"tools": {"listChanged": False}},
                   "serverInfo": {"name": "minisim", "title": "minisim EDA engines", "version": VERSION},
                   "instructions": "minisim simulation, lint, coverage, debug, regression and planning engines. "
                                   "Paths are relative to the server's root. Call minisim_info first."})
    if method == "ping":
        return ok({})
    if method == "tools/list":
        return ok({"tools": srv.listed()})
    if method == "tools/call":
        name = p.get("name")
        if name not in srv.tools or not srv.available(name):
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": f"unknown tool {name!r}"}}
        return ok(result_of(srv, name, p.get("arguments") or {}))
    if method in ("resources/list", "prompts/list"):
        return ok({method.split("/")[0]: []})
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"no method {method!r}"}}


def serve(srv, inp=sys.stdin, out=sys.stdout):
    wlock = threading.Lock()

    def reply(r):
        if r is not None:
            with wlock:
                out.write(json.dumps(r) + "\n")
                out.flush()

    def work(msg):
        try:
            reply(handle(srv, msg))
        except Exception as e:
            reply({"jsonrpc": "2.0", "id": msg.get("id"), "error": {"code": -32603, "message": str(e)}})

    threads = []
    for line in inp:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            reply({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}})
            continue
        for m in (msg if isinstance(msg, list) else [msg]):
            if not isinstance(m, dict):
                continue
            if m.get("method") == "tools/call":   # long runs must not hold up pings and lists
                t = threading.Thread(target=work, args=(m,), daemon=True)
                t.start()
                threads.append(t)
            else:
                work(m)
    for t in threads:
        t.join()


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mmcp", description="minisim engines as MCP tools (stdio)")
    ap.add_argument("-root", default=".", help="agents may only use files under this directory (default: .)")
    ap.add_argument("-bin", action="append", default=[], help="directory with the minisim programs (may be repeated)")
    ap.add_argument("-plugin", action="append", default=[], help="directory of MCP tool plug-ins (may be repeated)")
    ap.add_argument("-timeout", type=float, default=600, help="default seconds per tool call")
    ap.add_argument("-list", action="store_true", help="print the tools as JSON and exit")
    ap.add_argument("-call", nargs=2, metavar=("TOOL", "JSON"), help="run one tool and print its result")
    ap.add_argument("-version", action="version", version=f"mmcp {VERSION}")
    a = ap.parse_args(argv)
    if not os.path.isdir(a.root):
        ap.error(f"-root {a.root}: no such directory")
    srv = Server(a.root, a.bin, a.plugin, a.timeout)
    for e in srv.plugin_errors:
        print(f"mmcp: plug-in not loaded: {e}", file=sys.stderr)
    if a.list:
        print(json.dumps(srv.listed(), indent=1))
        return 0
    if a.call:
        try:
            args = json.loads(a.call[1])
        except ValueError as e:
            ap.error(f"-call: bad JSON: {e}")
        r = result_of(srv, a.call[0], args) if a.call[0] in srv.tools else None
        if r is None:
            print(f"mmcp: no tool {a.call[0]!r}", file=sys.stderr)
            return 2
        print(json.dumps(r["structuredContent"], indent=1))
        return 1 if r["isError"] else 0
    serve(srv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
