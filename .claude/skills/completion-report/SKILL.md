---
name: completion-report
description: How the per-tool completion rate is measured and refreshed - weighted checklist per tool against its commercial counterpart, "main %" and "with open PRs %", validation level. Use when asked for completion rates, after tool PRs merge, or when a tool's Limits section changes.
---

# Tool completion report

The report and its generator live in the project's shared folder, not in
this repository: `reports/tool-completion.md` and
`reports/tool-completion-gen.py`. **All data is in the generator; never
hand-edit the `.md`.**

```sh
python3 reports/tool-completion-gen.py reports/tool-completion.md
```

## Rubric

- Each tool has a checklist of the main capabilities of the commercial tool
  it is measured against. Weight 3 = cannot be adopted without it, 2 =
  commonly used, 1 = advanced.
- Status per item: done = 1, partial = 0.5, missing = 0. In the generator:
  `M` on main, `P` in an open PR, `h` partial on main, `hp` partial in a PR,
  `hP` partial on main and done in a PR, `X` missing.
- Two numbers per tool: **main %** (merged code only) and **with open PRs %**.
- Validation level, separate from the percentage: L1 unit tests and
  generated data; L2 run on real public designs; L3 cross-checked against an
  independent tool or standard vectors. Nothing is L4 (production or silicon).

## Refresh

1. `git fetch origin main` and note the head (PR number and short SHA).
2. Diff every `docs/*.md` on main against the last snapshot. The
   **Limits / Not yet** sections are the evidence.
3. For open branches, diff their docs against main.
4. In the generator, flip statuses (`P` to `M` for merged work), adjust the
   gaps list, update the date and base commit, regenerate.
5. Report the average for both numbers, what moved since the last version,
   and each tool's top three gaps.

## Say this every time the numbers are quoted

- They measure feature breadth, not QoR, accuracy, capacity or sign-off
  quality. "Done" means the feature exists and has a test.
- Items and weights are judgment; a different checklist moves a tool by 5
  to 10 points. Use the numbers to rank and find gaps.
- Do not mark an item done from a PR description alone: find the test.
