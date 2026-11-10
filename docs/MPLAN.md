# mplan: verification planning and tracking

`mplan` answers three questions about a verification effort: which features
of the design are proven and by what, how that is moving over time, and what
the current failures are. It reads what the other tools already write: the
`results.json` of `mregress` and the coverage database of `msim -cov` /
`mcov`. It does not run tests or collect coverage itself.

```sh
mregress tests.list -cov -plan uart.vplan     # regress, then score the plan into regress.d/plan/
mplan check  uart.vplan                       # read the plan, report mistakes
mplan report uart.vplan -results regress.d    # score, snapshot, plan.json, plan.html
mplan triage -results regress.d -plan uart.vplan
mplan trend  plan.d/history.jsonl
mplan init   -results regress.d -o draft.vplan
```

## The plan file

A plan is indented text. Indentation nests features; `#` starts a comment.

```
plan uart "UART verification plan"
  owner: tony
  goal: 100                         # default goal of every metric below
  feature tx "Transmit path" weight=2 owner=alice
    doc: spec 3.1, shift register and baud tick
    test   uart_tx_*                # mregress test names
    line   rtl/uart_tx.sv goal=95   # code coverage of the files
    branch rtl/uart_tx.sv goal=90
    toggle tb.dut.u_tx goal=80      # signals in that scope
    cover  tb.cg_tx                 # a covergroup type, or type.item
    assert tb.dut.u_tx              # assertions in that scope
    feature parity "Parity"
      test uart_parity_*
  include rx.vplan                  # the features of another plan file
```

| Line | Meaning |
|---|---|
| `plan NAME ["title"]` | Names the plan. Once, first. Optional: without it the file name is the plan name |
| `feature NAME ["title"] [weight=N] [goal=PCT] [owner=WHO]` | A feature; the lines indented under it belong to it |
| `owner:` / `doc:` / `goal:` / `weight:` | Attributes of the feature (or plan) they sit in. Owner and goal are inherited by the features below |
| `<kind> PATTERN [goal=PCT] [weight=N]` | A metric: what proves the feature |
| `include FILE` | Another plan file, relative to this one. A file with its own `plan` line comes in as one feature; a bare list of features is spliced in |

In a pattern `*` matches any text and `?` one character. A hierarchical name
also matches everything below it (`tb.dut.u_tx` covers `tb.dut.u_tx.busy`).
A file pattern matches the end of the path (`rtl/uart_tx.sv`), or a
directory when it ends in `/` (`rtl/`); one that starts with `/` is the
whole path.

| Kind | Score |
|---|---|
| `test` | Tests that pass over tests matched. A test passes when every seed of it passed; a known gap (`@xfail`) or a test that did not run is not a pass. No matching test scores 0 |
| `line` | Lines hit over lines, of the matched files |
| `branch` | Branch outcomes seen over outcomes, of the matched files |
| `toggle` | Bits toggled both ways over bits, of the matched signals |
| `cover` | The matched covergroup items, each bins covered over bins (a bin is covered at `option.at_least` hits), weighed by `option.weight`. The instances of a type are joined by bin name (IEEE 1800-2023 19.11.3) |
| `assert` | `assert` / `assume` directives that passed at least once and never failed, plus `cover` directives that were hit, over directives matched |

A feature's score is the weighted mean of its metrics and child features. A
feature with nothing mapped yet is planned work and counts as 0. Its status:

| Status | When |
|---|---|
| `failing` | A mapped test fails or a mapped assertion failed, here or below |
| `met` | Every metric reached its goal, here and below |
| `empty` | Nothing is mapped yet |
| `not started` | The score is 0 |
| `partial` | Anything else |

## report

`mplan report PLAN -results DIR [-results DIR ...] [-cov DB ...] [-o plan.d]`

Each `-results` is an mregress output directory (or its `results.json`);
several are combined, for example one per simulator. The coverage database
is `cov.json` in each results directory unless `-cov` names others. The
output directory gets:

- `plan.html`: one self-contained page. Summary tiles, the trend chart, the
  feature tree with a bar and goal mark per metric, the failure triage
  table with a reproduce command per bucket, and the results that no plan
  line points at (tests, files, covergroups, assertions).
- `plan.json`: the same, for scripts.
- `history.jsonl`: one snapshot per line (see below).

`-md FILE` also writes the feature table as Markdown. `-q` prints only the
summary line.

Gates, for CI. Each exits 1 when it fails:

| Option | Fails when |
|---|---|
| `-gate` | The plan as a whole is not `met` |
| `-min-score PCT` | The plan score is below PCT |
| `-fail-on-new` | A failure bucket is new and no rule marks it known or waived |

## History and trend

`report` appends a snapshot to `<o>/history.jsonl` (`-history FILE` to keep
it somewhere that outlives the build directory; `-no-history` to leave it
alone). A snapshot holds the time, a label, every feature's score and
status, the test counts, the coverage totals, the failing tests and the
failure buckets.

- The label is `-label`, else the first ten characters of `GIT_COMMIT` or
  `GITHUB_SHA`, else the time. Reporting the same label twice in a row
  replaces the snapshot instead of adding one.
- The time is `-time` (ISO 8601), else `SOURCE_DATE_EPOCH`, else now.

The report compares with the snapshot before it: the change in score, the
features whose score dropped, and the triage classes below.
`mplan trend history.jsonl [-n N] [-feature PATH]` prints the history as a
table, for the plan or for one feature.

## Triage

Every failing run goes into a bucket by its failure signature: mregress's,
the first error line with numbers, paths and hex values replaced. A timeout
or a bare exit status says little, so those buckets are split further by the
last line of the run's log.

| Mark | Meaning |
|---|---|
| `new` | The bucket was not in the previous snapshot |
| `recurring, since X` | It was; X is the first of the consecutive snapshots that have it |
| `flaky` | Every test in the bucket also passed on another seed |
| fixed | Listed separately: buckets of the previous snapshot that are gone |

Each bucket lists its tests, the features those tests are mapped to, and the
owners of those features. A rules file (`-rules FILE`) annotates buckets.
One rule per line, the first match wins:

```
/timeout/                 status=known owner=infra bug=INFRA-12
/output differs at line/  status=waived note="reference model issue"
```

The regex is searched in the signature. `status` is `known`, `waived` or
`open`; `owner` replaces the feature owners; `bug` and `note` are shown.
To file the buckets as issues, use `mbug regress` on the same results.

## init

`mplan init -results DIR [-cov DB] [-name NAME] [-o FILE]` writes a draft
plan from an existing regression: one feature per test-name prefix, the
covergroup types, the assertion scopes and the source files. It is a
starting point to regroup by hand.

## Speed

Result directories, coverage databases and failure logs are read in
parallel: `-threads N` (or `-j N`), by default every logical CPU this
process may use. The output is the same for any N. Patterns are matched
against sorted name tables, so a plan with thousands of lines stays fast: a
400-feature, 7,600-metric plan over 100,000 runs and an 80,000-signal
coverage database scores in about two seconds.
