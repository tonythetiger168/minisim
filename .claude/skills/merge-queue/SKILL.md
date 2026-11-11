---
name: merge-queue
description: How a minisim PR gets merged - arm auto-merge, merge only on green CI for the exact head, watch main afterwards, and the one-at-a-time queue used while the up-to-date rule is on. Use when opening a PR, merging one, or when a PR is stuck or conflicted.
---

# Merging a minisim PR

`main` is protected: the checks `build-and-test` and `build-and-test-slang`
are both required and admins are not exempt. The "branch must be up to date"
rule (strict) is currently off, so PRs merge in parallel, each on its own
green head. Check the live rule before relying on this text:

```sh
gh api repos/tonythetiger168/minisim/branches/main/protection/required_status_checks \
  --jq '{strict, contexts}'
```

Sessions cannot change repository settings (the API returns 403). If a
setting has to change, ask the repository owner and give the settings link.

## Rules

1. **Green means CI green on the exact head commit being merged.** A local
   `ctest` run, or green on an earlier commit, does not count.
2. **strict off (now): no queue.** Mark the PR ready and arm auto-merge as
   soon as the work is complete; it merges when both checks pass. Merge
   `main` in only when there is a conflict or the PR depends on something
   that just landed.
3. **After your PR merges, watch CI on `main`.** Two PRs that were each
   green can break together. If the merge commit goes red, fix it at once
   with a small follow-up PR; a red `main` blocks everyone.
4. **strict on: one PR at a time.** Every merge puts the others behind
   `main`, so keep the PR draft with auto-merge disarmed until the
   coordinator says it is your turn, then follow the steps below. New PRs
   join the end of the queue.
5. Stacked PRs merge in stack order. When a newer PR contains older ones,
   merge only the newest and close the rest as superseded.
6. Never skip, disable or quarantine a test to get green. Never force-push a
   branch someone else owns.

## Merging

```sh
git fetch origin main
git merge origin/main            # only if needed (conflict, strict on); merge, do not rebase
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
After the merge, check the CI run of the merge commit on `main` and report
the merge to the coordinator.

## Conflicts that keep coming back

- `THIRD_PARTY_NOTICES.md`: several PRs add it. Keep both sides, in the
  order they landed. Never drop another tool's entry. See `third-party-code`.
- `docs/PRODUCT.md`, `README.md` tables, `CHANGELOG.md`: keep both rows.
- `CMakeLists.txt`: tools belong in their own `cmake/tools/<tool>.cmake`
  (see `add-tool`), which avoids this conflict entirely.
- `.github/workflows/ci.yml`: keep the mirror probe first (see `ci-apt-steps`).
