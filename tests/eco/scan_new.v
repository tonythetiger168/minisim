// The revised design after synthesis: r0 now loads (a0 & a1 ^ b) | c, y is an
// XOR of the two registers, and z is inverted. No scan, no clock buffer.
module top (clk, rst_n, a, b, c, y, z);
  input clk, rst_n;
  input [1:0] a;
  input b, c;
  output y, z;
  wire m1, m2, m3, q0, q1;
  AND2_X1 g1 (.A(a[1]), .B(a[0]), .Y(m1));
  XOR2_X1 g2 (.A(b), .B(m1), .Y(m2));
  OR2_X1 g3 (.A(m2), .B(c), .Y(m3));
  DFFR_X1 r0 (.CK(clk), .D(m3), .RN(rst_n), .Q(q0));
  DFFR_X1 r1 (.CK(clk), .D(m2), .RN(rst_n), .Q(q1));
  XOR2_X1 g4 (.A(q0), .B(q1), .Y(y));
  INV_X1 g5 (.A(q1), .Y(z));
endmodule
