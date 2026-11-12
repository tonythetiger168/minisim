import pathlib,sys,subprocess,json
s=pathlib.Path(__file__).parent;r=s.resolve().parents[0]
sys.path.insert(0,str(r/'tools'))
from vpi_provenance import verify
from vpi_results import check_results
from vpi_build import build
from vpi_runtime import environment,identity
out=r/'build/alu-vpi-auto-manifest';out.mkdir(parents=True,exist_ok=True)
build(r.parent/'basanos/hdl/tinyalu.sv',out,[s/'cocotb_tinyalu_native.py'])
xml=out/'results.xml'
if xml.exists():xml.unlink()
env=environment('tinyalu','cocotb_tinyalu_native',[s],xml,{'MINISIM_TRACE_PATH':str(out/'trace.json')})
(out/'runtime.json').write_text(json.dumps(identity(),indent=2))
with (out/'simulation.log').open('w') as f:subprocess.run([str(out/'simulator')],cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=90)
verify(out/'provenance.json')
check_results(xml)
assert len(json.loads((out/'trace.json').read_text()))==128
(out/'summary.json').write_text(json.dumps({'cocotb_samples':128,'xml_pass':True,'scope':'experimental manifest-configured flat TinyALU VPI host'}))
print('Real cocotb TinyALU VPI: 128 samples, XML PASS')
