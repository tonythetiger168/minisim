# Vendor command-line compatibility

Every minisim tool has one engine. Users moving from Synopsys, Cadence or
Siemens tools keep their command lines: the tool translates the other
vendor's options into its own before parsing them. There is never a second
implementation for compatibility. The shared layer is `src/compat.hpp`, and
each tool adds only a table of option rules.

## Choosing a vendor

```
msim --compat=cadence -sv -access +rwc -svseed 7 top.sv
msim --compat=siemens -sv_seed 7 top.sv
```

- `--compat=synopsys|cadence|siemens` (also `-compat X`; `snps`, `cdns`,
  `mentor` are accepted) anywhere on the command line. The last one wins.
- Without it, the name the tool was started as (`argv[0]`, without directory
  or `.exe`) is looked up in the tool's alias table. Users who want `xrun ...`
  to work can create that symlink themselves. minisim ships no binaries
  named after other vendors' products.
- Default: the tool's native options.

## What the layer does

- **Rename**: an option becomes one or more native options, keeping its value
  (`-svseed 7` becomes `-seed 7`). A rule can match a prefix, so
  `+ntb_random_seed=42` becomes `-seed=42`.
- **Drop**: options that have no meaning for minisim are removed, and one line
  per option is printed (`ignored cadence option -access +rwc (all signals are
  visible)`). Licence-queue and 32/64-bit options are dropped for every tool
  (`-full64`, `-licqueue`, `-64bit`, `-64`, ...).
- Options are matched case-insensitively in Cadence mode, as xrun does, and
  exactly in the other modes.
- Unknown options are passed through unchanged, so the tool still reports
  them as errors.
- **Messages**: `compat::format` prints diagnostics in the selected vendor's
  layout, so existing log filters keep working:

| Mode | Layout |
|---|---|
| native | `top.sv:12:5: error: expecting a semicolon [EXPSMC]` |
| synopsys | `Error-[EXPSMC] expecting a semicolon` then `  "top.sv", 12` |
| cadence | `xmvlog: *E,EXPSMC (top.sv,12\|5): expecting a semicolon` |
| siemens | `** Error: (vlog-EXPSMC) top.sv(12): expecting a semicolon` |

## Adding compatibility to a tool

```cpp
#include "compat.hpp"
static const compat::Alias kAliases[] = {{"xrun", compat::Vendor::Cadence}};
static const compat::Rule kRules[] = {
    {compat::Vendor::Cadence, "-svseed", compat::Act::Rename, "-seed", /*takesValue*/ true},
    {compat::Vendor::Cadence, "-access", compat::Act::Drop, {}, true, false, "all signals are visible"},
};
std::vector<std::string> args(argv + 1, argv + argc);
auto v = compat::detect(argv[0], args, kAliases);
auto r = compat::translate(v, args, kRules);
for (auto& n : r.notes) std::cerr << n << "\n";
// parse r.args as native options
```

Tcl shell commands of the other vendors (Genus or Innovus `set_db`, Conformal
dofiles, JasperGold Tcl, ...) are not handled here. They are commands
registered in each tool's shell on `src/tcl.hpp`.

## msim: VCS partition compile

msim takes the VCS partition compile options as spellings of its compiled
mode (`-compile`), directly and through the xrun front end:

| VCS | msim |
|---|---|
| `-partcomp`, `-partcomp=<mode>`, `-fastpartcomp` | `-compile` |
| `-fastpartcomp=j<n>` | `-compile -cfiles <n>` (build up to n generated C++ files in parallel) |
| `-partcomp_dir=<dir>`, `-Mdir=<dir>` | `-o <dir>` (the build directory) |
| `-partcomp_sharedlib`, `-pcmakeprof` | accepted, no effect |

msim chooses its own parts: the generated C++ files of the design. The build
directory keeps them, a file that did not change is not rebuilt, and a design
that did not change reuses the `simv` already there. There is no per-module
partition yet (an edit to one module does not yet rebuild only that module's
part), and partition configuration files are not read. msim prints one note
saying so (`tests/msim_partcomp.cmake`).

## msim: the Siemens and Synopsys multi-step flows

The three-step flows run on msim's one engine (`src/mstep.inc`). msim takes
them when it runs under the tool's name (a link you create; minisim ships
none) or with `-vlog`, `-vcom`, `-vopt`, `-vsim`, `-vlib`, `-vmap`, `-vlogan`,
`-vhdlan` or `-vcs` as its first option.

