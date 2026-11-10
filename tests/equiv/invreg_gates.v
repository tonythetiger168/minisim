// invreg.sv with the count stored inverted in DFFR cells (reset to 0) and an
// inverter on each output. The flops have no useful names; mequiv finds them
// through the nets on their Q pins and sees that they hold !q.
module invreg (input clk, rst_n, en, output [2:0] q);
  wire [2:0] nq, d, nd;
  wire c1, c2;
  // q + 1 when en
  XOR2 u1 (.A(q[0]), .B(en), .Y(d[0]));
  AND2 u2 (.A(q[0]), .B(en), .Y(c1));
  XOR2 u3 (.A(q[1]), .B(c1), .Y(d[1]));
  AND2 u4 (.A(q[1]), .B(c1), .Y(c2));
  XOR2 u5 (.A(q[2]), .B(c2), .Y(d[2]));
  INV u6 (.A(d[0]), .Y(nd[0]));
  INV u7 (.A(d[1]), .Y(nd[1]));
  INV u8 (.A(d[2]), .Y(nd[2]));
  DFFR u9 (.D(nd[0]), .CK(clk), .RN(rst_n), .Q(nq[0]));
  DFFR u10 (.D(nd[1]), .CK(clk), .RN(rst_n), .Q(nq[1]));
  DFFR u11 (.D(nd[2]), .CK(clk), .RN(rst_n), .Q(nq[2]));
  INV u12 (.A(nq[0]), .Y(q[0]));
  INV u13 (.A(nq[1]), .Y(q[1]));
  INV u14 (.A(nq[2]), .Y(q[2]));
endmodule
