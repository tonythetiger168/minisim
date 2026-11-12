# msim -profile: where simulation time goes

`msim -profile [<file>]` times every process while the design runs and writes a
report (default `simprofile.txt`) when the simulation ends. It plays the part of
the VCS simulation profiler (`-simprofile`) and the Xcelium `-profile` report.

```sh
msim tb.sv dut.sv -top tb -profile            # writes simprofile.txt
msim tb.sv dut.sv -top tb -profile run1.txt   # a .txt name picks the file
```

## What is timed

- Every procedure (`always`, `always_ff`, `always_comb`, `initial`, `final`),
  every continuous assignment and every port connection, with the instance it
  belongs to and its first source line. A port connection counts toward the
  instance whose port it is.
- The scheduler regions that are not one process: NBA updates, the observed
  region (concurrent assertions) and the postponed region (`$strobe`,
  `$monitor`, final deferred assertions).
- Processes started while the design runs (fork branches, tasks called from
  other processes' forks) are summed in one line.

## The report

1. Simulation time, run time (from the start of the run to the end, not
   counting parsing and elaboration) and the total number of activations.
2. Time by category: processes, each scheduler region, and the rest
   (scheduler, waveform writing).
3. The 50 busiest processes: share of the run time, milliseconds,
   activations, kind, instance, source.
4. The same time summed per instance, per source file and per kind of process.

Activation counts are the same in every run; times are wall-clock and vary.

## Limits

- The profile runs the interpreter on one thread so that the times add up:
  `-compile` and `-threads` are ignored while profiling (msim says so), and the
  run is slower than an unprofiled one.
- Time spent in a task or function is counted in the process that calls it,
  not in the task.
- There is no per-line or call-graph view yet, and no memory profile.
