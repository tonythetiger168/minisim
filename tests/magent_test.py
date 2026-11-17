#!/usr/bin/env python3
"""magent (tools/magent.py): the triage, lint-fix and coverage-closure agents
with a scripted model, so the engines' verdicts are what is tested; and the
OpenAI-compatible backend against a local fake endpoint. No network, no key.

    magent_test.py <source dir> <bin dir with msim, mlint, mdebug>
"""
import importlib.util
import json
import os
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

SRC = Path(sys.argv[1]).resolve()
BIN = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
HAVE = BIN is not None and (BIN / "msim").exists()
spec = importlib.util.spec_from_file_location("magent", SRC / "tools" / "magent.py")
ma = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ma)


class Scripted(ma.Backend):
    """A model that follows a script: each step gets the last tool results and returns the next calls."""
    model = "scripted"

    def __init__(self, steps):
        self.steps, self.seen = list(steps), []

    def start(self, system, text, tools):
        self.system, self.text, self.tool_names = system, text, [t["name"] for t in tools]
        self.last = []

    def turn(self):
        self.calls += 1
        if not self.steps:
            return "done", []
        step = self.steps.pop(0)
        calls = step(self) if callable(step) else step
        return "", [(f"c{self.calls}_{i}", n, x) for i, (n, x) in enumerate(calls)]

    def answer(self, results):
        self.last = [json.loads(out) for _, out, _ in results]
        self.seen.append(self.last)


LATCHY = """\
module m(input logic [7:0] a, output logic [3:0] y);
  logic [3:0] u;
  always_comb begin
    y = a[3:0];
  end
endmodule
"""

DUT = """\
module dut(input logic clk, rst, load, input logic [3:0] d, output logic [3:0] q);
  always_ff @(posedge clk) begin
    if (rst) q <= 0;
    else if (load) q <= d;
    else q <= q + 1;
  end
endmodule
"""
TB = """\
module tb;
  logic clk = 0, rst = 1, load = 0;
  logic [3:0] d = 0, q;
  dut u(.clk, .rst, .load, .d, .q);
  always #5 clk = ~clk;
  initial begin
    #22 rst = 0;
    #40 $display("TEST PASSED");
    $finish;
  end
endmodule
"""
LOAD_TEST = """\
module tb_load;
  logic clk = 0, rst = 1, load = 0;
  logic [3:0] d = 4'd7, q;
  dut u(.clk, .rst, .load, .d, .q);
  always #5 clk = ~clk;
  initial begin
    #22 rst = 0; load = 1;
    #10 load = 0;
    if (q !== 4'd7) $fatal(1, "load failed: q=%0d", q);
    $display("TEST PASSED");
    $finish;
  end
endmodule
"""


def run(argv, backend):
    with tempfile.NamedTemporaryFile("r", suffix=".json", delete=False) as f:
        path = f.name
    rc = ma.main(argv + ["-o", path], backend=backend)
    with open(path) as f:
        r = json.load(f)
    os.remove(path)
    return rc, r