```
vlib work; vlib pkgs; vmap pkgs pkgs
vlog -work pkgs pkg.sv
vcom -2008 ctr.vhd
vlog -sv +incdir+inc +define+W=8 tb.sv -l comp.log
vsim -c -L pkgs -do "run -all; quit -f" -sv_seed 5 work.tb +more -l run.log

vlogan -full64 -sverilog -work PKGS pkg.sv      # synopsys_sim.setup maps PKGS
vhdlan ctr.vhd
vlogan -sverilog -timescale=1ns/1ps +incdir+inc +define+W=8 tb.sv
vcs -full64 -debug_access+all tb -o simv -l comp.log
./simv +ntb_random_seed=5 +more -l run.log

vcs -sverilog +incdir+inc pkg.sv ctr.vhd tb.sv -R +more   # one step
```

- **Libraries.** A library is a directory (`./work` by default) holding one
  text file, `msim.lib`: the analysed sources in analysis order, each with
  the options it was analysed with. Analysing a file again replaces its
  line. Names map to directories through `modelsim.ini` (`[Library]`,
  `name = dir`; `$MODELSIM`, `-modelsimini`) and `synopsys_sim.setup`
  (`NAME : dir`, `WORK > NAME`; `$SYNOPSYS_SIM_SETUP`). `vlib` creates a
  library, `vmap` reads and edits `modelsim.ini`; `vlogan` and `vhdlan`
  create the work directory when it is missing.
- **Analysis** (`vlog`, `vcom`, `vlogan`, `vhdlan`) checks the files'
  syntax (`msim -parse`) and records them. A file with errors is not added.
  Kept per file: `+define+`, `-define`, `+incdir+`, `-y`, `-v`, `+libext+`,
  `-timescale`, `-ntb_opts uvm`. `-f`/`-F` files are read. Language-version,
  lint, debug-access and message options are accepted and ignored. VHDL
  files go to msim's VHDL front end (docs/VHDL.md), checked with the
  library's earlier VHDL in front of them.
- **vsim** runs msim on the sources of the `-L` libraries and the tops'
  libraries (`lib.top`; `-lib` names the default). `-do` commands or a
  `-do` file: a script of only `run -all` and `quit` just runs; anything
  else becomes an msim `-do` Tcl script. `-c` with no `-do` reads commands
  from the terminal. `-sv_seed` (also `random`), `-gNAME=V`/`-GNAME=V`,
  `-sv_lib`, `-sv_root`, `-pli`, `-coverage`, `-l` are translated;
  msim's own options and plusargs pass through.
- **vcs** in one step (source files on the line) or after `vlogan` /
  `vhdlan` (design unit names: every library the setup file maps is
  searched). It checks the design at once (`msim -elab`), then writes
  `simv` (`-o` names it): a script that runs msim on the recorded command
  line, kept in `simv.daidir/msim.args`. `-R` runs it at once with the
  plusargs. `simv` takes `-l`, `-ucli`, `-do`, `+vcs+finish+<t>`,
  `+ntb_random_seed=` and plusargs.
- **Compiled mode.** `vsim -compile` or `vcs -partcomp` builds in
  `<library>/_msim/<top>` (one-step vcs: `simv.daidir/msim.d`), so the
  generated parts are reused from the library. The kernel cache stays
  user-wide (`$MSIM_KCACHE`).
- **vopt** checks the design (`msim -elab`) and records it in the library
  under its `-o` name (`_msim/opt/<name>.args`); `vsim <name>` runs it.
- **Compilation units.** Each analysis step is its own compilation unit, as
  vlog is without `-mfcu`: a `+define+` given to one step reaches only its
  files. When the files do not all share one set of `+define+`s, each one
  with its own is given to msim through a small generated file in the
  library (`_msim/units`) that defines them and includes it.
- **UVM.** `vlog` of a file that imports `uvm_pkg` or includes
  `uvm_macros.svh` uses msim's UVM (`-uvm`, `$UVM_HOME`), as Questa finds
  its precompiled one; `-L uvm` / `-L mtiUvm` are accepted for it. Analysing
  the UVM sources yourself into a library works too.
- **Not here:** `vlog -mfcu` (one unit over several files: macros defined
  in one file reaching the next), `.db` snapshot files, other vendors'
  precompiled libraries.

## Limits / Not yet

- Option abbreviations (xrun accepts any unique prefix of an option) are not
  expanded; write the full option name.
- `-opt=value` and `-opt value` are separate rules; a tool registers both if
  it needs both.
- Proprietary formats with no public specification (SHM, WLF, binary UCDB,
  FSDB, emulator databases) are out of scope. Open formats (VCD/FST, UCIS XML,
  SDF, SPEF, Liberty, LEF/DEF, GDS/OASIS, UPF) are used instead.
