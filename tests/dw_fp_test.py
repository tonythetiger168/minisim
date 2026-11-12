#!/usr/bin/env python3
"""Checks the floating point datapath modules (dw/MW_fp_*.sv) on msim
against an exact reference written here with Python's fractions.

    dw_fp_test.py REPO MSIM [N]

For each format (tiny 4/3, half 5/10, bfloat16 8/7, single 8/23,
double 11/52) and each of the five rounding modes, N random operand pairs
(default 300; a third of them special: zeros, infinities, quiet and
signaling NaNs, subnormals, the largest and smallest numbers, ties) go
through add, sub, mult, div, sqrt, fused multiply-add, reciprocal,
compare/min/max, both integer conversions and a conversion to another
format. The reference rounds the exact rational result once; the
module output and all five status flags must match bit for bit.
"""
import os
import random
import subprocess
import sys
import tempfile
from fractions import Fraction
from math import isqrt

FORMATS = [("tiny", 4, 3), ("half", 5, 10), ("bf16", 8, 7), ("single", 8, 23), ("double", 11, 52)]
IW = 32
# MW_fp_flt2flt is checked from each format to this one: narrowing and widening.
TARGET = {"tiny": FORMATS[4], "half": FORMATS[0], "bf16": FORMATS[1], "single": FORMATS[2], "double": FORMATS[3]}
RNE, RTZ, RUP, RDN, RNA = range(5)
INVALID, DIVZERO, OVERFLOW, UNDERFLOW, INEXACT = (1 << i for i in range(5))


class Fmt:
    def __init__(self, exp, frac):
        self.exp, self.frac = exp, frac
        self.bias = (1 << (exp - 1)) - 1
        self.emin = 1 - self.bias
        self.emax = self.bias
        self.w = exp + frac + 1
        self.qnan = (((1 << exp) - 1) << frac) | (1 << (frac - 1))

    def fields(self, x):
        return x >> (self.w - 1), (x >> self.frac) & ((1 << self.exp) - 1), x & ((1 << self.frac) - 1)

    def isnan(self, x):
        _, e, f = self.fields(x)
        return e == (1 << self.exp) - 1 and f != 0

    def issnan(self, x):
        return self.isnan(x) and not (x >> (self.frac - 1)) & 1

    def isinf(self, x):
        _, e, f = self.fields(x)
        return e == (1 << self.exp) - 1 and f == 0

    def inf(self, s):
        return (s << (self.w - 1)) | (((1 << self.exp) - 1) << self.frac)

    def zero(self, s):
        return s << (self.w - 1)

    def value(self, x):
        """Exact value of a finite encoding."""
        s, e, f = self.fields(x)
        v = Fraction(f, 1 << self.frac) * Fraction(2) ** self.emin if e == 0 else \
            Fraction((1 << self.frac) | f, 1 << self.frac) * Fraction(2) ** (e - self.bias)
        return -v if s else v

    def round(self, v, rnd, sign=None):
        """Round an exact nonzero value (or zero with an explicit sign)."""
        s = int(v < 0) if v != 0 else sign
        a = abs(v)
        if a == 0:
            return self.zero(s), 0
        e = a.numerator.bit_length() - a.denominator.bit_length()
        if Fraction(2) ** e > a:
            e -= 1
        tiny = e < self.emin
        e = max(e, self.emin)
        q = a / Fraction(2) ** (e - self.frac)
        n = q.numerator // q.denominator
        r = q - n
        inc = rounds_up(n, r, rnd, s)
        n += inc
        flags = INEXACT if r else 0
        if tiny and r:
            flags |= UNDERFLOW
        if n == 1 << (self.frac + 1):
            n >>= 1
            e += 1
        if e > self.emax:
            to_inf = {RTZ: False, RUP: not s, RDN: bool(s)}.get(rnd, True)
            z = self.inf(s) if to_inf else (s << (self.w - 1)) | ((((1 << self.exp) - 2) << self.frac) | ((1 << self.frac) - 1))
            return z, OVERFLOW | INEXACT
        if n < 1 << self.frac:
            return (s << (self.w - 1)) | n, flags
        return (s << (self.w - 1)) | ((e + self.bias) << self.frac) | (n - (1 << self.frac)), flags


