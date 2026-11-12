import pathlib,tempfile
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
from vpi_manifest import build_manifest,manifest_header
from pyslang_lower import Unsupported
r=pathlib.Path(__file__).resolve().parents[2]
m=build_manifest(r/'basanos/hdl/tinyalu.sv')
assert len(m['ports'])==8 and m['ports']['result']=={'direction':'output','width':16}
assert [x for x,v in m['ports'].items() if v['direction']=='output']==['result','done']
assert '"tinyalu"' in manifest_header(m)
c=build_manifest(r/'minisim/examples/counter.sv');assert c['ports']['cnt']['direction']=='output'
with tempfile.TemporaryDirectory() as d:
    p=pathlib.Path(d)/'dut.sv';p.write_text('module dut(input logic clk,inout wire pad,output logic q);always @(posedge clk)q<=pad;endmodule')
    try:build_manifest(p)
    except Unsupported:pass
    else:raise AssertionError('inout accepted')
print('3/3 manifest checks: TinyALU, counter, inout rejection')
