#!/usr/bin/env python3
"""Random C programs through mhls, checked three ways.

For every seed: generate a C function, run mhls on it with random options,
then check the same random calls (a) with the host C++ compiler against
mhls's reference run and (b) in msim on the generated RTL. A front-end bug
shows up in (a), a scheduling/binding/emission bug in (b).

  fuzz.py --mhls build/mhls --msim build/msim --out /tmp/hlsfuzz --seeds 0:50
"""
import argparse
import os
import random
import subprocess
import sys

TYPES = ["int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t", "bool"]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.pipe = self.r.random() < 0.3
        self.vars = []      # (name, type) assignable scalars
        self.ro = []        # read-only names
        self.arrs = []      # (name, size, writable)
        self.pts = []       # lvalues of struct type pt_t
        self.loopy = not self.pipe and self.r.random() < 0.3   # has loops shaped for loop pipelining
        self.streams = not self.pipe and self.r.random() < 0.25   # has stream arguments
        self.depth = 0
        self.loop = 0
        self.n = 0
        self.lines = []

    def ty(self):
        return self.r.choice(TYPES)

    def const(self):
        r = self.r
        c = r.choice([0, 1, 2, 3, 5, 7, 8, 15, 16, 31, 100, 255, 256, 1000, 65535, 0x7fffffff, 0xdeadbeef, r.getrandbits(32)])
        s = str(c) + ("u" if c > 0x7fffffff else "")
        return r.choice([s, s, "(%s)%s" % (self.ty(), s), "(-%s)" % s if c <= 0x7fffffff else s])

    def expr(self, d=0):
        r = self.r
        if d > 3 or r.random() < 0.25:
            pool = [n for n, _ in self.vars] + self.ro
            if pool and r.random() < 0.8:
                return r.choice(pool)
            return self.const()
        k = r.random()
        a = lambda: self.expr(d + 1)
        if k < 0.34:
            return "(%s %s %s)" % (a(), r.choice(["+", "-", "*", "&", "|", "^", "+", "-"]), a())
        if k < 0.42:
            return "(%s %s (%s & %d))" % (a(), r.choice(["<<", ">>"]), a(), r.choice([3, 7, 15]))
        if k < 0.48:
            return "(%s %s ((%s & 15) + 1))" % (a(), r.choice(["/", "%"]), a())
        if k < 0.60:
            return "(%s %s %s)" % (a(), r.choice(["<", "<=", ">", ">=", "==", "!="]), a())
        if k < 0.66:
            return "(%s %s %s)" % (a(), r.choice(["&&", "||"]), a())
        if k < 0.74:
            return "(%s ? %s : %s)" % (a(), a(), a())
        if k < 0.82:
            return "(%s)%s" % (self.ty(), a())
        if k < 0.88:
            return "(%s %s)" % (r.choice(["-", "~", "!"]), a())
        if k < 0.96 and self.arrs:
            n, size, _ = r.choice(self.arrs)
            return "%s[%s & %d]" % (n, a(), size - 1)
        return self.const()

    def emit(self, s):
        self.lines.append("  " * (self.depth + 1) + s)

    def stmt(self):
        r = self.r
        if self.streams and r.random() < 0.15:
            if r.random() < 0.5:
                self.n += 1
                self.emit("uint8_t v%d = sin.read();" % self.n)
                self.vars.append(("v%d" % self.n, "uint8_t"))
            else:
                self.emit("sout.write(%s);" % self.expr(1))
            return
        k = r.random()
        if k < 0.40 and self.vars:
            n, _ = r.choice(self.vars)
            self.emit("%s %s %s;" % (n, r.choice(["=", "=", "=", "+=", "-=", "^=", "|=", "&=", "*="]), self.expr()))
        elif k < 0.48:
            self.n += 1
            n, t = "v%d" % self.n, self.ty()
            self.emit("%s %s = %s;" % (t, n, self.expr()))
            self.vars.append((n, t))
        elif k < 0.56 and [a for a in self.arrs if a[2]]:
            n, size, _ = r.choice([a for a in self.arrs if a[2]])
            self.emit("%s[%s & %d] %s %s;" % (n, self.expr(1), size - 1, r.choice(["=", "+=", "^="]), self.expr(1)))
        elif k < 0.76 and self.depth < 3:
            self.emit("if (%s) {" % self.expr(1))
            self.block(r.randint(1, 3))
            if r.random() < 0.5:
                self.emit("} else {")
                self.block(r.randint(1, 3))
            self.emit("}")
        elif k < 0.90 and self.depth < 2 and self.loop < 2:
            self.n += 1
            i = "i%d" % self.n
            unroll = self.pipe or r.random() < 0.3
            if unroll or r.random() < 0.5:
                bound = str(r.randint(1, 5))
            else:
                bound = "(%s & 7)" % self.expr(2)
                self.n += 1
                self.emit("int n%d = %s;" % (self.n, bound))
                bound = "n%d" % self.n
            if unroll:
                self.emit("#pragma hls unroll")
            self.emit("for (int %s = 0; %s < %s; %s++) {" % (i, i, bound, i))
            self.ro.append(i)
            self.loop += 1
            self.block(r.randint(1, 3))
            if not unroll and r.random() < 0.3:
                self.emit("  if (%s) %s;" % (self.expr(2), r.choice(["break", "continue"])))
                self.block(1)
            self.loop -= 1
            self.ro.remove(i)
            self.emit("}")
        elif k < 0.915 and self.depth < 2:
            self.emit("switch (%s & 3) {" % self.expr(2))
            for c in r.sample([0, 1, 2, 3], r.randint(1, 3)):
                self.emit("  case %d: {" % c)
                self.block(r.randint(1, 2))
                self.emit("  }")
                if r.random() < 0.75:
                    self.emit("    break;")
            if r.random() < 0.6:
                self.emit("  default: {")
                self.block(1)
                self.emit("  }")
            self.emit("}")
        elif k < 0.93 and not self.pipe and self.depth > 0 and self.loop == 0:
            self.emit("if (%s) return %s;" % (self.expr(2), self.expr(1)))
        elif k < 0.955 and self.pts:
            d = r.choice(self.pts)
            if r.random() < 0.6:
                self.emit("%s = hmix(%s, %s);" % (d, r.choice(self.pts), r.choice(self.pts)))
            else:
                self.emit("%s = %s;" % (d, r.choice(self.pts)))
        elif [v for v in self.vars if v[1] != "bool"]:
            n, _ = r.choice([v for v in self.vars if v[1] != "bool"])
            self.emit("(%s)%s;" % (n, r.choice(["++", "--"])))

    def piped_loop(self):
        """A loop over src and dst that can start an iteration every cycle or every few."""
        r = self.r
        self.n += 1
        i = "i%d" % self.n
        if r.random() < 0.5:
            bound = str(r.randint(2, 14))
        else:
            self.emit("int n%d = (%s) & 15;" % (self.n, self.expr(2)))
            bound = "n%d" % self.n
        self.emit("for (int %s = 0; %s < %s; %s++) {" % (i, i, bound, i))
        arrs, self.arrs = self.arrs, [a for a in self.arrs if a[0] == "rom"]
        self.ro.append(i)
        self.depth += 1
        nv = len(self.vars)
        x = "x%d" % self.n
        self.emit("%s %s = src[(%s + %d) & 15];" % (self.src_t, x, i, r.randint(0, 3)))
        self.ro.append(x)
        for _ in range(r.randint(1, 3)):
            k = r.random()
            if k < 0.6 and self.vars:
                n, _t = r.choice(self.vars)
                self.emit("%s %s %s;" % (n, r.choice(["=", "+=", "-=", "^=", "|=", "&="]), self.expr(1)))
            elif k < 0.8:
                self.n += 1
                n, t = "v%d" % self.n, self.ty()
                self.emit("%s %s = %s;" % (t, n, self.expr(1)))
                self.vars.append((n, t))
            elif self.vars:
                n, _t = r.choice(self.vars)
                self.emit("if (%s) %s = %s;" % (self.expr(2), n, self.expr(2)))
        k = r.random()
        if k < 0.45:
            self.emit("dst[%s] = %s;" % (i, self.expr(1)))
        elif k < 0.65:   # updated in place: one iteration every second cycle at best
            self.emit("dst[%s & 15] %s %s;" % (r.choice([i, x]), r.choice(["+=", "^=", "-="]), self.expr(1)))
        elif k < 0.8 and self.vars:   # a value carried through a memory read
            n, _t = r.choice(self.vars)
            self.emit("%s = src[%s & 15] + %s;" % (n, n, r.choice([i, x, "1"])))
        del self.vars[nv:]
        self.depth -= 1
        self.ro.remove(i)
        self.ro.remove(x)
        self.arrs = arrs
        self.emit("}")

    def block(self, n):
        self.depth += 1
        nv = len(self.vars)
        for _ in range(n):
            self.stmt()
        del self.vars[nv:]
        self.depth -= 1

    def program(self):
        r = self.r
        params = []
        for i in range(r.randint(1, 5)):
            t = self.ty()
            params.append("%s p%d" % (t, i))
            self.vars.append(("p%d" % i, t))
        out = []
        out.append("#include <stdint.h>")
        if r.random() < 0.6:
            t = r.choice(TYPES[:8])
            out.append("static const %s rom[8] = {%s};" % (t, ", ".join(str(r.randint(-200, 70000)) for _ in range(8))))
            self.arrs.append(("rom", 8, False))
        if r.random() < 0.4:
            t = r.choice(TYPES[:8])
            out.append("static %s gstate = %d;" % (t, r.randint(0, 50)))
            self.vars.append(("gstate", t))
        structs = r.random() < 0.4
        late = []
        if structs:
            ta, tb, tk, tarr = self.ty(), self.ty(), self.ty(), r.choice(TYPES[:8])
            out.append("typedef struct { %s a; %s b; } pt_t;" % (ta, tb))
            out.append("typedef struct { pt_t p; %s k; %s arr[4]; } rec_t;" % (tk, tarr))
            out.append("static pt_t hmix(pt_t x, pt_t y) {\n  pt_t r;\n  r.a = x.a + y.b;\n  r.b = x.b ^ y.a;\n"
                       "  if (x.a > y.a) r.a = x.b;\n  return r;\n}")

            def pt(n):
                self.pts.append(n)
                self.vars += [(n + ".a", ta), (n + ".b", tb)]

            if r.random() < 0.6:
                params.append("pt_t q0")
                pt("q0")
            if not self.pipe and r.random() < 0.5:
                params.append("rec_t %sr0" % r.choice(["*", "&"]))
                acc = "r0->" if params[-1].startswith("rec_t *") else "r0."
                pt(acc + "p")
                self.vars.append((acc + "k", tk))
                self.arrs.append((acc + "arr", 4, True))
            if r.random() < 0.7:
                late.append("pt_t l0 = {%s, %s};" % (self.expr(2), self.expr(2)))
                late.append(("pt", "l0"))
            if r.random() < 0.5:
                late.append("rec_t l1 = {{%s, %s}, %s, {%s, %s}};" % tuple(self.expr(2) for _ in range(5)))
                late.append(("rec", "l1", tk))
        outp = None
        if r.random() < 0.5:
            t = self.ty()
            outp = r.choice(["*", "&"])
            params.append("%s %so0" % (t, outp))
            self.vars.append(("*o0" if outp == "*" else "o0", t))
        if not self.pipe and r.random() < 0.4:
            t = r.choice(TYPES[:8])
            params.append("%s m0[16]" % t)
            self.arrs.append(("m0", 16, True))
        if self.loopy:
            self.src_t = r.choice(TYPES[:8])
            params.append("const %s src[16]" % self.src_t)
            params.append("%s dst[16]" % r.choice(TYPES[:8]))
        if self.streams:
            out.insert(1, '#include "%s"' % os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "include", "mhls_stream.h"))
            params.append("hls::stream<uint8_t>& sin")
            params.append("hls::stream<uint16_t>& sout")
        ret = self.ty()
        out.append("%s f(%s) {" % (ret, ", ".join(params)))
        for x in late:
            if isinstance(x, str):
                self.emit(x)
            elif x[0] == "pt":
                self.pts.append(x[1])
                self.vars += [(x[1] + ".a", ta), (x[1] + ".b", tb)]
            else:
                self.pts.append(x[1] + ".p")
                self.vars += [(x[1] + ".p.a", ta), (x[1] + ".p.b", tb), (x[1] + ".k", x[2])]
                self.arrs.append((x[1] + ".arr", 4, True))
        if r.random() < 0.5:
            t = r.choice(TYPES[:8])
            size = r.choice([4, 8]) if self.pipe else r.choice([4, 8, 16])
            self.emit("%s la[%d] = {%s};" % (t, size, ", ".join(self.expr(2) for _ in range(r.randint(1, size)))))
            self.arrs.append(("la", size, True))
        if r.random() < 0.3:
            t = r.choice(TYPES[:8])
            self.emit("static %s acc = %d;" % (t, r.randint(0, 9)))
            self.vars.append(("acc", t))
        self.depth = -1
        self.block(r.randint(3, 9))
        self.depth = 0
        if self.loopy:
            for _ in range(r.randint(1, 2)):
                self.piped_loop()
                self.depth = -1
                self.block(r.randint(0, 2))
                self.depth = 0
        self.emit("return %s;" % self.expr())
        return "\n".join(out + self.lines + ["}"]) + "\n"

    def options(self):
        r = self.r
        o = ["-clock", r.choice(["1.5", "3", "10", "40"])]
        if self.pipe:
            o = ["-pipeline", "-clock", r.choice(["12", "20", "40"])]
        else:
            if r.random() < 0.3:
                o += ["-mul", "1"]
            if r.random() < 0.2:
                o += ["-div", "1"]
            if r.random() < 0.2:
                o += ["-no-share"]
            if r.random() < 0.3:
                o += ["-max-reg-array", "4"]
            if r.random() < 0.2:
                o += ["-unroll-all"]
            if r.random() < 0.35 or self.loopy:
                o += ["-pipeline-loops"]
            if self.loopy:
                o[1] = r.choice(["1.5", "3", "5", "10"])
        if r.random() < 0.15:
            o += ["-handshake"]
        return o


