#!/bin/sh
# build.sh - rebuild the test programs in elf/ from their sources. The ELF
# files are checked in, so the tests run where no RISC-V compiler is
# installed; run this after changing a program and commit the result.
#   tests/vplat/build.sh [OUTDIR]     (needs clang with the riscv32 and riscv64 targets, and lld)
set -e
cd "$(dirname "$0")"
out=${1:-elf}
mkdir -p "$out"
CC="${CLANG:-clang} --target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -O2 -g0 -nostdlib -ffreestanding -fuse-ld=lld -Wl,-T,link.ld -Wl,--build-id=none -static"
for p in hello irq smp bench pmp; do
  $CC crt0.S $p.c -o "$out/$p.elf"
done
# The same programs as RV64 (the ELF class makes mvplat's harts 64-bit).
CC64="${CLANG:-clang} --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany -O2 -g0 -nostdlib -ffreestanding -fuse-ld=lld -Wl,-T,link.ld -Wl,--build-id=none -static"
for p in hello irq smp; do
  $CC64 crt0.S $p.c -o "$out/${p}64.elf"
done
