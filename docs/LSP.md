# Editors: `mlsp` and the VS Code extension

`mlsp` is the minisim language server: SystemVerilog and Verilog errors as you
type, go to definition, references, hover, outline, workspace symbols and
completion, over the Language Server Protocol (3.17, stdio), so VS Code, Vim,
Neovim, Emacs, Helix, Sublime Text, Zed and JetBrains IDEs (with an LSP
plugin) all get the same features. It has no front end of its own: every
answer comes from slang set up the way `msim` sets it up (IEEE 1800-2023, use
before declaration allowed, the same diagnostics downgraded to warnings), so
the editor shows the errors `msim` would give. Lint findings are `mlint`'s:
on save, `mlsp` runs the `mlint` next to it with `-json` and shows them.

The VS Code extension in `editors/vscode/` starts `mlsp`, adds syntax
colours, and puts the other tools one command away: run `msim` on the design
of the open file, open the waveform in `mdebug`'s viewer, show `mcov`
coverage in the editor, and offer the minisim MCP server (`mmcp`, see
`docs/MCP.md`) to agents in VS Code. It runs no engine of its own.

```
mlsp                                  # an editor starts it (stdin/stdout)
mlsp -check tb/tb.sv rtl/alu.sv       # the same analysis once, diagnostics on stdout, exit 1 on errors
mlsp -check -root . tb.sv -- +define+SYNTHESIS +incdir+inc
mlsp -check -f design.f tb.sv         # exactly the files of a -f list
```

## What goes into an analysis

`mlsp` indexes every SystemVerilog file of the workspace (a token scan, in
parallel; about 0.1 s for a few thousand files) and records which file
defines which module, interface, program, package, primitive, checker,
class and `` `define ``. When a file is opened or edited it analyzes that
file with every file defining a design unit it uses, and so on down (an
instance `alu u(...)`, `alu #(...)`, `pkg::x`, `import pkg::*`, an interface
port). When a file uses a macro that another file defines without an
`` `include `` (as a `-f` list would put the defining file first), that file
goes first and the set becomes one compilation unit (`--single-unit`).
`` `include `` files are found next to the including file, then anywhere in
the workspace. A header (`.svh`) is analyzed inside a file that includes it.
When a name is defined in several files, the one nearest the open file wins
(same directory, then the longest common path).

With `minisim.fileList` (or `mlsp -f`) the analysis uses exactly the files
and options of that `-f` file instead.

Open files are read from the editor's buffers, saved or not: rename a port in
an open `alu.sv` and the open testbench shows the broken connection before
either file is saved. A save re-analyzes the other open files whose design
includes the saved one.

## Features

