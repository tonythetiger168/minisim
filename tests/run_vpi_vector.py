from pathlib import Path
import sys,json,subprocess
r=Path(__file__).resolve().parents[1];s=r/'tests';sys.path.insert(0,str(r/'tools'))
from vpi_provenance import verify,capture
base=r/'build/vpi-wide-64';record=verify(base/'provenance.json')
out=r/'build/vpi-vector-64';out.mkdir(parents=True,exist_ok=True)
cases=[(0,0,0,0),(0xffffffff,0,0xffffffff,0),(0,0,0x80000000,0),(0xaaaaaaaa,0xcccccccc,0x55555555,0xf0f0f0f0),(0xffffffff,0xffffffff,0xffffffff,0xffffffff),(0,0xffffffff,0,0xffffffff)]
header=out/'vector_cases.hpp';header.write_text('static const uint32_t vector_cases[][4]={'+','.join('{'+','.join(hex(x)+'u' for x in row)+'}' for row in cases)+'};')
binary=out/'vector-test';cmd=['g++','-std=c++20','-O0','-rdynamic','-I'+str(r/'src'),'-I'+str(s),'-I'+str(base),'-I'+str(out),'-I/usr/include/iverilog',str(s/'vpi_vector_main.cpp'),'-ldl','-o',str(binary)]
with (out/'build.log').open('w') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=120)
inputs=[Path(item['path']) for item in record['inputs']]+[s/'vpi_vector_main.cpp',Path(__file__).resolve(),header]
capture(binary,list(dict.fromkeys(inputs)),out/'provenance.json');verify(out/'provenance.json')
text=subprocess.check_output([str(binary)],text=True,timeout=15);(out/'native.log').write_text(text)
actual=[line[4:] for line in text.splitlines() if line.startswith('ROW ')]
expected=[]
for a0,b0,a1,b1 in cases:
    a=a0|(a1<<32);b=b0|(b1<<32)
    expected.append(''.join(('x' if (a>>i)&1 else 'z') if (b>>i)&1 else ('1' if (a>>i)&1 else '0') for i in reversed(range(64))))
assert actual==expected
sv='module tb;reg[63:0]din;initial begin '
for bits in expected:sv+="din=64'b"+bits+";#1;$display(\"ROW %b\",din);"
sv+='$finish;end endmodule';(out/'tb.sv').write_text(sv)
subprocess.run(['iverilog','-g2012','-o',str(out/'reference.vvp'),str(out/'tb.sv')],check=True,timeout=30)
ref=subprocess.check_output(['vvp',str(out/'reference.vvp')],text=True,timeout=30);(out/'reference.log').write_text(ref)
assert actual==[line[4:] for line in ref.splitlines() if line.startswith('ROW ')]
verify(out/'provenance.json');verify(base/'provenance.json')
(out/'summary.json').write_text(json.dumps({'cases':len(cases),'vector_two_word_roundtrip':True,'matched_icarus_binary':True,'scope':'direct Minisim VPI input aval/bval plus Icarus RTL binary encoding reference; not Icarus VPI plugin test'}))
print('6/6 direct VPI vector two-word roundtrips matched Icarus binary encoding')
