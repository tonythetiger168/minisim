from pathlib import Path
import tempfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from alu_functional_holes import analyze
r=Path(__file__).resolve().parents[1]/'build'
report=analyze(r/'basanos-pyuvm-vpi/tx.jsonl')
assert report['samples']==500 and report['total_bins']==55 and not report['closed']
assert report['holes'] and report['directed_stimuli']
try:analyze(r/'basanos-pyuvm-mutant/tx.jsonl')
except ValueError as e:assert 'incorrect' in str(e)
else:raise AssertionError('mutant accepted')
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'empty.jsonl';p.write_text('')
    try:analyze(p)
    except ValueError as e:assert 'empty' in str(e)
    else:raise AssertionError('empty accepted')
out=r/'alu-functional-holes.json'
import json
out.write_text(json.dumps(report,indent=2))
print('3/3 checks; '+str(report['hit_bins'])+'/55 bins; '+str(len(report['holes']))+' holes')
