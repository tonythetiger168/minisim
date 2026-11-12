#!/bin/sh
# build.sh - rebuild the test programs in elf/ from their sources. The ELF
# files are checked in, so the tests run where no RISC-V compiler is
# installed; run this after changing a program and commit the result.
#   tests/vplat/build.sh [OUTDIR]     (needs clang with the riscv32 target, and lld)
set -e
cd "$(dirname "$0")"
out=${1:-elf}
mkdir -p "$out"
CC="${CLANG:-clang} --target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -O2 -g0 -nostdlib -ffreestanding -fuse-ld=lld -Wl,-T,link.ld -Wl,--build-id=none -static"
for p in hello irq smp bench pmp; do
  $CC crt0.S $p.c -o "$out/$p.elf"
done
