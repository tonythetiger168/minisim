// New: the write enable no longer depends on the address, and bit 3 of the
// register takes d[3] ^ d[1].
module top (clk, we, a, di, d, do, q);
  input clk, we, a, di;
  input [3:0] d;
  output do;
  output [3:0] q;
  wire x;
  RAM4 mem (.CK(clk), .WE(we), .A(a), .DI(di), .DO(do));
  XOR2_X1 g1 (.A(d[3]), .B(d[1]), .Y(x));
  REG4_X1 r (.CK(clk), .D({x, d[2:0]}), .Q(q));
endmodule
