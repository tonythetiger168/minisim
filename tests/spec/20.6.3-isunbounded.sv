// IEEE 1800-2023 20.6.3: $isunbounded is 1 for a constant that is $,
// also for $ itself and for a localparam set to $.
module t #(parameter int P = $, parameter int Q = 4);
  localparam int L = $;
  initial $display("%0d %0d %0d %0d %0d", $isunbounded(P), $isunbounded(Q), $isunbounded(1), $isunbounded($), $isunbounded(L));
endmodule
//> 1 0 0 1 1
//! icarus-skip: Icarus has no $isunbounded
