# xrun / irun command line

msim takes the Cadence `xrun` / `irun` command line through a small
translation layer (`src/xrun.inc`). It is the same msim binary and the same
engine: the options are rewritten to msim's own and the run is msim's.

```sh
ln -s $(which msim) ~/bin/xrun        # or irun, xmverilog, ncverilog
xrun -64bit -sv -access +rwc -f run.f -top tb -incdir inc -define W=8 -l run.log
msim -xrun -f run.f -uvm -input run.tcl           # without a link
msim --compat=cadence -f run.f -top tb            # the same, as every minisim tool spells it
```

The plain renames and the options accepted with no effect are msim's table
in the shared vendor-compatibility layer (`src/compat.hpp`, the same one the
other minisim tools use); Cadence options match in any case, as in xrun.
What needs more than a rename (`-f` files, `-incdir` / `-define` values,
`-top lib.cell:view`, the log, `-input` Tcl, `-sdf_cmd_file`) is handled in
`src/xrun.inc`.

Options msim does not list below pass through unchanged, so msim's own
options (`-vcd`, `-compile`, `-threads`, `-profile`, ...) work in this mode too.

## Translated

| xrun / irun | msim |
|---|---|
| `-f file`, `-file file` | read the file's words (`//`, `#` and `/* */` comments, quotes, `$VAR` / `${VAR}`) and translate them too |
| `-F file` | the same, with relative paths taken from the file's directory |
| `-incdir dir` | `+incdir+dir` |
| `-define N=V` | `+define+N=V` |
| `+incdir+`, `+define+`, `+libext+` | unchanged |
| `-top cell`, `-top lib.cell:view`, `+nctop+cell`, `+xmtop+cell` | `-top cell` |
| `-v file`, `-y dir` | library file and directory (`--libfile`, `--libdir`) |
| `-timescale u/p`, `-override_timescale u/p` | `--timescale u/p` |
| `-l file`, `-log file` | stdout and stderr also go to the file; default `xrun.log` (`irun.log` as irun) |
| `-append_log`, `-nolog` | append to the log; no log |
| `-input file.tcl` | `-do file.tcl`: a Tcl script for msim's interactive shell (docs/DEBUG.md), which takes the xrun forms: `run 100 ns`, `run -step`, `run -next`, `value`, `deposit x = v`, `stop -create -object/-line/-time/-condition`, `scope -set`, `describe`, `probe -create`, `finish`, and Tcl's `set`, `proc`, `foreach`, `source` |
| `-tcl` | `-i`: stop before time 0 and read commands |
| `-c`, `-elaborate` | `-elab`: stop after elaboration |
| `-uvm`, `-uvmhome dir` | `-uvm`, `-uvmhome dir`; a built-in name such as `CDNS-1.2` uses `$UVM_HOME` |
| `-coverage all`, `-coverage b:e:t:f:u`, `-cov` | `-cov` (every metric is recorded) |
| `-xprop F`, `-xprop C` | `-xprop=xmerge`, `-xprop` |
| `-svseed n`, `-seed n`, `+xmseed+n` | `-seed n`; `-svseed random` picks a seed and prints it |
| `-notimingchecks` | `+notimingcheck` |
| `-sdf_cmd_file file` | one `-sdf` per `SDF_FILE` / `COMPILED_SDF_FILE` with its `SCOPE` and `MTM_CONTROL` |
| `-sv_lib`, `-sv_root`, `-dpiheader` | unchanged |
| `-loadvpi lib:boot` | `-load lib:boot` |
| `-xmlibdirname dir`, `-nclibdirname dir` | `-o dir` (the `-compile` build directory) |
| `-makelib lib ... -endlib` | the files are compiled; the library name is dropped |
| `+plusarg` | a run-time plusarg, as in msim |

## Accepted and ignored

`-64bit`, `-sv`, `-sysv`, `-access <x>`, `+access+<x>`, `-linedebug`,
`-debug`, `-createdebugdb`, `-licqueue`, `-nocopyright`, `-q`, `-quiet`,
`-status`, `-clean`, `-exit`, `-run`, `-relax`, `-messages`, `-errormax n`,
`-nowarn X`, `-covoverwrite`, `-covfile f`, `-covtest t`, `-covworkdir d`,
`-work lib`, `-cdslib f`, `-hdlvar f`, `-snapshot s`, `-timedetail`,
`-unbuffered` and a few others in `src/xrun.inc`. Full access and line debug
are always on in msim; coverage files and snapshot names have no counterpart.

## Refused with a message

| xrun / irun | why |
|---|---|
| `.vhd`, `.vhdl`, `-v93`, `-v200x` | VHDL files go to msim's VHDL front end ([VHDL.md](VHDL.md)); the version options are accepted |
| `-R`, `-r snapshot` | msim keeps no snapshot between runs; give the sources again |
| `-gui` | no GUI; the run goes on, open the waveform with mdebug (docs/DEBUG.md) |
| Tcl `database`, `simvision`, `dumptcf`, `assertion` in `-input` | ignored with a note; `probe` writes a VCD (`dump`), not an SHM database |

## Exit status

The exit status is msim's: 0 after a normal end or `$finish`, 1 when the
design does not compile or elaborate or `$fatal` ends it, 2 for a bad
command line.
