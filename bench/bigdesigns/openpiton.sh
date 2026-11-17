#!/usr/bin/env bash
# OpenPiton (PrincetonUniversity/openpiton; BSD + GPL OpenSPARC T1 core: external test input only) on msim.
#   bench/bigdesigns/openpiton.sh X Y   # X x Y tile mesh of OpenSPARC T1 cores: sims -vlt_build makes the
#                                       # preprocessed RTL and file list (and Verilator's model), then msim -elab chip
# needs: csh, libbit-vector-perl
source "$(dirname "$0")/common.sh"
X=${1:-1}; Y=${2:-1}
clone https://github.com/PrincetonUniversity/openpiton openpiton
export PITON_ROOT=$BIG/openpiton; source "$PITON_ROOT/piton/piton_settings.bash"
export PATH=$(dirname "$VERILATOR"):$DV_ROOT/tools/bin:$PATH VERILATOR_ROOT=$(dirname "$VERILATOR")/../share/verilator
cd "$PITON_ROOT/build"
# MAKEFLAGS keeps the generated Verilator make -j from using every core and all memory
MAKEFLAGS=-j2 $TM sims -sys=manycore -x_tiles=$X -y_tiles=$Y -vlt_build -vlt_build_args=--no-timing 2>&1 | grep -E "Walltime|TM "
R=$PITON_ROOT/build/manycore/rel-0.1
# -v monitors: msim does not yet see main-file macros in -v files, so they are plain sources here and
# only the chip is elaborated (the monitors reference cmp_top paths that do not exist under chip)
sed -e 's#//.*##' -e 's/^-v //' "$R/flist" | grep -v slam_init > "$R/msim.f"
cd "$R" && $TM "$MSIM" --single-unit -elab -top chip +define+SYNC_MUX +define+PITON_DPI -f msim.f 2>&1 | tail -1
