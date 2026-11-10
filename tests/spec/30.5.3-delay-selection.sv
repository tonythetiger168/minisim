`timescale 1ns/1ns
// IEEE 1800-2023 30.5.3: when an output with several specify paths
// changes, the paths whose input changed most recently (and whose condition
// holds) are active, and the smallest of their delays for this transition
// is used. Example 1 and Example 2 of 30.5.3 (Example 2 compares MODE with
// <, which Table 30-1 does not list; -Wno-specify-condition-expr allows it).
//! icarus-skip: Icarus 13 ignores specify delays unless run with -gspecify
//! args: -Wno-specify-condition-expr
module or2(input A, B, output Y);
  assign Y = A | B;
  specify
    (A => Y) = (6, 9);
    (B => Y) = (5, 11);
  endspecify
endmodule
module modes(input A, input [2:0] MODE, output Y);
  assign Y = A;
  specify
    if (MODE < 5) (A => Y) = (5, 9);
    if (MODE < 4) (A => Y) = (4, 8);
    if (MODE < 3) (A => Y) = (6, 5);
    if (MODE < 2) (A => Y) = (3, 2);
    if (MODE < 1) (A => Y) = (7, 7);
  endspecify
endmodule
module top;
  reg A = 0, B = 0, C = 0;
  reg [2:0] MODE = 2;
  wire Y, Z;
  or2 u1(A, B, Y);
  modes u2(C, MODE, Z);
  always @(Y) $display("%0t Y=%b", $time, Y);
  always @(Z) $display("%0t Z=%b", $time, Z);
  initial begin
    #20 A = 1;            // rise through A: 6
    #20 A = 0;            // fall through A: 9
    #20 B = 1;            // rise through B: 5
    #20 B = 0;            // fall through B: 11
    #20 A = 1; B = 1;     // both changed last: the smaller rise, 5
    #20 A = 0; B = 0;     // the smaller fall, 9
    #20 $finish;
  end
  initial begin
    #20 C = 1;            // MODE 2: three paths active, smallest rise 4
    #20 C = 0;            // smallest fall 5
    #20 MODE = 0; C = 1;  // all five active: rise 3
    #20 MODE = 7; C = 0;  // none active: no path delay
  end
endmodule

//> 0 Y=x
//> 0 Z=x
//> 5 Z=0
//> 9 Y=0
//> 24 Z=1
//> 26 Y=1
//> 45 Z=0
//> 49 Y=0
//> 63 Z=1
//> 65 Y=1
//> 80 Z=0
//> 91 Y=0
//> 105 Y=1
//> 129 Y=0
