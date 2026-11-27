#!/bin/bash
# difftest.sh - random A64 instruction blocks run on mvplat and on a
# reference, register by register. The reference is QEMU's user-mode
# emulator, run as a separate program (qemu-aarch64, from the distribution;
# no QEMU code is part of minisim). Each block starts from its own random
# registers, NZCV and FPCR (rounding mode, FZ, DN, AHP) and ends with a dump
# of x0-x28, NZCV, FPSR and v0-v31.
#   difftest.sh MVPLAT QEMU WORK SEED BLOCKS INSNS
# Environment: INTP (share of integer instructions, default .45), NOFP=1.
# Needs clang and ld.lld. Exit 1 when a block differs.
set -e
MVPLAT=$1 QEMU=$2 W=$3 S=$4 NB=$5 NI=$6
D=$(cd "$(dirname "$0")" && pwd)
A=$D/..
mkdir -p "$W"
cd "$W"
python3 "$D/mk.py" "$S" "$NB" "$NI" > p$S.S
CF="--target=aarch64-none-elf -O1 -nostdlib -ffreestanding -fno-builtin -fuse-ld=lld -Wno-main -mgeneral-regs-only"
clang --target=aarch64-none-elf -march=armv8-a -c -o p$S.o p$S.S
clang $CF -Wl,-Ttext=0x400000 -Wl,-e,_start -static -o l$S.elf "$D/linux.c" "$D/common.c" p$S.o
clang $CF -I"$A" -Wl,-T,"$A/link.ld" -o b$S.elf "$A/crt0.S" "$D/bare.c" "$D/common.c" p$S.o
"$QEMU" -cpu max l$S.elf > q$S.txt
"$MVPLAT" b$S.elf > m$S.txt 2> u$S.txt || true
python3 "$D/cmp.py" $S
