#!/usr/bin/env python3
"""memu_mtkahypar: Mt-KaHyPar as a partitioner for memu.

    pip install mtkahypar                     # MIT licence
    memu soc.gv ... -partitioner tools/memu_mtkahypar.py

memu calls it as `memu_mtkahypar.py part.hgr K part.out THREADS` and reads
one FPGA number per vertex from part.out. Beside part.hgr memu leaves
  part.hgr.fix    the FPGA each vertex must be in, or -1
  part.hgr.caps   what each FPGA holds, in the vertex weights' unit (0: no limit)
  part.hgr.board  the pairs of FPGAs the board wires together

memu then moves cells until every FPGA's LUT, flop and block RAM limits
hold, improves the result with its own passes, and keeps whichever of this
and its own attempts runs fastest. Mt-KaHyPar knows nothing of flops, block
RAM or the board's wiring, so its answer is a starting point, not the end.

MEMU_MTKAHYPAR_PRESET picks the preset: deterministic (default), default,
quality, highest_quality.
"""
import os
import sys


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    hgr, k, out, threads = sys.argv[1], int(sys.argv[2]), sys.argv[3], max(1, int(sys.argv[4]))
    try:
        import mtkahypar
    except ImportError:
        sys.exit("memu_mtkahypar: the mtkahypar module is not installed (pip install mtkahypar)")

    mk = mtkahypar.initialize(threads)
    mtkahypar.set_seed(1)
    preset = os.environ.get("MEMU_MTKAHYPAR_PRESET", "deterministic").upper()
    ctx = mk.context_from_preset(getattr(mtkahypar.PresetType, preset))
    ctx.logging = False
    hg = mk.hypergraph_from_file(hgr, ctx, mtkahypar.FileFormat.HMETIS)

    caps = [int(x) for x in open(hgr + ".caps").read().split()] if os.path.exists(hgr + ".caps") else []
    total = hg.total_weight()
    if len(caps) == k and all(c > 0 for c in caps):
        # Each FPGA's own limit. The parts need not be equal, only fit. The
        # weights here are rounded up (a flop weighs 1, not 0), so when the
        # limits fall short of the total they are stretched to cover it, and
        # memu settles the difference.
        if sum(caps) < total * 1.02:
            scale = total * 1.02 / sum(caps)
            caps = [int(c * scale) + 1 for c in caps]
        ctx.set_partitioning_parameters(k, 0.0, mtkahypar.Objective.KM1)
        ctx.set_individual_target_block_weights(caps)
    else:
        # No limits: allow parts up to twice the average, and let the cut decide.
        ctx.set_partitioning_parameters(k, 1.0, mtkahypar.Objective.KM1)

    if os.path.exists(hgr + ".fix"):
        fix = [int(x) for x in open(hgr + ".fix").read().split()]
        if any(f >= 0 for f in fix):
            hg.add_fixed_vertices(fix, k)

    part = hg.partition(ctx)
    with open(out, "w") as f:
        for v in range(hg.num_nodes()):
            f.write("%d\n" % part.block_id(v))


if __name__ == "__main__":
    main()
