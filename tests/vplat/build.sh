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
# A supervisor-mode payload for OpenSBI, at the address firmware starts the
# kernel (RAM + 2 MiB).
${CLANG:-clang} --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany -O2 -g0 -nostdlib \
  -ffreestanding -fuse-ld=lld -Wl,-T,link-s.ld -Wl,--build-id=none -static sbi_start.S sbi.c -o "$out/sbi64.elf"
# /init for the Linux boot test, and the initramfs that holds it.
${CLANG:-clang} --target=riscv64-unknown-linux-gnu -march=rv64imac -mabi=lp64 -O2 -g0 -static -nostdlib \
  -ffreestanding -fno-stack-protector -fuse-ld=lld -Wl,--build-id=none linux_init.c -o "$out/linux_init64"
python3 mkcpio.py "$out/initramfs64.cpio" d:dev c:dev/console:5:1 d:proc "f:init:$out/linux_init64"
rm -f "$out/linux_init64"
