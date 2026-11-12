#!/bin/sh
# Compares the operator delays `mhls -lib` assumes with what msynth builds
# and msta times for the same operator between registers.
#   tests/hls/calibrate.sh <build dir> <file.lib> [work dir]
# Prints, per operator, the measured register-to-register delay in ns. The
# formulas in opDelay() (src/mhls.cpp) were fitted to this table for
# lib/mini45/mini45_tt.lib and checked against mini45_ss.lib.
set -e
B=$1; L=$2; W=${3:-/tmp/mhls_calibrate}
mkdir -p "$W"
echo "create_clock -period 10 [get_ports clk]" > "$W/c.sdc"
gen() {
  cat > "$W/$1.sv" <<EOT
module $1(input logic clk, input logic [$3-1:0] a, b, input logic s, output logic [$4-1:0] q);
  logic [$3-1:0] ra, rb; logic rs;
  always_ff @(posedge clk) begin ra <= a; rb <= b; rs <= s; q <= $2; end
endmodule
EOT
  "$B/msynth" "$W/$1.sv" -top $1 -lib "$L" -period 0.3 -clock clk -o "$W/$1_net.v" -q > /dev/null 2>&1
  printf '%-8s %s\n' $1 "$("$B/msta" -lib "$L" "$W/$1_net.v" -top $1 -sdc "$W/c.sdc" -q 2>&1 | awk '/data arrival time/ {print $NF; exit}')"
}
gen add8 "ra + rb" 8 8
gen add32 "ra + rb" 32 32
gen add64 "ra + rb" 64 64
gen lt32 "ra < rb" 32 1
gen eq32 "ra == rb" 32 1
gen mul8 "ra * rb" 8 8
gen mul16 "ra * rb" 16 16
gen mux32 "rs ? ra : rb" 32 32
gen shl32 "ra << rb[4:0]" 32 32
gen div8 "ra / rb" 8 8
gen div16 "ra / rb" 16 16
