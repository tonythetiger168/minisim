#!/usr/bin/env bash
# CVA6 / Ariane (openhwgroup/cva6, Solderpad 0.51) on msim.
#   bench/bigdesigns/cva6.sh elab   # core only (TARGET_CFG, default cv64a6_imafdc_sv39), plus Verilator lint
#   bench/bigdesigns/cva6.sh run    # corev_apu ariane_tb (UVM) + a bare-metal program, via elf_dpi.c
source "$(dirname "$0")/common.sh"
clone https://github.com/openhwgroup/cva6 cva6
C=$BIG/cva6
(cd "$C" && git submodule update --init --recursive --depth 1 -q)
export CVA6_REPO_DIR=$C TARGET_CFG=${TARGET_CFG:-cv64a6_imafdc_sv39} HPDCACHE_DIR=$C/core/cache_subsystem/hpdcache RISCV=/nonexistent
W=$BIG/cva6-$TARGET_CFG; mkdir -p "$W"; cd "$W"
python3 "$HERE/flatten_flist.py" "$C/core/Flist.cva6" > core.f
case "${1:-elab}" in
elab)
  $TM "$MSIM" -elab -top cva6 -f core.f 2>&1 | tail -1
  $TM "$VERILATOR" --lint-only -Wno-fatal -Wno-lint -Wno-style --top-module cva6 -f core.f 2>&1 | tail -1 ;;
run)
  : "${UVM_HOME:?set UVM_HOME to a uvm-core 2020.3.1 checkout}"
  P() { make -s --no-print-directory -C "$C" --eval='print-%: ; @echo $($*)' print-$1 2>/dev/null | tr ' ' '\n'; }
  { cat core.f; echo "$C/corev_apu/tb/common/mock_uart.sv"; for v in ariane_pkg uart_src_sv src; do P $v; done
    echo "$C/corev_apu/tb/ariane_tb.sv"; P list_incdir; } | grep -v '^$' \
    | awk -v R="$C" '/^\+incdir/ || /^\// {print; next} {print R"/"$0}' | awk '!seen[$0]++' > tb.f
  # msim loops forever at one time step in rv_tracer's te_packet_emitter (reported); patch a copy
  python3 "$HERE/cva6/patch_tracer.py" "$C/corev_apu/instr_tracing/rv_tracer-main/rtl/te_packet_emitter.sv" te_packet_emitter.sv
  sed -i "s#^.*rv_tracer-main/rtl/te_packet_emitter.sv#$PWD/te_packet_emitter.sv#" tb.f
  clang --target=riscv64 -march=rv64imac -mabi=lp64 -mcmodel=medany -O2 -nostdlib -ffreestanding -fuse-ld=lld \
    -Wl,-T,"$HERE/cva6/link.ld" -Wl,--no-relax "$HERE/cva6/start.S" "$HERE/cva6/prog.c" -o prog.elf
  $TM "$MSIM" ${MODE:-} -uvm -top ariane_tb -f tb.f "$HERE/cva6/elf_dpi.c" +elf_file=$PWD/prog.elf \
    +UVM_NO_RELNOTES -maxtime ${MAXTIME:-2ms} 2>&1 | grep -E "SUCCESS|FAILED|UVM_(ERROR|FATAL)|TM |error" ;;
esac
