#!/usr/bin/env python3
"""Check mformal's AIGER export against its own verdicts.

usage: aiger_check.py MFORMAL DESIGN.sv [mformal options]

Writes the model with -aiger, reads it back with a small independent AIGER
reader, explores every reachable state (the designs under tests/formal are
small) and compares, per property, "a bad state is reachable" with the
verdict mformal reports: falsified / covered must be reachable, proven /
unreachable must not be.
"""
import json, os, subprocess, sys, tempfile

def read_aag(path):
    with open(path) as f:
        lines = f.read().split('\n')
    h = lines[0].split()
    assert h[0] == 'aag', h
    M, I, L, O, A = map(int, h[1:6])
    B = int(h[6]) if len(h) > 6 else 0
    C = int(h[7]) if len(h) > 7 else 0
    p = 1
    ins = [int(lines[p + i]) for i in range(I)]; p += I
    latches = []
    for i in range(L):
        t = list(map(int, lines[p + i].split()))
        latches.append((t[0], t[1], t[2] if len(t) > 2 else 0))
    p += L + O
    bad = [int(lines[p + i]) for i in range(B)]; p += B
    con = [int(lines[p + i]) for i in range(C)]; p += C
    ands = [tuple(map(int, lines[p + i].split())) for i in range(A)]; p += A
    names = {}
    for l in lines[p:]:
        if l == 'c': break
        if l[:1] == 'b' and ' ' in l:
            k, n = l.split(' ', 1)
            names[int(k[1:])] = n
    return M, ins, latches, bad, con, ands, names

def main():
    mformal, design, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
    tmp = tempfile.mkdtemp()
    aag, js = os.path.join(tmp, 'm.aag'), os.path.join(tmp, 'r.json')
    subprocess.run([mformal, design, '-q', '-aiger', aag] + extra, check=True)
    subprocess.run([mformal, design, '-q', '-no-trace', '-json', js, '-o', os.path.join(tmp, 'out')] + extra, stdout=subprocess.DEVNULL)
    M, ins, latches, bad, con, ands, names = read_aag(aag)
    verdict = {p['name']: p['result'] for p in json.load(open(js))['properties']}

    def ev(state, inp):
        v = [0] * (M + 1)
        for x, b in zip(ins, inp): v[x >> 1] = b
        for (q, _, _), b in zip(latches, state): v[q >> 1] = b
        for lhs, a, b in ands:
            v[lhs >> 1] = (v[a >> 1] ^ (a & 1)) & (v[b >> 1] ^ (b & 1))
        g = lambda l: v[l >> 1] ^ (l & 1)
        return all(g(c) for c in con), tuple(g(n) for _, n, _ in latches), [g(b) for b in bad]

    # initial states: an uninitialized latch (reset == itself) takes both values
    free = [i for i, (q, _, r) in enumerate(latches) if r == q]
    if len(free) > 12:
        print('skip: %d uninitialized latches' % len(free)); return 0
    init = []
    for m in range(1 << len(free)):
        s = [1 if r == 1 else 0 for _, _, r in latches]
        for k, i in enumerate(free): s[i] = (m >> k) & 1
        init.append(tuple(s))
    inputs = [tuple((m >> k) & 1 for k in range(len(ins))) for m in range(1 << len(ins))]
    seen, todo, hit = set(init), list(init), [False] * len(bad)
    while todo:
        s = todo.pop()
        for inp in inputs:
            ok, nxt, bs = ev(s, inp)
            if not ok: continue   # a constraint fails: the path ends before this cycle
            for i, b in enumerate(bs): hit[i] = hit[i] or bool(b)
            if nxt not in seen:
                seen.add(nxt); todo.append(nxt)
                if len(seen) > 400000:
                    print('skip: too many states'); return 0
    fails = 0
    for i in range(len(bad)):
        v = verdict.get(names[i], '?')
        want = {'falsified': True, 'covered': True, 'proven': False, 'unreachable': False}.get(v)
        if want is None: continue   # bounded, inconclusive: nothing to compare
        if want != hit[i]:
            fails += 1
            print('MISMATCH %s: mformal says %s, bad state %s in the AIGER model' % (names[i], v, 'reachable' if hit[i] else 'not reachable'))
    print('%d properties, %d latches, %d states, %d mismatches' % (len(bad), len(latches), len(seen), fails))
    return 1 if fails else 0

sys.exit(main())
