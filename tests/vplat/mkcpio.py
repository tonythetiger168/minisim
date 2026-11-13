# mkcpio.py - write a cpio archive in the "newc" format the Linux kernel unpacks
# as its initramfs. Entries: d:PATH (directory), c:PATH:MAJOR:MINOR (character
# device), f:PATH:FILE (executable file).
#   python3 mkcpio.py OUT.cpio ENTRY...
import sys, os
out = open(sys.argv[1], 'wb'); ino = [1]
def ent(name, mode, data=b'', rmaj=0, rmin=0, nlink=1):
    n = name.encode() + b'\0'
    h = '070701' + ''.join('%08x' % v for v in (ino[0], mode, 0, 0, nlink, 0, len(data), 0, 0, rmaj, rmin, len(n), 0))
    ino[0] += 1
    b = h.encode() + n; b += b'\0' * (-len(b) % 4); b += data; b += b'\0' * (-len(b) % 4)
    out.write(b)
for a in sys.argv[2:]:
    k, p = a.split(':', 1)
    if k == 'd': ent(p, 0o40755, nlink=2)
    elif k == 'c': p, ma, mi = p.split(':'); ent(p, 0o20600, rmaj=int(ma), rmin=int(mi))
    elif k == 'f': p, src = p.split(':'); ent(p, 0o100755, open(src, 'rb').read())
ent('TRAILER!!!', 0)
