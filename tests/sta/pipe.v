module pipe (input clk, input in1, output out1);
  wire q1, n1, n2, q2;
  DFF_X1 r1 (.CK(clk), .D(in1), .Q(q1));
  INV_X1 u1 (.A(q1), .Y(n1));
  NAND2_X1 u2 (.A(n1), .B(q1), .Y(n2));
  DFF_X1 r2 (.CK(clk), .D(n2), .Q(q2));
  BUF_X1 u3 (.A(q2), .Y(out1));
endmodule
