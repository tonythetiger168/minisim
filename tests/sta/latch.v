// Latches and time borrowing.
//   r1 -> b1 b2 -> la (transparent while clk is low) -> b3 -> r2
//   r1 -> lh (transparent while clk is high) -> r3 (falling edge)
//   la -> b4 .. b7 -> lb (transparent while clk is high) -> r4 (falling edge)
module latch (input clk, input d, input d2, output [2:0] q);
  wire cn, q1, n1, n2, qa, n3, qh, n4, n5, n6, n7, qb, q0;
  INV_X1 ci (.A(clk), .Y(cn));
  DFF_X1 r1 (.CK(clk), .D(d), .Q(q1));
  BUF_X1 b1 (.A(q1), .Y(n1));
  BUF_X1 b2 (.A(n1), .Y(n2));
  LATCH_X1 la (.G(cn), .D(n2), .Q(qa));
  BUF_X1 b3 (.A(qa), .Y(n3));
  DFF_X1 r2 (.CK(clk), .D(n3), .Q(q[0]));
  LATCH_X1 lh (.G(clk), .D(q1), .Q(qh));
  DFFN_X1 r3 (.CK(clk), .D(qh), .Q(q[1]));
  BUF_X1 b4 (.A(qa), .Y(n4));
  BUF_X1 b5 (.A(n4), .Y(n5));
  BUF_X1 b6 (.A(n5), .Y(n6));
  BUF_X1 b7 (.A(n6), .Y(n7));
  LATCH_X1 lb (.G(clk), .D(n7), .Q(qb));
  DFFN_X1 r4 (.CK(clk), .D(qb), .Q(q[2]));
endmodule
