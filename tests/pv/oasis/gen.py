# Writes a random OASIS file that uses every record, repetition and point-list type.
import random, struct, sys, zlib
seed = int(sys.argv[1]); out = sys.argv[2]
R = random.Random(seed)
def U(v):
    b = bytearray()
    while True:
        c = v & 0x7f; v >>= 7
        if v: b.append(c | 0x80)
        else: b.append(c); return bytes(b)
def S(v): return U((abs(v) << 1) | (1 if v < 0 else 0))
def STR(s): s = s.encode() if isinstance(s, str) else s; return U(len(s)) + s
def REAL(v):
    k = R.randrange(3)
    if float(v).is_integer() and k == 0: return U(0) + U(int(v)) if v >= 0 else U(1) + U(int(-v))
    if k == 1: return U(6) + struct.pack('<f', v)
    return U(7) + struct.pack('<d', v)
def G(x, y):
    if R.random() < 0.5 and (x == 0 or y == 0 or abs(x) == abs(y)) and (x or y):
        m = max(abs(x), abs(y))
        d = {(1,0):0,(0,1):1,(-1,0):2,(0,-1):3,(1,1):4,(-1,1):5,(-1,-1):6,(1,-1):7}[((x>0)-(x<0), (y>0)-(y<0))]
        return U((m << 4) | (d << 1))
    return U((abs(x) << 2) | ((1 if x < 0 else 0) << 1) | 1) + S(y)
def D3(x, y):
    m = max(abs(x), abs(y))
    d = {(1,0):0,(0,1):1,(-1,0):2,(0,-1):3,(1,1):4,(-1,1):5,(-1,-1):6,(1,-1):7}[((x>0)-(x<0), (y>0)-(y<0))]
    return U((m << 3) | d)
def D2(x, y):
    m = max(abs(x), abs(y)); d = {(1,0):0,(0,1):1,(-1,0):2,(0,-1):3}[((x>0)-(x<0), (y>0)-(y<0))]
    return U((m << 2) | d)

M = {}          # modal variables
rel = [False]
def reset():
    M.clear(); rel[0] = False
    for k in ('px','py','gx','gy','tx','ty'): M[k] = 0
def opt(key, val, enc):
    # (present?, bytes): leaves the field out when the modal variable already has the value
    if key in M and M[key] == val and R.random() < 0.6: return False, b''
    M[key] = val
    return True, enc(val)
def xy(kx, ky, x, y):
    px = not (M[kx] == x and R.random() < 0.6)
    py = not (M[ky] == y and R.random() < 0.6)
    b = b''
    if px: b += S(x - M[kx] if rel[0] else x)
    if py: b += S(y - M[ky] if rel[0] else y)
    M[kx] = x; M[ky] = y
    return px, py, b
def rep():
    # (present?, bytes)
    if R.random() < 0.6: return False, b''
    if 'rep' in M and R.random() < 0.2: return True, U(0)
    M['rep'] = 1
    t = R.randrange(1, 12)
    n = R.randrange(0, 3); m = R.randrange(0, 3)
    if t == 1: return True, U(1) + U(n) + U(m) + U(R.randrange(1, 500)) + U(R.randrange(1, 500))
    if t in (2, 3): return True, U(t) + U(n) + U(R.randrange(1, 500))
    if t in (4, 6): return True, U(t) + U(n) + b''.join(U(R.randrange(1, 300)) for _ in range(n + 1))
    if t in (5, 7): return True, U(t) + U(n) + U(R.randrange(1, 9)) + b''.join(U(R.randrange(1, 50)) for _ in range(n + 1))
    if t == 8: return True, U(8) + U(n) + U(m) + G(R.randrange(1, 400), R.randrange(-100, 100)) + G(R.randrange(-100, 100), R.randrange(1, 400))
    if t == 9: return True, U(9) + U(n) + G(R.randrange(-300, 300), R.randrange(1, 300))
    if t == 10: return True, U(10) + U(n) + b''.join(G(R.randrange(1, 300), R.randrange(-300, 300)) for _ in range(n + 1))
    return True, U(11) + U(n) + U(R.randrange(1, 9)) + b''.join(G(R.randrange(1, 40), R.randrange(-40, 40)) for _ in range(n + 1))
