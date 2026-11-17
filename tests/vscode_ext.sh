#!/bin/bash
# The VS Code extension's own checks: TypeScript compile against the VS Code
# API types, and the unit tests of its helpers (node --test). Needs node and
# npm; installs the pinned packages from package-lock.json when they are not
# there. Exit 77 (skipped) without node/npm or when the packages cannot be
# installed (no network).
set -u
EXT=${1:?usage: vscode_ext.sh editors/vscode}
command -v node >/dev/null 2>&1 && command -v npm >/dev/null 2>&1 || { echo "skip: no node/npm"; exit 77; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# Work on a copy: the source tree stays clean.
cp -r "$EXT"/package.json "$EXT"/package-lock.json "$EXT"/tsconfig.json "$EXT"/src "$WORK"/
cd "$WORK"
if ! npm ci --no-audit --no-fund >npm.log 2>&1; then
  tail -5 npm.log
  echo "skip: npm ci failed (offline?)"
  exit 77
fi
npm run -s compile || { echo "FAIL: tsc"; exit 1; }
npm test || { echo "FAIL: unit tests"; exit 1; }
echo "vscode extension: compile and unit tests passed"
