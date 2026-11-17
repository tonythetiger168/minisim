# AI agents: `mmcp`, the minisim MCP server

`mmcp` puts the minisim engines behind the
[Model Context Protocol](https://modelcontextprotocol.io), so any agent that
speaks MCP can run simulations, lint, coverage, debug queries, regressions
and plan scoring itself, and read the results as JSON instead of scraping
logs. It works with Claude Code and Claude Desktop, Cursor, VS Code, and
agents on other models (DeepSeek, Qwen, a local model) through any MCP client.

```sh
mmcp                         # serve on stdin / stdout; the client starts it
mmcp -root ~/proj            # files are confined to ~/proj (default: the current directory)
mmcp -list                   # every tool and its input schema, as JSON
mmcp -call msim_run '{"files": ["rtl/alu.sv", "tb/tb.sv"], "top": "tb"}'   # one call, no agent
```

## Connecting a client

Claude Code, from the project directory:

```sh
claude mcp add minisim -- mmcp -root .
```

Any client that takes a JSON server list (Claude Desktop, Cursor, VS Code
`mcp.json`):

```json
{"mcpServers": {"minisim": {"command": "mmcp", "args": ["-root", "/path/to/project"]}}}
```

If the minisim programs are not on `PATH` or next to `mmcp`, add
`"-bin", "/opt/minisim/bin"` or set `MINISIM_HOME=/opt/minisim`.

## Tools

| Tool | Runs | Returns |
|---|---|---|
| `minisim_info` | | the programs found, the msim version, the tools offered, the root |
| `msim_run` | `msim` | `status` (`pass`, `compile_error`, `error`, `fatal`, `uvm_error`, `timeout`), the error lines with `file` and `line`, UVM counts (`msim -result`), the log tail, and with `coverage` / `wave` the database and the waveform to pass on |
| `mlint_check` | `mlint -json` | every finding (`file`, `line`, `rule`, `severity`, `text`, `signal`), counts by rule; `only`, `severity`, `waivers` narrow it |
| `mcov_report` | `mcov` | line, branch, toggle, condition, FSM, functional and assertion totals, per file and per covergroup; `uncovered` adds the items not hit, the uncovered source lines per file and the decisions with an outcome never taken; `min` gates metrics (`{"line": 90}`) |
| `mdebug_query` | `mdebug -script -` | one JSON answer per command: `drivers`, `loads`, `value`, `changes`, `active`, `trace`, `fsm`, `assert`, `uncovered`, `power`, `scopes`, `signals`, `end` (docs/DEBUG.md, "Scripts and other programs") |
| `mregress_run` | `mregress` | pass / fail counts and the failures grouped by first error line, each with its log |
| `mplan_report` | `mplan` | `report`: each feature's score and the metrics below goal; `check`: the plan's problems; `triage`: failure buckets |
| `read_file` | | a range of lines of a log or report, or the lines matching a regex |

A typical loop: `msim_run` with `coverage` → `mcov_report` with
`uncovered` → the agent writes a test or a constraint → `msim_run` again; or
`mregress_run` → `read_file` on a failing log → `mdebug_query` on its
waveform → a fix → `mregress_run` with `rerun`.

Agents built on these tools that keep only what the engines accept: `magent`
(docs/AGENTS.md).

## Files and safety

- Every path is relative to the root and must stay inside it, symbolic links
  included; anything else is refused.
- Outputs go under `<root>/.mmcp/<tool>-<pid>-<n>/` unless the call gives
  `out`. Add `.mmcp/` to `.gitignore`.
- Programs run without a shell, one argument per list item, with a time limit
  (`timeout` per call, `-timeout` for the default, 600 s; regressions 3600 s).
- Results are capped (log tails, at most 200 findings, 400 uncovered items by
  default) so they fit in a model's context; the full reports stay on disk.

## Add-on tools

Python files in `<prefix>/share/minisim/mcp/`, in the directories listed in
`$MINISIM_MCP_PLUGINS`, and in `-plugin DIR` are loaded at start. Each defines
`register(server)` and adds its tools:

```python
def register(srv):
    def run(srv, a):
        exe = srv.need("mytool")                       # ToolError when not installed
        rc, out, err, timed_out, secs = srv.run([exe, srv.path(a["file"])], a.get("timeout"))
        return {"exit_status": rc, "log_tail": out[-4000:]}
    srv.tool("mytool_run", "What it does, for the model.",
             {"type": "object", "required": ["file"], "properties": {"file": {"type": "string"}}},
             run, needs=["mytool"])                    # listed only when mytool is found
```

A tool is listed only when the programs in `needs` are installed. A plug-in
that fails to load is reported on stderr and by `minisim_info`; the other
tools keep working. Add-on programs check their own licence: without one the
call returns `isError` with the program's message.

## Protocol

MCP over stdio: JSON-RPC 2.0, one message per line, protocol versions
2025-06-18, 2025-03-26 and 2024-11-05. Tool results carry the JSON both as
text and as `structuredContent`. Calls run concurrently, so a long regression
does not block other requests. Python standard library only.
