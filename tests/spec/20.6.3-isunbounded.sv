// IEEE 1800-2023 20.6.3: $isunbounded is 1 for a constant that is $.
module t #(parameter int P = $, parameter int Q = 4);
  initial $display("%0d %0d %0d", $isunbounded(P), $isunbounded(Q), $isunbounded(1));
endmodule
//> 1 0 0
//! icarus-skip: Icarus has no $isunbounded
