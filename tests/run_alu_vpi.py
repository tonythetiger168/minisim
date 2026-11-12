"""Run the original TinyALU cocotb test through shared build/runtime guards."""
import argparse,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from vpi_build import build
from vpi_provenance import verify
from vpi_runtime import environment
from vpi_results import check_results

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,default=ROOT/'build/alu-vpi')
    out=parser.parse_args().output.resolve()
    binary=build(ROOT.parent/'basanos/hdl/tinyalu.sv',out,
                 [Path(__file__),ROOT/'tests/cocotb_tinyalu_native.py'])
    xml=out/'results.xml';trace=out/'trace.json'
    for path in (xml,trace):
        if path.exists():path.unlink()
    env=environment('tinyalu','cocotb_tinyalu_native',[ROOT/'tests'],xml,
                    {'MINISIM_TRACE_PATH':str(trace)})
    verify(out/'provenance.json')
    with (out/'simulation.log').open('w') as log:
        subprocess.run([str(binary)],cwd=out,env=env,stdout=log,
                       stderr=subprocess.STDOUT,check=True,timeout=90)
    verify(out/'provenance.json');check_results(xml)
    if len(json.loads(trace.read_text()))!=128:
        raise RuntimeError('incomplete TinyALU scoreboard trace')
    (out/'summary.json').write_text(json.dumps({'cocotb_samples':128,'xml_pass':True,
        'shared_build':True,'scope':'experimental manifest-configured flat TinyALU VPI host'},indent=2))
    print('Real cocotb TinyALU VPI: 128 samples, XML PASS; shared build/runtime/provenance')
if __name__=='__main__':main()
