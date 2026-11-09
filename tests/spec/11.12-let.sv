// IEEE 1800-2023 11.12: let declarations (11.12).
//! icarus-skip: Icarus 13 does not support let
module top;
  let max(a, b) = a > b ? a : b;
  let sq(x) = x * x;
  initial $display("%0d %0d", max(3, 7), sq(4));
endmodule

//> 7 16
