#!/bin/sh
# mspice against ngspice on the netlists of gen.py.
#   bench/spice/run.sh <mspice> [dir]
# Prints wall-clock seconds (best of three) for mspice at one thread, mspice
# at every CPU, and ngspice, and the first measurement of each as a check
# that they agree. mc.cir uses .step, which ngspice does not have.
set -e
MSPICE=$1
DIR=${2:-/tmp/mspice-bench}
HERE=$(dirname "$0")
mkdir -p "$DIR"
python3 "$HERE/gen.py" "$DIR"
best() {   # best of three wall-clock times of a command
    b=""
    for i in 1 2 3; do
        s=$(date +%s.%N); "$@" > "$DIR/out.txt" 2>&1 || true; e=$(date +%s.%N)
        t=$(echo "$e - $s" | bc)
        if [ -z "$b" ] || [ "$(echo "$t < $b" | bc)" = 1 ]; then b=$t; fi
    done
    echo "$b"
}
printf "%-8s %10s %10s %10s   %s\n" netlist "mspice-1" "mspice-all" ngspice "first measurement (mspice / ngspice)"
for n in ladder mesh chain ring mc; do
    t1=$(best "$MSPICE" "$DIR/$n.cir" -q -threads 1)
    ta=$(best "$MSPICE" "$DIR/$n.cir" -q)
    m=$(grep -m1 ' = ' "$DIR/out.txt" | awk '{print $3}')
    tn="-"; g="-"
    if [ "$n" != mc ] && command -v ngspice > /dev/null; then
        sed '/^\.end$/d' "$DIR/$n.cir" > "$DIR/ng_$n.cir"
        # Save what the measurements read, not every node.
        printf '.save v(n0) v(n10) v(n50) v(n200) v(n59_59)\n.end\n' >> "$DIR/ng_$n.cir"
        tn=$(best ngspice -b "$DIR/ng_$n.cir")
        g=$(grep -m1 -E '^[a-z0-9]+ +=' "$DIR/out.txt" | awk '{print $3}')
    fi
    printf "%-8s %10.3f %10.3f %10s   %s / %s\n" "$n" "$t1" "$ta" "$tn" "$m" "$g"
done
