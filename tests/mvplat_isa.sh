#!/bin/bash
# mvplat_isa.sh - the RISC-V architecture tests (riscv-tests, BSD-3-Clause:
# github.com/riscv-software-src/riscv-tests) on mvplat: every rv32ui, rv32um,
# rv32ua, rv32uc and rv32mi test in its "p" environment, which ends through
# HTIF tohost with the number of the first failing case.
#
# The tests are fetched at a pinned commit and built with clang; nothing from
# them is shipped. Without git access or a clang that targets riscv32 (and
# lld), the test is skipped (exit 77).
#   tests/mvplat_isa.sh MVPLAT OUT
set -u
MVPLAT=$1 OUT=$2
REV=3df703cd52d5f0d8224e3c50bf03c6cb70efff98
ENV_REV=6de71edb142be36319e380ce782c3d1830c65d68
mkdir -p "$OUT"; cd "$OUT"
CLANG=${CLANG:-clang}
CL="$CLANG --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32"
printf '.globl _start\n_start: nop\n' > probe.s
if ! $CL -nostdlib -fuse-ld=lld -x assembler probe.s -o probe.elf 2>/dev/null; then
  echo "skipped: no clang with the riscv32 target and lld"; exit 77
fi
fetch() {  # fetch DIR URL REV
  [ -d "$1/.git" ] && git -C "$1" rev-parse HEAD 2>/dev/null | grep -q "$3" && return 0
  rm -rf "$1" && git init -q "$1" && git -C "$1" remote add origin "$2" &&
    timeout 300 git -C "$1" fetch -q --depth 1 origin "$3" && git -C "$1" checkout -q FETCH_HEAD
}
if ! fetch rt https://github.com/riscv-software-src/riscv-tests $REV ||
   ! fetch rt/env https://github.com/riscv/riscv-test-env $ENV_REV; then
  echo "skipped: cannot fetch riscv-tests"; exit 77
fi
RT=$OUT/rt
pass=0; failed=""
for suite in rv32ui rv32um rv32ua rv32uc rv32mi; do
  for f in "$RT"/isa/$suite/*.S; do
    t=$suite-p-$(basename "$f" .S)
    # clang's assembler refuses a weak symbol made global later in the file;
    # the tests that define their own trap handler do exactly that.
    $CL -E -I"$RT/env/p" -I"$RT/isa/macros/scalar" "$f" -o $t.s
    if grep -q "mtvec_handler:" $t.s; then sed -i 's/\.weak mtvec_handler;//; s/\.weak mtvec_handler$//' $t.s; fi
    sed -i 's/\btcontrol\b/0x7a5/g' $t.s   # a CSR name clang 18 does not know
    if ! $CL -nostdlib -static -fuse-ld=lld -T"$RT/env/p/link.ld" -x assembler $t.s -o $t 2> $t.err; then
      failed="$failed $t(build)"; continue
    fi
    timeout 60 "$MVPLAT" $t -q -max-cycles 5000000 > $t.out 2>&1
    rc=$?
    if [ $rc = 0 ]; then pass=$((pass + 1)); else failed="$failed $t(case $rc)"; fi
  done
done
echo "riscv-tests: $pass passed"
if [ -n "$failed" ]; then echo "failed:$failed"; exit 1; fi
