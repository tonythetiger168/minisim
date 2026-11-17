# Shared settings for the big-design scripts (sourced).
# BIG: work area for clones and generated RTL (never committed); MSIM: msim binary.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIG=${BIG:-$HOME/bigdesigns}
MSIM=${MSIM:-$(cd "$HERE/../.." && pwd)/build/msim}
VERILATOR=${VERILATOR:-$HOME/verilator5054/bin/verilator}
TM="$HERE/tm"
mkdir -p "$BIG"
clone() {  # clone <url> <dir> [branch]: shallow clone once, with submodules
  [ -d "$BIG/$2/.git" ] || GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 ${3:+--branch $3} "$1" "$BIG/$2"
}
