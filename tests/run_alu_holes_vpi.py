"""Fill fixed TinyALU functional bins using the shared real-cocotb lifecycle."""
import argparse,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from alu_functional_holes import analyze
from vpi_build import build
from vpi_runtime import environment
from vpi_provenance import verify
from vpi_results import check_results

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,default=ROOT/'build/alu-holes-vpi')
    parser.add_argument('--baseline',type=Path,default=ROOT/'build/basanos-pyuvm-vpi/tx.jsonl')
    parser.add_argument('--plan',type=Path,default=ROOT/'build/alu-functional-holes.json')
    args=parser.parse_args();baseline=args.baseline.resolve();plan=args.plan.resolve()
    before=analyze(baseline);planned=json.loads(plan.read_text())
    if not before['holes']:raise ValueError('baseline has no functional holes')
    if planned.get('source_sha256')!=before['source_sha256']:raise ValueError('stale holes plan')
    if planned.get('directed_stimuli')!=before['directed_stimuli']:raise ValueError('plan stimuli disagree with fixed-bin analysis')
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    binary=build(ROOT.parent/'basanos/hdl/tinyalu.sv',out,
        [Path(__file__),ROOT/'tests/cocotb_alu_holes.py',ROOT/'tools/alu_functional_holes.py',baseline,plan])
    xml=out/'results.xml';tx=out/'directed.jsonl'
    env=environment('tinyalu','cocotb_alu_holes',[ROOT/'tests'],xml,
        {'MINISIM_HOLES_PLAN':str(plan),'MINISIM_DIRECTED_TX':str(tx)})
    verify(out/'provenance.json')
    with (out/'simulation.log').open('w') as log:
        subprocess.run([str(binary)],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
    verify(out/'provenance.json');check_results(xml)
    directed=analyze(tx)
    if directed['samples']!=len(before['directed_stimuli']):raise ValueError('incomplete directed samples')
    combined=out/'combined.jsonl';combined.write_text(baseline.read_text().rstrip()+'\n'+tx.read_text())
    after=analyze(combined)
    if not after['closed'] or after['total_bins']!=55 or after['samples']!=before['samples']+directed['samples']:
        raise ValueError('fixed functional bins not closed')
    report={'before':before,'directed_samples':directed['samples'],'after':after,'xml_pass':True,'shared_build':True,
        'scope':'transaction functional bins only; independent runs do not share one global timeline'}
    (out/'summary.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({'directed_samples':directed['samples'],'before':before['hit_bins'],'after':after['hit_bins'],'total':55,'closed':after['closed']}))
if __name__=='__main__':main()
