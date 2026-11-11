#!/usr/bin/env python3
"""msupport (tools/msupport.py): product files, documentation search, the
example sandbox, the web service's checks, and the agent loop against a fake
Messages API. No network and no API key.

    msupport_test.py <source dir> [<msim binary>]

Without the binary the sandbox tests that need a simulator are skipped; the
agent-loop test is skipped when the anthropic package is not installed.
"""
import argparse
import hashlib
import importlib.util
import json
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

SRC = Path(sys.argv[1]).resolve()
MSIM = sys.argv[2] if len(sys.argv) > 2 else None
spec = importlib.util.spec_from_file_location("msupport", SRC / "tools" / "msupport.py")
ms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ms)

HELLO = 'module tb; initial begin $display("hello %0d", 6*7); $finish; end endmodule\n'


class Products(unittest.TestCase):
    def test_every_product_indexes(self):
        products = ms.load_products()
        self.assertIn("msim", products)
        for p in products.values():
            desk = ms.Desk(p)                       # raises when a doc or a section is gone
            self.assertGreater(len(desk.index.sections), 3, p.name)
            self.assertIn(p.name, desk.system)
            self.assertEqual(p.access, "public", p.name)   # only free products live here

    def test_search(self):
        desk = ms.Desk(ms.load_products()["msim"])
        self.assertIn("-maxtime", desk.search_docs("stop the simulation at a time maxtime"))
        self.assertIn("FST", desk.search_docs("fst waveform"))
        self.assertIn("No section", desk.search_docs("zzzqqq"))
        sid = desk.index.sections[0][0]
        self.assertEqual(desk.read_doc(sid), desk.index.sections[0][2])
        self.assertIn("No such section", desk.read_doc("nope#nope"))

    def test_unclosed_fence(self):
        secs = ms.split_markdown("x.md", "# A\n```sh\n# not a heading\ncmd\n\n## B\n```sh\nx\n```\n## C\ntext\n")
        self.assertEqual([s[1] for s in secs], ["A", "C"])

    def test_extra_directory(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "manual.md").write_text("# Manual\n\n## Setup\nUse the -frob option.\n")
            Path(d, "x.json").write_text(json.dumps({"name": "xtool", "edition": "paid", "access": "customer",
                                                     "docs": ["manual.md"]}))
            products = ms.load_products([d])
            self.assertIn("-frob", ms.Desk(products["xtool"]).search_docs("frob"))
            Path(d, "x.json").write_text(json.dumps({"name": "xtool", "docs": ["../../etc/passwd"]}))
            with self.assertRaises(ValueError):
                ms.Desk(ms.load_products([d])["xtool"])


class Sandbox(unittest.TestCase):
    def setUp(self):
        self.p = ms.load_products()["msim"]

    def test_options(self):
        run = self.p.run
        self.assertEqual(ms.check_options(run, ["-top", "tb", "-maxtime", "100", "+define+W=8"])[0],
                         ["-top", "tb", "-maxtime", "100", "+define+W=8"])
        for bad in (["-compile"], ["-f", "x"], ["x.c"], ["-sv_lib", "a"], ["+incdir+/etc"], ["-top", "../x"],
                    ["-top"], ["-top", "a;b"], ["-o", "/tmp/x"], ["-vcd", "/etc/x"]):
            self.assertIsNone(ms.check_options(run, bad)[0], bad)

    def test_refused_sources(self):
        for src in ('module t; initial $system("id"); endmodule', 'import "DPI-C" function int f();',
                    'module t; integer f; initial f = $fopen("/etc/passwd","r"); endmodule',
                    '`include "/etc/passwd"', '`define S(a) $sys``a\n', 'module t; reg [7:0] m[4]; initial $readmemh("x", m); endmodule'):
            self.assertIn("error", ms.run_example(self.p, src), src)
        self.assertIn("error", ms.run_example(self.p, "x" * 70000))
        self.assertIn("error", ms.run_example(self.p, HELLO, ["-compile"]))

    @unittest.skipUnless(MSIM, "no msim binary")
    def test_runs(self):
        import os
        os.environ["MINISIM_MSIM"] = MSIM
        r = ms.run_example(self.p, HELLO, ["-top", "tb"])
        self.assertEqual(r.get("exit_code"), 0, r)
        self.assertIn("hello 42", r["output"])
        r = ms.run_example(self.p, "module tb; initial $display(; endmodule\n")
        self.assertNotEqual(r["exit_code"], 0)
        self.assertNotIn("msupport-", r["output"])           # no host paths in what the user sees

    @unittest.skipUnless(MSIM, "no msim binary")
    def test_runaway_is_stopped(self):
        import os
        os.environ["MINISIM_MSIM"] = MSIM
        self.p.run = dict(self.p.run, cpu_seconds=2, wall_seconds=6)
        r = ms.run_example(self.p, "module tb; integer i; initial begin i = 0; forever i = i + 1; end endmodule\n")
        self.assertTrue(r["timed_out"] or r["exit_code"] < 0, r)     # killed by the wall clock or the CPU limit
        self.assertLess(r["seconds"], 8)