def ld(info):
    b = b''
    p, e = opt('layer', R.randrange(0, 5), U)
    if p: info |= 1; b += e
    p, e = opt('dt', R.randrange(0, 3), U)
    if p: info |= 2; b += e
    return info, b
def plist(polygon):
    t = R.randrange(6)
    if t in (0, 1):
        n = R.choice((2, 4, 6)) if polygon else R.randrange(1, 6)
        ds = []
        for i in range(n):
            v = R.randrange(10, 200) * R.choice((1, -1))
            ds.append(v)
        if polygon:
            # make it a staircase so the polygon is simple: all same sign per axis
            ds = [abs(v) for v in ds]
        return U(t) + U(n) + b''.join(S(v) for v in ds)
    n = R.randrange(2, 7) if polygon else R.randrange(1, 6)
    pts = []
    if polygon:
        # a convex fan around the first point keeps the polygon simple
        import math
        angs = sorted(R.uniform(0.05, 1.5) for _ in range(n))
        cur = (0, 0)
        raw = [(int(300 * math.cos(a)) + 20 * i, int(300 * math.sin(a))) for i, a in enumerate(angs)]
        deltas = []
        for q in raw:
            deltas.append((q[0] - cur[0], q[1] - cur[1])); cur = q
    else:
        deltas = [(R.randrange(-200, 200), R.randrange(-200, 200)) for _ in range(n)]
    if t == 2:
        deltas = [(R.randrange(10, 200), 0) if i % 2 == 0 else (0, R.randrange(10, 200)) for i in range(n)]
        return U(2) + U(n) + b''.join(D2(*d) for d in deltas)
    if t == 3:
        deltas = [[(v, 0), (0, v), (v, v)][i % 3] for i, v in enumerate(R.randrange(10, 200) for _ in range(n))]
        return U(3) + U(n) + b''.join(D3(*d) for d in deltas)
    if t == 4: return U(4) + U(n) + b''.join(G(*d) for d in deltas)
    dd = []; prev = (0, 0)
    for d in deltas: dd.append((d[0] - prev[0], d[1] - prev[1])); prev = d
    return U(5) + U(n) + b''.join(G(*d) for d in dd)
def prop():
    b = b''
    for _ in range(R.randrange(0, 3)):
        if 'prop' in M and R.random() < 0.2: b += U(29); continue
        M['prop'] = 1
        vals = []
        for _ in range(R.randrange(0, 18)):
            k = R.choice((0, 1, 2, 4, 6, 7, 8, 9, 10, 11, 12, 13))
            if k == 0: vals.append(U(0) + U(R.randrange(100)))
            elif k == 1: vals.append(U(1) + U(R.randrange(100)))
            elif k == 2: vals.append(U(2) + U(R.randrange(1, 100)))
            elif k == 4: vals.append(U(4) + U(R.randrange(100)) + U(R.randrange(1, 100)))
            elif k == 6: vals.append(U(6) + struct.pack('<f', 1.5))
            elif k == 7: vals.append(U(7) + struct.pack('<d', 2.5))
            elif k == 8: vals.append(U(8) + U(R.randrange(1000)))
            elif k == 9: vals.append(U(9) + S(R.randrange(-1000, 1000)))
            elif k in (10, 11, 12): vals.append(U(k) + STR('v%d' % R.randrange(99)))
            else: vals.append(U(13) + U(0))
        info = (min(len(vals), 15) << 4) | 0x04
        byref = R.random() < 0.5
        if byref: info |= 0x02
        b += U(28) + bytes([info]) + (U(0) if byref else STR('pname')) + (U(len(vals)) if len(vals) >= 15 else b'') + b''.join(vals)
    return b

