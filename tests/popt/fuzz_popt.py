#!/usr/bin/env python3
"""Random RTL for mpopt: writes modules made of enabled registers, pipelines,
case statements, memories and multipliers, runs mpopt -verify on each, and
fails on the first rewrite that does not simulate like the original.

    fuzz_popt.py <mpopt> <scratch dir> [count] [first seed]
"""
import os
import random
import subprocess
import sys


def gen(seed):
    r = random.Random(seed)
    arst = r.random() < 0.5          # asynchronous reset, else synchronous, else (rarely) none
    norst = r.random() < 0.1
    nen = r.randint(2, 4)
    ndata = r.randint(2, 4)
    nreg = r.randint(4, 12)
    ens = [f"en{i}" for i in range(nen)]
    datas = [(f"d{i}", r.choice([4, 8, 8, 16])) for i in range(ndata)]
    regs = [(f"r{i}", r.choice([1, 2, 4, 8, 8, 16, 16, 24])) for i in range(nreg)]
    mems = []
    lines = []
    ports = ["input clk", "input rst_n" if arst else "input rst"]
    ports += [f"input {e}" for e in ens]
    ports += [f"input [{w-1}:0] {n}" for n, w in datas]
    ports += ["input [1:0] sel", "output [23:0] out", "output [7:0] mout"]
    lines.append("module fz (\n  " + ",\n  ".join(ports) + "\n);")
    for n, w in regs:
        lines.append(f"  reg {'signed ' if r.random() < 0.15 else ''}[{w-1}:0] {n};")
    lines.append("  wire [15:0] cmb = " + " ^ ".join(f"{n}" for n, _ in regs[:3]) + ";")

    def cond(pool1):
        k = r.random()
        if k < 0.5 or not pool1:
            return r.choice(ens)
        if k < 0.7:
            return f"({r.choice(ens)} && !{r.choice(ens)})"
        if k < 0.85:
            return f"({r.choice(pool1)} == {r.randint(0, 3)})"
        return f"{r.choice(pool1)}[0]"

    def rhs(srcs, w):
        a = r.choice(srcs)
        k = r.random()
        if k < 0.35:
            return a
        b = r.choice(srcs)
        if k < 0.6:
            return f"{a} + {b}"
        if k < 0.75:
            return f"{a} ^ {{{b}, {b}}}"
        if k < 0.9:
            return f"{a} * {b}"
        return f"({a} > {b}) ? {a} : {b} - 1"

    def stmt(targets, srcs, condsrcs, depth, ind):
        k = r.random()
        if depth >= 3 or k < 0.35:
            t, w = r.choice(targets)
            if r.random() < 0.08 and len(targets) > 1:
                t2, _ = r.choice([x for x in targets if x[0] != t])
                return [f"{ind}{{{t}, {t2}}} <= {rhs(srcs, w)};"]
            if r.random() < 0.05:
                return [f"{ind}{t} <= {t};"]
            return [f"{ind}{t} <= {rhs(srcs, w)};"]
        if k < 0.7:
            out = [f"{ind}if ({cond(condsrcs)}) begin"]
            for _ in range(r.randint(1, 2)):
                out += stmt(targets, srcs, condsrcs, depth + 1, ind + "  ")
            if r.random() < 0.4:
                out.append(f"{ind}end else begin")
                for _ in range(r.randint(1, 2)):
                    out += stmt(targets, srcs, condsrcs, depth + 1, ind + "  ")
            out.append(f"{ind}end")
            return out
        if k < 0.9:
            selx = r.choice(["sel"] + condsrcs)
            kw = r.choice(["case", "case", "casez", "unique case"])
            out = [f"{ind}{kw} ({selx}[1:0])"]
            items = r.sample(["2'd0", "2'd1", "2'd2", "2'd3"], r.randint(1, 3))
            if kw == "casez":
                items = ["2'b1?", "2'b01"][: r.randint(1, 2)]
            for it in items:
                out.append(f"{ind}  {it}: begin")
                out += stmt(targets, srcs, condsrcs, depth + 1, ind + "    ")
                out.append(f"{ind}  end")
            if r.random() < 0.5 or kw == "unique case":
                out.append(f"{ind}  default: begin")
                if r.random() < 0.5:
                    out += stmt(targets, srcs, condsrcs, depth + 1, ind + "    ")
                out.append(f"{ind}  end")
            out.append(f"{ind}endcase")
            return out
        out = [f"{ind}begin"]
        for _ in range(2):
            out += stmt(targets, srcs, condsrcs, depth + 1, ind + "  ")
        out.append(f"{ind}end")
        return out

    left = list(regs)
    r.shuffle(left)
    done = []                         # registers of earlier blocks: pipeline sources
    multibit = lambda v: [n for n, w in v if w >= 2]
    while left:
        n = min(len(left), r.randint(1, 3))
        targets, left = left[:n], left[n:]
        mode = r.random()
        if mode < 0.45 and done:      # registers only: sequential gating applies
            srcs = [x for x, _ in done]
            condsrcs = multibit(done)
            use_inputs = False
        else:
            srcs = [x for x, _ in datas] + [x for x, _ in done] + (["cmb"] if r.random() < 0.3 else [])
            condsrcs = multibit(done) + multibit(datas)
            use_inputs = True
        own = r.random() < 0.2        # reads its own targets (a counter)
        if own:
            srcs = srcs + [targets[0][0]]
        body = []
        for _ in range(r.randint(1, 3)):
            if use_inputs or r.random() < 0.3:
                body += stmt(targets, srcs, condsrcs, 0, "      ")
            else:
                # no inputs at all, not even in conditions
                save = ens[:]
                ens[:] = [f"{x}[0]" for x in (condsrcs or [srcs[0]])]
                body += stmt(targets, srcs, condsrcs, 0, "      ")
                ens[:] = save
        rst_lines = [f"      {t} <= {r.choice(['0', '0', '1'])};" for t, _ in targets]
        edge = "posedge clk"
        if r.random() < 0.05:
            edge = "negedge clk"
        if norst and not arst:
            lines.append(f"  always @({edge}) begin")
            lines += [x[2:] for x in body]
            lines.append("  end")
        elif arst and r.random() < 0.25:
            # a reset branch of one statement, and registers with no reset
            lines.append(f"  always @({edge} or negedge rst_n)")
            lines.append("    if (!rst_n)")
            lines.append("  " + rst_lines[0])
            lines.append("    else begin")
            lines += body
            lines.append("    end")
        elif arst:
            lines.append(f"  always @({edge} or negedge rst_n)")
            lines.append("    if (!rst_n) begin")
            lines += rst_lines
            lines.append("    end else begin")
            lines += body
            lines.append("    end")
        else:
            lines.append(f"  always @({edge})")
            lines.append("    if (rst) begin")
            lines += rst_lines
            lines.append("    end else begin")
            lines += body
            lines.append("    end")
        done += targets
    # a memory with a registered read
    a = r.choice([n for n, w in regs if w >= 4] or ["d0"])
    lines.append("  reg [7:0] mem [0:15];")
    lines.append("  reg [7:0] mq;")
    lines.append(f"  always @(posedge clk) if ({r.choice(ens)}) mem[{datas[0][0]}[3:0]] <= {datas[1][0]}[7:0];" if len(datas[1][0]) and datas[1][1] >= 8
                 else f"  always @(posedge clk) if ({r.choice(ens)}) mem[{datas[0][0]}[3:0]] <= {{2{{{datas[0][0]}[3:0]}}}};")
    if arst:
        lines.append(f"  always @(posedge clk or negedge rst_n) if (!rst_n) mq <= 0; else mq <= mem[{a}[3:0]];")
    else:
        lines.append(f"  always @(posedge clk) if (rst) mq <= 0; else mq <= mem[{a}[3:0]];")
    lines.append("  assign mout = mq;")
    lines.append("  assign out = " + " ^ ".join(n for n, _ in regs) + ";")
    lines.append("endmodule")
    return "\n".join(lines) + "\n"


def main():
    mpopt, out = sys.argv[1], sys.argv[2]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    first = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    os.makedirs(out, exist_ok=True)
    gated = seq = 0
    for seed in range(first, first + count):
        src = os.path.join(out, f"fz{seed}.sv")
        with open(src, "w") as f:
            f.write(gen(seed))
        p = subprocess.run([mpopt, src, "-top", "fz", "-o", os.path.join(out, f"o{seed}"), "-verify",
                            "-cycles", "1500", "-iso_add", "16", "-min_bits", "2", "-seq_min_bits", "2"],
                           capture_output=True, text=True)
        if p.returncode != 0 or "-verify passed" not in p.stdout:
            print(f"seed {seed}: FAILED\n{p.stdout}{p.stderr}")
            return 1
        for line in p.stdout.splitlines():
            if "clock gates from enables" in line or "memory gates" in line:
                gated += int(line.split(":")[1].split()[0])
            if "sequential clock gates" in line:
                seq += int(line.split(":")[1].split()[0])
    print(f"fuzz_popt: {count} designs passed ({gated} enable and memory gates, {seq} sequential gates)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
