#!/bin/sh
# Times memu on a generated netlist, on one thread and on all of them, and
# checks that both give the same files.
#   bench/emu/run.sh [BLOCKS] [FPGAS]     (5000 blocks is about 910,000 cells)
set -e
here=$(cd "$(dirname "$0")" && pwd); root=$here/../..
memu=${MEMU:-$root/build/memu}
blocks=${1:-5000}; fpgas=${2:-4}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
python3 "$here/gen_netlist.py" "$blocks" > "$tmp/big.gv"
cells=$(grep -c '_X1 ' "$tmp/big.gv"); lut=$((cells * 43 / 100 / fpgas))
echo "$cells cells, $fpgas FPGAs of $lut LUTs"
for t in 1 0; do
  args="-threads $t"; [ $t = 0 ] && args=""
  "$memu" "$tmp/big.gv" -lib "$root/tests/sta/sta_test.lib" -fpgas "$fpgas" -lut "$lut" -pins 2000 -o "$tmp/out$t" $args | grep -E "thread|cut cost"
done
cat "$tmp"/out1/big_fpga*.v | cksum > "$tmp/a"; cat "$tmp"/out0/big_fpga*.v | cksum > "$tmp/b"
cmp -s "$tmp/a" "$tmp/b" && echo "same result on 1 thread and on all" || { echo "DIFFERENT results"; exit 1; }