def rounds_up(n, r, rnd, s):
    """1 if the integer n with remainder 0 <= r < 1 rounds up in magnitude."""
    if r == 0:
        return 0
    if rnd == RNE:
        return int(r > Fraction(1, 2) or (r == Fraction(1, 2) and n & 1))
    if rnd == RTZ:
        return 0
    if rnd == RUP:
        return int(not s)
    if rnd == RDN:
        return int(bool(s))
    return int(r >= Fraction(1, 2))


def nan_result(f, xs, always_invalid=False):
    inv = always_invalid or any(f.issnan(x) for x in xs)
    return f.qnan, INVALID if inv else 0


def ref_add(f, a, b, rnd, sub=False):
    if sub:
        b ^= 1 << (f.w - 1)
    sa, sb = a >> (f.w - 1), b >> (f.w - 1)
    if f.isnan(a) or f.isnan(b):
        return nan_result(f, [a, b])
    if f.isinf(a) and f.isinf(b):
        return (a, 0) if sa == sb else (f.qnan, INVALID)
    if f.isinf(a):
        return a, 0
    if f.isinf(b):
        return b, 0
    v = f.value(a) + f.value(b)
    if v == 0:
        return f.zero(sa if sa == sb else int(rnd == RDN)), 0
    return f.round(v, rnd)


def ref_mult(f, a, b, rnd):
    s = (a ^ b) >> (f.w - 1)
    if f.isnan(a) or f.isnan(b):
        return nan_result(f, [a, b])
    za, zb = f.value(a) == 0 if not f.isinf(a) else False, f.value(b) == 0 if not f.isinf(b) else False
    if (f.isinf(a) and zb) or (f.isinf(b) and za):
        return f.qnan, INVALID
    if f.isinf(a) or f.isinf(b):
        return f.inf(s), 0
    v = f.value(a) * f.value(b)
    return f.round(v, rnd, s)


def ref_div(f, a, b, rnd):
    s = (a ^ b) >> (f.w - 1)
    if f.isnan(a) or f.isnan(b):
        return nan_result(f, [a, b])
    ia, ib = f.isinf(a), f.isinf(b)
    za = not ia and f.value(a) == 0
    zb = not ib and f.value(b) == 0
    if (ia and ib) or (za and zb):
        return f.qnan, INVALID
    if ia:
        return f.inf(s), 0
    if zb:
        return f.inf(s), DIVZERO
    if ib or za:
        return f.zero(s), 0
    return f.round(f.value(a) / f.value(b), rnd, s)


def ref_sqrt(f, a, rnd):
    s = a >> (f.w - 1)
    if f.isnan(a):
        return nan_result(f, [a])
    if f.isinf(a):
        return (f.qnan, INVALID) if s else (a, 0)
    v = f.value(a)
    if v == 0:
        return a, 0
    if v < 0:
        return f.qnan, INVALID
    # sqrt(v) = sqrt(num * 2^(2k) / den) / 2^k: an integer root with many
    # more bits than the format; an inexact root is moved half a unit up
    # so that it stays strictly between the two neighbours.
    k = 2 * (f.frac + f.bias + 8)
    n = v.numerator * v.denominator * (1 << (2 * k))
    r = isqrt(n)
    root = Fraction(2 * r + (r * r != n), 2) / (v.denominator * (1 << k))
    return f.round(root, rnd)


def ref_cmp(f, a, b, zctr):
    na, nb = f.isnan(a), f.isnan(b)
    inv = INVALID if f.issnan(a) or f.issnan(b) else 0
    if na or nb:
        lt = eq = gt = 0
        un = 1
        if na and nb:
            mn = mx = f.qnan
        else:
            mn = mx = b if na else a
    else:
        un = 0
        va = f.value(a) if not f.isinf(a) else (Fraction(-1) if a >> (f.w - 1) else Fraction(1)) * 10 ** 400
        vb = f.value(b) if not f.isinf(b) else (Fraction(-1) if b >> (f.w - 1) else Fraction(1)) * 10 ** 400
        lt, eq, gt = int(va < vb), int(va == vb), int(va > vb)
        # -0 < +0 for min and max
        ka, kb = (va, -(a >> (f.w - 1))), (vb, -(b >> (f.w - 1)))
        mn, mx = (a, b) if ka < kb else (b, a)
    z0, z1 = (mx, mn) if zctr else (mn, mx)
    return lt, eq, gt, un, z0, z1, inv