@unittest.skipUnless(HAVE, "needs msim, mlint and mdebug")
class LintFix(unittest.TestCase):
    def test_edits_are_judged(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, "m.sv").write_text(LATCHY)
            steps = [
                [("read_file", {"path": "m.sv"})],
                [("edit_file", {"file": "m.sv", "old": "logic [3:0] u;", "new": "logic [3:0] u", "reason": "typo"})],
                [("edit_file", {"file": "m.sv", "old": "  y = a[3:0];", "new": "  y = a[3:0];\n    begin end", "reason": "noop"}),
                 ("edit_file", {"file": "m.sv", "old": "  logic [3:0] u;\n", "new": "", "reason": "unused"})],
                [("edit_file", {"file": "m.sv", "old": "  always_comb", "new": "  logic [7:0] v;\n  always_comb", "reason": "adds one"})],
                [("edit_file", {"file": "m.sv", "old": "nope", "new": "x", "reason": "missing"})],
                [("finish", {})],
                [("read_file", {"path": "m.sv"})],          # never reached: finish ends the session
            ]
            b = Scripted(steps)
            rc, r = run(["lintfix", "m.sv", "-top", "m", "-root", root, "-bin", str(BIN), "-no-equiv"], b)
            self.assertEqual(rc, 0, r)
            m = r["metrics"]
            self.assertEqual(m["edits_accepted"], 1, r)
            self.assertEqual(m["removed"], 1)
            self.assertEqual(m["added"], 0)
            self.assertTrue(m["compiles"])
            self.assertEqual(m["edits_rejected"], 3, r["rejected"])
            why = [x["verdict"] for x in r["rejected"]]
            self.assertFalse(why[0]["compiles"])                   # the syntax error was undone
            self.assertTrue(why[2]["added"])                       # the new unused signal was undone
            self.assertIn("occurs 0 times", json.dumps(b.seen[4]))
            patch = Path(root, r["patch"]).read_text()
            self.assertIn("-  logic [3:0] u;", patch)
            self.assertIn("logic [3:0] u;", Path(root, "m.sv").read_text())   # no -apply: untouched
            self.assertEqual(b.steps, [[("read_file", {"path": "m.sv"})]])

    def test_apply(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, "m.sv").write_text(LATCHY)
            b = Scripted([[("edit_file", {"file": "m.sv", "old": "  logic [3:0] u;\n", "new": "", "reason": "unused"})],
                          [("finish", {})]])
            rc, r = run(["lintfix", "m.sv", "-top", "m", "-root", root, "-bin", str(BIN), "-no-equiv", "-apply"], b)
            self.assertEqual(r["applied"], ["m.sv"])
            self.assertNotIn("logic [3:0] u;", Path(root, "m.sv").read_text())


@unittest.skipUnless(HAVE, "needs msim")
class CovClose(unittest.TestCase):
    def test_tests_kept_only_when_they_add(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, "dut.sv").write_text(DUT)
            Path(root, "tb.sv").write_text(TB)
            steps = [
                [("uncovered_now", {})],
                [("add_test", {"name": "broken", "source": "module tb_x; initial begin x = ; end endmodule"})],
                [("add_test", {"name": "same", "source": TB.replace("module tb;", "module tb_same;")})],
                [("add_test", {"name": "load", "source": LOAD_TEST})],
                [("finish", {"unreachable": []})],
            ]
            b = Scripted(steps)
            rc, r = run(["covclose", "dut.sv", "-tb", "tb.sv", "-root", root, "-bin", str(BIN), "-goal", "branch=100"], b)
            self.assertEqual(rc, 0, r)
            m = r["metrics"]
            self.assertLess(m["before"]["branch"], 100)
            self.assertGreater(m["after"]["branch"], m["before"]["branch"])
            self.assertEqual(m["tests_kept"], 1)
            self.assertEqual(m["tests_dropped"], 2)
            self.assertEqual(r["kept"][0]["top"], "tb_load")
            self.assertTrue(Path(root, r["kept"][0]["file"]).exists())
            self.assertTrue(Path(root, r["coverage_db"]).exists())
            self.assertFalse(b.seen[1][0]["kept"])
            self.assertIn("nothing new", b.seen[2][0]["why"])
            br = b.seen[0][0]["uncovered_branches"]
            self.assertEqual([(x["file"], x["line"], x["never"]) for x in br], [("dut.sv", 4, ["true"])])


