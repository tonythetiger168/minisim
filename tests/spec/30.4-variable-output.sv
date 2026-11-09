`timescale 1ns/1ns
// IEEE 1800-2023 30.4: a module path may end at an output declared as a
// variable. Inside the module the variable changes at once (the counter
// below reads its own value); outside, the port shows each change after
// the path delay: rise 3, fall 5 from clk (an edge-sensitive path), with
// pulses filtered as for a net (30.7).
//! icarus-skip: Icarus 13 does not delay variable outputs
module cnt(input clk, output reg [1:0] q, output reg t);
  initial begin q = 0; t = 0; end
  always @(posedge clk) begin
    q <= q + 1;
    $display("%0t: inside q=%0d", $time, q + 2'd1);
  end
  always @(clk) t = clk;
  specify
    (posedge clk *> (q +: 1'b1)) = (3, 5);
    (clk => t) = 4;
  endspecify
endmodule
module top;
  reg clk = 0;
  wire [1:0] q;
  wire t;
  cnt u(clk, q, t);
  initial begin
    $monitor("%0t: port q=%0d t=%b", $time, q, t);
    #10 clk = 1; #10 clk = 0;
    #10 clk = 1; #10 clk = 0;
    #10 clk = 1; #2 clk = 0;   // a 2-wide pulse on t: filtered (delay 4)
    #20 $finish;
  end
endmodule
//> 0: port q=x t=x
//> 4: port q=x t=0
//> 5: port q=0 t=0
//> 10: inside q=1
//> 13: port q=1 t=0
//> 14: port q=1 t=1
//> 24: port q=1 t=0
//> 30: inside q=2
//> 33: port q=2 t=0
//> 34: port q=2 t=1
//> 44: port q=2 t=0
//> 50: inside q=3
//> 53: port q=3 t=0