def ref_mac(f, a, b, c, rnd):
    sp = (a ^ b) >> (f.w - 1)
    sc = c >> (f.w - 1)
    if f.isnan(a) or f.isnan(b) or f.isnan(c):
        inv = any(f.issnan(x) for x in (a, b, c))
        za = not f.isinf(a) and not f.isnan(a) and f.value(a) == 0
        zb = not f.isinf(b) and not f.isnan(b) and f.value(b) == 0
        return f.qnan, INVALID if inv or (f.isinf(a) and zb) or (za and f.isinf(b)) else 0
    ia, ib, ic = f.isinf(a), f.isinf(b), f.isinf(c)
    za = not ia and f.value(a) == 0
    zb = not ib and f.value(b) == 0
    if (ia and zb) or (za and ib):
        return f.qnan, INVALID
    if ia or ib:
        if ic and sc != sp:
            return f.qnan, INVALID
        return f.inf(sp), 0
    if ic:
        return c, 0
    v = f.value(a) * f.value(b) + f.value(c)
    if v == 0:
        return f.zero(sp if sp == sc else int(rnd == RDN)), 0
    return f.round(v, rnd)


def ref_flt2flt(f, g, a, rnd):
    s = a >> (f.w - 1)
    if f.isnan(a):
        return g.qnan, INVALID if f.issnan(a) else 0
    if f.isinf(a):
        return g.inf(s), 0
    return g.round(f.value(a), rnd, s)


def ref_i2flt(f, a, tc, rnd):
    v = a - (1 << IW) if tc and a >> (IW - 1) else a
    return f.round(Fraction(v), rnd, 0)


def ref_flt2i(f, a, tc, rnd):
    s = a >> (f.w - 1)
    hi = (1 << (IW - 1)) - 1 if tc else (1 << IW) - 1
    lo = -(1 << (IW - 1)) if tc else 0
    if f.isnan(a):
        return hi & ((1 << IW) - 1), INVALID
    if f.isinf(a):
        return (lo if s else hi) & ((1 << IW) - 1), INVALID
    v = f.value(a)
    m = abs(v)
    n = m.numerator // m.denominator
    n += rounds_up(n, m - n, rnd, s)
    r = -n if s else n
    if r > hi or r < lo:
        return (lo if s else hi) & ((1 << IW) - 1), INVALID
    return r & ((1 << IW) - 1), INEXACT if m != int(m) else 0


def operand(f, rng):
    """A random encoding; a third of them are special values."""
    s = rng.getrandbits(1)
    emask = (1 << f.exp) - 1
    k = rng.randrange(12)
    if k == 0:
        return f.zero(s)
    if k == 1:
        return f.inf(s)
    if k == 2:  # quiet or signaling NaN
        q = rng.getrandbits(1)
        frac = (q << (f.frac - 1)) | rng.getrandbits(f.frac - 1) if f.frac > 1 else 1
        if not frac:
            frac = 1
        return (s << (f.w - 1)) | (emask << f.frac) | frac
    if k == 3:  # subnormal
        return (s << (f.w - 1)) | max(1, rng.getrandbits(f.frac))
    if k == 4:  # near the largest or smallest normal
        e = rng.choice([emask - 1, 1, 2])
        return (s << (f.w - 1)) | (e << f.frac) | rng.choice([0, 1, (1 << f.frac) - 1, rng.getrandbits(f.frac)])
    if k == 5:  # near 1
        e = f.bias + rng.randrange(-2, 3)
        return (s << (f.w - 1)) | (e << f.frac) | rng.getrandbits(f.frac)
    if k == 6:  # small integer-valued
        return f.round(Fraction(rng.randrange(-1000, 1001) or 1), RNE)[0]
    return (s << (f.w - 1)) | (rng.randrange(emask) << f.frac) | rng.getrandbits(f.frac)


def integer(rng):
    k = rng.randrange(6)
    if k == 0:
        return rng.choice([0, 1, (1 << IW) - 1, 1 << (IW - 1), (1 << (IW - 1)) - 1])
    if k == 1:
        return rng.getrandbits(rng.randrange(1, IW + 1))
    return rng.getrandbits(IW)


