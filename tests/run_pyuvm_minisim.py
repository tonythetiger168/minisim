"""Run unchanged existing pyuvm scoreboard on verified Minisim VPI binaries."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
import find_libpython
from cocotb_tools import config
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'build/pyuvm-minisim';out.mkdir(parents=True,exist_ok=True)
results={}
for name in ('clean','increment2'):
    binary=ROOT/'build/cocotb-minisim'/name/'simulator'
    if not binary.is_file():raise RuntimeError('Run tests/run_cocotb_minisim.py first')
    folder=out/name;folder.mkdir(exist_ok=True);xml=folder/'results.xml'
    if xml.exists():xml.unlink()
    env={**os.environ,'MINISIM_COCOTB_VPI':str(config.lib_name_path('vpi','icarus')),
         'GPI_USERS':find_libpython.find_libpython()+';'+config.pygpi_entry_point(),
         'PYGPI_PYTHON_BIN':sys.executable,'PYTHONPATH':os.pathsep.join([str(ROOT/'tests/pyuvm')]+sys.path),
         'COCOTB_TEST_MODULES':'counter_test','COCOTB_TOPLEVEL':'counter','TOPLEVEL_LANG':'verilog',
         'COCOTB_RESULTS_FILE':str(xml),'COCOTB_RANDOM_SEED':'168'}
    with (folder/'simulation.log').open('w') as log:
        process=subprocess.run([str(binary)],cwd=folder,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=90)
    if process.returncode or not xml.exists():raise RuntimeError('pyuvm host/bootstrap failed: '+name)
    tree=ET.parse(xml);failures=tree.findall('.//failure');text=(folder/'simulation.log').read_text()
    if len(tree.findall('.//testcase'))!=1 or tree.findall('.//error') or tree.findall('.//skipped'):
        raise RuntimeError('Missing/skipped pyuvm test')
    if name=='clean':
        metric=re.search(r'Scoreboard PASS: (\d+) samples, (\d+) wraps, (\d+) reset samples',text)
        if failures or not metric or tuple(map(int,metric.groups()))!=(96,3,7):raise RuntimeError('pyuvm clean scoreboard not validated')
        results[name]=dict(samples=96,wraps=3,resets=7,failures=0)
    else:
        evidence='\n'.join(ET.tostring(f,encoding='unicode') for f in failures)
        if not failures or 'counter mismatch sample=2' not in evidence:raise RuntimeError('pyuvm mutant not caught as expected')
        results[name]=dict(expected_failure=True,reason='counter mismatch sample=2',failures=len(failures))
    results[name]['binary_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest()
    print(name,results[name],flush=True)
(out/'summary.json').write_text(json.dumps(dict(results=results,scope='existing pyuvm counter test on experimental Minisim VPI host'),indent=2))
