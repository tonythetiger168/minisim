#!/bin/bash
# mvplat_isa.sh - the RISC-V architecture tests (riscv-tests, BSD-3-Clause:
# github.com/riscv-software-src/riscv-tests) on mvplat: every ui, um, ua, uc,
# uf, ud, uzba, uzbb, uzbc, uzbs, uzicond, uzbkb, uzbkx, uzabha, uzacas,
# uzabha_zacas, uzfh, si and mi test, for RV32 and RV64 (and RV64's mzicbo and
# ssvnapot), in its "p" environment (physical memory), and the user-level ones again in
# the "v" environment, which runs them in user mode under Sv32 or Sv39 paging
# (and RV64's ui, um, ua and uc again under Sv48) with a small supervisor
# that maps pages on demand. A test ends through HTIF tohost with the number
# of the first failing case. The vector benchmarks (benchmarks/vec-*) run
# too, at VLEN 128 and 512.
#
# The tests are fetched at a pinned commit and built with clang; nothing from
# them is shipped. Without git access or a clang that targets riscv32 (and
# lld), the test is skipped (exit 77).
#   tests/mvplat_isa.sh MVPLAT OUT
set -u
MVPLAT=$1 OUT=$2
SRC_VPLAT=$(cd "$(dirname "$0")" && pwd)/vplat
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
# The tests of a suite, as its Makefrag lists them (rv32ud leaves out move).
tests_of() {
  awk -v v="$1_sc_tests" '$1 == v { on = 1; sub(/^[^=]*=/, "") }
    on { l = $0; more = (l ~ /\\[ \t]*$/); gsub(/\\/, "", l); print l; if (!more) on = 0 }' \
    "$RT/isa/$1/Makefrag" | tr -s ' \t' '\n' | grep -v '^$'
}
# clang's assembler refuses a weak symbol made global later in the file; the
# tests that define their own trap handler do exactly that.
unweak() {
  local h
  for h in mtvec_handler stvec_handler; do
    if grep -q "$h:" "$1"; then sed -i -E "s/\\.weak $h;?//g" "$1"; fi
  done
}
# Some rv32 tests reuse the rv64 sources, whose constants are 64-bit; the GNU
# assembler keeps the low 32 bits of such an li, clang refuses it. Keep the
# low 32 bits as GNU as does.
rv32li() {
  perl -pi -e 's/\bli (\w+), 0x([0-9a-fA-F]{9,16})\b/"li $1, 0x" . substr($2, -8)/ge' "$1"
}
# clang 18 has no Zabha (and Zacas only as an experimental extension, given
# on the command line): write the byte and halfword AMOs as .insn.
zabha() {
  perl -pi -e 's/\.option arch(, \+(zabha|zacas)\b)+//g;
    my %f = (add => 0, swap => 1, cas => 5, xor => 4, or => 8, and => 12, min => 16, max => 20, minu => 24, maxu => 28);
    s/\bamo(add|swap|cas|xor|or|and|minu|maxu|min|max)\.([bh])(\.aqrl|\.aq|\.rl)?\s+(\w+),\s*(\w+),\s*(?:0\s*)?\((\w+)\)/
      do { my ($o, $w, $q, $d, $b, $a) = ($1, $2, $3 \/\/ "", $4, $5, $6);
        sprintf(".insn r 0x2f, %d, %d, %s, %s, %s", $w eq "b" ? 0 : 1, $f{$o} << 2 | (index($q, "aq") >= 0 ? 2 : 0) | (index($q, "rl") >= 0 ? 1 : 0), $d, $a, $b) }/ge' "$1"
}
# clang 18 has no .float16: write each value as the half's bits (Python
# rounds a decimal to half to nearest even, as the GNU assembler does).
float16() {
  grep -q '\.float16' "$1" || return 0
  python3 -I -c '
import re, struct, sys
def h(m):
    v = m.group(1).strip()
    if v.startswith("0h:"): return ".half 0x" + v[3:]
    if v.lower() == "nan": return ".half 0x7e00"
    return ".half 0x%04x" % struct.unpack("<H", struct.pack("<e", float(v)))[0]
p = sys.argv[1]
s = open(p).read()
open(p, "w").write(re.sub(r"\.float16\s+([^;\n]+)", h, s))' "$1"
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
  B=_zba_zbb_zbc_zbs_zicond_zbkb_zbkx_zacas1p0_zfh
  if [ $X = 32 ]; then CL="$CLANG --target=riscv32-unknown-elf -march=rv32imafdc_zicsr_zifencei$B -menable-experimental-extensions -mabi=ilp32"; VM=rv32imafdc_zicsr_zifencei
  else CL="$CLANG --target=riscv64-unknown-elf -march=rv64imafdc_zicsr_zifencei$B -menable-experimental-extensions -mabi=lp64 -mcmodel=medany"; VM=rv64imafdc_zicsr_zifencei; fi
  BSUITES="rv${X}uzba rv${X}uzbb rv${X}uzbc rv${X}uzbs rv${X}uzicond rv${X}uzbkb rv${X}uzbkx rv${X}uzabha rv${X}uzacas rv${X}uzabha_zacas rv${X}uzfh"
  PONLY="rv${X}si rv${X}mi"
  [ $X = 64 ] && PONLY="$PONLY rv64mzicbo rv64ssvnapot"
  for suite in rv${X}ui rv${X}um rv${X}ua rv${X}uc rv${X}uf rv${X}ud $BSUITES $PONLY; do
    for n in $(tests_of $suite); do
      f=$RT/isa/$suite/$n.S
      t=$suite-p-$n
      $CL -E -I"$RT/env/p" -I"$RT/isa/macros/scalar" "$f" -o $t.s
      unweak $t.s
      sed -i 's/\btcontrol\b/0x7a5/g' $t.s   # a CSR name clang 18 does not know
      [ $X = 32 ] && rv32li $t.s
      zabha $t.s
      float16 $t.s
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
  for suite in rv${X}ui rv${X}um rv${X}ua rv${X}uc rv${X}uf rv${X}ud $BSUITES; do
    for n in $(tests_of $suite); do
      f=$RT/isa/$suite/$n.S
      t=$suite-v-$n
      $CL -E -I"$RT/env/v" -I"$RT/isa/macros/scalar" "$f" -o $t.s
      unweak $t.s
      [ $X = 32 ] && rv32li $t.s
      zabha $t.s
      float16 $t.s
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
  # RV64 again under Sv48 (4 levels): the supervisor built with -DSv48.
  if [ $X = 64 ]; then
    for suite in rv64ui rv64um rv64ua rv64uc; do
      for n in $(tests_of $suite); do
        f=$RT/isa/$suite/$n.S
        t=$suite-v48-$n
        $CL -E -I"$RT/env/v" -I"$RT/isa/macros/scalar" "$f" -o $t.s
        unweak $t.s
        entropy=0x$(echo $t | md5sum | cut -c 1-7)
        if ! $VCC -c -x assembler $t.s -o $t.o 2> $t.err ||
           ! $VCC -march=$VM -DSv48 -DENTROPY=$entropy -c "$RT/env/v/vm.c" -o $t.vm.o 2>> $t.err ||
           ! $VCC -static -fuse-ld=lld -Tv-link.ld v$X-entry.o $t.vm.o v$X-string.o $t.o -o $t 2>> $t.err; then
          failed="$failed $t(build)"; continue
        fi
        run $t
      done
    done
  fi
done
# The vector benchmarks (vec-daxpy, vec-memcpy, vec-sgemm, vec-strcmp),
# which check their own results. Their common/syscalls.c uses GCC's nested
# functions, which clang lacks: tests/vplat/bench_rt.c stands in for it.
# clang wants _Atomic objects in stdatomic.h's calls and the linker script's
# flags as a number; vec-sgemm's vsetvli leaves LMUL out, and the routine
# uses s3 without saving it, which a clang-compiled caller notices.
mkdir -p binc/sys
: > binc/sys/signal.h
cp libc/*.h binc/
cat > binc/stdatomic.h <<'EOH'
#pragma once
typedef enum { memory_order_relaxed = __ATOMIC_RELAXED, memory_order_acquire = __ATOMIC_ACQUIRE,
               memory_order_release = __ATOMIC_RELEASE, memory_order_acq_rel = __ATOMIC_ACQ_REL,
               memory_order_seq_cst = __ATOMIC_SEQ_CST } memory_order;
#define atomic_fetch_add_explicit(p, v, o) __atomic_fetch_add((p), (v), (o))
#define atomic_store_explicit(p, v, o) __atomic_store_n((p), (v), (o))
#define atomic_load_explicit(p, o) __atomic_load_n((p), (o))
EOH
BM=$RT/benchmarks
sed 's/FLAGS(SHF_ALLOC | SHF_EXECINSTR)/FLAGS(7)/' "$BM/common/test.ld" > bench.ld
perl -0pe 's/e32, ta, ma/e32, m1, ta, ma/; s/(    REG_S s2, 16\(sp\)\n)/$1    REG_S s3, 24(sp)\n/; s/(    REG_L s2, 16\(sp\)\n)/$1    REG_L s3, 24(sp)\n/' \
  "$BM/vec-sgemm/vec-sgemm.S" > vec-sgemm.S
for b in vec-daxpy vec-memcpy vec-sgemm vec-strcmp; do
  s=$BM/$b/$b.S
  [ $b = vec-sgemm ] && s=vec-sgemm.S
  if ! $CLANG --target=riscv64-unknown-elf -march=rv64gcv -mabi=lp64d -mcmodel=medany -static -O2 -std=gnu99 \
       -ffast-math -fno-common -fno-builtin -nostdlib -fuse-ld=lld -DPREALLOCATE=1 -Ibinc -I"$RT/env" \
       -I"$BM/common" -I"$BM/$b" -T bench.ld "$BM/$b"/*.c "$s" "$SRC_VPLAT/bench_rt.c" "$BM/common/crt.S" -o $b 2> $b.err; then
    failed="$failed $b(build)"; continue
  fi
  for v in 128 512; do
    timeout 60 "$MVPLAT" $b -q -vlen $v -max-cycles 50000000 > $b-$v.out 2>&1
    rc=$?
    if [ $rc = 0 ]; then pass=$((pass + 1)); else failed="$failed $b-vlen$v(exit $rc)"; fi
  done
done
echo "riscv-tests: $pass passed"
if [ -n "$failed" ]; then echo "failed:$failed"; exit 1; fi
