#!/usr/bin/env python3
"""mregress -farm slurm on a real Slurm (tests/mregress_slurm.cmake):
slurm_test.py MREGRESS.PY MSIM LIST OUT_DIR

Skips (prints "slurm_test: skipped") when no Slurm node is idle, e.g. when
tests/regress/slurm_up.sh did not run. Otherwise:
  1. the smoke list through srun gives the same verdicts as a local run,
     and the jobs really went through Slurm (job.rc written on the node);
  2. with the node full, -farm-wait cancels the queued jobs (scancel) and
     leaves nothing of ours in the queue.
"""
import os
import re
import shutil
import subprocess
import sys
import time

mr, msim, lst, out = sys.argv[1:5]


def sh(*cmd, timeout=60):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        return 1, str(e)


def fail(what, detail=""):
    raise SystemExit(f"slurm_test: {what}\n{detail}")


rc, state = sh("sinfo", "-h", "-o", "%T %c", timeout=15)
if rc != 0 or not re.search(r"^idle \d+", state, re.M):
    print("slurm_test: skipped (no idle Slurm node; tests/regress/slurm_up.sh starts one)")
    sys.exit(0)
cpus = int(re.search(r"^idle (\d+)", state, re.M).group(1))
shutil.rmtree(out, ignore_errors=True)


def regress(name, *extra):
    p = subprocess.run([sys.executable, mr, lst, "-exe", msim, "-o", os.path.join(out, name), "-seed", "7", *extra],
                       capture_output=True, text=True, timeout=600)
    verdicts = sorted(re.findall(r"^(PASS|FAIL|XFAIL|XPASS)  (\S+)  seed=(\d+)", p.stdout, re.M))
    return p.returncode, p.stdout + p.stderr, verdicts


# 1. Same verdicts as a local run. Run as many jobs at once as fit on the node,
# so nothing waits for Slurm's scheduling cycle.
lrc, lout, local = regress("local", "-j", "2")
jobs = max(1, cpus // 2)
src, sout, farm = regress("slurm", "-farm", "slurm", "-farm-cpus", "2", "-j", str(jobs))
if not local or farm != local or src != lrc:
    fail("verdicts differ between the local and the Slurm run", f"local rc={lrc}\n{lout}\nslurm rc={src}\n{sout}")
if "farm:" in sout:
    fail("a Slurm job did not run", sout)
rcs = [os.path.join(dp, "job.rc") for dp, _, fs in os.walk(os.path.join(out, "slurm", "runs")) if "job.rc" in fs]
if len(rcs) != len(farm):
    fail(f"{len(rcs)} job.rc files for {len(farm)} runs", sout)
with open(os.path.join(out, "slurm", "runs", "adder_w8_s7", "cmd.sh")) as f:
    if "-threads 2" not in f.read():
        fail("-farm-cpus did not reach msim")

# 2. A full node: our jobs queue, -farm-wait cancels them, the queue is left clean.
hog = subprocess.Popen(["srun", "-J", "slurm_test_hog", "-c", str(cpus), "sleep", "120"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(30):
        if "slurm_test_hog RUNNING" in sh("squeue", "-h", "-o", "%j %T")[1]:
            break
        time.sleep(1)
    else:
        fail("the job that fills the node did not start", sh("squeue")[1])
    wrc, wout, _ = regress("wait", "-farm", "slurm", "-farm-cpus", "1", "-j", "2", "-farm-wait", "4")
    cancelled = re.findall(r"^FAIL  \S+  seed=\d+ .*farm: no result after 4s \(queued or running\); cancelled", wout, re.M)
    if wrc != 1 or len(cancelled) != len(local):
        fail("-farm-wait did not cancel every queued job", wout)
    for _ in range(20):
        left = [j for j in sh("squeue", "-h", "-o", "%j")[1].split() if j.startswith("mregress_")]
        if not left:
            break
        time.sleep(0.5)
    else:
        fail("cancelled jobs are still in the queue", "\n".join(left))
finally:
    sh("scancel", "-n", "slurm_test_hog")
    hog.kill()
    hog.wait()
print(f"slurm_test: ok ({len(farm)} runs through srun on {cpus} CPUs, {len(cancelled)} cancelled by -farm-wait)")
