import hashlib, json, pathlib, subprocess, sys
ROOT=pathlib.Path('/mnt/c/Users/chien/OneDrive/Documents/ChatGPT/AI-EDA/minisim')
if (pathlib.Path(__file__).resolve().parents[1]/'src/minisim.cpp').is_file():
    ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from pyslang_lower import Lowerer, cpp_header
OUT=pathlib.Path(__file__).parent/'event-differential'
if pathlib.Path(__file__).resolve().parent.name == 'tests':
    OUT=ROOT/'build/event-differential'
OUT.mkdir(parents=True,exist_ok=True)
def run(args,cwd):
    p=subprocess.run(args,cwd=cwd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=90)
    if p.returncode: raise RuntimeError(str(args)+'\n'+p.stderr)
    return p.stdout
results=[]
for name,event,stimuli in [
 ('async','posedge clk or posedge rst',[(0,1,0),(0,0,0),(1,0,0),(0,0,0),(1,1,0),(0,0,0),(1,0,0),(0,1,0)]),
 ('simultaneous','posedge clk or posedge aux',[(1,1,0),(0,0,0),(1,0,1),(0,0,0),(0,0,1),(0,0,0),(1,0,1),(0,0,0)])]:
    folder=OUT/name;folder.mkdir(exist_ok=True)
    dut='module dut(input logic clk,rst,aux,output logic [3:0] q); always @('+event+') if(rst) q<=0; else q<=q+1; endmodule\n'
    source=folder/'dut.sv';source.write_text(dut)
    model=Lowerer(source).lower();(folder/'design.hpp').write_text(cpp_header(model))
    sv='`timescale 1ns/1ps\nmodule tb; reg clk=0,rst=0,aux=0; wire [3:0] q; dut d(clk,rst,aux,q); initial begin\n'
    cpp='#define MINISIM_NO_DEMO\n#include "minisim.cpp"\n#include "design.hpp"\nGeneratedDesign d; Task stimulus(){\n'
    cpp+='S.set(d.signals.at("clk"),mk(0));S.set(d.signals.at("rst"),mk(0));S.set(d.signals.at("aux"),mk(0));co_await Delay{2};\n'
    sv+='#2;\n'
    for i,(clk,rst,aux) in enumerate(stimuli):
        sv+=f'clk={clk};rst={rst};aux={aux}; #1; $display("ROW {i} %0d",q); #1;\n'
        cpp+=f'S.set(d.signals.at("clk"),mk({clk}));S.set(d.signals.at("rst"),mk({rst}));S.set(d.signals.at("aux"),mk({aux}));co_await Delay{{1}}; auto v{i}=S.get(d.signals.at("q")); if(v{i}.x || v{i}.z) throw std::runtime_error("unknown output");std::cout<<"ROW {i} "<<v{i}.v<<"\\n";co_await Delay{{1}};\n'
    sv+='$finish;end endmodule\n';cpp+='S.finished=true;} int main(){d=build_generated_design();for(auto& p:d.programs)S.spawn(exec(&p));S.spawn(stimulus());S.run(100);}\n'
    (folder/'tb.sv').write_text(sv);(folder/'harness.cpp').write_text(cpp)
    run(['iverilog','-g2012','-s','tb','-o','ref.vvp','dut.sv','tb.sv'],folder)
    ref=run(['vvp','ref.vvp'],folder)
    run(['g++','-std=c++20','-O0','-I'+str(ROOT/'src'),'harness.cpp','-o','native'],folder)
    native=run(['./native'],folder)
    rows=lambda text:[line for line in text.splitlines() if line.startswith('ROW ')]
    assert len(rows(ref))==len(stimuli) and rows(ref)==rows(native),(name,ref,native)
    (folder/'reference.log').write_text(ref);(folder/'native.log').write_text(native)
    results.append({'case':name,'samples':len(stimuli),'matched':True,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()})
(OUT/'summary.json').write_text(json.dumps(results,indent=2))
print(json.dumps(results))
