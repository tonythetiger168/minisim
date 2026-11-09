// IEEE 1800-2023 23.3.3.5: a multidimensional array of instances. A packed
// actual is split with the left-most index of each dimension first, row
// by row: m[0][2] takes the most significant bits.
//! icarus-skip: Icarus 13 does not support multidimensional instance arrays
module leaf(input logic [1:0] a, output logic [1:0] y);
  assign y = a + 2'd1;
  initial #1 $display("%m a=%b", a);
endmodule
module top;
  logic [11:0] a = 12'b11_10_01_00_11_10;
  logic [11:0] y;
  leaf m[0:1][2:0] (.a(a), .y(y));
  initial #2 $display("y=%b", y);
endmodule
//> top.m[0][0] a=01
//> top.m[0][1] a=10
//> top.m[0][2] a=11
//> top.m[1][0] a=10
//> top.m[1][1] a=11
//> top.m[1][2] a=00
//> y=001110010011
