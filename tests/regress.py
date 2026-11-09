#!/usr/bin/env python3
"""IEEE 1800-2023 regression report for minisim.

Runs every tests/spec/<clause>-<topic>.sv through msim (and Icarus with
--icarus), compares the output with the file's "//> " lines, and prints a
table per chapter: how many tests pass, how many are known gaps
("//! xfail: <what>"), and any surprise in either direction.

  tests/regress.py [--msim build/msim] [--icarus] [--md report.md] [-j N] [filter]

Exit status is 1 if any test that should pass fails, or any xfail test passes
(remove its xfail line). Known gaps alone do not fail the run.
"""
import argparse, concurrent.futures as cf, os, re, subprocess, sys, tempfile
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = os.path.join(HERE, 'spec')


def parse(path):
    exp, meta = [], {}
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if line.startswith('//> '): exp.append(line[4:])
        elif line == '//>': exp.append('')
        else:
            m = re.match(r'//! ([a-z-]+): ?(.*)', line)
            if m: meta[m.group(1)] = m.group(2)
    return ''.join(e + '\n' for e in exp), meta


def run_msim(msim, path, want_rc):
    try:
        extra = parse(path)[1].get('args', '').split()
        r = subprocess.run([msim, os.path.basename(path)] + extra, cwd=os.path.dirname(path),
                           capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return None, 'timeout'
    if r.returncode != want_rc:
        return None, (r.stderr.strip().splitlines() or ['exit %d' % r.returncode])[-1]
    return r.stdout, ''


def run_icarus(path):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, 'a.vvp')
        r = subprocess.run(['iverilog', '-g2012', '-o', out, path], capture_output=True, text=True)
        if r.returncode: return None, 'iverilog: ' + (r.stderr.strip().splitlines() or ['?'])[0]
        try:
            r = subprocess.run(['vvp', '-n', out], cwd=os.path.dirname(path), capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            return None, 'timeout'
        got = re.sub(r'[^\n]*(\$finish called|VCD info:|WARNING: )[^\n]*\n', '', r.stdout)
        return got, ''


def chapter(name):
    return int(re.match(r'(\d+)', name).group(1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--msim', default=os.path.join(HERE, '..', 'build', 'msim'))
    ap.add_argument('--icarus', action='store_true', help='also check the expectations against Icarus')
    ap.add_argument('--md', help='write the report as Markdown to this file')
    ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
    ap.add_argument('filter', nargs='?', default='', help='only tests whose name contains this')
    a = ap.parse_args()
    files = sorted((f for f in os.listdir(SPEC) if f.endswith('.sv') and a.filter in f),
                   key=lambda f: [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', f)])

    def one(f):
        path = os.path.join(SPEC, f)
        exp, meta = parse(path)
        got, why = run_msim(os.path.abspath(a.msim), path, int(meta.get('exit', 0)))
        ok = got == exp
        if got is not None and not ok: why = 'output differs'
        ic = None
        if a.icarus and 'icarus-skip' not in meta:
            igot, iwhy = run_icarus(path)
            ic = 'ok' if igot == exp else (iwhy or 'output differs')
        return f, meta, ok, why, ic

    with cf.ThreadPoolExecutor(a.j) as ex:
        res = list(ex.map(one, files))

    by = defaultdict(lambda: {'pass': 0, 'xfail': 0, 'fail': [], 'xpass': [], 'gaps': [], 'icarus': []})
    for f, meta, ok, why, ic in res:
        c = by[chapter(f)]
        name = f[:-3]
        if 'xfail' in meta:
            if ok: c['xpass'].append(name)
            else: c['xfail'] += 1; c['gaps'].append((name, meta['xfail']))
        else:
            if ok: c['pass'] += 1
            else: c['fail'].append((name, why))
        if ic and ic != 'ok': c['icarus'].append((name, ic))

    tp = sum(c['pass'] for c in by.values()); tx = sum(c['xfail'] for c in by.values())
    tf = sum(len(c['fail']) for c in by.values()); txp = sum(len(c['xpass']) for c in by.values())
    total = tp + tx + tf + txp
    lines = ['# minisim IEEE 1800-2023 regression', '',
             f'{total} tests: {tp} pass, {tx} known gaps (xfail), {tf} unexpected failures, {txp} xfail tests now passing.',
             f'Spec conformance by test: {100.0 * (tp + txp) / max(total, 1):.1f}%.', '',
             '| Chapter | Pass | Known gaps | Unexpected | Pass rate |', '|---|---|---|---|---|']
    for ch in sorted(by):
        c = by[ch]; n = c['pass'] + c['xfail'] + len(c['fail']) + len(c['xpass'])
        bad = ', '.join([x for x, _ in c['fail']] + [x + ' (xfail passes)' for x in c['xpass']]) or ''
        lines.append(f"| {ch} | {c['pass'] + len(c['xpass'])} | {c['xfail']} | {bad} | {100.0 * (c['pass'] + len(c['xpass'])) / n:.0f}% |")
    lines += ['', '## Known gaps', '']
    for ch in sorted(by):
        for name, what in by[ch]['gaps']: lines.append(f'- `{name}`: {what}')
    probs = [(n, w) for ch in sorted(by) for n, w in by[ch]['fail']]
    if probs:
        lines += ['', '## Unexpected failures', ''] + [f'- `{n}`: {w}' for n, w in probs]
    icp = [(n, w) for ch in sorted(by) for n, w in by[ch]['icarus']]
    if a.icarus:
        lines += ['', '## Icarus disagreements', ''] + ([f'- `{n}`: {w}' for n, w in icp] or ['none'])
    text = '\n'.join(lines) + '\n'
    print(text)
    if a.md: open(a.md, 'w').write(text)
    return 1 if (tf or txp) else 0


if __name__ == '__main__':
    sys.exit(main())
