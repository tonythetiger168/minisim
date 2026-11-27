#!/usr/bin/env python3
"""mk.py SEED BLOCKS INSNS > prog.S - wraps gen.py's blocks with per-block
initial state and a dump of every register after the block."""
import random, sys, subprocess
seed, nb, ni = sys.argv[1:4]
body = subprocess.run([sys.executable, __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'gen.py'), seed, nb, ni], capture_output=True, text=True, check=True).stdout
R = random.Random(int(seed) * 7919)
blocks = []
cur = None
for line in body.splitlines():
    if line.startswith('block'): cur = []; blocks.append(cur)
    elif line.startswith('    ') and line.strip() != 'ret': cur.append(line.strip())
# drop what the assembler refuses (the generator is not exact about operand forms)
import tempfile, os, re
flat = [i for b in blocks for i in b]
with tempfile.NamedTemporaryFile('w', suffix='.S', delete=False) as t:
    t.write('\n'.join(flat) + '\n')
r = subprocess.run(['clang', '--target=aarch64-none-elf', '-march=armv8-a', '-c', '-o', os.devnull, t.name], capture_output=True, text=True)
badl = {int(m) - 1 for m in re.findall(r'\.S:(\d+):\d+: error', r.stderr)}
os.unlink(t.name)
k = 0
for b in blocks:
    keep = []
    for i in b:
        if k not in badl: keep.append(i)
        k += 1
    b[:] = keep
sys.stderr.write('%d of %d lines refused by the assembler\n' % (len(badl), len(flat)))
if os.environ.get('SHOWBAD'): sys.stderr.write('\n'.join(sorted({flat[i].split()[0] + ' ' + flat[i] for i in badl})[:60]) + '\n')
def val64():
    c = R.randrange(6)
    if c == 0: return R.getrandbits(64)
    if c == 1: return R.choice([0, 1, 0xffffffffffffffff, 0x8000000000000000, 0x7fffffffffffffff, 0x80000000, 0x7fffffff, 0xffffffff, 0x100000000])
    if c == 2: return R.getrandbits(R.randrange(1, 64))
    import struct
    if c == 3: return struct.unpack('<Q', struct.pack('<d', R.choice([0.0, -0.0, 1.0, -1.5, 0.5, 3.0, 1e300, -1e-300, 2.5, 123456.789, float('inf'), -float('inf'), 4.9e-324, 2**63, 2**64, -2**63, 2**31, 4294967296.5]) * (1 if R.random() < .7 else R.random() * 1000)))[0]
    if c == 4:
        f = lambda: struct.unpack('<I', struct.pack('<f', R.choice([0.0, -0.0, 1.0, -1.5, 0.5, 3.0, 1e30, 1e-30, 2.5, 65504.0, float('inf'), 2**31, 2**32, -2**31, 1e-40, 7.0, 0.1]) * (1 if R.random() < .7 else R.random() * 100)))[0]
        return f() | f() << 32
    return R.getrandbits(64) & 0xffff00ff00ff00ff
out = ['.text', '.global runall', 'runall:',
       '    adrp x9, saved', '    add x9, x9, :lo12:saved',
       '    stp x19, x20, [x9, #0]', '    stp x21, x22, [x9, #16]', '    stp x23, x24, [x9, #32]',
       '    stp x25, x26, [x9, #48]', '    stp x27, x28, [x9, #64]', '    stp x29, x30, [x9, #80]',
       '    stp d8, d9, [x9, #96]', '    stp d10, d11, [x9, #112]', '    stp d12, d13, [x9, #128]', '    stp d14, d15, [x9, #144]']
data = ['.data', '.balign 16']
for b, ins in enumerate(blocks):
    out += ['    adrp x29, init%d' % b, '    add x29, x29, :lo12:init%d' % b,
            '    ldr x0, [x29, #232]', '    msr nzcv, x0', '    ldr x0, [x29, #240]', '    msr fpcr, x0', '    msr fpsr, xzr']
    for r in range(0, 28, 2): out.append('    ldp x%d, x%d, [x29, #%d]' % (r, r + 1, 8 * r))
    out.append('    ldr x28, [x29, #224]')
    for r in range(0, 32, 2): out.append('    ldp q%d, q%d, [x29, #%d]' % (r, r + 1, 256 + 16 * r))
    out += ['    ' + i for i in ins]
    out += ['    adrp x29, outbuf + %d' % (b * 768), '    add x29, x29, :lo12:outbuf + %d' % (b * 768)]
    for r in range(0, 28, 2): out.append('    stp x%d, x%d, [x29, #%d]' % (r, r + 1, 8 * r))
    out += ['    str x28, [x29, #224]', '    mrs x0, nzcv', '    str x0, [x29, #232]', '    mrs x0, fpsr', '    str x0, [x29, #240]']
    for r in range(0, 32, 2): out.append('    stp q%d, q%d, [x29, #%d]' % (r, r + 1, 256 + 16 * r))
    xs = [val64() for _ in range(29)]
    nzcv = R.randrange(16) << 28
    fpcr = R.randrange(4) << 22 | (R.random() < .2) << 25 | (R.random() < .1) << 24 | (R.random() < .2) << 26
    vs = [val64() | val64() << 64 for _ in range(32)]
    data.append('init%d:' % b)
    data += ['    .quad 0x%x' % v for v in xs + [nzcv, fpcr, 0]]
    for v in vs: data.append('    .quad 0x%x, 0x%x' % (v & (2**64 - 1), v >> 64))
out += ['    adrp x9, saved', '    add x9, x9, :lo12:saved',
        '    ldp x19, x20, [x9, #0]', '    ldp x21, x22, [x9, #16]', '    ldp x23, x24, [x9, #32]',
        '    ldp x25, x26, [x9, #48]', '    ldp x27, x28, [x9, #64]', '    ldp x29, x30, [x9, #80]',
        '    ldp d8, d9, [x9, #96]', '    ldp d10, d11, [x9, #112]', '    ldp d12, d13, [x9, #128]', '    ldp d14, d15, [x9, #144]',
        '    ret']
out += data + ['.bss', '.balign 16', 'saved: .space 160', '.global outbuf', 'outbuf: .space %d' % (len(blocks) * 768),
               '.global nblocks', '.section .rodata', '.balign 8', 'nblocks: .quad %d' % len(blocks)]
print('\n'.join(out))
# the block listing for diagnosis
with open('blocks-%s.txt' % seed, 'w') as f:  # in the current directory
    for b, ins in enumerate(blocks): f.write('block %d\n' % b + ''.join('  %s\n' % i for i in ins))
