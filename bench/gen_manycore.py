#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""bench/gen_manycore.py - a tiled many-core SoC in the style of OpenPiton.

  python3 bench/gen_manycore.py [options] > mc.sv
  msim -compile picorv32.v mc.sv -top mc_tb
  verilator --binary -O3 -Wno-fatal --top-module mc_tb picorv32.v mc.sv && obj_dir/Vmc_tb

The chip is an X x Y mesh of identical tiles. Every tile has
  * a RISC-V core: mcrv (ours, in bench/manycore/mc_rv.sv, nothing to fetch),
    picorv32, femtorv32, cva6 (OpenHW CVA6/Ariane, RV32IMAC with its own
    I- and D-caches), or a mix,
  * 4 KB of private RAM with the program,
  * a private L1 data cache (direct-mapped, write-through),
  * one slice of the distributed shared L2 (lines homed by address), with a
    limited-pointer directory that invalidates sharers on a write
    (broadcast once more tiles share a line than it has pointers),
  * three routers, one per physical network: NoC1 requests, NoC2 data,
    write acks and invalidations, NoC3 invalidation acks (XY routing).
So unlike bench/gen_rv_soc.py (N copies that never talk), the cores share
memory: every access to the shared region crosses the mesh to the line's
home tile, and writes invalidate other tiles' copies.

The program (the same image in every tile) runs ROUNDS rounds. In each round
core i runs a xorshift32 loop for a core-dependent number of steps (so some
cores wait at the barrier while others compute), writes its value to a shared
array, joins a barrier (fetch-and-increment on a counter, then the last core
releases a flag that the others spin on in their L1), and mixes in the
values of tile i+1 and tile i+X. Then core 0 sums every core's final value
into a checksum. The testbench compares it with the value this script
computes, so a run checks the cache coherence, the NoC and the cores, not
just that nothing hangs:

  mc: tiles=16 cores=picorv32 done=16/16 cycles=... checksum=... expect=... PASS

Options
  --mesh XxY        tiles (default 4x4); 1x1 up to hundreds of tiles
  --core C          mcrv (default), picorv32, femtorv32, cva6, or mix (checkerboard of
                    picorv32 and femtorv32). A comma list (mcrv,picorv32,...) is
                    repeated over the tiles.
  --rounds R        exchange rounds (default 4)
  --work K          base xorshift steps per round; core i adds (i mod 16)*8 (default 32)
  --max-cycles M    give up after M cycles (default: estimated from the work)
  --l2-lines L      lines per L2 slice (default 64, i.e. 1 KB per tile)
  --dir-ptrs P      directory pointers per line before broadcast (default 4)

Outside cores (not part of minisim; fetch them, permissive licences only):
  picorv32   ISC, https://raw.githubusercontent.com/YosysHQ/picorv32/main/picorv32.v
  femtorv32  BSD-3, https://raw.githubusercontent.com/BrunoLevy/learn-fpga/master/FemtoRV/RTL/PROCESSOR/femtorv32_quark.v
             (compile with -DBENCH, see bench/README.md)
  cva6       Solderpad 0.51 (OpenHW), python3 bench/manycore/fetch_cva6.py DIR, then
             compile with -f DIR/cva6.f before the generated file
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORES = {"picorv32": 0, "femtorv32": 1, "mcrv": 2, "cva6": 3}

# ---------------------------------------------------------------- assembler
REGS = 16  # the program only uses x0..x15 (RV32E-compatible)