class FakeDesk:
    def answer(self, history, q):
        return f"echo {len(history)}: {q}", {}


class Web(unittest.TestCase):
    def service(self, tokens=None):
        products = ms.load_products()
        with tempfile.TemporaryDirectory() as d:
            Path(d, "m.md").write_text("# M\ntext\n")
            Path(d, "p.json").write_text(json.dumps({"name": "paidtool", "access": "customer", "docs": ["m.md"]}))
            products.update(ms.load_products([d]))
        args = argparse.Namespace(tokens=tokens, allow_run=False, tickets=None, per_minute=3)
        s = ms.Service(products, args)
        s.desk = lambda name: FakeDesk()
        return s

    def test_chat_and_limits(self):
        s = self.service()
        code, r = s.chat({"product": "msim", "message": "hi"}, "", "1.1.1.1")
        self.assertEqual((code, r["answer"]), (200, "echo 0: hi"))
        code, r2 = s.chat({"product": "msim", "message": "again", "session": r["session"]}, "", "1.1.1.1")
        self.assertEqual(r2["answer"], "echo 2: again")
        self.assertEqual(s.chat({"product": "nope", "message": "x"}, "", "1.1.1.1")[0], 400)
        self.assertEqual(s.chat({"product": "msim", "message": ""}, "", "1.1.1.1")[0], 400)
        s.chat({"product": "msim", "message": "3"}, "", "2.2.2.2")
        s.chat({"product": "msim", "message": "3"}, "", "2.2.2.2")
        s.chat({"product": "msim", "message": "3"}, "", "2.2.2.2")
        self.assertEqual(s.chat({"product": "msim", "message": "4"}, "", "2.2.2.2")[0], 429)

    def test_customer_products_need_a_token(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write(hashlib.sha256(b"secret-token").hexdigest() + "\n")
        s = self.service(tokens=f.name)
        self.assertEqual(s.chat({"product": "paidtool", "message": "hi"}, "", "3.3.3.3")[0], 403)
        self.assertEqual(s.chat({"product": "paidtool", "message": "hi"}, "wrong", "3.3.3.3")[0], 403)
        self.assertEqual(s.chat({"product": "paidtool", "message": "hi"}, "secret-token", "3.3.3.3")[0], 200)


def fake_api(replies, seen):
    """A Messages API that plays back `replies` in order and records requests."""
    class H(BaseHTTPRequestHandler):
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            seen.append(body)
            msg = {"id": f"msg_{len(seen)}", "type": "message", "role": "assistant", "model": body["model"],
                   "stop_sequence": None, "usage": {"input_tokens": 10, "output_tokens": 5}}
            msg.update(replies[min(len(seen), len(replies)) - 1])
            data = json.dumps(msg).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *a):
            pass
    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


@unittest.skipUnless(importlib.util.find_spec("anthropic"), "anthropic package not installed")
class Loop(unittest.TestCase):
    def test_search_then_answer(self):
        import anthropic
        seen = []
        srv = fake_api([
            {"stop_reason": "tool_use", "content": [
                {"type": "tool_use", "id": "tu_1", "name": "search_docs", "input": {"query": "maxtime"}},
                {"type": "tool_use", "id": "tu_2", "name": "escalate", "input": {"summary": "s", "category": "defect"}}]},
            {"stop_reason": "end_turn", "content": [{"type": "text", "text": "Use -maxtime <n>."}]},
        ], seen)
        try:
            with tempfile.TemporaryDirectory() as d:
                client = anthropic.Anthropic(api_key="test", base_url=f"http://127.0.0.1:{srv.server_address[1]}",
                                             max_retries=0)
                desk = ms.Desk(ms.load_products()["msim"], tickets=Path(d, "t.jsonl"), client=client)
                text, usage = desk.answer([], "How do I stop at a time?")
                ticket = json.loads(Path(d, "t.jsonl").read_text())
        finally:
            srv.shutdown()
        self.assertEqual(text, "Use -maxtime <n>.")
        self.assertEqual(ticket["category"], "defect")
        first, second = seen[0], seen[1]
        self.assertEqual(first["model"], "claude-opus-5-5")
        self.assertEqual(first["fallbacks"], "default")
        self.assertEqual(first["output_config"], {"effort": "medium"})
        self.assertEqual(sorted(t["name"] for t in first["tools"]),
                         ["escalate", "read_doc", "run_example", "search_docs"])
        results = [b for b in second["messages"][-1]["content"] if b["type"] == "tool_result"]
        self.assertEqual(len(results), 2)                     # both results in one user message
        self.assertIn("-maxtime", json.dumps(results[0]["content"]))
        self.assertIn("ticket", json.dumps(results[1]["content"]))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v"])
