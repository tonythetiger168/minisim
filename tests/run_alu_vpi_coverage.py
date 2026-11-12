"""TinyALU coverage through the shared build and real cocotb runtime."""
import argparse,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from vpi_build import build
from vpi_provenance import verify,digest
from vpi_runtime import environment
from vpi_results import check_results

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,default=ROOT/'build/alu-vpi-coverage')
    out=parser.parse_args().output.resolve();out.mkdir(parents=True,exist_ok=False)
    binary=build(ROOT.parent/'basanos/hdl/tinyalu.sv',out,[Path(__file__),ROOT/'tests/cocotb_tinyalu_native.py'])
    xml=out/'results.xml';trace=out/'trace.json'
    for name in ('results.xml','trace.json','line.json','toggle.json'):
        file=out/name
        if file.exists():file.unlink()
    env=environment('tinyalu','cocotb_tinyalu_native',[ROOT/'tests'],xml,{'MINISIM_COVERAGE':'1','MINISIM_TRACE_PATH':str(trace)})
    verify(out/'provenance.json')
    with (out/'simulation.log').open('w') as log:
        subprocess.run([str(binary)],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
    verify(out/'provenance.json');check_results(xml)
    if len(json.loads(trace.read_text()))!=128:raise RuntimeError('incomplete TinyALU scoreboard trace')
    stats={}
    for kind in ('line','toggle'):
        report=json.loads((out/(kind+'.json')).read_text());bins=report['bins']
        if not bins or any(type(v) is not int or v<0 for v in bins.values()):raise ValueError('invalid coverage denominator/counts')
        holes=[key for key,hits in bins.items() if hits==0]
        if report['closed'] != (not holes):raise ValueError('inconsistent coverage closure')
        stats[kind]={'hit':len(bins)-len(holes),'total':len(bins),'holes':holes,'closed':report['closed']}
    (out/'coverage-summary.json').write_text(json.dumps(stats,indent=2))
    (out/'summary.json').write_text(json.dumps({'cocotb_samples':128,'xml_pass':True,'shared_build':True,'scope':'flat TinyALU source-mapped IR line and directional toggle'},indent=2))
    (out/'coverage-identity.json').write_text(json.dumps({'binary_sha256':digest(binary),'toggle_sha256':digest(out/'toggle.json'),'provenance_sha256':digest(out/'provenance.json')},indent=2))
    print(json.dumps(stats))
if __name__=='__main__':main()
