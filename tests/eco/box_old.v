// Old: a memory and a 4-bit register cell without a logic model (black boxes).
module top (clk, we, a, di, d, do, q);
  input clk, we, a, di;
  input [3:0] d;
  output do;
  output [3:0] q;
  wire wen, x;
  AND2_X1 u0 (.A(we), .B(a), .Y(wen));
  RAM4 mem (.CK(clk), .WE(wen), .A(a), .DI(di), .DO(do));
  XOR2_X1 u1 (.A(d[3]), .B(d[0]), .Y(x));
  REG4_X1 r (.CK(clk), .D({x, d[2:0]}), .Q(q));
endmodule
