import pathlib,subprocess,json,random,hashlib,sys
ROOT=pathlib.Path('/mnt/c/Users/chien/OneDrive/Documents/ChatGPT/AI-EDA')
if (pathlib.Path(__file__).resolve().parents[1]/'src/minisim.cpp').exists():
    ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'minisim/tools'))
if (pathlib.Path(__file__).parent/'pyslang_lower.py').exists():
    sys.path.insert(0,str(pathlib.Path(__file__).parent))
from pyslang_lower import Lowerer,cpp_header
OUT=pathlib.Path(__file__).parent/'tinyalu-native'
if pathlib.Path(__file__).parent.name=='tests': OUT=ROOT/'minisim/build/tinyalu-native'
OUT.mkdir(parents=True,exist_ok=True)
source=ROOT/'basanos/hdl/tinyalu.sv'
(OUT/'dut.sv').write_bytes(source.read_bytes())
model=Lowerer(OUT/'dut.sv').lower();header=cpp_header(model);(OUT/'design.hpp').write_text(header)
rng=random.Random(168)
vectors=[(0,0,0,0,0)]
for op in range(8):
    for a,b in [(0,0),(255,1),(255,255),(128,128)]:vectors.append((1,1,a,b,op))
vectors += [(int(i%37!=0),int(i%9!=0),rng.randrange(256),rng.randrange(256),rng.randrange(8)) for i in range(500)]
vectors += [(1,1,255,255,'x'),(1,1,255,255,'z')]
sv='`timescale 1ns/1ps\nmodule tb;reg clk=0,reset_n=1,start=0;reg[7:0]a=0,b=0;reg[2:0]op=0;wire[15:0]result;wire done;tinyalu d(clk,reset_n,start,a,b,op,result,done);initial begin #1;'
cpp='#define MINISIM_NO_DEMO\n#include "minisim.cpp"\n#include "design.hpp"\nGeneratedDesign d;void setv(const char*n,Val v){S.set(d.signals.at(n),v);}Task stim(){setv("clk",mk(0));setv("reset_n",mk(1));co_await Delay{1};'
expected=[];result=0
for i,(reset,start,a,b,op) in enumerate(vectors):
    literal=f"3'b{op*3}" if isinstance(op,str) else f"3'd{op}"
    value='allx(3)' if op=='x' else 'allz(3)' if op=='z' else f'mk({op},3)'
    sv+=f'clk=0;reset_n={reset};start={start};a={a};b={b};op={literal};#1;clk=1;#1;$display("ROW {i} %0d %0d",result,done);'
    cpp+=f'setv("clk",mk(0));setv("reset_n",mk({reset}));setv("start",mk({start}));setv("a",mk({a},8));setv("b",mk({b},8));setv("op",{value});co_await Delay{{1}};setv("clk",mk(1));co_await Delay{{1}};{{auto q=S.get(d.signals.at("result"));auto done=S.get(d.signals.at("done"));if(q.x||done.x)throw std::runtime_error("unknown output");std::cout<<"ROW {i} "<<q.v<<" "<<done.v<<"\\n";}}'
    if not reset:result=0
    elif start:result={1:a+b,2:a&b,3:a^b,4:a*b}.get(op,0)&65535
    expected.append(f'ROW {i} {result} {int(bool(reset and start))}')
sv+='$finish;end endmodule';cpp+='S.finished=true;}int main(){d=build_generated_design();for(auto&p:d.programs)S.spawn(exec(&p));S.spawn(stim());S.run(2000);}'
(OUT/'tb.sv').write_text(sv);(OUT/'harness.cpp').write_text(cpp)
def run(args):
    p=subprocess.run(args,cwd=OUT,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=120)
    if p.returncode:raise RuntimeError(str(args)+'\n'+p.stderr)
    return p.stdout
rows=lambda text:[l for l in text.splitlines() if l.startswith('ROW ')]
run(['iverilog','-g2012','-s','tb','-o','reference.vvp','dut.sv','tb.sv']);ref=run(['vvp','reference.vvp'])
compile=lambda name:run(['g++','-std=c++20','-O0','-I'+str(ROOT/'minisim/src'),'harness.cpp','-o',name])
compile('native');native=run(['./native'])
assert rows(ref)==expected, 'Icarus disagrees with independent oracle'
assert rows(native)==expected, 'Minisim disagrees with Icarus/oracle'
mutants=[]
try:
    for name,old,new in [('mul_as_add','Op::Mul','Op::Add'),('case_mismatch','Op::CaseEq','Op::Ne')]:
        assert old in header
        (OUT/'design.hpp').write_text(header.replace(old,new));compile(name)
        bad=run(['./'+name]);assert rows(bad)!=expected,name
        mutants.append(name)
finally:(OUT/'design.hpp').write_text(header)
(OUT/'reference.log').write_text(ref);(OUT/'native.log').write_text(native)
summary={'samples':len(vectors),'matched_icarus':True,'matched_independent_oracle':True,'mutants_rejected':mutants,'rtl_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'frontend_sha256':hashlib.sha256(pathlib.Path(sys.modules[Lowerer.__module__].__file__).read_bytes()).hexdigest(),'scope':'direct generated native DUT harness; not cocotb/pyuvm VPI integration'}
(OUT/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
