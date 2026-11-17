#!/usr/bin/env python3
"""mlsp end to end: a small Language Server Protocol client drives the real
server over stdio, the way VS Code does, on the design in tests/mlsp.

    mlsp_test.py MLSP TESTDIR [MLINT]

Checks diagnostics (and that they clear), definition across files, through
parameter and port connections, into a package and to a `define, hover,
references, document highlight, outline, workspace symbols, completion,
UTF-16 positions after non-ASCII text, unsaved buffers that other files
see, minisim/compileInfo, mlint findings on save (when MLINT is given), and
that mlsp -check prints the same thing with -threads 1 and with all threads.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

MLSP = os.path.abspath(sys.argv[1])
SRC = sys.argv[2]
MLINT = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def uri(p):
    return "file://" + urllib.parse.quote(os.path.realpath(p))


class Client:
    def __init__(self, root, bindir=None):
        env = dict(os.environ)
        self.p = subprocess.Popen([MLSP], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env)
        self.next = 1
        self.responses = {}
        self.diags = {}
        self.log = []
        self.cv = threading.Condition()
        threading.Thread(target=self.reader, daemon=True).start()
        threading.Thread(target=lambda: self.p.stderr.read(), daemon=True).start()
        opts = {"mlint": {"enable": MLINT is not None}}
        if bindir:
            opts["binDir"] = bindir
        r = self.request("initialize", {"processId": os.getpid(), "rootUri": uri(root),
                                        "capabilities": {}, "initializationOptions": opts})
        self.caps = r["capabilities"]
        self.notify("initialized", {})

    def reader(self):
        f = self.p.stdout
        while True:
            line = f.readline()
            if not line:
                return
            n = 0
            while line.strip():
                if line.lower().startswith(b"content-length:"):
                    n = int(line.split(b":")[1])
                line = f.readline()
            msg = json.loads(f.read(n))
            with self.cv:
                if "id" in msg and "method" not in msg:
                    self.responses[msg["id"]] = msg
                elif msg.get("method") == "textDocument/publishDiagnostics":
                    self.diags[msg["params"]["uri"]] = msg["params"]["diagnostics"]
                else:
                    self.log.append(msg)
                self.cv.notify_all()

    def send(self, msg):
        b = json.dumps(msg).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
        self.p.stdin.flush()

    def request(self, method, params, timeout=60):
        i = self.next
        self.next += 1
        self.send({"jsonrpc": "2.0", "id": i, "method": method, "params": params})
        with self.cv:
            ok = self.cv.wait_for(lambda: i in self.responses, timeout)
        if not ok:
            raise RuntimeError("no answer to " + method)
        r = self.responses.pop(i)
        if "error" in r:
            raise RuntimeError(method + ": " + r["error"]["message"])
        return r["result"]

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def open(self, path, text=None):
        if text is None:
            text = open(path, encoding="utf-8").read()
        self.notify("textDocument/didOpen", {"textDocument": {
            "uri": uri(path), "languageId": "systemverilog", "version": 1, "text": text}})
        return text

    def change(self, path, text, version):
        self.notify("textDocument/didChange", {"textDocument": {"uri": uri(path), "version": version},
                                               "contentChanges": [{"text": text}]})

    def wait_diags(self, path, pred, timeout=30):
        u = uri(path)
        with self.cv:
            self.cv.wait_for(lambda: u in self.diags and pred(self.diags[u]), timeout)
            return self.diags.get(u)

    def at(self, method, path, line, ch, extra=None):
        params = {"textDocument": {"uri": uri(path)}, "position": {"line": line, "character": ch}}
        params.update(extra or {})
        return self.request(method, params)

    def close(self):
        self.request("shutdown", None)
        self.notify("exit", None)
        try:
            return self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.p.kill()
            return -1


def pos(text, needle, nth=0, offset=0):
    """(line, UTF-16 column) of the nth occurrence of needle, plus offset chars."""
    k = -1
    for _ in range(nth + 1):
        k = text.index(needle, k + 1)
    k += offset
    line = text.count("\n", 0, k)
    start = text.rfind("\n", 0, k) + 1
    return line, len(text[start:k].encode("utf-16-le")) // 2


def loc(r):
    if isinstance(r, list):
        r = r[0] if r else None
    if not r:
        return None
    return (os.path.relpath(urllib.parse.unquote(r["uri"][7:]), ROOT),
            r["range"]["start"]["line"], r["range"]["start"]["character"])


work = tempfile.mkdtemp(prefix="mlsp_test_")
ROOT = os.path.realpath(os.path.join(work, "ws"))
shutil.copytree(SRC, ROOT)
bindir = None
if MLINT:
    bindir = os.path.join(work, "bin")
    os.makedirs(bindir)
    os.symlink(os.path.realpath(MLINT), os.path.join(bindir, "mlint"))

TB = os.path.join(ROOT, "tb", "tb.sv")
ALU = os.path.join(ROOT, "rtl", "alu.sv")
BROKEN = os.path.join(ROOT, "tb", "broken.sv")
LINT = os.path.join(ROOT, "tb", "lint_me.sv")

c = Client(ROOT, bindir)
caps = c.caps
check(caps.get("definitionProvider") and caps.get("hoverProvider") and caps.get("referencesProvider"),
      "initialize: definition, hover, references")
check(caps.get("positionEncoding") == "utf-16", "initialize: UTF-16 positions by default")

tb = c.open(TB)
alu = c.open(ALU)

# Definition across files.
got = loc(c.at("textDocument/definition", TB, *pos(tb, "alu #(")))
check(got == ("rtl/alu.sv", 1, 7),
      "definition: module name -> rtl/alu.sv (%s)" % (got,))
got = loc(c.at("textDocument/definition", TB, *pos(tb, ".W(", offset=1)))
check(got == ("rtl/alu.sv", 3, 16),
      "definition: named parameter assignment -> parameter W (%s)" % (got,))
got = loc(c.at("textDocument/definition", TB, *pos(tb, ".op(", offset=1)))
check(got == ("rtl/alu.sv", 6, 23),
      "definition: named port connection -> port op (%s)" % (got,))
got = loc(c.at("textDocument/definition", TB, *pos(tb, "ADD;")))
check(got == ("rtl/alu_pkg.sv", 1, 28),
      "definition: alu_pkg::ADD -> enum value in the package (%s)" % (got,))
got = loc(c.at("textDocument/definition", TB, *pos(tb, "op_t op;")))
check(got == ("rtl/alu_pkg.sv", 1, 53),
      "definition: alu_pkg::op_t (a type) -> typedef (%s)" % (got,))
got = loc(c.at("textDocument/definition", ALU, *pos(alu, "apply(op")))
check(got == ("rtl/alu_pkg.sv", 2, 33),
      "definition: function call -> the package function (%s)" % (got,))
got = loc(c.at("textDocument/definition", ALU, *pos(alu, "ALU_W")))
check(got == ("rtl/inc/defs.svh", 2, 8),
      "definition: `ALU_W -> the `define in another file (%s)" % (got,))
# UTF-16: `r` after a comment with CJK characters on the same line.
got = loc(c.at("textDocument/definition", ALU, *pos(alu, "= r;", offset=2)))
check(got == ("rtl/alu.sv", 10, 16),
      "definition: UTF-16 column after non-ASCII text (%s)" % (got,))
got = loc(c.at("textDocument/definition", ALU, *pos(alu, "w2")))
check(got == ("rtl/alu.sv", 12, 24),
      "definition: declaration after non-ASCII text points at itself (%s)" % (got,))

# Hover.
h = c.at("textDocument/hover", TB, *pos(tb, "y);", offset=0))
check(h and "logic[7:0] y" in h["contents"]["value"], "hover: variable type")
h = c.at("textDocument/hover", TB, *pos(tb, "alu #("))
check(h and "module alu" in h["contents"]["value"] and "parameter int W" in h["contents"]["value"],
      "hover: module header with parameters")
h = c.at("textDocument/hover", TB, *pos(tb, "dut"))
check(h and "alu dut" in h["contents"]["value"] and "tb.dut" in h["contents"]["value"],
      "hover: instance and its hierarchical path")
h = c.at("textDocument/hover", ALU, *pos(alu, "W-1:0] a"))
check(h and "parameter int W = 8" in h["contents"]["value"], "hover: parameter value (%s)" % (h and h["contents"]["value"]))

# References and highlight.
refs = c.at("textDocument/references", ALU, *pos(alu, "r;"), {"context": {"includeDeclaration": True}})
check(len(refs) == 4, "references: r declared once and used three times (got %d)" % len(refs))
refs = c.at("textDocument/references", ALU, *pos(alu, "r;"), {"context": {"includeDeclaration": False}})
check(len(refs) == 3, "references: without the declaration")
hl = c.at("textDocument/documentHighlight", ALU, *pos(alu, "r;"))
check(len(hl) == 4 and sum(1 for x in hl if x["kind"] == 3) == 1, "documentHighlight: one write-kind declaration")
refs = c.at("textDocument/references", ALU, *pos(alu, "module alu", offset=7), {"context": {"includeDeclaration": True}})
check(len(refs) == 1, "references: a module seen from its own file (tb is not in alu's design)")

# Outline.
syms = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri(ALU)}})
names = [s["name"] for s in syms]
check(names == ["alu"], "documentSymbol: one module")
kids = [k["name"] for k in syms[0].get("children", [])] if syms else []
check(kids == ["W", "clk", "op", "a", "b", "y", "r", "w2"], "documentSymbol: parameter, ports, signals (%s)" % kids)

# Workspace symbols.
ws = c.request("workspace/symbol", {"query": "alu"})
wn = sorted(s["name"] for s in ws)
check(wn == ["`ALU_W", "alu", "alu_pkg"], "workspace/symbol: alu, alu_pkg, `ALU_W (%s)" % wn)
ws = c.request("workspace/symbol", {"query": "_w"})
check([s["name"] for s in ws] == ["`ALU_W"], "workspace/symbol: macros (%s)" % [s["name"] for s in ws])

# Completion.
tb2 = tb.replace("$display", "$disp")
c.change(TB, tb2, 2)
comp = c.at("textDocument/completion", TB, *pos(tb2, "$disp", offset=5))
labels = {i["label"] for i in comp["items"]}
check("display" in labels, "completion: system tasks after $")
comp = c.at("textDocument/completion", TB, *pos(tb2, "initial"))
labels = {i["label"] for i in comp["items"]}
check({"always_ff", "alu", "clk"} <= labels, "completion: keywords, modules, names of this file")
c.change(TB, tb, 3)

# Diagnostics, and that they clear.
broken = c.open(BROKEN)
d = c.wait_diags(BROKEN, lambda d: len(d) >= 2)
codes = sorted(x.get("code") for x in d or [])
check(codes == ["UndeclaredIdentifier", "UnknownModule"], "diagnostics: two errors from slang (%s)" % codes)
first = min(d or [{}], key=lambda x: x["range"]["start"]["line"]) if d else {}
check(first.get("range", {}).get("start") == {"line": 2, "character": 13} and first.get("severity") == 1,
      "diagnostics: position and severity")
fixed = broken.replace("undeclared_sig", "4'd0").replace("  nosuch_mod u0 ();\n", "")
c.change(BROKEN, fixed, 2)
d = c.wait_diags(BROKEN, lambda d: len(d) == 0)
check(d == [], "diagnostics: cleared after the fix (unsaved buffer)")

# An unsaved change in one open file is what another open file sees.
alu2 = alu.replace("input  logic         clk,", "input  logic         clock,").replace("posedge clk", "posedge clock")
c.change(ALU, alu2, 2)
c.change(TB, tb + "\n", 4)
d = c.wait_diags(TB, lambda d: any("clk" in x["message"] for x in d))
check(d and any("clk" in x["message"] for x in d), "unsaved buffers: tb sees the renamed port of the open alu.sv")
c.change(ALU, alu, 3)
c.change(TB, tb, 5)
d = c.wait_diags(TB, lambda d: len(d) == 0)
check(d == [], "unsaved buffers: diagnostics clear again")

# What the editor runs msim with.
info = c.request("minisim/compileInfo", {"textDocument": {"uri": uri(TB)}})
rel = sorted(os.path.relpath(f, ROOT) for f in info["files"])
check(info["tops"] == ["tb"], "compileInfo: top tb")
check(rel == ["rtl/alu.sv", "rtl/alu_pkg.sv", "rtl/inc/defs.svh", "tb/tb.sv"], "compileInfo: the design's files (%s)" % rel)
check(info["files"][0].endswith("defs.svh") and "--single-unit" in info["args"],
      "compileInfo: the `define file first, one compilation unit")

# mlint on save.
if MLINT:
    c.open(LINT)
    c.notify("textDocument/didSave", {"textDocument": {"uri": uri(LINT)}})
    d = c.wait_diags(LINT, lambda d: any(x.get("source") == "mlint" for x in d), 60)
    ml = [x for x in d or [] if x.get("source") == "mlint"]
    check(len(ml) == 1 and ml[0]["code"] == "L001" and ml[0]["range"]["start"] == {"line": 3, "character": 22},
          "mlint on save: L001 latch at 4:23 (%s)" % d)
else:
    print("skip mlint on save (no mlint)")

check(c.close() == 0, "shutdown and exit")

# mlsp -check: the same with one thread and with all of them.
files = [TB, ALU, BROKEN]
one = subprocess.run([MLSP, "-threads", "1", "-check", "-root", ROOT] + files, capture_output=True, text=True)
many = subprocess.run([MLSP, "-check", "-root", ROOT] + files, capture_output=True, text=True)
check(one.stdout == many.stdout and one.returncode == many.returncode, "-check: -threads 1 and all threads agree")
check(one.returncode == 1 and "UnknownModule" in one.stdout, "-check: exit 1 with the errors printed")

# mlsp -check -f: exactly the files of a -f list (the `define file first).
flist = os.path.join(work, "design.f")
with open(flist, "w") as f:
    f.write("--single-unit\nrtl/inc/defs.svh\nrtl/alu_pkg.sv\nrtl/alu.sv\ntb/tb.sv\n")
r = subprocess.run([MLSP, "-check", "-root", ROOT, "-f", flist, TB], capture_output=True, text=True, cwd=ROOT)
check(r.returncode == 0 and "4 files" not in r.stdout and "tops tb" in r.stdout, "-check -f: the list's design, no errors (%s)" % r.stdout.strip())

shutil.rmtree(work, ignore_errors=True)
if failures:
    print("%d failed" % len(failures))
    sys.exit(1)
print("all passed")
