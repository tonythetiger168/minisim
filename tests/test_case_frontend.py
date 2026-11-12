import pathlib,sys,tempfile,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
from pyslang_lower import Lowerer,Unsupported
class CaseTests(unittest.TestCase):
    def lower(self,body):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d)/'case.sv'
            p.write_text('module dut(input logic clk,input logic[1:0]op,output logic[3:0]q);always @(posedge clk) '+body+' endmodule')
            return Lowerer(p).lower()
    def test_grouped_labels_and_default(self):
        m=self.lower("case(op) 0,1:q<=3;2:q<=4;default:q<=5;endcase")
        self.assertEqual(m['programs'][0][1]['expr']['op'],'Or')
        self.assertEqual(m['programs'][0][1]['expr']['left']['op'],'CaseEq')
    def test_no_default(self):
        m=self.lower('case(op) 0:q<=1;endcase')
        self.assertEqual(m['programs'][0][1]['expr']['op'],'CaseEq')
    def test_unsupported_case_forms(self):
        for body in ("casez(op) 2'b?1:q<=1;endcase", "casex(op) 0:q<=1;endcase",'unique case(op) 0:q<=1;endcase',"case(op) 2'bxx:q<=1;endcase",'case(op) op:q<=1;endcase'):
            with self.subTest(body=body),self.assertRaises(Unsupported):self.lower(body)
if __name__=='__main__':unittest.main()
