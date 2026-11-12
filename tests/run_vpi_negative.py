from pathlib import Path
import sys,subprocess,json
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from vpi_build import build
def main():
    out=ROOT/'build/vpi-interface-negative'
    build(ROOT.parent/'basanos/hdl/tinyalu.sv',out,[ROOT/'tests/vpi_negative_main.cpp'])
    subprocess.run(['g++','-std=c++20','-O0','-rdynamic','-I'+str(ROOT/'src'),'-I'+str(ROOT/'tests'),'-I'+str(out),'-I/usr/include/iverilog',str(ROOT/'tests/vpi_negative_main.cpp'),'-ldl','-o',str(out/'negative')],check=True,timeout=120)
    expected={'output':'write requires input signal','delay':'only zero-delay deposit','force':'only zero-delay deposit','bad-binary':'invalid binary value','index':'indexed handles','read-format':'read format','missing-name':None,'null-vector':'null vector value','module-read':'read requires signal or constant'}
    results={}
    for name,message in expected.items():
        p=subprocess.run([str(out/'negative'),name],capture_output=True,text=True,timeout=15)
        (out/(name+'.log')).write_text(p.stdout+p.stderr)
        if p.returncode or (message is not None and message not in p.stdout):raise RuntimeError('negative case failed: '+name)
        results[name]='rejected' if message else 'null lookup accepted'
    (out/'summary.json').write_text(json.dumps(results,indent=2));print(json.dumps(results))
if __name__=='__main__':main()
