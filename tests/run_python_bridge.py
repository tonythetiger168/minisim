"""Drive real lowered RTL via ctypes; validate settled NBA against Icarus."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from pyslang_lower import Lowerer,cpp_header

def run(cmd,cwd):
    return subprocess.check_output(cmd,cwd=cwd,text=True,stderr=subprocess.STDOUT,timeout=120)

def reset(i):
    return 'x' if i==20 else 'z' if i==50 else str(int(i in (0,37,38,58)))

def callback_worker(folder):
    lib=C.CDLL(str(folder/'model.so'));u=C.c_uint64;cb_type=C.CFUNCTYPE(None,u)
    lib.ms_error.restype=C.c_char_p;lib.ms_time.restype=u
    lib.ms_write.argtypes=[C.c_char_p,u,u,u];lib.ms_advance.argtypes=[u]
    lib.ms_read.argtypes=[C.c_char_p,C.POINTER(u),C.POINTER(u),C.POINTER(u)]
    lib.ms_callback.argtypes=[C.c_int,C.c_char_p,u,C.c_int,cb_type,u,C.POINTER(u)]
    lib.ms_cancel.argtypes=[u]
    keep=[];errors=[];events=[];samples=[]
    def ok(rc):
        if rc: raise RuntimeError(lib.ms_error().decode())
    def read():
        v,x,z=u(),u(),u();ok(lib.ms_read(b'cnt',C.byref(v),C.byref(x),C.byref(z)))
        return v.value,x.value,z.value
    def register(kind,fn,delay=0,signal=None,edge=0):
        @cb_type
        def invoke(token):
            try: fn()
            except BaseException as e: errors.append(repr(e))
        keep.append(invoke);handle=u()
        ok(lib.ms_callback(kind,signal,delay,edge,invoke,0,C.byref(handle)))
        return handle.value
    ok(lib.ms_init());ok(lib.ms_write(b'clk',0,0,0));ok(lib.ms_write(b'rst',1,0,0))
    canceled=register(0,lambda:errors.append('canceled timer fired'),delay=1)
    assert lib.ms_cancel(canceled)==1 and lib.ms_cancel(canceled)==0
    invalid=u()
    assert lib.ms_callback(1,b'clk',0,99,keep[0],0,C.byref(invalid))==-1
    for i in range(4):
        def rise(i=i):
            events.append(['timer',int(lib.ms_time())]);before=read()
            def edge():
                events.append(['edge',int(lib.ms_time())]);assert read()==before
            register(1,edge,signal=b'clk',edge=1)
            ok(lib.ms_write(b'rst',int(i==0),0,0));ok(lib.ms_write(b'clk',1,0,0))
            def rw():
                events.append(['rw',int(lib.ms_time())]);assert read()==(i,0,0)
            register(2,rw)
            def ro():
                events.append(['ro',int(lib.ms_time())]);samples.append(read()[0]);assert read()==(i,0,0)
                assert lib.ms_write(b'clk',0,0,0)==-1
                # Capture registration failure explicitly, never swallow ctypes exceptions.
                handle=u();assert lib.ms_callback(2,None,0,0,keep[0],0,C.byref(handle))==-1
                assert lib.ms_advance(lib.ms_time())==-1
            register(3,ro)
        register(0,rise,delay=2*i+1)
        register(0,lambda:ok(lib.ms_write(b'clk',0,0,0)),delay=2*i+2)
    ok(lib.ms_advance(8))
    assert not errors,errors
    assert samples==[0,1,2,3]
    assert events==[[phase,2*i+1] for i in range(4) for phase in ('timer','edge','rw','ro')]
    (folder/'callbacks.json').write_text(json.dumps({'events':events,'samples':samples,'errors':errors,
        'scope':'native callbacks via ctypes, not cocotb triggers'},indent=2))

def worker(folder):
    lib=C.CDLL(str(folder/'model.so'))
    u=C.c_uint64
    lib.ms_write.argtypes=[C.c_char_p,u,u,u]
    lib.ms_read.argtypes=[C.c_char_p,C.POINTER(u),C.POINTER(u),C.POINTER(u)]
    lib.ms_advance.argtypes=[u];lib.ms_time.restype=u;lib.ms_error.restype=C.c_char_p
    def ok(rc):
        if rc: raise RuntimeError(lib.ms_error().decode())
    def read(name):
        v,x,z=u(),u(),u();ok(lib.ms_read(name.encode(),C.byref(v),C.byref(x),C.byref(z)))
        return v.value,x.value,z.value
    ok(lib.ms_init())
    assert read('cnt')[1] != 0
    assert lib.ms_init() == -1
    assert lib.ms_write(b'cnt',0,0,0) == -1
    try: read('missing')
    except RuntimeError: pass
    else: raise AssertionError('missing signal accepted')
    rows=[]
    for i in range(96):
        ok(lib.ms_write(b'clk',0,0,0))
        r=reset(i);v,x,z=(0,1,int(r=='z')) if r in 'xz' else (int(r),0,0)
        ok(lib.ms_write(b'rst',v,x,z)); assert read('rst') == (v,x,z)
        ok(lib.ms_advance(2*i+1)); before=read('cnt')
        ok(lib.ms_write(b'clk',1,0,0));assert read('cnt')==before
        ok(lib.ms_advance(2*i+1)) # same-time external event + NBA must settle
        value=read('cnt'); assert value[1:] == (0,0)
        rows.append(f'{i} {value[0]}')
        ok(lib.ms_advance(2*i+2));assert lib.ms_time()==2*i+2
    assert lib.ms_advance(0)==-1 and lib.ms_time()==192
    wave=Path('dump.vcd').read_text()
    assert wave.count('$enddefinitions')==1
    (folder/'native.txt').write_text('\n'.join(rows)+'\n')

def main():
    p=argparse.ArgumentParser();p.add_argument('--worker',type=Path);p.add_argument('--callback-worker',type=Path)
    p.add_argument('--output',type=Path,default=ROOT/'build/python-bridge');a=p.parse_args()
    if a.worker: return worker(a.worker.resolve())
    if a.callback_worker: return callback_worker(a.callback_worker.resolve())
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    original=(ROOT/'examples/counter.sv').read_text();traces={};cases={}
    for name,text in [('clean',original),('width8',original.replace('[3:0]','[7:0]')),
                      ('increment2',original.replace('cnt + 1','cnt + 2'))]:
        folder=out/name;folder.mkdir(exist_ok=True)
        source=folder/'input.sv';source.write_text(text)
        model=Lowerer(source).lower()
        (folder/'design.hpp').write_text(cpp_header(model))
        (folder/'design.json').write_text(json.dumps(model,indent=2))
        run(['g++','-std=c++20','-O2','-shared','-fPIC','-I',str(ROOT/'src'),'-I',str(folder),
             str(ROOT/'tests/python_counter_bridge.cpp'),'-o',str(folder/'model.so')],folder)
        run([sys.executable,str(Path(__file__).resolve()),'--worker',str(folder)],folder)
        width=model['signals']['cnt']
        tb=[f'module tb; reg clk=0,rst; wire [{width-1}:0] cnt; counter dut(clk,rst,cnt); initial begin']
        for i in range(96):
            tb.append(f"rst=1'b{reset(i)}; #1 clk=1; #1 $display(\"{i} %0d\",cnt); clk=0;")
        tb.append('end endmodule');(folder/'tb.sv').write_text('\n'.join(tb))
        run(['iverilog','-g2012','-s','tb','-o','ref.vvp',str(source),'tb.sv'],folder)
        ref=run(['vvp','ref.vvp'],folder);(folder/'reference.txt').write_text(ref)
        native=(folder/'native.txt').read_text();assert native.splitlines()==ref.splitlines()
        traces[name]=native
        cases[name]={'samples':96,'match':True,'rtl_sha256':hashlib.sha256(text.encode()).hexdigest()}
        print(name+': 96 external-drive samples match Icarus')
    assert traces['increment2']!=traces['clean'] and traces['width8']!=traces['clean']
    run([sys.executable,str(Path(__file__).resolve()),'--callback-worker',str(out/'clean')],out/'clean')
    report={'cases':cases,'total_samples':288,'scope':'ctypes counter fixture, not cocotb/GPI or pyuvm integration',
            'checks':['same-time NBA settling','pre-settle value','X/Z readback','idle time advance',
                      'backwards time rejected','read-only output','unknown signal','single VCD header'],
            'versions':{'iverilog':run(['iverilog','-V'],out).splitlines()[0]},
            'kernel_sha256':hashlib.sha256((ROOT/'src/minisim.cpp').read_bytes()).hexdigest()}
    report['callback_samples']=4
    report['callback_order']=['timer','edge (pre downstream)','ReadWrite (settled)','ReadOnly (write forbidden)']
    (out/'summary.json').write_text(json.dumps(report,indent=2))

if __name__=='__main__': main()