| LSP request | What it gives |
|---|---|
| `textDocument/publishDiagnostics` | slang's errors and warnings (source `slang`, code = slang's diagnostic name, notes as related information); `mlint` findings on save (source `mlint`, code = rule) |
| `textDocument/definition` | the declaration of the name under the cursor, from slang's resolved references: signals, ports, parameters (also in `[W-1:0]`), types (`pkg::t`), enum values, functions and tasks, class members, the module of an instance, the port of `.p(x)` and the parameter of `#(.W(8))`, the base class of `extends`. Falls back to the workspace index for a name slang did not resolve and for `` `MACRO `` ; on an `` `include `` line, the included file |
| `textDocument/references`, `documentHighlight` | every resolved use of that declaration in the design of the file |
| `textDocument/hover` | the declaration (`logic [7:0] y`, `parameter int W = 8`, a module's header with its parameters and ports, a function's prototype) and the hierarchical path (`tb.dut.r`) |
| `textDocument/documentSymbol` | the outline: modules, interfaces, packages, classes, parameters, ports, signals, typedefs, functions, tasks, instances, named generate blocks |
| `workspace/symbol` | design units, classes and macros of the whole workspace |
| `textDocument/completion` | keywords, design units, names declared in the file, `` `macros `` after a backtick, system tasks after `$` |
| `minisim/compileInfo` (minisim extension) | the files, options and top modules of the open file's design: what the editor passes to `msim`, `mlint` and `mdebug` |

Positions are UTF-16 code units (the LSP default) or bytes when the client
offers `utf-8`; a line with Chinese comments before the code is handled.

## Settings

The extension's settings (`minisim.*`) are sent to `mlsp` as
`initializationOptions` and on every change; another editor sends the same
object:

| Setting | Default | |
|---|---|---|
| `args` | `[]` | front-end options for every analysis and run: `+incdir+DIR`, `+define+X=1`, `-DX`, `--std=1800-2017` |
| `fileList` | `""` | a `-f` file (relative to the workspace); empty = follow the design through the workspace |
| `binDir` | `""` | where the minisim programs are; empty = next to `mlsp`, then `PATH` |
| `uvmHome` | `$UVM_HOME` | the UVM library, compiled in when the design imports `uvm_pkg` and no workspace file defines it |
| `mlint.enable` | `true` | run `mlint` on save |
| `mlint.args` | `[]` | extra `mlint` options: `-goal design`, `-config lint.cfg` |
| `maxFiles` | 20000 | the most workspace files indexed |

VS Code only: `minisim.msim.args` (extra options for Run Simulation),
`minisim.waveform` (`fst`, `vcd` or `none`), `minisim.coverage` (run with
`-cov`), `minisim.mcp.enable` (offer `mmcp` to agents),
`minisim.trace.server`.

## VS Code

Build and install the extension (it is not on the Marketplace):

```
cd editors/vscode
npm ci && npm run compile
npx @vscode/vsce package -o minisim.vsix
code --install-extension minisim.vsix
```

Then set `minisim.binDir` to the minisim `bin` directory (or put it on
`PATH`). `mlsp` runs where the files are: on Linux directly, on Windows
inside WSL or a Remote-SSH host (the VS Code server side runs it).

| Command | What it does |
|---|---|
| minisim: Run Simulation (msim) | asks `mlsp` for the design of the open file, picks the top (the only one, or a choice with testbenches first), runs `msim` as a VS Code task with a problem matcher; writes `.minisim/<top>.fst` (and `.cov.json` with `minisim.coverage`). Also the Run button in the editor title |
| minisim: Lint Design (mlint) | `mlint` on the same design, findings in Problems |
| minisim: Open Waveform (mdebug) | starts `mdebug ... -serve 0` on the last run's waveform (or one you pick) and opens the viewer in VS Code's Simple Browser |
| minisim: Show Coverage (mcov) | `mcov report -lcov` on the last run's coverage (or a `.json`/`.info` you pick) and shows it with VS Code's test-coverage view: covered and missed lines and branches in the gutter, per-file percentages |
| minisim: Restart Language Server (mlsp) | |

Agents: with VS Code 1.101 or later the extension registers `mmcp` (the
minisim MCP server, built by the agent-layer work, `docs/MCP.md`) as an MCP
server for each workspace folder (`mmcp -root <folder> [-bin <binDir>]`), so
agent mode can run `msim`, `mlint`, `mcov` and `mdebug` through it. The
extension adds no tool interface of its own.

## Other editors

Neovim (0.11+):

```lua
vim.lsp.config('mlsp', { cmd = { 'mlsp' }, filetypes = { 'systemverilog', 'verilog' },
  root_markers = { '.git' }, init_options = { args = { '+incdir+rtl/include' } } })
vim.lsp.enable('mlsp')
```

Emacs (eglot): `(add-to-list 'eglot-server-programs '((verilog-mode) "mlsp"))`.
Helix (`languages.toml`): `[language-server.mlsp] command = "mlsp"` and add
`mlsp` to the `language-servers` of `verilog`.

## Performance

| Design | Files | Analysis (parse, elaborate, all references) |
|---|---|---|
| tests/mlsp (ALU, package, testbench) | 4 | under 10 ms |
| OpenTitan Ibex (`tb_ibex.sv` with `-DSYNTHESIS`, design followed through the clone) | 67 | 0.16 s, 7265 symbols, no errors |

Measured on 4 cores. An edit is analyzed 300 ms after the last keystroke;
requests wait for the analysis of the text they ask about (at most 3 s).
The workspace index is built on all cores (`-threads N`); `mlsp -check
-threads 1` and `-threads N` print the same thing (tested).

## Limits / Not yet

- The design-following index is a token scan: it ignores `` `ifdef ``, so a
  file used only under a macro that is off (for example a formal testbench
  included under `FORMAL`) can join the analysis as an extra top. Use
  `fileList` for exact control.
- An open file's design is re-elaborated in full on each edit (fast for one
  core or block; a 100k-line SoC is seconds). No incremental re-elaboration.
- No rename, code actions (quick fixes for `mlint` rules), formatting,
  semantic tokens, signature help, inlay hints or call hierarchy yet.
- Names in a type's dimensions are found by name lookup in the declaring
  scope, not by slang's binding; a generate-scoped name there may resolve to
  an outer one.
- VHDL files are not analyzed (msim's VHDL front end is not wired in yet).
- The extension's language features are tested through the protocol
  (`tests/mlsp_test.py`) and its helpers with node; it has not been run
  inside a VS Code instance by the test suite (VS Code cannot be downloaded
  in CI here).
- Not published on the VS Code Marketplace or Open VSX.
