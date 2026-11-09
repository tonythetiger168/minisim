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

## Anything else (GitLab, GitHub Actions, a cron job)

```sh
mregress tests.list -sim msim -j "$(nproc)" -seeds 3 -o regress -cov
```

Then collect `regress/junit.xml` for the test report, `regress/cov.info` for
coverage, and `regress/report.html` plus `regress/cov_html/` as artifacts. On
failure, `mregress tests.list -o regress -rerun` repeats only the failures.
