#!/usr/bin/env python3
"""Compare two VCD files by the value every signal holds at the end of each
time step. The order of changes inside a step, glitches and the name of the
top scope may differ. usage: vcd_same.py a.vcd b.vcd"""
import sys


def load(path):
    ids, scope, cur, out, t, header = {}, [], {}, {}, 0, True
    for line in open(path):
        line = line.strip()
        if header:
            w = line.split()
            if line.startswith('$scope'):
                scope.append(w[2])
            elif line.startswith('$upscope'):
                scope.pop()
            elif line.startswith('$var'):
                ids.setdefault(w[3], []).append('.'.join(scope[1:] + [w[4]]))
            elif line.startswith('$enddefinitions'):
                header = False
            continue
        if not line or line[0] == '$':
            continue
        if line[0] == '#':
            out[t] = dict(cur)
            t = int(line[1:])
            continue
        if line[0] in 'bBrR':
            v, i = line.split()
            v = v[1:]
        else:
            v, i = line[0], line[1:]
        for n in ids.get(i, []):
            cur[n] = v.lstrip('0') or '0'
    out[t] = dict(cur)
    return out


a, b = load(sys.argv[1]), load(sys.argv[2])
bad = 0
for t in sorted(set(a) | set(b)):
    x, y = a.get(t), b.get(t)
    if x is None or y is None:
        print('time %d is in one file only' % t)
        bad += 1
        continue
    for n in sorted(set(x) | set(y)):
        if x.get(n) != y.get(n):
            print('#%d %s: %s / %s' % (t, n, x.get(n), y.get(n)))
            bad += 1
    if bad > 20:
        break
if not a or len(next(iter(a.values()), {})) == 0 and len(a) < 2:
    print('no values in', sys.argv[1])
    bad += 1
sys.exit(1 if bad else 0)
