from pathlib import Path
import json,subprocess,sys,hashlib
ROOT=Path('/mnt/c/Users/chien/OneDrive/Documents/ChatGPT/AI-EDA')
if (Path(__file__).resolve().parents[1]/'src/minisim.cpp').exists():ROOT=Path(__file__).resolve().parents[2]
OUT=Path(__file__).parent/'pyuvm-cross-backend'
if Path(__file__).parent.name=='tests':OUT=ROOT/'minisim/build/pyuvm-cross-backend'
OUT.mkdir(parents=True,exist_ok=True)
a=ROOT/'basanos/tx.jsonl';b=ROOT/'minisim/build/basanos-pyuvm-vpi/tx.jsonl'
rows=lambda p:[json.loads(x) for x in p.read_text().splitlines() if x.strip()]
gold=rows(a);actual=rows(b)
assert len(gold)==len(actual)==500
ops=set()
for row in actual:
    fields={k:int(v) for k,v in (x.split('=') for x in row['payload'].split(','))}
    op,x,y=fields['op'],fields['a'],fields['b'];ops.add(op)
    assert fields['result']=={0:0,1:x+y,2:x&y,3:x^y,4:x*y}[op]
assert ops=={0,1,2,3,4}
mutant=[dict(x) for x in actual];mutant[0]['payload']+='1'
missing=actual[:-1];shifted=[{**x,'t':x['t']+1} for x in actual]
def write(name,records):
    p=OUT/(name+'.jsonl');p.write_text(''.join(json.dumps(x)+'\n' for x in records));return p
checks={}
for name,records,expected in [('clean',actual,0),('wrong-value',mutant,1),('missing',missing,1),('shifted-time',shifted,1)]:
    p=write(name,records);out=OUT/name
    proc=subprocess.run([sys.executable,str(ROOT/'helios-verify/scripts/run_dynamis_gate.py'),str(a),str(p),'--dynamis',str(ROOT/'dynamis'),'--output',str(out)],capture_output=True,text=True,timeout=45)
    (OUT/(name+'.log')).write_text(proc.stdout+proc.stderr)
    assert proc.returncode==expected,(name,proc.stdout,proc.stderr)
    checks[name]=json.loads((out/'evidence.json').read_text())['status']
summary={'samples':500,'operations':sorted(ops),'independent_alu_checked':500,'gates':checks,'with_time':True,'sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (a,b)},'limitation':'saved Icarus baseline; not a new Icarus run or general design proof'}
(OUT/'summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary))
