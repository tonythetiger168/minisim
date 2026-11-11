module soc (input clk, input clk2, input rst_n, input [1:0] a, input sel, output [1:0] y, output z);
  wire clk_b, clk_n, qa0, qa1, s, c, mx, qn, divn, divq;
  BUF_X1 cb1 (.A(clk), .Y(clk_b));
  DFF_X1 ra0 (.CK(clk_b), .D(a[0]), .Q(qa0));
  DFF_X1 ra1 (.CK(clk_b), .D(a[1]), .Q(qa1));
  XOR2_X1 x1 (.A(qa0), .B(qa1), .Y(s));
  AND2_X1 g1 (.A(qa0), .B(qa1), .Y(c));
  MUX2_X1 m1 (.A(s), .B(c), .S(sel), .Y(mx));
  DFFR_X1 rb (.CK(clk_b), .D(mx), .RN(rst_n), .Q(y[0]));
  DFFN_X1 rn (.CK(clk_b), .D(s), .Q(qn));
  DFF_X1 rc (.CK(clk2), .D(qn), .Q(y[1]));
  DFF_X1 div (.CK(clk_b), .D(divn), .Q(divq));
  INV_X1 di (.A(divq), .Y(divn));
  DFF_X1 rd (.CK(divq), .D(c), .Q(z));
endmodule
