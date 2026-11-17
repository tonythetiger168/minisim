#!/bin/sh
# run.sh - standard benchmarks on mvplat, and on Spike and QEMU when they
# are installed: CoreMark, Dhrystone and the Embench-IoT programs, built for
# a bare RV64 hart, timed by the host's wall clock (best of RUNS).
#
#   bench/mvplat/run.sh MVPLAT [WORKDIR]
#     SPIKE=path  QEMU=path (qemu-system-riscv64)  RUNS=3
#     MARCH=rv64imac (CoreMark, Dhrystone; Embench uses rv64gc: some of it
#     is floating point)
#
# The benchmarks are fetched at pinned commits into WORKDIR and never kept in
# this repository (CoreMark: Apache-2.0 with EEMBC's naming rules; Embench:
# GPL-3.0; Dhrystone from riscv-tests: BSD). Only the port in this directory
# (rt.c, coremark/, embench/, inc/) is minisim's. Each program is built twice:
# printing on the UART and leaving through the test finisher (mvplat, QEMU),
# and silent, leaving through HTIF's tohost (mvplat, Spike).
#
# Output: WORKDIR/results.tsv (program, simulator, seconds, exit status) and a
# table on stdout. A run that exits non-zero is a failure (CoreMark: a wrong
# CRC; Embench: verify_benchmark failed).
set -e
MVPLAT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
W=${2:-bench-mvplat}
RUNS=${RUNS:-3}
MARCH=${MARCH:-rv64imac}
HERE=$(cd "$(dirname "$0")" && pwd)
VPLAT=$HERE/../../tests/vplat
SPIKE=${SPIKE:-$(command -v spike || true)}
QEMU=${QEMU:-$(command -v qemu-system-riscv64 || true)}
COREMARK_REV=1f483d5b8316753a742cbf5590caf5bd0a4e4777
EMBENCH_REV=09c2ed8c3b7008c95d08b038de4a3f6dc103ed70
RVTESTS_REV=3df703cd52d5f0d8224e3c50bf03c6cb70efff98

mkdir -p "$W/elf"
W=$(cd "$W" && pwd)
fetch() {  # fetch NAME URL REV
  [ -d "$W/$1/.git" ] && return 0
  git init -q "$W/$1"
  git -C "$W/$1" fetch -q --depth 1 "$2" "$3"
  git -C "$W/$1" checkout -q FETCH_HEAD
}
fetch coremark https://github.com/eembc/coremark.git $COREMARK_REV
fetch embench https://github.com/embench/embench-iot.git $EMBENCH_REV
fetch riscv-tests https://github.com/riscv-software-src/riscv-tests.git $RVTESTS_REV

CC="${CLANG:-clang} --target=riscv64-unknown-elf -mabi=lp64 -mcmodel=medany -O2 -g0 -nostdlib -ffreestanding -fno-builtin"
CC="$CC -fuse-ld=lld -static -Wl,-T,$HERE/link.ld -Wl,--build-id=none -w -I$HERE/inc"
RT="$VPLAT/crt0.S $HERE/rt.c"
# Dhrystone's header fixes its run count; let the build set it.
mkdir -p "$W/dhrystone"
cp "$W/riscv-tests/benchmarks/dhrystone/"* "$W/dhrystone/"
sed -i 's/^#define NUMBER_OF_RUNS.*/#ifndef NUMBER_OF_RUNS\n#define NUMBER_OF_RUNS 500\n#endif/' "$W/dhrystone/dhrystone.h"

EMB="aha-mont64 crc32 edn huffbench matmult-int md5sum nettle-aes nettle-sha256 nsichneu picojpeg qrduino
     sglib-combined slre statemate tarfind ud wikisort"
for v in uart htif; do
  X=""; [ $v = htif ] && X=-DEXIT_HTIF
  C=$W/coremark
  $CC -march=$MARCH $X -I"$HERE/coremark" -I"$C" -DITERATIONS=${COREMARK_ITERATIONS:-4000} -DPERFORMANCE_RUN=1 \
    "-DFLAGS_STR=\"-O2 -march=$MARCH\"" $RT "$HERE/coremark/core_portme.c" "$C/core_list_join.c" "$C/core_main.c" \
    "$C/core_matrix.c" "$C/core_state.c" "$C/core_util.c" -o "$W/elf/coremark.$v.elf"
  $CC -march=$MARCH $X -std=gnu89 -I"$W/dhrystone" -DNUMBER_OF_RUNS=${DHRYSTONE_RUNS:-2000000} $RT "$HERE/dhrystone_rt.c" \
    "$W/dhrystone/dhrystone_main.c" "$W/dhrystone/dhrystone.c" -o "$W/elf/dhrystone.$v.elf"
  E=$W/embench
  for b in $EMB; do
    $CC -march=rv64gc -mabi=lp64d $X -DHAVE_BOARDSUPPORT_H -I"$HERE/embench" -I"$E/support" -I"$E/src/$b" \
      -DCPU_MHZ=${EMBENCH_MHZ:-100} -DGLOBAL_SCALE_FACTOR=${EMBENCH_MHZ:-100} $RT "$HERE/embench/boardsupport.c" \
      "$E/support/main.c" "$E/support/beebsc.c" "$E/src/$b/"*.c -o "$W/elf/$b.$v.elf"
  done
done

now() { date +%s.%N; }
# best SIM PROGRAM CMD...: the least wall time of RUNS runs, and the status.
: > "$W/results.tsv"
best() {
  sim=$1; prog=$2; shift 2
  b=""; st=0
  i=0
  while [ $i -lt "$RUNS" ]; do
    t0=$(now); set +e; "$@" > "$W/$prog.$sim.out" 2>&1; st=$?; set -e; t1=$(now)
    t=$(echo "$t1 - $t0" | bc)
    if [ -z "$b" ] || [ "$(echo "$t < $b" | bc)" = 1 ]; then b=$t; fi
    i=$((i + 1))
  done
  printf '%s\t%s\t%.3f\t%s\n' "$prog" "$sim" "$b" "$st" >> "$W/results.tsv"
}
for p in coremark dhrystone $EMB; do
  best mvplat "$p" "$MVPLAT" "$W/elf/$p.htif.elf" -q
  [ -n "$SPIKE" ] && best spike "$p" "$SPIKE" --isa=rv64gc "$W/elf/$p.htif.elf"
  [ -n "$QEMU" ] && best qemu "$p" "$QEMU" -machine virt -nographic -bios none -kernel "$W/elf/$p.uart.elf"
done
awk -F'\t' '
  { t[$1, $2] = $3; s[$1, $2] = $4; if (!($1 in seen)) { seen[$1] = 1; order[n++] = $1 }
    if (!($2 in sims)) { sims[$2] = 1; sl[m++] = $2 } }
  END {
    printf "%-16s", "program"; for (j = 0; j < m; j++) printf "%12s", sl[j]; printf "\n"
    for (i = 0; i < n; i++) {
      printf "%-16s", order[i]
      for (j = 0; j < m; j++) {
        k = order[i] SUBSEP sl[j]
        if (k in t) printf "%11.2fs%s", t[k], s[k] == 0 ? "" : "!"; else printf "%12s", "-"
      }
      printf "\n"
    }
    print "(seconds, best of runs; ! = non-zero exit)"
  }' "$W/results.tsv"
