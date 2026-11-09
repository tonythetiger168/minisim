// IEEE 1800-2023 6.14: A chandle starts as null and compares with null
// (6.14).
//! icarus-skip: Icarus 13 does not support chandle
module top;
  chandle h;
  initial $display("%0d", h == null);
endmodule

//> 1
