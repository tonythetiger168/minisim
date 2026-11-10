// A small gate netlist for mdft -insert_scan: a counter with an asynchronous
// reset, a gated clock, a falling-edge flop, a second clock domain inside a
// submodule, and one flop on a divided clock that scan cannot reach.
module sub (input clk, input d, output q);
  wire n1;
  DFF u_a (.D(d), .CK(clk), .Q(n1));
  DFF u_b (.D(n1), .CK(clk), .Q(q));
endmodule
module top (clk, clk2, rst_n, en, din, cnt, dout, q2);
  input clk, clk2, rst_n, en;
  input [1:0] din;
  output [3:0] cnt;
  output dout, q2;
  wire n0, n1, n2, n3, c1, c2, gclk, clkb, nq, divq, x;
  INV i0 (.A(cnt[0]), .Y(n0));
  XOR2 x1 (.A(cnt[1]), .B(cnt[0]), .Y(n1));
  AND2 a1 (.A(cnt[1]), .B(cnt[0]), .Y(c1));
  XOR2 x2 (.A(cnt[2]), .B(c1), .Y(n2));
  AND2 a2 (.A(cnt[2]), .B(c1), .Y(c2));
  XOR2 x3 (.A(cnt[3]), .B(c2), .Y(n3));
  DFFR cnt_reg_0_ (.D(n0), .CK(clk), .RN(rst_n), .Q(cnt[0]));
  DFFR cnt_reg_1_ (.D(n1), .CK(clk), .RN(rst_n), .Q(cnt[1]));
  DFFR cnt_reg_2_ (.D(n2), .CK(clk), .RN(rst_n), .Q(cnt[2]));
  DFFR cnt_reg_3_ (.D(n3), .CK(clk), .RN(rst_n), .Q(cnt[3]));
  ICG g0 (.CK(clk), .E(en), .TE(1'b0), .GCK(gclk));
  BUF b0 (.A(gclk), .Y(clkb));
  DFF gated_reg (.D(din[0]), .CK(clkb), .Q(x));
  DFFN neg_reg (.D(x), .CKN(clk), .Q(nq));
  sub u_sub (.clk(clk2), .d(din[1]), .q(q2));
  DFF div_reg (.D(nq), .CK(cnt[0]), .Q(divq));
  XOR2 xo (.A(nq), .B(divq), .Y(dout));
endmodule
