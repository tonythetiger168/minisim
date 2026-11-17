#!/usr/bin/env bash
# NVDLA nv_full (nvdla/hw, NVIDIA Open NVDLA License: test input only, never vendored) on msim.
#   bench/bigdesigns/nvdla.sh   # generate vmod with tmake, msim -elab NV_nvdla, Verilator --cc for comparison
# needs perl modules: apt install libyaml-perl libio-tee-perl libcapture-tiny-perl
source "$(dirname "$0")/common.sh"
clone https://github.com/nvdla/hw nvdla
N=$BIG/nvdla; cd "$N"
[ -f tree.make ] || printf '\n/usr/bin/cpp\n/usr/bin/g++\n/usr/bin/perl\n/usr/bin/java\n\n\n\n\n\n\n' | make tree.make >/dev/null
[ -d outdir/nv_full/vmod ] || ./tools/bin/tmake -build vmod >/dev/null
cd verif/verilator
# Verilator's -I<dir> is both a library directory and an include directory
{ grep -v -- '^-Wno\|^--top-module\|^-I' verilator.f; grep '^-I' verilator.f | sed 's/^-I/-y /'
  grep '^-I' verilator.f | sed 's/^-I/+incdir+/'; echo +libext+.v; } | grep -v '^\s*$' > msim.f
$TM "$MSIM" -elab -top NV_nvdla -f msim.f 2>&1 | tail -1
rm -rf vl && $TM "$VERILATOR" --cc --Mdir vl -f verilator.f 2>&1 | tail -1
