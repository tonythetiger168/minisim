"""Original Basanos pyuvm suite using a self-contained shared VPI build."""
from pathlib import Path
import sys,json,subprocess
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from vpi_build import build
from vpi_runtime import environment
from vpi_provenance import verify
from vpi_results import check_results

def main():
    basanos=ROOT.parent/'basanos';out=ROOT/'build/basanos-pyuvm-vpi'
    binary=build(basanos/'hdl/tinyalu.sv',out,[basanos/'testbench.py',Path(__file__)])
    xml=out/'results.xml';transactions=out/'tx.jsonl'
    for path in (xml,transactions):
        if path.exists():path.unlink()
    env=environment('tinyalu','testbench',[basanos],xml,{'SEED':'0'})
    verify(out/'provenance.json')
    with (out/'simulation.log').open('w') as log:
        subprocess.run([str(binary)],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
    verify(out/'provenance.json');check_results(xml)
    log=(out/'simulation.log').read_text()
    if 'SCOREBOARD: 500 checked, 0 errors' not in log:raise RuntimeError('missing complete clean scoreboard')
    rows=[json.loads(line) for line in transactions.read_text().splitlines() if line.strip()]
    if len(rows)!=500 or any(row.get('kind')!='sample' for row in rows):raise RuntimeError('invalid/incomplete transactions')
    report={'suite':'original basanos/testbench.py','scoreboard_checked':500,'scoreboard_errors':0,'transactions':500,'xml_pass':True,'shared_build':True,'scope':'experimental flat TinyALU Minisim VPI host'}
    (out/'summary.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
if __name__=='__main__':main()
