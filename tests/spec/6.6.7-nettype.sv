// IEEE 1800-2023 6.6.7: A user-defined nettype with a single driver
// (6.6.7).
//! icarus-skip: Icarus 13 does not support nettype
module top;
  nettype logic [3:0] nib;
  nib n;
  assign n = 4'h3;
  initial #1 $display("%h", n);
endmodule

//> 3
