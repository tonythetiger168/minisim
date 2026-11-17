#!/usr/bin/env bash
# OpenC910 (XUANTIE-RV/openc910, Apache-2.0) on msim with its own smart_run testbench.
#   bench/bigdesigns/c910.sh        # c910/hello.S -> inst.pat, run tb, expect "simulation finished successfully"
source "$(dirname "$0")/common.sh"
clone https://github.com/XUANTIE-RV/openc910 openc910
S=$BIG/openc910/smart_run
# msim today rejects arrays over 2^20 elements and stores ~350 bytes per element, so the
# 16 x 2 MB testbench RAM becomes 16 x 64 KB (enough for programs below 0xf0000).
sed -i 's/^parameter ADDR_WIDTH = 21;/parameter ADDR_WIDTH = 16;/' "$S/logical/mem/f_spsram_large.v"
mkdir -p "$S/work"; cd "$S/work"
clang --target=riscv64 -march=rv64imac -mabi=lp64 -nostdlib -fuse-ld=lld -Wl,-Ttext=0 -Wl,-e,__start -Wl,--no-relax \
  "$HERE/c910/hello.S" -o hello.elf
objcopy -I elf64-little -O srec -j .text hello.elf hello_inst.hex   # llvm-objcopy 18 has no srec
../tests/bin/Srec2vmem hello_inst.hex inst.pat
echo "@00000000  00000000  00000000  00000000  00000000" > data.pat
export CODE_BASE_PATH=$BIG/openc910/C910_RTL_FACTORY
# --single-unit: cpu_cfig.h's `defines must reach every later file, as in VCS/Verilator
$TM "$MSIM" --single-unit -top tb -f ../logical/filelists/sim.fl 2>&1 | grep -E "successfully|with error|TEST|TM "
