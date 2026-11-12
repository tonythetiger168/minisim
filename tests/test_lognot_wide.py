import pathlib,subprocess,json,itertools
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
from pyslang_lower import Lowerer,cpp_header

def check_bitwise_not(output):
    import hashlib

    root = pathlib.Path(__file__).resolve().parents[1]
    output.mkdir(parents=True, exist_ok=False)
    rtl = output / 'dut.sv'
    rtl.write_text('''module dut(input logic clk, input logic [3:0] a,
 input logic [63:0] wide, output logic [3:0] same,
 output logic [7:0] expanded, output logic [1:0] narrowed,
 output logic [63:0] full);
 always @(posedge clk) begin
   same <= ~a;
   expanded <= ~a;
   narrowed <= ~a;
   full <= ~wide;
 end
endmodule
''')
    header = output / 'design.hpp'
    clean = cpp_header(Lowerer(rtl).lower())
    header.write_text(clean)
    sv = ['module tb; reg clk=0; reg [3:0] a; reg [63:0] wide;',
          'wire [3:0] same; wire [7:0] expanded; wire [1:0] narrowed; wire [63:0] full;',
          'dut d(clk,a,wide,same,expanded,narrowed,full); initial begin']
    cpp = ['#define MINISIM_NO_DEMO', '#include "minisim.cpp"', '#include "design.hpp"',
           'GeneratedDesign d;',
           'void show(const char* name) {',
           '  auto v = S.get(d.signals.at(name));',
           "  for (int bit=v.w-1; bit>=0; --bit) std::cout << (((v.z>>bit)&1) ? 'z' : ((v.x>>bit)&1) ? 'x' : ((v.v>>bit)&1) ? '1' : '0');",
           '}',
           'Task stim() { S.set(d.signals.at("clk"),mk(0)); co_await Delay{1};']
    expected = []
    patterns = ('0'*64, '1'*64, 'x'+'0'*62+'z', '1'+'0'*63, '01xz'*16)
    invert = str.maketrans('01xz', '10xx')
    for i, digits in enumerate(itertools.product('01xz', repeat=4)):
        a = ''.join(digits)
        wide = patterns[i % len(patterns)]
        expected.append(f'ROW {i} {a.translate(invert)} {a.zfill(8).translate(invert)} '
                        f'{a[-2:].translate(invert)} {wide.translate(invert)}')
        sv.append(f"a=4'b{a};wide=64'b{wide};#1;clk=1;#1;"
                  f'$display("ROW {i} %b %b %b %b",same,expanded,narrowed,full);clk=0;#1;')
        for name, bits in [('a', a), ('wide', wide)]:
            value = int(bits.translate(str.maketrans('xz', '00')), 2)
            unknown = int(''.join('1' if c in 'xz' else '0' for c in bits), 2)
            high_z = int(''.join('1' if c == 'z' else '0' for c in bits), 2)
            cpp.append(f'S.set(d.signals.at("{name}"),make({value}ULL,{unknown}ULL,{len(bits)},{high_z}ULL));')
        cpp.extend(['co_await Delay{1}; S.set(d.signals.at("clk"),mk(1)); co_await Delay{1};',
                    f'std::cout << "ROW {i} "; show("same"); std::cout << " "; show("expanded");',
                    'std::cout << " "; show("narrowed"); std::cout << " "; show("full");',
                    'std::cout << "\\n"; S.set(d.signals.at("clk"),mk(0)); co_await Delay{1};'])
    sv.append('$finish; end endmodule')
    cpp.extend(['S.finished=true;}', 'int main(){d=build_generated_design();',
                'for(auto& p:d.programs)S.spawn(exec(&p)); S.spawn(stim()); S.run(1000);}'])
    (output / 'tb.sv').write_text('\n'.join(sv))
    (output / 'harness.cpp').write_text('\n'.join(cpp))

    def run(*args):
        return subprocess.check_output(args, cwd=output, text=True, stderr=subprocess.STDOUT, timeout=90)

    def trace(name, log):
        (output / (name + '.log')).write_text(log)
        return [line for line in log.splitlines() if line.startswith('ROW ')]

    compile_cpp = ('g++', '-std=c++20', '-O0', '-I'+str(root/'src'), 'harness.cpp', '-o')
    run('iverilog', '-g2012', '-s', 'tb', '-o', 'reference.vvp', 'dut.sv', 'tb.sv')
    reference = trace('reference', run('vvp', 'reference.vvp'))
    run(*compile_cpp, 'native')
    native = trace('native', run('./native'))
    if native != expected or reference != expected:
        raise RuntimeError('bitwise complement trace disagrees with the oracle')
    if clean.count('Op::Not,') != 4:
        raise RuntimeError('unexpected bitwise complement mutation sites')
    try:
        header.write_text(clean.replace('Op::Not,', 'Op::LogNot,'))
        run(*compile_cpp, 'mutant')
        mutant = trace('mutant', run('./mutant'))
    finally:
        header.write_text(clean)
    if len(mutant) != len(expected) or [row.split()[:2] for row in mutant] != [row.split()[:2] for row in expected]:
        raise RuntimeError('incomplete mutant trace')
    mismatches = sum(actual != wanted for actual, wanted in zip(mutant, expected))
    if mismatches == 0:
        raise RuntimeError('logical-not substitution escaped oracle')
    result = {'samples': len(expected), 'outputs_per_sample': 4, 'icarus_and_oracle_match': True,
              'mutant_mismatches': mismatches, 'rtl_sha256': hashlib.sha256(rtl.read_bytes()).hexdigest()}
    (output / 'summary.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result))


if __name__ == '__main__' and '--bitwise-not' in sys.argv:
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--bitwise-not', action='store_true')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    check_bitwise_not(parser.parse_args().output.resolve())
    sys.exit(0)

out=pathlib.Path(__file__).resolve().parents[1]/'build/lognot-wide';out.mkdir(parents=True,exist_ok=True)
root=pathlib.Path(__file__).resolve().parents[1]
dut='module dut(input logic clk,input logic [3:0] a,output logic q); always @(posedge clk) q<=!a; endmodule\n'
(out/'dut.sv').write_text(dut)
model=Lowerer(out/'dut.sv').lower();(out/'design.hpp').write_text(cpp_header(model))
sv='module tb; reg clk=0;reg [3:0] a; wire q; dut d(clk,a,q); initial begin '
cpp='#define MINISIM_NO_DEMO\n#include "minisim.cpp"\n#include "design.hpp"\nGeneratedDesign d;Task stim(){S.set(d.signals.at("clk"),mk(0));co_await Delay{1};'
vectors=[]
for bits in itertools.product('01xz',repeat=4):
    value=unknown=z=0
    for bit,c in enumerate(reversed(bits)):
        if c=='1': value |= 1<<bit
        if c in 'xz': unknown |= 1<<bit
        if c=='z': z |= 1<<bit
    vectors.append((''.join(bits),f'make({value},{unknown},4,{z})'))
for i,(v,expr) in enumerate(vectors):
    sv+=f'a=4\'b{v};#1;clk=1;#1;$display("ROW {i} %b",q);clk=0;#1;'
    cpp+=f'S.set(d.signals.at("a"),{expr});co_await Delay{{1}};S.set(d.signals.at("clk"),mk(1));co_await Delay{{1}};auto v{i}=S.get(d.signals.at("q"));std::cout<<"ROW {i} "<<(v{i}.x?"x":v{i}.v?"1":"0")<<"\\n";S.set(d.signals.at("clk"),mk(0));co_await Delay{{1}};'
sv+='$finish;end endmodule';cpp+='S.finished=true;}int main(){d=build_generated_design();for(auto& p:d.programs)S.spawn(exec(&p));S.spawn(stim());S.run(1000);}'
(out/'tb.sv').write_text(sv);(out/'harness.cpp').write_text(cpp)
def run(args):return subprocess.check_output(args,cwd=out,text=True,stderr=subprocess.STDOUT,timeout=90)
run(['iverilog','-g2012','-s','tb','-o','ref.vvp','dut.sv','tb.sv']);ref=run(['vvp','ref.vvp'])
run(['g++','-std=c++20','-O0','-I'+str(root/'src'),'harness.cpp','-o','native']);native=run(['./native'])
rows=lambda text:[l for l in text.splitlines() if l.startswith('ROW ')]
assert rows(ref)==rows(native) and len(rows(ref))==256,(ref,native)
(out/'summary.json').write_text(json.dumps({'samples':256,'matched':True,'rows':rows(ref)},indent=2))
print('256/256 native logical-not samples matched Icarus')

# Deliberately use bitwise-not to prove the same oracle rejects a semantic mutant.
header=(out/'design.hpp').read_text()
assert 'Op::LogNot' in header
(out/'design.hpp').write_text(header.replace('Op::LogNot','Op::Not'))
try:
    run(['g++','-std=c++20','-O0','-I'+str(root/'src'),'harness.cpp','-o','mutant'])
    bad=run(['./mutant'])
    assert rows(bad)!=rows(ref), 'mutant unexpectedly equivalent'
finally:
    (out/'design.hpp').write_text(header)
summary=json.loads((out/'summary.json').read_text());summary['bitwise_not_mutant_rejected']=True
(out/'summary.json').write_text(json.dumps(summary,indent=2))
print('bitwise-not mutant rejected')