ncell = R.randrange(2, 5)
names = ['c%d' % i for i in range(ncell)]
byref_cells = R.random() < 0.5
body = b''
texts = ['lbl%d' % i for i in range(3)]
head_names = R.random() < 0.5
text_by_ref = R.random() < 0.5
namerecs = b''.join(U(4) + STR(n) + U(i + 10) for i, n in enumerate(names)) + b''.join(U(6) + STR(t) + U(i) for i, t in enumerate(texts))
namerecs += U(8) + STR('pname') + U(0) + U(10) + STR('pstr') + U(0) + U(11) + STR('metal') + U(3) + U(1) + U(0) + U(12) + STR('txt') + U(4) + U(1) + U(2) + U(1) + U(0)
for ci in range(ncell):
    c = b''
    c += (U(13) + U(ci + 10)) if byref_cells else (U(14) + STR(names[ci]))
    reset()
    c += prop()
    for _ in range(R.randrange(3, 25)):
        k = R.random()
        if k < 0.07: rel[0] = not rel[0]; c += U(16 if rel[0] else 15); continue
        if k < 0.10: c += U(0); continue
        if k < 0.25 and ci > 0:
            general = R.random() < 0.4
            info = 0
            tgt = R.randrange(0, ci)
            if not ('pc' in M and M['pc'] == tgt and R.random() < 0.5):
                M['pc'] = tgt; info |= 0x80
                if R.random() < 0.5: info |= 0x40; ref = U(tgt + 10)
                else: ref = STR(names[tgt])
            else: ref = b''
            extra = b''
            if general:
                if R.random() < 0.5: info |= 0x04; extra += REAL(R.choice((1.0, 2.0, 0.5)))
                if R.random() < 0.7: info |= 0x02; extra += REAL(R.choice((0.0, 90.0, 180.0, 270.0, 45.0)))
            else: info |= R.randrange(4) << 1
            if R.random() < 0.5: info |= 1
            px, py, b = xy('px', 'py', R.randrange(-2000, 2000), R.randrange(-2000, 2000))
            if px: info |= 0x20
            if py: info |= 0x10
            hr, rb = rep()
            if hr: info |= 0x08
            c += U(18 if general else 17) + bytes([info]) + ref + extra + b + rb + prop(); continue
        if k < 0.35:
            info = 0; b = b''
            s = R.randrange(3)
            if not ('ts' in M and M['ts'] == s and R.random() < 0.5):
                M['ts'] = s; info |= 0x40
                if text_by_ref: info |= 0x20; b += U(s)
                else: b += STR(texts[s])
            p, e = opt('tl', R.randrange(0, 5), U)
            if p: info |= 1; b += e
            p, e = opt('tt', R.randrange(0, 3), U)
            if p: info |= 2; b += e
            px, py, e = xy('tx', 'ty', R.randrange(-2000, 2000), R.randrange(-2000, 2000))
            if px: info |= 0x10
            if py: info |= 0x08
            hr, rb = rep()
            if hr: info |= 0x04
            c += U(19) + bytes([info]) + b + e + rb + prop(); continue
        x, y = R.randrange(-2000, 2000), R.randrange(-2000, 2000)
        if k < 0.50:
            info, b = ld(0)
            sq = R.random() < 0.3
            w = R.randrange(1, 400); h = w if sq else R.randrange(1, 400)
            p, e = opt('w', w, U)
            if p: info |= 0x40; b += e
            if sq: info |= 0x80; M['h'] = w
            else:
                p, e = opt('h', h, U)
                if p: info |= 0x20; b += e
            rec = 20
        elif k < 0.62:
            info, b = ld(0)
            if 'pp' not in M or R.random() < 0.7: M['pp'] = 1; info |= 0x20; b += plist(True)
            rec = 21
        elif k < 0.74:
            info, b = ld(0)
            hw = R.randrange(0, 60)
            p, e = opt('hw', hw, U)
            if p: info |= 0x40; b += e
            if 'ext' not in M or R.random() < 0.5:
                M['ext'] = 1; info |= 0x80
                ss, ee = R.randrange(1, 4), R.randrange(1, 4)
                b += U((ss << 2) | ee) + (S(R.randrange(-20, 80)) if ss == 3 else b'') + (S(R.randrange(-20, 80)) if ee == 3 else b'')
            if 'ppath' not in M or R.random() < 0.7: M['ppath'] = 1; info |= 0x20; b += plist(False)
            rec = 22
        elif k < 0.84:
            info, b = ld(0)
            w, h = R.randrange(200, 400), R.randrange(200, 400)
            p, e = opt('w', w, U)
            if p: info |= 0x40; b += e
            p, e = opt('h', h, U)
            if p: info |= 0x20; b += e
            if R.random() < 0.5: info |= 0x80
            rec = R.choice((23, 24, 25))
            if rec != 25: b += S(R.randrange(-90, 90))
            if rec != 24: b += S(R.randrange(-90, 90))
        elif k < 0.95:
            info, b = ld(0)
            t = R.randrange(26)
            p, e = opt('ct', t, U)
            if p: info |= 0x80; b += e
            if t < 4: w, h = R.randrange(300, 400), R.randrange(50, 250)
            elif t < 8: w, h = R.randrange(300, 400), R.randrange(50, 140)
            elif t < 12: w, h = R.randrange(50, 250), R.randrange(300, 400)
            elif t < 16: w, h = R.randrange(50, 140), R.randrange(300, 400)
            else: w, h = R.randrange(50, 400), R.randrange(50, 400)
            if t in (16, 17, 18, 19, 22, 23, 25) or t not in (20, 21):
                p, e = opt('w', w, U)
                if p: info |= 0x40; b += e
            if t in (20, 21) or t < 16 or t == 24:
                p, e = opt('h', h, U)
                if p: info |= 0x20; b += e
            if t in (16, 17, 18, 19, 25): M['h'] = M['w']
            elif t in (20, 21): M['w'] = 2 * M['h']
            elif t in (22, 23): M['h'] = 2 * M['w']
            rec = 26
        else:
            info, b = ld(0)
            p, e = opt('r', R.randrange(10, 300), U)
            if p: info |= 0x20; b += e
            rec = 27
        px, py, e = xy('gx', 'gy', x, y)
        if px: info |= 0x10
        if py: info |= 0x08
        hr, rb = rep()
        if hr: info |= 0x04
        c += U(rec) + bytes([info]) + b + e + rb + prop()
    if R.random() < 0.4:
        z = zlib.compressobj(9, zlib.DEFLATED, -15, 9, R.choice((zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED)))
        comp = z.compress(c[1 + (1 if not byref_cells else 0) * 0:]) + z.flush() if False else None
        # the CELL record stays outside the block
        hdr = (U(13) + U(ci + 10)) if byref_cells else (U(14) + STR(names[ci]))
        rest = c[len(hdr):]
        z = zlib.compressobj(R.choice((0, 1, 9)), zlib.DEFLATED, -15)
        comp = z.compress(rest) + z.flush()
        c = hdr + U(34) + U(0) + U(len(rest)) + U(len(comp)) + comp
    body += c
data = b'%SEMI-OASIS\r\n' + U(1) + STR('1.0') + REAL(R.choice((1000.0, 2000.0, 400.0))) + U(1)
data += (namerecs + body) if head_names else (body + namerecs)
end = U(2) + b''.join(U(0) for _ in range(12))
pad = 255 - len(end) - 1 - (2 if 255 - len(end) - 1 - 2 > 127 else 1)
end += STR(b'\0' * (256 - len(end) - 2 - 1)) + U(0)
assert len(end) == 256, len(end)
open(out, 'wb').write(data + end)
