// IEEE 1800-2023 10.3.3: Delays on continuous assignments and nets: rise
// and fall values, a vector, and inertial cancelling of a short pulse
// (10.3.3, 28.16).
//! icarus-skip: Icarus 13 does not support net delays
`timescale 1ns/100ps
module top;
  logic a = 0;
  logic [3:0] b = 0;
  wire w1, w2;
  wire #2 w3;
  wire [3:0] v;
  assign #2 w1 = a;
  assign #(3, 1) w2 = a;
  assign w3 = a;
  assign #(2, 1) v = b;
  initial begin
    #0.5 $display("%b %b %b", w1, w2, w3);
    #2   $display("%b %b %b %h", w1, w2, w3, v);
    a = 1; b = 3;
    #2.5 $display("%b %b %b %h", w1, w2, w3, v);
    #1   $display("%b %b %b %h", w1, w2, w3, v);
    a = 0; b = 0;
    #1.5 $display("%b %b %b %h", w1, w2, w3, v);
    #1.5 a = 1; b = 5;
    #1   a = 0; b = 0;
    #2.5 $display("%b %b %b %h", w1, w2, w3, v);
  end
endmodule

//> x x x
//> 0 0 0 0
//> 1 0 1 3
//> 1 1 1 3
//> 1 0 1 0
//> 0 0 0 0
