import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from pyslang_lower import Lowerer, cpp_header, Unsupported

def lower(event):
    with tempfile.TemporaryDirectory() as d:
        p=Path(d)/'dut.sv'
        p.write_text('module dut(input logic clk, rst, output logic [3:0] q); always @('+event+') if(rst) q<=0; else q<=q+1; endmodule')
        return Lowerer(p).lower()

m=lower('posedge clk or posedge rst')
w=m['programs'][0][0]
assert w['events']==[{'signal':'clk','edge':'POS'},{'signal':'rst','edge':'POS'}]
assert cpp_header(m).count('i.events.emplace_back')==2
m=lower('posedge clk or negedge rst')
assert m['programs'][0][0]['events'][1]['edge']=='NEG'
try:
    lower('clk or posedge rst')
except Unsupported:
    pass
else:
    raise AssertionError('level event was accepted')
print('3/3 event-list frontend checks passed; no native simulation claimed')
