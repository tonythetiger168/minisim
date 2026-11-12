import pathlib,sys,subprocess,json
s=pathlib.Path(__file__).parent;r=pathlib.Path('/mnt/c/Users/chien/OneDrive/Documents/ChatGPT/AI-EDA/minisim')
if (s.resolve().parents[0]/'src/minisim.cpp').exists():r=s.resolve().parents[0]
sys.path.insert(0,str(r/'tools'))
from vpi_runtime import environment
from vpi_results import check_results
from vpi_provenance import verify
base=r/'build/alu-vpi-auto-manifest';verify(base/'provenance.json')
out=s/'alu-fourstate'
if s.name=='tests':out=r/'build/alu-fourstate'
out.mkdir(parents=True,exist_ok=True);xml=out/'results.xml';trace=out/'trace.json'
if xml.exists():xml.unlink()
env=environment('tinyalu','cocotb_alu_fourstate',[s],xml,{'MINISIM_TRACE_PATH':str(trace)})
with (out/'simulation.log').open('w') as f:subprocess.run([str(base/'simulator')],cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=90)
check_results(xml);actual=json.loads(trace.read_text());assert len(actual)==15
sv='module tb;reg clk=0,reset_n=0,start=0;reg[7:0]a=0,b=0;reg[2:0]op=0;wire[15:0]result;wire done;tinyalu d(clk,reset_n,start,a,b,op,result,done);initial begin #1;reset_n=1;'
for bits in ('xxxxxxxx','zzzzzzzz','10xz01z0','00000000','11111111'):
    for op in (2,3,4):sv+=f"clk=0;a=8'b{bits};b=0;op={op};start=1;#1;clk=1;#1;$display(\"ROW %b\",result);"
sv+='$finish;end endmodule';(out/'tb.sv').write_text(sv)
subprocess.run(['iverilog','-g2012','-s','tb','-o',str(out/'ref.vvp'),str(r.parent/'basanos/hdl/tinyalu.sv'),str(out/'tb.sv')],check=True,timeout=30)
ref=subprocess.check_output(['vvp',str(out/'ref.vvp')],text=True,timeout=30);expected=[x[4:] for x in ref.splitlines() if x.startswith('ROW ')]
assert actual==expected,(actual,expected)
(out/'reference.log').write_text(ref);(out/'summary.json').write_text(json.dumps({'samples':15,'input_xz_roundtrip':True,'matched_icarus':True,'xml_pass':True}))
print('15/15 real cocotb/VPI four-state samples matched Icarus; input X/Z preserved')
