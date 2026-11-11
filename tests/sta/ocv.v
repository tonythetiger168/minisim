// A clock tree with two branches, for on-chip variation and clock
// reconvergence pessimism removal.
//   clk -> cb0 -> cb1 -> r1, r2, rn (falling edge)
//              -> cb2 -> r3
module ocv (input clk, input in1, output out1, output out2);
  wire c0, c1, c2, q1, q2, q3, qn, n1, n2, n3, n4;
  BUF_X1 cb0 (.A(clk), .Y(c0));
  BUF_X1 cb1 (.A(c0), .Y(c1));
  BUF_X1 cb2 (.A(c0), .Y(c2));
  DFF_X1 r1 (.CK(c1), .D(in1), .Q(q1));
  INV_X1 u1 (.A(q1), .Y(n1));
  DFF_X1 r2 (.CK(c1), .D(n4), .Q(q2));
  XOR2_X1 u4 (.A(n1), .B(q2), .Y(n4));
  BUF_X1 u2 (.A(q1), .Y(n2));
  DFF_X1 r3 (.CK(c2), .D(n3), .Q(q3));
  AND2_X1 u3 (.A(n2), .B(qn), .Y(n3));
  DFFN_X1 rn (.CK(c1), .D(q2), .Q(qn));
  BUF_X1 u5 (.A(q3), .Y(out1));
  BUF_X1 u6 (.A(q2), .Y(out2));
endmodule
