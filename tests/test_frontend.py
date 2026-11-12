import json
from pathlib import Path
import tempfile
import unittest

from tools.pyslang_lower import Lowerer, Unsupported, cpp_header

ROOT = Path(__file__).resolve().parents[1]
COUNTER = (ROOT / "examples/counter.sv").read_text()


class FrontendTests(unittest.TestCase):
    def lower_text(self, text):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "design.sv"
            path.write_text(text)
            return Lowerer(path).lower()

    def test_real_source_mapping(self):
        model = Lowerer(ROOT / "examples/counter.sv").lower()
        self.assertEqual(model["signals"], {"clk": 1, "rst": 1, "cnt": 4})
        self.assertEqual({v["line"] for v in model["source_map"].values()}, {4, 5})
        self.assertEqual(len(model["source_sha256"]), 64)
        self.assertEqual(len(model["programs"]), 1)
        self.assertTrue(all(key in cpp_header(model) for key in model["source_map"]))

    def test_names_are_from_ast(self):
        model = self.lower_text(COUNTER.replace("cnt", "result"))
        self.assertIn("result", model["signals"])
        self.assertNotIn("cnt", model["signals"])
        self.assertIn('d.signals.at("result")', cpp_header(model))

    def test_begin_end_and_no_else(self):
        model = self.lower_text('''module m(input logic clk, en, output logic [3:0] q);
always_ff @(posedge clk) begin if(en) begin q <= 0; end end
endmodule''')
        self.assertTrue(model["source_map"])

    def test_sizing_is_explicit(self):
        model = self.lower_text(COUNTER.replace("[3:0]", "[7:0]"))
        self.assertEqual(model["signals"]["cnt"], 8)
        self.assertIn('"op": "resize"', json.dumps(model))

    def test_elaborated_parameters(self):
        text="""module counter #(parameter int W=8, parameter logic [7:0] STEP=3)
(input logic clk,rst,output logic [W-1:0] cnt);
localparam logic [W-1:0] ZERO=0;
always_ff @(posedge clk) if(rst) cnt<=ZERO;else cnt<=cnt+STEP;
endmodule
"""
        model=self.lower_text(text)
        self.assertEqual(model["signals"]["cnt"],8)
        self.assertEqual(model["parameters"]["STEP"]["value"],3)
        self.assertTrue(model["parameters"]["ZERO"]["local"])
        self.assertFalse(model["parameters"]["W"]["local"])
        self.assertIn('"value": 3',json.dumps(model["programs"]))
        high=self.lower_text(text.replace('STEP=3','STEP=200'))
        self.assertEqual(high["parameters"]["STEP"]["value"],200)

    def test_parameter_rejections(self):
        for declaration in ("parameter logic P=1'bx", 'parameter int P=-1',
                            'parameter logic [64:0] P=1', 'parameter real P=1.5',
                            'parameter string P="hello"', 'parameter type P=logic'):
            with self.subTest(declaration=declaration), self.assertRaises(Unsupported):
                self.lower_text("module counter #("+declaration+")(input logic clk,rst,output logic[3:0]cnt);always_ff @(posedge clk)if(rst)cnt<=0;else cnt<=cnt+1;endmodule\n")

    def test_unsupported_is_rejected(self):
        cases = {
            "two_state": COUNTER.replace("logic", "bit"),
            "signed": COUNTER.replace("logic [3:0]", "logic signed [3:0]"),
            "ascending": COUNTER.replace("[3:0]", "[0:3]"),
            "wide": COUNTER.replace("[3:0]", "[64:0]"),
            "blocking": COUNTER.replace("cnt <=", "cnt ="),
            "partial_nba": COUNTER.replace("cnt <= 0", "cnt[0] <= 0"),
            "initial": COUNTER.replace("always_ff @(posedge clk)", "initial"),
            "divide": COUNTER.replace("cnt + 1", "cnt / 2"),
            "bit_cast": COUNTER.replace("cnt + 1", "bit'(rst)"),
            "unique_if": COUNTER.replace("if (rst)", "unique if (rst)"),
            "sva": "module m(input logic clk,a); assert property(@(posedge clk) a); endmodule",
            "hierarchy": "module leaf; endmodule module m; leaf u(); endmodule",
            "initializer": "module m(input logic clk); logic q=0; always_ff @(posedge clk) q<=1; endmodule",
            "syntax_error": "module incomplete(",
            "empty": "module m; endmodule",
        }
        for name, text in cases.items():
            with self.subTest(name=name), self.assertRaises(Unsupported):
                self.lower_text(text)


if __name__ == "__main__":
    unittest.main()
