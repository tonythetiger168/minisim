"""Run the TinyALU multiply-as-add DUT mutant through shared build/runtime guards."""
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
    parser.add_argument('--output',type=Path,default=ROOT/'build/alu-vpi-mutant')
    out=parser.parse_args().output.resolve()
    out.mkdir(parents=True,exist_ok=True)
    source=ROOT.parent/'basanos/hdl/tinyalu.sv'
    original=source.read_text()
    target='MUL: result <= a * b;'
    if original.count(target)!=1:raise RuntimeError('ambiguous or missing mutation site')
    mutant=out/'tinyalu_mutant.sv'
    mutant.write_text(original.replace(target,'MUL: result <= a + b;'))
    binary=build(mutant,out,[source,Path(__file__),ROOT/'tests/cocotb_tinyalu_native.py'])
    xml=out/'results.xml';trace=out/'trace.json'
    for path in (xml,trace):
        if path.exists():path.unlink()
    env=environment('tinyalu','cocotb_tinyalu_native',[ROOT/'tests'],xml,
                    {'MINISIM_TRACE_PATH':str(trace)})
    verify(out/'provenance.json')
    with (out/'simulation.log').open('w') as log:
        subprocess.run([str(binary)],cwd=out,env=env,stdout=log,
                       stderr=subprocess.STDOUT,check=True,timeout=90)
    verify(out/'provenance.json');check_results(xml,expected_failure='ALU mismatch sample')
    (out/'summary.json').write_text(json.dumps({'mutant':'multiply-as-add',
        'mutation_layer':'RTL source copy; historical generated-header mutant evidence remains separate',
        'expected_scoreboard_failure':True,'xml_failures':1,'shared_build':True,
        'scope':'real cocotb flat TinyALU VPI DUT mutant'},indent=2))
    print('Real cocotb VPI RTL multiplier mutant rejected by ALU scoreboard; shared build')
if __name__=='__main__':main()
