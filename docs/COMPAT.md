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

## Limits / Not yet

- No tool uses the layer yet. Each tool's owner adds its own rule table.
- Option abbreviations (xrun accepts any unique prefix of an option) are not
  expanded; write the full option name.
- `-opt=value` and `-opt value` are separate rules; a tool registers both if
  it needs both.
- Proprietary formats with no public specification (SHM, WLF, binary UCDB,
  FSDB, emulator databases) are out of scope. Open formats (VCD/FST, UCIS XML,
  SDF, SPEF, Liberty, LEF/DEF, GDS/OASIS, UPF) are used instead.
