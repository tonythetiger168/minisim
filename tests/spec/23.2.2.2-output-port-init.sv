// IEEE 1800-2023 23.2.2.2: a variable output port in an ANSI port list may
// be initialized; it acts as the variable's declaration initializer (6.8),
// whether the port is connected, left open, or not named at all.
module src(input logic clk, output logic [7:0] q = 8'd7, output int n = -3);
  always @(posedge clk) q <= q + 8'd1;
endmodule
module top;
  logic clk = 0;
  logic [7:0] a;
  wire [7:0] w;
  int m;
  src u0(clk, a, m);
  src u1(.clk(clk), .q(w), .n());
  src u2(clk, , );
  initial begin
    #1 $display("%0d %0d %0d %0d %0d", a, m, w, u1.n, u2.q);
    clk = 1;
    #1 $display("%0d %0d %0d", a, w, u2.q);
  end
endmodule
//> 7 -3 7 -3 7
//> 8 8 8
