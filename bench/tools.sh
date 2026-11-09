#!/usr/bin/env bash
# bench/tools.sh - time the tools around msim on one design: waveforms (VCD,
# FST), coverage (-cov, mcov merge/report), lint/DFT, and a regression.
#   bench/tools.sh [build-dir] [cores] [cycles]
# Defaults: ../build, 512 cores of bench/gen_soc.py, 2000 cycles. Set
# DESIGN="files... -top t" to time another design (OpenTitan Ibex, say) for
# the simulation and lint/DFT rows. Each row is wall-clock seconds.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=$(cd "${1:-$here/../build}" && pwd)
cores=${2:-512}
cycles=${3:-2000}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
now() { date +%s.%N; }
t() {   # t <label> <command...>: one row
  local s rc=0
  s=$(now)
  "${@:2}" > "$work/out" 2>&1 || rc=$?
  printf '%-34s %8.2f%s\n' "$1" "$(echo "$(now) - $s" | bc)" "$([ $rc -eq 0 ] || echo "  (exit $rc)")"
}
if [ -z "${DESIGN:-}" ]; then
  python3 "$here/gen_soc.py" "$cores" "$cycles" > "$work/soc.sv"
  DESIGN="$work/soc.sv -top soc_tb"
fi
read -r -a D <<< "$DESIGN"
cd "$work"
echo "== tools on: $DESIGN"
t "msim" "$build/msim" "${D[@]}"
t "msim -vcd" "$build/msim" "${D[@]}" -vcd w.vcd
t "msim -fst" "$build/msim" "${D[@]}" -fst w.fst
t "msim -cov" "$build/msim" "${D[@]}" -cov c.json
t "msim -compile (build + run)" "$build/msim" -compile -o cdir "${D[@]}"
t "msim -compile (cached build)" "$build/msim" -compile -o cdir "${D[@]}"
t "msim -compile -vcd" "$build/msim" -compile -o cdir "${D[@]}" -vcd cw.vcd
t "msim -compile -cov (build + run)" "$build/msim" -compile -o cdir "${D[@]}" -cov cc.json
mkdir -p dbs
for i in $(seq 1 100); do cp c.json "dbs/r$i.json"; done
t "mcov merge (100 runs)" python3 "$here/../tools/mcov.py" merge dbs/*.json -o all.json
t "mcov report -html -lcov" python3 "$here/../tools/mcov.py" report all.json -html html -lcov all.info
t "mlint" "$build/mlint" "${D[@]}"
t "mdft" "$build/mdft" "${D[@]}"
t "mregress -spec tests/spec" python3 "$here/../tools/mregress.py" -spec "$here/../tests/spec" \
    -exe "$build/msim" -q -o reg
