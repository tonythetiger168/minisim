#!/bin/bash
# mvplat_isa.sh - the RISC-V architecture tests (riscv-tests, BSD-3-Clause:
# github.com/riscv-software-src/riscv-tests) on mvplat: every ui, um, ua, uc,
# si and mi test, for RV32 and RV64, in its "p" environment (physical memory),
# and the user-level ones again in the "v" environment, which runs them in
# user mode under Sv32 or Sv39 paging with a small supervisor that maps pages
# on demand. A test ends through HTIF tohost with the number of the first
# failing case.
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
# clang's assembler refuses a weak symbol made global later in the file; the
# tests that define their own trap handler do exactly that.
unweak() {
  local h
  for h in mtvec_handler stvec_handler; do
    if grep -q "$h:" "$1"; then sed -i -E "s/\\.weak $h;?//g" "$1"; fi
  done
}
run() {  # run TEST
  timeout 60 "$MVPLAT" $1 -q -max-cycles 20000000 > $1.out 2>&1
  local rc=$?
  if [ $rc = 0 ]; then pass=$((pass + 1)); else failed="$failed $1(case $rc)"; fi
}
# The "v" environment's supervisor (env/v/vm.c) is C. It includes three C
# library headers for a few declarations; with no RISC-V C library installed,
# these stand in for them.
mkdir -p libc
printf '#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\nvoid *memset(void *, int, size_t);\nsize_t strlen(const char *);\nint strcmp(const char *, const char *);\nint memcmp(const void *, const void *, size_t);\nchar *strcpy(char *, const char *);\nlong atol(const char *);\n' > libc/string.h
: > libc/stdio.h
: > libc/ctype.h
# The supervisor's small variables go in .sbss, which the linker script does
# not place. GNU ld puts it after .bss; lld puts it after .data, in the
# test's data page, which the supervisor then finds changed on exit with no
# store to it. Place it after .bss as GNU ld does.
sed 's/^  \.bss : { \*(\.bss) }/  .bss : { *(.bss) }\n  .sbss : { *(.sbss) }/' "$RT/env/v/link.ld" > v-link.ld

for X in 32 64; do
  if [ $X = 32 ]; then CL="$CLANG --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32"; VM=rv32imafc_zicsr_zifencei
  else CL="$CLANG --target=riscv64-unknown-elf -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany"; VM=rv64imafc_zicsr_zifencei; fi
  for suite in rv${X}ui rv${X}um rv${X}ua rv${X}uc rv${X}si rv${X}mi; do
    for f in "$RT"/isa/$suite/*.S; do
      t=$suite-p-$(basename "$f" .S)
      $CL -E -I"$RT/env/p" -I"$RT/isa/macros/scalar" "$f" -o $t.s
      unweak $t.s
      sed -i 's/\btcontrol\b/0x7a5/g' $t.s   # a CSR name clang 18 does not know
      if ! $CL -nostdlib -static -fuse-ld=lld -T"$RT/env/p/link.ld" -x assembler $t.s -o $t 2> $t.err; then
        failed="$failed $t(build)"; continue
      fi
      run $t
    done
  done
  # The "v" environment: the user-level tests in user mode, under Sv32 or
  # Sv39 paging, with a supervisor that maps pages on demand.
  VCC="$CL -O2 -mcmodel=medany -fvisibility=hidden -nostdlib -ffreestanding -fno-builtin -Ilibc -I$RT/env/v -I$RT/isa/macros/scalar"
  $VCC -c "$RT/env/v/string.c" -o v$X-string.o 2> v$X-string.err || failed="$failed v$X-string(build)"
  $VCC -c "$RT/env/v/entry.S" -o v$X-entry.o 2> v$X-entry.err || failed="$failed v$X-entry(build)"
  for suite in rv${X}ui rv${X}um rv${X}ua rv${X}uc; do
    for f in "$RT"/isa/$suite/*.S; do
      t=$suite-v-$(basename "$f" .S)
      $CL -E -I"$RT/env/v" -I"$RT/isa/macros/scalar" "$f" -o $t.s
      unweak $t.s
      # As upstream: the supervisor's page placement is seeded from the test name.
      entropy=0x$(echo $t | md5sum | cut -c 1-7)
      if ! $VCC -c -x assembler $t.s -o $t.o 2> $t.err ||
         ! $VCC -march=$VM -DENTROPY=$entropy -c "$RT/env/v/vm.c" -o $t.vm.o 2>> $t.err ||
         ! $VCC -static -fuse-ld=lld -Tv-link.ld v$X-entry.o $t.vm.o v$X-string.o $t.o -o $t 2>> $t.err; then
        failed="$failed $t(build)"; continue
      fi
      run $t
    done
  done
done
echo "riscv-tests: $pass passed"
if [ -n "$failed" ]; then echo "failed:$failed"; exit 1; fi
