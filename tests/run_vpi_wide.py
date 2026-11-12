import json,pathlib,subprocess,sys
s=pathlib.Path(__file__).resolve().parent;r=s.parent
sys.path.insert(0,str(r/'tools'))
from vpi_build import build
from vpi_runtime import environment
from vpi_results import check_results
from vpi_provenance import verify
out=r/'build/vpi-wide-64';out.mkdir(parents=True,exist_ok=True)
vectors=[format(n,'064b') for n in (0,1,1<<31,1<<32,(1<<32)|1,1<<63,(1<<64)-1)]+['x'*64,'z'*64,'10xz'*16]
vfile=out/'vectors.json';vfile.write_text(json.dumps(vectors))
build(s/'vpi_wide.sv',out,[s/'cocotb_vpi_wide.py',pathlib.Path(__file__),vfile])
xml=out/'results.xml';trace=out/'trace.json'
for p in (xml,trace):
    if p.exists():p.unlink()
verify(out/'provenance.json')
env=environment('vpi_wide','cocotb_vpi_wide',[s],xml,{'WIDE_VECTORS':str(vfile),'MINISIM_TRACE_PATH':str(trace)})
with (out/'simulation.log').open('w') as f:subprocess.run([str(out/'simulator')],cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=60)
verify(out/'provenance.json');check_results(xml);actual=json.loads(trace.read_text());assert actual==vectors
sv='module tb;reg clk=0;reg[63:0]din=0;wire[63:0]dout;vpi_wide d(clk,din,dout);initial begin '
for bits in vectors:sv+="clk=0;din=64'b"+bits+";#1;clk=1;#1;$display(\"ROW %b\",dout);"
sv+='$finish;end endmodule';(out/'tb.sv').write_text(sv)
subprocess.run(['iverilog','-g2012','-s','tb','-o',str(out/'reference.vvp'),str(s/'vpi_wide.sv'),str(out/'tb.sv')],check=True,timeout=30)
ref=subprocess.check_output(['vvp',str(out/'reference.vvp')],text=True,timeout=30);(out/'reference.log').write_text(ref)
expected=[line[4:] for line in ref.splitlines() if line.startswith('ROW ')];assert actual==expected
(out/'summary.json').write_text(json.dumps({'samples':len(actual),'matched_icarus':True,'input_xz_roundtrip':True,'xml_pass':True,'scope':'flat 64-bit clocked register only; binary-string VPI interface'}))
print('10/10 64-bit real cocotb VPI samples matched Icarus including X/Z')
