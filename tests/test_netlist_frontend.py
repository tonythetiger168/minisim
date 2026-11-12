from pathlib import Path
import tempfile
import unittest

from tools.netlist_lower import NetlistLowerer, header
from tools.pyslang_lower import Unsupported


class NetlistTests(unittest.TestCase):
    def lower(self, text):
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / "netlist.sv"
            file.write_text(text)
            return NetlistLowerer(file).lower()

    def test_alias_and_mux(self):
        model = self.lower("module m(input logic a,b,s,output wire y); assign y=s?a:b; endmodule")
        program = model["programs"][0]
        self.assertEqual(program[0]["expr"]["op"], "mux")
        self.assertEqual(set(program[1]["events"]), {"a", "b", "s"})
        self.assertIn("S.newDriver", header(model))

    def test_reject_cycles_and_strengths(self):
        cases = ["module m(output wire y); assign y=y+1'b1; endmodule",
                 "module m(input a,output wire y); assign #1 y=a; endmodule",
                 "module m(input a,output wire y); assign (weak0,weak1) y=a; endmodule"]
        for source in cases:
            with self.subTest(source=source), self.assertRaises(Unsupported): self.lower(source)

    def test_escaped_flatten_name(self):
        model = self.lower("module m(input a,output wire y); wire \\u.q ; assign \\u.q =a; assign y=\\u.q ; endmodule")
        self.assertIn("u.q", model["signals"])


if __name__ == "__main__": unittest.main()
