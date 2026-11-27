import sys, collections
s = sys.argv[1]
q = open('q%s.txt' % s).read().split('\n'); m = open('m%s.txt' % s).read().split('\n')
names = ['x%d' % i for i in range(29)] + ['nzcv', 'fpsr'] + ['v%d' % i for i in range(32)]
blocks = open('blocks-%s.txt' % s).read().split('block ')[1:]
bad = 0
by = collections.OrderedDict()
for b in range(len(blocks)):
    qa = ' '.join(q[5 * b:5 * b + 5]).split(); ma = ' '.join(m[5 * b:5 * b + 5]).split()
    if qa != ma:
        bad += 1
        ins = blocks[b].strip().split('\n')[1:]
        key = ins[-1].split()[0] if ins else '?'
        diffs = ['%s ref %s mvplat %s' % (names[i], a, c) for i, (a, c) in enumerate(zip(qa, ma)) if a != c]
        if len(ma) < len(qa): diffs.append('mvplat output short')
        onlyfpsr = all(d.startswith('fpsr') for d in diffs)
        by.setdefault(key + (' (fpsr only)' if onlyfpsr else ''), []).append((ins, diffs))
for k, v in sorted(by.items(), key=lambda kv: -len(kv[1])):
    ins, diffs = v[0]
    print('%4d %-22s e.g. %s' % (len(v), k, ' ; '.join(i.strip() for i in ins)))
    for d in diffs[:3]: print('        ' + d)
print('seed %s: %d of %d blocks differ' % (s, bad, len(blocks)))
sys.exit(1 if bad else 0)
