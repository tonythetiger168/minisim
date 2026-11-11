---
name: merge-queue
description: How a minisim PR gets merged - open as draft, wait for your turn in the merge queue, update to main, merge only on green CI for the exact head. Use when opening a PR, when told it is your PR's turn, or when a PR is stuck behind main.
---

# Merging a minisim PR

`main` is protected: the checks `build-and-test` and `build-and-test-slang`
are both required, admins are not exempt, and (while "strict" is on) the
branch must contain current `main`. Check the live rule before relying on
this text:

```sh
gh api repos/tonythetiger168/minisim/branches/main/protection/required_status_checks \
  --jq '{strict, contexts}'
```

Sessions cannot change repository settings (the API returns 403). If a
setting has to change, ask the repository owner and give the settings link.

## Rules

1. **Green means CI green on the exact head commit being merged.** A local
   `ctest` run, or green on an earlier commit, does not count.
2. **While strict is on, PRs merge one at a time.** Every merge puts all the
   other PRs behind `main` again, so updating them all at once only burns CI
   minutes. New PRs join the end of the queue.
3. **Keep auto-merge disarmed until it is your turn.** The coordinator says
   when. Until then: PR stays draft, no auto-merge, keep working.
4. Stacked PRs merge in stack order. When a newer PR contains older ones,
   merge only the newest and close the rest as superseded.
5. Never skip, disable or quarantine a test to get green. Never force-push a
   branch someone else owns.

## When it is your turn

```sh
git fetch origin main
git merge origin/main            # merge, do not rebase: the PR is already reviewed/linked
# resolve conflicts (see below), build, run the tests you touched
git push -u origin <branch>
gh pr ready <n>                  # drafts can neither merge nor auto-merge
gh pr merge <n> --auto --merge   # arms auto-merge; it fires when both checks pass
```

In a Claude Code cloud session GitHub GraphQL is blocked, so `gh pr ready`,
`gh pr merge --auto` and `gh pr view --json` fail with 403. Use the GitHub
MCP tools (`update_pull_request` with `draft: false`, `enable_pr_auto_merge`)
or the REST routes:

```sh
gh api -X POST repos/tonythetiger168/minisim/pulls/<n>/ccr/ready_for_review
gh api -X PUT  repos/tonythetiger168/minisim/pulls/<n>/ccr/auto_merge -f merge_method=merge
gh api repos/tonythetiger168/minisim/commits/<sha>/check-runs \
  --jq '.check_runs[]|{name,status,conclusion}'
```

Then stop and wait for the PR event. If CI goes red, reproduce the failing
check locally, fix it, push, and let auto-merge re-arm on the new head.
After the merge, report it to the coordinator so the next PR can start.

## Conflicts that keep coming back

- `THIRD_PARTY_NOTICES.md`: several PRs add it. Keep both sides, in the
  order they landed. Never drop another tool's entry. See `third-party-code`.
- `docs/PRODUCT.md`, `README.md` tables, `CHANGELOG.md`: keep both rows.
- `CMakeLists.txt`: tools belong in their own `cmake/tools/<tool>.cmake`
  (see `add-tool`), which avoids this conflict entirely.
- `.github/workflows/ci.yml`: keep the mirror probe first (see `ci-apt-steps`).
