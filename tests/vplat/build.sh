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
for p in hello irq smp bench pmp virtio vnet vcon; do
  $CC crt0.S $p.c -o "$out/$p.elf"
done
# The same programs as RV64 (the ELF class makes mvplat's harts 64-bit).
CC64="${CLANG:-clang} --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany -O2 -g0 -nostdlib -ffreestanding -fuse-ld=lld -Wl,-T,link.ld -Wl,--build-id=none -static"
for p in hello irq smp virtio; do
  $CC64 crt0.S $p.c -o "$out/${p}64.elf"
done
# The V extension: vkern.c's loops scalar and vectorized by clang (vec),
# and single instructions against scalar models (vops), RV32 and RV64.
VF="-O3 -g0 -nostdlib -ffreestanding -fno-builtin -fno-math-errno"
for x in 32 64; do
  if [ $x = 32 ]; then T="--target=riscv32-unknown-elf -mabi=ilp32d"; else T="--target=riscv64-unknown-elf -mabi=lp64d -mcmodel=medany"; fi
  V="${CLANG:-clang} $T $VF"
  $V -march=rv${x}gc -DSFX=_s -c vkern.c -o "$out/vk_s.o"
  $V -march=rv${x}gcv -DSFX=_v -c vkern.c -o "$out/vk_v.o"
  $V -march=rv${x}gcv -fuse-ld=lld -Wl,-T,link.ld -Wl,--build-id=none -static crt0.S vec.c "$out/vk_s.o" "$out/vk_v.o" -o "$out/vec$x.elf"
  # LMUL 8 register groups too
  $V -march=rv${x}gcv -DSFX=_v -mllvm -riscv-v-register-bit-width-lmul=8 -c vkern.c -o "$out/vk_v.o"
  $V -march=rv${x}gcv -fuse-ld=lld -Wl,-T,link.ld -Wl,--build-id=none -static crt0.S vec.c "$out/vk_s.o" "$out/vk_v.o" -o "$out/vec${x}m8.elf"
  $V -march=rv${x}gcv -fno-vectorize -fno-slp-vectorize -fuse-ld=lld -Wl,-T,link.ld -Wl,--build-id=none -static \
    crt0.S vops.c -o "$out/vops$x.elf"
done
# ... and as a Linux program, for the BusyBox test.
V="${CLANG:-clang} --target=riscv64-unknown-linux-gnu -mabi=lp64d $VF -fno-stack-protector"
$V -march=rv64gc -DSFX=_s -c vkern.c -o "$out/vk_s.o"
$V -march=rv64gcv -DSFX=_v -c vkern.c -o "$out/vk_v.o"
$V -march=rv64gcv -static -fuse-ld=lld -Wl,--build-id=none vec_linux.c "$out/vk_s.o" "$out/vk_v.o" -o "$out/vec_linux64"
rm -f "$out/vk_s.o" "$out/vk_v.o"
# A supervisor-mode payload for OpenSBI, at the address firmware starts the
# kernel (RAM + 2 MiB).
${CLANG:-clang} --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany -O2 -g0 -nostdlib \
  -ffreestanding -fuse-ld=lld -Wl,-T,link-s.ld -Wl,--build-id=none -static sbi_start.S sbi.c -o "$out/sbi64.elf"
# /init for the Linux boot test, and the initramfs that holds it.
${CLANG:-clang} --target=riscv64-unknown-linux-gnu -march=rv64imac -mabi=lp64 -O2 -g0 -static -nostdlib \
  -ffreestanding -fno-stack-protector -fuse-ld=lld -Wl,--build-id=none linux_init.c -o "$out/linux_init64"
python3 mkcpio.py "$out/initramfs64.cpio" d:dev c:dev/console:5:1 d:proc "f:init:$out/linux_init64"
rm -f "$out/linux_init64"
