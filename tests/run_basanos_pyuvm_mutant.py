"""Original pyuvm suite must reject a privately built multiply-as-add RTL."""
from pathlib import Path
import json,sys,subprocess,re,hashlib
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from vpi_build import build
from vpi_runtime import environment
from vpi_results import check_results
from vpi_provenance import verify

def main():
    basanos=ROOT.parent/'basanos';out=ROOT/'build/basanos-pyuvm-mutant';out.mkdir(parents=True,exist_ok=True)
    original=basanos/'hdl/tinyalu.sv';raw=original.read_bytes();text=raw.decode('utf-8')
    if text.count('a * b')!=1:raise ValueError('unexpected multiplier source')
    mutant=out/'mutant.sv';mutant.write_text(text.replace('a * b','a + b'))
    mutation={'name':'multiply-as-add','original_sha256':hashlib.sha256(raw).hexdigest(),'mutant_sha256':hashlib.sha256(mutant.read_bytes()).hexdigest()}
    (out/'mutation.json').write_text(json.dumps(mutation,indent=2))
    if hashlib.sha256(original.read_bytes()).hexdigest()!=mutation['original_sha256']:raise RuntimeError('original RTL changed during mutant creation')
    binary=build(mutant,out,[basanos/'testbench.py',Path(__file__),original,out/'mutation.json'])
    xml=out/'results.xml';tx=out/'tx.jsonl'
    for path in (xml,tx):
        if path.exists():path.unlink()
    env=environment('tinyalu','testbench',[basanos],xml,{'SEED':'0'})
    verify(out/'provenance.json')
    with (out/'simulation.log').open('w') as log:subprocess.run([str(binary)],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
    verify(out/'provenance.json');check_results(xml,expected_failure='mismatches!')
    match=re.search(r'SCOREBOARD: (\d+) checked, (\d+) errors',(out/'simulation.log').read_text())
    if not match or int(match[1])!=500 or int(match[2])<=0:raise RuntimeError('expected complete failing scoreboard')
    rows=[json.loads(line) for line in tx.read_text().splitlines() if line.strip()]
    if len(rows)!=500:raise RuntimeError('incomplete mutant transactions')
    report={'suite':'original basanos/testbench.py','mutant':'multiply-as-add','checked':500,'scoreboard_errors':int(match[2]),'expected_scoreboard_failure':True,'xml_failures':1,'transactions':500,'shared_build':True,'mutation':mutation}
    (out/'summary.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
if __name__=='__main__':main()
