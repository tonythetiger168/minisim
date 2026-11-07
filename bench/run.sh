#!/usr/bin/env bash
# bench/run.sh - time a benchmark design on msim (interpreter and -compile),
# Verilator and Icarus. The top module is named after the file.
#   bench/run.sh [design.sv] [build-dir] [end-time]
# Defaults: lanes.sv, ../build, and the design's own END_TIME (10 per cycle).
# Prints the checksum of each run so a wrong answer is visible next to its time.
# Build time and run time are separate columns. Tools that are not installed are skipped.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
sv=$(realpath "${1:-$here/lanes.sv}")
top=$(basename "$sv" .sv)
msim=$(cd "${2:-$here/../build}" && pwd)/msim
def=()
if [ -n "${3:-}" ]; then def=(+define+END_TIME=$3); fi
vdef=("${def[@]/+define+/-D}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
now() { date +%s.%N; }
row() { printf '%-22s %10s %10s  %s\n' "$1" "$2" "$3" "$4"; }
t() { local s; s=$(now); "$@" > "$work/out" 2>&1; echo "$(now) - $s" | bc | xargs printf '%.2f'; }

echo "== $top"
row tool build_s run_s checksum
b=0; r=$(t "$msim" "$sv" "${def[@]}")
row "msim (interpreter)" "-" "$r" "$(grep -o 'checksum=.*' "$work/out")"

b=$(t "$msim" -compile -o "$work/msim" "$sv" "${def[@]}" -maxtime 0)
r=$(t "$work/msim/simv")
row "msim -compile" "$b" "$r" "$(grep -o 'checksum=.*' "$work/out")"

if command -v verilator > /dev/null; then
  b=$(t verilator --binary --timing -O3 -Wno-fatal --top-module "$top" "${vdef[@]}" \
        -Mdir "$work/vl" "$sv")
  r=$(t "$work/vl/V$top")
  row "verilator -O3" "$b" "$r" "$(grep -o 'checksum=.*' "$work/out")"
fi
if command -v iverilog > /dev/null; then
  b=$(t iverilog -g2012 "${vdef[@]}" -o "$work/a.vvp" "$sv")
  r=$(t vvp -n "$work/a.vvp")
  row "icarus" "$b" "$r" "$(grep -o 'checksum=.*' "$work/out")"
fi
