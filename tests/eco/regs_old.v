// Old: two registers, one of them with both outputs in use.
module top (clk, a, b, y, z);
  input clk, a, b;
  output y, z;
  wire d0, q0, q0n, q1, t;
  XOR2_X1 u0 (.A(a), .B(b), .Y(d0));
  DFFQN_X1 r0 (.CK(clk), .D(d0), .Q(q0), .QN(q0n));
  DFF_X1 r1 (.CK(clk), .D(q0), .Q(q1));
  AND2_X1 u1 (.A(q0n), .B(q1), .Y(t));
  BUF_X1 u2 (.A(t), .Y(y));
  assign z = q0;
endmodule
