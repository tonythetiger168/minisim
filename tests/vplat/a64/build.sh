#!/bin/sh
# build.sh - rebuild the AArch64 test programs in elf/ (checked in, so the
# tests need no cross compiler). Needs clang and ld.lld.
set -e
cd "$(dirname "$0")"
mkdir -p elf
CF="--target=aarch64-none-elf -O2 -nostdlib -ffreestanding -fno-builtin -fno-math-errno -ffp-contract=off -fuse-ld=lld -Wno-main -Wl,-T,link.ld"
for p in ${*:-hello calc trap mmu irq smp}; do
    clang $CF -o elf/$p.elf crt0.S $p.c
done
