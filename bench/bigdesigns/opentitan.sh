#!/usr/bin/env bash
# OpenTitan Earl Grey (lowRISC/opentitan, Apache-2.0) on msim: elaboration of chip_sim_tb.
#   bench/bigdesigns/opentitan.sh   # fusesoc --setup for the Verilator sim target gives the file list
source "$(dirname "$0")/common.sh"
clone https://github.com/lowRISC/opentitan opentitan
[ -x "$BIG/otvenv/bin/fusesoc" ] || { python3 -m venv "$BIG/otvenv"; "$BIG/otvenv/bin/pip" install -q fusesoc==2.4.5 edalize==0.6.3 hjson mako pyyaml; }
cd "$BIG/opentitan"
B=$BIG/ot-build
"$BIG/otvenv/bin/fusesoc" --cores-root . run --flag=fileset_top --target=sim --setup --build-root "$B" \
  --tool=verilator lowrisc:dv:top_earlgrey_chip_verilator_sim >/dev/null
D=$(dirname "$(ls "$B"/*/sim-verilator/*.vc)"); cd "$D"
{ grep '^+incdir' *.vc; grep '^-D' *.vc | sed 's/^-D/+define+/'; echo +define+DISABLE_PRIM_CDC_RAND_DELAY
  grep -E '\.(sv|v)$' *.vc; } > msim.f
$TM "$MSIM" --single-unit -elab -top chip_sim_tb -f msim.f 2>&1 | tail -1
