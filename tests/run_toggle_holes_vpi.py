"""Supplement an explicitly verified TinyALU toggle baseline without rebuilding it."""
import argparse,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from vpi_provenance import verify,digest
from vpi_runtime import environment
from vpi_results import check_results

def baseline_identity(base):
    verify(base/'provenance.json')
    recorded=json.loads((base/'coverage-identity.json').read_text())
    actual={'binary_sha256':digest(base/'simulator'),'toggle_sha256':digest(base/'toggle.json'),
            'provenance_sha256':digest(base/'provenance.json')}
    if recorded!=actual:raise ValueError('baseline coverage identity mismatch')
    return actual

def bins(path):
    report=json.loads(path.read_text());values=report['bins']
    if not values or any(type(v) is not int or v<0 for v in values.values()):raise ValueError('invalid toggle bins')
    if report['closed']!=(all(v>0 for v in values.values())):raise ValueError('inconsistent toggle closure')
    return report

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--baseline',type=Path,default=ROOT/'build/alu-vpi-coverage')
    parser.add_argument('--output',type=Path,default=ROOT/'build/alu-toggle-holes')
    args=parser.parse_args();base=args.baseline.resolve();identity=baseline_identity(base)
    before=bins(base/'toggle.json')
    if 'd.signals.emplace("result", S.sig("result", 16));' not in (base/'design.hpp').read_text():raise ValueError('result registration changed')
    holes={k for k,v in before['bins'].items() if v==0}
    if holes!={'sig:6:bit:15:rise','sig:6:bit:15:fall'} or len(before['bins'])!=78:raise ValueError('unexpected baseline holes/schema')
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    inputs=[Path(__file__).resolve(),ROOT/'tests/cocotb_alu_toggle_holes.py']
    test_identity={str(p):digest(p) for p in inputs}
    xml=out/'results.xml';binary=base/'simulator'
    env=environment('tinyalu','cocotb_alu_toggle_holes',[ROOT/'tests'],xml,{'MINISIM_COVERAGE':'1'})
    if baseline_identity(base)!=identity:raise ValueError('baseline changed before simulation')
    with (out/'simulation.log').open('w') as log:
        subprocess.run([str(binary)],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
    if baseline_identity(base)!=identity or any(digest(p)!=sha for p,sha in test_identity.items()):raise ValueError('baseline/test changed during simulation')
    check_results(xml);after=bins(out/'toggle.json')
    if before['bins'].keys()!=after['bins'].keys():raise ValueError('toggle key mismatch')
    merged={k:v+after['bins'][k] for k,v in before['bins'].items()}
    if not all(v>0 for v in merged.values()):raise ValueError('remaining toggle holes')
    summary={'baseline_hit':76,'total':78,'merged_hit':78,'new_samples':2,'merged_closed':True,'bins':merged,
        'baseline_identity':identity,'supplement_inputs':test_identity,'xml_pass':True,
        'scope':'same verified binary and exact bin keys; independent run union, not one global timeline'}
    (out/'summary.json').write_text(json.dumps(summary,indent=2))
    print('Two directed samples: verified baseline76/78 -> merged78/78')
if __name__=='__main__':main()
