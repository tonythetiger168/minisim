import pathlib,subprocess,json
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
from pyslang_lower import Lowerer,cpp_header
out=pathlib.Path(__file__).parent/'lognot-native';out.mkdir(exist_ok=True)
root=pathlib.Path('/mnt/c/Users/chien/OneDrive/Documents/ChatGPT/AI-EDA/minisim')
dut='module dut(input logic clk,a,output logic q); always @(posedge clk) q<=!a; endmodule\n'
(out/'dut.sv').write_text(dut)
model=Lowerer(out/'dut.sv').lower();(out/'design.hpp').write_text(cpp_header(model))
sv='module tb; reg clk=0,a; wire q; dut d(clk,a,q); initial begin '
cpp='#define MINISIM_NO_DEMO\n#include "minisim.cpp"\n#include "design.hpp"\nGeneratedDesign d;Task stim(){S.set(d.signals.at("clk"),mk(0));co_await Delay{1};'
for i,(v,expr) in enumerate([("0","mk(0)"),("1","mk(1)"),("x","allx(1)"),("z","allz(1)")]):
    sv+=f'a=1\'b{v};#1;clk=1;#1;$display("ROW {i} %b",q);clk=0;#1;'
    cpp+=f'S.set(d.signals.at("a"),{expr});co_await Delay{{1}};S.set(d.signals.at("clk"),mk(1));co_await Delay{{1}};auto v{i}=S.get(d.signals.at("q"));std::cout<<"ROW {i} "<<(v{i}.x?"x":v{i}.v?"1":"0")<<"\\n";S.set(d.signals.at("clk"),mk(0));co_await Delay{{1}};'
sv+='$finish;end endmodule';cpp+='S.finished=true;}int main(){d=build_generated_design();for(auto& p:d.programs)S.spawn(exec(&p));S.spawn(stim());S.run(50);}'
(out/'tb.sv').write_text(sv);(out/'harness.cpp').write_text(cpp)
def run(args):return subprocess.check_output(args,cwd=out,text=True,stderr=subprocess.STDOUT,timeout=90)
run(['iverilog','-g2012','-s','tb','-o','ref.vvp','dut.sv','tb.sv']);ref=run(['vvp','ref.vvp'])
run(['g++','-std=c++20','-O0','-I'+str(root/'src'),'harness.cpp','-o','native']);native=run(['./native'])
rows=lambda text:[l for l in text.splitlines() if l.startswith('ROW ')]
assert rows(ref)==rows(native) and len(rows(ref))==4,(ref,native)
(out/'summary.json').write_text(json.dumps({'samples':4,'matched':True,'rows':rows(ref)},indent=2))
print('4/4 native logical-not samples matched Icarus: '+str(rows(ref)))