def _r(f7, rs2, rs1, f3, rd): return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x33
def _i(imm, rs1, f3, rd, op=0x13): return ((imm & 0xfff) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
def _s(imm, rs2, rs1): return ((imm >> 5 & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | (2 << 12) | ((imm & 0x1f) << 7) | 0x23
def _b(off, rs2, rs1, f3):
    o = off & 0x1fff
    return ((o >> 12 & 1) << 31) | ((o >> 5 & 0x3f) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | \
           ((o >> 1 & 0xf) << 8) | ((o >> 11 & 1) << 7) | 0x63
def _j(off, rd):
    o = off & 0x1fffff
    return ((o >> 20 & 1) << 31) | ((o >> 1 & 0x3ff) << 21) | ((o >> 11 & 1) << 20) | ((o >> 12 & 0xff) << 12) | (rd << 7) | 0x6f


class Asm:
    def __init__(self):
        self.items, self.labels = [], {}

    def label(self, name): self.labels[name] = len(self.items) * 4
    def emit(self, f): self.items.append(f)

    def __getattr__(self, op):
        enc = {
            "lw": lambda rd, imm, rs1=0: lambda pc: _i(imm, rs1, 2, rd, 0x03),
            "sw": lambda rs2, imm, rs1=0: lambda pc: _s(imm, rs2, rs1),
            "addi": lambda rd, rs1, imm: lambda pc: _i(imm, rs1, 0, rd),
            "andi": lambda rd, rs1, imm: lambda pc: _i(imm, rs1, 7, rd),
            "slli": lambda rd, rs1, sh: lambda pc: _i(sh, rs1, 1, rd),
            "srli": lambda rd, rs1, sh: lambda pc: _i(sh, rs1, 5, rd),
            "add": lambda rd, a, b: lambda pc: _r(0, b, a, 0, rd),
            "sub": lambda rd, a, b: lambda pc: _r(0x20, b, a, 0, rd),
            "xor": lambda rd, a, b: lambda pc: _r(0, b, a, 4, rd),
            "or_": lambda rd, a, b: lambda pc: _r(0, b, a, 6, rd),
            "bne": lambda a, b, l: lambda pc: _b(self.labels[l] - pc, b, a, 1),
            "bltu": lambda a, b, l: lambda pc: _b(self.labels[l] - pc, b, a, 6),
            "jal": lambda rd, l: lambda pc: _j(self.labels[l] - pc, rd),
            "fence": lambda: lambda pc: 0x0330000f,             # fence rw,rw
        }[op]

        def f(*a):
            for r in a:
                assert not isinstance(r, int) or r < 4096
            self.emit(enc(*a))
        return f

    def words(self): return [f(4 * k) for k, f in enumerate(self.items)]


SHB, AMOB, MMIO, GOLD = 0x8000_0000, 0xC000_0000, 0xF000_0000, 0x9E3779B9
CNT, FLAG, R0 = 0x00, 0x10, 0x40
D = 0x400  # constants in private RAM
D_N, D_X, D_ROUNDS, D_SHB, D_AMOB, D_MMIO, D_GOLD, D_OFF, D_R0, D_S, D_WORK = (D + 4 * k for k in range(11))


def layout(n):
    arr = (4 * n + 15) & ~15
    return R0 + arr, R0 + 2 * arr      # R1, S


def program(n, x, rounds, work, fences=False):
    r1, s = layout(n)
    a = Asm()
    a.lw(13, D_MMIO); a.lw(4, 8, 13)            # x4 = id
    a.lw(5, D_N); a.lw(9, D_SHB)
    a.slli(10, 4, 2)                            # own slot offset
    a.addi(11, 4, 1); a.bne(11, 5, "nb1"); a.addi(11, 0, 0)
    a.label("nb1")
    a.lw(2, D_X); a.add(12, 4, 2); a.bltu(12, 5, "nb2"); a.sub(12, 12, 5)
    a.label("nb2")
    a.slli(11, 11, 2); a.slli(12, 12, 2)
    a.lw(2, D_GOLD); a.addi(1, 4, 1); a.xor(1, 1, 2)   # seed
    a.addi(6, 0, 0); a.lw(15, D_OFF); a.addi(8, 0, 0); a.lw(7, D_ROUNDS)

    def barrier(tag):
        # RVWMO: on an out-of-order core (cva6) fences order this core's shared
        # stores before the arrival and its shared loads after it. The in-order
        # cores do not need them, and FemtoRV32 Quark does not decode FENCE.
        if fences:
            a.fence()
        a.add(8, 8, 5)                          # target = N * generation
        a.lw(2, D_AMOB); a.lw(14, CNT, 2)       # old = fetch_inc(CNT)
        a.addi(14, 14, 1); a.bne(14, 8, "spin" + tag)
        a.sw(8, FLAG, 9)                        # last arrival releases
        a.label("spin" + tag)
        a.lw(14, FLAG, 9); a.bltu(14, 8, "spin" + tag)
        if fences:
            a.fence()

    a.label("round")
    a.andi(3, 4, 15); a.slli(3, 3, 3); a.lw(2, D_WORK); a.add(3, 3, 2)
    a.label("loop")
    a.slli(2, 1, 13); a.xor(1, 1, 2); a.srli(2, 1, 17); a.xor(1, 1, 2); a.slli(2, 1, 5); a.xor(1, 1, 2)
    a.addi(3, 3, -1); a.bne(3, 0, "loop")
    a.lw(13, D_R0); a.add(13, 13, 9); a.add(13, 13, 6)
    a.add(14, 13, 10); a.sw(1, 0, 14)
    barrier("a")
    a.add(14, 13, 11); a.lw(2, 0, 14); a.add(14, 13, 12); a.lw(14, 0, 14)
    a.xor(2, 2, 14); a.add(1, 1, 2)
    a.xor(6, 6, 15)
    a.addi(7, 7, -1); a.bne(7, 0, "round")
    a.lw(13, D_S); a.add(13, 13, 9); a.add(14, 13, 10); a.sw(1, 0, 14)
    barrier("b")
    a.bne(4, 0, "done")
    a.addi(2, 0, 0); a.addi(3, 5, 0); a.addi(14, 13, 0)
    a.label("sum")
    a.lw(1, 0, 14); a.slli(6, 2, 1); a.srli(7, 2, 31); a.or_(2, 6, 7); a.xor(2, 2, 1)
    a.addi(14, 14, 4); a.addi(3, 3, -1); a.bne(3, 0, "sum")
    a.lw(13, D_MMIO); a.sw(2, 4, 13)
    a.label("done")
    a.lw(13, D_MMIO); a.addi(2, 0, 1); a.sw(2, 0, 13)
    a.label("halt")
    a.jal(0, "halt")
    code = a.words()
    assert len(code) * 4 <= D, "program too long"
    data = {D_N: n, D_X: x, D_ROUNDS: rounds, D_SHB: SHB, D_AMOB: AMOB, D_MMIO: MMIO, D_GOLD: GOLD,
            D_OFF: r1 - R0, D_R0: R0, D_S: s, D_WORK: work}
    return code, data


def golden(n, x, rounds, work):
    M = 0xffffffff
    v = [((i + 1) ^ GOLD) & M for i in range(n)]
    for _ in range(rounds):
        for i in range(n):
            for _ in range(work + ((i & 15) << 3)):
                t = v[i]
                t ^= (t << 13) & M; t ^= t >> 17; t ^= (t << 5) & M
                v[i] = t
        r = list(v)
        for i in range(n):
            v[i] = (v[i] + (r[(i + 1) % n] ^ r[(i + x) % n])) & M
    chk = 0
    for i in range(n):
        chk = (((chk << 1) | (chk >> 31)) & M) ^ v[i]
    return chk


# ---------------------------------------------------------------- RTL
CORE_RTL = {
    "cva6": """    if (CORE == 3) begin : g_cva6
      mc_cva6 cpu(.clk(clk), .rst_n(rst_n), .req_valid(req_valid), .req_addr(req_addr), .req_wdata(req_wdata),
                  .req_wstrb(req_wstrb), .req_ready(req_ready), .rdata(rdata));
    end
""",
    "mcrv": """    if (CORE == 2) begin : g_mcrv
      mc_rv cpu(.clk(clk), .rst_n(rst_n), .req_valid(req_valid), .req_addr(req_addr), .req_wdata(req_wdata),
                .req_wstrb(req_wstrb), .req_ready(req_ready), .rdata(rdata));
    end
""",
    "picorv32": """    if (CORE == 0) begin : g_picorv32
      logic mem_valid, mem_instr;
      picorv32 #(.ENABLE_COUNTERS(0), .ENABLE_COUNTERS64(0), .CATCH_MISALIGN(0), .CATCH_ILLINSN(0))
        cpu(.clk(clk), .resetn(rst_n), .mem_valid(mem_valid), .mem_instr(mem_instr), .mem_ready(req_ready),
            .mem_addr(req_addr), .mem_wdata(req_wdata), .mem_wstrb(req_wstrb), .mem_rdata(rdata),
            .pcpi_wr(1'b0), .pcpi_rd(32'd0), .pcpi_wait(1'b0), .pcpi_ready(1'b0), .irq(32'd0), .trace_valid(), .trace_data(),
            .mem_la_read(), .mem_la_write(), .mem_la_addr(), .mem_la_wdata(), .mem_la_wstrb(), .pcpi_valid(), .pcpi_insn(),
            .pcpi_rs1(), .pcpi_rs2(), .eoi(), .trap());
      assign req_valid = mem_valid;
    end
""",
    "femtorv32": """    if (CORE == 1) begin : g_femtorv32
      // FemtoRV32 strobes a read or write for one cycle and then waits while
      // busy: hold the request until the tile answers.
      logic [31:0] addr, wdata, rd_q;
      logic [3:0] wmask;
      logic rstrb, pend;
      FemtoRV32 #(.ADDR_WIDTH(32)) cpu(.clk(clk), .reset(rst_n), .mem_addr(addr), .mem_wdata(wdata),
        .mem_wmask(wmask), .mem_rdata(rd_q), .mem_rstrb(rstrb), .mem_rbusy(pend), .mem_wbusy(pend));
      always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) pend <= 1'b0;
        else if (pend && req_ready) begin
          pend <= 1'b0;
          rd_q <= rdata;
        end else if (!pend && (rstrb || wmask != 4'd0)) begin
          pend <= 1'b1;
          req_addr <= addr;
          req_wdata <= wdata;
          req_wstrb <= wmask;
        end
      end
      assign req_valid = pend;
    end
""",
}


def mc_core(kinds):
    body = "".join(CORE_RTL[k] for k in sorted(kinds, key=lambda k: CORES[k]))
    return f"""// mc_core: the tile's RISC-V core behind one held-request memory port
// (valid stays high until the one-cycle ready; read data comes with ready).
module mc_core #(parameter int CORE = 0) (
  input  logic clk, rst_n,
  output logic req_valid,
  output logic [31:0] req_addr, req_wdata,
  output logic [3:0] req_wstrb,
  input  logic req_ready,
  input  logic [31:0] rdata
);
  generate
{body}  endgenerate
endmodule
"""


def top(x, y, xw, yw, kinds_of, args, n, expect, max_cycles, cores_desc):
    out = []
    w = out.append
    w(f"""module mc_soc (input logic clk, rst_n, output logic [{n - 1}:0] done, output logic [31:0] result,
               output logic [31:0] l2req [{n}]);
  localparam int X = {x}, Y = {y}, XW = {xw}, YW = {yw};
  localparam int FW = XW + YW + XW + YW + 3 + 28 + 4 + 128;
  // per tile and network: valid/flit/ready leaving through each side (0 N, 1 E, 2 S, 3 W)
  wire [2:0][3:0] ov [Y][X], iv [Y][X], ordy [Y][X], irdy [Y][X];
  wire [2:0][3:0][FW-1:0] of [Y][X], ifl [Y][X];
  wire [31:0] res [Y][X];
@CORE_OF@
  for (genvar ty = 0; ty < Y; ty++) begin : row
    for (genvar tx = 0; tx < X; tx++) begin : col
      for (genvar n = 0; n < 3; n++) begin : link
        // north side <- south side of (tx, ty-1), and so on; mesh edges are tied off
        assign iv[ty][tx][n][0]  = ty > 0     ? ov[ty > 0 ? ty-1 : 0][tx][n][2] : 1'b0;
        assign ifl[ty][tx][n][0] = ty > 0     ? of[ty > 0 ? ty-1 : 0][tx][n][2] : '0;
        assign ordy[ty][tx][n][0] = ty > 0    ? irdy[ty > 0 ? ty-1 : 0][tx][n][2] : 1'b1;
        assign iv[ty][tx][n][2]  = ty < Y - 1 ? ov[ty < Y-1 ? ty+1 : ty][tx][n][0] : 1'b0;
        assign ifl[ty][tx][n][2] = ty < Y - 1 ? of[ty < Y-1 ? ty+1 : ty][tx][n][0] : '0;
        assign ordy[ty][tx][n][2] = ty < Y - 1 ? irdy[ty < Y-1 ? ty+1 : ty][tx][n][0] : 1'b1;
        assign iv[ty][tx][n][1]  = tx < X - 1 ? ov[ty][tx < X-1 ? tx+1 : tx][n][3] : 1'b0;
        assign ifl[ty][tx][n][1] = tx < X - 1 ? of[ty][tx < X-1 ? tx+1 : tx][n][3] : '0;
        assign ordy[ty][tx][n][1] = tx < X - 1 ? irdy[ty][tx < X-1 ? tx+1 : tx][n][3] : 1'b1;
        assign iv[ty][tx][n][3]  = tx > 0     ? ov[ty][tx > 0 ? tx-1 : 0][n][1] : 1'b0;
        assign ifl[ty][tx][n][3] = tx > 0     ? of[ty][tx > 0 ? tx-1 : 0][n][1] : '0;
        assign ordy[ty][tx][n][3] = tx > 0    ? irdy[ty][tx > 0 ? tx-1 : 0][n][1] : 1'b1;
      end
      mc_tile #(.XW(XW), .YW(YW), .X(X), .Y(Y), .CORE(CORE_OF(ty * X + tx)),
                .L2_LINES({args.l2_lines}), .DIR_PTRS({args.dir_ptrs})) t (
        .clk(clk), .rst_n(rst_n), .my_x(XW'(tx)), .my_y(YW'(ty)), .my_id(32'(ty * X + tx)),
        .in_v(iv[ty][tx]), .in_f(ifl[ty][tx]), .in_rdy(irdy[ty][tx]),
        .out_v(ov[ty][tx]), .out_f(of[ty][tx]), .out_rdy(ordy[ty][tx]),
        .done(done[ty * X + tx]), .result(res[ty][tx]), .n_l2req(l2req[ty * X + tx]));
    end
  end
  assign result = res[0][0];
endmodule""")
    pattern = [CORES[k] for k in kinds_of]
    f = []
    if len(set(pattern)) == 1:
        f.append(f"  function automatic int CORE_OF(int i); return {pattern[0]}; endfunction")
    else:
        f.append("  function automatic int CORE_OF(int i);")
        f.append(f"    case (i % {len(pattern)})")
        for k, c in enumerate(pattern):
            f.append(f"      {k}: return {c};")
        f.append("      default: return 0;")
        f.append("    endcase")
        f.append("  endfunction")
    out[0] = out[0].replace("@CORE_OF@", "\n".join(f))
    w(f"""
module mc_tb;
  logic clk = 1'b0, rst_n = 1'b0;
  logic [{n - 1}:0] done;
  logic [31:0] result;
  logic [31:0] l2req [{n}];
  mc_soc soc(.clk(clk), .rst_n(rst_n), .done(done), .result(result), .l2req(l2req));
  always #5 clk = ~clk;
  initial begin
    longint cyc;
    longint reqs;
    int nd;
    #20 rst_n = 1'b1;
    cyc = 0;
    while (!(&done) && cyc < {max_cycles}) begin
      @(posedge clk);
      cyc++;
    end
    nd = 0;
    reqs = 0;
    for (int i = 0; i < {n}; i++) begin
      if (done[i]) nd++;
      reqs += l2req[i];
    end
    $display("mc: tiles={n} mesh={x}x{y} cores={cores_desc} done=%0d/{n} cycles=%0d l2_requests=%0d checksum=%h expect={expect:08x} %s",
             nd, cyc, reqs, result, (done[0] && result == 32'h{expect:08x}) ? "PASS" : "FAIL");
    $finish;
  end
endmodule""")
    return "\n".join(out)


def parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mesh", default="4x4")
    ap.add_argument("--core", default="mcrv")
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--work", type=int, default=32)
    ap.add_argument("--max-cycles", type=int, default=0)
    ap.add_argument("--l2-lines", type=int, default=64)
    ap.add_argument("--dir-ptrs", type=int, default=4)
    return ap


class GenError(Exception):
    pass


def generate(args, cmdline=""):
    """the design as one SystemVerilog text, plus facts other generators need
    (tile count, layout, constants' addresses, expected checksum).
    args: the parsed options (parser().parse_args([...]))."""
    x, y = (int(v) for v in args.mesh.lower().split("x"))
    n = x * y
    if x < 1 or y < 1 or args.rounds < 1 or args.work < 1:
        raise GenError("--mesh, --rounds and --work must be positive")
    kinds = ["picorv32", "femtorv32"] if args.core == "mix" else args.core.split(",")
    for k in kinds:
        if k not in CORES:
            raise GenError(f"unknown core {k!r} (mcrv, picorv32, femtorv32, cva6, mix)")
    if args.core == "mix" and x % 2 == 0:
        # checkerboard: alternate along rows and shift by one each row
        pattern = [kinds[(i % x + i // x) % 2] for i in range(n)]
    else:
        pattern = kinds
    r1, s = layout(n)
    if (s + 4 * n + 15) // 16 > n * args.l2_lines:
        raise GenError("shared data does not fit, raise --l2-lines")
    if "cva6" in pattern and "femtorv32" in pattern:
        raise GenError("cva6 needs FENCE in the program, which femtorv32 does not decode")
    code, data = program(n, x, args.rounds, args.work, fences="cva6" in pattern)
    expect = golden(n, x, args.rounds, args.work)
    xw, yw = max(1, (x - 1).bit_length()), max(1, (y - 1).bit_length())
    # cycles: ~8 cycles per instruction for the slowest core, plus barriers
    steps = args.work + 120
    max_cycles = args.max_cycles or args.rounds * (steps * 8 * 8 + 64 * n + 2000) + 64 * n + 10000

    prog = ["  initial begin", "    for (int k = 0; k < 1024; k++) ram[k] = 32'd0;"]
    prog += [f"    ram[{k}] = 32'h{w:08x};" for k, w in enumerate(code)]
    prog += [f"    ram[{a // 4}] = 32'h{v & 0xffffffff:08x};" for a, v in sorted(data.items())]
    prog.append("  end")
    used = sorted(set(pattern), key=lambda k: CORES[k])
    cores_desc = "+".join(used)
    out = [f"// Generated by bench/gen_manycore.py {cmdline}",
           f"// {x}x{y} tiles ({n} cores: {cores_desc}), {args.rounds} rounds, work {args.work}; expect checksum {expect:08x}"]
    srcs = {"picorv32": "picorv32.v", "femtorv32": "femtorv32_quark.v (with -DBENCH)",
            "cva6": "-f DIR/cva6.f from bench/manycore/fetch_cva6.py DIR (before this file)"}
    if any(k in srcs for k in used):
        out.append(f"// Compile with the core sources: {', '.join(srcs[k] for k in used if k in srcs)}")
    for f in ("mc_router.sv", "mc_l1.sv", "mc_l2.sv") + (("mc_rv.sv",) if "mcrv" in used else ()) \
            + (("mc_cva6.sv",) if "cva6" in used else ()):
        out.append(open(os.path.join(HERE, "manycore", f)).read())
    tile = open(os.path.join(HERE, "manycore", "mc_tile.sv")).read()
    marker = "  // @PROGRAM@ (bench/gen_manycore.py puts the program image here)"
    assert marker in tile
    out.append(tile.replace(marker, "\n".join(prog)))
    out.append(mc_core(used))
    out.append(top(x, y, xw, yw, pattern, args, n, expect, max_cycles, cores_desc))
    info = dict(x=x, y=y, n=n, xw=xw, yw=yw, cores=used, expect=expect, max_cycles=max_cycles,
                l2_lines=args.l2_lines, shb=SHB, r0=R0, r1=r1, s=s, d_rounds=D_ROUNDS, d_work=D_WORK,
                gold=GOLD, rounds=args.rounds, work=args.work)
    return "\n".join(out) + "\n", info


def main():
    args = parser().parse_args()
    try:
        text, _ = generate(args, " ".join(sys.argv[1:]))
    except GenError as e:
        sys.exit(f"gen_manycore.py: {e}")
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
