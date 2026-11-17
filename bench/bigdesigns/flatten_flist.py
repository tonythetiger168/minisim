#!/usr/bin/env python3
"""Flatten a VCS-style file list: expand $VARS, inline -f/-F files, drop // comments.
usage: flatten_flist.py <list> > out.f"""
import os, sys

def expand(path, out):
    base = os.path.dirname(os.path.abspath(path))
    for line in open(path):
        s = line.split('//')[0].strip() if not line.lstrip().startswith('//') else ''
        s = os.path.expandvars(s)
        if not s:
            continue
        if s[:3] in ('-f ', '-F '):
            sub = s[3:].strip()
            expand(sub if os.path.isabs(sub) else os.path.join(base, sub), out)
            continue
        out.append(s)

out = []
expand(sys.argv[1], out)
print('\n'.join(out))
