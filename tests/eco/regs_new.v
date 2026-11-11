// New: r0 gets an enable (a different cell, without QN), r1 is gone, a third
// register r2 and its input c and output w are new.
module top (clk, a, b, c, en, y, z, w);
  input clk, a, b, c, en;
  output y, z, w;
  wire d0, q0, q0n, q2, t, d2;
  XOR2_X1 g0 (.A(a), .B(b), .Y(d0));
  DFFE_X1 r0 (.CK(clk), .D(d0), .E(en), .Q(q0));
  INV_X1 g1 (.A(q0), .Y(q0n));
  NOR2_X1 g3 (.A(c), .B(q0), .Y(d2));
  DFF_X1 r2 (.CK(clk), .D(d2), .Q(q2));
  AND2_X1 g2 (.A(q0n), .B(q2), .Y(t));
  BUF_X1 g4 (.A(t), .Y(y));
  assign z = q0;
  assign w = q2;
endmodule
