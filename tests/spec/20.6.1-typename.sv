// IEEE 1800-2023 20.6.1: $typename returns the type as a string (20.6.1).
//! icarus-skip: Icarus 13 does not support $typename
module top;
  int a;
  logic [3:0] b;
  initial $display("%s|%s", $typename(a), $typename(b));
endmodule

//> int|logic[3:0]