def run(cmd, timeout=60):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "timed out: " + " ".join(cmd)
    return p.returncode, p.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mhls", required=True)
    ap.add_argument("--msim")
    ap.add_argument("--cxx", default="c++")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seeds", default="0:20")
    ap.add_argument("--vectors", default="12")
    a = ap.parse_args()
    lo, hi = [int(x) for x in a.seeds.split(":")]
    os.makedirs(a.out, exist_ok=True)
    bad = skipped = 0
    for seed in range(lo, hi):
        g = Gen(seed)
        src = g.program()
        opts = g.options()
        base = os.path.join(a.out, "f%d" % seed)
        with open(base + ".c", "w") as f:
            f.write(src)
        cmd = [a.mhls, base + ".c", "-top", "f", "-o", base + ".sv", "-tb", base + "_tb.sv", "-cref", base + "_ref.cpp",
               "-vectors", a.vectors, "-seed", str(seed + 1), "-q"] + opts
        rc, out = run(cmd)
        if rc != 0:
            # A pipeline that cannot meet one input per cycle is a legal refusal.
            if g.pipe and "cannot be pipelined" in out:
                skipped += 1
                continue
            print("seed %d: mhls failed: %s\n%s" % (seed, " ".join(cmd), out))
            bad += 1
            continue
        rc, out = run([a.cxx, "-std=c++17", "-fwrapv", "-w", "-o", base + "_ref", base + "_ref.cpp"])
        if rc == 0:
            rc, out = run([base + "_ref"])
        if rc != 0 or "PASS" not in out:
            print("seed %d: host compiler disagrees (%s)\n%s" % (seed, " ".join(opts), out[-600:]))
            bad += 1
            continue
        if a.msim:
            rc, out = run([a.msim, base + ".sv", base + "_tb.sv", "-top", "f_tb"])
            if "PASS" not in out:
                print("seed %d: RTL simulation disagrees (%s)\n%s" % (seed, " ".join(opts), out[-600:]))
                bad += 1
    print("mhls fuzz: %d seeds, %d failed, %d pipelines refused" % (hi - lo, bad, skipped))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
