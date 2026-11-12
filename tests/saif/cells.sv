// Library cells and an RTL counter, for forward SAIF (IEEE 1801-2024 Annex J).
`timescale 1ns/1ns
module AN2(input A, input B, output Y);
  assign #1 Y = A & B;
endmodule

module counter(input clk, input rst, output reg [1:0] q);
  always @(posedge clk) q <= rst ? 2'd0 : q + 2'd1;
endmodule

module top;
  reg a = 0, b = 0, clk = 0, rst = 1;
  wire y;
  wire [1:0] q;
  AN2 u_and(.A(a), .B(b), .Y(y));
  counter u_cnt(.clk(clk), .rst(rst), .q(q));
  always #5 clk = ~clk;
  initial begin
    #10 a = 1;      // A rises while B is 0
    #10 b = 1;      // B rises while A is 1: Y rises, caused by B
    #10 a = 0;      // A falls while B is 1: Y falls, caused by A
    #10 a = 1;      // Y rises, caused by A
    #10 b = 0;      // Y falls, caused by B
    rst = 0;
    #50 $finish;
  end
endmodule
