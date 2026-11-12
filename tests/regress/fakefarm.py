#!/usr/bin/env python3
"""A stand-in for a farm's submit command (tests/mregress.cmake):

    fakefarm.py LOGDIR NAME CPUS sh JOB

Runs the job script and returns when it is done, like bsub -K or srun.
The first submission of every job whose name contains "adder_w8" is lost
(exit 1, nothing runs), and every submission of a job whose name contains
"unique_case" is, so mregress has to resubmit one and give up on the other."""
import os
import subprocess
import sys

logdir, name, cpus = sys.argv[1:4]
os.makedirs(logdir, exist_ok=True)
with open(os.path.join(logdir, "submitted"), "a") as f:
    f.write(f"{name} cpus={cpus}\n")
if "unique_case" in name:
    print("fakefarm: host unreachable")
    sys.exit(1)
if "adder_w8" in name:
    mark = os.path.join(logdir, name + ".lost")
    if not os.path.exists(mark):
        open(mark, "w").close()
        print("fakefarm: job lost")
        sys.exit(1)
sys.exit(subprocess.call(sys.argv[4:]))