@unittest.skipUnless(HAVE, "needs msim")
class Triage(unittest.TestCase):
    def test_submit_and_score(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, "dut.sv").write_text(DUT)
            Path(root, "tb.sv").write_text(TB)
            Path(root, "bad.sv").write_text(LOAD_TEST.replace("4'd7)", "4'd8)"))
            Path(root, "t.list").write_text(f"good: {root}/dut.sv {root}/tb.sv -top tb\nbad: {root}/dut.sv {root}/bad.sv -top tb_load\n")
            srv = ma.mmcp.Server(root, [str(BIN)])
            reg = ma.mmcp.result_of(srv, "mregress_run", {"lists": ["t.list"], "out": "reg"})["structuredContent"]
            self.assertEqual(reg["failed"], 1, reg)
            sig = reg["failure_groups"][0]["reason"]
            sigs = {}

            def diagnose(b):
                g = json.loads(b.text.split("Failure groups (most runs first):\n")[1].split("\n\nTests that pass")[0])
                sigs["s"] = g[0]["group"]
                return [("read_file", {"path": g[0]["log"]})]

            def submit(b):
                return [("submit_triage", {"group": "not a group", "category": "design_bug", "root_cause": "?", "confidence": "low"}),
                        ("submit_triage", {"group": sigs["s"], "category": "testbench_bug", "confidence": "high",
                                           "root_cause": "the test expects 8 after loading 7",
                                           "file": "bad.sv", "line": 10, "quote": "if (q !== 4'd8)"})]
            b = Scripted([diagnose, submit])
            Path(root, "labels.json").write_text(json.dumps({"placeholder": {}}))
            rc, r = run(["triage", "-results", "reg", "-root", root, "-bin", str(BIN)], b)
            self.assertEqual(rc, 0, r)
            self.assertIn("log", json.dumps(b.seen[0]))             # the log was readable
            self.assertIn("must be one of", json.dumps(b.seen[1][0]))
            m = r["metrics"]
            self.assertEqual((m["groups"], m["diagnosed"], m["locations_valid"]), (1, 1, 1), r)
            Path(root, "labels.json").write_text(json.dumps({sigs["s"]: {"category": "testbench_bug", "file": "bad.sv", "line": 10}}))
            b = Scripted([diagnose, submit])
            rc, r = run(["triage", "-results", "reg", "-root", root, "-bin", str(BIN), "-labels", "labels.json"], b)
            self.assertEqual((r["metrics"]["category_correct"], r["metrics"]["location_correct"]), (1, 1))
            self.assertTrue(sig)


class OpenAIWire(unittest.TestCase):
    def test_tool_call_round_trip(self):
        got = []

        class H(BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                got.append((self.path, self.headers.get("Authorization"), body))
                if len(got) == 1:
                    msg = {"role": "assistant", "content": None, "tool_calls": [
                        {"id": "t1", "type": "function", "function": {"name": "read_file", "arguments": '{"path": "a.txt"}'}}]}
                else:
                    msg = {"role": "assistant", "content": "all done"}
                out = json.dumps({"choices": [{"message": msg}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(out)))
                self.end_headers()
                self.wfile.write(out)

            def log_message(self, *a):
                pass

        httpd = HTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            with tempfile.TemporaryDirectory() as root:
                Path(root, "a.txt").write_text("hello\n")
                srv = ma.mmcp.Server(root)
                b = ma.OpenAIBackend("deepseek-chat", f"http://127.0.0.1:{httpd.server_port}/v1", key="k")
                sess = ma.Session(srv, b, ["read_file"])
                final = sess.run("sys", "read a.txt")
            self.assertEqual(final, "all done")
            path, auth, first = got[0]
            self.assertEqual(path, "/v1/chat/completions")
            self.assertEqual(auth, "Bearer k")
            self.assertEqual(first["model"], "deepseek-chat")
            self.assertEqual(first["tools"][0]["function"]["name"], "read_file")
            second = got[1][2]["messages"]
            self.assertEqual(second[-1]["role"], "tool")
            self.assertEqual(second[-1]["tool_call_id"], "t1")
            self.assertIn("hello", json.loads(second[-1]["content"])["text"])
        finally:
            httpd.shutdown()


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1], verbosity=2)