def tb_source(name, e, fr, files, ez, fz):
    W = e + fr + 1
    return """// generated by tests/dw_fp_test.py
module tb;
    localparam int W = %(W)d;
    logic [W-1:0] c, z_mac, z_rcp;
    logic [%(WZ1)d:0] z_cvt;
    logic [4:0] s_mac, s_rcp, s_cvt;
    MW_fp_mac   #(%(P)s) u_mac  (.a, .b, .c, .rnd, .z(z_mac), .status(s_mac));
    MW_fp_recip #(%(P)s) u_rcp  (.a, .rnd, .z(z_rcp), .status(s_rcp));
    MW_fp_flt2flt #(.EXP_A(%(E)d), .FRAC_A(%(F)d), .EXP_Z(%(EZ)d), .FRAC_Z(%(FZ)d)) u_cvt (.a, .rnd, .z(z_cvt), .status(s_cvt));
    logic [W-1:0] a, b, z_add, z_sub, z_mul, z_div, z_sqrt, z_i2f, z0, z1;
    logic [%(IW1)d:0] i, z_f2i;
    logic [2:0] rnd;
    logic tc, zctr, lt, eq, gt, un;
    logic [4:0] s_add, s_sub, s_mul, s_div, s_sqrt, s_i2f, s_f2i, s_cmp;
    MW_fp_add   #(%(P)s) u_add  (.a, .b, .rnd, .z(z_add), .status(s_add));
    MW_fp_sub   #(%(P)s) u_sub  (.a, .b, .rnd, .z(z_sub), .status(s_sub));
    MW_fp_mult  #(%(P)s) u_mul  (.a, .b, .rnd, .z(z_mul), .status(s_mul));
    MW_fp_div   #(%(P)s) u_div  (.a, .b, .rnd, .z(z_div), .status(s_div));
    MW_fp_sqrt  #(%(P)s) u_sqrt (.a, .rnd, .z(z_sqrt), .status(s_sqrt));
    MW_fp_cmp   #(%(P)s) u_cmp  (.a, .b, .zctr, .altb(lt), .aeqb(eq), .agtb(gt), .unordered(un),
                                 .z0, .z1, .status(s_cmp));
    MW_fp_i2flt #(%(P)s, .IW(%(IW)d)) u_i2f (.a(i), .tc, .rnd, .z(z_i2f), .status(s_i2f));
    MW_fp_flt2i #(%(P)s, .IW(%(IW)d)) u_f2i (.a, .tc, .rnd, .z(z_f2i), .status(s_f2i));
    initial begin
        int fd, n, errs, k;
        logic [W-1:0] wa, wb, w_add, w_sub, w_mul, w_div, w_sqrt, w_i2f, w0, w1;
        logic [%(IW1)d:0] wi, w_f2i;
        logic [4:0] f_add, f_sub, f_mul, f_div, f_sqrt, f_i2f, f_f2i, f_cmp;
        logic [3:0] wc;
        logic [W-1:0] wcc, w_mac, w_rcp;
        logic [%(WZ1)d:0] w_cvt;
        logic [4:0] f_mac, f_rcp, f_cvt;
        int r, t, cz;
        errs = 0; n = 0;
        fd = $fopen("%(vec)s", "r");
        while ($fscanf(fd, "%%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h %%h",
                       r, t, cz, wa, wb, wi, w_add, f_add, w_sub, f_sub, w_mul, f_mul, w_div, f_div,
                       w_sqrt, f_sqrt, wc, w0, w1, f_cmp, w_i2f, f_i2f, w_f2i, f_f2i,
                       wcc, w_mac, f_mac, w_rcp, f_rcp, w_cvt, f_cvt) == 31) begin
            a = wa; b = wb; c = wcc; i = wi; rnd = r; tc = t; zctr = cz;
            #1;
            k = 0;
            if ({z_add, s_add} !== {w_add, f_add}) k = 1;
            else if ({z_sub, s_sub} !== {w_sub, f_sub}) k = 2;
            else if ({z_mul, s_mul} !== {w_mul, f_mul}) k = 3;
            else if ({z_div, s_div} !== {w_div, f_div}) k = 4;
            else if ({z_sqrt, s_sqrt} !== {w_sqrt, f_sqrt}) k = 5;
            else if ({lt, eq, gt, un, z0, z1, s_cmp} !== {wc, w0, w1, f_cmp}) k = 6;
            else if ({z_i2f, s_i2f} !== {w_i2f, f_i2f}) k = 7;
            else if ({z_f2i, s_f2i} !== {w_f2i, f_f2i}) k = 8;
            else if ({z_mac, s_mac} !== {w_mac, f_mac}) k = 9;
            else if ({z_rcp, s_rcp} !== {w_rcp, f_rcp}) k = 10;
            else if ({z_cvt, s_cvt} !== {w_cvt, f_cvt}) k = 11;
            if (k == 9 && errs < 10) $display("  mac c=%%h got %%h/%%b want %%h/%%b", wcc, z_mac, s_mac, w_mac, f_mac);
            if (k == 11 && errs < 10) $display("  cvt got %%h/%%b want %%h/%%b", z_cvt, s_cvt, w_cvt, f_cvt);
            if (k != 0) begin
                errs++;
                if (errs <= 10)
                    $display("%(name)s MISMATCH op%%0d rnd=%%0d tc=%%0d a=%%h b=%%h i=%%h | add %%h/%%b sub %%h/%%b mul %%h/%%b div %%h/%%b sqrt %%h/%%b cmp %%b %%h %%h/%%b i2f %%h/%%b f2i %%h/%%b",
                             k, r, t, wa, wb, wi, z_add, s_add, z_sub, s_sub, z_mul, s_mul, z_div, s_div,
                             z_sqrt, s_sqrt, {lt, eq, gt, un}, z0, z1, s_cmp, z_i2f, s_i2f, z_f2i, s_f2i);
            end
            n++;
        end
        $display("%(name)s vectors=%%0d errors=%%0d", n, errs);
        $finish;
    end
endmodule
""" % dict(W=W, E=e, F=fr, EZ=ez, FZ=fz, WZ1=ez + fz, IW=IW, IW1=IW - 1, P=".EXP(%d), .FRAC(%d)" % (e, fr), vec=files, name=name)


