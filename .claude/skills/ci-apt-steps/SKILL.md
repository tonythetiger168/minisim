---
name: ci-apt-steps
description: Rules for editing .github/workflows (ci.yml, release.yml) - the apt mirror probe, APT_OK gating for optional installs, pinned actions, build parallelism. Use whenever a workflow step installs a package or a tool needs a new CI dependency.
---

# CI steps that install packages

Hosted runners have lost DNS for `azure.archive.ubuntu.com` and
`archive.ubuntu.com` before, which failed every apt step. Each job therefore
starts with **"Pick a reachable apt mirror"**, which probes a list of
mirrors, points apt at the first that answers, sets short retries and
timeouts, and exports `APT_OK` (`1` or `0`).

## Rules

1. **Every apt step goes after the mirror probe.** A new job copies the
   probe step as its first step after checkout.
2. **Optional installs** (a comparison tool, something a test skips when
   absent):

   ```yaml
   - name: Install Yosys and nextpnr (mfpga tests)
     if: env.APT_OK == '1'
     continue-on-error: true
     run: sudo timeout 420 apt-get install -y yosys nextpnr-ice40 nextpnr-ecp5
   ```

   Use `timeout N` inside the command, not a step-level `timeout-minutes`:
   killing the step leaves apt holding the dpkg lock and breaks the next one.
3. **Required packages the image normally has:** guard, so no network is
   needed in the common case:

   ```sh
   dpkg -s zlib1g-dev >/dev/null 2>&1 || sudo apt-get install -y zlib1g-dev
   ```
4. **A test must pass without the optional program.** It skips the
   comparison and still checks its own expected values.
5. **Building a dependency from source** (Icarus 13, ABC): cache the install
   directory with `actions/cache`, key on the tag, and build only on a cache
   miss. Check the license first (`third-party-code`).
6. **Pin actions to a commit SHA** with the version in a comment, as the
   existing steps do.
7. **Do not raise build parallelism** in the slang job above `-j2`: unbounded
   `-j` was killed (exit 143) on the hosted runner.
8. The two job names, `build-and-test` and `build-and-test-slang`, are the
   required checks on `main`. Renaming either blocks every merge.
9. Apply the same change to `release.yml` when it has the same step.
