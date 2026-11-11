#!/usr/bin/env python3
"""Random ECO cases for meco: a random netlist, a mutated and re-rendered copy
of it, meco at two thread counts (same output required), and a simulation of
the patched netlist against the new one in msim."""
import random, subprocess, sys, os, copy, re
MECO, MSIM, LIBS, CELLS = sys.argv[1], sys.argv[2], sys.argv[3].split(','), sys.argv[4]
N0, N1 = int(sys.argv[5]), int(sys.argv[6])
SCALE = int(os.environ.get('SCALE', '1'))
SPARE = os.environ.get('SPARE') == '1'  # metal-only: spare cells in the old netlist, meco -spare
out = sys.argv[7]
os.makedirs(out, exist_ok=True)
G2 = ['NAND2_X1', 'NOR2_X1', 'AND2_X1', 'OR2_X1', 'XOR2_X1']

def gen(r):
    d = dict(ins=['a%d' % i for i in range(r.randint(2, 6) * (1 + SCALE // 8))], gates=[], flops=[], outs=[])
    nf = r.randint(0, 5) * SCALE
    for i in range(nf):
        d['flops'].append(dict(name='r%d' % i, q='q%d' % i, d=None, rst=r.random() < .6, en=None, qn=r.random() < .3))
    srcs = d['ins'] + [f['q'] for f in d['flops']]
    for i in range(r.randint(3, 30) * SCALE):
        t = r.choice(G2 + ['INV_X1', 'MUX2_X1', 'AOI21_X1', 'BUF_X1'])
        n = 1 if t in ('INV_X1', 'BUF_X1') else 3 if t in ('MUX2_X1', 'AOI21_X1') else 2
        d['gates'].append(dict(out='n%d' % i, t=t, ins=[r.choice(srcs[-12:] if r.random() < .7 else srcs) for _ in range(n)]))
        srcs.append('n%d' % i)
    for f in d['flops']:
        f['d'] = r.choice(srcs)
    for i in range(r.randint(1, 4) * (1 + SCALE // 4)):
        d['outs'].append(['y%d' % i, r.choice(srcs)])
    return d

def nets(d):
    return d['ins'] + [f['q'] for f in d['flops']] + [g['out'] for g in d['gates']]

def mutate(r, d):
    d = copy.deepcopy(d)
    for _ in range(r.randint(0, 4)):
        k = r.randint(0, 9)
        if k == 0 and d['gates']:
            g = r.choice(d['gates'])
            if g['t'] in G2: g['t'] = r.choice(G2)
        elif k == 1 and d['gates']:
            i = r.randrange(len(d['gates'])); g = d['gates'][i]
            avail = d['ins'] + [f['q'] for f in d['flops']] + [x['out'] for x in d['gates'][:i]]
            g['ins'][r.randrange(len(g['ins']))] = r.choice(avail)
        elif k == 2 and d['flops']:
            r.choice(d['flops'])['d'] = r.choice(nets(d))
        elif k == 3:
            r.choice(d['outs'])[1] = r.choice(nets(d))
        elif k == 4:
            i = len(d['flops']) + 10
            while any(f['name'] == 'r%d' % i for f in d['flops']): i += 1
            d['flops'].append(dict(name='r%d' % i, q='q%d' % i, d=r.choice(nets(d)), rst=r.random() < .5, en=None, qn=False))
            r.choice(d['outs'])[1] = 'q%d' % i
        elif k == 5 and d['flops']:
            f = d['flops'].pop(r.randrange(len(d['flops'])))
            rep = r.choice(d['ins'])
            for g in d['gates']: g['ins'] = [rep if x == f['q'] else x for x in g['ins']]
            for g in d['flops']:
                if g['d'] == f['q']: g['d'] = rep
                if g['en'] == f['q']: g['en'] = rep
            for o in d['outs']:
                if o[1] == f['q']: o[1] = rep
        elif k == 6:
            d['outs'].append(['w%d' % len(d['outs']), r.choice(nets(d))])
        elif k == 7:
            n = 'b%d' % len(d['ins']); d['ins'].append(n)
            if d['gates']:
                g = r.choice(d['gates']); g['ins'][0] = n
            else: d['outs'][0][1] = n
        elif k == 8 and d['flops']:
            f = r.choice(d['flops']); f['en'] = None if f['en'] else r.choice(d['ins']); f['rst'] = False
        elif k == 9 and d['flops']:
            f = r.choice(d['flops']); f['rst'] = not f['rst']; f['en'] = None
    return d

def render(r, d, style):
    """style 'old': scan flops, clock buffer, aliases; 'new': fresh names, swapped pins."""
    L = []
    old = style == 'old'
    ports = ['clk', 'rst_n'] + (['scan_en', 'scan_in', 'scan_out'] if old else [])
    bus = old_bus = [x for x in d['ins'] if x.startswith('a')]
    ports += ['a'] + [x for x in d['ins'] if not x.startswith('a')] + [o[0] for o in d['outs']]
    L.append('module top (%s);' % ', '.join(ports))
    L.append('  input clk, rst_n;')
    if old: L.append('  input scan_en, scan_in;\n  output scan_out;')
    L.append('  input [%d:0] a;' % (len(bus) - 1))
    for x in d['ins']:
        if not x.startswith('a'): L.append('  input %s;' % x)
    for o in d['outs']: L.append('  output %s;' % o[0])
    ren = {}
    def nm(n):
        if n.startswith('a'): return 'a[%s]' % n[1:]
        if n in d['ins']: return n
        if n not in ren: ren[n] = n if old else ('\\m%d$x ' % len(ren) if r.random() < .2 else 'm%d' % len(ren))
        return ren[n]
    body = []
    ck = 'clk'
    if old and r.random() < .7:
        body.append('  BUF_X1 ckbuf (.A(clk), .Y(cclk));'); ck = 'cclk'
    cnt = [0]
    def inst(): cnt[0] += 1; return ('u%d' if old else 'g%d') % cnt[0]
    extra = 0
    for g in d['gates']:
        ins = [nm(x) for x in g['ins']]; t = g['t']; o = nm(g['out'])
        if t in G2:
            if not old and r.random() < .5: ins.reverse()
            if not old and t == 'AND2_X1' and r.random() < .4:
                extra += 1; x = 'xx%d' % extra
                body.append('  NAND2_X1 %s (.A(%s), .B(%s), .Y(%s));' % (inst(), ins[0], ins[1], x))
                body.append('  INV_X1 %s (.A(%s), .Y(%s));' % (inst(), x, o))
            else:
                body.append('  %s %s (.A(%s), .B(%s), .Y(%s));' % (t, inst(), ins[0], ins[1], o))
        elif t in ('INV_X1', 'BUF_X1'):
            body.append('  %s %s (.A(%s), .Y(%s));' % (t, inst(), ins[0], o))
        elif t == 'MUX2_X1':
            body.append('  MUX2_X1 %s (.A(%s), .B(%s), .S(%s), .Y(%s));' % (inst(), *ins, o))
        else:
            body.append('  AOI21_X1 %s (.A1(%s), .A2(%s), .B(%s), .Y(%s));' % (inst(), *ins, o))
    si = 'scan_in'
    for f in d['flops']:
        q, dd = nm(f['q']), nm(f['d'])
        if f['en']:
            body.append('  DFFE_X1 %s (.CK(%s), .D(%s), .E(%s), .Q(%s));' % (f['name'], ck, dd, nm(f['en']), q))
        elif old and r.random() < .7:
            if f['rst']: body.append('  SDFFR_X1 %s (.CK(%s), .D(%s), .SI(%s), .SE(scan_en), .RN(rst_n), .Q(%s));' % (f['name'], ck, dd, si, q))
            else: body.append('  SDFF_X1 %s (.CK(%s), .D(%s), .SI(%s), .SE(scan_en), .Q(%s));' % (f['name'], ck, dd, si, q))
            si = q
        elif f['rst']:
            body.append('  DFFR_X1 %s (.CK(%s), .D(%s), .RN(rst_n), .Q(%s));' % (f['name'], ck, dd, q))
        elif f['qn'] and old:
            # the old netlist keeps the complement: Q is rebuilt from QN
            body.append('  DFFQN_X1 %s (.CK(%s), .D(%s), .QN(%s_n));' % (f['name'], ck, dd, f['q']))
            body.append('  INV_X1 %s (.A(%s_n), .Y(%s));' % (inst(), f['q'], q))
        elif f['qn']:
            body.append('  DFFQN_X1 %s (.CK(%s), .D(%s), .Q(%s));' % (f['name'], ck, dd, q))
        else:
            body.append('  DFF_X1 %s (.CK(%s), .D(%s), .Q(%s));' % (f['name'], ck, dd, q))
    if old:
        body.append('  assign scan_out = %s;' % si)
        body.append('  NOR2_X1 spare0 (.A(1\'b0), .B(1\'b0), .Y(spare_n));')
        if SPARE:
            for i in range(r.randint(0, 6)):
                t = r.choice(G2 + ['INV_X1', 'MUX2_X1', 'AOI21_X1'])
                pins = {'INV_X1': ['A'], 'MUX2_X1': ['A', 'B', 'S'], 'AOI21_X1': ['A1', 'A2', 'B']}.get(t, ['A', 'B'])
                body.append('  %s spare_%d (%s, .Y());' % (t, i + 1, ', '.join(".%s(1'b0)" % x for x in pins)))
    seen = {}
    for o in d['outs']:
        src = nm(o[1])
        body.append('  assign %s = %s;' % (o[0], src))
    if not old: r.shuffle(body)
    return '\n'.join(L + body + ['endmodule']) + '\n'

def tb(dn, ptext):
    ins = [x for x in dn['ins'] if not x.startswith('a')]
    pins = [x for x in ins if re.search(r'input %s;' % x, ptext)]
    na = len([x for x in dn['ins'] if x.startswith('a')])
    outs = [o[0] for o in dn['outs']]
    s = 'module tb;\n  reg clk = 0, rst_n = 0; reg [%d:0] a = 0;\n' % (na - 1)
    for x in ins: s += '  reg %s = 0;\n' % x
    for o in outs: s += '  wire %s_p, %s_n;\n' % (o, o)
    s += '  wire so;\n  integer i, bad = 0;\n'
    cp = ''.join(', .%s(%s)' % (x, x) for x in pins)
    s += '  top p (.clk(clk), .rst_n(rst_n), .scan_en(1\'b0), .scan_in(1\'b0), .scan_out(so), .a(a)%s%s);\n' % (cp, ''.join(', .%s(%s_p)' % (o, o) for o in outs))
    cp = ''.join(', .%s(%s)' % (x, x) for x in ins)
    s += '  top_new n (.clk(clk), .rst_n(rst_n), .a(a)%s%s);\n' % (cp, ''.join(', .%s(%s_n)' % (o, o) for o in outs))
    s += '  initial begin\n    for (i = 0; i < 300; i = i + 1) begin\n      a = $random; rst_n = (i > 1) && ($random % 16 != 0);\n'
    for x in ins: s += '      %s = $random;\n' % x
    s += '      #1;\n'
    for o in outs: s += '      if (%s_p !== %s_n) bad = bad + 1;\n' % (o, o)
    s += '      clk = 1; #1;\n'
    for o in outs: s += '      if (%s_p !== %s_n) bad = bad + 1;\n' % (o, o)
    s += '      clk = 0; #1;\n    end\n    $display("bad=%0d", bad);\n    $finish;\n  end\nendmodule\n'
    return s

fails = 0; stats = dict(cases=0, changed=0, metal=0)
for seed in range(N0, N1):
    r = random.Random(seed)
    base = gen(r); new = mutate(r, base)
    po, pn, pp = ['%s/%s_%d.v' % (out, k, seed) for k in ('old', 'new', 'patched')]
    open(po, 'w').write(render(r, base, 'old')); open(pn, 'w').write(render(r, new, 'new'))
    libs = sum((['-lib', l] for l in LIBS), [])
    res = []
    for th in ('1', '7'):
        p = subprocess.run([MECO, '-old', po, '-new', pn, *libs, '-tie', 'scan_en=0', *(['-spare'] if SPARE else []), '-o', pp + th, '-script', pp + th + '.tcl', '-threads', th], capture_output=True, text=True)
        res.append((p.returncode, p.stdout, open(pp + th).read() if os.path.exists(pp + th) else '', p.stderr))
    if res[0] != res[1]: print('seed', seed, 'THREAD MISMATCH'); fails += 1; continue
    if res[0][0] not in ((0, 3) if SPARE else (0,)) or (SPARE and (res[0][0] == 0) != (', 0 cells without a spare' in res[0][1] and ' +0 cells' in res[0][1])): print('seed', seed, 'rc', res[0][0], res[0][1][-300:], res[0][3]); fails += 1; continue
    stats['cases'] += 1
    if ' 0 changed' not in res[0][1]:
        stats['changed'] += 1
        if SPARE and res[0][0] == 0: stats['metal'] += 1
    if MSIM != '-':
        open(pp, 'w').write(res[0][2])
        nt = re.sub(r'module top ', 'module top_new ', open(pn).read())
        open(pn + '.sim.v', 'w').write(nt); open(pp + '.tb.v', 'w').write(tb(new, res[0][2]))
        p = subprocess.run([MSIM, CELLS, pp, pn + '.sim.v', pp + '.tb.v', '-top', 'tb'], capture_output=True, text=True)
        if 'bad=0' not in p.stdout: print('seed', seed, 'SIM MISMATCH', p.stdout[-200:], p.stderr[-300:]); fails += 1
print('fuzz: %d cases, %d with changes, %d failures' % (stats['cases'], stats['changed'], fails) + (', %d patched with spare cells only' % stats['metal'] if SPARE else ''))
sys.exit(1 if fails else 0)
