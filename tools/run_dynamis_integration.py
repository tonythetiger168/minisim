"""Validate real Minisim/Icarus traces through external Dynamis TX tools."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]

def transactions(text):
    lines=text.splitlines()
    if len(lines)!=96:raise ValueError('expected 96 non-empty counter samples')
    rows=[]
    for expected,line in enumerate(lines):
        parts=line.split()
        if len(parts)!=2:raise ValueError('invalid trace row')
        cycle,value=map(int,parts)
        if cycle!=expected or value<0:raise ValueError('unordered/invalid trace')
        rows.append(dict(kind='counter_sample',t=2*cycle+2,payload=f'cycle={cycle},cnt={value}'))
    return rows

def write(path,rows):path.write_text(''.join(json.dumps(r)+'\n' for r in rows))

def main():
    p=argparse.ArgumentParser();p.add_argument('--dynamis',type=Path,default=ROOT.parent/'dynamis')
    p.add_argument('--bridge',type=Path,default=ROOT/'build/python-bridge')
    p.add_argument('--output',type=Path,default=ROOT/'build/dynamis-integration');a=p.parse_args()
    repo=a.dynamis.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    tool=repo/'tx/tools/txdiff.py';viewer=repo/'tx/tools/txview.py'
    if not tool.is_file() or not viewer.is_file():raise RuntimeError('Dynamis TX tools missing')
    summary=json.loads((a.bridge/'summary.json').read_text())
    if summary['total_samples']!=288 or summary['kernel_sha256']!=hashlib.sha256((ROOT/'src/minisim.cpp').read_bytes()).hexdigest():
        raise RuntimeError('Missing/stale bridge evidence; run tests/run_python_bridge.py')
    results={}
    for name in ('clean','width8','increment2'):
        folder=out/name;folder.mkdir(exist_ok=True);native=a.bridge/name/'native.txt';reference=a.bridge/name/'reference.txt'
        rows=transactions(native.read_text());golden=transactions(reference.read_text())
        write(folder/'minisim.jsonl',rows);write(folder/'icarus.jsonl',golden)
        bad=[dict(r) for r in rows];bad[3]['payload']+='9'
        write(folder/'wrong-value.jsonl',bad);write(folder/'missing.jsonl',rows[:-1])
        checks={}
        for candidate,expected in [('minisim',0),('wrong-value',1),('missing',1)]:
            result=subprocess.run([sys.executable,str(tool),str(folder/'icarus.jsonl'),str(folder/(candidate+'.jsonl')),
                                   '--with-time','--json',str(folder/(candidate+'-diff.json'))],capture_output=True,text=True,timeout=30)
            (folder/(candidate+'-diff.log')).write_text(result.stdout+result.stderr)
            if result.returncode!=expected:raise RuntimeError(f'{name}/{candidate}: unexpected diff exit {result.returncode}')
            diff=json.loads((folder/(candidate+'-diff.json')).read_text())['diffs']
            if candidate=='wrong-value' and not any(d['kind']=='fields' for d in diff):raise RuntimeError('Mutant lacks field mismatch')
            if candidate=='missing' and not any(d['kind']=='missing' for d in diff):raise RuntimeError('Missing-row test not detected')
            checks[candidate]=dict(returncode=result.returncode,differences=len(diff))
        view=subprocess.run([sys.executable,str(viewer),str(folder/'minisim.jsonl'),
                             '--vcd',str((a.bridge/name/'dump.vcd').resolve()),'-o',str(folder/'transactions.html')],
                            capture_output=True,text=True,timeout=30)
        (folder/'viewer.log').write_text(view.stdout+view.stderr)
        if view.returncode or not (folder/'transactions.html').is_file():raise RuntimeError('Viewer failed')
        results[name]=dict(samples=len(rows),checks=checks,viewer_generated=True,
                          input_sha256={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in (native,reference)})
    try:transactions('')
    except ValueError:pass
    else:raise RuntimeError('Empty input accepted')
    revision=subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()
    report=dict(dynamis_revision=revision,results=results,comparison_gates=9,empty_input_rejected=True,
                tool_sha256={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in (tool,viewer)},
                scope='TX trace-format integration, not DPI support or merged simulation engines',
                time_contract='t=2*cycle+2 logical sample boundary in counter fixture; no jitter ignored')
    (out/'summary.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
