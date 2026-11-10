// flat.sv as a synthesis tool that flattens variables writes it: flops named by
// bit position in the whole variable, unused QN pins left open, and the
// memory on plain flops that recirculate while rst is high.
module flat (input clk, rst, input [1:0] a, input [3:0] d, output [3:0] q, output v);
  wire rstn, we1, we0;
  wire [3:0] it, itn; wire [7:0] m, mn; wire [3:0] rd;
  INV u_rn (.A(rst), .Y(rstn));
  AND2 u_we1 (.A(a[0]), .B(rstn), .Y(we1));
  NOR2 u_we0 (.A(a[0]), .B(rst), .Y(we0));
  DFFRX item_reg_0_ (.D(d[0]), .CK(clk), .RN(rstn), .Q(it[0]), .QN());
  DFFRX item_reg_1_ (.D(d[1]), .CK(clk), .RN(rstn), .Q(it[1]), .QN());
  DFFRX item_reg_2_ (.D(d[2]), .CK(clk), .RN(rstn), .Q(it[2]), .QN());
  DFFRX item_reg_3_ (.D(d[3]), .CK(clk), .RN(rstn), .Q(it[3]), .QN());
  MUX2 u_mn0 (.A(m[0]), .B(d[0]), .S(we1), .Y(mn[0]));
  DFF mem_reg_0_ (.D(mn[0]), .CK(clk), .Q(m[0]));
  MUX2 u_mn1 (.A(m[1]), .B(d[1]), .S(we1), .Y(mn[1]));
  DFF mem_reg_1_ (.D(mn[1]), .CK(clk), .Q(m[1]));
  MUX2 u_mn2 (.A(m[2]), .B(d[2]), .S(we1), .Y(mn[2]));
  DFF mem_reg_2_ (.D(mn[2]), .CK(clk), .Q(m[2]));
  MUX2 u_mn3 (.A(m[3]), .B(d[3]), .S(we1), .Y(mn[3]));
  DFF mem_reg_3_ (.D(mn[3]), .CK(clk), .Q(m[3]));
  MUX2 u_mn4 (.A(m[4]), .B(d[0]), .S(we0), .Y(mn[4]));
  DFF mem_reg_4_ (.D(mn[4]), .CK(clk), .Q(m[4]));
  MUX2 u_mn5 (.A(m[5]), .B(d[1]), .S(we0), .Y(mn[5]));
  DFF mem_reg_5_ (.D(mn[5]), .CK(clk), .Q(m[5]));
  MUX2 u_mn6 (.A(m[6]), .B(d[2]), .S(we0), .Y(mn[6]));
  DFF mem_reg_6_ (.D(mn[6]), .CK(clk), .Q(m[6]));
  MUX2 u_mn7 (.A(m[7]), .B(d[3]), .S(we0), .Y(mn[7]));
  DFF mem_reg_7_ (.D(mn[7]), .CK(clk), .Q(m[7]));
  MUX2 u_rd0 (.A(m[4]), .B(m[0]), .S(a[1]), .Y(rd[0]));
  MUX2 u_rd1 (.A(m[5]), .B(m[1]), .S(a[1]), .Y(rd[1]));
  MUX2 u_rd2 (.A(m[6]), .B(m[2]), .S(a[1]), .Y(rd[2]));
  MUX2 u_rd3 (.A(m[7]), .B(m[3]), .S(a[1]), .Y(rd[3]));
  XOR2 u_q0 (.A(rd[0]), .B(it[0]), .Y(q[0]));
`ifdef BUG
  XNOR2 u_q1 (.A(rd[1]), .B(it[1]), .Y(q[1]));
`else
  XOR2 u_q1 (.A(rd[1]), .B(it[1]), .Y(q[1]));
`endif
  XOR2 u_q2 (.A(rd[2]), .B(it[2]), .Y(q[2]));
  BUF u_q3 (.A(rd[3]), .Y(q[3]));
  BUF u_v (.A(it[3]), .Y(v));
endmodule
