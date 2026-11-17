#!/usr/bin/env bash
# XiangShan (OpenXiangShan/xiangshan, Mulan PSL v2) on msim.
#   bench/bigdesigns/xiangshan.sh gen     # Chisel -> SystemVerilog (needs JDK, ~10 GB)
#   bench/bigdesigns/xiangshan.sh elab    # msim -elab, and Verilator --cc for comparison
#   bench/bigdesigns/xiangshan.sh run [cycles]   # bare-metal hello.S on the interpreter
# CONFIG (default MinimalConfig) picks the XiangShan configuration.
source "$(dirname "$0")/common.sh"
CONFIG=${CONFIG:-MinimalConfig}
XS=$BIG/xiangshan
OUT=$BIG/xs-$CONFIG
DEFS="+define+DIFFTEST +define+PRINTF_COND=1 +define+RANDOMIZE_REG_INIT +define+RANDOMIZE_MEM_INIT +define+RANDOMIZE_GARBAGE_ASSIGN +define+RANDOMIZE_DELAY=0"
case "${1:-elab}" in
gen)
  clone https://github.com/OpenXiangShan/xiangshan xiangshan
  (cd "$XS" && git submodule update --init --depth 1 --jobs 2 \
     && (cd rocket-chip && git submodule update --init --depth 1 cde hardfloat) \
     && (cd XSCache && git submodule update --init --depth 1 OpenNCB ZhuJiang))
  command -v mill >/dev/null || { echo "install mill 0.12.17: curl -L -o /usr/local/bin/mill https://repo1.maven.org/maven2/com/lihaoyi/mill-dist/0.12.17/mill-dist-0.12.17.exe && chmod +x /usr/local/bin/mill"; exit 2; }
  # make runs Chisel and then firtool inside the same JVM; on a 16 GB machine the
  # pair runs out of memory, so let make write SimTop.fir and run firtool alone.
  (cd "$XS" && NOOP_HOME=$XS make sim-verilog CONFIG=$CONFIG JVM_XMX=${JVM_XMX:-8G} TIME_CMD= || true)
  FIRTOOL=$(ls -d $HOME/.cache/llvm-firtool/*/bin/firtool | tail -1)
  cd "$XS/build/rtl"
  [ -f SimTop.sv ] || $TM "$FIRTOOL" SimTop.fir -O=release --mlir-disable-threading --disable-annotation-unknown \
     --lowering-options=explicitBitcast,disallowLocalVariables,disallowPortDeclSharing,locationInfoStyle=none \
     --default-layer-specialization=enable --split-verilog -o .
  # the Makefile's post-processing (non-PLDM, non-XPROP)
  sed -i -e 's/$fatal/xs_assert_v2(`__FILE__, `__LINE__)/g' -e "s/\$error(/\$fwrite(32'h80000002, /g" *.sv *.v
  rm -rf "$OUT" && mkdir -p "$OUT" && cp -r "$XS/build/rtl" "$XS/build/generated-src" "$OUT/"
  rm -f "$OUT/rtl/SimTop.fir"
  { sed "s#^#$OUT/rtl/#" "$OUT/rtl/filelist.f"; ls "$XS"/difftest/src/test/vsrc/common/*.v;
    echo "+incdir+$OUT/rtl"; echo "+incdir+$OUT/generated-src"; } > "$OUT/xs.f"
  echo "files: $(wc -l < "$OUT/rtl/filelist.f"), lines: $(cat "$OUT"/rtl/*.sv "$OUT"/rtl/*.v | wc -l)" ;;
elab)
  cd "$OUT"
  $TM "$MSIM" -elab -top SimTop $DEFS -f xs.f 2>&1 | tail -2
  rm -rf vl && $TM "$VERILATOR" --cc --no-timing --top-module SimTop $DEFS -Wno-fatal -Wno-lint -Wno-style \
     --max-num-width 150000 --x-assign unique --output-split 30000 --output-split-cfuncs 30000 --Mdir vl -f xs.f 2>&1 | tail -2 ;;
run)
  cd "$OUT"
  [ -f generated-src/difftest-dpic.h ] || { echo "run gen first"; exit 2; }
  clang --target=riscv64 -march=rv64imac -mabi=lp64 -nostdlib -fuse-ld=lld -Wl,-Ttext=0x80000000 -Wl,--no-relax "$HERE/hello.S" -o hello.elf
  llvm-objcopy -O binary hello.elf hello.bin
  python3 "$HERE/gen_dpic_stubs.py" "$OUT/generated-src/difftest-dpic.h" > dpic_stubs.inc
  cp "$HERE/xs_dpi.cpp" .
  # reset must be held well past 10 cycles: the CSR's sticky criticalErrorState otherwise
  # latches random-init garbage right after reset and the ROB never commits.
  XS_BIN=$PWD/hello.bin $TM "$MSIM" -top tb_xs $DEFS -f xs.f "$HERE/tb_xs.sv" xs_dpi.cpp \
     +max_cycles=${2:-40000} +reset_cycles=100 2>&1 | grep -v -E "warning|^ *[~^]|^\.\./|^/" ;;
esac
