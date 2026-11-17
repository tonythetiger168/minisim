# Verification agents: `magent`

`magent` runs three agents on the minisim engines. In each, a model proposes
and an engine decides: nothing the model writes is kept unless msim, mlint,
mcov or mregress measures it as better, and the report gives those
measurements, so a run can be scored without reading the model's prose.

```sh
magent triage   -results regress.d                          # explain each failure group
magent lintfix  rtl/*.sv -top core -only LAT001,W001 -regress tests.list
magent covclose rtl/*.sv -tb tb/tb.sv -tb-top tb -goal line=100,branch=95
```

The model calls the same tools as any MCP client of `mmcp` (docs/MCP.md),
plus a few of the agent's own (`submit_triage`, `edit_file`, `add_test`,
`finish`). Files are read and written only under `-root` (default `.`).
Every run writes its work under `<root>/.mmcp/`; `-o report.json` saves the
report and prints its metrics, `-log turns.jsonl` records each model turn.

## Models

| Option | Model |
|---|---|
| (default) `-model claude-opus-5-5` | Claude through the Anthropic SDK (`pip install anthropic`); credentials from `ANTHROPIC_API_KEY` or `ant auth login` |
| `-api openai -base-url URL -model NAME` | any OpenAI-compatible endpoint: DeepSeek, Qwen, vLLM, Ollama, a local HeliosLM server; key from `MAGENT_API_KEY` |

`-effort` sets the Claude effort level (default `high`), `-max-steps` the
number of model turns (default 40).

## triage

Input: an mregress output directory. For each failure group (most runs
first, `-max-groups`, default 10) the model reads the logs and sources, may
query mdebug, and submits a category (`design_bug`, `testbench_bug`,
`tool_bug`, `environment`, `flaky`, `unknown`), the root cause, the file and
line with the quoted text, the evidence and a suggested fix.

Measured: groups diagnosed; cited locations that exist and whose quote is
at that line (`locations_valid`). With `-labels known.json`
(`{"<group signature>": {"category": ..., "file": ..., "line": N}}`), also
`category_correct` and `location_correct` (within 3 lines): run it on a set
of known bugs to compare models or prompts.

## lintfix

Input: design files and top; `-only` narrows the rules. The agent works on a
copy of the root. Each `edit_file` (an exact old / new replacement) is
checked at once and undone unless all of these hold:

- mlint: at least one of the original findings is gone and no new one appears
- msim: the design still elaborates
- `-regress LIST`: the test list fails no more tests than before the edits
  (paths in the list relative to the root, so they work in the copy)
- mequiv, when installed and not `-no-equiv`: the edited RTL is equivalent to
  the original, unless the edit says `functional_change` (a real bug fix,
  such as a latch or a truncation)

Output: `lintfix.patch` (unified diff) and the accepted and rejected edits
with each verdict; `-apply` copies the accepted files back. Measured:
findings before, removed, added (always 0 for accepted edits), compiles,
regression and equivalence, edits accepted / rejected.

## covclose

Input: design files, an existing passing testbench (`-tb`, top `-tb-top`)
and a goal (`-goal line=100,branch=100`). The model gets the coverage and
the uncovered lines and branch outcomes, and writes whole new testbenches
with `add_test` (at most `-rounds`). Each is simulated with coverage and kept
only when it passes and covers something new; the rest are deleted.

Output: the kept tests under the work directory, the merged coverage
database, and what the model says is unreachable. Measured: coverage before
and after (line, branch, condition, FSM, functional, assertion), tests kept
and dropped. Toggle coverage is not scored: it is named by hierarchy, so a
new testbench top would count as new toggles. The testbenches themselves are
left out of the score (`-exclude` to change that).

## Cost and safety

Each model turn is a paid request when the model is a hosted one. The agents
never run a shell: they call the engines with argument lists, inside the
root, with the time limit of `-timeout` per call.
