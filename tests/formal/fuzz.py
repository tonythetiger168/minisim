#!/usr/bin/env python3
"""mformal against msim on random designs.

    fuzz.py <build dir> <first seed> <end seed> [work dir]

Each seed makes a small design (random expressions into flops and a memory)
with three assertions. Then:
  * every counterexample mformal finds is replayed in msim with the testbench
    mformal wrote (-tb), and the assertion must fail there at the same time;
  * msim runs 3000 random cycles, and no assertion mformal proved may fail.
The expressions avoid X (no out-of-range selects, no division by zero):
mformal is two-state.
"""
import random, subprocess, sys, os, re
B = os.path.abspath(sys.argv[1]) + '/'
def expr(d, rnd):
    leaves=['a','b','sc','r','4\'d%d'%rnd.randrange(16),'3\'sd%d'%rnd.randrange(4),'%d'%rnd.randrange(20), 'a[2:1]','b[%d]'%rnd.randrange(4),'r[a[1:0]]','{a[1:0],b[3:2]}','$signed(a)','a[b[0]+:2]']
    if d==0 or rnd.random()<0.2: return rnd.choice(leaves)
    k=rnd.randrange(12)
    e=lambda: expr(d-1,rnd)
    if k<5:
        op=rnd.choice(['+','-','*','&','|','^','<<','>>','>>>','/','%'])
        return '(%s %s %s)'%(e(), op, ('(%s | 1)'%e()) if op in '/%' else e())
    if k<7: return '(%s %s %s)'%(e(), rnd.choice(['<','<=','==','!=','>','>=','&&','||','===']), e())
    if k==7: return '(%s ? %s : %s)'%(e(),e(),e())
    if k==8: return '(%s%s)'%(rnd.choice(['~','-','!','&','|','^']), e())
    if k==9: return '{%s, %s}'%(e(),e())
    if k==10: return '$countones(%s)'%e()
    return '$signed(%s)'%e()
def run(seed):
    rnd=random.Random(seed)
    e1=expr(3,rnd); e2=expr(3,rnd); e3=expr(2,rnd)
    K=rnd.randrange(16)
    src=f'''module d(input logic clk, input logic rst, input logic [3:0] a, b, input logic signed [3:0] sc);
  logic [3:0] r; logic [7:0] y; logic [3:0] m [4];
  wire [7:0] w = {e1};
  always_ff @(posedge clk) if (rst) begin r <= 4'd0; y <= 8'd0; end else begin r <= {e3}; y <= w; end
  always_ff @(posedge clk) if (rst) for (int i = 0; i < 4; i++) m[i] <= 4'd0; else if (a[0]) m[b[1:0]] <= r;
  a_x: assert property (@(posedge clk) disable iff (rst) y != 8'd{K});
  a_y: assert property (@(posedge clk) disable iff (rst) ({e2}) != {K} || m[a[3:2]] != 4'd{K%7});
  a_z: assert property (@(posedge clk) disable iff (rst) a == 4'd3 |=> ##[1:2] (r != $past(b,2)));
endmodule
'''
    os.makedirs('w%d'%seed,exist_ok=True); os.chdir('w%d'%seed)
    open('d.sv','w').write(src)
    p=subprocess.run([B+'mformal','d.sv','-top','d','-tb','-depth','8'],capture_output=True,text=True)
    if p.returncode not in (0,1) : 
        if 'error' in p.stderr and 'mformal:' not in p.stderr: os.chdir('..'); return 'skip'
        print('SEED',seed,'rc',p.returncode,p.stderr[:300]); os.chdir('..'); return 'err'
    res={}
    for l in p.stdout.splitlines():
        m=re.match(r'\s+assert\s+d\.(\w+)\s+(\w+)(?:\s+at cycle (\d+))?',l)
        if m: res[m.group(1)]=(m.group(2), m.group(3))
    bad=False
    for n,(v,cyc) in res.items():
        if v=='falsified':
            q=subprocess.run([B+'msim','d.sv','mformal_out/d.%s_tb.sv'%n,'-top','mformal_replay','-Wno-unconnected-output-port'],capture_output=True,text=True)
            t=(int(cyc)+1)*10+5
            if not re.search(r'd\.%s: at time %d000\b'%(n,t), q.stdout+q.stderr):
                print('SEED',seed,n,'CEX not reproduced at',t,(q.stdout+q.stderr)[:300]); bad=True
    # random simulation: nothing proven or bounded may fail
    tb='''module rt; logic clk=0, rst=1; logic [3:0] a,b; logic signed [3:0] sc; d d(.*);
  always #5 clk=~clk;
  initial begin repeat(2) @(negedge clk); rst=0; repeat(3000) begin a=$urandom; b=$urandom; sc=$urandom; @(negedge clk); end $finish; end
  initial begin a=0;b=0;sc=0; end
endmodule'''
    open('rt.sv','w').write(tb)
    q=subprocess.run([B+'msim','d.sv','rt.sv','-top','rt','-Wno-unconnected-output-port'],capture_output=True,text=True)
    out=q.stdout+q.stderr
    for n,(v,cyc) in res.items():
        failed = re.search(r'd\.%s: at time'%n, out) is not None
        if v in('proven',) and failed: print('SEED',seed,n,'PROVEN but fails in simulation'); bad=True
        if v=='bounded' and failed:
            # must fail later than the bound
            t=int(re.search(r'd\.%s: at time (\d+)'%n,out).group(1))//1000
            if t < 8*10: print('SEED',seed,n,'bounded but fails at',t); bad=True
    os.chdir('..')
    if not bad: subprocess.run(['rm','-rf','w%d'%seed])
    return 'bad' if bad else ' '.join(v for v,_ in res.values())
import collections
c=collections.Counter()
if len(sys.argv) > 4:
    os.makedirs(sys.argv[4], exist_ok=True)
    os.chdir(sys.argv[4])
for s in range(int(sys.argv[2]),int(sys.argv[3])):
    r=run(s)
    for w in r.split(): c[w]+=1
print(dict(c))
sys.exit(1 if c['bad'] or c['err'] else 0)
