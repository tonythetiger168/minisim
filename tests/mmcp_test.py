#!/usr/bin/env python3
"""mmcp (tools/mmcp.py): the MCP protocol over stdio, the root fence,
plug-ins, and every free tool on the real programs.

    mmcp_test.py <source dir> [<bin dir with msim, mlint, mdebug>]

Without the bin dir the tests that need the simulator are skipped.
"""
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SRC = Path(sys.argv[1]).resolve()
BIN = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
MMCP = SRC / "tools" / "mmcp.py"
HAVE = BIN is not None and (BIN / "msim").exists()

COUNTER = """\
module counter(input logic clk, input logic rst, output logic [3:0] q);
  always_ff @(posedge clk) begin
    if (rst) q <= 0;
    else if (q == 4'd9) q <= 0;
    else q <= q + 1;
  end
endmodule
"""
TB = """\
module tb;
  logic clk = 0, rst = 1;
  logic [3:0] q;
  counter u(.clk, .rst, .q);
  always #5 clk = ~clk;
  initial begin
    #12 rst = 0;
    #100;
    if (q !== 4'd0) $display("q=%0d", q);
    $display("TEST PASSED");
    $finish;
  end
endmodule
"""
FATAL = 'module tb; initial begin #3 $fatal(1, "boom"); end endmodule\n'
BROKEN = "module tb;\n  logic a\n  initial a = 1;\nendmodule\n"
LINTY = "module m(input logic [7:0] a, output logic [3:0] y);\n  logic [3:0] u;\n  always @(*) if (a[0]) y = a[3:0];\nendmodule\n"