def main():
    repo, msim = sys.argv[1], sys.argv[2]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 300
    rng = random.Random(1735)
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, e, fr in FORMATS:
            f = Fmt(e, fr)
            _, ez, fz = TARGET[name]
            g = Fmt(ez, fz)
            lines = []
            for rnd in range(5):
                for _ in range(count):
                    a, b, i = operand(f, rng), operand(f, rng), integer(rng)
                    if rng.randrange(8) == 0:  # b next to a: cancellation and ties
                        b = a ^ (1 << (f.w - 1)) ^ rng.randrange(4)
                    tc, zctr = rng.getrandbits(1), rng.getrandbits(1)
                    cols = [rnd, tc, zctr, a, b, i]
                    for z, fl in (ref_add(f, a, b, rnd), ref_add(f, a, b, rnd, True), ref_mult(f, a, b, rnd),
                                  ref_div(f, a, b, rnd), ref_sqrt(f, a, rnd)):
                        cols += [z, fl]
                    lt, eq, gt, un, z0, z1, inv = ref_cmp(f, a, b, zctr)
                    cols += [lt << 3 | eq << 2 | gt << 1 | un, z0, z1, inv]
                    cols += list(ref_i2flt(f, i, tc, rnd)) + list(ref_flt2i(f, a, tc, rnd))
                    c = operand(f, rng)
                    if rng.randrange(4) == 0 and not (f.isnan(a) or f.isnan(b) or f.isinf(a) or f.isinf(b)):
                        # c near -a*b: cancellation in the fused add
                        p = f.value(a) * f.value(b)
                        c = f.round(-p, rng.randrange(5), 1)[0] if p != 0 else c
                    cols += [c] + list(ref_mac(f, a, b, c, rnd))
                    cols += list(ref_div(f, f.round(Fraction(1), RNE)[0], a, rnd)) + list(ref_flt2flt(f, g, a, rnd))
                    lines.append(" ".join("%x" % c for c in cols))
            vec = os.path.join(tmp, name + ".hex")
            open(vec, "w").write("\n".join(lines) + "\n")
            tb = os.path.join(tmp, name + "_tb.sv")
            open(tb, "w").write(tb_source(name, e, fr, vec, ez, fz))
            p = subprocess.run([msim, "-y", os.path.join(repo, "dw"), "+libext+.sv", "-top", "tb", tb],
                               capture_output=True, text=True, timeout=600)
            out = p.stdout + p.stderr
            print(out.strip())
            if p.returncode != 0 or "%s vectors=%d errors=0" % (name, len(lines)) not in out:
                bad += 1
    if bad:
        sys.exit("dw_fp_test: %d format(s) failed" % bad)
    print("dw_fp_test: all formats match")


if __name__ == "__main__":
    main()
