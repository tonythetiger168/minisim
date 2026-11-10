#!/bin/sh
# Measures the mini45 Liberty files again: run this after changing the cell
# list, the transistor netlists or the device models in gen_mini45.py.
#
#   lib/mini45/characterize.sh <mchar> [-threads N]
#
# Takes a few minutes per corner on four cores. The result is the same on
# any thread count.
set -e
here=$(cd "$(dirname "$0")" && pwd)
mchar=${1:?usage: characterize.sh <mchar> [mchar options]}
shift
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 "$here/gen_mini45.py"
python3 "$here/gen_mini45.py" --templates "$tmp"
for corner in tt ss ff; do
    # Leakage stays analytic: a level-1 MOSFET has no subthreshold current.
    "$mchar" -lib "$tmp/mini45_$corner.lib" -spice "$here/mini45.sp" \
        -models "$here/mini45_models_$corner.sp" -keep leakage \
        -o "$here/mini45_$corner.lib" "$@"
done
