#!/usr/bin/env python3
"""gen.py SEED BLOCKS INSNS > body.S - random A64 register-only instruction
blocks for differential testing (mvplat against an external reference)."""
import random, sys
seed, nblocks, ninsn = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
R = random.Random(seed)
def x(): return 'x%d' % R.randrange(29)
def w(): return 'w%d' % R.randrange(29)
def xz(): return R.choice(['x%d' % R.randrange(29)] * 7 + ['xzr'])
def wz(): return R.choice(['w%d' % R.randrange(29)] * 7 + ['wzr'])
def gp():
    return (x, xz) if R.random() < .5 else (w, wz)
def vreg(): return R.randrange(32)
COND = 'eq ne cs cc mi pl vs vc hi ls ge lt gt le al'.split()
def bitmask(width):
    while True:
        e = R.choice([e for e in (2, 4, 8, 16, 32, 64) if e <= width])
        ones = R.randrange(1, e)
        rot = R.randrange(e)
        v = (1 << ones) - 1
        v = ((v >> rot) | (v << (e - rot))) & ((1 << e) - 1)
        r = 0
        for i in range(0, width, e): r |= v << i
        return '#0x%x' % r
def imm(n): return '#%d' % R.randrange(n)
def integer():
    d, s = gp()
    W = 64 if d is x else 32
    k = R.randrange(28)
    sh = R.choice(['lsl', 'lsr', 'asr'])
    if k == 0: return '%s %s, %s, %s' % (R.choice(['add', 'adds', 'sub', 'subs']), d(), s(), s()) + R.choice(['', ', %s #%d' % (sh, R.randrange(W))])
    if k == 1: return '%s %s, %s, #%d%s' % (R.choice(['add', 'adds', 'sub', 'subs']), d(), d(), R.randrange(4096), R.choice(['', ', lsl #12']))
    if k == 2:
        ext = R.choice(['uxtb', 'uxth', 'uxtw', 'sxtb', 'sxth', 'sxtw'] + (['uxtx', 'sxtx'] if W == 64 else []))
        src = w() if W == 64 and ext not in ('uxtx', 'sxtx') else (x() if W == 64 else w())
        return '%s %s, %s, %s, %s #%d' % (R.choice(['add', 'adds', 'sub', 'subs']), d(), d(), src, ext, R.randrange(5))
    if k == 3: return '%s %s, %s, %s' % (R.choice(['and', 'orr', 'eor', 'ands', 'bic', 'bics', 'orn', 'eon']), d(), s(), s()) + R.choice(['', ', %s #%d' % (R.choice(['lsl', 'lsr', 'asr', 'ror']), R.randrange(W))])
    if k == 4: return '%s %s, %s, %s' % (R.choice(['and', 'orr', 'eor', 'ands']), d(), d(), bitmask(W))
    if k == 5: return '%s %s, #%d, lsl #%d' % (R.choice(['movz', 'movn', 'movk']), d(), R.randrange(65536), 16 * R.randrange(W // 16))
    if k == 6: return '%s %s, %s, %s' % (R.choice(['adc', 'adcs', 'sbc', 'sbcs']), d(), s(), s())
    if k == 7: return '%s %s, %s, %s, %s' % (R.choice(['csel', 'csinc', 'csinv', 'csneg']), d(), s(), s(), R.choice(COND))
    if k == 8: return '%s %s, %s, #%d, %s' % (R.choice(['ccmp', 'ccmn']), d(), R.choice(['%s' % s(), '#%d' % R.randrange(32)]), R.randrange(16), R.choice(COND))
    if k == 9: return '%s %s, %s, %s, %s' % (R.choice(['madd', 'msub']), d(), s(), s(), s())
    if k == 10: return '%s %s, %s, %s, %s' % (R.choice(['smaddl', 'umaddl', 'smsubl', 'umsubl']), x(), w(), w(), xz())
    if k == 11: return '%s %s, %s, %s' % (R.choice(['smulh', 'umulh']), x(), x(), x())
    if k == 12: return '%s %s, %s, %s' % (R.choice(['udiv', 'sdiv', 'lslv', 'lsrv', 'asrv', 'rorv']), d(), s(), s())
    if k == 13: return '%s %s, %s' % (R.choice(['rbit', 'rev', 'rev16', 'clz', 'cls'] + (['rev32'] if W == 64 else [])), d(), s())
    if k == 14:
        a = R.randrange(W); b = R.randrange(1, W - a + 1)
        return '%s %s, %s, #%d, #%d' % (R.choice(['ubfx', 'sbfx', 'bfi', 'bfxil', 'ubfiz', 'sbfiz']), d(), s(), a, b)
    if k == 15: return '%s %s, %s, #%d' % (R.choice(['lsl', 'lsr', 'asr', 'ror']), d(), s(), R.randrange(W))
    if k == 16: return 'extr %s, %s, %s, #%d' % (d(), s(), s(), R.randrange(W))
    if k == 17: return '%s %s, %s' % (R.choice(['cset', 'csetm']), d(), R.choice(COND[:-1]))
    if k == 18: return '%s %s, %s' % (R.choice(['sxtb', 'sxth', 'uxtb', 'uxth']), d(), w()) if d is w or R.random() < .5 and False else '%s %s, %s' % (R.choice(['sxtb', 'sxth', 'sxtw']), x(), w())
    if k == 19: return '%s %s, %s, %s' % (R.choice(['cinc', 'cinv', 'cneg']), d(), s(), R.choice(COND[:-1]))
    if k == 20: return '%s %s, %s' % (R.choice(['tst', 'cmp', 'cmn']), s(), s())
    if k == 21: return 'mvn %s, %s' % (d(), s())
    if k == 22: return 'neg%s %s, %s' % (R.choice(['', 's']), d(), s())
    if k == 23: return 'ngc%s %s, %s' % (R.choice(['', 's']), d(), s())
    if k == 24: return '%s %s, %s, %s' % (R.choice(['smull', 'umull', 'smnegl', 'umnegl']), x(), w(), w())
    if k == 25: return 'mul %s, %s, %s' % (d(), s(), s())
    if k == 26: return 'tst %s, %s' % (d(), bitmask(W))
    return 'mneg %s, %s, %s' % (d(), s(), s())

ARR = {8: ['8b', '16b'], 16: ['4h', '8h'], 32: ['2s', '4s'], 64: ['2d']}
def arr(sizes=(8, 16, 32, 64)):
    e = R.choice(sizes); a = R.choice(ARR[e]); return e, a
def v(a): return 'v%d.%s' % (vreg(), a)
FPA = {32: ['2s', '4s'], 64: ['2d']}
FPK = {17, 18, 19, 20, 21, 22, 23, 24, 26, 32, 33, 34, 35}
def simd():
    k = R.randrange(40)
    while __import__('os').environ.get('NOFP') and k in FPK: k = R.randrange(40)
    if k == 0:
        e, a = arr()
        ops = ['add', 'sub', 'cmeq', 'cmge', 'cmgt', 'cmhi', 'cmhs', 'cmtst', 'sqadd', 'uqadd', 'sqsub', 'uqsub', 'sshl', 'ushl', 'sqshl', 'uqshl', 'srshl', 'urshl', 'sqrshl', 'uqrshl']
        if e != 64: ops += ['mul', 'mla', 'mls', 'smax', 'smin', 'umax', 'umin', 'sabd', 'uabd', 'saba', 'uaba', 'shadd', 'uhadd', 'srhadd', 'urhadd', 'shsub', 'uhsub', 'addp', 'smaxp', 'sminp', 'umaxp', 'uminp']
        else: ops += ['addp']
        op = R.choice(ops)
        if op == 'mul' or op in ('mla', 'mls'):
            if e == 64: op = 'add'
        if e == 8 and op in (): pass
        if op in ('sqdmulh', 'sqrdmulh') and e not in (16, 32): op = 'add'
        return '%s %s, %s, %s' % (op, v(a), v(a), v(a))
    if k == 1:
        a = R.choice(['8b', '16b'])
        return '%s %s, %s, %s' % (R.choice(['and', 'orr', 'eor', 'bic', 'orn', 'bsl', 'bit', 'bif']), v(a), v(a), v(a))
    if k == 2:
        e, a = arr((16, 32))
        return '%s %s, %s, %s' % (R.choice(['sqdmulh', 'sqrdmulh', 'mul', 'pmul' if False else 'mul']), v(a), v(a), v(a))
    if k == 3:
        e, a = arr()
        ops = ['abs', 'neg', 'cmeq', 'cmge', 'cmgt', 'cmle', 'cmlt', 'sqabs', 'sqneg']
        if e == 8: ops += ['cnt', 'not', 'rbit', 'rev16', 'rev32', 'rev64', 'cls', 'clz']
        elif e == 16: ops += ['rev32', 'rev64', 'cls', 'clz']
        elif e == 32: ops += ['rev64', 'cls', 'clz']
        op = R.choice(ops)
        if op in ('cmeq', 'cmge', 'cmgt', 'cmle', 'cmlt'): return '%s %s, %s, #0' % (op, v(a), v(a))
        if op in ('cnt', 'not', 'rbit') and e == 8: pass
        if op == 'abs' and a == '2d' and False: pass
        if e == 64 and a != '2d': a = '2d'
        return '%s %s, %s' % (op, v(a), v(a))
    if k == 4:
        e, a = arr()
        sh = R.randrange(e)
        op = R.choice(['shl', 'sqshl', 'uqshl', 'sshr', 'ushr', 'srshr', 'urshr', 'ssra', 'usra', 'srsra', 'ursra', 'sri', 'sli', 'sqshlu'])
        if op in ('sshr', 'ushr', 'srshr', 'urshr', 'ssra', 'usra', 'srsra', 'ursra', 'sri'): sh = R.randrange(1, e + 1)
        return '%s %s, %s, #%d' % (op, v(a), v(a), sh)
    if k == 5:
        e = R.choice([8, 16, 32]); n = {8: '8h', 16: '4s', 32: '2d'}[e]; a = ARR[e][0]; q = R.random() < .5
        op = R.choice(['shrn', 'rshrn', 'sqshrn', 'uqshrn', 'sqrshrn', 'uqrshrn', 'sqshrun', 'sqrshrun'])
        a = ARR[e][1] if q else ARR[e][0]
        return '%s%s %s, %s, #%d' % (op, '2' if q else '', v(a), v(n), R.randrange(1, e + 1))
    if k == 6:
        e = R.choice([8, 16, 32]); n = {8: '8h', 16: '4s', 32: '2d'}[e]; q = R.random() < .5; a = ARR[e][1] if q else ARR[e][0]
        op = R.choice(['xtn', 'sqxtn', 'uqxtn', 'sqxtun'])
        return '%s%s %s, %s' % (op, '2' if q else '', v(a), v(n))
    if k == 7:
        e = R.choice([8, 16, 32]); n = {8: '8h', 16: '4s', 32: '2d'}[e]; q = R.random() < .5; a = ARR[e][1] if q else ARR[e][0]
        op = R.choice(['saddl', 'uaddl', 'ssubl', 'usubl', 'smull', 'umull', 'smlal', 'umlal', 'smlsl', 'umlsl', 'sabal', 'uabal', 'sabdl', 'uabdl'] + (['sqdmull', 'sqdmlal', 'sqdmlsl'] if e != 8 else ['pmull'] if not q else []))
        return '%s%s %s, %s, %s' % (op, '2' if q else '', v(n), v(a), v(a))
    if k == 8:
        e = R.choice([8, 16, 32]); n = {8: '8h', 16: '4s', 32: '2d'}[e]; q = R.random() < .5; a = ARR[e][1] if q else ARR[e][0]
        op = R.choice(['saddw', 'uaddw', 'ssubw', 'usubw'])
        return '%s%s %s, %s, %s' % (op, '2' if q else '', v(n), v(n), v(a))
    if k == 9:
        e = R.choice([8, 16, 32]); n = {8: '8h', 16: '4s', 32: '2d'}[e]; q = R.random() < .5; a = ARR[e][1] if q else ARR[e][0]
        op = R.choice(['addhn', 'raddhn', 'subhn', 'rsubhn'])
        return '%s%s %s, %s, %s' % (op, '2' if q else '', v(a), v(n), v(n))
    if k == 10:
        e = R.choice([8, 16, 32]); n = {8: '8h', 16: '4s', 32: '2d'}[e]; q = R.random() < .5; a = ARR[e][1] if q else ARR[e][0]
        return '%s%s %s, %s, #%d' % (R.choice(['sshll', 'ushll']), '2' if q else '', v(n), v(a), R.randrange(e))
    if k == 11:
        e, a = arr((8, 16, 32))
        if a == '2s': a = '4s'
        op = R.choice(['addv', 'smaxv', 'sminv', 'umaxv', 'uminv', 'saddlv', 'uaddlv'])
        dst = {('addv', 8): 'b', ('addv', 16): 'h', ('addv', 32): 's'}
        r = {8: 'b', 16: 'h', 32: 's'}[e]
        if op in ('saddlv', 'uaddlv'): r = {8: 'h', 16: 's', 32: 'd'}[e]
        return '%s %s%d, %s' % (op, r, vreg(), v(a))
    if k == 12:
        e, a = arr()
        idx = R.randrange(128 // e)
        r = {8: 'b', 16: 'h', 32: 's', 64: 'd'}[e]
        c = R.randrange(5)
        if c == 0: return 'dup %s, v%d.%s[%d]' % (v(a), vreg(), r, idx)
        if c == 1: return 'dup %s, %s' % (v(a), (xz() if e == 64 else wz()))
        if c == 2: return 'ins v%d.%s[%d], %s' % (vreg(), r, idx, xz() if e == 64 else wz())
        if c == 3: return 'ins v%d.%s[%d], v%d.%s[%d]' % (vreg(), r, idx, vreg(), r, R.randrange(128 // e))
        if e == 64: return 'umov %s, v%d.d[%d]' % (x(), vreg(), idx & 1)
        return '%s %s, v%d.%s[%d]' % (R.choice(['umov', 'smov']), w() if R.random() < .5 or e == 32 else x(), vreg(), r, idx) if e != 32 else 'umov %s, v%d.s[%d]' % (w(), vreg(), idx)
    if k == 13:
        a = R.choice(['8b', '16b'])
        return 'ext %s, %s, %s, #%d' % (v(a), v(a), v(a), R.randrange(8 if a == '8b' else 16))
    if k == 14:
        a = R.choice(['8b', '16b']); n = R.randrange(1, 5); s = R.randrange(32 - n + 1)
        regs = ', '.join('v%d.16b' % (s + i) for i in range(n))
        return '%s %s, {%s}, %s' % (R.choice(['tbl', 'tbx']), v(a), regs, v(a))
    if k == 15:
        e, a = arr()
        return '%s %s, %s, %s' % (R.choice(['zip1', 'zip2', 'uzp1', 'uzp2', 'trn1', 'trn2']), v(a), v(a), v(a))
    if k == 16:
        c = R.randrange(4)
        if c == 0: return 'movi %s, #%d' % (v(R.choice(['8b', '16b'])), R.randrange(256))
        if c == 1: return '%s %s, #%d, lsl #%d' % (R.choice(['movi', 'mvni', 'orr', 'bic']), v(R.choice(['4h', '8h'])), R.randrange(256), R.choice([0, 8]))
        if c == 2: return '%s %s, #%d, %s #%d' % (R.choice(['movi', 'mvni']), v(R.choice(['2s', '4s'])), R.randrange(256), R.choice(['lsl', 'msl']), 8)
        return 'movi %s, #0x%s' % (v('2d'), ''.join(R.choice(['00', 'ff']) for _ in range(8)))
    if k in (17, 18, 19):
        e = R.choice([32, 64]); a = R.choice(FPA[e])
        op = R.choice(['fadd', 'fsub', 'fmul', 'fdiv', 'fmax', 'fmin', 'fmaxnm', 'fminnm', 'fabd', 'fmla', 'fmls', 'fcmeq', 'fcmge', 'fcmgt', 'facge', 'facgt', 'fmulx', 'frecps', 'frsqrts', 'faddp', 'fmaxp', 'fminp', 'fmaxnmp', 'fminnmp'])
        return '%s %s, %s, %s' % (op, v(a), v(a), v(a))
    if k == 20:
        e = R.choice([32, 64]); a = R.choice(FPA[e])
        op = R.choice(['fabs', 'fneg', 'fsqrt', 'frintn', 'frintm', 'frintp', 'frintz', 'frinta', 'frintx', 'frinti', 'fcvtns', 'fcvtnu', 'fcvtms', 'fcvtmu', 'fcvtps', 'fcvtpu', 'fcvtzs', 'fcvtzu', 'fcvtas', 'fcvtau', 'scvtf', 'ucvtf', 'frecpe', 'frsqrte'] + (['urecpe', 'ursqrte'] if e == 32 else []))
        if op in ('fcmeq', 'fcmge', 'fcmgt', 'fcmle', 'fcmlt'): return '%s %s, %s, #0.0' % (op, v(a), v(a))
        return '%s %s, %s' % (op, v(a), v(a))
    if k == 21:
        e = R.choice([32, 64]); a = R.choice(FPA[e])
        return '%s %s, %s, #0.0' % (R.choice(['fcmeq', 'fcmge', 'fcmgt', 'fcmle', 'fcmlt']), v(a), v(a))
    if k in (22, 23, 24):
        t = R.choice(['s', 'd']); r = lambda: '%s%d' % (t, vreg())
        c = R.randrange(9)
        if c == 0: return '%s %s, %s, %s' % (R.choice(['fadd', 'fsub', 'fmul', 'fdiv', 'fmax', 'fmin', 'fmaxnm', 'fminnm', 'fnmul', 'fabd', 'fmulx', 'frecps', 'frsqrts']), r(), r(), r())
        if c == 1: return '%s %s, %s, %s, %s' % (R.choice(['fmadd', 'fmsub', 'fnmadd', 'fnmsub']), r(), r(), r(), r())
        if c == 2: return '%s %s, %s' % (R.choice(['fabs', 'fneg', 'fsqrt', 'frintn', 'frintm', 'frintp', 'frintz', 'frinta', 'frintx', 'frinti', 'fmov', 'frecpe', 'frsqrte', 'frecpx']), r(), r())
        if c == 3: return '%s %s, %s' % (R.choice(['fcmp', 'fcmpe']), r(), R.choice([r(), '#0.0']))
        if c == 4: return '%s %s, %s, #%d, %s' % (R.choice(['fccmp', 'fccmpe']), r(), r(), R.randrange(16), R.choice(COND))
        if c == 5: return 'fcsel %s, %s, %s, %s' % (r(), r(), r(), R.choice(COND))
        if c == 6:
            o = {'s': 'd', 'd': 's'}[t]
            if R.random() < .3: return 'fcvt h%d, %s' % (vreg(), r())
            if R.random() < .3: return 'fcvt %s, h%d' % (r(), vreg())
            return 'fcvt %s%d, %s' % (o, vreg(), r())
        if c == 7:
            g = x() if R.random() < .5 else w()
            op = R.choice(['fcvtns', 'fcvtnu', 'fcvtms', 'fcvtmu', 'fcvtps', 'fcvtpu', 'fcvtzs', 'fcvtzu', 'fcvtas', 'fcvtau'])
            if R.random() < .3 and op in ('fcvtzs', 'fcvtzu'): return '%s %s, %s, #%d' % (op, g, r(), R.randrange(1, 33 if g[0] == 'w' else 65))
            return '%s %s, %s' % (op, g, r())
        g = xz() if R.random() < .5 else wz()
        op = R.choice(['scvtf', 'ucvtf'])
        if R.random() < .3: return '%s %s, %s, #%d' % (op, r(), g, R.randrange(1, 33 if g[0] == 'w' else 65))
        return '%s %s, %s' % (op, r(), g)
    if k == 25:
        c = R.randrange(4)
        if c == 0: return 'fmov %s, %s' % (x(), 'd%d' % vreg())
        if c == 1: return 'fmov %s, %s' % ('d%d' % vreg(), xz())
        if c == 2: return 'fmov %s, %s' % (w(), 's%d' % vreg())
        return 'fmov v%d.d[1], %s' % (vreg(), xz()) if R.random() < .5 else 'fmov %s, v%d.d[1]' % (x(), vreg())
    if k == 26:
        e = R.choice([32, 64]); a = R.choice(FPA[e]); r = {32: 's', 64: 'd'}[e]
        return '%s %s, %s, v%d.%s[%d]' % (R.choice(['fmul', 'fmla', 'fmls', 'fmulx']), v(a), v(a), vreg(), r, R.randrange(128 // e))
    if k == 27:
        e = R.choice([16, 32]); a = R.choice(ARR[e]); r = {16: 'h', 32: 's'}[e]
        reg = R.randrange(16) if e == 16 else vreg()
        return '%s %s, %s, v%d.%s[%d]' % (R.choice(['mul', 'mla', 'mls', 'sqdmulh', 'sqrdmulh']), v(a), v(a), reg, r, R.randrange(128 // e))
    if k == 28:
        e = R.choice([16, 32]); q = R.random() < .5; a = ARR[e][1] if q else ARR[e][0]; n = {16: '4s', 32: '2d'}[e]; r = {16: 'h', 32: 's'}[e]
        reg = R.randrange(16) if e == 16 else vreg()
        return '%s%s %s, %s, v%d.%s[%d]' % (R.choice(['smull', 'umull', 'smlal', 'umlal', 'smlsl', 'umlsl', 'sqdmull', 'sqdmlal', 'sqdmlsl']), '2' if q else '', v(n), v(a), reg, r, R.randrange(128 // e))
    if k == 29:
        t = R.choice(['b', 'h', 's', 'd']); e = {'b': 8, 'h': 16, 's': 32, 'd': 64}[t]
        r = lambda: '%s%d' % (t, vreg())
        ops = ['sqadd', 'uqadd', 'sqsub', 'uqsub', 'sqshl', 'uqshl', 'sqrshl', 'uqrshl']
        if t == 'd': ops += ['add', 'sub', 'cmeq', 'cmge', 'cmgt', 'cmhi', 'cmhs', 'cmtst', 'sshl', 'ushl', 'srshl', 'urshl']
        if t in ('h', 's'): ops += ['sqdmulh', 'sqrdmulh']
        return '%s %s, %s, %s' % (R.choice(ops), r(), r(), r())
    if k == 30:
        r = lambda: 'd%d' % vreg()
        op = R.choice(['shl', 'sshr', 'ushr', 'ssra', 'usra', 'srshr', 'urshr', 'sri', 'sli'])
        sh = R.randrange(64) if op in ('shl', 'sli') else R.randrange(1, 65)
        return '%s %s, %s, #%d' % (op, r(), r(), sh)
    if k == 31:
        r = lambda: 'd%d' % vreg()
        return '%s %s, %s' % (R.choice(['abs', 'neg']), r(), r()) if R.random() < .5 else '%s %s, %s, #0' % (R.choice(['cmeq', 'cmge', 'cmgt', 'cmle', 'cmlt']), r(), r())
    if k == 32:
        return 'addp d%d, v%d.2d' % (vreg(), vreg()) if R.random() < .5 else '%s %s%d, v%d.%s' % (R.choice(['faddp', 'fmaxp', 'fminp', 'fmaxnmp', 'fminnmp']), *R.choice([('s', vreg(), vreg(), '2s'), ('d', vreg(), vreg(), '2d')]))
    if k == 33:
        return '%s s%d, v%d.4s' % (R.choice(['fmaxv', 'fminv', 'fmaxnmv', 'fminnmv']), vreg(), vreg())
    if k == 34:
        e = R.choice([32, 64]); a = R.choice(FPA[e])
        return '%s %s, %s, #%d' % (R.choice(['scvtf', 'ucvtf', 'fcvtzs', 'fcvtzu']), v(a), v(a), R.randrange(1, e + 1))
    if k == 35:
        q = R.random() < .5
        c = R.randrange(4)
        if c == 0: return 'fcvtl%s v%d.2d, v%d.%s' % ('2' if q else '', vreg(), vreg(), '4s' if q else '2s')
        if c == 1: return 'fcvtn%s v%d.%s, v%d.2d' % ('2' if q else '', vreg(), '4s' if q else '2s', vreg())
        if c == 2: return 'fcvtl%s v%d.4s, v%d.%s' % ('2' if q else '', vreg(), vreg(), '8h' if q else '4h')
        return 'fcvtxn%s v%d.%s, v%d.2d' % ('2' if q else '', vreg(), '4s' if q else '2s', vreg())
    if k == 36:
        e = R.choice([8, 16, 32]); a = R.choice(ARR[e]); n = {8: '4h', 16: '2s', 32: '1d'}[e]
        if a in ('16b', '8h', '4s'): n = {8: '8h', 16: '4s', 32: '2d'}[e]
        return '%s %s, %s' % (R.choice(['saddlp', 'uaddlp', 'sadalp', 'uadalp']), v(n), v(a))
    if k == 37:
        e, a = arr((8, 16, 32, 64))
        return '%s %s, %s' % (R.choice(['suqadd', 'usqadd']), v(a), v(a))
    if k == 38:
        e = R.choice([8, 16, 32]); n = {8: '8h', 16: '4s', 32: '2d'}[e]; a = ARR[e][0]; q = R.random() < .5
        if q: a = ARR[e][1]
        return 'shll%s %s, %s, #%d' % ('2' if q else '', v(n), v(a), e)
    t = R.choice(['b', 'h', 's', 'd'])
    return '%s %s%d, %s%d' % (R.choice(['sqabs', 'sqneg']), t, vreg(), t, vreg())

out = []
for b in range(nblocks):
    out.append('.balign 4\nblock%d:' % b)
    for i in range(ninsn):
        out.append('    ' + (integer() if R.random() < float(__import__('os').environ.get('INTP', '.45')) else simd()))
    out.append('    ret')
print('\n'.join(out))
