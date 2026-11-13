# Checkpoints: $save, $restart, msim -restore

msim saves and restores a simulation by replay. A simulation in msim is
deterministic, so running it again from time 0 with the same command line
reaches the same state. A checkpoint records where that state is, and a
restore runs up to that point again with all output muted.

| How | What it does |
|---|---|
| `$save("file")`, `$incsave("file")` (IEEE 1800-2023 D.9) | writes a checkpoint: which `$save` of the run it is, the time, a digest of every signal's value, the directory and the command line |
| `$restart("file")` | from inside a run: starts msim again with the same command line, muted until that `$save`, then goes on |
| `msim -restore file [+plusargs]` (also `-r`, as in `simv -r`) | from the shell: goes to the checkpoint's directory, runs its command line muted up to the `$save`, then prints and goes on. Plusargs after the file are added to the command line |

When the run gets back to the checkpoint, msim compares the state with the
saved digest. It prints `restored the checkpoint at time T` if they match.
Otherwise it warns that the run is not repeatable and carries on. A run is
not repeatable when something it reads before the checkpoint changed: the
clock (`$system("date")`), the environment, files, a DPI model, or a plusarg
added at restore that the design reads before the checkpoint.

## Limits

- A restore costs as much time as the run up to the checkpoint, because
  nothing is kept in memory between runs. Commercial tools reload a memory
  image, which is faster for long runs.
- The sources and the command line must still be what they were at
  `$save` time. msim checks the resulting state, not the files.
- Interactive mode (`msim -i`): `save FILE` writes a checkpoint that also
  records the commands given so far; `restore FILE` and `msim -restore FILE`
  replay them (docs/DEBUG.md).
