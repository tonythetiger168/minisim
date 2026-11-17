#!/usr/bin/env bash
# OpenPiton (PrincetonUniversity/openpiton; BSD + GPL OpenSPARC T1 core: external test input only) on msim.
#   bench/bigdesigns/openpiton.sh X Y   # X x Y tile mesh: msim -elab chip, plus Verilator --cc for comparison
# needs: csh, libbit-vector-perl, libc6-i386 (OpenPiton's bundled 32-bit SPARC assembler, for running tests)
source "$(dirname "$0")/common.sh"
X=${1:-1}; Y=${2:-1}
clone https://github.com/PrincetonUniversity/openpiton openpiton
export PITON_ROOT=$BIG/openpiton; source "$PITON_ROOT/piton/piton_settings.bash"
W=$BIG/piton-${X}x${Y}; mkdir -p "$W/ncv/tools/bin"; cd "$W"
# sims writes the mesh-specific .tmp.v sources (pyhp) before it calls the simulator; a stub
# ncverilog lets it stop right after that without a Verilator or Xcelium build.
printf '#!/bin/sh\nexit 1\n' > ncv/tools/bin/ncverilog; chmod +x ncv/tools/bin/ncverilog
NCV_HOME=$W/ncv PATH=$W/ncv/tools/bin:$DV_ROOT/tools/bin:$PATH \
  sims -sys=manycore -x_tiles=$X -y_tiles=$Y -ncv_build > sims.log 2>&1 || true
echo "tiles: $(grep -c 'tile ' "$DV_ROOT/design/chip/rtl/chip.tmp.v")"
# chip only: the design sources (not verif/env), plus the behavioural PLL
sed -e 's#//.*##' flist | grep -v '^[^+].*verif/env\|^-v.*verif/env' > chip.f
echo "$DV_ROOT/verif/env/manycore/fake_pll.v" >> chip.f
$TM "$MSIM" -elab -top chip +define+SYNC_MUX +define+PITON_DPI -f chip.f 2>&1 | tail -2
rm -rf vl && $TM "$VERILATOR" --cc --no-timing -Wno-fatal -Wno-lint -Wno-style -Wno-BLKANDNBLK -Wno-TIMESCALEMOD \
  --unroll-count 256 --top-module chip -DSYNC_MUX -DPITON_DPI --Mdir vl -f chip.f 2>&1 | grep -E "Walltime|TM "