class Client:
    """An MCP client on a pipe, as an agent would be."""

    def __init__(self, root, *extra):
        argv = [sys.executable, str(MMCP), "-root", str(root)]
        if BIN:
            argv += ["-bin", str(BIN)]
        self.p = subprocess.Popen(argv + list(extra), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        self.n = 0

    def send(self, obj):
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params=None):
        self.n += 1
        self.send({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params or {}})
        return json.loads(self.p.stdout.readline())

    def call(self, name, **args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        self.last = r
        res = r["result"]
        body = json.loads(res["content"][0]["text"])
        assert body == res["structuredContent"]
        return body, res["isError"]

    def close(self):
        self.p.stdin.close()
        self.p.wait(30)
        self.p.stdout.close()
        self.p.stderr.close()


class Protocol(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.c = Client(self.root)

    def tearDown(self):
        self.c.close()
        self.tmp.cleanup()

    def test_handshake_and_list(self):
        r = self.c.rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                                      "clientInfo": {"name": "t", "version": "0"}})
        self.assertEqual(r["result"]["protocolVersion"], "2025-03-26")
        self.assertIn("tools", r["result"]["capabilities"])
        self.assertEqual(r["result"]["serverInfo"]["name"], "minisim")
        self.c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})   # no answer
        r = self.c.rpc("initialize", {"protocolVersion": "1999-01-01"})
        self.assertEqual(r["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual(self.c.rpc("ping")["result"], {})
        names = [t["name"] for t in self.c.rpc("tools/list")["result"]["tools"]]
        for n in ("minisim_info", "mcov_report", "mregress_run", "mplan_report", "read_file"):
            self.assertIn(n, names)
        if HAVE:
            for n in ("msim_run", "mlint_check", "mdebug_query"):
                self.assertIn(n, names)
        for t in self.c.rpc("tools/list")["result"]["tools"]:
            self.assertEqual(t["inputSchema"]["type"], "object")
            self.assertTrue(t["description"])

    def test_errors(self):
        self.assertEqual(self.c.rpc("nope/nope")["error"]["code"], -32601)
        self.assertEqual(self.c.rpc("tools/call", {"name": "nope"})["error"]["code"], -32602)
        self.c.p.stdin.write("{not json\n")
        self.c.p.stdin.flush()
        self.assertEqual(json.loads(self.c.p.stdout.readline())["error"]["code"], -32700)
        body, err = self.c.call("read_file")
        self.assertTrue(err)
        self.assertIn("missing argument 'path'", body["error"])

    def test_root_fence(self):
        (self.root / "a.txt").write_text("one\ntwo\nthree\n")
        body, err = self.c.call("read_file", path="a.txt", start=2, count=1)
        self.assertFalse(err)
        self.assertEqual(body["text"], "two\n")
        self.assertTrue(body["more"])
        body, err = self.c.call("read_file", path="a.txt", grep="^t")
        self.assertEqual([m["line"] for m in body["matches"]], [2, 3])
        for bad in ("../x", "/etc/passwd", "sub/../../x"):
            body, err = self.c.call("read_file", path=bad)
            self.assertTrue(err, bad)
            self.assertIn("outside the root", body["error"])
        os.symlink("/etc", self.root / "link")
        body, err = self.c.call("read_file", path="link/passwd")
        self.assertTrue(err)
        body, err = self.c.call("mcov_report", dbs=["../cov.json"])
        self.assertTrue(err)

    def test_info(self):
        body, err = self.c.call("minisim_info")
        self.assertFalse(err)
        self.assertEqual(body["root"], str(self.root.resolve()))
        self.assertIn("read_file", body["tools"])


class Plugins(unittest.TestCase):
    def test_plugins(self):
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as pdir:
            Path(pdir, "good.py").write_text(textwrap.dedent("""\
                def register(srv):
                    srv.tool("addon_echo", "echo", {"type": "object", "properties": {"x": {"type": "string"}}},
                             lambda s, a: {"echo": a.get("x"), "root": s.root})
                    srv.tool("addon_hidden", "needs a program that is not there", {"type": "object"},
                             lambda s, a: {}, needs=["no_such_program_mmcp"])
                    srv.tool("addon_refused", "a tool whose program says no licence", {"type": "object"},
                             lambda s, a: {"error": "licence: no licence file was found", "licence": False})
                """))
            Path(pdir, "broken.py").write_text("raise RuntimeError('broken on purpose')\n")
            env = dict(os.environ, MINISIM_MCP_PLUGINS=pdir)
            out = subprocess.run([sys.executable, str(MMCP), "-root", root, "-list"], capture_output=True, text=True, env=env)
            self.assertEqual(out.returncode, 0, out.stderr)
            names = [t["name"] for t in json.loads(out.stdout)]
            self.assertIn("addon_echo", names)
            self.assertIn("addon_refused", names)
            self.assertNotIn("addon_hidden", names)
            self.assertIn("read_file", names)                 # a broken plug-in leaves the rest working
            self.assertIn("broken on purpose", out.stderr)
            out = subprocess.run([sys.executable, str(MMCP), "-root", root, "-plugin", pdir, "-call", "addon_echo", '{"x": "hi"}'],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertEqual(json.loads(out.stdout)["echo"], "hi")
            out = subprocess.run([sys.executable, str(MMCP), "-root", root, "-plugin", pdir, "-call", "addon_refused", "{}"],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 1)
            self.assertFalse(json.loads(out.stdout)["licence"])


@unittest.skipUnless(HAVE, "needs msim, mlint and mdebug (pass the bin directory)")
class Engines(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        (cls.root / "rtl").mkdir()
        (cls.root / "rtl" / "counter.sv").write_text(COUNTER)
        (cls.root / "tb.sv").write_text(TB)
        (cls.root / "fatal.sv").write_text(FATAL)
        (cls.root / "broken.sv").write_text(BROKEN)
        (cls.root / "linty.sv").write_text(LINTY)
        cls.c = Client(cls.root)
        cls.c.rpc("initialize", {"protocolVersion": "2025-06-18"})

    @classmethod
    def tearDownClass(cls):
        cls.c.close()
        cls.tmp.cleanup()

    def test_sim_pass_cov_wave_debug(self):
        r, err = self.c.call("msim_run", files=["rtl/counter.sv", "tb.sv"], top="tb", coverage=True, wave="vcd")
        self.assertFalse(err, r)
        self.assertEqual(r["status"], "pass", r)
        self.assertTrue(r["passed"])
        self.assertIn("TEST PASSED", r["log_tail"])
        self.assertTrue((self.root / r["coverage_db"]).exists())
        self.assertTrue((self.root / r["wave"]).exists())
        self.assertFalse(r["command"][1].startswith("/"))       # paths shown relative to the root

        cov, err = self.c.call("mcov_report", dbs=[r["coverage_db"]], uncovered=True, min={"line": 50, "toggle": 100.5})
        self.assertFalse(err, cov)
        self.assertGreater(cov["totals"]["line"]["total"], 0)
        self.assertGreater(cov["totals"]["line"]["pct"], 50)
        self.assertEqual(cov["gate"]["failed"], ["toggle"])
        self.assertIn("uncovered_lines", cov)
        self.assertTrue(all(b["file"].endswith(".sv") for b in cov["uncovered_branches"]))
        self.assertTrue(any(f["file"].endswith("counter.sv") for f in cov["files"]))
        r2, _ = self.c.call("msim_run", files=["rtl/counter.sv", "tb.sv"], top="tb", coverage=True)
        both, err = self.c.call("mcov_report", dbs=[r["coverage_db"], r2["coverage_db"]])
        self.assertFalse(err, both)                                  # several databases are merged
        self.assertEqual(both["totals"]["line"]["total"], cov["totals"]["line"]["total"])

        d, err = self.c.call("mdebug_query", files=["rtl/counter.sv", "tb.sv"], top="tb", wave=r["wave"],
                             commands=["end", "value tb.u.q 60", "drivers tb.u.q", "value tb.nope"])
        self.assertFalse(err, d)
        self.assertEqual(len(d["answers"]), 4)
        self.assertTrue(d["answers"][1]["ok"])
        self.assertTrue(d["answers"][2]["drivers"])
        self.assertFalse(d["answers"][3]["ok"])

    def test_sim_failures(self):
        r, _ = self.c.call("msim_run", files=["fatal.sv"], top="tb")
        self.assertFalse(r["passed"])
        self.assertIn(r["status"], ("fatal", "error"))
        r, _ = self.c.call("msim_run", files=["broken.sv"], top="tb")
        self.assertEqual(r["status"], "compile_error", r)
        self.assertTrue(any(e.get("file", "").endswith("broken.sv") for e in r["errors"]), r["errors"])
        r, err = self.c.call("msim_run", files=["missing.sv"])
        self.assertTrue(err)

    def test_lint(self):
        r, err = self.c.call("mlint_check", files=["linty.sv"], top="m")
        self.assertFalse(err, r)
        self.assertTrue(r["ok"], r)
        self.assertGreater(r["count"], 0)
        self.assertIn("LAT001", r["by_rule"], r["findings"])
        self.assertTrue(any(f.get("signal") == "m.u" and f["line"] == 2 for f in r["findings"]), r["findings"])
        r, err = self.c.call("mlint_check", files=["linty.sv"], top="m", only="LAT001")
        self.assertEqual(list(r["by_rule"]), ["LAT001"])

    def test_regress_and_plan(self):
        (self.root / "tests.list").write_text(
            f"good: {self.root}/rtl/counter.sv {self.root}/tb.sv -top tb @pass=TEST PASSED\n"
            f"bad: {self.root}/fatal.sv -top tb\n")
        r, err = self.c.call("mregress_run", lists=["tests.list"], jobs=2, out="reg")
        self.assertFalse(err, r)
        self.assertTrue(r["ok"], r)
        self.assertEqual((r["passed"], r["failed"]), (1, 1))
        self.assertEqual(r["failure_groups"][0]["runs"][0]["test"], "bad")
        (self.root / "p.mplan").write_text("plan p \"P\"\n  feature f \"F\"\n    test good\n  feature g \"G\"\n    test bad\n")
        c, err = self.c.call("mplan_report", mode="check", plan="p.mplan")
        self.assertEqual(c["exit_status"], 0, c)
        p, err = self.c.call("mplan_report", plan="p.mplan", results="reg")
        self.assertFalse(err, p)
        feats = {f["id"]: f for f in p["plan"]["features"]}
        self.assertEqual(feats["f"]["status"], "met")
        self.assertNotEqual(feats["g"]["status"], "met")


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1], verbosity=2)
