# Running minisim regressions in CI

`mregress` is built for a CI job: it exits non-zero when a test fails
unexpectedly, and it writes machine-readable results next to the human ones.

| File | Written by | Read by |
| --- | --- | --- |
| `<out>/junit.xml` | always | Jenkins JUnit, GitLab `junit:`, GitHub test reporters |
| `<out>/results.json` | always | your own scripts (`summary.passed`, `.failed`, `.xfail`, `runs[]`) |
| `<out>/report.html` | always | people: every run, grouped by its first error line |
| `<out>/cov.json` | `-cov` | `mcov merge` / `mcov report` |
| `<out>/cov.info` | `mcov report -lcov` | Jenkins Coverage, Codecov, `genhtml` |
| `<out>/bugs.json` | `mbug ... -o` | your scripts: which issue each failure went to |
| `<out>/cov_html/` | `-cov`, or `mcov report -html` | people |

A test marked as a known gap (`@xfail=`, or `//! xfail:` in the spec suite) is
written to `junit.xml` as *skipped*, not as a failure, so a tracked gap does
not turn the job red. A known gap that starts passing does (XPASS), so the
marker gets removed.

## Jenkins

[`Jenkinsfile`](Jenkinsfile) is a working declarative pipeline: build info,
one regression stage, then JUnit, archived artifacts, the HTML reports, and
LCOV coverage. Copy it and change the agent label, `PATH`, and the test list.

Plugins: [JUnit](https://plugins.jenkins.io/junit),
[HTML Publisher](https://plugins.jenkins.io/htmlpublisher),
[Coverage](https://plugins.jenkins.io/coverage). All three are optional.

For a nightly matrix across simulators, put the regression stage in a
`matrix` over `SIM` (`msim`, `vcs`, `xrun`, `questa`, `iverilog`) with
`-o regress-$SIM`, and give each its own `junit`/`publishHTML` step.

## Bug reports

`mbug` turns failures into issues, one issue per failure signature, so a
bug that breaks 200 seeds is one issue, not 200. The signature is the same
one `report.html` groups by: the first error line with numbers, paths and
hex replaced by placeholders. It is stored on the issue as `mbug-<hash>`
(a label on GitHub and Jira, a `mbug-marker:` line in the description on
Redmine), and that is how the next build finds the issue again:

| The signature | mbug |
| --- | --- |
| has no issue yet | creates one: tests and seeds, reason, `mregress ... -filter '^test$' -seed S` to reproduce, the simulator command, the last 30 log lines, the build link, and (Redmine, Jira) `sim.log` and `cmd.sh` attached |
| has an open issue | adds a comment with this build, tests and seeds (`-no-comment` turns that off) |
| had an issue that was closed | creates a new one that says which issue it came back from |

At most `max_issues` (default 20) are created per run, so one broken build
does not file a pile of issues. The rest are listed as skipped.

| Tracker | Cost | `project` | Environment |
| --- | --- | --- | --- |
| `github` (default) | free | `owner/name` | `GITHUB_TOKEN` (Issues: read and write); in GitHub Actions the repository is taken from `GITHUB_REPOSITORY` |
| `redmine` | free, self-hosted | project identifier | `REDMINE_URL`, `REDMINE_KEY` (My account, API access key; enable the REST API in Administration) |
| `jira` | Cloud is free for up to 10 users | project key | `JIRA_URL`, `JIRA_USER` (e-mail), `JIRA_TOKEN` (API token); Server / Data Center: `JIRA_URL`, `JIRA_PAT` |

GitHub cannot take attachments through its API, so on GitHub the issue
quotes the log tail and links the Jenkins build's artifacts, where the
Jenkinsfile archives every run directory.

To set it up in Jenkins:

1. Make a token for your tracker and add it as a Jenkins *Secret text*
   credential with the id `mbug-token`.
2. Set the tracker and `project` in [`mbug.json`](mbug.json). For Redmine or
   Jira, also set the URL (and `JIRA_USER`) in the Jenkinsfile's
   `environment` block.
3. Run the job with `BUGS` on and `TRACKER` set. When the regression fails,
   the `failure` post step runs
   `mbug regress regress/results.json -tracker <tracker> -config ci/mbug.json`
   and archives `bugs.json`.

In GitHub Actions, a step after the regression is enough:

```yaml
      - name: File failures as issues
        if: failure()
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}   # needs permissions: issues: write
        run: mbug regress regress/results.json
```

Jenkins sets `BUILD_URL`, `JOB_NAME`, `BUILD_NUMBER` and `GIT_COMMIT`, and
GitHub Actions sets `GITHUB_RUN_ID` and related variables. mbug puts the
build link in every issue and comment. Try a configuration first with
`-dry-run`, which prints what would be filed and talks to no server.

`mbug.json` keys: `tracker`, `project`, `labels`, `assignee`, `attach`,
`max_issues`; for Jira `issuetype` (default `Bug`), `components`,
`priority`, `fields` (any other field id, such as a custom field); for
Redmine `tracker_name`, `priority`, `markup` (`textile`, the default, or
`markdown`), `signature_field` (a custom field id to hold the signature
instead of the description line), `fields`. Tokens are not accepted there.

Lint and DFT: `mlint ... | mbug lint - -config ci/mbug.json` files one
issue per rule, with every finding in a table. `-group finding` files one
issue per finding instead, and `-tool mdft` labels them as DFT.

### GitHub issues in Jira or Redmine

If the team plans in Jira or Redmine but users report bugs on GitHub,
`mbug sync-github` copies the repository's open issues across (pull
requests are left out), one issue each, linking back. Then it keeps them in
step:

| GitHub | Jira / Redmine | mbug |
| --- | --- | --- |
| closed | open | comments and moves it to `close_state` (a Jira transition or Redmine status, e.g. `Done`) |
| open, updated after the other side resolved it | resolved | comments and moves it to `reopen_state` |
| open | resolved after the last GitHub activity | with `-two-way`: closes the GitHub issue with a comment naming the other issue |

```sh
mbug sync-github -repo you/repo -tracker jira -project VER -config sync.json -backlink
```

`-backlink` comments the new key on each GitHub issue. `-gh-label bug` syncs
only labelled issues, `-since 2026-01-01` only recent ones, and
`-include-closed` also copies issues that are already closed. Run it on a
schedule (a Jenkins cron trigger or a GitHub Actions `schedule:`); each run
only changes what moved.

## Anything else (GitLab, GitHub Actions, a cron job)

```sh
mregress tests.list -sim msim -j "$(nproc)" -seeds 3 -o regress -cov
```

Then collect `regress/junit.xml` for the test report, `regress/cov.info` for
coverage, and `regress/report.html` plus `regress/cov_html/` as artifacts. On
failure, `mregress tests.list -o regress -rerun` repeats only the failures.
