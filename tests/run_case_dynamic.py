import pathlib,sys,subprocess,itertools,json
ROOT=pathlib.Path('/mnt/c/Users/chien/OneDrive/Documents/ChatGPT/AI-EDA/minisim')
if (pathlib.Path(__file__).resolve().parents[1]/'src/minisim.cpp').exists():ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from pyslang_lower import Lowerer,cpp_header
OUT=pathlib.Path(__file__).parent/'case-dynamic'
if pathlib.Path(__file__).parent.name=='tests':OUT=ROOT/'build/case-dynamic'
OUT.mkdir(parents=True,exist_ok=True)
results=[]
for name,body in [('default_first',"case(a) default:q<=0;0,1:q<=1;2:q<=0;endcase"),('hold',"case(a) 0,1:q<=1;2:q<=0;endcase")]:
    out=OUT/name;out.mkdir(exist_ok=True)
    (out/'dut.sv').write_text('module dut(input logic clk,input logic[1:0]a,output logic q);always @(posedge clk) '+body+' endmodule')
    (out/'design.hpp').write_text(cpp_header(Lowerer(out/'dut.sv').lower()))
    sv='module tb;reg clk=0;reg[1:0]a;wire q;dut d(clk,a,q);initial begin '
    cpp='#define MINISIM_NO_DEMO\n#include "minisim.cpp"\n#include "design.hpp"\nGeneratedDesign d;Task stim(){S.set(d.signals.at("clk"),mk(0));co_await Delay{1};'
    vectors=['00']+[''.join(x) for x in itertools.product('01xz',repeat=2)]
    for i,bits in enumerate(vectors):
        v=x=z=0
        for bit,c in enumerate(reversed(bits)):
            if c=='1':v|=1<<bit
            if c in 'xz':x|=1<<bit
            if c=='z':z|=1<<bit
        sv+=f"a=2'b{bits};#1;clk=1;#1;$display(\"ROW {i} %b\",q);clk=0;#1;"
        cpp+=f'S.set(d.signals.at("a"),make({v},{x},2,{z}));co_await Delay{{1}};S.set(d.signals.at("clk"),mk(1));co_await Delay{{1}};{{auto q=S.get(d.signals.at("q"));std::cout<<"ROW {i} "<<(q.x?"x":q.v?"1":"0")<<"\\n";}}S.set(d.signals.at("clk"),mk(0));co_await Delay{{1}};'
    sv+='$finish;end endmodule';cpp+='S.finished=true;}int main(){d=build_generated_design();for(auto&p:d.programs)S.spawn(exec(&p));S.spawn(stim());S.run(200);}'
    (out/'tb.sv').write_text(sv);(out/'harness.cpp').write_text(cpp)
    def run(args):return subprocess.check_output(args,cwd=out,text=True,stderr=subprocess.STDOUT,timeout=90)
    run(['iverilog','-g2012','-s','tb','-o','reference.vvp','dut.sv','tb.sv']);ref=run(['vvp','reference.vvp'])
    run(['g++','-std=c++20','-O0','-I'+str(ROOT/'src'),'harness.cpp','-o','native']);native=run(['./native'])
    rows=lambda text:[l for l in text.splitlines() if l.startswith('ROW ')]
    assert rows(ref)==rows(native) and len(rows(ref))==17,(name,ref,native)
    (out/'reference.log').write_text(ref);(out/'native.log').write_text(native)
    results.append({'case':name,'samples':17,'matched':True})
(OUT/'summary.json').write_text(json.dumps(results,indent=2));print(json.dumps(results))
