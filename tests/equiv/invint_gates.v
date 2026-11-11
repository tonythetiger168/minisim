// invint.sv on a library whose only flop clears: s is stored inverted, and no
// net carries s itself, so only the logic shows the polarity.
module invint (input clk, rst_n, d, output q);
  wire nd, n1;
  INV u_nd (.A(d), .Y(nd));
  DFFR s_reg (.D(nd), .CK(clk), .RN(rst_n), .Q(n1));
`ifdef BUG
  XOR2 u_q (.A(n1), .B(d), .Y(q));
`else
  XNOR2 u_q (.A(n1), .B(d), .Y(q));
`endif
endmodule
