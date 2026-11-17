#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""bench/manycore/fetch_cva6.py DIR - get CVA6 (Ariane) for gen_manycore.py --core cva6.

Clones OpenHW CVA6 at a pinned commit into DIR/cva6 (with the cvfpu and
hpdcache submodules it compiles), writes DIR/mc_cva6_config_pkg.sv and a
flat file list DIR/cva6.f. Nothing from CVA6 is copied into minisim.

Licences (all permissive): CVA6, its vendored pulp-platform IP and cvfpu are
Solderpad 0.51 / Apache-2.0; hpdcache is Solderpad 2.1; cvfpu's vendored
T-Head divider (openc910/opene906) is Apache-2.0.

The config is cv32a6_imac_sv32 (RV32IMAC, write-through D-cache, AXI) with
the changes the many-core needs:
  * only the private RAM (0x0-0xFFF) is cacheable, so every shared access
    leaves the core and goes through the tile's coherent L1;
  * 0xC000_0000-0xFFFF_FFFF (fetch-and-increment alias, MMIO) is
    non-idempotent: loads there are never speculative or repeated;
  * code executes only from the private RAM;
  * no debug module. The MMU stays (bare mode in M-mode): with
    MmuPresent = 0 CVA6 checks non-idempotence against the previous load's
    address register, and a fetch-and-increment load was issued speculatively
    and then replayed (seen on Verilator 5.054, CVA6 742dc82a).

  python3 bench/manycore/fetch_cva6.py cva6dir
  python3 bench/gen_manycore.py --core cva6 --mesh 2x2 > mc.sv
  msim -f cva6dir/cva6.f mc.sv -top mc_tb
"""
import os
import re
import subprocess
import sys

REPO = "https://github.com/openhwgroup/cva6"
COMMIT = "742dc82a0c706e0a3bfb98b709fd3ee06d50d12a"   # master, 2026-10-23
BASE_CFG = "cv32a6_imac_sv32"


def sh(*cmd, cwd=None):
    subprocess.run(cmd, cwd=cwd, check=True)


def config(src):
    s = open(src).read()
    subs = [
        (r"localparam config_pkg::cache_type_t CVA6ConfigDcacheType = [\w:]+;",
         "localparam config_pkg::cache_type_t CVA6ConfigDcacheType = config_pkg::WT;"),
        (r"NonIdempotentAddrBase: [^\n]*", "NonIdempotentAddrBase: 1024'({64'hC000_0000}),"),
        (r"NonIdempotentLength: [^\n]*", "NonIdempotentLength: 1024'({64'h4000_0000}),"),
        (r"NrExecuteRegionRules: [^\n]*", "NrExecuteRegionRules: unsigned'(1),"),
        (r"ExecuteRegionAddrBase: [^\n]*", "ExecuteRegionAddrBase: 1024'({64'h0}),"),
        (r"ExecuteRegionLength: [^\n]*", "ExecuteRegionLength: 1024'({64'h1000}),"),
        (r"CachedRegionAddrBase: [^\n]*", "CachedRegionAddrBase: 1024'({64'h0}),"),
        (r"CachedRegionLength: [^\n]*", "CachedRegionLength: 1024'({64'h1000}),"),
        (r"DebugEn: [^\n]*", "DebugEn: bit'(0),"),
    ]
    for pat, rep in subs:
        s, k = re.subn(pat, rep, s)
        if k != 1:
            sys.exit(f"fetch_cva6.py: {BASE_CFG} config changed, cannot apply {pat!r}")
    return f"// Derived by bench/manycore/fetch_cva6.py from CVA6 {BASE_CFG}_config_pkg.sv\n" \
           f"// (Solderpad 0.51, OpenHW Group) for the gen_manycore.py tile.\n" + s


def flatten(path, env, out):
    for line in open(path):
        line = line.split("//")[0].strip()
        if not line:
            continue
        for k, v in env.items():
            line = line.replace("${" + k + "}", v)
        if line.startswith("-F ") or line.startswith("-f "):
            flatten(line[3:].strip(), env, out)
        else:
            out.append(line)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = os.path.abspath(sys.argv[1])
    repo = os.path.join(d, "cva6")
    if not os.path.isdir(os.path.join(repo, ".git")):
        os.makedirs(repo, exist_ok=True)
        sh("git", "init", "-q", cwd=repo)
        sh("git", "remote", "add", "origin", REPO, cwd=repo)
    sh("git", "fetch", "-q", "--depth", "1", "origin", COMMIT, cwd=repo)
    sh("git", "checkout", "-q", "FETCH_HEAD", cwd=repo)
    sh("git", "submodule", "update", "-q", "--init", "--recursive", "--depth", "1", "core/cvfpu",
       "core/cache_subsystem/hpdcache", cwd=repo)
    cfg = os.path.join(d, "mc_cva6_config_pkg.sv")
    open(cfg, "w").write(config(os.path.join(repo, "core", "include", BASE_CFG + "_config_pkg.sv")))
    env = {"CVA6_REPO_DIR": repo, "HPDCACHE_DIR": os.path.join(repo, "core", "cache_subsystem", "hpdcache"),
           "TARGET_CFG": BASE_CFG}
    files = []
    flatten(os.path.join(repo, "core", "Flist.cva6"), env, files)
    target = os.path.join(repo, "core", "include", BASE_CFG + "_config_pkg.sv")
    files = [cfg if f == target else f for f in files]
    # the AXI typedef macros the tile wrapper uses
    files.insert(0, "+incdir+" + os.path.join(repo, "vendor", "pulp-platform", "axi", "include"))
    open(os.path.join(d, "cva6.f"), "w").write("\n".join(files) + "\n")
    print(f"CVA6 {COMMIT[:12]} in {repo}; file list {os.path.join(d, 'cva6.f')} ({len(files)} entries)")


if __name__ == "__main__":
    main()
