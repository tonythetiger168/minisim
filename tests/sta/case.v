module casev (input clk, input a, input b, input sel, input en, output y);
  wire qa, qb, x, m, g, s1, s2;
  DFF_X1 ra (.CK(clk), .D(a), .Q(qa));
  DFF_X1 rb (.CK(clk), .D(b), .Q(qb));
  XOR2_X1 x1 (.A(qa), .B(en), .Y(x));
  BUF_X1 slow1 (.A(qb), .Y(s1));
  BUF_X1 slow2 (.A(s1), .Y(s2));
  MUX2_X1 m1 (.A(x), .B(s2), .S(sel), .Y(m));
  AND2_X1 g1 (.A(m), .B(1'b1), .Y(g));
  DFF_X1 rc (.CK(clk), .D(g), .Q(y));
endmodule
